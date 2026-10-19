/*
LINK_CABLE.C

The Link's cable to the Nintendo 64 (link_cable.h): a TCP connection to an
emulator's emulated flashcart USB port, made or awaited as HALO_LINK_CABLE
says, read without blocking each frame. Its bytes are UNFLoader's USB
packets; those of the link's datatype are link packets: HELLO is answered
with WELCOME, INPUT kept for link_input.c, the rest queued for the
director (port/linux/game/link_director.c). A connection that closes, or an
N64 that says nothing for LINK_SILENCE_SECONDS, is gone; the cable is made
again (or awaited) every RETRY_SECONDS.
*/

#include "link_cable.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RETRY_SECONDS 2.0
#define LINK_SILENCE_SECONDS 5.0
#define STATUS_SECONDS 1.0
#define REQUEST_QUEUE 16
#define CONTROLLER_PORTS 4
/* the most of the stream kept unread: a whole packet and its framing */
#define STREAM_SIZE (2 * (LINK_CABLE_HEADER_SIZE + LINK_CABLE_PAYLOAD_MAXIMUM + 16))

static struct
{
	int configured, listening;
	char host[64];
	int port;
	int listener, sock;
	double next_try, last_heard, last_status;
	int hello;
	unsigned short sequence;
	char status[128];
	unsigned char stream[STREAM_SIZE];
	int stream_length;
	struct link_cable_request requests[REQUEST_QUEUE];
	int request_first, request_count;
	struct
	{
		int present;
		unsigned short buttons;
		unsigned char analog[8];
		short sticks[4];
	} controllers[CONTROLLER_PORTS];
} cable = { 0, 0, "", 0, -1, -1 };

/* (a native unit: the game's log is out of reach, standard error is not) */
static void cable_log(const char *format, ...)
{
	va_list arguments;

	fputs("halo64-link: ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

static double now_seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---------- the connection */

static void configure(void)
{
	const char *setting = getenv("HALO_LINK_CABLE");

	cable.configured = 1;
	if (!setting || !setting[0])
	{
		cable_log("link: no HALO_LINK_CABLE: no N64 (\"tcp:127.0.0.1:6464\" for an emulator's USB port)");
		return;
	}
	if (!strncmp(setting, "tcp:", 4))
	{
		const char *colon = strrchr(setting + 4, ':');

		if (colon && colon - (setting + 4) < (long)sizeof(cable.host))
		{
			memcpy(cable.host, setting + 4, colon - (setting + 4));
			cable.host[colon - (setting + 4)] = 0;
			cable.port = atoi(colon + 1);
		}
	}
	else if (!strncmp(setting, "listen:", 7))
	{
		cable.listening = 1;
		snprintf(cable.host, sizeof(cable.host), "127.0.0.1");
		cable.port = atoi(setting + 7);
	}
	if (cable.port <= 0 || cable.port > 65535)
	{
		cable_log("link: HALO_LINK_CABLE \"%s\" is not tcp:<host>:<port> or listen:<port>", setting);
		cable.port = 0;
		return;
	}
	cable_log("link: the N64's cable: %s %s:%d", cable.listening ? "waiting on" : "connecting to", cable.host,
		cable.port);
}

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	int one = 1;

	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void disconnect(const char *why)
{
	if (cable.sock >= 0)
	{
		close(cable.sock);
		cable.sock = -1;
		cable_log("link: the N64's cable: %s", why);
	}
	cable.hello = 0;
	cable.stream_length = 0;
	memset(cable.controllers, 0, sizeof(cable.controllers));
}

static void try_connect(double now)
{
	struct sockaddr_in address;

	if (now < cable.next_try)
		return;
	cable.next_try = now + RETRY_SECONDS;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)cable.port);
	if (inet_pton(AF_INET, cable.host, &address.sin_addr) != 1)
		return;
	if (cable.listening)
	{
		if (cable.listener < 0)
		{
			int one = 1;

			cable.listener = socket(AF_INET, SOCK_STREAM, 0);
			if (cable.listener < 0)
				return;
			setsockopt(cable.listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
			if (bind(cable.listener, (struct sockaddr *)&address, sizeof(address)) || listen(cable.listener, 1))
			{
				close(cable.listener);
				cable.listener = -1;
				return;
			}
			set_nonblocking(cable.listener);
		}
		cable.sock = accept(cable.listener, NULL, NULL);
	}
	else
	{
		cable.sock = socket(AF_INET, SOCK_STREAM, 0);
		if (cable.sock >= 0 && connect(cable.sock, (struct sockaddr *)&address, sizeof(address)))
		{
			close(cable.sock);
			cable.sock = -1;
		}
	}
	if (cable.sock >= 0)
	{
		set_nonblocking(cable.sock);
		cable.last_heard = now;
		cable_log("link: the N64's cable is connected");
	}
}

/* ---------- packets */

static void write_all(const unsigned char *data, int length)
{
	while (cable.sock >= 0 && length > 0)
	{
		ssize_t sent = send(cable.sock, data, (size_t)length, MSG_NOSIGNAL);

		if (sent < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			disconnect("closed while writing");
			return;
		}
		data += sent;
		length -= (int)sent;
	}
}

void link_cable_send(int type, const void *payload, int length)
{
	unsigned char packet[8 + LINK_CABLE_HEADER_SIZE + LINK_CABLE_PAYLOAD_MAXIMUM + 1 + 4];
	int size = LINK_CABLE_HEADER_SIZE + length, at = 0;
	unsigned long header = ((unsigned long)LINK_CABLE_DATATYPE << 24) | (unsigned long)size;

	if (cable.sock < 0 || length < 0 || length > LINK_CABLE_PAYLOAD_MAXIMUM)
		return;
	memcpy(packet, "DMA@", 4);
	packet[4] = (unsigned char)(header >> 24);
	packet[5] = (unsigned char)(header >> 16);
	packet[6] = (unsigned char)(header >> 8);
	packet[7] = (unsigned char)header;
	at = 8;
	packet[at++] = 'H';
	packet[at++] = '6';
	packet[at++] = LINK_CABLE_VERSION;
	packet[at++] = (unsigned char)type;
	packet[at++] = (unsigned char)(length >> 8);
	packet[at++] = (unsigned char)length;
	packet[at++] = (unsigned char)(cable.sequence >> 8);
	packet[at++] = (unsigned char)cable.sequence;
	cable.sequence++;
	if (length)
		memcpy(packet + at, payload, (size_t)length);
	at += length;
	if (size & 1)
		packet[at++] = 0;
	memcpy(packet + at, "CMPH", 4);
	at += 4;
	write_all(packet, at);
}

void link_cable_status(const char *text)
{
	unsigned char payload[128];
	size_t length = strlen(text);

	if (length > sizeof(cable.status) - 1)
		length = sizeof(cable.status) - 1;
	memcpy(cable.status, text, length);
	cable.status[length] = 0;
	payload[0] = (unsigned char)length;
	memcpy(payload + 1, text, length);
	link_cable_send(LINK_CABLE_STATUS, payload, (int)length + 1);
	cable.last_status = now_seconds();
}

static void welcome(void)
{
	static const char build[] = "Halo 64 Link 0.1 (ChupathingyCE 0.7.5d)";
	unsigned char payload[64];

	payload[0] = LINK_CABLE_VERSION;
	payload[1] = (unsigned char)(sizeof(build) - 1);
	memcpy(payload + 2, build, sizeof(build) - 1);
	link_cable_send(LINK_CABLE_WELCOME, payload, (int)sizeof(build) + 1);
}

/* INPUT: the N64's frame (4), how many controllers (1), each: its port (1),
buttons (2), analog buttons (8), the sticks (4 x 2), big-endian */
static void take_input(const unsigned char *at, int length)
{
	int count, i;

	if (length < 5)
		return;
	count = at[4];
	at += 5;
	length -= 5;
	for (i = 0; i < CONTROLLER_PORTS; i++)
		cable.controllers[i].present = 0;
	for (i = 0; i < count && length >= 19; i++, at += 19, length -= 19)
	{
		int port = at[0], k;

		if (port >= CONTROLLER_PORTS)
			continue;
		cable.controllers[port].present = 1;
		cable.controllers[port].buttons = (unsigned short)(at[1] << 8 | at[2]);
		memcpy(cable.controllers[port].analog, at + 3, 8);
		for (k = 0; k < 4; k++)
			cable.controllers[port].sticks[k] = (short)(at[11 + 2 * k] << 8 | at[12 + 2 * k]);
	}
}

static void handle(const unsigned char *packet, int size)
{
	int length;

	if (size < LINK_CABLE_HEADER_SIZE || packet[0] != 'H' || packet[1] != '6')
		return;
	length = packet[4] << 8 | packet[5];
	if (LINK_CABLE_HEADER_SIZE + length > size)
		return;
	cable.last_heard = now_seconds();
	if (packet[2] != LINK_CABLE_VERSION)
	{
		cable_log("link: the N64 speaks the cable's version %d, the Link %d", packet[2], LINK_CABLE_VERSION);
		return;
	}
	switch (packet[3])
	{
	case LINK_CABLE_HELLO:
		if (!cable.hello)
			cable_log("link: the N64 said hello");
		cable.hello = 1;
		welcome();
		if (cable.status[0])
			link_cable_status(cable.status);
		break;
	case LINK_CABLE_INPUT:
		take_input(packet + LINK_CABLE_HEADER_SIZE, length);
		break;
	case LINK_CABLE_LIST:
	case LINK_CABLE_JOIN:
	case LINK_CABLE_HOST:
	case LINK_CABLE_LEAVE:
		if (cable.request_count < REQUEST_QUEUE && length <= (int)sizeof(cable.requests[0].payload))
		{
			struct link_cable_request *request =
				&cable.requests[(cable.request_first + cable.request_count) % REQUEST_QUEUE];

			request->type = packet[3];
			request->length = length;
			memcpy(request->payload, packet + LINK_CABLE_HEADER_SIZE, (size_t)length);
			cable.request_count++;
		}
		break;
	default:
		break;
	}
}

/* the USB packets in what has arrived */
static void parse(void)
{
	int at = 0;

	for (;;)
	{
		int left = cable.stream_length - at, size, padded;
		unsigned long header;

		/* (out of step: on to the next "DMA@") */
		while (left >= 4 && memcmp(cable.stream + at, "DMA@", 4))
		{
			at++;
			left--;
		}
		if (left < 8)
			break;
		header = (unsigned long)cable.stream[at + 4] << 24 | (unsigned long)cable.stream[at + 5] << 16 |
			(unsigned long)cable.stream[at + 6] << 8 | cable.stream[at + 7];
		size = (int)(header & 0xFFFFFF);
		padded = size + (size & 1);
		if (8 + padded + 4 > (int)sizeof(cable.stream))
		{
			/* (longer than anything the cartridge sends: skipped) */
			at += 4;
			continue;
		}
		if (left < 8 + padded + 4)
			break;
		if (memcmp(cable.stream + at + 8 + padded, "CMPH", 4))
		{
			at += 4;
			continue;
		}
		if ((header >> 24) == LINK_CABLE_DATATYPE)
			handle(cable.stream + at + 8, size);
		at += 8 + padded + 4;
	}
	memmove(cable.stream, cable.stream + at, (size_t)(cable.stream_length - at));
	cable.stream_length -= at;
}

/* ---------- each frame */

void link_cable_update(void)
{
	double now = now_seconds();

	if (!cable.configured)
		configure();
	if (!cable.port)
		return;
	if (cable.sock < 0)
	{
		try_connect(now);
		if (cable.sock < 0)
			return;
	}
	for (;;)
	{
		ssize_t got = recv(cable.sock, cable.stream + cable.stream_length,
			sizeof(cable.stream) - (size_t)cable.stream_length, 0);

		if (got > 0)
		{
			cable.stream_length += (int)got;
			parse();
			if (cable.stream_length == (int)sizeof(cable.stream))
				cable.stream_length = 0;
			continue;
		}
		if (got == 0)
		{
			disconnect("closed");
			return;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			disconnect("lost");
		break;
	}
	if (cable.hello && now - cable.last_heard > LINK_SILENCE_SECONDS)
	{
		cable.hello = 0;
		memset(cable.controllers, 0, sizeof(cable.controllers));
		cable_log("link: the N64 stopped talking");
	}
	if (cable.hello && cable.status[0] && now - cable.last_status > STATUS_SECONDS)
		link_cable_status(cable.status);
}

int link_cable_connected(void)
{
	return cable.sock >= 0 && cable.hello;
}

int link_cable_take_request(struct link_cable_request *request)
{
	if (!cable.request_count)
		return 0;
	*request = cable.requests[cable.request_first];
	cable.request_first = (cable.request_first + 1) % REQUEST_QUEUE;
	cable.request_count--;
	return 1;
}

int link_cable_controller(int port, unsigned short *buttons, unsigned char analog[8], short sticks[4])
{
	if (port < 0 || port >= CONTROLLER_PORTS || !cable.controllers[port].present || !link_cable_connected())
		return 0;
	*buttons = cable.controllers[port].buttons;
	memcpy(analog, cable.controllers[port].analog, 8);
	memcpy(sticks, cable.controllers[port].sticks, sizeof(cable.controllers[port].sticks));
	return 1;
}
