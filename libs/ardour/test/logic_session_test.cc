/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <cppunit/extensions/HelperMacros.h>
#include <sndfile.h>
#include "aaf/libaaf.h"
#include "evoral/SMF.h"
#include "ardour/audioengine.h"
#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/gain_control.h"
#include "ardour/logic_project.h"
#include "ardour/location.h"
#include "ardour/midi_region.h"
#include "ardour/midi_model.h"
#include "ardour/panner_shell.h"
#include "ardour/playlist.h"
#include "ardour/track.h"
#include "ardour/project_session.h"
#include "ardour/session.h"
#include "ardour/studio_one_song.h"
#include "test_util.h"

using namespace ARDOUR;
namespace PX = ARDOUR::ProjectExchange;
namespace {
struct EnvironmentOverride {
	EnvironmentOverride (const char* key, const std::string& value) : key (key), present (g_getenv (key) != nullptr), old (present ? g_getenv (key) : "") { g_setenv (key, value.c_str (), true); }
	~EnvironmentOverride () { if (present) { g_setenv (key, old.c_str (), true); } else { g_unsetenv (key); } }
	const char* key;
	bool present;
	std::string old;
};
struct Fixture {
	Fixture (float level = .125f, bool midi_control = false) : root (new_test_output_dir ("logic-exchange"))
	{
		create_and_start_dummy_backend ();
		PX::Project project;
		project.tempo = 100;
		project.numerator = 3;
		unsigned int rate = AudioEngine::instance ()->sample_rate ();
		SF_INFO info = {};
		info.channels = 2; info.samplerate = rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
		SNDFILE* file = sf_open (path ("source.wav").c_str (), SFM_WRITE, &info);
		CPPUNIT_ASSERT (file);
		std::vector<float> pcm (rate * 4, level);
		for (size_t i = 1; i < pcm.size (); i += 2) { pcm[i] = -level; }
		CPPUNIT_ASSERT_EQUAL (sf_count_t (rate * 2), sf_writef_float (file, pcm.data (), rate * 2));
		CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
		project.media.push_back ({"source", "audio/source.wav", rate, 2, 2});
		PX::Track master; master.id = "master"; master.name = "Master"; master.kind = PX::TrackKind::Bus; master.master = true;
		project.tracks.push_back (master);
		PX::Track audio; audio.id = "audio"; audio.name = "Stereo"; audio.color = "#13579b"; audio.destination = "master"; audio.volume = .75;
		PX::Clip clip; clip.name = u8"Audio café"; clip.color = "#abcdef"; clip.media = "source";
		clip.position = PX::Time (1.25, PX::TimeUnit::Seconds); clip.duration = .5; clip.offset = PX::Time (.375, PX::TimeUnit::Seconds);
		audio.clips.push_back (clip);
		clip.position.value = 2.5; clip.enabled = false; audio.clips.push_back (clip);
		project.tracks.push_back (audio);
		PX::Track midi; midi.id = "midi"; midi.name = "Notes"; midi.color = "#2468ac"; midi.kind = PX::TrackKind::MIDI; midi.destination = "master";
		clip = PX::Clip (); clip.kind = PX::ClipKind::MIDI; clip.name = "Phrase One"; clip.color = "#fedcba";
		clip.position = PX::Time (4); clip.duration = 4; clip.offset = PX::Time (1);
		PX::Note note; note.time = 1.25; note.duration = 1.5; note.key = 60; note.channel = 2; note.velocity = 93.0 / 127; note.release_velocity = 45.0 / 127;
		if (midi_control) {
			PX::MidiControl control; control.type = 0xb0; control.channel = 2; control.number = 64;
			control.points = {{.5, 127, false}, {1.5, 0, false}, {4.5, 127, false}};
			clip.midi_controls.push_back (control);
		}
		clip.notes.push_back (note); midi.clips.push_back (clip); project.tracks.push_back (midi);
		project.markers.push_back ({u8"Refrain 音楽", PX::Time (12.5)});
		PX::DawProject::write (project, path ("input.dawproject"), {{"source", path ("source.wav")}});
		session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), path ("input.dawproject"), path ("Session"), "Exchange", report);
		CPPUNIT_ASSERT_EQUAL (0, g_unlink (path ("source.wav").c_str ()));
		CPPUNIT_ASSERT_EQUAL (0, g_unlink (path ("input.dawproject").c_str ()));
		AudioEngine::instance ()->remove_session (); session.reset ();
		session.reset (new Session (*AudioEngine::instance (), path ("Session"), "Exchange", nullptr, "", true));
		AudioEngine::instance ()->set_session (session.get ());
	}
	~Fixture () { AudioEngine::instance ()->remove_session (); session.reset (); stop_and_destroy_backend (); }
	std::string path (const std::string& name) const { return Glib::build_filename (root, name); }
	void clean_failure ()
	{
		CPPUNIT_ASSERT (!Glib::file_test (path ("Output"), Glib::FILE_TEST_EXISTS));
		for (auto const& name : Glib::Dir (root)) { CPPUNIT_ASSERT (name.find (".ardour-exchange-") != 0); }
		CPPUNIT_ASSERT (AudioEngine::instance ()->session () == session.get ());
	}
	std::string root;
	std::unique_ptr<Session> session;
	std::vector<PX::Diagnostic> report;
};
}

class LogicSessionTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE (LogicSessionTest);
	CPPUNIT_TEST (saved_session_export);
	CPPUNIT_TEST (controller_clip_restoration);
	CPPUNIT_TEST (range_marker_roundtrip);
	CPPUNIT_TEST (paired_mono_layout);
	CPPUNIT_TEST (resampled_source_end);
	CPPUNIT_TEST (subsample_terminal_envelope);
	CPPUNIT_TEST (stereo_balance_roundtrip);
	CPPUNIT_TEST (stereo_balance_gain_automation_roundtrip);
	CPPUNIT_TEST (stereo_balance_automation_roundtrip);
	CPPUNIT_TEST (stereo_silent_balance_roundtrip);
	CPPUNIT_TEST (stereo_combined_automation_roundtrip);
	CPPUNIT_TEST (stereo_combined_silence_roundtrip);
	CPPUNIT_TEST (stereo_pairwise_overlap);
	CPPUNIT_TEST (existing_destination);
	CPPUNIT_TEST (missing_runtime);
	CPPUNIT_TEST (writer_failure);
	CPPUNIT_TEST (writer_missing_outputs);
	CPPUNIT_TEST (cancel_running_writer);
	CPPUNIT_TEST (out_of_range_audio);
	CPPUNIT_TEST (coordinated_import);
	CPPUNIT_TEST (import_cancel_and_failure);
	CPPUNIT_TEST (midi_validation);
	CPPUNIT_TEST (flat_mixer_automation);
	CPPUNIT_TEST (aaf_transitions);
	CPPUNIT_TEST_SUITE_END ();
public:
	void saved_session_export ();
	void controller_clip_restoration ();
	void range_marker_roundtrip ();
	void paired_mono_layout ();
	void resampled_source_end ();
	void subsample_terminal_envelope ();
	void stereo_balance_roundtrip ();
	void stereo_balance_gain_automation_roundtrip ();
	void stereo_balance_automation_roundtrip ();
	void stereo_silent_balance_roundtrip ();
	void stereo_combined_automation_roundtrip ();
	void stereo_combined_silence_roundtrip ();
	void stereo_pairwise_overlap ();
	void existing_destination ();
	void missing_runtime ();
	void writer_failure ();
	void writer_missing_outputs ();
	void cancel_running_writer ();
	void out_of_range_audio ();
	void coordinated_import ();
	void import_cancel_and_failure ();
	void midi_validation ();
	void flat_mixer_automation ();
	void aaf_transitions ();
};
CPPUNIT_TEST_SUITE_REGISTRATION (LogicSessionTest);

void LogicSessionTest::stereo_pairwise_overlap ()
{
	Fixture fixture;
	const samplecnt_t rate = fixture.session->sample_rate ();
	auto original = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Stereo"));
	CPPUNIT_ASSERT (original);
	auto regions = original->playlist ()->region_list ();
	CPPUNIT_ASSERT_EQUAL (size_t (2), regions->size ());
	for (auto const& region : *regions) {
		auto audio = std::dynamic_pointer_cast<AudioRegion> (region);
		CPPUNIT_ASSERT (audio);
		audio->set_opaque (false);
		if (audio->position_sample () > rate * 2) {
			audio->set_position (Temporal::timepos_t (samplepos_t (rate * 3 / 2)));
			audio->set_muted (false);
		}
	}
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("StereoOverlap"), fixture.report);
	PX::LogicProject aaf (fixture.path ("StereoOverlap/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	CPPUNIT_ASSERT_EQUAL (size_t (2), track.clips.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, track.clips[0].position.value, 1.0 / rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.5, track.clips[1].position.value, 1.0 / rate);
	CPPUNIT_ASSERT_EQUAL (size_t (1), aaf.project ().media.size ());
	regions.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("StereoOverlap/Project.aaf"),
	                                                      "", fixture.path ("OverlapImported"), "OverlapImported", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("OverlapImported"), "OverlapImported", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Stereo"));
	CPPUNIT_ASSERT (restored);
	regions = restored->playlist ()->region_list ();
	CPPUNIT_ASSERT_EQUAL (size_t (2), regions->size ());
	for (unsigned channel = 0; channel < 2; ++channel) {
		for (auto const& check : std::array<std::pair<double, double>, 3> {{{1.4, .125}, {1.6, .25}, {1.9, .125}}}) {
			samplepos_t at = samplepos_t (check.first * rate);
			float actual = 0;
			for (auto const& region : *regions) {
				auto audio = std::dynamic_pointer_cast<AudioRegion> (region);
				CPPUNIT_ASSERT (audio && audio->n_channels () == 2);
				if (at < audio->position_sample () || at >= audio->position_sample () + audio->length_samples ()) { continue; }
				float sample = 0, mix = 0, gain = 0;
				CPPUNIT_ASSERT_EQUAL (samplecnt_t (1), audio->read_at (&sample, &mix, &gain, at, 1, channel));
				actual += sample;
			}
			CPPUNIT_ASSERT_DOUBLES_EQUAL (channel ? -check.second : check.second, actual, 1e-5);
		}
	}
	regions.reset (); restored.reset ();
}

void LogicSessionTest::resampled_source_end ()
{
	Fixture fixture;
	const char* data = g_getenv ("ARDOUR_TEST_PATH"); CPPUNIT_ASSERT (data);
	{
		EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", Glib::build_filename (data, "aaf_stereo_layout.py"));
		PX::SessionExchange::export_logic (*fixture.session, fixture.path ("ResampleFixtures"), fixture.report);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	CPPUNIT_ASSERT_EQUAL (0, AudioEngine::instance ()->set_sample_rate (48000));
	const std::string source = fixture.path ("ResampleFixtures/ResampleEnd.aaf");
	PX::LogicProject model (source);
	CPPUNIT_ASSERT (!PX::has_unsupported_features (model.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (1), model.project ().tracks[1].clips.size ());
	CPPUNIT_ASSERT_EQUAL (samplecnt_t (48000), AudioEngine::instance ()->sample_rate ());
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), source, "",
		fixture.path ("Resampled"), "Resampled", fixture.report);
	auto track = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Stereo split"));
	CPPUNIT_ASSERT (track && track->playlist ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), track->playlist ()->region_list ()->size ());
	auto region = std::dynamic_pointer_cast<AudioRegion> (*track->playlist ()->region_list ()->begin ());
	CPPUNIT_ASSERT (region && region->audio_source ());
	CPPUNIT_ASSERT (region->start_sample () + region->length_samples () <= region->audio_source ()->length ().samples ());
	CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
		return item.detail.find ("Clip end rounded to available source frames") != std::string::npos;
	}));
}

void LogicSessionTest::subsample_terminal_envelope ()
{
	const double sample = 1.0 / 48000;
	const double end = 308254 * sample;
	PX::Automation envelope;
	envelope.unit = PX::TimeUnit::Seconds;
	envelope.points = {{0, 1, true}, {end - .000008, .5, true}, {end, .5, true}};
	auto rounded = PX::linearize_automation (envelope, sample);
	CPPUNIT_ASSERT_EQUAL (size_t (2), rounded.points.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (end, rounded.points.back ().time, sample * .001);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, rounded.points.back ().value, 1e-12);
	envelope.points.back ().value = .25;
	CPPUNIT_ASSERT_THROW (PX::linearize_automation (envelope, sample), PX::ExchangeError);
}

void LogicSessionTest::paired_mono_layout ()
{
	Fixture fixture;
	const char* data = g_getenv ("ARDOUR_TEST_PATH"); CPPUNIT_ASSERT (data);
	{
		EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", Glib::build_filename (data, "aaf_stereo_layout.py"));
		PX::SessionExchange::export_logic (*fixture.session, fixture.path ("SplitFixtures"), fixture.report);
	}
	const std::string source = fixture.path ("SplitFixtures/Project.aaf");
	PX::LogicProject split (source);
	CPPUNIT_ASSERT (!PX::has_unsupported_features (split.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (3), split.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Stereo split"), split.project ().tracks[1].name);
	CPPUNIT_ASSERT_EQUAL (2u, split.project ().tracks[1].channels);
	CPPUNIT_ASSERT_EQUAL (size_t (1), split.project ().tracks[1].clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Empty stereo"), split.project ().tracks[2].name);
	CPPUNIT_ASSERT_EQUAL (2u, split.project ().tracks[2].channels);
	CPPUNIT_ASSERT (split.project ().tracks[2].clips.empty ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), split.project ().media.size ());
	const std::string pcm = fixture.path ("split.wav");
	split.copy_media (split.project ().media[0], pcm);
	SF_INFO info = {};
	SNDFILE* file = sf_open (pcm.c_str (), SFM_READ, &info);
	CPPUNIT_ASSERT (file);
	CPPUNIT_ASSERT_EQUAL (2, info.channels);
	std::array<float, 2> sample {};
	CPPUNIT_ASSERT_EQUAL (sf_count_t (1), sf_readf_float (file, sample.data (), 1));
	CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
	CPPUNIT_ASSERT_EQUAL (.125f, sample[0]);
	CPPUNIT_ASSERT_EQUAL (-.25f, sample[1]);
	PX::LogicProject mismatched (fixture.path ("SplitFixtures/Mismatched.aaf"));
	CPPUNIT_ASSERT_EQUAL (size_t (3), mismatched.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (1u, mismatched.project ().tracks[1].channels);
	CPPUNIT_ASSERT_EQUAL (1u, mismatched.project ().tracks[2].channels);
	PX::LogicProject independent (fixture.path ("SplitFixtures/IndependentLevels.aaf"));
	CPPUNIT_ASSERT_EQUAL (size_t (3), independent.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (1u, independent.project ().tracks[1].channels);
	CPPUNIT_ASSERT_EQUAL (1u, independent.project ().tracks[2].channels);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.625, PX::automation_value (independent.project ().tracks[1].volume_automation, .5625), 1e-7);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.3125, PX::automation_value (independent.project ().tracks[2].volume_automation, .5625), 1e-7);
	PX::LogicProject terminal (fixture.path ("SplitFixtures/TerminalDuplicate.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (terminal.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), terminal.project ().tracks[1].volume_automation.points.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, terminal.project ().tracks[1].volume_automation.points.back ().value, 1e-7);
	CPPUNIT_ASSERT (std::any_of (terminal.project ().report.begin (), terminal.project ().report.end (), [] (const PX::Diagnostic& item) {
		return item.detail.find ("Repeated AAF control points") != std::string::npos;
	}));
	PX::LogicProject rounded (fixture.path ("SplitFixtures/RoundedDuplicate.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (rounded.project ()));
	const auto& rounded_curve = rounded.project ().tracks[1].volume_automation;
	CPPUNIT_ASSERT_EQUAL (size_t (2), rounded_curve.points.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0, rounded_curve.points.front ().time, 1e-7);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, rounded_curve.points.back ().time, 1e-7);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, rounded_curve.points.front ().value, 1e-7);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, rounded_curve.points.back ().value, 1e-7);
	CPPUNIT_ASSERT (std::any_of (rounded.project ().report.begin (), rounded.project ().report.end (), [] (const PX::Diagnostic& item) {
		return item.detail.find ("Repeated identical AAF control points") != std::string::npos;
	}));
	CPPUNIT_ASSERT (std::any_of (rounded.project ().report.begin (), rounded.project ().report.end (), [] (const PX::Diagnostic& item) {
		return item.detail.find ("one operation tick beyond the end") != std::string::npos;
	}));
	CPPUNIT_ASSERT_THROW (PX::LogicProject (fixture.path ("SplitFixtures/ConflictingDuplicate.aaf")), PX::ExchangeError);
	PX::LogicProject partial (fixture.path ("SplitFixtures/PartialTag.aaf"));
	CPPUNIT_ASSERT_EQUAL (size_t (3), partial.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (1u, partial.project ().tracks[1].channels);
	CPPUNIT_ASSERT_EQUAL (1u, partial.project ().tracks[2].channels);
	CPPUNIT_ASSERT (std::any_of (partial.project ().report.begin (), partial.project ().report.end (), [] (const PX::Diagnostic& item) {
		return item.detail.find ("channel levels remain separate mono tracks") != std::string::npos;
	}));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), source, "", fixture.path ("SplitImported"), "SplitImported", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("SplitImported"), "SplitImported", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto stereo = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Stereo split"));
	auto empty = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Empty stereo"));
	CPPUNIT_ASSERT (stereo && empty);
	CPPUNIT_ASSERT_EQUAL (size_t (1), stereo->playlist ()->region_list ()->size ());
	CPPUNIT_ASSERT (empty->playlist ()->region_list ()->empty ());
	auto audio = std::dynamic_pointer_cast<AudioRegion> (*stereo->playlist ()->region_list ()->begin ());
	CPPUNIT_ASSERT (audio);
	CPPUNIT_ASSERT_EQUAL (uint32_t (2), audio->n_channels ());
	for (unsigned channel = 0; channel < 2; ++channel) {
		auto source = std::dynamic_pointer_cast<AudioSource> (audio->source (channel));
		std::array<float, 31> samples {};
		CPPUNIT_ASSERT_EQUAL (samplecnt_t (samples.size ()), source->read (samples.data (), 0, samples.size ()));
		for (float value : samples) { CPPUNIT_ASSERT_EQUAL (channel ? -.25f : .125f, value); }
	}
}

void LogicSessionTest::stereo_balance_roundtrip ()
{
	Fixture fixture;
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	CPPUNIT_ASSERT_EQUAL (std::string ("http://ardour.org/plugin/panner_balance"), original->panner_shell ()->current_panner_uri ());
	CPPUNIT_ASSERT (original->pan_azimuth_control ());
	original->pan_azimuth_control ()->set_value (.25, PBD::Controllable::NoGroup);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Balance"), fixture.report);
	PX::LogicProject aaf (fixture.path ("Balance/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (2u, aaf.project ().tracks[1].channels);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, aaf.project ().tracks[1].volume, 1e-7);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, aaf.project ().tracks[1].pan, 1e-7);
	original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Balance/Project.aaf"),
	                                                       fixture.path ("Balance/Project.mid"), fixture.path ("Returned"), "Returned", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Returned"), "Returned", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->pan_azimuth_control ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, restored->pan_azimuth_control ()->get_value (), 1e-7);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("balance-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("balance-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& track) { return track.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, found->pan, 1e-7);
}

void LogicSessionTest::stereo_balance_gain_automation_roundtrip ()
{
	Fixture fixture;
	const std::array<std::pair<double, double>, 3> levels {{{0, .4}, {1, .6}, {2, .8}}};
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	CPPUNIT_ASSERT (original->pan_azimuth_control ());
	original->pan_azimuth_control ()->set_value (.25, PBD::Controllable::NoGroup);
	auto control = original->gain_control ();
	control->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (control->alist ()->set_interpolation (Evoral::ControlList::Linear));
	control->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .4, false, false);
	control->alist ()->add (Temporal::timepos_t (samplepos_t (fixture.session->sample_rate () * 2)), .8, false, false);
	control->set_automation_state (Play);
	for (double pan : {0.0, 1.0}) {
		original->pan_azimuth_control ()->set_value (pan, PBD::Controllable::NoGroup);
		const std::string folder = pan == 0 ? "HardLeftGain" : "HardRightGain";
		PX::SessionExchange::export_logic (*fixture.session, fixture.path (folder), fixture.report);
		PX::LogicProject hard (fixture.path (folder + "/Project.aaf"));
		CPPUNIT_ASSERT (!PX::has_unsupported_features (hard.project ()));
		CPPUNIT_ASSERT_EQUAL (size_t (2), hard.project ().tracks.size ());
		CPPUNIT_ASSERT_EQUAL (2u, hard.project ().tracks[1].channels);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (pan, hard.project ().tracks[1].pan, 2e-5);
	}
	original->pan_azimuth_control ()->set_value (.25, PBD::Controllable::NoGroup);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("BalancedGain"), fixture.report);
	PX::LogicProject aaf (fixture.path ("BalancedGain/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, track.pan, 2e-5);
	CPPUNIT_ASSERT (track.pan_automation.points.empty ());
	for (auto expected : levels) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, PX::automation_value (track.volume_automation, expected.first), 2e-5);
	}
	control.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("BalancedGain/Project.aaf"),
	                                                       fixture.path ("BalancedGain/Project.mid"), fixture.path ("BalancedGainReturn"), "BalancedGainReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("BalancedGainReturn"), "BalancedGainReturn", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->pan_azimuth_control ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, restored->pan_azimuth_control ()->get_value (), 2e-5);
	for (auto expected : levels) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, restored->gain_control ()->alist ()->eval (
		                              Temporal::timepos_t (samplepos_t (expected.first * fixture.session->sample_rate ()))), 2e-5);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("balanced-gain-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("balanced-gain-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.25, found->pan, 2e-5);
	for (auto expected : levels) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, PX::automation_value (found->volume_automation, expected.first), 2e-5);
	}
}

void LogicSessionTest::stereo_balance_automation_roundtrip ()
{
	Fixture fixture;
	const std::array<std::pair<double, double>, 3> positions {{{0, .25}, {1, .5}, {2, .75}}};
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	CPPUNIT_ASSERT_EQUAL (std::string ("http://ardour.org/plugin/panner_balance"), original->panner_shell ()->current_panner_uri ());
	auto control = original->pan_azimuth_control ();
	CPPUNIT_ASSERT (control);
	control->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (control->alist ()->set_interpolation (Evoral::ControlList::Linear));
	control->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .25, false, false);
	control->alist ()->add (Temporal::timepos_t (samplepos_t (fixture.session->sample_rate () * 2)), .75, false, false);
	control->set_automation_state (Play);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("MovingBalance"), fixture.report);
	PX::LogicProject aaf (fixture.path ("MovingBalance/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, track.volume, 2e-5);
	CPPUNIT_ASSERT (track.volume_automation.points.empty ());
	for (auto expected : positions) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, PX::automation_value (track.pan_automation, expected.first), 2e-5);
	}
	control.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("MovingBalance/Project.aaf"),
	                                                       fixture.path ("MovingBalance/Project.mid"), fixture.path ("MovingReturn"), "MovingReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("MovingReturn"), "MovingReturn", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->pan_azimuth_control ());
	for (auto expected : positions) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, restored->pan_azimuth_control ()->alist ()->eval (
		                              Temporal::timepos_t (samplepos_t (expected.first * fixture.session->sample_rate ()))), 2e-5);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("moving-balance-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("moving-balance-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	for (auto expected : positions) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected.second, PX::automation_value (found->pan_automation, expected.first), 2e-5);
	}
}

void LogicSessionTest::stereo_silent_balance_roundtrip ()
{
	Fixture fixture;
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	auto gain = original->gain_control ();
	auto pan = original->pan_azimuth_control ();
	CPPUNIT_ASSERT (gain && pan);
	gain->set_value (0, PBD::Controllable::NoGroup);
	pan->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (pan->alist ()->set_interpolation (Evoral::ControlList::Linear));
	pan->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .25, false, false);
	pan->alist ()->add (Temporal::timepos_t (samplepos_t (fixture.session->sample_rate () * 2)), .75, false, false);
	pan->set_automation_state (Play);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("SilentBalance"), fixture.report);
	PX::LogicProject aaf (fixture.path ("SilentBalance/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	CPPUNIT_ASSERT_EQUAL (0.0, track.volume);
	CPPUNIT_ASSERT (track.volume_automation.points.empty ());
	for (double seconds : {0.0, .5, 1.0, 1.5, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.25 + .25 * seconds, PX::automation_value (track.pan_automation, seconds), 2e-5);
	}
	gain.reset (); pan.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("SilentBalance/Project.aaf"),
	                                                       fixture.path ("SilentBalance/Project.mid"), fixture.path ("SilentBalanceReturn"), "SilentBalanceReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("SilentBalanceReturn"), "SilentBalanceReturn", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->gain_control () && restored->pan_azimuth_control ());
	CPPUNIT_ASSERT_EQUAL (0.0, restored->gain_control ()->get_value ());
	CPPUNIT_ASSERT (restored->gain_control ()->alist ()->empty ());
	for (double seconds : {0.0, .5, 1.0, 1.5, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.25 + .25 * seconds, restored->pan_azimuth_control ()->alist ()->eval (
		                              Temporal::timepos_t (samplepos_t (seconds * fixture.session->sample_rate ()))), 2e-5);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("silent-balance-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("silent-balance-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	CPPUNIT_ASSERT_EQUAL (0.0, found->volume);
	CPPUNIT_ASSERT (found->volume_automation.points.empty ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, PX::automation_value (found->pan_automation, 1), 2e-5);
}

void LogicSessionTest::stereo_combined_automation_roundtrip ()
{
	Fixture fixture;
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	auto gain = original->gain_control ();
	auto pan = original->pan_azimuth_control ();
	CPPUNIT_ASSERT (gain && pan);
	gain->alist ()->set_time_domain (Temporal::AudioTime);
	pan->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (gain->alist ()->set_interpolation (Evoral::ControlList::Linear));
	CPPUNIT_ASSERT (pan->alist ()->set_interpolation (Evoral::ControlList::Linear));
	const samplepos_t stop = fixture.session->sample_rate () * 2;
	gain->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .4, false, false);
	gain->alist ()->add (Temporal::timepos_t (stop), .8, false, false);
	pan->alist ()->add (Temporal::timepos_t (samplepos_t (0)), .25, false, false);
	pan->alist ()->add (Temporal::timepos_t (stop), .75, false, false);
	gain->set_automation_state (Play);
	pan->set_automation_state (Play);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("CombinedBalance"), fixture.report);
	PX::LogicProject aaf (fixture.path ("CombinedBalance/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	CPPUNIT_ASSERT (!track.volume_automation.points.empty () && !track.pan_automation.points.empty ());
	for (double seconds : {0.0, .25, .5, .75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.4 + .2 * seconds, PX::automation_value (track.volume_automation, seconds), 5e-5);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.25 + .25 * seconds, PX::automation_value (track.pan_automation, seconds), 5e-5);
	}
	gain.reset (); pan.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("CombinedBalance/Project.aaf"),
	                                                       fixture.path ("CombinedBalance/Project.mid"), fixture.path ("CombinedReturn"), "CombinedReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("CombinedReturn"), "CombinedReturn", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->gain_control () && restored->pan_azimuth_control ());
	for (double seconds : {0.0, .25, .5, .75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
		auto position = Temporal::timepos_t (samplepos_t (seconds * fixture.session->sample_rate ()));
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.4 + .2 * seconds, restored->gain_control ()->alist ()->eval (position), 5e-5);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.25 + .25 * seconds, restored->pan_azimuth_control ()->alist ()->eval (position), 5e-5);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("combined-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("combined-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	for (double seconds : {0.0, .25, .5, .75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.4 + .2 * seconds, PX::automation_value (found->volume_automation, seconds), 5e-5);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.25 + .25 * seconds, PX::automation_value (found->pan_automation, seconds), 5e-5);
	}
}

void LogicSessionTest::stereo_combined_silence_roundtrip ()
{
	Fixture fixture;
	auto original = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (original);
	original->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
	auto gain = original->gain_control ();
	auto pan = original->pan_azimuth_control ();
	CPPUNIT_ASSERT (gain && pan);
	gain->alist ()->set_time_domain (Temporal::AudioTime);
	pan->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (gain->alist ()->set_interpolation (Evoral::ControlList::Linear));
	CPPUNIT_ASSERT (pan->alist ()->set_interpolation (Evoral::ControlList::Linear));
	const samplepos_t rate = fixture.session->sample_rate ();
	for (auto point : {std::make_pair (0, .4), std::make_pair (1, 0.0), std::make_pair (2, 0.0), std::make_pair (3, .8)}) {
		gain->alist ()->add (Temporal::timepos_t (samplepos_t (point.first * rate)), point.second, false, false);
	}
	for (auto point : {std::make_pair (0, .25), std::make_pair (1, .3), std::make_pair (2, .7), std::make_pair (3, .75)}) {
		pan->alist ()->add (Temporal::timepos_t (samplepos_t (point.first * rate)), point.second, false, false);
	}
	gain->set_automation_state (Play);
	pan->set_automation_state (Play);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("SilentCombined"), fixture.report);
	PX::LogicProject aaf (fixture.path ("SilentCombined/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (aaf.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), aaf.project ().tracks.size ());
	auto const& track = aaf.project ().tracks[1];
	CPPUNIT_ASSERT_EQUAL (2u, track.channels);
	for (double seconds : {0.0, .5, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
		double expected_gain = seconds < 1 ? .4 * (1 - seconds) : seconds <= 2 ? 0 : .8 * (seconds - 2);
		double expected_pan = seconds < 1 ? .25 + .05 * seconds : seconds <= 2 ? .3 + .4 * (seconds - 1) : .7 + .05 * (seconds - 2);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected_gain, PX::automation_value (track.volume_automation, seconds), 5e-5);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected_pan, PX::automation_value (track.pan_automation, seconds), 5e-5);
	}
	gain.reset (); pan.reset (); original.reset ();
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("SilentCombined/Project.aaf"),
	                                                       fixture.path ("SilentCombined/Project.mid"), fixture.path ("SilentReturn"), "SilentReturn", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("SilentReturn"), "SilentReturn", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	auto restored = fixture.session->route_by_name ("Stereo");
	CPPUNIT_ASSERT (restored && restored->gain_control () && restored->pan_azimuth_control ());
	for (double seconds : {1.0, 1.25, 1.5, 1.75, 2.0}) {
		auto position = Temporal::timepos_t (samplepos_t (seconds * fixture.session->sample_rate ()));
		CPPUNIT_ASSERT_DOUBLES_EQUAL (0, restored->gain_control ()->alist ()->eval (position), 5e-5);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.3 + .4 * (seconds - 1), restored->pan_azimuth_control ()->alist ()->eval (position), 5e-5);
	}
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("silent-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("silent-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Stereo"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, PX::automation_value (found->pan_automation, 1.5), 5e-5);
}

void LogicSessionTest::aaf_transitions ()
{
	Fixture fixture;
	const samplecnt_t rate = fixture.session->sample_rate ();
	{
		const char* data = g_getenv ("ARDOUR_TEST_PATH"); CPPUNIT_ASSERT (data);
		EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", Glib::build_filename (data, "aaf_transitions.py"));
		PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Fixtures"), fixture.report);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	for (std::string mode : {"long", "bad-cut", "bad-level", "bad-time", "missing-end", "one-point", "spline",
	                         "two-bad-incoming", "two-bad-outgoing", "two-missing-end", "two-one-point", "two-spline-in", "two-spline-out", "two-constant-negative", "two-constant-excess"}) {
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Fixtures/" + mode + ".aaf"), "", fixture.path ("Invalid"), "Invalid", fixture.report), PX::ExchangeError);
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Invalid"), Glib::FILE_TEST_EXISTS));
		CPPUNIT_ASSERT (!AudioEngine::instance ()->session ());
	}
	{
		auto source = fixture.path ("Fixtures/spline.aaf");
		PX::LogicProject checked (source);
		CPPUNIT_ASSERT (PX::has_unsupported_features (checked.project ()));
		CPPUNIT_ASSERT (!PX::has_aaf_reader_errors (checked.project ()));
		fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), source, "",
		                         fixture.path ("PartialSpline"), "PartialSpline", fixture.report, PX::CancelCheck (), true);
		CPPUNIT_ASSERT (fixture.session);
		CPPUNIT_ASSERT (std::any_of (fixture.report.begin (), fixture.report.end (), [] (const PX::Diagnostic& item) {
			return item.disposition == PX::Disposition::Unsupported && item.detail.find ("dissolve curve") != std::string::npos;
		}));
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("PartialSpline"), "PartialSpline", nullptr, "", true));
		AudioEngine::instance ()->set_session (fixture.session.get ());
		auto track = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Dissolves"));
		CPPUNIT_ASSERT (track);
		CPPUNIT_ASSERT_EQUAL (size_t (2), track->playlist ()->region_list ()->size ());
		PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("partial-spline.dawproject"));
		auto returned = PX::DawProject::read (fixture.path ("partial-spline.dawproject"));
		auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& value) { return value.name == "Dissolves"; });
		CPPUNIT_ASSERT (found != returned.tracks.end ());
		CPPUNIT_ASSERT_EQUAL (size_t (2), found->clips.size ());
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Fixtures/bad-level.aaf"),
		                      "", fixture.path ("InvalidPartial"), "InvalidPartial", fixture.report, PX::CancelCheck (), true), PX::ExchangeError);
		CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("InvalidPartial"), Glib::FILE_TEST_EXISTS));
	}
	unsigned calls = 0;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Fixtures/custom.aaf"), "", fixture.path ("Cancelled"), "Cancelled", fixture.report, [&] { return ++calls > 8; }), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("Cancelled"), Glib::FILE_TEST_EXISTS));
	calls = 0;
	{ PX::LogicProject counted (fixture.path ("Fixtures/two-custom.aaf"), "", [&] { ++calls; return false; }); }
	CPPUNIT_ASSERT (calls > 1);
	unsigned cancel_at = calls / 2;
	calls = 0;
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Fixtures/two-custom.aaf"), "", fixture.path ("CancelledTwo"), "CancelledTwo", fixture.report, [&] { return ++calls >= cancel_at; }), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("CancelledTwo"), Glib::FILE_TEST_EXISTS));
	auto verify = [&] (const std::string& mode) {
		std::vector<float> sum (rate * 9 / 2, 0), mix (512), gain (512);
		auto track = std::dynamic_pointer_cast<Track> (fixture.session->route_by_name ("Dissolves")); CPPUNIT_ASSERT (track);
		auto regions = track->playlist ()->region_list (); CPPUNIT_ASSERT_EQUAL (size_t (2), regions->size ());
		for (auto const& region : *regions) {
			auto audio = std::dynamic_pointer_cast<AudioRegion> (region); CPPUNIT_ASSERT (audio); CPPUNIT_ASSERT (!audio->opaque ());
			CPPUNIT_ASSERT_EQUAL (rate * 2, audio->length_samples ());
			CPPUNIT_ASSERT (audio->position_sample () == rate / 2 || audio->position_sample () == rate * 2);
			CPPUNIT_ASSERT_EQUAL (audio->position_sample () == rate / 2 ? rate / 8 : 3 * rate / 8, audio->start_sample ());
			std::vector<float> pcm (audio->length_samples (), 0);
			unsigned block = 0;
			for (samplecnt_t at = 0; at < audio->length_samples ();) {
				auto count = std::min (std::array<samplecnt_t, 4> {{1, 257, 32, 509}}[block++ % 4], audio->length_samples () - at);
				CPPUNIT_ASSERT_EQUAL (count, audio->read_at (pcm.data () + at, mix.data (), gain.data (), audio->position_sample () + at, count, 0));
				at += count;
			}
			for (size_t i = 0; i < pcm.size (); ++i) { sum[audio->position_sample () + i] += pcm[i]; }
		}
		auto level = [&] (double t, bool incoming) {
			if (mode.compare (0, 4, "two-") == 0) {
				if (mode == "two-constant") { return incoming ? .75 : .5; }
				if (mode == "two-default" || mode == (incoming ? "two-incoming-default" : "two-outgoing-default")) { return incoming ? t : 1 - t; }
				if (!incoming) {
					if (mode == "two-reverse-mixed") { return t < .5 ? .65 : t < 1 ? .3 : 0.0; }
					return t < .5 ? .65 - .7 * t : .6 - .6 * t;
				}
				if (mode == "two-mixed") { return t < .25 ? 0.0 : t < .75 ? .8 : t < 1 ? .9 : .75; }
				if (t < .25) { return t * 3.2; }
				return t < .75 ? .8 + (t - .25) * .2 : .9 - (t - .75) * .6;
			}
			if (!incoming) { t = 1 - t; }
			if (mode == "constant") { return .5; }
			if (mode == "default") { return t; }
			if (mode == "hold") { return t < .25 ? 0.0 : t < .75 ? .8 : t < 1 ? .9 : 1.0; }
			if (t < .25) { return t * 3.2; }
			if (t < .75) { return .8 + (t - .25) * .2; }
			return .9 + (t - .75) * (mode == "nonunity" ? -.6 : .4);
		};
		for (samplepos_t i = 0; i < samplepos_t (sum.size ()); ++i) {
			double expected = 0;
			if (i >= rate / 2 && i < rate * 5 / 2) {
				double in = i < rate * 3 / 4 ? level (double (i - rate / 2) / (rate / 4), true) : 1;
				double out = i >= rate * 2 ? level (double (i - rate * 2) / (rate / 2), false) : 1;
				expected += .125 * in * out;
			}
			if (i >= rate * 2 && i < rate * 4) {
				double in = i < rate * 5 / 2 ? level (double (i - rate * 2) / (rate / 2), true) : 1;
				double out = i >= rate * 7 / 2 ? level (double (i - rate * 7 / 2) / (rate / 2), false) : 1;
				expected += .25 * in * out;
			}
			CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE (mode + " frame " + std::to_string (i), expected, sum[i], 3e-6);
		}
	};
	auto reopen = [&] (const std::string& name) {
		CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path (name), name, nullptr, "", true));
		AudioEngine::instance ()->set_session (fixture.session.get ());
	};
	for (std::string mode : {"default", "custom", "hold", "nonunity", "constant", "two-default", "two-custom", "two-mixed", "two-reverse-mixed",
	                         "two-incoming-default", "two-outgoing-default", "two-constant"}) {
		auto source = fixture.path ("Fixtures/" + mode + ".aaf"); auto bytes = Glib::file_get_contents (source);
		fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), source, "", fixture.path (mode), mode, fixture.report);
		reopen (mode); verify (mode);
		PX::SessionExchange::export_dawproject (*fixture.session, fixture.path (mode + ".dawproject"), fixture.report);
		PX::SessionExchange::export_logic (*fixture.session, fixture.path (mode + "-logic"), fixture.report);
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session = PX::SessionExchange::import_dawproject (*AudioEngine::instance (), fixture.path (mode + ".dawproject"), fixture.path (mode + "-daw"), mode + "-daw", fixture.report);
		reopen (mode + "-daw"); verify (mode);
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path (mode + "-logic/Project.aaf"), "", fixture.path (mode + "-returned"), mode + "-returned", fixture.report);
		reopen (mode + "-returned"); verify (mode);
		AudioEngine::instance ()->remove_session (); fixture.session.reset ();
		CPPUNIT_ASSERT_EQUAL (bytes, Glib::file_get_contents (source));
	}
	for (auto const& name : Glib::Dir (fixture.root)) { CPPUNIT_ASSERT (name.find (".ardour-exchange-") != 0); }
	std::cout << "AAF default, asymmetric, hold and non-unity fades preserve saved PCM through DAWproject and AAF returns; malformed/cancellation guards passed" << std::endl;
	std::cout << "AAF independent outgoing/incoming curves preserve saved PCM with separate interpolation, optional defaults and constant levels; malformed/cancellation guards passed" << std::endl;
}

void LogicSessionTest::flat_mixer_automation ()
{
	Fixture fixture;
	auto control = fixture.session->route_by_name ("Stereo")->gain_control ();
	control->alist ()->set_time_domain (Temporal::AudioTime);
	CPPUNIT_ASSERT (control->alist ()->set_interpolation (Evoral::ControlList::Linear));
	control->alist ()->add (Temporal::timepos_t (0), .5, false, false);
	control->alist ()->add (Temporal::timepos_t (fixture.session->sample_rate () * 4), .5, false, false);
	control->set_automation_state (Play);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Flat"), fixture.report);
	PX::LogicProject output (fixture.path ("Flat/Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (output.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), output.project ().tracks[1].volume_automation.points.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, output.project ().tracks[1].volume_automation.points.back ().time, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, output.project ().tracks[1].volume_automation.points.back ().value, 1e-9);
	control.reset ();
}

void LogicSessionTest::range_marker_roundtrip ()
{
	Fixture fixture;
	auto rate = fixture.session->sample_rate ();
	fixture.session->locations ()->add (new Location (*fixture.session,
	        Temporal::timepos_t (samplepos_t (2 * rate)), Temporal::timepos_t (samplepos_t (3 * rate)),
	        "Bridge range", Location::IsRangeMarker));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("range.dawproject"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT (!Glib::file_test (fixture.path ("range.dawproject"), Glib::FILE_TEST_EXISTS));
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Ranges"), fixture.report);
	std::string aaf_path = fixture.path ("Ranges/Project.aaf"), midi_path = fixture.path ("Ranges/Project.mid");
	std::unique_ptr<AAF_Iface, void (*) (AAF_Iface*)> aaf (aafi_alloc (nullptr), [] (AAF_Iface* value) { aafi_release (&value); });
	CPPUNIT_ASSERT (aaf);
	CPPUNIT_ASSERT_EQUAL (0, aafi_load_file (aaf.get (), aaf_path.c_str ()));
	unsigned range_count = 0;
	aafiMarker* marker;
	AAFI_foreachMarker (aaf.get (), marker) {
		if (marker->length > 0) {
			CPPUNIT_ASSERT_EQUAL (std::string ("Bridge range"), std::string (marker->comment));
			CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, double (marker->length) / aafRationalToDouble (*marker->edit_rate), 1.0 / rate);
			++range_count;
		}
	}
	CPPUNIT_ASSERT_EQUAL (1u, range_count);
	PX::LogicProject parsed (aaf_path, midi_path);
	CPPUNIT_ASSERT (!PX::has_unsupported_features (parsed.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (2), parsed.project ().markers.size ());
	auto found = std::find_if (parsed.project ().markers.begin (), parsed.project ().markers.end (), [] (const PX::Marker& item) { return item.name == "Bridge range"; });
	CPPUNIT_ASSERT (found != parsed.project ().markers.end ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (2.0, found->position.value, 1.0 / rate);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.0, found->duration, 1.0 / rate);
	auto native_report = PX::StudioOneSong::export_report (parsed.project ());
	CPPUNIT_ASSERT (std::any_of (native_report.begin (), native_report.end (), [] (const PX::Diagnostic& item) {
		return item.disposition == PX::Disposition::Unsupported && item.object == "Bridge range";
	}));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), aaf_path, midi_path,
	                  fixture.path ("ImportedRanges"), "ImportedRanges", fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("ImportedRanges"), "ImportedRanges", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	unsigned restored = 0;
	for (auto const& location : fixture.session->locations ()->list ()) {
		if (location->name () == "Bridge range") {
			CPPUNIT_ASSERT (location->is_range_marker ());
			CPPUNIT_ASSERT_EQUAL (samplepos_t (2 * rate), location->start_sample ());
			CPPUNIT_ASSERT_EQUAL (samplepos_t (3 * rate), location->end_sample ());
			++restored;
		}
	}
	CPPUNIT_ASSERT_EQUAL (1u, restored);
}

void LogicSessionTest::saved_session_export ()
{
	Fixture fixture;
	const std::string output = fixture.path (u8"Logic café");
	PX::SessionExchange::export_logic (*fixture.session, output, fixture.report);
	CPPUNIT_ASSERT (Glib::file_test (Glib::build_filename (output, "Metadata.xml"), Glib::FILE_TEST_IS_REGULAR));
	for (auto const& name : Glib::Dir (fixture.root)) { CPPUNIT_ASSERT (name.find (".ardour-exchange-") != 0); }
	std::unique_ptr<AAF_Iface, void (*) (AAF_Iface*)> aaf (aafi_alloc (nullptr), [] (AAF_Iface* value) { aafi_release (&value); });
	CPPUNIT_ASSERT (aaf);
	CPPUNIT_ASSERT_EQUAL (0, aafi_load_file (aaf.get (), Glib::build_filename (output, "Project.aaf").c_str ()));
	aafiAudioTrack* track = aaf->Audio->Tracks;
	CPPUNIT_ASSERT (track && track->next && !track->next->next);
	for (unsigned channel = 1; track; track = track->next, ++channel) {
		CPPUNIT_ASSERT_EQUAL (std::string ("Stereo"), std::string (track->name));
		CPPUNIT_ASSERT_EQUAL (channel, track->number);
		CPPUNIT_ASSERT_EQUAL (uint16_t (1), track->format);
		CPPUNIT_ASSERT (track->gain);
		CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, aafRationalToDouble (track->gain->value[0]), 1e-8);
		aafiTimelineItem* item;
		unsigned count = 0;
		AAFI_foreachTrackItem (track, item) {
			aafiAudioClip* clip = aafi_timelineItemToAudioClip (item);
			if (!clip) { continue; }
			CPPUNIT_ASSERT_EQUAL (int64_t ((count ? 2.5 : 1.25) * fixture.session->sample_rate ()), clip->pos);
			CPPUNIT_ASSERT_EQUAL (int64_t (.5 * fixture.session->sample_rate ()), clip->len);
			CPPUNIT_ASSERT_EQUAL (int64_t (.375 * fixture.session->sample_rate ()), clip->essence_offset);
			CPPUNIT_ASSERT (clip->essencePointerList);
			CPPUNIT_ASSERT_EQUAL (uint32_t (channel), clip->essencePointerList->essenceChannel);
			if (count) { CPPUNIT_ASSERT (clip->gain); CPPUNIT_ASSERT_EQUAL (0.0, aafRationalToDouble (clip->gain->value[0])); }
			++count;
		}
		CPPUNIT_ASSERT_EQUAL (2u, count);
	}
	PX::LogicProject regrouped (Glib::build_filename (output, "Project.aaf"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (regrouped.project ()));
	CPPUNIT_ASSERT_EQUAL (size_t (1), regrouped.project ().markers.size ());
	CPPUNIT_ASSERT_EQUAL (std::string (u8"Refrain 音楽"), regrouped.project ().markers[0].name);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (7.5, regrouped.project ().markers[0].position.value, 1.0 / fixture.session->sample_rate ());
	CPPUNIT_ASSERT_EQUAL (size_t (2), regrouped.project ().tracks.size ());
	CPPUNIT_ASSERT_EQUAL (2u, regrouped.project ().tracks[1].channels);
	CPPUNIT_ASSERT_EQUAL (size_t (2), regrouped.project ().tracks[1].clips.size ());
	CPPUNIT_ASSERT (regrouped.project ().tracks[1].clips[0].color.empty ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), regrouped.project ().media.size ());
	const std::string aaf_path = Glib::build_filename (output, "Project.aaf");
	const std::string midi_path = Glib::build_filename (output, "Project.mid");
	PX::LogicProject returned (aaf_path, midi_path);
	CPPUNIT_ASSERT_EQUAL (std::string ("#13579b"), returned.project ().tracks[1].color);
	CPPUNIT_ASSERT_EQUAL (std::string ("#2468ac"), returned.project ().tracks[2].color);
	CPPUNIT_ASSERT_EQUAL (std::string (u8"Audio café"), returned.project ().tracks[1].clips[0].name);
	CPPUNIT_ASSERT_EQUAL (std::string ("#abcdef"), returned.project ().tracks[1].clips[0].color);
	CPPUNIT_ASSERT_EQUAL (std::string ("#abcdef"), returned.project ().tracks[1].clips[1].color);
	const auto& returned_music = returned.project ().tracks[2];
	CPPUNIT_ASSERT_EQUAL (size_t (1), returned_music.clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Phrase One"), returned_music.clips[0].name);
	CPPUNIT_ASSERT_EQUAL (std::string ("#fedcba"), returned_music.clips[0].color);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, returned_music.clips[0].position.value, 1.0 / 19200);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, returned_music.clips[0].duration, 1.0 / 19200);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1, returned_music.clips[0].offset.value, 1.0 / 19200);
	CPPUNIT_ASSERT_EQUAL (size_t (1), returned_music.clips[0].notes.size ());
	const std::string metadata_path = Glib::build_filename (output, "Metadata.xml");
	char* data = nullptr;
	gsize metadata_size = 0;
	CPPUNIT_ASSERT (g_file_get_contents (metadata_path.c_str (), &data, &metadata_size, nullptr));
	std::unique_ptr<char, void (*) (void*)> contents (data, g_free);
	std::string metadata (data, metadata_size);
	std::string extra_clip = metadata;
	const size_t midi_end = extra_clip.find ("</MidiTrack>");
	CPPUNIT_ASSERT (midi_end != std::string::npos);
	extra_clip.insert (midi_end, "<Clip name=\"Empty return\" color=\"#aabbcc\" enabled=\"true\" gain=\"1\" time=\"8\" length=\"1\" offset=\"0\" />");
	CPPUNIT_ASSERT (g_file_set_contents (metadata_path.c_str (), extra_clip.data (), extra_clip.size (), nullptr));
	PX::LogicProject two_clips (aaf_path, midi_path);
	CPPUNIT_ASSERT_EQUAL (size_t (2), two_clips.project ().tracks[2].clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Empty return"), two_clips.project ().tracks[2].clips[1].name);
	CPPUNIT_ASSERT (two_clips.project ().tracks[2].clips[1].notes.empty ());
	std::string unmatched_note = metadata;
	const size_t key = unmatched_note.find ("key=\"60\"");
	CPPUNIT_ASSERT (key != std::string::npos);
	unmatched_note.replace (key, 8, "key=\"61\"");
	CPPUNIT_ASSERT (g_file_set_contents (metadata_path.c_str (), unmatched_note.data (), unmatched_note.size (), nullptr));
	PX::LogicProject unmatched (aaf_path, midi_path);
	CPPUNIT_ASSERT_EQUAL (size_t (1), unmatched.project ().tracks[2].clips.size ());
	CPPUNIT_ASSERT_EQUAL (0.0, unmatched.project ().tracks[2].clips[0].position.value);
	CPPUNIT_ASSERT_EQUAL (60, unmatched.project ().tracks[2].clips[0].notes[0].key);
	std::string invalid_hidden = metadata;
	const size_t midi_start = invalid_hidden.find ("<MidiTrack");
	const size_t clip_end = invalid_hidden.find ("</Clip>", midi_start);
	CPPUNIT_ASSERT (midi_start != std::string::npos && clip_end != std::string::npos);
	invalid_hidden.insert (clip_end, "<Note time=\"1e30\" length=\"1\" key=\"64\" channel=\"2\" velocity=\"83\" release=\"30\" />");
	CPPUNIT_ASSERT (g_file_set_contents (metadata_path.c_str (), invalid_hidden.data (), invalid_hidden.size (), nullptr));
	PX::LogicProject invalid_note (aaf_path, midi_path);
	CPPUNIT_ASSERT_EQUAL (0.0, invalid_note.project ().tracks[2].clips[0].position.value);
	CPPUNIT_ASSERT_EQUAL (60, invalid_note.project ().tracks[2].clips[0].notes[0].key);
	const size_t color = metadata.find ("color=\"#13579b\"");
	CPPUNIT_ASSERT (color != std::string::npos);
	std::string malformed = metadata;
	malformed.replace (color, 15, "color=\"invalid\"");
	CPPUNIT_ASSERT (g_file_set_contents (metadata_path.c_str (), malformed.data (), malformed.size (), nullptr));
	PX::LogicProject bad_color (aaf_path, midi_path);
	CPPUNIT_ASSERT (bad_color.project ().tracks[1].color.empty ());
	const size_t digest = metadata.find ("sha256=\"");
	CPPUNIT_ASSERT (digest != std::string::npos);
	metadata.replace (digest + 8, 64, 64, '0');
	CPPUNIT_ASSERT (g_file_set_contents (metadata_path.c_str (), metadata.data (), metadata.size (), nullptr));
	PX::LogicProject stale (aaf_path, midi_path);
	CPPUNIT_ASSERT (stale.project ().tracks[1].color.empty ());
	CPPUNIT_ASSERT (stale.project ().tracks[1].clips[0].color.empty ());
	unsigned count = 0;
	CPPUNIT_ASSERT_EQUAL (0, g_mkdir (fixture.path ("Extracted").c_str (), 0700));
	aafiAudioEssenceFile* media;
	count = 0;
	AAFI_foreachAudioEssenceFile (aaf.get (), media) {
		CPPUNIT_ASSERT (media->is_embedded);
		CPPUNIT_ASSERT_EQUAL (0, aafi_extractAudioEssenceFile (aaf.get (), media, AAFI_EXTRACT_WAV, fixture.path ("Extracted").c_str (), 0, 0, "audio", nullptr));
		SF_INFO info = {};
		SNDFILE* file = sf_open (media->usable_file_path, SFM_READ, &info);
		CPPUNIT_ASSERT (file);
		CPPUNIT_ASSERT_EQUAL (int (SF_FORMAT_PCM_24), info.format & SF_FORMAT_SUBMASK);
		std::vector<float> pcm (info.frames * info.channels);
		CPPUNIT_ASSERT_EQUAL (info.frames, sf_readf_float (file, pcm.data (), info.frames));
		CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
		for (size_t i = 0; i < pcm.size (); ++i) { CPPUNIT_ASSERT_EQUAL (i % 2 ? -.125f : .125f, pcm[i]); }
		++count;
	}
	CPPUNIT_ASSERT_EQUAL (1u, count);
	Evoral::SMF midi;
	CPPUNIT_ASSERT_EQUAL (0, midi.open (Glib::build_filename (output, "Project.mid"), 2));
	CPPUNIT_ASSERT_EQUAL (uint16_t (2), midi.num_tracks ());
	CPPUNIT_ASSERT_EQUAL (uint16_t (19200), midi.ppqn ());
	std::unique_ptr<Evoral::SMF::Tempo> tempo (midi.nth_tempo (0));
	CPPUNIT_ASSERT (tempo);
	CPPUNIT_ASSERT_EQUAL (600000, tempo->microseconds_per_quarter_note);
	CPPUNIT_ASSERT_EQUAL (3, tempo->numerator);
	CPPUNIT_ASSERT_EQUAL (4, tempo->denominator);
	uint32_t delta = 0, size = 0, time = 0;
	uint8_t* buffer = nullptr;
	Evoral::event_id_t id;
	count = 0;
	while (midi.read_event (&delta, &size, &buffer, &id) >= 0) {
		time += delta;
		if (size == 3 && (buffer[0] == 0x92 || buffer[0] == 0x82)) {
			CPPUNIT_ASSERT_EQUAL (uint32_t ((count ? 5.75 : 4.25) * 19200), time);
			CPPUNIT_ASSERT_EQUAL (uint8_t (60), buffer[1]);
			CPPUNIT_ASSERT_EQUAL (uint8_t (count ? 45 : 93), buffer[2]);
			++count;
		}
	}
	free (buffer);
	CPPUNIT_ASSERT_EQUAL (2u, count);
}

void LogicSessionTest::controller_clip_restoration ()
{
	Fixture fixture (.125f, true);
	const std::string output = fixture.path ("ControllerReturn");
	PX::SessionExchange::export_logic (*fixture.session, output, fixture.report);
	const std::string aaf = Glib::build_filename (output, "Project.aaf");
	const std::string midi = Glib::build_filename (output, "Project.mid");
	const std::string sidecar = Glib::build_filename (output, "Metadata.xml");
	PX::LogicProject restored (aaf, midi);
	CPPUNIT_ASSERT_EQUAL (size_t (3), restored.project ().tracks.size ());
	const auto& music = restored.project ().tracks[2];
	CPPUNIT_ASSERT_EQUAL (size_t (1), music.clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Phrase One"), music.clips[0].name);
	CPPUNIT_ASSERT_EQUAL (std::string ("#fedcba"), music.clips[0].color);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, music.clips[0].position.value, 1.0 / 19200);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1, music.clips[0].offset.value, 1.0 / 19200);
	CPPUNIT_ASSERT_EQUAL (size_t (1), music.clips[0].midi_controls.size ());
	const auto& control = music.clips[0].midi_controls[0];
	CPPUNIT_ASSERT_EQUAL (0xb0u, control.type);
	CPPUNIT_ASSERT_EQUAL (2u, control.channel);
	CPPUNIT_ASSERT_EQUAL (64u, control.number);
	CPPUNIT_ASSERT_EQUAL (size_t (4), control.points.size ());
	const std::array<double, 4> expected_times {{.5, 1, 1.5, 4.5}};
	const std::array<double, 4> expected_values {{127, 127, 0, 127}};
	for (size_t i = 0; i < 4; ++i) {
		CPPUNIT_ASSERT_DOUBLES_EQUAL (expected_times[i], control.points[i].time, 1.0 / 19200);
		CPPUNIT_ASSERT_EQUAL (expected_values[i], control.points[i].value);
	}
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), aaf, midi,
	                                                      fixture.path ("ControllerImported"), "ControllerImported", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, fixture.session->save_state (""));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("ControllerImported"), "ControllerImported", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("controller-return.dawproject"));
	auto returned = PX::DawProject::read (fixture.path ("controller-return.dawproject"));
	auto found = std::find_if (returned.tracks.begin (), returned.tracks.end (), [] (const PX::Track& track) { return track.name == "Notes"; });
	CPPUNIT_ASSERT (found != returned.tracks.end ());
	const auto& returned_music = *found;
	CPPUNIT_ASSERT_EQUAL (size_t (1), returned_music.clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Phrase One"), returned_music.clips[0].name);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, returned_music.clips[0].position.value, 1.0 / 19200);
	CPPUNIT_ASSERT_EQUAL (size_t (1), returned_music.clips[0].midi_controls.size ());
	CPPUNIT_ASSERT_EQUAL (size_t (4), returned_music.clips[0].midi_controls[0].points.size ());
	char* data = nullptr;
	gsize size = 0;
	CPPUNIT_ASSERT (g_file_get_contents (sidecar.c_str (), &data, &size, nullptr));
	std::unique_ptr<char, void (*) (void*)> contents (data, g_free);
	std::string changed (data, size);
	const size_t lane = changed.find ("<MidiControl");
	const size_t end = changed.find ("</MidiControl>", lane);
	const size_t point = changed.find ("value=\"0\"", lane);
	CPPUNIT_ASSERT (lane != std::string::npos && end != std::string::npos && point > lane && point < end);
	changed.replace (point, 9, "value=\"1\"");
	CPPUNIT_ASSERT (g_file_set_contents (sidecar.c_str (), changed.data (), changed.size (), nullptr));
	PX::LogicProject mismatched (aaf, midi);
	CPPUNIT_ASSERT_EQUAL (size_t (1), mismatched.project ().tracks[2].clips.size ());
	CPPUNIT_ASSERT_EQUAL (0.0, mismatched.project ().tracks[2].clips[0].position.value);
	CPPUNIT_ASSERT_EQUAL (size_t (1), mismatched.project ().tracks[2].clips[0].midi_controls.size ());
	CPPUNIT_ASSERT_EQUAL (0.0, mismatched.project ().tracks[2].clips[0].midi_controls[0].points[1].value);
}

void LogicSessionTest::existing_destination ()
{
	Fixture fixture;
	CPPUNIT_ASSERT_EQUAL (0, g_mkdir (fixture.path ("Output").c_str (), 0700));
	std::ofstream (fixture.path ("Output/keep")) << "existing";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT_EQUAL (std::string ("existing"), Glib::file_get_contents (fixture.path ("Output/keep")));
}

void LogicSessionTest::missing_runtime ()
{
	Fixture fixture;
	EnvironmentOverride python ("ARDOUR_LOGIC_PYTHON", fixture.path ("missing-python"));
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report), PX::ExchangeError);
	fixture.clean_failure ();
}

void LogicSessionTest::writer_failure ()
{
	Fixture fixture;
	std::ofstream (fixture.path ("fail.py")) << "import pathlib,sys\np=pathlib.Path(sys.argv[2])\n(p/'Project.aaf').write_bytes(b'partial')\nraise RuntimeError('Intentional writer failure')\n";
	EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", fixture.path ("fail.py"));
	try {
		PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report);
		CPPUNIT_FAIL ("Writer failure must reject export");
	} catch (const PX::ExchangeError& error) { CPPUNIT_ASSERT (std::string (error.what ()).find ("Intentional writer failure") != std::string::npos); }
	fixture.clean_failure ();
}

void LogicSessionTest::cancel_running_writer ()
{
	Fixture fixture;
	std::ofstream (fixture.path ("wait.py")) << "import pathlib,sys,time\np=pathlib.Path(sys.argv[2])\n(p/'ready').write_text('ready')\nwhile True: time.sleep(.1)\n";
	EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", fixture.path ("wait.py"));
	bool child_started = false;
	unsigned calls = 0;
	auto cancel = [&] {
		for (auto const& name : Glib::Dir (fixture.root)) {
			if (name.find (".ardour-exchange-") == 0 && Glib::file_test (fixture.path (name + "/session/ready"), Glib::FILE_TEST_EXISTS)) { child_started = true; }
		}
		return child_started || ++calls > 1000;
	};
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (child_started);
	fixture.clean_failure ();
}

void LogicSessionTest::writer_missing_outputs ()
{
	Fixture fixture;
	std::ofstream (fixture.path ("empty.py")) << "pass\n";
	EnvironmentOverride helper ("ARDOUR_LOGIC_HELPER", fixture.path ("empty.py"));
	try {
		PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report);
		CPPUNIT_FAIL ("Missing output must reject export");
	} catch (const PX::ExchangeError& error) { CPPUNIT_ASSERT (std::string (error.what ()).find ("did not produce Project.aaf") != std::string::npos); }
	fixture.clean_failure ();
}

void LogicSessionTest::out_of_range_audio ()
{
	Fixture fixture (1.25f);
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Output"), fixture.report), PX::ExchangeError);
	fixture.clean_failure ();
}

void LogicSessionTest::coordinated_import ()
{
	Fixture fixture;
	for (auto const& route : *fixture.session->get_routes ()) {
		if (auto track = std::dynamic_pointer_cast<Track> (route)) {
			auto regions = track->playlist ()->region_list ();
			for (auto const& region : *regions) {
				if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) { if (!audio->muted ()) { audio->set_scale_amplitude (.375); } }
			}
		}
	}
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Transfer"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session = PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Transfer/Project.aaf"),
	                    fixture.path ("Transfer/Project.mid"), fixture.path ("Imported"), "Imported", fixture.report);
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("Transfer/Project.aaf").c_str ()));
	CPPUNIT_ASSERT_EQUAL (0, g_unlink (fixture.path ("Transfer/Project.mid").c_str ()));
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	fixture.session.reset (new Session (*AudioEngine::instance (), fixture.path ("Imported"), "Imported", nullptr, "", true));
	AudioEngine::instance ()->set_session (fixture.session.get ());
	unsigned audio_count = 0, midi_count = 0;
	for (auto const& route : *fixture.session->get_routes ()) {
		auto track = std::dynamic_pointer_cast<Track> (route);
		if (!track) { continue; }
		auto regions = track->playlist ()->region_list ();
		for (auto const& region : *regions) {
			if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) {
				CPPUNIT_ASSERT_EQUAL (std::string ("Stereo"), route->name ());
				CPPUNIT_ASSERT_DOUBLES_EQUAL (.75, route->gain_control ()->get_value (), 1e-7);
				CPPUNIT_ASSERT_EQUAL (samplepos_t (.375 * fixture.session->sample_rate ()), audio->start_sample ());
				CPPUNIT_ASSERT_EQUAL (samplecnt_t (.5 * fixture.session->sample_rate ()), audio->length_samples ());
				bool first = audio->position_sample () == samplepos_t (1.25 * fixture.session->sample_rate ());
				CPPUNIT_ASSERT_DOUBLES_EQUAL (first ? .375 : 0, audio->scale_amplitude (), 1e-7);
				if (!first) { CPPUNIT_ASSERT_EQUAL (samplepos_t (2.5 * fixture.session->sample_rate ()), audio->position_sample ()); }
				CPPUNIT_ASSERT_EQUAL (uint32_t (2), audio->n_channels ());
				for (unsigned channel = 0; channel < 2; ++channel) {
					auto source = std::dynamic_pointer_cast<AudioSource> (audio->source (channel));
					std::array<float, 31> samples {};
					CPPUNIT_ASSERT_EQUAL (samplecnt_t (samples.size ()), source->read (samples.data (), 0, samples.size ()));
					for (float sample : samples) { CPPUNIT_ASSERT_EQUAL (channel ? -.125f : .125f, sample); }
				}
				++audio_count;
			} else if (auto midi = std::dynamic_pointer_cast<MidiRegion> (region)) {
				CPPUNIT_ASSERT_EQUAL (std::string ("Notes"), route->name ());
				CPPUNIT_ASSERT_EQUAL (std::string ("Phrase One"), midi->name ());
				CPPUNIT_ASSERT (midi->position ().beats () == Temporal::Beats (4, 0));
				CPPUNIT_ASSERT_EQUAL (size_t (1), midi->model ()->n_notes ());
				auto note = *midi->model ()->notes ().begin ();
				CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, double (note->time ().to_ticks ()) / Temporal::Beats::PPQN, 1e-7);
				CPPUNIT_ASSERT_DOUBLES_EQUAL (1.5, double (note->length ().to_ticks ()) / Temporal::Beats::PPQN, 1e-7);
				CPPUNIT_ASSERT_EQUAL (uint8_t (60), note->note ());
				CPPUNIT_ASSERT_EQUAL (uint8_t (2), note->channel ());
				++midi_count;
			}
		}
	}
	CPPUNIT_ASSERT_EQUAL (2u, audio_count); CPPUNIT_ASSERT_EQUAL (1u, midi_count);
	PX::SessionExchange::export_dawproject (*fixture.session, fixture.path ("ImportedStructure.dawproject"));
	auto structure = PX::DawProject::read (fixture.path ("ImportedStructure.dawproject"));
	auto music = std::find_if (structure.tracks.begin (), structure.tracks.end (), [] (const PX::Track& track) { return track.name == "Notes"; });
	CPPUNIT_ASSERT (music != structure.tracks.end ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), music->clips.size ());
	CPPUNIT_ASSERT_EQUAL (std::string ("Phrase One"), music->clips[0].name);
	CPPUNIT_ASSERT_EQUAL (std::string ("#fedcba"), music->clips[0].color);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (4, music->clips[0].position.value, 1.0 / Temporal::Beats::PPQN);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1, music->clips[0].offset.value, 1.0 / Temporal::Beats::PPQN);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (1.25, music->clips[0].notes[0].time, 1.0 / Temporal::Beats::PPQN);
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Reexport"), fixture.report);
	PX::LogicProject reloaded (fixture.path ("Reexport/Project.aaf"), fixture.path ("Reexport/Project.mid"));
	CPPUNIT_ASSERT (!PX::has_unsupported_features (reloaded.project ()));
	CPPUNIT_ASSERT_DOUBLES_EQUAL (100, reloaded.project ().tempo, 1e-7);
	CPPUNIT_ASSERT_EQUAL (3u, reloaded.project ().numerator);
	CPPUNIT_ASSERT_EQUAL (size_t (1), reloaded.project ().markers.size ());
	CPPUNIT_ASSERT_EQUAL (std::string (u8"Refrain 音楽"), reloaded.project ().markers[0].name);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (12.5, reloaded.project ().markers[0].position.value, 1e-7);
}

void LogicSessionTest::import_cancel_and_failure ()
{
	Fixture fixture;
	PX::SessionExchange::export_logic (*fixture.session, fixture.path ("Transfer"), fixture.report);
	AudioEngine::instance ()->remove_session (); fixture.session.reset ();
	bool copying = false;
	auto cancel = [&] {
		for (auto const& name : Glib::Dir (fixture.root)) {
			if (name.find (".ardour-exchange-") == 0 && Glib::file_test (fixture.path (name + "/input-0.media"), Glib::FILE_TEST_EXISTS)) { copying = true; }
		}
		return copying;
	};
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Transfer/Project.aaf"),
	                      fixture.path ("Transfer/Project.mid"), fixture.path ("Output"), "Imported", fixture.report, cancel), PX::ArchiveCancelled);
	CPPUNIT_ASSERT (copying); fixture.clean_failure ();
	std::ofstream (fixture.path ("broken.aaf"), std::ios::binary) << std::string (512, 'x');
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("broken.aaf"), "",
	                      fixture.path ("Output"), "Imported", fixture.report), PX::ExchangeError);
	fixture.clean_failure ();
	std::ofstream (fixture.path ("broken.mid"), std::ios::binary) << "not MIDI";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Transfer/Project.aaf"), fixture.path ("broken.mid"),
	                      fixture.path ("Output"), "Imported", fixture.report), PX::ExchangeError);
	fixture.clean_failure ();
	CPPUNIT_ASSERT_EQUAL (0, g_mkdir (fixture.path ("Output").c_str (), 0700));
	std::ofstream (fixture.path ("Output/keep")) << "existing";
	CPPUNIT_ASSERT_THROW (PX::SessionExchange::import_logic (*AudioEngine::instance (), fixture.path ("Transfer/Project.aaf"), fixture.path ("Transfer/Project.mid"),
	                      fixture.path ("Output"), "Imported", fixture.report), PX::ExchangeError);
	CPPUNIT_ASSERT_EQUAL (std::string ("existing"), Glib::file_get_contents (fixture.path ("Output/keep")));
}

void LogicSessionTest::midi_validation ()
{
	const std::string root = new_test_output_dir ("project-midi"), path = Glib::build_filename (root, "input.mid");
	const std::vector<unsigned char> valid = {
		0, 0xff, 0x51, 3, 9, 0x27, 0xc0, // 100 BPM
		0, 0xff, 0x58, 4, 3, 2, 24, 8, // 3/4
		0, 0xff, 0x54, 5, 0x20, 0, 0, 0, 0, // 25fps, zero origin
		0, 0xff, 3, 4, 'K', 'e', 'y', 's',
		0, 0x92, 60, 93, 0, 62, 77, // running status
		0x81, 0x70, 0x82, 60, 45, 0, 62, 64,
		0, 0xff, 0x2f, 0
	};
	auto write = [&] (const std::vector<unsigned char>& payload) {
		std::vector<unsigned char> bytes = {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xe0,'M','T','r','k'};
		for (int shift = 24; shift >= 0; shift -= 8) { bytes.push_back ((payload.size () >> shift) & 255); }
		bytes.insert (bytes.end (), payload.begin (), payload.end ());
		Glib::file_set_contents (path, std::string (bytes.begin (), bytes.end ()));
	};
	write (valid);
	PX::Project project; PX::read_project_midi (path, project, 0);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (100, project.tempo, 1e-7);
	CPPUNIT_ASSERT_EQUAL (3u, project.numerator);
	CPPUNIT_ASSERT_EQUAL (std::string ("Keys"), project.tracks[0].name);
	CPPUNIT_ASSERT_EQUAL (size_t (2), project.tracks[0].clips[0].notes.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, project.tracks[0].clips[0].notes[0].duration, 1e-7);
	CPPUNIT_ASSERT_THROW (PX::read_project_midi (path, project, 3600), PX::ExchangeError);
	CPPUNIT_ASSERT_THROW (PX::read_project_midi (path, project, 0, [] { return true; }), PX::ArchiveCancelled);
	auto broken = valid; broken.pop_back (); write (broken);
	CPPUNIT_ASSERT_THROW (PX::read_project_midi (path, project, 0), PX::ExchangeError);
	broken = valid; broken.insert (broken.end () - 4, {0, 0xb2, 64, 127}); write (broken);
	PX::Project controller; PX::read_project_midi (path, controller, 0);
	CPPUNIT_ASSERT_EQUAL (size_t (1), controller.tracks[0].clips[0].midi_controls.size ());
	CPPUNIT_ASSERT_EQUAL (127.0, controller.tracks[0].clips[0].midi_controls[0].points[0].value);
	CPPUNIT_ASSERT (controller.tracks[0].clips[0].duration > .5); // Keep a controller at the end-of-track tick audible.
	for (auto extra : std::vector<std::vector<unsigned char>> {{0, 0xb2, 101, 0}, {0, 0xc2, 5}, {0, 0xb2, 64, 127, 0, 64, 0}, {0, 0xe2, 128, 64}}) {
		broken = valid; broken.insert (broken.end () - 4, extra.begin (), extra.end ()); write (broken);
		CPPUNIT_ASSERT_THROW (PX::read_project_midi (path, project, 0), PX::ExchangeError);
	}
	broken = valid; broken.insert (broken.end () - 4, {0, 0xff, 0x51, 3, 7, 0xa1, 0x20}); write (broken);
	PX::read_project_midi (path, project, 0);
	CPPUNIT_ASSERT_EQUAL (size_t (2), project.tempos.size ());
	CPPUNIT_ASSERT_DOUBLES_EQUAL (.5, project.tempos[1].position.value, 1e-9);
	CPPUNIT_ASSERT_DOUBLES_EQUAL (120, project.tempos[1].bpm, 1e-9);
	broken = valid; broken.insert (broken.begin () + 7, {0, 0xff, 0x51, 3, 7, 0xa1, 0x20}); write (broken);
	CPPUNIT_ASSERT_THROW (PX::read_project_midi (path, project, 0), PX::ExchangeError);
}
