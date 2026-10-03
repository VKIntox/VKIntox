#ifndef UTIL_HPP_INCLUDED
#define UTIL_HPP_INCLUDED

#include <string>
#include <sstream>
#include <vector>

namespace VKIntox
{
    void addUniqueCString(std::vector<const char*>& stringVector, const char* addString);

    // Opens a file or url with the system handler: xdg-open normally, or
    // `flatpak-spawn --host xdg-open` inside a flatpak sandbox. never runs a
    // shell, so a url can't smuggle in extra arguments.
    bool openInShell(const char* url);

    // Writes contents to a sibling temp file and renames it over path, so a
    // crash mid-write leaves the previous file intact instead of truncated.
    bool writeAtomically(const std::string& path, const std::string& contents);

    enum class Color
    {
        defaultColor,

        black,
        red,
        green,
        yellow,
        blue,
        magenta,
        cyan,
        white
    };

    void outputInColor(std::string output, Color foreground = Color::defaultColor, Color background = Color::defaultColor);

    template<typename T>
    std::string convertToString(T object)
    {
        std::stringstream ss;
        ss << object;
        return ss.str();
    }
} // namespace VKIntox

#endif // UTIL_HPP_INCLUDED
