#ifndef DEPTH_DUMP_HPP_INCLUDED
#define DEPTH_DUMP_HPP_INCLUDED

#include <string>

#include "depth/depth_state.hh"
#include "render/device/vulkan_include.hh"

namespace VKIntox
{
    struct LogicalDevice;

    // One-shot host readback of the depth the frame used, started from
    // Settings > Diagnostics. Written out as a 16-bit grayscale PNG whose tEXt
    // chunk carries the source format and extent, so a reader can undo a padded
    // D24 or a reversed-Z range without being told them by hand.
    namespace DepthDump
    {
        void request(const std::string& path);

        // Cheap atomic read; the present path asks this every frame.
        bool wanted();

        // true while the frame is being captured, plus a hold after it, so the
        // UI can keep itself out of the picture
        bool overlaySuppressed();

        // Present path: copy the depth this frame used into a host-visible
        // buffer, submit that on the present queue and write the file. Runs
        // once per request; returns false when the request could not be served.
        bool readBackAtPresent(LogicalDevice* logicalDevice, VkQueue queue, const DepthState& source,
                               bool transformed, VkImage backbuffer, VkFormat backbufferFormat,
                               const VkExtent2D& backbufferExtent);

        // Overlay: pops the last outcome, false when nothing finished.
        bool pollResult(std::string& message, bool& success);

        // Present path: a reason the frame could not be read. Logged once per
        // request so a failed dump leaves a trail without a line every frame.
        void noteSkip(const std::string& reason);

        void abandon();
        void destroyStaging(LogicalDevice* logicalDevice);
    }
} // namespace VKIntox

#endif // DEPTH_DUMP_HPP_INCLUDED
