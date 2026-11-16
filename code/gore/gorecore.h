// gorecore - the geometry core of GF Gore. No game, no D3D: pure CPU code so it
// can be built and tested anywhere (Linux test harness, MSVC x86 in the ASI).
//
//   Body       a character's bind-pose skin mesh + per-vertex bone index
//   VoxelBody  that mesh as a solid voxel grid in bind space, with per-cell
//              depth under the skin and owning bone (the "carve field")
//   carve      a bullet: tube along the shot, entry + optional exit, holes bone
//   remesh     wound surface (naive surface nets) for a range of blocks
//   pieces     connected components after carving -> severed limbs / chunks
//
// Design notes: see docs/GF_GORE_DESIGN.md §4. Units are metres.
#pragma once
#include <stdint.h>
#include <vector>
#include <math.h>

namespace gore {

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return sqrtf(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 0}; }

struct Body {                       // input: bind-pose skin
    std::vector<V3>      pos;
    std::vector<uint8_t> bone;      // dominant bone per vertex (0..254)
    std::vector<uint32_t> idx;      // triangles
};

// Cell states. EMPTY outside the body, SOLID flesh, BONE inside a bone,
// CARVED removed by a wound (kept distinct from EMPTY so the wound surface
// and the skin mask know where wounds are).
enum : uint8_t { EMPTY = 0, SOLID = 1, BONE = 2, CARVED = 3 };

struct Wound {
    V3 entry, dir; float rIn, rOut, length; bool exited;
    int cellsRemoved, boneCellsHit;
};

struct Piece {                      // a connected component of the remaining body
    int cells; V3 centroid; V3 bmin, bmax;
    uint8_t mainBone;               // most common bone in the piece
    bool isMain;                    // the biggest component (stays on the body)
};

struct Mesh {
    std::vector<V3> pos, nrm;
    std::vector<uint8_t> bone;      // owning bone (for skinning the layer)
    std::vector<uint8_t> depthCm;   // depth under the skin; 255 = bone
    std::vector<uint8_t> wound;     // 1 = vertex lies on a wound wall (flesh), 0 = skin
    std::vector<uint32_t> idx;
};

class VoxelBody {
public:
    // cell = edge length (m). pad = empty border cells.
    bool build(const Body& b, float cell, int pad = 2);

    // bone rods: segments the skeleton occupies (radius in m). Cells within a
    // rod become BONE. Call after build(); used for "bones holed" and exits.
    void addBoneRod(V3 a, V3 b, float radius, uint8_t boneId);

    // A shot. 'energy' ~ how far it can travel through flesh (m); each bone cell
    // costs extra. Returns what happened. Cells are CARVED along a tube whose
    // radius grows from rIn at entry to rOut at exit.
    Wound carve(V3 entry, V3 dir, float rIn, float rOut, float energy);

    // Remove an explicit set of cells (e.g. a severed piece) - marks them EMPTY.
    void removeComponent(int label);

    // Connected components of SOLID|BONE cells (6-connected). Components
    // smaller than crumbCells are "crumbs": turned to CARVED and not returned
    // (MP2VR does the same - 'crumb (gone)').
    std::vector<Piece> pieces(int crumbCells = 40);
    const std::vector<int>& labels() const { return label_; }

    // Surface of the remaining body (skin + wound walls) via naive surface nets.
    // onlyWounds: emit only faces that border a CARVED cell (the flesh layer).
    Mesh remesh(bool onlyWounds) const;

    // grid access
    int nx = 0, ny = 0, nz = 0; float cell = 0; V3 origin{0, 0, 0};
    std::vector<uint8_t> state, depth, boneOf;   // depth in cells under the skin
    inline int id(int x, int y, int z) const { return (z * ny + y) * nx + x; }
    inline bool in(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz; }
    inline V3 centre(int x, int y, int z) const { return origin + V3{(x + 0.5f) * cell, (y + 0.5f) * cell, (z + 0.5f) * cell}; }
    inline bool filled(int i) const { return state[i] == SOLID || state[i] == BONE; }
private:
    std::vector<int> label_;
    void computeDepth();
};

} // namespace gore
