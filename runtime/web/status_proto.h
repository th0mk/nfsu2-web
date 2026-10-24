/* HOST_KIND_STATUS: counters for the page's frame-rate overlay
 * (web/src/app/status.ts reads them).
 *
 * Self-describing on purpose: a table of named doubles, so any part of the
 * web runtime (renderer, audio, pacing) can publish a counter with
 * host_status_slot()/host_status_set() without a layout change on the page.
 * The page shows the change in "frames" over the change in "frames_t" (the
 * C clock when frames was read, ms) as the game's frame rate, and lists the
 * rest in the detailed overlay.
 *
 * Consistency: a sequence lock.  The writer makes seq odd, writes, makes it
 * even; the page copies the table and keeps the copy only if seq was even
 * and unchanged around it.  Names are NUL-padded ASCII. */
#ifndef NFSU2_STATUS_PROTO_H
#define NFSU2_STATUS_PROTO_H
#include <stdint.h>

#define HOST_STATUS_MAGIC 0x31545453u      /* "STT1" */
#define HOST_STATUS_MAX   32
#define HOST_STATUS_NAME  16

typedef struct { char name[HOST_STATUS_NAME]; double value; } HostStatusItem;   /* 24 bytes */
#include "radio_proto.h"

typedef struct {
    uint32_t magic;        /* HOST_STATUS_MAGIC */
    uint32_t seq;          /* sequence lock, even = stable */
    uint32_t count;        /* items in use */
    uint32_t period_ms;    /* how often the publisher refreshes the table */
    HostStatusItem item[HOST_STATUS_MAX];
    HostStatusRadio radio;
} HostStatus;

_Static_assert(sizeof(HostStatusItem) == 24, "page layout (status.ts)");
_Static_assert(sizeof(HostStatus) == 16 + 24 * HOST_STATUS_MAX + sizeof(HostStatusRadio), "page layout (status.ts)");

/* status_web.c */
int  host_status_slot(const char *name);          /* index of a named counter (created on first use), -1 if full */
void host_status_set(int slot, double value);
void host_status_set_n(const int *slot, const double *value, int n);   /* in one consistent section */
HostStatusRadio *host_status_radio(void);         /* the radio's words, shared with the page */
#endif
