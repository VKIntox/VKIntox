// A missing hold here ends as a compositor-killed wayland connection at
// runtime, so the present/commit ordering is driven directly with threads.
#include "wayland_syncobj_gate.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace VKIntox::WaylandSyncobjGate;
using Clock = std::chrono::steady_clock;

namespace
{
    int failures = 0;

    void check(bool ok, const char* what, int line)
    {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "FAIL:%d: %s\n", line, what);
        }
    }

    uint64_t microsSince(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    }
} // namespace

#define CHECK(cond) check((cond), #cond, __LINE__)

int main()
{
    constexpr uint32_t SURFACE = 43;

    // no present open: commits pass straight through
    {
        resetForTests();
        auto start = Clock::now();
        beginCommit(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
    }

    // the presenter's own commit inside its present is never held
    {
        resetForTests();
        beginPresent(SURFACE);
        auto start = Clock::now();
        beginCommit(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
        endPresent(SURFACE);
    }

    // a foreign commit waits until the presenter closes the bracket
    {
        resetForTests();
        std::atomic<bool> presentOpen{false};
        std::atomic<bool> presentFinished{false};
        std::atomic<uint64_t> foreignWait{0};

        std::thread presenter([&] {
            beginPresent(SURFACE);
            presentOpen.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            presentFinished.store(true);
            endPresent(SURFACE);
        });

        while (!presentOpen.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::thread foreign([&] {
            auto start = Clock::now();
            beginCommit(SURFACE);
            foreignWait.store(microsSince(start));
            CHECK(presentFinished.load()); // released only after the bracket closed
        });

        presenter.join();
        foreign.join();

        CHECK(foreignWait.load() >= 25000);
        CHECK(heldCount() == 1);

        // and once closed, commits pass instantly
        auto start = Clock::now();
        beginCommit(SURFACE);
        CHECK(microsSince(start) < 5000);
    }

    // endPresent from another thread must not close the bracket
    {
        resetForTests();
        setWaitBudgetForTests(20000);
        std::atomic<uint64_t> waited{0};
        std::atomic<bool> presentOpen{false};
        std::atomic<bool> finish{false};

        std::thread presenter([&] {
            beginPresent(SURFACE);
            presentOpen.store(true);
            while (!finish.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            endPresent(SURFACE);
        });

        while (!presentOpen.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        endPresent(SURFACE); // different thread: ignored

        std::thread foreign([&] {
            auto start = Clock::now();
            beginCommit(SURFACE);
            waited.store(microsSince(start));
        });
        foreign.join();
        CHECK(waited.load() >= 18000);

        finish.store(true);
        presenter.join();
        resetForTests();
    }

    // bound: a present that never closes cannot park commits forever
    {
        resetForTests();
        setWaitBudgetForTests(20000);
        beginPresent(SURFACE); // opened by this thread, never ended

        std::atomic<uint64_t> first{0};
        std::atomic<uint64_t> second{0};

        std::thread a([&] {
            auto start = Clock::now();
            beginCommit(SURFACE);
            first.store(microsSince(start));
        });
        a.join();

        std::thread b([&] {
            auto start = Clock::now();
            beginCommit(SURFACE);
            second.store(microsSince(start));
        });
        b.join();

        CHECK(first.load() >= 18000);
        CHECK(first.load() < 500000);
        CHECK(second.load() >= 18000);
        CHECK(second.load() < 500000);
        CHECK(heldCount() == 2);

        resetForTests();
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
