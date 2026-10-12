/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_DAWPROJECT_H
#define ARDOUR_DAWPROJECT_H

#include <map>
#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"

namespace ARDOUR { namespace ProjectExchange {

class LIBARDOUR_API DawProject {
public:
	static Project parse_xml (const std::string&);
	static std::string write_xml (const Project&);
	static Project read (const std::string& filename, CancelCheck = CancelCheck ());
	/* Source paths are supplied explicitly, keyed by Media.id. Never publish a
	 * project with unacknowledged unsupported features in its conversion report. */
	static void write (const Project&, const std::string& filename,
	                   const std::map<std::string, std::string>& media_sources, CancelCheck = CancelCheck ());
};

} }
#endif
