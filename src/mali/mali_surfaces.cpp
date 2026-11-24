// mali_surfaces.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

extern "C" {
extern "C++" { namespace mali {
const MaliFormat *
findFormat(unsigned pixel_format)
{
    for (const MaliFormat &m : kFormats) {
        if (m.metal == pixel_format) {
            return &m;
        }
    }
    return nullptr;
}

// a texture over memory not its own: OWNER's BO at OFFSET, STRIDE apart
hakios_metal_texture *
textureOver(hakios_metal_device *d, const Bo &bo, uint64_t offset, uint32_t stride, unsigned type, const MaliFormat *f,
    unsigned pixel_format, unsigned width, unsigned height, unsigned usage, unsigned origin)
{
    auto *t = new hakios_metal_texture;
    t->device = d;
    t->bo = bo;
    t->offset = offset;
    t->type = type;
    t->format = pixel_format;
    t->width = width;
    t->height = height;
    t->depth = 1;
    t->usage = usage;
    t->mali_format = f->mali;
    if (f->metal == 1) t->swizzle = kAlphaSwizzle;
    t->bytes = f->bytes;
    t->bw = f->bw;
    t->bh = f->bh;
    t->stride = stride;
    t->origin = origin;
    return t;
}
} }  // namespace mali, extern "C++"

/*
 * MIRRORS (11-10). The Mali samples a linear texture only from rows a multiple of 64 bytes apart: an IOSurface of other
 * rows (the App Library's background, a material's snapshot of 4320-byte rows) read so came out as nothing -- black. The
 * plug-in's way round it, a copy on the CPU (HakiosMetalCA.m, -hakiosShadow:), cost ~160 ms as the App Library came: 10
 * MB taken, written back after the draw into it, taken again. Here a texture over such a surface is over its MIRROR: a
 * texture of 64-byte rows, one for all the textures over the surface; filled from the surface on the GPU (copy_rows_u32)
 * when made and when the plug-in says the CPU changed the surface (hakios_metal_texture_surface_changed), and copied
 * back into the surface by every command buffer that draws into it, as its last job (mirrorsBack, from markBusy's list).
 * boot-arg metal_nomirror: such textures refused (the plug-in's CPU copy).
 */
extern "C++" Job
mirrorJob(const hakios_metal_device::Mirror &m, bool in)
{
    Job j = {};
    j.shader = COPY_ROWS;
    j.dst = in ? m.tex->gpu() : m.surface_va;
    j.src = in ? m.surface_va : m.tex->gpu();
    j.src_stride = in ? m.row_bytes : m.tex->stride;
    j.dst_stride = in ? m.tex->stride : m.row_bytes;
    j.value = m.row_words;
    j.count = m.row_words * m.rows;
    return j;
}

// the surface into its mirror, now (the mirror lock held): a command buffer of its own, waited for
extern "C++" bool
mirrorFill(hakios_metal_device *d, const hakios_metal_device::Mirror &m)
{
    if (d->mirror_queue == nullptr && (d->mirror_queue = hakios_metal_queue_new(d)) == nullptr) {
        return false;
    }
    hakios_metal_cb *cb = hakios_metal_cb_new(d->mirror_queue);
    if (cb == nullptr) {
        return false;
    }
    cb->blits.push_back(mirrorJob(m, true));
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    bool ok = cb->status == HAKIOS_METAL_COMPLETED;
    hakios_metal_cb_free(cb);
    return ok;
}

extern "C++" hakios_metal_texture *
mirrorOf(hakios_metal_device *d, uint64_t id, const Bo &bo, const hakios_metal_device::SurfaceInfo &info, const MaliFormat *f,
    unsigned pixel_format)
{
    std::lock_guard<std::mutex> g(d->mirror_lock);
    auto it = d->mirrors.find(id);
    if (it != d->mirrors.end()) {
        // a mirror that is there already holds the surface as it was when it was last filled: another process may have
        // drawn into the surface since (a CAMetalLayer's drawable, presented again), so a new texture over it begins
        // with a fresh copy -- the wrapper takes the seed of this moment as the mirror's, and would not refill it
        it->second.refs++;
        mirrorFill(d, it->second);
        static const bool say = bootArg("touch_lat=1");
        if (say) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "tl: M %u again at %llu\n", (unsigned)id, (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000);
        }
        return it->second.tex;
    }
    hakios_metal_device::Mirror m = {};
    m.tex = hakios_metal_texture_new_levels(d, 2, pixel_format, info.width, info.height, 1, 1 | 2 | 4);
    if (m.tex == nullptr) {
        return nullptr;
    }
    m.refs = 1;
    m.surface_va = bo.va + info.offset;
    m.row_bytes = info.row_bytes;
    m.row_words = (info.width * f->bytes + 3) / 4;      /* (a row of 94 bytes copied as 24 words: the row's own padding) */
    m.rows = info.height;
    if (!mirrorFill(d, m)) {
        m.tex->release();
        fail("IOSurface %u's mirror could not be filled", (unsigned)id);
        return nullptr;
    }
    static unsigned told;
    if (told++ < 8) {
        fprintf(stderr, "hakios-metal: IOSurface %u%s (%ux%u, rows of %u bytes) sampled through a mirror of rows of %u, "
            "copied on the GPU\n", (unsigned)id, id >> 32 ? " (a later plane)" : "", info.width, info.height, info.row_bytes,
            m.tex->stride);
    }
    d->mirrors[id] = m;
    return m.tex;
}

extern "C++" void
mirrorRelease(hakios_metal_device *d, uint64_t key)
{
    hakios_metal_texture *gone = nullptr;
    {
        std::lock_guard<std::mutex> g(d->mirror_lock);
        auto it = d->mirrors.find(key);
        if (it == d->mirrors.end() || --it->second.refs > 0) {
            return;
        }
        gone = it->second.tex;
        d->mirrors.erase(it);
    }
    gone->release();
}

extern "C++" void
mirrorsBack(hakios_metal_cb *cb, hakios_metal_device *d)
{
    if (cb->mirror_out.empty()) {
        return;
    }
    std::lock_guard<std::mutex> g(d->mirror_lock);
    for (uint64_t id : cb->mirror_out) {
        auto it = d->mirrors.find(id);
        if (it != d->mirrors.end()) {
            cb->blits.push_back(mirrorJob(it->second, false));
            it->second.tex->retain();
            cb->held_textures.push_back(it->second.tex);
        }
    }
    cb->mirror_out.clear();
}

void
hakios_metal_texture_surface_changed(hakios_metal_texture *t)
{
    while (t != nullptr && t->origin == 4) {
        t = t->parent;
    }
    if (t == nullptr || !t->mirrored) {
        return;
    }
    hakios_metal_device *d = t->device;
    std::lock_guard<std::mutex> g(d->mirror_lock);
    auto it = d->mirrors.find(t->mirror_key);
    if (it != d->mirrors.end()) {
        mirrorFill(d, it->second);
    }
}

void
hakios_metal_texture_set_memoryless(hakios_metal_texture *t, int on)
{
    if (t != nullptr) {
        t->memoryless = on != 0;
    }
}

int
hakios_metal_texture_mirrored(hakios_metal_texture *t)
{
    while (t != nullptr && t->origin == 4) {
        t = t->parent;
    }
    return t != nullptr && t->mirrored;
}

hakios_metal_texture *
hakios_metal_texture_new_iosurface_plane(hakios_metal_device *d, unsigned surface_id, uint64_t plane_delta, unsigned row_bytes,
    unsigned pixel_format, unsigned width, unsigned height, unsigned usage)
{
    // a later plane of a planar surface, as a texture of the format and size asked (SpringBoard asks RGBA8 of a '420f'
    // surface's CbCr plane: its bytes four at a time): the same memory, further on
    const MaliFormat *f = findFormat(pixel_format);
    if (f == nullptr || f->bw != 1 || f->bh != 1 || width == 0 || height == 0) {
        fail("a texture over a plane of IOSurface %u of MTLPixelFormat %u (not served on the Mali yet)", surface_id, pixel_format);
        return nullptr;
    }
    Bo bo;
    hakios_metal_device::SurfaceInfo info;
    if (!d->surfaceBo(surface_id, bo, info)) {
        return nullptr;
    }
    const uint64_t offset = info.offset + plane_delta;
    static const bool no_mirror = bootArg("metal_nomirror");
    if (((row_bytes & 63) || (offset & 63)) && !no_mirror && ((row_bytes | offset) & 3) == 0 && offset < (1ull << 32) &&
        ((width * f->bytes + 3) & ~3u) <= row_bytes && offset + (uint64_t)row_bytes * height <= info.size) {
        /*
         * A LATER PLANE'S MIRROR (11-20): the Camera's photos came out GREEN -- VideoToolbox's Metal transfer
         * (VTMTSComputeFunction) writes a '420f' surface's CbCr plane as R16Uint, and that plane's rows (1456 bytes) are no
         * multiple of 64: refused, the chroma stayed zero. As plane 0's (mirrorOf): a texture of 64-byte rows, filled from
         * the plane and copied back after a command buffer writes it; its own mirror, keyed by the plane's place too.
         */
        hakios_metal_device::SurfaceInfo pinfo = info;
        pinfo.offset = (uint32_t)offset;
        pinfo.row_bytes = row_bytes;
        pinfo.width = width;
        pinfo.height = height;
        const uint64_t key = surface_id | plane_delta << 32;
        hakios_metal_texture *m = mirrorOf(d, key, bo, pinfo, f, pixel_format);
        if (m == nullptr) {
            d->surfaceRelease(surface_id);
            return nullptr;
        }
        hakios_metal_texture *t = textureOver(d, m->bo, m->offset, m->stride, 2, f, pixel_format, width, height, usage, 1);
        t->surface = surface_id;
        t->mirrored = true;
        t->mirror_key = key;
        return t;
    }
    if ((uint64_t)width * f->bytes > row_bytes || offset + (uint64_t)row_bytes * (height - 1) + (uint64_t)width * f->bytes > info.size ||
        (row_bytes & 63) || (offset & 63)) {
        d->surfaceRelease(surface_id);
        fail("a texture of %u x %u, MTLPixelFormat %u over a plane of IOSurface %u at %llu, rows of %u bytes, in %llu bytes (rows "
            "and start of multiples of 64 bytes, inside the surface)", width, height, pixel_format, surface_id,
            (unsigned long long)offset, row_bytes, (unsigned long long)info.size);
        return nullptr;
    }
    hakios_metal_texture *t = textureOver(d, bo, offset, row_bytes, 2, f, pixel_format, width, height, usage, 1);
    t->surface = surface_id;
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_iosurface(hakios_metal_device *d, unsigned surface_id, unsigned plane, unsigned pixel_format,
    unsigned width, unsigned height, unsigned usage)
{
    const MaliFormat *f = findFormat(pixel_format);
    // (11-23) BGR10_XR / BGR10_XR_sRGB (554/555) over a surface of 8-bit BGRA ('BGRA', 4 bytes a pixel): what the surface
    // holds is BGRA8, and it is taken as that -- SpringBoard's lock-screen hello (the bookend: Setup's screen capture of
    // backboardd's virtual framebuffer, which our compositor fills as BGRA8) asked it for every frame, ~500 refusals,
    // "Failed to create BGRA texture: -6684" (kCVReturnPixelBufferNotMetalCompatible) and no animation
    if (f == nullptr && plane == 0 && (pixel_format == 554 || pixel_format == 555)) {
        Bo peek;
        hakios_metal_device::SurfaceInfo pinfo;
        if (d->surfaceBo(surface_id, peek, pinfo)) {
            const bool bgra8 = pinfo.pixel_format == 0x42475241u;   // 'BGRA'
            d->surfaceRelease(surface_id);
            if (bgra8) {
                pixel_format = pixel_format == 555 ? 81 : 80;       // BGRA8Unorm_sRGB, BGRA8Unorm
                f = findFormat(pixel_format);
            }
        }
    }
    if (f == nullptr || plane != 0) {
        fail("a texture over IOSurface %u of MTLPixelFormat %u, plane %u (not served on the Mali yet)", surface_id,
            pixel_format, plane);
        return nullptr;
    }
    Bo bo;
    hakios_metal_device::SurfaceInfo info;
    if (!d->surfaceBo(surface_id, bo, info)) {
        return nullptr;
    }
    if (width > info.width || height > info.height || (uint64_t)width * f->bytes > info.row_bytes ||
        info.offset + (uint64_t)info.row_bytes * (height - 1) + (uint64_t)width * f->bytes > info.size) {
        d->surfaceRelease(surface_id);
        fail("a texture of %u x %u over IOSurface %u of %u x %u (rows of %u bytes)", width, height, surface_id, info.width,
            info.height, info.row_bytes);
        return nullptr;
    }
    static const bool no_mirror = bootArg("metal_nomirror");
    if (((info.row_bytes & 63) || (info.offset & 63)) && !no_mirror && f->bw == 1 && f->bh == 1 &&
        ((info.row_bytes | info.offset) & 3) == 0 && ((info.width * f->bytes + 3) & ~3u) <= info.row_bytes) {
        hakios_metal_texture *m = mirrorOf(d, surface_id, bo, info, f, pixel_format);
        if (m == nullptr) {
            d->surfaceRelease(surface_id);
            return nullptr;
        }
        hakios_metal_texture *t = textureOver(d, m->bo, m->offset, m->stride, 2, f, pixel_format, width, height, usage, 1);
        t->surface = surface_id;
        t->mirrored = true;
        t->mirror_key = surface_id;
        return t;
    }
    if ((info.row_bytes & 63) || (info.offset & 63)) {
        // rows not a multiple of 64 bytes, as buffer textures are held to (hakios_metal_texture_new_buffer): sampled so,
        // the App Library's background -- a material's snapshot SpringBoard asks for with 4320-byte rows, drawn by
        // backboardd into the surface right (its pixels checked) -- came out black on the panel (11-10). Refused, the
        // plug-in reads such a surface through a copy of its rows and writes what is drawn into it back, as it did on
        // the Adreno (HakiosMetalCA.m, -hakiosShadow:)
        d->surfaceRelease(surface_id);
        fail("a texture over IOSurface %u of rows of %u bytes at offset %u: the Mali takes rows of multiples of 64 bytes",
            surface_id, info.row_bytes, (unsigned)info.offset);
        return nullptr;
    }
    hakios_metal_texture *t = textureOver(d, bo, info.offset, info.row_bytes, 2, f, pixel_format, width, height, usage, 1);
    t->surface = surface_id;
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_buffer(hakios_metal_buffer *b, unsigned type, unsigned pixel_format, unsigned width,
    unsigned height, unsigned usage, uint64_t offset, uint64_t bytes_per_row)
{
    const MaliFormat *f = findFormat(pixel_format);
    if (b == nullptr || f == nullptr || type != 2 || (offset & 63) || (bytes_per_row & 63) ||
        bytes_per_row < (uint64_t)width * f->bytes || offset + bytes_per_row * height > b->length) {
        fail("a texture over a buffer: of MTLPixelFormat %u, type %u, offset %llu, rows of %llu bytes (64-byte aligned, "
            "2D, inside the buffer)", pixel_format, type, (unsigned long long)offset, (unsigned long long)bytes_per_row);
        return nullptr;
    }
    hakios_metal_texture *t = textureOver(b->device, b->bo, offset, (uint32_t)bytes_per_row, type, f, pixel_format, width,
        height, usage, 2);
    b->retain();
    t->buffer = b;
    return t;
}
extern "C++" { namespace mali {
// levels BASE .. BASE + COUNT - 1 of P as a texture of their own over P's memory (P held): level BASE its level 0; a
// view of one level is described as any one-level texture, of more through P's plane descriptors
hakios_metal_texture *
levelsOf(hakios_metal_texture *p, const MaliFormat *f, unsigned pixel_format, unsigned type, unsigned base, unsigned count,
    unsigned layer)
{
    // (LAYER of a layered texture: that layer's levels, a texture of one layer)
    hakios_metal_texture *t = textureOver(p->device, p->bo, p->offset + p->layer_stride * layer + p->levelOffset(base),
        p->levelStride(base), type, f, pixel_format, p->levelWidth(base), p->levelHeight(base), p->usage, 4);
    t->depth = p->depth;
    t->levels = count;
    for (unsigned l = 0; l < count; l++) {
        t->level_off[l] = p->levelOffset(base + l) - p->levelOffset(base);
        t->level_stride[l] = p->levelStride(base + l);
    }
    t->planes_off = count > 1 && p->planes_off != 0 ? p->planes_off + 32 * ((uint64_t)layer * p->levels + base) : 0;
    if (p->stencil_level_stride[base] != 0) {
        const uint64_t lo = p->levelOffset(base);
        t->stencil_off = p->stencil_level_off[base] - lo;
        t->stencil_stride = p->stencil_level_stride[base];
        for (unsigned l = 0; l < count; l++) {
            t->stencil_level_off[l] = p->stencil_level_off[base + l] - lo;
            t->stencil_level_stride[l] = p->stencil_level_stride[base + l];
        }
    } else if (base == 0) {
        t->stencil_off = p->stencil_off;
        t->stencil_stride = p->stencil_stride;
    }
    p->retain();
    t->parent = p;
    t->surface = p->surface;
    return t;
}

// a render pass's attachment: level LEVEL (and a 3D texture's depth plane SLICE) of T as a one-level 2D texture, or
// nullptr with the reason
hakios_metal_texture *
attachmentOf(hakios_metal_texture *t, unsigned level, unsigned slice)
{
    if (level >= t->levels || slice >= (t->type == 7 ? t->depth : t->layers)) {
        fail("an attachment at level %u, slice %u of a texture of %u level(s) and %u slice(s)", level, slice, t->levels,
            t->type == 7 ? t->depth : t->layers);
        return nullptr;
    }
    if (t->layers > 1) {
        // a 2D array's layer or a cube's face: that layer's level, a 2D texture
        hakios_metal_texture *v = levelsOf(t, findFormat(t->format), t->format, t->type == 1 ? 0 : 2, level, 1, slice);
        v->depth = 1;
        return v;
    }
    hakios_metal_texture *v = levelsOf(t, findFormat(t->format), t->format, t->type == 7 ? 2 : t->type, level, 1);
    if (slice != 0) {
        v->offset += (uint64_t)v->stride * v->height * slice;
    }
    v->depth = 1;
    return v;
}
} }  // namespace mali, extern "C++"

hakios_metal_texture *
hakios_metal_texture_new_view(hakios_metal_texture *p, unsigned pixel_format, unsigned type, unsigned base_level,
    unsigned levels, unsigned base_slice, unsigned slices)
{
    const MaliFormat *f = findFormat(pixel_format);
    if (p != nullptr && f != nullptr && f->bytes == p->bytes && p->layers > 1 && levels != 0 &&
        base_level + levels <= p->levels && slices != 0 && base_slice + slices <= p->layers) {
        // SLICES OF A LAYERED TEXTURE: one layer (any levels of it) as a 2D or 1D texture; or a run of whole layers
        // (every level) as an array or a cube -- the layers' plane descriptors are one after the other
        const bool one = slices == 1 && (type == 2 || type == 0 || type == 3 || type == 1);
        const bool run = (base_level == 0 && levels == p->levels) &&
                         (type == 3 || type == 1 || ((type == 5 || type == 6) && slices % 6 == 0));
        if (one || run) {
            hakios_metal_texture *v = levelsOf(p, f, pixel_format, one ? (type == 3 ? 2 : type == 1 ? 0 : type) : type,
                base_level, levels, base_slice);
            if (!one || slices > 1) {
                v->layers = slices;
                v->layer_stride = p->layer_stride;
                v->planes_off = p->planes_off + 32 * (uint64_t)base_slice * p->levels;
            }
            return v;
        }
    }
    if (p == nullptr || f == nullptr || f->bytes != p->bytes || type != p->type || levels == 0 ||
        base_level + levels > p->levels || base_slice != 0 || slices != 1) {
        fail("a texture view of MTLPixelFormat %u, type %u, levels %u+%u, slices %u+%u of one with %u level(s) (the Mali "
            "build views levels of a texture, as a format of the same size, all its slices)", pixel_format, type,
            base_level, levels, base_slice, slices, p ? p->levels : 0);
        return nullptr;
    }
    return levelsOf(p, f, pixel_format, type, base_level, levels);
}

hakios_metal_texture *
hakios_metal_texture_new_swizzled(hakios_metal_texture *p, unsigned red, unsigned green, unsigned blue, unsigned alpha)
{
    // MTLTextureSwizzle (0 zero, 1 one, 2 red .. 5 alpha) as Mali's component selects (0 R .. 3 A, 4 zero, 5 one), over
    // what P already reads
    static const uint32_t to_mali[6] = { 4, 5, 0, 1, 2, 3 };
    if (p == nullptr || red > 5 || green > 5 || blue > 5 || alpha > 5) {
        fail("a texture swizzle past MTLTextureSwizzleAlpha");
        return nullptr;
    }
    hakios_metal_texture *t = textureOver(p->device, p->bo, p->offset, p->stride, p->type, findFormat(p->format), p->format,
        p->width, p->height, p->usage, 4);
    uint32_t base = p->swizzle ? p->swizzle : kIdentitySwizzle;
    auto pick = [&](unsigned m) -> uint32_t {
        uint32_t sel = to_mali[m];
        return sel < 4 ? (base >> (3 * sel)) & 7 : sel;
    };
    t->swizzle = pick(red) | pick(green) << 3 | pick(blue) << 6 | pick(alpha) << 9;
    p->retain();
    t->parent = p;
    t->surface = p->surface;
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_levels(hakios_metal_device *d, unsigned type, unsigned pixel_format, unsigned width,
    unsigned height, unsigned levels, unsigned usage)
{
    hakios_metal_texture *t = hakios_metal_texture_new(d, type, pixel_format, width, height, usage);
    if (t == nullptr || levels <= 1) {
        return t;
    }
    // MIPMAPPED: every level in one BO, each level's rows 64-byte aligned after the one before, then the levels' plane
    // descriptors ("Generic Plane", level 0 first: the texture descriptor's Surfaces, Levels and Maximum LOD)
    unsigned most = 1;
    while (most < 16 && ((width >> most) || (type != 0 && (height >> most)))) most++;
    if (levels > most) levels = most;
    t->levels = levels;
    uint64_t at = 0;
    for (unsigned l = 0; l < levels; l++) {
        t->level_off[l] = at;
        t->level_stride[l] = ((t->levelWidth(l) + t->bw - 1) / t->bw * t->bytes + 63) & ~63u;
        at += ((uint64_t)t->level_stride[l] * t->levelRows(l) + 63) & ~63ULL;
    }
    if (pixel_format == 260) {
        // Depth32Float_Stencil8: a stencil plane a level, after the depth levels (the one level's plane hakios_metal_
        // texture_new made had no room in the bigger BO: stencil written to a level past 0 landed on the planes)
        for (unsigned l = 0; l < levels; l++) {
            t->stencil_level_off[l] = at;
            t->stencil_level_stride[l] = (t->levelWidth(l) + 63) & ~63u;
            at += ((uint64_t)t->stencil_level_stride[l] * t->levelHeight(l) + 63) & ~63ULL;
        }
        t->stencil_off = t->stencil_level_off[0];
        t->stencil_stride = t->stencil_level_stride[0];
    }
    Bo bigger;
    if (!d->newBo(bigger, at + 32 * levels, BIND_NOEXEC)) {
        t->release();
        return nullptr;
    }
    d->freeBo(t->bo);
    t->bo = bigger;
    t->planes_off = at;
    for (unsigned l = 0; l < levels; l++) {
        auto *plane = (uint32_t *)((uint8_t *)t->bo.cpu + at + 32 * l);
        uint64_t va = t->gpu() + t->level_off[l];
        uint32_t w = t->levelWidth(l), h = t->levelHeight(l);
        memset(plane, 0, 32);
        plane[0] = planeWord0(t);
        plane[1] = t->level_stride[l] * t->levelRows(l);
        plane[2] = (uint32_t)va;
        plane[3] = (uint32_t)(va >> 32);
        plane[4] = t->level_stride[l];
        plane[6] = t->level_stride[l] * t->levelRows(l);
        plane[7] = (w - 1) | (h - 1) << 16;
    }
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_multisample(hakios_metal_device *d, unsigned pixel_format, unsigned width, unsigned height,
    unsigned samples, unsigned usage)
{
    if (samples <= 1) {
        return hakios_metal_texture_new(d, 2, pixel_format, width, height, usage);
    }
    // (11-24) 2 samples are served as 4 (the Mali-G715 has no 2x pattern; Metal promises the resolved image, which 4 give
    // at least as well): the lock-screen poster's depth attachment (MercuryPoster, Depth16Unorm x2) was refused
    if (samples == 2) {
        samples = 4;
    }
    if (samples != 4) {
        fail("a multisample texture of %u samples (the Mali-G715 has no 2x sample pattern -- Mesa serves it from v12; 4 is "
            "served)", samples);
        return nullptr;
    }
    // FOUR SAMPLES A PIXEL, LAYERED: each sample a plane of the texture's rows, one after the other (the render target's
    // and depth/stencil's "Layered" write-back, the texture descriptor's sample count with the plane as its slice
    // stride); Depth32Float_Stencil8's stencil planes after the depth planes
    hakios_metal_texture *t = hakios_metal_texture_new(d, 2, pixel_format, width, height, usage);
    if (t == nullptr) {
        return nullptr;
    }
    uint64_t plane = ((uint64_t)t->stride * t->levelRows(0) + 63) & ~63ULL;
    if (plane != (uint64_t)t->stride * t->levelRows(0)) {
        // (the planes are stride * rows apart: a texture whose plane is not a multiple of 64 bytes is not served)
        t->release();
        fail("a multisample texture of %ux%u, MTLPixelFormat %u: its sample planes would not be 64-byte aligned", width,
            height, pixel_format);
        return nullptr;
    }
    uint64_t size = plane * samples;
    if (pixel_format == 260) {
        t->stencil_off = size;
        size += (uint64_t)t->stencil_stride * height * samples;
    }
    Bo bigger;
    if (!d->newBo(bigger, size, BIND_NOEXEC)) {
        t->release();
        return nullptr;
    }
    d->freeBo(t->bo);
    t->bo = bigger;
    t->samples = samples;
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_layered(hakios_metal_device *d, unsigned type, unsigned pixel_format, unsigned width,
    unsigned height, unsigned layers, unsigned levels, unsigned usage)
{
    if (layers <= 1 && (type == 2 || type == 3)) {
        return hakios_metal_texture_new_levels(d, 2, pixel_format, width, height, levels, usage);
    }
    const bool cube = type == 5 || type == 6;
    if ((type != 1 && type != 3 && !cube) || layers == 0 || layers > 2048 || (cube && (layers % 6 != 0 || width != height)) ||
        (type == 1 && height != 1) || pixel_format == 253 || pixel_format == 260) {
        fail("a layered texture: type %u, %u layers, %u x %u, MTLPixelFormat %u (the Mali build makes 1D and 2D arrays, cubes "
            "and cube arrays up to 2048 layers; no stencil)", type, layers, width, height, pixel_format);
        return nullptr;
    }
    hakios_metal_texture *t = hakios_metal_texture_new(d, type == 1 ? 0 : 2, pixel_format, width, height, usage);
    if (t == nullptr) {
        return nullptr;
    }
    t->type = type;
    unsigned most = 1;
    while (most < 16 && ((width >> most) || (type != 1 && (height >> most)))) most++;
    if (levels == 0) levels = 1;
    if (levels > most) levels = most;
    t->levels = levels;
    uint64_t at = 0;
    for (unsigned l = 0; l < levels; l++) {
        t->level_off[l] = at;
        t->level_stride[l] = ((t->levelWidth(l) + t->bw - 1) / t->bw * t->bytes + 63) & ~63u;
        at += ((uint64_t)t->level_stride[l] * t->levelRows(l) + 63) & ~63ULL;
    }
    Bo bigger;
    if (!d->newBo(bigger, at * layers + 32ULL * levels * layers, BIND_NOEXEC)) {
        t->release();
        return nullptr;
    }
    d->freeBo(t->bo);
    t->bo = bigger;
    t->layers = layers;
    t->layer_stride = at;
    t->planes_off = at * layers;
    for (unsigned y = 0; y < layers; y++) {
        for (unsigned l = 0; l < levels; l++) {
            auto *plane = (uint32_t *)((uint8_t *)t->bo.cpu + t->planes_off + 32 * ((uint64_t)y * levels + l));
            uint64_t va = t->gpu() + at * y + t->level_off[l];
            uint32_t w = t->levelWidth(l), h = t->levelHeight(l);
            memset(plane, 0, 32);
            plane[0] = planeWord0(t);
            plane[1] = t->level_stride[l] * t->levelRows(l);
            plane[2] = (uint32_t)va;
            plane[3] = (uint32_t)(va >> 32);
            plane[4] = t->level_stride[l];
            plane[6] = t->level_stride[l] * t->levelRows(l);
            plane[7] = (w - 1) | (h - 1) << 16;
        }
    }
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_3d(hakios_metal_device *d, unsigned pixel_format, unsigned width, unsigned height,
    unsigned depth, unsigned usage)
{
    // a 2D texture's rows, DEPTH planes of them one after the other (describe(): a 3D texture, the plane's slice size)
    if (depth == 0 || depth > 2048 || width > 2048 || height > 2048) {
        fail("a 3D texture of %u x %u x %u (the Mali build makes them up to 2048 each way)", width, height, depth);
        return nullptr;
    }
    hakios_metal_texture *t = hakios_metal_texture_new(d, 2, pixel_format, width, height, usage);
    if (t == nullptr || depth == 1) {
        if (t != nullptr) t->type = 7;
        return t;
    }
    Bo bigger;
    if (!d->newBo(bigger, (uint64_t)t->stride * height * depth, BIND_NOEXEC)) {
        t->release();
        return nullptr;
    }
    d->freeBo(t->bo);
    t->bo = bigger;
    t->type = 7;
    t->depth = depth;
    return t;
}

hakios_metal_texture *
hakios_metal_texture_new_compressed(hakios_metal_device *d, unsigned pixel_format, unsigned width, unsigned height,
    unsigned usage)
{
    // (the plug-in's UBWC on the Adreno; on the Mali, AFBC is not done yet: a linear texture, read the same way)
    return hakios_metal_texture_new(d, 2, pixel_format, width, height, usage);
}

unsigned hakios_metal_texture_iosurface(hakios_metal_texture *t) { return t ? t->surface : 0; }
unsigned hakios_metal_texture_levels(hakios_metal_texture *t) { return t ? t->levels : 0; }
unsigned hakios_metal_texture_samples(hakios_metal_texture *t) { return t ? t->samples : 0; }
unsigned hakios_metal_texture_depth(hakios_metal_texture *t) { return t ? (t->layers > 1 ? t->layers : t->depth) : 0; }
unsigned hakios_metal_texture_bytes_per_pixel(hakios_metal_texture *t) { return t ? t->bytes : 0; }
unsigned hakios_metal_texture_origin(hakios_metal_texture *t) { return t ? (t->origin == 4 ? (t->parent ? hakios_metal_texture_origin(t->parent) : 0) : t->origin) : 0; }
uint64_t hakios_metal_texture_allocated_bytes(hakios_metal_texture *t) { return t ? t->bo.size : 0; }
uint64_t hakios_metal_texture_resource_id(hakios_metal_texture *t) { return (uint64_t)(uintptr_t)t; }
uint64_t hakios_metal_sampler_resource_id(hakios_metal_sampler *s) { return (uint64_t)(uintptr_t)s; }
uint64_t hakios_metal_device_allocated_bytes(hakios_metal_device *d) { (void)d; return 0; }

int
hakios_metal_format_block(unsigned pixel_format, unsigned *block_w, unsigned *block_h, unsigned *bytes)
{
    const MaliFormat *f = findFormat(pixel_format);
    if (f == nullptr) {
        fail("MTLPixelFormat %u is not served on the Mali yet", pixel_format);
        return 0;
    }
    *block_w = f->bw;
    *block_h = f->bh;
    *bytes = f->bytes;
    return 1;
}

int
hakios_metal_texture_plan_bytes(unsigned type, unsigned pixel_format, unsigned width, unsigned height, unsigned depth,
    unsigned levels, unsigned samples, unsigned usage, uint64_t *bytes)
{
    (void)usage;
    const MaliFormat *f = findFormat(pixel_format);
    // (11-12: the layered types too -- DEPTH their layers, a cube's faces among them)
    const bool layered = type == 1 || type == 3 || type == 5 || type == 6;
    unsigned layers = layered ? (depth ? depth : 1) : 1;
    if (f == nullptr || (type != 2 && type != 4 && !layered) || (!layered && depth > 1) || (samples != 1 && samples != 4) ||
        (samples > 1 && (levels > 1 || layered)) || width == 0 || height == 0) {
        fail("a texture of type %u, MTLPixelFormat %u, %u levels, %u samples (not on the Mali yet)", type, pixel_format,
            levels, samples);
        return -1;
    }
    // as hakios_metal_texture_new(_levels) lays it out: each level's rows (of blocks) 64-byte aligned, the levels' plane
    // descriptors after them
    uint64_t at = 0;
    unsigned l = 0;
    for (; l < (levels ? levels : 1) && l < 16 && (l == 0 || (width >> l) || (height >> l)); l++) {
        unsigned w = width >> l ? width >> l : 1, h = height >> l ? height >> l : 1;
        at += ((uint64_t)(((w + f->bw - 1) / f->bw * f->bytes + 63) & ~63u) * ((h + f->bh - 1) / f->bh) + 63) & ~63ULL;
    }
    if (layers > 1) at = at * layers + 32ULL * l * layers;
    else if (l > 1) at += 32 * l;
    if (samples > 1) at = at * samples + (pixel_format == 260 ? (uint64_t)((width + 63) & ~63u) * height * samples : 0);
    *bytes = (at + 0x3fff) & ~0x3fffULL;
    return 0;
}

int
hakios_metal_texture_read_level(hakios_metal_texture *t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h,
    void *bytes, uint64_t bytes_per_row)
{
    return copyRows(t, x, y, w, h, (uint8_t *)bytes, bytes_per_row, false, level);
}

int
hakios_metal_texture_replace_level(hakios_metal_texture *t, unsigned level, unsigned x, unsigned y, unsigned w,
    unsigned h, const void *bytes, uint64_t bytes_per_row)
{
    return copyRows(t, x, y, w, h, (uint8_t *)bytes, bytes_per_row, true, level);
}

int
hakios_metal_texture_read_3d(hakios_metal_texture *t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w,
    unsigned h, unsigned d, void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image)
{
    if (t != nullptr && t->layers > 1 && d == 1 && z < t->layers && level < t->levels) {
        // a layer of an array, a face of a cube (Z): that layer's level
        hakios_metal_texture *v = levelsOf(t, findFormat(t->format), t->format, t->type == 1 ? 0 : 2, level, 1, z);
        int r = copyRows(v, x, y, w, h, (uint8_t *)bytes, bytes_per_row, false);
        v->release();
        return r;
    }
    if (t != nullptr && t->type == 7 && level == 0 && d >= 1 && z + d <= t->depth && (z != 0 || d != 1)) {
        // a 3D texture's depth planes Z .. Z + D - 1, each image BYTES_PER_IMAGE further on in the caller's memory
        // (SpringBoard's 64 x 64 x 64 colour table)
        const uint64_t image = bytes_per_image ? bytes_per_image : bytes_per_row * h;
        for (unsigned k = 0; k < d; k++) {
            hakios_metal_texture *v = attachmentOf(t, 0, z + k);
            if (v == nullptr) return -1;
            int r = copyRows(v, x, y, w, h, (uint8_t *)bytes + image * k, bytes_per_row, false);
            v->release();
            if (r != 0) return r;
        }
        return 0;
    }
    if (z != 0 || d != 1) {
        fail("a 3D region of a texture (not on the Mali yet)");
        return -1;
    }
    return hakios_metal_texture_read_level(t, level, x, y, w, h, bytes, bytes_per_row);
}

int
hakios_metal_texture_replace_3d(hakios_metal_texture *t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w,
    unsigned h, unsigned d, const void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image)
{
    if (t != nullptr && t->layers > 1 && d == 1 && z < t->layers && level < t->levels) {
        // a layer of an array, a face of a cube (Z): that layer's level
        hakios_metal_texture *v = levelsOf(t, findFormat(t->format), t->format, t->type == 1 ? 0 : 2, level, 1, z);
        int r = copyRows(v, x, y, w, h, (uint8_t *)bytes, bytes_per_row, true);
        v->release();
        return r;
    }
    if (t != nullptr && t->type == 7 && level == 0 && d >= 1 && z + d <= t->depth && (z != 0 || d != 1)) {
        // a 3D texture's depth planes Z .. Z + D - 1, each image BYTES_PER_IMAGE further on in the caller's memory
        // (SpringBoard's 64 x 64 x 64 colour table)
        const uint64_t image = bytes_per_image ? bytes_per_image : bytes_per_row * h;
        for (unsigned k = 0; k < d; k++) {
            hakios_metal_texture *v = attachmentOf(t, 0, z + k);
            if (v == nullptr) return -1;
            int r = copyRows(v, x, y, w, h, (uint8_t *)bytes + image * k, bytes_per_row, true);
            v->release();
            if (r != 0) return r;
        }
        return 0;
    }
    if (z != 0 || d != 1) {
        fail("a 3D region of a texture (not on the Mali yet)");
        return -1;
    }
    return hakios_metal_texture_replace_level(t, level, x, y, w, h, bytes, bytes_per_row);
}

// the clear alone: a pass of no geometry (the fragment job writes the clear into every tile, as mali_test's clear did)
int
hakios_metal_cb_clear(hakios_metal_cb *cb, const struct hakios_metal_render_pass *pass)
{
    if (cb != nullptr && atLevels(pass)) {
        struct hakios_metal_render_pass q;
        hakios_metal_texture *views[10];
        if (!passAtLevels(pass, q, views)) return -1;
        int r = hakios_metal_cb_clear(cb, &q);
        for (auto *v : views) if (v) v->release();
        return r;
    }
    // (the surfaces it writes marked busy, as a draw's: the display waits for them -- markBusy)
    if (cb != nullptr && pass != nullptr) {
        markBusy(cb, cb->queue->device, pass->color0);
        for (unsigned i = 0; i < 7; i++) {
            markBusy(cb, cb->queue->device, pass->more[i].texture);
        }
    }
    // THE ATTACHMENTS CLEARED (Clear load action), any of the eight: four a Mali pass (the tile buffer's four kilobytes
    // a tile), each its own render target -- one not cleared is not in the pass at all
    struct Cleared { hakios_metal_texture *t; const double *color; unsigned store; };
    Cleared c[8];
    unsigned nc = 0;
    if (cb == nullptr || pass == nullptr) {
        fail("a clear without a pass");
        return -1;
    }
    // DEPTH OR STENCIL ALONE, NO COLOUR ATTACHMENT AT ALL (11-13; ANGLE's first clear of a WebGL depth buffer, refused
    // twice a page until now): a Mali fragment job has a colour target, so the pass gets one of its own -- RGBA8 of the
    // depth buffer's size, not cleared to anything that is kept, not written, gone with the command buffer
    hakios_metal_texture *stand_in = nullptr;
    if (pass->color0 == nullptr) {
        hakios_metal_texture *zs = pass->depth != nullptr ? pass->depth : pass->stencil;
        bool more = false;
        for (unsigned i = 0; i < 7; i++) more = more || pass->more[i].texture != nullptr;
        if (zs == nullptr || more) {
            fail("a clear without colour attachment 0 (%s)", more ? "others are there" : "and no depth or stencil");
            return -1;
        }
        if (!((pass->depth && pass->depth_load_action == 2) || (pass->stencil && pass->stencil_load_action == 2))) {
            return 0;                                   // nothing cleared
        }
        stand_in = hakios_metal_texture_new_levels(cb->queue->device, 2, 70, zs->width, zs->height, 1, 4 | 1);
        if (stand_in == nullptr) {
            return -1;                                  // (the reason is the texture's)
        }
        if (zs->samples > 1) {
            // (a multisampled depth buffer wants a colour target of its sample count: not served, said as it is)
            stand_in->release();
            fail("a clear of a %u-sample depth or stencil buffer alone, no colour attachment (one sample on the Mali yet)", zs->samples);
            return -1;
        }
        struct hakios_metal_render_pass q = *pass;
        q.color0 = stand_in;
        q.load_action = 0;
        q.store_action = 0;
        int r = hakios_metal_cb_clear(cb, &q);
        stand_in->release();                            // (the command buffer holds it until it is done)
        return r;
    }
    if (pass->load_action == 2) c[nc++] = { pass->color0, pass->clear_color, pass->store_action };
    for (unsigned i = 0; i < 7; i++) {
        const auto &m = pass->more[i];
        if (m.texture != nullptr && m.load_action == 2) c[nc++] = { m.texture, m.clear_color, m.store_action };
    }
    bool zs_clear = (pass->depth && pass->depth_load_action == 2) || (pass->stencil && pass->stencil_load_action == 2);
    static const double kNone[4] = { 0, 0, 0, 0 };
    if (nc == 0 && !zs_clear) {
        return 0;                                       // nothing cleared, nothing drawn: nothing to do
    }
    if (nc == 0) {
        c[nc++] = { pass->color0, kNone, 0 };           // (depth or stencil alone: colour 0 in the pass, not written)
    }
    hakios_metal_texture *t0 = c[0].t;
    for (unsigned i = 0; i < nc; i++) {
        hakios_metal_texture *t = c[i].t;
        if (!cb->queue->device->heap_ok || !drawable(t->format) || t->width != t0->width || t->height != t0->height) {
            fail("a clear of MTLPixelFormat %u, %ux%u (RGBA8/BGRA8/BGR5A1/B5G6R5 attachments of one size on the Mali yet)",
                t->format, t->width, t->height);
            return -1;
        }
    }
    auto rt1Of = [](const hakios_metal_texture *t) { uint32_t w = 0; rtFormat(t->format, w); return w; };
    for (unsigned at = 0; at < nc; at += 4) {
        DrawJob dj = {};
        hakios_metal_texture *t = c[at].t;
        dj.clear_only = true;
        dj.samples = t->samples;
        dj.target_va = t->gpu();
        dj.width = t->width;
        dj.height = t->height;
        dj.row_bytes = t->stride;
        dj.rt1 = rt1Of(t);
        dj.write = c[at].store == 1;
        dj.clear = rtClear(dj.rt1, c[at].color);
        dj.clear_high = rtClearHigh(dj.rt1, c[at].color);
        dj.blend[0] = 1u << 9;
        dj.blend[1] = 0x132 | 0x132 << 12 | 0xfu << 28;
        dj.blend[2] = 1 | 3 << 3;
        dj.blend[3] = rtBlend(dj.rt1);
        dj.rts = nc - at < 4 ? nc - at : 4;
        for (unsigned i = 1; i < dj.rts; i++) {
            DrawJob::More &m = dj.more[i - 1];
            hakios_metal_texture *u = c[at + i].t;
            if (u->samples != t->samples) {
                fail("a clear of colour attachments of different sample counts");
                return -1;
            }
            m.va = u->gpu();
            m.row_bytes = u->stride;
            m.rt1 = rt1Of(u);
            m.clear = rtClear(m.rt1, c[at + i].color);
            m.clear_high = rtClearHigh(m.rt1, c[at + i].color);
            m.write = c[at + i].store == 1;
            m.texture = u;
            m.blend[0] = 1u << 9;
            m.blend[1] = 0x132 | 0x132 << 12 | 0xfu << 28;
            m.blend[2] = 1 | 3 << 3 | i << 16;
            m.blend[3] = rtBlend(m.rt1);
        }
        if (at == 0 && zs_clear) {
            const char *why = nullptr;
            if (!zsOf(pass, dj, t->width, t->height, false, why)) {
                fail("a clear: %s", why);
                return -1;
            }
            for (hakios_metal_texture *x : { pass->depth, pass->stencil }) {
                if (x != nullptr) {
                    x->retain();
                    cb->held_textures.push_back(x);
                }
            }
        }
        dj.mode = 8;
        dj.instances = 1;
        dj.scissor_max = (t->width - 1) | (t->height - 1) << 16;
        Job j = {};
        j.shader = DRAW;
        j.dst = cb->draws.size();
        cb->draws.push_back(dj);
        cb->blits.push_back(j);
        for (unsigned i = 0; i < dj.rts; i++) {
            c[at + i].t->retain();
            cb->held_textures.push_back(c[at + i].t);
        }
    }
    return 0;
}

int
hakios_metal_cb_draw_indexed(hakios_metal_cb *cb, hakios_metal_program *program, const struct hakios_metal_render_pass *pass,
    const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive,
    unsigned index_count, unsigned index_type, hakios_metal_buffer *index_buffer, uint64_t index_offset,
    unsigned instance_count, int base_vertex, unsigned base_instance,
    hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count,
    hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count,
    hakios_metal_texture *const *fs_textures, unsigned fs_texture_count,
    hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count)
{
    IndexInfo ix = { index_buffer, index_offset, index_type, base_vertex };
    return drawImpl(cb, program, pass, viewport, scissor, primitive, 0, index_count, instance_count, base_instance,
        vs_buffers, vs_offsets, vs_count, fs_buffers, fs_offsets, fs_count, fs_textures, fs_texture_count, fs_samplers,
        fs_sampler_count, &ix);
}

int
hakios_metal_cb_refuse(hakios_metal_cb *cb, const char *why)
{
    if (cb != nullptr) {
        if (cb->skip_refused) {
            cb->skipped++;
            cb->skip_reason = why ? why : "";
        } else {
            hakios_metal_cb_fail(cb, why ? why : "refused");
        }
    }
    return -1;
}

void hakios_metal_cb_skip_refused(hakios_metal_cb *cb, int on) { if (cb) cb->skip_refused = on != 0; }
extern "C++" { namespace mali {
hakios_metal_program *
blitProgram(hakios_metal_device *d)
{
    std::lock_guard<std::mutex> g(d->preload_lock);
    if (d->blit_prog != nullptr || d->blit_failed) {
        if (d->blit_prog == nullptr) fail("the driver's blit draw could not be built");
        return d->blit_prog;
    }
    auto *p = new hakios_metal_program;
    char why[256] = "";
    if (mali_blit_compile(&p->vs, &p->fs, why, sizeof(why)) != 0 || !p->upload(d)) {
        fail("the driver's blit draw: %s", why[0] ? why : gError.c_str());
        d->blit_failed = true;
        p->release();
        return nullptr;
    }
    snprintf(p->name, sizeof(p->name), "hakios_blit");
    d->blit_prog = p;
    return p;
}

unsigned linearOf(unsigned f) { return f == 71 ? 70 : f == 81 ? 80 : f; }

hakios_metal_program *
resolveProgram(hakios_metal_device *d)
{
    std::lock_guard<std::mutex> g(d->preload_lock);
    if (d->resolve_prog != nullptr || d->resolve_failed) {
        if (d->resolve_prog == nullptr) fail("the driver's resolve draw could not be built");
        return d->resolve_prog;
    }
    auto *p = new hakios_metal_program;
    char why[256] = "";
    if (mali_resolve_compile(&p->vs, &p->fs, 4, why, sizeof(why)) != 0 || !p->upload(d)) {
        fail("the driver's resolve draw: %s", why[0] ? why : gError.c_str());
        d->resolve_failed = true;
        p->release();
        return nullptr;
    }
    snprintf(p->name, sizeof(p->name), "hakios_resolve4");
    d->resolve_prog = p;
    return p;
}

// A MULTISAMPLE RESOLVE (MTLStoreActionMultisampleResolve, which the Metal side turns into this at -endEncoding): the
// SW x SH pixels at (SX, SY) of FROM (4 samples) averaged into the same size at (DX, DY) of TO's level TL, drawn
int
resolveDraw(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned sx, unsigned sy, hakios_metal_texture *to,
    unsigned tl, unsigned dx, unsigned dy, unsigned w, unsigned h)
{
    hakios_metal_device *d = cb->queue->device;
    hakios_metal_program *rp = resolveProgram(d);
    if (rp == nullptr) return -1;
    hakios_metal_texture *dst = levelsOf(to, findFormat(to->format), to->format, 2, tl, 1);
    dst->depth = 1;
    dst->usage |= HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET;
    hakios_metal_sampler smp;
    memset(smp.words, 0, sizeof(smp.words));
    smp.words[0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 23 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
    struct hakios_metal_render_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.color0 = dst;
    pass.load_action = dx == 0 && dy == 0 && w == dst->width && h == dst->height ? 0 : 1;
    pass.store_action = 1;
    struct hakios_metal_viewport vp = { (double)dx, (double)dy, (double)w, (double)h, 0, 1 };
    struct hakios_metal_scissor sc = { dx, dy, w, h };
    uint32_t words[2] = { (uint32_t)((int)sx - (int)dx), (uint32_t)((int)sy - (int)dy) };
    hakios_metal_texture *texs[1] = { from };
    hakios_metal_sampler *smps[1] = { &smp };
    int r = drawImpl(cb, rp, &pass, &vp, &sc, 3, 0, 3, 1, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, texs, 1, smps, 1,
        nullptr, words, 2);
    dst->release();
    return r;
}

// a draw of FROM's level FL into the DW x DH rectangle at (DX, DY) of TO's level TL, the destination pixel (x, y) (its
// centre) sampled at normalized (x m0 + y m1 + m2, x m3 + y m4 + m5) of the source level
int
blitDraw(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned fl, hakios_metal_texture *to, unsigned tl, unsigned dx,
    unsigned dy, unsigned dw, unsigned dh, const float m[6], bool linear, bool raw)
{
    hakios_metal_device *d = cb->queue->device;
    hakios_metal_program *bp = blitProgram(d);
    if (bp == nullptr) return -1;
    // the two levels as textures of their own; a byte copy reads and writes them without sRGB conversion
    unsigned ff = raw ? linearOf(from->format) : from->format, tf = raw ? linearOf(to->format) : to->format;
    hakios_metal_texture *src = levelsOf(from, findFormat(ff), ff, from->type == 7 ? 2 : from->type, fl, 1);
    hakios_metal_texture *dst = levelsOf(to, findFormat(tf), tf, 2, tl, 1);
    src->depth = dst->depth = 1;
    dst->usage |= HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET;   // (the Mali draws into any RGBA8 texture)
    hakios_metal_sampler smp;
    memset(smp.words, 0, sizeof(smp.words));
    smp.words[0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 23 | 1u << 25 | 1u << 26 |
        (linear ? 0u : 1u << 27 | 1u << 28);
    struct hakios_metal_render_pass pass;
    memset(&pass, 0, sizeof(pass));
    pass.color0 = dst;
    pass.load_action = dx == 0 && dy == 0 && dw == dst->width && dh == dst->height ? 0 : 1;
    pass.store_action = 1;
    struct hakios_metal_viewport vp = { (double)dx, (double)dy, (double)dw, (double)dh, 0, 1 };
    struct hakios_metal_scissor sc = { dx, dy, dw, dh };
    uint32_t words[6];
    memcpy(words, m, sizeof(words));
    hakios_metal_texture *texs[1] = { src };
    hakios_metal_sampler *smps[1] = { &smp };
    int r = drawImpl(cb, bp, &pass, &vp, &sc, 3, 0, 3, 1, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, texs, 1, smps, 1,
        nullptr, words, 6);
    src->release();
    dst->release();
    return r;
}
} }  // namespace mali, extern "C++"

int
hakios_metal_cb_blit(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned sx, unsigned sy,
    unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned dx, unsigned dy, unsigned dw, unsigned dh,
    int linear)
{
    return blitLayers(cb, from, from_level, 0, sx, sy, sw, sh, to, to_level, 0, dx, dy, dw, dh, linear);
}

int
hakios_metal_cb_blit_layer(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned from_layer,
    unsigned sx, unsigned sy, unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned to_layer,
    unsigned dx, unsigned dy)
{
    return blitLayers(cb, from, from_level, from_layer, sx, sy, sw, sh, to, to_level, to_layer, dx, dy, sw, sh, 0);
}

extern "C++" int
blitLayers(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned from_layer, unsigned sx, unsigned sy,
    unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned to_layer, unsigned dx, unsigned dy,
    unsigned dw, unsigned dh, int linear)
{
    { hakios_metal_texture *rt_ = (from); while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent; if (rt_ != nullptr) rt_->reads++; }
    const char *why = nullptr;
    if (cb != nullptr && to != nullptr) {
        markBusy(cb, cb->queue->device, to);    // (a copy into a surface: the display waits for it too, as for a draw)
    }
    if (cb == nullptr || from == nullptr || to == nullptr) why = "a texture copy without its two textures";
    else if (from_layer >= (from->layers > 1 ? from->layers : 1u) || to_layer >= (to->layers > 1 ? to->layers : 1u))
        why = "a texture copy of a layer the texture has not got";
    else if ((from_layer != 0 || to_layer != 0) && (from->samples > 1 || to->samples > 1))
        why = "a copy between layers of multisample textures";
    else if (from->samples > 1 && to->samples == 1) {
        // a resolve: pixel for pixel, a colour format into an RGBA8/BGRA8 one
        if (from_level != 0 || sw != dw || sh != dh) why = "a multisample resolve other than pixel for pixel";
        else if (from->format == 250 || from->format == 252 || from->format == 253 || from->format == 260)
            why = "a depth/stencil multisample resolve (not on the Mali yet)";
        else if (!drawable(to->format)) why = "a multisample resolve into a format other than RGBA8/BGRA8/BGR5A1/B5G6R5";
        else if (to_level >= to->levels || sx + sw > from->width || sy + sh > from->height ||
                 dx + dw > to->levelWidth(to_level) || dy + dh > to->levelHeight(to_level))
            why = "a multisample resolve outside a texture";
        else if (sw == 0 || sh == 0) return 0;
        else if (resolveDraw(cb, from, sx, sy, to, to_level, dx, dy, dw, dh) != 0) {
            hakios_metal_cb_fail(cb, gError.c_str());
            return -1;
        } else return 0;
    }
    else if (from->samples > 1 || to->samples > 1) why = "a copy into or between multisample textures (not on the Mali yet)";
    else if (from_level >= from->levels || to_level >= to->levels) why = "a texture copy of a level the texture has not got";
    else if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return 0;
    else if (sx + sw > from->levelWidth(from_level) || sy + sh > from->levelHeight(from_level) ||
             dx + dw > to->levelWidth(to_level) || dy + dh > to->levelHeight(to_level))
        why = "a texture copy outside a texture";
    if (why != nullptr) {
        fail("%s", why);
        if (cb) hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    bool same = sw == dw && sh == dh;
    bool blocks = from->bw > 1 || from->bh > 1 || to->bw > 1 || to->bh > 1;
    if (blocks && !same && (to->bw > 1 || to->bh > 1)) {
        fail("a scaled copy into a compressed texture (MTLPixelFormat %u)", to->format);
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    blocks = blocks && same;                            // (a scale from a compressed one: drawn, the GPU decodes it)
    if (blocks) {
        // a compressed format: copied block for block, between two of one block size, on block boundaries
        if (!same || from->bw != to->bw || from->bh != to->bh || from->bytes != to->bytes || sx % from->bw || sy % from->bh ||
            dx % to->bw || dy % to->bh) {
            fail("a copy of a compressed texture other than block for block (MTLPixelFormat %u into %u)", from->format,
                to->format);
            hakios_metal_cb_fail(cb, gError.c_str());
            return -1;
        }
        sx /= from->bw; dx /= from->bw; sw = (sw + from->bw - 1) / from->bw;
        sy /= from->bh; dy /= from->bh; sh = (sh + from->bh - 1) / from->bh;
    }
    if (same && from->bytes == to->bytes && (blocks || !drawable(to->format) || sh <= 16 || from_layer != 0 || to_layer != 0)) {
        // A BYTE COPY: one compute blit for rows that follow each other in both, else one a row
        uint64_t src = from->gpu() + from->layer_stride * from_layer + from->levelOffset(from_level);
        uint64_t dst = to->gpu() + to->layer_stride * to_layer + to->levelOffset(to_level);
        uint32_t ss = from->levelStride(from_level), ds = to->levelStride(to_level), row = sw * from->bytes;
        bool whole = sx == 0 && dx == 0 && ss == ds && row == ss;
        unsigned jobs = whole ? 1 : sh;
        if (jobs > 256) {
            fail("a texture copy of %u rows of MTLPixelFormat %u (one a row: a copy that big is drawn, RGBA8/BGRA8 only)",
                sh, to->format);
            hakios_metal_cb_fail(cb, gError.c_str());
            return -1;
        }
        for (unsigned r = 0; r < jobs; r++) {
            uint64_t s = src + (uint64_t)(sy + r) * ss + (uint64_t)sx * from->bytes;
            uint64_t t = dst + (uint64_t)(dy + r) * ds + (uint64_t)dx * to->bytes;
            uint64_t n = whole ? (uint64_t)row * sh : row;
            bool w = ((s | t | n) & 3) == 0;
            Job j = {};
            j.shader = w ? 0u : 2u;
            j.dst = t;
            j.src = s;
            j.count = (uint32_t)(w ? n / 4 : n);
            cb->blits.push_back(j);
        }
        from->retain();
        cb->held_textures.push_back(from);
        to->retain();
        cb->held_textures.push_back(to);
        return 0;
    }
    if (from_layer != 0 || to_layer != 0) {
        fail("a texture copy from layer %u into layer %u that is not one of bytes (%ux%u of MTLPixelFormat %u into %ux%u of %u: "
            "scaled, or between pixel sizes, is drawn, and a draw takes the first layer only)", from_layer, to_layer,
            sw, sh, from->format, dw, dh, to->format);
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    if (!drawable(to->format) || (same && from->bytes != to->bytes)) {
        fail("a %s texture copy from MTLPixelFormat %u into %u (into RGBA8/BGRA8 only on the Mali yet)", same ? "" : "scaled",
            from->format, to->format);
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    // A DRAW: destination pixel x (centre) -> source x' = sx + (x - dx) * sw / dw, normalized by the source level's width
    float W = (float)from->levelWidth(from_level), H = (float)from->levelHeight(from_level);
    float ku = (float)sw / dw / W, kv = (float)sh / dh / H;
    float m[6] = { ku, 0, (float)sx / W - dx * ku, 0, kv, (float)sy / H - dy * kv };
    if (blitDraw(cb, from, from_level, to, to_level, dx, dy, dw, dh, m, linear && !same, same) != 0) {
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    return 0;
}

int
hakios_metal_cb_blit_rotated(hakios_metal_cb *cb, hakios_metal_texture *from, hakios_metal_texture *to, unsigned rotation)
{
    { hakios_metal_texture *rt_ = (from); while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent; if (rt_ != nullptr) rt_->reads++; }
    // destination pixel (x, y) of TO (w x h) from FROM (W x H), normalized: 1 a quarter turn (TO's sides FROM's swapped):
    // u = y / H... each case the source position of the destination's, as a6xx_rotation turns
    if (cb != nullptr && to != nullptr) {
        markBusy(cb, cb->queue->device, to);    // (as hakios_metal_cb_blit's)
    }
    if (cb == nullptr || from == nullptr || to == nullptr || rotation > 5 || !drawable(to->format)) {
        fail("a rotated texture blit (%u) into MTLPixelFormat %u (RGBA8/BGRA8, rotations 0 to 5)", rotation, to ? to->format : 0);
        if (cb) hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    float W = (float)from->width, H = (float)from->height, w = (float)to->width, h = (float)to->height;
    bool swap = rotation == 1 || rotation == 3;
    if ((swap ? (to->width != from->height || to->height != from->width) : (to->width != from->width || to->height != from->height))) {
        fail("a rotated texture blit (%u) between textures of %ux%u and %ux%u", rotation, from->width, from->height, to->width,
            to->height);
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    float m[6];
    switch (rotation) {
    case 0: { float a[6] = { 1 / W, 0, 0, 0, 1 / H, 0 }; memcpy(m, a, sizeof(m)); break; }
    case 1: { float a[6] = { 0, 1 / W, 0, -1 / H, 0, w / H }; memcpy(m, a, sizeof(m)); break; }      // 90 clockwise
    case 2: { float a[6] = { -1 / W, 0, 1, 0, -1 / H, 1 }; memcpy(m, a, sizeof(m)); break; }         // 180
    case 3: { float a[6] = { 0, -1 / W, h / W, 1 / H, 0, 0 }; memcpy(m, a, sizeof(m)); break; }      // 270
    case 4: { float a[6] = { -1 / W, 0, 1, 0, 1 / H, 0 }; memcpy(m, a, sizeof(m)); break; }          // flip left-right
    default: { float a[6] = { 1 / W, 0, 0, 0, -1 / H, 1 }; memcpy(m, a, sizeof(m)); break; }         // flip top-bottom
    }
    if (blitDraw(cb, from, 0, to, 0, 0, 0, to->width, to->height, m, false, true) != 0) {
        hakios_metal_cb_fail(cb, gError.c_str());
        return -1;
    }
    return 0;
}

int
hakios_metal_cb_append(hakios_metal_cb *cb, hakios_metal_cb *other)
{
    (void)cb; (void)other;
    fail("a command buffer appended to another (not on the Mali yet)");
    return -1;
}

int
hakios_metal_cb_generate_mipmaps(hakios_metal_cb *cb, hakios_metal_texture *t)
{
    if (t == nullptr || t->levels <= 1) {
        return 0;                                       // one level: nothing to make
    }
    // A LAYERED TEXTURE (a cube, an array): each layer by itself, through a 2D view of it (11-13: only layer 0's levels
    // were made -- a cube's other five faces had level 0 and nothing below; ANGLE's skybox in the WebGL aquarium)
    if (t->layers > 1) {
        for (unsigned layer = 0; layer < t->layers; layer++) {
            hakios_metal_texture *v = hakios_metal_texture_new_view(t, t->format, 2, 0, t->levels, layer, 1);
            if (v == nullptr) {
                return -1;
            }
            int r = hakios_metal_cb_generate_mipmaps(cb, v);
            v->release();                               // (the command buffer holds what its copies name)
            if (r != 0) {
                return -1;
            }
        }
        return 0;
    }
    // each level from the one above, halved with linear filtering
    for (unsigned l = 1; l < t->levels; l++) {
        if (hakios_metal_cb_blit(cb, t, l - 1, 0, 0, t->levelWidth(l - 1), t->levelHeight(l - 1), t, l, 0, 0, t->levelWidth(l),
                t->levelHeight(l), 1) != 0) {
            return -1;
        }
    }
    return 0;
}

int
hakios_metal_cb_dispatch_indirect(hakios_metal_cb *cb, hakios_metal_kernel *kernel, hakios_metal_buffer *args,
    uint64_t offset, const unsigned threads[3], hakios_metal_buffer *const *buffers, const uint64_t *offsets,
    unsigned count, hakios_metal_texture *const *textures, unsigned texture_count, hakios_metal_sampler *const *samplers,
    unsigned sampler_count)
{
    (void)kernel; (void)args; (void)offset; (void)threads; (void)buffers; (void)offsets; (void)count; (void)textures;
    (void)texture_count; (void)samplers; (void)sampler_count;
    fail("an indirect dispatch (not on the Mali yet)");
    if (cb) hakios_metal_cb_fail(cb, gError.c_str());
    return -1;
}

hakios_metal_program *
hakios_metal_program_specialize(hakios_metal_program *base, const void *fs_bitcode, size_t fs_size,
    const hakios_metal_constant *fs_constants, unsigned fs_count, unsigned binding, const void *bytes, size_t length)
{
    // AN UBER SHADER SPECIALISED FOR ITS BUFFER'S BYTES (11-11): CoreAnimation draws widgets and cross-fades with its
    // generic fixed_frag_lph_cpf -- 160 KB of code, 64 registers, 144 bytes spilled a thread, ~30 cycles a pixel (a
    // 423x423 widget's 4 draws: 3.5 ms) -- whose path every fragment takes by a small constant buffer. HakiosMetalCA asks
    // for a program per content of that buffer (hakiosProgramFor:, in the background): the base's pipeline state with
    // the fragment stage compiled for those bytes (compile_mali.c's fold_known) -- 256 bytes to a few KB of code.
    if (base == nullptr || fs_bitcode == nullptr || fs_size == 0 || bytes == nullptr || length == 0 || length > 4096) {
        fail("a specialisation needs the program, its fragment function's bitcode and the buffer's bytes");
        return nullptr;
    }
    auto *p = new hakios_metal_program;
    {
        std::lock_guard<std::mutex> g(base->lock);
        p->color_format = base->color_format;
        p->samples = base->samples;
        p->blend_enabled = base->blend_enabled;
        p->vertex_descriptor = base->vertex_descriptor;
        p->blend_equation = base->blend_equation;
        p->vs_bitcode = base->vs_bitcode;
        p->vs_constants = base->vs_constants;
        p->fetch = base->fetch;
        p->fetch_described = base->fetch_described;
        p->shader_blend = base->shader_blend;
        for (unsigned i = 0; i < 4; i++) p->rt_blend_on[i] = base->rt_blend_on[i];
        for (unsigned i = 0; i < 4; i++) p->rt_format[i] = base->rt_format[i];
        p->blend_reads_dest = base->blend_reads_dest;
        p->blend_refusal = base->blend_refusal;
        for (unsigned i = 0; i < 3; i++) {
            p->more_blend_equation[i] = base->more_blend_equation[i];
            p->more_blend_reads_dest[i] = base->more_blend_reads_dest[i];
            p->more_blend_refusal[i] = base->more_blend_refusal[i];
        }
        memcpy(p->name, base->name, sizeof(p->name));
        memcpy(p->label, base->label, sizeof(p->label));
    }
    if (p->vs_bitcode.empty()) {
        fail("a specialisation of a program without its vertex function's bitcode");
        p->release();
        return nullptr;
    }
    p->fs_bitcode.assign((const uint8_t *)fs_bitcode, (const uint8_t *)fs_bitcode + fs_size);
    p->fs_constants.resize(fs_count);
    for (unsigned i = 0; i < fs_count; i++) {
        p->fs_constants[i].index = fs_constants[i].index;
        p->fs_constants[i].bits = fs_constants[i].bits;
    }
    p->known_binding = (int)binding;
    p->known.assign((const uint8_t *)bytes, (const uint8_t *)bytes + length);
    // both stages (the vertex stage again: the fragment stage is compiled against its varyings, which no cache keeps),
    // as compileFetch would at the first draw -- here, in the caller's background, and then not again
    bool blended = p->shader_blend.mask != 0 || p->shader_blend.format[0] || p->shader_blend.format[1] ||
        p->shader_blend.format[2] || p->shader_blend.format[3];   // (a float target's format: the fetch's conversion)
    // (11-23) a program without a fragment function (depth or stencil only, hakios_depth_only_fs) writes no colour: its
    // targets' formats do not matter -- iOS 27's path_edges_vertex draws, an RG32Float target in the pass, were refused
    if (p->fs_bitcode.empty()) {
        blended = false;
    }
    mali_kernel_binary vs = {}, fs = {};
    char why[512] = "";
    uint64_t key = cacheKey(p->vs_bitcode.data(), p->vs_bitcode.size(), p->vs_constants, p->fs_bitcode.data(),
                            p->fs_bitcode.size(), keyedConstants(p, p->fs_constants), p->color_format,
                            p->fetch_described ? &p->fetch : nullptr, blended ? &p->shader_blend : nullptr);
    if (!cacheLoad(key, &vs, &fs)) {
        KnownNow known(p);
        int r = p->fetch_described ?
            mali_vertex_compile(p->vs_bitcode.data(), p->vs_bitcode.size(), p->vs_constants.data(), p->vs_constants.size(),
                                &p->fetch, &vs, why, sizeof(why)) :
            mali_stage_compile(p->vs_bitcode.data(), p->vs_bitcode.size(), p->vs_constants.data(), p->vs_constants.size(), 0,
                               nullptr, &vs, why, sizeof(why));
        if (r != 0 ||
            mali_fragment_compile(p->fs_bitcode.data(), p->fs_bitcode.size(), p->fs_constants.data(), p->fs_constants.size(),
                                  &vs, blended ? &p->shader_blend : nullptr, &fs, why, sizeof(why)) != 0) {
            mali_kernel_binary_free(&vs);
            mali_kernel_binary_free(&fs);
            fail("the specialised stages: %s", why);
            p->release();
            return nullptr;
        }
        cacheStore(key, vs, fs);
    }
    p->vs = vs;
    p->fs = fs;
    p->fetch_compiled = true;           // (compileFetch: nothing left to do)
    return p;
}

int
hakios_metal_argument_buffer_layout(const void *bitcode, size_t size, unsigned set, unsigned *ids, unsigned *offsets,
    unsigned max)
{
    (void)bitcode; (void)size; (void)set; (void)ids; (void)offsets; (void)max;
    fail("argument buffers (not on the Mali yet)");
    return -1;
}

void hakios_metal_cb_set_vertex_textures(hakios_metal_cb *cb, hakios_metal_texture *const *textures, unsigned texture_count,
    hakios_metal_sampler *const *samplers, unsigned sampler_count)
{
    // (11-13) kept for the draws that follow; a draw takes what its vertex function declares. VectorKit binds them for
    // every map draw: refused here, every command buffer of Maps and of Weather's radar map failed without a word and
    // the map stayed black (the tester's phone; night13/maps4)
    if (cb == nullptr) return;
    cb->vs_texture_count = texture_count < MALI_KERNEL_TEXTURES ? texture_count : MALI_KERNEL_TEXTURES;
    cb->vs_sampler_count = sampler_count < MALI_KERNEL_TEXTURES ? sampler_count : MALI_KERNEL_TEXTURES;
    for (unsigned i = 0; i < MALI_KERNEL_TEXTURES; i++) {
        cb->vs_textures[i] = i < cb->vs_texture_count ? textures[i] : nullptr;
        cb->vs_sampler_set[i] = i < cb->vs_sampler_count && samplers[i] != nullptr;
        if (cb->vs_sampler_set[i]) memcpy(cb->vs_samplers[i], samplers[i]->words, 32);
    }
}
void hakios_metal_cb_set_indirect(hakios_metal_cb *cb, hakios_metal_buffer *args, uint64_t offset)
{
    (void)offset;
    if (cb && args) {
        hakios_metal_cb_fail(cb, "an indirect draw (not on the Mali yet)");
    }
}
void hakios_metal_cb_set_null_fs_textures(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_cb_set_gmem(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_cb_set_gmem_binning(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_cb_set_gmem_min_draws(hakios_metal_cb *cb, unsigned draws) { (void)cb; (void)draws; }
void hakios_metal_cb_set_gmem_off(hakios_metal_cb *cb, unsigned bits) { (void)cb; (void)bits; }
void hakios_metal_cb_set_gmem_tuned(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_cb_set_profile(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_cb_set_profile_draws(hakios_metal_cb *cb, int on) { (void)cb; (void)on; }
void hakios_metal_queue_set_deferred(hakios_metal_queue *q, int on) { (void)q; (void)on; }
void hakios_metal_cb_gmem_counts(hakios_metal_cb *cb, unsigned *passes, unsigned *tiles)
{
    (void)cb;
    if (passes) *passes = 0;
    if (tiles) *tiles = 0;
}
const char *hakios_metal_cb_gmem_refusal(hakios_metal_cb *cb) { (void)cb; return "no GMEM on a Mali"; }
const char *hakios_metal_cb_pass_sysmem_why(hakios_metal_cb *cb, unsigned i) { (void)cb; (void)i; return "a Mali: tiled by itself"; }
int hakios_metal_cb_pass_tiles(hakios_metal_cb *cb, unsigned i) { (void)cb; (void)i; return -1; }
int hakios_metal_cb_pass_attachments(hakios_metal_cb *cb, unsigned i, unsigned *attached, unsigned *touched)
{
    (void)cb; (void)i; (void)attached; (void)touched;
    return -1;
}
int hakios_metal_cb_pass_time(hakios_metal_cb *cb, unsigned i, unsigned *kind, unsigned *width, unsigned *height,
    unsigned *draws, double *us)
{
    (void)cb; (void)i; (void)kind; (void)width; (void)height; (void)draws; (void)us;
    return -1;
}
int hakios_metal_cb_draw_time(hakios_metal_cb *cb, unsigned i, const char **name, double *us)
{
    (void)cb; (void)i; (void)name; (void)us;
    return -1;
}
int hakios_metal_cb_draw_place(hakios_metal_cb *cb, unsigned i, unsigned *pass, unsigned area[4])
{
    (void)cb; (void)i; (void)pass; (void)area;
    return -1;
}
void hakios_metal_cb_counts(hakios_metal_cb *cb, unsigned *passes, unsigned *draws, unsigned *dispatches, unsigned *copies,
    unsigned *clears)
{
    unsigned p = 0, dr = 0, di = 0, co = 0, cl = 0;
    if (cb != nullptr) {
        for (const Job &j : cb->blits) {
            if (j.shader == DRAW) {
                (cb->draws[j.dst].clear_only ? cl : dr)++;
                p++;
            } else if (j.shader == DISPATCH) {
                di++;
            } else {
                co++;
            }
        }
    }
    if (passes) *passes = p;
    if (draws) *draws = dr;
    if (dispatches) *dispatches = di;
    if (copies) *copies = co;
    if (clears) *clears = cl;
}
void hakios_metal_buffer_counts(unsigned *made, unsigned *reused, unsigned long long *bytes)
{
    if (made) *made = 0;
    if (reused) *reused = 0;
    if (bytes) *bytes = 0;
}
}  // extern "C"

hakios_metal_texture *
nothingTexture(hakios_metal_device *d)
{
    std::lock_guard<std::mutex> g(d->scratch_lock);
    if (d->nothing_texture == nullptr) {
        d->nothing_texture = hakios_metal_texture_new(d, 2, 70, 4, 4, 3 | HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
        if (d->nothing_texture != nullptr) {
            memset(d->nothing_texture->pixels(), 0, (size_t)d->nothing_texture->stride * 4);
        }
    }
    return d->nothing_texture;
}
