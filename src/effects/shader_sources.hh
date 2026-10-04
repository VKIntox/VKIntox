#pragma once

#include <vector>
#include <cstdint>

namespace VKIntox
{
    const std::vector<uint32_t> depth_resolve_frag = {
#include "depth_resolve.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_ms_frag = {
#include "depth_resolve_ms.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_alpha_frag = {
#include "depth_resolve_alpha.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_alpha_ms_frag = {
#include "depth_resolve_alpha_ms.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_inverted_frag = {
#include "depth_resolve_inverted.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_inverted_ms_frag = {
#include "depth_resolve_inverted_ms.frag.h"
    };

    // Universal depth resolve shaders (comprehensive mode support)
    const std::vector<uint32_t> depth_resolve_universal_frag = {
#include "depth_resolve_universal.frag.h"
    };

    const std::vector<uint32_t> depth_resolve_universal_ms_frag = {
#include "depth_resolve_universal_ms.frag.h"
    };

    const std::vector<uint32_t> full_screen_triangle_vert = {
#include "full_screen_triangle.vert.h"
    };
} // namespace VKIntox
