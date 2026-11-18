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

  Swapchain(const Gpu& gpu, HWND hwnd, uint32_t size) : window(hwnd) {
    device.native = gpu.device.Get();
    queue.device = &device;
    queue.native = gpu.queue.Get();
    queue.allow_submit = true;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = size;
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

int RunNrEditReusePresentContracts() {
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
    Gpu gpu;
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
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Present x2 native queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Present x2 native list hooks");

    uint32_t size = 16;
    HWND window = CreateWindowExW(0, L"STATIC", L"NR Nexus Present x2 fixture", WS_OVERLAPPED,
        0, 0, size, size, nullptr, nullptr, module, nullptr);
    Check(window != nullptr, "Present x2 hidden test window");
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window_owner(window, &DestroyWindow);
    Swapchain output(gpu, window, size);
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
    const auto upload = [&](ID3D12Resource* target, const auto& value, D3D12_RESOURCE_STATES resting) {
      const std::vector pixels(size * size, value);
      auto transfer = gpu.CreateTextureTransfer(target);
      gpu.Transition(target, resting, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(target, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                        pixels.size() * sizeof(value), size * sizeof(value));
      gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, resting);
    };
    const auto read_back = [&] {
      reopen();
      const auto image = stages::Capture(gpu, back(), PRESENT);
      submit();
      reopen();
      auto bytes = image.Pixels();
      std::vector<Pixel> pixels(size * size);
      Check(bytes.size() == pixels.size() * sizeof(Pixel), "Present x2 reads all FP16 backbuffer pixels");
      std::memcpy(pixels.data(), bytes.data(), bytes.size());
      return pixels;
    };

    constexpr float JITTER_X = .25f, JITTER_Y = -.25f;
    Pixel game_pixel = GAME_PIXEL;
    ComPtr<ID3D12Resource> color, depth, motion, sr_output;
    addon::bridge::OwnedParameters base;
    NVSDK_NGX_Handle* sr_handle = nullptr;
    native::ngx_slot_real[0].create = reinterpret_cast<void*>(CreateSr);
    native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(reuse::CopySr);
    native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) -> NVSDK_NGX_Result {
      return NVSDK_NGX_Result_Success;
    });
    const auto create_source = [&] {
      color = gpu.Texture(size, size, FORMAT, NATIVE_INPUT_STATE);
      motion = gpu.Texture(size, size, DXGI_FORMAT_R32G32_FLOAT, NATIVE_INPUT_STATE);
      depth = gpu.Texture(size, size, DXGI_FORMAT_R32_FLOAT, NATIVE_INPUT_STATE);
      auto desc = color->GetDesc();
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(sr_output.ReleaseAndGetAddressOf()))),
          "Present x2 SR output is a full FP16 UAV");
      upload(color.Get(), GAME_PIXEL, NATIVE_INPUT_STATE);
      upload(motion.Get(), std::array<float, 2>{0.f, 0.f}, NATIVE_INPUT_STATE);
      upload(depth.Get(), .5f, NATIVE_INPUT_STATE);
      submit();
      reopen();
      base.Set(NVSDK_NGX_Parameter_Color, color.Get());
      base.Set(NVSDK_NGX_Parameter_Output, sr_output.Get());
      base.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
      base.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
      base.Set(NVSDK_NGX_Parameter_Width, size);
      base.Set(NVSDK_NGX_Parameter_Height, size);
      base.Set(NVSDK_NGX_Parameter_OutWidth, size);
      base.Set(NVSDK_NGX_Parameter_OutHeight, size);
      base.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, size);
      base.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, size);
      base.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
      base.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      base.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      Check(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &base, &sr_handle)
                == NVSDK_NGX_Result_Success && sr_handle != nullptr, "Present x2 wrapped game SR Create");
    };
    const auto record = [&](bool reset = false, bool jitter = true, bool overwrite = false,
                            ID3D12Resource* destination = nullptr) {
      upload(color.Get(), game_pixel, NATIVE_INPUT_STATE);
      auto parameters = base;
      parameters.Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(reset));
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, JITTER_X);
      if (jitter) {
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, JITTER_Y);
      }
      Check(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr_handle, &parameters, nullptr) == NVSDK_NGX_Result_Success,
            "Present x2 wrapped game SR Evaluate succeeds");
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
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* block) {
      const auto& stream = native::present_streams.at(&output);
      const auto* captured_depth = native::GetD3D12Resource(&stream.parameters, NVSDK_NGX_Parameter_Depth);
      const auto* captured_motion = native::GetD3D12Resource(&stream.parameters, NVSDK_NGX_Parameter_MotionVectors);
      Check(stages::presets.at(model) == 2 && captured_depth != nullptr && captured_depth != depth.Get()
                && captured_motion != nullptr && captured_motion != motion.Get()
                && native::GetD3D12Resource(block, "DLSSNR.Depth") == captured_depth
                && native::GetD3D12Resource(block, "DLSSNR.MVec") != nullptr
                && native::GetFloat(block, "DLSSNR.JitterOffsetX") == JITTER_X
                && native::GetFloat(block, "DLSSNR.JitterOffsetY") == JITTER_Y
                && native::GetFloat(block, "DLSSNR.MVecScaleX") == 1.f
                && native::GetFloat(block, "DLSSNR.MVecScaleY") == 1.f,
            "Present x2 model receives actual captured guides, source jitter and full motion scale");
    };
    warm(true);
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
      Swapchain other(gpu, other_window, size);
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
