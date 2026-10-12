/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <array>
#include <fstream>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <cppunit/extensions/HelperMacros.h>
#include <sndfile.h>
#include "aaf/libaaf.h"
#include "evoral/SMF.h"
#include "ardour/audioengine.h"
#include "ardour/project_session.h"
#include "ardour/session.h"
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
	Fixture (float level = .125f) : root (new_test_output_dir ("logic-exchange"))
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
		CPPUNIT_ASSERT_EQUAL (sf_count_t (rate * 2), sf_writef_float (file, pcm.data (), rate * 2));
		CPPUNIT_ASSERT_EQUAL (0, sf_close (file));
		project.media.push_back ({"source", "audio/source.wav", rate, 2, 2});
		PX::Track master; master.id = "master"; master.name = "Master"; master.kind = PX::TrackKind::Bus; master.master = true;
		project.tracks.push_back (master);
		PX::Track audio; audio.id = "audio"; audio.name = "Stereo"; audio.destination = "master"; audio.volume = .75;
		PX::Clip clip; clip.name = u8"Audio café"; clip.color = "#abcdef"; clip.media = "source";
		clip.position = PX::Time (1.25, PX::TimeUnit::Seconds); clip.duration = .5; clip.offset = PX::Time (.375, PX::TimeUnit::Seconds);
		audio.clips.push_back (clip);
		clip.position.value = 2.5; clip.enabled = false; audio.clips.push_back (clip);
		project.tracks.push_back (audio);
		PX::Track midi; midi.id = "midi"; midi.name = "Notes"; midi.kind = PX::TrackKind::MIDI; midi.destination = "master";
		clip = PX::Clip (); clip.kind = PX::ClipKind::MIDI; clip.name = "Notes";
		clip.position = PX::Time (4); clip.duration = 4; clip.offset = PX::Time (1);
		PX::Note note; note.time = 1.25; note.duration = 1.5; note.key = 60; note.channel = 2; note.velocity = 93.0 / 127; note.release_velocity = 45.0 / 127;
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
	CPPUNIT_TEST (existing_destination);
	CPPUNIT_TEST (missing_runtime);
	CPPUNIT_TEST (writer_failure);
	CPPUNIT_TEST (writer_missing_outputs);
	CPPUNIT_TEST (cancel_running_writer);
	CPPUNIT_TEST (out_of_range_audio);
	CPPUNIT_TEST_SUITE_END ();
public:
	void saved_session_export ();
	void existing_destination ();
	void missing_runtime ();
	void writer_failure ();
	void writer_missing_outputs ();
	void cancel_running_writer ();
	void out_of_range_audio ();
};
CPPUNIT_TEST_SUITE_REGISTRATION (LogicSessionTest);

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
	CPPUNIT_ASSERT (track && !track->next);
	CPPUNIT_ASSERT_EQUAL (std::string ("Stereo"), std::string (track->name));
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
		if (count) { CPPUNIT_ASSERT (clip->gain); CPPUNIT_ASSERT_EQUAL (0.0, aafRationalToDouble (clip->gain->value[0])); }
		++count;
	}
	CPPUNIT_ASSERT_EQUAL (2u, count);
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
		for (float sample : pcm) { CPPUNIT_ASSERT_EQUAL (.125f, sample); }
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
