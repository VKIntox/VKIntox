// A missing hold in the explicit-sync gate ends as a compositor-killed wayland
// connection at runtime, so the hold-and-release ordering is driven directly
// with threads here.
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
    constexpr uint32_t SYNCOBJ = 76;

    // commits on untracked surfaces never make the gate do anything
    {
        resetForTests();
        auto start = Clock::now();
        beginCommit(SURFACE);
        endCommit(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
    }

    // the presenter's own sequence passes straight through and closes
    {
        resetForTests();
        noteSyncobjSurface(SYNCOBJ, SURFACE);
        notePointSetForSyncobj(SYNCOBJ);
        auto start = Clock::now();
        beginCommit(SURFACE);
        endCommit(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);

        std::atomic<uint64_t> later{0};
        std::thread foreign([&] {
            auto t = Clock::now();
            beginCommit(SURFACE);
            later.store(microsSince(t));
            endCommit(SURFACE);
        });
        foreign.join();
        CHECK(later.load() < 5000);
        CHECK(heldCount() == 0);
    }

    // a foreign commit waits until the presenter's endCommit, not its
    // beginCommit: the marshal sits between the two
    {
        resetForTests();
        noteSyncobjSurface(SYNCOBJ, SURFACE);
        std::atomic<bool> presenterMarshaled{false};
        std::atomic<uint64_t> foreignWait{0};

        std::thread presenter([&] {
            notePointSetForSyncobj(SYNCOBJ);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            beginCommit(SURFACE);
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // marshal window
            presenterMarshaled.store(true);
            endCommit(SURFACE);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::thread foreign([&] {
            auto t = Clock::now();
            beginCommit(SURFACE);
            foreignWait.store(microsSince(t));
            endCommit(SURFACE);
        });

        presenter.join();
        foreign.join();

        CHECK(foreignWait.load() >= 25000); // held across the whole sequence
        CHECK(presenterMarshaled.load());   // and until the marshal finished
        CHECK(heldCount() == 1);
    }

    // a presenter that never commits must not park later commits forever
    {
        resetForTests();
        setWaitBudgetForTests(20000);
        noteSyncobjSurface(SYNCOBJ, SURFACE);
        notePointSetForSyncobj(SYNCOBJ);

        std::atomic<uint64_t> waited{0};
        std::thread foreign([&] {
            auto t = Clock::now();
            beginCommit(SURFACE);
            waited.store(microsSince(t));
            endCommit(SURFACE);
        });
        foreign.join();

        CHECK(waited.load() >= 18000);
        CHECK(waited.load() < 500000);
        CHECK(heldCount() == 1);

        // the stuck sequence was dropped: the next commit passes instantly
        std::atomic<uint64_t> second{0};
        std::thread again([&] {
            auto t = Clock::now();
            beginCommit(SURFACE);
            second.store(microsSince(t));
            endCommit(SURFACE);
        });
        again.join();
        CHECK(second.load() < 5000);
    }

    // destroying the syncobj drops the pairing and any open sequence
    {
        resetForTests();
        noteSyncobjSurface(SYNCOBJ, SURFACE);
        notePointSetForSyncobj(SYNCOBJ);
        noteSyncobjDestroyed(SYNCOBJ);

        std::atomic<uint64_t> waited{0};
        std::thread foreign([&] {
            auto t = Clock::now();
            beginCommit(SURFACE);
            waited.store(microsSince(t));
            endCommit(SURFACE);
        });
        foreign.join();
        CHECK(waited.load() < 5000);
        CHECK(heldCount() == 0);

        // point sets on the dead pairing no longer arm anything
        notePointSetForSyncobj(SYNCOBJ);
        std::thread stale([&] {
            beginCommit(SURFACE);
            endCommit(SURFACE);
        });
        stale.join();
        CHECK(heldCount() == 0);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
