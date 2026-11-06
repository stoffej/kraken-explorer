#pragma once

#include <string>
#include <vector>

#include "ui/graph.h"

struct App;

// One workspace tab (the old QMainWindow page in mainTabs): its own dockspace and window set.
struct WorkspaceTab
{
    std::string title;
    unsigned uid = 0;          // stable per tab, keys the dockspace id and the window names
    unsigned dockspace = 0;    // ImGuiID; app.h is included by targets without imgui
    bool layout_built = false; // default layout applied once; ImGui keeps it from then on
    int focus_front = 0;       // frames until the default front tabs are focused (0 = done)
    unsigned column_node = 0;  // default layout: the left column / Graph nodes, sized once by CAN Status' fit width
    unsigned graph_node = 0;
    std::vector<GraphState> graphs; // the docked "Graph" (id 0) plus New Graph View windows
};

// The tab bar at the bottom of the main window, with the version text on its right.
struct WorkspaceTabs
{
    std::vector<WorkspaceTab> tabs;
    int current = 0;     // index into tabs; windows of other tabs are not drawn
    bool select_current = false; // force the tab bar onto `current` next frame (new tab)
    bool hold_close = false;     // no close buttons until the mouse leaves the bar (after "+")
    unsigned next_uid = 1;
    unsigned rename_uid = 0; // tab being renamed in the "Rename tab" popup (0 = none)
    std::string rename_buf;
};

// Adds a tab with the default Trace layout (createTraceWindow) and makes it current.
// uid 0 takes the next free one; settings/workspace loading pass the saved uid.
WorkspaceTab& workspace_add_tab(WorkspaceTabs& ws, std::string title = "Trace", unsigned uid = 0);

// First free "Tab N" (N from tabs.size() + 1) for a new tab.
[[nodiscard]] std::string workspace_unique_title(const WorkspaceTabs& ws);

// Closes the tab with this uid unless it is the last one, and drops its per-tab window state
// (trace window, generator, replay, LIN control, instrument panel) and its dock nodes.
void workspace_close_tab(App& app, unsigned uid);

// Name of a docked window in a tab: "<title>###<title>@<uid>", unique per tab.
// Windows docked by the default layout: Trace, CAN Status, Log, Generator View,
// Message View, Graph, Python Script.
[[nodiscard]] std::string workspace_window_name(const WorkspaceTab& tab, const char* title);

// Current tab or nullptr when there is none.
[[nodiscard]] WorkspaceTab* workspace_current(WorkspaceTabs& ws);

// The tabbed node extra windows dock into (Value Search's: Log, Python Script, ...), the tab's
// dockspace when that floats. Dock Floating Windows and a new Replay use it.
[[nodiscard]] unsigned workspace_dock_target(const WorkspaceTab& tab);

// Draws the bottom tab bar and the current tab's dockspace; keeps the other tabs' dockspaces
// alive. Call after draw_main_menu and before the tab's windows. Returns the current tab.
WorkspaceTab* draw_workspace(App& app);
