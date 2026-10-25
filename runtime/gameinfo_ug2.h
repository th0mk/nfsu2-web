/* SPEED2.EXE (UG2 v1.2 NA) as the runtime is built for it, for a lift that
 * has no gameinfo.h of its own (one made before tools/genall.py wrote it, or
 * a test build without gen/all): the names and values of a UG2 lift's
 * gen/all/gameinfo.h.  runtime/cpu.h includes it in that case only. */
#ifndef GAMEINFO_H
#define GAMEINFO_H
#define GAMEINFO_IMAGE_BASE 0x00400000u   /* ImageBase */
#define GAMEINFO_IMAGE_SIZE 0x00532000u   /* SizeOfImage: the image ends at 0x00932000 */
#define GAMEINFO_TEXT_BASE  0x00401000u   /* .text: one dispatch table slot per byte */
#define GAMEINFO_TEXT_SIZE  0x00381af1u
#define GAMEINFO_ENTRY      0x0075bcc7u   /* the entry point, lifted as GAMEINFO_ENTRY_FN */
#define GAMEINFO_ENTRY_FN   sub_0075bcc7
#define GAMEINFO_FTOL2      0x0075bed4u   /* the C runtime's _ftol2 (FTOL2_VA) */
#endif
