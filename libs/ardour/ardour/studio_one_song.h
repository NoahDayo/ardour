/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_STUDIO_ONE_SONG_H
#define ARDOUR_STUDIO_ONE_SONG_H

#include <map>
#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"

namespace ARDOUR { namespace ProjectExchange {

using SongMarker = Marker;

/* Native .song data. External media is resolved only inside the selected
 * song's directory, including after that directory has been relocated. */
struct StudioOneSongData {
	Project project;
	std::string title;
	unsigned sample_rate = 48000;
	unsigned format_version = 9;
	std::map<std::string, std::string> audio_files;
	std::vector<SongMarker> markers;
	Time start;
	Time end;
	bool has_range = false;
};

class LIBARDOUR_API StudioOneSong {
public:
	static StudioOneSongData read (const std::string& filename, CancelCheck = CancelCheck ());
	static std::vector<Diagnostic> export_report (const Project&);
	static std::vector<Diagnostic> export_report (const Project&, unsigned format_version);
	/* Media must already be collected below filename's parent. document_path
	 * is the final song filename after the caller publishes its staged folder;
	 * this avoids putting temporary paths into the native document. */
	static void write (const StudioOneSongData&, const std::string& filename,
	                   const std::string& document_path, CancelCheck = CancelCheck ());
};

} }
#endif
