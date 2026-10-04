// Tests for AsyncWriter, the single background thread that keeps profile and
// settings writes off the present thread. FIFO order, error isolation and the
// idle barrier are the contract, so they are pinned directly.

#include "async_writer.hh"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using VKIntox::AsyncWriter;

namespace
{
    int g_failures = 0;
    int g_checks   = 0;

    void expect(bool condition, const std::string& what)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            std::printf("  FAILED: %s\n", what.c_str());
        }
    }

    void test_runs_every_job_in_order()
    {
        AsyncWriter writer;
        std::vector<int> order;

        for (int i = 0; i < 100; ++i)
            writer.submit([&order, i] { order.push_back(i); });

        writer.waitForIdle();
        expect(order.size() == 100, "every submitted job ran");
        bool ordered = true;
        for (int i = 0; i < static_cast<int>(order.size()); ++i)
            if (order[i] != i)
                ordered = false;
        expect(ordered, "jobs run FIFO");
    }

    void test_throwing_job_does_not_stop_the_queue()
    {
        AsyncWriter writer;
        std::atomic<int> ran{0};

        writer.submit([] { throw std::runtime_error("boom"); });
        writer.submit([&ran] { ran++; });
        writer.submit([&ran] { ran++; });

        writer.waitForIdle();
        expect(ran.load() == 2, "a throwing job does not poison the rest");
    }

    void test_null_job_is_ignored()
    {
        AsyncWriter writer;
        std::atomic<int> ran{0};

        writer.submit(nullptr);
        writer.submit([&ran] { ran++; });

        writer.waitForIdle();
        expect(ran.load() == 1, "a null job is dropped");
    }

    void test_idle_is_safe_without_any_job()
    {
        AsyncWriter writer;
        writer.waitForIdle();  // must return, not hang, when the thread never started
        expect(true, "waitForIdle on an unused writer returns");
    }

    void test_concurrent_submits_all_run()
    {
        AsyncWriter writer;
        std::mutex mutex;
        std::vector<int> seen;

        const int kThreads = 8;
        const int kPerThread = 500;
        std::vector<std::thread> submitters;
        submitters.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            submitters.emplace_back([&writer, &mutex, &seen, t] {
                for (int i = 0; i < kPerThread; ++i)
                {
                    writer.submit([&mutex, &seen, value = t * kPerThread + i] {
                        std::lock_guard<std::mutex> lock(mutex);
                        seen.push_back(value);
                    });
                }
            });
        }
        for (auto& s : submitters)
            s.join();

        writer.waitForIdle();
        expect(seen.size() == static_cast<size_t>(kThreads) * kPerThread,
               "every concurrently submitted job ran exactly once");
    }
} // namespace

int main()
{
    test_runs_every_job_in_order();
    test_throwing_job_does_not_stop_the_queue();
    test_null_job_is_ignored();
    test_idle_is_safe_without_any_job();
    test_concurrent_submits_all_run();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
