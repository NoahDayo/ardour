/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_STUDIO_ONE_SONG_H
#define ARDOUR_STUDIO_ONE_SONG_H

#include <map>
#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"

namespace ARDOUR { namespace ProjectExchange {

struct SongMarker {
	std::string name;
	Time position;
};

/* Native .song data. External media is resolved only inside the selected
 * song's directory, including after that directory has been relocated. */
struct StudioOneSongData {
	Project project;
	std::map<std::string, std::string> audio_files;
	std::vector<SongMarker> markers;
	Time start;
	Time end;
	bool has_range = false;
};

class LIBARDOUR_API StudioOneSong {
public:
	static StudioOneSongData read (const std::string& filename, CancelCheck = CancelCheck ());
};

} }
#endif
