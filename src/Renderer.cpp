#include "Renderer.h"
#include "Platform.h"
#include <vulkan/vulkan.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <map>
#include <unordered_map>
#include <functional>
#include <utility>
#include <tuple>
#include <set>
#include <deque>
#include <future>
#include <fstream>
#include <iostream>
#include <numeric>
#include <cstring>
#include <atomic>
namespace vke {
static void check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan error " + std::to_string(r));
}
static constexpr uint64_t UploadBytes = 16ull * 1024 * 1024;
static constexpr uint32_t MaxInstances = 100000;
struct Buffer {
    VmaAllocator allocator{};
    VkBuffer handle{};
    VmaAllocation allocation{};
    void *mapped{};
    VkDeviceSize size{};
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&b) noexcept {
        *this = std::move(b);
    }
    Buffer &operator=(Buffer &&b) noexcept {
        if (this != &b) {
            destroy();
            allocator = b.allocator;
            handle = b.handle;
            allocation = b.allocation;
            mapped = b.mapped;
            size = b.size;
            b.handle = {};
        }
        return *this;
    }
    ~Buffer() {
        destroy();
    }
    void destroy() {
        if (handle)
            vmaDestroyBuffer(allocator, handle, allocation);
        handle = {};
    }
    void flush() {
        vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    }
};
struct Image {
    VmaAllocator allocator{};
    VkDevice device{};
    VkImage handle{};
    VmaAllocation allocation{};
    VkImageView view{};
    std::vector<VkImageView> mipViews;
    VkFormat format{};
    uint32_t width{}, height{}, mips = 1, layers = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    Image() = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    ~Image() {
        for (auto v : mipViews)
            vkDestroyImageView(device, v, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (handle)
            vmaDestroyImage(allocator, handle, allocation);
    }
};
struct alignas(16) GlobalData {
    glm::mat4 vp{1}, inverseVP{1}, lightVP{1};
    glm::vec4 camera{}, light{}, lightColor{}, environment{};
};
struct alignas(16) InstanceData {
    glm::mat4 model{1};
    glm::uvec4 meta{};
};
struct alignas(16) TextureData {
    glm::vec4 offsetScale{0, 0, 1, 1}, rotationUV{0, 1, 0, 0};
};
struct alignas(16) MaterialData {
    glm::vec4 baseColor{1}, emissive{}, factors{}, specular{}, alpha{};
    TextureData info[8];
};
struct GpuMesh {
    Buffer vertices, indices;
    uint32_t count{}, material{};
};
struct GpuMaterial {
    Buffer uniform;
    VkDescriptorSet set{};
};
struct UploadPart {
    const unsigned char *data{};
    size_t bytes{}, offset{};
    VkBuffer buffer{};
    std::shared_ptr<Image> image;
};
struct GpuAsset {
    VkDevice device{};
    VkDescriptorPool descriptorPool{};
    std::shared_ptr<ImportedAsset> cpu;
    std::vector<GpuMesh> meshes;
    std::vector<GpuMaterial> materials;
    std::vector<std::shared_ptr<Image>> textures;
    std::deque<UploadPart> uploads;
    uint64_t readySerial = UINT64_MAX;
    ~GpuAsset() {
        for (auto &material : materials)
            if (material.set)
                vkFreeDescriptorSets(device, descriptorPool, 1, &material.set);
    }
};
struct Draw {
    GpuAsset *asset{};
    uint32_t mesh{}, first{}, count = 1;
    bool mirrored = false;
    float distance{};
};
struct EnvironmentData {
    int width = 0, height = 0;
    std::vector<float> rgba;
};
struct Renderer::Impl {
    GLFWwindow *window{};
    bool validation{}, vsync{}, rebuild = false;
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkDevice device{};
    uint32_t family{};
    VkQueue queue{};
    VmaAllocator allocator{};
    VkSwapchainKHR swapchain{};
    VkFormat swapFormat{};
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkSemaphore> presentSemaphores;
    VkDescriptorPool descriptorPool{};
    VkDescriptorSetLayout globalLayout{}, materialLayout{}, toneSetLayout{}, computeSetLayout{};
    VkPipelineLayout meshLayout{}, toneLayout{}, computeLayout{};
    VkPipeline opaque[3]{}, transparent[3]{}, shadow[3]{}, picking[3]{}, skyPipeline{}, tonePipeline{},
        computePipeline{};
    VkPipelineCache pipelineCache{};
    std::map<std::array<int, 4>, VkSampler> samplers;
    VkSampler linearSampler{}, shadowSampler{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
    struct Frame {
        VkCommandPool pool{};
        VkCommandBuffer cmd{};
        VkFence fence{};
        VkSemaphore acquired{};
        VkDescriptorSet global{}, tone{};
        Buffer uniform, instances, staging, pickReadback, screenshotBuffer;
        VkQueryPool queries{};
        uint64_t serial{};
        bool hasQueries = false, hasPick = false;
        fs::path screenshotPath;
        uint32_t screenshotWidth{}, screenshotHeight{};
    };
    std::array<Frame, 2> frames;
    uint64_t submitted = 0, completed = 0;
    uint32_t frameIndex = 0;
    std::shared_ptr<Image> hdr, msaa, depth, shadowDepth, pickImage, pickDepth, white, flatNormal, envSource,
        envSpecular, envDiffuse, envBrdf;
    uint32_t renderWidth = 0, renderHeight = 0;
    std::unordered_map<AssetHandle, std::unique_ptr<GpuAsset>> assets;
    AssetHandle nextAsset = 1, ground = 0;
    struct Retired {
        uint64_t serial;
        std::unique_ptr<GpuAsset> asset;
    };
    std::deque<Retired> retired;
    std::optional<glm::ivec2> pickRequest;
    std::optional<uint32_t> pickResult;
    fs::path captureRequest;
    std::future<EnvironmentData> environmentJob;
    fs::path activeEnvironmentPath, requestedEnvironmentPath;
    std::optional<fs::path> nextEnvironmentPath;
    std::vector<VkDescriptorSet> computeSets;
    std::vector<std::string> messages;
    std::atomic_uint errors{0};
    RenderStats stats;
    bool imguiInitialized = false;
    std::vector<Draw> draws, blends, shadowDraws;
    std::vector<InstanceData> instanceData;
    Impl(GLFWwindow *w, bool validate, bool sync) : window(w), validation(validate), vsync(sync) {
        init();
    }
    ~Impl();
    void init();
    void createSwapchain();
    void destroySwapchain();
    void createLayouts();
    void createPipelines();
    void createFrames();
    void ensureTargets(uint32_t w, uint32_t h);
    void updateGlobals();
    void createEnvironment(const EnvironmentData &data);
    void dispatchEnvironment(VkCommandBuffer cmd);
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false, bool readback = false);
    std::shared_ptr<Image> image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                 uint32_t mips = 1, uint32_t layers = 1,
                                 VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, bool cube = false);
    VkSampler sampler(const TextureRef &ref);
    VkDescriptorSet descriptor(VkDescriptorSetLayout layout);
    VkShaderModule shader(const char *name);
    VkPipeline pipeline(const char *vert, const char *frag, VkPipelineLayout layout, VkFormat color,
                        VkFormat depth, VkSampleCountFlagBits sample, VkCullModeFlags cull, VkFrontFace front,
                        bool blend, bool mesh, bool depthWrite = true);
    void transition(VkCommandBuffer cmd, Image &image, VkImageLayout layout,
                    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VkAccessFlags2 access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    void immediate(const std::function<void(VkCommandBuffer)> &fn);
    void mipmaps(VkCommandBuffer cmd, Image &image);
    void processUploads(VkCommandBuffer cmd, Frame &frame);
    void destroyAsset(GpuAsset &asset);
    void collect();
    AssetHandle enqueue(std::shared_ptr<ImportedAsset> a);
    void makeDraws(const Scene &scene);
    void drawList(VkCommandBuffer cmd, const std::vector<Draw> &list, int mode);
    void viewport(VkCommandBuffer cmd, float w, float h, float x = 0, float y = 0);
    void renderScene(VkCommandBuffer cmd, const Scene &scene, uint32_t selected);
    void pick(VkCommandBuffer cmd, Frame &frame, uint32_t selected);
    void waitFrame();
    void render(const Scene &scene, Viewport viewport, uint32_t selected);
};
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                    void *user) {
    auto *errors = static_cast<std::atomic_uint *>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++*errors;
    std::cerr << "[Vulkan] " << data->pMessage << "\n";
    return VK_FALSE;
}
Buffer Renderer::Impl::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool readback) {
    Buffer b;
    b.allocator = allocator;
    b.size = std::max(size, VkDeviceSize(16));
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b.size;
    ci.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host)
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                             : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    else
        ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(allocator, &ci, &ai, &b.handle, &b.allocation, &info));
    b.mapped = info.pMappedData;
    return b;
}
std::shared_ptr<Image> Renderer::Impl::image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                             uint32_t mips, uint32_t layers, VkSampleCountFlagBits sample,
                                             bool cube) {
    auto im = std::make_shared<Image>();
    im->allocator = allocator;
    im->device = device;
    im->width = w;
    im->height = h;
    im->format = format;
    im->mips = mips;
    im->layers = layers;
    im->aspect = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = sample;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(allocator, &ci, &ai, &im->handle, &im->allocation, nullptr));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->handle;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {im->aspect, 0, mips, 0, layers};
    check(vkCreateImageView(device, &vi, nullptr, &im->view));
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.subresourceRange.levelCount = 1;
        for (uint32_t i = 0; i < mips; i++) {
            vi.subresourceRange.baseMipLevel = i;
            VkImageView v;
            check(vkCreateImageView(device, &vi, nullptr, &v));
            im->mipViews.push_back(v);
        }
    }
    return im;
}
VkSampler Renderer::Impl::sampler(const TextureRef &t) {
    std::array<int, 4> key{t.wrapS, t.wrapT, t.minFilter, t.magFilter};
    if (samplers.contains(key))
        return samplers[key];
    auto wrap = [](int i) {
        return i == 33071   ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
               : i == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                            : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = t.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = t.minFilter == 9728 || t.minFilter == 9984 || t.minFilter == 9986 ? VK_FILTER_NEAREST
                                                                                     : VK_FILTER_LINEAR;
    ci.mipmapMode = t.minFilter == 9728 || t.minFilter == 9729 || t.minFilter == 9984 || t.minFilter == 9985
                        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = wrap(t.wrapS);
    ci.addressModeV = wrap(t.wrapT);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = t.minFilter == 9728 || t.minFilter == 9729 ? 0 : VK_LOD_CLAMP_NONE;
    ci.anisotropyEnable = ci.minFilter == VK_FILTER_LINEAR && ci.magFilter == VK_FILTER_LINEAR;
    ci.maxAnisotropy = std::min(8.f, properties.limits.maxSamplerAnisotropy);
    VkSampler s;
    check(vkCreateSampler(device, &ci, nullptr, &s));
    samplers[key] = s;
    return s;
}
VkDescriptorSet Renderer::Impl::descriptor(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &ai, &set));
    return set;
}
void Renderer::Impl::transition(VkCommandBuffer cmd, Image &im, VkImageLayout layout,
                                VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = im.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                            : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = im.layout == VK_IMAGE_LAYOUT_UNDEFINED
                          ? 0
                          : VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.oldLayout = im.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.handle;
    b.subresourceRange = {im.aspect, 0, im.mips, 0, im.layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    im.layout = layout;
}
void Renderer::Impl::init() {
    uint32_t count = 0;
    auto extensions = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char *> ext(extensions, extensions + count);
    std::vector<const char *> layers;
    if (validation) {
        uint32_t n;
        check(vkEnumerateInstanceLayerProperties(&n, nullptr));
        std::vector<VkLayerProperties> available(n);
        check(vkEnumerateInstanceLayerProperties(&n, available.data()));
        bool found = false;
        for (auto &l : available)
            if (std::string(l.layerName) == "VK_LAYER_KHRONOS_validation")
                found = true;
        if (!found)
            throw std::runtime_error("Validation layer requested but unavailable");
        ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VkEngine";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "VkEngine";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debugCallback;
    debug.pUserData = &errors;
    VkValidationFeatureEnableEXT enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = &enable;
    debug.pNext = &features;
    if (validation)
        ci.pNext = &debug;
    check(vkCreateInstance(&ci, nullptr, &instance));
    debug.pNext = nullptr;
    if (validation)
        check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger));
    check(glfwCreateWindowSurface(instance, window, nullptr, &surface));
    uint32_t n;
    check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    check(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    int best = -1;
    for (auto gpu : devices) {
        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties(gpu, &prop);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(gpu, &f2);
        if (prop.apiVersion < VK_API_VERSION_1_3 || !f13.dynamicRendering || !f13.synchronization2 ||
            !f2.features.samplerAnisotropy)
            continue;
        uint32_t qn;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, queues.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, surface, &present));
            if (!present || !(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                !(queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT) || !queues[q].timestampValidBits)
                continue;
            int score = prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
            if (score > best) {
                best = score;
                physical = gpu;
                family = q;
                properties = prop;
            }
        }
    }
    if (!physical)
        throw std::runtime_error("Vulkan 1.3 graphics/compute GPU with dynamic rendering, synchronization2 "
                                 "and anisotropy required");
    if (!(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts &
          VK_SAMPLE_COUNT_4_BIT))
        throw std::runtime_error("4x MSAA unsupported");
    stats.gpu = properties.deviceName;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceFeatures f{};
    f.samplerAnisotropy = VK_TRUE;
    const char *deviceExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f13;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 2;
    di.ppEnabledExtensionNames = deviceExt;
    di.pEnabledFeatures = &f;
    check(vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VmaAllocatorCreateInfo ai{};
    ai.instance = instance;
    ai.physicalDevice = physical;
    ai.device = device;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    check(vmaCreateAllocator(&ai, &allocator));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 131072},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 128}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 20000;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptorPool));
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    check(vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache));
    createLayouts();
    createSwapchain();
    createFrames();
    createPipelines();
    TextureRef t;
    t.wrapS = t.wrapT = 33071;
    linearSampler = sampler(t);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    check(vkCreateSampler(device, &sci, nullptr, &shadowSampler));
    shadowDepth = image(2048, 2048, VK_FORMAT_D32_SFLOAT,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    white =
        image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    flatNormal =
        image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto initial = buffer(8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    uint8_t pixels[] = {255, 255, 255, 255, 128, 128, 255, 255};
    memcpy(initial.mapped, pixels, 8);
    initial.flush();
    immediate([&](VkCommandBuffer cmd) {
        for (int i = 0; i < 2; i++) {
            auto &im = i ? *flatNormal : *white;
            transition(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.bufferOffset = i * 4;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(cmd, initial.handle, im.handle, im.layout, 1, &c);
            transition(cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    createEnvironment(EnvironmentData{});
    ground = enqueue(makeGround());
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto font = fs::path(L"C:\\Windows\\Fonts\\msyh.ttc");
    if (fs::exists(font))
        io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), 18);
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo vi{};
    vi.ApiVersion = VK_API_VERSION_1_3;
    vi.Instance = instance;
    vi.PhysicalDevice = physical;
    vi.Device = device;
    vi.QueueFamily = family;
    vi.Queue = queue;
    vi.DescriptorPoolSize = 128;
    vi.MinImageCount = 2;
    vi.ImageCount = uint32_t(swapImages.size());
    vi.UseDynamicRendering = true;
    vi.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat;
    vi.CheckVkResultFn = check;
    if (!ImGui_ImplVulkan_Init(&vi))
        throw std::runtime_error("ImGui Vulkan initialization failed");
    imguiInitialized = true;
}
void Renderer::Impl::createLayouts() {
    auto layout = [&](std::vector<VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout &out) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &out));
    };
    std::vector<VkDescriptorSetLayoutBinding> global{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    for (int i = 2; i < 6; i++)
        global.push_back({uint32_t(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                          VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    layout(global, globalLayout);
    std::vector<VkDescriptorSetLayoutBinding> material{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    for (int i = 1; i <= 8; i++)
        material.push_back({uint32_t(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    layout(material, materialLayout);
    layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
           toneSetLayout);
    layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
           computeSetLayout);
    VkDescriptorSetLayout sets[] = {globalLayout, materialLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &ci, nullptr, &meshLayout));
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &toneSetLayout;
    push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    check(vkCreatePipelineLayout(device, &ci, nullptr, &toneLayout));
    ci.pSetLayouts = &computeSetLayout;
    push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    check(vkCreatePipelineLayout(device, &ci, nullptr, &computeLayout));
}
void Renderer::Impl::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    uint32_t n;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(n);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data()));
    auto chosen = formats.at(0);
    for (auto f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = f;
    swapFormat = chosen.format;
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = {std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height)};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr));
        std::vector<VkPresentModeKHR> modes(n);
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, modes.data()));
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                mode = m;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
                mode = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount)
        ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = swapFormat;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));
    check(vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr));
    swapImages.resize(n);
    check(vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data()));
    for (auto im : swapImages) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        check(vkCreateImageView(device, &vi, nullptr, &view));
        swapViews.push_back(view);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem;
        check(vkCreateSemaphore(device, &si, nullptr, &sem));
        presentSemaphores.push_back(sem);
    }
}
void Renderer::Impl::destroySwapchain() {
    for (auto s : presentSemaphores)
        vkDestroySemaphore(device, s, nullptr);
    presentSemaphores.clear();
    for (auto v : swapViews)
        vkDestroyImageView(device, v, nullptr);
    swapViews.clear();
    swapImages.clear();
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = {};
}
void Renderer::Impl::createFrames() {
    for (auto &f : frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ai, &f.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &si, nullptr, &f.acquired));
        f.uniform = buffer(sizeof(GlobalData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.instances = buffer(sizeof(InstanceData) * MaxInstances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.staging = buffer(UploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        f.pickReadback = buffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        f.global = descriptor(globalLayout);
        f.tone = descriptor(toneSetLayout);
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        check(vkCreateQueryPool(device, &qi, nullptr, &f.queries));
    }
}
VkShaderModule Renderer::Impl::shader(const char *name) {
    auto bytes = readFile(executableDirectory() / "shaders" / (std::string(name) + ".spv"));
    if (bytes.size() % 4)
        throw std::runtime_error("Invalid SPIR-V");
    std::vector<uint32_t> aligned(bytes.size() / 4);
    memcpy(aligned.data(), bytes.data(), bytes.size());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes.size();
    ci.pCode = aligned.data();
    VkShaderModule m;
    check(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
}
VkPipeline Renderer::Impl::pipeline(const char *vert, const char *frag, VkPipelineLayout layout,
                                    VkFormat color, VkFormat depthFormat, VkSampleCountFlagBits sample,
                                    VkCullModeFlags cull, VkFrontFace front, bool blend, bool mesh,
                                    bool depthWrite) {
    auto vs = shader(vert), fs = shader(frag);
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages)
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, uint32_t(offsetof(Vertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, uint32_t(offsetof(Vertex, normal))},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Vertex, tangent))},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(Vertex, uv0))},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(Vertex, uv1))},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Vertex, color))}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (mesh) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 6;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull;
    rs.frontFace = front;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = sample;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depthFormat != VK_FORMAT_UNDEFINED;
    ds.depthWriteEnable = ds.depthTestEnable && depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = color == VK_FORMAT_R32_UINT ? VK_COLOR_COMPONENT_R_BIT : 15;
    ba.blendEnable = blend;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = color == VK_FORMAT_UNDEFINED ? 0 : 1;
    bs.pAttachments = &ba;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = color == VK_FORMAT_UNDEFINED ? 0 : 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = depthFormat;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &dy;
    ci.layout = layout;
    VkPipeline out;
    auto result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &ci, nullptr, &out);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    check(result);
    return out;
}
void Renderer::Impl::createPipelines() {
    for (int i = 0; i < 3; i++) {
        auto cull = i == 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        auto front = i == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        opaque[i] = pipeline("mesh.vert", "pbr.frag", meshLayout, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_FORMAT_D32_SFLOAT, samples, cull, front, false, true);
        transparent[i] = pipeline("mesh.vert", "pbr.frag", meshLayout, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_D32_SFLOAT, samples, cull, front, true, true, false);
        shadow[i] = pipeline("mesh.vert", "depth.frag", meshLayout, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT,
                             VK_SAMPLE_COUNT_1_BIT, cull, front, false, true);
        picking[i] = pipeline("mesh.vert", "pick.frag", meshLayout, VK_FORMAT_R32_UINT, VK_FORMAT_D32_SFLOAT,
                              VK_SAMPLE_COUNT_1_BIT, cull, front, false, true);
    }
    skyPipeline = pipeline("fullscreen.vert", "sky.frag", meshLayout, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_FORMAT_D32_SFLOAT, samples, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE,
                           false, false, false);
    tonePipeline =
        pipeline("fullscreen.vert", "tonemap.frag", toneLayout, swapFormat, VK_FORMAT_UNDEFINED,
                 VK_SAMPLE_COUNT_1_BIT, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, false, false);
    auto module = shader("environment.comp");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = computeLayout;
    check(vkCreateComputePipelines(device, pipelineCache, 1, &ci, nullptr, &computePipeline));
    vkDestroyShaderModule(device, module, nullptr);
}
void Renderer::Impl::immediate(const std::function<void(VkCommandBuffer)> &fn) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    check(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi));
    fn(cmd);
    check(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check(vkCreateFence(device, &fi, nullptr, &fence));
    check(vkQueueSubmit2(queue, 1, &si, fence));
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
}
void Renderer::Impl::ensureTargets(uint32_t w, uint32_t h) {
    if (w == renderWidth && h == renderHeight)
        return;
    for (auto &f : frames)
        check(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    completed = submitted;
    renderWidth = w;
    renderHeight = h;
    hdr = image(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    msaa = image(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 1, 1, samples);
    depth = image(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1, 1, samples);
    pickImage = image(w, h, VK_FORMAT_R32_UINT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT);
    pickDepth = image(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    for (auto &f : frames) {
        VkDescriptorImageInfo ii{linearSampler, hdr->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = f.tone;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &ii;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        TextureRef nearest;
        nearest.minFilter = nearest.magFilter = 9728;
        nearest.wrapS = nearest.wrapT = 33071;
        ii = {sampler(nearest), pickImage->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write.dstBinding = 1;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}
void Renderer::Impl::updateGlobals() {
    for (auto &f : frames) {
        VkDescriptorBufferInfo buffers[] = {{f.uniform.handle, 0, sizeof(GlobalData)},
                                            {f.instances.handle, 0, f.instances.size}};
        VkDescriptorImageInfo images[] = {
            {linearSampler, envSpecular->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {linearSampler, envDiffuse->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {linearSampler, envBrdf->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadowSampler, shadowDepth->view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[6]{};
        for (int i = 0; i < 6; i++) {
            auto &w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = f.global;
            w.dstBinding = i;
            w.descriptorCount = 1;
            if (i < 2) {
                w.descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &buffers[i];
            } else {
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &images[i - 2];
            }
        }
        vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
    }
}
void Renderer::Impl::createEnvironment(const EnvironmentData &input) {
    auto previousSource = envSource, previousSpecular = envSpecular, previousDiffuse = envDiffuse,
         previousBrdf = envBrdf;
    try {
        // Environment changes are infrequent. Wait only for owned frame fences before replacing descriptors.
        for (auto &f : frames)
            check(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
        completed = submitted;
        for (auto set : computeSets)
            check(vkFreeDescriptorSets(device, descriptorPool, 1, &set));
        computeSets.clear();
        EnvironmentData data = input;
        if (data.rgba.empty()) {
            data.width = 1024;
            data.height = 512;
            data.rgba.resize(size_t(data.width) * data.height * 4);
            auto softbox = [](glm::vec3 d, glm::vec3 center, float power, float strength) {
                return strength * std::pow(std::max(0.f, glm::dot(d, glm::normalize(center))), power);
            };
            for (int y = 0; y < data.height; y++)
                for (int x = 0; x < data.width; x++) {
                    float theta = (y + .5f) / data.height * glm::pi<float>(),
                          phi = ((x + .5f) / data.width - .5f) * 2 * glm::pi<float>();
                    glm::vec3 d{std::cos(phi) * std::sin(theta), std::cos(theta),
                                std::sin(phi) * std::sin(theta)};
                    glm::vec3 c = glm::mix(glm::vec3(.025f, .035f, .05f), glm::vec3(.32f, .4f, .55f),
                                           glm::clamp(d.y * .5f + .5f, 0.f, 1.f));
                    c += glm::vec3(1, .89f, .75f) * softbox(d, {1, 1, 1}, 60, 18);
                    c += glm::vec3(.65f, .8f, 1) * softbox(d, {-1, .5f, -1}, 35, 9);
                    c += glm::vec3(1) * softbox(d, {0, 1, 0}, 18, 2);
                    size_t i = (size_t(y) * data.width + x) * 4;
                    data.rgba[i] = c.x;
                    data.rgba[i + 1] = c.y;
                    data.rgba[i + 2] = c.z;
                    data.rgba[i + 3] = 1;
                }
        }
        envSource = image(data.width, data.height, VK_FORMAT_R32G32B32A32_SFLOAT,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        envSpecular =
            image(256, 256, VK_FORMAT_R16G16B16A16_SFLOAT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 9, 6, VK_SAMPLE_COUNT_1_BIT, true);
        envDiffuse =
            image(32, 32, VK_FORMAT_R16G16B16A16_SFLOAT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, 6, VK_SAMPLE_COUNT_1_BIT, true);
        envBrdf = image(256, 256, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        auto upload = buffer(data.rgba.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        memcpy(upload.mapped, data.rgba.data(), data.rgba.size() * sizeof(float));
        upload.flush();
        immediate([&](VkCommandBuffer cmd) {
            transition(cmd, *envSource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {uint32_t(data.width), uint32_t(data.height), 1};
            vkCmdCopyBufferToImage(cmd, upload.handle, envSource->handle, envSource->layout, 1, &copy);
            transition(cmd, *envSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            dispatchEnvironment(cmd);
        });
        updateGlobals();
    } catch (...) {
        envSource = previousSource;
        envSpecular = previousSpecular;
        envDiffuse = previousDiffuse;
        envBrdf = previousBrdf;
        throw;
    }
}

void Renderer::Impl::dispatchEnvironment(VkCommandBuffer cmd) {
    TextureRef ref;
    ref.wrapT = 33071;
    ref.minFilter = 9729;
    auto sourceSampler = sampler(ref);
    auto dispatch = [&](Image &dest, int mode, int mip, float roughness, int count) {
        auto set = descriptor(computeSetLayout);
        computeSets.push_back(set);
        VkDescriptorImageInfo ii[] = {
            {sourceSampler, envSource->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, dest.mipViews[mip], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (int i = 0; i < 2; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        struct Push {
            int mode;
            float roughness;
            int size;
            int samples;
        } push{mode, roughness, int(std::max(1u, dest.width >> mip)), count};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.size + 7) / 8, (push.size + 7) / 8, dest.layers);
    };
    transition(cmd, *envSpecular, VK_IMAGE_LAYOUT_GENERAL);
    for (int mip = 0; mip < 9; mip++)
        dispatch(*envSpecular, 0, mip, float(mip) / 8, 128);
    transition(cmd, *envSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, *envDiffuse, VK_IMAGE_LAYOUT_GENERAL);
    dispatch(*envDiffuse, 1, 0, 1, 512);
    transition(cmd, *envDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, *envBrdf, VK_IMAGE_LAYOUT_GENERAL);
    dispatch(*envBrdf, 2, 0, 0, 256);
    transition(cmd, *envBrdf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
AssetHandle Renderer::Impl::enqueue(std::shared_ptr<ImportedAsset> a) {
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    uint64_t available = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            available += budgets[i].budget > budgets[i].usage ? budgets[i].budget - budgets[i].usage : 0;
    if (a->byteSize() > available * 3 / 5)
        throw std::runtime_error("Asset exceeds available GPU memory budget");
    auto gpu = std::make_unique<GpuAsset>();
    gpu->device = device;
    gpu->descriptorPool = descriptorPool;
    gpu->cpu = a;
    gpu->meshes.reserve(a->meshes.size());
    for (auto &m : a->meshes) {
        GpuMesh gm;
        gm.count = uint32_t(m.indices.size());
        gm.material = m.material;
        gm.vertices = buffer(m.vertices.size() * sizeof(Vertex),
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        gm.indices =
            buffer(m.indices.size() * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        gpu->uploads.push_back({reinterpret_cast<const unsigned char *>(m.vertices.data()),
                                m.vertices.size() * sizeof(Vertex),
                                0,
                                gm.vertices.handle,
                                {}});
        gpu->uploads.push_back({reinterpret_cast<const unsigned char *>(m.indices.data()),
                                m.indices.size() * 4,
                                0,
                                gm.indices.handle,
                                {}});
        gpu->meshes.push_back(std::move(gm));
    }
    std::map<std::pair<int, bool>, std::shared_ptr<Image>> textureCache;
    gpu->materials.reserve(a->materials.size());
    for (auto &mat : a->materials) {
        GpuMaterial gm;
        gm.uniform = buffer(sizeof(MaterialData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        MaterialData data;
        data.baseColor = mat.baseColor;
        data.emissive = glm::vec4(mat.emissive, 0);
        data.factors = {mat.metallic, mat.roughness, mat.normalScale, mat.occlusionStrength};
        data.specular = glm::vec4(mat.specularColor, mat.specular);
        data.alpha = {mat.alphaCutoff, float(mat.alphaMode), mat.separateMetalRough ? 1.f : 0.f,
                      mat.unlit ? 1.f : 0.f};
        VkDescriptorImageInfo infos[8];
        for (int i = 0; i < TextureCount; i++) {
            auto &t = mat.textures[i];
            bool srgb = i == BaseColor || i == Emissive || i == SpecularColor;
            std::shared_ptr<Image> texture = i == Normal ? flatNormal : white;
            bool valid = t.image >= 0 && t.image < int(a->images.size());
            if (valid) {
                auto key = std::make_pair(t.image, srgb);
                if (textureCache.contains(key))
                    texture = textureCache[key];
                else {
                    auto &source = a->images[t.image];
                    if (source.width <= 0 || source.height <= 0 ||
                        uint32_t(source.width) > properties.limits.maxImageDimension2D ||
                        uint32_t(source.height) > properties.limits.maxImageDimension2D)
                        throw std::runtime_error("Texture exceeds device dimensions");
                    uint32_t levels =
                        uint32_t(std::floor(std::log2(std::max(source.width, source.height)))) + 1;
                    texture = image(source.width, source.height,
                                    srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                    levels);
                    gpu->textures.push_back(texture);
                    textureCache[key] = texture;
                    gpu->uploads.push_back({source.rgba.data(), source.rgba.size(), 0, {}, texture});
                }
            }
            data.info[i].offsetScale = {t.offset.x, t.offset.y, t.scale.x, t.scale.y};
            data.info[i].rotationUV = {std::sin(t.rotation), std::cos(t.rotation), float(t.uv),
                                       valid ? 1.f : 0.f};
            infos[i] = {sampler(t), texture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        memcpy(gm.uniform.mapped, &data, sizeof(data));
        gm.uniform.flush();
        gm.set = descriptor(materialLayout);
        VkDescriptorBufferInfo bi{gm.uniform.handle, 0, sizeof(data)};
        VkWriteDescriptorSet writes[9]{};
        for (int i = 0; i < 9; i++) {
            auto &w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = gm.set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType =
                i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (i)
                w.pImageInfo = &infos[i - 1];
            else
                w.pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(device, 9, writes, 0, nullptr);
        gpu->materials.push_back(std::move(gm));
    }
    auto id = nextAsset++;
    assets[id] = std::move(gpu);
    return id;
}
void Renderer::Impl::mipmaps(VkCommandBuffer cmd, Image &im) {
    for (uint32_t mip = 1; mip < im.mips; mip++) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im.handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {int32_t(std::max(1u, im.width >> (mip - 1))),
                              int32_t(std::max(1u, im.height >> (mip - 1))), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {int32_t(std::max(1u, im.width >> mip)), int32_t(std::max(1u, im.height >> mip)),
                              1};
        vkCmdBlitImage(cmd, im.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.handle,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.handle;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, im.mips - 1, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
void Renderer::Impl::processUploads(VkCommandBuffer cmd, Frame &f) {
    size_t used = 0;
    for (auto &[id, a] : assets) {
        while (!a->uploads.empty()) {
            auto &part = a->uploads.front();
            used = (used + 255) & ~size_t(255);
            size_t room = UploadBytes - used;
            if (room < 256)
                break;
            size_t amount = std::min(room, part.bytes - part.offset);
            if (part.image) {
                size_t row = size_t(part.image->width) * 4;
                amount = amount / row * row;
                if (!amount)
                    break;
                if (!part.offset)
                    transition(cmd, *part.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                memcpy(static_cast<char *>(f.staging.mapped) + used, part.data + part.offset, amount);
                VkBufferImageCopy c{};
                c.bufferOffset = used;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageOffset = {0, int32_t(part.offset / row), 0};
                c.imageExtent = {part.image->width, uint32_t(amount / row), 1};
                vkCmdCopyBufferToImage(cmd, f.staging.handle, part.image->handle,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            } else {
                amount &= ~size_t(3);
                if (!amount)
                    break;
                memcpy(static_cast<char *>(f.staging.mapped) + used, part.data + part.offset, amount);
                VkBufferCopy copy{used, part.offset, amount};
                vkCmdCopyBuffer(cmd, f.staging.handle, part.buffer, 1, &copy);
            }
            used += amount;
            part.offset += amount;
            if (part.offset == part.bytes) {
                if (part.image)
                    mipmaps(cmd, *part.image);
                a->uploads.pop_front();
            }
            if (used >= UploadBytes - 256)
                break;
        }
        if (a->uploads.empty() && a->readySerial == UINT64_MAX)
            a->readySerial = submitted + 1;
        if (used >= UploadBytes - 256)
            break;
    }
    if (used) {
        f.staging.flush();
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &di);
    }
}
void Renderer::Impl::destroyAsset(GpuAsset &a) {
    for (auto &m : a.materials)
        if (m.set) {
            check(vkFreeDescriptorSets(device, descriptorPool, 1, &m.set));
            m.set = VK_NULL_HANDLE;
        }
}
void Renderer::Impl::collect() {
    while (!retired.empty() && retired.front().serial <= completed) {
        destroyAsset(*retired.front().asset);
        retired.pop_front();
    }
}
void Renderer::Impl::makeDraws(const Scene &scene) {
    draws.clear();
    blends.clear();
    shadowDraws.clear();
    instanceData.clear();
    stats.triangles = 0;
    stats.draws = 0;
    stats.instances = 0;
    struct Item {
        Draw draw;
        InstanceData instance;
    };
    std::vector<Item> opaqueItems, blendItems, shadowItems;
    auto vp = scene.camera.projection(float(renderWidth) / renderHeight) * scene.camera.view();
    auto add = [&](AssetHandle handle, const glm::mat4 &transform, uint32_t objectId) {
        auto it = assets.find(handle);
        if (it == assets.end() || it->second->readySerial > completed)
            return;
        auto &a = *it->second;
        for (auto &node : a.cpu->nodes) {
            auto model = transform * node.world;
            if (std::abs(glm::determinant(glm::mat3(model))) < 1e-12f)
                continue;
            for (auto index : node.meshes) {
                auto &mesh = a.cpu->meshes[index];
                auto bounds = mesh.bounds.transformed(model);
                Draw d;
                d.asset = &a;
                d.mesh = index;
                d.mirrored = glm::determinant(glm::mat3(model)) < 0;
                d.distance = glm::distance(bounds.center(), scene.camera.eye());
                InstanceData inst{model, {objectId, 0, 0, 0}};
                auto &mat = a.cpu->materials[mesh.material];
                if (objectId && mat.alphaMode != 2)
                    shadowItems.push_back({d, inst});
                if (!visibleBounds(bounds, vp))
                    continue;
                if (mat.alphaMode == 2)
                    blendItems.push_back({d, inst});
                else
                    opaqueItems.push_back({d, inst});
                stats.triangles += mesh.indices.size() / 3;
                stats.instances++;
            }
        }
    };
    if (scene.environment.ground)
        add(ground, glm::mat4(1), 0);
    for (auto &o : scene.objects)
        if (o.visible && o.asset)
            add(o.asset, o.transform.matrix(), o.id);
    auto key = [](const Item &i) {
        return std::tuple(i.draw.asset, i.draw.asset->meshes[i.draw.mesh].material, i.draw.mesh,
                          i.draw.mirrored);
    };
    auto group = [&](std::vector<Item> &items, std::vector<Draw> &output, bool sorted) {
        if (sorted)
            std::sort(items.begin(), items.end(),
                      [&](const Item &a, const Item &b) { return key(a) < key(b); });
        for (auto &item : items) {
            if (instanceData.size() >= MaxInstances)
                throw std::runtime_error("Scene exceeds 100,000 render instances");
            bool merge = sorted && !output.empty() && output.back().asset == item.draw.asset &&
                         output.back().mesh == item.draw.mesh && output.back().mirrored == item.draw.mirrored;
            if (merge)
                output.back().count++;
            else {
                item.draw.first = uint32_t(instanceData.size());
                output.push_back(item.draw);
            }
            instanceData.push_back(item.instance);
        }
    };
    group(opaqueItems, draws, true);
    std::sort(blendItems.begin(), blendItems.end(),
              [](const Item &a, const Item &b) { return a.draw.distance > b.draw.distance; });
    group(blendItems, blends, false);
    group(shadowItems, shadowDraws, true);
}
void Renderer::Impl::viewport(VkCommandBuffer cmd, float w, float h, float x, float y) {
    VkViewport v{x, y, w, h, 0, 1};
    VkRect2D s{{int32_t(x), int32_t(y)}, {uint32_t(w), uint32_t(h)}};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &s);
}
void Renderer::Impl::drawList(VkCommandBuffer cmd, const std::vector<Draw> &list, int mode) {
    int push = mode == 1 ? 1 : 0;
    vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &push);
    for (auto &draw : list) {
        auto &mesh = draw.asset->meshes[draw.mesh];
        auto &mat = draw.asset->cpu->materials[mesh.material];
        int variant = mat.doubleSided ? 2 : draw.mirrored ? 1 : 0;
        auto pipe = mode == 1            ? shadow[variant]
                    : mode == 3          ? picking[variant]
                    : mat.alphaMode == 2 ? transparent[variant]
                                         : opaque[variant];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        auto set = draw.asset->materials[mesh.material].set;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &set, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.handle, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.indices.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh.count, draw.count, 0, 0, draw.first);
        stats.draws++;
    }
}
void Renderer::Impl::renderScene(VkCommandBuffer cmd, const Scene &, uint32_t selected) {
    auto global = frames[frameIndex].global;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &global, 0, nullptr);
    transition(cmd, *shadowDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo shadowAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    shadowAttachment.imageView = shadowDepth->view;
    shadowAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    shadowAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    shadowAttachment.clearValue.depthStencil = {1, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {2048, 2048}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &shadowAttachment;
    vkCmdBeginRendering(cmd, &ri);
    viewport(cmd, 2048, 2048);
    drawList(cmd, shadowDraws, 1);
    vkCmdEndRendering(cmd);
    transition(cmd, *shadowDepth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    transition(cmd, *msaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, *hdr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, *depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = hdr->view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = {{.02f, .03f, .05f, 1}};
    VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = depth->view;
    dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dep.clearValue.depthStencil = {1, 0};
    ri.renderArea = {{0, 0}, {renderWidth, renderHeight}};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(cmd, &ri);
    viewport(cmd, float(renderWidth), float(renderHeight));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    stats.draws++;
    drawList(cmd, draws, 0);
    drawList(cmd, blends, 0);
    vkCmdEndRendering(cmd);
    transition(cmd, *hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
void Renderer::Impl::pick(VkCommandBuffer cmd, Frame &f, uint32_t selected) {
    auto request = std::exchange(pickRequest, {});
    if (!selected && !request) {
        transition(cmd, *pickImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }
    bool valid = request && request->x >= 0 && request->y >= 0 && request->x < int(renderWidth) &&
                 request->y < int(renderHeight);
    transition(cmd, *pickImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, *pickDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = pickImage->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.uint32[0] = 0;
    VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = pickDepth->view;
    dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dep.clearValue.depthStencil = {1, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {renderWidth, renderHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(cmd, &ri);
    viewport(cmd, float(renderWidth), float(renderHeight));
    if (!selected && valid) {
        VkRect2D scissor{{request->x, request->y}, {1, 1}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    drawList(cmd, draws, 3);
    drawList(cmd, blends, 3);
    vkCmdEndRendering(cmd);
    if (valid) {
        transition(cmd, *pickImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {request->x, request->y, 0};
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, pickImage->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               f.pickReadback.handle, 1, &copy);
        f.hasPick = true;
    }
    transition(cmd, *pickImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
void Renderer::Impl::waitFrame() {
    auto &f = frames[frameIndex];
    check(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    completed = std::max(completed, f.serial);
    if (f.hasQueries) {
        uint64_t times[2]{};
        check(
            vkGetQueryPoolResults(device, f.queries, 0, 2, sizeof(times), times, 8, VK_QUERY_RESULT_64_BIT));
        stats.gpuMs = double(times[1] - times[0]) * properties.limits.timestampPeriod / 1e6;
        f.hasQueries = false;
    }
    if (f.hasPick) {
        vmaInvalidateAllocation(allocator, f.pickReadback.allocation, 0, 4);
        pickResult = *static_cast<uint32_t *>(f.pickReadback.mapped);
        f.hasPick = false;
    }
    if (!f.screenshotPath.empty()) {
        vmaInvalidateAllocation(allocator, f.screenshotBuffer.allocation, 0, VK_WHOLE_SIZE);
        auto *data = static_cast<unsigned char *>(f.screenshotBuffer.mapped);
        if (swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < size_t(f.screenshotWidth) * f.screenshotHeight; i++)
                std::swap(data[i * 4], data[i * 4 + 2]);
        std::ofstream output(f.screenshotPath, std::ios::binary);
        auto callback = [](void *c, void *d, int size) {
            static_cast<std::ofstream *>(c)->write(static_cast<char *>(d), size);
        };
        if (!output || !stbi_write_png_to_func(callback, &output, f.screenshotWidth, f.screenshotHeight, 4,
                                               data, f.screenshotWidth * 4))
            messages.push_back("Screenshot failed");
        else
            messages.push_back("Screenshot saved: " + utf8(f.screenshotPath));
        f.screenshotPath.clear();
    }
    collect();
    stats.validationErrors = errors.load();
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    stats.allocatedBytes = stats.budgetBytes = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            stats.allocatedBytes += budgets[i].statistics.allocationBytes;
            stats.budgetBytes += budgets[i].budget;
        }
}
void Renderer::Impl::render(const Scene &scene, Viewport view, uint32_t selected) {
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    auto &f = frames[frameIndex];
    if (rebuild || uint32_t(width) != extent.width || uint32_t(height) != extent.height) {
        check(vkDeviceWaitIdle(device));
        completed = submitted;
        destroySwapchain();
        createSwapchain();
        ImGui_ImplVulkan_SetMinImageCount(2);
        rebuild = false;
    }
    ensureTargets(std::max(1, view.width), std::max(1, view.height));
    uint32_t swapIndex;
    auto result =
        vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &swapIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuild = true;
        return;
    }
    if (result != VK_SUBOPTIMAL_KHR)
        check(result);
    else
        rebuild = true;
    makeDraws(scene);
    GlobalData globals;
    globals.vp = scene.camera.projection(float(renderWidth) / renderHeight) * scene.camera.view();
    globals.inverseVP = glm::inverse(globals.vp);
    globals.camera = glm::vec4(scene.camera.eye(), 1);
    auto direction = glm::normalize(scene.environment.lightDirection);
    globals.light = glm::vec4(direction, scene.environment.lightIntensity);
    globals.lightColor = glm::vec4(scene.environment.lightColor, 1);
    globals.environment = {scene.environment.exposure, scene.environment.rotation,
                           scene.environment.intensity, 0};
    Bounds sceneBounds;
    for (auto &o : scene.objects)
        if (o.visible && assets.contains(o.asset))
            sceneBounds.add(assets[o.asset]->cpu->bounds.transformed(o.transform.matrix()));
    if (!sceneBounds.valid()) {
        sceneBounds.add({-2, 0, -2});
        sceneBounds.add({2, 2, 2});
    }
    float radius = std::max(2.f, glm::length(sceneBounds.size()) * .6f);
    auto center = sceneBounds.center();
    auto lightView = glm::lookAt(center - direction * (radius * 2 + 10), center,
                                 std::abs(direction.y) > .95f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
    auto lightProjection = glm::ortho(-radius, radius, -radius, radius, .1f, radius * 4 + 20);
    lightProjection[1][1] *= -1;
    globals.lightVP = lightProjection * lightView;
    memcpy(f.uniform.mapped, &globals, sizeof(globals));
    f.uniform.flush();
    if (!instanceData.empty())
        memcpy(f.instances.mapped, instanceData.data(), instanceData.size() * sizeof(InstanceData));
    f.instances.flush();
    check(vkResetCommandPool(device, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &bi));
    vkCmdResetQueryPool(f.cmd, f.queries, 0, 2);
    vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, f.queries, 0);
    processUploads(f.cmd, f);
    renderScene(f.cmd, scene, selected);
    pick(f.cmd, f, selected);
    VkImageMemoryBarrier2 swapBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    swapBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    swapBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapBarrier.srcQueueFamilyIndex = swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image = swapImages[swapIndex];
    swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &swapBarrier;
    vkCmdPipelineBarrier2(f.cmd, &dependency);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = swapViews[swapIndex];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{.035f, .045f, .06f, 1}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(f.cmd, &rendering);
    viewport(f.cmd, float(view.width), float(view.height), float(view.x), float(view.y));
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonePipeline);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, toneLayout, 0, 1, &f.tone, 0, nullptr);
    struct TonePush {
        float exposure, srgb;
        uint32_t selected, pad;
    } push{scene.environment.exposure,
           swapFormat == VK_FORMAT_B8G8R8A8_SRGB || swapFormat == VK_FORMAT_R8G8B8A8_SRGB ? 1.f : 0.f,
           selected, 0};
    vkCmdPushConstants(f.cmd, toneLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &push);
    vkCmdDraw(f.cmd, 3, 1, 0, 0);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    vkCmdEndRendering(f.cmd);
    if (!captureRequest.empty()) {
        if (swapFormat != VK_FORMAT_B8G8R8A8_UNORM && swapFormat != VK_FORMAT_B8G8R8A8_SRGB &&
            swapFormat != VK_FORMAT_R8G8B8A8_UNORM && swapFormat != VK_FORMAT_R8G8B8A8_SRGB) {
            messages.push_back("Screenshot format unsupported");
            captureRequest.clear();
        } else {
            VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
            if (f.screenshotBuffer.size < bytes)
                f.screenshotBuffer = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
            swapBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            swapBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            swapBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            swapBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            swapBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            swapBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier2(f.cmd, &dependency);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(f.cmd, swapImages[swapIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   f.screenshotBuffer.handle, 1, &copy);
            f.screenshotPath = std::exchange(captureRequest, {});
            f.screenshotWidth = extent.width;
            f.screenshotHeight = extent.height;
        }
    }
    swapBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    swapBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    swapBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    swapBarrier.dstAccessMask = 0;
    swapBarrier.oldLayout = f.screenshotPath.empty() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(f.cmd, &dependency);
    vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, f.queries, 1);
    check(vkEndCommandBuffer(f.cmd));
    check(vkResetFences(device, 1, &f.fence));
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = f.cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = presentSemaphores[swapIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    check(vkQueueSubmit2(queue, 1, &submit, f.fence));
    f.serial = ++submitted;
    f.hasQueries = true;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &presentSemaphores[swapIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &swapIndex;
    result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        rebuild = true;
    else
        check(result);
    frameIndex = (frameIndex + 1) % 2;
}
Renderer::Impl::~Impl() {
    if (!device)
        return;
    vkDeviceWaitIdle(device);
    if (imguiInitialized) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    for (auto &[id, a] : assets)
        destroyAsset(*a);
    assets.clear();
    for (auto &a : retired)
        destroyAsset(*a.asset);
    retired.clear();
    hdr.reset();
    msaa.reset();
    depth.reset();
    shadowDepth.reset();
    pickImage.reset();
    pickDepth.reset();
    white.reset();
    flatNormal.reset();
    envSource.reset();
    envSpecular.reset();
    envDiffuse.reset();
    envBrdf.reset();
    for (auto &f : frames) {
        f.uniform.destroy();
        f.instances.destroy();
        f.staging.destroy();
        f.pickReadback.destroy();
        f.screenshotBuffer.destroy();
        vkDestroyQueryPool(device, f.queries, nullptr);
        vkDestroyFence(device, f.fence, nullptr);
        vkDestroySemaphore(device, f.acquired, nullptr);
        vkDestroyCommandPool(device, f.pool, nullptr);
    }
    for (auto &[k, s] : samplers)
        vkDestroySampler(device, s, nullptr);
    vkDestroySampler(device, shadowSampler, nullptr);
    for (auto *group : {opaque, transparent, shadow, picking})
        for (int i = 0; i < 3; i++)
            vkDestroyPipeline(device, group[i], nullptr);
    for (auto p : {skyPipeline, tonePipeline, computePipeline})
        vkDestroyPipeline(device, p, nullptr);
    vkDestroyPipelineCache(device, pipelineCache, nullptr);
    for (auto p : {meshLayout, toneLayout, computeLayout})
        vkDestroyPipelineLayout(device, p, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    for (auto l : {globalLayout, materialLayout, toneSetLayout, computeSetLayout})
        vkDestroyDescriptorSetLayout(device, l, nullptr);
    destroySwapchain();
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger)
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
}
Renderer::Renderer(GLFWwindow *w, bool validation, bool vsync)
    : p(std::make_unique<Impl>(w, validation, vsync)) {}
Renderer::~Renderer() = default;
void Renderer::newFrame() {
    p->waitFrame();
    if (p->environmentJob.valid() &&
        p->environmentJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto data = p->environmentJob.get();
            if (!p->nextEnvironmentPath) {
                p->createEnvironment(data);
                p->activeEnvironmentPath = p->requestedEnvironmentPath;
                p->messages.push_back("Environment ready");
            }
        } catch (const std::exception &e) {
            p->messages.push_back(e.what());
        }
        if (p->nextEnvironmentPath) {
            auto next = std::exchange(p->nextEnvironmentPath, {});
            setEnvironment(*next);
        }
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}
void Renderer::render(const Scene &s, Viewport viewport, uint32_t selected) {
    p->render(s, viewport, selected);
}
AssetHandle Renderer::enqueueUpload(std::shared_ptr<ImportedAsset> a) {
    return p->enqueue(std::move(a));
}
bool Renderer::ready(AssetHandle h) const {
    auto it = p->assets.find(h);
    return it != p->assets.end() && it->second->readySerial <= p->completed;
}
std::shared_ptr<const ImportedAsset> Renderer::asset(AssetHandle h) const {
    auto it = p->assets.find(h);
    return it == p->assets.end() ? nullptr : it->second->cpu;
}
void Renderer::retainOnly(const std::vector<AssetHandle> &handles) {
    std::set<AssetHandle> keep(handles.begin(), handles.end());
    keep.insert(p->ground);
    for (auto it = p->assets.begin(); it != p->assets.end();) {
        if (keep.contains(it->first)) {
            ++it;
            continue;
        }
        p->retired.push_back({p->submitted, std::move(it->second)});
        it = p->assets.erase(it);
    }
    p->collect();
}
void Renderer::requestPick(int x, int y) {
    p->pickRequest = glm::ivec2(x, y);
}
std::optional<uint32_t> Renderer::picked() {
    return std::exchange(p->pickResult, {});
}
void Renderer::setEnvironment(const fs::path &path) {
    if (p->environmentJob.valid()) {
        p->nextEnvironmentPath = path;
        return;
    }
    p->requestedEnvironmentPath = path;
    p->environmentJob = std::async(std::launch::async, [path]() {
        EnvironmentData data;
        if (path.empty())
            return data;
        auto bytes = readFile(path);
        int channels = 0;
        auto *pixels = stbi_loadf_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                              int(bytes.size()), &data.width, &data.height, &channels, 4);
        if (!pixels)
            throw std::runtime_error("Cannot decode HDR environment: " + utf8(path));
        if (data.width > 16384 || data.height > 8192) {
            stbi_image_free(pixels);
            throw std::runtime_error("HDR environment exceeds 16384x8192");
        }
        data.rgba.assign(pixels, pixels + size_t(data.width) * data.height * 4);
        stbi_image_free(pixels);
        return data;
    });
}
bool Renderer::environmentBusy() const {
    return p->environmentJob.valid();
}
fs::path Renderer::environmentPath() const {
    return p->activeEnvironmentPath;
}
void Renderer::screenshot(const fs::path &path) {
    p->captureRequest = path;
}
const RenderStats &Renderer::stats() const {
    return p->stats;
}
std::vector<std::string> Renderer::takeMessages() {
    return std::exchange(p->messages, {});
}
} // namespace vke
