#include "wayland_interpose_patch.hh"

#include "logger.hh"

#include <wayland-client.h>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    constexpr int kMaxCopies = 8;

    struct PatchedCopy
    {
        uintptr_t lo;
        uintptr_t hi;
        void* real;
    };

    PatchedCopy g_copies[kMaxCopies];
    std::atomic<int> g_copyCount{0};
    std::mutex g_installMutex;
    std::atomic<int64_t> g_lastScanNs{0};

    bool nameMatches(const char* path, const char* needle)
    {
        return path && std::strstr(path, needle) != nullptr;
    }

    // glibc rewrites DT_SYMTAB/DT_STRTAB/DT_JMPREL values in place to absolute
    // addresses at load time; a rare configuration leaves them relative.
    // Accept both so a wild pointer can never come out of this.
    uintptr_t normalize(uintptr_t base, uintptr_t value)
    {
        return value < base ? base + value : value;
    }

    // restore the protection the page had before we touched it, read straight
    // from /proc/self/maps (RELRO got pages are read-only)
    void restoreProtection(uintptr_t pageAddr, size_t pageSize)
    {
        int flags = PROT_READ;
        if (FILE* maps = std::fopen("/proc/self/maps", "re"))
        {
            char line[512];
            while (std::fgets(line, sizeof(line), maps))
            {
                unsigned long start = 0;
                unsigned long end = 0;
                char perms[8] = {0};
                if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3 && start <= pageAddr && pageAddr < end)
                {
                    flags = 0;
                    if (perms[0] == 'r') flags |= PROT_READ;
                    if (perms[1] == 'w') flags |= PROT_WRITE;
                    if (perms[2] == 'x') flags |= PROT_EXEC;
                    if (flags == 0)
                        flags = PROT_READ;
                    break;
                }
            }
            std::fclose(maps);
        }
        mprotect(reinterpret_cast<void*>(pageAddr), pageSize, flags);
    }

    struct Scan
    {
        const char* symbol;
        void* replacement;
        bool probeOnly;
        bool stopped;
        bool sawObject;
        void** foundSlot;
        void** foundReal;
    };

    void tryPatchObject(struct dl_phdr_info* info, Scan& scan)
    {
        const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);

        // already gated? (same base means same load)
        const int known = g_copyCount.load(std::memory_order_acquire);
        for (int i = 0; i < known; ++i)
        {
            if (g_copies[i].lo == base)
                return;
        }

        // object load ranges + dynamic segment
        uintptr_t lo = UINTPTR_MAX;
        uintptr_t hi = 0;
        const ElfW(Phdr)* dynPhdr = nullptr;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
        {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type == PT_LOAD && ph.p_memsz)
            {
                const uintptr_t segLo = base + static_cast<uintptr_t>(ph.p_vaddr);
                const uintptr_t segHi = segLo + static_cast<uintptr_t>(ph.p_memsz);
                lo = segLo < lo ? segLo : lo;
                hi = segHi > hi ? segHi : hi;
            }
            else if (ph.p_type == PT_DYNAMIC)
            {
                dynPhdr = &ph;
            }
        }
        if (!dynPhdr || lo >= hi)
            return;

        auto inRange = [&](uintptr_t p, size_t len) {
            return p >= lo && len <= hi - lo && p <= hi - len;
        };
        auto ptrIn = [&](const void* p, size_t len) {
            return inRange(reinterpret_cast<uintptr_t>(p), len);
        };

        const auto* dynamic = reinterpret_cast<const ElfW(Dyn)*>(base + static_cast<uintptr_t>(dynPhdr->p_vaddr));
        if (!ptrIn(dynamic, sizeof(ElfW(Dyn))))
            return;

        const ElfW(Sym)* symtab = nullptr;
        const char* strtab = nullptr;
        const ElfW(Rela)* jmprel = nullptr;
        size_t pltrelsz = 0;
        for (const ElfW(Dyn)* dyn = dynamic; dyn->d_tag != DT_NULL; ++dyn)
        {
            switch (dyn->d_tag)
            {
                case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(normalize(base, dyn->d_un.d_ptr)); break;
                case DT_STRTAB: strtab = reinterpret_cast<const char*>(normalize(base, dyn->d_un.d_ptr)); break;
                case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rela)*>(normalize(base, dyn->d_un.d_ptr)); break;
                case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(dyn->d_un.d_val); break;
                default: break;
            }
        }

        if (!symtab || !strtab || !jmprel || pltrelsz == 0)
            return;
        if (!ptrIn(symtab, sizeof(ElfW(Sym))) || !ptrIn(strtab, 1) || !ptrIn(jmprel, sizeof(ElfW(Rela))))
            return;

        const size_t count = pltrelsz / sizeof(ElfW(Rela));
        if (count == 0 || count > 65536 || !inRange(reinterpret_cast<uintptr_t>(jmprel), count * sizeof(ElfW(Rela))))
            return;

        const size_t wanted = std::strlen(scan.symbol);
        for (size_t i = 0; i < count; ++i)
        {
            const ElfW(Rela)& rel = jmprel[i];
            if (ELF64_R_TYPE(rel.r_info) != R_X86_64_JUMP_SLOT)
                continue;

            const ElfW(Sym)* sym = symtab + ELF64_R_SYM(rel.r_info);
            if (!ptrIn(sym, sizeof(ElfW(Sym))))
                continue;

            const char* name = strtab + sym->st_name;
            if (!ptrIn(name, wanted + 1) || std::memcmp(name, scan.symbol, wanted) != 0 || name[wanted] != '\0')
                continue;

            const uintptr_t slotAddr = base + static_cast<uintptr_t>(rel.r_offset);
            if (!ptrIn(reinterpret_cast<const void*>(slotAddr), sizeof(void*)))
                continue;

            auto* slot = reinterpret_cast<void**>(slotAddr);
            void* current = *slot;

            // the slot must already hold this object's own resolved entry: an
            // unresolved lazy stub would re-resolve and drop the patch, and a
            // slot bound to another object would call back into our wrapper
            if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0)
                continue;
            const uintptr_t ownEntry = base + static_cast<uintptr_t>(sym->st_value);
            if (reinterpret_cast<uintptr_t>(current) != ownEntry)
            {
                if (!scan.probeOnly)
                    VKIntox::Logger::trace("Wayland interpose: marshal jump slot in " + std::string(info->dlpi_name)
                                           + " is not resolved yet, skipping");
                continue;
            }

            if (scan.probeOnly)
            {
                if (scan.foundSlot)
                    *scan.foundSlot = slot;
                if (scan.foundReal)
                    *scan.foundReal = current;
                scan.stopped = true;
                return;
            }

            const int index = g_copyCount.load(std::memory_order_relaxed);
            if (index >= kMaxCopies)
                return;

            const long pageSizeLong = sysconf(_SC_PAGESIZE);
            const size_t pageSize = pageSizeLong > 0 ? static_cast<size_t>(pageSizeLong) : 4096;
            const uintptr_t pageAddr = slotAddr & ~static_cast<uintptr_t>(pageSize - 1);
            if (mprotect(reinterpret_cast<void*>(pageAddr), pageSize, PROT_READ | PROT_WRITE) != 0)
                return;

            // publish the real entry before any thread can reach the patched slot
            g_copies[index] = {lo, hi, current};
            g_copyCount.store(index + 1, std::memory_order_release);
            *slot = scan.replacement;

            restoreProtection(pageAddr, pageSize);

            VKIntox::Logger::info("Wayland interpose: gated the marshal entry in " + std::string(info->dlpi_name));
            return;
        }
    }

    int scanCallback(struct dl_phdr_info* info, size_t, void* data)
    {
        auto* scan = static_cast<Scan*>(data);
        if (nameMatches(info->dlpi_name, "libwayland-client.so"))
        {
            scan->sawObject = true;
            tryPatchObject(info, *scan);
        }
        return scan->stopped ? 1 : 0;
    }
} // namespace

namespace VKIntox
{
    void* waylandMarshalArrayFlagsForCaller(void* caller)
    {
        const uintptr_t pc = reinterpret_cast<uintptr_t>(caller);
        const int count = g_copyCount.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
        {
            if (pc >= g_copies[i].lo && pc < g_copies[i].hi)
                return g_copies[i].real;
        }
        return count > 0 ? g_copies[0].real : nullptr;
    }

    void installWaylandInterposeHooks()
    {
        // called before every present to catch libwayland copies that load
        // after us; scanning walks every loaded object, so keep it to a
        // once-a-second cadence instead of once a frame
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        int64_t last = g_lastScanNs.load(std::memory_order_relaxed);
        if (last != 0 && now - last < 1'000'000'000LL)
            return;
        if (!g_lastScanNs.compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lock(g_installMutex);
        Scan scan{"wl_proxy_marshal_array_flags",
                  reinterpret_cast<void*>(&wl_proxy_marshal_array_flags),
                  false, false, false, nullptr, nullptr};
        dl_iterate_phdr(scanCallback, &scan);

        // a wayland built with its internal calls bound directly has nothing
        // to redirect; say so once instead of looking armed while inert
        if (scan.sawObject && g_copyCount.load(std::memory_order_acquire) == 0)
        {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                Logger::warn("Wayland interpose: loaded libwayland exposes no patchable marshal entry, commits stay ungated");
        }
    }

    bool waylandMarshalSlotForTests(void** slotOut, void** realOut)
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        Scan scan{"wl_proxy_marshal_array_flags", nullptr, true, false, false, slotOut, realOut};
        dl_iterate_phdr(scanCallback, &scan);
        return scan.stopped;
    }
} // namespace VKIntox
