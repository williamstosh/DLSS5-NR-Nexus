/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace original_intel_sdk_cases {
using Microsoft::WRL::ComPtr;

class Consumer {
  HMODULE xell_module = nullptr;
  HMODULE xefg_module = nullptr;
  decltype(&xellD3D12CreateContext) create_latency = nullptr;
  decltype(&xellDestroyContext) destroy_latency = nullptr;
  decltype(&xellSetSleepMode) set_sleep = nullptr;
  decltype(&xellSleep) sleep = nullptr;
  decltype(&xellAddMarkerData) marker = nullptr;
  decltype(&xefgSwapChainD3D12CreateContext) create_fg = nullptr;
  decltype(&xefgSwapChainSetLatencyReduction) bind_latency = nullptr;
  decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) init_fg = nullptr;
  decltype(&xefgSwapChainD3D12GetSwapChainPtr) get_proxy = nullptr;
  decltype(&xefgSwapChainSetUiCompositionState) set_composition = nullptr;
  decltype(&xefgSwapChainSetEnabled) set_enabled = nullptr;
  decltype(&xefgSwapChainD3D12TagFrameResource) tag = nullptr;
  decltype(&xefgSwapChainTagFrameConstants) constants = nullptr;
  decltype(&xefgSwapChainSetPresentId) present_id = nullptr;
  decltype(&xefgSwapChainGetLastPresentStatus) status = nullptr;
  decltype(&xefgSwapChainDestroy) destroy_fg = nullptr;
  HWND window = nullptr;
  HANDLE event = nullptr;
  bool registered_class = false;
  xell_context_handle_t latency = nullptr;
  xefg_swapchain_handle_t context = nullptr;
  ComPtr<IDXGISwapChain3> proxy;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 allocation_bytes = 0;
  uint64_t submitted = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t frame = 0;
  bool finished = false;
  std::filesystem::path receipt_path;
  nlohmann::json receipt;

  void WaitForGpu() {
    if (submitted == 0 || fence->GetCompletedValue() >= submitted) return;
    Check(SUCCEEDED(fence->SetEventOnCompletion(submitted, event)), "real Intel SDK input Fence event");
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0
              && fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= submitted,
          "real Intel SDK input GPU completion");
  }

  void WriteReceipt() {
    std::ofstream output(receipt_path);
    output << receipt.dump(2) << '\n';
    Check(!output.fail(), "write actual Intel SDK acceptance receipt");
  }

 public:
  Consumer(ID3D12CommandQueue* sdk_queue, uint32_t input_width, uint32_t input_height,
           const std::filesystem::path& path)
      : width(input_width), height(input_height), receipt_path(std::filesystem::absolute(path)) {
    Check(receipt_path.native().starts_with(L"D:") && std::filesystem::current_path().native().starts_with(L"D:"),
          "the actual Intel SDK fixture and its receipt run only from D-drive task output");
    for (const auto* name : {L"TEMP", L"TMP", L"TMPDIR"}) {
      wchar_t temporary[32768]{};
      Check(GetEnvironmentVariableW(name, temporary, 32768) != 0
                && std::filesystem::path(temporary).is_absolute()
                && std::filesystem::path(temporary).native().starts_with(L"D:"),
            "actual Intel SDK child-process temporary paths are explicit D-drive paths");
    }
    wchar_t sdk_path[32768]{};
    Check(GetEnvironmentVariableW(L"NR_NEXUS_TEST_INTEL_SDK_DIR", sdk_path, 32768) != 0,
          "the explicit prepared official Intel SDK directory is required");
    const std::filesystem::path sdk_directory(sdk_path);
    Check(sdk_directory.is_absolute() && sdk_directory.native().starts_with(L"D:"),
          "Intel SDK fixture reads the prepared D-drive modules");
    const auto xell_path = sdk_directory / L"libxell.dll";
    const auto xefg_path = sdk_directory / L"libxess_fg.dll";
    xell_module = LoadLibraryExW(xell_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    xefg_module = LoadLibraryExW(xefg_path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(xell_module != nullptr && xefg_module != nullptr, "actual prepared Intel SDK modules load");
    wchar_t loaded_xell[32768]{}, loaded_xefg[32768]{};
    Check(GetModuleFileNameW(xell_module, loaded_xell, 32768) != 0
              && GetModuleFileNameW(xefg_module, loaded_xefg, 32768) != 0
              && std::filesystem::equivalent(xell_path, loaded_xell)
              && std::filesystem::equivalent(xefg_path, loaded_xefg),
          "the actual loaded Intel modules are the explicit prepared originals");
    const auto load = [](HMODULE module, const char* name, auto* output) {
      *output = reinterpret_cast<std::remove_pointer_t<decltype(output)>>(GetProcAddress(module, name));
      Check(*output != nullptr, "required actual Intel SDK export");
    };
    load(xell_module, "xellD3D12CreateContext", &create_latency);
    load(xell_module, "xellDestroyContext", &destroy_latency);
    load(xell_module, "xellSetSleepMode", &set_sleep);
    load(xell_module, "xellSleep", &sleep);
    load(xell_module, "xellAddMarkerData", &marker);
    load(xefg_module, "xefgSwapChainD3D12CreateContext", &create_fg);
    load(xefg_module, "xefgSwapChainSetLatencyReduction", &bind_latency);
    load(xefg_module, "xefgSwapChainD3D12InitFromSwapChainDesc", &init_fg);
    load(xefg_module, "xefgSwapChainD3D12GetSwapChainPtr", &get_proxy);
    load(xefg_module, "xefgSwapChainSetUiCompositionState", &set_composition);
    load(xefg_module, "xefgSwapChainSetEnabled", &set_enabled);
    load(xefg_module, "xefgSwapChainD3D12TagFrameResource", &tag);
    load(xefg_module, "xefgSwapChainTagFrameConstants", &constants);
    load(xefg_module, "xefgSwapChainSetPresentId", &present_id);
    load(xefg_module, "xefgSwapChainGetLastPresentStatus", &status);
    load(xefg_module, "xefgSwapChainDestroy", &destroy_fg);
    auto* native_queue = sdk_queue;
    renodx::utils::directx::NativeFromReShadeProxy(&native_queue);
    queue = native_queue;
    Check(queue != nullptr && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
              && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))),
          "actual Intel SDK uses this case's same native DIRECT submission queue");
    const auto image = GetModuleHandleW(nullptr);
    constexpr wchar_t CLASS_NAME[] = L"NrOriginalRuntimeIntelInputWitness";
    const WNDCLASSW window_class{.lpfnWndProc = DefWindowProcW, .hInstance = image, .lpszClassName = CLASS_NAME};
    registered_class = RegisterClassW(&window_class) != 0;
    Check(registered_class || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Intel SDK fixture window class");
    window = CreateWindowExW(WS_EX_NOACTIVATE, CLASS_NAME, L"NR Nexus offline SDK input witness",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(width), static_cast<int>(height),
                             nullptr, nullptr, image, nullptr);
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(window != nullptr && event != nullptr, "private offline Intel SDK output window");
    Check(create_latency(device.Get(), &latency) == XELL_RESULT_SUCCESS, "actual Intel XeLL context");
    Check(create_fg(device.Get(), &context) == XEFG_SWAPCHAIN_RESULT_SUCCESS, "actual Intel XeFG context");
    Check(bind_latency(context, latency) == XEFG_SWAPCHAIN_RESULT_SUCCESS, "actual SDK latency association");
    ComPtr<IDXGIFactory2> factory;
    Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "actual SDK output DXGI factory");
    const DXGI_SWAP_CHAIN_DESC1 swap_desc{.Width = width, .Height = height, .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1, 0}, .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .BufferCount = 3, .Scaling = DXGI_SCALING_STRETCH, .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD};
    const xefg_swapchain_d3d12_init_params_t init{.maxInterpolatedFrames = 1,
                                                  .uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS};
    Check(init_fg(context, window, &swap_desc, nullptr, queue.Get(), factory.Get(), &init)
              == XEFG_SWAPCHAIN_RESULT_SUCCESS,
          "actual Intel SDK initializes its original swapchain");
    Check(get_proxy(context, IID_PPV_ARGS(&proxy)) == XEFG_SWAPCHAIN_RESULT_SUCCESS,
          "actual Intel SDK swapchain interface");
    const xell_sleep_params_t sleep_mode{.bLowLatencyMode = 1};
    Check(set_sleep(latency, &sleep_mode) == XELL_RESULT_SUCCESS, "actual Intel SDK sleep mode");
    Check(set_composition(context, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED) == XEFG_SWAPCHAIN_RESULT_SUCCESS
              && set_enabled(context, 1) == XEFG_SWAPCHAIN_RESULT_SUCCESS,
          "actual Intel SDK enables mandatory HUDLess consumption");
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
              && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
              && SUCCEEDED(list->Close())
              && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "actual SDK input's owned native recording objects");
    const D3D12_RESOURCE_DESC texture_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                           .Width = width,
                                           .Height = height,
                                           .DepthOrArraySize = 1,
                                           .MipLevels = 1,
                                           .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                           .SampleDesc = {1, 0}};
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &allocation_bytes);
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC buffer_desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                          .Width = allocation_bytes,
                                          .Height = 1,
                                          .DepthOrArraySize = 1,
                                          .MipLevels = 1,
                                          .SampleDesc = {1, 0},
                                          .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))),
          "read back the exact same resource that the actual Intel SDK tags");
    receipt = {{"schema", "nr-original-runtime-intel-input/1"}, {"scope", "NATIVE_GPU+ACTUAL_INTEL_SDK_INPUT_ACCEPTANCE"}, {"claim", "Input resource/version/pixels and original API acceptance only; SDK internal first GPU read and display unobserved"}, {"xefg_module", reinterpret_cast<uintptr_t>(xefg_module)}, {"xell_module", reinterpret_cast<uintptr_t>(xell_module)}, {"xefg_path", xefg_path.string()}, {"xell_path", xell_path.string()}, {"xefg_loaded_path", std::filesystem::path(loaded_xefg).string()}, {"xell_loaded_path", std::filesystem::path(loaded_xell).string()}, {"context", reinterpret_cast<uintptr_t>(context)}, {"sdk_context_generation", 1}, {"queue", reinterpret_cast<uintptr_t>(queue.Get())}, {"device", reinterpret_cast<uintptr_t>(device.Get())}, {"format", static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM)}, {"width", width}, {"height", height}, {"frames", nlohmann::json::array()}};
    WriteReceipt();
  }

  void Accept(ID3D12Resource* edited, ID3D12Resource* depth, ID3D12Resource* motion,
              std::span<const uint8_t> expected_rgba, uint64_t source_epoch, uint64_t source_frame,
              const char* source_role) {
    Check(!finished && edited != nullptr && depth != nullptr && motion != nullptr, "actual Intel SDK accepts existing case resources");
    const auto desc = edited->GetDesc();
    Check(desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
              && expected_rgba.size() == static_cast<size_t>(width) * height * 4,
          "same candidate's actual current RGBA output matches this SDK context dimensions");
    ComPtr<ID3D12Device> source_device;
    Check(SUCCEEDED(edited->GetDevice(IID_PPV_ARGS(&source_device)))
              && renodx::utils::directx::SameNativeObject(source_device.Get(), device.Get()),
          "actual SDK input is on the same physical device, without transferring to a second device");
    WaitForGpu();
    Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)),
          "actual Intel SDK only resets a completed owned input recording");
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
      const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                           .Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}};
      list->ResourceBarrier(1, &barrier);
    };
    const auto api_frame = ++frame;
    Check(sleep(latency, api_frame) == XELL_RESULT_SUCCESS
              && marker(latency, api_frame, XELL_SIMULATION_START) == XELL_RESULT_SUCCESS
              && marker(latency, api_frame, XELL_SIMULATION_END) == XELL_RESULT_SUCCESS
              && marker(latency, api_frame, XELL_RENDERSUBMIT_START) == XELL_RESULT_SUCCESS,
          "actual Intel SDK frame markers are ordered for this application frame");
    ComPtr<ID3D12Resource> backbuffer;
    const auto backbuffer_index = proxy->GetCurrentBackBufferIndex();
    Check(SUCCEEDED(proxy->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer))), "actual Intel SDK current backbuffer");
    transition(edited, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer.Get(), edited);
    transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                  .PlacedFootprint = footprint};
    const D3D12_TEXTURE_COPY_LOCATION source{.pResource = edited,
                                             .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                             .SubresourceIndex = 0};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(edited, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (auto* resource : {depth, motion}) {
      transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    nlohmann::json tags = nlohmann::json::array();
    for (const auto& [role, resource, state] : {
             std::tuple{XEFG_SWAPCHAIN_RES_DEPTH, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
             std::tuple{XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
             std::tuple{XEFG_SWAPCHAIN_RES_BACKBUFFER, static_cast<ID3D12Resource*>(nullptr), D3D12_RESOURCE_STATE_PRESENT},
             std::tuple{XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, edited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}}) {
      const xefg_swapchain_d3d12_resource_data_t data{.type = role,
                                                      .validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT,
                                                      .resourceSize = {width, height},
                                                      .pResource = resource,
                                                      .incomingState = state};
      const auto result = tag(context, list.Get(), api_frame, &data);
      tags.push_back({{"role", static_cast<int32_t>(role)}, {"resource", reinterpret_cast<uintptr_t>(resource)}, {"subresource", 0}, {"state", static_cast<uint32_t>(state)}, {"result", static_cast<int32_t>(result)}});
      Check(result == XEFG_SWAPCHAIN_RESULT_SUCCESS, "actual Intel SDK original resource tag accepts this version");
    }
    xefg_swapchain_frame_constant_data_t data{};
    for (uint32_t index : {0u, 5u, 10u, 15u}) {
      data.viewMatrix[index] = data.projectionMatrix[index] = 1;
    }
    data.motionVectorScaleX = data.motionVectorScaleY = 1;
    data.resetHistory = (api_frame == 1 ? 1u : 0u);
    data.frameRenderTime = 16.67f;
    const auto constants_result = constants(context, api_frame, &data);
    Check(constants_result == XEFG_SWAPCHAIN_RESULT_SUCCESS && SUCCEEDED(list->Close()),
          "actual Intel SDK original constants and input list close");
    ID3D12CommandList* recorded = list.Get();
    queue->ExecuteCommandLists(1, &recorded);
    Check(SUCCEEDED(queue->Signal(fence.Get(), ++submitted)), "same native SDK queue completes this input recording");
    WaitForGpu();
    const D3D12_RANGE range{0, static_cast<SIZE_T>(allocation_bytes)};
    void* mapped = nullptr;
    Check(SUCCEEDED(readback->Map(0, &range, &mapped)), "map actual SDK-tagged input readback");
    std::vector<uint8_t> observed(expected_rgba.size());
    for (UINT row = 0; row < rows; ++row) {
      std::memcpy(observed.data() + static_cast<size_t>(row) * width * 4,
                  static_cast<const uint8_t*>(mapped) + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                  static_cast<size_t>(width) * 4);
    }
    const D3D12_RANGE unchanged{};
    readback->Unmap(0, &unchanged);
    Check(std::equal(observed.begin(), observed.end(), expected_rgba.begin()),
          "actual Intel SDK tagged resource carries the same qualified NR frame bytes");
    Check(marker(latency, api_frame, XELL_RENDERSUBMIT_END) == XELL_RESULT_SUCCESS
              && present_id(context, api_frame) == XEFG_SWAPCHAIN_RESULT_SUCCESS
              && marker(latency, api_frame, XELL_PRESENT_START) == XELL_RESULT_SUCCESS,
          "actual Intel SDK original Present selects the tagged application frame");
    const auto present_result = proxy->Present(1, 0);
    const auto present_end = marker(latency, api_frame, XELL_PRESENT_END);
    xefg_swapchain_present_status_t present_status{};
    const auto status_result = status(context, &present_status);
    receipt["frames"].push_back({{"sdk_application_frame", api_frame}, {"source_epoch", source_epoch}, {"source_base_frame", source_frame}, {"source_role", source_role}, {"source_resource", reinterpret_cast<uintptr_t>(edited)}, {"subresource", 0}, {"source_rgba_first", std::array{observed[0], observed[1], observed[2], observed[3]}}, {"source_rgba_bytes", observed.size()}, {"source_crc32", renodx::utils::hash::ComputeCRC32(observed.data(), observed.size())}, {"same_resource_same_input_bytes", true}, {"tags", tags}, {"constants_result", constants_result}, {"backbuffer", reinterpret_cast<uintptr_t>(backbuffer.Get())}, {"backbuffer_index", backbuffer_index}, {"input_list", reinterpret_cast<uintptr_t>(list.Get())}, {"input_fence", submitted}, {"present_result", static_cast<uint32_t>(present_result)}, {"present_end_marker_result", present_end}, {"status_result", status_result}, {"status_enabled", present_status.isFrameGenEnabled}, {"status_feature_result", present_status.frameGenResult}, {"status_outputs", present_status.framesPresented}});
    WriteReceipt();
    Check(SUCCEEDED(present_result) && present_end == XELL_RESULT_SUCCESS && status_result == XEFG_SWAPCHAIN_RESULT_SUCCESS,
          "actual Intel SDK original Present and status return accepted input");
    Check(SUCCEEDED(queue->Signal(fence.Get(), ++submitted)), "actual Intel SDK input follow-up completion");
    WaitForGpu();
    Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "restore owned case resource states");
    for (auto* resource : {edited, depth, motion}) {
      transition(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    Check(SUCCEEDED(list->Close()), "close actual SDK state restoration");
    recorded = list.Get();
    queue->ExecuteCommandLists(1, &recorded);
    Check(SUCCEEDED(queue->Signal(fence.Get(), ++submitted)), "actual SDK state restoration completion");
    WaitForGpu();
  }

  void Finish() {
    if (finished) return;
    WaitForGpu();
    proxy.Reset();
    const auto fg_result = destroy_fg(context);
    const auto latency_result = destroy_latency(latency);
    context = nullptr;
    latency = nullptr;
    finished = true;
    receipt["destroy_fg_result"] = fg_result;
    receipt["destroy_latency_result"] = latency_result;
    WriteReceipt();
    Check(fg_result == XEFG_SWAPCHAIN_RESULT_SUCCESS && latency_result == XELL_RESULT_SUCCESS,
          "actual Intel SDK contexts retire successfully");
  }

  ~Consumer() {
    try {
      Finish();
    } catch (...) {
    }
    if (event != nullptr) {
      CloseHandle(event);
    }
    if (window != nullptr) {
      DestroyWindow(window);
    }
    if (registered_class) {
      UnregisterClassW(L"NrOriginalRuntimeIntelInputWitness", GetModuleHandleW(nullptr));
    }
    if (xefg_module != nullptr) {
      FreeLibrary(xefg_module);
    }
    if (xell_module != nullptr) {
      FreeLibrary(xell_module);
    }
  }
};
}  // namespace original_intel_sdk_cases
