/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <DirectXPackedVector.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <utils/hash.hpp>

// Included after the native WARP/model and ReShade device/queue fixtures.
namespace nr_edit_reuse_present_cases {
namespace addon = renodx::addons::dlss5;
namespace stages = stage_execution_cases;
namespace reuse = nr_edit_reuse_gpu_cases;
constexpr auto FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto PRESENT = D3D12_RESOURCE_STATE_PRESENT;
using Pixel = std::array<uint16_t, 4>;
constexpr Pixel GAME_PIXEL{0x3400, 0x3600, 0x3800, 0x3900};
constexpr Pixel OVERWRITE_PIXEL{0x3a00, 0x3400, 0x3600, 0x3800};

struct Swapchain final : reshade::api::swapchain {
  PresentFaceDevice device;
  PresentFaceQueue queue;
  ComPtr<IDXGISwapChain3> swapchain;
  std::array<ComPtr<ID3D12Resource>, 2> buffers;
  HWND window = nullptr;

  Swapchain(const Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height) : window(hwnd) {
    device.native = gpu.device.Get();
    queue.device = &device;
    queue.native = gpu.queue.Get();
    queue.allow_submit = true;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = FORMAT;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = static_cast<UINT>(buffers.size());
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> created;
    Check(SUCCEEDED(gpu.factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &created))
              && SUCCEEDED(created.As(&swapchain)), "Present x2 hidden DXGI swapchain");
    BindBuffers();
  }

  void BindBuffers() {
    for (uint32_t index = 0; index < buffers.size(); ++index) {
      Check(SUCCEEDED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffers[index]))), "Present x2 actual DXGI buffer");
    }
  }

  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(swapchain.Get()); }
  void get_private_data(const uint8_t[16], uint64_t*) const override { throw std::logic_error("unexpected swapchain data"); }
  void set_private_data(const uint8_t[16], uint64_t) override { throw std::logic_error("unexpected swapchain data"); }
  reshade::api::device* get_device() override { return &device; }
  void* get_hwnd() const override { return window; }
  reshade::api::resource get_back_buffer(uint32_t index) override {
    return {reinterpret_cast<uintptr_t>(buffers.at(index).Get())};
  }
  uint32_t get_back_buffer_count() const override { return static_cast<uint32_t>(buffers.size()); }
  uint32_t get_current_back_buffer_index() const override { return swapchain->GetCurrentBackBufferIndex(); }
  bool check_color_space_support(reshade::api::color_space space) const override {
    return space == reshade::api::color_space::scrgb;
  }
  reshade::api::color_space get_color_space() const override { return reshade::api::color_space::scrgb; }
};

NVSDK_NGX_Result NVSDK_CONV CreateSr(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature feature,
                                   NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  Check(feature == native::kFeatureDlss, "Present x2 registers a game SR feature");
  static uint32_t sr_handle = 1;
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(&sr_handle);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto game_list = model_gpu->list;
  model_gpu->list = list;
  const auto restore = [&](int*) { model_gpu->list = game_list; };
  int key = 0;
  const std::unique_ptr<int, decltype(restore)> restore_list(&key, restore);
  return reuse::ConstantModel(list, handle, parameters, callback);
}

struct Observed {
  uint64_t outputs = 0;
  uint64_t consumed_serial = 0;
  uint64_t refreshes = 0;
  uint64_t reuses = 0;
  uint64_t history_sequence = 0;
  uint64_t anchor_generation = 0;
  uint64_t reuse_count = 0;
  uint64_t ring_submissions = 0;
  uint32_t calls = 0;
  uint32_t creates = 0;
};

}  // namespace nr_edit_reuse_present_cases

#include "nr_edit_reuse_present_diagnostic_cases.hpp"
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
#include "nr_edit_reuse_present_native_cases.hpp"
#endif

int RunNrEditReusePresentContracts(const char* project_id_path, bool diagnostic_only) {
  using namespace nr_edit_reuse_present_cases;
  ComPtr<ID3D12InfoQueue> info;
  try {
    const auto module = GetModuleHandleW(nullptr);
    reshade::internal::get_reshade_module_handle(module);
    Check(renodx::utils::directx::Initialize(), "Present x2 initializes DirectX");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    const bool native_ngx = project_id_path != nullptr;
    Gpu gpu(native_ngx);
    model_gpu = &gpu;
    gpu.device.As(&info);
    ComPtr<ID3D12InfoQueue1> immediate_messages;
    DWORD message_cookie = 0;
    if (SUCCEEDED(gpu.device.As(&immediate_messages))) {
      Check(SUCCEEDED(immediate_messages->RegisterMessageCallback(+[](
          D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID,
          LPCSTR message, void*) {
        if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message << '\n';
        }
      }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &message_cookie)), "Present x2 observes D3D12 validation");
    }
    native::enabled = true;
    native::hooks_enabled = true;
    native::d3d12_device_seen = true;
    native::shutdown_quiesce_enabled = false;
    native::dedupe_mode = addon::kDedupePerEvaluate;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 0;
    native::present_frames = 1;
    native::present_guides = native::kPresentGuidesRequired;
    native::stack_passes = 1;
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
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
    using nr_edit_reuse_present_native_cases::Runtime;
    std::unique_ptr<Runtime> runtime;
    if (native_ngx) {
      runtime = std::make_unique<Runtime>(gpu, project_id_path);
    }
#else
    Check(!native_ngx, "build ordinary Present native verification with the public NGX SDK");
#endif
    if (!native_ngx) {
      native::direct_api.module = module;
      native::direct_api.create = stages::Create;
      native::direct_api.evaluate = Evaluate;
      native::direct_api.release = ReleaseModel;
      native::direct_load_state = native::DirectLoadState::Ready;
      native::direct_device = gpu.device.Get();
      native::direct_device->AddRef();
      native::direct_device_native = gpu.device.Get();
      native::direct_runtime_requirements.dual_models = true;
      native::direct_runtime_requirements.file_version = "310.9.16.0";
      native::parameter_runtime_ready = true;
      native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
      native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
      native::ngx_loader_staging.nr_ready = true;
      native::ngx_loader_staging.core_ready = true;
      native::ngx_loader_staging.nr_requirements = native::direct_runtime_requirements;
      native::PublishNgxLoaderSymbols();
    }
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Present x2 native queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Present x2 native list hooks");

    uint32_t size = (native_ngx ? 960u : 16u);
    const uint32_t native_height = 540;
    HWND window = CreateWindowExW(0, L"STATIC", L"NR Nexus Present x2 fixture", WS_OVERLAPPED,
        0, 0, size, size, nullptr, nullptr, module, nullptr);
    Check(window != nullptr, "Present x2 hidden test window");
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window_owner(window, &DestroyWindow);
    Swapchain output(gpu, window, size, native_ngx ? native_height : size);
    const auto back = [&] { return output.buffers.at(output.get_current_back_buffer_index()).Get(); };
    const auto finish = [&] {
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "Present x2 test fence signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "Present x2 fence event");
      const HRESULT hr = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      const uint64_t completed = gpu.fence->GetCompletedValue();
      Check(wait == WAIT_OBJECT_0 && completed != UINT64_MAX && completed >= gpu.serial,
            "Present x2 real queue fence completes");
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "Present x2 game list closes");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      finish();
    };
    const auto reopen = [&] {
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "Present x2 game list reset");
    };
    const auto swap = [&] {
      const auto index = output.get_current_back_buffer_index();
      Check(SUCCEEDED(output.swapchain->Present(0, 0))
                && output.get_current_back_buffer_index() != index,
            "native DXGI Present rotates the actual backbuffers");
      finish();
    };
    const auto event = [&] {
      native::OnPresent(&output.queue, &output, nullptr, nullptr, 0, nullptr);
      finish();
    };
    const auto stats = [&] {
      const auto& stream = native::present_streams.at(&output);
      Observed observed{.outputs = stream.nr_presents, .consumed_serial = stream.consumed_serial,
          .refreshes = native::edit_reuse_refreshes.load(), .reuses = native::edit_reuse_reuses.load(),
          .ring_submissions = native::present_ring.submitted, .calls = model_calls, .creates = next_handle};
      if (auto found = native::features.find(stream.Handle()); found != native::features.end()) {
        if (const auto& state = found->second.slots[0].edit_reuse) {
          observed.history_sequence = state->source_sequence;
          observed.anchor_generation = state->anchor_generation;
          observed.reuse_count = state->reuse_count;
        }
      }
      return observed;
    };
    const auto no_output = [](const Observed& before, const Observed& after, const char* message) {
      Check(after.outputs == before.outputs && after.consumed_serial == before.consumed_serial
                && after.calls == before.calls && after.refreshes == before.refreshes && after.reuses == before.reuses
                && after.history_sequence <= before.history_sequence
                && after.anchor_generation <= before.anchor_generation && after.reuse_count <= before.reuse_count,
            message);
    };
    const auto upload_pixels = [&](ID3D12Resource* target, const auto& pixels, D3D12_RESOURCE_STATES resting) {
      const auto desc = target->GetDesc();
      const auto pixel_bytes = sizeof(pixels.front());
      auto transfer = gpu.CreateTextureTransfer(target);
      gpu.Transition(target, resting, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(target, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                        pixels.size() * pixel_bytes, static_cast<UINT>(desc.Width * pixel_bytes));
      gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, resting);
    };
    const auto upload = [&](ID3D12Resource* target, const auto& value, D3D12_RESOURCE_STATES resting) {
      const auto desc = target->GetDesc();
      upload_pixels(target, std::vector(desc.Width * desc.Height, value), resting);
    };
    const auto read_back = [&](ID3D12Resource* texture = nullptr,
                               D3D12_RESOURCE_STATES resting = PRESENT) {
      reopen();
      const auto image = stages::Capture(gpu, texture != nullptr ? texture : back(), resting);
      submit();
      reopen();
      auto bytes = image.Pixels();
      std::vector<Pixel> pixels(static_cast<size_t>(image.width) * image.height);
      Check(bytes.size() == pixels.size() * sizeof(Pixel), "Present x2 reads all FP16 backbuffer pixels");
      std::memcpy(pixels.data(), bytes.data(), bytes.size());
      return pixels;
    };

    const float jitter_x = (native_ngx ? 0.f : .25f);
    const float jitter_y = (native_ngx ? 0.f : -.25f);
    Pixel game_pixel = GAME_PIXEL;
    ComPtr<ID3D12Resource> color, depth, motion, sr_output;
    addon::bridge::OwnedParameters base;
    NVSDK_NGX_Parameter* source_parameters = &base;
    NVSDK_NGX_Handle* sr_handle = nullptr;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
    if (runtime != nullptr) {
      source_parameters = runtime->sr_parameters;
    }
#endif
    if (!native_ngx) {
      native::ngx_slot_real[0].create = reinterpret_cast<void*>(CreateSr);
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(reuse::CopySr);
      native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) -> NVSDK_NGX_Result {
        return NVSDK_NGX_Result_Success;
      });
    }
    const auto create_source = [&] {
      const uint32_t render_width = (native_ngx ? 640u : size);
      const uint32_t render_height = (native_ngx ? 360u : size);
      const uint32_t output_height = (native_ngx ? native_height : size);
      color = gpu.Texture(render_width, render_height, FORMAT, NATIVE_INPUT_STATE);
      motion = gpu.Texture(render_width, render_height, DXGI_FORMAT_R32G32_FLOAT, NATIVE_INPUT_STATE);
      depth = gpu.Texture(render_width, render_height, DXGI_FORMAT_R32_FLOAT, NATIVE_INPUT_STATE);
      auto desc = color->GetDesc();
      desc.Width = size;
      desc.Height = output_height;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(sr_output.ReleaseAndGetAddressOf()))),
          "Present x2 SR output is a full FP16 UAV");
      if (native_ngx) {
        const auto half = DirectX::PackedVector::XMConvertFloatToHalf;
        std::vector<Pixel> source(static_cast<size_t>(render_width) * render_height);
        for (uint32_t y = 0; y < render_height; ++y) {
          for (uint32_t x = 0; x < render_width; ++x) {
            const float u = static_cast<float>(x) / render_width;
            const float v = static_cast<float>(y) / render_height;
            source[static_cast<size_t>(y) * render_width + x] = {
                half(.12f + .65f * u), half(.1f + .65f * v), half(.15f + .5f * (1.f - u * v)), half(1.f)};
          }
        }
        upload_pixels(color.Get(), source, NATIVE_INPUT_STATE);
      } else {
        upload(color.Get(), GAME_PIXEL, NATIVE_INPUT_STATE);
      }
      upload(motion.Get(), std::array<float, 2>{0.f, 0.f}, NATIVE_INPUT_STATE);
      upload(depth.Get(), .5f, NATIVE_INPUT_STATE);
      submit();
      reopen();
      source_parameters->Set(NVSDK_NGX_Parameter_Color, color.Get());
      source_parameters->Set(NVSDK_NGX_Parameter_Output, sr_output.Get());
      source_parameters->Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
      source_parameters->Set(NVSDK_NGX_Parameter_Depth, depth.Get());
      source_parameters->Set(NVSDK_NGX_Parameter_Width, render_width);
      source_parameters->Set(NVSDK_NGX_Parameter_Height, render_height);
      source_parameters->Set(NVSDK_NGX_Parameter_OutWidth, size);
      source_parameters->Set(NVSDK_NGX_Parameter_OutHeight, output_height);
      source_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_width);
      source_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_height);
      if (!native_ngx) {
        source_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                               static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
      }
      source_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      source_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      NVSDK_NGX_Result result;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      if (native_ngx) {
        result = native::HookedCreateFeatureSlot<Runtime::SR_SLOT>(gpu.list.Get(), native::kFeatureDlss,
                                                                  source_parameters, &sr_handle);
      } else
#endif
      {
        result = native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, source_parameters, &sr_handle);
      }
      Check(NVSDK_NGX_SUCCEED(result) && sr_handle != nullptr, "Present x2 wrapped game SR Create");
    };
    const auto record = [&](bool reset = false, bool jitter = true, bool overwrite = false,
                            ID3D12Resource* destination = nullptr) {
      if (!native_ngx) {
        upload(color.Get(), game_pixel, NATIVE_INPUT_STATE);
      }
      auto parameters = base;
      auto* evaluate_parameters = (native_ngx ? source_parameters : &parameters);
      evaluate_parameters->Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(reset));
      evaluate_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter_x);
      if (jitter) {
        evaluate_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter_y);
      }
      NVSDK_NGX_Result result;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      if (native_ngx) {
        result = native::HookedEvaluateFeatureSlot<Runtime::SR_SLOT>(gpu.list.Get(), sr_handle, evaluate_parameters, nullptr);
      } else
#endif
      {
        result = native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr_handle, evaluate_parameters, nullptr);
      }
      Check(NVSDK_NGX_SUCCEED(result), "Present x2 wrapped game SR Evaluate succeeds");
      auto* target = (destination != nullptr ? destination : back());
      // The game list supplies the native transitions and the complete
      // delivery. Its one Execute is the only submission before the callback.
      gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.Transition(target, PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyResource(target, sr_output.Get());
      gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, PRESENT);
      gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (overwrite) {
        upload(target, OVERWRITE_PIXEL, PRESENT);
      }
    };
    const auto present_frame = [&](bool reset = false, bool recycle_recording = false) {
      record(reset);
      const uint64_t queue_entries = native::queue_submit_entries.load();
      submit();
      Check(native::queue_submit_entries.load() == queue_entries + 1, "one hooked native producer Execute precedes Present");
      if (recycle_recording) {
        reopen();
        Check(SUCCEEDED(gpu.list->Close()), "retire the completed producer before Present");
      }
      event();
    };
    const auto alpha_preserved = [&](const std::vector<Pixel>& pixels) {
      for (const auto& pixel : pixels) {
        Check(pixel[3] == game_pixel[3], "Present x2 preserves current backbuffer alpha");
      }
    };
    const auto unchanged_pixels = [&](const std::vector<Pixel>& pixels, Pixel expected) {
      Check(pixels == std::vector(size * size, expected), "Present x2 untouched backbuffer has exact game pixels");
    };
    const auto warm = [&](bool require_prepared) {
      bool prepared = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto before = stats();
        present_frame();
        const auto after = stats();
        const auto pixels = read_back();
        swap();
        if (after.outputs != before.outputs) {
          Check(after.outputs == before.outputs + 1 && after.calls == before.calls + 1
                    && after.refreshes == before.refreshes + 1 && after.reuses == before.reuses
                    && after.consumed_serial > before.consumed_serial,
                "first accepted Present after a cold start or handoff refreshes");
          Check(!require_prepared || prepared, "cold start observes a real prepared-only submission");
          return;
        }
        no_output(before, after, "prepared-only Present leaves actual output and consumed serial unchanged");
        unchanged_pixels(pixels, GAME_PIXEL);
        if (after.creates > before.creates) {
          Check(after.ring_submissions == before.ring_submissions + 1, "prepared model executes once on the Present ring");
          prepared = true;
        }
      }
      Check(false, "Present x2 reaches a completed refresh after bounded preparation");
    };

    event();  // Register the real swapchain inventory before the game's first producer.
    create_source();
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
    if (runtime != nullptr) {
      const auto read_sr = [&] {
        auto pixels = read_back(sr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Check(SUCCEEDED(gpu.list->Close()), "close empty recording before native backbuffer readback");
        return pixels;
      };
      nr_edit_reuse_present_native_cases::Run(*runtime, gpu, stats, present_frame, read_back, read_sr, swap);
      Check(SUCCEEDED(gpu.list->Close()), "close empty fixture list before NGX teardown");
      Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<Runtime::SR_SLOT>(sr_handle)),
            "ordinary Present x2 releases the real SR handle");
      native::RetirePresentStream(&output);
      Check(native::ReleasePresentPath(), "ordinary Present x2 releases the completed native Present ring");
      native::DrainRetiredResources(true);
      runtime->Shutdown();
      std::cout << "PASS ordinary Present x2 NATIVE_GPU+NATIVE_NGX: real SR and NR, 1/0/1/0, actual DXGI buffer\n";
      return 0;
    }
#endif
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* block) {
      const auto& stream = native::present_streams.at(&output);
      const auto* captured_depth = native::GetD3D12Resource(&stream.parameters, NVSDK_NGX_Parameter_Depth);
      const auto* captured_motion = native::GetD3D12Resource(&stream.parameters, NVSDK_NGX_Parameter_MotionVectors);
      Check(stages::presets.at(model) == 2 && captured_depth != nullptr && captured_depth != depth.Get()
                && captured_motion != nullptr && captured_motion != motion.Get()
                && native::GetD3D12Resource(block, "DLSSNR.Depth") == captured_depth
                && native::GetD3D12Resource(block, "DLSSNR.MVec") != nullptr
                && native::GetFloat(block, "DLSSNR.JitterOffsetX") == jitter_x
                && native::GetFloat(block, "DLSSNR.JitterOffsetY") == jitter_y
                && native::GetFloat(block, "DLSSNR.MVecScaleX") == 1.f
                && native::GetFloat(block, "DLSSNR.MVecScaleY") == 1.f,
            "Present x2 model receives actual captured guides, source jitter and full motion scale");
    };
    warm(true);
    if (diagnostic_only) {
      nr_edit_reuse_present_diagnostic_cases::Run(std::filesystem::current_path(), &gpu, &output, size,
          record, submit, finish, swap, reopen, motion.Get(), depth.Get());
      model_parameter_observer = {};
      Check(native::HookedReleaseFeatureSlot<0>(sr_handle) == NVSDK_NGX_Result_Success, "release diagnostic game SR");
      native::RetirePresentStream(&output);
      Check(native::ReleasePresentPath(), "release completed diagnostic Present recordings");
      native::DrainRetiredResources(true);
      native::UnhookMixedPresentCopyHooks();
      return 0;
    }
    std::vector<Pixel> refreshed;
    for (uint32_t index = 0; index < 4; ++index) {
      const bool refresh = (index % 2 == 0);
      game_pixel[3] = (refresh ? GAME_PIXEL[3] : OVERWRITE_PIXEL[3]);
      const auto before = stats();
      present_frame(index == 0, index == 2);
      const auto after = stats();
      std::cout << "PRESENT_X2 frame=" << index << " model_calls=" << after.calls - before.calls
                << " refreshes=" << after.refreshes - before.refreshes
                << " reuses=" << after.reuses - before.reuses << " outputs=" << after.outputs - before.outputs
                << " consumed=" << after.consumed_serial << " sequence=" << after.history_sequence << '\n';
      Check(after.outputs == before.outputs + 1 && after.consumed_serial > before.consumed_serial
                && after.calls == before.calls + refresh && after.refreshes == before.refreshes + refresh
                && after.reuses == before.reuses + !refresh && after.history_sequence > before.history_sequence,
            "Present x2 delivers four consecutive real bases in model cadence 1/0/1/0");
      if (!refresh) {
        event();
        const auto repeat = stats();
        no_output(after, repeat, "same base cannot count as another actual output");
        Check(repeat.history_sequence == after.history_sequence && repeat.anchor_generation == after.anchor_generation
                  && repeat.reuse_count == after.reuse_count && repeat.ring_submissions == after.ring_submissions,
              "same base leaves history and Present ring unchanged");
      }
      auto pixels = read_back();
      alpha_preserved(pixels);
      const size_t center = size * (size / 2) + size / 2;
      const auto half = DirectX::PackedVector::XMConvertHalfToFloat;
      Check(std::abs(half(pixels[center][0]) - half(GAME_PIXEL[0])) > .05f, "actual backbuffer has a nonzero edit");
      if (refresh) {
        refreshed = std::move(pixels);
      } else {
        for (size_t pixel = 0; pixel < pixels.size(); ++pixel) {
          for (size_t channel = 0; channel < 3; ++channel) {
            Check(std::abs(half(pixels[pixel][channel]) - half(refreshed[pixel][channel])) < .003f,
                  "reused edit reaches the actual backbuffer with the refresh RGB");
          }
        }
      }
      swap();
    }
    game_pixel = GAME_PIXEL;

    {
      HWND other_window = CreateWindowExW(0, L"STATIC", L"NR Nexus other Present x2 buffer", WS_OVERLAPPED,
          0, 0, size, size, nullptr, nullptr, module, nullptr);
      Check(other_window != nullptr, "Present x2 alternate hidden window");
      const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> other_owner(other_window, &DestroyWindow);
      Swapchain other(gpu, other_window, size, size);
      native::OnPresent(&other.queue, &other, nullptr, nullptr, 0, nullptr);
      const auto before = stats();
      record(false, true, false, other.buffers.at(other.get_current_back_buffer_index()).Get());
      submit();
      event();
      no_output(before, stats(), "a same-queue copy to another current DXGI buffer cannot serve this swapchain");
      reopen();
      native::RetirePresentStream(&other);
    }

    upload(back(), GAME_PIXEL, PRESENT);
    submit();
    reopen();
    auto before = stats();
    record();
    Check(SUCCEEDED(gpu.list->Close()), "close the discarded producer recording");
    reopen();
    event();
    no_output(before, stats(), "a discarded producer has no Present x2 receipt");
    Check(SUCCEEDED(gpu.list->Close()), "close the empty list before discarded-frame readback");
    unchanged_pixels(read_back(), GAME_PIXEL);
    swap();

    before = stats();
    record();
    // Keep the uploads referenced by the still-open producer alive.
    native::OnPresent(&output.queue, &output, nullptr, nullptr, 0, nullptr);
    no_output(before, stats(), "an open producer cannot serve the current Present");
    submit();
    event();
    no_output(before, stats(), "submitting a declined producer cannot reopen the same base frame");
    unchanged_pixels(read_back(), GAME_PIXEL);
    swap();

    before = stats();
    record(false, false);
    submit();
    event();
    no_output(before, stats(), "an incomplete jitter pair cannot borrow Present x2 history");
    unchanged_pixels(read_back(), GAME_PIXEL);
    swap();

    before = stats();
    record(false, true, true);
    submit();
    event();
    no_output(before, stats(), "a later write in the same producer list replaces the sealed base");
    unchanged_pixels(read_back(), OVERWRITE_PIXEL);
    swap();

    before = stats();
    record();
    submit();
    reopen();
    upload(back(), OVERWRITE_PIXEL, PRESENT);
    submit();
    event();
    no_output(before, stats(), "another submitted list cannot write after the sealed producer");
    unchanged_pixels(read_back(), OVERWRITE_PIXEL);
    swap();

    warm(false);
    before = stats();
    fail_call = model_calls + 1;
    present_frame(true);
    const auto failed = stats();
    Check(failed.calls == before.calls + 1 && failed.outputs == before.outputs
              && failed.consumed_serial == before.consumed_serial && failed.reuses == before.reuses
              && failed.refreshes == before.refreshes && failed.ring_submissions == before.ring_submissions,
          "a failed model call cannot submit or consume its base frame");
    event();
    no_output(failed, stats(), "a duplicate callback cannot retry the failed frame");
    fail_call = 0;
    unchanged_pixels(read_back(), GAME_PIXEL);
    swap();

    warm(false);
    native::present_pre_fg = 1;
    xefg::Configure(&native::fg_service::Service, &Prepare, &native::fg_service::Retire,
                    &native::fg_service::Requested);
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(FakeInit);
    adapter::modules[0].real[6] = reinterpret_cast<void*>(FakeReceiptEnable);
    {
      Session owner(&gpu, 0, 17, FORMAT);
      Check(Hooks::Enable(owner.handle, 0) == 0 && !xefg::OwnsStream(), "mock XeFG is initially disabled");
      before = stats();
      record();
      submit();
      Check(Hooks::Enable(owner.handle, 1) == 0 && xefg::OwnsStream(), "public mock XeFG Enable acquires ownership");
      event();
      no_output(before, stats(), "ordinary Present stands down for an active XeFG owner");
      Check(Hooks::Enable(owner.handle, 0) == 0 && !xefg::OwnsStream(), "public mock XeFG Disable returns ownership");
      event();
      no_output(before, stats(), "ordinary Present cannot consume the base across the owner handoff");
    }
    unchanged_pixels(read_back(), GAME_PIXEL);
    swap();
    warm(false);

    // DXGI requires actual Backbuffer writes on its associated queue. Submit
    // the independent offscreen writer first, but run it after the SR copy.
    // Only the second variant delivers those pixels to the Backbuffer.
    for (const bool late_write : {false, true}) {
      const auto before_foreign = stats();
      const auto cpu_first = native::queue_submit_entries.load();
      const auto source_done = gpu.serial + 1;
      auto foreign_output = gpu.Texture(size, size, FORMAT, PRESENT);
      ComPtr<ID3D12CommandQueue> foreign;
      const D3D12_COMMAND_QUEUE_DESC queue_desc{};
      Check(SUCCEEDED(gpu.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&foreign))),
            "Present x2 second native DIRECT queue");
      ComPtr<ID3D12Fence> foreign_done;
      Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreign_done))),
            "Present x2 independent writer fence");
      ComPtr<ID3D12CommandAllocator> foreign_allocator;
      ComPtr<ID3D12GraphicsCommandList> foreign_list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&foreign_allocator)))
                && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, foreign_allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&foreign_list))),
            "Present x2 second native recording");
      auto game_list = gpu.list;
      gpu.list = foreign_list;
      upload(foreign_output.Get(), OVERWRITE_PIXEL, PRESENT);
      Check(SUCCEEDED(foreign_list->Close()), "Present x2 foreign writer list closes");
      gpu.list = game_list;
      if (!late_write) {
        record(true);
        // Draw and Dispatch share this observation path. A different native
        // list must return even when the renderer's runtime lock is held.
        const addon::RuntimeLock lock(native::runtime_mutex);
        Check(native::IsPresentSourceList(game_list.Get()) && !native::IsPresentSourceList(foreign_list.Get()),
              "only the native SR recording reaches the Present source observer");
        native::ObservePresentSourceWork(foreign_list.Get(), false);
      }
      Check(SUCCEEDED(foreign->Wait(gpu.fence.Get(), source_done)), "the foreign queue waits for the SR copy");
      ID3D12CommandList* writer[]{foreign_list.Get()};
      {
        // The actual other queue must be able to enqueue while the Present
        // queue's handoff is held and neither GPU fence can complete.
        const auto order = native::present_queue_order.load();
        Check(order != nullptr && order->queue == gpu.queue.Get(), "Present x2 observes only the associated native queue");
        const std::unique_lock hold(order->submit_mutex);
        foreign->ExecuteCommandLists(1, writer);
      }
      Check(SUCCEEDED(foreign->Signal(foreign_done.Get(), 1)) && native::queue_submit_entries.load() == cpu_first + 1
                && foreign_done->GetCompletedValue() == 0 && gpu.fence->GetCompletedValue() < source_done,
            "first CPU submit cannot complete on the GPU until the SR producer signals");
      if (late_write) {
        record(true);
      }
      Check(SUCCEEDED(gpu.list->Close()), "Present x2 game producer closes");
      ID3D12CommandList* producer[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, producer);
      Check(native::queue_submit_entries.load() == cpu_first + 2
                && SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial))
                && SUCCEEDED(gpu.queue->Wait(foreign_done.Get(), 1)),
            "second CPU submit releases the offscreen writer and orders the present after it");
      if (late_write) {
        // Keep both GPU lists, allocators and uploads alive until the queue
        // fence. Use a separate list for the legal final Backbuffer writer.
        ComPtr<ID3D12GraphicsCommandList> late_list;
        ComPtr<ID3D12CommandAllocator> late_allocator;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&late_allocator)))
                  && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, late_allocator.Get(),
                                                             nullptr, IID_PPV_ARGS(&late_list))),
              "Present x2 late writer recording on the associated queue");
        gpu.list = late_list;
        gpu.Transition(foreign_output.Get(), PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Transition(back(), PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        late_list->CopyResource(back(), foreign_output.Get());
        gpu.Transition(back(), D3D12_RESOURCE_STATE_COPY_DEST, PRESENT);
        gpu.Transition(foreign_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, PRESENT);
        submit();
        gpu.list = game_list;
      }
      event();
      const auto after_foreign = stats();
      const auto pixels = read_back();
      Check(SUCCEEDED(gpu.list->Close()), "close empty recording before offscreen readback");
      unchanged_pixels(read_back(foreign_output.Get()), OVERWRITE_PIXEL);
      const size_t replaced_pixels = std::ranges::count(pixels, OVERWRITE_PIXEL);
      std::cout << "PRESENT_X2_FOREIGN_FIRST cpu=foreign,producer gpu=SR_copy,foreign_offscreen"
                << " associated_queue_late_copy=" << late_write
                << " fences=" << gpu.fence->GetCompletedValue() << ',' << foreign_done->GetCompletedValue()
                << " calls=" << after_foreign.calls - before_foreign.calls
                << " outputs=" << after_foreign.outputs - before_foreign.outputs
                << " history=" << before_foreign.history_sequence << ',' << after_foreign.history_sequence
                << " writer_pixels=" << replaced_pixels << '/' << pixels.size()
                << " backbuffer_crc32=" << renodx::utils::hash::ComputeCRC32(
                       reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(Pixel)) << std::endl;
      if (late_write) {
        no_output(before_foreign, after_foreign, "a GPU-later Backbuffer copy cannot advance Present x2 history");
        Check(replaced_pixels == pixels.size(), "Present x2 preserves the final GPU-later writer pixels");
      } else {
        Check(after_foreign.outputs == before_foreign.outputs + 1 && after_foreign.calls == before_foreign.calls + 1
                  && after_foreign.history_sequence > before_foreign.history_sequence
                  && pixels != std::vector(size * size, GAME_PIXEL) && replaced_pixels == 0,
              "independent queue work cannot veto a proven Present x2 refresh");
        alpha_preserved(pixels);
      }
      addon::ForgetQueueCompletion(foreign.Get());
      swap();
    }
    std::cout << "PASS independent native list observer and queue submit while the Present locks are held\n";

    before = stats();
    record();
    submit();
    reopen();
    native::OnDestroySwapchain(&output, true);
    for (auto& buffer : output.buffers) {
      buffer.Reset();
    }
    size = 24;
    const HRESULT resized = output.swapchain->ResizeBuffers(static_cast<UINT>(output.buffers.size()), size, size, FORMAT, 0);
    if (FAILED(resized)) {
      std::cerr << "Present x2 ResizeBuffers HRESULT=0x" << std::hex << static_cast<uint32_t>(resized) << std::dec << '\n';
    }
    Check(SUCCEEDED(resized), "Present x2 real DXGI ResizeBuffers");
    output.BindBuffers();
    event();
    no_output(before, stats(), "resized swapchain cannot consume the old producer or edit history");
    Check(native::HookedReleaseFeatureSlot<0>(sr_handle) == NVSDK_NGX_Result_Success, "release original game SR before resize rebind");
    create_source();
    warm(false);

    model_parameter_observer = {};
    native::present_frames = 0;
    before = stats();
    record(false, false);
    submit();
    event();
    const auto every_first = stats();
    event();
    const auto every_repeat = stats();
    Check(every_first.calls == before.calls + 1 && every_repeat.calls == every_first.calls + 1
              && every_repeat.outputs == every_first.outputs + 1 && every_repeat.reuses == before.reuses,
          "Every Present still runs per present without x2 provenance or a fresh guide serial");

    Check(native::HookedReleaseFeatureSlot<0>(sr_handle) == NVSDK_NGX_Result_Success, "release resized game SR");
    native::RetirePresentStream(&output);
    Check(native::ReleasePresentPath(), "Present x2 releases completed private recordings");
    native::DrainRetiredResources(true);
    native::UnhookMixedPresentCopyHooks();
    Check(SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "Present x2 WARP device stays valid");
    if (immediate_messages != nullptr) {
      Check(SUCCEEDED(immediate_messages->UnregisterMessageCallback(message_cookie)), "Present x2 closes D3D12 validation callback");
    }
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "read the Present x2 debug-layer message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS ordinary D3D12 Present x2 WARP+MOCK: prepared/output, 1/0/1/0 pixels, duplicate, "
                 "wrong buffer, discarded producer, missing jitter, late writes, failed model, "
                 "XeFG ownership, DXGI resize and legacy Every Present\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL ordinary Present x2: " << error.what() << '\n';
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(info->GetMessage(index, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
        }
      }
    }
    return 1;
  }
}
