// mali_textures.cpp (split from c_api_mali.cpp, 11-24)
#include "mali_internal.h"

using namespace mali;

extern "C" {
extern "C++" { namespace mali {
// BOOT-ARG metal_stats: a command buffer that took 40 ms or more from its commit to its completion, said pass by pass
// (each Mali pass: target size, draws, whether it preloads, its first program; the copies and dispatches between)
// (boot-arg metal_glitchsay: every target a finished command buffer drew into, sampled a row in 16: rows mostly pure
// blue or cyan -- the tester's video of a launch, 11-09 -- said with the command buffer's draws, once each up to 30)
void
glitchScan(hakios_metal_cb *cb, bool before)
{
    static const bool on = bootArg("metal_glitchsay");
    static std::atomic<unsigned> said{0}, mapped{0};
    if (!on || said >= 30) return;
    std::vector<hakios_metal_texture *> targets;
    for (const DrawJob &dj : cb->draws) {
        auto add = [&](hakios_metal_texture *t) {
            if (t != nullptr && t->bytes == 4 && t->samples == 1 && std::find(targets.begin(), targets.end(), t) == targets.end())
                targets.push_back(t);
        };
        add(dj.target_tex);
        for (unsigned i = 1; i < dj.rts; i++) if (!dj.more[i - 1].missing) add(dj.more[i - 1].texture);
    }
    std::string bad;
    std::vector<uint32_t> row;
    for (hakios_metal_texture *t : targets) {
        bool bgra = t->format == 80 || t->format == 81;
        unsigned rows_bad = 0, first = 0;
        row.resize(t->width);
        static const bool flush = bootArg("metal_glitchflush");    // (a test: the CPU's cached lines of it dropped first)
        for (unsigned y = 0; y < t->height; y += 16) {
            if (flush) sys_dcache_flush(t->pixels() + (uint64_t)y * t->stride, t->width * 4);
            if (hakios_metal_texture_read(t, 0, y, t->width, 1, row.data(), t->width * 4) != 0) break;
            unsigned n = 0;
            for (unsigned x = 0; x < t->width; x++) {
                uint32_t v = row[x];
                unsigned r = bgra ? (v >> 16) & 0xff : v & 0xff, g = (v >> 8) & 0xff, b = bgra ? v & 0xff : (v >> 16) & 0xff;
                n += (b == 255 && g < 8 && r < 8) || (b == 255 && g == 255 && r < 100);
            }
            if (n * 10 > t->width * 6) { if (rows_bad++ == 0) first = y; }
        }
        if (rows_bad != 0) {
            char line[160];
            snprintf(line, sizeof(line), " [target %p %ux%u format %u: %u rows blue/cyan from y %u]", (void *)t, t->width,
                t->height, t->format, rows_bad, first);
            bad += line;
            if (mapped++ < 2) {
                // a coarse map of it: k black, w white, b blue, c cyan, r red, g green, y yellow, m magenta, . other
                for (unsigned gy = 0; gy < 30; gy++) {
                    char cells[64];
                    unsigned y = gy * t->height / 30, n = 0;
                    if (hakios_metal_texture_read(t, 0, y, t->width, 1, row.data(), t->width * 4) != 0) break;
                    for (unsigned gx = 0; gx < 27; gx++) {
                        uint32_t v = row[gx * t->width / 27];
                        unsigned r = bgra ? (v >> 16) & 0xff : v & 0xff, g = (v >> 8) & 0xff, b = bgra ? v & 0xff : (v >> 16) & 0xff;
                        auto hi = [](unsigned c) { return c > 200; };
                        auto lo = [](unsigned c) { return c < 60; };
                        char ch = lo(r) && lo(g) && lo(b) ? 'k' : hi(r) && hi(g) && hi(b) ? 'w' : lo(r) && lo(g) && hi(b) ? 'b' :
                            lo(r) && hi(g) && hi(b) ? 'c' : hi(r) && lo(g) && lo(b) ? 'r' : lo(r) && hi(g) && lo(b) ? 'g' :
                            hi(r) && hi(g) && lo(b) ? 'y' : hi(r) && lo(g) && hi(b) ? 'm' : '.';
                        cells[n++] = ch;
                    }
                    cells[n] = 0;
                    fprintf(stderr, "hakios-metal: glitch map %p %s %2u %s\n", (void *)t, before ? "before" : "after", gy, cells);
                }
            }
        }
    }
    if (bad.empty()) return;
    said++;
    fprintf(stderr, "hakios-metal: glitch: a command buffer %s blue/cyan rows in%s; its draws:\n",
        before ? "FOUND, before it ran," : "left", bad.c_str());
    for (size_t i = 0; i < cb->draws.size(); i++) {
        const DrawJob &dj = cb->draws[i];
        std::string tex;
        for (unsigned k = 0; k < dj.texture_count; k++) {
            char one[64];
            hakios_metal_texture *x = dj.textures[k];
            snprintf(one, sizeof(one), " %ux%u/%u", x ? x->width : 0, x ? x->height : 0, x ? x->format : 0);
            tex += one;
        }
        fprintf(stderr, "hakios-metal: glitch:  %zu %s into %p %ux%u (rts %u, load %d, clear_only %d, zs %d), textures%s\n", i,
            dj.name, (void *)dj.target_tex, dj.width, dj.height, dj.rts, dj.load, dj.clear_only, dj.zs, tex.c_str());
    }
}

unsigned
gwBlueRows(hakios_metal_texture *t, unsigned step, unsigned *first)
{
    bool bgra = t->format == 80 || t->format == 81;
    std::vector<uint32_t> row(t->width);
    unsigned bad = 0;
    for (unsigned y = 0; y < t->height; y += step) {
        if (hakios_metal_texture_read(t, 0, y, t->width, 1, row.data(), t->width * 4) != 0) break;
        unsigned n = 0;
        for (unsigned x = 0; x < t->width; x++) {
            uint32_t v = row[x];
            unsigned r = bgra ? (v >> 16) & 0xff : v & 0xff, g = (v >> 8) & 0xff, b = bgra ? v & 0xff : (v >> 16) & 0xff;
            n += (b == 255 && g < 8 && r < 8) || (b == 255 && g == 255 && r < 100);
        }
        if (n * 10 > t->width * 6 && bad++ == 0 && first) *first = y;
    }
    return bad;
}

// a coarse colour map of T, 27x30 cells: k black, w white, b blue, c cyan, r red, g green, y yellow, m magenta, . other
void
gwMap(hakios_metal_texture *t, const char *what)
{
    bool bgra = t->format == 80 || t->format == 81;
    std::vector<uint32_t> row(t->width);
    for (unsigned gy = 0; gy < 30; gy++) {
        char cells[64];
        unsigned y = gy * t->height / 30, n = 0;
        if (hakios_metal_texture_read(t, 0, y, t->width, 1, row.data(), t->width * 4) != 0) break;
        for (unsigned gx = 0; gx < 27; gx++) {
            uint32_t v = row[gx * t->width / 27];
            unsigned r = bgra ? (v >> 16) & 0xff : v & 0xff, g = (v >> 8) & 0xff, b = bgra ? v & 0xff : (v >> 16) & 0xff;
            auto hi = [](unsigned c) { return c > 200; };
            auto lo = [](unsigned c) { return c < 60; };
            cells[n++] = lo(r) && lo(g) && lo(b) ? 'k' : hi(r) && hi(g) && hi(b) ? 'w' : lo(r) && lo(g) && hi(b) ? 'b' :
                lo(r) && hi(g) && hi(b) ? 'c' : hi(r) && lo(g) && lo(b) ? 'r' : lo(r) && hi(g) && lo(b) ? 'g' :
                hi(r) && hi(g) && lo(b) ? 'y' : hi(r) && lo(g) && hi(b) ? 'm' : '.';
        }
        cells[n] = 0;
        uint32_t mid = row[t->width / 2];
        fprintf(stderr, "hakios-metal: glitchwatch: map %s %p %2u %s  (middle %08x)\n", what, (void *)t, gy, cells, mid);
    }
}

// a command buffer's jobs as lines (consecutive draws alike said once, with how many), marking what lands in T
std::vector<std::string>
gwDescribe(hakios_metal_cb *cb, hakios_metal_texture *t)
{
    std::vector<std::string> out;
    if (cb == nullptr) return out;
    uint64_t lo = t->gpu(), hi = lo + t->span();
    static const char *kinds[] = { "copy_u32", "fill_u32", "copy_u8", "fill_u8", "dispatch", "draw", "copy_rows" };
    std::string last;
    size_t last_at = 0, alike = 0;
    char text[700];
    auto flush = [&] {
        if (!last.empty()) {
            snprintf(text, sizeof(text), "job %zu%s draw %s", last_at,
                alike > 1 ? (" (x" + std::to_string(alike) + ")").c_str() : "", last.c_str());
            out.push_back(text);
        }
        last.clear();
        alike = 0;
    };
    for (size_t i = 0; i < cb->blits.size(); i++) {
        const Job &b = cb->blits[i];
        if (b.shader == DRAW) {
            if (b.dst >= cb->draws.size()) continue;
            const DrawJob &dj = cb->draws[b.dst];
            std::string more, tex;
            for (unsigned k = 1; k < dj.rts; k++) {
                const auto &m = dj.more[k - 1];
                char one[128];
                snprintf(one, sizeof(one), " rt%u %ux%u/%u%s%s%s blend %08x", k, m.texture ? m.texture->width : 0,
                    m.texture ? m.texture->height : 0, m.texture ? m.texture->format : 0, m.missing ? " missing" : "",
                    m.write ? " w" : "", m.va >= lo && m.va < hi ? " IN-IT" : "", m.blend[0]);
                more += one;
            }
            for (unsigned k = 0; k < dj.texture_count; k++) {
                char one[48];
                hakios_metal_texture *x = dj.textures[k];
                if (x == nullptr) continue;
                snprintf(one, sizeof(one), " %u:%ux%u/%u%s", k, x->width, x->height, x->format, x == t ? "(IT)" : "");
                tex += one;
            }
            char dtext[700];
            snprintf(dtext, sizeof(dtext), "%s (%p) into %ux%u%s load %d write %d blend %08x %08x count %u inst %u scissor %08x-%08x%s "
                "textures%s", dj.name, dj.program, dj.width, dj.height, dj.target_va >= lo && dj.target_va < hi ? " (IT)" : "", dj.load,
                dj.write, dj.blend[0], dj.blend[1], dj.count, dj.instances, dj.scissor_min, dj.scissor_max, more.c_str(),
                tex.c_str());
            if (last == dtext) { alike++; continue; }
            flush();
            last = dtext;
            last_at = i;
            alike = 1;
            continue;
        }
        flush();
        if (b.shader == DISPATCH) {
            bool in = false;
            for (unsigned k = 0; k < MALI_KERNEL_BUFFERS; k++) in |= b.buffers[k] >= lo && b.buffers[k] < hi;
            for (unsigned k = 0; k < b.texture_count; k++) in |= b.textures[k] == t;
            snprintf(text, sizeof(text), "job %zu dispatch %ux%ux%u%s", i, b.groups[0], b.groups[1], b.groups[2],
                in ? " (USES IT)" : "");
        } else {
            unsigned unit = b.shader <= 1 ? 4 : 1;
            bool in = b.dst < hi && b.dst + (uint64_t)b.count * unit > lo;
            snprintf(text, sizeof(text), "job %zu %s dst 0x%llx count %u value %08x src 0x%llx%s", i,
                kinds[b.shader < 7 ? b.shader : 5], (unsigned long long)b.dst, b.count, b.value,
                (unsigned long long)b.src, in ? " (INTO IT)" : "");
        }
        out.push_back(text);
    }
    flush();
    return out;
}

void
glitchWatch(const char *event, hakios_metal_cb *cb)
{
    static const bool on = bootArg("metal_glitchwatch");
    static std::atomic<unsigned> said{0};
    if (!on) return;
    std::lock_guard<std::mutex> g(gwLock);
    if (cb != nullptr) {
        // screen targets: drawn into by 20 command buffers or more (the screen's drawables, not the launch snapshots);
        // the six most recently drawn kept
        static std::map<hakios_metal_texture *, unsigned> seen;
        std::vector<hakios_metal_texture *> mine;
        for (const DrawJob &dj : cb->draws) {
            hakios_metal_texture *t = dj.target_tex;
            if (t == nullptr || t->bytes != 4 || t->samples != 1 || t->width < 1000 || t->height < 2000) continue;
            if (std::find(mine.begin(), mine.end(), t) == mine.end()) mine.push_back(t);
        }
        for (hakios_metal_texture *t : mine) {
            auto at = std::find_if(gwTargets.begin(), gwTargets.end(), [&](auto &w) { return w.first == t; });
            if (at != gwTargets.end()) {
                auto keep = *at;                    // most recently drawn last
                gwTargets.erase(at);
                gwTargets.push_back(keep);
                continue;
            }
            if (++seen[t] < 20) continue;
            if (seen.size() > 256) seen.clear();
            t->retain();
            gwTargets.push_back({ t, false });
            fprintf(stderr, "hakios-metal: glitchwatch: watching target %p %ux%u format %u (command buffer #%u)\n", (void *)t,
                t->width, t->height, t->format, cb->seq);
            if (gwTargets.size() > 6) {
                gwTargets.front().first->release();
                gwTargets.erase(gwTargets.begin());
            }
        }
    }
    // this event in a line: the command buffer, the targets it draws into (with its draw count into each), and each
    // watched target's sampled blue rows now; the last 16 lines said when a target changes, and the 8 after it
    static std::deque<std::string> history;
    static unsigned after = 0;
    std::string line;
    char one[160];
    snprintf(one, sizeof(one), "%s #%u (in flight %u, %ld us):", event, cb ? cb->seq : 0, gwInFlight.load(),
        (long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000);
    line = one;
    if (cb != nullptr) {
        std::vector<std::pair<hakios_metal_texture *, unsigned>> into;
        for (const DrawJob &dj : cb->draws) {
            auto at = std::find_if(into.begin(), into.end(), [&](auto &x) { return x.first == dj.target_tex; });
            if (at == into.end()) into.push_back({ dj.target_tex, 1 }); else at->second++;
        }
        for (auto &x : into) {
            snprintf(one, sizeof(one), " %p%s*%u", (void *)x.first,
                x.first && x.first->width >= 1000 ? "(screen)" : "", x.second);
            line += one;
        }
        if (cb->blits.size() != cb->draws.size()) {
            snprintf(one, sizeof(one), " +%zu copies/dispatches", cb->blits.size() - cb->draws.size());
            line += one;
        }
    }
    line += " | watched:";
    bool changed = false;
    std::vector<std::pair<hakios_metal_texture *, unsigned>> turned;
    static std::map<hakios_metal_texture *, std::vector<std::string>> before;  // the last clean frame's jobs
    for (auto &w : gwTargets) {
        unsigned first = 0, bad = gwBlueRows(w.first, 64, &first);
        bool blue = bad * 64 >= 512;     // 8 sampled rows (about a fifth of the screen) or more
        snprintf(one, sizeof(one), " %p=%u", (void *)w.first, bad);
        line += one;
        bool draws_it = false;
        if (cb != nullptr && !strcmp(event, "completion")) {
            for (const DrawJob &dj : cb->draws) draws_it |= dj.target_tex == w.first;
        }
        if (blue == w.second) {
            if (draws_it && !blue && said < 40) before[w.first] = gwDescribe(cb, w.first);
            continue;
        }
        w.second = blue;
        changed = true;
        if (blue) turned.push_back({ w.first, first });
    }
    history.push_back(line);
    if (history.size() > 16) history.pop_front();
    if (said >= 40) return;
    if (changed) {
        said++;
        fprintf(stderr, "hakios-metal: glitchwatch: a watched target changed (sampled blue/cyan rows of 37 per target); "
            "the events up to it:\n");
        for (auto &h : history) fprintf(stderr, "hakios-metal: glitchwatch:   %s\n", h.c_str());
        for (auto &x : turned) {
            fprintf(stderr, "hakios-metal: glitchwatch: target %p TURNED BLUE/CYAN (from y %u) at the %s of command buffer #%u; "
                "its jobs:\n", (void *)x.first, x.second, event, cb ? cb->seq : 0);
            for (auto &l : gwDescribe(cb, x.first)) fprintf(stderr, "hakios-metal: glitchwatch:  now %s\n", l.c_str());
            for (auto &l : before[x.first]) fprintf(stderr, "hakios-metal: glitchwatch:  last clean frame %s\n", l.c_str());
            // the large textures its draws sample, as the CPU reads them now: blue there too (the data), or not (the
            // GPU's reading of it)
            std::vector<hakios_metal_texture *> sampled;
            if (cb != nullptr) {
                for (const DrawJob &dj : cb->draws) {
                    for (unsigned k = 0; k < dj.texture_count; k++) {
                        hakios_metal_texture *s2 = dj.textures[k];
                        if (s2 && s2->bytes == 4 && s2->width >= 500 && s2->height >= 500 && s2->levels == 1 &&
                            std::find(sampled.begin(), sampled.end(), s2) == sampled.end()) sampled.push_back(s2);
                    }
                }
            }
            for (hakios_metal_texture *s2 : sampled) {
                unsigned f2 = 0, b2 = gwBlueRows(s2, 64, &f2);
                fprintf(stderr, "hakios-metal: glitchwatch: sampled texture %p %ux%u format %u origin %u surface %u: %u sampled rows "
                    "blue/cyan\n", (void *)s2, s2->width, s2->height, s2->format, s2->origin, s2->surface, b2);
                if (said <= 3) gwMap(s2, "sampled");
            }
            if (said <= 3) gwMap(x.first, "target");
        }
        after = 8;
    } else if (after != 0) {
        after--;
        fprintf(stderr, "hakios-metal: glitchwatch:   then %s\n", line.c_str());
    }
}

void
glitchWatchCommit(hakios_metal_cb *cb)
{
    static const bool on = bootArg("metal_glitchwatch"), sync = bootArg("metal_glitchsync");
    if (!on) return;
    cb->seq = ++gwSeq;
    if (sync) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (gwInFlight.load() != 0 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    glitchWatch("commit", cb);
    gwInFlight++;
}

void
glitchWatchDone()
{
    static const bool on = bootArg("metal_glitchwatch");
    if (on) gwInFlight--;
}

void
passStats(hakios_metal_cb *cb)
{
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cb->committed_at).count();
    // metal_stats_ms=N (11-10): the threshold, 40 ms by default -- Spotlight's opening has buffers of 23-28 ms; at most
    // 300 said
    static double limit = -1;
    static std::atomic<unsigned> said{0};
    if (limit < 0) {
        char args[1024] = {0};
        size_t size = sizeof(args) - 1;
        const char *at;
        limit = 40;
        if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && (at = strstr(args, "metal_stats_ms=")) != nullptr) {
            limit = atof(at + 15);
        } else if ((at = strstr(args, "mdsay=")) != nullptr) {
            limit = atof(at + 6);                       // (mdsay=N: the threshold, and each slow pass's draws -- short:
        }                                               //  the command line holds 490 characters)
    }
    if (ms < limit || said >= 300) {
        return;
    }
    said++;
    double gpu_sum = 0;
    unsigned passes = 0, draws = 0, loads = 0, other = 0;
    uint64_t pixels = 0;
    std::string list;
    for (size_t i = 0; i < cb->blits.size(); i++) {
        const Job &b = cb->blits[i];
        if (b.shader != DRAW) {
            other++;
            continue;
        }
        const DrawJob &dj = cb->draws[b.dst];
        size_t k = i;
        unsigned n = 1;
        while (k + 1 < cb->blits.size() && cb->blits[k + 1].shader == DRAW && samePass(cb->draws[cb->blits[k].dst],
                                                                                         cb->draws[cb->blits[k + 1].dst])) {
            k++;
            n++;
        }
        // why the next draw (into the same target) opened a pass of its own
        const char *why = "";
        if (k + 1 < cb->blits.size() && cb->blits[k + 1].shader == DRAW) {
            const DrawJob &a = cb->draws[cb->blits[k].dst], &c = cb->draws[cb->blits[k + 1].dst];
            if (a.target_va == c.target_va) {
                why = c.clear_only || a.clear_only ? "clear" : !c.load ? "no load" : a.rts != c.rts ? "targets" :
                    a.zs != c.zs ? "zs" : "samples a target";
                for (unsigned m = 1; m < a.rts && m < c.rts; m++) {
                    if (a.more[m - 1].va != c.more[m - 1].va) why = "other target n";
                    else if (a.more[m - 1].missing != c.more[m - 1].missing) why = "missing n";
                    else if (!c.more[m - 1].missing && !c.more[m - 1].load) why = "target n not loaded";
                }
            }
        }
        unsigned touch1 = 0;                            // (draws of the pass that write or read attachment 1)
        for (size_t j = i; j <= k; j++) {
            const DrawJob &x = cb->draws[cb->blits[j].dst];
            touch1 += x.rts > 1 && !x.more[0].missing && !x.more[0].untouched;
        }
        double gpu_us = -1;
        if (cb->timestamps.size != 0 && 2 * passes + 1 < cb->timestamp_count) {
            auto *ts0 = (volatile uint64_t *)cb->timestamps.cpu;
            double us = (double)(ts0[2 * passes + 1] - ts0[2 * passes]) / 24.576;
            static const bool draws_say = bootArg("mdsay=");
            if (draws_say && us >= limit * 1000 * 0.5) {
                // (boot-arg mdsay=MS, 11-11: a slow pass's draws -- each one's area (its scissor), vertices,
                // program, the textures it samples (size, format, stride; an IOSurface's ID) and its flags)
                std::string d;
                for (size_t j = i; j <= k && d.size() < 3000; j++) {
                    const DrawJob &x = cb->draws[cb->blits[j].dst];
                    unsigned w = (x.scissor_max & 0xffff) - (x.scissor_min & 0xffff) + 1;
                    unsigned h = (x.scissor_max >> 16) - (x.scissor_min >> 16) + 1;
                    char one[400];
                    const hakios_metal_program *xp = (const hakios_metal_program *)x.program;
                    const char *lab = xp ? xp->label : "";
                    if (strncmp(lab, "com.apple.coreanimation.draw.", 29) == 0) lab += 29;
                    int at = snprintf(one, sizeof(one), " {%ux%u@%u,%u %uv %s %s fs %u dcd0 %x blend %x", w, h,
                        x.scissor_min & 0xffff, x.scissor_min >> 16, x.count * (x.instances ? x.instances : 1),
                        x.name ? x.name + (strlen(x.name) > 12 ? strlen(x.name) - 12 : 0) : "", lab, xp ? xp->fs.size : 0,
                        x.dcd0, x.blend[0]);
                    for (unsigned t = 0; t < x.texture_count && at < (int)sizeof(one) - 60; t++) {
                        const hakios_metal_texture *tx = x.textures[t];
                        if (tx) at += snprintf(one + at, sizeof(one) - at, " T%ux%u f%u s%u%s", tx->width, tx->height, tx->format,
                            tx->stride, tx->surface ? " ios" : "");
                    }
                    snprintf(one + at, sizeof(one) - at, "}");
                    d += one;
                }
                const DrawJob &h0 = cb->draws[cb->blits[i].dst];
                const hakios_metal_texture *tt = h0.target_tex;
                fprintf(stderr, "hakios-metal: slow pass %.0f us, %u draws into %ux%u f%u s%u origin %u surface %u%s%s%s at %llx:%s\n",
                    us, n, tt ? tt->width : 0, tt ? tt->height : 0, tt ? tt->format : 0, tt ? tt->stride : 0, tt ? tt->origin : 0,
                    tt ? tt->surface : 0, tt && tt->mirrored ? " mirrored" : "", h0.load ? " L" : "", h0.write ? " W" : "",
                    (unsigned long long)h0.target_va, d.c_str());
            }
        }
        if (cb->timestamps.size != 0 && 2 * passes + 1 < cb->timestamp_count) {
            auto *ts = (volatile uint64_t *)cb->timestamps.cpu;
            gpu_us = (double)(ts[2 * passes + 1] - ts[2 * passes]) / 24.576;    // (the system counter: 24.576 MHz)
            gpu_sum += gpu_us;
        }
        passes++;
        draws += n;
        loads += dj.load;
        pixels += (uint64_t)dj.width * dj.height;
        if (list.size() < 2500) {
            char one[400];
            char a1[160] = "";
            if (dj.rts > 1 && dj.more[0].texture != nullptr && !dj.more[0].missing) {
                const hakios_metal_texture *x = dj.more[0].texture;
                int at = snprintf(a1, sizeof(a1), " a1 fmt %u %ux%u: attachment %u times, READ %u @%llx", x->format, x->width,
                    x->height, x->as_extra_rt.load(), x->reads.load(), (unsigned long long)(dj.more[0].va & 0xffffffffff));
                if (dj.rts > 2 && dj.more[1].texture != nullptr && !dj.more[1].missing) {
                    const hakios_metal_texture *y = dj.more[1].texture;
                    snprintf(a1 + at, sizeof(a1) - at, " a2 %c fmt %u %ux%u @%llx", dj.more[1].memoryless ? 't' : dj.more[1].load ?
                        'l' : dj.more[1].write ? 'w' : '-', y->format, y->width, y->height,
                        (unsigned long long)(dj.more[1].va & 0xffffffffff));
                }
            }
            snprintf(one, sizeof(one), " [%.0fus %ux%u %u%s%s %s rts %u%s t%u%s%s%s]", gpu_us, dj.width, dj.height, n, dj.load ? " L" : "",
                dj.clear_only ? " C" : "", dj.name ? dj.name : "", dj.rts,
                dj.rts > 1 ? (dj.more[0].missing ? "m" : dj.more[0].memoryless ? "t" : dj.more[0].elide ? "e" : dj.more[0].load ? "l" :
                              dj.more[0].write ? "w" : "-") : "",
                touch1, a1, why[0] ? " | " : "", why);
            list += one;
        }
        i = k;
    }
    fprintf(stderr, "hakios-metal: stats: %.1f ms (the passes' GPU time %.1f ms): %u Mali passes (%u preload), %u draws, %u "
        "copies/dispatches, %.1f MP of targets:%s\n", ms, gpu_sum / 1e3, passes, loads, draws, other, pixels / 1e6, list.c_str());
}

// a word in the boot-args (testing switches: metal_nozs)
bool
bootArg(const char *word)
{
    char args[1024] = {0};
    size_t size = sizeof(args) - 1;
    return sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && strstr(args, word) != nullptr;
}
} }  // namespace mali, extern "C++"

hakios_metal_texture *
hakios_metal_texture_new(hakios_metal_device *d, unsigned type, unsigned pixel_format, unsigned width, unsigned height,
    unsigned usage)
{
    static const bool no_zs = bootArg("metal_nozs");
    if (no_zs && pixel_format >= 250 && pixel_format <= 261) {
        fail("a depth or stencil texture of MTLPixelFormat %u (boot-arg metal_nozs)", pixel_format);
        return nullptr;
    }
    const MaliFormat *f = nullptr;
    for (const MaliFormat &m : kFormats) {
        if (m.metal == pixel_format) {
            f = &m;
        }
    }
    if (f == nullptr) {
        fail("a texture of MTLPixelFormat %u (not served on the Mali yet)", pixel_format);
        return nullptr;
    }
    if ((type != 2 && !(type == 0 && height == 1)) || width == 0 || height == 0 || width > 16384 || height > 16384) {
        fail("a texture of type %u, %u x %u (the Mali build makes 2D and 1D ones up to 16384)", type, width, height);
        return nullptr;
    }
    auto *t = new hakios_metal_texture;
    t->device = d;
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
    t->stride = ((width + f->bw - 1) / f->bw * f->bytes + 63) & ~63u;
    uint64_t size = (uint64_t)t->stride * t->levelRows(0);
    if (pixel_format == 260) {
        t->stencil_off = (size + 63) & ~63ULL;
        t->stencil_stride = (width + 63) & ~63u;
        size = t->stencil_off + (uint64_t)t->stencil_stride * height;
    }
    if (!d->newBo(t->bo, size, BIND_NOEXEC)) {
        delete t;
        return nullptr;
    }
    return t;
}

void
hakios_metal_texture_free(hakios_metal_texture *t)
{
    if (t != nullptr) {
        t->release();       // (a command buffer that uses it keeps it until the GPU is done)
    }
}

void *hakios_metal_texture_contents(hakios_metal_texture *t) { return t ? t->pixels() : nullptr; }
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture *t) { return t ? t->stride : 0; }
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture *t) { return t ? t->gpu() : 0; }
unsigned hakios_metal_texture_width(hakios_metal_texture *t) { return t ? t->width : 0; }
unsigned hakios_metal_texture_height(hakios_metal_texture *t) { return t ? t->height : 0; }
unsigned hakios_metal_texture_format(hakios_metal_texture *t) { return t ? t->format : 0; }
unsigned hakios_metal_texture_type(hakios_metal_texture *t) { return t ? t->type : 0; }
unsigned hakios_metal_texture_usage(hakios_metal_texture *t) { return t ? t->usage : 0; }
extern "C++" { namespace mali {
int
copyRows(hakios_metal_texture *t, unsigned x, unsigned y, unsigned w, unsigned h, uint8_t *bytes, uint64_t bytes_per_row,
    bool in, unsigned level)
{
    if (t == nullptr || level >= t->levels || x + w > t->levelWidth(level) || y + h > t->levelHeight(level) ||
        bytes_per_row < (uint64_t)(w + t->bw - 1) / t->bw * t->bytes) {
        fail("a texture region outside the texture (level %u of %u), or rows shorter than its width", level,
            t ? t->levels : 0);
        return -1;
    }
    uint8_t *base = t->pixels() + t->levelOffset(level);
    uint32_t stride = t->levelStride(level);
    {
        // (11-13) THE TEXTURE'S OWN MEMORY: a copy that would run past it is refused with its numbers -- Weather died in
        // this memcpy as its radar map came up, at the first address after a mapping
        const uint64_t rows = (uint64_t)(y + h + t->bh - 1) / t->bh, cols = (uint64_t)(x + w + t->bw - 1) / t->bw;
        const uint64_t end = t->offset + t->levelOffset(level) + (rows ? rows - 1 : 0) * stride + cols * t->bytes;
        if (h != 0 && w != 0 && end > t->bo.size) {
            fail("a texture copy past the texture's memory: %ux%u at (%u, %u) of level %u, MTLPixelFormat %u, rows %u bytes "
                "apart from %llu + %llu: it would end at %llu of %llu bytes", w, h, x, y, level, t->format, stride,
                (unsigned long long)t->offset, (unsigned long long)t->levelOffset(level), (unsigned long long)end,
                (unsigned long long)t->bo.size);
            return -1;
        }
    }
    if (t->bw > 1 || t->bh > 1) {
        // a compressed format: the region in whole blocks (its edge blocks may be partly past the level), rows of blocks
        if (x % t->bw || y % t->bh) {
            fail("a compressed texture region not on its %ux%u blocks", t->bw, t->bh);
            return -1;
        }
        x /= t->bw;
        y /= t->bh;
        w = (w + t->bw - 1) / t->bw;
        h = (h + t->bh - 1) / t->bh;
    }
    for (unsigned row = 0; row < h; row++) {
        uint8_t *gpu = base + (uint64_t)(y + row) * stride + (uint64_t)x * t->bytes;
        uint8_t *cpu = bytes + row * bytes_per_row;
        if (in) {
            memcpy(gpu, cpu, (size_t)w * t->bytes);
        } else {
            memcpy(cpu, gpu, (size_t)w * t->bytes);
        }
    }
    return 0;
}
} }  // namespace mali, extern "C++"

int
hakios_metal_texture_replace(hakios_metal_texture *t, unsigned x, unsigned y, unsigned w, unsigned h, const void *bytes,
    uint64_t bytes_per_row)
{
    int r = copyRows(t, x, y, w, h, (uint8_t *)bytes, bytes_per_row, true);
    glitchWatch("CPU upload (texture replace)", nullptr);
    // (boot-arg metal_glitchsay: what a CPU upload of 64 rows or more brings that is mostly pure blue or cyan, with the
    // caller -- the screen surface was found blue BEFORE any command buffer drew it, 11-09)
    static const bool say = bootArg("metal_glitchsay");
    static std::atomic<unsigned> said{0};
    if (say && r == 0 && t != nullptr && t->bytes == 4 && h >= 64 && w >= 64 && said < 20) {
        bool bgra = t->format == 80 || t->format == 81;
        unsigned bad = 0, rows = 0;
        for (unsigned j = 0; j < h; j += 16, rows++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)bytes + (uint64_t)j * bytes_per_row);
            unsigned n = 0;
            for (unsigned i = 0; i < w; i++) {
                uint32_t v = row[i];
                unsigned rr = bgra ? (v >> 16) & 0xff : v & 0xff, g = (v >> 8) & 0xff, b = bgra ? v & 0xff : (v >> 16) & 0xff;
                n += (b == 255 && g < 8 && rr < 8) || (b == 255 && g == 255 && rr < 100);
            }
            bad += n * 10 > w * 6;
        }
        if (bad * 2 > rows) {
            said++;
            Dl_info info = {};
            void *caller = __builtin_return_address(0);
            dladdr(caller, &info);
            fprintf(stderr, "hakios-metal: glitch: a CPU upload of %ux%u at (%u,%u) into texture %p %ux%u format %u is blue/cyan "
                "(%u of %u rows), from %s+%#lx\n", w, h, x, y, (void *)t, t->width, t->height, t->format, bad, rows,
                info.dli_sname ? info.dli_sname : "?", (unsigned long)((uintptr_t)caller - (uintptr_t)info.dli_saddr));
        }
    }
    return r;
}

int
hakios_metal_texture_read(hakios_metal_texture *t, unsigned x, unsigned y, unsigned w, unsigned h, void *bytes,
    uint64_t bytes_per_row)
{
    { hakios_metal_texture *rt_ = (t); while (rt_ != nullptr && rt_->origin == 4) rt_ = rt_->parent; if (rt_ != nullptr) rt_->reads++; }
    return copyRows(t, x, y, w, h, (uint8_t *)bytes, bytes_per_row, false);
}

// MTLSamplerDescriptor as Mali's sampler (genxml v10): wraps, filters, normalized coordinates
hakios_metal_sampler *
hakios_metal_sampler_new(hakios_metal_device *d, const struct hakios_metal_sampler_desc *desc)
{
    static const uint32_t wraps[6] = { 9, 13, 8, 12, 11, 11 };     // Metal's address modes -> Mali's wrap modes
    (void)d;
    if (desc->s_address > 5 || desc->t_address > 5 || desc->r_address > 5) {
        fail("a sampler with an address mode past ClampToBorderColor");
        return nullptr;
    }
    auto *s = new hakios_metal_sampler;
    memset(s->words, 0, sizeof(s->words));
    s->words[0] = 1 | wraps[desc->r_address] << 8 | wraps[desc->t_address] << 12 | wraps[desc->s_address] << 16 | 1u << 21 |
        1u << 23 | (desc->normalized_coordinates ? 1u << 25 : 0) | 1u << 26 | (desc->min_filter == 0 ? 1u << 27 : 0) |
        (desc->mag_filter == 0 ? 1u << 28 : 0);
    // the levels: Metal's mip filter as the mipmap mode (NotMipmapped: none, level min_lod alone; Nearest; Linear:
    // trilinear) and its LOD clamps, 8.8 fixed point (13 bits: up to 31.99)
    auto ulod = [](float v) { return (uint32_t)(v <= 0 ? 0 : v >= 31.99f ? 0x1fff : v * 256); };
    uint32_t lo = ulod(desc->lod_min_clamp), hi = desc->mip_filter == 0 ? lo : ulod(desc->lod_max_clamp);
    s->words[0] |= (desc->mip_filter == 0 ? 1u : desc->mip_filter == 1 ? 0u : 3u) << 30;
    s->words[1] = lo | (hi < lo ? lo : hi) << 16;
    if (desc->s_address == 4 || desc->t_address == 4 || desc->r_address == 4 || desc->border_color == 0) {
        // ClampToZero, and the transparent black border: zeros (the border colour's words stay 0)
    } else if (desc->border_color == 1) {
        s->words[7] = 0x3f800000;                               // opaque black: alpha 1.0
    } else {
        s->words[4] = s->words[5] = s->words[6] = s->words[7] = 0x3f800000;    // opaque white
    }
    return s;
}

void hakios_metal_sampler_free(hakios_metal_sampler *s) { delete s; }
const uint32_t *hakios_metal_sampler_words(hakios_metal_sampler *s) { return s ? s->words : nullptr; }

int
hakios_metal_cb_dispatch2(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned groups[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count, hakios_metal_texture *const *textures,
    unsigned texture_count, hakios_metal_sampler *const *samplers, unsigned sampler_count)
{
    unsigned grid[3];

    for (int i = 0; i < 3; i++) {
        grid[i] = groups[i] * threads[i];
    }
    return dispatchGrid(cb, k, groups, threads, grid, buffers, offsets, count, textures, texture_count, samplers, sampler_count);
}

int
hakios_metal_cb_dispatch_threads2(hakios_metal_cb *cb, hakios_metal_kernel *k, const unsigned grid[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count, hakios_metal_texture *const *textures,
    unsigned texture_count, hakios_metal_sampler *const *samplers, unsigned sampler_count)
{
    unsigned groups[3];

    for (int i = 0; i < 3; i++) {
        groups[i] = threads[i] ? (grid[i] + threads[i] - 1) / threads[i] : 0;
    }
    return dispatchGrid(cb, k, groups, threads, grid, buffers, offsets, count, textures, texture_count, samplers, sampler_count);
}
}  // extern "C"
