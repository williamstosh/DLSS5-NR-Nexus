/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

#include <utils/hash.hpp>

namespace inline_execution_trace {

inline constexpr uint32_t WIDTH = 16;
inline constexpr uint32_t HEIGHT = 16;
inline constexpr std::array<float, 4> MODEL_RGBA{.85f, .75f, .9f, .875f};

inline constexpr auto SOURCE_RGBA = [] {
  constexpr std::array<uint16_t, 4> ALPHA{0x3400, 0x3600, 0x3800, 0x3900};
  std::array<uint16_t, WIDTH * HEIGHT * 4> rgba{};
  for (std::size_t pixel = 0; pixel < WIDTH * HEIGHT; ++pixel) {
    rgba[pixel * 4] = static_cast<uint16_t>(0x3400 + 2 * pixel);
    rgba[pixel * 4 + 1] = 0x3600;
    rgba[pixel * 4 + 2] = 0x3800;
    rgba[pixel * 4 + 3] = ALPHA[pixel % ALPHA.size()];
  }
  return rgba;
}();

struct Base {
  uint64_t base = 0;
  uint32_t calls = 0;
  int reset = -1;
  uint64_t history = 0;
  uint64_t anchor_base = 0;
  uint64_t anchor_generation = 0;
  uint64_t anchor_age = 0;
  bool complete = false;

  bool operator==(const Base&) const = default;
};

// Call after verifying the fixed single-pass plan. Sequence and generation
// values here are relative to the end of warmup in the observed route.
inline void PrintBase(const char* route, const Base& frame, std::span<const uint8_t> rgba) {
  std::cout << "INLINE_POLICY_BASE route=" << route << " base=" << frame.base
            << " plan_stage=Upscaled pass_begin=0 pass_count=1 model=2 calls=" << frame.calls << " reset=";
  if (frame.reset < 0) {
    std::cout << '-';
  } else {
    std::cout << frame.reset;
  }
  std::cout << " history=" << frame.history << " anchor_base=" << frame.anchor_base
            << " anchor_generation=" << frame.anchor_generation << " anchor_age=" << frame.anchor_age
            << " complete=" << static_cast<unsigned>(frame.complete)
            << " rgba_crc32=" << renodx::utils::hash::ComputeCRC32(rgba.data(), rgba.size()) << '\n';
}

}  // namespace inline_execution_trace
