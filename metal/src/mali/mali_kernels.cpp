// mali_kernels.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

extern "C" {

hakios_metal_kernel *
hakios_metal_kernel_compile_bitcode_constants(const void *bitcode, size_t size, const hakios_metal_constant *constants,
    unsigned count)
{
    std::vector<mali_constant> values(count);
    char why[512];

    for (unsigned i = 0; i < count; i++) {
        values[i] = { constants[i].index, constants[i].bits };
    }
    auto *k = new hakios_metal_kernel;
    if (mali_kernel_compile(bitcode, size, values.data(), count, &k->binary, why, sizeof(why)) != 0) {
        fail("%s", why);
        delete k;
        return nullptr;
    }
    return k;
}

hakios_metal_kernel *
hakios_metal_kernel_compile_bitcode(const void *bitcode, size_t size)
{
    return hakios_metal_kernel_compile_bitcode_constants(bitcode, size, nullptr, 0);
}

void
hakios_metal_kernel_free(hakios_metal_kernel *k)
{
    if (k != nullptr) {
        k->release();       // (a command buffer that dispatches it keeps it until the GPU is done)
    }
}

const char *
hakios_metal_kernel_name(hakios_metal_kernel *k)
{
    return k ? k->binary.name : "";
}

unsigned
hakios_metal_kernel_resource_count(hakios_metal_kernel *k)
{
    return k ? k->binary.num_resources : 0;
}

int
hakios_metal_kernel_resource(hakios_metal_kernel *k, unsigned i, unsigned *kind, unsigned *index, unsigned *size_bytes)
{
    if (k == nullptr || i >= k->binary.num_resources) {
        return -1;
    }
    const mali_kernel_resource &r = k->binary.resources[i];
    if (kind) {
        *kind = r.kind;
    }
    if (index) {
        *index = r.index;
    }
    if (size_bytes) {
        *size_bytes = r.size;
    }
    return 0;
}

void
hakios_metal_kernel_threadgroup_size(hakios_metal_kernel *k, unsigned size[3])
{
    for (int i = 0; i < 3; i++) {
        size[i] = k ? k->binary.workgroup[i] : 0;
    }
}

unsigned
hakios_metal_kernel_max_threads(hakios_metal_kernel *k)
{
    return k ? k->maxThreads() : 0;
}

unsigned
hakios_metal_kernel_execution_width(hakios_metal_kernel *k)
{
    (void)k;
    return 16;              // Valhall's warp
}

unsigned
hakios_metal_kernel_threadgroup_memory(hakios_metal_kernel *k)
{
    return k ? k->binary.threadgroup_bytes : 0;
}
extern "C++" { namespace mali {

int
dispatchGrid(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned groups[3], const unsigned threads[3],
    const unsigned grid[3], hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count,
    hakios_metal_texture *const *textures, unsigned texture_count,
    hakios_metal_sampler *const *samplers, unsigned sampler_count)
{
    Job j = {};
    unsigned total = 1;

    if (k == nullptr) {
        fail("dispatch: no kernel");
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        if (threads[i] == 0 || threads[i] > 1024) {
            fail("dispatch of %s: a threadgroup of %u x %u x %u", k->binary.name, threads[0], threads[1], threads[2]);
            return -1;
        }
        if (k->binary.workgroup[i] != 0 && k->binary.workgroup[i] != threads[i]) {
            fail("dispatch of %s: the kernel's threadgroup is %u x %u x %u", k->binary.name, k->binary.workgroup[0],
                k->binary.workgroup[1], k->binary.workgroup[2]);
            return -1;
        }
        total *= threads[i];
    }
    if (total > k->maxThreads()) {
        fail("dispatch of %s: %u threads a threadgroup, %u at most", k->binary.name, total, k->maxThreads());
        return -1;
    }
    if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0) {
        return 0;           // nothing to run
    }
    for (unsigned r = 0; r < k->binary.num_resources; r++) {
        const mali_kernel_resource &res = k->binary.resources[r];
        if (res.kind != HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER && res.kind != HAKIOS_METAL_RESOURCE_DEVICE_BUFFER) {
            continue;
        }
        if (res.index >= count || buffers[res.index] == nullptr) {
            // (nothing bound: the device's zeroes stand for it)
            uint64_t nothing = nothingVa(cb->queue->device);
            if (nothing == 0) {
                fail("dispatch of %s: nothing bound at [[buffer(%u)]]", k->binary.name, res.index);
                return -1;
            }
            j.buffers[res.slot] = nothing;
            continue;
        }
        hakios_metal_buffer *b = buffers[res.index];
        uint64_t offset = offsets ? offsets[res.index] : 0;
        if (offset > b->bo.size || (res.size != 0 && offset + res.size > b->bo.size)) {
            fail("dispatch of %s: [[buffer(%u)]] at offset %llu is past its buffer", k->binary.name, res.index,
                (unsigned long long)offset);
            return -1;
        }
        j.buffers[res.slot] = b->bo.va + offset;        // (its slot in the push constants: the index, under 16)
    }
    // textures and samplers the kernel declares: bound, and of a kind it can use
    uint32_t nothing_at = 0, null_at = 0;
    for (unsigned r = 0; r < k->binary.num_resources; r++) {
        const mali_kernel_resource &res = k->binary.resources[r];
        if (res.kind == HAKIOS_METAL_RESOURCE_TEXTURE && res.size == 0xfffe && res.index < MALI_KERNEL_TEXTURES) {
            /* (11-20) the front end's reserved index for Metal's null texture in a texture chosen while the shader runs (a resource of size 0xfffe, air_null_texture_slot): the zero texture there, whatever is bound */
            null_at |= 1u << res.index;
        }
        if (res.kind == HAKIOS_METAL_RESOURCE_TEXTURE || res.kind == HAKIOS_METAL_RESOURCE_IMAGE) {
            if (res.index >= texture_count || textures[res.index] == nullptr || (null_at & 1u << res.index)) {
                // (nothing bound: the device's 4x4 of zeroes stands for it)
                if (res.index >= MALI_KERNEL_TEXTURES || nothingTexture(cb->queue->device) == nullptr) {
                    fail("dispatch of %s: nothing bound at [[texture(%u)]]", k->binary.name, res.index);
                    return -1;
                }
                nothing_at |= 1u << res.index;
                j.texture_count = res.index + 1 > j.texture_count ? res.index + 1 : j.texture_count;
                continue;
            }
            if (res.kind == HAKIOS_METAL_RESOURCE_IMAGE) {
                markBusy(cb, cb->queue->device, textures[res.index]);   // (a kernel writing a surface: marked as a draw is)
            }
            if (res.kind == HAKIOS_METAL_RESOURCE_IMAGE && !(textures[res.index]->usage & 2)) {
                fail("dispatch of %s: [[texture(%u)]] is written and was made without MTLTextureUsageShaderWrite", k->binary.name,
                    res.index);
                return -1;
            }
            j.texture_count = res.index + 1 > j.texture_count ? res.index + 1 : j.texture_count;
        } else if (res.kind == HAKIOS_METAL_RESOURCE_SAMPLER) {
            if (res.index >= MALI_KERNEL_TEXTURES) {
                fail("dispatch of %s: nothing bound at [[sampler(%u)]]", k->binary.name, res.index);
                return -1;
            }
            if (res.index >= sampler_count || samplers[res.index] == nullptr) {
                // (nothing bound, 11-12: a nearest, clamped sampler stands for it -- as for a texel fetch below)
                j.samplers[res.index][0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
            }
            j.sampler_count = res.index + 1 > j.sampler_count ? res.index + 1 : j.sampler_count;
        }
    }
    if (j.texture_count != 0 && j.sampler_count == 0) {
        j.sampler_count = 1;                // (a texel fetch wants a valid sampler 0: a nearest, clamped one)
        j.samplers[0][0] = 1 | 9u << 8 | 9u << 12 | 9u << 16 | 1u << 21 | 1u << 25 | 1u << 26 | 1u << 27 | 1u << 28;
    }
    for (unsigned t = 0; t < j.texture_count; t++) {
        j.textures[t] = (null_at & 1u << t) ? nothingTexture(cb->queue->device) : t < texture_count && textures[t] != nullptr ? textures[t] :
            (nothing_at & 1u << t) ? nothingTexture(cb->queue->device) : nullptr;
        { hakios_metal_texture *rt_ = (j.textures[t]); while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent; if (rt_ != nullptr) rt_->reads++; }
    }
    for (unsigned t = 0; t < j.sampler_count && t < sampler_count; t++) {
        if (samplers[t] != nullptr) {
            memcpy(j.samplers[t], samplers[t]->words, 32);
        }
    }
    hakios_metal_device *d = cb->queue->device;
    if (!k->upload(d)) {
        return -1;
    }
    // threadgroup memory: the static arrays and the encoder's length, an instance per workgroup the cores can hold at once
    // (Mesa's pan_calc_wls_instances); spill space: every thread a core can run (pan_get_total_stack_size)
    uint32_t wls = k->binary.wls_size;
    if (k->binary.threadgroup_arg_base != ~0u && k->binary.threadgroup_arg_base + cb->threadgroup_length > wls) {
        wls = k->binary.threadgroup_arg_base + cb->threadgroup_length;     // the argument, past the static arrays
    }
    if (k->binary.imageblock_bytes != 0 && k->binary.imageblock_base + total * k->binary.imageblock_bytes > wls) {
        wls = k->binary.imageblock_base + total * k->binary.imageblock_bytes;   // (11-20) an imageblock: a pixel a thread
    }
    if (wls != 0) {
        uint32_t per_task = d->max_threads / (d->max_tasks ? d->max_tasks : 1);
        uint32_t wg_per_task = (per_task + total - 1) / total;
        uint32_t most = npot(wg_per_task * (d->max_tasks ? d->max_tasks : 1));
        uint32_t wanted = npot(groups[0]) * npot(groups[1]) * npot(groups[2]);
        j.wls_size = npot(wls < 128 ? 128 : wls);
        j.wls_instances = wanted < most ? wanted : most;
        if (!grow(d, d->wls, (uint64_t)j.wls_size * j.wls_instances * d->core_range)) {
            return -1;
        }
    }
    if (k->binary.tls_size != 0 && (log2u((k->binary.tls_size + 15) / 16) >= 12 ||
        !grow(d, d->tls[log2u((k->binary.tls_size + 15) / 16)], (uint64_t)npot((k->binary.tls_size + 15) & ~15u) * d->max_threads * d->core_range))) {
        return -1;
    }
    j.shader = DISPATCH;
    j.kernel = k;
    memcpy(j.groups, groups, sizeof(j.groups));
    memcpy(j.threads, threads, sizeof(j.threads));
    memcpy(j.grid, grid, sizeof(j.grid));
    cb->blits.push_back(j);
    k->retain();
    cb->kernels.push_back(k);
    for (unsigned r = 0; r < k->binary.num_resources; r++) {
        const mali_kernel_resource &res = k->binary.resources[r];
        if ((res.kind == HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER || res.kind == HAKIOS_METAL_RESOURCE_DEVICE_BUFFER) &&
            res.index < count && buffers[res.index] != nullptr) {
            cb->hold(buffers[res.index]);
        }
    }
    for (unsigned t = 0; t < j.texture_count; t++) {
        if (j.textures[t] != nullptr) {
            j.textures[t]->retain();
            cb->held_textures.push_back(j.textures[t]);
        }
    }
    return 0;
}
} }  // namespace mali, extern "C++"

void
hakios_metal_cb_set_threadgroup_length(hakios_metal_cb *cb, unsigned length)
{
    cb->threadgroup_length = length;
}

int
hakios_metal_cb_dispatch(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned groups[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count)
{
    unsigned grid[3];

    for (int i = 0; i < 3; i++) {
        grid[i] = groups[i] * threads[i];
    }
    return dispatchGrid(cb, k, groups, threads, grid, buffers, offsets, count);
}

int
hakios_metal_cb_dispatch_threads(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned grid[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count)
{
    unsigned groups[3];

    for (int i = 0; i < 3; i++) {
        groups[i] = threads[i] ? (grid[i] + threads[i] - 1) / threads[i] : 0;
    }
    return dispatchGrid(cb, k, groups, threads, grid, buffers, offsets, count);
}
}  // extern "C"
