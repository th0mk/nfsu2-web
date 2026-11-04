/* Per-player nitrous exposure, following the ASI's render_weather.cpp.
 * Full-screen post quads keep their UVs; each draw clips to the player's
 * normalized region, including when the target is downsampled. */
void sub_005bc180_orig(CPU *c);
void sub_005c5f70_orig(CPU *c);
void sub_005c6be0_orig(CPU *c);
#ifdef __wasm__
static float split_ob_smooth[6];
static uint32_t split_ob_amount[6], split_ob_threshold[6];
static int split_rain_seat = -1;
static void split_post_log(unsigned effect, int seat){
    static unsigned seen[3];
    if (!getenv("NFSU2_SPLIT_LIGHTS_LOG") || (seen[effect] & (1u << seat))) return;
    seen[effect] |= 1u << seat;
    const char *name[] = {"brightness", "rain", "lens drops"};
    fprintf(stderr, "[split effects] P%d %s camera=%d isolated\n", seat + 1, name[effect], split_view_id(seat));
}
#endif
void sub_005bc180(CPU *c){
    sub_005bc180_orig(c);
#ifdef __wasm__
    if (!split_racing()) return;
    split_ob_amount[0] = MR32(0x7ff114u);
    split_ob_threshold[0] = MR32(0x7ff10cu);
    split_ob_smooth[0] = MRF32(0x876848u);
    for (int k = 1; k < split_render_limit(); ++k){
        uint32_t fx = MR32(split_view(split_view_id(k)) + 0x60);
        float t = MRF32(0x7d7d3cu);
        if (MR8(0x870904u)) t = MRF32(0x7ff7c8u);
        else if (MR32(0x8654a4u) != 6) t = MRF32(0x7a20a8u);
        else if (fx && MR32(fx + 0x3c)) t = MRF32(fx + 0x1e0);
        float lo = MRF32(0x7ff0fcu), rate = MRF32(0x800b90u);
        split_ob_smooth[k] = (1.f-rate)*split_ob_smooth[k] + rate*(lo+(MRF32(0x7ff100u)-lo)*t);
        int a = MR32(0x7ff118u), b = MR32(0x7ff11cu);
        split_ob_amount[k] = (int)(a + (b-a)*split_ob_smooth[k]);
        a = MR32(0x7ff120u); b = MR32(0x7ff124u);
        split_ob_threshold[k] = (int)(a + (b-a)*split_ob_smooth[k]);
    }
#endif
}
void sub_005c5f70(CPU *c){
#ifdef __wasm__
    uint32_t ret = MR32(c->esp);
    if (split_racing() && (ret == 0x5cca2au || ret == 0x5cca45u || ret == 0x5cca66u)){
        CPU before = *c, after = *c;
        uint32_t saved = MR32(0x7ff10cu);
        for (int k = 0; k < split_render_limit(); ++k){
            split_post_seat = k; MR32(0x7ff10cu) = split_ob_threshold[k];
            split_post_log(0, k);
            *c = before; sub_005c5f70_orig(c); after = *c;
        }
        split_post_seat = -1; MR32(0x7ff10cu) = saved; *c = after;
        return;
    }
#endif
    sub_005c5f70_orig(c);
}
void sub_005c6be0(CPU *c){
#ifdef __wasm__
    if (split_racing() && MR32(c->esp) == 0x5ccbaau){
        CPU before = *c, after = *c;
        for (int k = 0; k < split_render_limit(); ++k){
            split_post_seat = k;
            uint32_t v = split_ob_amount[k] & 255u;
            reflection_state(c, 0xc1, v * 0x01010101u);
            *c = before; sub_005c6be0_orig(c); after = *c;
        }
        split_post_seat = -1; *c = after;
        uint32_t v = split_ob_amount[0] & 255u;
        reflection_state(c, 0xc1, v * 0x01010101u);
        return;
    }
#endif
    sub_005c6be0_orig(c);
}
void split_rain_begin(CPU *c){
#ifdef __wasm__
    split_rain_seat = -1;
    if (split_racing() && MR32(c->esp) == 0x5cb470u){
        static uint32_t frame_accum;
        int seat = -1;
        for (int k = 0; k < split_render_limit(); ++k)
            if (split_current_view == split_view_id(k)) seat = k;
        if (seat < 0) return;
        if (!seat) frame_accum = MR32(0x8a1d50u);
        else MR32(0x8a1d50u) = frame_accum;
        uint32_t rain = MR32(split_view(split_view_id(seat)) + 0x64);
        if (rain) c->ecx = rain;
        split_rain_seat = seat;
        split_post_log(1, seat);
    }
#endif
}
void split_rain_end(void){
#ifdef __wasm__
    split_rain_seat = -1;
#endif
}
void sub_005c7ab0(CPU *c){
#ifdef __wasm__
    if (split_racing()){
        CPU before = *c, after = *c;
        for (int k = 0; k < split_render_limit(); ++k){
            split_rain_seat = split_post_seat = k;
            split_post_log(2, k);
            *c = before; sub_005c7ab0_orig(c); after = *c;
        }
        split_rain_seat = split_post_seat = -1; *c = after; return;
    }
#endif
    sub_005c7ab0_orig(c);
}
void sub_006142c0(CPU *c){
#ifdef __wasm__
    uint32_t x = MR32(c->esp + 8), y = MR32(c->esp + 12), size = MR32(c->esp + 20);
#endif
    sub_006142c0_orig(c);
#ifdef __wasm__
    if (split_rain_seat >= 0 && x && y && size){
        unsigned w, h; void com_backbuffer_size(unsigned *, unsigned *);
        com_backbuffer_size(&w, &h);
        if (!w || !h) return;
        SplitCell cell = split_cell(split_rain_seat);
        MRF32(x) = (cell.x + MRF32(x)*cell.w) / w;
        MRF32(y) = (cell.y + MRF32(y)*cell.h) / h;
        MRF32(size) *= (float)cell.h / h;
    }
#endif
}
void sub_005d77f0(CPU *c){
#ifdef __wasm__
    uint32_t ctx = MR32(c->esp + 8), view = ctx ? MR32(ctx) : 0;
    uint32_t id = view ? MR32(view + 4) : 0;
    if (split_racing() && id >= 5 && id <= 8){
        static uint8_t tint[4][2][0x30], initialized[4];
        uint8_t saved[2][0x30];
        uint32_t slot = (id - 1) & 1, n = id - 5;
        uint32_t entry[] = {0x88ea08u + slot*0x30, 0x88f210u + slot*0x30};
        for (int t = 0; t < 2; ++t){
            memcpy(saved[t], g_mem + entry[t], 0x30);
            if (!initialized[n]) memcpy(tint[n][t], saved[t], 0x30);
            memcpy(g_mem + entry[t], tint[n][t], 0x30);
        }
        initialized[n] = 1;
        MR32(view + 4) = slot + 1;
        sub_005d77f0_orig(c);
        MR32(view + 4) = id;
        for (int t = 0; t < 2; ++t){
            memcpy(tint[n][t], g_mem + entry[t], 0x30);
            memcpy(g_mem + entry[t], saved[t], 0x30);
        }
        return;
    }
#endif
    sub_005d77f0_orig(c);
}


