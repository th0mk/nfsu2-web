/* Per-camera reflection passes, ported from the MIT-licensed ASI patch.
 * Included after split_world_for; all calls use the guest dispatcher. */
static uint32_t refl_tex[6], refl_surf[6], refl_game_tex, refl_game_surf;
static uint32_t env_cube[6], env_face[6][6];
static int env_valid[6], env_turn = 1, reflection_seat;
#define REFL_SCRATCH 0x00d0a000u

static uint32_t reflection_com(CPU *c, uint32_t obj, unsigned slot, unsigned n, const uint32_t *args){
    uint32_t a[12]; a[0] = obj;
    if (!obj || n > 11) return 0x80004005u;
    for (unsigned i = 0; i < n; ++i) a[i+1] = args[i];
    return gcall(c, MR32(MR32(obj) + slot * 4), 0, n+1, a);
}
static void reflection_release(CPU *c, uint32_t *obj){
    if (*obj) reflection_com(c, *obj, 2, 0, 0);
    *obj = 0;
}
static void reflection_release_all(CPU *c){
    if (refl_game_tex) MR32(0x870894u) = refl_game_tex;
    for (int k = 0; k < 6; ++k){
        reflection_release(c, &refl_surf[k]); reflection_release(c, &refl_tex[k]);
        for (int f = 0; f < 6; ++f) reflection_release(c, &env_face[k][f]);
        reflection_release(c, &env_cube[k]); env_valid[k] = 0;
    }
    refl_game_tex = refl_game_surf = 0; reflection_seat = 0; env_turn = 1;
}
static void reflection_update(CPU *c, uint32_t view){
    uint32_t a[] = {view, 0, 0, 0};
    gcall(c, 0x5c7ec0u, MR32(view), 4, a);
}
static void reflection_recull(CPU *c, uint32_t view){
    uint32_t camera = MR32(view + 0x40);
    if (!split_world || !camera) return;
    unsigned n = MR32(split_world + 0x6c0);
    for (unsigned i = 0; i < n && i < 12; ++i){
        uint32_t entry = split_world + i * 0x90;
        if (MR32(entry + 0x40) != view) continue;
        uint32_t a[] = {view, entry}; gcall(c, 0x5c26e0u, 0, 2, a);
        memcpy(g_mem + entry + 0x60, g_mem + camera + 0x40, 12);
        memcpy(g_mem + entry + 0x70, g_mem + camera + 0x50, 12);
        MR32(entry + 0x80) = MR32(view + 0xc);
        int mode = MR32(0x800df8u), section = -1;
        if (mode == 1 || (mode == 2 && MR32(0x7fb708u) % 0x3d >= 0xf)){
            int x = (int)MRF32(entry + 0x60), y = (int)MRF32(entry + 0x64);
            unsigned cell = (((y >> 5) & 0x7f) << 7) + ((x >> 5) & 0x7f);
            if (!(MR8(0x8832c0u + (cell >> 3)) & (1u << (cell & 7))))
                section = (((y >> 5) & 0x1f) << 5) + ((x >> 5) & 0x1f);
        }
        MR32(entry + 0x84) = section;
        MR32(entry + 0x48) = MR32(entry + 0x4c) = MR32(split_world + 0x6c8);
        MR32(entry + 0x50) = MR32(split_world + 0x6cc);
        gcall(c, 0x5e99c0u, split_world, 1, &entry);
        return; /* lists are consumed immediately; reuse free cull scratch */
    }
}
static void reflection_pass(CPU *c, uint32_t address, uint32_t view, int glow){
    uint32_t pass = MR32(address), dev = MR32(0x870974u);
    if (!pass || !dev) return;
    MR32(0x870910u) = pass;
    gcall(c, MR32(MR32(pass)+4), pass, 0, 0);
    uint32_t shader = MR32(pass+0x24); reflection_com(c, dev, 87, 1, &shader);
    uint32_t effect = MR32(pass+0x20), begin[] = {REFL_SCRATCH+16, 0};
    reflection_com(c, effect, 65, 2, begin);
    if (glow){
        if (MR32(0x870ce4u)){
            uint32_t a[] = {view, 2}; gcall(c, 0x491860u, 0, 2, a);
            a[1] = 1; gcall(c, 0x61ae90u, 0, 2, a); gcall(c, 0x61ace0u, 0, 2, a);
            uint32_t b[] = {view, 0, 1}; gcall(c, 0x494270u, 0, 3, b);
        }
    } else {
        uint32_t a[] = {view, 1};
        if (MR32(0x870cd4u) > 1) gcall(c, 0x630de0u, 0, 2, a);
        gcall(c, 0x615490u, 0, 1, &view);
    }
    gcall(c, MR32(MR32(pass)+8), pass, 0, 0);
    reflection_com(c, effect, 69, 0, 0); MR32(0x870910u) = 0;
}
static void reflection_roads(CPU *c){
    mem_commit(REFL_SCRATCH, 64);
    uint32_t v = split_view(4), rt = MR32(v+0x58), dev = MR32(0x870974u);
    if (!split_racing() || !MR32(0x870cd4u) || !rt || !MR8(v+8) || !MR32(v) || !split_world) return;
    refl_game_tex = MR32(0x870894u); refl_game_surf = MR32(0x870890u);
    if (!refl_game_tex || !refl_game_surf) return;
    uint32_t camera = MR32(v+0x40);
    for (int k = 1; k < split_render_limit(); ++k){
        uint32_t cam = MR32(split_view(split_view_id(k))+0x40);
        if (!cam) continue;
        if (!refl_tex[k]){
            uint32_t a[] = {320, 240, 1, 1, MR32(0x7fee78u), 0, REFL_SCRATCH, 0};
            MR32(REFL_SCRATCH) = 0;
            if (reflection_com(c, dev, 23, 8, a) & 0x80000000u) continue;
            refl_tex[k] = MR32(REFL_SCRATCH);
            uint32_t b[] = {0, REFL_SCRATCH}; MR32(REFL_SCRATCH) = 0;
            reflection_com(c, refl_tex[k], 18, 2, b); refl_surf[k] = MR32(REFL_SCRATCH);
            if (!refl_surf[k]){ reflection_release(c, &refl_tex[k]); continue; }
            fprintf(stderr, "[split reflections] P%d road texture=%08x camera=%08x detail=%u\n", k+1, refl_tex[k], cam, MR32(0x870cd4u));
        }
        MR32(0x870894u) = refl_tex[k]; MR32(0x870890u) = MR32(rt+8) = refl_surf[k];
        MR32(v+0x40) = cam; reflection_seat = k;
        reflection_update(c, v); reflection_recull(c, v);
        uint32_t begin[] = {rt, 1}; gcall(c, 0x5ba3c0u, 0, 2, begin);
        if (MR32(0x870cd4u) > 1){
            uint32_t draw[] = {v, 8}; gcall(c, 0x5d6ad0u, split_world, 2, draw);
            gcall(c, 0x566400u, 0, 1, &v); gcall(c, 0x631250u, 0, 1, &v);
        }
        reflection_pass(c, 0x86eb28u, v, 0);
        gcall(c, 0x5cf420u, 0, 0, 0);
        if (MR32(0x870cd4u) == 3) gcall(c, 0x5c7e40u, 0, 0, 0);
        reflection_pass(c, 0x86eb5cu, v, 1);
    }
    MR32(0x870894u) = refl_game_tex; MR32(0x870890u) = MR32(rt+8) = refl_game_surf;
    MR32(v+0x40) = camera; reflection_seat = 0; reflection_update(c, v);
}
static void reflection_state(CPU *c, uint32_t state, uint32_t value){
    uint32_t a[] = {state, value}; reflection_com(c, MR32(0x870974u), 57, 2, a);
}
static void reflection_env(CPU *c){
    mem_commit(REFL_SCRATCH, 64);
    if (!split_racing() || !MR32(0x870cb8u) || !split_world || !MR32(0x87085cu)) return;
    if (env_turn >= split_render_limit()) env_turn = 1;
    int k = env_turn++;
    uint32_t player = MR32(SPLIT_PLAYERS_INDEX + k*4), car = player ? MR32(player+4) : 0;
    uint32_t camera = MR32(split_view(split_view_id(k))+0x40), dev = MR32(0x870974u);
    if (!car || !camera) return;
    if (!env_cube[k]){
        uint32_t a[] = {MR32(0x7fee6cu), 1, 1, MR32(0x7fee70u), 0, REFL_SCRATCH, 0};
        MR32(REFL_SCRATCH) = 0;
        if (reflection_com(c, dev, 25, 7, a) & 0x80000000u) return;
        env_cube[k] = MR32(REFL_SCRATCH);
        for (unsigned f = 0; f < 6; ++f){
            uint32_t b[] = {f, 0, REFL_SCRATCH}; MR32(REFL_SCRATCH) = 0;
            reflection_com(c, env_cube[k], 18, 3, b); env_face[k][f] = MR32(REFL_SCRATCH);
            if (!env_face[k][f]){
                for (int i = 0; i < 6; ++i) reflection_release(c, &env_face[k][i]);
                reflection_release(c, &env_cube[k]); return;
            }
        }
        fprintf(stderr, "[split reflections] P%d environment cube=%08x car=%08x\n", k+1, env_cube[k], car);
    }
    for (unsigned i = 0; i < 3; ++i) MRF32(REFL_SCRATCH+32+i*4) = MRF32(car+0x50+i*4) + MRF32(0x803e50u+i*4);
    uint32_t v1 = split_view(1), cam1 = MR32(v1+0x40);
    MR32(v1+0x40) = camera;
    uint32_t place[] = {camera+0x40, REFL_SCRATCH+32}; gcall(c, 0x5c3450u, 0x871bc0u, 2, place);
    MR32(v1+0x40) = cam1;
    for (int id = 16; id <= 21; ++id){
        uint32_t v = split_view(id), rt = MR32(v+0x58);
        if (!MR8(v+8) || !rt || !MR32(rt+16) || !MR32(v)) continue;
        uint32_t original = MR32(rt+8); int f = -1;
        for (int i = 0; i < 6; ++i) if (MR32(0x87085cu+i*4) == original) f = i;
        if (f < 0) continue;
        MR32(rt+8) = env_face[k][f]; reflection_update(c, v); reflection_recull(c, v);
        uint32_t a[] = {rt, 1}; gcall(c, 0x5ba3c0u, 0, 2, a);
        reflection_state(c, 7, 0); reflection_state(c, 14, 0);
        gcall(c, 0x60f880u, 0, 1, &v);
        reflection_state(c, 14, 1); reflection_state(c, 7, 1);
        gcall(c, 0x5cf420u, 0, 0, 0); gcall(c, 0x5b9d90u, 0, 0, 0);
        if (MR32(0x870cbcu)) { uint32_t b[] = {v, 0}; gcall(c, 0x5d6ad0u, split_world, 2, b); }
        gcall(c, 0x5cf420u, 0, 0, 0); MR32(rt+8) = original;
    }
    env_valid[k] = 1;
    gcall(c, 0x61ab50u, 0, 0, 0);
    for (int id = 16; id <= 21; ++id) if (MR8(split_view(id)+8) && MR32(split_view(id))) reflection_update(c, split_view(id));
}
static void reflection_use(CPU *c, int k){
    if (refl_game_tex && MR32(0x870890u) == refl_game_surf)
        MR32(0x870894u) = k && refl_tex[k] ? refl_tex[k] : refl_game_tex;
    if (!k || !env_valid[k] || !MR32(0x870cb8u)) return;
    for (int f = 0; f < 6; ++f){
        uint32_t dst = MR32(0x87085cu + f*4);
        if (dst && env_face[k][f]){
            uint32_t a[] = {env_face[k][f], 0, dst, 0, 0};
            reflection_com(c, MR32(0x870974u), 34, 5, a);
        }
    }
}
