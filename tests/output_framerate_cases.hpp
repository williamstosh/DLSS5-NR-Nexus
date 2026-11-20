/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include "adaptive_resolution.hpp"
#include "output_framerate.hpp"

template <typename Check>
void RunOutputFramerateCases(const Check& check) {
  namespace rate = renodx::addons::dlss5::output_framerate;
  constexpr int64_t start = 1'000'000'000;
  constexpr int64_t step = 10'000'000;
  rate::Stream stream;
  for (int64_t index = 0; index <= 200; ++index) {
    stream.Observe(rate::Source::XEFG_STATUS, 1, 2, start + index * step, 3, 4, 3);
  }
  auto snapshot = stream.Read(start + 200 * step);
  check(snapshot.available && std::abs(snapshot.fps - 300) < 0.001
          && snapshot.window_begin_ns == start && snapshot.window_end_ns == start + 200 * step
          && snapshot.fps_mad == 0 && snapshot.requested_multiplier == 4 && snapshot.observed_multiplier == 3
          && snapshot.identity == 1,
      "SDK counts establish four complete output bins independently of requested multiplier");
  const auto generation = snapshot.source_generation;
  stream.Observe(rate::Source::XEFG_STATUS, 1, 3, start + 201 * step, 4, 4, 4);
  check(!stream.Read(start + 201 * step).available && stream.snapshot.source_generation > generation,
      "configuration changes discard the previous output window");
  for (int64_t index = 202; index <= 401; ++index) {
    stream.Observe(rate::Source::XEFG_STATUS, 1, 3, start + index * step, 0, 4, 0);
  }
  snapshot = stream.Read(start + 401 * step);
  check(snapshot.available && snapshot.fps == 0 && snapshot.observed_multiplier == 0,
      "zero SDK frames remain a valid measured zero rather than missing telemetry");
  check(!stream.Read(start + 502 * step).available, "stale output samples are unavailable");
  stream.Reset(rate::Reason::kDUPLICATE);
  check(!stream.snapshot.available && stream.snapshot.reason == rate::Reason::kDUPLICATE,
      "duplicate application frames clear the output window");
  stream.Observe(rate::Source::PRESENT, 2, 3, start + 503 * step, 1, 1, 1);
  check(stream.snapshot.source == rate::Source::PRESENT && !stream.snapshot.available,
      "source changes warm a fresh window instead of retaining SDK counts");
  stream.Observe(rate::Source::PRESENT, 2, 3, start + 503 * step, 1, 1, 1);
  check(!stream.snapshot.available, "non-monotonic observations cannot complete a window");
  rate::Stream fractional;
  for (int64_t index = 0; index <= 200; ++index) {
    fractional.Observe(rate::Source::XEFG_STATUS, 1, 1, start + index * step,
        (index % 10 < 7 ? 4 : 3), 4);
  }
  check(std::abs(fractional.Read(start + 200 * step).observed_multiplier - 3.7) < 0.001,
      "observed XeFG multiplier is the full-window output to application count ratio");

  rate::Stream sdk_off;
  for (int64_t index = 0; index <= 200; ++index) {
    sdk_off.Observe(rate::Source::XEFG_STATUS, 77, 9, start + index * step, 1, 3, 1, false, 4, true);
  }
  snapshot = sdk_off.Read(start + 200 * step);
  check(snapshot.available && snapshot.identity == 77 && snapshot.source == rate::Source::XEFG_STATUS
          && snapshot.fg_off && std::abs(snapshot.fps - 100.) < 0.001 && snapshot.observed_multiplier == 1.
          && snapshot.requested_multiplier == 3,
      "SDK Off counts retain the output context identity and SDK source independently of its requested multiplier");
  sdk_off.Observe(rate::Source::XEFG_STATUS, 78, 9, start + 201 * step, 1, 3, 1, false, 4, true);
  snapshot = sdk_off.Read(start + 201 * step);
  check(!snapshot.available && snapshot.identity == 78 && snapshot.fg_off,
      "a different SDK context exposes its own identity and cannot inherit the preceding FPS window");
  snapshot = sdk_off.Read(start + 302 * step);
  check(!snapshot.available && !snapshot.fg_off,
      "stale SDK output cannot retain positive Off sample metadata");

  const uintptr_t native_back_buffers[] = {90, 91};
  rate::RegisterPresentOutput(9, 1, 900, native_back_buffers);
  const auto native_output = rate::MatchPresentOutput(9, 90);
  const auto observe_state = [](uint32_t viewport, uint32_t frames, int64_t now) {
    rate::ObserveNativeState(rate::BeginNativeState(viewport), true, frames, now);
  };
  const auto observe_application = [&](uint32_t frame, int64_t now) {
    rate::ObserveNativePresent(native_output, true, now, false, std::nullopt,
        rate::NativeApplicationFrame{.generation = 1, .index = frame});
  };
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 1, 2);
  constexpr int64_t fixed_step = 25'000'000;
  for (int64_t index = 0; index <= 80; ++index) {
    const auto now = start + index * fixed_step;
    rate::ObserveNativeEvaluation(native_output, 2, true, now);
    observe_application(static_cast<uint32_t>(index), now);
  }
  snapshot = rate::Read(native_output, start + 80 * fixed_step);
  check(snapshot.available && snapshot.source == rate::Source::NATIVE_FIXED
          && std::abs(snapshot.fps - 80) < 0.001 && snapshot.observed_multiplier == 0,
      "matched fixed native mode estimates 80 FPS from 40 identified application frames");
  const auto fixed_generation = snapshot.source_generation;
  const auto fixed_end = start + 80 * fixed_step;
  check(!rate::Read(native_output, fixed_end + rate::Stream::STALE_NS + 1).available,
      "fixed estimate expires with its active execution and application cadence");
  rate::ObserveNativeOptions(8, 5, 1, 5);
  observe_state(8, 100, fixed_end);
  check(rate::Read(native_output, fixed_end).source_generation == fixed_generation,
      "another viewport's Options and GetState leave fixed evidence on this output intact");
  rate::ObserveNativeEvaluation(native_output, 4, true, fixed_end + 1);
  snapshot = rate::Read(native_output, fixed_end + 1);
  check(!snapshot.available && snapshot.reason == rate::Reason::kUNKNOWN_FG
          && snapshot.source_generation > fixed_generation,
      "conflicting successful native declaration immediately revokes the fixed estimate");
  rate::ObserveNativeEvaluation(native_output, 1, true, fixed_end + 2);
  observe_application(81, fixed_end + 2);
  check(!rate::Read(native_output, fixed_end + 2).available,
      "a successful one-frame declaration cannot retain a multi-frame fixed estimate");
  rate::ObserveNativeOptions(7, 5, 3, 4);
  observe_application(82, fixed_end + step);
  check(!rate::Read(native_output, fixed_end + step).available
          && rate::Read(native_output, fixed_end + step).reason == rate::Reason::kUNKNOWN_FG,
      "dynamic mode cannot fall back to a fixed multiplier");
  for (int64_t index = 0; index <= 200; ++index) {
    observe_state(7, 2, fixed_end + (2 + index) * step);
  }
  snapshot = rate::Read(native_output, fixed_end + 202 * step);
  check(snapshot.available && snapshot.source == rate::Source::NATIVE_STATE && snapshot.dynamic
          && std::abs(snapshot.fps - 200) < 0.001 && snapshot.multiplier_variable
          && snapshot.observed_multiplier == 0,
      "passive SDK output counts support dynamic FG independently of its request");
  const auto sdk_serial = snapshot.window_serial;
  const auto sdk_generation = snapshot.source_generation;
  rate::ObserveNativeOptions(8, 5, 1, 7);
  observe_state(8, 100, fixed_end + 203 * step);
  rate::ObserveNativeOptions(7, 5, 3, 6);
  snapshot = rate::Read(native_output, fixed_end + 203 * step);
  check(snapshot.available && snapshot.window_serial == sdk_serial && snapshot.source_generation == sdk_generation
          && snapshot.requested_multiplier == 6,
      "same-path native request and another viewport's observations preserve the actual counts");

  const auto xe_output = rate::RegisterXeFGOutput(10, 1, 11, 0);
  rate::ObserveXeFGMetadata(xe_output, 4, 0);
  rate::ObserveXeFG(xe_output, 1, start, false, true, 4, true, false);
  check(!rate::Read(xe_output, start).available,
      "failed or occluded proxy Present cannot publish the SDK count");
  for (uint64_t index = 0; index <= 200; ++index) {
    rate::ObserveXeFG(xe_output, index + 2, start + static_cast<int64_t>(index) * step, true, true,
        (index % 10 < 7 ? 4u : 3u), true, false);
  }
  snapshot = rate::Read(xe_output, start + 200 * step);
  check(snapshot.available && snapshot.requested_multiplier == 0 && snapshot.maximum_multiplier == 4
          && std::abs(snapshot.observed_multiplier - 3.7) < 0.001,
      "XeFG without optional setter retains observed multiplier and separate init ceiling");
  rate::ObserveXeFGMetadata(xe_output, 4, 2);
  rate::ObserveXeFG(xe_output, 202, start + 201 * step, true, true, 123, false, false);
  const auto unchanged = rate::Read(xe_output, start + 201 * step);
  check(unchanged.available && unchanged.window_serial == snapshot.window_serial
          && unchanged.source_generation == snapshot.source_generation && unchanged.requested_multiplier == 2,
      "independent XeFG request metadata and an already-accounted result preserve the measured window");
  rate::ReleaseXeFGOutput(xe_output);
  const auto xe_replacement = rate::RegisterXeFGOutput(10, 1, 11, 0);
  rate::ObserveXeFG(xe_output, 203, start + 202 * step, true, true, 100, true, false);
  check(xe_replacement != xe_output && !rate::IsCurrentOutput(xe_output)
          && rate::Read(xe_replacement, start + 202 * step).source == rate::Source::NONE,
      "a late receipt cannot warm the replacement XeFG generation");
  for (uint64_t index = 0; index <= 200; ++index) {
    rate::ObserveXeFG(xe_replacement, index + 1, start + static_cast<int64_t>(index) * step,
        true, true, 2, index % 2 == 0, false);
  }
  snapshot = rate::Read(xe_replacement, start + 200 * step);
  check(snapshot.available && std::abs(snapshot.fps - 200) < 0.001
          && std::abs(snapshot.observed_multiplier - 4) < 0.001,
      "separate successful XeFG Presents of the same application frame increase only output count");
  rate::ReleaseXeFGOutput(xe_replacement);

  const uintptr_t back_buffers_a[] = {30, 31};
  const uintptr_t back_buffers_b[] = {32, 33};
  rate::RegisterPresentOutput(21, 1, 210, back_buffers_a);
  rate::RegisterPresentOutput(22, 1, 220, back_buffers_b);
  const auto output_a = rate::MatchPresentOutput(21, 30);
  const auto output_b = rate::MatchPresentOutput(22, 32);
  check(!rate::MatchPresentOutput(21, 32) && rate::MatchPresentOutput(21, 31) == output_a,
      "different chains and rotating buffers resolve by actual resource and lifecycle");
  const rate::SourceIdentity source{.handle = 99, .generation = 1, .device = 1,
      .resource = 35, .resource_generation = 1};
  rate::BindSourceOutput(source, output_a);
  auto reused_resource = source;
  ++reused_resource.resource_generation;
  check(rate::FindSourceOutput(source) == output_a && !rate::FindSourceOutput(reused_resource),
      "a new SR resource at the same address must establish its own output relation");
  rate::BindSourceOutput(reused_resource, output_b);
  check(rate::FindSourceOutput(reused_resource) == output_b && rate::FindSourceOutput(source) == output_a,
      "only the actual SR resource generation supplies an output identity");
  rate::ObserveNativeOptions(91, 5, 0, 0);
  rate::ObserveNativeOptions(92, 5, 0, 0);
  check(rate::BindNativeOutputFromSource(91, source) && rate::NativeMode(output_a) == 0
            && !rate::BindNativeOutputFromSource(91, reused_resource)
            && !rate::BindNativeOutputFromSource(92, source) && !rate::NativeMode(output_b)
            && rate::BindNativeOutputFromSource(91, source),
      "confirmed Off follows the live SR relation without a Backbuffer tag and preserves conflicting owners");
  auto direct_back_buffer = source;
  direct_back_buffer.resource = back_buffers_b[0];
  check(rate::BindNativeOutputFromSource(92, direct_back_buffer) && rate::NativeMode(output_b) == 0,
      "the exact current SR backbuffer independently links an unbound viewport");
  const rate::SourceIdentity local_source{.handle = 100, .generation = 1, .device = 1,
      .resource = 37, .resource_generation = 2};
  const auto local_owner = rate::FindPresentOwner(local_source.device, back_buffers_a[0]);
  check(local_owner == output_a && !rate::BindNativeLocalSourceOutput(92, local_source, local_owner)
            && rate::BindNativeLocalSourceOutput(91, local_source, local_owner)
            && rate::FindSourceOutput(local_source) == local_owner && !rate::CaptureNativeLegacySourceTag(91),
      "a successful local tag pair binds its exact output without creating global tags or moving another viewport");
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObservePresent(output_a, start + index * step);
    if (index % 50 == 0) {
      rate::ObservePresent(output_b, start + index * step);
    }
  }
  snapshot = rate::Read(output_a, start + 200 * step);
  rate::DestroyPresentOutput(22);
  rate::ObserveNativeOptions(8, 5, 1, 5);
  const auto after_side = rate::Read(output_a, start + 200 * step);
  check(snapshot.available && snapshot.source == rate::Source::PRESENT && snapshot.fps == 100
          && after_side.window_begin_ns == snapshot.window_begin_ns
          && after_side.source_generation == snapshot.source_generation
          && after_side.window_serial == snapshot.window_serial && !rate::IsCurrentOutput(output_b),
      "side-chain Presents, destruction and unrelated Native requests leave the main output window intact");
  rate::RegisterPresentOutput(22, 1, 220, back_buffers_b);
  const auto replacement_b = rate::MatchPresentOutput(22, 32);
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObservePresent(replacement_b, start + (320 + index) * step);
  }
  check(replacement_b != output_b && rate::Read(replacement_b, start + 520 * step).available
          && !rate::Read(output_a, start + 520 * step).available,
      "a warmed replacement chain cannot provide the former main output's stale sample");
  rate::DestroyPresentOutput(21);
  rate::RegisterPresentOutput(21, 1, 210, back_buffers_a);
  check(rate::MatchPresentOutput(21, 30) != output_a && !rate::IsCurrentOutput(output_a)
            && !rate::FindSourceOutput(local_source) && !rate::BindNativeLocalSourceOutput(91, local_source, local_owner),
      "resize retires the old output even when swapchain and resource addresses are reused");
  rate::DestroyPresentOutput(21);
  rate::DestroyPresentOutput(22);

  rate::Stream slow_sdk;
  constexpr int64_t sdk_period = 1'050'000'000;
  for (int64_t index = 0; index <= 4; ++index) {
    slow_sdk.Observe(rate::Source::NATIVE_STATE, 1, 1, start + index * sdk_period, 105);
  }
  snapshot = slow_sdk.Read(start + 4 * sdk_period + 1'100'000'000);
  check(snapshot.available && std::abs(snapshot.fps - 100.) < 0.001
          && snapshot.window_end_ns - snapshot.window_begin_ns == 4 * sdk_period,
      "native SDK intervals slightly slower than 1 Hz retain complete intervals and a fresh window");
  check(!slow_sdk.Read(start + 4 * sdk_period + 2'000'000'001).available,
      "native SDK observations still expire after the bounded two-second interval");
  rate::Stream fast_present;
  for (int64_t index = 0; index <= 200; ++index) {
    fast_present.Observe(rate::Source::PRESENT, 1, 1, start + index * step, 1);
  }
  check(!fast_present.Read(start + 200 * step + 1'100'000'000).available,
      "the longer SDK allowance does not extend ordinary Present freshness");

  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  const auto controller_output = [](const rate::Snapshot& sample) {
    return adaptive::Output{.available = sample.available, .source = static_cast<uint64_t>(sample.source),
        .generation = sample.source_generation, .configuration = sample.configuration_generation,
        .serial = sample.window_serial, .begin_ns = sample.window_begin_ns, .end_ns = sample.window_end_ns,
        .fps = sample.fps, .mad = sample.fps_mad, .observed_multiplier = sample.observed_multiplier,
        .multiplier_variable = sample.multiplier_variable};
  };
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 1920, .height = 1080};
  floor[0] = {.width = 634, .height = 356};
  const adaptive::Settings controller_settings{.enabled = true, .target_fps = 250.f, .prepared_levels = 4};
  rate::ReleaseNative();
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 3, 4);
  adaptive::Session unpaired;
  for (int64_t index = 0; index <= 400; ++index) {
    const int64_t now = start + index * step;
    rate::ObserveNativePresent(native_output, true, now);
    observe_state(7, 2, now);
    unpaired.ObserveGpu({.generation = unpaired.status.generation,
        .frame = static_cast<uint64_t>(index + 1), .sequence = static_cast<uint64_t>(index + 1),
        .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = 3000., .valid = true, .stable = true,
        .sampling_generation = unpaired.sampling_generation}, now);
    unpaired.Prepare(controller_settings, upper, floor, 1,
        controller_output(rate::Read(native_output, now)), now, true, false);
    unpaired.UpdatePreparation(unpaired.preparation_epoch, static_cast<uint32_t>(unpaired.ladder.count),
        static_cast<uint32_t>(unpaired.ladder.count), adaptive::Reason::PREPARING, now);
    unpaired.Applied(unpaired.status.generation, unpaired.status.requested, now);
  }
  check(rate::Read(native_output, start + 400 * step).available && unpaired.level == 0 && !unpaired.trial
          && unpaired.status.reason == adaptive::Reason::COLLECTING,
      "successful outer SDK calls without frame identity cannot provide a dynamic denominator or start a trial");

  rate::ReleaseNative();
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 3, 4);
  observe_application(0, start);
  observe_state(7, 0, start + 1);
  adaptive::Session paired;
  bool raised_multiplier = false, accepted_confounded_quality = false, saw_native_ratio = false;
  uint32_t queued_frames = 0;
  for (int64_t index = 1; index <= 2200; ++index) {
    const int64_t now = start + index * step;
    observe_application(static_cast<uint32_t>(index), now);
    observe_application(static_cast<uint32_t>(index), now + 1);
    queued_frames += (raised_multiplier ? 4u : 2u);
    if (index % 105 == 0) {
      observe_state(7, queued_frames, now + 1);
      queued_frames = 0;
    }
    const auto sample = rate::Read(native_output, now + 1);
    saw_native_ratio |= sample.available && std::abs(sample.observed_multiplier - 2.) < 0.001;
    paired.ObserveGpu({.generation = paired.status.generation,
        .frame = static_cast<uint64_t>(index), .sequence = static_cast<uint64_t>(index),
        .context = 1, .recorded_ns = now, .completed_ns = now + 1,
        .total_us = (paired.level == 0 ? 3000. : 2900.), .valid = true, .stable = true,
        .sampling_generation = paired.sampling_generation}, now + 1);
    paired.Prepare(controller_settings, upper, floor, 1,
        controller_output(sample), now + 1, true, false);
    paired.UpdatePreparation(paired.preparation_epoch, static_cast<uint32_t>(paired.ladder.count),
        static_cast<uint32_t>(paired.ladder.count), adaptive::Reason::PREPARING, now + 1);
    paired.Applied(paired.status.generation, paired.status.requested, now + 1);
    if (paired.trial && paired.level != 0) raised_multiplier = true;
    accepted_confounded_quality |= raised_multiplier && paired.level != 0 && !paired.trial;
  }
  snapshot = rate::Read(native_output, start + 2200 * step + 1);
  check(saw_native_ratio && raised_multiplier && !accepted_confounded_quality && paired.level == 0
          && snapshot.available && snapshot.source == rate::Source::NATIVE_STATE
          && std::abs(snapshot.observed_multiplier - 4.) < 0.001,
      "identified native frames and their same-frame re-deliveries preserve coverage and reject a 2x to 4x false NR benefit");
  const auto native_generation = snapshot.source_generation;
  rate::ObserveNativePresent(native_output, true, start + 2201 * step);
  snapshot = rate::Read(native_output, start + 2201 * step);
  check(snapshot.available && snapshot.observed_multiplier == 0 && snapshot.source_generation == native_generation,
      "losing the frame identity withdraws the observed ratio while preserving the measured SDK FPS");
  rate::ObserveNativePresent(native_output, false, start + 2201 * step);
  snapshot = rate::Read(native_output, start + 2201 * step);
  check(!snapshot.available && snapshot.source_generation > native_generation,
      "a matched application Present failure invalidates the old SDK multiplier window");
  rate::ReleaseNative();
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 1, 2);
  adaptive::Session fixed_counts;
  constexpr adaptive::Settings target{.enabled = true, .target_fps = 100.f, .prepared_levels = 2};
  constexpr int64_t interval_ns = 1'500'000'000;
  bool fixed_trial_started = false;
  bool fixed_trial_accepted = false;
  for (int64_t index = 0; index <= 2400; ++index) {
    const int64_t ns = start + index * step;
    rate::ObserveNativeEvaluation(native_output, 2, true, ns);
    if ((index * step) % interval_ns == 0) {
      observe_state(7, fixed_counts.level != 0 ? 150u : 120u, ns);
    }
    const auto sample = rate::Read(native_output, ns);
    fixed_counts.ObserveGpu({.generation = fixed_counts.status.generation,
        .frame = static_cast<uint64_t>(index + 1), .sequence = static_cast<uint64_t>(index + 1),
        .context = 1, .recorded_ns = ns, .completed_ns = ns,
        .total_us = (fixed_counts.level == 0 ? 3100. : 2200.), .valid = true, .stable = true,
        .sampling_generation = fixed_counts.sampling_generation}, ns);
    fixed_counts.Prepare(target, upper, floor, 1, controller_output(sample), ns, true, false);
    fixed_counts.UpdatePreparation(fixed_counts.preparation_epoch, static_cast<uint32_t>(fixed_counts.ladder.count),
        static_cast<uint32_t>(fixed_counts.ladder.count), adaptive::Reason::PREPARING, ns);
    fixed_counts.Applied(fixed_counts.status.generation, fixed_counts.status.requested, ns);
    fixed_trial_started |= sample.available && !sample.multiplier_variable && sample.observed_multiplier == 0
        && fixed_counts.level != 0 && fixed_counts.trial;
    fixed_trial_accepted |= fixed_trial_started && fixed_counts.level != 0 && !fixed_counts.trial;
  }
  check(fixed_trial_started && fixed_trial_accepted,
      "fixed SDK counts can trial and accept 80 to 100 FPS without an application-frame denominator");

  rate::ReleaseNative();
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 3, 2);
  uint32_t next_application = 0;
  for (int64_t index = 0; index <= 600; ++index) {
    const int64_t ns = start + index * step;
    if (index < 210 || index > 340) {
      observe_application(next_application++, ns);
    }
    if ((index * step) % interval_ns == 0) {
      observe_state(7, 120, ns);
    }
  }
  snapshot = rate::Read(native_output, start + 600 * step);
  check(snapshot.available && snapshot.fps == 80 && snapshot.multiplier_variable
          && snapshot.observed_multiplier == 0,
      "fresh application endpoints cannot cover a missing middle segment of a variable-rate SDK interval");
  for (int64_t index = 601; index <= 1200; ++index) {
    const int64_t ns = start + index * step;
    observe_application(next_application++, ns);
    if ((index * step) % interval_ns == 0) {
      observe_state(7, 120, ns);
    }
  }
  snapshot = rate::Read(native_output, start + 1200 * step);
  check(snapshot.available && snapshot.multiplier_variable && std::abs(snapshot.observed_multiplier - 0.8) < 0.001,
      "complete subsequent intervals restore the observed variable-rate ratio");

  rate::ReleaseNative();
  rate::BindNativeOutput(native_output, 7);
  rate::ObserveNativeOptions(7, 5, 3, 2);
  next_application = 0;
  for (int64_t index = 0; index <= 600; ++index) {
    const int64_t ns = start + index * step;
    if (index >= 210 && index <= 340) {
      observe_application(next_application - 1, ns);
    } else {
      observe_application(next_application++, ns);
    }
    if ((index * step) % interval_ns == 0) {
      observe_state(7, 120, ns);
    }
  }
  snapshot = rate::Read(native_output, start + 600 * step);
  check(snapshot.available && snapshot.fps == 80 && snapshot.multiplier_variable
          && std::abs(snapshot.observed_multiplier - 480. / 469.) < 0.001,
      "known same-frame redeliveries keep the interval covered without adding denominator frames");

  const auto late_query = rate::BeginNativeState(7);
  rate::DestroyPresentOutput(9);
  rate::RegisterPresentOutput(9, 1, 900, native_back_buffers);
  const auto replacement = rate::MatchPresentOutput(9, 90);
  rate::BindNativeOutput(replacement, 7);
  rate::ObserveNativeState(late_query, true, 120, start + 1300 * step);
  check(replacement != native_output && rate::Read(replacement, start + 1300 * step).source == rate::Source::NONE,
      "an old SDK read cannot be relabeled with a replacement output generation");

  const uintptr_t side_back_buffers[] = {100, 101};
  rate::RegisterPresentOutput(10, 1, 1000, side_back_buffers);
  const auto native_side = rate::MatchPresentOutput(10, 100);
  constexpr uintptr_t main_feature = 701, side_feature = 801, replacement_feature = 702;
  rate::BindNativeOutput(native_side, 8);
  rate::ObserveNativeOptions(7, 5, 1, 2);
  rate::ObserveNativeOptions(8, 5, 3, 4);
  const int64_t feature_start = start + 1400 * step;
  const int64_t feature_end = feature_start + 200 * step;
  for (int64_t index = 0; index <= 200; ++index) {
    const int64_t ns = feature_start + index * step;
    rate::ObserveNativeEvaluation(replacement, 2, true, ns, main_feature);
    rate::ObserveNativeEvaluation(native_side, 0, true, ns, side_feature);
    observe_state(7, 2, ns);
    observe_state(8, 1, ns);
  }
  const auto main_before_release = rate::Read(replacement, feature_end);
  const auto side_before_release = rate::Read(native_side, feature_end);
  const auto main_binding = rate::NativeBindingGeneration(replacement);
  check(main_before_release.available && main_before_release.fps == 200
            && side_before_release.available && side_before_release.fps == 100
            && rate::NativeFeatureOutput(main_feature) == replacement
            && rate::NativeFeatureOutput(side_feature) == native_side,
        "different Native features and viewports have independent windows on the same device");
  rate::ReleaseNativeFeature(side_feature);
  const auto main_after_release = rate::Read(replacement, feature_end);
  check(!rate::NativeFeatureOutput(side_feature) && !rate::Read(native_side, feature_end).available
            && main_after_release.available && main_after_release.window_serial == main_before_release.window_serial
            && main_after_release.source_generation == main_before_release.source_generation
            && rate::NativeBindingGeneration(replacement) == main_binding && rate::NativeMode(replacement) == 1,
        "releasing a Native feature clears only its output and preserves the other viewport's window");

  const auto old_feature_query = rate::BeginNativeState(7);
  const int64_t feature_change = feature_end + step;
  rate::ObserveNativeEvaluation(replacement, 2, true, feature_change, replacement_feature);
  const auto replacement_binding = rate::NativeBindingGeneration(replacement);
  rate::ReleaseNativeFeature(main_feature);
  rate::ObserveNativeState(old_feature_query, true, 200, feature_change + 1);
  check(!rate::NativeFeatureOutput(main_feature) && rate::NativeFeatureOutput(replacement_feature) == replacement
            && replacement_binding != main_binding && rate::NativeBindingGeneration(replacement) == replacement_binding
            && rate::NativeMode(replacement) == 1 && !rate::Read(replacement, feature_change + 1).available,
        "changing only the Native feature on a viewport withdraws the old window and rejects its late receipt");
  const int64_t new_feature_start = feature_change + step;
  for (int64_t index = 0; index <= 200; ++index) {
    const int64_t ns = new_feature_start + index * step;
    rate::ObserveNativeEvaluation(replacement, 2, true, ns, replacement_feature);
    observe_state(7, 2, ns);
  }
  const auto new_feature_sample = rate::Read(replacement, new_feature_start + 200 * step);
  check(new_feature_sample.available && new_feature_sample.fps == main_before_release.fps
            && new_feature_sample.source_generation > main_before_release.source_generation
            && new_feature_sample.window_begin_ns == new_feature_start,
        "the new Native feature warms its own window even when viewport, FPS and multiplier agree");
  rate::DestroyPresentOutput(10);
  rate::DestroyPresentOutput(9);
  rate::ReleaseNative();
}
