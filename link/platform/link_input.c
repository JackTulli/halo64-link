/*
LINK_INPUT.C

Halo 64 Link's controllers (link/README.md), in place of xinput_sdl.c: the
players sit at the Nintendo 64, not at the computer. Adapted from the
dedicated server's (server/platform/server_input.c): the first port holds a
controller, the debug keyboard is there but never types, no other port has
one (yet: the Nintendo 64's four ports will be the Link's four, for split
screen). Reading the first controller, every frame, is also when the
platform layer pumps its events and paces the frame (link_platform.c).

Until the Nintendo 64 sends its controllers (link_usb.c, the next stage),
port 1's controller comes from HALO_LINK_INPUT, else debug.test_input:

- "idle" (or nothing): a controller that never moves, as the server's;
- "walk": walks forward at three quarters, turning right: a circle;
- "bot[:<seed>]", "look[:<seed>]", "menu:<buttons>": debug.test_input's
  scripted players (xinput_sdl.c, port/linux/NETCODE.md "Testing"): the bot
  walks and strafes in circles, turns, fires, jumps and throws grenades;
  look stands still, turning and looking up and down; menu presses a
  button a second (a, b, x, y, lb, rb, up, down, left, right, start, back,
  wait);
- "pipe:<path>": controller states, a line each, read as they arrive from
  a file or a named pipe (mkfifo): the line's state holds until the next.
  A line is the whole state, as words: lx=<n> ly=<n> rx=<n> ry=<n> (sticks,
  -32768 to 32767), a=<n> b=<n> x=<n> y=<n> black=<n> white=<n> lt=<n>
  rt=<n> (analog buttons, 0 to 255; a word alone is 255), up down left
  right start back lthumb rthumb (held when there); anything left out is
  released; "#" starts a comment. Example: "ly=32000 rx=8000 rt".
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PORT_COUNT 4

/* link_platform.c's */
void link_platform_pace(void);

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static struct controller keyboard_device;
static DWORD reported_gamepads;
static BOOL reported_keyboard;

/* ---------- port 1's source */

enum
{
	_source_idle,
	_source_walk,
	_source_bot,
	_source_look,
	_source_menu,
	_source_pipe,
};

static struct
{
	BOOL checked;
	int source;
	int seed;
	char setting[512];
	double menu_since;
	/* the action button held (network_test.c: picking up, getting in) */
	BOOL holding_action;
	double holding_action_since;
	/* pipe: the file, the partial line read so far and the last full state */
	int pipe;
	char pipe_line[512];
	size_t pipe_length;
	XINPUT_GAMEPAD pipe_state;
} link_input = { FALSE, _source_idle, 0, "", 0.0, FALSE, 0.0, -1, "", 0, { 0 } };

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void source_choose(void)
{
	const char *setting = getenv("HALO_LINK_INPUT");

	link_input.checked = TRUE;
	if (!setting || !setting[0])
		setting = config_string("debug.test_input");
	snprintf(link_input.setting, sizeof(link_input.setting), "%s", setting ? setting : "");
	setting = link_input.setting;
	if (!strncmp(setting, "bot", 3) && (!setting[3] || setting[3] == ':'))
	{
		link_input.source = _source_bot;
		link_input.seed = setting[3] ? atoi(setting + 4) : 0;
	}
	else if (!strncmp(setting, "look", 4) && (!setting[4] || setting[4] == ':'))
	{
		link_input.source = _source_look;
		link_input.seed = setting[4] ? atoi(setting + 5) : 0;
	}
	else if (!strncmp(setting, "menu:", 5))
	{
		link_input.source = _source_menu;
		link_input.menu_since = seconds_now();
	}
	else if (!strcmp(setting, "walk"))
		link_input.source = _source_walk;
	else if (!strncmp(setting, "pipe:", 5))
	{
		link_input.source = _source_pipe;
		/* (read and write, so that a named pipe opens without a writer and
		never reads as ended while its writer comes and goes) */
		link_input.pipe = open(setting + 5, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (link_input.pipe < 0)
			link_input.pipe = open(setting + 5, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (link_input.pipe < 0)
			platform_log("link: cannot open %s for port 1's controller (%s): it stays idle", setting + 5,
				strerror(errno));
	}
	else if (setting[0] && strcmp(setting, "idle"))
		platform_log("link: HALO_LINK_INPUT \"%s\" is not a source; port 1's controller stays idle", setting);
	platform_log("link: port 1's controller: %s", link_input.source == _source_idle ? "idle" : setting);
}

/* the automated tests (port/linux/game/network_test.c): the scripted player
stands still, holding the action button (X: picking up, swapping weapons)
after a second */
void test_input_hold_action(int hold)
{
	if (hold && !link_input.holding_action)
		link_input.holding_action_since = seconds_now();
	link_input.holding_action = hold;
}

/* menu: the button pressed now (NULL for none), for the first 150 ms of its
second, as xinput_sdl.c's */
static const char *menu_token(size_t *length)
{
	double elapsed = seconds_now() - link_input.menu_since;
	long step = (long)elapsed;
	const char *token = link_input.setting + 5;

	if (elapsed - (double)step >= 0.15)
		return NULL;
	for (;;)
	{
		token += strspn(token, " ,");
		*length = strcspn(token, " ,");
		if (!*length)
			return NULL;
		if (!step)
			return token;
		step--;
		token += *length;
	}
}

static const struct
{
	const char *name;
	int analog;
	WORD digital;
} button_names[] =
{
	{ "a", XINPUT_GAMEPAD_A, 0 },
	{ "b", XINPUT_GAMEPAD_B, 0 },
	{ "x", XINPUT_GAMEPAD_X, 0 },
	{ "y", XINPUT_GAMEPAD_Y, 0 },
	{ "black", XINPUT_GAMEPAD_BLACK, 0 },
	{ "white", XINPUT_GAMEPAD_WHITE, 0 },
	{ "rb", XINPUT_GAMEPAD_BLACK, 0 },
	{ "lb", XINPUT_GAMEPAD_WHITE, 0 },
	{ "lt", XINPUT_GAMEPAD_LEFT_TRIGGER, 0 },
	{ "rt", XINPUT_GAMEPAD_RIGHT_TRIGGER, 0 },
	{ "up", -1, XINPUT_GAMEPAD_DPAD_UP },
	{ "down", -1, XINPUT_GAMEPAD_DPAD_DOWN },
	{ "left", -1, XINPUT_GAMEPAD_DPAD_LEFT },
	{ "right", -1, XINPUT_GAMEPAD_DPAD_RIGHT },
	{ "start", -1, XINPUT_GAMEPAD_START },
	{ "back", -1, XINPUT_GAMEPAD_BACK },
	{ "lthumb", -1, XINPUT_GAMEPAD_LEFT_THUMB },
	{ "rthumb", -1, XINPUT_GAMEPAD_RIGHT_THUMB },
};

/* the button named (length characters at name), pressed as far as value */
static BOOL button_press(XINPUT_GAMEPAD *pad, const char *name, size_t length, int value)
{
	unsigned int index;

	for (index = 0; index < sizeof(button_names) / sizeof(button_names[0]); index++)
	{
		if (strlen(button_names[index].name) == length && !strncmp(name, button_names[index].name, length))
		{
			if (button_names[index].analog >= 0)
				pad->bAnalogButtons[button_names[index].analog] = (BYTE)(value < 0 ? 0 : value > 255 ? 255 : value);
			else if (value)
				pad->wButtons |= button_names[index].digital;
			return TRUE;
		}
	}
	return FALSE;
}

static SHORT stick_value(long value)
{
	return (SHORT)(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
}

/* pipe: one line, the whole state */
static void pipe_parse(const char *line)
{
	XINPUT_GAMEPAD pad;
	char words[512];
	char *word, *save = NULL;

	memset(&pad, 0, sizeof(pad));
	snprintf(words, sizeof(words), "%s", line);
	if (strchr(words, '#'))
		*strchr(words, '#') = 0;
	for (word = strtok_r(words, " \t\r", &save); word; word = strtok_r(NULL, " \t\r", &save))
	{
		char *equals = strchr(word, '=');
		size_t length = equals ? (size_t)(equals - word) : strlen(word);
		long value = equals ? strtol(equals + 1, NULL, 0) : 255;

		if (length == 2 && !strncmp(word, "lx", 2))
			pad.sThumbLX = stick_value(value);
		else if (length == 2 && !strncmp(word, "ly", 2))
			pad.sThumbLY = stick_value(value);
		else if (length == 2 && !strncmp(word, "rx", 2))
			pad.sThumbRX = stick_value(value);
		else if (length == 2 && !strncmp(word, "ry", 2))
			pad.sThumbRY = stick_value(value);
		else if (!button_press(&pad, word, length, (int)value))
			platform_log("link: port 1's controller: \"%.*s\" is not a button", (int)length, word);
	}
	link_input.pipe_state = pad;
}

/* pipe: whatever arrived since the last frame; the last whole line wins */
static void pipe_read(void)
{
	char buffer[1024];
	ssize_t count;

	if (link_input.pipe < 0)
		return;
	while ((count = read(link_input.pipe, buffer, sizeof(buffer))) > 0)
	{
		ssize_t index;

		for (index = 0; index < count; index++)
		{
			if (buffer[index] == '\n')
			{
				link_input.pipe_line[link_input.pipe_length] = 0;
				pipe_parse(link_input.pipe_line);
				link_input.pipe_length = 0;
			}
			else if (link_input.pipe_length < sizeof(link_input.pipe_line) - 1)
				link_input.pipe_line[link_input.pipe_length++] = buffer[index];
		}
	}
}

/* port 1's controller this frame */
static void source_gamepad(XINPUT_GAMEPAD *pad)
{
	double t;

	if (!link_input.checked)
		source_choose();
	if (link_input.source == _source_pipe)
	{
		pipe_read();
		*pad = link_input.pipe_state;
		return;
	}
	if (link_input.source == _source_menu)
	{
		size_t length;
		const char *token = menu_token(&length);

		if (token)
			button_press(pad, token, length, 255);
		return;
	}
	if (link_input.source == _source_idle)
		return;
	if (link_input.holding_action)
	{
		/* (standing still, the button held from a second on) */
		if (seconds_now() - link_input.holding_action_since >= 1.0)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	t = seconds_now() + link_input.seed * 1.7;
	switch (link_input.source)
	{
	case _source_walk:
		/* (the turn past the game's dead range, 9000: input_xbox.c) */
		pad->sThumbLY = 24000;
		pad->sThumbRX = 13000;
		break;
	case _source_look:
		pad->sThumbRX = (SHORT)(sin(t * 0.5) * 14000.0);
		pad->sThumbRY = (SHORT)(sin(t * 0.3) * 32000.0);
		break;
	case _source_bot:
		/* (xinput_sdl.c's test_input_gamepad) */
		pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
		pad->sThumbLX = (SHORT)(cos(t * 0.6 + link_input.seed) * 20000.0);
		pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
		if (fmod(t, 3.0) < 0.3)
			pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
		if (fmod(t, 5.0) < 0.1)
			pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
		if (fmod(t, 7.0) < 0.2)
			pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
		break;
	}
}

/* ---------- the keyboard and mouse's (none) */

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	(void)gamepad_index;
	*yaw = 0.0f;
	*pitch = 0.0f;
	return FALSE;
}

int halo_linux_mouse_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return FALSE;
}

int platform_input_scheme(void)
{
	return 0;
}

void platform_text_typing(int typing)
{
	(void)typing;
}

void platform_text_field(int typing)
{
	(void)typing;
}

int halo_input_from_name(const char *name)
{
	(void)name;
	return -1;
}

void halo_input_name(int input, char *name, size_t size)
{
	(void)input;
	if (size)
		name[0] = 0;
}

void halo_input_shown_name(const char *binding, char *shown, size_t size)
{
	if (size)
		snprintf(shown, size, "%s", binding);
}

unsigned long halo_keyboard_actions(short controller_index)
{
	(void)controller_index;
	return 0;
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	platform_sdl_initialize();
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		*insertions = XDEVICE_PORT0_MASK & ~reported_gamepads;
		reported_gamepads = XDEVICE_PORT0_MASK;
	}
	else if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && !reported_keyboard)
	{
		*insertions = 1;
		reported_keyboard = TRUE;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && port == 0)
	{
		keyboard_device.open = TRUE;
		return (HANDLE)&keyboard_device;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	struct controller *controller = &controllers[0];

	memset(state, 0, sizeof(*state));
	if (device != (HANDLE)controller || !controller->open)
	{
		int port;

		for (port = 1; port < PORT_COUNT; port++)
		{
			if (device == (HANDLE)&controllers[port] && controllers[port].open)
				return ERROR_SUCCESS;
		}
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	/* (once a frame: the frame's turn, and the stop asked for) */
	link_platform_pace();
	platform_pump_events();
	source_gamepad(&state->Gamepad);
	if (memcmp(&state->Gamepad, &controller->previous, sizeof(state->Gamepad)))
	{
		controller->packet_number++;
		controller->previous = state->Gamepad;
	}
	state->dwPacketNumber = controller->packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	(void)device;
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}
