/* LayoutCell port from NFSU2-SplitScreen, Copyright (c) 2026 mihaits.
 * MIT license: licenses/NFSU2-SplitScreen.txt.
 * Shared by the player-view, viewport and HUD hooks. */
#ifndef NFSU2_SPLITSCREEN_LAYOUT_H
#define NFSU2_SPLITSCREEN_LAYOUT_H

#define SPLIT_MAX_PLAYERS 6
typedef struct { int x, y, w, h; } SplitCell;
static inline int split_view_id(int seat){
    static const int ids[SPLIT_MAX_PLAYERS] = {1, 2, 5, 6, 7, 8};
    return seat >= 0 && seat < SPLIT_MAX_PLAYERS ? ids[seat] : -1;
}
static inline SplitCell split_layout_cell(int count, int seat, int w, int h, int vertical){
    if (count < 1 || count > SPLIT_MAX_PLAYERS || seat < 0 || seat >= count || w <= 0 || h <= 0)
        return (SplitCell){0, 0, 0, 0};
    if (count == 1) return (SplitCell){0, 0, w, h};
    if (count == 2) return vertical ? (SplitCell){seat * (w / 2), 0, w / 2, h}
                                   : (SplitCell){0, seat * (h / 2), w, h / 2};
    if (count <= 4){
        SplitCell c = {(seat & 1) * (w / 2), (seat >> 1) * (h / 2), w / 2, h / 2};
        if (count == 3 && seat == 2) c.x = w / 4;
        return c;
    }
    int row = seat / 3, col = seat % 3;
    SplitCell c = {col * w / 3, row * (h / 2), (col + 1) * w / 3 - col * w / 3, h / 2};
    if (count == 5 && row == 1) c.x += w / 6;
    return c;
}
#endif
