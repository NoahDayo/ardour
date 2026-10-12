
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include <stdexcept>
#include <chrono>
#include <future>

#include "pbd/textreceiver.h"
#include "pbd/file_utils.h"
#include "ardour/session.h"
#include "ardour/audioengine.h"
#include "ardour/smf_source.h"
#include "ardour/midi_model.h"
#include "ardour/midi_patch_manager.h"
#include "ardour/search_paths.h"

#include "test_util.h"

#include "session_test.h"

CPPUNIT_TEST_SUITE_REGISTRATION (SessionTest);

using namespace std;
using namespace ARDOUR;
using namespace PBD;

void
SessionTest::midnam_shutdown ()
{
	using MIDI::Name::MidiPatchManager;
	/* Stop the loader started by ARDOUR::init(), then cover both destruction
	 * without a worker and repeated cleanup without creating a new singleton. */
	MidiPatchManager::destroy ();
	MidiPatchManager::instance ();
	MidiPatchManager::destroy ();
	MidiPatchManager::destroy ();

	auto& manager = MidiPatchManager::instance ();
	manager.remove_search_path (midi_patch_search_path ());
	if (g_getenv ("ARDOUR_NO_PATCHFILES")) {
		manager.load_midnams_in_thread ();
		MidiPatchManager::destroy ();
		return;
	}

	std::promise<void> entered, release;
	auto entered_future = entered.get_future ();
	auto release_future = release.get_future ();
	PBD::ScopedConnection connection;
	manager.PatchesChanged.connect_same_thread (connection, [&] {
		entered.set_value ();
		release_future.wait ();
	});
	manager.load_midnams_in_thread ();
	bool running = entered_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
	if (!running) {
		release.set_value ();
		MidiPatchManager::destroy ();
		CPPUNIT_FAIL ("MIDI patch worker did not reach the shutdown test barrier");
	}
	std::promise<void> stopping;
	auto stopping_future = stopping.get_future ();
	auto finished = std::async (std::launch::async, [&] {
		stopping.set_value ();
		MidiPatchManager::destroy ();
	});
	stopping_future.wait ();
	bool waited = finished.wait_for (std::chrono::milliseconds (50)) == std::future_status::timeout;
	/* Release before asserting, so a failing assertion cannot strand the worker. */
	release.set_value ();
	finished.get ();
	CPPUNIT_ASSERT_MESSAGE ("Shutdown returned while the patch worker was still active", waited);
}

void
SessionTest::new_session ()
{
	const string session_name("test_session");
	std::string new_session_dir = Glib::build_filename (new_test_output_dir(), session_name);

	CPPUNIT_ASSERT (!Glib::file_test (new_session_dir, Glib::FILE_TEST_EXISTS));

	create_and_start_dummy_backend ();

	ARDOUR::Session* new_session = load_session (new_session_dir, "test_session");

	CPPUNIT_ASSERT (new_session);

	delete new_session;
	stop_and_destroy_backend ();
}

void
SessionTest::new_session_from_template ()
{
	const string session_name("two_tracks");
	const string session_template_dir_name("2 Track-template");

	std::string new_session_dir = Glib::build_filename (new_test_output_dir(), session_name);

	CPPUNIT_ASSERT (!Glib::file_test (new_session_dir, Glib::FILE_TEST_EXISTS));

	std::string session_template_dir = test_search_path ().front ();
	session_template_dir = Glib::build_filename (session_template_dir, "2 Track-template");

	CPPUNIT_ASSERT (Glib::file_test (session_template_dir, Glib::FILE_TEST_IS_DIR));

	Session* new_session = 0;
	BusProfile* bus_profile = 0;

	create_and_start_dummy_backend ();

	// create a new session based on session template
	new_session = new Session (*AudioEngine::instance (), new_session_dir, session_name,
				bus_profile, session_template_dir);

	CPPUNIT_ASSERT (new_session);

	delete new_session;
	stop_and_destroy_backend ();

	// keep the same audio backend
	create_and_start_dummy_backend ();

	Session* template_session = 0;

	// reopen same session to check that it opens without error
	template_session = new Session (*AudioEngine::instance (), new_session_dir, session_name);

	CPPUNIT_ASSERT (template_session);

	delete template_session;
	stop_and_destroy_backend ();
}

void
SessionTest::open_session_utf8_path ()
{
	std::vector<std::string> utf8_strings;

	get_utf8_test_strings (utf8_strings);

	CPPUNIT_ASSERT (!utf8_strings.empty());

	const string test_dir = new_test_output_dir ("open_session_utf8_path");

	for (std::vector<std::string>::const_iterator i = utf8_strings.begin (); i != utf8_strings.end ();
	     ++i) {

		const string session_name (*i);
		std::string new_session_dir = Glib::build_filename (test_dir, session_name);
		bool new_session_failed = false;

		CPPUNIT_ASSERT (!Glib::file_test (new_session_dir, Glib::FILE_TEST_EXISTS));

		create_and_start_dummy_backend ();

		ARDOUR::Session* session = 0;

		try {
			session = new Session (*AudioEngine::instance(), new_session_dir, session_name);

			CPPUNIT_ASSERT (session);

		} catch(...) {
			new_session_failed = true;

			std::cerr << "Failed to create new session using name : " << *i << std::endl;
		}

		delete session;
		session = 0;
		stop_and_destroy_backend ();

		CPPUNIT_ASSERT (!new_session_failed);

		if (new_session_failed) break;

		create_and_start_dummy_backend ();

		bool open_session_failed = false;

		try {
			// reopen same session to check that it opens without error
			session = new Session (*AudioEngine::instance (), new_session_dir, session_name);

			CPPUNIT_ASSERT (session);
		} catch (...) {
			open_session_failed = true;

			std::cerr << "Failed to open session using name : " << *i << std::endl;
		}

		delete session;
		session = 0;
		stop_and_destroy_backend ();

		CPPUNIT_ASSERT (!open_session_failed);

		if (open_session_failed) break;
	}

}
