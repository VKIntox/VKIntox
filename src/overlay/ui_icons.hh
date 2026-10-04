#ifndef UI_ICONS_HPP_INCLUDED
#define UI_ICONS_HPP_INCLUDED

// Codepoints for the Material Symbols Rounded face shipped in
// assets/font/MaterialSymbolsRounded.ttf.
//
// The shipped file is the upstream variable font instanced to a static regular
// (wght=400 FILL=0 GRAD=0 opsz=24), so it carries every icon. The overlay loads
// it as its own ImGui font but rasterises only the codepoints listed below,
// because building all 4000+ Private Use Area glyphs into the atlas would cost
// tens of MB and a slow startup. Add a name here and to GlyphRanges when a new
// icon is needed; no subset has to be regenerated.
//
//   fonttools varLib.instancer 'MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf' \
//       wght=400 FILL=0 GRAD=0 opsz=24 -o assets/font/MaterialSymbolsRounded.ttf

#include "vendor/imgui/imgui_m3.h"

namespace VKIntox::Icon
{
    // Navigation.
    constexpr ImWchar AutoAwesome    = 0xE65F;  // Effects
    constexpr ImWchar Palette        = 0xE3B7;  // Shaders
    constexpr ImWchar Settings       = 0xE8B8;  // Settings
    constexpr ImWchar Tune           = 0xE429;  // Advanced
    constexpr ImWchar MonitorHeart   = 0xEAA2;  // Diagnostics
    constexpr ImWchar BugReport      = 0xE868;  // Debug

    // Actions.
    constexpr ImWchar Add            = 0xE145;
    constexpr ImWchar Close          = 0xE14C;
    constexpr ImWchar Search         = 0xE8B6;
    constexpr ImWchar Delete         = 0xE872;
    constexpr ImWchar ExpandMore     = 0xE5CF;
    constexpr ImWchar ChevronRight   = 0xE409;
    constexpr ImWchar Refresh        = 0xE5D5;
    constexpr ImWchar Save           = 0xE161;
    constexpr ImWchar FolderOpen     = 0xE2C8;
    constexpr ImWchar Upload         = 0xE2C6;  // Import preset
    constexpr ImWchar Check          = 0xE5CA;
    constexpr ImWchar CheckCircle    = 0xE86C;
    constexpr ImWchar Cancel         = 0xE5C9;
    constexpr ImWchar Warning        = 0xE002;
    constexpr ImWchar Error          = 0xE000;
    constexpr ImWchar Info           = 0xE88E;
    constexpr ImWchar Visibility     = 0xE417;
    constexpr ImWchar VisibilityOff  = 0xE8F5;
    constexpr ImWchar Power          = 0xE8AC;
    constexpr ImWchar PlayArrow      = 0xE037;

    // Content / effect kinds.
    constexpr ImWchar Speed          = 0xE9E4;
    constexpr ImWchar Bolt           = 0xEA0B;
    constexpr ImWchar Science        = 0xEA4B;
    constexpr ImWchar Build          = 0xE869;
    constexpr ImWchar Extension      = 0xE87B;
    constexpr ImWchar Brush          = 0xE3AE;
    constexpr ImWchar Image          = 0xE251;
    constexpr ImWchar Layers         = 0xE53B;
    constexpr ImWchar Texture        = 0xE421;
    constexpr ImWchar BlurOn         = 0xE3A5;
    constexpr ImWchar Grain          = 0xE3EA;
    constexpr ImWchar Colorize       = 0xE3B8;
    constexpr ImWchar Contrast       = 0xEB37;
    constexpr ImWchar Straighten     = 0xE41C;
    constexpr ImWchar Memory         = 0xE322;
    constexpr ImWchar Terminal       = 0xEB8E;
    constexpr ImWchar Code           = 0xE86F;
    constexpr ImWchar Movie          = 0xE02C;
    constexpr ImWchar VideoSettings  = 0xEA75;
    constexpr ImWchar List           = 0xE896;
    constexpr ImWchar GridView       = 0xE9B0;
    constexpr ImWchar DragIndicator  = 0xE945;
    constexpr ImWchar MoreVert       = 0xE5D4;
    constexpr ImWchar FilterAlt      = 0xEF4F;
    constexpr ImWchar Star           = 0xE838;
    constexpr ImWchar DarkMode       = 0xE51C;
    constexpr ImWchar LightMode      = 0xE518;

    // UTF-8 encodings of the same glyphs, for embedding directly in labels once
    // the icon face is merged into the text faces ("\uE8B8 Settings").
    constexpr const char* AutoAwesomeUtf8   = "\xEE\x99\x9F";
    constexpr const char* PaletteUtf8       = "\xEE\x8E\xB7";
    constexpr const char* SettingsUtf8      = "\xEE\xA2\xB8";
    constexpr const char* TuneUtf8          = "\xEE\x90\xA9";
    constexpr const char* MonitorHeartUtf8  = "\xEE\xAA\xA2";
    constexpr const char* BugReportUtf8     = "\xEE\xA1\xA8";
    constexpr const char* AddUtf8           = "\xEE\x85\x85";
    constexpr const char* CloseUtf8         = "\xEE\x85\x8C";
    constexpr const char* SearchUtf8        = "\xEE\xA2\xB6";
    constexpr const char* DeleteUtf8        = "\xEE\xA1\xB2";
    constexpr const char* ExpandMoreUtf8    = "\xEE\x97\x8F";
    constexpr const char* ChevronRightUtf8  = "\xEE\x90\x89";
    constexpr const char* RefreshUtf8       = "\xEE\x97\x95";
    constexpr const char* SaveUtf8          = "\xEE\x85\xA1";
    constexpr const char* FolderOpenUtf8    = "\xEE\x8B\x88";
    constexpr const char* UploadUtf8        = "\xEE\x8B\x86";
    constexpr const char* CheckUtf8         = "\xEE\x97\x8A";
    constexpr const char* CheckCircleUtf8   = "\xEE\xA1\xAC";
    constexpr const char* CancelUtf8        = "\xEE\x97\x89";
    constexpr const char* WarningUtf8       = "\xEE\x80\x82";
    constexpr const char* ErrorUtf8         = "\xEE\x80\x80";
    constexpr const char* InfoUtf8          = "\xEE\xA2\x8E";
    constexpr const char* VisibilityUtf8    = "\xEE\x90\x97";
    constexpr const char* VisibilityOffUtf8 = "\xEE\xA3\xB5";
    constexpr const char* PowerUtf8         = "\xEE\xA2\xAC";
    constexpr const char* PlayArrowUtf8     = "\xEE\x80\xB7";
    constexpr const char* SpeedUtf8         = "\xEE\xA7\xA4";
    constexpr const char* BoltUtf8          = "\xEE\xA8\x8B";
    constexpr const char* ScienceUtf8       = "\xEE\xA9\x8B";
    constexpr const char* BuildUtf8         = "\xEE\xA1\xA9";
    constexpr const char* ExtensionUtf8     = "\xEE\xA1\xBB";
    constexpr const char* BrushUtf8         = "\xEE\x8E\xAE";
    constexpr const char* ImageUtf8         = "\xEE\x89\x91";
    constexpr const char* LayersUtf8        = "\xEE\x94\xBB";
    constexpr const char* TextureUtf8       = "\xEE\x90\xA1";
    constexpr const char* BlurOnUtf8        = "\xEE\x8E\xA5";
    constexpr const char* GrainUtf8         = "\xEE\x8F\xAA";
    constexpr const char* ColorizeUtf8      = "\xEE\x8E\xB8";
    constexpr const char* ContrastUtf8      = "\xEE\xAC\xB7";
    constexpr const char* StraightenUtf8    = "\xEE\x90\x9C";
    constexpr const char* MemoryUtf8        = "\xEE\x8C\xA2";
    constexpr const char* TerminalUtf8      = "\xEE\xAE\x8E";
    constexpr const char* CodeUtf8          = "\xEE\xA1\xAF";
    constexpr const char* MovieUtf8         = "\xEE\x80\xAC";
    constexpr const char* VideoSettingsUtf8 = "\xEE\xA9\xB5";
    constexpr const char* ListUtf8          = "\xEE\xA2\x96";
    constexpr const char* GridViewUtf8      = "\xEE\xA6\xB0";
    constexpr const char* DragIndicatorUtf8 = "\xEE\xA5\x85";
    constexpr const char* MoreVertUtf8      = "\xEE\x97\x94";
    constexpr const char* FilterAltUtf8     = "\xEE\xBD\x8F";
    constexpr const char* StarUtf8          = "\xEE\xA0\xB8";
    constexpr const char* DarkModeUtf8      = "\xEE\x94\x9C";
    constexpr const char* LightModeUtf8     = "\xEE\x94\x98";

    // Codepoints the icon face should rasterise. Must list every Icon used in
    // the overlay; it is the glyph range handed to the font loader.
    inline const ImWchar* GlyphRanges()
    {
        static const ImWchar ranges[] = {
            AutoAwesome, Palette, Settings, Tune, MonitorHeart, BugReport,
            Add, Close, Search, Delete, ExpandMore, ChevronRight, Refresh, Save,
            FolderOpen, Upload, Check, CheckCircle, Cancel, Warning, Error, Info,
            Visibility, VisibilityOff, Power, PlayArrow,
            Speed, Bolt, Science, Build, Extension, Brush, Image, Layers, Texture,
            BlurOn, Grain, Colorize, Contrast, Straighten, Memory, Terminal, Code,
            Movie, VideoSettings, List, GridView, DragIndicator, MoreVert, FilterAlt,
            Star, DarkMode, LightMode,
            0,
        };
        return ranges;
    }
}

#endif // UI_ICONS_HPP_INCLUDED
