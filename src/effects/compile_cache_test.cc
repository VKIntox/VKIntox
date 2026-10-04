// Tests for CompileCache, the bounded cache that lets a depth/effect chain
// reload reuse an already-compiled ReShade module instead of re-parsing every
// effect. The key semantics and the bound are the whole contract, so they are
// pinned here; the threaded case fails if the internal lock ever goes away.

#include "compile_cache.hh"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using VKIntox::CompileCache;
using VKIntox::hashBytes;

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

    void test_empty_cache_misses()
    {
        CompileCache<std::string> cache;
        std::string out = "unchanged";
        expect(!cache.lookup({123, 0}, out), "an empty cache misses");
        expect(out == "unchanged", "a miss leaves the output untouched");
        expect(cache.size() == 0, "an empty cache is empty");
    }

    void test_store_then_lookup_round_trips()
    {
        CompileCache<std::string> cache;
        cache.store({hashBytes("effect", 6), 7}, "compiled-module");

        std::string out;
        expect(cache.lookup({hashBytes("effect", 6), 7}, out), "a stored key hits");
        expect(out == "compiled-module", "the stored value comes back");
        expect(cache.size() == 1, "one entry after one store");
    }

    void test_flags_partition_the_key()
    {
        CompileCache<std::string> cache;
        const uint64_t h = hashBytes("same-source", 11);
        cache.store({h, 0}, "flags-0");
        cache.store({h, 1}, "flags-1");

        std::string out;
        expect(cache.lookup({h, 0}, out) && out == "flags-0", "flag word 0 is its own entry");
        expect(cache.lookup({h, 1}, out) && out == "flags-1", "flag word 1 is its own entry");
        expect(!cache.lookup({h, 2}, out), "an unseen flag word misses");
    }

    void test_source_partitions_the_key()
    {
        CompileCache<std::string> cache;
        cache.store({hashBytes("a", 1), 0}, "module-a");
        cache.store({hashBytes("b", 1), 0}, "module-b");

        std::string out;
        expect(cache.lookup({hashBytes("a", 1), 0}, out) && out == "module-a", "source a is distinct");
        expect(cache.lookup({hashBytes("b", 1), 0}, out) && out == "module-b", "source b is distinct");
    }

    void test_restore_does_not_duplicate()
    {
        CompileCache<std::string> cache;
        const CompileCache<std::string>::Key key{42, 0};
        cache.store(key, "first");
        cache.store(key, "second");

        std::string out;
        expect(cache.size() == 1, "re-storing the same key does not grow the cache");
        expect(cache.lookup(key, out) && out == "first", "first value wins; a reload is not a mutation");
    }

    void test_fifo_bound_evicts_oldest()
    {
        CompileCache<std::string> cache(3);
        cache.store({1, 0}, "one");
        cache.store({2, 0}, "two");
        cache.store({3, 0}, "three");
        expect(cache.size() == 3, "cache fills to capacity");

        cache.store({4, 0}, "four");
        expect(cache.size() == 3, "capacity is not exceeded");

        std::string out;
        expect(!cache.lookup({1, 0}, out), "the oldest entry was evicted");
        expect(cache.lookup({2, 0}, out) && out == "two", "the next entry survives");
        expect(cache.lookup({3, 0}, out) && out == "three", "the third entry survives");
        expect(cache.lookup({4, 0}, out) && out == "four", "the new entry is present");
    }

    void test_clear_empties()
    {
        CompileCache<std::string> cache;
        cache.store({1, 0}, "x");
        cache.clear();
        std::string out;
        expect(!cache.lookup({1, 0}, out), "clear removes every entry");
        expect(cache.size() == 0, "clear resets the size");
    }

    void test_hash_is_stable_and_sensitive()
    {
        const char* a = "depth_resolve";
        const char* b = "depth_resolvf";  // one byte different
        expect(hashBytes(a, 13) == hashBytes(a, 13), "hashing is deterministic");
        expect(hashBytes(a, 13) != hashBytes(b, 13), "a one-byte change changes the hash");
        expect(hashBytes(a, 5) != hashBytes(a, 13), "a length change changes the hash");
        expect(hashBytes("", 0) == hashBytes("", 0), "the empty input hashes consistently");
    }

    void test_concurrent_store_and_lookup()
    {
        // capacity is large enough that nothing is evicted, so every stored key
        // must still be retrievable after all threads finish
        CompileCache<int> cache(1u << 20);

        const int kThreads = 8;
        const int kPerThread = 4000;
        std::atomic<bool> bad{false};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            workers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i)
                {
                    const uint64_t h = static_cast<uint64_t>(t) * kPerThread + i;
                    cache.store({h, 0}, static_cast<int>(h));
                }
            });
        }
        for (auto& w : workers)
            w.join();

        for (int t = 0; t < kThreads; ++t)
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                const uint64_t h = static_cast<uint64_t>(t) * kPerThread + i;
                int out = -1;
                if (!cache.lookup({h, 0}, out) || out != static_cast<int>(h))
                    bad.store(true);
            }
        }

        expect(!bad.load(), "every concurrently stored entry is retrievable");
        expect(cache.size() == static_cast<size_t>(kThreads) * kPerThread,
               "no concurrent store was lost or duplicated");
    }
} // namespace

int main()
{
    test_empty_cache_misses();
    test_store_then_lookup_round_trips();
    test_flags_partition_the_key();
    test_source_partitions_the_key();
    test_restore_does_not_duplicate();
    test_fifo_bound_evicts_oldest();
    test_clear_empties();
    test_hash_is_stable_and_sensitive();
    test_concurrent_store_and_lookup();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
