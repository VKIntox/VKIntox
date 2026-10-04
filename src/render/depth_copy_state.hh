#ifndef DEPTH_COPY_STATE_HPP_INCLUDED
#define DEPTH_COPY_STATE_HPP_INCLUDED

#include "depth_state.hh"

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace VKIntox
{
    // v3 deferred depth copy: depth that needs blitting at QueueSubmit time.
    //
    // Two threads reach this state: a recording thread publishes a copy from
    // CmdEndRenderPass*, and the submitting thread consumes it. QueueSubmit
    // cannot use globalLock for it, because panicLayer() flushes the deferred
    // destroy queue underneath that path, so the state carries its own mutex.
    //
    // Lock order: globalLock is always acquired *before* this mutex, never the
    // other way round. QueueSubmit therefore never calls into globalLock-backed
    // code (storage ensure, active-depth update) from inside withRing().
    class DepthCopyState
    {
    public:
        static constexpr uint32_t RING_SIZE = 4;

        // --- pending handoff, from CmdEndRenderPass* to QueueSubmit ---

        void publish(const DepthState& depthState, VkImageLayout sourceLayout)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pendingDepthState = depthState;
            m_pendingSourceLayout = sourceLayout;
            m_pending = true;
        }

        bool hasPending() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_pending;
        }

        // takes the pending copy and clears the flag in one step, so two
        // submits can never blit the same depth and one is not lost to a torn
        // read of a half-published DepthState.
        bool consume(DepthState& depthState, VkImageLayout& sourceLayout)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pending)
                return false;
            depthState = m_pendingDepthState;
            sourceLayout = m_pendingSourceLayout;
            m_pending = false;
            return true;
        }

        // --- ring of pre-allocated copy command buffers ---

        class RingAccess
        {
        public:
            explicit RingAccess(DepthCopyState& state) : m_state(state) {}

            bool ready() const { return !m_state.m_buffers.empty(); }
            VkCommandPool pool() const { return m_state.m_pool; }
            uint32_t slotCount() const { return static_cast<uint32_t>(m_state.m_buffers.size()); }

            VkCommandBuffer buffer(uint32_t slot) const { return m_state.m_buffers[slot]; }
            VkFence fence(uint32_t slot) const
            {
                return slot < m_state.m_fences.size() ? m_state.m_fences[slot] : VK_NULL_HANDLE;
            }

            uint32_t reserveSlot() { return (m_state.m_index++) % RING_SIZE; }

            void install(VkCommandPool pool,
                         std::vector<VkCommandBuffer> buffers,
                         std::vector<VkFence> fences)
            {
                m_state.m_pool = pool;
                m_state.m_buffers = std::move(buffers);
                m_state.m_fences = std::move(fences);
                m_state.m_index = 0;
            }

            // hands the caller the handles it has to destroy, then clears so a
            // late submit cannot touch a torn-down ring
            void detach(VkCommandPool& pool, std::vector<VkFence>& fences)
            {
                pool = m_state.m_pool;
                fences = std::move(m_state.m_fences);
                m_state.m_pool = VK_NULL_HANDLE;
                m_state.m_buffers.clear();
                m_state.m_fences.clear();
                m_state.m_index = 0;
            }

        private:
            DepthCopyState& m_state;
        };

        // fn runs with the mutex held and receives the locked ring view. It must
        // not take globalLock, and must not call back into publish/consume.
        template <typename F>
        auto withRing(F&& fn) -> decltype(fn(std::declval<RingAccess&>()))
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RingAccess access(*this);
            return fn(access);
        }

    private:
        mutable std::mutex m_mutex;
        DepthState m_pendingDepthState;
        VkImageLayout m_pendingSourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool m_pending = false;

        VkCommandPool m_pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_buffers;
        std::vector<VkFence> m_fences;
        uint32_t m_index = 0;
    };
} // namespace VKIntox

#endif // DEPTH_COPY_STATE_HPP_INCLUDED
