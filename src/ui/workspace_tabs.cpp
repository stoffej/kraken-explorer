#include "ui/workspace_tabs.h"

#include <algorithm>
#include <format>

#include <imgui_internal.h> // DockBuilder*, BeginViewportSideBar, ImHashStr
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"

namespace
{

// Trace on top of the left column, one tabbed node with Log / Generator / Message / Python under
// it, and CAN Status in full column width at the bottom so its counter columns fit. Graph on the right.
void build_default_layout(WorkspaceTab& tab, ImVec2 size)
{
    ImGui::DockBuilderRemoveNode(tab.dockspace);
    ImGui::DockBuilderAddNode(tab.dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(tab.dockspace, size);
    // Default layout: Graph is the main area (right ~60 %), everything else in the left column.
    // Graph inherits the central node, so it takes up size changes and the column keeps its width.
    // With the column central, a layout saved on a wider screen squeezes it to WindowMinSize (T58).
    ImGuiID graph = 0;
    const ImGuiID column = ImGui::DockBuilderSplitNode(tab.dockspace, ImGuiDir_Left, 0.40f, nullptr, &graph);
    ImGuiID top = 0;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(column, ImGuiDir_Down, 0.45f, nullptr, &top);
    ImGuiID mid = 0;
    const ImGuiID status = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Down, 0.5f, nullptr, &mid);
    ImGui::DockBuilderGetNode(top)->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar; // the old central widget

    const auto dock = [&](const char* title, ImGuiID node)
    { ImGui::DockBuilderDockWindow(workspace_window_name(tab, title).c_str(), node); };
    dock("Trace", top);
    dock("CAN Status", status);
    dock("Log", mid);
    dock("Generator View", mid);
    dock("Message View", mid);
    dock("Python Script", mid);
    dock("Value Search", mid);
    dock("Graph", graph);
    ImGui::DockBuilderFinish(tab.dockspace);
    tab.focus_front = 2;
    tab.column_node = column;
    tab.graph_node = graph;
}

} // namespace

WorkspaceTab& workspace_add_tab(WorkspaceTabs& ws, std::string title, unsigned uid)
{
    WorkspaceTab& tab = ws.tabs.emplace_back();
    tab.title = std::move(title);
    tab.uid = uid != 0 ? uid : ws.next_uid;
    ws.next_uid = std::max(ws.next_uid, tab.uid + 1);
    const std::string key = std::format("workspace{}v4", tab.uid); // v4: one tabbed Log/Generator node; bump to re-layout old inis
    tab.dockspace = ImHashStr(key.c_str());
    ws.current = static_cast<int>(ws.tabs.size()) - 1;
    ws.select_current = true;
    return tab;
}

std::string workspace_unique_title(const WorkspaceTabs& ws)
{
    for (std::size_t n = ws.tabs.size() + 1;; ++n)
    {
        std::string title = std::format("Tab {}", n);
        if (std::ranges::none_of(ws.tabs, [&](const WorkspaceTab& t) { return t.title == title; }))
        {
            return title;
        }
    }
}

void workspace_close_tab(App& app, unsigned uid)
{
    WorkspaceTabs& ws = app.workspace;
    const auto it = std::ranges::find(ws.tabs, uid, &WorkspaceTab::uid);
    if (ws.tabs.size() < 2 || it == ws.tabs.end())
    {
        return;
    }
    const int index = static_cast<int>(it - ws.tabs.begin());
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::DockBuilderRemoveNode(it->dockspace); // its windows are never drawn again
    }
    ws.tabs.erase(it);
    app.trace_windows.erase(uid);
    app.tx_generators.erase(uid); // joins its sender thread
    app.replays.erase(uid);
    app.lin_controls.erase(uid);
    app.instrument_panels.erase(uid);
    app.watch_windows.erase(uid);
    if (index < ws.current || ws.current >= static_cast<int>(ws.tabs.size()))
    {
        --ws.current;
    }
    ws.select_current = true;
}

std::string workspace_window_name(const WorkspaceTab& tab, const char* title)
{
    return std::format("{0}###{0}@{1}", title, tab.uid);
}

WorkspaceTab* workspace_current(WorkspaceTabs& ws)
{
    if (ws.current < 0 || ws.current >= static_cast<int>(ws.tabs.size()))
    {
        return nullptr;
    }
    return &ws.tabs[static_cast<std::size_t>(ws.current)];
}

WorkspaceTab* draw_workspace(App& app)
{
    WorkspaceTabs& ws = app.workspace;
    if (menu_take(app.menu, Command::NewTraceView) || ws.tabs.empty())
    {
        workspace_add_tab(ws, ws.tabs.empty() ? "Trace" : workspace_unique_title(ws));
    }

    const float px = ImGui::GetFontSize() / 15.0f;
    const ImVec2 pad(4.0f * px, 2.0f * px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    const float height = ImGui::GetFrameHeight() + pad.y * 2.0f;
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    // Digit 1-9 selects that tab (yazi style), read as a typed character so any layout works.
    if (ImGuiIO& io = ImGui::GetIO(); !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
    {
        const auto digit = std::find_if(io.InputQueueCharacters.begin(), io.InputQueueCharacters.end(), [&](ImWchar c)
                                        { return c >= '1' && c <= '9' && c - '1' < static_cast<int>(ws.tabs.size()); });
        if (digit != io.InputQueueCharacters.end())
        {
            ws.current = *digit - '1';
            ws.select_current = true;
            io.InputQueueCharacters.erase(digit);
        }
    }
    if (ImGui::BeginViewportSideBar("##workspace_tabs", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags))
    {
        unsigned close_uid = 0;
        bool open_rename = false;
        bool add_tab = false;
        if (ImGui::BeginTabBar("##workspaces", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
        {
            // The last tab stays. After "+" the first tab's close X lands where "+" was: no X until
            // the mouse has left the bar, so a double click on "+" can't close a tab.
            if (ws.hold_close && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            {
                ws.hold_close = false;
            }
            const bool closable = ws.tabs.size() > 1 && !ws.hold_close;
            std::vector<ImGuiID> ids(ws.tabs.size()); // ImGui's tab ids, to follow a drag reorder
            for (int i = 0; i < static_cast<int>(ws.tabs.size()); ++i)
            {
                WorkspaceTab& tab = ws.tabs[static_cast<std::size_t>(i)];
                const std::string label = std::format("{} {}###tab{}", i + 1, tab.title, tab.uid);
                const ImGuiTabItemFlags tab_flags =
                    ws.select_current && i == ws.current ? ImGuiTabItemFlags_SetSelected : 0;
                bool open = true;
                const bool selected = ImGui::BeginTabItem(label.c_str(), closable ? &open : nullptr, tab_flags);
                ids[static_cast<std::size_t>(i)] = ImGui::GetItemID();
                if (selected)
                {
                    if (!ws.select_current)
                    {
                        ws.current = i;
                    }
                    ImGui::EndTabItem();
                }
                ImGui::PushID(static_cast<int>(tab.uid));
                if (ImGui::BeginPopupContextItem("##tab_menu"))
                {
                    if (ImGui::MenuItem("Rename..."))
                    {
                        ws.rename_uid = tab.uid;
                        ws.rename_buf = tab.title;
                        open_rename = true;
                    }
                    if (ImGui::MenuItem("Close", nullptr, false, closable))
                    {
                        open = false;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (!open)
                {
                    close_uid = tab.uid;
                }
            }
            add_tab = ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
            ImGui::SetItemTooltip("New tab  %s", command_shortcut(app.menu, Command::NewTraceView));
            // A drag reorders only ImGui's tabs: apply it now and put ws.tabs in the same order, so
            // numbers, digit keys and the saved workspace follow what is shown.
            if (ImGuiTabBar* bar = ImGui::GetCurrentTabBar(); bar->ReorderRequestTabId != 0)
            {
                const bool moved = ImGui::TabBarProcessReorder(bar);
                bar->ReorderRequestTabId = 0;
                if (moved)
                {
                    const unsigned current_uid = ws.tabs[static_cast<std::size_t>(ws.current)].uid;
                    std::vector<WorkspaceTab> order;
                    for (const ImGuiTabItem& t : bar->Tabs)
                    {
                        if (const auto k = std::ranges::find(ids, t.ID); k != ids.end())
                        {
                            order.push_back(std::move(ws.tabs[static_cast<std::size_t>(k - ids.begin())]));
                        }
                    }
                    if (order.size() == ws.tabs.size()) // every tab submitted this frame
                    {
                        ws.tabs = std::move(order);
                        ws.current = static_cast<int>(std::ranges::find(ws.tabs, current_uid, &WorkspaceTab::uid) - ws.tabs.begin());
                    }
                }
            }
            ImGui::EndTabBar();
        }
        ws.select_current = false;
        if (add_tab)
        {
            ws.hold_close = true;
            workspace_add_tab(ws, workspace_unique_title(ws)); // selected on the next frame
        }
        if (open_rename)
        {
            ImGui::OpenPopup("Rename tab##workspace");
        }
        if (ImGui::BeginPopup("Rename tab##workspace"))
        {
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            const bool done = ImGui::InputText("##name", &ws.rename_buf, ImGuiInputTextFlags_EnterReturnsTrue
                                                                           | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivatedAfterEdit())
            {
                const auto it = std::ranges::find(ws.tabs, ws.rename_uid, &WorkspaceTab::uid);
                if (it != ws.tabs.end() && !ws.rename_buf.empty())
                {
                    it->title = ws.rename_buf;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (close_uid != 0)
        {
            workspace_close_tab(app, close_uid);
        }
        // Windows docked in hidden tabs stay docked only while their dockspace is kept alive.
        for (int i = 0; i < static_cast<int>(ws.tabs.size()); ++i)
        {
            if (i != ws.current)
            {
                ImGui::DockSpace(ws.tabs[static_cast<std::size_t>(i)].dockspace, ImVec2(0.0f, 0.0f),
                                 ImGuiDockNodeFlags_KeepAliveOnly);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    WorkspaceTab* tab = workspace_current(ws);
    if (tab == nullptr)
    {
        return nullptr;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!tab->layout_built)
    {
        // A layout restored from the ini or a workspace file already has the node; keep it.
        if (ImGui::DockBuilderGetNode(tab->dockspace) == nullptr)
        {
            build_default_layout(*tab, vp->WorkSize);
        }
        tab->layout_built = true;
    }
    ImGui::DockSpaceOverViewport(tab->dockspace, vp);
    // Size to fit: once CAN Status knows the width of all its columns, the left column of a fresh
    // default layout takes that width (at most 70 % of the viewport), Graph the rest.
    if (tab->column_node != 0 && app.can_status.fit_width > 0.0f)
    {
        ImGuiDockNode* column = ImGui::DockBuilderGetNode(tab->column_node);
        ImGuiDockNode* graph = ImGui::DockBuilderGetNode(tab->graph_node);
        if (column != nullptr && graph != nullptr)
        {
            const float w = std::min(app.can_status.fit_width, vp->WorkSize.x * 0.7f);
            ImGui::DockBuilderSetNodeSize(tab->column_node, ImVec2(w, column->Size.y));
            ImGui::DockBuilderSetNodeSize(tab->graph_node, ImVec2(vp->WorkSize.x - w, graph->Size.y));
        }
        tab->column_node = tab->graph_node = 0;
    }

    // New windows take focus, so the last one docked would be the front tab. Once the tab bars
    // exist (a frame later), select the screenshot's front tabs and focus Trace.
    if (tab->focus_front > 0 && --tab->focus_front == 0)
    {
        for (const char* title : {"Generator View", "CAN Status"})
        {
            const ImGuiWindow* w = ImGui::FindWindowByName(workspace_window_name(*tab, title).c_str());
            if (w != nullptr && w->DockNode != nullptr && w->DockNode->TabBar != nullptr)
            {
                w->DockNode->TabBar->NextSelectedTabId = w->TabId;
            }
        }
        ImGui::SetWindowFocus(workspace_window_name(*tab, "Trace").c_str());
    }
    return tab;
}
