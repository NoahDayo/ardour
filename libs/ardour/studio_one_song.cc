/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/studio_one_song.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <gio/gio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {

void require (bool ok, const std::string& message) { if (!ok) { throw ExchangeError (message); } }
typedef xmlNode* Node;
typedef std::unique_ptr<xmlDoc, void (*) (xmlDoc*)> Document;

std::string value (Node node, const char* key, const std::string& fallback = "")
{
	if (!node) { return fallback; }
	xmlChar* raw = xmlGetProp (node, BAD_CAST key);
	if (!raw) { return fallback; }
	std::string result (reinterpret_cast<const char*> (raw));
	xmlFree (raw);
	return result;
}

std::string id (Node node)
{
	std::string result = value (node, "x:id");
	return result.empty () ? value (node, "id") : result;
}

std::string vst3_class_id (const std::string& native_uid)
{
	require (native_uid.size () == 38 && native_uid.front () == '{' && native_uid.back () == '}',
	         "Invalid native VST3 class ID");
	return canonical_vst3_id (native_uid.substr (1, 36));
}

bool is (Node node, const char* name) { return node && xmlStrEqual (node->name, BAD_CAST name); }
std::vector<Node> children (Node node)
{
	std::vector<Node> result;
	if (node) { for (Node n = node->children; n; n = n->next) { if (n->type == XML_ELEMENT_NODE) { result.push_back (n); } } }
	return result;
}
Node child (Node node, const char* name, const char* identifier = nullptr)
{
	Node found = nullptr;
	for (Node n : children (node)) {
		if (is (n, name) && (!identifier || id (n) == identifier)) {
			require (!found, "Duplicate native song element: " + std::string (name));
			found = n;
		}
	}
	return found;
}
double number (Node node, const char* key, double fallback = 0)
{
	std::string text = value (node, key);
	if (text.empty ()) { return fallback; }
	char* end = nullptr;
	errno = 0;
	double n = g_ascii_strtod (text.c_str (), &end);
	require (!errno && end != text.c_str () && !*end && std::isfinite (n), "Invalid native song number: " + std::string (key));
	return n;
}
unsigned integer (Node node, const char* key, unsigned fallback = 0)
{
	double n = number (node, key, fallback);
	require (n >= 0 && n <= std::numeric_limits<unsigned>::max () && std::floor (n) == n, "Invalid native song integer: " + std::string (key));
	return static_cast<unsigned> (n);
}
bool boolean (Node node, const char* key, bool fallback = false)
{
	double n = number (node, key, fallback ? 1 : 0);
	require (n == 0 || n == 1, "Invalid native song boolean: " + std::string (key));
	return n != 0;
}
TimeUnit unit (unsigned format)
{
	require (format == 0 || format == 2, "Unsupported Studio One time format");
	return format == 0 ? TimeUnit::Seconds : TimeUnit::Beats;
}
void reject_dtd (void* context, const xmlChar*, const xmlChar*, const xmlChar*) { xmlStopParser (static_cast<xmlParserCtxt*> (context)); }
void check_tree (Node n, size_t& count, unsigned depth)
{
	require (++count <= 1000000 && depth <= 64, "Native song XML exceeds structural limits");
	for (Node c : children (n)) { check_tree (c, count, depth + 1); }
}
Document xml (const std::string& text)
{
	require (!text.empty () && text.size () <= 16 * 1024 * 1024, "Invalid native song XML size");
	std::unique_ptr<xmlParserCtxt, void (*) (xmlParserCtxt*)> context (xmlNewParserCtxt (), xmlFreeParserCtxt);
	require (context.get (), "Cannot allocate native song XML parser");
	context->sax->internalSubset = reject_dtd;
	context->sax->externalSubset = reject_dtd;
	/* Studio One uses unbound x:id attributes in its native documents.
	 * libxml retains these literal names; namespace errors are not recovered
	 * into fabricated document structure. DTDs and malformed XML are refused. */
	Document doc (xmlCtxtReadMemory (context.get (), text.data (), text.size (), nullptr, nullptr,
	                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
	require (doc && context->wellFormed && !doc->intSubset && !doc->extSubset, "Malformed or unsafe native song XML");
	size_t count = 0;
	check_tree (xmlDocGetRootElement (doc.get ()), count, 0);
	return doc;
}

/* Bounded UBJSON reader for native musicx/envelopex. Numeric payloads are
 * big-endian. Duplicate keys, non-finite values and unsupported types fail. */
struct Value {
	enum Type { Null, Number, String, Array, Object, Boolean } type = Null;
	double number = 0;
	std::string string;
	std::vector<Value> array;
	std::map<std::string, Value> object;
	const Value* find (const std::string& name) const {
		require (type == Object, "Expected native MIDI object");
		auto i = object.find (name); return i == object.end () ? nullptr : &i->second;
	}
	double get (const char* name, double fallback) const {
		auto v = find (name); if (!v) { return fallback; }
		require (v->type == Number, "Expected native MIDI number: " + std::string (name)); return v->number;
	}
};
class BinaryJSON {
public:
	explicit BinaryJSON (const std::string& bytes) : data (bytes) {}
	Value parse () { Value result = read (0); skip (); require (pos == data.size (), "Trailing native MIDI data"); return result; }
private:
	const std::string& data;
	size_t pos = 0, nodes = 0;
	unsigned char byte () { require (pos < data.size (), "Truncated native MIDI data"); return static_cast<unsigned char> (data[pos++]); }
	void skip () { while (pos < data.size () && data[pos] == 'N') { ++pos; } }
	uint64_t bits (unsigned n) { uint64_t v = 0; while (n--) { v = (v << 8) | byte (); } return v; }
	int64_t signed_integer (char marker) {
		switch (marker) {
		case 'i': return static_cast<int8_t> (byte ());
		case 'U': return byte ();
		case 'I': return static_cast<int16_t> (bits (2));
		case 'l': return static_cast<int32_t> (bits (4));
		case 'L': { uint64_t b = bits (8); int64_t v; std::memcpy (&v, &b, 8); return v; }
		default: throw ExchangeError ("Invalid native MIDI integer type");
		}
	}
	size_t length () { int64_t n = signed_integer (byte ()); require (n >= 0 && uint64_t (n) <= data.size (), "Invalid native MIDI length"); return n; }
	std::string string () {
		size_t n = length (); require (n <= data.size () - pos, "Truncated native MIDI string");
		std::string s = data.substr (pos, n); pos += n;
		require (g_utf8_validate (s.data (), s.size (), nullptr) && s.find ('\0') == std::string::npos, "Invalid native MIDI text"); return s;
	}
	Value read (unsigned depth, char forced = 0) {
		require (++nodes <= 1000000 && depth <= 64, "Native MIDI exceeds structural limits");
		if (!forced) { skip (); }
		char marker = forced ? forced : byte (); Value v;
		switch (marker) {
		case 'Z': return v;
		case 'T': case 'F': v.type = Value::Boolean; v.number = marker == 'T'; return v;
		case 'i': case 'U': case 'I': case 'l': case 'L': v.type = Value::Number; v.number = signed_integer (marker); return v;
		case 'd': { uint32_t b = bits (4); float f; std::memcpy (&f, &b, 4); v.type = Value::Number; v.number = f; break; }
		case 'D': { uint64_t b = bits (8); double f; std::memcpy (&f, &b, 8); v.type = Value::Number; v.number = f; break; }
		case 'S': v.type = Value::String; v.string = string (); return v;
		case 'C': v.type = Value::String; v.string += byte (); require (static_cast<unsigned char> (v.string[0]) < 128, "Invalid native MIDI character"); return v;
		case '[': case '{': {
			v.type = marker == '[' ? Value::Array : Value::Object;
			char close = marker == '[' ? ']' : '}'; char typed = 0; bool counted = false; size_t count = 0;
			if (pos < data.size () && data[pos] == '$') { ++pos; typed = byte (); }
			if (pos < data.size () && data[pos] == '#') { ++pos; counted = true; count = length (); }
			require (!typed || counted, "Typed native MIDI container has no length");
			require (count <= 1000000, "Native MIDI container is too large");
			for (size_t i = 0;; ++i) {
				if (counted && i == count) { break; }
				if (!typed || v.type == Value::Object) { skip (); }
				if (!counted && pos < data.size () && data[pos] == close) { ++pos; break; }
				if (v.type == Value::Object) {
					std::string key = string ();
					require (!v.object.count (key), "Duplicate native MIDI key");
					v.object.emplace (key, read (depth + 1, typed));
				} else { v.array.push_back (read (depth + 1, typed)); }
			}
			return v;
		}
		default: throw ExchangeError ("Unsupported native MIDI encoding");
		}
		require (std::isfinite (v.number), "Non-finite native MIDI number"); return v;
	}
};

std::string color (const std::string& native)
{
	if (native.empty ()) { return ""; }
	require (native.size () == 8 && native.find_first_not_of ("0123456789abcdefABCDEF") == std::string::npos, "Unsupported Studio One color");
	/* Native colors are AABBGGRR. */
	return "#" + native.substr (6, 2) + native.substr (4, 2) + native.substr (2, 2);
}

typedef std::unique_ptr<GFile, void (*) (gpointer)> File;
File file (const std::string& path) { return File (g_file_new_for_path (path.c_str ()), g_object_unref); }
std::string path (GFile* f) { char* p = g_file_get_path (f); require (p, "Native song requires local media"); std::string s (p); g_free (p); return s; }
std::string url_path (const std::string& uri)
{
	require (uri.compare (0, 8, "file:///") == 0, "Native song media must be copied into its local song folder");
	File f (g_file_new_for_uri (uri.c_str ()), g_object_unref); return path (f.get ());
}
std::string member_url (const std::string& uri)
{
	require (uri.compare (0, 9, "media:///") == 0, "Native MIDI must be embedded in the song");
	char* unescaped = g_uri_unescape_string (uri.c_str () + 9, nullptr);
	require (unescaped, "Invalid native media URL"); std::string result (unescaped); g_free (unescaped);
	return result;
}

class Reader {
public:
	Reader (const std::string& filename, CancelCheck check) : archive (filename, ArchiveLimits (), check), cancel (check) {
		File song = file (filename); File parent (g_file_get_parent (song.get ()), g_object_unref);
		require (parent.get (), "Native song has no parent folder"); root = path (parent.get ());
		for (auto const& m : archive.members ()) { members.insert (m.name); }
	}
	StudioOneSongData read ();
private:
	ProjectArchive archive;
	CancelCheck cancel;
	StudioOneSongData result;
	std::string root, original_root;
	std::set<std::string> members;
	std::map<std::string, Node> pool, channels;
	std::map<std::string, size_t> media_indices;
	std::vector<Document> documents;
	Node document (const std::string& name, const char* expected) {
		check (); documents.push_back (xml (archive.read_text (name)));
		Node n = xmlDocGetRootElement (documents.back ().get ()); require (is (n, expected), "Unexpected native song document: " + name); return n;
	}
	void check () { if (cancel && cancel ()) { throw ArchiveCancelled (); } }
	void unsupported (const std::string& object, const std::string& detail) { result.project.report.push_back ({Disposition::Unsupported, object, detail}); }
	void insert (Track&, Node);
	void attributes (Node node, const std::set<std::string>& known, const std::string& object) {
		for (xmlAttr* a = node->properties; a; a = a->next) {
			std::string name (reinterpret_cast<const char*> (a->name));
			if (!known.count (name)) { unsupported (object, "Native property is not yet supported: " + name); }
		}
	}
	void index_pool (Node n) {
		if (is (n, "AudioClip") || is (n, "MusicClip")) { std::string key = value (n, "mediaID"); require (!key.empty () && pool.emplace (key, n).second, "Duplicate or empty native media ID"); }
		for (Node c : children (n)) { index_pool (c); }
	}
	void index_channels (Node n) {
		if (Node uid = child (n, "UID", "uniqueID")) {
			if (is (n, "AudioTrackChannel") || is (n, "MusicTrackChannel") || is (n, "AudioGroupChannel") || is (n, "AudioEffectChannel") || is (n, "AudioOutputChannel")) {
				std::string key = value (uid, "uid"); require (!key.empty () && channels.emplace (key, n).second, "Duplicate native channel ID");
			}
		}
		for (Node c : children (n)) { index_channels (c); }
	}
	std::string media_path (const std::string& uri);
	std::string audio (const std::string& key);
	void notes (Clip& clip, Node entry);
	void sends (Track& track, Node rack);
	void channel (Track& track, Node n);
	void transport (Node context);
	void track (Node n);
	void folder (Node n);
	void automation (Node n, Track& track, const std::string& channel_id, TimeUnit time_unit);
};

std::string Reader::media_path (const std::string& uri)
{
	File original = file (url_path (uri)); File base = file (original_root);
	char* raw = g_file_get_relative_path (base.get (), original.get ());
	require (raw, "Media is outside the song folder. In Studio One use Save to New Folder to collect the media.");
	std::string relative (raw); g_free (raw);
	require (!relative.empty (), "Invalid native audio path");
	File folder = file (root); File local (g_file_resolve_relative_path (folder.get (), relative.c_str ()), g_object_unref);
	require (g_file_has_prefix (local.get (), folder.get ()), "Native media path escapes the song folder");
	/* Refuse links in every component; never follow a song-supplied path to
	 * other local files or to a network share. */
	File current (G_FILE (g_object_ref (local.get ())), g_object_unref);
	while (!g_file_equal (current.get (), folder.get ())) {
		GFileInfo* info = g_file_query_info (current.get (), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
		                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
		require (info, "Missing native song media: " + relative);
		bool link = g_file_info_get_is_symlink (info); g_object_unref (info);
		require (!link, "Native song media links are not supported");
		current.reset (g_file_get_parent (current.get ())); require (current.get (), "Invalid native song media path");
	}
	require (g_file_query_file_type (local.get (), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) == G_FILE_TYPE_REGULAR, "Native audio is not a regular file");
	return path (local.get ());
}

std::string Reader::audio (const std::string& key)
{
	if (media_indices.count (key)) { return key; }
	auto entry = pool.find (key); require (entry != pool.end () && is (entry->second, "AudioClip"), "Native audio clip references missing media");
	Node n = entry->second; Node format = child (n, "Attributes", "format"); require (format, "Missing native audio format");
	Media m; m.id = key; m.path = "audio/source-" + std::to_string (result.project.media.size ()) + ".wav";
	m.sample_rate = integer (format, "sampleRate"); m.channels = integer (format, "numChannels");
	require (m.sample_rate, "Invalid native audio sample rate"); m.duration = number (format, "frameCount", -1) / m.sample_rate;
	result.audio_files[key] = media_path (value (child (n, "Url", "path"), "url"));
	media_indices[key] = result.project.media.size (); result.project.media.push_back (m); return key;
}

void Reader::notes (Clip& clip, Node entry)
{
	std::string name = member_url (value (child (entry, "Url", "dataPath"), "url"));
	std::string data = archive.read_text (name); Value content = BinaryJSON (data).parse ();
	double format = content.get ("timeFormat", 2); require (format == 0 || format == 2, "Invalid native MIDI time format");
	clip.offset.unit = unit (static_cast<unsigned> (format));
	auto events = content.find ("events"); require (events && events->type == Value::Array, "Native MIDI has no event array");
	for (auto const& e : events->array) {
		check (); Note n;
		double pitch = e.get ("pitch", -1), ch = e.get ("channel", 0);
		require (pitch >= 0 && pitch <= 127 && std::floor (pitch) == pitch && ch >= 0 && ch <= 15 && std::floor (ch) == ch, "Unsupported native MIDI event or channel");
		n.key = pitch; n.channel = ch; n.time = e.get ("start", 0); n.duration = e.get ("length", 0); n.velocity = e.get ("velocity", 1);
		n.release_velocity = e.get ("releaseVelocity", 0.5);
		for (auto const& a : e.object) {
			if (!std::set<std::string> {"start", "length", "pitch", "channel", "velocity", "releaseVelocity", "noteId"}.count (a.first)) { unsupported (clip.name, "Native MIDI event property: " + a.first); }
		}
		clip.notes.push_back (n);
	}
	for (auto const& a : content.object) {
		if (a.first == "envelopes") {
			require (a.second.type == Value::Array, "Invalid native MIDI envelopes");
			if (!a.second.array.empty ()) { unsupported (clip.name, "MIDI controllers and note expressions are not yet supported"); }
		} else if (a.first != "events" && a.first != "timeFormat") { unsupported (clip.name, "Native MIDI property: " + a.first); }
	}
}

void Reader::automation (Node n, Track& track, const std::string& channel_id, TimeUnit time_unit)
{
	for (Node c : children (n)) {
		if (!is (c, "AutomationRegion")) { unsupported (track.name, "Unknown native automation region"); continue; }
		std::string member = member_url (value (child (c, "Url", "points"), "url"));
		if (!members.count (member)) { unsupported (track.name, "Referenced native automation member is missing: " + member); continue; }
		std::string data = archive.read_text (member); Value envelope = BinaryJSON (data).parse ();
		auto events = envelope.find ("events");
		require (events && events->type == Value::Array, "Malformed native automation envelope");
		bool unknown = false;
		for (auto const& item : envelope.object) { unknown = unknown || (item.first != "events" && item.first != "bipolar"); }
		if (events->array.empty () && !unknown) { continue; }
		std::string identity = "param:///AudioMixer/" + channel_id + "/";
		std::string target = value (c, "metaIdentity");
		bool volume = target == "AudioTrackChannel/volume" && value (c, "identity") == identity + "volume";
		bool pan = target == "AudioPanner/panX1" && value (c, "identity") == identity + "Panner/panX1";
		if (unknown || track.kind != TrackKind::Audio || (!volume && !pan) || boolean (c, "mute")) {
			unsupported (track.name, "Automation is not yet supported: " + value (c, "title")); continue;
		}
		if (const Value* bipolar = envelope.find ("bipolar")) {
			if (bipolar->type != Value::Number || bipolar->number != (pan ? 1 : 0)) {
				unsupported (track.name, "Native automation polarity is not supported: " + value (c, "title")); continue;
			}
		}
		Automation& curve = pan ? track.pan_automation : track.volume_automation;
		if (!curve.points.empty () || events->array.size () > 100000) {
			unsupported (track.name, "Native automation has duplicate or excessive points: " + value (c, "title")); continue;
		}
		Automation decoded; decoded.unit = time_unit;
		double previous = -1;
		bool supported = true;
		for (auto const& event : events->array) {
			check ();
			if (event.type != Value::Object || event.object.size () != 2 || !event.find ("start") || !event.find ("value")) { supported = false; break; }
			double at = event.get ("start", -1), value = event.get ("value", -1);
			if (at < 0 || at <= previous) { throw ExchangeError ("Invalid or unordered native automation points"); }
			if (value < 0 || (pan && value > 1)) { supported = false; break; }
			decoded.points.push_back ({at, value, true}); previous = at;
		}
		if (!supported) { unsupported (track.name, "Native automation point type or value is not supported: " + value (c, "title")); continue; }
		curve = std::move (decoded);
		result.project.report.push_back ({Disposition::Transformed, track.name,
		                                 "Native " + std::string (pan ? "pan" : "volume") + " automation mapped as linear points; target playback is not yet validated"});
	}
}

void Reader::channel (Track& track, Node n)
{
	attributes (n, {"gain", "pan", "iconid", "name", "label", "mute", "solo", "soloSafe", "color", "transpose", "velocity", "meteringPosition"}, track.name);
	track.mute = boolean (n, "mute"); track.solo = boolean (n, "solo");
	track.volume = number (n, "gain", 1); track.pan = number (n, "pan", 0.5);
	if (number (n, "transpose") || number (n, "velocity")) { unsupported (track.name, "MIDI channel transpose or velocity adjustment"); }
	for (Node c : children (n)) {
		std::string identifier = id (c);
		if (identifier == "Inserts" || identifier == "PostFaderInserts") {
			for (Node item : children (c)) {
				if (id (item) == "Presets" && children (item).empty ()) { continue; }
				if (id (item) == "Combinator" && children (item).size () == 1 && is (children (item)[0], "UID")) { continue; }
				if (identifier == "Inserts" && is (item, "Attributes") && id (item).empty () && !value (item, "name").empty ()) {
					insert (track, item); continue;
				}
				unsupported (track.name, "Native plug-in slot is not yet supported");
			}
		} else if (identifier == "Panner") {
			track.pan = number (c, "panX1", track.pan);
			if (number (c, "panX2") || number (c, "panWidth", 1) != 1 || number (c, "stereoMode") || number (c, "spatialObjectMode")) { unsupported (track.name, "Advanced panning is not yet supported"); }
		} else if (identifier == "InputFX") {
			if (number (c, "gain") || number (c, "invertPhaseL") || number (c, "invertPhaseR")) { unsupported (track.name, "Input gain or polarity adjustment"); }
		} else if (identifier == "AudioBusMixFX" && !boolean (c, "passThrough", true)) { unsupported (track.name, "Mix engine effects are not yet supported"); }
	}
	if (Node connection = child (n, "Connection", "destination")) {
		std::string target = value (connection, "objectID");
		auto slash = target.find ('/');
		if (slash != std::string::npos && target.substr (slash) == "/Input") {
			track.destination = target.substr (0, slash);
		} else if (track.kind == TrackKind::MIDI && slash != std::string::npos && target.substr (slash) == "/MusicInput00") {
			unsupported (track.name, "Native instrument routing is not yet supported");
		} else {
			throw ExchangeError ("Unsupported native channel routing");
		}
	}
	if (Node rack = child (n, "Attributes", "Sends")) { sends (track, rack); }
}

void Reader::sends (Track& track, Node rack)
{
	for (Node item : children (rack)) {
		check ();
		if (is (item, "Attributes") && id (item) == "Presets" && children (item).empty ()) { continue; }
		std::string name = value (item, "name");
		if (!is (item, "Attributes") || name.compare (0, 4, "Send") != 0) {
			unsupported (track.name, "Native send rack content is not yet supported"); continue;
		}
		attributes (item, {"name", "bypass", "prefader", "level", "pan", "lockPan"}, track.name);
		Node connection = child (item, "Connection", "destination");
		if (!connection) { unsupported (track.name, "Native send has no destination"); continue; }
		std::string target = value (connection, "objectID");
		auto slash = target.find ('/');
		if (slash == std::string::npos || target.substr (slash) != "/Input") {
			unsupported (track.name, "Native sidechain or hardware send is not yet supported"); continue;
		}
		auto destination = channels.find (target.substr (0, slash));
		if (destination == channels.end () ||
		    (!is (destination->second, "AudioGroupChannel") && !is (destination->second, "AudioEffectChannel"))) {
			unsupported (track.name, "Native send target is not an audio bus"); continue;
		}
		for (Node content : children (item)) {
			if (content != connection) { unsupported (track.name, "Native send content is not yet supported"); }
		}
		Send send; send.destination = destination->first; send.pre_fader = boolean (item, "prefader");
		send.enabled = !boolean (item, "bypass"); send.volume = number (item, "level", 1);
		send.pan = boolean (item, "lockPan") ? track.pan : number (item, "pan", .5);
		if (boolean (item, "lockPan")) {
			result.project.report.push_back ({Disposition::Transformed, track.name,
			                                   "Channel-linked send pan copied as a static value; later channel-pan edits will not follow"});
		}
		track.sends.push_back (send);
	}
}

void Reader::insert (Track& track, Node item)
{
	check ();
	/* A native FX slot contains a VST3 class UID and an embedded preset. The
	 * ghost data also exists when the plug-in is unavailable in Studio Pro, so
	 * it is metadata only; Ardour still checks the installed plug-in on import. */
	for (xmlAttr* a = item->properties; a; a = a->next) {
		std::string name (reinterpret_cast<const char*> (a->name));
		if (name != "name" && name != "expanded") {
			unsupported (track.name, "Native plug-in slot property is not yet supported: " + name); return;
		}
	}
	if (value (item, "name").compare (0, 2, "FX") != 0) {
		unsupported (track.name, "Native insert type is not yet supported"); return;
	}
	Node class_uid = child (item, "UID", "deviceClassID");
	Node data = child (item, "Attributes", "deviceData");
	Node ghost = child (item, "Attributes", "ghostData");
	Node info = child (ghost, "Attributes", "classInfo");
	Node presets = child (item, "Attributes", "Presets");
	Node path = child (item, "String", "presetPath");
	if (!class_uid || !data || !ghost || !info || !path || value (ghost, "presetType") != "vstpreset" ||
	    value (info, "category") != "AudioEffect" || value (info, "subCategory") != "VST3") {
		unsupported (track.name, "Native insert is not a supported VST3 audio effect with an embedded preset"); return;
	}
	attributes (data, {"x:id", "id", "name"}, track.name);
	attributes (ghost, {"x:id", "id", "presetType"}, track.name);
	attributes (info, {"x:id", "id", "classID", "name", "category", "subCategory"}, track.name);
	attributes (path, {"x:id", "id", "text"}, track.name);
	if (!children (info).empty () || !children (path).empty () || (presets && !children (presets).empty ())) {
		unsupported (track.name, "Native insert metadata contains unsupported content"); return;
	}
	if (presets) { attributes (presets, {"x:id", "id", "pname", "dirty"}, track.name); }
	for (Node part : children (data)) {
		if (!is (part, "UID") || id (part) != "uniqueID") {
			unsupported (track.name, "Native insert device data is not yet supported"); return;
		}
	}
	for (Node part : children (ghost)) {
		if ((is (part, "Attributes") && (id (part) == "classInfo" || id (part) == "IO"))) { continue; }
		unsupported (track.name, "Native insert ghost data is not yet supported"); return;
	}
	for (Node part : children (item)) {
		if ((is (part, "UID") && (id (part) == "uniqueID" || id (part) == "deviceClassID")) ||
		    (is (part, "Attributes") && (id (part) == "deviceData" || id (part) == "ghostData" || id (part) == "Presets")) ||
		    (is (part, "String") && id (part) == "presetPath")) { continue; }
		unsupported (track.name, "Native insert contains unsupported processing"); return;
	}
	std::string native_id = value (class_uid, "uid");
	if (vst3_class_id (native_id) != vst3_class_id (value (info, "classID"))) {
		throw ExchangeError ("Native insert class IDs disagree");
	}
	PluginDevice plugin;
	plugin.id = vst3_class_id (native_id);
	plugin.name = value (info, "name", value (data, "name"));
	plugin.role = "audioFX";
	plugin.state_path = value (path, "text");
	if (plugin.state_path.compare (0, 17, "Presets/Channels/") != 0 ||
	    plugin.state_path.size () < 27 || plugin.state_path.substr (plugin.state_path.size () - 10) != ".vstpreset") {
		throw ExchangeError ("Invalid native VST3 preset path");
	}
	if (!members.count (plugin.state_path)) { throw ExchangeError ("Missing native VST3 preset: " + plugin.state_path); }
	plugin.state = archive.read_text (plugin.state_path, 64 * 1024 * 1024);
	validate_vst3_state (plugin.id, plugin.state);
	track.plugins.push_back (std::move (plugin));
	result.project.report.push_back ({Disposition::Preserved, track.plugins.back ().name,
	                                  "Embedded native VST3 preset found; the matching installed plug-in is checked during import"});
}

void Reader::track (Node n)
{
	check (); Track t; t.id = value (n, "trackID"); t.name = value (n, "name"); t.color = color (value (n, "color"));
	attributes (n, {"mediaType", "tempoFollow", "materialMode", "trackNumber", "layersFollow", "followLayerName", "followGlobalTransposition", "version", "trackID", "parentFolder", "timeFormat", "name", "color", "delay", "transpose"}, t.name);
	t.parent = value (n, "parentFolder");
	std::string type = value (n, "mediaType");
	require (type == "Audio" || type == "Music", "Unsupported native track media type"); t.kind = type == "Audio" ? TrackKind::Audio : TrackKind::MIDI;
	unsigned format = integer (n, "timeFormat", 2); unit (format);
	std::string channel_id = value (child (n, "UID", "channelID"), "uid");
	auto ch = channels.find (channel_id); require (ch != channels.end (), "Missing native track channel");
	channel (t, ch->second);
	std::string speaker = value (child (n, "SpeakerSetup", "trackFormat"), "type", "Stereo");
	require (speaker == "Stereo" || speaker == "Mono", "Unsupported native speaker arrangement"); t.channels = speaker == "Mono" ? 1 : 2;
	if (number (n, "delay") || number (n, "transpose")) { unsupported (t.name, "Track delay or transposition is not yet supported"); }
	automation (child (n, "Attributes", "AutomationRegionList"), t, channel_id, unit (format));
	for (Node c : children (n)) {
		if (is (c, "List") && id (c) != "Events" && !children (c).empty ()) { unsupported (t.name, "Track layers or additional event lists are not yet supported"); }
	}
	for (Node e : children (child (n, "List", "Events"))) {
		bool midi = t.kind == TrackKind::MIDI;
		if (!is (e, midi ? "MusicPart" : "AudioEvent")) { unsupported (t.name, "Unsupported native event type"); continue; }
		Clip c; c.kind = midi ? ClipKind::MIDI : ClipKind::Audio; c.name = value (e, "name", t.name); c.color = color (value (e, "color"));
		attributes (e, {"clipID", "timeFormat", "start", "length", "name", "color", "offset", "mute", "speed", "transpose", "tune", "normalize", "modification", "velocity", "fadeIn", "fadeOut", "loop", "gain"}, c.name);
		c.position = Time (number (e, "start"), unit (integer (e, "timeFormat", format))); c.duration = number (e, "length");
		c.offset = Time (number (e, "offset"), midi ? TimeUnit::Beats : TimeUnit::Seconds); c.enabled = !boolean (e, "mute");
		std::string key = value (e, "clipID"); auto entry = pool.find (key); require (entry != pool.end (), "Native event references missing media");
		if (midi) { require (is (entry->second, "MusicClip"), "Wrong native MIDI media type"); notes (c, entry->second); }
		else { c.media = audio (key); c.gain = number (e, "gain", 1); }
		if (Node curve = child (e, "Attributes", "volumeCurve")) {
			attributes (curve, {"x:id", "id", "fadeIn.length", "fadeOut.length"}, c.name);
			for (Node part : children (curve)) { unsupported (c.name, "Native event volume-curve content is not yet supported: " + std::string (reinterpret_cast<const char*> (part->name))); }
			double fade_in = number (curve, "fadeIn.length"), fade_out = number (curve, "fadeOut.length");
			require (fade_in >= 0 && fade_out >= 0, "Invalid native event fade length");
			if (fade_in || fade_out) {
				if (midi || c.position.unit != TimeUnit::Seconds) { unsupported (c.name, "Native event fades on MIDI or musical-time audio require an explicit conversion"); }
				else {
					double duration = c.duration; // This path accepts only second-timed audio events.
					require (fade_in <= duration && fade_out <= duration, "Native event fade exceeds clip length");
					if (fade_in + fade_out > duration) { unsupported (c.name, "Overlapping native event fades need an explicit conversion"); }
					else {
						c.envelope.unit = TimeUnit::Seconds;
						c.envelope.points.push_back ({0, fade_in ? 0.0 : 1.0, true});
						if (fade_in) { c.envelope.points.push_back ({fade_in, 1, true}); }
						if (fade_out && duration - fade_out > fade_in) { c.envelope.points.push_back ({duration - fade_out, 1, true}); }
						if (fade_out) { c.envelope.points.push_back ({duration, 0, true}); }
						result.project.report.push_back ({Disposition::Transformed, c.name, "Native default event fade lengths converted to editable linear-amplitude clip envelope"});
					}
				}
			}
		}
		for (const char* attr : {"transpose", "tune", "normalize", "modification", "velocity", "fadeIn", "fadeOut", "loop"}) {
			if (number (e, attr)) { unsupported (c.name, "Native event processing is not yet supported: " + std::string (attr)); }
		}
		if (number (e, "speed", 1) != 1) { unsupported (c.name, "Event speed adjustment is not yet supported"); }
		if (midi && number (e, "gain", 1) != 1) { unsupported (c.name, "MIDI event gain requires instrument rendering"); }
		for (Node part : children (e)) {
			if (!is (part, "Attributes") || id (part) != "volumeCurve") { unsupported (c.name, "Event envelopes or other nested processing are not yet supported"); }
		}
		t.clips.push_back (c);
	}
	if (t.kind == TrackKind::Audio && !t.clips.empty () && integer (n, "tempoFollow") == 2 &&
	    std::any_of (result.project.tempos.begin (), result.project.tempos.end (), [&] (const TempoChange& tempo) { return tempo.bpm != result.project.tempo; })) {
		unsupported (t.name, "Audio following a changing tempo with time-stretching requires rendering");
	}
	result.project.tracks.push_back (t);
}

void Reader::folder (Node n)
{
	check ();
	Track t; t.id = value (n, "trackID"); t.name = value (n, "name"); t.color = color (value (n, "color"));
	t.parent = value (n, "parentFolder"); t.kind = TrackKind::Folder;
	attributes (n, {"version", "trackID", "parentFolder", "timeFormat", "name", "color"}, t.name);
	unit (integer (n, "timeFormat", 2));
	for (Node c : children (n)) {
		if (is (c, "Attributes") && id (c) == "AutomationRegionList") {
			if (!children (c).empty ()) { unsupported (t.name, "Folder automation is not yet supported"); }
		} else if (!is (c, "Attributes") || id (c) != "attributes" || !children (c).empty ()) {
			unsupported (t.name, "Native folder content is not yet supported");
		}
	}
	result.project.tracks.push_back (t);
}

void Reader::transport (Node context)
{
	/* Native transport positions are quarter-note beats; tempo is seconds
	 * per quarter note. Keep steps in musical time, including after a meter
	 * change. Curve types have a different, as yet unverified interpolation. */
	Node tempos = child (context, "TempoMap", "tempoMap");
	double previous = -1, previous_end = -1;
	for (Node n : children (tempos)) {
		check ();
		/* Studio Pro also saves the editor's normalized tempo envelope.
		 * TempoMapSegment already contains its resolved musical timing. */
		if (is (n, "Attributes") && id (n) == "envelope") { continue; }
		if (!is (n, "TempoMapSegment")) { unsupported ("Tempo map", "Unknown native tempo segment"); continue; }
		attributes (n, {"start", "end", "tempo", "curveType"}, "Tempo map");
		double start = number (n, "start"), seconds_per_beat = number (n, "tempo");
		require (start >= 0 && start > previous && (previous >= 0 || start == 0), "Invalid or unordered native tempo map");
		require (seconds_per_beat > 0, "Invalid native song tempo");
		if (previous_end >= 0) { require (std::abs (previous_end - start) <= 1e-9, "Native tempo segments have a gap or overlap"); }
		previous_end = number (n, "end", -1);
		require (value (n, "end").empty () || previous_end > start, "Invalid native tempo segment end");
		if (integer (n, "curveType") != 0 || !children (n).empty ()) { unsupported ("Tempo map", "Native tempo curves are not yet supported"); }
		result.project.tempos.push_back ({Time (start), 60 / seconds_per_beat, false});
		require (result.project.tempos.size () <= 100000, "Native tempo map exceeds the event limit");
		previous = start;
	}
	require (!result.project.tempos.empty (), "Native song has no tempo");
	result.project.tempo = result.project.tempos.front ().bpm;
	/* A final segment normally uses a large finite sentinel for its end.
	 * The final tempo continues beyond the arrangement. */
	Node meters = child (context, "TimeSignatureMap", "timeSignatureMap");
	previous = -1;
	for (Node n : children (meters)) {
		check ();
		if (!is (n, "TimeSignatureMapSegment")) { unsupported ("Meter map", "Unknown native meter segment"); continue; }
		attributes (n, {"start", "numerator", "denominator", "anchor", "type"}, "Meter map");
		if (integer (n, "type") != 0) { unsupported ("Meter map", "Native meter type is not yet supported"); }
		double start = number (n, "start");
		require (start >= 0 && start > previous && (previous >= 0 || start == 0), "Invalid or unordered native meter map");
		if (!children (n).empty ()) { unsupported ("Meter map", "Native meter segment contains additional data"); }
		result.project.meters.push_back ({Time (start), integer (n, "numerator"), integer (n, "denominator")});
		require (result.project.meters.size () <= 100000, "Native meter map exceeds the event limit");
		previous = start;
	}
	require (!result.project.meters.empty (), "Native song has no meter");
	result.project.numerator = result.project.meters.front ().numerator;
	result.project.denominator = result.project.meters.front ().denominator;
}

StudioOneSongData Reader::read ()
{
	Node meta = document ("metainfo.xml", "MetaInformation"); std::map<std::string, std::string> metadata;
	for (Node n : children (meta)) { if (is (n, "Attribute")) { require (metadata.emplace (value (n, "id"), value (n, "value")).second, "Duplicate native song metadata"); } }
	require (metadata["Document:MimeType"] == "application/x.presonus-song", "This file is not a Studio One song");
	std::string format_version = metadata["Document:FormatVersion"];
	require (format_version == "7" || format_version == "9", "This Studio One native format version has not yet been validated (supported: 7, 9)");
	result.project.application = "Studio One / Studio Pro"; result.project.application_version = metadata["Document:Generator"];
	result.title = metadata["Document:Title"];
	Node song = document ("Song/song.xml", "Song"); Node root_node = child (song, "Attributes", "Root"); require (root_node, "Native song has no arrangement");
	Node context = child (root_node, "Attributes", "timeContext"); require (context, "Native song has no time context");
	result.sample_rate = integer (context, "sampleRate", 48000);
	require (result.sample_rate >= 8000 && result.sample_rate <= 768000, "Invalid native song sample rate");
	transport (context);
	Node media = document ("Song/mediapool.xml", "MediaPool");
	std::string original = url_path (value (child (media, "Attributes", "documentPath"), "url"));
	File original_file = file (original); File parent (g_file_get_parent (original_file.get ()), g_object_unref);
	require (parent.get (), "Native song original folder is missing"); original_root = path (parent.get ()); index_pool (media);
	index_channels (document ("Devices/audiomixer.xml", "AudioMixer"));
	if (members.count ("Devices/musictrackdevice.xml")) { index_channels (document ("Devices/musictrackdevice.xml", "MusicTrackDevice")); }
	if (members.count ("Devices/audiosynthfolder.xml")) {
		for (Node n : children (document ("Devices/audiosynthfolder.xml", "AudioSynthFolder"))) {
			if (id (n) != "Presets") { unsupported ("Instruments", "Native instrument plug-ins are not yet supported"); }
		}
	}
	for (Node n : children (child (root_node, "List", "Tracks"))) {
		if (is (n, "MediaTrack")) { track (n); }
		else if (is (n, "FolderTrack")) { folder (n); }
		else if (is (n, "MarkerTrack")) {
			unsigned format = integer (n, "timeFormat", 2); bool start = false, end = false;
			for (Node m : children (n)) { if (is (m, "MarkerEvent")) {
				Time time (number (m, "start"), unit (integer (m, "timeFormat", format)));
				require (time.value >= 0, "Invalid native marker time");
				unsigned type = integer (m, "markerType");
				if (type == 2) { require (!start, "Duplicate song start marker"); result.start = time; start = true; }
				else if (type == 3) { require (!end, "Duplicate song end marker"); result.end = time; end = true; }
				else { result.markers.push_back ({value (m, "name"), time}); }
				if (boolean (m, "markerStop")) { unsupported (value (m, "name"), "Stop-at-marker behavior is not yet supported"); }
			} }
			result.has_range = start && end;
		} else if (is (n, "ChordTrack") || is (n, "ArrangerTrack") || is (n, "LyricsTrack") || is (n, "VideoTrack")) {
			for (Node c : children (n)) {
				if (is (c, "UID") || (is (c, "Attributes") && (id (c) == "attributes" || (id (c) == "lyrics" && children (c).empty ())))) { continue; }
				unsupported (reinterpret_cast<const char*> (n->name), "This native track contains unsupported content");
			}
		} else { unsupported (value (n, "name"), "Native track type is not yet supported: " + std::string (reinterpret_cast<const char*> (n->name))); }
	}
	for (auto const& pair : channels) { if (is (pair.second, "AudioGroupChannel") || is (pair.second, "AudioEffectChannel")) {
		Track bus; bus.id = pair.first; bus.kind = TrackKind::Bus; bus.name = value (pair.second, "label", "Bus");
		try { bus.color = color (value (pair.second, "color")); }
		catch (const ExchangeError&) { result.project.report.push_back ({Disposition::Transformed, bus.name, "Invalid native bus color omitted"}); }
		channel (bus, pair.second);
		std::string speaker = value (child (pair.second, "SpeakerSetup", "speakerType"), "type", "Stereo");
		require (speaker == "Stereo", "Unsupported native bus speaker arrangement");
		if (is (pair.second, "AudioEffectChannel")) {
			result.project.report.push_back ({Disposition::Transformed, bus.name, "Native FX channel represented as an audio bus"});
		}
		result.project.tracks.push_back (bus);
	} }
	for (auto const& pair : channels) { if (is (pair.second, "AudioOutputChannel")) {
		Track master; master.id = pair.first; master.kind = TrackKind::Bus; master.master = true; master.name = value (pair.second, "label", "Master"); channel (master, pair.second);
		result.project.tracks.push_back (master);
	} }
	/* Native channel routing references channel IDs, while tracks use trackID. */
	std::map<std::string, std::string> destinations;
	for (Node n : children (child (root_node, "List", "Tracks"))) { if (is (n, "MediaTrack")) { destinations[value (child (n, "UID", "channelID"), "uid")] = value (n, "trackID"); } }
	for (auto& t : result.project.tracks) {
		auto i = destinations.find (t.destination); if (i != destinations.end ()) { t.destination = i->second; }
		for (auto& send : t.sends) { auto target = destinations.find (send.destination); if (target != destinations.end ()) { send.destination = target->second; } }
	}
	if (result.has_range) {
		TimeMap timing (result.project);
		require (timing.convert (result.end, TimeUnit::Seconds) > timing.convert (result.start, TimeUnit::Seconds), "Invalid native song boundaries");
	}
	result.project.markers = result.markers;
	validate_project (result.project);
	if (format_version == "7") {
		result.project.report.push_back ({Disposition::Transformed, "Native Studio One song",
		                                   "Studio One format 7 imported; native re-export writes format 9"});
	}
	result.project.report.push_back ({Disposition::Preserved, "Native Studio One song", "Audio, MIDI notes, clip positions and gain, track names, colors, static mixer controls, audio-bus routing, ordinary sends and tempo/meter steps read directly from the .song file"});
	return result;
}

} // namespace

StudioOneSongData StudioOneSong::read (const std::string& filename, CancelCheck cancel) { return Reader (filename, cancel).read (); }

} }
