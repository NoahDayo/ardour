/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <sstream>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <ytkmm/entry.h>
#include <ytkmm/checkbutton.h>
#include <ytkmm/actiongroup.h>
#include <ytkmm/filechooserbutton.h>
#include <ytkmm/filechooserdialog.h>
#include <ytkmm/label.h>
#include <ytkmm/main.h>
#include <ytkmm/scrolledwindow.h>
#include <ytkmm/spinbutton.h>
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
#include "ardour/logic_project.h"
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
bool aaf_project (const std::string& path) { return path.size () >= 4 && !g_ascii_strcasecmp (path.c_str () + path.size () - 4, ".aaf"); }

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
		, source (_("Choose a Studio One song, DAWproject or AAF file"), Gtk::FILE_CHOOSER_ACTION_OPEN)
		, midi_source (_("Choose the companion MIDI file"), Gtk::FILE_CHOOSER_ACTION_OPEN)
		, include_midi (_("Include companion MIDI (AAF only)"))
		, folder (_("Choose the session location"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER)
	{
		get_vbox ()->set_spacing (10);
		get_vbox ()->set_border_width (12);
		filter.set_name (_("Studio One songs, DAWproject and AAF files"));
		filter.add_pattern ("*.aaf");
		filter.add_pattern ("*.AAF");
		filter.add_pattern ("*.song");
		filter.add_pattern ("*.SONG");
		filter.add_pattern ("*.dawproject");
		filter.add_pattern ("*.DAWPROJECT");
		source.add_filter (filter);
		Gtkmm2ext::add_volume_shortcuts (source);
		Gtkmm2ext::add_volume_shortcuts (folder);
		folder.set_current_folder (ARDOUR::Config->get_default_session_parent_dir ());
		add_row (_("Project file"), source);
		midi_filter.set_name (_("Standard MIDI files"));
		for (auto const* pattern : {"*.mid", "*.MID", "*.midi", "*.MIDI"}) { midi_filter.add_pattern (pattern); }
		midi_source.add_filter (midi_filter);
		Gtkmm2ext::add_volume_shortcuts (midi_source);
		get_vbox ()->pack_start (include_midi, false, false);
		add_row (_("Companion MIDI"), midi_source);
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
		include_midi.signal_toggled ().connect (sigc::mem_fun (*this, &ImportDialog::update));
		midi_source.signal_selection_changed ().connect (sigc::mem_fun (*this, &ImportDialog::update));
		if (!archive.empty ()) {
			source.set_filename (archive);
			name.set_text (PBD::basename_nosuffix (archive));
			source_chosen ();
		}
		update ();
		set_default_size (600, -1);
		show_all_children ();
	}
	std::string archive () const { return source.get_filename (); }
	std::string midi () const { return aaf_project (archive ()) && include_midi.get_active () ? midi_source.get_filename ().raw () : std::string (); }
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
		midi_source.unselect_all ();
		std::string candidate = Glib::build_filename (Glib::path_get_dirname (archive ()), PBD::basename_nosuffix (archive ()) + ".mid");
		bool companion = aaf_project (archive ()) && Glib::file_test (candidate, Glib::FILE_TEST_IS_REGULAR);
		if (companion) { midi_source.set_filename (candidate); }
		include_midi.set_active (companion);
		update ();
	}
	void update ()
	{
		std::string value = snapshot ();
		include_midi.set_sensitive (aaf_project (archive ()));
		midi_source.set_sensitive (aaf_project (archive ()) && include_midi.get_active ());
		bool valid = !archive ().empty () && !folder.get_filename ().empty () && !value.empty () &&
		             value != "." && value != ".." && value.find_first_of ("/\\:;<>\"|?*") == std::string::npos &&
		             ARDOUR::Session::session_name_is_legal (value).empty ();
		if (aaf_project (archive ()) && include_midi.get_active () && midi ().empty ()) { valid = false; }
		set_response_sensitive (Gtk::RESPONSE_ACCEPT, valid);
	}
	Gtk::FileFilter filter;
	Gtk::FileChooserButton source;
	Gtk::FileFilter midi_filter;
	Gtk::FileChooserButton midi_source;
	Gtk::CheckButton include_midi;
	Gtk::FileChooserButton folder;
	Gtk::Entry name;
};

/* Keep model access on the GUI's control thread. Modal input and temporarily
 * disabled application actions prevent edits during cancellation checks. */
class ExchangeProgress : public ArdourDialog {
public:
	ExchangeProgress (Gtk::Window& parent, const std::string& title, bool rendering = false)
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
		Gtk::Label* label = Gtk::manage (new Gtk::Label (rendering ? _("Rendering the current mix and copying the project. Please wait...") : _("Copying and converting the project. Please wait...")));
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
	/* Keep Open's existing AAF workflow available while advanced AAF features
	 * are being added to the coordinated Session > Import Project workflow. */
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
		auto project = aaf_project (dialog.archive ()) ? PX::LogicProject (dialog.archive (), dialog.midi (), check).project () :
		               native_song (dialog.archive ()) ? PX::StudioOneSong::read (dialog.archive (), check).project : PX::DawProject::read (dialog.archive (), check);
		report = project.report;
		if (PX::has_unsupported_features (project)) { throw PX::ExchangeError (_("This project contains features that cannot yet be imported.")); }
		request.archive = dialog.archive ();
		request.midi = dialog.midi ();
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
		auto session = aaf_project (archive)
		    ? PX::SessionExchange::import_logic (engine, archive, request.midi, destination, snapshot, report, check)
		    : native_song (archive)
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

enum class ExportKind { DawProject, Logic, StudioOne };

static void export_project (Gtk::Window* parent, ExportKind kind)
{
	ARDOUR_UI& ui = *ARDOUR_UI::instance ();
	ARDOUR::Session* session = ui.the_session ();
	if (!parent || !session || exchange_active || ui.session_load_in_progress) { return; }
	PBD::Unwinder<bool> active (exchange_active, true);
	const bool dawproject = kind == ExportKind::DawProject;
	const bool native = kind == ExportKind::StudioOne;
	std::string title = native ? _("Export Studio One song to a new folder (Experimental)") : dawproject ? _("Export DAWproject") : _("Export AAF + MIDI to a new folder");
	if (session->transport_rolling () || session->actively_recording ()) {
		show_report (*parent, title, _("Stop playback and recording before exporting a project."));
		return;
	}
	Gtk::FileChooserDialog dialog (*parent, title, Gtk::FILE_CHOOSER_ACTION_SAVE);
	Gtkmm2ext::add_volume_shortcuts (dialog);
	dialog.add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	dialog.add_button (_("Export"), Gtk::RESPONSE_ACCEPT);
	dialog.set_current_name (session->name () + (native ? " Studio One" : dawproject ? ".dawproject" : " AAF-MIDI"));
	dialog.set_current_folder (Glib::path_get_dirname (session->path ()));
	Gtk::FileFilter filter;
	filter.set_name (dawproject ? _("DAWproject files") : _("All files and folders"));
	filter.add_pattern (dawproject ? "*.dawproject" : "*");
	dialog.add_filter (filter);
	Gtk::VBox options_box (false, 8);
	Gtk::Label native_hint (_("Creates Project.song and a Media folder. Opening and playback in Studio One / Studio Pro have not yet been verified. Keep the complete folder together."));
	native_hint.set_line_wrap (true);
	native_hint.set_alignment (0, 0.5);
	if (native) { options_box.pack_start (native_hint, false, false); }
	Gtk::CheckButton render_mix (_("Render current mix as one stereo audio track"));
	Gtk::Label render_hint (_("Includes instruments, effects, automation, routing, mute and solo. Individual tracks and MIDI notes become audio. Renders from timeline zero through session end, plus the extra time below. Content within that extra time is included too."));
	render_hint.set_line_wrap (true);
	render_hint.set_alignment (0, 0.5);
	Gtk::HBox tail_row (false, 8);
	Gtk::Label tail_label (_("Extend past session end (seconds):"));
	Gtk::SpinButton tail;
	tail.set_range (0, 600);
	tail.set_increments (0.1, 1);
	tail.set_digits (1);
	tail.set_value (2);
	tail_row.pack_start (tail_label, false, false);
	tail_row.pack_start (tail, false, false);
	tail_row.set_sensitive (false);
	render_mix.signal_toggled ().connect ([&] { tail_row.set_sensitive (render_mix.get_active ()); });
	options_box.pack_start (render_mix, false, false);
	options_box.pack_start (render_hint, false, false);
	options_box.pack_start (tail_row, false, false);
	options_box.show_all ();
	dialog.set_extra_widget (options_box);
	if (dialog.run () != Gtk::RESPONSE_ACCEPT) { return; }
	std::string destination = dialog.get_filename ();
	PX::SessionExchange::ExportOptions options;
	options.render_mix = render_mix.get_active ();
	options.render_tail_seconds = tail.get_value ();
	dialog.hide ();
	if (dawproject && (destination.size () < 11 || g_ascii_strcasecmp (destination.c_str () + destination.size () - 11, ".dawproject"))) { destination += ".dawproject"; }
	std::vector<PX::Diagnostic> report;
	try {
		ExchangeProgress progress (*parent, native ? _("Exporting Studio One song") : dawproject ? _("Exporting DAWproject") : _("Exporting AAF + MIDI"), options.render_mix);
		if (ui.the_session () != session) { throw PX::ExchangeError (_("The session changed. Choose the project to export again.")); }
		auto check = [&] {
			bool cancelled = progress.check ();
			if (ui.the_session () != session || session->actively_recording () ||
			    (session->transport_rolling () && !(options.render_mix && session->exporting ()))) {
				throw PX::ExchangeError (_("The session changed during export. Stop playback and try again."));
			}
			return cancelled;
		};
		if (native) { PX::SessionExchange::export_studio_one (*session, destination, report, options, check); }
		else if (dawproject) { PX::SessionExchange::export_dawproject (*session, destination, report, options, check); }
		else { PX::SessionExchange::export_logic (*session, destination, report, options, check); }
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
	show_report (*parent, native ? _("Studio One song exported") : dawproject ? _("DAWproject exported") : _("AAF + MIDI exported"), _("Project saved to:") + std::string ("\n") + destination, report);
}

void ProjectExchangeUI::export_dawproject (Gtk::Window* parent) { export_project (parent, ExportKind::DawProject); }
void ProjectExchangeUI::export_logic (Gtk::Window* parent) { export_project (parent, ExportKind::Logic); }
void ProjectExchangeUI::export_studio_one (Gtk::Window* parent) { export_project (parent, ExportKind::StudioOne); }
