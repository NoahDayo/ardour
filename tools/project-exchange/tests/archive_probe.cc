/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <glib.h>
#include "ardour/project_archive.h"

using namespace ARDOUR::ProjectExchange;

static int
run (const std::vector<std::string>& args)
{
	try {
		if (args.size () < 3) { return 2; }
		ArchiveLimits limits;
		if (const char* v = std::getenv ("ARCHIVE_TEST_ENTRY_LIMIT")) { limits.entry_bytes = std::stoull (v); }
		if (const char* v = std::getenv ("ARCHIVE_TEST_TOTAL_LIMIT")) { limits.total_bytes = std::stoull (v); }
		if (const char* v = std::getenv ("ARCHIVE_TEST_COUNT_LIMIT")) { limits.entries = std::stoull (v); }
		int checks = 0;
		int cancel_after = -1;
		if (const char* v = std::getenv ("ARCHIVE_TEST_CANCEL")) { cancel_after = std::stoi (v); }
		CancelCheck cancel = [&] { return cancel_after >= 0 && checks++ >= cancel_after; };
		const std::string& command = args[1];
		if (command == "write" || command == "abandon" || command == "poison") {
			ProjectArchiveWriter writer (args[2], limits, cancel);
			writer.add_text ("project.xml", "<Project version=\"1.0\"/>\n");
			writer.add_text ("metadata.xml", "<MetaData/>\n");
			writer.add_text ("empty", "");
			for (size_t i = 3; i + 1 < args.size (); i += 2) {
				if (command == "poison") {
					try { writer.add_file (args[i], args[i + 1]); } catch (const ArchiveError&) {}
				} else {
					writer.add_file (args[i], args[i + 1]);
				}
			}
			if (command != "abandon") { writer.finish (); }
		} else {
			ProjectArchive reader (args[2], limits, cancel);
			if (command == "list") {
				for (auto const& entry : reader.members ()) { std::cout << entry.size << "\t" << entry.name << "\n"; }
			} else if (command == "read" && args.size () >= 4) {
				std::cout << reader.read_text (args[3], args.size () == 5 ? std::stoull (args[4]) : 16 * 1024 * 1024);
			} else if (command == "copy" && args.size () == 5) {
				reader.copy_file (args[3], args[4]);
			} else { return 2; }
		}
		return 0;
	} catch (const ArchiveCancelled& e) {
		std::cerr << e.what () << "\n";
		return 3;
	} catch (const std::exception& e) {
		std::cerr << e.what () << "\n";
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
