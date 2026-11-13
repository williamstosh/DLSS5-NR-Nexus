/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include <iostream>

#include "../adaptive_resolution.hpp"

namespace adaptive_resolution_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;

inline bool Run() {
  bool passed = true;
  const auto check = [&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "adaptive-resolution: " << name << '\n';
      passed = false;
    }
  };
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 2560, .height = 1440};
  floor[0] = {.width = 1267, .height = 713};
  auto ladder = adaptive::BuildLadder(upper, floor, 0.67f);
  check(ladder.count == 8 && std::abs(ladder.coefficients[7] - 0.67f) < 0.001f,
        "ladder includes the exact relative lower bound");
  check(ladder.sizes[7][0] == adaptive::Size{.width = 1715, .height = 965}, "both axes share one coefficient");
  upper[1] = {.width = 10, .height = 10};
  floor[1] = {.width = 20, .height = 20};
  ladder = adaptive::BuildLadder(upper, floor, 0.33f);
  check(ladder.sizes[ladder.count - 1][0] == floor[0], "stage floor constrains relative minimum");
  check(ladder.sizes.back()[1].width == 0 || ladder.sizes[ladder.count - 1][1] == upper[1],
        "a Follow upper bound below the floor never grows");
  upper[1] = floor[1] = {};

  adaptive::Session session;
  session.Prepare({.enabled = true, .target_fps = 20.f}, upper, floor, 1, {}, 1, true, false);
  check(session.settings.target_fps == 30.f, "controller target lower bound is 30 FPS");
  session = {};
  adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1};
  uint64_t sequence = 0;
  constexpr int64_t start = 1'000'000'000;
  const auto tick = [&](int64_t now, double fps, double gpu_us) {
    session.ObserveGpu({.generation = session.status.generation, .frame = ++sequence,
        .sequence = sequence, .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = gpu_us, .valid = true, .stable = true}, now);
    output.serial++;
    output.begin_ns = now - 2'000'000'000;
    output.end_ns = now;
    output.fps = fps;
    session.Prepare({.enabled = true}, upper, floor, 1, output, now, true, false);
    session.Applied(session.status.generation, session.status.requested, now);
  };
  tick(start, 50., 3000.);
  for (int i = 1; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 2 && session.trial, "sustained low FPS probes without a prior NR cost increase");
  const auto lower_generation = session.status.generation;
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 2400.);
  check(session.level == 0 && session.blocked && !session.trial,
        "lower NR cost without output FPS benefit restores quality and stops probing");
  for (int i = 28; i <= 60; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 0 && session.blocked, "a steady cap does not trigger repeated downscaling");
  const size_t samples = session.gpu_count;
  session.ObserveGpu({.generation = lower_generation, .frame = ++sequence, .sequence = sequence,
      .recorded_ns = start, .completed_ns = start, .total_us = 1., .valid = true, .stable = true},
      start + 20'000'000'000);
  check(session.gpu_count == samples, "old generation completions cannot drive the new size");

  session = {};
  sequence = 0;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 2 && !session.trial, "useful lower resolution is accepted");
  for (int i = 28; i <= 54; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 1 && session.trial && session.raising,
        "quality recovers at a locked target after the conservative hold");
  for (int i = 55; i <= 68; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 2 && !session.trial && session.failed_raise_until_ns != 0,
        "unsuccessful quality recovery rolls back with a boundary cooldown");
  session.Prepare({.enabled = false}, upper, floor, 1, output, start + 20'000'000'000, true, false);
  check(session.level == 0 && session.status.requested == upper
      && session.status.reason == adaptive::Reason::DISABLED, "disabling restores the manual upper bound");

  session = {};
  sequence = 0;
  output.observed_multiplier = 2.;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  output.observed_multiplier = 4.;
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 2 && session.trial && session.extra_window,
        "an FG ratio increase gets one extra window instead of accepting an apparent NR benefit");
  for (int i = 28; i <= 38; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 0 && session.blocked && !session.trial,
        "a sustained FG ratio change restores the original quality after the bounded comparison");
  output.observed_multiplier = 0.;

  for (const int frequency : {60, 120, 240}) {
    adaptive::Session profiling;
    profiling.status.generation = 1;
    profiling.applied_ns = start;
    profiling.stable_after_ns = start;
    for (int i = 0; i < frequency * 2; ++i) {
      const int64_t now = start + static_cast<int64_t>(i) * 1'000'000'000 / frequency;
      profiling.ObserveGpu({.generation = 1, .frame = static_cast<uint64_t>(i + 1),
          .sequence = static_cast<uint64_t>(i + 1), .context = 1,
          .recorded_ns = now, .completed_ns = now, .total_us = 3000., .valid = true, .stable = true}, now);
    }
    check(profiling.gpu_count == 5
        && profiling.gpu_recorded_ns.back() - profiling.gpu_recorded_ns.front() >= 1'000'000'000,
        "profiling is downsampled by recording time at 60/120/240 Hz");
  }
  adaptive::Session dual;
  dual.status.generation = 1;
  dual.applied_ns = dual.stable_after_ns = start;
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 2, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 2000.,
      .role = 1, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 0, "a partial dual-color chain never becomes a GPU sample");
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 1, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 1000.,
      .role = 2, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 1 && dual.gpu_us[0] == 3000., "out-of-order color completion produces one complete sum");

  adaptive::Session reused;
  adaptive::Settings settings{.enabled = true, .minimum_scale = .67f, .prepared_levels = 4};
  reused.Prepare(settings, upper, floor, 1, {}, start, true, false, 10);
  const auto original = reused.ladder;
  reused.Request(2, start);
  reused.Applied(reused.status.generation, reused.status.requested, start);
  const auto lower = reused.status.applied;
  const auto generation = reused.status.generation;
  reused.gpu_count = 5;
  reused.blocked = true;
  settings.target_fps = 116.f;
  reused.Prepare(settings, upper, floor, 2, {}, start + 1, true, false, 10);
  check(reused.ladder.sizes == original.sizes && reused.level == 2 && reused.status.requested == lower
      && reused.status.generation > generation && reused.gpu_count == 0 && !reused.blocked,
      "target and measurement changes retain the actual legal tier while restarting observation");
  reused.Fail(reused.status.generation);
  reused.Prepare(settings, upper, floor, 3, {}, start + 2, true, false, 10);
  check(reused.failed, "compatible parameter edits do not clear a real execution failure");
  reused.Prepare(settings, upper, floor, 4, {}, start + 3, true, false, 11);
  check(!reused.failed && reused.level == 0 && reused.resource_identity == 11,
      "a changed allocation contract starts a new tier set and clears the old failure");

  reused.Request(3, start + 4);
  reused.Applied(reused.status.generation, reused.status.requested, start + 4);
  settings.minimum_scale = .8f;
  reused.Prepare(settings, upper, floor, 5, {}, start + 5, true, false, 11);
  check(reused.level == 0 && reused.ladder.sizes == original.sizes && reused.defer_preparation
      && !reused.IsLegal(reused.ladder.sizes[3]) && reused.IsLegal(reused.status.requested),
      "raising the minimum preserves allocations and requests the legal upper tier when the active tier is out of range");
  settings.minimum_scale = .5f;
  reused.Prepare(settings, upper, floor, 6, {}, start + 6, true, false, 11);
  check(reused.ladder.sizes == original.sizes,
      "lowering the minimum does not eagerly replace the resident coarse ladder");
  reused.Request(3, start + 7);
  reused.Applied(reused.status.generation, reused.status.requested, start + 7);
  reused.Request(2, start + 8);
  const auto active_size = reused.ladder.sizes[3];
  const auto requested_size = reused.ladder.sizes[2];
  check(reused.RequestMinimum(start + 9) && reused.level == 1 && reused.active_level == 3
      && reused.ladder.sizes[3] == active_size && reused.ladder.sizes[2] == requested_size
      && reused.status.applied == active_size && reused.status.requested == reused.minimum,
      "a requested new endpoint cannot replace either the actual active slot or the pending requested slot");
  const auto pending_generation = reused.status.generation;
  settings.minimum_scale = .8f;
  reused.Prepare(settings, upper, floor, 7, {}, start + 10, true, false, 11);
  reused.Applied(pending_generation, reused.minimum, start + 11);
  check(reused.level == 0 && reused.status.generation != pending_generation
      && reused.status.requested == upper && !reused.IsLegal(reused.ladder.sizes[1]),
      "a raised minimum cancels the old uncommitted endpoint without accepting its old generation");

  adaptive::Session two;
  settings.minimum_scale = .8f;
  settings.prepared_levels = 2;
  two.Prepare(settings, upper, floor, 1, {}, start, true, false, 20);
  two.Request(1, start + 1);
  two.Applied(two.status.generation, two.status.requested, start + 1);
  const auto old_minimum = two.ladder.sizes[1];
  settings.minimum_scale = .5f;
  two.Prepare(settings, upper, floor, 2, {}, start + 2, true, false, 20);
  check(!two.RequestMinimum(start + 3) && two.level == 0 && two.active_level == 1
      && two.ladder.sizes[1] == old_minimum,
      "two-tier capacity returns to the upper tier before defining a replacement for the active lower slot");
  two.Applied(two.status.generation, upper, start + 4);
  check(two.RequestMinimum(start + 5) && two.level == 1 && two.active_level == 0
      && two.ladder.sizes[1] == two.minimum && two.status.applied == upper,
      "the lower endpoint can be requested after the real upper tier is applied");

  adaptive::Session constrained;
  settings.minimum_scale = .8f;
  adaptive::Output constrained_output{.available = true, .source = 1, .generation = 1, .configuration = 1, .fps = 50.};
  constrained.Prepare(settings, upper, floor, 1, constrained_output, start, true, false, 30);
  constrained.Request(1, start);
  constrained.Applied(constrained.status.generation, constrained.status.requested, start);
  settings.minimum_scale = .5f;
  constrained.Prepare(settings, upper, floor, 2, constrained_output, start + 1, true, false, 30);
  constrained.Applied(constrained.status.generation, constrained.status.requested, start + 1);
  bool returned_upper = false, continued_minimum = false;
  for (int i = 1; i <= 40 && !continued_minimum; ++i) {
    const int64_t now = start + i * adaptive::SAMPLE_INTERVAL_NS;
    constrained.ObserveGpu({.generation = constrained.status.generation, .frame = static_cast<uint64_t>(i),
        .sequence = static_cast<uint64_t>(i), .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = 3000., .valid = true, .stable = true}, now);
    constrained_output.serial++;
    constrained_output.begin_ns = now - 2'000'000'000;
    constrained_output.end_ns = now;
    constrained.Prepare(settings, upper, floor, 2, constrained_output, now, true, false, 30);
    if (constrained.minimum_pending && constrained.level == 0) {
      returned_upper = true;
      check(!constrained.trial, "the two-slot return to upper discards the old lower-tier trial");
    }
    if (constrained.status.requested == constrained.minimum) {
      continued_minimum = true;
      check(returned_upper && constrained.trial && constrained.previous_level == 0
          && constrained.status.applied == upper && constrained.gpu_count == 0
          && constrained.baseline_gpu_us == 3000.,
          "automatic endpoint continuation freezes a fresh upper baseline and clears its samples before the new trial");
    }
    constrained.Applied(constrained.status.generation, constrained.status.requested, now);
  }
  check(returned_upper && continued_minimum,
      "the real controller automatically continues from upper to the requested minimum with only two physical slots");

  adaptive::Session single;
  settings.minimum_scale = 1.f;
  settings.prepared_levels = 4;
  single.Prepare(settings, upper, floor, 1, {}, start, true, false, 40);
  single.Applied(single.status.generation, upper, start);
  check(single.ladder.count == 1, "a 100-percent minimum starts with one upper tier");
  settings.minimum_scale = .5f;
  single.Prepare(settings, upper, floor, 2, {}, start + 1, true, false, 40);
  check(single.ladder.count == 1 && single.status.requested == upper,
      "lowering an initial 100-percent minimum does not eagerly add physical slots");
  check(single.RequestMinimum(start + 2) && single.ladder.count == 2 && single.level == 1
      && single.active_level == 0 && single.status.applied == upper && single.status.requested == single.minimum,
      "a real endpoint request adds one unused slot under the original prepared-level capacity");
  settings.minimum_scale = .67f;
  single.Prepare(settings, upper, floor, 3, {}, start + 3, true, false, 40);
  check(single.level == 0 && single.ladder.count == 2 && single.RequestMinimum(start + 4)
      && single.ladder.count == 2 && single.status.requested == single.minimum,
      "an illegal unused endpoint is replaced without growing the bounded tier set again");

  settings.minimum_scale = .5f;
  settings.prepared_levels = 4;
  reused.Prepare(settings, upper, floor, 8, {}, start + 12, true, false, 11);
  reused.Request(0, start + 12);
  reused.Applied(reused.status.generation, upper, start + 12);
  output = {.available = true, .source = 1, .generation = 1, .configuration = 1, .fps = 50.};
  for (int i = 1; i <= 14; ++i) {
    const int64_t now = start + i * adaptive::SAMPLE_INTERVAL_NS;
    reused.ObserveGpu({.generation = reused.status.generation, .frame = static_cast<uint64_t>(i),
        .sequence = static_cast<uint64_t>(i), .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = 3000., .valid = true, .stable = true}, now);
    output.serial++;
    output.begin_ns = now - 2'000'000'000;
    output.end_ns = now;
    reused.Prepare(settings, upper, floor, 8, output, now, true, false, 11);
    reused.Applied(reused.status.generation, reused.status.requested, now);
  }
  check(reused.level == 2 && reused.ladder.coefficients[1] == .5f,
      "the controller selects the nearest legal coarse size rather than treating physical slot order as quality order");
  return passed;
}
}  // namespace adaptive_resolution_cases
