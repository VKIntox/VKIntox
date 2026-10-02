//-----------------------------------------------------------------------------
// Material 3 Expressive theming for Dear ImGui.
//
// This header is the public surface of VKIntox's M3 Expressive skin. It lives in
// the vendored ImGui tree on purpose: the theme owns widget drawing, so it has to
// sit next to imgui_widgets.cc / imgui_draw.cc rather than in a wrapper library
// that can only restyle ImGuiCol_* after the fact.
//
// Three things live here:
//   1. The complete Material 3 color role set, plus shape / state / motion tokens.
//   2. A TonalPalette + HCT solver, so a single seed colour produces the whole
//      scheme exactly the way Matugen and material-color-utilities do.
//   3. A `.colors` file listing every color token, with live reload.
//
// The file is plain `key = value`, Matugen flavoured:
//
//     # VKIntox theme tokens.
//     variant  = dark
//     source   = #6750A4
//     primary  = #CFBCFF
//     ...
//
// Anything set explicitly wins; roles you leave out are derived from `source`, so
// a one-line file still produces the full scheme.
//-----------------------------------------------------------------------------

#ifndef IMGUI_M3_H_INCLUDED
#define IMGUI_M3_H_INCLUDED

#include "imgui.h"

// ImRect is declared in imgui_internal.h, which includes this header, so all we
// can do here is forward declare it. Every use below is a const reference.
struct ImRect;

//-----------------------------------------------------------------------------
// Color roles. The complete M3 role set as of the 2025 Expressive revision, in
// spec order.
//-----------------------------------------------------------------------------

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

// Scheme variants. EXPRESSIVE is the M3 Expressive default; the rest mirror
// Matugen's `--type` values so a Matugen config maps straight across.
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

// Contrast levels, added May 2025. STANDARD / MEDIUM / HIGH.
enum ImGuiM3Contrast_
{
    ImGuiM3Contrast_Standard = 0,
    ImGuiM3Contrast_Medium,
    ImGuiM3Contrast_High,
    ImGuiM3Contrast_COUNT
};

typedef ImGuiM3Contrast_ ImGuiM3Contrast;

//-----------------------------------------------------------------------------
// Shape scale. Radii are dp and get scaled by the density factor. Expressive
// added three steps on top of the classic six.
//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------
// Interaction states. Opacities are the M3 state layer table: hover 8%, focus
// 10%, press 10%, drag 16%. There is no "selected" state layer — selection is a
// container colour swap — but we keep the slot so callers can ask for it.
//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------
// Non-color tokens. These live in the `.colors` file under an `[expressive]`
// section so the file stays a single flat token sheet.
//-----------------------------------------------------------------------------

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
    float       switch_handle_with_icon = 24.0f;
    float       switch_pressed_track_width = 40.0f;
    float       switch_pressed_handle = 28.0f;

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

    // Elevation. Shadow geometry per level 0-5; surface tint is deprecated in the
    // 2025 spec in favour of the surface container roles, so we only track the
    // tint opacities for legacy call sites.
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

//-----------------------------------------------------------------------------
// Button flavours.
//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------
// Role names. Also used for the `.colors` file keys and for the colour picker UI.
//-----------------------------------------------------------------------------

IMGUI_API const char* ImGuiM3RoleName(ImGuiM3Role role);
IMGUI_API ImGuiM3Role ImGuiM3RoleFromName(const char* name);   // ImGuiM3Role_COUNT when unknown.
IMGUI_API const char* ImGuiM3VariantName(ImGuiM3Variant variant);
IMGUI_API ImGuiM3Variant ImGuiM3VariantFromName(const char* name);
IMGUI_API const char* ImGuiM3ContrastName(ImGuiM3Contrast contrast);
IMGUI_API ImGuiM3Contrast ImGuiM3ContrastFromName(const char* name);

//-----------------------------------------------------------------------------
// Theme lifetime.
//-----------------------------------------------------------------------------

// Points the theme at a `.colors` file. Loads it if it exists; writes a fully
// populated preset file if it does not. Pass NULL to go back to the compiled-in
// preset with no file involved.
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

// Named faces from the loaded Google Sans family, or NULL when the family was
// not found. Index 0 is always the regular face, so body text needs no lookup.
// M3 expressive leans on weight to signal emphasis (selected nav, titles),
// which a single face cannot express.
IMGUI_API ImFont*             ImGuiM3FontMedium();
IMGUI_API ImFont*             ImGuiM3FontBold();
IMGUI_API ImFont*             ImGuiM3FontExtraBold();
// Registers the named text faces by pointer. MergeMode font entries make
// ImGui's font indices unstable, so the overlay hands over the actual faces.
IMGUI_API void                ImGuiM3SetTextFonts(ImFont* regular, ImFont* medium, ImFont* bold, ImFont* extra_bold);

// Loads a Material Symbols subset as its own atlas face so icons can be drawn
// at any size. Call once before the atlas is built (overlay construction).
IMGUI_API bool                ImGuiM3LoadIconFont(const char* path, float size_px);
// Merges the icon subset into the face most recently added, so icon codepoints
// render inline in text labels at the text size.
IMGUI_API bool                ImGuiM3MergeIconFont(const char* path, float size_px);
IMGUI_API ImFont*             ImGuiM3IconFont();
IMGUI_API bool                ImGuiM3IsDark();
IMGUI_API ImU32               ImGuiM3SourceColor();
IMGUI_API ImGuiM3Variant      ImGuiM3GetVariant();
IMGUI_API ImGuiM3Contrast     ImGuiM3GetContrast();

// Writes the resolved theme as a `.colors` file, every token on its own line.
// Returns false and leaves the file untouched on any write failure.
IMGUI_API bool        ImGuiM3WriteThemeFile(const char* path);

// Re-derives every token from source / variant / contrast / explicit overrides and
// pushes the result into ImGuiStyle.
IMGUI_API void        ImGuiM3ApplyToStyle(float ui_scale);

// True on the frame the theme file changed and was re-read.
IMGUI_API bool        ImGuiM3ConsumeReloadedFlag();
// Human-readable note about the last reload ("reloaded 44 tokens from ..."), or
// NULL. Also consumes the flag.
IMGUI_API const char* ImGuiM3ConsumeReloadMessage();
// Last parse error, or NULL.
IMGUI_API const char* ImGuiM3GetError();

//-----------------------------------------------------------------------------
// Shape + motion helpers used by the M3 widget drawing in imgui_widgets.cc.
//-----------------------------------------------------------------------------

// Per-corner radii. M3 Expressive shapes morph per corner and defines
// asymmetric tokens (corner-extra-small-top, corner-large-start, ...), which a
// single ImGuiStyle rounding value cannot express.
struct ImGuiM3ShapeRounding
{
    float tl, tr, br, bl;
};

// A spring. M3 Expressive's signature move is that a container's corners morph
// when it is pressed; this drives that.
struct ImGuiM3Spring
{
    float value = 0.0f;
    float velocity = 0.0f;
};

// Integrates the spring for `id` toward `target` and returns its new value.
// Springs are keyed by ImGuiID so two buttons never share animation state.
// `damping` is the M3 damping *ratio*, `stiffness` is rad/s.
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
// The M3 Expressive morph: pressed corners pull in toward a smaller radius,
// animated by a spring. `pressed_radius` is the resting radius of that state.
IMGUI_API ImGuiM3ShapeRounding ImGuiM3MorphedRounding(ImVec2 size, float radius, float pressed_radius, bool pressed, ImGuiID id);

// Connected button group geometry (the expressive replacement for the
// segmented button). The group's outer corners are `corner-full` so the ends
// read as pills, while the inner corners stay modest — 8dp at size S, per
// md.comp.button-group.connected. `index` is 0-based, `count` the segment count.
IMGUI_API float ImGuiM3ConnectedInnerRadius();
IMGUI_API ImGuiM3ShapeRounding ImGuiM3ConnectedSegmentRounding(ImVec2 size, int index, int count);

// Draws an M3 state layer over a rect: `role` at `state`'s opacity.
IMGUI_API void ImGuiM3DrawStateLayer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImGuiM3Role role, ImGuiM3State state);
// Draws a Material Symbols glyph so its design box (not the taller text line
// box) is centred in `bb`. Use for standalone icons; icon+label runs should
// share a baseline instead.
IMGUI_API void ImGuiM3DrawIcon(ImDrawList* draw_list, const char* glyph, const ImRect& bb, float px, ImU32 col);
// Ink-rect centre of a Material Symbols glyph on both axes, as offsets from the
// text layout position. Returns false when the glyph can't be resolved so the
// caller can fall back to advance/line-box centring.
IMGUI_API bool ImGuiM3IconInkCenterXY(ImFont* font, float px, const char* glyph, ImVec2& out_center);
// Vertical distance (px) from the text draw position to the glyph's ink centre.
// Material Symbols glyphs are centred at 0.5*size, while the font's line box
// centres at 0.6*size, so line-box centring leaves icons 0.1*size too high.
IMGUI_API float ImGuiM3IconInkCenter(ImFont* font, float px, const char* glyph);
// Fill + optional outline, matching M3's container variants.
IMGUI_API void ImGuiM3DrawContainer(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 fill, ImU32 outline, float outline_width);
// Two-layer M3 shadow (key + ambient) under a container.
IMGUI_API void ImGuiM3DrawElevation(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, int level);
// Rounded rect path helper honouring per-corner radii. Leaves the path filled.
IMGUI_API void ImGuiM3PathRoundedRect(ImDrawList* draw_list, const ImRect& bb, ImGuiM3ShapeRounding rounding, ImU32 col);
// Translucent backdrop, the M3 scrim at 32%.
IMGUI_API void ImGuiM3DrawScrim(ImDrawList* draw_list, const ImRect& bb);

// Called from ImGui::NewFrame(): reloads the theme file if it changed and clears
// the spring table. Not public API.
IMGUI_API void ImGuiM3NewFrame();
IMGUI_API void ImGuiM3Shutdown();

// Picks the surface-container role that best expresses `elevation` on top of
// `role`. M3 dropped tint-per-level in 2025 in favour of these roles.
IMGUI_API ImGuiM3Role ImGuiM3SurfaceContainerForElevation(ImGuiM3Role role, int elevation);

//-----------------------------------------------------------------------------
// M3 widgets layered on top of core ImGui.
//-----------------------------------------------------------------------------

namespace ImGui
{
    IMGUI_API bool M3Button(const char* label, ImGuiM3ButtonVariant variant, const ImVec2& size_arg = ImVec2(0, 0));
    IMGUI_API bool M3IconButton(const char* glyph, const char* tooltip, ImGuiM3ButtonVariant variant = ImGuiM3Button_Text);
    IMGUI_API bool M3Switch(const char* label, bool* v);
    IMGUI_API bool M3SwitchWithID(const char* label, const char* id, bool* v);
    IMGUI_API bool M3Fab(const char* glyph, const char* tooltip, bool large = false);
    IMGUI_API void M3Icon(const char* glyph, ImVec2 size_arg = ImVec2(0, 0));
    IMGUI_API void M3SectionHeader(const char* label, const char* icon = NULL);
    IMGUI_API void M3Divider(ImGuiM3Role role = ImGuiM3Role_OutlineVariant);
    // A titled content card: rounded surface-container-low container that grows
    // with its contents, with an optional section header. Use for grouping
    // related controls so tabs read as a stack of coherent blocks.
    IMGUI_API void M3CardBegin(const char* id, const char* title = NULL, const char* icon = NULL);
    IMGUI_API void M3CardEnd();
    IMGUI_API void M3ListItem(const char* label, bool selected, bool* p_selected = NULL);
    IMGUI_API void M3LinearProgress(float fraction, const ImVec2& size_arg = ImVec2(-1, 0));
    IMGUI_API bool M3Chip(const char* label, bool* p_selected);
    IMGUI_API void M3StatusChip(const char* label, ImGuiM3Role role);
    // Connected button group: a row of related segments that share a shape.
    // Outer corners are fully rounded, inner corners are modest, and segments
    // are separated by the M3 2dp connected padding. Writes the chosen index
    // through `selected` and returns true when the selection changed. When
    // `icons` is non-null each segment leads with its Material Symbols
    // codepoint, falling back to labels only if the row cannot fit them.
    IMGUI_API bool M3ConnectedButtonGroup(const char* id, const char* const* labels, int count, int* selected, const ImWchar* icons = NULL);
    // A full theme editor: variant / contrast / source picker, every color token,
    // the shape + motion token sheet, and the `.colors` file path with live-reload
    // status. Meant to live in a settings tab.
    IMGUI_API void M3ThemeEditor();
}

#endif // IMGUI_M3_H_INCLUDED