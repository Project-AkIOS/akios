// mali_formats.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

namespace mali {

void
fail(const char *format, ...)
{
    char text[256];
    va_list ap;

    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    gError = text;
}
}  // namespace mali
uint64_t
nothingVa(hakios_metal_device *d)
{
    if (d->nothing_va.load() == 0) {
        std::lock_guard<std::mutex> g(d->scratch_lock);
        if (d->nothing_va.load() == 0 && d->newBo(d->nothing_bo, 0x10000, BIND_NOEXEC)) {
            memset(d->nothing_bo.cpu, 0, 0x10000);
            d->nothing_va = d->nothing_bo.va;
        }
    }
    gNothingVa = d->nothing_va.load();     // (this thread's draw: this device's zeroes)
    return d->nothing_va.load();
}
namespace mali {
// DEVICE's buffer B at least SIZE bytes (the old one kept until the device closes); false with the reason
bool
grow(hakios_metal_device *d, Bo &b, uint64_t size)
{
    if (b.size >= size) {
        return true;
    }
    Bo bigger;
    if (!d->newBo(bigger, size, BIND_NOEXEC)) {
        return false;
    }
    std::lock_guard<std::mutex> g(d->lock);
    if (b.handle != 0) {
        d->retired.push_back(b);
    }
    b = bigger;
    return true;
}
}  // namespace mali

void
hakios_metal_texture::release()
{
    if (refs.fetch_sub(1) != 1) {
        return;
    }
    if (origin == 0) {
        device->freeBo(bo);
    } else if (origin == 1) {
        if (mirrored) {
            mirrorRelease(device, mirror_key);
        }
        device->surfaceRelease(surface);
    }
    if (parent != nullptr) {
        parent->release();
    }
    if (buffer != nullptr) {
        buffer->release();
    }
    if (hakios_metal_texture *s = stand_in.load()) {
        s->release();
    }
    delete this;
}
namespace mali {
bool
rtFormat(unsigned f, uint32_t &rt1)
{
    uint32_t wb;
    bool bgra, srgb = f == 71 || f == 81, flt = false, raw = false, wide = false, integer = false;
    switch (f) {
    case 70: case 71: wb = 19; bgra = false; break;     // R8G8B8A8
    case 80: case 81: wb = 19; bgra = true; break;
    case 43: wb = 28; bgra = true; break;               // BGR5A1Unorm: R5G5B5A1
    case 40: wb = 21; bgra = true; break;               // B5G6R5Unorm: R5G6B5
    // (11-12, what widgets draw into once every process has the device: one and two 8-bit channels, the tile
    // buffer's RGBA8 written back as its first channel(s) -- panfrost's blendable R8_UNORM and R8G8_UNORM)
    case 10: wb = 16; bgra = false; break;              // R8Unorm: R8
    case 30: wb = 17; bgra = false; break;              // RG8Unorm: R8G8
    // HALF FLOATS (11-12: the widgets' accumulators, chronod): the tile buffer holds them as they are -- the internal
    // format R16F / RG16F (16, 17: four bytes a pixel like the rest, so the tile's kilobyte a target stands), the
    // write-back's "float colour format" of the same numbers, the blend descriptor's memory format the pixel
    // format's (rtBlend). kRtFloat marks the word for emitDraw, which takes it out again.
    case 25: wb = 16; bgra = false; flt = true; break;  // R16Float
    case 65: wb = 17; bgra = false; flt = true; break;  // RG16Float
    // R32Float (11-12 night: WebKit's GPU process draws its paths' coverage into one): no tile-buffer format of its
    // own -- raw, 32 bits as they are (internal RAW32, written back as RAW32; Mesa's get_rt_formats for a format that
    // is not blendable). Opaque writes only: the fixed-function blend does not know such a target.
    case 55: wb = 3; bgra = false; raw = true; break;
    // RG32Float (11-23: what iOS 27's vector paths -- Setup's text and shapes, path_exterior/path_interior, plane+spill --
    // draw their second colour attachment into): raw as R32Float, 64 bits a pixel -- internal RAW64 (35), written back as
    // RAW64 (5), eight bytes in the tile buffer like RGBA16Float (kRtWide: two kilobytes), its clear red's and green's
    // floats as they are (rtClear, rtClearHigh); opaque writes only, blending in the shader (rawBlend)
    case 105: wb = 5; bgra = false; raw = true; wide = true; break;
    // RG16Uint (11-23: iOS 27's glass "brim" passes in backboardd, brim_init_lph and its clears): raw as R32Float, 32
    // bits a pixel, the blend descriptor's memory format RG16UI (rtBlend) so the hardware packs the shader's two
    // integers; its clear the two 16-bit integers (rtClear). Integer targets do not blend.
    case 63: wb = 3; bgra = false; raw = true; integer = true; break;
    // RGBA16Float (11-13: HDR and wide-colour content): the tile buffer's RGBA16F (internal format 18, written back as
    // the float colour format 19), EIGHT bytes a pixel -- two of the tile's kilobytes (rtKilobytes: the targets after it
    // start further on, the framebuffer's allocation counts it), its clear two words (rtClearHigh)
    case 115: wb = 19; bgra = false; flt = true; wide = true; break;
    // RG11B10Float (11-13; not run on the phone yet): the tile buffer's RG11F_B10F (internal format 20, the float
    // colour format 20 -- Mesa's BFMT_FLOAT(R11G11B10_FLOAT, RG11F_B10F) for this architecture), four bytes a pixel
    // like the rest; the blend's memory format the texture's (89), its clear the packed word (rtClear)
    case 92: wb = 20; bgra = false; flt = true; break;
    default: return false;
    }
    uint32_t swizzle = bgra ? (2 | 1 << 3 | 0 << 6 | 3 << 9) : (0 | 1 << 3 | 2 << 6 | 3 << 9);
    rt1 = wb << 3 | 2u << 8 | (srgb ? 1u << 14 : 0) | swizzle << 16 | (flt ? kRtFloat : 0u) | (raw ? kRtRaw : 0u) |
        (wide ? kRtWide : 0u) | (integer ? kRtInt : 0u);
    return true;
}
bool drawable(unsigned f) { uint32_t w; return rtFormat(f, w); }

// a plane descriptor's first word: "Generic Plane" (clump RAW8..RAW128) or "ASTC 2D Plane" (block size; decoded wide,
// to RGBA16F, unless sRGB), linear
uint32_t
planeWord0(const hakios_metal_texture *t)
{
    if (t->bw > 1 || t->bh > 1) {
        bool srgb = (t->mali_format >> 20) & 1;
        return 10 | 4 << 4 | 2 << 8 | (srgb ? 0u : 1u << 25) | astcDim(t->bw) << 26 | astcDim(t->bh) << 29;
    }
    uint32_t clump = t->bytes == 1 ? 0 : t->bytes == 2 ? 1 : t->bytes == 4 ? 2 : t->bytes == 8 ? 3 : 4;
    return 10 | 1 << 4 | 2 << 8 | clump << 24;
}

// a texture's descriptor (genxml v10 "Texture", 32 bytes) and its plane's ("Generic Plane"), the plane at PLANE_VA
void
describe(const hakios_metal_texture *t, uint32_t *tex, uint32_t *plane, uint64_t plane_va)
{
    bool astc = t->bw > 1 || t->bh > 1;
    memset(tex, 0, 32);
    memset(plane, 0, 32);
    // a texture: 2D, 3D (MTLTextureType3D 7) or 1D (MTLTextureType1D 0, one row)
    // (a cube or cube array: dimension 0, its faces layers; a 1D array: 1D)
    const bool cube = t->type == 5 || t->type == 6;
    tex[0] = 2 | (t->type == 7 ? 3u : cube ? 0u : t->type == 0 || t->type == 1 ? 1u : 2u) << 4 | t->mali_format << 10;
    tex[1] = (t->width - 1) | (t->height - 1) << 16;
    tex[2] = t->swizzle ? t->swizzle : kIdentitySwizzle;                    // one level, from level 0
    if (astc) tex[2] |= 1u << 12;                                           // texel interleave (compressed, as Mesa)
    if ((t->levels > 1 || t->layers > 1) && t->planes_off != 0) {
        // every level: the plane descriptors the texture keeps (levels - 1, and the maximum LOD as 8.8 fixed point)
        tex[2] |= (t->levels - 1) << 16;
        tex[3] = ((t->levels - 1) * 256) << 16;
        plane_va = t->bo.va + t->planes_off;
    }
    if (t->layers > 1) {
        tex[6] = (cube ? t->layers / 6 : t->layers) - 1;                     // array size (cubes, for a cube array)
    }
    if (t->samples > 1) {
        // MULTISAMPLE: the sample count; each sample a plane (layered), the plane's slice stride apart (txf_ms)
        tex[3] |= (t->samples == 4 ? 2u : t->samples == 2 ? 1u : 0u) << 13;
    }
    tex[4] = (uint32_t)plane_va;
    tex[5] = (uint32_t)(plane_va >> 32);
    tex[7] = t->type == 7 ? t->depth - 1 : 0;
    plane[0] = planeWord0(t);                                               // a plane, generic (or ASTC), linear
    plane[1] = (uint32_t)(t->bo.size - t->offset);
    plane[2] = (uint32_t)t->gpu();
    plane[3] = (uint32_t)(t->gpu() >> 32);
    plane[4] = t->stride;
    plane[6] = t->stride * t->levelRows(0);                                 // (a slice: a 3D texture's depth)
    plane[7] = (t->width - 1) | (t->height - 1) << 16;
}
}  // namespace mali
