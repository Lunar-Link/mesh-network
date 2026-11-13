/* Line protocol on USART2: assembles command lines, passes them to the library, queues replies and events. */
#include "console.h"

#include <string.h>

#include "serial.h"

#define LINE_MAX 256
#define TX_SIZE 8192
#define RX_SIZE 512
#define EVENT_MAX 768
/* Room kept free for events the library emits while it answers a command */
#define EVENT_RESERVE 1024

static serial port;
static uint8_t rx_buf[RX_SIZE];
static uint8_t tx_buf[TX_SIZE];
static char line[LINE_MAX];
static size_t line_len;
static bool line_overflow;
static char reply[TX_SIZE];

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

bool console_poll(lmesh_node *node)
{
    bool ran = false;
    int c;
    while ((c = serial_getc(&port)) >= 0) {
        if (c == '\r' || c == '\n') {
            if (line_overflow) {
                console_line("{\"ok\":false,\"reason\":\"too_long\"}");
                ran = true;
            } else if (line_len > 0) {
                line[line_len] = '\0';
                run_line(node);
                ran = true;
            }
            line_len = 0;
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
