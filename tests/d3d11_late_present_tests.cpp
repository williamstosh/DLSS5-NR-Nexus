/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
// Cold-D3D11 test carrier. Official C APIs provision its own D3D12 device
// and NGX context; the unmodified Nexus recorder and RunBridge use them.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include "dlssnr.hpp"
#include "d3d11_late_present_readback.hpp"

namespace {
namespace nx = renodx::addons::dlss5::internal;
namespace addon = renodx::addons::dlss5;
namespace marker = addon::present_order_witness;
using Microsoft::WRL::ComPtr;
constexpr unsigned W = 3840, H = 2160;
constexpr marker::Dx11Roi kRegion{20, 129, 1220, 607};

bool Require(HRESULT hr, const char* phase) {
  if (SUCCEEDED(hr)) return true;
  std::fprintf(stderr, "%s hr=%08lx\n", phase, hr);
  return false;
}

uintptr_t Identity(IUnknown* object) {
  return reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(object));
}

struct Owner final : reshade::api::swapchain {
  reshade::api::effect_runtime* runtime = nullptr;
  IDXGISwapChain* chain = nullptr;
  HWND window = nullptr;
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(chain); }
  void get_private_data(const uint8_t[16], uint64_t*) const override {}
  void set_private_data(const uint8_t[16], uint64_t) override {}
  reshade::api::device* get_device() override { return runtime->get_device(); }
  void* get_hwnd() const override { return window; }
  reshade::api::resource get_back_buffer(uint32_t index) override { return runtime->get_back_buffer(index); }
  uint32_t get_back_buffer_count() const override { return runtime->get_back_buffer_count(); }
  uint32_t get_current_back_buffer_index() const override { return runtime->get_current_back_buffer_index(); }
  bool check_color_space_support(reshade::api::color_space c) const override {
    return c == reshade::api::color_space::hdr10_pq;
  }
  reshade::api::color_space get_color_space() const override { return reshade::api::color_space::hdr10_pq; }
};

struct Frame {
  unsigned id = 0, menu = 0, ngxcalls = 0;
  int effect = -1;
  bool gui = false;
  uintptr_t expected = 0;
  int reset = -1;
  bool delivered = false;
  addon::fg::xefg::OrdinaryPresentAttempt attempt;
  std::optional<marker::Dx11ColorSample> e, f, r, p;
  ComPtr<ID3D12Resource> input;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
Frame frame;
Owner* owner = nullptr;
ID3D11Texture2D* g_depth = nullptr;
ID3D11Texture2D* g_motion = nullptr;
uint32_t total_calls = 0;
ID3D12Device* ngx_device = nullptr;
bool official_initialized = false;

constexpr unsigned MarkerX(unsigned mark, unsigned col) {
  return static_cast<unsigned>(((mark ? .25f : .82f) + .03f * (col + .5f)) * W);
}
constexpr unsigned MarkerY(unsigned row) {
  return static_cast<unsigned>((.72f + .06f * (row + .5f)) * H);
}

NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature kind,
                                   const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  ComPtr<ID3D12Device> actual;
  if (kind != nx::kFeatureDlssNr || FAILED(list->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get() != ngx_device) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  const auto result = NVSDK_NGX_D3D12_CreateFeature(list, kind, const_cast<NVSDK_NGX_Parameter*>(parameters), handle);
  std::printf("official_create=%08x requested_preset=%u\n", static_cast<unsigned>(result),
              nx::GetUInt(parameters, "DLSSNR.Hint.Render.Preset"));
  return result;
}

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                     const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
  auto* color = nx::GetD3D12Resource(parameters, "DLSSNR.Color");
  ComPtr<ID3D12Device> actual;
  if (color == nullptr || FAILED(list->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get() != ngx_device) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  frame.reset = nx::GetInt(parameters, "DLSSNR.Reset", -1);
  const auto desc = color->GetDesc();
  frame.format = desc.Format;
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK, .CreationNodeMask = 1, .VisibleNodeMask = 1};
  const D3D12_RESOURCE_DESC rb{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 16 * 512, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  if (frame.ngxcalls == 0 && desc.Width == W && desc.Height == H && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
      && SUCCEEDED(ngx_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rb,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&frame.input)))) {
    nx::Transition(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (unsigned m = 0; m < 2; ++m)
      for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 4; ++x) {
          const unsigned ix = MarkerX(m, x), iy = MarkerY(y);
          const D3D12_TEXTURE_COPY_LOCATION dest{.pResource = frame.input.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = {512ull * (m * 8 + y * 4 + x), {desc.Format, 1, 1, 1, 256}}};
          const D3D12_TEXTURE_COPY_LOCATION from{.pResource = color, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          const D3D12_BOX box{ix, iy, 0, ix + 1, iy + 1, 1};
          list->CopyTextureRegion(&dest, 0, 0, 0, &from, &box);
        }
    nx::Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  ++frame.ngxcalls;
  ++total_calls;
  const auto result = NVSDK_NGX_D3D12_EvaluateFeature(list, handle, parameters, progress);
  std::printf("official_evaluate=%08x sequence=%u id=%u reset=%d bound_color=%p\n",
              static_cast<unsigned>(result), total_calls, frame.id, frame.reset, static_cast<void*>(color));
  return result;
}

bool InitOfficial(const wchar_t* project_file, const wchar_t* runtime_dir) {
  std::ifstream file(project_file);
  std::string id;
  if (!std::getline(file, id) || id.size() != 36) return false;
  const wchar_t* search[]{runtime_dir};
  NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo = {search, 1};
  const auto directory = std::filesystem::current_path().wstring();
  const auto hr = NVSDK_NGX_D3D12_Init_with_ProjectID(id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                      "NR-Nexus-ControlMask-P0-offline-1", directory.c_str(), ngx_device, &info);
  id.assign(id.size(), '\0');
  std::printf("official_init=%08x device=%p\n", static_cast<unsigned>(hr), static_cast<void*>(ngx_device));
  if (NVSDK_NGX_FAILED(hr)) return false;
  official_initialized = true;
  const auto module = GetModuleHandleW(L"_nvngx.dll");
  if (module == nullptr) return false;
  nx::direct_api = {.module = module, .allocate = NVSDK_NGX_D3D12_AllocateParameters, .destroy = NVSDK_NGX_D3D12_DestroyParameters, .create = Create, .evaluate = Evaluate, .release = NVSDK_NGX_D3D12_ReleaseFeature};
  nx::direct_device = ngx_device;
  ngx_device->AddRef();
  nx::direct_device_native = ngx_device;
  nx::direct_load_state = nx::DirectLoadState::Ready;
  nx::core_allocate_parameters = NVSDK_NGX_D3D12_AllocateParameters;
  nx::core_destroy_parameters = NVSDK_NGX_D3D12_DestroyParameters;
  nx::parameter_runtime_ready = true;
  nx::direct_runtime_requirements.dual_models = true;
  return true;
}

// The app owns both devices. Configure all actual interop resources using
// standard D3D11, D3D12 and DXGI. No private runtime discovery is invoked.
bool Provision(ID3D11Device* d11, ID3D11DeviceContext* c11,
               const wchar_t* project_file, const wchar_t* runtime_dir) {
  if (!renodx::utils::directx::Initialize()) return false;
  auto& b = nx::bridge;
  b.context_native = c11;
  b.device11_base = d11;
  d11->AddRef();
  if (!Require(c11->QueryInterface(IID_PPV_ARGS(&b.context11)), "D3D11 Context4")
      || !Require(d11->QueryInterface(IID_PPV_ARGS(&b.device11)), "D3D11 Device5")
      || !Require(c11->QueryInterface(IID_PPV_ARGS(&b.multithread)), "D3D11 Multithread")) return false;
  ComPtr<IDXGIDevice> dx;
  ComPtr<IDXGIAdapter> adapter;
  if (!Require(d11->QueryInterface(IID_PPV_ARGS(&dx)), "DXGI Device")
      || !Require(dx->GetAdapter(&adapter), "D3D11 adapter")
      || !Require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&b.device12)), "same-adapter D3D12")) return false;
  b.proxy12 = b.device12;
  b.proxy12->AddRef();
  ngx_device = b.device12;
  D3D12_COMMAND_QUEUE_DESC q{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (!Require(b.device12->CreateCommandQueue(&q, IID_PPV_ARGS(&b.queue)), "D3D12 private queue")) return false;
  for (auto& slot : b.ring) {
    if (!Require(b.device12->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&slot.allocator)), "private allocator")
        || !Require(b.device12->CreateCommandList(0, q.Type, slot.allocator, nullptr, IID_PPV_ARGS(&slot.list)), "private list")
        || !Require(slot.list->Close(), "private first Close")) return false;
  }
  for (auto [f12, f11] : {std::pair{&b.fence_in12, &b.fence_in11}, std::pair{&b.fence_done12, &b.fence_done11}}) {
    HANDLE token = nullptr;
    if (!Require(b.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f12)), "shared 12 fence")
        || !Require(b.device12->CreateSharedHandle(*f12, nullptr, GENERIC_ALL, nullptr, &token), "share fence")) return false;
    const auto opened = b.device11->OpenSharedFence(token, IID_PPV_ARGS(f11));
    CloseHandle(token);
    if (!Require(opened, "open same fence D3D11")) return false;
  }
  if (!Require(b.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b.fence_out12)), "out-only private fence")) return false;
  b.completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (b.completion_event == nullptr || !InitOfficial(project_file, runtime_dir)) return false;
  if (!nx::InstallQueueCompletionHooks(b.queue)) return false;

  nx::bridge_device11_identity.store(renodx::utils::directx::NativeIdentity(d11));
  nx::bridge_force_snippet = true;  // Ordinary no Core borrowing; exact ready public context is above.
  nx::bridge_adaptive_device_generation.fetch_add(1);
  nx::bridge_device_live = true;
  nx::bridge_ever_live = true;
  nx::bridge_state = nx::BridgeState::kUp;
  std::printf("fixture_boot=public_same_adapter_ctx12_shared_fences no_product_preload=1 d11=%p d12=%p\n",
              static_cast<void*>(d11), static_cast<void*>(b.device12));
  return true;
}

bool capture_after_nr = false;
void ObserveOverlay(reshade::api::effect_runtime* runtime) {
  if (runtime != owner->runtime) return;
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(runtime->get_command_queue()->get_native());
  auto* bb = reinterpret_cast<ID3D11Texture2D*>(runtime->get_current_back_buffer().handle);
  frame.gui = true;
  if (capture_after_nr) frame.r.emplace().Record(context, bb, 0, kRegion);
  ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(24.f, 136.f), ImVec2(110.f, 200.f), IM_COL32(255, 0, 0, 255));
}
uint32_t Pixel(const std::vector<uint8_t>& px, unsigned x, unsigned y) {
  uint32_t result = 0;
  const size_t offset = (size_t(y - kRegion.top) * (kRegion.right - kRegion.left) + x - kRegion.left) * 4;
  std::memcpy(&result, px.data() + offset, 4);
  return result;
}
bool Read(marker::Dx11ColorSample* sample, ID3D11DeviceContext* context, std::vector<uint8_t>* pixels) {
  const auto end = GetTickCount64() + 3000;
  context->Flush();
  while (GetTickCount64() < end) {
    const auto hr = sample->ReadIfReady(context, pixels);
    if (hr == S_OK) return true;
    if (FAILED(hr)) return false;
    Sleep(1);
  }
  return false;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) return 2;
  SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
  const HINSTANCE self = GetModuleHandleW(nullptr);
  static constexpr wchar_t cls[] = L"NexusLatePresentNoFx";
  const WNDCLASSW wc{.lpfnWndProc = DefWindowProcW, .hInstance = self, .lpszClassName = cls};
  if (!RegisterClassW(&wc)) return 3;
  HWND window = CreateWindowExW(WS_EX_NOACTIVATE, cls, L"NR Nexus isolated late DX11", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, self, nullptr);
  if (window == nullptr) return 3;
  DXGI_SWAP_CHAIN_DESC desc{};
  desc.BufferDesc = {W, H, {0, 0}, DXGI_FORMAT_R10G10B10A2_UNORM};
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 3;
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain> chain;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  if (!Require(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &desc, &chain, &device, nullptr, &ctx),
               "actual DX11 R10 chain")) return 4;
  ComPtr<IDXGISwapChain3> chain3;
  if (FAILED(chain.As(&chain3)) || FAILED(chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))) return 4;
  const auto rs = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (rs == nullptr || !reshade::register_addon(self, rs)) return 5;
  if (!Provision(device.Get(), ctx.Get(), argv[2], argv[3])) return 6;
  nx::enabled = true;
  nx::hooks_enabled = false;
  nx::d3d11_swapchain_seen = true;
  nx::dx11_route = nx::kDx11RouteNative;
  nx::hook_point = nx::kHookPresent;
  nx::stack_passes = 2;
  nx::preset = 2;
  nx::present_frames = 0;
  nx::present_pre_fg = 0;
  nx::monitor_requested = false;
  nx::hud_enabled = false;
  nx::nr_adaptive_enabled = false;
  nx::edit_reuse_enabled = false;
  nx::use_auto_mask = false;
  nx::codec_mode = 0;
  nx::feed_mode = 1;
  nx::look_mode = false;
  nx::nr_follow_input_res = false;
  nx::nr_resolution_scale = 1;
  nx::transfer_strength = 1;
  nx::color_strength = 1;
  nx::intensity = 1;
  nx::present_guides = nx::kPresentGuidesOptional;
  nx::dedupe_mode = addon::kDedupePerEvaluate;
  reshade::api::effect_runtime* runtime = nullptr;
  if (!reshade::create_effect_runtime(reshade::api::device_api::d3d11, device.Get(), ctx.Get(), chain.Get(), "ReShade.ini", &runtime)) return 8;
  Owner selected;
  selected.runtime = runtime;
  selected.chain = chain.Get();
  selected.window = window;
  owner = &selected;
  nx::OnInitEffectRuntime(runtime);
  std::vector<uintptr_t> bbs;
  for (uint32_t i = 0; i < selected.get_back_buffer_count(); ++i) bbs.push_back(selected.get_back_buffer(i).handle);
  addon::output_framerate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(owner), Identity(device.Get()), Identity(chain.Get()), bbs);
  bool observer_registered = false;
  unsigned qualified = 0;
  int failed = 0;
  const auto end = GetTickCount64() + 45000;
  for (unsigned id = 1; id <= 18 && GetTickCount64() < end && qualified < 3; ++id) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    frame = {};
    frame.id = id;
    ++nx::present_generation;
    ComPtr<ID3D11Texture2D> bb;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!Require(chain->GetBuffer(0, IID_PPV_ARGS(&bb)), "BB") || !Require(device->CreateRenderTargetView(bb.Get(), nullptr, &rtv), "RTV")) {
      failed = 20;
      break;
    }
    frame.expected = Identity(bb.Get());
    constexpr float early[]{.15f, .18f, .20f, 1.f};
    ctx->ClearRenderTargetView(rtv.Get(), early);
    const auto before_calls = total_calls;
    const auto before_nr = nx::present_streams.contains(owner) ? nx::present_streams.at(owner).nr_presents : 0;
    nx::OnPresent(runtime->get_command_queue(), owner, nullptr, nullptr, 0, nullptr);
    const bool deferred = nx::late_bridge_presents.size() == 1 && total_calls == before_calls;
    if (!observer_registered) {
      reshade::register_event<reshade::addon_event::reshade_overlay>(ObserveOverlay);
      observer_registered = true;
    }
    const float final_color[]{.36f + .015f * (id % 3), .43f, .26f, 1.f / 3.f};
    ctx->ClearRenderTargetView(rtv.Get(), final_color);
    ComPtr<ID3D11DeviceContext1> clear;
    ctx.As(&clear);
    const D3D11_RECT object{760, 154, 900, 350};
    const float object_color[]{.54f, .58f, .44f, 1.f / 3.f};
    clear->ClearView(rtv.Get(), object_color, &object, 1);
    frame.effect = static_cast<int>(id & 15);
    for (unsigned y = 0; y < 2; ++y)
      for (unsigned x = 0; x < 4; ++x) {
        const LONG mx = MarkerX(0, x), my = MarkerY(y);
        const D3D11_RECT cell{mx - 13, my - 12, mx + 13, my + 12};
        const float c = (((unsigned(frame.effect) >> x) & 1) ^ y) ? .60f : .18f;
        const float rgb[]{c, c, c, 1};
        clear->ClearView(rtv.Get(), rgb, &cell, 1);
      }
    frame.f.emplace().Record(ctx.Get(), bb.Get(), 0, kRegion);
    // Wrong runtime cannot consume the outer turn.
    nx::OnLateBridgeOverlay(nullptr);
    const bool wrong_runtime_rejected = nx::late_bridge_presents.size() == 1;
    const bool revoked = id == 3;
    const auto gaps_before = nx::present_gaps.load();
    if (revoked) ++nx::configuration_generation;
    capture_after_nr = true;
    runtime->open_overlay(false, reshade::api::input_source::none);
    reshade::update_and_present_effect_runtime(runtime);
    frame.p.emplace().Record(ctx.Get(), bb.Get(), 0, kRegion);
    const auto actual_nr = nx::present_streams.at(owner).nr_presents - before_nr;
    nx::OnFinishPresent(runtime->get_command_queue(), owner);
    if (!deferred || !wrong_runtime_rejected || !frame.gui || !nx::late_bridge_presents.empty()) {
      failed = 21;
      break;
    }
    std::vector<uint8_t> f, r, p;
    if (!Read(&*frame.f, ctx.Get(), &f) || !Read(&*frame.r, ctx.Get(), &r) || !Read(&*frame.p, ctx.Get(), &p)) {
      failed = 22;
      break;
    }
    unsigned changed = 0, preserved = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const unsigned x = 765 + i * 5, y = 160 + i * 5;
      const auto a = Pixel(f, x, y), b = Pixel(r, x, y), c = Pixel(p, x, y);
      changed += a != b;
      preserved += b == c;
    }
    const auto overlay_before = Pixel(r, 50, 150), overlay_after = Pixel(p, 50, 150);
    const bool overlay_clean = (overlay_after & 1023u) == 1023u && ((overlay_after >> 10) & 1023u) == 0
                               && overlay_before != overlay_after;
    bool fresh_rgb = false;
    int model_code = -1;
    if (frame.input) {
      const D3D12_RANGE range{0, 16 * 512};
      void* data = nullptr;
      if (SUCCEEDED(frame.input->Map(0, &range, &data))) {
        model_code = 0;
        for (unsigned x = 0; x < 4; ++x) {
          uint16_t low = 0, high = 0;
          std::memcpy(&low, static_cast<uint8_t*>(data) + x * 512, 2);
          std::memcpy(&high, static_cast<uint8_t*>(data) + (4 + x) * 512, 2);
          const float delta = DirectX::PackedVector::XMConvertHalfToFloat(low) - DirectX::PackedVector::XMConvertHalfToFloat(high);
          if (delta > 1e-5f)
            model_code |= 1 << x;
          else if (delta >= -1e-5f) {
            model_code = -1;
            break;
          }
        }
        const D3D12_RANGE no_write{};
        frame.input->Unmap(0, &no_write);
        fresh_rgb = model_code == frame.effect;
      }
    }
    const bool success = actual_nr == 1 && frame.ngxcalls == 2 && fresh_rgb && changed != 0 && preserved == 16 && overlay_clean;
    if (revoked) {
      if (actual_nr != 0 || frame.ngxcalls != 0 || changed != 0 || nx::present_gaps.load() != gaps_before + (before_nr != 0)) {
        failed = 23;
        break;
      }
    } else if (success)
      ++qualified;
    std::printf("frame=%u deferred=%d wrong_runtime_rejected=%d revoked=%d calls=%u nr=%llu fresh_rgb=%d changed=%u propagated=%u overlay_unmodified=%d result=%d\n",
                id, deferred, wrong_runtime_rejected, revoked, frame.ngxcalls, (unsigned long long)actual_nr, fresh_rgb, changed, preserved, overlay_clean, success);
    if (FAILED(chain->Present(0, 0))) {
      failed = 24;
      break;
    }
  }
  nx::CancelLateBridgePresent(owner);
  if (nx::late_bridge_overlay_listening) reshade::unregister_event<reshade::addon_event::reshade_overlay>(nx::OnLateBridgeOverlay);
  if (observer_registered) reshade::unregister_event<reshade::addon_event::reshade_overlay>(ObserveOverlay);
  nx::OnDestroyEffectRuntime(runtime);
  if (nx::DrainBridgeQueue() && official_initialized) {
    // Context teardown is app-owned; retain production resources until its
    // actual queue is idle, then release both model passes before Shutdown.
    const auto found = nx::features.find(nx::present_streams.at(owner).Handle());
    if (found != nx::features.end())
      for (auto& slot : found->second.slots) {
        if (slot.handle) {
          NVSDK_NGX_D3D12_ReleaseFeature(slot.handle);
          slot.handle = nullptr;
        }
        if (slot.parameters) {
          NVSDK_NGX_D3D12_DestroyParameters(slot.parameters);
          slot.parameters = nullptr;
        }
      }
    NVSDK_NGX_D3D12_Shutdown1(ngx_device);
  }
  nx::ReleaseBridge();
  addon::output_framerate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(owner));
  owner = nullptr;
  reshade::destroy_effect_runtime(runtime);
  reshade::unregister_addon(self, rs);
  DestroyWindow(window);
  std::printf("late_present qualified=%u failed=%d mode=%s fx=disabled hdr=PQ size=%ux%u\n", qualified, failed,
#if defined(NR_NEXUS_DEV_CONTROL)
              "DEV",
#else
              "normal",
#endif
              W, H);
  return failed != 0 ? failed : qualified == 3 ? 0
                                               : 25;
}
