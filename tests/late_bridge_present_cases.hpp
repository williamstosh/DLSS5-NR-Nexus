#pragma once

namespace late_bridge_present_cases {
inline int Run() {
  namespace nx = renodx::addons::dlss5::internal;
  namespace xefg = renodx::addons::dlss5::fg::xefg;
  namespace adapter = xefg::internal;
  try {
    Check(adapter::state.contexts.empty() && adapter::state.retired_contexts.empty(), "clean SDK ownership");
    const auto query = [] { return xefg::QueryOrdinaryPresent(0x1100, 0x2200, {}); };
    Check(query().allowed && xefg::OrdinarySdkInactive(query()), "no SDK consumer is inactive");
    const auto original_access = query();
    const auto handle = reinterpret_cast<void*>(uintptr_t{0x3300});
    auto context = std::make_shared<adapter::Context>();
    context->application_swapchain_identity = 0x4400;
    context->epoch = 7;
    adapter::state.contexts.emplace(handle, context);
    Check(query().allowed && !xefg::OrdinarySdkInactive(query()), "unassociated permission does not prove Off");
    bool legacy_wrote = false;
    Check(query() == original_access && xefg::WithOrdinaryPresentCommit(original_access, [&] { legacy_wrote = true; }) && legacy_wrote,
          "unrelated SDK context cannot revoke the existing ordinary contract");
    bool late_wrote = false;
    Check(!xefg::WithOrdinaryPresentCommit(original_access, [&] { late_wrote = true; }, true) && !late_wrote, "only the new late path requires positive no-consumer proof");
    context->application_swapchain_identity = 0x1100;
    Check(query().allowed && !xefg::OrdinarySdkInactive(query()), "unobserved default disabled is not completed Off");
    const auto saved_real = adapter::modules[0].real;
    adapter::modules[0].real[6] = reinterpret_cast<void*>(&FakeReceiptEnable);
    const auto saved_source_available = receipt_source_available;
    receipt_source_available = false;
    receipt_sdk_status = 0;
    context->proxy_identity = 0x1100;
    context->bridge_context_ready = context->bridge_proxy_ready = true;
    Check(Hooks::Enable(handle, 0) == 0 && !context->bridge_off_observed,
          "actual successful SDK Disable with no SR source does not manufacture adaptive evidence");
    const auto off = query();
    Check(off.allowed && xefg::OrdinarySdkInactive(off), "completed matched Off permits late write");
    context->bridge_enable_inflight = 1;
    Check(!xefg::OrdinarySdkInactive(query()), "inflight enable revokes Off");
    context->bridge_enable_inflight = 0;
    context->bridge_enable_ambiguous = true;
    Check(!xefg::OrdinarySdkInactive(query()), "ambiguous enable revokes Off");
    context->bridge_enable_ambiguous = false;
    context->ordinary_off_epoch = context->epoch - 1;
    Check(!xefg::OrdinarySdkInactive(query()), "old context epoch revokes Off");
    context->ordinary_off_epoch = context->epoch;
    context->enabled = true;
    Check(!xefg::OrdinarySdkInactive(query()), "enabled consumer revokes Off");
    bool wrote = false;
    Check(!xefg::WithOrdinaryPresentCommit(off, [&] { wrote = true; }, true) && !wrote, "final commit rejects stale Off proof before writing");
    context->enabled = false;
    context->bridge_present_fg_on = true;
    Check(!xefg::OrdinarySdkInactive(query()), "current FG Present revokes Off");
    context->bridge_present_fg_on = false;
    Check(query() == off, "same completed Off proof restores");
    adapter::ReleaseFrames(context.get());
    Check(!xefg::OrdinarySdkInactive(query()), "resize/lifetime epoch revokes a previous SDK Off");
    receipt_sdk_status = E_FAIL;
    Check(Hooks::Enable(handle, 0) == E_FAIL && !xefg::OrdinarySdkInactive(query()), "failed Disable cannot admit late work");
    receipt_sdk_status = 0;
    Check(Hooks::Enable(handle, 0) == 0 && xefg::OrdinarySdkInactive(query()), "successful no-source Disable restores its new epoch");
    adapter::modules[0].real = saved_real;
    receipt_source_available = saved_source_available;
    adapter::state.contexts.clear();
    adapter::state.retired_contexts.push_back(context);
    Check(query().allowed && !xefg::OrdinarySdkInactive(query()), "retired SDK context cannot prove no consumer");
    adapter::state.retired_contexts.clear();
    std::cout << "LATE_BRIDGE_NO_SOURCE actual-disable=success adaptive-source=absent unknown=rejected failed-disable=rejected resize=rejected unrelated-ordinary=preserved\n";
    std::cout << "LATE_BRIDGE_CONTRACT sdk=no-context+matched-off unrelated=not-off inflight=revoked stale-epoch=revoked enabled=revoked final-commit=revalidated\n";

    Check(nx::late_bridge_presents.empty(), "clean late tickets");
    const auto a = reinterpret_cast<reshade::api::swapchain*>(uintptr_t{0x5100});
    const auto b = reinterpret_cast<reshade::api::swapchain*>(uintptr_t{0x5200});
    const auto runtime_a = reinterpret_cast<reshade::api::effect_runtime*>(uintptr_t{0x6100});
    const auto runtime_b = reinterpret_cast<reshade::api::effect_runtime*>(uintptr_t{0x6200});
    const auto gaps = nx::present_gaps.load();
    nx::late_bridge_presents.push_back({.swapchain = a, .runtime = runtime_a, .had_nr = true});
    nx::OnLateBridgeOverlay(runtime_b);
    Check(nx::late_bridge_presents.size() == 1 && nx::present_gaps.load() == gaps,
          "wrong runtime does not consume another owner");
    nx::OnLateBridgeOverlay(runtime_a);
    Check(nx::late_bridge_presents.empty() && nx::present_gaps.load() == gaps + 1,
          "invalid current generation cancels once before dereferencing a borrowed object");
    nx::OnLateBridgeOverlay(runtime_a);
    Check(nx::present_gaps.load() == gaps + 1, "duplicate callback cannot deliver or gap twice");
    nx::late_bridge_presents.push_back({.swapchain = a, .runtime = runtime_a, .had_nr = true});
    nx::late_bridge_presents.push_back({.swapchain = b, .runtime = runtime_b, .had_nr = false});
    nx::CancelLateBridgePresent(a);
    Check(nx::late_bridge_presents.size() == 1 && nx::late_bridge_presents[0].swapchain == b,
          "finish/destroy cancels only its swapchain");
    nx::CancelLateBridgePresent(a);
    Check(nx::present_gaps.load() == gaps + 2, "owner cancellation is idempotent");
    nx::CancelLateBridgePresent(nullptr, runtime_b);
    Check(nx::late_bridge_presents.empty() && nx::present_gaps.load() == gaps + 2,
          "runtime destroy cancels its ticket without inventing mature NR");
    nx::present_gaps.store(gaps);
    std::cout << "LATE_BRIDGE_CONTRACT lifecycle=wrong-runtime+generation+finish+destroy duplicate=once owners=isolated evidence=CPU\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
}  // namespace late_bridge_present_cases
