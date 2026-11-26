// mali_iogpu.cpp: the Mali back end's second way to the kernel, Apple's IOGPU interface (boot-arg metal_iogpu=1;
// mali_internal.h's IOGPUTransport says what it changes). The kernel's side, the authority for every layout here:
// akios xnu/iokit/Kernel/akios_iogpu.cpp (IOGPUDeviceUserClient) and hakios.akita_gpu.cpp (the Mali's vendor
// selectors); the client this follows: akios userland/src/tools/iogpu_test.m.
#include "mali_internal.h"

#include <pthread.h>

using namespace mali;

// (the SDK marks the trap calls unavailable on iOS; IOGPU.framework calls them all the same, and so do we)
extern "C" kern_return_t akios_IOConnectTrap1(io_connect_t, uint32_t, uintptr_t) __asm__("_IOConnectTrap1");
extern "C" kern_return_t akios_IOConnectTrap4(io_connect_t, uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
    __asm__("_IOConnectTrap4");

namespace {

// the IOGPUDeviceUserClient's selectors and traps used here
enum : uint32_t {
    kSelNewCommandQueue = 6, kSelNewResource = 8, kSelDeleteResource = 9, kSelCreateShmem = 12,
    kSelCreateNotificationQueue = 14, kSelSetNotificationQueue = 24,
    kSelTilerHeap = 56, kSelTilerHeapGone = 57,         // (HakiosGPU::vendorMethod: the Mali's own)
    kTrapSubmit = 0, kTrapDeleteResource = 1,
};

// selector 8's argument (0x58 bytes, IOGPUNewResourceArgs) and answer (IOGPUNewResourceOut)
constexpr size_t kResourceArgs = 0x58;
constexpr uint32_t kKindSysMem = 0x00, kKindSurface = 0x82;
constexpr uint32_t kFlagAlways = 0x400, kFlagZeroFill = 0x40, kFlagGPURead = 0x10;

// a notification (IOGPU::notification): the token, when the work began and ended, its status, a fault's address
struct Notification { uint64_t token, start_ns, end_ns; uint32_t status, pad; uint64_t fault; };
static_assert(sizeof(Notification) == 0x28, "IOGPU's notification is 0x28 bytes");

// Submissions' tokens: even the "scheduled" one, odd the "completed" one of the same submission (both ours to choose;
// the kernel hands them back as they were). Room for the answers of 0x200 command buffers in flight at once: an entry
// that finds no room is lost and its command buffer would wait out its 10 s (iogpu_test's 0x100 is for one at a time).
constexpr uint32_t kNotificationEntries = 0x400;

void
newResourceArgs(uint8_t *a, uint32_t kind, uint32_t flags)
{
    memset(a, 0, kResourceArgs);
    *(uint32_t *)(a + 0x00) = kind;
    *(uint32_t *)(a + 0x08) = 0x10001;                  // width 1, height 1 (a buffer's, as IOGPU.framework's)
    *(uint16_t *)(a + 0x0c) = 1;                        // depth
    a[0x10] = 1;                                        // faces
    a[0x11] = 1;                                        // mip levels
    a[0x13] = 1;                                        // pixel info
    *(uint32_t *)(a + 0x14) = kFlagAlways | flags;
}
}  // namespace

bool
IOGPUTransport::open(io_service_t service)
{
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    if (kr != KERN_SUCCESS) {
        conn = 0;
        fail("IOServiceOpen type 1: 0x%x", kr);
        return false;
    }
    // the shared memory every command buffer names: its kernel commands (type 1) and its segment list (type 0). Left
    // zeroed: an empty segment list (its first record's control word 0) names no kernel commands -- no shared events
    // to wait for or signal yet (akios_iogpu.cpp's kernelCommands stops at it)
    for (uint32_t type = 0; type < 2; type++) {
        uint64_t in[2] = { 0x4000, type };
        struct { uint64_t address; uint32_t size, id; } shm = {};
        size_t size = sizeof(shm);
        kr = IOConnectCallMethod(conn, kSelCreateShmem, in, 2, nullptr, 0, nullptr, nullptr, &shm, &size);
        if (kr != KERN_SUCCESS || shm.id == 0) {
            fail("IOGPU shared memory: 0x%x", kr);
            close();
            return false;
        }
        (type == 0 ? seg_shmem : kcmd_shmem) = shm.id;
    }
    // the command queue (the process's path, as IOGPU.framework gives it; the kernel does not read it)
    {
        uint8_t in[0x408] = {};
        uint64_t out[2] = {};
        size_t size = sizeof(out);
        const char *name = getprogname();
        strlcpy((char *)in, name != nullptr ? name : "metal", 0x400);
        kr = IOConnectCallStructMethod(conn, kSelNewCommandQueue, in, sizeof(in), out, &size);
        if (kr != KERN_SUCCESS || out[0] == 0) {
            fail("IOGPU command queue: 0x%x", kr);
            close();
            return false;
        }
        queue = (uint32_t)out[0];
    }
    // its notification queue, the port that says "data available", and the queue told to answer there
    {
        uint64_t in[2] = { kNotificationEntries, sizeof(Notification) };
        struct { uint64_t address; uint32_t id, pad; } out = {};
        size_t size = sizeof(out);
        kr = IOConnectCallMethod(conn, kSelCreateNotificationQueue, in, 2, nullptr, 0, nullptr, nullptr, &out, &size);
        if (kr != KERN_SUCCESS || out.id == 0 || out.address == 0) {
            fail("IOGPU notification queue: 0x%x", kr);
            close();
            return false;
        }
        nq = out.id;
        nq_memory = (IODataQueueMemory *)(uintptr_t)out.address;
    }
    port = IODataQueueAllocateNotificationPort();
    // (a send right of ours too: close() wakes the completion thread with a message of its own)
    if (port == MACH_PORT_NULL ||
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
        fail("no notification port");
        close();
        return false;
    }
    kr = IOConnectSetNotificationPort(conn, 0, port, nq);
    if (kr == KERN_SUCCESS) {
        uint64_t in[2] = { queue, nq };
        kr = IOConnectCallScalarMethod(conn, kSelSetNotificationQueue, in, 2, nullptr, nullptr);
    }
    if (kr != KERN_SUCCESS) {
        fail("IOGPU notifications: 0x%x", kr);
        close();
        return false;
    }
    completions = std::thread([this] { run(); });
    return true;
}

void
IOGPUTransport::close()
{
    if (completions.joinable()) {
        {
            std::lock_guard<std::mutex> g(lock);
            stopping = true;
        }
        // (if the port's one message slot is already taken, that message wakes the thread instead)
        mach_msg_header_t h = {};
        h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        h.msgh_size = sizeof(h);
        h.msgh_remote_port = port;
        mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(h), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
        completions.join();
    }
    if (conn != 0) {
        IOServiceClose(conn);           // (the queue, the notification queue, the resources, the heaps: the kernel's)
        conn = 0;
    }
    if (port != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), port);                           // our send right (if made)
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        port = MACH_PORT_NULL;
    }
    nq_memory = nullptr;
    queue = nq = kcmd_shmem = seg_shmem = 0;
}

bool
IOGPUTransport::sysMemory(uint64_t size, bool gpu_read_only, uint32_t &id, uint64_t &gpu, uint64_t &cpu)
{
    uint8_t in[kResourceArgs], out[kResourceArgs] = {};
    size_t out_size = sizeof(out);
    newResourceArgs(in, kKindSysMem, kFlagZeroFill | (gpu_read_only ? kFlagGPURead : 0));
    *(uint64_t *)(in + 0x48) = size;
    kern_return_t kr = IOConnectCallStructMethod(conn, kSelNewResource, in, sizeof(in), out, &out_size);
    id = *(uint32_t *)(out + 0x24);
    gpu = *(uint64_t *)(out + 0x00);
    cpu = *(uint64_t *)(out + 0x08);
    if (kr != KERN_SUCCESS || id == 0 || gpu == 0 || cpu == 0) {
        if (kr == KERN_SUCCESS && id != 0) {
            deleteResource(id);
        }
        fail("no GPU memory of %llu bytes (IOGPU: 0x%x)", (unsigned long long)size, kr);
        return false;
    }
    return true;
}

void
IOGPUTransport::deleteResource(uint32_t id)
{
    // trap 1, IOGPU.framework's fast path; selector 9 if a kernel should not have it
    if (akios_IOConnectTrap1(conn, kTrapDeleteResource, id) != KERN_SUCCESS) {
        uint64_t in = id;
        IOConnectCallScalarMethod(conn, kSelDeleteResource, &in, 1, nullptr, nullptr);
    }
}

bool
IOGPUTransport::newMemory(Bo &bo, uint64_t size, bool gpu_read_only)
{
    uint32_t id;
    uint64_t gpu, cpu;
    if (!sysMemory(size, gpu_read_only, id, gpu, cpu)) {
        return false;
    }
    // NONE ACROSS 4 GB, as panthor's addresses of ours were placed: a local storage descriptor holds its buffer's
    // address in a form that must not carry over 32 bits, and nothing else was ever given such a buffer. The address is
    // the task's now, not ours: one that crosses is made again at twice the size, and the half-size piece on one side
    // of the boundary is the buffer (the rest of it stays mapped, unused, until the buffer goes).
    if (size <= (1ULL << 32) && gpu >> 32 != (gpu + size - 1) >> 32) {
        deleteResource(id);
        if (!sysMemory(size * 2, gpu_read_only, id, gpu, cpu)) {
            return false;
        }
        if (gpu >> 32 != (gpu + size - 1) >> 32) {
            uint64_t skip = ((gpu + size - 1) & ~0xffffffffULL) - gpu;      // to the boundary inside the first half
            gpu += skip;
            cpu += skip;
        }
    }
    bo = Bo();
    bo.handle = id;
    bo.size = size;
    bo.va = gpu;
    bo.cpu = (void *)(uintptr_t)cpu;
    return true;
}

bool
IOGPUTransport::newSurface(Bo &bo, unsigned surface, uint64_t size)
{
    uint8_t in[kResourceArgs], out[kResourceArgs] = {};
    size_t out_size = sizeof(out);
    newResourceArgs(in, kKindSurface, 0);
    *(uint64_t *)(in + 0x38) = surface;                 // the surface's id; plane 0 (above): the whole of its memory
    kern_return_t kr = IOConnectCallStructMethod(conn, kSelNewResource, in, sizeof(in), out, &out_size);
    uint32_t id = *(uint32_t *)(out + 0x24);
    // +0x50, the mapping's start: where panthor's surface buffer began, so the description's offset still applies
    // (+0x00 is that plus the surface's own offset). The kernel maps it into the task there (write-combined) to give
    // it its GPU address, and answers no processor address -- the mapping is the task's all the same.
    uint64_t base = *(uint64_t *)(out + 0x50);
    if (kr != KERN_SUCCESS || id == 0 || base == 0) {
        if (kr == KERN_SUCCESS && id != 0) {
            deleteResource(id);
        }
        fail("IOSurface %u could not be had as GPU memory (IOGPU: 0x%x)", surface, kr);
        return false;
    }
    bo = Bo();
    bo.handle = id;
    bo.size = size;
    bo.va = base;
    bo.cpu = (void *)(uintptr_t)base;
    return true;
}

void
IOGPUTransport::freeMemory(Bo &bo)
{
    if (bo.handle != 0) {
        deleteResource(bo.handle);
    }
}

bool
IOGPUTransport::newHeap(uint32_t chunk_size, uint32_t initial, uint32_t most, uint32_t in_flight, uint32_t &handle,
    uint64_t &ctx, uint64_t &first)
{
    uint64_t in[4] = { chunk_size, initial, most, in_flight }, out[3] = {};
    uint32_t n = 3;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kSelTilerHeap, in, 4, out, &n);
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
IOGPUTransport::freeHeap(uint32_t handle)
{
    uint64_t in = handle;
    IOConnectCallScalarMethod(conn, kSelTilerHeapGone, &in, 1, nullptr, nullptr);
}

bool
IOGPUTransport::submit(uint64_t stream_va, uint32_t bytes, uint64_t &token)
{
    uint64_t seq;
    {
        std::lock_guard<std::mutex> g(lock);
        seq = next_seq++;
        outstanding++;
    }
    // IOGPUCommandBufferArgs: IOGPU's common 0x30 bytes, then the Mali's stream
    uint8_t args[0x40] = {};
    *(uint32_t *)(args + 0x00) = kcmd_shmem;
    *(uint32_t *)(args + 0x04) = seg_shmem;
    *(uint64_t *)(args + 0x10) = seq << 1;              // "scheduled"
    *(uint64_t *)(args + 0x18) = seq << 1 | 1;          // "completed"
    *(uint64_t *)(args + 0x30) = stream_va;
    *(uint32_t *)(args + 0x38) = bytes;
    uint32_t status = 0;
    kern_return_t kr = akios_IOConnectTrap4(conn, kTrapSubmit, queue, sizeof(args), (uintptr_t)args, (uintptr_t)&status);
    if (kr != KERN_SUCCESS) {
        std::lock_guard<std::mutex> g(lock);
        outstanding--;
        fail("IOGPU did not take the submission (0x%x)", kr);
        return false;
    }
    token = seq << 1 | 1;
    return true;
}

int
IOGPUTransport::wait(uint64_t token, int64_t timeout_ms, uint32_t &status)
{
    std::unique_lock<std::mutex> l(lock);
    if (!finished.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return done.count(token) != 0; })) {
        given_up.insert(token);
        return -1;
    }
    status = done[token];
    done.erase(token);
    return 0;
}

// THE COMPLETIONS: every answer taken off the notification queue as soon as the port says there is one; a completed
// token's status kept for its waiter (the queue's thread: hakios_metal_queue::run). The kernel sends the port a message
// only when an entry lands in an empty queue, so the queue is emptied before each wait; while work is out the wait is
// a second at most, in case such a message was not sent (the port holds one at a time).
void
IOGPUTransport::run()
{
    pthread_setname_np("metal-iogpu completions");
    for (;;) {
        while (IODataQueueDataAvailable(nq_memory)) {
            Notification n = {};
            uint32_t size = sizeof(n);
            if (IODataQueueDequeue(nq_memory, &n, &size) != kIOReturnSuccess) {
                IODataQueueDequeue(nq_memory, nullptr, &size);  // (an entry of another size: skipped, not looped on)
                continue;
            }
            if (!(n.token & 1)) {
                continue;                                       // ("scheduled": the commit has said so already)
            }
            static std::atomic<unsigned> said{0};
            if (n.status == 0xb && said++ < 8) {
                // (the fault's address: the kernel does not fill it in yet -- its log has it, "page fault ... at")
                fprintf(stderr, "metal-iogpu: %s: a GPU page fault in a command buffer (address 0x%llx)\n", getprogname(),
                    (unsigned long long)n.fault);
            }
            std::lock_guard<std::mutex> g(lock);
            if (outstanding > 0) {
                outstanding--;
            }
            if (given_up.erase(n.token) != 0) {
                continue;
            }
            done[n.token] = n.status;
            finished.notify_all();
        }
        bool busy;
        {
            std::lock_guard<std::mutex> g(lock);
            if (stopping) {
                return;
            }
            busy = outstanding > 0;
        }
        struct { mach_msg_header_t h; uint8_t room[64]; } msg;
        mach_msg(&msg.h, MACH_RCV_MSG | (busy ? MACH_RCV_TIMEOUT : 0), 0, sizeof(msg), port, busy ? 1000 : 0,
            MACH_PORT_NULL);
    }
}
