// arm64run: maps a thin (or fat) arm64 Mach-O at its link addresses and applies chained fixups. Imports are bound
// to thunk_for(symbol). Only what stage 0 needs: LC_SEGMENT_64, LC_MAIN, LC_DYLD_CHAINED_FIXUPS (pointer formats
// DYLD_CHAINED_PTR_64 / _64_OFFSET, import formats 1-3).
#include "arm64run.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace a64r {

static void die(const char *m, const char *p) { fprintf(stderr, "arm64run: %s: %s\n", p, m); exit(127); }
template <typename T> static T get(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

Image load_macho(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open", path);
    struct stat st; fstat(fd, &st);
    auto *file = (const uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    const uint8_t *mh = file;
    if (get<uint32_t>(file) == 0xbebafecau) {  // FAT_MAGIC (big endian): pick the arm64 slice
        uint32_t n = __builtin_bswap32(get<uint32_t>(file + 4));
        mh = nullptr;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = file + 8 + i * 20;
            if (__builtin_bswap32(get<uint32_t>(a)) == 0x0100000c) mh = file + __builtin_bswap32(get<uint32_t>(a + 8));
        }
        if (!mh) die("no arm64 slice", path);
    }
    if (get<uint32_t>(mh) != 0xfeedfacfu || get<uint32_t>(mh + 4) != 0x0100000c) die("not an arm64 Mach-O", path);
    if ((get<uint32_t>(mh + 8) & 0xff) == 2) die("arm64e is not supported", path);
    uint32_t ncmds = get<uint32_t>(mh + 16);
    Image img; img.path = path;
    struct Seg { uint64_t vm, vmsize, fileoff, filesize; };
    std::vector<Seg> segs;
    uint64_t entryoff = 0; uint32_t fix_off = 0, fix_size = 0;
    const uint8_t *lc = mh + 32;
    for (uint32_t i = 0; i < ncmds; i++, lc += get<uint32_t>(lc + 4)) {
        uint32_t cmd = get<uint32_t>(lc);
        if (cmd == 0x19) {  // LC_SEGMENT_64
            Seg s{get<uint64_t>(lc + 24), get<uint64_t>(lc + 32), get<uint64_t>(lc + 40), get<uint64_t>(lc + 48)};
            if (get<uint32_t>(lc + 60) == 0 && s.filesize == 0) continue;  // __PAGEZERO
            segs.push_back(s);
            if (s.fileoff == 0 && s.filesize && !img.base) img.base = s.vm;
        } else if (cmd == 0x80000028) entryoff = get<uint64_t>(lc + 8);  // LC_MAIN
        else if (cmd == 0x80000034) { fix_off = get<uint32_t>(lc + 8); fix_size = get<uint32_t>(lc + 12); }
        else if (cmd == 0x22 || cmd == 0x80000022) die("classic dyld info (link with chained fixups)", path);
    }
    // Identity mapping: each segment at its own vmaddr (16K-aligned segments are fine on 4K host pages).
    for (auto &s : segs) {
        void *p = mmap((void *)s.vm, s.vmsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)s.vm) die("segment address is taken", path);
        memcpy(p, mh + s.fileoff, s.filesize);
    }
    if (fix_size) {
        const uint8_t *fx = mh + fix_off;
        uint32_t starts = get<uint32_t>(fx + 4), imports = get<uint32_t>(fx + 8), syms = get<uint32_t>(fx + 12);
        uint32_t nimp = get<uint32_t>(fx + 16), ifmt = get<uint32_t>(fx + 20);
        std::vector<uint64_t> targets(nimp);
        for (uint32_t i = 0; i < nimp; i++) {
            uint32_t nameoff; int64_t addend = 0;
            if (ifmt == 1) nameoff = get<uint32_t>(fx + imports + 4 * i) >> 9;
            else if (ifmt == 2) { nameoff = get<uint32_t>(fx + imports + 8 * i) >> 9; addend = get<int32_t>(fx + imports + 8 * i + 4); }
            else { nameoff = (uint32_t)(get<uint64_t>(fx + imports + 16 * i) >> 32); addend = get<int64_t>(fx + imports + 16 * i + 8); }
            targets[i] = thunk_for((const char *)fx + syms + nameoff) + addend;
        }
        const uint8_t *si = fx + starts;
        uint32_t nseg = get<uint32_t>(si);
        for (uint32_t s = 0; s < nseg; s++) {
            uint32_t off = get<uint32_t>(si + 4 + 4 * s);
            if (!off) continue;
            const uint8_t *ss = si + off;
            uint16_t page_size = get<uint16_t>(ss + 4), fmt = get<uint16_t>(ss + 6), npages = get<uint16_t>(ss + 20);
            uint64_t segoff = get<uint64_t>(ss + 8);
            if (fmt != 2 && fmt != 6) die("unsupported chained pointer format", path);
            for (uint32_t pg = 0; pg < npages; pg++) {
                uint16_t ps = get<uint16_t>(ss + 22 + 2 * pg);
                if (ps == 0xffff) continue;
                uint64_t a = img.base + segoff + (uint64_t)pg * page_size + ps;
                for (;;) {
                    uint64_t v = *(uint64_t *)a, next = (v >> 51) & 0xfff, out;
                    if (v >> 63) out = targets[v & 0xffffff] + ((v >> 24) & 0xff);
                    else { out = (v & 0xfffffffffull) | (((v >> 36) & 0xff) << 56); if (fmt == 6) out += img.base; }
                    *(uint64_t *)a = out;
                    if (!next) break;
                    a += next * 4;
                }
            }
        }
    }
    img.entry = img.base + entryoff;
    munmap((void *)file, st.st_size);
    return img;
}

}  // namespace a64r
