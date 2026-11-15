/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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

static std::vector<uint16_t> model_pixels;

NVSDK_NGX_Result NVSDK_CONV EditedModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto desc = output->GetDesc();
  Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && model_pixels.size() == desc.Width * desc.Height * 4,
        "PQ mock writes encoded FP16 model-domain pixels");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  model_gpu->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto upload = model_gpu->Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "PQ mock upload map");
  for (uint32_t y = 0; y < desc.Height; ++y) {
    std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                model_pixels.data() + y * desc.Width * 4, static_cast<size_t>(desc.Width) * 8);
  }
  upload->Unmap(0, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->uploads.push_back(upload);
  return result;
}

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
    native::direct_api.private_instance = true;
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
    // Supply the existing model substitute as the explicitly private pre-FG
    // provider. The real loader and NVIDIA runtime are outside this fixture.
    native::ngx_loader_staging = {};
    native::ngx_loader_staging.nr_ready = true;
    native::ngx_loader_staging.native_ready = true;
    native::ngx_loader_staging.native_api = native::direct_api;
    native::ngx_loader_staging.nr_requirements = native::direct_runtime_requirements;
    native::PublishNgxLoaderSymbols();
    native::parameter_runtime_ready = true;
    native::parameter_runtime_via_core = false;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "edit reuse XeFG queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "edit reuse XeFG list hooks");
    const auto upload = [&](ID3D12Resource* resource, const auto& values,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT row_bytes) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      if (resource->GetDesc().Format == DXGI_FORMAT_R32G8X24_TYPELESS) {
        // The shared transfer copies subresource 0: one float per depth pixel,
        // without the separate stencil plane or the resource's packed size.
        Check(transfer.footprint.Footprint.Depth == 1
                  && transfer.row_bytes == resource->GetDesc().Width * sizeof(float),
              "XeFG planar depth upload uses the four-byte plane-zero footprint");
      }
      gpu.Transition(resource, before, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(values.data()),
                        values.size() * sizeof(values[0]), row_bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, after);
    };
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&native::fg_service::Service, &native::fg_service::PrepareCapture,
                    &native::fg_service::Retire, &native::fg_service::Requested);
    for (const bool pq : {false, true}) {
      native::direct_api.evaluate = (pq ? EditedModel : nr_edit_reuse_gpu_cases::NeutralModel);
      native::codec_mode = (pq ? 3u : 0u);
      native::transfer_mode = 0;
      native::style_highlight_guard = 0.f;
      native::chroma_clamp_stops = 2.f;
      native::diffuse_white_nits = 200.f;
      const uint32_t guide_width = (pq ? width / 2 : width);
      const uint32_t guide_height = (pq ? height / 2 : height);
      auto depth = gpu.Texture(guide_width, guide_height,
          pq ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT);
      auto motion = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R16G16_FLOAT);
      upload(depth.Get(), std::vector<float>(guide_width * guide_height, .5f), D3D12_RESOURCE_STATE_COMMON,
             NATIVE_INPUT_STATE, guide_width * sizeof(float));
      upload(motion.Get(), std::vector<uint16_t>(guide_width * guide_height * 2, 0), D3D12_RESOURCE_STATE_COMMON,
             NATIVE_INPUT_STATE, guide_width * 4);
      gpu.Submit();
      Session session(&gpu, 1, 0, pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT);
      session.context->ui_composition = false;
      session.context->color_space = (pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
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
      std::vector<uint32_t> source_pq(width * height);
      // Independent double-precision ST.2084 and D65 primary transforms. These
      // reference absolute display light, without calling the production codec.
      const auto pq_decode = [](double code) {
        const double p = std::pow(code, 32.0 / 2523.0);
        return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0);
      };
      const auto pq_encode = [](double light) {
        const double p = std::pow(std::clamp(light, 0.0, 1.0), 2610.0 / 16384.0);
        return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
      };
      const auto linear_709 = [&](uint32_t packed) {
        const double r = pq_decode((packed & 1023u) / 1023.0);
        const double g = pq_decode(((packed >> 10) & 1023u) / 1023.0);
        const double b = pq_decode(((packed >> 20) & 1023u) / 1023.0);
        return std::array<double, 3>{1.6604910 * r - .5876411 * g - .0728499 * b,
            -.1245505 * r + 1.1328999 * g - .0083494 * b, -.0181508 * r - .1005789 * g + 1.1187297 * b};
      };
      const auto srgb_encode = [](double light) {
        light = std::clamp(light, 0.0, 1.0);
        return (light <= .0031308 ? 12.92 * light : 1.055 * std::pow(light, 1.0 / 2.4) - .055);
      };
      constexpr std::array<double, 3> gains{1.125, 1.25, 1.375};
      const auto source_matches = [&](const std::vector<uint8_t>& pixels) {
        const size_t bytes = (pq ? source_pq.size() * sizeof(uint32_t) : source.size() * sizeof(uint16_t));
        const void* current = (pq ? static_cast<const void*>(source_pq.data()) : static_cast<const void*>(source.data()));
        return pixels.size() == bytes && std::memcmp(pixels.data(), current, bytes) == 0;
      };
      uint32_t source_frame = 0;
      uint32_t present_id = 0;
      struct Observation {
        fg::Result result = fg::Result::NOT_SERVED;
        uint32_t evaluates = 0;
        uint64_t reuses = 0;
        int reset = -1;
        std::vector<uint8_t> pixels;
      };
      enum class Change { NONE, MOTION, DEPTH, COLOR };
      const auto frame = [&](bool reset, bool guides = true, Change change = Change::NONE) {
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
          const uint32_t rgb = (pixel % width < 2 ? (1u << 10) | (2u << 20)
              : (change == Change::COLOR ? 610u | (650u << 10) | (680u << 20) : 460u | (500u << 10) | (530u << 20)));
          source_pq[pixel] = rgb | (((pixel + source_frame) % 4) << 30);
        }
        if (pq) {
          model_pixels.resize(width * height * 4);
          for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
            auto proxy = linear_709(source_pq[pixel]);
            for (auto& channel : proxy) {
              channel = std::max(channel, 0.0) * 10000.0 / native::diffuse_white_nits.load();
            }
            const double peak = std::max({proxy[0], proxy[1], proxy[2]});
            const double toe = peak + .014 * peak / (peak + .001);
            const double shoulder = (toe > .75 ? .75 + .25 * (1.0 - std::exp(-5.770780 * (toe - .75))) : toe);
            for (uint32_t channel = 0; channel < 3; ++channel) {
              model_pixels[pixel * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(
                  static_cast<float>(srgb_encode(proxy[channel] * shoulder / std::max(peak, 1e-6) * gains[channel])));
            }
            model_pixels[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
          }
          upload(backbuffer.Get(), source_pq, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, width * 4);
        } else {
          upload(backbuffer.Get(), source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, width * 8);
        }
        upload(depth.Get(), std::vector<float>(guide_width * guide_height, change == Change::DEPTH ? .9f : .5f),
               NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
        std::vector<uint16_t> motion_values(guide_width * guide_height * 2, 0);
        if (change == Change::MOTION) {
          for (size_t pixel = 0; pixel < motion_values.size() / 2; ++pixel) {
            motion_values[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(32.f);
          }
        }
        upload(motion.Get(), motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
        gpu.Submit();
        if (guides) {
          for (auto [type, resource] : {std::pair{DEPTH, depth.Get()}, std::pair{MOTION, motion.Get()}}) {
            const xefg::abi::Resource tag{.type = type, .validity = UNTIL_NEXT_PRESENT,
                .base = {0, 0}, .size = {guide_width, guide_height}, .resource = resource, .state = NATIVE_INPUT_STATE};
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
      const auto check_pq_output = [&](const Observation& observed, bool exact_dark) {
        Check(observed.pixels.size() == source_pq.size() * sizeof(uint32_t), "PQ service returns packed HDR10 pixels");
        bool changed = false;
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
          uint32_t actual = 0;
          std::memcpy(&actual, observed.pixels.data() + pixel * sizeof(uint32_t), sizeof(actual));
          Check((actual >> 30) == (source_pq[pixel] >> 30), "PQ refresh and reuse preserve every current two-bit alpha code");
          if (pixel % width < 2) {
            if (exact_dark) {
              Check(actual == source_pq[pixel], "near-black PQ reuse selects current original RGB and alpha exactly");
            }
            continue;
          }
          auto light = linear_709(source_pq[pixel]);
          for (uint32_t channel = 0; channel < 3; ++channel) {
            light[channel] *= gains[channel];
          }
          const std::array<double, 3> target{.6274040 * light[0] + .3292820 * light[1] + .0433136 * light[2],
              .0690970 * light[0] + .9195400 * light[1] + .0113612 * light[2],
              .0163916 * light[0] + .0880132 * light[1] + .8955950 * light[2]};
          for (uint32_t channel = 0; channel < 3; ++channel) {
            const int expected = static_cast<int>(std::lround(pq_encode(target[channel]) * 1023.0));
            const int code = static_cast<int>((actual >> (channel * 10)) & 1023u);
            Check(std::abs(code - expected) <= 2, "non-neutral PQ edit matches independent linear-light and primary reference within two codes");
          }
          changed |= (actual & 0x3fffffffu) != (source_pq[pixel] & 0x3fffffffu);
        }
        Check(changed, "PQ service exercises a valid non-neutral edit rather than only selecting original pixels");
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
      if (pq) {
        check_pq_output(reused, true);
      } else {
        Check(source_matches(reused.pixels), "neutral XeFG reuse returns this frame's new half-float color and alpha exactly");
      }
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
      if (pq) {
        check_pq_output(refreshed, false);
      }
      Check(frame(false).reuses == 1, "XeFG can reuse the refreshed anchor exactly once");
      const auto reset = frame(true);
      Check(reset.evaluates == 1 && reset.reuses == 0 && reset.reset == 1, "explicit Reset interrupts XeFG reuse cadence");
      anchor.reset();
      if (pq) {
        for (const auto change : {Change::MOTION, Change::DEPTH, Change::COLOR}) {
          Check(frame(true).evaluates == 1, "refresh PQ anchor before rejection case");
          const auto rejected = frame(false, true, change);
          Check(rejected.result == fg::Result::SUBMITTED && rejected.evaluates == 0 && rejected.reuses == 1
                    && source_matches(rejected.pixels),
                "motion bounds, depth and color rejection each preserve exact current packed PQ source");
        }
        native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
        Check(frame(true).evaluates == 1, "refresh neutral PQ anchor");
        const auto neutral = frame(false);
        Check(neutral.evaluates == 0 && neutral.reuses == 1 && source_matches(neutral.pixels),
              "neutral PQ reuse preserves current packed RGB and all alpha codes exactly");
        native::direct_api.evaluate = EditedModel;
        Check(frame(true).evaluates == 1, "refresh edited PQ anchor before white change");
        const auto old_contract = native::features.at(owner).slots[0].edit_reuse->contract;
        native::diffuse_white_nits = 240.f;
        const auto white_changed = frame(false);
        Check(white_changed.evaluates == 1 && white_changed.reuses == 0
                  && native::features.at(owner).slots[0].edit_reuse->contract != old_contract,
              "actual PQ white divisor change invalidates the anchor without relying on a configuration-generation bump");
        native::diffuse_white_nits = 200.f;
        Check(frame(true).evaluates == 1, "refresh PQ anchor before actual color-contract change");
        session.context->color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        Check(frame(false).reuses == 0, "same packed output with a linear color-space contract cannot consume the PQ anchor");
        session.context->color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        bool restored = false;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12 && !restored; ++attempt) {
          const auto observed = frame(true);
          Check(observed.reuses == 0, "restoring PQ requires an actual fresh model output");
          restored = observed.result == fg::Result::SUBMITTED && observed.evaluates == 1;
        }
        Check(restored, "PQ color contract recovers after a bounded model refresh");
      }
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
                && source_matches(missing.pixels),
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
                && source_matches(failed.pixels),
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
    }
    model_pixels.clear();
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
                 " formats=scRGB/HDR10-PQ nonneutral_PQ=CPU_reference packed_RGBA=exact_on_rejection"
                 " depth=scRGB:R32_FLOAT/PQ:R32G8X24_TYPELESS_plane0"
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
