//===--- swift-reflection-dump.cpp - Reflection testing application -------===//
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
// This is a host-side tool to dump remote reflection sections in swift
// binaries.
//===----------------------------------------------------------------------===//

#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Reflection/ReflectionContext.h"
#include "swift/Reflection/TypeRef.h"
#include "swift/Reflection/TypeRefBuilder.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/CommandLine.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <csignal>
#include <iostream>

using llvm::ArrayRef;
using llvm::dyn_cast;
using llvm::StringRef;
using namespace llvm::object;

using namespace swift;
using namespace swift::reflection;
using namespace swift::remote;
using namespace Demangle;

enum class ActionType { DumpReflectionSections, DumpTypeLowering };

namespace options {
static llvm::cl::opt<ActionType> Action(
    llvm::cl::desc("Mode:"),
    llvm::cl::values(
        clEnumValN(ActionType::DumpReflectionSections,
                   "dump-reflection-sections",
                   "Dump the field reflection section"),
        clEnumValN(
            ActionType::DumpTypeLowering, "dump-type-lowering",
            "Dump the field layout for typeref strings read from stdin")),
    llvm::cl::init(ActionType::DumpReflectionSections));

static llvm::cl::list<std::string>
    BinaryFilename("binary-filename",
                   llvm::cl::desc("Filenames of the binary files"),
                   llvm::cl::OneOrMore);

static llvm::cl::opt<std::string>
    Architecture("arch",
                 llvm::cl::desc("Architecture to inspect in the binary"),
                 llvm::cl::Required);
} // end namespace options

template <typename T> static T unwrap(llvm::Expected<T> value) {
  if (value)
    return std::move(value.get());
  llvm::errs() << "swift-reflection-test error: " << toString(value.takeError())
               << "\n";
  exit(EXIT_FAILURE);
}

using NativeReflectionContext =
    swift::reflection::ReflectionContext<External<RuntimeTarget<sizeof(uintptr_t)>>>;

using ReadBytesResult = swift::remote::MemoryReader::ReadBytesResult;

// Since ObjectMemoryReader maintains ownership of the ObjectFiles and their
// raw data, we can vend ReadBytesResults with no-op destructors.
static void no_op_destructor(const void*) {}


class Image {
private:
  struct Segment {
    uint64_t Addr;
    StringRef Contents;
  };
  
  uint64_t HeaderAddress;
  std::vector<Segment> Segments;
  
  void scanMachO(const MachOObjectFile *O) {
    using namespace llvm::MachO;

    HeaderAddress = UINT64_MAX;
    
    for (const auto &Load : O->load_commands()) {
      if (Load.C.cmd == LC_SEGMENT_64) {
        auto Seg = O->getSegment64LoadCommand(Load);
        if (Seg.filesize == 0)
          continue;
        
        auto contents = O->getData().slice(Seg.fileoff,
                                           Seg.fileoff + Seg.filesize);
        
        if (contents.empty() || contents.size() != Seg.filesize)
          continue;
        
        Segments.push_back({Seg.vmaddr, contents});
        HeaderAddress = std::min(HeaderAddress, Seg.vmaddr);
      } else if (Load.C.cmd == LC_SEGMENT) {
        auto Seg = O->getSegmentLoadCommand(Load);
        if (Seg.filesize == 0)
          continue;
        
        auto contents = O->getData().slice(Seg.fileoff,
                                           Seg.fileoff + Seg.filesize);
        
        if (contents.empty() || contents.size() != Seg.filesize)
          continue;
        
        Segments.push_back({Seg.vmaddr, contents});
        HeaderAddress = std::min(HeaderAddress, (uint64_t)Seg.vmaddr);
      }
    }
  }
  
  template<typename ELFT>
  void scanELFType(const ELFObjectFile<ELFT> *O) {
    using namespace llvm::ELF;

    HeaderAddress = UINT64_MAX;

    auto phdrs = O->getELFFile()->program_headers();
    if (!phdrs) {
      llvm::consumeError(phdrs.takeError());
      return;
    }

    for (auto &ph : *phdrs) {
      if (ph.p_filesz == 0)
        continue;
      
      auto contents = O->getData().slice(ph.p_offset,
                                         ph.p_offset + ph.p_filesz);
      if (contents.empty() || contents.size() != ph.p_filesz)
        continue;
      
      Segments.push_back({ph.p_vaddr, contents});
      HeaderAddress = std::min(HeaderAddress, (uint64_t)ph.p_vaddr);
    }
  }
  
  void scanELF(const ELFObjectFileBase *O) {
    if (auto le32 = dyn_cast<ELFObjectFile<ELF32LE>>(O)) {
      scanELFType(le32);
    } else if (auto be32 = dyn_cast<ELFObjectFile<ELF32BE>>(O)) {
      scanELFType(be32);
    } else if (auto le64 = dyn_cast<ELFObjectFile<ELF64LE>>(O)) {
      scanELFType(le64);
    } else if (auto be64 = dyn_cast<ELFObjectFile<ELF64BE>>(O)) {
      scanELFType(be64);
    }
    
    // FIXME: ReflectionContext tries to read bits of the ELF structure that
    // aren't normally mapped by a phdr. Until that's fixed,
    // allow access to the whole file 1:1 in address space that isn't otherwise
    // mapped.
    Segments.push_back({HeaderAddress, O->getData()});
  }
  
  void scanCOFF(const COFFObjectFile *O) {
    HeaderAddress = O->getImageBase();
    
    for (auto SectionRef : O->sections()) {
      auto Section = O->getCOFFSection(SectionRef);
      
      if (Section->SizeOfRawData == 0)
        continue;
      
      auto SectionBase = O->getImageBase() + Section->VirtualAddress;
      auto SectionContent =
        O->getData().slice(Section->PointerToRawData,
                           Section->PointerToRawData + Section->SizeOfRawData);
      if (SectionContent.empty()
          || SectionContent.size() != Section->SizeOfRawData)
        continue;
      
      Segments.push_back({SectionBase, SectionContent});
    }
    
    Segments.push_back({HeaderAddress, O->getData()});
  }

public:
  explicit Image(const ObjectFile *O) {
    // Unfortunately llvm doesn't provide a uniform interface for iterating
    // loadable segments or dynamic relocations in executable images yet.
    if (auto macho = dyn_cast<MachOObjectFile>(O)) {
      scanMachO(macho);
    } else if (auto elf = dyn_cast<ELFObjectFileBase>(O)) {
      scanELF(elf);
    } else if (auto coff = dyn_cast<COFFObjectFile>(O)) {
      scanCOFF(coff);
    } else {
      fputs("unsupported image format\n", stderr);
      abort();
    }
    
    // ObjectMemoryReader uses the most significant 16 bits of the address to
    // index multiple images, so if an object maps stuff out of that range
    // we won't be able to read it. 2**48 of virtual address space ought to be
    // enough for anyone, but warn if we blow that limit.
    for (auto Segment : Segments) {
      if (Segment.Addr >= 0xFFFFFFFFFFFFull) {
        fputs("warning: segment mapped at address above 2**48\n", stderr);
        continue;
      }
    }
  }
    
  uint64_t getStartAddress() const {
    return HeaderAddress;
  }

  StringRef getContentsAtAddress(uint64_t Addr, uint64_t Size) const {
    for (auto &Segment : Segments) {
      auto addrInSegment = Segment.Addr <= Addr
        && Addr + Size <= Segment.Addr + Segment.Contents.size();
      
      if (!addrInSegment)
        continue;

      auto offset = Addr - Segment.Addr;
          
      return Segment.Contents.drop_front(offset);
    }
    return {};
  }
};

/// MemoryReader that reads from the on-disk representation of an executable
/// or dynamic library image.
///
/// This reader uses a remote addressing scheme where the most significant
/// 16 bits of the address value serve as an index into the array of loaded images,
/// and the low 48 bits correspond to the preferred virtual address mapping of
/// the image.
class ObjectMemoryReader : public MemoryReader {
  std::vector<Image> Images;

  StringRef getContentsAtAddress(uint64_t Addr, uint64_t Size) {
    auto imageIndex = Addr >> 48;
    if (imageIndex >= Images.size())
      return StringRef();
    
    return Images[imageIndex]
      .getContentsAtAddress(Addr & 0xFFFFFFFFFFFFull, Size);
  }
  
public:
  explicit ObjectMemoryReader(
      const std::vector<const ObjectFile *> &ObjectFiles) {
    // We use a 16-bit index for images, so can't take more than 64K at once.
    if (ObjectFiles.size() >= 0x10000) {
      fputs("can't dump more than 65,536 images at once", stderr);
      abort();
    }
    for (const ObjectFile *O : ObjectFiles)
      Images.emplace_back(O);
  }

  ArrayRef<Image> getImages() const { return Images; }

  bool queryDataLayout(DataLayoutQueryType type, void *inBuffer,
                       void *outBuffer) override {
    switch (type) {
    case DLQ_GetPointerSize: {
      auto result = static_cast<uint8_t *>(outBuffer);
      *result = sizeof(void *);
      return true;
    }
    case DLQ_GetSizeSize: {
      auto result = static_cast<uint8_t *>(outBuffer);
      *result = sizeof(size_t);
      return true;
    }
    }

    return false;
  }
  
  RemoteAddress getImageStartAddress(unsigned i) const {
    assert(i < Images.size());
    
    return RemoteAddress(Images[i].getStartAddress() | ((uint64_t)i << 48));
  }

  // TODO: We could consult the dynamic symbol tables of the images to
  // implement this.
  RemoteAddress getSymbolAddress(const std::string &name) override {
    return RemoteAddress(nullptr);
  }

  ReadBytesResult readBytes(RemoteAddress Addr, uint64_t Size) override {
    auto addrValue = Addr.getAddressData();
    auto resultBuffer = getContentsAtAddress(addrValue, Size);
    return ReadBytesResult(resultBuffer.data(), no_op_destructor);
  }

  bool readString(RemoteAddress Addr, std::string &Dest) override {
    auto addrValue = Addr.getAddressData();
    auto resultBuffer = getContentsAtAddress(addrValue, 1);
    if (resultBuffer.empty())
      return false;
    
    // Make sure there's a null terminator somewhere in the contents.
    unsigned i = 0;
    for (unsigned e = resultBuffer.size(); i < e; ++i) {
      if (resultBuffer[i] == 0)
        goto found_terminator;
    }
    return false;
    
  found_terminator:
    Dest.append(resultBuffer.begin(), resultBuffer.begin() + i);
    return true;
  }
};

static int doDumpReflectionSections(ArrayRef<std::string> BinaryFilenames,
                                    StringRef Arch, ActionType Action,
                                    std::ostream &OS) {
  // Note: binaryOrError and objectOrError own the memory for our ObjectFile;
  // once they go out of scope, we can no longer do anything.
  std::vector<OwningBinary<Binary>> BinaryOwners;
  std::vector<std::unique_ptr<ObjectFile>> ObjectOwners;
  std::vector<const ObjectFile *> ObjectFiles;

  for (const std::string &BinaryFilename : BinaryFilenames) {
    auto BinaryOwner = unwrap(createBinary(BinaryFilename));
    Binary *BinaryFile = BinaryOwner.getBinary();

    // The object file we are doing lookups in -- either the binary itself, or
    // a particular slice of a universal binary.
    std::unique_ptr<ObjectFile> ObjectOwner;
    const ObjectFile *O = dyn_cast<ObjectFile>(BinaryFile);
    if (!O) {
      auto Universal = cast<MachOUniversalBinary>(BinaryFile);
      ObjectOwner = unwrap(Universal->getObjectForArch(Arch));
      O = ObjectOwner.get();
    }

    // Retain the objects that own section memory
    BinaryOwners.push_back(std::move(BinaryOwner));
    ObjectOwners.push_back(std::move(ObjectOwner));
    ObjectFiles.push_back(O);
  }

  auto Reader = std::make_shared<ObjectMemoryReader>(ObjectFiles);
  NativeReflectionContext Context(Reader);
  for (unsigned i = 0, e = Reader->getImages().size(); i < e; ++i) {
    Context.addImage(Reader->getImageStartAddress(i));
 }

  switch (Action) {
  case ActionType::DumpReflectionSections:
    // Dump everything
    Context.getBuilder().dumpAllSections(OS);
    break;
  case ActionType::DumpTypeLowering: {
    for (std::string Line; std::getline(std::cin, Line);) {
      if (Line.empty())
        continue;

      if (StringRef(Line).startswith("//"))
        continue;

      Demangle::Demangler Dem;
      auto Demangled = Dem.demangleType(Line);
      auto *TypeRef =
          swift::Demangle::decodeMangledType(Context.getBuilder(), Demangled);
      if (TypeRef == nullptr) {
        OS << "Invalid typeref: " << Line << "\n";
        continue;
      }

      TypeRef->dump(OS);
      auto *TypeInfo =
          Context.getBuilder().getTypeConverter().getTypeInfo(TypeRef);
      if (TypeInfo == nullptr) {
        OS << "Invalid lowering\n";
        continue;
      }
      TypeInfo->dump(OS);
    }
    break;
  }
  }

  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  PROGRAM_START(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift Reflection Dump\n");
  return doDumpReflectionSections(options::BinaryFilename,
                                  options::Architecture, options::Action,
                                  std::cout);
}
