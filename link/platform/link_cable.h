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
#define LINK_CABLE_VERSION 1
#define LINK_CABLE_HEADER_SIZE 8
#define LINK_CABLE_PAYLOAD_MAXIMUM 4096

enum link_cable_type
{
	/* the N64's */
	LINK_CABLE_HELLO = 1,
	LINK_CABLE_INPUT = 2,
	LINK_CABLE_LIST = 3,
	LINK_CABLE_JOIN = 4,
	LINK_CABLE_HOST = 5,
	LINK_CABLE_LEAVE = 6,
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
/* the N64's controller on port (0-3) as its last INPUT had it: buttons,
analog buttons (8), the sticks (lx, ly, rx, ry); 0 if none */
int link_cable_controller(int port, unsigned short *buttons, unsigned char analog[8], short sticks[4]);

#endif
