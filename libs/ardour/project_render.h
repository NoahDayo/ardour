/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_PROJECT_RENDER_H
#define ARDOUR_PROJECT_RENDER_H

#include <string>

#include "ardour/project_archive.h"
#include "ardour/project_exchange.h"
#include "ardour/types.h"

namespace ARDOUR {
class Session;
namespace ProjectExchange {

struct RenderedMix {
	std::string path;
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

} }
#endif
