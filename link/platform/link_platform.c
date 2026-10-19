/*
LINK_PLATFORM.C

Halo 64 Link's platform layer (link/README.md), in place of sdl_platform.c:
the same functions, for a game that has no window, no display and no sound
device, whose players sit at a Nintendo 64 (link_input.c). The Link build
(tools/link_build.py) links this instead of SDL. Adapted from the dedicated
server's (server/platform/server_platform.c); unlike the server, the Link is
a CLIENT: it joins (and later hosts) games as a player's copy of the game
does, so it has no playlist, no control API and no director.

What the game asks of a window it gets as from a game whose window is not
there yet: no video (d3d8_gl.c draws nothing: debug.null_renderer is set),
no clipboard, no message boxes (a message is logged). Stopping is SIGTERM's
or SIGINT's (Ctrl+C): the next frame's event pump ends the program the way
the game ends when its window is closed (exit, whose handlers withdraw what
the game listed); a main loop that has not got there 5 seconds later (held
in a blocking read) is ended by SIGALRM, and a second signal ends it at
once.

With nothing drawn, no display's refresh paces the main loop: link_input.c
calls link_platform_pace once a frame (its read of the first controller),
which holds the loop to 60 frames a second, twice the game's ticks, as the
dedicated server's director does (server/src/dedicated.c).

Before the game starts (a constructor, so nothing has been loaded yet), the
program checks what it was given: --version and --help answer and exit; a
run with HALO_DEDICATED (a playlist: the dedicated server's) is refused; a
data folder without maps is a clear error (exit status 1).
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "posix.h"
#include "browser.h"
#include "halo_port_limits.h"
#include "delta.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* (given for this file by the build, as for updater.c: tools/link_build.py) */
#ifndef HALO_VERSION
#define HALO_VERSION "dev"
#endif

/* a frame at most this often (60 a second): link_platform_pace */
#define LINK_FRAME_NANOSECONDS 16666667LL
/* a stop asked for that has not happened this much later happens at once */
#define LINK_STOP_SECONDS 5

/* SIGTERM or SIGINT arrived: the next pump stops the Link */
static volatile sig_atomic_t stop_requested;

/* ---------- start-up checks */

static void print_version(void)
{
	printf("%s, network version %d\n", build_identity(), delta_legacy_announce());
}

static void print_usage(FILE *stream)
{
	fprintf(stream,
		"Usage: halo64-link\n"
		"\n"
		"Halo 64 Link, the computer side of the Nintendo 64 port's online play: the\n"
		"game itself, without a window or sound, as a player's machine in system link\n"
		"and internet games. Its players are the Nintendo 64's (for now: port 1's\n"
		"controller from HALO_LINK_INPUT). The data folder (HALO_DATA_ROOT, else the\n"
		"current folder or the program's) holds maps/ (ui.map and the multiplayer\n"
		"maps); its debug.txt is the game's log. Settings are config.toml's, beside\n"
		"the program, and the environment's.\n"
		"\n"
		"Settings (environment variables):\n"
		"  HALO_LINK_INPUT     port 1's controller (default: debug.test_input's, else idle):\n"
		"                        idle             no input\n"
		"                        walk             walks forward, turning: a circle\n"
		"                        bot[:<seed>]     debug.test_input's scripted player\n"
		"                        look[:<seed>]    stands, turning and looking about\n"
		"                        menu:<buttons>   a button a second (a, b, x, y, start...)\n"
		"                        pipe:<path>      lines of controller states read from a\n"
		"                                         file or named pipe (link/README.md)\n"
		"  HALO_NETWORK_TEST   join: joins the first system link game found\n"
		"  HALO_NET_ADDRESS    the address to play from (system link)\n"
		"  HALO_NET_BROADCAST  where system link searches go\n"
		"  HALO_DATA_ROOT, HALO_SAVE_ROOT  the data folder, and where saves go\n"
		"\n"
		"  --version  prints the version\n"
		"  --help     prints this\n"
		"\n"
		"More: link/README.md\n");
}

static BOOL readable(const char *path)
{
	FILE *file = fopen(path, "r");

	if (!file)
		return FALSE;
	fclose(file);
	return TRUE;
}

/* the data folder, before anything loads from it */
static void link_check_data(void)
{
	const char *root = platform_data_root();
	char path[1024];

	platform_translate_path("d:\\maps\\ui.map", path, sizeof(path));
	if (!readable(path))
	{
		fprintf(stderr,
			"halo64-link: no maps: %s/maps/ui.map is missing.\n"
			"Put ui.map and the multiplayer maps (the Xbox's) in the data folder's maps\n"
			"folder, or point HALO_DATA_ROOT at the folder that holds maps/ (link/README.md).\n", root);
		platform_log("no maps: %s/maps/ui.map is missing", root);
		exit(EXIT_FAILURE);
	}
}

static void stop_handler(int signal_number)
{
	static const char forced[] = "halo64-link: stopped at once (a second signal)\n";

	(void)signal_number;
	if (stop_requested)
	{
		(void)!write(STDERR_FILENO, forced, sizeof(forced) - 1);
		_exit(EXIT_FAILURE);
	}
	stop_requested = 1;
	/* (a main loop held up meanwhile, in a blocking read say, is ended by
	SIGALRM's default action) */
	alarm(LINK_STOP_SECONDS);
}

/* after the Xbox address space is reserved (xbox_memory.c, priority 101),
before the game starts */
__attribute__((constructor(102)))
static void link_arguments(void)
{
	struct sigaction action;
	char argument[256];
	int index;

	for (index = 1; posix_command_line_argument(index, argument, sizeof(argument)); index++)
	{
		if (!strcmp(argument, "--version") || !strcmp(argument, "-v"))
		{
			print_version();
			exit(EXIT_SUCCESS);
		}
		if (!strcmp(argument, "--help") || !strcmp(argument, "-h"))
		{
			print_version();
			printf("\n");
			print_usage(stdout);
			exit(EXIT_SUCCESS);
		}
		fprintf(stderr, "halo64-link: unknown argument %s\n\n", argument);
		print_usage(stderr);
		exit(2);
	}
	/* (a playlist is the dedicated server's: the Link is a player's machine) */
	if (browser_dedicated())
	{
		fprintf(stderr, "halo64-link: HALO_DEDICATED is the dedicated server's (chupathingyce-server); "
			"the Link joins and hosts games as a player's copy does\n");
		exit(2);
	}
	/* (no sound to open: dsound_sdl.c's mixer runs on its own clock; and
	nothing to draw: d3d8_gl.c opens no window, hud_hires.c decodes nothing,
	p2p.c registers no link scheme, the player's own copy stays their
	program for links) */
	setenv("HALO_NO_AUDIO", "1", 0);
	setenv("HALO_NULL_RENDERER", "1", 0);
	/* which program this is, first in its log (build_identity.c) */
	build_identity_log();
	link_check_data();
	/* (the legacy table: a newer one fetched meanwhile) */
	delta_legacy_start();
	/* a write to a connection the other end closed fails instead of ending
	the Link (as sdl_platform.c); SIGTERM and SIGINT stop it at the next
	frame */
	signal(SIGPIPE, SIG_IGN);
	memset(&action, 0, sizeof(action));
	action.sa_handler = stop_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
}

/* ---------- the platform layer's start (all done above) */

BOOL platform_sdl_initialize(void)
{
	return TRUE;
}

/* no disc image to offer: the Link is given its maps */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return FALSE;
}

/* as sdl_platform.c's (display.interpolation, on unless set off): the game's
main loop then runs unthrottled by the Xbox's 60 Hz display, which with no
display to wait for would spin; link_platform_pace paces it */
int halo_interpolation_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.interpolation");
	}
	return enabled;
}

/* ---------- frame pacing */

static long long monotonic_nanoseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* once a frame (link_input.c, the first controller's read): the frame waits
for its turn, 60 a second; a frame that ran late starts the count over
rather than running the next ones back to back */
void link_platform_pace(void)
{
	static long long next_frame;
	long long now = monotonic_nanoseconds();

	if (next_frame && now < next_frame)
	{
		struct timespec wait;
		long long remaining = next_frame - now;

		wait.tv_sec = (time_t)(remaining / 1000000000LL);
		wait.tv_nsec = (long)(remaining % 1000000000LL);
		while (nanosleep(&wait, &wait) == -1 && errno == EINTR && !stop_requested)
			;
		next_frame += LINK_FRAME_NANOSECONDS;
	}
	else
		next_frame = now + LINK_FRAME_NANOSECONDS;
}

/* ---------- no window */

BOOL platform_screen_mode(long *width, long *height)
{
	(void)width;
	(void)height;
	return FALSE;
}

int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	(void)width;
	(void)height;
	return FALSE;
}

void platform_display_apply(void)
{
}

void platform_video_drawable_size(int *width, int *height)
{
	*width = 640;
	*height = 480;
}

void platform_video_window_size(int *width, int *height)
{
	*width = 640;
	*height = 480;
}

void platform_video_swap(void)
{
}

void platform_mouse_capture(BOOL capture)
{
	(void)capture;
}

/* ---------- no keyboard, mouse or clipboard at the machine */

BOOL platform_next_keystroke(struct platform_keystroke *keystroke)
{
	(void)keystroke;
	return FALSE;
}

int platform_clipboard_get(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

void platform_clipboard_set(const char *text)
{
	(void)text;
}

void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
}

void platform_open_url(const char *url)
{
	(void)url;
}

void platform_request_quit(void)
{
	stop_requested = 1;
}

void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	if (notches)
		*notches = 0;
	if (pages)
		*pages = 0;
}

void platform_menus_set_active(BOOL active)
{
	(void)active;
}

void platform_binding_capture_begin(void)
{
}

int platform_binding_capture_poll(int *input)
{
	(void)input;
	return 3;
}

void platform_ui_pointer_set_active(BOOL active)
{
	(void)active;
}

BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
	memset(pointer, 0, sizeof(*pointer));
	return FALSE;
}

void platform_input_read(struct platform_input_state *state, BOOL consume_motion)
{
	(void)consume_motion;
	memset(state, 0, sizeof(*state));
}

/* ---------- events */

/* every frame (link_input.c's XInputGetState, d3d8_gl.c's Present): stops
as asked */
void platform_pump_events(void)
{
	if (stop_requested)
	{
		platform_log("link: stopping");
		exit(EXIT_SUCCESS);
	}
}

/* ---------- the version (updater.c's, which the Link leaves out) */

void updater_start(void)
{
}

const char *updater_version(void)
{
	return HALO_VERSION;
}
