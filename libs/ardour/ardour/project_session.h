/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_PROJECT_SESSION_H
#define ARDOUR_PROJECT_SESSION_H

#include <memory>
#include <string>
#include <vector>
#include "ardour/dawproject.h"

namespace ARDOUR {
class AudioEngine;
class Session;
namespace ProjectExchange {

class LIBARDOUR_API SessionExchange {
public:
	/* Requires a running engine with no session attached. Builds in a private
	 * sibling directory, saves, then moves and reopens the completed session.
	 * The returned session is attached to the engine and owned by the caller. */
	static std::unique_ptr<Session> import_dawproject (AudioEngine&, const std::string& archive,
	                                                const std::string& destination, const std::string& snapshot,
	                                                std::vector<Diagnostic>&, CancelCheck = CancelCheck ());
	/* Run on the session's owning control thread with editing suspended.
	 * Transport must be stopped. An existing destination is never replaced. */
	static std::vector<Diagnostic> export_dawproject (Session&, const std::string& destination, CancelCheck = CancelCheck ());
};

} }
#endif
