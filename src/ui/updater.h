/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/

// Update check against the latest GitHub release, at start and every 24 h. The menu bar shows
// "Update x.y.z" or "Up to date" left of the connection pill. The AppImage replaces its own file
// ($APPIMAGE) and restarts; an installed Windows build runs the new setup.exe silently, which
// starts the app again; other installs (.deb, the zip) get the release page.
// HTTP goes through the curl executable (Linux, and Windows 10 and later ship it): it does TLS
// with the system's certificates, so nothing is linked for it. KRAKEN_UPDATE_CHECK=0 turns the
// checks off, KRAKEN_UPDATE_URL replaces the GitHub API URL (a file:// JSON for testing).

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct App;

enum class UpdateStatus
{
    Idle,        // not checked yet / checks off
    Checking,
    UpToDate,
    Available,
    Downloading,
    Ready,       // the new version is in place (AppImage) or its installer downloaded (Windows)
    Failed,      // check or download failed: message says why
};

enum class InstallKind
{
    Manual,    // .deb, zip, a build tree: the release page
    AppImage,  // $APPIMAGE is replaced
    Installer, // Windows Inno Setup install: the new setup.exe runs silently
};

struct UpdateAsset
{
    std::string name;
    std::string url;
    uint64_t size = 0;
};

struct UpdateRelease
{
    std::string version; // the tag, e.g. "0.0.7"
    std::string tag;     // as published, "v" and all (raw.githubusercontent.com wants it)
    std::string page;    // html_url of the release
    std::vector<UpdateAsset> assets;
};

// What the worker thread writes. Shared, so a download still running at exit (the thread is
// detached) never touches a destroyed Updater.
struct UpdateShared
{
    mutable std::mutex mutex;
    UpdateStatus status = UpdateStatus::Idle;
    UpdateRelease release;
    UpdateAsset asset;               // the one for this install; empty url: update by hand
    std::filesystem::path download;  // being written / done
    std::filesystem::path installed; // AppImage: where the new version now is
    std::string message;
};

struct Updater
{
    bool enabled = false;       // main.cpp turns it on (with the setting); tests never check
    bool popup = false;         // the update window is open
    InstallKind kind = InstallKind::Manual;
    std::filesystem::path self; // $APPIMAGE / the installed exe's directory
    std::chrono::steady_clock::time_point next_check{};
    std::shared_ptr<UpdateShared> shared = std::make_shared<UpdateShared>();
};

// The latest release from the GitHub API's JSON; version empty when it does not parse.
[[nodiscard]] UpdateRelease updater_parse_release(std::string_view json);
// a > b, dotted numbers ("0.0.10" > "0.0.9"); a leading 'v' is skipped.
[[nodiscard]] bool updater_newer(std::string_view a, std::string_view b);
// The asset an install of kind takes, nullptr when it has none.
[[nodiscard]] const UpdateAsset* updater_pick_asset(const UpdateRelease& r, InstallKind kind);

// Detects the install kind; main.cpp, once.
void updater_init(Updater& u, bool enabled);
// Starts a check now (manual) or when due; every frame.
void updater_poll(App& app, Updater& u, bool now = false);
// Menu bar label, empty when there is nothing to show; highlight: an update to act on.
[[nodiscard]] std::string updater_label(const Updater& u, bool& highlight);
// A download runs: main.cpp keeps the 4 Hz tick so its progress shows.
[[nodiscard]] bool updater_downloading(const Updater& u);
// The update window while u.popup.
void draw_updater(App& app, Updater& u);
