/* Native per-player maps, adapted from the MIT ASI minimap_players/hud_split.
 * Reuse the game's map/marker update, snapshot its FE data per camera, then
 * draw each snapshot in that viewport. No independent browser map overlay. */
#define MM_MAX 192
static struct { uint32_t obj, data; int child, group, marker; } mm_obj[MM_MAX];
static uint8_t mm_snap[6][MM_MAX][0x54];
static unsigned mm_count, mm_players, mm_frame, mm_drawn;
static uint32_t mm_marker[6], mm_extended;
static int mm_center = -1, mm_target = -1;
/* Optional integration-test counters: actual rendered marker masks per map. */
static int mm_test = -1;
static const uint32_t mm_colors[6][4] = {
    {60,220,60,255}, {60,60,240,255}, {230,90,40,255},
    {40,210,230,255}, {255,0,160,255}, {220,220,0,255}
};
static int mm_kind(uint32_t obj){
    uint32_t h = MR32(obj + 0x10);
    for (unsigned i = 0; i < 140; ++i) if (h == split_minimap_hashes[i]) return i < 4 ? 1 : 2;
    return 0;
}
static void mm_add(uint32_t obj, int child){
    if (!obj || mm_count >= MM_MAX) return;
    for (unsigned i = 0; i < mm_count; ++i) if (mm_obj[i].obj == obj) return;
    unsigned i = mm_count++;
    mm_obj[i].obj = obj; mm_obj[i].data = MR32(obj + 0x2c);
    mm_obj[i].child = child; mm_obj[i].group = MR32(obj + 0x18) == 5;
    mm_obj[i].marker = mm_kind(obj) == 2;
    if (mm_obj[i].group) for (uint32_t p = MR32(obj + 0x64), guard = 0; p && guard++ < 192; p = MR32(p + 4)) mm_add(p, 1);
}
static uint32_t mm_find(uint32_t pkg, uint32_t hash){
    for (uint32_t p = pkg ? MR32(pkg + 0x4c) : 0, guard = 0; p && guard++ < 2048; p = MR32(p + 4))
        if (MR32(p + 0x10) == hash) return p;
    return 0;
}
static void mm_collect(CPU *c){
    uint32_t pkg[6] = {0}, hashes[2];
    split_minimap_names(c); mm_count = 0;
    memset(mm_marker, 0, sizeof mm_marker);
    for (int k = 0; k < split_render_limit(); ++k){
        uint32_t p = MR32(SPLIT_PLAYERS_INDEX + k*4), hud = p ? MR32(p + 0x9a4) : 0;
        uint32_t name = hud ? MR32(hud + 0x10) : 0;
        if (!name) continue;
        uint32_t e = split_call(c, 0x51bb70u, 0x8637b0u, 1, &name);
        pkg[k] = e ? MR32(e + 0x18) : 0;
    }
    for (unsigned j = 0; j < 2; ++j){
        uint32_t name = 0xd09400u;
        snprintf((char *)g_mem + name, 160, "PlayerCarIndicator_%u", j+1);
        hashes[j] = split_call(c, 0x505450u, 0, 1, &name);
    }
    for (int k = 0; k < split_render_limit(); ++k){
        mm_marker[k] = mm_find(pkg[k & ~1], hashes[k & 1]);
        if (k >= 2) mm_add(mm_marker[k], 0);
    }
    for (uint32_t p = pkg[0] ? MR32(pkg[0] + 0x4c) : 0, guard = 0; p && guard++ < 2048; p = MR32(p + 4))
        if (mm_kind(p)) mm_add(p, 0);
}
static int mm_extend(CPU *c, uint32_t mgr){
    if (mm_extended == mgr) return 1;
    for (int k = 2; k < split_render_limit(); ++k) if (!mm_marker[k]) return 0;
    for (int k = 2; k < split_render_limit(); ++k){
        uint32_t pool = MR32(0x8364b4u), desc = split_call(c, 0x43f8f0u, 0, 1, &pool);
        if (!desc) return 0;
        uint32_t node = split_call(c, 0x440ec0u, 0, 0, 0);
        if (!node) return 0;
        MR32(desc) = mm_marker[k]; MR32(desc+4) = 4;
        uint32_t tail = MR32(0x836564u);
        MR32(node+8) = desc; MR32(tail) = node; MR32(0x836564u) = node;
        MR32(node+4) = tail; MR32(node) = 0x836560u;
        uint32_t a[] = {4, (uint32_t)k};
        if (!split_call(c, 0x4ec790u, mgr, 2, a)) return 0;
    }
    mm_extended = mgr; return 1;
}
static void mm_apply(unsigned seat){
    for (unsigned i = 0; i < mm_count; ++i){
        if (mm_obj[i].data) memcpy(g_mem + mm_obj[i].data, mm_snap[seat][i], 0x54);
        MR32(mm_obj[i].obj + 0x1c) |= 0x400000u;
    }
}
void sub_004ca840(CPU *c){
    if (mm_test < 0){
        mm_test = getenv("NFSU2_SPLIT_MAP_TEST") != NULL;
        if (mm_test) mem_commit(0xd0d000u, 0x80);
    }
    mm_players = 0;
    if (!split_racing()){ sub_004ca840_orig(c); return; }
    mm_collect(c);
    if (!mm_count || !mm_marker[0] || !mm_marker[1] || !mm_extend(c, c->ecx)){
        sub_004ca840_orig(c); return;
    }
    CPU before = *c, after = *c;
    for (int k = split_render_limit()-1; k >= 0; --k){
        mm_center = k; *c = before; sub_004ca840_orig(c); after = *c;
        for (int j = 0; j < split_render_limit(); ++j) if (mm_marker[j]){
            uint32_t data = MR32(mm_marker[j] + 0x2c);
            if (data) memcpy(g_mem + data, mm_colors[j], 16);
            /* Follow native HUD visibility, including pause/cutscene hiding. */
            if (j >= 2) MR32(mm_marker[j]+0x1c) =
                (MR32(mm_marker[j]+0x1c) & ~1u) | (MR32(mm_marker[0]+0x1c) & 1u);
        }
        for (unsigned i = 0; i < mm_count; ++i)
            if (mm_obj[i].data) memcpy(mm_snap[k][i], g_mem + mm_obj[i].data, 0x54);
    }
    mm_center = -1; mm_players = split_render_limit(); ++mm_frame; *c = after;
    if (mm_test) MR32(0xd0d000u) = mm_players;
}
void sub_004ecb80(CPU *c){
    uint32_t mgr = c->ecx;
    if (mgr == mm_extended){
        for (uint32_t e = MR32(mgr), guard = 0; e && e != mgr && guard++ < 256;){
            uint32_t next = MR32(e), desc = MR32(e+0x1c);
            if (desc && MR32(desc+4) == 4 && MR32(e+8) >= 2){
                uint32_t prev = MR32(e+4); MR32(prev) = next; MR32(next+4) = prev;
                uint32_t a[] = {MR32(0x8364b4u), desc}; split_call(c, 0x43f920u, 0, 2, a);
                a[0] = MR32(0x8364b8u); a[1] = e; split_call(c, 0x43f920u, 0, 2, a);
            }
            e = next;
        }
    }
    mm_extended = mm_players = mm_count = 0; mm_center = mm_target = -1;
    if (mm_test > 0) MR32(0xd0d000u) = 0;
    sub_004ecb80_orig(c);
}
static int mm_near(uint32_t obj){
    uint32_t d = MR32(obj + 0x2c);
    return d && fabsf(MRF32(d+28) + 230.f) < 100.f && fabsf(MRF32(d+32) - 5.f) < 100.f;
}
static void mm_render(CPU *c, const CPU *before, uint32_t obj){
    *c = *before; MR32(c->esp+4) = obj;
    MR32(obj+0x1c) |= 0x400000u;
    sub_005370a0_orig(c);
}
void sub_005370a0(CPU *c){
    uint32_t obj = MR32(c->esp+4);
    if (!split_racing() || !mm_players || mm_target >= 0 || !obj){ sub_005370a0_orig(c); return; }
    int at = -1, marker = -1;
    for (unsigned i = 0; i < mm_count; ++i) if (mm_obj[i].obj == obj) at = i;
    for (unsigned i = 0; i < mm_players; ++i) if (mm_marker[i] == obj) marker = i;
    if (at < 0 && !mm_kind(obj)){ sub_005370a0_orig(c); return; }
    CPU before = *c;
    if (marker >= 0){
        if (marker < 2 && mm_drawn != mm_frame){
            mm_drawn = mm_frame;
            for (unsigned k = 0; k < mm_players; ++k){
                mm_apply(k); mm_target = k;
                if (mm_test) MR32(0xd0d010u+k*4) = 0;
                for (unsigned n = 1; n <= mm_players; ++n){
                    unsigned seat = (k+n)%mm_players;
                    uint32_t m = mm_marker[seat];
                    if (m && !(MR32(m+0x1c) & 1u) && mm_near(m)){
                        mm_render(c, &before, m);
                        if (mm_test) MR32(0xd0d010u+k*4) |= 1u << seat;
                    }
                }
            }
        }
    } else if (at >= 0){
        for (unsigned k = 0; k < mm_players; ++k){
            mm_apply(k); mm_target = k;
            if (!mm_obj[at].marker || mm_near(obj)) mm_render(c, &before, obj);
        }
    }
    mm_apply(0); mm_target = -1; *c = before;
    POP32(c->eip); c->esp += 8;
}
static int mm_transform(CPU *c){
    uint32_t data = MR32(c->esp+4), level = MR32(c->esp+16);
    if (!mm_players || !data) return 0;
    for (unsigned i = 0; i < mm_count; ++i) if (mm_obj[i].data == data){
        int group = !level && mm_obj[i].group;
        if (!group && mm_target < 0) return 0;
        uint32_t copy = 0xd0c000u + (split_hud_copy++ & 15u)*0x60u;
        mem_commit(copy, 0x60); memcpy(g_mem + copy, g_mem + data, 0x54);
        if (group){ MRF32(copy+28) = MRF32(copy+32) = 0.f; }
        else {
            unsigned w,h; void com_backbuffer_size(unsigned *, unsigned *); com_backbuffer_size(&w,&h);
            SplitCell cell = split_cell(mm_target);
            float ch = 480.f*cell.h/h, cw = 480.f*cell.w/h;
            float scale = fminf(ch/240.f, cw/320.f), ms = 0.55f*scale;
            float left = 480.f*(cell.x - w*0.5f)/h, bottom = 480.f*(cell.y+cell.h)/h-240.f;
            float x = MRF32(copy+28), y = MRF32(copy+32);
            if (!level){ x += 230.f; y -= 5.f; }
            MRF32(copy+28) = left + 52.f*scale + x*ms;
            MRF32(copy+32) = bottom - 48.f*scale + y*ms;
            for (unsigned j=4;j<=6;++j) MRF32(copy+j*4) *= ms;
            MRF32(copy+56) *= ms; MRF32(copy+60) *= ms;
        }
        MR32(c->esp+4) = copy; return 1;
    }
    return 0;
}
