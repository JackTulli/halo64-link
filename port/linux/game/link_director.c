/*
LINK_DIRECTOR.C

Halo 64 Link's director (link/README.md): what the Nintendo 64 on the cable
(link/platform/link_cable.c) asks for, done as the game's menus would, each
frame from main.c. Once the main menu is up and an N64 has said hello, the
Link searches for system link games, as Join Game does; LIST sends the N64
the games found (their ids their places in the client's list, plus one),
JOIN joins the one it chose, LEAVE leaves it. The N64's controller 1 is the
player (link/platform/link_input.c). The menu's line (STATUS) says what is
going on; when the game begins the N64 is told to load its map (LOAD), and
when it ends, ENDED.

Internet games (the game list's invites) and hosting come next.
*/

#ifdef HALO_LINK

#include "cseries.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"

#include "../../../link/platform/link_cable.h"

#include <stdio.h>
#include <string.h>

void platform_log(const char *format, ...);

/* network_client_manager.c's (HALO_LINK) */
boolean network_game_client_available_game(long index, char *name, long name_size, char *map, long map_size,
	short *engine_type, short *players, short *most);
boolean network_game_client_join_available_game(long index);
short network_game_client_state_number(void);

/* the client's states (network_client_manager.c) */
enum { CLIENT_SEARCHING, CLIENT_JOINING, CLIENT_PREGAME, CLIENT_INGAME, CLIENT_POSTGAME };

#define ADVERTISED_GAMES 32

enum link_director_state
{
	_link_waiting,   /* no N64 on the cable, or the menu not up yet */
	_link_searching, /* looking for games: LIST answered, JOIN taken */
	_link_joined     /* in a game's lobby, or its game */
};

static struct
{
	enum link_director_state state;
	short client_state;
	char joined_name[32], joined_map[32], joined_type[16];
	real seconds;
} director;

static char const *engine_name(short engine_type)
{
	switch (engine_type)
	{
	case game_engine_ctf: return "ctf";
	case game_engine_slayer: return "slayer";
	case game_engine_oddball: return "oddball";
	case game_engine_king: return "king";
	case game_engine_race: return "race";
	default: return "?";
	}
}

/* a string: its length (1), its bytes */
static long put_string(unsigned char *at, long room, char const *text)
{
	long length = (long)strlen(text);

	if (length > 255)
		length = 255;
	if (length + 1 > room)
		return 0;
	at[0] = (unsigned char)length;
	memcpy(at + 1, text, (size_t)length);
	return length + 1;
}

static void send_games(void)
{
	unsigned char payload[LINK_CABLE_PAYLOAD_MAXIMUM];
	long length = 1, index;
	int count = 0;

	for (index = 0; index < ADVERTISED_GAMES; index++)
	{
		char name[32], map[32];
		short engine_type, players, most;
		unsigned long id = (unsigned long)index + 1;

		if (!network_game_client_available_game(index, name, sizeof(name), map, sizeof(map), &engine_type, &players,
			&most))
		{
			continue;
		}
		if (length + 6 + 3 * 33 > (long)sizeof(payload))
			break;
		payload[length++] = (unsigned char)(id >> 24);
		payload[length++] = (unsigned char)(id >> 16);
		payload[length++] = (unsigned char)(id >> 8);
		payload[length++] = (unsigned char)id;
		payload[length++] = (unsigned char)players;
		payload[length++] = (unsigned char)most;
		length += put_string(payload + length, (long)sizeof(payload) - length, name);
		length += put_string(payload + length, (long)sizeof(payload) - length, map);
		length += put_string(payload + length, (long)sizeof(payload) - length, engine_name(engine_type));
		count++;
	}
	payload[0] = (unsigned char)count;
	link_cable_send(LINK_CABLE_GAMES, payload, (int)length);
}

/* as Join Game does: a client, searching; the N64's player joined to it */
static void start_searching(void)
{
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	if (create_global_network_game_client())
	{
		game_connection_set(_game_connection_network_client);
		player_ui_local_player_joined_multiplayer_game(0);
		director.state = _link_searching;
		link_cable_status("LOOKING FOR GAMES");
		platform_log("link: searching for games");
	}
	else
		link_cable_status("THE LINK COULD NOT LOOK FOR GAMES");
}

static void leave(void)
{
	dispose_global_network_game_client();
	dispose_global_network_game_server();
	ui_widgets_close_all();
	director.state = _link_waiting;
	platform_log("link: left the game");
}

static void take_requests(void)
{
	struct link_cable_request request;

	while (link_cable_take_request(&request))
	{
		switch (request.type)
		{
		case LINK_CABLE_LIST:
			if (director.state == _link_searching)
				send_games();
			break;
		case LINK_CABLE_JOIN:
			if (director.state == _link_searching && request.length >= 4)
			{
				unsigned long id = (unsigned long)request.payload[0] << 24 | (unsigned long)request.payload[1] << 16 |
					(unsigned long)request.payload[2] << 8 | request.payload[3];
				short engine_type = 0, players, most;

				director.joined_name[0] = director.joined_map[0] = 0;
				network_game_client_available_game((long)id - 1, director.joined_name, sizeof(director.joined_name),
					director.joined_map, sizeof(director.joined_map), &engine_type, &players, &most);
				snprintf(director.joined_type, sizeof(director.joined_type), "%s", engine_name(engine_type));
				if (id && network_game_client_join_available_game((long)id - 1))
				{
					char status[64];

					director.state = _link_joined;
					director.seconds = 0;
					ui_widgets_close_all();
					ui_widget_load_by_name_or_tag(
						"ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen",
						NONE, NULL, NONE, NONE, NONE, NONE);
					snprintf(status, sizeof(status), "JOINING %s", director.joined_name);
					link_cable_status(status);
					platform_log("link: joining game %lu (%s)", id, director.joined_name);
				}
				else
					link_cable_status("THAT GAME CAN'T BE JOINED NOW");
			}
			break;
		case LINK_CABLE_LEAVE:
			if (director.state == _link_joined)
			{
				leave();
				link_cable_send(LINK_CABLE_ENDED, NULL, 0);
			}
			break;
		case LINK_CABLE_HOST:
			link_cable_status("HOSTING FROM THE N64 COMES LATER");
			break;
		default:
			break;
		}
	}
}

void link_director_update(
	boolean main_menu_loaded,
	real seconds)
{
	short client_state;

	link_cable_update();
	if (!link_cable_connected())
	{
		if (director.state == _link_joined)
			leave();
		director.state = _link_waiting;
		return;
	}
	if (director.state == _link_waiting)
	{
		if (!main_menu_loaded)
			return;
		start_searching();
	}
	take_requests();
	client_state = network_game_client_state_number();
	if (director.state == _link_joined && client_state != director.client_state)
	{
		char status[64];

		switch (client_state)
		{
		case CLIENT_PREGAME:
			snprintf(status, sizeof(status), "IN %s'S LOBBY", director.joined_name);
			link_cable_status(status);
			break;
		case CLIENT_INGAME:
		{
			unsigned char payload[80];
			long length = 0;

			length += put_string(payload + length, (long)sizeof(payload) - length, director.joined_map);
			length += put_string(payload + length, (long)sizeof(payload) - length, director.joined_type);
			link_cable_send(LINK_CABLE_LOAD, payload, (int)length);
			link_cable_status("PLAYING");
			break;
		}
		case CLIENT_POSTGAME:
			link_cable_send(LINK_CABLE_ENDED, NULL, 0);
			link_cable_status("THE GAME IS OVER");
			break;
		default:
			break;
		}
		platform_log("link: the client's state is %d", client_state);
	}
	director.client_state = client_state;
	if (director.state == _link_joined)
	{
		director.seconds += seconds;
		/* (a join that came to nothing: back to looking) */
		if (client_state == CLIENT_SEARCHING && director.seconds > 10.0f)
		{
			link_cable_status("THE GAME DID NOT LET US IN");
			director.state = _link_searching;
		}
	}
}

#endif
