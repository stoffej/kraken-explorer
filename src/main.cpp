/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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
#include "app.h"
#include "core/log.h"
#include "core/png.h"
#include "core/rest_api.h"
#include "ui/frame_cache.h"
#include "ui/icons.h"
#include "ui/settings.h"
#include "ui/theme.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // AttachConsole
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <implot.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // SettingsDirtyTimer

namespace
{

// ponytail: fixed 120 fps cap on event-driven frames (RX, tasks, input); vsync still applies on top,
// so a 60 Hz monitor stays at 60. Follow the monitor refresh rate instead if that ever matters.
constexpr double min_frame_interval = 1.0 / 120.0;

// ponytail: RX-only frames (no user input for input_hold seconds, no replay animating) are capped at
// 120 fps too: a frame under a 1M frames/s flood measured < 10 ms even on llvmpipe. Lower it (or
// make it a setting) if RX redraw costs too much CPU on weak machines.
constexpr double rx_frame_interval = 1.0 / 120.0;
constexpr double input_hold = 0.5;

// One glfwPostEmptyEvent per rendered frame: RX threads post only while no wake-up is pending.
// Cleared right before a frame drains the inboxes and tasks, so nothing delivered after it is missed.
std::atomic<bool> wake_pending{false};

void wake_main_loop()
{
    if (!wake_pending.exchange(true, std::memory_order_acq_rel))
    {
        glfwPostEmptyEvent();
    }
}

// How long the idle loop may sleep when no event arrives. A replay loading or playing animates the
// depth gauge sweep (~30 fps); a script or a pending ini save keeps the 4 Hz tick. So do the 2 s
// after any wakeup, for ImGui's hover/tooltip delays and window settling, and an active text field
// (cursor blink). A measurement needs no tick of its own: every RX batch wakes the loop, and the 2 s
// tail drains the recorder queue (250 ms) and lets CAN Status / frames/s / bus load fall to zero.
// ponytail: counters that change without a frame (sysfs link state, error counters when error frames
// are masked) refresh only on the next wakeup; add a slow (1 s) tick while measuring if that matters.
bool replay_animating(const App& app)
{
    return ink_active(app.ink) || std::ranges::any_of(app.replays, [](const auto& kv)
                                      { return kv.second.open && (kv.second.loader.joinable() || kv.second.running); });
}

constexpr double watch_tick = 1.0; // s: the file-change check of a loaded log, no frame drawn for it

double wait_timeout(const App& app, double since_wake)
{
    if (replay_animating(app))
    {
        return 1.0 / 30.0;
    }
    // A load running with its window closed is polled; a loaded file is checked for changes once a
    // second (draw_replay). ponytail: a 1 Hz wakeup while a file is loaded; inotify if idle power matters.
    bool loading = false;
    bool watching = false;
    for (const auto& [uid, r] : app.replays)
    {
        loading = loading || r.loader.joinable();
        watching = watching || !r.data.path.empty();
    }
    const bool busy = app.python.running || ImGui::GetCurrentContext()->SettingsDirtyTimer > 0.0f
                      || ImGui::GetIO().WantTextInput || since_wake < 2.0 || loading;
    return busy ? 0.25 : watching ? watch_tick : -1.0;
}

} // namespace

namespace
{

// Graph "Export to PNG" (App::PngExport): the off-screen window's draw list, rendered into a
// framebuffer of its own size, read back and written by a thread. The FBO calls are GL 3.0, not in
// <GL/gl.h>: fetched from GLFW once.
void render_png_export(const App::PngExport& e)
{
    using GenFn = void (*)(GLsizei, GLuint*);
    using BindFn = void (*)(GLenum, GLuint);
    using TexFn = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
    static const auto gen_framebuffers = reinterpret_cast<GenFn>(glfwGetProcAddress("glGenFramebuffers"));
    static const auto delete_framebuffers = reinterpret_cast<void (*)(GLsizei, const GLuint*)>(glfwGetProcAddress("glDeleteFramebuffers"));
    static const auto bind_framebuffer = reinterpret_cast<BindFn>(glfwGetProcAddress("glBindFramebuffer"));
    static const auto framebuffer_texture = reinterpret_cast<TexFn>(glfwGetProcAddress("glFramebufferTexture2D"));
    constexpr GLenum framebuffer = 0x8D40, color0 = 0x8CE0, rgba8 = 0x8058; // GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RGBA8

    // Not on screen: out of the main viewport's draw data (the window was forced into it), in the
    // order the screen would have drawn them (a child after its parent).
    ImDrawData* screen = ImGui::GetDrawData();
    ImDrawData dd;
    for (int i = 0; i < screen->CmdLists.Size;)
    {
        ImDrawList* list = screen->CmdLists[i];
        if (std::ranges::find(e.lists, list) == e.lists.end())
        {
            ++i;
            continue;
        }
        screen->TotalVtxCount -= list->VtxBuffer.Size;
        screen->TotalIdxCount -= list->IdxBuffer.Size;
        screen->CmdLists.erase(screen->CmdLists.Data + i);
        dd.CmdLists.push_back(list);
        dd.TotalVtxCount += list->VtxBuffer.Size;
        dd.TotalIdxCount += list->IdxBuffer.Size;
    }
    screen->CmdListsCount = screen->CmdLists.Size;
    if (gen_framebuffers == nullptr || bind_framebuffer == nullptr || framebuffer_texture == nullptr)
    {
        log_error("Graph: Export to PNG needs OpenGL framebuffers");
        return;
    }
    GLuint tex = 0;
    GLuint fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, rgba8, e.w, e.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gen_framebuffers(1, &fbo);
    bind_framebuffer(framebuffer, fbo);
    framebuffer_texture(framebuffer, color0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, e.w, e.h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    dd.Valid = true;
    dd.CmdListsCount = dd.CmdLists.Size;
    dd.DisplayPos = ImVec2(e.x, e.y);
    dd.DisplaySize = ImVec2(static_cast<float>(e.w), static_cast<float>(e.h));
    dd.FramebufferScale = ImVec2(1.0f, 1.0f);
    dd.OwnerViewport = screen->OwnerViewport;
    dd.Textures = screen->Textures; // glyphs at the export's font size may be new this frame
    ImGui_ImplOpenGL3_RenderDrawData(&dd);
    std::vector<uint8_t> rgba(static_cast<std::size_t>(e.w) * e.h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, e.w, e.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    bind_framebuffer(framebuffer, 0);
    if (delete_framebuffers != nullptr)
    {
        delete_framebuffers(1, &fbo);
    }
    glDeleteTextures(1, &tex);
    std::thread([rgba = std::move(rgba), w = e.w, h = e.h, path = e.path]() mutable
    {
        const std::size_t stride = static_cast<std::size_t>(w) * 4;
        for (int row = 0; row < h / 2; ++row) // GL rows are bottom-up
        {
            std::swap_ranges(rgba.begin() + static_cast<std::ptrdiff_t>(row * stride), rgba.begin() + static_cast<std::ptrdiff_t>((row + 1) * stride),
                             rgba.begin() + static_cast<std::ptrdiff_t>((h - 1 - row) * stride));
        }
        // ImGui's blending over a cleared (0,0,0,0) target leaves premultiplied colour: straight for PNG.
        for (std::size_t i = 0; i < rgba.size(); i += 4)
        {
            const unsigned a = rgba[i + 3];
            for (std::size_t c = 0; a != 0 && a != 255 && c < 3; ++c)
            {
                rgba[i + c] = static_cast<uint8_t>(std::min(255u, (rgba[i + c] * 255u + a / 2) / a));
            }
        }
        std::string error;
        if (png_write_file(path, w, h, rgba, &error))
        {
            log_info(std::format("Graph exported to {} ({}x{})", path, w, h));
        }
        else
        {
            log_error("Graph: " + error);
        }
    }).detach();
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // A GUI subsystem exe starts without stdout / stderr unless they are redirected: take the
    // console of the shell that started us, if any.
    if (_fileno(stderr) < 0 && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    // --smoke N: render N frames, then exit (headless CI / sanitizer runs).
    // --measure: start a measurement on the default/loaded setup right away.
    // --record: arm recording (Ctrl+R) right away.
    // --setup: open the measurement setup dialog right away.
    // --replay FILE: open a Replay View with FILE, autoplay on (plays with --measure).
    // --workspace FILE: load a .kraken workspace before anything starts.
    // --script FILE: load a Python script into the script window and run it; under --smoke
    //   its console is printed to stderr at exit.
    // --api PORT: REST API on 127.0.0.1:PORT (docs/manual.md "REST API").
    long smoke_frames = -1;
    bool setup = false;
    bool measure = false;
    bool record = false;
    const char* replay_file = nullptr;
    const char* workspace = nullptr;
    const char* script = nullptr;
    long api_port = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--api") == 0 && i + 1 < argc)
        {
            api_port = std::strtol(argv[++i], nullptr, 10);
            continue;
        }
        if (std::strcmp(argv[i], "--smoke") == 0 && i + 1 < argc)
        {
            smoke_frames = std::strtol(argv[++i], nullptr, 10);
        }
        else if (std::strcmp(argv[i], "--workspace") == 0 && i + 1 < argc)
        {
            workspace = argv[++i];
        }
        else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc)
        {
            script = argv[++i];
        }
        else if (std::strcmp(argv[i], "--measure") == 0)
        {
            measure = true;
        }
        else if (std::strcmp(argv[i], "--record") == 0)
        {
            record = true;
        }
        else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
        {
            replay_file = argv[++i];
        }
        else if (std::strcmp(argv[i], "--setup") == 0)
        {
            setup = true;
        }
    }

    glfwSetErrorCallback([](int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); });
    // GLFW cannot place windows on Wayland, so imgui_impl_glfw turns multi-viewports off there.
    // Prefer X11 (XWayland) whenever an X display exists so floating docks become OS windows.
    // ponytail: XWayland scales blurry on fractional HiDPI; drop this if that matters more.
#ifndef _WIN32
    if (smoke_frames >= 0 || std::getenv("DISPLAY") != nullptr)
    {
        // Smoke runs are headless under xvfb-run, which leaves WAYLAND_DISPLAY set: without
        // this GLFW would open on the real Wayland desktop (and pull in libdecor's GTK plugin).
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
#endif
    if (!glfwInit())
    {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Kraken Explorer: Day of the N2K Tentacle " VERSION_STRING " — Deeper than a Peak. Wireshark is stuck in shallow waters.", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Floating docks become OS windows, except in a Wayland session: through XWayland the
    // compositor (sway) tiles them and reports window positions that put the mouse off by the
    // frame height, so clicks miss. ponytail: no setting for it; add one if someone wants
    // multi-viewport on Wayland with a floating-window rule in their compositor.
    if (std::getenv("WAYLAND_DISPLAY") == nullptr)
    {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.IniFilename = nullptr; // settings_init/settings_save handle the ini under $XDG_CONFIG_HOME
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    float dpi_scale = 1.0f;
    glfwGetWindowContentScale(window, &dpi_scale, nullptr);
    icons_init(std::max(dpi_scale, 2.0f)); // 2x cells: stay sharp at font scales up to 175 %
    theme_logo_init(window, 256); // also the About logo, drawn at ~10x font size

    App app;
    app.tasks.wake = wake_main_loop; // RX threads and posted tasks end glfwWaitEventsTimeout at once
    settings_init(app); // before the interfaces: CANblaster, recording folder, tabs + layout
    settings_apply_theme(app);
    app.fonts = theme_load_fonts(15.0f * dpi_scale); // ui/font_scale multiplies it via style.FontScaleMain;
    app_init_interfaces(app);
    RestApi api;
    if (api_port > 0)
    {
        rest_api_start(api, app, static_cast<uint16_t>(api_port)); // logs when the port is taken
    }
    if (workspace != nullptr)
    {
        workspace_load(app, workspace); // outside a frame, before the measurement starts
    }
    if (record)
    {
        recorder_set_armed(app.recorder, true, false);
    }
    if (replay_file != nullptr)
    {
        if (app.workspace.tabs.empty())
        {
            workspace_add_tab(app.workspace);
        }
        Replay& r = app.replays[workspace_current(app.workspace)->uid];
        r.open = true;
        r.autoplay = true;
        replay_load(app, r, replay_file);
    }
    if (measure)
    {
        app_measurement_start(app);
    }
    if (setup && !measure)
    {
        setup_dialog_open(app, app.setup_dialog);
    }
    if (script != nullptr && script_window_load_file(app.script, script))
    {
        app.script.was_measuring = app.measuring; // no AutoRun edge for the start above
        script_window_run(app, app.script);
    }
    const double loop_start = glfwGetTime();
    long frame = 0;
    double last_wake = loop_start;
    double last_frame = loop_start;
    double last_input = loop_start;
    ImVec2 last_display_size = io.DisplaySize;
    for (; !app.quit && !glfwWindowShouldClose(window); ++frame)
    {
        if (smoke_frames >= 0 && frame >= smoke_frames)
        {
            break;
        }
        if (smoke_frames >= 0)
        {
            glfwPollEvents();
        }
        else
        {
            // Idle: 0 % CPU; input, RX and posted tasks (glfwPostEmptyEvent) wake us immediately.
            const double t0 = glfwGetTime();
            const double timeout = wait_timeout(app, t0 - last_wake);
            timeout < 0.0 ? glfwWaitEvents() : glfwWaitEventsTimeout(timeout);
            const double t1 = glfwGetTime();
            const bool timed_out = timeout >= 0.0 && t1 - t0 >= timeout * 0.9;
            if (!timed_out)
            {
                last_wake = t1; // woken early: by an event, not the timeout
            }
            else if (timeout == watch_tick && !wake_pending.load(std::memory_order_acquire))
            {
                // A wake (a posted task, an RX batch) in the last 10 % of the tick looks like a
                // timeout; skipping the frame then would leave wake_pending set and the task queued
                // until the next input event (the REST API hung that way).
                // Only the file watch asked for this wakeup: a stat per loaded file, no frame (a
                // redraw a second cost 5-10 % CPU on a software GL rig, measured with perf).
                for (auto& [uid, r] : app.replays)
                {
                    replay_watch(app, r);
                }
                continue;
            }
            // Coalesce everything arriving within one frame interval of the last frame into one frame.
            // Input queued by the backend's GLFW callbacks during the wait switches to the 60 fps cap.
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const bool fast = glfwGetTime() - last_input < input_hold || replay_animating(app);
            for (double left = 0.0;; glfwWaitEventsTimeout(left))
            {
                const double interval =
                    fast || !g.InputEventsQueue.empty() ? min_frame_interval : rx_frame_interval;
                left = last_frame + interval - glfwGetTime();
                if (left <= 0.0)
                {
                    break;
                }
            }
            wake_pending.store(false, std::memory_order_release);
            last_frame = glfwGetTime();
        }

        app_before_frame(app);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // Mouse, keys, wheel, focus (all ImGui input events of this frame) or a resize: user input.
        if (!ImGui::GetCurrentContext()->InputEventsTrail.empty() || io.DisplaySize.x != last_display_size.x
            || io.DisplaySize.y != last_display_size.y)
        {
            last_input = glfwGetTime();
            last_display_size = io.DisplaySize;
        }
        app_frame(app);
        ImGui::Render();
        if (app.png_export)
        {
            render_png_export(*app.png_export); // before the screen: it takes its draw list out
            app.png_export.reset();
        }

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(window); // the platform windows switched the GL context
        }
        glfwSwapBuffers(window);
        settings_save_if_wanted(app);
    }

    const double loop_seconds = glfwGetTime() - loop_start;
    uint64_t rx_overruns = 0; // read before app_measurement_stop: stats are only there while open
    for (Iface& i : app.ifaces)
    {
        IfaceStats st;
        if (iface_stats(i, st))
        {
            rx_overruns += st.rx_overruns;
        }
    }

    settings_save(app);
    app.replays.clear(); // joins the replay players; they post tasks (glfwPostEmptyEvent) too
    rest_api_stop(api, app);   // a request in flight still gets its reply
    python_shutdown(app, app.python); // before the interfaces close: the script may still send
    app_measurement_stop(app); // RX threads call glfwPostEmptyEvent: stop them before glfwTerminate
    if (smoke_frames >= 0)
    {
        std::fprintf(stderr, "smoke: %zu interfaces, %llu frames in trace\n", app.ifaces.size(),
                     static_cast<unsigned long long>(app.trace.end));
        std::fprintf(stderr, "smoke: %ld frames in %.2f s, %.1f fps, %llu rx_overruns\n", frame, loop_seconds,
                     loop_seconds > 0 ? static_cast<double>(frame) / loop_seconds : 0.0,
                     static_cast<unsigned long long>(rx_overruns));
        if (script != nullptr)
        {
            for (const PyConsoleRun& run : app.python.console)
            {
                std::fprintf(stderr, "%s%s", run.error ? "[stderr] " : "", run.text.c_str());
            }
        }
    }
    icons_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    frame_cache_wait_saved(); // a log opened just now: its cache reaches the disk, not rebuilt next time
    return 0;
}
