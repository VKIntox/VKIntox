#include "util.hh"

#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace VKIntox
{
    void addUniqueCString(std::vector<const char*>& stringVector, const char* addString)
    {
        for (const char* other : stringVector)
        {
            if (other == std::string(addString))
            {
                return;
            }
        }
        stringVector.push_back(addString);
    }

    void outputInColor(std::string output, Color foreground, Color background)
    {
        std::vector<std::string> magicNumbers;
        switch (foreground)
        {
            case Color::black: magicNumbers.push_back("30"); break;
            case Color::red: magicNumbers.push_back("31"); break;
            case Color::green: magicNumbers.push_back("32"); break;
            case Color::yellow: magicNumbers.push_back("33"); break;
            case Color::blue: magicNumbers.push_back("34"); break;
            case Color::magenta: magicNumbers.push_back("35"); break;
            case Color::cyan: magicNumbers.push_back("36"); break;
            case Color::white: magicNumbers.push_back("37"); break;
            default: break;
        }
        switch (background)
        {
            case Color::black: magicNumbers.push_back("40"); break;
            case Color::red: magicNumbers.push_back("41"); break;
            case Color::green: magicNumbers.push_back("42"); break;
            case Color::yellow: magicNumbers.push_back("43"); break;
            case Color::blue: magicNumbers.push_back("44"); break;
            case Color::magenta: magicNumbers.push_back("45"); break;
            case Color::cyan: magicNumbers.push_back("46"); break;
            case Color::white: magicNumbers.push_back("47"); break;
            default: break;
        }
        std::string magicString = "";
        for (bool first = true; auto& magicNumber : magicNumbers)
        {
            if (!first)
            {
                magicString += ";";
            }
            magicString += magicNumber;
            first = false;
        }
        if (magicString.size() == 0 || !isatty(fileno(stdout)))
        {
            std::cout << output << std::endl;
        }
        else
        {
            std::cout << "\033[" << magicString << "m" << output << "\033[0m" << std::endl;
        }
    }
    bool openInShell(const char* url)
    {
        if (url == nullptr || *url == '\0')
            return false;

        // a flatpak sandbox cannot reach the host's openers directly, so hand the
        // url to the host. everywhere else xdg-open is the standard opener.
        const bool flatpak = access("/.flatpak-info", F_OK) == 0;
        const char* args[5] = {};
        int arg = 0;
        if (flatpak)
        {
            args[arg++] = "flatpak-spawn";
            args[arg++] = "--host";
        }
        args[arg++] = "xdg-open";
        args[arg++] = url;

        // double-fork so the opener is detached from the game and cannot linger
        // as a zombie. only async-signal-safe calls run between fork and exec.
        const pid_t pid = fork();
        if (pid < 0)
            return false;
        if (pid == 0)
        {
            setsid();
            const pid_t grandchild = fork();
            if (grandchild == 0)
            {
                execvp(args[0], const_cast<char* const*>(args));
                _exit(127);
            }
            _exit(grandchild < 0 ? 127 : 0);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        return true;
    }
} // namespace VKIntox
