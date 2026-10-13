
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include <stdexcept>
#include <chrono>
#include <future>
#include <atomic>
#include <thread>

#include "pbd/textreceiver.h"
#include "pbd/file_utils.h"
#include "ardour/session.h"
#include "ardour/audioengine.h"
#include "ardour/smf_source.h"
#include "ardour/midi_model.h"
#include "ardour/midi_patch_manager.h"
#include "ardour/search_paths.h"
#include "ardour/port_engine_shared.h"
#include "ardour/auditioner.h"
#include "ardour/filesystem_paths.h"
#include "ardour/amp.h"
#include "ardour/gain_control.h"

#include "test_util.h"

#include "session_test.h"

CPPUNIT_TEST_SUITE_REGISTRATION (SessionTest);

using namespace std;
using namespace ARDOUR;
using namespace PBD;

namespace {
class ConnectionTestPort : public BackendPort {
public:
	ConnectionTestPort (PortEngineSharedImpl& backend, const std::string& name, PortFlags flags)
		: BackendPort (backend, name, flags) {}
	DataType type () const override { return DataType::AUDIO; }
	void* get_buffer (pframes_t) override { return nullptr; }
};

class ConnectionTestBackend : public PortEngineSharedImpl {
public:
	ConnectionTestBackend (PortManager& manager) : PortEngineSharedImpl (manager, "Connections") {}
	~ConnectionTestBackend () {
		unregister_ports ();
		for (auto* change : _port_connection_queue) { delete change; }
	}
private:
	BackendPort* port_factory (const std::string& name, DataType, PortFlags flags) override {
		return new ConnectionTestPort (*this, name, flags);
	}
};
}

void
SessionTest::backend_connections ()
{
	ConnectionTestBackend backend (*AudioEngine::create ());
	auto output = std::dynamic_pointer_cast<BackendPort> (backend.register_port ("source", DataType::AUDIO, IsOutput));
	auto input = std::dynamic_pointer_cast<BackendPort> (backend.register_port ("sink", DataType::AUDIO, IsInput));
	CPPUNIT_ASSERT (output && input);
	CPPUNIT_ASSERT_EQUAL (0, backend.connect (output, input->name ()));
	auto old_output = output->get_connections ();
	auto old_input = input->get_connections ();
	CPPUNIT_ASSERT_EQUAL (0, backend.disconnect (output, input->name ()));
	CPPUNIT_ASSERT (output->get_connections ()->empty ());
	CPPUNIT_ASSERT (input->get_connections ()->empty ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), old_output->size ());
	CPPUNIT_ASSERT_EQUAL (size_t (1), old_input->size ());
	CPPUNIT_ASSERT (*old_output->begin () == input);
	CPPUNIT_ASSERT (*old_input->begin () == output);

	/* Latency/graph readers run outside the engine process lock. Their
	 * connection iterators must survive concurrent connect/disconnect. */
	std::atomic<bool> done (false), valid (true);
	std::atomic<unsigned> reads (0);
	std::promise<void> reading;
	auto started = reading.get_future ();
	std::thread reader ([&] {
		reading.set_value ();
		do {
			auto snapshot = input->get_connections ();
			for (auto const& port : *snapshot) {
				if (port != output) { valid.store (false); }
			}
			std::vector<std::string> names;
			if (backend.get_connections (input, names, false) < 0 || names.size () > 1 ||
			    (!names.empty () && names.front () != output->name ())) { valid.store (false); }
			++reads;
		} while (!done.load ());
	});
	started.wait ();
	bool changed = true;
	for (unsigned i = 0; i < 2000; ++i) {
		if (backend.connect (output, input->name ()) || backend.disconnect (output, input->name ())) { changed = false; break; }
	}
	done.store (true);
	reader.join ();
	CPPUNIT_ASSERT (changed && valid.load () && reads.load () > 0);
	CPPUNIT_ASSERT (output->get_connections ()->empty () && input->get_connections ()->empty ());

	/* Retained generations own ports, but disconnected graphs must not
	 * leave mutual references in the RCU manager after readers release. */
	std::weak_ptr<BackendPort> weak_output = output, weak_input = input;
	backend.unregister_port (output);
	backend.unregister_port (input);
	output.reset (); input.reset ();
	CPPUNIT_ASSERT (!weak_output.expired () && !weak_input.expired ());
	old_output.reset (); old_input.reset ();
	CPPUNIT_ASSERT (weak_output.expired () && weak_input.expired ());
}

void
SessionTest::misc_port_state ()
{
	struct Fixture {
		Fixture () { create_and_start_dummy_backend (); }
		~Fixture () { AudioEngine::instance ()->remove_session (); session.reset (); stop_and_destroy_backend (); }
		std::unique_ptr<Session> session;
	} fixture;
	const auto path = Glib::build_filename (new_test_output_dir ("misc-ports"), "Ports");
	fixture.session.reset (load_session (path, "Ports"));
	Session& session = *fixture.session;
	CPPUNIT_ASSERT (session.the_auditioner ());
	XMLNode patch ("SessionPorts");
	session.the_auditioner ()->update_misc_port_state (patch);
	patch.child ("Auditioner")->add_child ("Monitor")->set_property ("sequence", "initial");
	session.merge_misc_port_state (patch);
	auto saved = session.misc_port_state ();
	CPPUNIT_ASSERT (saved && saved->child ("Auditioner")->child ("DirectOut"));
	/* disconnect emits output_changed synchronously. Restoring the saved
	 * DirectOut must not dereference the child that callback replaces. */
	CPPUNIT_ASSERT_EQUAL (0, session.the_auditioner ()->connect ());
	CPPUNIT_ASSERT (saved->child ("Auditioner")->child ("DirectOut"));
	auto click_gain = session.click_gain ()->gain_control ();
	click_gain->set_value (0.25, PBD::Controllable::NoGroup);
	XMLNode click_patch ("SessionPorts");
	click_patch.add_child ("Click")->add_child_nocopy (session.click_gain ()->get_state ());
	session.merge_misc_port_state (click_patch);
	click_gain->set_value (0.375, PBD::Controllable::NoGroup);
	CPPUNIT_ASSERT_EQUAL (0, session.save_state ("Ports"));
	session.reconnect_click_output ();
	CPPUNIT_ASSERT_DOUBLES_EQUAL (0.375, click_gain->get_value (), 1e-7);

	std::atomic<bool> done (false);
	std::atomic<unsigned> updates (0);
	std::promise<void> writing;
	auto started = writing.get_future ();
	std::thread writer ([&] {
		writing.set_value ();
		do {
			XMLNode change ("SessionPorts");
			change.add_child ("Auditioner")->add_child ("Monitor")->set_property ("sequence", ++updates);
			session.merge_misc_port_state (change);
		} while (!done.load ());
	});
	started.wait ();
	bool valid = true;
	const auto rcfile = Glib::build_filename (user_config_directory (), "session_port.rc");
	for (unsigned i = 0; i < 20; ++i) {
		if (session.save_state ("Ports")) { valid = false; break; }
		XMLTree parsed;
		if (!parsed.read (rcfile) || !parsed.root () || parsed.root ()->name () != "SessionPorts") { valid = false; break; }
		auto auditioner = parsed.root ()->child ("Auditioner");
		if (!auditioner || !auditioner->child ("DirectOut") || !auditioner->child ("Monitor")) { valid = false; break; }
		auto snapshot = session.misc_port_state ();
		std::string before, after;
		snapshot->child ("Auditioner")->child ("Monitor")->get_property ("sequence", before);
		g_usleep (500);
		snapshot->child ("Auditioner")->child ("Monitor")->get_property ("sequence", after);
		if (before != after) { valid = false; break; }
	}
	done.store (true);
	writer.join ();
	CPPUNIT_ASSERT (valid && updates.load () > 0);
	std::string initial;
	CPPUNIT_ASSERT (saved->child ("Auditioner")->child ("Monitor")->get_property ("sequence", initial));
	CPPUNIT_ASSERT_EQUAL (std::string ("initial"), initial);
	CPPUNIT_ASSERT_EQUAL (0, session.save_state ("Ports"));
	click_gain.reset ();
	AudioEngine::instance ()->remove_session ();
	fixture.session.reset ();
	fixture.session.reset (load_session (path, "Ports"));
	auto restored = fixture.session->misc_port_state ();
	CPPUNIT_ASSERT (restored && restored->child ("Auditioner")->child ("DirectOut"));
	unsigned sequence = 0;
	CPPUNIT_ASSERT (restored->child ("Auditioner")->child ("Monitor")->get_property ("sequence", sequence));
	CPPUNIT_ASSERT_EQUAL (updates.load (), sequence);
}

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
