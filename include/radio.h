/* Radio driver interface. Each driver lives in its own folder under src/radio/; the build environment picks one. */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lmesh/lmesh.h"

/* Starts the radio; bring-up may finish later inside radio_poll, and radio_send refuses until it has. */
void radio_init(void);
/* Begins sending one frame; 0 if accepted. Completion is reported through radio_poll. */
int radio_send(const uint8_t *frame, size_t len);
/* Estimated time from radio_send to transmit done for a frame of len bytes. */
uint32_t radio_airtime_ms(size_t len);
/* Runs the driver from the main loop and hands received frames and transmit results to the library; true if it did. */
bool radio_poll(lmesh_node *node);

#endif
