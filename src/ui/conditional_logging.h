#pragma once

// Conditional logging (the old ConditionalLoggingManager + ConditionalLoggingDialog): DBC
// signal values decoded from the trace on the main thread, a condition set over them, and a
// CSV of the chosen signals written while the conditions hold (plus the 5 s before).

#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ui/file_dialog.h"

struct App;
struct BusMessage;
struct CanDbSignal;
struct Setup;

enum class ConditionOp : uint8_t
{
    Greater,
    Less,
    Equal,
    GreaterEqual,
    LessEqual,
    NotEqual,
    Count
};

[[nodiscard]] bool condition_holds(ConditionOp op, double value, double threshold) noexcept;

inline constexpr uint16_t log_condition_any_iface = UINT16_MAX;

struct LogCondition
{
    const CanDbSignal* signal = nullptr;
    ConditionOp op = ConditionOp::Greater;
    double threshold = 0.0;
    uint16_t iface = log_condition_any_iface; // BusMessage::iface the value must come from; any: latest of all
};

using SignalValues = std::map<const CanDbSignal*, double>;
using IfaceSignalKey = std::pair<uint16_t, const CanDbSignal*>;
using IfaceSignalValues = std::map<IfaceSignalKey, double>;

// AND: every condition has a current value and holds; OR: any one does. No conditions: false.
// A condition bound to an interface reads iface_values, otherwise values.
[[nodiscard]] bool conditions_met(const std::vector<LogCondition>& conds, bool and_logic, const SignalValues& values,
                                  const IfaceSignalValues& iface_values = {});

// What the dialog edits; applied as a whole on OK.
struct ConditionalLoggingConfig
{
    bool enabled = false;
    bool and_logic = true;
    std::vector<LogCondition> conditions;
    std::vector<const CanDbSignal*> log_signals; // CSV columns, in order
    std::string log_file_path;                   // empty: evaluate only, no file
};

// Not movable (ofstream): held by value in App.
struct ConditionalLogging
{
    ConditionalLoggingConfig config;

    // Runtime (main thread)
    bool condition_met = false;
    SignalValues values;
    std::map<const CanDbSignal*, std::chrono::steady_clock::time_point> updated;
    IfaceSignalValues iface_values; // same values per source interface, for interface-bound conditions
    std::map<IfaceSignalKey, std::chrono::steady_clock::time_point> iface_updated;
    std::deque<std::pair<double, SignalValues>> pre_buffer; // (timestamp s, values) of the 5 s before a trigger
    std::ofstream file;
    uint64_t processed = 0; // Trace index up to which frames were consumed
    uint64_t setup_generation = UINT64_MAX;
    std::chrono::steady_clock::time_point timeout_checked{};

    // Dialog
    bool open = false;
    ConditionalLoggingConfig edit;
    int selected_condition = -1;
    std::string error;
    FileDialog file_dialog; // Browse...
};

// Off: disabled. Armed: enabled, waiting for the conditions. Triggered: they hold, the CSV is written.
enum class TriggerState : uint8_t
{
    Off,
    Armed,
    Triggered
};
[[nodiscard]] TriggerState conditional_logging_state(const ConditionalLogging& cl) noexcept;
// "Armed: waiting for trigger" / "Triggered: logging"; "" when off.
[[nodiscard]] const char* trigger_state_label(TriggerState s) noexcept;

inline constexpr std::chrono::milliseconds conditional_logging_stale{1500}; // value dropped without an update
inline constexpr double conditional_logging_pre_seconds = 5.0;

// Decodes the frame's signals (if its message is in the setup), re-evaluates, writes a CSV
// row while the conditions hold. `ts` = frame time in seconds, `now` for the stale check.
void conditional_logging_process(ConditionalLogging& cl, const Setup& setup, const BusMessage& m,
                                 std::chrono::steady_clock::time_point now);
// Drops values older than conditional_logging_stale and re-evaluates. Returns true if any was dropped.
bool conditional_logging_timeout(ConditionalLogging& cl, std::chrono::steady_clock::time_point now);
// Re-evaluates the conditions; on a rising edge opens the CSV (header if new) and flushes the
// pre-buffer, on a falling edge closes it.
void conditional_logging_evaluate(ConditionalLogging& cl);
// Replaces the configuration; disabling closes the file and clears the runtime state.
void conditional_logging_apply(ConditionalLogging& cl, ConditionalLoggingConfig config);
// Removes conditions / columns whose signal is no longer in the setup (workspace or DBC changed).
void conditional_logging_prune(ConditionalLogging& cl, const Setup& setup);

// Once per frame after the trace was drained: new trace rows, stale check, setup changes.
void conditional_logging_frame(App& app, ConditionalLogging& cl);
// Opens the dialog on a copy of the current configuration.
void conditional_logging_open(ConditionalLogging& cl);
// The "Conditional Logging Configuration" window while cl.open.
void draw_conditional_logging(App& app, ConditionalLogging& cl);
