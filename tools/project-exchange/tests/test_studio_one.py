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
import xml.etree.ElementTree as ET


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


def unpack_music(data):
    stream = io.BytesIO(data)

    def value(marker=None):
        marker = marker or stream.read(1)
        if marker == b"i":
            return struct.unpack("b", stream.read(1))[0]
        if marker == b"D":
            return struct.unpack(">d", stream.read(8))[0]
        if marker == b"{":
            result = {}
            while True:
                lead = stream.read(1)
                if lead == b"}":
                    return result
                key = stream.read(value(lead)).decode("utf-8")
                if key in result:
                    raise ValueError("Duplicate UBJSON key")
                result[key] = value()
        if marker == b"[":
            result = []
            while True:
                lead = stream.read(1)
                if lead == b"]":
                    return result
                result.append(value(lead))
        raise ValueError(f"Unexpected UBJSON marker {marker!r}")

    result = value()
    if stream.read():
        raise ValueError("Trailing UBJSON data")
    return result


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
        self.assertEqual(p["markers"], [{"name": "Verse", "position": [8, "beats"]}])
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
        for before, after in [('length="1"', 'length="1" speed="2"'), ('length="4"', 'length="4" gain="0.5"'),
                              ('name="Trimmed"', 'name="Trimmed" unknownProcess="1"')]:
            self.entries["Song/song.xml"] = SONG.replace(before, after)
            self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_tempo_curves_and_instruments_reported(self):
        self.entries["Song/song.xml"] = SONG.replace('curveType="0"', 'curveType="1"').replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/></TempoMap>')
        self.entries["Devices/audiosynthfolder.xml"] = '<AudioSynthFolder><AudioSynth name="Synth"/></AudioSynthFolder>'
        report = self.read()["project"]["report"]
        self.assertTrue(any(d["object"] == "Tempo map" and d["disposition"] == 3 for d in report))
        self.assertTrue(any(d["object"] == "Instruments" and d["disposition"] == 3 for d in report))

    def test_tempo_meter_steps_and_audio_gain(self):
        self.entries["Song/song.xml"] = (SONG.replace('tempo="0.5"', 'tempo="0.5" end="8"')
            .replace('</TempoMap>', '<TempoMapSegment start="8" end="12.5" tempo="0.75"/><TempoMapSegment start="12.5" end="1.7976931348623156e+200" tempo="0.25"/></TempoMap>')
            .replace('</TimeSignatureMap>', '<TimeSignatureMapSegment start="8" numerator="7" denominator="8"/></TimeSignatureMap>')
            .replace('name="Trimmed"', 'name="Trimmed" gain="0.5"'))
        p = self.read()["project"]
        self.assertEqual(p["tempos"], [[[0, "beats"], 120, False], [[8, "beats"], 80, False], [[12.5, "beats"], 240, False]])
        self.assertEqual(p["meters"], [[[0, "beats"], 4, 4], [[8, "beats"], 7, 8]])
        self.assertEqual(p["tracks"][0]["clips"][0]["gain"], .5)
        self.assertFalse(any(d["disposition"] == 3 for d in p["report"]))

    def test_audio_gain_range(self):
        for gain in (0, .125, 1, 2):
            with self.subTest(gain=gain):
                self.entries["Song/song.xml"] = SONG.replace('name="Trimmed"', f'name="Trimmed" gain="{gain}"')
                self.assertEqual(self.read()["project"]["tracks"][0]["clips"][0]["gain"], gain)
        for gain in ('-0.5', 'nan', 'inf', '1e100'):
            with self.subTest(gain=gain):
                self.entries["Song/song.xml"] = SONG.replace('name="Trimmed"', f'name="Trimmed" gain="{gain}"')
                self.read(status=1)

    def test_native_event_fade_lengths(self):
        original = SONG
        self.entries["Song/song.xml"] = original.replace(
            'name="Trimmed" start="1" length="1" offset="0.25" timeFormat="0"/>',
            'name="Trimmed" start="1" length="1" offset="0.25" timeFormat="0">'
            '<Attributes x:id="volumeCurve" fadeIn.length="0.25" fadeOut.length="0.125"/></AudioEvent>')
        project = self.read()["project"]
        self.assertEqual(project["tracks"][0]["clips"][0]["envelope"],
                         [[0, 0, True], [.25, 1, True], [.875, 1, True], [1, 0, True]])
        self.assertTrue(any(d["disposition"] == 1 and "fade lengths" in d["detail"] for d in project["report"]))
        self.assertFalse(any(d["disposition"] == 3 for d in project["report"]))
        faded = self.entries["Song/song.xml"]
        for bad in ('-0.1', '1.1', 'nan'):
            with self.subTest(length=bad):
                self.entries["Song/song.xml"] = faded.replace('fadeIn.length="0.25"', f'fadeIn.length="{bad}"')
                self.read(status=1)
        self.entries["Song/song.xml"] = faded.replace('fadeIn.length="0.25"', 'fadeIn.length="0.9"')
        self.assertTrue(any(d["disposition"] == 3 and "Overlapping" in d["detail"] for d in self.read()["project"]["report"]))
        self.entries["Song/song.xml"] = faded.replace('fadeIn.length="0.25"', 'fadeIn.length="0.25" shape="custom"')
        self.assertTrue(any(d["disposition"] == 3 and "shape" in d["detail"] for d in self.read()["project"]["report"]))

    def test_actual_studio_pro_edited_fade(self):
        sample = os.environ.get("ARDOUR_STUDIO_PRO_FADE_TEST_PROJECT")
        if not sample:
            self.skipTest("Set ARDOUR_STUDIO_PRO_FADE_TEST_PROJECT to an app-saved .song with a simple fade")
        project = self.read(path=Path(sample))["project"]
        clips = [clip for track in project["tracks"] for clip in track["clips"] if clip["name"] == "Soft pulse"]
        self.assertEqual(len(clips), 1)
        self.assertEqual(clips[0]["envelope"], [[0, 0, True], [.875, 1, True]])
        self.assertFalse(any(d["disposition"] == 3 for d in project["report"]))

    def test_invalid_transport_refused(self):
        changes = [
            ('start="0" tempo', 'start="1" tempo'),
            ('start="0" tempo', 'start="-1" tempo'),
            ('tempo="0.5"', 'tempo="0"'),
            ('tempo="0.5"', 'tempo="-1"'),
            ('tempo="0.5"', 'tempo="1e-100"'),
            ('tempo="0.5"', 'tempo="0.5" end="0"'),
            ('</TempoMap>', '<TempoMapSegment start="0" tempo="0.75"/></TempoMap>'),
            ('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/><TempoMapSegment start="4" tempo="0.5"/></TempoMap>'),
            ('start="0" numerator', 'start="1" numerator'),
            ('denominator="4"', 'denominator="3"'),
            ('</TimeSignatureMap>', '<TimeSignatureMapSegment start="0" numerator="3" denominator="4"/></TimeSignatureMap>'),
            ('</TimeSignatureMap>', '<TimeSignatureMapSegment start="8" numerator="3" denominator="4"/><TimeSignatureMapSegment start="4" numerator="4" denominator="4"/></TimeSignatureMap>'),
        ]
        for before, after in changes:
            with self.subTest(after=after):
                self.entries["Song/song.xml"] = SONG.replace(before, after)
                self.read(status=1)
        for end in (7, 9):
            self.entries["Song/song.xml"] = SONG.replace('tempo="0.5"', f'tempo="0.5" end="{end}"').replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/></TempoMap>')
            self.assertIn('gap or overlap', self.read(status=1))

    def test_mixed_unit_boundaries_use_tempo_map(self):
        # At 120 BPM for eight beats then 240 BPM, beat 32 is ten seconds.
        song = SONG.replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.25"/></TempoMap>')
        self.entries["Song/song.xml"] = song.replace('markerType="2"', 'markerType="2" start="9" timeFormat="0"')
        self.assertTrue(self.read()["has_range"])
        self.entries["Song/song.xml"] = song.replace('markerType="2"', 'markerType="2" start="11" timeFormat="0"')
        self.assertIn('boundaries', self.read(status=1))

    def test_unknown_transport_data_reported(self):
        for before, after in [('curveType="0"', 'curveType="0" swing="0.5"'),
                              ('</TempoMap>', '<UnknownTempo start="8"/></TempoMap>'),
                              ('numerator="4"', 'numerator="4" unknown="1"'),
                              ('</TimeSignatureMap>', '<UnknownMeter/></TimeSignatureMap>')]:
            self.entries["Song/song.xml"] = SONG.replace(before, after)
            self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_studio_pro_transport_editor_metadata(self):
        # Structure observed in the Studio Pro 8.0.3 native sample. The
        # normalized editor point is redundant with the resolved segment.
        self.entries["Song/song.xml"] = (SONG.replace('<TempoMapSegment', '<Attributes x:id="envelope"><EnvelopePoint value="0.282051282051282"/></Attributes><TempoMapSegment')
            .replace('numerator="4"', 'numerator="4" type="0"'))
        p = self.read()["project"]
        self.assertEqual(p["tempo"], 120)
        self.assertFalse(any(d["disposition"] == 3 for d in p["report"]))
        self.entries["Song/song.xml"] = self.entries["Song/song.xml"].replace('type="0"', 'type="1"')
        self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_tempo_follow_stretching_remains_explicit(self):
        self.entries["Song/song.xml"] = (SONG.replace('mediaType="Audio"', 'mediaType="Audio" tempoFollow="2"')
            .replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/></TempoMap>'))
        self.assertTrue(any(d["disposition"] == 3 and 'time-stretching' in d["detail"] for d in self.read()["project"]["report"]))
        self.entries["Song/song.xml"] = self.entries["Song/song.xml"].replace('tempo="0.75"', 'tempo="0.5"')
        self.assertFalse(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_empty_bipolar_automation_allowed_but_points_reported(self):
        region = '<Attributes x:id="AutomationRegionList"><AutomationRegion title="Pan"><Url x:id="points" url="media:///pan.envelopex"/></AutomationRegion></Attributes>'
        self.entries["Song/song.xml"] = SONG.replace('<SpeakerSetup', region + '<SpeakerSetup')
        self.entries["pan.envelopex"] = ubj({"bipolar": 1, "events": []})
        self.assertFalse(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))
        self.entries["pan.envelopex"] = ubj({"bipolar": 1, "events": [{"time": 0., "value": .5}]})
        self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_organizational_folders_preserve_children_in_both_directions(self):
        folder = ('<FolderTrack version="1" trackID="band" timeFormat="2" name="Band" color="FF336699">'
                  '<Attributes x:id="AutomationRegionList" visible="0"/>'
                  '<Attributes x:id="attributes" expanded="1"/></FolderTrack>')
        self.entries["Song/song.xml"] = (SONG.replace('<MediaTrack trackID="audio"', folder + '<MediaTrack trackID="audio" parentFolder="band"')
            .replace('<MediaTrack trackID="midi"', '<MediaTrack trackID="midi" parentFolder="band"'))
        project = self.read()["project"]
        folder_track, audio, midi, master = project["tracks"]
        self.assertEqual((folder_track["name"], folder_track["kind"], folder_track["color"]), ("Band", 2, "#996633"))
        self.assertEqual((audio["parent"], midi["parent"], audio["destination"]), ("band", "band", master["id"]))
        self.assertFalse(any(d["disposition"] == 3 for d in project["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            meta = {a.get("id"): a.get("value") for a in self.native_xml(archive, "metainfo.xml")}
            self.assertEqual(meta["Media:TrackCount"], "2")
            arrangement = self.native_xml(archive, "Song/song.xml")
            tracks = arrangement.find("Attributes[@id='Root']/List[@id='Tracks']")
            exported_folder = tracks.find("FolderTrack")
            self.assertIsNotNone(exported_folder)
            self.assertEqual(exported_folder.get("name"), "Band")
            self.assertEqual([t.get("parentFolder") for t in tracks.findall("MediaTrack")],
                             [exported_folder.get("trackID")] * 2)
        returned = self.read(path=output)["project"]
        self.assertEqual([t["parent"] for t in returned["tracks"][1:3]], [returned["tracks"][0]["id"]] * 2)

    def test_native_audio_buses_preserve_nested_routing_in_both_directions(self):
        # Studio Pro saves buses in AudioGroup, separate from arranger MediaTracks.
        groups = ('<ChannelGroup name="AudioGroup">'
                  '<AudioGroupChannel gain="0.8" pan="0.3" label="Drums" color="FF112233" mute="1" solo="0">'
                  '<SpeakerSetup x:id="speakerType" type="Stereo"/>'
                  '<UID x:id="uniqueID" uid="bus-drums"/>'
                  '<Connection x:id="destination" objectID="bus-stem/Input"/>'
                  '<Attributes x:id="Panner" panX1="0.3" panWidth="1"/></AudioGroupChannel>'
                  '<AudioGroupChannel gain="0.6" pan="0.5" label="Stem" mute="0" solo="1">'
                  '<SpeakerSetup x:id="speakerType" type="Stereo"/>'
                  '<UID x:id="uniqueID" uid="bus-stem"/>'
                  '<Connection x:id="destination" objectID="master/Input"/>'
                  '</AudioGroupChannel></ChannelGroup>')
        self.entries["Devices/audiomixer.xml"] = (MIXER.replace('objectID="master/Input"', 'objectID="bus-drums/Input"', 1)
            .replace('<ChannelGroup name="AudioOutput">', groups + '<ChannelGroup name="AudioOutput">'))
        before = self.read()["project"]
        audio, midi, drums, stem, master = before["tracks"]
        self.assertEqual([(t["name"], t["kind"], t["destination"]) for t in (drums, stem)],
                         [("Drums", 3, stem["id"]), ("Stem", 3, master["id"])])
        self.assertEqual((audio["destination"], drums["id"]), ("bus-drums", "bus-drums"))
        self.assertEqual((drums["volume"], drums["pan"], drums["mute"], drums["color"]),
                         (.8, .3, True, "#332211"))
        self.assertFalse(any(d["disposition"] == 3 for d in before["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            mixer = self.native_xml(archive, "Devices/audiomixer.xml")
            buses = mixer.findall(".//ChannelGroup[@name='AudioGroup']/AudioGroupChannel")
            self.assertEqual([b.get("label") for b in buses], ["Drums", "Stem"])
            uid = {b.get("label"): b.find("UID").get("uid") for b in buses}
            self.assertEqual(buses[0].find("Connection").get("objectID"), uid["Stem"] + "/Input")
            self.assertEqual(mixer.find(".//AudioTrackChannel/Connection").get("objectID"), uid["Drums"] + "/Input")
            self.assertEqual(self.native_xml(archive, "Song/song.xml").findall(".//MediaTrack")[0].get("name"), "Audio")
        after = self.read(path=output)["project"]
        by_name = {t["name"]: t for t in after["tracks"]}
        self.assertEqual(by_name["Audio"]["destination"], by_name["Drums"]["id"])
        self.assertEqual(by_name["Drums"]["destination"], by_name["Stem"]["id"])
        self.assertEqual(by_name["Stem"]["destination"], by_name["Main"]["id"])
        self.assertEqual((by_name["Drums"]["volume"], by_name["Drums"]["pan"], by_name["Drums"]["mute"]),
                         (.8, .3, True))
        self.assertFalse(any(d["disposition"] == 3 for d in after["report"]))

    def test_native_bus_dangling_and_cyclic_routes_are_refused(self):
        group = ('<ChannelGroup name="AudioGroup"><AudioGroupChannel label="Bus">'
                 '<UID x:id="uniqueID" uid="bus"/>'
                 '<Connection x:id="destination" objectID="master/Input"/>'
                 '</AudioGroupChannel></ChannelGroup>')
        mixer = MIXER.replace('<ChannelGroup name="AudioOutput">', group + '<ChannelGroup name="AudioOutput">')
        self.entries["Devices/audiomixer.xml"] = mixer.replace('objectID="master/Input"', 'objectID="missing/Input"', 1)
        self.assertIn("Unknown track reference", self.read(status=1))
        self.entries["Devices/audiomixer.xml"] = mixer.replace(
            group, group.replace('objectID="master/Input"', 'objectID="bus/Input"'))
        self.assertIn("Cyclic", self.read(status=1))

    def test_native_fx_bus_and_audio_send_round_trip(self):
        effect = ('<ChannelGroup name="AudioEffect"><AudioEffectChannel gain="0.7" pan="0.5" label="Room">'
                  '<SpeakerSetup x:id="speakerType" type="Stereo"/><UID x:id="uniqueID" uid="room-fx"/>'
                  '<Connection x:id="destination" objectID="master/Input"/>'
                  '</AudioEffectChannel></ChannelGroup>')
        rack = ('<Attributes x:id="Sends"><Attributes name="Send01" bypass="1" prefader="1" '
                'level="0.25" pan="0.8" lockPan="0">'
                '<Connection x:id="destination" objectID="room-fx/Input"/></Attributes>'
                '<Attributes x:id="Presets" pname="default" dirty="0"/></Attributes>')
        self.entries["Devices/audiomixer.xml"] = (MIXER.replace('</AudioTrackChannel>', rack + '</AudioTrackChannel>')
            .replace('<ChannelGroup name="AudioOutput">', effect + '<ChannelGroup name="AudioOutput">'))
        before = self.read()["project"]
        audio, midi, room, master = before["tracks"]
        self.assertEqual((room["name"], room["kind"], room["destination"], room["volume"]),
                         ("Room", 3, master["id"], .7))
        self.assertEqual(audio["sends"], [{"destination": room["id"], "pre_fader": True,
                                          "enabled": False, "volume": .25, "pan": .8}])
        self.assertFalse(any(d["disposition"] == 3 for d in before["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            mixer = self.native_xml(archive, "Devices/audiomixer.xml")
            exported_bus = mixer.find(".//AudioGroupChannel")
            exported_send = mixer.find(".//AudioTrackChannel/Attributes[@id='Sends']/Attributes[@name='Send01']")
            self.assertEqual(exported_bus.get("label"), "Room")
            self.assertEqual(exported_send.find("Connection").get("objectID"),
                             exported_bus.find("UID").get("uid") + "/Input")
            self.assertEqual((exported_send.get("prefader"), exported_send.get("bypass"),
                              exported_send.get("level"), exported_send.get("lockPan")),
                             ("1", "1", "0.25", "0"))
            self.assertAlmostEqual(float(exported_send.get("pan")), .8)
        after = self.read(path=output)["project"]
        self.assertEqual(after["tracks"][0]["sends"], [{**audio["sends"][0], "destination": after["tracks"][2]["id"]}])
        self.assertFalse(any(d["disposition"] == 3 for d in after["report"]))

    def test_native_send_pan_link_and_sidechain_are_reported(self):
        effect = ('<ChannelGroup name="AudioEffect"><AudioEffectChannel label="Room">'
                  '<UID x:id="uniqueID" uid="room-fx"/>'
                  '<Connection x:id="destination" objectID="master/Input"/>'
                  '</AudioEffectChannel></ChannelGroup>')
        rack = ('<Attributes x:id="Sends"><Attributes name="Send01" level="0.5" pan="0.9" lockPan="1">'
                '<Connection x:id="destination" objectID="room-fx/Input"/></Attributes>'
                '<Attributes name="Send02" level="1"><Connection x:id="destination" '
                'objectID="plugin/AudioInput01"/></Attributes></Attributes>')
        self.entries["Devices/audiomixer.xml"] = (MIXER.replace('</AudioTrackChannel>', rack + '</AudioTrackChannel>')
            .replace('<ChannelGroup name="AudioOutput">', effect + '<ChannelGroup name="AudioOutput">'))
        project = self.read()["project"]
        self.assertEqual((len(project["tracks"][0]["sends"]), project["tracks"][0]["sends"][0]["pan"]), (1, .25))
        self.assertTrue(any(d["disposition"] == 1 and "Channel-linked send pan" in d["detail"]
                            for d in project["report"]))
        self.assertTrue(any(d["disposition"] == 3 and "sidechain" in d["detail"]
                            for d in project["report"]))

    def test_missing_native_automation_member_is_reported(self):
        automation = ('<Attributes x:id="AutomationRegionList"><AutomationRegion title="Volume">'
                      '<Url x:id="points" url="media:///Envelopes/missing.envelopex"/>'
                      '</AutomationRegion></Attributes>')
        self.entries["Song/song.xml"] = SONG.replace('<List x:id="Events">', automation + '<List x:id="Events">', 1)
        project = self.read()["project"]
        self.assertTrue(any(d["disposition"] == 3 and "automation member is missing" in d["detail"]
                            for d in project["report"]))
        self.writable_fixture()
        output, error = self.export(status=1)
        self.assertIn("automation member is missing", error)
        self.assertFalse(output.exists())

    def test_native_automation_member_with_cp437_zip_name(self):
        automation = ('<Attributes x:id="AutomationRegionList"><AutomationRegion title="Volume">'
                      '<Url x:id="points" url="media:///Envelopes/Lautstärke.envelopex"/>'
                      '</AutomationRegion></Attributes>')
        self.entries["Song/song.xml"] = SONG.replace('<List x:id="Events">', automation + '<List x:id="Events">', 1)
        self.entries["Envelopes/LautstXrke.envelopex"] = ubj({"events": []})
        self.save()
        data = self.song.read_bytes()
        self.assertEqual(data.count(b"Envelopes/LautstXrke.envelopex"), 2)
        self.song.write_bytes(data.replace(b"Envelopes/LautstXrke.envelopex",
                                           b"Envelopes/Lautst\x84rke.envelopex"))
        project = self.read(path=self.song)["project"]
        self.assertFalse(any("automation member is missing" in d["detail"] for d in project["report"]))

    def test_format_seven_audio_song_converts_to_format_nine(self):
        self.entries["metainfo.xml"] = META.replace('value="9"', 'value="7"')
        before = self.read()["project"]
        self.assertEqual([t["name"] for t in before["tracks"]], ["Audio", "Notes", "Main"])
        self.assertTrue(any(d["disposition"] == 1 and "format 7" in d["detail"] for d in before["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            metadata = {a.get("id"): a.get("value") for a in self.native_xml(archive, "metainfo.xml")}
            self.assertEqual(metadata["Document:FormatVersion"], "9")
        self.assertEqual([t["name"] for t in self.read(path=output)["project"]["tracks"]],
                         ["Audio", "Notes", "Main"])

    def test_application_authored_format_seven_template(self):
        sample = os.environ.get("ARDOUR_STUDIO_ONE_FORMAT7_TEMPLATE")
        if not sample:
            self.skipTest("Pass an installed Studio One 6 format-7 .data template privately")
        project = self.read(path=Path(sample))["project"]
        by_name = {track["name"]: track for track in project["tracks"]}
        self.assertEqual(by_name["Voiceover 1"]["sends"][0]["destination"], by_name["Room"]["id"])
        self.assertEqual(by_name["Voiceover 2"]["sends"][0]["destination"], by_name["Room"]["id"])
        self.assertTrue(any(d["disposition"] == 3 and "sidechain" in d["detail"] for d in project["report"]))
        self.assertFalse(any("automation member is missing" in d["detail"] for d in project["report"]))

    def test_native_vst3_insert_reads_embedded_state_without_silent_loss(self):
        class_id = "00112233445566778899AABBCCDDEEFF"
        native_id = "{00112233-4455-6677-8899-AABBCCDDEEFF}"
        member = "Presets/Channels/Stereo/1 - Shared Effect.vstpreset"
        state = (b"VST3" + struct.pack("<I", 1) + class_id.encode() + struct.pack("<Q", 48) + b"List"
                 + struct.pack("<I", 1) + b"Comp" + struct.pack("<QQ", 48, 0))
        slot = ('<Attributes x:id="Inserts"><Attributes name="FX01">'
                '<UID x:id="uniqueID" uid="{AAAA0000-0000-0000-0000-000000000001}"/>'
                f'<UID x:id="deviceClassID" uid="{native_id}"/>'
                '<Attributes x:id="deviceData" name="Shared Effect">'
                '<UID x:id="uniqueID" uid="{AAAA0000-0000-0000-0000-000000000002}"/></Attributes>'
                '<Attributes x:id="ghostData" presetType="vstpreset">'
                f'<Attributes x:id="classInfo" classID="{native_id}" name="Shared Effect" '
                'category="AudioEffect" subCategory="VST3"/>'
                '<Attributes x:id="IO"><List x:id="audioInputs"><PortDescription label="Input"/></List>'
                '<List x:id="audioOutputs"><PortDescription label="Output"/></List></Attributes></Attributes>'
                '<Attributes x:id="Presets" pname="default" dirty="1"/>'
                f'<String x:id="presetPath" text="{member}"/></Attributes>'
                '<Attributes x:id="Presets" pname="default" dirty="0"/>'
                '<Attributes x:id="Combinator"><UID x:id="uniqueID" uid="{AAAA0000-0000-0000-0000-000000000003}"/>'
                '</Attributes></Attributes>')
        self.entries["Devices/audiomixer.xml"] = MIXER.replace('<Connection x:id="destination"', slot + '<Connection x:id="destination"')
        self.entries[member] = state
        plugin = self.read()["project"]["tracks"][0]["plugins"][0]
        self.assertEqual((plugin["id"], plugin["name"], plugin["role"], plugin["state_path"], plugin["state_size"]),
                         (class_id, "Shared Effect", "audioFX", member, len(state)))
        self.assertEqual(plugin["enabled"], True)
        self.assertFalse(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            mixer = self.native_xml(archive, "Devices/audiomixer.xml")
            slot = mixer.find(".//AudioTrackChannel/Attributes[@id='Inserts']/Attributes[@name='FX01']")
            self.assertIsNotNone(slot)
            self.assertEqual(slot.find("UID[@id='deviceClassID']").get("uid"), native_id)
            exported_member = slot.find("String[@id='presetPath']").get("text")
            self.assertEqual(archive.read(exported_member), state)
        returned = self.read(path=output)["project"]["tracks"][0]["plugins"][0]
        self.assertEqual((returned["id"], returned["name"], returned["state_size"]),
                         (class_id, "Shared Effect", len(state)))

        self.entries[member] = state[:8] + b"F" + state[9:]
        self.assertIn("class ID does not match", self.read(status=1))
        self.entries.pop(member)
        self.assertIn("Missing native VST3 preset", self.read(status=1))
        self.entries[member] = state
        self.entries["Devices/audiomixer.xml"] = self.entries["Devices/audiomixer.xml"].replace('name="FX01"', 'name="FX01" bypass="1"')
        self.assertTrue(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))

    def test_native_bus_and_master_vst3_state_round_trip(self):
        class_id = "00112233445566778899AABBCCDDEEFF"
        native_id = "{00112233-4455-6677-8899-AABBCCDDEEFF}"
        state = (b"VST3" + struct.pack("<I", 1) + class_id.encode() + struct.pack("<Q", 48) + b"List"
                 + struct.pack("<I", 1) + b"Comp" + struct.pack("<QQ", 48, 0))

        def inserts(name):
            member = f"Presets/Channels/Stereo/{name}.vstpreset"
            self.entries[member] = state
            return ('<Attributes x:id="Inserts"><Attributes name="FX01">'
                    f'<UID x:id="deviceClassID" uid="{native_id}"/>'
                    f'<Attributes x:id="deviceData" name="{name}"/>'
                    '<Attributes x:id="ghostData" presetType="vstpreset">'
                    f'<Attributes x:id="classInfo" classID="{native_id}" name="{name}" '
                    'category="AudioEffect" subCategory="VST3"/></Attributes>'
                    f'<String x:id="presetPath" text="{member}"/></Attributes>'
                    '<Attributes x:id="Presets" pname="default" dirty="0"/></Attributes>')

        bus = ('<ChannelGroup name="AudioGroup"><AudioGroupChannel label="Mix Bus" gain="1">'
               '<UID x:id="uniqueID" uid="mix-bus"/>'
               '<Connection x:id="destination" objectID="master/Input"/>'
               + inserts("Bus Effect") + '</AudioGroupChannel></ChannelGroup>')
        mixer = MIXER.replace('objectID="master/Input"', 'objectID="mix-bus/Input"', 1)
        mixer = mixer.replace('<ChannelGroup name="AudioOutput">', bus + '<ChannelGroup name="AudioOutput">')
        mixer = mixer.replace('</AudioOutputChannel>', inserts("Master Effect") + '</AudioOutputChannel>')
        self.entries["Devices/audiomixer.xml"] = mixer
        before = self.read()["project"]
        by_name = {track["name"]: track for track in before["tracks"]}
        self.assertEqual([by_name[name]["plugins"][0]["name"] for name in ("Mix Bus", "Main")],
                         ["Bus Effect", "Master Effect"])
        self.assertFalse(any(d["disposition"] == 3 for d in before["report"]))
        self.writable_fixture()
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            exported = self.native_xml(archive, "Devices/audiomixer.xml")
            for channel, name in (("AudioGroupChannel", "Bus Effect"),
                                  ("AudioOutputChannel", "Master Effect")):
                slot = exported.find(f".//{channel}/Attributes[@id='Inserts']/Attributes[@name='FX01']")
                self.assertIsNotNone(slot)
                self.assertEqual(slot.find("UID[@id='deviceClassID']").get("uid"), native_id)
                self.assertEqual(archive.read(slot.find("String[@id='presetPath']").get("text")), state)
        returned = {track["name"]: track for track in self.read(path=output)["project"]["tracks"]}
        for track_name in ("Mix Bus", "Main"):
            plugin = returned[track_name]["plugins"][0]
            self.assertEqual((plugin["id"], plugin["state_size"], plugin["enabled"]),
                             (class_id, len(state), True))

    def test_folder_content_and_unknown_parent_are_not_discarded(self):
        folder = '<FolderTrack trackID="band" name="Band"><Attributes x:id="AutomationRegionList"/></FolderTrack>'
        self.entries["Song/song.xml"] = SONG.replace('<MediaTrack trackID="audio"', folder + '<MediaTrack trackID="audio" parentFolder="band"')
        self.assertFalse(any(d["disposition"] == 3 for d in self.read()["project"]["report"]))
        self.entries["Song/song.xml"] = self.entries["Song/song.xml"].replace('parentFolder="band"', 'parentFolder="missing"')
        self.assertIn("Unknown track reference", self.read(status=1))
        self.entries["Song/song.xml"] = self.entries["Song/song.xml"].replace('parentFolder="missing"', 'parentFolder="band"')
        self.entries["Song/song.xml"] = self.entries["Song/song.xml"].replace('<Attributes x:id="AutomationRegionList"/>',
            '<Attributes x:id="AutomationRegionList"><AutomationRegion title="Volume"/></Attributes>')
        self.assertTrue(any(d["disposition"] == 3 and "Folder automation" in d["detail"]
                            for d in self.read()["project"]["report"]))

    def writable_fixture(self):
        self.entries["Performances/notes.musicx"] = ubj({"timeFormat": 2, "events": [
            {"start": 1.25, "length": .5, "pitch": 60, "velocity": .75},
            {"start": 2., "length": 1., "pitch": 67, "velocity": .5}], "envelopes": []})

    def export(self, status=0, cancel=False, published=None):
        self.save()
        output = self.root / "Export.song"
        command = [self.probe, "cancel-song" if cancel else "repack-song", str(self.song), str(output)]
        if published:
            command.append(str(published))
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, status, result.stderr)
        return output, result.stderr

    def native_xml(self, archive, name):
        # Studio Pro's on-disk schema uses literal, unbound x:id names.
        return ET.fromstring(archive.read(name).decode("utf-8-sig").replace("x:id=", "id="))

    def test_native_writer_archive_and_independent_midi(self):
        self.writable_fixture()
        self.entries["Song/song.xml"] = SONG.replace('name="Audio"', 'name="Audio &amp; \u03a9"')
        output, _ = self.export()
        with zipfile.ZipFile(output) as archive:
            self.assertIsNone(archive.testzip())
            metadata = {a.get("id"): a.get("value") for a in self.native_xml(archive, "metainfo.xml")}
            self.assertEqual(metadata["Document:FormatVersion"], "9")
            self.assertEqual(metadata["Media:TrackCount"], "2")
            root = self.native_xml(archive, "Song/song.xml").find("Attributes[@id='Root']")
            tracks = root.findall("List[@id='Tracks']/MediaTrack")
            self.assertEqual([t.get("mediaType") for t in tracks], ["Audio", "Music"])
            self.assertEqual(tracks[0].get("name"), "Audio & \u03a9")
            self.assertEqual(tracks[0].get("tempoFollow"), "0")
            mixer = self.native_xml(archive, "Devices/audiomixer.xml")
            main = mixer.find(".//AudioOutputChannel/UID").get("uid")
            audio = mixer.find(".//AudioTrackChannel")
            self.assertEqual(audio.find("Connection").get("objectID"), main + "/Input")
            self.assertEqual(tracks[0].find("UID").get("uid"), audio.find("UID").get("uid"))
            self.assertRegex(main, r"^\{[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}\}$")
            pool = self.native_xml(archive, "Song/mediapool.xml")
            info = pool.find(".//AudioClip/Attributes[@id='format']")
            self.assertEqual((info.get("bitDepth"), info.get("frameCount"), info.get("sampleRate")), ("16", "192000", "48000"))
            clip_id = tracks[0].find("List/AudioEvent").get("clipID")
            self.assertEqual(clip_id, pool.find(".//AudioClip").get("mediaID"))
            music_name = pool.find(".//MusicClip/Url").get("url").removeprefix("media:///")
            music = unpack_music(archive.read(music_name))
            self.assertEqual(music, {"timeFormat": 2, "events": [
                {"start": 1.25, "length": .5, "pitch": 60, "velocity": .75},
                {"start": 2., "length": 1., "pitch": 67, "velocity": .5}], "envelopes": []})

    def test_native_writer_roundtrip_timing_fades_and_mixer(self):
        self.writable_fixture()
        self.entries["Song/song.xml"] = (SONG.replace('</TempoMap>', '<TempoMapSegment start="8" tempo="0.75"/></TempoMap>')
            .replace('</TimeSignatureMap>', '<TimeSignatureMapSegment start="8" numerator="7" denominator="8"/></TimeSignatureMap>')
            .replace('offset="0.25" timeFormat="0"/>', 'offset="0.25" timeFormat="0" gain="0.5" mute="1"><Attributes x:id="volumeCurve" fadeIn.length="0.125" fadeOut.length="0.25"/></AudioEvent>'))
        before = self.read()
        output, _ = self.export()
        after = self.read(path=output)
        for key in ("tempos", "meters", "markers"):
            self.assertEqual(before["project"][key], after["project"][key])
        for a, b in zip(before["project"]["tracks"], after["project"]["tracks"]):
            for key in ("name", "kind", "channels", "master", "mute", "solo", "volume", "pan"):
                self.assertEqual(a[key], b[key], key)
            for left, right in zip(a["clips"], b["clips"]):
                for key in ("position", "offset", "duration", "enabled", "gain", "envelope", "notes"):
                    self.assertEqual(left[key], right[key], key)
        self.assertEqual(after["start"], [0, "seconds"])
        self.assertEqual(after["end"], [22, "seconds"])

    def test_native_writer_published_paths_and_relocation(self):
        self.writable_fixture()
        final = self.root / "Final with spaces" / "Published.song"
        output, _ = self.export(published=final)
        with zipfile.ZipFile(output) as archive:
            pool = self.native_xml(archive, "Song/mediapool.xml")
            self.assertEqual(pool.find("Attributes[@id='documentPath']").get("url"), final.as_uri())
            self.assertEqual(pool.find(".//AudioClip/Url").get("url"), (final.parent / "Media/tone.wav").as_uri())
        final.parent.mkdir()
        shutil.move(output, final)
        shutil.move(self.root / "Media", final.parent / "Media")
        after = self.read(path=final)
        self.assertEqual(len(after["project"]["tracks"]), 3)
        self.assertFalse(any(d["disposition"] == 3 for d in after["project"]["report"]))

    def test_native_writer_cancel_and_existing_destination(self):
        self.writable_fixture()
        output, error = self.export(status=1, cancel=True)
        self.assertIn("cancelled", error)
        self.assertFalse(output.exists())
        self.assertFalse(list(self.root.glob("Export.song.part-*")))
        output.write_bytes(b"keep this file")
        self.export(status=1)
        self.assertEqual(output.read_bytes(), b"keep this file")
        self.assertFalse(list(self.root.glob("Export.song.part-*")))

    def test_native_writer_unverified_note_semantics_refused(self):
        for parameter, value in (("channel", 2), ("releaseVelocity", .25)):
            self.entries["Performances/notes.musicx"] = ubj({"events": [{"pitch": 60, "length": 1, parameter: value}], "envelopes": []})
            output, error = self.export(status=1)
            self.assertIn("Per-note MIDI channels", error)
            self.assertFalse(output.exists())

    def test_native_writer_rejects_inconsistent_collected_audio(self):
        self.writable_fixture()
        self.entries["Song/mediapool.xml"] = self.entries["Song/mediapool.xml"].replace('frameCount="192000"', 'frameCount="192001"')
        output, error = self.export(status=1)
        self.assertIn("metadata", error)
        self.assertFalse(output.exists())

    def test_native_writer_real_application_source(self):
        sample = os.environ.get("ARDOUR_STUDIO_ONE_EXPORT_TEST_SONG")
        if not sample:
            self.skipTest("Actual native application source is supplied privately")
        source = Path(sample)
        copied = self.root / "Application source"
        copied.mkdir()
        shutil.copy2(source, copied / source.name)
        shutil.copytree(source.parent / "Media", copied / "Media")
        before = self.read(path=copied / source.name)
        output = copied / "Export.song"
        result = subprocess.run([self.probe, "repack-song", str(copied / source.name), str(output)], capture_output=True, text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        after = self.read(path=output)
        self.assertEqual(len(before["project"]["tracks"]), len(after["project"]["tracks"]))
        for a, b in zip(before["project"]["tracks"], after["project"]["tracks"]):
            self.assertEqual(a["name"], b["name"])
            self.assertEqual(len(a["clips"]), len(b["clips"]))
            for left, right in zip(a["clips"], b["clips"]):
                for key in ("position", "offset", "duration", "notes", "gain", "enabled"):
                    self.assertEqual(left[key], right[key], key)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args, remaining = parser.parse_known_args()
    NativeSongTest.probe = str(Path(args.probe).resolve())
    unittest.main(argv=[__file__, *remaining], verbosity=2)
