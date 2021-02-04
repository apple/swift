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

#include "swift/Remote/MetadataReader.h"
#include "swift/Reflection/MetadataSourceBuilder.h"
#include "swift/Reflection/Records.h"
#include "swift/Reflection/TypeLowering.h"
#include "swift/Reflection/TypeRef.h"
#include "llvm/ADT/Optional.h"

#include <vector>
#include <unordered_map>

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
class ReflectionSectionIteratorBase
  : public std::iterator<std::forward_iterator_tag, Descriptor> {
protected:
  Self &asImpl() {
    return *static_cast<Self *>(this);
  }
public:
  RemoteRef<void> Cur;
  uint64_t Size;
    
  ReflectionSectionIteratorBase(RemoteRef<void> Cur, uint64_t Size)
    : Cur(Cur), Size(Size) {
    if (Size != 0 && Self::getCurrentRecordSize(this->operator*()) > Size) {
      fputs("reflection section too small!\n", stderr);
      abort();
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
        fputs("reflection section too small!\n", stderr);
        abort();
      }
    }

    return asImpl();
  }

  bool operator==(const Self &other) const {
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
    : ReflectionSectionIteratorBase(Cur, Size)
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
    : ReflectionSectionIteratorBase(Cur, Size)
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
    : ReflectionSectionIteratorBase(Cur, Size)
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
    : ReflectionSectionIteratorBase(Cur, Size)
  {}

  static uint64_t getCurrentRecordSize(RemoteRef<CaptureDescriptor> CR){
    return sizeof(CaptureDescriptor)
      + CR->NumCaptureTypes * sizeof(CaptureTypeRecord)
      + CR->NumMetadataSources * sizeof(MetadataSourceRecord);
  }
};
using CaptureSection = ReflectionSection<CaptureDescriptorIterator>;
using GenericSection = ReflectionSection<const void *>;

struct ReflectionInfo {
  FieldSection Field;
  AssociatedTypeSection AssociatedType;
  BuiltinTypeSection Builtin;
  CaptureSection Capture;
  GenericSection TypeReference;
  GenericSection ReflectionString;
};

struct ClosureContextInfo {
  std::vector<const TypeRef *> CaptureTypes;
  std::vector<std::pair<const TypeRef *, const MetadataSource *>> MetadataSources;
  unsigned NumBindings = 0;

  void dump() const;
  void dump(FILE *file) const;
};

struct FieldTypeInfo {
  std::string Name;
  int Value;
  const TypeRef *TR;
  bool Indirect;

  FieldTypeInfo() : Name(""), Value(0), TR(nullptr), Indirect(false) {}
  FieldTypeInfo(const std::string &Name, int Value, const TypeRef *TR, bool Indirect)
    : Name(Name), Value(Value), TR(TR), Indirect(Indirect) {}

  static FieldTypeInfo forEmptyCase(std::string Name, int Value) {
    return FieldTypeInfo(Name, Value, nullptr, false);
  }

  static FieldTypeInfo forIndirectCase(std::string Name, int Value, const TypeRef *TR) {
    return FieldTypeInfo(Name, Value, TR, true);
  }

  static FieldTypeInfo forField(std::string Name, int Value, const TypeRef *TR) {
    return FieldTypeInfo(Name, Value, TR, false);
  }
};

/// An implementation of MetadataReader's BuilderType concept for
/// building TypeRefs, and parsing field metadata from any images
/// it has been made aware of.
///
/// Note that the TypeRefBuilder owns the memory for all TypeRefs
/// it vends.
class TypeRefBuilder {
#define TYPEREF(Id, Parent) friend class Id##TypeRef;
#include "swift/Reflection/TypeRefs.def"

public:
  using BuiltType = const TypeRef *;
  using BuiltTypeDecl = llvm::Optional<std::string>;
  using BuiltProtocolDecl =
      llvm::Optional<std::pair<std::string, bool /*isObjC*/>>;

  // TODO: Represent protocol conformances
  struct BuiltProtocolConformance {};
  struct BuiltProtocolConformanceDecl {};
  
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

  TypeConverter TC;
  MetadataSourceBuilder MSB;

#define TYPEREF(Id, Parent) \
  std::unordered_map<TypeRefID, const Id##TypeRef *, \
                     TypeRefID::Hash, TypeRefID::Equal> Id##TypeRefs;
#include "swift/Reflection/TypeRefs.def"

public:
  template <typename TypeRefTy, typename... Args>
  const TypeRefTy *makeTypeRef(Args... args) {
    const auto TR = new TypeRefTy(::std::forward<Args>(args)...);
    TypeRefPool.push_back(std::unique_ptr<const TypeRef>(TR));
    return TR;
  }

  Demangle::NodeFactory &getNodeFactory() { return Dem; }

  ///
  /// Factory methods for all TypeRef kinds
  ///

  const BuiltinTypeRef *createBuiltinType(const std::string &builtinName,
                                          const std::string &mangledName) {
    return BuiltinTypeRef::create(*this, mangledName);
  }

  llvm::Optional<std::string> createTypeDecl(Node *node, bool &typeAlias) {
    return Demangle::mangleNode(node);
  }

  BuiltProtocolDecl
  createProtocolDecl(Node *node) {
    return std::make_pair(Demangle::mangleNode(node), false);
  }

  BuiltProtocolDecl
  createObjCProtocolDecl(std::string &&name) {
    return std::make_pair(name, true);
  }

  llvm::Optional<std::string> createTypeDecl(std::string &&mangledName,
                                             bool &typeAlias) {
    return std::move(mangledName);
  }

  const NominalTypeRef *
  createNominalType(const llvm::Optional<std::string> &mangledName) {
    return NominalTypeRef::create(*this, *mangledName, nullptr);
  }

  const NominalTypeRef *
  createNominalType(const llvm::Optional<std::string> &mangledName,
                    const TypeRef *parent) {
    return NominalTypeRef::create(*this, *mangledName, parent);
  }

  const TypeRef *
  createTypeAliasType(const llvm::Optional<std::string> &mangledName,
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

  const BoundGenericTypeRef *
  createBoundGenericType(const llvm::Optional<std::string> &mangledName,
                         const std::vector<const TypeRef *> &args) {
    return BoundGenericTypeRef::create(*this, *mangledName, args, nullptr);
  }

  const BoundGenericTypeRef *
  createBoundGenericType(const llvm::Optional<std::string> &mangledName,
                         llvm::ArrayRef<const TypeRef *> args,
                         const TypeRef *parent) {
    return BoundGenericTypeRef::create(*this, *mangledName, args, parent);
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
    
    // Otherwise, build a type ref that represents the opaque type.
    return OpaqueArchetypeTypeRef::create(*this,
                                          mangleNode(opaqueDescriptor,
                                                     SymbolicResolver(),
                                                     Dem),
                                          nodeToString(opaqueDescriptor),
                                          ordinal,
                                          genericArgs);
  }

  const TupleTypeRef *createTupleType(llvm::ArrayRef<const TypeRef *> elements,
                                      std::string &&labels) {
    return TupleTypeRef::create(*this, elements, std::move(labels));
  }

  const FunctionTypeRef *createFunctionType(
      llvm::ArrayRef<remote::FunctionParam<const TypeRef *>> params,
      const TypeRef *result, FunctionTypeFlags flags) {
    return FunctionTypeRef::create(*this, params, result, flags);
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

    funcFlags = funcFlags.withConcurrent(flags.isConcurrent());
    funcFlags = funcFlags.withAsync(flags.isAsync());

    auto result = createTupleType({}, "");
    return FunctionTypeRef::create(*this, {}, result, funcFlags);
  }

  const ProtocolCompositionTypeRef *
  createProtocolCompositionType(llvm::ArrayRef<BuiltProtocolDecl> protocols,
                                BuiltType superclass, bool isClassBound) {
    std::vector<const TypeRef *> protocolRefs;
    for (const auto &protocol : protocols) {
      if (!protocol)
        continue;

      if (protocol->second)
        protocolRefs.push_back(createObjCProtocolType(protocol->first));
      else
        protocolRefs.push_back(createNominalType(protocol->first));
    }

    return ProtocolCompositionTypeRef::create(*this, protocolRefs, superclass,
                                              isClassBound);
  }

  const ExistentialMetatypeTypeRef *createExistentialMetatypeType(
      const TypeRef *instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = None) {
    return ExistentialMetatypeTypeRef::create(*this, instance);
  }

  const MetatypeTypeRef *createMetatypeType(
      const TypeRef *instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = None) {
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

  const ObjCProtocolTypeRef *
  createObjCProtocolType(const std::string &name) {
    return ObjCProtocolTypeRef::create(*this, name);
  }

  const ForeignClassTypeRef *
  createForeignClassType(const std::string &mangledName) {
    return ForeignClassTypeRef::create(*this, mangledName);
  }
  
  BuiltProtocolConformance
  createConcreteProtocolConformance(const TypeRef *conformingType,
                                    BuiltProtocolConformanceDecl conformanceDecl,
                                    llvm::ArrayRef<BuiltProtocolConformance> args) {
    return {};
  }
  
  BuiltProtocolConformance
  createDependentProtocolConformanceRoot(const TypeRef *conformingType,
                                         BuiltProtocolDecl requirement,
                                         unsigned index) {
    return {};
  }
  
  BuiltProtocolConformance
  createDependentProtocolConformanceAssociated(BuiltProtocolConformance base,
                                               const TypeRef *conformingType,
                                               BuiltProtocolDecl requirement,
                                               unsigned index) {
    return {};
  }
  
  BuiltProtocolConformance
  createDependentProtocolConformanceInherited(BuiltProtocolConformance base,
                                              BuiltProtocolDecl requirement,
                                              unsigned index) {
    return {};
  }
  
  BuiltProtocolConformanceDecl
  createProtocolConformanceDeclInTypeModule(const TypeRef *conformingType,
                                            BuiltProtocolDecl protocol) {
    return {};
  }
  
  BuiltProtocolConformanceDecl
  createProtocolConformanceDeclInProtocolModule(const TypeRef *conformingType,
                                                BuiltProtocolDecl protocol) {
    return {};
  }

  BuiltProtocolConformanceDecl
  createProtocolConformanceDeclRetroactive(const TypeRef *conformingType,
                                           BuiltProtocolDecl protocol,
                                           StringRef moduleName) {
    return {};
  }

  const ForeignClassTypeRef *
  getUnnamedForeignClassType() {
    return createForeignClassType("");
  }

  const OpaqueTypeRef *getOpaqueType() {
    return OpaqueTypeRef::get();
  }

  ///
  /// Parsing reflection metadata
  ///

  void addReflectionInfo(ReflectionInfo I) {
    ReflectionInfos.push_back(I);
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
    
  std::string normalizeReflectionName(RemoteRef<char> name);
  bool reflectionNameMatches(RemoteRef<char> reflectionName,
                             StringRef searchName);

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
  // These fields are captured from the MetadataReader template passed into the
  // TypeRefBuilder struct, to isolate its template-ness from the rest of
  // TypeRefBuilder.
  unsigned PointerSize;
  std::function<Demangle::Node * (RemoteRef<char>)>
    TypeRefDemangler;
  std::function<const TypeRef* (uint64_t, unsigned)>
    OpaqueUnderlyingTypeReader;
  
public:
  template<typename Runtime>
  TypeRefBuilder(remote::MetadataReader<Runtime, TypeRefBuilder> &reader)
    : TC(*this),
      PointerSize(sizeof(typename Runtime::StoredPointer)),
      TypeRefDemangler(
      [this, &reader](RemoteRef<char> string) -> Demangle::Node * {
        return reader.demangle(string,
                               remote::MangledNameKind::Type,
                               Dem, /*useOpaqueTypeSymbolicReferences*/ true);
      }),
      OpaqueUnderlyingTypeReader(
      [&reader](uint64_t descriptorAddr, unsigned ordinal) -> const TypeRef* {
        return reader.readUnderlyingTypeForOpaqueTypeDescriptor(
          descriptorAddr, ordinal).getType();
      })
  {}

  Demangle::Node *demangleTypeRef(RemoteRef<char> string) {
    return TypeRefDemangler(string);
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

  ///
  /// Dumping typerefs, field declarations, associated types
  ///

  void dumpTypeRef(RemoteRef<char> MangledName,
                   FILE *file, bool printTypeName = false);
  void dumpFieldSection(FILE *file);
  void dumpAssociatedTypeSection(FILE *file);
  void dumpBuiltinTypeSection(FILE *file);
  void dumpCaptureSection(FILE *file);
  void dumpAllSections(FILE *file);
};


} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_TYPEREFBUILDER_H
