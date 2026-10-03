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

#include "core/python_engine.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <utility>

#include "app.h"
#include "core/autosar_e2e.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/trace_file_format.h"
#include "core/trace_file_writer.h"

namespace py = pybind11;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// LeakSanitizer reads this at start-up (ignored without sanitizers): CPython keeps interned
// objects and module state alive past Py_Finalize. Linked into every binary that embeds Python.
extern "C" const char* __lsan_default_suppressions()
{
    return "leak:libpython\nleak:_PyObject_\nleak:PyObject_\nleak:_PyMem_\nleak:PyMem_\n";
}

namespace
{

constexpr std::size_t console_max_bytes = 1 << 20;
constexpr std::size_t rx_queue_max = 10000;

// One interpreter, one engine: the module functions reach the App through these.
App* g_app = nullptr;
PyState* g_state = nullptr;

// Runs fn on the main thread and waits, with the GIL released so the main thread (and other
// Python threads) can run meanwhile. Call with the GIL held.
void on_main(const std::function<void(App&)>& fn)
{
    py::gil_scoped_release release;
    tasks_run_on_main_blocking(g_app->tasks, *g_app, [&fn](App& a) { fn(a); });
}

// Refreshes the worker-side copy of the setup and the interface names, at most once a
// second. The databases are shared with the main thread's setup: a DBC reload while a
// script runs is the same benign race as before the port.
// ponytail: time based, since Setup::generation is not readable off the main thread. Atomic generation if the interval shows.
void refresh_snapshot(PyState& s)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - s.snapshot_time < 1s)
    {
        return;
    }
    Setup setup;
    std::vector<std::string> names;
    on_main([&](App& a)
    {
        setup = a.setup;
        for (const Iface& i : a.ifaces)
        {
            names.push_back(i.info.name);
        }
    });
    // Back with the GIL: nothing else reads the snapshot meanwhile.
    s.setup = std::move(setup);
    s.iface_names = std::move(names);
    s.snapshot_time = now;
}

// No interface to go by: searches every network, first match wins. An int matches the
// id with or without the extended bit (bit 31 of the DBC raw id).
const CanDbMessage* find_can_message(const Setup& setup, const py::object& name_or_id)
{
    const bool by_name = py::isinstance<py::str>(name_or_id);
    const std::string name = by_name ? name_or_id.cast<std::string>() : std::string{};
    const uint32_t id = by_name ? 0 : name_or_id.cast<uint32_t>() & can_id_mask_extended;
    for (const SetupNetwork& net : setup.networks)
    {
        for (const CanDbMessage* m : net.can_messages | std::views::values)
        {
            if (by_name ? m->name == name : (m->raw_id & can_id_mask_extended) == id)
            {
                return m;
            }
        }
    }
    return nullptr;
}

// The signal-definition dict used by lookup() and find_message().
py::dict message_dict(const CanDbMessage& msg)
{
    py::list sigs;
    for (const CanDbSignal& sig : msg.signals)
    {
        py::dict s;
        s["name"] = sig.name;
        s["start_bit"] = sig.start_bit;
        s["length"] = sig.length;
        s["is_big_endian"] = sig.big_endian;
        s["is_unsigned"] = sig.is_unsigned;
        s["factor"] = sig.factor;
        s["offset"] = sig.offset;
        s["min"] = sig.min;
        s["max"] = sig.max;
        s["unit"] = sig.unit;
        s["comment"] = sig.comment;
        if (sig.is_muxed)
        {
            s["mux_value"] = sig.mux_value;
        }
        if (sig.is_muxer)
        {
            s["is_muxer"] = true;
        }
        sigs.append(s);
    }
    py::dict result;
    result["message"] = msg.name;
    result["id"] = msg.raw_id;
    result["dlc"] = msg.dlc;
    result["comment"] = msg.comment;
    result["signals"] = sigs;
    if (!msg.sender.empty())
    {
        result["sender"] = msg.sender;
    }
    return result;
}

py::dict lin_frame_dict(const LinFrame& frame)
{
    py::list sigs;
    for (const LinSignal& sig : frame.signals)
    {
        py::dict s;
        s["name"] = sig.name;
        s["bit_offset"] = sig.bit_offset;
        s["bit_length"] = sig.bit_length;
        s["factor"] = sig.factor;
        s["offset"] = sig.offset;
        s["min"] = sig.min;
        s["max"] = sig.max;
        s["unit"] = sig.unit;
        s["publisher"] = sig.publisher;
        s["init_value"] = sig.init_value;
        sigs.append(s);
    }
    py::dict d;
    d["id"] = frame.id;
    d["name"] = frame.name;
    d["publisher"] = frame.publisher;
    d["length"] = frame.length;
    d["signals"] = sigs;
    return d;
}

std::string format_names()
{
    std::string out;
    for (const TraceFileFormat format : trace_file_formats)
    {
        out += out.empty() ? "" : ", ";
        out += trace_format_name(format);
    }
    return out;
}

// kraken.send / send_periodic / lin_* without interface_id: the first open interface (the
// measurement's; ids of interfaces outside the setup come first and are never open), LIN calls the
// first open LIN one; else 0.
uint16_t send_iface(std::optional<uint16_t> interface_id, bool lin = false)
{
    if (interface_id)
    {
        return *interface_id;
    }
    py::gil_scoped_release release;
    for (Iface& i : g_app->ifaces)
    {
        std::shared_lock lock(i.io_mutex);
        if (i.open && (!lin || i.info.bus_type == BusType::LIN))
        {
            return i.index;
        }
    }
    return 0;
}

// LIN control calls follow the iface_send contract: shared io lock, only while open.
template <class Fn>
void with_open_iface(uint16_t interface_id, Fn fn)
{
    if (interface_id >= g_app->ifaces.size())
    {
        return;
    }
    Iface& iface = g_app->ifaces[interface_id];
    py::gil_scoped_release release;
    std::shared_lock lock(iface.io_mutex);
    if (iface.open)
    {
        fn(iface);
    }
}

std::string read_input_line(PyState& s)
{
    std::unique_lock lock(s.mutex);
    s.input_cv.wait(lock, [&s] { return !s.input_queue.empty() || s.stop_requested; });
    if (s.stop_requested)
    {
        return {};
    }
    std::string line = std::move(s.input_queue.front());
    s.input_queue.pop_front();
    return line;
}

void stop_all_periodic(PyState& s)
{
    std::map<int, std::jthread> tasks;
    {
        std::scoped_lock lock(s.periodic_mutex);
        tasks.swap(s.periodic);
    }
    for (auto& [handle, thread] : tasks)
    {
        thread.request_stop();
    }
    tasks.clear(); // joins
}

// A Python standard library shipped next to the executable (<app>/lib/pythonX.Y, or
// <prefix>/lib/pythonX.Y with the binary in <prefix>/bin as in an AppImage) wins over
// the compiled-in prefix. Distro installs have none and keep libpython's own.
void set_bundled_python_home()
{
    if (const char* home = std::getenv("PYTHONHOME"); home != nullptr && *home != '\0')
    {
        return;
    }
#ifndef _WIN32 // there Python finds a runtime shipped next to the executable (python3xx.dll) by itself
    std::error_code ec;
    const fs::path exe = platform_exe_path();
    if (exe.empty())
    {
        return;
    }
    const fs::path landmark = fs::path("lib") / std::format("python{}.{}", PY_MAJOR_VERSION, PY_MINOR_VERSION) / "os.py";
    for (const fs::path prefix : {exe.parent_path(), exe.parent_path().parent_path()})
    {
        if (fs::exists(prefix / landmark, ec))
        {
            setenv("PYTHONHOME", prefix.c_str(), 1);
            return;
        }
    }
#endif
}

constexpr const char* prelude = R"(
import io
import sys

class _SignalWriter:
    def __init__(self, is_err):
        self._is_err = is_err
    def write(self, text):
        if text:
            _kraken_output(text, self._is_err)
    def flush(self):
        pass

sys.stdout = _SignalWriter(False)
sys.stderr = _SignalWriter(True)

class _SignalReader(io.TextIOBase):
    def __init__(self):
        super().__init__()
        self._buffer = ""
    def readline(self, size=-1):
        if not self._buffer:
            self._buffer = _kraken_input()
            if not self._buffer and _kraken_stop_check():
                raise KeyboardInterrupt("Script stopped by user")
        if size is None or size < 0:
            line, self._buffer = self._buffer, ""
            return line
        line, self._buffer = self._buffer[:size], self._buffer[size:]
        return line
    def readable(self):
        return True
    def isatty(self):
        return True

sys.stdin = _SignalReader()

import threading as _threading

def _kraken_trace(frame, event, arg):
    if _kraken_stop_check():
        raise KeyboardInterrupt("Script stopped by user")
    return _kraken_trace

sys.settrace(_kraken_trace)
_threading.settrace(_kraken_trace)

# time.sleep is a C call the trace hook can't interrupt: wait on the engine instead, so Stop and
# Exit end a sleeping script (and its threads) at once.
import time as _time
def _kraken_time_sleep(secs):
    if secs < 0:
        raise ValueError("sleep length must be non-negative")
    if _kraken_sleep(secs):
        raise KeyboardInterrupt("Script stopped by user")
_time.sleep = _kraken_time_sleep

# Stop raises KeyboardInterrupt in the script's threads too: end them quietly.
_kraken_excepthook_orig = _threading.excepthook
def _kraken_excepthook(args):
    if not issubclass(args.exc_type, KeyboardInterrupt):
        _kraken_excepthook_orig(args)
_threading.excepthook = _kraken_excepthook
)";

void worker_main(PyState& s, App& app, std::string code, std::string name)
{
    const PyGILState_STATE gstate = PyGILState_Ensure();
    try
    {
        py::dict globals = py::globals();
        globals["_kraken_output"] = py::cpp_function(
            [&s](const std::string& text, bool is_err) { python_console_append(s, text, is_err); });
        globals["_kraken_stop_check"] = py::cpp_function([&s]() -> bool { return s.stop_requested.load(); });
        globals["_kraken_input"] = py::cpp_function([&s]() -> std::string { return read_input_line(s); },
                                                      py::call_guard<py::gil_scoped_release>());
        globals["_kraken_sleep"] = py::cpp_function([&s](double secs) -> bool
        {
            std::unique_lock lock(s.mutex);
            s.rx_cv.wait_for(lock, std::chrono::duration<double>(secs), [&s] { return s.stop_requested.load(); });
            return s.stop_requested.load();
        }, py::call_guard<py::gil_scoped_release>());
        py::exec(prelude);
        try
        {
            const py::module_ builtins = py::module_::import("builtins");
            builtins.attr("exec")(builtins.attr("compile")(code, name, "exec"), globals);
        }
        catch (py::error_already_set& e)
        {
            const std::string what = e.what();
            if (what.find("KeyboardInterrupt") == std::string::npos)
            {
                python_console_append(s, what + "\n", true);
            }
        }
        // Threads the script started (daemon ones too) stop at their next traced line: wait
        // for them, or a sleeping one would wake into the next run once stop_requested clears.
        python_stop(s); // also wakes threads sleeping in time.sleep
        PyEval_SetTrace(nullptr, nullptr); // this thread's own lines must not raise now
        const py::object script_threads = py::eval(
            "lambda: [t for t in _threading.enumerate()"
            " if t is not _threading.main_thread() and not isinstance(t, _threading._DummyThread)]");
        // ponytail: a thread stuck in another long C call (socket recv) is left behind after 2 s. PyThreadState_SetAsyncExc if a leaked thread matters.
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (py::len(script_threads()) > 0 && std::chrono::steady_clock::now() < deadline)
        {
            const py::gil_scoped_release release;
            std::this_thread::sleep_for(10ms);
        }
    }
    catch (const std::exception& e)
    {
        python_console_append(s, std::string(e.what()) + "\n", true);
    }
    PyGILState_Release(gstate);

    s.stop_requested = true; // also when the prelude failed
    stop_all_periodic(s);
    s.running = false;
    if (app.tasks.wake != nullptr)
    {
        app.tasks.wake(); // repaint, so the window shows the end of the run at once
    }
}

// Python property pair for one bus_flag bit (extended / fd / rtr / brs).
auto flag_getter(uint16_t flag)
{
    return [flag](const BusMessage& msg) { return has_flag(msg, flag); };
}

auto flag_setter(uint16_t flag)
{
    return [flag](BusMessage& msg, bool on) { msg.flags = on ? msg.flags | flag : msg.flags & ~flag; };
}

// Payload length for Message.dlc / set_data: a CAN FD length (0-8, 12, 16, ... 64), LIN 0-8.
// Anything else raises instead of going out as an invalid frame.
int checked_length(long long len, bool lin)
{
    if (len < 0 || (lin ? len > 8 : std::ranges::find(bus_dlc_lengths, len) == bus_dlc_lengths.end()))
    {
        throw py::value_error(lin ? std::format("{} bytes: a LIN frame has 0-8", len)
                                  : std::format("{} bytes is not a CAN FD length (0-8, 12, 16, 20, 24, 32, 48, 64)", len));
    }
    return static_cast<int>(len);
}

} // namespace

// ---------------------------------------------------------------------------
// The embedded "kraken" module. Every function runs on a script thread with the GIL.
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE(kraken, m)
{
    py::class_<BusMessage>(m, "Message")
        .def(py::init<>())
        .def(py::init([](uint32_t id)
        {
            BusMessage msg;
            msg.id = id;
            return msg;
        }))
        .def_property("id", [](const BusMessage& msg) { return msg.id; },
                      [](BusMessage& msg, uint32_t id) { msg.id = id; })
        .def_property("dlc", [](const BusMessage& msg) { return msg.len; },
                      [](BusMessage& msg, int len) { set_length(msg, checked_length(len, msg.type == BusType::LIN)); })
        .def_property("extended", flag_getter(bus_flag::extended), flag_setter(bus_flag::extended))
        .def_property("fd", flag_getter(bus_flag::fd), flag_setter(bus_flag::fd))
        .def_property("rtr", flag_getter(bus_flag::rtr), flag_setter(bus_flag::rtr))
        .def_property("brs", flag_getter(bus_flag::brs), flag_setter(bus_flag::brs))
        .def_property_readonly("interface_id", [](const BusMessage& msg) { return msg.iface; })
        .def_property_readonly("timestamp", [](const BusMessage& msg) { return static_cast<double>(msg.ts_ns) / 1e9; })
        .def_property_readonly("is_rx", [](const BusMessage& msg) { return !has_flag(msg, bus_flag::tx); })
        .def_property_readonly("is_lin_sleep", [](const BusMessage& msg) { return has_flag(msg, bus_flag::lin_sleep); })
        .def_property_readonly("is_lin_wakeup", [](const BusMessage& msg) { return has_flag(msg, bus_flag::lin_wakeup); })
        .def("get_byte", [](const BusMessage& msg, int i) -> uint8_t
        {
            return i >= 0 && i < bus_max_data_bytes ? msg.data[static_cast<std::size_t>(i)] : 0;
        })
        .def("set_byte", [](BusMessage& msg, int i, uint8_t value)
        {
            if (i >= 0 && i < bus_max_data_bytes)
            {
                msg.data[static_cast<std::size_t>(i)] = value;
            }
        })
        .def("get_data", [](const BusMessage& msg)
        {
            return py::bytes(reinterpret_cast<const char*>(msg.data.data()), msg.len);
        })
        .def("set_data", [](BusMessage& msg, const py::bytes& data)
        {
            const std::string s = data;
            const int n = checked_length(static_cast<long long>(s.size()), msg.type == BusType::LIN);
            set_length(msg, n);
            msg.data.fill(0);
            std::copy_n(s.data(), n, reinterpret_cast<char*>(msg.data.data()));
        })
        .def_property("bustype",
            [](const BusMessage& msg) -> std::string { return msg.type == BusType::LIN ? "LIN" : "CAN"; },
            [](BusMessage& msg, const std::string& s) { msg.type = s == "LIN" ? BusType::LIN : BusType::CAN; })
        .def("__repr__", [](const BusMessage& msg)
        {
            std::string r = msg.type == BusType::LIN ? "<kraken.LinMessage id=" : "<kraken.Message id=";
            append_id(r, msg);
            r += std::format(" dlc={} data=", msg.len);
            append_bytes(r, std::span<const uint8_t>(msg.data.data(), msg.len), false);
            r += ">";
            return r;
        });

    m.def("make_lin_message", [](uint8_t id, uint8_t dlc)
    {
        BusMessage msg{.id = id, .type = BusType::LIN};
        set_length(msg, checked_length(dlc, true));
        return msg;
    }, py::arg("id"), py::arg("dlc") = 0);

    // --- send / receive ---

    m.def("lin_sleep", [](std::optional<uint16_t> interface_id)
    {
        with_open_iface(send_iface(interface_id, true), [](Iface& i)
        {
            if (i.ops->lin_sleep_wakeup != nullptr) i.ops->lin_sleep_wakeup(i, false);
        });
    }, py::arg("interface_id") = py::none());

    m.def("lin_wakeup", [](std::optional<uint16_t> interface_id)
    {
        with_open_iface(send_iface(interface_id, true), [](Iface& i)
        {
            if (i.ops->lin_sleep_wakeup != nullptr) i.ops->lin_sleep_wakeup(i, true);
        });
    }, py::arg("interface_id") = py::none());

    m.def("lin_set_schedule_table", [](uint8_t table_index, std::optional<uint16_t> interface_id)
    {
        with_open_iface(send_iface(interface_id, true), [table_index](Iface& i)
        {
            if (i.ops->lin_set_schedule != nullptr) i.ops->lin_set_schedule(i, table_index);
        });
    }, py::arg("table_index"), py::arg("interface_id") = py::none());

    m.def("send", [](BusMessage& msg, std::optional<uint16_t> iface_arg)
    {
        const uint16_t interface_id = send_iface(iface_arg);
        if (interface_id >= g_app->ifaces.size())
        {
            throw py::value_error(std::format("no interface with id {}", interface_id));
        }
        msg.iface = interface_id;
        const BusMessage copy = msg;
        bool sent = false;
        {
            py::gil_scoped_release release;
            sent = iface_send(g_app->ifaces[interface_id], copy);
        }
        if (!sent)
        {
            throw std::runtime_error(std::format("interface {} not open or send failed", interface_id));
        }
    }, "Send a Message on interface `interface_id` (see interfaces(); default: the first interface of "
       "the running measurement). Raises ValueError for "
       "an unknown id and RuntimeError when the interface is not open (no measurement "
       "running) or the driver rejects the frame.",
       py::arg("msg"), py::arg("interface_id") = py::none());

    m.def("receive", [](double timeout_sec)
    {
        PyState& s = *g_state;
        std::vector<BusMessage> got;
        {
            py::gil_scoped_release release; // an RX thread may be waiting for s.mutex
            std::unique_lock lock(s.mutex);
            s.rx_cv.wait_for(lock, std::chrono::duration<double>(timeout_sec),
                             [&s] { return !s.rx_queue.empty() || s.stop_requested; });
            got.assign(s.rx_queue.begin(), s.rx_queue.end());
            s.rx_queue.clear();
        }
        return got;
    }, py::arg("timeout") = 1.0);

    // --- RX filter, applied before frames enter the receive() queue ---

    m.def("set_filter", [](uint32_t id, uint32_t mask, const py::object& extended, const py::object& interface_id)
    {
        std::scoped_lock lock(g_state->mutex);
        g_state->filter = {.id = id, .mask = mask,
                           .extended = extended.is_none() ? std::nullopt : std::optional<bool>(extended.cast<bool>()),
                           .iface = interface_id.is_none() ? std::nullopt : std::optional<uint16_t>(interface_id.cast<uint16_t>()),
                           .active = true};
    }, "Accept only frames with (id & mask) == (filter id & mask); extended=True/False limits to "
       "extended/standard frames, interface_id to one interface (None: all).",
       py::arg("id"), py::arg("mask") = 0xFFFFFFFFu, py::arg("extended") = py::none(),
       py::arg("interface_id") = py::none());

    m.def("rx_dropped", []() { return g_state->rx_dropped.load(); },
          "Frames dropped this run because the receive() queue was full.");

    m.def("clear_filter", []()
    {
        std::scoped_lock lock(g_state->mutex);
        g_state->filter.active = false;
    });

    // TX echoes are left out of receive() unless enabled.
    m.def("enable_tx_echo", [](bool enabled) { g_state->echo_tx = enabled; }, py::arg("enabled") = true);

    // --- Periodic TX ---

    m.def("send_periodic", [](BusMessage msg, unsigned interval_ms, std::optional<uint16_t> iface_arg) -> int
    {
        const uint16_t interface_id = send_iface(iface_arg);
        PyState& s = *g_state;
        msg.iface = interface_id;
        std::scoped_lock lock(s.periodic_mutex);
        const int handle = s.next_periodic++;
        s.periodic.emplace(handle, std::jthread([&s, msg, interval_ms, interface_id](std::stop_token stop)
        {
            while (!stop.stop_requested() && !s.stop_requested)
            {
                if (interface_id < g_app->ifaces.size())
                {
                    iface_send(g_app->ifaces[interface_id], msg);
                }
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (stop.stop_requested() || s.stop_requested) return;
                    std::this_thread::sleep_for(10ms);
                }
            }
        }));
        return handle;
    }, py::arg("msg"), py::arg("interval_ms"), py::arg("interface_id") = py::none());

    m.def("stop_periodic", [](int handle)
    {
        py::gil_scoped_release release; // joining must not hold the GIL
        std::scoped_lock lock(g_state->periodic_mutex);
        g_state->periodic.erase(handle); // jthread: request_stop + join
    }, py::arg("handle"));

    // --- Trace access (main thread) ---

    m.def("get_trace", [](int count)
    {
        std::vector<BusMessage> snapshot;
        on_main([&](App& a) { snapshot = trace_copy(a.trace); });
        if (count > 0 && static_cast<size_t>(count) < snapshot.size())
        {
            snapshot.erase(snapshot.begin(), snapshot.end() - count);
        }
        return snapshot;
    }, py::arg("count") = 0);

    m.def("trace_size", []() -> int
    {
        uint64_t size = 0;
        on_main([&](App& a) { size = trace_size(a.trace); });
        return static_cast<int>(size);
    });

    m.def("clear_trace", []() { on_main([](App& a) { trace_clear(a.trace); }); });

    m.def("save_trace", [](const std::string& path, const py::object& format)
    {
        // An explicit format wins; otherwise infer from the extension. Unlike the GUI save
        // dialog an unrecognised name is an error, not a silent fallback to ASC.
        std::optional<TraceFileFormat> resolved;
        if (format.is_none())
        {
            resolved = trace_format_from_path(path);
            if (!resolved)
            {
                throw py::value_error(std::format("cannot infer trace format from '{}'; pass format=... (one of {})",
                                                  path, format_names()));
            }
        }
        else
        {
            const std::string name = format.cast<std::string>();
            resolved = trace_format_from_name(name);
            if (!resolved)
            {
                throw py::value_error(std::format("unknown trace format '{}'; expected one of {}", name, format_names()));
            }
        }
        PyState& s = *g_state;
        refresh_snapshot(s);
        const std::vector<std::string> names = s.iface_names;
        std::vector<BusMessage> msgs;
        on_main([&](App& a) { msgs = trace_copy(a.trace); });
        py::gil_scoped_release release; // plain file IO from here on
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error(std::format("cannot open '{}' for writing", path));
        }
        write_trace_file(out, *resolved, msgs,
                         [&names](uint16_t i) { return i < names.size() ? names[i] : std::string{}; });
        out.close();
        if (!out)
        {
            throw std::runtime_error(std::format("error writing '{}'", path));
        }
    }, py::arg("path"), py::arg("format") = py::none(),
       "Save the current trace to a file. The format is taken from `format` "
       "('candump', 'asc', 'mdf', 'pcap', 'pcapng') or inferred from the file "
       "extension. Raises ValueError for an unknown format and RuntimeError if the "
       "file cannot be written.");

    // --- Measurement control (main thread; start/stop go the way of F5/F6) ---

    m.def("measurement_running", []() -> bool
    {
        bool running = false;
        on_main([&](App& a) { running = a.measuring; });
        return running;
    });

    m.def("start_measurement", []() -> bool
    {
        // app_frame takes the command after its task drain, so the next task sees the result.
        on_main([](App& a) { a.menu.pending.set(static_cast<std::size_t>(Command::MeasurementStart)); });
        bool running = false;
        on_main([&](App& a) { running = a.measuring; });
        return running;
    });

    m.def("stop_measurement", []() -> bool
    {
        on_main([](App& a) { a.menu.pending.set(static_cast<std::size_t>(Command::MeasurementStop)); });
        bool running = false;
        on_main([&](App& a) { running = a.measuring; });
        return !running;
    });

    // --- Interfaces ---

    m.def("interfaces", []()
    {
        struct Row
        {
            uint16_t id;
            std::string name;
            bool lin;
            std::string state;
        };
        std::vector<Row> rows;
        on_main([&](App& a)
        {
            for (Iface& i : a.ifaces)
            {
                const bool in_setup = std::ranges::any_of(a.setup.networks, [&i](const SetupNetwork& net)
                {
                    return std::ranges::any_of(net.interfaces, [&i](const SetupInterface& si)
                    {
                        return si.enabled && si.driver == i.ops->name && si.name == i.info.name;
                    });
                });
                if (!in_setup)
                {
                    continue;
                }
                IfaceStats stats;
                rows.push_back({.id = i.index, .name = i.info.name, .lin = i.info.bus_type == BusType::LIN,
                                .state = iface_stats(i, stats) ? iface_state_name(stats.state) : ""});
            }
        });
        py::list result;
        for (const Row& r : rows)
        {
            py::dict d;
            d["id"] = r.id;
            d["name"] = r.name;
            d["bus_type"] = r.lin ? "LIN" : "CAN";
            d["state"] = r.state;
            result.append(d);
        }
        return result;
    }, "The enabled interfaces of the measurement setup as dicts {id, name, bus_type, state}; "
       "`id` is what send() takes. `state` is empty while the interface is not open.");

    m.def("interface_name", [](uint16_t id) -> std::string
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        return id < s.iface_names.size() ? s.iface_names[id] : std::string{};
    });

    m.def("interface_state", [](uint16_t interface_id) -> std::string
    {
        std::string state;
        on_main([&](App& a)
        {
            IfaceStats stats;
            if (interface_id < a.ifaces.size() && iface_stats(a.ifaces[interface_id], stats))
            {
                state = iface_state_name(stats.state);
            }
        });
        return state;
    }, py::arg("interface_id"));

    // --- Logging ---

    m.def("log", [](const std::string& text) { log_info(text); });
    m.def("log_info", [](const std::string& text) { log_info(text); });
    m.def("log_warning", [](const std::string& text) { log_warning(text); });
    m.def("log_error", [](const std::string& text) { log_error(text); });

    // --- DBC access (worker-side snapshot of the setup) ---

    m.def("databases", []()
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        py::list result;
        for (const SetupNetwork& net : s.setup.networks)
        {
            for (const auto& db : net.can_dbs)
            {
                py::dict info;
                info["file"] = fs::path(db->path).filename().string();
                info["path"] = db->path;
                info["network"] = net.name;
                py::list msgs;
                for (const auto& [raw_id, msg] : db->messages)
                {
                    py::dict m_info;
                    m_info["name"] = msg.name;
                    m_info["id"] = msg.raw_id;
                    m_info["dlc"] = msg.dlc;
                    py::list names;
                    for (const CanDbSignal& sig : msg.signals)
                    {
                        names.append(sig.name);
                    }
                    m_info["signals"] = names;
                    msgs.append(m_info);
                }
                info["messages"] = msgs;
                result.append(info);
            }
        }
        return result;
    });

    // { "message", "id", "sender"?, "signals": { name: { "value", "raw", "unit", "min", "max", "value_name"? } } } or None
    m.def("decode", [](const BusMessage& msg) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const CanDbMessage* db = setup_find_can_message(s.setup, msg);
        if (db == nullptr)
        {
            return py::none();
        }
        py::dict sigs;
        for (const CanDbSignal& sig : db->signals)
        {
            if (!can_signal_present(*db, sig, msg))
            {
                continue;
            }
            const uint64_t raw = can_signal_extract_raw(sig, msg);
            py::dict info;
            info["value"] = can_signal_raw_to_physical(sig, raw);
            info["raw"] = raw;
            info["unit"] = sig.unit;
            info["min"] = sig.min;
            info["max"] = sig.max;
            if (const std::string_view name = can_signal_value_name(sig, raw); !name.empty())
            {
                info["value_name"] = std::string(name);
            }
            sigs[py::cast(sig.name)] = info;
        }
        py::dict result;
        result["message"] = db->name;
        result["id"] = db->raw_id;
        result["signals"] = sigs;
        if (!db->sender.empty())
        {
            result["sender"] = db->sender;
        }
        return result;
    }, py::arg("msg"));

    m.def("lookup", [](const BusMessage& msg) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const CanDbMessage* db = setup_find_can_message(s.setup, msg);
        return db != nullptr ? py::object(message_dict(*db)) : py::none();
    }, py::arg("msg"));

    // By name (str) or raw id (int); the same dict as lookup(), or None.
    m.def("find_message", [](const py::object& name_or_id) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const CanDbMessage* db = find_can_message(s.setup, name_or_id);
        return db != nullptr ? py::object(message_dict(*db)) : py::none();
    }, py::arg("name_or_id"));

    m.def("signal_value", [](const BusMessage& msg, const std::string& signal_name) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const CanDbMessage* db = setup_find_can_message(s.setup, msg);
        const CanDbSignal* sig = db != nullptr ? can_db_find_signal(*db, signal_name) : nullptr;
        return sig != nullptr ? py::cast(can_signal_extract_physical(*sig, msg)) : py::none();
    }, py::arg("msg"), py::arg("signal_name"));

    // Builds a Message with the DBC id and DLC from physical signal values.
    m.def("encode", [](const py::object& name_or_id, const py::dict& values) -> BusMessage
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const CanDbMessage* db = find_can_message(s.setup, name_or_id);
        if (db == nullptr)
        {
            throw py::value_error("message not found in loaded DBC databases");
        }
        BusMessage msg{.id = db->raw_id & can_id_mask_extended};
        if ((db->raw_id & 0x80000000u) != 0)
        {
            msg.flags |= bus_flag::extended;
        }
        set_length(msg, db->dlc);
        for (const auto& item : values)
        {
            const std::string name = item.first.cast<std::string>();
            const CanDbSignal* sig = can_db_find_signal(*db, name);
            if (sig == nullptr)
            {
                throw py::value_error("signal not found in DBC: " + name);
            }
            if (sig->factor == 0.0)
            {
                throw py::value_error(std::format("signal '{}' has factor=0 in DBC - cannot encode", name));
            }
            can_signal_inject_physical(*sig, msg, item.second.cast<double>());
        }
        return msg;
    }, py::arg("name_or_id"), py::arg("values"));

    // --- LIN database access ---

    m.def("lin_databases", []()
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        py::list result;
        for (const SetupNetwork& net : s.setup.networks)
        {
            for (const auto& db : net.lin_dbs)
            {
                py::dict info;
                info["file"] = fs::path(db->path).filename().string();
                info["path"] = db->path;
                info["network"] = net.name;
                info["speed"] = static_cast<int>(db->speed_bps);
                info["master"] = db->master_node;
                py::list frames;
                for (const auto& [id, frame] : db->frames)
                {
                    frames.append(lin_frame_dict(frame));
                }
                info["frames"] = frames;
                result.append(info);
            }
        }
        return result;
    });

    m.def("find_lin_frame", [](const py::object& name_or_id) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        for (const SetupNetwork& net : s.setup.networks)
        {
            for (const auto& db : net.lin_dbs)
            {
                const LinFrame* frame = nullptr;
                if (py::isinstance<py::str>(name_or_id))
                {
                    frame = lin_db_find_frame(*db, name_or_id.cast<std::string>());
                }
                else if (const auto it = db->frames.find(static_cast<uint8_t>(name_or_id.cast<int>())); it != db->frames.end())
                {
                    frame = &it->second;
                }
                if (frame != nullptr)
                {
                    return lin_frame_dict(*frame);
                }
            }
        }
        return py::none();
    }, py::arg("name_or_id"));

    // { "frame", "id", "signals": { name: { "value", "raw", "unit", "value_name"? } } } or None
    m.def("decode_lin", [](const BusMessage& msg) -> py::object
    {
        PyState& s = *g_state;
        refresh_snapshot(s);
        const LinFrame* frame = setup_find_lin_frame(s.setup, msg);
        if (frame == nullptr)
        {
            return py::none();
        }
        const std::span<const uint8_t> payload(msg.data.data(), msg.len);
        py::dict sigs;
        for (const LinSignal& sig : frame->signals)
        {
            const uint64_t raw = lin_signal_extract_raw(sig, payload);
            py::dict info;
            info["value"] = lin_signal_raw_to_physical(sig, raw);
            info["raw"] = raw;
            info["unit"] = sig.unit;
            if (const std::string_view name = lin_signal_value_name(sig, raw); !name.empty())
            {
                info["value_name"] = std::string(name);
            }
            sigs[py::cast(sig.name)] = info;
        }
        py::dict result;
        result["frame"] = frame->name;
        result["id"] = frame->id;
        result["signals"] = sigs;
        return result;
    }, py::arg("msg"));

    // --- AUTOSAR E2E Profile 2 ---

    m.def("e2e_p2_compute_crc", [](const BusMessage& msg, uint16_t data_id) -> uint8_t
    {
        return e2e_p2_compute_crc(msg, data_id);
    }, py::arg("msg"), py::arg("data_id"),
       "Compute AUTOSAR E2E Profile 2 CRC-8H2F over msg. Byte 0 is treated as 0x00; byte 1 "
       "must already hold the counter nibble. Returns the CRC byte without modifying the message.");

    m.def("e2e_p2_protect", [](BusMessage& msg, uint16_t data_id, uint8_t counter)
    {
        msg.data[1] = counter & 0x0Fu;
        msg.data[0] = e2e_p2_compute_crc(msg, data_id);
    }, py::arg("msg"), py::arg("data_id"), py::arg("counter"),
       "Write the AUTOSAR E2E Profile 2 header into msg in place: counter nibble in byte 1, "
       "then the CRC in byte 0.");
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

void python_console_append(PyState& s, std::string text, bool error)
{
    if (text.empty())
    {
        return;
    }
    std::scoped_lock lock(s.mutex);
    s.console_bytes += text.size();
    if (!s.console.empty() && s.console.back().error == error)
    {
        s.console.back().text += text;
    }
    else
    {
        s.console.push_back({.text = std::move(text), .error = error});
    }
    while (s.console_bytes > console_max_bytes)
    {
        std::string& front = s.console.front().text;
        if (s.console.size() > 1)
        {
            s.console_bytes -= front.size();
            s.console.erase(s.console.begin());
        }
        else
        {
            const std::size_t drop = front.size() - console_max_bytes / 2;
            front.erase(0, drop);
            s.console_bytes -= drop;
        }
    }
    ++s.console_total;
}

void python_console_clear(PyState& s)
{
    std::scoped_lock lock(s.mutex);
    s.console.clear();
    s.console_bytes = 0;
    ++s.console_total;
}

void python_rx_consumer(void* user, const BusMessage& msg)
{
    PyState& s = *static_cast<PyState*>(user);
    if (!s.running.load(std::memory_order_relaxed))
    {
        return;
    }
    if (msg.type == BusType::CAN && has_flag(msg, bus_flag::tx) && !s.echo_tx.load(std::memory_order_relaxed))
    {
        return;
    }
    {
        std::scoped_lock lock(s.mutex);
        const PyRxFilter& f = s.filter;
        if (f.active && ((msg.id & f.mask) != (f.id & f.mask)
                         || (f.extended.has_value() && has_flag(msg, bus_flag::extended) != *f.extended)
                         || (f.iface.has_value() && msg.iface != *f.iface)))
        {
            return;
        }
        if (s.rx_queue.size() < rx_queue_max)
        {
            s.rx_queue.push_back(msg);
            s.rx_cv.notify_one();
            return;
        }
    }
    if (s.rx_dropped.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        log_warning("Python: receive() queue full (" + std::to_string(rx_queue_max)
                    + " frames), dropping frames; see kraken.rx_dropped()");
    }
}

void python_input(PyState& s, std::string line)
{
    if (!s.running)
    {
        return;
    }
    if (line.empty() || line.back() != '\n')
    {
        line += '\n';
    }
    std::scoped_lock lock(s.mutex);
    s.input_queue.push_back(std::move(line));
    s.input_cv.notify_one();
}

bool python_run(App& app, PyState& s, std::string code, std::string name)
{
    g_app = &app;
    g_state = &s;
    if (s.running)
    {
        python_console_append(s, "A script is already running.\n", true);
        return false;
    }
    python_poll(s);
    python_console_clear(s);
    if (s.main_thread_state == nullptr)
    {
        if (s.init_error.empty())
        {
            set_bundled_python_home();
            try
            {
                py::initialize_interpreter(false); // no SIGINT handler: Ctrl+C still ends the app
            }
            catch (const std::exception& e)
            {
                s.init_error = e.what();
            }
        }
        if (!s.init_error.empty())
        {
            python_console_append(s, "Python interpreter failed to initialize: " + s.init_error + "\n", true);
            return false;
        }
        s.main_thread_state = PyEval_SaveThread(); // the main thread never holds the GIL again
    }
    {
        std::scoped_lock lock(s.mutex);
        s.rx_queue.clear();
        s.input_queue.clear();
        s.filter = {}; // set_filter() lasts one run
    }
    s.rx_dropped = 0;
    s.stop_requested = false;
    s.running = true;
    s.worker = std::jthread([&s, &app, code = std::move(code), name = std::move(name)]() mutable
                            { worker_main(s, app, std::move(code), std::move(name)); });
    return true;
}

void python_stop(PyState& s)
{
    s.stop_requested = true;
    std::scoped_lock lock(s.mutex);
    s.rx_cv.notify_all();
    s.input_cv.notify_all();
}

void python_poll(PyState& s)
{
    if (!s.running && s.worker.joinable())
    {
        s.worker.join();
    }
}

void python_shutdown(App& app, PyState& s)
{
    python_stop(s);
    // ponytail: time.sleep() is interruptible (prelude); a script blocked in another C call (socket recv) delays exit until it returns.
    while (s.running)
    {
        tasks_drain(app.tasks, app); // the script may be waiting on a main-thread call
        std::this_thread::sleep_for(10ms);
    }
    python_poll(s);
    stop_all_periodic(s);
    if (s.main_thread_state != nullptr)
    {
        PyEval_RestoreThread(static_cast<PyThreadState*>(s.main_thread_state));
        s.main_thread_state = nullptr;
        py::finalize_interpreter();
    }
}
