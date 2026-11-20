/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <utils/hash.hpp>
#include <unicode_input.hpp>

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, char* value, size_t* size) {
  if (present_fixture_ini.empty()) return false;
  std::wstring wide_section, wide_key;
  namespace unicode = renodx::addons::dlss5::unicode_input::internal;
  if (!unicode::Utf8ToWide(section, &wide_section) || !unicode::Utf8ToWide(key, &wide_key)) return false;
  wchar_t buffer[4096];
  const auto count = GetPrivateProfileStringW(wide_section.c_str(), wide_key.c_str(), L"", buffer,
                                               static_cast<DWORD>(std::size(buffer)), present_fixture_ini.c_str());
  std::string result;
  if (count == 0 || !unicode::WideToUtf8({buffer, count}, &result)) {
    *size = 0;
    return false;
  }
  if (value == nullptr) {
    *size = result.size() + 2;
    return true;
  }
  if (*size == 0) return false;
  const size_t copied = (std::min)(*size - 1, result.size());
  std::memcpy(value, result.data(), copied);
  value[copied] = '\0';
  *size = copied;
  return true;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, const char* value) {
  if (present_fixture_ini.empty()) return;
  std::wstring wide_section, wide_key, wide_value;
  namespace unicode = renodx::addons::dlss5::unicode_input::internal;
  Check(unicode::Utf8ToWide(section, &wide_section) && unicode::Utf8ToWide(key, &wide_key)
            && (value == nullptr || unicode::Utf8ToWide(value, &wide_value))
            && WritePrivateProfileStringW(wide_section.c_str(), wide_key.c_str(),
                                          value == nullptr ? nullptr : wide_value.c_str(), present_fixture_ini.c_str()),
        "write a setting only into the isolated Present fixture INI");
}

// Included after the native WARP/model and ReShade device/queue fixtures.
namespace nr_edit_reuse_present_cases {
namespace addon = renodx::addons::dlss5;
namespace stages = stage_execution_cases;
namespace reuse = nr_edit_reuse_gpu_cases;
namespace trace = nr_edit_reuse_xefg_cases;
constexpr auto FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto PRESENT = D3D12_RESOURCE_STATE_PRESENT;
using Pixel = std::array<uint16_t, 4>;
constexpr Pixel GAME_PIXEL{0x3400, 0x3600, 0x3800, 0x3900};
constexpr Pixel OVERWRITE_PIXEL{0x3a00, 0x3400, 0x3600, 0x3800};

struct OutputFrameToken : sl::FrameToken {
  uint32_t value;
  explicit OutputFrameToken(uint32_t frame) : value(frame) {}
  operator uint32_t() const override { return value; }
};
inline std::function<void()> output_source_evaluate;

// Only the Streamline identity is substituted. DXGI buffers and GPU work are real.
struct StreamlineSwapchainFace final : IDXGISwapChain {
  IDXGISwapChain* const base;
  ULONG references = 1;

  explicit StreamlineSwapchainFace(IDXGISwapChain* base) : base(base) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    static constexpr GUID RETRIEVE_BASE = {
        0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
    if (output == nullptr) return E_POINTER;
    if (iid == RETRIEVE_BASE) return base->QueryInterface(IID_IUnknown, output);
    *output = nullptr;
    if (iid != IID_IUnknown && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIDeviceSubObject)
        && iid != __uuidof(IDXGISwapChain)) return E_NOINTERFACE;
    *output = static_cast<IDXGISwapChain*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return base->GetContainingOutput(output); }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
};

struct Swapchain final : reshade::api::swapchain {
  PresentFaceDevice device;
  PresentFaceQueue queue;
  ComPtr<IDXGISwapChain3> swapchain;
  std::array<ComPtr<ID3D12Resource>, 2> buffers;
  HWND window = nullptr;
  IDXGISwapChain* present_face = nullptr;

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

  uint64_t get_native() const override {
    return reinterpret_cast<uintptr_t>(present_face != nullptr ? present_face : swapchain.Get());
  }
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
  static uint32_t sr_handles[2] = {1, 2};
  static size_t next_source = 0;
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(&sr_handles[next_source++ % 2]);
  return NVSDK_NGX_Result_Success;
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
  uint64_t game_observations = 0;
  uint64_t reset_epoch = 0;
  uint64_t base_claims = 0;
  uint64_t base_outputs = 0;
  bool base_complete = false;
  uint32_t calls = 0;
  uint32_t creates = 0;

  bool operator==(const Observed&) const = default;
};

struct ParkedExecute {
  static constexpr DWORD TIMEOUT_MS = 5000;
  static inline ParkedExecute* active = nullptr;
  using Event = std::unique_ptr<void, decltype(&CloseHandle)>;

  ID3D12CommandQueue* const queue;
  ID3D12CommandList* const writer;
  void* const original = native::real_queue_execute_command_lists;
  const Event entered{CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle};
  const Event release{CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle};
  DWORD release_wait = WAIT_FAILED;
  std::atomic_bool forwarded = false;

  ParkedExecute(ID3D12CommandQueue* native_queue, ID3D12CommandList* writer_list) : queue(native_queue), writer(writer_list) {
    Check(entered && release && original != nullptr, "Present x2 can park a native Execute at its trampoline");
    active = this;
    native::real_queue_execute_command_lists = reinterpret_cast<void*>(Forward);
  }

  ~ParkedExecute() {
    native::real_queue_execute_command_lists = original;
    active = nullptr;
  }

  static void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    auto& gate = *active;
    const bool parked_writer = queue == gate.queue && count == 1 && lists != nullptr && lists[0] == gate.writer;
    if (parked_writer) {
      if (!SetEvent(gate.entered.get())) return;
      gate.release_wait = WaitForSingleObject(gate.release.get(), TIMEOUT_MS);
      // A fixture timeout must never leave a GPU submission referencing resources being unwound.
      if (gate.release_wait != WAIT_OBJECT_0) return;
    }
    reinterpret_cast<native::QueueExecuteCommandListsFn>(gate.original)(queue, count, lists);
    if (parked_writer) {
      gate.forwarded.store(true);
    }
  }
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
    const bool config_fixture = project_id_path == nullptr && !diagnostic_only;
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
    native::monitor_requested = true;
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
    native::telemetry_seconds = 0;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
    if (config_fixture) {
      native::present_ui_correction = false;
      native::chroma_clamp_stops = 2.f;
      native::diffuse_white_nits = 200.f;
      native::present_white_nits = 200.f;
    }
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
      native::direct_api.evaluate = trace::EvaluatePresentEdit;
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
          .ring_submissions = native::present_ring.submitted, .game_observations = native::game_window.frames,
          .reset_epoch = native::history_reset_epoch.load(), .base_claims = stream.base_frame_clock.claims,
          .base_outputs = stream.base_frame_clock.outputs, .base_complete = stream.base_frame_clock.complete,
          .calls = model_calls, .creates = next_handle};
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
                && after.base_outputs == before.base_outputs && (after.base_claims == before.base_claims || !after.base_complete)
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
    const auto evaluate_sr = [&](bool reset, bool jitter, bool jitter_x_present = true, bool c_export = false) {
      if (!native_ngx) {
        upload(color.Get(), game_pixel, NATIVE_INPUT_STATE);
      }
      auto parameters = base;
      auto* evaluate_parameters = (native_ngx ? source_parameters : &parameters);
      evaluate_parameters->Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(reset));
      if (jitter_x_present) {
        evaluate_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter_x);
      }
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
        result = c_export
            ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), sr_handle, evaluate_parameters, nullptr)
            : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr_handle, evaluate_parameters, nullptr);
      }
      Check(NVSDK_NGX_SUCCEED(result), "Present x2 wrapped game SR Evaluate succeeds");
    };
    const auto record = [&](bool reset = false, bool jitter = true, bool overwrite = false,
                            ID3D12Resource* destination = nullptr, bool jitter_x_present = true) {
      evaluate_sr(reset, jitter, jitter_x_present);
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
    const auto warm = [&](bool require_prepared, uint32_t resize_from = 0) {
      bool prepared = false;
      bool delivered_while_settling = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto before = stats();
        present_frame();
        const auto after = stats();
        const auto pixels = read_back();
        swap();
        if (after.outputs != before.outputs) {
          const auto& work = native::stage_telemetry[static_cast<size_t>(addon::stage_plan::Stage::PRESENT)];
          Check(after.outputs == before.outputs + 1 && after.calls == before.calls + 1
                    && after.consumed_serial > before.consumed_serial,
                "first accepted Present records one model call and consumes its own guide serial");
          if (resize_from != 0 && work.work_width == resize_from && work.work_height == resize_from) {
            Check(after.refreshes == before.refreshes && after.reuses == before.reuses && after.anchor_generation == 0,
                  "resize settle delivers per-frame NR without a low-grid x2 anchor");
            delivered_while_settling = true;
            continue;
          }
          Check(work.work_width == size && work.work_height == size
                    && after.refreshes == before.refreshes + 1 && after.reuses == before.reuses,
                "first accepted full-grid Present after a cold start or handoff refreshes");
          Check(resize_from == 0 || delivered_while_settling, "resize exercises the old committed grid before refreshing");
          Check(!require_prepared || prepared, "cold start observes a real prepared-only submission");
          Check(native::present_streams.at(&output).edit_reuse_monitor.reason == addon::PresentEditReuseRejection::NONE,
                "the verified delivery replaces the stream's early rejection");
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
    std::array<trace::PresentTraceFrame, 4> baseline;
    for (const auto variant : {trace::PresentTrace::BASELINE, trace::PresentTrace::GENERATED_OUTPUT,
                               trace::PresentTrace::REPEATED_CONSUMPTION}) {
      const auto& source = native::features.at(sr_handle);
      const auto sequence_prefix = source.sr_sequence;
      const auto source_generation = source.source_generation;
      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      const auto claims_prefix = clock.claims;
      const auto outputs_prefix = clock.outputs;
      const auto attempts_prefix = stream.base_frame_attempt;
      const auto prefix = stats();
      uint64_t first_anchor_generation = 0;
      uint64_t anchor_sequence = 0;
      uint32_t downstream_outputs = 0;
      uint32_t repeated_callbacks = 0;
      std::vector<Pixel> refreshed;
      for (uint32_t index = 0; index < baseline.size(); ++index) {
        const bool refresh = index % 2 == 0;
        game_pixel = trace::PRESENT_SOURCE_PIXELS[index];
        const auto before = stats();
        present_frame(index == 0, index == 2);
        const auto after = stats();
        const auto calls = after.calls - before.calls;
        const auto& delivery = source.edit_reuse_monitor[0];
        const auto& slot = native::features.at(stream.Handle()).slots[0];
        Check(source.source_generation == source_generation && delivery.source_generation == source_generation
                  && delivery.stage == addon::stage_plan::Stage::PRESENT && delivery.sequence == source.sr_sequence
                  && delivery.surface == renodx::utils::directx::NativeIdentity(back()) && slot.nr_preset == 2
                  && !slot.look_history.valid && slot.edit_reuse != nullptr && slot.edit_reuse->valid,
              "ordinary Present uses the same actual SR source, stage, model and current Backbuffer for the group");
        Check(clock.claims == claims_prefix + index + 1 && clock.outputs == outputs_prefix + index + 1
                  && clock.frame.source == reinterpret_cast<uintptr_t>(sr_handle)
                  && clock.frame.generation == source_generation && clock.frame.sequence == source.sr_sequence
                  && clock.frame.output_epoch == stream.base_frame_epoch && clock.frame.pass_begin == 0
                  && clock.frame.pass_count == 1 && clock.frame.roles == 1 && clock.frame.reuse_edits,
              "ordinary Present recognizes one actual successful SR output and commits one BB product per base frame");
        Check(after.outputs == before.outputs + 1 && after.consumed_serial > before.consumed_serial
                  && after.refreshes == before.refreshes + refresh && after.reuses == before.reuses + !refresh
                  && after.game_observations == prefix.game_observations + index + 1,
              "ordinary Present submits one output and observes one game frame per real base");
        if (index == 0) first_anchor_generation = after.anchor_generation;
        if (after.anchor_generation != before.anchor_generation) anchor_sequence = after.history_sequence;
        Check(after.anchor_generation != 0 && anchor_sequence != 0, "ordinary Present has a recorded anchor source");
        trace::PresentTraceFrame observed{.evaluates = calls, .reset = (calls != 0 ? model_resets.back() : -1),
            .sequence = clock.frame.sequence - sequence_prefix, .history_sequence = after.history_sequence - sequence_prefix,
            .anchor_sequence = anchor_sequence - sequence_prefix,
            .anchor_generation = after.anchor_generation - first_anchor_generation + 1,
            .anchor_age = after.history_sequence - anchor_sequence, .reuses = after.reuses - before.reuses,
            .complete = clock.complete && delivery.handed_off
                && delivery.delivery == (refresh ? native::EditReuseDelivery::REFRESH : native::EditReuseDelivery::REUSED)};
        if (variant == trace::PresentTrace::REPEATED_CONSUMPTION && index == 1) {
          const auto callback = stream.base_frame_attempt;
          const auto frame_clock = trace::PresentClockState(clock);
          event();
          ++repeated_callbacks;
          const auto repeated = stats();
          Check(repeated.calls == after.calls,
                ("duplicate OnPresent added " + std::to_string(repeated.calls - after.calls) + " model Evaluate call(s)").c_str());
          Check(stream.base_frame_attempt == callback + 1 && repeated == after && trace::PresentClockState(clock) == frame_clock,
                "duplicate OnPresent arrived on the unchanged ordered BB and changed neither history nor submission");
        }
        auto pixels = read_back();
        observed.rgba.resize(pixels.size() * sizeof(Pixel));
        std::memcpy(observed.rgba.data(), pixels.data(), observed.rgba.size());
        const auto half = DirectX::PackedVector::XMConvertHalfToFloat;
        const size_t center = size * (size / 2) + size / 2;
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
        if (variant == trace::PresentTrace::GENERATED_OUTPUT && index == 0) {
          const auto frame_clock = trace::PresentClockState(clock);
          Check(trace::ConsumeGeneratedOutput(&gpu, back()) == observed.rgba,
                "downstream stand-in consumes the separate generated surface in queue order");
          ++downstream_outputs;
          Check(stats() == after && trace::PresentClockState(clock) == frame_clock,
                "extra downstream consumption cannot advance ordinary Present history");
        }
        swap();
        ++downstream_outputs;
        if (variant == trace::PresentTrace::BASELINE) {
          trace::CheckPresentTraceFrame(observed, index, "ordinary");
          baseline[index] = std::move(observed);
        } else {
          Check(observed == baseline[index], "ordinary G/R base trace must equal A including complete ordered RGBA");
        }
      }
      const bool generated = variant == trace::PresentTrace::GENERATED_OUTPUT;
      const bool repeated = variant == trace::PresentTrace::REPEATED_CONSUMPTION;
      Check(downstream_outputs == 4 + generated && repeated_callbacks == static_cast<uint32_t>(repeated)
                && stream.base_frame_attempt == attempts_prefix + 4 + repeated
                && native::features.at(sr_handle).sr_sequence == sequence_prefix + 4,
            "ordinary G adds one downstream output and R one OnPresent on the same four actual SR calls");
      std::cout << "PRESENT_POLICY_CONSUMERS route=ordinary generated=" << generated << " repeated=" << repeated
                << " application_callbacks=" << stream.base_frame_attempt - attempts_prefix
                << " downstream_outputs=" << downstream_outputs << '\n';
    }
    game_pixel = GAME_PIXEL;

    present_frame(true);
    const auto original_source = native::present_streams.at(&output).base_frame_clock.frame;
    const auto original_handle = sr_handle;
    auto original_sr_output = sr_output;
    const auto original_pixels = read_back();
    swap();
    Check(native::HookedReleaseFeatureSlot<0>(sr_handle) == NVSDK_NGX_Result_Success,
          "release the old SR source before a genuine replacement Create");
    create_source();
    const auto replacement_prefix = stats();
    game_pixel = OVERWRITE_PIXEL;
    present_frame();
    const auto& replacement = native::present_streams.at(&output).base_frame_clock;
    Check(sr_handle != original_handle && replacement.frame.source == reinterpret_cast<uintptr_t>(sr_handle)
              && replacement.frame.source != original_source.source && sr_output.Get() != original_sr_output.Get()
              && replacement.frame.generation != original_source.generation && replacement.frame.sequence == 1
              && replacement.complete && replacement.outputs == replacement_prefix.base_outputs + 1
              && model_calls == replacement_prefix.calls + 1 && model_resets.back() == 1,
          "real replacement SR source starts at its own first frame with independent model history and Reset");
    const auto replacement_pixels = read_back();
    alpha_preserved(replacement_pixels);
    Check(replacement_pixels != original_pixels, "replacement source delivers its own current RGBA");
    original_sr_output.Reset();
    swap();
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
    const auto before_discard_recovery = stats();
    present_frame();
    Check(stats().base_outputs == before_discard_recovery.base_outputs + 1 && model_calls == before_discard_recovery.calls + 1
              && model_resets.back() == 1,
          "the next ordered SR producer recovers from discarded base delivery with Reset");
    reopen();
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
    const auto before_order_recovery = stats();
    present_frame();
    Check(stats().base_outputs == before_order_recovery.base_outputs + 1 && model_calls == before_order_recovery.calls + 1
              && model_resets.back() == 1,
          "the next ordered SR producer recovers from an unsubmitted base with Reset");
    reopen();
    swap();

    for (const bool jitter_x_present : {true, false}) {
      before = stats();
      record(false, !jitter_x_present, false, nullptr, jitter_x_present);
      submit();
      event();
      const auto after = stats();
      no_output(before, after, "an incomplete jitter pair cannot borrow Present x2 history");
      const auto captured = std::ranges::find_if(native::present_guide_sets, [&](const auto& set) {
        return set.source_handle == sr_handle && set.base_frame.sequence == native::features.at(sr_handle).sr_sequence;
      });
      Check(captured != std::end(native::present_guide_sets) && captured->base_frame.sealed
                && captured->base_frame.backbuffer == renodx::utils::directx::NativeIdentity(back())
                && !captured->contract.has_jitter && after.base_claims == before.base_claims,
            "strict x2 preserves the proven source but cannot claim it with an incomplete jitter pair");
      unchanged_pixels(read_back(), GAME_PIXEL);
      swap();
      std::cout << "PRESENT_SOURCE_JITTER strict=1 jitter_x=" << jitter_x_present
                << " jitter_y=" << !jitter_x_present << " source_retained=1 claims=0 calls=0 outputs=0\n";
    }
    const auto before_jitter_recovery = stats();
    present_frame();
    const auto jitter_recovery = stats();
    Check(jitter_recovery.calls == before_jitter_recovery.calls + 1 && model_resets.back() == 1
              && jitter_recovery.base_claims == before_jitter_recovery.base_claims + 1
              && jitter_recovery.base_outputs == before_jitter_recovery.base_outputs + 1 && jitter_recovery.base_complete
              && jitter_recovery.refreshes == before_jitter_recovery.refreshes + 1
              && jitter_recovery.reuses == before_jitter_recovery.reuses,
          "the next complete jitter pair refreshes the same source with Reset after strict rejection");
    alpha_preserved(read_back());
    swap();
    std::cout << "PRESENT_SOURCE_JITTER recovery=1 calls=1 reset=1 outputs=1\n";

    before = stats();
    record(false, true, true);
    submit();
    event();
    no_output(before, stats(), "a later write in the same producer list replaces the sealed base");
    const auto& rejected = native::present_streams.at(&output).edit_reuse_monitor;
    Check(rejected.reason == addon::PresentEditReuseRejection::SOURCE_UNCONFIRMED
              && rejected.backbuffer == renodx::utils::directx::NativeIdentity(back()),
          "the late write publishes a source rejection for the current backbuffer");
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
    const uint32_t resize_from = size;
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
    warm(false, resize_from);

    bool preserves_cross_epoch_writer = false;
    {
      native::enabled = false;
      event();
      const auto epoch_off = native::present_streams.at(&output).base_frame_epoch;
      {
        // Retire previous Present recordings so the edit-ordering mutex doesn't serialize these two game calls.
        const addon::RuntimeLock lock(native::runtime_mutex);
        Check(native::ReleasePresentRing(), "Present x2 cross-epoch test retires the completed private ring");
        native::PruneCompletedSubmissionUses();
        Check(addon::control_mask_execution::ReleaseIfIdle(), "Present x2 cross-epoch test starts without edit-ordering records");
      }
      ComPtr<ID3D12CommandAllocator> writer_allocator;
      ComPtr<ID3D12GraphicsCommandList> writer;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&writer_allocator)))
                && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, writer_allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&writer))),
            "Present x2 cross-epoch writer uses a separate native list");
      const auto game_list = gpu.list;
      auto* const current_buffer = back();
      gpu.list = writer;
      const auto produced = stages::Capture(gpu, current_buffer, PRESENT);
      upload(current_buffer, OVERWRITE_PIXEL, PRESENT);
      const auto written = stages::Capture(gpu, current_buffer, PRESENT);
      // The producer's fence cleanup runs while this writer is still parked.
      const auto writer_uploads = std::exchange(gpu.retained_resources, {});
      gpu.list = game_list;
      Check(SUCCEEDED(writer->Close()), "Present x2 cross-epoch writer closes");

      const auto cpu_first = native::queue_submit_entries.load();
      constexpr Pixel PRODUCER_PIXEL{0x3600, 0x3a00, 0x3400, 0x3b00};
      uint64_t epoch_enabled = 0;
      uint64_t producer_fence = 0;
      {
        ParkedExecute gate(gpu.queue.Get(), writer.Get());
        std::jthread old_submit([&] {
          ID3D12CommandList* lists[]{writer.Get()};
          native::HookedQueueExecuteCommandLists(gpu.queue.Get(), 1, lists);
        });
        Check(WaitForSingleObject(gate.entered.get(), ParkedExecute::TIMEOUT_MS) == WAIT_OBJECT_0
                  && native::queue_submit_entries.load() == cpu_first + 1,
              "Present x2 old Execute reaches the native trampoline before enabling observation");
        native::enabled = true;
        event();
        epoch_enabled = native::present_streams.at(&output).base_frame_epoch;
        Check(back() == current_buffer, "Present x2 producer and old writer target the same current DXGI buffer");
        game_pixel = PRODUCER_PIXEL;
        record(true);
        submit();
        game_pixel = GAME_PIXEL;
        producer_fence = gpu.serial;
        Check(native::queue_submit_entries.load() == cpu_first + 2
                  && gpu.fence->GetCompletedValue() == producer_fence && !gate.forwarded.load(),
              "Present x2 producer executes and completes while the older hook call is parked");
        Check(SetEvent(gate.release.get()), "Present x2 releases the older hook call after the producer fence");
        old_submit.join();
        Check(gate.release_wait == WAIT_OBJECT_0 && gate.forwarded.load(), "Present x2 older Execute really forwards");
      }
      finish();
      const auto writer_fence = gpu.fence->GetCompletedValue();
      const auto all_pixels = [&](const stages::Snapshot& snapshot, Pixel expected) {
        const auto bytes = snapshot.Pixels();
        const std::vector pixels(size * size, expected);
        return bytes.size() == pixels.size() * sizeof(Pixel)
            && std::memcmp(bytes.data(), pixels.data(), bytes.size()) == 0;
      };
      Check(all_pixels(produced, PRODUCER_PIXEL), "Present x2 old list reads the new SR pixels immediately before its overwrite");
      Check(writer_fence > producer_fence && all_pixels(written, OVERWRITE_PIXEL),
            "Present x2 late native Execute writes every expected FP16 pixel before OnPresent");

      const auto before_present = stats();
      event();
      const auto after_present = stats();
      const auto pixels = read_back();
      const size_t preserved = std::ranges::count(pixels, OVERWRITE_PIXEL);
      preserves_cross_epoch_writer = preserved == pixels.size() && after_present.calls == before_present.calls
          && after_present.outputs == before_present.outputs && after_present.ring_submissions == before_present.ring_submissions
          && after_present.consumed_serial == before_present.consumed_serial && after_present.refreshes == before_present.refreshes
          && after_present.reuses == before_present.reuses && after_present.history_sequence == before_present.history_sequence;
      std::cout << "PRESENT_X2_CROSS_EPOCH cpu=old_hook,enable,producer,old_forward gpu=producer,writer"
                << " epoch=" << epoch_off << ',' << epoch_enabled << " fences=" << producer_fence << ',' << writer_fence
                << " calls=" << after_present.calls - before_present.calls
                << " outputs=" << after_present.outputs - before_present.outputs
                << " history=" << before_present.history_sequence << ',' << after_present.history_sequence
                << " producer_pixels=" << size * size << '/' << size * size
                << " writer_pixels_before=" << size * size << '/' << size * size
                << " writer_pixels_after=" << preserved << '/' << pixels.size()
                << " preserved=" << preserves_cross_epoch_writer << std::endl;
      swap();
    }

    bool reset_observer_bypassed = false;
    {
      evaluate_sr(false, true);
      const auto source_recording = addon::submission::RecordingIdentity(gpu.list.Get());
      Check(native::IsPresentSourceList(gpu.list.Get()), "Present x2 successful SR arms its list without a final Backbuffer copy");
      Check(SUCCEEDED(gpu.list->Close()), "close the source-only command recording before native Reset");
      reopen();
      const auto recycled_recording = addon::submission::RecordingIdentity(gpu.list.Get());
      Check(recycled_recording.first == source_recording.first && recycled_recording.second != source_recording.second,
            "Present x2 native Reset starts an unrelated recording on the very same list");

      // Exercise the observer shared by the native Draw and Dispatch detours. A bounded wait
      // under the lock lets a regressed observer finish after release and report its contention.
      const std::unique_ptr<void, decltype(&CloseHandle)> started(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
      Check(started != nullptr, "Present x2 source observer test start event");
      const auto lock_before = addon::runtime_lock_contended.load();
      constexpr DWORD OBSERVER_TIMEOUT_MS = 1000;
      DWORD observer_wait = WAIT_FAILED;
      std::jthread observer;
      {
        const addon::RuntimeLock hold(native::runtime_mutex);
        observer = std::jthread([&, list = gpu.list.Get()] {
          SetEvent(started.get());
          native::ObservePresentSourceWork(list, false);
        });
        Check(WaitForSingleObject(started.get(), OBSERVER_TIMEOUT_MS) == WAIT_OBJECT_0,
              "Present x2 recycled-list observer worker starts");
        observer_wait = WaitForSingleObject(observer.native_handle(), OBSERVER_TIMEOUT_MS);
      }
      observer.join();
      const auto contentions = addon::runtime_lock_contended.load() - lock_before;
      reset_observer_bypassed = observer_wait == WAIT_OBJECT_0 && contentions == 0;
      std::cout << "PRESENT_X2_RESET_OBSERVER recording=" << source_recording.first << ':' << source_recording.second
                << ',' << recycled_recording.first << ':' << recycled_recording.second
                << " lock_wait_result=" << observer_wait << " deadline_ms=" << OBSERVER_TIMEOUT_MS
                << " runtime_lock_contentions=" << contentions << " bypassed=" << reset_observer_bypassed << std::endl;
      Check(SUCCEEDED(gpu.list->Close()), "close unrelated post-Reset recording before the next base");
      finish();
      reopen();
    }

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

    const auto config_file = native::ReShadeBaseDirectory() / L"ReShade.ini";
    const HANDLE created = CreateFileW(config_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(created != INVALID_HANDLE_VALUE, "create the fixture INI in the runner output without replacing an existing file");
    const auto clean_config = [](const std::filesystem::path* file) {
      present_fixture_ini.clear();
      std::error_code ignored;
      std::filesystem::remove(*file, ignored);
    };
    const std::unique_ptr<const std::filesystem::path, decltype(clean_config)> config_cleanup(&config_file, clean_config);
    CloseHandle(created);
    present_fixture_ini = config_file;
    const auto load_present_mode = [&](const char* present_frames, bool reuse_requested = true) {
      std::ofstream ini(present_fixture_ini, std::ios::trunc);
      ini << '[' << native::kConfigSection << "]\nConfigVersion=" << native::kConfigVersion
          << "\nNeuralUplift=1\nEnableHooks=2\nNREditReuse=" << reuse_requested << "\nNRHookPoint=" << native::kHookPresent
          << "\nNRPasses=1\nNRPreset=2\nNRPresentPreFg=0\nNRPresentGuides=" << native::kPresentGuidesRequired
          << "\nNRPresentBlockHudlessConfigVersion=1\nNRPresentBlockHudless=1\nNRPresentUICorrection=0"
             "\nNRCodecMode=0\nNRFeedMode=1\nNRNormGovernor=0\nNRLookMode=0\nNRFollowInputRes=0"
             "\nNRAdaptiveEnabled=0\nNRResolutionScale=1\nNRDetailStability=1\nNRControlMaskEnabled=0"
             "\nNRAutoMask=0\nNRCostMeter=0\nNRGpuTimers=0\nNRTransferStrength=1\nNRColorStrength=1\nNRIntensity=1"
             "\nNRChromaClamp=2\nNRDiffuseWhiteNits=200\nNRPresentWhiteNits=200\n";
      if (present_frames != nullptr) {
        ini << "NRPresentFrames=" << present_frames << '\n';
      }
      ini.close();
      Check(ini.good(), "write the old INI before loading production settings");
      const auto saved = native::ReadPresentRestoreRaw("NRPresentFrames");
      Check(present_frames == nullptr ? !saved.has_value() : saved == present_frames,
            "file-backed ReShade config distinguishes missing and explicit NRPresentFrames");
      native::LoadConfiguration();
      const uint32_t expected = (present_frames != nullptr && present_frames[0] == '1' ? 1u : 0u);
      Check(native::enabled && native::edit_reuse_enabled == reuse_requested && native::hook_point == native::kHookPresent
                && native::stack_passes == 1 && native::present_frames == expected,
            "production INI loader keeps NR/x2 requested and resolves the selected legacy Present mode");
    };
    reopen();
    for (const char* value : {static_cast<const char*>(nullptr), "0"}) {
      load_present_mode(value);
      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      const auto& source = native::features.at(sr_handle);
      const auto source_sequence = source.sr_sequence;
      const auto claims = clock.claims;
      const auto before_proven = stats();
      present_frame();
      const auto proven = stats();
      Check(clock.claims == claims + 1 && clock.frame.sequence == source_sequence + 1
                && clock.frame.generation == source.source_generation
                && clock.frame.source == reinterpret_cast<uintptr_t>(sr_handle)
                && !clock.frame.reuse_edits && native::present_frames == 0 && !native::WantsPresentBaseFrames()
                && clock.complete && proven.base_outputs == before_proven.base_outputs + 1
                && proven.calls == before_proven.calls + 1 && proven.outputs == before_proven.outputs + 1
                && proven.refreshes == before_proven.refreshes && proven.reuses == before_proven.reuses,
            "missing and explicit zero INIs recognize a proven source while retaining Every present and disabling old x2");
      event();
      const auto repeated = stats();
      Check(clock.claims == claims + 1 && repeated.calls == proven.calls + 1 && repeated.outputs == proven.outputs + 1
                && repeated.base_outputs == proven.base_outputs
                && repeated.refreshes == proven.refreshes && repeated.reuses == proven.reuses,
            "Every present repeats legacy execution without counting a new base or running old x2");
      reopen();
      swap();
    }

    {
      load_present_mode("1", false);
      const auto before = stats();
      present_frame();
      const auto first = stats();
      const auto& clock = native::present_streams.at(&output).base_frame_clock;
      Check(native::present_frames == 1 && !native::edit_reuse_enabled && !native::WantsPresentBaseFrames()
                && first.calls == before.calls + 1 && first.outputs == before.outputs + 1
                && first.base_claims == before.base_claims + 1 && first.base_complete && !clock.frame.reuse_edits
                && first.refreshes == before.refreshes && first.reuses == before.reuses,
            "explicit Game frames processes an ordered source once without implicitly enabling x2");
      event();
      const auto duplicate = stats();
      Check(duplicate.calls == first.calls && duplicate.outputs == first.outputs
                && duplicate.base_claims == first.base_claims && duplicate.base_outputs == first.base_outputs,
            "Game frames retains its completed output on a duplicate without another NR or base-history claim");
      reopen();
      swap();
      present_frame();
      const auto next = stats();
      Check(next.calls == first.calls + 1 && next.outputs == first.outputs + 1
                && next.base_claims == first.base_claims + 1 && next.base_complete,
            "the next real ordered source still receives ordinary NR in Game frames mode");
      load_present_mode("0", false);
      event();
      const auto compatible = stats();
      Check(native::present_frames == 0 && !native::edit_reuse_enabled
                && compatible.calls == next.calls + 1 && compatible.outputs == next.outputs + 1
                && compatible.base_claims == next.base_claims && compatible.base_outputs == next.base_outputs
                && compatible.refreshes == before.refreshes && compatible.reuses == before.reuses,
            "explicit zero restores every-present compatibility without pretending a repeat is a new base frame");
      std::cout << "PRESENT_POLICY_COMPAT game_frames_calls=1/0/1 every_present_repeat_calls=1"
                   " new_base_claim_on_repeat=0 x2=0 evidence=WARP+MOCK\n";
      reopen();
      swap();
      load_present_mode("0");
    }

    {
      // Both SR views are successful on the same queue, but only A supplies the BB.
      const auto view_a = std::tuple{color, motion, depth, sr_output, base, sr_handle};
      create_source();
      auto [view_b_color, view_b_motion, view_b_depth, view_b_output, view_b_parameters, view_b_handle] =
          std::tuple{color, motion, depth, sr_output, base, sr_handle};
      std::tie(color, motion, depth, sr_output, base, sr_handle) = view_a;
      constexpr std::array<float, 2> motion_a{0.f, 0.f};
      constexpr std::array<float, 2> motion_b{2.f, -3.f};
      constexpr float depth_a = .5f;
      constexpr float depth_b = .75f;
      upload(view_b_motion.Get(), motion_b, NATIVE_INPUT_STATE);
      upload(view_b_depth.Get(), depth_b, NATIVE_INPUT_STATE);
      view_b_parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter_x);
      view_b_parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter_y);

      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      struct SourceCase {
        bool proven = true;
        bool jitter_x = true;
        bool jitter_y = true;
        const char* present_frames = "0";
        bool depth_mask = false;
        bool motion_mask = false;
      };
      for (const auto& scenario : std::array{SourceCase{}, SourceCase{.jitter_y = false}, SourceCase{},
               SourceCase{.jitter_x = false, .present_frames = nullptr}, SourceCase{},
               SourceCase{.proven = false}, SourceCase{},
               SourceCase{.jitter_y = false, .depth_mask = true},
               SourceCase{.proven = false, .depth_mask = true}, SourceCase{.depth_mask = true},
               SourceCase{.proven = false, .depth_mask = true, .motion_mask = true}, SourceCase{}}) {
        load_present_mode(scenario.present_frames);
        if (scenario.depth_mask) {
          native::control_mask_enabled = true;
          native::control_mask_mode = native::kControlMaskDepthFar;
          native::control_mask_red = .8f;
          native::control_mask_green = .6f;
          native::control_mask_blue = .4f;
          // Both views' depth values lie past this far endpoint, with either
          // depth direction. B must visibly change R if incorrectly admitted.
          native::control_mask_depth_near = 0.f;
          native::control_mask_depth_far = .2f;
          native::control_mask_motion_enabled = scenario.motion_mask;
          native::control_mask_motion_start = 0.f;
          native::control_mask_motion_end = .01f;
        }
        const bool proven = scenario.proven;
        const auto before = stats();
        const auto previous = clock.frame;
        if (proven) {
          record(false, scenario.jitter_y, false, nullptr, scenario.jitter_x);
        } else {
          evaluate_sr(false, true);
          upload(back(), OVERWRITE_PIXEL, PRESENT);
        }
        Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), view_b_handle,
                  &view_b_parameters, nullptr)),
              "second SR view produces its own guides via the wrapped Evaluate");
        submit();
        const auto latest_capture = [](const NVSDK_NGX_Handle* source) {
          const native::PresentGuideSet* guide = nullptr;
          for (const auto& candidate : native::present_guide_sets) {
            if (candidate.source_handle == source && (guide == nullptr || candidate.serial > guide->serial)) {
              guide = &candidate;
            }
          }
          return guide;
        };
        const auto* capture_a = latest_capture(sr_handle);
        const auto* capture_b = latest_capture(view_b_handle);
        Check(capture_a != nullptr && capture_b != nullptr && capture_a->serial > 0 && capture_b->serial > capture_a->serial
                  && capture_a->device_identity == capture_b->device_identity
                  && capture_a->output_width == capture_b->output_width && capture_a->output_height == capture_b->output_height
                  && capture_b->base_frame.backbuffer == nullptr,
              "newer compatible view B has no SR-to-BB copy and cannot identify the current A picture");

        stages::Snapshot model_motion, model_depth, model_mask;
        const auto* expected = (proven ? capture_a : capture_b);
        bool expected_inputs = false;
        model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
          auto* const mv = native::GetD3D12Resource(block, "DLSSNR.MVec");
          auto* const z = native::GetD3D12Resource(block, "DLSSNR.Depth");
          expected_inputs = mv == expected->motion && z == expected->depth;
          if (mv != nullptr && z != nullptr) {
            model_motion = stages::Capture(gpu, mv, NATIVE_INPUT_STATE);
            model_depth = stages::Capture(gpu, z, NATIVE_INPUT_STATE);
          }
          if (auto* mask = native::GetD3D12Resource(block, "DLSSNR.ControlMask")) {
            model_mask = stages::Capture(gpu, mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          }
        };
        event();
        model_parameter_observer = {};
        const auto after = stats();
        const auto all_pixels = [](const stages::Snapshot& snapshot, const auto& pixel) {
          if (snapshot.readback == nullptr || snapshot.pixel_bytes != sizeof(pixel)) return false;
          const auto pixels = snapshot.Pixels();
          for (size_t offset = 0; offset < pixels.size(); offset += sizeof(pixel)) {
            if (std::memcmp(pixels.data() + offset, &pixel, sizeof(pixel)) != 0) return false;
          }
          return !pixels.empty();
        };
        const bool motion_matches = all_pixels(model_motion, proven ? motion_a : motion_b);
        const bool depth_matches = all_pixels(model_depth, proven ? depth_a : depth_b);
        std::cout << "PRESENT_SOURCE_VIEWS proven=" << proven << " serial_a=" << capture_a->serial
                  << " serial_b=" << capture_b->serial << " consumed=" << after.consumed_serial
                  << " jitter_x=" << scenario.jitter_x << " jitter_y=" << scenario.jitter_y
                  << " ini_missing=" << (scenario.present_frames == nullptr)
                  << " motion_match=" << motion_matches << " depth_match=" << depth_matches
                  << " claims=" << after.base_claims - before.base_claims
                  << " base_outputs=" << after.base_outputs - before.base_outputs << std::endl;
        Check(after.calls == before.calls + 1 && after.outputs == before.outputs + 1
                  && after.ring_submissions == before.ring_submissions + 1 && native::PresentRingComplete()
                  && after.refreshes == before.refreshes && after.reuses == before.reuses
                  && native::present_frames.load() == 0 && native::edit_reuse_enabled.load(),
              "two-view Every present records one model call and completes one real GPU submission");
        Check(expected_inputs && motion_matches && depth_matches && after.consumed_serial == expected->serial,
              "two-view Every present model must read MV and depth from the SR view that supplies its proven BB");
        if (scenario.depth_mask) {
          Check(model_mask.readback != nullptr && model_mask.format == DXGI_FORMAT_R8G8B8A8_UNORM,
                "ordinary Present binds the generated Control Mask to the actual model call");
          const std::array<uint8_t, 4> expected_mask{
              static_cast<uint8_t>((proven || scenario.motion_mask) ? 0 : 204), 153, 102, 255};
          const auto pixels = model_mask.Pixels();
          std::cout << "PRESENT_DEPTH_MASK proven=" << proven << " motion=" << scenario.motion_mask
                    << " consumed=" << after.consumed_serial << " rgba="
                    << static_cast<uint32_t>(pixels[0]) << ',' << static_cast<uint32_t>(pixels[1]) << ','
                    << static_cast<uint32_t>(pixels[2]) << ',' << static_cast<uint32_t>(pixels[3])
                    << " expected_r=" << static_cast<uint32_t>(expected_mask[0]) << std::endl;
          Check(all_pixels(model_mask, expected_mask),
                "only proven current depth may attenuate mask R; compatibility keeps manual RGB and existing motion");
        }
        if (proven) {
          const auto& source = native::features.at(sr_handle);
          Check(after.base_claims == before.base_claims + 1 && after.base_outputs == before.base_outputs + 1
                    && capture_a->base_frame.sealed
                    && after.base_complete && !stream.base_history_unbound && !clock.frame.reuse_edits
                    && clock.frame.source == reinterpret_cast<uintptr_t>(sr_handle)
                    && clock.frame.generation == source.source_generation && clock.frame.sequence == source.sr_sequence,
                "the actual A group owns the complete Every present product");
          if (!before.base_complete) {
            Check(model_resets.back() == 1, "the next proven A product resets after unbound Every present execution");
          }
        } else {
          Check(after.base_claims == before.base_claims && after.base_outputs == before.base_outputs
                    && !capture_a->base_frame.sealed && !after.base_complete
                    && stream.base_history_unbound && clock.frame.sequence == previous.sequence,
                "legacy Every present can use B guides without claiming the unproven BB as a base frame");
        }
        reopen();
        swap();
      }
      Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(view_b_handle)), "release the second wrapped SR view");
    }


    for (const uint32_t mask_case : {0u, 1u, 2u}) {
      const bool depth_mask = mask_case != 0;
      const bool motion_mask = mask_case == 2;
      if (depth_mask) {
        native::control_mask_enabled = true;
        native::control_mask_mode = native::kControlMaskDepthFar;
        native::control_mask_red = .8f;
        native::control_mask_green = .6f;
        native::control_mask_blue = .4f;
        native::control_mask_depth_near = 0.f;
        native::control_mask_depth_far = .2f;
        native::control_mask_motion_enabled = motion_mask;
        native::control_mask_motion_start = 0.f;
        native::control_mask_motion_end = .01f;
        if (motion_mask) {
          upload(motion.Get(), std::array<float, 2>{2.f, -3.f}, NATIVE_INPUT_STATE);
        }
        present_frame();
        reopen();
        swap();
      }
      stages::Snapshot raced_mask, recovered_mask;
      ComPtr<ID3D12CommandAllocator> writer_allocator;
      ComPtr<ID3D12GraphicsCommandList> writer;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&writer_allocator)))
                && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, writer_allocator.Get(),
                                                           nullptr, IID_PPV_ARGS(&writer))),
            "Every present race records a second game thread's Backbuffer writer in advance");
      auto* const current_buffer = back();
      const auto game_list = gpu.list;
      gpu.list = writer;
      const auto produced = stages::Capture(gpu, current_buffer, PRESENT);
      upload(current_buffer, OVERWRITE_PIXEL, PRESENT);
      const auto written = stages::Capture(gpu, current_buffer, PRESENT);
      const auto writer_uploads = std::exchange(gpu.retained_resources, {});
      gpu.list = game_list;
      Check(SUCCEEDED(writer->Close()), "Every present race closes its independent BB writer before the SR producer");

      record();
      submit();
      const auto before_race = stats();
      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      const auto previous_frame = clock.frame;
      const auto source_sequence = native::features.at(sr_handle).sr_sequence;
      const auto order = native::present_queue_order.load();
      Check(order != nullptr && order->queue == gpu.queue.Get() && native::present_frames == 0,
            "Every present race uses the actual SR producer's associated presentation queue");
      const auto producer_entry = order->entered.load();
      const auto submit_entries = native::queue_submit_entries.load();
      uint32_t observed_gates = 0;
      {
        ParkedExecute gate(gpu.queue.Get(), writer.Get());
        const auto clear_observer = [](int*) { model_parameter_observer = {}; };
        int observer_key = 0;
        const std::unique_ptr<int, decltype(clear_observer)> observer_cleanup(&observer_key, clear_observer);
        model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* parameters) {
          if (depth_mask) {
            auto* mask = native::GetD3D12Resource(parameters, "DLSSNR.ControlMask");
            Check(mask != nullptr, "late writer reaches a real mask-bound model call");
            raced_mask = stages::Capture(gpu, mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          }
          Check(observed_gates++ == 0 && model == native::features.at(stream.Handle()).slots[0].handle
                    && clock.claims == before_race.base_claims && clock.outputs == before_race.base_outputs
                    && !clock.complete && clock.frame.sequence == previous_frame.sequence
                    && stream.nr_presents == before_race.outputs
                    && native::present_ring.submitted == before_race.ring_submissions,
                "Every present leaves base claims unchanged until the identified SR group is delivered");
          const auto guides = std::ranges::find_if(native::present_guide_sets, [&](const auto& candidate) {
            return candidate.source_handle == sr_handle && candidate.base_frame.sequence == source_sequence
                && candidate.base_frame.backbuffer == renodx::utils::directx::NativeIdentity(current_buffer);
          });
          Check(guides != std::end(native::present_guide_sets) && guides->base_frame.sealed && guides->base_frame.attempted
                    && guides->base_frame.queue_entry == producer_entry && order->entered.load() == producer_entry,
                "Every present race starts after successful SR-to-current-BB identification with an exact queue receipt");
          std::jthread concurrent_writer([&] {
            ID3D12CommandList* lists[]{writer.Get()};
            native::HookedQueueExecuteCommandLists(gpu.queue.Get(), 1, lists);
          });
          const auto parked = WaitForSingleObject(gate.entered.get(), ParkedExecute::TIMEOUT_MS);
          const auto entered = order->entered.load();
          const bool still_parked = !gate.forwarded.load();
          const bool released = SetEvent(gate.release.get()) != FALSE;
          concurrent_writer.join();
          Check(parked == WAIT_OBJECT_0 && entered == producer_entry + 1 && still_parked
                    && released && gate.release_wait == WAIT_OBJECT_0 && gate.forwarded.load(),
                "another real game thread enters the same queue after Identify and forwards the pre-recorded BB writer");
          const std::unique_lock verify(order->submit_mutex, std::try_to_lock);
          Check(verify.owns_lock() && order->entered.load() == producer_entry + 1
                    && guides->base_frame.queue_entry == producer_entry && clock.outputs == before_race.base_outputs,
                "writer has returned before Present submission so the held-lock queue-entry comparison must reject old provenance");
        };
        event();
        Check(observed_gates == 1, "Every present race reaches the real model parameter observation exactly once");
      }

      const auto raced = stats();
      const auto raced_reset = model_resets.back();
      Check(raced.base_claims == before_race.base_claims && raced.base_outputs == before_race.base_outputs
                && !raced.base_complete && stream.base_history_unbound && clock.frame.sequence == previous_frame.sequence
                && raced.calls == before_race.calls + 1 && raced_reset == 0 && raced.outputs == before_race.outputs + 1
                && raced.ring_submissions == before_race.ring_submissions + 1
                && raced.refreshes == before_race.refreshes && raced.reuses == before_race.reuses
                && native::queue_submit_entries.load() == submit_entries + 2 && native::present_frames == 0,
            "late BB writer prevents base claim and publication while Every present still evaluates and submits once");
      const auto all_pixels = [&](const stages::Snapshot& snapshot, Pixel expected) {
        const auto bytes = snapshot.Pixels();
        const std::vector pixels(size * size, expected);
        return bytes.size() == pixels.size() * sizeof(Pixel)
            && std::memcmp(bytes.data(), pixels.data(), bytes.size()) == 0;
      };
      Check(all_pixels(produced, GAME_PIXEL) && all_pixels(written, OVERWRITE_PIXEL),
            "racing game list reads the successful SR image then writes every expected FP16 Backbuffer pixel on the GPU");
      if (depth_mask) {
        const auto pixels = raced_mask.Pixels();
        const std::array<uint8_t, 4> expected_mask{static_cast<uint8_t>(motion_mask ? 0 : 204), 153, 102, 255};
        bool matches = !pixels.empty();
        for (size_t offset = 0; offset < pixels.size(); offset += expected_mask.size()) {
          matches &= std::memcmp(pixels.data() + offset, expected_mask.data(), expected_mask.size()) == 0;
        }
        std::cout << "PRESENT_DEPTH_LATE motion=" << motion_mask << " current_bb_overwritten=1 complete=" << raced.base_complete
                  << " calls=" << raced.calls - before_race.calls << " outputs=" << raced.outputs - before_race.outputs
                  << " rgba=" << static_cast<uint32_t>(pixels[0]) << ',' << static_cast<uint32_t>(pixels[1]) << ','
                  << static_cast<uint32_t>(pixels[2]) << ',' << static_cast<uint32_t>(pixels[3])
                  << " expected_r=" << static_cast<uint32_t>(expected_mask[0])
                  << std::endl;
        Check(matches, "late BB replacement revokes only depth attenuation before the original NR submission");
      }
      const auto raced_pixels = read_back();
      Check(raced_pixels != std::vector(size * size, OVERWRITE_PIXEL)
                && std::ranges::all_of(raced_pixels, [](const Pixel& pixel) { return pixel[3] == OVERWRITE_PIXEL[3]; }),
            "Every present delivers its actual edited color with the racing writer's current alpha");
      const auto raced_frame = clock.frame;
      swap();
      const auto before_recovery = stats();
      {
        funnel_accounting_tests::Restore observer;
        observer.Value(&model_parameter_observer);
        if (depth_mask) {
          model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* parameters) {
            auto* mask = native::GetD3D12Resource(parameters, "DLSSNR.ControlMask");
            Check(mask != nullptr, "next proven frame still binds the depth mask");
            recovered_mask = stages::Capture(gpu, mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          };
        }
        present_frame();
      }
      const auto recovered = stats();
      Check(recovered.base_claims == before_recovery.base_claims + 1
                && recovered.base_outputs == before_recovery.base_outputs + 1 && recovered.base_complete
                && clock.frame.source == raced_frame.source && clock.frame.generation == raced_frame.generation
                && clock.frame.output_epoch == raced_frame.output_epoch && clock.frame.sequence == source_sequence + 1
                && !clock.frame.reuse_edits && recovered.calls == before_recovery.calls + 1 && model_resets.back() == 1
                && recovered.outputs == before_recovery.outputs + 1
                && recovered.ring_submissions == before_recovery.ring_submissions + 1
                && recovered.refreshes == before_recovery.refreshes && recovered.reuses == before_recovery.reuses,
            "the next actual ordered SR copy recovers a complete Every present base product and sends model Reset");
      if (depth_mask) {
        const auto pixels = recovered_mask.Pixels();
        const std::array<uint8_t, 4> expected_mask{0, 153, 102, 255};
        bool matches = !pixels.empty();
        for (size_t offset = 0; offset < pixels.size(); offset += expected_mask.size()) {
          matches &= std::memcmp(pixels.data() + offset, expected_mask.data(), expected_mask.size()) == 0;
        }
        Check(matches, "next proven frame restores depth R attenuation and preserves manual G/B");
        std::cout << "PRESENT_DEPTH_LATE recovery=1 rgba=0,153,102,255 calls=1 outputs=1\n";
      }
      alpha_preserved(read_back());
      swap();
      std::cout << "PRESENT_EVERY_ORDER_RACE producer_entry=" << producer_entry
                << " writer_entry=" << producer_entry + 1 << " candidate_sequence=" << source_sequence
                << " claims=" << raced.base_claims - before_race.base_claims
                << " calls=" << raced.calls - before_race.calls << " reset=" << raced_reset
                << " outputs=" << raced.base_outputs - before_race.base_outputs
                << " complete=" << raced.base_complete << " recovery_outputs=" << recovered.base_outputs - raced.base_outputs
                << " recovery_reset=" << model_resets.back() << '\n';
      if (depth_mask) {
        if (motion_mask) {
          upload(motion.Get(), std::array<float, 2>{0.f, 0.f}, NATIVE_INPUT_STATE);
        }
        load_present_mode("0");
        present_frame();
        reopen();
        swap();
      }
    }

    {
      namespace signal_fault = present_stage_resolution_cases;
      ComPtr<ID3D12QueryHeap> execution_query;
      const D3D12_QUERY_HEAP_DESC query_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 1};
      Check(SUCCEEDED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&execution_query))),
            "Every present Signal failure observes actual model GPU execution");
      const auto execution_readback = gpu.Buffer(sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
      const auto execution_stamp = [&] {
        uint64_t* mapped = nullptr;
        Check(SUCCEEDED(execution_readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))),
              "Every present Signal failure reads the completed GPU timestamp");
        const auto stamp = *mapped;
        execution_readback->Unmap(0, nullptr);
        return stamp;
      };
      funnel_accounting_tests::Restore substitutions;
      substitutions.Value(&model_execution_query);
      substitutions.Value(&model_execution_readback);
      substitutions.Value(&model_parameter_observer);
      substitutions.Value(&signal_fault::real_signal);
      substitutions.Value(&signal_fault::fail_signal);
      model_execution_query = execution_query.Get();
      model_execution_readback = execution_readback.Get();
      signal_fault::fail_signal = false;
      signal_fault::real_signal = reinterpret_cast<signal_fault::SignalFn>((*reinterpret_cast<void***>(gpu.queue.Get()))[14]);
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
                && DetourAttach(reinterpret_cast<PVOID*>(&signal_fault::real_signal),
                                reinterpret_cast<PVOID>(signal_fault::Signal)) == NO_ERROR
                && DetourTransactionCommit() == NO_ERROR,
            "Every present reuses the scoped Present ring Signal fault detour");
      const auto unhook_signal = [](int*) {
        signal_fault::fail_signal = false;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&signal_fault::real_signal), reinterpret_cast<PVOID>(signal_fault::Signal));
        DetourTransactionCommit();
      };
      int hook_key = 0;
      const std::unique_ptr<int, decltype(unhook_signal)> signal_hook(&hook_key, unhook_signal);

      const auto before_first = stats();
      present_frame();
      const auto first = stats();
      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      const auto first_frame = clock.frame;
      const auto first_execution = execution_stamp();
      Check(first.base_complete && !stream.base_history_unbound && !clock.frame.reuse_edits
                && first.base_claims == before_first.base_claims + 1 && first.base_outputs == before_first.base_outputs + 1
                && first.calls == before_first.calls + 1 && first.ring_submissions == before_first.ring_submissions + 1
                && native::present_frames == 0 && first_execution != 0,
            "F1 first completes an identified Every present output with actual GPU execution");
      bool repeated_complete_seen = false;
      model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) {
        Check(clock.complete && !stream.base_history_unbound && clock.claims == first.base_claims
                  && clock.outputs == first.base_outputs && clock.frame.sequence == first_frame.sequence,
              "same-F1 Every present Evaluate begins with the old completed product still published");
        repeated_complete_seen = true;
      };
      ComPtr<ID3D12Fence> execution_gate;
      Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&execution_gate)))
                && SUCCEEDED(gpu.queue->Wait(execution_gate.Get(), 1)),
            "hold the real repeated-F1 GPU submission while checking Signal failure retention");
      Observed failed;
      try {
        signal_fault::fail_signal = true;
        native::OnPresent(&output.queue, &output, nullptr, nullptr, 0, nullptr);
        model_parameter_observer = {};
        failed = stats();
        Check(repeated_complete_seen && !signal_fault::fail_signal
                  && failed.base_claims == first.base_claims && failed.base_outputs == first.base_outputs
                  && !failed.base_complete && stream.base_history_unbound
                  && clock.frame.sequence == first_frame.sequence
                  && failed.calls == first.calls + 1 && model_resets.back() == 0
                  && failed.outputs == first.outputs + 1 && failed.ring_submissions == first.ring_submissions + 1,
              "same-F1 Execute followed by failed Signal withdraws old completion without publishing another base output");
        const addon::RuntimeLock lock(native::runtime_mutex);
        const auto& pending = native::present_ring.slots[failed.ring_submissions % native::kPresentRing];
        Check(pending.list != nullptr && pending.allocator != nullptr && pending.value == failed.ring_submissions
                  && !native::PresentRingComplete() && !native::ReleasePresentRing(),
              "failed Signal retains the genuinely in-flight Present list, allocator and owned work until completion");
      } catch (...) {
        model_parameter_observer = {};
        execution_gate->Signal(1);
        finish();
        throw;
      }
      Check(SUCCEEDED(execution_gate->Signal(1)), "release the actual repeated-F1 GPU submission");
      finish();
      const auto repeated_execution = execution_stamp();
      Check(repeated_execution > first_execution && !native::PresentRingComplete(),
            "an independent real Fence proves repeated-F1 GPU execution while its failed ring Signal remains unproven");
      const auto pixels = read_back();
      alpha_preserved(pixels);
      Check(pixels != std::vector(size * size, GAME_PIXEL), "the failed-Signal submission actually writes edited BB pixels");
      swap();

      present_frame();
      const auto recovered = stats();
      Check(recovered.base_claims == failed.base_claims + 1 && recovered.base_outputs == failed.base_outputs + 1
                && recovered.base_complete && !stream.base_history_unbound
                && native::present_frame_policy::SameSource(first_frame, clock.frame)
                && clock.frame.sequence == first_frame.sequence + 1 && !clock.frame.reuse_edits
                && recovered.calls == failed.calls + 1 && model_resets.back() == 1
                && recovered.outputs == failed.outputs + 1 && recovered.ring_submissions == failed.ring_submissions + 1
                && recovered.refreshes == first.refreshes && recovered.reuses == first.reuses
                && native::PresentRingComplete()
                && native::present_ring.fence->GetCompletedValue() >= failed.ring_submissions,
            "next actual F2 resets model history and its successful same-queue Signal proves both submissions complete");
      alpha_preserved(read_back());
      swap();
      std::cout << "PRESENT_EVERY_SIGNAL_FAILURE sequence=" << first_frame.sequence
                << " gpu_execution=" << first_execution << ',' << repeated_execution
                << " complete=" << failed.base_complete << " base_outputs=" << failed.base_outputs - first.base_outputs
                << " recovery_outputs=" << recovered.base_outputs - failed.base_outputs
                << " recovery_reset=" << model_resets.back() << '\n';

      auto& present_stream = native::present_streams.at(&output);
      const std::array back_buffers{reinterpret_cast<uintptr_t>(output.buffers[0].Get()),
          reinterpret_cast<uintptr_t>(output.buffers[1].Get())};
      const auto swapchain_key = reinterpret_cast<uintptr_t>(&output);
      const auto native_device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get()));
      addon::output_framerate::RegisterPresentOutput(swapchain_key, native_device,
          reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.swapchain.Get())), back_buffers);
      substitutions.Atomic(&native::nr_adaptive_enabled);
      substitutions.Atomic(&native::nr_adaptive_selected_levels);
      substitutions.Atomic(&native::nr_adaptive_limited_residency);
      const auto unregister = [&](int*) { addon::output_framerate::DestroyPresentOutput(swapchain_key); };
      const std::unique_ptr<int, decltype(unregister)> output_registration(&hook_key, unregister);
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = 2;
      native::nr_adaptive_limited_residency = false;
      using AdaptiveReceipt = addon::adaptive_resolution::DeliveryReceipt;
      const auto compatible_frame = [&](std::shared_ptr<AdaptiveReceipt>* receipt = nullptr) {
        // Guides accompany successful SR. The game writes a separate scene to
        // BB, so this compatibility frame has no exact SR-to-BB provenance.
        const auto previous_timing = addon::gpu_timers::internal::next_sequence;
        evaluate_sr(false, false);
        upload(back(), game_pixel, PRESENT);
        submit();
        event();
        if (receipt != nullptr) {
          const auto& segments = addon::gpu_timers::internal::segments;
          const auto timing = std::ranges::find_if(segments, [&](const auto& segment) {
            return segment.recorded && segment.measure && segment.sequence > previous_timing
                && std::strcmp(segment.path, "present") == 0 && segment.adaptive.delivery != nullptr;
          });
          Check(timing != std::end(segments), "compatibility Present records an adaptive GPU segment for the actual frame");
          *receipt = timing->adaptive.delivery;
        }
        const auto edited = read_back();
        swap();
        return edited;
      };
      const auto claim_count = present_stream.base_frame_clock.claims;
      std::shared_ptr<addon::adaptive_resolution::Session> adaptive;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 24; ++attempt) {
        compatible_frame();
        const auto producer = native::features.find(present_stream.Handle());
        if (producer == native::features.end()) continue;
        adaptive = producer->second.adaptive_session;
        if (adaptive != nullptr && adaptive->TargetReady() && adaptive->applied_generation == adaptive->status.generation) break;
      }
      Check(adaptive != nullptr && adaptive->TargetReady() && adaptive->ladder.count == 2
                && adaptive->status.applied == adaptive->status.upper
                && adaptive->applied_generation == adaptive->status.generation,
            "Every present compatibility confirms the upper tier after a complete ordered two-tier preparation");
      const auto sampled_output = addon::output_framerate::MatchPresentOutput(swapchain_key, back_buffers[0]);
      const auto output_end = addon::output_framerate::NowNs() - 10'000'000;
      for (int index = 240; index >= 0; --index) {
        addon::output_framerate::ObservePresent(sampled_output, output_end - index * int64_t{8'333'334});
      }
      Check(addon::output_framerate::Read(sampled_output, output_end).available,
            "ordinary adaptive trial has an independent valid output window");
      compatible_frame();
      const auto upper_generation = adaptive->applied_generation;
      const auto upper_applied_ns = adaptive->applied_ns;
      adaptive->previous_level = 0;
      adaptive->trial = true;
      adaptive->Request(1, native::SteadyNowNs());
      const auto low_request = adaptive->status.generation;
      const auto before_fault = stats();
      const auto before_fault_stamp = execution_stamp();
      adaptive->next_measure_ns = 0;
      signal_fault::fail_signal = true;
      std::shared_ptr<AdaptiveReceipt> failed_receipt;
      const auto failed_pixels = compatible_frame(&failed_receipt);
      Check(!signal_fault::fail_signal && stats().ring_submissions == before_fault.ring_submissions + 1
                && *failed_receipt == AdaptiveReceipt::REJECTED
                && execution_stamp() > before_fault_stamp && !native::PresentRingComplete()
                && failed_pixels != std::vector(size * size, GAME_PIXEL)
                && adaptive->TargetReady() && adaptive->status.generation == low_request
                && adaptive->applied_generation == upper_generation && adaptive->applied_ns == upper_applied_ns
                && adaptive->status.applied == adaptive->status.upper && adaptive->gpu_count == 0
                && addon::output_framerate::Read(sampled_output, addon::output_framerate::NowNs()).available
                && present_stream.base_frame_clock.claims == claim_count,
            "failed compatibility Signal retains actual WARP writes and upper confirmation, with no base claim or low-tier sample");
      adaptive->next_measure_ns = 0;
      std::shared_ptr<AdaptiveReceipt> recovered_receipt;
      compatible_frame(&recovered_receipt);
      Check(adaptive->applied_generation == low_request && adaptive->status.applied == adaptive->ladder.sizes[1]
                && *recovered_receipt == AdaptiveReceipt::DELIVERED
                && adaptive->applied_ns > upper_applied_ns && model_resets.back() == 1
                && native::PresentRingComplete() && present_stream.base_frame_clock.claims == claim_count,
            "the next unclaimed compatibility frame resets locally and confirms the complete low tier only after Signal");
      const auto confirmed_at = adaptive->applied_ns;
      compatible_frame();
      Check(adaptive->applied_ns == confirmed_at && present_stream.base_frame_clock.claims == claim_count,
            "subsequent compatibility delivery leaves first confirmation and base claims unchanged");
      std::cout << "ADAPTIVE_DELIVERY_PRESENT evidence=WARP+MOCK failed_signal_writes=1 recovered_reset=1 base_claim_delta=0\n";
    }

    {
      namespace options = addon::fg::native_options;
      namespace directx = renodx::utils::directx;
      enum class Route { ORDINARY, SDK_FIRST, OUTER_FIRST };

      StreamlineSwapchainFace outer(output.swapchain.Get());
      const auto* identity = directx::NativeIdentity(output.swapchain.Get());
      Check(identity != nullptr && directx::NativeIdentity(&outer) != identity
                && native::StreamlineBaseIdentity(&outer) == identity,
            "raw Present fixture exposes distinct outer and SDK swapchain identities");
      funnel_accounting_tests::Restore substitutions;
      substitutions.Atomic(&options::internal::present_observed);
      substitutions.Atomic(&options::internal::decisions_ready);
      substitutions.Atomic(&options::internal::stopped);
      options::internal::present_observed.store(native::OnNativePresentObserved);
      options::internal::decisions_ready.store(true);
      options::internal::stopped.store(false);
      Check(options::ObservesPresentDecisions() && native::present_frames == 0 && !native::WantsPresentBaseFrames(),
            "native SDK routing tests start with the file-backed Every present strategy");

      const auto& stream = native::present_streams.at(&output);
      const auto& clock = stream.base_frame_clock;
      bool raw_contracts_passed = true;
      const auto expect = [&](bool ok, const char* scenario, const char* message) {
        if (ok) return;
        std::cerr << "FAIL PRESENT_EVERY_RAW " << scenario << ' ' << message << '\n';
        raw_contracts_passed = false;
      };
      const auto forward = [&](Route route, const void* marker = nullptr) {
        const bool deferred = route == Route::OUTER_FIRST;
        const auto before = stats();
        const auto prior_clock = trace::PresentClockState(clock);
        const auto epoch = stream.base_frame_epoch;
        const auto fg_submissions = native::native_fg_submitted_frames.load();
        const auto callbacks = native::present_frames_seen.load();
        if (deferred) {
          output.present_face = &outer;
          event();
          const auto& pending = native::native_deferred_present;
          std::cout << "PRESENT_EVERY_RAW_WAIT epoch=" << epoch << ',' << stream.base_frame_epoch
                    << " claims=" << before.base_claims << ',' << clock.claims
                    << " outputs=" << before.base_outputs << ',' << clock.outputs
                    << " calls=" << model_calls - before.calls << " pending=" << (pending.identity == identity) << std::endl;
          expect(pending.identity == identity && pending.swapchain == &output && pending.queue == &output.queue
                     && pending.route_id == stream.native_route_id && epoch == stream.base_frame_epoch
                     && stats() == before && trace::PresentClockState(clock) == prior_clock,
                 "outer-first", "outer OnPresent waits without consuming the base or changing its history");
        }
        {
          const options::internal::PresentDecisionScope decision(output.swapchain.Get(), 0);
          const auto& input = native::native_sdk_present_input;
          Check(input.identity == identity && input.decision_id != 0, "production SDK_BEGIN identifies this decision");
          if (marker != nullptr) {
            // Stand in for the same-decision enhanced input from RestoreNativePreFg.
            native::native_sdk_present_input = {.identity = input.identity, .decision_id = input.decision_id,
                .owner = &stream, .route_id = stream.native_route_id, .backbuffer = back(),
                .marker = marker, .serial = input.decision_id};
          }
          decision.Publish(S_FALSE, false);
        }
        if (deferred) {
          finish();
        } else {
          expect(native::native_passthrough_identity == identity && stats() == before
                     && trace::PresentClockState(clock) == prior_clock && stream.base_frame_epoch == epoch,
                 "sdk-first", "SDK_FORWARD only grants routing before the real OnPresent callback");
          event();
        }
        expect(native::native_passthrough_identity == nullptr && native::native_deferred_present.identity == nullptr
                   && native::present_frames_seen.load() == callbacks + 1
                   && native::native_fg_submitted_frames.load() == fg_submissions,
               deferred ? "outer-first" : "sdk-first", "one application callback consumes the route without inventing FG submission");
        output.present_face = nullptr;
      };
      const auto trusted = [&](const char* scenario, Route route, int expected_reset, bool game_reset = false) {
        const auto before = stats();
        const auto epoch = stream.base_frame_epoch;
        const auto sequence = native::features.at(sr_handle).sr_sequence;
        const auto history_resets = native::present_history_resets.load();
        const auto queue_entries = native::queue_submit_entries.load();
        record(game_reset);
        submit();
        Check(native::queue_submit_entries.load() == queue_entries + 1, "raw Present producer submits one real SR-to-BB list");
        if (route == Route::ORDINARY) {
          event();
        } else {
          forward(route);
        }
        const auto after = stats();
        const auto& source = native::features.at(sr_handle);
        const int reset = (after.calls != before.calls ? model_resets.back() : -1);
        const auto pixels = read_back();
        std::cout << "PRESENT_EVERY_RAW " << scenario << " source_sequence=" << source.sr_sequence
                  << " claimed_sequence=" << clock.frame.sequence << " epoch=" << epoch << ',' << stream.base_frame_epoch
                  << " claims=" << before.base_claims << ',' << after.base_claims
                  << " base_outputs=" << before.base_outputs << ',' << after.base_outputs
                  << " outputs=" << before.outputs << ',' << after.outputs << " complete=" << after.base_complete
                  << " calls=" << after.calls - before.calls << " reset=" << reset << " expected_reset=" << expected_reset
                  << " reset_epoch=" << before.reset_epoch << ',' << after.reset_epoch
                  << " history_unbound=" << stream.base_history_unbound
                  << " history_resets=" << native::present_history_resets.load() - history_resets << " rgba_crc32="
                  << renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(Pixel))
                  << std::endl;
        expect(epoch != 0 && stream.base_frame_epoch == epoch && clock.frame.output_epoch == epoch
                   && source.sr_sequence == sequence + 1 && clock.frame.sequence == source.sr_sequence
                   && clock.frame.source == reinterpret_cast<uintptr_t>(sr_handle)
                   && clock.frame.generation == source.source_generation && clock.frame.kind == native::present_frame_policy::SourceKind::SR
                   && clock.frame.pass_count == 1 && clock.frame.roles == 1 && !clock.frame.reuse_edits
                   && after.base_claims == before.base_claims + 1 && after.base_outputs == before.base_outputs + 1
                   && after.base_complete && !stream.base_history_unbound,
               scenario, "current ordered SR source publishes one complete Every present base in its existing epoch");
        expect(after.calls == before.calls + 1 && reset == expected_reset && after.outputs == before.outputs + 1
                   && after.consumed_serial > before.consumed_serial && after.ring_submissions == before.ring_submissions + 1
                   && native::PresentRingComplete() && after.reset_epoch == before.reset_epoch + static_cast<uint64_t>(game_reset)
                   && native::present_frames == 0
                   && (expected_reset != 0 || native::present_history_resets.load() == history_resets)
                   && after.refreshes == before.refreshes && after.reuses == before.reuses,
               scenario, "one model Evaluate and completed submission preserve the requested Reset and Every present strategy");
        alpha_preserved(pixels);
        expect(pixels != std::vector(size * size, game_pixel), scenario, "complete GPU readback contains an actual color edit");
        swap();
        return pixels;
      };
      for (const auto route : {Route::SDK_FIRST, Route::OUTER_FIRST}) {
        // Keep both failure witnesses runnable when the baseline discarded the prior epoch.
        if (stream.base_frame_epoch == 0) event();
        const auto ordinary = trusted("ordinary-warm", Route::ORDINARY, 1, true);
        const char* scenario = (route == Route::SDK_FIRST ? "sdk-first" : "outer-first");
        for (int frame = 0; frame < 2; ++frame) {
          expect(trusted(scenario, route, 0) == ordinary, scenario, "GPU readback matches all ordinary RGBA pixels");
        }
        expect(trusted("ordinary-return", Route::ORDINARY, 0) == ordinary, scenario,
               "switching back to ordinary processing preserves the same source history and RGBA");
      }

      const auto before_unknown = stats();
      const auto before_unknown_frame = clock.frame;
      evaluate_sr(false, true);
      upload(back(), OVERWRITE_PIXEL, PRESENT);
      submit();
      forward(Route::SDK_FIRST);
      const auto unknown = stats();
      std::cout << "PRESENT_EVERY_RAW unknown-source epoch=" << stream.base_frame_epoch
                << " claims=" << before_unknown.base_claims << ',' << unknown.base_claims
                << " base_outputs=" << before_unknown.base_outputs << ',' << unknown.base_outputs
                << " complete=" << unknown.base_complete << " calls=" << unknown.calls - before_unknown.calls << std::endl;
      expect(unknown.base_claims == before_unknown.base_claims && unknown.base_outputs == before_unknown.base_outputs
                 && !unknown.base_complete && clock.frame.sequence == before_unknown_frame.sequence
                 && stream.base_frame_epoch == before_unknown_frame.output_epoch,
             "unknown-source", "SDK_FORWARD cannot identify an independent BB write as the current SR base");
      read_back();
      swap();
      trusted("after-unknown", Route::SDK_FIRST, 1);

      const auto enhanced = gpu.Texture(size, size, FORMAT, PRESENT);
      for (const bool ordered : {true, false}) {
        const auto epoch = stream.base_frame_epoch;
        record();
        if (!ordered) {
          submit();
          reopen();
        } else {
          upload(enhanced.Get(), OVERWRITE_PIXEL, PRESENT);
        }
        native::fg_service::CopyColor(gpu.list.Get(), back(), PRESENT, enhanced.Get(), PRESENT);
        if (ordered) {
          submit();
        } else {
          Check(SUCCEEDED(gpu.list->Close()), "close the enhanced recording without executing it");
        }
        std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
        const auto proof = addon::submission::UseSubmissionTokens(enhanced.Get(), &receipts);
        Check(ordered ? proof == addon::submission::UseSubmission::kTokens && receipts.size() == 1
                            && receipts.front().first->GetCompletedValue() >= receipts.front().second
                      : proof == addon::submission::UseSubmission::kUnsubmitted,
              "enhanced SDK input has a real copy receipt only when the copy was actually submitted");
        const auto before_enhanced = stats();
        const auto decline = (ordered ? addon::NrDeclineReason::kPresentPreFg : addon::NrDeclineReason::kPresentBusy);
        const auto& declines = addon::nr_decline_counts[static_cast<size_t>(decline)];
        const auto prior_declines = declines.load();
        forward(Route::SDK_FIRST, enhanced.Get());
        const auto after = stats();
        const char* scenario = (ordered ? "enhanced-ordered" : "enhanced-unsubmitted");
        std::cout << "PRESENT_EVERY_RAW " << scenario << " token_count=" << receipts.size()
                  << " epoch=" << epoch << ',' << stream.base_frame_epoch << " complete=" << after.base_complete
                  << " calls=" << after.calls - before_enhanced.calls << " claims=" << before_enhanced.base_claims << ','
                  << after.base_claims << " base_outputs=" << before_enhanced.base_outputs << ',' << after.base_outputs
                  << " decline=" << declines.load() - prior_declines << std::endl;
        expect(after.calls == before_enhanced.calls && after.ring_submissions == before_enhanced.ring_submissions
                   && after.outputs == before_enhanced.outputs && after.base_claims == before_enhanced.base_claims
                   && after.base_outputs == before_enhanced.base_outputs && declines.load() == prior_declines + 1,
               scenario, "the same SDK decision checks delivery and skips a second NR evaluation");
        expect(epoch != 0 && stream.base_frame_epoch == epoch && !after.base_complete,
               scenario, "enhanced input clears the base product while keeping the BB observation epoch active");
        // Readback recycles the game list, discarding the unsubmitted copy.
        unchanged_pixels(read_back(), ordered ? OVERWRITE_PIXEL : GAME_PIXEL);
        swap();
        trusted(ordered ? "after-enhanced-ordered" : "after-enhanced-unsubmitted", Route::OUTER_FIRST, 1);
      }
      trusted("after-enhanced-steady", Route::OUTER_FIRST, 0);
      Check(raw_contracts_passed, "Every present raw SDK_FORWARD contracts (see PRESENT_EVERY_RAW witnesses)");
    }

    load_present_mode("1");
    const auto before_unproven = stats();
    evaluate_sr(false, true);
    upload(back(), OVERWRITE_PIXEL, PRESENT);
    submit();
    event();
    const auto unproven = stats();
    no_output(before_unproven, unproven, "explicit one cannot obtain a base product without the actual SR-to-current-BB copy");
    Check(unproven.base_claims == before_unproven.base_claims
              && unproven.game_observations == before_unproven.game_observations,
          "successful game SR and valid guides alone cannot advance claimed ordinary Present history");
    unchanged_pixels(read_back(), OVERWRITE_PIXEL);
    swap();
    const auto before_ini_recovery = stats();
    present_frame();
    Check(stats().base_outputs == before_ini_recovery.base_outputs + 1 && model_calls == before_ini_recovery.calls + 1
              && model_resets.back() == 1,
          "explicit one recovers when the next producer copies its successful SR output to the current BB");

    read_back();
    swap();

    if (config_fixture) {
      namespace rate = addon::output_framerate;
      funnel_accounting_tests::Restore source_restore;
      source_restore.Value(&native::real_streamline_set_tag_for_frame);
      source_restore.Value(&native::real_streamline_set_tag);
      source_restore.Value(&native::real_streamline_evaluate);
      source_restore.Value(&native::real_streamline_free[0]);
      source_restore.Value(&output_source_evaluate);
      source_restore.Atomic(&native::nr_adaptive_enabled);
      native::nr_adaptive_enabled.store(true);
      std::array<uintptr_t, 2> buffers{};
      for (size_t index = 0; index < buffers.size(); ++index) {
        buffers[index] = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.buffers[index].Get()));
      }
      rate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(&output),
          reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
          reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.swapchain.Get())), buffers);
      const auto owner = rate::FindPresentOutput(0, buffers[output.get_current_back_buffer_index()]);
      Check(static_cast<bool>(owner), "Streamline source fixture has an actual output owner");
      sl::Resource buffer(sl::ResourceType::eTex2d, back());
      sl::Resource source(sl::ResourceType::eTex2d, sr_output.Get());
      const sl::ResourceTag tags[] = {
          {&buffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilEvaluate},
          {&source, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
      };
      OutputFrameToken frame(37);
      sl::ViewportHandle viewport(91);
      native::real_streamline_set_tag_for_frame = +[](const sl::FrameToken&, const sl::ViewportHandle&,
          const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) { return sl::Result::eOk; };
      Check(native::HookedStreamlineSetTagForFrame(frame, viewport, tags, static_cast<uint32_t>(std::size(tags)),
                reinterpret_cast<sl::CommandBuffer*>(gpu.list.Get())) == sl::Result::eOk,
            "successful actual framed tag wrapper binds the output and SR texture");
      output_source_evaluate = [&] { evaluate_sr(false, true); };
      native::real_streamline_evaluate = +[](sl::Feature, const sl::FrameToken&, const sl::BaseStructure**,
          uint32_t, sl::CommandBuffer*) {
        output_source_evaluate();
        return sl::Result::eOk;
      };
      const sl::BaseStructure* inputs[]{&viewport};
      const auto evaluate_output = [&](const sl::BaseStructure** selected_inputs, uint32_t count) {
        const auto result = native::HookedStreamlineEvaluate(sl::kFeatureDLSS, frame, selected_inputs, count,
            reinterpret_cast<sl::CommandBuffer*>(gpu.list.Get()));
        // Guide captures remain replayable until the producing list is reset.
        // Complete each source call before the next independent application frame.
        submit();
        event();
        reopen();
        return result;
      };
      Check(evaluate_output(inputs, 1) == sl::Result::eOk,
            "actual Streamline Evaluate forwards through the successful NGX SR wrapper");
      const rate::SourceIdentity identity{.handle = reinterpret_cast<uintptr_t>(sr_handle),
          .generation = native::create_contracts.at(sr_handle).source_generation, .device = owner.device,
          .resource = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(sr_output.Get())),
          .resource_generation = native::AdaptiveResourceGeneration(sr_output.Get())};
      Check(!rate::FindPresentOutput(identity.device, identity.resource) && rate::FindSourceOutput(identity) == owner,
            "the real framed production relation associates a distinct SR output without a strict x2 pixel copy");
      native::real_streamline_free[0] = +[](sl::Feature, const sl::ViewportHandle&) { return sl::Result::eOk; };
      Check(native::HookedStreamlineFreeResources<0>(sl::kFeatureDLSS, viewport) == sl::Result::eOk
                && !rate::FindSourceOutput(identity),
            "actual Streamline resource release revokes the old SR output owner");
      native::real_streamline_set_tag = +[](const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
          sl::CommandBuffer*) { return sl::Result::eOk; };
      native::nr_adaptive_enabled.store(false);
      Check(native::HookedStreamlineSetTag(viewport, tags, static_cast<uint32_t>(std::size(tags)),
                reinterpret_cast<sl::CommandBuffer*>(gpu.list.Get())) == sl::Result::eOk
                && evaluate_output(inputs, 1) == sl::Result::eOk
                && rate::FindSourceOutput(identity) == owner,
            "valid unframed Streamline tags observe the successful SR output while adaptive control is off");
      native::nr_adaptive_enabled.store(true);
      Check(evaluate_output(inputs, 1) == sl::Result::eOk
                && rate::FindSourceOutput(identity) == owner,
            "enabling adaptive control keeps the live persistent tag relation without requiring another tag call");
      Check(native::HookedStreamlineFreeResources<0>(sl::kFeatureDLSS, viewport) == sl::Result::eOk
                && evaluate_output(inputs, 1) == sl::Result::eOk
                && !rate::FindSourceOutput(identity),
            "a released unframed tag cannot rebind the next successful SR evaluation");
      sl::Resource different_texture(sl::ResourceType::eTex2d, color.Get());
      sl::ResourceTag local_back_buffer = tags[0];
      sl::ResourceTag local_source(&different_texture, sl::kBufferTypeScalingOutputColor,
          sl::ResourceLifecycle::eOnlyValidNow);
      local_source.next = &local_back_buffer;
      const sl::BaseStructure* local_inputs[]{&viewport, &local_source};
      Check(evaluate_output(local_inputs, 2) == sl::Result::eOk
                && !rate::FindSourceOutput(identity),
            "a local SR tag pointing to a different texture cannot substitute for the actual nested NGX output");
      local_source.resource = &source;
      Check(evaluate_output(local_inputs, 2) == sl::Result::eOk
                && rate::FindSourceOutput(identity) == owner && !rate::CaptureNativeLegacySourceTag(91),
            "Evaluate-local chained tags prove the nested NGX output without installing a global tag");
      Check(native::HookedStreamlineFreeResources<0>(sl::kFeatureDLSS, viewport) == sl::Result::eOk
                && !rate::FindSourceOutput(identity),
            "resource release withdraws the relation established by the local tags");
      rate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(&output));
      std::cout << "ADAPTIVE_OUTPUT_SOURCE evidence=WARP+MOCK framed_tags=1 legacy_tags=1 actual_sr=1 release_revoked=1\n";
    }

    {
      funnel_accounting_tests::Restore source_restore;
      source_restore.Value(&native::real_streamline_free[0]);
      load_present_mode("0");
      native::StreamlineCapture capture{.viewport = 73, .has_viewport = true, .has_constants = true,
          .hdr = true, .has_options = true, .motion_scale_x = 1.f / size, .motion_scale_y = 1.f / size};
      capture.color.resource = color.Get();
      capture.color.state = NATIVE_INPUT_STATE;
      capture.output.resource = sr_output.Get();
      capture.output.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      capture.motion.resource = motion.Get();
      capture.motion.state = NATIVE_INPUT_STATE;
      capture.depth.resource = depth.Get();
      capture.depth.state = NATIVE_INPUT_STATE;
      const auto before = stats();
      bool delivered = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !delivered; ++attempt) {
        upload(color.Get(), GAME_PIXEL, NATIVE_INPUT_STATE);
        upload(sr_output.Get(), GAME_PIXEL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        upload(back(), GAME_PIXEL, PRESENT);
        const auto calls = model_calls;
        const auto captures = native::present_captures.load();
        {
          const addon::RuntimeLock lock(native::runtime_mutex);
          Check(!native::ProcessStreamlineInline(gpu.list.Get(), sl::kFeatureDLSS, capture)
                    && model_calls == calls && native::present_captures.load() == captures + 1,
                "standalone Streamline Present records guides without an inline NR pass");
          const auto* source = native::last_source_plan.source_handle;
          Check(source != nullptr && native::synthetic_handles.contains(source)
                    && !native::EditReuseSourceConfirmed(source, native::features.at(source))
                    && native::QueryConfigurationCapability().CanSubmit()
                    && native::RequestedStagePlan().At(addon::stage_plan::Stage::PRESENT).count == 1,
                "Streamline keeps its ordinary Present capability without native SR or strict x2 credentials");
        }
        submit();
        const auto submitted = stats();
        event();
        const auto after = stats();
        Check(after.calls == submitted.calls || after.calls == submitted.calls + 1,
              "the Streamline ordinary Present consumer cannot issue extra model calls");
        delivered = after.outputs == submitted.outputs + 1 && after.calls == submitted.calls + 1;
        const auto pixels = read_back();
        if (delivered) {
          Check(after.calls == submitted.calls + 1 && pixels != std::vector(size * size, GAME_PIXEL),
                "ordinary OnPresent delivers a real edit from the Streamline guides");
          for (const auto& pixel : pixels) {
            Check(pixel[3] == GAME_PIXEL[3], "Streamline ordinary Present preserves the game alpha");
          }
        }
        Check(after.base_claims == before.base_claims && after.base_outputs == before.base_outputs
                  && after.refreshes == before.refreshes && after.reuses == before.reuses,
              "synthetic Streamline guides alone cannot claim a base frame or enable x2");
        swap();
      }
      Check(delivered, "the existing ordinary Present consumer accepts standalone Streamline guides");
      native::real_streamline_free[0] = +[](sl::Feature, const sl::ViewportHandle&) { return sl::Result::eOk; };
      Check(native::HookedStreamlineFreeResources<0>(sl::kFeatureDLSS, sl::ViewportHandle(capture.viewport)) == sl::Result::eOk,
            "Streamline releases the tested ordinary Present producer");
      std::cout << "PRESENT_STREAMLINE delivered_frame_calls=1 ordered_rgba=1 alpha=1 base_claims=0 x2=0 evidence=WARP+MOCK\n";
    }
    if (config_fixture) {
      namespace rate = addon::output_framerate;
      funnel_accounting_tests::Restore settings;
      settings.Atomic(&native::nr_adaptive_enabled);
      settings.Atomic(&native::nr_adaptive_selected_levels);
      settings.Atomic(&native::nr_adaptive_target_fps);
      settings.Atomic(&native::edit_reuse_enabled);
      settings.Atomic(&native::hook_point);
      settings.Value(&native::ngx_slot_real[0].evaluate);
      settings.Value(&native::ngx_slot_real[0].evaluate_c);
      settings.Value(&native::ngx_slot_real[0].create);
      native::ngx_slot_real[0].create = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
          NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) -> NVSDK_NGX_Result {
        static uint32_t foreign_handle = 1;
        *handle = reinterpret_cast<NVSDK_NGX_Handle*>(&foreign_handle);
        return NVSDK_NGX_Result_Success;
      });
      static ID3D12Resource* ordinary_game_color = nullptr;
      static bool ordinary_sr_fail = false;
      settings.Value(&ordinary_game_color);
      settings.Value(&ordinary_sr_fail);
      ordinary_game_color = color.Get();
      native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
          PFN_NVSDK_NGX_ProgressCallback_C) -> NVSDK_NGX_Result {
        // Exercise the C export's outer source scope and its nested C++ delegate.
        return native::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
      });
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* parameters,
          PFN_NVSDK_NGX_ProgressCallback) -> NVSDK_NGX_Result {
        auto* input = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
        auto* result = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
        const auto state = (input == ordinary_game_color ? NATIVE_INPUT_STATE : native::kPreSrReadable);
        native::Transition(list, input, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        native::Transition(list, result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(result, input);
        native::Transition(list, result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        native::Transition(list, input, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        return (ordinary_sr_fail ? NVSDK_NGX_Result_FAIL_UnableToInitializeFeature : NVSDK_NGX_Result_Success);
      });
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = 2;
      native::nr_adaptive_target_fps = 100.f;
      native::edit_reuse_enabled = false;
      const std::array buffers{
          reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.buffers[0].Get())),
          reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.buffers[1].Get())),
          reinterpret_cast<uintptr_t>(output.buffers[0].Get()), reinterpret_cast<uintptr_t>(output.buffers[1].Get())};
      const auto identity = [&] {
        return rate::SourceIdentity{.handle = reinterpret_cast<uintptr_t>(sr_handle),
            .generation = native::features.at(sr_handle).source_generation,
            .device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
            .resource = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(sr_output.Get())),
            .resource_generation = native::AdaptiveResourceGeneration(sr_output.Get())};
      };
      const auto copy_output = [&](bool full) {
        gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Transition(back(), PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        if (full) {
          gpu.list->CopyResource(back(), sr_output.Get());
        } else {
          const D3D12_TEXTURE_COPY_LOCATION from{.pResource = sr_output.Get(),
              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
          const D3D12_TEXTURE_COPY_LOCATION to{.pResource = back(),
              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
          const D3D12_BOX box{0, 0, 0, size - 1, size, 1};
          gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }
        gpu.Transition(back(), D3D12_RESOURCE_STATE_COPY_DEST, PRESENT);
        gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      };
      for (const auto hook : {native::kHookRender, native::kHookUpscaled}) {
        native::kSetHookPoint.Store(static_cast<float>(hook));
        rate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(&output));
        rate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(&output), identity().device,
            reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.swapchain.Get())), buffers);
        const auto owner = rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&output),
            reinterpret_cast<uintptr_t>(back()));
        Check(static_cast<bool>(owner), "the live ordinary output registers its native and canonical backbuffer identities");
        Check(!rate::FindSourceOutput(identity()), "a pure NGX stage starts without a prefilled output relation");
        event();

        evaluate_sr(false, true);
        copy_output(false);
        submit();
        event();
        Check(!rate::FindSourceOutput(identity()), "a partial copy cannot associate the intermediate NGX output");
        reopen();

        evaluate_sr(false, true);
        submit();
        reopen();
        copy_output(true);
        submit();
        event();
        Check(!rate::FindSourceOutput(identity()), "a full copy on a different recording cannot associate the NGX output");
        reopen();

        {
          evaluate_sr(false, true);
          copy_output(true);
          // event() finishes only prior queue work. Keep this unsubmitted
          // fixture recording's model uploads alive until its own submit.
          const auto pending_uploads = gpu.uploads;
          const auto pending_resources = gpu.retained_resources;
          event();
          Check(!rate::FindSourceOutput(identity()), "an unsubmitted copy cannot associate the NGX output");
          submit();
          event();
          Check(!rate::FindSourceOutput(identity()), "a late producer cannot reopen a rejected output observation");
          reopen();
        }

        NVSDK_NGX_Handle* foreign_source = nullptr;
        Check(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss,
                  source_parameters, &foreign_source) == NVSDK_NGX_Result_Success && foreign_source != sr_handle,
              "the failed overwrite comes from an independent registered NGX source");
        evaluate_sr(false, true);
        ordinary_sr_fail = true;
        Check(NVSDK_NGX_FAILED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), foreign_source,
                  source_parameters, nullptr)), "the second NGX source writes the intermediate output and fails");
        ordinary_sr_fail = false;
        copy_output(true);
        submit();
        event();
        Check(!rate::FindSourceOutput(identity()), "a failed opaque NGX overwrite revokes the earlier source proof");
        reopen();
        Check(native::HookedReleaseFeatureSlot<0>(foreign_source) == NVSDK_NGX_Result_Success,
              "release the failed independent source after its GPU work completes");

        {
          const auto other_window = CreateWindowExW(0, L"STATIC", L"NR Nexus ambiguous ordinary output", WS_OVERLAPPED,
              0, 0, size, size, nullptr, nullptr, module, nullptr);
          Check(other_window != nullptr, "the second output has its own hidden window");
          const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> other_owner(other_window, &DestroyWindow);
          Swapchain other(gpu, other_window, size, size);
          native::OnPresent(&other.queue, &other, nullptr, nullptr, 0, nullptr);
          evaluate_sr(false, true, true, true);
          copy_output(true);
          submit();
          event();
          Check(rate::FindSourceOutput(identity()) == owner, "the first full delivery establishes an ordinary owner");
          reopen();
          evaluate_sr(false, true);
          copy_output(true);
          auto* other_back = other.buffers.at(other.get_current_back_buffer_index()).Get();
          gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.Transition(other_back, PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.list->CopyResource(other_back, sr_output.Get());
          gpu.Transition(other_back, D3D12_RESOURCE_STATE_COPY_DEST, PRESENT);
          gpu.Transition(sr_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          submit();
          event();
          Check(!rate::FindSourceOutput(identity()), "one SR source fully delivered to two outputs is not a unique FPS owner");
          reopen();
          native::RetirePresentStream(&other);
        }

        if (hook == native::kHookRender) {
          Check(std::ranges::any_of(native::present_guide_sets, [](const auto& set) {
                    return set.motion != nullptr || set.depth != nullptr;
                  }), "the inactive Present path still owns real guide textures before idle cleanup");
          const auto cache = native::features.at(sr_handle).adaptive_cache;
          const auto epoch = native::present_streams.at(&output).base_frame_epoch;
          native::present_served_ns = native::SteadyNowNs() - native::kWorksetIdleRetireNs - 1;
          event();
          Check(std::ranges::all_of(native::present_guide_sets, [](const auto& set) {
                    return set.motion == nullptr && set.depth == nullptr;
                  }) && native::present_ring.fence == nullptr && native::present_prefg_hudless == nullptr
                    && native::present_streams.at(&output).base_frame_epoch == epoch
                    && native::features.at(sr_handle).adaptive_cache == cache,
                "idle cleanup retires legacy Present GPU objects while keeping the live source observer and NR cache");
        }

        const auto start = std::chrono::steady_clock::now();
        for (uint32_t frame = 0; frame < 260; ++frame) {
          std::this_thread::sleep_until(start + std::chrono::nanoseconds(8'333'333) * frame);
          evaluate_sr(false, true, true, frame % 2 == 0);
          copy_output(true);
          submit();
          const auto calls = model_calls;
          const auto clock_sequence = native::present_streams.at(&output).base_frame_clock.frame.sequence;
          event();
          rate::ObservePresent(owner, rate::NowNs());
          Check(model_calls == calls
                    && native::present_streams.at(&output).base_frame_clock.frame.sequence == clock_sequence,
                "ordinary source association adds no Present NR pass or base-frame claim");
          Check(rate::FindSourceOutput(identity()) == owner,
                "a successful NGX source and ordered same-list full copy bind its actual current backbuffer");
          reopen();
        }
        evaluate_sr(false, true);
        copy_output(true);
        submit();
        event();
        const auto sample = rate::Read(owner, rate::NowNs());
        const auto& session = native::features.at(sr_handle).adaptive_session;
        Check(sample.available && sample.source == rate::Source::PRESENT && sample.fps > 100.
                  && session != nullptr && session->status.output_fps > 100.
                  && session->status.display_output_generation == owner.generation
                  && session->status.applied[0].width != 0,
              "Render and Upscaled consume their attributed ordinary FPS from an intermediate pure NGX output");
        reopen();
        rate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(&output));
        Check(!rate::FindSourceOutput(identity()), "destroying the matched output removes the learned NGX relation");
      }
      std::cout << "ADAPTIVE_OUTPUT_NGX evidence=WARP+MOCK stages=Render,Upscaled intermediate=1 full_copy=1 rejected_partial_recording_unsubmitted=1\n";
    }
    Check(SUCCEEDED(gpu.list->Close()), "close the source consumer recording before teardown");

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
    if (!preserves_cross_epoch_writer) {
      std::cerr << "FAIL Present x2 cross-epoch writer must preserve its pixels without NR output, history, or model work\n";
    }
    if (!reset_observer_bypassed) {
      std::cerr << "FAIL Present x2 recycled recording must bypass the Draw/Dispatch source observer runtime lock\n";
    }
    if (!preserves_cross_epoch_writer || !reset_observer_bypassed) return 1;
    std::cout << "PASS ordinary D3D12 Present x2 WARP+MOCK: A/G/R 1/0/1/0 ordered RGBA, source replacement, prepared/output, "
                 "wrong buffer, discarded producer, missing jitter, late writes, failed model, "
                 "XeFG ownership, DXGI resize, cross-epoch writer, recycled list and legacy INI source/strategy\n";
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
