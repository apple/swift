//===--- TypeRefBuilder.h - Swift Type Reference Builder --------*- C++ -*-===//
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

#ifndef SWIFT_REFLECTION_TYPEREFBUILDER_H
#define SWIFT_REFLECTION_TYPEREFBUILDER_H

#include "swift/Remote/ExternalTypeRefCache.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/RemoteInspection/MetadataSourceBuilder.h"
#include "swift/RemoteInspection/Records.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace swift {
namespace reflection {

using remote::RemoteRef;

template <typename Runtime> class ReflectionContext;

template <typename Iterator>
class ReflectionSection {
  using const_iterator = Iterator;
  RemoteRef<void> Start;
  uint64_t Size;

public:
  ReflectionSection(RemoteRef<void> Start, uint64_t Size)
    : Start(Start), Size(Size) {}

  RemoteRef<void> startAddress() const {
    return Start;
  }

  RemoteRef<void> endAddress() const {
    return Start.atByteOffset(Size);
  }

  const_iterator begin() const {
    return const_iterator(Start, Size);
  }

  const_iterator end() const {
    return const_iterator(endAddress(), 0);
  }

  size_t size() const {
    return Size;
  }
  
  bool containsRemoteAddress(uint64_t remoteAddr,
                             uint64_t size) const {
    return Start.getAddressData() <= remoteAddr
      && remoteAddr + size <= Start.getAddressData() + Size;
  }
  
  template<typename U>
  RemoteRef<U> getRemoteRef(uint64_t remoteAddr) const {
    assert(containsRemoteAddress(remoteAddr, sizeof(U)));
    auto localAddr = (uint64_t)(uintptr_t)Start.getLocalBuffer()
      + (remoteAddr - Start.getAddressData());
    
    return RemoteRef<U>(remoteAddr, (const U*)localAddr);
  }
};

template<typename Self, typename Descriptor>
class ReflectionSectionIteratorBase {
  uint64_t OriginalSize;
protected:
  Self &asImpl() {
    return *static_cast<Self *>(this);
  }
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = Descriptor;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

  RemoteRef<void> Cur;
  uint64_t Size;
  std::string Name;
    
  ReflectionSectionIteratorBase(RemoteRef<void> Cur, uint64_t Size, std::string Name)
    : OriginalSize(Size), Cur(Cur), Size(Size), Name(Name) {
    if (Size != 0) {
      auto NextRecord = this->operator*();
      if (!NextRecord) {
        // NULL record pointer, don't attempt to proceed. Setting size to 0 will
        // make this iterator compare equal to the end iterator.
        this->Size = 0;
        return;
      }
      auto NextSize = Self::getCurrentRecordSize(NextRecord);
      if (NextSize > Size) {
        std::cerr << "!!! Reflection section too small to contain first record\n" << std::endl;
        std::cerr << "Section Type: " << Name << std::endl;
        std::cerr << "Section size: "
                  << Size
                  << ", size of first record: "
                  << NextSize
                  << std::endl;
        // Set this iterator equal to the end. This section is effectively
        // empty.
        this->Size = 0;
      }
    }
  }

  RemoteRef<Descriptor> operator*() const {
    assert(Size > 0);
    return RemoteRef<Descriptor>(Cur.getAddressData(),
                                 (const Descriptor*)Cur.getLocalBuffer());
  }

  Self &operator++() {
    auto CurRecord = this->operator*();
    auto CurSize = Self::getCurrentRecordSize(CurRecord);
    Cur = Cur.atByteOffset(CurSize);
    Size -= CurSize;
    
    if (Size > 0) {
      auto NextRecord = this->operator*();
      auto NextSize = Self::getCurrentRecordSize(NextRecord);
      if (NextSize > Size) {
        int offset = (int)(OriginalSize - Size);
        std::cerr << "!!! Reflection section too small to contain next record\n" << std::endl;
        std::cerr << "Section Type: " << Name << std::endl;
        std::cerr << "Remaining section size: " << Size
                  << ", total section size: " << OriginalSize
                  << ", offset in section: " << offset
                  << ", size of next record: " << NextSize
                  << std::endl;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(Cur.getLocalBuffer());
        std::cerr << "Last bytes of previous record: ";
        for (int i = std::max(-8, -offset); i < 0; i++) {
          std::cerr << std::hex << std::setw(2) << (int)p[i] << " ";
        }
        std::cerr << std::endl;
        std::cerr << "Next bytes in section: ";
        for (unsigned i = 0; i < Size && i < 16; i++) {
          std::cerr << std::hex << std::setw(2) << (int)p[i] << " ";
        }
        std::cerr << std::endl;
        Size = 0; // Set this iterator equal to the end.
      }
    }

    return asImpl();
  }

  bool operator==(const Self &other) const {
    // Size = 0 means we're at the end even if Cur doesn't match. This allows
    // iterators that encounter an incorrect size to safely end iteration.
    if (Size == 0 && other.Size == 0)
      return true;
    return Cur == other.Cur && Size == other.Size;
  }

  bool operator!=(const Self &other) const {
    return !(*this == other);
  }
};

class FieldDescriptorIterator
  : public ReflectionSectionIteratorBase<FieldDescriptorIterator,
                                         FieldDescriptor>
{
public:
  FieldDescriptorIterator(RemoteRef<void> Cur, uint64_t Size)
    : ReflectionSectionIteratorBase(Cur, Size, "FieldDescriptor")
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<FieldDescriptor> FR) {
    return sizeof(FieldDescriptor) + FR->NumFields * FR->FieldRecordSize;
  }
};
using FieldSection = ReflectionSection<FieldDescriptorIterator>;

class AssociatedTypeIterator
  : public ReflectionSectionIteratorBase<AssociatedTypeIterator,
                                         AssociatedTypeDescriptor>
{
public:
  AssociatedTypeIterator(RemoteRef<void> Cur, uint64_t Size)
    : ReflectionSectionIteratorBase(Cur, Size, "AssociatedType")
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<AssociatedTypeDescriptor> ATR){
    return sizeof(AssociatedTypeDescriptor)
      + ATR->NumAssociatedTypes * ATR->AssociatedTypeRecordSize;
  }
};
using AssociatedTypeSection = ReflectionSection<AssociatedTypeIterator>;

class BuiltinTypeDescriptorIterator
  : public ReflectionSectionIteratorBase<BuiltinTypeDescriptorIterator,
                                         BuiltinTypeDescriptor> {
public:
  BuiltinTypeDescriptorIterator(RemoteRef<void> Cur, uint64_t Size)
    : ReflectionSectionIteratorBase(Cur, Size, "BuiltinTypeDescriptor")
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<BuiltinTypeDescriptor> ATR){
    return sizeof(BuiltinTypeDescriptor);
  }
};
using BuiltinTypeSection = ReflectionSection<BuiltinTypeDescriptorIterator>;

class CaptureDescriptorIterator
  : public ReflectionSectionIteratorBase<CaptureDescriptorIterator,
                                         CaptureDescriptor> {
public:
  CaptureDescriptorIterator(RemoteRef<void> Cur, uint64_t Size)
    : ReflectionSectionIteratorBase(Cur, Size, "CaptureDescriptor")
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<CaptureDescriptor> CR){
    return sizeof(CaptureDescriptor)
      + CR->NumCaptureTypes * sizeof(CaptureTypeRecord)
      + CR->NumMetadataSources * sizeof(MetadataSourceRecord);
  }
};
using CaptureSection = ReflectionSection<CaptureDescriptorIterator>;

class MultiPayloadEnumDescriptorIterator
  : public ReflectionSectionIteratorBase<MultiPayloadEnumDescriptorIterator,
                                         MultiPayloadEnumDescriptor> {
public:
  MultiPayloadEnumDescriptorIterator(RemoteRef<void> Cur, uint64_t Size)
    : ReflectionSectionIteratorBase(Cur, Size, "MultiPayloadEnum")
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<MultiPayloadEnumDescriptor> MPER) {
    return MPER->getSizeInBytes();
  }
};
using MultiPayloadEnumSection = ReflectionSection<MultiPayloadEnumDescriptorIterator>;

using GenericSection = ReflectionSection<const void *>;

struct ReflectionInfo {
  FieldSection Field;
  AssociatedTypeSection AssociatedType;
  BuiltinTypeSection Builtin;
  CaptureSection Capture;
  GenericSection TypeReference;
  GenericSection ReflectionString;
  GenericSection Conformance;
  MultiPayloadEnumSection MultiPayloadEnum;
  llvm::SmallVector<llvm::StringRef, 1> PotentialModuleNames;
};

struct ClosureContextInfo {
  std::vector<const TypeRef *> CaptureTypes;
  std::vector<std::pair<const TypeRef *, const MetadataSource *>> MetadataSources;
  unsigned NumBindings = 0;

  void dump() const;
  void dump(std::ostream &stream) const;
};

struct FieldTypeInfo {
  std::string Name;
  int Value;
  const TypeRef *TR;
  bool Indirect;
  bool Generic;

  FieldTypeInfo() : Name(""), Value(0), TR(nullptr), Indirect(false), Generic(false) {}
  FieldTypeInfo(const std::string &Name, int Value, const TypeRef *TR, bool Indirect, bool Generic)
    : Name(Name), Value(Value), TR(TR), Indirect(Indirect), Generic(Generic) {}

  static FieldTypeInfo forEmptyCase(std::string Name, int Value) {
    return FieldTypeInfo(Name, Value, nullptr, false, false);
  }

  static FieldTypeInfo forField(std::string Name, int Value, const TypeRef *TR) {
    return FieldTypeInfo(Name, Value, TR, false, false);
  }
};

/// Info about a protocol conformance read out from an Image
struct ProtocolConformanceInfo {
  std::string TypeName;
  std::string ProtocolName;
  std::string MangledTypeName;
};

struct ConformanceCollectionResult {
  std::vector<ProtocolConformanceInfo> Conformances;
  std::vector<std::string> Errors;
};

struct TypeAliasInfo {
  std::string TypeAliasName;
  std::string SubstitutedTypeMangledName;
  std::string SubstitutedTypeFullyQualifiedName;
  std::string SubstitutedTypeDiagnosticPrintName;
};

struct AssociatedType {
  TypeAliasInfo SubstitutionInfo;
  std::vector<std::string> OpaqueTypeProtocolConformanceRequirements;
  std::vector<TypeAliasInfo> OpaqueTypeSameTypeRequirements;
};

/// Info about all of a given type's associated types, as read out from an Image
struct AssociatedTypeInfo {
  std::string MangledTypeName;
  std::string FullyQualifiedName;
  std::string ProtocolFullyQualifiedName;
  std::vector<AssociatedType> AssociatedTypes;
};

struct AssociatedTypeCollectionResult {
  std::vector<AssociatedTypeInfo> AssociatedTypeInfos;
  std::vector<std::string> Errors;
};

struct PropertyTypeInfo {
  std::string Label;
  std::string TypeMangledName;
  std::string TypeFullyQualifiedName;
  std::string TypeDiagnosticPrintName;
};

struct EnumCaseInfo {
  std::string Label;
};

/// Info about all of a given type's fields, as read out from an Image
struct FieldMetadata {
  std::string MangledTypeName;
  std::string FullyQualifiedName;
  std::vector<PropertyTypeInfo> Properties;
  std::vector<EnumCaseInfo> EnumCases;
};

struct FieldTypeCollectionResult {
  std::vector<FieldMetadata> FieldInfos;
  std::vector<std::string> Errors;
};

struct TypeRefDecl {
  std::string mangledName;

  // Only used when building a bound generic typeref, and when the
  // generic params for all the levels are stored as a flat array.
  llvm::Optional<std::vector<size_t>> genericParamsPerLevel;

  TypeRefDecl(std::string mangledName, 
              std::vector<size_t> genericParamsPerLevel)
      : mangledName(mangledName), 
        genericParamsPerLevel(genericParamsPerLevel) {}

  TypeRefDecl(std::string mangledName) 
      : mangledName(mangledName), 
        genericParamsPerLevel(llvm::None) {}

};

/// An implementation of MetadataReader's BuilderType concept for
/// building TypeRefs, and parsing field metadata from any images
/// it has been made aware of.
///
/// Note that the TypeRefBuilder owns the memory for all TypeRefs
/// it vends.
class TypeRefBuilder {
#define TYPEREF(Id, Parent) friend class Id##TypeRef;
#include "swift/RemoteInspection/TypeRefs.def"

public:
  using BuiltType = const TypeRef *;
  using BuiltTypeDecl = llvm::Optional<TypeRefDecl>;
  using BuiltProtocolDecl =
      llvm::Optional<std::pair<std::string, bool /*isObjC*/>>;
  using BuiltSubstitution = std::pair<const TypeRef *, const TypeRef *>;
  using BuiltRequirement = TypeRefRequirement;
  using BuiltLayoutConstraint = TypeRefLayoutConstraint;
  using BuiltGenericTypeParam = const GenericTypeParameterTypeRef *;
  using BuiltGenericSignature = const GenericSignatureRef *;
  using BuiltSubstitutionMap = llvm::DenseMap<DepthAndIndex, const TypeRef *>;

  static constexpr bool needsToPrecomputeParentGenericContextShapes = true;

  TypeRefBuilder(const TypeRefBuilder &other) = delete;
  TypeRefBuilder &operator=(const TypeRefBuilder &other) = delete;

private:
  Demangle::Demangler Dem;

  /// Makes sure dynamically allocated TypeRefs stick around for the life of
  /// this TypeRefBuilder and are automatically released.
  std::vector<std::unique_ptr<const TypeRef>> TypeRefPool;

  /// Cache for associated type lookups.
  std::unordered_map<TypeRefID, const TypeRef *,
                     TypeRefID::Hash, TypeRefID::Equal> AssociatedTypeCache;

  /// Cache for field info lookups.
  std::unordered_map<std::string, RemoteRef<FieldDescriptor>> FieldTypeInfoCache;

  std::vector<std::unique_ptr<const GenericSignatureRef>> SignatureRefPool;

  TypeConverter TC;
  MetadataSourceBuilder MSB;

  remote::ExternalTypeRefCache *ExternalTypeRefCache = nullptr;

#define TYPEREF(Id, Parent) \
  std::unordered_map<TypeRefID, const Id##TypeRef *, \
                     TypeRefID::Hash, TypeRefID::Equal> Id##TypeRefs;
#include "swift/RemoteInspection/TypeRefs.def"

public:
  template <typename TypeRefTy, typename... Args>
  const TypeRefTy *makeTypeRef(Args... args) {
    const auto TR = new TypeRefTy(::std::forward<Args>(args)...);
    TypeRefPool.push_back(std::unique_ptr<const TypeRef>(TR));
    return TR;
  }

  template <typename... Args>
  const GenericSignatureRef *makeGenericSignatureRef(Args... args) {
    const auto TR = new GenericSignatureRef(::std::forward<Args>(args)...);
    SignatureRefPool.push_back(std::unique_ptr<const GenericSignatureRef>(TR));
    return TR;
  }

  Demangle::NodeFactory &getNodeFactory() { return Dem; }

  NodeFactory::Checkpoint pushNodeFactoryCheckpoint() const {
    return Dem.pushCheckpoint();
  }

  void popNodeFactoryCheckpoint(NodeFactory::Checkpoint checkpoint) {
    Dem.popCheckpoint(checkpoint);
  }

  class ScopedNodeFactoryCheckpoint {
    TypeRefBuilder *Builder;
    NodeFactory::Checkpoint Checkpoint;

  public:
    ScopedNodeFactoryCheckpoint(TypeRefBuilder *Builder)
        : Builder(Builder), Checkpoint(Builder->pushNodeFactoryCheckpoint()) {}

    ~ScopedNodeFactoryCheckpoint() {
      Builder->popNodeFactoryCheckpoint(Checkpoint);
    }
  };

  BuiltType decodeMangledType(Node *node, bool forRequirement = true);

  ///
  /// Factory methods for all TypeRef kinds
  ///

  const BuiltinTypeRef *createBuiltinType(const std::string &builtinName,
                                          const std::string &mangledName) {
    return BuiltinTypeRef::create(*this, mangledName);
  }

  BuiltTypeDecl createTypeDecl(Node *node, std::vector<size_t> paramsPerLevel) {
    auto mangling = Demangle::mangleNode(node);
    if (!mangling.isSuccess()) {
      return llvm::None;
    }
    return {{mangling.result(), paramsPerLevel}};
  }

  BuiltTypeDecl createTypeDecl(std::string &&mangledName,
                               std::vector<size_t> paramsPerLevel) {
    return {{std::move(mangledName), {paramsPerLevel}}};
  }

  BuiltTypeDecl createTypeDecl(Node *node, bool &typeAlias) {
    auto mangling = Demangle::mangleNode(node);
    if (!mangling.isSuccess()) {
      return llvm::None;
    }
    return {{mangling.result()}};
  }

  BuiltTypeDecl createTypeDecl(std::string &&mangledName,
                                             bool &typeAlias) {
    return {{(mangledName)}};;
  }

  BuiltProtocolDecl
  createProtocolDecl(Node *node) {
    auto mangling = Demangle::mangleNode(node);
    if (!mangling.isSuccess()) {
      return llvm::None;
    }
    return std::make_pair(mangling.result(), false);
  }

  BuiltProtocolDecl
  createObjCProtocolDecl(std::string &&name) {
    return std::make_pair(name, true);
  }


  const NominalTypeRef *
  createNominalType(const BuiltTypeDecl &typeRefDecl) {
    return NominalTypeRef::create(*this, typeRefDecl->mangledName, nullptr);
  }

  const NominalTypeRef *
  createNominalType(const BuiltTypeDecl &typeRefDecl,
                    const TypeRef *parent) {
    return NominalTypeRef::create(*this, typeRefDecl->mangledName, parent);
  }

  const TypeRef *
  createTypeAliasType(const BuiltTypeDecl &typeRefDecl,
                      const TypeRef *parent) {
    // TypeRefs don't contain sugared types
    return nullptr;
  }

  const TypeRef *createOptionalType(const TypeRef *base) {
    // TypeRefs don't contain sugared types
    return nullptr;
  }

  const TypeRef *createArrayType(const TypeRef *base) {
    // TypeRefs don't contain sugared types
    return nullptr;
  }

  const TypeRef *createDictionaryType(const TypeRef *key, const TypeRef *value) {
    // TypeRefs don't contain sugared types
    return nullptr;
  }

  const TypeRef *createParenType(const TypeRef *base) {
    // TypeRefs don't contain sugared types
    return nullptr;
  }

  // Construct a bound generic type ref along with the parent type info
  // The parent list contains every parent type with at least 1 generic
  // type parameter.
  const BoundGenericTypeRef *reconstructParentsOfBoundGenericType(
      const NodePointer startNode,
      const std::vector<size_t> &genericParamsPerLevel,
      const llvm::ArrayRef<const TypeRef *> &args)
  {
    // Collect the first N parents that potentially have generic args
    // (Ignore the last genericParamPerLevel, which
    // applies to the startNode itself.)
    std::vector<NodePointer> nodes;
    NodePointer node = startNode;
    while (nodes.size() < genericParamsPerLevel.size() - 1) {
      if (!node || !node->hasChildren()) {
	return nullptr;
      }
      node = node->getFirstChild();
      switch (node->getKind()) {
      case Node::Kind::Class:
      case Node::Kind::Structure:
      case Node::Kind::Enum:
	nodes.push_back(node);
	break;
      default:
	break;
      }
    }
    assert(nodes.size() == genericParamsPerLevel.size() - 1);

    // We're now going to build the type tree from the
    // outermost parent in, which matches the order of
    // the generic parameter list and genericParamsPerLevel.
    std::reverse(nodes.begin(), nodes.end());

    // Walk the list of parent types together with
    // the generic argument list...
    const BoundGenericTypeRef *typeref = nullptr;
    auto argBegin = args.begin();
    for (size_t i = 0; i < nodes.size(); i++) {
      // Get the mangling for this node
      auto mangling = Demangle::mangleNode(nodes[i]);
      if (!mangling.isSuccess()) {
	return nullptr;
      }

      // Use the next N params for this node
      auto numGenericArgs = genericParamsPerLevel[i];
      // Skip nodes that don't have any actual type params.
      if (numGenericArgs == 0) {
	continue;
      }
      auto argEnd = argBegin + numGenericArgs;
      std::vector<const TypeRef *> params(argBegin, argEnd);
      argBegin = argEnd;
      
      // Extend the typeref list towards the innermost type
      typeref = BoundGenericTypeRef::create(*this, mangling.result(), params, typeref);
    }
    return typeref;
  }

  const BoundGenericTypeRef *
  createBoundGenericType(const BuiltTypeDecl &builtTypeDecl,
                         const llvm::ArrayRef<const TypeRef *> &args) {
    if (!builtTypeDecl)
      return nullptr;

    // If there aren't generic params on the parent types, we just emit
    // a single BG typeref with all the generic args
    auto maybeGenericParamsPerLevel = builtTypeDecl->genericParamsPerLevel;
    if (!maybeGenericParamsPerLevel) {
      return BoundGenericTypeRef::create(*this, builtTypeDecl->mangledName, args, nullptr);
    }

    // Otherwise, work from a full demangle tree to produce a
    // typeref that includes information about parent generic args
    auto node = Dem.demangleType(builtTypeDecl->mangledName);
    if (!node || !node->hasChildren() || node->getKind() != Node::Kind::Type) {
      return nullptr;
    }
    auto startNode = node->getFirstChild();
    auto mangling = Demangle::mangleNode(startNode);
    if (!mangling.isSuccess()) {
      return nullptr;
    }

    // Sanity:  Verify that the generic params per level add
    // up exactly to the number of args we were provided, and
    // that we don't have a rediculous number of either one
    auto genericParamsPerLevel = *maybeGenericParamsPerLevel;
    if (genericParamsPerLevel.size() > 1000 || args.size() > 1000) {
      return nullptr;
    }
    size_t totalParams = 0;
    for (size_t i = 0; i < genericParamsPerLevel.size(); i++) {
      if (genericParamsPerLevel[i] > args.size()) {
	return nullptr;
      }
      totalParams += genericParamsPerLevel[i];
    }
    if (totalParams != args.size()) {
      return nullptr;
    }

    // Reconstruct all parents that have non-zero generic params
    auto parents = reconstructParentsOfBoundGenericType(startNode, genericParamsPerLevel, args);

    // Collect the final set of generic params for the
    // innermost type.  Note: This will sometimes be empty:
    // consider `Foo<Int, String>.Bar.Baz<Double>.Quux`
    // which has 2 parents in the parent list
    // (`Foo<Int,String>`, `Baz<Double>`), and the
    // startNode is `Quux` with no params.
    auto numGenericArgs = genericParamsPerLevel[genericParamsPerLevel.size() - 1];
    auto argBegin = args.end() - numGenericArgs;
    std::vector<const TypeRef *> params(argBegin, args.end());

    // Build and return the top typeref
    return BoundGenericTypeRef::create(*this, mangling.result(), params, parents);
  }

  const BoundGenericTypeRef *
  createBoundGenericType(const BuiltTypeDecl &builtTypeDecl,
                         llvm::ArrayRef<const TypeRef *> args,
                         const TypeRef *parent) {
    if (!builtTypeDecl)
      return nullptr;

    if (!builtTypeDecl->genericParamsPerLevel)
      return BoundGenericTypeRef::create(*this, builtTypeDecl->mangledName, args,
                                       parent);
    assert(parent == nullptr &&
           "Parent is not null but we're reconstructing the parent!");
    return createBoundGenericType(builtTypeDecl, args);
  }

  const TypeRef *
  resolveOpaqueType(NodePointer opaqueDescriptor,
                    llvm::ArrayRef<llvm::ArrayRef<const TypeRef *>> genericArgs,
                    unsigned ordinal) {
    // TODO: Produce a type ref for the opaque type if the underlying type isn't
    // available.

    // Try to resolve to the underlying type, if we can.
    if (opaqueDescriptor->getKind() ==
                            Node::Kind::OpaqueTypeDescriptorSymbolicReference) {
      auto underlyingTy = OpaqueUnderlyingTypeReader(
                                         opaqueDescriptor->getIndex(), ordinal);

      if (!underlyingTy)
        return nullptr;

      GenericArgumentMap subs;
      for (unsigned d = 0, de = genericArgs.size(); d < de; ++d) {
        auto argsForDepth = genericArgs[d];
        for (unsigned i = 0, ie = argsForDepth.size(); i < ie; ++i) {
          subs.insert({{d, i}, argsForDepth[i]});
        }
      }
      
      return underlyingTy->subst(*this, subs);
    }

    auto mangling = mangleNode(opaqueDescriptor,
                               SymbolicResolver(),
                               Dem);
    if (!mangling.isSuccess())
      return nullptr;

    // Otherwise, build a type ref that represents the opaque type.
    return OpaqueArchetypeTypeRef::create(*this,
                                          mangling.result(),
                                          nodeToString(opaqueDescriptor),
                                          ordinal,
                                          genericArgs);
  }

  const TupleTypeRef *createTupleType(llvm::ArrayRef<const TypeRef *> elements,
                                      std::string &&labels) {
    return TupleTypeRef::create(*this, elements, std::move(labels));
  }

  const TypeRef *createPackType(llvm::ArrayRef<const TypeRef *> elements) {
    // FIXME: Remote mirrors support for variadic generics.
    return nullptr;
  }

  const TypeRef *createSILPackType(llvm::ArrayRef<const TypeRef *> elements,
                                   bool isElementAddress) {
    // FIXME: Remote mirrors support for variadic generics.
    return nullptr;
  }

  const TypeRef *createPackExpansionType(const TypeRef *patternType,
                                         const TypeRef *countType) {
    // FIXME: Remote mirrors support for variadic generics.
    return nullptr;
  }

  const FunctionTypeRef *createFunctionType(
      llvm::ArrayRef<remote::FunctionParam<const TypeRef *>> params,
      const TypeRef *result, FunctionTypeFlags flags,
      FunctionMetadataDifferentiabilityKind diffKind,
      const TypeRef *globalActor) {
    return FunctionTypeRef::create(
        *this, params, result, flags, diffKind, globalActor);
  }

  const FunctionTypeRef *createImplFunctionType(
      Demangle::ImplParameterConvention calleeConvention,
      llvm::ArrayRef<Demangle::ImplFunctionParam<const TypeRef *>> params,
      llvm::ArrayRef<Demangle::ImplFunctionResult<const TypeRef *>> results,
      llvm::Optional<Demangle::ImplFunctionResult<const TypeRef *>> errorResult,
      ImplFunctionTypeFlags flags) {
    // Minimal support for lowered function types. These come up in
    // reflection as capture types. For the reflection library's
    // purposes, the only part that matters is the convention.
    FunctionTypeFlags funcFlags;
    switch (flags.getRepresentation()) {
    case Demangle::ImplFunctionRepresentation::Thick:
    case Demangle::ImplFunctionRepresentation::Closure:
      funcFlags = funcFlags.withConvention(FunctionMetadataConvention::Swift);
      break;
    case Demangle::ImplFunctionRepresentation::Thin:
    case Demangle::ImplFunctionRepresentation::Method:
    case Demangle::ImplFunctionRepresentation::ObjCMethod:
    case Demangle::ImplFunctionRepresentation::WitnessMethod:
      funcFlags = funcFlags.withConvention(FunctionMetadataConvention::Thin);
      break;
    case Demangle::ImplFunctionRepresentation::CFunctionPointer:
      funcFlags = funcFlags.withConvention(FunctionMetadataConvention::CFunctionPointer);
      break;
    case Demangle::ImplFunctionRepresentation::Block:
      funcFlags = funcFlags.withConvention(FunctionMetadataConvention::Block);
      break;
    }

    funcFlags = funcFlags.withConcurrent(flags.isSendable());
    funcFlags = funcFlags.withAsync(flags.isAsync());
    funcFlags = funcFlags.withDifferentiable(flags.isDifferentiable());

    FunctionMetadataDifferentiabilityKind diffKind;
    switch (flags.getDifferentiabilityKind()) {
    case ImplFunctionDifferentiabilityKind::NonDifferentiable:
      diffKind = FunctionMetadataDifferentiabilityKind::NonDifferentiable;
      break;
    case ImplFunctionDifferentiabilityKind::Forward:
      diffKind = FunctionMetadataDifferentiabilityKind::Forward;
      break;
    case ImplFunctionDifferentiabilityKind::Reverse:
      diffKind = FunctionMetadataDifferentiabilityKind::Reverse;
      break;
    case ImplFunctionDifferentiabilityKind::Normal:
      diffKind = FunctionMetadataDifferentiabilityKind::Normal;
      break;
    case ImplFunctionDifferentiabilityKind::Linear:
      diffKind = FunctionMetadataDifferentiabilityKind::Linear;
      break;
    }

    auto result = createTupleType({}, "");
    return FunctionTypeRef::create(
        *this, {}, result, funcFlags, diffKind, nullptr);
  }

  BuiltType createProtocolTypeFromDecl(BuiltProtocolDecl protocol) {
    if (protocol->second) {
      return llvm::cast<TypeRef>(createObjCProtocolType(protocol->first));
    } else {
      return llvm::cast<TypeRef>(createNominalType(TypeRefDecl(protocol->first)));
    }
  }

  const ProtocolCompositionTypeRef *
  createProtocolCompositionType(llvm::ArrayRef<BuiltProtocolDecl> protocols,
                                BuiltType superclass, bool isClassBound,
                                bool forRequirement = true) {
    std::vector<const TypeRef *> protocolRefs;
    for (const auto &protocol : protocols) {
      if (!protocol)
        continue;

      auto protocolType = createProtocolTypeFromDecl(*protocol);
      if (!protocolType)
        continue;
      protocolRefs.push_back(protocolType);
    }

    return ProtocolCompositionTypeRef::create(*this, protocolRefs, superclass,
                                              isClassBound);
  }

  const ConstrainedExistentialTypeRef *createConstrainedExistentialType(
      const TypeRef *base, llvm::ArrayRef<BuiltRequirement> constraints) {
    auto *baseProto = llvm::dyn_cast<ProtocolCompositionTypeRef>(base);
    if (!baseProto)
      return nullptr;
    return ConstrainedExistentialTypeRef::create(*this, baseProto, constraints);
  }

  const TypeRef *
  createSymbolicExtendedExistentialType(NodePointer shapeNode,
                                        llvm::ArrayRef<const TypeRef *> args) {
    // Can't handle this here.
    return nullptr;
  }

  const ExistentialMetatypeTypeRef *createExistentialMetatypeType(
      const TypeRef *instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = llvm::None) {
    return ExistentialMetatypeTypeRef::create(*this, instance);
  }

  const MetatypeTypeRef *createMetatypeType(
      const TypeRef *instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = llvm::None) {
    bool WasAbstract = (repr && *repr != ImplMetatypeRepresentation::Thin);
    return MetatypeTypeRef::create(*this, instance, WasAbstract);
  }

  const GenericTypeParameterTypeRef *
  createGenericTypeParameterType(unsigned depth, unsigned index) {
    return GenericTypeParameterTypeRef::create(*this, depth, index);
  }

  const DependentMemberTypeRef *
  createDependentMemberType(const std::string &member,
                            const TypeRef *base) {
    // Should not have unresolved dependent member types here.
    return nullptr;
  }

  const DependentMemberTypeRef *
  createDependentMemberType(const std::string &member,
                            const TypeRef *base,
                            BuiltProtocolDecl protocol) {
    // Objective-C protocols don't have dependent types.
    if (protocol->second)
      return nullptr;
    return DependentMemberTypeRef::create(*this, member, base,
                                          protocol->first);
  }

#define REF_STORAGE(Name, ...) \
  const Name##StorageTypeRef *create##Name##StorageType(const TypeRef *base) { \
    return Name##StorageTypeRef::create(*this, base); \
  }
#include "swift/AST/ReferenceStorage.def"

  const SILBoxTypeRef *createSILBoxType(const TypeRef *base) {
    return SILBoxTypeRef::create(*this, base);
  }

  using BuiltSILBoxField = typename SILBoxTypeWithLayoutTypeRef::Field;
  BuiltLayoutConstraint getLayoutConstraint(LayoutConstraintKind kind) {
    // FIXME: Implement this.
    return {};
  }
  BuiltLayoutConstraint
  getLayoutConstraintWithSizeAlign(LayoutConstraintKind kind, unsigned size,
                                   unsigned alignment) {
    // FIXME: Implement this.
    return {};
  }

  const SILBoxTypeWithLayoutTypeRef *createSILBoxTypeWithLayout(
      const llvm::SmallVectorImpl<BuiltSILBoxField> &Fields,
      const llvm::SmallVectorImpl<BuiltSubstitution> &Substitutions,
      const llvm::SmallVectorImpl<BuiltRequirement> &Requirements) {
    return SILBoxTypeWithLayoutTypeRef::create(*this, Fields, Substitutions,
                                               Requirements);
  }

  bool isExistential(const TypeRef *) {
    // FIXME: Implement this.
    return true;
  }

  const TypeRef *createDynamicSelfType(const TypeRef *selfType) {
    // TypeRefs should not contain DynamicSelfType.
    return nullptr;
  }

  const ObjCClassTypeRef *getUnnamedObjCClassType() {
    return createObjCClassType("");
  }

  const ObjCClassTypeRef *
  createObjCClassType(const std::string &name) {
    return ObjCClassTypeRef::create(*this, name);
  }

  const ObjCClassTypeRef *
  createBoundGenericObjCClassType(const std::string &name,
                                  llvm::ArrayRef<const TypeRef *> args) {
    // Remote reflection just ignores generic arguments for Objective-C
    // lightweight generic types, since they don't affect layout.
    return createObjCClassType(name);
  }

  const ObjCProtocolTypeRef *createObjCProtocolType(const std::string &name) {
    return ObjCProtocolTypeRef::create(*this, name);
  }

  const ForeignClassTypeRef *
  createForeignClassType(const std::string &mangledName) {
    return ForeignClassTypeRef::create(*this, mangledName);
  }

  const ForeignClassTypeRef *
  getUnnamedForeignClassType() {
    return createForeignClassType("");
  }

  const OpaqueTypeRef *getOpaqueType() {
    return OpaqueTypeRef::get();
  }

  BuiltGenericSignature
  createGenericSignature(llvm::ArrayRef<BuiltType> builtParams,
                         llvm::ArrayRef<BuiltRequirement> requirements) {
    std::vector<BuiltGenericTypeParam> params;
    for (auto &builtParam : builtParams) {
      auto *genericRef =
          llvm::dyn_cast<GenericTypeParameterTypeRef>(builtParam);
      if (!genericRef)
        return nullptr;
      params.push_back(genericRef);
    }
    return GenericSignatureRef::create(*this, params, requirements);
  }

  BuiltSubstitutionMap
  createSubstitutionMap(BuiltGenericSignature sig,
                        llvm::ArrayRef<BuiltType> replacements) {
    assert(sig->getParams().size() == replacements.size() &&
           "Not enough replacement parameters!");
    if (sig->getParams().size() != replacements.size())
      return BuiltSubstitutionMap{};

    BuiltSubstitutionMap map{};
    for (unsigned paramIdx : indices(sig->getParams())) {
      const auto *param = sig->getParams()[paramIdx];
      auto replacement = replacements[paramIdx];
      map[{param->getDepth(), param->getIndex()}] = replacement;
    }
    return map;
  }

  BuiltType subst(BuiltType subject, const BuiltSubstitutionMap &Subs) {
    return subject->subst(*this, Subs);
  }

  ///
  /// Parsing reflection metadata
  ///

  /// Add the ReflectionInfo and return a unique ID for the reflection image
  /// added. Since we only add reflection infos, the ID can be its index.
  /// We return a uint32_t since it's extremely unlikely we'll run out of
  /// indexes.
  uint32_t addReflectionInfo(ReflectionInfo I) {
    ReflectionInfos.push_back(I);
    auto InfoID = ReflectionInfos.size() - 1;
    assert(InfoID <= UINT32_MAX && "ReflectionInfo ID overflow");
    return InfoID;
  }

  const std::vector<ReflectionInfo> &getReflectionInfos() {
    return ReflectionInfos;
  }

public:
  enum ForTesting_t { ForTesting };
  
  // Only for testing. A TypeRefBuilder built this way will not be able to
  // decode records in remote memory.
  explicit TypeRefBuilder(ForTesting_t) : TC(*this) {}

private:
  std::vector<ReflectionInfo> ReflectionInfos;

  /// Indexes of Reflection Infos we've already processed.
  llvm::DenseSet<size_t> ProcessedReflectionInfoIndexes;

  llvm::Optional<std::string> normalizeReflectionName(RemoteRef<char> name);
  bool reflectionNameMatches(RemoteRef<char> reflectionName,
                             StringRef searchName);
  void populateFieldTypeInfoCacheWithReflectionAtIndex(size_t Index);
  llvm::Optional<RemoteRef<FieldDescriptor>>
  findFieldDescriptorAtIndex(size_t Index, const std::string &MangledName);

  llvm::Optional<RemoteRef<FieldDescriptor>>
  getFieldDescriptorFromExternalCache(const std::string &MangledName);

public:
  RemoteRef<char> readTypeRef(uint64_t remoteAddr);
  
  template<typename Record, typename Field>
  RemoteRef<char> readTypeRef(RemoteRef<Record> record,
                              const Field &field) {
    uint64_t remoteAddr = record.resolveRelativeFieldData(field);
    
    return readTypeRef(remoteAddr);
  }

  StringRef getTypeRefString(RemoteRef<char> record) {
    return Demangle::makeSymbolicMangledNameStringRef(record.getLocalBuffer());
  }
  
private:
  using RefDemangler = std::function<Demangle::Node * (RemoteRef<char>, bool)>;
  using UnderlyingTypeReader = std::function<const TypeRef* (uint64_t, unsigned)>;
  using ByteReader = std::function<remote::MemoryReader::ReadBytesResult (remote::RemoteAddress, unsigned)>;
  using StringReader = std::function<bool (remote::RemoteAddress, std::string &)>;
  using PointerReader = std::function<llvm::Optional<remote::RemoteAbsolutePointer> (remote::RemoteAddress, unsigned)>;
  using DynamicSymbolResolver = std::function<llvm::Optional<remote::RemoteAbsolutePointer> (remote::RemoteAddress)>;
  using IntVariableReader = std::function<llvm::Optional<uint64_t> (std::string, unsigned)>;

  // These fields are captured from the MetadataReader template passed into the
  // TypeRefBuilder struct, to isolate its template-ness from the rest of
  // TypeRefBuilder.
  unsigned PointerSize;
  RefDemangler TypeRefDemangler;
  UnderlyingTypeReader OpaqueUnderlyingTypeReader;

  // Opaque fields captured from the MetadataReader's MemoryReader
  ByteReader OpaqueByteReader;
  StringReader OpaqueStringReader;
  PointerReader OpaquePointerReader;
  DynamicSymbolResolver OpaqueDynamicSymbolResolver;
  IntVariableReader OpaqueIntVariableReader;

public:
  template<typename Runtime>
  TypeRefBuilder(remote::MetadataReader<Runtime, TypeRefBuilder> &reader,
                 remote::ExternalTypeRefCache *externalCache = nullptr)
      : TC(*this), ExternalTypeRefCache(externalCache),
      PointerSize(sizeof(typename Runtime::StoredPointer)),
      TypeRefDemangler(
      [this, &reader](RemoteRef<char> string, bool useOpaqueTypeSymbolicReferences) -> Demangle::Node * {
        return reader.demangle(string,
                               remote::MangledNameKind::Type,
                               Dem, useOpaqueTypeSymbolicReferences);
      }),
      OpaqueUnderlyingTypeReader(
      [&reader](uint64_t descriptorAddr, unsigned ordinal) -> const TypeRef* {
        return reader.readUnderlyingTypeForOpaqueTypeDescriptor(
          descriptorAddr, ordinal).getType();
      }),
      OpaqueByteReader([&reader](remote::RemoteAddress address, unsigned size) -> remote::MemoryReader::ReadBytesResult {
        return reader.Reader->readBytes(address, size);
      }),
      OpaqueStringReader([&reader](remote::RemoteAddress address, std::string &dest) -> bool {
        return reader.Reader->readString(address, dest);
      }),
      OpaquePointerReader([&reader](remote::RemoteAddress address, unsigned size) -> llvm::Optional<remote::RemoteAbsolutePointer> {
        return reader.Reader->readPointer(address, size);
      }),
      OpaqueDynamicSymbolResolver([&reader](remote::RemoteAddress address) -> llvm::Optional<remote::RemoteAbsolutePointer> {
        return reader.Reader->getDynamicSymbol(address);
      }),
      OpaqueIntVariableReader(
        [&reader](std::string symbol, unsigned size) -> llvm::Optional<uint64_t> {
          llvm::Optional<uint64_t> result;
          if (auto Reader = reader.Reader) {
            auto Addr = Reader->getSymbolAddress(symbol);
            if (Addr) {
              switch (size) {
              case 8: {
                uint64_t i;
                if (Reader->readInteger(Addr, &i)) {
                  result = i;
                }
                break;
              }
              case 4: {
                uint32_t i;
                if (Reader->readInteger(Addr, &i)) {
                  result = i;
                }
                break;
              }
              default: {
                assert(false && "Can only read 4- or 8-byte integer variables from image");
              }
              }
            }
          }
          return result;
      })
  { }

  Demangle::Node *demangleTypeRef(RemoteRef<char> string,
                                  bool useOpaqueTypeSymbolicReferences = true) {
    return TypeRefDemangler(string, useOpaqueTypeSymbolicReferences);
  }

  TypeConverter &getTypeConverter() { return TC; }

  const TypeRef *
  lookupTypeWitness(const std::string &MangledTypeName,
                    const std::string &Member,
                    StringRef Protocol);

  const TypeRef *lookupSuperclass(const TypeRef *TR);

  /// Load unsubstituted field types for a nominal type.
  RemoteRef<FieldDescriptor> getFieldTypeInfo(const TypeRef *TR);

  /// Get the parsed and substituted field types for a nominal type.
  bool getFieldTypeRefs(const TypeRef *TR, RemoteRef<FieldDescriptor> FD,
                        remote::TypeInfoProvider *ExternalTypeInfo,
                        std::vector<FieldTypeInfo> &Fields);

  /// Get the primitive type lowering for a builtin type.
  RemoteRef<BuiltinTypeDescriptor> getBuiltinTypeInfo(const TypeRef *TR);

  /// Get the raw capture descriptor for a remote capture descriptor
  /// address.
  RemoteRef<CaptureDescriptor> getCaptureDescriptor(uint64_t RemoteAddress);

  /// Get the unsubstituted capture types for a closure context.
  ClosureContextInfo getClosureContextInfo(RemoteRef<CaptureDescriptor> CD);

  /// Get the multipayload enum projection information for a given TR
  RemoteRef<MultiPayloadEnumDescriptor> getMultiPayloadEnumInfo(const TypeRef *TR);

private:
  llvm::Optional<uint64_t> multiPayloadEnumPointerMask;

public:
  /// Retrieve the MPE pointer mask from the target
  // If it can't read it, it will make an educated guess
  // Note: This is a pointer-sized value stored in a uint64_t
  // If the target is 32 bits, the mask is in the lower 32 bits
  uint64_t getMultiPayloadEnumPointerMask() {
    unsigned pointerSize = TC.targetPointerSize();
    if (!multiPayloadEnumPointerMask.has_value()) {
      // Ask the target for the spare bits mask
      multiPayloadEnumPointerMask
        = OpaqueIntVariableReader("_swift_debug_multiPayloadEnumPointerSpareBitsMask", pointerSize);
    }
    if (!multiPayloadEnumPointerMask.has_value()) {
      if (pointerSize == sizeof(void *)) {
        // Most reflection tools run on the target machine,
        // in which case, they use the same configuration:
        multiPayloadEnumPointerMask = _swift_abi_SwiftSpareBitsMask;
      } else if (pointerSize == 4) {
        // All 32-bit platforms are the same, so this is always correct.
        multiPayloadEnumPointerMask = SWIFT_ABI_ARM_SWIFT_SPARE_BITS_MASK;
      } else {
        // This is not always correct.  But we can't do any better?
        multiPayloadEnumPointerMask = SWIFT_ABI_ARM64_SWIFT_SPARE_BITS_MASK;
      }
    }
    return multiPayloadEnumPointerMask.value();
  }

  ///
  /// Dumping typerefs, field declarations, builtin types, captures, multi-payload enums
  ///
  void dumpTypeRef(RemoteRef<char> MangledName, std::ostream &stream,
                   bool printTypeName = false);
  FieldTypeCollectionResult collectFieldTypes(llvm::Optional<std::string> forMangledTypeName);
  void dumpFieldSection(std::ostream &stream);
  void dumpBuiltinTypeSection(std::ostream &stream);
  void dumpCaptureSection(std::ostream &stream);
  void dumpMultiPayloadEnumSection(std::ostream &stream);

  ///
  /// Extraction of associated types
  ///
public:
  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  AssociatedTypeCollectionResult
  collectAssociatedTypes(llvm::Optional<std::string> forMangledTypeName) {
    AssociatedTypeCollectionResult result;
    for (const auto &sections : ReflectionInfos) {
      for (auto descriptor : sections.AssociatedType) {
        // Read out the relevant info from the associated type descriptor:
        // The type's name and which protocol conformance it corresponds to
        llvm::Optional<std::string> optionalMangledTypeName;
        std::string typeName;
        std::string protocolName;
        {
          ScopedNodeFactoryCheckpoint checkpoint(this);
          auto typeRef =
              readTypeRef(descriptor, descriptor->ConformingTypeName);
          typeName = nodeToString(demangleTypeRef(typeRef));
          optionalMangledTypeName = normalizeReflectionName(typeRef);
          auto protocolNode = demangleTypeRef(
              readTypeRef(descriptor, descriptor->ProtocolTypeName));
          protocolName = nodeToString(protocolNode);
        }
        if (optionalMangledTypeName.has_value()) {
          auto mangledTypeName = optionalMangledTypeName.value();
          if (forMangledTypeName.has_value()) {
            if (mangledTypeName != forMangledTypeName.value())
              continue;
          }

          // For each associated type, gather its typealias name,
          // the substituted type info, and if the substituted type is opaque -
          // gather its protocol conformance requirements
          std::vector<AssociatedType> associatedTypes;
          for (const auto &associatedTypeRef : *descriptor.getLocalBuffer()) {
            auto associatedType = descriptor.getField(associatedTypeRef);
            std::string typealiasTypeName =
                getTypeRefString(
                    readTypeRef(associatedType, associatedType->Name))
                    .str();

            std::string mangledSubstitutedTypeName =
                std::string(associatedType->SubstitutedTypeName);
            auto substitutedTypeRef = readTypeRef(
                associatedType, associatedType->SubstitutedTypeName);
            auto optionalMangledSubstitutedTypeName =
                normalizeReflectionName(substitutedTypeRef);
            if (optionalMangledSubstitutedTypeName.has_value()) {
              mangledSubstitutedTypeName =
                  optionalMangledSubstitutedTypeName.value();
            }

            // We intentionally do not want to resolve opaque type
            // references, because if the substituted type is opaque, we
            // would like to get at its OpaqueTypeDescriptor address, which
            // is stored on the OpaqueTypeDescriptorSymbolicReference typeRef.
            auto substitutedDemangleTree =
                demangleTypeRef(substitutedTypeRef,
                                /* useOpaqueTypeSymbolicReferences */ true);

            // If the substituted type is an opaque type, also gather info
            // about which protocols it is required to conform to and the corresponding
            // same-type requirements
            std::vector<std::string> opaqueTypeConformanceRequirements;
            std::vector<TypeAliasInfo> sameTypeRequirements;
            gatherOpaqueTypeRequirements<ObjCInteropKind, PointerSize>(
                substitutedDemangleTree, opaqueTypeConformanceRequirements,
                sameTypeRequirements);

            auto substitutedTypeName = nodeToString(substitutedDemangleTree);
            std::stringstream OS;
            dumpTypeRef(substitutedTypeRef, OS);
            associatedTypes.emplace_back(AssociatedType{
                TypeAliasInfo{typealiasTypeName, mangledSubstitutedTypeName,
                              substitutedTypeName, OS.str()},
                opaqueTypeConformanceRequirements, sameTypeRequirements});
          }
          result.AssociatedTypeInfos.emplace_back(AssociatedTypeInfo{
              mangledTypeName, typeName, protocolName, associatedTypes});
        }
      }
    }
    return result;
  }

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  void gatherOpaqueTypeRequirements(
      Demangle::Node *substitutedTypeDemangleTree,
      std::vector<std::string> &opaqueTypeConformanceRequirements,
      std::vector<TypeAliasInfo> &sameTypeRequirements) {
    // With unresolved opaque symbolic references, the demangle tree we
    // extract the opaque type descriptor's address from is of the form:
    // kind=Type
    //  kind=OpaqueType
    //    kind=OpaqueTypeDescriptorSymbolicReference, index={{1-9+}}
    // Where the `index` value is the descriptor's address
    //
    if (substitutedTypeDemangleTree->getKind() == Node::Kind::Type) {
      auto childDemangleTree = substitutedTypeDemangleTree->getFirstChild();
      if (childDemangleTree->getKind() == Node::Kind::OpaqueType) {
        auto opaqueTypeChildDemangleTree = childDemangleTree->getFirstChild();
        if (opaqueTypeChildDemangleTree->getKind() ==
            Node::Kind::OpaqueTypeDescriptorSymbolicReference) {
          extractOpaqueTypeProtocolRequirements<ObjCInteropKind, PointerSize>(
            opaqueTypeChildDemangleTree->getIndex(),
            opaqueTypeConformanceRequirements,
            sameTypeRequirements);
        }
      }
    }
  }

private:
  struct ContextNameInfo {
    std::string name;
    uintptr_t descriptorAddress;
    bool isAnonymous;

    ~ContextNameInfo() {}
  };

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  struct QualifiedContextNameReader {
    std::string Error;
    ByteReader OpaqueByteReader;
    StringReader OpaqueStringReader;
    PointerReader OpaquePointerReader;
    DynamicSymbolResolver OpaqueDynamicSymbolResolver;

    QualifiedContextNameReader(ByteReader byteReader,
                               StringReader stringReader,
                               PointerReader pointerReader,
                               DynamicSymbolResolver dynamicSymbolResolver)
        : Error(""), OpaqueByteReader(byteReader),
          OpaqueStringReader(stringReader),
          OpaquePointerReader(pointerReader),
          OpaqueDynamicSymbolResolver(dynamicSymbolResolver) {}

    llvm::Optional<std::string> readProtocolNameFromProtocolDescriptor(
        uintptr_t protocolDescriptorAddress) {
      std::string protocolName;
      auto protocolDescriptorBytes = OpaqueByteReader(
          remote::RemoteAddress(protocolDescriptorAddress),
          sizeof(ExternalProtocolDescriptor<ObjCInteropKind, PointerSize>));
      if (!protocolDescriptorBytes.get()) {
        Error = "Error reading protocol descriptor.";
        return llvm::None;
      }
      const ExternalProtocolDescriptor<ObjCInteropKind, PointerSize>
          *protocolDescriptor =
              (const ExternalProtocolDescriptor<ObjCInteropKind, PointerSize> *)
                  protocolDescriptorBytes.get();

      // Compute the address of the protocol descriptor's name field and read
      // the offset
      auto protocolNameOffsetAddress = detail::applyRelativeOffset(
          (const char *)protocolDescriptorAddress,
          (int32_t)protocolDescriptor->getNameOffset());
      auto protocolNameOffsetBytes = OpaqueByteReader(
          remote::RemoteAddress(protocolNameOffsetAddress), sizeof(uint32_t));
      if (!protocolNameOffsetBytes.get()) {
        Error = "Failed to read type name offset in a protocol descriptor.";
        return llvm::None;
      }
      auto protocolNameOffset = (const uint32_t *)protocolNameOffsetBytes.get();

      // Using the offset above, compute the address of the name field itsel
      // and read it.
      auto protocolNameAddress =
          detail::applyRelativeOffset((const char *)protocolNameOffsetAddress,
                                      (int32_t)*protocolNameOffset);
      OpaqueStringReader(remote::RemoteAddress(protocolNameAddress),
                         protocolName);
      return protocolName;
    }

    llvm::Optional<std::string> readTypeNameFromTypeDescriptor(
        const ExternalTypeContextDescriptor<ObjCInteropKind, PointerSize>
            *typeDescriptor,
        uintptr_t typeDescriptorAddress) {
      auto typeNameOffsetAddress =
          detail::applyRelativeOffset((const char *)typeDescriptorAddress,
                                      (int32_t)typeDescriptor->getNameOffset());
      auto typeNameOffsetBytes = OpaqueByteReader(
          remote::RemoteAddress(typeNameOffsetAddress), sizeof(uint32_t));
      if (!typeNameOffsetBytes.get()) {
        Error = "Failed to read type name offset in a type descriptor.";
        return llvm::None;
      }
      auto typeNameOffset = (const uint32_t *)typeNameOffsetBytes.get();
      auto typeNameAddress = detail::applyRelativeOffset(
          (const char *)typeNameOffsetAddress, (int32_t)*typeNameOffset);
      std::string typeName;
      OpaqueStringReader(remote::RemoteAddress(typeNameAddress), typeName);
      return typeName;
    }

    llvm::Optional<std::string> readModuleNameFromModuleDescriptor(
        const ExternalModuleContextDescriptor<ObjCInteropKind, PointerSize>
            *moduleDescriptor,
        uintptr_t moduleDescriptorAddress) {
      auto parentNameOffsetAddress = detail::applyRelativeOffset(
          (const char *)moduleDescriptorAddress,
          (int32_t)moduleDescriptor->getNameOffset());
      auto parentNameOffsetBytes = OpaqueByteReader(
          remote::RemoteAddress(parentNameOffsetAddress), sizeof(uint32_t));
      if (!parentNameOffsetBytes.get()) {
        Error = "Failed to read parent name offset in a module descriptor.";
        return llvm::None;
      }
      auto parentNameOffset = (const uint32_t *)parentNameOffsetBytes.get();
      auto parentNameAddress = detail::applyRelativeOffset(
          (const char *)parentNameOffsetAddress, (int32_t)*parentNameOffset);
      std::string parentName;
      OpaqueStringReader(remote::RemoteAddress(parentNameAddress), parentName);
      return parentName;
    }

    llvm::Optional<std::string> readAnonymousNameFromAnonymousDescriptor(
        const ExternalAnonymousContextDescriptor<ObjCInteropKind, PointerSize>
            *anonymousDescriptor,
        uintptr_t anonymousDescriptorAddress) {
      if (!anonymousDescriptor->hasMangledName()) {
        std::stringstream stream;
        stream << "(unknown context at $" << std::hex
               << anonymousDescriptorAddress << ")";
        return stream.str();
      }
      return llvm::None;
    }

    llvm::Optional<std::string>
    readFullyQualifiedTypeName(uintptr_t typeDescriptorTarget) {
      std::string typeName;
      auto contextTypeDescriptorBytes = OpaqueByteReader(
          remote::RemoteAddress(typeDescriptorTarget),
          sizeof(ExternalContextDescriptor<ObjCInteropKind, PointerSize>));
      if (!contextTypeDescriptorBytes.get()) {
        Error = "Failed to read context descriptor.";
        return llvm::None;
      }
      const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
          *contextDescriptor =
              (const ExternalContextDescriptor<ObjCInteropKind, PointerSize> *)
                  contextTypeDescriptorBytes.get();

      auto typeDescriptor =
          dyn_cast<ExternalTypeContextDescriptor<ObjCInteropKind, PointerSize>>(
              contextDescriptor);
      if (!typeDescriptor) {
        Error = "Unexpected type of context descriptor.";
        return llvm::None;
      }

      auto optionalTypeName = readTypeNameFromTypeDescriptor(
          typeDescriptor, typeDescriptorTarget);
      if (!optionalTypeName.has_value())
        return llvm::None;
      else
        typeName = optionalTypeName.value();

      std::vector<ContextNameInfo> contextNameChain;
      contextNameChain.push_back(
          ContextNameInfo{typeName, typeDescriptorTarget, false});
      getParentContextChain(typeDescriptorTarget, contextDescriptor,
                            contextNameChain);
      return constructFullyQualifiedNameFromContextChain(contextNameChain);
    }

    llvm::Optional<std::string>
    readFullyQualifiedProtocolName(
        uintptr_t protocolDescriptorTarget) {
      llvm::Optional<std::string> protocolName;
      // Set low bit indicates that this is an indirect
      // reference
      if (protocolDescriptorTarget & 0x1) {
        auto adjustedProtocolDescriptorTarget = protocolDescriptorTarget & ~0x1;
        if (auto symbol = OpaquePointerReader(
                remote::RemoteAddress(adjustedProtocolDescriptorTarget),
                PointerSize)) {
          if (!symbol->getSymbol().empty()) {
            Demangle::Context Ctx;
            auto demangledRoot =
                Ctx.demangleSymbolAsNode(symbol->getSymbol().str());
            assert(demangledRoot->getKind() == Node::Kind::Global);
            assert(demangledRoot->getChild(0)->getKind() ==
                   Node::Kind::ProtocolDescriptor);
            protocolName =
                nodeToString(demangledRoot->getChild(0)->getChild(0));
          } else {
            // This is an absolute address of a protocol descriptor
            auto protocolDescriptorAddress = (uintptr_t)symbol->getOffset();
            protocolName =
                readFullyQualifiedProtocolNameFromProtocolDescriptor(
                    protocolDescriptorAddress);
          }
        } else {
          Error = "Error reading external protocol address.";
          return llvm::None;
        }
      } else {
        // If this is a direct reference, get symbol name from the protocol
        // descriptor.
        protocolName =
            readFullyQualifiedProtocolNameFromProtocolDescriptor(
                protocolDescriptorTarget);
      }
      return protocolName;
    }

  private:
    llvm::Optional<std::string>
    readFullyQualifiedProtocolNameFromProtocolDescriptor(
        uintptr_t protocolDescriptorAddress) {
      llvm::Optional<std::string> protocolName =
        readProtocolNameFromProtocolDescriptor(protocolDescriptorAddress);

      // Read the protocol conformance descriptor itself
      auto protocolContextDescriptorBytes = OpaqueByteReader(
          remote::RemoteAddress(protocolDescriptorAddress),
          sizeof(ExternalContextDescriptor<ObjCInteropKind, PointerSize>));
      if (!protocolContextDescriptorBytes.get()) {
        return llvm::None;
      }
      const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
          *protocolDescriptor =
              (const ExternalContextDescriptor<ObjCInteropKind, PointerSize> *)
                  protocolContextDescriptorBytes.get();

      std::vector<ContextNameInfo> contextNameChain;
      contextNameChain.push_back(ContextNameInfo{
          protocolName.value(), protocolDescriptorAddress, false});
      getParentContextChain(protocolDescriptorAddress, protocolDescriptor,
                            contextNameChain);
      return constructFullyQualifiedNameFromContextChain(contextNameChain);
    }

    uintptr_t getParentDescriptorAddress(
        uintptr_t contextDescriptorAddress,
        const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
            *contextDescriptor) {
      auto parentOffsetAddress = detail::applyRelativeOffset(
          (const char *)contextDescriptorAddress,
          (int32_t)contextDescriptor->getParentOffset());
      auto parentOfsetBytes = OpaqueByteReader(
          remote::RemoteAddress(parentOffsetAddress), sizeof(uint32_t));
      auto parentFieldOffset = (const int32_t *)parentOfsetBytes.get();
      auto parentTargetAddress = detail::applyRelativeOffset(
          (const char *)parentOffsetAddress, *parentFieldOffset);
      return parentTargetAddress;
    }

    llvm::Optional<ContextNameInfo>
    getContextName(uintptr_t contextDescriptorAddress,
                   const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
                       *contextDescriptor) {
      if (auto moduleDescriptor = dyn_cast<
              ExternalModuleContextDescriptor<ObjCInteropKind, PointerSize>>(
              contextDescriptor)) {
        auto moduleDescriptorName = readModuleNameFromModuleDescriptor(
            moduleDescriptor, contextDescriptorAddress);
        if (!moduleDescriptorName.has_value())
          return llvm::None;
        else
          return ContextNameInfo{moduleDescriptorName.value(),
                                 contextDescriptorAddress, false};
      } else if (auto typeDescriptor = dyn_cast<ExternalTypeContextDescriptor<
                     ObjCInteropKind, PointerSize>>(contextDescriptor)) {
        auto typeDescriptorName = readTypeNameFromTypeDescriptor(
            typeDescriptor, contextDescriptorAddress);
        if (!typeDescriptorName.has_value())
          return llvm::None;
        else
          return ContextNameInfo{typeDescriptorName.value(),
                                 contextDescriptorAddress, false};
      } else if (auto anonymousDescriptor =
                     dyn_cast<ExternalAnonymousContextDescriptor<
                         ObjCInteropKind, PointerSize>>(contextDescriptor)) {
        auto anonymousDescriptorName = readAnonymousNameFromAnonymousDescriptor(
            anonymousDescriptor, contextDescriptorAddress);
        if (!anonymousDescriptorName.has_value())
          return llvm::None;
        else
          return ContextNameInfo{anonymousDescriptorName.value(),
                                 contextDescriptorAddress, true};
      } else {
        Error = "Unexpected type of context descriptor.";
        return llvm::None;
      }
    }

    bool isModuleDescriptor(
        const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
            *contextDescriptor) {
      return isa<ExternalModuleContextDescriptor<ObjCInteropKind, PointerSize>>(
          contextDescriptor);
    }

    void getParentContextChain(
        uintptr_t contextDescriptorAddress,
        const ExternalContextDescriptor<ObjCInteropKind, PointerSize>
            *contextDescriptor,
        std::vector<ContextNameInfo> &chain) {
      const auto parentDescriptorAddress = getParentDescriptorAddress(
          contextDescriptorAddress, contextDescriptor);

      auto addParentNameAndRecurse =
          [&](uintptr_t parentContextDescriptorAddress,
              std::vector<ContextNameInfo> &chain) -> void {
        auto parentContextDescriptorBytes = OpaqueByteReader(
            remote::RemoteAddress(parentContextDescriptorAddress),
            sizeof(ExternalContextDescriptor<ObjCInteropKind, PointerSize>));
        if (!parentContextDescriptorBytes.get()) {
          Error = "Failed to read context descriptor.";
          return;
        }
        const auto parentDescriptor =
            (const ExternalContextDescriptor<ObjCInteropKind, PointerSize> *)
                parentContextDescriptorBytes.get();
        const auto parentNameInfo =
            getContextName(parentContextDescriptorAddress, parentDescriptor);
        if (!parentNameInfo.has_value()) {
          return;
        }
        chain.push_back(parentNameInfo.value());
        if (!isModuleDescriptor(parentDescriptor)) {
          getParentContextChain(parentContextDescriptorAddress,
                                parentDescriptor, chain);
        }
      };

      // Set low bit indicates that this is an indirect reference
      if (parentDescriptorAddress & 0x1) {
        auto adjustedParentTargetAddress = parentDescriptorAddress & ~0x1;
        if (auto symbol = OpaquePointerReader(
                remote::RemoteAddress(adjustedParentTargetAddress),
                PointerSize)) {
          if (!symbol->getSymbol().empty()) {
            Demangle::Context Ctx;
            auto demangledRoot =
                Ctx.demangleSymbolAsNode(symbol->getSymbol().str());
            assert(demangledRoot->getKind() == Node::Kind::Global);
            std::string nodeName =
                nodeToString(demangledRoot->getChild(0)->getChild(0));
            chain.push_back(
                ContextNameInfo{nodeName, adjustedParentTargetAddress, false});
          } else {
            addParentNameAndRecurse(adjustedParentTargetAddress, chain);
          }
        } else {
          Error = "Error reading external symbol address.";
          return;
        }
      } else {
        addParentNameAndRecurse(parentDescriptorAddress, chain);
      }
      return;
    }

    std::string constructFullyQualifiedNameFromContextChain(
        const std::vector<ContextNameInfo> &contextNameChain) {
      std::string newQualifiedTypeName = "";
      std::vector<std::string> reversedQualifiedTypeNameMembers;

      // Traverse the context chain, adding up context names.
      // Anonymous contexts require special handling: when a type is nested in
      // an anonymous context, its qualified name is printed as `(type_name in
      // $hex_val)` where hex_val is the address of the descriptor of the
      // anonymous parent context.
      bool skipNext = false;
      for (size_t i = 0; i < contextNameChain.size(); ++i) {
        if (skipNext) {
          skipNext = false;
          continue;
        }
        const auto &contextNameInfo = contextNameChain[i];
        bool lastContext = (i == contextNameChain.size() - 1);
        bool currentContextIsAnonymous = contextNameInfo.isAnonymous;
        bool nextContextIsAnonymous =
            lastContext ? false : contextNameChain[i + 1].isAnonymous;
        if (nextContextIsAnonymous && !currentContextIsAnonymous) {
          std::stringstream stream;
          stream << "(" << contextNameInfo.name << " in $" << std::hex
                 << contextNameChain[i + 1].descriptorAddress << ")";
          reversedQualifiedTypeNameMembers.push_back(stream.str());
          skipNext = true;
        } else if (nextContextIsAnonymous && currentContextIsAnonymous) {

        } else if (!nextContextIsAnonymous && !currentContextIsAnonymous) {
          reversedQualifiedTypeNameMembers.push_back(contextNameInfo.name);
        } else if (!nextContextIsAnonymous && currentContextIsAnonymous) {
          reversedQualifiedTypeNameMembers.push_back(contextNameInfo.name);
        } else {
          llvm_unreachable("Exhausted possibilities.");
        }
      }

      // Combine the individual context name reps into a single fully-qualified
      // name string
      for (auto it = reversedQualifiedTypeNameMembers.rbegin();
           it != reversedQualifiedTypeNameMembers.rend(); ++it) {
        newQualifiedTypeName.append(*it);
        if (std::next(it) != reversedQualifiedTypeNameMembers.rend()) {
          newQualifiedTypeName.append(".");
        }
      }

      return newQualifiedTypeName;
    }
  };

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  void dumpAssociatedTypeSection(std::ostream &stream) {
    auto associatedTypeCollectionResult =
        collectAssociatedTypes<ObjCInteropKind, PointerSize>(
            llvm::Optional<std::string>());
    for (const auto &info :
         associatedTypeCollectionResult.AssociatedTypeInfos) {
      stream << "- " << info.FullyQualifiedName << " : "
             << info.ProtocolFullyQualifiedName << "\n";
      for (const auto &typeAlias : info.AssociatedTypes) {
        stream << "typealias " << typeAlias.SubstitutionInfo.TypeAliasName << " = "
               << typeAlias.SubstitutionInfo.SubstitutedTypeFullyQualifiedName << "\n";
        stream << typeAlias.SubstitutionInfo.SubstitutedTypeDiagnosticPrintName;
        if (!typeAlias.OpaqueTypeProtocolConformanceRequirements.empty()) {
          stream << "-------------------------\n";
          stream << "conformance requirements: \n";
          for (const auto &protocolName :
               typeAlias.OpaqueTypeProtocolConformanceRequirements) {
            stream << protocolName << "\n";
          }
        }
        if (!typeAlias.OpaqueTypeSameTypeRequirements.empty()) {
          stream << "-----------------------\n";
          stream << "same-type requirements: \n";
          for (const auto &sameTypeRequirementInfo :
               typeAlias.OpaqueTypeSameTypeRequirements) {
            stream << sameTypeRequirementInfo.TypeAliasName << " = "
                   << sameTypeRequirementInfo.SubstitutedTypeMangledName << " ("
                   << sameTypeRequirementInfo.SubstitutedTypeFullyQualifiedName
                   << ")\n";
          }
        }
      }
      stream << "\n";
    }
  }

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  void extractOpaqueTypeProtocolRequirements(
      uintptr_t opaqueTypeDescriptorAddress,
      std::vector<std::string> &protocolRequirements,
      std::vector<TypeAliasInfo> &sameTypeRequirements) {
    auto opaqueTypeDescriptorBytes = OpaqueByteReader(
        remote::RemoteAddress(opaqueTypeDescriptorAddress),
        sizeof(ExternalOpaqueTypeDescriptor<ObjCInteropKind, PointerSize>));
    if (!opaqueTypeDescriptorBytes.get()) {
      return;
    }
    const ExternalOpaqueTypeDescriptor<ObjCInteropKind, PointerSize>
        *opaqueTypeDescriptor =
            (const ExternalOpaqueTypeDescriptor<ObjCInteropKind, PointerSize> *)
                opaqueTypeDescriptorBytes.get();

    if (!opaqueTypeDescriptor) {
      return;
    }

    // Given that at a given offset from the opaque type descriptor base there
    // is an offset to a TypeRef string, read it.
    auto readRequirementTypeRefAddress =
        [&](uintptr_t offsetFromOpaqueDescBase,
            uintptr_t requirementAddress) -> uintptr_t {
      std::string typeRefString = "";
      auto fieldOffsetOffset = requirementAddress + offsetFromOpaqueDescBase -
                               (uintptr_t)opaqueTypeDescriptor;
      auto fieldOffsetAddress = opaqueTypeDescriptorAddress + fieldOffsetOffset;
      auto fieldOffsetBytes = OpaqueByteReader(
          remote::RemoteAddress(fieldOffsetAddress), sizeof(uint32_t));
      auto fieldOffset = (const int32_t *)fieldOffsetBytes.get();
      auto fieldAddress = detail::applyRelativeOffset(
          (const char *)fieldOffsetAddress, *fieldOffset);
      return fieldAddress;
    };

    for (const auto &req : opaqueTypeDescriptor->getGenericRequirements()) {
      if (req.getKind() == GenericRequirementKind::Protocol) {
        // Compute the address of the protocol descriptor offset as:
        // opaqueTypeDescriptorAddress + offset of the protocol descriptor
        // offset in the descriptor
        auto protocolDescriptorOffsetOffset = (uintptr_t)(&req) +
                                              req.getProtocolOffset() -
                                              (uintptr_t)opaqueTypeDescriptor;
        auto protocolDescriptorOffsetAddress =
            opaqueTypeDescriptorAddress + protocolDescriptorOffsetOffset;
        auto protocolDescriptorOffsetValue = req.getUnresolvedProtocolAddress();

        // Compute the address of the protocol descriptor by following the
        // offset
        auto protocolDescriptorAddress = detail::applyRelativeOffset(
            (const char *)protocolDescriptorOffsetAddress,
            protocolDescriptorOffsetValue);

        auto nameReader =
            QualifiedContextNameReader<ObjCInteropKind, PointerSize>(
                OpaqueByteReader, OpaqueStringReader, OpaquePointerReader,
                OpaqueDynamicSymbolResolver);
        auto conformanceRequirementProtocolName =
            nameReader.readFullyQualifiedProtocolName(
                protocolDescriptorAddress);
        protocolRequirements.push_back(*conformanceRequirementProtocolName);
      }
      if (req.getKind() == GenericRequirementKind::SameType) {
        // Read Param Name
        auto paramAddress = readRequirementTypeRefAddress(req.getParamOffset(),
                                                          (uintptr_t)(&req));
        std::string demangledParamName =
            nodeToString(demangleTypeRef(readTypeRef(paramAddress)));

        // Read the substituted Type Name
        auto typeAddress = readRequirementTypeRefAddress(
            req.getSameTypeNameOffset(), (uintptr_t)(&req));
        auto typeTypeRef = readTypeRef(typeAddress);
        std::string demangledTypeName =
            nodeToString(demangleTypeRef(typeTypeRef));
        std::string mangledTypeName;
        auto typeMangling = Demangle::mangleNode(demangleTypeRef(typeTypeRef));
        if (!typeMangling.isSuccess())
          mangledTypeName = "";
        else
          mangledTypeName = typeMangling.result();
        sameTypeRequirements.push_back(TypeAliasInfo{
            demangledParamName, mangledTypeName, demangledTypeName, ""});
      }
    }
    return;
  }

  ///
  /// Extraction of protocol conformances
  ///
private:
  /// Reader of protocol descriptors from Images
  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  struct ProtocolConformanceDescriptorReader {
    std::string Error;
    PointerReader OpaquePointerReader;
    ByteReader OpaqueByteReader;
    DynamicSymbolResolver OpaqueDynamicSymbolResolver;
    QualifiedContextNameReader<ObjCInteropKind, PointerSize> NameReader;

    ProtocolConformanceDescriptorReader(ByteReader byteReader,
                                        StringReader stringReader,
                                        PointerReader pointerReader,
                                        DynamicSymbolResolver dynamicSymbolResolver)
        : Error(""),
          OpaquePointerReader(pointerReader), OpaqueByteReader(byteReader),
          OpaqueDynamicSymbolResolver(dynamicSymbolResolver),
          NameReader(byteReader, stringReader, pointerReader, dynamicSymbolResolver) {}

    /// Extract conforming type's name from a Conformance Descriptor
    /// Returns a pair of (mangledTypeName, fullyQualifiedTypeName)
    llvm::Optional<std::pair<std::string, std::string>> getConformingTypeName(
        const uintptr_t conformanceDescriptorAddress,
        const ExternalProtocolConformanceDescriptor<
            ObjCInteropKind, PointerSize> &conformanceDescriptor) {
      std::string typeName;
      std::string mangledTypeName = "";

      // If this is a conformance added to an ObjC class, detect that here and return class name
      if (conformanceDescriptor.getTypeKind() == TypeReferenceKind::DirectObjCClassName) {
        auto className = conformanceDescriptor.getDirectObjCClassName();
        typeName = MANGLING_MODULE_OBJC.str() + std::string(".") + className;
        return std::make_pair(mangledTypeName, typeName);
      }

      // Compute the address of the type descriptor as follows:
      //    - Compute the address of the TypeRef field in the protocol
      //    descriptor
      //    - Read the TypeRef field to compute the offset to the type
      //    descriptor
      //    - Address of the type descriptor is found at the (2) offset from the
      //      conformance descriptor address
      auto contextDescriptorFieldAddress = detail::applyRelativeOffset(
          (const char *)conformanceDescriptorAddress,
          (int32_t)conformanceDescriptor.getTypeRefDescriptorOffset());
      auto contextDescriptorOffsetBytes =
          OpaqueByteReader(remote::RemoteAddress(contextDescriptorFieldAddress),
                           sizeof(uint32_t));
      if (!contextDescriptorOffsetBytes.get()) {
        Error =
            "Failed to read type descriptor field in conformance descriptor.";
        return llvm::None;
      }
      auto contextDescriptorOffset =
          (const int32_t *)contextDescriptorOffsetBytes.get();

      // Read the type descriptor itself using the address computed above
      auto contextTypeDescriptorAddress = detail::applyRelativeOffset(
          (const char *)contextDescriptorFieldAddress,
          *contextDescriptorOffset);

      // Instead of a type descriptor this may just be a reference to an
      // external, check that first
      if (auto symbol = OpaqueDynamicSymbolResolver(
              remote::RemoteAddress(contextTypeDescriptorAddress))) {
        if (!symbol->isResolved()) {
          Demangle::Context Ctx;
          auto demangledRoot =
            Ctx.demangleSymbolAsNode(symbol->getSymbol().str());
          assert(demangledRoot->getKind() == Node::Kind::Global);
          auto nomTypeDescriptorRoot = demangledRoot->getChild(0);
          assert(nomTypeDescriptorRoot->getKind() == Node::Kind::NominalTypeDescriptor);
          auto typeRoot = nomTypeDescriptorRoot->getChild(0);
          typeName = nodeToString(typeRoot);

          auto typeMangling = Demangle::mangleNode(typeRoot);
          if (!typeMangling.isSuccess())
            mangledTypeName = "";
          else
            mangledTypeName = typeMangling.result();

          return std::make_pair(mangledTypeName, typeName);
        } else if (symbol->getOffset()) {
          // If symbol is empty and has an offset, this is the resolved remote address
          contextTypeDescriptorAddress = symbol->getOffset();
        }
      }

      auto fullyQualifiedName =
          NameReader.readFullyQualifiedTypeName(contextTypeDescriptorAddress);
      if (!fullyQualifiedName.has_value())
        return llvm::None;
      else
        return std::make_pair(mangledTypeName, *fullyQualifiedName);
    }

    /// Extract protocol name from a Conformance Descriptor
    llvm::Optional<std::string> getConformanceProtocolName(
        const uintptr_t conformanceDescriptorAddress,
        const ExternalProtocolConformanceDescriptor<
            ObjCInteropKind, PointerSize> &conformanceDescriptor) {
      llvm::Optional<std::string> protocolName;
      auto protocolDescriptorFieldAddress = detail::applyRelativeOffset(
          (const char *)conformanceDescriptorAddress,
          (int32_t)conformanceDescriptor.getProtocolDescriptorOffset());

      auto protocolDescriptorOffsetBytes = OpaqueByteReader(
          remote::RemoteAddress(protocolDescriptorFieldAddress),
          sizeof(uint32_t));
      if (!protocolDescriptorOffsetBytes.get()) {
        Error = "Error reading protocol descriptor field in conformance "
                "descriptor.";
        return llvm::None;
      }
      auto protocolDescriptorOffset =
          (const uint32_t *)protocolDescriptorOffsetBytes.get();

      auto protocolDescriptorTarget = detail::applyRelativeOffset(
          (const char *)protocolDescriptorFieldAddress,
          (int32_t)*protocolDescriptorOffset);

      return NameReader.readFullyQualifiedProtocolName(
          protocolDescriptorTarget);
    }

    /// Given the address of a conformance descriptor, attempt to read it.
    llvm::Optional<ProtocolConformanceInfo>
    readConformanceDescriptor(RemoteRef<void> conformanceRecordRef,
                              const std::unordered_map<std::string, std::string>
                                  &typeNameToManglingMap) {
      const ExternalProtocolConformanceRecord<ObjCInteropKind,
                                              PointerSize> *CD =
          (const ExternalProtocolConformanceRecord<ObjCInteropKind, PointerSize>
               *)conformanceRecordRef.getLocalBuffer();
      // Read the Protocol Conformance Descriptor by getting its address from
      // the conformance record.
      auto conformanceDescriptorAddress = (uintptr_t)CD->getRelative(
          (void *)conformanceRecordRef.getAddressData());

      auto descriptorBytes = OpaqueByteReader(
          remote::RemoteAddress(conformanceDescriptorAddress),
          sizeof(ExternalProtocolConformanceDescriptor<ObjCInteropKind,
                                                       PointerSize>));
      if (!descriptorBytes.get()) {
        Error = "Failed to read protocol conformance descriptor.";
        return llvm::None;
      }
      const ExternalProtocolConformanceDescriptor<ObjCInteropKind, PointerSize>
          *conformanceDescriptorPtr =
              (const ExternalProtocolConformanceDescriptor<
                  ObjCInteropKind, PointerSize> *)descriptorBytes.get();

      auto optionalConformingTypeNamePair = getConformingTypeName(
          conformanceDescriptorAddress, *conformanceDescriptorPtr);
      if (!optionalConformingTypeNamePair.has_value())
        return llvm::None;

      auto optionalConformanceProtocol = getConformanceProtocolName(
          conformanceDescriptorAddress, *conformanceDescriptorPtr);
      if (!optionalConformanceProtocol.has_value())
        return llvm::None;

      std::string mangledTypeName;
      if (optionalConformingTypeNamePair.value().first.empty()) {
        auto it = typeNameToManglingMap.find(optionalConformingTypeNamePair.value().second);
        if (it != typeNameToManglingMap.end()) {
          mangledTypeName = it->second;
        } else {
          mangledTypeName = "";
        }
      } else {
        mangledTypeName = optionalConformingTypeNamePair.value().first;
      }

      return ProtocolConformanceInfo{optionalConformingTypeNamePair.value().second,
                                     optionalConformanceProtocol.value(),
                                     mangledTypeName};
    }
  };
public:
  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  ConformanceCollectionResult collectAllConformances() {
    ConformanceCollectionResult result;

    // The Fields section has gathered info on types that includes their mangled
    // names. Use that to build a dictionary from a type's demangled name to its
    // mangled name
    std::unordered_map<std::string, std::string> typeNameToManglingMap;
    for (const auto &section : ReflectionInfos) {
      for (auto descriptor : section.Field) {
        ScopedNodeFactoryCheckpoint checkpoint(this);
        auto TypeRef = readTypeRef(descriptor, descriptor->MangledTypeName);
        auto OptionalMangledTypeName = normalizeReflectionName(TypeRef);
        auto TypeName = nodeToString(demangleTypeRef(TypeRef));
        if (OptionalMangledTypeName.has_value()) {
          typeNameToManglingMap[TypeName] = OptionalMangledTypeName.value();
        }
      }
    }

    // Collect all conformances and aggregate them per-conforming-type.
    std::unordered_map<std::string, std::vector<std::string>> typeConformances;
    ProtocolConformanceDescriptorReader<ObjCInteropKind, PointerSize>
        conformanceReader(OpaqueByteReader, OpaqueStringReader,
                          OpaquePointerReader, OpaqueDynamicSymbolResolver);
    for (const auto &section : ReflectionInfos) {
      auto ConformanceBegin = section.Conformance.startAddress();
      auto ConformanceEnd = section.Conformance.endAddress();
      for (auto conformanceAddr = ConformanceBegin;
           conformanceAddr != ConformanceEnd;
           conformanceAddr = conformanceAddr.atByteOffset(4)) {
        auto optionalConformanceInfo =
            conformanceReader.readConformanceDescriptor(conformanceAddr,
                                                        typeNameToManglingMap);
        if (!optionalConformanceInfo.has_value())
          result.Errors.push_back(conformanceReader.Error);
        else
          result.Conformances.push_back(optionalConformanceInfo.value());
      }
    }
    return result;
  }

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  void dumpConformanceSection(std::ostream &stream) {
    auto conformanceCollectionResult = collectAllConformances<ObjCInteropKind, PointerSize>();

    // Collect all conformances and aggregate them per-conforming-type.
    std::unordered_map<std::string, std::vector<std::string>> typeConformances;
    for (auto &conformanceInfo : conformanceCollectionResult.Conformances) {
      auto typeConformancesKey = conformanceInfo.MangledTypeName + " (" +
                                 conformanceInfo.TypeName + ")";
      if (typeConformances.count(typeConformancesKey) != 0) {
        typeConformances[typeConformancesKey].push_back(
            conformanceInfo.ProtocolName);
      } else {
        typeConformances.emplace(
            typeConformancesKey,
            std::vector<std::string>{conformanceInfo.ProtocolName});
      }
    }
    for (auto &pair : typeConformances) {
      stream << pair.first << " : ";
      bool first = true;
      for (auto &protocol : pair.second) {
        if (!first) {
          stream << ", ";
        }
        first = false;
        stream << protocol;
      }
      stream << "\n";
    }

    // Report encountered errors
    for (auto &error : conformanceCollectionResult.Errors) {
      stream << "Error reading conformance descriptor: "
             << error << "\n";
    }
  }

  template <template <typename Runtime> class ObjCInteropKind,
            unsigned PointerSize>
  void dumpAllSections(std::ostream &stream) {
    stream << "FIELDS:\n";
    stream << "=======\n";
    dumpFieldSection(stream);
    stream << "\n";
    stream << "ASSOCIATED TYPES:\n";
    stream << "=================\n";
    dumpAssociatedTypeSection<ObjCInteropKind, PointerSize>(stream);
    stream << "\n";
    stream << "BUILTIN TYPES:\n";
    stream << "==============\n";
    dumpBuiltinTypeSection(stream);
    stream << "\n";
    stream << "CAPTURE DESCRIPTORS:\n";
    stream << "====================\n";
    dumpCaptureSection(stream);
    stream << "\n";
    stream << "CONFORMANCES:\n";
    stream << "=============\n";
    dumpConformanceSection<ObjCInteropKind, PointerSize>(stream);
    stream << "\n";
    stream << "MULTI-PAYLOAD ENUM DESCRIPTORS:\n";
    stream << "===============================\n";
    dumpMultiPayloadEnumSection(stream);
    stream << "\n";
  }
};

} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_TYPEREFBUILDER_H
