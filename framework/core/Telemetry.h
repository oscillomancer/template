// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What the audio thread tells an open editor, at Processor::kTelemetryHz.
//
// The framework measures the main input and output itself -- sample peak,
// per channel, over each frame's interval, output measured after bypass --
// so every product gets honest I/O meters without writing any. A product
// adds its own payload by declaring Processor::Telemetry.
#pragma once

#include "core/Ring.h"
#include "Processor.h"

namespace supple {

inline constexpr int kMeterChannels = 2;
inline constexpr double kTelemetryHz = product::Processor::kTelemetryHz;

struct TelemetryFrame {
    float inputPeak[kMeterChannels]{};   // linear sample peak since the previous frame
    float outputPeak[kMeterChannels]{};
    int channels = 0;
    product::Processor::Telemetry product{};
};

// An editor drains this every frame, so it only fills when the editor stalls;
// a full queue drops the newest frame.
using TelemetryRing = Ring<TelemetryFrame, product::Processor::kTelemetryQueueLog2>;

}  // namespace supple
