// gorecore - see gorecore.h
#include "gorecore.h"
#include <algorithm>
#include <deque>
#include <string.h>

namespace gore {

// ------------------------------------------------------------------ voxelize
// Parity fill along one axis: for every row of cell centres parallel to `ax`,
// intersect the row with all triangles and fill between entry/exit pairs.
// Done along all three axes; a cell is solid if >= 2 of 3 agree, which keeps
// small holes in the mesh (eyes, mouth, cuffs) from leaking whole rows.
static void parityAxis(const Body& b, int ax, int nx, int ny, int nz, V3 o, float c,
                       std::vector<uint8_t>& votes) {
    int n[3] = {nx, ny, nz};
    int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
    std::vector<std::vector<float>> rows((size_t)n[a1] * n[a2]);
    auto comp = [](V3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; };
    float oo[3] = {o.x, o.y, o.z};
    for (size_t t = 0; t + 2 < b.idx.size(); t += 3) {
        V3 p0 = b.pos[b.idx[t]], p1 = b.pos[b.idx[t + 1]], p2 = b.pos[b.idx[t + 2]];
        float u0 = comp(p0, a1), u1 = comp(p1, a1), u2 = comp(p2, a1);
        float v0 = comp(p0, a2), v1 = comp(p1, a2), v2 = comp(p2, a2);
        float umin = std::min(u0, std::min(u1, u2)), umax = std::max(u0, std::max(u1, u2));
        float vmin = std::min(v0, std::min(v1, v2)), vmax = std::max(v0, std::max(v1, v2));
        int i0 = std::max(0, (int)floorf((umin - oo[a1]) / c - 0.5f)), i1 = std::min(n[a1] - 1, (int)ceilf((umax - oo[a1]) / c - 0.5f));
        int j0 = std::max(0, (int)floorf((vmin - oo[a2]) / c - 0.5f)), j1 = std::min(n[a2] - 1, (int)ceilf((vmax - oo[a2]) / c - 0.5f));
        float det = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0);
        if (fabsf(det) < 1e-14f) continue;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                // tiny offset avoids hitting edges/vertices exactly
                float u = oo[a1] + (i + 0.5f) * c + 1.3e-6f, v = oo[a2] + (j + 0.5f) * c + 0.7e-6f;
                float w1 = ((u - u0) * (v2 - v0) - (u2 - u0) * (v - v0)) / det;
                float w2 = ((u1 - u0) * (v - v0) - (u - u0) * (v1 - v0)) / det;
                if (w1 < 0 || w2 < 0 || w1 + w2 > 1) continue;
                float x = comp(p0, ax) + w1 * (comp(p1, ax) - comp(p0, ax)) + w2 * (comp(p2, ax) - comp(p0, ax));
                rows[(size_t)j * n[a1] + i].push_back(x);
            }
    }
    for (int j = 0; j < n[a2]; ++j)
        for (int i = 0; i < n[a1]; ++i) {
            auto& r = rows[(size_t)j * n[a1] + i];
            if (r.size() < 2) continue;
            std::sort(r.begin(), r.end());
            for (size_t k = 0; k + 1 < r.size(); k += 2) {
                int s = std::max(0, (int)ceilf((r[k] - oo[ax]) / c - 0.5f));
                int e = std::min(n[ax] - 1, (int)floorf((r[k + 1] - oo[ax]) / c - 0.5f));
                for (int q = s; q <= e; ++q) {
                    int xyz[3]; xyz[ax] = q; xyz[a1] = i; xyz[a2] = j;
                    votes[((size_t)xyz[2] * ny + xyz[1]) * nx + xyz[0]]++;
                }
            }
        }
}

bool VoxelBody::build(const Body& b, float c, int pad) {
    if (b.pos.empty() || b.idx.size() < 3) return false;
    V3 mn = b.pos[0], mx = b.pos[0];
    for (auto& p : b.pos) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    cell = c;
    origin = mn - V3{pad * c, pad * c, pad * c};
    nx = (int)ceilf((mx.x - mn.x) / c) + 2 * pad;
    ny = (int)ceilf((mx.y - mn.y) / c) + 2 * pad;
    nz = (int)ceilf((mx.z - mn.z) / c) + 2 * pad;
    size_t N = (size_t)nx * ny * nz;
    if (N > 40u * 1000 * 1000) return false;           // ~40 MB per byte-array guard
    std::vector<uint8_t> votes(N, 0);
    for (int ax = 0; ax < 3; ++ax) parityAxis(b, ax, nx, ny, nz, origin, c, votes);
    state.assign(N, EMPTY);
    for (size_t i = 0; i < N; ++i) if (votes[i] >= 2) state[i] = SOLID;

    // owning bone: seed from vertices, then flood inward (multi-source BFS)
    boneOf.assign(N, 255);
    std::deque<int> q;
    for (size_t v = 0; v < b.pos.size(); ++v) {
        V3 p = b.pos[v];
        int x = (int)((p.x - origin.x) / c), y = (int)((p.y - origin.y) / c), z = (int)((p.z - origin.z) / c);
        // the vertex sits on the skin: mark the nearest filled cell around it
        for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            int X = x + dx, Y = y + dy, Z = z + dz;
            if (!in(X, Y, Z)) continue;
            int i = id(X, Y, Z);
            if (filled(i) && boneOf[i] == 255) { boneOf[i] = v < b.bone.size() ? b.bone[v] : 0; q.push_back(i); }
        }
    }
    const int D[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    while (!q.empty()) {
        int i = q.front(); q.pop_front();
        int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
        for (auto& d : D) {
            int X = x + d[0], Y = y + d[1], Z = z + d[2];
            if (!in(X, Y, Z)) continue;
            int j = id(X, Y, Z);
            if (filled(j) && boneOf[j] == 255) { boneOf[j] = boneOf[i]; q.push_back(j); }
        }
    }
    computeDepth();
    label_.assign(N, -1);
    return true;
}

void VoxelBody::computeDepth() {
    size_t N = state.size();
    depth.assign(N, 255);
    std::deque<int> q;
    const int D[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int z = 0; z < nz; ++z) for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
        int i = id(x, y, z);
        if (!filled(i)) continue;
        for (auto& d : D) {
            int X = x + d[0], Y = y + d[1], Z = z + d[2];
            if (!in(X, Y, Z) || state[id(X, Y, Z)] == EMPTY) { depth[i] = 0; q.push_back(i); break; }
        }
    }
    while (!q.empty()) {
        int i = q.front(); q.pop_front();
        int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
        for (auto& d : D) {
            int X = x + d[0], Y = y + d[1], Z = z + d[2];
            if (!in(X, Y, Z)) continue;
            int j = id(X, Y, Z);
            if (filled(j) && depth[j] == 255 && depth[i] < 254) { depth[j] = depth[i] + 1; q.push_back(j); }
        }
    }
}

// ------------------------------------------------------------------ bone rods
static float segDist(V3 p, V3 a, V3 b) {
    V3 ab = b - a; float t = dot(p - a, ab) / std::max(1e-12f, dot(ab, ab));
    t = std::max(0.0f, std::min(1.0f, t));
    return len(p - (a + ab * t));
}

void VoxelBody::addBoneRod(V3 a, V3 b, float r, uint8_t boneId) {
    V3 mn{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, std::min(a.z, b.z) - r};
    V3 mx{std::max(a.x, b.x) + r, std::max(a.y, b.y) + r, std::max(a.z, b.z) + r};
    int x0 = std::max(0, (int)((mn.x - origin.x) / cell)), x1 = std::min(nx - 1, (int)((mx.x - origin.x) / cell));
    int y0 = std::max(0, (int)((mn.y - origin.y) / cell)), y1 = std::min(ny - 1, (int)((mx.y - origin.y) / cell));
    int z0 = std::max(0, (int)((mn.z - origin.z) / cell)), z1 = std::min(nz - 1, (int)((mx.z - origin.z) / cell));
    for (int z = z0; z <= z1; ++z) for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        int i = id(x, y, z);
        if (state[i] == SOLID && segDist(centre(x, y, z), a, b) <= r) { state[i] = BONE; boneOf[i] = boneId; }
    }
}

// ------------------------------------------------------------------ carving
Wound VoxelBody::carve(V3 entry, V3 dir, float rIn, float rOut, float energy) {
    Wound w{}; w.entry = entry; w.dir = norm(dir); w.rIn = rIn; w.rOut = rOut;
    const float step = cell * 0.5f, maxL = 1.2f;
    const float boneCost = 6.0f;                    // a metre of bone = 6 m of flesh
    // pass 1: where does the ray enter, how far does its energy carry it,
    // and is there body left beyond that point (=no exit)?
    float t0 = -1, tEnd = -1, spent = 0, lastFilled = -1;
    for (float t = -0.05f; t < maxL; t += step) {
        V3 p = entry + w.dir * t;
        int x = (int)floorf((p.x - origin.x) / cell), y = (int)floorf((p.y - origin.y) / cell), z = (int)floorf((p.z - origin.z) / cell);
        if (!in(x, y, z)) { if (t0 >= 0) break; else continue; }
        uint8_t s = state[id(x, y, z)];
        if (s == SOLID || s == BONE || s == CARVED) {
            if (t0 < 0) t0 = t;
            if (s == SOLID) spent += step;
            if (s == BONE) spent += step * boneCost;
            if (s != CARVED) lastFilled = t;
            if (spent > energy && tEnd < 0) tEnd = t;
        }
    }
    if (t0 < 0) { w.length = 0; return w; }       // missed the body entirely
    w.exited = (tEnd < 0);
    if (tEnd < 0) tEnd = lastFilled + cell;       // through-and-through
    w.length = tEnd - t0;
    // pass 2: carve a tube from the entry to tEnd, radius rIn -> rOut
    for (float t = t0 - cell; t <= tEnd; t += step) {
        float f = w.length > 1e-4f ? std::max(0.0f, std::min(1.0f, (t - t0) / w.length)) : 0;
        float r = rIn + (w.exited ? (rOut - rIn) * f : 0.0f);
        V3 c = entry + w.dir * t;
        int rc = (int)ceilf(r / cell) + 1;
        int cx = (int)floorf((c.x - origin.x) / cell), cy = (int)floorf((c.y - origin.y) / cell), cz = (int)floorf((c.z - origin.z) / cell);
        for (int z = cz - rc; z <= cz + rc; ++z) for (int y = cy - rc; y <= cy + rc; ++y) for (int x = cx - rc; x <= cx + rc; ++x) {
            if (!in(x, y, z)) continue;
            int i = id(x, y, z);
            if (!filled(i)) continue;
            V3 p = centre(x, y, z);
            V3 d = p - c; float along = dot(d, w.dir);
            if (len(d - w.dir * along) > r || fabsf(along) > step) continue;
            if (state[i] == BONE) w.boneCellsHit++;
            state[i] = CARVED; w.cellsRemoved++;
        }
    }
    return w;
}

void VoxelBody::removeComponent(int lab) {
    for (size_t i = 0; i < state.size(); ++i) if (label_[i] == lab) state[i] = EMPTY;
}

// ------------------------------------------------------------------ pieces
std::vector<Piece> VoxelBody::pieces(int crumbCells) {
    size_t N = state.size();
    label_.assign(N, -1);
    std::vector<Piece> out;
    std::vector<int> stack;
    const int D[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (size_t s = 0; s < N; ++s) {
        if (!filled((int)s) || label_[s] >= 0) continue;
        int lab = (int)out.size();
        Piece pc{}; pc.bmin = {1e9f, 1e9f, 1e9f}; pc.bmax = {-1e9f, -1e9f, -1e9f};
        unsigned hist[256] = {0};
        double sx = 0, sy = 0, sz = 0;
        stack.push_back((int)s); label_[s] = lab;
        while (!stack.empty()) {
            int i = stack.back(); stack.pop_back();
            int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
            V3 p = centre(x, y, z);
            sx += p.x; sy += p.y; sz += p.z; pc.cells++;
            pc.bmin = {std::min(pc.bmin.x, p.x), std::min(pc.bmin.y, p.y), std::min(pc.bmin.z, p.z)};
            pc.bmax = {std::max(pc.bmax.x, p.x), std::max(pc.bmax.y, p.y), std::max(pc.bmax.z, p.z)};
            hist[boneOf[i]]++;
            for (auto& d : D) {
                int X = x + d[0], Y = y + d[1], Z = z + d[2];
                if (!in(X, Y, Z)) continue;
                int j = id(X, Y, Z);
                if (filled(j) && label_[j] < 0) { label_[j] = lab; stack.push_back(j); }
            }
        }
        pc.centroid = {(float)(sx / pc.cells), (float)(sy / pc.cells), (float)(sz / pc.cells)};
        pc.mainBone = (uint8_t)(std::max_element(hist, hist + 255) - hist);
        out.push_back(pc);
    }
    // drop crumbs, renumber labels so they index the returned vector
    std::vector<int> remap(out.size(), -1);
    std::vector<Piece> kept;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].cells >= crumbCells) { remap[i] = (int)kept.size(); kept.push_back(out[i]); }
    for (size_t i = 0; i < N; ++i) {
        if (label_[i] < 0) continue;
        int r = remap[label_[i]];
        if (r < 0) state[i] = CARVED;
        label_[i] = r;
    }
    int big = -1, bc = -1;
    for (size_t i = 0; i < kept.size(); ++i) if (kept[i].cells > bc) { bc = kept[i].cells; big = (int)i; }
    if (big >= 0) kept[big].isMain = true;
    return kept;
}

// ------------------------------------------------------------------ surface nets
// Dual contouring on the binary field: one vertex per 2x2x2 corner block that
// straddles the surface, at the mean of its crossing-edge midpoints, followed
// by light smoothing. A quad per pair of face-adjacent cells that differ.
Mesh VoxelBody::remesh(bool onlyWounds) const {
    Mesh m;
    const int bx = nx - 1, by = ny - 1, bz = nz - 1;
    // each block's vertex, -1 none yet. Kept between calls (all -1 again when one ends, by the
    // list of those it set) instead of a table the size of the grid made and filled each time.
    static std::vector<int> vid;
    static std::vector<size_t> vidUsed;
    if (vid.size() < (size_t)bx * by * bz) vid.assign((size_t)bx * by * bz, -1);
    auto vidx = [&](int x, int y, int z) { return ((size_t)z * by + y) * bx + x; };
    // onlyWounds: a face is only wanted where one of its two cells is CARVED, so a row of
    // cells with none in it, nor in the row above or behind it, has nothing to give (that is
    // nearly every row of a body with a few bullet holes in it), and the others only between
    // the first and the last of them. rowFirst, rowLast: those, -1 for a row with none.
    std::vector<short> rowFirst, rowLast;
    if (onlyWounds) {
        rowFirst.assign((size_t)ny * nz, -1);
        rowLast.assign((size_t)ny * nz, -1);
        for (int z = 0; z < nz; ++z) for (int y = 0; y < ny; ++y) {
            const uint8_t *row = &state[id(0, y, z)];
            const uint8_t *first = (const uint8_t *)memchr(row, CARVED, nx);
            if (!first) continue;
            int last = nx - 1;
            while (row[last] != CARVED) --last;
            rowFirst[(size_t)z * ny + y] = (short)(first - row);
            rowLast[(size_t)z * ny + y] = (short)last;
        }
    }

    auto vertexFor = [&](int x, int y, int z) -> int {   // block with low corner cell (x,y,z)
        if (x < 0 || y < 0 || z < 0 || x >= bx || y >= by || z >= bz) return -1;
        int& v = vid[vidx(x, y, z)];
        if (v >= 0) return v;
        V3 acc{0, 0, 0}; int n = 0; int fx = -1, fy = 0, fz = 0; int best = 1 << 30; bool touchesWound = false;
        // (a block's eight cells are all in the grid: looked up once each)
        bool fill[8];
        const int base = id(x, y, z);
        const int off[8] = { 0, 1, nx, nx + 1, nx * ny, nx * ny + 1, nx * ny + nx, nx * ny + nx + 1 };
        int numFilled = 0;
        for (int c = 0; c < 8; ++c) {
            const uint8_t s = state[base + off[c]];
            fill[c] = s == SOLID || s == BONE;
            numFilled += fill[c];
            if (s == CARVED) touchesWound = true;
        }
        if (numFilled == 0 || numFilled == 8) return -1; // (no side of it crosses the surface)
        for (int c = 0; c < 8; ++c) {
            int ax = x + (c & 1), ay = y + ((c >> 1) & 1), az = z + ((c >> 2) & 1);
            if (fill[c]) {                              // shallowest filled cell sets the colour
                int d = state[base + off[c]] == BONE ? 1000 : depth[base + off[c]];
                if (d < best) { best = d; fx = ax; fy = ay; fz = az; }
            }
            for (int k = 0; k < 3; ++k) {                // edges in +x/+y/+z from this corner, inside the block
                if (c & (1 << k)) continue;
                const int c2 = c | (1 << k);
                if (fill[c] != fill[c2]) { acc = acc + (centre(ax, ay, az) + centre(ax + (k == 0), ay + (k == 1), az + (k == 2))) * 0.5f; ++n; }
            }
        }
        if (n == 0) return -1;
        v = (int)m.pos.size();
        vidUsed.push_back(vidx(x, y, z));
        m.pos.push_back(acc * (1.0f / n));
        m.nrm.push_back({0, 0, 0});
        if (fx < 0) { fx = x; fy = y; fz = z; }
        int fi = id(fx, fy, fz);
        m.bone.push_back(boneOf[fi]);
        m.depthCm.push_back(state[fi] == BONE ? 255 : (uint8_t)std::min(254.0f, depth[fi] * cell * 100.0f));
        m.wound.push_back(touchesWound ? 1 : 0);
        return v;
    };

    for (int z = 0; z < nz; ++z) for (int y = 0; y < ny; ++y) {
      int xFrom = 0, xTo = nx - 1;
      if (onlyWounds) {
          // the cells of this row with a carved one for a neighbour in +x, +y or +z, or carved themselves
          const size_t r = (size_t)z * ny + y;
          xFrom = nx; xTo = -1;
          if (rowFirst[r] >= 0) { xFrom = std::max(0, rowFirst[r] - 1); xTo = rowLast[r]; }
          if (y + 1 < ny && rowFirst[r + 1] >= 0) { xFrom = std::min(xFrom, (int)rowFirst[r + 1]); xTo = std::max(xTo, (int)rowLast[r + 1]); }
          if (z + 1 < nz && rowFirst[r + ny] >= 0) { xFrom = std::min(xFrom, (int)rowFirst[r + ny]); xTo = std::max(xTo, (int)rowLast[r + ny]); }
      }
      for (int x = xFrom; x <= xTo; ++x) {
        int a = id(x, y, z);
        for (int k = 0; k < 3; ++k) {
            int X = x + (k == 0), Y = y + (k == 1), Z = z + (k == 2);
            if (!in(X, Y, Z)) continue;
            int b = id(X, Y, Z);
            bool fa = filled(a), fb = filled(b);
            if (fa == fb) continue;
            if (onlyWounds) {                           // only walls facing a wound cavity
                uint8_t empt = fa ? state[b] : state[a];
                if (empt != CARVED) continue;
            }
            // the 4 blocks sharing this face's edge ring
            int q[4];
            if (k == 0) { q[0] = vertexFor(x, y - 1, z - 1); q[1] = vertexFor(x, y, z - 1); q[2] = vertexFor(x, y, z); q[3] = vertexFor(x, y - 1, z); }
            if (k == 1) { q[0] = vertexFor(x - 1, y, z - 1); q[1] = vertexFor(x - 1, y, z); q[2] = vertexFor(x, y, z); q[3] = vertexFor(x, y, z - 1); }
            if (k == 2) { q[0] = vertexFor(x - 1, y - 1, z); q[1] = vertexFor(x, y - 1, z); q[2] = vertexFor(x, y, z); q[3] = vertexFor(x - 1, y, z); }
            if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
            if (!fa) std::swap(q[1], q[3]);              // face outward from the filled side
            m.idx.insert(m.idx.end(), {(uint32_t)q[0], (uint32_t)q[1], (uint32_t)q[2], (uint32_t)q[0], (uint32_t)q[2], (uint32_t)q[3]});
        }
      }
    }
    for (size_t i : vidUsed) vid[i] = -1;
    vidUsed.clear();
    // smoothing (2 passes, umbrella operator, keeps the shape inside the cell)
    std::vector<V3> sum(m.pos.size()); std::vector<int> cnt(m.pos.size());
    for (int it = 0; it < 2; ++it) {
        std::fill(sum.begin(), sum.end(), V3{0, 0, 0}); std::fill(cnt.begin(), cnt.end(), 0);
        for (size_t t = 0; t < m.idx.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                uint32_t a = m.idx[t + e], b = m.idx[t + (e + 1) % 3];
                sum[a] = sum[a] + m.pos[b]; cnt[a]++; sum[b] = sum[b] + m.pos[a]; cnt[b]++;
            }
        for (size_t i = 0; i < m.pos.size(); ++i)
            if (cnt[i]) m.pos[i] = m.pos[i] * 0.5f + sum[i] * (0.5f / cnt[i]);
    }
    for (size_t t = 0; t < m.idx.size(); t += 3) {
        V3 n = cross(m.pos[m.idx[t + 1]] - m.pos[m.idx[t]], m.pos[m.idx[t + 2]] - m.pos[m.idx[t]]);
        for (int e = 0; e < 3; ++e) m.nrm[m.idx[t + e]] = m.nrm[m.idx[t + e]] + n;
    }
    for (auto& n : m.nrm) n = norm(n);
    return m;
}

} // namespace gore
