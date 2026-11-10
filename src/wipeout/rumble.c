#include "../platform.h"
#include "../system.h"
#include "../utils.h"

#include "game.h"
#include "ship.h"
#include "rumble.h"

static float impulse_strength;
static float impulse_time;
static float pulse_strength;
static int pulse_count;
static float pulse_on;
static float pulse_off;
static float pulse_timer;
static bool pulse_on_phase;

void rumble_impulse(float strength, float duration) {
	if (!save.vibration_toggle || g.is_attract_mode) return;
	impulse_strength = fmaxf(impulse_strength, strength);
	impulse_time = fmaxf(impulse_time, duration);
}

void rumble_pulses(float strength, int count, float on, float off) {
	if (!save.vibration_toggle || g.is_attract_mode) return;
	pulse_strength = strength;
	pulse_count = count;
	pulse_on = on;
	pulse_off = off;
	pulse_timer = 0;
	pulse_on_phase = false;
}

static void rumble_step_pulses(float dt) {
	if (pulse_count <= 0 && !pulse_on_phase) return;
	pulse_timer -= dt;
	if (pulse_timer > 0) return;
	if (pulse_on_phase) {
		pulse_on_phase = false;
		pulse_timer += pulse_off;
	}
	else {
		pulse_on_phase = true;
		pulse_count--;
		pulse_timer += pulse_on;
	}
}

void rumble_update(void) {
	double dt = system_tick();
	if (impulse_time > 0) {
		impulse_time -= dt;
		if (impulse_time <= 0) {
			impulse_time = 0;
			impulse_strength = 0;
		}
	}
	rumble_step_pulses(dt);

	ship_t *player = &g.ships[g.pilot];
	float level = 0;
	if (impulse_time > 0) level = fmaxf(level, impulse_strength);
	if (pulse_on_phase) level = fmaxf(level, pulse_strength);
	if (flags_is(player->flags, SHIP_SHIELDED)) level = fmaxf(level, 0.25f);
	if (!save.vibration_toggle || g.is_attract_mode || flags_not(player->flags, SHIP_RACING)) {
		level = 0;
	}
	platform_set_rumble(fminf(fmaxf(level, 0), 1));
}

void rumble_stop(void) {
	impulse_strength = 0;
	impulse_time = 0;
	pulse_strength = 0;
	pulse_count = 0;
	pulse_on = 0;
	pulse_off = 0;
	pulse_timer = 0;
	pulse_on_phase = false;
	platform_set_rumble(0);
}
