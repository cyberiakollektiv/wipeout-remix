#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include <stb_image_write.h>

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
// Metal Shading Language (MSL) Shaders

static const char * const METAL_SHADER_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
	float3 pos      [[attribute(0)]];
	float2 uv       [[attribute(1)]];
	uchar4 color    [[attribute(2)]];
};

struct GameVertexOutput {
	float4 position [[position]];
	float4 color;
	float2 uv;
};

struct UniformsGame {
	float4x4 view;
	float4x4 model;
	float4x4 projection;
	float2 screen;
	float3 camera_pos;
	float2 fade;
	float time;
};

vertex GameVertexOutput game_vertex(VertexInput in [[stage_in]],
                                    constant UniformsGame &uni [[buffer(1)]]) {
	GameVertexOutput out;
	float4 world_pos = uni.model * float4(in.pos, 1.0);
	out.position = uni.projection * uni.view * world_pos;
	out.position.xy += uni.screen * out.position.w;
	
	float4 c_float = float4(in.color) / 255.0;
	float dist = length(float4(uni.camera_pos, 1.0) - world_pos);
	float alpha_fade = smoothstep(uni.fade.y, uni.fade.x, dist);
	out.color = float4(c_float.rgb, c_float.a * alpha_fade);
	out.uv = in.uv / 2048.0; // ATLAS_GRID * ATLAS_SIZE
	return out;
}

fragment float4 game_fragment(GameVertexOutput in [[stage_in]],
                              texture2d<float> colorTexture [[texture(0)]],
                              sampler textureSampler [[sampler(0)]]) {
	float4 tex_color = colorTexture.sample(textureSampler, in.uv);
	float4 color = tex_color * in.color;
	if (color.a == 0.0) {
		discard_fragment();
	}
	color.rgb = color.rgb * 2.0;
	return color;
}

// -----------------------------------------------------------------------------
// Post-Processing Shaders

struct PostVertexInput {
	float3 pos [[attribute(0)]];
	float2 uv  [[attribute(1)]];
};

struct PostVertexOutput {
	float4 position [[position]];
	float2 uv;
};

struct UniformsPost {
	float4x4 projection;
	float2 screen_size;
	float time;
};

vertex PostVertexOutput post_vertex(PostVertexInput in [[stage_in]],
                                    constant UniformsPost &uni [[buffer(1)]]) {
	PostVertexOutput out;
	out.position = uni.projection * float4(in.pos, 1.0);
	out.uv = in.uv;
	return out;
}

fragment float4 post_fragment_default(PostVertexOutput in [[stage_in]],
                                      texture2d<float> colorTexture [[texture(0)]],
                                      sampler textureSampler [[sampler(0)]]) {
	return colorTexture.sample(textureSampler, in.uv);
}

// CRT effect translated from shader_gl.c
fragment float4 post_fragment_crt(PostVertexOutput in [[stage_in]],
                                    constant UniformsPost &uni [[buffer(1)]],
                                    texture2d<float> colorTexture [[texture(0)]],
                                    sampler textureSampler [[sampler(0)]],
                                    float4 fragCoord [[position]]) {
	vec2 uv = fragCoord.xy / uni.screen_size;
	
	// curve function
	vec2 cuv = (uv - 0.5) * 2.0;
	cuv *= 1.1;
	cuv.x *= 1.0 + pow((abs(cuv.y) / 5.0), 2.0);
	cuv.y *= 1.0 + pow((abs(cuv.x) / 4.0), 2.0);
	cuv = (cuv / 2.0) + 0.5;
	cuv = cuv * 0.92 + 0.04;

	float x = sin(0.3 * uni.time + cuv.y * 21.0) * sin(0.7 * uni.time + cuv.y * 29.0) * sin(0.3 + 0.33 * uni.time + cuv.y * 31.0) * 0.0017;

	vec3 color;
	color.r = colorTexture.sample(textureSampler, vec2(x + cuv.x + 0.001, cuv.y + 0.001)).r + 0.05;
	color.g = colorTexture.sample(textureSampler, vec2(x + cuv.x + 0.000, cuv.y - 0.002)).g + 0.05;
	color.b = colorTexture.sample(textureSampler, vec2(x + cuv.x - 0.002, cuv.y + 0.000)).b + 0.05;
	
	color.r += 0.08 * colorTexture.sample(textureSampler, 0.75 * vec2(x + 0.025, -0.027) + vec2(cuv.x + 0.001, cuv.y + 0.001)).r;
	color.g += 0.05 * colorTexture.sample(textureSampler, 0.75 * vec2(x - 0.022, -0.02) + vec2(cuv.x + 0.000, cuv.y - 0.002)).g;
	color.b += 0.08 * colorTexture.sample(textureSampler, 0.75 * vec2(x - 0.02, -0.018) + vec2(cuv.x - 0.002, cuv.y + 0.000)).b;

	color = clamp(color * 0.6 + 0.4 * color * color * 1.0, 0.0, 1.0);

	float vignette = (0.0 + 1.0 * 16.0 * cuv.x * cuv.y * (1.0 - cuv.x) * (1.0 - cuv.y));
	color *= vec3(pow(vignette, 0.25));

	color *= vec3(0.95, 1.05, 0.95);
	color *= 2.8;

	float scanlines = clamp(0.35 + 0.35 * sin(3.5 * uni.time + cuv.y * uni.screen_size.y * 1.5), 0.0, 1.0);
	float s = pow(scanlines, 1.7);
	color = color * vec3(0.4 + 0.7 * s);

	color *= 1.0 + 0.01 * sin(110.0 * uni.time);
	if (cuv.x < 0.0 || cuv.x > 1.0) color *= 0.0;
	if (cuv.y < 0.0 || cuv.y > 1.0) color *= 0.0;

	color *= 1.0 - 0.65 * vec3(clamp((mod(fragCoord.x, 2.0) - 1.0) * 2.0, 0.0, 1.0));
	return float4(color, 1.0);
}
)";

// -----------------------------------------------------------------------------
// Renderer State Variables

static id<MTLDevice> mtl_device = nil;
static id<MTLCommandQueue> mtl_command_queue = nil;
static id<MTLLibrary> mtl_library = nil;

static id<MTLRenderPipelineState> pipeline_game = nil;
static id<MTLRenderPipelineState> pipeline_post_default = nil;
static id<MTLRenderPipelineState> pipeline_post_crt = nil;
static id<MTLRenderPipelineState> active_post_pipeline = nil;

static id<MTLTexture> atlas_texture = nil;
static id<MTLTexture> backbuffer_texture = nil;
static id<MTLTexture> depth_texture = nil;

static id<MTLBuffer> vertex_buffer = nil;

static uint32_t atlas_map[ATLAS_SIZE] = {0};
static render_blend_mode_t blend_mode = RENDER_BLEND_NORMAL;

static vec2i_t screen_size;
static vec2i_t backbuffer_size;

static mat4_t projection_mat_2d = mat4_identity();
static mat4_t projection_mat_bb = mat4_identity();
static mat4_t projection_mat_3d = mat4_identity();
static mat4_t sprite_mat = mat4_identity();
static mat4_t view_mat = mat4_identity();

static render_texture_t textures[TEXTURES_MAX];
static uint32_t textures_len = 0;
static bool texture_mipmap_is_dirty = false;
static render_resolution_t render_res;

static tris_t tris_buffer[RENDER_TRIS_BUFFER_CAPACITY];
static uint32_t tris_len = 0;

static render_stats_t running_stats = {0};
static render_stats_t end_stats = {0};

// Uniform structures matching MSL
typedef struct {
	matrix_float4x4 view;
	matrix_float4x4 model;
	matrix_float4x4 projection;
	simd_float2 screen;
	simd_float3 camera_pos;
	simd_float2 fade;
	float time;
} UniformsGame;

typedef struct {
	matrix_float4x4 projection;
	simd_float2 screen_size;
	float time;
} UniformsPost;

static UniformsGame current_game_uniforms;
static UniformsPost current_post_uniforms;

static void render_flush(void);

// Helper conversions for mat4_t to SIMD matrices
static inline matrix_float4x4 mat4_to_metal(mat4_t m) {
	matrix_float4x4 out;
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			out.columns[i][j] = m.m[i * 4 + j];
		}
	}
	return out;
}

void render_init(vec2i_t initial_screen_size) {
	mtl_device = MTLCreateSystemDefaultDevice();
	error_if(!mtl_device, "Failed to find Metal system default device");

	mtl_command_queue = [mtl_device newCommandQueue];
	
	NSError *error = nil;
	MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
	mtl_library = [mtl_device newLibraryWithSource:[NSString stringWithUTF8String:METAL_SHADER_SOURCE]
	                                       options:options
	                                         error:&error];
	if (!mtl_library) {
		die("Failed to compile Metal shaders: %s", [[error localizedDescription] UTF8String]);
	}

	// Create Pipeline State for Game
	MTLRenderPipelineDescriptor *gameDesc = [[MTLRenderPipelineDescriptor alloc] init];
	gameDesc.vertexFunction = [mtl_library newFunctionWithName:@"game_vertex"];
	gameDesc.fragmentFunction = [mtl_library newFunctionWithName:@"game_fragment"];
	gameDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	gameDesc.colorAttachments[0].blendingEnabled = YES;
	gameDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	gameDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	gameDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
	gameDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	gameDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

	// Setup vertex descriptor matching vertex_t layout
	MTLVertexDescriptor *vertexDesc = [MTLVertexDescriptor vertexDescriptor];
	vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
	vertexDesc.attributes[0].offset = offsetof(vertex_t, pos);
	vertexDesc.attributes[0].bufferIndex = 0;

	vertexDesc.attributes[1].format = MTLVertexFormatFloat2;
	vertexDesc.attributes[1].offset = offsetof(vertex_t, uv);
	vertexDesc.attributes[1].bufferIndex = 0;

	vertexDesc.attributes[2].format = MTLVertexFormatUChar4;
	vertexDesc.attributes[2].offset = offsetof(vertex_t, color);
	vertexDesc.attributes[2].bufferIndex = 0;

	vertexDesc.layouts[0].stride = sizeof(vertex_t);
	vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

	gameDesc.vertexDescriptor = vertexDesc;
	pipeline_game = [mtl_device newRenderPipelineStateWithDescriptor:gameDesc error:&error];
	error_if(!pipeline_game, "Failed to create game pipeline state");

	// Create Pipeline States for Post-processing
	MTLRenderPipelineDescriptor *postDesc = [[MTLRenderPipelineDescriptor alloc] init];
	postDesc.vertexFunction = [mtl_library newFunctionWithName:@"post_vertex"];
	postDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	
	MTLVertexDescriptor *postVertexDesc = [MTLVertexDescriptor vertexDescriptor];
	postVertexDesc.attributes[0].format = MTLVertexFormatFloat3;
	postVertexDesc.attributes[0].offset = offsetof(vertex_t, pos);
	postVertexDesc.attributes[0].bufferIndex = 0;
	postVertexDesc.attributes[1].format = MTLVertexFormatFloat2;
	postVertexDesc.attributes[1].offset = offsetof(vertex_t, uv);
	postVertexDesc.attributes[1].bufferIndex = 0;
	postVertexDesc.layouts[0].stride = sizeof(vertex_t);
	postVertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
	postDesc.vertexDescriptor = postVertexDesc;

	postDesc.fragmentFunction = [mtl_library newFunctionWithName:@"post_fragment_default"];
	pipeline_post_default = [mtl_device newRenderPipelineStateWithDescriptor:postDesc error:&error];

	postDesc.fragmentFunction = [mtl_library newFunctionWithName:@"post_fragment_crt"];
	pipeline_post_crt = [mtl_device newRenderPipelineStateWithDescriptor:postDesc error:&error];

	active_post_pipeline = pipeline_post_default;

	// Initialize Dynamic Vertex Buffer
	vertex_buffer = [mtl_device newBufferWithLength:sizeof(tris_t) * RENDER_TRIS_BUFFER_CAPACITY
	                                        options:MTLResourceStorageModeShared];

	// Initialize Atlas Texture
	MTLTextureDescriptor *atlasDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                                    width:ATLAS_SIZE * ATLAS_GRID
	                                                                                   height:ATLAS_SIZE * ATLAS_GRID
	                                                                                mipmapped:RENDER_USE_MIPMAPS];
	atlas_texture = [mtl_device newTextureWithDescriptor:atlasDesc];

	// White fallback texture
	rgba_t white_pixels[4] = {
		rgba(128,128,128,255), rgba(128,128,128,255),
		rgba(128,128,128,255), rgba(128,128,128,255)
	};
	RENDER_NO_TEXTURE = render_texture_create(2, 2, white_pixels);

	render_res = RENDER_RES_NATIVE;
	render_set_screen_size(initial_screen_size);
}

void render_cleanup(void) {
	// Metal resource ARC cleanup handled automatically
}

static mat4_t render_setup_2d_projection_mat(vec2i_t size) {
	float near = -1, far = 1, left = 0, right = size.x, bottom = size.y, top = 0;
	float lr = 1 / (left - right), bt = 1 / (bottom - top), nf = 1 / (near - far);
	return mat4(-2 * lr, 0, 0, 0, 0, -2 * bt, 0, 0, 0, 0, 2 * nf, 0, 
	            (left + right) * lr, (top + bottom) * bt, (far + near) * nf, 1);
}

static mat4_t render_setup_3d_projection_mat(vec2i_t size) {
	float aspect = (float)size.x / (float)size.y;
	float fov = (73.75 / 180.0) * 3.14159265358;
	float f = 1.0 / tan(fov / 2);
	float nf = 1.0 / (NEAR_PLANE - FAR_PLANE);
	return mat4(f / aspect, 0, 0, 0, 0, f, 0, 0, 
	            0, 0, (FAR_PLANE + NEAR_PLANE) * nf, -1, 
	            0, 0, 2 * FAR_PLANE * NEAR_PLANE * nf, 0);
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
		else die("Invalid resolution: %d", res);
	}

	MTLTextureDescriptor *bbDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                 width:backbuffer_size.x
	                                                                                height:backbuffer_size.y
	                                                                             mipmapped:NO];
	bbDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	backbuffer_texture = [mtl_device newTextureWithDescriptor:bbDesc];

	MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
	                                                                                   width:backbuffer_size.x
	                                                                                  height:backbuffer_size.y
	                                                                               mipmapped:NO];
	depthDesc.usage = MTLTextureUsageRenderTarget;
	depthDesc.storageMode = MTLStorageModePrivate;
	depth_texture = [mtl_device newTextureWithDescriptor:depthDesc];

	projection_mat_2d = render_setup_2d_projection_mat(backbuffer_size);
	projection_mat_3d = render_setup_3d_projection_mat(backbuffer_size);
}

void render_set_post_effect(render_post_effect_t post) {
	error_if(post < 0 || post > NUM_RENDER_POST_EFFECTS, "Invalid post effect %d", post);
	if (post == RENDER_POST_CRT) {
		active_post_pipeline = pipeline_post_crt;
	} else {
		active_post_pipeline = pipeline_post_default;
	}
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

	// Copy data to vertex buffer
	memcpy([vertex_buffer contents], tris_buffer, sizeof(tris_t) * tris_len);

	id<MTLCommandBuffer> cmd_buffer = [mtl_command_queue commandBuffer];
	
	MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
	passDesc.colorAttachments[0].texture = backbuffer_texture;
	passDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
	passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
	passDesc.depthAttachment.texture = depth_texture;
	passDesc.depthAttachment.loadAction = MTLLoadActionLoad;
	passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;

	id<MTLRenderCommandEncoder> encoder = [cmd_buffer renderCommandEncoderWithDescriptor:passDesc];
	[encoder setRenderPipelineState:pipeline_game];
	[encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
	[encoder setVertexBytes:&current_game_uniforms length:sizeof(UniformsGame) atIndex:1];
	[encoder setFragmentTexture:atlas_texture atIndex:0];

	MTLSamplerDescriptor *samplerDesc = [MTLSamplerDescriptor new];
	samplerDesc.minFilter = MTLSamplerMinMagFilterNearest;
	samplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
	id<MTLSamplerState> samplerState = [mtl_device newSamplerStateWithDescriptor:samplerDesc];
	[encoder setFragmentSamplerState:samplerState atIndex:0];

	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:tris_len * 3];
	[encoder endEncoding];
	[cmd_buffer commit];
	[cmd_buffer waitUntilCompleted];

	tris_len = 0;
}

void render_set_view(vec3_t pos, vec3_t angles) {
	render_flush();
	view_mat = mat4_identity();
	mat4_set_translation(&view_mat, vec3(0, 0, 0));
	mat4_set_roll_pitch_yaw(&view_mat, vec3(angles.x, -angles.y + M_PI, angles.z + M_PI));
	mat4_translate(&view_mat, vec3_inv(pos));
	mat4_set_yaw_pitch_roll(&sprite_mat, vec3(-angles.x, angles.y - M_PI, 0));

	current_game_uniforms.view = mat4_to_metal(view_mat);
	current_game_uniforms.projection = mat4_to_metal(projection_mat_3d);
	current_game_uniforms.camera_pos = (simd_float3){pos.x, pos.y, pos.z};
	current_game_uniforms.fade = (simd_float2){RENDER_FADEOUT_NEAR, RENDER_FADEOUT_FAR};
}

void render_set_view_2d(void) {
	render_flush();
	current_game_uniforms.camera_pos = (simd_float3){0, 0, 0};
	current_game_uniforms.view = mat4_to_metal(mat4_identity());
	current_game_uniforms.projection = mat4_to_metal(projection_mat_2d);
}

void render_set_model_mat(mat4_t *m) {
	render_flush();
	current_game_uniforms.model = mat4_to_metal(*m);
}

void render_set_depth_write(bool enabled) { render_flush(); }
void render_set_depth_test(bool enabled) { render_flush(); }
void render_set_depth_offset(float offset) { render_flush(); }

void render_set_screen_position(vec2_t pos) {
	render_flush();
	current_game_uniforms.screen = (simd_float2){pos.x, -pos.y};
}

void render_set_blend_mode(render_blend_mode_t new_mode) { blend_mode = new_mode; }
void render_set_cull_backface(bool enabled) {}

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
	render_push_tris((tris_t){.vertices = {{.pos = p1, .uv = {0, 0}, .color = color},
	                                      {.pos = p2, .uv = {(float)t->size.x, 0}, .color = color},
	                                      {.pos = p3, .uv = {0, (float)t->size.y}, .color = color}}}, texture_index);
	render_push_tris((tris_t){.vertices = {{.pos = p3, .uv = {0, (float)t->size.y}, .color = color},
	                                      {.pos = p2, .uv = {(float)t->size.x, 0}, .color = color},
	                                      {.pos = p4, .uv = {(float)t->size.x, (float)t->size.y}, .color = color}}}, texture_index);
}

void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture_index) {
	render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture_index), size, color, texture_index);
}

void render_push_2d_tile(vec2i_t pos, vec2i_t uv_offset, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture_index) {
	render_push_tris((tris_t){.vertices = {
		{.pos = {pos.x, pos.y + size.y, 0}, .uv = {(float)uv_offset.x, (float)(uv_offset.y + uv_size.y)}, .color = color},
		{.pos = {pos.x + size.x, pos.y, 0}, .uv = {(float)(uv_offset.x + uv_size.x), (float)uv_offset.y}, .color = color},
		{.pos = {pos.x, pos.y, 0}, .uv = {(float)uv_offset.x, (float)uv_offset.y}, .color = color},
	}}, texture_index);
	render_push_tris((tris_t){.vertices = {
		{.pos = {pos.x + size.x, pos.y + size.y, 0}, .uv = {(float)(uv_offset.x + uv_size.x), (float)(uv_offset.y + uv_size.y)}, .color = color},
		{.pos = {pos.x + size.x, pos.y, 0}, .uv = {(float)(uv_offset.x + uv_size.x), (float)uv_offset.y}, .color = color},
		{.pos = {pos.x, pos.y + size.y, 0}, .uv = {(float)uv_offset.x, (float)(uv_offset.y + uv_size.y)}, .color = color},
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
	for (uint32_t cx = grid_x; cx < grid_x + grid_width; cx++) atlas_map[cx] = grid_y + grid_height;

	rgba_t *pb = mem_temp_alloc(sizeof(rgba_t) * bw * bh);
	if (tw && th) {
		for (int32_t y = 0; y < ATLAS_BORDER; y++) memcpy(pb + bw * y + ATLAS_BORDER, pixels, tw * sizeof(rgba_t));
		for (int32_t y = 0; y < ATLAS_BORDER; y++) memcpy(pb + bw * (bh - ATLAS_BORDER + y) + ATLAS_BORDER, pixels + tw * (th-1), tw * sizeof(rgba_t));
		for (int32_t y = 0; y < bh; y++) {
			for (int32_t x = 0; x < ATLAS_BORDER; x++) pb[y * bw + x] = pixels[clamp(y-ATLAS_BORDER, 0, th-1) * tw];
			for (int32_t x = 0; x < ATLAS_BORDER; x++) pb[y * bw + x + bw - ATLAS_BORDER] = pixels[tw - 1 + clamp(y-ATLAS_BORDER, 0, th-1) * tw];
		}
		for (int32_t y = 0; y < th; y++) memcpy(pb + bw * (y + ATLAS_BORDER) + ATLAS_BORDER, pixels + tw * y, tw * sizeof(rgba_t));
	}

	uint32_t x = grid_x * ATLAS_GRID;
	uint32_t y = grid_y * ATLAS_GRID;
	[atlas_texture replaceRegion:MTLRegionMake2D(x, y, bw, bh) mipmapLevel:0 withBytes:pb bytesPerRow:bw * sizeof(rgba_t)];
	mem_temp_free(pb);

	uint16_t idx = textures_len++;
	textures[idx] = (render_texture_t){ {x + ATLAS_BORDER, y + ATLAS_BORDER}, {tw, th} };
	return idx;
}

vec2i_t render_texture_size(uint16_t texture_index) { return textures[texture_index].size; }

void render_texture_replace_pixels(int16_t texture_index, rgba_t *pixels) {
	render_texture_t *t = &textures[texture_index];
	[atlas_texture replaceRegion:MTLRegionMake2D(t->offset.x, t->offset.y, t->size.x, t->size.y) mipmapLevel:0 withBytes:pixels bytesPerRow:t->size.x * sizeof(rgba_t)];
}

uint16_t render_textures_len(void) { return textures_len; }
void render_textures_reset(uint16_t len) { textures_len = len; clear(atlas_map); }
void render_textures_dump(const char *path) {}
