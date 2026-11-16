/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
// Included by the existing DEV runtime CPU fixture. API calls below go only to
// CPU substitutes; protected pages detect copying newer fields from old options.
{
  namespace probe = renodx::addons::dlss5::fg_output_probe;
  namespace native_probe = renodx::addons::dlss5::fg::native_options::internal;
  struct RestoreProbe {
    probe::Snapshot saved = probe::CopySnapshot();
    RestoreProbe() {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = {};
    }
    ~RestoreProbe() {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = saved;
    }
  } restore_probe;
  static const sl::DLSSGOptions* expected_original = nullptr;
  static size_t expected_bytes = 0;
  static std::array<unsigned char, sizeof(sl::DLSSGOptions)> expected_forwarded{};
  static bool native_bytes_match = false;
  static bool native_original_forwarded = false;
  static sl::Result native_result = sl::Result::eOk;
  const auto native_real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions& forwarded) -> sl::Result {
    native_original_forwarded = &forwarded == expected_original;
    native_bytes_match = std::memcmp(&forwarded, expected_forwarded.data(), expected_bytes) == 0;
    return native_result;
  };
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  sl::DLSSGOptions chained;
  for (uint64_t version = 1; version <= 5; ++version) {
    struct Pages {
      unsigned char* base = nullptr;
      ~Pages() {
        if (base != nullptr) {
          VirtualFree(base, 0, MEM_RELEASE);
        }
      }
    } pages;
    const auto bytes = native_probe::ProbeOptionsBytes(version);
    const auto object_bytes = (bytes + alignof(sl::DLSSGOptions) - 1) & ~(alignof(sl::DLSSGOptions) - 1);
    pages.base = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2 * system.dwPageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(pages.base != nullptr, "native output probe allocates a bounded CPU legacy-options guard");
    if (pages.base == nullptr) continue;
    DWORD old_protection = 0;
    const bool protected_tail = VirtualProtect(pages.base + system.dwPageSize,
        system.dwPageSize, PAGE_NOACCESS, &old_protection) != FALSE;
    check(protected_tail, "native output probe protects memory after the actual legacy object size");
    if (!protected_tail) continue;
    sl::DLSSGOptions original;
    original.structVersion = version;
    original.next = &chained;
    original.mode = sl::DLSSGMode::eOn;
    original.numFramesToGenerate = 3;
    original.flags = sl::DLSSGFlags::eDynamicResolutionEnabled | sl::DLSSGFlags::eRetainResourcesWhenOff;
    original.dynamicResWidth = 1234;
    original.dynamicResHeight = 567;
    original.bReserved15 = sl::Boolean::eTrue;
    original.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
    original.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    original.dynamicTargetFrameRate = 144.25f;
    auto* memory = pages.base + system.dwPageSize - object_bytes;
    std::memset(memory, 0x5a, object_bytes);
    std::memcpy(memory, &original, bytes);
    std::array<unsigned char, sizeof(sl::DLSSGOptions)> before{};
    std::memcpy(before.data(), memory, object_bytes);
    expected_original = reinterpret_cast<const sl::DLSSGOptions*>(memory);
    expected_bytes = bytes;
    std::memcpy(expected_forwarded.data(), memory, bytes);
    const auto flags = original.flags | sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    std::memcpy(expected_forwarded.data() + offsetof(sl::DLSSGOptions, flags), &flags, sizeof(flags));
    const auto result = native_probe::ProbeSetOptions(native_real, sl::ViewportHandle{23}, *expected_original, false);
    const auto receipt = probe::CopySnapshot().native;
    check(result == sl::Result::eOk && native_bytes_match && !native_original_forwarded
            && std::memcmp(memory, before.data(), object_bytes) == 0
            && receipt.active && receipt.version_supported && receipt.flag_forwarded
            && receipt.viewport == 23 && receipt.struct_version == version && receipt.num_frames == 3
            && receipt.original_flags == static_cast<uint32_t>(original.flags)
            && receipt.forwarded_flags == static_cast<uint32_t>(flags),
        "native output probe preserves every declared version field, borrowed next chain and caller bytes while adding only the SDK diagnostic bit");
  }
  sl::DLSSGOptions ordinary;
  ordinary.mode = sl::DLSSGMode::eOn;
  ordinary.flags = sl::DLSSGFlags::eEnableFullscreenMenuDetection;
  expected_original = &ordinary;
  expected_bytes = native_probe::ProbeOptionsBytes(ordinary.structVersion);
  std::memcpy(expected_forwarded.data(), &ordinary, expected_bytes);
  const auto flags = ordinary.flags | sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
  std::memcpy(expected_forwarded.data() + offsetof(sl::DLSSGOptions, flags), &flags, sizeof(flags));
  const auto success_generation = probe::CopySnapshot().native.after_success_generation;
  native_result = static_cast<sl::Result>(-91);
  native_probe::ProbeSetOptions(native_real, sl::ViewportHandle{23}, ordinary, false);
  check(native_bytes_match && !probe::CopySnapshot().native.active
          && probe::CopySnapshot().native.after_success_generation == success_generation
          && probe::CopySnapshot().native.result == -91,
      "a failed real native options return is retained and cannot inherit the earlier active or success generation");
  native_result = sl::Result::eOk;
  ordinary.structVersion = 99;
  expected_bytes = native_probe::ProbeOptionsBytes(1);
  std::memcpy(expected_forwarded.data(), &ordinary, expected_bytes);
  native_probe::ProbeSetOptions(native_real, sl::ViewportHandle{24}, ordinary, false);
  check(native_bytes_match && native_original_forwarded && !probe::CopySnapshot().native.version_supported
          && !probe::CopySnapshot().native.flag_forwarded && !probe::CopySnapshot().native.active,
      "unknown native options versions forward the caller object unchanged and remain unsupported for probe success");
  ordinary.structVersion = 1;
  ordinary.mode = sl::DLSSGMode::eOff;
  std::memcpy(expected_forwarded.data(), &ordinary, expected_bytes);
  native_probe::ProbeSetOptions(native_real, sl::ViewportHandle{24}, ordinary, false);
  check(native_bytes_match && native_original_forwarded && !probe::CopySnapshot().native.active,
      "native Off calls are never changed into an On diagnostic request");
  ordinary.mode = sl::DLSSGMode::eOn;
  std::memcpy(expected_forwarded.data(), &ordinary, expected_bytes);
  native_probe::ProbeSetOptions(native_real, sl::ViewportHandle{24}, ordinary, true);
  check(native_bytes_match && native_original_forwarded && probe::CopySnapshot().native.stopped
          && !probe::CopySnapshot().native.active,
      "a stopped native hook passes On through without adding the diagnostic bit");
  probe::StopNative();
  probe::RecordNative({.version_supported = true, .flag_forwarded = true}, true);
  check(probe::CopySnapshot().native_stopped && !probe::CopySnapshot().native.active,
      "native logical Stop also rejects a late successful in-flight receipt without an SDK replay");

  static std::array<uint32_t, 2> debug_calls{};
  static std::array<int32_t, 2> debug_results{};
  static bool debug_arguments_match = true;
  const auto debug_real = +[](void* handle, probe::XefgDebugFeature feature, uint32_t enabled, void* argument) -> int32_t {
    const auto index = static_cast<uint32_t>(feature);
    debug_arguments_match &= handle == reinterpret_cast<void*>(0x4444) && enabled == 1 && argument == nullptr && index < 2;
    if (index >= 2) return -99;
    ++debug_calls[index];
    return debug_results[index];
  };
  const probe::XefgReceipt context{.sdk_slot = 1, .module_base = 0x2222, .sdk_context = 0x4444,
      .context_identity = 0x5555, .init_epoch = 1, .init_generation = 7, .current_epoch = 1, .current_generation = 7};
  {
    const std::lock_guard lifecycle(probe::xefg_lifecycle_mutex);
    const std::lock_guard delegated_lifecycle(probe::xefg_lifecycle_mutex);
    const probe::XefgInitScope outer_init(1, 0x4444);
    {
      const probe::XefgInitScope delegated_init(1, 0x4444);
      check(outer_init.owner && !delegated_init.owner,
          "same-slot same-handle InitDesc-to-Init delegation keeps a single debug-combination owner");
    }
    {
      const probe::XefgInitScope other_route(0, 0x4444);
      check(other_route.owner, "a different actual XeFG SDK route is not deduplicated by matching handle bits");
    }
    probe::BeginXefg(1, 0x2222, 0x4444);
    probe::ApplyXefg(context, debug_real);
    probe::FinishXefgInit(1, 0x4444, 0);
  }
  auto xefg_receipt = probe::CopySnapshot().xefg[0];
  check(debug_arguments_match && debug_calls[0] == 1 && debug_calls[1] == 1
          && xefg_receipt.debug_applied && xefg_receipt.sdk_slot == 1 && xefg_receipt.module_base == 0x2222
          && xefg_receipt.context_identity == 0x5555 && xefg_receipt.show_called && xefg_receipt.tag_called,
      "XeFG probe calls exactly the two official debug features on the supplied actual SDK route and handle");
  probe::UpdateXefg(0x5555, 2, 8);
  xefg_receipt = probe::CopySnapshot().xefg[0];
  check(xefg_receipt.debug_applied && xefg_receipt.init_epoch == 1 && xefg_receipt.init_generation == 7
          && xefg_receipt.current_epoch == 2 && xefg_receipt.current_generation == 8,
      "ordinary XeFG Enable epoch changes preserve context debug configuration and distinguish Init from current metadata");
  probe::DestroyXefg(1, 0x4444, 0);
  check(!probe::CopySnapshot().xefg[0].registered && !probe::CopySnapshot().xefg[0].debug_applied
          && probe::CopySnapshot().xefg[0].destroy_called,
      "successful XeFG destruction invalidates its bound context receipt");
  probe::BeginXefg(1, 0x2222, 0x4444);
  check(!probe::CopySnapshot().xefg[0].debug_applied && !probe::CopySnapshot().xefg[0].show_called,
      "a reinitialized SDK handle starts with no inherited debug-feature success");
  debug_results[1] = -73;
  {
    const std::lock_guard lifecycle(probe::xefg_lifecycle_mutex);
    probe::ApplyXefg(context, debug_real);
  }
  xefg_receipt = probe::CopySnapshot().xefg[0];
  check(debug_calls[0] == 2 && debug_calls[1] == 2 && xefg_receipt.show_result == 0
          && xefg_receipt.tag_result == -73 && !xefg_receipt.debug_applied,
      "each XeFG SDK result remains independent and one failed feature cannot be treated as an active display probe");
  probe::BeginXefg(1, 0x2222, 0x4444);
  probe::ApplyXefg(context, nullptr);
  check(!probe::CopySnapshot().xefg[0].show_called && !probe::CopySnapshot().xefg[0].tag_called
          && !probe::CopySnapshot().xefg[0].debug_applied && debug_calls[0] == 2 && debug_calls[1] == 2,
      "an unavailable debug export performs no SDK calls and does not reuse an old result");
  probe::BeginXefg(1, 0x2222, 0x4444);
  probe::FinishXefgInit(1, 0x4444, -31);
  check(probe::CopySnapshot().xefg[0].init_result == -31 && !probe::CopySnapshot().xefg[0].debug_applied,
      "a failed XeFG Init reports its real error without debug calls or inherited success");
  probe::StopXefg();
  probe::ApplyXefg(context, debug_real);
  check(probe::CopySnapshot().xefg_stopped && probe::CopySnapshot().xefg[0].stopped
          && !probe::CopySnapshot().xefg[0].debug_applied && debug_calls[0] == 2 && debug_calls[1] == 2,
      "terminal XeFG Stop disables receipt eligibility without claiming to reset the SDK's context flags");
}
#endif
