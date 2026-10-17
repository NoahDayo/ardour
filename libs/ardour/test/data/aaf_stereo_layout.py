# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent paired-mono AAF fixtures for Ardour's Logic importer."""
from pathlib import Path
import struct
import sys
import wave

vendor = Path(sys.executable).resolve().parent.parent / "vendor"
if vendor.is_dir():
    sys.path.insert(0, str(vendor))
import aaf2


def make_fixture(path, rate=48000, right_position=12000, empty_pair=True, independent_levels=False,
                 proportional_levels=False, one_tag=False, terminal_duplicate=False,
                 rounded_duplicate=False, conflicting_duplicate=False):
    sources = []
    for name, sample in (("Left", 4096), ("Right", -8192)):
        source = path.with_name(path.stem + "-" + name + ".wav")
        with wave.open(str(source), "wb") as audio:
            audio.setparams((1, 2, rate, rate * 2, "NONE", "not compressed"))
            audio.writeframes(struct.pack("<h", sample) * (rate * 2))
        sources.append(source)
    try:
        with aaf2.open(str(path), "w") as output:
            composition = output.create.CompositionMob("Independent paired mono fixture")
            composition.usage = "Usage_TopLevel"
            output.content.mobs.append(composition)
            if independent_levels:
                amplitude = output.create.ParameterDef(
                    "e4962321-2267-11d3-8a4c-0050040ef7d2", "Amplitude", typedef="Rational")
                output.dictionary.register_def(amplitude)
                definition = output.create.OperationDef(
                    "9d2ea894-0968-11d3-8a38-0050040ef7d2", "MonoAudioGain")
                definition.media_kind = "sound"
                definition.number_inputs = 1
                definition["IsTimeWarp"].value = False
                definition["ParametersDefined"].append(amplitude)
                output.dictionary.register_def(definition)
                output.dictionary.register_def(output.create.InterpolationDef(
                    "5b6c85a4-0ede-11d3-80a9-006008143e6f", "Linear"))
                if one_tag:
                    output.dictionary.register_def(output.create.TaggedValueDef(
                        "21a8ae63-2486-43b5-a6f2-54132481bcf3", "Ardour.StereoGainBalance"))
            masters = []
            for source in sources:
                master = output.create.MasterMob(source.stem)
                output.content.mobs.append(master)
                slot = master.import_audio_essence(str(source), edit_rate=rate)
                slot["PhysicalTrackNumber"].value = 1
                masters.append((master, slot))
            for channel, (master, source_slot) in enumerate(masters, 1):
                slot = composition.create_sound_slot(edit_rate=rate)
                slot.name = "Stereo split"
                slot["PhysicalTrackNumber"].value = channel
                position = 12000 if channel == 1 else right_position
                slot.segment.components.append(output.create.Filler("sound", position))
                slot.segment.components.append(master.create_source_clip(
                    source_slot.slot_id, start=6000, length=24000, media_kind="sound"))
                slot.segment.length = position + 24000
                if independent_levels:
                    group = output.create.OperationGroup(definition, slot.segment.length, media_kind="sound")
                    group.segments.append(slot.segment.copy())
                    curve = output.create.VaryingValue(amplitude, "Linear")
                    levels = (("2/5", "4/5") if channel == 1 else ("1/5", "2/5")) if proportional_levels else (
                        ("1/4", "3/4") if channel == 1 else ("1/2", "1/4"))
                    def append_point(at, value):
                        point = output.create.ControlPoint()
                        point.time = at
                        point["Value"].add_pid_entry()
                        point["Value"].data = point["Value"].typedef.encode(value, curve.typedef)
                        point["Value"].mark_modified()
                        curve["PointList"].append(point)

                    if rounded_duplicate or conflicting_duplicate:
                        curve.add_keyframe(0, levels[0])
                        append_point(0, "1/8" if conflicting_duplicate else levels[0])
                        append_point(1 if conflicting_duplicate else
                                     f"{group.length + 1}/{group.length}", levels[1])
                    else:
                        for at, value in enumerate(levels):
                            curve.add_keyframe(at, value)
                    if terminal_duplicate:
                        # add_keyframe replaces an equal-time key, so append a
                        # raw ControlPoint as seen in app-authored AAF files.
                        append_point(1, "1/8")
                    group.parameters.append(curve)
                    if one_tag and channel == 1:
                        group["UserComments"].append(output.create.TaggedValue("Ardour.StereoGainBalance", "1"))
                    slot.segment = group
            if empty_pair:
                for channel in (1, 2):
                    slot = composition.create_sound_slot(edit_rate=rate)
                    slot.name = "Empty stereo"
                    slot["PhysicalTrackNumber"].value = channel
                    slot.segment.components.append(output.create.Filler("sound", 0))
                    slot.segment.length = 0
    finally:
        for source in sources:
            source.unlink()


if __name__ == "__main__":
    destination = Path(sys.argv[2])
    make_fixture(destination / "Project.aaf")
    make_fixture(destination / "Mismatched.aaf", right_position=12001, empty_pair=False)
    make_fixture(destination / "IndependentLevels.aaf", empty_pair=False, independent_levels=True)
    make_fixture(destination / "PartialTag.aaf", empty_pair=False, independent_levels=True,
                 proportional_levels=True, one_tag=True)
    make_fixture(destination / "TerminalDuplicate.aaf", empty_pair=False, independent_levels=True,
                 terminal_duplicate=True)
    make_fixture(destination / "RoundedDuplicate.aaf", empty_pair=False, independent_levels=True,
                 rounded_duplicate=True)
    make_fixture(destination / "ConflictingDuplicate.aaf", empty_pair=False, independent_levels=True,
                 conflicting_duplicate=True)
    (destination / "Project.mid").write_bytes(bytes.fromhex(
        "4d54686400000006000000014b004d54726b0000000400ff2f00"))
    (destination / "Metadata.xml").write_text("<IndependentFixture/>", encoding="utf-8")
