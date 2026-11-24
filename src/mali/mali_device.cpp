// mali_device.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

namespace mali {
// what draws need of the device: the queue's tiler heap (panvk's sizes) and its descriptor with a 64 KiB geometry buffer
// after it (heapdesc + 0x1000); the sample positions (descriptors + 0x1800), a depth/stencil descriptor that lets all
// through (+ 0x1980). (The GPU's pages are outer shareable -- hakios-kernel's PTE_SH_OS: without that the tiler read
// what the shader cores had written as memory held it before, and every pass looked like the one before it.)

bool
openDraws(hakios_metal_device *d)
{
    // (HAKIOS_MALI_HEAP_CHUNKS=N: the heap's most chunks, 5 to 64 -- a test of incremental rendering, mali_draw_test)
    const char *most = getenv("HAKIOS_MALI_HEAP_CHUNKS");
    uint32_t max_chunks = most != nullptr && atoi(most) >= 5 && atoi(most) <= 64 ? (uint32_t)atoi(most) : 64;
    // (11-13) THE HEAP'S FIRST CHUNKS: five (10 MB) for the window server, which draws the screen from its first frame;
    // ONE for every other process -- with Metal for every process each client held 10 MB of the kernel's memory from its
    // start, drawn or not (141 MB of them in an application sweep), and the heap grows by itself when a pass fills it
    // (HakiosGPU's tiler out-of-memory answer, a chunk a time up to max_chunks). HAKIOS_MALI_HEAP_FIRST=N: N.
    const char *prog = getprogname(), *first_env = getenv("HAKIOS_MALI_HEAP_FIRST");
    uint32_t first_chunks = first_env != nullptr && atoi(first_env) >= 1 && atoi(first_env) <= 5 ? (uint32_t)atoi(first_env) :
        prog != nullptr && strcmp(prog, "backboardd") == 0 ? 5 : 1;
    struct { uint32_t vm_id, initial, chunk_size, max_chunks, in_flight, handle; uint64_t ctx, first; } th =
        { d->vm, first_chunks, 2u << 20, max_chunks, 65535, 0, 0, 0 };
    if (d->call(0x4b /* TILER_HEAP_CREATE */, &th, sizeof(th)) != 0) {
        fail("HakiosGPU: no tiler heap (hakios-kernel with heaps, 11-05 or later)");
        return false;
    }
    d->heap_handle = th.handle;
    d->heap_ctx = th.ctx;
    d->heap_first = th.first;
    if (!d->newBo(d->heapdesc, 0x18000, BIND_NOEXEC)) {
        return false;
    }
    auto *h = (uint32_t *)d->heapdesc.cpu;
    memset(h, 0, d->heapdesc.size);
    h[0] = 9 | 2 << 4;                                  // a buffer: a tiler heap
    h[1] = 2u << 20;
    *(uint64_t *)&h[2] = th.first;
    *(uint64_t *)&h[4] = th.first + 64;                 // (past the chunk's header)
    *(uint64_t *)&h[6] = th.first + (2u << 20);
    auto *desc = (uint8_t *)d->descriptors.cpu;
    auto *pos = (uint32_t *)(desc + 0x1800), *zs = (uint32_t *)(desc + 0x1980);
    memset(pos, 0, 256);
    pos[0] = pos[32] = 128 | 128 << 16;                 // one sample, at the pixel's centre (and the origin)
    // four samples (descriptors + 0x2000): Mesa's rotated 4x grid (pan_samples.c), 1/256 pixel
    auto *pos4 = (uint32_t *)(desc + 0x2000);
    memset(pos4, 0, 256);
    pos4[0] = 96 | 32u << 16;
    pos4[1] = 224 | 96u << 16;
    pos4[2] = 32 | 160u << 16;
    pos4[3] = 160 | 224u << 16;
    pos4[32] = 128 | 128u << 16;
    memset(zs, 0, 32);
    zs[0] = 7 | 7 << 4 | 7 << 16;                       // stencil faces: always
    zs[4] = 1u << 21 | 1u << 22 | 1u << 23 | 2u << 25 | 7u << 29;   // depth: always, nothing written (zsOf's word 4)
    d->heap_ok = true;
    return true;
}
}  // namespace mali
extern "C" {

const char *
hakios_metal_error(void)
{
    return gError.c_str();
}

/*
 * The kernel starts the Mali mali_delay (8) s after its own start, once the firmware's file can be read: on the Pixel 8a
 * HakiosGPU appears about 20 s into the boot, after backboardd has asked for its device (15 s) -- and a window server
 * without one composites in software for good. With the GPU kept (mali_keep, mali= among the boot-args) the service is
 * waited for, up to 15 s; without it the answer is at once, as before.
 */
extern "C++" bool
gpu_expected(void)
{
    char args[1024] = {};
    size_t n = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &n, nullptr, 0) != 0) {
        return false;
    }
    for (const char *p = args; *p != 0;) {
        while (*p == ' ') p++;
        if ((strncmp(p, "mali_keep=", 10) == 0 && p[10] != '0') || (strncmp(p, "mali=", 5) == 0 && p[5] != '0')) {
            return true;
        }
        while (*p != 0 && *p != ' ') p++;
    }
    return false;
}

extern "C++" io_service_t
find_gpu_service(void)
{
    io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
    if (service != 0 || !gpu_expected()) {
        return service;
    }
    for (int i = 0; i < 150 && service == 0; i++) {     // 15 s in steps of 100 ms
        usleep(100000);
        service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
    }
    if (service != 0) {
        fprintf(stderr, "hakios-metal: HakiosGPU appeared while it was waited for\n");
    }
    return service;
}

hakios_metal_device *
hakios_metal_open(void)
{
    io_service_t service = find_gpu_service();
    if (service == 0) {
        fail("no HakiosGPU service (hakios-kernel with mali_keep=1)");
        return nullptr;
    }
    CFTypeRef kind = IORegistryEntryCreateCFProperty(service, CFSTR("DRMDriver"), kCFAllocatorDefault, 0);
    bool panthor = kind != nullptr && CFGetTypeID(kind) == CFStringGetTypeID() &&
        CFStringCompare((CFStringRef)kind, CFSTR("panthor"), 0) == kCFCompareEqualTo;
    if (kind != nullptr) {
        CFRelease(kind);
    }
    if (!panthor) {
        fail("HakiosGPU is not a Mali (panthor) here");
        IOObjectRelease(service);
        return nullptr;
    }
    auto *d = new hakios_metal_device;
    if (IOServiceOpen(service, mach_task_self(), 0, &d->connection) != KERN_SUCCESS) {
        IOObjectRelease(service);
        delete d;
        fail("HakiosGPU could not be opened");
        return nullptr;
    }
    IOObjectRelease(service);
    gpu_info info = {};
    dev_query q = { 0, sizeof(info), (uint64_t)(uintptr_t)&info };
    vm_create vc = {};
    vc.user_va_range = 1ULL << 47;
    if (d->call(DEV_QUERY, &q, sizeof(q)) != 0 || d->call(VM_CREATE, &vc, sizeof(vc)) != 0) {
        fail("HakiosGPU: no description or no VM");
        hakios_metal_close(d);
        return nullptr;
    }
    d->gpu_id = info.gpu_id;
    d->shaders = info.shader_present;
    d->tiler = info.tiler_present;
    d->max_threads = info.max_threads;
    d->max_tasks = info.thread_features >> 24;
    for (d->core_range = 64; d->core_range > 0 && !(info.shader_present >> (d->core_range - 1) & 1); d->core_range--) {
    }
    d->vm = vc.id;
    // the blit shaders (code read only and executable), their program descriptors and the local storage
    if (!d->newBo(d->descriptors, 0x4000, BIND_READONLY)) {
        hakios_metal_close(d);
        return nullptr;
    }
    const mali_blit_shader *shaders[4] = { &mali_copy_u32, &mali_fill_u32, &mali_copy_u8, &mali_fill_u8 };
    auto *bytes = (uint8_t *)d->descriptors.cpu;
    for (unsigned i = 0; i < 4; i++) {
        memcpy(bytes + 0x200 * i, shaders[i]->code, shaders[i]->size);     // code at 0x000, 0x200, 0x400, 0x600
        auto *spd = (uint32_t *)(bytes + 0x1000 + 0x40 * i);                // program descriptors from 0x1000
        spd[0] = 8 | 1 << 4 | (shaders[i]->work_regs <= 32 ? 2u : 0u) << 30;
        spd[1] = (uint32_t)(shaders[i]->preload >> 48);
        spd[2] = (uint32_t)(d->descriptors.va + 0x200 * i);
        spd[3] = (uint32_t)((d->descriptors.va + 0x200 * i) >> 32);
        d->spd[i] = d->descriptors.va + 0x1000 + 0x40 * i;
    }
    {
        // copy_rows_u32 (COPY_ROWS): its code at 0x800 (up to 2 KiB), its program descriptor at 0x1100
        const mali_blit_shader *r = &mali_copy_rows_u32;
        memcpy(bytes + 0x800, r->code, r->size);
        auto *spd = (uint32_t *)(bytes + 0x1100);
        spd[0] = 8 | 1 << 4 | (r->work_regs <= 32 ? 2u : 0u) << 30;
        spd[1] = (uint32_t)(r->preload >> 48);
        spd[2] = (uint32_t)(d->descriptors.va + 0x800);
        spd[3] = (uint32_t)((d->descriptors.va + 0x800) >> 32);
        d->spd[4] = d->descriptors.va + 0x1100;
    }
    ((uint32_t *)(bytes + 0x1200))[1] = 31;                                 // local storage: no workgroup memory
    __asm__ volatile("dsb sy" ::: "memory");
    // one group with one queue
    queue_create qc = {};
    qc.priority = 1;
    qc.ringbuf_size = 0x10000;
    group_create gc = {};
    gc.queues = { sizeof(qc), 1, (uint64_t)(uintptr_t)&qc };
    gc.max_compute_cores = gc.max_fragment_cores = 64;
    gc.max_tiler_cores = 1;
    gc.priority = 1;
    gc.compute_core_mask = gc.fragment_core_mask = d->shaders;
    gc.tiler_core_mask = d->tiler;
    gc.vm_id = d->vm;
    d->tiler_levels = (info.tiler_features >> 8) & 0xf;
    openDraws(d);                                       // (without it, draws are refused by name)
    if (d->call(GROUP_CREATE, &gc, sizeof(gc)) != 0) {
        fail("HakiosGPU: no group");
        hakios_metal_close(d);
        return nullptr;
    }
    d->group = gc.group_handle;
    return d;
}

hakios_metal_device *
hakios_metal_open_fake(void)
{
    fail("no fake GPU in the Mali build");
    return nullptr;
}

void
hakios_metal_close(hakios_metal_device *d)
{
    if (d == nullptr) {
        return;
    }
    if (d->tidy != nullptr) {
        std::lock_guard<std::mutex> alive(d->tidy->m);     // (the pool's thread: no device from now on)
        d->tidy->d = nullptr;
    }
    {
        std::lock_guard<std::mutex> g(d->lock);
        for (auto &kept : d->pool) {
            d->freeBoNow(kept.bo);
        }
        d->pool.clear();
        d->pool_bytes = ~0ull >> 1;                         // (nothing more is kept)
    }
    for (Bo &b : d->stream_pool) {
        d->freeBo(b);
    }
    d->stream_pool.clear();
    d->freeBo(d->descriptors);
    d->freeBo(d->heapdesc);
    if (d->heap_handle != 0) {
        struct { uint32_t handle, pad; } td = { d->heap_handle, 0 };
        d->call(0x4c /* TILER_HEAP_DESTROY */, &td, sizeof(td));
    }
    d->freeBo(d->wls);
    for (Bo &b : d->tls) d->freeBo(b);
    for (Bo &b : d->retired) {
        d->freeBo(b);
    }
    if (d->connection) {
        IOServiceClose(d->connection);
    }
    delete d;
}

uint64_t
hakios_metal_gpu_id(hakios_metal_device *d)
{
    return d ? d->gpu_id : 0;
}

int
hakios_metal_has_compiler(void)
{
    return 1;
}

hakios_metal_buffer *
hakios_metal_buffer_new(hakios_metal_device *d, uint64_t length)
{
    auto *b = new hakios_metal_buffer;
    b->device = d;
    b->length = length;
    if (d->takeBuffer(b->bo, (length ? length : 1) + kBufferSlack)) {
        memset(b->bo.cpu, 0, (size_t)length);           // (a new buffer is zeroes, as the kernel's are)
    } else if (!d->newBo(b->bo, (length ? length : 1) + kBufferSlack, BIND_NOEXEC)) {
        delete b;
        return nullptr;
    }
    if (b->bo.cpu != nullptr && b->bo.size > length) {
        memset((uint8_t *)b->bo.cpu + length, kSlackByte, (size_t)(b->bo.size - length));
    }
    return b;
}

void
hakios_metal_buffer_free(hakios_metal_buffer *b)
{
    if (b != nullptr) {
        b->release();       // (a command buffer that uses it keeps it until the GPU is done)
    }
}

void *
hakios_metal_buffer_contents(hakios_metal_buffer *b)
{
    return b ? b->bo.cpu : nullptr;
}

uint64_t
hakios_metal_buffer_length(hakios_metal_buffer *b)
{
    return b ? b->length : 0;
}

uint64_t
hakios_metal_buffer_gpu_address(hakios_metal_buffer *b)
{
    return b ? b->bo.va : 0;
}

hakios_metal_queue *
hakios_metal_queue_new(hakios_metal_device *d)
{
    auto *q = new hakios_metal_queue;
    q->device = d;
    q->thread = std::thread([q] {
        q->run();
        if (q->self_free.load()) {
            delete q;
        }
    });
    return q;
}

void
hakios_metal_queue_free(hakios_metal_queue *q)
{
    if (q == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> l(q->lock);
        q->stopping = true;
    }
    q->work.notify_all();
    if (std::this_thread::get_id() == q->thread.get_id()) {
        // FREED ON ITS OWN THREAD (11-12): a command buffer's completion handler let the last reference to the queue
        // go -- joining oneself is libc++'s "thread::join failed: Resource deadlock avoided", uncaught: mediaserverd
        // aborted. The thread ends by itself once the handler returns (stopping is set) and deletes the queue.
        q->self_free = true;
        q->thread.detach();
        return;
    }
    q->thread.join();
    delete q;
}

hakios_metal_cb *
hakios_metal_cb_new(hakios_metal_queue *q)
{
    auto *cb = new hakios_metal_cb;
    cb->queue = q;
    return cb;
}

void
hakios_metal_cb_free(hakios_metal_cb *cb)
{
    if (cb == nullptr) {
        return;
    }
    std::unique_lock<std::mutex> l(cb->lock);
    if (cb->status == HAKIOS_METAL_COMMITTED || cb->status == HAKIOS_METAL_SCHEDULED) {
        if (cb->callback == nullptr) {
            cb->freed = true;   // the queue's thread deletes it after
            return;
        }
        cb->done.wait(l, [&] { return cb->status == HAKIOS_METAL_COMPLETED || cb->status == HAKIOS_METAL_ERROR; });
    }
    l.unlock();
    cb->letGo();            // (never committed: what it held)
    delete cb;
}

int
hakios_metal_cb_copy(hakios_metal_cb *cb, hakios_metal_buffer *from, uint64_t from_offset, hakios_metal_buffer *to,
    uint64_t to_offset, uint64_t length)
{
    if (from_offset + length > from->bo.size || to_offset + length > to->bo.size || length >= (1ULL << 32)) {
        fail("copy: out of the buffers");
        return -1;
    }
    bool words = ((from_offset | to_offset | length) & 3) == 0;
    Job j = {};
    j.shader = words ? 0u : 2u;
    j.dst = to->bo.va + to_offset;
    j.src = from->bo.va + from_offset;
    j.count = (uint32_t)(words ? length / 4 : length);
    cb->blits.push_back(j);
    cb->hold(from);
    cb->hold(to);
    return 0;
}

int
hakios_metal_cb_fill(hakios_metal_cb *cb, hakios_metal_buffer *buffer, uint64_t offset, uint64_t length, uint8_t value)
{
    if (offset + length > buffer->bo.size || length >= (1ULL << 32)) {
        fail("fill: out of the buffer");
        return -1;
    }
    bool words = ((offset | length) & 3) == 0;
    Job j = {};
    j.shader = words ? 1u : 3u;
    j.dst = buffer->bo.va + offset;
    if (getenv("HAKIOS_MALI_FAULT_TEST") != nullptr) {
        j.dst = 0x1000;                                 // (a test of the kernel's recovery: an address nothing maps)
    }
    j.value = words ? value * 0x01010101u : value;
    j.count = (uint32_t)(words ? length / 4 : length);
    cb->blits.push_back(j);
    cb->hold(buffer);
    return 0;
}

void
hakios_metal_cb_fail(hakios_metal_cb *cb, const char *why)
{
    if (cb->error.empty()) {
        cb->error = why;
    }
}

/*
 * AN ATTACHMENT NO DRAW OF A MALI PASS TOUCHES (11-10): CoreAnimation's passes carry a second colour attachment that
 * most of their draws neither write nor read; each such draw keeps it loaded and stored back unchanged, so a full-screen
 * Mali pass read and wrote ~10 MB of it for nothing -- Spotlight's opening drew ~8 full-screen passes a frame at 2.1-2.8
 * ms each, 17-25 ms of GPU time a frame (metal_stats_ms). Over the passes as the encoder will group them (samePass,
 * which reads none of this), an attachment every draw of the pass leaves untouched is elided: not preloaded, not
 * written back -- its memory keeps what it had, as a load and a store of it unchanged would leave it. A pass with
 * nothing else to load has no preload at all. Boot-arg metal_noelide: not done.
 */
extern "C++" void
passElide(hakios_metal_cb *cb)
{
    static const bool off = bootArg("metal_noelide");
    if (off) return;
    for (size_t i = 0; i < cb->blits.size(); i++) {
        if (cb->blits[i].shader != DRAW) continue;
        size_t k = i;
        while (k + 1 < cb->blits.size() && cb->blits[k + 1].shader == DRAW &&
               samePass(cb->draws[cb->blits[k].dst], cb->draws[cb->blits[k + 1].dst])) {
            k++;
        }
        const DrawJob &head = cb->draws[cb->blits[i].dst];
        for (unsigned r = 1; r < head.rts; r++) {
            bool all = true;
            for (size_t j = i; j <= k && all; j++) {
                const DrawJob::More &m = cb->draws[cb->blits[j].dst].more[r - 1];
                all = m.untouched && !m.missing;
            }
            for (size_t j = i; j <= k; j++) {
                cb->draws[cb->blits[j].dst].more[r - 1].elide = all;
            }
        }
        i = k;
    }
}

/*
 * DEPTH AND STENCIL A LATER PASS LOADS ARE STORED (11-20): Maps draws a frame in several passes on one multisample depth
 * and stencil texture -- the first clears them with a draw and ends with store DontCare, the next ones begin with load
 * Load and test against them. On the phone's own GPU what the first pass left is there for the second; here nothing was
 * written back, the second pass loaded memory nobody had written (zeroes), and every fragment of the terrain failed its
 * lessEqual: the map was the clear colour with labels (metal_nozs: the land is there). So within a command buffer a
 * pass's depth or stencil that a later draw loads is written back whatever its store action says. metal_nozskeep: not.
 */
extern "C++" void
zsKeep(hakios_metal_cb *cb)
{
    static const bool off = bootArg("metal_nozskeep");
    if (off) return;
    size_t z_from = 0, s_from = 0;              // (everything before was looked at for an earlier load)
    for (size_t j = 0; j < cb->blits.size(); j++) {
        if (cb->blits[j].shader != DRAW) continue;
        const DrawJob &dj = cb->draws[cb->blits[j].dst];
        if (dj.has_z && dj.z_load) {
            for (size_t i = z_from; i < j; i++) {
                if (cb->blits[i].shader != DRAW) continue;
                DrawJob &e = cb->draws[cb->blits[i].dst];
                if (e.has_z && e.z_va == dj.z_va) e.z_write = true;
            }
            z_from = j;
        }
        if (dj.has_s && dj.s_load) {
            for (size_t i = s_from; i < j; i++) {
                if (cb->blits[i].shader != DRAW) continue;
                DrawJob &e = cb->draws[cb->blits[i].dst];
                if (e.has_s && e.s_va == dj.s_va) e.s_write = true;
            }
            s_from = j;
        }
    }
}

unsigned
hakios_metal_cb_surfaces_written(hakios_metal_cb *cb, unsigned *out, unsigned max)
{
    unsigned n = 0;

    if (cb == nullptr) {
        return 0;
    }
    for (unsigned sid : cb->busy) {     // (markBusy's list: every surface a pass, copy or kernel writes)
        if (n < max) {
            out[n] = sid;
        }
        n++;
    }
    return n < max ? n : max;
}

void
hakios_metal_cb_commit(hakios_metal_cb *cb, void (*callback)(hakios_metal_cb *cb, void *context), void *context)
{
    hakios_metal_device *d = cb->queue->device;

    cb->callback = callback;
    cb->context = context;
    cb->committed_at = std::chrono::steady_clock::now();
    mirrorsBack(cb, d);
    zsKeep(cb);
    passElide(cb);
    // NOT INTO THE SURFACE ON THE PANEL (11-10): the GPU service holds this until the panel has left each surface it draws
    // into (HakiosGPU 0x80, at most 25 ms) -- CoreAnimation records its next frame into the one still shown, and the GPU
    // overwrote what the panel was reading. Here, not at the first draw: the recording itself goes on meanwhile.
    for (unsigned sid : cb->busy) {
        struct { uint32_t surface; int32_t delta; } r = { sid, 0 };
        d->call(0x80, &r, sizeof(r));
    }
    glitchWatchCommit(cb);
    glitchScan(cb, true);
    cb->status = HAKIOS_METAL_COMMITTED;
    if (!cb->error.empty()) {
        cb->status = HAKIOS_METAL_ERROR;
    } else if (!cb->blits.empty()) {
        // the stream (from 0), then each job's push constants: 32 bytes for a blit, a kernel's fau_words for a dispatch
        static const bool stats = bootArg("metal_stats") || bootArg("mdsay=");
        size_t words = 18 + cb->blits.size() * 24 + cb->draws.size() * (DRAW_WORDS + (stats ? 4 : 0));   // (24: a compute job's 15, its flush's 3)
        for (const DrawJob &dj : cb->draws) {
            if (dj.layers > 1) words += 3 * dj.layers;                          // (a layered pass's fragment jobs)
        }
        if (stats && !cb->draws.empty() && cb->timestamps.size == 0) {
            if (!d->newBo(cb->timestamps, ((cb->draws.size() * 16) + 0xfff) & ~0xfffULL, BIND_NOEXEC)) {
                cb->timestamps.size = 0;
            }
        }
        cb->timestamp_count = 0;
        uint64_t fau_area = (words * 8 + 0xff) & ~0xffULL, fau_size = 0;
        std::vector<uint64_t> fau_at(cb->blits.size()), ir_at(cb->blits.size(), 0);
        for (size_t i = 0; i < cb->blits.size(); i++) {
            fau_at[i] = fau_area + fau_size;
            if (cb->blits[i].shader == DRAW) {
                fau_size += (drawBytes(cb->draws[cb->blits[i].dst]) + 0x3f) & ~0x3fULL;
                // a Mali pass's first draw: the pass's incremental rendering (emitIr) after its area
                if (i == 0 || cb->blits[i - 1].shader != DRAW ||
                    !samePass(cb->draws[cb->blits[i - 1].dst], cb->draws[cb->blits[i].dst])) {
                    ir_at[i] = fau_area + fau_size;
                    fau_size += IR_BYTES;
                }
                continue;
            }
            // (a dispatch: its push constants, then its local storage descriptor, 64-byte aligned)
            fau_size += cb->blits[i].shader == DISPATCH ? ((cb->blits[i].kernel->binary.fau_words * 4 + 0x3f) & ~0x3fULL) + 0x40
                                                        : 0x40;
            // (and its resource table: 6 entries, then the texture, sampler and image tables, and the planes)
            if (cb->blits[i].shader == DISPATCH && (cb->blits[i].texture_count || cb->blits[i].sampler_count)) {
                fau_size += 0x80 + (cb->blits[i].texture_count * 3 + cb->blits[i].sampler_count) * 32 + 0x40;
            }
        }
        bool stream_ok = d->takeStream(cb->stream, fau_area + fau_size);
        cb->bo_at = std::chrono::steady_clock::now();
        if (!stream_ok) {
            cb->error = gError;
            cb->status = HAKIOS_METAL_ERROR;
        } else {
            auto *s = (uint64_t *)cb->stream.cpu;
            unsigned n = 0;
            s[n++] = set_state(0, 3);           // v11's scoreboards, as panvk sets them up
            s[n++] = set_state(9, 0x8);
            s[n++] = set_state(1, 0);
            s[n++] = set_state(2, 1);
            s[n++] = set_state(8, 0xf0);
            // the GPU's caches made to see what the CPU wrote, and (at the end) to show the CPU what the GPU wrote, as
            // gallium's panfrost ends a batch: L2 and load/store cleaned, the others (the tiler's, descriptors')
            // invalidated. Without it a queue drew its first render pass only: later passes found the tiler heap and
            // contexts as the GPU's caches held them.
            s[n++] = move32(74, 0);
            s[n++] = 36ULL << 56 | 74ULL << 40 | 2 << 8 | 1 << 4 | 1;          // FLUSH_CACHE2
            s[n++] = WAIT_ALL;
            if (!cb->draws.empty()) {
                s[n++] = 34ULL << 56 | 0xf;                                     // REQ_RESOURCE: compute, fragment, tiler, IDVS
                if (!d->heap_set.exchange(true)) {                              // the queue's tiler heap, once
                    s[n++] = move48(0, d->heap_ctx);
                    s[n++] = 48ULL << 56;                                       // HEAP_SET
                }
            }
            uint8_t *pass_area = nullptr;           // the open Mali pass's first draw (emitDraw), or none
            uint64_t pass_va = 0;
            uint8_t *pass_ir = nullptr;             // ... and its incremental rendering (emitIr)
            uint64_t pass_ir_va = 0;
            for (size_t i = 0; i < cb->blits.size(); i++) {
                const Job &b = cb->blits[i];
                auto *pc = (uint8_t *)cb->stream.cpu + fau_at[i];
                if (b.shader == DRAW) {
                    const DrawJob &dj = cb->draws[b.dst];
                    bool first = pass_area == nullptr;
                    bool last = i + 1 >= cb->blits.size() || cb->blits[i + 1].shader != DRAW ||
                        !samePass(dj, cb->draws[cb->blits[i + 1].dst]);
                    if (first) {
                        pass_area = pc;
                        pass_va = cb->stream.va + fau_at[i];
                        pass_ir = ir_at[i] != 0 ? (uint8_t *)cb->stream.cpu + ir_at[i] : nullptr;
                        pass_ir_va = ir_at[i] != 0 ? cb->stream.va + ir_at[i] : 0;
                    }
                    // (metal_stats: STORE_STATE timestamp, opcode 40, into the command buffer's timestamps, r80:r81
                    // the address; after everything before it is done)
                    auto stamp = [&] {
                        if (cb->timestamps.size != 0 && (cb->timestamp_count + 1) * 8 <= cb->timestamps.size) {
                            s[n++] = WAIT_ALL;
                            s[n++] = move48(80, cb->timestamps.va + 8ULL * cb->timestamp_count++);
                            s[n++] = 40ULL << 56 | 80ULL << 40 | 0ULL << 32;
                        }
                    };
                    if (first) stamp();
                    emitDraw(d, dj, pc, cb->stream.va + fau_at[i], s, n, first, last, pass_area, pass_va, pass_ir, pass_ir_va);
                    if (last) stamp();
                    cb->draw_areas.push_back(fau_at[i]);
                    if (last) {
                        pass_area = nullptr;
                    }
                    continue;
                }
                uint64_t fau_va = cb->stream.va + fau_at[i], spd;
                unsigned fau_entries, size[3], groups[3];
                uint64_t local_storage = d->descriptors.va + 0x1200;            // none
                uint64_t resources = 0;
                if (b.shader == DISPATCH) {
                    const mali_kernel_binary &k = b.kernel->binary;
                    auto *w = (uint32_t *)pc;
                    memset(pc, 0, k.fau_words * 4);
                    memcpy(pc, b.buffers, sizeof(b.buffers));                       // words 2n, 2n+1
                    for (int c = 0; c < 3; c++) {
                        w[MALI_KERNEL_WORKGROUP_WORD + c] = b.threads[c];
                        w[MALI_KERNEL_GROUPS_WORD + c] = b.groups[c];
                        w[MALI_KERNEL_GRID_WORD + c] = b.grid[c];
                    }
                    memcpy(w + MALI_KERNEL_RESERVED, k.consts, (k.fau_words - MALI_KERNEL_RESERVED) * 4);
                    if (k.tls_size != 0 || b.wls_size != 0) {
                        // the local storage descriptor (Mesa's genxml v10 "Local Storage"): spill space, packed, per thread
                        // 16 << word 0 bytes; threadgroup memory 2^(word 1 & 31) instances of 2^((word 1 >> 8) - 1) bytes
                        uint64_t at = (k.fau_words * 4 + 0x3f) & ~0x3fULL;
                        auto *ls = (uint32_t *)(pc + at);
                        memset(ls, 0, 32);
                        if (k.tls_size != 0) {
                            ls[0] = log2u((k.tls_size + 15) / 16);
                            ls[2] = (uint32_t)(d->tls[ls[0]].va >> 8);
                            ls[3] = (uint32_t)(d->tls[ls[0]].va >> 40) | 1u << 28;
                        }
                        if (b.wls_size != 0) {
                            ls[1] = log2u(b.wls_instances) | (log2u(b.wls_size) + 1) << 8;
                            ls[4] = (uint32_t)d->wls.va;
                            ls[5] = (uint32_t)(d->wls.va >> 32);
                        } else {
                            ls[1] = 31;                                         // no threadgroup memory
                        }
                        local_storage = fau_va + at;
                    }
                    if (b.texture_count || b.sampler_count) {
                        // the resource table (r0): 16-byte entries, table t's descriptors at entry t (gallium's
                        // numbering: sampler 3, texture 4, image 5); each texture once in the texture table, once in the
                        // image table, its plane after them
                        uint64_t at = ((k.fau_words * 4 + 0x3f) & ~0x3fULL) + 0x40;
                        uint8_t *rt = pc + at;
                        uint64_t rt_va = fau_va + at, tex_va = rt_va + 0x80;
                        uint64_t samp_va = tex_va + b.texture_count * 32, img_va = samp_va + b.sampler_count * 32;
                        uint64_t plane_va = img_va + b.texture_count * 32;
                        memset(rt, 0, 0x80);
                        auto entry = [&](unsigned table, uint64_t va, unsigned count) {
                            auto *e = (uint32_t *)(rt + 16 * table);
                            e[0] = (uint32_t)va;
                            e[1] = (uint32_t)(va >> 32) | 1u << 24;                 // contains descriptors
                            e[2] = count * 32;
                        };
                        entry(MALI_TABLE_TEXTURE, tex_va, b.texture_count);
                        entry(MALI_TABLE_SAMPLER, samp_va, b.sampler_count);
                        entry(MALI_TABLE_IMAGE, img_va, b.texture_count);
                        for (unsigned t = 0; t < b.texture_count; t++) {
                            auto *tex = (uint32_t *)(pc + (tex_va - fau_va) + 32 * t);
                            auto *img = (uint32_t *)(pc + (img_va - fau_va) + 32 * t);
                            uint32_t plane[8];
                            if (b.textures[t] == nullptr) {
                                memset(tex, 0, 32);
                                memset(img, 0, 32);
                                continue;
                            }
                            // (one plane, shared by the two descriptors; 32-byte aligned as everything here)
                            uint64_t pva = plane_va;
                            plane_va = pva + 32;
                            describe(b.textures[t], tex, plane, pva);
                            memcpy(pc + (pva - fau_va), plane, 32);
                            memcpy(img, tex, 32);
                        }
                        for (unsigned t = 0; t < b.sampler_count; t++) {
                            memcpy(pc + (samp_va - fau_va) + 32 * t, b.samplers[t], 32);
                        }
                        resources = rt_va | MALI_TABLES;
                    }
                    spd = b.kernel->spd;
                    fau_entries = (k.fau_words + 1) / 2;
                    memcpy(size, b.threads, sizeof(size));
                    memcpy(groups, b.groups, sizeof(groups));
                } else {
                    memset(pc, 0, 32);
                    memcpy(pc + 0, &b.dst, 8);
                    memcpy(pc + 8, &b.src, 8);
                    memcpy(pc + 16, &b.value, 4);
                    memcpy(pc + 20, &b.count, 4);
                    if (b.shader == COPY_ROWS) {
                        memcpy(pc + 24, &b.src_stride, 4);
                        memcpy(pc + 28, &b.dst_stride, 4);
                        spd = d->spd[4];
                    } else {
                        spd = d->spd[b.shader];
                    }
                    fau_entries = 4;                                                // 8 words: 4 of 64 bits
                    size[0] = 64, size[1] = size[2] = 1;                            // 64 invocations a workgroup
                    groups[0] = (b.count + 63) / 64, groups[1] = groups[2] = 1;
                }
                // A COMPUTE JOB AFTER OTHER WORK OF THIS COMMAND BUFFER (11-10): the load/store caches cleaned and the
                // texture caches dropped first, as a render pass now opens (pass_inv) -- a blit or kernel reading what a
                // pass or another job just wrote (CoreAnimation's backdrop blurs copy what is drawn so far) could read a
                // core's stale texture lines: the widgets page's and the App Library's blurred backgrounds came out
                // empty or with noise for a frame (the tester's clips). Jobs were only ordered (WAIT_ALL).
                if (i > 0) {
                    s[n++] = move32(74, 0);
                    s[n++] = 36ULL << 56 | 74ULL << 40 | 2 << 8 | 1 << 4;      // FLUSH_CACHE2: LSC clean, others invalidated
                    s[n++] = WAIT_ALL;
                }
                s[n++] = move48(16, spd);
                s[n++] = move48(24, local_storage);
                s[n++] = move48(8, fau_va);
                s[n++] = move32(9, (uint32_t)(fau_va >> 32) | fau_entries << 24);
                s[n++] = move48(0, resources);                                      // the resource table, or none
                s[n++] = move32(32, 0);
                s[n++] = move32(33, (size[0] - 1) | (size[1] - 1) << 10 | (size[2] - 1) << 20);
                s[n++] = move32(34, 0);
                s[n++] = move32(35, 0);
                s[n++] = move32(36, 0);
                s[n++] = move32(37, groups[0]);
                s[n++] = move32(38, groups[1]);
                s[n++] = move32(39, groups[2]);
                s[n++] = RUN_COMPUTE;
                s[n++] = WAIT_ALL;                                                  // in order, as Metal's encoders are
            }
            s[n++] = move32(74, 0);
            s[n++] = 36ULL << 56 | 74ULL << 40 | 2 << 8 | 1 << 4 | 1;          // FLUSH_CACHE2, as at the start
            s[n++] = WAIT_ALL;
            __asm__ volatile("dsb sy" ::: "memory");
            syncobj_create sc = {};
            sync_op signal = {};
            queue_submit qs = {};
            group_submit gs = {};
            std::lock_guard<std::mutex> g(d->lock);
            if (d->call(SYNCOBJ_CREATE, &sc, sizeof(sc)) != 0) {
                cb->error = "no sync object";
                cb->status = HAKIOS_METAL_ERROR;
            } else {
                cb->sync = sc.handle;
                signal = { 1u << 31, sc.handle, 0 };
                qs.stream_size = n * 8;
                qs.stream_addr = cb->stream.va;
                qs.syncs = { sizeof(signal), 1, (uint64_t)(uintptr_t)&signal };
                gs.group_handle = d->group;
                gs.queue_submits = { sizeof(qs), 1, (uint64_t)(uintptr_t)&qs };
                int submitted = d->call(GROUP_SUBMIT, &gs, sizeof(gs));
                cb->submitted_at = std::chrono::steady_clock::now();
                if (submitted != 0) {
                    cb->error = "HakiosGPU did not take the submission";
                    cb->status = HAKIOS_METAL_ERROR;
                } else {
                    cb->status = HAKIOS_METAL_SCHEDULED;
                }
            }
        }
    } else {
        // nothing recorded: done at once, in its turn
        syncobj_create sc = {};
        sc.flags = 1;           // signalled
        std::lock_guard<std::mutex> g(d->lock);
        if (d->call(SYNCOBJ_CREATE, &sc, sizeof(sc)) == 0) {
            cb->sync = sc.handle;
            cb->status = HAKIOS_METAL_SCHEDULED;
        } else {
            cb->status = HAKIOS_METAL_ERROR;
        }
    }
    // THE SCHEDULED HANDLERS NOW (11-10), the work submitted and not yet the queue thread's (completed comes after):
    // CoreAnimation presents its frame from one, and the framebuffer waits for what was submitted before the
    // presentation (MiniIOSurfaceDrawn). Run before the submission, as they were, the frame's own work was not among it:
    // the panel showed surfaces the Mali had not drawn yet -- icons missing, an older page through the new one, bands
    // of half-written tiles (the tester's slow-motion clips of 11-10, the App Library and sb_cube=1's cube)
    if (cb->scheduled != nullptr) {
        auto scheduled = cb->scheduled;
        cb->scheduled = nullptr;
        scheduled(cb, cb->scheduled_context);
    }
    {
        std::lock_guard<std::mutex> l(cb->queue->lock);
        cb->queue->pending.push_back(cb);
    }
    cb->queue->work.notify_all();
}

void
hakios_metal_cb_on_scheduled(hakios_metal_cb *cb, void (*scheduled)(hakios_metal_cb *, void *), void *context)
{
    cb->scheduled = scheduled;
    cb->scheduled_context = context;
}

void
hakios_metal_cb_wait(hakios_metal_cb *cb)
{
    std::unique_lock<std::mutex> l(cb->lock);
    cb->done.wait(l, [&] { return cb->status == HAKIOS_METAL_COMPLETED || cb->status == HAKIOS_METAL_ERROR ||
                                  cb->status == HAKIOS_METAL_NOT_ENQUEUED; });
}

enum hakios_metal_status
hakios_metal_cb_status(hakios_metal_cb *cb)
{
    return cb->status;
}

const char *
hakios_metal_cb_error(hakios_metal_cb *cb)
{
    return cb->error.c_str();
}

int64_t
hakios_metal_cb_gpu_nanoseconds(hakios_metal_cb *cb)
{
    (void)cb;
    return 0;
}

// ---- kernels (src/mali/compile_mali.c) and their dispatch ----

unsigned
hakios_metal_bitcode_constant_count(const void *bitcode, size_t size)
{
    return bitcode != nullptr && size != 0 ? mali_bitcode_constant_count(bitcode, size) : 0;
}
}  // extern "C"
