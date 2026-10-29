// libarm64jit: the Mach-O structures we read and write (subset of <mach-o/loader.h>, which Linux doesn't have).
#pragma once
#include <cstdint>

namespace a64 {
constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf, MH_EXECUTE = 2, MH_DYLIB = 6, MH_DYLINKER = 7;
constexpr int32_t CPU_TYPE_ARM64 = 0x0100000c;
constexpr uint32_t MH_NOUNDEFS = 0x1, MH_DYLDLINK = 0x4, MH_TWOLEVEL = 0x80, MH_PIE = 0x200000, MH_NO_REEXPORTED_DYLIBS = 0x100000;
constexpr uint32_t LC_SEGMENT_64 = 0x19, LC_SYMTAB = 0x2, LC_DYSYMTAB = 0xb, LC_UNIXTHREAD = 0x5, LC_ID_DYLIB = 0xd,
                   LC_UUID = 0x1b, LC_BUILD_VERSION = 0x32, LC_MAIN = 0x80000028, LC_DYLD_EXPORTS_TRIE = 0x80000033;
constexpr uint32_t S_REGULAR = 0, S_ATTR_PURE_INSTRUCTIONS = 0x80000000, S_ATTR_SOME_INSTRUCTIONS = 0x400;
constexpr int VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE = 2;

struct mach_header_64 { uint32_t magic; int32_t cputype, cpusubtype; uint32_t filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
    uint32_t cmd, cmdsize; char segname[16]; uint64_t vmaddr, vmsize, fileoff, filesize; int32_t maxprot, initprot;
    uint32_t nsects, flags;
};
struct section_64 {
    char sectname[16], segname[16]; uint64_t addr, size; uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct dylib { uint32_t name_offset, timestamp, current_version, compatibility_version; };
struct dylib_command { uint32_t cmd, cmdsize; dylib dylib; };
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; };
struct symtab_command { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct dysymtab_command { uint32_t cmd, cmdsize, v[18]; };
struct uuid_command { uint32_t cmd, cmdsize; uint8_t uuid[16]; };
struct build_version_command { uint32_t cmd, cmdsize, platform, minos, sdk, ntools; };
struct entry_point_command { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; };
}  // namespace a64
