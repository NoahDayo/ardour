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
	struct ExportOptions {
		bool render_mix = false;
		bool render_stems = false;
		double render_tail_seconds = 2.0;
		unsigned studio_one_format_version = 9;
		bool rendered () const { return render_mix || render_stems; }
	};
	/* Inspect plug-in availability before showing the partial-import choice.
	 * Each returned item describes processing that would be omitted. */
	static std::vector<Diagnostic> unavailable_vst3_plugins (const Project&);
	/* Requires a running engine with no session attached. Builds in a private
	 * sibling directory, saves, then moves and reopens the completed session.
	 * The returned session is attached to the engine and owned by the caller. */
	/* Unsupported project features are rejected unless the caller explicitly
	 * accepts a partial import. The returned report still lists every omission. */
	static std::unique_ptr<Session> import_dawproject (AudioEngine&, const std::string& archive,
	                                                const std::string& destination, const std::string& snapshot,
	                                                std::vector<Diagnostic>&, CancelCheck = CancelCheck ());
	static std::unique_ptr<Session> import_dawproject (AudioEngine&, const std::string& archive,
	                                                const std::string& destination, const std::string& snapshot,
	                                                std::vector<Diagnostic>&, CancelCheck, bool allow_unsupported);
	/* The same partial-import rules apply to native Studio One songs. */
	static std::unique_ptr<Session> import_studio_one (AudioEngine&, const std::string& song,
	                                                const std::string& destination, const std::string& snapshot,
	                                                std::vector<Diagnostic>&, CancelCheck = CancelCheck (),
	                                                bool allow_unsupported = false);
	/* Same engine and transaction requirements as import_dawproject. */
	static std::unique_ptr<Session> import_logic (AudioEngine&, const std::string& aaf, const std::string& midi,
	                                            const std::string& destination, const std::string& snapshot,
	                                            std::vector<Diagnostic>&, CancelCheck = CancelCheck ());
	static std::unique_ptr<Session> import_logic (AudioEngine&, const std::string& aaf, const std::string& midi,
	                                            const std::string& destination, const std::string& snapshot,
	                                            std::vector<Diagnostic>&, CancelCheck, bool allow_unsupported);
	/* Run on the session's owning control thread with editing suspended.
	 * Transport must be stopped. An existing destination is never replaced. */
	static std::vector<Diagnostic> export_dawproject (Session&, const std::string& destination, CancelCheck = CancelCheck ());
	/* Also return all collected diagnostics when unsupported content prevents
	 * export, so the caller can present every required conversion together. */
	static void export_dawproject (Session&, const std::string& destination,
	                               std::vector<Diagnostic>& report, CancelCheck = CancelCheck ());
	/* Explicit fallback: render the current stereo master mix from zero to
	 * session end plus tail (0..600 seconds). Tracks, MIDI and processing become
	 * audio; tempo, meter and ordinary markers remain editable. The stem option
	 * prints stereo routes feeding a neutral master, with shared buses grouped. */
	static void export_dawproject (Session&, const std::string& destination,
	                               std::vector<Diagnostic>& report, const ExportOptions&, CancelCheck = CancelCheck ());
	/* Publish an AAF with embedded audio and a coordinated type-1 MIDI file
	 * in a new directory. Both use timeline/timecode origin zero. */
	static void export_logic (Session&, const std::string& destination,
	                          std::vector<Diagnostic>& report, CancelCheck = CancelCheck ());
	static void export_logic (Session&, const std::string& destination,
	                          std::vector<Diagnostic>& report, const ExportOptions&, CancelCheck = CancelCheck ());
	/* A new portable folder with Project.song and collected Media. Native
	 * formats 7, 8 and 9 are experimental pending target-application validation. */
	static void export_studio_one (Session&, const std::string& destination,
	                               std::vector<Diagnostic>& report, const ExportOptions&, CancelCheck = CancelCheck ());
};

} }
#endif
