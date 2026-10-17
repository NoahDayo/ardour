/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/studio_one_song.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <type_traits>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libxml/tree.h>
#include <sndfile.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {

void require (bool ok, const std::string& why) { if (!ok) { throw ExchangeError (why); } }
void check (const CancelCheck& cancel) { if (cancel && cancel ()) { throw ArchiveCancelled (); } }
using Node = xmlNode*;
using File = std::unique_ptr<GFile, void (*) (gpointer)>;
const xmlChar* xml (const char* text) { return reinterpret_cast<const xmlChar*> (text); }
File file (const std::string& path) { return File (g_file_new_for_path (path.c_str ()), g_object_unref); }
std::string number (double value) { std::ostringstream out; out.imbue (std::locale::classic ()); out << std::setprecision (17) << value; return out.str (); }
void prop (Node n, const char* name, const std::string& value) {
	require (value.size () <= 1024 * 1024 && value.find ('\0') == std::string::npos && g_utf8_validate (value.data (), value.size (), nullptr), "Invalid native song text");
	for (unsigned char c : value) { require (c >= 32 || c == 9 || c == 10 || c == 13, "Invalid native song XML character"); }
	require (xmlNewProp (n, xml (name), xml (value.c_str ())), "Cannot write native song property");
}
void prop (Node n, const char* name, const char* value) { prop (n, name, std::string (value)); }
template <typename T> typename std::enable_if<std::is_arithmetic<T>::value>::type
prop (Node n, const char* name, T value) { prop (n, name, number (double (value))); }
Node node (Node parent, const char* name, const char* id = nullptr) {
	Node n = xmlNewChild (parent, nullptr, xml (name), nullptr); require (n, "Cannot write native song element");
	if (id) { prop (n, "x:id", id); } return n;
}
std::string uid () {
	gchar* raw = g_uuid_string_random (); require (raw, "Cannot allocate native song ID");
	std::string result = "{"; for (char* p = raw; *p; ++p) { result += g_ascii_toupper (*p); } g_free (raw); return result + "}";
}
void unique_id (Node parent, const char* kind, const std::string& value) { prop (node (parent, "UID", kind), "uid", value); }
void speaker (Node parent, const char* id, unsigned channels) { prop (node (parent, "SpeakerSetup", id), "type", channels == 1 ? "Mono" : "Stereo"); }
std::string native_class_id (const std::string& id) {
	return "{" + id.substr (0, 8) + "-" + id.substr (8, 4) + "-" + id.substr (12, 4) + "-" + id.substr (16, 4) + "-" + id.substr (20) + "}";
}
void insert (Node inserts, const PluginDevice& plugin, const std::string& member, unsigned index) {
	Node slot = node (inserts, "Attributes");
	prop (slot, "name", std::string ("FX") + (index < 10 ? "0" : "") + std::to_string (index));
	unique_id (slot, "uniqueID", uid ());
	std::string class_id = native_class_id (plugin.id);
	unique_id (slot, "deviceClassID", class_id);
	Node data = node (slot, "Attributes", "deviceData"); prop (data, "name", plugin.name);
	unique_id (data, "uniqueID", uid ());
	Node ghost = node (slot, "Attributes", "ghostData"); prop (ghost, "presetType", "vstpreset");
	Node info = node (ghost, "Attributes", "classInfo");
	prop (info, "classID", class_id); prop (info, "name", plugin.name);
	prop (info, "category", "AudioEffect"); prop (info, "subCategory", "VST3");
	Node io = node (ghost, "Attributes", "IO");
	for (const char* kind : {"audioInputs", "audioOutputs"}) {
		Node ports = node (io, "List", kind), port = node (ports, "PortDescription");
		prop (port, "flags", 3); prop (port, "subType", 3);
		prop (port, "label", std::string (kind) == "audioInputs" ? "Input" : "Output");
	}
	Node presets = node (slot, "Attributes", "Presets"); prop (presets, "pname", "default"); prop (presets, "dirty", 1);
	prop (node (slot, "String", "presetPath"), "text", member);
}
std::string color (const std::string& value) {
	if (value.empty ()) { return "FF808080"; }
	require (value.size () == 7 && value[0] == '#' && value.find_first_not_of ("0123456789abcdefABCDEF", 1) == std::string::npos, "Invalid native song color");
	return "FF" + value.substr (5, 2) + value.substr (3, 2) + value.substr (1, 2);
}
unsigned format (TimeUnit unit) { return unit == TimeUnit::Seconds ? 0 : 2; }

struct Document {
	explicit Document (const char* name) : doc (xmlNewDoc (xml ("1.0")), xmlFreeDoc), root (xmlNewNode (nullptr, xml (name))) {
		require (doc && root, "Cannot allocate native song document"); xmlDocSetRootElement (doc.get (), root);
	}
	std::string text () const {
		xmlChar* bytes = nullptr; int length = 0; xmlDocDumpFormatMemoryEnc (doc.get (), &bytes, &length, "UTF-8", 1);
		require (bytes && length > 0, "Cannot serialize native song document");
		std::string result (reinterpret_cast<char*> (bytes), length); xmlFree (bytes);
		require (result.size () <= 16 * 1024 * 1024, "Native song XML exceeds its supported size"); return result;
	}
	std::unique_ptr<xmlDoc, void (*) (xmlDoc*)> doc;
	Node root;
};

/* A deliberately small UBJSON encoder: native musicx uses uncounted objects
 * and arrays with big-endian numeric payloads. Names are fixed schema keys. */
void key (std::string& out, const std::string& name) { require (name.size () < 128, "Native MIDI key is too long"); out += 'i'; out += char (name.size ()); out += name; }
void real (std::string& out, double value) {
	require (std::isfinite (value), "Non-finite native MIDI value"); uint64_t bits; std::memcpy (&bits, &value, sizeof (bits));
	out += 'D'; for (int shift = 56; shift >= 0; shift -= 8) { out += char ((bits >> shift) & 255); }
}
void field (std::string& out, const char* name, double value) { key (out, name); real (out, value); }
std::string music (const Clip& clip, const TimeMap& timing, const CancelCheck& cancel) {
	std::string out = "{"; field (out, "timeFormat", 2); key (out, "events"); out += '[';
	for (auto const& note : clip.notes) {
		check (cancel);
		double start = timing.content (Time (note.time, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
		double end = timing.content (Time (note.time + note.duration, clip.offset.unit), clip.position, clip.offset, TimeUnit::Beats);
		out += '{'; field (out, "start", start); field (out, "length", end - start); field (out, "pitch", note.key); field (out, "velocity", note.velocity);
		if (note.channel) { field (out, "channel", note.channel); }
		if (note.release_velocity != .5) { field (out, "releaseVelocity", note.release_velocity); }
		out += '}';
		require (out.size () <= 16 * 1024 * 1024 - 64, "Native MIDI exceeds its supported size");
	}
	out += ']'; key (out, "envelopes"); out += "[]}"; return out;
}

std::string envelope (const Automation& curve, const TimeMap& timing, bool pan, const CancelCheck& cancel) {
	std::string out = "{";
	if (pan) { key (out, "bipolar"); out += "i\x01"; }
	key (out, "events"); out += '[';
	for (auto const& point : curve.points) {
		check (cancel);
		out += '{'; field (out, "start", timing.convert (Time (point.time, curve.unit), TimeUnit::Seconds));
		field (out, "value", point.value); out += '}';
		require (out.size () <= 16 * 1024 * 1024 - 64, "Native automation exceeds its supported size");
	}
	out += "]}"; return out;
}

bool fades (const Clip& clip, const Project& project, const TimeMap& timing, double& in, double& out, double& gain) {
	in = out = 0; gain = 1; auto const& points = clip.envelope.points; if (points.empty ()) { return true; }
	if (clip.kind != ClipKind::Audio || clip.envelope.unit != TimeUnit::Seconds) { return false; }
	double length = timing.duration (Time (clip.duration, clip.position.unit), clip.position, TimeUnit::Seconds);
	auto media = std::find_if (project.media.begin (), project.media.end (), [&] (const Media& m) { return m.id == clip.media; });
	if (media == project.media.end ()) { return false; }
	const double quantum = 1.0 / media->sample_rate;
	gain = std::max_element (points.begin (), points.end (), [] (const AutomationPoint& a, const AutomationPoint& b) { return a.value < b.value; })->value;
	if (!std::isfinite (clip.gain * gain)) { return false; }
	if (gain == 0) { return true; }
	size_t count = points.size ();
	/* The collected envelope includes a synthetic held endpoint after
	 * Ardour's last audible sample. Validate the audible line below. */
	if (count >= 2 && std::abs (points.back ().time - length) < 1e-9 && points.back ().value > 0 && points.back ().value < gain &&
	    points[count - 2].value == points.back ().value && length - points[count - 2].time <= quantum + 1e-9) { --count; }
	if (points.front ().time != 0 || (points.front ().value != 0 && points.front ().value != gain)) { return false; }
	if (points.front ().value == 0) {
		auto first = std::find_if (points.begin (), points.end (), [&] (const AutomationPoint& p) { return p.value == gain; });
		if (first == points.end ()) { return false; } in = first->time;
	}
	if (points[count - 1].value != gain) {
		size_t last = count; while (last && points[last - 1].value != gain) { --last; }
		if (!last) { return false; } out = length - points[last - 1].time;
		/* Ardour clips its envelope at the last audible sample. Only accept
		 * that truncation when the same straight line reaches zero at end. */
		if (length - points[count - 1].time > quantum + 1e-9) { return false; }
	}
	if (in + out > length + 1e-9) { return false; }
	for (size_t i = 0; i < count; ++i) {
		auto const& p = points[i];
		double value = in > 0 && p.time < in ? p.time / in : out > 0 && p.time > length - out ? (length - p.time) / out : 1;
		if (std::abs (p.value / gain - value) > 1e-9 || (i + 1 < points.size () && !p.linear && points[i + 1].value != p.value)) { return false; }
	}
	return true;
}

std::string media_uri (GFile* parent, GFile* published_parent, const std::string& source) {
	require (g_path_is_absolute (source.c_str ()), "Native song media paths must be absolute"); File f = file (source);
	std::unique_ptr<gchar, void (*) (gpointer)> relative (g_file_get_relative_path (parent, f.get ()), g_free);
	require (relative && *relative, "Native song audio must be collected inside its song folder");
	File published (g_file_resolve_relative_path (published_parent, relative.get ()), g_object_unref);
	File current (G_FILE (g_object_ref (f.get ())), g_object_unref);
	while (!g_file_equal (current.get (), parent)) {
		GFileType type = g_file_query_file_type (current.get (), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr);
		require (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_DIRECTORY, "Native song media must not contain missing files or links");
		current.reset (g_file_get_parent (current.get ())); require (current.get (), "Invalid native media parent");
	}
	gchar* uri = g_file_get_uri (published.get ()); require (uri, "Cannot encode native media URI"); std::string result (uri); g_free (uri); return result;
}

unsigned audio_format (const std::string& filename, const Media& media) {
	int fd = g_open (filename.c_str (), O_RDONLY
#ifdef O_BINARY
	                 | O_BINARY
#endif
	                 , 0);
	require (fd >= 0, "Cannot open collected native song audio");
	SF_INFO info = {}; std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> input (sf_open_fd (fd, SFM_READ, &info, SF_TRUE), sf_close);
	if (!input) { g_close (fd, nullptr); throw ExchangeError ("Invalid collected native song audio"); }
	require (info.samplerate == int (media.sample_rate) && info.channels == int (media.channels) &&
	         std::abs (double (info.frames) - media.duration * media.sample_rate) < .5, "Collected native song audio does not match its metadata");
	require ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV || (info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_RF64, "Native song export requires collected WAV audio");
	switch (info.format & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_16: return 16;
	case SF_FORMAT_PCM_24: return 24;
	case SF_FORMAT_PCM_32: case SF_FORMAT_FLOAT: return 32;
	default: throw ExchangeError ("Unsupported native song WAV sample format");
	}
}

} // namespace

std::vector<Diagnostic> StudioOneSong::export_report (const Project& project)
{
	validate_project (project); TimeMap timing (project); std::vector<Diagnostic> report = project.report;
	auto unsupported = [&] (const std::string& name, const std::string& detail) { report.push_back ({Disposition::Unsupported, name, detail}); };
	std::string master; for (auto const& track : project.tracks) { if (track.master) { master = track.id; } }
	std::map<std::string, const Track*> routes;
	for (auto const& track : project.tracks) { routes.emplace (track.id, &track); }
	std::set<std::string> folders;
	for (auto const& track : project.tracks) { if (track.kind == TrackKind::Folder) { folders.insert (track.id); } }
	if (master.empty ()) { unsupported ("Master", "Native song export requires a stereo master output"); }
	for (auto const& media : project.media) { if (media.channels > 2) { unsupported (media.id, "Native multichannel audio requires premixed stereo media from the source DAW"); } }
	for (auto const& marker : project.markers) {
		if (marker.duration > 0) { unsupported (marker.name, "Native song range marker encoding has not been verified; export AAF/MIDI to preserve its duration"); }
	}
	for (auto const& track : project.tracks) {
		if (track.channels > 2) { unsupported (track.name, "Native multichannel tracks require premixed stereo media from the source DAW"); }
		if (track.master) {
			if (track.channels != 2 || track.pan != .5) { unsupported (track.name, "Native master export requires stereo with centered balance"); }
		} else if (!track.parent.empty () && !folders.count (track.parent)) {
			unsupported (track.name, "Native parent folder is missing");
		} else if (track.kind == TrackKind::Bus && (track.channels != 2 || !track.parent.empty ())) {
			unsupported (track.name, "Native bus export requires a stereo bus outside an arranger folder");
		} else if (track.kind == TrackKind::Audio || track.kind == TrackKind::Bus) {
			auto destination = routes.find (track.destination);
			if (destination == routes.end () || destination->second->kind != TrackKind::Bus || track.destination.empty ()) {
				unsupported (track.name, "Connect the audio track or bus to a master or audio bus before native song export");
			}
		} else if (!track.destination.empty () && track.destination != master) {
			unsupported (track.name, "Native MIDI and folder routing requires the rendered mix option");
		}
		if (track.kind == TrackKind::Folder && (track.channels != 2 || track.volume != 1 || track.pan != .5 || track.mute || track.solo)) {
			unsupported (track.name, "Native folder processing requires the rendered mix option");
		}
		if (!track.sends.empty () && (track.kind != TrackKind::Audio && track.kind != TrackKind::Bus)) {
			unsupported (track.name, "Native sends require an audio track or stereo bus");
		}
		for (auto const& plugin : track.plugins) {
			if ((track.kind != TrackKind::Audio && track.kind != TrackKind::Bus) || track.channels != 2 || plugin.role != "audioFX" ||
			    !plugin.enabled || plugin.state.empty ()) {
				unsupported (track.name, "Native VST3 export supports enabled, stereo audio-track, bus, and master effects with embedded preset state");
			} else {
				report.push_back ({Disposition::Transformed, plugin.name, "VST3 state embedded in the native song; Studio Pro opening and playback are not yet validated"});
			}
		}
		if (!track.volume_automation.points.empty () || !track.pan_automation.points.empty ()) {
			bool linear = track.kind == TrackKind::Audio;
			for (auto const* curve : {&track.volume_automation, &track.pan_automation}) {
				linear = linear && std::all_of (curve->points.begin (), curve->points.empty () ? curve->points.end () : curve->points.end () - 1,
				                                [] (const AutomationPoint& point) { return point.linear; });
			}
			if (!linear) { unsupported (track.name, "Native mixer automation supports only linear audio-track volume and pan points; render other automation"); }
			else {
				report.push_back ({Disposition::Transformed, track.name, "Native audio-track volume and pan automation written as linear points; Studio Pro playback is not yet validated"});
				if ((!track.volume_automation.points.empty () && track.volume_automation.unit == TimeUnit::Beats) ||
				    (!track.pan_automation.points.empty () && track.pan_automation.unit == TimeUnit::Beats)) {
					report.push_back ({Disposition::Transformed, track.name, "Beat-timed mixer automation becomes absolute-time points; later tempo edits will not move those points"});
				}
			}
		}
		if (track.kind == TrackKind::MIDI && (track.volume != 1 || track.pan != .5)) { unsupported (track.name, "Native MIDI mixer gain and pan require instrument rendering"); }
		for (auto const& clip : track.clips) {
			if (!clip.midi_controls.empty ()) { unsupported (clip.name, "Native MIDI controller mapping has not been verified; use DAWproject, AAF/MIDI or rendered audio"); }
			double in, out, gain; if (!fades (clip, project, timing, in, out, gain)) { unsupported (clip.name, "Native export supports ordinary linear fade lengths; other clip envelopes require rendering"); }
			else if (!clip.envelope.points.empty ()) { report.push_back ({Disposition::Transformed, clip.name, "Linear clip envelopes become native gain and fade lengths at audio sample resolution"}); }
			if (std::any_of (clip.notes.begin (), clip.notes.end (), [] (const Note& note) {
				return note.channel != 0 || note.release_velocity != .5;
			})) {
				report.push_back ({Disposition::Transformed, clip.name, "Per-note MIDI channels and release velocity encoded in native music data; target playback is not yet validated"});
			}
		}
	}
	if (std::any_of (project.tempos.begin (), project.tempos.end (), [] (const TempoChange& p) { return p.linear; })) {
		report.push_back ({Disposition::Transformed, "Tempo", "Tempo curves become native steps with at most 10 microseconds of timeline error"});
	}
	if (std::any_of (project.tracks.begin (), project.tracks.end (), [] (const Track& t) { return t.kind == TrackKind::MIDI; })) {
		report.push_back ({Disposition::Transformed, "MIDI", "MIDI notes retain musical timing without a target instrument; release velocity uses Studio One's default"});
	}
	report.push_back ({Disposition::Transformed, "Native Studio One song", "Experimental format-version-9 export. Target-application opening and playback have not yet been validated. Audio clips use absolute time; copy the complete song folder when moving it."});
	report.push_back ({Disposition::Transformed, "Audio device", "Hardware assignments are not transferred; choose the target computer's main outputs in Studio One or Studio Pro"});
	return report;
}

void StudioOneSong::write (const StudioOneSongData& song, const std::string& filename, const std::string& document_path, CancelCheck cancel)
{
	check (cancel); auto report = export_report (song.project);
	for (auto const& item : report) { if (item.disposition == Disposition::Unsupported) { throw ExchangeError (item.object + ": " + item.detail); } }
	require (g_path_is_absolute (filename.c_str ()) && g_path_is_absolute (document_path.c_str ()), "Native song filenames must be absolute");
	require (song.sample_rate >= 8000 && song.sample_rate <= 768000, "Invalid native song sample rate");
	require (song.format_version >= 7 && song.format_version <= 9, "Native song export format must be 7, 8 or 9");
	File output = file (filename), published = file (document_path);
	File parent (g_file_get_parent (output.get ()), g_object_unref), final_parent (g_file_get_parent (published.get ()), g_object_unref);
	require (parent && final_parent, "Native song requires a parent folder");
	std::unique_ptr<gchar, void (*) (gpointer)> document_uri (g_file_get_uri (published.get ()), g_free);
	require (document_uri && std::string (document_uri.get ()).compare (0, 8, "file:///") == 0, "Export native songs to a local folder before copying them to another computer");
	auto const& project = song.project; TimeMap timing (project);
	double begin = song.has_range ? timing.convert (song.start, TimeUnit::Seconds) : 0;
	double end = song.has_range ? timing.convert (song.end, TimeUnit::Seconds) : 0;
	for (auto const& track : project.tracks) {
		for (auto const& clip : track.clips) { if (!song.has_range) { end = std::max (end, timing.convert (Time (clip.position.value + clip.duration, clip.position.unit), TimeUnit::Seconds)); } }
		if (!song.has_range) { for (auto const* curve : {&track.volume_automation, &track.pan_automation}) {
			if (!curve->points.empty ()) { end = std::max (end, timing.convert (Time (curve->points.back ().time, curve->unit), TimeUnit::Seconds)); }
		} }
	}
	if (!song.has_range) { end = std::max (end, 1.0); }
	require (std::isfinite (begin) && std::isfinite (end) && begin >= 0 && end > begin, "Invalid native song range");
	std::map<std::string, std::string> channels, track_ids, media_ids;
	std::string master;
	for (auto const& track : project.tracks) {
		if (track.kind != TrackKind::Folder) { channels[track.id] = uid (); }
		if (!track.master) { track_ids[track.id] = uid (); }
		if (track.master) { master = channels[track.id]; }
	}
	Document meta ("MetaInformation"), arrangement ("Song"), pool ("MediaPool"), mixer ("AudioMixer"), midi ("MusicTrackDevice");
	auto attribute = [&] (const char* id, const std::string& value) { Node a = node (meta.root, "Attribute"); prop (a, "id", id); prop (a, "value", value); };
	attribute ("Document:MimeType", "application/x.presonus-song"); attribute ("Document:FormatVersion", number (song.format_version));
	attribute ("Document:Generator", "Ardour Project Exchange"); attribute ("Document:Title", song.title);
	attribute ("Media:SampleRate", number (song.sample_rate)); attribute ("Media:Tempo", number (project.tempo));
	attribute ("Media:TimeSignatureNumerator", number (project.numerator)); attribute ("Media:TimeSignatureDenominator", number (project.denominator));
	attribute ("Media:TrackCount", number (std::count_if (project.tracks.begin (), project.tracks.end (), [] (const Track& t) {
		return t.kind == TrackKind::Audio || t.kind == TrackKind::MIDI;
	}))); attribute ("Media:Length", number (end - begin)); attribute ("Media:TimeFormat", "2");
	Node root = node (arrangement.root, "Attributes", "Root"); prop (root, "defaultTimeFormat", 2); prop (root, "timeFormat", 0); prop (root, "length", end);
	Node context = node (root, "Attributes", "timeContext"); prop (context, "sampleRate", song.sample_rate); prop (context, "frameType", 1);
	Node tempos = node (context, "TempoMap", "tempoMap"); auto steps = timing.stepped (.00001);
	for (size_t i = 0; i < steps.size (); ++i) {
		check (cancel); Node n = node (tempos, "TempoMapSegment"); prop (n, "curveType", 0); prop (n, "start", steps[i].position.value);
		prop (n, "end", i + 1 < steps.size () ? steps[i + 1].position.value : 1.7976931348623156e200); prop (n, "tempo", 60 / steps[i].bpm);
	}
	Node meters = node (context, "TimeSignatureMap", "timeSignatureMap");
	auto meter_points = project.meters; if (meter_points.empty ()) { meter_points.push_back ({Time (0), project.numerator, project.denominator}); }
	for (auto const& point : meter_points) { Node n = node (meters, "TimeSignatureMapSegment"); prop (n, "start", timing.convert (point.position, TimeUnit::Beats)); prop (n, "numerator", point.numerator); prop (n, "denominator", point.denominator); prop (n, "type", 0); }
	Node tracks = node (root, "List", "Tracks"), markers = node (tracks, "MarkerTrack"); prop (markers, "version", 1); prop (markers, "timeFormat", 2);
	auto marker = [&] (const std::string& name, Time at, unsigned type) { Node n = node (markers, "MarkerEvent"); prop (n, "name", name); prop (n, "start", at.value); prop (n, "timeFormat", format (at.unit)); prop (n, "markerType", type); prop (n, "markerStop", 0); };
	marker ("Start", Time (begin, TimeUnit::Seconds), 2); marker ("End", Time (end, TimeUnit::Seconds), 3);
	for (auto const& item : project.markers) { marker (item.name, item.position, 0); }
	Node pool_root = node (pool.root, "Attributes", "rootFolder"), audio_pool = node (pool_root, "MediaFolder"), music_pool = node (pool_root, "MediaFolder");
	prop (audio_pool, "name", "Audio"); prop (music_pool, "name", "Music");
	prop (node (pool.root, "Attributes", "documentPath"), "url", document_uri.get ());
	for (auto const& media : project.media) {
		check (cancel); auto f = song.audio_files.find (media.id); require (f != song.audio_files.end (), "Missing collected native song audio");
		std::string url = media_uri (parent.get (), final_parent.get (), f->second); unsigned bits = audio_format (f->second, media);
		Node n = node (audio_pool, "AudioClip"); media_ids[media.id] = uid (); prop (n, "mediaID", media_ids[media.id]);
		Node location = node (n, "Url", "path"); prop (location, "type", 1); prop (location, "url", url);
		Node info = node (n, "Attributes", "format"); prop (info, "sampleRate", media.sample_rate); prop (info, "numChannels", media.channels);
		prop (info, "frameCount", std::round (media.duration * media.sample_rate)); prop (info, "bitDepth", bits);
	}
	Node audio_channels = node (mixer.root, "Attributes", "channels"), music_channels = node (midi.root, "Attributes", "channels");
	Node outputs = node (audio_channels, "ChannelGroup"), audio_tracks = node (audio_channels, "ChannelGroup"), audio_groups = node (audio_channels, "ChannelGroup"), music_tracks = node (music_channels, "ChannelGroup");
	prop (outputs, "name", "AudioOutput"); prop (audio_tracks, "name", "AudioTrack"); prop (audio_groups, "name", "AudioGroup"); prop (music_tracks, "name", "MusicTrack");
	ProjectArchiveWriter archive (filename, ArchiveLimits (), cancel); unsigned track_number = 0, music_number = 0;
	for (auto const& track : project.tracks) {
		if (track.kind == TrackKind::Folder) {
			Node folder = node (tracks, "FolderTrack"); prop (folder, "version", 1);
			prop (folder, "trackID", track_ids.at (track.id)); prop (folder, "timeFormat", 2); prop (folder, "name", track.name);
			if (!track.color.empty ()) { prop (folder, "color", color (track.color)); }
			if (!track.parent.empty ()) { prop (folder, "parentFolder", track_ids.at (track.parent)); }
			prop (node (folder, "Attributes", "AutomationRegionList"), "visible", 0);
			continue;
		}
		check (cancel); bool musical = track.kind == TrackKind::MIDI, bus = track.kind == TrackKind::Bus;
		Node channel = node (track.master ? outputs : bus ? audio_groups : musical ? music_tracks : audio_tracks, track.master ? "AudioOutputChannel" : bus ? "AudioGroupChannel" : musical ? "MusicTrackChannel" : "AudioTrackChannel");
		unique_id (channel, "uniqueID", channels[track.id]); prop (channel, "name", "Channel" + std::to_string (++track_number)); prop (channel, "label", track.name);
		prop (channel, "mute", track.mute); prop (channel, "solo", track.solo); prop (channel, "color", color (track.color));
		if (!musical) {
			prop (channel, "gain", track.volume); speaker (channel, "speakerType", track.channels);
			if (!track.master) {
				prop (channel, "pan", track.pan); prop (node (channel, "Attributes", "Panner"), "panX1", track.pan);
				prop (node (channel, "Connection", "destination"), "objectID", channels.at (track.destination) + "/Input");
			}
		}
		if (!track.plugins.empty ()) {
			Node inserts = node (channel, "Attributes", "Inserts"); unsigned index = 0;
			for (auto const& plugin : track.plugins) {
				check (cancel);
				std::string member = "Presets/Channels/Stereo/track-" + std::to_string (track_number) +
				                     "-fx-" + std::to_string (++index) + ".vstpreset";
				insert (inserts, plugin, member, index);
				archive.add_text (member, plugin.state);
			}
			Node presets = node (inserts, "Attributes", "Presets"); prop (presets, "pname", "default"); prop (presets, "dirty", 0);
			Node combinator = node (inserts, "Attributes", "Combinator"); prop (combinator, "name", "Combinator");
			unique_id (combinator, "uniqueID", uid ());
		}
		if (!track.sends.empty ()) {
			Node rack = node (channel, "Attributes", "Sends"); unsigned send_number = 0;
			for (auto const& send : track.sends) {
				check (cancel); Node item = node (rack, "Attributes"); ++send_number;
				prop (item, "name", std::string ("Send") + (send_number < 10 ? "0" : "") + std::to_string (send_number));
				prop (item, "bypass", !send.enabled); prop (item, "prefader", send.pre_fader);
				prop (item, "level", send.volume); prop (item, "pan", send.pan); prop (item, "lockPan", 0);
				prop (node (item, "Connection", "destination"), "objectID", channels.at (send.destination) + "/Input");
			}
			Node presets = node (rack, "Attributes", "Presets"); prop (presets, "pname", "default"); prop (presets, "dirty", 0);
		}
		if (track.master || bus) { continue; }
		Node t = node (tracks, "MediaTrack"); prop (t, "mediaType", musical ? "Music" : "Audio"); prop (t, "tempoFollow", musical ? 2 : 0);
		prop (t, "version", 1); prop (t, "trackID", track_ids.at (track.id)); prop (t, "trackNumber", track_number); prop (t, "timeFormat", musical ? 2 : 0); prop (t, "name", track.name); prop (t, "color", color (track.color));
		if (!track.parent.empty ()) { prop (t, "parentFolder", track_ids.at (track.parent)); }
		unique_id (t, "channelID", channels[track.id]); if (!musical) { speaker (t, "trackFormat", track.channels); }
		if (!musical && (!track.volume_automation.points.empty () || !track.pan_automation.points.empty ())) {
			Node regions = node (t, "Attributes", "AutomationRegionList"); prop (regions, "visible", 0);
			auto add_curve = [&] (const Automation& curve, bool pan) {
				if (curve.points.empty ()) { return; }
				std::string member = "Envelopes/track-" + std::to_string (track_number) + (pan ? "/Pan.envelopex" : "/Volume.envelopex");
				Node region = node (regions, "AutomationRegion"); prop (region, "title", pan ? "Pan L/R" : "Volume");
				prop (region, "identity", "param:///AudioMixer/" + channels.at (track.id) + (pan ? "/Panner/panX1" : "/volume"));
				prop (region, "metaIdentity", pan ? "AudioPanner/panX1" : "AudioTrackChannel/volume");
				prop (region, "userRenamed", 0); prop (region, "color", pan ? "FFC7FF3A" : "FFFFD829"); prop (region, "mute", 0);
				Node location = node (region, "Url", "points"); prop (location, "type", 1); prop (location, "url", "media:///" + member);
				archive.add_text (member, envelope (curve, timing, pan, cancel));
			};
			add_curve (track.volume_automation, false); add_curve (track.pan_automation, true);
		}
		Node events = node (t, "List", "Events");
		for (auto const& clip : track.clips) {
			check (cancel); TimeUnit unit = musical ? TimeUnit::Beats : TimeUnit::Seconds;
			Node e = node (events, musical ? "MusicPart" : "AudioEvent"); prop (e, "name", clip.name); prop (e, "color", color (clip.color));
			prop (e, "timeFormat", format (unit)); prop (e, "start", timing.convert (clip.position, unit));
			prop (e, "length", timing.duration (Time (clip.duration, clip.position.unit), clip.position, unit));
			prop (e, "offset", timing.content (clip.offset, clip.position, clip.offset, unit)); prop (e, "mute", !clip.enabled);
			if (musical) {
				std::string id = uid (), member = "Performances/notes-" + std::to_string (++music_number) + ".musicx";
				prop (e, "clipID", id); Node source = node (music_pool, "MusicClip"); prop (source, "mediaID", id);
				Node location = node (source, "Url", "dataPath"); prop (location, "type", 1); prop (location, "url", "media:///" + member);
				archive.add_text (member, music (clip, timing, cancel));
			} else {
				double in, out, gain; require (fades (clip, project, timing, in, out, gain), "Unsupported native clip envelope");
				prop (e, "clipID", media_ids.at (clip.media)); prop (e, "gain", clip.gain * gain); prop (e, "speed", 1);
				if (in || out) { Node envelope = node (e, "Attributes", "volumeCurve"); prop (envelope, "fadeIn.length", in); prop (envelope, "fadeOut.length", out); }
			}
		}
	}
	archive.add_text ("metainfo.xml", meta.text ()); archive.add_text ("Song/song.xml", arrangement.text ());
	archive.add_text ("Song/mediapool.xml", pool.text ()); archive.add_text ("Devices/audiomixer.xml", mixer.text ()); archive.add_text ("Devices/musictrackdevice.xml", midi.text ());
	archive.add_text ("Devices/audiosynthfolder.xml", "<AudioSynthFolder><Attributes x:id=\"Presets\"/></AudioSynthFolder>");
	archive.add_text ("Devices/audioiomanager.xml", "<AudioIOManager><List x:id=\"deviceList\"/></AudioIOManager>");
	archive.add_text ("Devices/transportdevice.xml", "<TransportDevice position=\"0\" loopActive=\"0\"/>");
	check (cancel); archive.finish ();
}

} }
