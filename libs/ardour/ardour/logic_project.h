/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_LOGIC_PROJECT_H
#define ARDOUR_LOGIC_PROJECT_H

#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"

namespace ARDOUR { namespace ProjectExchange {

/* AAF audio and an optional, explicitly selected companion SMF share one
 * timeline origin. Media is copied only into the caller's private staging
 * directory. The reader must outlive those copies. */
class LIBARDOUR_API LogicProject {
public:
	LogicProject (const std::string& aaf, const std::string& midi = std::string (), CancelCheck = CancelCheck ());
	~LogicProject ();
	const Project& project () const;
	void copy_media (const Media&, const std::string& destination);
private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};

/* Append note tracks and transport information from a bounded type-0/1 SMF.
 * The AAF origin is timecode seconds, while resulting clips start at beat zero. */
LIBARDOUR_API void read_project_midi (const std::string&, Project&, double aaf_origin, CancelCheck = CancelCheck ());

} }
#endif
