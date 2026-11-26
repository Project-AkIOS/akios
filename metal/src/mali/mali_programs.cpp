// mali_programs.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

extern "C" {
extern "C++" { namespace mali {

bool
cacheOn()
{
    static int on = -1;
    if (on < 0) {
        char args[1024] = {};
        size_t n = sizeof(args) - 1;
        on = !(sysctlbyname("kern.bootargs", args, &n, nullptr, 0) == 0 && strstr(args, "metal_cache=0") != nullptr);
    }
    return on != 0;
}

void
cacheTrim(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) return;
    struct Entry { std::string path; time_t mtime; uint64_t size; };
    std::vector<Entry> files;
    uint64_t total = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back({ p, st.st_mtime, (uint64_t)st.st_size });
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    if (total <= CACHE_MOST) return;
    std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) { return a.mtime < b.mtime; });
    unsigned removed = 0;
    uint64_t was = total;
    for (const Entry &f : files) {
        if (total <= CACHE_LEAST) break;
        if (unlink(f.path.c_str()) == 0) {
            total -= f.size;
            removed++;
        }
    }
    fprintf(stderr, "hakios-metal: the shader cache was %llu KB: %u of its oldest files removed, %llu KB left\n",
        (unsigned long long)(was >> 10), removed, (unsigned long long)(total >> 10));
}

const std::string &
cacheDir()
{
    static std::string dir;
    static std::once_flag once;
    std::call_once(once, [] {
        const char *tries[] = { "/private/var/mobile/Library/Caches/com.hakios.metal-mali", "/private/var/tmp/com.hakios.metal-mali" };
        for (const char *t : tries) {
            mkdir(t, 0755);
            struct stat st;
            if (stat(t, &st) == 0 && S_ISDIR(st.st_mode) && access(t, W_OK) == 0) {
                dir = t;
                break;
            }
        }
        if (!dir.empty()) {
            std::string d = dir;
            std::thread([d] { cacheTrim(d); }).detach();
        }
    });
    return dir;
}

void
fnv(uint64_t &h, const void *data, size_t n)
{
    auto *b = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ULL;
    }
}

// this library's image UUID (LC_UUID): another build, another key
void
fnvImage(uint64_t &h)
{
    static uint8_t uuid[16];
    static bool have = false;
    static std::once_flag once;
    std::call_once(once, [] {
        Dl_info info;
        if (dladdr((const void *)&fnvImage, &info) == 0 || info.dli_fbase == nullptr) {
            return;
        }
        auto *mh = (const struct mach_header_64 *)info.dli_fbase;
        auto *lc = (const struct load_command *)(mh + 1);
        for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize)) {
            if (lc->cmd == LC_UUID) {
                memcpy(uuid, ((const struct uuid_command *)lc)->uuid, 16);
                have = true;
                break;
            }
        }
    });
    if (have) {
        fnv(h, uuid, 16);
    }
}

uint64_t
cacheKey(const void *vs, size_t vs_size, const std::vector<mali_constant> &vc, const void *fs, size_t fs_size,
    const std::vector<mali_constant> &fc, unsigned format, const mali_vertex_fetch *fetch,
    const mali_shader_blend *blend)
{
    uint64_t h = 14695981039346656037ULL;
    fnv(h, kCacheMagic, sizeof(kCacheMagic));
#ifdef HAKIOS_MALI_CACHE_BUILD
    // the compiler's own make-up (the Makefile's MALI_CACHE_BUILD), not this library's build: see there
    fnv(h, HAKIOS_MALI_CACHE_BUILD, sizeof(HAKIOS_MALI_CACHE_BUILD) - 1);
    fnv(h, &sizeof_binary, sizeof(sizeof_binary));
    {
        // (and the compile options the boot-args choose: compile_mali.c's mali_mediump=0)
        static const uint32_t no_mediump = bootArg("mali_mediump=0") ? 1 : 0;
        fnv(h, &no_mediump, sizeof(no_mediump));
        // (11-26: and mali_defer=0 -- a deferred fragment stage, kept without code, must not answer for a refusal)
        static const uint32_t no_defer = bootArg("mali_defer=0") ? 1 : 0;
        if (no_defer) fnv(h, &no_defer, sizeof(no_defer));
    }
#else
    fnvImage(h);
#endif
    fnv(h, &vs_size, sizeof(vs_size));
    fnv(h, vs, vs_size);
    for (const mali_constant &k : vc) fnv(h, &k, sizeof(k));
    fnv(h, &fs_size, sizeof(fs_size));
    fnv(h, fs, fs_size);
    for (const mali_constant &k : fc) fnv(h, &k, sizeof(k));
    fnv(h, &format, sizeof(format));
    if (fetch != nullptr) fnv(h, fetch, sizeof(*fetch));
    if (blend != nullptr) {
        static const char tag[] = "shader blend";
        fnv(h, tag, sizeof(tag));
        fnv(h, blend, sizeof(*blend));
    }
    return h;
}

std::string
cachePath(uint64_t key)
{
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.prog", (unsigned long long)key);
    return cacheDir() + name;
}

bool
writeAll(int fd, const void *p, size_t n)
{
    auto *b = (const uint8_t *)p;
    while (n != 0) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

bool
readAll(int fd, void *p, size_t n)
{
    auto *b = (uint8_t *)p;
    while (n != 0) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}
void
cacheSay()
{
    unsigned n = cache_hits + cache_misses + cache_bad;
    if (n == 40) {
        fprintf(stderr, "hakios-metal: shader cache %s: %u found (%u of them in the image's seed), %u not there, %u not whole, "
            "%u not written (of the first 40 looks)\n", cacheDir().empty() ? "(none: no directory)" : cacheDir().c_str(),
            (unsigned)cache_hits, (unsigned)cache_seeded, (unsigned)cache_misses, (unsigned)cache_bad, (unsigned)cache_unwritten);
    }
}

// THE IMAGE'S SEED (TODO 21, 11-11): programs compiled on an earlier boot and put into the image's RAM disk
// (devices/akita/tools/usbmux-pull-cache.py -> userland/hakios-metal-cache-mali -> userland/build.sh), read-only, looked
// at before the phone's own cache: at boot a 160 KB file of the latter took 1.1-2.5 s to read (behind the boot's ~2 GB
// of storage reads: CoreAnimation's seven generic pipelines ~12 s, their draws skipped meanwhile), and a new image's
// first boot compiled for minutes. Names as the cache's own (cachePath); boot-arg metal_seed=0: not looked at.
const std::string &
seedDir()
{
    static std::string dir;
    static std::once_flag once;
    std::call_once(once, [] {
        const char *d = "/akita/share/hakios-metal-cache-mali";      // (11-23: was /redwood, missed by the rename)
        struct stat st;
        if (!bootArg("metal_seed=0") && stat(d, &st) == 0 && S_ISDIR(st.st_mode)) {
            dir = d;
        }
    });
    return dir;
}
bool
refusalLoad(uint64_t key, char *why, size_t why_size)
{
    if (!cacheOn() || cacheDir().empty()) return false;
    int fd = open((cachePath(key) + ".refused").c_str(), O_RDONLY);
    if (fd < 0) return false;
    char text[512] = {};
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0 || strstr(text, kTooBig) == nullptr) return false;
    // (only under the same limit: a refusal remembered under mali_maxblocks=300 must not outlive a raised limit)
    char limit[48];
    unsigned max_blocks = 1000;
    {
        char args[1024] = {};
        size_t an = sizeof(args) - 1;
        const char *at = nullptr;
        if (sysctlbyname("kern.bootargs", args, &an, nullptr, 0) == 0 && (at = strstr(args, "mali_maxblocks=")) != nullptr)
            max_blocks = (unsigned)strtoul(at + 15, nullptr, 10);
    }
    snprintf(limit, sizeof(limit), "(mali_maxblocks=%u)", max_blocks);
    if (strstr(text, limit) == nullptr) return false;
    snprintf(why, why_size, "%s (remembered)", text);
    return true;
}
void
refusalStore(uint64_t key, const char *why)
{
    if (!cacheOn() || cacheDir().empty() || strstr(why, kTooBig) == nullptr) return;
    std::string path = cachePath(key) + ".refused", tmp = path + "." + std::to_string(getpid()) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    bool ok = write(fd, why, strlen(why)) == (ssize_t)strlen(why);
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) unlink(tmp.c_str());
}

bool
cacheLoad(uint64_t key, mali_kernel_binary *vs, mali_kernel_binary *fs)
{
    if (!cacheOn()) { cache_misses++; cacheSay(); return false; }
    int fd = -1;
    bool seeded = false;
    if (!seedDir().empty()) {
        char name[32];
        snprintf(name, sizeof(name), "/%016llx.prog", (unsigned long long)key);
        fd = open((seedDir() + name).c_str(), O_RDONLY);
        seeded = fd >= 0;
    }
    if (fd < 0) {
        if (cacheDir().empty()) { cache_misses++; cacheSay(); return false; }
        std::string path = cachePath(key);
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) { cache_misses++; cacheSay(); return false; }
        utimes(path.c_str(), nullptr);          // (used now: the trim keeps it)
    }
    char magic[8];
    uint64_t k = 0;
    uint32_t struct_size = 0;
    bool ok = readAll(fd, magic, 8) && memcmp(magic, kCacheMagic, 8) == 0 && readAll(fd, &k, 8) && k == key &&
        readAll(fd, &struct_size, 4) && struct_size == sizeof(mali_kernel_binary);
    mali_kernel_binary *b[2] = { vs, fs };
    for (int i = 0; i < 2 && ok; i++) {
        ok = readAll(fd, b[i], sizeof(mali_kernel_binary));
        b[i]->code = nullptr;
        b[i]->varyings = nullptr;               // (the layout a fragment stage is compiled against: not kept)
        // (11-26) a deferred fragment stage (compile_mali.h) is kept with no code: what it reads, for the plug-in to
        // specialise it, without the seconds its front end takes again at every pipeline of it
        if (ok && ((b[i]->size == 0 && !(i == 1 && b[i]->deferred)) || b[i]->size > (16u << 20))) ok = false;
        if (ok && b[i]->size != 0) {
            b[i]->code = malloc(b[i]->size);
            ok = b[i]->code != nullptr && readAll(fd, b[i]->code, b[i]->size);
        }
    }
    close(fd);
    (ok ? cache_hits : cache_bad)++;
    if (ok && seeded) cache_seeded++;
    cacheSay();
    if (!ok) {
        free(vs->code);
        free(fs->code);
        memset(vs, 0, sizeof(*vs));
        memset(fs, 0, sizeof(*fs));
    }
    return ok;
}

void
cacheStore(uint64_t key, const mali_kernel_binary &vs, const mali_kernel_binary &fs)
{
    if (!cacheOn() || cacheDir().empty()) return;
    std::string path = cachePath(key), tmp = path + "." + std::to_string(getpid()) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { cache_unwritten++; return; }
    uint32_t struct_size = sizeof(mali_kernel_binary);
    bool ok = writeAll(fd, kCacheMagic, 8) && writeAll(fd, &key, 8) && writeAll(fd, &struct_size, 4);
    const mali_kernel_binary *b[2] = { &vs, &fs };
    for (int i = 0; i < 2 && ok; i++) {
        ok = writeAll(fd, b[i], sizeof(mali_kernel_binary)) && writeAll(fd, b[i]->code, b[i]->size);
    }
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        cache_unwritten++;
    }
}
} }  // namespace mali, extern "C++"

hakios_metal_program *
hakios_metal_program_new_bitcode_constants(const void *vs_bitcode, size_t vs_size, const hakios_metal_constant *vs_constants,
    unsigned vs_count, const void *fs_bitcode, size_t fs_size, const hakios_metal_constant *fs_constants, unsigned fs_count,
    unsigned color_pixel_format)
{
    std::vector<mali_constant> vc(vs_count), fc(fs_count);
    char why[512];

    // A PIPELINE WITH NO FRAGMENT FUNCTION (11-15): depth (and stencil) alone, a shadow map's -- refused until now. Its
    // fragment stage is a built-in that writes nothing (mali_depth_only_fragment_compile); the draw's colour target, if
    // the pass has none, a stand-in nothing is written to (hakios_metal_cb_draw...: color0 NULL)
    const bool depth_only = fs_bitcode == nullptr || fs_size == 0;
    if (depth_only) {
        fs_bitcode = nullptr;
        fs_size = 0;
    }
    for (unsigned i = 0; i < vs_count; i++) {
        vc[i] = { vs_constants[i].index, vs_constants[i].bits };
    }
    for (unsigned i = 0; i < fs_count; i++) {
        fc[i] = { fs_constants[i].index, fs_constants[i].bits };
    }
    auto *p = new hakios_metal_program;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t key = cacheKey(vs_bitcode, vs_size, vc, fs_bitcode, fs_size, fc, color_pixel_format, nullptr);
    auto t1 = std::chrono::steady_clock::now();
    bool found = cacheLoad(key, &p->vs, &p->fs);
    auto t2 = std::chrono::steady_clock::now();
    {
        // (a slow program's time by step, 11-10: CoreAnimation's uber pipelines took 1-3 s with their cache file found)
        auto ms = [](std::chrono::steady_clock::duration d) {
            return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        };
        if (ms(t2 - t0) >= 200) {
            fprintf(stderr, "hakios-metal: program %016llx: key %lld ms (bitcode %zu + %zu bytes), cache %s %lld ms (%u + %u bytes of code)\n",
                (unsigned long long)key, ms(t1 - t0), vs_size, fs_size, found ? "found in" : "not found in", ms(t2 - t1),
                found ? p->vs.size : 0, found ? p->fs.size : 0);
        }
    }
    if (!found && refusalLoad(key, why, sizeof(why))) {
        fail("%s", why);
        p->release();
        return nullptr;
    }
    if (!found) {
        if (mali_stage_compile(vs_bitcode, vs_size, vc.data(), vs_count, 0, nullptr, &p->vs, why, sizeof(why)) != 0 ||
            (depth_only ? mali_depth_only_fragment_compile(&p->vs, &p->fs, why, sizeof(why))
                        : mali_stage_compile(fs_bitcode, fs_size, fc.data(), fs_count, 1, &p->vs, &p->fs, why, sizeof(why))) != 0) {
            refusalStore(key, why);
            fail("%s", why);
            p->release();
            return nullptr;
        }
        cacheStore(key, p->vs, p->fs);
    }
    p->color_format = color_pixel_format;
    snprintf(p->name, sizeof(p->name), "%s+%s", p->vs.name, p->fs.name);
    {
        // (boot-arg metal_shaderstats, 11-11: each program's compiled size, registers and spilling -- CoreAnimation's
        // full-screen cph passes took 7 ms each with a few draws: how big is the shader)
        static const bool stats = bootArg("metal_shaderstats");
        if (stats) {
            fprintf(stderr, "hakios-metal: shader %s: vertex %u bytes %u regs tls %u; fragment %u bytes %u regs tls %u, "
                "%u constants, %u resources%s\n", p->name, p->vs.size, p->vs.work_regs, p->vs.tls_size, p->fs.size,
                p->fs.work_regs, p->fs.tls_size, fs_count, p->fs.num_resources, found ? " (cached)" : "");
        }
    }
    p->vs_bitcode.assign((const uint8_t *)vs_bitcode, (const uint8_t *)vs_bitcode + vs_size);
    if (!depth_only) {
        p->fs_bitcode.assign((const uint8_t *)fs_bitcode, (const uint8_t *)fs_bitcode + fs_size);
    }
    p->vs_constants = vc;
    p->fs_constants = fc;
    return p;
}

hakios_metal_program *
hakios_metal_program_new_bitcode(const void *vs_bitcode, size_t vs_size, const void *fs_bitcode, size_t fs_size,
    unsigned color_pixel_format)
{
    return hakios_metal_program_new_bitcode_constants(vs_bitcode, vs_size, nullptr, 0, fs_bitcode, fs_size, nullptr, 0,
        color_pixel_format);
}

void hakios_metal_program_free(hakios_metal_program *p) { if (p) p->release(); }
const char *hakios_metal_program_name(hakios_metal_program *p) { return p ? p->name : ""; }
void hakios_metal_program_set_label(hakios_metal_program *p, const char *label)
{
    if (p != nullptr && label != nullptr) snprintf(p->label, sizeof(p->label), "%s", label);
}

unsigned
hakios_metal_program_stage_words(hakios_metal_program *p, unsigned stage)
{
    return p == nullptr ? 0 : (unsigned)((stage == 0 ? p->vs.size : p->fs.size) / 8);
}

unsigned
hakios_metal_program_seen_contents(hakios_metal_program *base, unsigned binding, void *bytes, size_t each,
    unsigned *lengths, unsigned max)
{
    if (base == nullptr || max == 0 || each == 0) return 0;
    uint64_t key;
    {
        std::lock_guard<std::mutex> g(base->lock);
        key = seenKey(base);
    }
    return seenRead(key, binding, bytes, each, lengths, max);
}

int
hakios_metal_program_control_mask(hakios_metal_program *p, unsigned binding, uint8_t mask[32])
{
    if (p == nullptr) return 0;
    std::lock_guard<std::mutex> g(p->lock);
    if (!p->fs.deferred || p->fs.control_binding != binding) return 0;
    memcpy(mask, p->fs.control, 32);
    return 1;
}

unsigned
hakios_metal_program_resource_count(hakios_metal_program *p, unsigned stage)
{
    return p == nullptr ? 0 : stage == 0 ? p->vs.num_resources : p->fs.num_resources;
}

int
hakios_metal_program_resource(hakios_metal_program *p, unsigned stage, unsigned i, unsigned *kind, unsigned *index,
    unsigned *size_bytes)
{
    const mali_kernel_binary *b = p == nullptr ? nullptr : stage == 0 ? &p->vs : &p->fs;
    if (b == nullptr || i >= b->num_resources) {
        return -1;
    }
    if (kind) *kind = b->resources[i].kind;
    if (index) *index = b->resources[i].index;
    if (size_bytes) *size_bytes = b->resources[i].size;
    return 0;
}

const char *
hakios_metal_program_resource_name(hakios_metal_program *p, unsigned stage, unsigned i, unsigned *type_size)
{
    const mali_kernel_binary *b = p == nullptr ? nullptr : stage == 0 ? &p->vs : &p->fs;
    if (b == nullptr || i >= b->num_resources) {
        return nullptr;
    }
    if (type_size) *type_size = b->resources[i].type_size;
    return b->resources[i].name;
}

// (recorded; a draw refuses what the Mali build does not serve yet, by name)
int
hakios_metal_program_set_vertex_attribute(hakios_metal_program *p, unsigned index, unsigned format, unsigned offset,
    unsigned buffer_index)
{
    if (p == nullptr || index >= MALI_VERTEX_SLOTS) {
        fail("a vertex attribute past 30");
        return -1;
    }
    std::lock_guard<std::mutex> g(p->lock);
    p->fetch.attributes[index].format = format;
    p->fetch.attributes[index].offset = offset;
    p->fetch.attributes[index].buffer = buffer_index;
    if (format != 0) {
        p->fetch_described = true;              // (every slot is set, described or not: only a format counts)
    }
    p->fetch_compiled = false;
    return 0;
}

int
hakios_metal_program_set_vertex_layout(hakios_metal_program *p, unsigned buffer_index, unsigned stride,
    unsigned step_function, unsigned step_rate)
{
    if (p == nullptr || buffer_index >= MALI_VERTEX_SLOTS) {
        fail("a vertex layout past 30");
        return -1;
    }
    std::lock_guard<std::mutex> g(p->lock);
    p->fetch.layouts[buffer_index].stride = stride;
    p->fetch.layouts[buffer_index].step_function = step_function;
    p->fetch.layouts[buffer_index].step_rate = step_rate;
    p->fetch_compiled = false;
    return 0;
}
extern "C++" { namespace mali {
// the fragment constants as the cache's key holds them: a specialised program's buffer bytes after them (pseudo-
// constants: what the stage is compiled for, not what the front end is given)
std::vector<mali_constant>
keyedConstants(const hakios_metal_program *p, const std::vector<mali_constant> &fc)
{
    if (p->known_binding < 0) return fc;
    // (11-26: KNOWN is already masked for a deferred base -- compile_mali.c's control_mask, part of what the compiler's
    // build hash covers -- so the masked bytes are the key, as wanted)
    std::vector<mali_constant> k(fc.size() + 1 + (p->known.size() + 7) / 8);   // (value-initialised: no stray padding)
    std::copy(fc.begin(), fc.end(), k.begin());
    size_t at = fc.size();
    k[at].index = 0x40000000u | (unsigned)p->known_binding;
    k[at++].bits = p->known.size();
    for (size_t i = 0; i < p->known.size(); i += 8, at++) {
        uint64_t w = 0;
        memcpy(&w, p->known.data() + i, std::min<size_t>(8, p->known.size() - i));
        k[at].index = 0x40010000u + (unsigned)(i / 8);
        k[at].bits = w;
    }
    return k;
}
// the stages again with the vertex descriptor's attributes fetched in the shader, before the program's first upload;
// true when there is nothing to do or it was done, false with p->fetch_refusal said
bool
compileFetch(hakios_metal_program *p)
{
    std::lock_guard<std::mutex> g(p->lock);
    bool blended = p->shader_blend.mask != 0 || p->shader_blend.format[0] || p->shader_blend.format[1] ||
        p->shader_blend.format[2] || p->shader_blend.format[3];   // (a float target's format: the fetch's conversion)
    // (11-23) a program without a fragment function (depth or stencil only, hakios_depth_only_fs) writes no colour: its
    // targets' formats do not matter -- iOS 27's path_edges_vertex draws, an RG32Float target in the pass, were refused
    if (p->fs_bitcode.empty()) {
        blended = false;
    }
    if ((!p->fetch_described && !blended) || p->fetch_compiled) {
        return p->fetch_refusal.empty();
    }
    p->fetch_compiled = true;
    if (p->device != nullptr) {
        p->fetch_refusal = "a vertex descriptor or blending set after the program's first draw";
        return false;
    }
    if (p->vs_bitcode.empty() || p->fs_bitcode.empty()) {
        p->fetch_refusal = "blending in the shader for a program without its bitcode";
        return false;
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
            p->fetch_refusal = std::string(p->fetch_described ? "the vertex descriptor: " : "blending in the shader: ") + why;
            return false;
        }
        cacheStore(key, vs, fs);
    }
    mali_kernel_binary_free(&p->vs);
    mali_kernel_binary_free(&p->fs);
    p->vs = vs;
    p->fs = fs;
    p->fetch_refusal.clear();
    return true;
}
uint64_t
seenKey(const hakios_metal_program *p)
{
    bool blended = p->shader_blend.mask != 0 || p->shader_blend.format[0] || p->shader_blend.format[1] ||
        p->shader_blend.format[2] || p->shader_blend.format[3];
    if (p->fs_bitcode.empty()) blended = false;
    static const mali_constant tag = { 0x53454e00u, 0x6e656573u };     // ("seen": apart from every program's own key)
    std::vector<mali_constant> fc = p->fs_constants;
    fc.push_back(tag);
    return cacheKey(p->vs_bitcode.data(), p->vs_bitcode.size(), p->vs_constants, p->fs_bitcode.data(), p->fs_bitcode.size(),
                    fc, p->color_format, p->fetch_described ? &p->fetch : nullptr, blended ? &p->shader_blend : nullptr);
}
// a record: binding (4 bytes), length (4), the bytes; appended once a content (O_APPEND: whole records, one process)
static std::mutex seen_lock;
unsigned
seenRead(uint64_t key, unsigned binding, void *bytes, size_t each, unsigned *lengths, unsigned max)
{
    if (!cacheOn() || cacheDir().empty()) return 0;
    int fd = open((cachePath(key) + ".seen").c_str(), O_RDONLY);
    if (fd < 0) return 0;
    unsigned n = 0;
    uint32_t head[2];
    uint8_t buf[4096];
    while (n < max && readAll(fd, head, sizeof(head)) && head[1] != 0 && head[1] <= sizeof(buf) && readAll(fd, buf, head[1])) {
        if (head[0] != binding || head[1] > each) continue;
        if (bytes != nullptr) memcpy((uint8_t *)bytes + (size_t)n * each, buf, head[1]);
        if (lengths != nullptr) lengths[n] = head[1];
        n++;
    }
    close(fd);
    return n;
}
void
seenNote(const hakios_metal_program *p)
{
    if (p->known_binding < 0 || p->known.empty() || p->known.size() > 4096 || !cacheOn() || cacheDir().empty()) return;
    std::lock_guard<std::mutex> g(seen_lock);
    uint64_t key = seenKey(p);
    std::string path = cachePath(key) + ".seen";
    {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            uint32_t head[2];
            uint8_t buf[4096];
            unsigned records = 0;
            while (readAll(fd, head, sizeof(head)) && head[1] != 0 && head[1] <= sizeof(buf) && readAll(fd, buf, head[1])) {
                records++;
                if (head[0] == (uint32_t)p->known_binding && head[1] == p->known.size() &&
                    memcmp(buf, p->known.data(), head[1]) == 0) {
                    close(fd);
                    utimes(path.c_str(), nullptr);      // (used now: the trim keeps it)
                    return;
                }
            }
            close(fd);
            if (records >= 1024) return;            // (enough: the plug-in keeps 256 a pipeline)
        }
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    std::vector<uint8_t> rec(8 + p->known.size());
    uint32_t head[2] = { (uint32_t)p->known_binding, (uint32_t)p->known.size() };
    memcpy(rec.data(), head, 8);
    memcpy(rec.data() + 8, p->known.data(), p->known.size());
    (void)writeAll(fd, rec.data(), rec.size());
    close(fd);
}
bool
maliFactor(unsigned metal, MaliFactor &f)
{
    static const MaliFactor table[10] = {
        { MF_ONE, true }, { MF_ONE, false }, { MF_SRC, false }, { MF_SRC, true }, { MF_SRC_ALPHA, false },
        { MF_SRC_ALPHA, true }, { MF_DEST, false }, { MF_DEST, true }, { MF_DEST_ALPHA, false }, { MF_DEST_ALPHA, true },
    };
    if (metal >= 10) {
        return false;           // alpha saturated, the blend constant, the second source: not yet
    }
    f = table[metal];
    return true;
}

// One channel group's A + B * C (pan_blend.c's to_mali_function, Mesa 26.2.5), for Metal's operation (0 add, 1
// subtract, 2 reverse subtract); 0 and WHY for what fixed function cannot do (min, max; factors that neither match nor
// are zero or one)
uint32_t
maliBlendFunction(unsigned op, unsigned src_metal, unsigned dst_metal, bool alpha, const char *&why)
{
    MaliFactor src, dst;
    if (op > 2) {
        why = "a min or max blend operation (fixed function adds and subtracts only)";
        return 0;
    }
    if (!maliFactor(src_metal, src) || !maliFactor(dst_metal, dst)) {
        why = "a blend factor with no fixed-function operand (alpha saturated, the blend colour, the second source)";
        return 0;
    }
    enum { A_ZERO = 1, A_SRC = 2, A_DEST = 3 };
    enum { B_MINUS = 0, B_PLUS = 1, B_SRC = 2, B_DEST = 3 };
    unsigned a = 0, b = 0, c = 0;
    bool neg_a = false, neg_b = false, inv_c = false;
    // the hardware's C has zero and inverts it to one, gallium's ONE is uninverted: hence the extra invert for ONE
    auto inverted = [](MaliFactor f) { return f.inv ^ (f.base == MF_ONE); };
    auto c_of = [](MaliFactor f) { return f.base == MF_ONE ? 1u /* zero */ : f.base; };
    bool src_zero = src.base == MF_ONE && src.inv, src_one = src.base == MF_ONE && !src.inv;
    bool dst_zero = dst.base == MF_ONE && dst.inv, dst_one = dst.base == MF_ONE && !dst.inv;
    bool two_src_dest = op == 0 && (src.base == MF_DEST || (alpha && src.base == MF_DEST_ALPHA)) && !src.inv &&
        (dst.base == MF_SRC || (alpha && dst.base == MF_SRC_ALPHA)) && !dst.inv;
    if (src_zero) {
        a = A_ZERO; b = B_DEST; neg_b = op == 1; inv_c = inverted(dst); c = c_of(dst);
    } else if (src_one) {
        a = A_SRC; b = B_DEST; neg_b = op == 1; neg_a = op == 2; inv_c = inverted(dst); c = c_of(dst);
    } else if (dst_zero) {
        a = A_ZERO; b = B_SRC; neg_b = op == 2; inv_c = inverted(src); c = c_of(src);
    } else if (dst_one) {
        a = A_DEST; b = B_SRC; neg_a = op == 1; neg_b = op == 2; inv_c = inverted(src); c = c_of(src);
    } else if (src.base == dst.base && src.inv == dst.inv) {
        a = A_ZERO; inv_c = inverted(src); c = c_of(src);
        b = op == 0 ? B_PLUS : B_MINUS;
        neg_b = op == 2;
    } else if (two_src_dest) {
        a = A_ZERO; b = B_DEST; c = 4;                  // dest * (2 * src)
    } else if (src.base == dst.base) {                  // the same factor, one of them inverted
        a = A_DEST; inv_c = inverted(src); c = c_of(src);
        if (op == 0) { b = B_MINUS; }
        else if (op == 2) { b = B_PLUS; neg_b = true; }
        else { b = B_PLUS; neg_a = true; }
    } else {
        why = "blend factors that neither match nor are zero or one (fixed function has one multiplier)";
        return 0;
    }
    return a | (neg_a ? 1u << 3 : 0) | b << 4 | (neg_b ? 1u << 7 : 0) | c << 8 | (inv_c ? 1u << 11 : 0);
}
// one colour attachment's blend state as the fixed-function equation word, whether it reads the tile buffer, or why not
void
encodeBlend(int enabled, unsigned rgb_op, unsigned alpha_op, unsigned src_rgb, unsigned dst_rgb, unsigned src_alpha,
    unsigned dst_alpha, unsigned write_mask, uint32_t &equation, bool &reads_dest, std::string &refusal,
    mali_blend_rt *shader, bool *in_shader)
{
    if (in_shader) *in_shader = false;
    // Metal's write mask: alpha 1, blue 2, green 4, red 8; Mali's colour mask: red bit 0 .. alpha bit 3
    uint32_t mask = (write_mask & 8 ? 1u : 0) | (write_mask & 4 ? 2u : 0) | (write_mask & 2 ? 4u : 0) | (write_mask & 1 ? 8u : 0);
    refusal.clear();
    reads_dest = mask != 0xf;                   // a channel kept is read from the tile buffer
    if (!enabled) {
        equation = 0x132 | 0x132 << 12 | mask << 28;
        return;
    }
    const char *why = nullptr;
    uint32_t rgb = maliBlendFunction(rgb_op, src_rgb, dst_rgb, false, why);
    uint32_t alpha = why == nullptr ? maliBlendFunction(alpha_op, src_alpha, dst_alpha, true, why) : 0;
    // in the shader instead: any operation and factor -- the blend colour from the draw's push constants
    // (MALI_DRAW_BLEND_COLOR), the second source the fragment function's [[color(n), index(1)]] (nir_lower_blend)
    auto shaderable = [](unsigned f) { return f <= 18; };
    const bool can_shade = shader != nullptr && rgb_op <= 4 && alpha_op <= 4 && shaderable(src_rgb) && shaderable(dst_rgb) &&
        shaderable(src_alpha) && shaderable(dst_alpha);
    if (can_shade) {
        // (kept whichever way it is blended: a raw target's blend is the shader's even when fixed function could --
        // rawBlend)
        *shader = { rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask };
    }
    if (why != nullptr && can_shade) {
        *in_shader = true;
        equation = 0x132 | 0x132 << 12 | 0xfu << 28;    // the shader's result as it is (it applied the write mask)
        reads_dest = true;
        return;
    }
    if (why != nullptr) {
        char factors[96];
        snprintf(factors, sizeof(factors), " (operations %u/%u, factors %u %u / %u %u)", rgb_op, alpha_op, src_rgb, dst_rgb,
            src_alpha, dst_alpha);
        refusal = std::string("blending: ") + why + factors;
        return;
    }
    equation = rgb | alpha << 12 | mask << 28;
    reads_dest = true;
}
// A RAW COLOUR ATTACHMENT (R32Float: the tile buffer holds its 32 bits as they are, and the fixed-function blend does
// not know such a target) with blending on: the blend computed in the fragment shader (nir_lower_blend over the
// target's own format), the hardware writing the result as it is -- as when fixed function has no such equation
void
rawBlend(hakios_metal_program *p, unsigned i)
{
    if (i > 3) {
        return;
    }
    // WHAT THE COMPILE IS TOLD OF THE ATTACHMENT'S FORMAT (11-13): R32Float always (the fetch's conversion); a half-float
    // one only when its blend is computed in the shader (nir_lower_blend then reads and writes it as the floats it holds
    // -- it took every target for RGBA8, clamped to 0..1) -- told otherwise, the program takes the shader-blend compile
    // for nothing
    const unsigned f = p->rt_format[i];
    // (11-19) ALWAYS, now: a function that only READS a float attachment (framebuffer fetch) has no blend of its own there,
    // was not told, and read the half floats as RGBA8 -- RenderBox's accumulator_color reads the coverage its
    // mask_accumulate draws left in an RG16Float attachment: every shape filled that way was missing (the News widget's
    // placeholders, "No events today" in Calendar's). Float attachments are the widgets' alone; metal_rtfmt=0: as before.
    static const bool told_always = !bootArg("metal_rtfmt=0");
    p->shader_blend.format[i] = f == 55 || f == 105 || f == 63 || ((told_always || (p->shader_blend.mask & 1u << i)) && (f == 25 || f == 65 || f == 115 || f == 92)) ||
        f == 71 || f == 81 ? f : 0;       // (an sRGB target's always: a framebuffer fetch from it must decode, 11-13)
    if ((f != 55 && f != 105) || !p->rt_blend_on[i]) {
        return;
    }
    p->shader_blend.mask |= 1u << i;
    if (i == 0) {
        p->blend_equation = 0x132 | 0x132 << 12 | 0xfu << 28;
        p->blend_reads_dest = true;
        p->blend_refusal.clear();
    } else {
        p->more_blend_equation[i - 1] = 0x132 | 0x132 << 12 | 0xfu << 28;
        p->more_blend_reads_dest[i - 1] = true;
        p->more_blend_refusal[i - 1].clear();
    }
}
} }  // namespace mali, extern "C++"

// colour attachment INDEX's MTLPixelFormat, from the pipeline's descriptor (11-12 night): the ones that are not 8 bits a
// channel are what the stage's compile and the blend go by
void
hakios_metal_program_set_attachment_format(hakios_metal_program *p, unsigned index, unsigned pixel_format)
{
    if (p == nullptr || index > 3) {
        return;
    }
    p->rt_format[index] = pixel_format;
    rawBlend(p, index);
}

void
hakios_metal_program_set_blend(hakios_metal_program *p, int enabled, unsigned rgb_op, unsigned alpha_op, unsigned src_rgb,
    unsigned dst_rgb, unsigned src_alpha, unsigned dst_alpha, unsigned write_mask)
{
    if (p == nullptr) {
        return;
    }
    p->blend_enabled = enabled != 0;
    p->rt_blend_on[0] = enabled != 0;
    bool in_shader = false;
    encodeBlend(enabled, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask, p->blend_equation,
                p->blend_reads_dest, p->blend_refusal, &p->shader_blend.rt[0], &in_shader);
    p->shader_blend.mask = (p->shader_blend.mask & ~1u) | (in_shader ? 1u : 0u);
    rawBlend(p, 0);
}

int
hakios_metal_program_set_blend_at(hakios_metal_program *p, unsigned index, int enabled, unsigned rgb_op, unsigned alpha_op,
    unsigned src_rgb, unsigned dst_rgb, unsigned src_alpha, unsigned dst_alpha, unsigned write_mask)
{
    if (index > 7) {
        fail("a colour attachment past 7");
        return -1;
    }
    if (index == 0) {
        hakios_metal_program_set_blend(p, enabled, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask);
    } else if (p != nullptr && index <= 3) {
        bool in_shader = false;
        encodeBlend(enabled, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask,
                    p->more_blend_equation[index - 1], p->more_blend_reads_dest[index - 1], p->more_blend_refusal[index - 1],
                    &p->shader_blend.rt[index], &in_shader);
        p->shader_blend.mask = (p->shader_blend.mask & ~(1u << index)) | (in_shader ? 1u << index : 0u);
        p->rt_blend_on[index] = enabled != 0;
        rawBlend(p, index);
    }
    return 0;
}

int
hakios_metal_program_set_samples(hakios_metal_program *p, unsigned samples)
{
    if (p == nullptr || (samples != 1 && samples != 2 && samples != 4)) {
        fail("a sample count of %u", samples);
        return -1;
    }
    p->samples = samples == 2 ? 4 : samples;    // (11-24: 2 served as 4, as the textures are)
    return 0;
}
}  // extern "C"
