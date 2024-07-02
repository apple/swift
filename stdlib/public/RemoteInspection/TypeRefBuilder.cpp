//===--- TypeRefBuilder.cpp - Swift Type Reference Builder ----------------===//
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
// Implements utilities for constructing TypeRefs and looking up field and
// enum case types.
//
//===----------------------------------------------------------------------===//

#if SWIFT_ENABLE_REFLECTION

#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/RemoteInspection/Records.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace swift;
using namespace reflection;
using ReadBytesResult = swift::remote::MemoryReader::ReadBytesResult;

TypeRefBuilder::BuiltType
TypeRefBuilder::decodeMangledType(Node *node, bool forRequirement) {
  return swift::Demangle::decodeMangledType(*this, node, forRequirement)
      .getType();
}

RemoteRef<char> TypeRefBuilder::ReflectionTypeDescriptorFinder::readTypeRef(
    uint64_t remoteAddr) {
  // The remote address should point into one of the TypeRef or
  // ReflectionString references we already read out of the images.
  RemoteRef<char> foundTypeRef;
  RemoteRef<void> limitAddress;
  for (auto &info : ReflectionInfos) {
    if (info.TypeReference.containsRemoteAddress(remoteAddr, 1)) {
      foundTypeRef = info.TypeReference.getRemoteRef<char>(remoteAddr);
      limitAddress = info.TypeReference.endAddress();
      goto found_type_ref;
    }
    if (info.ReflectionString.containsRemoteAddress(remoteAddr, 1)) {
      foundTypeRef = info.ReflectionString.getRemoteRef<char>(remoteAddr);
      limitAddress = info.ReflectionString.endAddress();
      goto found_type_ref;
    }
  }
  // TODO: Try using MetadataReader to read the string here?

  // Invalid type ref pointer.
  return nullptr;

found_type_ref:
  // Make sure there's a valid mangled string within the bounds of the
  // section.
  for (auto i = foundTypeRef;
       i.getAddressData() < limitAddress.getAddressData();) {
    auto c = *i.getLocalBuffer();
    if (c == '\0')
      goto valid_type_ref;

    if (c >= '\1' && c <= '\x17')
      i = i.atByteOffset(5);
    else if (c >= '\x18' && c <= '\x1F') {
      i = i.atByteOffset(Builder.PointerSize + 1);
    } else {
      i = i.atByteOffset(1);
    }
  }

  // Unterminated string.
  return nullptr;

valid_type_ref:
  // Look past the $s prefix if the string has one.
  auto localStr = foundTypeRef.getLocalBuffer();
  if (localStr[0] == '$' && localStr[1] == 's') {
    foundTypeRef = foundTypeRef.atByteOffset(2);
  }

  return foundTypeRef;
}

/// Load and normalize a mangled name so it can be matched with string equality.
std::optional<std::string>
TypeRefBuilder::ReflectionTypeDescriptorFinder::normalizeReflectionName(
    RemoteRef<char> reflectionName) {
  const auto reflectionNameRemoteAddress = reflectionName.getAddressData();

  if (const auto found =
          NormalizedReflectionNameCache.find(reflectionNameRemoteAddress);
      found != NormalizedReflectionNameCache.end()) {
    return found->second;
  }

  TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
  // Remangle the reflection name to resolve symbolic references.
  if (auto node =
          Builder.demangleTypeRef(reflectionName,
                                  /*useOpaqueTypeSymbolicReferences*/ false)) {
    switch (node->getKind()) {
    case Node::Kind::TypeSymbolicReference:
    case Node::Kind::ProtocolSymbolicReference:
    case Node::Kind::OpaqueTypeDescriptorSymbolicReference:
      // Symbolic references cannot be mangled, return a failure.
      NormalizedReflectionNameCache.insert(std::make_pair(
          reflectionNameRemoteAddress, std::optional<std::string>()));
      return {};
    default:
      auto mangling = mangleNode(node);
      if (!mangling.isSuccess()) {
        NormalizedReflectionNameCache.insert(std::make_pair(
            reflectionNameRemoteAddress, std::optional<std::string>()));
        return {};
      }
      NormalizedReflectionNameCache.insert(
          std::make_pair(reflectionNameRemoteAddress, mangling.result()));
      return std::move(mangling.result());
    }
  }

  // Fall back to the raw string.
  const auto manglingResult = Builder.getTypeRefString(reflectionName).str();
  NormalizedReflectionNameCache.insert(
      std::make_pair(reflectionNameRemoteAddress, manglingResult));
  return std::move(manglingResult);
}

/// Determine whether the given reflection protocol name matches.
bool TypeRefBuilder::ReflectionTypeDescriptorFinder::reflectionNameMatches(
    RemoteRef<char> reflectionName, StringRef searchName) {
  auto normalized = normalizeReflectionName(reflectionName);
  if (!normalized)
    return false;
  return searchName == *normalized;
}

const TypeRef *
TypeRefBuilder::ReflectionTypeDescriptorFinder::lookupTypeWitness(
    const std::string &MangledTypeName, const std::string &Member,
    const StringRef Protocol) {
  TypeRefID key;
  key.addString(MangledTypeName);
  key.addString(Member);
  key.addString(Protocol.str());
  auto found = AssociatedTypeCache.find(key);
  if (found != AssociatedTypeCache.end())
    return found->second;

  // Cache missed - we need to look through all of the assocty sections
  // for all images that we've been notified about.
  for (auto &Info : ReflectionInfos) {
    for (auto AssocTyDescriptor : Info.AssociatedType) {
      if (!reflectionNameMatches(
              readTypeRef(AssocTyDescriptor,
                          AssocTyDescriptor->ConformingTypeName),
              MangledTypeName))
        continue;

      if (!reflectionNameMatches(
              readTypeRef(AssocTyDescriptor,
                          AssocTyDescriptor->ProtocolTypeName),
              Protocol))
        continue;

      for (auto &AssocTyRef : *AssocTyDescriptor.getLocalBuffer()) {
        auto AssocTy = AssocTyDescriptor.getField(AssocTyRef);
        if (Member.compare(
                Builder.getTypeRefString(readTypeRef(AssocTy, AssocTy->Name))
                    .str()) != 0)
          continue;

        TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
        auto SubstitutedTypeName =
            readTypeRef(AssocTy, AssocTy->SubstitutedTypeName);
        auto Demangled = Builder.demangleTypeRef(SubstitutedTypeName);
        auto *TypeWitness = Builder.decodeMangledType(Demangled);

        AssociatedTypeCache.insert(std::make_pair(key, TypeWitness));
        return TypeWitness;
      }
    }
  }
  return nullptr;
}

const TypeRef *TypeRefBuilder::lookupSuperclass(const TypeRef *TR) {
  const auto &FD = getFieldDescriptor(TR);
  if (FD == nullptr)
    return nullptr;

  if (!FD->HasSuperClass)
    return nullptr;

  ScopedNodeFactoryCheckpoint checkpoint(this);
  auto Demangled = FD->demangleSuperclass();
  auto Unsubstituted = decodeMangledType(Demangled);
  if (!Unsubstituted)
    return nullptr;

  auto SubstMap = TR->getSubstMap();
  if (!SubstMap)
    return nullptr;
  return Unsubstituted->subst(*this, *SubstMap);
}

static std::optional<StringRef> FindOutermostModuleName(NodePointer Node) {
  if (!Node)
    return {};
  // Breadth first search until we find the module name so we find the outermost
  // one.
  llvm::SmallVector<NodePointer, 8> Queue;
  Queue.push_back(Node);
  // Instead of removing items from the front of the queue we just iterate over
  // them.
  for (size_t i = 0; i < Queue.size(); ++i) {
    NodePointer Current = Queue[i];
    if (Current->getKind() == Node::Kind::Module) {
      if (Current->hasText())
        return Current->getText();
      else
        return {};
    }
    for (auto Child : *Current)
      Queue.push_back(Child);
  }
  return {};
}

void TypeRefBuilder::ReflectionTypeDescriptorFinder::
    populateFieldTypeInfoCacheWithReflectionAtIndex(size_t Index) {
  if (ProcessedReflectionInfoIndexes.contains(Index))
    return;

  llvm::SmallVector<std::string, 0> Names;
  const auto &Info = ReflectionInfos[Index];
  for (auto FD : Info.Field) {
    if (FD->hasMangledTypeName()) {
      auto CandidateMangledName = readTypeRef(FD, FD->MangledTypeName);
      if (auto NormalizedName = normalizeReflectionName(CandidateMangledName)) {
        if (ExternalTypeRefCache)
          Names.push_back(*NormalizedName);
        FieldTypeInfoCache[std::move(*NormalizedName)] = FD;
      }
    } else if (ExternalTypeRefCache) {
      // Mark the lack of a mangled name for this field descriptor with an empty
      // string.
      Names.push_back("");
    }
  }

  if (ExternalTypeRefCache)
    ExternalTypeRefCache->cacheFieldDescriptors(Index, Info.Field, Names);

  ProcessedReflectionInfoIndexes.insert(Index);
}

std::optional<RemoteRef<FieldDescriptor>>
TypeRefBuilder::ReflectionTypeDescriptorFinder::findFieldDescriptorAtIndex(
    size_t Index, const std::string &MangledName) {
  populateFieldTypeInfoCacheWithReflectionAtIndex(Index);
  auto Found = FieldTypeInfoCache.find(MangledName);
  if (Found != FieldTypeInfoCache.end()) {
    return Found->second;
  }
  return std::nullopt;
}

std::optional<RemoteRef<FieldDescriptor>>
TypeRefBuilder::ReflectionTypeDescriptorFinder::
    getFieldDescriptorFromExternalCache(const std::string &MangledName) {
  if (!ExternalTypeRefCache)
    return std::nullopt;

  if (auto Locator =
          ExternalTypeRefCache->getFieldDescriptorLocator(MangledName)) {
    if (Locator->InfoID >= ReflectionInfos.size())
      return std::nullopt;

    auto &Field = ReflectionInfos[Locator->InfoID].Field;
    auto Addr = Field.startAddress().getAddressData() + Locator->Offset;

    // Validate that we've got the correct field descriptor offset by parsing
    // the mangled name for that specific offset and making sure it's the one
    // we're looking for.
    for (auto FD : Field) {
      if (FD.getAddressData() == Addr) {
        if (!FD->hasMangledTypeName())
          break;
        auto CandidateMangledName = readTypeRef(FD, FD->MangledTypeName);
        if (auto NormalizedName =
                normalizeReflectionName(CandidateMangledName)) {
          FieldTypeInfoCache[std::move(*NormalizedName)] = FD;
          break;
        }
      }
    }
    auto Found = FieldTypeInfoCache.find(MangledName);
    if (Found != FieldTypeInfoCache.end())
      return Found->second;
  }
  return std::nullopt;
}

RemoteRef<FieldDescriptor>
TypeRefBuilder::ReflectionTypeDescriptorFinder::getFieldTypeInfo(
    const TypeRef *TR) {
  const std::string *MangledName;
  NodePointer Node;
  Demangler Dem;
  if (auto N = dyn_cast<NominalTypeRef>(TR)) {
    Node = N->getDemangling(Dem);
    MangledName = &N->getMangledName();
  } else if (auto BG = dyn_cast<BoundGenericTypeRef>(TR)) {
    Node = BG->getDemangling(Dem);
    MangledName = &BG->getMangledName();
  } else
    return nullptr;

  // Try the cache.
  auto Found = FieldTypeInfoCache.find(*MangledName);
  if (Found != FieldTypeInfoCache.end())
    return Found->second;

  if (auto FD = getFieldDescriptorFromExternalCache(*MangledName))
    return *FD;

  // Heuristic: find the outermost Module node available, and try to parse the
  // ReflectionInfos with a matching name first.
  auto ModuleName = FindOutermostModuleName(Node);
  // If we couldn't find a module name or the type is imported (__C module) we
  // don't any useful information on which image to look for the type.
  if (ModuleName && ModuleName != "__C") {
    for (size_t i = 0; i < ReflectionInfos.size(); ++i) {
      // If the external cache already has the contents of this reflection info,
      // and the previous lookup in the cache failed, then the field descriptor
      // we're looking for isn't in this reflection info.
      if (ExternalTypeRefCache &&
          ExternalTypeRefCache->isReflectionInfoCached(i))
        continue;
      if (llvm::is_contained(ReflectionInfos[i].PotentialModuleNames,
                             ModuleName))
        if (auto FD = findFieldDescriptorAtIndex(i, *MangledName))
          return *FD;
    }
  }

  // If the heuristic didn't work, iterate over every reflection info
  // that the external cache hasn't processed.
  for (size_t i = 0; i < ReflectionInfos.size(); ++i) {
    if (ExternalTypeRefCache && ExternalTypeRefCache->isReflectionInfoCached(i))
      continue;
    if (auto FD = findFieldDescriptorAtIndex(i, *MangledName))
      return *FD;
  }

  // If we still haven't found the field descriptor go over every reflection
  // info, even the ones the external cache supposedly processed.
  // TODO: if we find the field descriptor here there is a bug somewhere (most
  // likely on the external cache). Log this somehow.
  for (size_t i = 0; i < ReflectionInfos.size(); ++i)
    if (auto FD = findFieldDescriptorAtIndex(i, *MangledName))
      return *FD;

  return nullptr;
}

namespace {
// A field record implementation that wraps a reflection field record.
class FieldRecordImpl : public FieldRecordBase {
  RemoteRef<const FieldRecord> Field;
  TypeRefBuilder &Builder;

public:
  FieldRecordImpl(RemoteRef<const FieldRecord> FR, TypeRefBuilder &Builder)
      : FieldRecordBase(FR->isIndirectCase(), FR->isVar(),
                             FR->hasMangledTypeName()),
        Field(FR), Builder(Builder) {}

  ~FieldRecordImpl() override {}

  StringRef getFieldName() override {
    return Builder.getTypeRefString(
        Builder.readTypeRef(Field, Field->FieldName));
  }
    
  NodePointer getDemangledTypeName() override {
    return Builder.demangleTypeRef(
        Builder.readTypeRef(Field, Field->MangledTypeName));
  }
};

/// A field descriptor implementation that wraps a reflection field
/// descriptor.
class FieldDescriptorImpl : public FieldDescriptorBase {
  RemoteRef<FieldDescriptor> FD;
  TypeRefBuilder &Builder;

public:
  FieldDescriptorImpl(RemoteRef<FieldDescriptor> FD, TypeRefBuilder &Builder)
      : FieldDescriptorBase(FD->Kind, FD->hasSuperclass()), FD(FD),
        Builder(Builder) {}

  ~FieldDescriptorImpl() override {}

  NodePointer demangleSuperclass() override {
    return Builder.demangleTypeRef(Builder.readTypeRef(FD, FD->Superclass));
  }

  std::vector<std::unique_ptr<FieldRecordBase>> getFieldRecords() override {
    std::vector<std::unique_ptr<FieldRecordBase>> FieldRecords;
    for (auto &FieldRef : *FD.getLocalBuffer()) {
      FieldRecords.emplace_back(
          std::make_unique<FieldRecordImpl>(FD.getField(FieldRef), Builder));
    }
    return FieldRecords;
  }
};
} // namespace

std::unique_ptr<FieldDescriptorBase>
TypeRefBuilder::ReflectionTypeDescriptorFinder::getFieldDescriptor(
    const TypeRef *TR) {
  if (auto FDI = getFieldTypeInfo(TR))
    return std::make_unique<FieldDescriptorImpl>(FDI, Builder);
  return nullptr;
}

std::unique_ptr<FieldDescriptorBase>
TypeRefBuilder::getFieldDescriptor(const TypeRef *TR) {
  for (auto DF : getDescriptorFinders())
    if (auto descriptor = DF->getFieldDescriptor(TR))
      return descriptor;
  return nullptr;
}

bool TypeRefBuilder::getFieldTypeRefs(
    const TypeRef *TR, FieldDescriptorBase &FD,
    remote::TypeInfoProvider *ExternalTypeInfo,
    std::vector<FieldTypeInfo> &Fields) {
  auto Subs = TR->getSubstMap();
  if (!Subs)
    return false;

  int FieldValue = -1;
  for (auto &Field : FD.getFieldRecords()) {
    auto FieldName = Field->getFieldName();
    FieldValue += 1;

    // Empty cases of enums do not have a type
    if (FD.isEnum() && !Field->HasMangledTypeName) {
      Fields.push_back(
          FieldTypeInfo::forEmptyCase(FieldName.str(), FieldValue));
      continue;
    }

    ScopedNodeFactoryCheckpoint checkpoint(this);
    auto Demangled = Field->getDemangledTypeName();
    auto Unsubstituted = decodeMangledType(Demangled);
    if (!Unsubstituted)
      return false;

    // We need this for enums; an enum case "is generic" if any generic type
    // parameter substitutions occurred on the payload.  E.g.,
    // `case a([T?])` is generic, but `case a([Int?])` is not.
    bool IsGeneric = false;
    auto Substituted = Unsubstituted->subst(*this, *Subs, IsGeneric);
    bool IsIndirect = FD.isEnum() && Field->IsIndirectCase;

    auto FieldTI = FieldTypeInfo(FieldName.str(), FieldValue, Substituted,
                                 IsIndirect, IsGeneric);
    Fields.push_back(FieldTI);
  }
  return true;
}

RemoteRef<BuiltinTypeDescriptor>
TypeRefBuilder::ReflectionTypeDescriptorFinder::getBuiltinTypeInfo(
    const TypeRef *TR) {
  std::string MangledName;
  if (auto B = dyn_cast<BuiltinTypeRef>(TR))
    MangledName = B->getMangledName();
  else if (auto N = dyn_cast<NominalTypeRef>(TR))
    MangledName = N->getMangledName();
  else if (auto B = dyn_cast<BoundGenericTypeRef>(TR))
    MangledName = B->getMangledName();
  else
    return nullptr;

  for (; NormalizedReflectionNameCacheLastReflectionInfoCache <
         ReflectionInfos.size();
       NormalizedReflectionNameCacheLastReflectionInfoCache++) {
    for (auto BuiltinTypeDescriptor :
         ReflectionInfos[NormalizedReflectionNameCacheLastReflectionInfoCache]
             .Builtin) {
      if (BuiltinTypeDescriptor->Stride <= 0)
        continue;
      if (!BuiltinTypeDescriptor->hasMangledTypeName())
        continue;

      auto Alignment = BuiltinTypeDescriptor->getAlignment();
      if (Alignment <= 0)
        continue;
      // Reject any alignment that's not a power of two.
      if (Alignment & (Alignment - 1))
        continue;

      auto CandidateMangledName =
          readTypeRef(BuiltinTypeDescriptor, BuiltinTypeDescriptor->TypeName);
      auto CandidateNormalizedName =
          normalizeReflectionName(CandidateMangledName);
      if (CandidateNormalizedName) {
        BuiltInTypeDescriptorCache.insert(
            std::make_pair(*CandidateNormalizedName, BuiltinTypeDescriptor));
      }
    }
  }

  if (const auto found = BuiltInTypeDescriptorCache.find(MangledName);
      found != BuiltInTypeDescriptorCache.end()) {
    return found->second;
  }

  return nullptr;
}

namespace {
/// A builtin type descriptor implementation that wraps a reflection builtin
/// type descriptor.
class BuiltinTypeDescriptorImpl : public BuiltinTypeDescriptorBase {
  RemoteRef<BuiltinTypeDescriptor> BTD;
  TypeRefBuilder &Builder;

public:
  BuiltinTypeDescriptorImpl(RemoteRef<BuiltinTypeDescriptor> BTD,
                            TypeRefBuilder &Builder)
      : BuiltinTypeDescriptorBase(BTD->Size, BTD->getAlignment(),
                                       BTD->Stride, BTD->NumExtraInhabitants,
                                       BTD->isBitwiseTakable()),
        BTD(BTD), Builder(Builder) {}

  ~BuiltinTypeDescriptorImpl() override {}

  StringRef getMangledTypeName() override {
    return Builder.getTypeRefString(Builder.readTypeRef(BTD, BTD->TypeName));
  };
};
} // namespace

std::unique_ptr<BuiltinTypeDescriptorBase>
TypeRefBuilder::ReflectionTypeDescriptorFinder::getBuiltinTypeDescriptor(
    const TypeRef *TR) {
  if (auto BTI = getBuiltinTypeInfo(TR))
    return std::make_unique<BuiltinTypeDescriptorImpl>(BTI, Builder);
  return nullptr;
}

std::unique_ptr<BuiltinTypeDescriptorBase>
TypeRefBuilder::getBuiltinTypeDescriptor(const TypeRef *TR) {
  for (auto *DF : getDescriptorFinders())
    if (auto descriptor = DF->getBuiltinTypeDescriptor(TR))
      return descriptor;

  return nullptr;
}

RemoteRef<CaptureDescriptor>
TypeRefBuilder::ReflectionTypeDescriptorFinder::getCaptureDescriptor(
    uint64_t RemoteAddress) {
  for (auto Info : ReflectionInfos) {
    for (auto CD : Info.Capture) {
      if (RemoteAddress == CD.getAddressData()) {
        return CD;
      }
    }
  }

  return nullptr;
}

/// Get the unsubstituted capture types for a closure context.
ClosureContextInfo
TypeRefBuilder::ReflectionTypeDescriptorFinder::getClosureContextInfo(
    RemoteRef<CaptureDescriptor> CD) {
  ClosureContextInfo Info;

  for (auto i = CD->capture_begin(), e = CD->capture_end(); i != e; ++i) {
    const TypeRef *TR = nullptr;
    auto CR = CD.getField(*i);

    if (CR->hasMangledTypeName()) {
      TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
      auto MangledName = readTypeRef(CR, CR->MangledTypeName);
      auto DemangleTree = Builder.demangleTypeRef(MangledName);
      TR = Builder.decodeMangledType(DemangleTree);
    }
    Info.CaptureTypes.push_back(TR);
  }

  for (auto i = CD->source_begin(), e = CD->source_end(); i != e; ++i) {
    const TypeRef *TR = nullptr;
    auto MSR = CD.getField(*i);

    if (MSR->hasMangledTypeName()) {
      TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
      auto MangledName = readTypeRef(MSR, MSR->MangledTypeName);
      auto DemangleTree = Builder.demangleTypeRef(MangledName);
      TR = Builder.decodeMangledType(DemangleTree);
    }

    const MetadataSource *MS = nullptr;
    if (MSR->hasMangledMetadataSource()) {
      auto MangledMetadataSource = Builder.getTypeRefString(
          readTypeRef(MSR, MSR->MangledMetadataSource));
      MS = MetadataSource::decode(MSB, MangledMetadataSource.str());
    }

    Info.MetadataSources.push_back({TR, MS});
  }

  Info.NumBindings = CD->NumBindings;

  return Info;
}

///
/// Dumping reflection metadata
///

void TypeRefBuilder::ReflectionTypeDescriptorFinder::dumpTypeRef(
    RemoteRef<char> MangledName, std::ostream &stream, bool printTypeName) {
  TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
  auto DemangleTree = Builder.demangleTypeRef(MangledName);
  auto TypeName = nodeToString(DemangleTree);
  stream << TypeName << "\n";
  auto Result = swift::Demangle::decodeMangledType(Builder, DemangleTree);
  if (Result.isError()) {
    auto *Error = Result.getError();
    char *ErrorStr = Error->copyErrorString();
    auto str = Builder.getTypeRefString(MangledName);
    stream << "!!! Invalid typeref: " << str.str() << " - " << ErrorStr << "\n";
    Error->freeErrorString(ErrorStr);
    return;
  }
  auto TR = Result.getType();
  TR->dump(stream);
  stream << "\n";
}

FieldTypeCollectionResult
TypeRefBuilder::ReflectionTypeDescriptorFinder::collectFieldTypes(
    std::optional<std::string> forMangledTypeName) {
  FieldTypeCollectionResult result;
  for (const auto &sections : ReflectionInfos) {
    for (auto descriptor : sections.Field) {
      std::optional<std::string> optionalMangledTypeName;
      std::string typeName;
      {
        TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
        auto typeRef = readTypeRef(descriptor, descriptor->MangledTypeName);
        typeName = nodeToString(Builder.demangleTypeRef(typeRef));
        optionalMangledTypeName = normalizeReflectionName(typeRef);
      }
      if (optionalMangledTypeName.has_value()) {
        auto mangledTypeName = optionalMangledTypeName.value();
        if (forMangledTypeName.has_value()) {
          if (mangledTypeName != forMangledTypeName.value())
            continue;
        }

        std::vector<PropertyTypeInfo> properties;
        std::vector<EnumCaseInfo> enumCases;
        for (auto &fieldRef : *descriptor.getLocalBuffer()) {
          auto field = descriptor.getField(fieldRef);
          auto fieldName =
              Builder.getTypeRefString(readTypeRef(field, field->FieldName));
          if (field->hasMangledTypeName()) {
            std::string mangledFieldTypeName =
                std::string(field->MangledTypeName);
            auto fieldTypeRef = readTypeRef(field, field->MangledTypeName);
            auto optionalMangledfieldTypeName =
                normalizeReflectionName(fieldTypeRef);
            if (optionalMangledfieldTypeName.has_value()) {
              mangledFieldTypeName = optionalMangledfieldTypeName.value();
            }
            auto fieldTypeDemangleTree = Builder.demangleTypeRef(fieldTypeRef);
            auto fieldTypeName = nodeToString(fieldTypeDemangleTree);
            std::stringstream OS;
            dumpTypeRef(fieldTypeRef, OS);
            properties.emplace_back(PropertyTypeInfo{fieldName.str(),
                                                     mangledFieldTypeName,
                                                     fieldTypeName, OS.str()});
          } else {
            enumCases.emplace_back(EnumCaseInfo{fieldName.str()});
          }
        }
        result.FieldInfos.emplace_back(
            FieldMetadata{mangledTypeName, typeName, properties, enumCases});
      }
    }
  }

  return result;
}

void TypeRefBuilder::ReflectionTypeDescriptorFinder::dumpFieldSection(
    std::ostream &stream) {
  auto fieldInfoCollectionResult =
      collectFieldTypes(std::optional<std::string>());
  for (const auto &info : fieldInfoCollectionResult.FieldInfos) {
    stream << info.FullyQualifiedName << "\n";
    for (size_t i = 0; i < info.FullyQualifiedName.size(); ++i)
      stream << "-";
    stream << "\n";
    for (const auto &field : info.Properties) {
      stream << field.Label;
      stream << ": ";
      stream << field.TypeDiagnosticPrintName;
    }
    for (const auto &field : info.EnumCases) {
      stream << field.Label;
      stream << "\n\n";
    }
  }
}

void TypeRefBuilder::ReflectionTypeDescriptorFinder::dumpBuiltinTypeSection(
    std::ostream &stream) {
  for (const auto &sections : ReflectionInfos) {
    for (auto descriptor : sections.Builtin) {
      TypeRefBuilder::ScopedNodeFactoryCheckpoint checkpoint(&Builder);
      auto typeNode = Builder.demangleTypeRef(
          readTypeRef(descriptor, descriptor->TypeName));
      auto typeName = nodeToString(typeNode);

      stream << "\n- " << typeName << ":\n";
      stream << "Size: " << descriptor->Size << "\n";
      stream << "Alignment: " << descriptor->getAlignment() << ":\n";
      stream << "Stride: " << descriptor->Stride << ":\n";
      stream << "NumExtraInhabitants: " << descriptor->NumExtraInhabitants
             << ":\n";
      stream << "BitwiseTakable: " << descriptor->isBitwiseTakable() << ":\n";
    }
  }
}

void ClosureContextInfo::dump() const { dump(std::cerr); }

void ClosureContextInfo::dump(std::ostream &stream) const {
  stream << "- Capture types:\n";
  for (auto *TR : CaptureTypes) {
    if (TR == nullptr)
      stream << "!!! Invalid typeref\n";
    else
      TR->dump(stream);
  }
  stream << "- Metadata sources:\n";
  for (auto MS : MetadataSources) {
    if (MS.first == nullptr)
      stream << "!!! Invalid typeref\n";
    else
      MS.first->dump(stream);
    if (MS.second == nullptr)
      stream << "!!! Invalid metadata source\n";
    else
      MS.second->dump(stream);
  }
  stream << "\n";
}

void TypeRefBuilder::ReflectionTypeDescriptorFinder::dumpCaptureSection(
    std::ostream &stream) {
  for (const auto &sections : ReflectionInfos) {
    for (const auto descriptor : sections.Capture) {
      auto info = getClosureContextInfo(descriptor);
      info.dump(stream);
    }
  }
}

#endif
