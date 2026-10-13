/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "project_render.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <sndfile.h>

#include "midi++/mmc.h"
#include "pbd/controllable.h"
#include "pbd/mutex.h"
#include "pbd/scoped_file_descriptor.h"
#include "pbd/signals.h"

#include "ardour/audioengine.h"
#include "ardour/audio_port.h"
#include "ardour/automation_control.h"
#include "ardour/automation_list.h"
#include "ardour/butler.h"
#include "ardour/export_channel.h"
#include "ardour/export_channel_configuration.h"
#include "ardour/export_filename.h"
#include "ardour/export_format_specification.h"
#include "ardour/export_formats.h"
#include "ardour/export_handler.h"
#include "ardour/export_status.h"
#include "ardour/export_timespan.h"
#include "ardour/io.h"
#include "ardour/plugin_insert.h"
#include "ardour/port.h"
#include "ardour/port_insert.h"
#include "ardour/port_set.h"
#include "ardour/project_exchange.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/send.h"
#include "ardour/slavable_automation_control.h"
#include "ardour/track.h"
#include "ardour/triggerbox.h"

namespace ARDOUR { namespace ProjectExchange {
namespace {

void check_cancel (const CancelCheck& cancel)
{
	if (cancel && cancel ()) { throw ArchiveCancelled (); }
}

template <typename Predicate>
void wait_for (Predicate ready, const char* failure)
{
	const gint64 deadline = g_get_monotonic_time () + 10000000;
	while (!ready ()) {
		if (g_get_monotonic_time () >= deadline) { throw ExchangeError (failure); }
		g_usleep (1000);
	}
}

bool external_input (Session& session, const std::shared_ptr<IO>& input)
{
	if (!input) { return false; }
	for (auto const& port : *input->ports ()) {
		std::vector<std::string> connections;
		port->get_connections (connections);
		for (auto const& connection : connections) {
			if (!session.engine ().port_is_mine (connection)) { return true; }
		}
	}
	return false;
}

struct SavedControl {
	std::shared_ptr<AutomationControl> control;
	AutoState mode;
	double value;
};

struct RenderedFile {
	explicit RenderedFile (const std::string& name) : path (name), keep (false) {}
	~RenderedFile () { if (!keep) { g_unlink (path.c_str ()); } }
	std::string path;
	bool keep;
};

struct StatusSnapshot {
	bool running, aborted, errors, stopped;
	samplecnt_t processed;
};

StatusSnapshot status_snapshot (const std::shared_ptr<ExportStatus>& status)
{
	PBD::Mutex::Lock lock (status->lock ());
	return {status->running (), status->aborted (), status->errors (), status->stop, status->processed_samples};
}

class SourceState {
public:
	explicit SourceState (Session& session)
		: _session (session)
		, _position (session.transport_sample ())
		, _dirty (session.dirty ())
		, _mmc (session.mmc ().send_enabled ())
	{
		PBD::Mutex::Lock process (_session.engine ().process_lock ());
		PBD::ControllableSet controls;
		for (auto const& route : *session.get_routes ()) { route->automatables (controls); }
		for (auto const& value : controls) {
			auto control = std::dynamic_pointer_cast<AutomationControl> (value);
			if (!control || (control->flags () & PBD::Controllable::NotAutomatable)) { continue; }
			if (control->touching ()) { throw ExchangeError ("Release touched controls before rendering the mix"); }
			if (auto slaved = std::dynamic_pointer_cast<SlavableAutomationControl> (control)) {
				if (slaved->slaved ()) { throw ExchangeError ("Remove VCA control assignments before rendering the mix"); }
			}
			AutoState mode = control->automation_state ();
			if (mode == Write && control->toggled ()) {
				throw ExchangeError ("Switch toggle automation out of Write before rendering the mix");
			}
			if ((mode == Touch || mode == Latch) && control->alist () && control->alist ()->empty ()) {
				throw ExchangeError ("Empty touch or latch automation cannot yet be restored safely after mix rendering");
			}
			_controls.push_back ({control, mode, control->get_value ()});
		}
	}

	void protect ()
	{
		PBD::Mutex::Lock process (_session.engine ().process_lock ());
		for (auto const& saved : _controls) {
			if (saved.mode == Write) { saved.control->set_automation_state (Off); }
			else if (saved.mode == Touch || saved.mode == Latch) { saved.control->set_automation_state (Play); }
		}
	}

	void restore ()
	{
		wait_for ([&] { return !_session.butler ()->transport_work_requested (); },
		          "Transport stop work did not finish; source automation remains protected. Restart Ardour before editing");
		std::exception_ptr position_failure;
		try {
			if (!_session.engine ().running ()) { throw ExchangeError ("The audio engine stopped before the source position could be restored"); }
			wait_for ([&] { return !_session.engine ().freewheeling (); }, "The audio engine did not leave offline rendering");
			auto located = std::make_shared<std::atomic<bool>> (false);
			PBD::ScopedConnection connection;
			_session.Located.connect_same_thread (connection, [located] { located->store (true); });
			samplecnt_t cycle;
			{
				PBD::Mutex::Lock lock (_session.engine ().process_lock ());
				cycle = _session.engine ().processed_samples ();
			}
			_session.request_locate (_position, true, MustStop, TRS_UI);
			wait_for ([&] {
				PBD::Mutex::Lock lock (_session.engine ().process_lock ());
				return located->load () && _session.engine ().processed_samples () != cycle &&
				       _session.transport_sample () == _position && _session.transport_stopped () &&
				       !_session.locate_pending () && !_session.locate_initiated ();
			}, "The source transport position could not be restored after rendering");
		} catch (...) { position_failure = std::current_exception (); }

		/* Exclude the processing callback, including any export callback copied
		 * before its connection was dropped. No export finalization runs here. */
		{
			PBD::Mutex::Lock lock (_session.engine ().process_lock ());
			for (auto const& saved : _controls) { saved.control->set_automation_state (Off); }
			for (auto const& saved : _controls) {
				if (saved.control->get_value () != saved.value) { saved.control->set_value_unchecked (saved.value); }
			}
			for (auto const& saved : _controls) { saved.control->set_automation_state (saved.mode); }
		}
		_session.mmc ().enable_send (_mmc);
		if (_dirty) { _session.set_dirty (); }
		else { _session.set_clean (); }
		if (position_failure) { std::rethrow_exception (position_failure); }
	}

private:
	Session& _session;
	samplepos_t _position;
	bool _dirty;
	bool _mmc;
	std::vector<SavedControl> _controls;
};

void preflight (Session& session, samplecnt_t length)
{
	if (!session.engine ().running () || session.engine ().in_process_thread ()) {
		throw ExchangeError ("Mix rendering requires the session control thread and a running audio engine");
	}
	if (session.exporting () || session.engine ().freewheeling ()) { throw ExchangeError ("Finish the active export before rendering the mix"); }
	if (session.transport_speed (true) != 0 || !session.transport_stopped () || session.actively_recording ()) {
		throw ExchangeError ("Stop playback and recording before rendering the mix");
	}
	if (session.get_record_enabled ()) { throw ExchangeError ("Disarm session recording before rendering the mix"); }
	if (session.get_play_loop () || session.get_play_range ()) { throw ExchangeError ("Disable loop and range playback before rendering the mix"); }
	if (session.config.get_external_sync ()) { throw ExchangeError ("Disable external synchronization before offline mix rendering"); }
	if (session.locate_pending () || session.locate_initiated () || session.global_locate_pending ()) {
		throw ExchangeError ("Wait for the transport locate to finish before rendering the mix");
	}
	if (length <= 0 || session.sample_rate () <= 0 || session.nominal_sample_rate () != session.sample_rate ()) {
		throw ExchangeError ("Mix rendering requires a positive range and an unmodified session sample rate");
	}
	auto master = session.master_out ();
	if (!master || master->output ()->n_ports ().n_audio () != 2 || master->output ()->n_ports ().n_total () != 2) {
		throw ExchangeError ("Mix rendering requires exactly two master audio outputs; no automatic downmix is performed");
	}
	for (auto const& route : *session.get_routes ()) {
		if (!route->active () || route->is_auditioner () || route->is_monitor ()) { continue; }
		auto track = std::dynamic_pointer_cast<ARDOUR::Track> (route);
		if (track && track->rec_enable_control ()->get_value () != 0) {
			throw ExchangeError ("Disarm track recording before rendering the mix: " + route->name ());
		}
		if ((!track || (route->monitoring_state () & MonitoringInput)) && external_input (session, route->input ())) {
			throw ExchangeError ("Disconnect live external inputs before offline mix rendering: " + route->name ());
		}
		if (!route->is_master () && external_input (session, route->output ())) {
			throw ExchangeError ("Route external outputs through the master before rendering the mix: " + route->name ());
		}
		if (auto triggers = route->triggerbox ()) {
			if (!triggers->empty () || triggers->currently_playing () || triggers->peek_next_trigger () || triggers->armed () || triggers->rec_enabled ()) {
				throw ExchangeError ("Clear clip launcher slots and pending launches before offline mix rendering: " + route->name ());
			}
		}
		route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			auto processor = weak.lock ();
			if (!processor || !processor->active ()) { return; }
			if (std::dynamic_pointer_cast<PortInsert> (processor)) {
				throw ExchangeError ("External hardware inserts require realtime capture and cannot be rendered offline: " + route->name ());
			}
			if (auto send = std::dynamic_pointer_cast<ARDOUR::Send> (processor)) {
				if (external_input (session, send->output ())) {
					throw ExchangeError ("Route external sends through the master before rendering the mix: " + route->name ());
				}
			}
			if (auto plugin = std::dynamic_pointer_cast<PluginInsert> (processor)) {
				if (external_input (session, plugin->sidechain_input ())) {
					throw ExchangeError ("External sidechains require realtime capture and cannot be rendered offline: " + route->name ());
				}
			}
		});
	}
}

} // namespace

RenderedMix
render_session_mix (Session& session, const std::string& stage_directory, samplecnt_t length, const CancelCheck& cancel)
{
	preflight (session, length);
	if (!Glib::path_is_absolute (stage_directory) || !Glib::file_test (stage_directory, Glib::FILE_TEST_IS_DIR)) {
		throw ExchangeError ("Mix rendering requires an existing absolute staging directory");
	}
	check_cancel (cancel);
	SourceState source (session);
	RenderedMix result;
	result.path = Glib::build_filename (stage_directory, "master-render.wav");
	result.channels = 2;
	result.frames = length;
	result.sample_rate = session.nominal_sample_rate ();
	if (Glib::file_test (result.path, Glib::FILE_TEST_EXISTS)) { throw ExchangeError ("Rendered mix destination already exists"); }
	RenderedFile output_file (result.path);

	auto handler = session.get_export_handler ();
	auto status = session.get_export_status ();
	std::exception_ptr failure;
	bool protected_source = false;
	try {
		handler->reset ();
		auto span = handler->add_timespan ();
		span->set_name ("Exchange mix");
		span->set_range (0, length);
		span->set_realtime (false);
		span->set_vapor ("");
		auto channels = handler->add_channel_config ();
		channels->clear_channels ();
		channels->set_name ("Master mix");
		channels->set_split (false);
		channels->set_region_processing_type (RegionExportChannelFactory::None);
		for (uint32_t channel = 0; channel < result.channels; ++channel) {
			ComparableSharedPtr<PortExportChannel> tap (new PortExportChannel);
			tap->add_port (session.master_out ()->output ()->audio (channel));
			channels->register_channel (tap);
		}
		auto format = handler->add_format ();
		auto wav = std::make_shared<ExportFormatLinear> ("WAV", ExportFormatBase::F_WAV);
		wav->add_sample_format (ExportFormatBase::SF_Float);
		wav->set_extension ("wav");
		format->set_format (wav);
		format->set_name ("Exchange float WAV");
		format->set_sample_format (ExportFormatBase::SF_Float);
		format->set_sample_rate (static_cast<ExportFormatBase::SampleRate> (result.sample_rate));
		format->set_endianness (ExportFormatBase::E_FileDefault);
		format->set_quality (ExportFormatBase::Q_LosslessLinear);
		format->set_dither_type (ExportFormatBase::D_None);
		format->set_src_quality (ExportFormatBase::SRC_SincBest);
		format->set_trim_beginning (false);
		format->set_trim_end (false);
		format->set_silence_beginning (AnyTime ());
		format->set_silence_end (AnyTime ());
		format->set_normalize (false);
		format->set_normalize_loudness (false);
		format->set_use_tp_limiter (false);
		format->set_tag (false);
		format->set_with_cue (false);
		format->set_with_toc (false);
		format->set_with_mp4chaps (false);
		format->set_soundcloud_upload (false);
		format->set_command ("");
		format->set_analyse (false);
		format->set_reimport (false);
		format->set_demo_noise_duration (0);
		format->set_demo_noise_interval (0);
		auto filename = handler->add_filename ();
		filename->include_label = true;
		filename->include_session = filename->use_session_snapshot_name = filename->include_revision = false;
		filename->include_channel_config = filename->include_format_name = filename->include_channel = false;
		filename->include_timespan = filename->include_time = filename->include_date = false;
		filename->set_date_format (ExportFilename::D_None);
		filename->set_time_format (ExportFilename::T_None);
		filename->set_label ("master-render");
		filename->include_label = true;
		filename->set_timespan (span);
		filename->set_channel_config (channels);
		if (!filename->set_folder (stage_directory) || filename->get_path (format) != result.path || !format->is_complete ()) {
			throw ExchangeError ("Cannot configure the rendered mix output");
		}
		handler->add_export_config (span, channels, format, filename, BroadcastInfoPtr ());
		check_cancel (cancel);
		protected_source = true;
		source.protect ();
		if (handler->do_export () != 0) { throw ExchangeError ("Cannot start offline mix rendering"); }
		gint64 progressed = g_get_monotonic_time ();
		auto snapshot = status_snapshot (status);
		samplecnt_t previous = snapshot.processed;
		while (snapshot.running) {
			check_cancel (cancel);
			if (!session.engine ().running ()) { throw ExchangeError ("The audio engine stopped during mix rendering"); }
			if (snapshot.processed != previous) { previous = snapshot.processed; progressed = g_get_monotonic_time (); }
			if (g_get_monotonic_time () - progressed > 30000000) { throw ExchangeError ("Offline mix rendering stopped making progress"); }
			g_usleep (10000);
			snapshot = status_snapshot (status);
		}
		if (!session.engine ().running ()) { throw ExchangeError ("The audio engine stopped during mix rendering"); }
		if (snapshot.aborted || snapshot.errors) { throw ExchangeError ("Offline mix rendering failed"); }
		check_cancel (cancel);
	} catch (...) { failure = std::current_exception (); }

	bool cleaned = false;
	/* Keep both objects alive through Finished: its Session callback releases
	 * the Session's owning references and requests freewheel shutdown. */
	try {
		if (session.exporting ()) {
			if (failure || status_snapshot (status).running) { status->abort (bool (failure)); }
			/* Finalization flushes live processors. Leave offline mode without
			 * holding either lock: the export helper may hold the status lock
			 * while waiting for the backend's freewheel acknowledgement. Stop
			 * the graph through its native export callback, preserving solo selection. */
			try {
				wait_for ([&] {
					auto snapshot = status_snapshot (status);
					PBD::Mutex::Lock lock (session.engine ().process_lock ());
					return !session.engine ().running () ||
					       (session.transport_stopped () && (snapshot.stopped || snapshot.processed >= length));
				}, "Offline mix rendering could not stop its processing graph");
				int stopped = session.engine ().freewheel (false);
				if (stopped != 0 && session.engine ().running ()) { throw ExchangeError ("Cannot stop offline mix rendering"); }
				wait_for ([&] { return !session.engine ().running () || !session.engine ().freewheeling (); }, "The audio engine did not leave offline rendering");
			} catch (...) {
				failure = std::current_exception ();
				/* A backend that cannot leave freewheel must be stopped before
				 * restoring automation or releasing any graph resources. */
				if (session.engine ().running () && session.engine ().stop () != 0) {
					throw RenderShutdownFailed ("The audio engine could not stop. Restart Ardour before editing this session. Temporary audio is kept at: " + stage_directory);
				}
			}
			PBD::Mutex::Lock lock (session.engine ().process_lock ());
			status->finish (TRS_UI);
		}
		{
			PBD::Mutex::Lock lock (status->lock ());
			handler->reset ();
		}
		handler.reset ();
		cleaned = true;
	} catch (...) { failure = std::current_exception (); }
	if (!cleaned) {
		output_file.keep = true;
		throw RenderShutdownFailed ("Mix cleanup could not finish. Restart Ardour before editing this session. Temporary audio is kept at: " + stage_directory);
	}
	if (protected_source) {
		try { source.restore (); }
		catch (...) { failure = std::current_exception (); }
	}
	if (failure) { std::rethrow_exception (failure); }

	SF_INFO info = {};
	PBD::ScopedFileDescriptor fd (g_open (result.path.c_str (), O_RDONLY
#ifdef O_BINARY
	                                    | O_BINARY
#endif
	                                    , 0));
	std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> file (fd < 0 ? nullptr : sf_open_fd (fd, SFM_READ, &info, SF_FALSE), sf_close);
	if (!file || info.channels != int (result.channels) || info.frames != length ||
	    info.samplerate != result.sample_rate || (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_FLOAT) {
		throw ExchangeError ("Rendered mix length, channels or sample rate do not match the requested range");
	}
	std::vector<float> pcm (8192 * result.channels);
	for (samplecnt_t frame = 0; frame < length;) {
		check_cancel (cancel);
		sf_count_t count = std::min (samplecnt_t (8192), length - frame);
		if (sf_readf_float (file.get (), pcm.data (), count) != count) { throw ExchangeError ("Rendered mix audio is incomplete"); }
		for (size_t sample = 0; sample < size_t (count) * result.channels; ++sample) {
			if (!std::isfinite (pcm[sample])) { throw ExchangeError ("Rendered mix contains non-finite audio samples"); }
		}
		frame += count;
	}
	if (sf_error (file.get ()) != SF_ERR_NO_ERROR) { throw ExchangeError ("Cannot verify the rendered mix audio"); }
	check_cancel (cancel);
	output_file.keep = true;
	return result;
}

} }
