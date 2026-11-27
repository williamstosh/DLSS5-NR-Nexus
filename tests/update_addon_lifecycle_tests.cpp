/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "vulkan_test_api_stubs.hpp"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Event = reshade::addon_event;
using Overlay = void (*)(reshade::api::effect_runtime*);
using namespace std::chrono_literals;

struct VulkanDevice final : nr::test::vk::DeviceApiStubs {
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(this); }
  reshade::api::device_api get_api() const override { return reshade::api::device_api::vulkan; }
};

constexpr Event VULKAN_DEVICE_EVENTS[] = {
    Event::reset_command_list,
    Event::begin_render_pass,
    Event::end_render_pass,
    Event::execute_secondary_command_list,
    Event::init_pipeline_layout,
    Event::destroy_pipeline_layout,
    Event::bind_pipeline,
    Event::bind_descriptor_tables,
    Event::push_descriptors,
    Event::push_constants,
};

struct HostRegistration {
  HMODULE module = nullptr;
  bool external = false;
  unsigned registered = 0;
  unsigned unregistered = 0;
  std::map<Event, std::vector<void*>> events;
  std::vector<Overlay> overlays;
};

HostRegistration host;
std::string base_path;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::fflush(stderr);
  // A timed-out unload can still be executing DLL code. End this isolated host
  // before stack or process teardown could wait on that thread under loader lock.
  TerminateProcess(GetCurrentProcess(), 1);
  std::abort();
}

template <Event event, typename... Args>
void Dispatch(Args... args) {
  const auto entries = host.events.at(event);
  for (auto* entry : entries) {
    reinterpret_cast<typename reshade::addon_event_traits<event>::decl>(entry)(args...);
  }
}

void Present() {
  Dispatch<Event::present>(nullptr, nullptr, nullptr, nullptr, 0u, nullptr);
}

template <typename Predicate>
void WaitUntil(Predicate predicate, const char* failure) {
  const auto deadline = Clock::now() + 8s;
  while (!predicate()) {
    Check(Clock::now() < deadline, failure);
    std::this_thread::sleep_for(10ms);
  }
}

HMODULE LoadAddon(const fs::path& path) {
  // ReShade 6.8 skips the candidate by filename while its external registration remains.
  if (host.external) return host.module;
  const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  Check(module != nullptr, "LoadLibraryExW failed");
  const auto init = reinterpret_cast<bool (*)(HMODULE, HMODULE)>(GetProcAddress(module, "AddonInit"));
  if (init != nullptr) {
    Check(init(module, GetModuleHandleW(nullptr)), "AddonInit failed");
  }
  Check(host.module == module, "load did not register its module with the host");
  host.external = false;
  return module;
}

DWORD WINAPI UnloadAddon(void* argument) {
  const auto module = static_cast<HMODULE>(argument);
  const bool was_external = host.external;
  const auto uninit = reinterpret_cast<void (*)(HMODULE, HMODULE)>(GetProcAddress(module, "AddonUninit"));
  Check(uninit != nullptr, "AddonUninit export missing");
  uninit(module, GetModuleHandleW(nullptr));
  if (!was_external) {
    Check(FreeLibrary(module) == TRUE, "host FreeLibrary failed");
    if (host.module == module) {
      host.external = true;
    }
  }
  return 0;
}

void UnloadWithinBound(HMODULE module) {
  const auto start = Clock::now();
  const auto thread = CreateThread(nullptr, 0, UnloadAddon, module, 0, nullptr);
  Check(thread != nullptr, "could not launch host unload thread");
  Check(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
      "AddonUninit and FreeLibrary did not return within 2 seconds");
  CloseHandle(thread);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  std::printf("host unload returned in %lld ms\n", static_cast<long long>(elapsed));
}

struct AddonApi {
  explicit AddonApi(HMODULE module)
      : configure(reinterpret_cast<decltype(configure)>(GetProcAddress(module, "FixtureConfigure"))),
        start_download(reinterpret_cast<decltype(start_download)>(GetProcAddress(module, "FixtureStartDownload"))),
        request_hud(reinterpret_cast<decltype(request_hud)>(GetProcAddress(module, "FixtureRequestHud"))),
        read_transfer(reinterpret_cast<decltype(read_transfer)>(GetProcAddress(module, "FixtureReadTransfer"))),
        read_callbacks(reinterpret_cast<decltype(read_callbacks)>(GetProcAddress(module, "FixtureReadCallbacks"))) {
    Check(configure && start_download && request_hud && read_transfer && read_callbacks, "test fixture ABI missing");
  }

  bool (*configure)(uint16_t, const char*, const wchar_t*);
  HANDLE (*start_download)();
  void (*request_hud)();
  const char* (*read_transfer)(uint64_t*, uint64_t*);
  size_t (*read_callbacks)(uint64_t*);
};

std::string_view Transfer(const AddonApi& addon) {
  uint64_t received = 0;
  uint64_t total = 0;
  return addon.read_transfer(&received, &total);
}

void StartRuntime(const AddonApi& addon, VulkanDevice* device) {
  Check(host.events[Event::init_device].size() == 1
          && host.events[Event::destroy_device].size() == 1
          && host.events[Event::init_effect_runtime].size() == 2
          && host.events[Event::destroy_effect_runtime].size() == 1
          && host.events[Event::present].size() == 2
          && host.events[Event::reshade_present].size() == 2
          && host.overlays.size() == 1,
      "expected one copy of each device, runtime, present and overlay callback");
  uint64_t before = 0;
  Check(addon.read_callbacks(&before) == 0, "previous runtime still registered");
  Dispatch<Event::init_device>(device);
  for (const auto event : VULKAN_DEVICE_EVENTS) {
    Check(host.events[event].size() == 1, "Vulkan device callback missing or duplicated after init_device");
  }
  // With no effect runtime walking the overlay callback list, Present can
  // register the HUD for the pending notice without a real surface or ImGui.
  Check(!host.events.contains(Event::reshade_overlay), "HUD callback registered before it was needed");
  addon.request_hud();
  Present();
  const auto hud = host.events[Event::reshade_overlay];
  Check(hud.size() == 1, "HUD callback missing or duplicated on first eligible present");
  addon.request_hud();
  Present();
  Check(host.events.at(Event::reshade_overlay) == hud, "second eligible present changed the HUD registration");
  // Null runtime and surfaces exercise the host ticks without graphics resources.
  Dispatch<Event::init_effect_runtime>(nullptr);
  Present();
  uint64_t after = 0;
  Check(addon.read_callbacks(&after) == 1 && after == before + 3, "callbacks did not execute in the active generation");
  std::printf("generation %u: HUD and %zu Vulkan event registrations each present once\n",
      host.registered, std::size(VULKAN_DEVICE_EVENTS));
}

unsigned PhysicalUnloads(const fs::path& path) {
  std::ifstream log(path);
  Check(log.good(), "production last-gasp log missing");
  const auto this_pid = " pid=" + std::to_string(GetCurrentProcessId()) + " ";
  unsigned unloads = 0;
  for (std::string line; std::getline(log, line);) {
    if (line.starts_with("[exit] ") && line.ends_with(" unload") && line.find(this_pid) != std::string::npos) {
      ++unloads;
    }
  }
  return unloads;
}

}  // namespace

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void* module, uint32_t version) {
  if (host.module != nullptr || module == nullptr || version != RESHADE_API_VERSION) return false;
  host.module = static_cast<HMODULE>(module);
  ++host.registered;
  return true;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void* module) {
  if (host.module != module) return;
  for (const auto event : VULKAN_DEVICE_EVENTS) {
    const auto callbacks = host.events.find(event);
    Check(callbacks == host.events.end() || callbacks->second.empty(),
        "Vulkan device callbacks were not explicitly released before add-on unregister");
  }
  host.module = nullptr;
  host.external = false;
  host.events.clear();
  host.overlays.clear();
  ++host.unregistered;
}

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(Event event, void* callback) {
  Check(host.module != nullptr, "SDK event registered before add-on registration");
  host.events[event].push_back(callback);
}

extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(Event event, void* callback) {
  const auto entries = host.events.find(event);
  if (entries != host.events.end()) {
    std::erase(entries->second, callback);
  }
}

extern "C" __declspec(dllexport) void ReShadeRegisterOverlay(const char*, Overlay callback) {
  Check(host.module != nullptr, "SDK overlay registered before add-on registration");
  host.overlays.push_back(callback);
}

extern "C" __declspec(dllexport) void ReShadeUnregisterOverlay(const char*, Overlay callback) {
  std::erase(host.overlays, callback);
}

extern "C" __declspec(dllexport) const imgui_function_table* ReShadeGetImGuiFunctionTable(uint32_t version) {
  static const imgui_function_table table{};
  return (version == IMGUI_VERSION_NUM ? &table : nullptr);
}

extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char* path, size_t* size) {
  const size_t required = base_path.size() + 1;
  if (path != nullptr && *size >= required) {
    std::memcpy(path, base_path.c_str(), required);
  }
  *size = required;
  return true;
}

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, char* value, size_t* size) {
  if (section == nullptr || std::string_view(section) != "RenoDX.DLSS5") return false;
  const auto setting = std::string_view(key);
  if (setting != "EnableHooks" && setting != "NRVkStateRestore") return false;
  const bool fits = (value == nullptr || *size >= 2);
  if (value != nullptr && fits) {
    std::memcpy(value, setting == "EnableHooks" ? "0" : "1", 2);
  }
  *size = 2;
  return fits;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, const char*) {}

extern "C" __declspec(dllexport) void ReShadeSetConfigArray(
    void*, reshade::api::effect_runtime*, const char*, const char*, const char*, size_t) {}

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::puts(message);
}

int wmain(int argc, wchar_t** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Check(argc == 5, "expected DLL path, loopback port, case and gate directory");
  const fs::path dll = argv[1];
  wchar_t* port_end = nullptr;
  const auto port = std::wcstoul(argv[2], &port_end, 10);
  Check(*port_end == L'\0' && port > 0 && port <= 65535, "invalid loopback port");
  const std::wstring_view case_name = argv[3];
  const bool cold_load = (case_name == L"gate-cold");
  const bool body_gate = (case_name != L"gate-headers");
  Check(cold_load || case_name == L"gate-body" || !body_gate, "unexpected gate case");
  std::string route;
  for (const auto character : case_name) {
    route.push_back(static_cast<char>(character));
  }
  const fs::path gate_directory = argv[4];
  const auto entered = gate_directory / (route + ".entered");
  const auto release = gate_directory / (route + ".release");
  const auto scratch = fs::current_path();
  const auto downloads = scratch / "downloads";
  fs::create_directory(downloads);
  const auto utf8_path = scratch.u8string();
  base_path.assign(utf8_path.begin(), utf8_path.end());
  Check(SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str()) == TRUE, "could not isolate profile storage");
  const auto lastgasp = dll.parent_path() / "RenoDX-DLSS5-crash.log";

  const auto old_module = LoadAddon(dll);
  const AddonApi old_addon(old_module);
  const auto original_events = host.events;
  const auto original_overlays = host.overlays;
  Check(host.registered == 1 && host.unregistered == 0 && PhysicalUnloads(lastgasp) == 0,
      "expected one initial SDK registration and a loaded image");
  Check(old_addon.configure(static_cast<uint16_t>(port), route.c_str(), downloads.c_str()), "loopback configuration failed");
  VulkanDevice old_device;
  StartRuntime(old_addon, &old_device);
  const auto active_events = host.events;
  WaitUntil([&] { return Transfer(old_addon) == "available"; }, "startup release check did not complete");
  const auto worker = old_addon.start_download();
  Check(worker != nullptr, "could not start or observe the real download worker");
  WaitUntil([&] { return fs::exists(entered); }, "download did not reach the server gate");
  uint64_t before = 0;
  uint64_t expected = 0;
  WaitUntil([&] {
    const std::string_view state = old_addon.read_transfer(&before, &expected);
    return state == "downloading" && expected != 0 && (body_gate ? before > 0 && before < expected : before == 0);
  }, "download did not reach the expected gated progress");
  std::printf("%s gate closed, progress=%llu/%llu\n", route.c_str(),
      static_cast<unsigned long long>(before), static_cast<unsigned long long>(expected));

  Dispatch<Event::destroy_effect_runtime>(nullptr);
  Dispatch<Event::destroy_device>(&old_device);
  UnloadWithinBound(old_module);
  Check(!fs::exists(release) && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT,
      "worker stopped while the server gate remained closed");
  Check(GetModuleHandleW(dll.filename().c_str()) == old_module && PhysicalUnloads(lastgasp) == 0,
      "worker failed to keep the original image mapped after host FreeLibrary");
  Check(!host.external && host.module == nullptr && host.unregistered == 1 && host.events.empty() && host.overlays.empty(),
      "old SDK registration survived host unload and would be treated as external");

  HMODULE new_module = nullptr;
  VulkanDevice new_device;
  if (!cold_load) {
    new_module = LoadAddon(dll);
    Check(new_module == old_module && host.registered == 2 && host.unregistered == 1
            && host.events == original_events && host.overlays == original_overlays,
        "warm AddonInit did not restore the complete SDK registration exactly once");
    const AddonApi new_addon(new_module);
    StartRuntime(new_addon, &new_device);
    Check(host.events == active_events, "warm generation did not restore the active SDK callback set exactly once");
    uint64_t progress = 0;
    uint64_t total = 0;
    Check(std::string_view(new_addon.read_transfer(&progress, &total)) == "downloading"
            && progress >= before && progress < total && total == expected
            && !fs::exists(release) && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT,
        "new registration did not retain the gated download and its progress");
  }

  {
    std::ofstream signal(release);
    signal << "release\n";
    Check(signal.good(), "could not release the server gate");
  }
  Check(WaitForSingleObject(worker, 8000) == WAIT_OBJECT_0, "old worker did not finish after gate release");
  CloseHandle(worker);
  const auto output = fs::directory_iterator(downloads);
  Check(output != fs::directory_iterator{} && output->path().extension() == ".zip" && output->file_size() == expected
          && std::next(output) == fs::directory_iterator{},
      "download did not publish the entire ZIP as the sole output");

  if (cold_load) {
    Check(GetModuleHandleW(dll.filename().c_str()) == nullptr && PhysicalUnloads(lastgasp) == 1,
        "old worker did not release the final reference before cold load");
    new_module = LoadAddon(dll);
    const AddonApi new_addon(new_module);
    Check(Transfer(new_addon) == "idle", "cold image inherited the previous in-memory update service");
    Check(new_addon.configure(static_cast<uint16_t>(port), route.c_str(), downloads.c_str()), "cold loopback configuration failed");
    StartRuntime(new_addon, &new_device);
    WaitUntil([&] { return Transfer(new_addon) == "available"; }, "cold runtime did not run its startup check");
  } else {
    Check(GetModuleHandleW(dll.filename().c_str()) == new_module && PhysicalUnloads(lastgasp) == 0
            && host.module == new_module && host.registered == 2 && host.unregistered == 1
            && host.events == active_events && host.overlays == original_overlays,
        "old worker exit detached or retired the new generation");
    const AddonApi new_addon(new_module);
    uint64_t received = 0;
    uint64_t total = 0;
    Check(std::string_view(new_addon.read_transfer(&received, &total)) == "completed"
            && received == expected && total == expected,
        "old worker result was not published to the active generation");
    uint64_t presents = 0;
    Check(new_addon.read_callbacks(&presents) == 1, "new runtime was retired by the old worker");
    Present();
    uint64_t next = 0;
    Check(new_addon.read_callbacks(&next) == 1 && next == presents + 1, "new present callback stopped after worker exit");
  }

  Dispatch<Event::destroy_effect_runtime>(nullptr);
  Dispatch<Event::destroy_device>(&new_device);
  UnloadWithinBound(new_module);
  Check(GetModuleHandleW(dll.filename().c_str()) == nullptr && PhysicalUnloads(lastgasp) == (cold_load ? 2u : 1u)
          && host.registered == 2 && host.unregistered == 2 && host.module == nullptr
          && host.events.empty() && host.overlays.empty(),
      "final host unload did not detach the image and retire the second SDK registration");
  std::printf("PASS %s: sdk_generations=2 physical_unloads=%u zip_bytes=%llu\n", route.c_str(),
      cold_load ? 2u : 1u, static_cast<unsigned long long>(expected));
}
