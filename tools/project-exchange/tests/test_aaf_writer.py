# SPDX-License-Identifier: GPL-2.0-or-later
"""Check an independent writer's output with Ardour's actual AAF reader.

Run with --probe PATH_TO_AAF_PROBE. All media is generated in a temporary
directory; these fixtures are not exports from Logic Pro or Studio Pro.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave
import importlib.util

import aaf2

fixture_spec = importlib.util.spec_from_file_location("aaf_transitions", Path(__file__).resolve().parents[3] / "libs/ardour/test/data/aaf_transitions.py")
transition_fixtures = importlib.util.module_from_spec(fixture_spec)
fixture_spec.loader.exec_module(transition_fixtures)


def pcm_wave(path, channels, frames=96000, rate=48000):
    # Distinct deterministic signals detect sample offsets and channel swaps.
    data = b"".join(
        struct.pack("<h", (frame * (channel + 3) * 29) % 40001 - 20000)
        for frame in range(frames) for channel in range(channels)
    )
    with wave.open(str(path), "wb") as output:
        output.setparams((channels, 2, rate, frames, "NONE", "not compressed"))
        output.writeframes(data)
    return hashlib.sha256(data).hexdigest()


def make_fixture(directory):
    audio_paths = [directory / "mono.wav", directory / "stereo.wav"]
    checksums = [pcm_wave(path, i + 1) for i, path in enumerate(audio_paths)]
    target = directory / "timeline.aaf"
    with aaf2.open(str(target), "w") as output:
        combiner = output.create.OperationDef(
            "6b46dd7a-132d-4856-ab21-8b751d8462ec", "AudioChannelCombiner")
        combiner.media_kind = "sound"
        combiner.number_inputs = 2
        combiner["IsTimeWarp"].value = False
        output.dictionary.register_def(combiner)
        composition = output.create.CompositionMob("Exchange writer test")
        composition.usage = "Usage_TopLevel"
        output.content.mobs.append(composition)
        for index, path in enumerate(audio_paths):
            master = output.create.MasterMob(path.stem)
            output.content.mobs.append(master)
            source_slot = master.import_audio_essence(str(path), edit_rate=48000)
            source_slot["PhysicalTrackNumber"].value = 1
            channel_slots = [source_slot]
            if index == 1:
                # Two channel references to one embedded interleaved essence.
                right = master.create_timeline_slot(edit_rate=48000)
                right.segment = source_slot.segment.copy()
                right["PhysicalTrackNumber"].value = 2
                channel_slots.append(right)
            slot = composition.create_sound_slot(edit_rate=48000)
            slot.name = ["Mono café", "Stereo"][index]
            clips = [(0, 12000, 24000), (48000, 36000, 12000)] if index == 0 else [(72000, 6000, 18000)]
            end = 0
            for position, offset, length in clips:
                if position > end:
                    slot.segment.components.append(output.create.Filler("sound", position - end))
                references = [master.create_source_clip(
                    channel.slot_id, start=offset, length=length, media_kind="sound")
                    for channel in channel_slots]
                if len(references) == 1:
                    segment = references[0]
                else:
                    segment = output.create.OperationGroup(combiner, length, media_kind="sound")
                    segment.segments.extend(references)
                slot.segment.components.append(segment)
                end = position + length
            slot.segment.length = end
    # Prove that the reader does not find the original WAVs by accident.
    for path in audio_paths:
        path.unlink()
    return target, checksums


class AafWriterTest(unittest.TestCase):
    probe = None

    def test_dissolve_default_and_explicit_points(self):
        for mode in ("default", "custom", "hold"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="ardour-aaf-fades-") as work:
                directory = Path(work)
                transition_fixtures.make_transition_fixture(directory / "fades.aaf", mode=mode)
                result = subprocess.run([self.probe, str(directory / "fades.aaf"), str(directory)], capture_output=True, text=True, encoding="utf-8", timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                track = json.loads(result.stdout)["tracks"][0]
                self.assertEqual([(c["position"], c["length"], c["offset"]) for c in track["clips"]], [(24000, 96000, 6000), (96000, 96000, 18000)])
                self.assertEqual([t["length"] for t in track["transitions"]], [12000, 24000, 24000])
                expected = [[0, 0], [1, 1]] if mode == "default" else [[0, 0], [.25, .8], [.75, .9], [1, 1]]
                for index, transition in enumerate(track["transitions"]):
                    self.assertEqual(transition["points"], expected)
                    self.assertTrue(transition["flags"] & (1 << (13 if mode == "hold" else 11)))
                    self.assertTrue(transition["flags"] & (1 << [6, 8, 7][index]))

    def test_incomplete_dissolve_level_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ardour-aaf-fades-") as work:
            directory = Path(work)
            transition_fixtures.make_transition_fixture(directory / "fades.aaf", mode="one-point")
            result = subprocess.run([self.probe, str(directory / "fades.aaf"), str(directory)], capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("dissolve Level curve", result.stderr)

    def test_embedded_audio_timeline(self):
        with tempfile.TemporaryDirectory(prefix="ardour-aaf-") as work:
            directory = Path(work)
            target, checksums = make_fixture(directory)
            extracted = directory / "extracted"
            extracted.mkdir()
            result = subprocess.run([self.probe, str(target), str(extracted)],
                                    capture_output=True, text=True, encoding="utf-8", timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            parsed = json.loads(result.stdout)
            self.assertEqual(parsed["tracks"], [
                {"name": "Mono café", "channels": 1, "rate": [48000, 1], "clips": [
                    {"position": 0, "length": 24000, "offset": 12000, "channels": 1},
                    {"position": 48000, "length": 12000, "offset": 36000, "channels": 1}]},
                {"name": "Stereo", "channels": 2, "rate": [48000, 1], "clips": [
                    {"position": 72000, "length": 18000, "offset": 6000, "channels": 2}]}])
            media = sorted(parsed["media"], key=lambda item: item["channels"])
            self.assertEqual(len(media), 2)
            for index, item in enumerate(media):
                self.assertEqual(item["channels"], index + 1)
                self.assertEqual(item["rate"], 48000)
                self.assertEqual(item["frames"], 96000)
                path = Path(item["path"])
                self.assertEqual(path.resolve().parent, extracted.resolve())
                with wave.open(str(path), "rb") as audio:
                    self.assertEqual(audio.getnchannels(), index + 1)
                    self.assertEqual(audio.getframerate(), 48000)
                    self.assertEqual(audio.getnframes(), 96000)
                    digest = hashlib.sha256(audio.readframes(audio.getnframes())).hexdigest()
                self.assertEqual(digest, checksums[index])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    AafWriterTest.probe = str(args.probe.resolve())
    unittest.main(argv=[__file__], verbosity=2)
