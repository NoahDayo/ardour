# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent native-song fixtures; real application samples stay outside Git."""
import argparse
import io
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zipfile


def ubj(value):
    if isinstance(value, dict):
        return b"{" + b"".join(ubj(len(k.encode())) + k.encode() + ubj(v) for k, v in value.items()) + b"}"
    if isinstance(value, list):
        return b"[" + b"".join(map(ubj, value)) + b"]"
    if isinstance(value, int) and -128 <= value <= 127:
        return b"i" + struct.pack("b", value)
    if isinstance(value, str):
        return b"S" + ubj(len(value.encode())) + value.encode()
    return b"D" + struct.pack(">d", value)


META = '''<MetaInformation>
<Attribute id="Document:MimeType" value="application/x.presonus-song"/>
<Attribute id="Document:FormatVersion" value="9"/>
<Attribute id="Document:Generator" value="Independent test fixture"/>
</MetaInformation>'''
SONG = '''<Song><Attributes x:id="Root"><Attributes x:id="timeContext" sampleRate="48000">
<TempoMap x:id="tempoMap"><TempoMapSegment start="0" tempo="0.5" curveType="0"/></TempoMap>
<TimeSignatureMap x:id="timeSignatureMap"><TimeSignatureMapSegment start="0" numerator="4" denominator="4"/></TimeSignatureMap>
</Attributes><List x:id="Tracks">
<MarkerTrack timeFormat="2"><MarkerEvent markerType="2" name="Start"/>
<MarkerEvent markerType="3" start="32" name="End"/><MarkerEvent start="8" name="Verse"/></MarkerTrack>
<MediaTrack trackID="audio" mediaType="Audio" name="Audio" color="FF123456" timeFormat="2">
<UID x:id="channelID" uid="audio-channel"/><SpeakerSetup x:id="trackFormat" type="Stereo"/>
<List x:id="Events"><AudioEvent clipID="wav" name="Trimmed" start="1" length="1" offset="0.25" timeFormat="0"/></List>
</MediaTrack><MediaTrack trackID="midi" mediaType="Music" name="Notes" timeFormat="2">
<UID x:id="channelID" uid="midi-channel"/><List x:id="Events"><MusicPart clipID="notes" start="8" length="4" offset="1"/></List>
</MediaTrack></List></Attributes></Song>'''
MIXER = '''<AudioMixer><Attributes x:id="channels"><ChannelGroup name="AudioTrack">
<AudioTrackChannel gain="0.75" pan="0.25" mute="1" solo="0"><UID x:id="uniqueID" uid="audio-channel"/>
<Connection x:id="destination" objectID="master/Input"/></AudioTrackChannel></ChannelGroup>
<ChannelGroup name="AudioOutput"><AudioOutputChannel label="Main" gain="1"><UID x:id="uniqueID" uid="master"/></AudioOutputChannel>
</ChannelGroup></Attributes></AudioMixer>'''
MUSIC = '''<MusicTrackDevice><Attributes x:id="channels"><MusicTrackChannel><UID x:id="uniqueID" uid="midi-channel"/></MusicTrackChannel></Attributes></MusicTrackDevice>'''
POOL = '''<MediaPool><Attributes x:id="documentPath" url="{song}"/><Attributes x:id="rootFolder"><MediaFolder>
<AudioClip mediaID="wav"><Url x:id="path" url="{audio}"/><Attributes x:id="format" sampleRate="48000" numChannels="2" frameCount="192000"/></AudioClip>
<MusicClip mediaID="notes"><Url x:id="dataPath" url="media:///Performances/notes.musicx"/></MusicClip>
</MediaFolder></Attributes></MediaPool>'''


class NativeSongTest(unittest.TestCase):
    probe = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ardour-native-song-")
        self.root = Path(self.temporary.name).resolve()
        self.addCleanup(self.temporary.cleanup)
        (self.root / "Media").mkdir()
        audio = self.root / "Media/tone.wav"
        with wave.open(str(audio), "wb") as wav:
            wav.setparams((2, 2, 48000, 192000, "NONE", "not compressed"))
            wav.writeframes(b"\0\1\0\2" * 192000)
        self.song = self.root / "Sample.song"
        self.entries = {
            "metainfo.xml": META,
            "Song/song.xml": SONG,
            "Song/mediapool.xml": POOL.format(song=self.song.as_uri(), audio=audio.as_uri()),
            "Devices/audiomixer.xml": MIXER,
            "Devices/musictrackdevice.xml": MUSIC,
            "Devices/audiosynthfolder.xml": '<AudioSynthFolder><Attributes x:id="Presets"/></AudioSynthFolder>',
            "Performances/notes.musicx": ubj({"timeFormat": 2, "events": [
                {"start": 1.25, "length": 0.5, "pitch": 60, "velocity": 0.75, "channel": 2},
                {"start": 2.0, "length": 1.0, "pitch": 67, "velocity": 0.5}], "envelopes": []}),
        }

    def save(self):
        with zipfile.ZipFile(self.song, "w", zipfile.ZIP_DEFLATED) as z:
            for name, content in self.entries.items():
                z.writestr(name, content)

    def read(self, status=0, path=None):
        if path is None:
            self.save()
        result = subprocess.run([self.probe, "read-song", str(path or self.song)], capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, status, result.stderr)
        return json.loads(result.stdout) if status == 0 else result.stderr

    def test_audio_midi_mixer_and_timing(self):
        song = self.read()
        p = song["project"]
        self.assertEqual((p["tempo"], p["meter"]), (120, [4, 4]))
        a, m, master = p["tracks"]
        self.assertEqual((a["name"], a["volume"], a["pan"], a["mute"], a["destination"]), ("Audio", .75, .25, True, "master"))
        self.assertEqual((a["clips"][0]["position"], a["clips"][0]["offset"], a["clips"][0]["duration"]), ([1, "seconds"], [.25, "seconds"], 1))
        self.assertEqual(m["clips"][0]["notes"], [[1.25, .5, 60, 2, .75, .5], [2, 1, 67, 0, .5, .5]])
        self.assertEqual(m["clips"][0]["position"], [8, "beats"])
        self.assertEqual(m["clips"][0]["offset"], [1, "beats"])
        self.assertTrue(master["master"])
        self.assertEqual(song["markers"], [["Verse", [8, "beats"]]])
        self.assertEqual((song["start"], song["end"], song["has_range"]), ([0, "beats"], [32, "beats"], True))
        self.assertFalse(any(d["disposition"] == 3 for d in p["report"]))

    def test_relocated_song_uses_local_media(self):
        self.save()
        relocated = self.root / "Relocated with spaces"
        relocated.mkdir()
        shutil.copy2(self.song, relocated / self.song.name)
        shutil.copytree(self.root / "Media", relocated / "Media")
        (self.root / "Media/tone.wav").unlink()
        self.assertEqual(len(self.read(path=relocated / self.song.name)["project"]["media"]), 1)

    def test_missing_and_outside_media_refused(self):
        pool = self.entries["Song/mediapool.xml"]
        for uri in ((self.root.parent / "outside.wav").as_uri(), "https://example.invalid/audio.wav", "file://server/share/audio.wav"):
            self.entries["Song/mediapool.xml"] = pool.replace((self.root / "Media/tone.wav").as_uri(), uri)
            self.read(status=1)
        self.entries["Song/mediapool.xml"] = pool
        (self.root / "Media/tone.wav").unlink()
        self.assertIn("Missing", self.read(status=1))

    def test_malformed_xml_and_entities_refused(self):
        for document in ('<Song>', '<!DOCTYPE Song [<!ENTITY x SYSTEM "file:///etc/passwd">]><Song>&x;</Song>', '<Wrong/>'):
            self.entries["Song/song.xml"] = document
            self.read(status=1)

    def test_unknown_version_and_duplicate_ids_refused(self):
        self.entries["metainfo.xml"] = META.replace('value="9"', 'value="999"')
        self.assertIn("version", self.read(status=1))
        self.entries["metainfo.xml"] = META
        self.entries["Song/song.xml"] = SONG.replace('trackID="midi"', 'trackID="audio"')
        self.read(status=1)

    def test_bad_midi_binary_refused(self):
        valid = self.entries["Performances/notes.musicx"]
        for data in (valid[:-1], valid + b"extra", b"{i\xff", b"{i\x06events[#L\x7f" + b"\xff" * 7, b"{" * 100,
                     ubj({"events": [{"pitch": 60, "length": math.nan}]}),
                     ubj({"events": [{"pitch": 128, "length": 1}]}),
                     ubj({"events": [{"pitch": 60.5, "length": 1}]})):
            self.entries["Performances/notes.musicx"] = data
            self.read(status=1)

    def test_optimized_midi_container(self):
        event = ubj({"pitch": 78, "length": 1., "velocity": .5})
        # Counted, typed object array; its closing bracket is intentionally absent.
        self.entries["Performances/notes.musicx"] = b'{i\x06events[${#i\x01' + event[1:] + b'}'
        notes = self.read()["project"]["tracks"][1]["clips"][0]["notes"]
        self.assertEqual(notes[0][2], 78)

    def test_negative_range_refused(self):
        self.entries["Song/song.xml"] = SONG.replace('length="1"', 'length="-1"')
        self.read(status=1)

    def test_unsupported_processing_is_reported(self):
        for before, after in [('length="1"', 'length="1" speed="2"'), ('length="1"', 'length="1" gain="0.5"'),
                              ('name="Trimmed"', 'name="Trimmed" unknownProcess="1"')]:
            self.entries["Song/song.xml"] = SONG.replace(before, after)
            self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_variable_tempo_and_instruments_reported(self):
        self.entries["Song/song.xml"] = SONG.replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/></TempoMap>')
        self.entries["Devices/audiosynthfolder.xml"] = '<AudioSynthFolder><AudioSynth name="Synth"/></AudioSynthFolder>'
        report = self.read()["project"]["report"]
        self.assertTrue(any(d["object"] == "Tempo map" and d["disposition"] == 3 for d in report))
        self.assertTrue(any(d["object"] == "Instruments" and d["disposition"] == 3 for d in report))

    def test_empty_bipolar_automation_allowed_but_points_reported(self):
        region = '<Attributes x:id="AutomationRegionList"><AutomationRegion title="Pan"><Url x:id="points" url="media:///pan.envelopex"/></AutomationRegion></Attributes>'
        self.entries["Song/song.xml"] = SONG.replace('<SpeakerSetup', region + '<SpeakerSetup')
        self.entries["pan.envelopex"] = ubj({"bipolar": 1, "events": []})
        self.assertFalse(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))
        self.entries["pan.envelopex"] = ubj({"bipolar": 1, "events": [{"time": 0., "value": .5}]})
        self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args, remaining = parser.parse_known_args()
    NativeSongTest.probe = str(Path(args.probe).resolve())
    unittest.main(argv=[__file__, *remaining], verbosity=2)
