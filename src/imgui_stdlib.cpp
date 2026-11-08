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

// ImGui's std::string InputText overloads (misc/cpp/imgui_stdlib.cpp, replaced in cmake/deps.cmake)
// plus a right-click menu on every such field: Cut / Copy / Paste / Select All. Every text field
// of the app goes through these, so one file gives them all the menu, read-only ones (logs, the
// Python console) Copy and Select All.

#include <algorithm>
#include <string>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

namespace
{

struct CallbackData
{
    std::string* text;
    ImGuiInputTextCallback chained_callback;
    void* chained_user_data;
};

int resize_callback(ImGuiInputTextCallbackData* data)
{
    auto* user = static_cast<CallbackData*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        std::string* text = user->text;
        IM_ASSERT(data->Buf == text->c_str());
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    else if (user->chained_callback != nullptr)
    {
        data->UserData = user->chained_user_data;
        return user->chained_callback(data);
    }
    return 0;
}

ImGuiID g_select_all = 0; // field to select all of once it is active again (ActivateItemByID lands next frame)

// The menu of the field drawn just before (id = its item id). While the field is being edited
// (or was, until the right-click took the focus) the selection is used, else the whole text.
// Returns true when the text changed, so the field's caller sees an edit.
bool text_context_menu(ImGuiID id, std::string& text, bool read_only)
{
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (g_select_all == id && state != nullptr && ImGui::IsItemActive())
    {
        state->SelectAll();
        g_select_all = 0;
    }
    if (!ImGui::BeginPopupContextItem())
    {
        return false;
    }
    // The stretch the menu works on: the selection while editing, else the whole text.
    std::size_t begin = 0;
    std::size_t end = text.size();
    if (state != nullptr && state->HasSelection())
    {
        begin = std::min(static_cast<std::size_t>(std::min(state->GetSelectionStart(), state->GetSelectionEnd())), text.size());
        end = std::min(static_cast<std::size_t>(std::max(state->GetSelectionStart(), state->GetSelectionEnd())), text.size());
    }
    const bool has_text = end > begin;
    const char* clipboard = ImGui::GetClipboardText();
    const bool has_clipboard = clipboard != nullptr && clipboard[0] != '\0';
    bool changed = false;
    if (ImGui::MenuItem("Cut", nullptr, false, !read_only && has_text))
    {
        ImGui::SetClipboardText(text.substr(begin, end - begin).c_str());
        text.erase(begin, end - begin);
        changed = true;
    }
    if (ImGui::MenuItem("Copy", nullptr, false, has_text))
    {
        ImGui::SetClipboardText(text.substr(begin, end - begin).c_str());
    }
    if (ImGui::MenuItem("Paste", nullptr, false, !read_only && has_clipboard))
    {
        text.replace(begin, end - begin, clipboard);
        changed = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Select All", nullptr, false, !text.empty()))
    {
        ImGui::ActivateItemByID(id);
        g_select_all = id;
    }
    ImGui::EndPopup();
    if (changed && state != nullptr)
    {
        state->ClearSelection(); // the old offsets no longer fit the text
    }
    return changed;
}

} // namespace

bool ImGui::InputText(const char* label, std::string* str, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    CallbackData cb{str, callback, user_data};
    const bool edited = InputText(label, str->data(), str->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, resize_callback, &cb);
    return text_context_menu(GetItemID(), *str, (flags & ImGuiInputTextFlags_ReadOnly) != 0) || edited;
}

bool ImGui::InputTextMultiline(const char* label, std::string* str, const ImVec2& size, ImGuiInputTextFlags flags,
                               ImGuiInputTextCallback callback, void* user_data)
{
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    CallbackData cb{str, callback, user_data};
    const bool edited = InputTextMultiline(label, str->data(), str->capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize,
                                           resize_callback, &cb);
    return text_context_menu(GetItemID(), *str, (flags & ImGuiInputTextFlags_ReadOnly) != 0) || edited;
}

bool ImGui::InputTextWithHint(const char* label, const char* hint, std::string* str, ImGuiInputTextFlags flags,
                              ImGuiInputTextCallback callback, void* user_data)
{
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    CallbackData cb{str, callback, user_data};
    const bool edited = InputTextWithHint(label, hint, str->data(), str->capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize,
                                          resize_callback, &cb);
    return text_context_menu(GetItemID(), *str, (flags & ImGuiInputTextFlags_ReadOnly) != 0) || edited;
}
