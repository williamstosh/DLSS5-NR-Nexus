/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

{
  const float previous_width = panel_width;
  panel_width = 450.f;
  io.DisplaySize = ImVec2(panel_width, panel_height);
  update::Snapshot evidence;
  evidence.settings.auto_check = false;
  evidence.addon_directory = LR"(C:\Example Game\Addons)";
  update_fixture = &evidence;
  const auto draw = [&]() { frame(); frame(); frame(); };
  const auto is_drawn = [&](std::string_view text) {
    return std::ranges::any_of(rendered_text, [&](const auto& item) { return item.text == text; });
  };
  draw();
  check(is_drawn(ui::Tr(ui::text::kUpdateUnchecked)) && !find("update_check").disabled
            && !find("update_choose_directory").disabled && !find("update_open_directory").disabled,
        "update panel before a check allows manual checks and folder actions with automatic checks off");

  evidence.check_state = update::CheckState::UP_TO_DATE;
  auto release = UpdateUiRelease();
  evidence.release = release;
  draw();
  check(is_drawn(ui::Tr(ui::text::kUpdateLatest)) && !find("update_download").disabled,
        "up-to-date state keeps the official package available");
  evidence.check_state = update::CheckState::AHEAD;
  draw();
  check(is_drawn(ui::Tr(ui::text::kUpdateAhead)) && !find("update_download").disabled
            && !is_drawn(ui::Tr(ui::text::kUpdateAvailable)),
        "local version ahead is distinct from an upgrade and can still download the official release");
  check(is_drawn("更新日志") && is_drawn("Release notes") && is_drawn("选择更新包的保存目录。")
            && is_drawn("The progress bar reaches 100% when the package is saved.")
            && is_drawn("1.") && is_drawn("Choose a folder.")
            && is_drawn("Keep a backup of the `renodx-dlss5.addon64` file. "),
        "formatted notes draw both languages, headings, list content, line breaks and literal percent signs");
  check(std::ranges::none_of(rendered_text, [](const auto& item) {
          return item.text.find("<a id=") != std::string::npos || item.text.find("hidden") != std::string::npos
                 || item.text.find("Internal comment") != std::string::npos;
        }), "only the formatted notes hide HTML anchors and comments");
  const auto* update_window = find("update_check").window;
  const auto notes_toggle = ImHashStr("update_raw_notes", 0, ImHashStr("nr_updates", 0, update_window->ID));
  check(items.contains(notes_toggle), "the formatted notes offer the original Markdown");
  click(items.at(notes_toggle));
  const auto raw = find("##update_original_markdown");
  click(raw);
  const auto* input = ImGui::GetInputTextState(raw.id);
  check(input != nullptr && (input->Flags & ImGuiInputTextFlags_ReadOnly) != 0
            && release->body == input->TextToRevertTo.Data,
        "the original Markdown editor is read-only and preserves every byte of the release body");
  click(items.at(notes_toggle));

  release->asset.reset();
  draw();
  check(find("update_download").disabled && is_drawn(ui::Tr(ui::text::kUpdateZipUnavailable)),
        "an unavailable update ZIP disables downloads and explains the unique match requirement");

  evidence.check_state = update::CheckState::FAILED;
  evidence.check_error = {.code = update::Error::RATE_LIMITED, .http_status = 429};
  evidence.retry_after_utc = update::UtcNow() + 3600;
  evidence.stale = true;
  draw();
  check(is_drawn(ui::Tr(ui::text::kUpdateCheckFailed)) && is_drawn(ui::Tr(ui::text::kUpdateStale))
            && find("update_check").disabled && !find("update_choose_directory").disabled
            && !find("update_release_page").disabled,
        "a rate-limited retry does not disable other actions or hide cached release details");

  evidence.download_state = update::DownloadState::COMPLETED;
  evidence.completed_path = LR"(C:\Previous Downloads\NR-Nexus.zip)";
  evidence.settings.download_directory = LR"(C:\Next Downloads)";
  draw();
  check(!find("update_open_directory").disabled && !find("update_open_completed_directory").disabled,
        "a completed download keeps separate actions for its saved folder and the next download folder");

  update_fixture = nullptr;
  panel_width = previous_width;
  io.DisplaySize = ImVec2(panel_width, panel_height);
  io.AddMousePosEvent(-100.f, -100.f);
}
