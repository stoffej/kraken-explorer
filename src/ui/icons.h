#pragma once

#include <cstdint>
#include <vector>

// Toolbar/menu icons: src/assets SVGs, rasterised once with nanosvg into one texture atlas.
// Glyphs are white with the SVG's alpha, so the draw-time tint (ImGuiCol_Text) colours them
// for both themes. Order must match ICON_SVGS in src/ui/CMakeLists.txt.
enum class Icon
{
    DocumentOpen,
    DocumentSaveAs,
    DocumentSave,
    EditClear,
    PlaybackStart,
    PlaybackStop,
    PreferencesSystem,
    ViewRefresh,
    Graph,
    Filter,
    Folder,
    File,
    GoUp,
    GoHome,
    Record,
    Replay,
    Database,
    Convert,
    PlaybackPause,
    PlaybackStep,
    Count
};

struct IconAtlasPixels
{
    int width = 0;
    int height = 0;
    int cell = 0;                // square glyph size in pixels
    std::vector<uint8_t> rgba;   // width * height * 4
};

// CPU part, testable without a GL context. scale is 1 or 2 (glyphs are 16 * scale px).
IconAtlasPixels icons_rasterize(int scale);

// Uploads the atlas for the current GL context. dpi_scale is the window content scale;
// anything above 1 rasterises at 2x.
void icons_init(float dpi_scale);
void icons_shutdown();

// Draw helpers, size in logical pixels (0 = current font size), tinted with ImGuiCol_Text.
void icon_image(Icon icon, float size = 0.0f);
bool icon_button(const char* id, Icon icon, float size = 0.0f);

// Button with an icon left of label (label is also the ImGui id, so keep it unique per window).
bool icon_text_button(const char* label, Icon icon);
float icon_text_button_width(const char* label);

// Toolbar wrapping: SameLine() when the next next_width pixels still fit in the window,
// otherwise the item starts a new line. button_width() is the width of ImGui::Button(label).
void same_line_or_wrap(float next_width);
float button_width(const char* label);
