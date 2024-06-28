//===------------ ObjectFileContext.cpp - Swift Compiler ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/StaticMirror/ObjectFileContext.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Unreachable.h"
#include "swift/Demangling/Demangler.h"
#include "swift/RemoteInspection/ReflectionContext.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/Remote/CMemoryReader.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/MachOUniversal.h"

#include "llvm/Object/Archive.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/RelocationResolver.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/StringSaver.h"

#include <sstream>

using namespace llvm::object;

namespace swift {
namespace static_mirror {

// Since ObjectMemoryReader maintains ownership of the ObjectFiles and their
// raw data, we can vend ReadBytesResults with no-op destructors.
static void no_op_destructor(const void *) {}

void Image::scanMachO(const llvm::object::MachOObjectFile *O) {
  using namespace llvm::MachO;

  HeaderAddress = UINT64_MAX;

  // Collect the segment preferred vm mappings.
  for (const auto &Load : O->load_commands()) {
    if (Load.C.cmd == LC_SEGMENT_64) {
      auto Seg = O->getSegment64LoadCommand(Load);
      if (Seg.filesize == 0)
        continue;

      auto contents =
          O->getData().slice(Seg.fileoff, Seg.fileoff + Seg.filesize);

      if (contents.empty() || contents.size() != Seg.filesize)
        continue;

      Segments.push_back({Seg.vmaddr, contents});
      HeaderAddress = std::min(HeaderAddress, Seg.vmaddr);
    } else if (Load.C.cmd == LC_SEGMENT) {
      auto Seg = O->getSegmentLoadCommand(Load);
      if (Seg.filesize == 0)
        continue;

      auto contents =
          O->getData().slice(Seg.fileoff, Seg.fileoff + Seg.filesize);

      if (contents.empty() || contents.size() != Seg.filesize)
        continue;

      Segments.push_back({Seg.vmaddr, contents});
      HeaderAddress = std::min(HeaderAddress, (uint64_t)Seg.vmaddr);
    }
  }

  // Walk through the bindings list to collect all the external references
  // in the image.
  llvm::Error error = llvm::Error::success();
  auto OO = const_cast<llvm::object::MachOObjectFile *>(O);

  for (auto bind : OO->bindTable(error)) {
    if (error) {
      llvm::consumeError(std::move(error));
      break;
    }

    // The offset from the symbol is stored at the target address.
    uint64_t Offset = 0;
    auto OffsetContent =
        getContentsAtAddress(bind.address(), O->getBytesInAddress());
    if (OffsetContent.empty())
      continue;

    if (O->getBytesInAddress() == 8) {
      memcpy(&Offset, OffsetContent.data(), sizeof(Offset));
    } else if (O->getBytesInAddress() == 4) {
      uint32_t OffsetValue;
      memcpy(&OffsetValue, OffsetContent.data(), sizeof(OffsetValue));
      Offset = OffsetValue;
    } else {
      assert(false && "unexpected word size?!");
    }

    DynamicRelocations.insert({bind.address(), {bind.symbolName(), Offset}});
  }
  if (error) {
    llvm::consumeError(std::move(error));
  }
}

template <typename ELFT>
void Image::scanELFType(const llvm::object::ELFObjectFile<ELFT> *O) {
  using namespace llvm::ELF;

  HeaderAddress = UINT64_MAX;

  auto phdrs = O->getELFFile().program_headers();
  if (!phdrs) {
    llvm::consumeError(phdrs.takeError());
  }

  for (auto &ph : *phdrs) {
    if (ph.p_filesz == 0)
      continue;

    auto contents = O->getData().slice(ph.p_offset, ph.p_offset + ph.p_filesz);
    if (contents.empty() || contents.size() != ph.p_filesz)
      continue;

    Segments.push_back({ph.p_vaddr, contents});
    HeaderAddress = std::min(HeaderAddress, (uint64_t)ph.p_vaddr);
  }

  // Collect the dynamic relocations.
  auto resolver = getRelocationResolver(*O);
  auto resolverSupports = resolver.first;
  auto resolve = resolver.second;

  if (!resolverSupports || !resolve)
    return;

  auto machine = O->getELFFile().getHeader().e_machine;
  auto relativeRelocType = llvm::object::getELFRelativeRelocationType(machine);

  for (auto &S : static_cast<const llvm::object::ELFObjectFileBase *>(O)
                     ->dynamic_relocation_sections()) {
    bool isRela =
        O->getSection(S.getRawDataRefImpl())->sh_type == llvm::ELF::SHT_RELA;

    for (const llvm::object::RelocationRef &R : S.relocations()) {
      // `getRelocationResolver` doesn't handle RELATIVE relocations, so we
      // have to do that ourselves.
      if (isRela && R.getType() == relativeRelocType) {
        auto rela = O->getRela(R.getRawDataRefImpl());
        DynamicRelocations.insert(
            {R.getOffset(), {{}, HeaderAddress + rela->r_addend}});
        continue;
      }

      if (!resolverSupports(R.getType()))
        continue;
      auto symbol = R.getSymbol();
      auto name = symbol->getName();
      if (!name) {
        llvm::consumeError(name.takeError());
        continue;
      }
      uint64_t offset = resolve(R.getType(), R.getOffset(), 0, 0, 0);
      DynamicRelocations.insert({R.getOffset(), {*name, offset}});
    }
  }
}

void Image::scanELF(const llvm::object::ELFObjectFileBase *O) {
  if (auto le32 =
          dyn_cast<llvm::object::ELFObjectFile<llvm::object::ELF32LE>>(O)) {
    scanELFType(le32);
  } else if (auto be32 =
                 dyn_cast<llvm::object::ELFObjectFile<llvm::object::ELF32BE>>(
                     O)) {
    scanELFType(be32);
  } else if (auto le64 =
                 dyn_cast<llvm::object::ELFObjectFile<llvm::object::ELF64LE>>(
                     O)) {
    scanELFType(le64);
  } else if (auto be64 =
                 dyn_cast<llvm::object::ELFObjectFile<llvm::object::ELF64BE>>(
                     O)) {
    scanELFType(be64);
  } else {
    return;
  }

  // FIXME: ReflectionContext tries to read bits of the ELF structure that
  // aren't normally mapped by a phdr. Until that's fixed,
  // allow access to the whole file 1:1 in address space that isn't otherwise
  // mapped.
  Segments.push_back({HeaderAddress, O->getData()});
}

void Image::scanCOFF(const llvm::object::COFFObjectFile *O) {
  HeaderAddress = O->getImageBase();

  for (auto SectionRef : O->sections()) {
    auto Section = O->getCOFFSection(SectionRef);

    if (Section->SizeOfRawData == 0)
      continue;

    auto SectionBase = O->getImageBase() + Section->VirtualAddress;
    auto SectionContent =
        O->getData().slice(Section->PointerToRawData,
                           Section->PointerToRawData + Section->SizeOfRawData);
    if (SectionContent.empty() ||
        SectionContent.size() != Section->SizeOfRawData)
      continue;

    Segments.push_back({SectionBase, SectionContent});
  }

  // FIXME: We need to map the header at least, but how much of it does
  // Windows typically map?
  Segments.push_back({HeaderAddress, O->getData()});
}

bool Image::isMachOWithPtrAuth() const {
  auto macho = dyn_cast<llvm::object::MachOObjectFile>(O);
  if (!macho)
    return false;

  auto &header = macho->getHeader();

  return header.cputype == llvm::MachO::CPU_TYPE_ARM64 &&
         header.cpusubtype == llvm::MachO::CPU_SUBTYPE_ARM64E;
}

Image::Image(const llvm::object::ObjectFile *O) : O(O) {
  // Unfortunately llvm doesn't provide a uniform interface for iterating
  // loadable segments or dynamic relocations in executable images yet.
  if (auto macho = dyn_cast<llvm::object::MachOObjectFile>(O)) {
    scanMachO(macho);
  } else if (auto elf = dyn_cast<llvm::object::ELFObjectFileBase>(O)) {
    scanELF(elf);
  } else if (auto coff = dyn_cast<llvm::object::COFFObjectFile>(O)) {
    scanCOFF(coff);
  } else {
    fputs("unsupported image format\n", stderr);
    abort();
  }
}

uint64_t Image::getEndAddress() const {
  uint64_t max = 0;
  for (auto &Segment : Segments) {
    max = std::max(max, Segment.Addr + Segment.Contents.size());
  }
  return max;
}

StringRef Image::getContentsAtAddress(uint64_t Addr, uint64_t Size) const {
  for (auto &Segment : Segments) {
    auto addrInSegment = Segment.Addr <= Addr &&
                         Addr + Size <= Segment.Addr + Segment.Contents.size();

    if (!addrInSegment)
      continue;

    auto offset = Addr - Segment.Addr;
    auto result = Segment.Contents.drop_front(offset);
    return result;
  }
  return {};
}

remote::RemoteAbsolutePointer
Image::resolvePointer(uint64_t Addr, uint64_t pointerValue) const {
  // In Mach-O images with ptrauth, the pointer value has an offset from the
  // base address in the low 32 bits, and ptrauth discriminator info in the top
  // 32 bits.
  if (isMachOWithPtrAuth()) {
    return remote::RemoteAbsolutePointer(
        "", HeaderAddress + (pointerValue & 0xffffffffull));
  } else {
    return remote::RemoteAbsolutePointer("", pointerValue);
  }
}

remote::RemoteAbsolutePointer Image::getDynamicSymbol(uint64_t Addr) const {
  auto found = DynamicRelocations.find(Addr);
  if (found == DynamicRelocations.end())
    return nullptr;
  return remote::RemoteAbsolutePointer(found->second.Symbol,
                                       found->second.Offset);
}

std::pair<const Image *, uint64_t>
ObjectMemoryReader::decodeImageIndexAndAddress(uint64_t Addr) const {
  for (auto &Image : Images) {
    if (Image.TheImage.getStartAddress() + Image.Slide <= Addr &&
        Addr < Image.TheImage.getEndAddress() + Image.Slide) {
      return {&Image.TheImage, Addr - Image.Slide};
    }
  }
  return {nullptr, 0};
}

uint64_t
ObjectMemoryReader::encodeImageIndexAndAddress(const Image *image,
                                               uint64_t imageAddr) const {
  auto entry = (const ImageEntry *)image;
  return imageAddr + entry->Slide;
}

StringRef ObjectMemoryReader::getContentsAtAddress(uint64_t Addr,
                                                   uint64_t Size) {
  const Image *image;
  uint64_t imageAddr;
  std::tie(image, imageAddr) = decodeImageIndexAndAddress(Addr);

  if (!image)
    return StringRef();

  return image->getContentsAtAddress(imageAddr, Size);
}

ObjectMemoryReader::ObjectMemoryReader(
    const std::vector<const llvm::object::ObjectFile *> &ObjectFiles) {
  if (ObjectFiles.empty()) {
    fputs("no object files provided\n", stderr);
    abort();
  }
  unsigned WordSize = 0;
  for (const llvm::object::ObjectFile *O : ObjectFiles) {
    // All the object files we look at should share a word size.
    if (!WordSize) {
      WordSize = O->getBytesInAddress();
    } else if (WordSize != O->getBytesInAddress()) {
      fputs("object files must all be for the same architecture\n", stderr);
      abort();
    }
    Images.push_back({Image(O), 0});
  }

  // If there is more than one image loaded, try to fit them into one address
  // space.
  if (Images.size() > 1) {
    uint64_t NextAddrSpace = 0;
    for (auto &Image : Images) {
      Image.Slide = NextAddrSpace - Image.TheImage.getStartAddress();
      NextAddrSpace +=
          Image.TheImage.getEndAddress() - Image.TheImage.getStartAddress();
      NextAddrSpace = (NextAddrSpace + 16383) & ~16383;
    }

    if (WordSize < 8 && NextAddrSpace > 0xFFFFFFFFu) {
      fputs("object files did not fit in address space", stderr);
      abort();
    }
  }
}

bool ObjectMemoryReader::queryDataLayout(DataLayoutQueryType type,
                                         void *inBuffer, void *outBuffer) {
  auto wordSize = Images.front().TheImage.getBytesInAddress();
  // TODO: The following should be set based on inspecting the image.
  // This code sets it to match the platform this code was compiled for.
#if defined(__APPLE__) && __APPLE__
  auto applePlatform = true;
#else
  auto applePlatform = false;
#endif
#if defined(__APPLE__) && __APPLE__ &&                                         \
    ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) ||                              \
     (defined(TARGET_OS_IOS) && TARGET_OS_WATCH) ||                            \
     (defined(TARGET_OS_TV) && TARGET_OS_TV) || defined(__arm64__))
  auto iosDerivedPlatform = true;
#else
  auto iosDerivedPlatform = false;
#endif

  switch (type) {
  case DLQ_GetPointerSize: {
    auto result = static_cast<uint8_t *>(outBuffer);
    *result = wordSize;
    return true;
  }
  case DLQ_GetSizeSize: {
    auto result = static_cast<uint8_t *>(outBuffer);
    *result = wordSize;
    return true;
  }
  case DLQ_GetPtrAuthMask: {
    // We don't try to sign pointers at all in our view of the object
    // mapping.
    if (wordSize == 4) {
      auto result = static_cast<uint32_t *>(outBuffer);
      *result = (uint32_t)~0ull;
      return true;
    } else if (wordSize == 8) {
      auto result = static_cast<uint64_t *>(outBuffer);
      *result = (uint64_t)~0ull;
      return true;
    }
    return false;
  }
  case DLQ_GetObjCReservedLowBits: {
    auto result = static_cast<uint8_t *>(outBuffer);
    if (applePlatform && !iosDerivedPlatform && wordSize == 8) {
      // Obj-C reserves low bit on 64-bit macOS only.
      // Other Apple platforms don't reserve this bit (even when
      // running on x86_64-based simulators).
      *result = 1;
    } else {
      *result = 0;
    }
    return true;
  }
  case DLQ_GetLeastValidPointerValue: {
    auto result = static_cast<uint64_t *>(outBuffer);
    if (applePlatform && wordSize == 8) {
      // Swift reserves the first 4GiB on 64-bit Apple platforms
      *result = 0x100000000;
    } else {
      // Swift reserves the first 4KiB everywhere else
      *result = 0x1000;
    }
    return true;
  }
  case DLQ_GetObjCInteropIsEnabled:
    break;
  }

  return false;
}

reflection::RemoteAddress
ObjectMemoryReader::getImageStartAddress(unsigned i) const {
  assert(i < Images.size());

  return reflection::RemoteAddress(encodeImageIndexAndAddress(
      &Images[i].TheImage, Images[i].TheImage.getStartAddress()));
}

ReadBytesResult ObjectMemoryReader::readBytes(reflection::RemoteAddress Addr,
                                              uint64_t Size) {
  auto addrValue = Addr.getAddressData();
  auto resultBuffer = getContentsAtAddress(addrValue, Size);
  return ReadBytesResult(resultBuffer.data(), no_op_destructor);
}

bool ObjectMemoryReader::readString(reflection::RemoteAddress Addr,
                                    std::string &Dest) {
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

remote::RemoteAbsolutePointer
ObjectMemoryReader::resolvePointer(reflection::RemoteAddress Addr,
                                   uint64_t pointerValue) {
  auto addrValue = Addr.getAddressData();
  const Image *image;
  uint64_t imageAddr;
  std::tie(image, imageAddr) = decodeImageIndexAndAddress(addrValue);

  if (!image)
    return remote::RemoteAbsolutePointer();

  auto resolved = image->resolvePointer(imageAddr, pointerValue);
  // Mix in the image index again to produce a remote address pointing into the
  // same image.
  return remote::RemoteAbsolutePointer(
      "", encodeImageIndexAndAddress(
              image, resolved.getResolvedAddress().getAddressData()));
}

remote::RemoteAbsolutePointer
ObjectMemoryReader::getDynamicSymbol(reflection::RemoteAddress Addr) {
  auto addrValue = Addr.getAddressData();
  const Image *image;
  uint64_t imageAddr;
  std::tie(image, imageAddr) = decodeImageIndexAndAddress(addrValue);

  if (!image)
    return nullptr;

  return image->getDynamicSymbol(imageAddr);
}

template <typename Runtime>
std::unique_ptr<ReflectionContextHolder> makeReflectionContextForMetadataReader(
    std::shared_ptr<ObjectMemoryReader> reader, uint8_t pointerSize) {
  using ReflectionContext = reflection::ReflectionContext<Runtime>;
  auto context = new ReflectionContext(reader);
  auto &builder = context->getBuilder();
  for (unsigned i = 0, e = reader->getImages().size(); i < e; ++i) {
    context->addImage(reader->getImageStartAddress(i));
  }

  ReflectionContextHolder *holder = new ReflectionContextHolder{
      ReflectionContextOwner(context,
                             [](void *x) { delete (ReflectionContext *)x; }),
      builder, *reader, pointerSize};
  return std::unique_ptr<ReflectionContextHolder>(holder);
}

std::unique_ptr<ReflectionContextHolder> makeReflectionContextForObjectFiles(
  const std::vector<const ObjectFile *> &objectFiles, bool ObjCInterop) {
  auto Reader = std::make_shared<ObjectMemoryReader>(objectFiles);

  uint8_t pointerSize;
  Reader->queryDataLayout(DataLayoutQueryType::DLQ_GetPointerSize, nullptr,
                          &pointerSize);

  switch (pointerSize) {
  case 4:
#define MAKE_CONTEXT(INTEROP, PTRSIZE)                                         \
  makeReflectionContextForMetadataReader<                                      \
      External<INTEROP<RuntimeTarget<PTRSIZE>>>>(std::move(Reader),            \
                                                 pointerSize)
#if SWIFT_OBJC_INTEROP
    if (ObjCInterop)
      return MAKE_CONTEXT(WithObjCInterop, 4);
    else
      return MAKE_CONTEXT(NoObjCInterop, 4);
#else
    return MAKE_CONTEXT(NoObjCInterop, 4);
#endif
   case 8:
#if SWIFT_OBJC_INTEROP
    if (ObjCInterop)
      return MAKE_CONTEXT(WithObjCInterop, 8);
    else
      return MAKE_CONTEXT(NoObjCInterop, 8);
#else
    return MAKE_CONTEXT(NoObjCInterop, 8);
#endif
  default:
    fputs("unsupported word size in object file\n", stderr);
    abort();
  }
}
} // end namespace static_mirror
} // end namespace swift
