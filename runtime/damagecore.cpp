// damagecore.cpp -- the visual crash damage of FRS Mods' "Crashing Foreign Cars" (Damage.asi, NFSU2 v1.2),
// ported to the browser build; runtime/damage.c is its link to the game (hooks, guest calls).
//
// The game draws a part through eModel -> eSolid -> mesh -> vertex/index buffers. eModels are per car
// (CarRenderInfo+0x17A8); eSolids and meshes are shared by every car using the part. On a car's first
// impact each of its body parts' eModels is pointed at a private clone of the eSolid and mesh, refined
// (damage/Tessellate.h) so panels can fold, with its own vertices, indices, strips and buffers; only the
// texture and light-material tables and the effect stay shared. The model stays in the original solid's
// model list, so when the game rebuilds the parts or unloads the pack it moves the model away from our
// clone, which we notice and free a couple of frames later (the draw queue may still reference it).
//
// The same code as the .asi's DamageU2.cpp where it can be: on wasm a guest address is a pointer, so the
// game's structures are read and written in place. What the game reads of a clone (solid, mesh, strips,
// indices, vertices, texture tables) lives in the guest heap; the rest in the runtime's. Direct3D is the
// runtime's own (com.c), called through the device's vtable as the game calls it. Differences:
//  - the refinement ahead of the first impact runs on the game's thread, a few milliseconds a frame,
//    instead of on a worker thread;
//  - no engine or suspension damage, health bar, saved damage, repairs, neon or sounds (not ported);
//  - settings are the .ini's defaults (NFSU2_DAMAGE_LOG=1 for its LogHits).
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <initializer_list>
#include <unordered_map>
#include <vector>

#include "damage/Dent.h"
#include "damage/Tessellate.h"

extern "C" {
uint32_t dmg_call(uint32_t fn, int n, const uint32_t* a);
uint32_t dmg_thiscall(uint32_t fn, uint32_t self, int n, const uint32_t* a);
uint32_t dmg_alloc(uint32_t n);
void dmg_free(uint32_t p);
int dmg_committed(uint32_t a, uint32_t n);
int fileio_resolve_game(const char* win, char* out, size_t outsz);
}

namespace {
using namespace damage;

// ---------------------------------------------------------------------------------------------------------
// Settings: the defaults of the .asi's CrashingForeignCars.ini

struct HangSettings {
    bool enabled = true;
    float hangDamage = 0.18f, popDamage = 0.08f, trunkDamage = 0.06f, mirrorDamage = 0.04f;
    float mirrorPerImpulse = 0.004f, doorDamage = 0.40f, doorShockDepth = 0.20f, fallDamage = 0.70f;
    float popAngleDeg = 6.0f, windOpen = 0.0006f, flutter = 0.6f, damping = 3.0f, bounce = 0.3f;
    float hoodFlySpeed = 36.0f, hoodFlyLift = 7.0f;
};
struct WearSettings {
    const char* pack = "GLOBAL\\DAMAGE.BIN";
    float scratchThreshold = 0.5f, scratchPerImpulse = 0.4f, scratchRadius = 0.45f, scrapeBand = 0.08f;
    const char* scratchMaterial = "MAGGUNMETAL";
    float scratchScale = 2.0f, lightBreakDepth = 0.03f, lightBreakRadius = 0.45f;
    bool nativeGlass = true;
};
struct Settings {
    DentSettings dent;
    HangSettings hang;
    WearSettings wear;
    float impulseThreshold = 4.0f, depthPerImpulse = 0.007f, maxDepthPerHit = 0.20f;
    bool crushAtWheels = true;
    float crushSide = 0.12f, crushRoof = 0.15f, maxEdge = 0.07f;
    int tessellateLods = 1;
    bool logHits = false;
} g_settings;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[damage] %s\n", b);
}

double NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------------------------------------
// SPEED2.EXE v1.2

constexpr uint32_t kDevice = 0x870974;             // IDirect3DDevice9*
constexpr uint32_t kBoundVB = 0x87092C;            // last VB passed to SetStreamSource by the draw loop
constexpr uint32_t kBoundIB = 0x870930;            // last IB passed to SetIndices by the draw loop
constexpr uint32_t kFrameCounter = 0x870818;       // eFrameCounter
constexpr uint32_t kWorldTimer = 0x86518C;         // simulation clock, 1/4000 s ticks; stops while paused
constexpr uint32_t kImpulseMassScale = 0x7A1884;   // float, ApplyImpulse divides 1 by it
// Render's open flags (1 = hood/trunk/door open), at esp + these at 0x620C2B
constexpr size_t kOpenHood = 0x30C, kOpenTrunk = 0xD0, kOpenLeftDoor = 0x184, kOpenRightDoor = 0xFC;
// How far the hood and the trunk lid are open (float, from their opening animation, 0x434610), which Render works
// out before 0x620C2B only for a lid that was open from its start (0x620BD6, 0x620C08), so a lid opened by the
// hook kept 0. The engine's light is the car's scaled by the hood's (0x6304DD): at 0 the engine was black.
constexpr size_t kOpenHoodAmount = 0x158, kOpenTrunkAmount = 0x150;
constexpr size_t kCarTypeInfo = 0x0C;                     // Car
constexpr size_t kTypeFrontAxleX = 0x120, kTypeFrontWheelRadius = 0x130, kTypeRearAxleX = 0x180;
constexpr uint32_t kGetTextureInfo = 0x4901D0;           // cdecl TextureInfo* (hash, int default, int unloaded)
constexpr uint32_t kGetLightMaterial = 0x48F6A0;         // cdecl eLightMaterial* (hash), default if missing
constexpr uint32_t kLoadChunks = 0x577AB0;               // cdecl (void* data, int size, const char* name)
constexpr uint32_t kSetVisibleDamageEnabled = 0x63A4B0;  // thiscall CarDamage::(int)
constexpr size_t kCarDamageVisible = 0x08;                // u8: the game shows window cracks
constexpr size_t kCarPartDamageValues = 0x120;            // CarPartDamage's 6 part damages (float)
constexpr int kCarPartDamageCount = 6;
constexpr uint32_t kClearZoneReturn = 0x5FFEE5;          // Player::ClearZone's call to SetDamageToZero returns here
constexpr size_t kSolidTexCount = 0x19, kSolidLmCount = 0x1A;   // i8 counts
constexpr size_t kSolidTexTable = 0x2C, kSolidLmTable = 0x3C;   // {u32 hash, pointer}[count]
constexpr size_t kStripTexIndex = 0x1C, kStripLmIndex = 0x20, kStripFlags = 0x38;
constexpr uint32_t kStripTransparent = 0x8000;
constexpr size_t kCriCar = 0x00;                          // CarRenderInfo: Car* (null for a front-end car)
constexpr size_t kCriPivot = 0x30, kCriFlares = 0x3F0;    // flare list: bTList of eLightFlare
constexpr size_t kFlareName = 0x08, kFlarePos = 0x10;     // pos relative to the pivot (BODY LOD A)
constexpr uint32_t kLightFlares[] = {0x9DB90133, 0xD09091C6, 0x31A66786, 0xBF700A79};   // L/R head, L/R brake
constexpr int kSlotHeadlight = 14, kSlotBrakelight = 15;
constexpr int kSlotRoofScoop = 7, kSlotSpoiler = 12, kSlotWingMirror = 32;
constexpr int kSlotExhaust = 16, kSlotPlate = 33;
constexpr size_t kCarVelocity = 0x70, kCarMatrix = 0xD0;
constexpr int kScratchLevels = 4;
constexpr int kOverlayStrips = kScratchLevels + 1;        // + the cracked lens
constexpr float kOverlayLift = 0.003f;                    // metres the overlay floats over the surface
constexpr int kMaxRefinedVerts = 32000;                   // the overlay doubles the vertices; indices are 16-bit

constexpr size_t kBodyCar = 0x130, kBodyRigidBody = 0x68;      // CarCollisionBody
constexpr size_t kRigidOrientation = 0x30, kRigidMassTerm = 0x168, kRigidVelocity = 0xA0;
constexpr size_t kRigidCgOffset = 0x1F0;   // the body's position is the car's origin + R * this
constexpr size_t kCarRenderInfo = 0x554, kCarDamage = 0x958;   // Car
constexpr size_t kCriModels = 0x17A8;                          // eModel* [63 slots][2 rows][4 LODs]
constexpr size_t kModelSolid = 0x0C;                           // eModel
constexpr size_t kSolidSize = 0xC0, kSolidNode = 0x04, kSolidBoxMin = 0x20, kSolidBoxMax = 0x30;
constexpr size_t kSolidModelList = 0x88;
constexpr size_t kMeshSize = 0x44, kMeshStripCount = 0x10, kMeshStrips = 0x14, kMeshIndices = 0x18;
constexpr size_t kMeshIndexBytes = 0x1C, kMeshIB = 0x20, kMeshVerts = 0x28, kMeshVB = 0x2C;
constexpr size_t kMeshBytes = 0x30, kMeshCount = 0x34;
constexpr size_t kStripSize = 0x3C, kStripBoxMin = 0x00, kStripIndexCount = 0x0C, kStripBoxMax = 0x10;
constexpr size_t kStripIndexPtr = 0x34;   // pointer into the mesh's index data
constexpr int kStride = 36;               // pos f32x3, normal f32x3, colour, uv f32x2
constexpr float kStripBoxMargin = 1.5f;   // loose parts swing outside the undamaged box
constexpr float kPi = 3.14159265f;

// CAR_SLOT_ID values that crumple (the engine too, crushed with the nose). Wheels, brakes and the spinner
// have their own transforms; the exhaust tips and the plate are drawn once per marker (see MarkerDraw).
constexpr int kDentSlots[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 32,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62};
constexpr size_t kCriSpoilerMarker = 0x2C04, kCriRoofScoopMarker = 0x2C08, kCriMirrorMarkers = 0x2C0C;
constexpr size_t kMarkerMatrix = 0x10;   // ePositionMarker: hash, iParam, fParam0, fParam1, bMatrix4

// Direct3D 9 (the runtime's com.c): vtable slots and values
constexpr int kDevCreateVB = 26, kDevCreateIB = 27, kRelease = 2, kLock = 11, kUnlock = 12;
constexpr uint32_t kUsageWriteOnly = 8, kPoolManaged = 1, kFmtIndex16 = 101;

// ---------------------------------------------------------------------------------------------------------
// Guest access

template <typename T>
T& At(const void* base, size_t off) {
    return *(T*)((uint8_t*)base + off);
}
inline uint8_t* G(uint32_t a) { return (uint8_t*)(uintptr_t)a; }
inline uint32_t A(const void* p) { return (uint32_t)(uintptr_t)p; }
inline uint32_t R32(uint32_t a) { return *(volatile uint32_t*)G(a); }

uint32_t Frame() { return R32(kFrameCounter); }
uint32_t WorldTime() { return R32(kWorldTimer); }
uint32_t Device() { return R32(kDevice); }

// A COM method of the runtime's Direct3D: stdcall, `self` first.
uint32_t Com(uint32_t self, int slot, std::initializer_list<uint32_t> args) {
    uint32_t a[8];
    int n = 0;
    a[n++] = self;
    for (uint32_t v : args) a[n++] = v;
    return dmg_call(R32(R32(self) + 4u * slot), n, a);
}

uint32_t g_out = 0;   // guest dword for the COM calls' out parameters

void* GAlloc(uint32_t n) { return G(dmg_alloc(n)); }
void GFree(void* p) {
    if (p) dmg_free(A(p));
}

// The car still exists and still draws through this render info.
bool CarHas(uint8_t* car, void* renderInfo) {
    return car && dmg_committed(A(car) + kCarRenderInfo, 4) && At<void*>(car, kCarRenderInfo) == renderInfo;
}

// bStringHash
uint32_t HashName(const char* s) {
    uint32_t h = 0xFFFFFFFF;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

// Rodrigues rotation of v about the unit axis a by angle t.
Vec3 Rotate(Vec3 v, Vec3 a, float t) {
    const float c = cosf(t), s = sinf(t);
    return v * c + Cross(a, v) * s + a * (Dot(a, v) * (1.0f - c));
}

// ---------------------------------------------------------------------------------------------------------
// Loose parts

enum class HangKind {
    Droop,   // one end tears off and falls about the forward axis through the other end (bumpers, mirrors)
    Lift,    // a lid whose latch breaks: turns about the sideways axis through its hinged edge
    Door,    // a side door: turns about the vertical axis through its front edge
};
enum class Push { Any, Frontal, Rear, FromLeft, FromRight };   // which impacts count for the part

struct HangSpec {
    const char* name;
    int slots[4];     // the first gives the part's box; the others move with it (panels, decals); -1 ends
    int row;          // model row of the slots, -1 for both (mirrors: row 0 left, row 1 right)
    HangKind kind;
    Push push;
    float side;       // Droop: free end, +1 left / -1 right / 0 the side that was hit
                      // Lift: -1 hinge at the rear edge (hood), +1 at the front edge (trunk lid)
                      // Door: +1 left door, -1 right door
    float maxDeg;     // furthest swing (a drooping part also stops at the ground)
    float wind;       // Lift/Door: +1 the airflow opens it, -1 shuts it
    float fallScale;  // the part tears off at fallDamage * this; 0 = never
    int parent = -1;  // index of the spec it is mounted on: it also follows that part's swing and fall
};

constexpr int kSpecTrunk = 5, kSpecLeftDoor = 6, kSpecRightDoor = 7;
const HangSpec kHangSpecs[] = {
    {"front bumper", {1, -1}, -1, HangKind::Droop, Push::Any, 0, 60, 0, 0},
    {"rear bumper", {2, -1}, -1, HangKind::Droop, Push::Any, 0, 60, 0, 0},
    {"left mirror", {32, -1}, 0, HangKind::Droop, Push::Any, 1, 80, 0, 1.0f, kSpecLeftDoor},
    {"right mirror", {32, -1}, 1, HangKind::Droop, Push::Any, -1, 80, 0, 1.0f, kSpecRightDoor},
    {"hood", {9, 25, 52, -1}, -1, HangKind::Lift, Push::Frontal, -1, 70, 1, 1.4f},
    {"trunk", {10, 26, -1}, -1, HangKind::Lift, Push::Rear, 1, 70, -1, 1.4f},
    {"left door", {17, 19, 55, 59}, -1, HangKind::Door, Push::FromLeft, 1, 70, -1, 0},
    {"right door", {18, 20, 56, 60}, -1, HangKind::Door, Push::FromRight, -1, 70, -1, 0},
    // Bends down at the end that was hit and rides on the trunk lid when that opens.
    {"spoiler", {12, -1}, -1, HangKind::Droop, Push::Any, 0, 25, 0, 0.5f, kSpecTrunk},
};
constexpr int kHangCount = sizeof(kHangSpecs) / sizeof(kHangSpecs[0]);

struct HangPart {
    float damage[2] = {0, 0};   // Droop with side 0: per side (left, right); otherwise [0] only
    bool loose = false;
    bool fallen = false;
    Vec3 hinge{0, 0, 0};
    Vec3 axis{1, 0, 0};         // positive angles open the part
    float lever = 0.5f;         // hinge to the far end, metres
    float maxAngle = 0;         // radians
    float angle = 0, rate = 0;  // radians, radians/s
    float shownAngle = 0;       // state of the vertices last uploaded
    bool shownFallen = false;
    bool flying = false;        // a lid the airflow tore off, flying over the car before it is gone
    float flyTime = 0;
    Vec3 offset{0, 0, 0};       // car space, added after the swing
};

struct CarState {
    uint8_t* car = nullptr;
    uint8_t* rigid = nullptr;   // the car's RigidBody, from the impulse that created this state
    CrushLimits crush;          // from the car type's wheels
    struct BrokenLight {
        uint8_t* flare;
        float z;                // the flare's height before it was sunk out of sight
    };
    std::vector<BrokenLight> brokenLights;
    Vec3 lastVelocity{0, 0, 0};
    bool hasVelocity = false;
    float flutterPhase = 0;
    HangPart parts[kHangCount];
};

bool SpecHas(const HangSpec& spec, int slot, int row) {
    if (spec.row >= 0 && spec.row != row) return false;
    for (int s : spec.slots) {
        if (s < 0) return false;
        if (s == slot) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------
// Clones

// Parts drawn on a marker (Render: M = Marker.Matrix * Body) have their vertices in the marker's space. The
// clone keeps them in car space, like every other part, and puts them back on upload.
struct Placement {
    bool on = false;
    float m[9], inv[9];   // row vectors: car = local * m + t
    Vec3 t{0, 0, 0};

    static Vec3 Mul(const float* a, Vec3 v) {
        return {v.x * a[0] + v.y * a[3] + v.z * a[6], v.x * a[1] + v.y * a[4] + v.z * a[7],
                v.x * a[2] + v.y * a[5] + v.z * a[8]};
    }
    Vec3 ToCar(Vec3 p) const { return Mul(m, p) + t; }
    Vec3 ToLocal(Vec3 p) const { return Mul(inv, p - t); }
    Vec3 NormalToCar(Vec3 n) const { return Normalize(Mul(m, n)); }
    Vec3 NormalToLocal(Vec3 n) const { return Normalize(Mul(inv, n)); }

    bool Set(const float* matrix16) {
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) m[r * 3 + k] = matrix16[r * 4 + k];
        t = {matrix16[12], matrix16[13], matrix16[14]};
        const float c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
        const float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
        if (fabsf(det) < 1e-8f) return false;
        const float d = 1.0f / det;
        const float r[9] = {c0 * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
                            c1 * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
                            c2 * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d};
        memcpy(inv, r, sizeof(r));
        on = true;
        return true;
    }
};

struct Clone {
    uint8_t* model = nullptr;
    uint8_t* original = nullptr;   // the shared eSolid
    // what the game reads: in the guest heap
    uint8_t* solid = nullptr;
    uint8_t* mesh = nullptr;
    uint8_t* strips = nullptr;
    uint16_t* indices = nullptr;
    Vertex36* verts = nullptr;     // dented, before any loose-part swing
    uint8_t* texTable = nullptr;   // the solid's tables plus our textures and light material
    uint8_t* lmTable = nullptr;
    uint32_t vb = 0, ib = 0;       // IDirect3DVertexBuffer9*, IDirect3DIndexBuffer9*
    // ours
    std::vector<Vertex36> rest;    // as refined, before any damage
    void* renderInfo = nullptr;
    int slot = -1, row = 0, lod = 0;
    int count = 0;
    int stripCount = 0;
    bool dirty = false;
    uint32_t retiredAt = 0;

    // Overlay: a second copy of the vertices, lifted off the surface and mapped onto the scratch (or
    // cracked lens) texture, drawn by extra transparent strips over the scratched (cracked) triangles.
    bool overlay = false;           // room for it (the vertex count leaves 16-bit indices for the copy)
    bool light = false;             // a headlight or brake light: cracks instead of scratches
    std::vector<float> scratch;     // 0..1 per vertex
    std::vector<uint8_t> crack;     // 1 per cracked vertex
    std::vector<float> overlayUv;   // 2 per vertex
    int overlayTex[kOverlayStrips] = {-1, -1, -1, -1, -1};
    int scratchLm = -1, crackLm = 0;
    int baseIndexCount = 0;
    int drawStrips = 0;
    uint8_t overlayStrip[kStripSize] = {};
    bool overlayDirty = false;

    Placement place;                // vertices are kept in car space; this puts them back on the marker
    Vec3 boxMin{0, 0, 0}, boxMax{0, 0, 0};   // undamaged shape, car space

    // Parts the game draws once per marker (the exhaust tips, the plates) get one clone per draw: the
    // model keeps its original solid and the clone is swapped in only around that draw (see MarkerDraw).
    int instance = -1;
    uint64_t key = 0;
};

uint64_t CloneKey(const uint8_t* model, int instance) {
    return (uint64_t)A(model) | (uint64_t)(uint32_t)(instance + 1) << 32;
}

std::unordered_map<uint64_t, Clone*> g_clones;     // by CloneKey(eModel, instance)
std::unordered_map<void*, CarState*> g_cars;       // by CarRenderInfo

// Where the game draws each exhaust tip and plate (RY(90) * marker, car space), learnt from every draw of
// every car (before any damage), by CarRenderInfo.
struct MarkerDrawn {
    uint8_t* model;
    int index, slot, row;
    Placement place;
};
std::unordered_map<void*, std::vector<MarkerDrawn>> g_markerDraws;
std::vector<Clone*> g_retired;
uint32_t g_lastFrame = 0xFFFFFFFF;
uint32_t g_lastWorldTime = 0;

int SpecOf(const Clone* c) {
    for (int i = 0; i < kHangCount; i++)
        if (SpecHas(kHangSpecs[i], c->slot, c->row)) return i;
    return -1;
}

const HangPart* HangFor(const Clone* c) {
    auto it = g_cars.find(c->renderInfo);
    const int spec = SpecOf(c);
    return it == g_cars.end() || spec < 0 ? nullptr : &it->second->parts[spec];
}

// The loose parts that move this clone, innermost first: its own, then the part it is mounted on.
int HangChain(const Clone* c, const HangPart* chain[2]) {
    auto it = g_cars.find(c->renderInfo);
    int n = 0;
    for (int spec = SpecOf(c); it != g_cars.end() && spec >= 0 && n < 2; spec = kHangSpecs[spec].parent)
        chain[n++] = &it->second->parts[spec];
    return n;
}

// Writes the clone's vertices to its buffer, swinging loose parts about their hinges (a mirror with its
// door), followed by the overlay copy of the scratched and cracked ones.
bool Upload(Clone* c) {
    const HangPart* chain[2];
    const int links = HangChain(c, chain);
    bool fallen = false;
    for (int k = 0; k < links; k++) fallen = fallen || chain[k]->fallen;
    At<int>(c->mesh, kMeshStripCount) = fallen ? 0 : c->drawStrips;
    c->dirty = false;
    if (fallen) return true;

    // A part in flight leaves its boxes far behind: open them up so it is not culled on the way.
    for (int k = 0; k < links; k++)
        if (chain[k]->flying)
            for (int s = 0; s < c->drawStrips; s++)
                for (int a = 0; a < 3; a++) {
                    At<float>(c->strips + s * kStripSize, kStripBoxMin + a * 4) = -60.0f;
                    At<float>(c->strips + s * kStripSize, kStripBoxMax + a * 4) = 60.0f;
                }

    if (!c->vb || Com(c->vb, kLock, {0, 0, g_out, 0}) != 0) return false;
    Vertex36* p = (Vertex36*)G(R32(g_out));
    if (!p) return false;
    for (int i = 0; i < c->count; i++) {
        Vertex36 v = c->verts[i];
        Vec3 pos = *(Vec3*)v.pos, nrm = *(Vec3*)v.nrm;
        for (int k = 0; k < links; k++) {
            const HangPart* h = chain[k];
            const bool moved = h->offset.x != 0.0f || h->offset.y != 0.0f || h->offset.z != 0.0f;
            if (!h->loose || (h->angle == 0.0f && !h->flying && !moved)) continue;
            pos = h->hinge + Rotate(pos - h->hinge, h->axis, h->angle) + h->offset;
            nrm = Rotate(nrm, h->axis, h->angle);
        }
        const auto put = [&](Vertex36& out, Vec3 at) {
            const Vec3 lp = c->place.on ? c->place.ToLocal(at) : at;
            const Vec3 ln = c->place.on ? c->place.NormalToLocal(nrm) : nrm;
            memcpy(out.pos, &lp, 12);
            memcpy(out.nrm, &ln, 12);
        };
        put(v, pos);
        p[i] = v;
        if (c->overlay && (c->scratch[i] > 0 || c->crack[i])) {
            put(v, pos + nrm * kOverlayLift);
            v.uv[0] = c->overlayUv[i * 2];
            v.uv[1] = c->overlayUv[i * 2 + 1];
            p[c->count + i] = v;
        }
    }
    Com(c->vb, kUnlock, {});
    return true;
}

// Rebuilds the overlay strips from the per-vertex scratch and crack state: a triangle joins the scratch
// strip of its strength (four textures of growing opacity) once all its corners are scratched.
void RebuildOverlay(Clone* c) {
    c->overlayDirty = false;
    if (!c->overlay) return;
    std::vector<uint16_t> lists[kOverlayStrips];
    const uint16_t* base = c->indices;
    for (int t = 0; t + 2 < c->baseIndexCount; t += 3) {
        const uint16_t a = base[t], b = base[t + 1], d = base[t + 2];
        int list = -1;
        if (c->crack[a] && c->crack[b] && c->crack[d]) {
            list = kScratchLevels;
        } else if (c->scratch[a] > 0 && c->scratch[b] > 0 && c->scratch[d] > 0) {
            const float w = (c->scratch[a] + c->scratch[b] + c->scratch[d]) / 3.0f;
            list = (int)(w * kScratchLevels - 0.001f);
            list = list < 0 ? 0 : list >= kScratchLevels ? kScratchLevels - 1 : list;
        }
        if (list < 0 || c->overlayTex[list] < 0) continue;
        for (uint16_t v : {a, b, d}) lists[list].push_back((uint16_t)(v + c->count));
    }

    int strips = c->stripCount;
    uint16_t* out = c->indices + c->baseIndexCount;
    for (int k = 0; k < kOverlayStrips; k++) {
        if (lists[k].empty()) continue;
        uint8_t* strip = c->strips + strips++ * kStripSize;
        memcpy(strip, c->overlayStrip, kStripSize);
        At<int>(strip, kStripTexIndex) = c->overlayTex[k];
        At<int>(strip, kStripLmIndex) = k == kScratchLevels ? c->crackLm : c->scratchLm;
        At<int>(strip, kStripIndexCount) = (int)lists[k].size();
        At<uint32_t>(strip, kStripIndexPtr) = A(out);
        memcpy(out, lists[k].data(), lists[k].size() * 2);
        out += lists[k].size();
    }
    c->drawStrips = strips;
    if (Com(c->ib, kLock, {0, 0, g_out, 0}) == 0 && R32(g_out)) {
        memcpy(G(R32(g_out)), c->indices, (out - c->indices) * 2);
        Com(c->ib, kUnlock, {});
    }
    c->dirty = true;
}

void Free(Clone* c) {
    if (c->vb) {
        if (R32(kBoundVB) == c->vb) At<uint32_t>(G(kBoundVB), 0) = 0;
        Com(c->vb, kRelease, {});
    }
    if (c->ib) {
        if (R32(kBoundIB) == c->ib) At<uint32_t>(G(kBoundIB), 0) = 0;
        Com(c->ib, kRelease, {});
    }
    GFree(c->verts);
    GFree(c->texTable);
    GFree(c->lmTable);
    GFree(c->indices);
    GFree(c->strips);
    GFree(c->mesh);
    GFree(c->solid);
    delete c;
}

// The model still draws the clone (the game has not rebuilt or released it). A per-draw clone's model keeps
// the original solid.
bool Live(const Clone* c) {
    return dmg_committed(A(c->model) + kModelSolid, 4) &&
           At<uint8_t*>(c->model, kModelSolid) == (c->instance < 0 ? c->solid : c->original);
}

// Takes the clone out of use. `restore` puts the original solid back on the model (repair).
void Retire(Clone* c, bool restore) {
    if (restore && c->instance < 0 && Live(c)) At<uint8_t*>(c->model, kModelSolid) = c->original;
    g_clones.erase(c->key);
    c->retiredAt = Frame();
    g_retired.push_back(c);
}

// Calls f(model, slot, row, lod) once per distinct body-part model of the car.
template <typename F>
void ForEachPartModel(void* renderInfo, F f) {
    uint8_t** table = (uint8_t**)((uint8_t*)renderInfo + kCriModels);
    uint8_t* done[sizeof(kDentSlots) / sizeof(kDentSlots[0]) * 8];
    int doneCount = 0;
    for (int slot : kDentSlots)
        for (int i = 0; i < 8; i++) {   // 2 rows x 4 LODs
            uint8_t* model = table[slot * 8 + i];
            if (!model || !At<uint8_t*>(model, kModelSolid)) continue;
            bool seen = false;   // a missing LOD reuses the previous LOD's model
            for (int k = 0; k < doneCount && !seen; k++) seen = done[k] == model;
            if (seen) continue;
            done[doneCount++] = model;
            f(model, slot, i / 4, i % 4);
        }
}

// ---------------------------------------------------------------------------------------------------------
// Refinement ahead of the impact. Refining a car's parts (Tessellate) is most of the work of its first impact;
// done there, the game froze at the first hit of every car. Instead each racer's parts are queued as soon as
// the car is drawn and refined a few milliseconds a frame; the first impact finds the results here. A result
// is keyed by the content of its source mesh, so it never goes to another part that came to sit at the same
// address. A part not reached yet is refined on the spot.

constexpr size_t kTessCacheBytes = 64u << 20;   // results kept (least recently used dropped beyond)
constexpr int kPrewarmPerFrame = 6;              // source meshes copied per frame
constexpr double kRefineMsPerFrame = 3.0;        // refinement per frame ahead of the impacts

struct TessJob {
    bool done = false;
    TessMesh mesh;          // the source until done, then the refined mesh
    size_t bytes = 0;       // of the refined mesh
    uint32_t lastUse = 0;   // frame
};

std::unordered_map<uint64_t, TessJob> g_tessCache;   // by SourceKey
std::deque<uint64_t> g_tessQueue;
size_t g_tessBytes = 0;
struct Prewarmed {
    const void* verts;   // the solid's mesh when it was queued (a solid's memory may be reused for another)
    int count;
    uint64_t key;
};
std::unordered_map<const void*, Prewarmed> g_prewarmed;   // by source solid
int g_prewarmBudget = 0;
int g_refinedAhead = 0, g_refinedNow = 0;

size_t TessBytes(const TessMesh& t) {
    size_t n = t.verts.size() * sizeof(Vertex36);
    for (const auto& s : t.strips) n += s.size() * 2;
    return n;
}

// A model's source mesh as MakeClone uses it: its vertices and per-strip triangle lists.
bool ReadSource(uint8_t* model, TessMesh& t) {
    uint8_t* original = At<uint8_t*>(model, kModelSolid);
    uint8_t* mesh = original ? At<uint8_t*>(original, 0) : nullptr;
    if (!mesh) return false;
    const int count = At<int>(mesh, kMeshCount);
    const int stripCount = At<int>(mesh, kMeshStripCount);
    const uint8_t* srcVerts = At<uint8_t*>(mesh, kMeshVerts);
    const uint8_t* srcStrips = At<uint8_t*>(mesh, kMeshStrips);
    if (count <= 0 || At<int>(mesh, kMeshBytes) != count * kStride || !srcVerts || stripCount <= 0 || !srcStrips ||
        !At<uint16_t*>(mesh, kMeshIndices))
        return false;
    t.verts.assign((const Vertex36*)srcVerts, (const Vertex36*)srcVerts + count);
    t.strips.resize(stripCount);
    for (int s = 0; s < stripCount; s++) {
        const uint8_t* strip = srcStrips + s * kStripSize;
        const uint16_t* first = At<uint16_t*>(strip, kStripIndexPtr);
        t.strips[s].assign(first, first + At<int>(strip, kStripIndexCount));
    }
    return true;
}

// FNV-1a over the source's vertices and triangle lists.
uint64_t SourceKey(const TessMesh& t) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    const uint32_t* w = (const uint32_t*)t.verts.data();
    for (size_t i = 0, n = t.verts.size() * sizeof(Vertex36) / 4; i < n; i++) mix(w[i]);
    for (const auto& s : t.strips) {
        mix((uint32_t)s.size());
        for (uint16_t v : s) mix(v);
    }
    return h;
}

void Finish(TessJob& job) {
    job.bytes = TessBytes(job.mesh);
    job.done = true;
    job.lastUse = Frame();
    g_tessBytes += job.bytes;
}

// Replaces the source `t` with its refined shape: from the cache when it is there, otherwise refined here.
void Refine(TessMesh& t) {
    const uint64_t key = SourceKey(t);
    TessJob& job = g_tessCache[key];
    if (job.done) {
        job.lastUse = Frame();
        t = job.mesh;
        g_refinedAhead++;
        return;
    }
    Tessellate(t, g_settings.maxEdge, kMaxRefinedVerts);
    g_refinedNow++;
    job.mesh = t;
    Finish(job);
}

// Queues the refinement of one model's source mesh, unless it is known (then it is kept from being dropped).
void PrewarmModel(uint8_t* model) {
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    uint8_t* mesh = solid ? At<uint8_t*>(solid, 0) : nullptr;
    if (!mesh) return;
    const void* verts = At<void*>(mesh, kMeshVerts);
    const int count = At<int>(mesh, kMeshCount);
    auto known = g_prewarmed.find(solid);
    if (known != g_prewarmed.end() && known->second.verts == verts && known->second.count == count) {
        auto it = g_tessCache.find(known->second.key);
        if (it != g_tessCache.end()) {
            it->second.lastUse = Frame();
            return;
        }
    }
    if (g_prewarmBudget <= 0) return;
    g_prewarmBudget--;
    TessMesh src;
    if (!ReadSource(model, src)) return;
    const uint64_t key = SourceKey(src);
    if (g_prewarmed.size() > 4096) g_prewarmed.clear();
    g_prewarmed[solid] = {verts, count, key};
    if (g_tessCache.count(key)) return;
    TessJob& job = g_tessCache[key];
    job.mesh = std::move(src);
    job.lastUse = Frame();
    g_tessQueue.push_back(key);
}

// Refines queued sources for a few milliseconds.
void RunPrewarm() {
    const double t0 = NowMs();
    while (!g_tessQueue.empty() && NowMs() - t0 < kRefineMsPerFrame) {
        const uint64_t key = g_tessQueue.front();
        g_tessQueue.pop_front();
        auto it = g_tessCache.find(key);
        if (it == g_tessCache.end() || it->second.done) continue;   // dropped, or refined at an impact
        Tessellate(it->second.mesh, g_settings.maxEdge, kMaxRefinedVerts);
        Finish(it->second);
    }
}

// Refines a racer's parts ahead of its first impact (called on its draws; cheap once they are all known).
void Prewarm(void* renderInfo) {
    if (g_settings.tessellateLods <= 0) return;
    ForEachPartModel(renderInfo, [&](uint8_t* model, int, int, int lod) {
        if (lod >= g_settings.tessellateLods) return;
        auto it = g_clones.find(CloneKey(model, -1));
        if (it != g_clones.end() && Live(it->second)) return;   // cloned already
        PrewarmModel(model);
    });
    // The parts drawn per marker are refined at LOD A (see EnsureClones).
    uint8_t** table = (uint8_t**)((uint8_t*)renderInfo + kCriModels);
    for (int slot : {kSlotExhaust, kSlotPlate})
        for (int row = 0; row < 2; row++) PrewarmModel(table[slot * 8 + row * 4]);
}

// Once a second: drops the least recently used results beyond the cache's size.
void TrimTessCache() {
    static uint32_t last = 0;
    if (Frame() - last < 60) return;
    last = Frame();
    while (g_tessBytes > kTessCacheBytes) {
        auto oldest = g_tessCache.end();
        for (auto it = g_tessCache.begin(); it != g_tessCache.end(); ++it)
            if (it->second.done && (oldest == g_tessCache.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == g_tessCache.end()) break;
        g_tessBytes -= oldest->second.bytes;
        g_tessCache.erase(oldest);
    }
}

Clone* MakeClone(uint8_t* model, void* renderInfo, int slot, int row, int lod, int instance = -1,
                 const Placement* instancePlace = nullptr) {
    uint8_t* original = At<uint8_t*>(model, kModelSolid);
    const uint32_t device = Device();
    TessMesh t;
    if (!device || !ReadSource(model, t)) return nullptr;
    uint8_t* mesh = At<uint8_t*>(original, 0);
    const int stripCount = (int)t.strips.size();
    const uint8_t* srcStrips = At<uint8_t*>(mesh, kMeshStrips);
    if (lod < g_settings.tessellateLods) Refine(t);

    Clone* c = new Clone;
    c->model = model;
    c->original = original;
    c->renderInfo = renderInfo;
    c->slot = slot;
    c->row = row;
    c->lod = lod;
    c->count = (int)t.verts.size();
    c->stripCount = c->drawStrips = stripCount;
    c->light = slot == kSlotHeadlight || slot == kSlotBrakelight;
    c->overlay = c->count * 2 <= 65535;
    size_t indexCount = 0;
    for (const auto& s : t.strips) indexCount += s.size();
    c->baseIndexCount = (int)indexCount;
    const int copies = c->overlay ? 2 : 1;
    const uint32_t vbBytes = c->count * kStride * copies, ibBytes = (uint32_t)(indexCount * 2 * copies);
    const int texCount = At<int8_t>(original, kSolidTexCount), lmCount = At<int8_t>(original, kSolidLmCount);
    c->verts = (Vertex36*)GAlloc(c->count * kStride);
    c->rest = t.verts;
    c->scratch.assign(c->count, 0.0f);
    c->crack.assign(c->count, 0);
    c->overlayUv.resize(c->count * 2);
    c->indices = (uint16_t*)GAlloc(ibBytes + 2);
    c->strips = (uint8_t*)GAlloc((stripCount + kOverlayStrips) * kStripSize);
    c->texTable = (uint8_t*)GAlloc((texCount + kOverlayStrips) * 8);
    c->lmTable = (uint8_t*)GAlloc((lmCount + 1) * 8);
    c->mesh = (uint8_t*)GAlloc(0x50);
    c->solid = (uint8_t*)GAlloc(kSolidSize);
    if (!c->verts || !c->indices || !c->strips || !c->texTable || !c->lmTable || !c->mesh || !c->solid ||
        Com(device, kDevCreateVB, {vbBytes, kUsageWriteOnly, 0, kPoolManaged, g_out, 0}) != 0 ||
        !(c->vb = R32(g_out)) ||
        Com(device, kDevCreateIB, {ibBytes, 0, kFmtIndex16, kPoolManaged, g_out, 0}) != 0 || !(c->ib = R32(g_out))) {
        Log("slot %d: no memory or buffers for its clone", slot);
        Free(c);
        return nullptr;
    }
    // Marker parts (as Render places them): the spoiler and roof scoop always when the car has the marker,
    // the mirrors only when aftermarket (modelled around their own origin; stock ones are in car space).
    const uint8_t* marker = nullptr;
    bool flipY = false;
    if (slot == kSlotSpoiler) marker = At<uint8_t*>(renderInfo, kCriSpoilerMarker);
    if (slot == kSlotRoofScoop) marker = At<uint8_t*>(renderInfo, kCriRoofScoopMarker);
    if (slot == kSlotWingMirror) {
        const Vec3 centre = (At<Vec3>(original, kSolidBoxMin) + At<Vec3>(original, kSolidBoxMax)) * 0.5f;
        if (fabsf(centre.y) < 0.3f) marker = At<uint8_t*>(renderInfo, kCriMirrorMarkers + row * 4);
        flipY = row == 1;   // Render negates m[1][1] of the right marker
    }
    if (marker) {
        float m16[16];
        memcpy(m16, marker + kMarkerMatrix, sizeof(m16));
        if (flipY) m16[5] = -m16[5];
        c->place.Set(m16);
    }
    c->instance = instance;
    c->key = CloneKey(model, instance);
    if (instancePlace) c->place = *instancePlace;
    if (c->place.on)
        for (Vertex36& v : t.verts) {
            const Vec3 p = c->place.ToCar(*(Vec3*)v.pos), n = c->place.NormalToCar(*(Vec3*)v.nrm);
            memcpy(v.pos, &p, 12);
            memcpy(v.nrm, &n, 12);
        }
    c->boxMin = c->boxMax = *(Vec3*)t.verts[0].pos;
    for (const Vertex36& v : t.verts)
        for (int k = 0; k < 3; k++) {
            (&c->boxMin.x)[k] = v.pos[k] < (&c->boxMin.x)[k] ? v.pos[k] : (&c->boxMin.x)[k];
            (&c->boxMax.x)[k] = v.pos[k] > (&c->boxMax.x)[k] ? v.pos[k] : (&c->boxMax.x)[k];
        }

    memcpy(c->verts, t.verts.data(), c->count * kStride);
    c->rest = t.verts;

    // Overlay mapping: a box projection along the dominant axis of the undamaged normal, in car space.
    const float scale = c->light ? 0.35f : g_settings.wear.scratchScale;
    for (int i = 0; i < c->count; i++) {
        const Vec3 p = *(Vec3*)c->rest[i].pos, n = *(Vec3*)c->rest[i].nrm;
        const float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
        const float u = ax >= ay && ax >= az ? p.y : p.x;
        const float v = az > ax && az > ay ? p.y : p.z;
        c->overlayUv[i * 2] = 0.5f + u / scale;
        c->overlayUv[i * 2 + 1] = 0.5f - v / scale;
    }

    // Strips: same materials, our index ranges, boxes grown for parts that swing loose.
    memcpy(c->strips, srcStrips, stripCount * kStripSize);
    size_t at = 0;
    Vec3 boxMin{1e9f, 1e9f, 1e9f}, boxMax{-1e9f, -1e9f, -1e9f};
    for (int s = 0; s < stripCount; s++) {
        uint8_t* strip = c->strips + s * kStripSize;
        memcpy(c->indices + at, t.strips[s].data(), t.strips[s].size() * 2);
        At<uint32_t>(strip, kStripIndexPtr) = A(c->indices + at);
        At<int>(strip, kStripIndexCount) = (int)t.strips[s].size();
        for (int k = 0; k < 3; k++) {
            float& lo = At<float>(strip, kStripBoxMin + k * 4);
            float& hi = At<float>(strip, kStripBoxMax + k * 4);
            lo -= kStripBoxMargin;
            hi += kStripBoxMargin;
            (&boxMin.x)[k] = lo < (&boxMin.x)[k] ? lo : (&boxMin.x)[k];
            (&boxMax.x)[k] = hi > (&boxMax.x)[k] ? hi : (&boxMax.x)[k];
        }
        at += t.strips[s].size();
    }
    if (Com(c->ib, kLock, {0, 0, g_out, 0}) == 0 && R32(g_out)) {
        memcpy(G(R32(g_out)), c->indices, indexCount * 2);
        Com(c->ib, kUnlock, {});
    }

    // The overlay strips start as a copy of the first transparent strip (a lamp's glass) or the first one,
    // marked transparent and boxed around the whole part.
    int glass = 0;
    for (int s = 0; s < stripCount; s++)
        if (At<uint32_t>(c->strips + s * kStripSize, kStripFlags) & kStripTransparent) {
            glass = s;
            break;
        }
    memcpy(c->overlayStrip, c->strips + glass * kStripSize, kStripSize);
    At<uint32_t>(c->overlayStrip, kStripFlags) |= kStripTransparent;
    At<Vec3>(c->overlayStrip, kStripBoxMin) = boxMin;
    At<Vec3>(c->overlayStrip, kStripBoxMax) = boxMax;
    c->crackLm = At<int>(c->overlayStrip, kStripLmIndex);

    // Texture and light-material tables: the solid's, then ours. The game keeps replacing entries by hash
    // (paint, per-car skins), which works the same on the copies.
    memcpy(c->texTable, At<uint8_t*>(original, kSolidTexTable), texCount * 8);
    memcpy(c->lmTable, At<uint8_t*>(original, kSolidLmTable), lmCount * 8);
    static const char* kOverlayNames[kOverlayStrips] = {"DAMAGE_SCRATCH1", "DAMAGE_SCRATCH2", "DAMAGE_SCRATCH3",
                                                        "DAMAGE_SCRATCH4", "DAMAGE_LENS_CRACK"};
    int texTotal = texCount;
    for (int k = 0; k < kOverlayStrips; k++) {
        if (c->light != (k == kScratchLevels)) continue;   // lamps crack, the rest scratches
        const uint32_t hash = HashName(kOverlayNames[k]);
        const uint32_t args[3] = {hash, 0, 0};
        const uint32_t info = dmg_call(kGetTextureInfo, 3, args);
        if (!info) continue;
        At<uint32_t>(c->texTable, texTotal * 8) = hash;
        At<uint32_t>(c->texTable, texTotal * 8 + 4) = info;
        c->overlayTex[k] = texTotal++;
    }
    int lmTotal = lmCount;
    if (!c->light) {
        const uint32_t hash = HashName(g_settings.wear.scratchMaterial);
        At<uint32_t>(c->lmTable, lmTotal * 8) = hash;
        At<uint32_t>(c->lmTable, lmTotal * 8 + 4) = dmg_call(kGetLightMaterial, 1, &hash);
        c->scratchLm = lmTotal++;
    }

    // Own copies of the mesh and solid headers, linked into no game list.
    memcpy(c->mesh, mesh, kMeshSize);
    At<uint32_t>(c->mesh, 0) = A(c->mesh);
    At<uint32_t>(c->mesh, 4) = A(c->mesh);
    At<uint32_t>(c->mesh, kMeshStrips) = A(c->strips);
    At<uint32_t>(c->mesh, kMeshIndices) = A(c->indices);
    At<int>(c->mesh, kMeshIndexBytes) = (int)ibBytes;
    At<uint32_t>(c->mesh, kMeshIB) = c->ib;
    At<uint32_t>(c->mesh, kMeshVerts) = A(c->verts);
    At<uint32_t>(c->mesh, kMeshVB) = c->vb;
    At<int>(c->mesh, kMeshBytes) = (int)vbBytes;
    At<int>(c->mesh, kMeshCount) = c->count * copies;   // DrawIndexedPrimitive's NumVertices covers the overlay
    memcpy(c->solid, original, kSolidSize);
    At<uint32_t>(c->solid, 0) = A(c->mesh);
    At<int8_t>(c->solid, kSolidTexCount) = (int8_t)texTotal;
    At<int8_t>(c->solid, kSolidLmCount) = (int8_t)lmTotal;
    At<uint32_t>(c->solid, kSolidTexTable) = A(c->texTable);
    At<uint32_t>(c->solid, kSolidLmTable) = A(c->lmTable);
    At<uint32_t>(c->solid, kSolidNode) = A(c->solid + kSolidNode);
    At<uint32_t>(c->solid, kSolidNode + 4) = A(c->solid + kSolidNode);
    At<uint32_t>(c->solid, kSolidModelList) = A(c->solid + kSolidModelList);
    At<uint32_t>(c->solid, kSolidModelList + 4) = A(c->solid + kSolidModelList);

    Upload(c);
    if (instance < 0) At<uint8_t*>(model, kModelSolid) = c->solid;   // stays in the original's list on purpose
    g_clones[c->key] = c;
    return c;
}

// Clone of this model for this car, made on demand.
Clone* CloneFor(uint8_t* model, void* renderInfo, int slot, int row, int lod) {
    auto it = g_clones.find(CloneKey(model, -1));
    if (it != g_clones.end()) {
        Clone* c = it->second;
        if (Live(c) && c->renderInfo == renderInfo) return c;
        Retire(c, false);
    }
    return MakeClone(model, renderInfo, slot, row, lod);
}

// The contact comes from the collision hull, which does not follow the bodywork. Slides the point along
// the push to the first vertex the other object would touch: among the LOD A vertices within `width` of
// the line through the point, the one furthest out against the push.
bool SnapToSurface(void* renderInfo, Dent& dent, float width) {
    float best = 1e30f;
    ForEachPartModel(renderInfo, [&](uint8_t* model, int, int, int lod) {
        if (lod != 0) return;
        const uint8_t* v;
        int n;
        auto it = g_clones.find(CloneKey(model, -1));
        if (it != g_clones.end() && Live(it->second)) {   // the dented shape
            v = (const uint8_t*)it->second->verts;
            n = it->second->count;
        } else {
            uint8_t* mesh = At<uint8_t*>(At<uint8_t*>(model, kModelSolid), 0);
            if (!mesh || At<int>(mesh, kMeshBytes) != At<int>(mesh, kMeshCount) * kStride) return;
            v = At<uint8_t*>(mesh, kMeshVerts);
            n = At<int>(mesh, kMeshCount);
        }
        for (int i = 0; i < n; i++, v += kStride) {
            const Vec3 rel = *(const Vec3*)v - dent.point;
            const float t = Dot(rel, dent.direction);
            if (t < best && Length(rel - dent.direction * t) < width) best = t;
        }
    });
    if (best == 1e30f) return false;
    dent.point = dent.point + dent.direction * best;
    return true;
}

// Box of a part's undamaged LOD A shape, in car space (from its clone, which handles marker parts).
bool PartBox(void* renderInfo, int slot, int row, Vec3& mn, Vec3& mx) {
    uint8_t* model = ((uint8_t**)((uint8_t*)renderInfo + kCriModels))[slot * 8 + row * 4];
    uint8_t* solid = model ? At<uint8_t*>(model, kModelSolid) : nullptr;
    if (!solid) return false;
    auto it = g_clones.find(CloneKey(model, -1));
    if (it != g_clones.end() && it->second->solid == solid) {
        mn = it->second->boxMin;
        mx = it->second->boxMax;
        return true;
    }
    mn = At<Vec3>(solid, kSolidBoxMin);
    mx = At<Vec3>(solid, kSolidBoxMax);
    return mx.x > mn.x;
}

float DistanceToBox(Vec3 p, Vec3 mn, Vec3 mx) {
    const auto axis = [](float v, float a, float b) { return v < a ? a - v : v > b ? v - b : 0.0f; };
    const Vec3 d{axis(p.x, mn.x, mx.x), axis(p.y, mn.y, mx.y), axis(p.z, mn.z, mx.z)};
    return Length(d);
}

// The largest droop angle that keeps the swung LOD A part above the ground.
float GroundLimit(void* renderInfo, const HangSpec& spec, const HangPart& h, float limit) {
    const Clone* lodA = nullptr;
    for (auto& kv : g_clones) {
        const Clone* c = kv.second;
        if (c->renderInfo == renderInfo && c->slot == spec.slots[0] && c->lod == 0 && SpecHas(spec, c->slot, c->row))
            lodA = c;
    }
    if (!lodA) return limit;
    for (float t = 0.0f; t < limit; t += 0.02f)
        for (int i = 0; i < lodA->count; i += 3) {
            const Vec3 p = h.hinge + Rotate(*(const Vec3*)lodA->verts[i].pos - h.hinge, h.axis, t);
            if (p.z < 0.03f) return t;
        }
    return limit;
}

bool PushCounts(Push push, Vec3 dir) {
    switch (push) {
        case Push::Frontal: return dir.x < -0.3f;   // corner hits count too
        case Push::Rear: return dir.x > 0.3f;
        case Push::FromLeft: return dir.y < -0.3f;
        case Push::FromRight: return dir.y > 0.3f;
        default: return true;
    }
}

// Tears the part loose: places its hinge and opening axis from its undamaged box.
void Loosen(void* renderInfo, const HangSpec& spec, HangPart& h, Vec3 mn, Vec3 mx, int hitSide) {
    const Vec3 centre = (mn + mx) * 0.5f;
    h.loose = true;
    h.maxAngle = spec.maxDeg * kPi / 180.0f;
    switch (spec.kind) {
        case HangKind::Droop: {
            // The free end falls; the part turns about the forward axis through the other end.
            const bool leftFree = spec.side != 0 ? spec.side > 0 : hitSide == 0;
            // Mirrors and the spoiler turn about the bottom of their mount.
            const bool mounted = spec.slots[0] == kSlotWingMirror || spec.slots[0] == kSlotSpoiler;
            h.hinge = {centre.x, leftFree ? mn.y : mx.y, mounted ? mn.z : mx.z};
            h.axis = {leftFree ? -1.0f : 1.0f, 0, 0};
            h.lever = mx.y - mn.y;
            h.maxAngle = GroundLimit(renderInfo, spec, h, h.maxAngle);
            break;
        }
        case HangKind::Lift:   // hood: hinge at the rear edge, opens forward up; trunk lid: the opposite
            h.hinge = {spec.side < 0 ? mn.x : mx.x, centre.y, mx.z};
            h.axis = {0, spec.side, 0};
            h.lever = mx.x - mn.x;
            h.rate = 4.0f;
            break;
        case HangKind::Door:   // hinge at the front edge, on the outer face; the rear edge swings out
            h.hinge = {mx.x, spec.side > 0 ? mx.y : mn.y, centre.z};
            h.axis = {0, 0, -spec.side};
            h.lever = mx.x - mn.x;
            h.rate = 3.0f;
            break;
    }
}

// Adds an impact to the loose-part bookkeeping and tears parts loose or off when they had enough.
void DamageParts(void* renderInfo, CarState& car, const Dent& dent) {
    const HangSettings& hs = g_settings.hang;
    const float radius = DentRadius(dent, g_settings.dent);
    for (int i = 0; i < kHangCount; i++) {
        const HangSpec& spec = kHangSpecs[i];
        HangPart& h = car.parts[i];
        Vec3 mn, mx;
        if (h.fallen || !PartBox(renderInfo, spec.slots[0], spec.row < 0 ? 0 : spec.row, mn, mx)) continue;
        // A violent impact anywhere on a side springs that side's door, whatever the direction.
        if (spec.kind == HangKind::Door && !h.loose && dent.depth >= hs.doorShockDepth &&
            dent.point.y * spec.side > 0) {
            Loosen(renderInfo, spec, h, mn, mx, 0);
            continue;
        }
        if (!PushCounts(spec.push, dent.direction)) continue;
        // The hull reports contacts near the ground: for the lids and the spoiler, high on the body, the
        // distance is taken flat (as for the mirrors), so the hits on the bumpers below them count.
        Vec3 at = dent.point;
        if (spec.kind == HangKind::Lift || spec.slots[0] == kSlotSpoiler) at.z = (mn.z + mx.z) * 0.5f;
        const float d = DistanceToBox(at, mn, mx);
        if (d >= radius) continue;
        const float amount = dent.depth * (1.0f - d / radius);

        const bool perSide = spec.kind == HangKind::Droop && spec.side == 0;
        const int side = perSide && dent.point.y < (mn.y + mx.y) * 0.5f ? 1 : 0;
        h.damage[side] += amount;
        const float total = h.damage[0] + h.damage[1];
        const float threshold = spec.kind == HangKind::Droop ? hs.hangDamage
                              : spec.kind == HangKind::Door  ? hs.doorDamage
                              : spec.side < 0                ? hs.popDamage     // hood
                                                             : hs.trunkDamage;
        if (spec.fallScale > 0 && total > hs.fallDamage * spec.fallScale)
            h.fallen = true;
        else if (!h.loose && h.damage[side] > threshold)
            Loosen(renderInfo, spec, h, mn, mx, side);
    }
}

// Makes sure every body part of the car has its clone (scratches can come before any dent), and every
// exhaust tip and plate drawn so far its own.
void EnsureClones(void* renderInfo) {
    ForEachPartModel(renderInfo, [&](uint8_t* model, int slot, int row, int lod) {
        CloneFor(model, renderInfo, slot, row, lod);
    });
    auto drawn = g_markerDraws.find(renderInfo);
    if (drawn == g_markerDraws.end()) return;
    uint8_t** table = (uint8_t**)((uint8_t*)renderInfo + kCriModels);
    for (const MarkerDrawn& d : drawn->second) {
        if (table[d.slot * 8 + d.row * 4] != d.model) continue;   // learnt before the parts were rebuilt
        auto it = g_clones.find(CloneKey(d.model, d.index));
        if (it != g_clones.end()) {
            if (Live(it->second) && it->second->renderInfo == renderInfo) continue;
            Retire(it->second, false);
        }
        if (At<uint8_t*>(d.model, kModelSolid)) MakeClone(d.model, renderInfo, d.slot, d.row, 0, d.index, &d.place);
    }
}

int DentCar(void* renderInfo, const CarState& car, const Dent& dent) {
    EnsureClones(renderInfo);
    int moved = 0;
    for (auto& kv : g_clones) {
        Clone* c = kv.second;
        if (c->renderInfo != renderInfo) continue;
        const int n = ApplyDent(dent, g_settings.dent, car.crush, c->count, (const uint8_t*)c->rest.data(), kStride,
                                (uint8_t*)c->verts, kStride, (uint8_t*)c->verts + 12, kStride);
        if (n) c->dirty = true;
        moved += n;
    }
    return moved;
}

// Scratches the paint around a contact: strength falls off with the distance and builds up over contacts.
void ScratchCar(void* renderInfo, Vec3 point, float amount, float radius) {
    for (auto& kv : g_clones) {
        Clone* c = kv.second;
        if (c->renderInfo != renderInfo || c->light || !c->overlay) continue;
        bool changed = false;
        for (int i = 0; i < c->count; i++) {
            // Stretched vertically: the hull reports low contacts, a scrape marks the whole panel height.
            Vec3 rel = *(Vec3*)c->verts[i].pos - point;
            rel.z *= 0.5f;
            const float d = Length(rel);
            if (d >= radius || c->scratch[i] >= 1.0f) continue;
            const float f = 1.0f - d / radius;
            const float s = c->scratch[i] + amount * f * f;
            c->scratch[i] = s > 1.0f ? 1.0f : s;
            changed = true;
        }
        if (changed) c->overlayDirty = true;
    }
}

// A light contact taken as a scrape against a flat surface (a wall, the side of a car): the surface is the
// plane across the push, laid against the outermost point of the car, and whatever lies within `scrapeBand`
// of it touches. What touches is scratched, and a mirror that touches is knocked loose bit by bit.
void ScrapeSurface(void* renderInfo, CarState& car, Vec3 push, float amount, float magnitude) {
    const Vec3 out = push * -1.0f;   // towards the surface
    const auto skip = [](const Clone* c) {
        const HangPart* h = HangFor(c);
        return h && (h->loose || h->fallen);
    };
    float outerAll = -1e9f, outerBody = -1e9f;
    for (auto& kv : g_clones) {
        const Clone* c = kv.second;
        if (c->renderInfo != renderInfo || c->lod != 0 || skip(c)) continue;
        const bool mirror = c->slot == kSlotWingMirror;
        for (int i = 0; i < c->count; i++) {
            const float s = Dot(*(Vec3*)c->verts[i].pos, out);
            outerAll = s > outerAll ? s : outerAll;
            if (!mirror) outerBody = s > outerBody ? s : outerBody;
        }
    }
    if (outerBody == -1e9f) return;
    const float band = g_settings.wear.scrapeBand;

    bool mirrorTouched[2] = {false, false};
    for (auto& kv : g_clones) {
        Clone* c = kv.second;
        if (c->renderInfo != renderInfo || !c->overlay || skip(c)) continue;
        const bool mirror = c->slot == kSlotWingMirror;
        const float outer = mirror ? outerAll : outerBody;
        bool changed = false;
        for (int i = 0; i < c->count; i++) {
            const float gap = outer - Dot(*(Vec3*)c->verts[i].pos, out);
            if (gap >= band) continue;
            if (mirror && c->lod == 0) mirrorTouched[c->row & 1] = true;
            if (c->light || c->scratch[i] >= 1.0f) continue;
            const float s = c->scratch[i] + amount * (1.0f - gap / band);
            c->scratch[i] = s > 1.0f ? 1.0f : s;
            changed = true;
        }
        if (changed) c->overlayDirty = true;
    }

    if (!g_settings.hang.enabled) return;
    const HangSettings& hs = g_settings.hang;
    for (int i = 0; i < kHangCount; i++) {
        const HangSpec& spec = kHangSpecs[i];
        HangPart& h = car.parts[i];
        Vec3 mn, mx;
        if (spec.slots[0] != kSlotWingMirror || spec.row < 0 || !mirrorTouched[spec.row] || h.fallen ||
            !PartBox(renderInfo, spec.slots[0], spec.row, mn, mx))
            continue;
        h.damage[0] += magnitude * hs.mirrorPerImpulse;
        if (spec.fallScale > 0 && h.damage[0] > hs.fallDamage * spec.fallScale)
            h.fallen = true;
        else if (!h.loose && h.damage[0] > hs.mirrorDamage)
            Loosen(renderInfo, spec, h, mn, mx, 0);
    }
}

// Breaks the headlights and brake lights near a dent: the flare is sunk out of sight and the lens cracks.
void BreakLights(void* renderInfo, CarState& car, const Dent& dent) {
    const WearSettings& ws = g_settings.wear;
    if (dent.depth < ws.lightBreakDepth) return;
    uint8_t* head = (uint8_t*)renderInfo + kCriFlares;
    const Vec3 pivot = At<Vec3>(renderInfo, kCriPivot);
    for (uint8_t* f = At<uint8_t*>(head, 0); f && f != head; f = At<uint8_t*>(f, 0)) {
        const uint32_t name = At<uint32_t>(f, kFlareName);
        bool lamp = false;
        for (uint32_t h : kLightFlares) lamp = lamp || h == name;
        bool broken = false;
        for (const auto& b : car.brokenLights) broken = broken || b.flare == f;
        if (!lamp || broken) continue;
        const Vec3 pos = At<Vec3>(f, kFlarePos) + pivot;
        if (Length(pos - dent.point) >= ws.lightBreakRadius) continue;

        car.brokenLights.push_back({f, At<float>(f, kFlarePos + 8)});
        At<float>(f, kFlarePos + 8) = -1000.0f;
        for (auto& kv : g_clones) {
            Clone* c = kv.second;
            if (c->renderInfo != renderInfo || !c->light || !c->overlay) continue;
            for (int i = 0; i < c->count; i++)
                if (Length(*(Vec3*)c->rest[i].pos - pos) < 0.3f) c->crack[i] = 1;
            c->overlayDirty = true;
        }
        if (g_settings.logHits) Log("  light %08X broken", name);
    }
}

void RestoreLights(CarState& car) {
    for (const auto& b : car.brokenLights) At<float>(b.flare, kFlarePos + 8) = b.z;
    car.brokenLights.clear();
}

// `alive`: the car's render info still exists, so its flares can be put back.
void Forget(void* renderInfo, bool alive) {
    auto it = g_cars.find(renderInfo);
    if (it == g_cars.end()) return;
    CarState* car = it->second;
    if (alive) RestoreLights(*car);
    delete car;
    g_cars.erase(it);
}

void RepairRenderInfo(void* renderInfo) {
    std::vector<Clone*> mine;
    for (auto& kv : g_clones)
        if (!renderInfo || kv.second->renderInfo == renderInfo) mine.push_back(kv.second);
    for (Clone* c : mine) Retire(c, true);
    if (renderInfo) {
        Forget(renderInfo, true);
    } else {
        for (auto& kv : g_cars) {
            RestoreLights(*kv.second);
            delete kv.second;
        }
        g_cars.clear();
    }
}

void SetCar(CarState& state, uint8_t* car) {
    state.car = car;
    if (const uint8_t* type = car ? At<uint8_t*>(car, kCarTypeInfo) : nullptr) {
        const float radius = At<float>(type, kTypeFrontWheelRadius);
        state.crush.enabled = g_settings.crushAtWheels;
        state.crush.frontX = At<float>(type, kTypeFrontAxleX) + radius;
        state.crush.rearX = At<float>(type, kTypeRearAxleX) - radius;
        state.crush.side = g_settings.crushSide;
        state.crush.roof = g_settings.crushRoof;
    }
}

CarState* StateFor(void* renderInfo, uint8_t* car, bool hit) {
    auto known = g_cars.find(renderInfo);
    if (known != g_cars.end()) return known->second;
    if (!hit) return nullptr;
    CarState* state = new CarState;
    g_cars[renderInfo] = state;
    SetCar(*state, car);
    return state;
}

// Swings the loose parts of one car, as hinged plates driven by the car's acceleration (inertia), gravity
// and the airflow. Droopers fall and bounce off the ground; lids rest slightly open and flutter; doors
// swing out under braking and in turns and slam against the body when the airflow shuts them.
void SwingParts(CarState& car, void* renderInfo, float dt) {
    if (!CarHas(car.car, renderInfo) || dt <= 0) return;
    const HangSettings& hs = g_settings.hang;
    const float* m = car.rigid ? &At<float>(car.rigid, kRigidOrientation) : &At<float>(car.car, kCarMatrix);
    const Vec3 ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]}, az{m[8], m[9], m[10]};
    const Vec3 vw = car.rigid ? At<Vec3>(car.rigid, kRigidVelocity) : At<Vec3>(car.car, kCarVelocity);
    const Vec3 v{Dot(vw, ax), Dot(vw, ay), Dot(vw, az)};
    Vec3 a{0, 0, 0};
    if (car.hasVelocity) a = (v - car.lastVelocity) * (1.0f / dt);
    car.lastVelocity = v;
    car.hasVelocity = true;
    const auto clamp = [](float x, float lim) { return x < -lim ? -lim : x > lim ? lim : x; };
    a = {clamp(a.x, 40), clamp(a.y, 40), clamp(a.z, 40)};
    const float speed = Length(v);
    const float airflow = speed * speed * hs.windOpen;
    car.flutterPhase += dt * (6.0f + speed * 0.4f);
    const float gust = sinf(car.flutterPhase) * sinf(car.flutterPhase * 2.3f + 1.0f);

    for (int i = 0; i < kHangCount; i++) {
        const HangSpec& spec = kHangSpecs[i];
        HangPart& h = car.parts[i];
        if (!h.loose || h.fallen) continue;
        // At speed the airflow tears an open hood away: it flips over the car, rises and drops behind.
        if (spec.kind == HangKind::Lift && spec.wind > 0 && !h.flying && speed > hs.hoodFlySpeed &&
            h.angle > 0.15f) {
            h.flying = true;
            h.flyTime = 0;
        }
        if (h.flying) {
            h.flyTime += dt;
            const float t = h.flyTime;
            h.angle += dt * 7.0f;
            h.offset = {-9.0f * t * t, 0.0f, hs.hoodFlyLift * t - 4.9f * t * t};
            if (t > 1.8f) h.fallen = true;
            continue;
        }
        const float lever = h.lever > 0.2f ? h.lever : 0.2f;
        for (float left = dt; left > 1e-5f; left -= 1.0f / 120) {
            const float step = left < 1.0f / 120 ? left : 1.0f / 120;
            const float c = cosf(h.angle), s = sinf(h.angle);
            float acc = 0;
            switch (spec.kind) {
                case HangKind::Droop:   // gravity, plus the car's vertical bumps
                    acc = 1.5f * (9.8f + a.z) * c / lever;
                    break;
                case HangKind::Lift: {   // a spring towards the resting gap, the airflow, bumps and gusts
                    float rest = hs.popAngleDeg * kPi / 180.0f + spec.wind * airflow;
                    rest = rest < 0 ? 0 : rest > h.maxAngle ? h.maxAngle : rest;
                    acc = 25.0f * (rest - h.angle) - 1.5f * a.z * c / lever +
                          hs.flutter * speed * speed * 0.004f * gust;
                    break;
                }
                case HangKind::Door:   // braking swings it open, turns away from its side too
                    acc = 1.5f * (-a.x * s - a.y * spec.side * c) / lever + spec.wind * airflow * 20.0f * c +
                          hs.flutter * speed * 0.05f * gust;
                    break;
            }
            h.rate += (acc - hs.damping * h.rate) * step;
            h.angle += h.rate * step;
            if (h.angle > h.maxAngle) {
                h.angle = h.maxAngle;
                if (h.rate > 0) h.rate = -h.rate * hs.bounce;
            } else if (h.angle < 0) {   // slams shut against the body (or rests in place)
                h.angle = 0;
                if (h.rate < 0) h.rate = -h.rate * hs.bounce;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Once a frame (the first car render of the frame)

void OnFrame() {
    const uint32_t frame = Frame();
    if (frame == g_lastFrame) return;
    g_lastFrame = frame;

    // Game time: loose parts stop while the game is paused.
    const uint32_t now = WorldTime();
    float dt = g_lastWorldTime && now >= g_lastWorldTime ? (now - g_lastWorldTime) / 4000.0f : 0.0f;
    g_lastWorldTime = now;
    if (dt > 0.05f) dt = 0.05f;

    g_prewarmBudget = kPrewarmPerFrame;
    RunPrewarm();
    TrimTessCache();

    for (size_t i = 0; i < g_retired.size();)
        if (frame - g_retired[i]->retiredAt >= 2) {
            Free(g_retired[i]);
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            i++;
        }

    std::vector<Clone*> stale;
    for (auto& kv : g_clones)
        if (!Live(kv.second)) stale.push_back(kv.second);
    for (Clone* c : stale) Retire(c, false);

    // A car whose clones are all gone was repaired, rebuilt or destroyed: its state goes too, and its
    // lights come back if the car still has this render info.
    std::vector<void*> gone;
    for (auto& kv : g_cars) {
        bool any = false;
        for (auto& c : g_clones) any = any || c.second->renderInfo == kv.first;
        if (!any) gone.push_back(kv.first);
    }
    for (void* r : gone) Forget(r, CarHas(g_cars[r]->car, r));

    for (auto& kv : g_cars) {
        CarState& car = *kv.second;
        SwingParts(car, kv.first, dt);
        // The game's own part damage hides an aftermarket mirror past 0.25; the loose parts are ours.
        if (CarHas(car.car, kv.first))
            if (uint8_t* carDamage = At<uint8_t*>(car.car, kCarDamage))
                memset(carDamage + kCarPartDamageValues, 0, kCarPartDamageCount * sizeof(float));
        for (int i = 0; i < kHangCount; i++) {
            HangPart& h = car.parts[i];
            if (h.fallen == h.shownFallen && fabsf(h.angle - h.shownAngle) <= 0.002f) continue;
            for (auto& c : g_clones) {
                if (c.second->renderInfo != kv.first) continue;
                const int spec = SpecOf(c.second);   // the part itself, or one mounted on it (a mirror)
                if (spec == i || (spec >= 0 && kHangSpecs[spec].parent == i)) c.second->dirty = true;
            }
            h.shownFallen = h.fallen;
            h.shownAngle = h.angle;
        }
    }

    for (auto& kv : g_clones) {
        if (kv.second->overlayDirty) RebuildOverlay(kv.second);
        if (kv.second->dirty) Upload(kv.second);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Parts drawn once per marker. Render draws the exhaust model at every exhaust marker and the plate model at
// every plate marker with M = RY(90) * Marker * Body. Each draw is told apart by its order within the car's
// Render and placed by M * Body^-1, with Body taken from the draw of the car's base. Its clone is swapped onto
// the model only around that draw: the draw queue keeps the solid, so every tip and plate shows its own damage.

void* g_drawCar = nullptr;   // the CarRenderInfo being rendered
float g_body[16];
bool g_bodyKnown = false;
struct DrawCount {
    void* model;
    int count;
};
DrawCount g_drawCounts[8];
int g_drawCountUsed = 0;
uint8_t* g_swapModel = nullptr;   // a marker draw's model with its clone in, until the draw returns
uint8_t* g_swapOriginal = nullptr;

int NextDrawIndex(void* model) {
    for (int i = 0; i < g_drawCountUsed; i++)
        if (g_drawCounts[i].model == model) return g_drawCounts[i].count++;
    if (g_drawCountUsed == 8) return -1;
    g_drawCounts[g_drawCountUsed++] = {model, 1};
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// From runtime/damage.c

extern "C" int dmg_init(void) {
    const char* e = getenv("NFSU2_DAMAGE_LOG");
    g_settings.logHits = e && *e && *e != '0';
    g_out = dmg_alloc(16);
    Log("crash damage on (Crashing Foreign Cars by FRS Mods, the visual damage)%s",
        g_settings.logHits ? ", every impact logged" : "");
    return g_out != 0;
}

// CarRenderInfo::Render, before the game's
extern "C" void dmg_render_entry(uint32_t cri) {
    void* renderInfo = G(cri);
    if (!dmg_committed(cri, kCriModels + 63 * 8 * 4)) return;
    OnFrame();
    // A racer (it owns a CarDamage) gets its parts refined ahead of its first impact.
    if (uint8_t* car = At<uint8_t*>(renderInfo, kCriCar))
        if (CarHas(car, renderInfo) && At<void*>(car, kCarDamage)) Prewarm(renderInfo);
    g_drawCar = renderInfo;
    g_bodyKnown = false;
    g_drawCountUsed = 0;
    g_markerDraws[renderInfo].clear();   // learnt again by this render's draws
}

// CarCollisionBody::ApplyImpulse, after the game's
extern "C" void dmg_impulse(uint32_t bodyAddr, uint32_t impulseAddr, uint32_t pointAddr) {
    uint8_t* body = G(bodyAddr);
    if (!dmg_committed(bodyAddr, kBodyCar + 4) || !dmg_committed(impulseAddr, 12) || !dmg_committed(pointAddr, 12))
        return;
    const float* impulse = (const float*)G(impulseAddr);
    const float* point = (const float*)G(pointAddr);
    uint8_t* car = At<uint8_t*>(body, kBodyCar);
    if (!car || !dmg_committed(A(car), kCarDamage + 4) || !At<void*>(car, kCarDamage)) return;   // racers only
    // The game's part damage (it hides aftermarket mirrors) has just been fed: our loose parts replace it.
    memset(At<uint8_t*>(car, kCarDamage) + kCarPartDamageValues, 0, kCarPartDamageCount * sizeof(float));
    void* renderInfo = At<void*>(car, kCarRenderInfo);
    uint8_t* rigid = At<uint8_t*>(body, kBodyRigidBody);
    if (!renderInfo || !rigid) return;

    // The same mass scale ApplyImpulse applies before CarCollisionBody::AddDamage.
    const float limit = 1.0f / At<float>(G(kImpulseMassScale), 0);
    const float massTerm = At<float>(rigid, kRigidMassTerm);
    const float scale = massTerm > limit ? limit / massTerm : 1.0f;
    const Vec3 j{impulse[0], impulse[1], impulse[2]};
    const float magnitude = Length(j) * scale;
    const WearSettings& ws = g_settings.wear;
    const bool dents = magnitude > g_settings.impulseThreshold;
    if (!dents && magnitude <= ws.scratchThreshold) return;

    // World to car space: the point is relative to the body, the orientation rows are the car's axes.
    const float* m = &At<float>(rigid, kRigidOrientation);
    const Vec3 ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]}, az{m[8], m[9], m[10]};
    const Vec3 p{point[0], point[1], point[2]};
    const Vec3 cg = At<Vec3>(rigid, kRigidCgOffset);
    Dent dent;
    dent.point = Vec3{Dot(p, ax), Dot(p, ay), Dot(p, az)} + cg;
    dent.direction = Normalize({Dot(j, ax), Dot(j, ay), Dot(j, az)});
    dent.depth = dents ? (magnitude - g_settings.impulseThreshold) * g_settings.depthPerImpulse : 0.0f;
    if (dent.depth > g_settings.maxDepthPerHit) dent.depth = g_settings.maxDepthPerHit;
    const Vec3 contact = dent.point;
    const bool snapped = SnapToSurface(renderInfo, dent, 0.15f) || SnapToSurface(renderInfo, dent, 0.4f);

    CarState* state = StateFor(renderInfo, car, true);
    // The game's own window cracks and lamp debris, which free roam (and the crash camera's exit) turn off.
    uint8_t* carDamage = At<uint8_t*>(car, kCarDamage);
    if (ws.nativeGlass && !At<uint8_t>(carDamage, kCarDamageVisible)) {
        const uint32_t one = 1;
        dmg_thiscall(kSetVisibleDamageEnabled, A(carDamage), 1, &one);
    }
    SetCar(*state, car);
    state->rigid = rigid;

    const double t0 = NowMs();
    const size_t clonesBefore = g_clones.size();
    g_refinedAhead = g_refinedNow = 0;
    EnsureClones(renderInfo);
    if (g_clones.size() != clonesBefore)   // the car's first impact (or its first since it was rebuilt)
        Log("car %08X cloned: %d parts in %.1f ms (%d refined ahead, %d at the impact)", A(car),
            (int)(g_clones.size() - clonesBefore), NowMs() - t0, g_refinedAhead, g_refinedNow);
    int moved = 0;
    if (dents) {
        moved = DentCar(renderInfo, *state, dent);
        if (g_settings.hang.enabled) DamageParts(renderInfo, *state, dent);
        BreakLights(renderInfo, *state, dent);
    }
    if (magnitude > ws.scratchThreshold) {
        float amount = (magnitude - ws.scratchThreshold) * ws.scratchPerImpulse;
        amount = amount > 1.0f ? 1.0f : amount;
        if (dents)   // an impact also scratches around the dent
            ScratchCar(renderInfo, dent.point, amount, ws.scratchRadius + 0.015f * magnitude);
        // Every contact scrapes along whatever the car leans on. The impulse carries the friction against
        // the slide, which tilts it off the surface's normal: the part along the car's motion is dropped.
        const Vec3 vw = At<Vec3>(rigid, kRigidVelocity);
        const Vec3 slide = Normalize({Dot(vw, ax), Dot(vw, ay), Dot(vw, az)});
        Vec3 normal = Normalize(dent.direction - slide * Dot(dent.direction, slide));
        if (Length(normal) < 0.5f) normal = dent.direction;   // pushed straight along the motion
        ScrapeSurface(renderInfo, *state, normal, amount, magnitude);
    }

    if (!g_settings.logHits || !dents) return;
    Log("hit car %08X impulse %.2f contact (%.2f %.2f %.2f) %s (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) "
        "depth %.3f: %d verts, %d new clones, %.1f ms",
        A(car), magnitude, contact.x, contact.y, contact.z, snapped ? "surface" : "no surface", dent.point.x,
        dent.point.y, dent.point.z, dent.direction.x, dent.direction.y, dent.direction.z, dent.depth, moved,
        (int)(g_clones.size() - clonesBefore), NowMs() - t0);
    for (int i = 0; i < kHangCount; i++) {
        const HangPart& h = state->parts[i];
        if (h.loose || h.damage[0] + h.damage[1] > 0)
            Log("  %-12s damage %.2f/%.2f%s%s", kHangSpecs[i].name, h.damage[0], h.damage[1], h.loose ? " loose" : "",
                h.fallen ? " fallen" : "");
    }
}

// CarDamage::SetDamageToZero, before the game's: the game's own repair at the end of a race intro also
// removes the dents; the other caller, the return from the crash camera, is ignored.
extern "C" void dmg_zero(uint32_t carDamage, uint32_t caller) {
    if (caller == kClearZoneReturn || !dmg_committed(carDamage, 4)) return;
    uint8_t* car = At<uint8_t*>(G(carDamage), 0);
    if (!car || !dmg_committed(A(car), kCarRenderInfo + 4)) return;
    void* renderInfo = At<void*>(car, kCarRenderInfo);
    if (renderInfo && g_cars.count(renderInfo)) {
        RepairRenderInfo(renderInfo);
        if (g_settings.logHits) Log("car %08X repaired by the game", A(car));
    }
}

// Render, after its four open flags: the loose lids and doors of this car are marked open for the rest of
// Render, so the engine, the undersides and the door panels are drawn (a fallen hood leaves the engine in sight).
extern "C" void dmg_open_flags(uint32_t cri, uint32_t frame) {
    auto it = g_cars.find(G(cri));
    if (it == g_cars.end()) return;
    static const struct {
        const char* name;
        size_t flag, amount;   // amount: 0 for none
    } kOpen[] = {{"hood", kOpenHood, kOpenHoodAmount}, {"trunk", kOpenTrunk, kOpenTrunkAmount},
                 {"left door", kOpenLeftDoor, 0}, {"right door", kOpenRightDoor, 0}};
    for (int i = 0; i < kHangCount; i++) {
        const HangPart& h = it->second->parts[i];
        if (!h.loose && !h.fallen) continue;
        for (const auto& o : kOpen)
            if (strcmp(o.name, kHangSpecs[i].name) == 0) {
                if (!At<int>(G(frame), o.flag) && o.amount) At<float>(G(frame), o.amount) = 1.0f;   // fully open
                At<int>(G(frame), o.flag) = 1;
            }
    }
}

// Render's draw of a part: Body's matrix, from the draw of the car's base
extern "C" void dmg_body_draw(uint32_t modelAddr, uint32_t l2w) {
    if (!g_drawCar || !modelAddr || !l2w) return;
    uint8_t* model = G(modelAddr);
    uint8_t** table = (uint8_t**)((uint8_t*)g_drawCar + kCriModels);
    for (int lod = 0; lod < 4 && !g_bodyKnown; lod++)
        if (model == table[0 * 8 + lod] || model == table[5 * 8 + lod]) {   // BASE, BODY: drawn with Body
            memcpy(g_body, G(l2w), sizeof(g_body));
            g_bodyKnown = true;
        }
}

// An exhaust tip's or a plate's draw: its placement learnt, its clone swapped in until the draw returns.
extern "C" void dmg_marker_pre(int slot, uint32_t modelAddr, uint32_t l2wAddr) {
    g_swapModel = nullptr;
    uint8_t* model = G(modelAddr);
    const float* l2w = (const float*)G(l2wAddr);
    uint8_t** table = g_drawCar ? (uint8_t**)((uint8_t*)g_drawCar + kCriModels) : nullptr;
    const int row = !table ? -1 : model == table[slot * 8] ? 0 : model == table[slot * 8 + 4] ? 1 : -1;
    if (!g_bodyKnown || !modelAddr || !l2wAddr || row < 0) return;
    const int index = NextDrawIndex(model);
    if (index < 0) return;

    // Placement = M * Body^-1 (row vectors).
    Placement body;
    if (!body.Set(g_body)) return;
    float m16[16] = {};
    for (int r = 0; r < 3; r++) {
        const Vec3 v = Placement::Mul(body.inv, {l2w[r * 4], l2w[r * 4 + 1], l2w[r * 4 + 2]});
        memcpy(&m16[r * 4], &v, 12);
    }
    const Vec3 t = Placement::Mul(body.inv, Vec3{l2w[12], l2w[13], l2w[14]} - body.t);
    memcpy(&m16[12], &t, 12);
    m16[15] = 1.0f;
    std::vector<MarkerDrawn>& drawn = g_markerDraws[g_drawCar];
    MarkerDrawn* known = nullptr;
    for (auto& d : drawn)
        if (d.model == model && d.index == index) known = &d;
    if (!known) {
        drawn.push_back({model, index, slot, row, Placement()});
        known = &drawn.back();
    }
    known->place.Set(m16);

    auto it = g_clones.find(CloneKey(model, index));
    Clone* c = it == g_clones.end() ? nullptr : it->second;
    if (!c || !Live(c) || c->renderInfo != g_drawCar) return;
    // The game has just set this draw's light materials (the exhaust tip colour) on the original's table.
    memcpy(c->lmTable, At<uint8_t*>(c->original, kSolidLmTable), At<int8_t>(c->original, kSolidLmCount) * 8);
    At<uint8_t*>(model, kModelSolid) = c->solid;
    g_swapModel = model;
    g_swapOriginal = c->original;
}

extern "C" void dmg_marker_post(void) {
    if (!g_swapModel) return;
    At<uint8_t*>(g_swapModel, kModelSolid) = g_swapOriginal;
    g_swapModel = nullptr;
}

// The car loader (0x63AED0), right before it asks the car's TEXTURES.BIN for the textures its parts use: the
// engine's own, <CAR>_ENGINE, joins them. The game never shows the engine (its part is off under the hood), so it
// never loads that texture, and the engine a lost hood leaves in sight was drawn with the default texture: black.
constexpr size_t kUsedTexList = 0x08, kUsedTexCount = 0x230;   // u32 hashes, and how many
constexpr int kUsedTexMax = (0x11C - 0x08) / 4;                // 69, the list's room
extern "C" void dmg_car_textures(uint32_t used, uint32_t nameAddr) {
    if (!dmg_committed(used, kUsedTexCount + 4) || !dmg_committed(nameAddr, 32)) return;
    char name[64];
    snprintf(name, sizeof name, "%.40s_ENGINE", (const char*)G(nameAddr));
    const uint32_t hash = HashName(name);
    uint32_t* list = &At<uint32_t>(G(used), kUsedTexList);
    int& count = At<int>(G(used), kUsedTexCount);
    if (count < 0 || count >= kUsedTexMax) return;
    for (int i = 0; i < count; i++)
        if (list[i] == hash) return;
    list[count++] = hash;
    if (g_settings.logHits) Log("car textures: %s added (%d in all)", name, count);
}

// The main loop, once a frame: the texture pack (scratches, cracked lenses), given once to the game's
// LoadChunks; it is permanent (the game keeps pointers into it).
extern "C" void dmg_main_loop(void) {
    static bool tried = false;
    if (tried) return;
    tried = true;
    const char* win = g_settings.wear.pack;
    char path[1024];
    FILE* fp = fileio_resolve_game(win, path, sizeof path) ? fopen(path, "rb") : nullptr;
    if (!fp) {
        Log("pack %s missing: no scratches or cracked lenses", win);
        return;
    }
    long size = 0;
    uint8_t* data = nullptr;
    if (!fseek(fp, 0, SEEK_END) && (size = ftell(fp)) > 0 && !fseek(fp, 0, SEEK_SET))
        data = (uint8_t*)GAlloc((uint32_t)size);
    const bool ok = data && fread(data, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) {
        Log("pack %s unreadable", win);
        return;
    }
    const uint32_t name = dmg_alloc((uint32_t)strlen(win) + 1);
    memcpy(G(name), win, strlen(win) + 1);
    const uint32_t args[3] = {A(data), (uint32_t)size, name};
    dmg_call(kLoadChunks, 3, args);
    Log("pack %s loaded (%ld bytes)", win, size);
}
