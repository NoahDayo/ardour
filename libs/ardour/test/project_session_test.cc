/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <thread>
#ifdef PLATFORM_WINDOWS
#include <winsock2.h>
#include <windows.h>
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <cppunit/extensions/HelperMacros.h>
#include <sndfile.h>

#include "ardour/audioengine.h"
#include "ardour/audio_port.h"
#include "ardour/io.h"
#include "ardour/audioregion.h"
#include "ardour/gain_control.h"
#include "ardour/export_status.h"
#include "ardour/internal_send.h"
#include "ardour/automation_list.h"
#include "pbd/control_math.h"
#include "ardour/midi_model.h"
#include "ardour/midi_source.h"
#include "ardour/midi_region.h"
#include "ardour/location.h"
#include "ardour/logic_project.h"
#include "ardour/luaproc.h"
#include "ardour/export_handler.h"
#include "temporal/tempo.h"
#include "ardour/playlist.h"
#include "ardour/panner_shell.h"
#include "ardour/pannable.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/project_session.h"
#include "ardour/studio_one_song.h"
#include "ardour/recent_sessions.h"
#include "ardour/rc_configuration.h"
#include "ardour/session.h"
#include "ardour/source.h"
#include "ardour/sndfileimportable.h"
#include "ardour/track.h"
#include "test_util.h"

using namespace ARDOUR;
namespace PX = ARDOUR::ProjectExchange;

namespace {

struct Fixture {
	Fixture () : root (new_test_output_dir ("project-exchange")) { create_and_start_dummy_backend (); }
	~Fixture () { AudioEngine::instance ()->remove_session (); session.reset (); stop_and_destroy_backend (); }
	std::string path (const std::string& name) const { return Glib::build_filename (root, name); }
	std::string root;
	std::unique_ptr<Session> session;
	std::vector<PX::Diagnostic> report;
};

PX::Project example (Fixture& fixture)
{
	PX::Project project;
	project.tempo = 100;
	project.numerator = 3;
	project.denominator = 4;
	PX::Media media;
	media.id = "source";
	media.path = "audio/stereo.wav";
	media.sample_rate = AudioEngine::instance ()->sample_rate ();
	media.channels = 2;
	media.duration = 2;
	project.media.push_back (media);
	SF_INFO info = {};
	info.channels = 2;
	info.samplerate = media.sample_rate;
	info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	SNDFILE* output = sf_open (fixture.path ("source.wav").c_str (), SFM_WRITE, &info);
	CPPUNIT_ASSERT (output);
	std::vector<float> pcm (media.sample_rate * 4);
	for (size_t i = 0; i < pcm.size () / 2; ++i) {
		pcm[2 * i] = float (int (i % 37) - 18) / 32;
		pcm[2 * i + 1] = float (int (i % 61) - 30) / 64;
	}
	CPPUNIT_ASSERT_EQUAL (sf_count_t (media.sample_rate * 2), sf_writef_float (output, pcm.data (), media.sample_rate * 2));
	CPPUNIT_ASSERT_EQUAL (0, sf_close (output));
	PX::Track master;
	master.id = "master";
	master.name = "Main";
	master.kind = PX::TrackKind::Bus;
	master.master = true;
	project.tracks.push_back (master);
	PX::Track folder;
	folder.id = "folder";
	folder.name = "Band";
	folder.kind = PX::TrackKind::Folder;
	folder.destination = "master";
	project.tracks.push_back (folder);
	PX::Track audio;
	audio.id = "audio";
	audio.name = "Stereo";
	audio.color = "#12ab34";
	audio.parent = "folder";
	audio.destination = "folder";
	audio.volume = 0.75;
	audio.pan = 0.3;
	PX::Clip clip;
	clip.name = "Offset audio";
	clip.color = "#abcdef";
	clip.media = "source";
	clip.position = PX::Time (1.25, PX::TimeUnit::Seconds);
	clip.duration = 0.5;
	clip.offset = PX::Time (0.375, PX::TimeUnit::Seconds);
	audio.clips.push_back (clip);
	clip.name = "Muted audio";
	clip.position.value = 2.5;
	clip.enabled = false;
	audio.clips.push_back (clip);
	project.tracks.push_back (audio);
	PX::Track midi;
	midi.id = "midi";
	midi.name = "Notes";
	midi.kind = PX::TrackKind::MIDI;
	midi.parent = "folder";
	midi.destination = "master";
	clip = PX::Clip ();
	clip.name = "Offset MIDI";
	clip.kind = PX::ClipKind::MIDI;
	clip.position = PX::Time (4, PX::TimeUnit::Beats);
	clip.duration = 4;
	clip.offset = PX::Time (1, PX::TimeUnit::Beats);
	PX::Note note;
	note.time = 1.25;
	note.duration = 1.5;
	note.velocity = 93.0 / 127;
	note.release_velocity = 45.0 / 127;
	note.channel = 2;
	clip.notes.push_back (note);
	note.time = 3;
	note.key = 67;
	note.channel = 3;
	clip.notes.push_back (note);
	midi.clips.push_back (clip);
	project.tracks.push_back (midi);
	return project;
}

void package (Fixture& fixture, const PX::Project& project, const std::string& name = "input.dawproject")
{
	PX::DawProject::write (project, fixture.path (name), {{"source", fixture.path ("source.wav")}});
}

void native_package (Fixture& fixture, bool processing = false, bool variable = false, bool fades = false, unsigned format_version = 9)
{
	auto project = example (fixture);
	auto uri = [] (const std::string& path) {
		gchar* raw = g_filename_to_uri (path.c_str (), NULL, NULL);
		CPPUNIT_ASSERT (raw);
		std::string result (raw); g_free (raw); return result;
	};
	PX::ProjectArchiveWriter song (fixture.path ("input.song"));
	song.add_text ("metainfo.xml", std::string ("<MetaInformation><Attribute id=\"Document:MimeType\" value=\"application/x.presonus-song\"/><Attribute id=\"Document:FormatVersion\" value=\"") + std::to_string (format_version) + "\"/></MetaInformation>");
	std::string transport = R"(
<TempoMap x:id="tempoMap"><TempoMapSegment start="0" tempo="0.6"/></TempoMap>
<TimeSignatureMap x:id="timeSignatureMap"><TimeSignatureMapSegment start="0" numerator="3" denominator="4"/></TimeSignatureMap>
)", events = std::string (R"(<AudioEvent clipID="media" start="1.25" length="0.5" offset="0.375" name="Native clip" speed=")") + (processing ? "2" : "1") + R"("/>)";
	if (variable) {
		transport = R"(<TempoMap x:id="tempoMap"><TempoMapSegment start="0" end="3" tempo="0.6"/><TempoMapSegment start="3" end="6" tempo="0.5"/><TempoMapSegment start="6" tempo="0.25"/></TempoMap>
<TimeSignatureMap x:id="timeSignatureMap"><TimeSignatureMapSegment start="0" numerator="3" denominator="4"/><TimeSignatureMapSegment start="3" numerator="5" denominator="8"/><TimeSignatureMapSegment start="5.5" numerator="4" denominator="4"/></TimeSignatureMap>)";
		events = R"(<AudioEvent clipID="media" start="5" length="2" offset="0.375" timeFormat="2" gain="0.5" name="Quiet"/>
<AudioEvent clipID="media" start="8" length="1" offset="0.375" timeFormat="2" gain="2" name="Loud"/>
<AudioEvent clipID="media" start="10" length="1" offset="0.375" timeFormat="2" gain="0" name="Silent"/>)";
	}
	if (fades) {
		events = R"(<AudioEvent clipID="media" start="1.25" length="0.5" offset="0.375" timeFormat="0" name="Faded clip"><Attributes x:id="volumeCurve" fadeIn.length="0.125" fadeOut.length="0.125"/></AudioEvent>)";
	}
	song.add_text ("Song/song.xml", std::string (R"(<Song><Attributes x:id="Root"><Attributes x:id="timeContext">)") + transport +
	               R"(</Attributes><List x:id="Tracks"><MarkerTrack timeFormat="2"><MarkerEvent markerType="2")" +
	               (variable ? R"( start="2.3" timeFormat="0")" : "") + R"(/><MarkerEvent markerType="3" start="12"/><MarkerEvent name="Verse" start="8"/></MarkerTrack>
<MediaTrack trackID="audio" mediaType="Audio" name="Native audio" timeFormat="0" color="FF34AB12"><UID x:id="channelID" uid="channel"/>
<List x:id="Events">)" + events + R"(</List></MediaTrack></List></Attributes></Song>)");
	song.add_text ("Song/mediapool.xml", "<MediaPool><Attributes x:id=\"documentPath\" url=\"" + uri (fixture.path ("input.song")) + "\"/><AudioClip mediaID=\"media\"><Url x:id=\"path\" url=\"" + uri (fixture.path ("source.wav")) + "\"/><Attributes x:id=\"format\" sampleRate=\"" + std::to_string (project.media[0].sample_rate) + "\" numChannels=\"2\" frameCount=\"" + std::to_string (project.media[0].sample_rate * 2) + "\"/></AudioClip></MediaPool>");
	song.add_text ("Devices/audiomixer.xml", std::string (R"(<AudioMixer><AudioTrackChannel gain="0.75" pan=")") + (variable || fades ? "0.5" : "0.3") + R"("><UID x:id="uniqueID" uid="channel"/><Connection x:id="destination" objectID="master/Input"/></AudioTrackChannel><AudioOutputChannel label="Main"><UID x:id="uniqueID" uid="master"/></AudioOutputChannel></AudioMixer>)");
	song.finish ();
}

const PX::Track& track_named (const PX::Project& project, const std::string& name)
{
	for (auto const& track : project.tracks) { if (track.name == name) { return track; } }
	throw std::runtime_error ("Missing exported track: " + name);
}

size_t stages (const std::string& root)
{
	size_t count = 0;
	Glib::Dir dir (root);
	for (auto const& name : dir) { if (name.find (".ardour-exchange-") == 0) { ++count; } }
	return count;
}

double curve_value (const PX::Automation& curve, double at)
{
	CPPUNIT_ASSERT (!curve.points.empty ());
	for (size_t i = 1; i < curve.points.size (); ++i) {
		if (at < curve.points[i].time) {
			auto const& a = curve.points[i - 1]; auto const& b = curve.points[i];
			if (at <= a.time || !a.linear) { return a.value; }
			return a.value + (b.value - a.value) * (at - a.time) / (b.time - a.time);
		}
	}
	return curve.points.back ().value;
}

template <typename Predicate>
void wait_render_state (Predicate ready)
{
	const gint64 deadline = g_get_monotonic_time () + 10000000;
	while (!ready () && g_get_monotonic_time () < deadline) { g_usleep (1000); }
	CPPUNIT_ASSERT_MESSAGE ("Transport or offline render did not settle", ready ());
}

std::string xml_text (const XMLNode& node)
{
	XMLTree tree;
	tree.set_root (new XMLNode (node));
	return tree.write_buffer ();
}

/* Keep only values and owning XML copies: no SessionHandleRef may survive the
 * native save/reopen portion of these tests. */
struct RenderSourceState {
	explicit RenderSourceState (Fixture& fixture)
		: position (fixture.session->transport_sample ())
		, start (fixture.session->current_start_sample ())
		, end (fixture.session->current_end_sample ())
		, dirty (fixture.session->dirty ())
		, loop (fixture.session->get_play_loop ())
		, range (fixture.session->get_play_range ())
		, record (fixture.session->get_record_enabled ())
		, external (fixture.session->config.get_external_sync ())
		, mmc (fixture.session->mmc ().send_enabled ())
	{
		disk = Glib::file_get_contents (fixture.path ("Render/Render.ardour"));
		std::unique_ptr<XMLNode> locations (&fixture.session->locations ()->get_state ());
		native = xml_text (*locations);
		stable_native = native;
		fixture.session->foreach_source ([&] (std::shared_ptr<Source> source) {
			std::unique_ptr<XMLNode> state (&source->get_state ());
			native += xml_text (*state);
			if (!source->writable () || !source->empty () || source->used ()) { stable_native += xml_text (*state); }
		});
		PBD::ControllableSet all;
		for (auto const& route : *fixture.session->get_routes ()) { route->automatables (all); }
		for (auto const& value : all) {
			auto control = std::dynamic_pointer_cast<AutomationControl> (value);
			if (!control) { continue; }
			std::string list;
			if (control->alist ()) {
				std::unique_ptr<XMLNode> state (&control->alist ()->get_state ());
				list = xml_text (*state);
			}
			controls[control->id ().to_s ()] = std::make_tuple (int (control->automation_state ()), control->get_value (), list);
		}
	}

	void check (Fixture& fixture, bool engine_restarted = false) const
	{
		wait_render_state ([&] { return !fixture.session->exporting () && !AudioEngine::instance ()->freewheeling () &&
			fixture.session->transport_stopped () && !fixture.session->locate_pending () && !fixture.session->locate_initiated (); });
		RenderSourceState after (fixture);
		CPPUNIT_ASSERT_EQUAL (position, after.position);
		CPPUNIT_ASSERT_EQUAL (start, after.start); CPPUNIT_ASSERT_EQUAL (end, after.end);
		CPPUNIT_ASSERT_EQUAL (dirty, after.dirty); CPPUNIT_ASSERT_EQUAL (loop, after.loop);
		CPPUNIT_ASSERT_EQUAL (range, after.range); CPPUNIT_ASSERT_EQUAL (record, after.record);
		CPPUNIT_ASSERT_EQUAL (external, after.external); CPPUNIT_ASSERT_EQUAL (mmc, after.mmc);
		CPPUNIT_ASSERT_EQUAL (disk, after.disk);
		/* Restarting a backend recreates unused empty capture Sources. Every
		 * used/nonempty Source, location and saved session must still match. */
		CPPUNIT_ASSERT_EQUAL (engine_restarted ? stable_native : native, engine_restarted ? after.stable_native : after.native);
		CPPUNIT_ASSERT_EQUAL (controls.size (), after.controls.size ());
		for (auto const& control : controls) {
			auto const& actual = after.controls.at (control.first);
			CPPUNIT_ASSERT_EQUAL (std::get<0> (control.second), std::get<0> (actual));
			CPPUNIT_ASSERT_DOUBLES_EQUAL (std::get<1> (control.second), std::get<1> (actual), 1e-9);
			CPPUNIT_ASSERT_EQUAL (std::get<2> (control.second), std::get<2> (actual));
		}
		CPPUNIT_ASSERT (AudioEngine::instance ()->running ());
		CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	}

	samplepos_t position, start, end;
	bool dirty, loop, range, record, external, mmc;
	std::string disk, native, stable_native;
	std::map<std::string, std::tuple<int, double, std::string>> controls;
};

const char* render_echo = R"lua(
ardour { ["type"] = "dsp", name = "Exchange delayed echo", license = "MIT", author = "Ardour" }
local dry, echo, di, ei = {}, {}, 1, 1
function dsp_ioconfig () return {{audio_in = 2, audio_out = 2}} end
function dsp_latency () return 64 end
function dsp_init (rate)
  for c = 1,2 do
    dry[c], echo[c] = {}, {}
    for s = 1,64 do dry[c][s] = 0 end
    for s = 1,321 do echo[c][s] = 0 end
  end
end
function dsp_runmap (bufs, in_map, out_map, n, offset)
  local a = {}
  for c = 1,2 do
    local ib = in_map:get (ARDOUR.DataType ("audio"), c - 1)
    assert (ib == out_map:get (ARDOUR.DataType ("audio"), c - 1))
    a[c] = bufs:get_audio (ib):data (offset):array ()
  end
  for s = 1,n do
    for c = 1,2 do
      local x = a[c][s]
      a[c][s] = 2 * dry[c][di] + 0.5 * echo[c][ei]
      dry[c][di], echo[c][ei] = x, x
    end
    di, ei = di % 64 + 1, ei % 321 + 1
  end
end
)lua";

const char* render_instrument = R"lua(
ardour { ["type"] = "dsp", name = "Exchange MIDI gate", category = "Instrument", license = "MIT", author = "Ardour" }
local active = false
function dsp_ioconfig () return {{midi_in = 1, audio_in = 0, audio_out = 2}} end
function dsp_run (ins, outs, n)
  local a, b, at = {}, {}, 1
  local function fill (first, last)
    for s = first,last do a[s], b[s] = active and 0.0625 or 0, active and 0.125 or 0 end
  end
  for _,event in ipairs (midiin) do
    local t, d = event.time, event.data
    fill (at, t - 1)
    if #d == 3 then
      local status = d[1] & 240
      if status == 144 then active = d[3] > 0 end
      if status == 128 or (status == 176 and (d[2] == 120 or d[2] == 123)) then active = false end
    end
    at = t
  end
  fill (at, n)
  outs[1]:set_table (a, n)
  outs[2]:set_table (b, n)
end
)lua";

void make_render_fixture (Fixture& fixture, bool automated = false)
{
	auto project = example (fixture);
	const auto rate = project.media.front ().sample_rate;
	SF_INFO info = {}; info.channels = 2; info.samplerate = rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	SNDFILE* output = sf_open (fixture.path ("source.wav").c_str (), SFM_WRITE, &info); CPPUNIT_ASSERT (output);
	std::vector<float> pcm (rate * 4);
	for (size_t i = 0; i < pcm.size () / 2; ++i) { pcm[2 * i] = .125; pcm[2 * i + 1] = .25; }
	CPPUNIT_ASSERT_EQUAL (sf_count_t (rate * 2), sf_writef_float (output, pcm.data (), rate * 2)); CPPUNIT_ASSERT_EQUAL (0, sf_close (output));
	project.tracks.erase (project.tracks.begin () + 1);
	project.tracks.resize (2);
	auto& audio = project.tracks[1]; audio.parent.clear (); audio.pan = .5; audio.volume = .5;
	audio.clips.resize (1); audio.clips[0].name = "Render pulse"; audio.clips[0].offset.value = 0;
	audio.clips[0].duration = automated ? 1 : .25; audio.clips[0].position.value = automated ? 1 : 1.25;
	audio.destination = automated ? "master" : "program";
	project.tracks[0].volume = automated ? 1 : .5;
	project.markers = {{"Render marker", PX::Time (.9, PX::TimeUnit::Seconds)}};
	if (automated) {
		audio.volume_automation = {PX::TimeUnit::Seconds, {{0, .25, false}, {1.375, .75, false}}};
		audio.pan_automation = {PX::TimeUnit::Seconds, {{0, 0, false}, {1.375, 1, false}}};
	} else {
		audio.sends = {{"room", false, true, .25, .5}};
		PX::Track program; program.id = "program"; program.name = "Program"; program.kind = PX::TrackKind::Bus;
		program.volume = .5; program.destination = "master"; project.tracks.push_back (program);
		PX::Track room = program; room.id = "room"; room.name = "Room"; room.volume = .25; project.tracks.push_back (room);
		PX::Track midi; midi.id = "gate"; midi.name = "MIDI gate"; midi.kind = PX::TrackKind::MIDI; midi.destination = "master";
		PX::Clip clip; clip.kind = PX::ClipKind::MIDI; clip.name = "Gate note";
		clip.position = PX::Time (.75, PX::TimeUnit::Seconds); clip.duration = .375; clip.offset = PX::Time (0, PX::TimeUnit::Seconds);
		PX::Note note; note.duration = .125; clip.notes.push_back (note); midi.clips.push_back (clip); project.tracks.push_back (midi);
	}
	package (fixture, project);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Render"), "Render", fixture.report);
	{
		auto route = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Stereo")); CPPUNIT_ASSERT (route);
		auto regions = route->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto audio = std::dynamic_pointer_cast<AudioRegion> (region); CPPUNIT_ASSERT (audio);
			audio->set_fade_in_active (false); audio->set_fade_out_active (false); audio->set_envelope_active (false);
		}
		if (!automated) {
			std::shared_ptr<Processor> before;
			route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
				auto send = std::dynamic_pointer_cast<InternalSend> (weak.lock ());
				if (send && send->role () == Delivery::Aux) { before = send; }
			});
			CPPUNIT_ASSERT (before);
			auto effect = std::make_shared<LuaProc> (*AudioEngine::instance (), *fixture.session, render_echo);
			auto insert = std::make_shared<PluginInsert> (*fixture.session, *fixture.session, effect);
			CPPUNIT_ASSERT_EQUAL (0, route->add_processor (insert, before)); CPPUNIT_ASSERT (!insert->get_pre_fader ());
			auto synth = std::make_shared<LuaProc> (*AudioEngine::instance (), *fixture.session, render_instrument);
			auto instrument = std::make_shared<PluginInsert> (*fixture.session, *fixture.session, synth);
			CPPUNIT_ASSERT_EQUAL (0, fixture.session->route_by_name ("MIDI gate")->add_processor (instrument, PreFader));
		}
	}
	fixture.session->set_session_extents (Temporal::timepos_t (samplepos_t (rate / 2)), Temporal::timepos_t (samplepos_t ((automated ? 2 : 1.5) * rate)));
	auto loop = new Location (*fixture.session, Temporal::timepos_t (samplepos_t (rate)), Temporal::timepos_t (samplepos_t (rate * 5 / 4)), "Inactive render loop", Location::IsAutoLoop);
	fixture.session->locations ()->add (loop); fixture.session->set_auto_loop_location (loop);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	fixture.session->request_locate (rate / 2, true, MustStop);
	wait_render_state ([&] { return fixture.session->transport_sample () == rate / 2 && fixture.session->transport_stopped () && !fixture.session->locate_pending () && !fixture.session->locate_initiated (); });
}

std::vector<float> read_rendered_pcm (const std::string& path, samplecnt_t frames, unsigned rate)
{
	SF_INFO info = {}; SNDFILE* input = sf_open (path.c_str (), SFM_READ, &info); CPPUNIT_ASSERT (input);
	CPPUNIT_ASSERT_EQUAL (sf_count_t (frames), info.frames); CPPUNIT_ASSERT_EQUAL (2, info.channels); CPPUNIT_ASSERT_EQUAL (int (rate), info.samplerate);
	std::vector<float> pcm (size_t (frames) * 2);
	CPPUNIT_ASSERT_EQUAL (sf_count_t (frames), sf_readf_float (input, pcm.data (), frames)); CPPUNIT_ASSERT_EQUAL (0, sf_close (input));
	return pcm;
}

void assert_render_model (const PX::Project& project, samplecnt_t frames, unsigned rate)
{
	CPPUNIT_ASSERT_EQUAL (size_t (2), project.tracks.size ()); CPPUNIT_ASSERT_EQUAL (size_t (1), project.media.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (100, project.tempo, .000001);
	CPPUNIT_ASSERT_EQUAL (unsigned (3), project.numerator); CPPUNIT_ASSERT_EQUAL (unsigned (4), project.denominator);
	unsigned masters = 0, tracks = 0;
	PX::TimeMap time (project);
	for (auto const& track : project.tracks) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (1, track.volume, 1e-8); CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, track.pan, 1e-8);
		CPPUNIT_ASSERT (!track.mute && !track.solo && track.plugins.empty () && track.sends.empty ());
		CPPUNIT_ASSERT (track.volume_automation.points.empty () && track.pan_automation.points.empty ());
		if (track.master) { ++masters; continue; }
		++tracks; CPPUNIT_ASSERT (track.kind == PX::TrackKind::Audio); CPPUNIT_ASSERT_EQUAL (size_t (1), track.clips.size ());
		auto const& clip = track.clips.front ();
		CPPUNIT_ASSERT (clip.enabled && clip.envelope.points.empty ()); CPPUNIT_ASSERT_DOUBLES_EQUAL (1, clip.gain, 1e-8);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (0, time.convert (clip.position, PX::TimeUnit::Seconds), 1.0 / rate);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (0, time.convert (clip.offset, PX::TimeUnit::Seconds), 1.0 / rate);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (double (frames) / rate, time.duration (PX::Time (clip.duration, clip.position.unit), clip.position, PX::TimeUnit::Seconds), 1.0 / rate);
	}
	CPPUNIT_ASSERT_EQUAL (1u, masters); CPPUNIT_ASSERT_EQUAL (1u, tracks);
	CPPUNIT_ASSERT (std::any_of (project.markers.begin (), project.markers.end (), [&] (const PX::Marker& marker) {
		return marker.name == "Render marker" && std::abs (time.convert (marker.position, PX::TimeUnit::Seconds) - .9) < .0001;
	}));
}

void assert_graph_pcm (const std::vector<float>& pcm, unsigned rate, double tolerance)
{
	const samplepos_t begin = std::llround (1.25 * rate), end = std::llround (1.5 * rate);
	const samplepos_t note_on = std::llround (.75 * rate), note_off = std::llround (.875 * rate);
	for (samplepos_t frame = 0; frame < samplepos_t (pcm.size () / 2); ++frame) {
		for (unsigned channel = 0; channel < 2; ++channel) {
			double input = channel ? .25 : .125;
			double dry = frame >= begin && frame < end ? input * .5 : 0;
			double echo = frame >= begin + 257 && frame < end + 257 ? input * .5 : 0;
			double gate = frame >= note_on && frame < note_off ? (channel ? .125 : .0625) : 0;
			// Output tap includes source fader, real delayed DSP, Program bus,
			// post-fader Room send, Room bus, master fader and a MIDI instrument.
			double expected = .5 * ((2 * dry + .5 * echo) * (.5 + .25 * .25) + gate);
			CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE ("Frame " + std::to_string (frame) + ", channel " + std::to_string (channel), expected, pcm[size_t (frame) * 2 + channel], tolerance);
		}
	}
}

} // namespace

class ProjectSessionTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE (ProjectSessionTest);
	CPPUNIT_TEST (roundtrip);
	CPPUNIT_TEST (multichannel_audio_roundtrip);
	CPPUNIT_TEST (midi_controller_roundtrip);
	CPPUNIT_TEST (midi_ramp_roundtrip);
	CPPUNIT_TEST (native_midi_curves_roundtrip);
	CPPUNIT_TEST (native_midi_curves_limit);
	CPPUNIT_TEST (vst3_plugin_exchange);
	CPPUNIT_TEST (native_song_app_authored_roundtrip);
	CPPUNIT_TEST (bad_media);
	CPPUNIT_TEST (corrupt_media);
	CPPUNIT_TEST (existing_destination);
#ifdef PLATFORM_WINDOWS
	CPPUNIT_TEST (retry_locked_publish);
#endif
	CPPUNIT_TEST (cancel_import);
	CPPUNIT_TEST (cancel_populated_import);
	CPPUNIT_TEST (cancel_reopened_import);
	CPPUNIT_TEST (reject_overlapping_notes);
	CPPUNIT_TEST (cancel_export);
	CPPUNIT_TEST (existing_export);
	CPPUNIT_TEST (reject_unmapped_processing);
	CPPUNIT_TEST (native_song_import);
	CPPUNIT_TEST (native_song_mixer_automation);
	CPPUNIT_TEST (native_song_timing_and_gain);
	CPPUNIT_TEST (native_song_fades);
	CPPUNIT_TEST (native_song_export_roundtrip);
	CPPUNIT_TEST (native_song_export_guards);
	CPPUNIT_TEST (native_song_render_export);
	CPPUNIT_TEST (bus_sends_roundtrip);
	CPPUNIT_TEST (native_song_fx_sends_roundtrip);
	CPPUNIT_TEST (native_song_format_seven_import);
	CPPUNIT_TEST (native_song_format_eight_import);
	CPPUNIT_TEST (native_song_format_eight_app_authored);
	CPPUNIT_TEST (native_song_cancel);
	CPPUNIT_TEST (native_song_unsupported);
	CPPUNIT_TEST (native_song_partial_import);
	CPPUNIT_TEST (dawproject_partial_import);
	CPPUNIT_TEST (native_song_instrument_template_import);
	CPPUNIT_TEST (marker_roundtrip);
	CPPUNIT_TEST (studio_pro_export);
	CPPUNIT_TEST (variable_timing);
	CPPUNIT_TEST (second_based_ramp);
	CPPUNIT_TEST (transport_collision);
	CPPUNIT_TEST (mixer_automation_roundtrip);
	CPPUNIT_TEST (native_fader_automation);
	CPPUNIT_TEST (automation_collision);
	CPPUNIT_TEST (region_fades_roundtrip);
	CPPUNIT_TEST (render_mix_roundtrip);
	CPPUNIT_TEST (render_mix_automation);
	CPPUNIT_TEST (render_pending_buffering);
	CPPUNIT_TEST (render_mix_cancel_and_guards);
	CPPUNIT_TEST (render_stems_roundtrip);
	CPPUNIT_TEST (render_stems_guards);
	CPPUNIT_TEST_SUITE_END ();
public:
	void roundtrip ();
	void multichannel_audio_roundtrip ();
	void midi_controller_roundtrip ();
	void midi_ramp_roundtrip ();
	void native_midi_curves_roundtrip ();
	void native_midi_curves_limit ();
	void vst3_plugin_exchange ();
	void native_song_app_authored_roundtrip ();
	void bad_media ();
	void corrupt_media ();
	void existing_destination ();
#ifdef PLATFORM_WINDOWS
	void retry_locked_publish ();
#endif
	void cancel_import ();
	void cancel_populated_import ();
	void cancel_reopened_import ();
	void reject_overlapping_notes ();
	void cancel_export ();
	void existing_export ();
	void reject_unmapped_processing ();
	void native_song_import ();
	void native_song_mixer_automation ();
	void native_song_timing_and_gain ();
	void native_song_fades ();
	void native_song_export_roundtrip ();
	void native_song_export_guards ();
	void native_song_render_export ();
	void bus_sends_roundtrip ();
	void native_song_fx_sends_roundtrip ();
	void native_song_format_seven_import ();
	void native_song_format_eight_import ();
	void native_song_format_eight_app_authored ();
	void native_song_cancel ();
	void native_song_unsupported ();
	void native_song_partial_import ();
	void dawproject_partial_import ();
	void native_song_instrument_template_import ();
	void marker_roundtrip ();
	void studio_pro_export ();
	void variable_timing ();
	void second_based_ramp ();
	void transport_collision ();
	void mixer_automation_roundtrip ();
	void native_fader_automation ();
	void region_fades_roundtrip ();
	void automation_collision ();
	void render_mix_roundtrip ();
	void render_mix_automation ();
	void render_pending_buffering ();
	void render_mix_cancel_and_guards ();
	void render_stems_roundtrip ();
	void render_stems_guards ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (ProjectSessionTest);

void ProjectSessionTest::native_midi_curves_limit ()
{
	Fixture fixture; auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 1);
	for (auto& track : input.tracks) { track.parent.clear (); track.pan = .5; if (!track.master) { track.destination = "master"; } }
	auto& clip = input.tracks.back ().clips.front (); clip.offset.value = 0; clip.duration = 1400; clip.notes.clear ();
	PX::MidiControl lane; lane.type = 0xe0; lane.channel = 2;
	for (unsigned i = 0; i <= 14; ++i) { lane.points.push_back ({i * 100.0, i % 2 ? 16383.0 : 0.0, false}); }
	clip.midi_controls.push_back (lane); package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Limit"), "Limit", fixture.report);
	{
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Notes")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto midi = std::dynamic_pointer_cast<MidiRegion> (region); CPPUNIT_ASSERT (midi);
			for (auto const& control : midi->model ()->controls ()) {
				midi->midi_source ()->set_interpolation_of (control.first, Evoral::ControlList::Linear);
				CPPUNIT_ASSERT (control.second->list ()->set_interpolation (Evoral::ControlList::Linear));
			}
		}
	}
	for (bool logic : {false, true}) {
		bool rejected = false; auto path = fixture.path (logic ? "OversizedLogic" : "oversized.dawproject");
		try {
			if (logic) { PX::SessionExchange::export_logic (*fixture.session, path, fixture.report); }
			else { PX::SessionExchange::export_dawproject (*fixture.session, path, fixture.report); }
		} catch (PX::ExchangeError const& e) {
			rejected = true; CPPUNIT_ASSERT (std::string (e.what ()).find ("sampled MIDI controllers exceed") != std::string::npos);
		}
		CPPUNIT_ASSERT (rejected); CPPUNIT_ASSERT (!Glib::file_test (path, Glib::FILE_TEST_EXISTS));
	}
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "Native MIDI curve expansion limit rejects both editable export routes before publication" << std::endl;
}

void ProjectSessionTest::native_midi_curves_roundtrip ()
{
	Fixture fixture; auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 1);
	for (auto& track : input.tracks) { track.parent.clear (); track.pan = .5; if (!track.master) { track.destination = "master"; } }
	input.tempo = 120; input.tempos = {{PX::Time (5), 90, false}, {PX::Time (13), 150, false}};
	auto& midi = input.tracks.back ();
	for (unsigned type : {0xb0u, 0xe0u, 0xd0u, 0xa0u}) {
		PX::MidiControl lane; lane.type = type; lane.channel = 2; lane.number = type == 0xb0 ? 1 : type == 0xa0 ? 60 : 0;
		double maximum = type == 0xe0 ? 16383 : 127;
		lane.points = {{0, 0, false}, {2, maximum, false}, {4, 0, false}, {6, maximum, false}};
		midi.clips[0].midi_controls.push_back (lane);
	}
	PX::MidiControl hold; hold.number = 64; hold.channel = 2; hold.points = {{0, 127, false}, {3, 0, false}};
	midi.clips[0].midi_controls.push_back (hold);
	auto second = midi.clips[0]; second.position.value = 12; second.offset.value = .3; second.duration = 2; midi.clips.push_back (second);
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("NativeCurves"), "NativeCurves", fixture.report);
	{
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Notes")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto mr = std::dynamic_pointer_cast<MidiRegion> (region); CPPUNIT_ASSERT (mr);
			for (auto const& entry : mr->model ()->controls ()) {
				if (entry.first.type () == MidiCCAutomation && entry.first.id () == 64) { continue; }
				mr->midi_source ()->set_interpolation_of (entry.first, Evoral::ControlList::Linear);
				CPPUNIT_ASSERT (entry.second->list ()->set_interpolation (Evoral::ControlList::Linear));
			}
			mr->model ()->set_edited (true);
		}
	}
	auto reopen = [&] (const std::string& name) {
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		AudioEngine::instance ()->set_session (fixture.session.get ());
	};
	reopen ("NativeCurves");
	using Events = std::map<samplepos_t, std::multiset<std::vector<uint8_t>>>;
	auto capture = [&] () {
		struct Sink : Evoral::EventSink<samplepos_t> {
			Events events;
			uint32_t write (samplepos_t at, Evoral::EventType, uint32_t size, const uint8_t* data) override {
				if ((data[0] & 0xf0) != 0x80 && (data[0] & 0xf0) != 0x90) { events[at].insert (std::vector<uint8_t> (data, data + size)); }
				return size;
			}
		} sink;
		std::ostringstream curves; curves << std::setprecision (17);
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Notes")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto mr = std::dynamic_pointer_cast<MidiRegion> (region); CPPUNIT_ASSERT (mr);
			mr->render (sink, 0, Sustained, nullptr);
			for (auto const& entry : mr->model ()->controls ()) {
				curves << entry.first.type () << ':' << entry.first.channel () << ':' << entry.first.id () << ':' << entry.second->list ()->interpolation () << ';';
				for (auto const& point : *entry.second->list ()) { curves << point->when.beats ().to_ticks () << ':' << point->value << ';'; }
			}
		}
		return std::make_pair (sink.events, curves.str ());
	};
	const auto expected = capture (); CPPUNIT_ASSERT (expected.first.size () > 100);
	const auto disk = Glib::file_get_contents (fixture.path ("NativeCurves/NativeCurves.ardour"));
	unsigned calls = 0;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("cancelled-curves.dawproject"), fixture.report, [&] { return ++calls > 250; }), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("cancelled-curves.dawproject"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT (expected == capture ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-curves.dawproject"), fixture.report);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Transformed && d.detail.find ("Ardour playback timing") != std::string::npos; }));
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("NativeCurveLogic"), fixture.report);
	CPPUNIT_ASSERT (expected == capture ());
	CPPUNIT_ASSERT_EQUAL (disk, Glib::file_get_contents (fixture.path ("NativeCurves/NativeCurves.ardour")));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("native-curves.dawproject"), fixture.path ("ReturnedCurves"), "ReturnedCurves", fixture.report);
	reopen ("ReturnedCurves"); CPPUNIT_ASSERT (expected.first == capture ().first);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("NativeCurveLogic/Project.aaf"), fixture.path ("NativeCurveLogic/Project.mid"), fixture.path ("LogicCurves"), "LogicCurves", fixture.report);
	reopen ("LogicCurves"); CPPUNIT_ASSERT (expected.first == capture ().first);
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "Native MIDI controller curves preserve saved playback through DAWproject and AAF/SMF; cancellation and source state passed" << std::endl;
}

void ProjectSessionTest::midi_ramp_roundtrip ()
{
	Fixture fixture;
	{
		PX::ProjectArchiveWriter archive (fixture.path ("ramps.dawproject"));
		archive.add_text ("project.xml", R"(<Project version="1.0">
<Application name="Independent MIDI ramp fixture" version="1"/>
<Transport><Tempo value="120" unit="bpm"/><TimeSignature numerator="4" denominator="4"/></Transport>
<Structure><Track id="notes" name="Ramps" contentType="notes"><Channel id="midi" audioChannels="2" destination="master"/></Track>
<Channel id="master" name="Master" role="master" audioChannels="2"/></Structure>
<Arrangement><Lanes><Lanes track="notes"><Clips><Clip name="Ramps" time="4" duration="2" playStart="1"><Lanes>
<Points><Target expression="channelController" channel="1" controller="1"/><IntegerPoint time="0" value="0" interpolation="linear"/><IntegerPoint time="4" value="4"/></Points>
<Points><Target expression="pitchBend" channel="2"/><IntegerPoint time="0" value="0" interpolation="linear"/><IntegerPoint time="4" value="16383"/></Points>
<Points timeUnit="seconds"><Target expression="channelPressure" channel="3"/><IntegerPoint time="0" value="0" interpolation="linear"/><IntegerPoint time="2" value="4"/></Points>
<Points><Target expression="polyPressure" channel="4" key="60"/><IntegerPoint time="0" value="4" interpolation="linear"/><IntegerPoint time="4" value="0"/></Points>
</Lanes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)");
		archive.finish ();
	}
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("ramps.dawproject"), fixture.path ("Ramps"), "Ramps", fixture.report);
	for (std::string message : {"half a MIDI unit", "last source value", "clip start"}) {
		CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [&] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Transformed && d.detail.find (message) != std::string::npos; }));
	}
	using Events = std::map<samplepos_t, std::multiset<std::vector<uint8_t>>>;
	Events expected;
	auto add = [&] (double beat, const std::vector<uint8_t>& data) {
		expected[Temporal::timepos_t (Temporal::Beats::from_double (beat)).samples ()].insert (data);
	};
	for (int i = 0; i < 3; ++i) {
		double at = i == 0 ? 4 : 3.5 + i;
		add (at, {0xb1, 1, uint8_t (1 + i)});
		add (at, {0xd3, uint8_t (1 + i)});
		add (at, {0xa4, 60, uint8_t (3 - i)});
	}
	// Independently evaluate the last value in each native tick's rounding
	// interval. Dense 14-bit crossings merge; 7-bit lanes remain unaffected.
	const int ppqn = Temporal::Beats::PPQN;
	for (int tick = ppqn; tick < 3 * ppqn; ++tick) {
		unsigned value = std::floor ((tick + .5) * 16383 / (4 * ppqn) + .5);
		add (3 + double (tick) / ppqn, {0xe2, uint8_t (value & 127), uint8_t (value >> 7)});
	}
	auto check_reopened = [&] (const std::string& name) {
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		AudioEngine::instance ()->set_session (fixture.session.get ());
		struct Sink : Evoral::EventSink<samplepos_t> {
			Events events;
			uint32_t write (samplepos_t at, Evoral::EventType, uint32_t size, const uint8_t* data) override {
				events[at].insert (std::vector<uint8_t> (data, data + size)); return size;
			}
		} sink;
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Ramps")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto midi = std::dynamic_pointer_cast<MidiRegion> (region); CPPUNIT_ASSERT (midi);
			midi->render (sink, 0, Sustained, nullptr);
		}
		CPPUNIT_ASSERT_EQUAL (expected.size (), sink.events.size ());
		CPPUNIT_ASSERT (expected == sink.events);
	};
	check_reopened ("Ramps");
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("returned.dawproject"));
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("RampLogic"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("returned.dawproject"), fixture.path ("ReturnedRamps"), "ReturnedRamps", fixture.report);
	check_reopened ("ReturnedRamps");
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("RampLogic/Project.aaf"), fixture.path ("RampLogic/Project.mid"), fixture.path ("LogicRamps"), "LogicRamps", fixture.report);
	check_reopened ("LogicRamps");
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "MIDI ramp quantization, trim chase and saved playback across DAWproject and AAF/SMF passed" << std::endl;
}

void ProjectSessionTest::midi_controller_roundtrip ()
{
	Fixture fixture; auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 1);
	for (auto& track : input.tracks) { track.parent.clear (); track.pan = .5; if (!track.master) { track.destination = "master"; } }
	auto& midi = input.tracks.back ();
	for (unsigned type : {0xb0u, 0xe0u, 0xd0u, 0xa0u}) {
		PX::MidiControl lane; lane.type = type; lane.channel = 2; lane.number = type == 0xb0 ? 64 : type == 0xa0 ? 60 : 0;
		lane.points = {{.25, type == 0xe0 ? 8192.0 : 127.0, false}, {1.25, 0, false}, {3.5, type == 0xe0 ? 16383.0 : 100.0, false}, {5, 0, false}};
		midi.clips[0].midi_controls.push_back (lane);
	}
	PX::Clip controls; controls.kind = PX::ClipKind::MIDI; controls.name = "Expression only"; controls.position = PX::Time (8); controls.duration = 2;
	PX::MidiControl lane; lane.channel = 3; lane.number = 11; lane.points = {{0, 30, false}, {1, 90, false}};
	controls.midi_controls.push_back (lane); midi.clips.push_back (controls);
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Controllers"), "Controllers", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Controllers"), "Controllers", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	{
		struct Sink : Evoral::EventSink<samplepos_t> {
			std::vector<std::pair<samplepos_t, std::vector<uint8_t>>> events;
			uint32_t write (samplepos_t at, Evoral::EventType, uint32_t size, const uint8_t* data) override {
				if ((data[0] & 0xf0) != 0x80 && (data[0] & 0xf0) != 0x90) { events.push_back ({at, std::vector<uint8_t> (data, data + size)}); }
				return size;
			}
		} sink;
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Notes")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			auto midi_region = std::dynamic_pointer_cast<MidiRegion> (region); CPPUNIT_ASSERT (midi_region);
			if (region->position ().beats () == Temporal::Beats (4, 0)) { midi_region->render (sink, 0, Sustained, nullptr); }
		}
		CPPUNIT_ASSERT_EQUAL (size_t (12), sink.events.size ());
		const samplepos_t at = Temporal::timepos_t (Temporal::Beats (4, 0)).samples ();
		std::set<std::vector<uint8_t>> initial;
		for (auto const& event : sink.events) { if (event.first == at) { initial.insert (event.second); } }
		const std::set<std::vector<uint8_t>> expected = {{0xb2, 64, 127}, {0xe2, 0, 64}, {0xd2, 127}, {0xa2, 60, 127}};
		CPPUNIT_ASSERT (initial == expected);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("controllers.dawproject"));
	auto exported = PX::DawProject::read (fixture.path ("controllers.dawproject"));
	auto const& actual = track_named (exported, "Notes");
	CPPUNIT_ASSERT_EQUAL (size_t (2), actual.clips.size ());
	for (size_t c = 0; c < midi.clips.size (); ++c) {
		CPPUNIT_ASSERT_EQUAL (midi.clips[c].midi_controls.size (), actual.clips[c].midi_controls.size ());
		for (auto const& expected : midi.clips[c].midi_controls) {
			auto found = std::find_if (actual.clips[c].midi_controls.begin (), actual.clips[c].midi_controls.end (), [&] (const PX::MidiControl& p) { return p.type == expected.type && p.channel == expected.channel && p.number == expected.number; });
			CPPUNIT_ASSERT (found != actual.clips[c].midi_controls.end ());
			auto points = expected.points;
			if (c == 0) { points.insert (points.begin () + 1, {1, points.front ().value, false}); }
			CPPUNIT_ASSERT_EQUAL (points.size (), found->points.size ());
			for (size_t i = 0; i < points.size (); ++i) {
				CPPUNIT_ASSERT_DOUBLES_EQUAL (points[i].time, found->points[i].time, 1e-6);
				CPPUNIT_ASSERT_EQUAL (points[i].value, found->points[i].value); CPPUNIT_ASSERT (!found->points[i].linear);
			}
		}
	}
	PX::SessionExchange::ExportOptions options;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("UnverifiedNative"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("UnverifiedNative"), Glib::FILE_TEST_EXISTS));
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("ControllerLogic"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("ControllerLogic/Project.aaf"), fixture.path ("ControllerLogic/Project.mid"), fixture.path ("ReturnedControls"), "ReturnedControls", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("ReturnedControls"), "ReturnedControls", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("returned-controls.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("returned-controls.dawproject"));
	auto const& flat = track_named (returned, "Notes"); CPPUNIT_ASSERT_EQUAL (size_t (1), flat.clips.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (5), flat.clips[0].midi_controls.size ());
	for (auto const& control : flat.clips[0].midi_controls) {
		if (control.channel == 3) {
			CPPUNIT_ASSERT_EQUAL (size_t (2), control.points.size ()); CPPUNIT_ASSERT_DOUBLES_EQUAL (8, control.points[0].time, 1e-6); CPPUNIT_ASSERT_EQUAL (30.0, control.points[0].value);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (9, control.points[1].time, 1e-6); CPPUNIT_ASSERT_EQUAL (90.0, control.points[1].value); continue;
		}
		CPPUNIT_ASSERT_EQUAL (size_t (3), control.points.size ());
		CPPUNIT_ASSERT_DOUBLES_EQUAL (4, control.points[0].time, 1e-6); CPPUNIT_ASSERT_EQUAL (control.type == 0xe0 ? 8192.0 : 127.0, control.points[0].value);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (4.25, control.points[1].time, 1e-6); CPPUNIT_ASSERT_EQUAL (0.0, control.points[1].value);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (6.5, control.points[2].time, 1e-6); CPPUNIT_ASSERT_EQUAL (control.type == 0xe0 ? 16383.0 : 100.0, control.points[2].value);
	}
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "MIDI controller DAWproject and AAF/SMF trim, chase, save/reopen round trips passed" << std::endl;
}

void ProjectSessionTest::native_song_export_roundtrip ()
{
	Fixture fixture; auto input = example (fixture);
	for (auto& track : input.tracks) {
		if (!track.master) { track.destination = "master"; }
		for (auto& clip : track.clips) { for (auto& note : clip.notes) { note.channel = 0; note.release_velocity = 64.0 / 127; } }
	}
	input.tempos = {{PX::Time (0), 100, false}, {PX::Time (3), 120, false}};
	input.meters = {{PX::Time (0), 3, 4}, {PX::Time (3), 5, 8}};
	input.markers.push_back ({"Verse", PX::Time (6)});
	auto& clip = input.tracks[2].clips[0]; clip.gain = .5;
	clip.envelope.unit = PX::TimeUnit::Seconds;
	clip.envelope.points = {{0, 0, true}, {.125, 1, true}, {.375, 1, true}, {.5, 0, true}};
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("NativeSource"), "NativeSource", fixture.report);
	const unsigned rate = fixture.session->sample_rate ();
	fixture.session->set_session_extents (Temporal::timepos_t (samplepos_t (rate / 2)), Temporal::timepos_t (samplepos_t (6 * rate)));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	const std::string saved = Glib::file_get_contents (fixture.path ("NativeSource/NativeSource.ardour"));
	PX::SessionExchange::ExportOptions options;
	options.studio_one_format_version = 6;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("InvalidVersion"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("InvalidVersion"), Glib::FILE_TEST_EXISTS));
	options.studio_one_format_version = 9;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("Exported Song"), fixture.report, options);
	CPPUNIT_ASSERT_EQUAL (saved, Glib::file_get_contents (fixture.path ("NativeSource/NativeSource.ardour")));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT (!fixture.session->dirty ());
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.detail.find ("Experimental") != std::string::npos; }));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("input.dawproject").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("source.wav").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_rename (fixture.path ("Exported Song").c_str (), fixture.path ("Moved Song").c_str ()));
	auto output = PX::StudioOneSong::read (fixture.path ("Moved Song/Project.song"));
	CPPUNIT_ASSERT_EQUAL (std::string ("NativeSource"), output.title);
	CPPUNIT_ASSERT_EQUAL (9u, output.format_version);
	auto const& exported_folder = track_named (output.project, "Band");
	CPPUNIT_ASSERT_EQUAL (PX::TrackKind::Folder, exported_folder.kind);
	CPPUNIT_ASSERT_EQUAL (exported_folder.id, track_named (output.project, "Stereo").parent);
	CPPUNIT_ASSERT_EQUAL (exported_folder.id, track_named (output.project, "Notes").parent);
	CPPUNIT_ASSERT_EQUAL (rate, output.sample_rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, output.start.value, 1.0 / rate); CPPUNIT_ASSERT_DOUBLES_EQUAL (6, output.end.value, 1.0 / rate);
	CPPUNIT_ASSERT_EQUAL (size_t (2), output.project.tempos.size ()); CPPUNIT_ASSERT_EQUAL (size_t (2), output.project.meters.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), output.project.markers.size ());
	auto pcm = read_rendered_pcm (output.audio_files.at (output.project.media.front ().id), 2 * rate, rate);
	for (size_t i = 0; i < pcm.size () / 2; ++i) {
		CPPUNIT_ASSERT_EQUAL (float (int (i % 37) - 18) / 32, pcm[2 * i]);
		CPPUNIT_ASSERT_EQUAL (float (int (i % 61) - 30) / 64, pcm[2 * i + 1]);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("Moved Song/Project.song"), fixture.path ("NativeReturn"), "NativeReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("NativeReturn"), "NativeReturn", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("returned.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("returned.dawproject"));
	auto const& audio = track_named (returned, "Stereo"); auto const& midi = track_named (returned, "Notes");
	auto const& folder = track_named (returned, "Band");
	CPPUNIT_ASSERT_EQUAL (PX::TrackKind::Folder, folder.kind);
	CPPUNIT_ASSERT_EQUAL (folder.id, audio.parent); CPPUNIT_ASSERT_EQUAL (folder.id, midi.parent);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, audio.volume, 1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (.3, audio.pan, 1e-6);
	CPPUNIT_ASSERT_EQUAL (std::string ("#12ab34"), audio.color);
	CPPUNIT_ASSERT_EQUAL (size_t (2), audio.clips.size ()); CPPUNIT_ASSERT (!audio.clips[1].enabled);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, audio.clips[0].position.value, 1.0 / rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.375, audio.clips[0].offset.value, 1.0 / rate);
	for (double at : {0.0, .0625, .125, .25, .4375, .499}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (clip.gain * curve_value (clip.envelope, at), audio.clips[0].gain * curve_value (audio.clips[0].envelope, at), 1e-6);
	}
	CPPUNIT_ASSERT_EQUAL (size_t (1), midi.clips.size ()); CPPUNIT_ASSERT_EQUAL (size_t (2), midi.clips[0].notes.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, midi.clips[0].position.value, 1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (1, midi.clips[0].offset.value, 1e-6);
	for (size_t i = 0; i < 2; ++i) {
		auto const& expected = input.tracks[3].clips[0].notes[i]; auto const& actual = midi.clips[0].notes[i];
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.time, actual.time, 1e-6); CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.duration, actual.duration, 1e-6);
		CPPUNIT_ASSERT_EQUAL (expected.key, actual.key); CPPUNIT_ASSERT_EQUAL (expected.channel, actual.channel);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.velocity, actual.velocity, 1e-6);
	}
	std::cout << "Native song audio/MIDI export, relocation and session reopen passed" << std::endl;
}

void ProjectSessionTest::native_song_export_guards ()
{
	Fixture fixture; native_package (fixture);
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("Native"), "Native", fixture.report);
	PX::SessionExchange::ExportOptions options;
	CPPUNIT_ASSERT_EQUAL (0, g_mkdir (fixture.path ("Existing").c_str (), 0700));
	std::ofstream (fixture.path ("Existing/keep.txt")) << "keep";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("Existing"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT_EQUAL (std::string ("keep"), Glib::file_get_contents (fixture.path ("Existing/keep.txt")));
	bool staged = false;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("Cancelled"), fixture.report, options,
		[&] { return staged = stages (fixture.root) != 0; }), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (staged); CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Cancelled"), Glib::FILE_TEST_EXISTS));
	{
		auto route = fixture.session->route_by_name ("Native audio"); auto list = route->gain_control ()->alist ();
		list->clear (); list->add (Temporal::timepos_t (samplepos_t (0)), .5); list->add (Temporal::timepos_t (samplepos_t (48000)), 1); list->set_automation_state (Play);
		CPPUNIT_ASSERT (list->set_interpolation (Evoral::ControlList::Discrete));
	}
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("Unsupported"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Unsupported; }));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Unsupported"), Glib::FILE_TEST_EXISTS)); CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "Native song cancellation, overwrite and unsupported-feature guards passed" << std::endl;
}

void ProjectSessionTest::native_song_render_export ()
{
	Fixture fixture; make_render_fixture (fixture); const unsigned rate = fixture.session->sample_rate ();
	const samplecnt_t frames = fixture.session->current_end_sample () + 1024;
	PX::SessionExchange::ExportOptions options; options.render_mix = true; options.render_tail_seconds = 1024.0 / rate;
	RenderSourceState original (fixture);
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("RenderedSong"), fixture.report, options);
	original.check (fixture);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Rendered; }));
	auto song = PX::StudioOneSong::read (fixture.path ("RenderedSong/Project.song"));
	assert_render_model (song.project, frames, rate);
	assert_graph_pcm (read_rendered_pcm (song.audio_files.at (song.project.media.front ().id), frames, rate), rate, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0, song.start.value, 1e-9); CPPUNIT_ASSERT_DOUBLES_EQUAL (double (frames) / rate, song.end.value, 1.0 / rate);
	std::cout << "Native song rendered master graph PCM and source restoration passed" << std::endl;
}

void ProjectSessionTest::render_stems_roundtrip ()
{
	Fixture fixture; make_render_fixture (fixture);
	fixture.session->master_out ()->gain_control ()->set_value (1, PBD::Controllable::NoGroup);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	const unsigned rate = fixture.session->sample_rate ();
	const samplecnt_t frames = fixture.session->current_end_sample () + 1024;
	PX::SessionExchange::ExportOptions options; options.render_stems = true; options.render_tail_seconds = 1024.0 / rate;
	RenderSourceState original (fixture);
	auto verify_pcm = [&] (const std::string& name, const std::vector<float>& pcm, double tolerance) {
		CPPUNIT_ASSERT (name == "Program" || name == "Room" || name == "MIDI gate");
		for (samplecnt_t i = 0; i < frames; ++i) {
			for (unsigned c = 0; c < 2; ++c) {
				double input = c ? .25 : .125;
				double dry = i >= std::llround (1.25 * rate) && i < std::llround (1.5 * rate) ? input * .5 : 0;
				double echo = i >= std::llround (1.25 * rate) + 257 && i < std::llround (1.5 * rate) + 257 ? input * .5 : 0;
				double gate = i >= std::llround (.75 * rate) && i < std::llround (.875 * rate) ? (c ? .125 : .0625) : 0;
				double expected = name == "MIDI gate" ? gate : (2 * dry + .5 * echo) * (name == "Program" ? .5 : .25 * .25);
				CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE (name + " frame " + std::to_string (i), expected, pcm[size_t (i) * 2 + c], tolerance);
			}
		}
	};
	for (unsigned kind = 0; kind < 3; ++kind) {
		const std::string destination = fixture.path (kind == 0 ? "stems.dawproject" : kind == 1 ? "StemLogic" : "StemNative");
		if (kind == 0) { PX::SessionExchange::export_dawproject (*fixture.session, destination, fixture.report, options); }
		else if (kind == 1) { PX::SessionExchange::export_logic (*fixture.session, destination, fixture.report, options); }
		else { PX::SessionExchange::export_studio_one (*fixture.session, destination, fixture.report, options); }
		original.check (fixture);
		PX::Project model; std::map<std::string, std::string> files;
		if (kind == 0) {
			model = PX::DawProject::read (destination);
			for (auto const& media : model.media) { files[media.id] = fixture.path ("daw-" + media.id + ".wav"); PX::ProjectArchive (destination).copy_file (media.path, files[media.id]); }
		} else if (kind == 1) {
			PX::LogicProject project (Glib::build_filename (destination, "Project.aaf"), Glib::build_filename (destination, "Project.mid")); model = project.project ();
			for (auto const& media : model.media) { files[media.id] = fixture.path ("logic-" + media.id + ".wav"); project.copy_media (media, files[media.id]); }
		} else {
			auto song = PX::StudioOneSong::read (Glib::build_filename (destination, "Project.song")); model = song.project; files = song.audio_files;
		}
		CPPUNIT_ASSERT_EQUAL (size_t (4), model.tracks.size ()); CPPUNIT_ASSERT_EQUAL (size_t (3), model.media.size ());
		std::vector<float> sum (size_t (frames) * 2, 0);
		for (auto const& track : model.tracks) {
			if (track.master) { continue; }
			CPPUNIT_ASSERT (track.kind == PX::TrackKind::Audio && track.plugins.empty () && track.sends.empty ());
			CPPUNIT_ASSERT_EQUAL (1.0, track.volume); CPPUNIT_ASSERT_EQUAL (.5, track.pan);
			CPPUNIT_ASSERT_EQUAL (size_t (1), track.clips.size ());
			CPPUNIT_ASSERT_EQUAL (0.0, track.clips.front ().position.value);
			auto pcm = read_rendered_pcm (files.at (track.clips.front ().media), frames, rate);
			verify_pcm (track.name, pcm, 2e-6);
			for (size_t i = 0; i < sum.size (); ++i) { sum[i] += pcm[i] * .5; }
		}
		assert_graph_pcm (sum, rate, 3e-6);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (unsigned kind = 0; kind < 3; ++kind) {
		const std::string name = "StemReturn" + std::to_string (kind);
		if (kind == 0) { fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("stems.dawproject"), fixture.path (name), name, fixture.report); }
		else if (kind == 1) { fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("StemLogic/Project.aaf"), fixture.path ("StemLogic/Project.mid"), fixture.path (name), name, fixture.report); }
		else { fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("StemNative/Project.song"), fixture.path (name), name, fixture.report); }
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		unsigned count = 0;
		for (auto const& route : *fixture.session->get_routes ()) {
			auto track = std::dynamic_pointer_cast<ARDOUR::Track> (route); if (!track || route->is_auditioner ()) { continue; } ++count;
			auto regions = track->playlist ()->region_list (); CPPUNIT_ASSERT_EQUAL (size_t (1), regions->size ());
			auto region = std::dynamic_pointer_cast<AudioRegion> (regions->front ()); CPPUNIT_ASSERT (region);
			std::vector<float> pcm (size_t (frames) * 2), channel (frames), mix (frames), gain (frames);
			for (unsigned c = 0; c < 2; ++c) {
				std::fill (channel.begin (), channel.end (), 0);
				CPPUNIT_ASSERT_EQUAL (frames, region->read_at (channel.data (), mix.data (), gain.data (), 0, frames, c));
				for (samplecnt_t i = 0; i < frames; ++i) { pcm[size_t (i) * 2 + c] = channel[i]; }
			}
			verify_pcm (route->name (), pcm, 2e-6);
		}
		CPPUNIT_ASSERT_EQUAL (3u, count); AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	}
	std::cout << "Separate stems DAWproject, AAF and native song PCM save/reopen round trips passed" << std::endl;
}

void ProjectSessionTest::render_stems_guards ()
{
	Fixture fixture; make_render_fixture (fixture);
	PX::SessionExchange::ExportOptions options; options.render_stems = true; options.render_tail_seconds = 0;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("master-gain.dawproject"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("master-gain.dawproject"), Glib::FILE_TEST_EXISTS));
	fixture.session->master_out ()->gain_control ()->set_value (1, PBD::Controllable::NoGroup);
	options.render_mix = true;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("both-modes"), fixture.report, options), PX::ExchangeError);
	options.render_mix = false;
	{
		auto output = fixture.session->route_by_name ("Program")->output ();
		auto port = output->audio (0);
		auto master = fixture.session->master_out ()->input ();
		CPPUNIT_ASSERT_EQUAL (0, output->disconnect (port, master->audio (0)->name ()));
		CPPUNIT_ASSERT_EQUAL (0, output->connect (port, master->audio (1)->name ()));
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("crosswired.dawproject"), fixture.report, options), PX::ExchangeError);
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("crosswired.dawproject"), Glib::FILE_TEST_EXISTS));
		CPPUNIT_ASSERT_EQUAL (0, output->disconnect (port, master->audio (1)->name ()));
		CPPUNIT_ASSERT_EQUAL (0, output->connect (port, master->audio (0)->name ()));
	}
	{
		auto control = fixture.session->master_out ()->gain_control ();
		control->alist ()->add (Temporal::timepos_t (samplepos_t (0)), 1, false, false);
		control->set_automation_state (Play);
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("master-automation"), fixture.report, options), PX::ExchangeError);
		control->set_automation_state (Off); control->alist ()->clear ();
	}
	fixture.session->set_session_extents (Temporal::timepos_t (samplepos_t (0)), Temporal::timepos_t (samplepos_t (120 * fixture.session->sample_rate ())));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("")); RenderSourceState original (fixture);
	for (unsigned kind = 0; kind < 3; ++kind) {
		bool during = false; auto status = fixture.session->get_export_status ();
		auto cancel = [&] {
			PBD::Mutex::Lock lock (status->lock ());
			if (fixture.session->exporting () && status->processed_samples > 512 && status->processed_samples < status->total_samples) { during = true; }
			return during;
		};
		std::string destination = fixture.path ("cancel-stems-" + std::to_string (kind));
		if (kind == 0) { CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, destination, fixture.report, options, cancel), PX::ArchiveCancelled); }
		else if (kind == 1) { CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, destination, fixture.report, options, cancel), PX::ArchiveCancelled); }
		else { CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, destination, fixture.report, options, cancel), PX::ArchiveCancelled); }
		CPPUNIT_ASSERT (during); CPPUNIT_ASSERT (!Glib::file_test (destination, Glib::FILE_TEST_EXISTS)); status.reset (); original.check (fixture);
	}
	std::cout << "Separate stem cancellation, neutral master and mode guards passed" << std::endl;
}

void ProjectSessionTest::render_mix_roundtrip ()
{
	Fixture fixture; make_render_fixture (fixture);
	const unsigned rate = fixture.session->sample_rate ();
	const samplecnt_t frames = fixture.session->current_end_sample () + 1024;
	PX::SessionExchange::ExportOptions options; options.render_mix = true; options.render_tail_seconds = 1024.0 / rate;
	fixture.session->set_dirty ();
	RenderSourceState original (fixture);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("strict.dawproject"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("strict.dawproject"), Glib::FILE_TEST_EXISTS)); original.check (fixture);
	for (bool logic : {false, true}) {
		fixture.report.clear ();
		const std::string destination = fixture.path (logic ? "RenderedLogic" : "rendered.dawproject");
		if (logic) { PX::SessionExchange::export_logic (*fixture.session, destination, fixture.report, options); }
		else { PX::SessionExchange::export_dawproject (*fixture.session, destination, fixture.report, options); }
		CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Rendered; }));
		CPPUNIT_ASSERT (!std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Unsupported; }));
		original.check (fixture);
		const std::string audio = fixture.path (logic ? "logic-mix.wav" : "daw-mix.wav");
		if (logic) {
			PX::LogicProject project (Glib::build_filename (destination, "Project.aaf"), Glib::build_filename (destination, "Project.mid"));
			assert_render_model (project.project (), frames, rate); project.copy_media (project.project ().media.front (), audio);
		} else {
			auto project = PX::DawProject::read (destination); assert_render_model (project, frames, rate);
			PX::ProjectArchive (destination).copy_file (project.media.front ().path, audio);
		}
		assert_graph_pcm (read_rendered_pcm (audio, frames, rate), rate, logic ? 2e-6 : 1e-6);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (bool logic : {false, true}) {
		const std::string name = logic ? "RenderedAAFReturn" : "RenderedDAWReturn";
		fixture.session = logic ? PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("RenderedLogic/Project.aaf"), fixture.path ("RenderedLogic/Project.mid"), fixture.path (name), name, fixture.report)
		                        : PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("rendered.dawproject"), fixture.path (name), name, fixture.report);
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		unsigned tracks = 0;
		for (auto const& route : *fixture.session->get_routes ()) {
			auto track = std::dynamic_pointer_cast<ARDOUR::Track> (route); if (!track || route->is_auditioner ()) { continue; }
			++tracks; CPPUNIT_ASSERT_DOUBLES_EQUAL (1, route->gain_control ()->get_value (), 1e-8);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, route->pan_azimuth_control ()->get_value (), 1e-8);
			auto regions = track->playlist ()->region_list (); CPPUNIT_ASSERT_EQUAL (size_t (1), regions->size ());
			auto region = std::dynamic_pointer_cast<AudioRegion> (regions->front ()); CPPUNIT_ASSERT (region);
			CPPUNIT_ASSERT_EQUAL (samplepos_t (0), region->position_sample ()); CPPUNIT_ASSERT_EQUAL (frames, region->length_samples ());
			std::vector<float> pcm (size_t (frames) * 2), channel (frames), mix (frames), gain (frames);
			for (unsigned c = 0; c < 2; ++c) {
				std::fill (channel.begin (), channel.end (), 0);
				CPPUNIT_ASSERT_EQUAL (frames, region->read_at (channel.data (), mix.data (), gain.data (), 0, frames, c));
				for (samplecnt_t i = 0; i < frames; ++i) { pcm[size_t (i) * 2 + c] = channel[i]; }
			}
			assert_graph_pcm (pcm, rate, logic ? 2e-6 : 1e-6);
		}
		CPPUNIT_ASSERT_EQUAL (1u, tracks);
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	}
	std::cout << "Rendered mix DAWproject and AAF/MIDI PCM round trips passed" << std::endl;
}

void ProjectSessionTest::render_pending_buffering ()
{
	Fixture fixture; make_render_fixture (fixture, true);
	const unsigned rate = fixture.session->sample_rate ();
	const samplecnt_t frames = fixture.session->current_end_sample ();
	PX::SessionExchange::ExportOptions options; options.render_mix = true; options.render_tail_seconds = 0;
	fixture.session->set_clean (); RenderSourceState original (fixture);
	for (unsigned i = 0; i < 8; ++i) {
		// Both requests require the process lock in the butler. Export must
		// let that work finish before locking the audio graph for its seek.
		fixture.session->adjust_playback_buffering ();
		fixture.session->adjust_capture_buffering ();
		options.render_mix = i % 2 == 0; options.render_stems = !options.render_mix;
		auto destination = fixture.path ("buffering-" + std::to_string (i) + ".dawproject");
		PX::SessionExchange::export_dawproject (*fixture.session, destination, fixture.report, options);
		original.check (fixture);
		auto output = PX::DawProject::read (destination); assert_render_model (output, frames, rate);
	}
	// Prove normal process callbacks resume, rather than inspecting a flag.
	const samplepos_t before = fixture.session->transport_sample ();
	fixture.session->request_roll ();
	wait_render_state ([&] { return fixture.session->transport_sample () > before; });
	fixture.session->request_stop ();
	wait_render_state ([&] { return fixture.session->transport_stopped (); });
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	std::cout << "Rendered export with pending playback/capture buffering and processing restoration passed" << std::endl;
}

void ProjectSessionTest::render_mix_automation ()
{
	Fixture fixture; make_render_fixture (fixture, true);
	const unsigned rate = fixture.session->sample_rate ();
	const samplecnt_t frames = fixture.session->current_end_sample ();
	PX::SessionExchange::ExportOptions options; options.render_mix = true; options.render_tail_seconds = 0;
	fixture.session->set_clean (); RenderSourceState original (fixture);
	for (bool stems : {false, true}) {
		options.render_mix = !stems; options.render_stems = stems;
		for (bool logic : {false, true}) {
			const std::string destination = fixture.path ((stems ? "stem-" : "mix-") + std::string (logic ? "AutomationLogic" : "automation-render.dawproject"));
			if (logic) { PX::SessionExchange::export_logic (*fixture.session, destination, fixture.report, options); }
			else { PX::SessionExchange::export_dawproject (*fixture.session, destination, fixture.report, options); }
			original.check (fixture);
			const std::string audio = fixture.path ((stems ? "stem-" : "mix-") + std::string (logic ? "automation-logic.wav" : "automation-daw.wav"));
			if (logic) {
				PX::LogicProject project (Glib::build_filename (destination, "Project.aaf"), Glib::build_filename (destination, "Project.mid"));
				assert_render_model (project.project (), frames, rate); project.copy_media (project.project ().media.front (), audio);
			} else {
				auto project = PX::DawProject::read (destination); assert_render_model (project, frames, rate);
				PX::ProjectArchive (destination).copy_file (project.media.front ().path, audio);
			}
			auto pcm = read_rendered_pcm (audio, frames, rate);
			/* The native 25 Hz gain smoother uses float state. Its rounding error
			 * is bounded by epsilon / pole coefficient, plus 24-bit quantization. */
			const double gain_error = .25 * std::numeric_limits<float>::epsilon () * rate / 156.0 + 1.0 / 8388608;
			for (auto const& window : std::vector<std::tuple<double, double, double>> {{.25, 0, 0}, {1.15, .125 * .25, 0}, {1.8, 0, .25 * .75}}) {
				samplepos_t at = std::llround (std::get<0> (window) * rate);
				for (samplepos_t i = at; i < at + 128; ++i) {
					CPPUNIT_ASSERT_DOUBLES_EQUAL (std::get<1> (window), pcm[size_t (i) * 2], std::get<1> (window) == 0 ? 1e-7 : gain_error);
					CPPUNIT_ASSERT_DOUBLES_EQUAL (std::get<2> (window), pcm[size_t (i) * 2 + 1], std::get<2> (window) == 0 ? 1e-7 : gain_error);
				}
			}
		}
	}

	std::cout << "Rendered gain/pan automation PCM and source restoration passed" << std::endl;
}

void ProjectSessionTest::render_mix_cancel_and_guards ()
{
	Fixture fixture; make_render_fixture (fixture);
	const unsigned rate = fixture.session->sample_rate ();
	PX::SessionExchange::ExportOptions options; options.render_mix = true; options.render_tail_seconds = 0;
	fixture.session->set_session_extents (Temporal::timepos_t (samplepos_t (rate / 2)), Temporal::timepos_t (samplepos_t (120 * rate)));
	{
		auto route = fixture.session->route_by_name ("Stereo");
		route->gain_control ()->set_automation_state (Write);
		auto pan = route->pan_azimuth_control (); pan->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .5, false, false);
		pan->set_automation_state (Touch);
	}
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("")); fixture.session->set_dirty ();
	RenderSourceState original (fixture);
	{
		struct ExportPreroll {
			ExportPreroll () : prior (Config->get_export_preroll ()) { Config->set_export_preroll (60.0f); }
			~ExportPreroll () { Config->set_export_preroll (prior); }
			float prior;
		} preroll;
		auto early_status = fixture.session->get_export_status ();
		bool cancelled_before_audio = false;
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("cancelled-preroll.dawproject"), fixture.report, options, [&] {
			PBD::Mutex::Lock lock (early_status->lock ());
			if (fixture.session->exporting () && early_status->processed_samples == 0) { cancelled_before_audio = true; }
			return cancelled_before_audio;
		}), PX::ArchiveCancelled);
		CPPUNIT_ASSERT (cancelled_before_audio);
		{
			PBD::Mutex::Lock lock (early_status->lock ());
			CPPUNIT_ASSERT_EQUAL (samplecnt_t (0), samplecnt_t (early_status->processed_samples));
		}
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("cancelled-preroll.dawproject"), Glib::FILE_TEST_EXISTS));
		CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
		original.check (fixture);
	}
	auto status = fixture.session->get_export_status ();
	bool cancelled_during_processing = false;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("cancelled-render.dawproject"), fixture.report, options, [&] {
		PBD::Mutex::Lock lock (status->lock ());
		if (fixture.session->exporting () && status->processed_samples > 512 && status->processed_samples < status->total_samples) { cancelled_during_processing = true; }
		return cancelled_during_processing;
	}), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (cancelled_during_processing);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("cancelled-render.dawproject"), Glib::FILE_TEST_EXISTS)); original.check (fixture);
	status.reset ();
	fixture.session->maybe_enable_record (false); CPPUNIT_ASSERT (fixture.session->get_record_enabled ());
	RenderSourceState armed (fixture);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("armed-render"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("armed-render"), Glib::FILE_TEST_EXISTS)); armed.check (fixture);
	fixture.session->disable_record (false, true);
	struct LoopMode {
		LoopMode () : prior (Config->get_loop_is_mode ()) { Config->set_loop_is_mode (true); }
		~LoopMode () { Config->set_loop_is_mode (prior); }
		bool prior;
	} loop_mode;
	fixture.session->request_play_loop (true, false);
	wait_render_state ([&] { return fixture.session->get_play_loop () && fixture.session->transport_stopped () && !fixture.session->locate_pending () && !fixture.session->locate_initiated (); });
	RenderSourceState looping (fixture);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("loop-render.dawproject"), fixture.report, options), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("loop-render.dawproject"), Glib::FILE_TEST_EXISTS)); looping.check (fixture);
	fixture.session->request_play_loop (false, false);
	wait_render_state ([&] { return !fixture.session->get_play_loop () && fixture.session->transport_stopped () && !fixture.session->locate_pending () && !fixture.session->locate_initiated (); });
	for (double invalid : {-1.0, 601.0, std::numeric_limits<double>::infinity ()}) {
		options.render_tail_seconds = invalid; RenderSourceState before (fixture);
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("invalid-tail.dawproject"), fixture.report, options), PX::ExchangeError);
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("invalid-tail.dawproject"), Glib::FILE_TEST_EXISTS)); before.check (fixture);
	}
	options.render_tail_seconds = 0;
	RenderSourceState before_loss (fixture);
	auto loss_status = fixture.session->get_export_status ();
	bool lost_engine = false;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("engine-loss.dawproject"), fixture.report, options, [&] {
		bool progressed;
		{
			PBD::Mutex::Lock lock (loss_status->lock ());
			progressed = fixture.session->exporting () && loss_status->processed_samples > 512 && loss_status->processed_samples < loss_status->total_samples;
		}
		if (progressed && !lost_engine) {
			lost_engine = true;
			CPPUNIT_ASSERT_EQUAL (0, AudioEngine::instance ()->stop ());
		}
		return false;
	}), PX::ExchangeError);
	CPPUNIT_ASSERT (lost_engine);
	CPPUNIT_ASSERT (!fixture.session->exporting ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("engine-loss.dawproject"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	loss_status.reset ();
	{
		auto stopped_handler = fixture.session->get_export_handler ();
		auto stopped_status = fixture.session->get_export_status ();
		CPPUNIT_ASSERT (stopped_handler->do_export () != 0);
		PBD::Mutex::Lock lock (stopped_status->lock ());
		CPPUNIT_ASSERT (stopped_status->aborted ());
		CPPUNIT_ASSERT (stopped_status->errors ());
		CPPUNIT_ASSERT (!stopped_status->running ());
	}
	CPPUNIT_ASSERT_EQUAL (0, AudioEngine::instance ()->start ());
	wait_render_state ([&] { return fixture.session->transport_sample () == before_loss.position && fixture.session->transport_stopped () && !fixture.session->locate_pending () && !fixture.session->locate_initiated (); });
	before_loss.check (fixture, true);
	std::cout << "Rendered cancellation and source-state guards passed" << std::endl;
}

void ProjectSessionTest::vst3_plugin_exchange ()
{
	{
		Fixture fixture;
		auto project = example (fixture);
		PX::PluginDevice missing;
		missing.id = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";
		missing.name = "Missing VST3";
		missing.role = "audioFX";
		missing.state_path = "plugins/missing.vstpreset";
		missing.state = "VST3";
		missing.state.append ("\x01\0\0\0", 4);
		missing.state += missing.id;
		missing.state.append ("\x30\0\0\0\0\0\0\0", 8);
		missing.state += "List";
		missing.state.append ("\x01\0\0\0", 4);
		missing.state += "Comp";
		missing.state.append ("\x30\0\0\0\0\0\0\0", 8);
		missing.state.append (8, '\0');
		project.tracks[2].plugins.push_back (missing);
		package (fixture, project);
		auto unavailable = PX::SessionExchange::unavailable_vst3_plugins (PX::DawProject::read (fixture.path ("input.dawproject")));
		CPPUNIT_ASSERT_EQUAL (size_t (1), unavailable.size ());
		CPPUNIT_ASSERT (unavailable.front ().disposition == PX::Disposition::Unsupported);
		CPPUNIT_ASSERT (unavailable.front ().detail.find ("Render this track") != std::string::npos);
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
		                                                            fixture.path ("Unavailable"), "Exchange", fixture.report), PX::ExchangeError);
		CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Unavailable"), Glib::FILE_TEST_EXISTS));
		fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
		                                                           fixture.path ("Partial"), "Partial", fixture.report,
		                                                           PX::CancelCheck (), true);
		CPPUNIT_ASSERT (fixture.session);
		CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
			return item.disposition == PX::Disposition::Unsupported && item.detail.find ("VST3 plugin is not installed") != std::string::npos;
		}));
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Partial"), "Partial", nullptr, "", true));
		PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("partial-no-plugin.dawproject"));
		auto returned = PX::DawProject::read (fixture.path ("partial-no-plugin.dawproject"));
		CPPUNIT_ASSERT (track_named (returned, "Stereo").plugins.empty ());
		CPPUNIT_ASSERT_EQUAL (size_t (2), track_named (returned, "Stereo").clips.size ());
		CPPUNIT_ASSERT_EQUAL (size_t (1), track_named (returned, "Notes").clips.size ());
	}

	/* Set this only in a private run with an installed VST3 effect. The normal
	 * session suite tests the missing-plugin transaction without host plugins. */
	const char* requested = std::getenv ("ARDOUR_VST3_TEST_PLUGIN");
	if (!requested || !*requested) { return; }
	Fixture fixture;
	PluginManager::instance ().refresh (false);
	PluginInfoPtr info;
	for (auto const& candidate : PluginManager::instance ().vst3_plugin_info ()) {
		if (candidate->name == requested) { info = candidate; break; }
	}
	CPPUNIT_ASSERT_MESSAGE ("Requested VST3 test plugin was not discovered", info);
	CPPUNIT_ASSERT (info->is_effect ());
	auto project = example (fixture);
	for (auto& track : project.tracks) {
		if (!track.master) { track.destination = "master"; }
		for (auto& clip : track.clips) { for (auto& note : clip.notes) { note.channel = 0; note.release_velocity = 64.0 / 127; } }
	}
	package (fixture, project);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
	                                                       fixture.path ("Original"), "Exchange", fixture.report);
	std::shared_ptr<Route> route;
	for (auto const& item : *fixture.session->get_routes ()) { if (item->name () == "Stereo") { route = item; break; } }
	CPPUNIT_ASSERT (route);
	auto plugin = info->load (*fixture.session);
	CPPUNIT_ASSERT (plugin);
	auto insert = std::make_shared<PluginInsert> (*fixture.session, *fixture.session, plugin);
	auto reject_export = [&] (const std::string& detail, const std::string& context) {
		fixture.report.clear ();
		bool rejected = false;
		try { PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("unsupported.dawproject"), fixture.report); }
		catch (const PX::ExchangeError&) { rejected = true; }
		CPPUNIT_ASSERT_MESSAGE (context, rejected);
		CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [&] (const PX::Diagnostic& item) {
			return item.disposition == PX::Disposition::Unsupported && item.detail.find (detail) != std::string::npos;
		}));
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("unsupported.dawproject"), Glib::FILE_TEST_EXISTS));
	};
	CPPUNIT_ASSERT_EQUAL (0, route->add_processor (insert, PostFader));
	reject_export ("must precede the fader and sends", "Post-fader plugin");
	CPPUNIT_ASSERT_EQUAL (0, route->remove_processor (insert));
	insert.reset ();
	plugin = info->load (*fixture.session);
	CPPUNIT_ASSERT (plugin);
	insert = std::make_shared<PluginInsert> (*fixture.session, *fixture.session, plugin);
	CPPUNIT_ASSERT_EQUAL (0, route->add_processor (insert, PreFader));
	CPPUNIT_ASSERT_EQUAL (uint32_t (1), insert->get_count ());
	auto input = insert->input_map (0), output = insert->output_map (0), thru = insert->thru_map ();
	for (bool output_pin : {false, true}) {
		auto changed = output_pin ? output : input;
		changed.unset (DataType::AUDIO, 0);
		changed.unset (DataType::AUDIO, 1);
		changed.set (DataType::AUDIO, 0, 1);
		changed.set (DataType::AUDIO, 1, 0);
		if (output_pin) { insert->set_output_map (0, changed); }
		else { insert->set_input_map (0, changed); }
		CPPUNIT_ASSERT_MESSAGE (output_pin ? "Output swap retained" : "Input swap retained", changed == (output_pin ? insert->output_map (0) : insert->input_map (0)));
		CPPUNIT_ASSERT (changed != (output_pin ? output : input));
		reject_export ("Custom VST3 pin routing", output_pin ? "Swapped plugin outputs" : "Swapped plugin inputs");
		if (output_pin) { insert->set_output_map (0, output); }
		else { insert->set_input_map (0, input); }
	}
	ChanMapping bypass; bypass.set (DataType::AUDIO, 0, 0);
	auto disconnected = output; disconnected.unset (DataType::AUDIO, 0);
	insert->set_output_map (0, disconnected);
	insert->set_thru_map (bypass);
	CPPUNIT_ASSERT_EQUAL (uint32_t (1), insert->thru_map ().n_total ());
	reject_export ("Custom VST3 pin routing", "Plugin thru routing");
	insert->set_thru_map (thru);
	insert->set_output_map (0, output);
	uint32_t changed_parameter = 0;
	float changed_value = 0, original_value = 0;
	bool altered = false;
	for (uint32_t index = 0; index < plugin->parameter_count (); ++index) {
		bool ok = false;
		uint32_t parameter = plugin->nth_parameter (index, ok);
		ParameterDescriptor descriptor;
		if (!ok || !plugin->parameter_is_input (parameter) || plugin->get_parameter_descriptor (parameter, descriptor) ||
		    descriptor.toggled || descriptor.enumeration || descriptor.integer_step || descriptor.upper <= descriptor.lower) { continue; }
		auto control = insert->automation_control (Evoral::Parameter (PluginAutomation, 0, parameter));
		if (!control) { continue; }
		float original = plugin->get_parameter (parameter);
		float target = descriptor.lower + (descriptor.upper - descriptor.lower) * (descriptor.to_interface (original) < .5 ? .73 : .27);
		control->set_value (target, PBD::Controllable::NoGroup);
		changed_value = plugin->get_parameter (parameter);
		if (std::fabs (changed_value - original) > 1e-5) { changed_parameter = parameter; original_value = original; altered = true; break; }
	}
	CPPUNIT_ASSERT_MESSAGE ("Real plugin test needs a nondefault continuous parameter", altered);
	auto parameter_value = [&] () {
		bool found = false;
		float current = 0;
		for (auto const& item : *fixture.session->get_routes ()) {
			if (item->name () != "Stereo") { continue; }
			item->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
				if (auto value = std::dynamic_pointer_cast<PluginInsert> (weak.lock ())) {
					CPPUNIT_ASSERT (!found);
					current = value->plugin (0)->get_parameter (changed_parameter);
					found = true;
				}
			});
		}
		CPPUNIT_ASSERT (found);
		return current;
	};
	// Let the dummy engine deliver the parameter change to the plugin DSP.
	g_usleep (100000);
	insert->deactivate ();
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("Exchange"));
	plugin.reset ();
	insert.reset ();
	route.reset ();
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Original"), "Exchange", NULL, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	// The plugin may quantize a requested parameter. Use its saved native
	// value as the exchange reference, and still prove it is nondefault.
	changed_value = parameter_value ();
	CPPUNIT_ASSERT (std::fabs (changed_value - original_value) > 1e-5);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("with-plugin.dawproject"));
	auto exported = PX::DawProject::read (fixture.path ("with-plugin.dawproject"));
	auto const& devices = track_named (exported, "Stereo").plugins;
	CPPUNIT_ASSERT_EQUAL (size_t (1), devices.size ());
	CPPUNIT_ASSERT_EQUAL (PX::canonical_vst3_id (info->unique_id), devices[0].id);
	CPPUNIT_ASSERT (!devices[0].enabled);
	PX::validate_vst3_state (devices[0].id, devices[0].state);
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("with-plugin.dawproject"),
	                                                       fixture.path ("Returned"), "Exchange", fixture.report);
	CPPUNIT_ASSERT (fixture.session);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("returned.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("returned.dawproject"));
	CPPUNIT_ASSERT_EQUAL (devices[0].id, track_named (returned, "Stereo").plugins.at (0).id);
	CPPUNIT_ASSERT_EQUAL (devices[0].state, track_named (returned, "Stereo").plugins.at (0).state);
	CPPUNIT_ASSERT (!track_named (returned, "Stereo").plugins.at (0).enabled);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (changed_value, parameter_value (), 1e-5);
	bool reactivated = false;
	for (auto const& item : *fixture.session->get_routes ()) { if (item->name () == "Stereo") {
		item->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			if (auto value = std::dynamic_pointer_cast<PluginInsert> (weak.lock ())) { value->activate (); reactivated = true; }
		});
	} }
	CPPUNIT_ASSERT (reactivated);
	PX::SessionExchange::ExportOptions native_options;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("with-plugin-song"), fixture.report, native_options);
	auto native = PX::StudioOneSong::read (fixture.path ("with-plugin-song/Project.song"));
	auto const& native_devices = track_named (native.project, "Stereo").plugins;
	CPPUNIT_ASSERT_EQUAL (size_t (1), native_devices.size ());
	CPPUNIT_ASSERT_EQUAL (devices[0].id, native_devices[0].id);
	CPPUNIT_ASSERT_EQUAL (devices[0].state, native_devices[0].state);
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("with-plugin-song/Project.song"),
	                                                        fixture.path ("NativePluginReturn"), "Exchange", fixture.report);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (changed_value, parameter_value (), 1e-5);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-plugin-return.dawproject"));
	auto native_returned = PX::DawProject::read (fixture.path ("native-plugin-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (devices[0].state, track_named (native_returned, "Stereo").plugins.at (0).state);
	/* Studio Pro saves the same insert rack on bus and main-output channels.
	 * Prove that an actual installed VST3 survives both routes, Ardour's
	 * session save/reopen, and return to the neutral exchange format. */
	auto stereo_track = std::find_if (native.project.tracks.begin (), native.project.tracks.end (), [] (const PX::Track& t) { return t.name == "Stereo"; });
	auto master_track = std::find_if (native.project.tracks.begin (), native.project.tracks.end (), [] (const PX::Track& t) { return t.master; });
	CPPUNIT_ASSERT (stereo_track != native.project.tracks.end () && master_track != native.project.tracks.end ());
	PX::PluginDevice bus_device = native_devices[0];
	bus_device.state_path = "plugins/bus.vstpreset";
	PX::PluginDevice master_device = bus_device;
	master_device.state_path = "plugins/master.vstpreset";
	std::string master_id = master_track->id, master_name = master_track->name;
	stereo_track->destination = "vst3-bus";
	master_track->plugins.push_back (master_device);
	PX::Track bus; bus.id = "vst3-bus"; bus.name = "VST3 Bus"; bus.kind = PX::TrackKind::Bus;
	bus.destination = master_id; bus.plugins.push_back (bus_device);
	native.project.tracks.push_back (bus);
	std::string bus_song = fixture.path ("bus-master-plugins.song");
	PX::StudioOneSong::write (native, bus_song, bus_song);
	auto bus_native = PX::StudioOneSong::read (bus_song);
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (bus_native.project, "VST3 Bus").plugins.at (0).state);
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (bus_native.project, master_name).plugins.at (0).state);
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), bus_song,
	                                                        fixture.path ("BusMasterPluginReturn"), "Exchange", fixture.report);
	CPPUNIT_ASSERT (fixture.session->route_by_name ("VST3 Bus"));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("Exchange"));
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("BusMasterPluginReturn"), "Exchange", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	fixture.report.clear ();
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("bus-master-native-return"), fixture.report, native_options);
	auto native_bus_returned = PX::StudioOneSong::read (fixture.path ("bus-master-native-return/Project.song"));
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (native_bus_returned.project, "VST3 Bus").plugins.at (0).state);
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (native_bus_returned.project, master_name).plugins.at (0).state);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("bus-master-plugin-return.dawproject"));
	auto bus_returned = PX::DawProject::read (fixture.path ("bus-master-plugin-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (bus_returned, "VST3 Bus").plugins.at (0).state);
	CPPUNIT_ASSERT_EQUAL (bus_device.state, track_named (bus_returned, master_name).plugins.at (0).state);
	if (const char* external = std::getenv ("ARDOUR_STUDIO_PRO_VST3_TEST_PROJECT")) {
		if (!*external) { return; }
		auto assert_supported = [] (const std::vector<PX::Diagnostic>& report) {
			std::string unsupported;
			for (auto const& diagnostic : report) {
				if (diagnostic.disposition == PX::Disposition::Unsupported) {
					unsupported += "\n" + diagnostic.object + ": " + diagnostic.detail;
				}
			}
			CPPUNIT_ASSERT_MESSAGE ("Unsupported Studio Pro return features:" + unsupported, unsupported.empty ());
		};
		auto external_project = PX::DawProject::read (external);
		assert_supported (external_project.report);
		auto const& external_devices = track_named (external_project, "Stereo").plugins;
		CPPUNIT_ASSERT_EQUAL (size_t (1), external_devices.size ());
		CPPUNIT_ASSERT_EQUAL (devices[0].id, external_devices[0].id);
		AudioEngine::instance ()->remove_session ();
		fixture.session.reset ();
		fixture.report.clear ();
		fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), external,
		                                                       fixture.path ("StudioReturned"), "Exchange", fixture.report);
		CPPUNIT_ASSERT (fixture.session);
		assert_supported (fixture.report);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (changed_value, parameter_value (), 1e-5);
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("Exchange"));
		AudioEngine::instance ()->remove_session ();
		fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("StudioReturned"), "Exchange", nullptr, "", true));
		AudioEngine::instance ()->set_session (fixture.session.get ());
		CPPUNIT_ASSERT_DOUBLES_EQUAL (changed_value, parameter_value (), 1e-5);
		fixture.report.clear ();
		PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("studio-returned.dawproject"), fixture.report);
		assert_supported (fixture.report);
		auto studio_returned = PX::DawProject::read (fixture.path ("studio-returned.dawproject"));
		auto const& studio_devices = track_named (studio_returned, "Stereo").plugins;
		CPPUNIT_ASSERT_EQUAL (size_t (1), studio_devices.size ());
		CPPUNIT_ASSERT_EQUAL (external_devices[0].id, studio_devices[0].id);
		CPPUNIT_ASSERT_EQUAL (external_devices[0].enabled, studio_devices[0].enabled);
		std::cout << "Studio Pro VST3 return: import/save/reopen/export passed" << std::endl;
	}
}

void ProjectSessionTest::native_song_app_authored_roundtrip ()
{
	const char* source = std::getenv ("ARDOUR_STUDIO_PRO_VST3_TEST_SONG");
	if (!source || !*source) { return; }
	Fixture fixture;
	PluginManager::instance ().refresh (false);
	auto supported = [] (const std::vector<PX::Diagnostic>& report) {
		std::string details;
		for (auto const& item : report) {
			if (item.disposition == PX::Disposition::Unsupported) { details += "\n" + item.object + ": " + item.detail; }
		}
		CPPUNIT_ASSERT_MESSAGE ("Unsupported app-authored song content:" + details, details.empty ());
	};
	auto original = PX::StudioOneSong::read (source);
	supported (original.project.report);
	auto const& source_audio = track_named (original.project, "Stereo");
	auto const& source_midi = track_named (original.project, "Notes");
	CPPUNIT_ASSERT_EQUAL (size_t (1), source_audio.plugins.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), source_audio.clips.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), source_midi.clips.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), source_midi.clips[0].notes.size ());
	PX::PluginDevice original_plugin = source_audio.plugins[0];
	auto verify = [&] (const PX::Project& project) {
		auto const& folder = track_named (project, "Band");
		auto const& audio = track_named (project, "Stereo");
		auto const& midi = track_named (project, "Notes");
		CPPUNIT_ASSERT_EQUAL (folder.id, audio.parent);
		CPPUNIT_ASSERT_EQUAL (folder.id, midi.parent);
		CPPUNIT_ASSERT_EQUAL (size_t (1), audio.plugins.size ());
		CPPUNIT_ASSERT_EQUAL (original_plugin.id, audio.plugins[0].id);
		CPPUNIT_ASSERT_EQUAL (original_plugin.state, audio.plugins[0].state);
		CPPUNIT_ASSERT_EQUAL (size_t (1), audio.clips.size ());
		CPPUNIT_ASSERT_EQUAL (size_t (1), midi.clips.size ());
		CPPUNIT_ASSERT_DOUBLES_EQUAL (source_audio.clips[0].duration, audio.clips[0].duration, 1e-6);
		CPPUNIT_ASSERT_EQUAL (size_t (2), midi.clips[0].notes.size ());
		for (size_t i = 0; i < 2; ++i) {
			CPPUNIT_ASSERT_EQUAL (source_midi.clips[0].notes[i].key, midi.clips[0].notes[i].key);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (source_midi.clips[0].notes[i].time, midi.clips[0].notes[i].time, 1e-6);
		}
	};
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), source,
	                                                        fixture.path ("AppAuthored"), "AppAuthored", fixture.report);
	supported (fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("AppAuthored"));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("AppAuthored"), "AppAuthored", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	PX::SessionExchange::ExportOptions options;
	fixture.report.clear ();
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("NativeReturn"), fixture.report, options);
	supported (fixture.report);
	auto native_return = PX::StudioOneSong::read (fixture.path ("NativeReturn/Project.song"));
	supported (native_return.project.report);
	verify (native_return.project);
	fixture.report.clear ();
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("app-authored-return.dawproject"), fixture.report);
	supported (fixture.report);
	auto neutral_return = PX::DawProject::read (fixture.path ("app-authored-return.dawproject"));
	supported (neutral_return.report);
	verify (neutral_return);
	std::shared_ptr<PluginInsert> live_insert;
	for (auto const& route : *fixture.session->get_routes ()) {
		if (route->name () != "Stereo") { continue; }
		route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			if (auto insert = std::dynamic_pointer_cast<PluginInsert> (weak.lock ())) { live_insert = insert; }
		});
	}
	CPPUNIT_ASSERT (live_insert);
	auto live_plugin = live_insert->plugin (0);
	bool changed = false;
	for (uint32_t index = 0; index < live_plugin->parameter_count (); ++index) {
		bool ok = false;
		uint32_t parameter = live_plugin->nth_parameter (index, ok);
		ParameterDescriptor descriptor;
		if (!ok || !live_plugin->parameter_is_input (parameter) || live_plugin->get_parameter_descriptor (parameter, descriptor) ||
		    descriptor.toggled || descriptor.enumeration || descriptor.integer_step || descriptor.upper <= descriptor.lower) { continue; }
		auto control = live_insert->automation_control (Evoral::Parameter (PluginAutomation, 0, parameter));
		if (!control) { continue; }
		float before = live_plugin->get_parameter (parameter);
		float target = descriptor.lower + (descriptor.upper - descriptor.lower) * (descriptor.to_interface (before) < .5 ? .73 : .27);
		control->set_value (target, PBD::Controllable::NoGroup);
		if (std::fabs (live_plugin->get_parameter (parameter) - before) > 1e-5) { changed = true; break; }
	}
	CPPUNIT_ASSERT_MESSAGE ("App-authored VST3 needs an editable continuous parameter", changed);
	g_usleep (100000);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state ("AppAuthored"));
	live_plugin.reset (); live_insert.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("AppAuthored"), "AppAuthored", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	fixture.report.clear ();
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("app-authored-edited.dawproject"), fixture.report);
	supported (fixture.report);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Transformed && item.detail.find ("VST3 preset wrapper metadata differs") != std::string::npos;
	}));
	auto edited_return = PX::DawProject::read (fixture.path ("app-authored-edited.dawproject"));
	CPPUNIT_ASSERT (track_named (edited_return, "Stereo").plugins.at (0).state != original_plugin.state);
	std::cout << "App-authored Studio Pro song survived import, save/reopen and both native and neutral return exports" << std::endl;
}

void ProjectSessionTest::region_fades_roundtrip ()
{
	Fixture fixture;
	auto input = example (fixture);
	// Constant, distinct channel levels make the processed PCM a direct
	// measurement of the complete gain/fade curve, including abrupt edges.
	{
		SF_INFO info = {}; info.channels = 2; info.samplerate = input.media[0].sample_rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		SNDFILE* file = sf_open (fixture.path ("source.wav").c_str (), SFM_WRITE, &info); CPPUNIT_ASSERT (file);
		std::vector<float> pcm (info.samplerate * 4);
		for (size_t i = 0; i < pcm.size (); ++i) { pcm[i] = i % 2 ? .25 : .125; }
		CPPUNIT_ASSERT_EQUAL (sf_count_t (info.samplerate * 2), sf_writef_float (file, pcm.data (), info.samplerate * 2));
		CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
	}
	input.tracks.erase (input.tracks.begin () + 1); input.tracks.pop_back ();
	auto& lane = input.tracks[1]; lane.parent.clear (); lane.destination = "master"; lane.pan = .5;
	auto clip = lane.clips.front (); lane.clips.clear ();
	for (unsigned i = 0; i < 12; ++i) { clip.name = "Fade " + std::to_string (i); clip.position.value = 1 + i; lane.clips.push_back (clip); }
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Fades"), "Fades", fixture.report);
	const samplecnt_t rate = fixture.session->sample_rate ();
	auto render = [&] (std::shared_ptr<AudioRegion> audio, samplecnt_t block_size = 256) {
		std::vector<float> output (audio->length_samples () * 2, 0);
		std::vector<float> mix (block_size), gain (block_size);
		for (unsigned channel = 0; channel < 2; ++channel) {
			for (samplecnt_t at = 0; at < audio->length_samples (); at += block_size) {
				auto count = std::min (block_size, audio->length_samples () - at);
				CPPUNIT_ASSERT_EQUAL (count, audio->read_at (output.data () + channel * audio->length_samples () + at, mix.data (), gain.data (), audio->position_sample () + at, count, channel));
			}
		}
		return output;
	};
	std::map<std::string, std::vector<float>> expected;
	{
		auto native = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name (lane.name));
		auto regions = native->playlist ()->region_list ();
		unsigned i = 0;
		for (auto const& region : *regions) {
			auto audio = std::dynamic_pointer_cast<AudioRegion> (region); CPPUNIT_ASSERT (audio);
			audio->set_scale_amplitude (i == 11 ? 0 : 1.7);
			if (i < 10) {
				audio->set_opaque (i >= 5);
				audio->set_fade_in (static_cast<FadeShape> (i % 5), rate * 35 / 100);
				audio->set_fade_out (static_cast<FadeShape> (i % 5), rate * 35 / 100);
				audio->set_fade_in_active (true); audio->set_fade_out_active (true);
				auto envelope = audio->envelope (); envelope->clear (); envelope->freeze ();
				for (auto p : std::vector<std::pair<double, double>> {{0, .8}, {.15, 1.4}, {.35, .3}, {.5, 1}}) {
					envelope->fast_simple_add (Temporal::timepos_t (samplepos_t (std::llround (p.first * rate))), p.second);
				}
				envelope->thaw (); audio->set_envelope_active (true);
			}
			auto first = render (audio), second = render (audio, 73);
			CPPUNIT_ASSERT_EQUAL (first.size (), second.size ());
			for (size_t frame = 0; frame < first.size (); ++frame) { CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE ("Fade block-size invariance " + std::to_string (i) + " frame " + std::to_string (frame), first[frame], second[frame], 1e-6); }
			expected[audio->name ()] = std::move (first); ++i;
		}
	}
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Fades"), "Fades", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("fades.dawproject"), fixture.report);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.detail.find ("one-sample gain transition") != std::string::npos; }));
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("FadeAAF"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (bool logic : {false, true}) {
		std::string name = logic ? "AAFReturn" : "DAWReturn";
		fixture.session = logic ? PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("FadeAAF/Project.aaf"), fixture.path ("FadeAAF/Project.mid"), fixture.path (name), name, fixture.report)
		                        : PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("fades.dawproject"), fixture.path (name), name, fixture.report);
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		{
			auto native = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name (lane.name)); CPPUNIT_ASSERT (native);
			auto regions = native->playlist ()->region_list ();
			CPPUNIT_ASSERT_EQUAL (expected.size (), regions->size ());
			for (auto const& region : *regions) {
				auto audio = std::dynamic_pointer_cast<AudioRegion> (region); auto actual = render (audio);
				// AAF clip names currently use source/subclip names. Placement is stable.
				std::string key = "Fade " + std::to_string (int (std::llround (double (audio->position_sample ()) / rate)) - 1);
				auto const& reference = expected.at (key); CPPUNIT_ASSERT_EQUAL (reference.size (), actual.size ());
				for (size_t i = 0; i < actual.size (); ++i) {
					auto frame = i % audio->length_samples ();
					bool edge = frame == size_t (rate * 15 / 100 - 1) || frame == size_t (rate * 35 / 100 - 1);
					if (edge) {
						// Native read_at switches fade contributions on sample
						// boundaries. The shared linear envelope explicitly uses
						// a one-sample transition there, never an overshoot.
						CPPUNIT_ASSERT (actual[i] >= std::min (reference[i - 1], reference[i + 1]) - .0002);
						CPPUNIT_ASSERT (actual[i] <= std::max (reference[i - 1], reference[i + 1]) + .0002);
					} else { CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE (name + " " + key + " sample " + std::to_string (i), reference[i], actual[i], .0002); }
				}
			}
		}
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	}
	std::cout << "Native fade and envelope playback is block-size independent across saved exchange returns" << std::endl;
}

void ProjectSessionTest::mixer_automation_roundtrip ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 1);
	input.tracks.pop_back ();
	input.tracks[1].parent.clear (); input.tracks[1].destination = "master"; input.tracks[1].pan = .5;
	input.tempos = {{PX::Time (0), 100, true}, {PX::Time (4), 140, false}, {PX::Time (6), 80, false}};
	input.tracks[1].volume_automation.points = {{0, .25, true}, {2, .75, false}, {4, .5, true}, {8, .9, false}};
	PX::Track mono = input.tracks[1]; mono.id = "mono"; mono.name = "Mono"; mono.channels = 1;
	mono.clips.resize (1); mono.clips[0].media = "mono-media"; mono.clips[0].name = "Mono clip";
	mono.pan_automation.unit = PX::TimeUnit::Seconds;
	mono.pan_automation.points = {{0, .2, false}, {1, .8, false}, {3, .5, false}};
	input.tracks.push_back (mono);
	SF_INFO info = {}; info.channels = 1; info.samplerate = input.media[0].sample_rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	SNDFILE* file = sf_open (fixture.path ("mono.wav").c_str (), SFM_WRITE, &info);
	CPPUNIT_ASSERT (file); std::vector<float> pcm (info.samplerate * 2, .125);
	CPPUNIT_ASSERT_EQUAL (sf_count_t (pcm.size ()), sf_writef_float (file, pcm.data (), pcm.size ()));
	CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
	input.media.push_back ({"mono-media", "audio/mono.wav", unsigned (info.samplerate), 1, 2});
	PX::DawProject::write (input, fixture.path ("input.dawproject"), {{"source", fixture.path ("source.wav")}, {"mono-media", fixture.path ("mono.wav")}});
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Mix"), "Mix", fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Mix"), "Mix", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto native = fixture.session->route_by_name ("Mono"); CPPUNIT_ASSERT (native);
	CPPUNIT_ASSERT (native->gain_control ()->automation_state () == Play);
	CPPUNIT_ASSERT (native->gain_control ()->alist ()->time_domain () == Temporal::BeatTime);
	CPPUNIT_ASSERT (native->pan_azimuth_control ()->alist ()->time_domain () == Temporal::AudioTime);
	for (unsigned i = 0; i <= 32; ++i) {
		double beat = i / 4.0;
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (mono.volume_automation, beat), native->gain_control ()->alist ()->eval (Temporal::timepos_t (Temporal::Beats::from_double (beat))), .000001);
	}
	for (double t : {0.0, .5, 1.0, 2.0, 3.0, 4.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (mono.pan_automation, t), native->pan_azimuth_control ()->alist ()->eval (Temporal::timepos_t (samplepos_t (t * info.samplerate))), .000001);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("mix.dawproject"));
	auto exported = PX::DawProject::read (fixture.path ("mix.dawproject"));
	auto const& daw = track_named (exported, "Mono");
	for (unsigned i = 0; i <= 32; ++i) { CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (mono.volume_automation, i / 4.0), curve_value (daw.volume_automation, i / 4.0), .000001); }
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("AAF"), fixture.report);
	AudioEngine::instance ()->remove_session (); native.reset (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("AAF/Project.aaf"), fixture.path ("AAF/Project.mid"), fixture.path ("ReturnMix"), "ReturnMix", fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("ReturnMix"), "ReturnMix", nullptr, "", true));
	native = fixture.session->route_by_name ("Mono"); CPPUNIT_ASSERT (native);
	PX::TimeMap timing (input);
	for (unsigned i = 0; i <= 32; ++i) {
		double beat = i / 4.0, seconds = timing.convert (PX::Time (beat), PX::TimeUnit::Seconds);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (mono.volume_automation, beat), native->gain_control ()->alist ()->eval (Temporal::timepos_t (samplepos_t (std::llround (seconds * info.samplerate)))), .0002);
	}
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.8, native->pan_azimuth_control ()->alist ()->eval (Temporal::timepos_t (samplepos_t (2 * info.samplerate))), .000001);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, native->pan_azimuth_control ()->alist ()->eval (Temporal::timepos_t (samplepos_t (4 * info.samplerate))), .000001);
	native.reset ();
}

void ProjectSessionTest::native_fader_automation ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Fader"), "Fader", fixture.report);
	auto control = fixture.session->route_by_name ("Stereo")->gain_control ();
	auto list = control->alist (); list->set_time_domain (Temporal::BeatTime);
	CPPUNIT_ASSERT (list->set_interpolation (Evoral::ControlList::Exponential));
	list->add (Temporal::timepos_t (Temporal::Beats (0, 0)), 0, false, false);
	list->add (Temporal::timepos_t (Temporal::Beats (4, 0)), 1.5, false, false);
	list->add (Temporal::timepos_t (Temporal::Beats (8, 0)), .1, false, false);
	control->set_automation_state (Play);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("fader.dawproject"), fixture.report);
	auto exported = PX::DawProject::read (fixture.path ("fader.dawproject"));
	auto const& curve = track_named (exported, "Stereo").volume_automation;
	CPPUNIT_ASSERT (curve.points.size () > 3);
	for (unsigned i = 0; i <= 800; ++i) {
		double t = i / 100.0;
		double expected = t <= 4 ? interpolate_gain (0, 1.5, t / 4, control->upper ()) : interpolate_gain (1.5, .1, (t - 4) / 4, control->upper ());
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected, curve_value (curve, t), .000101);
	}
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Transformed && d.detail.find ("Native curved automation") != std::string::npos; }));
	list.reset (); control.reset ();
}

void ProjectSessionTest::automation_collision ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tracks[2].volume_automation.unit = PX::TimeUnit::Seconds;
	input.tracks[2].volume_automation.points = {{0, .25, true}, {1, 1, true}, {1.000000001, .5, false}};
	package (fixture, input);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Collision"), "Collision", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Collision"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::variable_timing ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tempo = 60; input.numerator = 4;
	input.tempos = {{PX::Time (0), 60, false}, {PX::Time (4.25), 120, true},
	                {PX::Time (8.25), 180, false}, {PX::Time (8.25), 90, false}};
	input.meters = {{PX::Time (0), 4, 4}, {PX::Time (4.25), 3, 4}, {PX::Time (8.25), 7, 8}};
	input.markers = {{"Ramp", PX::Time (4.25)}, {"Seconds", PX::Time (6, PX::TimeUnit::Seconds)}};
	input.tracks.erase (input.tracks.begin () + 1); // Direct routing is supported by both formats.
	for (auto& track : input.tracks) { track.parent.clear (); track.pan = .5; if (!track.master) { track.destination = "master"; } }
	auto& audio = input.tracks[1].clips;
	audio.resize (1); audio[0].position = PX::Time (3.5); audio[0].duration = 2;
	auto& midi = input.tracks[2].clips[0];
	midi.position = PX::Time (3.5); midi.duration = 5; midi.offset = PX::Time (1, PX::TimeUnit::Seconds);
	midi.notes.resize (1); midi.notes[0].time = 1.2; midi.notes[0].duration = 1.3;
	PX::TimeMap expected (input);
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Timing"), "Timing", fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Timing"), "Timing", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto native = Temporal::TempoMap::use ();
	CPPUNIT_ASSERT_EQUAL (size_t (3), size_t (native->n_tempos ()));
	CPPUNIT_ASSERT_EQUAL (size_t (3), size_t (native->n_meters ()));
	CPPUNIT_ASSERT_EQUAL (7, int (native->meter_at (Temporal::Beats::from_double (9)).divisions_per_bar ()));
	CPPUNIT_ASSERT_EQUAL (8, int (native->meter_at (Temporal::Beats::from_double (9)).note_value ()));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("timing.dawproject"));
	auto output = PX::DawProject::read (fixture.path ("timing.dawproject"));
	PX::TimeMap exported (output);
	for (unsigned i = 0; i <= 128; ++i) {
		double beat = i / 8.0;
		double seconds = double (native->superclock_at (Temporal::Beats::from_double (beat))) / Temporal::superclock_ticks_per_second ();
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.convert (PX::Time (beat), PX::TimeUnit::Seconds), seconds, 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (seconds, exported.convert (PX::Time (beat), PX::TimeUnit::Seconds), 1e-6);
	}
	auto const& out_audio = track_named (output, "Stereo").clips[0];
	CPPUNIT_ASSERT_DOUBLES_EQUAL (3.5, out_audio.position.value, 1.0 / fixture.session->sample_rate ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.duration (PX::Time (2), PX::Time (3.5), PX::TimeUnit::Seconds), out_audio.duration, 1.0 / fixture.session->sample_rate ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.375, out_audio.offset.value, 1e-8);
	auto const& out_midi = track_named (output, "Notes").clips[0];
	double start = expected.content (PX::Time (1.2, PX::TimeUnit::Seconds), midi.position, midi.offset, PX::TimeUnit::Beats);
	double stop = expected.content (PX::Time (2.5, PX::TimeUnit::Seconds), midi.position, midi.offset, PX::TimeUnit::Beats);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (start, out_midi.notes[0].time, 1.0 / Temporal::Beats::PPQN);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (stop - start, out_midi.notes[0].duration, 2.0 / Temporal::Beats::PPQN);
	CPPUNIT_ASSERT_EQUAL (size_t (3), output.meters.size ());
	for (size_t i = 0; i < input.meters.size (); ++i) { CPPUNIT_ASSERT_DOUBLES_EQUAL (input.meters[i].position.value, output.meters[i].position.value, 1e-9); }
	CPPUNIT_ASSERT_EQUAL (input.markers.size (), output.markers.size ());
	for (size_t i = 0; i < input.markers.size (); ++i) {
		CPPUNIT_ASSERT_EQUAL (input.markers[i].name, output.markers[i].name);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.convert (input.markers[i].position, PX::TimeUnit::Beats), exported.convert (output.markers[i].position, PX::TimeUnit::Beats), 1.0 / Temporal::Beats::PPQN);
	}
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Logic"), fixture.report);
	PX::Project smf;
	PX::read_project_midi (fixture.path ("Logic/Project.mid"), smf, 0);
	PX::TimeMap midi_timing (smf);
	CPPUNIT_ASSERT (smf.tempos.size () > 3);
	CPPUNIT_ASSERT_EQUAL (size_t (3), smf.meters.size ());
	for (unsigned i = 0; i <= 128; ++i) {
		double beat = i / 8.0;
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.convert (PX::Time (beat), PX::TimeUnit::Seconds), midi_timing.convert (PX::Time (beat), PX::TimeUnit::Seconds), .0001);
	}
	// AAF/MIDI import must retain the same tempo map after save and reopen.
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Logic/Project.aaf"), fixture.path ("Logic/Project.mid"), fixture.path ("Return"), "Return", fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Return"), "Return", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("return.dawproject"));
	PX::TimeMap returned_timing (returned);
	for (unsigned i = 0; i <= 128; ++i) {
		double beat = i / 8.0;
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.convert (PX::Time (beat), PX::TimeUnit::Seconds), returned_timing.convert (PX::Time (beat), PX::TimeUnit::Seconds), .001);
	}
}

void ProjectSessionTest::second_based_ramp ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tempos = {{PX::Time (0, PX::TimeUnit::Seconds), 60, true}, {PX::Time (4, PX::TimeUnit::Seconds), 180, false}};
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Ramp"), "Ramp", fixture.report);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) { return item.object == "Tempo" && item.disposition == PX::Disposition::Transformed; }));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Ramp"), "Ramp", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("ramp.dawproject"));
	auto output = PX::DawProject::read (fixture.path ("ramp.dawproject"));
	PX::TimeMap timing (output);
	for (unsigned i = 0; i <= 64; ++i) {
		double beat = i / 8.0;
		// q = s + s*s/4 for a 60 -> 180 BPM ramp over four seconds.
		double seconds = 2 * (std::sqrt (1 + beat) - 1);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (seconds, timing.convert (PX::Time (beat), PX::TimeUnit::Seconds), .001);
	}
}

void ProjectSessionTest::transport_collision ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tempos = {{PX::Time (0), 100, false}, {PX::Time (1), 120, false}, {PX::Time (1.00000001), 140, false}};
	package (fixture, input);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Collision"), "Collision", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Collision"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::marker_roundtrip ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.markers = {{"Intro", PX::Time (0)}, {u8"Refrain éŸ³æ¥½", PX::Time (12.5)}, {"Cue", PX::Time (2.1, PX::TimeUnit::Seconds)}};
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Markers"), "Markers", fixture.report);
	CPPUNIT_ASSERT (!fixture.session->unnamed ());
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Transformed && item.detail.find ("Marker converted to the session time basis") != std::string::npos;
	}));
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Markers"), "Markers", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("markers.dawproject"));
	auto output = PX::DawProject::read (fixture.path ("markers.dawproject"));
	CPPUNIT_ASSERT_EQUAL (input.markers.size (), output.markers.size ());
	for (size_t i = 0; i < input.markers.size (); ++i) {
		CPPUNIT_ASSERT_EQUAL (input.markers[i].name, output.markers[i].name);
		CPPUNIT_ASSERT (output.markers[i].position.unit == PX::TimeUnit::Beats);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (PX::convert_time (input.markers[i].position, PX::TimeUnit::Beats, input.tempo), output.markers[i].position.value, 1e-6);
	}
	CPPUNIT_ASSERT_DOUBLES_EQUAL (7.5, double (fixture.session->locations ()->session_range_location ()->end_sample ()) / fixture.session->sample_rate (), 1e-6);
	auto sample_position = Temporal::timepos_t (fixture.session->sample_rate () * 3);
	auto sample_marker = new Location (*fixture.session, sample_position, sample_position, "Sample marker", Location::IsMark);
	sample_marker->set_time_domain (Temporal::AudioTime);
	fixture.session->locations ()->add (sample_marker);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("mixed-markers.dawproject"), fixture.report);
	auto mixed = PX::DawProject::read (fixture.path ("mixed-markers.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (4), mixed.markers.size ());
	for (auto const& marker : mixed.markers) {
		CPPUNIT_ASSERT (marker.position.unit == PX::TimeUnit::Seconds);
		double expected = marker.name == "Intro" ? 0 : (marker.name == "Sample marker" ? 3 : (marker.name == "Cue" ? 2.1 : 7.5));
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected, marker.position.value, 1e-6);
	}
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Transformed && item.object == "Markers";
	}));
	auto start = Temporal::timepos_t (0), end = Temporal::timepos_t (fixture.session->sample_rate ());
	fixture.session->locations ()->add (new Location (*fixture.session, start, end, "Range", Location::IsRangeMarker));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("unsupported-range.dawproject"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("unsupported-range.dawproject"), Glib::FILE_TEST_EXISTS));
}

void ProjectSessionTest::studio_pro_export ()
{
	Fixture fixture;
	std::string archive_path;
	if (const char* external = g_getenv ("ARDOUR_STUDIO_PRO_TEST_PROJECT")) {
		archive_path = external;
		std::cout << "\nTesting actual Studio Pro DAWproject: " << archive_path << std::endl;
	} else {
		/* This XML is a Studio Pro 8.0.3.111164 export. The default test's
		 * audio is synthesized; an explicit package can test actual media. */
		std::string xml = Glib::file_get_contents (Glib::build_filename (test_search_path ().front (), "studio-pro-8.0.3-project.xml"));
		auto model = PX::DawProject::parse_xml (xml);
		archive_path = fixture.path ("studio-pro.dawproject");
		PX::ProjectArchiveWriter archive (archive_path);
		archive.add_text ("project.xml", xml);
		for (size_t i = 0; i < model.media.size (); ++i) {
			auto const& media = model.media[i];
			SF_INFO info = {};
			info.channels = media.channels; info.samplerate = media.sample_rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
			std::string path = fixture.path ("studio-" + std::to_string (i) + ".wav");
			SNDFILE* file = sf_open (path.c_str (), SFM_WRITE, &info);
			CPPUNIT_ASSERT (file);
			std::vector<float> block (4096 * media.channels);
			for (size_t sample = 0; sample < block.size (); ++sample) { block[sample] = float (int ((sample + i * 13) % 101) - 50) / 128; }
			sf_count_t frames = std::llround (media.duration * media.sample_rate);
			for (sf_count_t at = 0; at < frames; at += 4096) {
				sf_count_t count = std::min (sf_count_t (4096), frames - at);
				CPPUNIT_ASSERT_EQUAL (count, sf_writef_float (file, block.data (), count));
			}
			CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
			archive.add_file (media.path, path);
		}
		archive.finish ();
	}
	auto input = PX::DawProject::read (archive_path);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), archive_path, fixture.path ("Studio"), "Studio", fixture.report);
	CPPUNIT_ASSERT (!fixture.session->unnamed ());
	CPPUNIT_ASSERT (Glib::file_get_contents (fixture.path ("Studio/Studio.ardour")).find ("Master/audio_in") == std::string::npos);
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Studio"), "Studio", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("studio-output.dawproject"));
	auto output = PX::DawProject::read (fixture.path ("studio-output.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (4), output.tracks.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), output.media.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), output.markers.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Start"), output.markers[0].name);
	CPPUNIT_ASSERT_EQUAL (std::string ("End"), output.markers[1].name);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (300, PX::convert_time (output.markers[1].position, PX::TimeUnit::Seconds, output.tempo), 1e-6);
	CPPUNIT_ASSERT_EQUAL (std::string ("#dc8855"), track_named (output, "Soft pulse").color);
	auto const& notes = track_named (output, "MIDI notes").clips[0].notes;
	CPPUNIT_ASSERT_EQUAL (size_t (32), notes.size ());
	int keys[] = {60, 64, 67, 72};
	for (size_t i = 0; i < notes.size (); ++i) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (double (i), notes[i].time, 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (0.75, notes[i].duration, 1e-6);
		CPPUNIT_ASSERT_EQUAL (keys[i % 4], notes[i].key);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (83.0 / 127, notes[i].velocity, 1e-6);
	}
	for (auto const& name : {"Soft pulse", "Chords"}) {
		auto const& before = track_named (input, name).clips[0];
		auto const& after = track_named (output, name).clips[0];
		CPPUNIT_ASSERT_DOUBLES_EQUAL (0, after.position.value, 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (16, PX::convert_time (PX::Time (after.duration, after.position.unit), PX::TimeUnit::Seconds, output.tempo), 1e-6);
		auto in_media = std::find_if (input.media.begin (), input.media.end (), [&] (const PX::Media& m) { return m.id == before.media; });
		auto out_media = std::find_if (output.media.begin (), output.media.end (), [&] (const PX::Media& m) { return m.id == after.media; });
		CPPUNIT_ASSERT (in_media != input.media.end () && out_media != output.media.end ());
		PX::ProjectArchive (archive_path).copy_file (in_media->path, fixture.path ("before.wav"));
		PX::ProjectArchive (fixture.path ("studio-output.dawproject")).copy_file (out_media->path, fixture.path ("after.wav"));
		{
			SndFileImportableSource original (fixture.path ("before.wav")), converted (fixture.path ("after.wav"));
			CPPUNIT_ASSERT_EQUAL (original.length (), converted.length ());
			CPPUNIT_ASSERT_EQUAL (original.channels (), converted.channels ());
			std::vector<float> a (original.length () * original.channels ()), b (a.size ());
			CPPUNIT_ASSERT_EQUAL (samplecnt_t (a.size ()), original.read (a.data (), a.size ()));
			CPPUNIT_ASSERT_EQUAL (samplecnt_t (b.size ()), converted.read (b.data (), b.size ()));
			CPPUNIT_ASSERT (a == b);
		}
		CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("before.wav").c_str ()));
		CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("after.wav").c_str ()));
	}
	std::cout << "\nStudio Pro session round trip preserved audio, MIDI and markers: " << fixture.path ("studio-output.dawproject") << std::endl;
}

void ProjectSessionTest::native_song_import ()
{
	Fixture fixture;
	native_package (fixture);
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("Native"), "Native", fixture.report);
	CPPUNIT_ASSERT (!fixture.session->unnamed ());
	CPPUNIT_ASSERT (fixture.session);
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("input.song").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("source.wav").c_str ()));
	auto range = fixture.session->locations ()->session_range_location ();
	CPPUNIT_ASSERT (range);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (7.2, double (range->end ().samples ()) / fixture.session->sample_rate (), 1.0 / fixture.session->sample_rate ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-output.dawproject"));
	auto output = PX::DawProject::read (fixture.path ("native-output.dawproject"));
	CPPUNIT_ASSERT_DOUBLES_EQUAL (100, output.tempo, 1e-6);
	CPPUNIT_ASSERT_EQUAL (3U, output.numerator);
	CPPUNIT_ASSERT_EQUAL (size_t (1), output.markers.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Verse"), output.markers[0].name);
	CPPUNIT_ASSERT (output.markers[0].position.unit == PX::TimeUnit::Beats);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (8, output.markers[0].position.value, 1e-6);
	auto const& audio = track_named (output, "Native audio");
	CPPUNIT_ASSERT_EQUAL (std::string ("#12ab34"), audio.color);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.75, audio.volume, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.3, audio.pan, 1e-6);
	CPPUNIT_ASSERT_EQUAL (size_t (1), audio.clips.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, audio.clips[0].position.value, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.375, audio.clips[0].offset.value, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.5, audio.clips[0].duration, 1e-6);
	PX::ProjectArchive (fixture.path ("native-output.dawproject")).copy_file (output.media[0].path, fixture.path ("native-verify.wav"));
	SndFileImportableSource source (fixture.path ("native-verify.wav"));
	std::vector<Sample> pcm (source.length () * source.channels ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (pcm.size ()), source.read (pcm.data (), pcm.size ()));
	for (size_t i = 0; i < pcm.size () / 2; ++i) {
		CPPUNIT_ASSERT_EQUAL (float (int (i % 37) - 18) / 32, pcm[2 * i]);
		CPPUNIT_ASSERT_EQUAL (float (int (i % 61) - 30) / 64, pcm[2 * i + 1]);
	}
}

void ProjectSessionTest::native_song_mixer_automation ()
{
	Fixture fixture; native_package (fixture);
	auto source = PX::StudioOneSong::read (fixture.path ("input.song"));
	auto track = std::find_if (source.project.tracks.begin (), source.project.tracks.end (), [] (const PX::Track& item) { return item.name == "Native audio"; });
	CPPUNIT_ASSERT (track != source.project.tracks.end ());
	track->volume_automation.unit = PX::TimeUnit::Seconds;
	track->volume_automation.points = {{0, .25, true}, {1, .75, true}, {2, .5, true}};
	track->pan_automation.unit = PX::TimeUnit::Seconds;
	track->pan_automation.points = {{0, .2, true}, {1, .8, true}, {2, .5, true}};
	track->volume_automation.points.front ().linear = false;
	CPPUNIT_ASSERT_THROW (PX::StudioOneSong::write (source, fixture.path ("held.song"), fixture.path ("held.song")), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("held.song"), Glib::FILE_TEST_EXISTS));
	track->volume_automation.points.front ().linear = true;
	PX::StudioOneSong::write (source, fixture.path ("automation.song"), fixture.path ("automation.song"));
	auto parsed = PX::StudioOneSong::read (fixture.path ("automation.song"));
	auto const& native = track_named (parsed.project, "Native audio");
	CPPUNIT_ASSERT_EQUAL (size_t (3), native.volume_automation.points.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (3), native.pan_automation.points.size ());
	for (double at : {0.0, .5, 1.0, 1.5, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->volume_automation, at), curve_value (native.volume_automation, at), 1e-9);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->pan_automation, at), curve_value (native.pan_automation, at), 1e-9);
	}
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("automation.song"),
	                                                        fixture.path ("Automated"), "Automated", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Automated"), "Automated", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("automation-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("automation-return.dawproject"));
	auto const& daw = track_named (returned, "Native audio");
	for (double at : {0.0, .5, 1.0, 1.5, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->volume_automation, at), curve_value (daw.volume_automation, at), 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->pan_automation, at), curve_value (daw.pan_automation, at), 1e-6);
	}
	PX::SessionExchange::ExportOptions options;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("NativeAutomationReturn"), fixture.report, options);
	auto native_return = PX::StudioOneSong::read (fixture.path ("NativeAutomationReturn/Project.song"));
	auto const& final = track_named (native_return.project, "Native audio");
	CPPUNIT_ASSERT_EQUAL (size_t (3), final.volume_automation.points.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (3), final.pan_automation.points.size ());
	for (double at : {0.0, .5, 1.0, 1.5, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->volume_automation, at), curve_value (final.volume_automation, at), 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (curve_value (track->pan_automation, at), curve_value (final.pan_automation, at), 1e-6);
	}
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::native_song_timing_and_gain ()
{
	Fixture fixture;
	native_package (fixture, false, true);
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("Native"), "Native", fixture.report);
	CPPUNIT_ASSERT (!std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Unsupported; }));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("input.song").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("source.wav").c_str ()));
	auto reopen = [&] (const std::string& name) {
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
	};
	auto check_audio = [&] {
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Native audio")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list (); CPPUNIT_ASSERT_EQUAL (size_t (3), regions->size ());
		const samplecnt_t rate = fixture.session->sample_rate ();
		const double positions[] = {2.8, 3.8, 4.3}, lengths[] = {.75, .25, .25}, levels[] = {.5, 2, 0};
		std::set<size_t> found;
		for (auto const& region : *regions) {
			auto audio = std::dynamic_pointer_cast<AudioRegion> (region); CPPUNIT_ASSERT (audio);
			size_t index = 0;
			while (index < 3 && std::abs (double (audio->position_sample ()) / rate - positions[index]) > 1.0 / rate) { ++index; }
			CPPUNIT_ASSERT (index < 3 && found.insert (index).second);
			CPPUNIT_ASSERT_EQUAL (samplecnt_t (std::llround (lengths[index] * rate)), audio->length_samples ());
			CPPUNIT_ASSERT_EQUAL (samplepos_t (std::llround (.375 * rate)), audio->start_sample ());
			std::vector<float> output (audio->length_samples ()), mix (256), gain (256);
			for (unsigned channel = 0; channel < 2; ++channel) {
				std::fill (output.begin (), output.end (), 0);
				for (samplecnt_t at = 0; at < audio->length_samples (); at += 256) {
					auto count = std::min (samplecnt_t (256), audio->length_samples () - at);
					CPPUNIT_ASSERT_EQUAL (count, audio->read_at (output.data () + at, mix.data (), gain.data (), audio->position_sample () + at, count, channel));
				}
				for (size_t at = 0; at < output.size (); ++at) {
					size_t source_frame = at + size_t (std::llround (.375 * rate));
					double raw = channel ? double (int (source_frame % 61) - 30) / 64 : double (int (source_frame % 37) - 18) / 32;
					CPPUNIT_ASSERT_DOUBLES_EQUAL (raw * levels[index], output[at], .000001);
				}
			}
		}
	};
	auto check_timing = [&] (const std::string& archive) {
		auto model = PX::DawProject::read (fixture.path (archive)); PX::TimeMap timing (model);
		for (unsigned i = 0; i <= 48; ++i) {
			double beat = i / 4.0;
			double seconds = beat <= 3 ? beat * .6 : (beat <= 6 ? 1.8 + (beat - 3) * .5 : 3.3 + (beat - 6) * .25);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (seconds, timing.convert (PX::Time (beat), PX::TimeUnit::Seconds), 1.0 / fixture.session->sample_rate ());
		}
		CPPUNIT_ASSERT_EQUAL (size_t (3), model.meters.size ());
		const double starts[] = {0, 3, 5.5}; const unsigned numerators[] = {3, 5, 4}, denominators[] = {4, 8, 4};
		for (size_t i = 0; i < 3; ++i) {
			CPPUNIT_ASSERT_DOUBLES_EQUAL (starts[i], timing.convert (model.meters[i].position, PX::TimeUnit::Beats), 1.0 / Temporal::Beats::PPQN);
			CPPUNIT_ASSERT_EQUAL (numerators[i], model.meters[i].numerator);
			CPPUNIT_ASSERT_EQUAL (denominators[i], model.meters[i].denominator);
		}
		CPPUNIT_ASSERT_EQUAL (size_t (1), model.markers.size ());
		CPPUNIT_ASSERT_EQUAL (std::string ("Verse"), model.markers[0].name);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (3.8, timing.convert (model.markers[0].position, PX::TimeUnit::Seconds), 1.0 / fixture.session->sample_rate ());
	};
	reopen ("Native"); check_audio ();
	auto range = fixture.session->locations ()->session_range_location (); CPPUNIT_ASSERT (range);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (2.3, double (range->start_sample ()) / fixture.session->sample_rate (), 1.0 / fixture.session->sample_rate ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4.8, double (range->end_sample ()) / fixture.session->sample_rate (), 1.0 / fixture.session->sample_rate ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-steps.dawproject")); check_timing ("native-steps.dawproject");
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("NativeLogic"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (bool logic : {false, true}) {
		std::string name = logic ? "AAFReturn" : "DAWReturn";
		fixture.session = logic ? PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("NativeLogic/Project.aaf"), fixture.path ("NativeLogic/Project.mid"), fixture.path (name), name, fixture.report)
		                        : PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("native-steps.dawproject"), fixture.path (name), name, fixture.report);
		reopen (name); check_audio ();
		PX::SessionExchange::export_dawproject (*fixture.session, fixture.path (name + ".dawproject")); check_timing (name + ".dawproject");
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	}
}

void ProjectSessionTest::native_song_fades ()
{
	Fixture fixture;
	native_package (fixture, false, false, true);
	auto parsed = PX::StudioOneSong::read (fixture.path ("input.song"));
	auto const& original = track_named (parsed.project, "Native audio").clips.front ().envelope.points;
	CPPUNIT_ASSERT_EQUAL (size_t (4), original.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0, original[0].value, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.125, original[1].time, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1, original[1].value, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.375, original[2].time, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, original[3].time, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0, original[3].value, 1e-9);
	CPPUNIT_ASSERT (!PX::has_unsupported_features (parsed.project));
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("NativeFade"), "NativeFade", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("input.song").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("source.wav").c_str ()));
	auto check = [&] {
		auto route = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Native audio")); CPPUNIT_ASSERT (route);
		auto regions = route->playlist ()->region_list (); CPPUNIT_ASSERT_EQUAL (size_t (1), regions->size ());
		auto audio = std::dynamic_pointer_cast<AudioRegion> (*regions->begin ()); CPPUNIT_ASSERT (audio && audio->envelope_active ());
		const samplecnt_t rate = fixture.session->sample_rate ();
		const samplecnt_t fade = std::llround (.125 * rate);
		for (samplecnt_t at : {samplecnt_t (0), fade / 2, fade, fade * 2, audio->length_samples () - fade / 2, audio->length_samples () - 1}) {
			float output = 0, mix = 0, gain = 0;
			CPPUNIT_ASSERT_EQUAL (samplecnt_t (1), audio->read_at (&output, &mix, &gain, audio->position_sample () + at, 1, 0));
			size_t source_frame = size_t (at + std::llround (.375 * rate));
			double raw = double (int (source_frame % 37) - 18) / 32;
			double fade_level = std::min ({1.0, double (at) / fade, double (audio->length_samples () - at) / fade});
			CPPUNIT_ASSERT_DOUBLES_EQUAL (raw * fade_level, output, .0003);
		}
	};
	auto reopen = [&] (const std::string& name) {
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
	};
	check (); reopen ("NativeFade"); check ();
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-fade.dawproject"));
	auto exported = PX::DawProject::read (fixture.path ("native-fade.dawproject"));
	CPPUNIT_ASSERT (!track_named (exported, "Native audio").clips.front ().envelope.points.empty ());
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("NativeFadeLogic"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (bool logic : {false, true}) {
		std::string name = logic ? "FadeAAFReturn" : "FadeDAWReturn";
		fixture.session = logic ? PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("NativeFadeLogic/Project.aaf"), fixture.path ("NativeFadeLogic/Project.mid"), fixture.path (name), name, fixture.report)
		                        : PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("native-fade.dawproject"), fixture.path (name), name, fixture.report);
		reopen (name); check ();
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	}
}

void ProjectSessionTest::bus_sends_roundtrip ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 3); // MIDI track.
	input.tracks.erase (input.tracks.begin () + 1); // Folder.
	auto& audio = input.tracks[1];
	audio.parent.clear (); audio.destination = "master"; audio.pan = .5;
	audio.sends = {{"room", true, true, .25, .8}, {"delay", false, false, .625, .2}};
	PX::Track room; room.id = "room"; room.name = "Room"; room.kind = PX::TrackKind::Bus; room.destination = "master";
	PX::Track delay = room; delay.id = "delay"; delay.name = "Delay";
	input.tracks.push_back (room); input.tracks.push_back (delay);
	package (fixture, input);
	auto parsed = PX::DawProject::read (fixture.path ("input.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (2), track_named (parsed, "Stereo").sends.size ());
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Sends"), "Sends", fixture.report);
	auto check = [&] {
		auto route = fixture.session->route_by_name ("Stereo"); CPPUNIT_ASSERT (route);
		std::map<std::string, std::shared_ptr<InternalSend>> sends;
		route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			if (auto send = std::dynamic_pointer_cast<InternalSend> (weak.lock ())) {
				if (send->role () == Delivery::Aux && send->target_route ()) { sends[send->target_route ()->name ()] = send; }
			}
		});
		CPPUNIT_ASSERT_EQUAL (size_t (2), sends.size ());
		for (auto const& expected : {std::make_tuple ("Room", true, true, .25, .8),
		                            std::make_tuple ("Delay", false, false, .625, .2)}) {
			auto send = sends.at (std::get<0> (expected));
			CPPUNIT_ASSERT_EQUAL (std::get<1> (expected), send->get_pre_fader ());
			CPPUNIT_ASSERT_EQUAL (std::get<2> (expected), send->active ());
			CPPUNIT_ASSERT_DOUBLES_EQUAL (std::get<3> (expected), send->gain_control ()->get_value (), .000001);
			CPPUNIT_ASSERT (send->panner_shell () && send->panner_shell ()->pannable ());
			CPPUNIT_ASSERT (send->panner_shell ()->pannable ()->pan_azimuth_control);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (std::get<4> (expected), send->panner_shell ()->pannable ()->pan_azimuth_control->get_value (), .000001);
		}
	};
	check ();
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Sends"), "Sends", nullptr, "", true));
	check ();
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("sends.dawproject"));
	auto exported = PX::DawProject::read (fixture.path ("sends.dawproject"));
	auto const& out_sends = track_named (exported, "Stereo").sends;
	CPPUNIT_ASSERT_EQUAL (size_t (2), out_sends.size ());
	for (auto const& send : out_sends) {
		auto found = std::find_if (exported.tracks.begin (), exported.tracks.end (), [&] (const PX::Track& t) { return t.id == send.destination; });
		CPPUNIT_ASSERT (found != exported.tracks.end ());
		std::string target = found->name;
		CPPUNIT_ASSERT (target == "Room" || target == "Delay");
		CPPUNIT_ASSERT_DOUBLES_EQUAL (target == "Room" ? .8 : .2, send.pan, .000001);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (target == "Room" ? .25 : .625, send.volume, .000001);
		CPPUNIT_ASSERT_EQUAL (target == "Room", send.pre_fader);
		CPPUNIT_ASSERT_EQUAL (target == "Room", send.enabled);
	}
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("SendsAAF"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Unsupported && d.detail.find ("bus sends") != std::string::npos;
	}));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("sends.dawproject"), fixture.path ("SendsReturn"), "SendsReturn", fixture.report);
	check ();
}

void ProjectSessionTest::native_song_fx_sends_roundtrip ()
{
	Fixture fixture;
	auto input = example (fixture);
	input.tracks.erase (input.tracks.begin () + 3); // MIDI track.
	input.tracks.erase (input.tracks.begin () + 1); // Folder.
	auto& audio = input.tracks[1];
	audio.parent.clear (); audio.destination = "master"; audio.pan = .5;
	audio.sends = {{"room", true, true, .25, .8}, {"delay", false, false, .625, .2}};
	PX::Track room; room.id = "room"; room.name = "Room"; room.kind = PX::TrackKind::Bus; room.destination = "master";
	PX::Track delay = room; delay.id = "delay"; delay.name = "Delay";
	input.tracks.push_back (room); input.tracks.push_back (delay);
	package (fixture, input);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Source"), "Source", fixture.report);
	PX::SessionExchange::ExportOptions options;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("NativeSends"), fixture.report, options);
	auto verify = [&] (const PX::Project& project) {
		auto const& sends = track_named (project, "Stereo").sends;
		CPPUNIT_ASSERT_EQUAL (size_t (2), sends.size ());
		for (auto const& send : sends) {
			auto target = std::find_if (project.tracks.begin (), project.tracks.end (), [&] (const PX::Track& t) { return t.id == send.destination; });
			CPPUNIT_ASSERT (target != project.tracks.end ());
			CPPUNIT_ASSERT (target->name == "Room" || target->name == "Delay");
			bool first = target->name == "Room";
			CPPUNIT_ASSERT_EQUAL (first, send.pre_fader); CPPUNIT_ASSERT_EQUAL (first, send.enabled);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (first ? .25 : .625, send.volume, .000001);
			CPPUNIT_ASSERT_DOUBLES_EQUAL (first ? .8 : .2, send.pan, .000001);
		}
	};
	verify (PX::StudioOneSong::read (fixture.path ("NativeSends/Project.song")).project);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("NativeSends/Project.song"), fixture.path ("Returned"), "Returned", fixture.report);
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Room"));
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Delay"));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Returned"), "Returned", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("native-sends-return.dawproject"));
	verify (PX::DawProject::read (fixture.path ("native-sends-return.dawproject")));
	std::cout << "Native song FX/bus sends survived Ardour import, save/reopen and DAWproject return" << std::endl;
}

void native_song_older_format_import (unsigned format_version)
{
	Fixture fixture; native_package (fixture, false, false, false, format_version);
	auto original = PX::StudioOneSong::read (fixture.path ("input.song"));
	CPPUNIT_ASSERT_EQUAL (format_version, original.format_version);
	CPPUNIT_ASSERT (std::any_of (original.project.report.begin (), original.project.report.end (), [format_version] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Transformed && d.detail.find ("format " + std::to_string (format_version)) != std::string::npos;
	}));
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("OlderSong"), "OlderSong", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("OlderSong"), "OlderSong", nullptr, "", true));
	PX::SessionExchange::ExportOptions options;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("UpgradedSong"), fixture.report, options);
	PX::ProjectArchive archive (fixture.path ("UpgradedSong/Project.song"));
	CPPUNIT_ASSERT (archive.read_text ("metainfo.xml").find ("Document:FormatVersion\" value=\"9") != std::string::npos);
	options.studio_one_format_version = format_version;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("VersionMatchedSong"), fixture.report, options);
	auto matched = PX::StudioOneSong::read (fixture.path ("VersionMatchedSong/Project.song"));
	CPPUNIT_ASSERT_EQUAL (format_version, matched.format_version);
	CPPUNIT_ASSERT_EQUAL (size_t (1), track_named (matched.project, "Native audio").clips.size ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("older-format-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("older-format-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (1), track_named (returned, "Native audio").clips.size ());
	std::cout << "Format-" << format_version << " native song imported, saved/reopened and returned through format 9 and DAWproject" << std::endl;
}

void ProjectSessionTest::native_song_format_seven_import () { native_song_older_format_import (7); }
void ProjectSessionTest::native_song_format_eight_import () { native_song_older_format_import (8); }

void ProjectSessionTest::native_song_format_eight_app_authored ()
{
	const char* source = std::getenv ("ARDOUR_STUDIO_ONE_7_TEST_SONG");
	if (!source || !*source) { return; }
	Fixture fixture;
	auto original = PX::StudioOneSong::read (source);
	CPPUNIT_ASSERT_EQUAL (std::string ("OwlPlug"), original.title);
	CPPUNIT_ASSERT_EQUAL (std::string ("Studio One/7.2.3.108761"), original.project.application_version);
	CPPUNIT_ASSERT_EQUAL (44100u, original.sample_rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (120, original.project.tempo, 1e-6);
	CPPUNIT_ASSERT (track_named (original.project, "Synth").kind == PX::TrackKind::MIDI);
	CPPUNIT_ASSERT (track_named (original.project, "Track").kind == PX::TrackKind::Audio);
	CPPUNIT_ASSERT (PX::has_unsupported_features (original.project));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_studio_one (*AudioEngine::instance (), source,
	                     fixture.path ("Strict"), "Strict", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Strict"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	fixture.report.clear ();
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), source,
	                                                          fixture.path ("Partial"), "Partial", fixture.report,
	                                                          PX::CancelCheck (), true);
	CPPUNIT_ASSERT (fixture.session);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Unsupported && d.detail.find ("instrument") != std::string::npos;
	}));
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Synth"));
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Track"));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Partial"), "Partial", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("studio-one-7-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("studio-one-7-return.dawproject"));
	CPPUNIT_ASSERT (track_named (returned, "Synth").kind == PX::TrackKind::MIDI);
	CPPUNIT_ASSERT (track_named (returned, "Track").kind == PX::TrackKind::Audio);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (120, returned.tempo, 1e-6);
	PX::SessionExchange::ExportOptions options;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("NativeReturn"), fixture.report, options);
	auto native_return = PX::StudioOneSong::read (fixture.path ("NativeReturn/Project.song"));
	CPPUNIT_ASSERT_EQUAL (9u, native_return.format_version);
	CPPUNIT_ASSERT (track_named (native_return.project, "Synth").kind == PX::TrackKind::MIDI);
	CPPUNIT_ASSERT (track_named (native_return.project, "Track").kind == PX::TrackKind::Audio);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (120, native_return.project.tempo, 1e-6);
	options.studio_one_format_version = 8;
	PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("StudioOne7Return"), fixture.report, options);
	auto version_matched = PX::StudioOneSong::read (fixture.path ("StudioOne7Return/Project.song"));
	CPPUNIT_ASSERT_EQUAL (8u, version_matched.format_version);
	CPPUNIT_ASSERT (track_named (version_matched.project, "Synth").kind == PX::TrackKind::MIDI);
	CPPUNIT_ASSERT (track_named (version_matched.project, "Track").kind == PX::TrackKind::Audio);
	std::cout << "App-authored Studio One 7 song retained tracks and tempo through partial import, save/reopen, DAWproject and native-song return" << std::endl;
}

void ProjectSessionTest::native_song_cancel ()
{
	Fixture fixture;
	native_package (fixture);
	bool reached_session = false;
	auto cancel = [&] { return reached_session = AudioEngine::instance ()->session () != nullptr; };
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("Native"), "Native", fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (reached_session);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Native"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::native_song_unsupported ()
{
	Fixture fixture;
	native_package (fixture, true);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"), fixture.path ("Native"), "Native", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Native"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) { return d.disposition == PX::Disposition::Unsupported; }));
}

void ProjectSessionTest::native_song_partial_import ()
{
	Fixture fixture;
	native_package (fixture, true);
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"),
	                                                          fixture.path ("Partial"), "Partial", fixture.report,
	                                                          PX::CancelCheck (), true);
	CPPUNIT_ASSERT (fixture.session);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Unsupported && d.detail.find ("speed") != std::string::npos;
	}));
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Native audio"));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Partial"), "Partial", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("partial-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("partial-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (1), track_named (returned, "Native audio").clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Native clip"), track_named (returned, "Native audio").clips.front ().name);
}

void ProjectSessionTest::dawproject_partial_import ()
{
	Fixture fixture;
	auto project = example (fixture);
	std::string xml = PX::DawProject::write_xml (project);
	auto root = xml.find ("<Project version=");
	CPPUNIT_ASSERT (root != std::string::npos);
	xml.insert (root + std::string ("<Project").size (), " futureFeature=\"unavailable\"");
	{
		PX::ProjectArchiveWriter archive (fixture.path ("input.dawproject"));
		archive.add_text ("project.xml", xml);
		archive.add_file (project.media.front ().path, fixture.path ("source.wav"));
		archive.finish ();
	}
	auto parsed = PX::DawProject::read (fixture.path ("input.dawproject"));
	CPPUNIT_ASSERT (PX::has_unsupported_features (parsed));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
	                     fixture.path ("Strict"), "Strict", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Strict"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
	                                                           fixture.path ("Partial"), "Partial", fixture.report,
	                                                           PX::CancelCheck (), true);
	CPPUNIT_ASSERT (fixture.session);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Unsupported && item.detail.find ("futureFeature") != std::string::npos;
	}));
	CPPUNIT_ASSERT (fixture.session->route_by_name ("Stereo"));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Partial"), "Partial", nullptr, "", true));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("partial-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("partial-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (2), track_named (returned, "Stereo").clips.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), track_named (returned, "Notes").clips.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), track_named (returned, "Notes").clips.front ().notes.size ());
}

void ProjectSessionTest::native_song_instrument_template_import ()
{
	const char* source = std::getenv ("ARDOUR_STUDIO_PRO_INSTRUMENT_TEMPLATE");
	if (!source) { return; }
	Fixture fixture;
	std::ifstream input (source, std::ios::binary);
	std::ofstream output (fixture.path ("input.song"), std::ios::binary);
	CPPUNIT_ASSERT (input && output);
	output << input.rdbuf ();
	output.close ();
	CPPUNIT_ASSERT (output.good ());
	auto project = PX::StudioOneSong::read (fixture.path ("input.song")).project;
	CPPUNIT_ASSERT (PX::has_unsupported_features (project));
	fixture.session = PX::SessionExchange::import_studio_one (*AudioEngine::instance (), fixture.path ("input.song"),
	                                                          fixture.path ("Template"), "Template", fixture.report,
	                                                          PX::CancelCheck (), true);
	CPPUNIT_ASSERT (fixture.session);
	for (const char* name : {"Drum Pattern", "Bassline", "Chord _ Arp"}) {
		CPPUNIT_ASSERT (fixture.session->route_by_name (name));
	}
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Transformed && d.object == "Chord / Arp" &&
		       d.detail.find ("Chord _ Arp") != std::string::npos;
	}));
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& d) {
		return d.disposition == PX::Disposition::Unsupported && d.detail.find ("instrument") != std::string::npos;
	}));
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Template"), "Template", nullptr, "", true));
	for (const char* name : {"Drum Pattern", "Bassline", "Chord _ Arp"}) {
		CPPUNIT_ASSERT (fixture.session->route_by_name (name));
	}
}

void ProjectSessionTest::roundtrip ()
{
	Fixture fixture;
	PX::Project input = example (fixture);
	package (fixture, input);
	const std::string session_name = u8"éŸ³æ¥½ Session";
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path (session_name), "Exchange", fixture.report);
	CPPUNIT_ASSERT (fixture.session);
	CPPUNIT_ASSERT (!fixture.session->unnamed ());
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	CPPUNIT_ASSERT (Glib::file_test (Glib::build_filename (fixture.path (session_name), "Exchange.ardour"), Glib::FILE_TEST_IS_REGULAR));
	// Remove both external inputs. Export must read the saved session's copies.
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("source.wav").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("input.dawproject").c_str ()));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("output.dawproject"));
	PX::Project output = PX::DawProject::read (fixture.path ("output.dawproject"));
	CPPUNIT_ASSERT_DOUBLES_EQUAL (input.tempo, output.tempo, 1e-5);
	CPPUNIT_ASSERT_EQUAL (input.numerator, output.numerator);
	CPPUNIT_ASSERT_EQUAL (input.denominator, output.denominator);
	CPPUNIT_ASSERT_EQUAL (input.tracks.size (), output.tracks.size ());
	auto const& audio = track_named (output, "Stereo");
	auto const& folder = track_named (output, "Band");
	CPPUNIT_ASSERT (folder.kind == PX::TrackKind::Folder);
	CPPUNIT_ASSERT_EQUAL (folder.id, audio.parent);
	CPPUNIT_ASSERT_EQUAL (folder.id, audio.destination);
	CPPUNIT_ASSERT_EQUAL (std::string ("#12ab34"), audio.color);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.75, audio.volume, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.3, audio.pan, 1e-6);
	CPPUNIT_ASSERT_EQUAL (size_t (2), audio.clips.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, audio.clips[0].position.value, 1.0 / input.media[0].sample_rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.375, audio.clips[0].offset.value, 1.0 / input.media[0].sample_rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.5, audio.clips[0].duration, 1.0 / input.media[0].sample_rate);
	CPPUNIT_ASSERT_EQUAL (std::string ("#abcdef"), audio.clips[0].color);
	CPPUNIT_ASSERT (!audio.clips[1].enabled);
	auto const& midi = track_named (output, "Notes");
	CPPUNIT_ASSERT_EQUAL (size_t (1), midi.clips.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, midi.clips[0].position.value, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, midi.clips[0].duration, 1e-6);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1, midi.clips[0].offset.value, 1e-6);
	CPPUNIT_ASSERT_EQUAL (size_t (2), midi.clips[0].notes.size ());
	for (size_t i = 0; i < 2; ++i) {
		auto const& expected = input.tracks[3].clips[0].notes[i];
		auto const& actual = midi.clips[0].notes[i];
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.time, actual.time, 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.duration, actual.duration, 1e-6);
		CPPUNIT_ASSERT_EQUAL (expected.key, actual.key);
		CPPUNIT_ASSERT_EQUAL (expected.channel, actual.channel);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.velocity, actual.velocity, 1e-6);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.release_velocity, actual.release_velocity, 1e-6);
	}
	CPPUNIT_ASSERT_EQUAL (size_t (1), output.media.size ());
	PX::ProjectArchive (fixture.path ("output.dawproject")).copy_file (output.media[0].path, fixture.path ("verify.wav"));
	SndFileImportableSource source (fixture.path ("verify.wav"));
	CPPUNIT_ASSERT_EQUAL (uint32_t (2), source.channels ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (input.media[0].sample_rate * 2), source.length ());
	std::vector<Sample> pcm (source.length () * source.channels ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (pcm.size ()), source.read (pcm.data (), pcm.size ()));
	for (size_t i = 0; i < pcm.size () / 2; ++i) {
		CPPUNIT_ASSERT_EQUAL (float (int (i % 37) - 18) / 32, pcm[2 * i]);
		CPPUNIT_ASSERT_EQUAL (float (int (i % 61) - 30) / 64, pcm[2 * i + 1]);
	}
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("output.dawproject"), fixture.path ("Again"), "Again", fixture.report);
	CPPUNIT_ASSERT (fixture.session);
}

void ProjectSessionTest::multichannel_audio_roundtrip ()
{
	Fixture fixture;
	auto project = example (fixture);
	project.media[0].channels = 4;
	project.tracks.erase (project.tracks.begin () + 1);
	project.tracks.pop_back ();
	auto& quad = project.tracks.back ();
	quad.name = "Quad"; quad.channels = 4; quad.parent.clear (); quad.destination = "master"; quad.pan = .5;
	unsigned rate = project.media[0].sample_rate;
	SF_INFO info = {}; info.channels = 4; info.samplerate = rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	SNDFILE* file = sf_open (fixture.path ("source.wav").c_str (), SFM_WRITE, &info);
	CPPUNIT_ASSERT (file);
	std::vector<float> pcm (rate * 2 * 4);
	for (size_t frame = 0; frame < rate * 2; ++frame) {
		for (unsigned channel = 0; channel < 4; ++channel) { pcm[4 * frame + channel] = (channel % 2 ? -1.f : 1.f) * (channel + 1) / 8; }
	}
	CPPUNIT_ASSERT_EQUAL (sf_count_t (rate * 2), sf_writef_float (file, pcm.data (), rate * 2));
	CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
	package (fixture, project);
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("QuadSession"), "QuadSession", fixture.report);
	CPPUNIT_ASSERT (fixture.session);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("QuadSession"), "QuadSession", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto track = std::dynamic_pointer_cast<ARDOUR::Track> (fixture.session->route_by_name ("Quad"));
	CPPUNIT_ASSERT (track);
	CPPUNIT_ASSERT_EQUAL (4u, track->input ()->n_ports ().n_audio ());
	CPPUNIT_ASSERT_EQUAL (4u, track->output ()->n_ports ().n_audio ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), track->playlist ()->region_list ()->size ());
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Transformed && item.detail.find ("only the first two output channels") != std::string::npos;
	}));
	for (unsigned channel = 2; channel < 4; ++channel) {
		std::vector<std::string> connections;
		track->output ()->audio (channel)->get_connections (connections);
		CPPUNIT_ASSERT (connections.empty ());
	}
	for (auto const& region : *track->playlist ()->region_list ()) {
		auto audio = std::dynamic_pointer_cast<AudioRegion> (region);
		CPPUNIT_ASSERT (audio);
		CPPUNIT_ASSERT_EQUAL (uint32_t (4), audio->n_channels ());
	}
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("QuadAAF"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Unsupported && item.detail.find ("AAF/MIDI preserves mono and stereo media") != std::string::npos;
	}));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("QuadAAF"), Glib::FILE_TEST_EXISTS));
	PX::SessionExchange::ExportOptions rendered;
	rendered.render_mix = true;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("quad-rendered.dawproject"), fixture.report, rendered), PX::ExchangeError);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Unsupported && item.detail.find ("Stereo rendering would omit multichannel audio") != std::string::npos;
	}));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("quad-rendered.dawproject"), Glib::FILE_TEST_EXISTS));
	rendered.render_mix = false; rendered.render_stems = true;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("QuadRenderedAAF"), fixture.report, rendered), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("QuadRenderedAAF"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("quad-rendered.song"), fixture.report, rendered), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("quad-rendered.song"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_studio_one (*fixture.session, fixture.path ("quad-native.song"), fixture.report, PX::SessionExchange::ExportOptions ()), PX::ExchangeError);
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Unsupported && item.detail.find ("premixed stereo media") != std::string::npos;
	}));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("quad-native.song"), Glib::FILE_TEST_EXISTS));
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("quad-return.dawproject"), fixture.report);
	auto returned = PX::DawProject::read (fixture.path ("quad-return.dawproject"));
	CPPUNIT_ASSERT_EQUAL (size_t (1), returned.media.size ());
	CPPUNIT_ASSERT_EQUAL (4u, returned.media[0].channels);
	CPPUNIT_ASSERT_EQUAL (4u, returned.tracks.back ().channels);
	PX::ProjectArchive (fixture.path ("quad-return.dawproject")).copy_file (returned.media[0].path, fixture.path ("quad-verify.wav"));
	SndFileImportableSource source (fixture.path ("quad-verify.wav"));
	CPPUNIT_ASSERT_EQUAL (uint32_t (4), source.channels ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (rate * 2), source.length ());
	std::vector<Sample> actual (pcm.size ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (actual.size ()), source.read (actual.data (), actual.size ()));
	for (size_t i = 0; i < pcm.size (); ++i) { CPPUNIT_ASSERT_EQUAL (pcm[i], actual[i]); }
}

void ProjectSessionTest::bad_media ()
{
	Fixture fixture;
	auto project = example (fixture);
	project.media[0].sample_rate /= 2;
	package (fixture, project);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Session"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::existing_destination ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	CPPUNIT_ASSERT_EQUAL (0, g_mkdir (fixture.path ("Session").c_str (), 0700));
	std::ofstream (fixture.path ("Session/keep.txt")) << "keep";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT_EQUAL (std::string ("keep"), Glib::file_get_contents (fixture.path ("Session/keep.txt")));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

#ifdef PLATFORM_WINDOWS
void ProjectSessionTest::retry_locked_publish ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	bool acquired = false;
	gint64 locked_at = 0;
	std::thread release;
	auto cancel = [&] {
		if (acquired || AudioEngine::instance ()->session ()) { return false; }
		for (auto const& name : Glib::Dir (fixture.root)) {
			if (name.find (".ardour-exchange-") != 0) { continue; }
			std::string session = fixture.path (name + "/session");
			if (!Glib::file_test (Glib::build_filename (session, "Exchange.ardour"), Glib::FILE_TEST_IS_REGULAR)) { continue; }
			gunichar2* wide = g_utf8_to_utf16 (session.c_str (), -1, NULL, NULL, NULL);
			CPPUNIT_ASSERT (wide);
			HANDLE held = CreateFileW (reinterpret_cast<LPCWSTR> (wide), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
			                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
			g_free (wide);
			CPPUNIT_ASSERT (held != INVALID_HANDLE_VALUE);
			acquired = true;
			locked_at = g_get_monotonic_time ();
			release = std::thread ([held] { std::this_thread::sleep_for (std::chrono::milliseconds (400)); CloseHandle (held); });
			break;
		}
		return false;
	};
	std::exception_ptr failure;
	try {
		fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"),
		                                                             fixture.path ("Session"), "Exchange", fixture.report, cancel);
	} catch (...) { failure = std::current_exception (); }
	if (release.joinable ()) { release.join (); }
	if (failure) { std::rethrow_exception (failure); }
	CPPUNIT_ASSERT (acquired);
	CPPUNIT_ASSERT (g_get_monotonic_time () - locked_at >= 350000);
	CPPUNIT_ASSERT (Glib::file_test (fixture.path ("Session/Exchange.ardour"), Glib::FILE_TEST_IS_REGULAR));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}
#endif

void ProjectSessionTest::corrupt_media ()
{
	Fixture fixture;
	auto project = example (fixture);
	std::ofstream (fixture.path ("source.wav"), std::ios::binary | std::ios::trunc) << "invalid audio";
	package (fixture, project);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Session"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::cancel_import ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	// Cancel after session creation, not merely while checking the archive.
	bool reached_session = false;
	auto cancel = [&] { return reached_session = (AudioEngine::instance ()->session () != NULL); };
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (reached_session);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Session"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::reject_overlapping_notes ()
{
	Fixture fixture;
	auto project = example (fixture);
	project.tracks[3].clips[0].notes.push_back (project.tracks[3].clips[0].notes[0]);
	package (fixture, project);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::cancel_populated_import ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	bool reached_regions = false;
	auto cancel = [&] {
		if (auto session = AudioEngine::instance ()->session ()) {
			for (auto const& route : *session->get_routes ()) {
				auto track = std::dynamic_pointer_cast<ARDOUR::Track> (route);
				if (track && track->playlist () && !track->playlist ()->region_list ()->empty ()) {
					reached_regions = true;
					return true;
				}
			}
		}
		return false;
	};
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (reached_regions);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Session"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::cancel_reopened_import ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	RecentSessions before;
	CPPUNIT_ASSERT (read_recent_sessions (before) >= 0);
	bool reached_final = false;
	auto cancel = [&] {
		return reached_final = (AudioEngine::instance ()->session () && Glib::file_test (fixture.path ("Session/Exchange.ardour"), Glib::FILE_TEST_IS_REGULAR));
	};
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (reached_final);
	CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Session"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
	RecentSessions after;
	CPPUNIT_ASSERT (read_recent_sessions (after) >= 0);
	CPPUNIT_ASSERT (before == after);
}

void ProjectSessionTest::cancel_export ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report);
	bool reached_audio = false;
	auto cancel = [&] { return reached_audio = stages (fixture.root) > 0; };
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("cancelled.dawproject"), cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (reached_audio);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("cancelled.dawproject"), Glib::FILE_TEST_EXISTS));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}

void ProjectSessionTest::reject_unmapped_processing ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report);
	bool changed = false;
	for (auto const& route : *fixture.session->get_routes ()) {
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (route);
		if (!track) { continue; }
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) { audio->set_opaque (true); audio->set_position (Temporal::timepos_t (samplepos_t (0))); changed = true; }
		}
	}
	CPPUNIT_ASSERT (changed);
	std::vector<PX::Diagnostic> report;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("lossy.dawproject"), report), PX::ExchangeError);
	std::set<std::string> unsupported;
	for (auto const& item : report) { if (item.disposition == PX::Disposition::Unsupported) { unsupported.insert (item.object); } }
	CPPUNIT_ASSERT (unsupported.count ("Offset audio"));
	CPPUNIT_ASSERT (unsupported.count ("Muted audio"));
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("lossy.dawproject"), Glib::FILE_TEST_EXISTS));
}

void ProjectSessionTest::existing_export ()
{
	Fixture fixture;
	package (fixture, example (fixture));
	fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path ("input.dawproject"), fixture.path ("Session"), "Exchange", fixture.report);
	std::ofstream (fixture.path ("existing.dawproject"), std::ios::binary) << "keep";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("existing.dawproject")), PX::ExchangeError);
	CPPUNIT_ASSERT_EQUAL (std::string ("keep"), Glib::file_get_contents (fixture.path ("existing.dawproject")));
	CPPUNIT_ASSERT_EQUAL (size_t (0), stages (fixture.root));
}
