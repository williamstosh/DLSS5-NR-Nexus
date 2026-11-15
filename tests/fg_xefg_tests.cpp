#define RESHADE_ADDON 1
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <include/reshade.hpp>
#include "d3d12_test_gpu.hpp"
#include <fg_xefg.hpp>
#include <fg_native_options.hpp>

std::string interval_diagnostic;
#ifndef FG_XEFG_TEST_CUSTOM_LOGGER
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  if (std::strstr(message, "DLSS5 FG interval ") != nullptr) {
    interval_diagnostic = message;
  }
}
#endif

namespace {
using Microsoft::WRL::ComPtr;
using d3d12_test::Check;
using d3d12_test::Gpu;
namespace fg = renodx::addons::dlss5::fg;
namespace xefg = fg::xefg;
namespace adapter = xefg::internal;
namespace submission = renodx::addons::dlss5::submission;
using renodx::addons::dlss5::FenceRef;
using Hooks = adapter::SdkHooks<0>;
constexpr int32_t HUDLESS = 0, DEPTH = 1, MOTION = 2, BACKBUFFER = 4;
constexpr int32_t UNTIL_NEXT_PRESENT = 0, ONLY_NOW = 1;

void TestDiagnosticBudgets() {
  adapter::ContractTrace<int> trace;
  uint32_t emitted = 0;
  for (uint32_t id = 1; id <= 3; ++id) emitted += trace.Observe(id, 7);
  Check(emitted == 3 && trace.frame_samples == 3 && trace.change_samples == 0,
        "resource diagnostic emits at most three initial frame samples");
  for (int value = 8; value < 20; ++value) emitted += trace.Observe(3, value);
  Check(trace.change_samples == 8 && emitted == 11,
        "resource diagnostic emits at most eight contract changes");
  Check(!trace.Observe(3, 20), "resource diagnostic budget suppresses later changes");

  adapter::ContractTrace<std::array<int64_t, 3>> ui_trace;
  Check(ui_trace.Observe(0, {1, 0, 1}), "UI diagnostic emits its first state");
  for (int status = 1; status <= 8; ++status) {
    Check(ui_trace.Observe(0, {status + 1, 0, status + 1}),
          "UI diagnostic emits each bounded state change");
  }
  Check(ui_trace.frame_samples == 1 && ui_trace.change_samples == 8
            && !ui_trace.Observe(0, {99, 0, 99}),
        "UI diagnostic budget is one initial sample plus eight changes");
}
sl::Result options_result = sl::Result::eOk;
uint32_t options_calls = 0;
uint32_t options_events = 0;
bool last_options_enabled = true;
sl::Result FakeOptions(const sl::ViewportHandle&, const sl::DLSSGOptions&) {
  ++options_calls;
  return options_result;
}
void OptionsChanged(bool enabled) noexcept {
  ++options_events;
  last_options_enabled = enabled;
}
struct SdkCall {
  void* context;
  ID3D12CommandList* list;
  uint32_t id;
  xefg::abi::Resource resource;
};
std::vector<SdkCall> sdk_calls;
std::vector<fg::FrameInput> service_calls;
int32_t sdk_status = 0;
bool policy = true;
bool service_observed_preparing = true;
fg::Result service_outcome = fg::Result::SUBMITTED;
std::vector<fg::Result> observed_display_results;
bool Requested() noexcept { return policy; }
bool Prepare(ID3D12GraphicsCommandList*) noexcept { return true; }
fg::Result Service(const fg::FrameInput& input) noexcept {
  service_observed_preparing &= xefg::CurrentResult() == fg::Result::PREPARED;
  observed_display_results.push_back(xefg::DisplayResult());
  service_calls.push_back(input);
  return (input.depth != nullptr && input.motion != nullptr && input.has_constants
      ? service_outcome : fg::Result::NOT_SERVED);
}
int32_t FakeInit(void*, ID3D12CommandQueue*, const xefg::abi::Init*) { return sdk_status; }
int32_t FakeResource(void* context, ID3D12CommandList* list, uint32_t id, const xefg::abi::Resource* resource) {
  if (resource != nullptr) sdk_calls.push_back({context, list, id, *resource});
  return sdk_status;
}
int32_t FakeConstants(void*, uint32_t, const xefg::abi::Constants*) { return sdk_status; }
int32_t FakeId(void*, uint32_t) { return sdk_status; }
std::atomic_bool receipt_device_destroyed{false};
std::atomic_bool receipt_device_release_unlocked{false};
// CPU receipt tests supply captured identities explicitly; the Bridge WARP
// fixture separately checks real D3D11 GetDevice capture and source matching.
struct ReceiptDeviceIdentity : IUnknown {
  ULONG references = 1;
  bool supports_device11 = false;
  bool supports_device12 = false;
  uint32_t device11_queries = 0;
  uint32_t device12_queries = 0;
  void (*on_destroy)() = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    if (iid == __uuidof(ID3D11Device)) {
      ++device11_queries;
    }
    if (iid == __uuidof(ID3D12Device)) {
      ++device12_queries;
    }
    if (iid != IID_IUnknown && !(iid == __uuidof(ID3D11Device) && supports_device11)
        && !(iid == __uuidof(ID3D12Device) && supports_device12)) return E_NOINTERFACE;
    *output = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references;
    if (remaining == 0) {
      if (on_destroy != nullptr) on_destroy();
      delete this;
    }
    return remaining;
  }
};
struct ReceiptSwapchain : IDXGISwapChain {
  ULONG references = 1;
  uint32_t get_device_calls = 0;
  ComPtr<IUnknown> reported_device;
  ComPtr<IDXGISwapChain> native_swapchain;
  ComPtr<IUnknown> reported_device12;
  HRESULT (*present_callback)(ReceiptSwapchain*, UINT, UINT) = nullptr;
  explicit ReceiptSwapchain(IUnknown* device, IDXGISwapChain* native = nullptr)
      : reported_device(device), native_swapchain(native) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    if (iid == __uuidof(renodx::utils::directx::ReShadeRetrieveBaseInterface) && native_swapchain != nullptr) {
      return native_swapchain->QueryInterface(__uuidof(IDXGISwapChain), output);
    }
    if (iid != IID_IUnknown && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIDeviceSubObject)
        && iid != __uuidof(IDXGISwapChain)) return E_NOINTERFACE;
    *output = static_cast<IDXGISwapChain*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override {
    ++get_device_calls;
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    IUnknown* device = nullptr;
    if (iid == __uuidof(ID3D11Device)) {
      device = reported_device.Get();
    } else if (iid == __uuidof(ID3D12Device)) {
      device = reported_device12.Get();
    }
    if (device == nullptr) return E_NOINTERFACE;
    // The identity-only MOCK supports the IUnknown methods used by capture;
    // no D3D device method or GPU operation is emulated or executed.
    *output = device;
    device->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override {
    return (present_callback != nullptr ? present_callback(this, interval, flags) : E_NOTIMPL);
  }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
};
xefg::BridgeSourceIdentity receipt_source;
bool receipt_source_available = true;
uint32_t receipt_candidates = 1;
uint32_t receipt_device12_matches = 1;
int32_t receipt_sdk_status = 0;
void (*receipt_during_sdk)() = nullptr;
IUnknown* receipt_proxy = nullptr;  // Borrowed only across the mock SDK callback.
IUnknown* receipt_secondary_proxy = nullptr;
IDXGISwapChain* receipt_inner_source = nullptr;
void* receipt_context_handle = nullptr;
xefg::BridgeSourceObservation ObserveReceiptSource(uintptr_t device11) noexcept {
  xefg::BridgeSourceObservation observation{.queried_device11 = device11,
      .state = xefg::BridgeSnapshotState::RUNTIME_BUSY};
  if (!receipt_source_available) return observation;
  observation.state = xefg::BridgeSnapshotState::LOCKED;
  observation.candidates = receipt_candidates;
  if (receipt_source.device11 == device11) {
    observation.identity = receipt_source;
    observation.matches = 1;
  } else {
    observation.identity.device11 = device11;
  }
  return observation;
}
xefg::BridgeSourceObservation ObserveReceiptD3D12Source(uintptr_t device12) noexcept {
  xefg::BridgeSourceObservation observation{.state = xefg::BridgeSnapshotState::RUNTIME_BUSY,
      .queried_device12 = device12};
  if (!receipt_source_available) return observation;
  observation.state = xefg::BridgeSnapshotState::LOCKED;
  observation.candidates = receipt_device12_matches;
  if (receipt_source.device11 == 0 && receipt_source.device12 == device12) {
    observation.matches = receipt_device12_matches;
    if (receipt_device12_matches == 1) observation.identity = receipt_source;
  }
  observation.identity.device12 = device12;
  return observation;
}
int32_t FakeReceiptPresentStatus(void*, xefg::abi::PresentStatus* status) {
  *status = {.frames_presented = 1, .frame_gen_result = 0, .enabled = 0};
  return 0;
}
int32_t ReceiptSdkCall() {
  Check(!xefg::QueryBridgeOff(receipt_source).available, "old Off cannot authorize while the SDK contract call is in flight");
  if (receipt_during_sdk != nullptr) {
    receipt_during_sdk();
  }
  return receipt_sdk_status;
}
int32_t FakeReceiptEnable(void*, uint32_t) { return ReceiptSdkCall(); }
int32_t FakeReceiptInit(void*, ID3D12CommandQueue*, const xefg::abi::Init*) { return ReceiptSdkCall(); }
int32_t FakeReceiptInitDesc(void*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    ID3D12CommandQueue*, IDXGIFactory2*, const xefg::abi::Init*) { return ReceiptSdkCall(); }
int32_t FakeReceiptGetSwapchain(void*, REFIID iid, void** output) {
  const auto status = ReceiptSdkCall();
  if (status != 0 || receipt_proxy == nullptr) return status;
  return receipt_proxy->QueryInterface(iid, output);
}
int32_t FakeReceiptDestroy(void*) { return ReceiptSdkCall(); }
uint32_t fake_present_calls = 0;
bool fake_present_owns_stream = false;
HRESULT STDMETHODCALLTYPE FakePresent(IDXGISwapChain*, UINT, UINT) {
  ++fake_present_calls;
  fake_present_owns_stream = xefg::OwnsStream();
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeSourcePresent(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
  return swapchain->Present(interval, flags);
}
HRESULT STDMETHODCALLTYPE FailedReceiptPresent(IDXGISwapChain*, UINT, UINT) { return E_FAIL; }
HRESULT STDMETHODCALLTYPE OccludedReceiptPresent(IDXGISwapChain*, UINT, UINT) { return DXGI_STATUS_OCCLUDED; }
HRESULT STDMETHODCALLTYPE FakePresent1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS*) {
  return adapter::ProxyHooks<0>::Present(swapchain, interval, flags);
}
struct Session {
  Gpu* gpu;
  ComPtr<IDXGISwapChain3> proxy;
  void* handle = this;
  std::shared_ptr<adapter::Context> context;
  uint32_t extent_left = 1;
  uint32_t extent_top = 2;
  uint32_t extent_width = 4;
  uint32_t extent_height = 3;
  explicit Session(Gpu* value, int32_t ui_mode = 0, uint32_t init_flags = 17,
                   DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) : gpu(value), proxy(value->Swapchain(format)) {
    xefg::abi::Init init{.application_swapchain = proxy.Get(), .flags = init_flags, .max_interpolated = 1, .ui_mode = ui_mode};
    Check(Hooks::Init(handle, gpu->queue.Get(), &init) == 0, "SDK init");
    context = adapter::state.contexts.at(handle);
    context->proxy_identity = adapter::Identity(proxy.Get());
    context->enabled = true;
    context->ui_composition = true;
    adapter::RefreshOwnership();
  }
  ~Session() {
    // The fake SDK has no Destroy callback. Do not leave a dead proxy's
    // recycled COM identity eligible for another session's Present.
    context->enabled = false;
    context->proxy_identity = 0;
    adapter::RefreshOwnership();
  }
  xefg::abi::Resource Tag(uint32_t id, int32_t type, ID3D12Resource* resource, int32_t validity = UNTIL_NEXT_PRESENT) {
    const xefg::abi::Resource data{.type = type, .validity = validity,
        .base = {extent_left, extent_top}, .size = {extent_width, extent_height},
        .resource = resource, .state = D3D12_RESOURCE_STATE_COMMON};
    Check(Hooks::Resource(handle, gpu->list.Get(), id, &data) == sdk_status, "SDK resource result forwarding");
    Check(data.resource == resource && data.validity == validity, "caller tag mutated");
    return data;
  }
  void Guides(uint32_t id, ID3D12Resource* depth, ID3D12Resource* motion) {
    Tag(id, DEPTH, depth);
    Tag(id, MOTION, motion);
    const xefg::abi::Constants constants{.jitter_x = .25f, .jitter_y = -.5f,
        .motion_scale_x = 8.f, .motion_scale_y = -8.f, .frame_time_ms = 16.5f};
    Check(Hooks::Constants(handle, id, &constants) == sdk_status, "SDK constants result forwarding");
  }
  void Present(uint32_t id, UINT flags = 0) {
    Check(Hooks::PresentId(handle, id) == sdk_status, "SDK present id result forwarding");
    Check(SUCCEEDED(adapter::ProxyHooks<0>::Present(proxy.Get(), 0, flags)), "proxy present forwarding");
  }
};

void TestRouteDiagnostics(Gpu* gpu) {
  Session session(gpu);
  auto hudless = gpu->Texture();
  auto depth = gpu->Texture();
  auto motion = gpu->Texture();
  uint32_t id = 1200;
  const auto verify = [&](const char* route, const char* reason, fg::Result outcome,
                          uint32_t hudless_calls, uint32_t backbuffer_calls, bool composed = false,
                          bool record_stats = true, bool tag_hudless = true, bool repeated = false) {
    service_outcome = outcome;
    session.Guides(++id, depth.Get(), motion.Get());
    if (tag_hudless) {
      session.Tag(id, HUDLESS, hudless.Get());
    }
    gpu->Submit();
    Check(Hooks::PresentId(session.handle, id) == 0, "route diagnostic Present ID");
    if (repeated) {
      session.context->last_id_known = true;
      session.context->last_id = id;
    }
    auto present = adapter::BeginPresent(session.proxy.Get(), 0);
    if (record_stats && present.service_eligible) {
      adapter::RecordServiceStats(present.input.diagnostic_context, hudless_calls + backbuffer_calls,
          hudless_calls + backbuffer_calls, hudless_calls, backbuffer_calls,
          hudless_calls != 0 && backbuffer_calls != 0, composed);
    }
    session.context->interval_start = std::chrono::steady_clock::now() - std::chrono::seconds(6);
    interval_diagnostic.clear();
    adapter::EndPresent(&present);
    Check(interval_diagnostic.find(std::string("last_nr_route=") + route + " ") != std::string::npos,
          "last route must match successful role counts, not available inputs or earlier frames");
    Check(interval_diagnostic.find(std::string("last_nr_route_reason=") + reason + ";") != std::string::npos,
          "last route reason must match the serviced frame contract");
    Check(interval_diagnostic.find("nr_service[frames=") != std::string::npos
              && interval_diagnostic.find("public_SDK_status_not_visual_acceptance") != std::string::npos,
          "route fields preserve legacy counters and the log suffix without truncation");
  };
  session.context->omit_optional_hudless = true;
  session.context->ui_composition = false;
  verify("single_backbuffer", "single_color_default", fg::Result::SUBMITTED, 0, 1);
  session.context->ui_composition = true;
  verify("dual_chain", "ui_composition", fg::Result::SUBMITTED, 1, 1);
  session.context->omit_optional_hudless = false;
  session.context->ui_composition = false;
  verify("dual_chain", "manual_dual", fg::Result::SUBMITTED, 1, 1);
  session.context->init.ui_mode = 4;
  verify("dual_chain", "ui_mode", fg::Result::SUBMITTED, 1, 1);
  session.context->ui_composition = true;
  for (const int32_t mode : {1, 2}) {
    session.context->init.ui_mode = mode;
    verify("single_backbuffer", "ui_mode", fg::Result::SUBMITTED, 0, 1);
    Check(service_calls.back().hudless == nullptr, "NONE/UItexture diagnostics must follow SDK consumers");
  }
  session.context->init.ui_mode = 3;
  verify("single_hudless_composed", "ui_composition", fg::Result::SUBMITTED, 1, 0, true);
  session.context->init.ui_mode = 0;
  session.context->ui_composition = false;
  verify("single_backbuffer", "hudless_unavailable", fg::Result::SUBMITTED, 0, 1, false, true, false);
  verify("not_processed", "service_stats_unavailable", fg::Result::SUBMITTED, 0, 0, false, false);
  verify("not_processed", "failed", fg::Result::FAILED, 1, 1);
  verify("not_processed", "not_served", fg::Result::PREPARED, 1, 1);
  verify("not_processed", "not_served", fg::Result::NOT_SERVED, 0, 0);
  verify("not_processed", "not_served", fg::Result::SUBMITTED, 0, 0, false, false, true, true);
  policy = false;
  verify("not_processed", "nr_off", fg::Result::SUBMITTED, 0, 0);
  policy = true;
  session.context->enabled = false;
  verify("not_processed", "fg_off", fg::Result::SUBMITTED, 0, 0);
  service_outcome = fg::Result::SUBMITTED;
  gpu->Submit();
  submission::ResetAll();
  std::cout << "PASS last Present route/reason diagnostics: successful roles, SDK consumers, Off, skipped and failed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const bool bridge_off_cpu = argc == 2 && std::strcmp(argv[1], "--bridge-off-cpu") == 0;
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    TestDiagnosticBudgets();
    std::cout << "PASS bounded XeFG contract diagnostics\n";
    fg::native_options::internal::state.real = &FakeOptions;
    fg::native_options::Configure(&OptionsChanged);
    sl::DLSSGOptions options;
    const sl::ViewportHandle viewport(0);
    options_result = sl::Result::eErrorInvalidParameter;
    Check(fg::native_options::internal::HookedSetOptions(viewport, options) == options_result
        && options_events == 0, "rejected native Off changed ownership");
    options_result = sl::Result::eOk;
    Check(fg::native_options::internal::HookedSetOptions(viewport, options) == sl::Result::eOk
        && options_events == 1 && !last_options_enabled, "successful native Off not forwarded");
    options.mode = sl::DLSSGMode::eOn;
    fg::native_options::internal::HookedSetOptions(viewport, options);
    Check(options_events == 2 && last_options_enabled, "native enable request left Off latched");
    fg::native_options::Stop();
    options.mode = sl::DLSSGMode::eOff;
    fg::native_options::internal::HookedSetOptions(viewport, options);
    Check(options_events == 2 && options_calls == 4, "stopped native hook did not pass through");
    std::cout << "PASS native options result, explicit Off/On and stopped pass-through\n";
    {
      Check(adapter::state.contexts.empty(), "CPU Off receipt starts without a live XeFG context");
      const auto saved_real = adapter::modules[0].real;
      ComPtr<IUnknown> identity;
      identity.Attach(new ReceiptDeviceIdentity());
      ComPtr<IUnknown> other_identity;
      other_identity.Attach(new ReceiptDeviceIdentity());
      auto* context_handle = reinterpret_cast<void*>(uintptr_t{0xEF001});
      receipt_source = {.device11 = reinterpret_cast<uintptr_t>(identity.Get()), .handle = 0xD311, .generation = 1};
      const std::array<std::pair<xefg::BridgeSourceObservation, const char*>, 6> classifications{{
          {{.queried_device11 = receipt_source.device11, .state = xefg::BridgeSnapshotState::RUNTIME_BUSY}, "runtime_busy"},
          {{.queried_device11 = receipt_source.device11, .state = xefg::BridgeSnapshotState::SHUTTING_DOWN}, "shutting_down"},
          {{.queried_device11 = receipt_source.device11, .state = xefg::BridgeSnapshotState::LOCKED}, "no_live_source"},
          {{.queried_device11 = receipt_source.device11, .state = xefg::BridgeSnapshotState::LOCKED, .candidates = 1}, "device_mismatch"},
          {{.identity = {.device11 = receipt_source.device11}, .queried_device11 = receipt_source.device11,
               .state = xefg::BridgeSnapshotState::LOCKED, .candidates = 2, .matches = 2}, "ambiguous"},
          {{.identity = receipt_source, .queried_device11 = receipt_source.device11,
               .state = xefg::BridgeSnapshotState::LOCKED, .candidates = 1, .matches = 1}, "matched"},
      }};
      for (const auto& [observation, expected] : classifications) {
        Check(std::strcmp(xefg::BridgeSourceReason(observation), expected) == 0,
            "source observation classifies unavailable locks, usable D3D11 candidates and exact matches");
      }
      const auto fresh_context = [&] {
        adapter::state.contexts.clear();
        auto context = std::make_shared<adapter::Context>();
        context->bridge_context_ready = context->bridge_proxy_ready = true;
        context->bridge_application_device11 = identity;
        context->bridge_proxy_device11 = identity;
        context->proxy_identity = 0xEF111;
        context->bridge_declaration = ++adapter::state.bridge_declaration;
        adapter::state.contexts[context_handle] = context;
        return context;
      };
      adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeReceiptInit);
      adapter::modules[0].real[1] = reinterpret_cast<void*>(&FakeReceiptInitDesc);
      adapter::modules[0].real[2] = reinterpret_cast<void*>(&FakeReceiptGetSwapchain);
      adapter::modules[0].real[6] = reinterpret_cast<void*>(&FakeReceiptEnable);
      adapter::modules[0].real[8] = reinterpret_cast<void*>(&FakeReceiptDestroy);
      xefg::ConfigureBridgeSource(&ObserveReceiptSource);
      auto context = fresh_context();
      {
        ComPtr<ReceiptSwapchain> native_swapchain;
        native_swapchain.Attach(new ReceiptSwapchain(identity.Get()));
        ComPtr<ReceiptSwapchain> wrapper;
        wrapper.Attach(new ReceiptSwapchain(other_identity.Get(), native_swapchain.Get()));
        const auto native_refs = native_swapchain->references;
        const auto wrapper_refs = wrapper->references;
        const auto captured = adapter::BridgeDeviceIdentity(wrapper.Get());
        Check(captured.Get() == identity.Get() && wrapper->get_device_calls == 0 && native_swapchain->get_device_calls == 1,
            "known ReShade swapchain base supplies the canonical source device before proxy GetDevice can select another face");
        Check(wrapper->references == wrapper_refs && native_swapchain->references == native_refs,
            "Bridge identity capture balances outer and borrowed-base swapchain references");
        xefg::BridgeDeviceObservation application_observation;
        Check(adapter::BridgeDeviceIdentity(nullptr, &application_observation) == nullptr
                  && !application_observation.object_present && application_observation.swapchain_result == E_PENDING,
            "missing application swapchain records unattempted queries rather than a D3D11 failure result");
        xefg::BridgeDeviceObservation proxy_observation;
        Check(adapter::BridgeDeviceIdentity(wrapper.Get(), &proxy_observation).Get() == identity.Get()
                  && proxy_observation.object_present && proxy_observation.swapchain_unwrapped
                  && proxy_observation.swapchain_result == S_OK && proxy_observation.device11_result == S_OK
                  && proxy_observation.device12_result == E_NOINTERFACE,
            "lock-free capture preserves the known ReShade unwrap and separate D3D11/D3D12 GetDevice results");
        context->bridge_application_device11.Reset();
        context->bridge_proxy_device11 = captured;
        context->bridge_init_kind = "InitDesc";
        context->bridge_application_observation = application_observation;
        context->bridge_proxy_observation = proxy_observation;
        const auto metadata_only = xefg::QueryBridgeOff(receipt_source);
        Check(!metadata_only.available && metadata_only.application == application_observation
                  && metadata_only.proxy_device == proxy_observation && std::strcmp(metadata_only.init_kind, "InitDesc") == 0,
            "captured host API metadata is visible but cannot itself authorize Off");
        Hooks::Enable(context_handle, 0);
        Check(xefg::QueryBridgeOff(receipt_source).available,
            "the captured matching native device still requires and accepts a successful same-source SDK Disable");
        context = fresh_context();
        ComPtr<ReceiptSwapchain> different_swapchain;
        different_swapchain.Attach(new ReceiptSwapchain(other_identity.Get()));
        context->bridge_application_device11.Reset();
        context->bridge_proxy_device11 = adapter::BridgeDeviceIdentity(different_swapchain.Get());
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "an unwrapped but genuinely different device cannot claim the source's Disable receipt");
        ComPtr<ReceiptSwapchain> device12_swapchain;
        device12_swapchain.Attach(new ReceiptSwapchain(nullptr));
        device12_swapchain->reported_device12 = other_identity;
        xefg::BridgeDeviceObservation device12_observation;
        Check(adapter::BridgeDeviceIdentity(device12_swapchain.Get(), &device12_observation) == nullptr
                  && device12_observation.device11_result == E_NOINTERFACE && device12_observation.device12_result == S_OK,
            "a D3D12-only application chain is observable without inventing a D3D11 source identity");
        context = fresh_context();
        for (const auto& [supports_device11, supports_device12] : std::array<std::pair<bool, bool>, 4>{
                 {{false, false}, {false, true}, {true, false}, {true, true}}}) {
          ComPtr<ReceiptDeviceIdentity> returned_device;
          returned_device.Attach(new ReceiptDeviceIdentity());
          returned_device->supports_device11 = supports_device11;
          returned_device->supports_device12 = supports_device12;
          ComPtr<ReceiptSwapchain> reported_swapchain;
          reported_swapchain.Attach(new ReceiptSwapchain(returned_device.Get()));
          if (supports_device12) {
            reported_swapchain->reported_device12 = returned_device.Get();
          }
          const auto device_refs = returned_device->references;
          const auto swapchain_refs = reported_swapchain->references;
          xefg::BridgeDeviceObservation typed_observation;
          auto captured_device = adapter::BridgeDeviceIdentity(reported_swapchain.Get(), &typed_observation);
          const auto expected_identity = reinterpret_cast<uintptr_t>(static_cast<IUnknown*>(returned_device.Get()));
          Check(typed_observation.device11_result == S_OK
                    && typed_observation.device12_result == (supports_device12 ? S_OK : E_NOINTERFACE)
                    && typed_observation.returned_device_identity == expected_identity
                    && typed_observation.device11_interface_result == (supports_device11 ? S_OK : E_NOINTERFACE)
                    && typed_observation.device12_interface_result == (supports_device12 ? S_OK : E_NOINTERFACE)
                    && typed_observation.device11_interface_identity == (supports_device11 ? expected_identity : 0)
                    && typed_observation.device12_interface_identity == (supports_device12 ? expected_identity : 0),
              "returned-device diagnostics separate getter success, explicit interface capability and canonical identity");
          Check(captured_device.Get() == returned_device.Get(),
              "diagnostic interface queries preserve the existing getter-based classification for all four capabilities");
          context->bridge_proxy_observation = typed_observation;
          const auto typed_evidence = xefg::QueryBridgeOff(receipt_source);
          Check(!typed_evidence.available && typed_evidence.proxy_device == typed_observation,
              "returned-device interface metadata cannot independently grant an Off receipt");
          captured_device.Reset();
          const auto query11_before = returned_device->device11_queries;
          const auto query12_before = returned_device->device12_queries;
          auto identity_only = adapter::BridgeDeviceIdentity(reported_swapchain.Get());
          Check(returned_device->device11_queries == query11_before && returned_device->device12_queries == query12_before,
              "captures without an observation do not add per-Present device-interface queries");
          identity_only.Reset();
          Check(returned_device->references == device_refs && reported_swapchain->references == swapchain_refs,
              "successful and rejected interface probes balance device and swapchain references");
        }
        std::cout << "PASS CPU MOCK returned-device diagnostics: IUnknown/D3D12/D3D11/dual, unchanged admission and balanced references; GPU=NotRun\n";
      }
      Check(!xefg::QueryBridgeOff(receipt_source).available, "default enabled=false is not an observed Disable");
      receipt_sdk_status = -19;
      Check(Hooks::Enable(context_handle, 0) == -19 && !xefg::QueryBridgeOff(receipt_source).available,
          "failed SetEnabled(false) cannot publish Off");
      receipt_sdk_status = 0;
      receipt_source_available = false;
      receipt_during_sdk = +[] { receipt_source_available = true; };
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "a source arriving during Disable cannot claim that declaration");
      receipt_during_sdk = nullptr;
      Hooks::Enable(context_handle, 0);
      const auto accepted = xefg::QueryBridgeOff(receipt_source);
      Check(accepted.available && accepted.source == receipt_source && accepted.epoch == context->epoch,
          "successful false with the same captured context/proxy/source publishes a current receipt");
      receipt_during_sdk = +[] { receipt_candidates = 2; };
      Hooks::Enable(context_handle, 0);
      const auto metadata_changed = xefg::QueryBridgeOff(receipt_source);
      Check(metadata_changed.available && metadata_changed.before.identity == metadata_changed.after.identity
                && metadata_changed.before.candidates == 1 && metadata_changed.after.candidates == 2
                && metadata_changed.before != metadata_changed.after,
          "diagnostic candidate changes do not alter the original before/after identity-only admission comparison");
      receipt_during_sdk = nullptr;
      receipt_candidates = 1;
      auto wrong_source = receipt_source;
      ++wrong_source.generation;
      Check(!xefg::QueryBridgeOff(wrong_source).available, "a rebuilt source cannot inherit an older Disable");
      wrong_source = receipt_source;
      ++wrong_source.device11;
      Check(!xefg::QueryBridgeOff(wrong_source).available, "a different D3D11 device cannot inherit Off");
      receipt_source_available = false;
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "unavailable source snapshot revokes the prior Off receipt");
      Check(std::strcmp(xefg::BridgeSourceReason(xefg::QueryBridgeOff(receipt_source).before), "runtime_busy") == 0,
          "a busy observer is diagnosed and its empty identity still revokes Off immediately");
      receipt_source_available = true;
      Hooks::Enable(context_handle, 0);
      Hooks::Enable(context_handle, 1);
      Check(context->enabled && !xefg::QueryBridgeOff(receipt_source).available, "reenable immediately revokes Off");
      Hooks::Enable(context_handle, 0);
      Check(xefg::QueryBridgeOff(receipt_source).available, "a fresh successful Disable recovers after reenable");
      receipt_during_sdk = +[] { ++receipt_source.generation; };
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "source generation changing inside SDK call rejects the result");
      receipt_during_sdk = nullptr;
      Hooks::Enable(context_handle, 0);
      adapter::ReleaseFrames(context.get());
      Check(!xefg::QueryBridgeOff(receipt_source).available, "context epoch changes invalidate an existing receipt");
      Hooks::Enable(context_handle, 0);
      adapter::state.contexts[reinterpret_cast<void*>(uintptr_t{0xEF002})] = std::make_shared<adapter::Context>();
      Check(!xefg::QueryBridgeOff(receipt_source).available, "multiple XeFG contexts are ambiguous");
      context = fresh_context();
      context->bridge_proxy_device11 = other_identity;
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "conflicting original/proxy D3D11 identities reject Off");
      context = fresh_context();
      context->bridge_application_device11.Reset();
      context->bridge_proxy_device11.Reset();
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "unknown D3D11 identity cannot infer the source from a private D3D12 context");
      context = fresh_context();
      Hooks::Enable(context_handle, 0);
      receipt_sdk_status = -19;
      Hooks::Init(context_handle, nullptr, nullptr);
      Check(!xefg::QueryBridgeOff(receipt_source).available && !context->bridge_context_ready,
          "failed Init revokes old Off before entering SDK");
      context = fresh_context();
      receipt_sdk_status = 0;
      Hooks::Enable(context_handle, 0);
      receipt_sdk_status = -19;
      Hooks::InitDesc(context_handle, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
      Check(!xefg::QueryBridgeOff(receipt_source).available && !context->bridge_context_ready,
          "failed InitDesc revokes old Off before entering SDK");
      context = fresh_context();
      receipt_sdk_status = 0;
      Hooks::Enable(context_handle, 0);
      receipt_sdk_status = -19;
      void* proxy = nullptr;
      Hooks::GetSwapchain(context_handle, IID_IUnknown, &proxy);
      Check(!xefg::QueryBridgeOff(receipt_source).available && !context->bridge_proxy_ready,
          "failed proxy observation revokes old Off before entering SDK");
      context = fresh_context();
      receipt_sdk_status = 0;
      Hooks::Enable(context_handle, 0);
      receipt_sdk_status = -19;
      Hooks::Destroy(context_handle);
      Check(!xefg::QueryBridgeOff(receipt_source).available && !context->bridge_context_ready,
          "failed Destroy cannot leave an old Off receipt valid");
      context = fresh_context();
      receipt_sdk_status = 0;
      Hooks::Enable(context_handle, 0);
      adapter::state.contexts.erase(context_handle);
      Hooks::Enable(context_handle, 0);
      Check(!xefg::QueryBridgeOff(receipt_source).available, "a missing tracked context is never positive Off evidence");
      context = fresh_context();
      Check(!xefg::QueryBridgeOff(receipt_source).available, "replacement context starts without inherited Off");
      Hooks::Enable(context_handle, 0);
      Check(xefg::QueryBridgeOff(receipt_source).available, "replacement context can accept its own fresh Disable");
      {
        context = fresh_context();
        ComPtr<ReceiptSwapchain> source_swapchain;
        source_swapchain.Attach(new ReceiptSwapchain(identity.Get()));
        ComPtr<ReceiptSwapchain> sdk_proxy;
        sdk_proxy.Attach(new ReceiptSwapchain(other_identity.Get()));
        receipt_proxy = sdk_proxy.Get();
        receipt_context_handle = context_handle;
        context->bridge_application_device11.Reset();
        context->bridge_proxy_device11 = other_identity;
        context->proxy_identity = adapter::Identity(sdk_proxy.Get());
        const auto saved_proxy_real = adapter::proxies[0].real;
        const auto saved_source_real = adapter::source_present_slots[0].real;
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        adapter::source_present_slots[0].real[0] = reinterpret_cast<void*>(&FakeSourcePresent);
        const auto register_source = [&](ReceiptSwapchain* swapchain, uintptr_t owner) {
          auto source = std::make_shared<adapter::BridgeSourceSwapchain>();
          source->owner = owner;
          source->identity = adapter::Identity(swapchain);
          source->device11 = adapter::BridgeDeviceIdentity(swapchain);
          source->generation = ++adapter::state.bridge_source_generation;
          source->hook_ready = true;  // MOCK registration; no code detour or GPU is installed.
          adapter::state.bridge_swapchains[owner] = source;
          return source;
        };
        constexpr uintptr_t SOURCE_OWNER = 0xED001;
        auto registration = register_source(source_swapchain.Get(), SOURCE_OWNER);
        const auto source_refs = source_swapchain->references;
        const auto normal_present = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          return adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
        };
        source_swapchain->present_callback = normal_present;
        const auto run_source = [&](UINT flags = 0) {
          return adapter::SourcePresentHooks<0>::Present(source_swapchain.Get(), 0, flags);
        };
        const auto accept_link = [&] {
          Check(run_source() == S_OK, "source wrapper forwards its exact successful SDK proxy call");
          Hooks::Enable(context_handle, 0);
          Check(xefg::QueryBridgeOff(receipt_source).available, "a proven host link still requires a fresh successful Disable");
        };
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "different device identities cannot infer a host link from the sole registered source");
        adapter::ProxyHooks<0>::Present(sdk_proxy.Get(), 0, 0);
        Check(context->bridge_source_swapchain == 0, "an SDK proxy outside the source call stack does not establish a link");
        Check(run_source(DXGI_PRESENT_TEST) == S_OK && context->bridge_source_swapchain == 0,
            "test Presents do not establish a source link");
        accept_link();
        const auto linked = xefg::QueryBridgeOff(receipt_source);
        Check(linked.uses_source_link && linked.source_swapchain == SOURCE_OWNER
                  && linked.source_swapchain_generation == registration->generation
                  && linked.proxy_device11 == reinterpret_cast<uintptr_t>(other_identity.Get())
                  && source_swapchain->references == source_refs,
            "host mapping preserves the different raw SDK device and never retains the source swapchain");
        Check(run_source() == S_OK && xefg::QueryBridgeOff(receipt_source).available
                  && context->bridge_link_generation == linked.source_link_generation,
            "repeated identical successful calls do not churn an existing link or Disable receipt");
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&OccludedReceiptPresent);
        Check(run_source() == DXGI_STATUS_OCCLUDED && xefg::QueryBridgeOff(receipt_source).available
                  && context->bridge_link_generation == linked.source_link_generation,
            "a normal positive Present status preserves the call relation without claiming image delivery");
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          const auto status = adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          Hooks::Enable(receipt_context_handle, 1);
          Hooks::Enable(receipt_context_handle, 0);
          return status;
        };
        Check(run_source() == S_OK && xefg::QueryBridgeOff(receipt_source).available
                  && context->bridge_link_generation == linked.source_link_generation,
            "an interleaved Enable/Disable owns its new receipt without an old Present candidate revoking the stable link");
        source_swapchain->present_callback = normal_present;
        receipt_source_available = false;
        Check(run_source() == S_OK && xefg::QueryBridgeOff(receipt_source).available,
            "a busy nonblocking observer cannot grant a link or revoke an unchanged proven one");
        receipt_source_available = true;
        {
          adapter::BridgeSourcePresent present1(source_swapchain.Get(), 0);
          Check(run_source() == S_OK, "same-wrapper nested Present forwarding");
          present1.Finish(S_OK);
        }
        Check(context->bridge_link_generation == linked.source_link_generation,
            "the same wrapper's Present1-to-Present nesting counts one exact SDK call");
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FailedReceiptPresent);
        Check(run_source() == E_FAIL && !xefg::QueryBridgeOff(receipt_source).available
                  && std::strcmp(xefg::QueryBridgeOff(receipt_source).source_link_reason, "proxy_present_failed") == 0,
            "a failed inner SDK Present revokes the source link with a distinct reason");
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        accept_link();
        source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) -> HRESULT {
          adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          throw std::runtime_error("MOCK source Present exception");
        };
        bool caught = false;
        try { run_source(); } catch (const std::runtime_error&) { caught = true; }
        Check(caught && adapter::BridgeSourcePresent::current == nullptr && !xefg::QueryBridgeOff(receipt_source).available,
            "an exceptional outer return restores TLS and cannot preserve a mapped Disable receipt");
        source_swapchain->present_callback = normal_present;
        accept_link();
        source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          HRESULT status = E_FAIL;
          std::thread worker([&] {
            status = adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          });
          worker.join();
          return status;
        };
        Check(run_source() == S_OK && !xefg::QueryBridgeOff(receipt_source).available,
            "another thread's SDK Present cannot establish the caller's source association");
        source_swapchain->present_callback = normal_present;
        accept_link();
        ComPtr<ReceiptSwapchain> other_proxy;
        other_proxy.Attach(new ReceiptSwapchain(other_identity.Get()));
        receipt_secondary_proxy = other_proxy.Get();
        auto second_context = std::make_shared<adapter::Context>();
        second_context->bridge_context_ready = second_context->bridge_proxy_ready = true;
        second_context->proxy_identity = adapter::Identity(other_proxy.Get());
        second_context->bridge_declaration = ++adapter::state.bridge_declaration;
        adapter::state.contexts[reinterpret_cast<void*>(uintptr_t{0xEF002})] = second_context;
        source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          return adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_secondary_proxy), interval, flags);
        };
        Check(run_source() == S_OK && context->bridge_source_swapchain == 0,
            "two exact SDK proxies in one source call are ambiguous rather than last-write-wins");
        adapter::state.contexts.erase(reinterpret_cast<void*>(uintptr_t{0xEF002}));
        Check(!xefg::QueryBridgeOff(receipt_source).available, "removing a second context cannot revive the revoked receipt");
        source_swapchain->present_callback = normal_present;
        accept_link();
        ComPtr<ReceiptSwapchain> second_source;
        second_source.Attach(new ReceiptSwapchain(identity.Get()));
        second_source->present_callback = normal_present;
        register_source(second_source.Get(), SOURCE_OWNER + 1);
        Check(adapter::SourcePresentHooks<0>::Present(second_source.Get(), 0, 0) == S_OK
                  && context->bridge_source_swapchain == 0,
            "two different source wrappers calling one proxy revoke the previous association");
        Check(run_source() == S_OK && context->bridge_source_swapchain == 0,
            "subsequent calls cannot overwrite an unresolved multiple-source association");
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER + 1);
        accept_link();
        source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          const auto status = adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          ++receipt_source.generation;
          return status;
        };
        Check(run_source() == S_OK && !xefg::QueryBridgeOff(receipt_source).available,
            "a source rebuilt inside Present cannot receive the old source generation's link");
        source_swapchain->present_callback = normal_present;
        accept_link();
        receipt_during_sdk = +[] { ++receipt_source.generation; };
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "mapped devices retain the original before/after source-generation Disable requirement");
        receipt_during_sdk = nullptr;
        accept_link();
        {
          adapter::BridgeSourcePresent pending(source_swapchain.Get(), 0);
          adapter::ProxyHooks<0>::Present(sdk_proxy.Get(), 0, 0);
          xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
          registration = register_source(source_swapchain.Get(), SOURCE_OWNER);
          pending.Finish(S_OK);
        }
        Check(context->bridge_source_swapchain == 0 && !xefg::QueryBridgeOff(receipt_source).available,
            "resize/re-registration during Present cannot grant to a replacement registration at the same address");
        accept_link();
        Hooks::Enable(context_handle, 1);
        Check(!xefg::QueryBridgeOff(receipt_source).available, "reenabling revokes a mapped Off receipt");
        Hooks::Enable(context_handle, 0);
        Check(xefg::QueryBridgeOff(receipt_source).available, "a current link can receive its own fresh Disable after reenable");
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
        Check(!xefg::QueryBridgeOff(receipt_source).available && source_swapchain->references == source_refs,
            "source destroy revokes its mapped Off and leaves the source swapchain's COM ownership unchanged");
        adapter::proxies[0].real = saved_proxy_real;
        adapter::source_present_slots[0].real = saved_source_real;
        receipt_proxy = receipt_secondary_proxy = nullptr;
        const auto register_release_probe = [&](uintptr_t owner) {
          receipt_device_destroyed = receipt_device_release_unlocked = false;
          ComPtr<ReceiptDeviceIdentity> device;
          device.Attach(new ReceiptDeviceIdentity());
          device->on_destroy = +[] {
            std::thread probe([] {
              std::unique_lock lock(adapter::state.mutex, std::try_to_lock);
              receipt_device_destroyed.store(true);
              receipt_device_release_unlocked.store(lock.owns_lock());
            });
            probe.join();
          };
          auto source = std::make_shared<adapter::BridgeSourceSwapchain>();
          source->owner = owner;
          source->device11 = device;
          adapter::state.bridge_swapchains[owner] = std::move(source);
        };
        register_release_probe(SOURCE_OWNER);
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
        Check(receipt_device_destroyed && receipt_device_release_unlocked,
            "source unregister releases the last device reference after unlocking XeFG state");
        register_release_probe(SOURCE_OWNER);
        xefg::Stop();
        Check(receipt_device_destroyed && receipt_device_release_unlocked && adapter::state.bridge_swapchains.empty(),
            "Stop retires registered devices outside the state lock");
        adapter::stopped.store(false);
        std::cout << "PASS CPU MOCK source Present association: nested SDK boundary, identity, failure, ambiguity and teardown\n";
      }
      {
        const auto saved_proxy_real = adapter::proxies[0].real;
        auto& source_slot = adapter::source_present_slots[0];
        const auto saved_source_real = source_slot.real;
        const auto saved_source_target = source_slot.records[0].target;
        const bool saved_source_installed = source_slot.installed;
        const bool saved_source_complete = source_slot.complete;
        ComPtr<ReceiptSwapchain> source_swapchain;
        source_swapchain.Attach(new ReceiptSwapchain(nullptr));
        source_swapchain->reported_device12 = identity;
        ComPtr<ReceiptSwapchain> sdk_proxy;
        sdk_proxy.Attach(new ReceiptSwapchain(nullptr));
        sdk_proxy->reported_device12 = identity;
        receipt_proxy = sdk_proxy.Get();
        receipt_context_handle = context_handle;
        receipt_source = {.handle = 0xD312, .generation = 1,
            .device12 = reinterpret_cast<uintptr_t>(identity.Get())};
        xefg::ConfigureD3D12Source(&ObserveReceiptD3D12Source);
        context = fresh_context();
        context->bridge_application_device11.Reset();
        context->bridge_proxy_device11.Reset();
        context->device_identity = identity;
        context->proxy_identity = adapter::Identity(sdk_proxy.Get());
        context->get_status = &FakeReceiptPresentStatus;
        adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
        // Reuse a MOCK installed slot, so registration exercises its real
        // bookkeeping without installing a detour or creating a GPU device.
        source_slot.real[0] = reinterpret_cast<void*>(&FakeSourcePresent);
        source_slot.records[0].target = reinterpret_cast<uint8_t*>((*reinterpret_cast<void***>(source_swapchain.Get()))[8]);
        source_slot.installed = source_slot.complete = true;
        constexpr uintptr_t SOURCE_OWNER = 0xED012;
        const auto source_refs = source_swapchain->references;
        const auto register_source = [&] {
          Check(xefg::RegisterD3D12SourceSwapchain(SOURCE_OWNER, source_swapchain.Get()),
              "D3D12 source registration reuses its captured native device and installed source hook");
        };
        const auto normal_present = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
          return adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
        };
        source_swapchain->present_callback = normal_present;
        const auto run_source = [&](UINT flags = 0) {
          return adapter::SourcePresentHooks<0>::Present(source_swapchain.Get(), 0, flags);
        };
        const auto accept_link = [&] {
          Check(run_source() == S_OK, "D3D12 source forwards one successful nested SDK proxy Present");
          Hooks::Enable(context_handle, 0);
          Check(xefg::QueryBridgeOff(receipt_source).available,
              "D3D12 linked source accepts its own successful explicit Disable");
        };
        register_source();
        Check(adapter::BridgeDeviceIdentity(source_swapchain.Get()) == nullptr
                  && adapter::D3D12SourceDeviceIdentity(source_swapchain.Get()) == identity,
            "D3D12 identity stays separate from the established D3D11 capture contract");
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "same device and a unique D3D12 source cannot substitute for a nested source/proxy link");
        adapter::ProxyHooks<0>::Present(sdk_proxy.Get(), 0, 0);
        const auto sdk_output = renodx::addons::dlss5::output_framerate::Read(
            renodx::addons::dlss5::output_framerate::NowNs());
        Check(sdk_output.fg_off && sdk_output.identity == reinterpret_cast<uintptr_t>(context.get())
                  && !xefg::QueryBridgeOff(receipt_source).available,
            "SDK Off samples expose the real output context token without inventing a Disable receipt");
        Check(run_source(DXGI_PRESENT_TEST) == S_OK && context->bridge_source_swapchain == 0,
            "D3D12 test Presents cannot establish the source/proxy relation");
        accept_link();
        const auto linked = xefg::QueryBridgeOff(receipt_source);
        Check(linked.context == reinterpret_cast<uintptr_t>(context_handle)
                  && linked.context_identity == sdk_output.identity && linked.uses_source_link
                  && linked.source == receipt_source && linked.source_swapchain == SOURCE_OWNER
                  && linked.source.device11 == 0 && source_swapchain->references == source_refs
                  && linked.source_present_thread_id == GetCurrentThreadId() && linked.source_present_proxy_entry_depth == 0,
            "D3D12 Off evidence binds SDK handle, output identity, source generation and borrowed source swapchain");
        auto matched_output = sdk_output;
        matched_output.available = true;  // MOCK ready window; no wall-clock/GPU measurement.
        Check(linked.MatchesOutput(matched_output), "D3D12 Off receipt accepts a ready Off SDK window from its exact context");
        ++matched_output.identity;
        Check(!linked.MatchesOutput(matched_output), "D3D12 Off receipt rejects another context's SDK FPS");
        matched_output.identity = linked.context_identity;
        matched_output.source = renodx::addons::dlss5::output_framerate::Source::PRESENT;
        Check(!linked.MatchesOutput(matched_output), "D3D12 Off receipt does not relabel Present cadence as SDK output");
        matched_output.source = sdk_output.source;
        matched_output.fg_off = false;
        Check(!linked.MatchesOutput(matched_output), "D3D12 Off receipt rejects SDK windows without Off samples");
        matched_output.fg_off = true;
        matched_output.available = false;
        Check(!linked.MatchesOutput(matched_output), "D3D12 Off receipt rejects a warming or stale SDK window");
        matched_output.available = true;
        auto unobserved_off = linked;
        unobserved_off.available = false;
        Check(!unobserved_off.MatchesOutput(matched_output), "ready SDK Off samples cannot replace an explicit Disable receipt");
        Check(run_source() == S_OK && Hooks::Enable(context_handle, 0) == 0
                  && xefg::QueryBridgeOff(receipt_source).available
                  && context->epoch == linked.epoch && context->bridge_link_generation == linked.source_link_generation
                  && context->bridge_declaration != linked.declaration,
            "repeated identical Disable calls preserve the stable epoch/link cache key");
        receipt_sdk_status = -19;
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "D3D12 failed Disable revokes the old receipt despite Off status samples");
        receipt_sdk_status = 0;
        accept_link();
        receipt_source_available = false;
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "a busy D3D12 observer cannot preserve the previous Disable receipt");
        receipt_source_available = true;
        accept_link();
        receipt_during_sdk = +[] { ++receipt_source.generation; };
        Hooks::Enable(context_handle, 0);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "D3D12 source generation changing inside Disable rejects the result");
        receipt_during_sdk = nullptr;
        accept_link();
        receipt_device12_matches = 2;
        Check(run_source() == S_OK && !xefg::QueryBridgeOff(receipt_source).available,
            "multiple D3D12 SR sources on one device revoke the established relation");
        receipt_device12_matches = 1;
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "removing the source ambiguity cannot revive its old Disable receipt");
        accept_link();
        ComPtr<ReceiptSwapchain> second_source;
        second_source.Attach(new ReceiptSwapchain(nullptr));
        second_source->reported_device12 = identity;
        Check(xefg::RegisterD3D12SourceSwapchain(SOURCE_OWNER + 1, second_source.Get())
                  && !xefg::QueryBridgeOff(receipt_source).available,
            "a second registered D3D12 chain immediately revokes same-device Off before it presents");
        Check(run_source() == S_OK && context->bridge_source_swapchain == 0,
            "a proven chain cannot overwrite same-device D3D12 chain ambiguity");
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER + 1);
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "removing the second D3D12 chain does not revive a revoked receipt");
        accept_link();
        context->device_identity = other_identity;
        Check(run_source() == S_OK && !xefg::QueryBridgeOff(receipt_source).available,
            "a nested proxy on another D3D12 device cannot authorize the source");
        context->device_identity = identity;
        accept_link();
        Hooks::Enable(context_handle, 1);
        Check(!xefg::QueryBridgeOff(receipt_source).available, "D3D12 Enable immediately revokes explicit Off");
        Hooks::Enable(context_handle, 0);
        Check(xefg::QueryBridgeOff(receipt_source).available && context->epoch != linked.epoch,
            "D3D12 reenable/disable changes the epoch before accepting a new receipt");
        adapter::BeforeResize(sdk_proxy.Get());
        Check(!xefg::QueryBridgeOff(receipt_source).available,
            "D3D12 SDK proxy resize revokes its source link and Disable receipt");
        accept_link();
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
        Check(!xefg::QueryBridgeOff(receipt_source).available && source_swapchain->references == source_refs,
            "D3D12 source teardown revokes Off without retaining the swapchain");
        sdk_proxy->present_callback = normal_present;
        Check(xefg::RegisterD3D12SourceSwapchain(SOURCE_OWNER, sdk_proxy.Get()),
            "MOCK can register the SDK proxy itself to test rejection at the nested boundary");
        Check(adapter::SourcePresentHooks<0>::Present(sdk_proxy.Get(), 0, 0) == S_OK
                  && context->bridge_source_swapchain == 0 && !xefg::QueryBridgeOff(receipt_source).available,
            "D3D12 SDK proxy cannot self-certify as an independent source chain");
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
        register_source();
        accept_link();
        {
          // Reverse the same callbacks: the SDK proxy now presents the
          // registered D3D12 chain inside its own original Present call.
          const auto saved_secondary_real = adapter::proxies[1].real[0];
          adapter::proxies[1].real[0] = reinterpret_cast<void*>(&FakePresent);
          adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakeSourcePresent);
          receipt_inner_source = source_swapchain.Get();
          const auto leaf_present = +[](ReceiptSwapchain*, UINT, UINT) { return S_OK; };
          source_swapchain->present_callback = leaf_present;
          const auto reverse_present = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            return adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
          };
          sdk_proxy->present_callback = reverse_present;
          const auto run_proxy = [&](UINT flags = 0) {
            return adapter::ProxyHooks<0>::Present(sdk_proxy.Get(), 0, flags);
          };
          const auto accept_reverse = [&] {
            Check(run_proxy() == S_OK && Hooks::Enable(context_handle, 0) == 0
                      && xefg::QueryBridgeOff(receipt_source).available,
                "one successful reverse D3D12 proxy/source call accepts its own public Disable");
          };
          adapter::BeforeResize(sdk_proxy.Get());
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            const auto status = adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            Check(adapter::state.contexts.at(receipt_context_handle)->bridge_source_swapchain == 0,
                "a completed inner D3D12 source cannot grant a link before the outer SDK proxy returns");
            return status;
          };
          accept_reverse();
          sdk_proxy->present_callback = reverse_present;
          const auto reverse_link = xefg::QueryBridgeOff(receipt_source);
          Check(std::strcmp(reverse_link.source_link_reason, "proxy_to_source_mapping_observed") == 0,
              "reverse mapping reports its actual observed call direction as metadata");
          Check(reverse_link.source_present_thread_id == GetCurrentThreadId()
                    && reverse_link.source_present_proxy_entry_depth == 1,
              "reverse source metadata records its actual entry thread and SDK proxy depth");
          accept_reverse();
          Check(xefg::QueryBridgeOff(receipt_source).source_link_generation == reverse_link.source_link_generation
                    && source_swapchain->references == source_refs,
              "repeated reverse Presents retain the link generation without owning the source swapchain");
          {
            // Function detours also see unregistered objects sharing the same
            // Present implementation; their scope must not hide the SDK call.
            ComPtr<ReceiptSwapchain> unregistered_outer;
            unregistered_outer.Attach(new ReceiptSwapchain(nullptr));
            unregistered_outer->reported_device12 = identity;
            unregistered_outer->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
              return adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
            };
            ComPtr<ReceiptSwapchain> unregistered_inner;
            unregistered_inner.Attach(new ReceiptSwapchain(nullptr));
            unregistered_inner->reported_device12 = identity;
            unregistered_inner->present_callback = leaf_present;
            Check(adapter::Identity(unregistered_outer.Get()) != adapter::Identity(sdk_proxy.Get())
                      && adapter::Identity(unregistered_outer.Get()) != adapter::Identity(source_swapchain.Get())
                      && adapter::Identity(sdk_proxy.Get()) != adapter::Identity(source_swapchain.Get()),
                "unregistered outer, SDK proxy and registered source use different IUnknown identities");
            const auto run_wrapped_proxy = [&] {
              return adapter::SourcePresentHooks<0>::Present(unregistered_outer.Get(), 0, 0);
            };
            adapter::BeforeResize(sdk_proxy.Get());
            receipt_inner_source = unregistered_inner.Get();
            Check(run_wrapped_proxy() == S_OK && Hooks::Enable(context_handle, 0) == 0
                      && !xefg::QueryBridgeOff(receipt_source).available && context->bridge_source_swapchain == 0,
                "an unregistered outer and unregistered inner cannot certify a D3D12 source relation");
            receipt_inner_source = source_swapchain.Get();
            sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
              adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
              return adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            };
            Check(run_wrapped_proxy() == S_OK && Hooks::Enable(context_handle, 0) == 0
                      && !xefg::QueryBridgeOff(receipt_source).available && context->bridge_source_swapchain == 0,
                "an unregistered outer does not authorize two registered inner source Presents");
            std::cout << "PASS CPU MOCK unregistered-scope safety: missing registered inner and duplicate inner remain rejected\n";
            sdk_proxy->present_callback = reverse_present;
            source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT, UINT) {
              Check(adapter::proxy_entry_depth == 1,
                  "the registered source runs inside the actual instrumented SDK proxy call");
              Check(adapter::state.contexts.at(receipt_context_handle)->bridge_source_swapchain == 0,
                  "the unregistered-wrapper reverse candidate stays pending until outer SDK success");
              std::cout << "OBS CPU MOCK unregistered-scope inner: proxy_entry_depth=" << adapter::proxy_entry_depth
                        << " reverse_scope=" << (adapter::D3D12ProxyPresentObservation::current != nullptr) << '\n';
              return S_OK;
            };
            const auto presents_before = context->application_frame;
            const auto status = run_wrapped_proxy();
            const auto disable = Hooks::Enable(context_handle, 0);
            const auto wrapped_link = xefg::QueryBridgeOff(receipt_source);
            std::cout << "OBS CPU MOCK unregistered-scope result: present_status=" << status << " disable_status=" << disable
                      << " sdk_application_presents=" << context->application_frame - presents_before
                      << " source_link_reason=" << wrapped_link.source_link_reason
                      << " source_link_generation=" << wrapped_link.source_link_generation
                      << " available=" << wrapped_link.available << "; GPU=NotRun\n";
            Check(status == S_OK && disable == 0 && wrapped_link.available
                      && std::strcmp(wrapped_link.source_link_reason, "proxy_to_source_mapping_observed_unregistered_outer") == 0
                      && wrapped_link.source_present_thread_id == GetCurrentThreadId()
                      && wrapped_link.source_present_proxy_entry_depth == 1,
                "an unregistered source detour wrapper must not hide the strict reverse D3D12 source/proxy relation");
            source_swapchain->present_callback = leaf_present;
          }
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            return E_FAIL;
          };
          Check(run_proxy() == E_FAIL && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "a successful inner source cannot preserve proof after its outer proxy fails");
          sdk_proxy->present_callback = reverse_present;
          accept_reverse();
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            return adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
          };
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "two inner D3D12 source Presents cannot certify a one-to-one reverse relation");
          sdk_proxy->present_callback = reverse_present;
          accept_reverse();
          source_swapchain->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            return adapter::ProxyHooks<1>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
          };
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available,
              "another proxy entry inside the reverse source revokes the one-to-one proof");
          source_swapchain->present_callback = leaf_present;
          accept_reverse();
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            const auto status = adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            ++receipt_source.generation;
            return status;
          };
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "a source rebuilt after the inner return cannot receive the outer proxy's pending link");
          adapter::BeforeResize(sdk_proxy.Get());
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            const auto status = adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            ++adapter::state.contexts.at(receipt_context_handle)->epoch;
            return status;
          };
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "an epoch change after the inner return cannot accept a pending reverse link");
          sdk_proxy->present_callback = reverse_present;
          accept_reverse();
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
            const auto status = adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            adapter::state.contexts.at(receipt_context_handle) = std::make_shared<adapter::Context>();
            return status;
          };
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "a replacement context at the same SDK handle cannot receive a pending reverse link");
          adapter::state.contexts[context_handle] = context;
          sdk_proxy->present_callback = reverse_present;
          accept_reverse();
          sdk_proxy->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) -> HRESULT {
            adapter::SourcePresentHooks<0>::Present(receipt_inner_source, interval, flags);
            throw std::runtime_error("MOCK outer SDK proxy exception");
          };
          bool caught = false;
          try { run_proxy(); } catch (const std::runtime_error&) { caught = true; }
          Check(caught && !xefg::QueryBridgeOff(receipt_source).available
                    && adapter::BridgeSourcePresent::current == nullptr
                    && adapter::D3D12ProxyPresentObservation::current == nullptr && adapter::proxy_entry_depth == 0,
              "an exceptional reverse outer return revokes proof and restores all observation TLS");
          sdk_proxy->present_callback = reverse_present;
          accept_reverse();
          sdk_proxy->present_callback = leaf_present;
          Check(run_proxy() == S_OK && !xefg::QueryBridgeOff(receipt_source).available
                    && context->bridge_source_swapchain == 0,
              "a later SDK proxy without its registered inner source revokes an earlier reverse proof");
          sdk_proxy->present_callback = reverse_present;
          Check(run_proxy(DXGI_PRESENT_TEST) == S_OK && context->bridge_source_swapchain == 0,
              "reverse test Presents cannot establish a source relation");
          accept_reverse();
          adapter::BeforeResize(sdk_proxy.Get());
          xefg::ConfigureD3D12Source(nullptr);
          Check(run_proxy() == S_OK && context->bridge_source_swapchain == 0
                    && adapter::D3D12ProxyPresentObservation::current == nullptr,
              "an unconfigured production observer cannot activate reverse D3D12 observation");
          xefg::ConfigureD3D12Source(&ObserveReceiptD3D12Source);
          receipt_inner_source = nullptr;
          adapter::proxies[1].real[0] = saved_secondary_real;
          adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
          source_swapchain->present_callback = normal_present;
          sdk_proxy->present_callback = nullptr;
          accept_link();
          std::cout << "PASS CPU MOCK reverse D3D12 Present: deferred commit, public Disable, failure, ambiguity, generation and TLS; GPU=NotRun\n";
        }
        receipt_sdk_status = -19;
        Hooks::Init(context_handle, nullptr, nullptr);
        Check(!xefg::QueryBridgeOff(receipt_source).available && !context->bridge_context_ready,
            "D3D12 failed reinitialization revokes Off before the real SDK call");
        receipt_sdk_status = 0;
        xefg::ForgetBridgeSourceSwapchain(SOURCE_OWNER);
        xefg::ConfigureD3D12Source(nullptr);
        adapter::proxies[0].real = saved_proxy_real;
        source_slot.real = saved_source_real;
        source_slot.records[0].target = saved_source_target;
        source_slot.installed = saved_source_installed;
        source_slot.complete = saved_source_complete;
        receipt_proxy = nullptr;
        std::cout << "PASS CPU MOCK D3D12 explicit Off: nested source/proxy/device identity, SDK FPS token, failure and revocation; GPU=NotRun\n";
      }
      adapter::state.contexts.clear();
      adapter::modules[0].real = saved_real;
      xefg::ConfigureBridgeSource(nullptr);
      receipt_source = {};
      std::cout << "PASS CPU mocked SDK Off receipt: in-flight/failure, source/proxy/context identity, reenable and recovery\n";
    }
    if (bridge_off_cpu) {
      std::cout << "PASS CPU-only Bridge device capture and existing Off receipts; GPU=NotRun\n";
      return 0;
    }
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    Gpu gpu;
    TestRouteDiagnostics(&gpu);
    xefg::Configure(&Service, &Prepare, nullptr, &Requested, false, false);
    Check(!adapter::sdk_detours_enabled.load(), "XeFG SDK detour diagnostic switch did not latch");
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    Check(adapter::sdk_detours_enabled.load(), "XeFG SDK detours did not restore for the contract fixture");
    {
      std::vector<std::pair<FenceRef, uint64_t>> waits;
      waits.emplace_back(FenceRef(gpu.fence.Get()), 2);
      waits.emplace_back(FenceRef(gpu.fence.Get()), 5);
      waits.emplace_back(FenceRef(gpu.fence.Get()), 3);
      adapter::CoalesceWaits(&waits);
      Check(waits.size() == 1 && waits.front().second == 5,
            "same XeFG consumer fence waits coalesce to the highest value");
      adapter::Context stats;
      adapter::RecordServiceStats(reinterpret_cast<uintptr_t>(&stats), 2, 5, 1, 1, true, false);
      Check(stats.interval_nr_frames.load() == 1
                && stats.interval_nr_processes.load() == 2
                && stats.interval_nr_evaluates.load() == 5
                && stats.interval_nr_hudless_processes.load() == 1
                && stats.interval_nr_backbuffer_processes.load() == 1
                && stats.interval_nr_two_color_frames.load() == 1
                && stats.interval_nr_composed_frames.load() == 0,
            "XeFG service diagnostic counters preserve role and evaluation scope");
    }
    {
      Session nested(&gpu);
      auto depth = gpu.Texture();
      auto motion = gpu.Texture();
      bool nested_contracts_passed = true;
      for (uint32_t mode = 0; mode < 3; ++mode) {
        nested.context->enabled = mode != 0;
        policy = mode != 1;
        adapter::RefreshOwnership();
        nested.Guides(mode, depth.Get(), motion.Get());
        gpu.Submit();
        Check(Hooks::PresentId(nested.handle, mode) == 0, "nested SDK present id");
        const auto application_frame = nested.context->application_frame;
        const auto next_fence = nested.context->next_fence;
        const auto calls = service_calls.size();
        const auto presents = fake_present_calls;
        nested.context->interval_start = std::chrono::steady_clock::now();
        nested.context->interval_presents = 0;
        DXGI_PRESENT_PARAMETERS parameters{};
        Check(SUCCEEDED(adapter::ProxyHooks<0>::Present1(nested.proxy.Get(), 0, 0, &parameters)),
              "nested proxy Present1 forwarding");
        const auto frames_delta = nested.context->application_frame - application_frame;
        const auto fence_delta = nested.context->next_fence - next_fence;
        const auto service_delta = service_calls.size() - calls;
        const bool owned = mode == 2;
        const bool passed = frames_delta == 1 && fence_delta == 0 && nested.context->interval_presents == 1
            && service_delta == (owned ? 1u : 0u) && fake_present_calls == presents + 1
            && fake_present_owns_stream == owned && xefg::OwnsStream() == owned;
        nested_contracts_passed &= passed;
        std::cout << (passed ? "PASS" : "FAIL") << " nested Present1->Present "
                  << (mode == 0 ? "FGOff" : (mode == 1 ? "policyOff" : "FGOn"))
                  << " application_frame_delta=" << frames_delta << " next_fence_delta=" << fence_delta
                  << " interval_presents=" << nested.context->interval_presents
                  << " service_delta=" << service_delta << " owns_stream=" << fake_present_owns_stream << '\n';
      }
      Check(nested_contracts_passed, "nested proxy Present counted more than once or changed FG ownership/service");
      nested.context->enabled = false;
      adapter::RefreshOwnership();
      service_calls.clear();
      gpu.Submit();
    }
    Session first(&gpu), second(&gpu);
    auto color = gpu.Texture();
    auto depth = gpu.Texture();
    auto motion = gpu.Texture();
    auto decoy = gpu.Texture();
    gpu.Write(color.Get(), 0x31);
    gpu.Write(depth.Get(), 0x52);
    gpu.Write(motion.Get(), 0x73);
    const auto original = first.Tag(10, HUDLESS, color.Get(), ONLY_NOW);
    const auto forwarded = sdk_calls.back();
    Check(forwarded.resource.resource != color.Get() && forwarded.resource.validity == UNTIL_NEXT_PRESENT,
          "HUDLESS ONLY_NOW must forward owned UNTIL_NEXT_PRESENT texture");
    Check(forwarded.context == first.handle && forwarded.id == 10 && forwarded.list == gpu.list.Get()
        && forwarded.resource.type == original.type && forwarded.resource.base.x == original.base.x
        && forwarded.resource.base.y == original.base.y && forwarded.resource.size.x == original.size.x
        && forwarded.resource.size.y == original.size.y, "SDK routing/region parameters changed");
    first.Tag(10, DEPTH, depth.Get(), ONLY_NOW);
    auto captured_depth = first.context->frames.at(10)->resources[DEPTH].resource;
    Check(sdk_calls.back().resource.resource == depth.Get() && sdk_calls.back().resource.validity == ONLY_NOW,
          "non-HUDLESS SDK input changed");
    first.Tag(10, MOTION, motion.Get(), ONLY_NOW);
    auto captured_motion = first.context->frames.at(10)->resources[MOTION].resource;
    const xefg::abi::Constants constants{.motion_scale_x = 8.f, .motion_scale_y = -8.f, .frame_time_ms = 16.5f};
    Check(Hooks::Constants(first.handle, 10, &constants) == 0, "constants");
    first.Tag(10, BACKBUFFER, decoy.Get());
    gpu.Write(color.Get(), 0x91);
    gpu.Write(depth.Get(), 0xa2);
    gpu.Write(motion.Get(), 0xb3);
    gpu.Submit();
    gpu.ExpectPixels(forwarded.resource.resource, 0x31);
    gpu.ExpectPixels(captured_depth.Get(), 0x52);
    gpu.ExpectPixels(captured_motion.Get(), 0x73);
    gpu.ExpectPixels(color.Get(), 0x91);
    std::cout << "PASS ONLY_NOW GPU snapshots survive source rewrites; HUDLESS SDK forwarding\n";
    const auto submitted_before = xefg::SubmittedFrames();
    first.Present(10, DXGI_PRESENT_TEST);
    Check(service_calls.empty() && first.context->frames.contains(10), "TEST Present consumed frame");
    Check(xefg::SubmittedFrames() == submitted_before, "TEST Present inflated submitted frame count");
    first.Present(10);
    Check(service_calls.size() == 1 && xefg::CurrentResult() == fg::Result::SUBMITTED, "valid frame not served");
    Check(xefg::SubmittedFrames() == submitted_before + 1, "real service submission not counted once");
    Check(service_observed_preparing, "active recording is exposed as unserved to concurrent UI");
    Check(service_calls.back().hudless == forwarded.resource.resource
        && service_calls.back().depth == captured_depth.Get() && service_calls.back().motion == captured_motion.Get(),
          "service received original resources instead of ONLY_NOW snapshots");
    ComPtr<ID3D12Resource> real_backbuffer;
    Check(SUCCEEDED(first.proxy->GetBuffer(first.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer))), "real BB");
    Check(service_calls.back().backbuffer == real_backbuffer.Get() && service_calls.back().backbuffer != decoy.Get(),
          "BACKBUFFER tag substituted for proxy GetBuffer");
    const auto first_application_frame = first.context->application_frame;
    first.Present(10);
    Check(service_calls.size() == 1 && first.context->application_frame == first_application_frame,
          "duplicate id served twice or advanced the application frame");
    Check(xefg::SubmittedFrames() == submitted_before + 1, "repeated ID inflated submitted frame count");
    const auto calls_before_untagged = service_calls.size();
    Check(SUCCEEDED(adapter::ProxyHooks<0>::Present(first.proxy.Get(), 0, 0)), "repeat Present");
    Check(service_calls.size() == calls_before_untagged + 1
        && first.context->application_frame == first_application_frame + 1,
          "untagged application Present did not reach the Backbuffer fallback");
    Check(!service_calls.back().has_present_id && service_calls.back().backbuffer != nullptr
        && service_calls.back().hudless == nullptr && service_calls.back().depth == nullptr
        && service_calls.back().motion == nullptr && !service_calls.back().has_constants,
          "untagged Present borrowed stale XeFG guides or frame constants");
    Check(xefg::CurrentResult() != fg::Result::SUBMITTED,
          "untagged Backbuffer fallback incorrectly reported a served XeFG frame");
    first.Guides(11, depth.Get(), motion.Get());
    first.Present(11);
    Check(service_calls.size() == calls_before_untagged + 2 && service_calls.back().has_present_id
        && service_calls.back().application_frame == first_application_frame + 2,
          "next tagged Present did not resume the real-frame sequence");
    std::cout << "PASS repeated IDs are skipped; untagged Presents use Backbuffer-only fallback without stale guides\n";
    first.Guides(20, depth.Get(), motion.Get());
    second.Tag(20, HUDLESS, color.Get());
    second.Present(20);
    Check(service_calls.back().context == reinterpret_cast<uintptr_t>(second.handle)
        && service_calls.back().depth == nullptr && service_calls.back().motion == nullptr,
          "guides leaked across contexts sharing presentId");
    Check(xefg::CurrentResult() == fg::Result::NOT_SERVED, "missing guides incorrectly successful");
    first.Present(20);
    Check(service_calls.back().context == reinterpret_cast<uintptr_t>(first.handle)
        && service_calls.back().depth == depth.Get(), "matching context frame lost");
    first.Tag(21, HUDLESS, color.Get());
    first.Present(21);
    Check(service_calls.back().depth == nullptr && service_calls.back().motion == nullptr
        && !service_calls.back().has_constants, "previous presentId guides reused");
    std::cout << "PASS context+presentId association and no stale guides\n";
    first.Guides(30, depth.Get(), motion.Get());
    first.Tag(30, HUDLESS, color.Get(), ONLY_NOW);
    second.Guides(31, depth.Get(), motion.Get());
    const auto calls_before_unrelated = service_calls.size();
    second.Present(31);
    Check(service_calls.size() == calls_before_unrelated + 1 && xefg::CurrentResult() == fg::Result::SUBMITTED,
          "unrelated unsubmitted capture blocked another context");
    const auto calls_before_unsubmitted = service_calls.size();
    first.Present(30);
    Check(service_calls.size() == calls_before_unsubmitted && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "unsubmitted relevant copy served");
    gpu.Submit();
    std::cout << "PASS relevant unsubmitted copy blocks service; unrelated context remains serviceable\n";
    first.Guides(40, depth.Get(), motion.Get());
    sdk_status = -7;
    first.Tag(40, HUDLESS, color.Get(), ONLY_NOW);
    sdk_status = 0;
    gpu.Submit();
    const auto calls_before_rejection = service_calls.size();
    first.Present(40);
    Check(service_calls.size() == calls_before_rejection && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "SDK rejected resource reported successful");
    first.Guides(41, depth.Get(), motion.Get());
    sdk_status = -9;
    Check(Hooks::Constants(first.handle, 41, &constants) == -9, "constants SDK rejection masked");
    sdk_status = 0;
    first.Present(41);
    Check(!service_calls.back().has_constants && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "rejected constants reported successful");
    std::cout << "PASS SDK resource/constants rejection\n";
    Session consumer(&gpu);
    consumer.Guides(50, depth.Get(), motion.Get());
    consumer.Tag(50, HUDLESS, color.Get(), ONLY_NOW);
    const auto retained = consumer.context->frames.at(50)->resources[HUDLESS].copy;
    gpu.Submit();
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "consumer gate");
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "consumer queue wait");
    consumer.Present(50);
    const bool consumer_pending = retained->consumer_fence != nullptr
        && retained->consumer_fence->GetCompletedValue() < retained->consumer_value;
    consumer.Tag(51, HUDLESS, color.Get(), ONLY_NOW);
    const auto next = consumer.context->frames.at(51)->resources[HUDLESS].copy;
    const bool distinct_while_pending = next != retained;
    // Unblock the real queue before an assertion can unwind the host.
    Check(SUCCEEDED(gate->Signal(1)), "release consumer gate");
    gpu.Submit();
    Check(consumer_pending && distinct_while_pending, "surface reused before proxy consumer fence completion");
    Check(retained->consumer_fence->GetCompletedValue() >= retained->consumer_value, "consumer never completed");
    consumer.Tag(52, HUDLESS, color.Get(), ONLY_NOW);
    Check(consumer.context->frames.at(52)->resources[HUDLESS].copy == retained,
          "completed surface not returned to pool");
    gpu.Submit();
    std::cout << "PASS normal Present consumer fence prevents early surface reuse\n";
    policy = false;
    consumer.Present(51);
    Check(!consumer.context->frames.contains(51) && !next->reserved,
          "policy-off Present failed to release previously substituted input");
    consumer.Present(52);
    policy = true;
    gpu.Submit();
    std::cout << "PASS policy-off Present cleanup\n";
    Session routing(&gpu);
    for (uint32_t mode = 0; mode <= 5; ++mode) {
      for (const bool composition : {false, true}) {
        routing.context->init.ui_mode = static_cast<int32_t>(mode);
        routing.context->ui_composition = composition;
        const uint32_t id = 60 + mode * 2 + static_cast<uint32_t>(composition);
        routing.Guides(id, depth.Get(), motion.Get());
        routing.Tag(id, HUDLESS, color.Get());
        const auto tagged = sdk_calls.back().resource.resource;
        routing.Present(id);
        const bool consumes_hudless = !composition || (mode != 1 && mode != 2);
        Check((service_calls.back().hudless != nullptr) == consumes_hudless,
              "NR colors do not follow XeFG UI composition consumer mode");
        Check(tagged == color.Get(), "consumer routing changed SDK HUDLess input");
        Check(service_calls.back().backbuffer != nullptr, "consumer routing lost actual backbuffer");
      }
    }
    gpu.Submit();
    submission::ResetAll();
    std::cout << "PASS public UI modes select NR color consumers without changing SDK tags\n";
    xefg::Configure(&Service, &Prepare, nullptr, &Requested, true);
    Session final_color(&gpu);
    Check(final_color.context->omit_optional_hudless, "AUTO context did not latch the experimental single-color policy");
    final_color.context->ui_composition = false;
    final_color.Guides(80, depth.Get(), motion.Get());
    final_color.Tag(80, 3, decoy.Get());
    const auto before_optional = sdk_calls.size();
    final_color.Tag(80, HUDLESS, color.Get(), ONLY_NOW);
    Check(sdk_calls.size() == before_optional, "optional HUDLess still reached SDK in final-color mode");
    Check(final_color.context->frames.at(80)->captures.empty(), "blocked optional HUDLess still copied");
    final_color.Present(80);
    Check(service_calls.back().hudless == nullptr && service_calls.back().backbuffer != nullptr
        && service_calls.back().depth == depth.Get() && service_calls.back().motion == motion.Get()
        && service_calls.back().ui == decoy.Get(),
        "final-color mode changed guides or kept a second NR consumer");
    policy = false;
    final_color.Tag(81, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 1,
          "NR Off did not restore SDK HUDLess forwarding");
    Check(sdk_calls.back().resource.resource == color.Get() && sdk_calls.back().resource.type == HUDLESS,
          "NR Off forwarded the wrong HUDLess resource");
    final_color.Present(81);
    policy = true;
    final_color.context->ui_composition = true;
    final_color.Tag(82, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 2, "UI composition must forward HUDLess to the SDK");
    final_color.Present(82);
    Check(service_calls.back().hudless == color.Get() && service_calls.back().backbuffer != nullptr,
          "AUTO UI composition must retain both color consumers");
    final_color.context->ui_composition = false;
    final_color.context->enabled = false;
    final_color.Tag(83, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 3, "FG Off must forward HUDLess to the SDK");
    final_color.Present(83);
    final_color.context->enabled = true;
    sdk_status = -7;
    final_color.Tag(85, HUDLESS, color.Get(), 2);
    Check(sdk_calls.size() == before_optional + 4, "unsupported tag validity was hidden");
    const xefg::abi::Resource invalid_region{.type = HUDLESS, .validity = ONLY_NOW,
        .size = {0, 0}, .resource = color.Get()};
    Check(Hooks::Resource(final_color.handle, gpu.list.Get(), 86, &invalid_region) == -7,
          "invalid region error was hidden");
    const xefg::abi::Resource missing_list{.type = HUDLESS, .validity = ONLY_NOW,
        .size = {4, 3}, .resource = color.Get()};
    Check(Hooks::Resource(final_color.handle, nullptr, 87, &missing_list) == -7,
          "ONLY_NOW without command list error was hidden");
    sdk_status = 0;
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    final_color.Tag(84, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 6, "live option change altered an existing context's color contract");
    final_color.Present(84);
    Session dual_color(&gpu);
    dual_color.context->ui_composition = false;
    Check(!dual_color.context->omit_optional_hudless, "disabled experiment changed the default dual-color context");
    dual_color.Guides(88, depth.Get(), motion.Get());
    const auto before_dual_color = sdk_calls.size();
    dual_color.Tag(88, HUDLESS, color.Get());
    dual_color.Present(88);
    Check(sdk_calls.size() == before_dual_color + 1 && service_calls.back().hudless == color.Get(),
          "new default context did not preserve the dual-color route");
    xefg::Configure(&Service, &Prepare, nullptr, &Requested, true);
    Session non_auto(&gpu, 1);
    non_auto.context->ui_composition = false;
    Check(!non_auto.context->omit_optional_hudless, "non-AUTO XeFG context accepted the AUTO-only experiment");
    non_auto.Guides(89, depth.Get(), motion.Get());
    const auto before_non_auto = sdk_calls.size();
    non_auto.Tag(89, HUDLESS, color.Get());
    non_auto.Present(89);
    Check(sdk_calls.size() == before_non_auto + 1 && service_calls.back().hudless == color.Get(),
          "non-AUTO XeFG route stopped forwarding HUDLess");
    // An Off tag may already have been privately copied by the SDK. Resuming
    // NR in that same frame must not enhance only the backbuffer.
    for (const auto validity : {UNTIL_NEXT_PRESENT, ONLY_NOW}) {
      const uint32_t id = 90 + static_cast<uint32_t>(validity);
      policy = false;
      const auto before_tag = sdk_calls.size();
      final_color.Tag(id, HUDLESS, color.Get(), validity);
      Check(sdk_calls.size() == before_tag + 1 && sdk_calls.back().resource.resource == color.Get(),
            "Off tag must reach the real SDK unchanged");
      policy = true;
      const auto before_service = service_calls.size();
      final_color.Guides(id, depth.Get(), motion.Get());
      final_color.Present(id);
      Check(service_calls.size() == before_service, "Off-to-On frame mixed original HUDLess with NR backbuffer");
      final_color.Guides(id + 10, depth.Get(), motion.Get());
      final_color.Tag(id + 10, HUDLESS, color.Get());
      final_color.Present(id + 10);
      Check(service_calls.size() == before_service + 1 && service_calls.back().hudless == nullptr,
            "next complete On frame must resume single-color processing");
    }
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    gpu.Submit();
    submission::ResetAll();
    std::cout << "PASS AUTO single-color option stays latched; UI composition/non-AUTO retain colors; Off/On stays consistent\n";
    {
      Session continuous(&gpu);
      const auto submit = [&](uint32_t id, fg::Result outcome) {
        service_outcome = outcome;
        continuous.Guides(id, depth.Get(), motion.Get());
        continuous.Present(id);
      };
      submit(900, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "new context inherited active service");
      submit(901, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::SUBMITTED
                && xefg::CurrentResult() == fg::Result::SUBMITTED,
            "adjacent successful frames flash waiting while recording");
      continuous.Present(902);
      Check(xefg::DisplayResult() == fg::Result::NOT_SERVED, "declined return retained an active display");
      submit(903, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "submission gap retained active recording");
      const auto count = xefg::SubmittedFrames();
      submit(904, fg::Result::PREPARED);
      Check(observed_display_results.back() == fg::Result::SUBMITTED
                && xefg::DisplayResult() == fg::Result::PREPARED && xefg::SubmittedFrames() == count,
            "completed preparation fabricated a successful submission");
      submit(905, fg::Result::FAILED);
      Check(observed_display_results.back() == fg::Result::PREPARED
                && xefg::DisplayResult() == fg::Result::FAILED,
            "actual failure was hidden by service activity");
      submit(906, fg::Result::SUBMITTED);
      adapter::BeforeResize(continuous.proxy.Get());
      submit(907, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "new epoch inherited active recording");
      policy = false;
      Check(xefg::DisplayResult() == fg::Result::NOT_SERVED, "NR Off retained active display");
      continuous.Present(908);
      policy = true;
      submit(909, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "NR Off gap inherited active recording");
      Check(service_observed_preparing, "display service state changed exact frame handoff truth");
      std::cout << "PASS continuous UI service with exact phase, gap, pending, failure, epoch and Off truth\n";
    }
    gpu.Submit();
    submission::ResetAll();
    std::cout << "All XeFG adapter WARP contracts passed (fake SDK slots; no SDK/game loaded).\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
