#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "stress.vert.h"
#include "stress.frag.h"

// Vulkan 1.0, native Wayland, one graphics submission in flight.
// Presentation is queued BEFORE waiting for its rendering fence.
static volatile sig_atomic_t running = 1;
static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_surface *window;
    struct xdg_wm_base *wm;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    bool configured, resized;
    uint32_t width, height, iterations, passes, device_index;
    uint64_t frame_limit;
    double hz;
    bool fifo, explicit_device;

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkDevice device;
    uint32_t family, timestamp_bits;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    VkSemaphore *finished; // Per image: presentation may outlive a submit fence.
    VkRenderPass render_pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired;
    VkFence fence;
    VkQueryPool queries;
} app;

static void die(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(EXIT_FAILURE);
}

#define VK(call) do { \
    VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) { \
        fprintf(stderr, "%s: VkResult %d\n", #call, result_); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void *alloc(size_t count, size_t size)
{
    void *p = calloc(count, size);
    if (!p) die("Out of memory");
    return p;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void stop(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = ping };

static void registry_global(void *data, struct wl_registry *registry, uint32_t id,
                            const char *interface, uint32_t version)
{
    (void)data;
    if (!strcmp(interface, "wl_compositor"))
        app.compositor = wl_registry_bind(registry, id, &wl_compositor_interface,
                                         version < 4 ? version : 4);
    else if (!strcmp(interface, "xdg_wm_base")) {
        app.wm = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app.wm, &wm_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t id)
{
    (void)data; (void)registry; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove
};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    app.configured = true;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)toplevel; (void)states;
    if (width > 0 && height > 0 &&
        ((uint32_t)width != app.width || (uint32_t)height != app.height)) {
        app.width = (uint32_t)width;
        app.height = (uint32_t)height;
        app.resized = true;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data; (void)toplevel;
    running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close
};

static void init_wayland(void)
{
    app.display = wl_display_connect(NULL);
    if (!app.display) die("Cannot connect to Wayland; run inside a Wayland session.");
    struct wl_registry *registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(app.display) < 0) die("Wayland connection failed");
    wl_registry_destroy(registry);
    if (!app.compositor || !app.wm) die("Compositor lacks wl_compositor or xdg_wm_base");
    app.window = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm, app.window);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, NULL);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(app.toplevel, "Wayland Vulkan GPU stress");
    xdg_toplevel_set_app_id(app.toplevel, "gpu-stress");
    wl_surface_commit(app.window);
    while (!app.configured && running)
        if (wl_display_dispatch(app.display) < 0) die("Wayland connection failed");
}

// Read only events already available. Fence/acquire timeouts keep this responsive.
static void pump_wayland(void)
{
    while (wl_display_prepare_read(app.display) != 0)
        if (wl_display_dispatch_pending(app.display) < 0) die("Wayland connection failed");
    int flushed = wl_display_flush(app.display);
    if (flushed < 0 && errno != EAGAIN) {
        wl_display_cancel_read(app.display);
        die("Wayland flush failed");
    }
    struct pollfd fd = { .fd = wl_display_get_fd(app.display), .events = POLLIN };
    int ready = poll(&fd, 1, 0);
    if (ready > 0 && (fd.revents & POLLIN)) {
        if (wl_display_read_events(app.display) < 0) die("Wayland read failed");
    } else {
        wl_display_cancel_read(app.display);
        if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            die("Wayland connection closed");
        if (ready < 0 && errno != EINTR) die("Wayland poll failed");
    }
    if (wl_display_dispatch_pending(app.display) < 0) die("Wayland dispatch failed");
}

static bool supports_swapchain(VkPhysicalDevice physical)
{
    uint32_t count;
    VK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = alloc(count, sizeof(*extensions));
    VK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) found = true;
    free(extensions);
    return found;
}

static void init_vulkan(void)
{
    const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gpu-stress", .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions
    };
    VK(vkCreateInstance(&instance, NULL, &app.instance));
    VkWaylandSurfaceCreateInfoKHR surface = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = app.display, .surface = app.window
    };
    VK(vkCreateWaylandSurfaceKHR(app.instance, &surface, NULL, &app.surface));
    uint32_t count;
    VK(vkEnumeratePhysicalDevices(app.instance, &count, NULL));
    if (!count) die("No Vulkan devices found");
    VkPhysicalDevice *devices = alloc(count, sizeof(*devices));
    VK(vkEnumeratePhysicalDevices(app.instance, &count, devices));
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        fprintf(stderr, "Device %u: %s%s\n", i, properties.deviceName,
                properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? " (software renderer)" : "");
        if (app.physical || (app.explicit_device && i != app.device_index) ||
            !supports_swapchain(devices[i])) continue;
        uint32_t families;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        VkQueueFamilyProperties *queues = alloc(families, sizeof(*queues));
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, queues);
        for (uint32_t j = 0; j < families; ++j) {
            VkBool32 present;
            VK(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, app.surface, &present));
            if (!(queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            app.physical = devices[i];
            app.properties = properties;
            app.family = j;
            app.timestamp_bits = queues[j].timestampValidBits;
            break;
        }
        free(queues);
    }
    free(devices);
    if (!app.physical) die("No suitable Vulkan graphics/present device (check --device)");
    fprintf(stderr, "Using: %s\n", app.properties.deviceName);
    if (app.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        fprintf(stderr, "Software rendering cannot test physical GPU contention.\n");
    float priority = 0.5f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app.family, .queueCount = 1, .pQueuePriorities = &priority
    };
    const char *swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &swapchain
    };
    VK(vkCreateDevice(app.physical, &device, NULL, &app.device));
    vkGetDeviceQueue(app.device, app.family, 0, &app.queue);
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = app.family
    };
    VK(vkCreateCommandPool(app.device, &pool, NULL, &app.pool));
    VkCommandBufferAllocateInfo cmd = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VK(vkAllocateCommandBuffers(app.device, &cmd, &app.cmd));
    VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(app.device, &semaphore, NULL, &app.acquired));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VK(vkCreateFence(app.device, &fence, NULL, &app.fence));
    if (app.timestamp_bits) {
        VkQueryPoolCreateInfo queries = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2
        };
        VK(vkCreateQueryPool(app.device, &queries, NULL, &app.queries));
    } else fprintf(stderr, "Device has no timestamp support; only CPU frame rate is reported.\n");
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .size = 16
    };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push
    };
    VK(vkCreatePipelineLayout(app.device, &layout, NULL, &app.layout));
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code
    };
    VkShaderModule module;
    VK(vkCreateShaderModule(app.device, &info, NULL, &module));
    return module;
}

static void create_pipeline(VkFormat format)
{
    VkAttachmentDescription attachment = {
        .format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    VkAttachmentReference color = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color
    };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };
    VkRenderPassCreateInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1,
        .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency
    };
    VK(vkCreateRenderPass(app.device, &render, NULL, &app.render_pass));
    VkShaderModule vert = shader(stress_vert, sizeof(stress_vert));
    VkShaderModule frag = shader(stress_frag, sizeof(stress_frag));
    VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" }
    };
    VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    // Every pass contributes to the image, so earlier draws cannot be discarded.
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment
    };
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dynamic_states
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = app.layout, .renderPass = app.render_pass
    };
    VK(vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app.pipeline));
    vkDestroyShaderModule(app.device, vert, NULL);
    vkDestroyShaderModule(app.device, frag, NULL);
}

static void destroy_swapchain(void)
{
    for (uint32_t i = 0; i < app.image_count; ++i) {
        vkDestroyFramebuffer(app.device, app.framebuffers[i], NULL);
        vkDestroyImageView(app.device, app.views[i], NULL);
        vkDestroySemaphore(app.device, app.finished[i], NULL);
    }
    free(app.framebuffers); free(app.views); free(app.finished);
    app.image_count = 0;
    vkDestroyPipeline(app.device, app.pipeline, NULL);
    vkDestroyRenderPass(app.device, app.render_pass, NULL);
    vkDestroySwapchainKHR(app.device, app.swapchain, NULL);
}

static uint32_t clamp(uint32_t n, uint32_t lo, uint32_t hi)
{
    return n < lo ? lo : n > hi ? hi : n;
}

static void create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.physical, app.surface, &caps));
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        die("Surface does not support color attachments");
    app.extent = caps.currentExtent;
    if (app.extent.width == UINT32_MAX) app.extent = (VkExtent2D) {
        clamp(app.width, caps.minImageExtent.width, caps.maxImageExtent.width),
        clamp(app.height, caps.minImageExtent.height, caps.maxImageExtent.height)
    };
    uint32_t count;
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(app.physical, app.surface, &count, NULL));
    if (!count) die("No surface formats");
    VkSurfaceFormatKHR *formats = alloc(count, sizeof(*formats));
    VK(vkGetPhysicalDeviceSurfaceFormatsKHR(app.physical, app.surface, &count, formats));
    VkSurfaceFormatKHR format = formats[0];
    if (count == 1 && format.format == VK_FORMAT_UNDEFINED)
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = formats[i];
    free(formats);
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(app.physical, format.format, &format_properties);
    if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
        die("Surface format does not support blending");
    VK(vkGetPhysicalDeviceSurfacePresentModesKHR(app.physical, app.surface, &count, NULL));
    VkPresentModeKHR *modes = alloc(count, sizeof(*modes));
    VK(vkGetPhysicalDeviceSurfacePresentModesKHR(app.physical, app.surface, &count, modes));
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!app.fifo)
        for (uint32_t i = 0; i < count; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    free(modes);
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    // Prefer opaque so the compositor doesn't need alpha blending for this window.
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    while (!(caps.supportedCompositeAlpha & alpha)) alpha <<= 1;
    VkSwapchainCreateInfoKHR swapchain = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = app.surface, .minImageCount = images,
        .imageFormat = format.format, .imageColorSpace = format.colorSpace,
        .imageExtent = app.extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = mode, .clipped = VK_TRUE
    };
    VK(vkCreateSwapchainKHR(app.device, &swapchain, NULL, &app.swapchain));
    VK(vkGetSwapchainImagesKHR(app.device, app.swapchain, &app.image_count, NULL));
    VkImage *swap_images = alloc(app.image_count, sizeof(*swap_images));
    VK(vkGetSwapchainImagesKHR(app.device, app.swapchain, &app.image_count, swap_images));
    app.views = alloc(app.image_count, sizeof(*app.views));
    app.framebuffers = alloc(app.image_count, sizeof(*app.framebuffers));
    app.finished = alloc(app.image_count, sizeof(*app.finished));
    create_pipeline(format.format);
    for (uint32_t i = 0; i < app.image_count; ++i) {
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = swap_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format.format,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1, .layerCount = 1 }
        };
        VK(vkCreateImageView(app.device, &view, NULL, &app.views[i]));
        VkFramebufferCreateInfo framebuffer = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app.render_pass, .attachmentCount = 1,
            .pAttachments = &app.views[i], .width = app.extent.width,
            .height = app.extent.height, .layers = 1
        };
        VK(vkCreateFramebuffer(app.device, &framebuffer, NULL, &app.framebuffers[i]));
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK(vkCreateSemaphore(app.device, &semaphore, NULL, &app.finished[i]));
    }
    free(swap_images);
    app.resized = false;
    fprintf(stderr, "%ux%u, %u images, %s presentation\n", app.extent.width, app.extent.height,
            app.image_count, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");
}

static void record(uint32_t image, float time)
{
    VK(vkResetCommandBuffer(app.cmd, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    VK(vkBeginCommandBuffer(app.cmd, &begin));
    if (app.queries) {
        vkCmdResetQueryPool(app.cmd, app.queries, 0, 2);
        vkCmdWriteTimestamp(app.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, app.queries, 0);
    }
    VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
    VkRenderPassBeginInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = app.render_pass,
        .framebuffer = app.framebuffers[image], .renderArea = { .extent = app.extent },
        .clearValueCount = 1, .pClearValues = &clear
    };
    vkCmdBeginRenderPass(app.cmd, &render, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = { .width = (float)app.extent.width, .height = (float)app.extent.height,
                            .maxDepth = 1.0f };
    VkRect2D scissor = { .extent = app.extent };
    vkCmdSetViewport(app.cmd, 0, 1, &viewport);
    vkCmdSetScissor(app.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(app.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipeline);
    struct { uint32_t iterations; float time, seed, weight; } work = {
        app.iterations, time, 0, 1.0f / app.passes
    };
    for (uint32_t p = 0; p < app.passes; ++p) {
        work.seed = (float)p * 0.123f;
        vkCmdPushConstants(app.cmd, app.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(work), &work);
        vkCmdDraw(app.cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(app.cmd);
    if (app.queries)
        vkCmdWriteTimestamp(app.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, app.queries, 1);
    VK(vkEndCommandBuffer(app.cmd));
}

static void report(uint64_t samples, double elapsed, double sum, double maximum, uint64_t over)
{
    if (!samples) return;
    fprintf(stderr, "%.1f frames/s", samples / elapsed);
    if (app.queries)
        fprintf(stderr, " | GPU avg %.2f ms, max %.2f ms | %" PRIu64 "/%" PRIu64
                " above %.2f ms (%.1f Hz)", sum / samples, maximum, over, samples,
                1000.0 / app.hz, app.hz);
    fputc('\n', stderr);
}

static void render_loop(void)
{
    uint64_t submitted = 0, samples = 0, over = 0;
    bool pending = false;
    double start = now(), last_report = start, sum = 0, maximum = 0;
    while (running) {
        pump_wayland();
        if (!running) break;
        VkResult wait = vkWaitForFences(app.device, 1, &app.fence, VK_TRUE, 10000000);
        if (wait == VK_TIMEOUT) continue;
        VK(wait);
        if (pending) {
            ++samples;
            if (app.queries) {
                uint64_t ticks[2];
                VK(vkGetQueryPoolResults(app.device, app.queries, 0, 2, sizeof(ticks), ticks,
                                        sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT));
                uint64_t mask = app.timestamp_bits == 64 ? UINT64_MAX :
                                (UINT64_C(1) << app.timestamp_bits) - 1;
                double ms = ((ticks[1] - ticks[0]) & mask) *
                            (double)app.properties.limits.timestampPeriod / 1e6;
                sum += ms;
                if (ms > maximum) maximum = ms;
                if (ms > 1000.0 / app.hz) ++over;
            }
            pending = false;
            double t = now();
            if (t - last_report >= 1.0) {
                report(samples, t - last_report, sum, maximum, over);
                last_report = t; samples = over = 0; sum = maximum = 0;
            }
        }
        if (app.frame_limit && submitted >= app.frame_limit) break;
        if (app.resized) {
            VK(vkDeviceWaitIdle(app.device));
            destroy_swapchain();
            create_swapchain();
        }
        uint32_t image;
        VkResult acquired = vkAcquireNextImageKHR(app.device, app.swapchain, 10000000,
                                                  app.acquired, VK_NULL_HANDLE, &image);
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) continue;
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { app.resized = true; continue; }
        if (acquired != VK_SUBOPTIMAL_KHR) VK(acquired);
        // SUBOPTIMAL still supplies an image and signals the acquire semaphore.
        record(image, (float)(now() - start));
        VK(vkResetFences(app.device, 1, &app.fence));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &app.acquired, .pWaitDstStageMask = &stage,
            .commandBufferCount = 1, .pCommandBuffers = &app.cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &app.finished[image]
        };
        VK(vkQueueSubmit(app.queue, 1, &submit, app.fence));
        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &app.finished[image], .swapchainCount = 1,
            .pSwapchains = &app.swapchain, .pImageIndices = &image
        };
        // Do not wait for the fence here: hand WSI a still-rendering buffer.
        VkResult presented = vkQueuePresentKHR(app.queue, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            app.resized = true;
        else VK(presented);
        ++submitted;
        pending = true;
    }
    report(samples, now() - last_report, sum, maximum, over);
}

static void cleanup(void)
{
    VK(vkDeviceWaitIdle(app.device));
    destroy_swapchain();
    vkDestroyQueryPool(app.device, app.queries, NULL);
    vkDestroyFence(app.device, app.fence, NULL);
    vkDestroySemaphore(app.device, app.acquired, NULL);
    vkDestroyCommandPool(app.device, app.pool, NULL);
    vkDestroyPipelineLayout(app.device, app.layout, NULL);
    vkDestroyDevice(app.device, NULL);
    vkDestroySurfaceKHR(app.instance, app.surface, NULL);
    vkDestroyInstance(app.instance, NULL);
    xdg_toplevel_destroy(app.toplevel);
    xdg_surface_destroy(app.xdg_surface);
    wl_surface_destroy(app.window);
    xdg_wm_base_destroy(app.wm);
    wl_compositor_destroy(app.compositor);
    wl_display_disconnect(app.display);
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --iterations N  Dependent math iterations per pixel (default 1024)\n"
           "  --passes N      Blended fullscreen draws per frame (default 1)\n"
           "  --width N       Initial window width (default 960)\n"
           "  --height N      Initial window height (default 540)\n"
           "  --hz HZ         Refresh rate for timing comparison (default 60)\n"
           "  --device N      Vulkan device index (default first suitable)\n"
           "  --frames N      Exit after N frames (default unlimited)\n"
           "  --fifo          Use FIFO instead of preferring MAILBOX\n"
           "  --help          Show this help\n"
           "Close the window or press Ctrl-C in the terminal to stop.\n", program);
}

static uint32_t number(const char *text, bool allow_zero)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || end == text || *end || *text == '-' || n > UINT32_MAX || (!n && !allow_zero))
        die("Expected an integer between 1 and 4294967295 (device index may be 0)");
    return (uint32_t)n;
}

int main(int argc, char **argv)
{
    app.width = 960; app.height = 540; app.iterations = 1024; app.passes = 1; app.hz = 60;
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(option, "--fifo")) { app.fifo = true; continue; }
        if (i + 1 >= argc) die("Missing option value (see --help)");
        const char *value = argv[++i];
        if (!strcmp(option, "--iterations")) app.iterations = number(value, false);
        else if (!strcmp(option, "--passes")) app.passes = number(value, false);
        else if (!strcmp(option, "--width")) app.width = number(value, false);
        else if (!strcmp(option, "--height")) app.height = number(value, false);
        else if (!strcmp(option, "--frames")) app.frame_limit = number(value, false);
        else if (!strcmp(option, "--device")) {
            app.device_index = number(value, true); app.explicit_device = true;
        } else if (!strcmp(option, "--hz")) {
            char *end;
            errno = 0;
            app.hz = strtod(value, &end);
            if (errno || end == value || *end || !isfinite(app.hz) || app.hz <= 0)
                die("--hz must be a finite positive number");
        } else die("Unknown option (see --help)");
    }
    if (app.width > INT32_MAX || app.height > INT32_MAX)
        die("Window dimensions must fit in a signed 32-bit integer");
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    fprintf(stderr, "%u iterations/pixel, %u passes/frame; target interval %.2f ms\n",
            app.iterations, app.passes, 1000.0 / app.hz);
    init_wayland();
    if (!running) return 0;
    init_vulkan();
    create_swapchain();
    render_loop();
    cleanup();
    return 0;
}
