/* Line protocol on the host UART: assembles command lines, passes them to the library, queues replies and events. */
#include "console.h"

#include <string.h>

#include "serial.h"

#define LINE_MAX 256
/* A power of two. A table dump that does not fit ends early with "truncated":true, so a chip with little RAM sets a
   smaller ring in its mcu.h and gets shorter dumps. */
#ifndef CONSOLE_TX_SIZE
#define CONSOLE_TX_SIZE 8192
#endif
#define TX_SIZE CONSOLE_TX_SIZE
#define RX_SIZE 512
#define EVENT_MAX 768
/* Room kept free for events the library emits while it answers a command */
#define EVENT_RESERVE 1024
/* A command waits until the ring has drained to at least this much room for its reply */
#define REPLY_MIN 512

_Static_assert((TX_SIZE & (TX_SIZE - 1)) == 0 && TX_SIZE >= 2 * EVENT_RESERVE && REPLY_MIN <= EVENT_RESERVE,
               "the transmit ring is a power of two with room for a reply");

static serial port;
static uint8_t rx_buf[RX_SIZE];
static uint8_t tx_buf[TX_SIZE];
static char line[LINE_MAX];
static size_t line_len;
static bool line_overflow;
static bool line_ready;
static char reply[TX_SIZE - EVENT_RESERVE];

void console_init(void)
{
    serial_init(&port, &board_host_uart, rx_buf, sizeof rx_buf, tx_buf, sizeof tx_buf);
}

static void run_line(lmesh_node *node)
{
    size_t room = serial_tx_free(&port);
    room = room > EVENT_RESERVE ? room - EVENT_RESERVE : 0;
    if (room > sizeof reply) {
        room = sizeof reply;
    }
    size_t n = lmesh_host_command(node, line, reply, room);
    serial_write(&port, reply, n);
}

/* A complete line is held, and input left in the receive ring, until the transmit ring has room for a reply, so
   commands sent in a burst are answered in turn instead of with replies cut short. */
bool console_poll(lmesh_node *node)
{
    bool ran = false;
    for (;;) {
        if (line_ready) {
            if (serial_tx_free(&port) < REPLY_MIN + EVENT_RESERVE) {
                break;
            }
            run_line(node);
            line_ready = false;
            line_len = 0;
            ran = true;
        }
        int c = serial_getc(&port);
        if (c < 0) {
            break;
        }
        if (c == '\r' || c == '\n') {
            if (line_overflow) {
                console_line("{\"ok\":false,\"reason\":\"too_long\"}");
                line_len = 0;
                ran = true;
            } else if (line_len > 0) {
                line[line_len] = '\0';
                line_ready = true;
            }
            line_overflow = false;
        } else if (line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
        } else {
            line_overflow = true;
        }
    }
    return ran;
}

void console_event(void *ctx, const lmesh_event *ev)
{
    static char text[EVENT_MAX];
    (void)ctx;
    size_t n = lmesh_host_format_event(ev, text, sizeof text);
    serial_write(&port, text, n);
}

void console_line(const char *text)
{
    size_t n = strlen(text);
    if (n + 1 <= serial_tx_free(&port)) {
        serial_write(&port, text, n);
        serial_write(&port, "\n", 1);
    }
}
