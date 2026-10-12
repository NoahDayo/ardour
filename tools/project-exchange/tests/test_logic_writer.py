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
                assert status & 0xF0 in (0x80, 0x90)
                events.append((tick, status, *data[position:position + 2]))
                position += 2
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
    ET.SubElement(root, "Marker", name="Refrain 音楽", time="16")
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
        self.assertEqual(len(parsed["tracks"]), 2)
        for i, track in enumerate(parsed["tracks"], 1):
            self.assertEqual(track["name"], "Audio %d café" % i)
            self.assertEqual(track["channels"], i)
            self.assertEqual(track["rate"], [48000, 1])
            self.assertEqual(track["gain"], .75)
            if i == 1:
                self.assertEqual(track["pan"], .25)
            self.assertEqual(track["clips"], [
                {"position": 12000, "length": 24000, "offset": 6000, "channels": i, "gain": .375},
                {"position": 48000, "length": 12000, "offset": 30000, "channels": i, "gain": 0}])
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
            tc = list(composition.slots)[-1].segment
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
                    self.assertEqual(len(json.loads(result.stdout)["tracks"]), 2)

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

    def test_rejects_ambiguous_overlapping_notes(self):
        clip = self.root.find("MidiTrack/Clip")
        ET.SubElement(clip, "Note", time="2.5", length="1", key="60", channel="2", velocity="83", release="30")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Overlapping notes", result.stderr)

    def test_rejects_unmapped_stereo_balance(self):
        self.root.findall("AudioTrack")[1].set("pan", ".25")
        result = self.run_writer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Stereo balance", result.stderr)

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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    LogicWriterTest.probe = str(args.probe.resolve())
    unittest.main(argv=[__file__], verbosity=2)
