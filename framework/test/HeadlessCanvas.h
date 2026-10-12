// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A NanoVG context with no GPU behind it: every draw call is accepted and
// discarded. Enough to run an editor's real paint(), font loading and repaint
// logic in a test with no display.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include <nanovg.h>

namespace supple::test {

// An optional hash records stroke geometry for editor drawing regressions.
inline NVGcontext* createHeadlessCanvas(uint64_t* strokeHash = nullptr) {
    NVGparams p{};
    p.userPtr = strokeHash;
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
    p.renderStroke = [](void* user, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float,
                        const NVGpath* paths, int count) {
        if (!user) return;
        auto& hash = *static_cast<uint64_t*>(user);
        for (int p = 0; p < count; ++p) for (int v = 0; v < paths[p].nstroke; ++v) {
            for (float position : {paths[p].stroke[v].x, paths[p].stroke[v].y}) {
                uint32_t bits = 0;
                std::memcpy(&bits, &position, sizeof(bits));
                hash = (hash ^ bits) * UINT64_C(1099511628211);
            }
        }
    };
    p.renderTriangles = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, const NVGvertex*, int,
                           float) {};
    p.renderDelete = [](void*) {};
    return nvgCreateInternal(&p);
}

}  // namespace supple::test
