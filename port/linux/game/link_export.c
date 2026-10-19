/*
LINK_EXPORT.C

Halo 64 Link's frames (link/README.md): after each tick of a game, what the
Nintendo 64 on the cable draws, for its renderer as it is (halo-64's
port/n64/src/n64.h: n64_render_view, n64_render_object):

  FRAME      the tick (4), the views (1), each: the local player (1), the
             eye (3 x f32), forward and up (3 + 3 x s16, x32767), the
             vertical field of view (f32), the clusters it may see: their
             count (2) and a bit each (bytes, rounded up to four), then
             its player's HUD: flags (1: 1 alive), health (1, x255),
             shields (2, x1000), its weapon's name (a string), the rounds
             loaded and in reserve (2 + 2), heat (1, x255), frag and plasma
             grenades (1 + 1), then its player's own unit (put_self, the
             N64 plays it itself and is corrected by it: n64_online_game.c):
             the N64's tick whose control the Link last played (4,
             link_control.c), flags (1: 1 alive, 2 in a vehicle, 4 something
             near to pick up or get into), position and velocity (3 + 3 x
             f32), the facing's yaw and pitch (2 x f32), the respawn's ticks
             (2), the team (1), the weapon held (1, its slot; 0xFF none),
             and each of four weapon slots: the weapon's tag (2, its absolute
             index; 0xFFFF none), rounds loaded and in reserve (2 + 2), its
             age (1, x255: an energy weapon's charge used)
  OBJECTS    the tick (4), the objects in this packet (1), each:
             its id (2: the object's absolute index), its model's tag index
             (2), its tint (4: RGBA, alpha its camouflage), its regions'
             permutations (a count (1), a byte each), its bounding sphere
             (the centre 3 x f32, the radius f32), its nodes (a count (1),
             flags (1: 1 scales follow), the first node's position (3 x
             f32), then each node: its rotation as a quaternion (4 x s16,
             x32767), its position from the first node's (3 x s16, x256),
             and with flag 1 its scale (u16, x4096))
  FRAME_END  the tick (4), the objects in the frame (2)

each a link packet (link_cable.c), big-endian; as many OBJECTS packets a
frame as its objects fill (each at most LINK_CABLE_PAYLOAD_MAXIMUM bytes).
The objects are those the game would draw (visible, with a model, not
invisible) that reach a cluster one of the views may see, within
EXPORT_RANGE of one of their eyes, EXPORT_OBJECTS at most -- but not the
map's own (scenery, devices, lights: the N64's game has them), nor what the
N64 plays itself: its players' units on foot and what they hold, their
bodies, and their projectiles.
*/

#ifdef HALO_LINK

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "units/units.h"
#include "items/weapons.h"
#include "tag_files/tag_files.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "camera/observer.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "objects/damage.h"

#include "../../../link/platform/link_cable.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LINK_CABLE_OBJECTS 70
#define LINK_CABLE_FRAME_END 71

#define EXPORT_OBJECTS 128
#define EXPORT_RANGE 150.0f
#define EXPORT_NODES 64
#define CLUSTER_BITS 1024
/* the map's own objects: the N64's game has them */
#define MAP_OBJECT_MASK (FLAG(_object_type_scenery) | FLAG(_object_type_machine) | FLAG(_object_type_control) | \
	FLAG(_object_type_light_fixture) | FLAG(_object_type_placeholder) | FLAG(_object_type_sound_scenery))

/* link_control.c's */
unsigned long link_control_applied_tick(void);
void link_control_facing(real *yaw, real *pitch);

static struct
{
	long last_tick;
	unsigned long last_inputs;
	/* the local players: their units (played on the N64: left out while on
	foot), their bodies and players */
	long own_units[4], own_dead_units[4], own_players[4];
	unsigned char packet[LINK_CABLE_PAYLOAD_MAXIMUM];
	long length;
	short view_count;
	real_point3d eyes[4];
	unsigned long clusters[4][CLUSTER_BITS / 32];
	short cluster_count;
} export;

/* ---------- writing */

static void put_u8(unsigned long value)
{
	export.packet[export.length++] = (unsigned char)value;
}

static void put_u16(unsigned long value)
{
	put_u8(value >> 8);
	put_u8(value);
}

static void put_u32(unsigned long value)
{
	put_u16(value >> 16);
	put_u16(value);
}

static void put_f32(real value)
{
	unsigned long bits;

	memcpy(&bits, &value, sizeof(bits));
	put_u32(bits);
}

static void put_unit(real value)
{
	long scaled = (long)floor(value * 32767.0f + 0.5f);

	put_u16((unsigned long)(scaled > 32767 ? 32767 : scaled < -32767 ? -32767 : scaled) & 0xFFFF);
}

static void put_fixed(real value, real scale)
{
	long scaled = (long)floor(value * scale + 0.5f);

	put_u16((unsigned long)(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled) & 0xFFFF);
}

/* ---------- the views */

static void view_clusters(short view, struct observer_result const *camera)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	unsigned long *pvs;
	short cluster = camera->location.cluster_index;
	short words;

	memset(export.clusters[view], 0xFF, sizeof(export.clusters[view]));
	if (!bsp || bsp->clusters.count <= 0 || bsp->clusters.count > CLUSTER_BITS)
	{
		export.cluster_count = 0;
		return;
	}
	export.cluster_count = (short)bsp->clusters.count;
	words = (short)((export.cluster_count + 31) / 32);
	if (cluster == NONE || !(pvs = structure_bsp_get_cluster_pvs(bsp, cluster)))
		return;
	memcpy(export.clusters[view], pvs, (size_t)words * sizeof(unsigned long));
}

static void put_string(char const *text)
{
	long length = (long)strlen(text);

	if (length > 31)
		length = 31;
	put_u8((unsigned long)length);
	memcpy(export.packet + export.length, text, (size_t)length);
	export.length += length;
}

/* a view's player's HUD (the format above) */
static void put_hud(short view)
{
	long unit_index = export.own_units[view];
	struct unit_datum *unit = unit_index != NONE ? (struct unit_datum *)object_try_and_get(unit_index) : NULL;
	struct weapon_datum *weapon = NULL;
	char name[32] = "";

	if (unit && unit->unit.current_weapon_index >= 0 && unit->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT &&
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] != NONE)
	{
		weapon = (struct weapon_datum *)object_try_and_get(
			unit->unit.weapon_object_indices[unit->unit.current_weapon_index]);
	}
	if (weapon)
	{
		char const *path = tag_get_name(weapon->definition_index);

		snprintf(name, sizeof(name), "%s", path ? tag_name_strip_path(path) : "");
	}
	put_u8(unit ? 1 : 0);
	put_u8(unit ? (unsigned long)PIN(unit->object.body_vitality * 255.0f, 0.0f, 255.0f) : 0);
	put_u16(unit ? (unsigned long)PIN(unit->object.shield_vitality * 1000.0f, 0.0f, 65535.0f) : 0);
	put_string(name);
	put_u16(weapon ? (unsigned long)(weapon->weapon.magazines[0].rounds_loaded & 0xFFFF) : 0);
	put_u16(weapon ? (unsigned long)(weapon->weapon.magazines[0].rounds_total & 0xFFFF) : 0);
	put_u8(weapon ? (unsigned long)PIN(weapon->weapon.heat * 255.0f, 0.0f, 255.0f) : 0);
	put_u8(unit ? (unsigned long)(unsigned char)unit->unit.grenade_counts[0] : 0);
	put_u8(unit ? (unsigned long)(unsigned char)unit->unit.grenade_counts[1] : 0);
}

/* a view's player's own unit (the format above) */
static void put_self(short view)
{
	long player_index = export.own_players[view];
	struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
	long unit_index = export.own_units[view];
	struct unit_datum *unit = unit_index != NONE ? (struct unit_datum *)object_try_and_get(unit_index) : NULL;
	boolean alive = unit && TEST_FLAG(_object_mask_unit, unit->object.type) &&
		!TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
	unsigned long flags = 0;
	real yaw = 0.0f, pitch = 0.0f;
	short slot;

	put_u32(view == 0 ? link_control_applied_tick() : 0xFFFFFFFFUL);
	if (alive)
		flags |= 1;
	if (alive && unit->object.parent_object_index != NONE)
		flags |= 2;
	if (alive && player && player->action_object_index != NONE)
		flags |= 4;
	put_u8(flags);
	put_f32(alive ? unit->object.position.x : 0.0f);
	put_f32(alive ? unit->object.position.y : 0.0f);
	put_f32(alive ? unit->object.position.z : 0.0f);
	put_f32(alive ? unit->object.translational_velocity.i : 0.0f);
	put_f32(alive ? unit->object.translational_velocity.j : 0.0f);
	put_f32(alive ? unit->object.translational_velocity.k : 0.0f);
	if (view == 0)
		link_control_facing(&yaw, &pitch);
	put_f32(yaw);
	put_f32(pitch);
	put_u16(player ? (unsigned long)PIN(player->respawn_timer, 0, 65535) : 0);
	put_u8(player ? (unsigned long)(player->team_index & 0xFF) : 0xFF);
	put_u8(alive && unit->unit.current_weapon_index >= 0 ? (unsigned long)unit->unit.current_weapon_index : 0xFF);
	for (slot = 0; slot < 4; slot++)
	{
		struct weapon_datum *weapon = NULL;

		if (alive && slot < MAXIMUM_WEAPONS_PER_UNIT && unit->unit.weapon_object_indices[slot] != NONE)
			weapon = (struct weapon_datum *)object_try_and_get(unit->unit.weapon_object_indices[slot]);
		put_u16(weapon ? (unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(weapon->definition_index) : 0xFFFF);
		put_u16(weapon ? (unsigned long)(weapon->weapon.magazines[0].rounds_loaded & 0xFFFF) : 0);
		put_u16(weapon ? (unsigned long)(weapon->weapon.magazines[0].rounds_total & 0xFFFF) : 0);
		put_u8(weapon ? (unsigned long)PIN(weapon->weapon.age * 255.0f, 0.0f, 255.0f) : 0);
	}
}

static void send_frame(long tick)
{
	short view;

	export.length = 0;
	put_u32((unsigned long)tick);
	export.view_count = local_player_count();
	if (export.view_count > 4)
		export.view_count = 4;
	put_u8((unsigned long)export.view_count);
	for (view = 0; view < export.view_count; view++)
	{
		struct observer_result const *camera = observer_get_camera(view);
		short words, word;

		put_u8((unsigned long)view);
		put_f32(camera->position.x);
		put_f32(camera->position.y);
		put_f32(camera->position.z);
		put_unit(camera->forward.i);
		put_unit(camera->forward.j);
		put_unit(camera->forward.k);
		put_unit(camera->up.i);
		put_unit(camera->up.j);
		put_unit(camera->up.k);
		put_f32(camera->field_of_view);
		export.eyes[view] = camera->position;
		{
			long player_index = local_player_get_player_index(view);
			struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;

			export.own_players[view] = player_index;
			export.own_units[view] = player ? player->unit_index : NONE;
			export.own_dead_units[view] = player ? player->dead_unit_index : NONE;
		}
		view_clusters(view, camera);
		put_u16((unsigned long)export.cluster_count);
		words = (short)((export.cluster_count + 31) / 32);
		for (word = 0; word < words; word++)
			put_u32(export.clusters[view][word]);
		put_hud(view);
		put_self(view);
	}
	link_cable_send(LINK_CABLE_FRAME, export.packet, (int)export.length);
}

/* ---------- the objects */

static boolean object_wanted(long object_index, struct object_datum *object)
{
	struct object_cluster_iterator iterator;
	short cluster, view;
	boolean near = FALSE;
	long ultimate = object_get_ultimate_parent(object_index);

	/* (the map's own: the N64's game has them; and garbage -- shell casings,
	debris --, which the effects that throw it make on the N64 too) */
	if (TEST_FLAG(MAP_OBJECT_MASK, object->object.type) || object->object.type == _object_type_garbage)
		return FALSE;
	/* (what the N64 plays itself: a local player's unit on foot and what it
	holds -- in a vehicle the vehicle is the ultimate parent, and it is drawn
	-- its body, and the projectiles it fires) */
	for (view = 0; view < export.view_count; view++)
	{
		if (export.own_units[view] != NONE && ultimate == export.own_units[view])
			return FALSE;
		if (export.own_dead_units[view] != NONE && ultimate == export.own_dead_units[view])
			return FALSE;
		if (object->object.type == _object_type_projectile && export.own_players[view] != NONE &&
			object->object.owner_player_index == export.own_players[view])
		{
			return FALSE;
		}
	}

	for (view = 0; view < export.view_count && !near; view++)
	{
		real dx = object->object.bounding_sphere_center.x - export.eyes[view].x;
		real dy = object->object.bounding_sphere_center.y - export.eyes[view].y;
		real dz = object->object.bounding_sphere_center.z - export.eyes[view].z;

		near = dx * dx + dy * dy + dz * dz < EXPORT_RANGE * EXPORT_RANGE;
	}
	if (!near)
		return FALSE;
	if (!export.cluster_count)
		return TRUE;
	for (cluster = object_get_first_cluster(&iterator, object_index); cluster != NONE;
		cluster = object_get_next_cluster(&iterator, object_index))
	{
		if (cluster < 0 || cluster >= export.cluster_count)
			return TRUE;
		for (view = 0; view < export.view_count; view++)
		{
			if (export.clusters[view][cluster >> 5] & (1UL << (cluster & 31)))
				return TRUE;
		}
	}
	return FALSE;
}

/* a rotation (forward, left, up: the columns) as a quaternion */
static void quaternion(struct real_matrix4x3 const *matrix, real q[4])
{
	real fx = matrix->forward.i, fy = matrix->forward.j, fz = matrix->forward.k;
	real lx = matrix->left.i, ly = matrix->left.j, lz = matrix->left.k;
	real ux = matrix->up.i, uy = matrix->up.j, uz = matrix->up.k;
	real trace = fx + ly + uz, s;

	if (trace > 0.0f)
	{
		s = 0.5f / (real)sqrt(trace + 1.0f);
		q[3] = 0.25f / s;
		q[0] = (lz - uy) * s;
		q[1] = (ux - fz) * s;
		q[2] = (fy - lx) * s;
	}
	else if (fx > ly && fx > uz)
	{
		s = 2.0f * (real)sqrt(1.0f + fx - ly - uz);
		q[3] = (lz - uy) / s;
		q[0] = 0.25f * s;
		q[1] = (lx + fy) / s;
		q[2] = (ux + fz) / s;
	}
	else if (ly > uz)
	{
		s = 2.0f * (real)sqrt(1.0f + ly - fx - uz);
		q[3] = (ux - fz) / s;
		q[0] = (lx + fy) / s;
		q[1] = 0.25f * s;
		q[2] = (uy + lz) / s;
	}
	else
	{
		s = 2.0f * (real)sqrt(1.0f + uz - fx - ly);
		q[3] = (fy - lx) / s;
		q[0] = (ux + fz) / s;
		q[1] = (uy + lz) / s;
		q[2] = 0.25f * s;
	}
}

static unsigned char camouflage(struct object_datum *object)
{
	short depth;

	for (depth = 0; object && depth < 16; depth++)
	{
		if (TEST_FLAG(_object_mask_unit, object->object.type))
		{
			real opacity = 1.0f - ((struct unit_datum *)object)->unit.active_camouflage * 0.9f;

			return (unsigned char)PIN(opacity * 255.0f, 0.0f, 255.0f);
		}
		if (object->object.parent_object_index == NONE)
			break;
		object = object_try_and_get(object->object.parent_object_index);
	}
	return 255;
}

/* the object's record (its size), or 0 if it would not fit */
static long object_record(long object_index, struct object_datum *object, unsigned char *out, long room)
{
	struct object_definition *definition = object_definition_get(object->definition_index);
	struct model *model;
	struct real_matrix4x3 const *nodes;
	struct object_datum *owner;
	long start = export.length, region, node_count, node;
	boolean scaled = FALSE;
	unsigned char tint[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

	if (definition->object.model.index == NONE)
		return 0;
	model = model_definition_get(definition->object.model.index);
	node_count = model->nodes.count;
	if (node_count <= 0 || node_count > EXPORT_NODES)
		return 0;
	nodes = object_get_node_matrices(object_index);
	for (node = 0; node < node_count; node++)
	{
		if (fabs(nodes[node].scale - 1.0f) > 0.001f)
			scaled = TRUE;
	}
	if (start + 64 + MAXIMUM_REGIONS_PER_OBJECT + node_count * (scaled ? 16 : 14) > room)
		return 0;
	owner = object_get(object_get_ultimate_parent(object_index));
	if (TEST_FLAG(_object_mask_unit, object->object.type) || TEST_FLAG(_object_mask_unit, owner->object.type))
	{
		real_rgb_color const *color = &owner->object.base_change_colors[_object_change_color_a];

		tint[0] = (unsigned char)PIN(color->red * 255.0f, 0.0f, 255.0f);
		tint[1] = (unsigned char)PIN(color->green * 255.0f, 0.0f, 255.0f);
		tint[2] = (unsigned char)PIN(color->blue * 255.0f, 0.0f, 255.0f);
	}
	tint[3] = camouflage(object);
	put_u16((unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index));
	put_u16((unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(definition->object.model.index));
	put_u8(tint[0]);
	put_u8(tint[1]);
	put_u8(tint[2]);
	put_u8(tint[3]);
	put_u8(MAXIMUM_REGIONS_PER_OBJECT);
	for (region = 0; region < MAXIMUM_REGIONS_PER_OBJECT; region++)
		put_u8(object->object.region_permutations[region]);
	put_f32(object->object.bounding_sphere_center.x);
	put_f32(object->object.bounding_sphere_center.y);
	put_f32(object->object.bounding_sphere_center.z);
	put_f32(object->object.bounding_sphere_radius);
	put_u8((unsigned long)node_count);
	put_u8(scaled ? 1 : 0);
	put_f32(nodes[0].position.x);
	put_f32(nodes[0].position.y);
	put_f32(nodes[0].position.z);
	for (node = 0; node < node_count; node++)
	{
		real q[4];

		quaternion(&nodes[node], q);
		put_unit(q[0]);
		put_unit(q[1]);
		put_unit(q[2]);
		put_unit(q[3]);
		put_fixed(nodes[node].position.x - nodes[0].position.x, 256.0f);
		put_fixed(nodes[node].position.y - nodes[0].position.y, 256.0f);
		put_fixed(nodes[node].position.z - nodes[0].position.z, 256.0f);
		if (scaled)
			put_u16((unsigned long)PIN(nodes[node].scale * 4096.0f, 0.0f, 65535.0f));
	}
	(void)out;
	return export.length - start;
}

static long send_objects(long tick)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long sent = 0, in_packet = 0, count_at;

	export.length = 0;
	put_u32((unsigned long)tick);
	count_at = export.length;
	put_u8(0);
	object_iterator_new(&iterator, _object_mask_all, FLAG(_object_header_visible_bit));
	while ((object = object_iterator_next(&iterator)) != NULL && sent < EXPORT_OBJECTS)
	{
		long before = export.length;

		if (TEST_FLAG(object->object.flags, _object_invisible_bit) || !object_wanted(iterator.index, object))
			continue;
		if (!object_record(iterator.index, object, export.packet, (long)sizeof(export.packet)))
		{
			/* (the packet is full: send it, the object in the next) */
			export.length = before;
			if (!in_packet)
				continue;
			export.packet[count_at] = (unsigned char)in_packet;
			link_cable_send(LINK_CABLE_OBJECTS, export.packet, (int)export.length);
			export.length = 0;
			put_u32((unsigned long)tick);
			put_u8(0);
			in_packet = 0;
			if (!object_record(iterator.index, object, export.packet, (long)sizeof(export.packet)))
			{
				export.length = 5;
				continue;
			}
		}
		in_packet++;
		sent++;
		if (in_packet == 255)
		{
			export.packet[count_at] = (unsigned char)in_packet;
			link_cable_send(LINK_CABLE_OBJECTS, export.packet, (int)export.length);
			export.length = 0;
			put_u32((unsigned long)tick);
			put_u8(0);
			in_packet = 0;
		}
	}
	if (in_packet)
	{
		export.packet[count_at] = (unsigned char)in_packet;
		link_cable_send(LINK_CABLE_OBJECTS, export.packet, (int)export.length);
	}
	return sent;
}

/* ---------- the game's messages (hud_messaging.c's hud_print_message,
HALO_LINK): who killed whom, who joined or left, the scores -- a local
player's, to the N64's HUD: MESSAGE, the local player (1), its length (1),
its UTF-16 code units (2 each) */

#define LINK_CABLE_MESSAGE 72
#define MESSAGE_LENGTH 63

void link_export_message(
	short local_player_index,
	wchar_t const *message_text)
{
	unsigned char payload[2 + 2 * MESSAGE_LENGTH];
	long length = 0, units = 0;

	if (local_player_index < 0 || local_player_index > 3 || !message_text || !link_cable_connected())
		return;
	payload[length++] = (unsigned char)local_player_index;
	length++;
	while (units < MESSAGE_LENGTH && message_text[units])
	{
		payload[length++] = (unsigned char)((unsigned short)message_text[units] >> 8);
		payload[length++] = (unsigned char)message_text[units];
		units++;
	}
	payload[1] = (unsigned char)units;
	link_cable_send(LINK_CABLE_MESSAGE, payload, (int)length);
}

/* ---------- each frame (main.c, after the cameras' update) */

/* ---------- what happens in the game, for the N64 to make again: the
effects begun (EFFECTS: weapons fired, impacts, explosions, blood, with
their sounds), the sounds that are not an effect's (SOUNDS), the
announcer, the hits the player takes (DAMAGE), and the HUD's view of the
game (HUD: the crosshair's target, the state message, the goals, the
motion sensor's blips). The N64's own player's -- its own weapon's fire,
its own shots' impacts on the map -- are left out: its own game makes
them. Effects and sounds wait for the next frame (at most EVENTS_* each);
DAMAGE goes at once.

  EFFECTS  count (1), each: the effect's tag (2: absolute index), where
           (3 x f32), its forward (3 x s16, x32767), scale a and b (1 + 1,
           x255)
  SOUNDS   count (1), each: the sound's tag (2: absolute index; with flag
           2 the announcer's sound index), where (3 x f32), scale (1, x255),
           flags (1: 1 heard where it is, 2 the announcer)
  DAMAGE   the local player (1), the damage effect's tag (2), its direction
           (3 x s16, x32767), scale and damage done (2 x f32), flags (1: 1
           the attacker known), the attacker's place (3 x f32)
  HUD      the local player (1), the autoaim level (1, x255: the crosshair
           red at 255), the name the crosshair shows (a string) and whether
           an enemy's (1), the state message (its length (1), UTF-16 code
           units), how goals are shown (1: the variant's goal_radar), the
           goals the player sees (count (1), each: where (3 x f32), the nav
           point (s16)), the motion sensor's blips (count (1), each: where
           (3 x f32), type and size (1 + 1)) */

#define LINK_CABLE_EFFECTS 74
#define LINK_CABLE_SOUNDS 75
#define LINK_CABLE_DAMAGE 76
#define LINK_CABLE_HUD 77
#define EVENTS_EFFECTS 32
#define EVENTS_SOUNDS 24
/* what happens farther than this from the eye is left out */
#define EVENT_RANGE 100.0f
#define HUD_GOALS 16
#define HUD_BLIPS 16

/* effects.c's: set while an effect plays a sound (the effect carries it) */
int link_effect_sound;

/* game_engine.c's, motion_sensor.c's and player_control.c's (HALO_LINK) */
long link_game_engine_goal_count(void);
boolean link_game_engine_visible_goal(long player_index, long goal_index, real_point3d *position, short *nav_index);
long link_game_engine_goal_radar(void);
long link_game_engine_target_player(long player_index);
int link_motion_sensor_blips(short local_player_index, real_point3d const *position, real_point3d *positions,
	char *types, char *sizes, int maximum);
struct player_control *player_control_get(short local_player_index);
boolean game_engine_get_state_message(long player_index, wchar_t *message, long message_character_count);

static struct
{
	int effect_count, sound_count;
	struct
	{
		unsigned short definition;
		real_point3d point;
		real_vector3d forward;
		unsigned char scale_a, scale_b;
	} effects[EVENTS_EFFECTS];
	struct
	{
		unsigned short definition;
		real_point3d point;
		unsigned char scale, flags;
	} sounds[EVENTS_SOUNDS];
} events;

static long own_unit(void)
{
	long player_index = local_player_get_player_index(0);

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

/* (the N64's own player's: its unit, what it holds or sits in -- when on
foot -- and its body) */
static boolean own_object(long object_index, long unit_index)
{
	return unit_index != NONE && object_index != NONE && object_try_and_get(object_index) &&
		object_get_ultimate_parent(object_index) == unit_index;
}

static boolean near_eye(real_point3d const *point)
{
	real dx, dy, dz;

	if (export.view_count <= 0)
		return TRUE;
	dx = point->x - export.eyes[0].x;
	dy = point->y - export.eyes[0].y;
	dz = point->z - export.eyes[0].z;
	return dx * dx + dy * dy + dz * dz < EVENT_RANGE * EVENT_RANGE;
}

static boolean playing(void)
{
	return link_cable_connected() && game_in_progress() && game_engine_running();
}

void link_export_effect(
	long definition_index,
	long owner_object_index,
	long object_index,
	real_point3d const *point,
	real_vector3d const *forward,
	real scale_a,
	real scale_b)
{
	long unit_index = own_unit();
	struct object_datum *owner = owner_object_index != NONE ? object_try_and_get(owner_object_index) : NULL;
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;
	boolean on_the_map = !object || TEST_FLAG(MAP_OBJECT_MASK, object->object.type);

	if (!playing() || events.effect_count == EVENTS_EFFECTS || definition_index == NONE)
		return;
	/* (the N64's own: on its unit or weapon; its detonations and its shots'
	impacts on the map -- on another unit or a vehicle they are shown) */
	if (own_object(object_index, unit_index))
		return;
	if (owner && unit_index != NONE && on_the_map)
	{
		if (owner_object_index == unit_index)
			return;
		if (owner->object.type == _object_type_projectile && owner->object.owner_object_index == unit_index)
			return;
	}
	if (!near_eye(point))
		return;
	events.effects[events.effect_count].definition = (unsigned short)DATUM_INDEX_TO_ABSOLUTE_INDEX(definition_index);
	events.effects[events.effect_count].point = *point;
	events.effects[events.effect_count].forward = *forward;
	events.effects[events.effect_count].scale_a = (unsigned char)PIN(scale_a * 255.0f, 0.0f, 255.0f);
	events.effects[events.effect_count].scale_b = (unsigned char)PIN(scale_b * 255.0f, 0.0f, 255.0f);
	events.effect_count++;
}

void link_export_sound(
	long definition_index,
	long object_index,
	real_point3d const *position,
	real scale)
{
	if (link_effect_sound || !playing() || events.sound_count == EVENTS_SOUNDS || definition_index == NONE)
		return;
	if (object_index != NONE && own_object(object_index, own_unit()))
		return;
	if (!near_eye(position))
		return;
	events.sounds[events.sound_count].definition = (unsigned short)DATUM_INDEX_TO_ABSOLUTE_INDEX(definition_index);
	events.sounds[events.sound_count].point = *position;
	events.sounds[events.sound_count].scale = (unsigned char)PIN(scale * 255.0f, 0.0f, 255.0f);
	events.sounds[events.sound_count].flags = 1;
	events.sound_count++;
}

void link_export_announcer(
	long sound_index)
{
	if (!playing() || events.sound_count == EVENTS_SOUNDS || sound_index < 0 || sound_index > 0xFFFF)
		return;
	events.sounds[events.sound_count].definition = (unsigned short)sound_index;
	events.sounds[events.sound_count].point = *global_origin3d;
	events.sounds[events.sound_count].scale = 255;
	events.sounds[events.sound_count].flags = 2;
	events.sound_count++;
}

void link_export_damage(
	short local_player_index,
	struct damage_data const *damage,
	real_vector3d const *direction,
	real scale,
	real total_damage)
{
	struct object_datum *attacker;
	real_point3d attacker_position = { 0.0f, 0.0f, 0.0f };

	if (local_player_index != 0 || !playing() || !damage || damage->definition_index == NONE)
		return;
	attacker = damage->owner_object_index != NONE ? object_try_and_get(damage->owner_object_index) : NULL;
	if (attacker)
		object_get_origin(damage->owner_object_index, &attacker_position);
	export.length = 0;
	put_u8((unsigned long)local_player_index);
	put_u16((unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(damage->definition_index));
	put_unit(direction->i);
	put_unit(direction->j);
	put_unit(direction->k);
	put_f32(scale);
	put_f32(total_damage);
	put_u8(attacker ? 1 : 0);
	put_f32(attacker_position.x);
	put_f32(attacker_position.y);
	put_f32(attacker_position.z);
	link_cable_send(LINK_CABLE_DAMAGE, export.packet, (int)export.length);
}

static void send_events(void)
{
	int i;

	if (events.effect_count)
	{
		export.length = 0;
		put_u8((unsigned long)events.effect_count);
		for (i = 0; i < events.effect_count; i++)
		{
			put_u16(events.effects[i].definition);
			put_f32(events.effects[i].point.x);
			put_f32(events.effects[i].point.y);
			put_f32(events.effects[i].point.z);
			put_unit(events.effects[i].forward.i);
			put_unit(events.effects[i].forward.j);
			put_unit(events.effects[i].forward.k);
			put_u8(events.effects[i].scale_a);
			put_u8(events.effects[i].scale_b);
		}
		link_cable_send(LINK_CABLE_EFFECTS, export.packet, (int)export.length);
	}
	if (events.sound_count)
	{
		export.length = 0;
		put_u8((unsigned long)events.sound_count);
		for (i = 0; i < events.sound_count; i++)
		{
			put_u16(events.sounds[i].definition);
			put_f32(events.sounds[i].point.x);
			put_f32(events.sounds[i].point.y);
			put_f32(events.sounds[i].point.z);
			put_u8(events.sounds[i].scale);
			put_u8(events.sounds[i].flags);
		}
		link_cable_send(LINK_CABLE_SOUNDS, export.packet, (int)export.length);
	}
	events.effect_count = events.sound_count = 0;
}

/* the HUD's view of the game, the first local player's */
static void send_hud(void)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
	struct player_control *control = player_control_get(0);
	long target_player_index = player ? link_game_engine_target_player(player_index) : NONE;
	wchar_t state[64];
	long goal_index, goals = link_game_engine_goal_count(), count_at, count = 0, unit_index;
	real_point3d positions[HUD_BLIPS], eye;
	char types[HUD_BLIPS], sizes[HUD_BLIPS];
	int blips = 0, i, length = 0;

	if (!player)
		return;
	export.length = 0;
	put_u8(0);
	put_u8((unsigned long)PIN(control->autoaim_level * 255.0f, 0.0f, 255.0f));
	if (target_player_index != NONE)
	{
		struct player_datum *target = player_get(target_player_index);
		char name[16];

		for (i = 0; i < 11 && target->name[i]; i++)
			name[i] = target->name[i] >= ' ' && target->name[i] <= '~' ? (char)target->name[i] : '?';
		name[i] = 0;
		put_string(name);
		put_u8(!game_engine_has_teams() || target->team_index != player->team_index ? 1 : 0);
	}
	else
	{
		put_u8(0);
		put_u8(0);
	}
	csmemset(state, 0, sizeof(state));
	if (game_engine_get_state_message(player_index, state, NUMBEROF(state)))
	{
		while (length < 63 && state[length])
			length++;
	}
	put_u8((unsigned long)length);
	for (i = 0; i < length; i++)
		put_u16((unsigned long)(unsigned short)state[i]);
	put_u8((unsigned long)link_game_engine_goal_radar());
	count_at = export.length;
	put_u8(0);
	for (goal_index = 0; goal_index < goals && count < HUD_GOALS; goal_index++)
	{
		real_point3d position;
		short nav_index;

		if (!link_game_engine_visible_goal(player_index, goal_index, &position, &nav_index))
			continue;
		put_f32(position.x);
		put_f32(position.y);
		put_f32(position.z);
		put_u16((unsigned long)(unsigned short)nav_index);
		count++;
	}
	export.packet[count_at] = (unsigned char)count;
	unit_index = player->unit_index;
	if (unit_index != NONE && object_try_and_get(unit_index))
	{
		unit_get_camera_position(unit_index, &eye);
		blips = link_motion_sensor_blips(0, &eye, positions, types, sizes, HUD_BLIPS);
	}
	put_u8((unsigned long)blips);
	for (i = 0; i < blips; i++)
	{
		put_f32(positions[i].x);
		put_f32(positions[i].y);
		put_f32(positions[i].z);
		put_u8((unsigned long)(unsigned char)types[i]);
		put_u8((unsigned long)(unsigned char)sizes[i]);
	}
	link_cable_send(LINK_CABLE_HUD, export.packet, (int)export.length);
}

/* ---------- each frame (main.c, after the cameras' update) */

void link_export_update(
	void)
{
	long tick;
	long sent;

	if (!link_cable_connected() || !game_in_progress() || game_engine_running() == FALSE)
	{
		events.effect_count = events.sound_count = 0;
		return;
	}
	tick = game_time_get();
	if (tick == export.last_tick)
		return;
	/* (one frame each time the N64 has drawn one: it sends INPUT then) */
	if (link_cable_input_count() == export.last_inputs)
		return;
	export.last_inputs = link_cable_input_count();
	export.last_tick = tick;
	send_frame(tick);
	sent = send_objects(tick);
	send_events();
	send_hud();
	export.length = 0;
	put_u32((unsigned long)tick);
	put_u16((unsigned long)sent);
	link_cable_send(LINK_CABLE_FRAME_END, export.packet, (int)export.length);
}

#endif
