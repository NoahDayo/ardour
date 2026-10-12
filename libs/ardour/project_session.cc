/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_session.h"
#include "ardour/studio_one_song.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sndfile.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>

#include "pbd/controllable.h"
#include "pbd/scoped_file_descriptor.h"
#include "pbd/xml++.h"
#include "temporal/tempo.h"
#include "evoral/Event.h"
#include "ardour/audioengine.h"
#include "ardour/audioregion.h"
#include "ardour/audio_track.h"
#include "ardour/audio_port.h"
#include "ardour/audiofilesource.h"
#include "ardour/automation_list.h"
#include "ardour/import_status.h"
#include "ardour/io.h"
#include "ardour/lua_api.h"
#include "ardour/midi_region.h"
#include "ardour/midi_model.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/playlist.h"
#include "ardour/panner_shell.h"
#include "ardour/phase_control.h"
#include "ardour/polarity_processor.h"
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
		FileObject root = file_object (path);
		remove_owned_tree (root.get ());
		if (!published.empty ()) { FileObject destination = file_object (published); remove_owned_tree (destination.get ()); }
	}
	void publish (const std::string& destination)
	{
		FileObject from = file_object (Glib::build_filename (path, "session"));
		FileObject to = file_object (destination);
		GError* error = NULL;
		if (!g_file_move (from.get (), to.get (), G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL, NULL, &error)) {
			std::string message = error ? error->message : "unknown error";
			g_clear_error (&error);
			throw ExchangeError ("Cannot publish imported session: " + message);
		}
		published = destination;
	}
	std::string path;
	std::string published;
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

SourceList midi_source (Session& session, const Clip& clip, double tempo, const CancelCheck& cancel)
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
	double end = convert_time (clip.offset, TimeUnit::Beats, tempo) + convert_time (Time (clip.duration, clip.position.unit), TimeUnit::Beats, tempo);
	for (auto const& note : clip.notes) {
		cancelled (cancel);
		if (note.velocity == 0) { throw ExchangeError ("Zero-velocity notes cannot be preserved in an SMF source"); }
		double start = convert_time (Time (note.time, clip.offset.unit), TimeUnit::Beats, tempo);
		double stop = convert_time (Time (note.time + note.duration, clip.offset.unit), TimeUnit::Beats, tempo);
		Temporal::Beats on = beats (start), off = beats (stop);
		if (off <= on) { throw ExchangeError ("MIDI note is shorter than Ardour's timing resolution"); }
		uint8_t velocity = std::max (1L, std::lround (note.velocity * 127));
		uint8_t release = std::lround (note.release_velocity * 127);
		events.push_back ({on, {{static_cast<uint8_t> (0x90 | note.channel), static_cast<uint8_t> (note.key), velocity}}});
		events.push_back ({off, {{static_cast<uint8_t> (0x80 | note.channel), static_cast<uint8_t> (note.key), release}}});
		end = std::max (end, stop);
	}
	std::stable_sort (events.begin (), events.end (), [] (const Event& a, const Event& b) {
		return a.time != b.time ? a.time < b.time : a.data[0] < b.data[0];
	});
	std::set<std::pair<uint8_t, uint8_t>> sounding;
	for (auto const& event : events) {
		auto key = std::make_pair (uint8_t (event.data[0] & 15), event.data[1]);
		if ((event.data[0] & 0xf0) == 0x90) {
			if (!sounding.insert (key).second) { throw ExchangeError ("Overlapping notes with the same pitch and channel cannot be preserved in SMF"); }
		} else { sounding.erase (key); }
	}
	Source::WriterLock lock (source->mutex ());
	source->mark_streaming_write_started (lock);
	source->drop_model (lock);
	for (auto const& event : events) {
		cancelled (cancel);
		source->append_event_beats (lock, Evoral::Event<Temporal::Beats> (Evoral::MIDI_EVENT, event.time, 3, event.data.data ()));
	}
	source->mark_streaming_write_completed (lock, Temporal::timecnt_t (beats (end)));
	source->load_model (lock, true);
	return SourceList (1, source);
}

void populate (Session& session, const Project& project, const std::map<std::string, std::string>& assets,
               std::vector<Diagnostic>& report, const CancelCheck& cancel)
{
	if (project.numerator > 127 || project.denominator > 64) { throw ExchangeError ("Meter exceeds Ardour's supported range"); }
	session.config.set_native_file_data_format (FormatFloat);
	auto map = Temporal::TempoMap::write_copy ();
	map->set_tempo (Temporal::Tempo (project.tempo, 4), Temporal::timepos_t (Temporal::Beats ()));
	map->set_meter (Temporal::Meter (project.numerator, project.denominator), Temporal::timepos_t (Temporal::Beats ()));
	if (Temporal::TempoMap::update (map)) { throw ExchangeError ("Cannot update session tempo"); }

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
	}

	samplepos_t end = 0;
	for (auto const& track : project.tracks) {
		cancelled (cancel);
		auto route = routes.at (track.id);
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
		for (auto const& clip : track.clips) {
			cancelled (cancel);
			bool midi = clip.kind == ClipKind::MIDI;
			TimeUnit native_unit = midi ? TimeUnit::Beats : TimeUnit::Seconds;
			double position = convert_time (clip.position, native_unit, project.tempo);
			double duration = convert_time (Time (clip.duration, clip.position.unit), native_unit, project.tempo);
			double offset = convert_time (clip.offset, native_unit, project.tempo);
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
			SourceList source = midi ? midi_source (session, clip, project.tempo, cancel) : sources.at (clip.media);
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
				audio->set_fade_in_active (false);
				audio->set_fade_out_active (false);
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
			report.push_back ({Disposition::Transformed, marker.name, "Marker converted to the session time basis at the current constant tempo"});
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

Project collect (Session& session, AudioAssets& assets, const CancelCheck& cancel)
{
	Project project;
	project.application = "Ardour";
	auto tempo = Temporal::TempoMap::use ();
	auto const& initial_tempo = tempo->tempo_at (Temporal::Beats ());
	auto const& initial_meter = tempo->meter_at (Temporal::Beats ());
	project.tempo = initial_tempo.quarter_notes_per_minute ();
	project.numerator = initial_meter.divisions_per_bar ();
	project.denominator = initial_meter.note_value ();
	if (tempo->n_tempos () != 1 || tempo->n_meters () != 1 || initial_tempo.ramped ()) {
		unsupported (project, "Tempo map", "Variable tempo and meter cannot yet be exchanged");
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
		project.report.push_back ({Disposition::Transformed, "Markers", "Mixed musical and sample-time markers exported in seconds at the current constant tempo"});
	}
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
		check_control (project, track.name + " gain", route->gain_control ());
		check_control (project, track.name + " pan", route->pan_azimuth_control ());
		check_control (project, track.name + " width", route->pan_width_control ());
		check_control (project, track.name + " mute", route->mute_control ());
		check_control (project, track.name + " trim", route->trim_control ());
		if (route->trim_control ()->get_value () != 1 || route->polarity ()->phase_control ()->any ()) {
			unsupported (project, track.name, "Trim gain and polarity changes cannot yet be exchanged");
		}
		std::string panner = route->panner_shell ()->current_panner_uri ();
		if (route->panner_shell ()->bypassed ()) { unsupported (project, track.name, "Bypassed panning cannot yet be exchanged"); }
		if (panner == "http://ardour.org/plugin/panner_2in2out") {
			if (std::fabs (route->pan_width_control ()->get_value () - 1) > 1e-6 || std::fabs (track.pan - 0.5) > 1e-6) {
				unsupported (project, track.name, "Stereo position and width panning cannot yet be exchanged; use a balance panner");
			}
		} else if (!panner.empty () && panner != "http://ardour.org/plugin/panner_balance" && panner != "http://ardour.org/plugin/panner_1in2out") {
			unsupported (project, track.name, "Unsupported panner");
		}
		route->foreach_processor ([&] (std::weak_ptr<Processor> weak) {
			auto processor = weak.lock ();
			if (!processor) { return; }
			if (auto box = std::dynamic_pointer_cast<TriggerBox> (processor)) {
				if (!box->empty ()) { unsupported (project, track.name, "Cue clips cannot yet be exchanged"); }
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
					if (audio->scale_amplitude () != 1 || audio->envelope_active () || audio->fade_in_active () || audio->fade_out_active ()) {
						unsupported (project, clip.name, "Region gain, envelopes, and fades cannot yet be exchanged");
					}
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
					if (!parameters.empty () || !model->sysexes ().empty () || !model->patch_changes ().empty ()) {
						unsupported (project, clip.name, "MIDI controllers, patch changes, and system messages cannot yet be exchanged");
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

void write_audio (const SourceList& sources, const std::string& path, const CancelCheck& cancel)
{
	SF_INFO info = {};
	info.channels = sources.size ();
	info.samplerate = std::dynamic_pointer_cast<AudioSource> (sources.front ())->sample_rate ();
	samplecnt_t frames = sources.front ()->length ().samples ();
	info.format = (static_cast<uint64_t> (frames) * info.channels * sizeof (float) > 0xffff0000ULL ? SF_FORMAT_RF64 : SF_FORMAT_WAV) | SF_FORMAT_FLOAT;
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
			for (samplecnt_t i = 0; i < count; ++i) { interleaved[i * info.channels + channel] = channel_data[i]; }
		}
		if (sf_writef_float (output.get (), interleaved.data (), count) != count) { throw ExchangeError ("Cannot write complete export audio"); }
		position += count;
	}
	if (sf_close (output.release ())) { throw ExchangeError ("Cannot finish export audio file"); }
}

std::unique_ptr<Session>
import_session (AudioEngine& engine, const Project& project, const std::string& destination,
                const std::string& snapshot, std::vector<Diagnostic>& report, const CancelCheck& cancel,
                const std::function<void (const Media&, const std::string&)>& copy_media,
                const std::function<void (Session&)>& finish = std::function<void (Session&)> ())
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
		if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); }
	}
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::map<std::string, std::string> assets;
	for (size_t i = 0; i < project.media.size (); ++i) {
		std::string path = Glib::build_filename (stage.path, "input-" + std::to_string (i) + ".media");
		copy_media (project.media[i], path);
		assets[project.media[i].id] = path;
	}
	std::unique_ptr<Session> session;
	try {
		BusProfile profile;
		profile.master_out_channels = 2;
		/* Ardour recognizes a new session by the absence of its directory. */
		session.reset (new Session (engine, Glib::build_filename (stage.path, "session"), snapshot, &profile, "", true));
		engine.set_session (session.get ());
		populate (*session, project, assets, report, cancel);
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
SessionExchange::import_dawproject (AudioEngine& engine, const std::string& archive_path, const std::string& destination,
                                   const std::string& snapshot, std::vector<Diagnostic>& report, CancelCheck cancel)
{
	Project project = DawProject::read (archive_path, cancel);
	ProjectArchive archive (archive_path, ArchiveLimits (), cancel);
	return import_session (engine, project, destination, snapshot, report, cancel,
	                       [&] (const Media& media, const std::string& path) { archive.copy_file (media.path, path); });
}

std::unique_ptr<Session>
SessionExchange::import_studio_one (AudioEngine& engine, const std::string& filename, const std::string& destination,
                                   const std::string& snapshot, std::vector<Diagnostic>& report, CancelCheck cancel)
{
	auto song = StudioOneSong::read (filename, cancel);
	return import_session (engine, song.project, destination, snapshot, report, cancel,
	                       [&] (const Media& media, const std::string& path) { copy_native_audio (song.audio_files.at (media.id), path, cancel); },
	                       [&] (Session& session) {
		                       auto position = [&] (Time t) { return Temporal::timepos_t (samples (convert_time (t, TimeUnit::Seconds, song.project.tempo), session.sample_rate ())); };
		                       if (song.has_range) { session.set_session_extents (position (song.start), position (song.end)); }
	                       });
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
	report.clear ();
	if (session.transport_rolling () || session.actively_recording ()) { throw ExchangeError ("Stop playback and recording before exporting a project"); }
	if (!Glib::path_is_absolute (destination)) { throw ExchangeError ("Export destination must be absolute"); }
	FileObject target = file_object (destination);
	if (g_file_query_exists (target.get (), NULL)) { throw ExchangeError ("Export destination already exists"); }
	cancelled (cancel);
	AudioAssets assets;
	Project project = collect (session, assets, cancel);
	report = project.report;
	for (auto const& item : project.report) {
		if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); }
	}
	OwnedDirectory stage (Glib::path_get_dirname (destination));
	std::map<std::string, std::string> files;
	for (auto const& media : project.media) {
		std::string path = Glib::build_filename (stage.path, media.id + ".wav");
		write_audio (assets.at (media.id), path, cancel);
		files[media.id] = path;
	}
	DawProject::write (project, destination, files, cancel);
}

} }
