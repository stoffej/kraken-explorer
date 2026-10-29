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

#include "ui/updater.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#include <imgui.h>

#include "app.h"
#include "core/platform.h"

namespace
{

constexpr const char* api_url = "https://api.github.com/repos/stoffej/kraken-explorer/releases/latest";
constexpr const char* releases_url = "https://github.com/stoffej/kraken-explorer/releases/latest";
constexpr auto check_interval = std::chrono::hours(24);
constexpr auto retry_interval = std::chrono::hours(1); // after a failed check (offline)

std::string utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

// The string value of "key" at or after from; empty when missing.
std::string json_str(std::string_view json, std::string_view key, std::size_t from = 0, std::size_t* at = nullptr)
{
    const std::string needle = std::format("\"{}\"", key);
    std::size_t k = json.find(needle, from);
    while (k != std::string_view::npos)
    {
        std::size_t v = json.find_first_not_of(" \t\r\n", k + needle.size());
        if (v != std::string_view::npos && json[v] == ':')
        {
            v = json.find_first_not_of(" \t\r\n", v + 1);
            if (v != std::string_view::npos && json[v] == '"')
            {
                const std::size_t end = json.find('"', v + 1); // URLs and tags carry no escapes
                if (at != nullptr)
                {
                    *at = end;
                }
                return std::string(json.substr(v + 1, end - v - 1));
            }
        }
        k = json.find(needle, k + 1);
    }
    if (at != nullptr)
    {
        *at = std::string_view::npos;
    }
    return {};
}

std::vector<int> version_parts(std::string_view v)
{
    if (!v.empty() && (v.front() == 'v' || v.front() == 'V'))
    {
        v.remove_prefix(1);
    }
    std::vector<int> parts;
    while (!v.empty())
    {
        int n = 0;
        const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
        if (ec != std::errc{})
        {
            break;
        }
        parts.push_back(n);
        v.remove_prefix(static_cast<std::size_t>(end - v.data()));
        if (v.empty() || v.front() != '.')
        {
            break;
        }
        v.remove_prefix(1);
    }
    return parts;
}

using Wake = void (*)();

void check(std::shared_ptr<UpdateShared> s, InstallKind kind, std::filesystem::path, Wake wake)
{
    std::error_code ec;
    const std::filesystem::path dir = platform_cache_dir() / "kraken-explorer";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path out = dir / "latest-release.json";
    std::filesystem::remove(out, ec);
    const char* url = std::getenv("KRAKEN_UPDATE_URL"); // tests: a file:// release JSON
    const std::vector<std::string> args = {"curl", "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
                                           "-H", "User-Agent: kraken-explorer/" VERSION_STRING, "-o", utf8(out),
                                           url != nullptr && *url != '\0' ? url : api_url};
    const int rc = platform_run(args);
    std::ostringstream json;
    json << std::ifstream(out, std::ios::binary).rdbuf();
    const UpdateRelease r = updater_parse_release(json.str());

    const std::scoped_lock lock(s->mutex);
    if (rc != 0 || r.version.empty())
    {
        s->status = UpdateStatus::Failed;
        s->message = rc < 0 ? "The update check needs curl, which was not found."
                           : std::format("The update check failed (curl exit code {}).", rc);
    }
    else
    {
        s->release = r;
        const UpdateAsset* a = updater_pick_asset(r, kind);
        s->asset = a != nullptr ? *a : UpdateAsset{};
        s->status = updater_newer(r.version, VERSION_STRING) ? UpdateStatus::Available : UpdateStatus::UpToDate;
        s->message.clear();
    }
    if (wake != nullptr)
    {
        wake();
    }
}

void download(std::shared_ptr<UpdateShared> s, InstallKind kind, std::filesystem::path self, Wake wake)
{
    UpdateAsset asset;
    UpdateRelease release;
    {
        const std::scoped_lock lock(s->mutex);
        asset = s->asset;
        release = s->release;
    }
    std::error_code ec;
    std::filesystem::path target;
    if (kind == InstallKind::AppImage)
    {
        target = self; // next to it: the rename below is atomic and the running file keeps its inode
        target += ".update";
    }
    else
    {
        target = platform_cache_dir() / "kraken-explorer" / asset.name;
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    // curl writes a .part; a curl left behind by a quit mid-download (Windows keeps the file
    // open) then never blocks the next try.
    std::filesystem::path part = target;
    part += ".part";
    std::filesystem::remove(target, ec);
    std::filesystem::remove(part, ec);
    {
        const std::scoped_lock lock(s->mutex);
        s->download = part; // the progress reads its size
        s->installed.clear();
    }
    const std::vector<std::string> args = {"curl", "-fsSL", "--max-time", "1800", "-H", "User-Agent: kraken-explorer/" VERSION_STRING,
                                           "-o", utf8(part), asset.url};
    const int rc = platform_run(args);
    const uint64_t got = std::filesystem::file_size(part, ec);
    // ponytail: size check only, the transfer is HTTPS from GitHub; verify the asset's sha256
    // digest (the API lists it) once a hash function is at hand.
    bool ok = rc == 0 && !ec && (asset.size == 0 || got == asset.size);
    if (ok)
    {
        std::filesystem::rename(part, target, ec);
        ok = !ec;
    }
    std::filesystem::path installed;
    std::string error;
    if (!ok)
    {
        error = rc < 0 ? "curl was not found." : rc != 0 ? std::format("The download failed (curl exit code {}).", rc)
                                                         : "The download is incomplete.";
        if (rc == 23)
        {
            error = std::format("Cannot write {} (curl exit code 23).", utf8(part.parent_path()));
        }
        std::filesystem::remove(part, ec);
        std::filesystem::remove(target, ec);
    }
    else if (kind == InstallKind::AppImage)
    {
        using std::filesystem::perms;
        std::filesystem::permissions(target, perms::owner_all | perms::group_read | perms::group_exec | perms::others_read
                                                 | perms::others_exec, ec);
        // A file named after its version gets the new version's name.
        installed = self;
        std::string name = utf8(self.filename());
        if (const std::size_t at = name.find(VERSION_STRING); at != std::string::npos)
        {
            name.replace(at, std::string_view(VERSION_STRING).size(), release.version);
            installed = self.parent_path() / std::filesystem::path(std::u8string(name.begin(), name.end()));
        }
        std::filesystem::rename(target, installed, ec);
        if (ec)
        {
            error = std::format("Cannot replace {}: {}", utf8(self), ec.message());
            std::filesystem::remove(target, ec);
        }
        else if (installed != self)
        {
            std::filesystem::remove(self, ec);
        }
    }
    {
        const std::scoped_lock lock(s->mutex);
        s->status = error.empty() ? UpdateStatus::Ready : UpdateStatus::Failed;
        s->message = error;
        s->installed = installed;
        s->download = target; // Windows: the installer to run
    }
    if (wake != nullptr)
    {
        wake();
    }
}

void start(App& app, Updater& u, void (*job)(std::shared_ptr<UpdateShared>, InstallKind, std::filesystem::path, Wake))
{
    std::thread(job, u.shared, u.kind, u.self, app.tasks.wake).detach();
}

} // namespace

UpdateRelease updater_parse_release(std::string_view json)
{
    UpdateRelease r;
    std::string tag = json_str(json, "tag_name");
    if (version_parts(tag).empty())
    {
        return r;
    }
    if (tag.front() == 'v' || tag.front() == 'V')
    {
        tag.erase(0, 1);
    }
    r.version = tag;
    r.page = json_str(json, "html_url");
    std::size_t at = 0;
    for (;;)
    {
        std::size_t end = 0;
        const std::string url = json_str(json, "browser_download_url", at, &end);
        if (end == std::string_view::npos)
        {
            break;
        }
        // An asset object lists size before browser_download_url; one from an earlier object
        // (before this asset's, or its uploader's, opening brace) is not taken.
        UpdateAsset a{.name = url.substr(url.rfind('/') + 1), .url = url};
        const std::size_t s = json.rfind("\"size\"", end);
        const std::size_t v = s != std::string_view::npos ? json.find_first_of("0123456789", s) : std::string_view::npos;
        if (v != std::string_view::npos && s > at && s > json.rfind('{', end))
        {
            std::from_chars(json.data() + v, json.data() + json.size(), a.size);
        }
        r.assets.push_back(a);
        at = end;
    }
    return r;
}

bool updater_newer(std::string_view a, std::string_view b)
{
    return version_parts(a) > version_parts(b);
}

const UpdateAsset* updater_pick_asset(const UpdateRelease& r, InstallKind kind)
{
    const std::string_view suffix = kind == InstallKind::AppImage ? "-x86_64.AppImage"
                                    : kind == InstallKind::Installer ? "-win64-setup.exe" : "";
    if (suffix.empty())
    {
        return nullptr;
    }
    const auto it = std::ranges::find_if(r.assets, [&](const UpdateAsset& a) { return a.name.ends_with(suffix); });
    return it != r.assets.end() ? &*it : nullptr;
}

void updater_init(Updater& u, bool enabled)
{
    const char* off = std::getenv("KRAKEN_UPDATE_CHECK");
    u.enabled = enabled && !(off != nullptr && std::string_view(off) == "0");
    u.next_check = std::chrono::steady_clock::now();
#ifdef _WIN32
    // Inno Setup leaves its uninstaller next to the exe; the zip has none.
    const std::filesystem::path dir = platform_exe_path().parent_path();
    std::error_code ec;
    if (std::filesystem::exists(dir / "unins000.exe", ec))
    {
        u.kind = InstallKind::Installer;
        u.self = dir;
    }
#else
    if (const char* appimage = std::getenv("APPIMAGE"); appimage != nullptr && *appimage != '\0')
    {
        u.kind = InstallKind::AppImage;
        u.self = appimage;
    }
#endif
}

void updater_poll(App& app, Updater& u, bool now)
{
    if (!u.enabled && !now)
    {
        return;
    }
    const auto t = std::chrono::steady_clock::now();
    if (!now && t < u.next_check)
    {
        return;
    }
    {
        UpdateShared& s = *u.shared;
        const std::scoped_lock lock(s.mutex);
        if (s.status == UpdateStatus::Checking || s.status == UpdateStatus::Downloading || s.status == UpdateStatus::Ready)
        {
            return;
        }
        s.status = UpdateStatus::Checking;
    }
    u.next_check = t + check_interval;
    start(app, u, check);
}

bool updater_downloading(const Updater& u)
{
    const std::scoped_lock lock(u.shared->mutex);
    return u.shared->status == UpdateStatus::Downloading;
}

std::string updater_label(const Updater& u, bool& highlight)
{
    const UpdateShared& s = *u.shared;
    const std::scoped_lock lock(s.mutex);
    highlight = true;
    switch (s.status)
    {
    case UpdateStatus::UpToDate:
        highlight = false;
        return "Up to date";
    case UpdateStatus::Available:
        return "Update " + s.release.version;
    case UpdateStatus::Downloading:
    {
        std::error_code ec;
        const uint64_t got = std::filesystem::file_size(s.download, ec);
        return s.asset.size > 0 && !ec ? std::format("Updating {}%", got * 100 / s.asset.size) : "Updating";
    }
    case UpdateStatus::Ready:
        return "Restart to update";
    case UpdateStatus::Failed:
        // A failed check stays quiet (offline); a failed download still has its update.
        return updater_newer(s.release.version, VERSION_STRING) ? "Update " + s.release.version : std::string{};
    default:
        return {};
    }
}

void draw_updater(App& app, Updater& u)
{
    UpdateShared& s = *u.shared;
    UpdateStatus status;
    UpdateRelease release;
    UpdateAsset asset;
    std::string message;
    std::filesystem::path installed;
    std::filesystem::path downloaded;
    {
        const std::scoped_lock lock(s.mutex);
        // Retry a failed check in an hour rather than a day.
        if (s.status == UpdateStatus::Failed && s.release.version.empty()
            && u.next_check - std::chrono::steady_clock::now() > retry_interval)
        {
            u.next_check = std::chrono::steady_clock::now() + retry_interval;
        }
        status = s.status;
        release = s.release;
        asset = s.asset;
        message = s.message;
        installed = s.installed;
        downloaded = s.download;
    }
    if (!u.popup)
    {
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Kraken Explorer update", &u.popup,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    const std::string page = release.page.empty() ? releases_url : release.page;
    const bool newer = updater_newer(release.version, VERSION_STRING);
    ImGui::Text("Running version %s", VERSION_STRING);
    switch (status)
    {
    case UpdateStatus::Checking:
        ImGui::TextUnformatted("Checking for updates...");
        break;
    case UpdateStatus::UpToDate:
        ImGui::Text("Up to date: %s is the latest release.", release.version.c_str());
        break;
    case UpdateStatus::Downloading:
    {
        std::error_code ec;
        const uint64_t got = std::filesystem::file_size(downloaded, ec);
        ImGui::Text("Downloading %s ...", asset.name.c_str());
        ImGui::ProgressBar(asset.size > 0 && !ec ? static_cast<float>(got) / static_cast<float>(asset.size) : 0.0f,
                           ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f));
        break;
    }
    case UpdateStatus::Ready:
        if (u.kind == InstallKind::AppImage)
        {
            ImGui::Text("Version %s is installed as\n%s", release.version.c_str(), utf8(installed).c_str());
            if (ImGui::Button("Restart now"))
            {
                app.restart = {utf8(installed)};
                app.quit = true;
            }
        }
        else
        {
            ImGui::Text("The installer of version %s is downloaded.", release.version.c_str());
            if (ImGui::Button("Install and restart"))
            {
                // Inno Setup: silent, and its [Run] entry for silent installs starts the app again.
                app.restart = {utf8(downloaded), "/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART"};
                app.quit = true;
            }
            ImGui::SetItemTooltip("Closes Kraken Explorer, installs %s and starts it", release.version.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Later"))
        {
            u.popup = false;
        }
        break;
    case UpdateStatus::Available:
    case UpdateStatus::Failed:
    case UpdateStatus::Idle:
        if (!message.empty())
        {
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", message.c_str());
        }
        if (newer)
        {
            ImGui::Text("Version %s is out.", release.version.c_str());
            if (!asset.url.empty())
            {
                if (ImGui::Button("Update now"))
                {
                    {
                        const std::scoped_lock lock(s.mutex);
                        s.status = UpdateStatus::Downloading;
                        s.message.clear();
                    }
                    start(app, u, download);
                }
                ImGui::SetItemTooltip("%s", u.kind == InstallKind::AppImage
                                                ? "Downloads the new AppImage over this one; restart to use it"
                                                : "Downloads the installer; it runs when you restart");
                ImGui::SameLine();
            }
            else
            {
                ImGui::TextDisabled("This install updates from the release page (.deb, zip, .dmg).");
            }
        }
        break;
    }
    if (ImGui::Button("Release page"))
    {
        ImGui::GetPlatformIO().Platform_OpenInShellFn(ImGui::GetCurrentContext(), page.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(status == UpdateStatus::Checking || status == UpdateStatus::Downloading || status == UpdateStatus::Ready);
    if (ImGui::Button("Check again"))
    {
        updater_poll(app, u, true);
    }
    ImGui::EndDisabled();
    ImGui::End();
}
