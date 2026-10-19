// usb-echo: test ROM for an emulated flashcart USB port (ares ARES_N64_USB).
//
// Uses libdragon's usb.c (UNFLoader's USB library). After boot it sends a
// DATATYPE_TEXT hello (repeated every second until the first packet arrives,
// so a PC program that connects late still gets it), then echoes every packet
// it receives with the same datatype, prefixed by a counter (1, 2, 3...):
// 4 bytes big-endian, or for DATATYPE_TEXT 8 hex digits and ':' so UNFLoader's
// debug mode prints it. A packet of type 0x30 is answered with the counter and
// the N64's 64-bit tick count (get_ticks(), 46.875 MHz) instead, so the PC can
// compare emulated time with its own. Status goes to the IS-Viewer (debugf).

#include <libdragon.h>
#include <usb.h>
#include <stdio.h>
#include <string.h>

#define MAX_PACKET (64 * 1024)
#define DATATYPE_TICKS 0x30

static uint8_t packet[MAX_PACKET + 9] __attribute__((aligned(16)));

int main(void)
{
    debug_init_isviewer();
    debugf("usb-echo: boot\n");

    if (!usb_initialize()) {
        debugf("usb-echo: no flashcart USB found\n");
        for (;;) {}
    }
    debugf("usb-echo: usb_initialize ok, cart %d (2 = EverDrive)\n", usb_getcart());

    char hello[64];
    int hello_len = snprintf(hello, sizeof(hello), "usb-echo hello, cart %d\n", usb_getcart());
    usb_write(DATATYPE_TEXT, hello, hello_len);
    uint32_t last_hello = TICKS_READ();

    uint32_t echoed = 0, failures = 0;
    for (;;) {
        uint32_t header = usb_poll();
        if (header == 0) {
            if (echoed == 0 && TICKS_SINCE(last_hello) > TICKS_FROM_MS(1000)) {
                usb_write(DATATYPE_TEXT, hello, hello_len);
                last_hello = TICKS_READ();
            }
            continue;
        }

        int type = USBHEADER_GETTYPE(header);
        int size = USBHEADER_GETSIZE(header);
        if (size > MAX_PACKET) {
            debugf("usb-echo: dropping a %d byte packet (max %d)\n", size, MAX_PACKET);
            usb_purge();
            continue;
        }
        int prefix = type == DATATYPE_TEXT ? 9 : 4;
        usb_read(packet + prefix, size);

        echoed++;
        if (type == DATATYPE_TICKS) {
            uint64_t now = get_ticks();
            for (int i = 0; i < 8; i++)
                packet[4 + i] = now >> (56 - 8 * i);
            size = 8;
        }
        if (type == DATATYPE_TEXT) {
            char counter[10];
            snprintf(counter, sizeof(counter), "%08lx:", echoed);
            memcpy(packet, counter, 9);
        } else {
            packet[0] = echoed >> 24;
            packet[1] = echoed >> 16;
            packet[2] = echoed >> 8;
            packet[3] = echoed;
        }
        if (usb_write(type, packet, size + prefix) != 1) {
            failures++;
            debugf("usb-echo: write of echo %lu failed (timed out: %d)\n", echoed, usb_timedout());
        }
        if (echoed <= 3 || echoed % 1000 == 0)
            debugf("usb-echo: echoed %lu packets (last: type %d, %d bytes), %lu write failures\n",
                   echoed, type, size, failures);
    }
}
