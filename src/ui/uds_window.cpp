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

#include "ui/uds_window.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "ui/diag_dialog.h"
#include "ui/raw_tx.h"
#include "ui/script_window.h"
#include "ui/workspace_tabs.h"

namespace
{

constexpr auto tester_present_period = std::chrono::seconds(2);
constexpr auto scan_window = std::chrono::milliseconds(300);
constexpr std::array<int, 8> fd_lengths = {8, 12, 16, 20, 24, 32, 48, 64};

std::string ascii_of(std::span<const uint8_t> b);
std::string dtc_list_named(std::span<const uint8_t> resp, const std::map<uint32_t, std::string>& names);

std::optional<uint32_t> parse_hex_id(std::string_view s)
{
    while (!s.empty() && s.front() == ' ')
    {
        s.remove_prefix(1);
    }
    if (s.starts_with("0x") || s.starts_with("0X"))
    {
        s.remove_prefix(2);
    }
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || end != s.data() + s.size() || v > can_id_mask_extended)
    {
        return std::nullopt;
    }
    return v;
}

std::optional<uint8_t> parse_u8_hex(std::string_view s)
{
    const auto v = parse_hex_id(s);
    return v && *v <= 0xFF ? std::optional<uint8_t>(static_cast<uint8_t>(*v)) : std::nullopt;
}

int parse_int(const std::string& s, int fallback)
{
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && end == s.data() + s.size() && v >= 0 ? v : fallback;
}

// Transport (CAN FD, padding, address extension) read from the window onto a target.
void apply_transport(UdsTarget& t, const UdsWindow& w)
{
    t.fd = w.fd;
    t.brs = w.brs;
    t.tx_len = static_cast<uint8_t>(fd_lengths[std::clamp(w.tx_len_idx, 0, 7)]);
    t.padding = parse_u8_hex(w.padding).value_or(0xCC);
    t.tx_ae = w.tx_ae.empty() ? std::nullopt : parse_u8_hex(w.tx_ae);
    t.rx_ae = w.rx_ae.empty() ? std::nullopt : parse_u8_hex(w.rx_ae);
}

UdsTiming timing_of(const UdsWindow& w)
{
    return {.p2 = std::chrono::milliseconds(parse_int(w.p2, 1000)),
            .p2_star = std::chrono::milliseconds(parse_int(w.p2_star, 5000)),
            .n_bs = std::chrono::milliseconds(parse_int(w.n_bs, 1000)),
            .n_cr = std::chrono::milliseconds(parse_int(w.n_cr, 1000))};
}

void log_line(App& app, UdsWindow& w, const std::string& line)
{
    {
        const std::scoped_lock lock(w.mutex);
        if (w.log.empty())
        {
            w.log_start = std::chrono::steady_clock::now();
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - w.log_start).count();
        w.log += std::format("{:9.3f}  {}\n", t, line);
        ++w.log_total;
    }
    if (app.tasks.wake != nullptr)
    {
        app.tasks.wake();
    }
}

std::string target_text(const UdsTarget& t)
{
    return std::format("{:0{}X} -> {:0{}X}", t.tx_id, t.extended ? 8 : 3, t.rx_id, t.extended ? 8 : 3);
}

void worker_loop(std::stop_token stop, App& app, UdsWindow& w)
{
    auto next_tp = std::chrono::steady_clock::now();
    while (!stop.stop_requested())
    {
        std::function<void()> job;
        UdsTarget tp_target;
        bool tp = false;
        {
            std::unique_lock lock(w.mutex);
            w.cv.wait_until(lock, stop, next_tp, [&] { return !w.jobs.empty(); });
            if (stop.stop_requested())
            {
                return;
            }
            if (!w.jobs.empty())
            {
                job = std::move(w.jobs.front());
                w.jobs.pop_front();
                w.busy = true;
            }
            tp = w.tester_present;
            tp_target = w.target;
        }
        if (job)
        {
            job();
            const std::scoped_lock lock(w.mutex);
            w.busy = !w.jobs.empty();
        }
        if (std::chrono::steady_clock::now() >= next_tp)
        {
            next_tp = std::chrono::steady_clock::now() + tester_present_period;
            if (tp)
            {
                const std::array<uint8_t, 2> req = {0x3E, 0x80}; // no answer wanted
                (void)uds_request(app.uds_client, [&app](const BusMessage& m) { return uds_send(app, m); }, tp_target, req);
            }
        }
    }
}

void ensure_worker(App& app, UdsWindow& w)
{
    if (!w.worker.joinable())
    {
        w.worker = std::jthread([&app, &w](std::stop_token stop) { worker_loop(stop, app, w); });
    }
}

void post(App& app, UdsWindow& w, std::function<void()> job)
{
    ensure_worker(app, w);
    {
        const std::scoped_lock lock(w.mutex);
        w.jobs.push_back(std::move(job));
        w.busy = true;
    }
    w.cv.notify_all();
}

// The selected ECU, or the manual ids when they parse; transport is folded in either way.
std::optional<UdsTarget> current_target(const UdsWindow& w)
{
    UdsTarget t;
    if (w.ecu >= 0 && w.ecu < static_cast<int>(w.learner.ecus.size()))
    {
        const UdsEcu& e = w.learner.ecus[static_cast<std::size_t>(w.ecu)];
        t = {.iface = e.iface, .tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended};
    }
    else
    {
        const auto tx = parse_hex_id(w.tx_text);
        const auto rx = parse_hex_id(w.rx_text);
        if (!tx || !rx || (!w.extended && (*tx > can_id_mask_standard || *rx > can_id_mask_standard)))
        {
            return std::nullopt;
        }
        t = {.iface = w.iface, .tx_id = *tx, .rx_id = *rx, .extended = w.extended};
    }
    apply_transport(t, w);
    return t;
}

void send_request(App& app, UdsWindow& w, std::vector<uint8_t> req)
{
    const std::optional<UdsTarget> t = current_target(w);
    if (!t || req.empty())
    {
        return;
    }
    const UdsTiming timing = timing_of(w);
    auto dtc_names = w.dtc_names; // snapshot: the worker must not read the map the main thread edits
    post(app, w, [&app, &w, t = *t, req = std::move(req), timing, dtc_names = std::move(dtc_names)]
    {
        log_line(app, w, std::format("{}  >> {}  {}", target_text(t), format_hex_bytes(req), uds_describe_request(req)));
        const UdsResult r = uds_request(app.uds_client, [&app](const BusMessage& m) { return uds_send(app, m); }, t, req, timing);
        if (!r.error.empty())
        {
            log_line(app, w, "   " + r.error);
            return;
        }
        if (r.suppressed)
        {
            log_line(app, w, "   sent (no response requested)");
            return;
        }
        std::string text;
        // A DTC list with a loaded DTC text file shows the code and its description.
        if (!dtc_names.empty() && r.response.size() >= 2 && r.response[0] == 0x59
            && (r.response[1] == 0x02 || r.response[1] == 0x0A))
        {
            text = dtc_list_named(r.response, dtc_names);
        }
        else
        {
            text = uds_describe(req, r.response);
        }
        for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1))
        {
            text.insert(at + 1, 13, ' '); // DTC lists: every line under the first
        }
        log_line(app, w, std::format("   << {}\n             {}", format_hex_bytes(r.response), text));
    });
}

std::string ecu_label(const App& app, const UdsEcu& e)
{
    const std::string iface = e.iface < app.ifaces.size() ? app.ifaces[e.iface].info.name : std::to_string(e.iface);
    return std::format("{}: {}{}{}", iface, target_text({.tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended}),
                       e.guessed ? " ?" : "", e.name.empty() ? "" : "  " + e.name);
}

// Functional TesterPresent on 11 and 29 bit, then each answer's name (DID F197, else VIN F190).
void scan(App& app, UdsWindow& w, uint16_t iface)
{
    const UdsTiming timing = timing_of(w);
    UdsTarget transport;
    apply_transport(transport, w);
    post(app, w, [&app, &w, iface, transport]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, "scan: 7DF and 18DB33F1 >> 3E 00");
        std::vector<UdsEcu> found = uds_scan(app.uds_client, send, iface, scan_window, transport);
        const UdsTiming quick{.p2 = std::chrono::milliseconds(300), .p2_star = std::chrono::milliseconds(2000)};
        for (UdsEcu& e : found)
        {
            UdsTarget t{.iface = e.iface, .tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended};
            apply_transport(t, w);
            for (const uint16_t did : {0xF197, 0xF190})
            {
                const std::array<uint8_t, 3> req = {0x22, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did)};
                const UdsResult r = uds_request(app.uds_client, send, t, req, quick);
                if (r.response.size() > 3 && r.response[0] == 0x62)
                {
                    for (auto it = r.response.begin() + 3; it != r.response.end(); ++it)
                    {
                        if (*it >= 0x20 && *it < 0x7F)
                        {
                            e.name += static_cast<char>(*it);
                        }
                    }
                    break;
                }
            }
            log_line(app, w, std::format("   found {}{}{}", target_text(t), e.guessed ? " (request id guessed)" : "",
                                         e.name.empty() ? "" : "  " + e.name));
            const std::scoped_lock lock(w.mutex);
            w.found.push_back(e);
        }
        if (found.empty())
        {
            log_line(app, w, "   no ECU answered");
        }
    });
}

void run_sweep(App& app, UdsWindow& w, UdsSweep sweep)
{
    w.cancel.store(false);
    post(app, w, [&app, &w, sweep = std::move(sweep)]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, sweep.transport.extended
                             ? std::format("sweep 29-bit: 18DA<node>{:02X} >> 3E 00 ({} candidates)",
                                           sweep.tester, sweep.candidates.empty() ? 256 : sweep.candidates.size())
                             : std::format("sweep 11-bit: {:03X}..{:03X} >> 3E 00", sweep.first, sweep.last));
        const auto on_found = [&app, &w](const UdsEcu& e)
        {
            log_line(app, w, std::format("   found {}{}",
                                         target_text({.tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended}),
                                         e.guessed ? " (request id guessed)" : ""));
            const std::scoped_lock lock(w.mutex);
            w.found.push_back(e);
        };
        const auto found = uds_sweep(app.uds_client, send, sweep, on_found, &w.cancel);
        log_line(app, w, w.cancel.load() ? "   stopped" : std::format("   done, {} ECU(s)", found.size()));
    });
}

void service_scan(App& app, UdsWindow& w, UdsTarget t)
{
    w.cancel.store(false);
    const UdsTiming timing = timing_of(w);
    post(app, w, [&app, &w, t, timing]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, std::format("{}  services >>", target_text(t)));
        const auto probes = uds_service_scan(app.uds_client, send, t, timing, &w.cancel);
        std::string list;
        for (const UdsServiceProbe& p : probes)
        {
            if (p.supported())
            {
                list += (list.empty() ? "" : " ") + uds_service_name(p.sid);
            }
        }
        log_line(app, w, "   " + (list.empty() ? "none" : list));
        const std::scoped_lock lock(w.mutex);
        for (UdsEcu& e : w.learner.ecus)
        {
            if (e.iface == t.iface && e.extended == t.extended && e.rx_id == t.rx_id)
            {
                e.services = list;
            }
        }
    });
}

void did_scan(App& app, UdsWindow& w, UdsTarget t, uint16_t first, uint16_t last)
{
    w.cancel.store(false);
    const UdsTiming timing = timing_of(w);
    post(app, w, [&app, &w, t, first, last, timing]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, std::format("{}  DIDs {:04X}..{:04X} >>", target_text(t), first, last));
        const auto on_found = [&app, &w](const UdsDidValue& v)
        {
            const std::string_view nm = uds_did_name(v.did);
            log_line(app, w, std::format("   {:04X}{} = {}  \"{}\"", v.did, nm.empty() ? "" : std::format(" {}", nm),
                                         format_hex_bytes(v.data), ascii_of(v.data)));
        };
        const auto dids = uds_did_scan(app.uds_client, send, t, first, last, timing, on_found, &w.cancel);
        log_line(app, w, w.cancel.load() ? "   stopped" : std::format("   done, {} DID(s)", dids.size()));
    });
}

std::string dtc_list_named(std::span<const uint8_t> resp, const std::map<uint32_t, std::string>& names);

// Reads DTCs (19 02 FF) from every learned ECU in one sweep, logging each node's codes.
void read_all_dtcs(App& app, UdsWindow& w)
{
    w.cancel.store(false);
    std::vector<UdsEcu> ecus;
    {
        const std::scoped_lock lock(w.mutex);
        ecus = w.learner.ecus;
    }
    auto names = w.dtc_names;
    UdsTarget transport;
    apply_transport(transport, w);
    const UdsTiming timing = timing_of(w);
    post(app, w, [&app, &w, ecus = std::move(ecus), names = std::move(names), transport, timing]
    {
        const auto send = [&app](const BusMessage& m) { return uds_send(app, m); };
        log_line(app, w, std::format("read DTCs from {} node(s) >>", ecus.size()));
        const std::array<uint8_t, 3> req = {0x19, 0x02, 0xFF};
        for (const UdsEcu& e : ecus)
        {
            if (w.cancel.load())
            {
                break;
            }
            UdsTarget t = transport;
            t.iface = e.iface;
            t.tx_id = e.tx_id;
            t.rx_id = e.rx_id;
            t.extended = e.extended;
            const UdsResult r = uds_request(app.uds_client, send, t, req, timing);
            const std::string who = std::format("{}{}", target_text(t), e.name.empty() ? "" : "  " + e.name);
            if (!r.error.empty())
            {
                log_line(app, w, std::format("   {}: {}", who, r.error));
            }
            else if (r.response.size() >= 3 && r.response[0] == 0x59)
            {
                std::string text = names.empty() ? uds_describe(req, r.response) : dtc_list_named(r.response, names);
                for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1))
                {
                    text.insert(at + 1, 6, ' ');
                }
                log_line(app, w, std::format("   {}: {}", who, text));
            }
        }
        log_line(app, w, w.cancel.load() ? "   stopped" : "   done");
    });
}

// Printable form of raw bytes: non-printable bytes become '.', so a DID value never dumps control
// characters into the log.
std::string ascii_of(std::span<const uint8_t> b)
{
    std::string s;
    for (const uint8_t c : b)
    {
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
    }
    return s;
}

// Loads a DTC text file: one "code text" per line, code a hex trouble code (F00016, 0xF00016),
// the rest free text; '#' lines and blanks are skipped. Returns how many codes were read, -1 on
// a file that could not be opened.
int load_dtc_file(std::map<uint32_t, std::string>& out, const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return -1;
    }
    out.clear();
    std::string line;
    while (std::getline(in, line))
    {
        std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] == '#')
        {
            continue;
        }
        const std::size_t code_end = line.find_first_of(" \t,;", i);
        std::string code = line.substr(i, code_end - i);
        if (code.starts_with("0x") || code.starts_with("0X"))
        {
            code = code.substr(2);
        }
        uint32_t id = 0;
        const auto [p, ec] = std::from_chars(code.data(), code.data() + code.size(), id, 16);
        if (ec != std::errc{} || p != code.data() + code.size())
        {
            continue;
        }
        std::string text;
        if (code_end != std::string::npos)
        {
            const std::size_t t = line.find_first_not_of(" \t,;", code_end);
            if (t != std::string::npos)
            {
                text = line.substr(t);
                while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
                {
                    text.pop_back();
                }
            }
        }
        out[id] = text;
    }
    return static_cast<int>(out.size());
}

// A 19 02 / 19 0A DTC list with the loaded text appended per code.
std::string dtc_list_named(std::span<const uint8_t> resp, const std::map<uint32_t, std::string>& names)
{
    std::string s;
    int n = 0;
    for (std::size_t i = 3; i + 4 <= resp.size(); i += 4, ++n)
    {
        const uint32_t code = (static_cast<uint32_t>(resp[i]) << 16) | (static_cast<uint32_t>(resp[i + 1]) << 8) | resp[i + 2];
        const auto it = names.find(code);
        s += std::format("\n  {:06X} status 0x{:02X}{}", code, resp[i + 3],
                         it != names.end() ? "  " + it->second : "");
    }
    return std::format("{} DTC{}{}", n, n == 1 ? "" : "s", s);
}

// Hex text of n bytes as a big-endian number (DID, routine id).
std::optional<std::vector<uint8_t>> hex_field(const std::string& text, std::size_t n)
{
    auto b = parse_hex_bytes(text);
    if (!b || b->size() != n)
    {
        return std::nullopt;
    }
    return b;
}

std::vector<uint8_t> concat(std::initializer_list<uint8_t> head, std::span<const uint8_t> a, std::span<const uint8_t> b = {})
{
    std::vector<uint8_t> out(head);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// --- tabs ---

void draw_services_tab(App& app, UdsWindow& w, bool ok)
{
    const float em = ImGui::GetFontSize();
    ImGui::BeginDisabled(!ok);

    ImGui::SeparatorText("Session & reset");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Session");
    ImGui::SameLine();
    for (const auto& [label, sub] : {std::pair{"Default", 0x01}, {"Programming", 0x02}, {"Extended", 0x03}})
    {
        if (ImGui::Button(label))
        {
            send_request(app, w, {0x10, static_cast<uint8_t>(sub)});
        }
        ImGui::SameLine();
    }
    {
        const std::scoped_lock lock(w.mutex);
        ImGui::Checkbox("Tester present", &w.tester_present);
    }
    ImGui::SetItemTooltip("3E 80 every 2 s to the selected ECU, keeps a non-default session open");
    if (w.tester_present)
    {
        ensure_worker(app, w); // it sends them
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::Combo("##reset", &w.reset_kind, "hard\0keyOffOn\0soft\0");
    ImGui::SameLine();
    if (ImGui::Button("ECU Reset"))
    {
        send_request(app, w, {0x11, static_cast<uint8_t>(w.reset_kind + 1)});
    }
    ImGui::SameLine();
    if (ImGui::Button("Tester Present"))
    {
        send_request(app, w, {0x3E, 0x00});
    }

    ImGui::SeparatorText("Read / write data");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DID");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##did", &w.did, ImGuiInputTextFlags_CharsHexadecimal);
    const auto did = hex_field(w.did, 2);
    ImGui::SameLine();
    ImGui::BeginDisabled(!did);
    if (ImGui::Button("Read"))
    {
        send_request(app, w, concat({0x22}, *did));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 14.0f);
    ImGui::InputTextWithHint("##did_data", "data (hex)", &w.did_data);
    const auto did_data = parse_hex_bytes(w.did_data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!did_data || did_data->empty());
    if (ImGui::Button("Write"))
    {
        send_request(app, w, concat({0x2E}, *did, *did_data));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Diagnostic trouble codes");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DTC");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 12.0f);
    ImGui::Combo("##dtcreport", &w.dtc_report,
                 "count (01)\0by status (02)\0supported (0A)\0snapshot (04)\0extended (06)\0");
    ImGui::SameLine();
    if (ImGui::Button("Read DTCs"))
    {
        static constexpr std::array<uint8_t, 5> sub = {0x01, 0x02, 0x0A, 0x04, 0x06};
        const uint8_t s = sub[static_cast<std::size_t>(std::clamp(w.dtc_report, 0, 4))];
        if (s == 0x04 || s == 0x06) // need a DTC + (04) record number / (06) record 0xFF
        {
            const auto code = hex_field(w.dtc_code, 3);
            if (code)
            {
                send_request(app, w, concat({0x19, s}, *code, std::array<uint8_t, 1>{0xFF}));
            }
        }
        else
        {
            const uint8_t mask = parse_u8_hex(w.dtc_mask).value_or(0xFF);
            send_request(app, w, {0x19, s, mask});
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputText("##dtcmask", &w.dtc_mask, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetItemTooltip("status mask for 01/02/0A");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputTextWithHint("##dtccode", "DTC", &w.dtc_code, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetItemTooltip("3-byte DTC for 04/06");
    ImGui::SameLine();
    if (ImGui::Button("Clear DTCs"))
    {
        send_request(app, w, {0x14, 0xFF, 0xFF, 0xFF});
    }
    ImGui::SetItemTooltip("14 FF FF FF: all groups");

    // DTC text file: codes read by "Read DTCs" get their description from it.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DTC text");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 20.0f);
    ImGui::InputTextWithHint("##dtcfile", "path to a 'code text' file", &w.dtc_file);
    ImGui::SameLine();
    if (ImGui::Button("Load##dtc"))
    {
        const int n = load_dtc_file(w.dtc_names, w.dtc_file);
        log_line(app, w, n < 0 ? std::format("DTC file: cannot open {}", w.dtc_file)
                               : std::format("DTC file: loaded {} code(s) from {}", n, w.dtc_file));
    }
    ImGui::SetItemTooltip("One \"<hex code> <text>\" per line, e.g. \"F00016 Oil pressure low\".\n"
                          "# lines are comments. Read DTCs then shows each code's text.");
    if (!w.dtc_names.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu codes", w.dtc_names.size());
    }

    ImGui::SeparatorText("Routine & control");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Routine");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##rid", &w.routine, ImGuiInputTextFlags_CharsHexadecimal);
    const auto rid = hex_field(w.routine, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::InputTextWithHint("##rid_data", "options (hex)", &w.routine_data);
    const auto rid_data = parse_hex_bytes(w.routine_data);
    ImGui::BeginDisabled(!rid || !rid_data);
    for (const auto& [label, sub] : {std::pair{"Start", 0x01}, {"Stop", 0x02}, {"Results", 0x03}})
    {
        ImGui::SameLine();
        if (ImGui::Button(label))
        {
            send_request(app, w, concat({0x31, static_cast<uint8_t>(sub)}, *rid, *rid_data));
        }
    }
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("I/O");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##io_did", &w.io_did, ImGuiInputTextFlags_CharsHexadecimal);
    const auto io_did = hex_field(w.io_did, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::InputTextWithHint("##io_data", "control+state (hex)", &w.io_data);
    const auto io_data = parse_hex_bytes(w.io_data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!io_did);
    if (ImGui::Button("returnControl"))
    {
        send_request(app, w, concat({0x2F}, *io_did, std::array<uint8_t, 1>{0x00}));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!io_data || io_data->empty());
    if (ImGui::Button("shortTermAdjust"))
    {
        send_request(app, w, concat({0x2F}, *io_did, concat({0x03}, *io_data)));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Memory");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    ImGui::InputTextWithHint("##mem_addr", "addr", &w.mem_addr, ImGuiInputTextFlags_CharsHexadecimal);
    const auto maddr = parse_hex_bytes(w.mem_addr);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputTextWithHint("##mem_size", "size", &w.mem_size, ImGuiInputTextFlags_CharsHexadecimal);
    const auto msize = parse_hex_bytes(w.mem_size);
    ImGui::SameLine();
    ImGui::BeginDisabled(!maddr || !msize || maddr->empty() || msize->empty() || maddr->size() > 15 || msize->size() > 15);
    if (ImGui::Button("Read mem"))
    {
        // 23 <ALFID> <addr..> <size..>: ALFID high nibble = size bytes, low nibble = addr bytes.
        const uint8_t alfid = static_cast<uint8_t>((msize->size() << 4) | maddr->size());
        send_request(app, w, concat({0x23, alfid}, *maddr, *msize));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::InputTextWithHint("##mem_data", "data (hex)", &w.mem_data);
    const auto mdata = parse_hex_bytes(w.mem_data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!mdata || mdata->empty());
    if (ImGui::Button("Write mem"))
    {
        const uint8_t alfid = static_cast<uint8_t>((msize->size() << 4) | maddr->size());
        send_request(app, w, concat({0x3D, alfid}, *maddr, concat({}, *msize, *mdata)));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Security access");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Security");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputText("##sec_level", &w.sec_level, ImGuiInputTextFlags_CharsHexadecimal);
    const auto level = parse_u8_hex(w.sec_level);
    ImGui::SameLine();
    ImGui::BeginDisabled(!level);
    if (ImGui::Button("Request seed"))
    {
        send_request(app, w, {0x27, static_cast<uint8_t>(*level | 1)}); // odd sub-function requests the seed
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 12.0f);
    ImGui::InputTextWithHint("##sec_key", "key (hex)", &w.sec_key);
    const auto key = parse_hex_bytes(w.sec_key);
    ImGui::SameLine();
    ImGui::BeginDisabled(!level || !key || key->empty());
    if (ImGui::Button("Send key"))
    {
        send_request(app, w, concat({0x27, static_cast<uint8_t>((*level | 1) + 1)}, *key));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("No key algorithm is built in: compute the key from the seed yourself\n"
                          "(or in a Python script) and paste it here. Last seed is shown in the log.");

    ImGui::SeparatorText("Raw request");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Raw");
    ImGui::SameLine();
    const auto raw = parse_hex_bytes(w.raw);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Send").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##raw", "request bytes (hex), Enter sends", &w.raw, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!raw || raw->empty());
    if ((ImGui::Button("Send") || enter) && raw && !raw->empty())
    {
        send_request(app, w, *raw);
    }
    ImGui::EndDisabled();

    ImGui::EndDisabled(); // !ok
}

void draw_scan_tab(App& app, UdsWindow& w, bool busy)
{
    const float em = ImGui::GetFontSize();
    const uint16_t iface = w.ecu >= 0 && w.ecu < static_cast<int>(w.learner.ecus.size())
        ? w.learner.ecus[static_cast<std::size_t>(w.ecu)].iface : w.iface;

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Functional"))
    {
        scan(app, w, iface);
    }
    ImGui::SetItemTooltip("TesterPresent to 0x7DF and 0x18DB33F1; every ECU that answers is added and named");
    ImGui::SameLine();
    if (ImGui::Button("Sweep 11-bit"))
    {
        UdsSweep s;
        s.transport = {.iface = iface};
        apply_transport(s.transport, w);
        s.first = parse_hex_id(w.sweep_first).value_or(0x700);
        s.last = parse_hex_id(w.sweep_last).value_or(0x7FF);
        s.wait = std::chrono::milliseconds(parse_int(w.sweep_wait, 40));
        run_sweep(app, w, std::move(s));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##swfirst", &w.sweep_first, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::TextUnformatted("..");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##swlast", &w.sweep_last, ImGuiInputTextFlags_CharsHexadecimal);

    if (ImGui::Button("Sweep 29-bit"))
    {
        UdsSweep s;
        s.transport = {.iface = iface, .extended = true};
        apply_transport(s.transport, w);
        s.tester = parse_u8_hex(w.sweep_tester).value_or(0xF1);
        s.wait = std::chrono::milliseconds(parse_int(w.sweep_wait, 40));
        if (w.sweep_j1939)
        {
            s.candidates = w.learner.j1939_sources; // the source addresses seen on the bus
        }
        run_sweep(app, w, std::move(s));
    }
    ImGui::SetItemTooltip("Requests 18DA<node><tester> for each candidate node address");
    ImGui::SameLine();
    ImGui::TextUnformatted("tester");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputText("##swtester", &w.sweep_tester, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::Checkbox("J1939 nodes only", &w.sweep_j1939);
    ImGui::SetItemTooltip(w.learner.j1939_sources.empty()
                              ? "No 29-bit source addresses seen yet; off = sweep all 256"
                              : "Sweep only the source addresses seen in the 29-bit traffic; off = all 256");

    ImGui::SameLine();
    ImGui::TextUnformatted("wait");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##swwait", &w.sweep_wait);
    ImGui::SetItemTooltip("milliseconds to listen after each request");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Stop"))
    {
        w.cancel.store(true);
    }
    ImGui::EndDisabled();

    // DID sweep on the selected ECU.
    const std::optional<UdsTarget> t = current_target(w);
    ImGui::BeginDisabled(busy || !t);
    if (ImGui::Button("Scan DIDs"))
    {
        did_scan(app, w, *t, parse_hex_id(w.did_first).value_or(0xF180), parse_hex_id(w.did_last).value_or(0xF1FF));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##didfirst", &w.did_first, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::TextUnformatted("..");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.0f);
    ImGui::InputText("##didlast", &w.did_last, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetItemTooltip("ReadDataByIdentifier for each DID of the range on the selected ECU");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(busy || w.learner.ecus.empty());
    if (ImGui::Button("Read DTCs (all nodes)"))
    {
        read_all_dtcs(app, w);
    }
    ImGui::SetItemTooltip("19 02 FF to every node in the table below; uses the loaded DTC text file for names");
    ImGui::EndDisabled();

    ImGui::Separator();

    // The learned / found ECU table: name, services, select.
    if (ImGui::BeginTable("##ecus", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, em * 9.0f)))
    {
        ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
        ImGui::TableSetupColumn("Request -> Response", ImGuiTableColumnFlags_WidthFixed, em * 11.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Services", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < w.learner.ecus.size(); ++i)
        {
            UdsEcu& e = w.learner.ecus[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted((e.iface < app.ifaces.size() ? app.ifaces[e.iface].info.name
                                                                : std::to_string(e.iface)).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::format("{}{}", target_text({.tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended}),
                                              e.guessed ? " ?" : "").c_str(),
                                  w.ecu == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
            {
                w.ecu = static_cast<int>(i);
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &e.name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.services.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Services"))
            {
                UdsTarget t2{.iface = e.iface, .tx_id = e.tx_id, .rx_id = e.rx_id, .extended = e.extended};
                apply_transport(t2, w);
                service_scan(app, w, t2);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void draw_transport_tab(UdsWindow& w)
{
    const float em = ImGui::GetFontSize();
    ImGui::Checkbox("CAN FD", &w.fd);
    ImGui::SameLine();
    ImGui::BeginDisabled(!w.fd);
    ImGui::Checkbox("BRS", &w.brs);
    ImGui::SameLine();
    ImGui::TextUnformatted("Max frame");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 6.0f);
    static constexpr const char* len_items[] = {"8", "12", "16", "20", "24", "32", "48", "64"};
    ImGui::Combo("##txlen", &w.tx_len_idx, len_items, IM_ARRAYSIZE(len_items));
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputText("Padding byte", &w.padding, ImGuiInputTextFlags_CharsHexadecimal);

    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputTextWithHint("Tx address ext.", "off", &w.tx_ae, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 3.0f);
    ImGui::InputTextWithHint("Rx address ext.", "off", &w.rx_ae, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetItemTooltip("Extended / mixed addressing: a leading address byte on every frame.\n"
                          "Leave empty for normal (11-bit) or normal fixed (29-bit) addressing.");

    ImGui::SeparatorText("Timing (ms)");
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputText("P2", &w.p2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputText("P2*", &w.p2_star);
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputText("N_Bs", &w.n_bs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::InputText("N_Cr", &w.n_cr);
}

} // namespace

bool uds_send(App& app, const BusMessage& m)
{
    return m.iface < app.ifaces.size() && iface_send(app.ifaces[m.iface], m);
}

void uds_window_on_rx(UdsWindow& w, std::span<const BusMessage> batch)
{
    for (const BusMessage& m : batch)
    {
        (void)uds_learn(w.learner, m);
    }
}

void uds_window_shutdown(App& app, UdsWindow& w)
{
    w.cancel.store(true);
    uds_client_cancel(app.uds_client);
    w.worker = {}; // request_stop + join
}

void draw_uds_window(App& app, UdsWindow& w, const WorkspaceTab& tab)
{
    // Scan results and the log come from the worker.
    bool changed = false;
    bool busy = false;
    {
        const std::scoped_lock lock(w.mutex);
        for (const UdsEcu& e : w.found)
        {
            uds_learner_add(w.learner, e);
        }
        w.found.clear();
        if (w.log_total != w.log_seen)
        {
            w.log_seen = w.log_total;
            w.log_view = w.log;
            changed = true;
        }
        busy = w.busy;
    }
    const std::optional<UdsTarget> target = current_target(w);
    if (target)
    {
        const std::scoped_lock lock(w.mutex);
        w.target = *target; // tester present follows the selection
    }
    if (!w.open)
    {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 50.0f, em * 38.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(workspace_window_name(tab, "UDS Diagnostics").c_str(), &w.open))
    {
        ImGui::End();
        return;
    }

    // --- Target header (shared by every tab) ---
    const std::string preview = w.ecu >= 0 && w.ecu < static_cast<int>(w.learner.ecus.size())
        ? ecu_label(app, w.learner.ecus[static_cast<std::size_t>(w.ecu)]) : std::string("Manual");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ECU");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 22.0f);
    if (ImGui::BeginCombo("##ecu", preview.c_str()))
    {
        if (ImGui::Selectable("Manual", w.ecu < 0))
        {
            w.ecu = -1;
        }
        for (std::size_t i = 0; i < w.learner.ecus.size(); ++i)
        {
            const std::string label = ecu_label(app, w.learner.ecus[i]) + std::format("##{}", i);
            if (ImGui::Selectable(label.c_str(), w.ecu == static_cast<int>(i)))
            {
                w.ecu = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("ECUs learned from the traffic (request/response pairs) and found by Scan.\n"
                          "? marks a request id guessed as response id - 8.");
    if (w.ecu < 0)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 8.0f);
        draw_iface_combo("##iface", app, w.iface, BusType::CAN);
        ImGui::SameLine();
        ImGui::TextUnformatted("Req");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5.0f);
        ImGui::InputText("##tx", &w.tx_text, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::TextUnformatted("Resp");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5.0f);
        ImGui::InputText("##rx", &w.rx_text, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::Checkbox("29-bit", &w.extended);
    }
    const bool ok = target.has_value();
    if (!ok)
    {
        ImGui::TextDisabled("Request and response ids must be hex (11-bit up to 7FF unless 29-bit)");
    }

    // The tabs live in a scrolling child so the log below always keeps at least a third of the
    // window (never less than ~9 lines): a tall Services tab must not push the log out of sight.
    const float avail = ImGui::GetContentRegionAvail().y;
    const float log_h = std::max(em * 9.0f, avail * 0.35f);
    const float tabs_h = std::max(em * 6.0f, avail - log_h - ImGui::GetFrameHeightWithSpacing() * 2.0f);
    if (ImGui::BeginChild("##uds_tabs_area", ImVec2(0.0f, tabs_h), ImGuiChildFlags_None, ImGuiWindowFlags_None))
    {
        if (ImGui::BeginTabBar("##uds_tabs"))
        {
            if (ImGui::BeginTabItem("Services"))
            {
                w.tab = 0;
                draw_services_tab(app, w, ok);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scan"))
            {
                w.tab = 1;
                draw_scan_tab(app, w, busy);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Transport"))
            {
                w.tab = 2;
                draw_transport_tab(w);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Clear log"))
    {
        const std::scoped_lock lock(w.mutex);
        w.log.clear();
        ++w.log_total;
    }
    if (busy)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("waiting for the ECU...");
    }

    ImGui::PushFont(app.fonts.mono, 0.0f);
    draw_console_box("##log", w.log_view, -FLT_MIN, -FLT_MIN, changed, w.log_follow, w.log_active);
    ImGui::PopFont();
    ImGui::End();
}
