# SPDX-License-Identifier: GPL-2.0-or-later
"""Internal AAF/SMF writer. Input is Ardour's validated exchange manifest."""
import argparse
from bisect import bisect_right
import hashlib
from fractions import Fraction
import math
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET

# Packaged dependencies live beside this helper, never in the working directory.
vendor = Path(__file__).resolve().parent / "vendor"
if vendor.is_dir():
    sys.path.insert(0, str(vendor))
import aaf2
from aaf2.audio import WaveReader

PPQN = 19200
VLQ_MAX = 0x0FFFFFFF


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(node, name):
    value = float(node.attrib[name])
    require(math.isfinite(value), "Nonfinite " + name)
    return value


def integer(node, name):
    value = int(node.attrib[name])
    require(0 <= value <= 2**53, "Invalid " + name)
    return value


def ticks(beats):
    require(math.isfinite(beats) and 0 <= beats <= 2**40, "MIDI time out of range")
    return math.floor(beats * PPQN + 0.5)


def vlq(value):
    require(0 <= value <= VLQ_MAX, "MIDI delta out of range")
    result = bytearray([value & 127])
    while value >> 7:
        value >>= 7
        result.insert(0, (value & 127) | 128)
    return bytes(result)


def meta(kind, data):
    require(len(data) <= 1024 * 1024, "MIDI metadata too long")
    return bytes([255, kind]) + vlq(len(data)) + data


def track_chunk(events, end):
    data = bytearray()
    previous = 0
    # Note-offs sort before note-ons at the same tick.
    for at, order, payload in sorted(events, key=lambda item: (item[0], item[1])):
        delta = at - previous
        require(delta >= 0, "Invalid MIDI ordering")
        while delta > VLQ_MAX:
            data.extend(vlq(VLQ_MAX) + meta(1, b""))
            delta -= VLQ_MAX
        data.extend(vlq(delta) + payload)
        previous = at
    end = max(end, previous)
    delta = end - previous
    while delta > VLQ_MAX:
        data.extend(vlq(VLQ_MAX) + meta(1, b""))
        delta -= VLQ_MAX
    data.extend(vlq(delta) + meta(47, b""))
    require(len(data) <= 0xFFFFFFFF, "MIDI track too large")
    return b"MTrk" + struct.pack(">I", len(data)) + data


def write_midi(root, destination):
    tempo = number(root, "tempo")
    require(tempo > 0, "Invalid tempo")
    def tempo_payload(bpm):
        require(bpm > 0, "Invalid tempo")
        micros = math.floor(60000000 / bpm + 0.5)
        require(1 <= micros <= 0xFFFFFF, "Tempo cannot be represented in MIDI")
        return meta(81, micros.to_bytes(3, "big"))

    def meter_payload(node):
        numerator, denominator = integer(node, "numerator"), integer(node, "denominator")
        require(1 <= numerator <= 255 and denominator > 0 and denominator & (denominator - 1) == 0,
                "Meter cannot be represented in MIDI")
        exponent = denominator.bit_length() - 1
        require(exponent <= 255, "Meter denominator too large")
        return meta(88, bytes([numerator, exponent, 24, 8]))

    events = [
        (0, -4, meta(3, root.attrib["name"].encode("utf-8"))),
        # 25 fps, 00:00:00:00. Matches the AAF timecode origin.
        (0, -1, meta(84, bytes([0x20, 0, 0, 0, 0]))),
    ]
    for tag, order, initial, encode in (
            ("Tempo", -3, tempo_payload(tempo), lambda n: tempo_payload(number(n, "bpm"))),
            ("TimeSignature", -2, meter_payload(root), meter_payload)):
        transport = {0: initial}
        previous = -1
        for point in root.findall(tag):
            at = ticks(number(point, "time"))
            require(at > previous, "Transport changes collide at MIDI tick resolution")
            transport[at] = encode(point)
            previous = at
        events.extend((at, order, payload) for at, payload in transport.items())
    end = ticks(number(root, "durationBeats") if "durationBeats" in root.attrib
                else number(root, "duration") * tempo / 60)
    for marker in root.findall("Marker"):
        at = ticks(number(marker, "time"))
        events.append((at, 0, meta(6, marker.attrib["name"].encode("utf-8"))))
        end = max(end, at)
    tracks = []
    for track in root.findall("MidiTrack"):
        output = [(0, -1, meta(3, track.attrib["name"].encode("utf-8")))]
        intervals = {}
        control_intervals, control_values = {}, {}
        for clip in track.findall("Clip"):
            require(clip.attrib.get("enabled") == "true", "Muted MIDI clips are not supported")
            position, length, offset = (number(clip, key) for key in ("time", "length", "offset"))
            require(position >= 0 and length > 0 and offset >= 0, "Invalid MIDI clip")
            end = max(end, ticks(position + length))
            for note in clip.findall("Note"):
                start = max(number(note, "time"), offset)
                stop = min(number(note, "time") + number(note, "length"), offset + length)
                if start >= stop:
                    continue
                on = ticks(position + start - offset)
                off = ticks(position + stop - offset)
                require(off > on, "MIDI note is shorter than file resolution")
                channel, key, velocity, release = (integer(note, field) for field in ("channel", "key", "velocity", "release"))
                require(channel < 16 and key < 128 and 1 <= velocity <= 127 and release < 128, "Invalid MIDI note")
                intervals.setdefault((channel, key), []).append((on, off))
                output.append((on, 2, bytes([0x90 | channel, key, velocity])))
                output.append((off, 0, bytes([0x80 | channel, key, release])))
            seen_targets = set()
            for lane in clip.findall("MidiControl"):
                kind, channel, number_ = (integer(lane, field) for field in ("type", "channel", "number"))
                supported = ((kind == 0xB0 and 0 < number_ < 96 and number_ not in (6, 32, 38)) or
                             (kind == 0xA0 and number_ < 128) or (kind in (0xD0, 0xE0) and number_ == 0))
                require(supported and channel < 16, "Unsupported MIDI controller target")
                target = (kind, channel, number_)
                require(target not in seen_targets, "Duplicate MIDI controller lane")
                seen_targets.add(target)
                previous, carry, points, at_boundary = -1, None, [], False
                for point in lane.findall("Point"):
                    at, value = number(point, "time"), integer(point, "value")
                    require(at >= 0 and at > previous and value <= (16383 if kind == 0xE0 else 127), "Invalid MIDI controller point")
                    previous = at
                    if at < offset:
                        carry = value
                    elif at < offset + length:
                        at_boundary |= at == offset
                        points.append((ticks(position + at - offset), value))
                if carry is not None and not at_boundary:
                    points.insert(0, (ticks(position), carry))
                if points:
                    control_intervals.setdefault(target, []).append((ticks(position), ticks(position + length)))
                for at, value in points:
                    address = (*target, at)
                    require(address not in control_values or control_values[address] == value,
                            "MIDI controllers conflict at output tick resolution")
                    control_values[address] = value
        for spans in intervals.values():
            spans.sort()
            require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])),
                    "Overlapping notes with the same channel and pitch cannot be represented faithfully")
        for spans in control_intervals.values():
            spans.sort()
            require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])),
                    "Overlapping MIDI controller clips need explicit consolidation")
        for (kind, channel, number_, at), value in control_values.items():
            data = bytes([kind | channel, value]) if kind == 0xD0 else bytes(
                [kind | channel, value & 127, value >> 7]) if kind == 0xE0 else bytes([kind | channel, number_, value])
            output.append((at, 1, data))
        tracks.append(output)
    require(len(tracks) < 65535, "Too many MIDI tracks")
    with destination.open("xb") as output:
        output.write(b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks) + 1, PPQN))
        output.write(track_chunk(events, end))
        for track in tracks:
            output.write(track_chunk(track, end))


def operation(output, uid, name, inputs, parameter=None):
    definition = output.create.OperationDef(uid, name)
    definition.media_kind = "sound"
    definition.number_inputs = inputs
    definition["IsTimeWarp"].value = False
    if parameter is not None:
        definition["ParametersDefined"].append(parameter)
    output.dictionary.register_def(definition)
    return definition


def apply_constant(output, segment, definition, parameter, value):
    group = output.create.OperationGroup(definition, segment.length, media_kind="sound")
    group.segments.append(segment)
    rational = Fraction(value).limit_denominator(1000000)
    require(abs(rational.numerator) <= 0x7FFFFFFF, "AAF parameter out of range")
    group.parameters.append(output.create.ConstantValue(parameter, str(rational)))
    return group


def automation_points(node, rate, pan=False):
    points = []
    if node is None:
        return points
    for point in node.findall("Point"):
        time, value = number(point, "time"), number(point, "value")
        require(time >= 0 and value >= 0 and (not pan or value <= 1), "Invalid mixer automation point")
        at = math.floor(time * rate + .5)
        require(at <= 2**53 and (not points or at > points[-1][0]), "Automation points collide at audio resolution")
        require(point.attrib["linear"] in ("true", "false"), "Invalid automation interpolation")
        points.append((at, value, point.attrib["linear"] == "true"))
    require(0 < len(points) <= 100000, "Invalid automation point count")
    return points


def stereo_balance_points(points, gain):
    """Sample a balance ramp at its center kink, retaining exact sample values."""
    expanded = []
    for previous, following in zip(points, points[1:]):
        expanded.append(previous)
        start, first, linear = previous
        end, last, _ = following
        if linear and (first - .5) * (last - .5) < 0:
            crossing = start + (end - start) * (.5 - first) / (last - first)
            for at in sorted({math.floor(crossing), math.ceil(crossing)}):
                if start < at < end:
                    value = first + (last - first) * (at - start) / (end - start)
                    expanded.append((at, value, True))
    expanded.append(points[-1])
    require(len(expanded) <= 100000, "Stereo balance automation exceeds the event limit")
    return [[(at, gain * min(1, 2 * (1 - value) if channel == 0 else 2 * value), linear)
             for at, value, linear in expanded] for channel in range(2)]


def moving_stereo_levels(gains, pans, end):
    """Approximate gain times balance at audio samples, splitting quadratic arcs."""
    gain_times, pan_times = ([point[0] for point in curve] for curve in (gains, pans))

    def value(curve, curve_times, at):
        index = bisect_right(curve_times, at) - 1
        if index < 0:
            return curve[0][1]
        if index + 1 == len(curve) or not curve[index][2]:
            return curve[index][1]
        start, first, _ = curve[index]
        stop, last, _ = curve[index + 1]
        return first + (last - first) * (at - start) / (stop - start)

    times = {0, end, *gain_times, *pan_times}
    for curve in (gains, pans):
        for previous, following in zip(curve, curve[1:]):
            if not previous[2] and previous[1] != following[1] and following[0] - previous[0] > 1:
                times.add(following[0] - 1)
    for previous, following in zip(pans, pans[1:]):
        start, first, linear = previous
        stop, last, _ = following
        if linear and (first - .5) * (last - .5) < 0:
            crossing = start + (stop - start) * (.5 - first) / (last - first)
            times.update((math.floor(crossing), math.ceil(crossing)))
    times = sorted(at for at in times if 0 <= at <= end)
    require(len(times) <= 100000, "Stereo mixer automation exceeds the event limit")
    levels = {}
    selected = set(times)

    def at(time):
        if time not in levels:
            gain = value(gains, gain_times, time)
            balance = value(pans, pan_times, time)
            levels[time] = (gain * min(1, 2 * (1 - balance)), gain * min(1, 2 * balance))
        return levels[time]

    def refine(start, stop):
        if stop - start <= 1:
            return
        first, last = at(start), at(stop)
        middle = (start + stop) // 2
        actual = at(middle)
        fraction = (middle - start) / (stop - start)
        error = max(abs(actual[channel] - (first[channel] + (last[channel] - first[channel]) * fraction))
                    for channel in range(2))
        tolerance = 1e-5 * max(1, *first, *last)
        if error > tolerance:
            require(len(selected) < 100000, "Stereo mixer automation exceeds the event limit")
            selected.add(middle)
            refine(start, middle)
            refine(middle, stop)

    for start, stop in zip(times, times[1:]):
        refine(start, stop)
    output_times = sorted(selected)
    require(len(output_times) <= 100000, "Stereo mixer automation exceeds the event limit")
    silent = [(time, value(pans, pan_times, time)) for time in output_times if at(time) == (0, 0)]
    tag = "2|" + ",".join(f"{time}={balance:.17g}" for time, balance in silent) if silent else "1"
    require(len(tag) <= 4 * 1024 * 1024, "Stereo balance metadata exceeds the event limit")
    return ([[(time, at(time)[channel], True) for time in output_times] for channel in range(2)], tag)


def apply_automation(output, segment, definition, parameter, points):
    discrete = all(not p[2] for p in points[:-1])
    expanded = []
    for i, (at, value, linear) in enumerate(points):
        if i and not discrete and not points[i - 1][2] and points[i - 1][1] != value and at - 1 > expanded[-1][0]:
            expanded.append((at - 1, points[i - 1][1]))
        expanded.append((at, value))
    group = output.create.OperationGroup(definition, segment.length, media_kind="sound")
    group.segments.append(segment)
    curve = output.create.VaryingValue(parameter, "Constant" if discrete else "Linear")
    for at, value in expanded:
        time = Fraction(at, segment.length)
        level = Fraction(value).limit_denominator(1000000)
        require(abs(time.numerator) <= 0x7fffffff and time.denominator <= 0x7fffffff
                and abs(level.numerator) <= 0x7fffffff, "AAF automation rational out of range")
        curve.add_keyframe(time, str(level))
    group.parameters.append(curve)
    return group


def write_aaf(root, destination):
    rate = integer(root, "rate")
    require(8000 <= rate <= 768000, "Invalid audio sample rate")
    with aaf2.open(str(destination), "w") as output:
        for name, uid in (("Linear", "5b6c85a4-0ede-11d3-80a9-006008143e6f"),
                          ("Constant", "5b6c85a5-0ede-11d3-80a9-006008143e6f")):
            output.dictionary.register_def(output.create.InterpolationDef(uid, name))
        amplitude = output.create.ParameterDef("e4962321-2267-11d3-8a4c-0050040ef7d2", "Amplitude", typedef="Rational")
        pan = output.create.ParameterDef("e4962322-2267-11d3-8a4c-0050040ef7d2", "Pan", typedef="Rational")
        output.dictionary.register_def(amplitude)
        output.dictionary.register_def(pan)
        level = output.create.ParameterDef("e4962320-2267-11d3-8a4c-0050040ef7d2", "Level", typedef="Rational")
        output.dictionary.register_def(level)
        dissolve = operation(output, "0c3bea41-fc05-11d2-8a29-0050040ef7d2", "MonoAudioDissolve", 2, level)
        mono_gain = operation(output, "9d2ea894-0968-11d3-8a38-0050040ef7d2", "MonoAudioGain", 1, amplitude)
        mono_pan = operation(output, "9d2ea893-0968-11d3-8a38-0050040ef7d2", "MonoAudioPan", 1, pan)
        output.dictionary.register_def(output.create.TaggedValueDef(
            "21a8ae63-2486-43b5-a6f2-54132481bcf3", "Ardour.StereoGainBalance",
            "Paired mono channel levels encode Ardour stereo gain and balance automation"))
        composition = output.create.CompositionMob(root.attrib["name"])
        composition.usage = "Usage_TopLevel"
        output.content.mobs.append(composition)
        media = {}
        for source in root.findall("Media"):
            path = Path(source.attrib["path"])
            require(path.is_absolute() and path.is_file(), "Missing prepared audio")
            with WaveReader(str(path)) as wave:
                channels = integer(source, "channels")
                frames = integer(source, "frames")
                require(channels in (1, 2) and wave.getnchannels() == channels
                        and wave.getframerate() == rate and wave.getnframes() == frames
                        and wave.getsampwidth() == 3, "Prepared 24-bit audio does not match manifest")
            master = output.create.MasterMob(source.attrib["id"])
            output.content.mobs.append(master)
            slot = master.import_audio_essence(str(path), edit_rate=rate)
            slot["PhysicalTrackNumber"].value = 1
            slots = [slot]
            if channels == 2:
                right = master.create_timeline_slot(rate)
                right.segment = slot.segment.copy()
                right["PhysicalTrackNumber"].value = 2
                slots.append(right)
            require(source.attrib["id"] not in media, "Duplicate audio ID")
            media[source.attrib["id"]] = (master, slots, frames)
        duration = math.ceil(number(root, "duration") * rate)
        for track in root.findall("AudioTrack"):
            channels = integer(track, "channels")
            require(channels in (1, 2), "Unsupported audio track channels")
            composition_slots = [composition.create_sound_slot(rate) for _ in range(channels)]
            sequences = [output.create.Sequence("sound") for _ in composition_slots]
            for channel, slot in enumerate(composition_slots, 1):
                slot.name = track.attrib["name"]
                slot["PhysicalTrackNumber"].value = channel
            end = 0
            previous_position, preceding_end = -1, 0
            for clip in sorted(track.findall("Clip"), key=lambda c: integer(c, "time")):
                position, length, offset = (integer(clip, key) for key in ("time", "length", "offset"))
                require(length > 0, "Empty audio clip")
                master, slots, frames = media[clip.attrib["media"]]
                require(len(slots) == channels and offset + length <= frames, "Invalid audio clip source range")
                if position < end:
                    require(position > previous_position and position + length > end and position >= preceding_end,
                            "Overlapping audio requires pairwise, non-nested overlaps")
                    overlap = end - position
                    # AS-01 12.19: Level(t)*B + Level(1-t)*A. Unity Level
                    # sums the overlap on each mono slot independently; each
                    # clip keeps its own gain envelope.
                    for sequence in sequences:
                        transition = output.create.Transition("sound", overlap)
                        transition.cutpoint = overlap // 2
                        group = output.create.OperationGroup(dissolve, overlap, media_kind="sound")
                        curve = output.create.VaryingValue(level, "Linear")
                        curve.add_keyframe(0, "1")
                        curve.add_keyframe(1, "1")
                        group.parameters.append(curve)
                        transition["OperationGroup"].value = group
                        sequence.components.append(transition)
                if position > end:
                    for sequence in sequences:
                        sequence.components.append(output.create.Filler("sound", position - end))
                references = [master.create_source_clip(s.slot_id, start=offset, length=length, media_kind="sound") for s in slots]
                clip_gain = number(clip, "gain") if "gain" in clip.attrib else 1
                require(clip_gain >= 0, "Invalid clip gain")
                if clip.attrib.get("enabled") != "true":
                    clip_gain = 0
                clip_points = automation_points(clip.find("GainAutomation"), rate)
                if clip_points:
                    require(clip_points[-1][0] <= length, "Clip automation extends outside clip")
                    clip_points = [(at, value * clip_gain, linear) for at, value, linear in clip_points]
                for sequence, segment in zip(sequences, references):
                    if clip_points:
                        segment = apply_automation(output, segment, mono_gain, amplitude, clip_points)
                    elif clip_gain != 1:
                        segment = apply_constant(output, segment, mono_gain, amplitude, clip_gain)
                    for field in ("name", "color"):
                        if clip.attrib.get(field):
                            segment["UserComments"].append(output.create.TaggedValue("Ardour." + field, clip.attrib[field]))
                    sequence.components.append(segment)
                previous_position, preceding_end, end = position, end, position + length
            gain_points = automation_points(track.find("GainAutomation"), rate)
            pan_points = automation_points(track.find("PanAutomation"), rate, pan=True)
            automation_end = max([end, 1] + [p[0] for p in gain_points + pan_points])
            if (gain_points or pan_points) and automation_end > end:
                for sequence in sequences:
                    sequence.components.append(output.create.Filler("sound", automation_end - end))
                end = automation_end
            gain = number(track, "gain")
            require(gain >= 0, "Invalid audio gain")
            balance = number(track, "pan")
            require(0 <= balance <= 1, "Invalid audio pan")
            combined = None
            if channels == 2 and pan_points and (gain_points or gain == 0):
                combined = moving_stereo_levels(gain_points or [(0, 0, True)], pan_points, end)
                if not gain_points:
                    require(combined[1].startswith("2|"), "Missing silent stereo balance metadata")
                    combined = (combined[0], "3|" + combined[1][2:])
            balance_curves = stereo_balance_points(pan_points, gain) if channels == 2 and pan_points and not combined else None
            for channel, (slot, sequence) in enumerate(zip(composition_slots, sequences)):
                sequence.length = end
                if combined:
                    sequence = apply_automation(output, sequence, mono_gain, amplitude, combined[0][channel])
                    sequence["UserComments"].append(output.create.TaggedValue("Ardour.StereoGainBalance", combined[1]))
                elif balance_curves:
                    sequence = apply_automation(output, sequence, mono_gain, amplitude, balance_curves[channel])
                elif gain_points:
                    side = min(1, 2 * (1 - balance) if channel == 0 else 2 * balance) if channels == 2 else 1
                    sequence = apply_automation(output, sequence, mono_gain, amplitude,
                                                [(at, value * side, linear) for at, value, linear in gain_points])
                else:
                    # Ardour's stereo balance panner leaves one side at unity
                    # and attenuates the other. AS-01 exposes separate mono
                    # slots, so encode that balance as per-slot level.
                    side = min(1, 2 * (1 - balance) if channel == 0 else 2 * balance) if channels == 2 else 1
                    if gain * side != 1:
                        sequence = apply_constant(output, sequence, mono_gain, amplitude, gain * side)
                if channels == 1 and pan_points:
                    sequence = apply_automation(output, sequence, mono_pan, pan, pan_points)
                elif channels == 1 and balance != .5:
                    sequence = apply_constant(output, sequence, mono_pan, pan, balance)
                slot.segment = sequence
            duration = max(duration, end)
        markers = root.findall("Marker")
        if markers:
            slot = output.create.EventMobSlot(max((s.slot_id for s in composition.slots), default=0) + 1)
            slot.name = "Markers"
            slot.edit_rate = rate
            sequence = output.create.Sequence("DescriptiveMetadata")
            for source in sorted(markers, key=lambda m: number(m, "seconds")):
                seconds = number(source, "seconds")
                length = float(source.attrib.get("lengthSeconds", "0"))
                require(math.isfinite(length) and length >= 0 and
                        0 <= seconds <= (2**63 - 1) / rate - length, "Invalid AAF marker range")
                position = math.floor(seconds * rate + 0.5)
                marker = output.create.DescriptiveMarker()
                marker["Position"].value = position
                if length:
                    frames = math.floor(length * rate + 0.5)
                    require(frames > 0, "AAF marker range is shorter than one sample")
                    marker["Length"].value = frames
                marker["Comment"].value = source.attrib["name"]
                sequence.components.append(marker)
            slot.segment = sequence
            composition.slots.append(slot)
        timecode = composition.create_timeline_slot(25)
        timecode.name = "Timecode"
        timecode.segment = output.create.Timecode(25, length=max(1, math.ceil(duration * 25 / rate)))
        timecode.segment.start = 0


def run(manifest, destination):
    require(destination.is_dir() and not any(destination.iterdir()), "Output directory must be empty")
    data = manifest.read_bytes()
    require(len(data) <= 32 * 1024 * 1024 and b"<!" not in data, "Invalid exchange manifest")
    root = ET.fromstring(data)
    require(root.tag == "LogicExchange" and root.attrib.get("version") in ("1", "2"), "Unknown exchange manifest")
    write_midi(root, destination / "Project.mid")
    write_aaf(root, destination / "Project.aaf")
    # Keep source structure and appearance recoverable without leaking local
    # source paths. These are sidecar data, not target-DAW display guarantees.
    for media in root.findall("Media"):
        del media.attrib["path"]
        media.set("embeddedIn", "Project.aaf")
    for name in ("Project.aaf", "Project.mid"):
        with (destination / name).open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        ET.SubElement(root, "OutputFile", name=name, sha256=digest)
    with (destination / "Metadata.xml").open("xb") as stream:
        ET.ElementTree(root).write(stream, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        run(args.manifest, args.destination)
    except Exception as error:
        print("Project export failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
