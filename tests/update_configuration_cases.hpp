/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the disk-backed configuration fixture after production load.
if (test_case == "online-update") {
  namespace updater = renodx::addons::dlss5::update;
  using Json = renodx::addons::dlss5::presets::Json;
  OutputRuntimeFixture primary_runtime(false);
  OutputRuntimeFixture secondary_runtime(false);
  auto* primary = &primary_runtime;
  auto* secondary = &secondary_runtime;
  const auto read_update = [](const char* key) {
    char value[32768]{};
    GetPrivateProfileStringA(runtime::kUpdateConfigSection, key, "<absent>", value, sizeof(value), ini_path.c_str());
    return std::string(value);
  };
  auto snapshot = updater::GetSnapshot();
  Check(snapshot.settings.auto_check && snapshot.settings.download_directory.empty(), "update defaults are automatic and addon-relative");
  Check(!snapshot.busy && !snapshot.release, "loading configuration never starts a request");
  Check(read_update("AutoCheck") == "<absent>", "loading defaults does not create update preferences");

  const auto chosen = base_directory / L"更新,Update Files";
  const auto chosen_u8 = chosen.u8string();
  const std::string chosen_utf8(chosen_u8.begin(), chosen_u8.end());
  const int64_t next_utc = updater::UtcNow() + 3600;
  const auto next_text = std::to_string(next_utc);
  reshade::set_config_value(nullptr, runtime::kUpdateConfigSection, "AutoCheck", "0");
  reshade::set_config_value(nullptr, runtime::kUpdateConfigSection, "DownloadDirectory", chosen_utf8.c_str());
  reshade::set_config_value(nullptr, runtime::kUpdateConfigSection, "NextAutoCheckUtc", next_text.c_str());
  runtime::LoadUpdateConfiguration();
  snapshot = updater::GetSnapshot();
  Check(!snapshot.settings.auto_check && snapshot.settings.download_directory == chosen,
        "update preferences preserve Unicode, spaces and commas in the independent section");
  Check(snapshot.settings.next_auto_check_utc == next_utc, "startup cooldown uses full UTC seconds");

  runtime::enabled = false;
  runtime::OnUpdateInit(nullptr);
  runtime::OnUpdateInit(primary);
  Check(!updater::GetSnapshot().busy && !updater::GetSnapshot().release, "automatic Off never launches a startup request across runtimes");

  const auto saved_update = read_update("DownloadDirectory");
  const auto captured = runtime::preset_ui::Capture(renodx::addons::dlss5::presets::Scope::FULL, Json::object());
  for (const char* key : {"AutoCheck", "DownloadDirectory", "NextAutoCheckUtc"}) {
    Check(runtime::preset_ui::FindSetting(key) == nullptr, "update key is absent from image settings");
    Check(renodx::addons::dlss5::presets::FindRule(key) == nullptr, "update key is absent from preset rules");
    Check(!captured.settings.contains(key), "full preset excludes update preferences");
  }
  runtime::ResetSettings({runtime::kLookSettings, runtime::kLookResultSettings});
  runtime::PersistConfig();
  Check(read_update("AutoCheck") == "0" && read_update("DownloadDirectory") == saved_update
            && read_update("NextAutoCheckUtc") == next_text, "image resets and persistence preserve update preferences");

  updater::SetAutoCheck(true);
  runtime::live_effect_runtimes = {primary, secondary};
  const auto before_secondary = writes.size();
  runtime::OnUpdatePresent(secondary);
  Check(writes.size() == before_secondary, "a secondary runtime cannot race primary configuration writes");
  runtime::OnUpdatePresent(primary);
  runtime::live_effect_runtimes.clear();
  runtime::OnUpdatePresent(nullptr);
  Check(read_update("AutoCheck") == "1" && read_update("DownloadDirectory") == saved_update,
        "frame callback persists update settings in the global game INI");
  const auto write_count = writes.size();
  runtime::OnUpdatePresent(nullptr);
  Check(writes.size() == write_count, "unchanged preferences are not written every frame");
  runtime::OnUpdateInit(nullptr);
  Check(!updater::GetSnapshot().busy, "enabling automatic checks after startup does not bypass the once-only startup decision");

  for (const char* invalid : {"-1", "1795801234tail", "999999999999999999999999999"}) {
    reshade::set_config_value(nullptr, runtime::kUpdateConfigSection, "NextAutoCheckUtc", invalid);
    runtime::LoadUpdateConfiguration();
    Check(updater::GetSnapshot().settings.next_auto_check_utc == 0, "malformed UTC cooldown recovers to checkable");
  }
  reshade::set_config_value(nullptr, runtime::kUpdateConfigSection, "DownloadDirectory", "relative/path");
  runtime::LoadUpdateConfiguration();
  Check(updater::GetSnapshot().settings.download_directory.empty(), "relative directory falls back to the actual addon directory");
  probe_config_concurrency = true;
  std::vector<std::thread> config_threads;
  for (unsigned index = 0; index < 4; ++index) {
    config_threads.emplace_back([index] {
      for (unsigned step = 0; step < 10; ++step) {
        renodx::addons::dlss5::configuration::Set(nullptr, runtime::kUpdateConfigSection, "AutoCheck", (index + step) % 2 == 0);
        bool value = false;
        renodx::addons::dlss5::configuration::Get(nullptr, runtime::kUpdateConfigSection, "AutoCheck", value);
      }
    });
  }
  for (auto& thread : config_threads) {
    thread.join();
  }
  probe_config_concurrency = false;
  Check(!overlapping_config_calls && live_config_calls == 0,
        "SDK reads and writes from multiple runtime threads share the same serialized boundary");
  Check(non_global_calls == 0, "update configuration never uses runtime-local INI scope");
  std::cout << "PASS online update configuration evidence=CPU+MOCK checks=" << checks << " failures=" << failures << '\n';
  std::cout << checks << " checks, " << failures << " failures\n";
  return (failures == 0 ? 0 : 1);
}
