/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <string>

namespace Gtk { class Window; }

namespace ProjectExchangeUI {
struct ImportRequest {
	std::string archive;
	std::string destination;
	std::string snapshot;
	double sample_rate = 48000;
};
bool is_importable_project (std::string const& path);
bool prepare_import (Gtk::Window*, std::string const& archive, ImportRequest&);
bool import_prepared (Gtk::Window*, ImportRequest const&, bool offer_session_dialog);
void import_dawproject (Gtk::Window* parent);
void export_dawproject (Gtk::Window* parent);
void export_logic (Gtk::Window* parent);
}
