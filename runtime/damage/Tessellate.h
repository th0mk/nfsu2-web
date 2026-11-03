#pragma once

// Refines a triangle mesh so every edge is shorter than a target length. Game meshes are cut for the
// undamaged shape: a hood may have no vertex for 30 cm along its middle, where a crease should form.
//
// Edges are split at their midpoint, and each split is applied to every triangle that has the same
// edge by position (not by index), so triangles on both sides of a UV seam split together and no
// T-junction (a crack once the surface bends) appears. Each step keeps the mesh conforming, so the
// refinement can stop at any vertex budget.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <queue>
#include <unordered_map>
#include <vector>

namespace damage {

struct Vertex36 {   // the BlackBox car vertex: position, normal, D3DCOLOR, uv
    float pos[3];
    float nrm[3];
    uint32_t colour;
    float uv[2];
};
static_assert(sizeof(Vertex36) == 36, "car vertex is 36 bytes");

struct TessMesh {
    std::vector<Vertex36> verts;
    std::vector<std::vector<uint16_t>> strips;   // triangle lists, one per material
};

namespace detail {

inline uint64_t PosKey(const float* p) {
    const auto q = [](float v) { return (uint64_t)(uint32_t)(int32_t)lroundf(v * 1000.0f) & 0x1FFFFF; };
    return q(p[0]) | q(p[1]) << 21 | q(p[2]) << 42;
}

struct EdgeKey {
    uint64_t a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};
struct EdgeKeyHash {
    size_t operator()(const EdgeKey& k) const { return (size_t)(k.a * 0x9E3779B97F4A7C15ull ^ k.b); }
};

inline EdgeKey MakeEdge(uint64_t p, uint64_t q) { return p < q ? EdgeKey{p, q} : EdgeKey{q, p}; }

inline Vertex36 Midpoint(const Vertex36& a, const Vertex36& b) {
    Vertex36 m;
    for (int i = 0; i < 3; i++) {
        m.pos[i] = (a.pos[i] + b.pos[i]) * 0.5f;
        m.nrm[i] = a.nrm[i] + b.nrm[i];
    }
    const float l = sqrtf(m.nrm[0] * m.nrm[0] + m.nrm[1] * m.nrm[1] + m.nrm[2] * m.nrm[2]);
    for (int i = 0; i < 3; i++) m.nrm[i] = l > 1e-12f ? m.nrm[i] / l : a.nrm[i];
    m.colour = 0;
    for (int s = 0; s < 32; s += 8)
        m.colour |= ((((a.colour >> s) & 255) + ((b.colour >> s) & 255)) / 2) << s;
    m.uv[0] = (a.uv[0] + b.uv[0]) * 0.5f;
    m.uv[1] = (a.uv[1] + b.uv[1]) * 0.5f;
    return m;
}

} // namespace detail

// Splits edges longer than `maxEdge` (metres) while the mesh has fewer than `maxVerts` vertices.
inline void Tessellate(TessMesh& mesh, float maxEdge, size_t maxVerts = 65000) {
    using namespace detail;
    struct Tri {
        uint32_t v[3];
        int strip;
        bool alive;
    };
    std::vector<Tri> tris;
    for (int s = 0; s < (int)mesh.strips.size(); s++)
        for (size_t i = 0; i + 2 < mesh.strips[s].size(); i += 3)
            tris.push_back({{mesh.strips[s][i], mesh.strips[s][i + 1], mesh.strips[s][i + 2]}, s, true});

    std::vector<uint64_t> keys;   // position key per vertex
    for (const Vertex36& v : mesh.verts) keys.push_back(PosKey(v.pos));
    const auto length = [&](uint32_t i, uint32_t j) {
        const float* a = mesh.verts[i].pos;
        const float* b = mesh.verts[j].pos;
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };

    // Longest edge first: then the split edge is the longest of every triangle that has it, which is
    // longest-edge bisection (Rivara) and keeps the triangles from degenerating into slivers.
    struct Work {
        float length;
        EdgeKey key;
        bool operator<(const Work& o) const { return length < o.length; }
    };
    std::unordered_map<EdgeKey, std::vector<uint32_t>, EdgeKeyHash> edgeTris;
    std::priority_queue<Work> work;
    const auto addTri = [&](uint32_t t) {
        const Tri& tri = tris[t];
        for (int e = 0; e < 3; e++) {
            const uint32_t i = tri.v[e], j = tri.v[(e + 1) % 3];
            const EdgeKey k = MakeEdge(keys[i], keys[j]);
            std::vector<uint32_t>& list = edgeTris[k];
            list.push_back(t);
            const float l = length(i, j);
            if (list.size() == 1 && l > maxEdge) work.push({l, k});
        }
    };
    for (uint32_t t = 0; t < tris.size(); t++) addTri(t);

    std::unordered_map<uint64_t, uint32_t> midpoints;   // by vertex index pair
    while (!work.empty() && mesh.verts.size() < maxVerts) {
        const EdgeKey k = work.top().key;
        work.pop();
        auto it = edgeTris.find(k);
        if (it == edgeTris.end()) continue;
        const std::vector<uint32_t> list = std::move(it->second);
        edgeTris.erase(it);
        for (uint32_t t : list) {
            if (!tris[t].alive) continue;
            int e = 0;
            while (e < 3 && !(MakeEdge(keys[tris[t].v[e]], keys[tris[t].v[(e + 1) % 3]]) == k)) e++;
            if (e == 3) continue;
            const uint32_t i = tris[t].v[e], j = tris[t].v[(e + 1) % 3], o = tris[t].v[(e + 2) % 3];
            const uint64_t pair = i < j ? (uint64_t)i << 32 | j : (uint64_t)j << 32 | i;
            uint32_t m;
            auto mid = midpoints.find(pair);
            if (mid != midpoints.end()) {
                m = mid->second;
            } else {
                m = (uint32_t)mesh.verts.size();
                mesh.verts.push_back(Midpoint(mesh.verts[i], mesh.verts[j]));
                keys.push_back(PosKey(mesh.verts[m].pos));
                midpoints[pair] = m;
            }
            const int strip = tris[t].strip;
            tris[t].alive = false;
            tris.push_back({{i, m, o}, strip, true});   // same winding as (i, j, o)
            tris.push_back({{m, j, o}, strip, true});
            addTri((uint32_t)tris.size() - 2);
            addTri((uint32_t)tris.size() - 1);
        }
    }

    for (auto& s : mesh.strips) s.clear();
    for (const Tri& t : tris)
        if (t.alive)
            for (uint32_t v : t.v) mesh.strips[t.strip].push_back((uint16_t)v);
}

} // namespace damage
