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
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/location.h"
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

void native_package (Fixture& fixture, bool processing = false)
{
	auto project = example (fixture);
	auto uri = [] (const std::string& path) {
		gchar* raw = g_filename_to_uri (path.c_str (), NULL, NULL);
		CPPUNIT_ASSERT (raw);
		std::string result (raw); g_free (raw); return result;
	};
	PX::ProjectArchiveWriter song (fixture.path ("input.song"));
	song.add_text ("metainfo.xml", "<MetaInformation><Attribute id=\"Document:MimeType\" value=\"application/x.presonus-song\"/><Attribute id=\"Document:FormatVersion\" value=\"9\"/></MetaInformation>");
	song.add_text ("Song/song.xml", std::string (R"(<Song><Attributes x:id="Root"><Attributes x:id="timeContext">
<TempoMap x:id="tempoMap"><TempoMapSegment start="0" tempo="0.6"/></TempoMap>
<TimeSignatureMap x:id="timeSignatureMap"><TimeSignatureMapSegment start="0" numerator="3" denominator="4"/></TimeSignatureMap>
</Attributes><List x:id="Tracks"><MarkerTrack timeFormat="2"><MarkerEvent markerType="2"/><MarkerEvent markerType="3" start="12"/></MarkerTrack>
<MediaTrack trackID="audio" mediaType="Audio" name="Native audio" timeFormat="0" color="FF34AB12"><UID x:id="channelID" uid="channel"/>
<List x:id="Events"><AudioEvent clipID="media" start="1.25" length="0.5" offset="0.375" name="Native clip" gain=")") + (processing ? "0.5" : "1") + R"("/></List></MediaTrack></List></Attributes></Song>)");
	song.add_text ("Song/mediapool.xml", "<MediaPool><Attributes x:id=\"documentPath\" url=\"" + uri (fixture.path ("input.song")) + "\"/><AudioClip mediaID=\"media\"><Url x:id=\"path\" url=\"" + uri (fixture.path ("source.wav")) + "\"/><Attributes x:id=\"format\" sampleRate=\"" + std::to_string (project.media[0].sample_rate) + "\" numChannels=\"2\" frameCount=\"" + std::to_string (project.media[0].sample_rate * 2) + "\"/></AudioClip></MediaPool>");
	song.add_text ("Devices/audiomixer.xml", R"(<AudioMixer><AudioTrackChannel gain="0.75" pan="0.3"><UID x:id="uniqueID" uid="channel"/><Connection x:id="destination" objectID="master/Input"/></AudioTrackChannel><AudioOutputChannel label="Main"><UID x:id="uniqueID" uid="master"/></AudioOutputChannel></AudioMixer>)");
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
	CPPUNIT_TEST (native_song_cancel);
	CPPUNIT_TEST (native_song_unsupported);
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
	void native_song_cancel ();
	void native_song_unsupported ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (ProjectSessionTest);

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
			if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) { audio->set_scale_amplitude (0.5); changed = true; }
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
