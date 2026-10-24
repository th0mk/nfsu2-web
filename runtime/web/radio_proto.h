/* The radio's words shared with the page (runtime/radio.c, web/src/radio.ts, RadioLink in web/src/app/status.ts).
 * Word order and size are the page's layout; web/test/radio.test.mjs checks the order. */
#ifndef NFSU2_RADIO_PROTO_H
#define NFSU2_RADIO_PROTO_H
#include <stdint.h>

#define RADIO_TRACKS 27            /* the game's EA Trax table */
#define RADIO_PLACES 2             /* 0 menus, 1 driving */
#define RADIO_TAG    44            /* bytes of one line of text: the plaque's 40 characters and the NUL, in whole words */
#define RADIO_RATE   44100         /* the game mixer's rate */
#define RADIO_MAGIC  0x31444152u   /* "RAD1": a runtime with the radio writes it first in the block */
#define RADIO_VERSION 1u           /* of the words below: a layout change raises it */

typedef struct {
    uint32_t radio_magic;          /* RADIO_MAGIC, written when the runtime starts; the page uses nothing below without it */
    uint32_t radio_version;        /* RADIO_VERSION */
    uint32_t on;                   /* odd: on.  Every switch adds one, so the runtime starts afresh at each switch-on; it adds one itself when it cannot go on */
    struct {                       /* written by the page: the next track of each place */
        uint32_t tag_seq;          /* sequence lock of offer_text, odd while the page writes; 0: no text yet */
        uint32_t places;           /* bit k: the page has a track for place k */
    } offer;
    struct {                       /* written by the runtime: the game started a song */
        uint32_t req_seq;          /* counts the requests */
        uint32_t req_index;        /* the place */
    } request;
    struct {                       /* written by the page: the track of request req_seq */
        uint32_t ready_seq;        /* the page serves track req_seq: total is set and written counts from 0 */
        uint32_t failed_seq;       /* the page cannot serve track req_seq */
        uint32_t total;            /* frames of the track; the page lowers it to `read` to end the track early */
        uint32_t written;          /* frames written since the track began */
    } reply;
    struct {                       /* two rings of floats (left, then right) */
        uint32_t ring;             /* address in the module's memory, set by the runtime */
        uint32_t ring_frames;      /* a power of two */
        uint32_t read;             /* frames taken by the runtime */
    } rings;
    char offer_text[RADIO_PLACES][3][RADIO_TAG];   /* per place: artist, title, album, NUL-padded ASCII */
} HostStatusRadio;

_Static_assert(sizeof(HostStatusRadio) == 56 + RADIO_PLACES * 3 * RADIO_TAG, "page layout (status.ts)");
#endif
