//===--- ReflectionContext.h - Swift Type Reflection Context ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implements the context for reflection of values in the address space of a
// remote process.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_REFLECTIONCONTEXT_H
#define SWIFT_REFLECTION_REFLECTIONCONTEXT_H

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Memory.h"
#include "llvm/ADT/STLExtras.h"

#include "swift/ABI/Enum.h"
#include "swift/ABI/ObjectFile.h"
#include "swift/Concurrency/Actor.h"
#include "swift/Remote/MemoryReader.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/RemoteInspection/GenericMetadataCacheEntry.h"
#include "swift/RemoteInspection/Records.h"
#include "swift/RemoteInspection/RuntimeInternals.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/Basic/Unreachable.h"

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <inttypes.h>

// The Swift runtime can be built in two ways: with or without
// SWIFT_CONCURRENCY_ENABLE_PRIORITY_ESCALATION enabled. In order to decode the
// lock used in a runtime with priority escalation enabled, we need inline
// functions from dispatch/swift_concurrency_private.h. If we don't have that
// header at build time, we can still build but we'll be unable to decode the
// lock and thus information about a running task is degraded. There are four
// combinations:
//
//      Runtime        | swift_concurrency_private.h | task running info
// --------------------+-----------------------------+------------------
// without escalation  |         present             |       full
// without escalation  |       not present           |       full
//   with escalation   |         present             |       full
//   with escalation   |       not present           |     DEGRADED
//
// Currently, degraded info has these effects:
// 1. Task.IsRunning is not available, indicated with Task.HasIsRunning = false.
// 2. Task async backtraces are not provided.
// 3. Task and actor thread ports are not available, indicated with
//    HasThreadPort = false.

#if __has_include(<dispatch/swift_concurrency_private.h>)
#include <dispatch/swift_concurrency_private.h>
#define HAS_DISPATCH_LOCK_IS_LOCKED 1
#endif

namespace {

template <unsigned PointerSize> struct MachOTraits;

template <> struct MachOTraits<4> {
  using Header = const struct llvm::MachO::mach_header;
  using SegmentCmd = const struct llvm::MachO::segment_command;
  using Section = const struct llvm::MachO::section;
  static constexpr size_t MagicNumber = llvm::MachO::MH_MAGIC;
};

template <> struct MachOTraits<8> {
  using Header = const struct llvm::MachO::mach_header_64;
  using SegmentCmd = const struct llvm::MachO::segment_command_64;
  using Section = const struct llvm::MachO::section_64;
  static constexpr size_t MagicNumber = llvm::MachO::MH_MAGIC_64;
};

template <unsigned char ELFClass> struct ELFTraits;

template <> struct ELFTraits<llvm::ELF::ELFCLASS32> {
  using Header = const struct llvm::ELF::Elf32_Ehdr;
  using Section = const struct llvm::ELF::Elf32_Shdr;
  using Offset = llvm::ELF::Elf32_Off;
  using Size = llvm::ELF::Elf32_Word;
  static constexpr unsigned char ELFClass = llvm::ELF::ELFCLASS32;
};

template <> struct ELFTraits<llvm::ELF::ELFCLASS64> {
  using Header = const struct llvm::ELF::Elf64_Ehdr;
  using Section = const struct llvm::ELF::Elf64_Shdr;
  using Offset = llvm::ELF::Elf64_Off;
  using Size = llvm::ELF::Elf64_Xword;
  static constexpr unsigned char ELFClass = llvm::ELF::ELFCLASS64;
};

} // namespace

namespace swift {
namespace reflection {

using swift::remote::MemoryReader;
using swift::remote::RemoteAddress;

template <typename Runtime>
class ReflectionContext
       : public remote::MetadataReader<Runtime, TypeRefBuilder> {
  using super = remote::MetadataReader<Runtime, TypeRefBuilder>;
  using super::readMetadata;
  using super::readObjCClassName;
  using super::readResolvedPointerValue;
  std::unordered_map<typename super::StoredPointer, const RecordTypeInfo *>
      Cache;

  /// All buffers we need to keep around long term. This will automatically free them
  /// when this object is destroyed.
  std::vector<MemoryReader::ReadBytesResult> savedBuffers;
  std::vector<std::tuple<RemoteAddress, RemoteAddress>> imageRanges;

  bool setupTargetPointers = false;
  typename super::StoredPointer target_non_future_adapter = 0;
  typename super::StoredPointer target_future_adapter = 0;
  typename super::StoredPointer target_task_wait_throwing_resume_adapter = 0;
  typename super::StoredPointer target_task_future_wait_resume_adapter = 0;
  bool supportsPriorityEscalation = false;

public:
  using super::getBuilder;
  using super::readDemanglingForContextDescriptor;
  using super::readGenericArgFromMetadata;
  using super::readIsaMask;
  using super::readMetadataAndValueErrorExistential;
  using super::readMetadataAndValueOpaqueExistential;
  using super::readMetadataFromInstance;
  using super::readTypeFromMetadata;
  using super::stripSignedPointer;
  using typename super::StoredPointer;
  using typename super::StoredSignedPointer;
  using typename super::StoredSize;

  struct AsyncTaskAllocationChunk {
    enum class ChunkKind {
      Unknown,
      NonPointer,
      RawPointer,
      StrongReference,
      UnownedReference,
      WeakReference,
      UnmanagedReference
    };

    StoredPointer Start;
    unsigned Length;
    ChunkKind Kind;
  };

  struct AsyncTaskSlabInfo {
    StoredPointer NextSlab;
    StoredSize SlabSize;
    std::vector<AsyncTaskAllocationChunk> Chunks;
  };

  struct AsyncTaskInfo {
    // Job flags.
    unsigned Kind;
    unsigned EnqueuePriority;
    bool IsChildTask;
    bool IsFuture;
    bool IsGroupChildTask;
    bool IsAsyncLetTask;

    // Task flags.
    unsigned MaxPriority;
    bool IsCancelled;
    bool IsStatusRecordLocked;
    bool IsEscalated;
    bool HasIsRunning; // If false, the IsRunning flag is not valid.
    bool IsRunning;
    bool IsEnqueued;

    bool HasThreadPort;
    uint32_t ThreadPort;

    uint64_t Id;
    StoredPointer RunJob;
    StoredPointer AllocatorSlabPtr;
    std::vector<StoredPointer> ChildTasks;
    std::vector<StoredPointer> AsyncBacktraceFrames;
  };

  struct ActorInfo {
    StoredPointer FirstJob;

    uint8_t State;
    bool IsPriorityEscalated;
    bool IsDistributedRemote;
    uint8_t MaxPriority;

    bool HasThreadPort;
    uint32_t ThreadPort;
  };

  explicit ReflectionContext(
      std::shared_ptr<MemoryReader> reader,
      remote::ExternalTypeRefCache *externalCache = nullptr)
      : super(std::move(reader), *this, externalCache) {}

  ReflectionContext(const ReflectionContext &other) = delete;
  ReflectionContext &operator=(const ReflectionContext &other) = delete;

  MemoryReader &getReader() {
    return *this->Reader;
  }

  unsigned getSizeOfHeapObject() {
    // This must match sizeof(HeapObject) for the target.
    return sizeof(StoredPointer) * 2;
  }

  /// On success returns the ID of the newly registered Reflection Info.
  template <typename T>
  llvm::Optional<uint32_t>
  readMachOSections(
      RemoteAddress ImageStart,
      llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    auto Buf =
        this->getReader().readBytes(ImageStart, sizeof(typename T::Header));
    if (!Buf)
      return false;
    auto Header = reinterpret_cast<typename T::Header *>(Buf.get());
    assert(Header->magic == T::MagicNumber && "invalid MachO file");

    auto NumCommands = Header->sizeofcmds;

    // The layout of the executable is such that the commands immediately follow
    // the header.
    auto CmdStartAddress =
        RemoteAddress(ImageStart.getAddressData() + sizeof(typename T::Header));
    uint32_t SegmentCmdHdrSize = sizeof(typename T::SegmentCmd);
    uint64_t Offset = 0;

    // Find the __TEXT segment.
    typename T::SegmentCmd *Command = nullptr;
    for (unsigned I = 0; I < NumCommands; ++I) {
      auto CmdBuf = this->getReader().readBytes(
          RemoteAddress(CmdStartAddress.getAddressData() + Offset),
          SegmentCmdHdrSize);
      if (!CmdBuf)
        return false;
      auto CmdHdr = reinterpret_cast<typename T::SegmentCmd *>(CmdBuf.get());
      if (strncmp(CmdHdr->segname, "__TEXT", sizeof(CmdHdr->segname)) == 0) {
        Command = CmdHdr;
        savedBuffers.push_back(std::move(CmdBuf));
        break;
      }
      Offset += CmdHdr->cmdsize;
    }

    // No __TEXT segment, bail out.
    if (!Command)
      return false;

   // Find the load command offset.
    auto loadCmdOffset = ImageStart.getAddressData() + Offset + sizeof(typename T::Header);

    // Read the load command.
    auto LoadCmdAddress = reinterpret_cast<const char *>(loadCmdOffset);
    auto LoadCmdBuf = this->getReader().readBytes(
        RemoteAddress(LoadCmdAddress), sizeof(typename T::SegmentCmd));
    if (!LoadCmdBuf)
      return false;
    auto LoadCmd = reinterpret_cast<typename T::SegmentCmd *>(LoadCmdBuf.get());

    // The sections start immediately after the load command.
    unsigned NumSect = LoadCmd->nsects;
    auto SectAddress = reinterpret_cast<const char *>(loadCmdOffset) +
                       sizeof(typename T::SegmentCmd);
    auto Sections = this->getReader().readBytes(
        RemoteAddress(SectAddress), NumSect * sizeof(typename T::Section));
    if (!Sections)
      return false;

    auto Slide = ImageStart.getAddressData() - Command->vmaddr;
    auto SectionsBuf = reinterpret_cast<const char *>(Sections.get());

    auto findMachOSectionByName = [&](llvm::StringRef Name)
        -> std::pair<RemoteRef<void>, uint64_t> {
      for (unsigned I = 0; I < NumSect; ++I) {
        auto S = reinterpret_cast<typename T::Section *>(
            SectionsBuf + (I * sizeof(typename T::Section)));
        if (strncmp(S->sectname, Name.data(), sizeof(S->sectname)) != 0)
          continue;

        auto RemoteSecStart = S->addr + Slide;
        auto LocalSectBuf =
            this->getReader().readBytes(RemoteAddress(RemoteSecStart), S->size);
        if (!LocalSectBuf)
          return {nullptr, 0};

        auto StartRef = RemoteRef<void>(RemoteSecStart, LocalSectBuf.get());
        savedBuffers.push_back(std::move(LocalSectBuf));
        return {StartRef, S->size};
      }
      return {nullptr, 0};
    };

    SwiftObjectFileFormatMachO ObjectFileFormat;
    auto FieldMdSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::fieldmd));
    auto AssocTySec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::assocty));
    auto BuiltinTySec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::builtin));
    auto CaptureSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::capture));
    auto TypeRefMdSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::typeref));
    auto ReflStrMdSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::reflstr));
    auto ConformMdSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::conform));
    auto MPEnumMdSec = findMachOSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::mpenum));

    if (FieldMdSec.first == nullptr &&
        AssocTySec.first == nullptr &&
        BuiltinTySec.first == nullptr &&
        CaptureSec.first == nullptr &&
        TypeRefMdSec.first == nullptr &&
        ReflStrMdSec.first == nullptr &&
        ConformMdSec.first == nullptr &&
        MPEnumMdSec.first == nullptr)
      return false;

    ReflectionInfo info = {{FieldMdSec.first, FieldMdSec.second},
                           {AssocTySec.first, AssocTySec.second},
                           {BuiltinTySec.first, BuiltinTySec.second},
                           {CaptureSec.first, CaptureSec.second},
                           {TypeRefMdSec.first, TypeRefMdSec.second},
                           {ReflStrMdSec.first, ReflStrMdSec.second},
                           {ConformMdSec.first, ConformMdSec.second},
                           {MPEnumMdSec.first, MPEnumMdSec.second},
                           PotentialModuleNames};

    auto InfoID = this->addReflectionInfo(info);

    // Find the __DATA segment.
    for (unsigned I = 0; I < NumCommands; ++I) {
      auto CmdBuf = this->getReader().readBytes(
          RemoteAddress(CmdStartAddress.getAddressData() + Offset),
          SegmentCmdHdrSize);
      if (!CmdBuf)
        return false;
      auto CmdHdr = reinterpret_cast<typename T::SegmentCmd *>(CmdBuf.get());
      if (strncmp(CmdHdr->segname, "__DATA", sizeof(CmdHdr->segname)) == 0) {
        auto DataSegmentEnd =
            ImageStart.getAddressData() + CmdHdr->vmaddr + CmdHdr->vmsize;
        assert(DataSegmentEnd > ImageStart.getAddressData() &&
               "invalid range for __DATA");
        imageRanges.push_back(
            std::make_tuple(ImageStart, RemoteAddress(DataSegmentEnd)));
        break;
      }
      Offset += CmdHdr->cmdsize;
    }

    savedBuffers.push_back(std::move(Buf));
    savedBuffers.push_back(std::move(Sections));
    return InfoID;
  }

  /// On success returns the ID of the newly registered Reflection Info.
  llvm::Optional<uint32_t> readPECOFFSections(
      RemoteAddress ImageStart,
      llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    auto DOSHdrBuf = this->getReader().readBytes(
        ImageStart, sizeof(llvm::object::dos_header));
    if (!DOSHdrBuf)
      return false;
    auto DOSHdr =
        reinterpret_cast<const llvm::object::dos_header *>(DOSHdrBuf.get());
    auto COFFFileHdrAddr = ImageStart.getAddressData() +
                           DOSHdr->AddressOfNewExeHeader +
                           sizeof(llvm::COFF::PEMagic);

    auto COFFFileHdrBuf = this->getReader().readBytes(
        RemoteAddress(COFFFileHdrAddr), sizeof(llvm::object::coff_file_header));
    if (!COFFFileHdrBuf)
      return false;
    auto COFFFileHdr = reinterpret_cast<const llvm::object::coff_file_header *>(
        COFFFileHdrBuf.get());

    auto SectionTableAddr = COFFFileHdrAddr +
                            sizeof(llvm::object::coff_file_header) +
                            COFFFileHdr->SizeOfOptionalHeader;
    auto SectionTableBuf = this->getReader().readBytes(
        RemoteAddress(SectionTableAddr),
        sizeof(llvm::object::coff_section) * COFFFileHdr->NumberOfSections);
    if (!SectionTableBuf)
      return false;

    auto findCOFFSectionByName =
        [&](llvm::StringRef Name) -> std::pair<RemoteRef<void>, uint64_t> {
      for (size_t i = 0; i < COFFFileHdr->NumberOfSections; ++i) {
        const llvm::object::coff_section *COFFSec =
            reinterpret_cast<const llvm::object::coff_section *>(
                SectionTableBuf.get()) +
            i;
        llvm::StringRef SectionName =
            (COFFSec->Name[llvm::COFF::NameSize - 1] == 0)
                ? COFFSec->Name
                : llvm::StringRef(COFFSec->Name, llvm::COFF::NameSize);
        if (SectionName != Name)
          continue;
        auto Addr = ImageStart.getAddressData() + COFFSec->VirtualAddress;
        auto Buf = this->getReader().readBytes(RemoteAddress(Addr),
                                               COFFSec->VirtualSize);
        if (!Buf)
          return {nullptr, 0};
        auto BufStart = Buf.get();
        savedBuffers.push_back(std::move(Buf));

        auto Begin = RemoteRef<void>(Addr, BufStart);
        auto Size = COFFSec->VirtualSize;

        // FIXME: This code needs to be cleaned up and updated
        // to make it work for 32 bit platforms.
        Begin = Begin.atByteOffset(8);
        Size -= 16;

        return {Begin, Size};
      }
      return {nullptr, 0};
    };

    SwiftObjectFileFormatCOFF ObjectFileFormat;
    auto FieldMdSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::fieldmd));
    auto AssocTySec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::assocty));
    auto BuiltinTySec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::builtin));
    auto CaptureSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::capture));
    auto TypeRefMdSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::typeref));
    auto ReflStrMdSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::reflstr));
    auto ConformMdSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::conform));
    auto MPEnumMdSec = findCOFFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::mpenum));

    if (FieldMdSec.first == nullptr &&
        AssocTySec.first == nullptr &&
        BuiltinTySec.first == nullptr &&
        CaptureSec.first == nullptr &&
        TypeRefMdSec.first == nullptr &&
        ReflStrMdSec.first == nullptr &&
        ConformMdSec.first == nullptr &&
        MPEnumMdSec.first == nullptr)
      return false;

    ReflectionInfo Info = {{FieldMdSec.first, FieldMdSec.second},
                           {AssocTySec.first, AssocTySec.second},
                           {BuiltinTySec.first, BuiltinTySec.second},
                           {CaptureSec.first, CaptureSec.second},
                           {TypeRefMdSec.first, TypeRefMdSec.second},
                           {ReflStrMdSec.first, ReflStrMdSec.second},
                           {ConformMdSec.first, ConformMdSec.second},
                           {MPEnumMdSec.first, MPEnumMdSec.second},
                           PotentialModuleNames};
    return this->addReflectionInfo(Info);
  }

  /// On success returns the ID of the newly registered Reflection Info.
  llvm::Optional<uint32_t> readPECOFF(RemoteAddress ImageStart,
                  llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    auto Buf = this->getReader().readBytes(ImageStart,
                                           sizeof(llvm::object::dos_header));
    if (!Buf)
      return false;

    auto DOSHdr = reinterpret_cast<const llvm::object::dos_header *>(Buf.get());

    auto PEHeaderAddress =
        ImageStart.getAddressData() + DOSHdr->AddressOfNewExeHeader;

    Buf = this->getReader().readBytes(RemoteAddress(PEHeaderAddress),
                                      sizeof(llvm::COFF::PEMagic));
    if (!Buf)
      return false;

    if (memcmp(Buf.get(), llvm::COFF::PEMagic, sizeof(llvm::COFF::PEMagic)))
      return false;

    return readPECOFFSections(ImageStart, PotentialModuleNames);
  }

  /// On success returns the ID of the newly registered Reflection Info.
  template <typename T>
  llvm::Optional<uint32_t> readELFSections(
      RemoteAddress ImageStart,
      llvm::Optional<llvm::sys::MemoryBlock> FileBuffer,
      llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    // When reading from the FileBuffer we can simply return a pointer to
    // the underlying data.
    // When reading from the process, we need to keep the memory around
    // until the end of the function, so we store it inside ReadDataBuffer.
    // We do this so in both cases we can return a simple pointer.
    std::vector<MemoryReader::ReadBytesResult> ReadDataBuffer;
    auto readData = [&](uint64_t Offset, uint64_t Size) -> const void * {
      if (FileBuffer.has_value()) {
        auto Buffer = FileBuffer.value();
        if (Offset + Size > Buffer.allocatedSize())
          return nullptr;
        return (const void *)((uint64_t)Buffer.base() + Offset);
      } else {
        MemoryReader::ReadBytesResult Buf =
            this->getReader().readBytes(ImageStart + Offset, Size);
        if (!Buf)
          return nullptr;
        ReadDataBuffer.push_back(std::move(Buf));
        return ReadDataBuffer.back().get();
      }
    };

    const void *Buf = readData(0, sizeof(typename T::Header));
    if (!Buf)
      return false;
    auto Hdr = reinterpret_cast<const typename T::Header *>(Buf);
    assert(Hdr->getFileClass() == T::ELFClass && "invalid ELF file class");

    // From the header, grab information about the section header table.
    uint64_t SectionHdrAddress = Hdr->e_shoff;
    uint16_t SectionHdrNumEntries = Hdr->e_shnum;
    uint16_t SectionEntrySize = Hdr->e_shentsize;

    if (sizeof(typename T::Section) > SectionEntrySize)
      return false;
    if (SectionHdrNumEntries == 0)
      return false;

    // Collect all the section headers, we need them to look up the
    // reflection sections (by name) and the string table.
    // We read the section headers from the FileBuffer, since they are
    // not mapped in the child process.
    std::vector<const typename T::Section *> SecHdrVec;
    for (unsigned I = 0; I < SectionHdrNumEntries; ++I) {
      uint64_t Offset = SectionHdrAddress + (I * SectionEntrySize);
      auto SecBuf = readData(Offset, sizeof(typename T::Section));
      if (!SecBuf)
        return false;
      const typename T::Section *SecHdr =
          reinterpret_cast<const typename T::Section *>(SecBuf);

      SecHdrVec.push_back(SecHdr);
    }

    // This provides quick access to the section header string table index.
    // We also here handle the unlikely even where the section index overflows
    // and it's just a pointer to secondary storage (SHN_XINDEX).
    uint32_t SecIdx = Hdr->e_shstrndx;
    if (SecIdx == llvm::ELF::SHN_XINDEX) {
      assert(!SecHdrVec.empty() && "malformed ELF object");
      SecIdx = SecHdrVec[0]->sh_link;
    }

    assert(SecIdx < SecHdrVec.size() && "malformed ELF object");

    const typename T::Section *SecHdrStrTab = SecHdrVec[SecIdx];
    typename T::Offset StrTabOffset = SecHdrStrTab->sh_offset;
    typename T::Size StrTabSize = SecHdrStrTab->sh_size;

    auto StrTabBuf = readData(StrTabOffset, StrTabSize);
    if (!StrTabBuf)
      return false;
    auto StrTab = reinterpret_cast<const char *>(StrTabBuf);
    bool Error = false;
    auto findELFSectionByName =
        [&](llvm::StringRef Name) -> std::pair<RemoteRef<void>, uint64_t> {
          if (Error)
            return {nullptr, 0};
          // Now for all the sections, find their name.
          for (const typename T::Section *Hdr : SecHdrVec) {
            uint32_t Offset = Hdr->sh_name;
            const char *Start = (const char *)StrTab + Offset;
            uint64_t StringSize = strnlen(Start, StrTabSize - Offset);
            if (StringSize > StrTabSize - Offset) {
              Error = true;
              break;
            }
            std::string SecName(Start, StringSize);
            if (SecName != Name)
              continue;
            RemoteAddress SecStart =
                RemoteAddress(ImageStart.getAddressData() + Hdr->sh_addr);
            auto SecSize = Hdr->sh_size;
            MemoryReader::ReadBytesResult SecBuf;
            if (FileBuffer.has_value()) {
              // sh_offset gives us the offset to the section in the file,
              // while sh_addr gives us the offset in the process.
              auto Offset = Hdr->sh_offset;
              if (FileBuffer->allocatedSize() < Offset + SecSize) {
                Error = true;
                break;
              }
              auto *Buf = malloc(SecSize);
              SecBuf = MemoryReader::ReadBytesResult(
                  Buf, [](const void *ptr) { free(const_cast<void *>(ptr)); });
              memcpy((void *)Buf,
                     (const void *)((uint64_t)FileBuffer->base() + Offset),
                     SecSize);
            } else {
              SecBuf = this->getReader().readBytes(SecStart, SecSize);
            }
            if (!SecBuf)
              return {nullptr, 0};
            auto SecContents =
                RemoteRef<void>(SecStart.getAddressData(), SecBuf.get());
            savedBuffers.push_back(std::move(SecBuf));
            return {SecContents, SecSize};
          }
          return {nullptr, 0};
        };

    SwiftObjectFileFormatELF ObjectFileFormat;
    auto FieldMdSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::fieldmd));
    auto AssocTySec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::assocty));
    auto BuiltinTySec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::builtin));
    auto CaptureSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::capture));
    auto TypeRefMdSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::typeref));
    auto ReflStrMdSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::reflstr));
    auto ConformMdSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::conform));
    auto MPEnumMdSec = findELFSectionByName(
        ObjectFileFormat.getSectionName(ReflectionSectionKind::mpenum));

    if (Error)
      return false;

    // We succeed if at least one of the sections is present in the
    // ELF executable.
    if (FieldMdSec.first == nullptr &&
        AssocTySec.first == nullptr &&
        BuiltinTySec.first == nullptr &&
        CaptureSec.first == nullptr &&
        TypeRefMdSec.first == nullptr &&
        ReflStrMdSec.first == nullptr &&
        ConformMdSec.first == nullptr &&
        MPEnumMdSec.first == nullptr)
      return false;

    ReflectionInfo info = {{FieldMdSec.first, FieldMdSec.second},
                           {AssocTySec.first, AssocTySec.second},
                           {BuiltinTySec.first, BuiltinTySec.second},
                           {CaptureSec.first, CaptureSec.second},
                           {TypeRefMdSec.first, TypeRefMdSec.second},
                           {ReflStrMdSec.first, ReflStrMdSec.second},
                           {ConformMdSec.first, ConformMdSec.second},
                           {MPEnumMdSec.first, MPEnumMdSec.second},
                           PotentialModuleNames};

    return this->addReflectionInfo(info);
  }

  /// Parses metadata information from an ELF image. Because the Section
  /// Header Table maybe be missing (for example, when reading from a
  /// process) this method optionally receives a buffer with the contents
  /// of the image's file, from where it will the necessary information.
  ///
  ///
  /// \param[in] ImageStart
  ///     A remote address pointing to the start of the image in the running
  ///     process.
  ///
  /// \param[in] FileBuffer
  ///     A buffer which contains the contents of the image's file
  ///     in disk. If missing, all the information will be read using the
  ///     instance's memory reader.
  ///
  /// \return
  ///     \b  The newly added reflection info ID if successful,
  ///     \b llvm::None otherwise.
  llvm::Optional<uint32_t>
  readELF(RemoteAddress ImageStart,
          llvm::Optional<llvm::sys::MemoryBlock> FileBuffer,
          llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    auto Buf =
        this->getReader().readBytes(ImageStart, sizeof(llvm::ELF::Elf64_Ehdr));
    if (!Buf)
      return llvm::None;

    // Read the header.
    auto Hdr = reinterpret_cast<const llvm::ELF::Elf64_Ehdr *>(Buf.get());

    if (!Hdr->checkMagic())
      return llvm::None;

    // Check if we have a ELFCLASS32 or ELFCLASS64
    unsigned char FileClass = Hdr->getFileClass();
    if (FileClass == llvm::ELF::ELFCLASS64) {
      return readELFSections<ELFTraits<llvm::ELF::ELFCLASS64>>(
          ImageStart, FileBuffer, PotentialModuleNames);
    } else if (FileClass == llvm::ELF::ELFCLASS32) {
      return readELFSections<ELFTraits<llvm::ELF::ELFCLASS32>>(
          ImageStart, FileBuffer, PotentialModuleNames);
    } else {
      return llvm::None;
    }
  }

  /// On success returns the ID of the newly registered Reflection Info.
  llvm::Optional<uint32_t>
  addImage(RemoteAddress ImageStart,
           llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    // Read the first few bytes to look for a magic header.
    auto Magic = this->getReader().readBytes(ImageStart, sizeof(uint32_t));
    if (!Magic)
      return false;

    uint32_t MagicWord;
    memcpy(&MagicWord, Magic.get(), sizeof(MagicWord));

    // 32- and 64-bit Mach-O.
    if (MagicWord == llvm::MachO::MH_MAGIC) {
      return readMachOSections<MachOTraits<4>>(ImageStart, PotentialModuleNames);
    }

    if (MagicWord == llvm::MachO::MH_MAGIC_64) {
      return readMachOSections<MachOTraits<8>>(ImageStart, PotentialModuleNames);
    }

    // PE. (This just checks for the DOS header; `readPECOFF` will further
    // validate the existence of the PE header.)
    auto MagicBytes = (const char*)Magic.get();
    if (MagicBytes[0] == 'M' && MagicBytes[1] == 'Z') {
      return readPECOFF(ImageStart, PotentialModuleNames);
    }


    // ELF.
    if (MagicBytes[0] == llvm::ELF::ElfMagic[0]
        && MagicBytes[1] == llvm::ELF::ElfMagic[1]
        && MagicBytes[2] == llvm::ELF::ElfMagic[2]
        && MagicBytes[3] == llvm::ELF::ElfMagic[3]) {
      return readELF(ImageStart, llvm::Optional<llvm::sys::MemoryBlock>(),
                     PotentialModuleNames);
    }

    // We don't recognize the format.
    return llvm::None;
  }

  /// Adds an image using the FindSection closure to find the swift metadata
  /// sections. \param FindSection
  ///     Closure that finds sections by name. ReflectionContext is in charge
  ///     of freeing the memory buffer in the RemoteRef return value.
  ///     process.
  /// \return
  ///     \b  The newly added reflection info ID if successful,
  ///     \b llvm::None otherwise.
  llvm::Optional<uint32_t>
  addImage(llvm::function_ref<
               std::pair<RemoteRef<void>, uint64_t>(ReflectionSectionKind)>
               FindSection,
           llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames = {}) {
    auto Sections = {
        ReflectionSectionKind::fieldmd, ReflectionSectionKind::assocty,
        ReflectionSectionKind::builtin, ReflectionSectionKind::capture,
        ReflectionSectionKind::typeref, ReflectionSectionKind::reflstr,
        ReflectionSectionKind::conform, ReflectionSectionKind::mpenum};

    llvm::SmallVector<std::pair<RemoteRef<void>, uint64_t>, 6> Pairs;
    for (auto Section : Sections) {
      Pairs.push_back(FindSection(Section));
      auto LatestRemoteRef = std::get<RemoteRef<void>>(Pairs.back());
      if (LatestRemoteRef) {
        MemoryReader::ReadBytesResult Buffer(
            LatestRemoteRef.getLocalBuffer(),
            [](const void *Ptr) { free(const_cast<void *>(Ptr)); });

        savedBuffers.push_back(std::move(Buffer));
      }
    }

    // If we didn't find any sections, return.
    if (llvm::all_of(Pairs, [](const auto &Pair) { return !Pair.first; }))
      return {};

    ReflectionInfo Info = {{Pairs[0].first, Pairs[0].second},
                           {Pairs[1].first, Pairs[1].second},
                           {Pairs[2].first, Pairs[2].second},
                           {Pairs[3].first, Pairs[3].second},
                           {Pairs[4].first, Pairs[4].second},
                           {Pairs[5].first, Pairs[5].second},
                           {Pairs[6].first, Pairs[6].second},
                           {Pairs[7].first, Pairs[7].second},
                           PotentialModuleNames};
    return addReflectionInfo(Info);
  }

  /// Adds the reflection info and returns it's id.
  uint32_t addReflectionInfo(ReflectionInfo I) {
    return getBuilder().addReflectionInfo(I);
  }

  bool ownsObject(RemoteAddress ObjectAddress) {
    auto MetadataAddress = readMetadataFromInstance(ObjectAddress.getAddressData());
    if (!MetadataAddress)
      return true;
    return ownsAddress(RemoteAddress(*MetadataAddress));
  }

  /// Returns true if the address falls within a registered image.
  bool ownsAddressRaw(RemoteAddress Address) {
    for (auto Range : imageRanges) {
      auto Start = std::get<0>(Range);
      auto End = std::get<1>(Range);
      if (Start.getAddressData() <= Address.getAddressData()
          && Address.getAddressData() < End.getAddressData())
        return true;
    }

    return false;
  }

  /// Returns true if the address is known to the reflection context.
  /// Currently, that means that either the address falls within a registered
  /// image, or the address points to a Metadata whose type context descriptor
  /// is within a registered image.
  bool ownsAddress(RemoteAddress Address) {
    if (ownsAddressRaw(Address))
      return true;

    // This is usually called on a Metadata address which might have been
    // on the heap. Try reading it and looking up its type context descriptor
    // instead.
    if (auto Metadata = readMetadata(Address.getAddressData()))
      if (auto DescriptorAddress =
          super::readAddressOfNominalTypeDescriptor(Metadata, true))
        if (ownsAddressRaw(RemoteAddress(DescriptorAddress)))
          return true;

    return false;
  }

  /// Returns the address of the nominal type descriptor given a metadata
  /// address.
  StoredPointer nominalTypeDescriptorFromMetadata(StoredPointer MetadataAddress) {
    auto Metadata = readMetadata(MetadataAddress);
    if (!Metadata)
      return 0;
    return super::readAddressOfNominalTypeDescriptor(Metadata, true);
  }

  /// Return a description of the layout of a class instance with the given
  /// metadata as its isa pointer.
  const RecordTypeInfo *
  getMetadataTypeInfo(StoredPointer MetadataAddress,
                      remote::TypeInfoProvider *ExternalTypeInfo) {
    // See if we cached the layout already
    auto found = Cache.find(MetadataAddress);
    if (found != Cache.end())
      return found->second;

    auto &TC = getBuilder().getTypeConverter();

    const RecordTypeInfo *TI = nullptr;

    auto TR = readTypeFromMetadata(MetadataAddress);
    auto kind = this->readKindFromMetadata(MetadataAddress);
    if (TR != nullptr && kind) {
      switch (*kind) {
      case MetadataKind::Class: {
        // Figure out where the stored properties of this class begin
        // by looking at the size of the superclass
        auto start =
            this->readInstanceStartAndAlignmentFromClassMetadata(MetadataAddress);

        // Perform layout
        if (start)
          TI = TC.getClassInstanceTypeInfo(TR, *start, ExternalTypeInfo);

        break;
      }
      default:
        break;
      }
    }

    // Cache the result for future lookups
    Cache[MetadataAddress] = TI;
    return TI;
  }

  /// Return a description of the layout of a class instance with the given
  /// metadata as its isa pointer.
  const TypeInfo *
  getInstanceTypeInfo(StoredPointer ObjectAddress,
                      remote::TypeInfoProvider *ExternalTypeInfo) {
    auto MetadataAddress = readMetadataFromInstance(ObjectAddress);
    if (!MetadataAddress)
      return nullptr;

    auto kind = this->readKindFromMetadata(*MetadataAddress);
    if (!kind)
      return nullptr;

    switch (*kind) {
    case MetadataKind::Class:
      return getMetadataTypeInfo(*MetadataAddress, ExternalTypeInfo);

    case MetadataKind::HeapLocalVariable: {
      auto CDAddr = this->readCaptureDescriptorFromMetadata(*MetadataAddress);
      if (!CDAddr)
        return nullptr;
      if (!CDAddr->isResolved())
        return nullptr;

      // FIXME: Non-generic SIL boxes also use the HeapLocalVariable metadata
      // kind, but with a null capture descriptor right now (see
      // FixedBoxTypeInfoBase::allocate).
      //
      // Non-generic SIL boxes share metadata among types with compatible
      // layout, but we need some way to get an outgoing pointer map for them.
      auto CD = getBuilder().getCaptureDescriptor(
                                CDAddr->getResolvedAddress().getAddressData());
      if (CD == nullptr)
        return nullptr;

      auto Info = getBuilder().getClosureContextInfo(CD);

      return getClosureContextInfo(ObjectAddress, Info, ExternalTypeInfo);
    }

    case MetadataKind::HeapGenericLocalVariable: {
      // Generic SIL @box type - there is always an instantiated metadata
      // pointer for the boxed type.
      if (auto Meta = readMetadata(*MetadataAddress)) {
        auto GenericHeapMeta =
          cast<TargetGenericBoxHeapMetadata<Runtime>>(Meta.getLocalBuffer());
        return getMetadataTypeInfo(GenericHeapMeta->BoxedType,
                                   ExternalTypeInfo);
      }
      return nullptr;
    }

    case MetadataKind::ErrorObject:
      // Error boxed existential on non-Objective-C runtime target
      return nullptr;

    default:
      return nullptr;
    }
  }

  llvm::Optional<std::pair<const TypeRef *, RemoteAddress>>
  getDynamicTypeAndAddressClassExistential(RemoteAddress ExistentialAddress) {
    auto PointerValue =
        readResolvedPointerValue(ExistentialAddress.getAddressData());
    if (!PointerValue)
      return {};
    auto Result = readMetadataFromInstance(*PointerValue);
    if (!Result)
      return {};
    auto TypeResult = readTypeFromMetadata(Result.value());
    if (!TypeResult)
      return {};
    return {{std::move(TypeResult), RemoteAddress(*PointerValue)}};
  }

  llvm::Optional<std::pair<const TypeRef *, RemoteAddress>>
  getDynamicTypeAndAddressErrorExistential(RemoteAddress ExistentialAddress,
                                           bool *IsBridgedError = nullptr) {
    auto Result = readMetadataAndValueErrorExistential(ExistentialAddress);
    if (!Result)
      return {};

    auto TypeResult =
        readTypeFromMetadata(Result->MetadataAddress.getAddressData());
    if (!TypeResult)
      return {};

    if (IsBridgedError)
      *IsBridgedError = Result->IsBridgedError;

    return {{TypeResult, Result->PayloadAddress}};
  }

  llvm::Optional<std::pair<const TypeRef *, RemoteAddress>>
  getDynamicTypeAndAddressOpaqueExistential(RemoteAddress ExistentialAddress) {
    auto Result = readMetadataAndValueOpaqueExistential(ExistentialAddress);
    if (!Result)
      return {};

    auto TypeResult =
        readTypeFromMetadata(Result->MetadataAddress.getAddressData());
    if (!TypeResult)
      return {};
    return {{std::move(TypeResult), Result->PayloadAddress}};
  }

  bool projectExistential(RemoteAddress ExistentialAddress,
                          const TypeRef *ExistentialTR,
                          const TypeRef **OutInstanceTR,
                          RemoteAddress *OutInstanceAddress,
                          remote::TypeInfoProvider *ExternalTypeInfo) {
    if (ExistentialTR == nullptr)
      return false;

    auto ExistentialTI = getTypeInfo(ExistentialTR, ExternalTypeInfo);
    if (ExistentialTI == nullptr)
      return false;

    auto ExistentialRecordTI = dyn_cast<const RecordTypeInfo>(ExistentialTI);
    if (ExistentialRecordTI == nullptr)
      return false;

    switch (ExistentialRecordTI->getRecordKind()) {
    // Class existentials have trivial layout.
    // It is itself the pointer to the instance followed by the witness tables.
    case RecordKind::ClassExistential:
      // This is just AnyObject.
      *OutInstanceTR = ExistentialRecordTI->getFields()[0].TR;
      *OutInstanceAddress = ExistentialAddress;
      return true;

    case RecordKind::OpaqueExistential: {
      auto OptMetaAndValue =
          readMetadataAndValueOpaqueExistential(ExistentialAddress);
      if (!OptMetaAndValue)
        return false;

      auto InstanceTR = readTypeFromMetadata(
          OptMetaAndValue->MetadataAddress.getAddressData());
      if (!InstanceTR)
        return false;

      *OutInstanceTR = InstanceTR;
      *OutInstanceAddress = OptMetaAndValue->PayloadAddress;
      return true;
    }
    case RecordKind::ErrorExistential: {
      auto OptMetaAndValue =
          readMetadataAndValueErrorExistential(ExistentialAddress);
      if (!OptMetaAndValue)
        return false;

      // FIXME: Check third value, 'IsBridgedError'

      auto InstanceTR = readTypeFromMetadata(
          OptMetaAndValue->MetadataAddress.getAddressData());
      if (!InstanceTR)
        return false;

      *OutInstanceTR = InstanceTR;
      *OutInstanceAddress = OptMetaAndValue->PayloadAddress;
      return true;
    }
    default:
      return false;
    }
  }
  /// A version of `projectExistential` tailored for LLDB.
  /// This version dereferences the resulting TypeRef if it wraps
  /// a class type, it also dereferences the input `ExistentialAddress` before
  /// attempting to find its dynamic type and address when dealing with error
  /// existentials.
  llvm::Optional<std::pair<const TypeRef *, RemoteAddress>>
  projectExistentialAndUnwrapClass(RemoteAddress ExistentialAddress,
                                   const TypeRef &ExistentialTR) {
    auto IsClass = [](const TypeRef *TypeResult) {
      // When the existential wraps a class type, LLDB expects that the
      // address returned is the class instance itself and not the address
      // of the reference.
      bool IsClass = TypeResult->getKind() == TypeRefKind::ForeignClass ||
                     TypeResult->getKind() == TypeRefKind::ObjCClass;
      if (auto *nominal = llvm::dyn_cast<NominalTypeRef>(TypeResult))
        IsClass = nominal->isClass();
      else if (auto *boundGeneric =
                   llvm::dyn_cast<BoundGenericTypeRef>(TypeResult))
        IsClass = boundGeneric->isClass();
      return IsClass;
    };

    auto DereferenceAndSet = [&](RemoteAddress &Address) {
      auto PointerValue = readResolvedPointerValue(Address.getAddressData());
      if (!PointerValue)
        return false;
      Address = RemoteAddress(*PointerValue);
      return true;
    };

    auto ExistentialRecordTI = getRecordTypeInfo(&ExistentialTR, nullptr);
    if (!ExistentialRecordTI)
      return {};

    switch (ExistentialRecordTI->getRecordKind()) {
    case RecordKind::ClassExistential:
      return getDynamicTypeAndAddressClassExistential(ExistentialAddress);
    case RecordKind::ErrorExistential: {
      // LLDB stores the address of the error pointer.
      if (!DereferenceAndSet(ExistentialAddress))
        return {};

      bool IsBridgedError = false;
      auto Pair = getDynamicTypeAndAddressErrorExistential(ExistentialAddress,
                                                           &IsBridgedError);
      if (!Pair)
        return {};

      if (!IsBridgedError && IsClass(std::get<const TypeRef *>(*Pair)))
        if (!DereferenceAndSet(std::get<RemoteAddress>(*Pair)))
          return {};

      return Pair;
    }
    case RecordKind::OpaqueExistential: {
      auto Pair = getDynamicTypeAndAddressOpaqueExistential(ExistentialAddress);
      if (!Pair)
        return {};

      if (IsClass(std::get<const TypeRef *>(*Pair)))
        if (!DereferenceAndSet(std::get<RemoteAddress>(*Pair)))
          return {};

      return Pair;
    }
    default:
      return {};
    }
  }

  /// Projects the value of an enum.
  ///
  /// Takes the address and typeref for an enum and determines the
  /// index of the currently-selected case within the enum.
  /// You can use this index with `swift_reflection_childOfTypeRef`
  /// to get detailed information about the specific case.
  ///
  /// Returns true if the enum case could be successfully determined.  In
  /// particular, note that this code may return false for valid in-memory data
  /// if the compiler used a strategy we do not yet understand.
  bool projectEnumValue(RemoteAddress EnumAddress, const TypeRef *EnumTR,
                        int *CaseIndex,
                        remote::TypeInfoProvider *ExternalTypeInfo) {
    // Get the TypeInfo and sanity-check it
    if (EnumTR == nullptr) {
      return false;
    }
    auto TI = getTypeInfo(EnumTR, ExternalTypeInfo);
    if (TI == nullptr) {
      return false;
    }
    auto EnumTI = dyn_cast<const EnumTypeInfo>(TI);
    if (EnumTI == nullptr){
      return false;
    }
    return EnumTI->projectEnumValue(getReader(), EnumAddress, CaseIndex);
  }

  /// Return a description of the layout of a value with the given type.
  const TypeInfo *getTypeInfo(const TypeRef *TR,
                              remote::TypeInfoProvider *ExternalTypeInfo) {
    if (TR == nullptr) {
      return nullptr;
    } else {
      return getBuilder().getTypeConverter().getTypeInfo(TR, ExternalTypeInfo);
    }
  }

  const RecordTypeInfo *getRecordTypeInfo(const TypeRef *TR,
                              remote::TypeInfoProvider *ExternalTypeInfo) {
    auto *TypeInfo = getTypeInfo(TR, ExternalTypeInfo);
    return dyn_cast_or_null<const RecordTypeInfo>(TypeInfo);
  }

  bool metadataIsActor(StoredPointer MetadataAddress) {
    auto Metadata = readMetadata(MetadataAddress);
    if (!Metadata)
      return false;

    // Only classes can be actors.
    if (Metadata->getKind() != MetadataKind::Class)
      return false;

    auto DescriptorAddress =
        super::readAddressOfNominalTypeDescriptor(Metadata);
    if (!DescriptorAddress)
      return false;

    auto DescriptorBytes =
        getReader().readBytes(RemoteAddress(DescriptorAddress),
                              sizeof(TargetTypeContextDescriptor<Runtime>));
    if (!DescriptorBytes)
      return false;
    auto Descriptor =
        reinterpret_cast<const TargetTypeContextDescriptor<Runtime> *>(
            DescriptorBytes.get());
    return Descriptor->getTypeContextDescriptorFlags().class_isActor();
  }

  /// Iterate the protocol conformance cache tree rooted at NodePtr, calling
  /// Call with the type and protocol in each node.
  void iterateConformanceTree(StoredPointer NodePtr,
    std::function<void(StoredPointer Type, StoredPointer Proto)> Call) {
    if (!NodePtr)
      return;
    auto NodeBytes = getReader().readBytes(RemoteAddress(NodePtr),
                                           sizeof(ConformanceNode<Runtime>));
    if (!NodeBytes)
      return;
    auto NodeData =
      reinterpret_cast<const ConformanceNode<Runtime> *>(NodeBytes.get());
    Call(NodeData->Type, NodeData->Proto);
    iterateConformanceTree(NodeData->Left, Call);
    iterateConformanceTree(NodeData->Right, Call);
  }

  void IterateConformanceTable(
      RemoteAddress ConformancesPtr,
      std::function<void(StoredPointer Type, StoredPointer Proto)> Call) {
    auto MapBytes = getReader().readBytes(RemoteAddress(ConformancesPtr),
                                          sizeof(ConcurrentHashMap<Runtime>));
    if (!MapBytes)
      return;
    auto MapData =
        reinterpret_cast<const ConcurrentHashMap<Runtime> *>(MapBytes.get());

    auto Count = MapData->ElementCount;
    auto Size = Count * sizeof(ConformanceCacheEntry<Runtime>) + sizeof(StoredPointer);

    auto ElementsBytes =
        getReader().readBytes(RemoteAddress(MapData->Elements), Size);
    if (!ElementsBytes)
      return;
    auto ElementsData =
        reinterpret_cast<const ConformanceCacheEntry<Runtime> *>(
            reinterpret_cast<const char *>(ElementsBytes.get()) + sizeof(StoredPointer));

    for (StoredSize i = 0; i < Count; i++) {
      auto &Element = ElementsData[i];
      Call(Element.Type, Element.Proto);
    }
  }

  /// Iterate the protocol conformance cache in the target process, calling Call
  /// with the type and protocol of each conformance. Returns None on success,
  /// and a string describing the error on failure.
  llvm::Optional<std::string> iterateConformances(
    std::function<void(StoredPointer Type, StoredPointer Proto)> Call) {
    std::string ConformancesPointerName =
        "_swift_debug_protocolConformanceStatePointer";
    auto ConformancesAddrAddr =
      getReader().getSymbolAddress(ConformancesPointerName);
    if (!ConformancesAddrAddr)
      return "unable to look up debug variable " + ConformancesPointerName;

    auto ConformancesAddr =
      getReader().readPointer(ConformancesAddrAddr, sizeof(StoredPointer));
    if (!ConformancesAddr)
      return "unable to read value of " + ConformancesPointerName;

    IterateConformanceTable(ConformancesAddr->getResolvedAddress(), Call);
    return llvm::None;
  }

  /// Fetch the metadata pointer from a metadata allocation, or 0 if this
  /// allocation's tag is not handled or an error occurred.
  StoredPointer allocationMetadataPointer(
    MetadataAllocation<Runtime> Allocation) {
    if (Allocation.Tag == GenericMetadataCacheTag) {
        auto AllocationBytes =
          getReader().readBytes(RemoteAddress(Allocation.Ptr),
                                              Allocation.Size);
        if (!AllocationBytes)
          return 0;
        auto Entry =
          reinterpret_cast<const GenericMetadataCacheEntry<StoredPointer> *>(
            AllocationBytes.get());
        return Entry->Value;
    }
    return 0;
  }

  /// Get the name of a metadata tag, if known.
  llvm::Optional<std::string> metadataAllocationTagName(int Tag) {
    switch (Tag) {
#define TAG(name, value)                                                       \
  case value:                                                                  \
    return std::string(#name);
#include "../../../stdlib/public/runtime/MetadataAllocatorTags.def"
    default:
      return llvm::None;
    }
  }

  llvm::Optional<MetadataCacheNode<Runtime>>
  metadataAllocationCacheNode(MetadataAllocation<Runtime> Allocation) {
    switch (Allocation.Tag) {
    case BoxesTag:
    case ObjCClassWrappersTag:
    case FunctionTypesTag:
    case MetatypeTypesTag:
    case ExistentialMetatypeValueWitnessTablesTag:
    case ExistentialMetatypesTag:
    case ExistentialTypesTag:
    case ExtendedExistentialTypesTag:
    case ExtendedExistentialTypeShapesTag:
    case OpaqueExistentialValueWitnessTablesTag:
    case ClassExistentialValueWitnessTablesTag:
    case ForeignWitnessTablesTag:
    case TupleCacheTag:
    case GenericMetadataCacheTag:
    case ForeignMetadataCacheTag:
    case GenericWitnessTableCacheTag: {
      auto NodeBytes = getReader().readBytes(
          RemoteAddress(Allocation.Ptr), sizeof(MetadataCacheNode<Runtime>));
      if (!NodeBytes)
        return llvm::None;
      auto Node =
          reinterpret_cast<const MetadataCacheNode<Runtime> *>(NodeBytes.get());
      return *Node;
    }
    default:
      return llvm::None;
    }
  }

  /// Iterate the metadata allocations in the target process, calling Call with
  /// each allocation found. Returns None on success, and a string describing
  /// the error on failure.
  llvm::Optional<std::string> iterateMetadataAllocations(
    std::function<void (MetadataAllocation<Runtime>)> Call) {
    std::string IterationEnabledName =
        "_swift_debug_metadataAllocationIterationEnabled";
    std::string AllocationPoolPointerName =
        "_swift_debug_allocationPoolPointer";

    auto IterationEnabledAddr =
      getReader().getSymbolAddress(IterationEnabledName);
    if (!IterationEnabledAddr)
      return "unable to look up debug variable " + IterationEnabledName;
    char IterationEnabled;
    if (!getReader().readInteger(IterationEnabledAddr, &IterationEnabled))
      return "failed to read value of " + IterationEnabledName;
    if (!IterationEnabled)
      return std::string("remote process does not have metadata allocation "
                         "iteration enabled");

    auto AllocationPoolAddrAddr =
      getReader().getSymbolAddress(AllocationPoolPointerName);
    if (!AllocationPoolAddrAddr)
      return "unable to look up debug variable " + AllocationPoolPointerName;
    auto AllocationPoolAddr =
      getReader().readPointer(AllocationPoolAddrAddr, sizeof(StoredPointer));
    if (!AllocationPoolAddr)
      return "failed to read value of " + AllocationPoolPointerName;

    struct PoolRange {
      StoredPointer Begin;
      StoredSize Remaining;
    };
    struct PoolTrailer {
      StoredPointer PrevTrailer;
      StoredSize PoolSize;
    };
    struct alignas(StoredPointer) AllocationHeader {
      uint16_t Size;
      uint16_t Tag;
    };

    auto PoolBytes = getReader()
      .readBytes(AllocationPoolAddr->getResolvedAddress(), sizeof(PoolRange));
    if (!PoolBytes)
      return std::string("failure reading allocation pool contents");
    auto Pool = reinterpret_cast<const PoolRange *>(PoolBytes.get());

    // Limit how many iterations of this loop we'll do, to avoid potential
    // infinite loops when reading bad data. Limit to 1 million iterations. In
    // normal operation, each pool allocation is 16kB, so that would be ~16GB of
    // metadata which is far more than any normal program should have.
    unsigned LoopCount = 0;
    unsigned LoopLimit = 1000000;

    auto TrailerPtr = Pool->Begin + Pool->Remaining;
    while (TrailerPtr && LoopCount++ < LoopLimit) {
      auto TrailerBytes = getReader()
        .readBytes(RemoteAddress(TrailerPtr), sizeof(PoolTrailer));
      if (!TrailerBytes)
        break;
      auto Trailer = reinterpret_cast<const PoolTrailer *>(TrailerBytes.get());
      auto PoolStart = TrailerPtr - Trailer->PoolSize;
      auto PoolBytes = getReader()
        .readBytes(RemoteAddress(PoolStart), Trailer->PoolSize);
      if (!PoolBytes)
        break;
      auto PoolPtr = (const char *)PoolBytes.get();

      uintptr_t Offset = 0;
      while (Offset < Trailer->PoolSize) {
        auto AllocationPtr = PoolPtr + Offset;
        auto Header = (const AllocationHeader *)AllocationPtr;
        if (Header->Size == 0)
          break;
        auto RemoteAddr = PoolStart + Offset + sizeof(AllocationHeader);
        MetadataAllocation<Runtime> Allocation;
        Allocation.Tag = Header->Tag;
        Allocation.Ptr = RemoteAddr;
        Allocation.Size = Header->Size;
        Call(Allocation);

        Offset += sizeof(AllocationHeader) + Header->Size;
      }

      TrailerPtr = Trailer->PrevTrailer;
    }
    return llvm::None;
  }

  llvm::Optional<std::string> iterateMetadataAllocationBacktraces(
      std::function<void(StoredPointer, uint32_t, const StoredPointer *)>
          Call) {
    std::string BacktraceListName =
        "_swift_debug_metadataAllocationBacktraceList";

    auto BacktraceListAddr = getReader().getSymbolAddress(BacktraceListName);
    if (!BacktraceListAddr)
      return "unable to look up debug variable " + BacktraceListName;
    auto BacktraceListNextPtr =
        getReader().readPointer(BacktraceListAddr, sizeof(StoredPointer));
    if (!BacktraceListNextPtr)
      return llvm::None;

    // Limit how many iterations of this loop we'll do, to avoid potential
    // infinite loops when reading bad data. Limit to 1 billion iterations. In
    // normal operation, a program shouldn't have anywhere near 1 billion
    // metadata allocations.
    unsigned LoopCount = 0;
    unsigned LoopLimit = 1000000000;

    auto BacktraceListNext = BacktraceListNextPtr->getResolvedAddress();
    while (BacktraceListNext && LoopCount++ < LoopLimit) {
      auto HeaderBytes = getReader().readBytes(
          RemoteAddress(BacktraceListNext),
          sizeof(MetadataAllocationBacktraceHeader<Runtime>));
      if (!HeaderBytes) {
        // FIXME: std::stringstream would be better, but LLVM's standard library
        // introduces a vtable and we don't want that.
        char result[128];
        std::snprintf(result, sizeof(result),
            "unable to read Next pointer %#" PRIx64,
            BacktraceListNext.getAddressData());
        return std::string(result);
      }
      auto HeaderPtr =
          reinterpret_cast<const MetadataAllocationBacktraceHeader<Runtime> *>(
              HeaderBytes.get());
      auto BacktraceAddrPtr =
          BacktraceListNext +
          sizeof(MetadataAllocationBacktraceHeader<Runtime>);
      auto BacktraceBytes =
          getReader().readBytes(RemoteAddress(BacktraceAddrPtr),
                                HeaderPtr->Count * sizeof(StoredPointer));
      auto BacktracePtr =
          reinterpret_cast<const StoredPointer *>(BacktraceBytes.get());

      Call(HeaderPtr->Allocation, HeaderPtr->Count, BacktracePtr);

      BacktraceListNext = RemoteAddress(HeaderPtr->Next);
    }
    return llvm::None;
  }

  std::pair<llvm::Optional<std::string>, AsyncTaskSlabInfo>
  asyncTaskSlabAllocations(StoredPointer SlabPtr) {
    using StackAllocator = StackAllocator<Runtime>;
    auto SlabBytes = getReader().readBytes(
        RemoteAddress(SlabPtr), sizeof(typename StackAllocator::Slab));
    auto Slab = reinterpret_cast<const typename StackAllocator::Slab *>(
        SlabBytes.get());
    if (!Slab)
      return {std::string("failure reading slab"), {}};

    // For now, we won't try to walk the allocations in the slab, we'll just
    // provide the whole thing as one big chunk.
    size_t HeaderSize =
        llvm::alignTo(sizeof(*Slab), llvm::Align(MaximumAlignment));

    AsyncTaskAllocationChunk AllocatedSpaceChunk;
    AllocatedSpaceChunk.Start = SlabPtr + HeaderSize;
    AllocatedSpaceChunk.Length = Slab->CurrentOffset;
    AllocatedSpaceChunk.Kind = AsyncTaskAllocationChunk::ChunkKind::Unknown;

    // Provide a second chunk just for the Next pointer, so the client knows
    // that there's an allocation there.
    AsyncTaskAllocationChunk NextPtrChunk;
    NextPtrChunk.Start =
        SlabPtr + offsetof(typename StackAllocator::Slab, Next);
    NextPtrChunk.Length = sizeof(Slab->Next);
    NextPtrChunk.Kind = AsyncTaskAllocationChunk::ChunkKind::RawPointer;

    // Total slab size is the slab's capacity plus the header.
    StoredPointer SlabSize = Slab->Capacity + HeaderSize;

    return {llvm::None,
            {Slab->Next, SlabSize, {NextPtrChunk, AllocatedSpaceChunk}}};
  }

  std::pair<llvm::Optional<std::string>, AsyncTaskInfo>
  asyncTaskInfo(StoredPointer AsyncTaskPtr, unsigned ChildTaskLimit,
                unsigned AsyncBacktraceLimit) {
    loadTargetPointers();

    if (supportsPriorityEscalation)
      return asyncTaskInfo<
          AsyncTask<Runtime, ActiveTaskStatusWithEscalation<Runtime>>>(
          AsyncTaskPtr, ChildTaskLimit, AsyncBacktraceLimit);
    else
      return asyncTaskInfo<
          AsyncTask<Runtime, ActiveTaskStatusWithoutEscalation<Runtime>>>(
          AsyncTaskPtr, ChildTaskLimit, AsyncBacktraceLimit);
  }

  std::pair<llvm::Optional<std::string>, ActorInfo>
  actorInfo(StoredPointer ActorPtr) {
    if (supportsPriorityEscalation)
      return actorInfo<
          DefaultActorImpl<Runtime, ActiveActorStatusWithEscalation<Runtime>>>(
          ActorPtr);
    else
      return actorInfo<DefaultActorImpl<
          Runtime, ActiveActorStatusWithoutEscalation<Runtime>>>(ActorPtr);
  }

  StoredPointer nextJob(StoredPointer JobPtr) {
    using Job = Job<Runtime>;

    auto JobBytes = getReader().readBytes(RemoteAddress(JobPtr), sizeof(Job));
    auto *JobObj = reinterpret_cast<const Job *>(JobBytes.get());
    if (!JobObj)
      return 0;

    // This is a JobRef which stores flags in the low bits.
    return JobObj->SchedulerPrivate[0] & ~StoredPointer(0x3);
  }

private:
  void setIsRunning(
      AsyncTaskInfo &Info,
      const AsyncTask<Runtime, ActiveTaskStatusWithEscalation<Runtime>> *Task) {
#if HAS_DISPATCH_LOCK_IS_LOCKED
    Info.HasIsRunning = true;
    Info.IsRunning =
        dispatch_lock_is_locked(Task->PrivateStorage.Status.ExecutionLock[0]);
#else
    // The target runtime was built with priority escalation but we don't have
    // the swift_concurrency_private.h header needed to decode the running
    // status in the task. Set HasIsRunning to false to indicate that we can't
    // tell whether or not the task is running.
    Info.HasIsRunning = false;
#endif
  }

  void setIsRunning(
      AsyncTaskInfo &Info,
      const AsyncTask<Runtime, ActiveTaskStatusWithoutEscalation<Runtime>>
          *Task) {
    Info.HasIsRunning = true;
    Info.IsRunning =
        Task->PrivateStorage.Status.Flags[0] & ActiveTaskStatusFlags::IsRunning;
  }

  std::pair<bool, uint32_t> getThreadPort(
      const AsyncTask<Runtime, ActiveTaskStatusWithEscalation<Runtime>> *Task) {
#if HAS_DISPATCH_LOCK_IS_LOCKED
    return {true,
            dispatch_lock_owner(Task->PrivateStorage.Status.ExecutionLock[0])};
#else
    // The target runtime was built with priority escalation but we don't have
    // the swift_concurrency_private.h header needed to decode the lock.
    return {false, 0};
#endif
  }

  std::pair<bool, uint32_t> getThreadPort(
      const AsyncTask<Runtime, ActiveTaskStatusWithoutEscalation<Runtime>>
          *Task) {
    // Tasks without escalation have no thread port to query.
    return {false, 0};
  }

  std::pair<bool, uint32_t> getThreadPort(
      const DefaultActorImpl<Runtime, ActiveActorStatusWithEscalation<Runtime>>
          *Actor) {
#if HAS_DISPATCH_LOCK_IS_LOCKED
    return {true, dispatch_lock_owner(Actor->Status.DrainLock[0])};
#else
    // The target runtime was built with priority escalation but we don't have
    // the swift_concurrency_private.h header needed to decode the lock.
    return {false, 0};
#endif
  }

  std::pair<bool, uint32_t>
  getThreadPort(const DefaultActorImpl<
                Runtime, ActiveActorStatusWithoutEscalation<Runtime>> *Actor) {
    // Actors without escalation have no thread port to query.
    return {false, 0};
  }

  template <typename AsyncTaskType>
  std::pair<llvm::Optional<std::string>, AsyncTaskInfo>
  asyncTaskInfo(StoredPointer AsyncTaskPtr, unsigned ChildTaskLimit,
                unsigned AsyncBacktraceLimit) {
    auto AsyncTaskObj = readObj<AsyncTaskType>(AsyncTaskPtr);
    if (!AsyncTaskObj)
      return {std::string("failure reading async task"), {}};

    AsyncTaskInfo Info{};

    swift::JobFlags JobFlags(AsyncTaskObj->Flags);
    Info.Kind = static_cast<unsigned>(JobFlags.getKind());
    Info.EnqueuePriority = static_cast<unsigned>(JobFlags.getPriority());
    Info.IsChildTask = JobFlags.task_isChildTask();
    Info.IsFuture = JobFlags.task_isFuture();
    Info.IsGroupChildTask = JobFlags.task_isGroupChildTask();
    Info.IsAsyncLetTask = JobFlags.task_isAsyncLetTask();

    uint32_t TaskStatusFlags = AsyncTaskObj->PrivateStorage.Status.Flags[0];
    Info.IsCancelled = TaskStatusFlags & ActiveTaskStatusFlags::IsCancelled;
    Info.IsStatusRecordLocked =
        TaskStatusFlags & ActiveTaskStatusFlags::IsStatusRecordLocked;
    Info.IsEscalated = TaskStatusFlags & ActiveTaskStatusFlags::IsEscalated;
    Info.IsEnqueued = TaskStatusFlags & ActiveTaskStatusFlags::IsEnqueued;

    setIsRunning(Info, AsyncTaskObj.get());
    std::tie(Info.HasThreadPort, Info.ThreadPort) =
        getThreadPort(AsyncTaskObj.get());

    Info.Id =
        AsyncTaskObj->Id | ((uint64_t)AsyncTaskObj->PrivateStorage.Id << 32);
    Info.AllocatorSlabPtr = AsyncTaskObj->PrivateStorage.Allocator.FirstSlab;
    Info.RunJob = getRunJob(AsyncTaskObj.get());

    // Find all child tasks.
    unsigned ChildTaskLoopCount = 0;
    auto RecordPtr = AsyncTaskObj->PrivateStorage.Status.Record;
    while (RecordPtr && ChildTaskLoopCount++ < ChildTaskLimit) {
      auto RecordObj = readObj<TaskStatusRecord<Runtime>>(RecordPtr);
      if (!RecordObj)
        break;

      // This cuts off high bits if our size_t doesn't match the target's. We
      // only read the Kind bits which are at the bottom, so that's OK here.
      // Beware of this when reading anything else.
      TaskStatusRecordFlags Flags{RecordObj->Flags};
      auto Kind = Flags.getKind();

      StoredPointer ChildTask = 0;
      if (Kind == TaskStatusRecordKind::ChildTask) {
        auto RecordObj = readObj<ChildTaskStatusRecord<Runtime>>(RecordPtr);
        if (RecordObj)
          ChildTask = RecordObj->FirstChild;
      } else if (Kind == TaskStatusRecordKind::TaskGroup) {
        auto RecordObj = readObj<TaskGroupTaskStatusRecord<Runtime>>(RecordPtr);
        if (RecordObj)
          ChildTask = RecordObj->FirstChild;
      }

      while (ChildTask) {
        Info.ChildTasks.push_back(ChildTask);

        StoredPointer ChildFragmentAddr = ChildTask + sizeof(*AsyncTaskObj);
        auto ChildFragmentObj =
            readObj<ChildFragment<Runtime>>(ChildFragmentAddr);
        if (ChildFragmentObj)
          ChildTask = ChildFragmentObj->NextChild;
        else
          ChildTask = 0;
      }

      RecordPtr = RecordObj->Parent;
    }

    // Walk the async backtrace.
    if (Info.HasIsRunning && !Info.IsRunning) {
      auto ResumeContext = AsyncTaskObj->ResumeContextAndReserved[0];
      unsigned AsyncBacktraceLoopCount = 0;
      while (ResumeContext && AsyncBacktraceLoopCount++ < AsyncBacktraceLimit) {
        auto ResumeContextObj = readObj<AsyncContext<Runtime>>(ResumeContext);
        if (!ResumeContextObj)
          break;
        Info.AsyncBacktraceFrames.push_back(
            stripSignedPointer(ResumeContextObj->ResumeParent));
        ResumeContext = stripSignedPointer(ResumeContextObj->Parent);
      }
    }

    return {llvm::None, Info};
  }

  template <typename ActorType>
  std::pair<llvm::Optional<std::string>, ActorInfo>
  actorInfo(StoredPointer ActorPtr) {
    auto ActorObj = readObj<ActorType>(ActorPtr);
    if (!ActorObj)
      return {std::string("failure reading actor"), {}};

    ActorInfo Info{};

    uint32_t Flags = ActorObj->Status.Flags[0];
    Info.State = Flags & concurrency::ActorFlagConstants::ActorStateMask;
    Info.IsPriorityEscalated =
        Flags & concurrency::ActorFlagConstants::IsPriorityEscalated;
    Info.MaxPriority =
        (Flags & concurrency::ActorFlagConstants::PriorityMask) >>
        concurrency::ActorFlagConstants::PriorityShift;
    Info.IsDistributedRemote = ActorObj->IsDistributedRemote;

    // Don't read FirstJob when idle.
    if (Info.State != concurrency::ActorFlagConstants::Idle) {
      // This is a JobRef which stores flags in the low bits.
      Info.FirstJob = ActorObj->Status.FirstJob & ~StoredPointer(0x3);
    }

    std::tie(Info.HasThreadPort, Info.ThreadPort) =
        getThreadPort(ActorObj.get());

    return {llvm::None, Info};
  }

  // Get the most human meaningful "run job" function pointer from the task,
  // like AsyncTask::getResumeFunctionForLogging does.
  template <typename AsyncTaskType>
  StoredPointer getRunJob(const AsyncTaskType *AsyncTaskObj) {
    auto Fptr = stripSignedPointer(AsyncTaskObj->RunJob);

    loadTargetPointers();
    auto ResumeContextPtr = AsyncTaskObj->ResumeContextAndReserved[0];
    if (target_non_future_adapter && Fptr == target_non_future_adapter) {
      using Prefix = AsyncContextPrefix<Runtime>;
      auto PrefixAddr = ResumeContextPtr - sizeof(Prefix);
      auto PrefixBytes =
          getReader().readBytes(RemoteAddress(PrefixAddr), sizeof(Prefix));
      if (PrefixBytes) {
        auto PrefixPtr = reinterpret_cast<const Prefix *>(PrefixBytes.get());
        return stripSignedPointer(PrefixPtr->AsyncEntryPoint);
      }
    } else if (target_future_adapter && Fptr == target_future_adapter) {
      using Prefix = FutureAsyncContextPrefix<Runtime>;
      auto PrefixAddr = ResumeContextPtr - sizeof(Prefix);
      auto PrefixBytes =
          getReader().readBytes(RemoteAddress(PrefixAddr), sizeof(Prefix));
      if (PrefixBytes) {
        auto PrefixPtr = reinterpret_cast<const Prefix *>(PrefixBytes.get());
        return stripSignedPointer(PrefixPtr->AsyncEntryPoint);
      }
    } else if ((target_task_wait_throwing_resume_adapter &&
                Fptr == target_task_wait_throwing_resume_adapter) ||
               (target_task_future_wait_resume_adapter &&
                Fptr == target_task_future_wait_resume_adapter)) {
      auto ContextBytes = getReader().readBytes(RemoteAddress(ResumeContextPtr),
                                                sizeof(AsyncContext<Runtime>));
      if (ContextBytes) {
        auto ContextPtr =
            reinterpret_cast<const AsyncContext<Runtime> *>(ContextBytes.get());
        return stripSignedPointer(ContextPtr->ResumeParent);
      }
    }

    return Fptr;
  }

  void loadTargetPointers() {
    if (setupTargetPointers)
      return;

    auto getFunc = [&](const std::string &name) -> StoredPointer {
      auto Symbol = getReader().getSymbolAddress(name);
      if (!Symbol)
        return 0;
      auto Pointer = getReader().readPointer(Symbol, sizeof(StoredPointer));
      if (!Pointer)
        return 0;
      return Pointer->getResolvedAddress().getAddressData();
    };
    target_non_future_adapter =
        getFunc("_swift_concurrency_debug_non_future_adapter");
    target_future_adapter = getFunc("_swift_concurrency_debug_future_adapter");
    target_task_wait_throwing_resume_adapter =
        getFunc("_swift_concurrency_debug_task_wait_throwing_resume_adapter");
    target_task_future_wait_resume_adapter =
        getFunc("_swift_concurrency_debug_task_future_wait_resume_adapter");
    auto supportsPriorityEscalationAddr = getReader().getSymbolAddress(
        "_swift_concurrency_debug_supportsPriorityEscalation");
    if (supportsPriorityEscalationAddr) {
      getReader().readInteger(supportsPriorityEscalationAddr,
                              &supportsPriorityEscalation);
    }

    setupTargetPointers = true;
  }

  const TypeInfo *
  getClosureContextInfo(StoredPointer Context, const ClosureContextInfo &Info,
                        remote::TypeInfoProvider *ExternalTypeInfo) {
    RecordTypeInfoBuilder Builder(getBuilder().getTypeConverter(),
                                  RecordKind::ClosureContext);

    auto Metadata = readMetadataFromInstance(Context);
    if (!Metadata)
      return nullptr;

    // Calculate the offset of the first capture.
    // See GenHeap.cpp, buildPrivateMetadata().
    auto OffsetToFirstCapture =
        this->readOffsetToFirstCaptureFromMetadata(*Metadata);
    if (!OffsetToFirstCapture)
      return nullptr;

    // Initialize the builder.
    Builder.addField(*OffsetToFirstCapture,
                     /*alignment=*/sizeof(StoredPointer),
                     /*numExtraInhabitants=*/0,
                     /*bitwiseTakable=*/true);

    // Skip the closure's necessary bindings struct, if it's present.
    auto SizeOfNecessaryBindings = Info.NumBindings * sizeof(StoredPointer);
    Builder.addField(/*size=*/SizeOfNecessaryBindings,
                     /*alignment=*/sizeof(StoredPointer),
                     /*numExtraInhabitants=*/0,
                     /*bitwiseTakable=*/true);

    // FIXME: should be unordered_set but I'm too lazy to write a hash
    // functor
    std::set<std::pair<const TypeRef *, const MetadataSource *>> Done;
    GenericArgumentMap Subs;

    ArrayRef<const TypeRef *> CaptureTypes = Info.CaptureTypes;

    // Closure context element layout depends on the layout of the
    // captured types, but captured types might depend on element
    // layout (of previous elements). Use an iterative approach to
    // solve the problem.
    while (!CaptureTypes.empty()) {
      const TypeRef *OrigCaptureTR = CaptureTypes[0];

      // If we failed to demangle the capture type, we cannot proceed.
      if (OrigCaptureTR == nullptr)
        return nullptr;

      const TypeRef *SubstCaptureTR = nullptr;

      // If we have enough substitutions to make this captured value's
      // type concrete, or we know it's size anyway (because it is a
      // class reference or metatype, for example), go ahead and add
      // it to the layout.
      if (OrigCaptureTR->isConcreteAfterSubstitutions(Subs))
        SubstCaptureTR = OrigCaptureTR->subst(getBuilder(), Subs);
      else if (getBuilder().getTypeConverter().hasFixedSize(OrigCaptureTR))
        SubstCaptureTR = OrigCaptureTR;

      if (SubstCaptureTR != nullptr) {
        Builder.addField("", SubstCaptureTR, ExternalTypeInfo);
        if (Builder.isInvalid())
          return nullptr;

        // Try the next capture type.
        CaptureTypes = CaptureTypes.slice(1);
        continue;
      }

      // Ok, we do not have enough substitutions yet. Perhaps we have
      // enough elements figured out that we can pick off some
      // metadata sources though, and use those to derive some new
      // substitutions.
      bool Progress = false;
      for (auto Source : Info.MetadataSources) {
        // Don't read a source more than once.
        if (Done.count(Source))
          continue;

        // If we don't have enough information to read this source
        // (because it is fulfilled by metadata from a capture at
        // at unknown offset), keep going.
        if (!isMetadataSourceReady(Source.second, Builder))
          continue;

        auto Metadata = readMetadataSource(Context, Source.second, Builder);
        if (!Metadata)
          return nullptr;

        auto *SubstTR = readTypeFromMetadata(*Metadata);
        if (SubstTR == nullptr)
          return nullptr;

        if (!TypeRef::deriveSubstitutions(Subs, Source.first, SubstTR))
          return nullptr;

        Done.insert(Source);
        Progress = true;
      }

      // If we failed to make any forward progress above, we're stuck
      // and cannot close out this layout.
      if (!Progress)
        return nullptr;
    }

    // Ok, we have a complete picture now.
    return Builder.build();
  }

  /// Checks if we have enough information to read the given metadata
  /// source.
  ///
  /// \param Builder Used to obtain offsets of elements known so far.
  bool isMetadataSourceReady(const MetadataSource *MS,
                             const RecordTypeInfoBuilder &Builder) {
    switch (MS->getKind()) {
    case MetadataSourceKind::ClosureBinding:
      return true;
    case MetadataSourceKind::ReferenceCapture: {
      unsigned Index = cast<ReferenceCaptureMetadataSource>(MS)->getIndex();
      return Index < Builder.getNumFields();
    }
    case MetadataSourceKind::MetadataCapture: {
      unsigned Index = cast<MetadataCaptureMetadataSource>(MS)->getIndex();
      return Index < Builder.getNumFields();
    }
    case MetadataSourceKind::GenericArgument: {
      auto Base = cast<GenericArgumentMetadataSource>(MS)->getSource();
      return isMetadataSourceReady(Base, Builder);
    }
    case MetadataSourceKind::Self:
    case MetadataSourceKind::SelfWitnessTable:
      return true;
    }

    swift_unreachable("Unhandled MetadataSourceKind in switch.");
  }

  /// Read metadata for a captured generic type from a closure context.
  ///
  /// \param Context The closure context in the remote process.
  ///
  /// \param MS The metadata source, which must be "ready" as per the
  /// above.
  ///
  /// \param Builder Used to obtain offsets of elements known so far.
  llvm::Optional<StoredPointer>
  readMetadataSource(StoredPointer Context,
                     const MetadataSource *MS,
                     const RecordTypeInfoBuilder &Builder) {
    switch (MS->getKind()) {
    case MetadataSourceKind::ClosureBinding: {
      unsigned Index = cast<ClosureBindingMetadataSource>(MS)->getIndex();

      // Skip the context's HeapObject header
      // (one word each for isa pointer and reference counts).
      //
      // Metadata and conformance tables are stored consecutively after
      // the heap object header, in the 'necessary bindings' area.
      //
      // We should only have the index of a type metadata record here.
      unsigned Offset = getSizeOfHeapObject() +
                        sizeof(StoredPointer) * Index;

      StoredPointer MetadataAddress;
      if (!getReader().readInteger(RemoteAddress(Context + Offset),
                                   &MetadataAddress))
        break;

      return MetadataAddress;
    }
    case MetadataSourceKind::ReferenceCapture: {
      unsigned Index = cast<ReferenceCaptureMetadataSource>(MS)->getIndex();

      // We should already have enough type information to know the offset
      // of this capture in the context.
      unsigned CaptureOffset = Builder.getFieldOffset(Index);

      StoredPointer CaptureAddress;
      if (!getReader().readInteger(RemoteAddress(Context + CaptureOffset),
                                   &CaptureAddress))
        break;

      // Read the requested capture's isa pointer.
      return readMetadataFromInstance(CaptureAddress);
    }
    case MetadataSourceKind::MetadataCapture: {
      unsigned Index = cast<MetadataCaptureMetadataSource>(MS)->getIndex();

      // We should already have enough type information to know the offset
      // of this capture in the context.
      unsigned CaptureOffset = Builder.getFieldOffset(Index);

      StoredPointer CaptureAddress;
      if (!getReader().readInteger(RemoteAddress(Context + CaptureOffset),
                                   &CaptureAddress))
        break;

      return CaptureAddress;
    }
    case MetadataSourceKind::GenericArgument: {
      auto *GAMS = cast<GenericArgumentMetadataSource>(MS);
      auto Base = readMetadataSource(Context, GAMS->getSource(), Builder);
      if (!Base)
        break;

      unsigned Index = GAMS->getIndex();
      auto Arg = readGenericArgFromMetadata(*Base, Index);
      if (!Arg)
        break;

      return *Arg;
    }
    case MetadataSourceKind::Self:
    case MetadataSourceKind::SelfWitnessTable:
      break;
    }

    return llvm::None;
  }

  template <typename T>
  MemoryReader::ReadObjResult<T> readObj(StoredPointer Ptr) {
    return getReader().template readObj<T>(RemoteAddress(Ptr));
  }
};

} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_REFLECTIONCONTEXT_H
