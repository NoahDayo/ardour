/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_PROJECT_RENDER_H
#define ARDOUR_PROJECT_RENDER_H

#include <string>
#include <memory>
#include <vector>

#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"
#include "ardour/types.h"

namespace ARDOUR {
class Session;
class Route;
namespace ProjectExchange {

struct RenderedMix {
	std::string path;
	std::string route_id;
	uint32_t channels = 0;
	samplecnt_t frames = 0;
	samplecnt_t sample_rate = 0;
};

/* A backend that refuses both freewheel shutdown and engine stop cannot be
 * safely finalized. Keep its open staging files and protected controls alive. */
class RenderShutdownFailed : public ExchangeError {
public:
	explicit RenderShutdownFailed (const std::string& message) : ExchangeError (message) {}
};

/* The caller owns the stage directory and suspends editing on the session's
 * control thread. Length includes the selected tail, starting at sample zero. */
RenderedMix render_session_mix (Session&, const std::string& stage_directory,
                               samplecnt_t length, const CancelCheck&);

/* A stem is one complete stereo route feeding the master. Upstream shared
 * processing stays in its bus stem; the master itself must be neutral. */
std::vector<std::shared_ptr<Route>> master_stem_routes (Session&);
std::vector<RenderedMix> render_session_stems (Session&, const std::string& stage_directory,
                                              samplecnt_t length, const CancelCheck&);

} }
#endif
