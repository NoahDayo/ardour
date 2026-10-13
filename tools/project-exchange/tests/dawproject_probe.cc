/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <iomanip>
#include <iostream>
#include <locale>
#include <string>
#include <vector>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include "ardour/dawproject.h"
#include "ardour/studio_one_song.h"

using namespace ARDOUR::ProjectExchange;

static void quoted (const std::string& text)
{
	std::cout << '"';
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') { std::cout << '\\' << c; }
		else if (c < 32) {
			std::cout << "\\u" << std::hex << std::setw (4) << std::setfill ('0') << static_cast<unsigned int> (c) << std::dec;
		} else { std::cout << c; }
	}
	std::cout << '"';
}

static void time_json (Time time)
{
	std::cout << '[' << time.value << ',';
	quoted (time.unit == TimeUnit::Seconds ? "seconds" : "beats");
	std::cout << ']';
}

static void dump (const Project& p)
{
	std::cout << "{\"tempo\":" << p.tempo << ",\"meter\":[" << p.numerator << ',' << p.denominator << "],\"tempos\":[";
	bool first = true;
	for (auto const& point : p.tempos) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << '['; time_json (point.position); std::cout << ',' << point.bpm << ',' << (point.linear ? "true" : "false") << ']';
	}
	std::cout << "],\"meters\":["; first = true;
	for (auto const& point : p.meters) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << '['; time_json (point.position); std::cout << ',' << point.numerator << ',' << point.denominator << ']';
	}
	std::cout << "],\"media\":["; first = true;
	for (auto const& m : p.media) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << "{\"id\":"; quoted (m.id);
		std::cout << ",\"path\":"; quoted (m.path);
		std::cout << ",\"channels\":" << m.channels << ",\"rate\":" << m.sample_rate << ",\"duration\":" << m.duration << '}';
	}
	std::cout << "],\"tracks\":[";
	first = true;
	for (auto const& t : p.tracks) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << "{\"id\":"; quoted (t.id);
		std::cout << ",\"name\":"; quoted (t.name);
		std::cout << ",\"color\":"; quoted (t.color);
		std::cout << ",\"parent\":"; quoted (t.parent);
		std::cout << ",\"destination\":"; quoted (t.destination);
		std::cout << ",\"kind\":" << static_cast<int> (t.kind) << ",\"channels\":" << t.channels
		          << ",\"master\":" << (t.master ? "true" : "false") << ",\"mute\":" << (t.mute ? "true" : "false")
		          << ",\"solo\":" << (t.solo ? "true" : "false") << ",\"volume\":" << t.volume << ",\"pan\":" << t.pan;
		for (bool pan : {false, true}) {
			auto const& curve = pan ? t.pan_automation : t.volume_automation;
			std::cout << (pan ? ",\"pan_automation\":" : ",\"volume_automation\":") << "{\"unit\":";
			quoted (curve.unit == TimeUnit::Beats ? "beats" : "seconds"); std::cout << ",\"points\":[";
			bool first_point = true;
			for (auto const& point : curve.points) {
				if (!first_point) { std::cout << ','; } first_point = false;
				std::cout << '[' << point.time << ',' << point.value << ',' << (point.linear ? "true" : "false") << ']';
			}
			std::cout << "]}";
		}
		std::cout << ",\"sends\":[";
		for (size_t i = 0; i < t.sends.size (); ++i) {
			if (i) { std::cout << ','; }
			auto const& send = t.sends[i];
			std::cout << "{\"destination\":"; quoted (send.destination);
			std::cout << ",\"pre_fader\":" << (send.pre_fader ? "true" : "false")
			          << ",\"enabled\":" << (send.enabled ? "true" : "false") << ",\"volume\":" << send.volume
			          << ",\"pan\":" << send.pan << '}';
		}
		std::cout << ']';
		std::cout << ",\"plugins\":[";
		for (size_t i = 0; i < t.plugins.size (); ++i) {
			if (i) { std::cout << ','; }
			auto const& plugin = t.plugins[i];
			std::cout << "{\"id\":"; quoted (plugin.id);
			std::cout << ",\"name\":"; quoted (plugin.name);
			std::cout << ",\"role\":"; quoted (plugin.role);
			std::cout << ",\"state_path\":"; quoted (plugin.state_path);
			std::cout << ",\"state_size\":" << plugin.state.size ()
			          << ",\"enabled\":" << (plugin.enabled ? "true" : "false") << '}';
		}
		std::cout << ']';
		std::cout << ",\"clips\":[";
		bool first_clip = true;
		for (auto const& c : t.clips) {
			if (!first_clip) { std::cout << ','; } first_clip = false;
			std::cout << "{\"name\":"; quoted (c.name);
			std::cout << ",\"color\":"; quoted (c.color);
			std::cout << ",\"kind\":" << static_cast<int> (c.kind) << ",\"position\":"; time_json (c.position);
			std::cout << ",\"duration\":" << c.duration << ",\"offset\":"; time_json (c.offset);
			std::cout << ",\"gain\":" << c.gain << ",\"envelope\":[";
			for (size_t i = 0; i < c.envelope.points.size (); ++i) {
				auto const& point = c.envelope.points[i]; if (i) { std::cout << ','; }
				std::cout << '[' << point.time << ',' << point.value << ',' << (point.linear ? "true" : "false") << ']';
			}
			std::cout << ']';
			std::cout << ",\"enabled\":" << (c.enabled ? "true" : "false") << ",\"media\":"; quoted (c.media);
			std::cout << ",\"notes\":[";
			bool first_note = true;
			for (auto const& n : c.notes) {
				if (!first_note) { std::cout << ','; } first_note = false;
				std::cout << '[' << n.time << ',' << n.duration << ',' << n.key << ',' << n.channel << ',' << n.velocity << ',' << n.release_velocity << ']';
			}
			std::cout << "]}";
		}
		std::cout << "]}";
	}
	std::cout << "],\"markers\":[";
	first = true;
	for (auto const& marker : p.markers) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << "{\"name\":"; quoted (marker.name);
		std::cout << ",\"position\":"; time_json (marker.position); std::cout << '}';
	}
	std::cout << "],\"report\":[";
	first = true;
	for (auto const& d : p.report) {
		if (!first) { std::cout << ','; } first = false;
		std::cout << "{\"disposition\":" << static_cast<int> (d.disposition) << ",\"object\":"; quoted (d.object);
		std::cout << ",\"detail\":"; quoted (d.detail); std::cout << '}';
	}
	std::cout << "]}\n";
}

static std::string read_text (const std::string& path)
{
	gchar* buffer = NULL;
	gsize size = 0;
	if (!g_file_get_contents (path.c_str (), &buffer, &size, NULL)) { throw ExchangeError ("Cannot read test file"); }
	std::string result (buffer, size);
	g_free (buffer);
	return result;
}

static int run (const std::vector<std::string>& args)
{
	std::cout.imbue (std::locale::classic ());
	std::cout << std::setprecision (17);
	try {
		if (args.size () < 3) { return 2; }
		if (args[1] == "parse") { dump (DawProject::parse_xml (read_text (args[2]))); }
		else if (args[1] == "timing") {
			Project p = DawProject::parse_xml (read_text (args[2])); TimeMap map (p);
			Project approximation = p; approximation.tempos = map.stepped (0.00001); TimeMap steps (approximation);
			std::cout << '[';
			for (unsigned i = 0; i <= 400; ++i) {
				double beat = i / 10.0, second = map.convert (Time (beat), TimeUnit::Seconds);
				if (i) { std::cout << ','; }
				std::cout << '[' << beat << ',' << second << ',' << map.convert (Time (second, TimeUnit::Seconds), TimeUnit::Beats)
				          << ',' << steps.convert (Time (beat), TimeUnit::Seconds) << ']';
			}
			std::cout << ']';
		}
		else if (args[1] == "read") { dump (DawProject::read (args[2])); }
		else if (args[1] == "read-song") {
			auto song = StudioOneSong::read (args[2]);
			std::cout << "{\"project\":"; dump (song.project);
			std::cout << ",\"start\":"; time_json (song.start);
			std::cout << ",\"end\":"; time_json (song.end);
			std::cout << ",\"has_range\":" << (song.has_range ? "true" : "false") << ",\"markers\":[";
			bool first = true;
			for (auto const& marker : song.markers) { if (!first) { std::cout << ','; } first = false; std::cout << "["; quoted (marker.name); std::cout << ','; time_json (marker.position); std::cout << ']'; }
			std::cout << "]}\n";
		}
		else if ((args[1] == "repack-song" || args[1] == "cancel-song") && (args.size () == 4 || args.size () == 5)) {
			auto song = StudioOneSong::read (args[2]);
			CancelCheck cancel;
			if (args[1] == "cancel-song") {
				gchar* directory = g_path_get_dirname (args[3].c_str ()); gchar* basename = g_path_get_basename (args[3].c_str ());
				std::string parent (directory), prefix = std::string (basename) + ".part-"; g_free (directory); g_free (basename);
				cancel = [parent, prefix] {
					GDir* dir = g_dir_open (parent.c_str (), 0, nullptr); bool found = false;
					if (dir) { while (const char* name = g_dir_read_name (dir)) { if (std::string (name).compare (0, prefix.size (), prefix) == 0) { found = true; break; } } g_dir_close (dir); }
					return found;
				};
			}
			StudioOneSong::write (song, args[3], args.size () == 5 ? args[4] : args[3], cancel);
		}
		else if (args[1] == "rewrite" && args.size () == 4) {
			std::string xml = DawProject::write_xml (DawProject::parse_xml (read_text (args[2])));
			if (!g_file_set_contents (args[3].c_str (), xml.c_str (), xml.size (), NULL)) { throw ExchangeError ("Cannot write test file"); }
		} else if (args[1] == "repack" && args.size () == 5) {
			Project p = DawProject::read (args[2]);
			ProjectArchive archive (args[2]);
			std::map<std::string, std::string> sources;
			for (size_t i = 0; i < p.media.size (); ++i) {
				std::string path = args[4] + "/media" + std::to_string (i);
				archive.copy_file (p.media[i].path, path);
				sources[p.media[i].id] = path;
			}
			DawProject::write (p, args[3], sources);
		} else if (args[1] == "schema" && args.size () == 4) {
			std::string xsd = read_text (args[3]);
			std::string xml = read_text (args[2]);
			std::unique_ptr<xmlSchemaParserCtxt, void (*) (xmlSchemaParserCtxt*)> parser (
				xmlSchemaNewMemParserCtxt (xsd.data (), xsd.size ()), xmlSchemaFreeParserCtxt);
			std::unique_ptr<xmlSchema, void (*) (xmlSchema*)> schema (xmlSchemaParse (parser.get ()), xmlSchemaFree);
			if (!schema) { throw ExchangeError ("Invalid test schema"); }
			std::unique_ptr<xmlSchemaValidCtxt, void (*) (xmlSchemaValidCtxt*)> validator (xmlSchemaNewValidCtxt (schema.get ()), xmlSchemaFreeValidCtxt);
			std::unique_ptr<xmlDoc, void (*) (xmlDoc*)> doc (xmlReadMemory (xml.data (), xml.size (), "project.xml", "UTF-8", XML_PARSE_NONET), xmlFreeDoc);
			if (!doc || xmlSchemaValidateDoc (validator.get (), doc.get ()) != 0) { throw ExchangeError ("Project failed schema validation"); }
		} else { return 2; }
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what () << '\n';
		return 1;
	}
}

#ifdef _WIN32
int wmain (int argc, wchar_t** argv)
{
	std::vector<std::string> args;
	for (int i = 0; i < argc; ++i) {
		gchar* text = g_utf16_to_utf8 (reinterpret_cast<const gunichar2*> (argv[i]), -1, NULL, NULL, NULL);
		if (!text) { return 2; }
		args.emplace_back (text);
		g_free (text);
	}
	return run (args);
}
#else
int main (int argc, char** argv) { return run (std::vector<std::string> (argv, argv + argc)); }
#endif
