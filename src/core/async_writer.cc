#include "async_writer.hh"

#include "logger.hh"

namespace VKIntox
{
    AsyncWriter& AsyncWriter::instance()
    {
        static AsyncWriter writer;
        return writer;
    }

    AsyncWriter::~AsyncWriter()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    void AsyncWriter::submit(std::function<void()> job)
    {
        if (!job)
            return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping)
                return;
            if (!m_started)
            {
                m_started = true;
                m_thread = std::thread(&AsyncWriter::run, this);
            }
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_one();
    }

    void AsyncWriter::run()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });

                if (m_jobs.empty())
                    return;  // stopping and drained

                job = std::move(m_jobs.front());
                m_jobs.pop_front();
                m_active++;
            }

            try
            {
                job();
            }
            catch (const std::exception& e)
            {
                // a background write must never take the layer down; log and move on
                Logger::err(std::string("async writer job failed: ") + e.what());
            }
            catch (...)
            {
                Logger::err("async writer job failed with an unknown exception");
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_active > 0)
                    m_active--;
                if (m_jobs.empty() && m_active == 0)
                    m_done.notify_all();
            }
        }
    }

    void AsyncWriter::waitForIdle()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_jobs.empty() && m_active == 0; });
    }
} // namespace VKIntox
