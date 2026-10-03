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

// ui/watch_window: values, min / max, count and the changed mark from new trace frames, the
// workspace XML round trip and headless drawing. Expected values by hand from the DBC below.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>

#include <imgui.h>
#include <pugixml.hpp>

#include "app.h"

#include "core/trace.h"
#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"
#include "ui/theme.h"
#include "ui/watch_window.h"
#include "ui/workspace_tabs.h"

namespace
{

// Speed: Intel bytes 2-3, 0.25 rpm. Gear: byte 4 with a value table.
constexpr const char* dbc = R"(VERSION ""

BU_: ECU

BO_ 291 Engine: 8 ECU
 SG_ Speed : 16|16@1+ (0.25,0) [0|16383.75] "rpm" ECU
 SG_ Gear : 32|8@1+ (1,0) [0|8] "" ECU

VAL_ 291 Gear 0 "Neutral" 1 "First" ;
)";

void add_db(Setup& setup)
{
    auto db = std::make_shared<CanDb>();
    REQUIRE(dbc_parse(dbc, *db));
    setup.networks.push_back({.name = "Net", .can_dbs = {db}});
    setup_rebuild_cache(setup);
}

void add(WatchWindow& w, const Setup& setup, std::string_view label)
{
    signal_search_update(w.picker, setup, "");
    const auto e = std::ranges::find(w.picker.entries, label, &SignalEntry::label);
    REQUIRE(e != w.picker.entries.end());
    watch_add(w, setup, *e);
}

void engine_frame(Trace& trace, uint8_t speed_lo, uint8_t speed_hi, uint8_t gear)
{
    BusMessage m{.id = 0x123};
    set_length(m, 8);
    m.data[2] = speed_lo;
    m.data[3] = speed_hi;
    m.data[4] = gear;
    trace_append(trace, std::array{m});
}

} // namespace

TEST_CASE("values, min / max, count and the changed mark follow the trace")
{
    Setup setup;
    add_db(setup);
    WatchWindow w;
    add(w, setup, "Engine.Speed");
    add(w, setup, "Engine.Gear");
    add(w, setup, "Engine.Speed"); // already listed
    REQUIRE(w.items.size() == 2);
    Trace trace;
    watch_ingest(w, setup, trace);
    CHECK_FALSE(w.items[0].has_value);

    engine_frame(trace, 0xB0, 0x36, 1); // 0x36B0 = 14000 -> 3500 rpm
    watch_ingest(w, setup, trace);
    CHECK(w.items[0].value == doctest::Approx(3500.0));
    CHECK_FALSE(w.items[0].changed); // the first value has nothing before it
    CHECK(w.items[1].raw == 1);

    engine_frame(trace, 0xA0, 0x0F, 1); // 0x0FA0 = 4000 -> 1000 rpm, the gear stays
    watch_ingest(w, setup, trace);
    CHECK(w.items[0].value == doctest::Approx(1000.0));
    CHECK(w.items[0].changed);
    CHECK(w.items[0].min == doctest::Approx(1000.0));
    CHECK(w.items[0].max == doctest::Approx(3500.0));
    CHECK(w.items[0].count == 2);
    CHECK_FALSE(w.items[1].changed);
    watch_ingest(w, setup, trace); // nothing new
    CHECK(w.items[0].count == 2);

    trace_clear(trace);
    watch_ingest(w, setup, trace);
    CHECK_FALSE(w.items[0].has_value);
    CHECK(w.items[0].count == 0);
}

TEST_CASE("workspace XML round trip keeps the signals, a signal gone from the databases stays listed")
{
    Setup setup;
    add_db(setup);
    WatchWindow w;
    add(w, setup, "Engine.Gear");
    w.items.push_back({.network = "Net", .raw_id = 291, .signal = "Gone"});
    pugi::xml_document doc;
    pugi::xml_node el = doc.append_child("watchwindow");
    watch_save_xml(w, el);
    WatchWindow q;
    watch_load_xml(q, el);
    REQUIRE(q.items.size() == 2);
    CHECK(q.items[0].network == "Net");
    CHECK(q.items[0].raw_id == 291);
    CHECK(q.items[0].signal == "Gear");
    Trace trace;
    watch_ingest(q, setup, trace);
    CHECK(q.items[0].sig != nullptr);
    CHECK(q.items[1].sig == nullptr);
}

TEST_CASE("the window draws headless: values, a value-table name and an unresolved row")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    theme_load_fonts(15.0f);
    {
        App app;
        add_db(app.setup);
        const WorkspaceTab& tab = workspace_add_tab(app.workspace);
        WatchWindow& w = app.watch_windows[tab.uid];
        w.open = true;
        add(w, app.setup, "Engine.Speed");
        add(w, app.setup, "Engine.Gear");
        w.items.push_back({.network = "Net", .raw_id = 291, .signal = "Gone"});
        engine_frame(app.trace, 0xB0, 0x36, 1);
        engine_frame(app.trace, 0xA0, 0x0F, 0);
        for (int i = 0; i < 3; ++i)
        {
            ImGui::NewFrame();
            watch_ingest(w, app.setup, app.trace);
            draw_watch_window(app, tab, w);
            ImGui::EndFrame();
        }
        CHECK(w.items[1].changed);
        CHECK(w.items[2].sig == nullptr);
    }
    ImGui::DestroyContext();
}
