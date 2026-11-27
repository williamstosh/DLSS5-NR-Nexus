/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(NR_NEXUS_TEST_INTEL_SDK)
#include "original_intel_sdk_cases.hpp"
#endif

namespace bridge_xefg_warm_cases {

template <typename Submit, typename Wait>
void Run(Gpu& gpu, ID3D12Resource* depth12, ID3D12Resource* motion12,
         const Submit& submit, const Wait& wait, bool fg_first = false) {
  namespace addon = renodx::addons::dlss5;
  Check(native::direct_device == nullptr && native::nr_features_created == 0
            && !native::bridge_device_live.load() && !xefg::Enabled(),
        "fresh fixture has no ordinary NR or Bridge before either real entry");
  if (fg_first) {
    Check(native::NgxLoaderView().nr_module == nullptr, "FG-first has no prior Nexus loader preparation");
  }
  constexpr auto present_stage = static_cast<size_t>(addon::stage_plan::Stage::PRESENT);
  struct CallCounts {
    uint64_t init_requests;
    uint64_t init_successes;
    uint64_t creates;
    uint64_t evaluates;
    uint64_t accepted;
  };
  const auto counts = [] {
    const auto& stage = native::stage_telemetry[present_stage];
    return CallCounts{
        .init_requests = static_cast<uint64_t>(std::ranges::count_if(captured_logs, [](const auto& entry) {
          return entry.second.find("NR Init starting:") != std::string::npos;
        })),
        .init_successes = static_cast<uint64_t>(std::ranges::count_if(captured_logs, [](const auto& entry) {
          return entry.second.find("NR Init finished:") != std::string::npos && entry.second.ends_with("result=0x1");
        })),
        .creates = native::nr_features_created,
        .evaluates = stage.evaluates,
        .accepted = stage.accepted};
  };
  const auto cold_calls = counts();
  Check(cold_calls.init_requests == 0 && cold_calls.creates == 0 && cold_calls.evaluates == 0,
        "nothing in test setup initialized or evaluated NR before the selected first route");

  const UINT width = gpu.swapchain_width;
  const UINT height = gpu.swapchain_height;
  constexpr UINT pixel_bytes = 4;
  constexpr size_t passes = 2;
  const auto frame_bytes = static_cast<size_t>(width) * height * pixel_bytes;
  ComPtr<IDXGIAdapter> dxgi_adapter;
  Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&dxgi_adapter))),
        "Bridge selects the native fixture adapter");
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context11;
  Check(SUCCEEDED(D3D11CreateDevice(dxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device11, nullptr, &context11)),
        "real D3D11 device and immediate context for the ordinary Bridge");
  const auto release_bridge = [](ID3D11DeviceContext*) {
    if (native::DrainBridgeQueue()) {
      native::ReleaseBridge();
    }
  };
  const std::unique_ptr<ID3D11DeviceContext, decltype(release_bridge)> bridge_scope(context11.Get(), release_bridge);

  ComPtr<ID3D12Device> bridge_device;
  const auto prepare_bridge = [&] {
    // Same setup as the normal D3D11 Present caller. Library preparation
    // alone cannot create a feature or select an active recording face.
    const auto before = counts();
    native::PrimeNgxLoaderSymbols(nullptr);
    Check(native::EnsureBridgeUp(context11.Get()) == native::kBridgeReady
              && native::bridge.queue != gpu.queue.Get(),
          "production Bridge creates its own D3D12 lists and independent queue");
    Check(SUCCEEDED(native::bridge.ring[0].list->GetDevice(IID_PPV_ARGS(&bridge_device)))
              && bridge_device.Get() == native::bridge.device12 && bridge_device.Get() == gpu.device.Get(),
          "real Bridge recording and the native SDK queue use the same exact device");
    const auto after = counts();
    Check(after.init_requests == before.init_requests && after.creates == before.creates
              && after.evaluates == before.evaluates,
          "ordinary loader and Bridge creation do not proactively enter a vendor NR API");
  };
  HMODULE original_module = nullptr;
  uint64_t original_generation = 0;
  const auto check_active_original = [&] {
    const auto& loader = native::NgxLoaderView();
    Check(loader.nr_ready && loader.nr_module != nullptr && loader.nr_image_generation != 0
              && native::direct_api.module == loader.nr_module && native::direct_device == gpu.device.Get()
              && native::direct_api.create == loader.create && native::direct_api.evaluate == loader.evaluate
              && native::direct_api.release == loader.release,
          "actual successful calls use the selected original signed family and native recording interface");
    if (original_module == nullptr) {
      original_module = loader.nr_module;
      original_generation = loader.nr_image_generation;
    }
    Check(loader.nr_module == original_module && loader.nr_image_generation == original_generation,
          "the original image and its resolved generation remain unchanged when the route changes");
  };

  const auto make_texture11 = [&](DXGI_FORMAT format, D3D11_USAGE usage, const void* data = nullptr) {
    const D3D11_TEXTURE2D_DESC desc{.Width = width, .Height = height, .MipLevels = 1, .ArraySize = 1, .Format = format, .SampleDesc = {1, 0}, .Usage = usage, .BindFlags = (usage == D3D11_USAGE_DEFAULT ? UINT{D3D11_BIND_SHADER_RESOURCE} : 0u), .CPUAccessFlags = (usage == D3D11_USAGE_STAGING ? UINT{D3D11_CPU_ACCESS_READ} : 0u)};
    const D3D11_SUBRESOURCE_DATA initial{.pSysMem = data, .SysMemPitch = width * pixel_bytes};
    ComPtr<ID3D11Texture2D> texture;
    Check(SUCCEEDED(device11->CreateTexture2D(&desc, (data != nullptr ? &initial : nullptr), &texture)),
          "Bridge color, guides and staging textures");
    return texture;
  };
  std::vector<uint32_t> bridge_seed(static_cast<size_t>(width) * height);
  auto color11 = make_texture11(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT);
  auto motion11 = make_texture11(DXGI_FORMAT_R16G16_FLOAT, D3D11_USAGE_DEFAULT, bridge_seed.data());
  auto depth11 = make_texture11(DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, bridge_seed.data());
  auto staging11 = make_texture11(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_STAGING);
  const native::FeatureState contract{.input_width = width, .input_height = height};
  native::PresentStream bridge_stream;
  native::PresentTarget bridge_target{
      .workset_key = reinterpret_cast<const ID3D12Resource*>(&bridge_stream),
      .swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM};
  native::SnapshotPresentRestoreConfiguration(&bridge_target);
  bridge_target.diagnostic.bypass_host_look = true;
  bridge_target.pass_transfer_strength[0] = 1.f;
  bridge_target.pass_color_strength[0] = 1.f;
  const auto* bridge_handle = bridge_stream.Handle();

  const auto features_for = [](const NVSDK_NGX_Handle* source) {
    std::array<const NVSDK_NGX_Handle*, passes> models{};
    if (const auto feature = native::features.find(source); feature != native::features.end()) {
      for (size_t pass = 0; pass < passes; ++pass) {
        const auto& slot = feature->second.slots[pass];
        const auto& loader = native::NgxLoaderView();
        Check(slot.handle == nullptr || (!slot.via_core && slot.parameters != nullptr && slot.destroy_parameters != nullptr && slot.evaluate_feature == loader.evaluate && slot.release_feature == loader.release),
              "actual feature slots keep the selected signed Create/Evaluate/Release and parameter family");
        models[pass] = slot.handle;
      }
    }
    return models;
  };
  const auto addresses = [](const auto& handles) {
    std::array<uintptr_t, passes> values{};
    for (size_t pass = 0; pass < passes; ++pass) {
      values[pass] = reinterpret_cast<uintptr_t>(handles[pass]);
    }
    return values;
  };
  const auto check_exact_first_submit = [&](ID3D12GraphicsCommandList* list, const auto& handles,
                                            submission::RecordingOwner owner) {
    ComPtr<ID3D12Device> actual_device;
    Check(list != nullptr && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&actual_device)))
              && actual_device.Get() == gpu.device.Get(),
          "successful NR uses a real owned work list with the exact native device interface");
    const auto recording = submission::RecordingIdentity(list);
    const std::lock_guard lock(submission::tracker_mutex);
    const auto& generation = submission::tracked_lists.at(recording.first).generations.at(recording.second);
    Check(generation.owner == owner && generation.first_execute != 0
              && generation.first_submission_index < generation.submissions.size(),
          "this route's exact list generation has a tracked first Execute");
    Check(handles[0] != nullptr && handles[1] != nullptr && handles[0] != handles[1]
              && std::ranges::all_of(handles, [&](auto* handle) {
                   return std::ranges::find(generation.uses, handle) != generation.uses.end();
                 }),
          "the exact first Execute carries both distinct signed feature handles");
    const auto proof = submission::ReadSubmission(generation.submissions[generation.first_submission_index]);
    Check((proof.state == submission::SubmissionProofState::kInFlight
           || proof.state == submission::SubmissionProofState::kComplete)
              && proof.fence.fence != nullptr && proof.value != 0,
          "the first Execute has a healthy tracking Signal, not a later replay");
    return nlohmann::json{
        {"list_pointer", reinterpret_cast<uintptr_t>(list)}, {"recording_face", reinterpret_cast<uintptr_t>(actual_device.Get())}, {"list", recording.first}, {"generation", recording.second}, {"execute", generation.first_execute}, {"state", static_cast<uint32_t>(proof.state)}, {"fence", reinterpret_cast<uintptr_t>(proof.fence.fence)}, {"value", proof.value}, {"completed_at_observation", proof.completed_value}, {"replay_closed", !generation.open}};
  };
  nlohmann::json ordinary_cold = nlohmann::json::array();
  nlohmann::json xefg_cold = nlohmann::json::array();
  constexpr uint32_t full_mask = (1u << passes) - 1;
  const auto run_bridge = [&](uint8_t marker, nlohmann::json* frame) {
    const uint32_t seed = (static_cast<uint32_t>(marker) << 24) | 0x00203068u;
    for (UINT y = 0; y < height; ++y) {
      for (UINT x = 0; x < width; ++x) {
        const bool tile = ((x / 40 + y / 30) % 2) != 0;
        bridge_seed[static_cast<size_t>(y) * width + x] = seed + (tile ? 0x00241915u : 0u);
      }
    }
    context11->UpdateSubresource(color11.Get(), 0, nullptr, bridge_seed.data(), width * pixel_bytes, 0);
    auto& parameters = native::FillPresentBlock(&bridge_stream, color11.Get(), motion11.Get(), depth11.Get(),
                                                contract, width, height, 1.f, 0);
    const auto ready_before = LiveWarmFixturePassMask(bridge_handle);
    const auto calls_before = counts();
    const auto fence_before = native::bridge_submitted_value.load();
    bool delivered = false;
    {
      const addon::EvaluateChainScope chain;
      delivered = native::RunBridge(context11.Get(), bridge_handle, &parameters, nullptr, &bridge_target);
    }
    const auto calls_after = counts();
    const auto evaluates = calls_after.evaluates - calls_before.evaluates;
    const auto accepted = calls_after.accepted - calls_before.accepted;
    Check(accepted == evaluates, "all attempted ordinary NR evaluations returned success through the real API");
    if (!delivered || evaluates != passes) {
      const auto creates = calls_after.creates - calls_before.creates;
      Check(ready_before != full_mask && creates > 0 && evaluates < passes,
            "only an actual incomplete model creation can precede the first double-pass Bridge delivery");
      ordinary_cold.push_back({{"marker", marker}, {"ready_before", ready_before}, {"ready_after", LiveWarmFixturePassMask(bridge_handle)}, {"creates", creates}, {"evaluates", evaluates}, {"delivered", delivered}});
      return false;
    }
    const auto models = features_for(bridge_handle);
    const auto fence_value = native::bridge_submitted_value.load();
    const auto completed = native::bridge.fence_done12->GetCompletedValue();
    Check(fence_value > fence_before && completed != UINT64_MAX && completed >= fence_value,
          "ordinary Bridge submitted and completed this double-pass frame");
    const auto slot = std::ranges::find(native::bridge.ring, fence_value, &native::BridgeSlot::value);
    Check(slot != std::ranges::end(native::bridge.ring), "find the actual Bridge submission by its native Fence value");
    const auto first = check_exact_first_submit(slot->list, models, submission::RecordingOwner::kBridge);
    context11->CopyResource(staging11.Get(), color11.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context11->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
          "the first downstream D3D11 consumer takes the returned current color");
    std::array<uint8_t, pixel_bytes> rgba{};
    std::memcpy(rgba.data(), mapped.pData, rgba.size());
    size_t changed = 0;
    bool same_frame = true;
    for (UINT y = 0; y < height; ++y) {
      const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
      const auto* source = reinterpret_cast<const uint8_t*>(bridge_seed.data()) + static_cast<size_t>(y) * width * pixel_bytes;
      for (UINT x = 0; x < width; ++x) {
        const auto offset = static_cast<size_t>(x) * pixel_bytes;
        changed += std::memcmp(row + offset, source + offset, 3) != 0;
        same_frame &= row[offset + 3] == marker;
      }
    }
    context11->Unmap(staging11.Get(), 0);
    Check(changed > 0 && same_frame, "ordinary edited RGB preserves all of this frame's unique alpha");
    *frame = {{"role", "D3D11-returned-color"}, {"subresource", 0}, {"resource", reinterpret_cast<uintptr_t>(color11.Get())}, {"models", addresses(models)}, {"evaluates", passes}, {"marker", marker}, {"rgba", rgba}, {"list", reinterpret_cast<uintptr_t>(slot->list)}, {"first_execute", first}, {"queue", reinterpret_cast<uintptr_t>(native::bridge.queue)}, {"fence", reinterpret_cast<uintptr_t>(native::bridge.fence_done12)}, {"fence_value", fence_value}, {"fence_completed", completed}, {"edited_rgb_pixels", changed}};
    return true;
  };

  nlohmann::json first_bridge;
  decltype(features_for(bridge_handle)) bridge_models{};
  if (!fg_first) {
    prepare_bridge();
    bool bridge_ready = false;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !bridge_ready; ++attempt) {
      bridge_ready = run_bridge(static_cast<uint8_t>(64 + attempt), &first_bridge);
    }
    const auto ordinary_started = counts();
    Check(bridge_ready && ordinary_started.creates == cold_calls.creates + passes
              && ordinary_started.init_requests == cold_calls.init_requests + 1
              && ordinary_started.init_successes == cold_calls.init_successes + 1,
          "ordinary cold RunBridge enters one real Init and produces both genuine signed features with no NGX source callback");
    check_active_original();
    bridge_models = features_for(bridge_handle);
  }

  const auto before_session = counts();
#if defined(NR_NEXUS_TEST_INTEL_SDK)
  const char* sdk_receipt = (fg_first ? "intel-bridge-fg-first.json" : "intel-bridge-warm.json");
  original_intel_sdk_cases::Consumer intel(gpu.queue.Get(), width, height, sdk_receipt);
#endif
  Session session(&gpu);
  session.extent_left = session.extent_top = 0;
  session.extent_width = width;
  session.extent_height = height;
  session.context->ui_composition = false;
  ComPtr<ID3D12Device> session_face;
  Check(session.context->queue.Get() == gpu.queue.Get()
            && SUCCEEDED(session.context->queue->GetDevice(IID_PPV_ARGS(&session_face)))
            && session_face.Get() == gpu.device.Get(),
        "use the adapter context's actual native queue and its exact GetDevice, not an injected face");
  if (!fg_first) {
    Check(Hooks::Enable(session.handle, 0) == 0 && !xefg::Enabled(), "start from ordinary SDK-Off");
  }
  Check(counts().init_requests == before_session.init_requests && counts().creates == before_session.creates,
        "SDK context Init and preparation do not call vendor NR Init or manufacture features");
  if (fg_first) {
    Check(native::direct_device == nullptr && !native::bridge_device_live.load(),
          "ordinary Bridge and NR remain cold after the actual SDK Context was established");
  }
  const auto stream_key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
  ComPtr<ID3D12Resource> color12;
  Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color12))),
        "obtain the actual current mock-SDK DXGI backbuffer");
  const auto transfer12 = gpu.CreateTextureTransfer(color12.Get());

  funnel_accounting_tests::Restore consumer_restore;
  consumer_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
  consumer_restore.Value(&adapter::proxies[0].real[0]);
  adapter::proxies[0].real[0] = reinterpret_cast<void*>(&nr_edit_reuse_xefg_cases::ConsumeSdkPresent);

  std::vector<uint8_t> first_consumed;
  nlohmann::json first_execute;
  uint64_t consumer_reads = 0;
  uint64_t before_sdk_fence = 0;
  uint64_t stage_evaluations_before = 0;
  nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
    Check(output == session.proxy.Get(), "original SDK callback receives this output, not an old owner");
    ++consumer_reads;
    if (native::stage_telemetry[present_stage].evaluates == stage_evaluations_before + passes) {
      const auto& service = native::fg_service::streams.at(stream_key);
      const auto slot = std::ranges::find_if(service.slots, [&](const auto& candidate) {
        return candidate.list != nullptr && candidate.fence_value == service.submitted;
      });
      Check(service.submitted > before_sdk_fence && slot != service.slots.end(),
            "the same SDK call has its new actual color submission before any consumer read");
      first_execute = check_exact_first_submit(slot->list, features_for(service.roles[0].Handle()),
                                               submission::RecordingOwner::kXeFg);
    }
    gpu.Transition(color12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.RecordTextureReadback(color12.Get(), transfer12);
    gpu.Transition(color12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    submit();
    first_consumed = gpu.ReadTexture(transfer12);
  };

  uint32_t present_id = 900;
  const auto run_xefg = [&](uint8_t marker, nlohmann::json* frame) {
    gpu.Write(color12.Get(), marker);
    submit();
    const auto prior = native::fg_service::streams.find(stream_key);
    before_sdk_fence = (prior != native::fg_service::streams.end() ? prior->second.submitted : 0);
    const uint32_t ready_before = (prior != native::fg_service::streams.end()
                                       ? LiveWarmFixturePassMask(prior->second.roles[0].Handle())
                                       : 0);
    const auto calls_before = counts();
    stage_evaluations_before = calls_before.evaluates;
    const auto reads_before = consumer_reads;
    const auto submitted_before = xefg::SubmittedFrames();
    first_consumed.clear();
    first_execute = nullptr;
    session.Guides(++present_id, depth12, motion12);
    session.Present(present_id);
    Check(consumer_reads == reads_before + 1 && first_consumed.size() == frame_bytes,
          "one original SDK callback performs this frame's first same-queue full GPU read");
    const auto result = xefg::CurrentResult();
    const auto calls_after = counts();
    const auto evaluates = calls_after.evaluates - calls_before.evaluates;
    const auto accepted = calls_after.accepted - calls_before.accepted;
    Check(accepted == evaluates, "all attempted XeFG NR evaluations returned success through the real API");
    if (result != fg::Result::SUBMITTED || evaluates != passes) {
      const auto creates = calls_after.creates - calls_before.creates;
      const auto& service = native::fg_service::streams.at(stream_key);
      const auto ready_after = LiveWarmFixturePassMask(service.roles[0].Handle());
      const bool wrote_color = result == fg::Result::SUBMITTED;
      Check(ready_before != full_mask && creates > 0 && evaluates < passes
                && ((wrote_color && evaluates > 0 && xefg::SubmittedFrames() == submitted_before + 1)
                    || (result == fg::Result::PREPARED && evaluates == 0
                        && xefg::SubmittedFrames() == submitted_before)),
            "the distinct XeFG contract can build missing models but cannot count partial color as two passes");
      for (size_t i = 0; i < first_consumed.size(); i += pixel_bytes) {
        Check(first_consumed[i + 3] == marker
                  && (wrote_color || (first_consumed[i] == marker && first_consumed[i + 1] == marker && first_consumed[i + 2] == marker)),
              "the cold consumer sees current pixels, and preparation alone cannot edit color");
      }
      xefg_cold.push_back({{"marker", marker}, {"ready_before", ready_before}, {"ready_after", ready_after}, {"creates", creates}, {"evaluates", evaluates}, {"submitted_color", wrote_color}, {"result", static_cast<uint32_t>(result)}, {"first_rgba", {first_consumed[0], first_consumed[1], first_consumed[2], first_consumed[3]}}});
      return false;
    }
    Check(ready_before == full_mask, "first full XeFG color is after both real model creations matured");
    const auto& service = native::fg_service::streams.at(stream_key);
    const auto& sdk_write = session.context->last_handoff_write;
    const auto& submitted = sdk_write.service_submission;
    const auto models = features_for(service.roles[0].Handle());
    Check(service.queue == session.context->queue.Get() && service.queue != native::bridge.queue
              && submitted.signaled && submitted.fence == reinterpret_cast<uintptr_t>(service.fence)
              && submitted.fence_value == service.submitted && service.submitted > before_sdk_fence,
          "the proven new frame executes both signed calls and one service write on the actual SDK queue");
    Check(!first_execute.is_null() && first_execute.at("execute").get<uint64_t>() != 0
              && first_execute.at("fence").get<uintptr_t>() != 0 && first_execute.at("value").get<uint64_t>() != 0
              && first_execute.at("list").get<uint64_t>() == submitted.recording.first
              && first_execute.at("generation").get<uint64_t>() == submitted.recording.second
              && first_execute.at("replay_closed").get<bool>(),
          "original consumer observes the exact closed color recording's first Execute and healthy Signal");
    const auto state = static_cast<submission::SubmissionProofState>(first_execute.at("state").get<uint32_t>());
    Check(state == submission::SubmissionProofState::kInFlight || state == submission::SubmissionProofState::kComplete,
          "the exact first Execute carries valid queue ordering, not a later replay");
    wait(service.fence, service.submitted);
    const auto completed = service.fence->GetCompletedValue();
    Check(completed != UINT64_MAX && completed >= service.submitted
              && xefg::SubmittedFrames() == submitted_before + 1
              && sdk_write.context == reinterpret_cast<uintptr_t>(session.handle)
              && sdk_write.epoch == service.epoch && sdk_write.application_frame == session.context->application_frame
              && sdk_write.present_id == present_id && sdk_write.owner_current_at_return
              && sdk_write.sdk_result_available && SUCCEEDED(sdk_write.sdk_result)
              && sdk_write.color_executed && !sdk_write.ordinary_executed,
          "this same actual original SDK call accepts a single new current output");
    size_t changed = 0;
    for (size_t offset = 0; offset < first_consumed.size(); offset += pixel_bytes) {
      changed += first_consumed[offset] != marker || first_consumed[offset + 1] != marker
                 || first_consumed[offset + 2] != marker;
      Check(first_consumed[offset + 3] == marker, "the full first-consumer image is the current frame");
    }
    Check(changed > 0, "the original mock SDK first GPU read sees edited, not passthrough RGB");
    const std::array<uint8_t, pixel_bytes> rgba{
        first_consumed[0], first_consumed[1], first_consumed[2], first_consumed[3]};
    *frame = {{"context", sdk_write.context}, {"epoch", sdk_write.epoch}, {"application_frame", sdk_write.application_frame}, {"call", sdk_write.call}, {"present_id", present_id}, {"role", "backbuffer"}, {"subresource", 0}, {"resource", reinterpret_cast<uintptr_t>(color12.Get())}, {"models", addresses(models)}, {"evaluates", passes}, {"list", first_execute.at("list_pointer")}, {"first_execute", first_execute}, {"queue", submitted.queue}, {"fence", submitted.fence}, {"fence_value", service.submitted}, {"fence_completed", completed}, {"rgba", rgba}, {"marker", marker}, {"edited_rgb_pixels", changed}, {"readback_pixels", first_consumed.size() / pixel_bytes}, {"sdk_original_result", sdk_write.sdk_result}};
#if defined(NR_NEXUS_TEST_INTEL_SDK)
    intel.Accept(color12.Get(), depth12, motion12, first_consumed, sdk_write.epoch, sdk_write.application_frame,
                 "native-backbuffer");
#endif
    return true;
  };

  const auto before_xefg = counts();
  Check(Hooks::Enable(session.handle, 1) == 0 && xefg::Enabled(), "public SDK Off to On");
  Check(counts().init_requests == before_xefg.init_requests && counts().creates == before_xefg.creates,
        "public SDK Enable cannot call NVIDIA NR Init or Create before the actual frame");
  nlohmann::json first_xefg;
  bool xe_ready = false;
  uint32_t attempts = 0;
  for (; attempts < native::kContractDebounceFrames + 8 && !xe_ready; ++attempts) {
    xe_ready = run_xefg(static_cast<uint8_t>(140 + attempts), &first_xefg);
  }
  Check(xe_ready, "the first qualified XeFG double-Pass frame delivers edited current pixels");
  check_active_original();
  const auto xe_cold_calls = counts();
  Check(xe_cold_calls.creates == before_xefg.creates + passes
            && xe_cold_calls.init_requests == before_xefg.init_requests + (fg_first ? 1u : 0u)
            && xe_cold_calls.init_successes == before_xefg.init_successes + (fg_first ? 1u : 0u),
        "one real Init only for a true FG-first cold entry; two legitimate cold distinct XeFG handles");
  if (fg_first) {
    Check(!native::bridge_device_live.load() && native::bridge.queue == nullptr,
          "FG-first actually presented before any ordinary Bridge work or D3D12 Bridge construction");
  } else {
    Check(features_for(bridge_handle) == bridge_models, "SDK delivery did not retire ordinary live models");
  }
  nlohmann::json warmed_xefg;
  Check(run_xefg(192, &warmed_xefg), "the next qualified SDK frame immediately delivers on the warmed model contract");
  const auto xe_warm_calls = counts();
  Check(xe_warm_calls.creates == xe_cold_calls.creates && xe_warm_calls.init_requests == xe_cold_calls.init_requests
            && warmed_xefg.at("models") == first_xefg.at("models"),
        "the same SDK epoch makes no extra feature or Init and retains the two real handles");

  Check(Hooks::Enable(session.handle, 0) == 0 && !xefg::Enabled(), "public SDK On to Off");
  if (fg_first) {
    prepare_bridge();
    bool ordinary_ready = false;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !ordinary_ready; ++attempt) {
      ordinary_ready = run_bridge(static_cast<uint8_t>(64 + attempt), &first_bridge);
    }
    Check(ordinary_ready && counts().creates == xe_warm_calls.creates + passes
              && counts().init_requests == xe_warm_calls.init_requests,
          "ordinary Bridge starts on the same native original binding after FG-first with only its own legitimate cold models");
    bridge_models = features_for(bridge_handle);
  }
  const auto before_return = counts();
  nlohmann::json final_bridge;
  Check(run_bridge(216, &final_bridge) && features_for(bridge_handle) == bridge_models,
        "the ordinary Bridge immediately returns another edited frame using its mature signed handles");
  const auto final_calls = counts();
  Check(final_calls.init_requests == cold_calls.init_requests + 1 && final_calls.init_successes == cold_calls.init_successes + 1
            && final_calls.creates == before_return.creates && final_calls.evaluates == before_return.evaluates + passes
            && final_calls.accepted == before_return.accepted + passes,
        "the real routes share one compatible signed Init and the mature ordinary frame uses exactly two real calls");
  check_active_original();

  session.CloseSdk();
#if defined(NR_NEXUS_TEST_INTEL_SDK)
  intel.Finish();
#endif
  const auto same_image = [&] {
    std::array<HMODULE, 2048> modules{};
    DWORD bytes = 0;
    Check(EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &bytes)
              && bytes <= sizeof(modules),
          "enumerate all actual process modules");
    auto result = nlohmann::json::array();
    for (size_t index = 0; index < bytes / sizeof(HMODULE); ++index) {
      wchar_t image_path[32768]{};
      if (GetModuleFileNameW(modules[index], image_path, 32768) == 0) continue;
      const std::filesystem::path path(image_path);
      std::error_code file_error;
      const bool original_file = std::filesystem::equivalent(native::NgxLoaderView().nr_path, path, file_error);
      const bool known_isolation = _wcsicmp(path.parent_path().parent_path().filename().c_str(), L"nr-nexus-runtime") == 0;
      if (modules[index] == original_module || original_file || known_isolation
          || _wcsicmp(path.filename().c_str(), L"nvngx_dlssnr.dll") == 0) {
        result.push_back({{"module", reinterpret_cast<uintptr_t>(modules[index])}, {"path", path.string()}, {"physical_original", !file_error && original_file}, {"known_old_isolation", known_isolation}});
      }
    }
    return result;
  }();
  const auto& final_loader = native::NgxLoaderView();
  Check(same_image.size() == 1
            && same_image[0].at("module").get<uintptr_t>() == reinterpret_cast<uintptr_t>(original_module)
            && same_image[0].at("physical_original").get<bool>() && !same_image[0].at("known_old_isolation").get<bool>(),
        "the runtime mapping observed in the process is the exact selected original file");
  Check(counts().init_requests == final_calls.init_requests, "closing the SDK Context did not reinitialize NVIDIA NR");
  nlohmann::json receipt{
      {"schema", "nr-nexus-bridge-original-face/1"}, {"status", "Passed"}, {"evidence", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"}, {"first_route", fg_first ? "native-xefg-before-bridge" : "ordinary-bridge-without-source-ngx"}, {"recording_basis", "actual normal API results and current owned recording; no fixture owner or face assignment"}, {"images", same_image}, {"selected_original_path", final_loader.nr_path.string()}, {"image_generation", original_generation}, {"mapping_checkpoint_only", true}, {"dynamic_copy_creation", "separate full-run observation required"}, {"shared_shutdown", "report separately from the production call-site audit, not inferred from these logs"}, {"device_face", reinterpret_cast<uintptr_t>(bridge_device.Get())}, {"native_device_identity", reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(bridge_device.Get()))}, {"nr_sha256", final_loader.nr_sha256}, {"nr_version", native::direct_runtime_requirements.file_version}, {"driver_x100", native::direct_runtime_requirements.driver_x100}, {"adapter", {{"luid_low", gpu.device->GetAdapterLuid().LowPart}, {"luid_high", gpu.device->GetAdapterLuid().HighPart}}}, {"format", static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM)}, {"width", width}, {"height", height}, {"ordinary_first_double_pass", std::move(first_bridge)}, {"ordinary_first_double_pass_after_xefg", fg_first}, {"first_qualified_xefg", std::move(first_xefg)}, {"warmed_xefg", std::move(warmed_xefg)}, {"ordinary_return", std::move(final_bridge)}, {"cold_xefg_attempts", attempts}, {"nr_init_requests", final_calls.init_requests - cold_calls.init_requests}, {"nr_init_successes", final_calls.init_successes - cold_calls.init_successes}, {"switch_extra_init_calls", final_calls.init_requests - cold_calls.init_requests - 1},
#if defined(NR_NEXUS_TEST_INTEL_SDK)
      {"actual_intel_sdk", "input-and-API-acceptance-only"},
      {"actual_intel_sdk_receipt", sdk_receipt},
#else
      {"actual_intel_sdk", "NotRun"},
#endif
      {"actual_intel_internal_first_read", "NotObserved"}};
  receipt["ordinary_model_creation_window"] = std::move(ordinary_cold);
  receipt["xefg_distinct_model_creation_window"] = std::move(xefg_cold);
  {
    const addon::RuntimeLock lock(native::runtime_mutex);
    native::ForgetSourceHandleLocked(bridge_handle);
  }
  std::ofstream output(fg_first ? "bridge-xefg-native-fg-first.json" : "bridge-xefg-warm-original.json");
  output << receipt.dump(2) << '\n';
  output.close();
  Check(!output.fail(), "write the Bridge warm original runtime evidence");
  std::cout << "PASS Bridge original face: "
            << (fg_first ? "native XeFG -> ordinary" : "ordinary -> XeFG -> ordinary")
            << "; real double-Pass and current first-consumer RGBA; one actual signed Init, one observed original mapping; "
               "evidence=NATIVE_GPU+NATIVE_NGX+MOCK_XeFG\n";
}

}  // namespace bridge_xefg_warm_cases
#endif
