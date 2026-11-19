/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

#include <edit_reuse_policy.hpp>

int main() {
  namespace policy = renodx::addons::dlss5::internal::edit_reuse_policy;
  using Block = renodx::addons::dlss5::internal::EditReuseBlock;
  using policy::Anchors;
  constexpr policy::Layer full_layer{.transfer_strength = 1.f, .color_strength = 1.f,
      .working_width = 16, .working_height = 16, .full_width = 16, .full_height = 16};
  uint32_t cases = 0;
  uint32_t failures = 0;
  const auto check = [&](std::string_view name, std::span<const policy::Layer> layers, uint32_t encoding,
                         Anchors anchors, Block reason = Block::NONE) {
    ++cases;
    const auto decision = policy::DecideGroup(layers, encoding);
    if (decision.anchors != anchors || decision.reason != reason) {
      ++failures;
      std::cerr << "FAIL edit reuse policy: " << name << '\n';
    }
  };

  std::array layers{full_layer, full_layer, full_layer, full_layer};
  check("spatial full group", std::span{layers}.first(2), 1, Anchors::TERMINAL);
  layers[1].working_width = layers[1].working_height = 12;
  check("spatial unequal grids", std::span{layers}.first(2), 1, Anchors::TERMINAL);
  for (uint32_t temporal_pass = 0; temporal_pass < 2; ++temporal_pass) {
    layers.fill(full_layer);
    layers[temporal_pass].temporal_look = true;
    check("one full temporal layer", std::span{layers}.first(2), 1, Anchors::PER_LAYER);
    for (uint32_t smaller_pass = 0; smaller_pass < 2; ++smaller_pass) {
      layers[smaller_pass].working_width = 12;
      check("temporal Look and smaller grid anywhere in group", std::span{layers}.first(2), 1,
            Anchors::NONE, Block::GEOMETRY);
      layers[smaller_pass].working_width = 16;
    }
  }
  layers.fill(full_layer);
  layers[0].temporal_look = layers[1].temporal_look = true;
  check("both temporal layers", std::span{layers}.first(2), 1, Anchors::PER_LAYER);
  layers[1].working_height = 12;
  check("height alone invalidates temporal group", std::span{layers}.first(2), 1, Anchors::NONE, Block::GEOMETRY);
  layers.fill(full_layer);
  layers[0].transfer_strength = 0.f;
  layers[1].color_strength = 0.f;
  check("either real execution strength activates a layer", std::span{layers}.first(2), 1, Anchors::TERMINAL);
  layers[0].color_strength = 0.f;
  check("inactive first layer", std::span{layers}.first(2), 1, Anchors::NONE, Block::PASSES);
  layers.fill(full_layer);
  layers[1].transfer_strength = layers[1].color_strength = 0.f;
  check("inactive tail layer", std::span{layers}.first(2), 1, Anchors::NONE, Block::PASSES);
  check("inactive reason precedes encoding", std::span{layers}.first(2), 4, Anchors::NONE, Block::PASSES);
  layers.fill(full_layer);
  check("PQ group retains per-frame fallback", std::span{layers}.first(2), 4, Anchors::NONE, Block::GEOMETRY);
  check("SDR group retains per-frame fallback", std::span{layers}.first(2), 3, Anchors::NONE, Block::GEOMETRY);
  layers[2] = {.temporal_look = true, .working_width = 12, .full_width = 16};
  layers[3] = {};
  check("dormant layers are outside the active group", std::span{layers}.first(2), 1, Anchors::TERMINAL);
  layers[0].temporal_look = true;
  check("dormant layers do not affect temporal anchors", std::span{layers}.first(2), 1, Anchors::PER_LAYER);

  if (failures != 0) return 1;
  std::cout << "PASS edit reuse policy evidence=CPU cases=" << cases << '\n';
  return 0;
}
