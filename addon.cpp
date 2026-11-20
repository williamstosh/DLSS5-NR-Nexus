/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#if defined(RENODX_DLSS5_DEBUG)
// Debug variant: enable ReShade-side utils debug logging alongside the
// addon's own trace log + crash dumps (see debug.hpp).
#define DEBUG_LEVEL_2
#else
#define DEBUG_LEVEL_0
#endif

#include <windows.h>

#include <cstdint>
#include <vector>

#include "brand.h"
#include "dlssnr.hpp"

#include "lastgasp.hpp"
#include "output_framerate.hpp"

namespace {
void ObserveOutputInit(reshade::api::swapchain* swapchain, bool) {
  if (swapchain == nullptr) return;
  auto* device = swapchain->get_device();
  if (device == nullptr || (device->get_api() != reshade::api::device_api::d3d12
      && device->get_api() != reshade::api::device_api::d3d11)) return;
  namespace nr = renodx::addons::dlss5;
  nr::internal::GuardHook([&] {
    std::vector<uintptr_t> back_buffers;
    const auto count = swapchain->get_back_buffer_count();
    back_buffers.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      const auto buffer = static_cast<uintptr_t>(swapchain->get_back_buffer(index).handle);
      back_buffers.push_back(buffer);
      const auto canonical = reinterpret_cast<uintptr_t>(
          renodx::utils::directx::NativeIdentity(reinterpret_cast<IUnknown*>(buffer)));
      if (canonical != 0 && canonical != buffer) {
        back_buffers.push_back(canonical);
      }
    }
    const auto native_swapchain = renodx::utils::directx::NativeIdentity(
        reinterpret_cast<IUnknown*>(swapchain->get_native()));
    const auto native_device = renodx::utils::directx::NativeIdentity(
        reinterpret_cast<IUnknown*>(device->get_native()));
    nr::output_framerate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(swapchain),
        reinterpret_cast<uintptr_t>(native_device), reinterpret_cast<uintptr_t>(native_swapchain), back_buffers);
  });
}

void ObserveOutputPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
    const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  namespace nr = renodx::addons::dlss5;
  if (queue == nullptr || swapchain == nullptr || queue->get_device() != swapchain->get_device()
      || swapchain->get_back_buffer_count() == 0 || nr::fg::xefg::InProxyPresent()) return;
  const auto identity = nr::output_framerate::MatchPresentOutput(reinterpret_cast<uintptr_t>(swapchain),
      static_cast<uintptr_t>(swapchain->get_current_back_buffer().handle));
  // A Native SDK can forward ordinary Presents while Off. The observer
  // rejects this output only once its actual Native route owns the cadence.
  nr::output_framerate::ObservePresent(identity, nr::output_framerate::NowNs());
}

void ObserveOutputDestroy(reshade::api::swapchain* swapchain, bool) {
  renodx::addons::dlss5::output_framerate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(swapchain));
}

void RegisterOutputSampling() {
  reshade::register_event<reshade::addon_event::init_swapchain>(ObserveOutputInit);
  reshade::register_event<reshade::addon_event::present>(ObserveOutputPresent);
  reshade::register_event<reshade::addon_event::destroy_swapchain>(ObserveOutputDestroy);
}

void UnregisterOutputSampling() {
  reshade::unregister_event<reshade::addon_event::init_swapchain>(ObserveOutputInit);
  reshade::unregister_event<reshade::addon_event::present>(ObserveOutputPresent);
  reshade::unregister_event<reshade::addon_event::destroy_swapchain>(ObserveOutputDestroy);
}
}  // namespace

#if defined(NR_NEXUS_IME_DIAGNOSTICS)
#include "tests/ime_trace.hpp"
#endif

extern "C" __declspec(dllexport) constexpr const char* NAME = NR_NEXUS_ADDON_NAME;
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = NR_NEXUS_DESCRIPTION;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    // Before register_addon: the last-gasp reporter covers even a death in
    // the registration path.  The CRT and static initializers are already up.
    renodx::addons::dlss5::lastgasp::Install(
        module, renodx::addons::dlss5::internal::kAddonVersion);
    if (!reshade::register_addon(module)) {
      // No ReShade to register with: the loader unmaps this image next, and
      // the reporter's handlers must not outlive it.
      renodx::addons::dlss5::lastgasp::Exit(false);
      renodx::addons::dlss5::lastgasp::Uninstall();
      return FALSE;
    }
    RegisterOutputSampling();
  }

  renodx::addons::dlss5::Use(reason, module, reserved);

#if defined(NR_NEXUS_IME_DIAGNOSTICS)
  if (reason == DLL_PROCESS_ATTACH) {
    reshade::register_overlay(NR_NEXUS_NAME " IME diagnostics", nr_ime_trace::Draw);
  } else if (reason == DLL_PROCESS_DETACH) {
    nr_ime_trace::Stop();
    reshade::unregister_overlay(NR_NEXUS_NAME " IME diagnostics", nr_ime_trace::Draw);
  }
#endif

  if (reason == DLL_PROCESS_DETACH) {
    UnregisterOutputSampling();
    reshade::unregister_addon(module);
    // `reserved` is null for FreeLibrary and non-null at process exit.
    // [exit] (lastgasp.hpp): the session ended here, not in a silent death.
    renodx::addons::dlss5::lastgasp::Exit(reserved != nullptr);
    if (reserved == nullptr) renodx::addons::dlss5::lastgasp::Uninstall();
  }
  return TRUE;
}
