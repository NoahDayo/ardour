/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <sstream>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <ytkmm/entry.h>
#include <ytkmm/actiongroup.h>
#include <ytkmm/filechooserbutton.h>
#include <ytkmm/filechooserdialog.h>
#include <ytkmm/label.h>
#include <ytkmm/main.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/stock.h>
#include <ytkmm/textview.h>
#include <ytkmm/uimanager.h>

#include "pbd/basename.h"
#include "pbd/unwind.h"
#include "gtkmm2ext/actions.h"
#include "gtkmm2ext/utils.h"
#include "ardour/audioengine.h"
#include "ardour/project_session.h"
#include "ardour/studio_one_song.h"
#include "ardour/rc_configuration.h"
#include "ardour/session.h"
#include "ardour_dialog.h"
#include "ardour_ui.h"
#include "project_exchange_ui.h"
#include "pbd/i18n.h"

namespace PX = ARDOUR::ProjectExchange;

namespace {

bool exchange_active = false;

bool native_song (const std::string& path) { return path.size () >= 5 && !g_ascii_strcasecmp (path.c_str () + path.size () - 5, ".song"); }

void show_report (Gtk::Window& parent, const std::string& title, const std::string& message,
                  const std::vector<PX::Diagnostic>& report = {})
{
	ArdourDialog dialog (parent, title, true);
	Gtk::TextView text;
	Gtk::ScrolledWindow scroll;
	std::ostringstream content;
	content << message;
	for (auto const& item : report) {
		const char* disposition = item.disposition == PX::Disposition::Unsupported ? _("Unsupported") :
		                          item.disposition == PX::Disposition::Rendered ? _("Rendered audio") :
		                          item.disposition == PX::Disposition::Preserved ? _("Preserved") : _("Conversion");
		content << "\n\n" << disposition << ": " << item.object << "\n" << item.detail;
	}
	text.get_buffer ()->set_text (content.str ());
	text.set_editable (false);
	text.set_cursor_visible (false);
	text.set_wrap_mode (Gtk::WRAP_WORD_CHAR);
	text.set_left_margin (12);
	text.set_right_margin (12);
	scroll.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	scroll.add (text);
	dialog.get_vbox ()->pack_start (scroll);
	dialog.set_default_size (640, 360);
	dialog.add_button (Gtk::Stock::CLOSE, Gtk::RESPONSE_CLOSE);
	dialog.show_all_children ();
	dialog.run ();
}

class ImportDialog : public ArdourDialog {
public:
	ImportDialog (Gtk::Window& parent, const std::string& archive)
		: ArdourDialog (parent, _("Import Project"), true)
		, source (_("Choose a Studio One song or DAWproject file"), Gtk::FILE_CHOOSER_ACTION_OPEN)
		, folder (_("Choose the session location"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER)
	{
		get_vbox ()->set_spacing (10);
		get_vbox ()->set_border_width (12);
		filter.set_name (_("Studio One songs and DAWproject files"));
		filter.add_pattern ("*.song");
		filter.add_pattern ("*.SONG");
		filter.add_pattern ("*.dawproject");
		filter.add_pattern ("*.DAWPROJECT");
		source.add_filter (filter);
		Gtkmm2ext::add_volume_shortcuts (source);
		Gtkmm2ext::add_volume_shortcuts (folder);
		folder.set_current_folder (ARDOUR::Config->get_default_session_parent_dir ());
		add_row (_("Project file"), source);
		add_row (_("Session name"), name);
		add_row (_("Save in"), folder);
		Gtk::Label* hint = Gtk::manage (new Gtk::Label (_("Creates a new Ardour session with copied audio and MIDI clips.")));
		hint->set_line_wrap (true);
		get_vbox ()->pack_start (*hint, false, false);
		add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
		add_button (_("Import"), Gtk::RESPONSE_ACCEPT);
		set_default_response (Gtk::RESPONSE_ACCEPT);
		set_response_sensitive (Gtk::RESPONSE_ACCEPT, false);
		source.signal_file_set ().connect (sigc::mem_fun (*this, &ImportDialog::source_chosen));
		source.signal_selection_changed ().connect (sigc::mem_fun (*this, &ImportDialog::update));
		folder.signal_selection_changed ().connect (sigc::mem_fun (*this, &ImportDialog::update));
		name.signal_changed ().connect (sigc::mem_fun (*this, &ImportDialog::update));
		if (!archive.empty ()) {
			source.set_filename (archive);
			name.set_text (PBD::basename_nosuffix (archive));
		}
		set_default_size (600, -1);
		show_all_children ();
	}
	std::string archive () const { return source.get_filename (); }
	std::string snapshot () const { return name.get_text (); }
	std::string destination () const { return Glib::build_filename (folder.get_filename (), snapshot ()); }
private:
	void add_row (const char* title, Gtk::Widget& widget)
	{
		Gtk::HBox* row = Gtk::manage (new Gtk::HBox (false, 10));
		Gtk::Label* label = Gtk::manage (new Gtk::Label (title));
		label->set_size_request (120, -1);
		label->set_alignment (0, 0.5);
		row->pack_start (*label, false, false);
		row->pack_start (widget, true, true);
		get_vbox ()->pack_start (*row, false, false);
	}
	void source_chosen ()
	{
		name.set_text (PBD::basename_nosuffix (source.get_filename ()));
		update ();
	}
	void update ()
	{
		std::string value = snapshot ();
		bool valid = !archive ().empty () && !folder.get_filename ().empty () && !value.empty () &&
		             value != "." && value != ".." && value.find_first_of ("/\\:;<>\"|?*") == std::string::npos &&
		             ARDOUR::Session::session_name_is_legal (value).empty ();
		set_response_sensitive (Gtk::RESPONSE_ACCEPT, valid);
	}
	Gtk::FileFilter filter;
	Gtk::FileChooserButton source;
	Gtk::FileChooserButton folder;
	Gtk::Entry name;
};

/* Keep model access on the GUI's control thread. Modal input and temporarily
 * disabled application actions prevent edits during cancellation checks. */
class ExchangeProgress : public ArdourDialog {
public:
	ExchangeProgress (Gtk::Window& parent, const std::string& title)
		: ArdourDialog (parent, title, true), cancelled (false)
	{
		disallow_idle ();
		/* Block groups, leaving each action's own state available for engine
		 * and session notifications to update while we service GTK events. */
		for (auto const& group : ActionManager::ui_manager->get_action_groups ()) {
			groups.push_back (group);
			enabled.push_back (group->get_sensitive ());
			group->set_sensitive (false);
		}
		Gtk::Label* label = Gtk::manage (new Gtk::Label (_("Copying and converting the project. Please wait...")));
		get_vbox ()->pack_start (*label, false, false, 20);
		add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
		signal_response ().connect (sigc::mem_fun (*this, &ExchangeProgress::response));
		show_all ();
	}
	~ExchangeProgress ()
	{
		hide ();
		for (size_t i = 0; i < groups.size (); ++i) { groups[i]->set_sensitive (enabled[i]); }
	}
	bool check ()
	{
		while (!cancelled && Gtk::Main::events_pending ()) { Gtk::Main::iteration (false); }
		return cancelled;
	}
private:
	void response (int) { cancelled = true; }
	bool cancelled;
	std::vector<Glib::RefPtr<Gtk::ActionGroup>> groups;
	std::vector<bool> enabled;
};

} // namespace

bool ProjectExchangeUI::is_importable_project (std::string const& path)
{
	return native_song (path) || (path.size () >= 11 && !g_ascii_strcasecmp (path.c_str () + path.size () - 11, ".dawproject"));
}

bool ProjectExchangeUI::prepare_import (Gtk::Window* parent, std::string const& archive, ImportRequest& request)
{
	if (!parent || exchange_active) { return false; }
	PBD::Unwinder<bool> active (exchange_active, true);
	ImportDialog dialog (*parent, archive);
	if (dialog.run () != Gtk::RESPONSE_ACCEPT) { return false; }
	dialog.hide ();
	std::vector<PX::Diagnostic> report;
	try {
		if (Glib::file_test (dialog.destination (), Glib::FILE_TEST_EXISTS)) { throw PX::ExchangeError (_("That session folder already exists. Choose another session name.")); }
		ExchangeProgress progress (*parent, _("Checking Project"));
		auto check = [&] { return progress.check (); };
		auto project = native_song (dialog.archive ()) ? PX::StudioOneSong::read (dialog.archive (), check).project : PX::DawProject::read (dialog.archive (), check);
		report = project.report;
		if (PX::has_unsupported_features (project)) { throw PX::ExchangeError (_("This project contains features that cannot yet be imported.")); }
		request.archive = dialog.archive ();
		request.destination = dialog.destination ();
		request.snapshot = dialog.snapshot ();
		request.sample_rate = project.media.empty () ? 48000 : project.media.front ().sample_rate;
	} catch (PX::ArchiveCancelled const&) {
		return false;
	} catch (Glib::Exception const& error) {
		show_report (*parent, _("Project not imported"), error.what ().raw (), report);
		return false;
	} catch (std::exception const& error) {
		show_report (*parent, _("Project not imported"), error.what (), report);
		return false;
	} catch (...) {
		show_report (*parent, _("Project not imported"), _("An unexpected error occurred while checking the project."));
		return false;
	}
	return true;
}

void ProjectExchangeUI::import_dawproject (Gtk::Window* parent)
{
	if (ARDOUR_UI::instance ()->session_load_in_progress) { return; }
	ImportRequest request;
	if (prepare_import (parent, std::string (), request)) { import_prepared (parent, request, true); }
}

bool ProjectExchangeUI::import_prepared (Gtk::Window* parent, ImportRequest const& request, bool offer_session_dialog)
{
	ARDOUR_UI& ui = *ARDOUR_UI::instance ();
	if (!parent || exchange_active || ui.session_load_in_progress) { return false; }
	PBD::Unwinder<bool> active (exchange_active, true);
	auto& engine = *ARDOUR::AudioEngine::instance ();
	if (!engine.running ()) {
		show_report (*parent, _("Import Project"), _("Start the audio engine before importing a project."));
		return false;
	}
	const std::string& archive = request.archive;
	const std::string& destination = request.destination;
	const std::string& snapshot = request.snapshot;
	std::vector<PX::Diagnostic> report;

	std::string previous_path, previous_snapshot;
	if (auto previous = ui.the_session ()) {
		if (previous->transport_rolling () || previous->actively_recording ()) {
			show_report (*parent, _("Import Project"), _("Stop playback and recording before importing a project."));
			return false;
		}
		if (!previous->unnamed ()) { previous_path = previous->path (); previous_snapshot = previous->snap_name (); }
	}
	if (ui.unload_session ()) { return false; }
	bool imported = false;
	std::string failure;
	try {
		ExchangeProgress progress (*parent, _("Importing Project"));
		auto check = [&] { return progress.check (); };
		auto session = native_song (archive)
		    ? PX::SessionExchange::import_studio_one (engine, archive, destination, snapshot, report, check)
		    : PX::SessionExchange::import_dawproject (engine, archive, destination, snapshot, report, check);
		engine.remove_session ();
		session.reset ();
		imported = true;
	} catch (PX::ArchiveCancelled const&) {
		failure = _("Import cancelled.");
	} catch (Glib::Exception const& error) {
		failure = error.what ().raw ();
	} catch (std::exception const& error) {
		failure = error.what ();
	} catch (...) {
		failure = _("An unexpected error occurred while importing the project.");
	}
	if (imported) {
		if (ui.load_session (destination, snapshot) == 0) {
			show_report (*parent, _("Project imported"), _("The new session is ready. Review the conversion details below."), report);
			return true;
		}
		show_report (*parent, _("Session could not be opened"),
		             _("The imported session was saved here, but could not be opened:") + std::string ("\n") + destination, report);
	} else {
		show_report (*parent, _("Project not imported"), failure, report);
	}
	if (!ui.the_session ()) {
		if (!previous_path.empty () && ui.load_session (previous_path, previous_snapshot) == 0) { return false; }
		if (offer_session_dialog) { ui.start_session_load (false); }
	}
	return false;
}

static void export_project (Gtk::Window* parent, bool logic)
{
	ARDOUR_UI& ui = *ARDOUR_UI::instance ();
	ARDOUR::Session* session = ui.the_session ();
	if (!parent || !session || exchange_active || ui.session_load_in_progress) { return; }
	PBD::Unwinder<bool> active (exchange_active, true);
	std::string title = logic ? _("Export AAF + MIDI to a new folder") : _("Export DAWproject");
	if (session->transport_rolling () || session->actively_recording ()) {
		show_report (*parent, title, _("Stop playback and recording before exporting a project."));
		return;
	}
	Gtk::FileChooserDialog dialog (*parent, title, Gtk::FILE_CHOOSER_ACTION_SAVE);
	Gtkmm2ext::add_volume_shortcuts (dialog);
	dialog.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	dialog.add_button (_("Export"), Gtk::RESPONSE_ACCEPT);
	dialog.set_current_name (session->name () + (logic ? " AAF-MIDI" : ".dawproject"));
	dialog.set_current_folder (Glib::path_get_dirname (session->path ()));
	Gtk::FileFilter filter;
	filter.set_name (logic ? _("All files and folders") : _("DAWproject files"));
	filter.add_pattern (logic ? "*" : "*.dawproject");
	dialog.add_filter (filter);
	if (dialog.run () != Gtk::RESPONSE_ACCEPT) { return; }
	std::string destination = dialog.get_filename ();
	dialog.hide ();
	if (!logic && (destination.size () < 11 || g_ascii_strcasecmp (destination.c_str () + destination.size () - 11, ".dawproject"))) { destination += ".dawproject"; }
	std::vector<PX::Diagnostic> report;
	try {
		ExchangeProgress progress (*parent, logic ? _("Exporting AAF + MIDI") : _("Exporting DAWproject"));
		if (ui.the_session () != session) { throw PX::ExchangeError (_("The session changed. Choose the project to export again.")); }
		auto check = [&] {
			bool cancelled = progress.check ();
			if (ui.the_session () != session || session->transport_rolling ()) { throw PX::ExchangeError (_("The session changed during export. Stop playback and try again.")); }
			return cancelled;
		};
		if (logic) { PX::SessionExchange::export_logic (*session, destination, report, check); }
		else { PX::SessionExchange::export_dawproject (*session, destination, report, check); }
	} catch (PX::ArchiveCancelled const&) {
		show_report (*parent, _("Export cancelled"), _("No project was created."));
		return;
	} catch (Glib::Exception const& error) {
		show_report (*parent, _("Project not exported"), error.what ().raw (), report);
		return;
	} catch (std::exception const& error) {
		show_report (*parent, _("Project not exported"), error.what (), report);
		return;
	} catch (...) {
		show_report (*parent, _("Project not exported"), _("An unexpected error occurred while exporting the project."));
		return;
	}
	show_report (*parent, logic ? _("AAF + MIDI exported") : _("DAWproject exported"), _("Project saved to:") + std::string ("\n") + destination, report);
}

void ProjectExchangeUI::export_dawproject (Gtk::Window* parent) { export_project (parent, false); }
void ProjectExchangeUI::export_logic (Gtk::Window* parent) { export_project (parent, true); }
