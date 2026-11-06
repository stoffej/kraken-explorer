// REST API control routes: generator, macros, script, workspace / tabs, graph export, convert,
// watch, settings. Route handling only, on the test thread as the main thread.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include "app.h"
#include "core/rest_api.h"
#include "drivers/driver.h"
#include "ui_test.h" // fake_driver

namespace
{

namespace fs = std::filesystem;

bool has(const std::string& body, std::string_view s)
{
    return body.find(s) != std::string::npos;
}

// One tab, the fake interface and a DBC with Status (Depth, Rate) loaded on it.
struct Fixture
{
    UiTest ui{ImVec2(800.0f, 600.0f), true, true};
    App app;
    fs::path dir = fs::temp_directory_path() / "kraken_rest_api_control_test";
    int status = 0;

    Fixture()
    {
        fs::create_directories(dir);
        workspace_add_tab(app.workspace);
        Iface& i = app.ifaces.emplace_back();
        i.ops = &fake_driver;
        i.info.name = "fake0";
        std::ofstream(dir / "t.dbc") << "VERSION \"\"\n\nBU_: N\n\nBO_ 256 Status: 8 N\n"
                                        " SG_ Depth : 0|16@1+ (0.1,0) [0|6553.5] \"m\" N\n"
                                        " SG_ Rate : 16|8@1+ (1,0) [0|255] \"bpm\" N\n";
        rest_api_handle(app, "POST", "/interface", R"({"name":"fake0"})", status);
        rest_api_handle(app, "POST", "/database", std::format(R"({{"path":"{}","iface":"fake0"}})", (dir / "t.dbc").generic_string()), status);
        REQUIRE(status == 200);
    }
    ~Fixture()
    {
        app_measurement_stop(app);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string req(std::string_view method, std::string_view path, std::string_view body = "")
    {
        return rest_api_handle(app, method, path, body, status);
    }
};

} // namespace

TEST_CASE("generator: add, list, signal value, enable, remove")
{
    Fixture f;
    std::string body = f.req("POST", "/generator/add", R"({"message":"Nope"})");
    CHECK(f.status == 400);
    f.req("POST", "/generator/add", R"({"id":"zz"})");
    CHECK(f.status == 400);
    f.req("POST", "/generator/add", R"({"message":"Status","iface":"nope"})");
    CHECK(f.status == 400);
    body = f.req("POST", "/generator/add", R"({"message":"Status","iface":"fake0","interval_ms":50})");
    CHECK(f.status == 200);
    CHECK(has(body, "\"index\":0"));
    body = f.req("POST", "/generator/add", R"({"id":"0x123","dlc":2,"iface":"fake0"})");
    CHECK(has(body, "\"index\":1"));
    body = f.req("GET", "/generator");
    CHECK(has(body, R"("name":"Status","id":256,"iface":"fake0","interval_ms":50,"enabled":false)"));
    CHECK(has(body, R"("name":"Manual","id":291)"));

    f.req("POST", "/generator/signal", R"({"signal":"Status.Nope","value":1})");
    CHECK(f.status == 400);
    f.req("POST", "/generator/signal", R"({"signal":"Status.Depth"})");
    CHECK(f.status == 400); // value missing
    f.req("POST", "/generator/signal", R"({"signal":"Depth","value":100,"index":1})");
    CHECK(f.status == 400); // row 1 is not Status
    f.req("POST", "/generator/signal", R"({"signal":"Depth","value":100})");
    CHECK(f.status == 200);
    const TxGenerator& gen = f.app.tx_generators[f.app.workspace.tabs[0].uid];
    CHECK(gen.rows[0].msg.data[0] == 0xE8); // 1000 raw
    CHECK(gen.rows[0].msg.data[1] == 0x03);

    f.req("POST", "/generator/enable", R"({"index":0})");
    CHECK(f.status == 409); // no measurement
    f.req("POST", "/measurement/start");
    REQUIRE(f.app.measuring);
    f.req("POST", "/generator/enable", R"({"index":0,"interval_ms":20})");
    CHECK(f.status == 200);
    CHECK(gen.rows[0].enabled);
    CHECK(gen.rows[0].interval_ms == 20);
    f.req("POST", "/generator/enable", R"({"index":0,"enabled":false})");
    CHECK_FALSE(gen.rows[0].enabled);

    f.req("POST", "/generator/remove", R"({"index":5})");
    CHECK(f.status == 400);
    body = f.req("POST", "/generator/remove", R"({"index":0})");
    CHECK(f.status == 200);
    CHECK(gen.rows.size() == 1);
    f.req("POST", "/generator/remove", R"({"index":0,"tab":"nope"})");
    CHECK(f.status == 400);
}

TEST_CASE("macros, script, settings")
{
    Fixture f;
    CHECK(f.req("GET", "/macros") == "{\"macros\":[]}");
    f.req("POST", "/macros/run", R"({"name":"m"})");
    CHECK(f.status == 400);
    Macro& m = f.app.macros.items.emplace_back();
    m.name = "m";
    m.steps.push_back({.kind = MacroStepKind::Wait, .wait_ms = 10000});
    f.req("POST", "/macros/run", R"({"name":"m"})");
    CHECK(f.status == 200);
    CHECK(has(f.req("GET", "/macros"), R"({"name":"m","steps":1,"running":true})"));
    f.req("POST", "/macros/run", R"({"name":"m"})");
    CHECK(f.status == 409);
    f.req("POST", "/macros/stop", R"({"name":"m"})");
    CHECK_FALSE(m.running->load());

    f.req("POST", "/script/file", R"({"path":"/nonexistent.py"})");
    CHECK(f.status == 400);
    std::ofstream(f.dir / "s.py") << "print(1)\n";
    f.req("POST", "/script/file", std::format(R"({{"path":"{}","run":false}})", (f.dir / "s.py").generic_string()));
    CHECK(f.status == 200);
    CHECK(f.app.script.code == "print(1)\n");
    CHECK(has(f.req("GET", "/script/output"), "\"running\":false"));
    f.req("POST", "/script/stop");
    CHECK(f.status == 200);

    f.req("POST", "/settings", R"({"theme":"blue"})");
    CHECK(f.status == 400);
    f.req("POST", "/settings", R"({"font_scale":500})");
    CHECK(f.status == 400);
    CHECK(f.req("POST", "/settings", R"({"theme":"dark","font_scale":120})") == R"({"theme":"dark","font_scale":120})");
    CHECK(f.app.settings.theme == ThemeMode::Dark);
    CHECK(f.req("GET", "/settings") == R"({"theme":"dark","font_scale":120})");
}

TEST_CASE("workspace and tabs")
{
    Fixture f;
    f.req("POST", "/workspace/save");
    CHECK(f.status == 400); // never saved
    const std::string ws = (f.dir / "w.kraken").generic_string();
    f.req("POST", "/workspace/save", std::format(R"({{"path":"{}"}})", ws));
    CHECK(f.status == 200);
    CHECK(fs::exists(ws));
    f.req("POST", "/workspace/open", R"({"path":"/nonexistent.kraken"})");
    CHECK(f.status == 400);
    f.req("POST", "/workspace/open", std::format(R"({{"path":"{}"}})", ws));
    CHECK(f.status == 200);
    CHECK(f.app.settings.pending_open == ws);

    f.req("POST", "/tab/close", R"({"index":0})");
    CHECK(f.status == 409); // the last one
    std::string body = f.req("POST", "/tab/new", R"({"title":"Two"})");
    CHECK(has(body, R"("tabs":["Trace","Two"],"current":1)"));
    f.req("POST", "/tab/switch", R"({"title":"Nope"})");
    CHECK(f.status == 400);
    body = f.req("POST", "/tab/switch", R"({"title":"Trace"})");
    CHECK(has(body, "\"current\":0"));
    f.req("POST", "/tab/rename", R"({"title":"Two"})");
    CHECK(f.status == 400); // name missing
    body = f.req("POST", "/tab/rename", R"({"title":"Two","name":"Second"})");
    CHECK(has(body, R"(["Trace","Second"])"));
    body = f.req("POST", "/tab/close", R"({"title":"Second"})");
    CHECK(f.status == 200);
    CHECK(has(f.req("GET", "/tab"), R"("tabs":["Trace"])"));
}

TEST_CASE("graph export, convert, watch")
{
    Fixture f;
    f.req("POST", "/graph", R"({"signals":"Status.Depth"})");
    REQUIRE(f.status == 200);
    f.req("POST", "/graph/export", R"({"graph":"Graph 9","path":"x.csv"})");
    CHECK(f.status == 400);
    const std::string csv = (f.dir / "g.csv").generic_string();
    f.req("POST", "/graph/export", std::format(R"({{"graph":"Graph 1","path":"{}"}})", csv));
    CHECK(f.status == 200);
    std::string head;
    std::getline(std::ifstream(csv) >> std::ws, head);
    CHECK(head == "signal,unit,time_s,value");
    f.req("POST", "/graph/export", R"({"graph":"Graph 1","path":"/tmp/p.png"})");
    CHECK(f.app.workspace.tabs[0].graphs[0].png_path == "/tmp/p.png");

    f.req("POST", "/convert/database", R"({"in":"a.dbc"})");
    CHECK(f.status == 400);
    std::string body = f.req("POST", "/convert/database",
                             std::format(R"({{"in":"{}","out":"{}"}})", (f.dir / "t.dbc").generic_string(), (f.dir / "t.dbf").generic_string()));
    CHECK(f.status == 200);
    CHECK(fs::exists(f.dir / "t.dbf"));
    f.req("POST", "/convert/log", R"({"in":"a.asc","out":"b.xyz"})");
    CHECK(f.status == 400);
    CHECK(has(f.req("GET", "/convert/status"), "\"busy\":false"));

    f.req("POST", "/watch/add", R"({"signals":"Status.Nope"})");
    CHECK(f.status == 400);
    f.req("POST", "/watch/add", R"({"signals":"Status.Depth, Rate"})");
    CHECK(f.status == 200);
    BusMessage m{.id = 256};
    m.data[0] = 0x10; // 1000 m
    m.data[1] = 0x27;
    set_length(m, 8);
    trace_append(f.app.trace, {&m, 1});
    WatchWindow& w = f.app.watch_windows[f.app.workspace.tabs[0].uid];
    CHECK(w.open);
    watch_ingest(w, f.app.setup, f.app.trace); // app_frame does this every frame
    body = f.req("GET", "/watch");
    CHECK(has(body, R"({"signal":"Status.Depth","has_value":true,"value":1000,"min":1000,"max":1000,"count":1})"));
    CHECK(has(body, R"("signal":"Status.Rate")"));
    f.req("POST", "/watch/reset");
    CHECK(has(f.req("GET", "/watch"), "\"count\":0"));
}
