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
	static std::unique_ptr<Session> import_studio_one (AudioEngine&, const std::string& song,
	                                                const std::string& destination, const std::string& snapshot,
	                                                std::vector<Diagnostic>&, CancelCheck = CancelCheck ());
	/* Run on the session's owning control thread with editing suspended.
	 * Transport must be stopped. An existing destination is never replaced. */
	static std::vector<Diagnostic> export_dawproject (Session&, const std::string& destination, CancelCheck = CancelCheck ());
	/* Also return all collected diagnostics when unsupported content prevents
	 * export, so the caller can present every required conversion together. */
	static void export_dawproject (Session&, const std::string& destination,
	                               std::vector<Diagnostic>& report, CancelCheck = CancelCheck ());
	/* Publish an AAF with embedded audio and a coordinated type-1 MIDI file
	 * in a new directory. Both use timeline/timecode origin zero. */
	static void export_logic (Session&, const std::string& destination,
	                          std::vector<Diagnostic>& report, CancelCheck = CancelCheck ());
};

} }
#endif
