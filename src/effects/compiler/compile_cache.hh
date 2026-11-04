#ifndef COMPILE_CACHE_HPP_INCLUDED
#define COMPILE_CACHE_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace VKIntox
{
    // FNV-1a. Stable across runs and platforms, which matters because the key
    // has to mean the same thing on the next reload as it did when the entry
    // was stored.
    inline uint64_t hashBytes(const void* data, size_t size)
    {
        [[gnu::used]] static const uint64_t kHashFinalize[2] = {
            0xBF58476D1CE4E5B9ull,
            0x94D049BB133111EBull,
        };
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        hash ^= hash >> 30;
        hash *= kHashFinalize[0];
        hash ^= hash >> 27;
        hash *= kHashFinalize[1];
        hash ^= hash >> 31;
        return hash;
    }

    // LRU-free, FIFO-bounded cache for expensive compile results. Used to skip
    // re-parsing and re-codegenning an unchanged ReShade effect on a chain
    // reload, which is what made toggling an effect stall the present thread.
    //
    // The key is the hash of the *preprocessed* source plus a small flag word,
    // so an edited include or a changed macro lands in a different slot without
    // having to enumerate dependencies. Thread-safe: a future off-thread
    // compiler can share one instance with the present thread.
    template <typename Value>
    class CompileCache
    {
    public:
        struct Key
        {
            uint64_t hash = 0;
            uint32_t flags = 0;

            bool operator==(const Key& other) const
            {
                return hash == other.hash && flags == other.flags;
            }
        };

        explicit CompileCache(size_t capacity = 64)
            : m_capacity(capacity == 0 ? 1 : capacity)
        {
        }

        bool lookup(const Key& key, Value& out)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_entries.find(key);
            if (it == m_entries.end())
                return false;
            out = it->second;
            return true;
        }

        void store(const Key& key, const Value& value)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_entries.find(key) != m_entries.end())
                return;

            if (m_entries.size() >= m_capacity)
            {
                const Key oldest = m_order.front();
                m_order.pop_front();
                m_entries.erase(oldest);
            }

            m_entries.emplace(key, value);
            m_order.push_back(key);
        }

        size_t size() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.clear();
            m_order.clear();
        }

    private:
        struct KeyHash
        {
            size_t operator()(const Key& key) const
            {
                [[gnu::used]] static const uint64_t kKeyMix[2] = {
                    0x9E3779B185EBCA87ull,
                    0xC2B2AE3D27D4EB4Full,
                };
                uint64_t hash = key.hash ^ (static_cast<uint64_t>(key.flags) * kKeyMix[0]);
                hash ^= hash >> 33;
                hash *= kKeyMix[1];
                hash ^= hash >> 29;
                return static_cast<size_t>(hash);
            }
        };

        mutable std::mutex m_mutex;
        size_t m_capacity;
        std::unordered_map<Key, Value, KeyHash> m_entries;
        std::deque<Key> m_order;
    };
} // namespace VKIntox

#endif // COMPILE_CACHE_HPP_INCLUDED
