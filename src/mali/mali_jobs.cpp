// mali_jobs.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

namespace mali {

// the tiler's hierarchy mask for a W x H target of 16x16 tiles (Mesa's pan_select_tiler_hierarchy_mask, no budget)
uint32_t
hierarchyMask(unsigned w, unsigned h, unsigned levels)
{
    static const uint32_t spread[8] = { 0, 0x80, 0x82, 0xa2, 0xaa, 0xea, 0xee, 0xfe };
    uint32_t mask = levels >= 8 ? (1u << (levels > 13 ? 13 : levels)) - 1 : spread[levels];
    unsigned most = w > h ? w : h, last = 0;
    for (unsigned b = (most + 15) / 16; b != 0; b >>= 1) {
        last++;
    }
    unsigned max_levels = levels < 8 ? 8 : levels;
    if (last > max_levels) {
        mask <<= last - max_levels;
    }
    return mask & 0x1fff;
}

// a texture descriptor and its plane for raw memory: 2D, one level, linear (the depth and stencil planes as the
// pre-frame shader reads them: R32F or R16 UNORM for depth, R8UI (131; 163 is R8 UNORM) for stencil)
void
describeRaw(uint32_t mali_format, unsigned bytes, uint64_t va, uint32_t stride, unsigned w, unsigned h, uint32_t *tex,
    uint32_t *plane, uint64_t plane_va, unsigned samples)
{
    memset(tex, 0, 32);
    memset(plane, 0, 32);
    tex[0] = 2 | 2u << 4 | mali_format << 10;
    tex[1] = (w - 1) | (h - 1) << 16;
    tex[2] = kIdentitySwizzle;
    tex[3] = (samples == 4 ? 2u : 0u) << 13;                                    // (the planes a sample apart)
    tex[4] = (uint32_t)plane_va;
    tex[5] = (uint32_t)(plane_va >> 32);
    plane[0] = 10 | 1 << 4 | 2 << 8 | (bytes == 1 ? 0u : bytes == 2 ? 1u : 2u) << 24;
    // (11-20) the plane's size: every sample's slice, not the first's -- samples 1..3 of a reloaded four-sample depth were
    // read past it, as zeroes: after a split pass most of Maps' terrain failed its depth test (a quarter of it shown)
    plane[1] = stride * h * (samples > 1 ? samples : 1);
    plane[2] = (uint32_t)va;
    plane[3] = (uint32_t)(va >> 32);
    plane[4] = stride;
    plane[6] = stride * h;
    plane[7] = (w - 1) | (h - 1) << 16;
}

// the depth and/or stencil (ZS bits 1, 2) of DJ's pass as textures FIRST.. of a resource table's texture table at
// TEX/TEX_VA (32 bytes each), their planes at PLANES/PLANES_VA
void
describeZs(const DrawJob &dj, unsigned zs, uint8_t *tex, uint8_t *planes, uint64_t planes_va)
{
    unsigned i = 0;
    if (zs & 1) {
        describeRaw(dj.z_format == 1 ? 164u << 12 | 16 : 167u << 12 | 16, dj.z_format == 1 ? 2 : 4, dj.z_va, dj.z_row,
            dj.width, dj.height, (uint32_t *)(tex + 32 * i), (uint32_t *)(planes + 32 * i), planes_va + 32 * i, dj.samples);
        i++;
    }
    if (zs & 2) {
        describeRaw(131u << 12 | 16, 1, dj.s_va, dj.s_row, dj.width, dj.height, (uint32_t *)(tex + 32 * i),
            (uint32_t *)(planes + 32 * i), planes_va + 32 * i, dj.samples);
    }
}

// a depth/stencil descriptor that writes what the pre-frame shader outputs (panvk's for its Z/S preload): depth
// always, written, from the shader; stencil always, replaced by the shader's
void
zsdFromShader(uint32_t *w, unsigned zs)
{
    memset(w, 0, 32);
    const uint32_t w4 = 1u << 21 | 1u << 22 | 1u << 23;
    w[0] = 7 | 7u << 4 | 7u << 16;
    w[4] = w4 | 2u << 25 | 7u << 29;
    if (zs & 1) {
        w[4] = w4 | 3u << 25 | 1u << 27 | 7u << 29;
    }
    if (zs & 2) {
        w[0] = 7 | 7u << 4 | 1u << 7 | 1u << 10 | 1u << 13 | 7u << 16 | 1u << 19 | 1u << 22 | 1u << 25 | 1u << 30 | 1u << 31;
        w[1] = 0xffffffffu;
    }
}

// the pass's own pre-frame DCD that loads its depth/stencil (dj.zs_frag_spd), at IR + IR_ZS_DCD
void
emitZsLoad(hakios_metal_device *d, const DrawJob &dj, uint8_t *ir, uint64_t ir_va)
{
    const unsigned zs = (dj.has_z && dj.z_load ? 1u : 0u) | (dj.has_s && dj.s_load ? 2u : 0u);
    const unsigned n = (zs & 1) + (zs >> 1);
    uint8_t *t = ir + IR_ZS_SRT;
    uint64_t t_va = ir_va + IR_ZS_SRT, tex_va = t_va + 0x80, samp_va = tex_va + 32 * n, plane_va = samp_va + 32;
    memset(t, 0, IR_ZSD - IR_ZS_SRT);
    auto entry = [&](unsigned table, uint64_t va, unsigned count) {
        auto *e = (uint32_t *)(t + 16 * table);
        e[0] = (uint32_t)va;
        e[1] = (uint32_t)(va >> 32) | 1u << 24;
        e[2] = count * 32;
    };
    entry(MALI_TABLE_TEXTURE, tex_va, n);
    entry(MALI_TABLE_SAMPLER, samp_va, 1);
    describeZs(dj, zs, ir + (tex_va - ir_va), ir + (plane_va - ir_va), plane_va);
    ((uint32_t *)(ir + (samp_va - ir_va)))[0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
    zsdFromShader((uint32_t *)(ir + IR_ZSD), zs);
    memcpy(ir + IR_ZS_FAU, dj.zs_fs_fau.data(), dj.zs_fs_fau.size() * 4);
    auto *w = (uint32_t *)(ir + IR_ZS_DCD);
    memset(w, 0, 0x180);
    w[0] = 1u << 1 | 3u << 2 | 3u << 4 | 1u << 9;                             // killed, Z/S late, clean tiles written
    if (dj.samples > 1) w[0] |= 1u << 11 | 1u << 19;                           // (multisample: run a sample)
    w[1] = 0xffff;                                                              // no colour target
    w[5] = 1u << 24 | 1u << 25;
    w[7] = 0x3f800000;
    *(uint64_t *)&w[10] = ir_va + IR_ZSD;
    w[17] = (uint32_t)((dj.zs_fs_fau.size() + 1) / 2);
    *(uint64_t *)&w[24] = t_va | MALI_TABLES;
    *(uint64_t *)&w[26] = dj.zs_frag_spd;
    *(uint64_t *)&w[28] = d->descriptors.va + 0x1200;
    *(uint64_t *)&w[30] = ir_va + IR_ZS_FAU;
}

void
emitIr(hakios_metal_device *d, const DrawJob &dj, uint8_t *pass_area, uint64_t pass_va, uint8_t *ir, uint64_t ir_va,
    uint64_t rt_at)
{
    const unsigned nt = dj.rts;
    const uint64_t fbd_bytes = rt_at + 0x40 * nt - 0x80, tags = 1 | (dj.zs ? 2u : 0u) | (nt - 1) << 2;
    auto missing = [&](unsigned i) { return i > 0 && dj.more[i - 1].missing; };
    // the pre-frame shader's resources: each target as a texture (the preload's table), a sampler, a blend descriptor
    // a target: a replace, nothing written to an output with no attachment
    // (with depth/stencil: the depth attachment the next texture, the stencil plane the one after)
    const unsigned zs = (dj.has_z ? 1u : 0u) | (dj.has_s ? 2u : 0u), nzs = (zs & 1) + (zs >> 1), ntex = nt + nzs;
    uint8_t *t = ir + IR_SRT;
    uint64_t t_va = ir_va + IR_SRT, tex_va = t_va + 0x80, samp_va = tex_va + 32 * ntex, plane_va = samp_va + 32;
    uint64_t bl_at = (IR_SRT + 0x80 + (2 * ntex + 1) * 32 + 0x3f) & ~0x3fULL, fau_at = bl_at + 0x40;
    memset(t, 0, IR_FBD - IR_SRT);
    auto entry = [&](unsigned table, uint64_t va, unsigned count) {
        auto *e = (uint32_t *)(t + 16 * table);
        e[0] = (uint32_t)va;
        e[1] = (uint32_t)(va >> 32) | 1u << 24;
        e[2] = count * 32;
    };
    entry(MALI_TABLE_TEXTURE, tex_va, ntex);
    entry(MALI_TABLE_SAMPLER, samp_va, 1);
    describeZs(dj, zs, ir + (tex_va - ir_va) + 32 * nt, ir + (plane_va - ir_va) + 32 * nt, plane_va + 32 * nt);
    unsigned loaded = 0;
    for (unsigned i = 0; i < nt; i++) {
        uint32_t plane[8];
        describe(i == 0 ? dj.ir_target : dj.more[i - 1].texture, (uint32_t *)(ir + (tex_va - ir_va) + 32 * i), plane,
                 plane_va + 32 * i);
        memcpy(ir + (plane_va - ir_va) + 32 * i, plane, 32);
        bool loads = !missing(i);
        auto *b = (uint32_t *)(ir + bl_at) + 4 * i;
        b[0] = 1u << 9 | (loads ? 0u : 1u);
        b[1] = 0x132 | 0x132 << 12 | (loads ? 0xfu : 0u) << 28;
        b[2] = (loads ? 1u : 2u) | 3 << 3 | i << 16;
        b[3] = dj.blend[3];
        loaded |= loads ? 1u << i : 0;
    }
    ((uint32_t *)(ir + (samp_va - ir_va)))[0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
    memcpy(ir + fau_at, dj.ir_fs_fau.data(), dj.ir_fs_fau.size() * 4);
    // the pre-frame draw descriptor (panvk's cmd_emit_dcd for a colour preload): Z/S forced early, may kill and be
    // killed, clean tiles written; every target; no depth/stencil test (the descriptor that lets all through)
    auto *w = (uint32_t *)(ir + IR_DCD);
    memset(w, 0, 0x180);
    w[0] = 1u << 0 | 1u << 1 | 1u << 9;
    if (zs != 0) {
        // (depth/stencil written by the shader: Z/S late, and it may not kill -- panvk's frame shaders)
        w[0] = 1u << 1 | 3u << 2 | 3u << 4 | 1u << 9;
        zsdFromShader((uint32_t *)(ir + IR_IR_ZSD), zs);
    }
    if (dj.samples > 1) w[0] |= 1u << 11 | 1u << 19;                           // (multisample: run a sample)
    w[1] = 0xffff | ((1u << nt) - 1) << 16;
    w[5] = dj.dcd2;
    w[7] = 0x3f800000;
    *(uint64_t *)&w[10] = zs != 0 ? ir_va + IR_IR_ZSD : d->descriptors.va + 0x1980;
    *(uint64_t *)&w[12] = (ir_va + bl_at) | nt;
    w[17] = (uint32_t)((dj.ir_fs_fau.size() + 1) / 2);
    *(uint64_t *)&w[24] = t_va | MALI_TABLES;
    *(uint64_t *)&w[26] = dj.ir_frag_spd;
    *(uint64_t *)&w[28] = d->descriptors.va + 0x1200;
    *(uint64_t *)&w[30] = ir_va + fau_at;
    // the three framebuffer descriptors, copies of the pass's as it now stands (its targets' write-back is the last
    // draw's): 0 the first flush, 1 the later ones, 2 the pass's end after a flush
    uint64_t fbd_va[3];
    for (unsigned k = 0; k < 3; k++) {
        uint8_t *v = ir + IR_FBD + k * IR_FBD_STRIDE;
        auto *f = (uint32_t *)v;
        fbd_va[k] = ir_va + IR_FBD + k * IR_FBD_STRIDE;
        memcpy(v, pass_area + 0x80, fbd_bytes);
        if (k != 2) {
            for (unsigned i = 0; i < nt; i++) {
                if (!missing(i)) ((uint32_t *)(v + rt_at - 0x80))[16 * i + 1] |= 1;
            }
            f[12] |= (dj.has_z ? 1u << 18 : 0) | (dj.has_s ? 1u << 8 : 0);
        }
        if (k != 0) {
            f[0] = (f[0] & ~7u) | 1;                                            // pre-frame 0: always (targets and
            *(uint64_t *)&f[6] = ir_va + IR_DCD;                                // depth/stencil loaded)
        }
    }
    *(uint64_t *)(ir + 0) = fbd_va[0] | tags;
    *(uint64_t *)(ir + 8) = (pass_va + 0x80) | tags;
    // the handler: registers r62-r79 are free between draws; r40-r47 (the fragment job's, also the next draw's) kept
    // and given back. Its own waits name their scoreboards (0: loads/stores and flushes; its fragment job on 4): the
    // IDVS job that ran out (slot 3) is still in flight
    auto *h = (uint64_t *)(ir + IR_HANDLER);
    unsigned k = 0;
    memset(h, 0, IR_HANDLER_BYTES);                                             // (NOPs after it)
    h[k++] = move48(76, ir_va);
    h[k++] = 21ULL << 56 | 40ULL << 48 | 76ULL << 40 | 0xffULL << 16 | 0x40;   // STORE_MULTIPLE r40-r47 -> [r76 + 0x40]
    h[k++] = 20ULL << 56 | 40ULL << 48 | 76ULL << 40 | 0x3ULL << 16 | 0;       // LOAD_MULTIPLE r40-r41 <- [r76]: this flush's
    h[k++] = move32(42, 0);
    h[k++] = move32(43, (dj.width - 1) | (dj.height - 1) << 16);
    h[k++] = move48(44, 0);
    h[k++] = move32(46, 0);
    h[k++] = wait_on(1);
    h[k++] = set_state(0, 4);                                                   // endpoint work on scoreboard 4
    h[k++] = 7ULL << 56;                                                        // RUN_FRAGMENT
    h[k++] = wait_on(1 << 4);
    h[k++] = set_state(0, 3);
    h[k++] = move48(78, pass_va);
    h[k++] = 20ULL << 56 | 62ULL << 48 | 78ULL << 40 | 0xfULL << 16 | 40;      // LOAD_MULTIPLE r62-r65 <- the completed chunks
    h[k++] = wait_on(1);
    h[k++] = 11ULL << 56 | 62ULL << 40 | 64ULL << 32;                          // FINISH_FRAGMENT, the pass not counted done
    h[k++] = wait_on(1);
    h[k++] = move48(66, 0);
    h[k++] = 21ULL << 56 | 66ULL << 48 | 78ULL << 40 | 0x3ULL << 16 | 0;       // the polygon list,
    h[k++] = 21ULL << 56 | 66ULL << 48 | 78ULL << 40 | 0x3ULL << 16 | 40;      // completed top and bottom: 0
    h[k++] = 21ULL << 56 | 66ULL << 48 | 78ULL << 40 | 0x3ULL << 16 | 48;
    h[k++] = move48(62, fbd_va[1] | tags);
    h[k++] = move48(64, fbd_va[2] | tags);
    h[k++] = 21ULL << 56 | 62ULL << 48 | 76ULL << 40 | 0xfULL << 16 | 0;       // the next flush's, the pass's end
    h[k++] = move32(68, 0);
    h[k++] = 36ULL << 56 | 68ULL << 40 | 2 << 8 | 1 << 4 | 1;                  // FLUSH_CACHE2: what was stored, seen by
    h[k++] = wait_on(1);                                                        // the pre-frame shader's texture reads
    h[k++] = 20ULL << 56 | 40ULL << 48 | 76ULL << 40 | 0xffULL << 16 | 0x40;   // r40-r47 back
    h[k++] = wait_on(1);
}

void
emitDraw(hakios_metal_device *d, const DrawJob &dj, uint8_t *area, uint64_t area_va, uint64_t *s, unsigned &n,
    bool first, bool last, uint8_t *pass_area, uint64_t pass_va, uint8_t *ir,
    uint64_t ir_va)
{
    // ONE MALI PASS FOR A RUN OF DRAWS (the stream loop: the draws of one Metal pass, one after the other into the same
    // attachments): the first opens it -- the tiler context, the framebuffer and its targets, the preload -- each draw
    // then only its own registers and RUN_IDVS into that tiler context, and the last closes it: tiling finished, one
    // fragment job, the targets written back as the last draw stores them. A pass a draw read and wrote the whole
    // target (the preload) for every draw before.
    if (first) {
        pass_area = area;
        pass_va = area_va;
    }
    // the tiler context, the framebuffer descriptor, a render target descriptor for each target (64 bytes each, after
    // the framebuffer's), their blend descriptors (16 bytes each, in 64), then the stages' push constants
    // (with depth or stencil the framebuffer's ZS extension comes first, 64 bytes, and the draw's depth/stencil
    // descriptor after the blend descriptors)
    const uint64_t rt_at = 0x100 + (dj.zs ? 0x40 : 0);
    uint64_t bl_at = rt_at + 0x40 * dj.rts;
    auto *tc = (uint32_t *)area, *fb = (uint32_t *)(area + 0x80), *rt = (uint32_t *)(area + rt_at);
    auto *bl = (uint32_t *)(area + bl_at);
    uint64_t zsd_at = bl_at + 0x40, vs_at = zsd_at + (dj.zs ? 0x40 : 0), fs_at = vs_at + ((dj.vs_fau.size() * 4 + 0x3f) & ~0x3fULL);
    uint64_t srt_at = fs_at + ((dj.fs_fau.size() * 4 + 0x3f) & ~0x3fULL), srt = 0;

    memset(area, 0, vs_at);
    // FIRST PROVOKING VERTEX (11-11): a flat varying is the first vertex's in Metal (and Vulkan), the last's in this
    // GPU's default -- CoreAnimation's quads showed a seam along their diagonals (Control Center's tiles, TODO 8h).
    // The tiler context's bit 18 and the framebuffer's word 11 bit 14 (Mesa's first_provoking_vertex); metal_lastpv:
    // the old way
    static const bool last_pv = bootArg("metal_lastpv");
    tc[2] = hierarchyMask(dj.width, dj.height, d->tiler_levels) | 1u << 17 | (last_pv ? 0u : 1u << 18);  // (sample test off)
    tc[3] = (dj.width - 1) | (dj.height - 1) << 16;
    tc[4] = dj.layers - 1;                                                      // (layer count; layer offset 0)
    *(uint64_t *)&tc[6] = d->heapdesc.va;
    *(uint64_t *)&tc[8] = (d->heapdesc.va + 0x1000) | (0x10000 >> 12);
    // sample positions: one, centred; four: the rotated 4x grid (descriptors + 0x2000)
    *(uint64_t *)&fb[4] = d->descriptors.va + (dj.samples > 1 ? 0x2000 : 0x1800);
    fb[8] = (dj.width - 1) | (dj.height - 1) << 16;
    fb[10] = (dj.width - 1) | (dj.height - 1) << 16;
    // 16x16 tiles, targets, 1 KiB a target a tile; four samples: 4 (log2 2), the rotated 4x grid (2), 8x8 tiles -- the
    // same kilobyte a target a tile (64 pixels of four samples)
    // (the colour buffer's allocation: each target's kilobytes of the tile -- one, or two for 8 bytes a pixel)
    uint32_t tile_kb = rtKilobytes(dj.rt1);
    for (unsigned i = 1; i < dj.rts; i++) tile_kb += rtKilobytes(dj.more[i - 1].rt1);
    fb[11] = (dj.samples > 1 ? 2u | 2u << 3 : 0u) | 2 << 6 | (dj.samples > 1 ? 6u : 8u) << 9 | (last_pv ? 0u : 1u << 14) |
        1u << 15 | (dj.rts - 1) << 19 | tile_kb << 24;
    fb[12] = 1u << 29;                                                          // empty tiles written (the clear)
    *(uint64_t *)&fb[14] = area_va;
    if (dj.zs) {
        auto *ext = (uint32_t *)(area + 0x100);
        fb[12] |= 1u << 21;                                                     // the ZS extension
        ext[0] = 1u << 31;                                                      // clean tiles written
        if (dj.has_z) {
            fb[12] |= dj.z_internal << 16 | (dj.z_write ? 1u << 18 : 0);
            memcpy(&fb[13], &dj.z_clear, 4);
            ext[0] |= dj.z_format | 2u << 4 | (dj.samples > 1 ? 3u << 8 : 0u);  // linear (layered samples)
            *(uint64_t *)&ext[8] = dj.z_va;
            ext[10] = dj.z_row;
            ext[11] = dj.z_row * dj.height;
        }
        if (dj.has_s) {
            fb[12] |= (dj.s_clear & 0xff) | (dj.s_write ? 1u << 8 : 0);
            ext[0] |= 1u << 16 | 2u << 20 | (dj.samples > 1 ? 3u << 24 : 0u);   // S8, linear (layered samples)
            *(uint64_t *)&ext[12] = dj.s_va;
            ext[14] = dj.s_row;
            ext[15] = dj.s_row * dj.height;
        }
        memcpy(area + zsd_at, dj.zsd, 32);
        if (first && dj.zs_frag_spd != 0 && ir != nullptr && dj.layers <= 1) {      // (a layered pass is not loaded)
            // DEPTH/STENCIL LOADED by the pre-frame shader (the descriptor's Z/S preload bits, 19 and 9, load nothing on
            // the Mali: what a pass after another found was its clear values)
            emitZsLoad(d, dj, ir, ir_va);
            fb[0] = (fb[0] & ~7u) | 1;                                          // pre-frame 0: always
            *(uint64_t *)&fb[6] = ir_va + IR_ZS_DCD;
        }
    }
    rt[0] = rtInternal(dj.rt1) << 26;                                          // internal format (R8G8B8A8, or a float one)
    rt[1] = (dj.rt1 & ~kRtOurs) | (dj.write ? 1u : 0u) | 1u << 31 | (dj.samples > 1 ? 3u << 12 : 0u);   // clean tiles written too
    // (multisample: written back layered, a plane a sample, rt[11] apart)
    *(uint64_t *)&rt[8] = dj.target_va;
    rt[10] = dj.row_bytes;
    rt[11] = dj.row_bytes * dj.height;
    rt[12] = rt[14] = dj.clear;
    rt[13] = rt[15] = (dj.rt1 & kRtWide) ? dj.clear_high : dj.clear;
    memcpy(bl, dj.blend, 16);
    // (11-13) the blend descriptor's sRGB bit (10) for an sRGB target: the fixed-function blender decodes what it reads
    // of the tile buffer and encodes what it writes -- without it a blended or masked draw wrote linear values there
    // (opaque mode converts by the memory format alone: rtBlend)
    if (dj.rt1 & 1u << 14) bl[0] |= 1u << 10;
    uint32_t tile_at = rtKilobytes(dj.rt1) * 1024;      // where the next target's part of the tile buffer starts
    for (unsigned i = 1; i < dj.rts; i++) {
        // target i: its kilobyte of the tile buffer (internal buffer offset, 16-byte units), its own clear and memory
        const DrawJob::More &m = dj.more[i - 1];
        auto *r = rt + 16 * i;
        r[0] = rtInternal(m.rt1) << 26 | (tile_at / 16) << 4;
        tile_at += rtKilobytes(m.rt1) * 1024;
        r[1] = (m.rt1 & ~kRtOurs) | (m.write && !m.elide ? 1u : 0u) | 1u << 31 | (dj.samples > 1 ? 3u << 12 : 0u);
        *(uint64_t *)&r[8] = m.va;
        r[10] = m.row_bytes;
        r[11] = m.row_bytes * dj.height;
        r[12] = r[14] = m.clear;
        r[13] = r[15] = (m.rt1 & kRtWide) ? m.clear_high : m.clear;
        memcpy(bl + 4 * i, m.blend, 16);
        if (m.rt1 & 1u << 14) bl[4 * i] |= 1u << 10;
    }
    memcpy(area + vs_at, dj.vs_fau.data(), dj.vs_fau.size() * 4);
    memcpy(area + fs_at, dj.fs_fau.data(), dj.fs_fau.size() * 4);
    if (dj.texture_count || dj.sampler_count) {
        // the fragment stage's resource table, as a dispatch's (texture table 4, sampler 3, image 5)
        uint8_t *t = area + srt_at;
        uint64_t t_va = area_va + srt_at, tex_va = t_va + 0x80, samp_va = tex_va + dj.texture_count * 32;
        uint64_t img_va = samp_va + dj.sampler_count * 32, plane_va = img_va + dj.texture_count * 32;
        memset(t, 0, 0x80);
        auto entry = [&](unsigned table, uint64_t va, unsigned count) {
            auto *e = (uint32_t *)(t + 16 * table);
            e[0] = (uint32_t)va;
            e[1] = (uint32_t)(va >> 32) | 1u << 24;
            e[2] = count * 32;
        };
        entry(MALI_TABLE_TEXTURE, tex_va, dj.texture_count);
        entry(MALI_TABLE_SAMPLER, samp_va, dj.sampler_count);
        entry(MALI_TABLE_IMAGE, img_va, dj.texture_count);
        for (unsigned i = 0; i < dj.texture_count; i++) {
            auto *tex = (uint32_t *)(area + (tex_va - area_va) + 32 * i), *img = (uint32_t *)(area + (img_va - area_va) + 32 * i);
            uint32_t plane[8];
            if (dj.textures[i] == nullptr) {
                memset(tex, 0, 32);
                memset(img, 0, 32);
                continue;
            }
            describe(dj.textures[i], tex, plane, plane_va);
            memcpy(area + (plane_va - area_va), plane, 32);
            memcpy(img, tex, 32);
            plane_va += 32;
        }
        for (unsigned i = 0; i < dj.sampler_count; i++) {
            memcpy(area + (samp_va - area_va) + 32 * i, dj.samplers[i], 32);
        }
        srt = t_va | MALI_TABLES;
    }
    // the vertex stage's resource table, after the fragment's (64-byte aligned): the same three tables
    const uint64_t fs_srt_bytes = dj.texture_count || dj.sampler_count ? 0x80 + (dj.texture_count * 3 + dj.sampler_count) * 32 : 0;
    const uint64_t vsrt_at = (srt_at + fs_srt_bytes + 0x3f) & ~0x3fULL;
    const uint64_t vs_srt_bytes = dj.vs_texture_count || dj.vs_sampler_count ? 0x80 + (dj.vs_texture_count * 3 + dj.vs_sampler_count) * 32 : 0;
    uint64_t vsrt = 0;
    if (vs_srt_bytes != 0) {
        uint8_t *t = area + vsrt_at;
        uint64_t t_va = area_va + vsrt_at, tex_va = t_va + 0x80, samp_va = tex_va + dj.vs_texture_count * 32;
        uint64_t img_va = samp_va + dj.vs_sampler_count * 32, plane_va = img_va + dj.vs_texture_count * 32;
        memset(t, 0, 0x80);
        auto entry = [&](unsigned table, uint64_t va, unsigned count) {
            auto *e = (uint32_t *)(t + 16 * table);
            e[0] = (uint32_t)va;
            e[1] = (uint32_t)(va >> 32) | 1u << 24;
            e[2] = count * 32;
        };
        entry(MALI_TABLE_TEXTURE, tex_va, dj.vs_texture_count);
        entry(MALI_TABLE_SAMPLER, samp_va, dj.vs_sampler_count);
        entry(MALI_TABLE_IMAGE, img_va, dj.vs_texture_count);
        for (unsigned i = 0; i < dj.vs_texture_count; i++) {
            auto *tex = (uint32_t *)(area + (tex_va - area_va) + 32 * i), *img = (uint32_t *)(area + (img_va - area_va) + 32 * i);
            uint32_t plane[8];
            if (dj.vs_textures[i] == nullptr) {
                memset(tex, 0, 32);
                memset(img, 0, 32);
                continue;
            }
            describe(dj.vs_textures[i], tex, plane, plane_va);
            memcpy(area + (plane_va - area_va), plane, 32);
            memcpy(img, tex, 32);
            plane_va += 32;
        }
        for (unsigned i = 0; i < dj.vs_sampler_count; i++) {
            memcpy(area + (samp_va - area_va) + 32 * i, dj.vs_samplers[i], 32);
        }
        vsrt = t_va | MALI_TABLES;
    }
    uint64_t vs_fau_va = area_va + vs_at, fs_fau_va = area_va + fs_at;
    // A PASS OPENED (11-10): the texture caches dropped first (and the load/store caches cleaned) -- what an earlier pass
    // of this command buffer wrote back is in L2, but a texture cache may still hold that memory as it was before:
    // CoreAnimation's blur samples the passes before it (the tester's slow-motion clip: frames partly unblurred and
    // streaked mid-swipe). Passes were only ordered (WAIT_ALL). This Mali (v11) does not invalidate them at RUN_FRAGMENT
    // (v13+ does, implicitly; Arm's own driver forces it before every fragment job -- Mesa's panvk, IMPLICIT_OTHERS_INV).
    // A pass that loads its targets has the same flush before its preload, below. metal_passinv=0: not done (a test).
    static const bool pass_inv = [] {
        char args[1024] = {0};
        size_t size = sizeof(args) - 1;
        return !(sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && strstr(args, "metal_passinv=0") != nullptr);
    }();
    // (with every target that asked for a load elided -- passElide -- there is nothing to preload)
    bool preload = dj.load && dj.layers <= 1;       // (a layered pass is not loaded: refused where the draw is recorded)
    bool lazy_pass = false;                         // (the load is a pre-frame shader's: no draw of ours started the tiler)
    if (preload && !dj.load0) {
        preload = false;
        for (unsigned i = 1; i < dj.rts; i++) {
            preload = preload || (dj.more[i - 1].load && !dj.more[i - 1].missing && !dj.more[i - 1].elide);
        }
    }
    if (first && pass_inv && !preload && !dj.clear_only) {
        s[n++] = move32(74, 0);
        s[n++] = 36ULL << 56 | 74ULL << 40 | 2 << 8 | 1 << 4;                  // FLUSH_CACHE2: LSC clean, others invalidated
        s[n++] = WAIT_ALL;
    }
    if (first && ir != nullptr) {
        *(uint64_t *)(ir + 0x10) = 0;
        if (!dj.clear_only && dj.ir_frag_spd != 0 && dj.layers <= 1) {             // (a layered pass: its area holds
            *(uint64_t *)(ir + 0x10) = 1;                                       // the layers' framebuffers instead)
            s[n++] = move48(76, ir_va + IR_HANDLER);
            s[n++] = move32(78, IR_HANDLER_BYTES);
            s[n++] = 25ULL << 56 | 76ULL << 40 | 78ULL << 32 | 2;              // SET_EXCEPTION_HANDLER: tiler OOM
        }
    }
    if (preload && first) {
        // THE LOAD ACTION: the target's own pixels drawn first, one triangle over it whose fragment is the target's
        // texel (texture 0, txf), into the same tiler context as the draw -- the tile buffer preloaded through the
        // ordinary draw path. Caches first, as at a command buffer's start: what an earlier pass wrote back reaches
        // L2, and the texture cache drops what an earlier preload read of the same target.
        uint64_t end_at = vs_srt_bytes != 0 ? vsrt_at + vs_srt_bytes : srt_at + fs_srt_bytes;
        uint64_t pvs_at = (end_at + 0x3f) & ~0x3fULL, pfs_at = pvs_at + ((dj.pre_vs_fau.size() * 4 + 0x3f) & ~0x3fULL);
        uint64_t psrt_at = pfs_at + ((dj.pre_fs_fau.size() * 4 + 0x3f) & ~0x3fULL);
        const unsigned nt = dj.rts;
        memcpy(area + pvs_at, dj.pre_vs_fau.data(), dj.pre_vs_fau.size() * 4);
        memcpy(area + pfs_at, dj.pre_fs_fau.data(), dj.pre_fs_fau.size() * 4);
        uint8_t *t = area + psrt_at;
        uint64_t t_va = area_va + psrt_at, tex_va = t_va + 0x80, samp_va = tex_va + 32 * nt, plane_va = samp_va + 32;
        memset(t, 0, 0x80 + (2 * nt + 1) * 32 + 0x40 + 16 * nt);
        // the preload's own blend descriptors: a replace for a target that loads, nothing written to one that does not
        // (the draw's may blend, the preload must not)
        uint64_t pbl_at = (psrt_at + 0x80 + (2 * nt + 1) * 32 + 0x3f) & ~0x3fULL;
        auto *pbl = (uint32_t *)(area + pbl_at);
        for (unsigned i = 0; i < nt; i++) {
            bool loads = i == 0 ? dj.load0 : (dj.more[i - 1].load && !dj.more[i - 1].missing && !dj.more[i - 1].elide);
            auto *b = pbl + 4 * i;
            b[0] = 1u << 9 | (loads ? 0u : 1u);
            b[1] = 0x132 | 0x132 << 12 | (loads ? 0xfu : 0u) << 28;
            b[2] = (loads ? 1u : 2u) | 3 << 3 | i << 16;
            b[3] = dj.blend[3];
        }
        auto entry = [&](unsigned table, uint64_t va, unsigned count) {
            auto *e = (uint32_t *)(t + 16 * table);
            e[0] = (uint32_t)va;
            e[1] = (uint32_t)(va >> 32) | 1u << 24;
            e[2] = count * 32;
        };
        entry(MALI_TABLE_TEXTURE, tex_va, nt);
        entry(MALI_TABLE_SAMPLER, samp_va, 1);
        for (unsigned i = 0; i < nt; i++) {
            uint32_t plane[8];
            describe(i == 0 ? dj.pre_target : dj.more[i - 1].texture, (uint32_t *)(area + (tex_va - area_va) + 32 * i), plane,
                     plane_va + 32 * i);
            memcpy(area + (plane_va - area_va) + 32 * i, plane, 32);
        }
        ((uint32_t *)(area + (samp_va - area_va)))[0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
        uint64_t pvs_va = area_va + pvs_at, pfs_va = area_va + pfs_at;
        s[n++] = move32(74, 0);
        s[n++] = 36ULL << 56 | 74ULL << 40 | 2 << 8 | 1 << 4 | 1;              // FLUSH_CACHE2: L2/LSC clean, others invalidated
        s[n++] = WAIT_ALL;
        // (11-19) THE LOAD AS A PRE-FRAME SHADER, ONLY WHERE THE PASS DRAWS (metal_lazyload=0: the triangle, as before): the same fragment
        // shader and tables in a pre-frame draw descriptor of mode Intersect -- a tile no draw touches is neither read
        // nor written back (the render targets' clean-pixel writes and the framebuffer's empty-tile writes off), its
        // memory is the load. What Mesa's panvk does for a colour preload. The triangle through the tiler loads and
        // writes back the whole target, 10 MB each way for a screen: CoreAnimation draws the screen in three passes a
        // frame while a list scrolls (two of them a navigation bar's worth), ~2 ms each -- 75 frames a second of 120.
        // Not with depth/stencil, samples or layers; not when a target that is stored starts from a clear.
        static const bool lazy_on = !bootArg("metal_lazyload=0");
        bool lazy = lazy_on && !dj.zs && dj.samples <= 1 && dj.load0;
        for (unsigned i = 1; i < nt && lazy; i++) {
            const DrawJob::More &m = dj.more[i - 1];
            lazy = !(m.write && !m.elide && !m.missing) || m.load;
        }
        lazy_pass = lazy;
        if (lazy) {
            const uint64_t pdcd_at = (pbl_at + 16 * nt + 0x3f) & ~0x3fULL;
            auto *w = (uint32_t *)(area + pdcd_at);
            memset(w, 0, 0x180);
            w[0] = 1u << 0 | 1u << 1 | 1u << 9;                                 // kills, is killed, clean fragments written
            w[1] = 0xffff | ((1u << nt) - 1) << 16;
            w[5] = dj.dcd2;
            w[7] = 0x3f800000;
            *(uint64_t *)&w[10] = d->descriptors.va + 0x1980;
            *(uint64_t *)&w[12] = (area_va + pbl_at) | nt;
            w[17] = (uint32_t)((dj.pre_fs_fau.size() + 1) / 2);
            *(uint64_t *)&w[24] = t_va | MALI_TABLES;
            *(uint64_t *)&w[26] = dj.pre_frag_spd;
            *(uint64_t *)&w[28] = d->descriptors.va + 0x1200;
            *(uint64_t *)&w[30] = pfs_va;
            fb[0] = (fb[0] & ~7u) | 2;                                          // pre-frame 0: where geometry is
            *(uint64_t *)&fb[6] = area_va + pdcd_at;
            fb[12] &= ~(1u << 29);                                              // empty tiles not written
            for (unsigned i = 0; i < nt; i++) rt[16 * i + 1] &= ~(1u << 31);    // nor clean pixels
        } else {
        s[n++] = move48(0, 0);
        s[n++] = move48(4, t_va | MALI_TABLES);
        s[n++] = move48(8, pvs_va);
        s[n++] = move32(9, (uint32_t)(pvs_va >> 32) | (uint32_t)((dj.pre_vs_fau.size() + 1) / 2) << 24);
        s[n++] = move48(12, pfs_va);
        s[n++] = move32(13, (uint32_t)(pfs_va >> 32) | (uint32_t)((dj.pre_fs_fau.size() + 1) / 2) << 24);
        s[n++] = move48(16, dj.pre_pos_spd);
        s[n++] = move48(18, 0);                                                 // no varying shader
        s[n++] = move48(20, dj.pre_frag_spd);
        s[n++] = move48(24, d->descriptors.va + 0x1200);
        s[n++] = move32(32, 0);
        s[n++] = move32(33, 3);                                                 // one triangle
        s[n++] = move32(34, 1);
        s[n++] = move32(35, 0);
        s[n++] = move32(36, 0);
        s[n++] = move32(37, 0);
        s[n++] = move32(38, dj.dcd2);
        s[n++] = move32(39, 0);
        s[n++] = move48(40, area_va);
        s[n++] = move32(42, 0);                                                 // the whole target
        s[n++] = move32(43, (dj.width - 1) | (dj.height - 1) << 16);
        s[n++] = move32(44, 0);
        s[n++] = move32(45, 0x3f800000);
        s[n++] = move48(46, 0);
        s[n++] = move32(48, 0);
        s[n++] = move48(50, (area_va + pbl_at) | nt);                          // the preload's
        s[n++] = move48(52, d->descriptors.va + 0x1980);
        s[n++] = move48(54, 0);
        s[n++] = move32(56, 8 | 1u << 22);                                      // triangles, basic FIFO
        s[n++] = move32(57, 1u << 23 | 1u << 26 | (dj.samples > 1 ? 1u << 11 | 1u << 19 : 0u));   // no face culled
        // (multisample: run a sample, each its own texel)
        s[n++] = move32(58, dj.dcd1);
        s[n++] = move32(60, 0x3f800000);
        s[n++] = move32(61, 0);
        s[n++] = 49ULL << 56 | 0ULL << 32;                                      // HEAP_OPERATION: vertex/tiler started
        s[n++] = 6ULL << 56 | 1ULL << 33 | 1ULL << 38;                          // RUN_IDVS
        }
    }
    s[n++] = move48(0, vsrt);                                                   // the vertex stage's SRT (0: none)
    s[n++] = move48(4, srt);                                                    // fragment SRT
    s[n++] = move48(8, vs_fau_va);
    s[n++] = move32(9, (uint32_t)(vs_fau_va >> 32) | (uint32_t)((dj.vs_fau.size() + 1) / 2) << 24);
    s[n++] = move48(12, fs_fau_va);
    s[n++] = move32(13, (uint32_t)(fs_fau_va >> 32) | (uint32_t)((dj.fs_fau.size() + 1) / 2) << 24);
    s[n++] = move48(16, dj.pos_spd);
    s[n++] = move48(18, dj.vary_spd);
    s[n++] = move48(20, dj.frag_spd);
    if (dj.tls_size != 0) {
        // SPILLED REGISTERS: a shader too big for its registers (CoreAnimation's fixed_frag_lph_cpf) spills to thread
        // local storage, which a draw had none of -- its stores went to addresses near 0 and the GPU faulted (AS1,
        // writes at 0x600000, 0x8000000, ...). The descriptor as a dispatch's: per thread 16 << word 0 bytes, no
        // threadgroup memory (word 1 = 31), the device's spill buffer (sized in drawImpl)
        uint64_t ls_at = (drawBytes(dj) - 0x80 + 0x3f) & ~0x3fULL;
        auto *ls = (uint32_t *)(area + ls_at);
        memset(ls, 0, 32);
        ls[0] = log2u((dj.tls_size + 15) / 16);
        ls[1] = 31;
        ls[2] = (uint32_t)(d->tls[ls[0]].va >> 8);
        ls[3] = (uint32_t)(d->tls[ls[0]].va >> 40) | 1u << 28;
        s[n++] = move48(24, area_va + ls_at);
    } else {
        s[n++] = move48(24, d->descriptors.va + 0x1200);                       // local storage: none
    }
    s[n++] = move32(32, 0);
    s[n++] = move32(33, dj.count);
    s[n++] = move32(34, dj.instances);
    s[n++] = move32(35, 0);
    s[n++] = move32(36, dj.vertex_start);
    s[n++] = move32(37, 0);
    s[n++] = move32(38, dj.dcd2);
    s[n++] = move32(39, (uint32_t)dj.index_size);                               // the index buffer's bytes
    s[n++] = move48(40, pass_va);                                               // the pass's tiler context
    s[n++] = move32(42, dj.scissor_min);
    s[n++] = move32(43, dj.scissor_max);
    s[n++] = move32(44, 0);                                                     // depth clamps 0 .. 1
    s[n++] = move32(45, 0x3f800000);
    s[n++] = move48(46, 0);                                                     // no occlusion query
    s[n++] = move32(48, dj.vary_size);
    s[n++] = move48(50, (area_va + bl_at) | dj.rts);                           // a blend descriptor a target
    s[n++] = move48(52, dj.zs ? area_va + zsd_at : d->descriptors.va + 0x1980);   // depth/stencil (none: all pass)
    s[n++] = move48(54, dj.index_va);                                           // the index buffer (0: none)
    s[n++] = move32(56, dj.tiler_flags);
    s[n++] = move32(57, dj.dcd0);
    s[n++] = move32(58, dj.dcd1);
    s[n++] = move32(60, 0x3f800000);                                            // primitive size 1.0
    s[n++] = move32(61, 0);                                                     // TILER_FLAGS2: VRS 1x1
    if (!dj.clear_only) {                                                       // (a clear: the fragment job alone)
        // (11-20: also when the load is the pre-frame shader's, or nothing is loaded after all -- every target elided, a
        // layered pass: no preload draw came first, and the heap was never told a pass began)
        if (first && (!dj.load || lazy_pass || !preload)) {                     // (else started already by the preload's draw)
            s[n++] = 49ULL << 56 | 0ULL << 32;                                  // HEAP_OPERATION: vertex/tiler started
        }
        s[n++] = 6ULL << 56 | 1ULL << 33 | 1ULL << 38;                          // RUN_IDVS: malloc, the fragment's SRT
        if (!last) {
            return;                                                             // (the pass goes on with the next draw)
        }
        s[n++] = WAIT_ALL;
        s[n++] = 9ULL << 56;                                                    // FINISH_TILING
        s[n++] = WAIT_ALL;
        s[n++] = 49ULL << 56 | 1ULL << 32;                                      // HEAP_OPERATION: vertex/tiler completed
        s[n++] = WAIT_ALL;
    }
    const bool ir_on = ir != nullptr && *(uint64_t *)(ir + 0x10) != 0;
    if (ir_on) {
        s[n++] = move48(76, 0);                                                 // the handler off: tiling is done
        s[n++] = move32(78, 0);
        s[n++] = 25ULL << 56 | 76ULL << 40 | 78ULL << 32 | 2;
    }
    if (!first) {
        // the targets written back as the last draw stores them (the pass's descriptors are the first draw's)
        auto *prt = (uint32_t *)(pass_area + rt_at);
        prt[1] = (prt[1] & ~1u) | (dj.write ? 1u : 0u);
        for (unsigned i = 1; i < dj.rts; i++) {
            prt[16 * i + 1] = (prt[16 * i + 1] & ~1u) | (dj.more[i - 1].write && !dj.more[i - 1].elide ? 1u : 0u);
        }
        if (dj.zs) {
            auto *pfb = (uint32_t *)(pass_area + 0x80);
            pfb[12] = (pfb[12] & ~(1u << 18 | 1u << 8)) | (dj.has_z && dj.z_write ? 1u << 18 : 0) |
                (dj.has_s && dj.s_write ? 1u << 8 : 0);
        }
    }
    if (ir_on) {
        // the framebuffer descriptor: the pass's own, or after a flush its copy that loads what the flushes stored
        emitIr(d, dj, pass_area, pass_va, ir, ir_va, rt_at);
        s[n++] = move48(76, ir_va);
        s[n++] = 20ULL << 56 | 40ULL << 48 | 76ULL << 40 | 0x3ULL << 16 | 8;   // LOAD_MULTIPLE r40-r41 <- [r76 + 8]
        s[n++] = wait_on(1);
    } else {
        s[n++] = move48(40, (pass_va + 0x80) | 1 | (dj.zs ? 2u : 0u) | (dj.rts - 1) << 2);   // the framebuffer descriptor, its targets
    }
    s[n++] = move32(42, 0);
    s[n++] = move32(43, (dj.width - 1) | (dj.height - 1) << 16);
    s[n++] = move48(44, 0);                                                     // no tile enable map (r44 held the
    s[n++] = move32(46, 0);                                                     // depth clamps), its row stride
    if (dj.layers > 1 && ir != nullptr && !ir_on) {
        // LAYERED RENDERING (11-14): the tiler put each primitive into the list of the layer its vertex function named
        // (the tiler context's layer count, the draws' "layer index enable"); now a fragment job a layer, each with a
        // copy of the pass's framebuffer descriptor that names its layer (internal layer index; the frame argument as
        // panvk has it) and whose render target is that layer's memory. The copies are in the pass's incremental-
        // rendering area, which a layered pass does not use (no tiler OOM handler: a pass whose lists outgrow the heap
        // is not flushed).
        for (unsigned layer = 0; layer < dj.layers; layer++) {
            uint8_t *copy = ir + 0x100 + 0x100 * layer;
            uint64_t copy_va = ir_va + 0x100 + 0x100 * layer;
            // (the framebuffer descriptor, its ZS extension if the pass has depth, then the render target)
            const uint64_t ext_bytes = dj.zs ? 0x40 : 0;
            memcpy(copy, pass_area + 0x80, 0x80 + ext_bytes + 0x40 * dj.rts);
            auto *lfb = (uint32_t *)copy, *lext = (uint32_t *)(copy + 0x80), *lrt = (uint32_t *)(copy + 0x80 + ext_bytes);
            lfb[0] = (lfb[0] & 0x00ffffffu) | layer << 24;
            *(uint64_t *)&lfb[2] = layer;
            *(uint64_t *)&lrt[8] += dj.layer_stride * layer;
            if (dj.zs && dj.has_z) {
                *(uint64_t *)&lext[8] += dj.z_layer_stride * layer;            // (that layer of the depth texture)
            }
            s[n++] = move48(40, copy_va | 1 | (dj.zs ? 2u : 0u) | (dj.rts - 1) << 2);
            s[n++] = 7ULL << 56;                                                // RUN_FRAGMENT
            s[n++] = WAIT_ALL;
        }
    } else {
    s[n++] = 7ULL << 56;                                                        // RUN_FRAGMENT
    s[n++] = WAIT_ALL;
    }
    // the pass finished, as panvk ends one: the tiler context's completed top and bottom (byte 40, four words) into
    // r70-r73, and FINISH_FRAGMENT gives that heap memory back and counts the pass done. Without it a queue drew one
    // pass and then binned nothing (the heap's in-flight passes never went down).
    // (A CLEAR -- no tiling, no heap pass started -- does not: counted done, the firmware's count of the heap's passes
    // in flight went one below the truth, and a later pass that ran out of heap had "none in flight": its tiler OOM
    // handler never ran and the queue waited for good)
    if (dj.clear_only) {
        return;
    }
    s[n++] = move48(74, pass_va);
    s[n++] = 20ULL << 56 | 70ULL << 48 | 74ULL << 40 | 0xfULL << 16 | 40;      // LOAD_MULTIPLE r70-73 <- [r74 + 40]
    s[n++] = WAIT_ALL;
    s[n++] = 11ULL << 56 | 70ULL << 40 | 72ULL << 32 | 1;                       // FINISH_FRAGMENT: chunks r70..r72, count it
    s[n++] = WAIT_ALL;
}
                                              // tiler OOM handler's on/off included)

// whether draw B goes on in the Mali pass draw A is in: the same attachments, and B keeps what is in them (Load)
bool
samePass(const DrawJob &a, const DrawJob &b)
{
    if (a.clear_only || b.clear_only || !b.load || !b.load0 || a.target_va != b.target_va || a.rts != b.rts || a.width != b.width ||
        a.layers != b.layers ||
        a.height != b.height || a.row_bytes != b.row_bytes || a.zs != b.zs || a.z_va != b.z_va || a.s_va != b.s_va ||
        (b.zs && ((b.has_z && !b.z_load) || (b.has_s && !b.s_load)))) {
        return false;
    }
    for (unsigned i = 1; i < a.rts; i++) {
        if (a.more[i - 1].va != b.more[i - 1].va || a.more[i - 1].missing != b.more[i - 1].missing ||
            (!b.more[i - 1].missing && !b.more[i - 1].load && !b.more[i - 1].memoryless)) {
            return false;
        }
    }
    // a draw that samples one of the pass's attachments needs what the draws before it left there IN MEMORY: in the
    // same Mali pass that is still in the tile buffer (the home screen's search label went missing so)
    for (unsigned t = 0; t < b.texture_count; t++) {
        const hakios_metal_texture *x = b.textures[t];
        if (x == nullptr) continue;
        uint64_t va = x->gpu(), end = va + x->span();  // (every level of a mipmapped one: a pass into one level)
        if (a.target_va >= va && a.target_va < end) return false;
        for (unsigned i = 1; i < a.rts; i++) {
            if (a.more[i - 1].va >= va && a.more[i - 1].va < end) return false;
        }
    }
    return true;
}

// the bytes a draw needs in its command buffer's stream BO, past the instructions
uint64_t
drawBytes(const DrawJob &dj)
{
    uint64_t bytes = 0x100 + 0x40 * dj.rts + 0x40 + (dj.zs ? 0x80 : 0) + ((dj.vs_fau.size() * 4 + 0x3f) & ~0x3fULL) +
        ((dj.fs_fau.size() * 4 + 0x3f) & ~0x3fULL);
    if (dj.texture_count || dj.sampler_count) {
        bytes += 0x80 + (dj.texture_count * 3 + dj.sampler_count) * 32 + 0x40;
    }
    if (dj.vs_texture_count || dj.vs_sampler_count) {
        bytes += 0x80 + (dj.vs_texture_count * 3 + dj.vs_sampler_count) * 32 + 0x80;
    }
    if (dj.tls_size != 0) {
        bytes += 0x80;                                  // (the local storage descriptor, 64-byte aligned, last)
    }
    if (dj.load) {
        bytes += 0x40 + ((dj.pre_vs_fau.size() * 4 + 0x3f) & ~0x3fULL) + ((dj.pre_fs_fau.size() * 4 + 0x3f) & ~0x3fULL) + 0x80 +
            (2 * dj.rts + 1) * 32 + 0x80 + 16 * dj.rts + 0x1c0;      // (... and the pre-frame DCDs, lazyLoad)
    }
    return bytes;
}
}  // namespace mali
