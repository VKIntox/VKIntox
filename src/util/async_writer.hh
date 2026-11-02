#ifndef ASYNC_WRITER_HPP_INCLUDED
#define ASYNC_WRITER_HPP_INCLUDED

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace VKIntox
{
    // Runs slow, order-dependent work (profile and settings writes) on one
    // background thread so the present thread never blocks on disk. Jobs run
    // FIFO; the thread is created on the first submit and drained+joined when
    // the process-level instance is destroyed.
    class AsyncWriter
    {
    public:
        AsyncWriter() = default;

        static AsyncWriter& instance();

        void submit(std::function<void()> job);

        // Blocks until every queued job has finished. For shutdown and tests.
        void waitForIdle();

        AsyncWriter(const AsyncWriter&) = delete;
        AsyncWriter& operator=(const AsyncWriter&) = delete;
        ~AsyncWriter();

    private:
        void run();

        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_done;
        std::deque<std::function<void()>> m_jobs;
        std::thread m_thread;
        bool m_started = false;
        bool m_stopping = false;
        size_t m_active = 0;
    };
} // namespace VKIntox

#endif // ASYNC_WRITER_HPP_INCLUDED
