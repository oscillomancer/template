// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// For the in-process plugin tests only: reach what an open editor would,
// without opening a window. Hidden like everything else; never exported.
#pragma once

#include <clap/clap.h>

namespace supple::gui {
class EditorHost;
}

// The plugin's editor-facing interface (null without a GUI build).
supple::gui::EditorHost* supple_test_editor_host(const clap_plugin_t* plugin);
// What opening and closing an editor does to telemetry.
void supple_test_set_metering(const clap_plugin_t* plugin, bool on);
