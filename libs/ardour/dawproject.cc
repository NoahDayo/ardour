/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/dawproject.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <iterator>
#include <memory>
#include <set>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {

typedef xmlNode* Node;
typedef std::unique_ptr<xmlDoc, void (*) (xmlDoc*)> Document;

bool is (Node node, const char* name) { return xmlStrEqual (node->name, BAD_CAST name); }
bool has (Node node, const char* name) { return xmlHasProp (node, BAD_CAST name) != NULL; }

std::string attr (Node node, const char* name, const std::string& fallback = "")
{
	xmlChar* raw = xmlGetProp (node, BAD_CAST name);
	if (!raw) { return fallback; }
	std::string result (reinterpret_cast<const char*> (raw));
	xmlFree (raw);
	return result;
}

std::string required (Node node, const char* name)
{
	if (!has (node, name)) { throw ExchangeError ("Missing " + std::string (name) + " on " + reinterpret_cast<const char*> (node->name)); }
	return attr (node, name);
}

std::vector<Node> children (Node node)
{
	std::vector<Node> result;
	for (Node child = node->children; child; child = child->next) {
		if (child->type == XML_ELEMENT_NODE) { result.push_back (child); }
	}
	return result;
}

Node child (Node node, const char* name)
{
	Node found = NULL;
	for (Node item : children (node)) {
		if (is (item, name)) {
			if (found) { throw ExchangeError ("Duplicate " + std::string (name) + " element"); }
			found = item;
		}
	}
	return found;
}

double number (Node node, const char* name, double fallback = 0, bool mandatory = false)
{
	if (!has (node, name)) {
		if (mandatory) { required (node, name); }
		return fallback;
	}
	std::string value = attr (node, name);
	char* end = NULL;
	errno = 0;
	double result = g_ascii_strtod (value.c_str (), &end);
	bool converted = end != value.c_str ();
	while (g_ascii_isspace (*end)) { ++end; }
	if (errno == ERANGE || !converted || *end || !std::isfinite (result)) {
		throw ExchangeError ("Invalid numeric attribute: " + std::string (name));
	}
	return result;
}

unsigned int integer (Node node, const char* name, unsigned int fallback = 0, bool mandatory = false)
{
	double value = number (node, name, fallback, mandatory);
	if (value < 0 || value > std::numeric_limits<unsigned int>::max () || std::floor (value) != value) {
		throw ExchangeError ("Invalid integer attribute: " + std::string (name));
	}
	return static_cast<unsigned int> (value);
}

bool boolean (Node node, const char* name, bool fallback)
{
	if (!has (node, name)) { return fallback; }
	std::string value = attr (node, name);
	if (value == "true" || value == "1") { return true; }
	if (value == "false" || value == "0") { return false; }
	throw ExchangeError ("Invalid boolean attribute: " + std::string (name));
}

TimeUnit time_unit (Node node, const char* attribute, TimeUnit fallback)
{
	if (!has (node, attribute)) { return fallback; }
	std::string value = attr (node, attribute);
	if (value == "beats") { return TimeUnit::Beats; }
	if (value == "seconds") { return TimeUnit::Seconds; }
	throw ExchangeError ("Invalid time unit: " + value);
}

void reject_dtd (void* context, const xmlChar*, const xmlChar*, const xmlChar*)
{
	xmlStopParser (static_cast<xmlParserCtxt*> (context));
}

Document parse_document (const std::string& text)
{
	if (text.empty () || text.size () > 16 * 1024 * 1024) { throw ExchangeError ("Invalid project XML size"); }
	std::unique_ptr<xmlParserCtxt, void (*) (xmlParserCtxt*)> context (xmlNewParserCtxt (), xmlFreeParserCtxt);
	if (!context) { throw ExchangeError ("Cannot allocate XML parser"); }
	/* Stop at any DOCTYPE, before loading or expanding its entities. Do not
	 * change libxml's process-global loader or locale in the audio application. */
	context->sax->internalSubset = reject_dtd;
	context->sax->externalSubset = reject_dtd;
	Document doc (xmlCtxtReadMemory (context.get (), text.data (), text.size (), "project.xml", "UTF-8",
	                               XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
	if (!doc || doc->intSubset || doc->extSubset) { throw ExchangeError ("Invalid project XML; DTDs and external entities are not allowed"); }
	return doc;
}

struct Parser {
	Project project;
	std::unique_ptr<TimeMap> timing;
	std::map<std::string, Node> ids;
	std::map<std::string, size_t> track_indices;
	std::map<std::string, std::string> channel_tracks;
	std::map<Node, std::pair<std::string, bool>> automation_targets;
	std::set<std::string> model_ids;
	std::set<std::string> infer_track_kind;
	size_t generated = 0;
	size_t node_count = 0;
	size_t expanded_objects = 0;

	void expansion ()
	{
		if (++expanded_objects > 250000) { throw ExchangeError ("Project exceeds the expanded object limit"); }
	}

	void diagnostic (Node node, const std::string& message, Disposition disposition = Disposition::Unsupported)
	{
		if (project.report.size () >= 10000) { throw ExchangeError ("Project exceeds the diagnostic limit"); }
		std::string object = reinterpret_cast<const char*> (node->name);
		if (has (node, "id")) { object += " " + attr (node, "id"); }
		project.report.push_back ({disposition, object, message});
	}

	void attributes (Node node, const std::set<std::string>& known)
	{
		for (xmlAttr* prop = node->properties; prop; prop = prop->next) {
			std::string name (reinterpret_cast<const char*> (prop->name));
			if (!known.count (name)) { diagnostic (node, "Unsupported attribute: " + name); }
		}
	}

	std::string color (Node node)
	{
		std::string value = attr (node, "color");
		if (value.size () == 9 && value[0] == '#') {
			if (g_ascii_strcasecmp (value.c_str () + 7, "ff")) {
				diagnostic (node, "Color transparency cannot be represented by an RGB project color");
			}
			value.resize (7);
		}
		if (!value.empty () && (value.size () != 7 || value[0] != '#' ||
		    value.find_first_not_of ("0123456789abcdefABCDEF", 1) != std::string::npos)) {
			throw ExchangeError ("Invalid project color");
		}
		return value;
	}

	void points (Node node, TimeUnit unit, bool arrangement = true, Automation* expression = nullptr)
	{
		attributes (node, {"id", "name", "color", "timeUnit", "unit"});
		Node target = child (node, "Target");
		if (!target) { throw ExchangeError ("Automation is missing its target"); }
		attributes (target, {"parameter", "expression", "channel", "key", "controller"});
		if (has (target, "parameter") == has (target, "expression")) { throw ExchangeError ("Invalid automation target"); }
		if (has (target, "parameter") && !ids.count (attr (target, "parameter"))) { throw ExchangeError ("Unknown automation target"); }
		if (!children (target).empty ()) { throw ExchangeError ("Invalid automation target content"); }
		Node parameter = has (target, "parameter") ? ids.at (attr (target, "parameter")) : NULL;
		auto mapped = automation_targets.find (parameter);
		std::vector<Node> values;
		for (Node item : children (node)) {
			if (!is (item, "Target")) { values.push_back (item); }
		}
		if (values.empty ()) { return; }
		bool clip_gain = !arrangement && expression && attr (target, "expression") == "gain";
		if ((!clip_gain && (!arrangement || mapped == automation_targets.end ())) || has (target, "channel") || has (target, "key") || has (target, "controller")) {
			diagnostic (node, "Automation target is not yet converted"); return;
		}
		bool pan = !clip_gain && mapped->second.second;
		Automation& curve = clip_gain ? *expression : (pan ? project.tracks.at (track_indices.at (mapped->second.first)).pan_automation
		                                                : project.tracks.at (track_indices.at (mapped->second.first)).volume_automation);
		if (!curve.points.empty ()) { throw ExchangeError ("Multiple automation timelines target the same mixer parameter"); }
		curve.unit = unit;
		std::string value_unit = attr (node, "unit", clip_gain ? "linear" : attr (parameter, "unit"));
		bool decibel = !pan && value_unit == "decibel";
		if ((pan && value_unit != "normalized") || (!pan && value_unit != "linear" && !decibel)) { throw ExchangeError ("Unsupported mixer automation unit"); }
		double previous_time = -1, previous_value = 0;
		bool previous_linear = false;
		for (Node point : values) {
			expansion ();
			if (!is (point, "RealPoint") || !children (point).empty ()) { throw ExchangeError ("Mixer automation requires real-valued points"); }
			attributes (point, {"time", "value", "interpolation"});
			double at = number (point, "time", 0, true), raw = number (point, "value", 0, true);
			if (at < 0 || at <= previous_time) { throw ExchangeError ("Invalid or unordered mixer automation"); }
			std::string mode = attr (point, "interpolation", "hold");
			if (mode != "hold" && mode != "linear") { throw ExchangeError ("Unsupported mixer automation interpolation"); }
			double value = decibel ? std::pow (10, raw / 20) : raw;
			if (!std::isfinite (value) || value < 0 || (pan && value > 1)) { throw ExchangeError ("Mixer automation value is out of range"); }
			if (decibel && previous_linear) {
				append_automation_segment (curve, at, value, [&] (double t) {
					return std::pow (10, (previous_value + (raw - previous_value) * ((t - previous_time) / (at - previous_time))) / 20);
				});
			} else { curve.points.push_back ({at, value, mode == "linear"}); }
			curve.points.back ().linear = mode == "linear";
			previous_time = at; previous_value = raw; previous_linear = mode == "linear";
		}
		if (decibel) { diagnostic (node, "Decibel automation converted to linear amplitude; curved segments sampled within 0.0001 amplitude before session rounding", Disposition::Transformed); }
	}

	void index (Node node, size_t depth = 0)
	{
		if (++node_count > 250000 || depth > 128) { throw ExchangeError ("Project XML exceeds object or nesting limits"); }
		if (node->ns) { throw ExchangeError ("Unsupported project XML namespace"); }
		for (xmlAttr* prop = node->properties; prop; prop = prop->next) {
			if (prop->ns) { throw ExchangeError ("Unsupported project attribute namespace"); }
		}
		for (Node item = node->children; item; item = item->next) {
			if ((item->type == XML_TEXT_NODE || item->type == XML_CDATA_SECTION_NODE) && !xmlIsBlankNode (item)) {
				throw ExchangeError ("Unexpected text in project XML");
			}
		}
		if ((is (node, "Application") || is (node, "Tempo") || is (node, "TimeSignature") ||
		     is (node, "Volume") || is (node, "Pan") || is (node, "Mute") || is (node, "Enable") || is (node, "File")) && !children (node).empty ()) {
			throw ExchangeError ("Unexpected child element in project parameter");
		}
		if (has (node, "id")) {
			std::string id = attr (node, "id");
			if (id.empty () || xmlValidateNCName (BAD_CAST id.c_str (), 0) || !ids.emplace (id, node).second) {
				throw ExchangeError ("Duplicate or invalid project ID: " + id);
			}
		}
		for (Node item : children (node)) { index (item, depth + 1); }
	}

	std::string model_id (Node node)
	{
		std::string id = attr (node, "id");
		if (id.empty ()) {
			do { id = "imported_" + std::to_string (++generated); } while (ids.count (id) || model_ids.count (id));
		}
		if (!model_ids.insert (id).second) { throw ExchangeError ("Duplicate exchange object ID"); }
		return id;
	}

	void channel (Node node, Track& track)
	{
		attributes (node, {"id", "name", "color", "audioChannels", "destination", "role", "solo"});
		if ((has (node, "name") && attr (node, "name") != track.name) ||
		    (has (node, "color") && color (node) != track.color)) {
			diagnostic (node, "A separate channel name or color cannot be represented by the track metadata");
		}
		std::string role = attr (node, "role", "regular");
		if (role == "master" || role == "effect") {
			track.kind = TrackKind::Bus;
			track.master = role == "master";
		} else if (role != "regular") { throw ExchangeError ("Unsupported mixer role: " + role); }
		if (has (node, "id")) { channel_tracks.emplace (attr (node, "id"), track.id); }
		track.destination = attr (node, "destination");
		track.channels = integer (node, "audioChannels", 2);
		track.solo = boolean (node, "solo", false);
		for (Node item : children (node)) {
			if (is (item, "Volume") || is (item, "Pan")) {
				automation_targets[item] = {track.id, is (item, "Pan")};
				attributes (item, {"id", "name", "value", "min", "max", "unit"});
				std::string unit = required (item, "unit");
				double value = number (item, "value", is (item, "Volume") ? 1 : 0.5);
				if (is (item, "Volume")) {
					if (unit == "linear") { track.volume = value; }
					else if (unit == "decibel") { track.volume = std::pow (10, value / 20); }
					else { throw ExchangeError ("Unsupported channel volume unit: " + unit); }
				} else {
					if (unit != "normalized") { throw ExchangeError ("Unsupported pan unit: " + unit); }
					track.pan = value;
				}
			} else if (is (item, "Mute")) {
				attributes (item, {"id", "name", "value"});
				track.mute = boolean (item, "value", false);
			} else if (is (item, "Sends")) {
				attributes (item, {});
				for (Node entry : children (item)) {
					if (!is (entry, "Send")) { diagnostic (entry, "Unknown send element"); continue; }
					attributes (entry, {"id", "destination", "type"});
					Send send;
					send.destination = required (entry, "destination");
					std::string type = required (entry, "type");
					if (type != "pre" && type != "post") { throw ExchangeError ("Invalid send placement"); }
					send.pre_fader = type == "pre";
					Node volume = child (entry, "Volume");
					if (!volume) { throw ExchangeError ("Send has no volume"); }
					attributes (volume, {"id", "value", "unit"});
					std::string unit = required (volume, "unit");
					double value = number (volume, "value", 1);
					if (unit == "linear") { send.volume = value; }
					else if (unit == "decibel") { send.volume = std::pow (10, value / 20); }
					else { throw ExchangeError ("Unsupported send volume unit: " + unit); }
					if (Node enabled = child (entry, "Enable")) {
						attributes (enabled, {"id", "value"});
						send.enabled = boolean (enabled, "value", true);
					}
					if (Node pan = child (entry, "Pan")) {
						attributes (pan, {"id", "unit", "value"});
						if (required (pan, "unit") != "normalized") { throw ExchangeError ("Unsupported send pan unit"); }
						send.pan = number (pan, "value", .5);
					}
					for (Node parameter : children (entry)) {
						if (!is (parameter, "Enable") && !is (parameter, "Pan") && !is (parameter, "Volume")) {
							diagnostic (parameter, "Unknown send parameter");
						}
					}
					track.sends.push_back (send);
				}
			} else if (is (item, "Devices")) {
				attributes (item, {});
				for (Node device : children (item)) {
					if (!is (device, "Vst3Plugin")) { diagnostic (device, "Device requires a compatible plugin or rendered fallback"); continue; }
					attributes (device, {"id", "name", "color", "comment", "deviceID", "deviceName", "deviceRole", "deviceVendor", "loaded", "pluginVersion"});
					if (!boolean (device, "loaded", true) || !has (device, "deviceID")) {
						diagnostic (device, "Unloaded or unidentified VST3 plugin requires a rendered fallback"); continue;
					}
					PluginDevice plugin;
					plugin.id = canonical_vst3_id (attr (device, "deviceID"));
					plugin.name = attr (device, "deviceName");
					plugin.role = attr (device, "deviceRole");
					if (plugin.role != "audioFX" && plugin.role != "instrument") {
						diagnostic (device, "Unsupported VST3 role requires a rendered fallback"); continue;
					}
					Node state = child (device, "State");
					if (!state || boolean (state, "external", false)) {
						diagnostic (device, "VST3 plugin needs embedded preset state"); continue;
					}
					attributes (state, {"path", "external"});
					if (!children (state).empty ()) { throw ExchangeError ("VST3 preset reference has unexpected content"); }
					plugin.state_path = required (state, "path");
					if (Node enabled = child (device, "Enabled")) {
						attributes (enabled, {"id", "name", "value"});
						if (!children (enabled).empty ()) { throw ExchangeError ("VST3 enabled parameter has unexpected content"); }
						plugin.enabled = boolean (enabled, "value", true);
					}
					for (Node parameter : children (device)) {
						if (is (parameter, "Parameters")) {
							attributes (parameter, {});
							if (!children (parameter).empty ()) { diagnostic (parameter, "Independent VST3 parameter values or automation require verification against preset state"); }
						} else if (!is (parameter, "Enabled") && !is (parameter, "State")) {
							diagnostic (parameter, "Unknown VST3 device element");
						}
					}
					track.plugins.push_back (std::move (plugin));
				}
			} else if (!children (item).empty ()) {
				diagnostic (item, "Channel processing or routing is not yet converted");
			} else if (!is (item, "Sends")) {
				diagnostic (item, "Unknown channel element");
			} else {
				attributes (item, {});
			}
		}
		child (node, "Volume"); child (node, "Pan"); child (node, "Mute"); child (node, "Devices");
	}

	void structure (Node node, const std::string& parent = "")
	{
		Track track;
		track.id = model_id (node);
		track.name = attr (node, "name");
		track.color = color (node);
		track.parent = parent;
		if (is (node, "Channel")) {
			track.kind = TrackKind::Bus;
			channel (node, track);
			if (attr (node, "role", "regular") == "regular") {
				diagnostic (node, "Standalone regular channels require an explicit bus-role mapping");
			}
		} else {
			attributes (node, {"id", "name", "color", "contentType", "loaded"});
			if (!boolean (node, "loaded", true)) { diagnostic (node, "Unloaded track content requires a rendered fallback"); }
			std::string type = attr (node, "contentType");
			if (type == "notes") { track.kind = TrackKind::MIDI; }
			else if (type.empty () || type == "tracks") { track.kind = TrackKind::Folder; }
			else if (!type.empty () && type != "audio") { throw ExchangeError ("Unsupported track content type: " + type); }
			bool folder = false;
			for (Node item : children (node)) {
				if (is (item, "Track")) { folder = true; }
				else if (!is (item, "Channel")) { diagnostic (item, "Unknown track element"); }
			}
			if (folder) { track.kind = TrackKind::Folder; }
			if (Node c = child (node, "Channel")) { channel (c, track); }
			if (!folder && type.empty () && track.kind != TrackKind::Bus) { infer_track_kind.insert (track.id); }
		}
		track_indices.emplace (track.id, project.tracks.size ());
		project.tracks.push_back (track);
		for (Node item : children (node)) {
			if (is (item, "Track")) { structure (item, track.id); }
		}
	}

	std::string audio (Node node)
	{
		attributes (node, {"id", "timeUnit", "channels", "sampleRate", "duration"});
		if (time_unit (node, "timeUnit", TimeUnit::Seconds) != TimeUnit::Seconds) { throw ExchangeError ("Audio timeline must use seconds"); }
		Node file = child (node, "File");
		if (!file) { throw ExchangeError ("Audio is missing its File reference"); }
		attributes (file, {"path", "external"});
		if (boolean (file, "external", false)) { throw ExchangeError ("External media must be embedded before project exchange"); }
		Media media;
		media.path = required (file, "path");
		media.sample_rate = integer (node, "sampleRate", 0, true);
		media.channels = integer (node, "channels", 0, true);
		media.duration = number (node, "duration", 0, true);
		for (Node item : children (node)) { if (!is (item, "File")) { diagnostic (item, "Unknown audio element"); } }
		for (auto const& existing : project.media) {
			if (existing.path == media.path) {
				if (existing.sample_rate != media.sample_rate || existing.channels != media.channels || existing.duration != media.duration) {
					throw ExchangeError ("Inconsistent descriptions of the same audio file");
				}
				return existing.id;
			}
		}
		media.id = model_id (node);
		project.media.push_back (media);
		return media.id;
	}

	std::vector<Clip> clip (Node, TimeUnit, std::set<Node>&, Time origin = Time ());
	void timeline (Node, TimeUnit, const std::string&);
	Project parse (Node);
};

std::vector<Clip>
Parser::clip (Node node, TimeUnit parent_unit, std::set<Node>& active, Time origin)
{
	expansion ();
	if (active.size () >= 128 || !active.insert (node).second) { throw ExchangeError ("Cyclic or excessively deep clip reference"); }
	attributes (node, {"name", "color", "time", "duration", "contentTimeUnit", "playStart", "playStop", "enable", "reference",
	                  "loopStart", "loopEnd", "fadeInTime", "fadeOutTime", "fadeTimeUnit"});
	if (has (node, "loopStart") || has (node, "loopEnd")) { throw ExchangeError ("Looped clips require explicit expansion before exchange"); }
	double fade_in = number (node, "fadeInTime"), fade_out = number (node, "fadeOutTime");
	if (fade_out < 0) { throw ExchangeError ("Negative fade-out duration"); }
	if ((fade_in || fade_out) && !has (node, "fadeTimeUnit")) { throw ExchangeError ("Nonzero clip fades need an explicit fadeTimeUnit"); }
	TimeUnit fade_unit = time_unit (node, "fadeTimeUnit", parent_unit);
	Clip outer;
	outer.name = attr (node, "name");
	outer.color = color (node);
	double local_position = number (node, "time", 0, true);
	outer.position = Time (timing->convert (origin, parent_unit) + local_position, parent_unit);
	outer.enabled = boolean (node, "enable", true);
	TimeUnit content_unit = time_unit (node, "contentTimeUnit", parent_unit);
	outer.offset = Time (number (node, "playStart"), content_unit);
	if (outer.offset.value < 0 || local_position < 0) { throw ExchangeError ("Negative clip time is not supported"); }
	if (has (node, "duration")) {
		outer.duration = number (node, "duration");
	} else {
		double stop = number (node, "playStop", 0, true);
		outer.duration = timing->duration (Time (stop - outer.offset.value, content_unit), outer.position, parent_unit);
	}
	if (outer.duration <= 0) { throw ExchangeError ("Clip duration must be positive"); }
	if (has (node, "playStop")) {
		double span = number (node, "playStop") - outer.offset.value;
		if (span <= 0) { throw ExchangeError ("Clip playStop must follow playStart"); }
		double maximum = timing->duration (Time (span, content_unit), outer.position, parent_unit);
		if (maximum < outer.duration) {
			outer.duration = maximum;
			diagnostic (node, "Clipped the region to its content playback stop", Disposition::Transformed);
		}
	}
	if (fade_in < 0) {
		Time earlier (timing->convert (outer.position, fade_unit) + fade_in, fade_unit);
		double extension = timing->convert (outer.position, parent_unit) - timing->convert (earlier, parent_unit);
		outer.offset.value -= timing->duration (Time (extension, parent_unit), earlier, outer.offset.unit);
		outer.position = Time (timing->convert (earlier, parent_unit), parent_unit);
		outer.duration += extension;
		if (outer.position.value < 0 || outer.offset.value < 0) { throw ExchangeError ("Crossfade extends before available timeline or source media"); }
		fade_in = -fade_in;
		diagnostic (node, "Expanded negative fade-in using the preceding source handle", Disposition::Transformed);
	}
	Time source_offset = outer.offset;
	double clip_start = timing->convert (outer.position, TimeUnit::Seconds);
	double clip_length = timing->duration (Time (outer.duration, parent_unit), outer.position, TimeUnit::Seconds);
	Automation expression;
	std::vector<Node> contents = children (node);
	Node content = NULL;
	if (has (node, "reference")) {
		if (!contents.empty ()) { throw ExchangeError ("Clip has both content and reference"); }
		auto found = ids.find (attr (node, "reference"));
		if (found == ids.end ()) { throw ExchangeError ("Unknown clip reference"); }
		content = found->second;
	} else {
		if (contents.size () != 1) { throw ExchangeError ("Clip must have exactly one content timeline"); }
		content = contents.front ();
	}
	/* Studio Pro wraps note content in one or more parallel-timeline
	 * containers, even when there is only one playable lane. */
	while (is (content, "Lanes")) {
		attributes (content, {"id", "timeUnit"});
		content_unit = time_unit (content, "timeUnit", content_unit);
		Node playable = NULL;
		for (Node item : children (content)) {
			if (is (item, "Points")) { points (item, time_unit (item, "timeUnit", content_unit), false, &expression); continue; }
			if (playable) { throw ExchangeError ("Parallel clip content lanes are not yet supported"); }
			playable = item;
		}
		if (!playable) { throw ExchangeError ("Clip lanes contain no playable content"); }
		content = playable;
	}
	std::vector<Clip> result;
	if (is (content, "Audio")) {
		outer.kind = ClipKind::Audio;
		outer.media = audio (content);
		outer.offset = Time (timing->content (outer.offset, outer.position, outer.offset, TimeUnit::Seconds), TimeUnit::Seconds);
		result.push_back (outer);
	} else if (is (content, "Notes")) {
		attributes (content, {"id", "timeUnit"});
		outer.kind = ClipKind::MIDI;
		TimeUnit unit = time_unit (content, "timeUnit", content_unit);
		outer.offset = Time (timing->content (outer.offset, outer.position, outer.offset, unit), unit);
		for (Node item : children (content)) {
			if (!is (item, "Note")) { diagnostic (item, "Unknown notes element"); continue; }
			expansion ();
			attributes (item, {"time", "duration", "channel", "key", "vel", "rel"});
			Note note;
			note.time = number (item, "time", 0, true);
			note.duration = number (item, "duration", 0, true);
			unsigned int key = integer (item, "key", 0, true);
			unsigned int channel = integer (item, "channel");
			if (key > 127 || channel > 15) { throw ExchangeError ("Invalid MIDI key or channel"); }
			note.key = key;
			note.channel = channel;
			note.velocity = number (item, "vel", 1);
			note.release_velocity = number (item, "rel", 0.5);
			if (!children (item).empty ()) { diagnostic (item, "Per-note expression is not yet converted"); }
			outer.notes.push_back (note);
		}
		result.push_back (outer);
	} else if (is (content, "Clips")) {
		attributes (content, {"id", "timeUnit"});
		TimeUnit unit = time_unit (content, "timeUnit", content_unit);
		double start = timing->convert (outer.position, TimeUnit::Seconds);
		double stop = start + timing->duration (Time (outer.duration, parent_unit), outer.position, TimeUnit::Seconds);
		Time child_origin (timing->convert (outer.position, outer.offset.unit) - outer.offset.value, outer.offset.unit);
		for (Node item : children (content)) {
			if (!is (item, "Clip")) { diagnostic (item, "Unknown nested clips element"); continue; }
			for (Clip& inner : clip (item, unit, active, child_origin)) {
				double inner_start = timing->convert (inner.position, TimeUnit::Seconds);
				double inner_stop = inner_start + timing->duration (Time (inner.duration, inner.position.unit), inner.position, TimeUnit::Seconds);
				double begin = std::max (start, inner_start);
				double end = std::min (stop, inner_stop);
				if (end <= begin) { continue; }
				inner.envelope = slice_automation (inner.envelope, begin - inner_start, end - inner_start);
				inner.offset.value += timing->duration (Time (begin - inner_start, TimeUnit::Seconds), inner.position, inner.offset.unit);
				inner.position = Time (timing->convert (Time (begin, TimeUnit::Seconds), parent_unit), parent_unit);
				inner.duration = timing->duration (Time (end - begin, TimeUnit::Seconds), inner.position, parent_unit);
				inner.enabled = inner.enabled && outer.enabled;
				if (inner.name.empty ()) { inner.name = outer.name; }
				if (inner.color.empty ()) { inner.color = outer.color; }
				result.push_back (std::move (inner));
			}
		}
		diagnostic (node, "Flattened nested clips while preserving audible ranges and source offsets", Disposition::Transformed);
	} else {
		throw ExchangeError ("Unsupported clip content: " + std::string (reinterpret_cast<const char*> (content->name)));
	}
	Automation envelope; envelope.unit = TimeUnit::Seconds;
	if (!expression.points.empty ()) {
		Time origin (timing->convert (outer.position, source_offset.unit) - source_offset.value, source_offset.unit);
		for (auto& p : expression.points) { p.time += timing->convert (origin, expression.unit); }
		envelope = slice_automation (convert_automation (expression, *timing, TimeUnit::Seconds), clip_start, clip_start + clip_length);
	}
	for (auto& value : result) {
		if (value.kind != ClipKind::Audio && (fade_in || fade_out || !envelope.points.empty ())) {
			diagnostic (node, "Audio gain/fades on MIDI content require instrument rendering"); continue;
		}
		if (value.kind != ClipKind::Audio) { continue; }
		auto source = std::find_if (project.media.begin (), project.media.end (), [&] (const Media& m) { return m.id == value.media; });
		double quantum = 1.0 / source->sample_rate;
		Automation combined = envelope;
		for (bool in : {true, false}) {
			double length = in ? fade_in : fade_out;
			if (!length) { continue; }
			double anchor = in ? timing->convert (outer.position, fade_unit)
			                   : timing->convert (Time (clip_start + clip_length, TimeUnit::Seconds), fade_unit) - length;
			double begin = timing->convert (Time (anchor, fade_unit), TimeUnit::Seconds) - clip_start;
			double end = timing->convert (Time (anchor + length, fade_unit), TimeUnit::Seconds) - clip_start;
			if (begin < -1e-8 || end > clip_length + 1e-8) { throw ExchangeError ("Fade extends outside clip"); }
			Automation fade {fade_unit, {{anchor, in ? 0.0 : 1.0, true}, {anchor + length, in ? 1.0 : 0.0, true}}};
			fade = slice_automation (convert_automation (fade, *timing, TimeUnit::Seconds), clip_start, clip_start + clip_length);
			combined = multiply_automation (combined, fade, quantum);
		}
		if (!combined.points.empty ()) {
			double begin = timing->convert (value.position, TimeUnit::Seconds) - clip_start;
			double length = timing->duration (Time (value.duration, value.position.unit), value.position, TimeUnit::Seconds);
			value.envelope = multiply_automation (value.envelope, slice_automation (combined, begin, begin + length), quantum);
		}
	}
	if (fade_in || fade_out) { diagnostic (node, "Duration-only fades converted to linear-amplitude clip envelopes; DAW-specific fade shapes are not specified", Disposition::Transformed); }
	if (!expression.points.empty ()) { diagnostic (node, "Clip gain expression mapped to an editable sample-timed envelope; holds use a one-sample transition", Disposition::Transformed); }
	active.erase (node);
	return result;
}

void
Parser::timeline (Node node, TimeUnit inherited_unit, const std::string& inherited_track)
{
	TimeUnit unit = time_unit (node, "timeUnit", inherited_unit);
	std::string track = attr (node, "track", inherited_track);
	if (is (node, "Points")) { points (node, unit); return; }
	attributes (node, {"id", "timeUnit", "track"});
	if (is (node, "Lanes")) {
		for (Node item : children (node)) { timeline (item, unit, track); }
	} else if (is (node, "Clips")) {
		auto found = track_indices.find (track);
		if (found == track_indices.end ()) { throw ExchangeError ("Clips reference an unknown track: " + track); }
		for (Node item : children (node)) {
			if (!is (item, "Clip")) { diagnostic (item, "Unknown clips element"); continue; }
			std::set<Node> active;
			std::vector<Clip> parsed = clip (item, unit, active);
			Track& destination = project.tracks[found->second];
			if (!parsed.empty () && destination.clips.empty () && infer_track_kind.count (track)) {
				destination.kind = parsed.front ().kind == ClipKind::Audio ? TrackKind::Audio : TrackKind::MIDI;
			}
			destination.clips.insert (destination.clips.end (), std::make_move_iterator (parsed.begin ()), std::make_move_iterator (parsed.end ()));
		}
	} else {
		diagnostic (node, "Timeline content is not yet converted");
	}
}

Project
Parser::parse (Node root)
{
	if (!root || !is (root, "Project")) { throw ExchangeError ("Unsupported DAWproject root element"); }
	std::string version = required (root, "version");
	if (version != "1.0" && version != "0.1") { throw ExchangeError ("Unsupported DAWproject version"); }
	index (root);
	attributes (root, {"version"});
	Node application = child (root, "Application");
	if (!application) { throw ExchangeError ("Project is missing its Application element"); }
	attributes (application, {"name", "version"});
	project.application = required (application, "name");
	project.application_version = required (application, "version");
	if (Node transport = child (root, "Transport")) {
		attributes (transport, {});
		if (Node tempo = child (transport, "Tempo")) {
			attributes (tempo, {"id", "name", "min", "max", "unit", "value"});
			if (required (tempo, "unit") != "bpm") { throw ExchangeError ("Unsupported tempo unit"); }
			project.tempo = number (tempo, "value", 120);
		}
		if (Node meter = child (transport, "TimeSignature")) {
			attributes (meter, {"id", "name", "numerator", "denominator"});
			project.numerator = integer (meter, "numerator", 0, true);
			project.denominator = integer (meter, "denominator", 0, true);
		}
		for (Node item : children (transport)) {
			if (!is (item, "Tempo") && !is (item, "TimeSignature")) { diagnostic (item, "Unknown transport element"); }
		}
	}
	/* Validate transport before any time conversion or flattening. */
	validate_project (project);
	if (Node s = child (root, "Structure")) {
		attributes (s, {});
		for (Node node : children (s)) {
			if (is (node, "Track") || is (node, "Channel")) { structure (node); }
			else { diagnostic (node, "Unknown structure element"); }
		}
	}
	for (Track& track : project.tracks) {
		if (!track.destination.empty ()) {
			auto found = channel_tracks.find (track.destination);
			if (found == channel_tracks.end ()) { throw ExchangeError ("Unknown channel destination: " + track.destination); }
			track.destination = found->second;
		}
		for (auto& send : track.sends) {
			auto found = channel_tracks.find (send.destination);
			if (found == channel_tracks.end ()) { throw ExchangeError ("Unknown send destination: " + send.destination); }
			send.destination = found->second;
		}
	}
	if (Node arrangement = child (root, "Arrangement")) {
		attributes (arrangement, {"id"});
		for (bool tempo : {true, false}) {
			Node automation = child (arrangement, tempo ? "TempoAutomation" : "TimeSignatureAutomation");
			if (!automation) { continue; }
			attributes (automation, {"id", "timeUnit", "unit"});
			Node target = child (automation, "Target");
			if (!target) { throw ExchangeError ("Tempo/meter automation is missing its target"); }
			attributes (target, {"parameter"});
			auto found = ids.find (required (target, "parameter"));
			Node transport = child (root, "Transport");
			if (found == ids.end () || !transport || found->second != child (transport, tempo ? "Tempo" : "TimeSignature") || !children (target).empty ()) {
				throw ExchangeError ("Tempo/meter automation has an invalid target");
			}
			if (has (automation, "unit") && (!tempo || attr (automation, "unit") != "bpm")) { throw ExchangeError ("Invalid tempo/meter automation unit"); }
			TimeUnit unit = time_unit (automation, "timeUnit", TimeUnit::Beats);
			for (Node point : children (automation)) {
				if (is (point, "Target")) { continue; }
				expansion ();
				if (!children (point).empty ()) { throw ExchangeError ("Invalid transport point content"); }
				if (tempo && is (point, "RealPoint")) {
					attributes (point, {"time", "value", "interpolation"});
					std::string mode = attr (point, "interpolation", "hold");
					if (mode != "hold" && mode != "linear") { throw ExchangeError ("Unsupported tempo interpolation"); }
					project.tempos.push_back ({Time (number (point, "time", 0, true), unit), number (point, "value", 0, true), mode == "linear"});
				} else if (!tempo && is (point, "TimeSignaturePoint")) {
					attributes (point, {"time", "numerator", "denominator"});
					project.meters.push_back ({Time (number (point, "time", 0, true), unit), integer (point, "numerator", 0, true), integer (point, "denominator", 0, true)});
				} else { throw ExchangeError ("Unexpected tempo/meter point type"); }
			}
		}
		timing.reset (new TimeMap (project));
		if (Node lanes = child (arrangement, "Lanes")) { timeline (lanes, TimeUnit::Beats, ""); }
		if (Node markers = child (arrangement, "Markers")) {
			attributes (markers, {"id", "timeUnit"});
			TimeUnit unit = time_unit (markers, "timeUnit", TimeUnit::Beats);
			for (Node item : children (markers)) {
				if (!is (item, "Marker")) { diagnostic (item, "Unknown marker element"); continue; }
				attributes (item, {"name", "time"});
				if (!children (item).empty ()) { throw ExchangeError ("Invalid marker content"); }
				project.markers.push_back ({attr (item, "name"), Time (number (item, "time", 0, true), unit)});
			}
		}
		for (Node item : children (arrangement)) {
			if (!is (item, "Lanes") && !is (item, "Markers") && !is (item, "TempoAutomation") && !is (item, "TimeSignatureAutomation")) { diagnostic (item, "Arrangement data is not yet converted"); }
		}
	}
	for (Node item : children (root)) {
		if (!is (item, "Application") && !is (item, "Transport") && !is (item, "Structure") && !is (item, "Arrangement")) {
			if (!is (item, "Scenes") || !children (item).empty ()) { diagnostic (item, "Project data is not yet converted"); }
		}
	}
	validate_project (project);
	return project;
}

Node element (Node parent, const char* name) { return xmlNewChild (parent, NULL, BAD_CAST name, NULL); }

void property (Node node, const char* name, const std::string& value)
{
	if (!g_utf8_validate (value.data (), value.size (), NULL)) { throw ExchangeError ("Invalid UTF-8 project text"); }
	for (const char* p = value.c_str (); *p; p = g_utf8_next_char (p)) {
		gunichar c = g_utf8_get_char (p);
		bool valid = c == 0x9 || c == 0xa || c == 0xd || (c >= 0x20 && c <= 0xd7ff) ||
		             (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
		if (!valid) { throw ExchangeError ("Text contains an invalid XML character"); }
	}
	if (!xmlNewProp (node, BAD_CAST name, BAD_CAST value.c_str ())) { throw ExchangeError ("Cannot create XML property"); }
}

void property (Node node, const char* name, double value)
{
	char buffer[G_ASCII_DTOSTR_BUF_SIZE];
	property (node, name, std::string (g_ascii_dtostr (buffer, sizeof (buffer), value)));
}

void property (Node node, const char* name, const char* value) { property (node, name, std::string (value)); }
const char* unit_name (TimeUnit unit) { return unit == TimeUnit::Beats ? "beats" : "seconds"; }

struct Writer {
	explicit Writer (const Project& p) : project (p), timing (p) {}
	const Project& project;
	TimeMap timing;
	std::map<std::string, std::string> track_ids;
	std::map<std::string, std::string> channel_ids;
	std::map<std::string, std::string> media_ids;
	std::set<std::string> written_media;

	void channel (Node parent, const Track& track)
	{
		Node output = element (parent, "Channel");
		property (output, "id", channel_ids.at (track.id));
		property (output, "role", track.master ? "master" : track.kind == TrackKind::Bus ? "effect" : "regular");
		property (output, "audioChannels", track.channels);
		property (output, "solo", track.solo ? "true" : "false");
		if (!track.destination.empty ()) { property (output, "destination", channel_ids.at (track.destination)); }
		if (!track.plugins.empty ()) {
			Node devices = element (output, "Devices");
			for (auto const& plugin : track.plugins) {
				Node device = element (devices, "Vst3Plugin");
				property (device, "deviceID", plugin.id);
				property (device, "deviceName", plugin.name);
				property (device, "deviceRole", plugin.role);
				property (device, "loaded", "true");
				property (element (device, "Enabled"), "value", plugin.enabled ? "true" : "false");
				property (element (device, "State"), "path", plugin.state_path);
			}
		}
		Node mute = element (output, "Mute");
		property (mute, "value", track.mute ? "true" : "false");
		Node pan = element (output, "Pan");
		property (pan, "id", channel_ids.at (track.id) + "-pan");
		property (pan, "unit", "normalized");
		property (pan, "value", track.pan);
		if (!track.sends.empty ()) {
			Node sends = element (output, "Sends");
			for (auto const& send : track.sends) {
				Node entry = element (sends, "Send");
				property (entry, "destination", channel_ids.at (send.destination));
				property (entry, "type", send.pre_fader ? "pre" : "post");
				if (!send.enabled) { property (element (entry, "Enable"), "value", "false"); }
				if (send.pan != .5) {
					Node pan = element (entry, "Pan");
					property (pan, "unit", "normalized"); property (pan, "value", send.pan);
				}
				Node volume = element (entry, "Volume");
				property (volume, "unit", "linear");
				property (volume, "value", send.volume);
			}
		}
		Node volume = element (output, "Volume");
		property (volume, "id", channel_ids.at (track.id) + "-volume");
		property (volume, "unit", "linear");
		property (volume, "value", track.volume);
	}

	void tracks (Node parent, const std::string& parent_id)
	{
		for (auto const& track : project.tracks) {
			if (track.parent != parent_id) { continue; }
			Node output = element (parent, "Track");
			property (output, "id", track_ids.at (track.id));
			property (output, "name", track.name);
			if (!track.color.empty ()) { property (output, "color", track.color); }
			if (track.kind == TrackKind::Audio || track.kind == TrackKind::MIDI) {
				property (output, "contentType", track.kind == TrackKind::Audio ? "audio" : "notes");
			} else if (track.kind == TrackKind::Folder) {
				property (output, "contentType", "tracks");
			}
			channel (output, track);
			tracks (output, track.id);
		}
	}

	void clip (Node parent, const Clip& clip)
	{
		Node output = element (parent, "Clip");
		property (output, "time", clip.position.value);
		property (output, "duration", clip.duration);
		property (output, "contentTimeUnit", unit_name (clip.offset.unit));
		property (output, "playStart", clip.offset.value);
		property (output, "playStop", clip.offset.value + timing.duration (Time (clip.duration, clip.position.unit), clip.position, clip.offset.unit));
		property (output, "enable", clip.enabled ? "true" : "false");
		if (!clip.name.empty ()) { property (output, "name", clip.name); }
		if (!clip.color.empty ()) { property (output, "color", clip.color); }
		if (clip.kind == ClipKind::Audio) {
			bool first = written_media.insert (clip.media).second;
			bool processing = clip.gain != 1 || !clip.envelope.points.empty ();
			if (!first && !processing) {
				property (output, "reference", media_ids.at (clip.media));
				return;
			}
			auto source = std::find_if (project.media.begin (), project.media.end (), [&] (const Media& m) { return m.id == clip.media; });
			Node content = output;
			if (processing) {
				content = element (output, "Lanes"); property (content, "timeUnit", "seconds");
				Node points = element (content, "Points"); property (points, "unit", "linear");
				property (element (points, "Target"), "expression", "gain");
				auto curve = clip.envelope;
				if (curve.points.empty ()) { curve.points.push_back ({0, 1, true}); }
				for (auto const& point : curve.points) {
					Node p = element (points, "RealPoint"); property (p, "time", clip.offset.value + point.time);
					property (p, "value", clip.gain * point.value); property (p, "interpolation", point.linear ? "linear" : "hold");
				}
			}
			Node audio = element (content, "Audio");
			if (first) { property (audio, "id", media_ids.at (clip.media)); }
			property (audio, "timeUnit", "seconds");
			property (audio, "duration", source->duration);
			property (audio, "sampleRate", source->sample_rate);
			property (audio, "channels", source->channels);
			Node file = element (audio, "File");
			property (file, "path", source->path);
			property (file, "external", "false");
		} else {
			Node notes = element (output, "Notes");
			property (notes, "timeUnit", unit_name (clip.offset.unit));
			for (auto const& note : clip.notes) {
				Node n = element (notes, "Note");
				property (n, "time", note.time);
				property (n, "duration", note.duration);
				property (n, "key", note.key);
				property (n, "channel", note.channel);
				property (n, "vel", note.velocity);
				property (n, "rel", note.release_velocity);
			}
		}
	}

	std::string write ()
	{
		validate_project (project);
		if (has_unsupported_features (project)) { throw ExchangeError ("Cannot export a project with unacknowledged unsupported features"); }
		for (size_t i = 0; i < project.tracks.size (); ++i) {
			track_ids[project.tracks[i].id] = "track" + std::to_string (i);
			channel_ids[project.tracks[i].id] = "channel" + std::to_string (i);
		}
		for (size_t i = 0; i < project.media.size (); ++i) { media_ids[project.media[i].id] = "audio" + std::to_string (i); }
		Document doc (xmlNewDoc (BAD_CAST "1.0"), xmlFreeDoc);
		if (!doc) { throw ExchangeError ("Cannot allocate XML document"); }
		Node root = xmlNewNode (NULL, BAD_CAST "Project");
		xmlDocSetRootElement (doc.get (), root);
		property (root, "version", "1.0");
		Node application = element (root, "Application");
		property (application, "name", project.application);
		property (application, "version", project.application_version);
		Node transport = element (root, "Transport");
		Node tempo = element (transport, "Tempo");
		property (tempo, "id", "transport-tempo");
		property (tempo, "unit", "bpm");
		property (tempo, "value", project.tempo);
		Node meter = element (transport, "TimeSignature");
		property (meter, "id", "transport-meter");
		property (meter, "numerator", project.numerator);
		property (meter, "denominator", project.denominator);
		tracks (element (root, "Structure"), "");
		Node arrangement = element (root, "Arrangement");
		Node lanes = element (arrangement, "Lanes");
		property (lanes, "timeUnit", "beats");
		for (auto const& track : project.tracks) {
			if (track.clips.empty () && track.volume_automation.points.empty () && track.pan_automation.points.empty ()) { continue; }
			Node lane = element (lanes, "Lanes");
			property (lane, "track", track_ids.at (track.id));
			for (bool pan : {false, true}) {
				auto const& curve = pan ? track.pan_automation : track.volume_automation;
				if (curve.points.empty ()) { continue; }
				Node points = element (lane, "Points");
				property (points, "timeUnit", unit_name (curve.unit));
				property (points, "unit", pan ? "normalized" : "linear");
				property (element (points, "Target"), "parameter", channel_ids.at (track.id) + (pan ? "-pan" : "-volume"));
				for (auto const& point : curve.points) {
					Node item = element (points, "RealPoint");
					property (item, "time", point.time); property (item, "value", point.value);
					property (item, "interpolation", point.linear ? "linear" : "hold");
				}
			}
			for (auto const& c : track.clips) {
				Node clips = element (lane, "Clips");
				property (clips, "timeUnit", unit_name (c.position.unit));
				clip (clips, c);
			}
		}
		if (!project.markers.empty ()) {
			Node markers = element (arrangement, "Markers");
			/* A single marker lane has one unit; preserve absolute positions
			 * when the source session mixes musical and sample-time markers. */
			TimeUnit unit = project.markers.front ().position.unit;
			for (auto const& marker : project.markers) { if (marker.position.unit != unit) { unit = TimeUnit::Seconds; break; } }
			property (markers, "timeUnit", unit_name (unit));
			for (auto const& marker : project.markers) {
				Node item = element (markers, "Marker");
				property (item, "name", marker.name);
				property (item, "time", timing.convert (marker.position, unit));
			}
		}
		if (!project.tempos.empty ()) {
			Node points = element (arrangement, "TempoAutomation");
			property (points, "timeUnit", unit_name (project.tempos.front ().position.unit));
			property (points, "unit", "bpm");
			property (element (points, "Target"), "parameter", "transport-tempo");
			for (auto const& change : project.tempos) {
				Node point = element (points, "RealPoint");
				property (point, "time", change.position.value); property (point, "value", change.bpm);
				property (point, "interpolation", change.linear ? "linear" : "hold");
			}
		}
		if (!project.meters.empty ()) {
			Node points = element (arrangement, "TimeSignatureAutomation");
			TimeUnit unit = project.meters.front ().position.unit;
			for (auto const& change : project.meters) { if (change.position.unit != unit) { unit = TimeUnit::Beats; break; } }
			property (points, "timeUnit", unit_name (unit));
			property (element (points, "Target"), "parameter", "transport-meter");
			for (auto const& change : project.meters) {
				Node point = element (points, "TimeSignaturePoint");
				property (point, "time", timing.convert (change.position, unit));
				property (point, "numerator", change.numerator); property (point, "denominator", change.denominator);
			}
		}
		xmlChar* buffer = NULL;
		int size = 0;
		xmlDocDumpFormatMemoryEnc (doc.get (), &buffer, &size, "UTF-8", 1);
		if (!buffer) { throw ExchangeError ("Cannot serialize project XML"); }
		std::string output (reinterpret_cast<const char*> (buffer), size);
		xmlFree (buffer);
		if (output.size () > 16 * 1024 * 1024) { throw ExchangeError ("Project XML exceeds the size limit"); }
		return output;
	}
};

} // namespace

Project
DawProject::parse_xml (const std::string& text)
{
	Document doc = parse_document (text);
	Parser parser;
	return parser.parse (xmlDocGetRootElement (doc.get ()));
}

Project
DawProject::read (const std::string& filename, CancelCheck cancel)
{
	ProjectArchive archive (filename, ArchiveLimits (), cancel);
	Project result = parse_xml (archive.read_text ("project.xml"));
	for (auto const& media : result.media) {
		if (cancel && cancel ()) { throw ArchiveCancelled (); }
		auto const& members = archive.members ();
		if (std::none_of (members.begin (), members.end (), [&] (const ArchiveMember& m) { return m.name == media.path; })) {
			throw ExchangeError ("Missing embedded media: " + media.path);
		}
	}
	size_t remaining_state = 512 * 1024 * 1024;
	for (auto& track : result.tracks) {
		for (auto& plugin : track.plugins) {
			if (cancel && cancel ()) { throw ArchiveCancelled (); }
			plugin.state = archive.read_text (plugin.state_path, std::min (size_t (64 * 1024 * 1024), remaining_state));
			remaining_state -= plugin.state.size ();
			validate_vst3_state (plugin.id, plugin.state);
		}
	}
	validate_project (result);
	return result;
}

std::string
DawProject::write_xml (const Project& project)
{
	return Writer (project).write ();
}

void
DawProject::write (const Project& project, const std::string& filename,
                   const std::map<std::string, std::string>& media_sources, CancelCheck cancel)
{
	std::string xml = write_xml (project);
	for (auto const& track : project.tracks) {
		for (auto const& plugin : track.plugins) {
			if (plugin.state.empty ()) { throw ExchangeError ("Missing embedded VST3 preset state: " + plugin.name); }
		}
	}
	ProjectArchiveWriter output (filename, ArchiveLimits (), cancel);
	output.add_text ("project.xml", xml);
	output.add_text ("metadata.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MetaData/>\n");
	for (auto const& media : project.media) {
		if (cancel && cancel ()) { throw ArchiveCancelled (); }
		auto source = media_sources.find (media.id);
		if (source == media_sources.end ()) { throw ExchangeError ("Missing export media source: " + media.id); }
		output.add_file (media.path, source->second);
	}
	for (auto const& track : project.tracks) {
		for (auto const& plugin : track.plugins) {
			if (cancel && cancel ()) { throw ArchiveCancelled (); }
			output.add_text (plugin.state_path, plugin.state);
		}
	}
	output.finish ();
}

} }
