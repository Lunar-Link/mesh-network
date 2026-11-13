/* LR1121 power control beyond the radio interface. Nothing calls these yet; the node stays in receive. */
#ifndef LR1121_H
#define LR1121_H

/* Puts the chip in power down from idle. The configuration is lost, so waking runs the full bring-up. */
void lr1121_sleep(void);
void lr1121_wake(void);

#endif
