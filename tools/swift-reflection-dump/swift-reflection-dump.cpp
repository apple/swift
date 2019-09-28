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

#include "../../stdlib/public/runtime/ImageInspectionELF.h"
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
#include "llvm/Support/Error.h"

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

static void reportError(StringRef Message) {
  llvm::errs() << "swift-reflection-test error: " << Message << ".\n";
  exit(EXIT_FAILURE);
}

static void reportError(std::error_code EC) {
  assert(EC);
  reportError(EC.message());
}

using NativeReflectionContext = swift::reflection::ReflectionContext<
    External<RuntimeTarget<sizeof(uintptr_t)>>>;

using ReadBytesResult = swift::remote::MemoryReader::ReadBytesResult;

static uint64_t getSectionAddress(SectionRef S) {
  // See COFFObjectFile.cpp for the implementation of
  // COFFObjectFile::getSectionAddress. The image base address is added
  // to all the addresses of the sections, thus the behavior is slightly
  // different from the other platforms.
  if (auto C = dyn_cast<COFFObjectFile>(S.getObject()))
    return S.getAddress() - C->getImageBase();
  return S.getAddress();
}

static bool needToRelocate(SectionRef S) {
  if (!getSectionAddress(S))
    return false;

  if (auto EO = dyn_cast<ELFObjectFileBase>(S.getObject())) {
    static const llvm::StringSet<> ELFSectionsList = {
        ".data",
        ".rodata",
        ".note.swift_reflection_metadata",
        "swift5_protocols",
        "swift5_protocol_conformances",
        "swift5_typeref",
        "swift5_reflstr",
        "swift5_assocty",
        "swift5_replace",
        "swift5_type_metadata",
        "swift5_fieldmd",
        "swift5_capture",
        "swift5_builtin",
    };
    llvm::Expected<llvm::StringRef> NameOrErr = S.getName();
    if (!NameOrErr) {
      reportError(errorToErrorCode(NameOrErr.takeError()));
      return false;
    }
    return ELFSectionsList.count(*NameOrErr);
  }

  return true;
}

static section_iterator findSectionByName(StringRef Name, const ObjectFile &O) {
  auto Sections = O.sections();
  SmallVector<section_iterator, 1> FoundSections;
  std::copy_if(Sections.begin(), Sections.end(),
               std::back_inserter(FoundSections), [Name](SectionRef S) {
                 auto NameOrErr = S.getName();
                 if (!NameOrErr)
                   reportError(toString(NameOrErr.takeError()));
                 return *NameOrErr == Name;
               });
  if (FoundSections.size() != 1)
    return Sections.end();
  return FoundSections.front();
}

template <class ELFObj,
          typename std::enable_if<
              std::is_base_of<ELFObjectFileBase, ELFObj>::value, int>::type = 0>
const typename ELFObj::Elf_Rela *findRelaForOffset(uint64_t Offset,
                                                   const ELFObj &O) {
  for (const SectionRef &S :
       static_cast<const ObjectFile *>(&O)->dynamic_relocation_sections()) {
    const auto *Sec = O.getSection(S.getRawDataRefImpl());
    if (Sec->sh_type != llvm::ELF::SHT_RELA)
      continue;
    for (const RelocationRef &R : S.relocations()) {
      const auto *Rela = O.getRela(R.getRawDataRefImpl());
      if (Rela->r_offset == Offset &&
          Rela->getType(false /* isMips64EL */) == llvm::ELF::R_X86_64_RELATIVE)
        return Rela;
    }
  }
  return nullptr;
}

static void updateReflectionMetdataNote(const ObjectFile &O, char *Memory) {
  auto EO = dyn_cast<const ELFObjectFileBase>(&O);
  if (!EO)
    return;
  auto Note = findSectionByName(".note.swift_reflection_metadata", O);
  if (Note == O.sections().end())
    return;
  const StringRef MagicString = SWIFT_REFLECTION_METADATA_ELF_NOTE_MAGIC_STRING;
  if (StringRef(Memory + Note->getAddress(), MagicString.size()) != MagicString)
    reportError(".note.swift_reflection_metadata is invalid");

  const uint64_t Offset = Note->getAddress() + MagicString.size() + 1;
  uint64_t Addend = 0;
  if (auto *ELF32LEObj = dyn_cast<ELF32LEObjectFile>(EO))
    if (auto *Rela = findRelaForOffset(Offset, *ELF32LEObj))
      Addend = static_cast<uint64_t>(Rela->r_addend);
  if (auto *ELF64LEObj = dyn_cast<ELF64LEObjectFile>(EO))
    if (auto *Rela = findRelaForOffset(Offset, *ELF64LEObj))
      Addend = static_cast<uint64_t>(Rela->r_addend);
  if (!Addend)
    reportError("No supported relocations found");
  uintptr_t PtrValue = reinterpret_cast<uintptr_t>(Memory) + Addend;
  *reinterpret_cast<uintptr_t *>(Memory + Note->getAddress() +
                                 MagicString.size() + 1) = PtrValue;
  auto S = reinterpret_cast<MetadataSections *>(PtrValue);

#define STRINGIFY(s) #s
#define UPDATE_SECTION_RANGE(Name)                                             \
  [&]() {                                                                      \
    auto It = findSectionByName(STRINGIFY(Name), O);                           \
    if (It == O.sections().end())                                              \
      return;                                                                  \
    uintptr_t Addr = reinterpret_cast<uintptr_t>(Memory) + It->getAddress();   \
    S->Name = {Addr, It->getSize()};                                           \
  }();

  UPDATE_SECTION_RANGE(swift5_protocols)
  UPDATE_SECTION_RANGE(swift5_protocol_conformances)
  UPDATE_SECTION_RANGE(swift5_type_metadata)
  UPDATE_SECTION_RANGE(swift5_typeref)
  UPDATE_SECTION_RANGE(swift5_reflstr)
  UPDATE_SECTION_RANGE(swift5_fieldmd)
  UPDATE_SECTION_RANGE(swift5_assocty)
  UPDATE_SECTION_RANGE(swift5_replace)
  UPDATE_SECTION_RANGE(swift5_replac2)
  UPDATE_SECTION_RANGE(swift5_builtin)
  UPDATE_SECTION_RANGE(swift5_capture)
#undef STRINGIFY
#undef UPDATE_SECTION_RANGE
}

class Image {
  std::vector<char> Memory;

public:
  explicit Image(const ObjectFile *O) {
    uint64_t VASize = O->getData().size();
    for (SectionRef S : O->sections()) {
      if (auto SectionAddr = getSectionAddress(S))
        VASize = std::max(VASize, SectionAddr + S.getSize());
    }
    Memory.resize(VASize);
    std::memcpy(&Memory[0], O->getData().data(), O->getData().size());

    for (SectionRef S : O->sections()) {
      if (!needToRelocate(S))
        continue;
      llvm::Expected<llvm::StringRef> Content = S.getContents();
      if (!Content)
        reportError(errorToErrorCode(Content.takeError()));
      std::memcpy(&Memory[getSectionAddress(S)], Content->data(),
                  Content->size());
    }
    updateReflectionMetdataNote(*O, Memory.data());
  }

  RemoteAddress getStartAddress() const {
    return RemoteAddress((uintptr_t)Memory.data());
  }

  bool isAddressValid(RemoteAddress Addr, uint64_t Size) const {
    return (uintptr_t)Memory.data() <= Addr.getAddressData() &&
           Addr.getAddressData() + Size <=
               (uintptr_t)Memory.data() + Memory.size();
  }

  ReadBytesResult readBytes(RemoteAddress Addr, uint64_t Size) {
    if (!isAddressValid(Addr, Size))
      return ReadBytesResult(nullptr, [](const void *) {});
    return ReadBytesResult((const void *)(Addr.getAddressData()),
                           [](const void *) {});
  }
};

class ObjectMemoryReader : public MemoryReader {
  std::vector<Image> Images;

public:
  explicit ObjectMemoryReader(
      const std::vector<const ObjectFile *> &ObjectFiles) {
    for (const ObjectFile *O : ObjectFiles)
      Images.emplace_back(O);
  }

  const std::vector<Image> &getImages() const { return Images; }

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

  RemoteAddress getSymbolAddress(const std::string &name) override {
    return RemoteAddress(nullptr);
  }

  ReadBytesResult readBytes(RemoteAddress Addr, uint64_t Size) override {
    auto I = std::find_if(Images.begin(), Images.end(), [=](const Image &I) {
      return I.isAddressValid(Addr, Size);
    });
    return I == Images.end() ? ReadBytesResult(nullptr, [](const void *) {})
                             : I->readBytes(Addr, Size);
  }

  bool readString(RemoteAddress Addr, std::string &Dest) override {
    ReadBytesResult R = readBytes(Addr, 1);
    if (!R)
      return false;
    StringRef Str((const char *)R.get());
    Dest.append(Str.begin(), Str.end());
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
      ObjectOwner = unwrap(Universal->getMachOObjectForArch(Arch));
      O = ObjectOwner.get();
    }

    // Retain the objects that own section memory
    BinaryOwners.push_back(std::move(BinaryOwner));
    ObjectOwners.push_back(std::move(ObjectOwner));
    ObjectFiles.push_back(O);
  }

  auto Reader = std::make_shared<ObjectMemoryReader>(ObjectFiles);
  NativeReflectionContext Context(Reader);
  for (const Image &I : Reader->getImages())
    Context.addImage(I.getStartAddress());

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
