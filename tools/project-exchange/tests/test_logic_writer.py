# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production writer with independent native AAF and SMF readers."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import xml.etree.ElementTree as ET

import aaf2

HELPER = Path(__file__).resolve().parents[3] / "share/project-exchange/aaf_writer.py"
PPQN = 19200


def read_midi(path):
    data = path.read_bytes()
    assert data[:8] == b"MThd\0\0\0\6"
    kind, count, resolution = struct.unpack_from(">HHH", data, 8)
    assert kind == 1 and resolution == PPQN
    position = 14
    tracks = []
    for _ in range(count):
        assert data[position:position + 4] == b"MTrk"
        end = position + 8 + int.from_bytes(data[position + 4:position + 8], "big")
        position += 8
        tick, events = 0, []

        def variable():
            nonlocal position
            result = 0
            for _ in range(4):
                value = data[position]
                position += 1
                result = (result << 7) | (value & 127)
                if value < 128:
                    return result
            raise AssertionError("Invalid MIDI variable-length quantity")

        while position < end:
            tick += variable()
            status = data[position]
            position += 1
            if status == 255:
                meta = data[position]
                position += 1
                size = variable()
                payload = data[position:position + size]
                position += size
                events.append((tick, status, meta, payload))
            else:
                assert status & 0xF0 in (0x80, 0x90, 0xA0, 0xB0, 0xD0, 0xE0)
                size = 1 if status & 0xF0 == 0xD0 else 2
                events.append((tick, status, *data[position:position + size]))
                position += size
        assert position == end and events[-1][1:] == (255, 47, b"")
        tracks.append(events)
    assert position == len(data)
    return tracks


def fixture(directory):
    root = ET.Element("LogicExchange", version="1", name="Session café 音楽", rate="48000",
                      tempo="100", numerator="3", denominator="4", duration="9.6")
    checksums = {}
    for channels in (1, 2):
        path = directory / ("source %d.wav" % channels)
        pcm = b"".join(((frame * (channel + 3) * 29) % 40001 - 20000).to_bytes(3, "little", signed=True)
                       for frame in range(96000) for channel in range(channels))
        with wave.open(str(path), "wb") as output:
            output.setparams((channels, 3, 48000, 96000, "NONE", "not compressed"))
            output.writeframes(pcm)
        checksums[channels] = hashlib.sha256(pcm).hexdigest()
        ET.SubElement(root, "Media", id="source-%d" % channels, path=str(path), channels=str(channels), frames="96000")
        track = ET.SubElement(root, "AudioTrack", name="Audio %d café" % channels, channels=str(channels),
                              gain="0.75", pan="0.25" if channels == 1 else "0.5", color="#123456")
        ET.SubElement(track, "Clip", name="Offset 音楽", time="12000", length="24000", offset="6000",
                      enabled="true", media="source-%d" % channels, color="#abcdef", gain="0.375")
        ET.SubElement(track, "Clip", name="Muted", time="48000", length="12000", offset="30000",
                      enabled="false", media="source-%d" % channels)
    track = ET.SubElement(root, "MidiTrack", name="Notes café")
    clip = ET.SubElement(track, "Clip", name="Trimmed notes", time="8", length="4", offset="2", enabled="true")
    for at, length, key in ((0, 1, 59), (1, 2, 60), (3.5, .5, 64), (5.5, 2, 67), (6, 1, 72)):
        ET.SubElement(clip, "Note", time=str(at), length=str(length), key=str(key), channel="2", velocity="83", release="30")
    ET.SubElement(root, "Marker", name="Refrain 音楽", time="16", seconds="9.6")
    return root, checksums


class LogicWriterTest(unittest.TestCase):
    probe = None

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="ardour logic ")
        self.directory = Path(self.work.name)
        self.root, self.checksums = fixture(self.directory)
        self.output = self.directory / "export"
        self.output.mkdir()

    def tearDown(self):
        self.work.cleanup()

    def run_writer(self):
        manifest = self.directory / "manifest.xml"
        ET.ElementTree(self.root).write(manifest, encoding="utf-8", xml_declaration=True)
        return subprocess.run([sys.executable, "-I", str(HELPER), str(manifest), str(self.output)],
                              capture_output=True, text=True, encoding="utf-8", timeout=30)

    def test_midi_only_aaf_without_embedded_essence(self):
        for child in list(self.root):
            if child.tag in ('Media', 'AudioTrack'):
                self.root.remove(child)
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        with aaf2.open(str(self.output / "Project.aaf"), "r") as aaf:
            self.assertNotIn("EssenceData", aaf.content)
            self.assertTrue(aaf.content.classdef.get_propertydef_from_pid(0x1902).optional)
        extracted = self.directory / 'extracted'
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = json.loads(result.stdout)
        self.assertEqual(parsed["tracks"], [])
        self.assertEqual(parsed["media"], [])
        self.assertEqual(len(read_midi(self.output / "Project.mid")), 2)

    def test_aaf_markers_keep_names_and_sample_positions(self):
        ET.SubElement(self.root, "Marker", name="Same sample", time="16.00001", seconds="9.600001")
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        with aaf2.open(str(self.output / "Project.aaf"), "r") as aaf:
            composition = next(aaf.content.toplevel())
            slot = next(slot for slot in composition.slots if isinstance(slot, aaf2.mobslots.EventMobSlot))
            self.assertEqual([(m["Position"].value, m["Comment"].value) for m in slot.segment.components],
                             [(460800, "Refrain 音楽"), (460800, "Same sample")])

    def test_aaf_invalid_marker_time_is_refused(self):
        for value in ("-1", "1e20", "NaN"):
            with self.subTest(seconds=value):
                self.output = self.directory / ("export-" + value)
                self.output.mkdir()
                self.root.find("Marker").set("seconds", value)
                result = self.run_writer()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("marker time" if value != "NaN" else "Nonfinite seconds", result.stderr)

    def test_embedded_audio_mixer_and_coordinated_midi(self):
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in self.directory.glob("*.wav"):
            path.unlink()
        extracted = self.directory / "extracted"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = json.loads(result.stdout)
        self.assertEqual(len(parsed["tracks"]), 3)
        for i, track in enumerate(parsed["tracks"]):
            self.assertEqual(track["name"], "Audio %d café" % (1 if i == 0 else 2))
            self.assertEqual(track["channels"], 1)
            self.assertEqual(track["rate"], [48000, 1])
            self.assertEqual(track["gain"], .75)
            if i == 0:
                self.assertEqual(track["pan"], .25)
            self.assertEqual(track["clips"], [
                {"position": 12000, "length": 24000, "offset": 6000, "channels": 1, "gain": .375},
                {"position": 48000, "length": 12000, "offset": 30000, "channels": 1, "gain": 0}])
        for media in parsed["media"]:
            with wave.open(media["path"], "rb") as source:
                self.assertEqual(source.getsampwidth(), 3)
                self.assertEqual(hashlib.sha256(source.readframes(source.getnframes())).hexdigest(),
                                 self.checksums[source.getnchannels()])
        tracks = read_midi(self.output / "Project.mid")
        self.assertEqual(len(tracks), 2)
        self.assertIn((0, 255, 81, b"\x09\x27\xc0"), tracks[0])  # 600000 us / quarter
        self.assertIn((0, 255, 88, bytes([3, 2, 24, 8])), tracks[0])
        self.assertIn((0, 255, 84, bytes([0x20, 0, 0, 0, 0])), tracks[0])
        self.assertIn((16 * PPQN, 255, 6, "Refrain 音楽".encode()), tracks[0])
        notes = [event for event in tracks[1] if event[1] != 255]
        self.assertEqual(notes, [(int(at * PPQN), status, key, velocity) for at, status, key, velocity in (
            (8, 0x92, 60, 83), (9, 0x82, 60, 30), (9.5, 0x92, 64, 83),
            (10, 0x82, 64, 30), (11.5, 0x92, 67, 83), (12, 0x82, 67, 30))])
        self.assertTrue(all(track[-1][0] == 16 * PPQN for track in tracks))
        with aaf2.open(str(self.output / "Project.aaf"), "r") as aaf:
            composition = next(aaf.content.toplevel())
            slots = list(composition.slots)
            sound = [slot for slot in slots if slot.name.startswith("Audio ")]
            self.assertEqual([slot["PhysicalTrackNumber"].value for slot in sound], [1, 1, 2])
            self.assertEqual([slot.name for slot in sound], ["Audio 1 café", "Audio 2 café", "Audio 2 café"])
            event = next(slot for slot in slots if isinstance(slot, aaf2.mobslots.EventMobSlot))
            self.assertEqual(event.edit_rate, 48000)
            self.assertEqual(event.name, "Markers")
            marker, = event.segment.components
            self.assertIsInstance(marker, aaf2.components.DescriptiveMarker)
            self.assertEqual(marker["Position"].value, 460800)
            self.assertEqual(marker["Comment"].value, "Refrain 音楽")
            tc = slots[-1].segment
            self.assertEqual((tc.start, tc.length, tc.fps), (0, 240, 25))
        metadata = ET.parse(self.output / "Metadata.xml").getroot()
        self.assertTrue(all("path" not in media.attrib for media in metadata.findall("Media")))
        self.assertEqual(metadata.find("AudioTrack/Clip").attrib["color"], "#abcdef")
        for output in metadata.findall("OutputFile"):
            self.assertEqual(hashlib.sha256((self.output / output.attrib["name"]).read_bytes()).hexdigest(), output.attrib["sha256"])

    def test_variable_transport_and_duration(self):
        self.root.set("durationBeats", "24")
        for at, bpm in ((0, 60), (4.25, 120), (8.25, 90)):
            ET.SubElement(self.root, "Tempo", time=str(at), bpm=str(bpm))
        for at, n, d in ((0, 4, 4), (4.25, 3, 4), (8.25, 7, 8)):
            ET.SubElement(self.root, "TimeSignature", time=str(at), numerator=str(n), denominator=str(d))
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        tracks = read_midi(self.output / "Project.mid")
        tempos = [(tick / PPQN, int.from_bytes(payload, "big"))
                  for tick, status, kind, payload in tracks[0] if kind == 81]
        self.assertEqual(tempos, [(0, 1000000), (4.25, 500000), (8.25, 666667)])
        meters = [(tick / PPQN, payload[:2]) for tick, status, kind, payload in tracks[0] if kind == 88]
        self.assertEqual(meters, [(0, bytes([4, 2])), (4.25, bytes([3, 2])), (8.25, bytes([7, 3]))])
        self.assertTrue(all(track[-1][0] == 24 * PPQN for track in tracks))

    def test_rejects_transport_tick_collision(self):
        ET.SubElement(self.root, "Tempo", time="1", bpm="100")
        ET.SubElement(self.root, "Tempo", time="1.00000001", bpm="120")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("collide", result.stderr)

    def test_mixer_automation_and_operation_duration(self):
        for track in self.root.findall('AudioTrack'):
            curve = ET.SubElement(track, 'GainAutomation')
            for at, value, linear in ((0, .25, True), (.5, .75, False), (2, .5, True), (3, .75, False)):
                ET.SubElement(curve, 'Point', time=str(at), value=str(value), linear=str(linear).lower())
        curve = ET.SubElement(self.root.find('AudioTrack'), 'PanAutomation')
        for at, value in ((.25, .2), (2.5, .8)):
            ET.SubElement(curve, 'Point', time=str(at), value=str(value), linear='false')
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / 'extracted'
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / 'Project.aaf'), str(extracted)],
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        tracks = json.loads(result.stdout)['tracks']
        for track in tracks:
            self.assertEqual(track['gain_length'], 144000)
            points = track['gain_curve']
            self.assertEqual(len(points), 5)
            expected = [(0, .25), (.5, .75), (2 - 1 / 48000, .75), (2, .5), (3, .75)]
            for (at, value), (time, level) in zip(points, expected):
                self.assertAlmostEqual(at * 3, time, delta=1e-10)
                self.assertEqual(value, level)
        self.assertEqual(tracks[0]['pan_length'], 144000)
        for (at, value), (time, level) in zip(tracks[0]['pan_curve'], ((.25, .2), (2.5, .8))):
            self.assertAlmostEqual(at * 3, time, delta=1e-10)
            self.assertEqual(value, level)

    def test_rejects_mixer_automation_collision(self):
        curve = ET.SubElement(self.root.find('AudioTrack'), 'GainAutomation')
        for at in (1, 1.000000001):
            ET.SubElement(curve, 'Point', time=str(at), value='.5', linear='true')
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('collide', result.stderr)

    def test_clip_gain_envelopes(self):
        for track in self.root.findall('AudioTrack'):
            clip = track.find('Clip')
            curve = ET.SubElement(clip, 'GainAutomation')
            for at, value in ((0, .2), (.1, 1.4), (.3, .5), (.5, 0)):
                ET.SubElement(curve, 'Point', time=str(at), value=str(value), linear='true')
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / 'envelope-audio'
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / 'Project.aaf'), str(extracted)], capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for track in json.loads(result.stdout)['tracks']:
            clip = track['clips'][0]
            self.assertEqual(clip['envelope_length'], 24000)
            for actual, expected in zip(clip['envelope'], ((0, .075), (.2, .525), (.6, .1875), (1, 0))):
                self.assertAlmostEqual(actual[0], expected[0])
                self.assertAlmostEqual(actual[1], expected[1])
            self.assertEqual(len(clip['envelope']), 4)

    def test_rejects_clip_envelope_outside_range(self):
        curve = ET.SubElement(self.root.find('AudioTrack/Clip'), 'GainAutomation')
        ET.SubElement(curve, 'Point', time='0.6', value='.5', linear='true')
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside clip', result.stderr)

    def test_refuses_existing_output(self):
        sentinel = self.output / "Project.aaf"
        sentinel.write_bytes(b"existing project")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_bytes(), b"existing project")
        self.assertFalse((self.output / "Project.mid").exists())

    def test_reader_rejects_used_unresolved_extension_type(self):
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        original = self.output / "Project.aaf"
        for stored in (False, True):
            with self.subTest(stored=stored):
                path = self.directory / ("extension-%s.aaf" % stored)
                path.write_bytes(original.read_bytes())
                with aaf2.open(str(path), "rw") as output:
                    composition = next(output.content.toplevel())
                    definition = composition.classdef.register_propertydef(
                        "ExchangeTestExtension", "84f8e1e6-89e8-45ed-a263-1ba76aa2199f",
                        None, "aafUInt8", True)
                    if stored:
                        composition["ExchangeTestExtension"].value = 7
                    # Break the dictionary reference while leaving the object's
                    # encoded property intact, as in an incomplete vendor file.
                    definition.property_entries[aaf2.metadict.PID_TYPE].data = aaf2.auid.AUID(
                        "f128d426-62af-427f-950a-26d120bd53eb").bytes_le
                    definition.write_properties()
                extracted = self.directory / ("extension-media-%s" % stored)
                extracted.mkdir()
                result = subprocess.run([self.probe, str(path), str(extracted)], capture_output=True,
                                        text=True, encoding="utf-8", timeout=30)
                if stored:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("with an unresolved extension type", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(len(json.loads(result.stdout)["tracks"]), 3)

    def test_reader_preserves_stereo_gain_automation(self):
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        path = self.output / "Project.aaf"
        with aaf2.open(str(path), "rw") as output:
            composition = next(output.content.toplevel())
            gain = list(composition.slots)[1].segment
            gain.parameters.clear()
            linear = output.create.InterpolationDef("5b6c85a4-0ede-11d3-80a9-006008143e6f", "Linear")
            output.dictionary.register_def(linear)
            curve = output.create.VaryingValue("Amplitude", "Linear")
            curve.add_keyframe(0, "1/4")
            curve.add_keyframe(1, "3/4")
            gain.parameters.append(curve)
        extracted = self.directory / "extracted"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(path), str(extracted)], capture_output=True,
                                text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["tracks"][1]["gain_curve"], [[0, .25], [1, .75]])

    def test_midi_controller_bytes_trim_and_chase(self):
        self.root.set('version', '2')
        clip = self.root.find('MidiTrack/Clip')
        for kind, number_, values in [(0xB0, 64, [127, 0, 127]), (0xE0, 0, [8192, 16383, 0]), (0xD0, 0, [15, 99, 0]), (0xA0, 60, [31, 100, 0])]:
            lane = ET.SubElement(clip, 'MidiControl', type=str(kind), channel='2', number=str(number_))
            for at, value in zip([1, 3, 6], values):
                ET.SubElement(lane, 'Point', time=str(at), value=str(value))
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        events = read_midi(self.output / 'Project.mid')[1]
        controls = [e for e in events if e[1] != 255 and e[1] & 0xF0 not in (0x80, 0x90)]
        self.assertEqual(controls, [(8*PPQN, 0xB2, 64, 127), (8*PPQN, 0xE2, 0, 64), (8*PPQN, 0xD2, 15), (8*PPQN, 0xA2, 60, 31),
                                    (9*PPQN, 0xB2, 64, 0), (9*PPQN, 0xE2, 127, 127), (9*PPQN, 0xD2, 99), (9*PPQN, 0xA2, 60, 100)])
        at_start = [e for e in events if e[0] == 8*PPQN]
        self.assertEqual(at_start[-1][1] & 0xF0, 0x90)

    def test_rejects_controller_collisions_and_ordered_commands(self):
        clip = self.root.find('MidiTrack/Clip')
        lane = ET.SubElement(clip, 'MidiControl', type='176', channel='2', number='64')
        ET.SubElement(lane, 'Point', time='3', value='127')
        point = ET.SubElement(lane, 'Point', time='3.000001', value='0')
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('tick resolution', result.stderr)
        point.set('time', '4')
        lane.set('number', '101')
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unsupported MIDI controller target', result.stderr)

    def test_rejects_chased_controller_tick_collision(self):
        clip = self.root.find('MidiTrack/Clip')
        lane = ET.SubElement(clip, 'MidiControl', type='176', channel='2', number='64')
        ET.SubElement(lane, 'Point', time='1', value='127')
        ET.SubElement(lane, 'Point', time='2.000001', value='0')
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('tick resolution', result.stderr)

    def test_rejects_ambiguous_overlapping_notes(self):
        clip = self.root.find("MidiTrack/Clip")
        ET.SubElement(clip, "Note", time="2.5", length="1", key="60", channel="2", velocity="83", release="30")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Overlapping notes", result.stderr)

    def test_stereo_balance_becomes_independent_channel_levels(self):
        for pan, levels in ((0, (.75, 0)), (.25, (.75, .375)),
                            (.75, (.375, .75)), (1, (0, .75))):
            with self.subTest(pan=pan):
                self.output = self.directory / ("balance-" + str(pan))
                self.output.mkdir()
                self.root.findall("AudioTrack")[1].set("pan", str(pan))
                result = self.run_writer()
                self.assertEqual(result.returncode, 0, result.stderr)
                extracted = self.directory / ("extracted-" + str(pan))
                extracted.mkdir()
                result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                        capture_output=True, text=True, encoding="utf-8", timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                tracks = json.loads(result.stdout)["tracks"]
                self.assertEqual([track["gain"] for track in tracks[1:]], list(levels))
                self.assertEqual([track.get("pan", .5) for track in tracks[1:]], [.5, .5])

    def test_stereo_balance_ramp_and_hold_become_channel_curves(self):
        stereo = self.root.findall("AudioTrack")[1]
        automation = ET.SubElement(stereo, "PanAutomation")
        for at, value, linear in ((0, .25, True), (2, .75, False), (4, .25, True)):
            ET.SubElement(automation, "Point", time=str(at), value=str(value), linear=str(linear).lower())
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / "extracted"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for track, levels in zip(json.loads(result.stdout)["tracks"][1:],
                                 ((.75, .75, .375, .375, .75), (.375, .75, .75, .75, .375))):
            self.assertNotIn("pan_curve", track)
            self.assertEqual(track["gain_length"], 192000)
            self.assertEqual([(round(at * 192000), value) for at, value in track["gain_curve"]],
                             list(zip((0, 48000, 96000, 191999, 192000), levels)))

    def test_stereo_balance_center_between_samples(self):
        stereo = self.root.findall("AudioTrack")[1]
        automation = ET.SubElement(stereo, "PanAutomation")
        for at, value in ((0, .2), (3 / 48000, .9)):
            ET.SubElement(automation, "Point", time=str(at), value=str(value), linear="true")
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / "extracted"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for track, levels in zip(json.loads(result.stdout)["tracks"][1:],
                                 ((.75, .75, .5, .15), (.3, .65, .75, .75))):
            points = [(round(at * track["gain_length"]), value) for at, value in track["gain_curve"]]
            self.assertEqual([at for at, _ in points], [0, 1, 2, 3])
            for (_, actual), expected in zip(points, levels):
                self.assertAlmostEqual(actual, expected, delta=1e-6)

    def test_static_stereo_balance_with_gain_automation(self):
        stereo = self.root.findall("AudioTrack")[1]
        automation = ET.SubElement(stereo, "GainAutomation")
        for at, value in ((0, .4), (2, .8)):
            ET.SubElement(automation, "Point", time=str(at), value=str(value), linear="true")
        for pan, levels in ((0, ((.4, .8), (0, 0))),
                            (.25, ((.4, .8), (.2, .4))),
                            (1, ((0, 0), (.4, .8)))):
            with self.subTest(pan=pan):
                self.output = self.directory / ("gain-balance-" + str(pan))
                self.output.mkdir()
                stereo.set("pan", str(pan))
                result = self.run_writer()
                self.assertEqual(result.returncode, 0, result.stderr)
                extracted = self.directory / ("extracted-" + str(pan))
                extracted.mkdir()
                result = subprocess.run([self.probe, str(self.output / "Project.aaf"), str(extracted)],
                                        capture_output=True, text=True, encoding="utf-8", timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                for track, expected in zip(json.loads(result.stdout)["tracks"][1:], levels):
                    self.assertNotIn("pan_curve", track)
                    self.assertEqual(track["gain_length"], 96000)
                    self.assertEqual([(round(at * 96000), value) for at, value in track["gain_curve"]],
                                     [(0, expected[0]), (96000, expected[1])])

    def test_rejects_moving_stereo_balance_with_gain_automation(self):
        stereo = self.root.findall("AudioTrack")[1]
        stereo.set("pan", ".25")
        automation = ET.SubElement(stereo, "GainAutomation")
        ET.SubElement(automation, "Point", time="0", value=".5", linear="true")
        pan = ET.SubElement(stereo, "PanAutomation")
        ET.SubElement(pan, "Point", time="0", value=".25", linear="true")
        ET.SubElement(pan, "Point", time="2", value=".75", linear="true")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Moving stereo balance with gain automation", result.stderr)

    def test_rejects_zero_gain_stereo_balance_automation(self):
        stereo = self.root.findall("AudioTrack")[1]
        stereo.set("gain", "0")
        automation = ET.SubElement(stereo, "PanAutomation")
        ET.SubElement(automation, "Point", time="0", value=".25", linear="true")
        ET.SubElement(automation, "Point", time="2", value=".75", linear="true")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("zero gain", result.stderr)

    def test_rejects_audio_outside_source(self):
        self.root.find("AudioTrack/Clip").set("offset", "95000")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source range", result.stderr)

    def test_rejects_overlapping_audio(self):
        self.root.find("AudioTrack").findall("Clip")[1].set("time", "13000")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Overlapping", result.stderr)

    def test_mono_pairwise_overlap_preserves_clip_timing(self):
        track = self.root.find("AudioTrack")
        track.findall("Clip")[1].set("time", "30000")
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / "overlap-audio"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / 'Project.aaf'), str(extracted)], capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        track = json.loads(result.stdout)['tracks'][0]
        self.assertEqual([c['position'] for c in track['clips']], [12000, 30000])
        self.assertEqual([c['length'] for c in track['clips']], [24000, 12000])
        self.assertEqual(len(track['transitions']), 1)
        transition = track['transitions'][0]
        self.assertEqual(transition['length'], 6000)
        self.assertEqual(transition['points'], [[0, 1], [1, 1]])

    def test_stereo_pairwise_overlap_preserves_both_channels(self):
        track = self.root.findall("AudioTrack")[1]
        track.findall("Clip")[1].set("time", "30000")
        result = self.run_writer()
        self.assertEqual(result.returncode, 0, result.stderr)
        extracted = self.directory / "stereo-overlap-audio"
        extracted.mkdir()
        result = subprocess.run([self.probe, str(self.output / 'Project.aaf'), str(extracted)],
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        tracks = json.loads(result.stdout)['tracks']
        self.assertEqual(len(tracks), 3)
        for channel in tracks[1:]:
            self.assertEqual(channel['channels'], 1)
            self.assertEqual([c['position'] for c in channel['clips']], [12000, 30000])
            self.assertEqual([c['length'] for c in channel['clips']], [24000, 12000])
            self.assertEqual(len(channel['transitions']), 1)
            self.assertEqual(channel['transitions'][0]['length'], 6000)
            self.assertEqual(channel['transitions'][0]['points'], [[0, 1], [1, 1]])

    def test_rejects_three_simultaneous_audio_clips(self):
        track = self.root.find("AudioTrack")
        track.findall("Clip")[1].set("time", "30000")
        third = ET.fromstring(ET.tostring(track.findall("Clip")[1]))
        third.set("time", "32000")
        third.set("length", "24000")
        track.append(third)
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pairwise", result.stderr)

    def test_rejects_three_simultaneous_stereo_clips(self):
        track = self.root.findall("AudioTrack")[1]
        track.findall("Clip")[1].set("time", "30000")
        third = ET.fromstring(ET.tostring(track.findall("Clip")[1]))
        third.set("time", "32000")
        third.set("length", "24000")
        track.append(third)
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pairwise", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    LogicWriterTest.probe = str(args.probe.resolve())
    unittest.main(argv=[__file__], verbosity=2)
