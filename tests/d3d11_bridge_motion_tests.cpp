#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
using Microsoft::WRL::ComPtr;
int ProbeFormats() {
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
  for (bool warp : {false, true}) {
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = warp ? factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) return 2;
    DXGI_ADAPTER_DESC1 info{};
    adapter->GetDesc1(&info);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &device, nullptr, &context);
    std::printf("%s adapter=%ls D3D11=0x%08X\n", warp ? "WARP" : "hardware", info.Description, unsigned(hr));
    if (FAILED(hr)) return 3;
    ComPtr<ID3D12Device> device12;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
    std::printf("same-adapter D3D12=0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 4;
    for (auto format : {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}) {
      for (UINT bind : {UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), UINT(D3D11_BIND_SHADER_RESOURCE)}) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 13; desc.Height = 9; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
        std::printf("format=%u bind=0x%X create=0x%08X", unsigned(format), bind, unsigned(hr));
        if (SUCCEEDED(hr)) {
          ComPtr<IDXGIResource1> resource;
          hr = texture.As(&resource);
          HANDLE handle = nullptr;
          if (SUCCEEDED(hr)) hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
          std::printf(" handle=0x%08X", unsigned(hr));
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D12Resource> opened;
            hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
            std::printf(" open12=0x%08X", unsigned(hr));
            CloseHandle(handle);
          }
        }
        std::printf("\n");
      }
    }
  }
  return 0;
}
#ifdef MOTION_FORMAT_PROBE_ONLY
int main() { return ProbeFormats(); }
#endif

#ifndef MOTION_FORMAT_PROBE_ONLY
#define DEBUG_LEVEL_0
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <nlohmann/json.hpp>
#include <dlssnr.hpp>
#include "inline_execution_trace.hpp"
// Reuse the established host types and checks, without executing its matrix.
#define main UnusedXeFgMotionHostMain
#define FG_XEFG_TEST_CUSTOM_LOGGER
#include "fg_xefg_tests.cpp"
#undef FG_XEFG_TEST_CUSTOM_LOGGER
#undef main
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  if (std::strstr(message, "DLSS5 FG interval ") != nullptr) {
    interval_diagnostic = message;
  }
  std::cout << message << '\n';
}
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char*, size_t* size) {
  *size = 0;
  return false;
}
namespace {
namespace native = renodx::addons::dlss5::internal;
namespace params = renodx::addons::dlss5::bridge;
std::vector<ComPtr<ID3D12Resource>> model_uploads;
uint32_t model_calls = 0;
std::vector<uint32_t> model_presets;
struct MotionModel {
  uint32_t preset;
  uint64_t id;
  uint32_t evaluations = 0;
};
uint64_t model_created = 0;
std::vector<uint64_t> model_released;
uint32_t fail_model_width_once = 0;
struct ModelObservation {
  int reset;
  float intensity;
  uint32_t style;
  float tone;
  uint64_t id;
  uint32_t evaluation;
  uint32_t width;
  uint32_t height;
  ID3D12Resource* depth;
  ID3D12Resource* control_mask;
};
std::vector<ModelObservation> model_observations;
uint32_t fail_model_call = 0;
std::array<unsigned short, 2> model_stamps{0x3400, 0x3800};
std::optional<std::array<unsigned short, 4>> model_rgba;
NVSDK_NGX_Handle* source_create_result = nullptr;
NVSDK_NGX_Result NVSDK_CONV MotionCreateSource(ID3D11DeviceContext*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  *handle = source_create_result;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionReleaseSource(NVSDK_NGX_Handle*) {
  return NVSDK_NGX_Result_Success;
}
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
native::DirectCreateFn bridge_native_create = nullptr;
native::DirectEvaluateFn bridge_native_evaluate = nullptr;
native::DirectReleaseFn bridge_native_release = nullptr;
native::DirectInitFn bridge_snippet_init = nullptr;
native::DirectShutdownFn bridge_snippet_shutdown = nullptr;
ID3D12Device* bridge_snippet_initialized_device = nullptr;  // Borrowed from the still-live private Bridge.
uint32_t bridge_snippet_init_calls = 0, bridge_snippet_shutdown_calls = 0;
NVSDK_NGX_Result bridge_snippet_init_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
std::unordered_map<const NVSDK_NGX_Handle*, uint32_t> bridge_native_evaluations;
uint64_t bridge_native_sr_calls = 0;
std::string bridge_private_project_id;
void NVSDK_CONV BridgeSdkLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature) {
  std::string text = (message != nullptr ? message : "");
  if (!bridge_private_project_id.empty()) {
    for (size_t offset = 0; (offset = text.find(bridge_private_project_id, offset)) != std::string::npos;) {
      text.replace(offset, bridge_private_project_id.size(), "[ProjectID]");
      offset += std::strlen("[ProjectID]");
    }
  }
  std::cerr << "NGX SDK level=" << static_cast<uint32_t>(level) << " feature=" << static_cast<uint32_t>(feature)
      << " " << text << std::endl;
}
NVSDK_NGX_Result NVSDK_CONV BridgeSnippetInit(unsigned long long application_id, const wchar_t* data_path,
    ID3D12Device* device, NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* parameters) {
  ++bridge_snippet_init_calls;
  const auto begin = native::SteadyNowNs();
  const auto result = bridge_snippet_init(application_id, data_path, device, version, parameters);
  bridge_snippet_init_result = result;
  if (NVSDK_NGX_SUCCEED(result)) {
    bridge_snippet_initialized_device = device;
  }
  std::cout << nlohmann::json{{"event", "nr_snippet_init_ext"}, {"api_result", static_cast<uint32_t>(result)},
      {"cpu_ms", (native::SteadyNowNs() - begin) / 1'000'000.}, {"private_device", device == native::bridge.device12},
      {"production_application_identity", application_id == native::kDirectApplicationId},
      {"identity_iat_installed", native::get_module_filename_iat != nullptr}}.dump() << '\n';
  return result;
}
NVSDK_NGX_Result NVSDK_CONV BridgeSnippetShutdown(ID3D12Device* device) {
  ++bridge_snippet_shutdown_calls;
  const auto begin = native::SteadyNowNs();
  const auto result = bridge_snippet_shutdown(device);
  if (NVSDK_NGX_SUCCEED(result) && device == bridge_snippet_initialized_device) {
    bridge_snippet_initialized_device = nullptr;
  }
  std::cout << nlohmann::json{{"event", "nr_snippet_shutdown"}, {"api_result", static_cast<uint32_t>(result)},
      {"cpu_ms", (native::SteadyNowNs() - begin) / 1'000'000.}, {"private_device", device == native::bridge.device12}}.dump() << '\n';
  return result;
}
NVSDK_NGX_Result NVSDK_CONV BridgeNativeCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  const auto begin = native::SteadyNowNs();
  const auto result = bridge_native_create(list, feature, parameters, handle);
  if (NVSDK_NGX_SUCCEED(result) && handle != nullptr && *handle != nullptr) {
    ++model_created;
  }
  std::cout << nlohmann::json{{"event", "nr_create"}, {"api_result", static_cast<uint32_t>(result)},
      {"feature", static_cast<uint32_t>(feature)}, {"requested_preset", native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset")},
      {"cpu_ms", (native::SteadyNowNs() - begin) / 1'000'000.},
      {"handle", reinterpret_cast<uintptr_t>(handle != nullptr ? *handle : nullptr)}}.dump() << '\n';
  return result;
}
NVSDK_NGX_Result NVSDK_CONV BridgeNativeEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
  ++model_calls;
  auto* color = native::GetD3D12Resource(parameters, "DLSSNR.Color");
  model_observations.push_back({native::GetInt(parameters, "DLSSNR.Reset"), native::GetFloat(parameters, "DLSSNR.Intensity"),
      native::GetUInt(parameters, "DLSSNR.Style"), native::GetFloat(parameters, "DLSSNR.LocalToneStrength"),
      reinterpret_cast<uintptr_t>(handle), ++bridge_native_evaluations[handle],
      static_cast<uint32_t>(color->GetDesc().Width), color->GetDesc().Height,
      native::GetD3D12Resource(parameters, "DLSSNR.Depth"), native::GetD3D12Resource(parameters, "DLSSNR.ControlMask")});
  const auto begin = native::SteadyNowNs();
  const auto result = bridge_native_evaluate(list, handle, parameters, progress);
  std::cout << nlohmann::json{{"event", "nr_evaluate"}, {"api_result", static_cast<uint32_t>(result)},
      {"width", model_observations.back().width}, {"height", model_observations.back().height},
      {"reset", model_observations.back().reset}, {"cpu_ms", (native::SteadyNowNs() - begin) / 1'000'000.},
      {"handle", reinterpret_cast<uintptr_t>(handle)}}.dump() << '\n';
  return result;
}
NVSDK_NGX_Result NVSDK_CONV BridgeNativeRelease(NVSDK_NGX_Handle* handle) {
  const auto result = bridge_native_release(handle);
  if (NVSDK_NGX_SUCCEED(result)) {
    model_released.push_back(reinterpret_cast<uintptr_t>(handle));
    bridge_native_evaluations.erase(handle);
  }
  std::cout << nlohmann::json{{"event", "nr_release"}, {"api_result", static_cast<uint32_t>(result)},
      {"handle", reinterpret_cast<uintptr_t>(handle)}}.dump() << '\n';
  return result;
}
NVSDK_NGX_Result NVSDK_CONV BridgeNativeSrEvaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
  ++bridge_native_sr_calls;
  const auto begin = native::SteadyNowNs();
  const auto result = NVSDK_NGX_D3D11_EvaluateFeature(context, handle, parameters, progress);
  std::cout << nlohmann::json{{"event", "sr_evaluate"}, {"api_result", static_cast<uint32_t>(result)},
      {"cpu_ms", (native::SteadyNowNs() - begin) / 1'000'000.}}.dump() << '\n';
  return result;
}
#endif
NVSDK_NGX_Result NVSDK_CONV MotionCreateModel(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(new MotionModel{
      .preset = native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset"), .id = ++model_created});
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionReleaseModel(NVSDK_NGX_Handle* handle) {
  model_released.push_back(reinterpret_cast<MotionModel*>(handle)->id);
  delete reinterpret_cast<MotionModel*>(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionEvaluateModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  ++model_calls;
  auto* model = reinterpret_cast<MotionModel*>(const_cast<NVSDK_NGX_Handle*>(handle));
  model_presets.push_back(model->preset);
  model_observations.push_back({native::GetInt(parameters, "DLSSNR.Reset"),
      native::GetFloat(parameters, "DLSSNR.Intensity"), native::GetUInt(parameters, "DLSSNR.Style"),
      native::GetFloat(parameters, "DLSSNR.LocalToneStrength"), model->id, ++model->evaluations,
      static_cast<uint32_t>(native::GetD3D12Resource(parameters, "DLSSNR.Color")->GetDesc().Width),
      native::GetD3D12Resource(parameters, "DLSSNR.Color")->GetDesc().Height,
      native::GetD3D12Resource(parameters, "DLSSNR.Depth"),
      native::GetD3D12Resource(parameters, "DLSSNR.ControlMask")});
  if (model_calls == fail_model_call) return NVSDK_NGX_Result_Fail;
  if (fail_model_width_once != 0 && model_observations.back().width == fail_model_width_once) {
    fail_model_width_once = 0;
    return NVSDK_NGX_Result_Fail;
  }
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  Check(output != nullptr, "model output is a real D3D12 resource");
  const auto input_desc = native::GetD3D12Resource(parameters, "DLSSNR.Color")->GetDesc();
  Check(input_desc.Width == output->GetDesc().Width && input_desc.Height == output->GetDesc().Height,
      "bridge NR input and output have equal actual working dimensions");
  auto* device = native::bridge.device12;
  const auto desc = output->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_RESOURCE_DESC buffer{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
  ComPtr<ID3D12Resource> upload;
  Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))), "model upload");
  void* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "model upload map");
  std::memset(mapped, 0, static_cast<size_t>(bytes));
  Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "model output fixture format");
  // Distinct finite model results make both stages observable even at 1:1.
  const unsigned short model_pixel = model_stamps[model_presets.back() == 2 ? 1 : 0];
  for (UINT y = 0; y < desc.Height; ++y) {
    auto* row = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
    if (model_rgba.has_value()) {
      for (UINT x = 0; x < desc.Width; ++x) {
        std::copy(model_rgba->begin(), model_rgba->end(), row + x * 4);
      }
    } else {
      std::fill_n(row, desc.Width * 4, model_pixel);
    }
  }
  upload->Unmap(0, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_uploads.push_back(upload);
  return NVSDK_NGX_Result_Success;
}
template <typename Fence>
void WaitForGpu(Fence* fence, uint64_t value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr, "GPU event");
  const HRESULT hr = fence->SetEventOnCompletion(value, event);
  const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
  CloseHandle(event);
  Check(wait == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX, "bounded real GPU completion");
}
std::vector<unsigned char> Read11(ID3D11Texture2D* texture, UINT pixel_bytes) {
  auto desc = D3D11_TEXTURE2D_DESC{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stage;
  Check(SUCCEEDED(native::bridge.device11->CreateTexture2D(&desc, nullptr, &stage)), "D3D11 readback stage");
  native::bridge.context11->CopyResource(stage.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(SUCCEEDED(native::bridge.context11->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 readback map");
  std::vector<unsigned char> result(size_t(desc.Width) * desc.Height * pixel_bytes);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(result.data() + size_t(y) * desc.Width * pixel_bytes,
        static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * pixel_bytes);
  }
  native::bridge.context11->Unmap(stage.Get(), 0);
  return result;
}
std::vector<unsigned char> Read12(ID3D12Resource* texture, UINT pixel_bytes,
    D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON) {
  auto& bridge = native::bridge;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  Check(SUCCEEDED(bridge.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "readback allocator");
  Check(SUCCEEDED(bridge.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "readback list");
  const auto desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  bridge.device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_RESOURCE_DESC buffer{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
  ComPtr<ID3D12Resource> readback;
  Check(SUCCEEDED(bridge.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "D3D12 readback buffer");
  native::Transition(list.Get(), texture, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
  Check(SUCCEEDED(list->Close()), "readback close");
  ID3D12CommandList* lists[]{list.Get()};
  bridge.queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  Check(SUCCEEDED(bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "readback fence");
  Check(SUCCEEDED(bridge.queue->Signal(fence.Get(), 1)), "readback GPU signal");
  WaitForGpu(fence.Get(), 1);
  void* mapped = nullptr;
  Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "D3D12 readback map");
  std::vector<unsigned char> result(size_t(desc.Width) * desc.Height * pixel_bytes);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(result.data() + size_t(y) * desc.Width * pixel_bytes,
        static_cast<const unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
        size_t(desc.Width) * pixel_bytes);
  }
  readback->Unmap(0, nullptr);
  return result;
}
}
#include "d3d11_present_stage_resolution_cases.hpp"
int main(int argc, char** argv) {
  try {
    bool warp = false;
    bool dlaa_only = false;
    bool sr_only = false;
    bool stage_resolution = false;
    bool same_stage_resolution = false;
    bool render_stage_resolution = false;
    bool present_stage_resolution = false;
    bool ordinary_watchdog = false;
    bool edit_reuse_production = false;
    bool adaptive_case = false;
    bool adaptive_hardware = false;
    bool adaptive_native_ngx = false;
    bool adaptive_native_snippet = false;
    const char* adaptive_project_id_path = nullptr;
    for (int arg = 1; arg < argc; ++arg) {
      if (std::strcmp(argv[arg], "--warp") == 0) {
        warp = true;
      } else if (std::strcmp(argv[arg], "--dlaa") == 0) {
        dlaa_only = true;
      } else if (std::strcmp(argv[arg], "--sr") == 0) {
        sr_only = true;
      } else if (std::strcmp(argv[arg], "--ordinary-watchdog") == 0) {
        ordinary_watchdog = true;
      } else if (std::strcmp(argv[arg], "--edit-reuse-production") == 0) {
        edit_reuse_production = true;
      } else if (std::strcmp(argv[arg], "--adaptive") == 0) {
        Check(!adaptive_case, "one explicit Bridge Adaptive condition per process");
        adaptive_case = true;
      } else if (std::strcmp(argv[arg], "--adaptive-hardware") == 0) {
        Check(!adaptive_case, "one explicit Bridge Adaptive condition per process");
        adaptive_case = adaptive_hardware = true;
      } else if (std::strcmp(argv[arg], "--adaptive-native-ngx") == 0 || std::strcmp(argv[arg], "--adaptive-native-snippet") == 0) {
        Check(!adaptive_case && arg + 1 < argc, "Bridge native NGX requires its existing ProjectID file argument");
        adaptive_case = adaptive_hardware = adaptive_native_ngx = true;
        adaptive_native_snippet = std::strcmp(argv[arg], "--adaptive-native-snippet") == 0;
        adaptive_project_id_path = argv[++arg];
#if !defined(NR_NEXUS_TEST_NATIVE_NGX)
        Check(false, "Bridge native NGX requires the SDK-enabled build; no MOCK fallback");
#endif
      } else if (std::strcmp(argv[arg], "--stage-resolution") == 0) {
        stage_resolution = true;
      } else if (std::strcmp(argv[arg], "--same-stage-resolution") == 0) {
        stage_resolution = true;
        same_stage_resolution = true;
      } else if (std::strcmp(argv[arg], "--present-stage-resolution") == 0) {
        present_stage_resolution = true;
      } else if (std::strcmp(argv[arg], "--render-stage-resolution") == 0) {
        stage_resolution = true;
        same_stage_resolution = true;
        render_stage_resolution = true;
      } else {
        Check(false, "known GPU fixture argument");
      }
    }
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    native::addon_module = GetModuleHandleW(nullptr);
#if defined(NR_NEXUS_TEST_WARP_ONLY)
    Check(warp, "hardware adapter forbidden in WARP-only Bridge test build");
#endif
    const bool strict_debug = ordinary_watchdog || edit_reuse_production || adaptive_case || (warp && sr_only);
    ComPtr<ID3D12Debug> adaptive_debug;
    if (ordinary_watchdog) {
      Check(!adaptive_case && !stage_resolution && !present_stage_resolution && !dlaa_only && !sr_only,
          "ordinary Bridge watchdog runs without other stage variants");
    }
    if (edit_reuse_production) {
      Check(warp && !ordinary_watchdog && !adaptive_case && !stage_resolution && !present_stage_resolution && !dlaa_only && !sr_only,
          "production Bridge x2 uses WARP without other stage variants");
    }
    if (adaptive_case) {
      Check(warp != adaptive_hardware && !stage_resolution && !present_stage_resolution && !dlaa_only && !sr_only,
          "Bridge Adaptive explicitly selects WARP or hardware, without other stage variants or fallback");
    }
    if (strict_debug) {
      Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&adaptive_debug))), "Bridge requires D3D12 Debug Layer");
      adaptive_debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    Check(SUCCEEDED(warp ? factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(0, &adapter)), "adapter");
    if (adaptive_case || warp) {
      DXGI_ADAPTER_DESC1 software{};
      Check(SUCCEEDED(adapter->GetDesc1(&software)), "Bridge Adaptive adapter description");
      if (adaptive_hardware) {
        Check((software.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && software.VendorId == 0x10de,
            "Bridge Adaptive hardware resolves an NVIDIA non-SOFTWARE adapter before device creation");
      } else {
        Check((software.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
            "Bridge resolves a SOFTWARE adapter before device creation, with no hardware fallback");
      }
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        strict_debug ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "D3D11 device");
    if (warp && sr_only) {
      D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{.InFormat = DXGI_FORMAT_R32G32_FLOAT};
      const auto result = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support));
      std::printf("BRIDGE_SR format=R32G32_FLOAT format_support2_hr=0x%08X support2=0x%X shareable_bit=%u query_valid=%u\n",
          unsigned(result), support.OutFormatSupport2,
          unsigned((support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) != 0), unsigned(SUCCEEDED(result)));
    }
    if (!adaptive_native_snippet) {
      native::direct_api.module = GetModuleHandleW(nullptr);
      native::direct_api.create = MotionCreateModel;
      native::direct_api.evaluate = MotionEvaluateModel;
      native::direct_api.release = MotionReleaseModel;
      native::direct_load_state = native::DirectLoadState::Ready;
    }
    // Provider setup is explicit below; do not eagerly prime from device callbacks.
    native::hooks_enabled.store(false);
    Check(native::EnsureBridgeUp(context.Get()) == native::kBridgeReady, "production bridge bring-up");
    ComPtr<ID3D11InfoQueue> adaptive_info11;
    ComPtr<ID3D12InfoQueue> adaptive_info12;
    if (strict_debug) {
      Check(SUCCEEDED(device.As(&adaptive_info11))
          && SUCCEEDED(native::bridge.device12->QueryInterface(IID_PPV_ARGS(&adaptive_info12))),
          "Bridge requires both real D3D11 and D3D12 InfoQueues");
      std::cout << (edit_reuse_production ? "BRIDGE_EDIT_REUSE_PRODUCTION"
          : ordinary_watchdog ? "BRIDGE_ORDINARY_WATCHDOG" : adaptive_case ? "BRIDGE_ADAPTIVE" : "BRIDGE_SR")
          << " debug11=1 debug12=1 info11=1 info12=1 adapter="
          << (warp ? "SOFTWARE" : "HARDWARE")
          << " runtime=" << (adaptive_native_snippet ? "PRODUCTION_SNIPPET_INIT"
              : (adaptive_native_ngx ? "SDK_PREINITIALIZED_NGX" : "MOCK")) << '\n';
    }
    const auto check_debug_messages = [&] {
      if (!strict_debug) return;
      uint64_t errors11 = 0;
      uint64_t errors12 = 0;
      for (UINT64 index = 0; index < adaptive_info11->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
        SIZE_T bytes = 0;
        Check(SUCCEEDED(adaptive_info11->GetMessage(index, nullptr, &bytes)), "D3D11 Debug message size read");
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        Check(SUCCEEDED(adaptive_info11->GetMessage(index, message, &bytes)), "D3D11 Debug message read");
        if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
          ++errors11;
          std::cerr << "Bridge D3D11 Debug id=" << unsigned(message->ID)
                    << " severity=" << unsigned(message->Severity) << ": " << message->pDescription << '\n';
        }
      }
      for (UINT64 index = 0; index < adaptive_info12->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
        SIZE_T bytes = 0;
        Check(SUCCEEDED(adaptive_info12->GetMessage(index, nullptr, &bytes)), "D3D12 Debug message size read");
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(adaptive_info12->GetMessage(index, message, &bytes)), "D3D12 Debug message read");
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
          ++errors12;
          std::cerr << "Bridge D3D12 Debug id=" << unsigned(message->ID)
                    << " severity=" << unsigned(message->Severity) << ": " << message->pDescription << '\n';
        }
      }
      std::cout << "Bridge InfoQueue error_or_corruption D3D11=" << errors11 << " D3D12=" << errors12
                << " total=" << errors11 + errors12 << '\n';
      Check(errors11 == 0 && errors12 == 0, "Bridge D3D11/D3D12 InfoQueues have no errors/corruption");
      std::cout << "PASS Bridge Debug/InfoQueue error=0 corruption=0\n";
    };
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    const auto luid12 = native::bridge.device12->GetAdapterLuid();
    Check(luid12.HighPart == adapter_desc.AdapterLuid.HighPart && luid12.LowPart == adapter_desc.AdapterLuid.LowPart,
        "D3D11 and D3D12 adapter LUID must match");
    std::printf("GPU %s adapter=%ls LUID=%08X:%08X same-adapter=PASS\n", warp ? "WARP" : "hardware",
        adapter_desc.Description, unsigned(luid12.HighPart), luid12.LowPart);
    if (!adaptive_native_snippet) {
      native::direct_device = native::bridge.device12;
      native::direct_device->AddRef();
      native::direct_device_native = native::bridge.device12;
      native::parameter_runtime_ready = true;
      native::core_allocate_parameters = params::AllocateOwnedParameters;
      native::core_destroy_parameters = params::DestroyOwnedParameters;
    }
    native::enabled.store(true);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::debug_bypass_host_look.store(true);
    native::detail_stability.store(1);
    native::look_mode.store(false);
    const auto make_texture = [&](UINT width, UINT height, DXGI_FORMAT format, UINT bind,
                                  const void* data, UINT pitch) {
      D3D11_TEXTURE2D_DESC desc{.Width = width, .Height = height, .MipLevels = 1, .ArraySize = 1,
          .Format = format, .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind};
      D3D11_SUBRESOURCE_DATA initial{.pSysMem = data, .SysMemPitch = pitch};
      ComPtr<ID3D11Texture2D> texture;
      Check(SUCCEEDED(device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture)), "input texture");
      return texture;
    };
    if (present_stage_resolution) {
      return d3d11_present_stage_resolution_cases::Run(context.Get(), make_texture);
    }
    // Non-multiples of eight exercise dispatch edges; selected values cannot survive a float16 detour.
    const UINT width = (edit_reuse_production ? inline_execution_trace::WIDTH : adaptive_hardware ? 1280u : 13u);
    const UINT height = (edit_reuse_production ? inline_execution_trace::HEIGHT : adaptive_hardware ? 720u : 9u);
    const UINT adaptive_output_width = (edit_reuse_production ? width : adaptive_hardware ? 1920u : 17u);
    const UINT adaptive_output_height = (edit_reuse_production ? height : adaptive_hardware ? 1080u : 11u);
    const float values[]{0.f, -0.f, 0.1f, -0.375f, 65537.f, -131071.25f, 1.234567f, -8388607.f};
    std::vector<float> motion_pixels(width * height * 2);
    for (size_t i = 0; i < motion_pixels.size(); ++i) motion_pixels[i] = values[i % std::size(values)];
    std::vector<unsigned short> rg16_pixels(width * height * 2, 0x3400);
    std::vector<unsigned short> color_pixels(width * height * 4, 0x3800);
    if (edit_reuse_production) {
      std::fill(rg16_pixels.begin(), rg16_pixels.end(), 0);
      model_rgba.emplace();
      std::transform(inline_execution_trace::MODEL_RGBA.begin(), inline_execution_trace::MODEL_RGBA.end(), model_rgba->begin(),
          DirectX::PackedVector::XMConvertFloatToHalf);
    }
    if (adaptive_hardware) {
      std::fill(rg16_pixels.begin(), rg16_pixels.end(), 0);
      for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
          const auto pixel = size_t(y) * width + x;
          const float u = static_cast<float>(x) / (width - 1), v = static_cast<float>(y) / (height - 1);
          color_pixels[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.12f + .65f * u);
          color_pixels[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .65f * v);
          color_pixels[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.15f + .5f * (1.f - u * v));
          color_pixels[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
        }
      }
    }
    auto color = make_texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, color_pixels.data(), width * 8);
    std::vector<unsigned short> output_pixels(adaptive_output_width * adaptive_output_height * 4, 0x3800);
    if (edit_reuse_production) {
      output_pixels.assign(inline_execution_trace::SOURCE_RGBA.begin(), inline_execution_trace::SOURCE_RGBA.end());
    }
    if (adaptive_hardware) {
      for (UINT y = 0; y < adaptive_output_height; ++y) {
        for (UINT x = 0; x < adaptive_output_width; ++x) {
          const auto pixel = size_t(y) * adaptive_output_width + x;
          output_pixels[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.12f + .65f * x / (adaptive_output_width - 1));
          output_pixels[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .65f * y / (adaptive_output_height - 1));
          output_pixels[pixel * 4 + 2] = 0x3400;
          output_pixels[pixel * 4 + 3] = 0x3c00;
        }
      }
    }
    auto output = make_texture(adaptive_output_width, adaptive_output_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, output_pixels.data(), adaptive_output_width * 8);
    auto motion = make_texture(width, height, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, motion_pixels.data(), width * 8);
    auto rg16 = make_texture(width, height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, rg16_pixels.data(), width * 4);
    std::vector<float> depth_pixels(width * height, (edit_reuse_production ? .5f : .375f));
    if (adaptive_hardware) {
      for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) depth_pixels[size_t(y) * width + x] = .2f + .6f * y / (height - 1);
      }
    }
    auto depth_direct = make_texture(width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth_pixels.data(), width * 4);
    struct StallRelease {
      ID3D12Fence* gate;
      ~StallRelease() {
        if (gate != nullptr) {
          gate->Signal(1);
        }
      }
    };
    if (ordinary_watchdog || edit_reuse_production) {
      native::d3d11_swapchain_seen.store(true);
      native::dx11_route.store(native::kDx11RouteNative);
      native::dedupe_mode.store(renodx::addons::dlss5::kDedupePerEvaluate);
      native::hook_point.store(native::kHookUpscaled);
      native::preset.store(edit_reuse_production ? 2 : 1);
      native::intensity.store(1.f);
      native::nr_follow_input_res.store(false);
      native::nr_resolution_scale.store(1.f);
      native::nr_adaptive_enabled.store(false);
      native::edit_reuse_enabled.store(edit_reuse_production);
      native::ngx_loader_staging.nr_ready = true;
      native::ngx_loader_staging.core_ready = true;
      native::PublishNgxLoaderSymbols();
      if (edit_reuse_production) {
        native::feed_mode.store(1);
        native::norm_governor.store(0);
        native::use_auto_mask.store(false);
        native::direct_runtime_requirements.dual_models = true;
        native::direct_runtime_requirements.file_version = "310.9.16.0";
        native::native_fg_public_off.store(true);
        native::native_fg_declaration.store(native::NativeFgDeclaration::kOff);

        D3D11_TEXTURE2D_DESC desc{};
        output->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> shared11;
        ComPtr<ID3D12Resource> shared12;
        const HRESULT shared = native::CreateSharedTwin(desc, desc.Format,
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &shared11, &shared12);
        if (FAILED(shared)) {
          std::fprintf(stderr, "UNVERIFIED edit-reuse-production: WARP FP16 Output sharing failed (0x%08X)\n", unsigned(shared));
          native::ReleaseBridge();
          return 1;
        }
      }

      params::OwnedParameters parameters;
      const auto setup_parameters = [&](bool jitter_y) {
        parameters.Set(NVSDK_NGX_Parameter_Width, width);
        parameters.Set(NVSDK_NGX_Parameter_Height, height);
        parameters.Set(NVSDK_NGX_Parameter_OutWidth, adaptive_output_width);
        parameters.Set(NVSDK_NGX_Parameter_OutHeight, adaptive_output_height);
        parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
            int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        if (edit_reuse_production) {
          parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
          parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
          parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
          if (jitter_y) parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
        }
      };
      setup_parameters(true);
      auto* handle = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD511});
      if (edit_reuse_production) {
        source_create_result = handle;
        native::ngx11_slot_real[0].create = reinterpret_cast<void*>(&MotionCreateSource);
        const auto created = native::HookedCreateFeature11Slot<0>(context.Get(), native::kFeatureDlss, &parameters, &handle);
        Check(NVSDK_NGX_SUCCEED(created), "production D3D11 Create wrapper registers the mock SR");
        const auto& source = native::create_contracts.at(handle);
        Check(source.source_type_confirmed && source.source_generation != 0
                  && source.source_device11_identity.Get() == renodx::utils::directx::NativeIdentity(device.Get())
                  && native::features.at(handle).sr_sequence == 0,
            "registered Bridge source has the original D3D11 device and an unused attempt sequence");
      } else {
        const params::ParameterView create_view(&parameters, nullptr);
        native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &create_view);
      }
      const auto frame = [&](bool sr_succeeds = true) {
        ++native::present_generation;
        const auto sequence_before = native::features.at(handle).sr_sequence;
        uint32_t real_calls = 0;
        const auto mock_sr = [&] {
          ++real_calls;
          context->UpdateSubresource(output.Get(), 0, nullptr, output_pixels.data(), adaptive_output_width * 8, 0);
          return (sr_succeeds ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail);
        };
        const auto result = native::Ngx11Evaluate(0, "ordinary Bridge SR", context.Get(), handle, &parameters, [&] {
          return (edit_reuse_production
              ? native::Ngx11Evaluate(0, "nested Bridge SR mirror", context.Get(), handle, &parameters, mock_sr) : mock_sr());
        });
        Check(real_calls == 1 && NVSDK_NGX_SUCCEED(result) == sr_succeeds, "Bridge preserves the original MOCK SR result");
        if (edit_reuse_production) {
          const auto& source = native::features.at(handle);
          const auto& plan = native::last_source_plan;
          Check(source.sr_sequence == sequence_before + 1 && plan.source_handle == handle
                    && plan.source_generation == source.source_generation && plan.plan.valid
                    && plan.plan.At(renodx::addons::dlss5::stage_plan::Stage::UPSCALED).count == 1,
              "one outer SR attempt advances the registered source and publishes its one-pass Upscaled plan");
        }
      };

      auto* const raw_context = native::bridge.context11;
      const auto original = Read11(output.Get(), 8);
      if (edit_reuse_production) {
        Check(Read11(color.Get(), 8) != original, "Color and the original SR Output contain distinguishable RGBA");
      }
      auto consumer = make_texture(adaptive_output_width, adaptive_output_height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, nullptr, 0);
      ComPtr<ID3D11Device5> fence_device;
      ComPtr<ID3D11Fence> consumer_tail;
      Check(SUCCEEDED(device.As(&fence_device))
                && SUCCEEDED(fence_device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer_tail))),
          "ordinary Bridge D3D11 consumer fence");
      uint64_t consumer_value = 0;
      const auto consume_output = [&] {
        raw_context->CopyResource(consumer.Get(), output.Get());
        Check(SUCCEEDED(raw_context->Signal(consumer_tail.Get(), ++consumer_value)), "signal after the real D3D11 consumer copy");
        raw_context->Flush();
        WaitForGpu(consumer_tail.Get(), consumer_value);
        return Read11(consumer.Get(), 8);
      };

      {
        // The stalled twin still contains the SR snapshot. An early copy can be byte-identical.
        using CopyRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Resource*, UINT, UINT, UINT,
            UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        static CopyRegion original_copy = nullptr;
        static ID3D11DeviceContext4* observed_context = raw_context;
        static ID3D11Resource* observed_output = output.Get();
        static ID3D11Resource* observed_motion = rg16.Get();
        static ID3D11Resource* observed_depth = depth_direct.Get();
        static uint32_t bridge_copies = 0;
        static uint32_t motion_copies = 0;
        static uint32_t depth_copies = 0;
        static uint32_t output_uploads = 0;
        static uint32_t output_copies = 0;
        renodx::utils::vtable::Slot<CopyRegion> copy_slot{
            .object = raw_context,
            .index = 46,  // ID3D11DeviceContext4 inherits CopySubresourceRegion at this ABI slot.
            .original = &original_copy,
            .replacement = [](ID3D11DeviceContext4* observed, ID3D11Resource* dst, UINT dst_subresource, UINT x, UINT y,
                               UINT z, ID3D11Resource* src, UINT src_subresource, const D3D11_BOX* box) {
              original_copy(observed, dst, dst_subresource, x, y, z, src, src_subresource, box);
              if (observed == observed_context) {
                ++bridge_copies;
                if (src == observed_motion) {
                  ++motion_copies;
                } else if (src == observed_depth) {
                  ++depth_copies;
                } else if (src == observed_output) {
                  ++output_uploads;
                } else if (dst == observed_output) {
                  ++output_copies;
                }
              }
            }};
        renodx::utils::vtable::Install(&copy_slot);
        struct RestoreCopySlot {
          decltype(copy_slot)* slot;
          ~RestoreCopySlot() { renodx::utils::vtable::Uninstall(slot); }
        } restore_copy_slot{.slot = &copy_slot};

        uint32_t warm_copies = 0;
        uint64_t warm_waits = 0;
        uint64_t warm_wait_ns = 0;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 2 && model_calls == 0; ++attempt) {
          const auto copies_before = output_copies;
          const auto waits_before = native::bridge_completion_waits;
          const auto wait_ns_before = native::bridge_completion_wait_ns;
          frame();
          if (const auto submitted = native::bridge_submitted_value.load(); submitted != 0) {
            WaitForGpu(native::bridge.fence_done12, submitted);
          }
          warm_copies = output_copies - copies_before;
          warm_waits = native::bridge_completion_waits - waits_before;
          warm_wait_ns = native::bridge_completion_wait_ns - wait_ns_before;
        }
        const auto warm_pixels = Read11(output.Get(), 8);
        std::cout << "BRIDGE_ORDINARY_WATCHDOG warm_model_calls=" << model_calls << " warm_copies=" << warm_copies
                  << " completion_waits=" << warm_waits << " completion_wait_ns=" << warm_wait_ns << '\n';
        Check(model_calls == 1 && warm_copies == 1, "completed ordinary NR issues one observed game Output copy");
        Check(std::memcmp(warm_pixels.data(), original.data(), 3 * sizeof(uint16_t)) != 0,
            "completed ordinary NR changes game Output RGB");

        const auto edited_pixels = [&](const std::vector<unsigned char>& pixels) {
          Check(pixels.size() == original.size() && pixels != original, "the D3D11 consumer reads a nonzero NR edit");
          for (size_t offset = 6; offset < pixels.size(); offset += 8) {
            Check(std::memcmp(pixels.data() + offset, original.data() + offset, sizeof(uint16_t)) == 0,
                "the D3D11 consumer preserves every pixel's original FP16 alpha");
          }
        };
        const auto expect_frame = [&](uint32_t calls, int reset) {
          const auto copies_before = output_copies;
          const auto calls_before = model_calls;
          frame();
          Check(model_calls - calls_before == calls && output_copies - copies_before == 1,
              "each complete Bridge frame copies back once, including zero-Evaluate reuse");
          const auto done = native::bridge.fence_done12->GetCompletedValue();
          Check(done != UINT64_MAX && done >= native::bridge_submitted_value.load(),
              "each Bridge copy-back has real D3D12 done");
          if (calls != 0 && reset >= 0) {
            Check(model_observations.back().reset == reset, "Bridge model consumes the expected History Reset");
          }
          auto pixels = consume_output();
          edited_pixels(pixels);
          return pixels;
        };
        using native::EditReuseBlock;
        using native::EditReuseDelivery;
        constexpr auto upscaled = renodx::addons::dlss5::stage_plan::Stage::UPSCALED;
        const auto monitor_matches = [&](EditReuseDelivery delivery, EditReuseBlock reason = EditReuseBlock::NONE) {
          const auto& feature = native::features.at(handle);
          const auto& latest = feature.edit_reuse_monitor[0];
          if (latest.sequence != feature.sr_sequence || latest.source_generation != feature.source_generation
              || latest.configuration != native::configuration_generation.load()
              || latest.reset_epoch != native::history_reset_epoch.load() || latest.stage != upscaled
              || latest.device != renodx::utils::directx::NativeIdentity(device.Get())
              || latest.handed_off != (delivery != EditReuseDelivery::NONE)
              || latest.delivery != delivery || latest.reason != reason) return false;
          if (delivery == EditReuseDelivery::REFRESH) return true;
          // The monitor displays delivered reuse, while refresh only updates the frame receipt.
          const auto observed = native::ReadDeliveredEditReuse(feature.edit_reuse_monitor, feature.source_generation,
              feature.sr_sequence, upscaled, native::SteadyNowNs());
          return observed.delivery == delivery && observed.reason == reason;
        };
        if (edit_reuse_production) {
          const auto& warm_feature = native::features.at(handle);
          const auto& anchor_before = warm_feature.slots[0].edit_reuse;
          Check(anchor_before != nullptr && anchor_before->valid, "production Bridge warm-up produced a valid edit anchor");
          const uint64_t warm_sequence = warm_feature.sr_sequence;
          const uint64_t warm_anchor = anchor_before->anchor_generation;
          uint64_t anchor_base = 0;
          std::vector<unsigned char> refresh_pixels;
          bool monitor_deliveries_match = true;
          parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
          for (uint32_t index = 0; index < 4; ++index) {
            if (index == 1) native::d3d12_swapchain_seen.store(true);
            const bool refresh = index % 2 == 0;
            const bool monitor_on = index != 2;
            native::monitor_requested.store(monitor_on);
            const auto& prior_feature = native::features.at(handle);
            const auto previous_anchor = prior_feature.slots[0].edit_reuse->anchor_generation;
            const auto prior_monitor_sequence = prior_feature.edit_reuse_monitor[0].sequence;
            const auto prior_monitor_ns = prior_feature.edit_reuse_monitor[0].recorded_ns;
            const auto calls_before = model_calls;
            const auto copies_before = bridge_copies;
            const auto motion_before = motion_copies;
            const auto depth_before = depth_copies;
            const auto uploads_before = output_uploads;
            const auto returns_before = output_copies;
            const auto queue_waits_before = native::bridge_waits.load();
            const auto return_waits_before = native::bridge_waits11.load();
            const auto input_signals_before = native::bridge_signals.load();
            const auto pixels = expect_frame(refresh ? 1u : 0u, index == 0 ? 1 : 0);
            parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
            const auto& feature = native::features.at(handle);
            const auto& anchor = feature.slots[0].edit_reuse;
            Check(anchor != nullptr && anchor->valid && anchor->source_sequence == feature.sr_sequence
                      && anchor->anchor_generation == previous_anchor + (refresh ? 1 : 0) && anchor->reused == !refresh,
                "Bridge anchor follows the real SR sequence and advances only on model refresh");
            const uint64_t base = feature.sr_sequence - warm_sequence;
            if (refresh) {
              Check(model_presets.back() == 2, "Bridge refresh invokes the requested model B");
              anchor_base = base;
              refresh_pixels = pixels;
            } else {
              for (size_t offset = 0; offset < pixels.size(); offset += sizeof(uint16_t)) {
                uint16_t refreshed, reused;
                std::memcpy(&refreshed, refresh_pixels.data() + offset, sizeof(uint16_t));
                std::memcpy(&reused, pixels.data() + offset, sizeof(uint16_t));
                Check(std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(refreshed)
                            - DirectX::PackedVector::XMConvertHalfToFloat(reused)) < .003f,
                    "zero-motion Bridge reuse transports the refreshed RGBA edit");
              }
            }
            inline_execution_trace::PrintBase("bridge", {
                .base = base, .calls = model_calls - calls_before, .reset = (refresh ? model_observations.back().reset : -1),
                .history = anchor->source_sequence - warm_sequence, .anchor_base = anchor_base,
                .anchor_generation = anchor->anchor_generation - warm_anchor, .anchor_age = base - anchor_base,
                .complete = true}, pixels);
            const auto copies = bridge_copies - copies_before;
            const auto motion = motion_copies - motion_before;
            const auto depth = depth_copies - depth_before;
            const auto uploads = output_uploads - uploads_before;
            const auto returns = output_copies - returns_before;
            const auto queue_waits = native::bridge_waits.load() - queue_waits_before;
            const auto return_waits = native::bridge_waits11.load() - return_waits_before;
            const auto input_signals = native::bridge_signals.load() - input_signals_before;
            std::cout << "BRIDGE_MONITOR_COST frame=" << base << " requested=" << monitor_on
                      << " copies=" << copies << " mv=" << motion << " depth=" << depth
                      << " output_upload=" << uploads << " output_return=" << returns << " queue_waits=" << queue_waits
                      << " return_waits=" << return_waits << " input_signals=" << input_signals << '\n';
            Check(copies == 4 && motion == 1 && depth == 1 && uploads == 1 && returns == 1
                      && queue_waits == 1 && return_waits == 1 && input_signals == 1,
                "each warmed Bridge frame uses the same copies and fence operations with monitor On or Off");
            if (monitor_on) {
              const auto delivery = (refresh ? EditReuseDelivery::REFRESH : EditReuseDelivery::REUSED);
              monitor_deliveries_match &= monitor_matches(delivery);
            } else {
              const auto& latest = feature.edit_reuse_monitor[0];
              Check(latest.sequence == prior_monitor_sequence && latest.recorded_ns == prior_monitor_ns,
                  "monitor Off leaves the last Bridge delivery receipt untouched");
            }
          }
          Check(monitor_deliveries_match, "copied-back Bridge refresh and reuse publish the current D3D11 monitor delivery");
          native::d3d12_swapchain_seen.store(false);

          const auto saved_upsample = native::look_upsample.load();
          for (const auto upsample : {renodx::addons::dlss5::look::kUpsampleAreaInput, saved_upsample}) {
            const auto epoch = native::history_reset_epoch.load();
            native::look_upsample = upsample;
            expect_frame(1, 1);
            const auto& feature = native::features.at(handle);
            const auto& receipt = feature.edit_reuse_monitor[0];
            Check(native::history_reset_epoch.load() > epoch && receipt.handed_off
                      && receipt.sequence == feature.sr_sequence && receipt.reset_epoch == native::history_reset_epoch.load(),
                "Bridge delivery carries the actual model epoch after a Look input change advances Reset");
            std::cout << "BRIDGE_MONITOR_EPOCH current=" << native::history_reset_epoch.load()
                      << " receipt=" << receipt.reset_epoch << " model_reset=" << model_observations.back().reset << '\n';
          }

          {
            ComPtr<ID3D11Device> other_device;
            Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &other_device, nullptr, nullptr))
                      && !renodx::utils::directx::SameNativeObject(other_device.Get(), device.Get()),
                "same-adapter ownership contrast uses a second real D3D11 device");
            D3D11_TEXTURE2D_DESC guide_desc{}, other_desc{};
            rg16->GetDesc(&guide_desc);
            ComPtr<ID3D11Texture2D> other_guide;
            Check(SUCCEEDED(other_device->CreateTexture2D(&guide_desc, nullptr, &other_guide)),
                "second D3D11 device creates a guide with the original descriptor");
            other_guide->GetDesc(&other_desc);
            Check(native::SameSurface(guide_desc, other_desc), "foreign guide matches the warm Bridge surface cache key");

            const auto layout = native::bridge_sets.at(handle)[0].layout_generation;
            const auto prior_copies = bridge_copies;
            const auto prior_calls = model_calls;
            const auto prior_submit = native::bridge_submitted_value.load();
            parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(other_guide.Get()));
            frame();
            parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
            Check(bridge_copies == prior_copies && model_calls == prior_calls
                      && native::bridge_submitted_value.load() == prior_submit
                      && native::bridge_sets.at(handle)[0].layout_generation == layout && consume_output() == original,
                "foreign same-shape guide issues no Bridge copies, model calls or submit and preserves SR Output");
            expect_frame(1, 1);
            Check(native::bridge_sets.at(handle)[0].layout_generation == layout,
                "the next SR with its own guide resumes NR and ordered copy-back on the original layout");
          }
          const auto complete_parameters = parameters;
          parameters.Reset();
          setup_parameters(false);
          const auto reuse_fallbacks = native::edit_reuse_fallbacks.load();
          expect_frame(1, -1);
          expect_frame(1, -1);
          Check(native::edit_reuse_fallbacks.load() == reuse_fallbacks + 2,
              "missing jitter Y prevents reuse while both frames retain the normal per-frame NR output");
          parameters = complete_parameters;
          expect_frame(1, -1);

          const auto rejected_copies = output_copies;
          const auto rejected_calls = model_calls;
          const auto rejected_submit = native::bridge_submitted_value.load();
          frame(false);
          Check(output_copies == rejected_copies && model_calls == rejected_calls
                    && native::bridge_submitted_value.load() == rejected_submit && consume_output() == original,
              "a failed original SR advances the attempt sequence without NR submit or game Output copy-back");
          Check(monitor_matches(EditReuseDelivery::NONE, EditReuseBlock::SR_FAILED),
              "a failed original SR replaces the prior delivered reuse with its actual failure reason");
          expect_frame(1, 1);
        }

        const auto pending = native::bridge_submitted_value.load() + 1;
        const auto calls_before = model_calls;
        const auto copies_before = output_copies;
        const auto timeouts_before = native::bridge_timeouts.load();
        const auto wait_ns_before = native::bridge_completion_wait_ns;
        ComPtr<ID3D12Fence> stalled;
        Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled))),
            "ordinary Bridge stall gate");
        Check(SUCCEEDED(native::bridge.queue->Wait(stalled.Get(), 1)), "stall ordinary Bridge private queue");
        StallRelease stall_release{.gate = stalled.Get()};
        frame();
        const uint32_t stalled_model_calls = (edit_reuse_production ? 0u : 1u);
        Check(native::bridge_submitted_value.load() == pending && model_calls == calls_before + stalled_model_calls,
            "the stalled frame submits a real recording with the expected number of model calls");
        if (edit_reuse_production) {
          const auto& feature = native::features.at(handle);
          const auto& slot = feature.slots[0];
          Check(slot.edit_reuse != nullptr && slot.edit_reuse->source_sequence == feature.sr_sequence
                    && !slot.edit_reuse->valid && slot.pending_reset
                    && native::bridge_pending_delivery_value == pending
                    && !renodx::addons::dlss5::submission::ResourceReleasable(slot.edit_reuse.get()),
              "unhanded Bridge reuse revokes the candidate and resets History while retaining the in-flight edit state");
        }
        const auto watchdog_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (native::bridge.fence_out12->GetCompletedValue() < pending
               && std::chrono::steady_clock::now() < watchdog_deadline) {
          Sleep(10);
        }
        const auto input_value = native::bridge.fence_in12->GetCompletedValue();
        const auto out_value = native::bridge.fence_out12->GetCompletedValue();
        Check(input_value == pending && out_value == pending && native::bridge_timeouts.load() == timeouts_before + 1
                  && native::bridge.fence_done12->GetCompletedValue() < pending,
            "actual watchdog releases out after the D3D11 input while real D3D12 done is still pending");

        const auto consumed_pixels = consume_output();
        const auto done_before_release = native::bridge.fence_done12->GetCompletedValue();
        Check(done_before_release < pending, "D3D11 game consumption finishes before the stalled NR GPU work");
        const auto early_copies = output_copies - copies_before;
        std::cout << "BRIDGE_ORDINARY_WATCHDOG input=" << input_value << " out=" << out_value
                  << " done=" << done_before_release << " game_copy_backs=" << early_copies
                  << " consumed_sr=" << (consumed_pixels == original)
                  << " completion_wait_ns=" << native::bridge_completion_wait_ns - wait_ns_before << '\n';

        if (edit_reuse_production) {
          Check(monitor_matches(EditReuseDelivery::NONE, EditReuseBlock::ORDER),
              "the watchdog publishes an order rejection while GPU done and copy-back are pending");
        }
        Check(SUCCEEDED(stalled->Signal(1)), "release the ordinary Bridge test gate");
        stall_release.gate = nullptr;
        WaitForGpu(native::bridge.fence_done12, pending);
        const auto after_done = Read11(output.Get(), 8);
        Check(early_copies == 0, "ordinary Upscaled copied back to game Output before real D3D12 done");
        Check(consumed_pixels == original, "stalled ordinary Bridge preserves the SR pixels seen by the game");
        Check(after_done == original && output_copies == copies_before, "late NR completion leaves game Output intact");

        if (edit_reuse_production) {
          Check(monitor_matches(EditReuseDelivery::NONE, EditReuseBlock::ORDER),
              "late GPU done leaves the undelivered Bridge monitor frame rejected");
          expect_frame(1, 1);
          Check(native::bridge_submitted_value.load() == pending + 1, "the first real frame after GPU done refreshes and delivers");
          Check(monitor_matches(EditReuseDelivery::REFRESH), "Bridge monitor recovers with the next delivered refresh");
          expect_frame(0, 0);
          Check(monitor_matches(EditReuseDelivery::REUSED), "the next delivered Bridge reuse is visible again");
          native::monitor_requested.store(false);
        } else {
          frame();
          Check(native::bridge_submitted_value.load() == pending + 1 && model_calls == calls_before + 2,
              "first frame after real completion resumes NR");
          WaitForGpu(native::bridge.fence_done12, pending + 1);
          Check(model_observations.back().reset == 1, "first resumed NR frame resets history after the undelivered frame");
          Check(Read11(output.Get(), 8) == warm_pixels && output_copies == copies_before + 1,
              "first resumed frame writes the completed NR result to game Output exactly once");
        }
      }
      if (edit_reuse_production) {
        native::ngx11_slot_real[0].release = reinterpret_cast<void*>(&MotionReleaseSource);
        Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeature11Slot<0>(handle)), "production D3D11 release retires the tested source");

        source_create_result = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD512});
        Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeature11Slot<0>(context.Get(), native::kFeatureDlss, &parameters, &handle))
                  && native::features.at(handle).sr_sequence == 0,
            "Before-SR contrast starts with a new registered native D3D11 source");
        native::hook_point.store(native::kHookRender);
        const auto before_color = Read11(color.Get(), 8);
        const auto refreshes = native::edit_reuse_refreshes.load();
        const auto reuses = native::edit_reuse_reuses.load();
        const auto before_sr = [&](bool reuse_requested, bool sr_succeeds, bool require_nr) {
          native::edit_reuse_enabled.store(reuse_requested);
          parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
          ++native::present_generation;
          const auto prior_sequence = native::features.at(handle).sr_sequence;
          const auto calls_before = model_calls;
          uint32_t sr_calls = 0;
          bool redirected = false;
          std::vector<unsigned char> consumed;
          const auto result = native::Ngx11Evaluate(0, "Before-SR sequence fallback", context.Get(), handle, &parameters, [&] {
            ++sr_calls;
            Check(native::features.at(handle).sr_sequence == prior_sequence + 1,
                "native source sequence advances at the original SR call after Bridge's preceding NR");
            auto* input = params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color);
            redirected = input != color.Get();
            if (redirected) {
              const auto done = native::bridge.fence_done12->GetCompletedValue();
              Check(done != UINT64_MAX && done >= native::bridge_submitted_value.load(),
                  "original SR consumes redirected Color only after actual Bridge GPU done");
            }
            context->CopyResource(output.Get(), input);
            consumed = Read11(output.Get(), 8);
            return (sr_succeeds ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail);
          });
          const auto& source = native::features.at(handle);
          const auto calls = model_calls - calls_before;
          Check(sr_calls == 1 && NVSDK_NGX_SUCCEED(result) == sr_succeeds
                    && source.source_device11_identity.Get() == renodx::utils::directx::NativeIdentity(device.Get())
                    && source.slots[0].edit_reuse == nullptr && native::edit_reuse_refreshes.load() == refreshes
                    && native::edit_reuse_reuses.load() == reuses,
              "native Before-SR has no current tracked SR sequence: neither the first nor a later frame enters x2");
          Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Output) == output.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_MotionVectors) == rg16.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Depth) == depth_direct.Get()
                    && Read11(color.Get(), 8) == before_color,
              "Before-SR success and failure restore original bindings and protect all original Color bytes");
          if (require_nr) {
            Check(calls == 1 && redirected && consumed != before_color,
                "native Before-SR keeps one fresh NR Evaluate and gives its actual Color result to original SR");
          }
          std::cout << "OBS Bridge Before-SR request=" << reuse_requested << " prior_sequence=" << prior_sequence
                    << " sequence=" << source.sr_sequence << " calls=" << calls << " redirected=" << redirected
                    << " anchor=0 refreshes=0 reuses=0 restored=1 sr_success=" << sr_succeeds << '\n';
          return std::pair{redirected, consumed};
        };
        bool warmed = false;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !warmed; ++attempt) {
          warmed = before_sr(false, true, false).first;
        }
        Check(warmed, "ordinary native Before-SR reaches its Color delivery");
        const auto per_frame = before_sr(false, true, true).second;
        for (uint32_t frame = 0; frame < 4; ++frame) {
          Check(before_sr(true, true, true).second == per_frame,
              "requesting x2 without a current Before-SR sequence preserves exact ordinary RGBA every frame");
        }
        before_sr(true, false, true);
        Check(before_sr(true, true, true).second == per_frame,
            "the next original SR recovers the same ordinary Color delivery after an SR rejection");
        Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeature11Slot<0>(handle)), "Before-SR source releases through its real wrapper");
      }
      native::ReleaseBridge();
      check_debug_messages();
      if (edit_reuse_production) {
#if defined(NR_NEXUS_DEV_CONTROL)
        constexpr int dev = 1;
#else
        constexpr int dev = 0;
#endif
        std::cout << "PASS case=bridge-edit-reuse-production evidence=WARP+MOCK dev=" << dev << '\n';
      } else {
        std::cout << "PASS ordinary Upscaled watchdog " << (warp ? "WARP" : "NATIVE_GPU")
                  << "+MOCK game consumption and first resumed NR result\n";
      }
      return 0;
    }
    if (adaptive_case) {
      native::d3d11_swapchain_seen.store(true);
      native::dx11_route.store(native::kDx11RouteNative);
      if (!adaptive_native_ngx) {
        native::direct_runtime_requirements.dual_models = true;
      }
      native::dedupe_mode.store(renodx::addons::dlss5::kDedupePerEvaluate);
      native::nr_follow_input_res.store(false);
      native::nr_resolution_scale.store(1.f);
      native::gpu_timers_enabled.store(true);
      if (!adaptive_native_ngx) {
        native::ngx_loader_staging.nr_ready = true;
        native::ngx_loader_staging.core_ready = true;
        native::PublishNgxLoaderSymbols();
      }
      native::OnNativeFgOptions(false);
      native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
      renodx::addons::dlss5::gpu_timers::comparison_settings = native::comparison_ui::CaptureSettings;
      native::hook_point.store(native::kHookUpscaled);
      native::preset.store(adaptive_hardware ? 2u : 1u);
      native::nr_adaptive_enabled.store(true);
      native::nr_adaptive_selected_levels.store(2);
      native::nr_adaptive_min_scale.store(.5f);
      Check(!native::bridge_adaptive_experiment.load() && !native::nr_adaptive_limited_residency.load()
                && !native::async_autoscale_preparation.load()
                && native::adaptive_feature_mode.load() == native::AdaptiveFeatureMode::OFF,
          "Bridge Adaptive and private PSO experiments start disabled; this case selects two full-prepared tiers only");
      params::OwnedParameters mock_parameters;
      NVSDK_NGX_Parameter* sdk_sr_parameters = nullptr;
      auto* handle = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD5AD});
      bool sr_sdk_initialized = false, nr_sdk_initialized = false, sr_feature_created = false;
      void** snippet_identity_slot = nullptr;
      void* snippet_identity_original = nullptr;
      const auto cleanup = [&] {
        if (!native::DrainBridgeQueue()) return false;
        if (adaptive_native_ngx) {
          // This is after the final readback on success; also bound the last
          // D3D11 consumer on exceptional exits before destroying either SDK.
          const auto tail = native::bridge_submitted_value.load() + 1;
          if (FAILED(native::bridge.context11->Signal(native::bridge.fence_in11, tail))) return false;
          native::bridge.context11->Flush();
          WaitForGpu(native::bridge.fence_in12, tail);
        }
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (sr_feature_created) {
          const auto result = native::HookedReleaseFeature11Slot<0>(handle);
          sr_feature_created = false;
          std::cout << nlohmann::json{{"event", "sr_release"}, {"api_result", static_cast<uint32_t>(result)}}.dump() << '\n';
          if (!NVSDK_NGX_SUCCEED(result)) return false;
        }
#endif
        native::BridgeDeviceLock device_lock(context.Get());
        const renodx::addons::dlss5::ngx_serial::Call ngx_turn;
        renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
        native::ForgetSourceHandleLocked(handle);
        native::RetireBridgeSetLocked(handle);
        for (auto& slot : native::bridge.ring) {
          if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr))) return false;
          renodx::addons::dlss5::submission::OnCommandListReset(slot.list);
          if (FAILED(slot.list->Close())) return false;
          slot.value = 0;
        }
        native::PruneCompletedSubmissionUses();
        native::DrainRetiredResources(true);
        native::DrainBridgeRetired(native::bridge.fence_done12->GetCompletedValue());
        renodx::addons::dlss5::gpu_timers::ReleaseAll();
        native::ReleaseCodecPipeline();
        model_uploads.clear();
        if (!native::bridge_retired.empty() || !native::retired_nr_features.empty()
            || !native::retired_final_resources.empty()) return false;
        bool sdk_clean = true;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (adaptive_native_snippet) {
          if (bridge_snippet_initialized_device != nullptr) {
            renodx::addons::dlss5::DirectCallScope direct_call;
            sdk_clean &= NVSDK_NGX_SUCCEED(native::direct_api.shutdown(bridge_snippet_initialized_device));
          }
          sdk_clean &= native::RestoreCallerIdentity();
          const bool restored = native::get_module_filename_iat == nullptr && native::original_get_module_filename == nullptr
              && (snippet_identity_slot == nullptr || *snippet_identity_slot == snippet_identity_original);
          sdk_clean &= restored;
          std::cout << nlohmann::json{{"event", "nr_snippet_identity_restored"}, {"restored", restored},
              {"init_calls", bridge_snippet_init_calls}, {"shutdown_calls", bridge_snippet_shutdown_calls}}.dump() << '\n';
        }
        if (nr_sdk_initialized) {
          const auto result = NVSDK_NGX_D3D12_Shutdown1(native::bridge.device12);
          nr_sdk_initialized = false;
          sdk_clean &= NVSDK_NGX_SUCCEED(result);
          std::cout << nlohmann::json{{"event", "nr_sdk_shutdown"}, {"api_result", static_cast<uint32_t>(result)}}.dump() << '\n';
        }
        if (sdk_sr_parameters != nullptr) {
          const auto result = NVSDK_NGX_D3D11_DestroyParameters(std::exchange(sdk_sr_parameters, nullptr));
          sdk_clean &= NVSDK_NGX_SUCCEED(result);
          std::cout << nlohmann::json{{"event", "sr_destroy_parameters"}, {"api_result", static_cast<uint32_t>(result)}}.dump() << '\n';
        }
        if (sr_sdk_initialized) {
          const auto result = NVSDK_NGX_D3D11_Shutdown1(device.Get());
          sr_sdk_initialized = false;
          sdk_clean &= NVSDK_NGX_SUCCEED(result);
          std::cout << nlohmann::json{{"event", "sr_sdk_shutdown"}, {"api_result", static_cast<uint32_t>(result)}}.dump() << '\n';
        }
#endif
        native::ReleaseCom(native::direct_device);
        native::direct_device_native = nullptr;
        return sdk_clean;
      };
      struct AdaptiveCleanup {
        const decltype(cleanup)* action;
        ~AdaptiveCleanup() {
          if (action != nullptr) {
            try {
              const bool drained = (*action)();
              std::cerr << "BRIDGE_ADAPTIVE exception_cleanup=" << drained << '\n';
              if (drained) native::ReleaseBridge();
            } catch (const std::exception& error) {
              std::cerr << "BRIDGE_ADAPTIVE exception_cleanup_failed=" << error.what() << '\n';
            }
          }
        }
      } cleanup_guard{&cleanup};
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      if (adaptive_native_ngx) {
        native::shutdown_quiesce_enabled.store(false);
        native::direct_api = {};
        native::direct_load_state = native::DirectLoadState::Unknown;
        native::parameter_runtime_ready = false;
        Check(native::get_module_filename_iat == nullptr && native::original_get_module_filename == nullptr,
            "Bridge SDK test starts without an identity IAT bridge");
        std::ifstream project_id_file(adaptive_project_id_path);
        std::string project_id;
        Check(project_id_file.is_open() && std::getline(project_id_file, project_id) && project_id.size() == 36,
            "read only the existing private test ProjectID, without logging it");
        bridge_private_project_id = project_id;
        const auto runtime_directory = native::AddonDirectory().wstring();
        const wchar_t* runtime_paths[]{runtime_directory.c_str()};
        NVSDK_NGX_FeatureCommonInfo common_info{};
        common_info.PathListInfo = {runtime_paths, 1};
        common_info.LoggingInfo = {BridgeSdkLog, NVSDK_NGX_LOGGING_LEVEL_ON, true};
        const auto sr_init = NVSDK_NGX_D3D11_Init_with_ProjectID(project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "NR-Nexus-Bridge-offline-1", runtime_directory.c_str(), device.Get(), &common_info);
        sr_sdk_initialized = NVSDK_NGX_SUCCEED(sr_init);
        std::cout << nlohmann::json{{"event", "sr_sdk_init"}, {"api_result", static_cast<uint32_t>(sr_init)}}.dump() << '\n';
        Check(sr_sdk_initialized, "SDK initializes the real D3D11 SR device, without fallback");
        if (adaptive_native_snippet) {
          Check(native::direct_device == nullptr && !native::parameter_runtime_ready
                    && !native::control_mask_use_core_context.load() && native::bridge_force_snippet.load(),
              "the production snippet starts without a prewritten private device or borrowed Core context");
          native::PrimeNgxLoaderSymbols(nullptr);  // Match the game's D3D11 route; no public D3D12 device identity is supplied.
          const auto& provider = native::NgxLoaderView();
          Check(provider.nr_ready && !provider.nr_failed && provider.nr_requirements.file_version == "310.9.16.0",
              "production prime resolves the pinned real NR Runtime before LoadDirectApi");
          renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
          const bool loaded = native::LoadDirectApi();
          snippet_identity_slot = native::get_module_filename_iat;
          snippet_identity_original = native::original_get_module_filename;
          std::cout << nlohmann::json{{"event", "nr_snippet_load"}, {"loaded", loaded},
              {"last_result", native::last_result.load()},
              {"identity_iat_installed", snippet_identity_slot != nullptr},
              {"private_device_initialized", native::direct_device != nullptr}}.dump() << '\n';
          Check(loaded && !native::direct_api.borrowed_core_context && native::direct_api.module == provider.nr_module
                    && native::direct_api.init != nullptr && native::direct_api.init == provider.init
                    && native::direct_api.create != nullptr && native::direct_api.create == provider.create
                    && native::direct_api.evaluate != nullptr && native::direct_api.evaluate == provider.evaluate
                    && native::direct_api.release != nullptr && native::direct_api.release == provider.release
                    && native::direct_api.shutdown != nullptr && native::direct_api.shutdown == provider.shutdown
                    && snippet_identity_slot != nullptr && snippet_identity_original != nullptr
                    && *snippet_identity_slot == reinterpret_cast<void*>(native::CallerGetModuleFileNameW),
              "production LoadDirectApi installs its real caller identity and resolves the signed snippet exports");
          bridge_snippet_init = native::direct_api.init;
          bridge_native_create = native::direct_api.create;
          bridge_native_evaluate = native::direct_api.evaluate;
          bridge_native_release = native::direct_api.release;
          bridge_snippet_shutdown = native::direct_api.shutdown;
          native::direct_api.init = BridgeSnippetInit;
          native::direct_api.create = BridgeNativeCreate;
          native::direct_api.evaluate = BridgeNativeEvaluate;
          native::direct_api.release = BridgeNativeRelease;
          native::direct_api.shutdown = BridgeSnippetShutdown;
        } else {
          const auto nr_init = NVSDK_NGX_D3D12_Init_with_ProjectID(project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
              "NR-Nexus-Bridge-offline-1", runtime_directory.c_str(), native::bridge.device12, &common_info);
          nr_sdk_initialized = NVSDK_NGX_SUCCEED(nr_init);
          std::cout << nlohmann::json{{"event", "nr_sdk_init"}, {"api_result", static_cast<uint32_t>(nr_init)}}.dump() << '\n';
          Check(nr_sdk_initialized, "SDK independently initializes the private D3D12 NR device, without snippet fallback");
          native::PrimeNgxLoaderSymbols(native::bridge.device12);
          const auto& provider = native::NgxLoaderView();
          Check(provider.nr_ready && !provider.nr_failed && provider.core_ready
                    && provider.nr_requirements.file_version == "310.9.16.0"
                    && provider.core_allocate != nullptr && provider.core_destroy != nullptr
                    && provider.core_create != nullptr && provider.core_evaluate != nullptr && provider.core_release != nullptr,
              "Bridge SDK provider resolves the pinned real NR Runtime and complete public Core functions");
          bridge_native_create = provider.core_create;
          bridge_native_evaluate = provider.core_evaluate;
          bridge_native_release = provider.core_release;
          native::direct_api = {.borrowed_core_context = false, .module = provider.nr_module,
              .allocate = provider.core_allocate, .destroy = provider.core_destroy,
              .create = BridgeNativeCreate, .evaluate = BridgeNativeEvaluate, .release = BridgeNativeRelease};
          native::direct_load_state = native::DirectLoadState::Ready;
          native::direct_runtime_requirements = provider.nr_requirements;
          native::direct_runtime_sha256 = provider.nr_sha256;
          native::core_allocate_parameters = provider.core_allocate;
          native::core_destroy_parameters = provider.core_destroy;
          native::core_create_feature = nullptr;
          native::core_evaluate_feature = nullptr;
          native::core_release_feature = nullptr;
          native::parameter_runtime_via_core.store(false);
          native::parameter_runtime_ready = true;
          Check(native::direct_device == native::bridge.device12 && !native::control_mask_use_core_context.load()
                    && native::get_module_filename_iat == nullptr && native::original_get_module_filename == nullptr,
              "SDK-preinitialized NR uses this exact private device and never borrows SR or installs identity hooks");
        }
        const auto allocated = NVSDK_NGX_D3D11_GetCapabilityParameters(&sdk_sr_parameters);
        std::cout << nlohmann::json{{"event", "sr_parameters"}, {"api_result", static_cast<uint32_t>(allocated)}}.dump() << '\n';
        Check(NVSDK_NGX_SUCCEED(allocated) && sdk_sr_parameters != nullptr, "SDK allocates real D3D11 SR parameters");
        native::ngx11_slot_real[0].create = reinterpret_cast<void*>(NVSDK_NGX_D3D11_CreateFeature);
        native::ngx11_slot_real[0].evaluate = reinterpret_cast<void*>(BridgeNativeSrEvaluate);
        native::ngx11_slot_real[0].release = reinterpret_cast<void*>(NVSDK_NGX_D3D11_ReleaseFeature);
      }
#endif
      auto& parameters = (adaptive_native_ngx ? *sdk_sr_parameters : static_cast<NVSDK_NGX_Parameter&>(mock_parameters));
      parameters.Set(NVSDK_NGX_Parameter_Width, width);
      parameters.Set(NVSDK_NGX_Parameter_Height, height);
      parameters.Set(NVSDK_NGX_Parameter_OutWidth, adaptive_output_width);
      parameters.Set(NVSDK_NGX_Parameter_OutHeight, adaptive_output_height);
      parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
      parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
      parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
      parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
              | (adaptive_native_ngx ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0)));
      parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      if (adaptive_native_ngx) {
        parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
        parameters.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        parameters.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
        for (const auto* key : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                 NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                 NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
                 NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y}) {
          parameters.Set(key, 0u);
        }
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
        parameters.Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.f / 60.f);
        parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
        handle = nullptr;
        const auto created = native::HookedCreateFeature11Slot<0>(context.Get(), native::kFeatureDlss, &parameters, &handle);
        sr_feature_created = NVSDK_NGX_SUCCEED(created) && handle != nullptr;
        std::cout << nlohmann::json{{"event", "sr_create"}, {"api_result", static_cast<uint32_t>(created)}}.dump() << '\n';
        Check(sr_feature_created && native::features.contains(handle), "production D3D11 Create wrapper registers real SR source identity");
      } else
#endif
      {
        const auto saved_create = native::ngx11_slot_real[0].create;
        native::ngx11_slot_real[0].create = reinterpret_cast<void*>(&MotionCreateSource);
        source_create_result = handle;
        const auto created = native::HookedCreateFeature11Slot<0>(context.Get(), native::kFeatureDlss, &parameters, &handle);
        native::ngx11_slot_real[0].create = saved_create;
        Check(NVSDK_NGX_SUCCEED(created) && native::create_contracts.contains(handle),
            "mock SDK uses the production D3D11 Create hook to capture the source device identity");
      }
      const auto frame = [&](bool wait_gpu = true, const char* phase = "preparing",
                             const NVSDK_NGX_Handle* source_handle = nullptr) {
        if (source_handle == nullptr) {
          source_handle = handle;
        }
        ++native::present_generation;
        const auto cpu_before = native::bridge_cpu_timing[0];
        const auto completion_before = native::bridge_completion_wait_ns;
        const auto samples_before = renodx::addons::dlss5::gpu_timers::published_segments;
        const auto gpu_before = renodx::addons::dlss5::gpu_timers::measured_total_us;
        const auto begin = native::SteadyNowNs();
        NVSDK_NGX_Result result;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (adaptive_native_ngx) {
          parameters.Set(NVSDK_NGX_Parameter_Reset, bridge_native_sr_calls == 0 ? 1 : 0);
          result = native::HookedEvaluateFeature11Slot<0>(context.Get(), source_handle, &parameters, nullptr);
        } else
#endif
        {
          result = native::Ngx11Evaluate(0, "adaptive Bridge", context.Get(), source_handle, &parameters, [&] {
            context->UpdateSubresource(output.Get(), 0, nullptr, output_pixels.data(), adaptive_output_width * 8, 0);
            return NVSDK_NGX_Result_Success;
          });
        }
        const auto source_ns = native::SteadyNowNs() - begin;
        Check(NVSDK_NGX_SUCCEED(result), "Bridge Adaptive preserves the selected SR provider's successful result");
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (adaptive_native_snippet && bridge_snippet_init_calls != 0) {
          Check(NVSDK_NGX_SUCCEED(bridge_snippet_init_result), "production snippet Init_Ext returned failure; no alternate provider is tried");
        }
#endif
        if (wait_gpu && native::bridge_submitted_value.load() != 0) {
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          const native::BridgeDeviceLock device_lock(context.Get());
          const renodx::addons::dlss5::ngx_serial::Call ngx_turn;
          renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
          native::PruneCompletedSubmissionUses();
          native::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(true, false);
          model_uploads.clear();
        }
        if (adaptive_hardware) {
          const auto& cpu = native::bridge_cpu_timing[0];
          const auto state = native::features.find(source_handle);
          std::cout << nlohmann::json{{"event", "bridge_source"}, {"phase", phase}, {"source", native::present_generation},
              {"evidence", adaptive_native_ngx ? "NATIVE_GPU+NATIVE_NGX" : "NATIVE_GPU+MOCK"},
              {"nr_provider", adaptive_native_snippet ? "production snippet Init_Ext"
                  : (adaptive_native_ngx ? "SDK-preinitialized private D3D12" : "MOCK")},
              {"source_cpu_ms", source_ns / 1'000'000.}, {"api_result", static_cast<uint32_t>(result)},
              {"completion_wait_ms", (native::bridge_completion_wait_ns - completion_before) / 1'000'000.},
              {"bridge_cpu_ms", {{"input", (cpu.input_ns - cpu_before.input_ns) / 1'000'000.},
                  {"submit", (cpu.submit_ns - cpu_before.submit_ns) / 1'000'000.},
                  {"completion", (cpu.completion_ns - cpu_before.completion_ns) / 1'000'000.},
                  {"return", (cpu.return_ns - cpu_before.return_ns) / 1'000'000.}}},
              {"gpu_segments", renodx::addons::dlss5::gpu_timers::published_segments - samples_before},
              {"gpu_measured_us", renodx::addons::dlss5::gpu_timers::measured_total_us - gpu_before},
              {"gpu_sampling", "production sparse NR timestamps, excludes D3D11 SR and bridge completion wait"},
              {"cpu_scope", adaptive_native_ngx ? "SR SDK + production Bridge/NR, excludes fixture tail wait/readback"
                  : "MOCK SR upload + production Bridge + MOCK NR upload/CPU, excludes fixture tail wait/readback"},
              {"nr_creates", native::nr_features_created}, {"nr_calls", model_calls},
              {"full_ready", state != native::features.end() && state->second.adaptive_session != nullptr
                  && state->second.adaptive_session->prepared},
              {"done", native::bridge.fence_done12->GetCompletedValue()},
              {"submitted", native::bridge_submitted_value.load()}}.dump() << '\n';
        }
        Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Output) == output.Get()
                  && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
            "Bridge Adaptive restores the game's source parameters");
      };
      if (!adaptive_hardware) {
        for (uint32_t source = 0; source < native::kContractDebounceFrames + 2; ++source) frame();
        Check(native::features.at(handle).adaptive_cache == nullptr,
            "the default-off Bridge experiment cannot enter the native Adaptive fallback");
      }
      native::bridge_adaptive_experiment.store(true);
      const auto mature = [&] {
        for (uint32_t source = 0; source < native::kContractDebounceFrames + 32; ++source) {
          frame();
          const auto& feature = native::features.at(handle);
          if (feature.adaptive_cache != nullptr && feature.adaptive_session != nullptr
              && feature.adaptive_session->prepared && feature.adaptive_cache->active == 0
              && feature.adaptive_session->status.applied == feature.adaptive_session->ladder.sizes[0]) break;
        }
        const auto& feature = native::features.at(handle);
        std::cout << "BRIDGE_ADAPTIVE ready=" << (feature.adaptive_session != nullptr && feature.adaptive_session->prepared)
            << " tiers=" << (feature.adaptive_cache != nullptr ? feature.adaptive_cache->count : 0)
            << " failed=" << (feature.adaptive_session != nullptr && feature.adaptive_session->failed)
            << " done=" << native::bridge.fence_done12->GetCompletedValue()
            << " submitted=" << native::bridge_submitted_value.load() << '\n';
        Check(feature.adaptive_cache != nullptr && feature.adaptive_session != nullptr
                  && feature.adaptive_cache->count == 2 && feature.adaptive_session->prepared
                  && feature.adaptive_session->TargetReady() && !feature.adaptive_session->failed,
            "both Bridge tiers become fully prepared from real submitted, completed and discarded recordings");
        for (size_t tier = 0; tier < 2; ++tier) {
          Check(feature.adaptive_cache->tiers[tier].ready, "each Bridge tier has GPU-ready output receipt");
          for (const auto& [resource, receipt] : feature.adaptive_cache->tiers[tier].output_completions) {
            Check(receipt->closed.load() && receipt->submitted.load() && receipt->completed_healthy,
                "Bridge readiness retains actual Reset plus healthy GPU completion proof");
          }
        }
      };
      if (adaptive_hardware) {
        mature();
      } else {
        namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
        namespace receipt_adapter = xefg::internal;
        namespace framerate = renodx::addons::dlss5::output_framerate;
        // Real WARP COM identities and production source observer, with only
        // the XeFG SDK calls/proxy-hook installation boundary mocked.
        ComPtr<IDXGISwapChain1> receipt_swapchain;
        const DXGI_SWAP_CHAIN_DESC1 description{.Width = 8, .Height = 8, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = {1, 0}, .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .BufferCount = 2,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, .AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED};
        {
          ComPtr<IDXGISwapChain1> device12_swapchain;
          Check(SUCCEEDED(factory->CreateSwapChainForComposition(native::bridge.queue, &description, nullptr, &device12_swapchain)),
              "WARP creates a real D3D12 swapchain for device-interface classification");
          xefg::BridgeDeviceObservation device12_observation;
          Check(receipt_adapter::BridgeDeviceIdentity(device12_swapchain.Get(), &device12_observation) == nullptr
                    && device12_observation.returned_device_request == 12
                    && device12_observation.device11_interface_result == E_NOINTERFACE
                    && device12_observation.device12_interface_result == S_OK
                    && device12_observation.returned_device_identity != 0
                    && device12_observation.device12_interface_identity == device12_observation.returned_device_identity,
              "real D3D12 getter fallback verifies the returned interface without inventing a D3D11 identity");
        }
        Check(SUCCEEDED(factory->CreateSwapChainForComposition(device.Get(), &description, nullptr, &receipt_swapchain)),
            "WARP creates the D3D11 application/proxy swapchain used for identity observation");
        ComPtr<IDXGISwapChain3> receipt_swapchain3;
        Check(SUCCEEDED(receipt_swapchain.As(&receipt_swapchain3)), "WARP proxy supports the existing SDK boundary");
        struct ReceiptCleanup {
          std::array<void*, receipt_adapter::EXPORTS.size()> real = receipt_adapter::modules[0].real;
          bool complete = receipt_adapter::modules[0].complete;
          receipt_adapter::ProxySlot proxy = receipt_adapter::proxies[0];
          xefg::BridgeSourceObserver observer = receipt_adapter::bridge_source_observer.load();
          bool native_off = native::native_fg_public_off.load();
          void* handle = reinterpret_cast<void*>(uintptr_t{0xEFAD});
          ~ReceiptCleanup() {
            {
              std::unique_lock lock(receipt_adapter::state.mutex);
              if (const auto it = receipt_adapter::state.contexts.find(handle);
                  it != receipt_adapter::state.contexts.end()) {
                framerate::ReleaseXeFGOutput(it->second->output_identity);
                receipt_adapter::state.contexts.erase(it);
              }
            }
            receipt_adapter::modules[0].real = real;
            receipt_adapter::modules[0].complete = complete;
            receipt_adapter::proxies[0] = proxy;
            xefg::ConfigureBridgeSource(observer);
            native::native_fg_public_off.store(native_off);
            receipt_proxy = nullptr;
            receipt_context_handle = nullptr;
            receipt_source = {};
            receipt_during_sdk = nullptr;
            receipt_adapter::RefreshOwnership();
          }
        } receipt_cleanup;
        receipt_adapter::modules[0].complete = true;
        receipt_adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeReceiptInit);
        receipt_adapter::modules[0].real[2] = reinterpret_cast<void*>(&FakeReceiptGetSwapchain);
        receipt_adapter::modules[0].real[6] = reinterpret_cast<void*>(&FakeReceiptEnable);
        receipt_adapter::proxies[0].installed = receipt_adapter::proxies[0].complete = true;
        receipt_adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        receipt_adapter::proxies[0].real[2] = (*reinterpret_cast<void***>(receipt_swapchain3.Get()))[13];
        receipt_adapter::proxies[0].records[0].target = static_cast<uint8_t*>(
            (*reinterpret_cast<void***>(receipt_swapchain3.Get()))[8]);
        xefg::ConfigureBridgeSource(&native::ObserveBridgeSourceForXefg);
        native::native_fg_public_off.store(false);
        receipt_sdk_status = 0;
        receipt_source_available = true;
        receipt_proxy = receipt_swapchain.Get();
        receipt_context_handle = receipt_cleanup.handle;
        const auto identity11 = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(device.Get()));
        receipt_source = native::ObserveBridgeSourceForXefg(identity11).identity;
        Check(receipt_source.handle == reinterpret_cast<uintptr_t>(handle)
                  && receipt_source.generation == native::create_contracts.at(handle).source_generation,
            "the production observer uniquely matches the live D3D11 Create source rather than the private D3D12 device");
        const xefg::abi::Init init{.application_swapchain = receipt_swapchain.Get()};
        Check(Hooks::Init(receipt_cleanup.handle, native::bridge.queue, &init) == 0,
            "mock SDK Init uses the real application-swapchain callback identity capture");
        const auto receipt_context = receipt_adapter::state.contexts.at(receipt_cleanup.handle);
        Check(receipt_context->bridge_application_device11.Get()
                  == native::create_contracts.at(handle).source_device11_identity.Get()
                  && receipt_context->bridge_context_ready
                  && receipt_context->bridge_application_observation.returned_device_request == 11
                  && receipt_context->bridge_application_observation.device11_interface_result == S_OK
                  && receipt_context->bridge_application_observation.device12_interface_result == E_NOINTERFACE,
            "the Init callback verifies the real D3D11 interface and holds the exact normalized source identity");
        ComPtr<IDXGISwapChain3> observed_proxy;
        Check(Hooks::GetSwapchain(receipt_cleanup.handle, IID_PPV_ARGS(&observed_proxy)) == 0
                  && receipt_context->proxy_identity == receipt_adapter::Identity(observed_proxy.Get())
                  && receipt_context->bridge_proxy_ready && !xefg::QueryBridgeOff(receipt_source).available,
            "a completed GetSwapchain establishes the public proxy before a later observation races Enable");
        observed_proxy.Reset();
        receipt_during_sdk = +[] {
          receipt_during_sdk = nullptr;
          Hooks::Enable(receipt_context_handle, 0);
        };
        const auto stale_proxy_result = Hooks::GetSwapchain(receipt_cleanup.handle, IID_PPV_ARGS(&observed_proxy));
        const auto stale_proxy_off = xefg::QueryBridgeOff(receipt_source);
        const auto expected_proxy_identity = receipt_adapter::Identity(receipt_swapchain.Get());
        std::cout << "BRIDGE_ADAPTIVE stale_proxy sdk_result=" << stale_proxy_result
            << " identity_matches=" << (receipt_context->proxy_identity == expected_proxy_identity)
            << " proxy_identity=" << reinterpret_cast<void*>(receipt_context->proxy_identity)
            << " returned_identity=" << reinterpret_cast<void*>(expected_proxy_identity)
            << " bridge_proxy_ready=" << receipt_context->bridge_proxy_ready
            << " off_available=" << stale_proxy_off.available << " reason=" << stale_proxy_off.reason << '\n';
        Check(stale_proxy_result == 0 && receipt_context->proxy_identity == expected_proxy_identity
                  && !receipt_context->bridge_proxy_ready && !stale_proxy_off.available,
            "concurrent Enable cannot discard the successful public proxy registration or accept the stale experiment declaration");
        observed_proxy.Reset();
        Check(Hooks::GetSwapchain(receipt_cleanup.handle, IID_PPV_ARGS(&observed_proxy)) == 0
                  && receipt_context->bridge_proxy_device11.Get() == receipt_context->bridge_application_device11.Get(),
            "fresh GetSwapchain captures the matching proxy D3D11 identity within the SDK callback");
        frame();
        Check(native::features.at(handle).adaptive_cache == nullptr && !xefg::QueryBridgeOff(receipt_source).available,
            "default enabled=false and the stale in-flight Disable cannot activate Bridge Adaptive");
        Check(Hooks::Enable(receipt_cleanup.handle, 0) == 0 && xefg::QueryBridgeOff(receipt_source).available,
            "a fresh successful Disable binds the unique source before and after the actual mock SDK call");
        mature();
        Check(!native::native_fg_public_off.load() && native::features.at(handle).adaptive_session->prepared,
            "the real Bridge source reaches full two-tier readiness via XeFG receipt without fabricating native SetOptions Off");
        const auto preparation_epoch = native::features.at(handle).adaptive_session->preparation_epoch;
        const auto check_unavailable_window = [&](const framerate::OutputIdentity& identity, const char* message) {
          const auto& session = *native::features.at(handle).adaptive_session;
          Check(framerate::IsCurrentOutput(identity) && session.status.display_output_generation == identity.generation
                    && session.prepared && session.status.ready_levels == 2 && session.preparation_epoch == preparation_epoch
                    && session.status.output_fps == 0.,
              message);
        };
        check_unavailable_window(xefg::QueryBridgeOff(receipt_source).output_identity,
            "Bridge retains the live output identity and prepares both tiers before the first SDK sample");

        receipt_context->get_status = &FakeReceiptPresentStatus;
        const auto present = [&] {
          return receipt_adapter::ProxyHooks<0>::Present(receipt_swapchain.Get(), 0, 0);
        };
        const auto warm_sdk_window = [&] {
          for (size_t bin = 0; bin < 4; ++bin) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(framerate::Stream::BIN_NS));
            Check(present() == S_OK, "the XeFG proxy forwards each successful Present and reads its SDK status");
          }
          return framerate::Read(receipt_context->output_identity, framerate::NowNs());
        };
        const auto read_bridge_window = [&] {
          const auto sdk_window = warm_sdk_window();
          const auto evidence = xefg::QueryBridgeOff(receipt_source);
          Check(evidence.MatchesOutput(sdk_window) && sdk_window.fps > 0., "the exact XeFG Off output has a complete SDK window");
          frame();
          const auto& status = native::features.at(handle).adaptive_session->status;
          Check(status.output_fps == sdk_window.fps && status.display_output_generation == sdk_window.output.generation,
              "Bridge Adaptive consumes the SDK FPS and generation from its XeFG Off receipt");
          return sdk_window.output;
        };
        Check(present() == S_OK, "the first off-state proxy Present opens the SDK window");
        const auto first_output = read_bridge_window();

        NVSDK_NGX_Handle* other_source = nullptr;
        source_create_result = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD5AE});
        auto& sdk_slot = native::ngx11_slot_real[0];
        const auto old_create = std::exchange(sdk_slot.create, reinterpret_cast<void*>(&MotionCreateSource));
        const auto other_created = native::HookedCreateFeature11Slot<0>(context.Get(), native::kFeatureDlss, &parameters, &other_source);
        sdk_slot.create = old_create;
        Check(NVSDK_NGX_SUCCEED(other_created) && native::create_contracts.contains(other_source),
            "a second live SR source on the same D3D11 device is registered by the real Create wrapper");
        const auto submitted = native::bridge_submits.load();
        frame(true, "wrong source", other_source);
        Check(native::bridge_submits.load() == submitted && framerate::Read(first_output, framerate::NowNs()).available,
            "the wrong Bridge source cannot use the existing SDK window even on the same device");
        const auto old_release = std::exchange(sdk_slot.release, reinterpret_cast<void*>(&MotionReleaseSource));
        const auto other_released = native::HookedReleaseFeature11Slot<0>(other_source);
        sdk_slot.release = old_release;
        Check(NVSDK_NGX_SUCCEED(other_released), "the second source releases through its real wrapper");

        receipt_sdk_status = -1;
        Check(Hooks::Enable(receipt_cleanup.handle, 0) == -1 && !xefg::QueryBridgeOff(receipt_source).available,
            "a failed Disable withdraws the existing Off receipt");
        frame();
        Check(native::bridge_submits.load() == submitted && framerate::Read(first_output, framerate::NowNs()).available,
            "a still-fresh SDK window cannot authorize Bridge after a failed Disable");
        receipt_sdk_status = 0;

        Check(receipt_adapter::ProxyHooks<0>::Resize(receipt_swapchain.Get(), 0, 9, 8, DXGI_FORMAT_UNKNOWN, 0) == S_OK
                  && !framerate::IsCurrentOutput(first_output),
            "resizing the same proxy retires its old SDK output generation");
        Check(Hooks::Enable(receipt_cleanup.handle, 0) == 0 && present() == S_OK,
            "the resized proxy has a fresh Off receipt and a first SDK sample");
        frame();
        check_unavailable_window(xefg::QueryBridgeOff(receipt_source).output_identity,
            "Bridge uses the resized proxy identity and preserves preparation while the SDK window is incomplete");
        const auto resized_output = read_bridge_window();
        Check(resized_output.generation != first_output.generation,
            "Bridge reads the newly completed window on the new generation of the same proxy");

        receipt_adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FailedReceiptPresent);
        Check(present() == E_FAIL && xefg::QueryBridgeOff(receipt_source).available,
            "a rejected proxy Present fails its output observation independently of the Disable receipt");
        frame();
        check_unavailable_window(resized_output,
            "a failed proxy Present preserves the Bridge output identity and preparation while withdrawing FPS");
        Check(native::features.at(handle).adaptive_session->status.reason == adaptive::Reason::OUTPUT_UNAVAILABLE
                  && framerate::Read(resized_output, framerate::NowNs()).reason == framerate::Reason::kFAILED,
            "Bridge withdraws the SDK FPS window after proxy Present fails");

        receipt_adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        receipt_context->get_status = +[](void*, xefg::abi::PresentStatus* status) {
          *status = {.frames_presented = 2, .enabled = 1};
          return 0;
        };
        Check(present() == S_OK, "the proxy reports its first SDK sample with FG enabled");
        const auto enabled_window = warm_sdk_window();
        Check(enabled_window.available && !enabled_window.fg_off && enabled_window.output == resized_output
                  && xefg::QueryBridgeOff(receipt_source).available,
            "the complete FG On SDK window conflicts with the matching Disable receipt");
        frame();
        check_unavailable_window(resized_output,
            "Bridge keeps the output identity and preparation while rejecting an SDK window reporting FG On");
        std::cout << "PASS Bridge Adaptive XeFG Off output ownership, resize and failure revocation\n";
      }
      auto& feature = native::features.at(handle);
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      if (adaptive_native_snippet) {
        Check(bridge_snippet_init_calls == 1 && bridge_snippet_initialized_device == native::bridge.device12
                  && native::direct_device == native::bridge.device12 && !nr_sdk_initialized
                  && !native::parameter_runtime_via_core.load() && native::core_create_feature == nullptr
                  && native::get_module_filename_iat == snippet_identity_slot,
            "EnsureDirectRuntime initialized the real private snippet device once, without SDK12 or Core borrowing");
      }
#endif
      if (adaptive_hardware) {
        frame(true, "upper");
      }
      const auto cache = feature.adaptive_cache;
      const auto source_generation = native::create_contracts.at(handle).source_generation;
      auto* shared_output = native::bridge_sets.at(handle)[0].output12;
      const auto shared_layout = native::bridge_sets.at(handle)[0].layout_generation;
      const auto creates = native::nr_features_created;
      const auto allocations = native::worksets_created;
      const auto twin_builds = native::bridge_sets_built;
      for (const size_t target : {size_t{1}, size_t{0}}) {
        feature.adaptive_session->Request(target, native::SteadyNowNs());
        const auto delivered_before = native::bridge_waits11.load();
        frame(true, target == 1 ? "lower" : "back-upper");
        Check(cache->active == target && feature.adaptive_session->status.applied == cache->ladder.sizes[target]
                  && model_observations.back().width == cache->ladder.sizes[target][0].width
                  && model_observations.back().height == cache->ladder.sizes[target][0].height
                  && model_observations.back().reset == 1,
            "prepared Bridge upper/lower selection applies real private working dimensions with Reset");
        const auto pixels = Read11(output.Get(), 8);
        if (adaptive_native_ngx) {
          Check(native::bridge_waits11.load() == delivered_before + 1
                    && pixels.size() == size_t(adaptive_output_width) * adaptive_output_height * 8,
              "real SDK Bridge completes the return Wait/copy and full-size D3D11 GPU readback");
          size_t nonzero = 0;
          for (size_t offset = 0; offset < pixels.size(); offset += sizeof(uint16_t)) {
            uint16_t value;
            std::memcpy(&value, pixels.data() + offset, sizeof(value));
            Check(std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(value)), "real Bridge output contains finite FP16 values");
            nonzero += (value & 0x7fff) != 0;
          }
          Check(nonzero != 0, "real Bridge output has valid nonzero GPU pixels; no image quality conclusion");
        } else {
          Check(std::memcmp(pixels.data(), output_pixels.data(), pixels.size()) != 0,
              "a completed Bridge tier delivers processed pixels to the full-size D3D11 target");
        }
      }
      Check(native::nr_features_created == creates && native::worksets_created == allocations
                && native::bridge_sets_built == twin_builds
                && native::bridge_sets.at(handle)[0].output12 == shared_output
                && native::bridge_sets.at(handle)[0].layout_generation == shared_layout
                && shared_output->GetDesc().Width == adaptive_output_width && shared_output->GetDesc().Height == adaptive_output_height
                && native::create_contracts.at(handle).source_generation == source_generation,
          "ready Bridge switching creates neither models nor worksets/twins and preserves full source identity");
      if (!adaptive_hardware) {
        // A real mocked standby API failure leaves the healthy active output valid.
        cache->tiers[1].ready = false;
        cache->tiers[1].output_completions.clear();
        fail_model_width_once = cache->ladder.sizes[1][0].width;
        frame();
        Check(fail_model_width_once == 0 && cache->failed && feature.adaptive_session->failed,
            "a real standby Evaluate failure pauses the same Bridge preparation attempt");
        const auto failed_creates = native::nr_features_created;
        frame();
        frame();
        Check(native::nr_features_created == failed_creates, "a failed Bridge preparation never retries Create each source");
        const auto before_recreate = native::configuration_generation.load();
        native::RecreateFeatures();
        Check(native::configuration_generation.load() == before_recreate + 1 && feature.adaptive_cache == nullptr,
            "the production clear-failure action advances configuration identity and retires the failed Bridge collection");
        mature();
      }
      const auto retained_upper = feature.slots[0].handle;
      native::nr_adaptive_enabled.store(false);
      frame(true, "off");
      frame(true, "off");
      Check(feature.adaptive_cache == nullptr && feature.slots[0].handle == retained_upper
                && feature.input_width == adaptive_output_width && feature.input_height == adaptive_output_height
                && feature.adaptive_session->status.applied[0].width == adaptive_output_width,
          "Adaptive Off restores the retained legal upper without changing shared Bridge twins");
      uint64_t gpu_samples = 0;
      if (adaptive_hardware) {
        native::enabled.store(false);
        const auto off_calls = model_calls;
        const auto off_creates = native::nr_features_created;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        const auto off_sr_calls = bridge_native_sr_calls;
#endif
        frame(true, "off");
        Check(model_calls == off_calls && native::nr_features_created == off_creates,
            "NR Off continues SR without NR Evaluate or Create");
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (adaptive_native_ngx) {
          Check(bridge_native_sr_calls == off_sr_calls + 1, "NR Off executes the real D3D11 SR provider once");
        }
#endif
        Check(!Read11(output.Get(), 8).empty(), "final D3D11 consumer completes before source release and SDK shutdown");
        gpu_samples = renodx::addons::dlss5::gpu_timers::published_segments;
        const bool cleaned = cleanup();
        cleanup_guard.action = nullptr;
        Check(cleaned && model_released.size() == model_created,
            "hardware source release and exact retirement release all private NR models and shared twins");
        Check(native::get_module_filename_iat == nullptr && native::original_get_module_filename == nullptr,
            "Bridge hardware cleanup leaves no caller identity IAT bridge");
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
        if (adaptive_native_snippet) {
          Check(bridge_snippet_shutdown_calls == 1 && bridge_snippet_initialized_device == nullptr && !nr_sdk_initialized,
              "the real snippet device shuts down once after retirement; no SDK12 shutdown is substituted");
        }
#endif
      } else {
        native::nr_adaptive_enabled.store(true);
        mature();
        // Reuse the existing bounded watchdog case: out can advance before done.
        ComPtr<ID3D12Fence> stalled;
        Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled))),
            "Bridge Adaptive stall gate");
        Check(SUCCEEDED(native::bridge.queue->Wait(stalled.Get(), 1)), "Bridge Adaptive stalls its private queue");
        std::jthread release_stall([stalled] { Sleep(1600); stalled->Signal(1); });
        const auto signals = native::bridge_signals.load();
        frame(false);
        const auto pending = native::bridge_submitted_value.load();
        const auto pending_model = model_observations.back().id;
        Check(native::bridge.fence_done12->GetCompletedValue() < pending
                  && native::bridge_pending_delivery_value == pending
                  && native::bridge.fence_out12->GetCompletedValue() >= pending,
            "watchdog out rescue is not Bridge delivery or GPU completion proof");
        const auto unmodified = Read11(output.Get(), 8);
        Check(std::memcmp(unmodified.data(), output_pixels.data(), unmodified.size()) == 0,
            "unproven Bridge completion preserves the original MOCK SR output");
        const auto blocked_calls = model_calls;
        frame(false);
        Check(model_calls == blocked_calls && native::bridge_signals.load() == signals + 1,
            "pending real done blocks repeated NR and shared twin writes");
        native::ngx11_slot_real[0].release = reinterpret_cast<void*>(MotionReleaseSource);
        Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeature11Slot<0>(handle)), "real Bridge source-release hook succeeds");
        native::DrainRetiredResources(true);
        native::DrainBridgeRetired(native::bridge.fence_done12->GetCompletedValue());
        Check(!native::bridge_retired.empty()
                  && std::find(model_released.begin(), model_released.end(), pending_model) == model_released.end(),
            "source release keeps in-flight private NR and shared twins until done and recording discard");
        release_stall.join();
        WaitForGpu(native::bridge.fence_done12, pending);
        gpu_samples = renodx::addons::dlss5::gpu_timers::published_segments;
        const bool cleaned = cleanup();
        cleanup_guard.action = nullptr;
        Check(cleaned && std::find(model_released.begin(), model_released.end(), pending_model) != model_released.end(),
            "actual done plus owned-list Reset releases the retired model, worksets and shared twins");
      }
      native::bridge_adaptive_experiment.store(false);
      native::ReleaseBridge();
      check_debug_messages();
      std::cout << "PASS Bridge Adaptive "
          << (adaptive_native_snippet ? "NATIVE_GPU+NATIVE_NGX real D3D11 SR + production private D3D12 snippet Init_Ext"
              : (adaptive_native_ngx ? "NATIVE_GPU+NATIVE_NGX real D3D11 SR + SDK-preinitialized private D3D12 NR"
                  : (adaptive_hardware ? "NATIVE_GPU+MOCK" : "WARP+MOCK")))
          << " two full-prepared tiers, ready roundtrip/Reset/zero creates, Off/source release/retirement"
          << (adaptive_hardware ? "; requested_model=B internal_model=unknown; " : ", standby failure/recovery/watchdog; ")
          << "gpu_segments=" << gpu_samples << '\n';
      return 0;
    }
    if (stage_resolution) {
      native::d3d11_swapchain_seen.store(true);
      native::dx11_route.store(native::kDx11RouteNative);
      native::direct_runtime_requirements.dual_models = true;
      native::dedupe_mode.store(renodx::addons::dlss5::kDedupePerEvaluate);
      native::nr_follow_input_res.store(false);
      native::gpu_timers_enabled.store(true);
      native::ngx_loader_staging.nr_ready = true;
      native::ngx_loader_staging.core_ready = true;
      native::PublishNgxLoaderSymbols();
      for (const uint32_t scenario : {0u, 1u, 2u}) {
        const bool mixed = !same_stage_resolution && scenario != 2;
        native::hook_point.store((mixed || render_stage_resolution) ? native::kHookRender : native::kHookUpscaled);
        native::stack_passes.store(2);
        native::preset.store(scenario == 1 ? 2u : 1u);
        native::pass_preset[0].store(scenario == 1 ? 1u : 2u);
        native::pass_stage[0].store(mixed ? 2u : 0u);
        native::pass_follow[0].store(true);
        native::pass_transfer_strength[0].store(1.f);
        native::pass_color_strength[0].store(1.f);
        native::pass_look_result[0].follow.store(true);
        native::nr_resolution_scale.store(scenario == 0 ? 1.f : .67f);
        for (uint32_t pass = 0; pass < 3; ++pass) {
          native::pass_resolution_mode[pass].store(0);
          native::pass_resolution_scale[pass].store(1.f);
        }
        if (mixed || (same_stage_resolution && scenario != 2)) {
          native::pass_resolution_mode[0].store(2);
          native::pass_resolution_scale[0].store(scenario == 0 ? .67f : 1.f);
        }
        params::OwnedParameters parameters;
        parameters.Set(NVSDK_NGX_Parameter_Width, width);
        parameters.Set(NVSDK_NGX_Parameter_Height, height);
        parameters.Set(NVSDK_NGX_Parameter_OutWidth, 17u);
        parameters.Set(NVSDK_NGX_Parameter_OutHeight, 11u);
        parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
            int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xD500} + scenario);
        const params::ParameterView create_view(&parameters, nullptr);
        native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &create_view);
        const auto timer_start = renodx::addons::dlss5::gpu_timers::measured_total_us;
        const auto samples_start = renodx::addons::dlss5::gpu_timers::published_segments;
        std::array<uint64_t, 2> resident{};
        uint32_t steady = 0;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 24 && steady < 2; ++attempt) {
          ++native::present_generation;
          context->UpdateSubresource(color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
          context->UpdateSubresource(output.Get(), 0, nullptr, output_pixels.data(), 17 * 8, 0);
          const auto before = model_calls;
          std::vector<unsigned char> sr_pixels;
          std::vector<unsigned char> sr_input;
          bool redirected = false;
          const auto result = native::Ngx11Evaluate(0, "stage resolution", context.Get(), handle, &parameters, [&] {
            auto* input = params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color);
            redirected = input != color.Get();
            if (render_stage_resolution) {
              ComPtr<ID3D11Texture2D> input_texture;
              Check(SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(&input_texture))), "Render SR consumer receives texture");
              sr_input = Read11(input_texture.Get(), 8);
              const auto original = Read11(color.Get(), 8);
              Check(std::memcmp(original.data(), color_pixels.data(), original.size()) == 0,
                  "Render chain protects original Color before native SR");
            }
            const D3D11_BOX region{0, 0, 0, width, height, 1};
            context->CopySubresourceRegion(output.Get(), 0, 0, 0, 0, input, 0, &region);
            sr_pixels = Read11(output.Get(), 8);
            // The subsequent group must snapshot SR output rather than this changed Color.
            std::vector<unsigned short> stale(width * height * 4, 0x3a00);
            if (!render_stage_resolution) {
              context->UpdateSubresource(color.Get(), 0, nullptr, stale.data(), width * 8, 0);
            }
            return NVSDK_NGX_Result_Success;
          });
          Check(NVSDK_NGX_SUCCEED(result), "resolution bridge preserves SR success");
          if (native::bridge_submitted_value.load()) {
            WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          }
          Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Output) == output.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Depth) == depth_direct.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_MotionVectors) == rg16.Get()
                    && native::GetFloat(&parameters, NVSDK_NGX_Parameter_MV_Scale_X) == 1.f
                    && native::GetFloat(&parameters, NVSDK_NGX_Parameter_MV_Scale_Y) == 1.f,
              "resolution bridge restores Color/Output and preserves game guide parameters");
          renodx::addons::dlss5::gpu_timers::Poll(native::queue_tracking_active.load(), false);
          if (model_calls - before != 2 || ((mixed || render_stage_resolution) && !redirected)) continue;
          const auto& first = model_observations[before];
          const auto& second = model_observations[before + 1];
          const uint32_t first_width = ((mixed || render_stage_resolution) ? (scenario == 0 ? width : 9u)
              : (same_stage_resolution && scenario == 0 ? 17u : 11u));
          const uint32_t first_height = ((mixed || render_stage_resolution) ? (scenario == 0 ? height : 6u)
              : (same_stage_resolution && scenario == 0 ? 11u : 7u));
          const uint32_t last_width = (render_stage_resolution ? (scenario == 1 ? width : 9u) : (scenario == 1 ? 17u : 11u));
          const uint32_t last_height = (render_stage_resolution ? (scenario == 1 ? height : 6u) : (scenario == 1 ? 11u : 7u));
          Check(first.width == first_width && first.height == first_height
                    && second.width == last_width && second.height == last_height,
              "bridge model inputs use independent stage reference dimensions");
          const auto& sets = native::bridge_sets.at(handle);
          const auto& post = native::worksets.at({handle,
              (render_stage_resolution ? sets[1].color12 : sets[0].output12), render_stage_resolution});
          if (same_stage_resolution && scenario != 2) {
            Check(post.resolution_tail != nullptr && post.resolution_tail->shared_stage_surfaces
                      && post.resolution_tail->work0 == post.work0
                      && post.resolution_tail->work_a == post.work_a
                      && post.resolution_tail->work_b == post.work_b
                      && post.resolution_tail->proxy != post.proxy
                      && post.resolution_tail->descriptors != post.descriptors,
                  "Bridge reuses full-stage images with distinct working textures and immutable descriptors");
            if (render_stage_resolution) {
              Check(post.resolution_tail->pre_sr_color == post.pre_sr_color,
                  "Render tail borrows root pre-SR delivery surface without a second allocation");
            }
          }
          if (render_stage_resolution) {
            Check(sr_input == Read11(sets[1].color11, 8)
                      && sr_input == Read12(post.pre_sr_color, 8, post.pre_sr_color_state),
                "SR consumes exact full-size final Render result through substituted Color");
            Check(std::memcmp(sr_input.data(), color_pixels.data(), sr_input.size()) != 0,
                "Render result differs from original Color");
            for (uint32_t y = 0; y < height; ++y) {
              Check(std::memcmp(sr_input.data() + y * width * 8, sr_pixels.data() + y * 17 * 8, width * 8) == 0,
                  "SR consumer GPU readback contains final Render pixels");
            }
            Check(Read11(output.Get(), 8) == sr_pixels,
                "Render-only chain leaves native SR result intact");
            const auto original = Read11(color.Get(), 8);
            Check(std::memcmp(original.data(), color_pixels.data(), original.size()) == 0,
                "Render-only success preserves original Color bytes");
          } else {
            Check(Read12(post.work0, 8, post.work0_state) == sr_pixels,
                "GPU readback proves Upscaled consumes SR output instead of changed Color");
            Check(Read11(output.Get(), 8) == Read11(sets[0].output11, 8),
                "bridge delivers full-size Upscaled decode to game Output");
          }
          if (mixed) {
            Check(sets[1].last_value < sets[0].last_value, "bridge Render submission precedes Upscaled");
            const auto decoded = Read11(sets[1].color11, 8);
            for (uint32_t y = 0; y < height; ++y) {
              Check(std::memcmp(decoded.data() + y * width * 8, sr_pixels.data() + y * 17 * 8, width * 8) == 0,
                  "SR GPU output contains preceding full-size Render decode");
            }
          }
          if (steady > 0) Check(first.id == resident[0] && second.id == resident[1]
                                    && first.reset == 0 && second.reset == 0,
              "steady resolution bridge retains independent Feature/history");
          resident = {first.id, second.id};
          if (first.reset == 0 && second.reset == 0) ++steady;
        }
        Check(steady == 2, "resolution bridge reaches two steady frames");
        if (render_stage_resolution) {
          ++native::present_generation;
          const auto before = model_calls;
          fail_model_call = before + 2;
          bool failed_redirected = false;
          std::vector<unsigned char> failed_sr;
          const auto result = native::Ngx11Evaluate(0, "Render tail failure", context.Get(), handle, &parameters, [&] {
            auto* input = params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color);
            failed_redirected = input != color.Get();
            const D3D11_BOX region{0, 0, 0, width, height, 1};
            context->CopySubresourceRegion(output.Get(), 0, 0, 0, 0, input, 0, &region);
            failed_sr = Read11(output.Get(), 8);
            return NVSDK_NGX_Result_Success;
          });
          fail_model_call = 0;
          Check(NVSDK_NGX_SUCCEED(result) && model_calls - before == 2 && !failed_redirected,
              "partial Render failure passes original Color to native SR");
          Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get()
                    && params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Output) == output.Get(),
              "failed Render chain restores game Color/Output parameters");
          Check(Read11(output.Get(), 8) == failed_sr, "failed Render chain preserves native SR output");
          const auto original = Read11(color.Get(), 8);
          Check(std::memcmp(original.data(), color_pixels.data(), original.size()) == 0,
              "partial Render failure preserves original Color bytes");
        }
        uint64_t bytes = 0;
        for (const auto& [key, resources] : native::worksets) {
          if (key.handle == handle) bytes += resources.allocated_bytes;
        }
        std::printf("PASS bridge-%s-resolution scenario=%u mixed=%u live_workset_bytes=%llu timer_segments=%llu timer_total_us=%llu GPU=standins timer_scope=warmup-readbacks-model-upload-and-failure\n",
            render_stage_resolution ? "render-stage" : (same_stage_resolution ? "same-stage" : "stage"),
            scenario, unsigned(mixed), static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(renodx::addons::dlss5::gpu_timers::published_segments - samples_start),
            static_cast<unsigned long long>(renodx::addons::dlss5::gpu_timers::measured_total_us - timer_start));
        native::ForgetSourceHandleLocked(handle);
        native::RetireBridgeSetLocked(handle);
      }
      Check(native::DrainBridgeQueue(), "resolution bridge final drain");
      native::ReleaseBridge();
      Check(native::bridge_retired.empty(), "completed bridge resources safely retire");
      return 0;
    }
    auto depth_planar = make_texture(width, height, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL, nullptr, 0);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D};
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateDepthStencilView(depth_planar.Get(), &dsv_desc, &dsv)), "depth fixture view");
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.375f, 0x17);
    auto sentinel = make_texture(1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    ComPtr<ID3D11ShaderResourceView> saved_srv;
    Check(SUCCEEDED(device->CreateShaderResourceView(sentinel.Get(), nullptr, &saved_srv)), "sentinel SRV");
    D3D11_BUFFER_DESC counter_desc{.ByteWidth = 16, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_UNORDERED_ACCESS, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        .StructureByteStride = 4};
    ComPtr<ID3D11Buffer> counter_buffer;
    Check(SUCCEEDED(device->CreateBuffer(&counter_desc, nullptr, &counter_buffer)), "sentinel counter buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC counter_view{.Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D11_UAV_DIMENSION_BUFFER, .Buffer = {0, 4, D3D11_BUFFER_UAV_FLAG_COUNTER}};
    ComPtr<ID3D11UnorderedAccessView> saved_uav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(counter_buffer.Get(), &counter_view, &saved_uav)), "sentinel counter UAV");
    const char sentinel_source[] =
        "interface IOperation { float Apply(float x); };"
        "class MotionSentinel : IOperation { float Apply(float x) { return x * 2; } };"
        "MotionSentinel sentinel; IOperation operation; RWStructuredBuffer<float> result : register(u0);"
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { result[0]=operation.Apply(1); }";
    ComPtr<ID3DBlob> bytecode, errors;
    Check(SUCCEEDED(D3DCompile(sentinel_source, sizeof(sentinel_source) - 1, nullptr, nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &bytecode, &errors)), "dynamic-linkage sentinel compile");
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> saved_instance;
    ComPtr<ID3D11ComputeShader> saved_shader;
    Check(SUCCEEDED(device->CreateClassLinkage(&linkage)), "sentinel linkage");
    Check(SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), linkage.Get(), &saved_shader)), "sentinel shader");
    Check(SUCCEEDED(linkage->CreateClassInstance("MotionSentinel", 0, 0, 0, 0, &saved_instance)), "sentinel class instance");
    const bool saved_control_mask_enabled = native::control_mask_enabled.exchange(true);
    const uint32_t saved_control_mask_mode = native::control_mask_mode.exchange(native::kControlMaskDepthFar);
    const std::string saved_control_mask_runtime_version = native::direct_runtime_requirements.file_version;
    const bool saved_d3d11_swapchain_seen = native::d3d11_swapchain_seen.exchange(true);
    const uint8_t saved_dx11_route = native::dx11_route.exchange(native::kDx11RouteNative);
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    for (bool pre : {false, true}) {
      if (dlaa_only) break;
      for (int variant = 0; variant < 3; ++variant) {
        const bool convert = variant != 2;
        const bool depth_convert = variant == 1;
        native::hook_point.store(pre ? native::kHookRender : native::kHookUpscaled);
        context->UpdateSubresource(color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        context->UpdateSubresource(output.Get(), 0, nullptr, output_pixels.data(), 17 * 8, 0);
        params::OwnedParameters parameters;
        parameters.Set(NVSDK_NGX_Parameter_Width, width);
        parameters.Set(NVSDK_NGX_Parameter_Height, height);
        parameters.Set(NVSDK_NGX_Parameter_OutWidth, 17u);
        parameters.Set(NVSDK_NGX_Parameter_OutHeight, 11u);
        parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(convert ? motion.Get() : rg16.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_convert ? depth_planar.Get() : depth_direct.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD110 + variant + (pre ? 16 : 0)));
        const params::ParameterView create_view(&parameters, nullptr);
        native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &create_view);
        auto* srv = saved_srv.Get();
        auto* uav = saved_uav.Get();
        auto* instance = saved_instance.Get();
        context->CSSetShader(saved_shader.Get(), &instance, 1);
        context->CSSetShaderResources(0, 1, &srv);
        const UINT initial_counter = 7;
        context->CSSetUnorderedAccessViews(0, 1, &uav, &initial_counter);
        ComPtr<ID3D11ComputeShader> baseline_shader;
        ComPtr<ID3D11ClassInstance> baseline_instance;
        UINT baseline_count = 1;
        context->CSGetShader(&baseline_shader, baseline_instance.GetAddressOf(), &baseline_count);
        Check(baseline_shader == saved_shader && baseline_count == 1 && baseline_instance == saved_instance,
            "sentinel shader/class instance actually bound before RunBridge");
        const auto calls_before = model_calls;
        bool delivered = false;
        for (int attempt = 0; attempt < 4 && (!delivered || model_calls == calls_before); ++attempt) {
          ++native::present_generation;
          if (pre) {
            params::ScopedD3D11Redirect redirect;
            delivered = native::RunBridge(context.Get(), handle, &parameters, &redirect);
            if (delivered) Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == native::bridge_sets.at(handle)[1].color11,
                "Render redirects shared Color");
          } else {
            delivered = native::RunBridge(context.Get(), handle, &parameters, nullptr);
          }
          if (native::bridge_submitted_value.load()) WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
        }
        if (!delivered || model_calls == calls_before) {
          for (size_t i = 0; i < std::size(renodx::addons::dlss5::nr_decline_counts); ++i) {
            if (const auto count = renodx::addons::dlss5::nr_decline_counts[i].load(); count != 0) {
              std::cerr << renodx::addons::dlss5::kNrDeclineTags[i] << "=" << count << '\n';
            }
          }
        }
        Check(delivered && model_calls > calls_before, "real RunBridge delivers model GPU output");
        auto& set = native::bridge_sets.at(handle)[pre ? 1 : 0];
        Check(model_calls == calls_before + 1, "bridge evaluation records one ControlMask consumer");
        auto& mask_workset = native::worksets.at({handle, pre ? set.color12 : set.output12, pre});
        const auto& observed = model_observations[calls_before];
        Check(observed.depth == set.depth12 && observed.control_mask != nullptr
                  && observed.control_mask == mask_workset.control_mask,
            "D3D11 Bridge binds its converted depth twin and generated ControlMask to NGX");
        auto& owner_feature = native::features.at(handle);
        auto* execution_feature = pre && owner_feature.render_session != nullptr
            ? owner_feature.render_session.get() : &owner_feature;
        Check(execution_feature->control_mask_stream != nullptr,
            "D3D11 Bridge owns an ordered ControlMask submission stream");
        const auto execution_status = renodx::addons::dlss5::control_mask_execution::GetStatus(
            execution_feature->control_mask_stream);
        Check(execution_status.recording_submitted && !execution_status.poisoned
                  && !execution_status.history_invalidated,
            "D3D11 Bridge queue submission completed in ControlMask history order");
        const auto mask_desc = mask_workset.control_mask->GetDesc();
        const auto mask_pixels = Read12(mask_workset.control_mask, 4, mask_workset.control_mask_state);
        const auto expected_mask_desc = (pre ? set.color12 : set.output12)->GetDesc();
        bool near_depth_mask = mask_desc.Width == expected_mask_desc.Width
            && mask_desc.Height == expected_mask_desc.Height
            && mask_pixels.size() == mask_desc.Width * mask_desc.Height * 4;
        for (const auto pixel : mask_pixels) near_depth_mask &= pixel == 255;
        Check(near_depth_mask,
            "D3D11 Bridge depth-far mask readback is full-size and white for near depth");
        Check(set.usable && set.motion_convert == convert && set.depth_convert == depth_convert, "selected conversion paths");
        Check(pre ? (set.color11 && !set.output11) : (!set.color11 && set.output11), "Render/Upscaled twin selection");
        Check(set.color12->GetDesc().Width == width && set.output12->GetDesc().Width == 17, "distinct input/output geometry");
        Check((set.motion_stage != nullptr) == convert && (set.motion_srv != nullptr) == convert && (set.motion_uav != nullptr) == convert,
            "fallback resources only on RG32");
        Check(set.motion12->GetDesc().Format == (convert ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16_FLOAT), "shared format");
        ComPtr<ID3D11ComputeShader> observed_shader;
        ComPtr<ID3D11ShaderResourceView> observed_srv;
        ComPtr<ID3D11UnorderedAccessView> observed_uav;
        UINT instances = 1;
        ComPtr<ID3D11ClassInstance> observed_instance;
        context->CSGetShader(&observed_shader, observed_instance.GetAddressOf(), &instances);
        context->CSGetShaderResources(0, 1, &observed_srv);
        context->CSGetUnorderedAccessViews(0, 1, &observed_uav);

        Check(observed_shader == saved_shader && observed_srv == saved_srv && observed_uav == saved_uav && instances == 1 && observed_instance == saved_instance, "D3D11 CS state restored");
        D3D11_BUFFER_DESC count_desc{.ByteWidth = 4, .Usage = D3D11_USAGE_DEFAULT};
        ComPtr<ID3D11Buffer> count_copy, count_stage;
        Check(SUCCEEDED(device->CreateBuffer(&count_desc, nullptr, &count_copy)), "counter copy target");
        count_desc.Usage = D3D11_USAGE_STAGING;
        count_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateBuffer(&count_desc, nullptr, &count_stage)), "counter readback");
        context->CopyStructureCount(count_copy.Get(), 0, saved_uav.Get());
        context->CopyResource(count_stage.Get(), count_copy.Get());
        D3D11_MAPPED_SUBRESOURCE count_mapped{};
        Check(SUCCEEDED(context->Map(count_stage.Get(), 0, D3D11_MAP_READ, 0, &count_mapped)), "counter map");
        const UINT observed_count = *static_cast<const UINT*>(count_mapped.pData);
        context->Unmap(count_stage.Get(), 0);
        Check(observed_count == initial_counter, "CS UAV hidden counter preserved");
        const auto delivered_pixels = Read11(pre ? set.color11 : output.Get(), 8);
        const UINT delivered_width = pre ? width : 17;
        const UINT delivered_height = pre ? height : 11;
        bool changed = false;
        for (size_t i = 0; i < size_t(delivered_width) * delivered_height; ++i) {
          unsigned short pixel[4]{};
          std::memcpy(pixel, delivered_pixels.data() + i * 8, sizeof(pixel));
          for (size_t channel = 0; channel < 3; ++channel) {
            Check((pixel[channel] & 0x7c00) != 0x7c00, "delivered RGB is finite");
            changed |= pixel[channel] != 0x3800;
          }
        }
        Check(changed, "model GPU output changes the delivered image");
        if (pre) {
          const auto game_output = Read11(output.Get(), 8);
          Check(std::memcmp(game_output.data(), output_pixels.data(), game_output.size()) == 0,
              "Render leaves game Output content untouched");
        } else {
          Check(Read11(set.output11, 8) == delivered_pixels, "Upscaled shared twin copied exactly to game Output");
        }
        const auto game_color = Read11(color.Get(), 8);
        Check(std::memcmp(game_color.data(), color_pixels.data(), game_color.size()) == 0, "game Color content untouched");
        const auto actual = Read12(set.motion12, convert ? 16 : 4);
        if (convert) {
          for (size_t i = 0; i < width * height; ++i) {
            Check(std::memcmp(actual.data() + i * 16, motion_pixels.data() + i * 2, 8) == 0,
                "float32 xy bit exact including edge texels");
          }
        } else {
          Check(actual.size() == rg16_pixels.size() * 2 && std::memcmp(actual.data(), rg16_pixels.data(), actual.size()) == 0, "RG16 direct copy pixels");
        }
        const auto depth_actual = Read12(set.depth12, 4);
        Check(depth_actual.size() == depth_pixels.size() * 4 && std::memcmp(depth_actual.data(), depth_pixels.data(), depth_actual.size()) == 0,
            "depth copied on motion-only and dual-convert paths");
        const auto original_motion = Read11(motion.Get(), 8);
        Check(std::memcmp(original_motion.data(), motion_pixels.data(), original_motion.size()) == 0, "original RG32 motion untouched");
        const auto original_rg16 = Read11(rg16.Get(), 4);
        Check(std::memcmp(original_rg16.data(), rg16_pixels.data(), original_rg16.size()) == 0, "original RG16 motion untouched");
        Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(), "Render Color binding restored");
        if (!depth_convert) {
          const bool saved_motion_enabled = native::control_mask_motion_enabled.exchange(true);
          const float saved_start = native::control_mask_motion_start.exchange(2.f);
          const float saved_end = native::control_mask_motion_end.exchange(6.f);
          const float saved_red = native::control_mask_red.exchange(0.8f);
          const float saved_green = native::control_mask_green.exchange(0.5f);
          const float saved_blue = native::control_mask_blue.exchange(0.25f);
          native::control_mask_mode.store(native::kControlMaskRgb);
          std::vector<float> moving32(width * height * 2, 0.f);
          std::vector<unsigned short> moving16(width * height * 2, 0);
          for (size_t i = 0; i < width * height; ++i) {
            moving32[i * 2] = 1.f;
            moving16[i * 2] = 0x3c00;
          }
          context->UpdateSubresource(convert ? motion.Get() : rg16.Get(), 0, nullptr,
              convert ? static_cast<const void*>(moving32.data()) : static_cast<const void*>(moving16.data()),
              width * (convert ? 8 : 4), 0);
          const float scale_x = 4.f * width / (pre ? width : 17.f);
          parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, scale_x);
          parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, -2.f);
          const auto motion_calls_before = model_calls;
          ++native::present_generation;
          bool motion_delivered = false;
          if (pre) {
            params::ScopedD3D11Redirect redirect;
            motion_delivered = native::RunBridge(context.Get(), handle, &parameters, &redirect);
          } else {
            motion_delivered = native::RunBridge(context.Get(), handle, &parameters, nullptr);
          }
          Check(motion_delivered && model_calls == motion_calls_before + 1,
              "moving frame reaches the production Bridge model consumer");
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          Check(model_observations[motion_calls_before].control_mask == mask_workset.control_mask,
              "moving frame binds the generated mask to the model consumer");
          const auto moving_mask = Read12(mask_workset.control_mask, 4, mask_workset.control_mask_state);
          for (size_t i = 0; i < moving_mask.size(); i += 4) {
            Check(std::abs(int(moving_mask[i]) - 102) <= 1
                      && std::abs(int(moving_mask[i + 1]) - 128) <= 1
                      && std::abs(int(moving_mask[i + 2]) - 64) <= 1 && moving_mask[i + 3] == 255,
                "scaled motion halves R and preserves G/B/A on the GPU");
          }
          Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get()
                    && native::GetFloat(&parameters, NVSDK_NGX_Parameter_MV_Scale_X) == scale_x
                    && native::GetFloat(&parameters, NVSDK_NGX_Parameter_MV_Scale_Y) == -2.f,
              "moving frame restores game Color and preserves motion scales");
          context->UpdateSubresource(convert ? motion.Get() : rg16.Get(), 0, nullptr,
              convert ? static_cast<const void*>(motion_pixels.data()) : static_cast<const void*>(rg16_pixels.data()),
              width * (convert ? 8 : 4), 0);
          parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
          parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
          native::control_mask_mode.store(native::kControlMaskDepthFar);
          native::control_mask_motion_enabled.store(saved_motion_enabled);
          native::control_mask_motion_start.store(saved_start);
          native::control_mask_motion_end.store(saved_end);
          native::control_mask_red.store(saved_red);
          native::control_mask_green.store(saved_green);
          native::control_mask_blue.store(saved_blue);
          std::printf("PASS ControlMask motion mode=%s path=%s GPU=real NR=MOCK rgba=102,128,64,255\n",
              pre ? "Render" : "Upscaled", convert ? "RG32-convert" : "RG16-direct");
        }
        if (!pre && variant == 2) {
          const auto after_frame_reset = [&] {
            ++native::present_generation;
            const auto calls_before = model_calls;
            Check(native::RunBridge(context.Get(), handle, &parameters, nullptr),
                "ordinary after frame delivers through the existing Bridge");
            WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
            Check(model_calls == calls_before + 1, "ordinary after frame evaluates exactly one model");
            return model_observations[calls_before].reset;
          };
          parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
          Check(after_frame_reset() == 0, "ordinary after history is steady before the rejected Reset frame");
          auto& history_slot = native::features.at(handle).slots[0];
          Check(!history_slot.pending_reset && history_slot.pending_reset_reasons == 0,
              "ordinary after has no pending reset before the busy gate");
          // Look is bypassed here; its valid bit is a CPU recovery sentinel.
          history_slot.look_history.valid = true;
          auto& busy_slot = native::bridge.ring[(native::bridge_submitted_value.load() + 1) % native::kBridgeRing];
          const auto old_value = busy_slot.value;
          busy_slot.value = native::bridge_submitted_value.load() + native::kBridgeRing;
          parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
          const auto calls_before_busy = model_calls;
          ++native::present_generation;
          const bool delivered_reset = native::RunBridge(context.Get(), handle, &parameters, nullptr);
          busy_slot.value = old_value;
          parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
          Check(!delivered_reset && model_calls == calls_before_busy,
              "the only source Reset frame is rejected before model evaluation when the ring is busy");
          Check(history_slot.pending_reset && (history_slot.pending_reset_reasons & native::kResetRecovery) != 0
                    && !history_slot.look_history.valid,
              "ordinary after refusal preserves a recovery reset and invalidates the Look history sentinel");
          Check(after_frame_reset() == 1 && (history_slot.model_reset_reasons & native::kResetRecovery) != 0,
              "the next source Reset=0 frame resets the model after the rejected Reset=1 frame");
          Check(after_frame_reset() == 0,
              "ordinary after resumes steady history after consuming the recovery reset once");
          std::printf("PASS ordinary after busy Reset=1 recovery/Look invalidation/next Reset=0 NR=MOCK\n");
        }
        // Hold one observer reference: after retirement only that reference may remain.
        ComPtr<ID3D11Texture2D> stage = set.motion_stage;
        ComPtr<ID3D11ShaderResourceView> motion_srv = set.motion_srv;
        ComPtr<ID3D11UnorderedAccessView> motion_uav = set.motion_uav;
        const auto last_value = set.last_value;
        const auto retired_before = native::bridge_retired.size();
        native::RetireBridgeSetLocked(handle);
        Check(!native::bridge_sets.contains(handle) && native::bridge_retired.size() == retired_before + 1, "set retired");
        native::DrainBridgeRetired(last_value - 1);
        Check(native::bridge_retired.size() == retired_before + 1, "D3D12 resources retained below completion value");
        native::DrainBridgeRetired(last_value);
        Check(native::bridge_retired.size() == retired_before, "D3D12 resources drained at completion value");
        context->ClearState();
        context->Flush();
        if (convert) {
          Check(motion_srv.Reset() == 0 && motion_uav.Reset() == 0 && stage.Reset() == 0,
              "motion SRV/UAV/stage released by RetireBridgeSet");
        }
        model_uploads.clear();
        std::printf("PASS mode=%s motion=%s depth=%s 13x9 float32/shared/readback/state/retire\n",
            pre ? "Render" : "Upscaled", convert ? "RG32 fallback" : "RG16 direct", depth_convert ? "converted" : "direct");
      }
    }
    native::control_mask_mode.store(saved_control_mask_mode);
    native::control_mask_enabled.store(saved_control_mask_enabled);
    native::direct_runtime_requirements.file_version = saved_control_mask_runtime_version;
    native::d3d11_swapchain_seen.store(saved_d3d11_swapchain_seen);
    native::dx11_route.store(saved_dx11_route);
    for (uint32_t fixture = (dlaa_only ? 1u : 0u); fixture < (sr_only ? 1u : 3u); ++fixture) {
      const UINT output_width = (fixture == 0 ? 17u : width);
      const UINT output_height = (fixture == 0 ? 11u : height);
      const bool aliased = fixture == 2;
      const auto game_color = make_texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, color_pixels.data(), width * 8);
      const std::vector<unsigned short> game_output_pixels(output_width * output_height * 4, 0x3800);
      ComPtr<ID3D11Texture2D> game_output;
      if (aliased) {
        game_output = game_color;
      } else {
        game_output = make_texture(output_width, output_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, game_output_pixels.data(), output_width * 8);
      }
      std::printf("CASE %s %ux%u->%ux%u aliased=%u\n", fixture == 0 ? "SR" : "DLAA",
          width, height, output_width, output_height, unsigned(aliased));
      const auto run_sr = [&](ID3D11Resource* input) {
        // Without a redirect an aliased identity SR needs no overlapping self-copy.
        if (input != game_output.Get()) {
          const D3D11_BOX region{0, 0, 0, width, height, 1};
          context->CopySubresourceRegion(game_output.Get(), 0, 0, 0, 0, input, 0, &region);
        }
        if (fixture != 0) {
          // Distinguish actual SR output from its Render input at equal dimensions.
          const unsigned short marker[]{0x3a00, 0x3800, 0x3400, 0x3c00};
          const D3D11_BOX pixel{width - 1, height - 1, 0, width, height, 1};
          context->UpdateSubresource(game_output.Get(), 0, &pixel, marker, sizeof(marker), 0);
        }
      };
      // Exercise the real D3D11 detour and both shared sets. Only SR/NR are stand-ins.
      native::d3d11_swapchain_seen.store(true);
      native::dx11_route.store(native::kDx11RouteNative);
      native::direct_runtime_requirements.dual_models = true;
      native::dedupe_mode.store(renodx::addons::dlss5::kDedupePerEvaluate);
      native::stack_passes.store(2);
      native::hook_point.store(native::kHookRender);
      native::preset.store(1);
      native::intensity.store(1.f);
      native::pass_preset[0].store(2);
      native::pass_stage[0].store(2);
      native::pass_transfer_strength[0].store(0.5f);
      native::pass_color_strength[0].store(0.5f);
      native::pass_look_result[0].follow.store(true);
      native::pass_follow[0].store(true);
      // Publish the fixture loader identity so the detour never loads a vendor DLL.
      native::ngx_loader_staging.nr_ready = true;
      native::ngx_loader_staging.core_ready = true;
      native::PublishNgxLoaderSymbols();
      params::OwnedParameters mixed_parameters;
      mixed_parameters.Set(NVSDK_NGX_Parameter_Width, width);
      mixed_parameters.Set(NVSDK_NGX_Parameter_Height, height);
      mixed_parameters.Set(NVSDK_NGX_Parameter_OutWidth, output_width);
      mixed_parameters.Set(NVSDK_NGX_Parameter_OutHeight, output_height);
      mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(game_color.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(game_output.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      mixed_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      mixed_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
      const auto* mixed_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD120 + fixture * 2));
      const params::ParameterView mixed_create_view(&mixed_parameters, nullptr);
      native::RegisterCreatedFeatureLocked(mixed_handle, native::kFeatureDlss, &mixed_create_view);
      bool mixed_delivered = false;
      uint64_t resident_builds = 0;
      bool stable_residency = false;
      for (int frame = 0; frame < 12; ++frame) {
        ++native::present_generation;
        context->UpdateSubresource(game_color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        if (!aliased) {
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
        }
        const auto calls_before = model_calls;
        bool redirected = false;
        std::vector<unsigned char> sr_input, sr_output;
        const auto result = native::Ngx11Evaluate(0, "mixed fixture", context.Get(), mixed_handle, &mixed_parameters, [&] {
          auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
          redirected = input != game_color.Get();
          ComPtr<ID3D11Texture2D> input_texture;
          Check(SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(&input_texture))), "SR receives D3D11 Color texture");
          sr_input = Read11(input_texture.Get(), 8);
          run_sr(input);
          sr_output = Read11(game_output.Get(), 8);
          return NVSDK_NGX_Result_Success;
        });
        Check(NVSDK_NGX_SUCCEED(result), "mixed detour preserves successful SR result");
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                  && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
            "mixed detour restores the original Color/Output bindings");
        if (native::bridge_submitted_value.load()) {
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
        }
        if (!redirected || model_calls - calls_before != 2) continue;
        Check(model_presets[calls_before] == 1 && model_presets[calls_before + 1] == 2,
            "actual model requests execute A before SR and B after SR");
        auto& sets = native::bridge_sets.at(mixed_handle);
        Check(sets[1].usable && sets[0].usable && sets[1].color11 && sets[0].output11,
            "Render and Upscaled shared sets remain resident together");
        Check(sets[1].last_value < sets[0].last_value, "Render submits before Upscaled");
        Check(sr_input == Read11(sets[1].color11, 8), "SR reads exact Render shared twin pixels");
        Check(std::memcmp(sr_input.data(), color_pixels.data(), sr_input.size()) != 0,
            "Render result actually changes SR input");
        for (UINT y = 0; y < height; ++y) {
          const UINT copied_width = width - unsigned(fixture != 0 && y == height - 1);
          Check(std::memcmp(sr_input.data() + size_t(y) * width * 8,
              sr_output.data() + size_t(y) * output_width * 8, copied_width * 8) == 0,
              "SR output contains exact Render input pixels");
        }
        Check(fixture == 0 || sr_output != sr_input, "DLAA SR output differs from its Render input");
        const auto final_pixels = Read11(game_output.Get(), 8);
        Check(final_pixels == Read11(sets[0].output11, 8), "Upscaled result handed back exactly to game Output");
        Check(final_pixels != sr_output, "Upscaled B processes SR output after Render A");
        if (aliased) {
          Check(Read11(game_color.Get(), 8) == final_pixels, "aliased Color receives the final Output writeback");
        } else {
          Check(std::memcmp(Read11(game_color.Get(), 8).data(), color_pixels.data(), color_pixels.size() * 2) == 0,
              "distinct game Color remains untouched");
        }
        auto& render_work = native::worksets.at({mixed_handle, sets[1].color12, true});
        auto& upscaled_work = native::worksets.at({mixed_handle, sets[0].output12, false});
        Check(render_work.work0 != upscaled_work.work0 && render_work.descriptors != upscaled_work.descriptors,
            "equal dimensions and game aliases retain separate stage worksets");
        // In HDR original aliases the decoded result; work0 retains the source.
        // The fixture uses linear FP16, so linearization preserves its bytes.
        Check(Read12(render_work.work0, 8, render_work.work0_state)
                  == std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(color_pixels.data()),
                      reinterpret_cast<const unsigned char*>(color_pixels.data()) + color_pixels.size() * 2),
            "Render snapshots the original game Color before SR can overwrite it");
        Check(Read12(upscaled_work.work0, 8, upscaled_work.work0_state) == sr_output,
            "Upscaled snapshots the actual SR output, not the old Color or Render input");
        const auto& feature = native::features.at(mixed_handle);
        Check(feature.render_session && feature.render_session->slots[0].handle
                  && feature.slots[0].handle && feature.render_session->slots[0].handle != feature.slots[0].handle,
            "Render and Upscaled retain distinct model/history instances");
        if (mixed_delivered) {
          Check(native::bridge_sets_built == resident_builds, "steady mixed frame does not rebuild either set");
          stable_residency = true;
          break;
        }
        mixed_delivered = true;
        resident_builds = native::bridge_sets_built;
      }
      Check(mixed_delivered && stable_residency, "AB reaches two complete frames with stable resident sets");
      {
        // Rebuild scratch while retaining the resident Render twin. Control the
        // tracker receipt separately from GPU timing so both retention gates are
        // deterministic; these extra lists record no GPU commands.
        const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD121 + fixture * 2));
        native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &mixed_create_view);
        auto* twin = native::bridge_sets.at(mixed_handle)[1].color12;
        const native::WorksetKey key{owner, twin, true};
        ComPtr<ID3D12CommandAllocator> allocator, new_allocator;
        ComPtr<ID3D12GraphicsCommandList> old_list, new_list;
        ComPtr<ID3D12Fence> receipt;
        auto* device12 = native::bridge.device12;
        Check(SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))), "retirement regression allocator");
        Check(SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&new_allocator))), "retirement new recording allocator");
        Check(SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(&old_list))), "retirement old recording");
        Check(SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            new_allocator.Get(), nullptr, IID_PPV_ARGS(&new_list))), "retirement new recording");
        Check(SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&receipt))), "retirement controlled submission receipt");
        auto* old = native::EnsureWorkset(device12, owner, twin, width, height, width, height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 0, true);
        Check(old != nullptr, "retirement old Render workset");
        twin->AddRef();
        old->pre_sr_source = twin;
        const auto old_id = old->id;
        const auto* old_scratch = old->original;
        const auto* old_heap = old->descriptors;
        const auto* shared_norm = old->norm_commit;
        native::TrackWorksetRecording(old_list.Get(), *old);
        Check(SUCCEEDED(old_list->Close()), "retirement close old recording");
        ID3D12CommandList* lists[] = {old_list.Get()};
        const auto pending = submission::BeginSubmission(1, lists);
        Check(pending.size() == 1, "old recording has an exact submission reservation");
        submission::CompleteSubmission(pending, receipt.Get(), 1);
        auto* current = native::EnsureWorkset(device12, owner, twin, width, height, width, height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 2, 0, true);
        Check(current != nullptr && current->id != old_id, "stack change rebuilds Render scratch");
        twin->AddRef();
        current->pre_sr_source = twin;
        Check(current->original != old_scratch && current->descriptors != old_heap
                  && current->norm_commit == shared_norm,
            "rebuilt workset has independent scratch/heap and shares stream Norm");
        native::TrackWorksetRecording(new_list.Get(), *current);
        Check(SUCCEEDED(new_list->Close()), "retirement close new recording");
        const auto retained = [&] {
          for (const auto& entry : native::retired_final_resources) {
            if (entry.resources.id == old_id) {
              Check(entry.tracker_proof, "regression uses exact tracker retirement");
              return true;
            }
          }
          return false;
        };
        native::DrainRetiredResources(true);
        Check(retained(), "old pending submission retains its scratch");
        Check(SUCCEEDED(receipt->Signal(1)), "complete controlled old submission receipt");
        native::DrainRetiredResources(true);
        Check(retained(), "completed but replayable old recording retains its scratch");
        const auto replay = submission::BeginSubmission(1, lists);
        Check(replay.size() == 1, "old replay has an exact submission reservation");
        submission::CompleteSubmission(replay, receipt.Get(), 2);
        Check(SUCCEEDED(old_list->Reset(allocator.Get(), nullptr)), "discard old recording");
        submission::OnCommandListReset(old_list.Get());
        Check(SUCCEEDED(old_list->Close()), "close discarded old recording");
        native::DrainRetiredResources(true);
        Check(retained(), "discarded old recording with pending replay retains its scratch");
        Check(SUCCEEDED(receipt->Signal(2)), "complete controlled old replay receipt");
        native::DrainRetiredResources(true);
        Check(!submission::ResourceReleasable(current->original),
            "new workset recording remains live during old retirement");
        Check(submission::ResourceReleasable(old_scratch)
                  && submission::ResourceReleasable(old_heap),
            "old workset exclusive objects have no remaining recorded uses");
        Check(!retained(), "new Render workset sharing Color twin must not pin retired scratch");
        submission::OnCommandListDestroyed(old_list.Get());
        submission::OnCommandListDestroyed(new_list.Get());
        native::RetireWorkset(key);
        native::DrainRetiredResources(true);
        native::ForgetSourceHandleLocked(owner);
        std::printf("PASS retirement rebuilt Render/shared Color/Norm/pending submission/replayable recording\n");
      }
      {
        // Neither binding's lifetime depends on the map or on Set retaining COM
        // objects. A bridge retirement during the SR handoff keeps Color live.
        ComPtr<ID3D11Texture2D> replacement = native::bridge_sets.at(mixed_handle)[1].color11;
        params::ScopedD3D11Redirect redirect;
        redirect.Point(&mixed_parameters, NVSDK_NGX_Parameter_Color, replacement.Get());
        native::RetireBridgeSetLocked(mixed_handle);
        Check(Read11(replacement.Get(), 8).size() == width * height * 8,
            "redirect owns its shared Color through bridge retirement");
        redirect.Restore();
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get(),
            "explicit redirect restoration remains idempotent");
      }
      std::printf("PASS mixed AB %ux%u->%ux%u aliased=%u handoff/readback/feature/history/residency\n",
          width, height, output_width, output_height, unsigned(aliased));
      struct FrameResult {
        NVSDK_NGX_Result result;
        uint32_t calls_before;
        uint32_t calls_at_sr = 0;
        bool redirected = false;
        std::vector<unsigned char> sr_pixels;
      };
      struct SrAborted {
        std::vector<unsigned char> pixels;
      };
      const auto frame = [&](bool fail_sr = false, bool change_generation = false,
                             bool nested = false, bool drain = true, bool inspect = true,
                             bool abort_sr = false) {
        ++native::present_generation;
        context->UpdateSubresource(game_color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        if (!aliased) {
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
        }
        FrameResult observed{.calls_before = model_calls};
        const auto sr = [&] {
          observed.calls_at_sr = model_calls;
          auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
          observed.redirected = input != game_color.Get();
          run_sr(input);
          if (inspect) {
            observed.sr_pixels = Read11(game_output.Get(), 8);
          }
          if (change_generation) {
            ++native::configuration_generation;
          }
          if (abort_sr) {
            Check(observed.redirected, "exception occurs during the Render handoff");
            throw SrAborted{observed.sr_pixels};
          }
          return fail_sr ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
        };
        observed.result = native::Ngx11Evaluate(0, "stage fixture", context.Get(), mixed_handle, &mixed_parameters, [&] {
          if (nested) {
            return native::Ngx11Evaluate(0, "nested C/C++ fixture", context.Get(), mixed_handle, &mixed_parameters, sr);
          }
          return sr();
        });
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                  && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
            "all detour exits restore Color/Output bindings");
        if (drain) {
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          native::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(native::queue_tracking_active.load(), false);
        }
        return observed;
      };
      const auto configure = [&](uint32_t count, uint32_t render_count, bool reverse) {
        native::stack_passes.store(count);
        native::hook_point.store(render_count != 0 ? native::kHookRender : native::kHookUpscaled);
        native::preset.store(reverse ? 2u : 1u);
        native::intensity.store(0.5f);
        native::style.store(0);
        native::local_tone_strength.store(0.25f);
        for (uint32_t pass = 1; pass < 4; ++pass) {
          native::pass_stage[pass - 1].store(pass < render_count ? 1u : 2u);
          native::pass_preset[pass - 1].store((pass + uint32_t(reverse)) % 2 + 1);
          native::pass_follow[pass - 1].store(false);
          native::pass_intensity[pass - 1].store(0.5f + 0.125f * pass);
          native::pass_style[pass - 1].store(pass % 3);
          native::pass_local_tone[pass - 1].store(0.25f + 0.125f * pass);
          native::pass_transfer_strength[pass - 1].store(0.5f);
          native::pass_color_strength[pass - 1].store(0.5f);
          native::pass_look_result[pass - 1].follow.store(false);
          native::pass_look_result[pass - 1].mode.store(true);
          native::pass_look_result[pass - 1].strength.store(0.5f + 0.125f * pass);
        }
        native::RecreateFeatures();
      };
      const auto mature = [&](uint32_t count, uint32_t render_count) {
        for (uint32_t attempt = 0; attempt < 16; ++attempt) {
          const auto observed = frame();
          Check(model_calls - observed.calls_before <= count, "no duplicate pass during maturation");
          if (model_calls - observed.calls_before == count
              && observed.calls_at_sr - observed.calls_before == render_count
              && (render_count == 0 || observed.redirected)) return;
        }
        Check(false, "stage configuration matures to a complete chain");
      };
      // Every legal Render/Upscaled split, plus AB/BA and 1->2->3->1 hot changes.
      const uint32_t configurations[][3] = {
          {1, 0, 0}, {2, 1, 0}, {2, 1, 1}, {3, 1, 0}, {3, 2, 1},
          {4, 1, 0}, {4, 2, 1}, {4, 3, 0}, {1, 1, 0}, {1, 0, 0}};
      for (const auto& config : configurations) {
        if (fixture != 0 && !(config[0] == 2 && config[1] == 1)) continue;
        const auto [count, render_count, reverse] = std::tuple{config[0], config[1], config[2]};
        configure(count, render_count, reverse != 0);
        mature(count, render_count);
        const auto built_before = native::bridge_sets_built;
        const auto observed = frame(false, false, true);
        Check(model_calls - observed.calls_before == count, "exact pass count across nested exports");
        Check(observed.calls_at_sr - observed.calls_before == render_count, "passes execute on the assigned side of SR");
        Check(native::bridge_sets_built == built_before, "hot model/stage changes retain compatible twins");
        for (uint32_t pass = 0; pass < count; ++pass) {
          const auto index = observed.calls_before + pass;
          Check(model_presets[index] == (pass + reverse) % 2 + 1, "logical model assignment survives local stage slots");
          const auto& model = model_observations[index];
          Check(model.intensity == 0.5f + 0.125f * pass && model.style == pass % 3
                    && model.tone == 0.25f + 0.125f * pass, "per-pass steering keeps logical pass identity");
          Check(model.reset == 0, "steady temporal history is not reset every frame");
        }
        mixed_parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
        const auto reset_frame = frame();
        mixed_parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
        for (uint32_t pass = 0; pass < count; ++pass) {
          Check(model_observations[reset_frame.calls_before + pass].reset == 1, "game Reset reaches every group");
        }
        std::printf("PASS stages passes=%u Render=%u Upscaled=%u reverse=%u\n", count, render_count, count - render_count, reverse);
      }
      for (const uint32_t render_count : {1u, 2u}) {
        configure(render_count + 1, render_count, false);
        mature(render_count + 1, render_count);
        fail_model_call = model_calls + render_count;
        const auto failed = frame();
        fail_model_call = 0;
        Check(NVSDK_NGX_SUCCEED(failed.result) && !failed.redirected, "failed/partial Render keeps native SR input");
        Check(model_calls - failed.calls_before == render_count, "failed/partial Render never executes Upscaled");
        Check(Read11(game_output.Get(), 8) == failed.sr_pixels, "Render failure preserves native SR result");
        Check(native::features.at(mixed_handle).slots[0].pending_reset, "failed predecessor resets following history");
      }
      configure(2, 1, false);
      mature(2, 1);
      const auto failed_sr = frame(true);
      Check(NVSDK_NGX_FAILED(failed_sr.result) && model_calls - failed_sr.calls_before == 1,
          "failed native SR never executes Upscaled");
      Check(Read11(game_output.Get(), 8) == failed_sr.sr_pixels, "failed SR output is not overwritten by NR");
      Check(native::features.at(mixed_handle).slots[0].pending_reset, "SR failure resets following history");
      const auto changed = frame(false, true);
      Check(model_calls - changed.calls_before == 1 && Read11(game_output.Get(), 8) == changed.sr_pixels,
          "configuration generation change blocks the dependent group");
      configure(2, 1, false);
      mature(2, 1);
      const auto calls_before_abort = model_calls;
      std::vector<unsigned char> aborted_pixels;
      try {
        frame(false, false, false, true, true, true);
      } catch (const SrAborted& aborted) {
        aborted_pixels = aborted.pixels;
      }
      Check(!aborted_pixels.empty() && model_calls - calls_before_abort == 1,
          "SR exception unwinds without executing the dependent group");
      Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
          "SR exception restores Color and preserves the original Output binding, including aliases");
      WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
      Check(Read11(game_output.Get(), 8) == aborted_pixels,
          "exception exit leaves the SR write intact");
      configure(2, 1, false);
      mature(2, 1);
      auto& busy_slot = native::bridge.ring[(native::bridge_submitted_value.load() + 1) % native::kBridgeRing];
      const auto old_value = busy_slot.value;
      busy_slot.value = native::bridge_submitted_value.load() + native::kBridgeRing;
      const auto busy = frame();
      busy_slot.value = old_value;
      Check(model_calls == busy.calls_before && !busy.redirected && Read11(game_output.Get(), 8) == busy.sr_pixels,
          "ring without completion proof preserves SR and skips both groups");
      // Out may be rescued while done is blocked by a test-owned GPU fence.
      // The bounded event wait must fail safely, and even a mode change must
      // not overwrite the still-pending twins. No production busy wait is used.
      ComPtr<ID3D12Fence> stalled;
      Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled))), "stall gate");
      Check(SUCCEEDED(native::bridge.queue->Wait(stalled.Get(), 1)), "stall private queue");
      StallRelease stall_release{.gate = stalled.Get()};
      const auto signals_before_stall = native::bridge_signals.load();
      const auto timed_out = frame(false, false, false, false);
      const auto pending = native::bridge_submitted_value.load();
      Check(!timed_out.redirected && model_calls - timed_out.calls_before == 1,
          "watchdog rescue is not a successful Render delivery");
      Check(native::bridge.fence_done12->GetCompletedValue() < pending
                && native::bridge_pending_delivery_value == pending, "real completion remains unproven after rescue");
      Check(Read11(game_output.Get(), 8) == timed_out.sr_pixels, "timeout preserves native SR result");
      const auto blocked = frame(false, false, false, false);
      native::hook_point.store(native::kHookUpscaled);
      native::stack_passes.store(1);
      const auto blocked_single = frame(false, false, false, false);
      Check(model_calls == blocked.calls_before && model_calls == blocked_single.calls_before
                && native::bridge_signals.load() == signals_before_stall + 1,
          "unproven mixed delivery blocks twin writes across a switch to single stage");
      const auto finish_stall = [&] {
        native::RetireBridgeSetLocked(mixed_handle);
        const auto retained = native::bridge_retired.size();
        native::DrainBridgeRetired(native::bridge.fence_done12->GetCompletedValue());
        Check(!native::bridge_retired.empty() && native::bridge_retired.size() <= retained,
            "in-flight twins remain retired until actual done");
        Check(SUCCEEDED(stalled->Signal(1)), "release the stalled Bridge submission");
        stall_release.gate = nullptr;
        WaitForGpu(native::bridge.fence_done12, pending);
      };
      if (warp && sr_only) {
        namespace submission = renodx::addons::dlss5::submission;
        const auto& pending_slot = native::bridge.ring[pending % native::kBridgeRing];
        auto* const pending_list = pending_slot.list;
        auto* const pending_allocator = pending_slot.allocator;
        auto* const bridge_device = native::bridge.device12;
        Check(pending != 0 && pending_list != nullptr && pending_allocator != nullptr && pending_slot.value == pending
                  && native::bridge.fence_out12->GetCompletedValue() >= pending
                  && native::bridge.fence_done12->GetCompletedValue() < pending,
            "WARP Bridge has a recorded submission stalled after the watchdog advanced out");
        const auto recording = submission::ObserveRecording(pending_list);
        Check(recording != nullptr && !recording->closed.load(), "the pending Bridge recording must be observed");
        const auto recording_identity = submission::RecordingIdentity(pending_list);

        // The Render twins return to COMMON. Keep them alive through the handoff
        // even after this fixture retires their Bridge source set.
        const auto& render_set = native::bridge_sets.at(mixed_handle)[1];
        const ComPtr<ID3D12Resource> color12 = render_set.color12;
        const ComPtr<ID3D12Resource> motion12 = render_set.motion12;
        const ComPtr<ID3D12Resource> depth12 = render_set.depth12;
        fg::FrameInput input{.queue = native::bridge.queue, .backbuffer = color12.Get(),
            .depth = depth12.Get(), .motion = motion12.Get(),
            .color_width = width, .color_height = height, .guide_width = width, .guide_height = height,
            .context = 0xD11F, .epoch = 1, .application_frame = 1,
            .color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, .fg_enabled = true};
        // Isolate the Bridge retirement from provider initialization by reusing
        // the ready fixture model on the same WARP device as a private provider.
        auto& symbols = native::ngx_loader_staging;
        native::direct_api.private_instance = true;
        symbols.native_api = native::direct_api;
        symbols.native_ready = true;
        native::PublishNgxLoaderSymbols();
        const auto saved_hooks = native::hooks_enabled.exchange(true);
        const auto saved_hook_point = native::hook_point.exchange(native::kHookPresent);
        Check(native::fg_service::Requested(), "XeFG Service is selected for the Bridge handoff");
        const auto calls_before_service = model_calls;
        Check(native::fg_service::Service(input) == fg::Result::NOT_SERVED && model_calls == calls_before_service
                  && !recording->closed.load() && pending_slot.value == pending && pending_slot.list == pending_list
                  && submission::RecordingIdentity(pending_list) == recording_identity,
            "Service refuses before GPU-only done and preserves the same Bridge slot and recording");

        finish_stall();
        Check(!recording->closed.load() && pending_slot.value == pending,
            "GPU-only done leaves the old Bridge recording replayable until Reset");
        auto result = fg::Result::NOT_SERVED;
        uint64_t service_submitted = 0;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && result != fg::Result::SUBMITTED; ++attempt) {
          ++input.application_frame;
          result = native::fg_service::Service(input);
          Check(recording->closed.load() && pending_slot.value == 0 && pending_slot.list == pending_list
                    && pending_slot.allocator == pending_allocator && native::bridge.device12 == bridge_device,
              "Service resets the completed recording and keeps the Bridge allocator, list and device");
          Check(result != fg::Result::FAILED, "Service can record after Bridge retirement");
          const auto& stream = native::fg_service::streams.at({false, input.context});
          service_submitted = stream.submitted;
          if (service_submitted != 0) {
            WaitForGpu(stream.fence, service_submitted);
          }
        }
        Check(result == fg::Result::SUBMITTED && service_submitted != 0 && model_calls > calls_before_service,
            "Service submits real WARP work using the model substitute after Bridge retirement");
        native::fg_service::Retire();
        native::DrainRetiredResources(true);
        native::DrainPresentRetired();
        native::hooks_enabled.store(saved_hooks);
        native::hook_point.store(saved_hook_point);
        native::direct_api.private_instance = false;
        symbols.native_api = {};
        symbols.native_ready = false;
        native::PublishNgxLoaderSymbols();
        std::printf("PASS Bridge Service handoff WARP+MOCK watchdog out / GPU done / SUBMITTED with completed fence\n");
      } else {
        finish_stall();
      }
      native::DrainBridgeRetired(pending);
      // Three watchdog trips intentionally make the production bridge lost for
      // the rest of this D3D11 session. The fixture starts a fresh bridge to
      // continue testing pending-delivery recovery; production does not.
      native::ReleaseBridge();
      Check(native::EnsureBridgeUp(context.Get()) == native::kBridgeReady,
          "fixture starts a fresh bridge after the watchdog burst");
      mature(1, 0);
      Check(native::bridge_pending_delivery_value == 0, "recovery clears pending delivery after real completion");
      std::printf("PASS failures Render/partial/SR/exception/generation/ring/watchdog/retire/recovery aliased=%u\n",
          unsigned(aliased));
      // Existing GPU timers measure NR command spans; wall time and the bridge
      // completion ledger separately include the fixture's synchronization.
      native::gpu_timers_enabled.store(true);
      for (uint32_t mode = 0; fixture == 0 && !sr_only && mode < 3; ++mode) {
        const uint32_t count = (mode == 2 ? 2u : 1u);
        configure(count, mode == 0 ? 0u : 1u, false);
        mature(count, mode == 0 ? 0u : 1u);
        const auto cpu_start = native::SteadyNowNs();
        const auto wait_start = native::bridge_completion_wait_ns;
        const auto gpu_start = renodx::addons::dlss5::gpu_timers::measured_total_us;
        for (int sample = 0; sample < 20; ++sample) frame(false, false, false, true, false);
        std::printf("PERF synthetic 13x9->17x11 mode=%u wall_us/frame=%.3f completion_wait_us/frame=%.3f nr_gpu_us/frame=%.3f\n",
            mode, (native::SteadyNowNs() - cpu_start) / 20000.0,
            (native::bridge_completion_wait_ns - wait_start) / 20000.0,
            (renodx::addons::dlss5::gpu_timers::measured_total_us - gpu_start) / 20.0);
      }
      if (!aliased) {
        // Reuse one set of game textures across source handles inside a single
        // outer chain. Dimensions cannot identify the source or own its history.
        configure(2, 1, false);
        const auto saved_budget = native::max_worksets.exchange(8);
        const auto saved_create = native::ngx11_slot_real[0].create;
        const auto saved_release = native::ngx11_slot_real[0].release;
        native::ngx11_slot_real[0].create = reinterpret_cast<void*>(MotionCreateSource);
        native::ngx11_slot_real[0].release = reinterpret_cast<void*>(MotionReleaseSource);
        NVSDK_NGX_Handle* sources[]{
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD180 + fixture * 4)),
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD181 + fixture * 4)),
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD182 + fixture * 4))};
        const auto create_source = [&](uint32_t index) {
          source_create_result = sources[index];
          NVSDK_NGX_Handle* created = nullptr;
          Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeature11Slot<0>(context.Get(),
                    index == 2 ? native::kFeatureDlssd : native::kFeatureDlss, &mixed_parameters, &created))
                    && created == sources[index], "DX11 source create wrapper records the returned handle");
        };
        const auto release_source = [&](uint32_t index) {
          Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeature11Slot<0>(sources[index])),
              "DX11 source release wrapper succeeds");
          Check(!native::features.contains(sources[index]) && !native::bridge_sets.contains(sources[index])
                    && !native::created_feature_ids.contains(sources[index]),
              "released source loses sessions, twins and feature identity");
          for (const auto& [key, workset] : native::worksets) {
            Check(key.handle != sources[index], "released source has no live stage workset");
          }
        };
        // Full transfer makes each model's deterministic GPU output observable.
        native::intensity.store(1.f);
        native::pass_intensity[0].store(1.f);
        native::pass_transfer_strength[0].store(1.f);
        native::pass_color_strength[0].store(1.f);
        native::look_mode.store(false);
        native::pass_look_result[0].follow.store(true);
        const std::vector<unsigned short> switch_pixels(width * height * 4, 0x3a00);
        context->UpdateSubresource(game_color.Get(), 0, nullptr, switch_pixels.data(), width * 8, 0);
        for (uint32_t index = 0; index < 3; ++index) create_source(index);
        UINT active_width = width, active_height = height;
        ComPtr<ID3D11Texture2D> active_color = game_color;
        const auto source_frame = [&](uint32_t index) {
          ++native::present_generation;
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
          FrameResult observed{.calls_before = model_calls};
          const auto result = native::Ngx11Evaluate(0, "source switch fixture", context.Get(), sources[index], &mixed_parameters, [&] {
            observed.calls_at_sr = model_calls;
            auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
            observed.redirected = input != active_color.Get();
            if (index == 2) {
              Check(!observed.redirected, "RR Render request never redirects Color");
            }
            const D3D11_BOX region{0, 0, 0, active_width, active_height, 1};
            context->CopySubresourceRegion(game_output.Get(), 0, 0, 0, 0, input, 0, &region);
            observed.sr_pixels = Read11(game_output.Get(), 8);
            return NVSDK_NGX_Result_Success;
          });
          Check(NVSDK_NGX_SUCCEED(result), "source switch preserves game evaluate result");
          Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == active_color.Get()
                    && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
              "source switch restores game bindings");
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          native::DrainRetiredResources(true);
          if (model_calls - observed.calls_before == 2) {
            Check(observed.calls_at_sr - observed.calls_before == (index == 2 ? 0u : 1u),
                "source-local feature type selects the actual side of SR/RR");
            Check(index == 2 || observed.redirected, "complete SR Render group delivers redirected Color");
            auto& sets = native::bridge_sets.at(sources[index]);
            Check(Read11(game_output.Get(), 8) == Read11(sets[0].output11, 8)
                      && Read11(game_output.Get(), 8) != observed.sr_pixels,
                "source switch delivers model GPU pixels through Upscaled writeback");
          }
          return observed;
        };
        const auto mature_source = [&](uint32_t index) {
          for (uint32_t attempt = 0; attempt < 16; ++attempt) {
            const auto observed = source_frame(index);
            Check(model_calls - observed.calls_before <= 2, "source maturation never duplicates passes");
            if (model_calls - observed.calls_before == 2) return observed;
          }
          Check(false, "source matures to two GPU passes");
          return FrameResult{};
        };
        std::array<std::array<ModelObservation, 2>, 3> previous{};
        const auto starts_with_reset = [&](uint64_t id) {
          for (const auto& observation : model_observations) {
            if (observation.id == id && observation.evaluation == 1) return observation.reset == 1;
          }
          return false;
        };
        for (uint32_t index = 0; index < 3; ++index) {
          mature_source(index);
          const auto steady = source_frame(index);
          Check(model_calls - steady.calls_before == 2, "source reaches steady state before interleaving");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            previous[index][pass] = model_observations[steady.calls_before + pass];
            Check(starts_with_reset(previous[index][pass].id), "new source stage starts with independent Reset");
          }
        }
        for (uint32_t index = 0; index < 3; ++index) {
          const auto& state = native::features.at(sources[index]);
          Check(state.source_feature == (index == 2 ? native::kFeatureDlssd : native::kFeatureDlss),
              "same-size sources retain their own creation identity");
          Check(index == 2 ? !state.render_session : state.render_session != nullptr,
              "RR uses root slots while SR retains its Render session");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            const auto* slot = (index != 2 && pass == 0 ? &state.render_session->slots[0]
                                                       : &state.slots[index == 2 ? pass : 0]);
            Check(slot->handle && reinterpret_cast<const MotionModel*>(slot->handle)->id == previous[index][pass].id,
                "model observations belong to the source-local stage slot");
            for (uint32_t other = 0; other < index; ++other) {
              for (const auto& model : previous[other]) {
                Check(previous[index][pass].id != model.id, "retained sources never share model instances");
              }
            }
          }
          Check(previous[index][0].id != previous[index][1].id, "each source owns two independent pass histories");
        }
        const auto check_continuity = [&](uint32_t index) {
          const auto observed = source_frame(index);
          Check(model_calls - observed.calls_before == 2, "every interleaved source executes both passes");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            const auto& current = model_observations[observed.calls_before + pass];
            Check(current.id == previous[index][pass].id && current.evaluation == previous[index][pass].evaluation + 1
                      && current.reset == 0, "interleaving preserves source-local slot and temporal history");
            previous[index][pass] = current;
          }
        };
        for (uint32_t cycle = 0; cycle < 2; ++cycle) {
          const renodx::addons::dlss5::EvaluateChainScope outer_chain;
          for (const uint32_t index : {0u, 1u, 2u, 0u}) {
            check_continuity(index);
          }
        }
        const auto old_generation = native::features.at(sources[1]).source_generation;
        const auto old_models = previous[1];
        release_source(1);
        check_continuity(0);
        check_continuity(2);
        create_source(1);
        Check(native::features.at(sources[1]).source_generation != old_generation,
            "same-address recreate has a new source generation");
        const auto recreated = mature_source(1);
        for (uint32_t pass = 0; pass < 2; ++pass) {
          const auto& current = model_observations[recreated.calls_before + pass];
          Check(current.id != old_models[pass].id, "same-address recreate never inherits the old model");
          Check(starts_with_reset(current.id), "same-address recreate starts each stage with Reset");
          previous[1][pass] = current;
        }
        check_continuity(1);
        check_continuity(0);
        check_continuity(2);
        // Change only A's render size; the display target and other handles stay live.
        for (const bool smaller : {true, false}) {
          active_width = (smaller ? width - 2 : width);
          active_height = (smaller ? height - 2 : height);
          std::vector<unsigned short> pixels(active_width * active_height * 4, 0x3a00);
          active_color = make_texture(active_width, active_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, pixels.data(), active_width * 8);
          const std::vector<uint32_t> zero_guides(active_width * active_height, 0);
          const auto active_motion = make_texture(active_width, active_height, DXGI_FORMAT_R16G16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE, zero_guides.data(), active_width * 4);
          const auto active_depth = make_texture(active_width, active_height, DXGI_FORMAT_R32_FLOAT,
              D3D11_BIND_SHADER_RESOURCE, zero_guides.data(), active_width * 4);
          mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(active_color.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(active_motion.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(active_depth.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_Width, active_width);
          mixed_parameters.Set(NVSDK_NGX_Parameter_Height, active_height);
          const auto before_resize = previous[0][0].id;
          FrameResult resized;
          for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 16; ++attempt) {
            const auto observed = source_frame(0);
            const auto& session = native::features.at(sources[0]).render_session;
            if (model_calls - observed.calls_before == 2 && session
                && session->input_width == active_width && session->input_height == active_height) {
              resized = observed;
              break;
            }
          }
          const auto& render = native::features.at(sources[0]).render_session;
          Check(render && render->input_width == active_width && render->input_height == active_height,
              "same source updates its Render working dimensions");
          Check(model_calls - resized.calls_before == 2, "resized source resumes both stages after contract debounce");
          Check(native::bridge_sets.at(sources[0])[1].color12->GetDesc().Width == active_width,
              "dynamic render size rebuilds the matching shared twin");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            previous[0][pass] = model_observations[resized.calls_before + pass];
          }
          Check(previous[0][0].id != before_resize, "changed Render dimensions create an independent model");
          Check(starts_with_reset(previous[0][0].id), "resized Render model starts with Reset");
          check_continuity(0);
        }
        mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(game_color.Get()));
        mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
        mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
        active_color = game_color;
        check_continuity(1);
        check_continuity(2);
        for (uint32_t index = 0; index < 3; ++index) release_source(index);
        native::ngx11_slot_real[0].create = saved_create;
        native::ngx11_slot_real[0].release = saved_release;
        source_create_result = nullptr;
        native::max_worksets.store(saved_budget);
        std::printf("PASS source switching SR A/B/RR C/A, retained history, same-address recreate, dynamic Render size %ux%u\n",
            output_width, output_height);
      }
      native::ForgetSourceHandleLocked(mixed_handle);
      native::RetireBridgeSetLocked(mixed_handle);
      Check(!native::features.contains(mixed_handle) && !native::bridge_sets.contains(mixed_handle),
          "source release removes both stage sessions and resident twins");
      for (const auto& [key, workset] : native::worksets) {
        Check(key.handle != mixed_handle, "source release retires both stage worksets");
      }
    }
    Check(native::DrainBridgeQueue(), "production bridge drained");
    native::ReleaseBridge();
    Check(native::bridge.motion_cs == nullptr && native::bridge_retired.empty()
              && native::bridge_pending_delivery_value == 0, "drained bridge releases shaders, twins and pending delivery");
    check_debug_messages();
    std::printf("PASS motion GPU regression model_calls=%u\n", model_calls);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
#endif
