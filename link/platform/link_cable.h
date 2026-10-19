/*
LINK_CABLE.H

The Link's cable to the Nintendo 64 (link/README.md; the N64's side is
halo-64's port/n64/src/n64_link.c, the protocol its n64_link.h): UNFLoader's
USB packets ("DMA@", the datatype and size, the data padded to an even
length, "CMPH"), one link packet each. HALO_LINK_CABLE says where the cable
is: "tcp:<host>:<port>" connects to an emulator's emulated USB port,
"listen:<port>" waits for one; a flashcart's USB (UNFLoader's device layer)
comes later.
*/

#ifndef __LINK_CABLE_H
#define __LINK_CABLE_H

#define LINK_CABLE_DATATYPE 0x48
/* 2: CONTROL, the players' own units in FRAME, LOAD's teams */
#define LINK_CABLE_VERSION 2
#define LINK_CABLE_HEADER_SIZE 8
#define LINK_CABLE_PAYLOAD_MAXIMUM 4096
/* a CONTROL's payload: the N64's tick (4), the local player (1), the
control's flags (4), yaw, pitch, the throttle's two parts and the trigger
(5 x f32), the weapon, grenade and zoom (3 x s8), big-endian */
#define LINK_CABLE_CONTROL_SIZE 32

enum link_cable_type
{
	/* the N64's */
	LINK_CABLE_HELLO = 1,
	LINK_CABLE_INPUT = 2,
	LINK_CABLE_LIST = 3,
	LINK_CABLE_JOIN = 4,
	LINK_CABLE_HOST = 5,
	LINK_CABLE_LEAVE = 6,
	LINK_CABLE_CONTROL = 7,
	LINK_CABLE_NAME = 8,
	/* the Link's */
	LINK_CABLE_WELCOME = 64,
	LINK_CABLE_STATUS = 65,
	LINK_CABLE_GAMES = 66,
	LINK_CABLE_LOAD = 67,
	LINK_CABLE_FRAME = 68,
	LINK_CABLE_ENDED = 69
};

/* a request the N64 made (LIST, JOIN, HOST, LEAVE), for the director */
struct link_cable_request
{
	int type;
	int length;
	unsigned char payload[256];
};

/* each frame: the cable read and kept up (HELLO answered, STATUS repeated) */
void link_cable_update(void);
/* the N64 has said HELLO (and not gone quiet since) */
int link_cable_connected(void);
/* the oldest request not yet taken (1), or none (0) */
int link_cable_take_request(struct link_cable_request *request);
/* a packet to the N64 */
void link_cable_send(int type, const void *payload, int length);
/* the menu's line (sent now, and again each second) */
void link_cable_status(const char *text);
/* how many INPUTs have come (the N64 sends one each frame it draws: the
frames' pace, link_export.c) */
unsigned long link_cable_input_count(void);
/* the N64's controller on port (0-3) as its last INPUT had it: buttons,
analog buttons (8), the sticks (lx, ly, rx, ry); 0 if none */
int link_cable_controller(int port, unsigned short *buttons, unsigned char analog[8], short sticks[4]);
/* the N64's ticks' controls not yet played (CONTROL), the oldest first:
how many, and the oldest taken (its payload, LINK_CABLE_CONTROL_SIZE
bytes), 1, or 0 if none */
int link_cable_controls_queued(void);
int link_cable_take_control(unsigned char record[LINK_CABLE_CONTROL_SIZE]);
/* the player's name the N64 last sent (NAME; "" none): how many it has
sent, the name in name */
unsigned long link_cable_name(char *name, int size);

#endif
