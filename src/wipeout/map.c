#include "map.h"
#include "../render.h"
#include "track.h"
#include "ship.h"
#include <math.h>

#define BASE_MAP_WIDTH  120
#define BASE_MAP_HEIGHT 120
#define BASE_MAP_MARGIN 15

void map_init(void) {
}

void map_update(void) {
}

void map_draw(void *track_ptr, void *ships_ptr, int ship_count, int player_ship_index) {
    if (!track_ptr || !ships_ptr || player_ship_index < 0 || player_ship_index >= ship_count) {
        return;
    }

    track_t *track = (track_t *)track_ptr;
    ship_t *ships = (ship_t *)ships_ptr;
    ship_t *player = &ships[player_ship_index];

    vec2i_t screen_size = render_size();

    float ui_scale = (float)screen_size.y / 480.0f;
    if (ui_scale < 0.5f) ui_scale = 0.5f;

    int map_width  = (int)(BASE_MAP_WIDTH * ui_scale);
    int map_height = (int)(BASE_MAP_HEIGHT * ui_scale);
    int map_margin = (int)(BASE_MAP_MARGIN * ui_scale);
    int offset_up  = (int)(50.0f * ui_scale);

    int screen_x_offset = map_margin; 
    int screen_y_offset = screen_size.y - map_height - map_margin - offset_up;

    render_push_2d(
        vec2i(screen_x_offset, screen_y_offset), 
        vec2i(map_width, map_height), 
        rgba(0, 0, 0, 120), 
        RENDER_NO_TEXTURE
    );

    float map_center_x = screen_x_offset + (map_width / 2.0f);
    float map_center_y = screen_y_offset + (map_height / 2.0f);
    float world_scale = 0.0015f / ui_scale; 

    float yaw = player->angle.y;
    float cos_yaw = cosf(-yaw);
    float sin_yaw = sinf(-yaw);

    if (track && track->sections) {
        for (int i = 0; i < track->section_count; i++) {
            section_t *section = &track->sections[i];

            float dx = section->center.x - player->position.x;
            float dz = section->center.z - player->position.z;

            float rot_x = dx * cos_yaw - dz * sin_yaw;
            float rot_z = dx * sin_yaw + dz * cos_yaw;

            float map_x = map_center_x + (rot_x * world_scale);
            float map_y = map_center_y - (rot_z * world_scale);

            if (map_x >= screen_x_offset && map_x <= (screen_x_offset + map_width) &&
                map_y >= screen_y_offset && map_y <= (screen_y_offset + map_height)) {
                
                int node_radius = (ui_scale < 0.8f) ? 0 : 1;
                for (int cy = -node_radius; cy <= node_radius; cy++) {
                    for (int cx = -node_radius; cx <= node_radius; cx++) {
                        if (cx * cx + cy * cy <= (node_radius * node_radius + 1)) {
                            render_push_2d(
                                vec2i((int)(map_x + cx), (int)(map_y + cy)), 
                                vec2i(1, 1), 
                                rgba(100, 150, 250, 180), 
                                RENDER_NO_TEXTURE
                            );
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < ship_count; i++) {
        float dx = ships[i].position.x - player->position.x;
        float dz = ships[i].position.z - player->position.z;

        float rot_x = dx * cos_yaw - dz * sin_yaw;
        float rot_z = dx * sin_yaw + dz * cos_yaw;

        float map_x = map_center_x + (rot_x * world_scale);
        float map_y = map_center_y - (rot_z * world_scale);

        if (map_x >= screen_x_offset && map_x <= (screen_x_offset + map_width) &&
            map_y >= screen_y_offset && map_y <= (screen_y_offset + map_height)) {
            
            rgba_t dot_color = (i == player_ship_index) ? rgba(0, 255, 255, 255) : rgba(255, 50, 50, 255);

            int ship_radius = (ui_scale < 0.8f) ? 1 : 2;
            for (int cy = -ship_radius; cy <= ship_radius; cy++) {
                for (int cx = -ship_radius; cx <= ship_radius; cx++) {
                    if (cx * cx + cy * cy <= (ship_radius * ship_radius + 1)) {
                        render_push_2d(
                            vec2i((int)(map_x + cx), (int)(map_y + cy)), 
                            vec2i(1, 1), 
                            dot_color, 
                            RENDER_NO_TEXTURE
                        );
                    }
                }
            }
        }
    }
}
