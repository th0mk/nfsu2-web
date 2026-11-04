/* Shared-engine array relocation, derived from the MIT NFSU2-SplitScreen
 * local_players.cpp and input_ports.cpp. See licenses/NFSU2-SplitScreen.txt.
 * 0x00d00000 is above guest TEBs and below the module heaps (0x01000000).
 * Keep the native builds at the original addresses until their generator
 * and bootstrap opt into the expanded arrays too. */
#ifndef NFSU2_SPLITSCREEN_MEM_H
#define NFSU2_SPLITSCREEN_MEM_H
#ifdef __wasm__
#define SPLIT_PLAYERS_INDEX 0x00d00000u
#define SPLIT_PLAYERS_ORDER 0x00d00018u
#define SPLIT_PLAYERS_COUNT 0x00d00030u
#define SPLIT_PORT_TABLES 0x00d00100u
#define SPLIT_PORT_CAPACITY 6u
#else
#define SPLIT_PLAYERS_INDEX 0x008900acu
#define SPLIT_PLAYERS_ORDER 0x008900b4u
#define SPLIT_PLAYERS_COUNT 0x008900bcu
#define SPLIT_PORT_TABLES 0x008655a0u
#define SPLIT_PORT_CAPACITY 2u
#endif
#define SPLIT_PORT_TABLE_END (SPLIT_PORT_TABLES + SPLIT_PORT_CAPACITY * 0x1c8u)
int split_render_limit(void);
unsigned split_settings_address(unsigned original);
#endif
