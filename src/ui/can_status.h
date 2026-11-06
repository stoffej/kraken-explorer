#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "drivers/driver.h"
#include "ui/vim_nav.h"

struct App;
struct Setup;
struct SetupInterface;
struct WorkspaceTab;

// Last values of one interface; kept after the measurement stops, like the old tree rows.
struct CanStatusRow
{
    IfaceStats stats;  // since the interface was (re)opened: the cumulative counters minus base
    IfaceStats base;   // the driver's counters at the first poll after the (re)open
    uint64_t bits = 0;
    uint64_t load_bits = 0; // total_bits at load_time
    std::chrono::steady_clock::time_point load_time{};
    std::string load = "0.0%";
    uint64_t error_frames = 0; // frames with BusMessage::errors != 0 seen in the trace this measurement
    bool seen = false; // was open during the last measurement
};

// Sortable columns of the CAN Status table (ImGuiTableColumn user id).
enum class CanStatusSort : uint8_t
{
    Interface,
    State,
    RxFrames,
    RxErrors,
    TxFrames,
    TxErrors,
};

// The docked "CAN Status" window (old CanStatusWindow): per-interface counters and bus load.
struct CanStatusState
{
    std::vector<CanStatusRow> rows; // index = Iface::index
    bool was_measuring = false;
    uint64_t trace_done = 0; // Trace::end up to which error frames are counted
    std::chrono::steady_clock::time_point polled{};
    // Link buttons (SocketCAN up/down, vcan add/delete): `ip` runs here, pkexec may prompt.
    std::jthread link_worker;
    bool link_busy = false;                 // a command is running; buttons disabled
    std::vector<char> link_exists;          // index = Iface::index; false once the link is deleted
    std::chrono::steady_clock::time_point link_polled{};
    std::string link_error; // last failed command, shown until the next one succeeds
    std::string autobaud_iface;  // last auto-baud: interface and "Auto-baud...", "250 kbit/s", "no match" or an ip error
    std::string autobaud_result;
    std::vector<std::string> bitrate; // index = Iface::index: "500k", "500k / 2M", "\u2014" (vcan, unknown); with the link refresh
    std::vector<uint16_t> order;      // Iface indices in display order, rebuilt by can_status_sort
    CanStatusSort sort_key = CanStatusSort::Interface;
    bool sort_desc = false;
    bool sort_dirty = true; // sort specs, polled counters or links changed: re-sort before drawing
    int selected = -1;      // Iface::index of the selected row, -1 = none
    float fit_width = 0.0f; // width the table needs to show every column; the default layout sizes the left column by it
    VimNav vim;
    std::vector<VimYankItem> yank_items; // vim_yank_menu: [0] whole row, then one per column
};

// Rebuilds s.order from ifaces by s.sort_key / s.sort_desc (ties: Iface::index); interfaces
// without a polled row sort as zero counters / unknown state.
void can_status_sort(CanStatusState& s, const std::deque<Iface>& ifaces);

// Up/Down (and Delete for vcan) for a SocketCAN interface; nothing for other drivers.
void draw_link_buttons(App& app, const Iface& iface);

// "New vcan" button: creates the next free vcanN.
void draw_new_vcan_button(App& app);

// Status text for a failed `ip` call ("" for ok); the link buttons and REST /vcan show it.
[[nodiscard]] std::string ip_error_text(IpResult r, const std::string& name);
// The interface's entry in the setup (driver + name), nullptr when it is in no network.
[[nodiscard]] SetupInterface* setup_of(Setup& setup, const Iface& iface);

void draw_can_status(App& app, CanStatusState& s, const WorkspaceTab& tab);
