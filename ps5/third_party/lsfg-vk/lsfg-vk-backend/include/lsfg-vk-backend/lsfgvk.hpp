/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::backend {

    class [[gnu::visibility("default")]] ContextImpl;
    class [[gnu::visibility("default")]] InstanceImpl;

    using Context = ContextImpl;

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class [[gnu::visibility("default")]] error : public std::runtime_error {
    public:
        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        /// @param inner Inner exception.
        ///
        explicit error(const std::string &msg, const std::exception &inner);

        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        ///
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    /// Function type for picking a device based on its name and IDs
    using DevicePicker = std::function<bool(
        const std::string& deviceName,
        std::pair<const std::string&, const std::string&> ids, // (vendor ID, device ID) 0xXXXX format
        const std::optional<std::string>& pci // (bus:slot.func) if available, no padded zeros
    )>;

    ///
    /// (WSword) An application's own Vulkan device, used as it is (no second device, no
    /// shared memory): Vulkan 1.2 with timelineSemaphore enabled (and shaderFloat16 if fp16).
    ///
    struct ExternalDevice {
        VkInstance instance;
        VkPhysicalDevice physdev;
        VkDevice device;
        uint32_t queueFamily;
        VkQueue queue; // every submission goes here: the caller's thread only
        PFN_vkGetInstanceProcAddr getInstanceProcAddr;
        bool fp16; // shaderFloat16 is enabled on the device
        std::filesystem::path cacheFile; // pipeline cache (empty: none)
    };

    ///
    /// (WSword) The handles of a context opened with openLocalContext. All images stay in
    /// VK_IMAGE_LAYOUT_GENERAL. Frame n (frameIndex) goes into source[n % 2]; signal `sync`
    /// to nextSignal() once it is there. Generated frame i is in dest[i] once `sync` reaches
    /// nextSignal() + 1 + i (values read before scheduleFrames).
    ///
    struct ContextImages {
        VkImage source[2];
        std::vector<VkImage> dest;
        std::vector<VkImageView> destViews;
        VkSemaphore sync; // timeline
        VkFormat format;
    };

    ///
    /// Main entry point of the library
    ///
    class [[gnu::visibility("default")]] Instance {
    public:
        ///
        /// Create a lsfg-vk instance
        ///
        /// @param devicePicker Function that picks a physical device based on some identifiers.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const DevicePicker& devicePicker,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// (WSword) Create an instance on an existing device.
        ///
        /// @param device The application's device and queue.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders (needs device.fp16).
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const ExternalDevice& device,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// (WSword) Open a context whose images and semaphore are the instance's own.
        ///
        /// @param generated Frames generated between two real ones (1: 2x, 2: 3x, ...).
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR (R16G16B16A16_SFLOAT, else R8G8B8A8_UNORM).
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openLocalContext(size_t generated,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf);

        /// (WSword) A local context's handles.
        static ContextImages images(const Context& context);
        /// (WSword) The value the application signals for the next real frame.
        static uint64_t nextSignal(const Context& context);
        /// (WSword) The next real frame's index (0 for the first).
        static uint64_t frameIndex(const Context& context);
        /// (WSword) Whether the FP16 shaders were loaded.
        [[nodiscard]] bool lowPrecision() const;
        /// (WSword) Save the pipeline cache (silently failing).
        void persistCache() const;

        ///
        /// Open a frame generation context.
        ///
        /// The VkFormat of the exchanged images is inferred from whether hdr is true or false:
        /// - false: VK_FORMAT_R8G8B8A8_UNORM
        /// - true: VK_FORMAT_R16G16B16A16_SFLOAT
        ///
        /// The application and library must keep track of the frame index. When the next frame
        /// is ready, signal the syncFd with one increment (with the first trigger being 1).
        /// Each generated frame will increment the semaphore by one:
        /// - Application signals 1 -> Start generating with (curr, next) source images
        /// - Library signals 1 -> First frame between (curr, next) is ready
        /// - Library signals N -> N-th frame between (curr, next) is ready
        /// - Application signals N+1 -> Start generating with (next, curr) source images
        ///
        /// @param sourceFds Pair of file descriptors for the source images alternated between.
        /// @param destFds Vector with file descriptors to import output images from.
        /// @param syncFd File descriptor for the timeline semaphore used for synchronization.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<int, int> sourceFds,
            const std::vector<int>& destFds,
            int syncFd,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf
        );

        ///
        /// Schedule a new set of generated frames.
        ///
        /// @param context Context to use.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context);

        ///
        /// (WSword) Schedule some of a context's generated frames: the passes in `which` (ascending, each below the
        /// context's count), their frames signalled one after another - nextSignal() (read before) + 1 + k for
        /// the k-th. A 4x context makes 2x's frame with {1} (halfway): no second context for a lower multiplier.
        /// Empty: the shared pass alone (the next frame's history), nothing to show.
        ///
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context, const std::vector<size_t>& which);

        ///
        /// Close a frame generation context
        ///
        /// @param context Context to close.
        ///
        void closeContext(const Context& context);

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
    };

    ///
    /// (WSword) Check a Lossless.dll without a device: does it hold every frame generation
    /// shader this version uses?
    ///
    /// @param shaderDllPath Path to the Lossless.dll file.
    /// @return bit 0: the full-precision set is complete, bit 1: the FP16 set is.
    /// @throws backend::error when the file can't be read or neither set is complete.
    ///
    [[gnu::visibility("default")]] int checkShaderDll(const std::filesystem::path& shaderDllPath);

    ///
    /// Make all lsfg-vk instances leaking.
    /// This is to workaround a bug in the Vulkan loader, which
    /// makes it impossible to destroy Vulkan instances and devices.
    ///
    void makeLeaking();

}
