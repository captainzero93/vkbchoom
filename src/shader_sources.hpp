#pragma once

#include <vector>
#include <cstdint>

namespace vkbChoom
{
    const std::vector<uint32_t> bwr_frag = {
#include "bwr.frag.h"
    };

    const std::vector<uint32_t> depth_outline_frag = {
#include "depth_outline.frag.h"
    };

    const std::vector<uint32_t> full_screen_triangle_vert = {
#include "full_screen_triangle.vert.h"
    };

    const std::vector<uint32_t> posterize_frag = {
#include "posterize.frag.h"
    };

    const std::vector<uint32_t> rcas_frag = {
#include "rcas.frag.h"
    };

    const std::vector<uint32_t> smaa_blend_frag = {
#include "smaa_blend.frag.h"
    };

    const std::vector<uint32_t> smaa_blend_vert = {
#include "smaa_blend.vert.h"
    };

    const std::vector<uint32_t> smaa_edge_color_frag = {
#include "smaa_edge_color.frag.h"
    };

    const std::vector<uint32_t> smaa_edge_luma_frag = {
#include "smaa_edge_luma.frag.h"
    };

    const std::vector<uint32_t> smaa_edge_vert = {
#include "smaa_edge.vert.h"
    };

    const std::vector<uint32_t> smaa_neighbor_frag = {
#include "smaa_neighbor.frag.h"
    };

    const std::vector<uint32_t> smaa_neighbor_vert = {
#include "smaa_neighbor.vert.h"
    };
} // namespace vkbChoom
