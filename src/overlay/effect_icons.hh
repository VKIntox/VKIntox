#ifndef EFFECT_ICONS_HPP_INCLUDED
#define EFFECT_ICONS_HPP_INCLUDED

#include <algorithm>
#include <cctype>
#include <string>

#include "overlay/ui_icons.hh"

namespace VKIntox
{
    // A visual hint about what an effect does, picked from its name. First match
    // wins, so the more specific hints come before the broad ones. Shared by the
    // add-effects list and the parameters sheet so both agree on an effect.
    inline ImWchar effectIconCodepointFor(const std::string& name)
    {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto has = [&](const char* needle) { return n.find(needle) != std::string::npos; };

        if (has("motionblur") || has("motion_blur") || has("motion blur")) return Icon::MotionBlur;
        if (has("sharpen") || has("sharp") || has("clarity"))             return Icon::DiamondShine;
        if (has("dof") || has("bokeh"))                                   return Icon::LensBlur;
        if (has("ssr"))                                                   return Icon::Gradient;
        if (has("bloom") || has("flare") || has("glow"))                  return Icon::Flare;
        if (has("lut") || has("color") || has("curve") || has("grade"))   return Icon::Colorize;
        if (has("blur"))                                                  return Icon::BlurOn;
        if (has("film") || has("grain") || has("noise") || has("deb"))    return Icon::Grain;
        if (has("cas") || has("aa"))                                      return Icon::Straighten;
        if (has("light"))                                                 return Icon::Bolt;
        if (has("tone") || has("contrast") || has("hdr"))                 return Icon::Contrast;
        if (has("ssao") || has("mxao"))                                   return Icon::Shadow;
        if (has("depth"))                                                 return Icon::Layers;
        if (has("shader") || has("fx") || has("effect"))                  return Icon::Brush;
        return Icon::Extension;
    }
}

#endif // EFFECT_ICONS_HPP_INCLUDED
