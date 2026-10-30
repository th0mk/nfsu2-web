/* comrec.h -- the device-call recorder (runtime/comrec.c): with NFSU2_DXREC=<file> every
 * IDirect3DDevice9 call the guest makes, every state block's Capture, Apply and Release, and the
 * game's own context lines (comrec_marker: Most Wanted's effect methods) are written to <file>, one
 * line each, with object pointers and the data the calls point at made comparable between two runs
 * (docs/web/dxrec.md).  web/tools/dxrec-diff.mjs compares two such files.  Off (g_comrec_on 0) the
 * cost is one test per COM call. */
#ifndef COMREC_H
#define COMREC_H
#include <stdint.h>

struct CPU;
extern int g_comrec_on;

enum { CR_NONE, CR_DEV, CR_SB, CR_SWAP };          /* what com.c hands the recorder */

/* com.c: a new COM object of interface iface (com.c's index) */
void comrec_new(uint32_t obj, uint32_t iface);
/* com.c, around a call of method m of an object of kind (CR_*), before its body ran: the arguments
 * at c->esp + 4 ('this' first, the return address at c->esp).  Returns a token for comrec_post, which
 * runs after the body with the result in c->eax. */
int  comrec_pre(struct CPU *c, int kind, unsigned m, const char *name, int nargs);
void comrec_post(struct CPU *c, int tok);
/* a context line of the game's, written as it is (no newline needed); not a device call */
void comrec_marker(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void comrec_close(void);                         /* the file, whole (ExitProcess, the host's exit) */
/* a guest object as the recorder names it ("tex#3", "vs:1a2b3c4d", "dev"), or the hex value */
void comrec_obj(uint32_t v, char *buf, int n);

/* com.c's side: the interface index of one of its objects (0: not one), its name, a shader's code */
int com_obj_iface(uint32_t o);
const char *com_iface_name(int iface);
int com_shader_code(uint32_t o, uint32_t *p, uint32_t *n);
#endif
