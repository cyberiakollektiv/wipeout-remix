#ifndef MAP_H
#define MAP_H

void map_init(void);
void map_update(void);
void map_draw(void *track, void *ships, int ship_count, int player_ship_index);

#endif
