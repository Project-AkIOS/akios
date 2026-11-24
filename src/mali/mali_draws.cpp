// mali_draws.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

extern "C" {
extern "C++" { namespace mali {
// the stage's push constants for a draw: [[buffer(n)]]'s addresses, the viewport transform, the first vertex/instance
/*
 * A SURFACE THIS COMMAND BUFFER DRAWS INTO (11-10), marked busy with HakiosGPU (0x7d) from its first draw into it until
 * the command buffer is done or dropped (letGo): the display waits for the marks to go before it shows the surface
 * (MiniIOSurfaceDrawn, hakios.akita_gpu.cpp), as Apple's waits on a surface's own GPU writes. CoreAnimation draws some
 * frames with several command buffers and presents from the first one's scheduled handler, before the others are even
 * recorded: waiting only for what had been submitted, the panel showed such frames half drawn.
 */
void
markBusy(hakios_metal_cb *cb, hakios_metal_device *d, hakios_metal_texture *t)
{
    while (t != nullptr && t->origin == 4) {
        t = t->parent;
    }
    if (t == nullptr || t->origin != 1 || t->surface == 0) {
        return;
    }
    if (t->mirrored && std::find(cb->mirror_out.begin(), cb->mirror_out.end(), t->mirror_key) == cb->mirror_out.end()) {
        cb->mirror_out.push_back(t->mirror_key);
    }
    for (unsigned sid : cb->busy) {
        if (sid == t->surface) {
            return;
        }
    }
    struct { uint32_t surface; int32_t delta; } r = { t->surface, 1 };
    {
        std::lock_guard<std::mutex> g(d->lock);
        if (d->call(0x7d, &r, sizeof(r)) != 0) {
            return;
        }
    }
    cb->busy.push_back(t->surface);
    cb->busy_device = d;
}

bool
stagePushConstants(const mali_kernel_binary &b, hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count,
    const float scale[3], const float offset[3], unsigned first_vertex, unsigned base_instance, std::vector<uint32_t> &fau,
    std::vector<hakios_metal_buffer *> &used)
{
    fau.assign(b.fau_words, 0);
    for (unsigned r = 0; r < b.num_resources; r++) {
        const mali_kernel_resource &res = b.resources[r];
        if (res.kind != HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER && res.kind != HAKIOS_METAL_RESOURCE_DEVICE_BUFFER) {
            continue;
        }
        if (res.index >= count || buffers[res.index] == nullptr) {
            // (nothing bound: the device's zeroes stand for it -- gNothingVa, set by the draw before this)
            if (gNothingVa.load() == 0) {
                fail("draw with %s: nothing bound at [[buffer(%u)]]", b.name, res.index);
                return false;
            }
            fau[2 * res.slot] = (uint32_t)gNothingVa.load();
            fau[2 * res.slot + 1] = (uint32_t)(gNothingVa.load() >> 32);
            continue;
        }
        uint64_t va = buffers[res.index]->bo.va + (offsets ? offsets[res.index] : 0);
        fau[2 * res.slot] = (uint32_t)va;               // (its slot in the push constants: the index, under 16)
        fau[2 * res.slot + 1] = (uint32_t)(va >> 32);
        used.push_back(buffers[res.index]);
    }
    for (int i = 0; i < 3; i++) {
        memcpy(&fau[MALI_DRAW_VIEWPORT_SCALE + i], &scale[i], 4);
        memcpy(&fau[MALI_DRAW_VIEWPORT_OFFSET + i], &offset[i], 4);
    }
    fau[MALI_DRAW_FIRST_VERTEX] = first_vertex;
    fau[MALI_DRAW_BASE_INSTANCE] = base_instance;
    if (b.fau_words > MALI_KERNEL_RESERVED) {
        memcpy(&fau[MALI_KERNEL_RESERVED], b.consts, (b.fau_words - MALI_KERNEL_RESERVED) * 4);
    }
    return true;
}

uint8_t
unorm8(double v)
{
    return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0 + 0.5);
}

// A function's constexpr sampler (AIR's __air_sampler_state word; upstream tools/mesa/air's air_decode_sampler_state:
// address s/t/r bits 0/3/6, mag 9, min 11, mip 13, coord::pixel 15, compare 16, anisotropy 20, lod 24/40, border 56)
// as the API's samplers are made: AIR's clamp_to_zero 0 / clamp_to_edge 1 / repeat 2 / mirrored_repeat 3 /
// clamp_to_border 4 are Metal's 4 / 0 / 2 / 3 / 5. A comparison (compare_func other than none 0 and never 8) is refused.
bool
inlineSampler(uint64_t w, uint32_t words[8])
{
    static const unsigned address[5] = { 4, 0, 2, 3, 5 };
    auto field = [&](unsigned lo, unsigned n) { return (unsigned)((w >> lo) & ((1ull << n) - 1)); };
    hakios_metal_sampler_desc desc = {};
    for (unsigned a = 0; a < 3; a++) {
        if (field(3 * a, 3) > 4) {
            fail("an address mode AIR does not have (%u)", field(3 * a, 3));
            return false;
        }
    }
    unsigned compare = field(16, 4);
    if (compare != 0 && compare != 8) {
        fail("a comparison sampler (compare_func %u, MSL's numbering): not on the Mali yet", compare);
        return false;
    }
    desc.s_address = address[field(0, 3)];
    desc.t_address = address[field(3, 3)];
    desc.r_address = address[field(6, 3)];
    desc.mag_filter = field(9, 2) ? 1 : 0;                  // (bicubic as linear)
    desc.min_filter = field(11, 2) ? 1 : 0;
    desc.mip_filter = field(13, 2);
    desc.normalized_coordinates = field(15, 1) == 0;
    desc.max_anisotropy = field(20, 4) + 1;
    desc.border_color = field(56, 2);
    desc.lod_min_clamp = 0;                                  // (MSL's lod_clamp default: every level)
    desc.lod_max_clamp = 1e30f;
    hakios_metal_sampler *s = hakios_metal_sampler_new(nullptr, &desc);
    if (s == nullptr) {
        return false;
    }
    memcpy(words, s->words, 32);
    delete s;
    return true;
}

// THE PASS'S DEPTH AND STENCIL into DJ (its attachments, actions, clear values; with DRAW, the draw's depth/stencil
// state as a "Depth/stencil" descriptor), the attachments the size of WIDTH x HEIGHT; false with WHY
bool
zsOf(const struct hakios_metal_render_pass *pass, DrawJob &dj, unsigned width, unsigned height, bool draw, const char *&why)
{
    hakios_metal_texture *z = pass->depth, *st = pass->stencil;
    if (z == nullptr && st == nullptr) {
        if (draw && (pass->depth_state_set || pass->stencil_state_set)) {
            // (a depth/stencil state with no attachment: Metal tests against nothing, every fragment passes)
        }
        return true;
    }
    if (z != nullptr && z->format != 252 && z->format != 250 && z->format != 260) {
        why = "a depth attachment not Depth16Unorm, Depth32Float or Depth32Float_Stencil8 (not on the Mali yet)";
        return false;
    }
    if (st != nullptr && st->format != 253 && st->format != 260) {
        why = "a stencil attachment not Stencil8 or Depth32Float_Stencil8 (not on the Mali yet)";
        return false;
    }
    if ((z && z->samples != dj.samples) || (st && st->samples != dj.samples)) {
        why = "depth or stencil attachments of another sample count than the colour attachments";
        return false;
    }
    if ((z && (z->width != width || z->height != height)) || (st && (st->width != width || st->height != height))) {
        why = "depth or stencil attachments of another size than the colour attachments";
        return false;
    }
    dj.zs = true;
    {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            fprintf(stderr, "hakios-metal: the first pass with depth/stencil: depth MTLPixelFormat %u, stencil %u, %ux%u, "
                "depth state %d (compare %u write %d), stencil state %d\n", z ? z->format : 0, st ? st->format : 0, width,
                height, pass->depth_state_set, pass->depth_compare, pass->depth_write, pass->stencil_state_set);
        }
    }
    if (z != nullptr) {
        dj.has_z = true;
        dj.z_va = z->gpu();
        dj.z_row = z->stride;
        dj.z_format = z->format == 250 ? 1 : 14;
        dj.z_internal = z->format == 250 ? 0 : 2;
        dj.z_write = pass->depth_store_action == 1;
        dj.z_load = pass->depth_load_action == 1;
        dj.z_clear = (float)pass->clear_depth;
    }
    if (st != nullptr) {
        dj.has_s = true;
        dj.s_va = st->format == 260 ? st->gpu() + st->stencil_off : st->gpu();
        dj.s_row = st->format == 260 ? st->stencil_stride : st->stride;
        dj.s_write = pass->stencil_store_action == 1;
        dj.s_load = pass->stencil_load_action == 1;
        dj.s_clear = pass->clear_stencil & 0xff;
    }
    auto *w = dj.zsd;
    memset(w, 0, 32);
    w[0] = 7 | 7u << 4 | 7u << 16;                     // stencil faces: always (no test)
    // (word 4: v11's separated dependency tracking 21, depth cull 22, clamp mode [0, 1] (1) at 23, depth from fixed
    // function (2) at 25 -- the two genxml defaults that are not zero, without which the depth test did nothing)
    const uint32_t w4 = 1u << 21 | 1u << 22 | 1u << 23 | 2u << 25;
    w[4] = w4 | 7u << 29;                               // depth: always, nothing written
    if (!draw) {
        return true;
    }
    if (dj.has_z && pass->depth_state_set) {
        // MTLCompareFunction is genxml's "Func" (Never 0 .. Always 7)
        w[4] = w4 | (pass->depth_write ? 1u << 27 : 0) | (pass->depth_compare & 7) << 29;
    }
    if (dj.has_s && pass->stencil_state_set) {
        // MTLStencilOperation (Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap,
        // DecrementWrap) as genxml's "Stencil Op"
        static const uint32_t op[8] = { 0, 2, 1, 6, 7, 3, 4, 5 };
        const auto &f = pass->stencil_front, &b = pass->stencil_back;
        w[0] = 7 | (f.compare & 7) << 4 | op[f.stencil_fail & 7] << 7 | op[f.depth_fail & 7] << 10 | op[f.pass & 7] << 13 |
            (b.compare & 7) << 16 | op[b.stencil_fail & 7] << 19 | op[b.depth_fail & 7] << 22 | op[b.pass & 7] << 25 |
            1u << 31;
        w[1] = (f.write_mask & 0xff) | (b.write_mask & 0xff) << 8 | (f.read_mask & 0xff) << 16 | (b.read_mask & 0xff) << 24;
        w[2] = (pass->stencil_front_reference & 0xff) | (pass->stencil_back_reference & 0xff) << 8;
    }
    return true;
}

// A PASS INTO A MIP LEVEL OR SLICE: the pass as one whose attachments are those levels as textures of their own
// (attachmentOf), held in VIEWS until the caller releases them; false (and nothing held) with the reason
bool
passAtLevels(const struct hakios_metal_render_pass *pass, struct hakios_metal_render_pass &q, hakios_metal_texture *views[10])
{
    q = *pass;
    for (unsigned i = 0; i < 10; i++) views[i] = nullptr;
    auto release = [&] { for (unsigned i = 0; i < 10; i++) if (views[i]) { views[i]->release(); views[i] = nullptr; } };
    if (q.color0 != nullptr && (q.color0_level || q.color0_slice)) {
        if ((views[0] = attachmentOf(q.color0, q.color0_level, q.color0_slice)) == nullptr) return false;
        q.color0 = views[0];
        q.color0_level = q.color0_slice = 0;
    }
    for (unsigned i = 0; i < 7; i++) {
        auto &m = q.more[i];
        if (m.texture != nullptr && (m.level || m.slice)) {
            if ((views[i + 1] = attachmentOf(m.texture, m.level, m.slice)) == nullptr) { release(); return false; }
            m.texture = views[i + 1];
            m.level = m.slice = 0;
        }
    }
    if (q.depth != nullptr && (q.depth_level || q.depth_slice)) {
        // the depth attachment's level (with its own stencil plane when it is Depth32Float_Stencil8 and the stencil
        // attachment too: Metal's stencil descriptor names the same level)
        if ((views[8] = attachmentOf(q.depth, q.depth_level, q.depth_slice)) == nullptr) { release(); return false; }
        if (q.stencil == q.depth) q.stencil = views[8];
        q.depth = views[8];
        q.depth_level = q.depth_slice = 0;
    }
    return true;
}

bool
atLevels(const struct hakios_metal_render_pass *pass)
{
    if (pass == nullptr) return false;
    if (pass->color0 != nullptr && (pass->color0_level || pass->color0_slice)) return true;
    if (pass->depth != nullptr && (pass->depth_level || pass->depth_slice)) return true;
    for (unsigned i = 0; i < 7; i++) {
        if (pass->more[i].texture != nullptr && (pass->more[i].level || pass->more[i].slice)) return true;
    }
    return false;
}

hakios_metal_program *
preloadProgram(hakios_metal_device *d, unsigned targets, unsigned zs)
{
    std::lock_guard<std::mutex> g(d->preload_lock);
    unsigned k = targets > 4 ? 4 : targets;
    zs &= 7;
    if (d->preload_for[k][zs] != nullptr || d->preload_failed_for[k][zs]) {
        return d->preload_for[k][zs];
    }
    auto *p = new hakios_metal_program;
    char why[256] = "";
    if (mali_preload_zs_compile(&p->vs, &p->fs, targets, zs, why, sizeof(why)) != 0 || !p->upload(d)) {
        d->preload_error = std::string("load action Load: the preload: ") + (why[0] ? why : gError.c_str());
        d->preload_failed_for[k][zs] = true;
        p->release();
        return nullptr;
    }
    snprintf(p->name, sizeof(p->name), "hakios_preload%u_%u", targets, zs);
    d->preload_for[k][zs] = p;
    return p;
}

int
drawImpl(hakios_metal_cb *cb, hakios_metal_program *p, const struct hakios_metal_render_pass *pass,
    const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive,
    unsigned vertex_start, unsigned vertex_count, unsigned instance_count, unsigned base_instance,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count, hakios_metal_sampler *const *fs_samplers,
    unsigned fs_sampler_count, const IndexInfo *ix, const uint32_t *fs_words, unsigned fs_word_count)
{
    if (atLevels(pass)) {
        struct hakios_metal_render_pass q;
        hakios_metal_texture *views[10];
        if (!passAtLevels(pass, q, views)) {
            std::string why = gError;
            fail("%s%s%s", p ? p->name : "", p ? ": " : "", why.c_str());
            return -1;
        }
        int r = drawImpl(cb, p, &q, viewport, scissor, primitive, vertex_start, vertex_count, instance_count, base_instance,
            vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, fs_textures, fs_texture_count, fs_samplers,
            fs_sampler_count, ix, fs_words, fs_word_count);
        for (auto *v : views) if (v) v->release();     // (the draw holds what it uses)
        return r;
    }
    if (pass != nullptr && pass->color0 == nullptr && (pass->depth != nullptr || pass->stencil != nullptr)) {
        // DEPTH OR STENCIL ALONE (11-15: a shadow map's pass, no colour attachment): the fragment job's colour target is
        // the depth texture's stand-in -- not stored, so nothing is written to it
        bool more = false;
        for (unsigned i = 0; i < 7; i++) more = more || pass->more[i].texture != nullptr;
        hakios_metal_texture *zs = pass->depth != nullptr ? pass->depth : pass->stencil;
        if (more || zs->samples > 1) {
            fail("%s%sa draw without colour attachment 0 %s", p ? p->name : "", p ? ": " : "",
                more ? "but with others (not on the Mali yet)" : "into a multisampled depth buffer (one sample on the Mali yet)");
            return -1;
        }
        hakios_metal_texture *stand = zs->stand_in.load();
        if (stand == nullptr) {
            hakios_metal_texture *made = hakios_metal_texture_new_levels(cb->queue->device, 2, 70, zs->width, zs->height, 1, 4 | 1);
            if (made == nullptr) {
                return -1;                              // (the reason is the texture's)
            }
            if (zs->stand_in.compare_exchange_strong(stand, made)) {
                stand = made;
            } else {
                made->release();
            }
        }
        struct hakios_metal_render_pass q = *pass;
        q.color0 = stand;
        q.store_action = 0;
        return drawImpl(cb, p, &q, viewport, scissor, primitive, vertex_start, vertex_count, instance_count, base_instance,
            vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, fs_textures, fs_texture_count, fs_samplers,
            fs_sampler_count, ix, fs_words, fs_word_count);
    }
    hakios_metal_device *d = cb->queue->device;
    const char *refused = nullptr;
    hakios_metal_texture *t = pass ? pass->color0 : nullptr;

    // what the Mali build draws so far; the rest by name
    if (!d->heap_ok) refused = "draws (no tiler heap on this device: hakios_metal_open said why)";
    else if (p == nullptr || t == nullptr) refused = "a draw without a program or a colour attachment";
    else if (!drawable(t->format)) {
        static thread_local char why[120];      // (the format named: which ones applications ask for)
        snprintf(why, sizeof(why), "a colour attachment of MTLPixelFormat %u, not RGBA8/BGRA8/BGR5A1/B5G6R5 (not on the Mali yet)", t->format);
        refused = why;
    }
    else if (!(t->usage & HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET)) refused = "a colour attachment made without render-target usage";
    else if (pass->store_action > 1) refused = "a store action other than DontCare and Store";
    else if (pass->visibility && pass->visibility_mode) refused = "visibility results (not on the Mali yet)";
    else if (!p->blend_refusal.empty()) refused = p->blend_refusal.c_str();
    else if (!compileFetch(p)) refused = p->fetch_refusal.c_str();
    else if (p->samples == 2) refused = "2 samples a pixel (the Mali-G715 has no 2x sample pattern -- Mesa serves it from v12; 4 is served)";
    else if (t->samples != p->samples) refused = "a colour attachment of another sample count than the pipeline's rasterSampleCount";
    else if (primitive > 4) refused = "a primitive type past MTLPrimitiveTypeTriangleStrip";
    else if (instance_count == 0) refused = "no instances";
    else if (ix && (ix->buffer == nullptr || ix->type > 1 || ix->base_vertex < 0 ||
                    ix->offset + (uint64_t)vertex_count * (ix->type ? 4 : 2) > ix->buffer->length))
        refused = "an indexed draw past its index buffer, of an unknown index type or with a negative base vertex";
    // colour attachments past 0: the ones the fragment function writes are drawn into (up to 3 more: RGBA8/BGRA8, the
    // size of the first, level and slice 0); one it does not write is left as it is -- CoreAnimation attaches more
    // targets than most of its draws write -- and a clear of such a one is not done (said once).
    unsigned rts = 1;
    for (unsigned w = p ? p->fs.rt_written >> 1 : 0, i = 1; refused == nullptr && w != 0; w >>= 1, i++) {
        if (!(w & 1)) continue;
        const hakios_metal_render_pass::hakios_metal_color_attachment &m = pass->more[i - 1];
        if (i > 3) refused = "a fragment function writing a colour attachment past 3 (not on the Mali yet)";
        else if (m.texture == nullptr) { rts = i + 1; continue; }   // (Metal discards the output: see dj.more below)
        else if (!drawable(m.texture->format)) {
            static thread_local char why[120];
            snprintf(why, sizeof(why), "colour attachment %u of MTLPixelFormat %u, not RGBA8/BGRA8/BGR5A1/B5G6R5 (not on the Mali yet)", i, m.texture->format);
            refused = why;
        }
        else if (m.texture->width != t->width || m.texture->height != t->height)
            refused = "colour attachments of different sizes";
        else if (m.texture->samples != t->samples) refused = "colour attachments of different sample counts";
        else if (!(m.texture->usage & HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET)) refused = "a colour attachment made without render-target usage";
        else if (m.store_action > 1) refused = "a store action other than DontCare and Store";
        else if (!p->more_blend_refusal[i - 1].empty()) refused = p->more_blend_refusal[i - 1].c_str();
        rts = i + 1;
    }
    // EVERY ATTACHMENT OF THE PASS IN EVERY DRAW (up to 4 targets): one the fragment function does not write is still
    // in the Mali pass -- loaded, stored, cleared as the pass says -- its blend writing nothing. Without this a pass
    // whose draws wrote attachment 1 only now and then (CoreAnimation's in Settings) split into a Mali pass at every
    // change, each preloading and writing back the whole screen: 20-36 passes a frame, 100-240 ms.
    for (unsigned i = rts; refused == nullptr && i <= 3; i++) {
        const hakios_metal_render_pass::hakios_metal_color_attachment &m = pass->more[i - 1];
        if (m.texture == nullptr) continue;
        if (!drawable(m.texture->format) || m.texture->width != t->width || m.texture->height != t->height ||
            m.store_action > 1 || m.texture->samples != t->samples) {
            break;                                      // (left out, as before: a later one too)
        }
        rts = i + 1;
    }
    // ONE MALI PASS FOR CROSS-FADED DRAWS (11-11): CoreAnimation's fixed_frag_lph_cpf (the generic pipeline) writes a
    // third colour output its passes have no attachment for -- a target of the scratch's (dj.more below) -- and so it had
    // a Mali pass of three targets amid the passes of two of the same encoder: the home screen's frames split into up to
    // 16 full-screen passes, each preloading and writing back the screen (~1.1 ms each, 18 ms a frame; metal_stats_ms).
    // So each target set (keyed on colour attachment 1, CoreAnimation's memoryless one, or else attachment 0) remembers
    // the most targets a draw of it had, and a draw with fewer takes the missing ones too -- outputs it never writes, a
    // scratch target never loaded nor written back -- and samePass keeps it in the Mali pass. Boot-arg mwide=0: not done.
    static const bool no_wide = bootArg("mwide=0");
    if (refused == nullptr && !no_wide && t != nullptr) {
        hakios_metal_texture *key = rts > 1 && pass->more[0].texture != nullptr ? pass->more[0].texture : t;
        unsigned seen = key->widest_rts.load();
        if (key == t) {
            // (11-20) ... AND BY THE KIND OF TARGET where there is no attachment 1 to key on: Maps draws each frame into
            // a NEW multisample texture, so nothing was remembered -- its clear, atmosphere and stars (one target) and
            // then its terrain (a second output with no attachment) were two Mali passes every frame, the depth and
            // stencil of four samples stored and loaded between them (and not even stored: the pass ends DontCare --
            // zsKeep), the terrain coming out a quarter there over the sky.
            std::lock_guard<std::mutex> g(d->wide_lock);
            unsigned &kind = d->wide_kinds[(uint64_t)t->width | (uint64_t)t->height << 16 | (uint64_t)t->format << 32 |
                (uint64_t)t->samples << 48];
            if (rts > kind) kind = rts;
            else if (kind > seen) seen = kind;
        }
        if (rts > seen) {
            key->widest_rts.store(rts);
        } else if (seen > rts) {
            bool none = true;                           // (only past every attachment the pass has)
            for (unsigned i = rts; i < seen && none; i++) {
                none = pass->more[i - 1].texture == nullptr;
            }
            if (none) {
                rts = seen;
            }
        }
    }
    for (unsigned i = rts; refused == nullptr && i < 8; i++) {
        if (pass->more[i - 1].texture != nullptr && pass->more[i - 1].load_action == 2) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) {
                fprintf(stderr, "hakios-metal: colour attachment %u's clear is not done: no draw writes it\n", i);
            }
        }
    }
    bool loads = refused == nullptr && pass->load_action == 1;
    for (unsigned i = 1; refused == nullptr && i < rts; i++) {
        static const bool no_ml = bootArg("metal_nomemoryless"), mlread_old = bootArg("mlread=1");
        const bool ml = pass->more[i - 1].texture != nullptr && pass->more[i - 1].texture->memoryless && !no_ml &&
            (!(p->fs.rt_read & 1u << i) || !mlread_old);    // (a memoryless attachment is not loaded: below)
        loads = loads || (pass->more[i - 1].texture != nullptr && !ml &&
                          (pass->more[i - 1].load_action == 1 || !(p->fs.rt_written & 1u << i)));
    }
    const unsigned ms4 = t != nullptr && t->samples > 1 ? 4u : 0u;     // (preloads of multisample attachments: a sample a run)
    hakios_metal_program *pre = loads ? preloadProgram(d, rts, ms4) : nullptr;
    if (loads && pre == nullptr) refused = d->preload_error.c_str();
    if (refused != nullptr) {
        fail("%s%s%s", p ? p->name : "", p ? ": " : "", refused);
        return -1;
    }
    if (vertex_count == 0) {
        return 0;
    }
    {
        // (boot-arg metal_skipprog=A,B, 11-11 for TODO 8h: the draws of programs whose names or pipeline labels hold A
        // or B are not done -- a screenshot without them says which draw makes a mark)
        static const std::vector<std::string> skip = [] {
            std::vector<std::string> v;
            char args[1024] = {};
            size_t size = sizeof(args) - 1;
            const char *at;
            if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && (at = strstr(args, "metal_skipprog=")) != nullptr) {
                std::string w;
                for (at += strlen("metal_skipprog="); ; at++) {
                    if (*at == ',' || *at == ' ' || *at == 0) {
                        if (!w.empty()) v.push_back(w);
                        w.clear();
                        if (*at != ',') break;
                    } else {
                        w += *at;
                    }
                }
            }
            for (const auto &w : v) fprintf(stderr, "hakios-metal: draws of programs named *%s* skipped (metal_skipprog)\n", w.c_str());
            return v;
        }();
        for (const auto &w : skip) {
            if (strstr(p->name, w.c_str()) != nullptr || strstr(p->label, w.c_str()) != nullptr) return 0;
        }
    }
    if (!p->upload(d)) {
        return -1;
    }
    {
        // (boot-arg metal_progsay: each program's first draw, with the time since boot as the kernel log's)
        static const bool say = bootArg("metal_progsay");
        if (say) {
            static std::mutex m;
            static std::vector<const void *> seen;
            std::lock_guard<std::mutex> g(m);
            if (std::find(seen.begin(), seen.end(), (const void *)p) == seen.end()) {
                seen.push_back(p);
                mach_timebase_info_data_t tb;
                mach_timebase_info(&tb);
                double t = (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
                fprintf(stderr, "hakios-metal: first draw of %s [%s] at %.3f s: fs push words %u, textures %u, samplers %u, blend "
                    "in shader 0x%x, written 0x%x, read 0x%x, target %ux%u format %u, samples %u, %u vertices x %u\n",
                    p->name, p->label, t, p->fs.fau_words, fs_texture_count, fs_sampler_count, p->shader_blend.mask, p->fs.rt_written,
                    p->fs.rt_read, pass && pass->color0 ? pass->color0->width : 0, pass && pass->color0 ? pass->color0->height : 0, pass && pass->color0 ? pass->color0->format : 0, p->samples,
                    vertex_count, instance_count);
                for (unsigned i = 0; i < 4; i++) {
                    if (p->shader_blend.mask & 1u << i) {
                        const mali_blend_rt &b = p->shader_blend.rt[i];
                        fprintf(stderr, "hakios-metal:   %s: target %u blended in the shader: ops %u/%u, factors %u %u / %u %u, "
                            "write mask 0x%x; blend colour %g %g %g %g\n", p->name, i, b.rgb_op, b.alpha_op, b.src_rgb, b.dst_rgb,
                            b.src_alpha, b.dst_alpha, b.write_mask, pass->blend_color[0], pass->blend_color[1],
                            pass->blend_color[2], pass->blend_color[3]);
                    }
                }
            }
        }
    }
    DrawJob dj = {};
    dj.samples = t->samples;
    dj.target_tex = t;
    markBusy(cb, d, t);
    {
        // marksay=1 (a test, 11-10): every big render target said once -- what it is over, and whether it is marked
        static const bool marksay = bootArg("marksay");
        static std::mutex said_lock;
        static std::vector<hakios_metal_texture *> said;
        if (marksay && t->width >= 1000 && t->height >= 1000) {
            std::lock_guard<std::mutex> g(said_lock);
            if (std::find(said.begin(), said.end(), t) == said.end() && said.size() < 64) {
                said.push_back(t);
                hakios_metal_texture *r = t;
                unsigned depth = 0;
                while (r != nullptr && r->origin == 4) { r = r->parent; depth++; }
                fprintf(stderr, "hakios-metal: marksay: target %p %ux%u fmt %u origin %u (after %u views: origin %u surface %u)%s\n",
                    (void *)t, t->width, t->height, t->format, t->origin, depth, r ? r->origin : 99, r ? r->surface : 0,
                    r && r->origin == 1 ? " MARKED" : " NOT MARKED");
            }
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        markBusy(cb, d, pass->more[i].texture);
    }
    std::vector<hakios_metal_buffer *> used;
    struct hakios_metal_viewport vp = viewport ? *viewport :
        hakios_metal_viewport{ 0, 0, (double)t->width, (double)t->height, 0, 1 };
    // Metal's viewport: origin top left, NDC y up, z in [0, 1]
    float scale[3] = { (float)(vp.width / 2), (float)(-vp.height / 2), (float)(vp.zfar - vp.znear) };
    float offset[3] = { (float)(vp.x + vp.width / 2), (float)(vp.y + vp.height / 2), (float)vp.znear };
    nothingVa(d);       // (for a buffer a stage declares and nothing is bound at)
    if (!stagePushConstants(p->vs, vs_buffers, vs_offsets, vs_count, scale, offset, vertex_start, base_instance, dj.vs_fau, used) ||
        !stagePushConstants(p->fs, fs_buffers, fs_offsets, fs_count, scale, offset, vertex_start, base_instance, dj.fs_fau, used)) {
        return -1;
    }
    for (unsigned i = 0; i < fs_word_count && i < dj.fs_fau.size(); i++) {
        dj.fs_fau[i] = fs_words[i];                     // (the driver's own programs: their words from 0, no buffers)
    }
    if (pass != nullptr && dj.fs_fau.size() > MALI_DRAW_BLEND_COLOR + 1) {
        // the blend colour, for blending in the shader (BlendColor/BlendAlpha factors): four halves
        uint16_t half[4];
        for (int c = 0; c < 4; c++) {
            __fp16 h = (__fp16)pass->blend_color[c];
            memcpy(&half[c], &h, 2);
        }
        dj.fs_fau[MALI_DRAW_BLEND_COLOR] = half[0] | (uint32_t)half[1] << 16;
        dj.fs_fau[MALI_DRAW_BLEND_COLOR + 1] = half[2] | (uint32_t)half[3] << 16;
    }
    // the fragment stage's textures and samplers, as a dispatch's
    uint32_t fs_nothing_at = 0, fs_null_at = 0;
    for (unsigned r = 0; r < p->fs.num_resources; r++) {
        const mali_kernel_resource &res = p->fs.resources[r];
        if (res.kind == HAKIOS_METAL_RESOURCE_TEXTURE && res.size == 0xfffe && res.index < MALI_KERNEL_TEXTURES) {
            fs_null_at |= 1u << res.index;     /* (the null texture's reserved index: see the dispatch's) */
        }
        if (res.kind == HAKIOS_METAL_RESOURCE_TEXTURE) {
            if (res.index >= fs_texture_count || fs_textures[res.index] == nullptr || (fs_null_at & 1u << res.index)) {
                // (nothing bound: the device's 4x4 of zeroes stands for it)
                if (res.index >= MALI_KERNEL_TEXTURES || nothingTexture(d) == nullptr) {
                    fail("draw with %s: nothing bound at [[texture(%u)]]", p->name, res.index);
                    return -1;
                }
                fs_nothing_at |= 1u << res.index;
            }
            dj.texture_count = res.index + 1 > dj.texture_count ? res.index + 1 : dj.texture_count;
        } else if (res.kind == HAKIOS_METAL_RESOURCE_SAMPLER) {
            if (res.inline_sampler) {
                // the function's own (constexpr sampler): made from its state word below, nothing bound
            } else if (res.index >= MALI_KERNEL_TEXTURES) {
                fail("draw with %s: nothing bound at [[sampler(%u)]]", p->name, res.index);
                return -1;
            } else if (res.index >= fs_sampler_count || fs_samplers[res.index] == nullptr) {
                // (nothing bound, 11-12: a nearest, clamped sampler stands for it)
                dj.samplers[res.index][0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
            }
            dj.sampler_count = res.index + 1 > dj.sampler_count ? res.index + 1 : dj.sampler_count;
        } else if (res.kind == HAKIOS_METAL_RESOURCE_IMAGE) {
            fail("draw with %s: a texture the fragment function writes (not on the Mali yet)", p->name);
            return -1;
        }
    }
    if (dj.texture_count != 0 && dj.sampler_count == 0) {
        dj.sampler_count = 1;
        dj.samplers[0][0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
    }
    for (unsigned i = 0; i < dj.texture_count; i++) {
        dj.textures[i] = (fs_null_at & 1u << i) ? nothingTexture(d) : i < fs_texture_count && fs_textures[i] != nullptr ? fs_textures[i] :
            (fs_nothing_at & 1u << i) ? nothingTexture(d) : nullptr;
        { hakios_metal_texture *rt_ = (dj.textures[i]); while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent; if (rt_ != nullptr) rt_->reads++; }
    }
    for (unsigned i = 0; i < dj.sampler_count && i < fs_sampler_count; i++) {
        if (fs_samplers[i] != nullptr) {
            memcpy(dj.samplers[i], fs_samplers[i]->words, 32);
        }
    }
    for (unsigned r = 0; r < p->fs.num_resources; r++) {
        const mali_kernel_resource &res = p->fs.resources[r];
        if (res.kind != HAKIOS_METAL_RESOURCE_SAMPLER || !res.inline_sampler || res.index >= MALI_KERNEL_TEXTURES) {
            continue;
        }
        if (!inlineSampler(res.sampler_state, dj.samplers[res.index])) {
            fail("draw with %s: its constexpr sampler at %u: %s", p->name, res.index, gError.c_str());
            return -1;
        }
    }
    // the vertex stage's, the same way from what the command buffer was given (hakios_metal_cb_set_vertex_textures)
    {
        const uint32_t nearest = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
        uint32_t vs_nothing_at = 0, vs_null_at = 0;
        for (unsigned r = 0; r < p->vs.num_resources; r++) {
            const mali_kernel_resource &res = p->vs.resources[r];
            if (res.kind == HAKIOS_METAL_RESOURCE_TEXTURE) {
                if (res.index >= MALI_KERNEL_TEXTURES || (cb->vs_textures[res.index] == nullptr && nothingTexture(d) == nullptr)) {
                    fail("draw with %s: nothing bound at the vertex function's [[texture(%u)]]", p->name, res.index);
                    return -1;
                }
                if (cb->vs_textures[res.index] == nullptr || res.size == 0xfffe) vs_nothing_at |= 1u << res.index;
                if (res.size == 0xfffe) vs_null_at |= 1u << res.index;     /* (the null texture's reserved index) */
                dj.vs_texture_count = res.index + 1 > dj.vs_texture_count ? res.index + 1 : dj.vs_texture_count;
            } else if (res.kind == HAKIOS_METAL_RESOURCE_SAMPLER) {
                if (res.index >= MALI_KERNEL_TEXTURES) {
                    fail("draw with %s: nothing bound at the vertex function's [[sampler(%u)]]", p->name, res.index);
                    return -1;
                }
                if (res.inline_sampler) {
                    if (!inlineSampler(res.sampler_state, dj.vs_samplers[res.index])) {
                        fail("draw with %s: its vertex function's constexpr sampler at %u: %s", p->name, res.index, gError.c_str());
                        return -1;
                    }
                } else if (cb->vs_sampler_set[res.index]) {
                    memcpy(dj.vs_samplers[res.index], cb->vs_samplers[res.index], 32);
                } else {
                    dj.vs_samplers[res.index][0] = nearest;
                }
                dj.vs_sampler_count = res.index + 1 > dj.vs_sampler_count ? res.index + 1 : dj.vs_sampler_count;
            } else if (res.kind == HAKIOS_METAL_RESOURCE_IMAGE) {
                fail("draw with %s: a texture the vertex function writes (not on the Mali yet)", p->name);
                return -1;
            }
        }
        if (dj.vs_texture_count != 0 && dj.vs_sampler_count == 0) {
            dj.vs_sampler_count = 1;
            dj.vs_samplers[0][0] = nearest;
        }
        for (unsigned i = 0; i < dj.vs_texture_count; i++) {
            hakios_metal_texture *t = (vs_null_at & 1u << i) ? nothingTexture(d) : cb->vs_textures[i] != nullptr ? cb->vs_textures[i] : (vs_nothing_at & 1u << i) ? nothingTexture(d) : nullptr;
            dj.vs_textures[i] = t;
            if (t != nullptr) {
                hakios_metal_texture *rt_ = t;
                while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent;
                if (rt_ != nullptr) rt_->reads++;
                t->retain();
                cb->held_textures.push_back(t);
            }
        }
    }
    dj.name = p->name;
    dj.program = p;
    dj.pos_spd = p->pos_spd;
    dj.vary_spd = p->vary_spd;
    dj.frag_spd = p->frag_spd;
    {
        uint32_t tls = p->vs.tls_size > p->fs.tls_size ? p->vs.tls_size : p->fs.tls_size;
        if (tls != 0) {
            if (log2u((tls + 15) / 16) >= 12 ||
                !grow(d, d->tls[log2u((tls + 15) / 16)], (uint64_t)npot((tls + 15) & ~15u) * d->max_threads * d->core_range)) {
                fail("draw with %s: no memory for %u bytes of spilled registers a thread", p->name, tls);
                return -1;
            }
            dj.tls_size = tls;
        }
    }
    {
        const char *why = nullptr;
        if (!zsOf(pass, dj, t->width, t->height, true, why)) {
            fail("%s: %s", p->name, why);
            return -1;
        }
    }
    dj.vary_size = p->vs.varying_bytes;
    dj.target_va = t->gpu();
    dj.width = t->width;
    dj.height = t->height;
    dj.row_bytes = t->stride;
    rtFormat(t->format, dj.rt1);
    dj.write = pass->store_action == 1;
    if (pass->load_action == 2) {
        dj.clear = rtClear(dj.rt1, pass->clear_color);
        dj.clear_high = rtClearHigh(dj.rt1, pass->clear_color);
    }
    if (loads) {
        // Load: the preload draw first (emitDraw), target n its texture n, over the whole target
        float full_scale[3] = { (float)t->width / 2, -(float)t->height / 2, 1 }, full_offset[3] = { (float)t->width / 2, (float)t->height / 2, 0 };
        if (!stagePushConstants(pre->vs, nullptr, nullptr, 0, full_scale, full_offset, 0, 0, dj.pre_vs_fau, used) ||
            !stagePushConstants(pre->fs, nullptr, nullptr, 0, full_scale, full_offset, 0, 0, dj.pre_fs_fau, used)) {
            return -1;
        }
        dj.load = true;
        dj.load0 = pass->load_action == 1;
        dj.pre_pos_spd = pre->pos_spd;
        dj.pre_frag_spd = pre->frag_spd;
        dj.pre_target = t;                              // (held with the draw's target below)
    }
    {
        static const bool no_ir = bootArg("metal_noir");
        const unsigned irzs = (dj.has_z ? 1u : 0u) | (dj.has_s ? 2u : 0u);
        hakios_metal_program *irp = no_ir ? nullptr : loads && irzs == 0 ? pre : preloadProgram(d, rts, irzs | ms4);
        float full_scale[3] = { (float)t->width / 2, -(float)t->height / 2, 1 }, full_offset[3] = { (float)t->width / 2, (float)t->height / 2, 0 };
        if (irp != nullptr &&
            stagePushConstants(irp->fs, nullptr, nullptr, 0, full_scale, full_offset, 0, 0, dj.ir_fs_fau, used)) {
            dj.ir_frag_spd = irp->frag_spd;
            dj.ir_target = t;
        }
        // DEPTH/STENCIL LOADED: by a pre-frame shader (the framebuffer descriptor's Z/S preload bits load nothing)
        const unsigned zsl = (dj.has_z && dj.z_load ? 1u : 0u) | (dj.has_s && dj.s_load ? 2u : 0u);
        if (zsl != 0) {
            hakios_metal_program *zp = preloadProgram(d, 0, zsl | ms4);
            if (zp == nullptr || !stagePushConstants(zp->fs, nullptr, nullptr, 0, full_scale, full_offset, 0, 0, dj.zs_fs_fau, used)) {
                fail("%s: depth/stencil Load: %s", p->name, d->preload_error.c_str());
                return -1;
            }
            dj.zs_frag_spd = zp->frag_spd;
        }
    }
    // the blend descriptor: enabled; the tile buffer loaded when the equation or the mask reads it; opaque (mode 1)
    // for a plain replace of every channel, fixed function (2) otherwise
    // (a target the fragment function reads -- framebuffer fetch, LD_TILE -- is loaded too, as panfrost's blend for a
    // shader that reads the tile buffer: in opaque mode the pixel before is not there to read)
    bool reads0 = p->blend_reads_dest || (p->fs.rt_read & 1);
    dj.blend[0] = 1u << 9 | (reads0 ? 1u : 0);
    dj.blend[1] = p->blend_equation;
    // (a raw target, R32Float: opaque always -- the fixed-function blender does not know it; what the shader read of it
    // came through LD_TILE, the tile loaded by blend[0]'s bit all the same)
    dj.blend[2] = (reads0 && !(dj.rt1 & kRtRaw) ? 2u : 1u) | 3 << 3;   // mode, 4 components, RT 0
    dj.blend[3] = rtBlend(dj.rt1);                           // the tile buffer's RGBA8
    dj.rts = rts;
    if (pass->layers > 1) {
        // LAYERED RENDERING: see the pass's end in emitDraw
        // (a pass that LOADS its layers is not refused here -- every draw after an encoder's first comes as one that
        // loads -- but nothing is loaded: emitDraw's preload is left out, the layers begin as the tile buffer's clear)
        // (the colour target: an array or cube of that many layers -- or one that is not stored at all, the stand-in of
        // a pass of depth alone; the depth target, if any: an array or cube too, and no stencil)
        const bool colour_ok = t->layers >= pass->layers || !dj.write;
        const bool depth_ok = !dj.zs || (dj.has_z && !dj.has_s && pass->depth != nullptr && pass->depth->layers >= pass->layers);
        if (pass->layers > 8 || !colour_ok || !depth_ok || rts != 1 || t->samples > 1) {
            fail("%s: a layered pass (%u layers) into a colour texture of %u layers%s, %u colour targets%s%s: served are 2 to 8 "
                "layers of one array or cube colour texture and a depth texture of as many, no stencil, one sample", p->name,
                pass->layers, t->layers, dj.zs && !depth_ok ? ", a depth or stencil target that is not such an array" : "", rts,
                dj.has_s ? ", stencil" : "", t->samples > 1 ? ", multisampled" : "");
            return -1;
        }
        dj.layers = pass->layers;
        dj.layer_stride = t->layers >= pass->layers ? t->layer_stride : 0;
        dj.z_layer_stride = dj.has_z ? pass->depth->layer_stride : 0;
    }
    for (unsigned i = 1; i < rts; i++) {
        const hakios_metal_render_pass::hakios_metal_color_attachment &a = pass->more[i - 1];
        DrawJob::More &m = dj.more[i - 1];
        if (a.texture == nullptr) {
            // AN OUTPUT WITH NO ATTACHMENT, which Metal discards: a target of its own in the tile buffer, never written
            // back, whose blend writes no channel (CoreAnimation's fixed_frag_lph_cpf writes two in passes of one: its
            // draws were refused, pieces missing while the home screen moved)
            m.missing = true;
            m.row_bytes = (t->width * 4 + 63) & ~63u;
            {
                std::lock_guard<std::mutex> g(d->scratch_lock);
                uint64_t need = (uint64_t)m.row_bytes * t->height * t->samples;
                if (d->scratch_rt_size < need) {
                    if (d->scratch_rt_size != 0) {
                        d->freeBo(d->scratch_rt);       // (a draw in flight may still write the old one: rare, and only
                    }                                   //  its own scratch pixels, which nobody reads)
                    d->scratch_rt_size = d->newBo(d->scratch_rt, need, BIND_NOEXEC) ? need : 0;
                }
                if (d->scratch_rt_size == 0) {
                    fail("no scratch memory for a colour output with no attachment");
                    return -1;
                }
                m.va = d->scratch_rt.va;
            }
            m.rt1 = 19 << 3 | 2 << 8;
            m.write = false;
            m.load = false;
            m.clear = 0;
            m.texture = t;                              // (only for the preload's table: its blend writes nothing here)
            m.blend[0] = 1u << 9 | 1u;
            m.blend[1] = 0x132 | 0x132 << 12;           // colour mask 0
            m.blend[2] = 2u | 3 << 3 | i << 16;
            m.blend[3] = rtBlend(m.rt1);
            continue;
        }
        m.va = a.texture->gpu();
        m.row_bytes = a.texture->stride;
        a.texture->as_extra_rt++;
        /*
         * MEMORYLESS (11-10): CoreAnimation's second colour attachment is a memoryless texture -- an Apple GPU keeps it in
         * tile memory, its contents undefined past the pass; it is never sampled, copied or read (128682 draws, 0 reads).
         * The Mali has a tile buffer too: there it stays, never preloaded nor written back. Loaded and stored as any
         * other, a full-screen pass moved ~20 MB for it -- Spotlight's opening, ~8 such passes a frame at 2.1-2.8 ms
         * each. A draw whose fragment function reads it (framebuffer fetch) keeps the load and store, as before.
         * Boot-arg metal_nomemoryless: as before everywhere.
         */
        // (11-11: read by the fragment function too -- framebuffer fetch reads the tile buffer, and Metal never loads a
        // memoryless attachment from memory: CoreAnimation's cross-fade reads it, and its Mali pass loaded and wrote back
        // ~20 MB of stale memory for it. Boot-arg mlread=1: loaded and stored when read, as before)
        static const bool no_memoryless = bootArg("metal_nomemoryless"), mlread_old = bootArg("mlread=1");
        m.memoryless = a.texture->memoryless && !no_memoryless && (!(p->fs.rt_read & 1u << i) || !mlread_old);
        rtFormat(a.texture->format, m.rt1);
        m.write = a.store_action == 1;
        m.load = a.load_action == 1;
        m.clear = a.load_action == 2 ? rtClear(m.rt1, a.clear_color) : 0;
        m.clear_high = a.load_action == 2 ? rtClearHigh(m.rt1, a.clear_color) : 0;
        m.texture = a.texture;
        if (!(p->fs.rt_written & 1u << i)) {
            // not written by this draw: in the pass only to keep the pass whole, exactly as it is -- loaded and stored
            // back unchanged, never cleared here (as when it was left out: CoreAnimation's Settings showed another
            // cell's label when this draw applied the attachment's Clear/DontCare). When no draw of the Mali pass
            // touches it, it is not loaded nor stored at all (passElide)
            m.load = !m.memoryless;
            m.write = !m.memoryless;
            m.untouched = !(p->fs.rt_read & 1u << i) && !p->more_blend_reads_dest[i - 1];
            m.clear = 0;
            m.blend[0] = 1u << 9 | 1u;
            m.blend[1] = 0x132 | 0x132 << 12;           // colour mask 0
            m.blend[2] = 2u | 3 << 3 | i << 16;
            m.blend[3] = rtBlend(m.rt1);
            continue;
        }
        if (m.memoryless) {
            m.load = false;                             // (its clear, if any, still starts the tile buffer's copy)
            m.write = false;
        }
        bool reads = p->more_blend_reads_dest[i - 1] || (p->fs.rt_read & 1u << i);
        m.blend[0] = 1u << 9 | (reads ? 1u : 0);
        m.blend[1] = p->more_blend_equation[i - 1];
        m.blend[2] = (reads && !(m.rt1 & kRtRaw) ? 2u : 1u) | 3 << 3 | i << 16;
        m.blend[3] = rtBlend(m.rt1);
    }
    // Metal's point, line, line strip, triangle, triangle strip as Mali's draw modes (genxml v10 "Draw Mode")
    static const unsigned kDrawModes[5] = { 1, 2, 4, 8, 10 };
    dj.mode = kDrawModes[primitive];
    dj.count = vertex_count;
    dj.instances = instance_count;
    dj.vertex_start = ix ? (unsigned)ix->base_vertex : vertex_start;
    // DCD0: scissor to the bounding box, cull zero area (v11); the face culled and the front's winding.
    // THE WINDING AS METAL NAMES IT (11-13): Metal's clockwise is clockwise as the target shows the triangle (y down),
    // and so is the Mali's "front face CCW" (panvk sets it from Vulkan's framebuffer-space winding, which is that
    // too). From 11-10 until now it was FLIPPED here, on the strength of sb_cube=1's cube -- CoreAnimation layers,
    // which do not cull on the GPU at all (both ways draw it right, day13/cube-both.png). WebKit's WebGL does: the
    // aquarium (webglsamples.org) drew every fish from its inside -- lit from behind, dark -- and its glass globe not
    // at all, so the page's clear colour stood where the dome is; the tester's pictures from another phone beside
    // ours (day13/aq25 against aq26). metal_winding=flip: as it was from 11-10 (a test).
    static const bool winding_flipped = [] {
        char args[1024] = {0};
        size_t size = sizeof(args) - 1;
        return sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && strstr(args, "metal_winding=flip") != nullptr;
    }();
    bool front_ccw = winding_flipped ? pass->winding == 0 : pass->winding == 1;
    dj.dcd0 = 1u << 23 | 1u << 26 | (front_ccw ? 1u << 16 : 0) | (pass->cull_mode == 1 ? 1u << 17 : 0) |
        (pass->cull_mode == 2 ? 1u << 18 : 0) | (dj.samples > 1 ? 1u << 19 : 0);  // (19: multisample)
    if (dj.zs && ((dj.zsd[4] >> 29) != 7 || (dj.zsd[4] & 1u << 27) || (dj.zsd[0] >> 31))) {
        // a depth or stencil test or write: pixels killed weakly early, Z/S updated late (a fragment the shader
        // discards must not write them; whether a shader discards is not tracked here)
        dj.dcd0 |= 2u << 2 | 3u << 4;
    }
    dj.dcd1 = 0xffff | 1u << 16;
    // (11-12 night) THE READ MASK: the colour targets the fragment function reads from the tile buffer (framebuffer fetch,
    // or the blend computed in the shader) -- Mesa's cfg.read_mask = rt_read. Without it two draws over the same pixels
    // in one pass each read the tile as it was before either: WebKit's path coverage, added into an R32Float target, came
    // out as noise (mali_draw_test "blending into R32Float": two draws added once)
    dj.dcd2 = 1u << 12 | 1u << 24 | 1u << 25 | (p->fs.rt_read & 0xffu);
    // POINTS (11-14): a point is as large as the vertex function's [[point_size]] -- the size array's format FP16 and
    // the position FIFO's extended entries, which hold it (panvk: writes_point_size, extended_fifo); without both the
    // points of MetalTest's page 17 were not drawn at all. Anything else that such a function draws goes through its
    // variant without the write. A function that writes none: points of the primitive size register's 1.0.
    const bool psiz = primitive == 0 && p->writes_psiz;
    if (!psiz && p->writes_psiz && p->pos_spd_no_psiz != 0) {
        dj.pos_spd = p->pos_spd_no_psiz;
    }
    dj.tiler_flags = dj.mode | (psiz || p->extended_fifo ? 2u : 1u) << 22 | (psiz ? 2u << 11 : 0) |
        (dj.vary_spd ? 1u << 18 : 0) |  // the FIFO's format; the varying shader
        (dj.layers > 1 && p->writes_layer ? 1u << 20 : 0);                      // the layer the vertex function names
    if (ix) {
        // the indices: 16 or 32 bits (Mali's index type 2, 3), primitive restart at the type's all-ones as Metal has it
        dj.index_va = ix->buffer->bo.va + ix->offset;
        dj.index_size = ix->buffer->length - ix->offset;
        dj.tiler_flags |= (ix->type ? 3u : 2u) << 8 | 1u << 19;
        used.push_back(ix->buffer);
    }
    unsigned x0 = 0, y0 = 0, x1 = t->width, y1 = t->height;
    if (scissor != nullptr) {
        x0 = scissor->x < t->width ? scissor->x : t->width;
        y0 = scissor->y < t->height ? scissor->y : t->height;
        x1 = scissor->x + scissor->width < t->width ? scissor->x + scissor->width : t->width;
        y1 = scissor->y + scissor->height < t->height ? scissor->y + scissor->height : t->height;
        if (x1 <= x0 || y1 <= y0) {
            return 0;                                   // nothing inside the scissor
        }
    }
    dj.scissor_min = x0 | y0 << 16;
    dj.scissor_max = (x1 - 1) | (y1 - 1) << 16;
    // recorded: the GPU's state kept alive until it is done
    Job j = {};
    j.shader = DRAW;
    j.dst = cb->draws.size();
    // (moved, not copied: a copy allocated each of the draw's vectors of push constants again -- the aquarium's 615
    // draws a frame, with the queue's thread freeing the frame before's at the same time and both waiting for the
    // allocator's lock; the scalars read below stay as they are)
    cb->draws.push_back(std::move(dj));
    cb->blits.push_back(j);
    p->retain();
    cb->programs.push_back(p);
    t->retain();
    cb->held_textures.push_back(t);
    for (hakios_metal_texture *x : { pass->depth, pass->stencil }) {
        if (x != nullptr && dj.zs) {
            x->retain();
            cb->held_textures.push_back(x);
        }
    }
    for (unsigned i = 1; i < dj.rts; i++) {
        dj.more[i - 1].texture->retain();
        cb->held_textures.push_back(dj.more[i - 1].texture);
    }
    for (unsigned i = 0; i < dj.texture_count; i++) {
        if (dj.textures[i] != nullptr) {
            dj.textures[i]->retain();
            cb->held_textures.push_back(dj.textures[i]);
        }
    }
    for (auto *b : used) {
        cb->hold(b);
    }
    return 0;
}
} }  // namespace mali, extern "C++"

int
hakios_metal_cb_draw(hakios_metal_cb *cb, hakios_metal_program *program, const struct hakios_metal_render_pass *pass,
    const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive,
    unsigned vertex_start, unsigned vertex_count, unsigned instance_count, hakios_metal_buffer *const *vs_buffers,
    const uint64_t *vs_offsets, unsigned vs_count, hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets,
    unsigned fs_count)
{
    return drawImpl(cb, program, pass, viewport, scissor, primitive, vertex_start, vertex_count, instance_count, 0,
        vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, nullptr, 0, nullptr, 0);
}

int
hakios_metal_cb_draw2(hakios_metal_cb *cb, hakios_metal_program *program, const struct hakios_metal_render_pass *pass,
    const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive,
    unsigned vertex_start, unsigned vertex_count, unsigned instance_count, hakios_metal_buffer *const *vs_buffers,
    const uint64_t *vs_offsets, unsigned vs_count, hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets,
    unsigned fs_count, hakios_metal_texture *const *fs_textures, unsigned fs_texture_count,
    hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count)
{
    return drawImpl(cb, program, pass, viewport, scissor, primitive, vertex_start, vertex_count, instance_count, 0,
        vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, fs_textures, fs_texture_count, fs_samplers,
        fs_sampler_count);
}

int
hakios_metal_cb_draw3(hakios_metal_cb *cb, hakios_metal_program *program, const struct hakios_metal_render_pass *pass,
    const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive,
    unsigned vertex_start, unsigned vertex_count, unsigned instance_count, unsigned base_instance,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count, hakios_metal_sampler *const *fs_samplers,
    unsigned fs_sampler_count)
{
    return drawImpl(cb, program, pass, viewport, scissor, primitive, vertex_start, vertex_count, instance_count,
        base_instance, vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, fs_textures, fs_texture_count,
        fs_samplers, fs_sampler_count);
}
}  // extern "C"
