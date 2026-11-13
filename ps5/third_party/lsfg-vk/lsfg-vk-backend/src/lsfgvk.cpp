/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "extraction/dll_reader.hpp"
#include "extraction/shader_registry.hpp"
#include "helpers/limits.hpp"
#include "helpers/utils.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "shaderchains/alpha0.hpp"
#include "shaderchains/alpha1.hpp"
#include "shaderchains/beta0.hpp"
#include "shaderchains/beta1.hpp"
#include "shaderchains/delta0.hpp"
#include "shaderchains/delta1.hpp"
#include "shaderchains/gamma0.hpp"
#include "shaderchains/gamma1.hpp"
#include "shaderchains/generate.hpp"
#include "shaderchains/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#ifdef LSFGVK_TESTING_RENDERDOC
#include <renderdoc_app.h>
#include <dlfcn.h>
#endif

using namespace lsfgvk;
using namespace lsfgvk::backend;

namespace lsfgvk::backend {
    error::error(const std::string& msg, const std::exception& inner)
        : std::runtime_error(msg + "\n- " + inner.what()) {}
    error::error(const std::string& msg)
        : std::runtime_error(msg) {}
    error::~error() = default;

    /// instance class
    class InstanceImpl {
    public:
        /// create an instance
        /// (see lsfg-vk documentation)
        InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision);
        /// (WSword) on an existing device
        InstanceImpl(vk::Vulkan&& vk,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision);

        /// whether the FP16 shaders were loaded
        [[nodiscard]] bool lowPrecision() const { return this->fp16; }
        /// get the Vulkan instance
        /// @return the Vulkan instance
        [[nodiscard]] const auto& getVulkan() const { return this->vk; }
        /// get the shader registry
        /// @return the shader registry
        [[nodiscard]] const auto& getShaderRegistry() const { return this->shaders; }
#ifdef LSFGVK_TESTING_RENDERDOC
        /// get the RenderDoc API
        /// @return the RenderDoc API
        [[nodiscard]] const auto& getRenderDocAPI() const { return this->renderdoc; }
#endif
        // Movable, non-copyable, custom destructor
        InstanceImpl(const InstanceImpl&) = delete;
        InstanceImpl& operator=(const InstanceImpl&) = delete;
        InstanceImpl(InstanceImpl&&) = default;
        InstanceImpl& operator=(InstanceImpl&&) = default;
        ~InstanceImpl();
    private:
        vk::Vulkan vk;
        bool fp16{false};
        ShaderRegistry shaders;

#ifdef LSFGVK_TESTING_RENDERDOC
        std::optional<RENDERDOC_API_1_6_0> renderdoc;
#endif
    };

    /// context class
    class ContextImpl {
    public:
        /// create a context
        /// (see lsfg-vk documentation)
        /// (WSword: from images and a semaphore made beforehand, imported or the instance's own)
        ContextImpl(const InstanceImpl& instance,
            std::pair<vk::Image, vk::Image> source, std::vector<vk::Image> dest,
            vk::TimelineSemaphore sync,
            VkExtent2D extent, bool hdr, float flow, bool perf);

        /// schedule frames
        /// (see lsfg-vk documentation)
        void scheduleFrames();
        /// (WSword) some of the passes (Instance::scheduleFrames)
        void scheduleFrames(const std::vector<size_t>& which);

        /// (WSword) the handles, the next signal value and the next real frame's index
        [[nodiscard]] ContextImages images(VkFormat format) const {
            ContextImages out{};
            out.source[0] = this->sourceImages.first.handle();
            out.source[1] = this->sourceImages.second.handle();
            for (const auto& image : this->destImages) {
                out.dest.push_back(image.handle());
                out.destViews.push_back(image.imageview());
            }
            out.sync = this->syncSemaphore.handle();
            out.format = format;
            return out;
        }
        [[nodiscard]] uint64_t nextSignal() const { return this->idx; }
        [[nodiscard]] uint64_t frameIndex() const { return this->fidx; }
        [[nodiscard]] bool isHdr() const { return this->ctx.hdr; }
    private:
        std::pair<vk::Image, vk::Image> sourceImages;
        std::vector<vk::Image> destImages;
        vk::Image blackImage;

        vk::TimelineSemaphore syncSemaphore; // imported
        vk::TimelineSemaphore prepassSemaphore;
        size_t idx{1};
        size_t fidx{0}; // real frame index

        std::vector<vk::CommandBuffer> cmdbufs;
        vk::Fence cmdbufFence;

        Ctx ctx;

        Mipmaps mipmaps;
        std::array<Alpha0, 7> alpha0;
        std::array<Alpha1, 7> alpha1;
        Beta0 beta0;
        Beta1 beta1;
        struct Pass {
            std::vector<Gamma0> gamma0;
            std::vector<Gamma1> gamma1;

            std::vector<Delta0> delta0;
            std::vector<Delta1> delta1;
            ls::lazy<Generate> generate;
        };
        std::vector<Pass> passes;
    };
}

Instance::Instance(
        const DevicePicker& devicePicker,
        const std::filesystem::path& shaderDllPath,
        bool allowLowPrecision) {
    const auto selectFunc = [&devicePicker](const vk::VulkanInstanceFuncs funcs,
            const std::vector<VkPhysicalDevice>& devices) {
        for (const auto& device : devices) {
            // check if the physical device supports VK_EXT_pci_bus_info
            uint32_t ext_count{};
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, VK_NULL_HANDLE);

            std::vector<VkExtensionProperties> extensions(ext_count);
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, extensions.data());

            const bool has_pci_ext = std::ranges::find_if(extensions,
                [](const VkExtensionProperties& ext) {
                    return std::string(std::to_array(ext.extensionName).data())
                        == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME;
                }) != extensions.end();

            // then fetch all available properties
            VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT
            };
            VkPhysicalDeviceProperties2 props{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = has_pci_ext ? &pciInfo : nullptr
            };
            funcs.GetPhysicalDeviceProperties2(device, &props);

            std::array<char, 256> devname = std::to_array(props.properties.deviceName);
            devname.at(255) = '\0'; // ensure null-termination

            if (devicePicker(
                std::string(devname.data()),
                { backend::to_hex_id(props.properties.vendorID),
                  backend::to_hex_id(props.properties.deviceID) },
                has_pci_ext ? std::optional<std::string>{
                    std::to_string(pciInfo.pciBus) + ":" +
                    std::to_string(pciInfo.pciDevice) + "." +
                    std::to_string(pciInfo.pciFunction)
                } : std::nullopt
            ))
                return device;
        }

        throw ls::vulkan_error("no suitable physical device found");
    };

    this->m_impl = std::make_unique<InstanceImpl>(
        selectFunc, shaderDllPath, allowLowPrecision
    );
}

namespace {
    /// find the cache file path
    std::filesystem::path findCacheFilePath() {
        const char* xdgCacheHome = std::getenv("XDG_CACHE_HOME");
        if (xdgCacheHome && *xdgCacheHome != '\0')
            return std::filesystem::path(xdgCacheHome) / "lsfg-vk_pipeline_cache.bin";

        const char* home = std::getenv("HOME");
        if (home && *home != '\0')
            return std::filesystem::path(home) / ".cache" / "lsfg-vk_pipeline_cache.bin";

        return{"/tmp/lsfg-vk_pipeline_cache.bin"};
    }
    /// create a Vulkan instance
    vk::Vulkan createVulkanInstance(vk::PhysicalDeviceSelector selectPhysicalDevice) {
        try {
            return{
                "lsfg-vk", vk::version{2, 0, 0},
                "lsfg-vk-engine", vk::version{2, 0, 0},
                selectPhysicalDevice,
                false, std::nullopt,
                findCacheFilePath()
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to initialize Vulkan", e);
        }
    }
    /// build a shader registry
    ShaderRegistry createShaderRegistry(vk::Vulkan& vk,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision) {
        std::unordered_map<uint32_t, std::vector<uint8_t>> resources{};

        try {
            resources = backend::extractResourcesFromDLL(shaderDllPath);
        } catch (const std::exception& e) {
            throw backend::error("Unable to parse Lossless Scaling DLL", e);
        }

        try {
            return backend::buildShaderRegistry(
                vk, allowLowPrecision && vk.supportsFP16(),
                resources
            );
        } catch (const std::exception& e) {
            throw backend::error("Unable to build shader registry", e);
        }
    }
#ifdef LSFGVK_TESTING_RENDERDOC
    /// load RenderDoc integration
    std::optional<RENDERDOC_API_1_6_0> loadRenderDocIntegration() {
        void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (!module)
            return std::nullopt;

        auto renderdocGetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(
            dlsym(module, "RENDERDOC_GetAPI"));
        if (!renderdocGetAPI)
            return std::nullopt;

        RENDERDOC_API_1_6_0* api{};
        renderdocGetAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api));
        if (!api)
            return std::nullopt;

        return *api;
    }
#endif
}

InstanceImpl::InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision)
        : vk(createVulkanInstance(selectPhysicalDevice)),
        fp16(allowLowPrecision && this->vk.supportsFP16()),
        shaders(createShaderRegistry(this->vk, shaderDllPath, this->fp16)) {
#ifdef LSFGVK_TESTING_RENDERDOC
    this->renderdoc = loadRenderDocIntegration();
#endif
    vk.persistPipelineCache(); // will silently fail
}

InstanceImpl::InstanceImpl(vk::Vulkan&& device,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision)
        : vk(std::move(device)),
        fp16(allowLowPrecision && this->vk.supportsFP16()),
        shaders(createShaderRegistry(this->vk, shaderDllPath, this->fp16)) {
    vk.persistPipelineCache(); // will silently fail
}

namespace {
    std::pair<vk::Image, vk::Image> importImages(const vk::Vulkan& vk,
            const std::pair<int, int>& sourceFds, VkExtent2D extent, VkFormat format);
    std::vector<vk::Image> importImages(const vk::Vulkan& vk,
            const std::vector<int>& destFds, VkExtent2D extent, VkFormat format);
    vk::TimelineSemaphore importTimelineSemaphore(const vk::Vulkan& vk, int syncFd);
    vk::TimelineSemaphore createPrepassSemaphore(const vk::Vulkan& vk);

    /// (WSword) the instance's own source images: also copied into (TRANSFER_DST)
    std::pair<vk::Image, vk::Image> createLocalSourceImages(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        try {
            return { vk::Image(vk, extent, format, usage), vk::Image(vk, extent, format, usage) };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create source images", e);
        }
    }
    /// (WSword) the instance's own destination images: sampled by the application
    std::vector<vk::Image> createLocalDestImages(const vk::Vulkan& vk, size_t count,
            VkExtent2D extent, VkFormat format) {
        try {
            std::vector<vk::Image> images;
            images.reserve(count);
            for (size_t i = 0; i < count; ++i)
                images.emplace_back(vk, extent, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            return images;
        } catch (const std::exception& e) {
            throw backend::error("Unable to create destination images", e);
        }
    }

    /// (WSword) the instance and device functions of an existing device
    vk::Vulkan wrapExternalDevice(const ExternalDevice& d) {
        try {
            const auto fi = vk::initVulkanInstanceFuncs(d.instance, d.getInstanceProcAddr, false);
            const auto fd = vk::initVulkanDeviceFuncs(fi, d.device, false);
            return{
                d.instance, d.device, d.physdev, fi, fd,
                d.queueFamily, d.queue, d.fp16,
                d.cacheFile.empty() ? std::nullopt
                    : std::optional<std::filesystem::path>(d.cacheFile)
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to use the Vulkan device", e);
        }
    }
}

Instance::Instance(const ExternalDevice& device,
        const std::filesystem::path& shaderDllPath,
        bool allowLowPrecision) {
    this->m_impl = std::make_unique<InstanceImpl>(
        wrapExternalDevice(device), shaderDllPath, allowLowPrecision
    );
}

bool Instance::lowPrecision() const { return this->m_impl->lowPrecision(); }
void Instance::persistCache() const { this->m_impl->getVulkan().persistPipelineCache(); }

ContextImages Instance::images(const Context& context) {
    return context.images(context.isHdr()
        ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
}
uint64_t Instance::nextSignal(const Context& context) { return context.nextSignal(); }
uint64_t Instance::frameIndex(const Context& context) { return context.frameIndex(); }

Context& Instance::openContext(std::pair<int, int> sourceFds, const std::vector<int>& destFds,
        int syncFd, uint32_t width, uint32_t height,
        bool hdr, float flow, bool perf) {
    const VkExtent2D extent{ width, height };
    const auto& vk = this->m_impl->getVulkan();
    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    auto source = importImages(vk, sourceFds, extent, format);
    auto dest = importImages(vk, destFds, extent, format);
    auto sync = importTimelineSemaphore(vk, syncFd);
    return *this->m_contexts.emplace_back(std::make_unique<ContextImpl>(*this->m_impl,
        std::move(source), std::move(dest), std::move(sync),
        extent, hdr, flow, perf
    )).get();
}

Context& Instance::openLocalContext(size_t generated,
        uint32_t width, uint32_t height,
        bool hdr, float flow, bool perf) {
    if (generated < 1)
        throw backend::error("no frames to generate");
    const VkExtent2D extent{ width, height };
    const auto& vk = this->m_impl->getVulkan();
    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    std::pair<vk::Image, vk::Image> source = createLocalSourceImages(vk, extent, format);
    std::vector<vk::Image> dest = createLocalDestImages(vk, generated, extent, format);
    vk::TimelineSemaphore sync = createPrepassSemaphore(vk);
    return *this->m_contexts.emplace_back(std::make_unique<ContextImpl>(*this->m_impl,
        std::move(source), std::move(dest), std::move(sync),
        extent, hdr, flow, perf
    )).get();
}

namespace {
    /// import source images
    std::pair<vk::Image, vk::Image> importImages(const vk::Vulkan& vk,
            const std::pair<int, int>& sourceFds,
            VkExtent2D extent, VkFormat format) {
        try {
            return {
                vk::Image(vk, extent, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, sourceFds.first),
                vk::Image(vk, extent, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, sourceFds.second)
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to import destination images", e);
        }
    }
    /// import destination images
    std::vector<vk::Image> importImages(const vk::Vulkan& vk,
            const std::vector<int>& destFds,
            VkExtent2D extent, VkFormat format) {
        try {
            std::vector<vk::Image> destImages;
            destImages.reserve(destFds.size());

            for (const auto& fd : destFds)
                destImages.emplace_back(vk, extent, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, fd);

            return destImages;
        } catch (const std::exception& e) {
            throw backend::error("Unable to import destination images", e);
        }
    }
    /// create a black image
    vk::Image createBlackImage(const vk::Vulkan& vk) {
        try {
            return{vk,
                { .width = 4, .height = 4 }
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create black image", e);
        }
    }
    /// import timeline semaphore
    vk::TimelineSemaphore importTimelineSemaphore(const vk::Vulkan& vk, int syncFd) {
        try {
            return{vk, 0, syncFd};
        } catch (const std::exception& e) {
            throw backend::error("Unable to import timeline semaphore", e);
        }
    }
    /// create prepass semaphores
    vk::TimelineSemaphore createPrepassSemaphore(const vk::Vulkan& vk) {
        try {
            return{vk, 0};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create prepass semaphore", e);
        }
    }
    /// create command buffers
    std::vector<vk::CommandBuffer> createCommandBuffers(const vk::Vulkan& vk, size_t count) {
        try {
            std::vector<vk::CommandBuffer> cmdbufs;
            cmdbufs.reserve(count);

            for (size_t i = 0; i < count; ++i)
                cmdbufs.emplace_back(vk);

            return cmdbufs;
        } catch (const std::exception& e) {
            throw backend::error("Unable to create command buffers", e);
        }
    }
    /// create context data
    Ctx createCtx(const InstanceImpl& instance, VkExtent2D extent,
            bool hdr, float flow, bool perf, size_t count) {
        const auto& vk = instance.getVulkan();
        const auto& shaders = instance.getShaderRegistry();

        try {
            std::vector<vk::Buffer> constantBuffers{};
            constantBuffers.reserve(count);

            for (size_t i = 0; i < count; ++i)
                constantBuffers.emplace_back(vk,
                    backend::getDefaultConstantBuffer(
                        i, count, flow
                    )
                );

            return {
                .vk = std::ref(vk),
                .shaders = std::ref(shaders),
                .pool{vk, backend::calculateDescriptorPoolLimits(count, perf)},
                .constantBuffer{vk, backend::getDefaultConstantBuffer(0, 1, flow)},
                .constantBuffers{std::move(constantBuffers)},
                .bnbSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false},
                .bnwSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true},
                .eabSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false},
                .sourceExtent = extent,
                .flowExtent = VkExtent2D {
                    .width = static_cast<uint32_t>(static_cast<float>(extent.width) / flow),
                    .height = static_cast<uint32_t>(static_cast<float>(extent.height) / flow)
                },
                .hdr = hdr,
                .flow = flow,
                .perf = perf,
                .count = count
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create context", e);
        }
    }
}

ContextImpl::ContextImpl(const InstanceImpl& instance,
            std::pair<vk::Image, vk::Image> source, std::vector<vk::Image> dest,
            vk::TimelineSemaphore sync,
            VkExtent2D extent, bool hdr, float flow, bool perf) :
        sourceImages(std::move(source)),
        destImages(std::move(dest)),
        blackImage(createBlackImage(instance.getVulkan())),
        syncSemaphore(std::move(sync)),
        prepassSemaphore(createPrepassSemaphore(instance.getVulkan())),
        cmdbufs(createCommandBuffers(instance.getVulkan(), this->destImages.size() + 1)),
        cmdbufFence(instance.getVulkan()),
        ctx(createCtx(instance, extent, hdr, flow, perf, this->destImages.size())),
        mipmaps(ctx, sourceImages),
        alpha0{
            Alpha0(ctx, mipmaps.getImages().at(0)),
            Alpha0(ctx, mipmaps.getImages().at(1)),
            Alpha0(ctx, mipmaps.getImages().at(2)),
            Alpha0(ctx, mipmaps.getImages().at(3)),
            Alpha0(ctx, mipmaps.getImages().at(4)),
            Alpha0(ctx, mipmaps.getImages().at(5)),
            Alpha0(ctx, mipmaps.getImages().at(6))
        },
        alpha1{
            Alpha1(ctx, 3, alpha0.at(0).getImages()),
            Alpha1(ctx, 2, alpha0.at(1).getImages()),
            Alpha1(ctx, 2, alpha0.at(2).getImages()),
            Alpha1(ctx, 2, alpha0.at(3).getImages()),
            Alpha1(ctx, 2, alpha0.at(4).getImages()),
            Alpha1(ctx, 2, alpha0.at(5).getImages()),
            Alpha1(ctx, 2, alpha0.at(6).getImages())
        },
        beta0(ctx, alpha1.at(0).getImages()),
        beta1(ctx, beta0.getImages()) {
    // build main passes
    for (size_t i = 0; i < destImages.size(); ++i) {
        auto& pass = this->passes.emplace_back();

        pass.gamma0.reserve(7);
        pass.gamma1.reserve(7);
        pass.delta0.reserve(3);
        pass.delta1.reserve(3);
        for (size_t j = 0; j < 7; j++) {
            if (j == 0) { // first pass has no prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    this->blackImage,
                    this->beta1.getImages().at(5)
                );
            } else { // other passes use prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j - 1).getImage(),
                    this->beta1.getImages().at(6 - j)
                );
            }

            if (j == 4) { // first special pass has no prior data
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage,
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    this->blackImage,
                    this->beta1.getImages().at(6 - j),
                    this->blackImage
                );
            } else if (j > 4) { // further passes do
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.delta1.at(j - 5).getImage0(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    pass.delta1.at(j - 5).getImage0(),
                    this->beta1.getImages().at(6 - j),
                    pass.delta1.at(j - 5).getImage1()
                );
            }
        }

        pass.generate.emplace(ctx, i,
            this->sourceImages,
            pass.gamma1.at(6).getImage(),
            pass.delta1.at(2).getImage0(),
            pass.delta1.at(2).getImage1(),
            this->destImages.at(i)
        );
    }

    // initialize all images
    std::vector<VkImage> images{};
    images.push_back(this->blackImage.handle());
    // (WSword: the source and destination images too - the instance's own start undefined)
    images.push_back(this->sourceImages.first.handle());
    images.push_back(this->sourceImages.second.handle());
    for (const auto& image : this->destImages)
        images.push_back(image.handle());
    mipmaps.prepare(images);
    for (size_t i = 0; i < 7; ++i) {
        alpha0.at(i).prepare(images);
        alpha1.at(i).prepare(images);
    }
    beta0.prepare(images);
    beta1.prepare(images);
    for (const auto& pass : this->passes) {
        for (size_t i = 0; i < 7; ++i) {
            pass.gamma0.at(i).prepare(images);
            pass.gamma1.at(i).prepare(images);

            if (i < 4) continue;
            pass.delta0.at(i - 4).prepare(images);
            pass.delta1.at(i - 4).prepare(images);
        }
    }

    std::vector<vk::Barrier> barriers{};
    barriers.reserve(images.size());

    for (const auto& image : images) {
        barriers.emplace_back(vk::Barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });
    }

    const vk::CommandBuffer cmdbuf{ctx.vk};
    cmdbuf.begin(ctx.vk);
    cmdbuf.insertBarriers(ctx.vk, barriers);
    cmdbuf.end(ctx.vk);
    cmdbuf.submit(ctx.vk); // wait for completion
}

void Instance::scheduleFrames(Context& context) { // NOLINT (static)
#ifdef LSFGVK_TESTING_RENDERDOC
    const auto& impl = this->m_impl;
    if (impl->getRenderDocAPI()) {
        impl->getRenderDocAPI()->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
    try {
        context.scheduleFrames();
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
#ifdef LSFGVK_TESTING_RENDERDOC
    if (impl->getRenderDocAPI()) {
        impl->getVulkan().df().DeviceWaitIdle(impl->getVulkan().dev());
        impl->getRenderDocAPI()->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
}

void Instance::scheduleFrames(Context& context, const std::vector<size_t>& which) { // NOLINT (static)
    try {
        context.scheduleFrames(which);
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
}

void Context::scheduleFrames() {
    std::vector<size_t> all(this->destImages.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    this->scheduleFrames(all);
}

void Context::scheduleFrames(const std::vector<size_t>& which) {
    for (size_t k = 0; k < which.size(); ++k)
        if (which[k] >= this->destImages.size() || (k && which[k] <= which[k - 1]))
            throw backend::error("bad pass selection");
    // wait for previous pre-pass to complete
    if (this->fidx && !this->cmdbufFence.wait(this->ctx.vk))
        throw backend::error("Timeout waiting for previous frame to complete");
    this->cmdbufFence.reset(this->ctx.vk);

    // schedule pre-pass
    const auto& cmdbuf = this->cmdbufs.at(0);
    cmdbuf.begin(ctx.vk);

    this->mipmaps.render(ctx.vk, cmdbuf, this->fidx);
    for (size_t i = 0; i < 7; ++i) {
        this->alpha0.at(6 - i).render(ctx.vk, cmdbuf);
        this->alpha1.at(6 - i).render(ctx.vk, cmdbuf, this->fidx);
    }
    this->beta0.render(ctx.vk, cmdbuf, this->fidx);
    this->beta1.render(ctx.vk, cmdbuf);

    cmdbuf.end(ctx.vk);
    cmdbuf.submit(this->ctx.vk,
        {}, this->syncSemaphore.handle(), this->idx,
        {}, this->prepassSemaphore.handle(), this->idx,
        which.empty() ? this->cmdbufFence.handle() : VK_NULL_HANDLE // (WSword: no main pass - the fence here)
    );

    this->idx++;

    // schedule main passes (WSword: the chosen ones, signalled one after another)
    for (size_t k = 0; k < which.size(); k++) {
        const size_t i = which[k];
        const auto& cmdbuf = this->cmdbufs.at(i + 1);
        cmdbuf.begin(ctx.vk);

        const auto& pass = this->passes.at(i);
        for (size_t j = 0; j < 7; j++) {
            pass.gamma0.at(j).render(ctx.vk, cmdbuf, this->fidx);
            pass.gamma1.at(j).render(ctx.vk, cmdbuf);

            if (j < 4) continue;
            pass.delta0.at(j - 4).render(ctx.vk, cmdbuf, this->fidx);
            pass.delta1.at(j - 4).render(ctx.vk, cmdbuf);
        }
        pass.generate->render(ctx.vk, cmdbuf, this->fidx);

        cmdbuf.end(ctx.vk);
        cmdbuf.submit(this->ctx.vk,
            {}, this->prepassSemaphore.handle(), this->idx - 1,
            {}, this->syncSemaphore.handle(), this->idx + k,
            k == which.size() - 1 ? this->cmdbufFence.handle() : VK_NULL_HANDLE
        );
    }

    this->idx += which.size();
    this->fidx++;
}

void Instance::closeContext(const Context& context) {
    auto it = std::ranges::find_if(this->m_contexts,
        [context = &context](const std::unique_ptr<ContextImpl>& ctx) {
            return ctx.get() == context;
        });
    if (it == this->m_contexts.end())
        throw backend::error("attempted to close unknown context",
            std::runtime_error("no such context"));

    const auto& vk = this->m_impl->getVulkan();
    vk.df().DeviceWaitIdle(vk.dev());

    this->m_contexts.erase(it);
}

Instance::~Instance() = default;

int backend::checkShaderDll(const std::filesystem::path& shaderDllPath) {
    std::unordered_map<uint32_t, std::vector<uint8_t>> resources{};
    try {
        resources = backend::extractResourcesFromDLL(shaderDllPath);
    } catch (const std::exception& e) {
        throw backend::error("Unable to parse Lossless Scaling DLL", e);
    }
    // (extraction/shader_registry.cpp: resource 49 + id, + 23 for the performance set,
    // + 49 for full precision; ids 255-279, the performance set 257-279)
    const auto complete = [&resources](bool fp16) {
        for (uint32_t id = 255; id <= 279; ++id) {
            const uint32_t base = 49 + id + (fp16 ? 0 : 49);
            if (!resources.contains(base)) return false;
            if (id >= 257 && !resources.contains(base + 23)) return false;
        }
        return true;
    };
    const int sets = (complete(false) ? 1 : 0) | (complete(true) ? 2 : 0);
    if (!sets)
        throw backend::error("This Lossless.dll lacks the frame generation shaders ("
            + std::to_string(resources.size()) + " resources)");
    return sets;
}

// leaking shenanigans

namespace {
    bool leaking{false}; // NOLINT (global variable)
}

InstanceImpl::~InstanceImpl() {
    if (!leaking) return;

    try {
        new vk::Vulkan(std::move(this->vk));
    } catch (...) {
        std::cerr << "lsfg-vk: failed to leak Vulkan instance\n";
    }

}

void backend::makeLeaking() {
    leaking = true;
}
