/* The line protocol (SPEC.md Section 11) on the host UART: the ST-LINK virtual COM port on a Nucleo, a USB serial
   adapter on a Blue Pill. */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>

#include "lmesh/lmesh.h"

void console_init(void);
/* Reads complete command lines and answers them; true if a command ran. */
bool console_poll(lmesh_node *node);
void console_event(void *ctx, const lmesh_event *ev);
/* Writes one line of platform output, such as a radio driver report, followed by a newline. */
void console_line(const char *text);

#endif
