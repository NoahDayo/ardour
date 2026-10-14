# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent AAF dissolve fixtures; no target-application claims."""
from pathlib import Path
from fractions import Fraction
import struct
import sys
import wave
import xml.etree.ElementTree as ET

# The embedded interpreter keeps pyaaf2 outside its isolated standard library.
vendor = Path(sys.executable).resolve().parent.parent / "vendor"
if vendor.is_dir():
    sys.path.insert(0, str(vendor))
import aaf2


def make_transition_fixture(path, rate=48000, mode="custom"):
    source = path.with_suffix(".wav")
    with aaf2.open(str(path), "w") as output:
        operation = output.create.OperationDef("0c3bea41-fc05-11d2-8a29-0050040ef7d2", "MonoAudioDissolve")
        operation.media_kind = "sound"
        operation.number_inputs = 2
        operation["IsTimeWarp"].value = False
        level = output.create.ParameterDef("e4962320-2267-11d3-8a4c-0050040ef7d2", "Level", typedef="Rational")
        output.dictionary.register_def(level)
        operation["ParametersDefined"].append(level)
        output.dictionary.register_def(operation)
        for uid, name in (("5b6c85a4-0ede-11d3-80a9-006008143e6f", "Linear"),
                          ("5b6c85a5-0ede-11d3-80a9-006008143e6f", "Constant"),
                          ("5b6c85a6-0ede-11d3-80a9-006008143e6f", "BSpline")):
            output.dictionary.register_def(output.create.InterpolationDef(uid, name))
        composition = output.create.CompositionMob("Independent transition fixture")
        composition.usage = "Usage_TopLevel"
        output.content.mobs.append(composition)
        slot = composition.create_sound_slot(edit_rate=rate)
        slot.name = "Dissolves"

        def audio(name, value, offset):
            with wave.open(str(source), "wb") as wav:
                wav.setparams((1, 2, rate, 4 * rate, "NONE", "not compressed"))
                wav.writeframes(struct.pack("<h", value) * (4 * rate))
            master = output.create.MasterMob(name)
            output.content.mobs.append(master)
            imported = master.import_audio_essence(str(source), edit_rate=rate)
            imported["PhysicalTrackNumber"].value = 1
            return master.create_source_clip(imported.slot_id, start=offset, length=2 * rate, media_kind="sound")

        def fade(length, cross=False):
            if cross and mode == "long":
                length = 3 * rate
            transition = output.create.Transition("sound", length)
            transition.cutpoint = length + 1 if cross and mode == "bad-cut" else length // 2
            group = output.create.OperationGroup(operation, length, media_kind="sound")
            transition["OperationGroup"].value = group
            if mode != "default":
                curve = output.create.VaryingValue(level, "Constant" if mode == "hold" else "BSpline" if mode == "spline" else "Linear")
                points = [("0", "0"), ("1/4", "4/5"), ("3/4", "9/10"), ("1", "1")]
                if mode == "bad-level": points[1] = ("1/4", "6/5")
                if mode == "bad-time": points[0] = ("-1/4", "0")
                if mode == "missing-end": points.pop()
                if mode == "one-point": points = points[:1]
                if mode == "nonunity": points[-1] = ("1", "3/4")
                for time, value in points: curve.add_keyframe(float(Fraction(time)), value)
                group.parameters.append(curve)
            return transition

        slot.segment.components.extend([
            output.create.Filler("sound", 3 * rate // 4), fade(rate // 4),
            audio("First", 4096, rate // 8), fade(rate // 2, True),
            audio("Second", 8192, 3 * rate // 8), fade(rate // 2),
            output.create.Filler("sound", rate),
        ])
        slot.segment.length = 9 * rate // 2
    source.unlink()


if __name__ == "__main__":
    # Invoked as a test-only writer replacement by the native session tests.
    manifest, directory = Path(sys.argv[1]), Path(sys.argv[2])
    rate = int(ET.parse(manifest).getroot().get("rate"))
    for mode in ("default", "custom", "hold", "nonunity", "long", "bad-cut", "bad-level", "bad-time", "missing-end", "one-point", "spline"):
        make_transition_fixture(directory / (mode + ".aaf"), rate, mode)
    (directory / "Project.aaf").write_bytes((directory / "custom.aaf").read_bytes())
    (directory / "Project.mid").write_bytes(bytes.fromhex("4d54686400000006000000014b004d54726b0000000400ff2f00"))
    (directory / "Metadata.xml").write_text('<IndependentFixture/>', encoding="utf-8")
