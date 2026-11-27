/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <deps/imgui/imgui.h>

#include "../update.hpp"
#include "../version.h"
#include "i18n.hpp"
#include "strings.hpp"
#include "widgets.hpp"

namespace renodx::addons::dlss5::ui {

inline void DrawUpdatePanel(const update::Snapshot& snapshot) {
  const auto draw_failure = [](const update::Failure& failure) {
    const char* message = text::kUpdateErrorInternal;
    switch (failure.code) {
      case update::Error::NONE: return;
      case update::Error::THREAD_START: message = text::kUpdateErrorThread; break;
      case update::Error::CANCELLED: message = text::kUpdateErrorCancelled; break;
      case update::Error::INVALID_URL: message = text::kUpdateErrorAddress; break;
      case update::Error::NETWORK: message = text::kUpdateErrorNetwork; break;
      case update::Error::HTTP_STATUS: message = text::kUpdateErrorHttp; break;
      case update::Error::RATE_LIMITED: message = text::kUpdateErrorRateLimited; break;
      case update::Error::NO_RELEASE: message = text::kUpdateErrorNoRelease; break;
      case update::Error::INVALID_RELEASE: message = text::kUpdateErrorInvalidRelease; break;
      case update::Error::RESPONSE_TOO_LARGE: message = text::kUpdateErrorTooLarge; break;
      case update::Error::REDIRECT: message = text::kUpdateErrorRedirect; break;
      case update::Error::DIRECTORY_UNAVAILABLE: message = text::kUpdateErrorDirectory; break;
      case update::Error::FILE_CONFLICT: message = text::kUpdateErrorFileConflict; break;
      case update::Error::FILE_IO: message = text::kUpdateErrorFileIo; break;
      case update::Error::SIZE_MISMATCH: message = text::kUpdateErrorSize; break;
      case update::Error::DIALOG: message = text::kUpdateErrorDialog; break;
      case update::Error::SHELL: message = text::kUpdateErrorShell; break;
      case update::Error::INTERNAL: break;
    }
    ImGui::TextWrapped("%s", Tr(message));
    if (failure.http_status != 0) {
      ImGui::TextWrapped(Tr(text::kUpdateHttpStatus), static_cast<unsigned>(failure.http_status));
    }
    if (failure.os_code != 0) {
      ImGui::TextWrapped(Tr(text::kUpdateWindowsError), static_cast<unsigned>(failure.os_code));
    }
  };
  const auto draw_path = [](const char* label, const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    ImGui::TextWrapped(Tr(label), reinterpret_cast<const char*>(bytes.c_str()));
  };

  ImGui::PushID("nr_updates");
  ImGui::BeginGroup();
  SubCaption(text::kUpdateHeading);
  ImGui::TextWrapped(Tr(text::kUpdateCurrentVersion), NR_NEXUS_VERSION);
  bool auto_check = snapshot.settings.auto_check;
  if (Checkbox(Tr(text::kUpdateAutoCheck), &auto_check)) {
    update::SetAutoCheck(auto_check);
  }
  const bool rate_limited = snapshot.retry_after_utc > 0 && snapshot.retry_after_utc > update::UtcNow();
  ImGui::BeginDisabled(snapshot.busy || rate_limited);
  if (ActionButton(Tr(text::kUpdateCheck), "update_check")) {
    update::StartCheck();
  }
  ImGui::EndDisabled();
  const char* check_label = text::kUpdateUnchecked;
  switch (snapshot.check_state) {
    case update::CheckState::IDLE: break;
    case update::CheckState::CHECKING: check_label = text::kUpdateChecking; break;
    case update::CheckState::UPDATE_AVAILABLE: check_label = text::kUpdateAvailable; break;
    case update::CheckState::UP_TO_DATE: check_label = text::kUpdateLatest; break;
    case update::CheckState::AHEAD: check_label = text::kUpdateAhead; break;
    case update::CheckState::FAILED: check_label = text::kUpdateCheckFailed; break;
  }
  ImGui::TextWrapped("%s", Tr(check_label));
  draw_failure(snapshot.check_error);
  if (rate_limited) {
    const std::chrono::sys_seconds retry_time{std::chrono::seconds{snapshot.retry_after_utc}};
    const auto utc = std::format("{:%F %T}", retry_time);
    ImGui::TextWrapped(Tr(text::kUpdateRetryAfter), utc.c_str());
  }
  if (snapshot.stale && snapshot.release != nullptr) {
    ImGui::TextWrapped("%s", Tr(text::kUpdateStale));
  }

  ImGui::Spacing();
  const auto& directory = (snapshot.settings.download_directory.empty()
                               ? snapshot.addon_directory : snapshot.settings.download_directory);
  draw_path(text::kUpdateDirectory, directory);
  const bool complete = snapshot.download_state == update::DownloadState::COMPLETED && !snapshot.completed_path.empty();
  const auto draw_open_directory = [&](bool completed) {
    ImGui::BeginDisabled(snapshot.busy || (!completed && directory.empty()));
    if (ActionButton(Tr(text::kUpdateOpenDirectory),
                     completed ? "update_open_completed_directory" : "update_open_directory")) {
      update::OpenDirectory(completed);
    }
    ImGui::EndDisabled();
  };
  ImGui::BeginDisabled(snapshot.busy);
  if (ActionButton(Tr(text::kUpdateChooseDirectory), "update_choose_directory")) {
    update::ChooseDirectory();
  }
  ImGui::EndDisabled();
  const char* open_directory_label = Tr(text::kUpdateOpenDirectory);
  SameLineIfFits(ImGui::CalcTextSize(open_directory_label).x + ImGui::GetStyle().FramePadding.x * 2.f);
  draw_open_directory(false);
  draw_failure(snapshot.action_error);

  if (snapshot.release != nullptr) {
    ImGui::Spacing();
    const auto& release = *snapshot.release;
    ImGui::TextWrapped(Tr(text::kUpdateReleaseVersion), release.version.c_str());
    ImGui::TextWrapped(Tr(text::kUpdatePublished), release.published_at.c_str());
    std::string download_label = Tr(text::kUpdateDownload);
    download_label.replace(download_label.find("%s"), 2, release.version);
    ImGui::BeginDisabled(snapshot.busy || !release.asset.has_value());
    if (ActionButton(download_label.c_str(), "update_download")) {
      update::StartDownload();
    }
    ImGui::EndDisabled();
    const char* release_page = Tr(text::kUpdateReleasePage);
    SameLineIfFits(ImGui::CalcTextSize(release_page).x + ImGui::GetStyle().FramePadding.x * 2.f);
    ImGui::BeginDisabled(snapshot.busy || release.html_url.empty());
    if (ActionButton(release_page, "update_release_page")) {
      update::OpenRelease();
    }
    ImGui::EndDisabled();
    if (!release.asset.has_value()) {
      ImGui::TextWrapped("%s", Tr(text::kUpdateZipUnavailable));
    }
  }

  switch (snapshot.download_state) {
    case update::DownloadState::IDLE: break;
    case update::DownloadState::DOWNLOADING: {
      if (snapshot.release != nullptr) {
        ImGui::TextWrapped(Tr(text::kUpdateDownloading), snapshot.release->version.c_str());
      }
      const double progress = (snapshot.download_size == 0
                                   ? 0.0 : static_cast<double>(snapshot.downloaded_bytes) / snapshot.download_size);
      StyleScope progress_style;
      progress_style.Color(ImGuiCol_PlotHistogram, CurrentTheme().accent);
      ImGui::ProgressBar(static_cast<float>((std::min)(progress, 1.0)), ImVec2(-FLT_MIN, 0.f));
      break;
    }
    case update::DownloadState::FAILED:
      ImGui::TextWrapped("%s", Tr(text::kUpdateDownloadFailed));
      draw_failure(snapshot.download_error);
      break;
    case update::DownloadState::COMPLETED:
      if (complete) {
        ImGui::Spacing();
        SubCaption(text::kUpdateComplete);
        draw_path(text::kUpdateDownloadedFile, snapshot.completed_path.filename());
        draw_path(text::kUpdateSavedIn, snapshot.completed_path.parent_path());
        draw_open_directory(true);
        draw_path(text::kUpdateAddonDirectory, snapshot.addon_directory);
        ImGui::TextWrapped("%s", Tr(text::kUpdateInstallSteps));
      }
      break;
  }

  if (snapshot.release != nullptr && !snapshot.release->body.empty()) {
    ImGui::Spacing();
    SubCaption(text::kUpdateNotes);
    if (TreeNode("update_raw_notes", Tr(text::kUpdateRawNotes))) {
      auto raw = snapshot.release->body;
      ImGui::InputTextMultiline("##update_original_markdown", raw.data(), raw.size() + 1,
                                ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 10.f),
                                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
      ImGui::TreePop();
    } else {
      ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, ImGui::GetTextLineHeightWithSpacing() * 12.f));
      if (ImGui::BeginChild("##update_release_notes", ImVec2(0.f, 0.f),
                            ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0.f);
        bool in_comment = false;
        bool has_text = false;
        bool paragraph_break = false;
        std::string visible;
        std::string_view remaining = snapshot.release->body;
        while (!remaining.empty()) {
          const auto newline = remaining.find('\n');
          auto line = remaining.substr(0, newline);
          remaining.remove_prefix(newline == std::string_view::npos ? remaining.size() : newline + 1);
          if (line.ends_with('\r')) {
            line.remove_suffix(1);
          }
          if (line.find_first_not_of(" \t") == std::string_view::npos) {
            if (!in_comment) {
              paragraph_break = has_text;
            }
            continue;
          }

          // Strip HTML comments and anchor tags without removing visible text on either side.
          if (in_comment || line.find('<') != std::string_view::npos) {
            visible.clear();
            while (!line.empty()) {
              if (in_comment) {
                const auto end = line.find("-->");
                if (end == std::string_view::npos) break;
                line.remove_prefix(end + 3);
                in_comment = false;
                continue;
              }
              const auto tag = line.find('<');
              visible.append(line.substr(0, tag));
              if (tag == std::string_view::npos) break;
              line.remove_prefix(tag);
              if (line.starts_with("<!--")) {
                line.remove_prefix(4);
                in_comment = true;
                continue;
              }
              const bool anchor_tag = line.starts_with("<a ") || line.starts_with("<a\t")
                                      || line.starts_with("<a>") || line.starts_with("</a>");
              if (anchor_tag) {
                const auto end = line.find('>');
                if (end != std::string_view::npos) {
                  line.remove_prefix(end + 1);
                  continue;
                }
              }
              visible.push_back('<');
              line.remove_prefix(1);
            }
            line = visible;
          }
          const auto content_start = line.find_first_not_of(" \t");
          if (content_start == std::string_view::npos) continue;
          if (paragraph_break) {
            ImGui::Spacing();
            paragraph_break = false;
          }
          has_text = true;

          const auto content = line.substr(content_start);
          const auto heading = content.find_first_not_of('#');
          if (content_start <= 3 && heading != std::string_view::npos && heading >= 1 && heading <= 6
              && (content[heading] == ' ' || content[heading] == '\t')) {
            const auto title_start = content.find_first_not_of(" \t", heading);
            if (title_start == std::string_view::npos) continue;
            auto title = content.substr(title_start, content.find_last_not_of(" \t") - title_start + 1);
            const auto before_closing = title.find_last_not_of('#');
            if (before_closing != std::string_view::npos && before_closing + 1 < title.size()
                && (title[before_closing] == ' ' || title[before_closing] == '\t')) {
              title = title.substr(0, title.substr(0, before_closing).find_last_not_of(" \t") + 1);
            }
            StyleScope heading_style;
            heading_style.Color(ImGuiCol_Text, CurrentTheme().accent);
            ImGui::TextUnformatted(title.data(), title.data() + title.size());
          } else if (content.starts_with("- ") || content.starts_with("* ") || content.starts_with("+ ")) {
            if (content.size() == 2) continue;
            ImGui::Bullet();
            ImGui::TextUnformatted(content.data() + 2, content.data() + content.size());
          } else if (const auto number_end = content.find_first_not_of("0123456789");
                     number_end != std::string_view::npos && number_end >= 1 && number_end <= 9
                     && content.size() > number_end + 2
                     && (content[number_end] == '.' || content[number_end] == ')') && content[number_end + 1] == ' ') {
            ImGui::TextUnformatted(content.data(), content.data() + number_end + 1);
            ImGui::SameLine();
            ImGui::TextUnformatted(content.data() + number_end + 2, content.data() + content.size());
          } else {
            ImGui::TextUnformatted(line.data(), line.data() + line.size());
          }
        }
        ImGui::PopTextWrapPos();
      }
      ImGui::EndChild();
    }
  }
  ImGui::EndGroup();
  ImGui::PopID();
}

inline void DrawUpdatePanel() {
  DrawUpdatePanel(update::GetSnapshot());
}

}  // namespace renodx::addons::dlss5::ui
