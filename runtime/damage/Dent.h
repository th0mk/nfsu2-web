#pragma once

#include <math.h>
#include <stdint.h>

namespace damage {

struct Vec3 {
    float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return sqrtf(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
    const float l = Length(a);
    return l > 1e-12f ? a * (1.0f / l) : Vec3{0, 0, 0};
}

// Tuning of the dent shape. Distances in metres, in the car's own space (X forward, Y left, Z up).
struct DentSettings {
    float radiusMin = 0.30f;      // radius of the softest dent
    float radiusPerDepth = 2.5f;  // the radius grows with the depth: radius = radiusMin + depth * radiusPerDepth
    float crumple = 0.35f;        // noise amplitude, as a fraction of the dent depth (0 = smooth bowl)
    float crumpleScale = 9.0f;    // noise frequency, cycles per metre
    float buckle = 0.5f;          // fold height of the panels along the push (hood, fenders, roof), x depth
    float buckleWidth = 1.5f;     // sideways reach of the fold, x dent radius
    float centreHeight = 0.55f;   // height of the car's axis the panels fold away from
    float maxOffset = 0.40f;      // cap of a vertex's total offset from its original position
};

// How far the body may be crushed, in the car's space. The crumple zones end at the wheels: what is ahead
// of the front tyres (behind the rear ones) may be pushed back to the tyre but not past it, and the cabin,
// the sides and the roof only cave in by a limited amount.
struct CrushLimits {
    bool enabled = false;
    float frontX = 1e9f;   // front face of the front tyres
    float rearX = -1e9f;   // rear face of the rear tyres
    float side = 0.12f;    // cabin and sides: furthest a point moves along X or towards the centre (Y)
    float roof = 0.15f;    // furthest a point moves down
};

// One impact, already converted to the car's space.
struct Dent {
    Vec3 point;      // centre of the impact, on the car's surface
    Vec3 direction;  // unit vector the metal is pushed along (into the car)
    float depth;     // displacement at the centre, metres
};

namespace detail {

inline float Hash(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFF) / (float)0xFFFFFF * 2.0f - 1.0f;
}

inline float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise in [-1, 1], continuous: equal positions always give equal values.
inline float Noise(Vec3 p) {
    const float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    const float tx = Smooth(p.x - fx), ty = Smooth(p.y - fy), tz = Smooth(p.z - fz);
    float c[2][2];
    for (int j = 0; j < 2; j++)
        for (int k = 0; k < 2; k++) {
            const float a = Hash(ix, iy + j, iz + k), b = Hash(ix + 1, iy + j, iz + k);
            c[j][k] = a + (b - a) * tx;
        }
    const float c0 = c[0][0] + (c[1][0] - c[0][0]) * ty;
    const float c1 = c[0][1] + (c[1][1] - c[0][1]) * ty;
    return c0 + (c1 - c0) * tz;
}

inline float Bell(float r) { return r >= 1.0f ? 0.0f : (1.0f - r * r) * (1.0f - r * r); }

} // namespace detail

inline float DentRadius(const Dent& d, const DentSettings& s) { return s.radiusMin + d.depth * s.radiusPerDepth; }

// Displacement of the point p. It depends on the position only, so vertices that share a position in
// different parts (a hood edge and its fender) move together and no gap opens between them.
//  - push: a bowl along the impact direction, roughened by noise;
//  - buckle: behind the impact the panels that run along the push fold outwards, away from the car's
//    axis, in one crease halfway into the crushed zone (the hood tents up in a frontal crash).
inline Vec3 DentDisplacement(const Dent& d, const DentSettings& s, Vec3 p) {
    const float radius = DentRadius(d, s);
    const Vec3 rel = p - d.point;
    Vec3 out{0, 0, 0};

    const float bowl = detail::Bell(Length(rel) / radius);
    if (bowl > 0.0f) {
        const float n = detail::Noise(p * s.crumpleScale);
        out = d.direction * (d.depth * bowl * (1.0f + s.crumple * n));
    }

    const float along = Dot(rel, d.direction);   // how deep into the car, from the impact
    if (s.buckle > 0.0f && along > 0.0f && along < radius) {
        const Vec3 side = rel - d.direction * along;
        const float across = detail::Bell(Length(side) / (radius * s.buckleWidth));
        if (across > 0.0f) {
            // Away from the axis through the car's centre parallel to the push.
            const Vec3 c = p - Vec3{0, 0, s.centreHeight};
            const Vec3 radial = c - d.direction * Dot(c, d.direction);
            const float dist = Length(radial);
            if (dist > 1e-4f) {
                const float fade = dist < 0.3f ? dist / 0.3f : 1.0f;
                const float crease = sinf(3.14159265f * along / radius);
                out = out + radial * (s.buckle * d.depth * crease * crease * across * fade / dist);
            }
        }
    }
    return out;
}

// Applies one dent to a vertex array. `rest` holds the original positions (for the offset cap);
// `pos` and `nrm` are updated in place. Strides are in bytes so the game's interleaved vertices
// can be passed directly. Returns how many vertices moved.
//
// Normals follow the inverse transpose of the map's Jacobian J = I + dD/dx (numeric), taken as the
// cofactor matrix: the scale does not matter because the normal is renormalized.
inline void Confine(Vec3& p, Vec3 r0, const CrushLimits& c) {
    if (!c.enabled) return;
    if (r0.x > c.frontX) {
        if (p.x < c.frontX) p.x = c.frontX;
    } else if (r0.x < c.rearX) {
        if (p.x > c.rearX) p.x = c.rearX;
    } else if (fabsf(p.x - r0.x) > c.side) {
        p.x = r0.x + (p.x > r0.x ? c.side : -c.side);
    }
    const float inward = fabsf(r0.y) - fabsf(p.y);
    if (inward > c.side) p.y = (r0.y < 0 ? -1.0f : 1.0f) * (fabsf(r0.y) - c.side);
    if (p.z < r0.z - c.roof) p.z = r0.z - c.roof;
}

inline int ApplyDent(const Dent& d, const DentSettings& s, const CrushLimits& limits, int count,
                     const uint8_t* rest, int restStride,
                     uint8_t* pos, int posStride,
                     uint8_t* nrm, int nrmStride) {
    const float radius = DentRadius(d, s);
    const float reach = radius * (s.buckleWidth > 1.0f ? s.buckleWidth : 1.0f) * 1.5f;
    const float h = radius * 0.02f;   // finite-difference step
    int moved = 0;
    for (int i = 0; i < count; i++) {
        Vec3& p = *(Vec3*)(pos + i * posStride);
        if (Length(p - d.point) >= reach) continue;
        const Vec3 disp = DentDisplacement(d, s, p);
        if (disp.x == 0.0f && disp.y == 0.0f && disp.z == 0.0f) continue;

        if (nrm) {
            Vec3& n = *(Vec3*)(nrm + i * nrmStride);
            const float k = 1.0f / (2 * h);
            const Vec3 jx = Vec3{1, 0, 0} + (DentDisplacement(d, s, {p.x + h, p.y, p.z}) -
                                             DentDisplacement(d, s, {p.x - h, p.y, p.z})) * k;
            const Vec3 jy = Vec3{0, 1, 0} + (DentDisplacement(d, s, {p.x, p.y + h, p.z}) -
                                             DentDisplacement(d, s, {p.x, p.y - h, p.z})) * k;
            const Vec3 jz = Vec3{0, 0, 1} + (DentDisplacement(d, s, {p.x, p.y, p.z + h}) -
                                             DentDisplacement(d, s, {p.x, p.y, p.z - h})) * k;
            // jx, jy, jz are the columns of J; the rows of cof(J) = J^-T * det are their cross products.
            const Vec3 cx = Cross(jy, jz), cy = Cross(jz, jx), cz = Cross(jx, jy);
            const Vec3 m = {cx.x * n.x + cy.x * n.y + cz.x * n.z,
                            cx.y * n.x + cy.y * n.y + cz.y * n.z,
                            cx.z * n.x + cy.z * n.y + cz.z * n.z};
            if (Dot(jx, cx) > 0.05f) n = Normalize(m);   // det(J) > 0: the surface did not fold onto itself
        }

        p = p + disp;
        const Vec3 r0 = *(const Vec3*)(rest + i * restStride);
        const Vec3 off = p - r0;
        const float len = Length(off);
        if (len > s.maxOffset) p = r0 + off * (s.maxOffset / len);
        Confine(p, r0, limits);
        moved++;
    }
    return moved;
}

} // namespace damage
