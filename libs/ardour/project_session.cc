/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_session.h"
#include "ardour/studio_one_song.h"
#include "ardour/logic_project.h"
#include "project_render.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sndfile.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "pbd/controllable.h"
#include "pbd/control_math.h"
#include "evoral/Curve.h"
#include "evoral/Control.h"
#include "evoral/TypeMap.h"
#include "pbd/scoped_file_descriptor.h"
#include "pbd/xml++.h"
#include "temporal/tempo.h"
#include "evoral/Event.h"
#include "ardour/audioengine.h"
#include "ardour/amp.h"
#include "ardour/audioregion.h"
#include "ardour/audio_track.h"
#include "ardour/audio_port.h"
#include "ardour/audiofilesource.h"
#include "ardour/automation_list.h"
#include "ardour/filesystem_paths.h"
#include "ardour/import_status.h"
#include "ardour/internal_send.h"
#include "ardour/io.h"
#include "ardour/lua_api.h"
#include "ardour/midi_region.h"
#include "ardour/midi_model.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/panner_shell.h"
#include "ardour/parameter_types.h"
#include "ardour/pannable.h"
#include "ardour/phase_control.h"
#include "ardour/polarity_processor.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/triggerbox.h"
#include "ardour/velocity_control.h"
#include "ardour/location.h"
#include "ardour/region_factory.h"
#include "ardour/recent_sessions.h"
#include "ardour/route_group.h"
#include "ardour/session.h"
#include "ardour/source_factory.h"

namespace ARDOUR { namespace ProjectExchange {
namespace {

void cancelled (const CancelCheck& check)
{
	if (check && check ()) { throw ArchiveCancelled (); }
}

bool default_plugin_mapping (const PluginInsert& insert)
{
	if (insert.get_count () != 1 || insert.has_sidechain () || insert.thru_map ().n_total ()) { return false; }
	ChanMapping input;
	auto plugin = insert.plugin (0);
	for (DataType::iterator type = DataType::begin (); type != DataType::end (); ++type) {
		uint32_t shift = 0, next = 0, count = insert.input_streams ().get (*type);
		for (uint32_t pin = 0; pin < insert.natural_input_streams ().get (*type); ++pin) {
			if (plugin->describe_io_port (*type, true, pin).is_sidechain) { ++shift; continue; }
			if (insert.splitting ()) {
				if (count) { input.set (*type, pin, next++ % count); }
			} else if (pin - shift < count) { input.set (*type, pin, pin - shift); }
		}
	}
	ChanMapping output (ChanCount::min (insert.natural_output_streams (), insert.output_streams ()));
	return input == insert.input_map (0) && output == insert.output_map (0);
}

/* validate_vst3_state() is called before reading these offsets.  A host may
 * omit an empty controller chunk or metadata when saving its own preset. */
uint64_t vst3_unsigned_at (const std::string& state, size_t offset, unsigned bytes)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < bytes; ++i) { value |= uint64_t (uint8_t (state[offset + i])) << (8 * i); }
	return value;
}

std::pair<size_t, size_t> vst3_chunk (const std::string& state, const char* type)
{
	size_t list = size_t (vst3_unsigned_at (state, 40, 8));
	size_t count = size_t (vst3_unsigned_at (state, list + 4, 4));
	for (size_t i = 0; i < count; ++i) {
		size_t entry = list + 8 + i * 20;
		if (state.compare (entry, 4, type) == 0) {
			return {size_t (vst3_unsigned_at (state, entry + 4, 8)), size_t (vst3_unsigned_at (state, entry + 12, 8))};
		}
	}
	return {0, 0};
}

bool vst3_transferable_state_equal (const std::string& original, const std::string& current)
{
	for (const char* type : {"Comp", "Cont"}) {
		auto a = vst3_chunk (original, type), b = vst3_chunk (current, type);
		if (a.second != b.second || (a.second && original.compare (a.first, a.second, current, b.first, b.second) != 0)) { return false; }
	}
	return true;
}

typedef std::unique_ptr<GFile, void (*) (gpointer)> FileObject;
FileObject file_object (const std::string& path) { return FileObject (g_file_new_for_path (path.c_str ()), g_object_unref); }

/* Called only on a directory created by this transaction, never an archive
 * pathname or an existing destination. Do not follow filesystem links. */
void remove_owned_tree (GFile* root)
{
	GFileEnumerator* entries = g_file_enumerate_children (root, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
	                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	if (entries) {
		while (GFileInfo* info = g_file_enumerator_next_file (entries, NULL, NULL)) {
			FileObject item (g_file_get_child (root, g_file_info_get_name (info)), g_object_unref);
			if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) { remove_owned_tree (item.get ()); }
			else { g_file_delete (item.get (), NULL, NULL); }
			g_object_unref (info);
		}
		g_object_unref (entries);
	}
	g_file_delete (root, NULL, NULL);
}

struct OwnedDirectory {
	explicit OwnedDirectory (const std::string& parent)
	{
		std::string pattern = Glib::build_filename (parent, ".ardour-exchange-XXXXXX");
		std::vector<char> buffer (pattern.begin (), pattern.end ());
		buffer.push_back (0);
		if (!g_mkdtemp (buffer.data ())) { throw ExchangeError ("Cannot create project staging directory"); }
		path = buffer.data ();
	}
	~OwnedDirectory ()
	{
		if (retained) { return; }
		FileObject root = file_object (path);
		remove_owned_tree (root.get ());
		if (!published.empty ()) { FileObject destination = file_object (published); remove_owned_tree (destination.get ()); }
	}
	void publish (const std::string& destination)
	{
		FileObject from = file_object (Glib::build_filename (path, "session"));
		FileObject to = file_object (destination);
		for (unsigned attempt = 0;; ++attempt) {
			GError* error = NULL;
			if (g_file_move (from.get (), to.get (), G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL, NULL, &error)) { break; }
			bool retry = false;
#ifdef PLATFORM_WINDOWS
			/* A short-lived handle held by a scanner or the just-closed session
			 * can deny a directory rename. Never retry an existing destination. */
			retry = attempt < 40 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) &&
			        g_file_query_exists (from.get (), NULL) && !g_file_query_exists (to.get (), NULL);
#endif
			std::string message = error ? error->message : "unknown error";
			g_clear_error (&error);
			if (!retry) { throw ExchangeError ("Cannot publish project: " + message); }
			g_usleep (50000);
		}
		published = destination;
	}
	std::string path;
	std::string published;
	bool retained = false;
};

samplepos_t samples (double seconds, samplecnt_t rate)
{
	long double value = static_cast<long double> (seconds) * rate;
	/* Ardour's time representation stores superclock in a signed 62-bit value. */
	long double maximum = static_cast<long double> (std::numeric_limits<int64_t>::max () / 4) /
	                      Temporal::superclock_ticks_per_second ();
	if (!std::isfinite (seconds) || seconds < 0 || seconds > maximum || value > std::numeric_limits<samplepos_t>::max ()) {
		throw ExchangeError ("Project time exceeds Ardour's range");
	}
	return static_cast<samplepos_t> (std::llround (value));
}

double beat_value (Temporal::Beats value) { return double (value.to_ticks ()) / Temporal::Beats::PPQN; }

Temporal::Beats beats (double value)
{
	if (!std::isfinite (value) || value < 0 || value >= beat_value (std::numeric_limits<Temporal::Beats>::max ())) { throw ExchangeError ("MIDI time exceeds Ardour's range"); }
	return Temporal::Beats::from_double (value);
}

uint32_t color_value (const std::string& color)
{
	if (color.size () != 7 || color[0] != '#' || color.find_first_not_of ("0123456789abcdefABCDEF", 1) != std::string::npos) {
		throw ExchangeError ("Invalid project color: " + color);
	}
	return (std::stoul (color.substr (1), NULL, 16) << 8) | 0xff;
}

std::string color_text (uint32_t color)
{
	std::ostringstream text;
	text << '#' << std::hex << std::setw (6) << std::setfill ('0') << (color >> 8);
	return text.str ();
}

void populate_tempo (const Project& project, std::vector<Diagnostic>& report)
{
	TimeMap timing (project);
	std::vector<TempoChange> changes = project.tempos;
	bool sampled = false, rounded = false, partial_bar = false;
	for (auto const& change : changes) { if (change.linear && change.position.unit == TimeUnit::Seconds) { sampled = true; } }
	if (sampled) { changes = timing.stepped (0.00001); }
	for (auto& change : changes) { change.position = Time (timing.convert (change.position, TimeUnit::Beats)); }
	if (changes.empty () || changes.front ().position.value > 0) { changes.insert (changes.begin (), {Time (), project.tempo, false}); }
	struct TempoEntry { Temporal::Beats beat; double bpm, end; };
	std::vector<TempoEntry> tempos;
	for (size_t i = 0; i < changes.size ();) {
		size_t last = i;
		while (last + 1 < changes.size () && changes[last + 1].position.value == changes[i].position.value) { ++last; }
		auto const& change = changes[last];
		Temporal::Beats beat = beats (change.position.value);
		rounded |= std::fabs (beat_value (beat) - change.position.value) > 1e-9;
		if (!tempos.empty () && beat <= tempos.back ().beat) { throw ExchangeError ("Tempo changes collide at Ardour's tick resolution"); }
		double end = change.linear && last + 1 < changes.size () ? changes[last + 1].bpm : change.bpm;
		if (60.0 * Temporal::superclock_ticks_per_second () / std::min (change.bpm, end) > std::numeric_limits<int64_t>::max () / 4.0) {
			throw ExchangeError ("Tempo exceeds Ardour's clock range");
		}
		samples (timing.convert (Time (beat_value (beat)), TimeUnit::Seconds), 1);
		tempos.push_back ({beat, change.bpm, end}); i = last + 1;
	}
	struct MeterEntry { Temporal::Beats beat; Temporal::BBT_Time bbt; Temporal::Meter meter; };
	auto meter = [] (unsigned n, unsigned d) {
		if (n > 127 || d > 64) { throw ExchangeError ("Meter exceeds Ardour's supported range"); }
		return Temporal::Meter (n, d);
	};
	std::vector<MeterEntry> meters {{Temporal::Beats (), Temporal::BBT_Time (1, 1, 0), meter (project.numerator, project.denominator)}};
	auto bbt_after = [] (const MeterEntry& entry, Temporal::Beats beat) {
		int64_t ticks = (beat - entry.beat).to_ticks (), grid = entry.meter.ticks_per_grid ();
		int64_t bar = grid * entry.meter.divisions_per_bar ();
		if (ticks < 0 || ticks / bar > INT32_MAX - entry.bbt.bars) { throw ExchangeError ("Meter map exceeds Ardour's bar range"); }
		return entry.meter.bbt_add (entry.bbt, Temporal::BBT_Offset (ticks / bar, (ticks % bar) / grid, ticks % grid));
	};
	for (auto const& change : project.meters) {
		double value = timing.convert (change.position, TimeUnit::Beats);
		Temporal::Beats beat = beats (value);
		samples (timing.convert (Time (beat_value (beat)), TimeUnit::Seconds), 1);
		rounded |= std::fabs (beat_value (beat) - value) > 1e-9;
		if (beat == Temporal::Beats () && value == 0) { meters.front ().meter = meter (change.numerator, change.denominator); continue; }
		if (beat <= meters.back ().beat) { throw ExchangeError ("Meter changes collide at Ardour's tick resolution"); }
		Temporal::BBT_Time bbt = bbt_after (meters.back (), beat);
		if (bbt.beats != 1 || bbt.ticks != 0) { bbt = Temporal::BBT_Time (bbt.bars + 1, 1, 0); partial_bar = true; }
		meters.push_back ({beat, bbt, meter (change.numerator, change.denominator)});
	}
	auto bbt_at = [&] (Temporal::Beats beat) {
		auto end = std::upper_bound (meters.begin (), meters.end (), beat,
		                            [] (Temporal::Beats b, const MeterEntry& m) { return b < m.beat; });
		return bbt_after (*std::prev (end), beat);
	};
	/* Use Ardour's persistence representation: the editor-facing setters round
	 * changes to whole beats/bars, while valid imported maps may use sub-beats
	 * and truncated measures. Construct every timestamp from the same native
	 * TempoPoint integration used for playback, including ramp coefficients. */
	Temporal::TempoMap candidate (Temporal::Tempo (tempos.front ().bpm, 4), meters.front ().meter);
	XMLNode state ("TempoMap");
	state.set_property ("superclocks-per-second", Temporal::superclock_ticks_per_second ());
	XMLNode& tempo_nodes = *state.add_child ("Tempos");
	XMLNode& meter_nodes = *state.add_child ("Meters");
	state.add_child ("MusicTimes");
	std::vector<std::unique_ptr<Temporal::TempoPoint>> points;
	for (size_t i = 0; i < tempos.size (); ++i) {
		auto const& entry = tempos[i];
		Temporal::superclock_t sc = i ? points.back ()->superclock_at (entry.beat) : 0;
		if (sc < 0 || sc > std::numeric_limits<int64_t>::max () / 4) { throw ExchangeError ("Tempo map exceeds Ardour's time range"); }
		std::unique_ptr<Temporal::TempoPoint> point (new Temporal::TempoPoint (candidate,
		        Temporal::Tempo (entry.bpm, entry.end, 4), sc, entry.beat, bbt_at (entry.beat)));
		if (i + 1 < tempos.size ()) { point->compute_omega_from_quarter_duration (tempos[i + 1].beat - entry.beat, point->end_superclocks_per_quarter_note ()); }
		tempo_nodes.add_child_nocopy (point->get_state ()); points.push_back (std::move (point));
	}
	for (auto const& entry : meters) {
		auto end = std::upper_bound (points.begin (), points.end (), entry.beat,
		                            [] (Temporal::Beats beat, const std::unique_ptr<Temporal::TempoPoint>& p) { return beat < p->beats (); });
		Temporal::superclock_t sc = (*std::prev (end))->superclock_at (entry.beat);
		Temporal::MeterPoint point (candidate, entry.meter, sc, entry.beat, entry.bbt);
		meter_nodes.add_child_nocopy (point.get_state ());
	}
	auto map = Temporal::TempoMap::write_copy ();
	try {
		if (map->set_state (state, PBD::Stateful::current_state_version)) { throw ExchangeError ("Cannot construct imported tempo map"); }
	} catch (...) { Temporal::TempoMap::abort_update (); throw; }
	if (Temporal::TempoMap::update (map)) { throw ExchangeError ("Cannot update session tempo map"); }
	if (sampled) { report.push_back ({Disposition::Transformed, "Tempo", "Second-based linear ramps sampled into constant tempos with a 10-microsecond timing-error bound before native tick rounding"}); }
	if (rounded) { report.push_back ({Disposition::Transformed, "Transport", "Tempo/meter positions rounded to Ardour's 1/" + std::to_string (Temporal::Beats::PPQN) + " quarter-note resolution"}); }
	if (partial_bar) { report.push_back ({Disposition::Transformed, "Meter", "A meter change inside a measure starts a new bar at the original musical position"}); }
}

SourceList midi_source (Session& session, const Clip& clip, const TimeMap& timing, std::vector<Diagnostic>& report, const CancelCheck& cancel)
{
	std::string path = session.new_midi_source_path (clip.name.empty () ? "MIDI" : clip.name);
	std::shared_ptr<MidiSource> source = std::dynamic_pointer_cast<MidiSource> (
		SourceFactory::createWritable (DataType::MIDI, session, path, session.sample_rate ()));
	if (!source) { throw ExchangeError ("Cannot create MIDI source"); }
	source->set_allow_remove_if_empty (false);
	struct Event {
		Temporal::Beats time;
		std::array<uint8_t, 3> data;
	};
	std::vector<Event> events;
	double end = timing.content (clip.offset, clip.position, clip.offset, TimeUnit::Beats) + timing.duration (Time (clip.duration, clip.position.unit), clip.position, TimeUnit::Beats);
	for (auto const& note : clip.notes) {
		cancelled (cancel);
		if (note.velocity == 0) { throw ExchangeError ("Zero-velocity notes cannot be preserved in an SMF source"); }
		double start = timing.content (Time (note.time, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
		double stop = timing.content (Time (note.time + note.duration, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
		Temporal::Beats on = beats (start), off = beats (stop);
		if (off <= on) { throw ExchangeError ("MIDI note is shorter than Ardour's timing resolution"); }
		uint8_t velocity = std::max (1L, std::lround (note.velocity * 127));
		uint8_t release = std::lround (note.release_velocity * 127);
		events.push_back ({on, {{static_cast<uint8_t> (0x90 | note.channel), static_cast<uint8_t> (note.key), velocity}}});
		events.push_back ({off, {{static_cast<uint8_t> (0x80 | note.channel), static_cast<uint8_t> (note.key), release}}});
		end = std::max (end, stop);
	}
	for (auto const& control : clip.midi_controls) {
		auto points = control.points;
		auto boundary = std::lower_bound (points.begin (), points.end (), clip.offset.value,
		                                 [] (const AutomationPoint& p, double t) { return p.time < t; });
		if (boundary != points.begin () && (boundary == points.end () || boundary->time != clip.offset.value)) {
			points.insert (boundary, {clip.offset.value, std::prev (boundary)->value, false});
			report.push_back ({Disposition::Transformed, clip.name, "MIDI controller state before the source offset copied to the clip start"});
		}
		Temporal::Beats previous; bool first = true, merged = false;
		for (auto const& point : points) {
			cancelled (cancel);
			double at = timing.content (Time (point.time, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
			auto when = beats (at);
			if (!first && when < previous) { throw ExchangeError ("MIDI controller timeline is reversed after timing conversion"); }
			unsigned value = point.value;
			uint8_t a = control.type == 0xd0 ? value : control.type == 0xe0 ? value & 127 : control.number;
			uint8_t b = control.type == 0xe0 ? value >> 7 : control.type == 0xd0 ? 0 : value;
			Event event = {when, {{static_cast<uint8_t> (control.type | control.channel), a, b}}};
			if (!first && when == previous) { events.back () = event; merged = true; }
			else { events.push_back (event); }
			previous = when; first = false; end = std::max (end, at);
		}
		if (merged) { report.push_back ({Disposition::Transformed, clip.name, "MIDI controller events rounded to the same Ardour tick merged using the last source value for each controller and channel"}); }
	}
	std::stable_sort (events.begin (), events.end (), [] (const Event& a, const Event& b) {
		auto order = [] (uint8_t status) { return (status & 0xf0) == 0x80 ? 0 : (status & 0xf0) == 0x90 ? 2 : 1; };
		return a.time != b.time ? a.time < b.time : order (a.data[0]) < order (b.data[0]);
	});
	std::set<std::pair<uint8_t, uint8_t>> sounding;
	for (auto const& event : events) {
		auto key = std::make_pair (uint8_t (event.data[0] & 15), event.data[1]);
		if ((event.data[0] & 0xf0) == 0x90) {
			if (!sounding.insert (key).second) { throw ExchangeError ("Overlapping notes with the same pitch and channel cannot be preserved in SMF"); }
		} else if ((event.data[0] & 0xf0) == 0x80) { sounding.erase (key); }
	}
	Source::WriterLock lock (source->mutex ());
	source->mark_streaming_write_started (lock);
	source->drop_model (lock);
	for (auto const& event : events) {
		cancelled (cancel);
		source->append_event_beats (lock, Evoral::Event<Temporal::Beats> (Evoral::MIDI_EVENT, event.time, (event.data[0] & 0xf0) == 0xd0 ? 2 : 3, event.data.data ()));
	}
	source->mark_streaming_write_completed (lock, Temporal::timecnt_t (beats (end)));
	source->load_model (lock, true);
	for (auto const& item : source->model ()->controls ()) {
		source->set_interpolation_of (item.first, Evoral::ControlList::Discrete);
		if (item.second->list ()) { item.second->list ()->set_interpolation (Evoral::ControlList::Discrete); }
	}
	return SourceList (1, source);
}

void populate_automation (Session& session, const Automation& curve, std::shared_ptr<AutomationControl> control,
                          const std::string& name, std::vector<Diagnostic>& report)
{
	if (curve.points.empty ()) { return; }
	if (!control || !control->alist ()) { throw ExchangeError (name + ": automation control is unavailable"); }
	bool discrete = std::all_of (curve.points.begin (), std::prev (curve.points.end ()), [] (const AutomationPoint& p) { return !p.linear; });
	bool transformed = false;
	auto position = [&] (double t) { return curve.unit == TimeUnit::Beats ? Temporal::timepos_t (beats (t)) : Temporal::timepos_t (samples (t, session.sample_rate ())); };
	std::vector<std::pair<Temporal::timepos_t, double>> points;
	for (size_t i = 0; i < curve.points.size (); ++i) {
		auto const& p = curve.points[i];
		if (p.value < control->lower () || p.value > control->upper ()) { throw ExchangeError (name + ": automation exceeds Ardour's control range"); }
		auto at = position (p.time);
		double converted = curve.unit == TimeUnit::Beats ? beat_value (at.beats ()) : double (at.samples ()) / session.sample_rate ();
		transformed |= std::fabs (p.time - converted) > 1e-10;
		if (i) {
			if (at <= points.back ().first) { throw ExchangeError (name + ": automation points collide at session resolution"); }
			if (!discrete && !curve.points[i - 1].linear && curve.points[i - 1].value != p.value) {
				auto guard = curve.unit == TimeUnit::Beats ? Temporal::timepos_t (at.beats () - Temporal::Beats (0, 1))
				             : Temporal::timepos_t (at.samples () - 1);
				if (guard > points.back ().first) { points.push_back ({guard, curve.points[i - 1].value}); }
				transformed = true;
			}
		}
		points.push_back ({at, p.value});
	}
	auto list = control->alist ();
	list->clear (); list->set_time_domain (curve.unit == TimeUnit::Beats ? Temporal::BeatTime : Temporal::AudioTime);
	if (!list->set_interpolation (discrete ? Evoral::ControlList::Discrete : Evoral::ControlList::Linear)) { throw ExchangeError ("Cannot set automation interpolation"); }
	list->freeze ();
	for (auto const& p : points) { list->fast_simple_add (p.first, p.second); }
	list->thaw (); control->set_automation_state (Play);
	if (transformed) { report.push_back ({Disposition::Transformed, name, "Automation rounded to session resolution; mixed hold/linear steps use a one-sample or one-tick transition"}); }
}

void populate (Session& session, const Project& project, const std::map<std::string, std::string>& assets,
                std::vector<Diagnostic>& report, const CancelCheck& cancel,
                std::map<std::string, std::string>& route_ids)
{
	session.config.set_native_file_data_format (FormatFloat);
	TimeMap timing (project);
	populate_tempo (project, report);

	std::map<std::string, SourceList> sources;
	for (auto const& media : project.media) {
		cancelled (cancel);
		if (media.channels > 2) { throw ExchangeError ("Multichannel media is not yet supported by session exchange"); }
		PBD::ScopedFileDescriptor fd (g_open (assets.at (media.id).c_str (), O_RDONLY
#ifdef O_BINARY
		                                    | O_BINARY
#endif
		                                    , 0));
		SF_INFO info = {};
		std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> probe (fd < 0 ? NULL : sf_open_fd (fd, SFM_READ, &info, SF_FALSE), sf_close);
		if (!probe) { throw ExchangeError ("Cannot read project audio: " + media.path); }
		if (info.channels <= 0 || info.samplerate <= 0 || static_cast<unsigned> (info.channels) != media.channels ||
		    static_cast<unsigned> (info.samplerate) != media.sample_rate ||
		    std::fabs (media.duration * media.sample_rate - info.frames) > 1.0) {
			throw ExchangeError ("Audio file does not match its project description: " + media.path);
		}
		ImportStatus status;
		status.current = status.total = 1;
		status.freeze = false;
		status.quality = SrcBest;
		status.replace_existing_source = false;
		status.split_midi_channels = false;
		status.import_markers = false;
		status.midi_track_name_source = SMFTrackName;
		status.all_done = false;
		status.paths.push_back (assets.at (media.id));
		session.import_files (status);
		cancelled (cancel);
		if (status.cancel || status.sources.size () != media.channels) { throw ExchangeError ("Failed to import project audio: " + media.path); }
		sources[media.id] = status.sources;
		if (info.samplerate != session.sample_rate ()) {
			report.push_back ({Disposition::Transformed, media.id, "Audio resampled to the session sample rate"});
		}
	}

	std::map<std::string, std::shared_ptr<Route>> routes;
	std::map<std::string, std::shared_ptr<RouteGroup>> folders;
	/* Some exporters list the master last. Rename it before tracks are
	 * created, so their automatic connection names cannot retain "Master". */
	for (auto const& track : project.tracks) {
		if (track.master && session.master_out ()) { session.master_out ()->set_name (track.name); }
	}
	for (auto const& track : project.tracks) {
		if (track.kind == TrackKind::Folder) {
			std::string name = track.name;
			unsigned suffix = 2;
			while (session.route_group_by_name (name)) { name = track.name + " " + std::to_string (suffix++); }
			auto group = session.new_route_group (name);
			session.add_route_group (group);
			group->set_active (false, NULL);
			folders[track.id] = group;
		}
	}
	for (auto const& track : project.tracks) {
		cancelled (cancel);
		if (track.channels > 2) { throw ExchangeError ("Multichannel track routing is not yet supported"); }
		if ((track.kind == TrackKind::Folder || track.kind == TrackKind::Bus) && track.channels != 2) {
			throw ExchangeError ("Only stereo buses and folders are currently supported");
		}
		std::shared_ptr<Route> route;
		std::shared_ptr<RouteGroup> group = track.parent.empty () ? std::shared_ptr<RouteGroup> () : folders.at (track.parent);
		if (track.master) {
			route = session.master_out ();
			if (route) { route->set_name (track.name); }
		} else if (track.kind == TrackKind::Audio) {
			auto list = session.new_audio_track (track.channels, 2, group, 1, track.name, PresentationInfo::max_order, Normal, false);
			if (!list.empty ()) { route = list.front (); }
		} else if (track.kind == TrackKind::MIDI) {
			ChanCount input (DataType::MIDI, 1), output (DataType::MIDI, 1);
			output.set_audio (2);
			auto list = session.new_midi_track (input, output, false, std::shared_ptr<PluginInfo> (), NULL, group,
			                                    1, track.name, PresentationInfo::max_order, Normal, false);
			if (!list.empty ()) { route = list.front (); }
		} else {
			auto list = session.new_audio_route (track.channels, 2, group, 1, track.name, PresentationInfo::AudioBus, PresentationInfo::max_order);
			if (!list.empty ()) { route = list.front (); }
		}
		if (!route) { throw ExchangeError ("Cannot create track: " + track.name); }
		routes[track.id] = route;
		route_ids[track.id] = route->id ().to_s ();
		if (route->name () != track.name) { report.push_back ({Disposition::Transformed, track.name, "Track renamed to " + route->name ()}); }
		if (!track.color.empty ()) { route->presentation_info ().set_color (color_value (track.color)); }
		if (track.volume > route->gain_control ()->upper ()) { throw ExchangeError (track.name + ": gain exceeds Ardour's supported range"); }
		if (track.channels == 2 && track.kind != TrackKind::MIDI) {
			route->panner_shell ()->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
			if (route->panner_shell ()->current_panner_uri () != "http://ardour.org/plugin/panner_balance") {
				throw ExchangeError ("Ardour's stereo balance panner is not available");
			}
		}
		route->gain_control ()->set_value (track.volume, PBD::Controllable::NoGroup);
		route->mute_control ()->set_value (track.mute ? 1 : 0, PBD::Controllable::NoGroup);
		route->solo_control ()->set_value (track.solo ? 1 : 0, PBD::Controllable::NoGroup);
		if (auto pan = route->pan_azimuth_control ()) { pan->set_value (track.pan, PBD::Controllable::NoGroup); }
		populate_automation (session, track.volume_automation, route->gain_control (), track.name + " volume", report);
		populate_automation (session, track.pan_automation, route->pan_azimuth_control (), track.name + " pan", report);
		for (auto const& device : track.plugins) {
			cancelled (cancel);
			validate_vst3_state (device.id, device.state);
			PluginInfoPtr info;
			for (auto const& candidate : PluginManager::instance ().vst3_plugin_info ()) {
				if (canonical_vst3_id (candidate->unique_id) == device.id) { info = candidate; break; }
			}
			if (!info) { throw ExchangeError (track.name + ": VST3 plugin is not installed: " + device.name + " (" + device.id + "); provide a rendered track fallback"); }
			if ((device.role == "instrument" && (!info->is_instrument () || track.kind != TrackKind::MIDI)) ||
			    (device.role == "audioFX" && !info->is_effect ())) {
				throw ExchangeError (track.name + ": VST3 role or track type differs from the installed plugin: " + device.name);
			}
			PluginPtr plugin = info->load (session);
			if (!plugin) { throw ExchangeError (track.name + ": cannot load VST3 plugin " + device.name); }
			auto insert = std::make_shared<PluginInsert> (session, session, plugin);
			if (insert->has_sidechain () || route->add_processor (insert, PreFader)) {
				throw ExchangeError (track.name + ": cannot route imported VST3 plugin " + device.name);
			}
			if (insert->get_count () != 1 || !default_plugin_mapping (*insert) || insert->output_streams ().n_audio () > 2) {
				throw ExchangeError (track.name + ": VST3 plugin needs replicated instances or unsupported channel routing; provide a rendered fallback: " + device.name);
			}
			XMLNode state (plugin->state_node_name ());
			XMLNode* chunk = new XMLNode ("chunk");
			gchar* encoded = g_base64_encode (reinterpret_cast<const guchar*> (device.state.data ()), device.state.size ());
			if (!encoded) { delete chunk; throw ExchangeError ("Cannot encode VST3 preset state"); }
			chunk->add_content (encoded);
			g_free (encoded);
			state.add_child_nocopy (*chunk);
			if (plugin->set_state (state, PBD::Stateful::current_state_version)) {
				throw ExchangeError (track.name + ": cannot apply VST3 preset state for " + device.name);
			}
			if (vst3_chunk (device.state, "Info").first || vst3_chunk (device.state, "Cont").first) {
				/* The plugin API may return only component bytes.  Keep the
				 * original preset wrapper in the session, but reuse it only while
				 * the live component/controller state is unchanged. */
				XMLNode* metadata = new XMLNode ("ProjectExchangeVST3");
				XMLNode* preset = new XMLNode ("Preset");
				gchar* original = g_base64_encode (reinterpret_cast<const guchar*> (device.state.data ()), device.state.size ());
				if (!original) { delete preset; delete metadata; throw ExchangeError ("Cannot retain VST3 preset metadata"); }
				preset->add_content (original);
				g_free (original);
				metadata->add_child_nocopy (*preset);
				insert->add_extra_xml (*metadata);
			}
			if (!device.enabled) { insert->deactivate (); }
			report.push_back ({Disposition::Preserved, device.name, "Matched VST3 class ID and restored embedded preset state"});
		}
	}

	samplepos_t end = 0;
	for (auto const& track : project.tracks) {
		cancelled (cancel);
		auto route = routes.at (track.id);
		for (auto const& curve : {track.volume_automation, track.pan_automation}) {
			if (!curve.points.empty ()) { end = std::max (end, samples (timing.convert (Time (curve.points.back ().time, curve.unit), TimeUnit::Seconds), session.sample_rate ())); }
		}
		XMLNode* metadata = new XMLNode ("ProjectExchange");
		metadata->set_property ("folder", track.kind == TrackKind::Folder);
		if (!track.parent.empty ()) { metadata->set_property ("parent", routes.at (track.parent)->id ().to_s ()); }
		route->add_extra_xml (*metadata);
		if (!track.master) {
			auto target = track.destination.empty () ? session.master_out () : routes.at (track.destination);
			if (!target || target->input ()->n_ports ().n_audio () != 2 || route->output ()->n_ports ().n_audio () != 2 ||
			    route->output ()->connect_ports_to_bundle (target->input ()->bundle (), true, true)) {
				throw ExchangeError ("Cannot connect imported track: " + track.name);
			}
			for (unsigned channel = 0; channel < 2; ++channel) {
				if (!route->output ()->audio (channel)->connected_to (target->input ()->audio (channel).get ())) {
					throw ExchangeError ("Imported audio connection was not established: " + track.name);
				}
			}
		}
		for (auto const& send : track.sends) {
			cancelled (cancel);
			auto target = routes.at (send.destination);
			if (send.volume > route->gain_control ()->upper ()) { throw ExchangeError (track.name + ": send gain exceeds Ardour's range"); }
			session.add_internal_send (target, route->before_processor_for_placement (send.pre_fader ? PreFader : PostFader), route);
			std::shared_ptr<InternalSend> inserted;
			route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
				if (auto item = std::dynamic_pointer_cast<InternalSend> (weak.lock ())) {
					if (item->role () == Delivery::Aux && item->target_route () == target) { inserted = item; }
				}
			});
			if (!inserted || inserted->get_pre_fader () != send.pre_fader) { throw ExchangeError ("Cannot create imported bus send: " + track.name); }
			inserted->gain_control ()->set_value (send.volume, PBD::Controllable::NoGroup);
			auto shell = inserted->panner_shell ();
			if (!shell) { throw ExchangeError ("Imported bus send has no panner"); }
			shell->set_linked_to_route (false);
			shell->select_panner_by_uri ("http://ardour.org/plugin/panner_balance");
			if (shell->current_panner_uri () != "http://ardour.org/plugin/panner_balance") { throw ExchangeError ("Ardour's bus-send balance panner is not available"); }
			if (auto pan = shell->pannable ()->pan_azimuth_control) { pan->set_value (send.pan, PBD::Controllable::NoGroup); }
			else if (send.pan != .5) { throw ExchangeError ("Imported bus send cannot set its pan"); }
			if (!send.enabled) { inserted->deactivate (); }
		}
		for (auto const& clip : track.clips) {
			cancelled (cancel);
			bool midi = clip.kind == ClipKind::MIDI;
			TimeUnit native_unit = midi ? TimeUnit::Beats : TimeUnit::Seconds;
			double position = timing.convert (clip.position, native_unit);
			double duration = timing.duration (Time (clip.duration, clip.position.unit), clip.position, native_unit);
			double offset = timing.content (clip.offset, clip.position, clip.offset, native_unit);
			if (midi) { beats (position + duration); beats (offset + duration); }
			else { samples (position + duration, session.sample_rate ()); samples (offset + duration, session.sample_rate ()); }
			Temporal::timepos_t pos = midi ? Temporal::timepos_t (beats (position)) : Temporal::timepos_t (samples (position, session.sample_rate ()));
			Temporal::timecnt_t length = midi ? Temporal::timecnt_t (beats (duration), pos) : Temporal::timecnt_t (samples (duration, session.sample_rate ()), pos);
			if (!length.is_positive ()) { throw ExchangeError ("Clip is shorter than the session timing resolution"); }
			PBD::PropertyList properties;
			properties.add (Properties::start, midi ? Temporal::timepos_t (beats (offset)) : Temporal::timepos_t (samples (offset, session.sample_rate ())));
			properties.add (Properties::length, length);
			properties.add (Properties::name, clip.name.empty () ? track.name : clip.name);
			properties.add (Properties::whole_file, false);
			properties.add (Properties::opaque, false);
			SourceList source = midi ? midi_source (session, clip, timing, report, cancel) : sources.at (clip.media);
			auto region = RegionFactory::create (source, properties);
			if (!region) { throw ExchangeError ("Cannot create imported region"); }
			region->set_muted (!clip.enabled);
			if (!clip.color.empty ()) {
				XMLNode* color = new XMLNode ("ProjectExchange");
				color->set_property ("color", clip.color);
				region->add_extra_xml (*color);
				report.push_back ({Disposition::Transformed, clip.name, "Clip color retained for exchange; Ardour displays the track color"});
			}
			if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) {
				audio->set_scale_amplitude (clip.gain);
				audio->set_fade_in_active (false);
				audio->set_fade_out_active (false);
				if (!clip.envelope.points.empty ()) {
					auto curve = linearize_automation (slice_automation (clip.envelope, 0, duration), 1.0 / session.sample_rate ());
					auto list = audio->envelope ();
					double peak = 1;
					for (auto const& p : curve.points) { peak = std::max (peak, p.value); }
					double scale = std::max (1.0, peak / list->descriptor ().upper);
					if (!std::isfinite (clip.gain * scale) || clip.gain * scale > std::numeric_limits<float>::max ()) { throw ExchangeError ("Clip envelope amplitude exceeds native range"); }
					audio->set_scale_amplitude (clip.gain * scale);
					list->clear (); list->set_interpolation (Evoral::ControlList::Linear); list->freeze ();
					for (auto const& p : curve.points) { list->fast_simple_add (Temporal::timepos_t (samples (p.time, session.sample_rate ())), p.value / scale); }
					list->thaw (); audio->set_envelope_active (true);
					report.push_back ({Disposition::Transformed, clip.name, "Clip gain/fades use an editable linear-amplitude region envelope at the session sample resolution"});
				}
			}
			auto native_track = std::dynamic_pointer_cast<ARDOUR::Track> (route);
			if (!native_track || !native_track->playlist ()) { throw ExchangeError ("Imported region has no playlist"); }
			native_track->playlist ()->add_region (region, pos);
			end = std::max (end, region->end_position ().samples ());
			if (clip.position.unit != native_unit) {
				report.push_back ({Disposition::Transformed, clip.name, midi ? "MIDI clip anchored in musical time" : "Audio clip anchored in sample time"});
			}
			if (midi) { report.push_back ({Disposition::Transformed, clip.name, "Notes stored at Ardour's MIDI tick resolution with 7-bit velocities"}); }
		}
	}
	if (!folders.empty ()) { report.push_back ({Disposition::Transformed, "Track folders", "Folders represented by buses and route groups; parent relationships retained for exchange"}); }
	for (auto const& marker : project.markers) {
		cancelled (cancel);
		auto pos = marker.position.unit == TimeUnit::Beats ? Temporal::timepos_t (beats (marker.position.value))
		           : Temporal::timepos_t (samples (marker.position.value, session.sample_rate ()));
		auto location = new Location (session, pos, pos, marker.name, Location::IsMark);
		session.locations ()->add (location);
		if (location->name () != marker.name) { report.push_back ({Disposition::Transformed, "Unnamed marker", "Assigned marker name " + location->name ()}); }
		if (location->start ().time_domain () != pos.time_domain ()) {
			report.push_back ({Disposition::Transformed, marker.name, "Marker converted to the session time basis using the tempo map"});
		}
		end = std::max (end, pos.samples ());
	}
	report.push_back ({Disposition::Transformed, "Panning", "Stereo channels use Ardour's balance panner; pan laws can differ between applications"});
	session.set_session_extents (Temporal::timepos_t (0), Temporal::timepos_t (std::max (end, samplepos_t (session.sample_rate ()))));
	if (!ARDOUR::LuaAPI::wait_for_process_callback (2, 2000)) { throw ExchangeError ("Audio engine did not finish applying imported controls"); }
	cancelled (cancel);
}

void unsupported (Project& project, const std::string& object, const std::string& detail)
{
	project.report.push_back ({Disposition::Unsupported, object, detail});
}

void check_control (Project& project, const std::string& object, std::shared_ptr<AutomationControl> control)
{
	if (control && control->alist () && (!control->alist ()->empty () || control->automation_state () != Off)) {
		unsupported (project, object, "Automation is not yet supported by session exchange");
	}
}

Automation collect_automation (Project& project, const std::string& object, std::shared_ptr<AutomationControl> control, samplecnt_t rate)
{
	Automation curve;
	if (!control || !control->alist () || control->alist ()->empty ()) { return curve; }
	if (control->automation_state () == Off) { unsupported (project, object, "Inactive stored automation needs an explicit conversion choice"); return curve; }
	auto list = control->alist ();
	curve.unit = list->time_domain () == Temporal::BeatTime ? TimeUnit::Beats : TimeUnit::Seconds;
	auto style = list->interpolation ();
	if (style == Evoral::ControlList::Curved) { unsupported (project, object, "Spline mixer automation needs explicit conversion"); return curve; }
	bool transformed = false;
	for (auto const& event : *list) {
		double at = curve.unit == TimeUnit::Beats ? beat_value (event->when.beats ()) : double (event->when.samples ()) / rate;
		if (!curve.points.empty () && (style == Evoral::ControlList::Exponential || style == Evoral::ControlList::Logarithmic)) {
			auto before = curve.points.back ();
			if (style == Evoral::ControlList::Logarithmic && (before.value <= 0 || event->value <= 0)) { throw ExchangeError ("Logarithmic automation requires positive values"); }
			append_automation_segment (curve, at, event->value, [&] (double t) {
				double fraction = (t - before.time) / (at - before.time);
				return style == Evoral::ControlList::Exponential ? interpolate_gain (before.value, event->value, fraction, control->upper ())
				       : interpolate_logarithmic (before.value, event->value, fraction, control->lower (), control->upper ());
			});
			transformed = true;
		} else { curve.points.push_back ({at, event->value, style != Evoral::ControlList::Discrete}); }
	}
	if (transformed) { project.report.push_back ({Disposition::Transformed, object, "Native curved automation sampled to linear segments within 0.000101 amplitude before destination rounding"}); }
	if (control->automation_state () != Play) { project.report.push_back ({Disposition::Transformed, object, "Recorded envelope exported as playback automation; write/touch/latch mode is not transferred"}); }
	return curve;
}

Automation region_curve (std::shared_ptr<AutomationList> list, samplecnt_t rate)
{
	// Curve evaluation maintains a lookup cache. Copy the list under its
	// own read lock so export never shares that cache with disk playback.
	list = std::make_shared<AutomationList> (*list);
	Automation result; result.unit = TimeUnit::Seconds;
	if (list->empty ()) { return result; }
	if (list->time_domain () != Temporal::AudioTime) { throw ExchangeError ("Region gain curve must use audio time"); }
	auto style = list->interpolation ();
	list->curve ().solve ();
	size_t evaluations = 0;
	auto value = [&] (samplepos_t at) {
		if (++evaluations > 2000000) { throw ExchangeError ("Region curve exceeds conversion work limit"); }
		float v = 0; list->curve ().get_vector (Temporal::timepos_t (at), Temporal::timepos_t (at), &v, 1);
		if (!std::isfinite (v) || v < -0.0000001) { throw ExchangeError ("Invalid native region gain curve at sample " + std::to_string (at) + ": " + std::to_string (v) + " (interpolation " + std::to_string (int (style)) + ")"); }
		return std::max (0.0, double (v));
	};
	Evoral::ControlEvent* before = nullptr;
	for (auto const& event : *list) {
		samplepos_t end = event->when.samples ();
		if (!before) { result.points.push_back ({double (end) / rate, event->value, style != Evoral::ControlList::Discrete}); }
		else {
			std::function<void (samplepos_t, samplepos_t)> append;
			append = [&] (samplepos_t a, samplepos_t b) {
				double bound = 0;
				if (style == Evoral::ControlList::Curved && event->coeff) {
					double x = Temporal::timepos_t (a).val (), y = Temporal::timepos_t (b).val ();
					// Cubic secant error <= max |f''| * interval^2 / 8.
					bound = std::max (std::fabs (2 * event->coeff[2] + 6 * event->coeff[3] * x),
					                  std::fabs (2 * event->coeff[2] + 6 * event->coeff[3] * y)) * (y - x) * (y - x) / 8;
				}
				if (style == Evoral::ControlList::Exponential || style == Evoral::ControlList::Logarithmic) {
					double va = value (a), vb = value (b);
					auto line = [&] (samplepos_t x) { return va + (vb - va) * double (x - a) / (b - a); };
					std::function<bool (samplepos_t, double, samplepos_t, double)> bounded;
					bounded = [&] (samplepos_t left, double vl, samplepos_t right, double vr) {
						if (right - left <= 1) { return true; }
						if (std::max (std::fabs (vl - line (right)), std::fabs (vr - line (left))) <= .000001) { return true; }
						auto mid = left + (right - left) / 2; double vm = value (mid);
						if (std::fabs (vm - line (mid)) > .0000005) { return false; }
						return bounded (left, vl, mid, vm) && bounded (mid, vm, right, vr);
					};
					if (!bounded (a, va, b, vb)) { bound = 1; }
				}
				if (bound > .000001 && b - a > 1) { auto mid = a + (b - a) / 2; append (a, mid); append (mid, b); return; }
				if (result.points.size () >= 100000) { throw ExchangeError ("Region envelope exceeds conversion limit"); }
				result.points.push_back ({double (b) / rate, b == end ? event->value : value (b), style != Evoral::ControlList::Discrete});
			};
			if (end <= before->when.samples ()) { throw ExchangeError ("Native region curve points collide at sample resolution"); }
			append (before->when.samples (), end);
		}
		before = event;
	}
	return linearize_automation (result, 1.0 / rate);
}

Automation region_envelope (AudioRegion& audio, samplecnt_t rate)
{
	Automation result; result.unit = TimeUnit::Seconds;
	double duration = double (audio.length_samples ()) / rate, quantum = 1.0 / rate;
	if (audio.envelope_active ()) { result = slice_automation (region_curve (audio.envelope (), rate), 0, duration); }
	Automation fades[2] = {{TimeUnit::Seconds, {}}, {TimeUnit::Seconds, {}}}; double lengths[2] = {0, 0};
	for (bool in : {true, false}) {
		if (!(in ? audio.fade_in_active () : audio.fade_out_active ())) { continue; }
		auto list = in ? audio.fade_in () : audio.fade_out ();
		auto fade = region_curve (list, rate);
		if (fade.points.empty ()) { continue; }
		double length = fade.points.back ().time;
		lengths[in ? 0 : 1] = length;
		if (in) {
			if (length > quantum && fade.points.back ().value != 1) {
				float v; auto at = Temporal::timepos_t (samples (length - quantum, rate)); list->curve ().get_vector (at, at, &v, 1);
				while (fade.points.size () > 1 && fade.points.back ().time >= length - quantum - 1e-10) { fade.points.pop_back (); }
				fade.points.push_back ({length - quantum, v, true}); fade.points.push_back ({length, 1, true});
			}
		} else {
			double offset = duration - length;
			for (auto& point : fade.points) { point.time += offset; }
			if (offset > quantum && fade.points.front ().value != 1) { fade.points.insert (fade.points.begin (), {offset - quantum, 1, true}); }
		}
		fades[in ? 0 : 1] = slice_automation (fade, 0, duration);
	}
	Automation combined {TimeUnit::Seconds, {}};
	double overlap_start = duration - lengths[1], overlap_end = lengths[0];
	if (!fades[0].points.empty () && !fades[1].points.empty () && overlap_start < overlap_end) {
		// AudioRegion::read_at mixes both contributions where its own fades
		// overlap. For opaque regions the second contribution also applies
		// the inverse fade-out to the first contribution.
		auto first = fades[0];
		if (audio.opaque ()) {
			Automation inverse {TimeUnit::Seconds, {}};
			if (audio.inverse_fade_out ()) { inverse = region_curve (audio.inverse_fade_out (), rate); }
			else {
				inverse = region_curve (audio.fade_out (), rate);
				for (auto& p : inverse.points) { p.value = 1 - p.value; }
			}
			for (auto& p : inverse.points) { p.time += overlap_start; }
			first = multiply_automation (first, slice_automation (inverse, 0, duration), quantum);
		}
		std::set<double> points;
		for (auto const& curve : {first, fades[0], fades[1]}) { for (auto const& p : curve.points) { points.insert (p.time); } }
		for (double boundary : {overlap_start, overlap_end}) {
			if (boundary > 0 && boundary < duration) { points.insert (boundary); }
			if (boundary > quantum && boundary <= duration) { points.insert (boundary - quantum); }
		}
		combined.unit = TimeUnit::Seconds;
		for (double t : points) {
			double v = t < overlap_start - quantum * .001 ? automation_value (fades[0], t)
			           : t >= overlap_end - quantum * .001 ? automation_value (fades[1], t)
			           : automation_value (first, t) + automation_value (fades[1], t);
			combined.points.push_back ({t, v, true});
		}
	} else { combined = multiply_automation (fades[0], fades[1], quantum); }
	return multiply_automation (result, combined, quantum);
}

/* Only direct, one-to-one stereo connections can be represented by the
 * initial exchange model. Never guess a destination from the first port. */
std::string destination_for (Project& project, const std::shared_ptr<Route>& route, const RouteList& routes)
{
	if (route->is_master ()) { return std::string (); }
	std::vector<std::string> connections[2];
	if (route->output ()->n_ports ().n_audio () != 2) {
		unsupported (project, route->name (), "Only stereo main outputs are supported");
		return std::string ();
	}
	for (unsigned channel = 0; channel < 2; ++channel) {
		route->output ()->audio (channel)->get_connections (connections[channel]);
	}
	for (auto const& target : routes) {
		if (target->input ()->n_ports ().n_audio () != 2) { continue; }
		bool matches = true;
		for (unsigned channel = 0; channel < 2; ++channel) {
			matches = matches && connections[channel].size () == 1 &&
			          route->output ()->audio (channel)->connected_to (target->input ()->audio (channel).get ());
		}
		if (matches) { return "track-" + target->id ().to_s (); }
	}
	unsupported (project, route->name (), "Disconnected, external, split, or crossed output routing cannot yet be exchanged");
	return std::string ();
}

typedef std::map<std::string, SourceList> AudioAssets;

Project collect_timing (Session& session)
{
	Project project;
	project.application = "Ardour";
	auto tempo = Temporal::TempoMap::use ();
	auto const& initial_tempo = tempo->tempo_at (Temporal::Beats ());
	auto const& initial_meter = tempo->meter_at (Temporal::Beats ());
	project.tempo = initial_tempo.quarter_notes_per_minute ();
	project.numerator = initial_meter.divisions_per_bar ();
	project.denominator = initial_meter.note_value ();
	if (!tempo->bartimes ().empty ()) { unsupported (project, "Tempo map", "Music-time anchors cannot yet be exchanged"); }
	if (tempo->n_tempos () != 1 || initial_tempo.ramped ()) {
		for (auto it = tempo->tempos ().begin (); it != tempo->tempos ().end (); ++it) {
			auto next = std::next (it);
			bool ramp = it->actually_ramped () && next != tempo->tempos ().end ();
			project.tempos.push_back ({Time (beat_value (it->beats ())), it->quarter_notes_per_minute (), ramp});
			if (ramp) {
				double end = 60.0 * Temporal::superclock_ticks_per_second () / it->end_superclocks_per_quarter_note ();
				if (std::fabs (end - next->quarter_notes_per_minute ()) > 1e-8) {
					project.tempos.push_back ({Time (beat_value (next->beats ())), end, false});
				}
			}
		}
	}
	if (tempo->n_meters () > 1) {
		for (auto const& point : tempo->meters ()) {
			project.meters.push_back ({Time (beat_value (point.beats ())), unsigned (point.divisions_per_bar ()), unsigned (point.note_value ())});
		}
	}
	for (auto const& location : session.locations ()->list ()) {
		if (!location->is_session_range () && !location->is_auto_loop () && !location->is_auto_punch ()) {
			if (location->flags () != Location::IsMark || !location->cd_info.empty ()) {
				unsupported (project, location->name (), "Ranges and markers with special playback or CD semantics cannot yet be exchanged");
			} else {
				auto pos = location->start ();
				Time time = pos.time_domain () == Temporal::BeatTime ? Time (beat_value (pos.beats ()), TimeUnit::Beats)
				            : Time (double (pos.samples ()) / session.sample_rate (), TimeUnit::Seconds);
				project.markers.push_back ({location->name (), time});
			}
		}
	}
	if (!project.markers.empty () && std::any_of (project.markers.begin (), project.markers.end (), [&] (const Marker& marker) {
		return marker.position.unit != project.markers.front ().position.unit;
	})) {
		project.report.push_back ({Disposition::Transformed, "Markers", "Mixed musical and sample-time markers exported in seconds using the tempo map"});
	}
	return project;
}

Project collect_rendered (Session& session, const SessionExchange::ExportOptions& options)
{
	if (options.render_mix && options.render_stems) { throw ExchangeError ("Choose either one mix or separate stems"); }
	if (!std::isfinite (options.render_tail_seconds) || options.render_tail_seconds < 0 || options.render_tail_seconds > 600) {
		throw ExchangeError ("Rendered audio tail must be between zero and 600 seconds");
	}
	samplecnt_t end = session.current_end_sample ();
	if (end <= 0) { throw ExchangeError ("Set the session end before rendering the mix"); }
	samplecnt_t tail = samples (options.render_tail_seconds, session.sample_rate ());
	if (end > std::numeric_limits<samplecnt_t>::max () - tail) { throw ExchangeError ("Rendered audio range exceeds the sample limit"); }
	samplecnt_t length = end + tail;
	if (uint64_t (length) > 0xffff0000ULL / (2 * sizeof (float))) { throw ExchangeError ("Rendered mix exceeds the supported WAV file size; shorten the session range"); }
	Project project = collect_timing (session);
	Track master;
	master.id = "master";
	master.name = "Master";
	master.kind = TrackKind::Bus;
	master.master = true;
	project.tracks.push_back (master);
	if (!session.master_out ()) { throw ExchangeError ("Rendered project export requires a master bus"); }
	std::vector<std::shared_ptr<Route>> routes = options.render_stems ? master_stem_routes (session) : std::vector<std::shared_ptr<Route>> {session.master_out ()};
	if (uint64_t (length) > (ArchiveLimits ().total_bytes - 16 * 1024 * 1024) / (2 * sizeof (float) * routes.size ())) {
		throw ExchangeError ("Rendered stems exceed the exchange package size limit; shorten the range or use one stereo mix");
	}
	for (auto const& route : routes) {
		Track mix;
		mix.id = options.render_stems ? "render-track-" + route->id ().to_s () : "mix-track";
		mix.name = options.render_stems ? route->name () : session.name () + " Mix";
		mix.destination = master.id;
		if (options.render_stems && route->presentation_info ().color_set ()) { mix.color = color_text (route->presentation_info ().color ()); }
		Media media;
		media.id = options.render_stems ? "rendered-" + route->id ().to_s () : "rendered-mix";
		media.path = "audio/" + media.id + ".wav";
		media.sample_rate = session.sample_rate (); media.channels = 2; media.duration = double (length) / media.sample_rate;
		Clip clip; clip.name = mix.name; clip.position = Time (0, TimeUnit::Seconds); clip.offset = Time (0, TimeUnit::Seconds);
		clip.duration = media.duration; clip.media = media.id;
		mix.clips.push_back (clip); project.media.push_back (media); project.tracks.push_back (mix);
	}
	project.report.push_back ({Disposition::Rendered, options.render_stems ? "Separate stems" : "Current master mix", options.render_stems ?
		"Each stereo track or bus feeding the master becomes a separate audio track. Upstream tracks and shared processing are combined inside their bus stem. Effects, instruments, automation, mute and solo are baked into the audio. Master processing must be neutral; MIDI notes and processors are no longer editable." :
		"All audible tracks, MIDI instruments, effects, automation and bus routing become one stereo audio clip. Individual tracks, notes and processors are no longer editable; current mute and solo states are included."});

	std::ostringstream range;
	range << "Rendered from timeline zero to session end (" << double (end) / session.sample_rate ()
		<< " seconds), plus " << options.render_tail_seconds
		<< " extra seconds for tails. Timeline content inside the extended range is included too; audio beyond it is excluded. Tempo, meter and ordinary markers remain editable.";
	project.report.push_back ({Disposition::Transformed, "Render range", range.str ()});
	validate_project (project);
	return project;
}

std::map<std::string, std::string> render_audio (Session& session, OwnedDirectory& stage, const Project& project,
                                              const SessionExchange::ExportOptions& options, const CancelCheck& cancel)
{
	std::map<std::string, std::string> files;
	try {
		auto length = samples (project.media.front ().duration, session.sample_rate ());
		if (options.render_stems) {
			for (auto const& stem : render_session_stems (session, stage.path, length, cancel)) { files["rendered-" + stem.route_id] = stem.path; }
		} else { files["rendered-mix"] = render_session_mix (session, stage.path, length, cancel).path; }
	} catch (RenderShutdownFailed const&) { stage.retained = true; throw; }
	if (files.size () != project.media.size ()) { throw ExchangeError ("Render routes changed during project export"); }
	for (auto const& media : project.media) { if (!files.count (media.id)) { throw ExchangeError ("A rendered route changed during project export"); } }
	return files;
}

/* Use the native playback iterator on independent controller lists, so export
 * does not share or alter the live model's interpolation/search caches. */
class ControllerSequence : public Evoral::Sequence<Temporal::Beats> {
public:
	explicit ControllerSequence (const Evoral::TypeMap& map) : Evoral::Sequence<Temporal::Beats> (map) {}
	std::shared_ptr<Evoral::Control> control_factory (const Evoral::Parameter& parameter) override {
		auto const& desc = type_map ().descriptor (parameter);
		auto list = std::make_shared<Evoral::ControlList> (parameter, desc, Temporal::TimeDomainProvider (Temporal::BeatTime));
		return std::make_shared<Evoral::Control> (parameter, desc, list);
	}
};

Project collect (Session& session, AudioAssets& assets, const CancelCheck& cancel)
{
	Project project = collect_timing (session);
	RouteList routes (*session.get_routes ());
	routes.remove_if ([] (const std::shared_ptr<Route>& route) { return route->is_auditioner () || route->is_monitor (); });
	routes.sort ([] (const std::shared_ptr<Route>& a, const std::shared_ptr<Route>& b) {
		return a->presentation_info ().order () < b->presentation_info ().order ();
	});
	std::map<std::vector<std::string>, std::string> media_ids;
	for (auto const& route : routes) {
		cancelled (cancel);
		Track track;
		track.id = "track-" + route->id ().to_s ();
		track.name = route->name ();
		track.master = route->is_master ();
		track.channels = route->input ()->n_ports ().n_audio ();
		track.volume = route->gain_control ()->get_value ();
		track.mute = route->muted_by_self ();
		track.solo = route->self_soloed ();
		if (route->presentation_info ().color_set ()) { track.color = color_text (route->presentation_info ().color ()); }
		if (auto pan = route->pan_azimuth_control ()) { track.pan = pan->get_value (); }
		auto native_track = std::dynamic_pointer_cast<ARDOUR::Track> (route);
		auto midi_track = std::dynamic_pointer_cast<MidiTrack> (route);
		track.kind = midi_track ? TrackKind::MIDI : native_track ? TrackKind::Audio : TrackKind::Bus;
		if (midi_track) { track.channels = 2; }
		if (const XMLNode* metadata = route->extra_xml ("ProjectExchange")) {
			bool folder = false;
			metadata->get_property ("folder", folder);
			if (folder && !native_track && !track.master) { track.kind = TrackKind::Folder; }
			std::string parent;
			if (metadata->get_property ("parent", parent)) { track.parent = "track-" + parent; }
		}
		track.destination = destination_for (project, route, routes);
		if (!route->active ()) { unsupported (project, track.name, "Inactive tracks cannot yet be exchanged"); }
		track.volume_automation = collect_automation (project, track.name + " gain", route->gain_control (), session.sample_rate ());
		track.pan_automation = collect_automation (project, track.name + " pan", route->pan_azimuth_control (), session.sample_rate ());
		check_control (project, track.name + " width", route->pan_width_control ());
		check_control (project, track.name + " mute", route->mute_control ());
		check_control (project, track.name + " trim", route->trim_control ());
		if (route->trim_control ()->get_value () != 1 || route->polarity ()->phase_control ()->any ()) {
			unsupported (project, track.name, "Trim gain and polarity changes cannot yet be exchanged");
		}
		std::string panner = route->panner_shell ()->current_panner_uri ();
		if (route->panner_shell ()->bypassed ()) { unsupported (project, track.name, "Bypassed panning cannot yet be exchanged"); }
		if (panner == "http://ardour.org/plugin/panner_2in2out") {
			if (std::fabs (route->pan_width_control ()->get_value () - 1) > 1e-6 || std::fabs (track.pan - 0.5) > 1e-6 || !track.pan_automation.points.empty ()) {
				unsupported (project, track.name, "Stereo position and width panning cannot yet be exchanged; use a balance panner");
			}
		} else if (!panner.empty () && panner != "http://ardour.org/plugin/panner_balance" && panner != "http://ardour.org/plugin/panner_1in2out") {
			unsupported (project, track.name, "Unsupported panner");
		}
		bool after_fader = false, after_send = false;
		route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			auto processor = weak.lock ();
			if (!processor) { return; }
			if (processor == route->amp ()) { after_fader = true; }
			if (auto send = std::dynamic_pointer_cast<InternalSend> (processor)) {
				after_send = true;
				if (send->role () != Delivery::Aux || !send->target_route () || send->target_route ()->is_master () ||
				    std::find (routes.begin (), routes.end (), send->target_route ()) == routes.end ()) {
					unsupported (project, track.name, "Unsupported internal send destination or role");
					return;
				}
				auto shell = send->panner_shell ();
				if (!shell || shell->bypassed () ||
				    (shell->current_panner_uri () != "http://ardour.org/plugin/panner_balance" &&
				     shell->current_panner_uri () != "http://ardour.org/plugin/panner_1in2out")) {
					unsupported (project, track.name, "Unsupported bus-send panner"); return;
				}
				if (!shell->pannable ()) { unsupported (project, track.name, "Bus send has no pan controls"); return; }
				Send item;
				item.destination = "track-" + send->target_route ()->id ().to_s ();
				item.pre_fader = send->get_pre_fader ();
				item.enabled = send->active ();
				item.volume = send->gain_control ()->get_value ();
				if (auto pan = shell->pannable ()->pan_azimuth_control) {
					item.pan = pan->get_value ();
					check_control (project, track.name + " send pan", pan);
				}
				check_control (project, track.name + " send gain", send->gain_control ());
				track.sends.push_back (item);
				return;
			}
			if (auto box = std::dynamic_pointer_cast<TriggerBox> (processor)) {
				if (!box->empty ()) { unsupported (project, track.name, "Cue clips cannot yet be exchanged"); }
				return;
			}
			if (auto insert = std::dynamic_pointer_cast<PluginInsert> (processor)) {
				if (after_fader || after_send) {
					unsupported (project, track.name, "VST3 plugins must precede the fader and sends; render processing first: " + processor->display_name ());
					return;
				}
				auto plugin = insert->plugin (0);
				auto info = plugin ? plugin->get_info () : PluginInfoPtr ();
				/* Ardour's stereo master uses strict I/O by default. The pin-map
				 * check below still rejects any nonstandard signal routing. */
				if (!info || info->type != VST3 || insert->get_count () != 1 || insert->has_sidechain () || insert->custom_cfg () ||
				    (insert->strict_io () && !route->is_master ())) {
					unsupported (project, track.name, "Plugin configuration requires a compatible VST3 instance or rendered fallback: " + processor->display_name ());
					return;
				}
				if (!default_plugin_mapping (*insert)) {
					unsupported (project, track.name, "Custom VST3 pin routing requires a rendered fallback: " + processor->display_name ());
					return;
				}
				for (auto const& control : insert->controls ()) {
					check_control (project, processor->display_name (), std::dynamic_pointer_cast<AutomationControl> (control.second));
				}
				std::unique_ptr<XMLNode> state (&plugin->get_state ());
				auto chunks = state->children ("chunk");
				if (chunks.size () != 1 || chunks.front ()->children ().size () != 1 || !chunks.front ()->children ().front ()->is_content ()) {
					unsupported (project, track.name, "VST3 plugin has no transferable preset state: " + processor->display_name ());
					return;
				}
				std::string encoded = chunks.front ()->children ().front ()->content ();
				if (encoded.size () > 90 * 1024 * 1024) { unsupported (project, track.name, "VST3 preset state exceeds the exchange size limit"); return; }
				gsize size = 0;
				guchar* decoded = g_base64_decode (encoded.c_str (), &size);
				if (!decoded) { unsupported (project, track.name, "Cannot decode VST3 preset state"); return; }
				PluginDevice device;
				device.id = canonical_vst3_id (info->unique_id);
				device.name = info->name;
				device.role = track.kind == TrackKind::MIDI && info->is_instrument () ? "instrument" : "audioFX";
				device.enabled = insert->active ();
				device.state_path = "plugins/state-" + std::to_string (project.tracks.size ()) + "-" + std::to_string (track.plugins.size ()) + ".vstpreset";
				device.state.assign (reinterpret_cast<const char*> (decoded), size);
				g_free (decoded);
				try { validate_vst3_state (device.id, device.state); }
				catch (const ExchangeError& e) { unsupported (project, track.name, std::string ("VST3 preset needs a rendered fallback: ") + e.what ()); return; }
				if (const XMLNode* metadata = insert->extra_xml ("ProjectExchangeVST3")) {
					bool retained = false;
					auto presets = metadata->children ("Preset");
					if (presets.size () == 1 && presets.front ()->children ().size () == 1 && presets.front ()->children ().front ()->is_content ()) {
						std::string original_encoded = presets.front ()->children ().front ()->content ();
						if (original_encoded.size () <= 90 * 1024 * 1024) {
							gsize original_size = 0;
							guchar* original_bytes = g_base64_decode (original_encoded.c_str (), &original_size);
							if (original_bytes) {
								std::string original (reinterpret_cast<const char*> (original_bytes), original_size);
								g_free (original_bytes);
								try {
									validate_vst3_state (device.id, original);
									if (vst3_transferable_state_equal (original, device.state)) {
										device.state = std::move (original);
										retained = true;
									}
								} catch (const ExchangeError&) { /* Session metadata may be damaged; live plugin state remains valid. */ }
							}
						}
					}
					if (!retained) {
						project.report.push_back ({Disposition::Transformed, device.name,
						                           "VST3 preset wrapper metadata differs from the current plugin state; exported current component/controller state"});
					}
				}
				track.plugins.push_back (std::move (device));
				return;
			}
			std::unique_ptr<XMLNode> state (&processor->get_state ());
			std::string type;
			state->get_property ("type", type);
			/* "delay" is Ardour's automatic route latency compensation. */
			static const std::set<std::string> basic = {"amp", "trim", "meter", "polarity", "main-outs", "diskreader", "diskwriter", "intreturn", "delay"};
			if (!basic.count (type)) { unsupported (project, track.name, "Processor cannot yet be exchanged: " + processor->display_name ()); }
		});
		if (midi_track) {
			check_control (project, track.name + " velocity", midi_track->velocity_control ());
			if (midi_track->get_playback_channel_mode () != AllChannels || midi_track->velocity_control ()->get_value () != midi_track->velocity_control ()->normal () || midi_track->note_mode () != Sustained) {
				unsupported (project, track.name, "MIDI channel, velocity, or percussive playback changes cannot yet be exchanged");
			}
		}
		if (native_track && native_track->playlist ()) {
			auto regions = native_track->playlist ()->region_list ();
			for (auto const& region : *regions) {
				cancelled (cancel);
				Clip clip;
				clip.name = region->name ();
				clip.enabled = !region->muted ();
				if (const XMLNode* metadata = region->extra_xml ("ProjectExchange")) { metadata->get_property ("color", clip.color); }
				if (region->stretch () != 1 || region->shift () != 1) {
					project.report.push_back ({Disposition::Rendered, clip.name, "Previously rendered stretch or pitch is carried in its audio source"});
				}
				if (auto audio = std::dynamic_pointer_cast<AudioRegion> (region)) {
					clip.kind = ClipKind::Audio;
					clip.position = Time (double (region->position_sample ()) / session.sample_rate (), TimeUnit::Seconds);
					clip.duration = double (region->length_samples ()) / session.sample_rate ();
					clip.offset = Time (double (region->start_sample ()) / session.sample_rate (), TimeUnit::Seconds);
					clip.gain = audio->scale_amplitude ();
					if (audio->envelope_active () || audio->fade_in_active () || audio->fade_out_active ()) {
						if (!session.config.get_use_region_fades () && (audio->fade_in_active () || audio->fade_out_active ())) {
							unsupported (project, clip.name, "Globally disabled region fades need an explicit conversion choice");
						}
						try { clip.envelope = region_envelope (*audio, session.sample_rate ()); }
						catch (const ExchangeError& e) { throw ExchangeError (clip.name + ": " + e.what ()); }
						project.report.push_back ({Disposition::Transformed, clip.name, "Region envelope and fade shapes combined into editable gain points, sampled at audio resolution"});
						if (audio->fade_in_active () && audio->fade_out_active () && audio->fade_in ()->when (false).samples () + audio->fade_out ()->when (false).samples () > audio->length_samples ()) {
							project.report.push_back ({Disposition::Transformed, clip.name, "Overlapping native fades retain their mixing behavior; abrupt overlap edges use a one-sample gain transition"});
						}
					}
					if (audio->has_region_fx ()) { unsupported (project, clip.name, "Region effects require rendering or compatible plugin transfer"); }
					if (region->opaque ()) {
						for (auto const& other : *regions) {
							if (other != region && other->position () < region->end_position () && region->position () < other->end_position ()) {
								unsupported (project, clip.name, "Layered overlapping audio regions cannot yet be exchanged");
								break;
							}
						}
					}
					std::vector<std::string> key;
					for (auto const& source : region->sources ()) { key.push_back (source->id ().to_s ()); }
					auto found = media_ids.find (key);
					if (found != media_ids.end ()) { clip.media = found->second; }
					else {
						Media media;
						media.id = "media-" + std::to_string (project.media.size ());
						media.path = "audio/" + media.id + ".wav";
						media.sample_rate = session.sample_rate ();
						media.channels = region->sources ().size ();
						media.duration = double (region->source ()->length ().samples ()) / media.sample_rate;
						for (auto const& source : region->sources ()) {
							auto audio_source = std::dynamic_pointer_cast<AudioFileSource> (source);
							if (!audio_source || audio_source->sample_rate () != media.sample_rate || source->length () != region->source ()->length ()) {
								throw ExchangeError (clip.name + ": incompatible audio source channels");
							}
						}
						clip.media = media.id;
						media_ids[key] = media.id;
						assets[media.id] = region->sources ();
						project.media.push_back (media);
					}
				} else if (auto midi = std::dynamic_pointer_cast<MidiRegion> (region)) {
					clip.kind = ClipKind::MIDI;
					clip.position = Time (beat_value (region->position ().beats ()), TimeUnit::Beats);
					clip.duration = beat_value (region->length ().beats ());
					clip.offset = Time (beat_value (region->start ().beats ()), TimeUnit::Beats);
					auto model = midi->model ();
					if (!model) { throw ExchangeError (clip.name + ": missing MIDI model"); }
					auto lock = model->read_lock ();
					std::set<Evoral::Parameter> parameters;
					model->what_has_data (parameters);
					if (!model->sysexes ().empty () || !model->patch_changes ().empty ()) {
						unsupported (project, clip.name, "MIDI patch changes and system messages cannot yet be exchanged");
					}
					ControllerSequence sampled (model->type_map ());
					using Target = std::tuple<unsigned, unsigned, unsigned>;
					std::map<Target, size_t> sampled_lanes;
					size_t source_points = 0, output_points = 0;
					for (auto const& parameter : parameters) {
						MidiControl control; control.type = parameter_midi_type (static_cast<AutomationType> (parameter.type ()));
						control.channel = parameter.channel (); control.number = parameter.id ();
						if (!supported_midi_control (control.type, control.number)) { unsupported (project, clip.name, "Ordered MIDI bank/data-entry/channel-mode commands cannot yet be exchanged"); continue; }
						if (control.channel >= 16) { throw ExchangeError (clip.name + ": invalid MIDI controller channel"); }
						if (midi->midi_source ()->automation_state_of (parameter) != Play) { unsupported (project, clip.name, "Disabled MIDI controller playback cannot yet be exchanged"); continue; }
						auto native = model->control (parameter); auto list = native ? native->list () : nullptr;
						if (!list || list->empty ()) { continue; }
						if (list->time_domain () != Temporal::BeatTime) { unsupported (project, clip.name, "MIDI controller source must use beat time"); continue; }
						for (auto const& point : *list) {
							cancelled (cancel);
							if (++source_points > 100000 || point->when.beats ().to_ticks () < 0 || !std::isfinite (point->value) || point->value < 0 || point->value > (control.type == 0xe0 ? 16383 : 127)) {
								throw ExchangeError (clip.name + ": invalid or excessive MIDI controller source data");
							}
						}
						if (list->interpolation () != Evoral::ControlList::Discrete) {
							sampled.add_control (std::make_shared<Evoral::Control> (parameter, model->type_map ().descriptor (parameter), std::make_shared<Evoral::ControlList> (*list)));
							sampled_lanes.emplace (Target (control.type, control.channel, control.number), clip.midi_controls.size ());
						} else {
							for (auto const& point : *list) { control.points.push_back ({beat_value (point->when.beats ()), point->value, false}); ++output_points; }
						}
						clip.midi_controls.push_back (std::move (control));
					}
					if (!sampled_lanes.empty ()) {
						const auto start = region->start ().beats ();
						const auto end = start + region->length ().beats ();
						for (auto it = sampled.begin (start); it != sampled.end () && it->time () < end; ++it) {
							cancelled (cancel);
							if (++output_points > 100000) { throw ExchangeError (clip.name + ": sampled MIDI controllers exceed the event limit; shorten the clip or render audio"); }
							if (it->size () < 2) { throw ExchangeError ("Invalid sampled MIDI event"); }
							const uint8_t* data = it->buffer ();
							unsigned type = data[0] & 0xf0, channel = data[0] & 15;
							if ((type != 0xb0 && type != 0xa0 && type != 0xd0 && type != 0xe0) || it->size () != (type == 0xd0 ? 2 : 3)) { throw ExchangeError ("Invalid sampled MIDI event"); }
							unsigned number = type == 0xb0 || type == 0xa0 ? data[1] : 0;
							unsigned value = type == 0xe0 ? data[1] | (unsigned (data[2]) << 7) : type == 0xd0 ? data[1] : data[2];
							auto lane = sampled_lanes.find (Target (type, channel, number));
							if (lane == sampled_lanes.end ()) { throw ExchangeError ("Unexpected sampled MIDI target"); }
							auto& points = clip.midi_controls[lane->second].points;
							double time = beat_value (it->time ());
							if (it->time () < start || (!points.empty () && time <= points.back ().time)) { throw ExchangeError ("Non-increasing sampled MIDI timeline"); }
							points.push_back ({time, double (value), false});
						}
						project.report.push_back ({Disposition::Transformed, clip.name, "Continuous MIDI curves converted to discrete events using Ardour playback timing and values; curve data outside the clip is omitted"});
					}
					for (auto const& event : model->notes ()) {
						Note note;
						note.time = beat_value (event->time ());
						note.duration = beat_value (event->length ());
						note.key = event->note ();
						note.channel = event->channel ();
						note.velocity = double (event->velocity ()) / 127;
						note.release_velocity = double (event->off_velocity ()) / 127;
						clip.notes.push_back (note);
					}
				} else { throw ExchangeError (clip.name + ": unsupported region type"); }
				track.clips.push_back (clip);
			}
		}
		project.tracks.push_back (track);
	}
	validate_project (project);
	return project;
}

void write_audio (const SourceList& sources, const std::string& path, const CancelCheck& cancel, bool pcm24 = false)
{
	SF_INFO info = {};
	info.channels = sources.size ();
	info.samplerate = std::dynamic_pointer_cast<AudioSource> (sources.front ())->sample_rate ();
	samplecnt_t frames = sources.front ()->length ().samples ();
	uint64_t bytes = static_cast<uint64_t> (frames) * info.channels * (pcm24 ? 3 : sizeof (float));
	if (pcm24 && bytes > 0xffff0000ULL) { throw ExchangeError ("AAF source exceeds the prepared WAV size limit"); }
	info.format = (bytes > 0xffff0000ULL ? SF_FORMAT_RF64 : SF_FORMAT_WAV) | (pcm24 ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT);
	int fd = g_open (path.c_str (), O_WRONLY | O_CREAT | O_EXCL
#ifdef O_BINARY
	                 | O_BINARY
#endif
	                 , 0600);
	if (fd < 0) { throw ExchangeError ("Cannot create export audio file"); }
	std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> output (sf_open_fd (fd, SFM_WRITE, &info, SF_TRUE), sf_close);
	if (!output) { close (fd); throw ExchangeError ("Cannot open export audio file"); }
	const samplecnt_t block = 8192;
	std::vector<Sample> channel_data (block), interleaved (block * info.channels);
	for (samplepos_t position = 0; position < frames;) {
		cancelled (cancel);
		samplecnt_t count = std::min (block, frames - position);
		for (int channel = 0; channel < info.channels; ++channel) {
			auto source = std::dynamic_pointer_cast<AudioSource> (sources[channel]);
			if (source->read (channel_data.data (), position, count) != count) { throw ExchangeError ("Cannot read complete audio source"); }
			if (pcm24 && std::any_of (channel_data.begin (), channel_data.begin () + count, [] (Sample sample) {
				return !std::isfinite (sample) || sample < -1 || sample > 1;
			})) { throw ExchangeError ("Audio exceeds 24-bit PCM range; render or reduce its source level before AAF export"); }
			for (samplecnt_t i = 0; i < count; ++i) { interleaved[i * info.channels + channel] = channel_data[i]; }
		}
		if (sf_writef_float (output.get (), interleaved.data (), count) != count) { throw ExchangeError ("Cannot write complete export audio"); }
		position += count;
	}
	if (sf_close (output.release ())) { throw ExchangeError ("Cannot finish export audio file"); }
}

/* AAF embeds integer PCM. Convert the rendered float file without registering
 * new Sources or Regions in the user's session, and reject clipping. */
void write_rendered_pcm24 (const std::string& source, const std::string& path, const CancelCheck& cancel)
{
	SF_INFO info = {};
	PBD::ScopedFileDescriptor input_fd (g_open (source.c_str (), O_RDONLY
#ifdef O_BINARY
	                                          | O_BINARY
#endif
	                                          , 0));
	std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> input (input_fd >= 0 ? sf_open_fd (input_fd, SFM_READ, &info, SF_FALSE) : nullptr, sf_close);
	if (!input || info.frames <= 0 || info.channels != 2 || uint64_t (info.frames) > 0xffff0000ULL / (3 * info.channels)) {
		throw ExchangeError ("Cannot convert rendered audio to the supported AAF PCM format");
	}
	SF_INFO output_info = info;
	output_info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
	int fd = g_open (path.c_str (), O_WRONLY | O_CREAT | O_EXCL
#ifdef O_BINARY
	                 | O_BINARY
#endif
	                 , 0600);
	if (fd < 0) { throw ExchangeError ("Cannot create rendered AAF audio file"); }
	std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> output (sf_open_fd (fd, SFM_WRITE, &output_info, SF_TRUE), sf_close);
	if (!output) { close (fd); throw ExchangeError ("Cannot open rendered AAF audio file"); }
	std::vector<float> data (8192 * info.channels);
	for (sf_count_t position = 0; position < info.frames;) {
		cancelled (cancel);
		sf_count_t count = std::min (sf_count_t (8192), info.frames - position);
		if (sf_readf_float (input.get (), data.data (), count) != count) { throw ExchangeError ("Cannot read complete rendered audio"); }
		if (std::any_of (data.begin (), data.begin () + count * info.channels, [] (float value) {
			return !std::isfinite (value) || value < -1 || value > 1;
		})) { throw ExchangeError ("Rendered mix exceeds 24-bit PCM range; reduce the master level before AAF export"); }
		if (sf_writef_float (output.get (), data.data (), count) != count) { throw ExchangeError ("Cannot write complete rendered AAF audio"); }
		position += count;
	}
	if (sf_close (output.release ())) { throw ExchangeError ("Cannot finish rendered AAF audio"); }
}

void run_logic_writer (const std::string& manifest, const std::string& destination, const CancelCheck& cancel)
{
	std::string base = Glib::build_filename (ardour_dll_directory (), "project-exchange");
	const char* python_override = g_getenv ("ARDOUR_LOGIC_PYTHON");
	const char* helper_override = g_getenv ("ARDOUR_LOGIC_HELPER");
	std::string helper = helper_override ? helper_override : Glib::build_filename (base, "aaf_writer.py");
#ifdef PLATFORM_WINDOWS
	std::string python = python_override ? python_override : Glib::build_filename (base, "python", "pythonw.exe");
#else
	std::string python = python_override ? python_override : Glib::build_filename (base, "python", "bin", "python3");
#endif
	if (!Glib::path_is_absolute (python) || !Glib::file_test (python, Glib::FILE_TEST_IS_REGULAR) ||
	    !Glib::path_is_absolute (helper) || !Glib::file_test (helper, Glib::FILE_TEST_IS_REGULAR)) {
		throw ExchangeError ("The Logic exchange writer is missing from this installation");
	}
	const char* arguments[] = {python.c_str (), "-I", "-B", "-X", "utf8", helper.c_str (), manifest.c_str (), destination.c_str (), nullptr};
	GError* error = nullptr;
	GSubprocess* child = g_subprocess_newv (arguments, GSubprocessFlags (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
	if (!child) {
		std::string message = error ? error->message : "unknown error"; g_clear_error (&error);
		throw ExchangeError ("Cannot start Logic exchange writer: " + message);
	}
	std::unique_ptr<GSubprocess, void (*) (gpointer)> process (child, g_object_unref);
	std::atomic<bool> done (false);
	bool success = false;
	std::string detail;
	std::thread waiter;
	try {
		waiter = std::thread ([&] {
			gchar* errors = nullptr;
			GError* failure = nullptr;
			bool communicated = g_subprocess_communicate_utf8 (child, nullptr, nullptr, nullptr, &errors, &failure);
			success = communicated && g_subprocess_get_successful (child);
			if (failure) { detail = failure->message; }
			else if (errors) { detail.assign (errors, std::min (size_t (16384), strlen (errors))); }
			g_free (errors); g_clear_error (&failure);
			done.store (true);
		});
		while (!done.load ()) { cancelled (cancel); g_usleep (10000); }
		waiter.join ();
		cancelled (cancel);
	} catch (...) {
		if (!done.load ()) { g_subprocess_force_exit (child); }
		if (waiter.joinable ()) { waiter.join (); }
		else { g_subprocess_wait (child, nullptr, nullptr); }
		throw;
	}
	if (!success) { throw ExchangeError ("Logic exchange writer failed: " + detail); }
	for (const char* name : {"Project.aaf", "Project.mid", "Metadata.xml"}) {
		FileObject file = file_object (Glib::build_filename (destination, name));
		GFileInfo* info = g_file_query_info (file.get (), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
		                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
		bool valid = info && g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR && g_file_info_get_size (info) > 0;
		if (info) { g_object_unref (info); }
		if (!valid) { throw ExchangeError (std::string ("Logic exchange writer did not produce ") + name); }
	}
}

std::unique_ptr<Session>
import_session (AudioEngine& engine, const Project& project, const std::string& destination,
                const std::string& snapshot, std::vector<Diagnostic>& report, const CancelCheck& cancel,
                const std::function<void (const Media&, const std::string&)>& copy_media,
                const std::function<void (Session&)>& finish = std::function<void (Session&)> (),
                bool allow_unsupported = false)
{
	if (!engine.running () || engine.session ()) { throw ExchangeError ("Import requires a running engine with no session attached"); }
	if (!Glib::path_is_absolute (destination) || snapshot.empty () || snapshot == "." || snapshot == ".." ||
	    snapshot.find_first_of ("/\\:;<>\"|?*") != std::string::npos || !Session::session_name_is_legal (snapshot).empty ()) {
		throw ExchangeError ("Invalid import destination or session name");
	}
	FileObject final = file_object (destination);
	if (g_file_query_exists (final.get (), NULL)) { throw ExchangeError ("Import destination already exists"); }
	report = project.report;
	for (auto const& item : report) {
		if (!allow_unsupported && item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); }
	}
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::map<std::string, std::string> assets;
	for (size_t i = 0; i < project.media.size (); ++i) {
		std::string path = Glib::build_filename (stage.path, "input-" + std::to_string (i) + ".media");
		copy_media (project.media[i], path);
		assets[project.media[i].id] = path;
	}
	std::unique_ptr<Session> session;
	std::map<std::string, std::string> route_ids;
	try {
		BusProfile profile;
		profile.master_out_channels = 2;
		/* Ardour recognizes a new session by the absence of its directory. */
		session.reset (new Session (engine, Glib::build_filename (stage.path, "session"), snapshot, &profile, "", true));
		engine.set_session (session.get ());
		populate (*session, project, assets, report, cancel, route_ids);
		if (finish) { finish (*session); }
		/* Suppress recent-session registration while staging, but publish a
		 * named session so saving or closing it does not prompt for a rename. */
		session->end_unnamed_status ();
		if (session->unnamed ()) { throw ExchangeError ("Cannot finalize imported session name"); }
		if (session->save_state (snapshot)) { throw ExchangeError ("Cannot save imported session"); }
		engine.remove_session ();
		session.reset ();
		for (auto const& asset : assets) {
			if (g_unlink (asset.second.c_str ())) { throw ExchangeError ("Cannot remove staged source media"); }
		}
		cancelled (cancel);
		stage.publish (destination);
		session.reset (new Session (engine, destination, snapshot, NULL, "", true));
		engine.set_session (session.get ());
		cancelled (cancel);
		for (auto const& track : project.tracks) {
			auto routes = session->get_routes ();
			auto match = std::find_if (routes->begin (), routes->end (), [&] (const std::shared_ptr<Route>& route) {
				return route->id ().to_s () == route_ids.at (track.id);
			});
			if (match == routes->end ()) { throw ExchangeError ("Imported track did not survive save/reopen: " + track.name); }
			if ((*match)->name () != track.name) {
				std::string detail = "Track renamed to " + (*match)->name ();
				if (std::none_of (report.begin (), report.end (), [&] (const Diagnostic& item) {
					return item.disposition == Disposition::Transformed && item.object == track.name && item.detail == detail;
				})) { report.push_back ({Disposition::Transformed, track.name, detail}); }
			}
		}
		store_recent_sessions (session->name (), session->path ());
		stage.published.clear ();
		return session;
	} catch (...) {
		if (session && engine.session () == session.get ()) { engine.remove_session (); }
		session.reset ();
		throw;
	}
}

void copy_native_audio (const std::string& source, const std::string& destination, const CancelCheck& cancel)
{
	int binary = 0;
#ifdef O_BINARY
	binary = O_BINARY;
#endif
	PBD::ScopedFileDescriptor input (g_open (source.c_str (), O_RDONLY | binary, 0));
	PBD::ScopedFileDescriptor output (g_open (destination.c_str (), O_WRONLY | O_CREAT | O_EXCL | binary, 0600));
	if (input < 0 || output < 0) { throw ExchangeError ("Cannot copy native song audio"); }
	std::vector<char> buffer (1024 * 1024);
	uint64_t total = 0;
	for (;;) {
		cancelled (cancel);
		ssize_t count = ::read (input, buffer.data (), buffer.size ());
		if (count < 0 && errno == EINTR) { continue; }
		if (count < 0) { throw ExchangeError ("Cannot read native song audio"); }
		if (!count) { break; }
		total += count;
		if (total > ArchiveLimits ().entry_bytes) { throw ExchangeError ("Native audio exceeds import size limit"); }
		for (ssize_t offset = 0; offset < count;) {
			ssize_t written = ::write (output, buffer.data () + offset, count - offset);
			if (written < 0 && errno == EINTR) { continue; }
			if (written <= 0) { throw ExchangeError ("Cannot write native song audio"); }
			offset += written;
		}
	}
}

} // namespace

std::unique_ptr<Session>
SessionExchange::import_logic (AudioEngine& engine, const std::string& aaf, const std::string& midi,
                              const std::string& destination, const std::string& snapshot,
                              std::vector<Diagnostic>& report, CancelCheck cancel)
{
	LogicProject project (aaf, midi, cancel);
	return import_session (engine, project.project (), destination, snapshot, report, cancel,
	                       [&] (const Media& media, const std::string& path) { project.copy_media (media, path); });
}

std::unique_ptr<Session>
SessionExchange::import_dawproject (AudioEngine& engine, const std::string& archive_path, const std::string& destination,
                                   const std::string& snapshot, std::vector<Diagnostic>& report, CancelCheck cancel)
{
	return import_dawproject (engine, archive_path, destination, snapshot, report, cancel, false);
}

std::unique_ptr<Session>
SessionExchange::import_dawproject (AudioEngine& engine, const std::string& archive_path, const std::string& destination,
                                   const std::string& snapshot, std::vector<Diagnostic>& report, CancelCheck cancel,
                                   bool allow_unsupported)
{
	Project project = DawProject::read (archive_path, cancel);
	ProjectArchive archive (archive_path, ArchiveLimits (), cancel);
	return import_session (engine, project, destination, snapshot, report, cancel,
	                       [&] (const Media& media, const std::string& path) { archive.copy_file (media.path, path); },
	                       std::function<void (Session&)> (), allow_unsupported);
}

std::unique_ptr<Session>
SessionExchange::import_studio_one (AudioEngine& engine, const std::string& filename, const std::string& destination,
	                                const std::string& snapshot, std::vector<Diagnostic>& report, CancelCheck cancel,
	                                bool allow_unsupported)
{
	auto song = StudioOneSong::read (filename, cancel);
	TimeMap timing (song.project);
	return import_session (engine, song.project, destination, snapshot, report, cancel,
	                       [&] (const Media& media, const std::string& path) { copy_native_audio (song.audio_files.at (media.id), path, cancel); },
	                       [&] (Session& session) {
		                       auto position = [&] (Time t) { return Temporal::timepos_t (samples (timing.convert (t, TimeUnit::Seconds), session.sample_rate ())); };
		                       if (song.has_range) { session.set_session_extents (position (song.start), position (song.end)); }
	                       }, allow_unsupported);
}

std::vector<Diagnostic>
SessionExchange::export_dawproject (Session& session, const std::string& destination, CancelCheck cancel)
{
	std::vector<Diagnostic> report;
	export_dawproject (session, destination, report, cancel);
	return report;
}

void
SessionExchange::export_dawproject (Session& session, const std::string& destination, std::vector<Diagnostic>& report, CancelCheck cancel)
{
	export_dawproject (session, destination, report, ExportOptions (), cancel);
}

void
SessionExchange::export_dawproject (Session& session, const std::string& destination, std::vector<Diagnostic>& report, const ExportOptions& options, CancelCheck cancel)
{
	report.clear ();
	if (session.transport_rolling () || session.actively_recording ()) { throw ExchangeError ("Stop playback and recording before exporting a project"); }
	if (!Glib::path_is_absolute (destination)) { throw ExchangeError ("Export destination must be absolute"); }
	FileObject target = file_object (destination);
	if (g_file_query_exists (target.get (), NULL)) { throw ExchangeError ("Export destination already exists"); }
	cancelled (cancel);
	AudioAssets assets;
	Project project = options.rendered () ? collect_rendered (session, options) : collect (session, assets, cancel);
	report = project.report;
	for (auto const& item : project.report) {
		if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); }
	}
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::map<std::string, std::string> files;
	if (options.rendered ()) { files = render_audio (session, stage, project, options, cancel); }
	for (auto const& media : project.media) {
		if (files.count (media.id)) { continue; }
		std::string path = Glib::build_filename (stage.path, media.id + ".wav");
		write_audio (assets.at (media.id), path, cancel);
		files[media.id] = path;
	}
	DawProject::write (project, destination, files, cancel);
}

void
SessionExchange::export_logic (Session& session, const std::string& destination, std::vector<Diagnostic>& report, CancelCheck cancel)
{
	export_logic (session, destination, report, ExportOptions (), cancel);
}

void
SessionExchange::export_logic (Session& session, const std::string& destination, std::vector<Diagnostic>& report, const ExportOptions& options, CancelCheck cancel)
{
	report.clear ();
	if (session.transport_rolling () || session.actively_recording ()) { throw ExchangeError ("Stop playback and recording before exporting a project"); }
	if (!Glib::path_is_absolute (destination)) { throw ExchangeError ("Export destination must be absolute"); }
	FileObject target = file_object (destination);
	if (g_file_query_exists (target.get (), NULL)) { throw ExchangeError ("Export destination already exists"); }
	cancelled (cancel);
	AudioAssets assets;
	Project project = options.rendered () ? collect_rendered (session, options) : collect (session, assets, cancel);
	std::string master;
	for (auto const& track : project.tracks) { if (track.master) { master = track.id; } }
	for (auto const& track : project.tracks) {
		if (track.master) {
			if (!track.plugins.empty ()) { unsupported (project, track.name, "AAF/MIDI cannot preserve master plugins; render processing first"); }
			if (track.volume != 1 || track.mute || track.solo || track.pan != .5 || !track.volume_automation.points.empty () || !track.pan_automation.points.empty ()) {
				unsupported (project, track.name, "Master mixing cannot yet be represented in AAF/MIDI");
			}
			continue;
		}
		if (track.kind == TrackKind::Bus || track.kind == TrackKind::Folder || !track.parent.empty () || track.destination != master) {
			unsupported (project, track.name, "AAF/MIDI export currently requires tracks connected directly to the master, without folders or buses");
		}
		if (!track.sends.empty ()) { unsupported (project, track.name, "AAF/MIDI export cannot preserve bus sends; render or remove them first"); }
		if (!track.plugins.empty ()) { unsupported (project, track.name, "AAF/MIDI cannot preserve VST3 plugin state; render processing first"); }
		if (track.solo) { unsupported (project, track.name, "Clear solo before AAF/MIDI export"); }
		if (track.kind == TrackKind::Audio && track.channels == 2 && (track.pan != .5 || !track.pan_automation.points.empty ())) {
			project.report.push_back ({Disposition::Transformed, track.name, "Stereo balance becomes independent AAF left/right channel levels; moving curves are sampled to audio resolution"});
		}
		if (track.kind == TrackKind::MIDI && (track.volume != 1 || track.pan != .5 || track.mute || !track.volume_automation.points.empty () || !track.pan_automation.points.empty ())) {
			unsupported (project, track.name, "MIDI track mixer settings cannot yet be represented in Standard MIDI Files");
		}
		if (track.mute && !track.volume_automation.points.empty ()) { unsupported (project, track.name, "Unmute an automated track before AAF export to retain its editable gain envelope"); }
		for (auto const& clip : track.clips) {
			if (clip.kind == ClipKind::MIDI && !clip.enabled) { unsupported (project, clip.name, "Muted MIDI clips cannot yet be represented in Standard MIDI Files"); }
		}
	}
	report = project.report;
	for (auto const& item : report) { if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); } }
	validate_project (project);
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::map<std::string, std::string> rendered;
	if (options.rendered ()) { rendered = render_audio (session, stage, project, options, cancel); }
	std::string output = Glib::build_filename (stage.path, "session");
	if (g_mkdir (output.c_str (), 0700)) { throw ExchangeError ("Cannot create AAF/MIDI export staging directory"); }
	XMLTree manifest;
	XMLNode& root = *manifest.set_root (new XMLNode ("LogicExchange"));
	root.set_property ("version", 2);
	root.set_property ("name", session.name ());
	root.set_property ("rate", session.sample_rate ());
	root.set_property ("tempo", project.tempo);
	root.set_property ("numerator", project.numerator);
	root.set_property ("denominator", project.denominator);
	TimeMap timing (project);
	if (!project.tempos.empty ()) {
		for (auto const& change : timing.stepped (0.00001)) {
			XMLNode& item = *root.add_child ("Tempo");
			item.set_property ("time", change.position.value);
			item.set_property ("bpm", change.bpm);
		}
		report.push_back ({Disposition::Transformed, "Tempo", "MIDI tempos rounded to whole microseconds per quarter note; ramps sampled with a 10-microsecond timing-error bound before MIDI rounding"});
	}
	for (auto const& change : project.meters) {
		XMLNode& item = *root.add_child ("TimeSignature");
		item.set_property ("time", timing.convert (change.position, TimeUnit::Beats));
		item.set_property ("numerator", change.numerator);
		item.set_property ("denominator", change.denominator);
	}
	double duration = 0;
	for (auto const& media : project.media) {
		cancelled (cancel);
		std::string path = Glib::build_filename (stage.path, media.id + ".wav");
		if (options.rendered ()) { write_rendered_pcm24 (rendered.at (media.id), path, cancel); }
		else { write_audio (assets.at (media.id), path, cancel, true); }
		XMLNode& source = *root.add_child ("Media");
		source.set_property ("id", media.id);
		source.set_property ("path", path);
		source.set_property ("channels", media.channels);
		source.set_property ("frames", options.rendered () ? samples (media.duration, session.sample_rate ()) : assets.at (media.id).front ()->length ().samples ());
	}
	for (auto const& track : project.tracks) {
		if (track.master) { continue; }
		cancelled (cancel);
		bool audio = track.kind == TrackKind::Audio;
		XMLNode& lane = *root.add_child (audio ? "AudioTrack" : "MidiTrack");
		lane.set_property ("name", track.name);
		lane.set_property ("color", track.color);
		lane.set_property ("channels", track.channels);
		lane.set_property ("gain", track.mute ? 0.0 : track.volume);
		lane.set_property ("pan", track.pan);
		for (bool pan : {false, true}) {
			auto const& original = pan ? track.pan_automation : track.volume_automation;
			if (original.points.empty ()) { continue; }
			auto curve = convert_automation (original, timing, TimeUnit::Seconds);
			XMLNode& envelope = *lane.add_child (pan ? "PanAutomation" : "GainAutomation");
			for (auto const& p : curve.points) {
				XMLNode& point = *envelope.add_child ("Point");
				point.set_property ("time", p.time); point.set_property ("value", p.value);
				point.set_property ("linear", p.linear ? "true" : "false");
			}
			duration = std::max (duration, curve.points.back ().time);
			report.push_back ({Disposition::Transformed, track.name, "AAF automation uses seconds, rational values and sample-rounded positions; mixed hold/linear steps use a one-sample transition"});
		}
		if (track.mute) { report.push_back ({Disposition::Transformed, track.name, "Muted audio track exported with zero track gain"}); }
		for (auto const& clip : track.clips) {
			XMLNode& item = *lane.add_child ("Clip");
			item.set_property ("name", clip.name);
			item.set_property ("color", clip.color);
			item.set_property ("enabled", clip.enabled ? "true" : "false");
			item.set_property ("gain", clip.gain);
			if (!clip.envelope.points.empty ()) {
				XMLNode& envelope = *item.add_child ("GainAutomation");
				for (auto const& p : clip.envelope.points) {
					XMLNode& point = *envelope.add_child ("Point");
					point.set_property ("time", p.time); point.set_property ("value", p.value); point.set_property ("linear", p.linear ? "true" : "false");
				}
			}
			TimeUnit unit = audio ? TimeUnit::Seconds : TimeUnit::Beats;
			double position = timing.convert (clip.position, unit);
			double length = timing.duration (Time (clip.duration, clip.position.unit), clip.position, unit);
			double offset = timing.content (clip.offset, clip.position, clip.offset, unit);
			duration = std::max (duration, timing.convert (Time (position + length, unit), TimeUnit::Seconds));
			if (audio) {
				item.set_property ("time", samples (position, session.sample_rate ()));
				item.set_property ("length", samples (length, session.sample_rate ()));
				item.set_property ("offset", samples (offset, session.sample_rate ()));
				item.set_property ("media", clip.media);
				if (!clip.enabled) { report.push_back ({Disposition::Transformed, clip.name, "Muted audio clip exported with zero clip gain"}); }
			} else {
				item.set_property ("time", position);
				item.set_property ("length", length);
				item.set_property ("offset", offset);
				for (auto const& note : clip.notes) {
					XMLNode& event = *item.add_child ("Note");
					double start = timing.content (Time (note.time, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
					double stop = timing.content (Time (note.time + note.duration, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
					event.set_property ("time", start);
					event.set_property ("length", stop - start);
					event.set_property ("key", note.key);
					event.set_property ("channel", note.channel);
					event.set_property ("velocity", int (std::max (1.0, std::round (note.velocity * 127))));
					event.set_property ("release", int (std::round (note.release_velocity * 127)));
				}
				for (auto const& control : clip.midi_controls) {
					XMLNode& lane = *item.add_child ("MidiControl");
					lane.set_property ("type", control.type); lane.set_property ("channel", control.channel); lane.set_property ("number", control.number);
					for (auto const& point : control.points) {
						XMLNode& event = *lane.add_child ("Point");
						event.set_property ("time", timing.content (Time (point.time, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats));
						event.set_property ("value", int (point.value));
					}
				}
			}
		}
	}
	for (auto const& marker : project.markers) {
		XMLNode& item = *root.add_child ("Marker");
		item.set_property ("name", marker.name);
		item.set_property ("time", timing.convert (marker.position, TimeUnit::Beats));
		double seconds = timing.convert (marker.position, TimeUnit::Seconds);
		item.set_property ("seconds", seconds);
		duration = std::max (duration, seconds);
	}
	root.set_property ("duration", duration);
	root.set_property ("durationBeats", timing.convert (Time (duration, TimeUnit::Seconds), TimeUnit::Beats));
	std::string path = Glib::build_filename (stage.path, "manifest.xml");
	if (!manifest.write (path)) { throw ExchangeError ("Cannot write AAF/MIDI export manifest"); }
	run_logic_writer (path, output, cancel);
	report.push_back ({Disposition::Transformed, "Audio", "Embedded 24-bit PCM; floating-point sources are quantized to 24 bits"});
	if (!options.rendered ()) { report.push_back ({Disposition::Transformed, "MIDI", "Clips merged per track and events rounded to 1/19200 beat; notes trimmed and retriggered at the left edge; held MIDI controller values copied to the clip start"}); }
	if (!options.rendered ()) { report.push_back ({Disposition::Transformed, "Metadata", "Original clip structure and colors retained in Metadata.xml; target applications may not display this metadata"}); }
	report.push_back ({Disposition::Transformed, "Timeline", "AAF and MIDI start at zero, with 25 fps zero timecode; load both files at that origin"});
	report.push_back ({Disposition::Transformed, "Mixing", "Gain and pan use AAF rational values; target panning laws can differ"});
	cancelled (cancel);
	stage.publish (destination);
	stage.published.clear ();
}

void
SessionExchange::export_studio_one (Session& session, const std::string& destination, std::vector<Diagnostic>& report, const ExportOptions& options, CancelCheck cancel)
{
	report.clear ();
	if (session.transport_rolling () || session.actively_recording ()) { throw ExchangeError ("Stop playback and recording before exporting a project"); }
	if (!Glib::path_is_absolute (destination)) { throw ExchangeError ("Export destination must be absolute"); }
	FileObject target = file_object (destination);
	if (g_file_query_exists (target.get (), NULL)) { throw ExchangeError ("Export destination already exists"); }
	cancelled (cancel); AudioAssets assets; StudioOneSongData song;
	song.project = options.rendered () ? collect_rendered (session, options) : collect (session, assets, cancel);
	song.title = session.name (); song.sample_rate = session.sample_rate ();
	song.has_range = true;
	song.start = Time (options.rendered () ? 0 : double (session.current_start_sample ()) / session.sample_rate (), TimeUnit::Seconds);
	song.end = Time (options.rendered () ? song.project.media.front ().duration : double (session.current_end_sample ()) / session.sample_rate (), TimeUnit::Seconds);
	report = StudioOneSong::export_report (song.project);
	for (auto const& item : report) { if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); } }
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::string output = Glib::build_filename (stage.path, "session"), media_directory = Glib::build_filename (output, "Media");
	if (g_mkdir (output.c_str (), 0700) || g_mkdir (media_directory.c_str (), 0700)) { throw ExchangeError ("Cannot create native song staging folder"); }
	std::map<std::string, std::string> rendered;
	if (options.rendered ()) { rendered = render_audio (session, stage, song.project, options, cancel); }
	for (size_t i = 0; i < song.project.media.size (); ++i) {
		cancelled (cancel); auto const& media = song.project.media[i];
		std::string path = Glib::build_filename (media_directory, options.render_mix ? "mix.wav" : "source-" + std::to_string (i + 1) + ".wav");
		if (options.rendered ()) {
			if (g_rename (rendered.at (media.id).c_str (), path.c_str ())) { throw ExchangeError ("Cannot collect rendered native song audio"); }
		} else { write_audio (assets.at (media.id), path, cancel); }
		song.audio_files[media.id] = path;
	}

	std::string filename = Glib::build_filename (output, "Project.song");
	StudioOneSong::write (song, filename, Glib::build_filename (destination, "Project.song"), cancel);
	/* Enforce the reader's structural limits before publishing a native folder. */
	auto verified = StudioOneSong::read (filename, cancel);
	if (has_unsupported_features (verified.project)) { throw ExchangeError ("The generated native song contains unsupported data"); }
	cancelled (cancel); stage.publish (destination); stage.published.clear ();
}

} }
