// Update check (ui/updater): the GitHub release JSON, version order, the asset per install kind.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/updater.h"

namespace
{

// The shape of the GitHub API's releases/latest answer, cut to the keys that matter.
constexpr const char* release_json = R"({
  "url": "https://api.github.com/repos/stoffej/kraken-explorer/releases/1",
  "html_url": "https://github.com/stoffej/kraken-explorer/releases/tag/0.0.7",
  "tag_name": "0.0.7",
  "name": "Kraken Explorer 0.0.7",
  "draft": false,
  "assets": [
    {"url": "a", "name": "kraken-explorer-0.0.7-win64-setup.exe", "size": 13783111,
     "browser_download_url": "https://github.com/stoffej/kraken-explorer/releases/download/0.0.7/kraken-explorer-0.0.7-win64-setup.exe"},
    {"url": "b", "name": "kraken-explorer_0.0.7_amd64.deb", "size": 2876540,
     "browser_download_url": "https://github.com/stoffej/kraken-explorer/releases/download/0.0.7/kraken-explorer_0.0.7_amd64.deb"},
    {"url": "c", "name": "Kraken_Explorer-0.0.7-x86_64.AppImage", "size": 21305848,
     "browser_download_url": "https://github.com/stoffej/kraken-explorer/releases/download/0.0.7/Kraken_Explorer-0.0.7-x86_64.AppImage"}
  ],
  "body": "notes with a \"tag_name\": \"9.9.9\" inside"
})";

} // namespace

TEST_CASE("release JSON: tag, page, assets with their sizes")
{
    const UpdateRelease r = updater_parse_release(release_json);
    CHECK(r.version == "0.0.7");
    CHECK(r.tag == "0.0.7");
    CHECK(r.page == "https://github.com/stoffej/kraken-explorer/releases/tag/0.0.7");
    REQUIRE(r.assets.size() == 3);
    CHECK(r.assets[2].name == "Kraken_Explorer-0.0.7-x86_64.AppImage");
    CHECK(r.assets[2].size == 21305848);
    CHECK(r.assets[0].size == 13783111);
    CHECK(r.assets[2].url.ends_with("/0.0.7/Kraken_Explorer-0.0.7-x86_64.AppImage"));

    CHECK(updater_pick_asset(r, InstallKind::AppImage)->name == r.assets[2].name);
    CHECK(updater_pick_asset(r, InstallKind::Installer)->name == r.assets[0].name);
    CHECK(updater_pick_asset(r, InstallKind::Manual) == nullptr);
}

TEST_CASE("a 'v' prefix is skipped, garbage and rate-limit answers give no version")
{
    CHECK(updater_parse_release(R"({"tag_name": "v1.2.3", "assets": []})").version == "1.2.3");
    CHECK(updater_parse_release(R"({"tag_name": "v1.2.3", "assets": []})").tag == "v1.2.3");
    CHECK(updater_parse_release(R"({"message": "API rate limit exceeded", "tag_name": "latest"})").version.empty());
    CHECK(updater_parse_release("").version.empty());
    CHECK(updater_parse_release("not json").version.empty());
}

TEST_CASE("version order is numeric per part")
{
    CHECK(updater_newer("0.0.7", "0.0.6"));
    CHECK(updater_newer("0.0.10", "0.0.9"));
    CHECK(updater_newer("1.0.0", "0.9.9"));
    CHECK(updater_newer("v0.1.0", "0.0.6"));
    CHECK(!updater_newer("0.0.6", "0.0.6"));
    CHECK(!updater_newer("0.0.5", "0.0.6"));
    CHECK(!updater_newer("", "0.0.6"));
}
