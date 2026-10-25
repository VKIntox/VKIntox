#include <vulkan/vulkan.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>

namespace
{
    volatile sig_atomic_t deliveries[NSIG]{};
    volatile sig_atomic_t invalidContext = 0;

    void priorHandler(int signal)
    {
        deliveries[signal] = deliveries[signal] + 1;
        sigset_t mask;
        sigprocmask(SIG_SETMASK, nullptr, &mask);
        if (!sigismember(&mask, SIGUSR1) || sigismember(&mask, signal))
            invalidContext = 1;
    }

    void priorInfoHandler(int signal, siginfo_t* info, void* context)
    {
        if (!info || info->si_signo != signal || !context)
            invalidContext = 1;
        priorHandler(signal);
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const bool ignored = std::strcmp(argv[2], "ignored") == 0;
    const bool siginfo = std::strcmp(argv[2], "siginfo") == 0;
    const int signals[] = {SIGSEGV, SIGFPE, SIGABRT};
    for (int signal : signals)
    {
        struct sigaction original{};
        original.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESTART;
        sigemptyset(&original.sa_mask);
        sigaddset(&original.sa_mask, SIGUSR1);
        if (ignored)
            original.sa_handler = SIG_IGN;
        else if (siginfo)
        {
            original.sa_sigaction = priorInfoHandler;
            original.sa_flags |= SA_SIGINFO;
        }
        else
            original.sa_handler = priorHandler;
        if (sigaction(signal, &original, nullptr) != 0)
            return 1;
    }

    char configDirectory[] = "/tmp/vkintox-signal-chain-XXXXXX";
    if (!mkdtemp(configDirectory))
        return 1;
    setenv("XDG_CONFIG_HOME", configDirectory, 1);
    setenv("VKINTOX_LOG_LEVEL", "debug", 1);
    setenv("ENABLE_VKINTOX", "1", 1);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto getter = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "VKIntox_GetInstanceProcAddr"));
    if (!getter || !getter(VK_NULL_HANDLE, "vkGetInstanceProcAddr"))
        return 1;
    for (int signal : signals)
    {
        struct sigaction installed{};
        sigaction(signal, nullptr, &installed);
        if ((installed.sa_flags & (SA_NODEFER | SA_ONSTACK | SA_RESTART)) !=
                (SA_NODEFER | SA_ONSTACK | SA_RESTART) ||
            (installed.sa_flags & SA_RESETHAND) || !sigismember(&installed.sa_mask, SIGUSR1))
        {
            std::fprintf(stderr, "Signal %d lost the application's mask/flags\n", signal);
            return 1;
        }
        raise(signal);
        raise(signal);
        if (deliveries[signal] != (ignored ? 0 : 2) || invalidContext)
        {
            std::fprintf(stderr, "Signal %d was not forwarded twice with its original context\n", signal);
            return 1;
        }
    }
    std::filesystem::remove_all(configDirectory);
    return 0;
}
