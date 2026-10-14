# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent fixtures plus project XML from an actual Studio Pro export."""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave
import xml.etree.ElementTree as ET
import zipfile


PROJECT = '''<Project version="1.0">
  <Application name="Independent fixture" version="1"/>
  <Transport><Tempo unit="bpm" value="100"/><TimeSignature numerator="7" denominator="8"/></Transport>
  <Structure>
    <Track id="folder" name="Band &amp; Friends" contentType="tracks"><Channel id="cf" audioChannels="2" destination="master"/>
      <Track id="audio" name="Stereo café" contentType="audio" color="#abcdef">
        <Channel id="ca" audioChannels="2" destination="master" solo="true">
          <Mute value="false"/><Pan unit="normalized" value="0.25"/><Volume unit="linear" value="0.8"/>
        </Channel>
      </Track>
      <Track id="midi" name="Keys" contentType="notes"><Channel id="cm" audioChannels="2" destination="master"/></Track>
    </Track>
    <Channel id="master" name="Main" role="master" audioChannels="2"/>
  </Structure>
  <Arrangement><Lanes timeUnit="beats">
    <Lanes track="audio" timeUnit="seconds"><Clips timeUnit="beats">
      <Clip name="Lead" time="8" contentTimeUnit="seconds" playStart="0.25" playStop="1.75">
        <Audio id="source" timeUnit="seconds" duration="4" channels="2" sampleRate="48000"><File path="audio/波形.wav"/></Audio>
      </Clip>
      <Clip name="Echo" time="16" duration="1.25" contentTimeUnit="seconds" playStart="1" reference="source" enable="false"/>
    </Clips></Lanes>
    <Lanes track="midi"><Clips><Clip time="4" duration="4" playStart="1" name="Notes">
      <Notes><Note time="0" duration="0.5" key="60" vel="0.7"/>
        <Note time="1" duration="0.5" key="67" channel="3" vel="0.6" rel="0.2"/>
      </Notes>
    </Clip></Clips></Lanes>
  </Lanes></Arrangement>
</Project>'''


def pcm_wave():
    data = bytes(range(256)) * 3000  # Four seconds of stereo PCM16 at 48 kHz.
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setparams((2, 2, 48000, 192000, "NONE", "not compressed"))
        wav.writeframes(data)
    return output.getvalue()


class DawProjectTest(unittest.TestCase):
    probe = None
    schema = None

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ardour-dawproject-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.input = self.root / "project.xml"

    def command(self, *args, status=0):
        result = subprocess.run([self.probe, *map(str, args)], capture_output=True,
                                text=True, encoding="utf-8", timeout=30, env=os.environ)
        self.assertEqual(result.returncode, status, result.stderr)
        return result

    def parse(self, xml=PROJECT, status=0):
        self.input.write_text(xml, encoding="utf-8")
        result = self.command("parse", self.input, status=status)
        return json.loads(result.stdout) if status == 0 else result

    def package(self, xml=PROJECT, media=True):
        path = self.root / "input.dawproject"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("project.xml", xml)
            output.writestr("metadata.xml", "<MetaData/>")
            if media:
                output.writestr("audio/波形.wav", pcm_wave())
        return path

    def assert_schema(self, path):
        if self.schema is not None:
            self.command("schema", path, self.schema)

    def semantic(self, parsed):
        result = json.loads(json.dumps(parsed))
        names = {t["id"]: t["name"] for t in result["tracks"]}
        media = {m["id"]: m["path"] for m in result["media"]}
        for source in result["media"]:
            del source["id"]
        for track in result["tracks"]:
            del track["id"]
            track["parent"] = names.get(track["parent"], "")
            track["destination"] = names.get(track["destination"], "")
            for send in track["sends"]:
                send["destination"] = names.get(send["destination"], "")
            for clip in track["clips"]:
                clip["media"] = media.get(clip["media"], "")
        del result["report"]
        return result

    def control_fixture(self, points):
        return PROJECT.replace('<Notes>', '<Lanes>' + points + '<Notes>').replace('</Notes>', '</Notes></Lanes>')

    def test_midi_controller_lanes_schema_roundtrip(self):
        lanes = ''
        for expression, extra, maximum in [('channelController', 'controller="64"', 127), ('pitchBend', '', 16383), ('channelPressure', '', 127), ('polyPressure', 'key="60"', 127)]:
            lanes += f'<Points><Target expression="{expression}" channel="3" {extra}/><IntegerPoint time="0" value="0"/><IntegerPoint time="1.25" value="{maximum}"/><IntegerPoint time="3" value="0"/></Points>'
        original = self.parse(self.control_fixture(lanes))
        self.assertEqual(original['report'], [])
        controls = original['tracks'][2]['clips'][0]['midi_controls']
        self.assertEqual([c['type'] for c in controls], [176, 224, 208, 160])
        self.assertEqual(controls[1]['points'], [[0, 0], [1.25, 16383], [3, 0]])
        output = self.root / 'controllers.xml'
        self.command('rewrite', self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(original), self.semantic(json.loads(self.command('parse', output).stdout)))

    def test_normalized_midi_and_mixed_time_units(self):
        lanes = '<Points timeUnit="seconds" unit="normalized"><Target expression="channelController" channel="0" controller="1"/><RealPoint time="0.6" value="0.5" interpolation="hold"/><RealPoint time="1.2" value="1"/></Points>'
        parsed = self.parse(self.control_fixture(lanes))
        self.assertEqual(parsed['tracks'][2]['clips'][0]['midi_controls'][0]['points'], [[1, 64], [2, 127]])
        self.assertTrue(all(d['disposition'] != 3 for d in parsed['report']))

    def test_controller_only_clip(self):
        lanes = '<Points><Target expression="channelController" channel="1" controller="64"/><IntegerPoint time="0" value="127"/><IntegerPoint time="4" value="0"/></Points>'
        for wrapper in ('<Lanes><Notes/>{}</Lanes>', '<Lanes>{}</Lanes>', '{}'):
            with self.subTest(wrapper=wrapper):
                xml = ET.fromstring(PROJECT)
                clip = xml.find('.//Clip[@name="Notes"]')
                clip.remove(clip.find('Notes'))
                clip.append(ET.fromstring(wrapper.format(lanes)))
                parsed = self.parse(ET.tostring(xml, encoding='unicode'))
                self.assertEqual(parsed['tracks'][2]['clips'][0]['notes'], [])
                self.assertEqual(len(parsed['tracks'][2]['clips'][0]['midi_controls']), 1)

    def test_midi_controller_validation_and_explicit_limits(self):
        valid = '<Points><Target expression="channelController" channel="0" controller="64"/><IntegerPoint time="0" value="127"/><IntegerPoint time="1" value="0"/></Points>'
        for before, after in [('channel="0"', 'channel="16"'), ('value="127"', 'value="128"'), ('time="1"', 'time="0"')]:
            self.parse(self.control_fixture(valid.replace(before, after)), status=1)
        self.parse(self.control_fixture(valid + valid), status=1)
        for number in (0, 6, 32, 38, 96, 101, 120, 127):
            parsed = self.parse(self.control_fixture(valid.replace('controller="64"', f'controller="{number}"')))
            self.assertTrue(any(d['disposition'] == 3 for d in parsed['report']))

    def test_midi_controller_linear_ramps(self):
        for expression, extra, maximum in [('channelController', 'controller="1"', 127), ('pitchBend', '', 16383)]:
            with self.subTest(expression=expression):
                lane = f'<Points><Target expression="{expression}" channel="0" {extra}/><IntegerPoint time="0" value="0" interpolation="linear"/><IntegerPoint time="4" value="{maximum}" interpolation="linear"/><IntegerPoint time="8" value="0"/></Points>'
                parsed = self.parse(self.control_fixture(lane))
                points = parsed['tracks'][2]['clips'][0]['midi_controls'][0]['points']
                self.assertEqual(len(points), 2 * maximum + 3)
                self.assertEqual(points[0], [0, 0])
                self.assertEqual(points[maximum + 1], [4, maximum])
                self.assertEqual(points[-1], [8, 0])
                for i in range(1, maximum + 1):
                    self.assertAlmostEqual(points[i][0], 4 * (i - .5) / maximum)
                    self.assertEqual(points[i][1], i)
                    self.assertAlmostEqual(points[maximum + 1 + i][0], 4 + 4 * (i - .5) / maximum)
                    self.assertEqual(points[maximum + 1 + i][1], maximum - i)
                self.assertTrue(any(d['disposition'] == 1 and 'half a MIDI unit' in d['detail'] for d in parsed['report']))
                self.assertFalse(any(d['disposition'] == 3 for d in parsed['report']))
                output = self.root / 'ramps.xml'
                self.command('rewrite', self.input, output)
                self.assert_schema(output)
                self.assertEqual(self.semantic(parsed), self.semantic(json.loads(self.command('parse', output).stdout)))

    def test_normalized_ramp_rounding_phase_and_hold(self):
        # Interpolate before rounding endpoints: these crossings are .01 and
        # 1.01, not .5 and 1.5. Preserve a hold followed by a descending ramp.
        lane = '<Points unit="normalized"><Target expression="polyPressure" channel="2" key="60"/>'
        for at, raw, mode in [(0, .49, 'linear'), (2, 2.49, 'hold'), (3, 3.49, 'linear'), (6, .49, 'hold')]:
            lane += f'<RealPoint time="{at}" value="{raw / 127:.17g}" interpolation="{mode}"/>'
        parsed = self.parse(self.control_fixture(lane + '</Points>'))
        points = parsed['tracks'][2]['clips'][0]['midi_controls'][0]['points']
        expected = [[0, 0], [.01, 1], [1.01, 2], [2, 2], [3, 3], [3.99, 2], [4.99, 1], [5.99, 0], [6, 0]]
        self.assertEqual(len(points), len(expected))
        for actual, want in zip(points, expected):
            self.assertAlmostEqual(actual[0], want[0])
            self.assertEqual(actual[1], want[1])
        # A descending half-integer start chooses the following value within the half-unit bound.
        lane = '<Points unit="normalized"><Target expression="channelPressure" channel="2"/>'
        lane += f'<RealPoint time="0" value="{1.5 / 127:.17g}" interpolation="linear"/><RealPoint time="1" value="0"/></Points>'
        points = self.parse(self.control_fixture(lane))['tracks'][2]['clips'][0]['midi_controls'][0]['points']
        self.assertEqual(points[0], [0, 1])
        self.assertAlmostEqual(points[1][0], 2 / 3)
        self.assertEqual(points[1][1], 0)

    def test_midi_ramp_seconds_across_tempo_change(self):
        lane = '<Points timeUnit="seconds"><Target expression="channelController" channel="0" controller="1"/><IntegerPoint time="0" value="0" interpolation="linear"/><IntegerPoint time="4" value="4"/></Points>'
        xml = self.control_fixture(lane).replace('<Tempo unit="bpm" value="100"/>', '<Tempo id="tempo-map" unit="bpm" value="60"/>')
        xml = xml.replace('time="4" duration="4" playStart="1" name="Notes"', 'time="0" duration="8" playStart="0" name="Notes"')
        xml = xml.replace('</Arrangement>', '<TempoAutomation timeUnit="beats" unit="bpm"><Target parameter="tempo-map"/><RealPoint time="2" value="120"/></TempoAutomation></Arrangement>')
        points = self.parse(xml)['tracks'][2]['clips'][0]['midi_controls'][0]['points']
        self.assertEqual(points, [[0, 0], [.5, 1], [1.5, 2], [3, 3], [5, 4], [6, 4]])

    def test_midi_ramp_expansion_limits(self):
        lane = '<Points><Target expression="pitchBend" channel="0"/>'
        lane += ''.join(f'<IntegerPoint time="{i}" value="{16383 if i % 2 else 0}" interpolation="linear"/>' for i in range(8))
        failure = self.parse(self.control_fixture(lane + '</Points>'), status=1)
        self.assertIn('controller event limit', failure.stderr)
        lane = '<Points><Target expression="pitchBend" channel="0"/><IntegerPoint time="1000000000000000" value="0" interpolation="linear"/><IntegerPoint time="1000000000000001" value="16383"/></Points>'
        failure = self.parse(self.control_fixture(lane), status=1)
        self.assertIn('floating-point time resolution', failure.stderr)

    def test_midi_controller_nested_clip_trim(self):
        lanes = '<Points><Target expression="channelController" channel="0" controller="64"/><IntegerPoint time="0" value="127"/><IntegerPoint time="3" value="0"/></Points>'
        xml = ET.fromstring(self.control_fixture(lanes))
        parent = xml.find('.//Clip[@name="Notes"]/..')
        inner = parent.find('Clip[@name="Notes"]')
        parent.remove(inner)
        outer = ET.SubElement(parent, 'Clip', time='10', duration='2', playStart='5')
        content = ET.SubElement(outer, 'Clips')
        content.append(inner)
        parsed = self.parse(ET.tostring(xml, encoding='unicode'))
        clip = parsed['tracks'][2]['clips'][0]
        self.assertEqual(clip['position'], [10, 'beats'])
        self.assertAlmostEqual(clip['duration'], 2)
        self.assertEqual(clip['offset'][1], 'beats')
        self.assertAlmostEqual(clip['offset'][0], 2)
        self.assertEqual(clip['midi_controls'][0]['points'], [[0, 127], [3, 0]])
        output = self.root / 'nested-controls.xml'
        self.command('rewrite', self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(parsed), self.semantic(json.loads(self.command('parse', output).stdout)))

    def test_independent_audio_midi_timing_and_mixer_fixture(self):
        project = self.parse()
        self.assertEqual(project["tempo"], 100)
        self.assertEqual(project["meter"], [7, 8])
        self.assertEqual(project["report"], [])
        self.assertEqual([t["name"] for t in project["tracks"]], ["Band & Friends", "Stereo café", "Keys", "Main"])
        self.assertEqual([t["kind"] for t in project["tracks"]], [2, 0, 1, 3])
        self.assertEqual(project["media"], [{"id": "source", "path": "audio/波形.wav", "channels": 2, "rate": 48000, "duration": 4}])
        track = project["tracks"][1]
        self.assertEqual((track["parent"], track["destination"]), ("folder", "master"))
        self.assertEqual((track["volume"], track["pan"], track["solo"]), (0.8, 0.25, True))
        lead, echo = track["clips"]
        self.assertEqual((lead["position"], lead["duration"], lead["offset"]), ([8, "beats"], 2.5, [0.25, "seconds"]))
        self.assertEqual((echo["position"], echo["duration"], echo["offset"], echo["enabled"]), ([16, "beats"], 1.25, [1, "seconds"], False))
        midi = project["tracks"][2]["clips"][0]
        self.assertEqual((midi["position"], midi["duration"], midi["offset"]), ([4, "beats"], 4, [1, "beats"]))
        self.assertEqual(midi["notes"], [[0, 0.5, 60, 0, 0.7, 0.5], [1, 0.5, 67, 3, 0.6, 0.2]])

    def test_xml_round_trip(self):
        before = self.parse()
        output = self.root / "rewritten.xml"
        self.command("rewrite", self.input, output)
        self.assert_schema(output)
        after = json.loads(self.command("parse", output).stdout)
        self.assertEqual(self.semantic(after), self.semantic(before))
        root = ET.parse(output).getroot()
        self.assertEqual(len(root.findall(".//Audio")), 1)
        self.assertEqual(len(root.findall(".//Clip[@reference]")), 1)
        self.assertEqual(len(root.findall(".//Note")), 2)
        self.assertEqual(root.find(".//Track[@name='Stereo café']").get("color"), "#abcdef")
        folder = root.find(".//Track[@name='Band & Friends']")
        self.assertEqual(folder.get("contentType"), "tracks")
        self.assertEqual([track.get("name") for track in folder.findall("Track")], ["Stereo café", "Keys"])
        legacy = self.parse(PROJECT.replace(' contentType="tracks"', "", 1))
        self.assertEqual(self.semantic(legacy), self.semantic(before))

    def test_pre_post_bus_sends_round_trip_and_invalid_routing(self):
        xml = PROJECT.replace('<Channel id="master"',
            '<Channel id="aux1" name="Room" role="effect" audioChannels="2" destination="master"/>'
            '<Channel id="aux2" name="Delay" role="effect" audioChannels="2" destination="master"/>'
            '<Channel id="master"').replace(
            '<Volume unit="linear" value="0.8"/>',
            '<Sends><Send destination="aux1" type="pre"><Pan unit="normalized" value="0.8"/><Volume unit="linear" value="0.25"/></Send>'
            '<Send destination="aux2" type="post"><Enable value="false"/><Volume unit="decibel" value="-6"/></Send></Sends>'
            '<Volume unit="linear" value="0.8"/>')
        parsed = self.parse(xml)
        sends = parsed['tracks'][1]['sends']
        self.assertEqual([(s['destination'], s['pre_fader'], s['enabled']) for s in sends],
                         [('aux1', True, True), ('aux2', False, False)])
        self.assertAlmostEqual(sends[0]['volume'], .25)
        self.assertAlmostEqual(sends[0]['pan'], .8)
        self.assertAlmostEqual(sends[1]['volume'], 10 ** (-6 / 20))
        output = self.root / 'sends.xml'
        self.command('rewrite', self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(json.loads(self.command('parse', output).stdout)), self.semantic(parsed))
        for bad, error in [
            ('destination="aux1" type="pre"', 'destination="master" type="pre"'),
            ('destination="aux1" type="pre"', 'destination="ca" type="pre"'),
            ('destination="aux2" type="post"', 'destination="aux1" type="post"'),
            ('destination="aux1" type="pre"', 'destination="missing" type="pre"'),
            ('value="0.25"/></Send>', 'value="-1"/></Send>'),
            ('<Pan unit="normalized" value="0.8"/>', '<Pan unit="normalized" value="1.1"/>'),
        ]:
            with self.subTest(error=error):
                self.parse(xml.replace(bad, error), status=1)
        cycle = xml.replace('<Channel id="aux1" name="Room" role="effect" audioChannels="2" destination="master"/>',
            '<Channel id="aux1" name="Room" role="effect" audioChannels="2" destination="master">'
            '<Sends><Send destination="aux2" type="pre"><Volume unit="linear" value="1"/></Send></Sends></Channel>').replace(
            '<Channel id="aux2" name="Delay" role="effect" audioChannels="2" destination="master"/>',
            '<Channel id="aux2" name="Delay" role="effect" audioChannels="2" destination="master">'
            '<Sends><Send destination="aux1" type="post"><Volume unit="linear" value="1"/></Send></Sends></Channel>')
        self.assertIn('cyclic', self.parse(cycle, status=1).stderr.lower())

    def test_official_schema(self):
        if self.schema is None:
            self.skipTest("Pass --schema with the official DAWproject Project.xsd")
        self.parse()
        output = self.root / "schema-output.xml"
        self.command("rewrite", self.input, output)
        self.command("schema", output, self.schema)

    def test_self_contained_zip_export_and_media_bytes(self):
        source = self.package()
        destination = self.root / "交換.dawproject"
        media = self.root / "extracted"
        media.mkdir()
        before = json.loads(self.command("read", source).stdout)
        self.command("repack", source, destination, media)
        source.unlink()
        for path in media.iterdir():
            path.unlink()
        after = json.loads(self.command("read", destination).stdout)
        self.assertEqual(self.semantic(after), self.semantic(before))
        with zipfile.ZipFile(destination) as output:
            self.assertIsNone(output.testzip())
            self.assertEqual(hashlib.sha256(output.read("audio/波形.wav")).digest(), hashlib.sha256(pcm_wave()).digest())
            self.input.write_bytes(output.read("project.xml"))
            self.assert_schema(self.input)

    def test_nested_clip_flattening_and_trim_offsets(self):
        nested = '''<Project version="1.0"><Application name="Fixture" version="1"/>
          <Structure><Track id="t" contentType="audio"/></Structure>
          <Arrangement><Lanes><Clips track="t"><Clip time="10" duration="8" contentTimeUnit="seconds" playStart="2" playStop="6" enable="false" name="Comp">
            <Clips timeUnit="seconds"><Clip time="1" duration="3" playStart="0.5">
              <Audio id="a" channels="1" sampleRate="48000" duration="10"><File path="a.wav"/></Audio>
            </Clip><Clip time="5" duration="3" reference="a"/></Clips>
          </Clip></Clips></Lanes></Arrangement></Project>'''
        project = self.parse(nested)
        clips = project["tracks"][0]["clips"]
        self.assertEqual(len(clips), 2)
        self.assertEqual((clips[0]["position"], clips[0]["duration"], clips[0]["offset"]), ([10, "beats"], 4, [1.5, "seconds"]))
        self.assertEqual((clips[1]["position"], clips[1]["duration"], clips[1]["offset"]), ([16, "beats"], 2, [0, "seconds"]))
        self.assertTrue(all(not c["enabled"] and c["name"] == "Comp" for c in clips))
        self.assertEqual([r["disposition"] for r in project["report"]], [1])
        output = self.root / "flat.xml"
        self.command("rewrite", self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(json.loads(self.command("parse", output).stdout)), self.semantic(project))

    def test_forward_references_and_inferred_track_kind(self):
        root = ET.fromstring(PROJECT)
        clips = root.find(".//Lanes[@track='audio']/Clips")
        first, second = list(clips)
        clips.remove(second)
        clips.insert(0, second)
        root.find(".//Track[@id='midi']").attrib.pop("contentType")
        parsed = self.parse(ET.tostring(root, encoding="unicode"))
        self.assertEqual(parsed["tracks"][1]["clips"][0]["name"], "Echo")
        self.assertEqual(len(parsed["media"]), 1)
        self.assertEqual(parsed["tracks"][2]["kind"], 1)

    def test_mixed_midi_content_units(self):
        xml = PROJECT.replace('<Notes>', '<Notes timeUnit="seconds">')
        project = self.parse(xml)
        self.assertEqual(project["tracks"][2]["clips"][0]["offset"], [0.6, "seconds"])

    def studio_pro_xml(self):
        path = Path(__file__).resolve().parents[3] / "libs/ardour/test/data/studio-pro-8.0.3-project.xml"
        return path.read_text(encoding="utf-8-sig")

    def test_actual_studio_pro_export_structure(self):
        before = self.parse(self.studio_pro_xml())
        self.assertFalse(any(d["disposition"] == 3 for d in before["report"]))
        self.assertEqual([t["name"] for t in before["tracks"]], ["Soft pulse", "Chords", "MIDI notes", "Main"])
        self.assertEqual([t["color"] for t in before["tracks"][:3]], ["#DC8855", "#55AACC", "#88BB55"])
        self.assertEqual(before["markers"], [{"name": "Start", "position": [0, "beats"]}, {"name": "End", "position": [600, "beats"]}])
        for track in before["tracks"][:2]:
            clip = track["clips"][0]
            self.assertEqual((clip["position"], clip["duration"], clip["offset"]), ([0, "beats"], 32, [0, "seconds"]))
            self.assertEqual(clip["name"], track["name"])
        notes = before["tracks"][2]["clips"][0]["notes"]
        self.assertEqual(len(notes), 32)
        for index, note in enumerate(notes):
            self.assertEqual(note, [index, .75, (60, 64, 67, 72)[index % 4], 0, .65, .5])
        output = self.root / "studio-pro-roundtrip.xml"
        self.command("rewrite", self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(before), self.semantic(json.loads(self.command("parse", output).stdout)))

    def test_nested_note_lane_time_units(self):
        xml = PROJECT.replace('<Notes>', '<Lanes timeUnit="seconds"><Lanes><Notes>').replace('</Notes>', '</Notes></Lanes></Lanes>')
        clip = self.parse(xml)["tracks"][2]["clips"][0]
        self.assertEqual(clip["offset"], [0.6, "seconds"])
        self.assertEqual(clip["notes"][1], [1, .5, 67, 3, .6, .2])
        parallel = xml.replace('</Notes>', '</Notes><Notes/>', 1)
        self.assertIn("Parallel", self.parse(parallel, status=1).stderr)

    def test_studio_pro_mixer_automation(self):
        root = ET.fromstring(self.studio_pro_xml())
        points = root.find('.//Points')
        ET.SubElement(points, 'RealPoint', time='1', value='.7')
        parsed = self.parse(ET.tostring(root, encoding='unicode'))
        self.assertFalse(any(d['disposition'] == 3 for d in parsed['report']))
        self.assertTrue(any(t['volume_automation']['points'] == [[1, .7, False]] for t in parsed['tracks']))
        self.command('rewrite', self.input, self.root / 'automation.xml')
        self.parse(self.studio_pro_xml().replace('parameter="id4"', 'parameter="missing"'), status=1)
        self.parse(self.studio_pro_xml().replace('<Target parameter="id4"/>', ''), status=1)

    def mixer_project(self, unit='linear'):
        root = ET.fromstring(PROJECT)
        volume = root.find('.//Track[@id="audio"]/Channel/Volume')
        volume.set('id', 'volume')
        pan = root.find('.//Track[@id="audio"]/Channel/Pan')
        pan.set('id', 'pan')
        lanes = root.find('Arrangement/Lanes')
        for parameter, scale, time_unit, values in (
                ('volume', unit, 'beats', [(0, -20 if unit == 'decibel' else .1, 'linear'), (4, 0 if unit == 'decibel' else 1, 'hold'), (8, -6 if unit == 'decibel' else .5, 'hold')]),
                ('pan', 'normalized', 'seconds', [(0, .25, 'hold'), (2, .75, 'linear'), (4, .5, 'hold')])):
            points = ET.SubElement(lanes, 'Points', unit=scale, timeUnit=time_unit)
            ET.SubElement(points, 'Target', parameter=parameter)
            for at, value, mode in values:
                ET.SubElement(points, 'RealPoint', time=str(at), value=str(value), interpolation=mode)
        return root

    def test_mixer_automation_roundtrip(self):
        root = self.mixer_project()
        before = self.parse(ET.tostring(root, encoding='unicode'))
        output = self.root / 'automation.xml'
        self.command('rewrite', self.input, output)
        after = self.parse(output.read_text())
        for key in ('volume_automation', 'pan_automation'):
            self.assertEqual(before['tracks'][1][key], after['tracks'][1][key])
        if self.schema:
            self.command('schema', output, self.schema)

    def test_decibel_automation_curve(self):
        parsed = self.parse(ET.tostring(self.mixer_project('decibel'), encoding='unicode'))
        points = parsed['tracks'][1]['volume_automation']['points']
        self.assertGreater(len(points), 3)
        for i in range(401):
            t = i / 100
            a, b = next((a, b) for a, b in zip(points, points[1:]) if a[0] <= t <= b[0])
            value = a[1] + (b[1] - a[1]) * (t - a[0]) / (b[0] - a[0])
            self.assertAlmostEqual(value, 10 ** ((-20 + 5 * t) / 20), delta=.000101)
        self.assertTrue(any(d['disposition'] == 1 and 'Decibel' in d['detail'] for d in parsed['report']))

    def test_invalid_mixer_automation(self):
        for attribute, value in (('time', '0'), ('time', '-1'), ('value', '-.1'), ('interpolation', 'bezier')):
            root = self.mixer_project()
            root.findall('Arrangement/Lanes/Points')[0].findall('RealPoint')[1].set(attribute, value)
            self.parse(ET.tostring(root, encoding='unicode'), status=1)
        root = self.mixer_project()
        root.findall('Arrangement/Lanes/Points')[1].find('RealPoint').set('value', '1.1')
        self.parse(ET.tostring(root, encoding='unicode'), status=1)

    def test_markers_and_rgba_color_validation(self):
        self.parse(self.studio_pro_xml().replace('time="600.000000"', 'time="-1"'), status=1)
        self.parse(self.studio_pro_xml().replace('time="600.000000"', 'time="NaN"'), status=1)
        self.parse(self.studio_pro_xml().replace('#DC8855FF', 'invalid'), status=1)
        parsed = self.parse(self.studio_pro_xml().replace('#DC8855FF', '#DC885580'))
        self.assertTrue(any(d['disposition'] == 3 and 'transparency' in d['detail'] for d in parsed['report']))
        xml = self.studio_pro_xml().replace('<Marker time="600.000000"', '<Marker vendorExtension="unmapped" time="600.000000"')
        self.assertTrue(any(d['disposition'] == 3 for d in self.parse(xml)['report']))
        parsed = self.parse(self.studio_pro_xml().replace('<Markers id="id15" timeUnit="beats">', '<Markers id="id15" timeUnit="seconds">'))
        self.assertEqual(parsed['markers'][1]['position'], [600, 'seconds'])

    def test_whitespace_and_decibel_gain(self):
        xml = PROJECT.replace('value="100"', 'value=" 100 "').replace('unit="linear" value="0.8"', 'unit="decibel" value="-6.020599913279624"')
        self.assertAlmostEqual(self.parse(xml)["tracks"][1]["volume"], 0.5, places=14)

    def test_feedback_routing_is_rejected(self):
        xml = PROJECT.replace('id="ca" audioChannels="2" destination="master"', 'id="ca" audioChannels="2" destination="cm"')
        xml = xml.replace('id="cm" audioChannels="2" destination="master"', 'id="cm" audioChannels="2" destination="ca"')
        self.assertIn("Cyclic", self.parse(xml, status=1).stderr)

    def test_reference_expansion_is_bounded(self):
        root = ET.fromstring('<Project version="1.0"><Application name="Expansion" version="1"/><Structure><Track id="t" contentType="audio"/></Structure><Arrangement><Lanes><Clips track="t"><Clip time="0" duration="1" reference="level19"/></Clips></Lanes></Arrangement><Scenes/></Project>')
        scenes = root.find("Scenes")
        for level in range(20):
            timeline = ET.SubElement(ET.SubElement(ET.SubElement(scenes, "Scene"), "Lanes"), "Clips", id=f"level{level}")
            if level == 0:
                clip = ET.SubElement(timeline, "Clip", time="0", duration="1", contentTimeUnit="seconds")
                audio = ET.SubElement(clip, "Audio", id="a", duration="1", channels="1", sampleRate="48000")
                ET.SubElement(audio, "File", path="a.wav")
            else:
                for _ in range(2):
                    ET.SubElement(timeline, "Clip", time="0", duration="1", reference=f"level{level-1}")
        result = self.parse(ET.tostring(root, encoding="unicode"), status=1)
        self.assertIn("limit", result.stderr)
        root = ET.fromstring(PROJECT)
        arrangement = root.find("Arrangement")
        arrangement.clear()
        clips = ET.SubElement(ET.SubElement(arrangement, "Lanes"), "Clips", track="midi")
        notes = ET.SubElement(ET.SubElement(clips, "Clip", time="0", duration="1"), "Notes", id="manyNotes")
        for _ in range(500):
            ET.SubElement(notes, "Note", time="0", duration="0.5", key="60")
        for index in range(600):
            ET.SubElement(clips, "Clip", time=str(index + 1), duration="1", reference="manyNotes")
        result = self.parse(ET.tostring(root, encoding="unicode"), status=1)
        self.assertIn("expanded object limit", result.stderr)

    def test_report_prevents_unacknowledged_feature_loss(self):
        changes = [('<Mute value="false"/>', '<Devices><Vst3Plugin deviceName="Effect" deviceRole="audioFX"/></Devices><Mute value="false"/>'),
                   ('<Notes>', '<Notes vendorExtension="yes">'),
                   ('<Channel id="ca"', '<Channel name="Separate mixer label" id="ca"'),
                   ('role="master"', 'role="regular"')]
        for old, new in changes:
            with self.subTest(feature=new):
                project = self.parse(PROJECT.replace(old, new))
                self.assertTrue(any(r["disposition"] == 3 for r in project["report"]))
                target = self.root / "lost.xml"
                self.command("rewrite", self.input, target, status=1)
                self.assertFalse(target.exists())

    def test_vst3_state_archive_round_trip_and_rejection(self):
        class_id = "00112233445566778899AABBCCDDEEFF"
        uuid = "00112233-4455-6677-8899-AABBCCDDEEFF"
        path = "plugins/effect.vstpreset"
        state = (b"VST3" + struct.pack("<I", 1) + class_id.encode("ascii") + struct.pack("<Q", 48) + b"List"
                 + struct.pack("<I", 1) + b"Comp" + struct.pack("<QQ", 48, 0))
        device = ('<Devices><Vst3Plugin deviceID="%s" deviceName="Shared Effect" deviceRole="audioFX">'
                  '<Enabled value="false"/><State path="%s"/></Vst3Plugin></Devices>') % (class_id.lower(), path)
        xml = PROJECT.replace('<Mute value="false"/>', device + '<Mute value="false"/>', 1)
        parsed = self.parse(xml)
        self.assertEqual(parsed["report"], [])
        self.assertEqual(parsed["tracks"][1]["plugins"], [{"id": class_id, "name": "Shared Effect", "role": "audioFX",
                                                         "state_path": path, "state_size": 0, "enabled": False}])
        hyphenated = self.parse(xml.replace(class_id.lower(), uuid.lower()))
        self.assertEqual(self.semantic(hyphenated), self.semantic(parsed))
        rewritten = self.root / "plugin.xml"
        self.command("rewrite", self.input, rewritten)
        self.assert_schema(rewritten)
        self.assertEqual(ET.parse(rewritten).find(".//Vst3Plugin").get("deviceID"), class_id)
        self.assertEqual(self.semantic(json.loads(self.command("parse", rewritten).stdout)), self.semantic(parsed))

        source = self.package(xml)
        with zipfile.ZipFile(source, "a") as archive:
            archive.writestr(path, state)
        loaded = json.loads(self.command("read", source).stdout)
        self.assertEqual(loaded["tracks"][1]["plugins"][0]["state_size"], len(state))
        output = self.root / "plugin-output.dawproject"
        self.command("repack", source, output, self.root)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(ET.fromstring(archive.read("project.xml")).find(".//Vst3Plugin").get("deviceID"), class_id)
            self.assertEqual(archive.read(path), state)
            self.assert_schema(self.root / "plugin.xml")
        returned = json.loads(self.command("read", output).stdout)
        self.assertEqual(returned["tracks"][1]["plugins"][0]["id"], class_id)

        for bad in [state.replace(class_id.encode("ascii"), b"F" * 32), b"wrong" + state[5:],
                    state[:-4] + struct.pack("<I", 1), state.replace(b"Comp", b"Prog"), state.replace(b"Comp", b"Info"),
                    state[:48] + b"List" + struct.pack("<I", 0), state[:4] + struct.pack("<I", 2) + state[8:]]:
            broken = self.root / "broken.dawproject"
            with zipfile.ZipFile(broken, "w") as archive:
                archive.writestr("project.xml", xml)
                archive.writestr(path, bad)
            self.command("read", broken, status=1)
        missing = self.package(xml)
        self.command("read", missing, status=1)
        for changed in [xml.replace('<State path=', '<State external="true" path='),
                        xml.replace('<Enabled value="false"/>', '<Parameters><RealParameter parameterID="1" unit="normalized" value="0.4"/></Parameters><Enabled value="false"/>')]:
            self.assertTrue(any(item["disposition"] == 3 for item in self.parse(changed)["report"]))
        self.parse(xml.replace(class_id.lower(), "invalid-id"), status=1)

    def test_reject_malformed_and_out_of_range_values(self):
        changes = [('version="1.0"', 'version="2.0"'), ('value="100"', 'value="NaN"'),
                   ('value="100"', 'value="0"'), ('denominator="8"', 'denominator="3"'),
                   ('time="8"', 'time="INF"'), ('playStart="0.25"', 'playStart="-1"'),
                   ('duration="1.25"', 'duration="0"'), ('playStart="1" reference', 'playStart="4" reference'),
                   ('channels="2" sampleRate', 'channels="0" sampleRate'), ('channel="3"', 'channel="16"'),
                   ('vel="0.6"', 'vel="1.01"'), ('key="67"', 'key="128"'), ('id="midi"', 'id="audio"'),
                   ('track="audio"', 'track="missing"'), ('reference="source"', 'reference="missing"'),
                   ('enable="false"', 'enable="maybe"'), ('contentTimeUnit="seconds"', 'contentTimeUnit="samples"'),
                   ('<Clip name="Lead"', '<Clip loopStart="0" loopEnd="1" name="Lead"'),
                   ('<Clip name="Lead"', '<Clip reference="source" name="Lead"'),
                   ('<Project version', '<Project xmlns="urn:unknown" version')]
        for old, new in changes:
            with self.subTest(change=new):
                self.parse(PROJECT.replace(old, new, 1), status=1)

    def test_reject_tempo_changes_without_target(self):
        xml = PROJECT.replace('</Arrangement>', '<TempoAutomation><RealPoint time="4" value="80"/></TempoAutomation></Arrangement>')
        result = self.parse(xml, status=1)
        self.assertIn("Tempo", result.stderr)

    def timed_project(self, points, unit="beats", meters=""):
        xml = PROJECT.replace('<Tempo ', '<Tempo id="tempo-map" ', 1)
        xml = xml.replace('<TimeSignature ', '<TimeSignature id="meter-map" ', 1)
        tempo = '<TempoAutomation timeUnit="%s" unit="bpm"><Target parameter="tempo-map"/>%s</TempoAutomation>' % (unit, points)
        meter = '<TimeSignatureAutomation timeUnit="beats"><Target parameter="meter-map"/>%s</TimeSignatureAutomation>' % meters
        return xml.replace('</Arrangement>', tempo + meter + '</Arrangement>')

    def test_variable_tempo_and_meter_roundtrip(self):
        xml = self.timed_project('<RealPoint time="9" value="60"/><RealPoint time="20" value="140"/>',
                                 meters='<TimeSignaturePoint time="10" numerator="7" denominator="8"/>')
        parsed = self.parse(xml)
        self.assertAlmostEqual(parsed["tracks"][1]["clips"][0]["duration"], 1.9)
        self.assertEqual(parsed["tempos"], [[[9, "beats"], 60, False], [[20, "beats"], 140, False]])
        self.assertEqual(parsed["meters"], [[[10, "beats"], 7, 8]])
        output = self.root / "rewritten.xml"
        self.command("rewrite", self.input, output)
        self.assert_schema(output)
        self.assertEqual(self.semantic(parsed), self.semantic(json.loads(self.command("parse", output).stdout)))

    def test_tempo_ramps_and_step_conversion(self):
        cases = [
            ("beats", '<RealPoint time="0" value="60" interpolation="linear"/><RealPoint time="8" value="180"/>',
             lambda q: 4 * math.log1p(q / 4) if q < 8 else 4 * math.log(3) + (q - 8) / 3),
            ("seconds", '<RealPoint time="0" value="60" interpolation="linear"/><RealPoint time="4" value="180"/>',
             lambda q: 2 * (math.sqrt(1 + q) - 1) if q < 8 else 4 + (q - 8) / 3),
            ("seconds", '<RealPoint time="0" value="180" interpolation="linear"/><RealPoint time="4" value="60"/>',
             lambda q: 2 * (3 - math.sqrt(9 - q)) if q < 8 else 4 + (q - 8)),
            ("beats", '<RealPoint time="0" value="60" interpolation="linear"/><RealPoint time="8" value="180"/><RealPoint time="8" value="90"/>',
             lambda q: 4 * math.log1p(q / 4) if q < 8 else 4 * math.log(3) + (q - 8) / 1.5),
        ]
        for unit, points, expected in cases:
            with self.subTest(unit=unit, points=points):
                self.parse(self.timed_project(points, unit))
                data = json.loads(self.command("timing", self.input).stdout)
                for beat, second, back, approximate in data:
                    self.assertAlmostEqual(second, expected(beat), places=11)
                    self.assertAlmostEqual(back, beat, places=10)
                    self.assertLessEqual(abs(approximate - second), .000010001)
                output = self.root / "ramp.xml"
                self.command("rewrite", self.input, output)
                self.assert_schema(output)

    def test_variable_tempo_nested_clip_ranges(self):
        xml = self.timed_project('<RealPoint time="0" value="60"/><RealPoint time="4" value="120"/>')
        # Use one explicit nested viewport; its content starts before the outer
        # audible range and must be trimmed at the root tempo-map position.
        root = ET.fromstring(xml)
        arrangement = root.find('Arrangement')
        arrangement.remove(arrangement.find('Lanes'))
        arrangement.insert(0, ET.fromstring('''<Lanes><Clips track="audio" timeUnit="beats"><Clip time="8" duration="4" playStart="2">
          <Clips timeUnit="seconds"><Clip time="0.25" duration="3" playStart="0.5" contentTimeUnit="seconds">
          <Audio duration="4" channels="2" sampleRate="48000"><File path="audio/nested.wav"/></Audio>
          </Clip></Clips></Clip></Clips></Lanes>'''))
        clip = self.parse(ET.tostring(root, encoding='unicode'))['tracks'][1]['clips'][0]
        self.assertAlmostEqual(clip['position'][0], 8)
        self.assertAlmostEqual(clip['offset'][0], 1.25)
        self.assertAlmostEqual(clip['duration'], 4)

    def test_invalid_transport_automation(self):
        for points in ('<RealPoint time="0" value="0"/>', '<RealPoint time="4" value="120"/><RealPoint time="3" value="90"/>',
                       '<RealPoint time="0" value="120" interpolation="curve"/>'):
            self.parse(self.timed_project(points), status=1)
        self.parse(self.timed_project('<RealPoint time="4" value="100"/>').replace('parameter="tempo-map"', 'parameter="meter-map"'), status=1)
        self.parse(self.timed_project('', meters='<TimeSignaturePoint time="2" numerator="3" denominator="3"/>'), status=1)

    @staticmethod
    def envelope_value(points, time):
        for a, b in zip(points, points[1:]):
            if time < b[0]:
                return a[1] if not a[2] else a[1] + (b[1] - a[1]) * (time - a[0]) / (b[0] - a[0])
        return points[-1][1]

    def test_clip_gain_and_overlapping_fades(self):
        xml = PROJECT.replace('name="Lead" time="8"', 'name="Lead" fadeInTime="0.9" fadeOutTime="0.9" fadeTimeUnit="seconds" time="8"')
        gain = '<Points unit="linear"><Target expression="gain"/><RealPoint time="0" value="0.5" interpolation="linear"/><RealPoint time="2" value="1.5" interpolation="linear"/></Points>'
        xml = xml.replace('<Audio id="source"', '<Lanes timeUnit="seconds">' + gain + '<Audio id="source"').replace('</Audio>', '</Audio></Lanes>', 1)
        parsed = self.parse(xml)
        self.assertFalse(any(d['disposition'] == 3 for d in parsed['report']))
        clip = parsed['tracks'][1]['clips'][0]
        for i in range(151):
            t = i / 100
            expected = (.5 + (.25 + t) / 2) * min(1, t / .9) * min(1, (1.5 - t) / .9)
            self.assertAlmostEqual(self.envelope_value(clip['envelope'], t), expected, delta=.00008)
        output = self.root / 'envelope.xml'
        self.command('rewrite', self.input, output)
        self.assert_schema(output)
        again = json.loads(self.command('parse', output).stdout)['tracks'][1]['clips'][0]
        for i in range(151):
            self.assertAlmostEqual(self.envelope_value(clip['envelope'], i / 100), self.envelope_value(again['envelope'], i / 100), delta=.000001)
        self.assertIsNotNone(ET.parse(output).find('.//Points/Target[@expression="gain"]'))

    def test_crossfade_source_handle_and_invalid_fades(self):
        xml = PROJECT.replace('name="Lead" time="8"', 'name="Lead" fadeInTime="-0.1" fadeOutTime="0.2" fadeTimeUnit="seconds" time="8"')
        clip = self.parse(xml)['tracks'][1]['clips'][0]
        self.assertAlmostEqual(clip['position'][0] * .6, 4.7)
        self.assertAlmostEqual(clip['offset'][0], .15)
        self.assertAlmostEqual(clip['duration'] * .6, 1.6)
        self.assertAlmostEqual(self.envelope_value(clip['envelope'], .05), .5)
        for change in ('fadeInTime="-0.3"', 'fadeInTime="2"', 'fadeInTime="nan"'):
            self.parse(xml.replace('fadeInTime="-0.1"', change), status=1)
        self.parse(xml.replace('fadeOutTime="0.2"', 'fadeOutTime="-1"'), status=1)
        self.parse(xml.replace(' fadeTimeUnit="seconds"', ''), status=1)

    def test_nested_clip_gain_trim(self):
        xml = '''<Project version="1.0"><Application name="Test" version="1"/><Structure><Track id="t" contentType="audio"/></Structure>
          <Arrangement><Lanes timeUnit="seconds"><Clips track="t"><Clip time="2" duration="1" playStart="1">
          <Lanes><Points unit="linear"><Target expression="gain"/><RealPoint time="0" value="0.5"/></Points>
          <Clips><Clip time="0" duration="3"><Lanes><Points unit="linear"><Target expression="gain"/>
          <RealPoint time="0" value="0" interpolation="linear"/><RealPoint time="3" value="1"/></Points>
          <Audio channels="2" sampleRate="48000" duration="4"><File path="audio.wav"/></Audio>
          </Lanes></Clip></Clips></Lanes></Clip></Clips></Lanes></Arrangement></Project>'''
        clip = self.parse(xml)['tracks'][0]['clips'][0]
        self.assertEqual(clip['position'], [2, 'seconds'])
        self.assertEqual(clip['offset'], [1, 'seconds'])
        for t in (0, .25, .5, 1):
            self.assertAlmostEqual(self.envelope_value(clip['envelope'], t), (1 + t) / 6, delta=.00001)

    def test_beat_fades_across_tempo_change(self):
        xml = self.timed_project('<RealPoint time="9" value="60"/>')
        xml = xml.replace('name="Lead" time="8"', 'name="Lead" fadeTimeUnit="beats" fadeInTime="1.5" fadeOutTime="0.8" time="8"')
        clip = self.parse(xml)['tracks'][1]['clips'][0]
        for i in range(151):
            time = i / 100
            beat = 8 + time / .6 if time <= .6 else 9 + time - .6
            expected = min(1, (beat - 8) / 1.5) * min(1, (9.9 - beat) / .8)
            self.assertAlmostEqual(self.envelope_value(clip['envelope'], time), expected, delta=.00015)

    def test_reject_dtd_entities_and_reference_cycles(self):
        secret = self.root / "private.txt"
        secret.write_text("MUST_NOT_APPEAR", encoding="utf-8")
        for declaration in [f'<!DOCTYPE Project SYSTEM "{secret.as_uri()}">',
                            '<!DOCTYPE Project [<!ENTITY internal "MUST_NOT_APPEAR">]>',
                            f'<!DOCTYPE Project [<!ENTITY external SYSTEM "{secret.as_uri()}">]>']:
            with self.subTest(declaration=declaration):
                result = self.parse(declaration + PROJECT, status=1)
                self.assertNotIn("MUST_NOT_APPEAR", result.stdout + result.stderr)
        cycle = '''<Project version="1.0"><Application name="Cycle" version="1"/><Structure><Track id="t" contentType="audio"/></Structure>
          <Arrangement><Lanes><Clips id="cycle" track="t"><Clip time="0" duration="1" reference="cycle"/></Clips></Lanes></Arrangement></Project>'''
        self.assertIn("Cyclic", self.parse(cycle, status=1).stderr)

    def test_missing_external_media_and_invalid_package(self):
        self.command("read", self.package(media=False), status=1)
        self.command("read", self.package(PROJECT.replace('path="audio/波形.wav"', 'path="audio/波形.wav" external="true"')), status=1)
        self.command("read", self.package(PROJECT.replace('path="audio/波形.wav"', 'path="../outside.wav"')), status=1)
        empty = self.root / "empty.dawproject"
        with zipfile.ZipFile(empty, "w"):
            pass
        self.command("read", empty, status=1)

    def test_invalid_or_deep_xml(self):
        for xml in ["", "not XML", "<Project>", "<Different/>", '<Project version="1.0"/>',
                    '<Project version="1.0">' + '<Lanes>' * 130 + '</Lanes>' * 130 + '</Project>']:
            self.parse(xml, status=1)
        self.parse(PROJECT.replace('<Volume unit="linear" value="0.8"/>', '<Volume unit="linear" value="0.8"><Unknown/></Volume>'), status=1)
        self.parse(PROJECT.replace('<Arrangement>', '<Arrangement>unexpected text'), status=1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--schema", type=Path, help="Optional official Project.xsd for independent output validation")
    args = parser.parse_args()
    DawProjectTest.probe = str(args.probe.resolve())
    DawProjectTest.schema = str(args.schema.resolve()) if args.schema else None
    unittest.main(argv=[__file__], verbosity=2)
