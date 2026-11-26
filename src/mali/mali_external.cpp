// mali_external.cpp: the Mali back end under a plug-in built on IOGPU.framework (AkIOS's AkiosMetal; phase 6 of
// ref/iogpu-research/phase6-plan.md, sections 2.1-2.3). hakios_metal.h's "OVER IOGPU.FRAMEWORK" says what changes: the
// plug-in owns the connection, the queues, the submission and the completion; the core records, builds the stream into
// memory the plug-in gives it, and lets go at retirement. The kernel's side: akios xnu/iokit/Kernel/akios_iogpu.cpp
// (IOGPUDeviceUserClient: one GPU address space per connection, the tail stream CALLed from the queue's ring) and
// hakios.akita_gpu.cpp (HakiosGPU: the vendor selectors 56/57, the description's properties).
#include "mali_internal.h"

using namespace mali;

namespace {

// HakiosGPU::vendorMethod's selectors on an IOGPU connection
enum : uint32_t { kSelTilerHeap = 56, kSelTilerHeapGone = 57 };

// a number property of the service (OSNumber, 64 bits), or false when there is none
bool
numberProperty(io_service_t service, const char *key, uint64_t &value)
{
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef p = name != nullptr ? IORegistryEntryCreateCFProperty(service, name, kCFAllocatorDefault, 0) : nullptr;
    bool ok = false;
    if (p != nullptr && CFGetTypeID(p) == CFNumberGetTypeID()) {
        int64_t v = 0;
        ok = CFNumberGetValue((CFNumberRef)p, kCFNumberSInt64Type, &v);
        value = (uint64_t)v;
    }
    if (p != nullptr) CFRelease(p);
    if (name != nullptr) CFRelease(name);
    return ok;
}

/*
 * THE GPU'S DESCRIPTION: HakiosGPU::publishDescription's properties on the service the connection is to -- panthor's
 * gpu_info fields under generic names ("gpu-id", "shader-present", "tiler-features", ...). A kernel without them (before
 * 11-26): panthor's DEV_QUERY on a type-0 connection of the same service, opened for that one question and closed
 * again -- the only IOServiceOpen of this path, and it goes once that kernel is gone.
 */
bool
describe(io_connect_t conn, gpu_info &info)
{
    io_service_t service = 0;
    if (IOConnectGetService(conn, &service) != KERN_SUCCESS || service == 0) {
        fail("IOGPU: the connection names no service");
        return false;
    }
    uint64_t v = 0;
    bool ok = numberProperty(service, "gpu-id", v);
    if (ok) {
        info.gpu_id = (uint32_t)v;
        uint64_t x;
        if (numberProperty(service, "shader-present", x)) info.shader_present = x;
        if (numberProperty(service, "tiler-present", x)) info.tiler_present = x;
        if (numberProperty(service, "l2-present", x)) info.l2_present = x;
        if (numberProperty(service, "max-threads", x)) info.max_threads = (uint32_t)x;
        if (numberProperty(service, "thread-features", x)) info.thread_features = (uint32_t)x;
        if (numberProperty(service, "tiler-features", x)) info.tiler_features = (uint32_t)x;
        if (numberProperty(service, "thread-max-workgroup-size", x)) info.thread_max_workgroup_size = (uint32_t)x;
        if (numberProperty(service, "csf-id", x)) info.csf_id = (uint32_t)x;
        // (no "gpu-rev": the revision is GPU_ID's low bits, as panthor derives it)
        info.gpu_rev = info.gpu_id & 0xffff;
    } else {
        // (a kernel before the properties: panthor's description, the type-0 connection closed again at once)
        io_connect_t old = 0;
        dev_query q = { 0, sizeof(info), (uint64_t)(uintptr_t)&info };
        size_t out = sizeof(q);
        ok = IOServiceOpen(service, mach_task_self(), 0, &old) == KERN_SUCCESS &&
             IOConnectCallStructMethod(old, DEV_QUERY, &q, sizeof(q), &q, &out) == KERN_SUCCESS;
        if (old != 0) IOServiceClose(old);
        if (ok) {
            fprintf(stderr, "metal-iogpu: %s: the GPU service has no description properties: panthor's DEV_QUERY instead\n",
                getprogname());
        }
    }
    IOObjectRelease(service);
    if (!ok || info.shader_present == 0) {
        fail("IOGPU: no description of the GPU (properties or DEV_QUERY)");
        return false;
    }
    return true;
}

// ---- texture layouts: the core's own (hakios_metal_texture_new / _levels / _layered / _3d / _multisample), in one place
// for a texture whose memory is made by someone else. LAYOUT's asked fields in; T's layout fields and the size out. ----

// level 0, one layer (hakios_metal_texture_new's rules)
bool
layoutBase(hakios_metal_texture &t, unsigned type, const MaliFormat *f, unsigned pixel_format, unsigned width,
    unsigned height, unsigned usage, uint64_t &size)
{
    if ((type != 2 && !(type == 0 && height == 1)) || width == 0 || height == 0 || width > 16384 || height > 16384) {
        fail("a texture of type %u, %u x %u (the Mali build makes 2D and 1D ones up to 16384)", type, width, height);
        return false;
    }
    t.type = type;
    t.format = pixel_format;
    t.width = width;
    t.height = height;
    t.depth = 1;
    t.usage = usage;
    t.mali_format = f->mali;
    if (f->metal == 1) t.swizzle = kAlphaSwizzle;
    t.bytes = f->bytes;
    t.bw = f->bw;
    t.bh = f->bh;
    t.stride = ((width + f->bw - 1) / f->bw * f->bytes + 63) & ~63u;
    size = (uint64_t)t.stride * t.levelRows(0);
    if (pixel_format == 260) {
        t.stencil_off = (size + 63) & ~63ULL;
        t.stencil_stride = (width + 63) & ~63u;
        size = t.stencil_off + (uint64_t)t.stencil_stride * height;
    }
    return true;
}

// the levels' chain (hakios_metal_texture_new_levels / _new_layered): offsets, strides; the bytes of one layer's chain
uint64_t
layoutLevels(hakios_metal_texture &t, unsigned type_for_most, unsigned levels)
{
    unsigned most = 1;
    while (most < 16 && ((t.width >> most) || (type_for_most != 0 && type_for_most != 1 && (t.height >> most)))) most++;
    if (levels == 0) levels = 1;
    if (levels > most) levels = most;
    t.levels = levels;
    uint64_t at = 0;
    for (unsigned l = 0; l < levels; l++) {
        t.level_off[l] = at;
        t.level_stride[l] = ((t.levelWidth(l) + t.bw - 1) / t.bw * t.bytes + 63) & ~63u;
        at += ((uint64_t)t.level_stride[l] * t.levelRows(l) + 63) & ~63ULL;
    }
    return at;
}

bool
layoutOf(const hakios_metal_texture_layout &in, hakios_metal_texture &t, uint64_t &size)
{
    static const bool no_zs = bootArg("metal_nozs");
    const unsigned type = in.type, fmt = in.pixel_format, w = in.width, h = in.height;
    if (no_zs && fmt >= 250 && fmt <= 261) {
        fail("a depth or stencil texture of MTLPixelFormat %u (boot-arg metal_nozs)", fmt);
        return false;
    }
    const MaliFormat *f = findFormat(fmt);
    if (f == nullptr) {
        fail("a texture of MTLPixelFormat %u (not served on the Mali yet)", fmt);
        return false;
    }
    unsigned samples = in.samples == 2 ? 4 : in.samples ? in.samples : 1;
    if (type == 4 && samples > 1) {
        // 2D MULTISAMPLE: each sample a plane of the rows (hakios_metal_texture_new_multisample)
        if (samples != 4) {
            fail("a multisample texture of %u samples (the Mali-G715 has no 2x sample pattern; 4 is served)", in.samples);
            return false;
        }
        if (!layoutBase(t, 2, f, fmt, w, h, in.usage, size)) return false;
        uint64_t plane = ((uint64_t)t.stride * t.levelRows(0) + 63) & ~63ULL;
        if (plane != (uint64_t)t.stride * t.levelRows(0)) {
            fail("a multisample texture of %ux%u, MTLPixelFormat %u: its sample planes would not be 64-byte aligned", w, h, fmt);
            return false;
        }
        size = plane * samples;
        if (fmt == 260) {
            t.stencil_off = size;
            size += (uint64_t)t.stencil_stride * h * samples;
        }
        t.samples = samples;
        return true;
    }
    if (samples != 1) {
        fail("%u samples for a texture of type %u (a 2D multisample texture's alone)", in.samples, type);
        return false;
    }
    const bool layered = type == 1 || type == 3 || type == 5 || type == 6;
    const unsigned layers = layered ? (in.depth ? in.depth : 1) : 1;
    if (layered && !(layers <= 1 && (type == 2 || type == 3))) {
        // 1D AND 2D ARRAYS, CUBES, CUBE ARRAYS: each layer its chain of levels, the plane descriptors after all of them
        const bool cube = type == 5 || type == 6;
        if (layers == 0 || layers > 2048 || (cube && (layers % 6 != 0 || w != h)) || (type == 1 && h != 1) || fmt == 253 ||
            fmt == 260) {
            fail("a layered texture: type %u, %u layers, %u x %u, MTLPixelFormat %u (the Mali build makes 1D and 2D arrays, "
                "cubes and cube arrays up to 2048 layers; no stencil)", type, layers, w, h, fmt);
            return false;
        }
        if (!layoutBase(t, type == 1 ? 0 : 2, f, fmt, w, h, in.usage, size)) return false;
        t.type = type;
        uint64_t at = layoutLevels(t, type, in.levels);
        t.stencil_off = 0;
        t.stencil_stride = 0;
        t.layers = layers;
        t.layer_stride = at;
        t.planes_off = at * layers;
        size = at * layers + 32ULL * t.levels * layers;
        return true;
    }
    if (type == 7) {
        // 3D: DEPTH planes of a 2D texture's rows (hakios_metal_texture_new_3d), one level
        const unsigned depth = in.depth ? in.depth : 1;
        if (depth > 2048 || w > 2048 || h > 2048) {
            fail("a 3D texture of %u x %u x %u (the Mali build makes them up to 2048 each way)", w, h, depth);
            return false;
        }
        if (!layoutBase(t, 2, f, fmt, w, h, in.usage, size)) return false;
        t.type = 7;
        if (depth > 1) {
            size = (uint64_t)t.stride * h * depth;
            t.depth = depth;
        }
        return true;
    }
    // 1D and 2D (a 2D array of one layer: a 2D texture, as hakios_metal_texture_new_layered makes it), mipmapped or not
    if (!layoutBase(t, type == 3 || type == 4 ? 2 : type, f, fmt, w, h, in.usage, size)) return false;
    if (in.levels <= 1) return true;
    uint64_t at = layoutLevels(t, t.type, in.levels);
    if (t.levels <= 1) {
        t.levels = 1;
        return true;
    }
    if (fmt == 260) {
        for (unsigned l = 0; l < t.levels; l++) {
            t.stencil_level_off[l] = at;
            t.stencil_level_stride[l] = (t.levelWidth(l) + 63) & ~63u;
            at += ((uint64_t)t.stencil_level_stride[l] * t.levelHeight(l) + 63) & ~63ULL;
        }
        t.stencil_off = t.stencil_level_off[0];
        t.stencil_stride = t.stencil_level_stride[0];
    }
    t.planes_off = at;
    size = at + 32ULL * t.levels;
    return true;
}

// one level's plane descriptor ("Generic Plane"), as hakios_metal_texture_new_levels writes them
void
writePlane(const hakios_metal_texture *t, uint32_t *plane, uint64_t va, unsigned l)
{
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

}  // namespace

namespace mali {
bool
extNewHeap(hakios_metal_device *d, uint32_t chunk_size, uint32_t initial, uint32_t most, uint32_t in_flight, uint32_t &handle,
    uint64_t &ctx, uint64_t &first)
{
    uint64_t in[4] = { chunk_size, initial, most, in_flight }, out[3] = {};
    uint32_t n = 3;
    kern_return_t kr = IOConnectCallScalarMethod(d->shared_conn, kSelTilerHeap, in, 4, out, &n);
    if (kr != KERN_SUCCESS || out[0] == 0) {
        fail("IOGPU: no tiler heap (0x%x)", kr);
        return false;
    }
    handle = (uint32_t)out[0];
    ctx = out[1];
    first = out[2];
    return true;
}

void
extFreeHeap(hakios_metal_device *d, uint32_t handle)
{
    uint64_t in = handle;
    IOConnectCallScalarMethod(d->shared_conn, kSelTilerHeapGone, &in, 1, nullptr, nullptr);
}
}  // namespace mali

extern "C" {

hakios_metal_device *
hakios_metal_open_iogpu(io_connect_t conn, const struct hakios_metal_memory_ops *ops)
{
    if (conn == 0 || ops == nullptr || ops->alloc == nullptr || ops->free == nullptr) {
        fail("hakios_metal_open_iogpu: no connection or no memory operations");
        return nullptr;
    }
    gpu_info info = {};
    if (!describe(conn, info)) {
        return nullptr;
    }
    auto *d = new hakios_metal_device;
    d->external = true;
    d->shared_conn = conn;
    d->ext_ops = *ops;
    d->ext_queue = new hakios_metal_queue;
    d->ext_queue->device = d;
    d->gpu_id = info.gpu_id;
    d->shaders = info.shader_present;
    d->tiler = info.tiler_present;
    d->max_threads = info.max_threads;
    d->max_tasks = info.thread_features >> 24;
    for (d->core_range = 64; d->core_range > 0 && !(info.shader_present >> (d->core_range - 1) & 1); d->core_range--) {
    }
    d->tiler_levels = (info.tiler_features >> 8) & 0xf;
    if (!openBlits(d)) {
        std::string why = gError;
        hakios_metal_close(d);
        fail("%s", why.c_str());
        return nullptr;
    }
    if (!openDraws(d)) {
        // (without the heap, draws are refused by name; blits and kernels go on)
        fprintf(stderr, "metal-iogpu: %s: no tiler heap (%s): draws will be refused\n", getprogname(), gError.c_str());
    }
    return d;
}

hakios_metal_buffer *
hakios_metal_buffer_wrap(hakios_metal_device *d, uint64_t gpu_va, void *cpu, uint64_t length, uint32_t resource_id)
{
    (void)resource_id;
    if (d == nullptr || gpu_va == 0 || length == 0) {
        fail("a buffer wrapped with no device, address or length");
        return nullptr;
    }
    auto *b = new hakios_metal_buffer;
    b->device = d;
    b->length = length;
    b->bo.handle = 0;               // (not the core's: freeBo and the pools leave it alone)
    b->bo.size = length;
    b->bo.va = gpu_va;
    b->bo.cpu = cpu;
    return b;
}

int
hakios_metal_texture_plan(hakios_metal_device *d, unsigned type, unsigned pixel_format, unsigned width, unsigned height,
    unsigned depth, unsigned levels, unsigned samples, unsigned usage, struct hakios_metal_texture_layout *layout)
{
    (void)d;
    if (layout == nullptr) {
        fail("a texture plan with nowhere to put it");
        return -1;
    }
    hakios_metal_texture_layout in = {};
    in.type = type;
    in.pixel_format = pixel_format;
    in.width = width;
    in.height = height;
    in.depth = depth;
    in.levels = levels;
    in.samples = samples;
    in.usage = usage;
    hakios_metal_texture t;
    uint64_t size = 0;
    if (!layoutOf(in, t, size)) {
        return -1;
    }
    *layout = in;
    layout->size = (size + 0x3fff) & ~0x3fffULL;
    layout->row_bytes = t.stride;
    layout->level_count = t.levels;
    for (unsigned l = 0; l < t.levels && l < 16; l++) {
        layout->level_offset[l] = t.levelOffset(l);
        layout->level_row_bytes[l] = t.levelStride(l);
    }
    layout->layers = t.layers;
    layout->layer_stride = t.layer_stride;
    layout->stencil_offset = t.stencil_off;
    layout->stencil_row_bytes = t.stencil_stride;
    layout->planes_offset = t.planes_off;
    layout->samples_made = t.samples;
    return 0;
}

hakios_metal_texture *
hakios_metal_texture_wrap(hakios_metal_device *d, const struct hakios_metal_texture_layout *layout, uint64_t gpu_va, void *cpu)
{
    if (d == nullptr || layout == nullptr || gpu_va == 0 || cpu == nullptr) {
        fail("a texture wrapped with no device, layout, address or mapping");
        return nullptr;
    }
    auto *t = new hakios_metal_texture;
    uint64_t size = 0;
    // (the layout again from what was asked: one source, and a layout changed by the caller is caught)
    if (!layoutOf(*layout, *t, size)) {
        delete t;
        return nullptr;
    }
    if (((size + 0x3fff) & ~0x3fffULL) != layout->size) {
        fail("a texture wrapped with a layout hakios_metal_texture_plan did not make");
        delete t;
        return nullptr;
    }
    t->device = d;
    t->origin = 0;
    t->bo.handle = 0;               // (the memory is the caller's: freeBo leaves it alone)
    t->bo.size = layout->size;
    t->bo.va = gpu_va;
    t->bo.cpu = cpu;
    if (t->planes_off != 0) {
        for (unsigned y = 0; y < t->layers; y++) {
            for (unsigned l = 0; l < t->levels; l++) {
                auto *plane = (uint32_t *)((uint8_t *)cpu + t->planes_off + 32 * ((uint64_t)y * t->levels + l));
                writePlane(t, plane, t->gpu() + t->layer_stride * y + t->level_off[l], l);
            }
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    return t;
}

hakios_metal_cb *
hakios_metal_cb_new_external(hakios_metal_device *d)
{
    if (d == nullptr || !d->external) {
        fail("an external command buffer of a device not opened over IOGPU");
        return nullptr;
    }
    auto *cb = new hakios_metal_cb;
    cb->queue = d->ext_queue;
    return cb;
}

int
hakios_metal_cb_build(hakios_metal_cb *cb, uint64_t *va, uint32_t *length)
{
    *va = 0;
    *length = 0;
    hakios_metal_device *d = cb->queue->device;
    cb->committed_at = std::chrono::steady_clock::now();
    // (no surface marks, mirrors or glitch scans: the display and IOSurface textures come in step D of the plan)
    zsKeep(cb);
    passElide(cb);
    cb->status = HAKIOS_METAL_COMMITTED;
    if (!cb->error.empty()) {
        cb->status = HAKIOS_METAL_ERROR;
        return -1;
    }
    if (cb->blits.empty()) {
        cb->status = HAKIOS_METAL_SCHEDULED;
        return 0;
    }
    unsigned n = 0;
    if (!emitStream(cb, d, n)) {
        return -1;                  // (CB's error set: no stream buffer)
    }
    cb->submitted_at = std::chrono::steady_clock::now();
    cb->status = HAKIOS_METAL_SCHEDULED;
    *va = cb->stream.va;
    *length = n * 8;
    return 0;
}

enum hakios_metal_status
hakios_metal_cb_retire(hakios_metal_cb *cb, uint32_t iogpu_status)
{
    hakios_metal_device *d = cb->queue->device;
    {
        std::lock_guard<std::mutex> g(cb->lock);
        cb->waited_at = std::chrono::steady_clock::now();
        if (cb->status != HAKIOS_METAL_ERROR) {
            if (iogpu_status == 0) {
                cb->status = HAKIOS_METAL_COMPLETED;
            } else {
                char why[160];
                if (iogpu_status == 0xb) {
                    snprintf(why, sizeof(why), "a GPU fault: the command buffer's work is lost (IOGPU status 0xb, page fault)");
                } else {
                    snprintf(why, sizeof(why), "the GPU did not run or finish the command buffer (IOGPU status 0x%x)", iogpu_status);
                }
                cb->error = why;
                cb->status = HAKIOS_METAL_ERROR;
            }
        }
    }
    static const bool stats = bootArg("metal_stats") || bootArg("mdsay=");
    if (stats) {
        passStats(cb);
    }
    d->giveStream(cb->stream);
    if (cb->timestamps.size != 0) {
        d->freeBo(cb->timestamps);
        cb->timestamps.size = 0;
    }
    cb->letGo();
    cb->done.notify_all();
    return cb->status;
}

}  // extern "C"
