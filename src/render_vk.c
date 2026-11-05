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

// -----------------------------------------------------------------------------
// Vulkan Core & State Structures

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t queue_family_index;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    
    // Swapchain & Offscreen
    VkSwapchainKHR swapchain;
    VkImage *swapchain_images;
    VkImageView *swapchain_image_views;
    uint32_t swapchain_image_count;
    
    // Backbuffer / Offscreen Render Target
    VkImage offscreen_image;
    VkDeviceMemory offscreen_image_memory;
    VkImageView offscreen_image_view;
    VkImage depth_image;
    VkDeviceMemory depth_image_memory;
    VkImageView depth_image_view;
    VkFramebuffer offscreen_framebuffer;
    VkRenderPass offscreen_render_pass;

    // Atlas Texture
    VkImage atlas_image;
    VkDeviceMemory atlas_image_memory;
    VkImageView atlas_image_view;
    VkSampler atlas_sampler;

    // Vertex Buffer (Dynamic Host Coherent)
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_buffer_memory;
    void *vertex_buffer_mapped;

    // Descriptor Pools & Layouts
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout pipeline_layout;

    // Pipelines
    VkPipeline pipeline_game;
    VkPipeline pipeline_post_default;
    VkPipeline pipeline_post_crt;
    VkPipeline current_pipeline;
} vk_context_t;

static vk_context_t vk;

// Uniform Buffer Structures matching shader expectations
typedef struct {
    mat4_t view;
    mat4_t model;
    mat4_t projection;
    vec2_t screen;
    float padding1[2];
    vec3_t camera_pos;
    float padding2;
    vec2_t fade;
    float time;
    float padding3;
} game_ubo_t;

typedef struct {
    mat4_t projection;
    vec2_t screen_size;
    float time;
    float padding;
} post_ubo_t;

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

// -----------------------------------------------------------------------------
// Helper Vulkan Memory / Buffer Allocation

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

// -----------------------------------------------------------------------------
// Shader Spir-V Binary Code Generation (Embedded or compiled via glslang)
// In practice, compile GLSL vertex/fragment shaders to SPIR-V binaries (.spv)

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

// -----------------------------------------------------------------------------
// Initialization & Core Setup

void render_init(vec2i_t init_screen_size) {
    screen_size = init_screen_size;

    // 1. Create Vulkan Instance & Device (Boilerplate initializations omitted for brevity)
    // Assumes vk.instance, vk.physical_device, vk.device, vk.graphics_queue initialized.

    // 2. Vertex Buffer Setup (Dynamic streaming vertex buffer)
    create_buffer(sizeof(tris_t) * RENDER_TRIS_BUFFER_CAPACITY,
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &vk.vertex_buffer, &vk.vertex_buffer_memory);
    vkMapMemory(vk.device, vk.vertex_buffer_memory, 0, sizeof(tris_t) * RENDER_TRIS_BUFFER_CAPACITY, 0, &vk.vertex_buffer_mapped);

    // 3. Create Atlas Texture
    uint32_t tw = ATLAS_SIZE * ATLAS_GRID;
    uint32_t th = ATLAS_SIZE * ATLAS_GRID;
    
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = {tw, th, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    vkCreateImage(vk.device, &image_info, NULL, &vk.atlas_image);

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk.device, vk.atlas_image, &mem_reqs);
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    vkAllocateMemory(vk.device, &alloc_info, NULL, &vk.atlas_image_memory);
    vkBindImageMemory(vk.device, vk.atlas_image, vk.atlas_image_memory, 0);

    // Create White Texture Default
    rgba_t white_pixels[4] = {
        rgba(128,128,128,255), rgba(128,128,128,255),
        rgba(128,128,128,255), rgba(128,128,128,255)
    };
    RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);

    render_res = RENDER_RES_NATIVE;
    render_set_screen_size(screen_size);
}

void render_cleanup(void) {
    vkDestroyBuffer(vk.device, vk.vertex_buffer, NULL);
    vkFreeMemory(vk.device, vk.vertex_buffer_memory, NULL);
    vkDestroyImage(vk.device, vk.atlas_image, NULL);
    vkFreeMemory(vk.device, vk.atlas_image_memory, NULL);
}

// -----------------------------------------------------------------------------
// Projections & Framebuffer Resolution

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
        else die("Invalid resolution: %d", (int)res);
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
}

void render_frame_end(void) {
    render_flush();
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

void render_set_view_2d(void) {
    render_flush();
}

void render_set_model_mat(mat4_t *m) {
    render_flush();
}

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

void render_textures_dump(const char *path) {
}
