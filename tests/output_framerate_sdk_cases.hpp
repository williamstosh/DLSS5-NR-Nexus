/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <chrono>
#include <cmath>
#include <thread>

#include "../adaptive_resolution.hpp"

namespace output_framerate_sdk_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
namespace rate = renodx::addons::dlss5::output_framerate;
namespace native_adapter = fg::native_options::internal;

struct ReflexFrameToken final : sl::FrameToken {
  uint32_t index = 0;
  operator uint32_t() const override { return index; }
};

inline void Run() {
  auto& now_ns = native_adapter::test_now_ns;
  now_ns = 10'000'000'000;
  constexpr uintptr_t owner = 0xAF001;
  ComPtr<ReceiptDeviceIdentity> buffer_face, source_face, device;
  buffer_face.Attach(new ReceiptDeviceIdentity());
  source_face.Attach(new ReceiptDeviceIdentity());
  device.Attach(new ReceiptDeviceIdentity());
  buffer_face->native_identity.Attach(new ReceiptDeviceIdentity());
  source_face->native_identity.Attach(new ReceiptDeviceIdentity());
  const auto buffer = reinterpret_cast<uintptr_t>(buffer_face->native_identity.Get());
  const auto source = reinterpret_cast<uintptr_t>(source_face->native_identity.Get());
  const uintptr_t buffers[] = {buffer};
  ComPtr<ReceiptSwapchain> proxy;
  proxy.Attach(new ReceiptSwapchain(nullptr));
  const auto native_identity = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(proxy.Get()));
  rate::RegisterPresentOutput(owner, reinterpret_cast<uintptr_t>(device.Get()), native_identity, buffers);
  const auto output = rate::MatchPresentOutput(owner, buffer);
  sl::Resource back_buffer(sl::ResourceType::eTex2d, buffer_face.Get());
  sl::Resource source_texture(sl::ResourceType::eTex2d, source_face.Get());
  const sl::ResourceTag tags[] = {
      {&back_buffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
      {&source_texture, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
  };
  constexpr uint64_t resource_generation = 7;
  const rate::SourceIdentity framed_source{.handle = 11, .generation = 2,
      .device = reinterpret_cast<uintptr_t>(device.Get()), .resource = source, .resource_generation = resource_generation};
  auto reused_source = framed_source;
  ++reused_source.resource_generation;
  fg::native_options::ObserveBackBufferTags(7, tags, 2, 12, resource_generation);
  Check(output && rate::BindNativeTaggedSourceOutput(7, 12, framed_source)
            && rate::FindSourceOutput(framed_source) == output
            && !rate::BindNativeTaggedSourceOutput(7, 12, reused_source)
            && !rate::BindNativeTaggedSourceOutput(7, 11, framed_source),
        "successful framed Streamline tags resolve the actual viewport and canonical SR output resource");

  fg::native_options::ObserveBackBufferTags(7, tags + 1, 1, std::nullopt, resource_generation);
  Check(!rate::CaptureNativeLegacySourceTag(7), "a ScalingOutputColor tag alone supplies no output owner");
  fg::native_options::ObserveBackBufferTags(7, tags, 1);
  const auto legacy_tag = rate::CaptureNativeLegacySourceTag(7);
  const rate::SourceIdentity legacy_source{.handle = 1, .generation = 2,
      .device = reinterpret_cast<uintptr_t>(device.Get()), .resource = source,
      .resource_generation = resource_generation};
  Check(legacy_tag && rate::MatchNativeLegacySourceTag(*legacy_tag, source, resource_generation) == output
            && !rate::MatchNativeLegacySourceTag(*legacy_tag, source, resource_generation + 1)
            && rate::BindNativeLegacySourceOutput(legacy_source, *legacy_tag)
            && rate::FindSourceOutput(legacy_source) == output,
        "separately issued legacy tags prove the live SR resource generation");
  const sl::ResourceTag unbind(nullptr, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate);
  fg::native_options::ObserveBackBufferTags(7, &unbind, 1);
  Check(!rate::MatchNativeLegacySourceTag(*legacy_tag, source, resource_generation)
            && !rate::FindSourceOutput(legacy_source),
        "a legacy ScalingOutputColor unbind revokes its receipt and the proven SR relation");
  const sl::ResourceTag volatile_tags[] = {
      tags[0], {&source_texture, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow},
  };
  fg::native_options::ObserveBackBufferTags(7, volatile_tags, 2, std::nullopt, resource_generation);
  Check(!rate::CaptureNativeLegacySourceTag(7), "a volatile legacy SR tag supplies no later Evaluate proof");
  fg::native_options::ObserveBackBufferTags(7, tags, 2, std::nullopt, resource_generation);
  rate::ObservePresent(output, now_ns);
  const auto persistent_legacy_tag = rate::CaptureNativeLegacySourceTag(7);
  Check(persistent_legacy_tag && rate::BindNativeLegacySourceOutput(legacy_source, *persistent_legacy_tag),
        "legacy tag evidence survives Present and can prove the next Evaluate");

  native_adapter::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) { return sl::Result::eOk; };
  static uint32_t reported_frames;
  reported_frames = 120;
  native_adapter::state.get_state = +[](const sl::ViewportHandle&, sl::DLSSGState& result, const sl::DLSSGOptions*) {
    result.status = sl::DLSSGStatus::eOk;
    result.numFramesActuallyPresented = reported_frames;
    return sl::Result::eOk;
  };
  native_adapter::state.present = +[](IDXGISwapChain*, UINT, UINT, bool& skip) -> HRESULT {
    skip = false;
    return S_OK;
  };
  sl::DLSSGOptions options;
  options.mode = sl::DLSSGMode::eOn;
  const sl::ViewportHandle viewport(7);
  native_adapter::HookedSetOptions(viewport, options);
  const auto warm = [&](auto operation, std::chrono::milliseconds step) {
    const auto begin = std::chrono::steady_clock::now();
    for (auto index = 0; index <= 2400 / step.count(); ++index) {
      if (now_ns != 0) {
        now_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(step).count();
      } else {
        std::this_thread::sleep_until(begin + index * step);
      }
      operation();
    }
  };
  const auto sdk_marker = reinterpret_cast<fg::native_options::frame_source::SetMarker>(
      native_adapter::REFLEX_MARKER_HOOK[0].detour);
  const auto observe_marker = [&](uint32_t index) {
    ReflexFrameToken token;
    token.index = index;
    Check(sdk_marker(sl::PCLMarker::ePresentStart, token) == sl::Result::eOk,
          "the registered Reflex entry forwards a successful PresentStart");
  };
  native_adapter::reflex_marker.real = +[](sl::PCLMarker, const sl::FrameToken&) { return sl::Result::eOk; };
  uint32_t marker_index = 1'000'000;
  warm([&] {
    bool skip = true;
    rate::ObserveNativeEvaluation(output, 2, true, now_ns);
    observe_marker(++marker_index);
    Check(native_adapter::HookedPresent(proxy.Get(), 0, 0, skip) == S_OK && !skip,
          "outer Native SDK forwarding still returns the original routing decision");
  }, std::chrono::milliseconds(24));
  auto sample = rate::Read(output, now_ns);
  Check(!sample.available,
        "successful outer Native SDK wrapper calls with only Reflex diagnostics cannot supply the fixed estimate");
  std::cout << "NR_RELEASE_PARITY native-application-frame-count marker-only fixed-fps=unavailable\n";

  proxy->current_back_buffer = buffer_face.Get();
  uint32_t application_frame = 30;
  const auto send_tagged_present = [&](uint32_t frame) {
    // Tag only the current Backbuffer. The earlier paired SR tag is a
    // separate proof and must not be needed for application-frame counts.
    fg::native_options::ObserveBackBufferTags(7, tags, 1, frame);
    observe_marker(++marker_index);
    bool skip = true;
    Check(native_adapter::HookedPresent(proxy.Get(), 0, 0, skip) == S_OK && !skip,
          "Native SDK forwards the tagged current backbuffer through its outer Present wrapper");
  };
  warm([&] {
    rate::ObserveNativeEvaluation(output, 2, true, now_ns);
    send_tagged_present(++application_frame);
    send_tagged_present(application_frame);
  }, std::chrono::milliseconds(25));
  sample = rate::Read(output, now_ns);
  Check(sample.available && sample.source == rate::Source::NATIVE_FIXED && std::abs(sample.fps - 80.) < 0.001,
        "Backbuffer FrameTokens produce a deduplicated fixed estimate despite a conflicting diagnostic token");
  fg::native_options::ObserveBackBufferTags(7, tags, 1, ++application_frame);
  send_tagged_present(++application_frame);
  Check(!rate::Read(output, now_ns).available,
        "multiple FrameTokens for the current backbuffer cut coverage without a marker substitute");
  warm([&] {
    rate::ObserveNativeEvaluation(output, 2, true, now_ns);
    send_tagged_present(++application_frame);
  }, std::chrono::milliseconds(25));
  Check(rate::Read(output, now_ns).source == rate::Source::NATIVE_FIXED
            && rate::Read(output, now_ns).available, "unique tags reestablish coverage after ambiguity");
  proxy->current_back_buffer = source_face.Get();
  send_tagged_present(++application_frame);
  Check(!rate::Read(output, now_ns).available,
        "a FrameToken for a different backbuffer cannot extend the fixed estimate");
  std::cout << "NR_RELEASE_PARITY native-application-frame-count fixed-estimate"
               " source=NATIVE_FIXED fps=80 denominator-hz=40 multiplier=2"
               " duplicate=deduplicated conflicting-marker=ignored ambiguous/wrong-bb=unavailable\n";
  proxy->current_back_buffer = buffer_face.Get();
  warm([&] {
    sl::DLSSGState state;
    rate::ObserveNativeEvaluation(output, 2, true, now_ns);
    Check(native_adapter::HookedGetState(viewport, state, nullptr) == sl::Result::eOk,
          "passive GetState returns the application result");
  }, std::chrono::milliseconds(550));
  sample = rate::Read(output, now_ns);
  Check(sample.available && sample.source == rate::Source::NATIVE_STATE && !sample.multiplier_variable
            && sample.observed_multiplier == 0,
        "passive Native GetState becomes the preferred source and fixed counts need no denominator");
  const auto native_serial = sample.window_serial;
  const auto native_generation = rate::NativeBindingGeneration(output);
  const sl::Extent subrect{.width = 16, .height = 16};
  sl::Resource implicit_bb(sl::ResourceType::eTex2d, nullptr);
  const sl::ResourceTag extent_tags[] = {
      {&implicit_bb, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &subrect},
      {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &subrect},
  };
  for (const auto& extent : extent_tags) {
    fg::native_options::ObserveBackBufferTags(7, tags, 1, ++application_frame);
    fg::native_options::ObserveBackBufferTags(7, &extent, 1, application_frame);
    Check(rate::Read(output, now_ns).window_serial == native_serial
              && rate::NativeBindingGeneration(output) == native_generation
              && !rate::TakeNativeApplicationFrame(output, native_generation, buffer, now_ns),
          "extent-only Backbuffer metadata preserves output and fixed SDK counts, with no application-frame proof");
  }
  Check(rate::CaptureNativeLegacySourceTag(7) && rate::BindNativeLegacySourceOutput(legacy_source, *persistent_legacy_tag),
        "global legacy evidence survives cadence staleness and later Evaluates until explicit replacement or release");
  options.numFramesToGenerate = 3;
  native_adapter::HookedSetOptions(sl::ViewportHandle(8), options);
  Check(rate::Read(output, now_ns).window_serial == native_serial,
        "another viewport's successful Native Options preserve the bound output");

  // Start a new viewport binding with no outer Present history. Only the SDK
  // responses supply FPS. Completed NR costs and delivery are MOCK inputs.
  const sl::ResourceTag remove_back_buffer(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent);
  fg::native_options::ObserveBackBufferTags(7, &remove_back_buffer, 1);
  Check(!rate::Read(output, now_ns).available && !rate::CaptureNativeLegacySourceTag(7)
            && !rate::FindSourceOutput(legacy_source),
        "explicit Backbuffer removal revokes the viewport association rather than acting as extent metadata");
  fg::native_options::ObserveBackBufferTags(7, tags, 2, 13, resource_generation);
  options.numFramesToGenerate = 1;
  Check(native_adapter::HookedSetOptions(viewport, options) == sl::Result::eOk
            && rate::BindNativeTaggedSourceOutput(7, 13, framed_source),
        "fixed SDK control starts with the live tagged output and accepted On options");
  constexpr adaptive::Settings target{.enabled = true, .target_fps = 100.f, .prepared_levels = 2};
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 1920, .height = 1080};
  floor[0] = {.width = 634, .height = 356};
  constexpr auto tick_interval = std::chrono::milliseconds(100);
  constexpr auto sdk_interval = std::chrono::milliseconds(1500);
  constexpr auto test_duration = std::chrono::milliseconds(22'500);
  constexpr double MOCK_UPPER_GPU_US = 3100.;
  constexpr double MOCK_LOWER_GPU_US = 2200.;
  const auto observe_mock_gpu = [&](adaptive::Session* session, uint64_t sequence, int64_t now_ns) {
    session->ObserveGpu({.generation = session->status.generation, .frame = sequence,
        .sequence = sequence, .context = 1, .recorded_ns = now_ns, .completed_ns = now_ns,
        .total_us = (session->active_level == 0 ? MOCK_UPPER_GPU_US : MOCK_LOWER_GPU_US),
        .valid = true, .stable = true, .sampling_generation = session->sampling_generation}, now_ns);
  };
  const auto prepare_from_sdk = [&](adaptive::Session* session, const rate::Snapshot& output_sample, int64_t now_ns) {
    session->Prepare(target, upper, floor, output_sample.output.generation,
        {.available = output_sample.available, .source = static_cast<uint64_t>(output_sample.source),
         .generation = output_sample.source_generation, .configuration = output_sample.configuration_generation,
         .serial = output_sample.window_serial, .begin_ns = output_sample.window_begin_ns,
         .end_ns = output_sample.window_end_ns, .fps = output_sample.fps, .mad = output_sample.fps_mad,
         .observed_multiplier = output_sample.observed_multiplier,
         .multiplier_variable = output_sample.multiplier_variable}, now_ns, true, false);
  };
  adaptive::Session controller;
  uint64_t low_generation = 0;
  int64_t last_read_ns = 0;
  bool accepted = false;
  for (int tick = 0; tick <= test_duration / tick_interval && !accepted; ++tick) {
    now_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(tick_interval).count();
    rate::ObserveNativeEvaluation(output, 2, true, now_ns);
    if (tick % (sdk_interval / tick_interval) == 0) {
      const bool full_low_interval = controller.active_level == 1 && controller.applied_ns <= last_read_ns;
      reported_frames = (full_low_interval ? 150u : 120u);
      sl::DLSSGState state;
      Check(native_adapter::HookedGetState(viewport, state, nullptr) == sl::Result::eOk,
            "fixed control receives each low-frequency count through the passive SDK wrapper");
      last_read_ns = now_ns;
    }
    sample = rate::Read(output, now_ns);
    observe_mock_gpu(&controller, static_cast<uint64_t>(tick + 1), now_ns);
    prepare_from_sdk(&controller, sample, now_ns);
    if (tick == 0) {
      controller.UpdatePreparation(controller.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, now_ns);
    }
    if (controller.trial && low_generation == 0) {
      Check(sample.available && sample.output == output && sample.source == rate::Source::NATIVE_STATE
                && !sample.multiplier_variable && sample.observed_multiplier == 0
                && std::abs(sample.fps - 80.) < 2. && controller.status.ready_levels == 2
                && controller.level == 1 && controller.active_level == 0 && controller.status.applied == upper
                && controller.baseline_gpu_us == MOCK_UPPER_GPU_US,
            "fixed 80 FPS SDK counts without outer Presents request a ready lower tier automatically");
      low_generation = controller.status.generation;
    }
    if (controller.status.generation != controller.applied_generation) {
      controller.Applied(controller.status.generation, controller.status.requested, now_ns);
    }
    accepted = low_generation != 0 && !controller.trial;
  }
  Check(accepted && controller.level == 1 && controller.active_level == 1
            && controller.applied_generation == low_generation && controller.status.reason == adaptive::Reason::OBSERVING
            && sample.available && sample.output == output && sample.source == rate::Source::NATIVE_STATE
            && !sample.multiplier_variable && sample.observed_multiplier == 0
            && std::abs(sample.fps - 100.) < 2. && sample.window_begin_ns >= controller.stable_after_ns
            && std::abs(controller.status.gpu_ms - MOCK_LOWER_GPU_US / 1000.) < 0.001,
        "fixed 100 FPS SDK counts and cheaper MOCK NR accept the delivered lower tier");
  std::cout << "PASS Native fixed low-frequency SDK counts request and accept the lower tier evidence=MOCK"
            << " baseline_fps=" << controller.baseline_fps << " accepted_fps=" << sample.fps << '\n';
  std::cout << "NR_RELEASE_PARITY native-application-frame-count fixed-state"
               " source=NATIVE_STATE intervals-ms=1500 denominator=unknown trial-fps=80 accepted-fps=100\n";

  rate::ReleaseNativeViewport(7);
  options.mode = sl::DLSSGMode::eAuto;
  fg::native_options::ObserveBackBufferTags(7, tags, 1);
  Check(native_adapter::HookedSetOptions(viewport, options) == sl::Result::eOk,
        "dynamic control starts on the same output under a new viewport binding");
  native_adapter::state.present1 = +[](IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool& skip)
      -> HRESULT {
    skip = false;
    return S_OK;
  };
  adaptive::Session dynamic;
  uint64_t sequence = 0;
  reported_frames = 120;
  const auto dynamic_interval = [&](uint32_t first_tag, uint32_t last_tag) {
    for (uint32_t frame = 0; frame < 60; ++frame) {
      now_ns += 25'000'000;
      observe_marker(++marker_index);
      if (frame >= first_tag && frame <= last_tag) {
        fg::native_options::ObserveBackBufferTags(7, tags, 1, ++application_frame);
      }
      bool skip = true;
      Check(native_adapter::HookedPresent1(proxy.Get(), 0, 0, nullptr, skip) == S_OK && !skip,
            "Native Present1 preserves the application's SDK result");
      if (frame == 59) {
        sl::DLSSGState state;
        Check(native_adapter::HookedGetState(viewport, state, nullptr) == sl::Result::eOk,
              "Native GetState returns the SDK count across the current coverage interval");
      }
      sample = rate::Read(output, now_ns);
      observe_mock_gpu(&dynamic, ++sequence, now_ns);
      prepare_from_sdk(&dynamic, sample, now_ns);
      dynamic.UpdatePreparation(dynamic.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, now_ns);
      if (dynamic.status.generation != dynamic.applied_generation) {
        dynamic.Applied(dynamic.status.generation, dynamic.status.requested, now_ns);
      }
    }
  };
  for (int interval = 0; interval < 9; ++interval) {
    dynamic_interval(60, 0);
  }
  Check(sample.available && sample.source == rate::Source::NATIVE_STATE && sample.multiplier_variable
            && sample.observed_multiplier == 0 && std::abs(sample.fps - 80.) < 0.001
            && !dynamic.trial && dynamic.active_level == 0 && dynamic.status.reason == adaptive::Reason::COLLECTING,
        "dynamic SDK output stays at 80 FPS but diagnostic-only Presents cannot drive a trial");
  for (int interval = 0; interval < 9 && !dynamic.trial; ++interval) {
    dynamic_interval(0, 59);
  }
  Check(sample.available && sample.source == rate::Source::NATIVE_STATE && sample.multiplier_variable
            && std::abs(sample.observed_multiplier - 2.) < 0.001 && dynamic.trial && dynamic.active_level == 1,
        "full current-backbuffer token coverage restores dynamic sampling and requests the lower tier");
  for (int frame = 0; frame < 60; ++frame) {
    now_ns += 25'000'000;
    if (frame == 0 || frame == 59) {
      send_tagged_present(++application_frame);
    } else {
      observe_marker(++marker_index);
      bool skip = true;
      Check(native_adapter::HookedPresent1(proxy.Get(), 0, 0, nullptr, skip) == S_OK && !skip,
            "unidentified SDK calls continue through the middle of the sample");
    }
  }
  sl::DLSSGState state;
  Check(native_adapter::HookedGetState(viewport, state, nullptr) == sl::Result::eOk,
        "SDK output count survives the middle coverage gap");
  sample = rate::Read(output, now_ns);
  prepare_from_sdk(&dynamic, sample, now_ns);
  Check(sample.available && sample.source == rate::Source::NATIVE_STATE && sample.observed_multiplier == 0
            && std::abs(sample.fps - 80.) < 0.001 && !dynamic.trial && dynamic.level == 0
            && dynamic.status.requested == upper,
        "fresh endpoint tags cannot bridge a middle gap or keep a dynamic trial alive");
  for (int interval = 0; interval < 9 && !dynamic.trial; ++interval) {
    dynamic_interval(0, 59);
  }
  Check(sample.available && std::abs(sample.observed_multiplier - 2.) < 0.001 && dynamic.trial,
        "a continuous fresh token window can drive a new trial after the gap");
  const auto old_binding = rate::NativeBindingGeneration(output);
  static const sl::ResourceTag* replacement_binding_tag = nullptr;
  replacement_binding_tag = tags;
  native_adapter::state.present = +[](IDXGISwapChain*, UINT, UINT, bool& skip) -> HRESULT {
    rate::ReleaseNativeViewport(7);
    fg::native_options::ObserveBackBufferTags(9, replacement_binding_tag, 1);
    skip = false;
    return S_OK;
  };
  fg::native_options::ObserveBackBufferTags(7, tags, 1, ++application_frame);
  bool skip = true;
  Check(native_adapter::HookedPresent(proxy.Get(), 0, 0, skip) == S_OK && !skip
            && rate::NativeBindingGeneration(output) != old_binding,
        "the real SDK return crosses a newly bound viewport on the same output");
  {
    const std::shared_lock lock(rate::internal::state.mutex);
    Check(rate::internal::FindOutput(output)->native.applications == 0,
          "the old invocation cannot contribute an application frame to the new binding");
  }
  replacement_binding_tag = nullptr;
  std::cout << "PASS Native dynamic SDK coverage and recovery evidence=MOCK\n";
  std::cout << "NR_RELEASE_PARITY native-application-frame-count dynamic-state"
               " source=NATIVE_STATE fps=80 marker-only=collecting full-tags=trial"
               " middle-gap=rollback full-window=recovered old-binding=discarded\n";

  rate::DestroyPresentOutput(owner);
  rate::ReleaseNative();
  native_adapter::state.real = nullptr;
  native_adapter::state.get_state = nullptr;
  native_adapter::state.present = nullptr;
  native_adapter::state.present1 = nullptr;
  now_ns = 0;
  native_adapter::reflex_marker.real = nullptr;
  proxy->current_back_buffer.Reset();

  const auto original_sdk = adapter::modules[0].real;
  const auto original_proxy = adapter::proxies[0].real;
  auto context = std::make_shared<adapter::Context>();
  context->device_identity.Attach(new ReceiptDeviceIdentity());
  context->proxy_identity = adapter::Identity(proxy.Get());
  static uint32_t xe_reported_frames;
  xe_reported_frames = 2;
  context->get_status = +[](void*, xefg::abi::PresentStatus* status) {
    *status = {.frames_presented = xe_reported_frames, .enabled = 1};
    return 0;
  };
  context->init.max_interpolated = 3;
  void* handle = context.get();
  adapter::state.contexts.emplace(handle, context);
  adapter::modules[0].real[5] = reinterpret_cast<void*>(+[](void*, uint32_t) { return 0; });
  adapter::modules[0].real[9] = reinterpret_cast<void*>(+[](void*, uint32_t) { return 0; });
  adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
  uint32_t id = 0;
  warm([&] {
    Check(Hooks::PresentId(handle, ++id) == 0 && adapter::ProxyHooks<0>::Present(proxy.Get(), 0, 0) == S_OK,
          "XeFG accepts a new application frame and completes its proxy Present");
    Check(Hooks::PresentId(handle, id) == 0 && adapter::ProxyHooks<0>::Present(proxy.Get(), 0, 0) == S_OK,
          "XeFG accepts a distinct outer Present of the same application frame");
  }, std::chrono::milliseconds(40));
  const auto xe_output = context->output_identity;
  sample = rate::Read(xe_output, rate::NowNs());
  Check(sample.available && sample.source == rate::Source::XEFG_STATUS
            && sample.observed_multiplier > 3.9 && sample.observed_multiplier < 4.1,
        "distinct outer XeFG calls of the same Present ID count only the extra output frames");
  Check(Hooks::NumInterpolated(handle, 1) == 0, "XeFG setter passes through its SDK result");
  const auto after_setter = rate::Read(xe_output, rate::NowNs());
  Check(after_setter.available && after_setter.source_generation == sample.source_generation
            && after_setter.window_serial == sample.window_serial && after_setter.requested_multiplier == 2,
        "XeFG setter metadata preserves the measured window from the real proxy");
  Check(adapter::ProxyHooks<0>::Present(proxy.Get(), 0, DXGI_PRESENT_TEST) == S_OK
            && rate::Read(xe_output, rate::NowNs()).window_serial == after_setter.window_serial,
        "a serialized XeFG TEST call does not consume application cadence or SDK output");
  auto late = adapter::BeginPresent(proxy.Get(), 0);
  adapter::BeforeResize(proxy.Get());
  adapter::EndPresent(&late, S_OK);
  Check(!rate::IsCurrentOutput(xe_output) && !context->output_identity,
        "XeFG never publishes an old invocation against the resized output");

  // Poll the controller at a fixed cadence long enough to compare complete
  // windows. The application's 60 Hz schedule continues through both tiers.
  constexpr auto application_interval = std::chrono::nanoseconds(16'666'667);
  constexpr int DECISION_FRAMES = 180;
  constexpr int SETTER_FRAME = 2 * DECISION_FRAMES;
  constexpr int COUNTS_CHANGE_FRAME = 3 * DECISION_FRAMES;
  adaptive::Session xe_controller;
  const auto xe_begin = std::chrono::steady_clock::now();
  uint64_t xe_low_generation = 0;
  double upper_fps = 0.;
  bool returned_to_upper = false;
  Check(Hooks::NumInterpolated(handle, 1) == 0, "XeFG fixed-cadence control starts with a successful 2x request");
  for (int frame = 0; frame <= 7 * DECISION_FRAMES && !returned_to_upper; ++frame) {
    std::this_thread::sleep_until(xe_begin + frame * application_interval);
    Check(Hooks::PresentId(handle, ++id) == 0 && adapter::ProxyHooks<0>::Present(proxy.Get(), 0, 0) == S_OK,
          "the XeFG proxy completes one application Present at 60 Hz with its own SDK count");
    const auto now_ns = rate::NowNs();
    observe_mock_gpu(&xe_controller, static_cast<uint64_t>(frame + 1), now_ns);
    if (frame % DECISION_FRAMES != 0) continue;
    sample = rate::Read(context->output_identity, now_ns);
    prepare_from_sdk(&xe_controller, sample, now_ns);
    if (frame == 0) {
      xe_controller.UpdatePreparation(xe_controller.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, now_ns);
    }
    if (frame == SETTER_FRAME || frame == COUNTS_CHANGE_FRAME) {
      const auto expected_request = (frame == SETTER_FRAME ? 2u : 1u);
      Check(sample.available && sample.source == rate::Source::XEFG_STATUS
                && std::abs(sample.fps - 120.) < 4. && std::abs(sample.observed_multiplier - 2.) < 0.01
                && sample.requested_multiplier == expected_request && !xe_controller.trial
                && xe_controller.level == 0 && xe_controller.status.applied == upper
                && xe_controller.status.reason == adaptive::Reason::OBSERVING,
            "XeFG holds the confirmed upper tier at 120 FPS while only the successful request changes");
      if (frame == SETTER_FRAME) {
        upper_fps = sample.fps;
        Check(Hooks::NumInterpolated(handle, 0) == 0, "XeFG passes a 1x request through the real setter wrapper");
        const auto request_only = rate::Read(context->output_identity, rate::NowNs());
        Check(request_only.available && request_only.source_generation == sample.source_generation
                  && request_only.window_serial == sample.window_serial && request_only.requested_multiplier == 1,
              "the real XeFG setter leaves the 120 FPS window intact before the SDK count changes");
      } else {
        xe_reported_frames = 1;
      }
    }
    if (xe_controller.trial && xe_low_generation == 0) {
      Check(frame > COUNTS_CHANGE_FRAME && sample.available && sample.source == rate::Source::XEFG_STATUS
                && std::abs(sample.fps - 60.) < 2. && std::abs(sample.observed_multiplier - 1.) < 0.01
                && xe_controller.level == 1 && xe_controller.active_level == 0
                && xe_controller.status.applied == upper && xe_controller.status.ready_levels == 2
                && xe_controller.baseline_gpu_us == MOCK_UPPER_GPU_US,
            "only the changed XeFG SDK count requests the ready lower tier automatically");
      xe_low_generation = xe_controller.status.generation;
    }
    if (xe_low_generation != 0 && !xe_controller.trial) {
      Check(sample.available && sample.source == rate::Source::XEFG_STATUS
                && sample.window_begin_ns >= xe_controller.stable_after_ns
                && std::abs(sample.fps - 60.) < 2. && std::abs(sample.observed_multiplier - 1.) < 0.01
                && std::abs(xe_controller.status.gpu_ms - MOCK_LOWER_GPU_US / 1000.) < 0.001
                && xe_controller.status.reason == adaptive::Reason::NO_BENEFIT && xe_controller.level == 0
                && xe_controller.active_level == 1 && xe_controller.applied_generation == xe_low_generation
                && xe_controller.status.requested == upper,
            "full 60 FPS windows at both tiers return to the upper tier despite cheaper MOCK NR");
      returned_to_upper = true;
    }
    if (xe_controller.status.generation != xe_controller.applied_generation) {
      xe_controller.Applied(xe_controller.status.generation, xe_controller.status.requested, now_ns);
    }
  }
  Check(returned_to_upper && xe_controller.active_level == 0 && xe_controller.status.applied == upper,
        "the XeFG controller applied the automatically restored upper tier");
  std::cout << "PASS XeFG SDK counts drive trial and rollback at fixed application cadence evidence=MOCK"
            << " upper_fps=" << upper_fps << " trial_fps=" << xe_controller.baseline_fps
            << " rollback_fps=" << sample.fps << '\n';
  rate::ReleaseXeFGOutput(context->output_identity);
  adapter::state.contexts.erase(handle);
  adapter::modules[0].real = original_sdk;
  adapter::proxies[0].real = original_proxy;
  std::cout << "PASS output SDK callback ownership and count provenance evidence=MOCK\n";
}
}  // namespace output_framerate_sdk_cases
