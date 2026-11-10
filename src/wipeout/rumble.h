#ifndef RUMBLE_H
#define RUMBLE_H

void rumble_impulse(float strength, float duration);
void rumble_pulses(float strength, int count, float on, float off);
void rumble_update(void);
void rumble_stop(void);

#endif
