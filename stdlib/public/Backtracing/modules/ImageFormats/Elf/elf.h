//===--- elf.h - Definitions of ELF structures for import into Swift ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Definitions of ELF structures for import into Swift code
//
// The types here are taken from the System V ABI update, here:
// <http://www.sco.com/developers/gabi/2012-12-31/contents.html>
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ELF_H
#define SWIFT_ELF_H

#include <inttypes.h>

/* .. Useful macros ......................................................... */

#define ELF_ENUM(t,n)   \
  enum __attribute__((enum_extensibility(open))) n: t
#define ELF_OPTIONS(t,n) \
  t n; \
  enum __attribute__((flag_enum,enum_extensibility(open))): t

/* .. Data Representation ................................................... */

// Common sizes (these don't change between 32-bit and 64-bit)
typedef uint8_t  Elf_Byte;
typedef uint16_t Elf_Half;
typedef uint32_t Elf_Word;
typedef uint64_t Elf_Xword;
typedef int32_t  Elf_Sword;
typedef int64_t  Elf_Sxword;

// 32-bit sizes (includes some aliases of the above, for compatibility)
typedef Elf_Byte  Elf32_Byte;
typedef uint32_t  Elf32_Addr;
typedef Elf_Half  Elf32_Half;
typedef uint32_t  Elf32_Off;
typedef Elf_Sword Elf32_Sword;
typedef Elf_Word  Elf32_Word;

// 64-bit sizes (includes some aliases of the above, for compatibility)
typedef Elf_Byte   Elf64_Byte;
typedef uint64_t   Elf64_Addr;
typedef uint64_t   Elf64_Off;
typedef Elf_Half   Elf64_Half;
typedef Elf_Word   Elf64_Word;
typedef Elf_Sword  Elf64_Sword;
typedef Elf_Xword  Elf64_Xword;
typedef Elf_Sxword Elf64_Sxword;

/* .. Constants ............................................................. */

// e_type values
typedef ELF_ENUM(Elf_Half, Elf_Ehdr_Type) {
  ET_NONE   = 0,      // No file type
  ET_REL    = 1,      // Relocatable file
  ET_EXEC   = 2,      // Executable file
  ET_DYN    = 3,      // Shared object file
  ET_CORE   = 4,      // Core file
  ET_LOOS   = 0xfe00, // Operating system specific
  ET_HIOS   = 0xfeff, // Operating system specific
  ET_LOPROC = 0xff00, // Processor specific
  ET_HIPROC = 0xffff, // Processor specific
} Elf_Ehdr_Type;

// e_machine values
typedef ELF_ENUM(Elf_Half, Elf_Ehdr_Machine) {
  EM_NONE          = 0,   // No machine
  EM_M32           = 1,   // AT&T WE 32100
  EM_SPARC         = 2,   // SPARC
  EM_386           = 3,   // Intel 80386
  EM_68K           = 4,   // Motorola 68000
  EM_88K           = 5,   // Motorola 88000

  EM_860           = 7,   // Intel 80860
  EM_MIPS          = 8,   // MIPS I Architecture
  EM_S370          = 9,   // IBM System/370 Processor
  EM_MIPS_RS3_LE   = 10,  // MIPS RS3000 Little-endian

  EM_PARISC        = 15,  // Hewlett-Packard PA-RISC

  EM_VPP500        = 17,  // Fujitsu VPP500
  EM_SPARC32PLUS   = 18,  // Enhanced instruction set SPARC
  EM_960           = 19,  // Intel 80960
  EM_PPC           = 20,  // PowerPC
  EM_PPC64         = 21,  // 64-bit PowerPC
  EM_S390          = 22,  // IBM System/390 Processor
  EM_SPU           = 23,  // IBM SPU/SPC

  EM_V800          = 36,  // NEC V800
  EM_FR20          = 37,  // Fujitsu FR20
  EM_RH32          = 38,  // TRW RH-32
  EM_RCE           = 39,  // Motorola RCE
  EM_ARM           = 40,  // ARM 32-bit architecture (AARCH32)
  EM_ALPHA         = 41,  // Digital Alpha
  EM_SH            = 42,  // Hitachi SH
  EM_SPARCV9       = 43,  // SPARC Version 9
  EM_TRICORE       = 44,  // Siemens TriCore embedded processor
  EM_ARC           = 45,  // Argonaut RISC Core, Argonaut Technologies Inc.
  EM_H8_300        = 46,  // Hitachi H8/300
  EM_H8_300H       = 47,  // Hitachi H8/300H
  EM_H8S           = 48,  // Hitachi H8S
  EM_H8_500        = 49,  // Hitachi H8/500
  EM_IA_64         = 50,  // Intel IA-64 processor architecture
  EM_MIPS_X        = 51,  // Stanford MIPS-X
  EM_COLDFIRE      = 52,  // Motorola ColdFire
  EM_68HC12        = 53,  // Motorola M68HC12
  EM_MMA           = 54,  // Fujitsu MMA Multimedia Accelerator
  EM_PCP           = 55,  // Siemens PCP
  EM_NCPU          = 56,  // Sony nCPU embedded RISC processor
  EM_NDR1          = 57,  // Denso NDR1 microprocessor
  EM_STARCORE      = 58,  // Motorola Star*Core processor
  EM_ME16          = 59,  // Toyota ME16 processor
  EM_ST100         = 60,  // STMicroelectronics ST100 processor
  EM_TINYJ         = 61,  // Advanced Logic Corp. TinyJ embedded processor family
  EM_X86_64        = 62,  // AMD x86-64 architecture
  EM_PDSP          = 63,  // Sony DSP Processor
  EM_PDP10         = 64,  // Digital Equipment Corp. PDP-10
  EM_PDP11         = 65,  // Digital Equipment Corp. PDP-11
  EM_FX66          = 66,  // Siemens FX66 microcontroller
  EM_ST9PLUS       = 67,  // STMicroelectronics ST9+ 8/16 bit microcontroller
  EM_ST7           = 68,  // STMicroelectronics ST7 8-bit microcontroller
  EM_68HC16        = 69,  // Motorola MC68HC16 Microcontroller
  EM_68HC11        = 70,  // Motorola MC68HC11 Microcontroller
  EM_68HC08        = 71,  // Motorola MC68HC08 Microcontroller
  EM_68HC05        = 72,  // Motorola MC68HC05 Microcontroller
  EM_SVX           = 73,  // Silicon Graphics SVx
  EM_ST19          = 74,  // STMicroelectronics ST19 8-bit microcontroller
  EM_VAX           = 75,  // Digital VAX
  EM_CRIS          = 76,  // Axis Communications 32-bit embedded processor
  EM_JAVELIN       = 77,  // Infineon Technologies 32-bit embedded processor
  EM_FIREPATH      = 78,  // Element 14 64-bit DSP Processor
  EM_ZSP           = 79,  // LSI Logic 16-bit DSP Processor
  EM_MMIX          = 80,  // Donald Knuth's educational 64-bit processor
  EM_HUANY         = 81,  // Harvard University machine-independent object files
  EM_PRISM         = 82,  // SiTera Prism
  EM_AVR           = 83,  // Atmel AVR 8-bit microcontroller
  EM_FR30          = 84,  // Fujitsu FR30
  EM_D10V          = 85,  // Mitsubishi D10V
  EM_D30V          = 86,  // Mitsubishi D30V
  EM_V850          = 87,  // NEC v850
  EM_M32R          = 88,  // Mitsubishi M32R
  EM_MN10300       = 89,  // Matsushita MN10300
  EM_MN10200       = 90,  // Matsushita MN10200
  EM_PJ            = 91,  // picoJava
  EM_OPENRISC      = 92,  // OpenRISC 32-bit embedded processor
  EM_ARC_COMPACT   = 93,  // ARC International ARCompact processor (old spelling/synonym: EM_ARC_A5)
  EM_XTENSA        = 94,  // Tensilica Xtensa Architecture
  EM_VIDEOCORE     = 95,  // Alphamosaic VideoCore processor
  EM_TMM_GPP       = 96,  // Thompson Multimedia General Purpose Processor
  EM_NS32K         = 97,  // National Semiconductor 32000 series
  EM_TPC           = 98,  // Tenor Network TPC processor
  EM_SNP1K         = 99,  // Trebia SNP 1000 processor
  EM_ST200         = 100, // STMicroelectronics (www.st.com) ST200 microcontroller
  EM_IP2K          = 101, // Ubicom IP2xxx microcontroller family
  EM_MAX           = 102, // MAX Processor
  EM_CR            = 103, // National Semiconductor CompactRISC microprocessor
  EM_F2MC16        = 104, // Fujitsu F2MC16
  EM_MSP430        = 105, // Texas Instruments embedded microcontroller msp430
  EM_BLACKFIN      = 106, // Analog Devices Blackfin (DSP) processor
  EM_SE_C33        = 107, // S1C33 Family of Seiko Epson processors
  EM_SEP           = 108, // Sharp embedded microprocessor
  EM_ARCA          = 109, // Arca RISC Microprocessor
  EM_UNICORE       = 110, // Microprocessor series from PKU-Unity Ltd. and MPRC of Peking University
  EM_EXCESS        = 111, // eXcess: 16/32/64-bit configurable embedded CPU
  EM_DXP           = 112, // Icera Semiconductor Inc. Deep Execution Processor
  EM_ALTERA_NIOS2  = 113, // Altera Nios II soft-core processor
  EM_CRX           = 114, // National Semiconductor CompactRISC CRX microprocessor
  EM_XGATE         = 115, // Motorola XGATE embedded processor
  EM_C166          = 116, // Infineon C16x/XC16x processor
  EM_M16C          = 117, // Renesas M16C series microprocessors
  EM_DSPIC30F      = 118, // Microchip Technology dsPIC30F Digital Signal Controller
  EM_CE            = 119, // Freescale Communication Engine RISC core
  EM_M32C          = 120, // Renesas M32C series microprocessors

  EM_TSK3000       = 131, // Altium TSK3000 core
  EM_RS08          = 132, // Freescale RS08 embedded processor
  EM_SHARC         = 133, // Analog Devices SHARC family of 32-bit DSP processors
  EM_ECOG2         = 134, // Cyan Technology eCOG2 microprocessor
  EM_SCORE7        = 135, // Sunplus S+core7 RISC processor
  EM_DSP24         = 136, // New Japan Radio (NJR) 24-bit DSP Processor
  EM_VIDEOCORE3    = 137, // Broadcom VideoCore III processor
  EM_LATTICEMICO32 = 138, // RISC processor for Lattice FPGA architecture
  EM_SE_C17        = 139, // Seiko Epson C17 family
  EM_TI_C6000      = 140, // The Texas Instruments TMS320C6000 DSP family
  EM_TI_C2000      = 141, // The Texas Instruments TMS320C2000 DSP family
  EM_TI_C5500      = 142, // The Texas Instruments TMS320C55x DSP family

  EM_MMDSP_PLUS    = 160, // STMicroelectronics 64bit VLIW Data Signal Processor
  EM_CYPRESS_M8C   = 161, // Cypress M8C microprocessor
  EM_R32C          = 162, // Renesas R32C series microprocessors
  EM_TRIMEDIA      = 163, // NXP Semiconductors TriMedia architecture family
  EM_QDSP6         = 164, // QUALCOMM DSP6 Processor
  EM_8051          = 165, // Intel 8051 and variants
  EM_STXP7X        = 166, // STMicroelectronics STxP7x family of configurable and extensible RISC processors
  EM_NDS32         = 167, // Andes Technology compact code size embedded RISC processor family
  EM_ECOG1         = 168, // Cyan Technology eCOG1X family
  EM_ECOG1X        = 168, // Cyan Technology eCOG1X family
  EM_MAXQ30        = 169, // Dallas Semiconductor MAXQ30 Core Micro-controllers
  EM_XIMO16        = 170, // New Japan Radio (NJR) 16-bit DSP Processor
  EM_MANIK         = 171, // M2000 Reconfigurable RISC Microprocessor
  EM_CRAYNV2       = 172, // Cray Inc. NV2 vector architecture
  EM_RX            = 173, // Renesas RX family
  EM_METAG         = 174, // Imagination Technologies META processor architecture
  EM_MCST_ELBRUS   = 175, // MCST Elbrus general purpose hardware architecture
  EM_ECOG16        = 176, // Cyan Technology eCOG16 family
  EM_CR16          = 177, // National Semiconductor CompactRISC CR16 16-bit microprocessor
  EM_ETPU          = 178, // Freescale Extended Time Processing Unit
  EM_SLE9X         = 179, // Infineon Technologies SLE9X core
  EM_L10M          = 180, // Intel L10M
  EM_K10M          = 181, // Intel K10M

  EM_AARCH64       = 183, // ARM 64-bit architecture (AARCH64)

  EM_AVR32         = 185, // Atmel Corporation 32-bit microprocessor family
  EM_STM8          = 186, // STMicroeletronics STM8 8-bit microcontroller
  EM_TILE64        = 187, // Tilera TILE64 multicore architecture family
  EM_TILEPRO       = 188, // Tilera TILEPro multicore architecture family
  EM_MICROBLAZE    = 189, // Xilinx MicroBlaze 32-bit RISC soft processor core
  EM_CUDA          = 190, // NVIDIA CUDA architecture
  EM_TILEGX        = 191, // Tilera TILE-Gx multicore architecture family
  EM_CLOUDSHIELD   = 192, // CloudShield architecture family
  EM_COREA_1ST     = 193, // KIPO-KAIST Core-A 1st generation processor family
  EM_COREA_2ND     = 194, // KIPO-KAIST Core-A 2nd generation processor family
  EM_ARC_COMPACT2  = 195, // Synopsys ARCompact V2
  EM_OPEN8         = 196, // Open8 8-bit RISC soft processor core
  EM_RL78          = 197, // Renesas RL78 family
  EM_VIDEOCORE5    = 198, // Broadcom VideoCore V processor
  EM_78KOR         = 199, // Renesas 78KOR family
  EM_56800EX       = 200, // Freescale 56800EX Digital Signal Controller (DSC)
  EM_BA1           = 201, // Beyond BA1 CPU architecture
  EM_BA2           = 202, // Beyond BA2 CPU architecture
  EM_XCORE         = 203, // XMOS xCORE processor family
  EM_MCHP_PIC      = 204, // Microchip 8-bit PIC(r) family
} Elf_Ehdr_Machine;

// e_version values
typedef ELF_ENUM(Elf_Word, Elf_Ehdr_Version) {
  EV_NONE    = 0, // Invalid version
  EV_CURRENT = 1, // Current version
} Elf_Ehdr_Version;

// e_ident[] identification indices
enum {
  EI_MAG0       = 0, // File identification =     0x7f
  EI_MAG1       = 1, // File identification = 'E' 0x45
  EI_MAG2       = 2, // File identification = 'L' 0x4c
  EI_MAG3       = 3, // File identification = 'F' 0x46
  EI_CLASS      = 4, // File class
  EI_DATA       = 5, // Data encoding
  EI_VERSION    = 6, // File version
  EI_OSABI      = 7, // Operating system/ABI identification
  EI_ABIVERSION = 8, // ABI version
  EI_PAD        = 9, // Start of padding bytes
};

// Magic number
enum : uint8_t {
  ELFMAG0 = 0x7f,
  ELFMAG1 = 'E',
  ELFMAG2 = 'L',
  ELFMAG3 = 'F',
};

// File class
typedef ELF_ENUM(Elf_Byte, Elf_Ehdr_Class) {
  ELFCLASSNONE = 0, // Invalid class
  ELFCLASS32   = 1, // 32-bit objects
  ELFCLASS64   = 2, // 64-bit objects
} Elf_Ehdr_Class;

// Data encoding
typedef ELF_ENUM(Elf_Byte, Elf_Ehdr_Data) {
  ELFDATANONE = 0, // Invalid data encoding
  ELFDATA2LSB = 1, // 2's complement Little Endian
  ELFDATA2MSB = 2, // 2's complement Big Endian
} Elk_Ehdr_Data;

// OS/ABI identification
typedef ELF_ENUM(Elf_Byte, Elf_Ehdr_OsAbi) {
  ELFOSABI_NONE    = 0,  // No extensions or unspecified
  ELFOSABI_HPUX    = 1,  // Hewlett-Packard HP-UX
  ELFOSABI_NETBSD  = 2,  // NetBSD
  ELFOSABI_GNU     = 3,  // GNU
  ELFOSABI_LINUX   = 3,  // Linux (historical - alias for ELFOSABI_GNU)
  ELFOSABI_SOLARIS = 6,  // Sun Solaris
  ELFOSABI_AIX     = 7,  // AIX
  ELFOSABI_IRIX    = 8,  // IRIX
  ELFOSABI_FREEBSD = 9,  // FreeBSD
  ELFOSABI_TRU64   = 10, // Compaq TRU64 UNIX
  ELFOSABI_MODESTO = 11, // Novell Modesto
  ELFOSABI_OPENBSD = 12, // Open BSD
  ELFOSABI_OPENVMS = 13, // Open VMS
  ELFOSABI_NSK     = 14, // Hewlett-Packard Non-Stop Kernel
  ELFOSABI_AROS    = 15, // Amiga Research OS
  ELFOSABI_FENIXOS = 16, // The FenixOS highly scalable multi-core OS
} Elf_Ehdr_OsAbi;

// Special Section Indices
enum {
  SHN_UNDEF     = 0,      // Undefined, missing, irrelevant or meaningless

  SHN_LORESERVE = 0xff00, // Lower bound of reserved indices

  SHN_LOPROC    = 0xff00, // Processor specific
  SHN_HIPROC    = 0xff1f,

  SHN_LOOS      = 0xff20, // OS specific
  SHN_HIOS      = 0xff3f,

  SHN_ABS       = 0xfff1, // Absolute (symbols are not relocated)
  SHN_COMMON    = 0xfff2, // Common
  SHN_XINDEX    = 0xffff, // Indicates section header index is elsewhere

  SHN_HIRESERVE = 0xffff,
};

// Section types
typedef ELF_ENUM(Elf_Word, Elf_Shdr_Type) {
  SHT_NULL          = 0,          // Inactive
  SHT_PROGBITS      = 1,          // Program-defined information
  SHT_SYMTAB        = 2,          // Symbol table
  SHT_STRTAB        = 3,          // String table
  SHT_RELA          = 4,          // Relocation entries with explicit addends
  SHT_HASH          = 5,          // Symbol hash table
  SHT_DYNAMIC       = 6,          // Information for dynamic linking
  SHT_NOTE          = 7,          // Notes
  SHT_NOBITS        = 8,          // Program-defined empty space (bss)
  SHT_REL           = 9,          // Relocation entries without explicit addents
  SHT_SHLIB         = 10,         // Reserved
  SHT_DYNSYM        = 11,
  SHT_INIT_ARRAY    = 14,         // Pointers to initialization functions
  SHT_FINI_ARRAY    = 15,         // Pointers to termination functions
  SHT_PREINIT_ARRAY = 16,         // Pointers to pre-initialization functions
  SHT_GROUP         = 17,         // Defines a section group
  SHT_SYMTAB_SHNDX  = 18,         // Section header indices for symtab

  SHT_LOOS          = 0x60000000, // OS specific
    SHT_GNU_ATTRIBUTES = 0x6ffffff5, // Object attributes
    SHT_GNU_HASH       = 0x6ffffff6, // GNU-style hash table
    SHT_GNU_LIBLIST    = 0x6ffffff7, // Prelink library list
    SHT_CHECKSUM       = 0x6ffffff8, // Checksum for DSK content

    SHT_LOSUNW         = 0x6ffffffa, // Sun-specific
    SHT_SUNW_move      = 0x6ffffffa,
    SHT_SUNW_COMDAT    = 0x6ffffffb,
    SHT_SUNW_syminfo   = 0x6ffffffc,

    SHT_GNU_verdef     = 0x6ffffffd,
    SHT_GNU_verneed    = 0x6ffffffe,
    SHT_GNU_versym     = 0x6fffffff,

    SHT_HISUNW         = 0x6fffffff,
  SHT_HIOS          = 0x6fffffff,

  SHT_LOPROC        = 0x70000000, // Processor specific
  SHT_HIPROC        = 0x7fffffff,

  SHT_LOUSER        = 0x80000000, // Application specific
  SHT_HIUSER        = 0xffffffff,
} Elf_Shdr_Type;

// Section attribute flags (we can't have a type for these because the
// 64-bit section header defines them as 64-bit)
enum {
  SHF_WRITE            = 0x1,        // Writable
  SHF_ALLOC            = 0x2,        // Mapped
  SHF_EXECINSTR        = 0x4,        // Executable instructions
  SHF_MERGE            = 0x10,       // Mergeable elements
  SHF_STRINGS          = 0x20,       // NUL-terminated strings
  SHF_INFO_LINK        = 0x40,       // Section header table index
  SHF_LINK_ORDER       = 0x80,       // Special ordering requirement
  SHF_OS_NONCONFORMING = 0x100,      // OS-specific processing
  SHF_GROUP            = 0x200,      // Section group member
  SHF_TLS              = 0x400,      // Thread Local Storage
  SHF_COMPRESSED       = 0x800,      // Compressed
  SHF_MASKOS           = 0x0ff00000, // Operating system specific flags
  SHF_MASKPROC         = 0xf0000000, // Processor specific flags
};

// Section group flags
enum : Elf_Word {
  GRP_COMDAT   = 0x1,        // COMDAT group
  GRP_MASKOS   = 0x0ff00000, // Operating system specific flags
  GRP_MASKPROC = 0xf0000000, // Processof specific flags
};

// Compression type
typedef ELF_ENUM(Elf_Word, Elf_Chdr_Type) {
  ELFCOMPRESS_ZLIB   = 1,          // DEFLATE algorithm
  ELFCOMPRESS_ZSTD   = 2,          // zstd algorithm

  ELFCOMPRESS_LOOS   = 0x60000000, // Operating system specific
  ELFCOMPRESS_HIOS   = 0x6fffffff,

  ELFCOMPRESS_LOPROC = 0x70000000, // Processor specific
  ELFCOMPRESS_HIPROC = 0x7fffffff
} Elf_Chdr_Type;

// Symbol table entry
enum : Elf_Word {
  STN_UNDEF = 0
};

typedef ELF_ENUM(Elf_Byte, Elf_Sym_Binding) {
  STB_LOCAL  = 0,
  STB_GLOBAL = 1,
  STB_WEAK   = 2,

  STB_LOOS   = 10, // Operating system specific
  STB_HIOS   = 12,

  STB_LOPROC = 13, // Processor specific
  STB_HIPROC = 15
} Elf_Sym_Binding;

typedef ELF_ENUM(Elf_Byte, Elf_Sym_Type) {
  STT_NOTYPE  = 0,  // Unspecified
  STT_OBJECT  = 1,  // Data object (variable, array, &c)
  STT_FUNC    = 2,  // Function or other executable code
  STT_SECTION = 3,  // A section
  STT_FILE    = 4,  // Source file name
  STT_COMMON  = 5,  // Uninitialized common block
  STT_TLS     = 6,  // Thread Local Storage

  STT_LOOS    = 10, // Operating system specific
  STT_HIOS    = 12,

  STT_LOPROC  = 13, // Processor specific
  STT_HIPROC  = 15,
} Elf_Sym_Type;

typedef ELF_ENUM(Elf_Byte, Elf_Sym_Visibility) {
  STV_DEFAULT   = 0,
  STV_INTERNAL  = 1, // Processor specific but like hidden
  STV_HIDDEN    = 2, // Not visible from other components
  STV_PROTECTED = 3, // Visible but cannot be preempted
} Elf_Sym_Visibility;

// Program header types
typedef ELF_ENUM(Elf_Word, Elf_Phdr_Type) {
  PT_NULL    = 0,          // Element unused
  PT_LOAD    = 1,          // Loadable segment
  PT_DYNAMIC = 2,          // Dynamic linking information
  PT_INTERP  = 3,          // Interpreter
  PT_NOTE    = 4,          // Auxiliary information
  PT_SHLIB   = 5,          // Reserved
  PT_PHDR    = 6,          // Program header table
  PT_TLS     = 7,          // Thread Local Storage

  PT_LOOS    = 0x60000000, // Operating system specific
    PT_GNU_EH_FRAME = 0x6474e550, // GNU .eh_frame_hdr segment
    PT_GNU_STACK    = 0x6474e551, // Indicates stack executability
    PT_GNU_RELRO    = 0x6474e552, // Read-only after relocation

    PT_LOSUNW       = 0x6ffffffa,
    PT_SUNWBSS      = 0x6ffffffa,
    PT_SUNWSTACK    = 0x6ffffffb,
    PT_HISUNW       = 0x6fffffff,
  PT_HIOS    = 0x6fffffff,

  PT_LOPROC  = 0x70000000, // Processor specific
  PT_HIPROC  = 0x7fffffff,
} Elf_Phdr_Type;

// Program header flags
typedef ELF_OPTIONS(Elf_Word, Elf_Phdr_Flags) {
  PF_X        = 0x1,        // Execute
  PF_W        = 0x2,        // Write
  PF_R        = 0x4,        // Read,

  PF_MASKOS   = 0x0ff00000, // Operating system specific
  PF_MASKPROC = 0xf0000000, // Processor specific
};

// Dynamic linking tags
enum {
  DT_NULL            = 0,  // Marks the end of the _DYNAMIC array
  DT_NEEDED          = 1,  // String table offset of name of needed library
  DT_PLTRELSZ        = 2,  // Total size of relocation entries for PLT
  DT_PLTGOT          = 3,  // Address of PLT/GOT
  DT_HASH            = 4,  // Address of symbol hash table
  DT_STRTAB          = 5,  // Address of string table
  DT_SYMTAB          = 6,  // Address of symbol table
  DT_RELA            = 7,  // Address of DT_RELA relocation table
  DT_RELASZ          = 8,  // Size of DT_RELA table
  DT_RELAENT         = 9,  // Size of DT_RELA entry
  DT_STRSZ           = 10, // Size of string table
  DT_SYMENT          = 11, // Size of symbol table entry
  DT_INIT            = 12, // Address of initialization function
  DT_FINI            = 13, // Address of termination function
  DT_SONAME          = 14, // String table offset of name of shared object
  DT_RPATH           = 15, // String table offset of search path
  DT_SYMBOLIC        = 16, // Means to search from shared object first
  DT_REL             = 17, // Address of DT_REL relocation table
  DT_RELSZ           = 18, // Size of DT_REL table
  DT_RELENT          = 19, // Size of DT_REL entry
  DT_PLTREL          = 20, // Type of PLT relocation entry (DT_REL/DT_RELA)
  DT_DEBUG           = 21, // Used for debugging
  DT_TEXTREL         = 22, // Means relocations might write to read-only segment
  DT_JMPREL          = 23, // Address of relocation entries for PLT
  DT_BIND_NOW        = 24, // Means linker should not lazily bind
  DT_INIT_ARRAY      = 25, // Address of pointers to initialization functions
  DT_FINI_ARRAY      = 26, // Address of pointers to termination functions
  DT_INIT_ARRAYSZ    = 27, // Size in bytes of initialization function array
  DT_FINI_ARRAYSZ    = 28, // Size in bytes of termination function array
  DT_RUNPATH         = 29, // String table offset of search path
  DT_FLAGS           = 30, // Flags

  DT_ENCODING        = 32, // Tags equal to or above this follow encoding rules

  DT_PREINIT_ARRAY   = 32, // Address of pre-initialization function array
  DT_PREINIT_ARRAYSZ = 33, // Size in bytes of pre-initialization fn array

  DT_LOOS            = 0x6000000D, // Operating system specific
  DT_HIOS            = 0x6ffff000,

  DT_LOPROC          = 0x70000000, // Processor specific
  DT_HIPROC          = 0x7fffffff,
};

// Dynamic linking flags
enum {
  DF_ORIGIN     = 0x1,  // Uses $ORIGIN substitution string
  DF_SYMBOLIC   = 0x2,  // Search shared object first before usual search
  DF_TEXTREL    = 0x4,  // Relocations may modify read-only segments
  DF_BIND_NOW   = 0x8,  // Linker should not lazily bind
  DF_STATIC_TLS = 0x10, // Uses static TLS - must not be dynamically loaded
};

// GNU note types
enum {
  NT_GNU_ABI_TAG         = 1, // ABI information
  NT_GNU_HWCAP           = 2, // Synthetic hwcap information
  NT_GNU_BUILD_ID        = 3, // Build ID
  NT_GNU_GOLD_VERSION    = 4, // Generated by GNU gold
  NT_GNU_PROPERTY_TYPE_0 = 5, // Program property
};

/* .. ELF Header ............................................................ */

#define EI_NIDENT 16

typedef struct {
  Elf32_Byte       e_ident[EI_NIDENT];
  Elf_Ehdr_Type    e_type;
  Elf_Ehdr_Machine e_machine;
  Elf_Ehdr_Version e_version;
  Elf32_Addr       e_entry;
  Elf32_Off        e_phoff;
  Elf32_Off        e_shoff;
  Elf32_Word       e_flags;
  Elf32_Half       e_ehsize;
  Elf32_Half       e_phentsize;
  Elf32_Half       e_phnum;
  Elf32_Half       e_shentsize;
  Elf32_Half       e_shnum;
  Elf32_Half       e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  Elf64_Byte       e_ident[EI_NIDENT];
  Elf_Ehdr_Type    e_type;
  Elf_Ehdr_Machine e_machine;
  Elf_Ehdr_Version e_version;
  Elf64_Addr       e_entry;
  Elf64_Off        e_phoff;
  Elf64_Off        e_shoff;
  Elf64_Word       e_flags;
  Elf64_Half       e_ehsize;
  Elf64_Half       e_phentsize;
  Elf64_Half       e_phnum;
  Elf64_Half       e_shentsize;
  Elf64_Half       e_shnum;
  Elf64_Half       e_shstrndx;
} Elf64_Ehdr;

/* .. Section Header ........................................................ */

typedef struct {
  Elf32_Word    sh_name;
  Elf_Shdr_Type sh_type;
  Elf32_Word    sh_flags;
  Elf32_Addr    sh_addr;
  Elf32_Off     sh_offset;
  Elf32_Word    sh_size;
  Elf32_Word    sh_link;
  Elf32_Word    sh_info;
  Elf32_Word    sh_addralign;
  Elf32_Word    sh_entsize;
} Elf32_Shdr;

typedef struct {
  Elf64_Word    sh_name;
  Elf_Shdr_Type sh_type;
  Elf64_Xword   sh_flags;
  Elf64_Addr    sh_addr;
  Elf64_Off     sh_offset;
  Elf64_Xword   sh_size;
  Elf64_Word    sh_link;
  Elf64_Word    sh_info;
  Elf64_Xword   sh_addralign;
  Elf64_Xword   sh_entsize;
} Elf64_Shdr;

/* .. Compression Header .................................................... */

typedef struct {
  Elf_Chdr_Type ch_type;
  Elf32_Word    ch_size;
  Elf32_Word    ch_addralign;
} Elf32_Chdr;

typedef struct {
  Elf_Chdr_Type ch_type;
  Elf64_Word    ch_reserved;
  Elf64_Xword   ch_size;
  Elf64_Xword   ch_addralign;
} Elf64_Chdr;

/* .. Symbol Table .......................................................... */

typedef struct {
  Elf32_Word st_name;
  Elf32_Addr st_value;
  Elf32_Word st_size;
  Elf32_Byte st_info;
  Elf32_Byte st_other;
  Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
  Elf64_Word  st_name;
  Elf64_Byte  st_info;
  Elf64_Byte  st_other;
  Elf64_Half  st_shndx;
  Elf64_Addr  st_value;
  Elf64_Xword st_size;
} Elf64_Sym;

static inline Elf_Sym_Binding ELF32_ST_BIND(Elf_Byte i) { return i >> 4; }
static inline Elf_Sym_Type ELF32_ST_TYPE(Elf_Byte i) { return i & 0xf; }
static inline Elf_Byte ELF32_ST_INFO(Elf_Sym_Binding b, Elf_Sym_Type t) {
  return (b << 4) | (t & 0xf);
}

static inline Elf_Sym_Binding ELF64_ST_BIND(Elf_Byte i) { return i >> 4; }
static inline Elf_Sym_Type ELF64_ST_TYPE(Elf_Byte i) { return i & 0xf; }
static inline Elf_Byte ELF64_ST_INFO(Elf_Sym_Binding b, Elf_Sym_Type t) {
  return (b << 4) | (t & 0xf);
}

static inline Elf_Sym_Visibility ELF32_ST_VISIBILITY(Elf_Byte o) {
  return o & 3;
}
static inline Elf_Sym_Visibility ELF64_ST_VISIBILITY(Elf_Byte o) {
  return o & 3;
}

/* .. Relocation ............................................................ */

typedef struct {
  Elf32_Addr    r_offset;
  Elf32_Word    r_info;
} Elf32_Rel;

typedef struct {
  Elf32_Addr    r_offset;
  Elf32_Word    r_info;
  Elf32_Sword   r_addend;
} Elf32_Rela;

typedef struct {
  Elf64_Addr    r_offset;
  Elf64_Xword   r_info;
} Elf64_Rel;

typedef struct {
  Elf64_Addr    r_offset;
  Elf64_Xword   r_info;
  Elf64_Sxword  r_addend;
} Elf64_Rela;

static inline Elf32_Byte ELF32_R_SYM(Elf32_Word i) { return i >> 8; }
static inline Elf32_Byte ELF32_R_TYPE(Elf32_Word i) { return i & 0xff; }
static inline Elf32_Word ELF32_R_INFO(Elf32_Byte s, Elf32_Byte t) {
  return (s << 8) | t;
}

static inline Elf64_Word ELF64_R_SYM(Elf64_Xword i) { return i >> 32; }
static inline Elf64_Word ELF64_R_TYPE(Elf64_Xword i) { return i & 0xffffffff; }
static inline Elf64_Xword ELF64_R_INFO(Elf64_Word s, Elf64_Word t) {
  return (((Elf64_Xword)s) << 32) | t;
}

/* .. Program Header ........................................................ */

typedef struct {
  Elf_Phdr_Type   p_type;
  Elf32_Off       p_offset;
  Elf32_Addr      p_vaddr;
  Elf32_Addr      p_paddr;
  Elf32_Word      p_filesz;
  Elf32_Word      p_memsz;
  Elf_Phdr_Flags  p_flags;
  Elf32_Word      p_align;
} Elf32_Phdr;

typedef struct {
  Elf_Phdr_Type   p_type;
  Elf_Phdr_Flags  p_flags;
  Elf64_Off       p_offset;
  Elf64_Addr      p_vaddr;
  Elf64_Addr      p_paddr;
  Elf64_Xword     p_filesz;
  Elf64_Xword     p_memsz;
  Elf64_Xword     p_align;
} Elf64_Phdr;

/* .. Note Header ........................................................... */

typedef struct {
  Elf32_Word n_namesz;
  Elf32_Word n_descsz;
  Elf32_Word n_type;
} Elf32_Nhdr;

typedef struct {
  Elf64_Word n_namesz;
  Elf64_Word n_descsz;
  Elf64_Word n_type;
} Elf64_Nhdr;

/* .. Dynamic Linking ....................................................... */

typedef struct {
  Elf32_Sword   d_tag;
  union {
    Elf32_Word  d_val;
    Elf32_Addr  d_ptr;
  } d_un;
} Elf32_Dyn;

typedef struct {
  Elf64_Sxword  d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr  d_ptr;
  } d_un;
} Elf64_Dyn;

/* .. Hash Table ............................................................ */

typedef struct {
  Elf32_Word h_nbucket;
  Elf32_Word h_nchain;
} Elf32_Hash;

typedef struct {
  Elf64_Word h_nbucket;
  Elf64_Word h_nchain;
} Elf64_Hash;

static inline unsigned long
elf_hash(const unsigned char *name)
{
  unsigned long h = 0, g;
  while (*name) {
    h = (h << 4) + *name++;
    if ((g = h & 0xf0000000))
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

#endif // ELF_H
