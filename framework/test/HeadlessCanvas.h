// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A NanoVG context with no GPU behind it: every draw call is accepted and
// discarded. Enough to run an editor's real paint(), font loading and repaint
// logic in a test with no display.
#pragma once

#include <nanovg.h>

namespace supple::test {

inline NVGcontext* createHeadlessCanvas() {
    NVGparams p{};
    p.renderCreate = [](void*) { return 1; };
    p.renderCreateTexture = [](void*, int, int, int, int, const unsigned char*) { return 1; };
    p.renderDeleteTexture = [](void*, int) { return 1; };
    p.renderUpdateTexture = [](void*, int, int, int, int, int, const unsigned char*) { return 1; };
    p.renderGetTextureSize = [](void*, int, int* w, int* h) {
        *w = *h = 512;
        return 1;
    };
    p.renderViewport = [](void*, float, float, float) {};
    p.renderCancel = [](void*) {};
    p.renderFlush = [](void*) {};
    p.renderFill = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, const float*, const NVGpath*,
                      int) {};
    p.renderStroke = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float, const NVGpath*,
                        int) {};
    p.renderTriangles = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, const NVGvertex*, int,
                           float) {};
    p.renderDelete = [](void*) {};
    return nvgCreateInternal(&p);
}

}  // namespace supple::test
