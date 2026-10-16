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


def make_fixture(path, rate=48000, right_position=12000, empty_pair=True, independent_levels=False):
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
                    for at, value in enumerate(("1/4", "3/4") if channel == 1 else ("1/2", "1/4")):
                        curve.add_keyframe(at, value)
                    group.parameters.append(curve)
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
    (destination / "Project.mid").write_bytes(bytes.fromhex(
        "4d54686400000006000000014b004d54726b0000000400ff2f00"))
    (destination / "Metadata.xml").write_text("<IndependentFixture/>", encoding="utf-8")
