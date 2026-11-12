#include <SDL3/SDL.h>

#include "platform.h"
#include "input.h"
#include "system.h"
#include "utils.h"
#include "mem.h"
#include "render.h"

static uint64_t perf_freq = 0;
static bool wants_to_exit = false;
static SDL_Window *window;
static SDL_AudioStream *audio_stream;
static SDL_Gamepad *gamepad;
static void (*audio_callback)(float *buffer, uint32_t len) = NULL;
static const char *path_assets = "";
static const char *path_userdata = "";
static char *temp_path = NULL;

#ifdef __APPLE__
    // macOS behaves best with desktop fullscreen (Space-based, non-exclusive).
    #define USE_FULLSCREEN_DESKTOP 1
#else
    #define USE_FULLSCREEN_DESKTOP 0
#endif


uint8_t platform_sdl_gamepad_map[] = {
    [SDL_GAMEPAD_BUTTON_SOUTH] = INPUT_GAMEPAD_A,
    [SDL_GAMEPAD_BUTTON_EAST] = INPUT_GAMEPAD_B,
    [SDL_GAMEPAD_BUTTON_WEST] = INPUT_GAMEPAD_X,
    [SDL_GAMEPAD_BUTTON_NORTH] = INPUT_GAMEPAD_Y,
    [SDL_GAMEPAD_BUTTON_BACK] = INPUT_GAMEPAD_SELECT,
    [SDL_GAMEPAD_BUTTON_GUIDE] = INPUT_GAMEPAD_HOME,
    [SDL_GAMEPAD_BUTTON_START] = INPUT_GAMEPAD_START,
    [SDL_GAMEPAD_BUTTON_LEFT_STICK] = INPUT_GAMEPAD_L_STICK_PRESS,
    [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = INPUT_GAMEPAD_R_STICK_PRESS,
    [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = INPUT_GAMEPAD_L_SHOULDER,
    [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = INPUT_GAMEPAD_R_SHOULDER,
    [SDL_GAMEPAD_BUTTON_DPAD_UP] = INPUT_GAMEPAD_DPAD_UP,
    [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = INPUT_GAMEPAD_DPAD_DOWN,
    [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = INPUT_GAMEPAD_DPAD_LEFT,
    [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = INPUT_GAMEPAD_DPAD_RIGHT,
    [SDL_GAMEPAD_BUTTON_COUNT] = INPUT_INVALID
};


uint8_t platform_sdl_axis_map[] = {
    [SDL_GAMEPAD_AXIS_LEFTX] = INPUT_GAMEPAD_L_STICK_LEFT,
    [SDL_GAMEPAD_AXIS_LEFTY] = INPUT_GAMEPAD_L_STICK_UP,
    [SDL_GAMEPAD_AXIS_RIGHTX] = INPUT_GAMEPAD_R_STICK_LEFT,
    [SDL_GAMEPAD_AXIS_RIGHTY] = INPUT_GAMEPAD_R_STICK_UP,
    [SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = INPUT_GAMEPAD_L_TRIGGER,
    [SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = INPUT_GAMEPAD_R_TRIGGER,
    [SDL_GAMEPAD_AXIS_COUNT] = INPUT_INVALID
};


void platform_exit(void) {
    wants_to_exit = true;
}

SDL_Gamepad *platform_find_gamepad(void) {
    int num_joysticks = 0;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&num_joysticks);
    if (joysticks) {
        for (int i = 0; i < num_joysticks; i++) {
            if (SDL_IsGamepad(joysticks[i])) {
                SDL_Gamepad *gp = SDL_OpenGamepad(joysticks[i]);
                if (gp && SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO)) {
                    SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true);
                }
                SDL_free(joysticks);
                return gp;
            }
        }
        SDL_free(joysticks);
    }
    return NULL;
}

void platform_set_rumble(float strength) {
    if (gamepad) {
        if (strength <= 0.0f) {
            SDL_RumbleGamepad(gamepad, 0, 0, 0);
        } else {
            uint16_t intensity = (uint16_t)(fminf(fmaxf(strength, 0.0f), 1.0f) * 0xFFFF);
            SDL_RumbleGamepad(gamepad, intensity, intensity, 250);
        }
    }
}

void platform_pump_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        // Detect ALT+Press (LALT or RALT) + Return to toggle fullscreen
        SDL_Keymod mod = SDL_GetModState();
        if (
            ev.type == SDL_EVENT_KEY_DOWN && 
            ev.key.scancode == SDL_SCANCODE_RETURN &&
            (mod & (SDL_KMOD_LALT | SDL_KMOD_RALT))
        ) {
            platform_set_fullscreen(!platform_get_fullscreen());
        }

        // Input Keyboard
        else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
            SDL_Scancode code = ev.key.scancode;
            float state = ev.type == SDL_EVENT_KEY_DOWN ? 1.0f : 0.0f;
            if (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RALT) {
                int code_internal = code - SDL_SCANCODE_LCTRL + INPUT_KEY_LCTRL;
                input_set_button_state(code_internal, state);
            }
            else if ((int)code > 0 && (int)code < INPUT_KEY_MAX) {
                input_set_button_state(code, state);
            }
        }

        else if (ev.type == SDL_EVENT_TEXT_INPUT) {
            input_textinput(ev.text.text[0]);
        }

        // Gamepads connect/disconnect
        else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
            gamepad = SDL_OpenGamepad(ev.gdevice.which);
            if (gamepad && SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO)) {
            SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true);
    }
}
        else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (gamepad && ev.gdevice.which == SDL_GetGamepadID(gamepad)) {
                SDL_CloseGamepad(gamepad);
                gamepad = platform_find_gamepad();
            }
        }

        // Input Gyro

        else if (ev.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
            if (ev.gsensor.sensor == SDL_SENSOR_GYRO) {
                
                float gyro_x = ev.gsensor.data[0];
                float gyro_y = ev.gsensor.data[1];
                float gyro_z = ev.gsensor.data[2];

                input_set_gyro_data(gyro_x, gyro_y, gyro_z);

            }
            else if (ev.gsensor.sensor == SDL_SENSOR_ACCEL) {
                float accel_x = ev.gsensor.data[0];
                float accel_y = ev.gsensor.data[1];
                float accel_z = ev.gsensor.data[2];
            }
        }

        // Input Gamepad Buttons
        else if (
            ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || 
            ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP
        ) {
            if (ev.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                button_t button = platform_sdl_gamepad_map[ev.gbutton.button];
                if (button != INPUT_INVALID) {
                    float state = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f;
                    input_set_button_state(button, state);
                }
            }
        }

        // Input Gamepad Axis
        else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            float state = (float)ev.gaxis.value / 32767.0f;

            if (ev.gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
                int code = platform_sdl_axis_map[ev.gaxis.axis];
                if (
                    code == INPUT_GAMEPAD_L_TRIGGER || 
                    code == INPUT_GAMEPAD_R_TRIGGER
                ) {
                    input_set_button_state(code, state);
                }
                else if (state > 0) {
                    input_set_button_state(code, 0.0f);
                    input_set_button_state(code+1, state);
                }
                else {
                    input_set_button_state(code, -state);
                    input_set_button_state(code+1, 0.0f);
                }
            }
        }

        // Mouse buttons
        else if (
            ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            ev.type == SDL_EVENT_MOUSE_BUTTON_UP
        ) {
            button_t button = INPUT_BUTTON_NONE;
            switch (ev.button.button) {
                case SDL_BUTTON_LEFT: button = INPUT_MOUSE_LEFT; break;
                case SDL_BUTTON_MIDDLE: button = INPUT_MOUSE_MIDDLE; break;
                case SDL_BUTTON_RIGHT: button = INPUT_MOUSE_RIGHT; break;
                default: break;
            }
            if (button != INPUT_BUTTON_NONE) {
                float state = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.0f : 0.0f;
                input_set_button_state(button, state);
            }
        }

        // Mouse wheel
        else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
            button_t button = ev.wheel.y > 0 
                ? INPUT_MOUSE_WHEEL_UP
                : INPUT_MOUSE_WHEEL_DOWN;
            input_set_button_state(button, 1.0f);
            input_set_button_state(button, 0.0f);
        }

        // Mouse move
        else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            input_set_mouse_pos(ev.motion.x, ev.motion.y);
        }

        // Window Events
        if (ev.type == SDL_EVENT_QUIT) {
            wants_to_exit = true;
        }
        else if (
            ev.type == SDL_EVENT_WINDOW_RESIZED ||
            ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
        ) {
            system_resize(platform_screen_size());
        }
    }
}

double platform_now(void) {
    uint64_t perf_counter = SDL_GetPerformanceCounter();
    return (double)perf_counter / (double)perf_freq;
}

bool platform_get_fullscreen(void) {
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void platform_set_fullscreen(bool fullscreen) {
    if (fullscreen) {
        #if !USE_FULLSCREEN_DESKTOP
            SDL_DisplayID display = SDL_GetDisplayForWindow(window);
            const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
            if (mode) {
                SDL_SetWindowFullscreenMode(window, mode);
            }
        #endif
        SDL_SetWindowFullscreen(window, true);
        SDL_HideCursor();
    }
    else {
        SDL_SetWindowFullscreen(window, false);
        SDL_ShowWindow(window);
        SDL_ShowCursor();
    }
}

static void SDLCALL platform_audio_stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    if (audio_callback) {
        int num_floats = additional_amount / sizeof(float);
        float *buffer = SDL_malloc(additional_amount);
        if (buffer) {
            audio_callback(buffer, num_floats);
            SDL_PutAudioStreamData(stream, buffer, additional_amount);
            SDL_free(buffer);
        }
    } else {
        void *zero_buffer = SDL_calloc(1, additional_amount);
        if (zero_buffer) {
            SDL_PutAudioStreamData(stream, zero_buffer, additional_amount);
            SDL_free(zero_buffer);
        }
    }
}

void platform_set_audio_mix_cb(void (*cb)(float *buffer, uint32_t len)) {
    audio_callback = cb;
    SDL_ResumeAudioStreamDevice(audio_stream);
}


bool platform_asset_exists(const char *name) {
    char *path = strcat(strcpy(temp_path, path_assets), name);
    return file_exists(path);
}

FILE *platform_open_asset(const char *name, const char *mode) {
    char *path = strcat(strcpy(temp_path, path_assets), name);
    return fopen(path, mode);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
    char *path = strcat(strcpy(temp_path, path_assets), name);
    return file_load(path, bytes_read);
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
    char *path = strcat(strcpy(temp_path, path_userdata), name);
    if (!file_exists(path)) {
        *bytes_read = 0;
        return NULL;
    }
    return file_load(path, bytes_read);
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
    char *path = strcat(strcpy(temp_path, path_userdata), name);
    return file_store(path, bytes, len);
}

#if defined(RENDERER_GL) // ----------------------------------------------------
    #define PLATFORM_WINDOW_FLAGS SDL_WINDOW_OPENGL
    SDL_GLContext platform_gl;

    void platform_video_init(void) {
        #if defined(USE_GLES2)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        #endif

        platform_gl = SDL_GL_CreateContext(window);
        SDL_GL_SetSwapInterval(1);
    }

    void platform_prepare_frame(void) {
        // nothing
    }

    void platform_video_cleanup(void) {
        SDL_GL_DestroyContext(platform_gl);
    }

    void platform_end_frame(void) {
        SDL_GL_SwapWindow(window);
    }

    vec2i_t platform_screen_size(void) {
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return vec2i(width, height);
    }

#elif defined(RENDERER_VK) // ---------------------------------------------------
    #define PLATFORM_WINDOW_FLAGS SDL_WINDOW_VULKAN

    void platform_video_init(void) {
        render_init_surface(window);
    }

    void platform_prepare_frame(void) {
        render_frame_prepare();
    }

    void platform_video_cleanup(void) {
        render_cleanup();    }

    void platform_end_frame(void) {
        render_frame_end();
    }

    vec2i_t platform_screen_size(void) {
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return vec2i(width, height);
    }

#elif defined(RENDERER_MTL) // ---------------------------------------------------
    #define PLATFORM_WINDOW_FLAGS SDL_WINDOW_METAL

    static SDL_MetalView metal_view = NULL;

    void platform_video_init(void) {
        metal_view = SDL_Metal_CreateView(window);
        
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        render_init(vec2i(width, height));
    }

    void platform_prepare_frame(void) {
        render_frame_prepare();
    }

    void platform_video_cleanup(void) {
        render_cleanup();
        if (metal_view) {
            SDL_Metal_DestroyView(metal_view);
            metal_view = NULL;
        }
    }

    void platform_end_frame(void) {
        render_frame_end();
    }

    vec2i_t platform_screen_size(void) {
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return vec2i(width, height);
    }
#elif defined(RENDERER_SOFTWARE) || defined(RENDERER_NULL)// -------------------
    #define PLATFORM_WINDOW_FLAGS 0
    static SDL_Renderer *renderer;
    static SDL_Texture *screenbuffer = NULL;
    static void *screenbuffer_pixels = NULL;
    static int screenbuffer_pitch;
    static vec2i_t screenbuffer_size = vec2i(0, 0);
    static vec2i_t screen_size = vec2i(0, 0);


    void platform_video_init(void) {
        renderer = SDL_CreateRenderer(window, NULL);
    }

    void platform_video_cleanup(void) {
        if (screenbuffer) {
            SDL_DestroyTexture(screenbuffer);
        }
        SDL_DestroyRenderer(renderer);
    }

    void platform_prepare_frame(void) {
        if (screen_size.x != screenbuffer_size.x || screen_size.y != screenbuffer_size.y) {
            if (screenbuffer) {
                SDL_DestroyTexture(screenbuffer);
            }
            screenbuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, screen_size.x, screen_size.y);
            screenbuffer_size = screen_size;
        }
        SDL_LockTexture(screenbuffer, NULL, &screenbuffer_pixels, &screenbuffer_pitch);
    }

    void platform_end_frame(void) {
        screenbuffer_pixels = NULL;
        SDL_UnlockTexture(screenbuffer);
        SDL_RenderTexture(renderer, screenbuffer, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    rgba_t *platform_get_screenbuffer(int32_t *pitch) {
        *pitch = screenbuffer_pitch;
        return screenbuffer_pixels;
    }

    vec2i_t platform_screen_size(void) {
        int width, height;
        SDL_GetWindowSize(window, &width, &height);

        screen_size = vec2i(width, height);
        return screen_size;
    }

#else
    #error "Unsupported renderer for platform SDL"
#endif

int main(int argc, char *argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        printf("Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    const char *sdl_path_assets = NULL;
    #ifdef PATH_ASSETS
        path_assets = TOSTRING(PATH_ASSETS);
    #else
        sdl_path_assets = SDL_GetBasePath();
        if (sdl_path_assets) {
            path_assets = sdl_path_assets;
        }
    #endif

    const char *sdl_path_userdata = NULL;
    #ifdef PATH_USERDATA
        path_userdata = TOSTRING(PATH_USERDATA);
    #else
        sdl_path_userdata = SDL_GetPrefPath("phoboslab", "wipeout");
        if (sdl_path_userdata) {
            path_userdata = sdl_path_userdata;
        }
    #endif

    temp_path = mem_bump(max(strlen(path_assets), strlen(path_userdata)) + 64);

    char *gcdb_path = strcat(strcpy(temp_path, path_assets), "gamecontrollerdb.txt");
    bool gcdb_res = SDL_AddGamepadMappingsFromFile(gcdb_path);
    if (!gcdb_res) {
        printf("Failed to load gamecontrollerdb.txt: %s\n", SDL_GetError());
    }
    else {
        printf("Loaded gamecontrollerdb.txt\n");
    }

    gamepad = platform_find_gamepad();

    perf_freq = SDL_GetPerformanceFrequency();

    SDL_AudioSpec audio_spec = {
        .format = SDL_AUDIO_F32,
        .channels = 2,
        .freq = 44100
    };

    audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, 
        &audio_spec, 
        platform_audio_stream_callback, 
        NULL
    );

    if (!audio_stream) {
        printf("Failed to create audio stream: %s\n", SDL_GetError());
    }

    window = SDL_CreateWindow(
        SYSTEM_WINDOW_NAME,
        SYSTEM_WINDOW_WIDTH, SYSTEM_WINDOW_HEIGHT,
        SDL_WINDOW_RESIZABLE | PLATFORM_WINDOW_FLAGS | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );

    if (!window) {
        printf("Failed to create window: %s\n", SDL_GetError());
    }

    platform_video_init();
    system_init();

    while (!wants_to_exit) {
        platform_pump_events();
        platform_prepare_frame();
        system_update();
        platform_end_frame();
    }

    system_cleanup();
    platform_video_cleanup();

    if (window) {
        SDL_DestroyWindow(window);
    }

    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }

    if (sdl_path_assets) {
        SDL_free((void *)sdl_path_assets);
    }
    if (sdl_path_userdata) {
        SDL_free((void *)sdl_path_userdata);
    }

    if (audio_stream) {
        SDL_DestroyAudioStream(audio_stream);
    }
    
    SDL_Quit();
    return 0;
}
