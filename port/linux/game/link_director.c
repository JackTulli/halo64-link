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

The internet's games come in the same list, after the local network's: the
game list's (network.browser_url, halo.milenko.org: browser.c), each id
INTERNET_ID plus its place in the list as last sent. Joining one joins its
invite, as Online Games does (browser_screen.c): the host's game is then
advertised to the client through the tunnel and joined. The list is sent
again whenever it changes while the Link is looking. A game on a map the
cartridge doesn't have (a Halo PC map, say) is listed, marked, and not
joined.

Hosting comes later.
*/

#ifdef HALO_LINK

#include "cseries.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_game_manager.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"

#include "../src/browser.h"
#include "../../../link/platform/link_cable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);

/* player_ui.c's (HALO_LINK) */
void player_ui_link_set_player_name(short local_player_index, wchar_t const *name);

/* network_client_manager.c's (HALO_LINK) */
boolean network_game_client_available_game(long index, char *name, long name_size, char *map, long map_size,
	short *engine_type, short *players, short *most);
boolean network_game_client_join_available_game(long index);
short network_game_client_state_number(void);

/* the client's states (network_client_manager.c) */
enum { CLIENT_SEARCHING, CLIENT_JOINING, CLIENT_PREGAME, CLIENT_INGAME, CLIENT_POSTGAME };

#define ADVERTISED_GAMES 32
/* the most games the N64 keeps (n64_link.h's N64_LINK_GAMES_MAXIMUM) */
#define SENT_GAMES 32
/* an internet game's id: this plus its place in director.listed */
#define INTERNET_ID 1000
/* the internet list looked at again this often while looking (browser.c asks
the server again when its list is older than its own interval) */
#define LIST_CHECK_SECONDS 2.0f
/* a listed game's host that hasn't answered by then won't */
#define CONNECT_SECONDS 20.0f

enum link_director_state
{
	_link_waiting,    /* no N64 on the cable, or the menu not up yet */
	_link_searching,  /* looking for games: LIST answered, JOIN taken */
	_link_connecting, /* an internet game's invite joined, its host not yet heard */
	_link_joined      /* in a game's lobby, or its game */
};

static struct
{
	enum link_director_state state;
	short client_state;
	char joined_name[32], joined_map[32], joined_type[16];
	real seconds;
	/* the internet games as last sent (their ids' places), and what was sent */
	struct browser_game listed[BROWSER_MAXIMUM_GAMES];
	int listed_count;
	unsigned long sent_hash;
	real list_seconds;
	char connecting_invite[BROWSER_INVITE_LENGTH + 1];
	/* the game begun, its LOAD not yet sent; the N64 told to load it */
	boolean load_pending, playing;
	real score_seconds;
	/* the main menu's map is the one loaded (this frame's) */
	boolean main_menu_loaded;
} director;

/* the maps on the online cartridge (halo-64's port.json "maps": the
multiplayer cartridge's), by their folders */
static char const *const cartridge_maps[] = {
	"wizard", "beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation",
	"hangemhigh", "longest", "prisoner", "putput", "ratrace", "sidewinder", NULL
};

/* a map's folder, as the cartridge names its maps: the last part of a
scenario's path ("levels\test\carousel\carousel"), lower case; a Halo PC
map keeps its family ("sneak@ce") */
static void map_folder(char const *map, char *folder, int size)
{
	char const *last = map;
	int length = 0;

	for (char const *c = map; *c; c++)
	{
		if (*c == '\\' || *c == '/')
			last = c + 1;
	}
	for (; last[length] && length < size - 1; length++)
		folder[length] = (char)(last[length] >= 'A' && last[length] <= 'Z' ? last[length] - 'A' + 'a' : last[length]);
	folder[length] = 0;
}

static boolean cartridge_map(char const *folder)
{
	for (int i = 0; cartridge_maps[i]; i++)
	{
		if (!strcmp(folder, cartridge_maps[i]))
			return TRUE;
	}
	return FALSE;
}

/* a listed game's name (UTF-16) as the N64's text has it */
static void ascii_name(unsigned short const *name, int length, char *out, int size)
{
	int kept = 0;

	for (int i = 0; i < length && name[i] && kept < size - 1; i++)
		out[kept++] = name[i] >= ' ' && name[i] <= '~' ? (char)name[i] : '?';
	out[kept] = 0;
}

/* the order the internet games are listed in: those the cartridge can play
and that take players first, then those with players in them, then by
name (not by how many: the list is sent again as players come and go, and
the N64's choice is a place in it) */
static int listed_order(void const *a, void const *b)
{
	struct browser_game const *x = a, *y = b;
	char folder_x[BROWSER_MAP_LENGTH], folder_y[BROWSER_MAP_LENGTH];
	int playable_x, playable_y;

	map_folder(x->map, folder_x, sizeof(folder_x));
	map_folder(y->map, folder_y, sizeof(folder_y));
	playable_x = cartridge_map(folder_x) && x->open;
	playable_y = cartridge_map(folder_y) && y->open;
	if (playable_x != playable_y)
		return playable_y - playable_x;
	if ((x->players > 0) != (y->players > 0))
		return (y->players > 0) - (x->players > 0);
	for (int i = 0; i < BROWSER_NAME_LENGTH; i++)
	{
		int cx = x->name[i] >= 'a' && x->name[i] <= 'z' ? x->name[i] - 32 : x->name[i];
		int cy = y->name[i] >= 'a' && y->name[i] <= 'z' ? y->name[i] - 32 : y->name[i];

		if (cx != cy || !cx)
			return cx - cy;
	}
	return strcmp(x->invite, y->invite);
}

static char const *engine_name(short engine_type)
{
	switch (engine_type)
	{
	case game_engine_ctf: return "ctf";
	case game_engine_slayer: return "slayer";
	case game_engine_oddball: return "oddball";
	case game_engine_king: return "king";
	case game_engine_race: return "race";
	default: return "other";
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

/* a game's line in GAMES (the format n64_link.h's N64_LINK_GAMES says) */
static long put_game(unsigned char *payload, long length, unsigned long id, short players, short most,
	char const *name, char const *map, char const *type)
{
	payload[length++] = (unsigned char)(id >> 24);
	payload[length++] = (unsigned char)(id >> 16);
	payload[length++] = (unsigned char)(id >> 8);
	payload[length++] = (unsigned char)id;
	payload[length++] = (unsigned char)PIN(players, 0, 255);
	payload[length++] = (unsigned char)PIN(most, 0, 255);
	length += put_string(payload + length, LINK_CABLE_PAYLOAD_MAXIMUM - length, name);
	length += put_string(payload + length, LINK_CABLE_PAYLOAD_MAXIMUM - length, map);
	length += put_string(payload + length, LINK_CABLE_PAYLOAD_MAXIMUM - length, type);
	return length;
}

/* the games, the local network's then the internet's; sent if asked
(force), else only if they changed since the last list sent */
static void send_games(boolean force)
{
	static struct browser_game games[BROWSER_MAXIMUM_GAMES];
	unsigned char payload[LINK_CABLE_PAYLOAD_MAXIMUM];
	long length = 1, index;
	int count = 0, internet;
	unsigned long hash = 5381;

	for (index = 0; index < ADVERTISED_GAMES && count < SENT_GAMES; index++)
	{
		char name[32], map[32], folder[32];
		short engine_type, players, most;

		if (!network_game_client_available_game(index, name, sizeof(name), map, sizeof(map), &engine_type, &players,
			&most))
		{
			continue;
		}
		if (length + 6 + 3 * 33 > (long)sizeof(payload))
			break;
		map_folder(map, folder, sizeof(folder));
		length = put_game(payload, length, (unsigned long)index + 1, players, most, name, folder,
			cartridge_map(folder) ? engine_name(engine_type) : "NOT ON CART");
		count++;
	}
	/* (the internet's: those a game of this version can join, browser.c) */
	internet = browser_get_games(games, BROWSER_MAXIMUM_GAMES);
	qsort(games, (size_t)internet, sizeof(games[0]), listed_order);
	director.listed_count = 0;
	for (index = 0; index < internet && count < SENT_GAMES; index++)
	{
		struct browser_game const *game = &games[index];
		char name[BROWSER_NAME_LENGTH + 1], folder[BROWSER_MAP_LENGTH], type[24];

		if (length + 6 + 3 * 33 > (long)sizeof(payload))
			break;
		ascii_name(game->name, BROWSER_NAME_LENGTH, name, sizeof(name));
		map_folder(game->map, folder, sizeof(folder));
		if (!cartridge_map(folder))
			snprintf(type, sizeof(type), "NOT ON CART");
		else if (!game->open)
			snprintf(type, sizeof(type), "CLOSED");
		else
			snprintf(type, sizeof(type), "%s%s", game->teams ? "team " : "", engine_name(game->engine));
		director.listed[director.listed_count] = *game;
		length = put_game(payload, length, INTERNET_ID + (unsigned long)director.listed_count, game->players,
			game->maximum_players, name, folder, type);
		director.listed_count++;
		count++;
	}
	payload[0] = (unsigned char)count;
	for (index = 0; index < length; index++)
		hash = hash * 33 + payload[index];
	if (!force && hash == director.sent_hash)
		return;
	director.sent_hash = hash;
	link_cable_send(LINK_CABLE_GAMES, payload, (int)length);
}

/* ---------- the players of the game joined: SCORES (halo-64's n64_link.h
says the layout), sent when they change, at most once a second, while the
Link is in a game or its lobby, and once when it leaves (not joined) */

#define LINK_CABLE_SCORES 73
#define SCORE_ROWS 16
#define SCORE_SECONDS 1.0f

struct score_row
{
	char name[16], score[12];
	short kills, deaths;
	signed char team;
	unsigned char self;
	long sort;
};

static void ascii_wide(wchar_t const *text, int length, char *out, int size)
{
	int kept = 0;

	for (int i = 0; i < length && text[i] && kept < size - 1; i++)
		out[kept++] = text[i] >= ' ' && text[i] <= '~' ? (char)text[i] : '?';
	out[kept] = 0;
}

/* (the leading team first when there are teams, then the best score) */
static boolean score_ahead(struct score_row const *a, struct score_row const *b, boolean teams, long first_team)
{
	if (teams && a->team != b->team)
		return a->team == first_team;
	if (a->sort != b->sort)
		return a->sort > b->sort;
	return a->kills > b->kills;
}

static void send_scores(boolean joined, boolean force)
{
	static unsigned long sent_hash;
	unsigned char payload[LINK_CABLE_PAYLOAD_MAXIMUM];
	struct score_row rows[SCORE_ROWS];
	struct network_game_client *client = global_network_game_client_get();
	struct network_game *network_game = joined && client ? network_game_client_get_game(client) : NULL;
	char game[24] = "", map[32] = "", type[24] = "";
	boolean teams = FALSE, in_game = joined && game_in_progress() && game_engine_running();
	long red = 0, blue = 0, first_team = 0, length = 0, index;
	int count = 0, total = 0;
	unsigned long hash = 5381;

	if (network_game)
	{
		ascii_wide(network_game->name, 16, game, sizeof(game));
		map_folder(network_game->map.name, map, sizeof(map));
		teams = network_game->variant.universal_variant.teams;
		snprintf(type, sizeof(type), "%s%s", teams ? "team " : "", engine_name((short)network_game->variant.game_engine_index));
	}
	if (in_game)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		teams = game_engine_has_teams();
		if (teams && game_engine->get_player_score)
		{
			red = game_engine_get_team_score(0);
			blue = game_engine_get_team_score(1);
			first_team = blue > red ? 1 : 0;
		}
		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL)
		{
			struct score_row row;
			wchar_t score_text[32];
			int at;

			total++;
			memset(&row, 0, sizeof(row));
			ascii_wide(player->name, 12, row.name, sizeof(row.name));
			memset(score_text, 0, sizeof(score_text));
			if (game_engine->format_player_score)
				game_engine->format_player_score(iterator.datum_index, score_text);
			ascii_wide(score_text, 31, row.score, sizeof(row.score));
			row.kills = player->statistics.kills[0];
			row.deaths = player->statistics.deaths;
			row.team = teams ? (signed char)(player->team_index & 1) : -1;
			row.self = player->local_player_index == 0;
			row.sort = game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, _get_score_individual) : row.kills;
			/* (sorted in: the best first; past the rows kept, dropped) */
			for (at = count; at > 0 && score_ahead(&row, &rows[at - 1], teams, first_team); at--)
			{
				if (at < SCORE_ROWS)
					rows[at] = rows[at - 1];
			}
			if (at < SCORE_ROWS)
			{
				rows[at] = row;
				if (count < SCORE_ROWS)
					count++;
			}
		}
	}
	else if (network_game)
	{
		/* (the lobby: who is in it) */
		for (index = 0; index < network_game->player_count; index++)
		{
			struct network_player const *player = &network_game->players[index];

			total++;
			if (count == SCORE_ROWS)
				continue;
			memset(&rows[count], 0, sizeof(rows[count]));
			ascii_wide(player->name, 12, rows[count].name, sizeof(rows[count].name));
			rows[count].team = teams ? (signed char)(player->team_index & 1) : -1;
			count++;
		}
	}
	payload[length++] = (unsigned char)((teams ? 1 : 0) | (in_game ? 2 : 0) | (joined ? 4 : 0));
	payload[length++] = (unsigned char)((unsigned long)red >> 8);
	payload[length++] = (unsigned char)red;
	payload[length++] = (unsigned char)((unsigned long)blue >> 8);
	payload[length++] = (unsigned char)blue;
	length += put_string(payload + length, (long)sizeof(payload) - length, game);
	length += put_string(payload + length, (long)sizeof(payload) - length, map);
	length += put_string(payload + length, (long)sizeof(payload) - length, type);
	payload[length++] = (unsigned char)PIN(total, 0, 255);
	payload[length++] = (unsigned char)count;
	for (index = 0; index < count; index++)
	{
		length += put_string(payload + length, (long)sizeof(payload) - length, rows[index].name);
		length += put_string(payload + length, (long)sizeof(payload) - length, rows[index].score);
		payload[length++] = (unsigned char)((unsigned short)rows[index].kills >> 8);
		payload[length++] = (unsigned char)rows[index].kills;
		payload[length++] = (unsigned char)((unsigned short)rows[index].deaths >> 8);
		payload[length++] = (unsigned char)rows[index].deaths;
		payload[length++] = (unsigned char)rows[index].team;
		payload[length++] = rows[index].self;
	}
	for (index = 0; index < length; index++)
		hash = hash * 33 + payload[index];
	if (!force && hash == sent_hash)
		return;
	sent_hash = hash;
	link_cable_send(LINK_CABLE_SCORES, payload, (int)length);
}

/* an internet game joined (its invite) as the list's place it had */
static void join_internet_game(unsigned long place)
{
	struct browser_game const *game;
	char folder[BROWSER_MAP_LENGTH], name[BROWSER_NAME_LENGTH + 1];

	if (place >= (unsigned long)director.listed_count)
	{
		link_cable_status("THAT GAME IS GONE: Z LOOKS AGAIN");
		return;
	}
	game = &director.listed[place];
	map_folder(game->map, folder, sizeof(folder));
	ascii_name(game->name, BROWSER_NAME_LENGTH, name, sizeof(name));
	if (!cartridge_map(folder))
	{
		link_cable_status("THAT GAME'S MAP IS NOT ON THE CARTRIDGE");
		return;
	}
	if (!game->open)
	{
		link_cable_status("THAT GAME IS NOT TAKING PLAYERS");
		return;
	}
	if (!browser_join(game->invite))
	{
		link_cable_status("INTERNET PLAY IS OFF IN THE LINK (HALO_NET_ONLINE)");
		return;
	}
	snprintf(director.joined_name, sizeof(director.joined_name), "%s", name);
	snprintf(director.joined_map, sizeof(director.joined_map), "%s", folder);
	snprintf(director.joined_type, sizeof(director.joined_type), "%s", engine_name(game->engine));
	snprintf(director.connecting_invite, sizeof(director.connecting_invite), "%s", game->invite);
	director.state = _link_connecting;
	director.seconds = 0;
	{
		char status[64];

		snprintf(status, sizeof(status), "CONNECTING TO %s", name);
		link_cable_status(status);
	}
	platform_log("link: joining internet game %s (%s)", name, folder);
}

/* the game joined: its lobby, as Join Game's */
static void joined(void)
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
	/* (from a game: back to the main menu, as the game's own Quit goes --
	its map unloaded, the main menu's disposing the client again -- and
	looking for games again once it is up, link_director_update. Without it
	the Link stayed in the game's map, waiting for a main menu that never
	came, and answered none of the N64's requests after) */
	if (!director.main_menu_loaded)
		main_goto_main_menu();
	director.state = _link_waiting;
	director.playing = director.load_pending = FALSE;
	send_scores(FALSE, TRUE);
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
			if (director.state == _link_searching || director.state == _link_connecting)
				send_games(TRUE);
			break;
		case LINK_CABLE_JOIN:
			if ((director.state == _link_searching || director.state == _link_connecting) && request.length >= 4)
			{
				unsigned long id = (unsigned long)request.payload[0] << 24 | (unsigned long)request.payload[1] << 16 |
					(unsigned long)request.payload[2] << 8 | request.payload[3];
				short engine_type = 0, players, most;
				char map[32];

				director.state = _link_searching;
				if (id >= INTERNET_ID)
				{
					join_internet_game(id - INTERNET_ID);
					break;
				}
				director.joined_name[0] = map[0] = 0;
				network_game_client_available_game((long)id - 1, director.joined_name, sizeof(director.joined_name),
					map, sizeof(map), &engine_type, &players, &most);
				map_folder(map, director.joined_map, sizeof(director.joined_map));
				snprintf(director.joined_type, sizeof(director.joined_type), "%s", engine_name(engine_type));
				if (map[0] && !cartridge_map(director.joined_map))
					link_cable_status("THAT GAME'S MAP IS NOT ON THE CARTRIDGE");
				else if (id && network_game_client_join_available_game((long)id - 1))
				{
					joined();
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

	director.main_menu_loaded = main_menu_loaded;
	link_cable_update();
	if (!link_cable_connected())
	{
		if (director.state == _link_joined || director.state == _link_connecting)
			leave();
		director.state = _link_waiting;
		return;
	}
	/* (the N64 player's name, the profile's: the name the next game joined
	gives the player) */
	{
		char name[16];
		wchar_t wide[12];
		int i;

		link_cable_name(name, sizeof(name));
		for (i = 0; i < 11 && name[i]; i++)
			wide[i] = (wchar_t)(unsigned char)name[i];
		wide[i] = 0;
		player_ui_link_set_player_name(0, wide);
	}
	if (director.state == _link_waiting)
	{
		if (!main_menu_loaded)
			return;
		start_searching();
	}
	take_requests();
	/* (looking: the list sent again when it changes, the internet's as the
	game list's server has it) */
	if (director.state == _link_searching || director.state == _link_connecting)
	{
		director.list_seconds += seconds;
		if (director.list_seconds >= LIST_CHECK_SECONDS)
		{
			director.list_seconds = 0;
			send_games(FALSE);
		}
	}
	/* (an internet game's invite joined: its game joined once its host
	advertises it through the tunnel, as Online Games does) */
	if (director.state == _link_connecting)
	{
		long joined_host = network_game_client_join_invite_host(director.connecting_invite);

		director.seconds += seconds;
		if (joined_host > 0)
		{
			joined();
			platform_log("link: joined internet game %s", director.joined_name);
		}
		else if (joined_host < 0)
		{
			director.state = _link_searching;
			link_cable_status("THAT GAME CAN'T BE JOINED FROM THIS VERSION");
		}
		else if (director.seconds > CONNECT_SECONDS)
		{
			director.state = _link_searching;
			link_cable_status("THE HOST DID NOT ANSWER");
			platform_log("link: internet game %s did not answer", director.joined_name);
		}
	}
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
			/* (LOAD once the map is loaded here: below) */
			director.load_pending = TRUE;
			link_cable_status("THE GAME IS STARTING");
			break;
		case CLIENT_POSTGAME:
			if (director.playing)
				link_cable_send(LINK_CABLE_ENDED, NULL, 0);
			director.playing = director.load_pending = FALSE;
			link_cable_status("THE GAME IS OVER");
			break;
		default:
			/* (the connection lost: the N64's game ended, back to looking) */
			if (client_state < 0)
			{
				if (director.playing)
					link_cable_send(LINK_CABLE_ENDED, NULL, 0);
				director.playing = director.load_pending = FALSE;
				send_scores(FALSE, TRUE);
				link_cable_status("THE CONNECTION TO THE GAME WAS LOST");
				director.client_state = client_state;
				start_searching();
				return;
			}
			break;
		}
		platform_log("link: the client's state is %d", client_state);
	}
	director.client_state = client_state;
	/* (the game's players, to the N64: its lobby, its scoreboard) */
	if (director.state == _link_joined)
	{
		director.score_seconds += seconds;
		if (director.score_seconds >= SCORE_SECONDS)
		{
			director.score_seconds = 0;
			send_scores(TRUE, FALSE);
		}
	}
	/* the game begun and its map loaded here: the N64 told to load it, with
	whether it has teams and the player's team (n64_online_game.c plays a
	game of its own on it) */
	if (director.state == _link_joined && director.load_pending && client_state == CLIENT_INGAME &&
		game_in_progress() && game_engine_running())
	{
		unsigned char payload[80];
		long length = 0, player_index = local_player_get_player_index(0);

		/* (the map being played -- a playlist's next game is another -- by its
		scenario's name: main_get_map_name is the campaign's) */
		if (global_scenario_index != NONE && tag_get_name(global_scenario_index))
			map_folder(tag_get_name(global_scenario_index), director.joined_map, sizeof(director.joined_map));
		else if (main_get_multiplayer_map_name() && main_get_multiplayer_map_name()[0])
			map_folder(main_get_multiplayer_map_name(), director.joined_map, sizeof(director.joined_map));
		length += put_string(payload + length, (long)sizeof(payload) - length, director.joined_map);
		length += put_string(payload + length, (long)sizeof(payload) - length, director.joined_type);
		payload[length++] = game_engine_has_teams() ? 1 : 0;
		payload[length++] = (unsigned char)(player_index != NONE ? player_get(player_index)->team_index & 0xFF : 0);
		link_cable_send(LINK_CABLE_LOAD, payload, (int)length);
		link_cable_status("PLAYING");
		director.load_pending = FALSE;
		director.playing = TRUE;
		platform_log("link: the N64 loads %s (teams %d)", director.joined_map, (int)game_engine_has_teams());
	}
	if (director.state == _link_joined)
	{
		director.seconds += seconds;
		/* (a join that came to nothing: back to looking) */
		if (client_state == CLIENT_SEARCHING && director.seconds > 10.0f)
		{
			link_cable_status("THE GAME DID NOT LET US IN");
			director.state = _link_searching;
			send_scores(FALSE, TRUE);
		}
	}
}

#endif
