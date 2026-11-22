/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <span>

namespace renodx::addons::dlss5::internal {

enum class EditReuseBlock : uint8_t {
  NONE, PASSES, MASK, ADAPTIVE, ROUTE, INPUTS, GEOMETRY, ORDER, HISTORY, NR_FAILED, SELECT_ORIGINAL, SR_FAILED,
};

namespace edit_reuse_policy {

struct Source {
  uint64_t generation = 0;
  uint64_t sequence = 0;
  bool current_color = false;
  bool ordered = false;
};

constexpr bool CanUseHistory(const Source& source) {
  return source.generation != 0 && source.sequence != 0 && source.current_color && source.ordered;
}

enum class Anchors : uint8_t { NONE, TERMINAL, PER_LAYER };

struct Layer {
  float transfer_strength = 0.f;
  float color_strength = 0.f;
  bool temporal_look = false;
  uint32_t working_width = 0;
  uint32_t working_height = 0;
  uint32_t full_width = 0;
  uint32_t full_height = 0;
};

struct Decision {
  Anchors anchors = Anchors::NONE;
  EditReuseBlock reason = EditReuseBlock::NONE;
};

constexpr Decision DecideGroup(std::span<const Layer> layers, uint32_t encoding) {
  bool active_layers = !layers.empty();
  bool temporal_look = false;
  bool full_working_grid = true;
  for (const auto& layer : layers) {
    active_layers &= layer.transfer_strength > 0.f || layer.color_strength > 0.f;
    temporal_look |= layer.temporal_look;
    full_working_grid &= layer.working_width == layer.full_width && layer.working_height == layer.full_height;
  }
  if (!active_layers) return {.reason = EditReuseBlock::PASSES};
  // Codec encoding 1 is linear HDR. Temporal anchors use the full-grid
  // validity footprint across the complete group, including spatial layers.
  if (encoding != 1u || (temporal_look && !full_working_grid)) return {.reason = EditReuseBlock::GEOMETRY};
  return {.anchors = (temporal_look ? Anchors::PER_LAYER : Anchors::TERMINAL)};
}

}  // namespace edit_reuse_policy
}  // namespace renodx::addons::dlss5::internal
