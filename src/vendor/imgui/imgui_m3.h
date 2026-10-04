// Material 3 Expressive theming for Dear ImGui.
//
// lives in the vendored tree because it draws its own widgets and must sit next
// to imgui_widgets.cc rather than restyle ImGuiCol_* from outside. covers the
// full role set, shape/state/motion tokens, an HCT solver, and a live-reload
// `.colors` file where explicit values win and the rest derives from `source`.

#ifndef IMGUI_M3_H_INCLUDED
#define IMGUI_M3_H_INCLUDED

#include "imgui.h"

// ImRect comes from imgui_internal.h, which includes this header, so forward
// declare it here; every use is a const reference.
struct ImRect;

// The complete M3 role set as of the 2025 Expressive revision, in spec order.

enum ImGuiM3Role_
{
    ImGuiM3Role_Primary,
    ImGuiM3Role_OnPrimary,
    ImGuiM3Role_PrimaryContainer,
    ImGuiM3Role_OnPrimaryContainer,
    ImGuiM3Role_InversePrimary,

    ImGuiM3Role_Secondary,
    ImGuiM3Role_OnSecondary,
    ImGuiM3Role_SecondaryContainer,
    ImGuiM3Role_OnSecondaryContainer,

    ImGuiM3Role_Tertiary,
    ImGuiM3Role_OnTertiary,
    ImGuiM3Role_TertiaryContainer,
    ImGuiM3Role_OnTertiaryContainer,

    ImGuiM3Role_Error,
    ImGuiM3Role_OnError,
    ImGuiM3Role_ErrorContainer,
    ImGuiM3Role_OnErrorContainer,

    ImGuiM3Role_Background,
    ImGuiM3Role_OnBackground,
    ImGuiM3Role_Surface,
    ImGuiM3Role_OnSurface,
    ImGuiM3Role_SurfaceVariant,
    ImGuiM3Role_OnSurfaceVariant,
    ImGuiM3Role_SurfaceDim,
    ImGuiM3Role_SurfaceBright,
    ImGuiM3Role_SurfaceContainerLowest,
    ImGuiM3Role_SurfaceContainerLow,
    ImGuiM3Role_SurfaceContainer,
    ImGuiM3Role_SurfaceContainerHigh,
    ImGuiM3Role_SurfaceContainerHighest,

    ImGuiM3Role_InverseSurface,
    ImGuiM3Role_InverseOnSurface,
    ImGuiM3Role_InverseSurfaceVariant,

    ImGuiM3Role_Outline,
    ImGuiM3Role_OutlineVariant,
    ImGuiM3Role_Shadow,
    ImGuiM3Role_Scrim,
    ImGuiM3Role_SurfaceTint,

    ImGuiM3Role_PrimaryFixed,
    ImGuiM3Role_PrimaryFixedDim,
    ImGuiM3Role_OnPrimaryFixed,
    ImGuiM3Role_OnPrimaryFixedVariant,
    ImGuiM3Role_SecondaryFixed,
    ImGuiM3Role_SecondaryFixedDim,
    ImGuiM3Role_OnSecondaryFixed,
    ImGuiM3Role_OnSecondaryFixedVariant,
    ImGuiM3Role_TertiaryFixed,
    ImGuiM3Role_TertiaryFixedDim,
    ImGuiM3Role_OnTertiaryFixed,
    ImGuiM3Role_OnTertiaryFixedVariant,

    ImGuiM3Role_COUNT
};

typedef ImGuiM3Role_ ImGuiM3Role;

// Scheme variants. mirrors Matugen's `--type` values so its config maps across.
enum ImGuiM3Variant_
{
    ImGuiM3Variant_TonalSpot = 0,
    ImGuiM3Variant_Expressive,
    ImGuiM3Variant_Vibrant,
    ImGuiM3Variant_Neutral,
    ImGuiM3Variant_Monochrome,
    ImGuiM3Variant_Fidelity,
    ImGuiM3Variant_Content,
    ImGuiM3Variant_COUNT
};

typedef ImGuiM3Variant_ ImGuiM3Variant;

// Contrast levels from the May 2025 revision.
enum ImGuiM3Contrast_
{
    ImGuiM3Contrast_Standard = 0,
    ImGuiM3Contrast_Medium,
    ImGuiM3Contrast_High,
    ImGuiM3Contrast_COUNT
};

typedef ImGuiM3Contrast_ ImGuiM3Contrast;

// Shape scale, in dp. Expressive added three steps on top of the classic six.

enum ImGuiM3Shape_
{
    ImGuiM3Shape_None = 0,             // 0dp
    ImGuiM3Shape_ExtraSmall,           // 4dp
    ImGuiM3Shape_Small,                // 8dp
    ImGuiM3Shape_Medium,               // 12dp
    ImGuiM3Shape_Large,                // 16dp
    ImGuiM3Shape_LargeIncreased,       // 20dp  (expressive)
    ImGuiM3Shape_ExtraLarge,           // 28dp
    ImGuiM3Shape_ExtraLargeIncreased,  // 32dp  (expressive)
    ImGuiM3Shape_ExtraExtraLarge,      // 48dp  (expressive)
    ImGuiM3Shape_Full,                 // pill
    ImGuiM3Shape_COUNT
};

typedef ImGuiM3Shape_ ImGuiM3Shape;

// Interaction states. The spec has no "selected" layer (selection is a container
// swap), but the slot stays so callers can ask for it.

enum ImGuiM3State_
{
    ImGuiM3State_Enabled = 0,
    ImGuiM3State_Hovered,       // 0.08
    ImGuiM3State_Focused,       // 0.10
    ImGuiM3State_Pressed,       // 0.10
    ImGuiM3State_Dragged,       // 0.16
    ImGuiM3State_Selected,
    ImGuiM3State_Disabled,      // 0.12 container, 0.38 content
    ImGuiM3State_COUNT
};

typedef ImGuiM3State_ ImGuiM3State;

// Non-color tokens, written to the `.colors` file under `[expressive]` so the
// file stays one flat sheet.

struct ImGuiM3Metrics
{
    // Scaling.
    float       density = 1.0f;             // Multiplies every dp value below.
    float       shape_scale = 1.0f;         // M3 Expressive shape scale knob.
    float       corner_none = 0.0f;
    float       corner_xs = 4.0f;
    float       corner_s = 8.0f;
    float       corner_m = 12.0f;
    float       corner_l = 16.0f;
    float       corner_l_increased = 20.0f;
    float       corner_xl = 28.0f;
    float       corner_xl_increased = 32.0f;
    float       corner_xxl = 48.0f;

    // Buttons: five sizes now exist (expressive).
    float       button_height_xsmall = 32.0f;
    float       button_height_small = 40.0f;
    float       button_height_default = 40.0f;
    float       button_height_medium = 56.0f;
    float       button_height_large = 96.0f;
    float       button_icon_xsmall = 20.0f;
    float       button_icon_small = 20.0f;
    float       button_icon_default = 20.0f;
    float       button_icon_medium = 24.0f;
    float       button_icon_large = 32.0f;
    float       button_outline_width = 1.0f;
    float       button_padding_x = 24.0f;

    // Icon buttons.
    float       icon_button_size = 40.0f;
    float       icon_button_size_medium = 56.0f;
    float       icon_button_icon = 24.0f;

    // FAB.
    float       fab_size = 56.0f;
    float       fab_size_small = 40.0f;
    float       fab_size_large = 96.0f;
    float       fab_icon = 24.0f;
    float       fab_icon_large = 36.0f;

    // Containers.
    float       card_radius = 12.0f;
    float       card_outline_width = 1.0f;
    float       chip_height = 32.0f;
    float       chip_radius = 8.0f;
    float       chip_icon = 18.0f;
    float       dialog_radius = 28.0f;
    float       dialog_min_width = 280.0f;
    float       menu_radius = 4.0f;
    float       menu_item_height = 48.0f;
    float       snackbar_radius = 4.0f;
    float       snackbar_height_1 = 48.0f;
    float       snackbar_height_2 = 68.0f;
    float       search_bar_height = 56.0f;

    // Navigation.
    float       nav_bar_height = 80.0f;
    float       nav_rail_width = 80.0f;
    float       nav_indicator_width = 64.0f;
    float       nav_indicator_height = 32.0f;
    float       nav_rail_indicator_width = 56.0f;
    float       badge_size = 16.0f;
    float       badge_size_large = 16.0f;

    // Selection controls.
    float       switch_track_width = 52.0f;
    float       switch_track_height = 32.0f;
    float       switch_track_outline = 2.0f;
    float       switch_handle = 20.0f;
    float       switch_handle_inset = 4.0f;
    float       switch_handle_with_icon = 22.0f;
    float       switch_pressed_track_width = 40.0f;
    float       switch_pressed_handle = 26.0f;

    float       checkbox_size = 18.0f;
    float       checkbox_radius = 2.0f;
    float       checkbox_outline = 2.0f;
    float       checkbox_icon = 18.0f;
    float       radio_size = 20.0f;
    float       radio_inner = 10.0f;

    // Slider.
    float       slider_track_height = 16.0f;
    float       slider_handle_width = 4.0f;
    float       slider_handle_width_pressed = 2.0f;
    float       slider_handle_height = 44.0f;
    float       slider_handle_padding = 6.0f;
    float       slider_track_inside_corner = 8.0f;
    float       slider_stop_indicator = 4.0f;

    // Tabs.
    float       tab_height = 48.0f;
    float       tab_height_with_icon = 64.0f;
    float       tab_icon = 24.0f;
    float       tab_indicator_height = 3.0f;
    float       tab_indicator_radius = 3.0f;

    // Text field.
    float       text_field_height = 56.0f;
    float       text_field_top_radius = 4.0f;
    float       text_field_outline = 1.0f;
    float       text_field_active_indicator = 1.0f;
    float       text_field_icon = 24.0f;

    // Lists.
    float       list_item_height_1 = 56.0f;
    float       list_item_height_2 = 72.0f;
    float       list_item_height_3 = 88.0f;
    float       list_item_radius = 16.0f;
    float       list_item_radius_expressive = 4.0f;
    float       list_leading_icon = 24.0f;
    float       list_leading_icon_expressive = 20.0f;
    float       list_avatar = 40.0f;
    float       list_image = 56.0f;

    // Misc.
    float       divider_thickness = 1.0f;
    float       progress_track_height = 4.0f;
    float       state_layer_size = 40.0f;
    float       state_layer_target_size = 48.0f;
    float       focus_indicator_thickness = 3.0f;
    float       focus_indicator_outer_offset = 2.0f;
    float       focus_indicator_inner_offset = -3.0f;
    float       scrim_opacity = 0.32f;

    // Elevation shadow geometry per level 0-5. Surface tint is deprecated in the
    // 2025 spec, so the tint opacities only remain for legacy callers.
    float       elevation_dips[6]     = {0.0f, 1.0f, 3.0f, 6.0f, 8.0f, 12.0f};
    float       key_shadow_y[6]       = {0.0f, 1.0f, 1.0f, 1.0f, 2.0f, 4.0f};
    float       key_shadow_blur[6]    = {0.0f, 2.0f, 2.0f, 3.0f, 3.0f, 4.0f};
    float       ambient_shadow_y[6]   = {0.0f, 1.0f, 2.0f, 4.0f, 6.0f, 8.0f};
    float       ambient_shadow_blur[6]= {0.0f, 3.0f, 6.0f, 8.0f, 10.0f, 12.0f};
    float       ambient_shadow_spread[6]={0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f};
    float       key_shadow_opacity    = 0.30f;
    float       ambient_shadow_opacity= 0.15f;
    float       elevation_tint[6]     = {0.0f, 0.05f, 0.08f, 0.11f, 0.12f, 0.14f};

    // Motion: the expressive spring system, spatial + effects x fast/default/slow.
    float       spring_spatial_fast_damping     = 0.6f;
    float       spring_spatial_fast_stiffness   = 800.0f;
    float       spring_spatial_default_damping  = 0.8f;
    float       spring_spatial_default_stiffness= 380.0f;
    float       spring_spatial_slow_damping     = 0.8f;
    float       spring_spatial_slow_stiffness   = 200.0f;
    float       spring_effects_fast_damping     = 1.0f;
    float       spring_effects_fast_stiffness   = 3800.0f;
    float       spring_effects_default_damping  = 1.0f;
    float       spring_effects_default_stiffness= 1600.0f;
    float       spring_effects_slow_damping     = 1.0f;
    float       spring_effects_slow_stiffness   = 800.0f;

    // Legacy easing durations (ms). Kept for the curves below.
    float       duration_short1 = 50.0f;
    float       duration_short2 = 100.0f;
    float       duration_short3 = 150.0f;
    float       duration_short4 = 200.0f;
    float       duration_medium1 = 250.0f;
    float       duration_medium2 = 300.0f;
    float       duration_medium3 = 350.0f;
    float       duration_medium4 = 400.0f;
    float       duration_long1 = 450.0f;
    float       duration_long2 = 500.0f;
    float       duration_long3 = 550.0f;
    float       duration_long4 = 600.0f;
    float       duration_extra_long1 = 700.0f;
    float       duration_extra_long2 = 800.0f;
    float       duration_extra_long3 = 900.0f;
    float       duration_extra_long4 = 1000.0f;
};

// Overlay backdrop blur. The settings live in the theme so the editor and the
// `.colors` file own them end to end. `blur` is the global toggle, `size` the
// blur radius/spread and `passes` (1..10) the number of separable ping-pong
// rounds. `background_opacity` is how strongly the backdrop surface tints the
// frame behind the overlay; blur is skipped once it reaches 1.
struct ImGuiM3BlurSettings
{
    bool  blur = true;
    float background_opacity = 0.80f;
    float size = 5.0f;
    int   passes = 3;
};

// A background surface. The overlay shell owns the only one; its
// `BackgroundBlur` flag opts that surface into the shared backdrop blur,
// independently of the global toggle.
struct ImGuiM3BackgroundObject
{
    bool BackgroundBlur = true;
};

// Button flavours.

enum ImGuiM3ButtonVariant_
{
    ImGuiM3Button_Filled = 0,
    ImGuiM3Button_Tonal,
    ImGuiM3Button_Outlined,
    ImGuiM3Button_Elevated,
    ImGuiM3Button_Text,
    ImGuiM3Button_Destructive,
    ImGuiM3Button_DestructiveTonal,
    ImGuiM3Button_DestructiveOutlined,
    ImGuiM3Button_COUNT
};

typedef ImGuiM3ButtonVariant_ ImGuiM3ButtonVariant;

// role names double as `.colors` file keys and picker labels.

IMGUI_API const char* ImGuiM3RoleName(ImGuiM3Role role);
IMGUI_API ImGuiM3Role ImGuiM3RoleFromName(const char* name);   // ImGuiM3Role_COUNT when unknown.
IMGUI_API const char* ImGuiM3VariantName(ImGuiM3Variant variant);
IMGUI_API ImGuiM3Variant ImGuiM3VariantFromName(const char* name);
IMGUI_API const char* ImGuiM3ContrastName(ImGuiM3Contrast contrast);
IMGUI_API ImGuiM3Contrast ImGuiM3ContrastFromName(const char* name);

// Points the theme at a `.colors` file. Loads it if it exists, otherwise writes
// a populated preset. NULL falls back to the compiled-in preset with no file.
IMGUI_API bool        ImGuiM3SetThemeFile(const char* path);
IMGUI_API const char* ImGuiM3GetThemeFile();

// Default location: $VKINTOX_THEME, else $XDG_CONFIG_HOME/VKIntox/theme.colors,
// else $HOME/.config/VKIntox/theme.colors.
IMGUI_API const char* ImGuiM3DefaultThemeFilePath();

// The active palette. Valid before any theme file is loaded (compiled-in preset).
IMGUI_API const ImVec4&       ImGuiM3Color(ImGuiM3Role role);
IMGUI_API ImU32               ImGuiM3ColorU32(ImGuiM3Role role);
IMGUI_API ImVec4              ImGuiM3OnColor(ImGuiM3Role role);   // the matching "on" role
IMGUI_API ImU32               ImGuiM3OnColorU32(ImGuiM3Role role);
IMGUI_API ImVec4              ImGuiM3StateLayer(ImGuiM3Role role, ImGuiM3State state);
IMGUI_API ImU32               ImGuiM3StateLayerU32(ImGuiM3Role role, ImGuiM3State state);
IMGUI_API ImVec4              ImGuiM3Elevate(ImGuiM3Role role, int level);
IMGUI_API const ImGuiM3Metrics& ImGuiM3GetMetrics();

// Overlay backdrop blur settings and the single overlay background surface.
IMGUI_API ImGuiM3BlurSettings&     ImGuiM3GetBlurSettings();
IMGUI_API ImGuiM3BackgroundObject& ImGuiM3GetBackgroundObject();

// Named Google Sans faces, or NULL when the family was missing. Expressive leans
// on weight for emphasis, which a single face cannot express.
IMGUI_API ImFont*             ImGuiM3FontMedium();
IMGUI_API ImFont*             ImGuiM3FontBold();
IMGUI_API ImFont*             ImGuiM3FontExtraBold();
// Font indices are unstable with MergeMode entries, so faces arrive by pointer.
IMGUI_API void                ImGuiM3SetTextFonts(ImFont* regular, ImFont* medium, ImFont* bold, ImFont* extra_bold);

// Adds the Material Symbols font as its own atlas face; call before the atlas
// is built. ranges selects which codepoints to rasterise (null = all of the PUA).
IMGUI_API bool                ImGuiM3LoadIconFont(const char* path, float size_px, const ImWchar* ranges = nullptr);
// Merges the icon font into the last-added face for inline icon glyphs.
IMGUI_API bool                ImGuiM3MergeIconFont(const char* path, float size_px, const ImWchar* ranges = nullptr);
IMGUI_API ImFont*             ImGuiM3IconFont();
IMGUI_API bool                ImGuiM3IsDark();
IMGUI_API ImU32               ImGuiM3SourceColor();
IMGUI_API ImGuiM3Variant      ImGuiM3GetVariant();
IMGUI_API ImGuiM3Contrast     ImGuiM3GetContrast();

// Writes every resolved token to a `.colors` file; on any failure returns false
// and leaves the file untouched.
IMGUI_API bool        ImGuiM3WriteThemeFile(const char* path);

// re-derives every token and pushes the result into ImGuiStyle.
IMGUI_API void        ImGuiM3ApplyToStyle(float ui_scale);

// True on the frame the theme file changed and was re-read.
IMGUI_API bool        ImGuiM3ConsumeReloadedFlag();
// note about the last reload, or NULL. consumes the flag.
IMGUI_API const char* ImGuiM3ConsumeReloadMessage();
// Last parse error, or NULL.
IMGUI_API const char* ImGuiM3GetError();

// Shape + motion helpers for the M3 widget drawing.

// Per-corner radii; a single ImGuiStyle rounding cannot express M3's asymmetric
// tokens (corner-extra-small-top, corner-large-start, ...).
struct ImGuiM3ShapeRounding
{
    float tl, tr, br, bl;
};

// Spring state for a corner morph.
struct ImGuiM3Spring
{
    float value = 0.0f;
    float velocity = 0.0f;
};

// Integrates `id`'s spring toward `target` and returns the new value. `damping`
// is the M3 damping ratio, `stiffness` in rad/s.
IMGUI_API float ImGuiM3SpringStep(ImGuiID id, float target, float damping_ratio, float stiffness);

// M3 Expressive spatial/effects spring presets.
IMGUI_API float ImGuiM3SpringStepSpatialFast(ImGuiID id, float target);
IMGUI_API float ImGuiM3SpringStepSpatialDefault(ImGuiID id, float target);
IMGUI_API float ImGuiM3SpringStepSpatialSlow(ImGuiID id, float target);
IMGUI_API float ImGuiM3SpringStepEffectsFast(ImGuiID id, float target);
IMGUI_API float ImGuiM3SpringStepEffectsDefault(ImGuiID id, float target);
IMGUI_API float ImGuiM3SpringStepEffectsSlow(ImGuiID id, float target);
// Drops every live spring. Called at the end of each frame.
IMGUI_API void  ImGuiM3ClearSprings();

// Easing curves, as the normalized cubic beziers the spec publishes.
IMGUI_API float ImGuiM3EaseStandard(float t);                 // 0.2, 0, 0, 1
IMGUI_API float ImGuiM3EaseStandardDecelerate(float t);        // 0, 0, 0, 1
IMGUI_API float ImGuiM3EaseStandardAccelerate(float t);        // 0.3, 0, 1, 1
IMGUI_API float ImGuiM3EaseEmphasizedAccelerate(float t);     // 0.3, 0, 0.8, 0.15
IMGUI_API float ImGuiM3EaseEmphasizedDecelerate(float t);     // 0.05, 0.7, 0.1, 1
IMGUI_API float ImGuiM3EaseLegacy(float t);                   // 0.4, 0, 0.2, 1
// Spring-mimicking curves for interruption-free animations (web fallback).
IMGUI_API float ImGuiM3EaseExpressiveFastSpatial(float t);    // 0.42, 1.67, 0.21, 0.90
IMGUI_API float ImGuiM3EaseExpressiveDefaultSpatial(float t); // 0.38, 1.21, 0.22, 1
IMGUI_API float ImGuiM3EaseExpressiveSlowSpatial(float t);    // 0.39, 1.29, 0.35, 0.98
IMGUI_API float ImGuiM3EaseExpressiveFastEffects(float t);    // 0.31, 0.94, 0.34, 1
IMGUI_API float ImGuiM3EaseExpressiveDefaultEffects(float t); // 0.34, 0.80, 0.34, 1
IMGUI_API float ImGuiM3EaseExpressiveSlowEffects(float t);    // 0.34, 0.88, 0.34, 1

// Shape scale lookup, honouring density and the shape-scale knob.
IMGUI_API float ImGuiM3Radius(ImGuiM3Shape shape);
// Fully-rounded helper: min(half the shorter side, radius).
IMGUI_API float ImGuiM3PillRadius(ImVec2 size, float radius);
// Spring-animated morph from `radius` toward `pressed_radius` while pressed.
IMGUI_API ImGuiM3ShapeRounding ImGuiM3MorphedRounding(ImVec2 size, float radius, float pressed_radius, bool pressed, ImGuiID id);

// Connected button group geometry: pill outer corners, 8dp inner at size S;
// `index` is 0-based, `count` the segment count.
IMGUI_API float ImGuiM3ConnectedInnerRadius();
IMGUI_API ImGuiM3ShapeRounding ImGuiM3ConnectedSegmentRounding(ImVec2 size, int index, int count);

// Draws an M3 state layer over a rect: `role` at `state`'s opacity.
IMGUI_API void ImGuiM3DrawStateLayer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImGuiM3Role role, ImGuiM3State state);
// centres a lone Material Symbols glyph on its ink box, not the text line box.
IMGUI_API void ImGuiM3DrawIcon(ImDrawList* draw_list, const char* glyph, const ImRect& bb, float px, ImU32 col);
// ink-box centre as offsets from the layout position; false when the glyph will
// not resolve, so the caller can fall back.
IMGUI_API bool ImGuiM3IconInkCenterXY(ImFont* font, float px, const char* glyph, ImVec2& out_center);
// y only. ink centres at 0.5*size, the line box at 0.6*size.
IMGUI_API float ImGuiM3IconInkCenter(ImFont* font, float px, const char* glyph);
// Fill + optional outline, matching M3's container variants.
IMGUI_API void ImGuiM3DrawContainer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 fill, ImU32 outline, float outline_width);
// Two-layer M3 shadow (key + ambient) under a container.
IMGUI_API void ImGuiM3DrawElevation(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, int level);
// Rounded rect path helper honouring per-corner radii. Leaves the path filled.
IMGUI_API void ImGuiM3PathRoundedRect(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 col);
// Translucent backdrop, the M3 scrim at 32%.
IMGUI_API void ImGuiM3DrawScrim(ImDrawList* draw_list, const ImRect& bb);

// hooked from ImGui::NewFrame: reload on change, clear the spring table. not public API.
IMGUI_API void ImGuiM3NewFrame();
IMGUI_API void ImGuiM3Shutdown();

// surface-container role for `elevation`; 2025 M3 replaced tint-per-level with these.
IMGUI_API ImGuiM3Role ImGuiM3SurfaceContainerForElevation(ImGuiM3Role role, int elevation);

// M3 widgets layered on top of core ImGui.
namespace ImGui
{
    IMGUI_API bool M3Button(const char* label, ImGuiM3ButtonVariant variant, const ImVec2& size_arg = ImVec2(0, 0), bool strong_label = false);
    IMGUI_API bool M3IconButton(const char* glyph, const char* tooltip, ImGuiM3ButtonVariant variant = ImGuiM3Button_Text);
    IMGUI_API bool M3Switch(const char* label, bool* v);
    IMGUI_API bool M3SwitchWithID(const char* label, const char* id, bool* v);
    IMGUI_API bool M3Fab(const char* glyph, const char* tooltip, bool large = false);
    IMGUI_API void M3Icon(const char* glyph, ImVec2 size_arg = ImVec2(0, 0));
    IMGUI_API void M3SectionHeader(const char* label, const char* icon = NULL);
    IMGUI_API void M3Divider(ImGuiM3Role role = ImGuiM3Role_OutlineVariant);
    // titled content card: surface-container-low, sized to its contents.
    IMGUI_API void M3CardBegin(const char* id, const char* title = NULL, const char* icon = NULL);
    IMGUI_API void M3CardEnd();
    IMGUI_API void M3ListItem(const char* label, bool selected, bool* p_selected = NULL);
    IMGUI_API void M3LinearProgress(float fraction, const ImVec2& size_arg = ImVec2(-1, 0));
    IMGUI_API bool M3Chip(const char* label, bool* p_selected);
    IMGUI_API void M3StatusChip(const char* label, ImGuiM3Role role);
    // segments sharing one shape with 2dp padding; `icons` is one codepoint per
    // segment.
    IMGUI_API bool M3ConnectedButtonGroup(const char* id, const char* const* labels, int count, int* selected, const ImWchar* icons = NULL);
    // full theme editor: generation knobs, every token, and the file path with
    // reload status.
    IMGUI_API void M3ThemeEditor();
}

#endif // IMGUI_M3_H_INCLUDED