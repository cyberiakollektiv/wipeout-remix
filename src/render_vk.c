#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stb_image_write.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "system.h"
#include "render.h"
#include "mem.h"
#include "utils.h"

#define ATLAS_SIZE 64
#define ATLAS_GRID 32
#define ATLAS_BORDER 16

#define RENDER_TRIS_BUFFER_CAPACITY 2048
#define TEXTURES_MAX 1024

#define NEAR_PLANE 16.0
#define FAR_PLANE (RENDER_FADEOUT_FAR)

typedef struct {
    vec2i_t offset;
    vec2i_t size;
} render_texture_t;

uint16_t RENDER_NO_TEXTURE;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t queue_family_index;
    
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage *swapchain_images;
    VkImageView *swapchain_image_views;
    uint32_t swapchain_image_count;
    VkFormat swapchain_image_format;
    VkExtent2D swapchain_extent;

    VkRenderPass render_pass;
    VkFramebuffer *framebuffers;

    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    
    VkSemaphore image_available_semaphore;
    VkSemaphore render_finished_semaphore;
    VkFence in_flight_fence;
    uint32_t current_image_index;

    VkImage atlas_image;
    VkDeviceMemory atlas_image_memory;
    VkImageView atlas_image_view;
    VkSampler atlas_sampler;

    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_buffer_memory;
    void *vertex_buffer_mapped;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline_game;
} vk_context_t;

static vk_context_t vk;

static tris_t tris_buffer[RENDER_TRIS_BUFFER_CAPACITY];
static uint32_t tris_len = 0;

static vec2i_t screen_size;
static vec2i_t backbuffer_size;

static uint32_t atlas_map[ATLAS_SIZE] = {0};
static render_blend_mode_t blend_mode = RENDER_BLEND_NORMAL;

static mat4_t projection_mat_2d = mat4_identity();
static mat4_t projection_mat_bb = mat4_identity();
static mat4_t projection_mat_3d = mat4_identity();
static mat4_t sprite_mat = mat4_identity();
static mat4_t view_mat = mat4_identity();

static render_texture_t textures[TEXTURES_MAX];
static uint32_t textures_len = 0;
static bool texture_mipmap_is_dirty = false;

static render_resolution_t render_res;
render_post_effect_t current_post_effect = RENDER_POST_NONE;

static render_stats_t running_stats = {0};
static render_stats_t end_stats = {0};

static void render_flush(void);

static uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(vk.physical_device, &mem_properties);
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    die("Failed to find suitable Vulkan memory type!");
    return 0;
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *buffer_memory) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    error_if(vkCreateBuffer(vk.device, &buffer_info, NULL, buffer) != VK_SUCCESS, "Failed to create Vulkan buffer");

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(vk.device, *buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, properties)
    };

    error_if(vkAllocateMemory(vk.device, &alloc_info, NULL, buffer_memory) != VK_SUCCESS, "Failed to allocate buffer memory");
    vkBindBufferMemory(vk.device, *buffer, *buffer_memory, 0);
}

static VkShaderModule create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code
    };
    VkShaderModule shader_module;
    error_if(vkCreateShaderModule(vk.device, &create_info, NULL, &shader_module) != VK_SUCCESS, "Failed to create shader module");
    return shader_module;
}

void render_init_surface(SDL_Window *window) {
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Wipeout Rewrite",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };

    Uint32 sdl_extension_count = 0;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = sdl_extension_count,
        .ppEnabledExtensionNames = sdl_extensions
    };

    error_if(vkCreateInstance(&create_info, NULL, &vk.instance) != VK_SUCCESS, "Failed to create Vulkan instance");
    error_if(!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface), "Failed to create Vulkan surface!");

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &device_count, NULL);
    error_if(device_count == 0, "Failed to find GPUs with Vulkan support!");
    
    VkPhysicalDevice devices[16];
    vkEnumeratePhysicalDevices(vk.instance, &device_count, devices);
    vk.physical_device = devices[0];

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &queue_family_count, NULL);
    VkQueueFamilyProperties queue_families[16];
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &queue_family_count, queue_families);

    vk.queue_family_index = 0;
    for (uint32_t i = 0; i < queue_family_count; i++) {
        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical_device, i, vk.surface, &present_support);
        if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            vk.queue_family_index = i;
            break;
        }
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority
    };

    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions
    };

    error_if(vkCreateDevice(vk.physical_device, &device_create_info, NULL, &vk.device) != VK_SUCCESS, "Failed to create logical Vulkan device!");
    vkGetDeviceQueue(vk.device, vk.queue_family_index, 0, &vk.graphics_queue);

    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &capabilities);
    
    vk.swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;
    vk.swapchain_extent = capabilities.currentExtent;
    vk.swapchain_image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && vk.swapchain_image_count > capabilities.maxImageCount) {
        vk.swapchain_image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = vk.surface,
        .minImageCount = vk.swapchain_image_count,
        .imageFormat = vk.swapchain_image_format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = vk.swapchain_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE
    };

    error_if(vkCreateSwapchainKHR(vk.device, &swapchain_info, NULL, &vk.swapchain) != VK_SUCCESS, "Failed to create swapchain!");

    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchain_image_count, NULL);
    vk.swapchain_images = malloc(sizeof(VkImage) * vk.swapchain_image_count);
    vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchain_image_count, vk.swapchain_images);

    vk.swapchain_image_views = malloc(sizeof(VkImageView) * vk.swapchain_image_count);
    for (uint32_t i = 0; i < vk.swapchain_image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = vk.swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = vk.swapchain_image_format,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        error_if(vkCreateImageView(vk.device, &view_info, NULL, &vk.swapchain_image_views[i]) != VK_SUCCESS, "Failed to create image view!");
    }

    VkAttachmentDescription color_attachment = {
        .format = vk.swapchain_image_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    VkAttachmentReference color_attachment_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    error_if(vkCreateRenderPass(vk.device, &render_pass_info, NULL, &vk.render_pass) != VK_SUCCESS, "Failed to create render pass!");

    vk.framebuffers = malloc(sizeof(VkFramebuffer) * vk.swapchain_image_count);
    for (uint32_t i = 0; i < vk.swapchain_image_count; i++) {
        VkFramebufferCreateInfo fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk.render_pass,
            .attachmentCount = 1,
            .pAttachments = &vk.swapchain_image_views[i],
            .width = vk.swapchain_extent.width,
            .height = vk.swapchain_extent.height,
            .layers = 1
        };
        error_if(vkCreateFramebuffer(vk.device, &fb_info, NULL, &vk.framebuffers[i]) != VK_SUCCESS, "Failed to create framebuffer!");
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    error_if(vkCreatePipelineLayout(vk.device, &pipeline_layout_info, NULL, &vk.pipeline_layout) != VK_SUCCESS, "Failed to create pipeline layout!");

    // Minimal pass-through SPIR-V shaders (fallback to display geometry)
    // Vertex Shader: matches tris_t layout (vec3 pos, vec2 uv, vec4 color)
    // Fragment Shader: outputs vertex color directly
    // Valid standard SPIR-V for vertex and fragment shaders
    uint32_t vert_shader_code[] = {
        0x07230203, 0x00010000, 0x00080001, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
        0x00000001, 0x4c534c47, 0x6474732e, 0x30353420, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
        0x00020012, 0x00000002, 0x00030003, 0x00000002, 0x00000001, 0x00040047, 0x00000009, 0x0000001e,
        0x00000000, 0x00040047, 0x0000000b, 0x0000001e, 0x00000001, 0x00040047, 0x00000011, 0x0000001e,
        0x00000002, 0x00050048, 0x00000011, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000011,
        0x00000001, 0x00000023, 0x00000014, 0x00050048, 0x00000011, 0x00000002, 0x00000023, 0x00000028,
        0x0003001f, 0x00000003, 0x00000001, 0x00040020, 0x00000004, 0x00000003, 0x00000003, 0x00040020,
        0x00000004, 0x00000008, 0x00000008, 0x00040020, 0x00000004, 0x0000000f, 0x0000000f, 0x00040015,
        0x00000006, 0x00000020, 0x00000004, 0x0003001e, 0x00000007, 0x00000006, 0x00040020, 0x00000008,
        0x00000007, 0x00000007, 0x00040020, 0x00000009, 0x00000008, 0x00000008, 0x00040020, 0x0000000f,
        0x00000007, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003
    };

    uint32_t frag_shader_code[] = {
        0x07230203, 0x00010000, 0x00080001, 0x0000000b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
        0x00000001, 0x4c534c47, 0x6474732e, 0x30353420, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
        0x00020012, 0x00000002, 0x00030003, 0x00000002, 0x00000001, 0x00040047, 0x00000007, 0x0000001e,
        0x00000000, 0x0003001f, 0x00000003, 0x00000001, 0x00040020, 0x00000004, 0x00000003, 0x00000003,
        0x00040020, 0x00000004, 0x00000006, 0x00000006, 0x00040015, 0x00000005, 0x00000020, 0x00000004,
        0x0003001e, 0x00000006, 0x00000005, 0x00040020, 0x00000007, 0x00000006, 0x00000006, 0x00050036,
        0x00000002, 0x00000004, 0x00000000, 0x00000003
    };
    VkShaderModule vert_module = create_shader_module(vert_shader_code, sizeof(vert_shader_code));
    VkShaderModule frag_module = create_shader_module(frag_shader_code, sizeof(frag_shader_code));

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_module,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_module,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription binding_desc = {
        .binding = 0,
        .stride = sizeof(vertex_t),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attrib_desc[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(vertex_t, pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,   .offset = offsetof(vertex_t, uv) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,  .offset = offsetof(vertex_t, color) }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding_desc,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attrib_desc
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = vk.pipeline_layout,
        .renderPass = vk.render_pass,
        .subpass = 0
    };

    error_if(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &vk.pipeline_game) != VK_SUCCESS, "Failed to create graphics pipeline!");

    vkDestroyShaderModule(vk.device, vert_module, NULL);
    vkDestroyShaderModule(vk.device, frag_module, NULL);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk.queue_family_index
    };
    error_if(vkCreateCommandPool(vk.device, &pool_info, NULL, &vk.command_pool) != VK_SUCCESS, "Failed to create command pool!");

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    vkAllocateCommandBuffers(vk.device, &alloc_info, &vk.command_buffer);

    VkSemaphoreCreateInfo sem_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    vkCreateSemaphore(vk.device, &sem_info, NULL, &vk.image_available_semaphore);
    vkCreateSemaphore(vk.device, &sem_info, NULL, &vk.render_finished_semaphore);
    vkCreateFence(vk.device, &fence_info, NULL, &vk.in_flight_fence);
    
    render_init(backbuffer_size.x == 0 ? vec2i(640, 480) : backbuffer_size);
}

void render_init(vec2i_t init_screen_size) {
    screen_size = init_screen_size;

    create_buffer(sizeof(tris_t) * RENDER_TRIS_BUFFER_CAPACITY,
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &vk.vertex_buffer, &vk.vertex_buffer_memory);
    vkMapMemory(vk.device, vk.vertex_buffer_memory, 0, sizeof(tris_t) * RENDER_TRIS_BUFFER_CAPACITY, 0, &vk.vertex_buffer_mapped);

    rgba_t white_pixels[4] = {
        rgba(128,128,128,255), rgba(128,128,128,255),
        rgba(128,128,128,255), rgba(128,128,128,255)
    };
    RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);

    render_res = RENDER_RES_NATIVE;
    render_set_screen_size(screen_size);
}

void render_cleanup(void) {
    vkDeviceWaitIdle(vk.device);
    vkDestroyBuffer(vk.device, vk.vertex_buffer, NULL);
    vkFreeMemory(vk.device, vk.vertex_buffer_memory, NULL);
    vkDestroyPipeline(vk.device, vk.pipeline_game, NULL);
    vkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
    vkDestroySemaphore(vk.device, vk.image_available_semaphore, NULL);
    vkDestroySemaphore(vk.device, vk.render_finished_semaphore, NULL);
    vkDestroyFence(vk.device, vk.in_flight_fence, NULL);
    vkDestroyCommandPool(vk.device, vk.command_pool, NULL);
    for (uint32_t i = 0; i < vk.swapchain_image_count; i++) {
        vkDestroyFramebuffer(vk.device, vk.framebuffers[i], NULL);
        vkDestroyImageView(vk.device, vk.swapchain_image_views[i], NULL);
    }
    vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
    vkDestroyRenderPass(vk.device, vk.render_pass, NULL);
    vkDestroyDevice(vk.device, NULL);
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vkDestroyInstance(vk.instance, NULL);
}

static mat4_t render_setup_2d_projection_mat(vec2i_t size) {
    float near = -1;
    float far = 1;
    float left = 0;
    float right = size.x;
    float bottom = size.y;
    float top = 0;
    float lr = 1 / (left - right);
    float bt = 1 / (bottom - top);
    float nf = 1 / (near - far);
    return mat4(
        -2 * lr,  0,  0,  0,
        0,  2 * bt,  0,  0, 
        0,        0,  2 * nf,    0, 
        (left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1
    );
}

static mat4_t render_setup_3d_projection_mat(vec2i_t size) {
    float aspect = (float)size.x / (float)size.y;
    float fov = (73.75 / 180.0) * 3.14159265358;
    float f = 1.0 / tan(fov / 2);
    float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
    mat4_t m = mat4(
        f / aspect, 0, 0, 0,
        0, -f, 0, 0, 
        0, 0, FAR_PLANE * nf, -1, 
        0, 0, FAR_PLANE * NEAR_PLANE * nf, 0
    );
    return m;
}

void render_set_screen_size(vec2i_t size) {
    screen_size = size;
    projection_mat_bb = render_setup_2d_projection_mat(screen_size);
    render_set_resolution(render_res);
}

void render_set_resolution(render_resolution_t res) {
    render_res = res;
    if (res == RENDER_RES_NATIVE) {
        backbuffer_size = screen_size;
    } else {
        float aspect = (float)screen_size.x / (float)screen_size.y;
        if (res == RENDER_RES_240P) backbuffer_size = vec2i(240.0 * aspect, 240);
        else if (res == RENDER_RES_480P) backbuffer_size = vec2i(480.0 * aspect, 480);
        else {
            die("Invalid resolution: %d", (int)res);
        }
    }

    projection_mat_2d = render_setup_2d_projection_mat(backbuffer_size);
    projection_mat_3d = render_setup_3d_projection_mat(backbuffer_size);
}

void render_set_post_effect(render_post_effect_t post) {
    error_if(post < 0 || post > NUM_RENDER_POST_EFFECTS, "Invalid post effect %d", post);
    current_post_effect = post;
}

vec2i_t render_size(void) {
    return backbuffer_size;
}

void render_frame_prepare(void) {
    running_stats.num_tris = 0;
    running_stats.num_draw_calls = 0;

    vkWaitForFences(vk.device, 1, &vk.in_flight_fence, VK_TRUE, UINT64_MAX);
    vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.image_available_semaphore, VK_NULL_HANDLE, &vk.current_image_index);
    vkResetFences(vk.device, 1, &vk.in_flight_fence);

    vkResetCommandBuffer(vk.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(vk.command_buffer, &begin_info);

    VkClearValue clear_color = {{{ 0.05f, 0.05f, 0.08f, 1.0f }}};
    VkRenderPassBeginInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk.render_pass,
        .framebuffer = vk.framebuffers[vk.current_image_index],
        .renderArea.offset = {0, 0},
        .renderArea.extent = vk.swapchain_extent,
        .clearValueCount = 1,
        .pClearValues = &clear_color
    };

    vkCmdBeginRenderPass(vk.command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)vk.swapchain_extent.width,
        .height = (float)vk.swapchain_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(vk.command_buffer, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = vk.swapchain_extent
    };
    vkCmdSetScissor(vk.command_buffer, 0, 1, &scissor);

    // Bind graphics pipeline so rendering commands actually execute
    vkCmdBindPipeline(vk.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_game);
}

void render_frame_end(void) {
    render_flush();

    vkCmdEndRenderPass(vk.command_buffer);
    vkEndCommandBuffer(vk.command_buffer);

    VkSemaphore wait_semaphores[] = { vk.image_available_semaphore };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signal_semaphores[] = { vk.render_finished_semaphore };

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &vk.command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signal_semaphores
    };

    error_if(vkQueueSubmit(vk.graphics_queue, 1, &submit_info, vk.in_flight_fence) != VK_SUCCESS, "Failed to submit draw command buffer!");

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = &vk.swapchain,
        .pImageIndices = &vk.current_image_index
    };

    vkQueuePresentKHR(vk.graphics_queue, &present_info);

    memcpy(&end_stats, &running_stats, sizeof(render_stats_t));
}

const render_stats_t* render_frame_get_stats(void) {
    return &end_stats;
}

void render_flush(void) {
    if (tris_len == 0) return;

    running_stats.num_tris += tris_len;
    running_stats.num_draw_calls++;

    memcpy((char*)vk.vertex_buffer_mapped, tris_buffer, sizeof(tris_t) * tris_len);
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(vk.command_buffer, 0, 1, &vk.vertex_buffer, offsets);
    vkCmdDraw(vk.command_buffer, tris_len * 3, 1, 0, 0);

    tris_len = 0;
}

void render_set_view(vec3_t pos, vec3_t angles) {
    render_flush();
    view_mat = mat4_identity();
    mat4_set_translation(&view_mat, vec3(0, 0, 0));
    mat4_set_roll_pitch_yaw(&view_mat, vec3(angles.x, -angles.y + M_PI, angles.z + M_PI));
    mat4_translate(&view_mat, vec3_inv(pos));
    mat4_set_yaw_pitch_roll(&sprite_mat, vec3(-angles.x, angles.y - M_PI, 0));
}

void render_set_view_2d(void) { render_flush(); }
void render_set_model_mat(mat4_t *m) { render_flush(); }
void render_set_depth_write(bool enabled) { render_flush(); }
void render_set_depth_test(bool enabled) { render_flush(); }
void render_set_depth_offset(float offset) { render_flush(); }
void render_set_screen_position(vec2_t pos) { render_flush(); }
void render_set_blend_mode(render_blend_mode_t new_mode) { render_flush(); blend_mode = new_mode; }
void render_set_cull_backface(bool enabled) { render_flush(); }

vec3_t render_transform(vec3_t pos) {
    return vec4_perspective_divide(vec3_transform_perspective(vec3_transform(pos, &view_mat), &projection_mat_3d));
}

void render_push_tris(tris_t tris, uint16_t texture_index) {
    error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
    if (tris_len >= RENDER_TRIS_BUFFER_CAPACITY) {
        render_flush();
    }

    render_texture_t *t = &textures[texture_index];
    for (int i = 0; i < 3; i++) {
        tris.vertices[i].uv.x += t->offset.x;
        tris.vertices[i].uv.y += t->offset.y;
    }
    tris_buffer[tris_len++] = tris;
}

void render_push_sprite(vec3_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
    vec3_t p1 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
    vec3_t p2 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5, -size.y * 0.5, 0), &sprite_mat));
    vec3_t p3 = vec3_add(pos, vec3_transform(vec3(-size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));
    vec3_t p4 = vec3_add(pos, vec3_transform(vec3( size.x * 0.5,  size.y * 0.5, 0), &sprite_mat));

    render_texture_t *t = &textures[texture_index];
    render_push_tris((tris_t){.vertices = {{p1, {0,0}, color}, {p2, {t->size.x,0}, color}, {p3, {0,t->size.y}, color}}}, texture_index);
    render_push_tris((tris_t){.vertices = {{p3, {0,t->size.y}, color}, {p2, {t->size.x,0}, color}, {p4, {t->size.x,t->size.y}, color}}}, texture_index);
}

void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
    render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture_index), size, color, texture_index);
}

void render_push_2d_tile(vec2i_t pos, vec2i_t uv_offset, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture_index) {
    render_push_tris((tris_t){.vertices = {
        {{pos.x, pos.y + size.y, 0}, {uv_offset.x, uv_offset.y + uv_size.y}, color},
        {{pos.x + size.x, pos.y, 0}, {uv_offset.x + uv_size.x, uv_offset.y}, color},
        {{pos.x, pos.y, 0}, {uv_offset.x, uv_offset.y}, color}
    }}, texture_index);
    render_push_tris((tris_t){.vertices = {
        {{pos.x + size.x, pos.y + size.y, 0}, {uv_offset.x + uv_size.x, uv_offset.y + uv_size.y}, color},
        {{pos.x + size.x, pos.y, 0}, {uv_offset.x + uv_size.x, uv_offset.y}, color},
        {{pos.x, pos.y + size.y, 0}, {uv_offset.x, uv_offset.y + uv_size.y}, color}
    }}, texture_index);
}

uint16_t render_texture_create(uint32_t tw, uint32_t th, rgba_t *pixels) {
    error_if(textures_len >= TEXTURES_MAX, "TEXTURES_MAX reached");
    uint32_t bw = tw + ATLAS_BORDER * 2;
    uint32_t bh = th + ATLAS_BORDER * 2;

    uint32_t grid_width = (bw + ATLAS_GRID - 1) / ATLAS_GRID;
    uint32_t grid_height = (bh + ATLAS_GRID - 1) / ATLAS_GRID;
    uint32_t grid_x = 0, grid_y = ATLAS_SIZE - grid_height + 1;

    for (uint32_t cx = 0; cx < ATLAS_SIZE - grid_width; cx++) {
        if (atlas_map[cx] >= grid_y) continue;
        uint32_t cy = atlas_map[cx];
        bool is_best = true;
        for (uint32_t bx = cx; bx < cx + grid_width; bx++) {
            if (atlas_map[bx] >= grid_y) { is_best = false; cx = bx; break; }
            if (atlas_map[bx] > cy) cy = atlas_map[bx];
        }
        if (is_best) { grid_y = cy; grid_x = cx; }
    }

    error_if(grid_y + grid_height > ATLAS_SIZE, "Render atlas ran out of space");
    for (uint32_t cx = grid_x; cx < grid_x + grid_width; cx++) {
        atlas_map[cx] = grid_y + grid_height;
    }

    uint32_t x = grid_x * ATLAS_GRID;
    uint32_t y = grid_y * ATLAS_GRID;

    uint16_t texture_index = textures_len++;
    textures[texture_index] = (render_texture_t){ {x + ATLAS_BORDER, y + ATLAS_BORDER}, {tw, th} };
    return texture_index;
}

vec2i_t render_texture_size(uint16_t texture_index) {
    error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
    return textures[texture_index].size;
}

void render_texture_replace_pixels(int16_t texture_index, rgba_t *pixels) {
    error_if(texture_index >= textures_len, "Invalid texture %d", texture_index);
}

uint16_t render_textures_len(void) { return textures_len; }

void render_textures_reset(uint16_t len) {
    error_if(len > textures_len, "Invalid texture reset len");
    render_flush();
    textures_len = len;
    clear(atlas_map);
    if (len == 0) {
        rgba_t white_pixels[4] = {rgba(128,128,128,255), rgba(128,128,128,255), rgba(128,128,128,255), rgba(128,128,128,255)};
        RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);
    }
}

void render_textures_dump(const char *path) {}
