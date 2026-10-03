#include "wayland_input_common.hh"
#include "wayland_display.hh"
#include "logger.hh"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <poll.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace VKIntox
{
    static wl_display* displayWrapper = nullptr;
    static wl_event_queue* queue = nullptr;
    static wl_registry* registry = nullptr;
    static wl_seat* seat = nullptr;
    static bool commonInitialized = false;

    // Input-capture surface: a transparent wl_subsurface of the game's surface
    // whose input region is the overlay hitbox.
    static wl_compositor* compositor = nullptr;
    static wl_subcompositor* subcompositor = nullptr;
    static wl_shm* shm = nullptr;
    static wl_surface* inputSurface = nullptr;
    static wl_subsurface* inputSubsurface = nullptr;
    static wl_region* inputRegion = nullptr;
    static wl_buffer* inputBuffer = nullptr;
    static wl_shm_pool* inputPool = nullptr;
    static void* inputBufferData = nullptr;
    static size_t inputBufferSize = 0;
    static int inputShmFd = -1;
    static int inputBufferWidth = 0;
    static int inputBufferHeight = 0;
    static float inputSurfaceX = 0.0f;
    static float inputSurfaceY = 0.0f;
    static float inputRectX = 0.0f;
    static float inputRectY = 0.0f;
    static float inputRectW = 0.0f;
    static float inputRectH = 0.0f;
    static bool inputRectValid = false;

    // Frame-level dispatch deduplication — tracks a monotonic counter so
    // multiple callers (getMouseState, getKeyboardState, isKeyPressed×N)
    // within the same frame only do one real dispatch.
    static std::atomic<uint64_t> dispatchFrameId{0};
    static std::atomic<uint64_t> lastDispatchedFrame{0};

    // Device bind callbacks — set by keyboard/mouse modules before init
    static KeyboardBindCallback keyboardBind = nullptr;
    static PointerBindCallback pointerBind = nullptr;

    void setKeyboardBindCallback(KeyboardBindCallback cb)
    {
        keyboardBind = cb;
        // If seat already bound, invoke callback immediately for late registration
        if (seat && cb)
            cb(seat);
    }

    void setPointerBindCallback(PointerBindCallback cb)
    {
        pointerBind = cb;
        // If seat already bound, invoke callback immediately for late registration
        if (seat && cb)
            cb(seat);
    }

    // Single seat listener that handles both keyboard and pointer capabilities
    static void seatCapabilities(void* /*data*/, wl_seat* s, uint32_t caps)
    {
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboardBind)
            keyboardBind(s);
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointerBind)
            pointerBind(s);
    }

    static void seatName(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/)
    {
    }

    static const wl_seat_listener sharedSeatListener = {
        .capabilities = seatCapabilities,
        .name = seatName,
    };

    static void registryGlobal(void* /*data*/, wl_registry* reg,
                               uint32_t name, const char* interface, uint32_t version)
    {
        if (strcmp(interface, wl_seat_interface.name) == 0)
        {
            if (seat)
                return;
            seat = (wl_seat*)wl_registry_bind(reg, name, &wl_seat_interface,
                                                version < 5 ? version : 5);
            wl_seat_add_listener(seat, &sharedSeatListener, nullptr);
            Logger::debug("Wayland: shared seat bound");
        }
        else if (strcmp(interface, wl_compositor_interface.name) == 0)
        {
            if (!compositor)
                compositor = (wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface,
                                                              version < 4 ? version : 4);
        }
        else if (strcmp(interface, wl_subcompositor_interface.name) == 0)
        {
            if (!subcompositor)
                subcompositor = (wl_subcompositor*)wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
        }
        else if (strcmp(interface, wl_shm_interface.name) == 0)
        {
            if (!shm)
                shm = (wl_shm*)wl_registry_bind(reg, name, &wl_shm_interface, 1);
        }
    }

    static void registryGlobalRemove(void* /*data*/, wl_registry* /*registry*/, uint32_t /*name*/)
    {
    }

    static const wl_registry_listener registryListener = {
        .global = registryGlobal,
        .global_remove = registryGlobalRemove,
    };

    // ── Input-capture surface ────────────────────────────────────────────────

    static void destroyInputBuffer()
    {
        if (inputBuffer)
        {
            wl_buffer_destroy(inputBuffer);
            inputBuffer = nullptr;
        }
        if (inputPool)
        {
            wl_shm_pool_destroy(inputPool);
            inputPool = nullptr;
        }
        if (inputBufferData)
        {
            munmap(inputBufferData, inputBufferSize);
            inputBufferData = nullptr;
        }
        if (inputShmFd >= 0)
        {
            close(inputShmFd);
            inputShmFd = -1;
        }
        inputBufferSize = 0;
        inputBufferWidth = 0;
        inputBufferHeight = 0;
    }

    // A fully transparent ARGB buffer, sized to the hitbox. It exists only so
    // the surface is mapped (an unmapped surface receives no input); nothing is
    // ever drawn into it.
    static bool ensureInputBuffer(int width, int height)
    {
        if (inputBuffer && inputBufferWidth == width && inputBufferHeight == height)
            return true;

        const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;

        char name[64];
        std::snprintf(name, sizeof(name), "/vkintox-input-%d", static_cast<int>(getpid()));
        const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            Logger::warn("Wayland: shm_open failed for the input buffer");
            return false;
        }
        shm_unlink(name);

        if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        {
            close(fd);
            return false;
        }

        void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED)
        {
            close(fd);
            return false;
        }
        memset(data, 0, size);

        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size));
        wl_buffer* buffer = pool
            ? wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888)
            : nullptr;
        if (!buffer)
        {
            if (pool)
                wl_shm_pool_destroy(pool);
            munmap(data, size);
            close(fd);
            return false;
        }

        // swap only once the replacement is complete: tearing the old buffer
        // down first would leave the surface unmapped for an instant, and an
        // unmapped surface loses the pointer focus, which reads as a frozen
        // cursor until the user moves again.
        destroyInputBuffer();

        inputBuffer = buffer;
        inputPool = pool;
        inputBufferData = data;
        inputBufferSize = size;
        inputShmFd = fd;
        inputBufferWidth = width;
        inputBufferHeight = height;
        return true;
    }

    static void createInputSurface()
    {
        if (inputSurface || !compositor || !subcompositor || !shm)
            return;

        wl_surface* parent = getWaylandSurface();
        if (!parent)
            return;

        inputSurface = wl_compositor_create_surface(compositor);
        if (!inputSurface)
            return;

        inputSubsurface = wl_subcompositor_get_subsurface(subcompositor, inputSurface, parent);
        if (!inputSubsurface)
            return;

        // apply our commits on their own rather than waiting on the parent's
        wl_subsurface_set_desync(inputSubsurface);

        // start with an empty region: everything passes through until the
        // overlay publishes a hitbox.
        inputRegion = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(inputSurface, inputRegion);
        wl_surface_commit(inputSurface);
        if (wl_display* display = getWaylandDisplay())
            wl_display_flush(display);

        Logger::info("Wayland: input capture surface created");
    }

    wl_surface* getWaylandInputSurface()
    {
        return inputSurface;
    }

    float getWaylandInputSurfaceX()
    {
        return inputSurfaceX;
    }

    float getWaylandInputSurfaceY()
    {
        return inputSurfaceY;
    }

    void setWaylandInputSurfaceRect(float x, float y, float width, float height)
    {
        if (!inputSurface)
            return;

        // the overlay calls this every frame; only churn the compositor when the
        // hitbox actually moves or resizes.
        if (inputRectValid && x == inputRectX && y == inputRectY &&
            width == inputRectW && height == inputRectH)
            return;
        inputRectX = x;
        inputRectY = y;
        inputRectW = width;
        inputRectH = height;
        inputRectValid = true;

        inputSurfaceX = x;
        inputSurfaceY = y;

        // a region is immutable once handed to set_input_region, so rebuild it.
        if (inputRegion)
            wl_region_destroy(inputRegion);
        inputRegion = wl_compositor_create_region(compositor);
        if (!inputRegion)
            return;

        if (width <= 0.0f || height <= 0.0f)
        {
            wl_surface_set_input_region(inputSurface, inputRegion); // empty => pass-through
            wl_surface_commit(inputSurface);
            if (wl_display* display = getWaylandDisplay())
                wl_display_flush(display);
            return;
        }

        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        if (!ensureInputBuffer(w, h))
            return;

        wl_region_add(inputRegion, 0, 0, w, h);
        wl_subsurface_set_position(inputSubsurface, static_cast<int>(x), static_cast<int>(y));
        wl_surface_set_input_region(inputSurface, inputRegion);
        wl_surface_attach(inputSurface, inputBuffer, 0, 0);
        wl_surface_commit(inputSurface);
        if (wl_display* display = getWaylandDisplay())
            wl_display_flush(display);
    }

    wl_event_queue* getWaylandInputQueue()
    {
        return queue;
    }

    wl_seat* getWaylandSeat()
    {
        return seat;
    }

    bool initWaylandInputCommon()
    {
        if (commonInitialized)
            return seat != nullptr;

        wl_display* display = getWaylandDisplay();
        if (!display)
            return false;

        displayWrapper = (wl_display*)wl_proxy_create_wrapper(display);
        if (!displayWrapper)
            return false;

        queue = wl_display_create_queue(display);
        if (!queue)
        {
            wl_proxy_wrapper_destroy(displayWrapper);
            displayWrapper = nullptr;
            return false;
        }

        wl_proxy_set_queue((wl_proxy*)displayWrapper, queue);

        registry = wl_display_get_registry(displayWrapper);
        wl_registry_add_listener(registry, &registryListener, nullptr);

        // Roundtrip to discover globals (seat)
        wl_display_roundtrip_queue(display, queue);
        // Second roundtrip to get seat capabilities (keyboard + pointer)
        wl_display_roundtrip_queue(display, queue);

        createInputSurface();

        if (seat)
        {
            commonInitialized = true;
            Logger::info("Wayland: shared input resources initialized");
        }
        else
        {
            Logger::warn("Wayland: no seat found");
            if (registry)
            {
                wl_registry_destroy(registry);
                registry = nullptr;
            }
            if (queue)
            {
                wl_event_queue_destroy(queue);
                queue = nullptr;
            }
            if (displayWrapper)
            {
                wl_proxy_wrapper_destroy(displayWrapper);
                displayWrapper = nullptr;
            }
        }

        return seat != nullptr;
    }

    void beginWaylandInputFrame()
    {
        dispatchFrameId.fetch_add(1, std::memory_order_release);
    }

    void dispatchWaylandInputEvents(bool readSocket)
    {
        if (!queue)
            return;

        wl_display* display = getWaylandDisplay();
        if (!display)
            return;

        // Consumer-only path: deliver whatever the game's own read already
        // queued for us, and never touch the socket. Used by the effect-facing
        // key/mouse queries, which run every present and must not entangle with
        // the game's frame-completion events.
        if (!readSocket)
        {
            wl_display_dispatch_queue_pending(display, queue);
            return;
        }

        // Skip if already dispatched this frame
        uint64_t currentFrame = dispatchFrameId.load(std::memory_order_acquire);
        if (lastDispatchedFrame.load(std::memory_order_acquire) == currentFrame)
            return;
        lastDispatchedFrame.store(currentFrame, std::memory_order_release);

        // Drain any already-queued events first. If another thread is mid-read
        // (prepare_read fails with EAGAIN) we must not read or retry in a loop:
        // that spins until the other thread releases the read lock, which stalls
        // the present thread whenever the game holds it. Just drain and return;
        // the next frame tries again.
        if (wl_display_prepare_read_queue(display, queue) != 0)
        {
            wl_display_dispatch_queue_pending(display, queue);
            return;
        }

        // Non-blocking socket read — many games only call
        // wl_display_dispatch_pending() in their render loop, which does
        // NOT read from the socket.  Without this, button release and
        // other events stay stuck in the kernel buffer.
        wl_display_flush(display);
        struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);

        wl_display_dispatch_queue_pending(display, queue);
    }

    void cleanupWaylandInputCommon()
    {
        destroyInputBuffer();
        if (inputRegion)
        {
            wl_region_destroy(inputRegion);
            inputRegion = nullptr;
        }
        if (inputSubsurface)
        {
            wl_subsurface_destroy(inputSubsurface);
            inputSubsurface = nullptr;
        }
        if (inputSurface)
        {
            wl_surface_destroy(inputSurface);
            inputSurface = nullptr;
        }
        if (shm)
        {
            wl_shm_destroy(shm);
            shm = nullptr;
        }
        if (subcompositor)
        {
            wl_subcompositor_destroy(subcompositor);
            subcompositor = nullptr;
        }
        if (compositor)
        {
            wl_compositor_destroy(compositor);
            compositor = nullptr;
        }
        if (seat)
        {
            wl_seat_destroy(seat);
            seat = nullptr;
        }
        if (registry)
        {
            wl_registry_destroy(registry);
            registry = nullptr;
        }
        if (queue)
        {
            wl_event_queue_destroy(queue);
            queue = nullptr;
        }
        if (displayWrapper)
        {
            wl_proxy_wrapper_destroy(displayWrapper);
            displayWrapper = nullptr;
        }

        keyboardBind = nullptr;
        pointerBind = nullptr;
        commonInitialized = false;
    }

} // namespace VKIntox
