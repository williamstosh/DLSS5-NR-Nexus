#pragma once

#include <map>
#include <string_view>

#include "vulkan_test_api_stubs.hpp"
#include "../addon.cpp"

namespace bridge_adaptive_cases {

inline std::map<reshade::addon_event, std::vector<void*>> callbacks;

template <reshade::addon_event Event, typename... Args>
void Dispatch(Args... args) {
  using Callback = typename reshade::addon_event_traits<Event>::decl;
  for (auto* entry : callbacks[Event]) {
    reinterpret_cast<Callback>(entry)(args...);
  }
}

struct Device final : nr::test::vk::DeviceApiStubs {
  ID3D11Device* native = nullptr;
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(native); }
  reshade::api::device_api get_api() const override { return reshade::api::device_api::d3d11; }
};

struct Queue final : reshade::api::command_queue {
  Device* device = nullptr;
  uint64_t get_native() const override { return 0; }
  void get_private_data(const uint8_t[16], uint64_t*) const override { throw std::logic_error("queue data"); }
  void set_private_data(const uint8_t[16], uint64_t) override { throw std::logic_error("queue data"); }
  reshade::api::device* get_device() override { return device; }
  reshade::api::command_queue_type get_type() const override { return reshade::api::command_queue_type::graphics; }
  void wait_idle() const override { throw std::logic_error("queue wait"); }
  void flush_immediate_command_list() const override { throw std::logic_error("unexpected queue submission"); }
  reshade::api::command_list* get_immediate_command_list() override { return nullptr; }
  void begin_debug_event(const char*, const float[4]) override { throw std::logic_error("queue event"); }
  void end_debug_event() override { throw std::logic_error("queue event"); }
  void insert_debug_marker(const char*, const float[4]) override { throw std::logic_error("queue event"); }
  bool wait(reshade::api::fence, uint64_t) override { throw std::logic_error("queue wait"); }
  bool signal(reshade::api::fence, uint64_t) override { throw std::logic_error("queue signal"); }
  uint64_t get_timestamp_frequency() const override { return 0; }
};

struct Swapchain final : reshade::api::swapchain {
  Device device;
  Queue queue;
  ComPtr<IDXGISwapChain1> native;
  std::array<ComPtr<ID3D11Texture2D>, 1> buffers;

  Swapchain(IDXGIFactory4* factory, ID3D11Device* game_device, uint32_t width, uint32_t height) {
    device.native = game_device;
    queue.device = &device;
    const DXGI_SWAP_CHAIN_DESC1 description{
        .Width = width, .Height = height, .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc = {1, 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS,
        .BufferCount = 2,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, .AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED};
    Check(SUCCEEDED(factory->CreateSwapChainForComposition(game_device, &description, nullptr, &native)),
        "Bridge Adaptive real D3D11 output");
    for (UINT index = 0; index < buffers.size(); ++index) {
      Check(SUCCEEDED(native->GetBuffer(index, IID_PPV_ARGS(&buffers[index]))), "Bridge Adaptive real D3D11 backbuffer");
    }
  }

  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(native.Get()); }
  void get_private_data(const uint8_t[16], uint64_t*) const override { throw std::logic_error("swapchain data"); }
  void set_private_data(const uint8_t[16], uint64_t) override { throw std::logic_error("swapchain data"); }
  reshade::api::device* get_device() override { return &device; }
  void* get_hwnd() const override { return nullptr; }
  reshade::api::resource get_back_buffer(uint32_t index) override {
    return {reinterpret_cast<uintptr_t>(buffers.at(index).Get())};
  }
  uint32_t get_back_buffer_count() const override { return static_cast<uint32_t>(buffers.size()); }
  uint32_t get_current_back_buffer_index() const override { return 0; }
  bool check_color_space_support(reshade::api::color_space space) const override {
    return space == reshade::api::color_space::scrgb;
  }
  reshade::api::color_space get_color_space() const override { return reshade::api::color_space::scrgb; }
};

}  // namespace bridge_adaptive_cases

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event event, void* callback) {
  bridge_adaptive_cases::callbacks[event].push_back(callback);
}

extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event event, void* callback) {
  std::erase(bridge_adaptive_cases::callbacks[event], callback);
}

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t version) {
  return version == RESHADE_API_VERSION;
}

extern "C" __declspec(dllexport) bool ReShadeUnregisterAddon(void*) { return true; }

#if defined(IMGUI_VERSION_NUM)
extern "C" __declspec(dllexport) const imgui_function_table* ReShadeGetImGuiFunctionTable(uint32_t version) {
  static const imgui_function_table table{};
  return (version == IMGUI_VERSION_NUM ? &table : nullptr);
}
#endif

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char* key, char* value, size_t* size) {
  const auto name = std::string_view(key);
  const char* selected = nullptr;
  if (name == "NRAdaptiveEnabled") selected = "1";
  if (name == "NRAdaptiveTargetFPS") selected = "100";
  if (name == "NRAdaptiveMinScale") selected = "0.5";
  if (name == "NRAdaptiveLevelCount") selected = "2";
  if (selected == nullptr || size == nullptr) return false;
  const auto required = std::strlen(selected) + 1;
  if (value == nullptr || *size < required) {
    *size = required;
    return value == nullptr;
  }
  std::memcpy(value, selected, required);
  *size = required;
  return true;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, const char*) {}
