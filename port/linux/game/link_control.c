/*
LINK_CONTROL.C

Halo 64 Link's control (link/README.md): the Nintendo 64 plays its player's
own unit in a game of its own on the same map, at once, and sends each
tick's control (CONTROL, link/platform/link_cable.c) -- the facing, the
throttle, the trigger, the buttons, the weapon, grenade and zoom it wants,
as player_control.c made them there. Here the same control is played: the
Link's player's control (player_control.c's handle_one_player_input, after
its own input: HALO_LINK) is the N64's, a tick's at a time, the oldest
first. So both games move the unit alike, and the Link's state of it,
sent back each frame (link_export.c), corrects the N64's.

- One control is taken each time the game's clock moves on (the Link runs
  two frames a tick); none queued: the last one is played again. More than
  CONTROL_LEAD queued: the oldest are dropped (the N64 must not get ahead).
- The N64's tick of the control last taken goes back with each frame
  (link_control_applied_tick): the N64 compares its unit after that tick
  with the Link's.
- A unit just spawned faces where the game spawned it: the N64's facing is
  not played until the N64 has turned that way too (it does when it spawns
  its unit where the Link's is), or SPAWN_HOLD_TICKS have passed.
*/

#ifdef HALO_LINK

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "units/units.h"
#include "objects/objects.h"

#include "../../../link/platform/link_cable.h"

#include <math.h>
#include <string.h>

/* player_control.c's */
struct player_control *player_control_get(short local_player_index);

/* controls queued past this many: the oldest dropped */
#define CONTROL_LEAD 3
#define SPAWN_HOLD_TICKS 90
/* a facing this close (radians) is the same */
#define FACING_SAME 0.02f

static struct
{
	long game_time;
	boolean have;
	unsigned long tick;
	unsigned long flags;
	real yaw, pitch, throttle[2], trigger;
	short weapon, grenade, zoom;
	/* the unit last seen, and a new one's facing held */
	long unit_index;
	long hold_until;
	real_euler_angles2d spawn_angles;
	/* the weapon and grenade the N64 last wanted (played when they change:
	a weapon picked up here is held until the N64 hears of it) */
	short played_weapon, played_grenade;
} control = { NONE, FALSE, 0xFFFFFFFFUL, 0, 0.0f, 0.0f, { 0.0f, 0.0f }, 0.0f, NONE, NONE, NONE, NONE, NONE,
	{ 0.0f, 0.0f }, NONE, NONE };

static unsigned long get_u32(unsigned char const *at)
{
	return (unsigned long)at[0] << 24 | (unsigned long)at[1] << 16 | (unsigned long)at[2] << 8 | at[3];
}

static real get_f32(unsigned char const *at)
{
	unsigned long bits = get_u32(at);
	unsigned int bits32 = (unsigned int)bits;
	float value;

	memcpy(&value, &bits32, sizeof(value));
	/* (not a number, or infinite: none) */
	return value == value && value < 3.0e38f && value > -3.0e38f ? value : 0.0f;
}

static void take(unsigned char const *record)
{
	control.tick = get_u32(record);
	/* (record[4]: the local player, the first for now) */
	control.flags = get_u32(record + 5);
	control.yaw = get_f32(record + 9);
	control.pitch = get_f32(record + 13);
	control.throttle[0] = PIN(get_f32(record + 17), -1.0f, 1.0f);
	control.throttle[1] = PIN(get_f32(record + 21), -1.0f, 1.0f);
	control.trigger = PIN(get_f32(record + 25), 0.0f, 1.0f);
	control.weapon = (short)(signed char)record[29];
	control.grenade = (short)(signed char)record[30];
	control.zoom = (short)(signed char)record[31];
	control.have = TRUE;
}

static real angle_difference(real a, real b)
{
	real d = (real)fmod(a - b, 2.0 * 3.14159265358979);

	if (d > 3.14159265f)
		d -= 6.2831853f;
	if (d < -3.14159265f)
		d += 6.2831853f;
	return (real)fabs(d);
}

/* player_control.c's (handle_one_player_input, HALO_LINK): the local
player's control made the N64's */
void link_control_apply(
	short local_player_index,
	struct player_control *player)
{
	long now = game_time_get();
	long unit_index;

	if (local_player_index != 0 || !link_cable_connected() || !game_in_progress())
	{
		if (!link_cable_connected())
			control.have = FALSE;
		return;
	}
	if (now != control.game_time)
	{
		unsigned char record[LINK_CABLE_CONTROL_SIZE];
		unsigned long dropped_flags = 0;

		control.game_time = now;
		/* (the oldest dropped, their buttons kept: a press is never lost) */
		while (link_cable_controls_queued() > CONTROL_LEAD && link_cable_take_control(record))
		{
			take(record);
			dropped_flags |= control.flags;
		}
		if (link_cable_take_control(record))
			take(record);
		control.flags |= dropped_flags;
	}
	/* (a new unit: its facing kept until the N64 faces that way too) */
	unit_index = player->unit_index;
	if (unit_index != control.unit_index)
	{
		control.unit_index = unit_index;
		if (unit_index != NONE)
		{
			control.spawn_angles = player->desired_angles;
			control.hold_until = now + SPAWN_HOLD_TICKS;
		}
	}
	if (!control.have)
		return;
	if (control.hold_until != NONE)
	{
		if (now > control.hold_until || (angle_difference(control.yaw, control.spawn_angles.yaw) < FACING_SAME &&
			angle_difference(control.pitch, control.spawn_angles.pitch) < FACING_SAME))
		{
			control.hold_until = NONE;
		}
	}
	if (control.hold_until == NONE)
	{
		player->desired_angles.yaw = control.yaw;
		player->desired_angles.pitch = control.pitch;
	}
	player->control_flags = control.flags;
	player->throttle.i = control.throttle[0];
	player->throttle.j = control.throttle[1];
	player->primary_trigger = control.trigger;
	if (unit_index != NONE)
	{
		/* (the weapon and grenade the N64 chose, when it chooses: a weapon
		picked up here goes in the unit's hands, and the N64 still wants the
		one it held until it hears of it, a frame or two later) */
		if (control.weapon != control.played_weapon && control.weapon >= 0 &&
			control.weapon < MAXIMUM_WEAPONS_PER_UNIT && unit_inventory_get_weapon(unit_index, control.weapon) != NONE)
		{
			player->desired_weapon_index = control.weapon;
		}
		control.played_weapon = control.weapon;
		if (control.grenade != control.played_grenade && control.grenade >= 0 &&
			control.grenade < NUMBER_OF_UNIT_GRENADE_TYPES && unit_get_grenade_count(unit_index, control.grenade) > 0)
		{
			player->desired_grenade_index = control.grenade;
		}
		control.played_grenade = control.grenade;
		player->zoom_level = control.zoom >= 0 ? control.zoom : NONE;
	}
}

/* the N64's tick whose control the Link's last tick played */
unsigned long link_control_applied_tick(
	void)
{
	return control.have ? control.tick : 0xFFFFFFFFUL;
}

/* the facing the N64's unit should have: a new unit's while it is held,
else the control's */
void link_control_facing(
	real *yaw,
	real *pitch)
{
	struct player_control *player = player_control_get(0);

	if (control.hold_until != NONE)
	{
		*yaw = control.spawn_angles.yaw;
		*pitch = control.spawn_angles.pitch;
	}
	else
	{
		*yaw = player->desired_angles.yaw;
		*pitch = player->desired_angles.pitch;
	}
}

#endif
