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

#include <algorithm>
#include <cmath>
#include <deque>
#include <initializer_list>
#include <memory>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "damage/Dent.h"
#include "damage/Tessellate.h"

extern "C" {
#include "roam_core.h"         // RC_DMG_*: a dent on the wire (free roam)
#include "splitscreen_mem.h"   // SPLIT_PLAYERS_INDEX: where the players' table is (moved for split screen on wasm)
}

extern "C" {
uint32_t dmg_call(uint32_t fn, int n, const uint32_t* a);
uint32_t dmg_thiscall(uint32_t fn, uint32_t self, int n, const uint32_t* a);
float dmg_thiscall_f(uint32_t fn, uint32_t self, int n, const uint32_t* a);   // its return in st0
uint32_t dmg_alloc(uint32_t n);
void dmg_free(uint32_t p);
int dmg_committed(uint32_t a, uint32_t n);
uint32_t dmg_va_free(void);                    // the guest address space still free, bytes
int dmg_big_memory(void);                      // the guest heap has its window above the runtime (a computer's browser)
uint32_t dmg_scratches_read(uint32_t* data, uint32_t* size);   // damage.c: MODPACKS/DAMAGE/SCRATCHES.TPK, 0 without it
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
    bool doorCollide = true;      // an open door hits walls and cars: pushed shut from the front, torn off from behind
    float doorTearSpeed = 3.0f;   // m/s an obstacle must push an open door open at to tear it off
    bool debris = true;           // a part that comes off drops to the road and lies there (else it is just gone)
    float debrisSeconds = 120.0f, debrisReach = 80.0f;   // how long it lies there; gone once the car is this far
    float mirrorDropDeg = 50.0f;  // how far a dangling mirror turns down before it rests on the body
    float mirrorSwing = 1.0f;     // a dangling mirror swings out and back against the body (turns, wind); 0 = it hangs
};
struct WearSettings {
    float scratchThreshold = 0.5f, scratchPerImpulse = 0.4f, scratchRadius = 0.45f, scrapeBand = 0.08f;
    const char* scratchMaterial = "MAGGUNMETAL";
    float scratchScale = 2.0f, lightBreakDepth = 0.03f, lightBreakRadius = 0.45f;
    bool nativeGlass = true;
};
// A bent wheel (drawn only: the handling is not touched), at full damage: its top leans in, its front turns out,
// and a bent rim wobbles as it turns.
struct WheelSettings {
    bool enabled = true;
    float wheelReach = 1.1f;      // metres (flat) from a wheel within which a dent hurts it
    float wheelPerDepth = 6.0f;   // wheel damage per metre of dent depth
    float bendCamberDeg = 15.0f, bendToeDeg = 8.0f, bendWobbleDeg = 4.0f;
};
struct Settings {
    DentSettings dent;
    HangSettings hang;
    WearSettings wear;
    WheelSettings wheels;
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

// The car draws through this render info, as its memory says: only a hint, as a car of a world that is gone (a race's
// end, the front end) says so too, from freed memory. CarLive (below) is what lets the car's memory be touched.
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
// The world around a car, for the doors that hit things and the fallen parts (the .asi's WorldU2.h): the ground
// and the walls of the track's topology, and the other cars.
//  - a TopologyCoordinate (0x400 bytes, 16-aligned): ctor 0x5E0B40, SetSectionPosition 0x5E68D0 (connects +8 to
//    the topology tree), dtor 0x5E0BF0;
//  - GetElevation 0x5E7E70 thiscall(tc, const bVector3*, TerrainType*, bVector3* normal, bool* found) -> st0;
//  - SegmentIntersectsPossiblyLong 0x5E7ED0 thiscall(tc, from, to, bVector3* normal, bVector3* hit, bool twoSided):
//    the first polygon (floor, wall or ceiling) the segment crosses, with its normal and the point;
//  - every car: a ring whose head [0x890080] is a sentinel, a car's +0 the next; car +0x3C its collision body
//    (+0xE0/+0xF0 its box, relative to the rigid body), the body's +0x68 its RigidBody (+0x20 position, rows +0x30
//    forward, +0x40 left, +0x50 up, +0xA0 velocity, world).
// The coordinate and the vectors the game is handed live in the guest heap.
namespace world {

constexpr uint32_t kTopoCtor = 0x5E0B40, kTopoDtor = 0x5E0BF0, kTopoSetSection = 0x5E68D0;
constexpr uint32_t kGetElevation = 0x5E7E70, kSegmentIntersects = 0x5E7ED0;
constexpr uint32_t kWorldCarList = 0x890080;
constexpr size_t kCarBody = 0x3C, kBodyRigid = 0x68, kBodyBoxMin = 0xE0, kBodyBoxMax = 0xF0;
constexpr size_t kRigidPosition = 0x20, kRigidRows = 0x30, kRigidVelocity = 0xA0, kRigidCg = 0x1F0;

// A rigid frame: the car's axes (world) and where its origin is.
struct Frame {
    Vec3 o{0, 0, 0}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    Vec3 ToWorld(Vec3 p) const { return o + x * p.x + y * p.y + z * p.z; }
    Vec3 DirToWorld(Vec3 d) const { return x * d.x + y * d.y + z * d.z; }
    Vec3 ToLocal(Vec3 w) const {
        const Vec3 d = w - o;
        return {Dot(d, x), Dot(d, y), Dot(d, z)};
    }
    Vec3 DirToLocal(Vec3 d) const { return {Dot(d, x), Dot(d, y), Dot(d, z)}; }
};

uint8_t* RigidOf(uint8_t* car) {
    if (!car || !dmg_committed(A(car) + kCarBody, 4)) return nullptr;
    uint8_t* body = At<uint8_t*>(car, kCarBody);
    return body && dmg_committed(A(body) + kBodyRigid, 4) ? At<uint8_t*>(body, kBodyRigid) : nullptr;
}

// The car space the damage works in (the impacts): the rigid body's axes, the origin the rigid body's position
// less its centre-of-gravity offset.
bool CarFrame(const uint8_t* rigid, Frame& f) {
    if (!rigid || !dmg_committed(A(rigid), kRigidCg + 12)) return false;
    const float* m = (const float*)(rigid + kRigidRows);
    f.x = {m[0], m[1], m[2]};
    f.y = {m[4], m[5], m[6]};
    f.z = {m[8], m[9], m[10]};
    if (Length(f.x) < 0.5f || Length(f.z) < 0.5f) return false;
    const Vec3 p = At<Vec3>(rigid, kRigidPosition), cg = At<Vec3>(rigid, kRigidCg);
    f.o = p - f.DirToWorld(cg);
    return true;
}

Vec3 Velocity(const uint8_t* rigid) { return rigid ? At<Vec3>(rigid, kRigidVelocity) : Vec3{0, 0, 0}; }

uint32_t g_topo = 0, g_vec = 0;   // guest: the coordinate (0x400), and 4 vectors + a flag (0x70)
bool Scratch() {
    if (!g_topo) g_topo = dmg_alloc(0x400);
    if (!g_vec) g_vec = dmg_alloc(0x70);
    return g_topo && g_vec;
}
bool Open(const float* at) {   // the coordinate at the section of `at`; false where there is no topology
    memset(G(g_topo), 0, 0x400);
    dmg_thiscall(kTopoCtor, g_topo, 0, nullptr);
    const uint32_t args[2] = {A(at), 0};   // (const bVector3*, float 0)
    dmg_thiscall(kTopoSetSection, g_topo, 2, args);
    if (R32(g_topo + 8)) return true;
    dmg_thiscall(kTopoDtor, g_topo, 0, nullptr);
    return false;
}

// The ground under p (looked for from a little above it); false where there is no topology.
bool Ground(Vec3 p, float& height, Vec3& normal) {
    if (!Scratch()) return false;
    float* at = (float*)G(g_vec);
    float* n = at + 4;
    uint8_t* found = (uint8_t*)(at + 8);
    at[0] = p.x, at[1] = p.y, at[2] = p.z + 1.0f, at[3] = 0;
    n[0] = 0, n[1] = 0, n[2] = 1, n[3] = 0;
    *found = 0;
    if (!Open(at)) return false;
    const uint32_t args[4] = {A(at), 0, A(n), A(found)};
    const float e = dmg_thiscall_f(kGetElevation, g_topo, 4, args);
    dmg_thiscall(kTopoDtor, g_topo, 0, nullptr);
    if (!*found || !(e > -10000.0f && e < 10000.0f)) return false;
    height = e;
    const Vec3 nn{n[0], n[1], n[2]};
    normal = Length(nn) > 0.5f ? Normalize(nn) : Vec3{0, 0, 1};
    return true;
}

// The first wall (a polygon of the topology steeper than 60 degrees) the segment a -> b crosses.
bool Wall(Vec3 a, Vec3 b, Vec3& hit, Vec3& normal) {
    if (!Scratch()) return false;
    float* pa = (float*)G(g_vec);
    float *pb = pa + 4, *n = pa + 8, *h = pa + 12;
    pa[0] = a.x, pa[1] = a.y, pa[2] = a.z, pa[3] = 0;
    pb[0] = b.x, pb[1] = b.y, pb[2] = b.z, pb[3] = 0;
    for (int k = 0; k < 4; k++) n[k] = h[k] = 0;
    if (!Open(pa)) return false;
    const uint32_t args[5] = {A(pa), A(pb), A(n), A(h), 1};
    const bool crossed = (dmg_thiscall(kSegmentIntersects, g_topo, 5, args) & 0xFF) != 0;
    dmg_thiscall(kTopoDtor, g_topo, 0, nullptr);
    if (!crossed) return false;
    const Vec3 nn{n[0], n[1], n[2]};
    if (Length(nn) < 0.5f || fabsf(Normalize(nn).z) > 0.5f) return false;   // a floor or a ceiling
    normal = Normalize(nn);
    if (Dot(normal, b - a) > 0) normal = normal * -1.0f;   // facing the side the segment comes from
    hit = {h[0], h[1], h[2]};
    return true;
}

// Calls f(car, rigid, body box min, max) for every car of the world.
template <typename F>
void ForEachCar(F f) {
    uint8_t* head = G(R32(kWorldCarList));
    if (!head || !dmg_committed(A(head), 4)) return;
    int guard = 0;
    for (uint8_t* car = At<uint8_t*>(head, 0); car && car != head && guard < 256; car = At<uint8_t*>(car, 0), ++guard) {
        if (!dmg_committed(A(car), kCarBody + 4)) break;
        uint8_t* body = At<uint8_t*>(car, kCarBody);
        if (!body || !dmg_committed(A(body), kBodyBoxMax + 12)) continue;
        uint8_t* rigid = At<uint8_t*>(body, kBodyRigid);
        if (!rigid || !dmg_committed(A(rigid), kRigidVelocity + 12)) continue;
        const Vec3 lo = At<Vec3>(body, kBodyBoxMin), hi = At<Vec3>(body, kBodyBoxMax);
        if (!(hi.x - lo.x > 1.0f && hi.x - lo.x < 15.0f && hi.y - lo.y > 0.8f && hi.y - lo.y < 5.0f)) continue;
        f(car, rigid, lo, hi);
    }
}

// Where a point (world) is in a car's box, relative to its rigid body: true when inside, grown by `margin`.
bool InCarBox(const uint8_t* rigid, Vec3 lo, Vec3 hi, Vec3 w, float margin, Vec3& local) {
    const float* m = (const float*)(rigid + kRigidRows);
    const Vec3 d = w - At<Vec3>(rigid, kRigidPosition);
    local = {d.x * m[0] + d.y * m[1] + d.z * m[2], d.x * m[4] + d.y * m[5] + d.z * m[6],
             d.x * m[8] + d.y * m[9] + d.z * m[10]};
    return local.x > lo.x - margin && local.x < hi.x + margin && local.y > lo.y - margin &&
           local.y < hi.y + margin && local.z > lo.z - margin && local.z < hi.z + margin;
}

}  // namespace world

// The car is one of the world's now (its car list has it: World::~World, 0x5FF6F0, takes every car off the list and
// clears [0x890080]) and draws through this render info: only then are its memory and its render info's (the flares)
// the car's. A car of a world that was unloaded still reads +0x554 == its render info from freed memory.
bool CarLive(uint8_t* car, void* renderInfo) {
    if (!car) return false;
    uint8_t* head = G(R32(world::kWorldCarList));
    if (!head || !dmg_committed(A(head), 4)) return false;
    int guard = 0;
    for (uint8_t* c = At<uint8_t*>(head, 0); c && c != head && guard < 256; c = At<uint8_t*>(c, 0), ++guard) {
        if (c == car) return CarHas(car, renderInfo);
        if (!dmg_committed(A(c), 4)) break;
    }
    return false;
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

constexpr int kSpecHood = 4, kSpecTrunk = 5, kSpecLeftDoor = 6, kSpecRightDoor = 7;
// CarState::gameOpen: the hood, the trunk lid, the left and the right door (Render's open flags, dmg_open_flags)
uint8_t OpenBit(int spec) {
    return spec == kSpecHood ? 1 : spec == kSpecTrunk ? 2 : spec == kSpecLeftDoor ? 4 : spec == kSpecRightDoor ? 8 : 0;
}
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

// A part that came off, lying in the world.
struct Debris {
    bool active = false, asleep = false;
    bool leftCar = false;   // it has been clear of its own car once (it starts against it)
    float age = 0, still = 0;
    float rot0[9] = {};     // the part's car-space transform when it came off: y = rot0 * x + t0
    Vec3 t0{0, 0, 0};
    Vec3 c0{0, 0, 0};       // its centre then (car space), and its box's half size (car axes then)
    Vec3 half{0, 0, 0};
    Vec3 p{0, 0, 0}, v{0, 0, 0}, w{0, 0, 0};   // world: centre, velocity, angular velocity
    float r[9] = {};        // world rotation, columns the body's axes (the car's when it came off)
    float drag = 0;
    float owed = 0;         // a friend's car's part: the time its steps waited for the frame's world queries (s)
};

struct CarState {
    uint8_t* car = nullptr;
    bool live = false;          // the car is the world's (CarLive), as of this frame: its memory may be touched
    bool remote = false;        // a friend's car (free roam): its upkeep within the frame's limits
    uint8_t gameOpen = 0;       // the parts the game opens itself (the show-off, a cover shoot): kOpenBits
    uint8_t* rigid = nullptr;   // the car's RigidBody, from the impulse that created this state
    CrushLimits crush;          // from the car type's wheels
    struct BrokenLight {
        uint8_t* flare;
        float z;                // the flare's height before it was sunk out of sight
    };
    std::vector<BrokenLight> brokenLights;
    Vec3 lastVelocity{0, 0, 0};
    bool hasVelocity = false;
    Vec3 lastWorldVelocity{0, 0, 0};   // for the true acceleration (the car-space one misses the turns)
    float flutterPhase = 0;
    HangPart parts[kHangCount];
    // A dangling mirror may also swing fore and aft about the car's Y axis through its hinge, after its droop.
    float sway[kHangCount] = {}, swayRate[kHangCount] = {}, shownSway[kHangCount] = {};
    Debris debris[kHangCount];
    bool shownDebris[kHangCount] = {};
    float doorCooldown[kHangCount] = {};
    world::Frame lastFrame{};   // the car's pose at the last SwingParts, for the doors' speed
    bool hasFrame = false;
    float wheel[4] = {0, 0, 0, 0};   // per physics wheel (front ones 0 and 1), 0..1: how bent it is drawn
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
    uint32_t drawnAt = 0;           // the frame Render last drew it (0: never seen drawn: uploaded whenever it changes)

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
std::unordered_map<void*, int> g_cloneCount;       // g_clones' clones by CarRenderInfo
std::unordered_map<void*, CarState*> g_cars;       // by CarRenderInfo

bool HasClones(void* renderInfo) { return g_cloneCount.count(renderInfo) != 0; }

// Where the game draws each exhaust tip and plate (RY(90) * marker, car space), learnt from every draw of
// every car (before any damage), by CarRenderInfo.
struct MarkerDrawn {
    uint8_t* model;
    int index, slot, row;
    Placement place;
};
struct MarkerDraws {
    uint32_t frame = 0;   // the car's last render
    std::vector<MarkerDrawn> draws;
};
std::unordered_map<void*, MarkerDraws> g_markerDraws;   // (a car not drawn for kMarkerKeepFrames goes)
constexpr uint32_t kMarkerKeepFrames = 120;
std::vector<Clone*> g_retired;
int g_cloneFails = 0;   // clones that found no memory or buffers (a friend's car's damage is given up at one)
uint32_t g_lastFrame = 0xFFFFFFFF;
uint32_t g_lastWorldTime = 0;

// The plates are drawn on their markers, but mounted on a part: the rear one on the trunk lid where the car type has
// it there, else on the rear bumper; the front one on the front bumper. They swing, drop and come off with it. Which
// car types carry the rear plate on the lid is the game's own table: Render (0x6253C0) draws the plate with the trunk
// lid's matrix (its opening animation: the cover shoot, the show-off) where table[type] is 0, for types 0..25
// (0x630DBC, a byte each; CarRenderInfo +4 the RideInfo, +0 its car type).
constexpr uint32_t kPlateOnTrunk = 0x630DBC;
constexpr uint32_t kPlateOnTrunkTypes = 26;
constexpr int kSpecFrontBumper = 0, kSpecRearBumper = 1;
int PlateSpec(const Clone* c) {
    if (c->place.t.x > 0) return kSpecFrontBumper;
    const uint32_t ride = dmg_committed(A(c->renderInfo) + 4, 4) ? At<uint32_t>(c->renderInfo, 4) : 0;
    const uint32_t type = ride && dmg_committed(ride, 4) ? R32(ride) : 0xFFFFFFFFu;
    return type < kPlateOnTrunkTypes && G(kPlateOnTrunk)[type] == 0 ? kSpecTrunk : kSpecRearBumper;
}

int SpecOf(const Clone* c) {
    if (c->slot == kSlotPlate && c->place.on) return PlateSpec(c);
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

// Parts that come off (defined with SwingParts).
Vec3 MatVec(const float m[9], Vec3 v);
uint8_t* RigidFor(const CarState& car, void* renderInfo);
bool StartDebris(CarState& car, void* renderInfo, int i, Vec3 kick, Vec3 spin);

// Writes the clone's vertices to its buffer, swinging loose parts about their hinges (a mirror with its
// door), followed by the overlay copy of the scratched and cracked ones. A part lying in the world (or one
// mounted on it) is drawn through that body's pose, from the car's pose now.
bool Upload(Clone* c) {
    const HangPart* chain[2];
    const int links = HangChain(c, chain);
    float sway[2] = {0, 0};   // a dangling mirror's fore-and-aft swing, per link
    const CarState* st = links ? g_cars.find(c->renderInfo)->second : nullptr;
    const Debris* debris = nullptr;
    int debrisLink = links;
    world::Frame now{};
    bool fallen = false;
    for (int k = 0; k < links; k++) {
        const int spec = (int)(chain[k] - st->parts);
        sway[k] = st->sway[spec];
        if (st->debris[spec].active && chain[k]->fallen) {
            if (world::CarFrame(RigidFor(*st, c->renderInfo), now)) {
                debris = &st->debris[spec];
                debrisLink = k;
            } else {
                fallen = true;
            }
            break;
        }
        if (chain[k]->fallen) {
            fallen = true;
            break;
        }
    }
    At<int>(c->mesh, kMeshStripCount) = fallen ? 0 : c->drawStrips;
    c->dirty = false;
    if (fallen) return true;

    // A part in flight or lying on the road leaves its boxes far behind: open them up so it is not culled.
    for (int k = 0; k < links; k++)
        if (chain[k]->flying || debris) {
            const float reach = debris ? g_settings.hang.debrisReach + 20.0f : 60.0f;
            for (int s = 0; s < c->drawStrips; s++)
                for (int a = 0; a < 3; a++) {
                    At<float>(c->strips + s * kStripSize, kStripBoxMin + a * 4) = -reach;
                    At<float>(c->strips + s * kStripSize, kStripBoxMax + a * 4) = reach;
                }
        }

    if (!c->vb || Com(c->vb, kLock, {0, 0, g_out, 0}) != 0) return false;
    Vertex36* p = (Vertex36*)G(R32(g_out));
    if (!p) return false;
    for (int i = 0; i < c->count; i++) {
        Vertex36 v = c->verts[i];
        Vec3 pos = *(Vec3*)v.pos, nrm = *(Vec3*)v.nrm;
        for (int k = 0; k < links; k++) {
            if (k == debrisLink) {   // where the part came off from, then the body's pose, then back into the car
                const Vec3 y = MatVec(debris->rot0, pos) + debris->t0;
                pos = now.ToLocal(debris->p + MatVec(debris->r, y - debris->c0));
                nrm = now.DirToLocal(MatVec(debris->r, MatVec(debris->rot0, nrm)));
                break;
            }
            const HangPart* h = chain[k];
            const bool moved = h->offset.x != 0.0f || h->offset.y != 0.0f || h->offset.z != 0.0f;
            if (!h->loose || (h->angle == 0.0f && sway[k] == 0.0f && !h->flying && !moved)) continue;
            // the game opens it (the show-off, a cover shoot): its own animation turns the part, ours would add to it
            if (st->gameOpen & OpenBit((int)(chain[k] - st->parts))) continue;
            Vec3 rel = Rotate(pos - h->hinge, h->axis, h->angle);
            nrm = Rotate(nrm, h->axis, h->angle);
            if (sway[k] != 0.0f) {
                rel = Rotate(rel, Vec3{0, 1, 0}, sway[k]);
                nrm = Rotate(nrm, Vec3{0, 1, 0}, sway[k]);
            }
            pos = h->hinge + rel + h->offset;
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
    auto it = g_clones.find(c->key);
    if (it != g_clones.end() && it->second == c) {
        g_clones.erase(it);
        auto n = g_cloneCount.find(c->renderInfo);
        if (n != g_cloneCount.end() && --n->second <= 0) g_cloneCount.erase(n);
    }
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

size_t g_tessCacheBytes = 64u << 20;           // results kept (least recently used dropped beyond): dmg_init's
constexpr int kPrewarmPerFrame = 6;              // source meshes copied per frame
constexpr double kRefineMsPerFrame = 3.0;        // refinement per frame ahead of the impacts (a step past it at most)
constexpr int kRefineStep = 16;                  // edges split between two looks at the clock (well under 1 ms)
constexpr size_t kMaxRefinedOnSpot = 4000;       // a part not refined ahead, refined at its impact: fewer vertices
                                                 // (32,000: ~30 ms natively; 4000: ~2.5 ms), the whole one queued

struct TessJob {
    bool done = false;
    TessMesh mesh;          // the source until done, then the refined mesh
    size_t bytes = 0;       // of the refined mesh
    uint32_t lastUse = 0;   // frame
    std::unique_ptr<Tessellator> work;   // its refinement under way (RunPrewarm: a few steps a frame)
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

// Replaces the source `t` with its refined shape: from the cache when it is there, otherwise refined here to fewer
// vertices (kMaxRefinedOnSpot, so that an impact's frame stays short), and the whole refinement queued for the next
// clones of it (RunPrewarm).
void Refine(TessMesh& t) {
    const uint64_t key = SourceKey(t);
    auto it = g_tessCache.find(key);
    if (it != g_tessCache.end() && it->second.done) {
        it->second.lastUse = Frame();
        t = it->second.mesh;
        g_refinedAhead++;
        return;
    }
    if (it == g_tessCache.end()) {
        TessJob& job = g_tessCache[key];
        job.mesh = t;
        job.lastUse = Frame();
        g_tessQueue.push_back(key);
    }
    Tessellate(t, g_settings.maxEdge, kMaxRefinedOnSpot);
    g_refinedNow++;
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

// Refines queued sources for a few milliseconds: a few edges at a time, a big one over several frames.
void RunPrewarm() {
    const double t0 = NowMs();
    while (!g_tessQueue.empty() && NowMs() - t0 < kRefineMsPerFrame) {
        auto it = g_tessCache.find(g_tessQueue.front());
        if (it == g_tessCache.end() || it->second.done) {   // dropped (never: only done ones are)
            g_tessQueue.pop_front();
            continue;
        }
        TessJob& job = it->second;
        if (!job.work) {
            job.work.reset(new Tessellator);
            job.work->Begin(job.mesh, g_settings.maxEdge, kMaxRefinedVerts);
        }
        bool done = false;
        while (!(done = job.work->Step(job.mesh, kRefineStep)) && NowMs() - t0 < kRefineMsPerFrame) {}
        if (!done) break;   // the rest next frame
        job.work.reset();
        Finish(job);
        g_tessQueue.pop_front();
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
    while (g_tessBytes > g_tessCacheBytes) {
        auto oldest = g_tessCache.end();
        for (auto it = g_tessCache.begin(); it != g_tessCache.end(); ++it)
            if (it->second.done && (oldest == g_tessCache.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == g_tessCache.end()) break;
        g_tessBytes -= oldest->second.bytes;
        g_tessCache.erase(oldest);
    }
}

// The cache's refinement of `t`, when it is there (a friend's car: its clones never pay for a refinement at the
// impact); false, and `t` as it was, when not
int g_unrefined = 0;   // parts of friends' cars cloned without their refinement (it was not ready)
bool RefineCached(TessMesh& t) {
    auto it = g_tessCache.find(SourceKey(t));
    if (it == g_tessCache.end() || !it->second.done) return false;
    it->second.lastUse = Frame();
    t = it->second.mesh;
    g_refinedAhead++;
    return true;
}

// mayRefine: false for a friend's car, whose parts are refined ahead or not at all (RefineCached)
Clone* MakeClone(uint8_t* model, void* renderInfo, int slot, int row, int lod, int instance = -1,
                 const Placement* instancePlace = nullptr, bool mayRefine = true) {
    uint8_t* original = At<uint8_t*>(model, kModelSolid);
    const uint32_t device = Device();
    TessMesh t;
    if (!device || !ReadSource(model, t)) return nullptr;
    uint8_t* mesh = At<uint8_t*>(original, 0);
    const int stripCount = (int)t.strips.size();
    const uint8_t* srcStrips = At<uint8_t*>(mesh, kMeshStrips);
    if (lod < g_settings.tessellateLods) {
        if (mayRefine) Refine(t);
        else if (!RefineCached(t)) g_unrefined++;
    }

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
        g_cloneFails++;
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
    // (a lamp's lens crack is the .asi's DAMAGE_LENS_CRACK, one of EA's ProStreet textures: not here, so a broken
    // lamp only goes dark, and lamps get no overlay texture)
    static const char* kOverlayNames[kScratchLevels] = {"DAMAGE_SCRATCH1", "DAMAGE_SCRATCH2", "DAMAGE_SCRATCH3",
                                                        "DAMAGE_SCRATCH4"};
    int texTotal = texCount;
    for (int k = 0; k < kScratchLevels; k++) {
        if (c->light) break;   // lamps would crack, the rest scratches
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
    Clone*& slotOf = g_clones[c->key];
    if (slotOf && slotOf != c) Retire(slotOf, false);   // (callers retire it first: never two under one key)
    g_clones[c->key] = c;
    g_cloneCount[renderInfo]++;
    return c;
}

// Clone of this model for this car, made on demand.
Clone* CloneFor(uint8_t* model, void* renderInfo, int slot, int row, int lod, bool mayRefine = true) {
    auto it = g_clones.find(CloneKey(model, -1));
    if (it != g_clones.end()) {
        Clone* c = it->second;
        if (Live(c) && c->renderInfo == renderInfo) return c;
        Retire(c, false);
    }
    return MakeClone(model, renderInfo, slot, row, lod, -1, nullptr, mayRefine);
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
        if (spec.fallScale > 0 && total > hs.fallDamage * spec.fallScale) {
            // Knocked off: away from the hit, a little up, tumbling.
            world::Frame f;
            const bool posed = world::CarFrame(RigidFor(car, renderInfo), f);
            const Vec3 kick = posed ? f.DirToWorld(dent.direction * -1.5f) + Vec3{0, 0, 1.5f} : Vec3{0, 0, 0};
            const Vec3 spin{(float)(rand() % 200 - 100) * 0.04f, (float)(rand() % 200 - 100) * 0.04f, 0};
            StartDebris(car, renderInfo, i, kick, spin);
        } else if (!h.loose && h.damage[side] > threshold)
            Loosen(renderInfo, spec, h, mn, mx, side);
    }
}

// Makes sure every body part of the car has its clone (scratches can come before any dent), and every
// exhaust tip and plate drawn so far its own.
void EnsureClones(void* renderInfo, bool mayRefine = true) {
    ForEachPartModel(renderInfo, [&](uint8_t* model, int slot, int row, int lod) {
        CloneFor(model, renderInfo, slot, row, lod, mayRefine);
    });
    auto drawn = g_markerDraws.find(renderInfo);
    if (drawn == g_markerDraws.end()) return;
    uint8_t** table = (uint8_t**)((uint8_t*)renderInfo + kCriModels);
    for (const MarkerDrawn& d : drawn->second.draws) {
        if (table[d.slot * 8 + d.row * 4] != d.model) continue;   // learnt before the parts were rebuilt
        auto it = g_clones.find(CloneKey(d.model, d.index));
        if (it != g_clones.end()) {
            if (Live(it->second) && it->second->renderInfo == renderInfo) continue;
            Retire(it->second, false);
        }
        if (At<uint8_t*>(d.model, kModelSolid))
            MakeClone(d.model, renderInfo, d.slot, d.row, 0, d.index, &d.place, mayRefine);
    }
}

// EnsureClones a part at a time (a friend's car, within the frame's budget): `more()` says whether another part may be
// cloned now; `at` is where the next call goes on (the parts in ForEachPartModel's order, then the marker draws), so
// a part that cannot be cloned is tried once. False while parts are left.
template <typename More>
bool CloneSome(void* renderInfo, int& at, More more) {
    const auto cloned = [&](uint8_t* model, int instance) {
        auto it = g_clones.find(CloneKey(model, instance));
        return it != g_clones.end() && Live(it->second) && it->second->renderInfo == renderInfo;
    };
    int i = 0;
    bool stop = false;
    ForEachPartModel(renderInfo, [&](uint8_t* model, int slot, int row, int lod) {
        const int me = i++;
        if (stop || me < at) return;
        if (!cloned(model, -1)) {
            if (!more()) {
                stop = true;
                return;
            }
            CloneFor(model, renderInfo, slot, row, lod, false);
        }
        at = me + 1;
    });
    if (stop) return false;
    auto drawn = g_markerDraws.find(renderInfo);
    if (drawn == g_markerDraws.end()) return true;
    uint8_t** table = (uint8_t**)((uint8_t*)renderInfo + kCriModels);
    for (const MarkerDrawn& d : drawn->second.draws) {
        const int me = i++;
        if (me < at) continue;
        if (table[d.slot * 8 + d.row * 4] == d.model && !cloned(d.model, d.index)) {
            if (!more()) return false;
            auto it = g_clones.find(CloneKey(d.model, d.index));
            if (it != g_clones.end()) Retire(it->second, false);
            if (At<uint8_t*>(d.model, kModelSolid))
                MakeClone(d.model, renderInfo, d.slot, d.row, 0, d.index, &d.place, false);
        }
        at = me + 1;
    }
    return true;
}

// The car's parts refined ahead (Prewarm), so that their clones are quick to make.
bool PrewarmDone(void* renderInfo) {
    bool done = true;
    ForEachPartModel(renderInfo, [&](uint8_t* model, int, int, int lod) {
        if (!done || lod >= g_settings.tessellateLods) return;
        auto own = g_clones.find(CloneKey(model, -1));   // cloned already (the model draws our solid: never prewarmed)
        if (own != g_clones.end() && Live(own->second) && own->second->renderInfo == renderInfo) return;
        auto known = g_prewarmed.find(At<uint8_t*>(model, kModelSolid));
        auto job = known == g_prewarmed.end() ? g_tessCache.end() : g_tessCache.find(known->second.key);
        done = job != g_tessCache.end() && job->second.done;
    });
    return done;
}

int DentCar(void* renderInfo, const CarState& car, const Dent& dent) {
    EnsureClones(renderInfo, !car.remote);
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
        if (spec.fallScale > 0 && h.damage[0] > hs.fallDamage * spec.fallScale) {
            const Vec3 spin{(float)(rand() % 200 - 100) * 0.05f, 0, (float)(rand() % 200 - 100) * 0.05f};
            StartDebris(car, renderInfo, i, Vec3{0, 0, 0.8f}, spin);
        } else if (!h.loose && h.damage[0] > hs.mirrorDamage)
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

// The broken lights back on, the car being live (CarLive): each flare still on the render info's list and still
// sunk where BreakLights sank it.
int g_lightsKept = 0;   // flares not put back (not on the list, or moved since): logged with the world's changes
void RestoreLights(void* renderInfo, CarState& car) {
    uint8_t* head = (uint8_t*)renderInfo + kCriFlares;
    for (const auto& b : car.brokenLights) {
        bool listed = false;
        int guard = 0;
        for (uint8_t* f = At<uint8_t*>(head, 0); f && f != head && guard < 256 && !listed; f = At<uint8_t*>(f, 0), ++guard)
            listed = f == b.flare;
        if (listed && At<float>(b.flare, kFlarePos + 8) == -1000.0f)
            At<float>(b.flare, kFlarePos + 8) = b.z;
        else
            g_lightsKept++;
    }
    car.brokenLights.clear();
}

// `alive`: the car is live (CarLive), so its flares can be put back.
void Forget(void* renderInfo, bool alive) {
    auto it = g_cars.find(renderInfo);
    if (it == g_cars.end()) return;
    CarState* car = it->second;
    if (alive) RestoreLights(renderInfo, *car);
    delete car;
    g_cars.erase(it);
}

// The car's damage taken off (the game's repair, or a friend's car's let go): its clones out, the original solids back
// on its models and its lights back on, only while the car is live; otherwise nothing of the car's is touched.  The car:
// its state's, else the caller's (the render info's own memory may be gone with its car: never read here).
void RepairRenderInfo(void* renderInfo, uint8_t* car) {
    auto st = g_cars.find(renderInfo);
    if (st != g_cars.end()) car = st->second->car;
    if (!CarLive(car, renderInfo)) {   // (its clones go once its models no longer draw them: OnFrame)
        Forget(renderInfo, false);
        return;
    }
    std::vector<Clone*> mine;
    for (auto& kv : g_clones)
        if (kv.second->renderInfo == renderInfo) mine.push_back(kv.second);
    for (Clone* c : mine) Retire(c, true);
    Forget(renderInfo, true);
}

// The world changed (a race's end, the front end, a new world): every car of the old one is gone with it, and none of
// its memory is touched again. Its clones go (freed with the next frames), its cars' states, the marker draws learnt,
// the friends' cars' slots and the refinement's notes of its solids.
struct NetSlot;
void NetForgetAll();
uint32_t g_world = 0;
void WorldWatch() {
    const uint32_t w = R32(world::kWorldCarList);
    if (w == g_world) return;
    const int cars = (int)g_cars.size(), clones = (int)g_clones.size();
    g_world = w;
    std::vector<Clone*> all;
    for (auto& kv : g_clones) all.push_back(kv.second);
    for (Clone* c : all) Retire(c, false);
    g_cloneCount.clear();
    for (auto& kv : g_cars) delete kv.second;
    g_cars.clear();
    g_markerDraws.clear();
    g_prewarmed.clear();
    NetForgetAll();
    if (cars || clones)
        Log("the world changed: %d cars' damage let go (%d clones), none of their memory touched; %d broken lights not put "
            "back since the last change (no longer on their car's list, or moved)", cars, clones, g_lightsKept);
    g_lightsKept = 0;
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

// ---------------------------------------------------------------------------------------------------------
// Parts that come off, and doors that hit things (the world queries above).
//
// A part that comes off (torn off by the damage, by the airflow, or a door hit from behind) becomes a free body
// in the world: a box the size of the part, with gravity, the air's drag, bounces and friction on the topology's
// ground, its walls, and the cars, which push it about. It stays drawn by the car's clones: every frame its
// vertices go from where the part was when it came off, through the body's pose, back into the car's space (the
// car moves under it). It is gone after DebrisSeconds, or once the car is DebrisReach away.

constexpr float kDebrisStep = 1.0f / 120.0f;

// Friends' cars' parts that came off, and their open doors, ask the world (the ground, the walls: four guest calls a
// question) at most this often a frame, all cars; past that a part's step waits (its time owed, kDebrisOwedMax at most)
// and a door's test is left out. And at most this many of their parts are awake on the road at once: past that a part
// that comes off is just gone.
constexpr int kNetWorldQueries = 6;
constexpr float kDebrisOwedMax = 0.25f;
constexpr int kNetDebrisAwake = 6;
int g_netQueriesLeft = 0;
int g_netQueriesWaited = 0, g_netDebrisGone = 0;   // (the 30-s line's)
bool MayQuery(const CarState& car, int n) {
    if (!car.remote) return true;
    if (g_netQueriesLeft < n) {
        g_netQueriesWaited++;
        return false;
    }
    g_netQueriesLeft -= n;
    return true;
}
int NetDebrisAwake() {
    int n = 0;
    for (auto& kv : g_cars)
        if (kv.second->remote)
            for (const Debris& d : kv.second->debris) n += d.active && !d.asleep;
    return n;
}

Vec3 MatVec(const float m[9], Vec3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}
Vec3 MatTVec(const float m[9], Vec3 v) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z,
            m[2] * v.x + m[5] * v.y + m[8] * v.z};
}
void MatMul(const float a[9], const float b[9], float o[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, r, sizeof(r));
}
// The matrix of Rotate(., axis, angle), built by rotating the basis (columns).
void RotationMatrix(Vec3 axis, float angle, float m[9]) {
    const Vec3 c[3] = {Rotate({1, 0, 0}, axis, angle), Rotate({0, 1, 0}, axis, angle), Rotate({0, 0, 1}, axis, angle)};
    for (int j = 0; j < 3; j++) m[j] = c[j].x, m[3 + j] = c[j].y, m[6 + j] = c[j].z;
}
void Orthonormalize(float m[9]) {   // the columns
    Vec3 x{m[0], m[3], m[6]}, y{m[1], m[4], m[7]};
    x = Normalize(x);
    y = Normalize(y - x * Dot(x, y));
    const Vec3 z = Cross(x, y);
    const float o[9] = {x.x, y.x, z.x, x.y, y.y, z.y, x.z, y.z, z.z};
    memcpy(m, o, sizeof(o));
}

// The rigid body the car's pose comes from.
uint8_t* RigidFor(const CarState& car, void* renderInfo) {
    (void)renderInfo;
    if (!car.live) return nullptr;
    if (car.rigid) return car.rigid;
    return world::RigidOf(car.car);
}

// The car-space transform of spec i's chain as it is drawn now (the part's own swing, then the part it is
// mounted on): y = rot * x + t.
void ChainTransform(const CarState& car, int i, float rot[9], Vec3& t) {
    const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(rot, id, sizeof(id));
    t = {0, 0, 0};
    for (int s = i; s >= 0; s = kHangSpecs[s].parent) {
        const HangPart& h = car.parts[s];
        if (!h.loose) continue;
        float a[9];
        RotationMatrix(h.axis, h.angle, a);
        if (car.sway[s] != 0.0f) {
            float sw[9];
            RotationMatrix({0, 1, 0}, car.sway[s], sw);
            MatMul(sw, a, a);
        }
        MatMul(a, rot, rot);
        t = h.hinge + MatVec(a, t - h.hinge) + h.offset;
    }
}

// Spec i comes off: it becomes a body in the world, thrown with the car's velocity plus `kick`, spinning at
// `spin` (both world). False when it cannot (no pose, no box): it is just gone.
bool StartDebris(CarState& car, void* renderInfo, int i, Vec3 kick, Vec3 spin) {
    HangPart& h = car.parts[i];
    h.fallen = true;
    h.flying = false;
    Debris& d = car.debris[i];
    d.active = false;
    const HangSpec& spec = kHangSpecs[i];
    uint8_t* rigid = RigidFor(car, renderInfo);
    world::Frame f;
    Vec3 mn, mx;
    if (car.remote && NetDebrisAwake() >= kNetDebrisAwake) {   // a friend's car: enough of theirs on the road now
        g_netDebrisGone++;
        return false;
    }
    if (!g_settings.hang.debris || !world::CarFrame(rigid, f) ||
        !PartBox(renderInfo, spec.slots[0], spec.row < 0 ? 0 : spec.row, mn, mx))
        return false;
    ChainTransform(car, i, d.rot0, d.t0);
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (int k = 0; k < 8; k++) {
        const Vec3 corner{k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z};
        const Vec3 y = MatVec(d.rot0, corner) + d.t0;
        for (int a = 0; a < 3; a++) {
            (&lo.x)[a] = (&y.x)[a] < (&lo.x)[a] ? (&y.x)[a] : (&lo.x)[a];
            (&hi.x)[a] = (&y.x)[a] > (&hi.x)[a] ? (&y.x)[a] : (&hi.x)[a];
        }
    }
    d.c0 = (lo + hi) * 0.5f;
    d.half = (hi - lo) * 0.5f;
    for (int a = 0; a < 3; a++) (&d.half.x)[a] = (&d.half.x)[a] < 0.03f ? 0.03f : (&d.half.x)[a];
    d.p = f.ToWorld(d.c0);
    const float r[9] = {f.x.x, f.y.x, f.z.x, f.x.y, f.y.y, f.z.y, f.x.z, f.y.z, f.z.z};
    memcpy(d.r, r, sizeof(r));
    d.v = world::Velocity(rigid) + kick;
    d.w = spin;
    // A lid or a door is a plate: the air slows it more than a mirror.
    d.drag = spec.kind == HangKind::Droop ? 0.01f : 0.025f;
    d.age = d.still = d.owed = 0;
    d.asleep = false;
    d.leftCar = false;
    d.active = true;
    return true;
}

// One substep of a body's motion on the ground plane (point g, normal n) when `grounded` says there is one.
void DebrisSubstep(Debris& d, float dt, bool grounded, Vec3 g, Vec3 n) {
    d.v.z -= 9.8f * dt;
    d.v = d.v - d.v * (Length(d.v) * d.drag * dt);
    d.w = d.w * expf(-0.3f * dt);
    d.p = d.p + d.v * dt;
    const float wl = Length(d.w);
    if (wl > 1e-5f) {
        float dr[9];
        RotationMatrix(d.w * (1.0f / wl), wl * dt, dr);
        MatMul(dr, d.r, d.r);
        Orthonormalize(d.r);
    }
    if (!grounded) return;
    // The box's corners against the ground: an impulse at each that goes down into it (restitution and friction).
    const Vec3 inv{3.0f / (d.half.y * d.half.y + d.half.z * d.half.z), 3.0f / (d.half.x * d.half.x + d.half.z * d.half.z),
                   3.0f / (d.half.x * d.half.x + d.half.y * d.half.y)};   // 1 / (m (b^2 + c^2) / 12), m = 1, full sizes
    const auto iinv = [&](Vec3 l) {   // world inverse inertia
        const Vec3 b = MatTVec(d.r, l);
        return MatVec(d.r, Vec3{b.x * inv.x, b.y * inv.y, b.z * inv.z});
    };
    float deepest = 0;
    for (int k = 0; k < 8; k++) {
        const Vec3 arm = MatVec(d.r, Vec3{k & 1 ? d.half.x : -d.half.x, k & 2 ? d.half.y : -d.half.y,
                                          k & 4 ? d.half.z : -d.half.z});
        const Vec3 q = d.p + arm;
        const float depth = Dot(g - q, n);
        if (depth <= 0) continue;
        deepest = depth > deepest ? depth : deepest;
        const Vec3 vel = d.v + Cross(d.w, arm);
        const float vn = Dot(vel, n);
        if (vn >= 0) continue;
        const float k1 = 1.0f + Dot(n, Cross(iinv(Cross(arm, n)), arm));
        const float j = -(1.0f + 0.2f) * vn / k1;
        Vec3 impulse = n * j;
        const Vec3 vt = vel - n * vn;
        const float vtl = Length(vt);
        if (vtl > 1e-4f) {
            const Vec3 t = vt * (1.0f / vtl);
            const float kt = 1.0f + Dot(t, Cross(iinv(Cross(arm, t)), arm));
            const float jt = fminf(vtl / kt, 0.6f * j);
            impulse = impulse - t * jt;
        }
        d.v = d.v + impulse;
        d.w = d.w + iinv(Cross(arm, impulse));
    }
    if (deepest > 0) {
        d.p = d.p + n * deepest;
        d.w = d.w * expf(-2.0f * dt);   // rolling resistance: a panel comes to rest rather than rocking forever
    }
}

void StepDebris(CarState& car, Debris& d, float dt, const world::Frame& own) {
    d.age += dt;
    if (d.age > g_settings.hang.debrisSeconds || Length(d.p - own.o) > g_settings.hang.debrisReach) {
        d.active = false;   // gone: the part stays lost
        return;
    }
    // The cars push it: one that runs into it throws it along (the car itself too).
    const float reach = fmaxf(d.half.x, fmaxf(d.half.y, d.half.z));
    world::ForEachCar([&](uint8_t* other, uint8_t* rigid, Vec3 lo, Vec3 hi) {
        Vec3 local;
        const bool inside = world::InCarBox(rigid, lo, hi, d.p, reach * 0.5f, local);
        if (other == car.car && !d.leftCar) {   // it comes off against its own car: only once clear of it
            d.leftCar = !inside && !world::InCarBox(rigid, lo, hi, d.p, reach + 0.3f, local);
            return;
        }
        if (!inside) return;
        const float* m = (const float*)(rigid + world::kRigidRows);
        const Vec3 ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]};
        // out through the nearest side
        const float dx0 = local.x - lo.x, dx1 = hi.x - local.x, dy0 = local.y - lo.y, dy1 = hi.y - local.y;
        const float best = fminf(fminf(dx0, dx1), fminf(dy0, dy1));
        const Vec3 out = best == dx0 ? ax * -1.0f : best == dx1 ? ax : best == dy0 ? ay * -1.0f : ay;
        const Vec3 vc = world::Velocity(rigid);
        if (Dot(d.v - vc, out) < 1.0f) {
            d.v = vc + out * (1.5f + Length(vc) * 0.15f) + Vec3{0, 0, 1.0f + Length(vc) * 0.08f};
            d.w = d.w + Vec3{(float)(rand() % 200 - 100) * 0.06f, (float)(rand() % 200 - 100) * 0.06f,
                             (float)(rand() % 200 - 100) * 0.06f};
            d.p = d.p + out * (best + reach * 0.5f);
            d.asleep = false;
            d.still = 0;
        }
    });
    if (d.asleep) return;
    if (!MayQuery(car, 2)) {   // (a friend's car's part: its step when the frame has room, with the time it waited)
        d.owed = fminf(d.owed + dt, kDebrisOwedMax);
        return;
    }
    dt += d.owed;
    d.owed = 0;
    float gz = 0;
    Vec3 gn{0, 0, 1};
    const bool grounded = world::Ground(d.p, gz, gn);
    const Vec3 g{d.p.x, d.p.y, gz};
    const Vec3 before = d.p;
    for (float left = dt; left > 1e-5f; left -= kDebrisStep)
        DebrisSubstep(d, left < kDebrisStep ? left : kDebrisStep, grounded, g, gn);
    // A wall in the way: it bounces off.
    Vec3 hit, wn;
    const Vec3 move = d.p - before;
    if (Length(move) > 1e-3f && world::Wall(before, d.p + Normalize(move) * reach * 0.5f, hit, wn)) {
        d.p = hit + wn * (reach * 0.5f + 0.02f);
        const float vn = Dot(d.v, wn);
        if (vn < 0) d.v = d.v - wn * (1.4f * vn);
        d.v = d.v * 0.7f;
        d.w = d.w * 0.7f;
    }
    // At rest: it sleeps until something hits it.
    const bool resting = grounded && d.p.z - gz < reach + 0.05f && Length(d.v) < 0.15f && Length(d.w) < 0.4f;
    d.still = resting ? d.still + dt : 0;
    if (d.still > 0.6f) {
        d.asleep = true;
        d.v = d.w = {0, 0, 0};
    }
}

// An open door against the walls and the other cars. The obstacle pushes its free edge: towards shutting (the
// car drives on with it open) it slams it, towards opening (hit from behind, or reversing) it swings it open, and
// hard enough tears it off.
void CollideDoor(CarState& car, void* renderInfo, int i, const world::Frame& now, const world::Frame& before, float dt) {
    HangPart& h = car.parts[i];
    if (car.doorCooldown[i] > 0) {
        car.doorCooldown[i] -= dt;
        return;
    }
    if (h.angle < 0.08f || h.lever < 0.3f || !MayQuery(car, 1)) return;
    const Vec3 edge{-h.lever, 0, 0};   // hinge to the free edge, closed (the hinge is at the door's front)
    const Vec3 tip = h.hinge + Rotate(edge, h.axis, h.angle), mid = h.hinge + Rotate(edge * 0.5f, h.axis, h.angle);
    const Vec3 tipW = now.ToWorld(tip), midW = now.ToWorld(mid);
    const Vec3 vTip = (tipW - before.ToWorld(tip)) * (1.0f / dt);
    const Vec3 open = now.DirToWorld(Normalize(Cross(h.axis, tip - h.hinge)));
    bool hit = false;
    Vec3 vObstacle{0, 0, 0}, wp, wn;
    if (world::Wall(now.ToWorld(h.hinge + Rotate(edge * 0.2f, h.axis, h.angle)), tipW, wp, wn)) hit = true;
    if (!hit)
        world::ForEachCar([&](uint8_t* other, uint8_t* rigid, Vec3 lo, Vec3 hi) {
            if (hit || other == car.car) return;
            Vec3 local;
            if (world::InCarBox(rigid, lo, hi, tipW, 0.02f, local) || world::InCarBox(rigid, lo, hi, midW, 0.02f, local)) {
                hit = true;
                vObstacle = world::Velocity(rigid);
            }
        });
    if (!hit) return;
    const Vec3 rel = vTip - vObstacle;
    const float push = Dot(rel * -1.0f, open);   // how fast the obstacle pushes the edge the opening way
    const HangSettings& hs = g_settings.hang;
    if (push > hs.doorTearSpeed) {
        const Vec3 kick = rel * -0.35f + Vec3{0, 0, 1.2f};
        const Vec3 spin = now.z * (kHangSpecs[i].side * -3.0f) + now.x * ((float)(rand() % 200 - 100) * 0.02f);
        StartDebris(car, renderInfo, i, kick, spin);
        if (g_settings.logHits) Log("parts: %s torn off from behind (%.1f m/s)", kHangSpecs[i].name, push);
        return;
    }
    if (push > 0)
        h.rate = fmaxf(h.rate, 1.2f * push / h.lever);
    else
        h.rate = fminf(h.rate, 1.2f * push / h.lever - 0.5f);   // shut against it
    car.doorCooldown[i] = 0.12f;
}

// Swings the loose parts of one car, as hinged plates driven by the car's acceleration (inertia), gravity
// and the airflow. Droopers fall and bounce off the ground; lids rest slightly open and flutter; doors
// swing out under braking and in turns and slam against the body when the airflow shuts them; a dangling
// mirror is thrown out and back against the door by the turns.
void SwingParts(CarState& car, void* renderInfo, float dt) {
    if (!car.live || dt <= 0) return;
    const HangSettings& hs = g_settings.hang;
    const float* m = car.rigid ? &At<float>(car.rigid, kRigidOrientation) : &At<float>(car.car, kCarMatrix);
    const Vec3 ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]}, az{m[8], m[9], m[10]};
    const Vec3 vw = car.rigid ? At<Vec3>(car.rigid, kRigidVelocity) : At<Vec3>(car.car, kCarVelocity);
    const Vec3 v{Dot(vw, ax), Dot(vw, ay), Dot(vw, az)};
    Vec3 a{0, 0, 0};
    Vec3 aw{0, 0, 0};   // the true acceleration, in car space (turning at a steady speed included)
    if (car.hasVelocity) {
        a = (v - car.lastVelocity) * (1.0f / dt);
        const Vec3 d = (vw - car.lastWorldVelocity) * (1.0f / dt);
        aw = {Dot(d, ax), Dot(d, ay), Dot(d, az)};
    }
    car.lastVelocity = v;
    car.lastWorldVelocity = vw;
    car.hasVelocity = true;
    const auto clamp = [](float x, float lim) { return x < -lim ? -lim : x > lim ? lim : x; };
    a = {clamp(a.x, 40), clamp(a.y, 40), clamp(a.z, 40)};
    aw = {clamp(aw.x, 40), clamp(aw.y, 40), clamp(aw.z, 40)};
    const float speed = Length(v);
    const float airflow = speed * speed * hs.windOpen;
    car.flutterPhase += dt * (6.0f + speed * 0.4f);
    world::Frame frameNow{};
    const bool posed = world::CarFrame(RigidFor(car, renderInfo), frameNow);
    const bool hadFrame = car.hasFrame;
    const world::Frame frameBefore = car.lastFrame;
    car.hasFrame = posed;
    if (posed) car.lastFrame = frameNow;
    const float gust = sinf(car.flutterPhase) * sinf(car.flutterPhase * 2.3f + 1.0f);

    for (int i = 0; i < kHangCount; i++) {
        const HangSpec& spec = kHangSpecs[i];
        HangPart& h = car.parts[i];
        const bool mirror = spec.slots[0] == kSlotWingMirror && hs.mirrorSwing > 0;
        // A dangling mirror rests on the body at MirrorDropDeg.
        if (spec.slots[0] == kSlotWingMirror && h.loose) {
            const float drop = hs.mirrorDropDeg * kPi / 180.0f;
            if (h.maxAngle > drop) h.maxAngle = drop;
            if (h.angle > h.maxAngle) h.angle = h.maxAngle;
        }
        if (!h.loose || h.fallen || !mirror) car.sway[i] = car.swayRate[i] = 0;
        Debris& debris = car.debris[i];
        if (!h.fallen) debris.active = false;   // repaired
        if (debris.active) {
            if (posed) StepDebris(car, debris, dt, frameNow);
            continue;
        }
        if (!h.loose || h.fallen) continue;
        if (spec.kind == HangKind::Door && hs.doorCollide && posed && hadFrame) {
            CollideDoor(car, renderInfo, i, frameNow, frameBefore, dt);
            if (h.fallen) continue;
        }
        // At speed the airflow tears an open hood away: it flips over the car, rises and drops behind.
        if (spec.kind == HangKind::Lift && spec.wind > 0 && !h.flying && speed > hs.hoodFlySpeed &&
            h.angle > 0.15f) {
            h.flying = true;
            h.flyTime = 0;
            // Thrown up off its hinge, spinning over: the air slows it and it lands behind the car.
            if (posed && StartDebris(car, renderInfo, i, frameNow.z * hs.hoodFlyLift, frameNow.DirToWorld(h.axis) * 6.0f))
                continue;
            h.fallen = false;   // no world to land in: the old flight
            h.flying = true;
        }
        if (h.flying) {
            h.flyTime += dt;
            const float t = h.flyTime;
            h.angle += dt * 7.0f;
            // Thrown up at HoodFlyLift m/s, falling at g, swept back by the airflow.
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
                    // A dangling mirror: the turns throw it out or against the door, the airflow buffets it.
                    if (mirror)
                        acc += hs.mirrorSwing * (1.5f * aw.y * spec.side * s / lever + hs.flutter * speed * 0.08f * gust);
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
// Bent wheels (drawn only). A dent near a wheel bends it (CarState::wheel, from the physics wheels' positions);
// Render(view, rig, pos, orient, tireMats, brakeMats, spinnerMats, ...) takes the four wheels' matrices as arrays
// of 4 bMatrix4 (row vectors; the tyre's mid-plane at the origin, the axle along Y, so a matrix holds the spin and
// the steering), which it reads at 0x62710D. They are copied here (into the guest heap), the damaged wheels bent,
// and Render is handed the copies: the caller's matrices are left as they were. Render's wheel order: 0 FL, 1 FR,
// 2 RR, 3 RL. Pre-multiplied (in the spinning frame) the bend is a bent rim that wobbles; post-multiplied (in the
// car's axes) it is the bent suspension's lean and toe.

constexpr size_t kCarKind = 0x28, kCarMover = 0x34;   // kind 1 = PhysicsMover
constexpr uint32_t kMoverVtable = 0x7A25E0;
constexpr size_t kMoverWheels = 0x38, kWheelInfo = 0x14;
constexpr int kRenderTireArg = 4;   // Render's stack arguments: tyres, then brakes, then spinners

// The car's PhysicsMover, when it has one.
uint8_t* MoverOf(uint8_t* car) {
    if (!car || !dmg_committed(A(car), kCarMover + 4) || At<int>(car, kCarKind) != 1) return nullptr;
    uint8_t* mover = At<uint8_t*>(car, kCarMover);
    return mover && dmg_committed(A(mover), kMoverWheels + 16) && At<uint32_t>(mover, 0) == kMoverVtable ? mover : nullptr;
}

// Car-space position of a physics wheel (its WheelInfo).
Vec3 WheelPosition(uint8_t* mover, int i) {
    const uint8_t* wheel = At<uint8_t*>(mover, kMoverWheels + i * 4);
    if (!wheel || !dmg_committed(A(wheel) + kWheelInfo, 4)) return {0, 0, 0};
    const uint8_t* info = At<uint8_t*>(wheel, kWheelInfo);
    return info && dmg_committed(A(info), 12) ? At<Vec3>(info, 0) : Vec3{0, 0, 0};
}

// A dent bends the wheels near it.
void BendNear(uint8_t* car, CarState& state, const Dent& dent) {
    const WheelSettings& ws = g_settings.wheels;
    uint8_t* mover = ws.enabled ? MoverOf(car) : nullptr;
    if (!mover) return;
    for (int i = 0; i < 4; i++) {
        const Vec3 w = WheelPosition(mover, i);
        const float d = sqrtf((w.x - dent.point.x) * (w.x - dent.point.x) + (w.y - dent.point.y) * (w.y - dent.point.y));
        if (d >= ws.wheelReach) continue;
        const float v = state.wheel[i] + dent.depth * ws.wheelPerDepth * (1.0f - d / ws.wheelReach);
        state.wheel[i] = v > 1.0f ? 1.0f : v;
    }
}

void MulRot(float* m, const float r[9], bool before) {   // m's 3x3 = r . m (before) or m . r
    float o[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += before ? r[i * 3 + k] * m[k * 4 + j] : m[i * 4 + k] * r[k * 3 + j];
            o[i * 3 + j] = v;
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i * 4 + j] = o[i * 3 + j];
}
// Row-vector rotations: about X (+ moves Z towards -Y) and about Z (+ turns X towards +Y).
void RotX(float r[9], float a) {
    const float c = cosf(a), s = sinf(a);
    const float m[9] = {1, 0, 0, 0, c, s, 0, -s, c};
    memcpy(r, m, sizeof(m));
}
void RotZ(float r[9], float a) {
    const float c = cosf(a), s = sinf(a);
    const float m[9] = {c, s, 0, -s, c, 0, 0, 0, 1};
    memcpy(r, m, sizeof(m));
}

// Render puts the wheels' matrices on the view's lists by address (0x5C5930), drawn at the frame's end: each Render of
// the frame with bent wheels gets a set of its own, kWheelSets at most (past that the wheels are drawn straight).
constexpr int kWheelSets = 64;
uint32_t g_wheelMats = 0;   // guest: kWheelSets sets x 3 kinds x 4 wheels x 16 floats
int g_wheelMatSet = 0;      // sets used this frame (OnFrame starts again)

void BendWheels(void* renderInfo, uint32_t args) {
    const WheelSettings& ws = g_settings.wheels;
    if (!ws.enabled || !args || !dmg_committed(args, (kRenderTireArg + 3) * 4)) return;
    auto it = g_cars.find(renderInfo);
    if (it == g_cars.end()) return;
    const CarState& state = *it->second;
    // Damage per render wheel, from the physics wheels' positions (order FL, FR, RR, RL when there is no car).
    float hurt[4] = {state.wheel[0], state.wheel[1], state.wheel[2], state.wheel[3]};
    if (uint8_t* mover = state.live ? MoverOf(state.car) : nullptr)
        for (int j = 0; j < 4; j++) {
            const Vec3 p = WheelPosition(mover, j);
            if (p.x == 0.0f && p.y == 0.0f) continue;
            const int r = p.x > 0 ? (p.y > 0 ? 0 : 1) : (p.y > 0 ? 3 : 2);
            hurt[r] = state.wheel[j];
        }
    if (hurt[0] <= 0 && hurt[1] <= 0 && hurt[2] <= 0 && hurt[3] <= 0) return;
    if (!g_wheelMats) g_wheelMats = dmg_alloc(kWheelSets * 3 * 4 * 64);
    if (!g_wheelMats || g_wheelMatSet >= kWheelSets) return;
    float (*set)[4][16] = (float (*)[4][16])G(g_wheelMats + (uint32_t)g_wheelMatSet * 3 * 4 * 64);
    g_wheelMatSet++;
    const float deg = kPi / 180.0f;
    for (int kind = 0; kind < 3; kind++) {   // tyres, brakes, spinners
        const uint32_t srcAddr = R32(args + 4u * (kRenderTireArg + kind));
        if (!srcAddr || !dmg_committed(srcAddr, 4 * 64)) continue;
        memcpy(set[kind], G(srcAddr), sizeof(set[kind]));
        for (int i = 0; i < 4; i++) {
            if (hurt[i] <= 0) continue;
            const float side = (i == 0 || i == 3) ? 1.0f : -1.0f;   // +1 left
            float* m = set[kind][i];
            float r[9];
            if (kind != 1) {   // the rim's wobble turns with it (the brake does not spin)
                RotX(r, hurt[i] * ws.bendWobbleDeg * deg);
                MulRot(m, r, true);
            }
            RotX(r, side * hurt[i] * ws.bendCamberDeg * deg);   // the top leans in
            MulRot(m, r, false);
            RotZ(r, side * hurt[i] * ws.bendToeDeg * deg);      // the front turns out
            MulRot(m, r, false);
        }
        At<uint32_t>(G(args), 4u * (kRenderTireArg + kind)) = A(set[kind]);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Once a frame (the first car render of the frame)

constexpr int kNetSlots = 64;   // roam_game.c's slots of friends' cars (its NSLOT: it asserts that they are as many)
int NetHeld(void* held[kNetSlots]);   // free roam (below): friends' cars whose dents are being made

void OnFrame() {
    WorldWatch();
    const uint32_t frame = Frame();
    if (frame == g_lastFrame) return;
    g_lastFrame = frame;
    g_wheelMatSet = 0;
    g_netQueriesLeft = kNetWorldQueries;

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

    // A car no longer the world's (deleted with the world still there) is not touched again: its clones go once its
    // models no longer draw them (a model that still might keeps its clone).
    for (auto& kv : g_cars) kv.second->live = CarLive(kv.second->car, kv.first);
    std::vector<Clone*> stale;
    for (auto& kv : g_clones)
        if (!Live(kv.second)) stale.push_back(kv.second);
    for (Clone* c : stale) Retire(c, false);

    // A car whose clones are all gone was repaired, rebuilt or destroyed: its state goes too, and its
    // lights come back if the car is still live with this render info.
    std::vector<void*> gone;
    for (auto& kv : g_cars)
        if (!HasClones(kv.first)) gone.push_back(kv.first);
    for (void* r : gone) Forget(r, g_cars[r]->live);

    // The marker draws of the cars no longer drawn go (every car the game ever drew had its entry).
    if (frame % kMarkerKeepFrames == 0)
        for (auto it = g_markerDraws.begin(); it != g_markerDraws.end();)
            it = frame - it->second.frame > kMarkerKeepFrames ? g_markerDraws.erase(it) : std::next(it);

    // Friends' cars' turn at the world queries goes round: a different one first each frame.
    std::vector<std::pair<void*, CarState*>> order(g_cars.begin(), g_cars.end());
    static uint32_t round = 0;
    if (!order.empty()) std::rotate(order.begin(), order.begin() + (round++ % order.size()), order.end());
    for (auto& kv : order) {
        CarState& car = *kv.second;
        SwingParts(car, kv.first, dt);
        // The game's own part damage hides an aftermarket mirror past 0.25; the loose parts are ours.
        if (car.live)
            if (uint8_t* carDamage = At<uint8_t*>(car.car, kCarDamage))
                memset(carDamage + kCarPartDamageValues, 0, kCarPartDamageCount * sizeof(float));
        for (int i = 0; i < kHangCount; i++) {
            HangPart& h = car.parts[i];
            if (h.fallen == h.shownFallen && fabsf(h.angle - h.shownAngle) <= 0.002f &&
                fabsf(car.sway[i] - car.shownSway[i]) <= 0.002f && !(car.debris[i].active && dt > 0) &&
                car.debris[i].active == car.shownDebris[i])
                continue;
            for (auto& c : g_clones) {
                if (c.second->renderInfo != kv.first) continue;
                const int spec = SpecOf(c.second);   // the part itself, or one mounted on it (a mirror)
                if (spec == i || (spec >= 0 && kHangSpecs[spec].parent == i)) c.second->dirty = true;
            }
            h.shownFallen = h.fallen;
            h.shownAngle = h.angle;
            car.shownSway[i] = car.sway[i];
            car.shownDebris[i] = car.debris[i].active;
        }
    }

    // A friend's car whose dents are being made (a few a frame) is drawn as it was until they are all made. A clone
    // not drawn last frame (another LOD of the part is) waits until it is: drawn once as it was, then uploaded. One
    // never drawn waits too where another copy of its part (a LOD, a row) was seen drawn: Render draws the part, just
    // not this copy (most of a car's copies: every part has 4 LODs). A part never seen drawn by Render's part loop has
    // its copies uploaded at each change, as no draw would say when.
    void* held[kNetSlots];
    const int nheld = NetHeld(held);
    const auto partKey = [](const Clone* c) { return (uint64_t)A(c->renderInfo) << 8 | (uint8_t)c->slot; };
    std::unordered_set<uint64_t> partsDrawn;
    for (auto& kv : g_clones)
        if (kv.second->drawnAt && kv.second->instance < 0) partsDrawn.insert(partKey(kv.second));
    for (auto& kv : g_clones) {
        Clone* c = kv.second;
        bool hold = false;
        for (int k = 0; k < nheld && !hold; k++) hold = c->renderInfo == held[k];
        if (hold) continue;
        if (c->drawnAt ? frame - c->drawnAt > 1 : c->instance < 0 && partsDrawn.count(partKey(c))) continue;
        if (c->overlayDirty) RebuildOverlay(c);
        if (c->dirty) Upload(c);
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
// Free roam: friends see the damage (roam_core's RC_MSG_DENTS, roam_game.c).
//
// The player's car is the only one whose damage its game decides: each dent (after its snap to the surface) goes in
// a log, quantized as it goes on the wire, so the dents made here are bit for bit the ones a friend's game makes. The
// log is published (a new revision, which roam_game.c puts in the record) at most every 1.5 s, 0.4 s after the last
// dent: a crash's burst of contacts is one record. A repair, or another car, starts a new epoch. A friend's car gets
// the dents its player's record has through the same steps as an impact here (dents, loose and lost parts, lights,
// wheels, the scratches around the dent), only the ones not made yet; when its parts are rebuilt (or the game repairs
// it) they are made again from the first. Impacts on a friend's car here make nothing: theirs do.
//
// What another player's records can make this game do is kept within limits (any record may come from a game that
// does not keep to the rules above):
//  - the dents are made a few a frame, kNetMsPerFrame across all cars (one at least), the batch with the fewest left
//    first; the car is drawn as it was until its batch is done;
//  - a car's damage is made anew at most every kNetEveryMs (a crash's record comes no more often), its repair (a new
//    epoch) at most every kNetRepairMs: what came in between waits, and the record then is made (the latest);
//  - only the kNetCars nearest friends' cars with damage show it (and kNetKeep more that have it already), and no
//    new one while the guest address space has less than kNetVaReserve free (the game's own cars and world come
//    first); a car whose clones found no memory is left undamaged for kNetFailMs;
//  - a dent no crash makes is not made: no push, or outside any car's body; the depth at most a hit's, the strength
//    (the scratches) at most the one its depth came from.

namespace {

struct NetDent {
    uint8_t b[RC_DMG_BYTES];
};
struct NetMine {
    void* renderInfo = nullptr;   // the player's car the log is of
    uint8_t epoch = 0;
    uint16_t first = 0;           // the number of the log's first dent in the epoch
    std::deque<NetDent> dents;
    bool pending = false;         // changed since the last publication
    double lastDent = 0, lastPub = -1e9;
    // as published
    uint32_t rev = 0;
    uint8_t pubEpoch = 0;
    uint16_t pubFirst = 0;
    std::vector<NetDent> pub;
} g_mine;

constexpr double kNetMsPerFrame = 2.0;          // friends' dents made per frame, all cars (one dent at least)
constexpr double kNetEveryMs = 1500.0;          // a car's damage made anew at most this often
constexpr double kNetRepairMs = 4000.0;         // a car's repair (a new epoch) taken at most this often
int g_netCars = 8;                              // friends' cars that show their damage, the nearest,
int g_netKeep = 4;                              // and this many more that have it already (the cars move about);
                                                // dmg_init's: fewer where the guest heap has no window (a phone)
constexpr int kNetPrewarmCars = 2;              // friends' cars whose parts are refined ahead at once (the others
                                                // wait their turn: the cache keeps theirs)
constexpr uint32_t kNetVaReserve = 128u << 20;  // guest address space left to the game: no new clones below it,
constexpr uint32_t kNetVaLow = 64u << 20;       // and below this the farthest friend's car's damage goes (a second)
constexpr double kNetFailMs = 30000.0;          // a car whose clones found no memory: undamaged for this long
constexpr double kNetPrewarmMs = 5000.0;        // its parts refined ahead first, for at most this long
constexpr double kNetFarMs = 3000.0;            // a car no longer among the nearest (or not shown) keeps its damage this long
// The box a car's body is within, in its space: the car's own collision box grown by kNetBoxGrow (or, without one,
// the game's largest car, with room), no more than the largest box below; a dent's point outside it, or a push of no
// direction, is no crash's.
constexpr float kNetReachX = 3.5f, kNetReachY = 1.6f, kNetLowZ = -0.5f, kNetHighZ = 2.8f;
constexpr float kNetBoxGrow = 0.6f;
constexpr float kNetMostX = 9.0f, kNetMostY = 2.5f, kNetMostZ = 5.0f;

struct NetSlot {
    uint32_t car = 0, peer = 0;
    void* renderInfo = nullptr;
    uint8_t epoch = 0;
    uint32_t next = 0;            // the first dent not made yet
    bool any = false;             // dents made on the car
    uint32_t seen = 0;            // frame the slot last had this car
    int dents = 0;                // the dents its record has
    float dist = 0;               // from the player's car
    bool near = false;            // among the kNetCars nearest with damage
    // a batch: the dents up to `end` (the record's when it began), a few a frame
    bool busy = false, ready = false;   // ready: the car's parts are refined ahead (or it had clones)
    uint32_t end = 0, from = 0;
    int cloneAt = 0;                    // CloneSome's place
    int frames = 0, dropped = 0;
    double work = 0, dentMs = 1.5;      // the batch's time, and a dent's (an average) for the budget
    double batchAt = -1e9, repairAt = -1e9, waitAt = 0, failUntil = 0, farSince = 0;
    bool noteWait = false, noteRoom = false, noteBad = false;   // logged once a car
    int waitedEpoch = -1;               // the last repair that waited (counted once)
    Vec3 boxLo{-kNetReachX, -kNetReachY, kNetLowZ}, boxHi{kNetReachX, kNetReachY, kNetHighZ};   // where its dents may be
};
NetSlot g_netSlots[kNetSlots];
void NetForgetAll() {   // the world changed (WorldWatch): the slots' cars and render infos are gone, nothing to take off
    for (NetSlot& s : g_netSlots) s = NetSlot{};
}
uint32_t g_netFrame = 0xFFFFFFFF;   // the frame g_netSpent is of
double g_netSpent = 0;              // ms of the frame's budget used
int g_netSteps = 0;                 // steps (a dent, a clone) made this frame
int g_netBest = -1;                 // the slot whose batch goes first this frame (-1: whichever comes)
struct NetStats {                   // logged every 30 s when there was any
    int batches = 0, stopped = 0, dents = 0, dropped = 0, repairs = 0, waited = 0, prewarmWaits = 0;
    double ms = 0, worst = 0;       // the work, and the most of it in one frame
} g_netStats;
double g_netStatsAt = 0;

// Player 1's car: Player* +4. The players' table is the game's 0x8900AC, which the browser build moves for split screen
// (splitscreen_mem.h): read where the game's code reads it.
uint8_t* PlayerCar() {
    const uint32_t p = R32(SPLIT_PLAYERS_INDEX);
    return p && dmg_committed(p + 4, 4) ? G(R32(p + 4)) : nullptr;
}

// A friend's car (slot of roam_game.c), as of the last frames.
bool IsRemote(uint32_t car) {
    if (!car) return false;
    for (const NetSlot& s : g_netSlots)
        if (s.car == car && Frame() - s.seen < 120) return true;
    return false;
}

// A friend's car whose batch waits for its parts' refinement (kNetPrewarmCars at most): they are refined ahead (only
// those: the others wait their turn, and their parts are not kept in the cache meanwhile).
bool NetWants(uint32_t car) {
    const NetSlot* now = nullptr;   // the slot that had the car last
    for (const NetSlot& s : g_netSlots)
        if (s.car == car && Frame() - s.seen < 120 && (!now || s.seen > now->seen)) now = &s;
    return now && now->busy && !now->ready && now->near && NowMs() >= now->failUntil;
}

// The friends' cars whose batch is under way: drawn as they were until it is done (OnFrame).
int NetHeld(void* held[kNetSlots]) {
    int n = 0;
    for (const NetSlot& s : g_netSlots)
        if (s.busy && s.renderInfo && Frame() - s.seen <= 2) held[n++] = s.renderInfo;
    return n;
}

void EncodeDent(const Dent& d, float magnitude, NetDent& o) {
    const auto i16 = [](float v) { v = roundf(v * 1000.0f); return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); };
    const auto i8 = [](float v) { v = roundf(v * 127.0f); return (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v); };
    const int16_t p[3] = {i16(d.point.x), i16(d.point.y), i16(d.point.z)};
    memcpy(o.b, p, 6);   // little-endian, as the wire
    o.b[6] = (uint8_t)i8(d.direction.x), o.b[7] = (uint8_t)i8(d.direction.y), o.b[8] = (uint8_t)i8(d.direction.z);
    const float depth = roundf(d.depth / RC_DMG_DEPTH * 255.0f);
    o.b[9] = (uint8_t)(depth < 0 ? 0 : depth > 255 ? 255 : depth);
    const float m = roundf(magnitude);
    o.b[10] = (uint8_t)(m < 0 ? 0 : m > 255 ? 255 : m);
}
void DecodeDent(const uint8_t* b, Dent& d, float& magnitude) {
    int16_t p[3];
    memcpy(p, b, 6);
    d.point = {p[0] / 1000.0f, p[1] / 1000.0f, p[2] / 1000.0f};
    d.direction = Normalize(Vec3{(int8_t)b[6] / 127.0f, (int8_t)b[7] / 127.0f, (int8_t)b[8] / 127.0f});
    d.depth = b[9] * RC_DMG_DEPTH / 255.0f;
    magnitude = b[10];
}
// A dent a friend's game sent, as a crash here would make it; false for one no crash makes (not made). lo, hi: where
// on the car it may be (NetBox).
bool NetDecode(const uint8_t* b, Dent& d, float& magnitude, Vec3 lo, Vec3 hi) {
    const int px = (int8_t)b[6], py = (int8_t)b[7], pz = (int8_t)b[8];
    if (px * px + py * py + pz * pz < 64 * 64) return false;   // a push is a unit vector (x 127)
    DecodeDent(b, d, magnitude);
    if (!(d.point.x >= lo.x && d.point.x <= hi.x && d.point.y >= lo.y && d.point.y <= hi.y && d.point.z >= lo.z &&
          d.point.z <= hi.z))
        return false;
    const Settings& st = g_settings;
    if (d.depth > st.maxDepthPerHit) d.depth = st.maxDepthPerHit;
    // Below the most one hit makes, the depth follows from the strength (dmg_impulse): the strength no more than that.
    if (d.depth < st.maxDepthPerHit - 0.002f) {
        const float most = st.impulseThreshold + d.depth / st.depthPerImpulse + 1.0f;
        if (magnitude > most) magnitude = most;
    }
    return true;
}

void NetReset(void* renderInfo) {
    g_mine.renderInfo = renderInfo;
    g_mine.epoch++;
    g_mine.first = 0;
    g_mine.dents.clear();
    g_mine.pending = true;
    g_mine.lastDent = NowMs();
}

// A dent of the player's car: quantized as it goes on the wire (made so here too), and logged.
void NetRecord(void* renderInfo, Dent& dent, float magnitude) {
    if (g_mine.renderInfo != renderInfo) NetReset(renderInfo);
    NetDent d;
    EncodeDent(dent, magnitude, d);
    float m;
    DecodeDent(d.b, dent, m);
    g_mine.dents.push_back(d);
    if ((int)g_mine.dents.size() > RC_DMG_MAX) {
        g_mine.dents.pop_front();
        g_mine.first++;
    }
    g_mine.pending = true;
    g_mine.lastDent = NowMs();
}

// Once a frame: the player's car gone or changed (a new log), and the log published when due.
void NetFrame() {
    uint8_t* car = PlayerCar();
    void* ri = car && dmg_committed(A(car) + kCarRenderInfo, 4) ? At<void*>(car, kCarRenderInfo) : nullptr;
    if (ri != g_mine.renderInfo && !g_mine.dents.empty()) NetReset(ri);
    const double now = NowMs();
    if (!g_mine.pending || now - g_mine.lastDent < 400.0 || now - g_mine.lastPub < 1500.0) return;
    g_mine.pending = false;
    g_mine.lastPub = now;
    g_mine.pubEpoch = g_mine.epoch;
    g_mine.pubFirst = g_mine.first;
    g_mine.pub.assign(g_mine.dents.begin(), g_mine.dents.end());
    g_mine.rev++;
    Log("free roam: damage published (epoch %u, dents %u..%u)", g_mine.pubEpoch, g_mine.pubFirst,
                                (unsigned)(g_mine.pubFirst + g_mine.pub.size()));
}

// The steps of an impact, for a dent of a friend's car.
void ApplyNetDent(void* renderInfo, uint8_t* car, const Dent& dent, float magnitude) {
    CarState* state = StateFor(renderInfo, car, true);
    SetCar(*state, car);
    state->remote = true;
    state->live = true;   // (dmg_net_car has the car from roam_game.c's slot, shown in this world)
    uint8_t* carDamage = dmg_committed(A(car) + kCarDamage, 4) ? At<uint8_t*>(car, kCarDamage) : nullptr;
    if (g_settings.wear.nativeGlass && carDamage && dmg_committed(A(carDamage) + kCarDamageVisible, 1) &&
        !At<uint8_t>(carDamage, kCarDamageVisible)) {
        const uint32_t one = 1;
        dmg_thiscall(kSetVisibleDamageEnabled, A(carDamage), 1, &one);
    }
    EnsureClones(renderInfo, false);
    if (dent.depth > 0) {
        DentCar(renderInfo, *state, dent);
        if (g_settings.hang.enabled) DamageParts(renderInfo, *state, dent);
        BreakLights(renderInfo, *state, dent);
        BendNear(car, *state, dent);
    }
    const WearSettings& ws = g_settings.wear;
    if (magnitude > ws.scratchThreshold) {
        float amount = (magnitude - ws.scratchThreshold) * ws.scratchPerImpulse;
        amount = amount > 1.0f ? 1.0f : amount;
        ScratchCar(renderInfo, dent.point, amount, ws.scratchRadius + 0.015f * magnitude);
    }
}

// Where a car's dents may be, in its space (the dents'): its collision body's box (relative to its rigid body; the
// space is the rigid body's less its centre of gravity) grown by kNetBoxGrow, within the largest box; false without one
// (the fixed box stays).
bool NetBox(uint8_t* car, Vec3& lo, Vec3& hi) {
    if (!car || !dmg_committed(A(car) + world::kCarBody, 4)) return false;
    uint8_t* body = At<uint8_t*>(car, world::kCarBody);
    if (!body || !dmg_committed(A(body), world::kBodyBoxMax + 12) || !dmg_committed(A(body) + world::kBodyRigid, 4))
        return false;
    uint8_t* rigid = At<uint8_t*>(body, world::kBodyRigid);
    if (!rigid || !dmg_committed(A(rigid), world::kRigidCg + 12)) return false;
    const Vec3 a = At<Vec3>(body, world::kBodyBoxMin), b = At<Vec3>(body, world::kBodyBoxMax), cg = At<Vec3>(rigid, world::kRigidCg);
    if (!(b.x - a.x > 0.5f && b.y - a.y > 0.5f && b.z - a.z > 0.2f && b.x - a.x < 30.0f && b.y - a.y < 10.0f &&
          b.z - a.z < 10.0f && fabsf(cg.x) < 5.0f && fabsf(cg.y) < 5.0f && fabsf(cg.z) < 5.0f))
        return false;
    const auto clamp = [](float v, float most) { return v < -most ? -most : v > most ? most : v; };
    lo = {clamp(a.x + cg.x - kNetBoxGrow, kNetMostX), clamp(a.y + cg.y - kNetBoxGrow, kNetMostY),
          clamp(a.z + cg.z - kNetBoxGrow, kNetMostZ)};
    hi = {clamp(b.x + cg.x + kNetBoxGrow, kNetMostX), clamp(b.y + cg.y + kNetBoxGrow, kNetMostY),
          clamp(b.z + cg.z + kNetBoxGrow, kNetMostZ)};
    return true;
}

// The slot's damage taken off its car (made again from its first dent when it may be). A car that went has nothing
// to put back (RepairRenderInfo touches nothing of a car that is not live): its clones are no longer live, and
// OnFrame lets them go.
void NetDrop(NetSlot& s) {
    if (s.renderInfo && (HasClones(s.renderInfo) || g_cars.count(s.renderInfo))) RepairRenderInfo(s.renderInfo, G(s.car));
    s.any = s.busy = false;
    s.next = 0;
}

// Once a frame, at its first slot: the frame's budget, the nearest cars with damage, the batch that goes first, and
// the farthest car's damage let go while the address space is short.
void NetBeginFrame(double now) {
    if (g_netFrame == Frame()) return;
    g_netFrame = Frame();
    NetStats& st = g_netStats;
    if (g_netSpent > st.worst) st.worst = g_netSpent;
    if (now - g_netStatsAt >= 30000.0) {
        if (st.batches || st.stopped || st.dents || st.dropped || st.repairs || st.waited || g_netQueriesWaited ||
            g_netDebrisGone || g_unrefined)
            Log("free roam: friends' damage in %.0f s: %d batches made, %d stopped (a repair, or the car went), %d dents "
                "made, %d no crash makes left out, %d repairs, %d changes waited, %d waits for a turn at the refinement; "
                "%.0f ms of work, at most %.2f ms in a frame; %d world queries waited, %d parts gone (enough on the "
                "road), %d parts cloned unrefined", (now - g_netStatsAt) / 1000.0, st.batches, st.stopped, st.dents,
                st.dropped, st.repairs, st.waited, st.prewarmWaits, st.ms, st.worst, g_netQueriesWaited,
                g_netDebrisGone, g_unrefined);
        st = NetStats{};
        g_netQueriesWaited = g_netDebrisGone = g_unrefined = 0;
        g_netStatsAt = now;
    }
    g_netSpent = 0;
    g_netSteps = 0;
    int order[kNetSlots], n = 0;
    for (int k = 0; k < kNetSlots; k++) {
        NetSlot& s = g_netSlots[k];
        s.near = false;
        if (s.car && Frame() - s.seen <= 2 && s.dents > 0 && now >= s.failUntil) order[n++] = k;
    }
    for (int i = 1; i < n; i++)   // nearest first
        for (int j = i; j > 0 && g_netSlots[order[j]].dist < g_netSlots[order[j - 1]].dist; j--) {
            const int t = order[j];
            order[j] = order[j - 1], order[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        NetSlot& s = g_netSlots[order[i]];
        s.near = i < g_netCars || (i < g_netCars + g_netKeep && (s.any || s.busy));   // a car made stays further out
    }
    for (NetSlot& s : g_netSlots) {   // a car no longer among the nearest, or not shown: its damage off after a while
        if (s.near || !(s.any || s.busy)) {
            s.farSince = 0;
            continue;
        }
        if (!s.farSince) s.farSince = now;
        if (now - s.farSince < kNetFarMs) continue;
        NetDrop(s);
        s.farSince = 0;
    }
    // The batch with the fewest dents left goes first (a crash's few before a whole car's), then the nearest.
    g_netBest = -1;
    uint32_t bestLeft = 0xFFFFFFFF;
    float bestDist = 0;
    for (int k = 0; k < kNetSlots; k++) {
        const NetSlot& s = g_netSlots[k];
        if (!s.busy || !s.ready || Frame() - s.seen > 2) continue;
        const uint32_t left = s.end > s.next ? s.end - s.next : 0;
        if (left < bestLeft || (left == bestLeft && s.dist < bestDist)) g_netBest = k, bestLeft = left, bestDist = s.dist;
    }
    static double lastLow = 0;
    if (now - lastLow < 1000.0) return;
    lastLow = now;
    const uint32_t room = dmg_va_free();
    if (room >= kNetVaLow) return;
    int far = -1;
    for (int k = 0; k < kNetSlots; k++) {
        const NetSlot& s = g_netSlots[k];
        if ((s.any || s.busy) && (far < 0 || s.dist > g_netSlots[far].dist)) far = k;
    }
    if (far < 0) return;
    NetSlot& s = g_netSlots[far];
    Log("free roam: friend's car %08X: its damage taken off (%u MB of address space free)", s.car, room >> 20);
    NetDrop(s);
    s.failUntil = now + kNetFailMs;
}

// The frame's budget for friends' dents: whether another step of about `est` ms fits (the frame's first always does).
bool NetMore(double t0, double est) {
    if (g_netSteps++ == 0) return true;
    return g_netSpent + (NowMs() - t0) + est <= kNetMsPerFrame;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// From runtime/damage.c

extern "C" int dmg_init(void) {
    const char* e = getenv("NFSU2_DAMAGE_LOG");
    g_settings.logHits = e && *e && *e != '0';
    g_out = dmg_alloc(16);
    // Without the guest heap's window (phones, tablets, Safari: guest_span.h) the game has 640 MB in all: fewer
    // friends' cars with damage, and fewer refinements kept.
    const bool big = dmg_big_memory() != 0;
    g_netCars = big ? 8 : 4;
    g_netKeep = big ? 4 : 2;
    g_tessCacheBytes = big ? 64u << 20 : 24u << 20;
    Log("crash damage on (Crashing Foreign Cars by FRS Mods, the visual damage)%s; friends' cars with damage: %d (+%d), "
        "refinements kept: %u MB", g_settings.logHits ? ", every impact logged" : "", g_netCars, g_netKeep,
        (unsigned)(g_tessCacheBytes >> 20));
    return g_out != 0;
}

// CarRenderInfo::Render, before the game's
extern "C" void dmg_render_entry(uint32_t cri, uint32_t args) {
    void* renderInfo = G(cri);
    if (!dmg_committed(cri, kCriModels + 63 * 8 * 4)) return;
    OnFrame();
    BendWheels(renderInfo, args);
    // A racer (it owns a CarDamage) gets its parts refined ahead of its first impact; a friend's car (free roam) when it
    // is to show the damage its record has.
    if (uint8_t* car = At<uint8_t*>(renderInfo, kCriCar))
        if (CarHas(car, renderInfo) && (IsRemote(A(car)) ? NetWants(A(car)) : At<void*>(car, kCarDamage) != nullptr))
            Prewarm(renderInfo);
    g_drawCar = renderInfo;
    g_bodyKnown = false;
    g_drawCountUsed = 0;
    MarkerDraws& md = g_markerDraws[renderInfo];   // learnt again by this render's draws
    md.draws.clear();
    md.frame = Frame();
}

// CarCollisionBody::ApplyImpulse, after the game's
extern "C" void dmg_impulse(uint32_t bodyAddr, uint32_t impulseAddr, uint32_t pointAddr) {
    WorldWatch();
    uint8_t* body = G(bodyAddr);
    if (!dmg_committed(bodyAddr, kBodyCar + 4) || !dmg_committed(impulseAddr, 12) || !dmg_committed(pointAddr, 12))
        return;
    const float* impulse = (const float*)G(impulseAddr);
    const float* point = (const float*)G(pointAddr);
    for (int i = 0; i < 3; i++)   // (a NaN point would break every lamp and bend the wheels' matrices into NaNs)
        if (!std::isfinite(impulse[i]) || !std::isfinite(point[i])) return;
    uint8_t* car = At<uint8_t*>(body, kBodyCar);
    if (!car || !dmg_committed(A(car), kCarDamage + 4) || !At<void*>(car, kCarDamage)) return;   // racers only
    if (IsRemote(A(car))) return;   // a friend's car (free roam): its player's game makes its damage
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
    if (!std::isfinite(magnitude) || !std::isfinite(dent.depth) || !std::isfinite(dent.point.x + dent.point.y + dent.point.z) ||
        !std::isfinite(dent.direction.x + dent.direction.y + dent.direction.z))
        return;   // (the car's own body gone wrong: nothing made of it)
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
    state->live = true;   // (the game's impulse on it, now)

    const double t0 = NowMs();
    const size_t clonesBefore = g_clones.size();
    g_refinedAhead = g_refinedNow = 0;
    EnsureClones(renderInfo);
    if (g_clones.size() != clonesBefore)   // the car's first impact (or its first since it was rebuilt)
        Log("car %08X cloned: %d parts in %.1f ms (%d refined ahead, %d at the impact)", A(car),
            (int)(g_clones.size() - clonesBefore), NowMs() - t0, g_refinedAhead, g_refinedNow);
    int moved = 0;
    if (dents) {
        if (car == PlayerCar()) NetRecord(renderInfo, dent, magnitude);   // friends make the same dent
        moved = DentCar(renderInfo, *state, dent);
        if (g_settings.hang.enabled) DamageParts(renderInfo, *state, dent);
        BreakLights(renderInfo, *state, dent);
        BendNear(car, *state, dent);
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
    WorldWatch();
    uint8_t* car = At<uint8_t*>(G(carDamage), 0);
    if (!car || !dmg_committed(A(car), kCarRenderInfo + 4)) return;
    void* renderInfo = At<void*>(car, kCarRenderInfo);
    if (renderInfo && renderInfo == g_mine.renderInfo) NetReset(renderInfo);   // friends see the repair
    if (renderInfo && g_cars.count(renderInfo)) {
        RepairRenderInfo(renderInfo, car);
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
    CarState& car = *it->second;
    const uint8_t was = car.gameOpen;
    car.gameOpen = 0;
    for (int i = 0; i < kHangCount; i++) {
        const HangPart& h = car.parts[i];
        for (const auto& o : kOpen)
            if (strcmp(o.name, kHangSpecs[i].name) == 0) {
                // the game opens it itself (the show-off, a cover shoot): its animation turns it (Upload leaves ours)
                if (At<int>(G(frame), o.flag)) car.gameOpen |= OpenBit(i);
                if (!h.loose && !h.fallen) continue;
                if (!At<int>(G(frame), o.flag) && o.amount) At<float>(G(frame), o.amount) = 1.0f;   // fully open
                At<int>(G(frame), o.flag) = 1;
            }
    }
    if (car.gameOpen != was)   // its clones as the swing (or the game's animation) has them now: the next frame
        for (auto& kv : g_clones)
            if (kv.second->renderInfo == G(cri)) {
                const int spec = SpecOf(kv.second);
                if (spec >= 0 && ((car.gameOpen ^ was) & (OpenBit(spec) | (kHangSpecs[spec].parent >= 0 ? OpenBit(kHangSpecs[spec].parent) : 0))))
                    kv.second->dirty = true;
            }
}

// showoff.c: the show-off opens no part the damage has lost (it lies on the road, or is gone: the game's animation would
// move what is left of it); part 0 the hood, 1 the trunk lid, 2 the left door, 3 the right door
extern "C" int dmg_part_lost(uint32_t cri, int part) {
    static const int kSpec[4] = {kSpecHood, kSpecTrunk, kSpecLeftDoor, kSpecRightDoor};
    auto it = g_cars.find(G(cri));
    return part >= 0 && part < 4 && it != g_cars.end() && it->second->parts[kSpec[part]].fallen;
}

// Render's draw of a part: Body's matrix, from the draw of the car's base
extern "C" void dmg_body_draw(uint32_t modelAddr, uint32_t l2w) {
    if (!g_drawCar || !modelAddr || !l2w) return;
    uint8_t* model = G(modelAddr);
    if (!g_clones.empty()) {   // the LOD drawn of a part: its clone's uploads (OnFrame)
        auto it = g_clones.find(CloneKey(model, -1));
        if (it != g_clones.end() && it->second->renderInfo == g_drawCar) it->second->drawnAt = Frame();
    }
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
    std::vector<MarkerDrawn>& drawn = g_markerDraws[g_drawCar].draws;
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
    c->drawnAt = Frame();
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

// The main loop, once a frame: the scratches' texture pack (Crashing Foreign Cars' own DAMAGE_SCRATCH1..4, DXT5: the
// game folder's MODPACKS/DAMAGE/SCRATCHES.TPK, read by damage.c), given once to the game's LoadChunks from a block of
// the guest heap, 128-aligned as the pack's textures are; it is permanent (the game keeps pointers into it).  Without
// it the dents come without scratches (MakeClone finds no DAMAGE_SCRATCH texture and draws no overlay).
extern "C" void dmg_main_loop(void) {
    static bool tried = false;
    WorldWatch();
    NetFrame();
    if (tried) return;
    tried = true;
    uint32_t data = 0, size = 0;
    if (!dmg_scratches_read(&data, &size)) {
        Log("scratches' pack: none (MODPACKS/DAMAGE/SCRATCHES.TPK in the game folder, or no memory for it): dents "
            "without scratches");
        return;
    }
    static const char kName[] = "Crash damage scratches";
    const uint32_t name = dmg_alloc(sizeof kName);
    if (!name) return;
    memcpy(G(name), kName, sizeof kName);
    const uint32_t args[3] = {data, size, name};
    dmg_call(kLoadChunks, 3, args);
    Log("scratches' pack loaded (%u bytes)", size);
}

// ---------------------------------------------------------------------------------------------------------
// Free roam (roam_game.c through damage.c)

extern "C" uint32_t dmg_net_rev(void) { return g_mine.rev; }

extern "C" int dmg_net_mine(uint8_t* epoch, uint16_t* first, uint8_t* dents, int max) {
    *epoch = g_mine.pubEpoch;
    const int n = (int)g_mine.pub.size() < max ? (int)g_mine.pub.size() : max;
    const int skip = (int)g_mine.pub.size() - n;   // the newest that fit
    *first = (uint16_t)(g_mine.pubFirst + skip);
    for (int i = 0; i < n; i++) memcpy(dents + i * RC_DMG_BYTES, g_mine.pub[skip + i].b, RC_DMG_BYTES);
    return n;
}

extern "C" void dmg_net_car(int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t* dents) {
    if (k < 0 || k >= kNetSlots) return;
    WorldWatch();
    const double now = NowMs();
    NetBeginFrame(now);
    NetSlot& s = g_netSlots[k];
    struct Turn {   // the slot whose batch was to go first and made nothing: the frame's budget to another
        int k;
        bool used = false;
        ~Turn() {
            if (!used && g_netBest == k) g_netBest = -1;
        }
    } turn{k};
    if (!car || !dmg_committed(car, kCarRenderInfo + 4)) return;   // not shown now: kept as it is (for a while)
    void* renderInfo = At<void*>(G(car), kCarRenderInfo);
    if (!renderInfo || !dmg_committed(A(renderInfo), kCriModels + 63 * 8 * 4)) return;
    s.seen = Frame();
    // another car or another player in the slot: the old one's damage off it (a parked car is reused)
    if (car != s.car || peer != s.peer || renderInfo != s.renderInfo) {
        NetDrop(s);
        s = NetSlot{};
        s.car = car, s.peer = peer, s.renderInfo = renderInfo, s.epoch = epoch, s.seen = Frame();
    }
    n = n < 0 ? 0 : n > RC_DMG_MAX ? RC_DMG_MAX : n;
    s.dents = n;
    if (!s.busy) NetBox(G(car), s.boxLo, s.boxHi);   // (the box a batch began with stays for it)
    uint8_t* rigid = world::RigidOf(G(car));
    uint8_t* mine = world::RigidOf(PlayerCar());
    s.dist = rigid && mine && dmg_committed(A(rigid) + world::kRigidPosition, 12) &&
                     dmg_committed(A(mine) + world::kRigidPosition, 12)
                 ? Length(At<Vec3>(rigid, world::kRigidPosition) - At<Vec3>(mine, world::kRigidPosition))
                 : 0.0f;
    if (now < s.failUntil) return;
    const auto wait = [&] {   // a change sooner than a crash's next record: made when it may be, as it is then
        if (s.waitedEpoch != epoch) g_netStats.waited++;
        s.waitedEpoch = epoch;
        if (!s.noteWait)
            Log("free roam: friend's car %08X: a change of its damage waits (made at most every %.1f s, a repair every "
                "%.0f s)", car, kNetEveryMs / 1000.0, kNetRepairMs / 1000.0);
        s.noteWait = true;
    };
    if (epoch != s.epoch) {   // the friend's car was repaired: the car as it is till the repair may be taken
        if (now - s.repairAt < kNetRepairMs) {
            if (s.busy) s.busy = false, s.any = s.any || s.next > s.from || HasClones(renderInfo), g_netStats.stopped++;
            wait();
            return;
        }
        s.repairAt = now;
        NetDrop(s);
        s.epoch = epoch;
        g_netStats.repairs++;
    }
    // rebuilt or repaired here (no state, no clones): again from the first
    if (s.any && !s.busy && !g_cars.count(renderInfo) && !HasClones(renderInfo)) s.next = 0, s.any = false;
    if (!s.near) {   // not among the nearest: none made (NetBeginFrame takes what it has off after a while, clones too)
        if (s.busy) s.busy = false, s.any = s.any || s.next > s.from || HasClones(renderInfo), g_netStats.stopped++;
        return;
    }
    const uint32_t last = (uint32_t)first + (uint32_t)n;
    if (s.next < first) s.next = first;   // dents that left the record before they were made: gone
    if (!s.busy) {
        if (s.next >= last) return;   // nothing new
        if (now - s.batchAt < kNetEveryMs) return wait();
        int made = 0;   // the dents a crash makes (none: nothing to clone the car for)
        for (uint32_t i = s.next; i < last; i++) {
            Dent d;
            float m;
            made += NetDecode(dents + (i - first) * RC_DMG_BYTES, d, m, s.boxLo, s.boxHi);
        }
        if (!made) {
            g_netStats.dropped += (int)(last - s.next);
            if (!s.noteBad)
                Log("free roam: friend's car %08X: dents %u..%u left out (epoch %u): no crash makes them", car, s.next,
                    last, epoch);
            s.noteBad = true;
            s.next = last;
            return;
        }
        if (!s.any && !g_cars.count(renderInfo)) {   // a car to clone: g_netCars + g_netKeep at most
            int have = 0, refining = 0;
            for (const NetSlot& o : g_netSlots) {
                have += &o != &s && (o.any || o.busy);
                refining += &o != &s && o.busy && !o.ready;
            }
            if (have >= g_netCars + g_netKeep) return;   // till the farthest one's damage goes (kNetFarMs)
            if (refining >= kNetPrewarmCars) {   // its turn at the refinement ahead: when one of these is ready
                g_netStats.prewarmWaits++;
                return;
            }
        }
        if (!s.any && !g_cars.count(renderInfo) && dmg_va_free() < kNetVaReserve) {   // the game's own memory first
            if (!s.noteRoom)
                Log("free roam: friend's car %08X: shown undamaged (%u MB of address space free)", car, dmg_va_free() >> 20);
            s.noteRoom = true;
            return;
        }
        s.busy = true, s.ready = s.any, s.from = s.next, s.end = last, s.batchAt = now, s.waitAt = now;
        s.frames = 0, s.dropped = 0, s.work = 0, s.cloneAt = 0;
    }
    if (s.end > last) s.end = last;
    if (!s.ready) {   // its parts refined ahead first (a few ms a frame), so that its clones are quick to make
        if (!PrewarmDone(renderInfo) && now - s.waitAt < kNetPrewarmMs) return;
        s.ready = true;
    }
    if ((g_netBest >= 0 && g_netBest != k) || (g_netSteps && g_netSpent >= kNetMsPerFrame)) return;   // another's turn
    g_netBest = k;
    turn.used = true;
    const double t0 = NowMs();
    const int fails = g_cloneFails;
    const bool cloned = CloneSome(renderInfo, s.cloneAt, [&] { return NetMore(t0, 0.2); });
    if (HasClones(renderInfo)) s.any = true;   // (its clones count against the limits from the first: NetBeginFrame)
    while (cloned && s.next < s.end && g_cloneFails == fails) {
        Dent d;
        float magnitude;
        if (!NetDecode(dents + (s.next - first) * RC_DMG_BYTES, d, magnitude, s.boxLo, s.boxHi)) {
            s.next++, s.dropped++, g_netStats.dropped++;
            continue;
        }
        if (!NetMore(t0, s.dentMs)) break;
        const double t = NowMs();
        ApplyNetDent(renderInfo, G(car), d, magnitude);
        s.next++;
        g_netStats.dents++;
        s.dentMs = s.dentMs * 0.75 + (NowMs() - t) * 0.25;
    }
    const double spent = NowMs() - t0;
    g_netSpent += spent;
    g_netStats.ms += spent;
    s.work += spent;
    s.frames++;
    if (g_cloneFails != fails) {
        Log("free roam: friend's car %08X: no memory for its clones: undamaged for %.0f s", car, kNetFailMs / 1000.0);
        NetDrop(s);
        s.failUntil = now + kNetFailMs;
        g_netBest = -1;
        return;
    }
    if (s.next < s.end) return;
    Log("free roam: friend's car %08X: dents %u..%u made (epoch %u) in %.1f ms over %d frames%s", car, s.from, s.end,
        epoch, s.work, s.frames, s.dropped ? ", some no crash makes left out" : "");
    s.busy = false;
    s.any = true;
    g_netStats.batches++;
    g_netBest = -1;   // the frame's rest to another
}
