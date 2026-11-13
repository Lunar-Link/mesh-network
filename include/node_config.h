/* Node identity for bring-up, set per board with -D flags (see README). Flash provisioning replaces this later. */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

/* 1 to 254, unique within the network */
#ifndef NODE_ID
#define NODE_ID 1
#endif

#ifndef NETWORK_ID
#define NETWORK_ID 1
#endif

/* 16-byte AES-CMAC key shared by every node; authentication only, nothing is encrypted (Part 97) */
#ifndef NETWORK_KEY
#define NETWORK_KEY {0x4c, 0x75, 0x6e, 0x61, 0x72, 0x2d, 0x4c, 0x69, 0x6e, 0x6b, 0x20, 0x62, 0x65, 0x6e, 0x63, 0x68}
#endif

/* Callsign sent in every status frame; required for Part 97 operation, at most 8 characters */
#ifndef STATION_ID
#define STATION_ID ""
#endif

#endif
