// mali_internal.h: the Mali back end's shared declarations -- types, tables, inline helpers and the prototypes of
// what src/mali/mali_*.cpp define (split from c_api_mali.cpp, 11-24)
#pragma once
// hakios_metal.h on the Pixel 8a's Mali-G715 (hakios-kernel devices/akita: HakiosGPU with Linux's panthor interface):
// what the Metal plug-in (plugin/HakiosMetal.m) asks of the GPU today -- the device, buffers, queues, command buffers
// with blit copies and fills -- done by the Mali. Built instead of src/c_api.cpp (`make ios-mali`); the Adreno's
// classes are not in it.
//
//   device          HakiosGPU opened (its "DRMDriver" property says panthor), a VM, one group with one queue
//   buffer          a BO of the service, write-combined for the processors, bound into the VM at an address of ours
//   command buffer  its copies and fills recorded; at commit one command stream of the GPU's (in a BO of its own)
//                   dispatches the blit shaders (src/mali/blit_shaders.h: Mesa 26.2.5's compiler for the G715, made by
//                   hakios-kernel's tools/mesa/pan/pan_blit_shaders.c), each after a WAIT for the one before, and is
//                   submitted with a sync object; the queue's thread waits for it, sets the status, runs the callback
//
//   kernel          a compute function's AIR compiled by src/mali/compile_mali.c (AIR -> NIR -> Mesa's panfrost); its
//                   code goes into a BO of the device the first time it is dispatched there
//   dispatch        recorded among the blits, in order; at commit a shader program descriptor, push constants (the
//                   buffers' addresses, the threadgroup's and the grid's sizes, the compiler's constants) and RUN_COMPUTE
//
// Kernels and buffers are counted references: a command buffer holds those it uses until the GPU is done with them,
// whatever their owners free meanwhile.
//
// Everything else of hakios_metal.h is not here yet: a plug-in that calls it does not link (on purpose).
#include "hakios-metal/hakios_metal.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IODataQueueClient.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#include <chrono>
#include <sys/stat.h>
#include <libkern/OSCacheControl.h>
#include <sys/time.h>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <mach-o/loader.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <set>
#include <vector>

#include "blit_shaders.h"
#include "compile_mali.h"

namespace mali {

// ---- panthor's interface, as much as is used (Linux's include/uapi/drm/panthor_drm.h, drm.h: MIT) ----
struct obj_array { uint32_t stride, count; uint64_t array; };
struct sync_op { uint32_t flags, handle; uint64_t timeline_value; };
struct dev_query { uint32_t type, size; uint64_t pointer; };
struct gpu_info {
    uint32_t gpu_id, gpu_rev, csf_id, l2_features, tiler_features, mem_features, mmu_features, thread_features,
        max_threads, thread_max_workgroup_size, thread_max_barrier_size, coherency_features, texture_features[4],
        as_present, selected_coherency;
    uint64_t shader_present, l2_present, tiler_present;
    uint32_t core_features, pad;
    uint64_t gpu_features;
};
struct vm_create { uint32_t flags, id; uint64_t user_va_range; };
struct bind_op { uint32_t flags, bo_handle; uint64_t bo_offset, va, size; obj_array syncs; };
struct vm_bind { uint32_t vm_id, flags; obj_array ops; };
struct bo_create { uint64_t size; uint32_t flags, exclusive_vm_id, handle, pad; };
struct queue_create { uint8_t priority, pad[3]; uint32_t ringbuf_size; };
struct group_create {
    obj_array queues;
    uint8_t max_compute_cores, max_fragment_cores, max_tiler_cores, priority;
    uint32_t pad;
    uint64_t compute_core_mask, fragment_core_mask, tiler_core_mask;
    uint32_t vm_id, group_handle;
};
struct queue_submit { uint32_t queue_index, stream_size; uint64_t stream_addr; uint32_t latest_flush, pad; obj_array syncs; };
struct group_submit { uint32_t group_handle, pad; obj_array queue_submits; };
struct gem_close { uint32_t handle, pad; };
struct syncobj_create { uint32_t handle, flags; };
struct syncobj_destroy { uint32_t handle, pad; };
struct syncobj_wait { uint64_t handles; int64_t timeout_nsec; uint32_t count_handles, flags, first_signaled, pad; uint64_t deadline; };
enum : uint32_t {
    GEM_CLOSE = 0x09, DEV_QUERY = 0x40, VM_CREATE, VM_DESTROY, VM_BIND, VM_GET_STATE, BO_CREATE, BO_MMAP_OFFSET,
    GROUP_CREATE, GROUP_DESTROY, GROUP_SUBMIT, SYNCOBJ_CREATE = 0xbf, SYNCOBJ_DESTROY = 0xc0, SYNCOBJ_WAIT = 0xc3,
};
constexpr uint32_t BIND_READONLY = 1, BIND_NOEXEC = 2, BIND_UNMAP = 1u << 28;

inline thread_local std::string gError;
void fail(const char *format, ...);

// ---- the GPU's command stream (v11: v10's instructions with v12's scoreboard state; as hakios-kernel's mali_test) ----
inline uint64_t move48(unsigned r, uint64_t v) { return 1ULL << 56 | (uint64_t)r << 48 | (v & 0xffffffffffffULL); }
inline uint64_t move32(unsigned r, uint32_t v) { return 2ULL << 56 | (uint64_t)r << 48 | v; }
inline uint64_t set_state(unsigned state, uint32_t v) { return 28ULL << 56 | (uint64_t)state << 32 | v; }
constexpr uint64_t RUN_COMPUTE = 4ULL << 56 | 1, WAIT_ALL = 3ULL << 56 | 0xffffULL << 16;
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
namespace mali {
bool bootArg(const char *word);         // (a word in the boot-args: defined below)
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)

struct Bo {
    uint32_t handle = 0;
    uint64_t size = 0, va = 0;
    void *cpu = nullptr;
    bool driver_mapped = false;         // mapped by HakiosGPU (0x7c): undone by GEM_CLOSE, not by IOConnectUnmapMemory64
    uint32_t bind = ~0u;                // newBo's bind flags (~0: not newBo's -- a surface's: never kept in the pool)
    void *ext = nullptr;                // (hakios_metal_open_iogpu) the caller's cookie for the memory, given back to ops->free
};

/*
 * THE SECOND WAY TO THE KERNEL (11-26, boot-arg metal_iogpu=1): Apple's IOGPU interface -- IOServiceOpen type 1 on the
 * same service, the kernel's IOGPUDeviceUserClient (akios_iogpu.cpp, with the Mali's parts in HakiosGPU) -- instead of
 * panthor's requests on type 0. It is where the driver is going (IOGPU.framework's interface); panthor's stays the
 * default until this one has been run on the phone as long.
 *
 * What differs for the back end:
 *   memory    a resource (selector 8) instead of BO_CREATE + map + VM_BIND: the kernel maps it into the task and into
 *             the client's one GPU address space at the SAME address (no address of ours any more), write-combined for
 *             the processors (the GPU does not snoop their caches); a Bo's handle is the resource's id
 *   surfaces  a resource of kind 0x82 (the IOSurface's id); refused for a surface around a client's memory, as
 *             panthor's surface buffer is
 *   queue     one IOGPU command queue for the device (the kernel makes it a CSF group of one queue in our address
 *             space, as GROUP_CREATE did), and its notification queue: every submission (trap 0: the stream's address
 *             and length) is answered twice there -- "scheduled" at once, "completed" when the GPU has passed it, with
 *             a status (0, 0xb a page fault, 1/0xa refused or abandoned). A thread of ours takes the answers off and
 *             hands each completed token to whoever waits for it (instead of SYNCOBJ_WAIT)
 *   heap      the Mali's own selectors 56/57 on the type-1 connection (instead of TILER_HEAP_CREATE/DESTROY)
 * What the type-1 interface does not have yet, and is still asked of a type-0 connection opened beside it (with no VM,
 * no group, no buffers of its own): DEV_QUERY's description of the GPU, an IOSurface's description (0x7e with no buffer
 * wanted), and the surface marks the display waits for (0x7d busy, 0x80 not on the panel, 0x7f the kernel's log). They
 * move over when the kernel has them on type 1.
 */
struct IOGPUTransport {
    io_connect_t conn = 0;                      // the type-1 connection (0: this transport is not used)
    uint32_t queue = 0;                         // the command queue (selector 6)
    uint32_t nq = 0;                            // its notification queue (selector 14)
    uint32_t kcmd_shmem = 0, seg_shmem = 0;     // a command buffer's kernel commands and segment list (selector 12)
    IODataQueueMemory *nq_memory = nullptr;
    mach_port_t port = MACH_PORT_NULL;          // the notification queue's "data available" port
    std::thread completions;
    std::mutex lock;                            // around what follows
    std::condition_variable finished;
    std::map<uint64_t, uint32_t> done;          // completed tokens nobody has collected yet -> their status
    std::set<uint64_t> given_up;                // tokens whose waiter stopped waiting (their answer is dropped)
    uint64_t next_seq = 1;
    unsigned outstanding = 0;                   // submitted, completion not seen
    bool stopping = false;

    bool open(io_service_t service);            // false with the reason (fail)
    void close();
    bool newMemory(Bo &bo, uint64_t size, bool gpu_read_only);
    bool newSurface(Bo &bo, unsigned surface, uint64_t size);
    void freeMemory(Bo &bo);
    bool newHeap(uint32_t chunk_size, uint32_t initial, uint32_t most, uint32_t in_flight, uint32_t &handle,
                 uint64_t &ctx, uint64_t &first);
    void freeHeap(uint32_t handle);
    bool submit(uint64_t stream_va, uint32_t bytes, uint64_t &token);  // token: the "completed" one to wait for
    int wait(uint64_t token, int64_t timeout_ms, uint32_t &status);    // 0 (status set), -1 not in time

private:
    bool sysMemory(uint64_t size, bool gpu_read_only, uint32_t &id, uint64_t &gpu, uint64_t &cpu);
    void deleteResource(uint32_t id);
    void run();
};

struct hakios_metal_device {
    io_connect_t connection = 0;                // panthor's interface (type 0; beside IOGPU: the queries only)
    IOGPUTransport iogpu;                       // Apple's IOGPU interface, when metal_iogpu=1 (iogpu.conn != 0)
    // OVER IOGPU.FRAMEWORK (hakios_metal_open_iogpu, mali_external.cpp): the plug-in's IOGPUMetalDevice owns the
    // connection, the queues and the submission; every piece of memory the core needs comes from the caller's ops (an
    // IOGPU resource on that connection: the one GPU address space the streams run in); nothing here opens the
    // service, and connection and iogpu stay unused (0)
    bool external = false;
    io_connect_t shared_conn = 0;               // IOGPU.framework's connection: the Mali's vendor selectors only
    hakios_metal_memory_ops ext_ops = {};
    hakios_metal_queue *ext_queue = nullptr;    // what every external command buffer names as its queue (no thread)
    uint64_t gpu_id = 0, shaders = 0, tiler = 0;
    uint32_t max_threads = 0, max_tasks = 0, core_range = 0;   // a core's threads and tasks; highest core id + 1
    Bo wls;                                     // threadgroup memory, grown as dispatches need
    std::mutex wide_lock;                       // the most targets a draw had, by kind of target (passWide)
    std::map<uint64_t, unsigned> wide_kinds;
    // (11-20) SPILL SPACE, A BUFFER FOR EACH SIZE A THREAD (16 << n bytes): the thread storage descriptor gives the GPU a
    // base and that size, and thread t of core c takes base + (c * threads + t) * size. One buffer for every size made
    // threads of two jobs that run at once share bytes -- a fragment job with 16- or 64-byte slots wrote into the
    // 512-byte slots of the vertex job beside it: Maps' terrain vertex shader (the first that spills much, 288 bytes)
    // got its buffers' addresses and its vertex index back from there as noise -- GPU page faults at addresses made of
    // floats, the queue restarted over and over. (Mesa: one size, the largest, for a whole command buffer.)
    Bo tls[12];
    std::vector<Bo> retired;                    // grown out of (a command buffer in flight may use one): freed at close
    uint32_t vm = 0, group = 0;
    uint64_t next_va = 0x100000000ULL;          // our addresses: the lower half is the program's (not used with IOGPU: its addresses are the task's)
    std::mutex lock;                            // around the service's calls and next_va
    Bo descriptors;                             // the blit shaders' code, their program descriptors, the local storage
    uint64_t spd[5] = {};
    // draws: the queue's one tiler heap (HEAP_SET once a stream; a second heap on the same queue was not taken), its
    // descriptor and 64 KiB geometry buffer (heapdesc), the tiler's levels
    uint32_t heap_handle = 0, tiler_levels = 0;
    uint64_t heap_ctx = 0, heap_first = 0;
    std::atomic<bool> heap_set{false};          // the queue given its heap (HEAP_SET once a queue, as panvk and gallium)
    Bo heapdesc;
    bool heap_ok = false;                       // the heap made: draws can be served
    struct hakios_metal_program *preload_for[5][8] = {};   // the Load action's draw for 0 to 4 targets, with depth (1)
    bool preload_failed_for[5][8] = {};                    // and/or stencil (2) loaded too, multisample (4) (preloadProgram)
    struct hakios_metal_program *resolve_prog = nullptr;   // the driver's resolve draw (resolveProgram), under preload_lock
    bool resolve_failed = false;
    std::mutex preload_lock;
    struct hakios_metal_program *blit_prog = nullptr;  // the driver's blit draw (blitProgram), under preload_lock
    bool blit_failed = false;
    // where a colour output with no attachment goes (DrawJob::More::missing): the GPU writes such a target back even
    // with its write disabled -- at address 0 it faulted (AS1, a write at 0x80200) and the queue stopped
    Bo scratch_rt;
    uint64_t scratch_rt_size = 0;
    std::mutex scratch_lock;
    // WHAT STANDS FOR A RESOURCE A FUNCTION DECLARES AND NOTHING IS BOUND AT (11-12): Metal lets a draw or dispatch go
    // with such a hole as long as the function does not use it on the road it takes (MPS binds what a layer has);
    // refusing it stopped mediaserverd's networks. A buffer: 64 KiB of zeroes; a texture: 4x4 of zeroes.
    Bo nothing_bo;
    std::atomic<uint64_t> nothing_va{0};
    hakios_metal_texture *nothing_texture = nullptr;
    std::string preload_error;

    int call(uint32_t selector, void *request, size_t size)
    {
        size_t out = size;
        return IOConnectCallStructMethod(connection, selector, request, size, request, &out) == KERN_SUCCESS ? 0 : -1;
    }

    // THE POOL (11-15): buffers given back are kept a moment and handed out again, zeroed, to whoever asks for the same
    // size -- making one is three calls into the kernel (create, map, bind) and giving one back three more, and what
    // draws makes and frees them by the frame: a texture for every CoreGraphics queue's render, a buffer for every
    // -newBufferWithBytes:; a quarter of the WebGL aquarium's GPU process's time went there (metal_sample=1000).
    // Data buffers only (BIND_NOEXEC), 16 MB at most each and 64 MB in all, not older than 2 s. metal_bopool=0: none.
    // (... and not kept by a process that has gone idle: a thread of the device's looks every second while there is a
    // device; hakios_metal_close cuts it off through `tidy`)
    struct Pooled { Bo bo; std::chrono::steady_clock::time_point at; };
    struct PoolTidy { std::mutex m; hakios_metal_device *d = nullptr; };
    std::shared_ptr<PoolTidy> tidy;
    std::vector<Pooled> pool;
    uint64_t pool_bytes = 0, pool_hits = 0, pool_misses = 0;
    static constexpr uint64_t kPoolMostEach = 16ull << 20, kPoolMost = 64ull << 20;

    void poolTrim(std::chrono::steady_clock::time_point now)        // (lock held)
    {
        size_t keep = 0;
        for (size_t i = 0; i < pool.size(); i++) {
            if (now - pool[i].at > std::chrono::seconds(2)) {
                pool_bytes -= pool[i].bo.size;
                freeBoNow(pool[i].bo);
            } else {
                if (keep != i) pool[keep] = pool[i];
                keep++;
            }
        }
        pool.resize(keep);
    }

    // (external) SIZE bytes from the caller's ops (lock held); no allocation of 4 GB or less across a 4 GB boundary (a
    // local storage descriptor's address must not carry over 32 bits): one that does is made again at twice the size
    // and the piece on one side of the boundary used, as IOGPUTransport::newMemory does
    bool extAlloc(Bo &bo, uint64_t size, uint32_t bind_flags)
    {
        hakios_metal_memory m = {};
        uint32_t flags = bind_flags == BIND_READONLY ? HAKIOS_METAL_MEMORY_GPU_READ_ONLY : 0;
        if (ext_ops.alloc == nullptr || ext_ops.alloc(ext_ops.context, size, flags, &m) != 0 || m.gpu_va == 0 || m.cpu == nullptr) {
            if (m.cookie != nullptr) ext_ops.free(ext_ops.context, m.cookie);
            fail("no GPU memory of %llu bytes (the plug-in's IOGPU resources)", (unsigned long long)size);
            return false;
        }
        uint64_t skip = 0;
        if (size <= (1ULL << 32) && m.gpu_va >> 32 != (m.gpu_va + size - 1) >> 32) {
            ext_ops.free(ext_ops.context, m.cookie);
            m = {};
            if (ext_ops.alloc(ext_ops.context, size * 2, flags, &m) != 0 || m.gpu_va == 0 || m.cpu == nullptr) {
                if (m.cookie != nullptr) ext_ops.free(ext_ops.context, m.cookie);
                fail("no GPU memory of %llu bytes (the plug-in's IOGPU resources)", (unsigned long long)size * 2);
                return false;
            }
            if (m.gpu_va >> 32 != (m.gpu_va + size - 1) >> 32) {
                skip = ((m.gpu_va + size - 1) & ~0xffffffffULL) - m.gpu_va;
            }
        }
        bo = Bo();
        bo.handle = m.resource_id != 0 ? m.resource_id : 1;     // (non-zero: the memory is the device's to give back)
        bo.size = size;
        bo.va = m.gpu_va + skip;
        bo.cpu = (uint8_t *)m.cpu + skip;
        bo.ext = m.cookie;
        return true;
    }

    bool newBo(Bo &bo, uint64_t size, uint32_t bind_flags)
    {
        std::lock_guard<std::mutex> g(lock);
        bo_create c = {};
        c.size = (size + 0x3fff) & ~0x3fffULL;
        for (size_t i = pool.size(); i-- > 0;) {
            if (pool[i].bo.size == c.size && pool[i].bo.bind == bind_flags) {
                bo = pool[i].bo;
                pool_bytes -= bo.size;
                pool.erase(pool.begin() + (long)i);
                pool_hits++;
                memset(bo.cpu, 0, (size_t)bo.size);     // (a new buffer is zeroes)
                return true;
            }
        }
        pool_misses++;
        if (external) {
            if (!extAlloc(bo, c.size, bind_flags)) {
                return false;
            }
            bo.bind = bind_flags;
            return true;
        }
        if (iogpu.conn != 0) {
            if (!iogpu.newMemory(bo, c.size, bind_flags == BIND_READONLY)) {
                return false;
            }
            bo.bind = bind_flags;
            return true;
        }
        if (call(BO_CREATE, &c, sizeof(c)) != 0) {
            fail("no GPU memory of %llu bytes", (unsigned long long)size);
            return false;
        }
        if (!mapAndBind(bo, c.handle, c.size, bind_flags)) {
            return false;
        }
        bo.bind = bind_flags;
        return true;
    }

    // a buffer the kernel holds (HANDLE, SIZE) mapped for the CPU and bound into the GPU's address space (lock held)
    bool mapAndBind(Bo &bo, uint32_t handle, uint64_t size, uint32_t bind_flags)
    {
        // HakiosGPU's 0x7c: mapped by the driver, which keeps the mapping and undoes it at GEM_CLOSE. NOT
        // IOConnectMapMemory64/IOConnectUnmapMemory64: for an IOSurface's buffer the unmap found the program's own
        // IOSurface mapping of the same memory instead and the buffer's stayed -- every surface a program had
        // drawn or sampled kept its memory wired until the program ended (backboardd: never; 11-13, the tester's
        // Speedometer run took the phone down at 4.6 GB wired). A kernel without 0x7c: as before.
        mach_vm_address_t address = 0;
        struct { uint32_t handle, pad; uint64_t address, size; } m = { handle, 0, 0, 0 };
        static const bool old_map = bootArg("metal_oldmap");    // (metal_oldmap=1: IOConnectMapMemory64 as before, to compare)
        bool own = !old_map && call(0x7c, &m, sizeof(m)) == 0 && m.address != 0;
        if (own) {
            address = m.address;
        } else {
            mach_vm_size_t mapped = size;
            if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != KERN_SUCCESS) {
                fail("a buffer could not be mapped");
                return false;
            }
        }
        bo.driver_mapped = own;
        bo.handle = handle;
        bo.size = size;
        bo.cpu = (void *)(uintptr_t)address;
        if (next_va >> 32 != (next_va + size - 1) >> 32) {
            next_va = (next_va + size - 1) & ~0xffffffffULL;    // (none across 4 GB: the local storage's must not)
        }
        bo.va = next_va;
        next_va += size + 0x4000;               // a page between them
        bind_op op = {};
        op.flags = bind_flags;
        op.bo_handle = handle;
        op.va = bo.va;
        op.size = size;
        vm_bind b = { vm, 0, { sizeof(op), 1, (uint64_t)(uintptr_t)&op } };
        if (call(VM_BIND, &b, sizeof(b)) != 0) {
            fail("a buffer could not be bound into the GPU's address space");
            return false;
        }
        return true;
    }

    // An IOSurface's memory as a buffer (HakiosGPU's 0x7e, as the Adreno service's), shared by every texture over it:
    // the kernel gives the same handle for the same surface, so it is closed with the last of them. false with the
    // reason; INFO (size, offset, row bytes, width, height, pixel format, planes) filled.
    struct SurfaceInfo { uint64_t size; uint32_t offset, row_bytes, width, height, pixel_format, planes; };
    // A SURFACE'S MIRROR (11-10): a texture of 64-byte rows standing for an IOSurface whose rows the Mali cannot sample
    // (mirrorOf), shared by every texture over that surface; copied from the surface on the GPU when made and when the
    // plug-in says the CPU changed the surface, copied back by each command buffer that draws into it (at its commit)
    struct Mirror { hakios_metal_texture *tex; int refs; uint64_t surface_va; uint32_t row_bytes, row_words, rows; };
    std::map<uint64_t, Mirror> mirrors;     // by mirrorKey: the surface's id, a later plane's place in it above (11-20)
    std::mutex mirror_lock;
    hakios_metal_queue *mirror_queue = nullptr;
    struct Surface { Bo bo; SurfaceInfo info; int refs; };
    std::map<unsigned, Surface> surfaces;
    bool surfaceBo(unsigned id, Bo &bo, SurfaceInfo &info)
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = surfaces.find(id);
        if (it != surfaces.end()) {
            it->second.refs++;
            bo = it->second.bo;
            info = it->second.info;
            return true;
        }
        struct { uint32_t surface, handle; uint64_t size; uint32_t offset, row_bytes, width, height, format, planes; } r = {};
        r.surface = id;
        if (iogpu.conn != 0) {
            r.handle = 0xffffffffu;             // (the description only: the memory comes as an IOGPU resource)
        }
        int e = call(0x7e, &r, sizeof(r));
        if (e != 0) {
            fail("IOSurface %u could not be had as GPU memory (HakiosGPU: 0x%x)", id, e);
            return false;
        }
        Surface sf = {};
        sf.info = { r.size, r.offset, r.row_bytes, r.width, r.height, r.format, r.planes };
        if (iogpu.conn != 0) {
            if (!iogpu.newSurface(sf.bo, id, r.size)) {
                return false;
            }
        } else if (!mapAndBind(sf.bo, r.handle, r.size, BIND_NOEXEC)) {
            gem_close gc = { r.handle, 0 };
            call(GEM_CLOSE, &gc, sizeof(gc));
            return false;
        }
        sf.refs = 1;
        surfaces[id] = sf;
        bo = sf.bo;
        info = sf.info;
        return true;
    }
    void surfaceRelease(unsigned id)
    {
        Bo bo;
        {
            std::lock_guard<std::mutex> g(lock);
            auto it = surfaces.find(id);
            if (it == surfaces.end() || --it->second.refs > 0) {
                return;
            }
            bo = it->second.bo;
            surfaces.erase(it);
        }
        freeBo(bo);
    }

    // COMMAND BUFFERS' STREAM BUFFERS, KEPT FOR THE NEXT: a buffer made, mapped and bound for each command buffer and
    // unbound after cost 100-400 us of the commit (metal_cbtime); sizes powers of two from 64 KiB, at most 16 kept
    std::vector<Bo> stream_pool;
    bool takeStream(Bo &bo, uint64_t size)
    {
        uint64_t want = 64 * 1024;
        while (want < size) want <<= 1;
        {
            std::lock_guard<std::mutex> g(lock);
            for (size_t i = 0; i < stream_pool.size(); i++) {
                if (stream_pool[i].size == want) {
                    bo = stream_pool[i];
                    stream_pool.erase(stream_pool.begin() + i);
                    return true;
                }
            }
        }
        return newBo(bo, want, BIND_NOEXEC);
    }
    void giveStream(Bo &bo)
    {
        {
            std::lock_guard<std::mutex> g(lock);
            if (bo.handle != 0 && stream_pool.size() < 16) {
                stream_pool.push_back(bo);
                bo = Bo();
                return;
            }
        }
        freeBo(bo);
    }

    // BUFFERS' MEMORY KEPT FOR THE NEXT BUFFER OF THE SAME SIZE (11-13): WebKit's WebGL (ANGLE) makes a buffer and
    // gives it back for every draw; made and freed through the kernel that is ~0.33 ms a draw, 200 ms of the WebGL
    // aquarium's frame (618 draws: 5 frames a second). Up to 256 KiB each, 384 of them, 24 MiB in all; boot-arg
    // metal_nobufpool: not done.
    std::vector<Bo> buffer_pool;
    uint64_t buffer_pool_bytes = 0;
    bool takeBuffer(Bo &bo, uint64_t size)
    {
        const uint64_t want = (size + 0x3fff) & ~0x3fffULL;     // (as newBo rounds)
        std::lock_guard<std::mutex> g(lock);
        for (size_t i = buffer_pool.size(); i-- > 0;) {
            if (buffer_pool[i].size == want) {
                bo = buffer_pool[i];
                buffer_pool[i] = buffer_pool.back();
                buffer_pool.pop_back();
                buffer_pool_bytes -= want;
                return true;
            }
        }
        return false;
    }
    bool giveBuffer(Bo &bo)
    {
        static const bool off = bootArg("metal_nobufpool");
        std::lock_guard<std::mutex> g(lock);
        if (off || external || bo.handle == 0 || bo.cpu == nullptr || bo.size > 256 * 1024 || buffer_pool.size() >= 384 ||
            buffer_pool_bytes + bo.size > (24ULL << 20)) {
            return false;
        }
        buffer_pool.push_back(bo);
        buffer_pool_bytes += bo.size;
        bo = Bo();
        return true;
    }

    void freeBo(Bo &bo)
    {
        std::lock_guard<std::mutex> g(lock);
        if (bo.handle == 0) {
            return;
        }
        static const bool pooling = !bootArg("metal_bopool=0");
        if (external) {
            freeBoNow(bo);                                  // (IOGPU resources: no pool of ours -- plan 2.1)
            return;
        }
        auto now = std::chrono::steady_clock::now();
        poolTrim(now);
        if (pooling && bo.bind == BIND_NOEXEC && bo.cpu != nullptr && bo.size <= kPoolMostEach && pool_bytes + bo.size <= kPoolMost) {
            pool.push_back({ bo, now });
            pool_bytes += bo.size;
            bo = Bo{};
            if (tidy == nullptr) {
                tidy = std::make_shared<PoolTidy>();
                tidy->d = this;
                std::thread([t = tidy] {
                    for (;;) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        std::lock_guard<std::mutex> alive(t->m);
                        if (t->d == nullptr) {
                            return;
                        }
                        std::lock_guard<std::mutex> g(t->d->lock);
                        t->d->poolTrim(std::chrono::steady_clock::now());
                    }
                }).detach();
            }
            return;
        }
        freeBoNow(bo);
    }

    void freeBoNow(Bo &bo)                              // (lock held)
    {
        if (external) {
            // (the kernel frees a deleted resource only after the client's earlier submissions have completed)
            if (bo.ext != nullptr && ext_ops.free != nullptr) {
                ext_ops.free(ext_ops.context, bo.ext);
            }
            bo = Bo();
            return;
        }
        if (iogpu.conn != 0) {
            iogpu.freeMemory(bo);                           // (out of the GPU's address space before the memory goes)
            bo = Bo();
            return;
        }
        bind_op op = {};
        op.flags = BIND_UNMAP;
        op.va = bo.va;
        op.size = bo.size;
        vm_bind b = { vm, 0, { sizeof(op), 1, (uint64_t)(uintptr_t)&op } };
        // (boot-arg metal_freetime, 11-13: what giving a buffer back costs, the unmap and the close apart -- 11 ms each
        // was measured, mali_draw_test's "what a buffer costs")
        static const bool timed = bootArg("metal_freetime");
        auto t0 = std::chrono::steady_clock::now();
        call(VM_BIND, &b, sizeof(b));
        auto t1 = std::chrono::steady_clock::now();
        if (!bo.driver_mapped) {
            IOConnectUnmapMemory64(connection, bo.handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)bo.cpu);
        }
        gem_close gc = { bo.handle, 0 };
        call(GEM_CLOSE, &gc, sizeof(gc));
        if (timed) {
            static double unmap_us, close_us;
            static unsigned n;
            auto t2 = std::chrono::steady_clock::now();
            unmap_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            close_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
            if (++n % 100 == 0) {
                fprintf(stderr, "hakios-metal: freetime: %u buffers given back: the unmap %.0f us each, the close %.0f us each\n", n,
                    unmap_us / 100, close_us / 100);
                unmap_us = close_us = 0;
            }
        }
        bo = Bo();
    }
};

// the zeroes that stand for a buffer or a texture a function declares and nothing is bound at (see the device's
// nothing_bo); 0 / nullptr if they cannot be made
// (per device, 11-26: the zeroes of the device whose draw this thread is recording -- nothingVa(d) sets it -- not one
// address for the process: two devices of one process have two GPU address spaces under IOGPU)
inline thread_local uint64_t gNothingVa = 0;
uint64_t nothingVa(hakios_metal_device *d);
hakios_metal_texture *nothingTexture(hakios_metal_device *d);
namespace mali {
bool grow(hakios_metal_device *d, Bo &b, uint64_t size);

inline uint32_t npot(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }
inline uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)

/*
 * A BUFFER'S SLACK (11-11): a page past what was asked for, filled with a pattern and looked at when the buffer goes --
 * backboardd died writing 0x750 bytes into the page after one (CA::OGL::MetalBufferPool::write_buffer, TODO 8i): an
 * overrun lands in the slack now and is said (the length, how far past it), instead of taking the window server down.
 */
constexpr uint64_t kBufferSlack = 16384;
constexpr uint8_t kSlackByte = 0xa5;

struct hakios_metal_buffer {
    hakios_metal_device *device;
    Bo bo;
    uint64_t length;
    std::atomic<int> refs{1};       // its owner's, and each command buffer's that uses it

    void retain() { refs.fetch_add(1); }
    void checkSlack()
    {
        auto *p = (const uint8_t *)bo.cpu;
        if (p == nullptr || bo.size <= length) return;
        // THE FIRST KILOBYTE PAST THE END, eight bytes at a time (11-13): the whole slack byte by byte through the
        // write-combined mapping took 11 ms for EVERY buffer given back (mali_draw_test, "what a buffer costs") --
        // WebKit's WebGL gives one back for every draw, and the aquarium's 618 a frame made a frame take 6 s after
        // the GPU had drawn it in a millisecond. What is written past an end begins at the end.
        // (THE FIRST 64 BYTES since the evening: the kilobyte was still a quarter of that GPU process's time,
        // metal_sample=1000 -- every read through that mapping goes to the memory itself.)
        const uint64_t upto = bo.size - length > 64 ? length + 64 : bo.size;
        uint64_t last = 0, i = length;
        for (; i < upto && (i & 7); i++) {
            if (p[i] != kSlackByte) last = i + 1;
        }
        for (; i + 8 <= upto; i += 8) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (w != 0xa5a5a5a5a5a5a5a5ULL) {
                for (unsigned k = 0; k < 8; k++) {
                    if (p[i + k] != kSlackByte) last = i + k + 1;
                }
            }
        }
        for (; i < upto; i++) {
            if (p[i] != kSlackByte) last = i + 1;
        }
        if (last != 0) {
            static std::atomic<unsigned> said{0};
            if (said++ < 20) {
                fprintf(stderr, "hakios-metal: A BUFFER WAS WRITTEN PAST ITS END: %llu bytes long, written to %llu bytes past "
                    "it (the slack caught it; TODO 8i)\n", (unsigned long long)length, (unsigned long long)(last - length));
            }
        }
    }
    void release()
    {
        if (refs.fetch_sub(1) == 1) {
            checkSlack();
            if (!device->giveBuffer(bo)) {
                device->freeBo(bo);
            }
            delete this;
        }
    }
};

struct hakios_metal_kernel {
    mali_kernel_binary binary = {};
    std::atomic<int> refs{1};
    std::mutex lock;                // around the code's upload
    hakios_metal_device *device = nullptr;
    Bo code;                        // the code, and its program descriptor at spd
    uint64_t spd = 0;

    void retain() { refs.fetch_add(1); }
    void release()
    {
        if (refs.fetch_sub(1) == 1) {
            if (device != nullptr) {
                device->freeBo(code);
            }
            mali_kernel_binary_free(&binary);
            delete this;
        }
    }

    unsigned maxThreads() const { return binary.work_regs <= 32 ? 1024 : 512; }

    // the code in the device's memory (once per kernel: one device in a process)
    bool upload(hakios_metal_device *d)
    {
        std::lock_guard<std::mutex> g(lock);
        if (device == d) {
            return true;
        }
        if (device != nullptr) {
            fail("%s: compiled for another device", binary.name);
            return false;
        }
        uint64_t at = (binary.size + 0x100 + 0x3f) & ~0x3fULL;     // (the shader core reads ahead of the code's end)
        if (!d->newBo(code, at + 0x40, BIND_READONLY)) {
            return false;
        }
        auto *bytes = (uint8_t *)code.cpu;
        memcpy(bytes, binary.code, binary.size);
        auto *w = (uint32_t *)(bytes + at);
        w[0] = 8 | 1 << 4 | (binary.work_regs <= 32 ? 2u : 0u) << 30;
        w[1] = (uint32_t)(binary.preload >> 48);
        w[2] = (uint32_t)code.va;
        w[3] = (uint32_t)(code.va >> 32);
        __asm__ volatile("dsb sy" ::: "memory");
        spd = code.va + at;
        device = d;
        return true;
    }
};
namespace mali {
bool bootArg(const char *word);         // (a word in the boot-args: defined below)
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)

struct hakios_metal_program {
    mali_kernel_binary vs = {}, fs = {};
    std::atomic<int> refs{1};
    std::mutex lock;
    hakios_metal_device *device = nullptr;
    Bo code;
    uint64_t pos_spd = 0, vary_spd = 0, frag_spd = 0;
    uint64_t pos_spd_no_psiz = 0;
    bool writes_psiz = false, extended_fifo = false, writes_layer = false;
    unsigned color_format = 0, samples = 1;
    bool blend_enabled = false, vertex_descriptor = false;
    // colour attachment 0's blend (hakios_metal_program_set_blend): the fixed-function equation word, whether it
    // reads the tile buffer, or why it cannot be done in fixed function
    uint32_t blend_equation = 0x132 | 0x132 << 12 | 0xfu << 28;    // replace, every channel written
    // the vertex descriptor (hakios_metal_program_set_vertex_*): the stages compiled again with the stage_in
    // attributes fetched in the shader (compile_mali.c's lower_vertex_fetch), once, before the first draw
    std::vector<uint8_t> vs_bitcode, fs_bitcode;
    std::vector<mali_constant> vs_constants, fs_constants;
    mali_vertex_fetch fetch = {};
    bool fetch_described = false, fetch_compiled = false;
    // the targets whose blend fixed function cannot do, blended in the fragment shader instead (compileFetch compiles it
    // again with mali_fragment_compile; their equation is a replace that reads the tile buffer)
    mali_shader_blend shader_blend = {};
    bool rt_blend_on[4] = {};                   // blending enabled at colour attachment n (hakios_metal_program_set_blend*)
    unsigned rt_format[4] = {};                 // colour attachment n's MTLPixelFormat (hakios_metal_program_set_attachment_format)
    std::string fetch_refusal;
    bool blend_reads_dest = false;
    std::string blend_refusal;
    // colour attachments 1 to 3's (hakios_metal_program_set_blend_at): the same, for the multiple-target draws
    uint32_t more_blend_equation[3] = { 0x132 | 0x132 << 12 | 0xfu << 28, 0x132 | 0x132 << 12 | 0xfu << 28,
                                        0x132 | 0x132 << 12 | 0xfu << 28 };
    bool more_blend_reads_dest[3] = {};
    std::string more_blend_refusal[3];
    char name[300] = "";
    char label[128] = "";       // the pipeline descriptor's (hakios_metal_program_set_label: diagnostics only)
    // a program specialised for a buffer's bytes (hakios_metal_program_specialize): its fragment stage compiled with
    // [[buffer(known_binding)]] read as KNOWN (compile_mali.c's mali_compile_known), every compile of it alike
    int known_binding = -1;
    std::vector<uint8_t> known;

    void retain() { refs.fetch_add(1); }
    void release()
    {
        if (refs.fetch_sub(1) == 1) {
            if (device != nullptr) {
                device->freeBo(code);
            }
            mali_kernel_binary_free(&vs);
            mali_kernel_binary_free(&fs);
            delete this;
        }
    }

    bool upload(hakios_metal_device *d)
    {
        std::lock_guard<std::mutex> g(lock);
        if (device == d) {
            return true;
        }
        if (device != nullptr) {
            fail("%s: made for another device", name);
            return false;
        }
        uint64_t fs_at = (vs.size + 0x100 + 0xff) & ~0xffULL, spd_at = (fs_at + fs.size + 0x100 + 0x3f) & ~0x3fULL;
        if (!d->newBo(code, spd_at + 0x100, BIND_READONLY)) {
            return false;
        }
        auto *bytes = (uint8_t *)code.cpu;
        memcpy(bytes, vs.code, vs.size);
        memcpy(bytes + fs_at, fs.code, fs.size);
        auto spd = [&](unsigned slot, unsigned stage, unsigned regs, uint64_t preload, uint64_t va, bool helpers) {
            auto *w = (uint32_t *)(bytes + spd_at + 0x40 * slot);
            // (bit 28, "requires helper threads", fragment only -- 11-11, TODO 8h: SET FOR EVERY FRAGMENT SHADER. Mesa sets
            // it for barriers alone and lets the compiler end the helper lanes after their last use (VA_FLOW_DISCARD after
            // the last CLPER / implicit-LOD texture); with CoreAnimation's shaders through our AIR front end that left the
            // derivatives wrong wherever a triangle covers part of a 2x2 quad -- a light seam along every edge of its
            // meshes: Control Center's tiles' dashed inner outlines, the diagonals of each quad's split (the round
            // buttons', the tiles' corner pieces). Kept to the end, they are right; no cost seen (idle cube 109-111 fps
            // either way). Why the compiler's own end of helpers is too early is not known yet. metal_nohelpers: barriers
            // only, as Mesa)
            static const bool no_helpers = bootArg("metal_nohelpers");
            w[0] = 8 | stage << 4 | (regs <= 32 ? 2u : 0u) << 30 |
                   ((helpers || (stage == 2 && !no_helpers)) ? 1u << 28 : 0u);
            w[1] = (uint32_t)(preload >> 48);
            w[2] = (uint32_t)va;
            w[3] = (uint32_t)(va >> 32);
            return code.va + spd_at + 0x40 * slot;
        };
        pos_spd = spd(0, 3, vs.work_regs, vs.preload, code.va, false);
        vary_spd = vs.secondary_offset ? spd(1, 3, vs.secondary_work_regs, vs.secondary_preload, code.va + vs.secondary_offset, false) : 0;
        frag_spd = spd(2, 2, fs.work_regs, fs.preload, code.va + fs_at, fs.helpers != 0);
        // a vertex function with a [[point_size]]: its variant that does not write one, for lines and triangles (the
        // position FIFO's basic entries have no room for it; Mesa's no_psiz variant)
        pos_spd_no_psiz = vs.writes_psiz && vs.no_psiz_offset ? spd(3, 3, vs.work_regs, vs.preload, code.va + vs.no_psiz_offset, false) : 0;
        writes_psiz = vs.writes_psiz != 0;
        extended_fifo = vs.extended_fifo != 0;
        {
            // (boot-arg metal_fetchsay, with metal_appsay inside an application: what the compiler made of the program)
            static const bool say = bootArg("metal_fetchsay");
            if (say) {
                fprintf(stderr, "hakios-metal: program %s: vertex %u bytes of code, %u push constant words, varyings %u bytes, work registers %u, "
                    "spilled %u bytes, psiz %d, extended fifo %d, layer %d; fragment %u bytes, %u words, spilled %u\n", vs.name, (unsigned)vs.size,
                    vs.fau_words, vs.varying_bytes, vs.work_regs, vs.tls_size, (int)vs.writes_psiz, (int)vs.extended_fifo, (int)vs.writes_layer,
                    (unsigned)fs.size, fs.fau_words, fs.tls_size);
                fprintf(stderr, "hakios-metal: program %s: the varying shader at +%u, work registers %u, preload %llx (the position shader's %llx); "
                    "fragment work registers %u, preload %llx\n", vs.name, vs.secondary_offset, vs.secondary_work_regs,
                    (unsigned long long)vs.secondary_preload, (unsigned long long)vs.preload, fs.work_regs, (unsigned long long)fs.preload);
            }
        }
        writes_layer = vs.writes_layer != 0;
        __asm__ volatile("dsb sy" ::: "memory");
        device = d;
        return true;
    }
};

// A texture: linear rows in a BO of its own (Mali's "Generic Plane", clump ordering linear), described to the GPU per
// dispatch. Metal's pixel formats served, as Mesa's lib/pan_format.c encodes them for v9+: RGB component order |
// format << 12 | sRGB << 20.
struct hakios_metal_texture {
    hakios_metal_device *device;
    Bo bo;                                      // the memory (its own, an IOSurface's, a buffer's or a parent's)
    unsigned type, format, width, height, depth, usage;
    uint32_t mali_format, bytes, stride;        // per texel (a compressed format's: per block); per row (of blocks;
                                                //  64-byte aligned for its own)
    unsigned bw = 1, bh = 1;                    // a compressed format's block (ASTC), in texels
    uint64_t stencil_off = 0;                   // Depth32Float_Stencil8: the stencil plane (a byte a texel) at offset +
    uint32_t stencil_stride = 0;                //  stencil_off, rows stencil_stride apart (0: none)
    uint64_t stencil_level_off[16] = {};        // ... mipmapped: each level's stencil plane (from gpu()), after all the
    uint32_t stencil_level_stride[16] = {};     //  depth levels (0: level 0's is stencil_off alone)
    uint64_t offset = 0;                        // where its pixels start in BO
    unsigned origin = 0;                        // 0 its own memory, 1 an IOSurface's, 2 a buffer's, 4 a view's
    unsigned surface = 0;                       // the IOSurface's ID (origin 1)
    bool mirrored = false;                      // ... its memory the surface's mirror (Mirror), not the surface's own
    uint64_t mirror_key = 0;                    // ... that mirror's key: the surface's id, | a later plane's offset << 32
    bool memoryless = false;                    // Metal's memoryless storage (hakios_metal_texture_set_memoryless): an
                                                //  extra colour attachment of it lives in the tile buffer alone
    std::atomic<unsigned> widest_rts{0};        // the most targets a draw with it as colour attachment 1 had (passWide)
    // (11-10, a diagnostic for metal_stats: CoreAnimation's second colour attachment -- is it ever read?)
    std::atomic<unsigned> as_extra_rt{0};       // draws it was colour attachment 1-3 of
    std::atomic<unsigned> reads{0};             // times it was sampled by a draw or dispatch, copied from, or read back
    unsigned levels = 1, samples = 1;
    // a mipmapped texture (levels > 1): level n's rows at offset + level_off[n], level_stride[n] apart, its size
    // levelWidth/levelHeight; the levels' plane descriptors (32 bytes each, level 0 first) at planes_off in BO, written
    // once when the texture is made (a view of levels n.. points at the parent's, 32 * n further on)
    uint64_t level_off[16] = {};
    uint32_t level_stride[16] = {};
    uint64_t planes_off = 0;                    // 0: none (one level, described per use)
    uint32_t swizzle = 0;                       // 0: the identity
    // A LAYERED TEXTURE (11-12: MTLTextureType 1DArray 1, 2DArray 3, Cube 5, CubeArray 6 -- a cube's faces are layers,
    // six a cube, in Metal's slice order, which is the Mali's): LAYERS of them, each the whole chain of its levels,
    // layer_stride bytes apart; the plane descriptors at planes_off layer by layer, each layer's levels in order
    // (Mesa's pan_emit_iview_texture_payload for v7 and later)
    unsigned layers = 1;
    uint64_t layer_stride = 0;
    hakios_metal_texture *parent = nullptr;     // a view's (held)
    // a depth or stencil texture drawn into with no colour attachment: the colour target a Mali fragment job has all the
    // same, RGBA8 of this size, never written or read -- made at the first such draw, kept with the texture
    std::atomic<hakios_metal_texture *> stand_in{nullptr};
    hakios_metal_buffer *buffer = nullptr;      // a buffer texture's (held)
    std::atomic<int> refs{1};

    void retain() { refs.fetch_add(1); }
    void release();
    uint8_t *pixels() const { return (uint8_t *)bo.cpu + offset; }
    uint64_t gpu() const { return bo.va + offset; }
    unsigned levelWidth(unsigned l) const { return width >> l ? width >> l : 1; }
    unsigned levelHeight(unsigned l) const { return type == 0 || type == 1 ? 1 : height >> l ? height >> l : 1; }
    uint32_t levelStride(unsigned l) const { return l == 0 ? stride : level_stride[l]; }
    unsigned levelRows(unsigned l) const { return (levelHeight(l) + bh - 1) / bh; }    // rows of blocks
    uint64_t levelOffset(unsigned l) const { return l == 0 ? 0 : level_off[l]; }
    // the bytes from gpu() that its pixels (every level, every slice) span
    uint64_t span() const {
        if (layers > 1) return layer_stride * layers;
        if (levels > 1) return level_off[levels - 1] + (uint64_t)level_stride[levels - 1] * levelRows(levels - 1);
        return (uint64_t)stride * levelRows(0) * depth * samples;    // (a multisample texture: a plane a sample)
    }
};

struct hakios_metal_sampler {
    uint32_t words[8];                          // genxml v10 "Sampler"
};

void mirrorRelease(hakios_metal_device *d, uint64_t key);
namespace mali {
struct MaliFormat { unsigned metal; uint32_t mali; uint32_t bytes; unsigned bw = 1, bh = 1; };  // (bytes: a block's)
inline const MaliFormat kFormats[] = {
    { 1, 163u << 12 | 16, 1 },                  // A8Unorm: R8's, the texture's swizzle 000R (alphaSwizzle below)
    { 10, 163u << 12 | 16, 1 },                 // R8Unorm (R001 -> RGB1)
    { 23, 132u << 12 | 16, 2 },                 // R16Uint
    { 25, 199u << 12 | 16, 2 },                 // R16Float
    { 30, 171u << 12 | 16, 2 },                 // RG8Unorm
    { 53, 133u << 12 | 16, 4 },                 // R32Uint
    { 55, 167u << 12 | 16, 4 },                 // R32Float
    { 70, 187u << 12 | 0, 4 },                  // RGBA8Unorm
    { 71, 187u << 12 | 0 | 1u << 20, 4 },       // RGBA8Unorm_sRGB
    { 80, 187u << 12 | 4, 4 },                  // BGRA8Unorm
    { 81, 187u << 12 | 4 | 1u << 20, 4 },       // BGRA8Unorm_sRGB
    { 105, 175u << 12 | 16, 8 },                // RG32Float
    { 115, 223u << 12 | 0, 8 },                 // RGBA16Float
    { 123, 157u << 12 | 0, 16 },                // RGBA32Uint
    { 125, 191u << 12 | 0, 16 },                // RGBA32Float
    // packed 16-bit and 32-bit formats (Metal names the components from the low bits, as gallium does: Mesa's
    // pan_format.c entry for the gallium format of the same name)
    { 40, 64u << 12 | 20, 2 },                  // B5G6R5Unorm: RGB565, BGR1
    { 42, 186u << 12 | 12, 2 },                 // ABGR4Unorm: RGBA4_UNORM, ABGR
    { 43, 65u << 12 | 4, 2 },                   // BGR5A1Unorm: RGB5_A1_UNORM, BGRA
    { 20, 164u << 12 | 16, 2 },                 // R16Unorm
    { 60, 172u << 12 | 16, 4 },                 // RG16Unorm
    { 65, 207u << 12 | 16, 4 },                 // RG16Float
    { 72, 123u << 12 | 0, 4 },                  // RGBA8Snorm
    { 90, 67u << 12 | 0, 4 },                   // RGB10A2Unorm
    { 94, 67u << 12 | 4, 4 },                   // BGR10A2Unorm
    { 92, 89u << 12 | 16, 4 },                  // RG11B10Float
    { 93, 91u << 12 | 16, 4 },                  // RGB9E5Float
    { 110, 188u << 12 | 0, 8 },                 // RGBA16Unorm
    // integer formats (11-12: mediaserverd's video kernels read and write R16Sint; the rest of the family with it --
    // genxml v10's R8UI 131 .. RGBA32I 221)
    { 13, 131u << 12 | 16, 1 },                 // R8Uint
    { 14, 195u << 12 | 16, 1 },                 // R8Sint
    { 24, 196u << 12 | 16, 2 },                 // R16Sint
    { 33, 139u << 12 | 16, 2 },                 // RG8Uint
    { 34, 203u << 12 | 16, 2 },                 // RG8Sint
    { 54, 197u << 12 | 16, 4 },                 // R32Sint
    { 63, 140u << 12 | 16, 4 },                 // RG16Uint
    { 64, 204u << 12 | 16, 4 },                 // RG16Sint
    { 73, 155u << 12 | 0, 4 },                  // RGBA8Uint
    { 74, 219u << 12 | 0, 4 },                  // RGBA8Sint
    { 103, 141u << 12 | 16, 8 },                // RG32Uint
    { 104, 205u << 12 | 16, 8 },                // RG32Sint
    { 113, 156u << 12 | 0, 8 },                 // RGBA16Uint
    { 114, 220u << 12 | 0, 8 },                 // RGBA16Sint
    { 124, 221u << 12 | 0, 16 },                // RGBA32Sint
    // depth and stencil (render pass attachments; sampled as the plain format of their size): Depth16Unorm,
    // Depth32Float, Stencil8, Depth32Float_Stencil8 (its depth plane; the stencil plane after it, stencil_off)
    { 250, 164u << 12 | 16, 2 },
    { 252, 167u << 12 | 16, 4 },
    { 253, 163u << 12 | 16, 1 },
    { 260, 167u << 12 | 16, 4 },
    // ASTC, LDR (Metal 204..217) and sRGB (186..199), 16 bytes a block: on Valhall the format is the one it decodes to
    // (RGBA16F; sRGB RGBA8), the block size in the plane descriptor ("ASTC 2D Plane", describe())
    // (11-13: METAL'S NUMBERS HAVE A GAP after 6x6 -- 204..208 are 4x4..6x6, 209 is not a format, 210..218 are 8x5..12x12
    // (sRGB: 186..190, 192..200). Counted straight through, every block size from 8x5 up was the next one's: Weather's
    // radar tiles, 512x512 ASTC 8x8 (212), were taken for 10x5 -- 103 rows of blocks read from the 64 the application
    // gave, and it died in the copy, three times in the tester's hands)
#define ASTC(n, w, h) { 204 + n, 223u << 12 | 0, 16, w, h }, { 186 + n, 187u << 12 | 0 | 1u << 20, 16, w, h }
    ASTC(0, 4, 4), ASTC(1, 5, 4), ASTC(2, 5, 5), ASTC(3, 6, 5), ASTC(4, 6, 6), ASTC(6, 8, 5), ASTC(7, 8, 6), ASTC(8, 8, 8),
    ASTC(9, 10, 5), ASTC(10, 10, 6), ASTC(11, 10, 8), ASTC(12, 10, 10), ASTC(13, 12, 10), ASTC(14, 12, 12),
#undef ASTC
};
// A COLOUR ATTACHMENT'S FORMAT: the render target's word 1 (write-back format, linear block, sRGB, swizzle) for the
// formats the Mali build draws into; false for the rest. The tile buffer holds R8G8B8A8 for every one of them (the
// blend descriptors' and LD_TILE's conversion), converted at write-back. Metal names packed formats from the low bits:
// BGRA8, BGR5A1 and B5G6R5 all have blue first, hence the swizzle.
constexpr uint32_t kRtFloat = 1u << 30;       // (not the descriptor's: a float target, see below)
constexpr uint32_t kRtRaw = 1u << 29;         // (not the descriptor's: a raw target, R32Float; with kRtWide RG32Float)
constexpr uint32_t kRtWide = 1u << 28;        // (not the descriptor's: 8 bytes a pixel in the tile buffer, RGBA16Float)
constexpr uint32_t kRtInt = 1u << 27;         // (not the descriptor's: an integer raw target, RG16Uint)
constexpr uint32_t kRtOurs = kRtFloat | kRtRaw | kRtWide | kRtInt;
bool rtFormat(unsigned f, uint32_t &rt1);
// the render target's internal format (word 0, bits 26..), the blend descriptor's memory format, and a clear colour
// as the tile buffer holds it, for a target whose word 1 is RT1
// (11-19) THE PACKED 16-BIT TARGETS have tile buffer formats of their own, as Mesa's blendable formats say (pan_format.c:
// B5G5R5A1 -> internal R5G5B5A1, 6; B5G6R5 -> R5G6B5A0, 5): each channel there is its bits and as many fraction bits
// again (5.5 5.5 5.5 1.1; 5.5 6.4 5.5 0.2 -- pan_clear.c's tib_layouts). Held as RGBA8 (internal 1) with the 16-bit
// write-back, a channel came out as its top three bits: every such target four times too dark, and FOUR TIMES DARKER
// AGAIN at each Load (boot-arg metal_loadtest: 0x90a5 written, 0x8421 after a pass that loaded it). CoreAnimation's
// backdrop capture is a BGR5A1 one: the blurred sheet too dark, and a black box where a frame redrew a part of it only.
inline bool rt5551(uint32_t rt1) { return !(rt1 & kRtOurs) && ((rt1 >> 3) & 31) == 28; }
inline bool rt565(uint32_t rt1) { return !(rt1 & kRtOurs) && ((rt1 >> 3) & 31) == 21; }
inline uint32_t rtInternal(uint32_t rt1) { return (rt1 & kRtRaw) ? ((rt1 & kRtWide) ? 35u : 34u) : (rt1 & kRtWide) ? 18 : (rt1 & kRtFloat) ? (rt1 >> 3) & 31 : rt5551(rt1) ? 6 : rt565(rt1) ? 5 : 1; }
inline uint32_t rtKilobytes(uint32_t rt1) { return (rt1 & kRtWide) ? 2 : 1; }   // of the tile buffer, a 16x16 tile (or 8x8 of 4 samples)
inline uint32_t
rtBlend(uint32_t rt1)
{
    if ((rt1 & kRtRaw) && (rt1 & kRtInt)) return 140u << 12 | 16;               // RG16UI, as a texture of it is named
    if (rt1 & kRtRaw) return (rt1 & kRtWide) ? (175u << 12 | 16) : (167u << 12 | 16);   // RG32F, R32F: as a texture of it is named
    if (rt1 & kRtWide) return 223u << 12;                       // RGBA16F
    if ((rt1 & kRtFloat) && ((rt1 >> 3) & 31) == 20) return 89u << 12;      // R11F_G11F_B10F
    // (11-13) AN sRGB TARGET's memory format says so (bit 20, as a texture of it does): without it the blend unit wrote
    // the shader's linear colour into the tile buffer as it was -- BGRA8Unorm_sRGB and RGBA8Unorm_sRGB targets came out
    // linear, some forty times too dark in the shadows: Maps' and Weather's maps in dark mode were all but black
    if (rt5551(rt1)) return 225u << 12;                         // RGB5_A1_PU (not dithered), RGBA
    if (rt565(rt1)) return 227u << 12 | 16;                     // R5G6B5_PU, RGB1
    if (!(rt1 & kRtFloat)) return 237u << 12 | ((rt1 & 1u << 14) ? 1u << 20 : 0u);
    return ((rt1 >> 3) & 31) == 16 ? 199u << 12 : 207u << 12;
}
inline uint32_t
halfOf(double v)
{
    float f = (float)v;
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7fffff;
    int e = (int)((x >> 23) & 0xff) - 127 + 15;
    if (e >= 31) return sign | 0x7c00 | (((x >> 23) & 0xff) == 0xff && mant ? 0x200 : 0);   // infinity, NaN
    if (e <= 0) return e < -10 ? sign : sign | ((mant | 0x800000) >> (14 - e));              // zero, denormal
    return sign | (uint32_t)e << 10 | mant >> 13;
}
uint8_t unorm8(double v);
inline uint32_t
rtClear(uint32_t rt1, const double *k)
{
    if ((rt1 & kRtRaw) && (rt1 & kRtInt)) {
        auto u16 = [](double v) { return v <= 0 ? 0u : v >= 65535 ? 65535u : (uint32_t)v; };
        return u16(k[0]) | u16(k[1]) << 16;                     // RG16Uint: red and green as integers
    }
    if (rt1 & kRtRaw) {
        float f = (float)k[0];
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }
    if (rt1 & kRtWide) {
        return halfOf(k[0]) | halfOf(k[1]) << 16;               // (red, green; blue and alpha: rtClearHigh)
    }
    if ((rt1 & kRtFloat) && ((rt1 >> 3) & 31) == 20) {
        // RG11B10Float: red and green 5 bits of exponent and 6 of mantissa, blue 5 and 5, no sign (a half without its
        // sign and its low mantissa bits; a negative value is 0)
        uint32_t h[3];
        for (int i = 0; i < 3; i++) h[i] = k[i] > 0 ? halfOf(k[i]) & 0x7fff : 0;
        return h[0] >> 4 | (h[1] >> 4) << 11 | (h[2] >> 5) << 22;
    }
    if (rt5551(rt1) || rt565(rt1)) {
        // (the tile buffer's layout: red from bit 0, a channel's value above its fraction bits)
        auto fixed = [](double v, unsigned bits, unsigned frac) {
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            return (uint32_t)(v * ((1u << bits) - 1) + 0.5) << frac;
        };
        return rt5551(rt1) ? fixed(k[0], 5, 5) | fixed(k[1], 5, 5) << 10 | fixed(k[2], 5, 5) << 20 | fixed(k[3], 1, 1) << 30
                           : fixed(k[0], 5, 5) | fixed(k[1], 6, 4) << 10 | fixed(k[2], 5, 5) << 20;
    }
    if (rt1 & kRtFloat) {
        uint32_t r = halfOf(k[0]);
        return ((rt1 >> 3) & 31) == 16 ? r | r << 16 : r | halfOf(k[1]) << 16;
    }
    if (rt1 & 1u << 14) {
        // (11-13) an sRGB target's clear colour is linear, as a shader's output is: encoded here (alpha is not)
        auto enc = [](double v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; };
        return unorm8(enc(k[0])) | unorm8(enc(k[1])) << 8 | unorm8(enc(k[2])) << 16 | (uint32_t)unorm8(k[3]) << 24;
    }
    return unorm8(k[0]) | unorm8(k[1]) << 8 | unorm8(k[2]) << 16 | (uint32_t)unorm8(k[3]) << 24;
}
// the clear's second word (the render target's clear is four words: a pixel of 8 bytes twice, one of 4 or less four
// times)
inline uint32_t
rtClearHigh(uint32_t rt1, const double *k)
{
    if ((rt1 & kRtRaw) && (rt1 & kRtWide)) {
        float f = (float)k[1];                                  // RG32Float: green's float (red's: rtClear)
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }
    return (rt1 & kRtWide) ? halfOf(k[2]) | halfOf(k[3]) << 16 : rtClear(rt1, k);
}
bool drawable(unsigned f);

// genxml's "ASTC 2D Dimension" for a block side
inline uint32_t astcDim(unsigned n) { return n == 4 ? 0 : n == 5 ? 1 : n == 6 ? 2 : n == 8 ? 4 : n == 10 ? 6 : 7; }
constexpr uint32_t kIdentitySwizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;
// A8Unorm's: zero (4) in red, green and blue, the R8 texel in alpha (as panfrost's A8_UNORM, 000R)
constexpr uint32_t kAlphaSwizzle = 4 | 4 << 3 | 4 << 6 | 0 << 9;
uint32_t planeWord0(const hakios_metal_texture *t);
void describe(const hakios_metal_texture *t, uint32_t *tex, uint32_t *plane, uint64_t plane_va);
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
namespace mali {
// one piece of work of a command buffer, in the order recorded
struct Job {
    unsigned shader;                // a blit: 0 copy_u32, 1 fill_u32, 2 copy_u8, 3 fill_u8; 4 a dispatch
    uint64_t dst, src;
    uint32_t value, count;
    uint32_t src_stride, dst_stride;    // COPY_ROWS: the rows' strides (value: words a row, count: words in all)
    hakios_metal_kernel *kernel;    // a dispatch's
    unsigned groups[3], threads[3], grid[3];
    uint64_t buffers[MALI_KERNEL_BUFFERS];
    uint32_t wls_size, wls_instances;   // a dispatch's threadgroup memory: bytes an instance (a power of two), instances
    hakios_metal_texture *textures[MALI_KERNEL_TEXTURES];  // bound at [[texture(n)]] (in the texture and image tables)
    uint32_t samplers[MALI_KERNEL_TEXTURES][8];
    unsigned texture_count, sampler_count;              // 0 0: no resource table
};
constexpr unsigned DISPATCH = 4, DRAW = 5, COPY_ROWS = 6;      // (a draw's Job: dst is its index in the command buffer's draws)

// One draw, everything resolved to what the GPU reads: one render pass of its own (begin, draw, end).
struct DrawJob {
    uint64_t pos_spd, vary_spd, frag_spd;
    unsigned vary_size;
    std::vector<uint32_t> vs_fau, fs_fau;               // the stages' push constants, whole
    uint64_t target_va;
    unsigned width, height, row_bytes;
    uint32_t rt1;                                       // the render target's word 1: write, format, block, swizzle
    uint32_t clear;                                     // (R8G8B8A8 in the tile buffer) and whether to write back
    uint32_t clear_high = 0;                            // (a wide target's second word: rtClearHigh)
    bool write;
    uint32_t blend[4];
    unsigned mode, count, instances, vertex_start;
    uint32_t dcd0, dcd1, dcd2, tiler_flags, scissor_min, scissor_max;
    hakios_metal_texture *textures[MALI_KERNEL_TEXTURES];
    uint32_t samplers[MALI_KERNEL_TEXTURES][8];
    unsigned texture_count, sampler_count;
    // (11-13) THE VERTEX STAGE's textures and samplers: a resource table of its own (register 0; the fragment's is 4)
    hakios_metal_texture *vs_textures[MALI_KERNEL_TEXTURES];
    uint32_t vs_samplers[MALI_KERNEL_TEXTURES][8];
    unsigned vs_texture_count, vs_sampler_count;
    uint64_t index_va = 0, index_size = 0;              // an indexed draw's indices (0: none)
    bool clear_only = false;                            // a pass with no geometry: the clear (hakios_metal_cb_clear)
    // DEPTH AND STENCIL (zs): the attachments' memory, formats, actions and clear values (the framebuffer descriptor's
    // word 12-13 and its ZS extension), and this draw's depth/stencil descriptor (zsd)
    unsigned samples = 1;                               // the attachments' (and the pipeline's): 1 or 4
    bool zs = false, has_z = false, has_s = false;
    uint64_t z_va = 0, s_va = 0;
    unsigned z_row = 0, s_row = 0;
    uint32_t z_format = 0, z_internal = 0;              // genxml "ZS Format" (D16 1, D32 14), "Z Internal Format"
    bool z_write = false, z_load = false, s_write = false, s_load = false;
    float z_clear = 0;
    uint32_t s_clear = 0;
    uint32_t zsd[8] = {};
    const char *name = "";                              // the program's (boot-arg metal_stats; held by the cb)
    const void *program = nullptr;                      // ... the program itself (boot-arg metal_glitchwatch)
    uint32_t tls_size = 0;                              // bytes a thread spills (the larger stage's): a local storage
                                                        // descriptor of its own at the area's end (drawBytes)
    // the Load action: the preload draw first, in the same Mali pass (compile_mali.c's mali_preload_compile)
    bool load = false;
    bool load0 = false;                                 // colour attachment 0 itself loads (else the preload leaves
                                                        // it to its clear: another target is what loads)
    uint64_t pre_pos_spd = 0, pre_frag_spd = 0;
    std::vector<uint32_t> pre_vs_fau, pre_fs_fau;
    hakios_metal_texture *pre_target = nullptr;
    // INCREMENTAL RENDERING (emitIr): the preload's fragment shader for every target as a pre-frame shader, its push
    // constants; 0: the pass has no tiler OOM handler (boot-arg metal_noir, or no preload program)
    // LAYERED RENDERING (11-14): the pass's layers (1: none) and how far apart they lie in the targets' memory
    unsigned layers = 1;
    uint64_t layer_stride = 0, z_layer_stride = 0;
    uint64_t ir_frag_spd = 0;
    std::vector<uint32_t> ir_fs_fau;
    hakios_metal_texture *ir_target = nullptr;          // colour attachment 0 (held as the draw's target)
    hakios_metal_texture *target_tex = nullptr;         // ... always (boot-arg metal_glitchsay's scan)
    uint64_t zs_frag_spd = 0;                           // depth/stencil Load: the pre-frame shader that loads them
    std::vector<uint32_t> zs_fs_fau;
    // colour attachments 1 to 3 (rts > 1): each one's address, rows, RT word 1, clear, write, load, blend, texture
    unsigned rts = 1;
    struct More {
        uint64_t va;
        unsigned row_bytes;
        uint32_t rt1, clear, blend[4];
        uint32_t clear_high = 0;
        bool write, load;
        bool missing;                                   // written by the fragment function, absent from the pass
        bool untouched;                                 // this draw neither writes nor reads it (kept in, loaded and stored)
        bool memoryless;                                // a memoryless texture's: in the tile buffer only, never loaded/stored
        bool elide;                                     // no draw of its Mali pass touches it: not loaded, not written back
        hakios_metal_texture *texture;
    } more[3] = {};
};
struct IndexInfo { hakios_metal_buffer *buffer; uint64_t offset; unsigned type; int base_vertex; };
uint32_t hierarchyMask(unsigned w, unsigned h, unsigned levels);

// One draw's descriptors at AREA (drawBytes(DJ) of them, 64-byte aligned) and its instructions into S from N on: the
// tiler context (fresh: an empty polygon list), the framebuffer descriptor and render target, the blend descriptor, the
// stages' push constants, the fragment stage's resource table; then RUN_IDVS, FINISH_TILING, RUN_FRAGMENT, each after a
// WAIT for the one before. Registers in v11's layout (upstream Mesa's genxml v11.xml: v10's plus TILER_FLAGS2 at r61);
// what each value is, hakios-kernel's the port's log ("render pipelines") and userland/src/mali_test.c.
uint64_t drawBytes(const DrawJob &dj);

// INCREMENTAL RENDERING (panvk's tiler OOM exception handler, csf/panvk_vX_exception_handler.c): a pass whose polygon
// lists outgrow the tiler heap's most chunks would wait for good. Each Mali pass has IR_BYTES of its own (ir): a
// handler the firmware runs on the queue when the kernel has no chunk left to give -- the lists so far drawn by a
// fragment job and stored, their chunks given back, the tiler context's lists emptied -- and three copies of the
// pass's framebuffer descriptor: the first flush (as the pass begins, every target stored), the later flushes (every
// target loaded by a pre-frame shader -- the preload's fragment shader -- and stored), and the pass's own last fragment
// job after a flush (loaded, then stored as the pass says). Word 0: the descriptor the next flush uses; word 8: the one
// the pass ends with (the stream loads r40 from it); 0x10: set when the handler is installed; 0x40: r40-r47 kept.
// (0xc00: the pre-frame DCDs of the later parts -- pre-frame 0, 1, post-frame, 128 bytes each; 0xd80: the pass's own,
// loading depth/stencil when it asks to, with their resource table at 0xf00, depth/stencil descriptors at 0x1040
// (the pass's) and 0x1060 (the later parts'), push constants at 0x1080)
constexpr uint64_t IR_BYTES = 0x1100, IR_HANDLER = 0x80, IR_HANDLER_BYTES = 0x180, IR_SRT = 0x200, IR_FBD = 0x600,
    IR_FBD_STRIDE = 0x200, IR_DCD = 0xc00, IR_ZS_DCD = 0xd80, IR_ZS_SRT = 0xf00, IR_ZSD = 0x1040, IR_IR_ZSD = 0x1060,
    IR_ZS_FAU = 0x1080;
inline uint64_t wait_on(unsigned mask) { return 3ULL << 56 | (uint64_t)mask << 16; }
void describeRaw(uint32_t mali_format, unsigned bytes, uint64_t va, uint32_t stride, unsigned w, unsigned h, uint32_t *tex, uint32_t *plane, uint64_t plane_va, unsigned samples = 1);
void describeZs(const DrawJob &dj, unsigned zs, uint8_t *tex, uint8_t *planes, uint64_t planes_va);
void zsdFromShader(uint32_t *w, unsigned zs);
void emitZsLoad(hakios_metal_device *d, const DrawJob &dj, uint8_t *ir, uint64_t ir_va);
void emitIr(hakios_metal_device *d, const DrawJob &dj, uint8_t *pass_area, uint64_t pass_va, uint8_t *ir, uint64_t ir_va, uint64_t rt_at);
void emitDraw(hakios_metal_device *d, const DrawJob &dj, uint8_t *area, uint64_t area_va, uint64_t *s, unsigned &n, bool first = true, bool last = true, uint8_t *pass_area = nullptr, uint64_t pass_va = 0, uint8_t *ir = nullptr, uint64_t ir_va = 0);
constexpr unsigned DRAW_WORDS = 62 + 45 + 10 + 3; // (instructions emitDraw writes, at most: the preload's 45, a pass's flush, the
bool samePass(const DrawJob &a, const DrawJob &b);
uint64_t drawBytes(const DrawJob &dj);
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
namespace mali {
void passStats(hakios_metal_cb *cb);
void glitchScan(hakios_metal_cb *cb, bool before);
void glitchWatch(const char *event, hakios_metal_cb *cb);
void glitchWatchCommit(hakios_metal_cb *cb);
void glitchWatchDone();
bool bootArg(const char *word);
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)

struct hakios_metal_cb {
    hakios_metal_queue *queue;
    void (*scheduled)(hakios_metal_cb *, void *) = nullptr;  // (hakios_metal_cb_on_scheduled: after the submission)
    void *scheduled_context = nullptr;
    std::vector<unsigned> busy;                         // the IOSurfaces it draws into, marked busy (HakiosGPU 0x7d)
    std::vector<uint64_t> mirror_out;                   // ... of them, those drawn through a mirror (their keys): copied back at commit
    hakios_metal_device *busy_device = nullptr;         // ... the device the marks were made through
    std::chrono::steady_clock::time_point committed_at; // (boot-arg metal_stats)
    Bo timestamps;                                      // ... the GPU's timestamp at each Mali pass's start and end
    std::chrono::steady_clock::time_point bo_at, submitted_at, waited_at;   // (boot-arg metal_cbtime)
    unsigned timestamp_count = 0;
    std::vector<Job> blits;
    // the vertex stage's [[texture(n)]] and [[sampler(m)]] for the draws that follow (hakios_metal_cb_set_vertex_textures)
    hakios_metal_texture *vs_textures[MALI_KERNEL_TEXTURES] = {};
    uint32_t vs_samplers[MALI_KERNEL_TEXTURES][8] = {};
    bool vs_sampler_set[MALI_KERNEL_TEXTURES] = {};
    unsigned vs_texture_count = 0, vs_sampler_count = 0;
    std::vector<hakios_metal_buffer *> held;    // retained until the GPU is done
    std::vector<hakios_metal_kernel *> kernels;
    std::vector<hakios_metal_texture *> held_textures;
    std::vector<DrawJob> draws;                 // a DRAW Job's (its dst the index)
    std::vector<uint64_t> draw_areas;
                                                // (where each draw's descriptors are in the stream BO: HAKIOS_MALI_TRACE)
    std::vector<hakios_metal_program *> programs;
    unsigned threadgroup_length = 0;            // setThreadgroupMemoryLength:atIndex: 0, for the dispatches after
    std::string error;
    enum hakios_metal_status status = HAKIOS_METAL_NOT_ENQUEUED;
    bool skip_refused = false;                  // a refused piece left out, not the whole command buffer failed
    unsigned skipped = 0;
    std::string skip_reason;
    unsigned seq = 0;                           // (boot-arg metal_glitchwatch: its number, in commit order)
    Bo stream;
    uint32_t sync = 0;
    uint64_t token = 0;             // (IOGPU) the "completed" token its submission is answered with; 0: nothing submitted
    void (*callback)(hakios_metal_cb *, void *) = nullptr;
    void *context = nullptr;
    bool freed = false;             // freed by its owner while in flight: the queue's thread frees it after
    std::mutex lock;
    std::condition_variable done;

    void hold(hakios_metal_buffer *b)
    {
        b->retain();
        held.push_back(b);
    }
    void letGo()
    {
        // the surfaces it drew into: not busy any more (the display waits for these marks to go: markBusy)
        for (unsigned sid : busy) {
            struct { uint32_t surface; int32_t delta; } r = { sid, -1 };
            std::lock_guard<std::mutex> g(busy_device->lock);
            busy_device->call(0x7d, &r, sizeof(r));
        }
        busy.clear();
        for (auto *b : held) {
            b->release();
        }
        for (auto *k : kernels) {
            k->release();
        }
        for (auto *t : held_textures) {
            t->release();
        }
        for (auto *p : programs) {
            p->release();
        }
        programs.clear();
        draws.clear();
        held.clear();
        kernels.clear();
        held_textures.clear();
    }
};

struct hakios_metal_queue {
    hakios_metal_device *device;
    std::thread thread;
    std::atomic<bool> self_free{false};         // freed from its own thread (a completion handler): run()'s end deletes it
    std::mutex lock;
    std::condition_variable work;
    std::deque<hakios_metal_cb *> pending;
    bool stopping = false;

    void run()
    {
        for (;;) {
            hakios_metal_cb *cb;
            {
                std::unique_lock<std::mutex> l(lock);
                work.wait(l, [&] { return stopping || !pending.empty(); });
                if (pending.empty()) {
                    return;
                }
                cb = pending.front();
                pending.pop_front();
            }
            if (cb->status != HAKIOS_METAL_ERROR) {
                int r;
                bool lost;                      // the queue restarted after a GPU fault with this work not done
                uint32_t iogpu_status = 0;      // (IOGPU) the completion's status: 0 done, 0xb a page fault, 1/0xa not run
                if (device->iogpu.conn != 0) {
                    // (IOGPU: the device's completion thread hands the "completed" answer over; nothing submitted for an
                    // empty command buffer -- done in its turn, as the signalled sync object was)
                    r = cb->token == 0 ? 0 : device->iogpu.wait(cb->token, 10000, iogpu_status);
                    lost = r == 0 && iogpu_status == 0xb;
                } else {
                    syncobj_wait w = {};
                    w.handles = (uint64_t)(uintptr_t)&cb->sync;
                    w.timeout_nsec = 10000000000LL;         // relative (HakiosGPU's convention)
                    w.count_handles = 1;
                    w.flags = 1;
                    // (WITHOUT the device's lock, 11-10: it guards the device's buffers and addresses, not the call -- held
                    // here, every other call waited for the GPU to finish this command buffer: the next one's submission in
                    // commit, and HakiosGPU's surface marks at a command buffer's first draw, so CoreAnimation recorded its
                    // frames one GPU completion at a time; the kernel drops its own lock while it waits)
                    r = device->call(SYNCOBJ_WAIT, &w, sizeof(w));
                    lost = r == 0 && w.pad != 0;    // (HakiosGPU's: the queue was restarted after a GPU fault)
                }
                cb->waited_at = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> g(cb->lock);
                if (r != 0) {
                    cb->error = "the GPU did not finish the command buffer in 10 s";
                    cb->status = HAKIOS_METAL_ERROR;
                } else if (!lost && iogpu_status != 0) {
                    char why[128];
                    snprintf(why, sizeof(why), "IOGPU did not run the command buffer (status 0x%x: %s)", iogpu_status,
                        iogpu_status == 0xa ? "the GPU's queue refused the stream" : "its queue went before it ran");
                    cb->error = why;
                    cb->status = HAKIOS_METAL_ERROR;
                } else if (lost) {
                    cb->error = "a GPU fault: the command buffer's work is lost (the queue was restarted)";
                    cb->status = HAKIOS_METAL_ERROR;
                    // (11-23) what the lost command buffer read: each draw's program, its target, its textures' memory and
                    // the 64-bit push constants that look like GPU addresses -- to find the draw of a fault's address
                    // (the kernel's "page fault ... at") among them. The first 6 faulting command buffers of a process.
                    static int said_faults;
                    if (said_faults++ < 6) {
                        for (size_t di = 0; di < cb->draws.size() && di < 24; di++) {
                            const DrawJob &x = cb->draws[di];
                            std::string line;
                            char one[128];
                            snprintf(one, sizeof(one), "fault cb draw %zu: %s target 0x%llx", di, x.name ? x.name : "?",
                                (unsigned long long)x.target_va);
                            line += one;
                            for (unsigned t = 0; t < x.texture_count; t++) {
                                if (x.textures[t]) {
                                    snprintf(one, sizeof(one), " T%u[0x%llx+%llu %ux%u]", t,
                                        (unsigned long long)(x.textures[t]->bo.va + x.textures[t]->offset),
                                        (unsigned long long)x.textures[t]->bo.size, x.textures[t]->width, x.textures[t]->height);
                                    line += one;
                                }
                            }
                            const std::vector<uint32_t> *faus[2] = { &x.vs_fau, &x.fs_fau };
                            for (int st = 0; st < 2; st++) {
                                const std::vector<uint32_t> &f = *faus[st];
                                for (size_t w = 0; w + 1 < f.size(); w += 2) {
                                    uint64_t v = f[w] | (uint64_t)f[w + 1] << 32;
                                    if (v >= 0x100000000ULL && v < 0x10000000000ULL) {
                                        snprintf(one, sizeof(one), " %s%zu=0x%llx", st ? "f" : "v", w / 2, (unsigned long long)v);
                                        line += one;
                                    }
                                }
                            }
                            fprintf(stderr, "HakiosMetal: %s\n", line.c_str());
                        }
                    }
                } else {
                    cb->status = HAKIOS_METAL_COMPLETED;
                }
            }
            {
                std::lock_guard<std::mutex> g(device->lock);
                if (cb->sync) {
                    syncobj_destroy d = { cb->sync, 0 };
                    device->call(SYNCOBJ_DESTROY, &d, sizeof(d));
                    cb->sync = 0;
                }
            }
            if (getenv("HAKIOS_MALI_TRACE") != nullptr && cb->stream.cpu != nullptr) {
                // what the tiler did with each draw: the polygon list (its top bits set when it binned something), and
                // the heap descriptor's bottom (primitive data, up) and top (bins, down)
                auto *h = (uint32_t *)device->heapdesc.cpu;
                for (uint64_t at : cb->draw_areas) {
                    auto *tc = (uint32_t *)((uint8_t *)cb->stream.cpu + at);
                    fprintf(stderr, "hakios-metal: draw: at 0x%llx: ", (unsigned long long)(cb->stream.va + at));
                    fprintf(stderr, "tiler context %08x %08x %08x %08x; completed %08x%08x %08x%08x; "
                        "private %08x %08x %08x %08x %08x %08x %08x %08x; heap bottom %08x%08x top %08x%08x\n",
                        tc[0], tc[1], tc[2], tc[3], tc[11], tc[10], tc[13], tc[12], tc[24], tc[25], tc[26], tc[27], tc[28],
                        tc[29], tc[30], tc[31], h[5], h[4], h[7], h[6]);
                }
                uint64_t nothing = 0;
                device->call(0x7f, &nothing, sizeof(nothing));        // (HakiosGPU: the queues and heaps into its log)
            }
            static const bool stats = bootArg("metal_stats") || bootArg("mdsay="), cbtime = bootArg("metal_cbtime");
            if (stats) {
                passStats(cb);
            }
            if (cbtime) {
                // commit -> stream BO made -> built and submitted -> the GPU's wait returned -> here (freed, called back)
                static unsigned said;
                auto us = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
                    return (long)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
                };
                if (said++ < 400) {
                    auto now = std::chrono::steady_clock::now();
                    fprintf(stderr, "hakios-metal: cbtime: bo %ld, build+submit %ld, wait %ld, after %ld us (%zu jobs)\n",
                        us(cb->committed_at, cb->bo_at), us(cb->bo_at, cb->submitted_at),
                        us(cb->submitted_at, cb->waited_at), us(cb->waited_at, now), cb->blits.size());
                }
            }
            glitchScan(cb, false);
            glitchWatch("completion", cb);
            glitchWatchDone();
            device->giveStream(cb->stream);
            if (cb->timestamps.size != 0) {
                device->freeBo(cb->timestamps);
                cb->timestamps.size = 0;
            }
            cb->letGo();
            cb->done.notify_all();
            if (cb->callback) {
                cb->callback(cb, cb->context);  // (it may free the command buffer)
            } else if (cb->freed) {
                delete cb;
            }
        }
    }
};
namespace mali {
bool openDraws(hakios_metal_device *d);
bool openBlits(hakios_metal_device *d);         // the blit shaders' code and descriptors (mali_device.cpp)
// (external) the Mali's tiler heap through vendor selectors 56/57 on the shared IOGPU connection (mali_external.cpp)
bool extNewHeap(hakios_metal_device *d, uint32_t chunk_size, uint32_t initial, uint32_t most, uint32_t in_flight,
    uint32_t &handle, uint64_t &ctx, uint64_t &first);
void extFreeHeap(hakios_metal_device *d, uint32_t handle);
}  // namespace mali
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C" {
extern "C++" bool gpu_expected(void);
extern "C++" io_service_t find_gpu_service(void);

extern "C++" void mirrorsBack(hakios_metal_cb *cb, hakios_metal_device *d);
extern "C++" void passElide(hakios_metal_cb *cb);
// a command buffer's stream built into a stream buffer of the device (N its words); false with cb's error set
extern "C++" bool emitStream(hakios_metal_cb *cb, hakios_metal_device *d, unsigned &n);
extern "C++" void zsKeep(hakios_metal_cb *cb);
extern "C++" { namespace mali {
void markBusy(hakios_metal_cb *cb, hakios_metal_device *d, hakios_metal_texture *t);
int dispatchGrid(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned groups[3], const unsigned threads[3], const unsigned grid[3], hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count, hakios_metal_texture *const *textures = nullptr, unsigned texture_count = 0, hakios_metal_sampler *const *samplers = nullptr, unsigned sampler_count = 0);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {

// BOOT-ARG metal_glitchwatch: every 1080x2340-class screen target (4 bytes a texel, drawn into by 20 command buffers)
// kept watched, scanned (a row in 64) at every commit and every completion of any command buffer, and at CPU uploads:
// the moment one turns blue/cyan is said with the event, the command buffer and its jobs (copies/fills with whether
// they land in it, draws with their attachments). metal_glitchsync too: a commit first waits (up to 200 ms) for every
// command buffer in flight, so "turned at the commit of N" means between N-1's completion and N's commit: no GPU work
// of ours -- a CPU write (ours, CoreAnimation's, another process's) or the kernel.
inline std::mutex gwLock;
inline std::vector<std::pair<hakios_metal_texture *, bool>> gwTargets;  // (retained) and whether it was blue at the last scan
inline std::atomic<unsigned> gwSeq{0}, gwInFlight{0};
unsigned gwBlueRows(hakios_metal_texture *t, unsigned step, unsigned *first);
void gwMap(hakios_metal_texture *t, const char *what);
std::vector<std::string> gwDescribe(hakios_metal_cb *cb, hakios_metal_texture *t);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
int copyRows(hakios_metal_texture *t, unsigned x, unsigned y, unsigned w, unsigned h, uint8_t *bytes, uint64_t bytes_per_row, bool in, unsigned level = 0);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
constexpr char kCacheMagic[8] = { 'H', 'K', 'M', 'A', 'L', 'I', '0', '1' };
bool cacheOn();

// THE CACHE'S SIZE (TODO 17): a key holds this library's build (fnvImage), so every new build leaves the old entries
// behind for good -- on a test phone each build. Over CACHE_MOST bytes, the least recently used files (their mtime:
// cacheLoad touches what it reads) go until CACHE_LEAST remain. Once a process, in the background.
constexpr uint64_t CACHE_MOST = 64ull << 20, CACHE_LEAST = 48ull << 20;
void cacheTrim(const std::string &dir);
const std::string & cacheDir();
void fnv(uint64_t &h, const void *data, size_t n);
void fnvImage(uint64_t &h);

inline const uint32_t sizeof_binary = sizeof(mali_kernel_binary);
uint64_t cacheKey(const void *vs, size_t vs_size, const std::vector<mali_constant> &vc, const void *fs, size_t fs_size, const std::vector<mali_constant> &fc, unsigned format, const mali_vertex_fetch *fetch, const mali_shader_blend *blend = nullptr);
std::string cachePath(uint64_t key);
bool writeAll(int fd, const void *p, size_t n);
bool readAll(int fd, void *p, size_t n);

// (the cache's use, said once a process after 40 looks: 11-10, seven pipelines compiled again at every boot)
inline std::atomic<unsigned> cache_hits{0}, cache_misses{0}, cache_bad{0}, cache_unwritten{0}, cache_seeded{0};
void cacheSay();
const std::string & seedDir();

// both stages from the file KEY names; false (and nothing left allocated) when there is none or it is not whole
// REFUSALS KEPT TOO (11-23): CoreAnimation's uber pipelines (VfxXgh + A3Xghfc, 3598 blocks) go through seconds of NIR
// passes before compile_mali.c refuses them for their size -- and a refusal was never cached, so every boot did that
// again for all of them: SpringBoard waited ~85 s for backboardd's warmup_shaders. A size refusal depends only on what the
// key covers and the limit (kept in the file and checked), so it is remembered as KEY.refused; no other refusal is (a fix
// of the plug-in may lift it).
inline const char kTooBig[] = "too big for the Mali back end's spiller yet";
bool refusalLoad(uint64_t key, char *why, size_t why_size);
void refusalStore(uint64_t key, const char *why);
bool cacheLoad(uint64_t key, mali_kernel_binary *vs, mali_kernel_binary *fs);
void cacheStore(uint64_t key, const mali_kernel_binary &vs, const mali_kernel_binary &fs);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
std::vector<mali_constant> keyedConstants(const hakios_metal_program *p, const std::vector<mali_constant> &fc);

// this thread's fragment compiles fold P's known buffer while it lives
struct KnownNow {
    mali_known_buffer kb = {};
    explicit KnownNow(const hakios_metal_program *p)
    {
        if (p->known_binding >= 0) {
            kb = { (unsigned)p->known_binding, p->known.data(), p->known.size() };
            mali_compile_known(&kb);
        }
    }
    ~KnownNow() { mali_compile_known(nullptr); }
};
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
bool compileFetch(hakios_metal_program *p);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
// Metal's blend factor as Mali's fixed-function operand: the base (C operand, with ONE as its own) and whether it is
// inverted; MTLBlendFactorZero is ONE inverted, as gallium's. false for what the hardware has no operand for.
struct MaliFactor { unsigned base; bool inv; };
enum { MF_ONE = 1, MF_SRC = 2, MF_DEST = 3, MF_SRC_ALPHA = 5, MF_DEST_ALPHA = 6 };    // (the C operand's values)
bool maliFactor(unsigned metal, MaliFactor &f);
uint32_t maliBlendFunction(unsigned op, unsigned src_metal, unsigned dst_metal, bool alpha, const char *&why);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
void encodeBlend(int enabled, unsigned rgb_op, unsigned alpha_op, unsigned src_rgb, unsigned dst_rgb, unsigned src_alpha, unsigned dst_alpha, unsigned write_mask, uint32_t &equation, bool &reads_dest, std::string &refusal, mali_blend_rt *shader = nullptr, bool *in_shader = nullptr);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
void rawBlend(hakios_metal_program *p, unsigned i);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
bool stagePushConstants(const mali_kernel_binary &b, hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count, const float scale[3], const float offset[3], unsigned first_vertex, unsigned base_instance, std::vector<uint32_t> &fau, std::vector<hakios_metal_buffer *> &used);
bool inlineSampler(uint64_t w, uint32_t words[8]);

// the Load action's program (compile_mali.c's mali_preload_compile), compiled and uploaded on first use; false with
// d->preload_error said when it cannot be
hakios_metal_texture *attachmentOf(hakios_metal_texture *t, unsigned level, unsigned slice);
bool zsOf(const struct hakios_metal_render_pass *pass, DrawJob &dj, unsigned width, unsigned height, bool draw, const char *&why);
bool passAtLevels(const struct hakios_metal_render_pass *pass, struct hakios_metal_render_pass &q, hakios_metal_texture *views[10]);
bool atLevels(const struct hakios_metal_render_pass *pass);
hakios_metal_program * preloadProgram(hakios_metal_device *d, unsigned targets, unsigned zs);
int drawImpl(hakios_metal_cb *cb, hakios_metal_program *p, const struct hakios_metal_render_pass *pass, const struct hakios_metal_viewport *viewport, const struct hakios_metal_scissor *scissor, unsigned primitive, unsigned vertex_start, unsigned vertex_count, unsigned instance_count, unsigned base_instance, hakios_metal_buffer *const *vs_buffers, const uint64_t *vs_offsets, unsigned vs_count, hakios_metal_buffer *const *fs_buffers, const uint64_t *fs_offsets, unsigned fs_count, hakios_metal_texture *const *fs_textures, unsigned fs_texture_count, hakios_metal_sampler *const *fs_samplers, unsigned fs_sampler_count, const IndexInfo *ix = nullptr, const uint32_t *fs_words = nullptr, unsigned fs_word_count = 0);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
const MaliFormat * findFormat(unsigned pixel_format);
hakios_metal_texture * textureOver(hakios_metal_device *d, const Bo &bo, uint64_t offset, uint32_t stride, unsigned type, const MaliFormat *f, unsigned pixel_format, unsigned width, unsigned height, unsigned usage, unsigned origin);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" Job mirrorJob(const hakios_metal_device::Mirror &m, bool in);
extern "C++" bool mirrorFill(hakios_metal_device *d, const hakios_metal_device::Mirror &m);
extern "C++" hakios_metal_texture * mirrorOf(hakios_metal_device *d, uint64_t id, const Bo &bo, const hakios_metal_device::SurfaceInfo &info, const MaliFormat *f, unsigned pixel_format);
extern "C++" void mirrorRelease(hakios_metal_device *d, uint64_t key);
extern "C++" void mirrorsBack(hakios_metal_cb *cb, hakios_metal_device *d);
extern "C++" { namespace mali {
hakios_metal_texture * levelsOf(hakios_metal_texture *p, const MaliFormat *f, unsigned pixel_format, unsigned type, unsigned base, unsigned count, unsigned layer = 0);
hakios_metal_texture * attachmentOf(hakios_metal_texture *t, unsigned level, unsigned slice);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)
extern "C++" { namespace mali {
hakios_metal_program * blitProgram(hakios_metal_device *d);
unsigned linearOf(unsigned f);
hakios_metal_program * resolveProgram(hakios_metal_device *d);
int resolveDraw(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned sx, unsigned sy, hakios_metal_texture *to, unsigned tl, unsigned dx, unsigned dy, unsigned w, unsigned h);
int blitDraw(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned fl, hakios_metal_texture *to, unsigned tl, unsigned dx, unsigned dy, unsigned dw, unsigned dh, const float m[6], bool linear, bool raw);
} }  // namespace mali, extern "C++"
using namespace mali;   // (it was an anonymous namespace: its names are seen outside it)

// (11-13) A copy between LAYERS -- a 2D array's slices, a cube's faces: FROM_LAYER and TO_LAYER, each layer_stride further
// on in its texture's memory. Served where the copy is one of bytes (one size, one pixel size; a compressed format block
// for block); a copy that has to be drawn (scaled, or big and into RGBA8/BGRA8 from another size) is refused for a layer
// other than the first, by name.
static int
blitLayers(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned from_layer, unsigned sx, unsigned sy,
    unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned to_layer, unsigned dx, unsigned dy,
    unsigned dw, unsigned dh, int linear);
extern "C++" int blitLayers(hakios_metal_cb *cb, hakios_metal_texture *from, unsigned from_level, unsigned from_layer, unsigned sx, unsigned sy, unsigned sw, unsigned sh, hakios_metal_texture *to, unsigned to_level, unsigned to_layer, unsigned dx, unsigned dy, unsigned dw, unsigned dh, int linear);
}  // extern "C"
hakios_metal_texture * nothingTexture(hakios_metal_device *d);
