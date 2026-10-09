#include "render_pt.hpp"

#if !defined(ASPERA_HAS_VULKAN_PT)

namespace aspera {
std::unique_ptr<NativePt> makeVulkanPt() {
    return {};
}
} // namespace aspera

#else

// The Vulkan path tracer deliberately lives outside bgfx.  The raster renderer and this
// backend therefore never share a VkDevice, images, buffers, or descriptor sets.  This is
// slightly more code than routing the output through bgfx, but it makes device loss and
// swapchain ownership unambiguous and lets the ray-query backend use the same scene data on
// every desktop platform.

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aspera {
namespace {
constexpr const char *VulkanUiBackendName = "aspera_vulkan_path_tracer";

// The temporal images, scene buffers, and uniform storage are intentionally
// shared by the compute passes. Keep one submitted frame at a time so a slot
// fence covers every shared resource before it is updated by the next frame.
constexpr uint32_t FramesInFlight = 1;
constexpr uint32_t TimestampTraceBegin = 0;
constexpr uint32_t TimestampTraceEnd = 1;
constexpr uint32_t TimestampDenoiseBegin = 2;
constexpr uint32_t TimestampDenoiseEnd = 3;
constexpr uint32_t TimestampQueryCount = 4;
constexpr uint32_t TimestampAsBegin = 0;
constexpr uint32_t TimestampAsEnd = 1;
constexpr uint32_t TimestampAsQueryCount = 2;
// Bindings 0..7 are the parameter/scene buffers and TLAS.  Every PtImage gets
// its own storage-image binding starting at eight; keeping this derived from
// the ABI lets the temporal history extension add images without a second
// Vulkan descriptor layout.
constexpr uint32_t DescriptorCount = 8 + PtImageCount;
constexpr uint32_t ComputeGroupSize = 8;
constexpr VkDeviceSize MinimumBufferSize = 16;

[[noreturn]] void failVk(const char *what, VkResult result) {
    throw std::runtime_error(std::string(what) + " (VkResult " + std::to_string(int(result)) + ')');
}
void vkCheck(VkResult result, const char *what) {
    if (result != VK_SUCCESS)
        failVk(what, result);
}
uint64_t timestampMask(uint32_t validBits) {
    if (validBits >= 64)
        return std::numeric_limits<uint64_t>::max();
    if (validBits == 0)
        return 0;
    return (uint64_t(1) << validBits) - 1;
}
uint64_t timestampDelta(uint64_t begin, uint64_t end, uint64_t mask) {
    return (end - begin) & mask;
}
bool hasName(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const auto &e) { return std::strcmp(e.extensionName, name) == 0; });
}
std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkCheck(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> result(count);
    if (count)
        vkCheck(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, result.data()),
                "vkEnumerateDeviceExtensionProperties");
    return result;
}
std::vector<uint8_t> readBinary(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const auto end = file.tellg();
    if (end <= 0 || end > std::streamoff(256 * 1024 * 1024))
        return {};
    std::vector<uint8_t> data(static_cast<size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file || data.size() < sizeof(uint32_t) || (data.size() & 3u))
        return {};
    return data;
}
std::filesystem::path shaderPath(std::string_view name) {
    std::vector<std::filesystem::path> roots;
#ifdef ASPERA_PT_SHADER_DIR
    roots.emplace_back(ASPERA_PT_SHADER_DIR);
#endif
#ifdef ASPERA_SHADER_DIR
    roots.emplace_back(ASPERA_SHADER_DIR);
#endif
    if (const char *base = SDL_GetBasePath()) {
        roots.emplace_back(std::filesystem::path(base) / "shaders");
        roots.emplace_back(std::filesystem::path(base) / "../share/aspera/shaders");
    }
    roots.emplace_back("assets/shaders");
    for (const auto &root : roots) {
        const auto direct = root / std::string(name);
        if (std::filesystem::is_regular_file(direct))
            return direct;
        const auto vulkan = root / "vulkan" / std::string(name);
        if (std::filesystem::is_regular_file(vulkan))
            return vulkan;
        const auto native = root / "pt" / std::string(name);
        if (std::filesystem::is_regular_file(native))
            return native;
        const auto spv = root / (std::string(name) + ".spv");
        if (std::filesystem::is_regular_file(spv))
            return spv;
    }
    return {};
}

struct Buffer {
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkBuffer handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{};
    VkDeviceAddress address{};
    void *mapped{};
    bool hostVisible{};

    void reset() {
        if (mapped) {
            vkUnmapMemory(device, memory);
            mapped = nullptr;
        }
        if (handle)
            vkDestroyBuffer(device, handle, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        handle = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        size = 0;
        address = 0;
        hostVisible = false;
    }
};

struct Image {
    VkDevice device{};
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{}, height{};

    void reset() {
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        width = height = 0;
    }
};

struct Accel {
    VkDevice device{};
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    Buffer storage;
    VkDeviceAddress address{};
    VkDeviceSize primitiveCount{};
    uint64_t key{};

    void reset() {
        if (handle)
            vkDestroyAccelerationStructureKHR(device, handle, nullptr);
        handle = VK_NULL_HANDLE;
        storage.reset();
        address = 0;
        primitiveCount = 0;
        key = 0;
    }
};

struct Frame {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
    VkSemaphore renderFinished{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkQueryPool timestampPool{VK_NULL_HANDLE};
    uint64_t timestampSerial{};
    bool timestampPending{};
};

struct RetiredBuffer {
    Buffer resource;
};

struct CachedBlas {
    Accel accel;
    uint64_t id{};
    uint64_t key{};
    uint32_t vertexOffset{}, vertexCount{}, indexOffset{}, indexCount{};
};

struct ImGuiResources {
    VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
    VkDescriptorPool pool{VK_NULL_HANDLE};
    VkDescriptorSet fontSet{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    Image fontImage;
    VkSampler sampler{VK_NULL_HANDLE};
    std::array<Buffer, FramesInFlight> vertices;
    std::array<Buffer, FramesInFlight> indices;
};

class VulkanPt final : public NativePt {
  public:
    ~VulkanPt() override {
        try {
            end();
        } catch (...) {
        }
        shutdown();
    }

    bool init(SDL_Window *window, uint32_t width, uint32_t height, std::string &reason) override;
    void resize(uint32_t width, uint32_t height) override;
    bool render(const PtScene &scene, const PtParams &params) override;
    void drawImGui(ImDrawData *draw) override;
    void end() override;
    void screenshot(const std::filesystem::path &path) override;
    void setVsync(bool enabled) override;
    RenderTimings timings() const override;

  private:
    bool initializeInstance(SDL_Window *, std::string &reason);
    bool initializeDevice(std::string &reason);
    bool initializeSwapchain(uint32_t width, uint32_t height, std::string &reason);
    bool initializeResources(std::string &reason);
    bool initializePipelines(std::string &reason);
    bool initializeImGui(std::string &reason);
    bool chooseDevice(std::string &reason);
    bool findQueueFamily();
    void shutdown();
    void waitIdle();
    void collectFrameTimestamps(Frame &frame);
    void destroySwapchain();
    void destroyResources();
    void destroyPipelines();
    void recreateSwapchain(uint32_t width, uint32_t height);
    void ensureImageTargets(uint32_t width, uint32_t height);
    void clearTargets(VkCommandBuffer command);
    void updateScene(const PtScene &scene);
    void updateDescriptorSet();
    void uploadBuffers(const PtScene &scene);
    void updateAcceleration(const PtScene &scene);
    void buildBlas(CachedBlas &cache, const PtGeometry &geometry);
    void updateTlas(const PtScene &scene, bool updateOnly = false);
    VkCommandBuffer beginImmediate(bool timestampAcceleration = false);
    void endImmediate(VkCommandBuffer command, bool timestampAcceleration = false);
    void beginFrame();
    void recordCompute(VkCommandBuffer command, const PtParams &params);
    void recordBlit(VkCommandBuffer command);
    void recordScreenshot(VkCommandBuffer command);
    void completeScreenshot();
    void checkDeviceLost(VkResult result, const char *what);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags properties) const;
    void createBuffer(Buffer &, VkDeviceSize, VkBufferUsageFlags, VkMemoryPropertyFlags);
    void upload(Buffer &, const void *, VkDeviceSize);
    void createImage(Image &, uint32_t width, uint32_t height, VkFormat, VkImageUsageFlags);
    VkShaderModule shader(std::string_view name);
    VkPipeline makeComputePipeline(std::string_view shaderName, VkPipelineLayout);
    VkPipeline makeBlitPipeline(std::string_view vertexName, std::string_view fragmentName);
    VkPipeline makeImGuiPipeline(std::string_view vertexName, std::string_view fragmentName);
    VkImageView imageViewFor(PtImage image) const {
        return targets[image].view;
    }
    const CachedBlas *blasForInstance(const PtScene &, size_t) const;

    SDL_Window *window{};
    VkInstance instance{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{};
    double timestampPeriodMs{};
    uint32_t timestampValidBits{};
    bool timestampsAvailable{};
    VkQueryPool accelerationTimestampPool{VK_NULL_HANDLE};
    bool accelerationTimestampsAvailable{};
    uint64_t nextTimestampSerial{1};
    uint64_t completedTimestampSerial{};
    double accelerationGpuMs{-1.0};
    bool accelerationTimingValid{};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchainFormat{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass{VK_NULL_HANDLE};
    bool vsync{true};
    bool linearOutput{true};
    bool initialized{};
    bool activeFrame{};
    bool swapchainOutOfDate{};
    uint32_t frameIndex{};
    uint32_t acquiredImage{};
    Frame frames[FramesInFlight]{};

    VkDescriptorSetLayout computeSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet computeSet{VK_NULL_HANDLE};
    VkPipelineLayout computeLayout{VK_NULL_HANDLE};
    VkPipeline tracePipeline{VK_NULL_HANDLE};
    VkPipeline temporalPipeline{VK_NULL_HANDLE};
    VkPipeline filterPipeline{VK_NULL_HANDLE};
    VkPipeline postPipeline{VK_NULL_HANDLE};
    VkDescriptorSetLayout blitSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool blitPool{VK_NULL_HANDLE};
    VkDescriptorSet blitSet{VK_NULL_HANDLE};
    VkPipelineLayout blitLayout{VK_NULL_HANDLE};
    VkPipeline blitPipeline{VK_NULL_HANDLE};
    VkSampler blitSampler{VK_NULL_HANDLE};
    VkDescriptorSetLayout imguiSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool imguiPool{VK_NULL_HANDLE};
    VkPipelineLayout imguiLayout{VK_NULL_HANDLE};
    VkPipeline imguiPipeline{VK_NULL_HANDLE};

    std::array<Image, PtImageCount> targets{};
    Buffer paramsBuffer;
    Buffer verticesBuffer;
    Buffer indicesBuffer;
    Buffer instancesBuffer;
    Buffer materialsBuffer;
    Buffer lightsBuffer;
    Buffer emittersBuffer;
    Buffer tlasInstancesBuffer;
    Buffer scratchBuffer;
    std::map<uint64_t, CachedBlas> blases;
    Accel tlas;
    uint64_t bufferGeneration{};
    uint64_t lastGeometryRevision{std::numeric_limits<uint64_t>::max()};
    uint64_t lastTransformRevision{std::numeric_limits<uint64_t>::max()};
    uint64_t lastLightingRevision{std::numeric_limits<uint64_t>::max()};
    uint64_t accelerationBufferGeneration{};
    uint64_t uploadedGeometryRevision{std::numeric_limits<uint64_t>::max()};
    uint64_t uploadedTransformRevision{std::numeric_limits<uint64_t>::max()};
    uint64_t uploadedLightingRevision{std::numeric_limits<uint64_t>::max()};
    bool descriptorDirty{true};
    std::array<uint8_t, PtImageCount> targetLayout{};
    PtParams lastParams{};
    bool historyReset{true};
    uint32_t accumulatedSamples{};

    ImGuiResources imgui;
    std::filesystem::path pendingScreenshot;
    std::filesystem::path screenshotInFlight;
    uint32_t screenshotFrame{};
    Buffer screenshotBuffer;
    uint32_t screenshotWidth{}, screenshotHeight{};

    RenderTimings stats{};
    std::chrono::steady_clock::time_point frameStart{};
    double accelerationMs{-1.0};
    double traceMs{-1.0};
    double denoiseMs{-1.0};
    uint64_t gpuMemoryBytes{};
    uint32_t lastSamples{1}, lastBounces{6};
    bool haveDenoiseSetting{};
    bool previousDenoise{true};
};

uint32_t VulkanPt::memoryType(uint32_t bits, VkMemoryPropertyFlags propertiesWanted) const {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & propertiesWanted) == propertiesWanted)
            return i;
    throw std::runtime_error("Vulkan device has no compatible memory type");
}

void VulkanPt::createBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags propertiesWanted) {
    buffer.reset();
    buffer.device = device;
    buffer.physical = physical;
    buffer.size = std::max(size, MinimumBufferSize);
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = buffer.size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device, &create, nullptr, &buffer.handle), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    VkMemoryPropertyFlags actual = propertiesWanted;
    uint32_t type = 0;
    try {
        type = memoryType(requirements.memoryTypeBits, actual);
    } catch (...) {
        actual = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        type = memoryType(requirements.memoryTypeBits, actual);
    }
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = flags.flags ? &flags : nullptr;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    vkCheck(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory),
            "vkAllocateMemory(buffer)");
    vkCheck(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
    buffer.hostVisible = (actual & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (buffer.hostVisible)
        vkCheck(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &buffer.mapped),
                "vkMapMemory(buffer)");
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = buffer.handle;
        buffer.address = vkGetBufferDeviceAddress(device, &address);
    }
    gpuMemoryBytes += requirements.size;
}

void VulkanPt::upload(Buffer &buffer, const void *data, VkDeviceSize size) {
    if (!buffer.handle || size == 0)
        return;
    if (!buffer.hostVisible)
        throw std::runtime_error("Vulkan path tracer upload buffer is not host visible");
    std::memcpy(buffer.mapped, data, static_cast<size_t>(std::min(size, buffer.size)));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer.memory;
    range.size = VK_WHOLE_SIZE;
    vkCheck(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
}

void VulkanPt::createImage(Image &image, uint32_t width, uint32_t height, VkFormat format,
                           VkImageUsageFlags usage) {
    image.reset();
    image.device = device;
    image.width = width;
    image.height = height;
    image.format = format;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = usage;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateImage(device, &create, nullptr, &image.image), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(device, &allocate, nullptr, &image.memory), "vkAllocateMemory(image)");
    vkCheck(vkBindImageMemory(device, image.image, image.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(device, &view, nullptr, &image.view), "vkCreateImageView(image)");
    gpuMemoryBytes += requirements.size;
}

VkCommandBuffer VulkanPt::beginImmediate(bool timestampAcceleration) {
    VkCommandPool pool = frames[0].pool;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command{};
    vkCheck(vkAllocateCommandBuffers(device, &allocate, &command),
            "vkAllocateCommandBuffers(immediate)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(immediate)");
    if (timestampAcceleration && accelerationTimestampsAvailable) {
        vkCmdResetQueryPool(command, accelerationTimestampPool, 0, TimestampAsQueryCount);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, accelerationTimestampPool,
                            TimestampAsBegin);
    }
    return command;
}

void VulkanPt::endImmediate(VkCommandBuffer command, bool timestampAcceleration) {
    if (timestampAcceleration && accelerationTimestampsAvailable)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                            accelerationTimestampPool, TimestampAsEnd);
    vkCheck(vkEndCommandBuffer(command), "vkEndCommandBuffer(immediate)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(immediate)");
    vkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle(immediate)");
    if (timestampAcceleration && accelerationTimestampsAvailable) {
        uint64_t timestamps[TimestampAsQueryCount]{};
        const VkResult result = vkGetQueryPoolResults(
            device, accelerationTimestampPool, 0, TimestampAsQueryCount, sizeof(timestamps),
            timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            const uint64_t mask = timestampMask(timestampValidBits);
            accelerationGpuMs += double(timestampDelta(timestamps[TimestampAsBegin] & mask,
                                                       timestamps[TimestampAsEnd] & mask, mask)) *
                                 timestampPeriodMs;
        } else
            accelerationTimingValid = false;
    }
    vkFreeCommandBuffers(device, frames[0].pool, 1, &command);
}

void VulkanPt::ensureImageTargets(uint32_t width, uint32_t height) {
    for (auto &target : targets)
        target.reset();
    targetLayout.fill(uint8_t(VK_IMAGE_LAYOUT_UNDEFINED));
    for (auto &target : targets)
        createImage(target, width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    descriptorDirty = true;
    if (blitSet) {
        VkDescriptorImageInfo info{};
        info.imageView = targets[PtOutput].view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.sampler = blitSampler;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = blitSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    historyReset = true;
}

void VulkanPt::clearTargets(VkCommandBuffer command) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    for (size_t i = 0; i < targets.size(); ++i) {
        auto &target = targets[i];
        const VkImageLayout oldLayout = static_cast<VkImageLayout>(targetLayout[i]);
        barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                    ? 0
                                    : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                          VK_ACCESS_TRANSFER_READ_BIT |
                                          VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = oldLayout;
        const VkPipelineStageFlags sourceStage = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                                                     ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                     : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        barrier.image = target.image;
        vkCmdPipelineBarrier(command, sourceStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        const VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        targetLayout[i] = uint8_t(VK_IMAGE_LAYOUT_GENERAL);
    }
}

VkShaderModule VulkanPt::shader(std::string_view name) {
    const auto path = shaderPath(name);
    if (path.empty())
        throw std::runtime_error("Missing Vulkan path-tracing shader: " + std::string(name));
    auto bytes = readBinary(path);
    if (bytes.empty())
        throw std::runtime_error("Invalid Vulkan path-tracing shader: " + path.string());
    VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = bytes.size();
    create.pCode = reinterpret_cast<const uint32_t *>(bytes.data());
    VkShaderModule module{};
    vkCheck(vkCreateShaderModule(device, &create, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkPipeline VulkanPt::makeComputePipeline(std::string_view shaderName, VkPipelineLayout layout) {
    VkShaderModule module = shader(shaderName);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = stage;
    create.layout = layout;
    VkPipeline pipeline{};
    VkResult result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vkCheck(result, "vkCreateComputePipelines");
    return pipeline;
}

VkPipeline VulkanPt::makeBlitPipeline(std::string_view vertexName, std::string_view fragmentName) {
    VkShaderModule vert = shader(vertexName), frag{};
    try {
        frag = shader(fragmentName);
    } catch (...) {
        vkDestroyShaderModule(device, vert, nullptr);
        throw;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module = vert;
    stages[1].module = frag;
    stages[0].pName = stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // pt_blit.vert maps NDC Y to image UVs with a Vulkan-style bottom-up
    // viewport. Keep the viewport negative-height so the compute image's row
    // zero (the top row) is presented without an extra vertical flip.
    VkViewport viewport{0,
                        float(swapchainExtent.height),
                        float(swapchainExtent.width),
                        -float(swapchainExtent.height),
                        0,
                        1};
    VkRect2D scissor{{0, 0}, swapchainExtent};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertexInput;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewportState;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pColorBlendState = &blending;
    create.layout = blitLayout;
    create.renderPass = renderPass;
    create.subpass = 0;
    VkPipeline pipeline{};
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    vkCheck(result, "vkCreateGraphicsPipelines(blit)");
    return pipeline;
}

VkPipeline VulkanPt::makeImGuiPipeline(std::string_view vertexName, std::string_view fragmentName) {
    VkShaderModule vert = shader(vertexName), frag{};
    try {
        frag = shader(fragmentName);
    } catch (...) {
        vkDestroyShaderModule(device, vert, nullptr);
        throw;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module = vert;
    stages[1].module = frag;
    stages[0].pName = stages[1].pName = "main";
    const std::array<VkVertexInputBindingDescription, 1> bindings{{
        {0, static_cast<uint32_t>(sizeof(ImDrawVert)), VK_VERTEX_INPUT_RATE_VERTEX},
    }};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ImDrawVert, pos))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(ImDrawVert, uv))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(ImDrawVert, col))},
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(swapchainExtent.width), float(swapchainExtent.height), 0, 1};
    VkRect2D scissor{{0, 0}, swapchainExtent};
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const std::array<VkDynamicState, 2> dynamicStates{
        {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertexInput;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewportState;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pColorBlendState = &blending;
    create.pDynamicState = &dynamic;
    create.layout = imguiLayout;
    create.renderPass = renderPass;
    create.subpass = 0;
    VkPipeline pipeline{};
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    vkCheck(result, "vkCreateGraphicsPipelines(imgui)");
    return pipeline;
}

bool VulkanPt::findQueueFamily() {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present) != VK_SUCCESS)
            continue;
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            present) {
            queueFamily = i;
            return true;
        }
    }
    return false;
}

bool VulkanPt::chooseDevice(std::string &reason) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        reason = "No Vulkan physical device was found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
            "vkEnumeratePhysicalDevices");
    std::string lastFailure;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties candidateProperties{};
        vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
        if (VK_VERSION_MAJOR(candidateProperties.apiVersion) < 1 ||
            (VK_VERSION_MAJOR(candidateProperties.apiVersion) == 1 &&
             VK_VERSION_MINOR(candidateProperties.apiVersion) < 2)) {
            lastFailure = "Vulkan 1.2 is required";
            continue;
        }
        const auto extensions = deviceExtensions(candidate);
        const bool extensionsOk =
            hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
            hasName(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            hasName(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
            hasName(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        if (!extensionsOk) {
            lastFailure = "Vulkan ray-query, acceleration-structure, deferred-host, and swapchain "
                          "extensions are required";
            continue;
        }
        physical = candidate;
        if (!findQueueFamily()) {
            lastFailure = "No Vulkan queue supports graphics, compute, and presentation";
            physical = VK_NULL_HANDLE;
            continue;
        }
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features vulkan12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR query{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        features.pNext = &vulkan12;
        vulkan12.pNext = &acceleration;
        acceleration.pNext = &query;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!vulkan12.bufferDeviceAddress || !acceleration.accelerationStructure ||
            !query.rayQuery) {
            lastFailure = "Vulkan device lacks buffer-device-address, acceleration-structure, or "
                          "ray-query support";
            physical = VK_NULL_HANDLE;
            continue;
        }
        properties = candidateProperties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        asProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &asProperties;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        return true;
    }
    reason = lastFailure.empty() ? "No Vulkan device supports path tracing" : lastFailure;
    return false;
}

bool VulkanPt::initializeInstance(SDL_Window *newWindow, std::string &reason) {
    window = newWindow;
    if (!window) {
        reason = "Vulkan path tracing requires a valid SDL window";
        return false;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        reason = std::string("SDL could not load Vulkan: ") + SDL_GetError();
        return false;
    }
    const auto getProc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!getProc) {
        reason = "SDL did not provide vkGetInstanceProcAddr";
        return false;
    }
    uint32_t extensionCount = 0;
    const char *const *required = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!required || extensionCount == 0) {
        reason = std::string("SDL Vulkan instance extensions unavailable: ") + SDL_GetError();
        return false;
    }
    std::vector<const char *> extensions(required, required + extensionCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Per Aspera path tracer";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Per Aspera";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_2;
    std::vector<const char *> layers;
    if (std::getenv("ASPERA_VULKAN_VALIDATION"))
        layers.push_back("VK_LAYER_KHRONOS_validation");
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    const VkResult result = vkCreateInstance(&create, nullptr, &instance);
    if (result != VK_SUCCESS) {
        reason = "vkCreateInstance failed (VkResult " + std::to_string(int(result)) + ')';
        instance = VK_NULL_HANDLE;
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        reason = std::string("SDL could not create a Vulkan surface: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool VulkanPt::initializeDevice(std::string &reason) {
    if (!chooseDevice(reason))
        return false;
    float priority = 1.f;
    VkDeviceQueueCreateInfo queueCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCreate.queueFamilyIndex = queueFamily;
    queueCreate.queueCount = 1;
    queueCreate.pQueuePriorities = &priority;
    const std::array<const char *, 4> extensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    acceleration.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    query.rayQuery = VK_TRUE;
    vulkan12.pNext = &acceleration;
    acceleration.pNext = &query;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &vulkan12;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.pNext = &features;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queueCreate;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    const VkResult result = vkCreateDevice(physical, &create, nullptr, &device);
    if (result != VK_SUCCESS) {
        reason = "vkCreateDevice failed (VkResult " + std::to_string(int(result)) + ')';
        device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> familyProperties(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, familyProperties.data());
    timestampValidBits = queueFamily < familyProperties.size()
                             ? familyProperties[queueFamily].timestampValidBits
                             : 0;
    timestampsAvailable = properties.limits.timestampPeriod > 0.0f && timestampValidBits != 0 &&
                          properties.limits.timestampComputeAndGraphics == VK_TRUE;
    timestampPeriodMs =
        timestampsAvailable ? double(properties.limits.timestampPeriod) * 1.0e-6 : 0.0;
    if (timestampsAvailable) {
        VkQueryPoolCreateInfo queryPool{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryPool.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryPool.queryCount = TimestampQueryCount;
        for (auto &frame : frames) {
            if (vkCreateQueryPool(device, &queryPool, nullptr, &frame.timestampPool) !=
                VK_SUCCESS) {
                timestampsAvailable = false;
                timestampPeriodMs = 0.0;
                for (auto &created : frames) {
                    if (created.timestampPool)
                        vkDestroyQueryPool(device, created.timestampPool, nullptr);
                    created.timestampPool = VK_NULL_HANDLE;
                }
                break;
            }
        }
    }
    if (timestampsAvailable) {
        VkQueryPoolCreateInfo queryPool{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryPool.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryPool.queryCount = TimestampAsQueryCount;
        if (vkCreateQueryPool(device, &queryPool, nullptr, &accelerationTimestampPool) !=
            VK_SUCCESS)
            accelerationTimestampsAvailable = false;
        else
            accelerationTimestampsAvailable = true;
    }
    for (auto &frame : frames) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queueFamily;
        vkCheck(vkCreateCommandPool(device, &pool, nullptr, &frame.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = frame.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device, &allocate, &frame.command),
                "vkAllocateCommandBuffers(frame)");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(device, &semaphore, nullptr, &frame.imageAvailable),
                "vkCreateSemaphore(image)");
        vkCheck(vkCreateSemaphore(device, &semaphore, nullptr, &frame.renderFinished),
                "vkCreateSemaphore(render)");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(device, &fence, nullptr, &frame.fence), "vkCreateFence");
    }
    return true;
}

bool VulkanPt::initializeSwapchain(uint32_t width, uint32_t height, std::string &reason) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    uint32_t formatCount = 0;
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr),
            "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount)
        vkCheck(
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()),
            "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formats.empty()) {
        reason = "Vulkan surface has no supported swapchain formats";
        return false;
    }
    // pt_post.comp performs the linear-to-sRGB display encoding explicitly.
    // Prefer an UNORM attachment so the presentation engine does not apply a
    // second implicit sRGB conversion.  Fall back to the common sRGB format
    // only when the surface does not expose an UNORM attachment.
    auto format = std::find_if(formats.begin(), formats.end(), [](const auto &f) {
        return f.format == VK_FORMAT_B8G8R8A8_UNORM &&
               f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (format == formats.end())
        format = std::find_if(formats.begin(), formats.end(), [](const auto &f) {
            return f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM;
        });
    if (format == formats.end())
        format = std::find_if(formats.begin(), formats.end(), [](const auto &f) {
            return f.format == VK_FORMAT_B8G8R8A8_SRGB &&
                   f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
    if (format == formats.end())
        format = formats.begin();
    VkSurfaceFormatKHR chosenFormat = *format;
    if (chosenFormat.format == VK_FORMAT_UNDEFINED)
        chosenFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
    swapchainFormat = chosenFormat.format;
    if (swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM && swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB &&
        swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM && swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB) {
        reason = "Vulkan surface format is not supported by native screenshot export";
        return false;
    }
    uint32_t modeCount = 0;
    vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr),
            "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount)
        vkCheck(
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes.data()),
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) !=
                 modes.end())
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    swapchainExtent = capabilities.currentExtent;
    if (swapchainExtent.width == std::numeric_limits<uint32_t>::max()) {
        swapchainExtent.width =
            std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        swapchainExtent.height = std::clamp(height, capabilities.minImageExtent.height,
                                            capabilities.maxImageExtent.height);
    }
    if (swapchainExtent.width == 0 || swapchainExtent.height == 0) {
        reason = "Vulkan surface is minimized or has zero drawable extent";
        return false;
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        reason = "Vulkan surface does not support transfer-source swapchain images required for "
                 "screenshots";
        return false;
    }
    uint32_t imageCount = std::max(2u, capabilities.minImageCount);
    if (capabilities.maxImageCount)
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface;
    create.minImageCount = imageCount;
    create.imageFormat = chosenFormat.format;
    create.imageColorSpace = chosenFormat.colorSpace;
    create.imageExtent = swapchainExtent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create.presentMode = presentMode;
    create.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain), "vkCreateSwapchainKHR");
    uint32_t actualCount = 0;
    vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr),
            "vkGetSwapchainImagesKHR(count)");
    swapchainImages.resize(actualCount);
    vkCheck(vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data()),
            "vkGetSwapchainImagesKHR");
    swapchainViews.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapchainImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchainFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        vkCheck(vkCreateImageView(device, &view, nullptr, &swapchainViews[i]),
                "vkCreateImageView(swapchain)");
    }
    VkAttachmentDescription attachment{};
    attachment.format = swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo renderPassCreate{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassCreate.attachmentCount = 1;
    renderPassCreate.pAttachments = &attachment;
    renderPassCreate.subpassCount = 1;
    renderPassCreate.pSubpasses = &subpass;
    renderPassCreate.dependencyCount = 1;
    renderPassCreate.pDependencies = &dependency;
    vkCheck(vkCreateRenderPass(device, &renderPassCreate, nullptr, &renderPass),
            "vkCreateRenderPass");
    framebuffers.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &swapchainViews[i];
        framebuffer.width = swapchainExtent.width;
        framebuffer.height = swapchainExtent.height;
        framebuffer.layers = 1;
        vkCheck(vkCreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[i]),
                "vkCreateFramebuffer");
    }
    return true;
}

void VulkanPt::destroySwapchain() {
    if (!device)
        return;
    for (auto framebuffer : framebuffers)
        if (framebuffer)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    if (renderPass)
        vkDestroyRenderPass(device, renderPass, nullptr);
    renderPass = VK_NULL_HANDLE;
    for (auto view : swapchainViews)
        if (view)
            vkDestroyImageView(device, view, nullptr);
    swapchainViews.clear();
    swapchainImages.clear();
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void VulkanPt::recreateSwapchain(uint32_t width, uint32_t height) {
    waitIdle();
    destroySwapchain();
    std::string reason;
    if (!initializeSwapchain(width, height, reason))
        throw std::runtime_error("Could not recreate Vulkan swapchain: " + reason);
    if (blitPipeline)
        vkDestroyPipeline(device, blitPipeline, nullptr);
    if (imguiPipeline)
        vkDestroyPipeline(device, imguiPipeline, nullptr);
    blitPipeline = VK_NULL_HANDLE;
    imguiPipeline = VK_NULL_HANDLE;
    blitPipeline = makeBlitPipeline("pt_blit.vert.spv", "pt_blit.frag.spv");
    imguiPipeline = makeImGuiPipeline("pt_imgui.vert.spv", "pt_imgui.frag.spv");
    ensureImageTargets(swapchainExtent.width, swapchainExtent.height);
    swapchainOutOfDate = false;
}

bool VulkanPt::initializeResources(std::string &reason) {
    try {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32A32_SFLOAT,
                                            &formatProperties);
        constexpr VkFormatFeatureFlags requiredImageFeatures =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((formatProperties.optimalTilingFeatures & requiredImageFeatures) !=
            requiredImageFeatures) {
            reason = "Vulkan device does not support sampled rgba32f storage images";
            return false;
        }
        linearOutput = (formatProperties.optimalTilingFeatures &
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        VkDescriptorSetLayoutBinding bindings[DescriptorCount]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                       nullptr};
        for (uint32_t i = 1; i <= 6; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                           nullptr};
        bindings[7] = {7, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 8; i < DescriptorCount; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                           nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = DescriptorCount;
        layout.pBindings = bindings;
        vkCheck(vkCreateDescriptorSetLayout(device, &layout, nullptr, &computeSetLayout),
                "vkCreateDescriptorSetLayout(compute)");
        std::array<VkDescriptorPoolSize, 4> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, PtImageCount},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        }};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        pool.pPoolSizes = poolSizes.data();
        vkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool),
                "vkCreateDescriptorPool(compute)");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &computeSetLayout;
        vkCheck(vkAllocateDescriptorSets(device, &allocate, &computeSet),
                "vkAllocateDescriptorSets(compute)");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &computeSetLayout;
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        vkCheck(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &computeLayout),
                "vkCreatePipelineLayout(compute)");

        VkDescriptorSetLayoutBinding blitBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo blitLayoutCreate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        blitLayoutCreate.bindingCount = 1;
        blitLayoutCreate.pBindings = &blitBinding;
        vkCheck(vkCreateDescriptorSetLayout(device, &blitLayoutCreate, nullptr, &blitSetLayout),
                "vkCreateDescriptorSetLayout(blit)");
        VkDescriptorPoolSize blitPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo blitPoolCreate{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        blitPoolCreate.maxSets = 1;
        blitPoolCreate.poolSizeCount = 1;
        blitPoolCreate.pPoolSizes = &blitPoolSize;
        vkCheck(vkCreateDescriptorPool(device, &blitPoolCreate, nullptr, &blitPool),
                "vkCreateDescriptorPool(blit)");
        allocate.descriptorPool = blitPool;
        allocate.pSetLayouts = &blitSetLayout;
        vkCheck(vkAllocateDescriptorSets(device, &allocate, &blitSet),
                "vkAllocateDescriptorSets(blit)");
        VkPipelineLayoutCreateInfo blitPipelineLayout{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        blitPipelineLayout.setLayoutCount = 1;
        blitPipelineLayout.pSetLayouts = &blitSetLayout;
        vkCheck(vkCreatePipelineLayout(device, &blitPipelineLayout, nullptr, &blitLayout),
                "vkCreatePipelineLayout(blit)");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = linearOutput ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.minFilter = linearOutput ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 1;
        vkCheck(vkCreateSampler(device, &sampler, nullptr, &blitSampler), "vkCreateSampler(blit)");
        createBuffer(paramsBuffer, sizeof(PtParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(verticesBuffer, MinimumBufferSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(indicesBuffer, MinimumBufferSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(instancesBuffer, MinimumBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(materialsBuffer, MinimumBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(lightsBuffer, MinimumBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(emittersBuffer, MinimumBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ensureImageTargets(swapchainExtent.width, swapchainExtent.height);
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool VulkanPt::initializePipelines(std::string &reason) {
    try {
        tracePipeline = makeComputePipeline("pt_trace.comp.spv", computeLayout);
        temporalPipeline = makeComputePipeline("pt_temporal.comp.spv", computeLayout);
        filterPipeline = makeComputePipeline("pt_filter.comp.spv", computeLayout);
        postPipeline = makeComputePipeline("pt_post.comp.spv", computeLayout);
        blitPipeline = makeBlitPipeline("pt_blit.vert.spv", "pt_blit.frag.spv");
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool VulkanPt::initializeImGui(std::string &reason) {
    // The UI pipeline is intentionally a small native pipeline.  The SDL platform backend still
    // owns input; the Vulkan backend consumes ImDrawData and uploads the finalized ImGui atlas.
    try {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        vkCheck(vkCreateDescriptorSetLayout(device, &layout, nullptr, &imguiSetLayout),
                "vkCreateDescriptorSetLayout(imgui)");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = 64;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        vkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &imguiPool),
                "vkCreateDescriptorPool(imgui)");
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &imguiSetLayout;
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16};
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        vkCheck(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &imguiLayout),
                "vkCreatePipelineLayout(imgui)");
        imguiPipeline = makeImGuiPipeline("pt_imgui.vert.spv", "pt_imgui.frag.spv");

        unsigned char *pixels = nullptr;
        int fontWidth = 0, fontHeight = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
        if (!pixels || fontWidth <= 0 || fontHeight <= 0)
            throw std::runtime_error("ImGui did not produce a font atlas");
        createImage(imgui.fontImage, static_cast<uint32_t>(fontWidth),
                    static_cast<uint32_t>(fontHeight), VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer staging;
        createBuffer(staging, VkDeviceSize(fontWidth) * fontHeight * 4,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        upload(staging, pixels, VkDeviceSize(fontWidth) * fontHeight * 4);
        VkCommandBuffer command = beginImmediate();
        VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.image = imgui.fontImage.image;
        toCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toCopy.subresourceRange.levelCount = 1;
        toCopy.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toCopy);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(fontWidth), static_cast<uint32_t>(fontHeight), 1};
        vkCmdCopyBufferToImage(command, staging.handle, imgui.fontImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        toCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toCopy.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toCopy);
        endImmediate(command);
        staging.reset();
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = imguiPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &imguiSetLayout;
        vkCheck(vkAllocateDescriptorSets(device, &allocate, &imgui.fontSet),
                "vkAllocateDescriptorSets(imgui font)");
        VkDescriptorImageInfo image{blitSampler, imgui.fontImage.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = imgui.fontSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        ImGui::GetIO().Fonts->SetTexID(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(imgui.fontSet)));
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool VulkanPt::init(SDL_Window *newWindow, uint32_t width, uint32_t height, std::string &reason) {
    if (initialized)
        return true;
    try {
        if (!initializeInstance(newWindow, reason)) {
            shutdown();
            return false;
        }
        if (!initializeDevice(reason)) {
            shutdown();
            return false;
        }
        if (!initializeSwapchain(width, height, reason) || !initializeResources(reason) ||
            !initializePipelines(reason) || !initializeImGui(reason)) {
            shutdown();
            return false;
        }
        ImGui::GetIO().BackendRendererName = VulkanUiBackendName;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        initialized = true;
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        shutdown();
        return false;
    }
}

void VulkanPt::waitIdle() {
    if (device)
        vkCheck(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
}

void VulkanPt::collectFrameTimestamps(Frame &frame) {
    if (!frame.timestampPending || !timestampsAvailable || !frame.timestampPool)
        return;
    uint64_t timestamps[TimestampQueryCount]{};
    const VkResult result = vkGetQueryPoolResults(
        device, frame.timestampPool, 0, TimestampQueryCount, sizeof(timestamps), timestamps,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result == VK_SUCCESS && frame.timestampSerial > completedTimestampSerial) {
        const uint64_t mask = timestampMask(timestampValidBits);
        traceMs = double(timestampDelta(timestamps[TimestampTraceBegin] & mask,
                                        timestamps[TimestampTraceEnd] & mask, mask)) *
                  timestampPeriodMs;
        denoiseMs = double(timestampDelta(timestamps[TimestampDenoiseBegin] & mask,
                                          timestamps[TimestampDenoiseEnd] & mask, mask)) *
                    timestampPeriodMs;
        completedTimestampSerial = frame.timestampSerial;
    } else if (frame.timestampSerial > completedTimestampSerial) {
        traceMs = -1.0;
        denoiseMs = -1.0;
        completedTimestampSerial = frame.timestampSerial;
    }
    frame.timestampPending = false;
}

void VulkanPt::destroyPipelines() {
    if (!device)
        return;
    if (tracePipeline)
        vkDestroyPipeline(device, tracePipeline, nullptr);
    if (temporalPipeline)
        vkDestroyPipeline(device, temporalPipeline, nullptr);
    if (filterPipeline)
        vkDestroyPipeline(device, filterPipeline, nullptr);
    if (postPipeline)
        vkDestroyPipeline(device, postPipeline, nullptr);
    if (blitPipeline)
        vkDestroyPipeline(device, blitPipeline, nullptr);
    if (imguiPipeline)
        vkDestroyPipeline(device, imguiPipeline, nullptr);
    tracePipeline = temporalPipeline = filterPipeline = postPipeline = blitPipeline =
        imguiPipeline = VK_NULL_HANDLE;
    if (computeLayout)
        vkDestroyPipelineLayout(device, computeLayout, nullptr);
    if (blitLayout)
        vkDestroyPipelineLayout(device, blitLayout, nullptr);
    if (imguiLayout)
        vkDestroyPipelineLayout(device, imguiLayout, nullptr);
    computeLayout = blitLayout = imguiLayout = VK_NULL_HANDLE;
}

void VulkanPt::destroyResources() {
    if (!device)
        return;
    for (auto &target : targets)
        target.reset();
    for (auto &[_, cache] : blases)
        cache.accel.reset();
    blases.clear();
    tlas.reset();
    scratchBuffer.reset();
    tlasInstancesBuffer.reset();
    emittersBuffer.reset();
    lightsBuffer.reset();
    materialsBuffer.reset();
    instancesBuffer.reset();
    indicesBuffer.reset();
    verticesBuffer.reset();
    paramsBuffer.reset();
    screenshotBuffer.reset();
    imgui.fontImage.reset();
    for (auto &buffer : imgui.vertices)
        buffer.reset();
    for (auto &buffer : imgui.indices)
        buffer.reset();
    if (blitSampler)
        vkDestroySampler(device, blitSampler, nullptr);
    blitSampler = VK_NULL_HANDLE;
    if (descriptorPool)
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (blitPool)
        vkDestroyDescriptorPool(device, blitPool, nullptr);
    if (imguiPool)
        vkDestroyDescriptorPool(device, imguiPool, nullptr);
    descriptorPool = blitPool = imguiPool = VK_NULL_HANDLE;
    if (computeSetLayout)
        vkDestroyDescriptorSetLayout(device, computeSetLayout, nullptr);
    if (blitSetLayout)
        vkDestroyDescriptorSetLayout(device, blitSetLayout, nullptr);
    if (imguiSetLayout)
        vkDestroyDescriptorSetLayout(device, imguiSetLayout, nullptr);
    computeSetLayout = blitSetLayout = imguiSetLayout = VK_NULL_HANDLE;
}

void VulkanPt::shutdown() {
    if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererName == VulkanUiBackendName) {
        ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::GetIO().BackendRendererName = nullptr;
    }
    if (!instance && !device)
        return;
    try {
        waitIdle();
    } catch (...) {
    }
    destroyPipelines();
    destroyResources();
    destroySwapchain();
    if (device) {
        for (auto &frame : frames) {
            if (frame.timestampPool)
                vkDestroyQueryPool(device, frame.timestampPool, nullptr);
            if (frame.fence)
                vkDestroyFence(device, frame.fence, nullptr);
            if (frame.imageAvailable)
                vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            if (frame.renderFinished)
                vkDestroySemaphore(device, frame.renderFinished, nullptr);
            if (frame.pool)
                vkDestroyCommandPool(device, frame.pool, nullptr);
            frame = {};
        }
        if (accelerationTimestampPool)
            vkDestroyQueryPool(device, accelerationTimestampPool, nullptr);
        accelerationTimestampPool = VK_NULL_HANDLE;
        vkDestroyDevice(device, nullptr);
    }
    device = VK_NULL_HANDLE;
    if (surface && instance)
        vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (instance)
        vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    initialized = false;
    activeFrame = false;
    timestampsAvailable = false;
    accelerationTimestampsAvailable = false;
    timestampPeriodMs = 0.0;
    timestampValidBits = 0;
    accelerationGpuMs = -1.0;
    accelerationTimingValid = false;
    nextTimestampSerial = 1;
    completedTimestampSerial = 0;
    traceMs = -1.0;
    denoiseMs = -1.0;
    accelerationMs = -1.0;
}

void VulkanPt::resize(uint32_t width, uint32_t height) {
    if (!initialized || width == 0 || height == 0)
        return;
    if (activeFrame)
        end();
    recreateSwapchain(width, height);
    historyReset = true;
}

void VulkanPt::uploadBuffers(const PtScene &scene) {
    const VkDeviceSize vertexBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.vertices.size() * sizeof(PtVertex));
    const VkDeviceSize indexBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.indices.size() * sizeof(uint32_t));
    const VkDeviceSize instanceBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.instances.size() * sizeof(PtInstance));
    const VkDeviceSize materialBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.materials.size() * sizeof(PtMaterial));
    const VkDeviceSize lightBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.lights.size() * sizeof(PtLight));
    const VkDeviceSize emitterBytes =
        std::max<VkDeviceSize>(MinimumBufferSize, scene.emitters.size() * sizeof(PtEmitter));
    const bool geometryDirty = scene.geometryRevision != uploadedGeometryRevision;
    const bool transformDirty = scene.transformRevision != uploadedTransformRevision;
    const bool lightingDirty = scene.lightingRevision != uploadedLightingRevision;
    const bool buffersNeedGrowth =
        verticesBuffer.size < vertexBytes || indicesBuffer.size < indexBytes ||
        instancesBuffer.size < instanceBytes || materialsBuffer.size < materialBytes ||
        lightsBuffer.size < lightBytes || emittersBuffer.size < emitterBytes;
    if ((geometryDirty || transformDirty || lightingDirty || buffersNeedGrowth) &&
        (uploadedGeometryRevision != std::numeric_limits<uint64_t>::max() ||
         uploadedTransformRevision != std::numeric_limits<uint64_t>::max() ||
         uploadedLightingRevision != std::numeric_limits<uint64_t>::max()))
        waitIdle();
    auto ensure = [&](Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
        if (buffer.size < size) {
            buffer.reset();
            createBuffer(buffer, std::max(size, buffer.size * 2), usage,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ++bufferGeneration;
        }
    };
    ensure(verticesBuffer, vertexBytes,
           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    ensure(indicesBuffer, indexBytes,
           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    ensure(instancesBuffer, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensure(materialsBuffer, materialBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensure(lightsBuffer, lightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensure(emittersBuffer, emitterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (geometryDirty || buffersNeedGrowth) {
        upload(verticesBuffer, scene.vertices.data(), vertexBytes);
        upload(indicesBuffer, scene.indices.data(), indexBytes);
    }
    if (transformDirty || buffersNeedGrowth)
        upload(instancesBuffer, scene.instances.data(), instanceBytes);
    if (lightingDirty || buffersNeedGrowth) {
        upload(materialsBuffer, scene.materials.data(), materialBytes);
        upload(lightsBuffer, scene.lights.data(), lightBytes);
        upload(emittersBuffer, scene.emitters.data(), emitterBytes);
    }
    uploadedGeometryRevision = scene.geometryRevision;
    uploadedTransformRevision = scene.transformRevision;
    uploadedLightingRevision = scene.lightingRevision;
    if (buffersNeedGrowth)
        descriptorDirty = true;
}

void VulkanPt::buildBlas(CachedBlas &cache, const PtGeometry &geometry) {
    VkAccelerationStructureGeometryKHR asGeometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    asGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    asGeometry.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    asGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    asGeometry.geometry.triangles.vertexData.deviceAddress =
        verticesBuffer.address + VkDeviceSize(geometry.vertexOffset) * sizeof(PtVertex);
    asGeometry.geometry.triangles.vertexStride = sizeof(PtVertex);
    asGeometry.geometry.triangles.maxVertex = geometry.vertexCount ? geometry.vertexCount - 1 : 0;
    asGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    asGeometry.geometry.triangles.indexData.deviceAddress =
        indicesBuffer.address + VkDeviceSize(geometry.indexOffset) * sizeof(uint32_t);
    asGeometry.geometry.triangles.transformData.deviceAddress = 0;
    const uint32_t primitiveCount = geometry.indexCount / 3;
    VkAccelerationStructureBuildGeometryInfoKHR query{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    query.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    query.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    query.geometryCount = 1;
    query.pGeometries = &asGeometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &query, &primitiveCount, &sizes);
    cache.accel.reset();
    cache.accel.device = device;
    createBuffer(cache.accel.storage, sizes.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR create{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = cache.accel.storage.handle;
    create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    vkCheck(vkCreateAccelerationStructureKHR(device, &create, nullptr, &cache.accel.handle),
            "vkCreateAccelerationStructureKHR(BLAS)");
    VkAccelerationStructureDeviceAddressInfoKHR address{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address.accelerationStructure = cache.accel.handle;
    cache.accel.address = vkGetAccelerationStructureDeviceAddressKHR(device, &address);
    cache.accel.primitiveCount = primitiveCount;
    cache.accel.key = geometry.key;
    VkDeviceSize scratchSize = std::max(sizes.buildScratchSize, sizes.updateScratchSize);
    if (scratchBuffer.size < scratchSize)
        createBuffer(scratchBuffer, scratchSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkBufferDeviceAddressInfo scratchAddress{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    scratchAddress.buffer = scratchBuffer.handle;
    query.dstAccelerationStructure = cache.accel.handle;
    query.scratchData.deviceAddress = vkGetBufferDeviceAddress(device, &scratchAddress);
    VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    VkCommandBuffer command = beginImmediate(true);
    vkCmdBuildAccelerationStructuresKHR(command, 1, &query, &ranges);
    endImmediate(command, true);
    cache.id = geometry.id;
    cache.key = geometry.key;
    cache.vertexOffset = geometry.vertexOffset;
    cache.vertexCount = geometry.vertexCount;
    cache.indexOffset = geometry.indexOffset;
    cache.indexCount = geometry.indexCount;
}

const CachedBlas *VulkanPt::blasForInstance(const PtScene &scene, size_t index) const {
    if (index >= scene.instances.size() || scene.geometry.empty())
        throw std::runtime_error("Vulkan path tracer received an instance without scene geometry");
    // offsets = { indexOffset, vertexOffset, stableId, geometryIndex }.  A
    // malformed geometry index would make the shader read a different mesh,
    // so reject it while publishing the TLAS instead of silently remapping it.
    const size_t geometryIndex = scene.instances[index].offsets[3];
    if (geometryIndex >= scene.geometry.size())
        throw std::runtime_error(
            "Vulkan path tracer instance references an invalid geometry index");
    const auto &geometry = scene.geometry[geometryIndex];
    auto it = blases.find(geometry.id);
    return it == blases.end() ? nullptr : &it->second;
}

void VulkanPt::updateTlas(const PtScene &scene, bool updateOnly) {
    if (scene.instances.empty() || scene.geometry.empty()) {
        tlas.reset();
        tlasInstancesBuffer.reset();
        descriptorDirty = true;
        return;
    }
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(scene.instances.size());
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const CachedBlas *blas = blasForInstance(scene, i);
        if (!blas || !blas->accel.handle)
            throw std::runtime_error("Vulkan path tracer instance references unpublished geometry");
        VkAccelerationStructureInstanceKHR instance{};
        const auto &model = scene.instances[i].model;
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] = model[col * 4 + row];
        instance.instanceCustomIndex = static_cast<uint32_t>(std::min<size_t>(i, 0xFFFFFFu));
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blas->accel.address;
        instances.push_back(instance);
    }
    if (instances.empty()) {
        tlas.reset();
        tlasInstancesBuffer.reset();
        descriptorDirty = true;
        return;
    }
    if (instances.size() > 0xFFFFFFu)
        throw std::runtime_error(
            "Vulkan path tracer scene has too many instances for TLAS custom indices");
    const VkDeviceSize bytes = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    const bool rebuild = !updateOnly || !tlas.handle || tlas.primitiveCount != instances.size() ||
                         tlasInstancesBuffer.size < bytes;
    Buffer replacementInstances;
    Accel replacementTlas;
    Buffer *instanceBuffer = &tlasInstancesBuffer;
    Accel *targetTlas = &tlas;
    if (rebuild) {
        replacementInstances.device = device;
        replacementInstances.physical = physical;
        try {
            createBuffer(replacementInstances, bytes,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        } catch (...) {
            replacementInstances.reset();
            throw;
        }
        replacementTlas.device = device;
        instanceBuffer = &replacementInstances;
        targetTlas = &replacementTlas;
    }
    try {
        upload(*instanceBuffer, instances.data(), bytes);
    } catch (...) {
        replacementTlas.reset();
        replacementInstances.reset();
        throw;
    }
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = instanceBuffer->handle;
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = vkGetBufferDeviceAddress(device, &address);
    VkAccelerationStructureBuildGeometryInfoKHR query{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    query.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    query.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    query.mode = rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                         : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    query.geometryCount = 1;
    query.pGeometries = &geometry;
    const uint32_t primitiveCount = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &query, &primitiveCount, &sizes);
    if (rebuild) {
        try {
            createBuffer(targetTlas->storage, sizes.accelerationStructureSize,
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkAccelerationStructureCreateInfoKHR create{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            create.buffer = targetTlas->storage.handle;
            create.size = sizes.accelerationStructureSize;
            create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            vkCheck(vkCreateAccelerationStructureKHR(device, &create, nullptr, &targetTlas->handle),
                    "vkCreateAccelerationStructureKHR(TLAS)");
        } catch (...) {
            replacementTlas.reset();
            replacementInstances.reset();
            throw;
        }
        VkAccelerationStructureDeviceAddressInfoKHR tlasAddress{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        tlasAddress.accelerationStructure = targetTlas->handle;
        targetTlas->address = vkGetAccelerationStructureDeviceAddressKHR(device, &tlasAddress);
        targetTlas->primitiveCount = primitiveCount;
    }
    targetTlas->key = scene.transformRevision;
    const VkDeviceSize scratchSize = std::max(sizes.buildScratchSize, sizes.updateScratchSize);
    try {
        if (scratchBuffer.size < scratchSize)
            createBuffer(scratchBuffer, scratchSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkBufferDeviceAddressInfo scratchAddress{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        scratchAddress.buffer = scratchBuffer.handle;
        query.srcAccelerationStructure = rebuild ? VK_NULL_HANDLE : tlas.handle;
        query.dstAccelerationStructure = targetTlas->handle;
        query.scratchData.deviceAddress = vkGetBufferDeviceAddress(device, &scratchAddress);
        VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
        VkCommandBuffer command = beginImmediate(true);
        vkCmdBuildAccelerationStructuresKHR(command, 1, &query, &ranges);
        endImmediate(command, true);
    } catch (...) {
        if (rebuild) {
            replacementTlas.reset();
            replacementInstances.reset();
        }
        throw;
    }
    if (rebuild) {
        tlas.reset();
        tlas = std::move(replacementTlas);
        tlasInstancesBuffer.reset();
        tlasInstancesBuffer = std::move(replacementInstances);
        descriptorDirty = true;
    }
}

void VulkanPt::updateAcceleration(const PtScene &scene) {
    accelerationGpuMs = 0.0;
    accelerationTimingValid = accelerationTimestampsAvailable;
    const bool buffersChanged = bufferGeneration != accelerationBufferGeneration;
    const bool geometryChanged = scene.geometryRevision != lastGeometryRevision || buffersChanged;
    const bool transformsChanged = scene.transformRevision != lastTransformRevision || !tlas.handle;
    if (geometryChanged || transformsChanged)
        waitIdle();
    if (geometryChanged) {
        descriptorDirty = true;
        std::set<uint64_t> active;
        for (const auto &geometry : scene.geometry) {
            if (geometry.vertexCount < 3 || geometry.indexCount < 3)
                continue;
            const size_t vertexOffset = geometry.vertexOffset;
            const size_t vertexCount = geometry.vertexCount;
            const size_t indexOffset = geometry.indexOffset;
            const size_t indexCount = geometry.indexCount;
            if (vertexOffset > scene.vertices.size() ||
                vertexCount > scene.vertices.size() - vertexOffset ||
                indexOffset > scene.indices.size() ||
                indexCount > scene.indices.size() - indexOffset || (indexCount % 3u) != 0u)
                throw std::runtime_error("Vulkan path tracer received out-of-range chunk geometry");
            for (size_t i = 0; i < indexCount; ++i)
                if (scene.indices[indexOffset + i] >= vertexCount)
                    throw std::runtime_error(
                        "Vulkan path tracer received an out-of-range chunk index");
            for (size_t i = 0; i < vertexCount; ++i) {
                const float materialValue = scene.vertices[vertexOffset + i].color[3];
                if (!std::isfinite(materialValue) || materialValue < 0.f ||
                    static_cast<double>(materialValue) >
                        static_cast<double>(std::numeric_limits<uint32_t>::max()))
                    throw std::runtime_error("Vulkan path tracer received an invalid material id");
                const auto materialId = static_cast<size_t>(materialValue + 0.5f);
                if (materialId >= scene.materials.size())
                    throw std::runtime_error(
                        "Vulkan path tracer geometry references an unknown material");
            }
            if (!active.insert(geometry.id).second)
                throw std::runtime_error(
                    "Vulkan path tracer received duplicate chunk geometry ids");
            auto it = blases.find(geometry.id);
            if (it == blases.end()) {
                CachedBlas cache;
                buildBlas(cache, geometry);
                blases.emplace(geometry.id, std::move(cache));
            } else if (it->second.key != geometry.key ||
                       it->second.vertexOffset != geometry.vertexOffset ||
                       it->second.vertexCount != geometry.vertexCount ||
                       it->second.indexOffset != geometry.indexOffset ||
                       it->second.indexCount != geometry.indexCount || buffersChanged) {
                // Build the replacement beside the published BLAS.  The old
                // structure remains valid until the new build has completed,
                // so a failed or device-lost replacement cannot leave the
                // scene with a half-published chunk.
                CachedBlas replacement;
                buildBlas(replacement, geometry);
                it->second.accel.reset();
                it->second = std::move(replacement);
            }
        }
        for (auto it = blases.begin(); it != blases.end();) {
            if (!active.contains(it->first)) {
                it->second.accel.reset();
                it = blases.erase(it);
            } else
                ++it;
        }
    }
    if (geometryChanged || transformsChanged)
        updateTlas(scene, transformsChanged && !geometryChanged);
    lastGeometryRevision = scene.geometryRevision;
    lastTransformRevision = scene.transformRevision;
    accelerationBufferGeneration = bufferGeneration;
    accelerationMs = accelerationTimingValid ? accelerationGpuMs : -1.0;
}

void VulkanPt::updateDescriptorSet() {
    VkDescriptorBufferInfo params{paramsBuffer.handle, 0, sizeof(PtParams)};
    VkDescriptorBufferInfo vertices{verticesBuffer.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo indices{indicesBuffer.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo instances{instancesBuffer.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo materials{materialsBuffer.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lights{lightsBuffer.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo emitters{emittersBuffer.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[8]{};
    VkDescriptorBufferInfo *infos[7]{&params,    &vertices, &indices, &instances,
                                     &materials, &lights,   &emitters};
    for (uint32_t i = 0; i < 7; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = computeSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = infos[i];
    }
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration.accelerationStructureCount = 1;
    acceleration.pAccelerationStructures = &tlas.handle;
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].pNext = &acceleration;
    writes[7].dstSet = computeSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    std::array<VkDescriptorImageInfo, PtImageCount> images{};
    std::array<VkWriteDescriptorSet, PtImageCount> imageWrites{};
    for (uint32_t i = 0; i < PtImageCount; ++i) {
        images[i].imageView = targets[i].view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        imageWrites[i].dstSet = computeSet;
        imageWrites[i].dstBinding = 8 + i;
        imageWrites[i].descriptorCount = 1;
        imageWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        imageWrites[i].pImageInfo = &images[i];
    }
    std::array<VkWriteDescriptorSet, DescriptorCount> allWrites{};
    std::copy(std::begin(writes), std::end(writes), allWrites.begin());
    std::copy(imageWrites.begin(), imageWrites.end(), allWrites.begin() + 8);
    vkUpdateDescriptorSets(device, DescriptorCount, allWrites.data(), 0, nullptr);
    descriptorDirty = false;
}

void VulkanPt::updateScene(const PtScene &scene) {
    uploadBuffers(scene);
    updateAcceleration(scene);
    // Vulkan does not permit a null acceleration-structure descriptor unless
    // the optional nullDescriptor feature is enabled.  Empty scenes skip the
    // compute dispatch and therefore keep the previous descriptor untouched.
    if (tlas.handle && descriptorDirty)
        updateDescriptorSet();
}

void VulkanPt::recordCompute(VkCommandBuffer command, const PtParams &params) {
    if (!tlas.handle) {
        if (timestampsAvailable && frames[frameIndex].timestampPool) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                frames[frameIndex].timestampPool, TimestampTraceEnd);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                frames[frameIndex].timestampPool, TimestampDenoiseBegin);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                frames[frameIndex].timestampPool, TimestampDenoiseEnd);
        }
        VkImageMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        outputBarrier.srcAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        outputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        outputBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        outputBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        outputBarrier.image = targets[PtOutput].image;
        outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        outputBarrier.subresourceRange.levelCount = 1;
        outputBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &outputBarrier);
        return;
    }
    const uint32_t groupsX = (swapchainExtent.width + ComputeGroupSize - 1) / ComputeGroupSize;
    const uint32_t groupsY = (swapchainExtent.height + ComputeGroupSize - 1) / ComputeGroupSize;
    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.layerCount = 1;
    for (auto &target : targets) {
        imageBarrier.image = target.image;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &imageBarrier);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1,
                            &computeSet, 0, nullptr);
    vkCmdPushConstants(command, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t),
                       &params.sizeFrame[2]);
    vkCmdDispatch(command, groupsX, groupsY, 1);
    VkMemoryBarrier computeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    computeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeBarrier, 0, nullptr, 0,
                         nullptr);
    if (timestampsAvailable && frames[frameIndex].timestampPool) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            frames[frameIndex].timestampPool, TimestampTraceEnd);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            frames[frameIndex].timestampPool, TimestampDenoiseBegin);
    }
    // Temporal resolve always runs so history and moments remain coherent when
    // denoising is toggled off.  The filter pass below becomes a one-pass
    // passthrough in that mode.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporalPipeline);
    vkCmdDispatch(command, groupsX, groupsY, 1);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeBarrier, 0, nullptr, 0,
                         nullptr);
    // Temporal resolve publishes the five current history planes.  Keep a
    // previous-frame snapshot in separate images so the next dispatch can
    // read history while this frame's filters write the current planes.
    if (PtImageCount >= 20) {
        const std::array<std::pair<uint32_t, uint32_t>, 5> historyCopies{{
            {PtHistoryGuide, 16},
            {PtHistoryMotion, 17},
            {PtHistoryDiffuse, 18},
            {PtHistorySpecular, 19},
            {PtMoments, PtPreviousMoments},
        }};
        for (const auto &[sourceIndex, destinationIndex] : historyCopies) {
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransfer.image = targets[sourceIndex].image;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.levelCount = 1;
            toTransfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &toTransfer);
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.image = targets[destinationIndex].image;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &toTransfer);
            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1;
            copy.extent = {swapchainExtent.width, swapchainExtent.height, 1};
            vkCmdCopyImage(command, targets[sourceIndex].image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[destinationIndex].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            std::array<VkImageMemoryBarrier, 2> after{};
            after[0].sType = after[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            after[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            after[0].image = targets[sourceIndex].image;
            after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            after[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            after[1].image = targets[destinationIndex].image;
            for (auto &barrier : after) {
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
            }
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 static_cast<uint32_t>(after.size()), after.data());
        }
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, filterPipeline);
    // The fifth pass publishes scratchA, which is the post-process input.  In
    // no-denoise mode each pass is a cheap passthrough, but all five dispatches
    // are still required to reach that publication point.
    constexpr uint32_t filterPasses = 5;
    for (uint32_t pass = 0; pass < filterPasses; ++pass) {
        vkCmdPushConstants(command, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t),
                           &pass);
        vkCmdDispatch(command, groupsX, groupsY, 1);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeBarrier, 0,
                             nullptr, 0, nullptr);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, postPipeline);
    vkCmdDispatch(command, groupsX, groupsY, 1);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeBarrier, 0, nullptr, 0,
                         nullptr);
    if (timestampsAvailable && frames[frameIndex].timestampPool)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            frames[frameIndex].timestampPool, TimestampDenoiseEnd);
    VkImageMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    outputBarrier.image = targets[PtOutput].image;
    outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.levelCount = 1;
    outputBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &outputBarrier);
}

void VulkanPt::recordBlit(VkCommandBuffer command) {
    VkClearValue clear{};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass;
    begin.framebuffer = framebuffers[acquiredImage];
    begin.renderArea.extent = swapchainExtent;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout, 0, 1, &blitSet, 0,
                            nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
}

void VulkanPt::beginFrame() {
    Frame &frame = frames[frameIndex];
    vkCheck(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(frame)");
    collectFrameTimestamps(frame);
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.imageAvailable,
                                            VK_NULL_HANDLE, &acquiredImage);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainOutOfDate = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        swapchainOutOfDate = true;
    else
        vkCheck(result, "vkAcquireNextImageKHR");
    vkCheck(vkResetFences(device, 1, &frame.fence), "vkResetFences(frame)");
    vkCheck(vkResetCommandBuffer(frame.command, 0), "vkResetCommandBuffer(frame)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(frame.command, &begin), "vkBeginCommandBuffer(frame)");
    if (timestampsAvailable && frame.timestampPool)
        vkCmdResetQueryPool(frame.command, frame.timestampPool, 0, TimestampQueryCount);
    if (historyReset)
        clearTargets(frame.command);
    if (timestampsAvailable && frame.timestampPool)
        vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            frame.timestampPool, TimestampTraceBegin);
    activeFrame = true;
}

bool VulkanPt::render(const PtScene &scene, const PtParams &params) {
    if (!initialized || swapchainExtent.width == 0 || swapchainExtent.height == 0)
        return false;
    if (activeFrame)
        end();
    if (swapchainOutOfDate) {
        int drawableWidth = 0, drawableHeight = 0;
        if (window)
            SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
            return false;
        recreateSwapchain(static_cast<uint32_t>(drawableWidth),
                          static_cast<uint32_t>(drawableHeight));
        swapchainOutOfDate = false;
    }
    frameStart = std::chrono::steady_clock::now();
    lastParams = params;
    lastSamples = params.counts[0];
    lastBounces = params.counts[1];
    const bool denoise = params.options[0] != 0;
    // A no-denoise reference capture still needs temporal sample accumulation;
    // invalidate once when the setting changes instead of clearing history on
    // every frame while denoising is disabled.
    if (params.sizeFrame[3] != 0 || (haveDenoiseSetting && denoise != previousDenoise))
        historyReset = true;
    previousDenoise = denoise;
    haveDenoiseSetting = true;
    completeScreenshot();
    updateScene(scene);
    beginFrame();
    if (!activeFrame)
        return false;
    // paramsBuffer is shared by the in-flight frame slots.  Upload only after
    // beginFrame has waited for this slot's fence, so a two-frame swapchain
    // cannot overwrite uniforms still consumed by the previous submission.
    upload(paramsBuffer, &params, sizeof(params));
    recordCompute(frames[frameIndex].command, params);
    recordBlit(frames[frameIndex].command);
    return true;
}

void VulkanPt::drawImGui(ImDrawData *draw) {
    if (!activeFrame || !draw || !draw->Valid || !imguiPipeline)
        return;
    const int framebufferWidth = static_cast<int>(draw->DisplaySize.x * draw->FramebufferScale.x);
    const int framebufferHeight = static_cast<int>(draw->DisplaySize.y * draw->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0)
        return;
    const float left = draw->DisplayPos.x;
    const float right = draw->DisplayPos.x + draw->DisplaySize.x;
    const float top = draw->DisplayPos.y;
    const float bottom = draw->DisplayPos.y + draw->DisplaySize.y;
    const float ortho[16]{
        2.f / (right - left),
        0,
        0,
        0,
        0,
        2.f / (top - bottom),
        0,
        0,
        0,
        0,
        -1.f,
        0,
        -(right + left) / (right - left),
        -(top + bottom) / (top - bottom),
        0,
        1.f,
    };
    VkCommandBuffer command = frames[frameIndex].command;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, imguiPipeline);
    vkCmdPushConstants(command, imguiLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ortho), ortho);
    VkViewport viewport{
        0, 0, static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight), 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    const VkDescriptorSet fontSet = imgui.fontSet;
    const VkDeviceSize vertexOffset = 0;
    size_t totalVertices = 0, totalIndices = 0;
    for (int listIndex = 0; listIndex < draw->CmdListsCount; ++listIndex) {
        const ImDrawList *list = draw->CmdLists[listIndex];
        if (list) {
            totalVertices += static_cast<size_t>(list->VtxBuffer.Size);
            totalIndices += static_cast<size_t>(list->IdxBuffer.Size);
        }
    }
    if (!totalVertices || !totalIndices)
        return;
    std::vector<ImDrawVert> vertices(totalVertices);
    std::vector<ImDrawIdx> indices(totalIndices);
    size_t vertexCursor = 0, indexCursor = 0;
    std::vector<std::pair<size_t, size_t>> listOffsets;
    listOffsets.reserve(static_cast<size_t>(draw->CmdListsCount));
    for (int listIndex = 0; listIndex < draw->CmdListsCount; ++listIndex) {
        const ImDrawList *list = draw->CmdLists[listIndex];
        listOffsets.emplace_back(vertexCursor, indexCursor);
        if (!list)
            continue;
        std::copy(list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size,
                  vertices.begin() + static_cast<ptrdiff_t>(vertexCursor));
        std::copy(list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size,
                  indices.begin() + static_cast<ptrdiff_t>(indexCursor));
        vertexCursor += static_cast<size_t>(list->VtxBuffer.Size);
        indexCursor += static_cast<size_t>(list->IdxBuffer.Size);
    }
    const VkDeviceSize vertexBytes = VkDeviceSize(vertices.size()) * sizeof(ImDrawVert);
    const VkDeviceSize indexBytes = VkDeviceSize(indices.size()) * sizeof(ImDrawIdx);
    // Each frame slot owns its UI buffers. beginFrame has already waited for
    // this slot's fence, so updating the current slot cannot race the previous
    // slot's draw while the GPU is still presenting it.
    Buffer &vertexBuffer = imgui.vertices[frameIndex];
    Buffer &indexBuffer = imgui.indices[frameIndex];
    if (vertexBuffer.size < vertexBytes) {
        vertexBuffer.reset();
        createBuffer(vertexBuffer, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (indexBuffer.size < indexBytes) {
        indexBuffer.reset();
        createBuffer(indexBuffer, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    upload(vertexBuffer, vertices.data(), vertexBytes);
    upload(indexBuffer, indices.data(), indexBytes);
    vkCmdBindVertexBuffers(command, 0, 1, &vertexBuffer.handle, &vertexOffset);
    vkCmdBindIndexBuffer(command, indexBuffer.handle, 0,
                         sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    for (int listIndex = 0; listIndex < draw->CmdListsCount; ++listIndex) {
        const ImDrawList *list = draw->CmdLists[listIndex];
        if (!list || list->VtxBuffer.empty() || list->IdxBuffer.empty())
            continue;
        const auto [baseVertex, baseIndex] = listOffsets[static_cast<size_t>(listIndex)];
        uint32_t indexOffset = static_cast<uint32_t>(baseIndex);
        for (const ImDrawCmd &item : list->CmdBuffer) {
            if (item.UserCallback) {
                item.UserCallback(list, &item);
                indexOffset += item.ElemCount;
                continue;
            }
            ImVec4 clip = item.ClipRect;
            clip.x = (clip.x - draw->DisplayPos.x) * draw->FramebufferScale.x;
            clip.y = (clip.y - draw->DisplayPos.y) * draw->FramebufferScale.y;
            clip.z = (clip.z - draw->DisplayPos.x) * draw->FramebufferScale.x;
            clip.w = (clip.w - draw->DisplayPos.y) * draw->FramebufferScale.y;
            if (clip.x >= framebufferWidth || clip.y >= framebufferHeight || clip.z <= 0 ||
                clip.w <= 0) {
                indexOffset += item.ElemCount;
                continue;
            }
            const int x = std::max(0, static_cast<int>(std::floor(clip.x)));
            const int y = std::max(0, static_cast<int>(std::floor(clip.y)));
            const int z = std::min(framebufferWidth, static_cast<int>(std::ceil(clip.z)));
            const int w = std::min(framebufferHeight, static_cast<int>(std::ceil(clip.w)));
            if (z <= x || w <= y) {
                indexOffset += item.ElemCount;
                continue;
            }
            VkRect2D scissor{{x, y}, {static_cast<uint32_t>(z - x), static_cast<uint32_t>(w - y)}};
            vkCmdSetScissor(command, 0, 1, &scissor);
            VkDescriptorSet texture = fontSet;
            if (item.GetTexID() != 0 && item.GetTexID() == ImGui::GetIO().Fonts->TexID)
                texture =
                    reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(item.GetTexID()));
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, imguiLayout, 0, 1,
                                    &texture, 0, nullptr);
            vkCmdDrawIndexed(command, item.ElemCount, 1, indexOffset,
                             static_cast<int32_t>(baseVertex) + item.VtxOffset, 0);
            indexOffset += item.ElemCount;
        }
    }
}

void VulkanPt::recordScreenshot(VkCommandBuffer command) {
    if (pendingScreenshot.empty())
        return;
    if (screenshotBuffer.size < VkDeviceSize(swapchainExtent.width) * swapchainExtent.height * 4)
        createBuffer(screenshotBuffer,
                     VkDeviceSize(swapchainExtent.width) * swapchainExtent.height * 4,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    // Copy the presented swapchain image after the render pass so ImGui and
    // diagnostic overlays are included in screenshots. The swapchain usage
    // flag is required by initializeSwapchain above.
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.image = swapchainImages[acquiredImage];
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {swapchainExtent.width, swapchainExtent.height, 1};
    vkCmdCopyImageToBuffer(command, swapchainImages[acquiredImage],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer.handle, 1, &copy);
    toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.dstAccessMask = 0;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);
    screenshotInFlight = std::move(pendingScreenshot);
    screenshotFrame = frameIndex;
    screenshotWidth = swapchainExtent.width;
    screenshotHeight = swapchainExtent.height;
}

void VulkanPt::completeScreenshot() {
    if (screenshotInFlight.empty())
        return;
    if (!screenshotBuffer.mapped)
        throw std::runtime_error("Vulkan screenshot staging buffer is not mapped");
    vkCheck(vkWaitForFences(device, 1, &frames[screenshotFrame].fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences(screenshot)");
    const auto path = screenshotInFlight;
    std::ofstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not open Vulkan screenshot path: " + path.string());
    file << "P6\n" << screenshotWidth << ' ' << screenshotHeight << "\n255\n";
    const auto *pixels = static_cast<const uint8_t *>(screenshotBuffer.mapped);
    const bool bgra =
        swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM || swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t y = 0; y < screenshotHeight; ++y)
        for (uint32_t x = 0; x < screenshotWidth; ++x) {
            const uint8_t *pixel = pixels + (size_t(y) * screenshotWidth + x) * 4;
            const uint8_t red = pixel[bgra ? 2 : 0];
            const uint8_t green = pixel[1];
            const uint8_t blue = pixel[bgra ? 0 : 2];
            file.put(static_cast<char>(red));
            file.put(static_cast<char>(green));
            file.put(static_cast<char>(blue));
        }
    if (!file)
        throw std::runtime_error("Could not write Vulkan screenshot: " + path.string());
    screenshotInFlight.clear();
}

void VulkanPt::end() {
    if (!activeFrame)
        return;
    Frame &frame = frames[frameIndex];
    vkCmdEndRenderPass(frame.command);
    // recordScreenshot below copies the completed swapchain image, after the
    // render pass has drawn both the scene blit and native ImGui overlays.
    if (!pendingScreenshot.empty())
        recordScreenshot(frame.command);
    VkImageMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.image = targets[PtOutput].image;
    outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.levelCount = 1;
    outputBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &outputBarrier);
    vkCheck(vkEndCommandBuffer(frame.command), "vkEndCommandBuffer(frame)");
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.renderFinished;
    VkResult result = vkQueueSubmit(queue, 1, &submit, frame.fence);
    checkDeviceLost(result, "vkQueueSubmit(frame)");
    if (timestampsAvailable && frame.timestampPool) {
        frame.timestampSerial = nextTimestampSerial++;
        frame.timestampPending = true;
    }
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &acquiredImage;
    result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        swapchainOutOfDate = true;
    else
        checkDeviceLost(result, "vkQueuePresentKHR");
    // A reference capture may stop the application immediately after this
    // frame.  Complete the staged transfer here so the requested PPM is
    // durable even when there is no following render tick to poll it.
    if (!screenshotInFlight.empty())
        completeScreenshot();
    activeFrame = false;
    historyReset = false;
    accumulatedSamples =
        lastParams.options[1] != 0 ? accumulatedSamples + lastSamples : lastSamples;
    ++frameIndex;
    frameIndex %= FramesInFlight;
}

void VulkanPt::checkDeviceLost(VkResult result, const char *what) {
    if (result == VK_ERROR_DEVICE_LOST)
        throw std::runtime_error(std::string(what) + ": Vulkan device lost");
    if (result != VK_SUCCESS)
        failVk(what, result);
}

void VulkanPt::screenshot(const std::filesystem::path &path) {
    pendingScreenshot = path;
}

void VulkanPt::setVsync(bool enabled) {
    if (vsync == enabled || !initialized)
        return;
    if (activeFrame)
        end();
    vsync = enabled;
    recreateSwapchain(swapchainExtent.width, swapchainExtent.height);
}

RenderTimings VulkanPt::timings() const {
    RenderTimings result = stats;
    uint64_t liveBytes = paramsBuffer.size + verticesBuffer.size + indicesBuffer.size +
                         instancesBuffer.size + materialsBuffer.size + lightsBuffer.size +
                         emittersBuffer.size + tlasInstancesBuffer.size + scratchBuffer.size +
                         tlas.storage.size + screenshotBuffer.size;
    for (const auto &buffer : imgui.vertices)
        liveBytes += buffer.size;
    for (const auto &buffer : imgui.indices)
        liveBytes += buffer.size;
    for (const auto &target : targets)
        liveBytes += uint64_t(target.width) * target.height * 16;
    for (const auto &[_, cache] : blases)
        liveBytes += cache.accel.storage.size;
    if (imgui.fontImage.image)
        liveBytes += uint64_t(imgui.fontImage.width) * imgui.fontImage.height * 4;
    result.cpuMs = frameStart.time_since_epoch().count() == 0
                       ? 0
                       : std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - frameStart)
                             .count();
    result.gpuMs = traceMs >= 0.0 && denoiseMs >= 0.0 ? traceMs + denoiseMs : -1.0;
    result.giMs = 0;
    result.traceMs = traceMs;
    result.denoiseMs = denoiseMs;
    result.accelerationMs = accelerationMs;
    result.ptBytes = liveBytes;
    result.ptSamples = lastSamples;
    result.ptBounces = lastBounces;
    result.accumulatedSamples = accumulatedSamples;
    result.backend = "path-traced (Vulkan)";
    result.fallbackReason.clear();
    result.geometryBytes = static_cast<uint64_t>(verticesBuffer.size + indicesBuffer.size);
    result.gpuFrame = static_cast<uint32_t>(completedTimestampSerial);
    result.textureBytes =
        uint64_t(swapchainExtent.width) * swapchainExtent.height * 16 * targets.size();
    result.passes = {{201, "Path trace", 0, traceMs, result.gpuFrame},
                     {202, "PT denoise and post", 0, denoiseMs, result.gpuFrame},
                     {203, "Acceleration structures", 0, accelerationMs, result.gpuFrame}};
    return result;
}

} // namespace

std::unique_ptr<NativePt> makeVulkanPt() {
    return std::make_unique<VulkanPt>();
}

} // namespace aspera

#endif
