/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <DirectXPackedVector.h>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

// Included after the shared XeFG adapter host and the NR model substitutes.
namespace nr_edit_reuse_xefg_cases {
namespace addon = renodx::addons::dlss5;

int Run() {
  ComPtr<ID3D12InfoQueue> info;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "initialize edit reuse XeFG DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    constexpr uint32_t width = 16, height = 16;
    Gpu gpu(false, width, height);
    model_gpu = &gpu;
    gpu.device.As(&info);
    model_calls = 0;
    model_resets.clear();
    model_evaluated_handles.clear();
    model_released_handles.clear();
    fail_call = 0;
    native::enabled = true;
    native::hooks_enabled = true;
    native::shutdown_quiesce_enabled = false;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 1;
    native::present_guides = native::kPresentGuidesRequired;
    native::present_ui_correction = false;
    native::OnNativeFgOptions(false);
    native::stack_passes = 1;
    native::pass_stage[0] = 0;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::nr_adaptive_enabled = false;
    native::codec_mode = 0;
    native::feed_mode = 1;
    native::norm_governor = 0;
    native::look_mode = false;
    native::detail_stability = 1;
    native::control_mask_enabled = false;
    native::use_auto_mask = false;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::parameter_runtime_via_core = false;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "edit reuse XeFG queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "edit reuse XeFG list hooks");
    auto depth = gpu.Texture(width, height, DXGI_FORMAT_R32_FLOAT);
    auto motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
    const auto upload = [&](ID3D12Resource* resource, const auto& values,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT row_bytes) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      gpu.Transition(resource, before, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(values.data()),
                        values.size() * sizeof(values[0]), row_bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, after);
    };
    upload(depth.Get(), std::vector<float>(width * height, .5f), D3D12_RESOURCE_STATE_COMMON,
           NATIVE_INPUT_STATE, width * sizeof(float));
    upload(motion.Get(), std::vector<uint16_t>(width * height * 2, 0), D3D12_RESOURCE_STATE_COMMON,
           NATIVE_INPUT_STATE, width * 4);
    gpu.Submit();
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&native::fg_service::Service, &native::fg_service::PrepareCapture,
                    &native::fg_service::Retire, &native::fg_service::Requested);
    Session session(&gpu, 1, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    session.context->ui_composition = false;
    session.context->color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    session.extent_left = session.extent_top = 0;
    session.extent_width = width;
    session.extent_height = height;
    const auto context = reinterpret_cast<uintptr_t>(session.handle);
    const auto wait_service = [&] {
      const auto found = native::fg_service::streams.find({false, context});
      if (found == native::fg_service::streams.end() || found->second.submitted == 0) return;
      const auto& stream = found->second;
      if (stream.fence->GetCompletedValue() < stream.submitted) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "edit reuse XeFG completion event");
        const auto armed = stream.fence->SetEventOnCompletion(stream.submitted, event);
        const auto waited = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
        CloseHandle(event);
        Check(waited == WAIT_OBJECT_0, "bounded edit reuse XeFG service completion");
      }
      Check(stream.fence->GetCompletedValue() >= stream.submitted
                && SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "healthy XeFG service Fence");
    };
    std::vector<uint16_t> source(width * height * 4);
    uint32_t source_frame = 0;
    uint32_t present_id = 0;
    struct Observation {
      fg::Result result = fg::Result::NOT_SERVED;
      uint32_t evaluates = 0;
      uint64_t reuses = 0;
      int reset = -1;
      std::vector<uint8_t> pixels;
    };
    const auto frame = [&](bool reset, bool guides = true) {
      ++source_frame;
      ++present_id;
      ComPtr<ID3D12Resource> backbuffer;
      Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))),
            "edit reuse XeFG actual proxy Backbuffer");
      for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
        source[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + source_frame / 4096.f);
        source[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.375f + (pixel % width) / 4096.f);
        source[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.5f);
        source[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + .125f * ((pixel + source_frame) % 4));
      }
      upload(backbuffer.Get(), source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, width * 8);
      gpu.Submit();
      if (guides) {
        for (auto [type, resource] : {std::pair{DEPTH, depth.Get()}, std::pair{MOTION, motion.Get()}}) {
          const xefg::abi::Resource tag{.type = type, .validity = UNTIL_NEXT_PRESENT,
              .base = {0, 0}, .size = {width, height}, .resource = resource, .state = NATIVE_INPUT_STATE};
          Check(Hooks::Resource(session.handle, gpu.list.Get(), present_id, &tag) == 0, "current XeFG guide tag");
        }
      }
      const xefg::abi::Constants constants{.motion_scale_x = 1.f, .motion_scale_y = 1.f,
          .reset = (reset ? 1u : 0u), .frame_time_ms = 1000.f / 60.f};
      Check(Hooks::Constants(session.handle, present_id, &constants) == 0, "current XeFG frame constants");
      const auto before_calls = model_calls;
      const auto before_reuses = native::edit_reuse_reuses.load();
      session.Present(present_id);
      wait_service();
      Observation observed{.result = xefg::CurrentResult(), .evaluates = model_calls - before_calls,
          .reuses = native::edit_reuse_reuses.load() - before_reuses,
          .reset = (model_calls != before_calls ? model_resets.back() : -1)};
      auto readback = gpu.CreateTextureTransfer(backbuffer.Get());
      gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(backbuffer.Get(), readback);
      gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      gpu.Submit();
      observed.pixels = gpu.ReadTexture(readback);
      native::DrainRetiredResources(true);
      return observed;
    };
    bool mature = false;
    for (uint32_t prefix = 0; prefix < native::kContractDebounceFrames + 12 && !mature; ++prefix) {
      const auto observed = frame(true);
      mature = observed.result == fg::Result::SUBMITTED && observed.evaluates == 1;
    }
    Check(mature, "single Backbuffer XeFG cache submits a mock-model refresh");
    auto& stream = native::fg_service::streams.at({false, context});
    const auto owner = stream.roles[0].Handle();
    Check(frame(true).evaluates == 1, "explicit XeFG Reset refreshes the mock NR model");
    auto anchor = native::features.at(owner).slots[0].edit_reuse;
    Check(anchor != nullptr && anchor->valid, "XeFG single Backbuffer establishes an edit anchor");
    const auto generation = anchor->anchor_generation;
    const auto old_model_frame = stream.last_model_frame[0];
    const auto application_frame = session.context->application_frame;
    const auto before_duplicate_calls = model_calls;
    const auto before_duplicate_reuses = native::edit_reuse_reuses.load();
    const auto before_duplicate_submissions = stream.submitted;
    session.Present(present_id);
    Check(session.context->application_frame == application_frame && stream.application_frame == application_frame
              && model_calls == before_duplicate_calls && native::edit_reuse_reuses.load() == before_duplicate_reuses
              && stream.submitted == before_duplicate_submissions && anchor->anchor_generation == generation,
          "same public presentID advances neither application cadence, model calls, reuse nor submission");
    const auto reused = frame(false);
    Check(reused.result == fg::Result::SUBMITTED && reused.evaluates == 0 && reused.reuses == 1,
          "two XeFG base frames use one mock-model Evaluate");
    Check(anchor == native::features.at(owner).slots[0].edit_reuse && anchor->anchor_generation == generation
              && stream.last_model_frame[0] == old_model_frame
              && stream.last_output_frame[0] == session.context->application_frame,
          "reuse advances delivered source output while retaining the actual model frame and anchor");
    Check(reused.pixels.size() == source.size() * sizeof(uint16_t)
              && std::memcmp(reused.pixels.data(), source.data(), reused.pixels.size()) == 0,
          "neutral XeFG reuse returns this frame's new half-float color and alpha exactly");
    ComPtr<ID3D12Resource> backbuffer;
    Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))),
          "inspect XeFG Backbuffer ownership");
    for (const auto& binding : anchor->bindings) {
      for (const auto& resource : binding.resources) {
        Check(resource.Get() != backbuffer.Get(), "cache bindings retain private current-original surfaces rather than DXGI Backbuffer");
      }
    }
    const auto refreshed = frame(false);
    Check(refreshed.result == fg::Result::SUBMITTED && refreshed.evaluates == 1 && refreshed.reuses == 0
              && refreshed.reset == 0, "third consecutive XeFG source refreshes without a false Reset after reuse");
    Check(frame(false).reuses == 1, "XeFG can reuse the refreshed anchor exactly once");
    const auto reset = frame(true);
    Check(reset.evaluates == 1 && reset.reuses == 0 && reset.reset == 1, "explicit Reset interrupts XeFG reuse cadence");
    anchor.reset();
    const auto recover = [&] {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
        const auto observed = frame(false);
        Check(observed.reuses == 0, "interrupted XeFG source cannot reuse before a fresh model output");
        if (observed.result == fg::Result::SUBMITTED && observed.evaluates == 1) {
          Check(observed.reset == 1, "XeFG recovery sends Reset on its first actual model refresh");
          return;
        }
      }
      Check(false, "bounded XeFG source recovery reaches a fresh model output");
    };
    const auto missing = frame(false, false);
    Check(missing.result != fg::Result::SUBMITTED && missing.evaluates == 0 && missing.reuses == 0
              && std::memcmp(missing.pixels.data(), source.data(), missing.pixels.size()) == 0,
          "missing Required guides keep the current source and do not consume cached edits");
    recover();
    std::array<bool, 3> quarantine{};
    for (size_t index = 0; index < quarantine.size(); ++index) {
      quarantine[index] = stream.slots[index].quarantined;
      stream.slots[index].quarantined = true;
    }
    const auto skipped = frame(false);
    for (size_t index = 0; index < quarantine.size(); ++index) {
      stream.slots[index].quarantined = quarantine[index];
    }
    Check(skipped.result != fg::Result::SUBMITTED && skipped.evaluates == 0 && skipped.reuses == 0,
          "a busy XeFG recording ring skips one real base frame without consuming the anchor");
    recover();
    Check(frame(true).evaluates == 1, "establish XeFG refresh before injected model failure");
    fail_call = model_calls + 1;
    const auto failed = frame(true);
    fail_call = 0;
    Check(failed.result != fg::Result::SUBMITTED && failed.evaluates == 1 && failed.reuses == 0
              && std::memcmp(failed.pixels.data(), source.data(), failed.pixels.size()) == 0,
          "actual mock-model failure restores current XeFG source instead of cached output");
    recover();
    auto final_anchor = native::features.at(owner).slots[0].edit_reuse;
    Check(final_anchor != nullptr, "recovered XeFG source owns a new edit cache");
    std::weak_ptr<addon::edit_reuse::State> retired_anchor = final_anchor;
    final_anchor.reset();
    native::fg_service::Retire();
    gpu.Submit();
    native::PruneCompletedSubmissionUses();
    native::DrainRetiredResources(true);
    Check(!native::fg_service::streams.contains({false, context}) && !native::features.contains(owner)
              && retired_anchor.expired(), "XeFG source retirement releases the cache after owned recordings and Fence completion");
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    model_gpu = nullptr;
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "edit reuse XeFG debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS XeFG x2 single Backbuffer evidence=WARP+MOCK SR=MOCK NR=MOCK XeFG=MOCK"
                 " source_updates=per_base_frame duplicate_present_id=stable refresh_reuse_refresh=1/0/1"
                 " reset/guides/gap/failure/retirement=checked interpolation=not_executed\n";
    return 0;
  } catch (const std::exception& error) {
    fail_call = 0;
    std::cerr << "nr-edit-reuse-xefg-contracts: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace nr_edit_reuse_xefg_cases
