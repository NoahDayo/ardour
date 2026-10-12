/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <cppunit/extensions/HelperMacros.h>
#include <sndfile.h>

#include "ardour/audioengine.h"
#include "ardour/audioregion.h"
#include "ardour/gain_control.h"
#include "ardour/automation_list.h"
#include "pbd/control_math.h"
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/location.h"
#include "ardour/logic_project.h"
#include "temporal/tempo.h"
#include "ardour/playlist.h"
#include "ardour/project_session.h"
#include "ardour/recent_sessions.h"
#include "ardour/session.h"
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

void native_package (Fixture& fixture, bool processing = false, bool variable = false)
{
	auto project = example (fixture);
	auto uri = [] (const std::string& path) {
		gchar* raw = g_filename_to_uri (path.c_str (), NULL, NULL);
		CPPUNIT_ASSERT (raw);
		std::string result (raw); g_free (raw); return result;
	};
	PX::ProjectArchiveWriter song (fixture.path ("input.song"));
	song.add_text ("metainfo.xml", "<MetaInformation><Attribute id=\"Document:MimeType\" value=\"application/x.presonus-song\"/><Attribute id=\"Document:FormatVersion\" value=\"9\"/></MetaInformation>");
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
	song.add_text ("Song/song.xml", std::string (R"(<Song><Attributes x:id="Root"><Attributes x:id="timeContext">)") + transport +
	               R"(</Attributes><List x:id="Tracks"><MarkerTrack timeFormat="2"><MarkerEvent markerType="2")" +
	               (variable ? R"( start="2.3" timeFormat="0")" : "") + R"(/><MarkerEvent markerType="3" start="12"/><MarkerEvent name="Verse" start="8"/></MarkerTrack>
<MediaTrack trackID="audio" mediaType="Audio" name="Native audio" timeFormat="0" color="FF34AB12"><UID x:id="channelID" uid="channel"/>
<List x:id="Events">)" + events + R"(</List></MediaTrack></List></Attributes></Song>)");
	song.add_text ("Song/mediapool.xml", "<MediaPool><Attributes x:id=\"documentPath\" url=\"" + uri (fixture.path ("input.song")) + "\"/><AudioClip mediaID=\"media\"><Url x:id=\"path\" url=\"" + uri (fixture.path ("source.wav")) + "\"/><Attributes x:id=\"format\" sampleRate=\"" + std::to_string (project.media[0].sample_rate) + "\" numChannels=\"2\" frameCount=\"" + std::to_string (project.media[0].sample_rate * 2) + "\"/></AudioClip></MediaPool>");
	song.add_text ("Devices/audiomixer.xml", std::string (R"(<AudioMixer><AudioTrackChannel gain="0.75" pan=")") + (variable ? "0.5" : "0.3") + R"("><UID x:id="uniqueID" uid="channel"/><Connection x:id="destination" objectID="master/Input"/></AudioTrackChannel><AudioOutputChannel label="Main"><UID x:id="uniqueID" uid="master"/></AudioOutputChannel></AudioMixer>)");
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

} // namespace

class ProjectSessionTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE (ProjectSessionTest);
	CPPUNIT_TEST (roundtrip);
	CPPUNIT_TEST (bad_media);
	CPPUNIT_TEST (corrupt_media);
	CPPUNIT_TEST (existing_destination);
	CPPUNIT_TEST (cancel_import);
	CPPUNIT_TEST (cancel_populated_import);
	CPPUNIT_TEST (cancel_reopened_import);
	CPPUNIT_TEST (reject_overlapping_notes);
	CPPUNIT_TEST (cancel_export);
	CPPUNIT_TEST (existing_export);
	CPPUNIT_TEST (reject_unmapped_processing);
	CPPUNIT_TEST (native_song_import);
	CPPUNIT_TEST (native_song_timing_and_gain);
	CPPUNIT_TEST (native_song_cancel);
	CPPUNIT_TEST (native_song_unsupported);
	CPPUNIT_TEST (marker_roundtrip);
	CPPUNIT_TEST (studio_pro_export);
	CPPUNIT_TEST (variable_timing);
	CPPUNIT_TEST (second_based_ramp);
	CPPUNIT_TEST (transport_collision);
	CPPUNIT_TEST (mixer_automation_roundtrip);
	CPPUNIT_TEST (native_fader_automation);
	CPPUNIT_TEST (automation_collision);
	CPPUNIT_TEST (region_fades_roundtrip);
	CPPUNIT_TEST_SUITE_END ();
public:
	void roundtrip ();
	void bad_media ();
	void corrupt_media ();
	void existing_destination ();
	void cancel_import ();
	void cancel_populated_import ();
	void cancel_reopened_import ();
	void reject_overlapping_notes ();
	void cancel_export ();
	void existing_export ();
	void reject_unmapped_processing ();
	void native_song_import ();
	void native_song_timing_and_gain ();
	void native_song_cancel ();
	void native_song_unsupported ();
	void marker_roundtrip ();
	void studio_pro_export ();
	void variable_timing ();
	void second_based_ramp ();
	void transport_collision ();
	void mixer_automation_roundtrip ();
	void native_fader_automation ();
	void region_fades_roundtrip ();
	void automation_collision ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (ProjectSessionTest);

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
	auto render = [&] (std::shared_ptr<AudioRegion> audio) {
		std::vector<float> output (audio->length_samples () * 2, 0);
		std::vector<float> mix (256), gain (256);
		for (unsigned channel = 0; channel < 2; ++channel) {
			for (samplecnt_t at = 0; at < audio->length_samples (); at += 256) {
				auto count = std::min (samplecnt_t (256), audio->length_samples () - at);
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
			expected[audio->name ()] = render (audio); ++i;
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
	input.markers = {{"Intro", PX::Time (0)}, {u8"Refrain 音楽", PX::Time (12.5)}, {"Cue", PX::Time (2.1, PX::TimeUnit::Seconds)}};
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

void ProjectSessionTest::roundtrip ()
{
	Fixture fixture;
	PX::Project input = example (fixture);
	package (fixture, input);
	const std::string session_name = u8"音楽 Session";
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
