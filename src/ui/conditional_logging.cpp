#include "ui/conditional_logging.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <format>
#include <iterator>
#include <span>
#include <unordered_set>

#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue
#include <misc/cpp/imgui_stdlib.h>

#include "app.h"
#include "core/log.h"
#include "ui/theme.h"

namespace
{

constexpr const char* op_names[] = {">", "<", "==", ">=", "<=", "!="}; // ConditionOp order
static_assert(std::size(op_names) == static_cast<std::size_t>(ConditionOp::Count));

void write_header(ConditionalLogging& cl)
{
    cl.file << "Timestamp";
    for (const CanDbSignal* sig : cl.config.log_signals)
    {
        cl.file << ',' << sig->name;
        if (!sig->unit.empty())
        {
            cl.file << " [" << sig->unit << ']';
        }
    }
    cl.file << '\n';
}

void write_row(ConditionalLogging& cl, double timestamp, const SignalValues& values)
{
    cl.file << std::format("{:.6f}", timestamp);
    for (const CanDbSignal* sig : cl.config.log_signals)
    {
        const auto it = values.find(sig);
        cl.file << ',' << (it == values.end() ? std::string("NaN") : std::format("{}", it->second));
    }
    cl.file << '\n';
}

void open_file(ConditionalLogging& cl)
{
    const std::string& path = cl.config.log_file_path;
    if (path.empty())
    {
        return;
    }
    std::error_code ec;
    const bool empty = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
    cl.file.open(path, std::ios::app);
    if (!cl.file.is_open())
    {
        log_error(std::format("Conditional logging: cannot open {}", path));
        return;
    }
    if (empty)
    {
        write_header(cl);
    }
    for (const auto& [ts, values] : cl.pre_buffer)
    {
        write_row(cl, ts, values);
    }
    cl.pre_buffer.clear();
    cl.file.flush();
}

// All signals of the setup, for pruning and the dialog combos.
struct SignalRef
{
    const CanDbSignal* signal;
    std::string label; // "Message.Signal"
};

std::vector<SignalRef> setup_signals(const Setup& setup)
{
    std::vector<SignalRef> out;
    for (const SetupNetwork& net : setup.networks)
    {
        for (const auto& db : net.can_dbs)
        {
            for (const auto& [raw_id, msg] : db->messages)
            {
                for (const CanDbSignal& sig : msg.signals)
                {
                    out.push_back({&sig, std::format("{}.{}", msg.name, sig.name)});
                }
            }
        }
    }
    return out;
}

// --- dialog ---------------------------------------------------------------

void set_checked(std::vector<const CanDbSignal*>& list, const CanDbSignal* sig, bool on)
{
    if (on && !std::ranges::contains(list, sig))
    {
        list.push_back(sig);
    }
    else if (!on)
    {
        std::erase(list, sig);
    }
}

// Tri-state checkbox over a group of signals; a click checks all or, if all are checked, none.
void group_checkbox(const char* id, std::vector<const CanDbSignal*>& list, std::span<const CanDbSignal* const> group)
{
    const auto checked = static_cast<std::size_t>(std::ranges::count_if(group, [&](const CanDbSignal* s) { return std::ranges::contains(list, s); }));
    bool all = !group.empty() && checked == group.size();
    const bool mixed = checked > 0 && !all;
    if (mixed)
    {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    if (ImGui::Checkbox(id, &all))
    {
        for (const CanDbSignal* s : group)
        {
            set_checked(list, s, all);
        }
    }
    if (mixed)
    {
        ImGui::PopItemFlag();
    }
}

void draw_signal_tree(const Setup& setup, ConditionalLoggingConfig& cfg)
{
    std::vector<const CanDbSignal*> group;
    for (const SetupNetwork& net : setup.networks)
    {
        ImGui::PushID(&net);
        group.clear();
        for (const auto& db : net.can_dbs)
        {
            for (const auto& [raw_id, msg] : db->messages)
            {
                for (const CanDbSignal& sig : msg.signals)
                {
                    group.push_back(&sig);
                }
            }
        }
        group_checkbox("##net", cfg.log_signals, group);
        ImGui::SameLine();
        if (ImGui::TreeNode(net.name.c_str()))
        {
            for (const auto& db : net.can_dbs)
            {
                for (const auto& [raw_id, msg] : db->messages)
                {
                    ImGui::PushID(&msg);
                    group.clear();
                    for (const CanDbSignal& sig : msg.signals)
                    {
                        group.push_back(&sig);
                    }
                    group_checkbox("##msg", cfg.log_signals, group);
                    ImGui::SameLine();
                    if (ImGui::TreeNode(msg.name.c_str()))
                    {
                        for (const CanDbSignal& sig : msg.signals)
                        {
                            bool on = std::ranges::contains(cfg.log_signals, &sig);
                            if (ImGui::Checkbox(sig.name.c_str(), &on))
                            {
                                set_checked(cfg.log_signals, &sig, on);
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// "Any" or one enumerated interface.
void draw_condition_iface(const App& app, uint16_t& iface)
{
    const char* preview = iface < app.ifaces.size() ? app.ifaces[iface].info.name.c_str() : "Any";
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##iface", preview))
    {
        if (ImGui::Selectable("Any", iface >= app.ifaces.size()))
        {
            iface = log_condition_any_iface;
        }
        for (std::size_t i = 0; i < app.ifaces.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(app.ifaces[i].info.name.c_str(), i == iface))
            {
                iface = static_cast<uint16_t>(i);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void draw_conditions(const App& app, ConditionalLogging& cl, const std::vector<SignalRef>& signals)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##conditions", 4, flags, ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * 5.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Operator", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(cl.edit.conditions.size()); ++i)
        {
            LogCondition& cond = cl.edit.conditions[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto ref = std::ranges::find(signals, cond.signal, &SignalRef::signal);
            const char* preview = ref != signals.end() ? ref->label.c_str() : "-";
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##sig", preview))
            {
                for (const SignalRef& s : signals)
                {
                    if (ImGui::Selectable(s.label.c_str(), s.signal == cond.signal))
                    {
                        cond.signal = s.signal;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemClicked())
            {
                cl.selected_condition = i;
            }
            ImGui::TableNextColumn();
            draw_condition_iface(app, cond.iface);
            if (ImGui::IsItemClicked())
            {
                cl.selected_condition = i;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int op = static_cast<int>(cond.op);
            if (ImGui::Combo("##op", &op, op_names, static_cast<int>(ConditionOp::Count)))
            {
                cond.op = static_cast<ConditionOp>(op);
            }
            if (ImGui::IsItemClicked())
            {
                cl.selected_condition = i;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputDouble("##th", &cond.threshold, 0.0, 0.0, "%g");
            if (ImGui::IsItemActivated())
            {
                cl.selected_condition = i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add Condition"))
    {
        cl.edit.conditions.push_back({.signal = signals.empty() ? nullptr : signals.front().signal});
        cl.selected_condition = static_cast<int>(cl.edit.conditions.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(cl.selected_condition < 0 || cl.selected_condition >= static_cast<int>(cl.edit.conditions.size()));
    if (ImGui::Button("Remove Condition"))
    {
        cl.edit.conditions.erase(cl.edit.conditions.begin() + cl.selected_condition);
        cl.selected_condition = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Use AND logic for all conditions", &cl.edit.and_logic);
}

} // namespace

bool condition_holds(ConditionOp op, double value, double threshold) noexcept
{
    switch (op)
    {
    case ConditionOp::Greater:
        return value > threshold;
    case ConditionOp::Less:
        return value < threshold;
    case ConditionOp::Equal:
        return value == threshold;
    case ConditionOp::GreaterEqual:
        return value >= threshold;
    case ConditionOp::LessEqual:
        return value <= threshold;
    case ConditionOp::NotEqual:
        return value != threshold;
    default:
        return false;
    }
}

bool conditions_met(const std::vector<LogCondition>& conds, bool and_logic, const SignalValues& values,
                    const IfaceSignalValues& iface_values)
{
    if (conds.empty())
    {
        return false;
    }
    const auto value = [&](const LogCondition& c) -> const double*
    {
        if (c.iface == log_condition_any_iface)
        {
            const auto it = values.find(c.signal);
            return it != values.end() ? &it->second : nullptr;
        }
        const auto it = iface_values.find({c.iface, c.signal});
        return it != iface_values.end() ? &it->second : nullptr;
    };
    for (const LogCondition& c : conds)
    {
        const double* v = value(c);
        const bool met = v != nullptr && condition_holds(c.op, *v, c.threshold);
        if (and_logic && !met)
        {
            return false;
        }
        if (!and_logic && met)
        {
            return true;
        }
    }
    return and_logic;
}

TriggerState conditional_logging_state(const ConditionalLogging& cl) noexcept
{
    return !cl.config.enabled ? TriggerState::Off : cl.condition_met ? TriggerState::Triggered : TriggerState::Armed;
}

const char* trigger_state_label(TriggerState s) noexcept
{
    return s == TriggerState::Armed ? "Armed: waiting for trigger" : s == TriggerState::Triggered ? "Triggered: logging" : "";
}

void conditional_logging_evaluate(ConditionalLogging& cl)
{
    const bool met = cl.config.enabled && conditions_met(cl.config.conditions, cl.config.and_logic, cl.values, cl.iface_values);
    if (met == cl.condition_met)
    {
        return;
    }
    cl.condition_met = met;
    if (met)
    {
        open_file(cl);
    }
    else
    {
        cl.file.close();
    }
}

void conditional_logging_process(ConditionalLogging& cl, const Setup& setup, const BusMessage& m,
                                 std::chrono::steady_clock::time_point now)
{
    if (!cl.config.enabled)
    {
        return;
    }
    const CanDbMessage* dbmsg = setup_find_can_message(setup, m);
    if (dbmsg == nullptr)
    {
        return;
    }
    bool relevant = false;
    for (const CanDbSignal& sig : dbmsg->signals)
    {
        if (can_signal_present(*dbmsg, sig, m))
        {
            const double v = can_signal_extract_physical(sig, m);
            cl.values[&sig] = v;
            cl.updated[&sig] = now;
            cl.iface_values[{m.iface, &sig}] = v;
            cl.iface_updated[{m.iface, &sig}] = now;
            relevant = true;
        }
    }
    if (!relevant)
    {
        return;
    }
    const double ts = static_cast<double>(m.ts_ns) / 1e9;
    if (!cl.condition_met && !cl.config.log_file_path.empty())
    {
        cl.pre_buffer.emplace_back(ts, cl.values);
        while (!cl.pre_buffer.empty() && ts - cl.pre_buffer.front().first > conditional_logging_pre_seconds)
        {
            cl.pre_buffer.pop_front();
        }
    }
    const bool was_met = cl.condition_met;
    conditional_logging_evaluate(cl);
    // On the rising edge this sample went out with the pre-buffer already.
    if (cl.condition_met && was_met && cl.file.is_open())
    {
        write_row(cl, ts, cl.values);
    }
}

bool conditional_logging_timeout(ConditionalLogging& cl, std::chrono::steady_clock::time_point now)
{
    bool dropped = false;
    for (auto it = cl.updated.begin(); it != cl.updated.end();)
    {
        if (now - it->second > conditional_logging_stale)
        {
            cl.values.erase(it->first);
            it = cl.updated.erase(it);
            dropped = true;
        }
        else
        {
            ++it;
        }
    }
    dropped |= std::erase_if(cl.iface_updated,
                             [&](const auto& kv)
                             {
                                 if (now - kv.second <= conditional_logging_stale)
                                 {
                                     return false;
                                 }
                                 cl.iface_values.erase(kv.first);
                                 return true;
                             }) > 0;
    if (dropped)
    {
        conditional_logging_evaluate(cl);
        if (cl.file.is_open())
        {
            cl.file.flush();
        }
    }
    return dropped;
}

void conditional_logging_apply(ConditionalLogging& cl, ConditionalLoggingConfig config)
{
    cl.file.close();
    cl.condition_met = false;
    cl.pre_buffer.clear();
    cl.config = std::move(config);
    if (!cl.config.enabled)
    {
        cl.values.clear();
        cl.updated.clear();
        cl.iface_values.clear();
        cl.iface_updated.clear();
    }
    conditional_logging_evaluate(cl);
}

void conditional_logging_prune(ConditionalLogging& cl, const Setup& setup)
{
    std::unordered_set<const CanDbSignal*> valid;
    for (const SignalRef& s : setup_signals(setup))
    {
        valid.insert(s.signal);
    }
    const auto gone = [&](const CanDbSignal* s) { return !valid.contains(s); };
    const auto before = cl.config.conditions.size() + cl.config.log_signals.size();
    std::erase_if(cl.config.conditions, [&](const LogCondition& c) { return gone(c.signal); });
    std::erase_if(cl.config.log_signals, gone);
    std::erase_if(cl.values, [&](const auto& kv) { return gone(kv.first); });
    std::erase_if(cl.updated, [&](const auto& kv) { return gone(kv.first); });
    std::erase_if(cl.iface_values, [&](const auto& kv) { return gone(kv.first.second); });
    std::erase_if(cl.iface_updated, [&](const auto& kv) { return gone(kv.first.second); });
    if (before != cl.config.conditions.size() + cl.config.log_signals.size())
    {
        cl.file.close(); // the column set changed: the header would no longer match
        cl.condition_met = false;
        cl.pre_buffer.clear();
        conditional_logging_evaluate(cl);
    }
}

void conditional_logging_frame(App& app, ConditionalLogging& cl)
{
    if (cl.setup_generation != app.setup.generation)
    {
        cl.setup_generation = app.setup.generation;
        conditional_logging_prune(cl, app.setup);
    }
    const Trace& t = app.trace;
    cl.processed = std::clamp(cl.processed, t.begin, t.end);
    if (!cl.config.enabled || !t.file.empty()) // a file view is not live traffic
    {
        cl.processed = t.end;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (; cl.processed < t.end; ++cl.processed)
    {
        conditional_logging_process(cl, app.setup, trace_at(t, cl.processed), now);
    }
    if (now - cl.timeout_checked >= std::chrono::seconds(1))
    {
        cl.timeout_checked = now;
        conditional_logging_timeout(cl, now);
        if (cl.file.is_open())
        {
            cl.file.flush();
        }
    }
}

void conditional_logging_open(ConditionalLogging& cl)
{
    cl.edit = cl.config;
    cl.selected_condition = -1;
    cl.error.clear();
    cl.open = true;
}

void draw_conditional_logging(App& app, ConditionalLogging& cl)
{
    if (!cl.open)
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(600.0f * px, 600.0f * px), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Conditional Logging Configuration", &cl.open))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput
        && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        cl.open = false; // like Cancel
    }
    const std::vector<SignalRef> signals = setup_signals(app.setup);

    ImGui::TextUnformatted("Conditions (Trigger Logging)");
    draw_conditions(app, cl, signals);

    ImGui::Spacing();
    ImGui::TextUnformatted("CAN Selection (Network / Message / Signal)");
    const float footer = ImGui::GetFrameHeightWithSpacing() * 3.0f + ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##tree", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders))
    {
        draw_signal_tree(app.setup, cl.edit);
    }
    ImGui::EndChild();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Log File Path (Optional for filtering):");
    ImGui::SameLine();
    const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-browse_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##path", &cl.edit.log_file_path, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        file_dialog_open(cl.file_dialog, FileDialogMode::Save, "Select Log File", cl.edit.log_file_path,
                         {{"CSV Files", "*.csv"}});
    }
    for (const auto& path : file_dialog_draw(cl.file_dialog))
    {
        cl.edit.log_file_path = path;
    }
    ImGui::Checkbox("Enable Conditional Logging (File Output)", &cl.edit.enabled);
    if (const TriggerState trig = conditional_logging_state(cl); trig != TriggerState::Off)
    {
        ImGui::SameLine();
        const unsigned c = theme_text(trig == TriggerState::Armed ? ThemeText::warn : ThemeText::rec);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c), "%s", trigger_state_label(trig));
    }
    if (!cl.error.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::error)), "%s", cl.error.c_str());
    }
    if (ImGui::Button("OK"))
    {
        if (cl.edit.enabled && cl.edit.log_file_path.empty())
        {
            cl.error = "Please select an output file for logging.";
        }
        else
        {
            cl.error.clear();
            conditional_logging_apply(cl, cl.edit);
            cl.open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        cl.open = false;
    }
    ImGui::End();
}
