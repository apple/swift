//===--- TypeRef.h - Swift Type References for Reflection -------*- C++ -*-===//
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
// Implements the structures of type references for property and enum
// case reflection.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_TYPEREF_H
#define SWIFT_REFLECTION_TYPEREF_H

#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Remote/MetadataReader.h"
#include "swift/Basic/Unreachable.h"
#include <ostream>
namespace swift {
namespace reflection {

using llvm::cast;
using llvm::dyn_cast;

enum class TypeRefKind {
#define TYPEREF(Id, Parent) Id,
#include "swift/RemoteInspection/TypeRefs.def"
#undef TYPEREF
};

#define FIND_OR_CREATE_TYPEREF(Allocator, TypeRefTy, ...)                      \
  auto ID = Profile(__VA_ARGS__);                                              \
  const auto Entry = Allocator.TypeRefTy##s.find(ID);                          \
  if (Entry != Allocator.TypeRefTy##s.end())                                   \
    return Entry->second;                                                      \
  const auto TR = Allocator.template makeTypeRef<TypeRefTy>(__VA_ARGS__);      \
  Allocator.TypeRefTy##s.insert({ID, TR});                                     \
  return TR;

/// An identifier containing the unique bit pattern made up of all of the
/// instance data needed to uniquely identify a TypeRef.
///
/// This allows for uniquing (via Equal) and for keying into a dictionary for
/// caching.
///
/// TypeRefs should be comparable by pointers, so if the TypeRefBuilder
/// gets a request to build a TypeRef with the same constructor arguments,
/// it should return the one already created with those arguments, not a fresh
/// copy. This allows for fast identity comparisons and substitutions, for
/// example. We use a similar strategy for Types in the full AST.
class TypeRefID {

  std::vector<uint32_t> Bits;

public:
  TypeRefID() = default;

  template <typename T>
  void addPointer(const T *Pointer) {
    auto Raw = reinterpret_cast<uint32_t *>(&Pointer);
    Bits.push_back(Raw[0]);
    if (sizeof(const T *) > 4) {
      Bits.push_back(Raw[1]);
    }
  }

  void addInteger(uint32_t Integer) {
    Bits.push_back(Integer);
  }

  void addInteger(uint64_t Integer) {
    Bits.push_back((uint32_t)Integer);
    Bits.push_back(Integer >> 32);
  }

  void addString(llvm::StringRef String) {
    if (String.empty()) {
      Bits.push_back(0);
    } else {
      size_t i = 0;
      size_t chunks = String.size() / 4;
      for (size_t chunk = 0; chunk < chunks; ++chunk, i+=4) {
        uint32_t entry = ((uint32_t) String[i]) +
                         (((uint32_t) String[i+1]) << 8) +
                         (((uint32_t) String[i+2]) << 16) +
                         (((uint32_t) String[i+3]) << 24);
        Bits.push_back(entry);
      }
      for (; i < String.size(); ++i) {
        Bits.push_back(String[i]);
      }
    }
  }

  struct Hash {
    std::size_t operator()(TypeRefID const &ID) const {
      size_t Hash = 0;
      std::hash<uint32_t> h;
      for (auto x : ID.Bits) {
        Hash ^= h(x) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2);
      }
      return Hash;
    }
  };

  struct Equal {
    bool operator()(const TypeRefID &lhs, const TypeRefID &rhs) const {
      return lhs.Bits == rhs.Bits;
    }
  };


  bool operator==(const TypeRefID &Other) {
    return Bits == Other.Bits;
  }
};

class TypeRef;
class TypeRefBuilder;
using DepthAndIndex = std::pair<unsigned, unsigned>;
using GenericArgumentMap = llvm::DenseMap<DepthAndIndex, const TypeRef *>;

/// FIXME: Implement me!
struct TypeRefLayoutConstraint {
  friend llvm::hash_code hash_value(const TypeRefLayoutConstraint &layout) {
    return llvm::hash_value(0);
  }
  operator bool() const { return true; }
  bool operator==(TypeRefLayoutConstraint rhs) const {
    return true;
  }
};

class TypeRefRequirement {
  llvm::PointerIntPair<const TypeRef *, 3, RequirementKind> FirstTypeAndKind;

  /// The second element of the requirement. Its content is dependent
  /// on the requirement kind.
  /// The payload of the following enum should always match the kind!
  /// Any access to the fields of this enum should first check if the
  /// requested access matches the kind of the requirement.
  union {
    const TypeRef * SecondType;
    TypeRefLayoutConstraint SecondLayout;
  };

public:
  TypeRefRequirement(RequirementKind kind, const TypeRef *first,
                     const TypeRef *second)
      : FirstTypeAndKind(first, kind), SecondType(second) {
    assert(first);
    assert(second);
    assert(kind != RequirementKind::Layout);
  }

  TypeRefRequirement(RequirementKind kind, const TypeRef *first,
                     TypeRefLayoutConstraint second)
      : FirstTypeAndKind(first, kind), SecondLayout(second) {
    assert(first);
    assert(second);
    assert(kind == RequirementKind::Layout);
  }

  /// Determine the kind of requirement.
  RequirementKind getKind() const { return FirstTypeAndKind.getInt(); }

  /// Retrieve the first type.
  const TypeRef *getFirstType() const {
    return FirstTypeAndKind.getPointer();
  }

  /// Retrieve the second type.
  const TypeRef *getSecondType() const {
    assert(getKind() != RequirementKind::Layout);
    return SecondType;
  }

  /// Retrieve the layout constraint.
  TypeRefLayoutConstraint getLayoutConstraint() const {
    assert(getKind() == RequirementKind::Layout);
    return SecondLayout;
  }
};

// On 32-bit systems this needs more than just pointer alignment to fit the
// extra bits needed by TypeRefRequirement.
class alignas(8) TypeRef {
  TypeRefKind Kind;

public:
  TypeRef(TypeRefKind Kind) : Kind(Kind) {}

  TypeRefKind getKind() const {
    return Kind;
  }

  void dump() const;
  void dump(std::ostream &stream, unsigned Indent = 0) const;

  /// Build a demangle tree from this TypeRef.
  Demangle::NodePointer getDemangling(Demangle::Demangler &Dem) const;

  bool isConcrete() const;
  bool isConcreteAfterSubstitutions(const GenericArgumentMap &Subs) const;

  const TypeRef *subst(TypeRefBuilder &Builder,
                       const GenericArgumentMap &Subs) const;

  const TypeRef *subst(TypeRefBuilder &Builder,
                       const GenericArgumentMap &Subs,
                       bool &DidSubstitute) const;

  llvm::Optional<GenericArgumentMap> getSubstMap() const;

  virtual ~TypeRef() = default;

  /// Given an original type and substituted type, decompose them in
  /// parallel to derive substitutions that produced the substituted
  /// type.
  ///
  /// This will fail if the resulting substitutions contradict already
  /// known substitutions, or if the original and substituted types
  /// have a structural mismatch.
  static bool deriveSubstitutions(GenericArgumentMap &Subs,
                                  const TypeRef *OrigTR,
                                  const TypeRef *SubstTR);
};

class BuiltinTypeRef final : public TypeRef {
  std::string MangledName;

  static TypeRefID Profile(const std::string &MangledName) {
    TypeRefID ID;
    ID.addString(MangledName);
    return ID;
  }

public:
  BuiltinTypeRef(const std::string &MangledName)
    : TypeRef(TypeRefKind::Builtin), MangledName(MangledName) {}

  template <typename Allocator>
  static const BuiltinTypeRef *create(Allocator &A, std::string MangledName) {
    FIND_OR_CREATE_TYPEREF(A, BuiltinTypeRef, MangledName);
  }

  const std::string &getMangledName() const {
    return MangledName;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Builtin;
  }
};

class NominalTypeTrait {
  std::string MangledName;
  const TypeRef *Parent;

protected:
  static TypeRefID Profile(const std::string &MangledName,
                           const TypeRef *Parent) {
    TypeRefID ID;
    ID.addPointer(Parent);
    ID.addString(MangledName);
    return ID;
  }

  NominalTypeTrait(const std::string &MangledName, const TypeRef *Parent)
    : MangledName(MangledName), Parent(Parent) {}

public:
  const std::string &getMangledName() const {
    return MangledName;
  }

  bool isStruct() const;
  bool isEnum() const;
  bool isClass() const;
  bool isProtocol() const;
  bool isAlias() const;

  bool isErrorProtocol() const {
    return MangledName == "s5ErrorP";
  }

  const TypeRef *getParent() const {
    return Parent;
  }

  unsigned getDepth() const;

};

class NominalTypeRef final : public TypeRef, public NominalTypeTrait {
  using NominalTypeTrait::Profile;
public:
  NominalTypeRef(const std::string &MangledName,
                 const TypeRef *Parent = nullptr)
    : TypeRef(TypeRefKind::Nominal), NominalTypeTrait(MangledName, Parent) {}

  template <typename Allocator>
  static const NominalTypeRef *create(Allocator &A,
                                      const std::string &MangledName,
                                      const TypeRef *Parent = nullptr) {
    FIND_OR_CREATE_TYPEREF(A, NominalTypeRef, MangledName, Parent);
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Nominal;
  }
};

class BoundGenericTypeRef final : public TypeRef, public NominalTypeTrait {
  std::vector<const TypeRef *> GenericParams;

  static TypeRefID Profile(const std::string &MangledName,
                           const std::vector<const TypeRef *> &GenericParams,
                           const TypeRef *Parent) {
    TypeRefID ID;
    ID.addPointer(Parent);
    ID.addString(MangledName);
    for (auto Param : GenericParams)
      ID.addPointer(Param);
    return ID;
  }

public:
  BoundGenericTypeRef(const std::string &MangledName,
                      std::vector<const TypeRef *> GenericParams,
                      const TypeRef *Parent = nullptr)
    : TypeRef(TypeRefKind::BoundGeneric),
      NominalTypeTrait(MangledName, Parent),
      GenericParams(GenericParams) {}

  template <typename Allocator>
  static const BoundGenericTypeRef *
  create(Allocator &A, const std::string &MangledName,
         std::vector<const TypeRef *> GenericParams,
         const TypeRef *Parent = nullptr) {
    FIND_OR_CREATE_TYPEREF(A, BoundGenericTypeRef, MangledName, GenericParams,
                           Parent);
  }

  const std::vector<const TypeRef *> &getGenericParams() const {
    return GenericParams;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::BoundGeneric;
  }
};

class TupleTypeRef final : public TypeRef {
protected:
  std::vector<const TypeRef *> Elements;
  std::vector<std::string> Labels;

  static TypeRefID Profile(const std::vector<const TypeRef *> &Elements,
                           const std::vector<std::string> &Labels) {
    TypeRefID ID;
    for (auto Element : Elements)
      ID.addPointer(Element);
    for (auto Label : Labels)
      ID.addString(Label);
    return ID;
  }

public:
  TupleTypeRef(std::vector<const TypeRef *> Elements,
               std::vector<std::string> Labels)
      : TypeRef(TypeRefKind::Tuple), Elements(std::move(Elements)),
        Labels(std::move(Labels)) {}

  template <typename Allocator>
  static const TupleTypeRef *create(Allocator &A,
                                    std::vector<const TypeRef *> Elements,
                                    const std::vector<std::string> Labels) {
    FIND_OR_CREATE_TYPEREF(A, TupleTypeRef, Elements, Labels);
  }

  const std::vector<const TypeRef *> &getElements() const { return Elements; };

  const std::vector<std::string> &getLabels() const { return Labels; }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Tuple;
  }
};

class OpaqueArchetypeTypeRef final : public TypeRef {
  std::string ID;
  std::string Description;
  unsigned Ordinal;
  // Each ArrayRef in ArgumentLists references into the buffer owned by this
  // vector, which must not be modified after construction.
  std::vector<const TypeRef *> AllArgumentsBuf;
  std::vector<llvm::ArrayRef<const TypeRef *>> ArgumentLists;

  static TypeRefID
  Profile(StringRef idString, StringRef description, unsigned ordinal,
          llvm::ArrayRef<llvm::ArrayRef<const TypeRef *>> argumentLists) {
    TypeRefID ID;
    ID.addString(idString.str());
    ID.addInteger(ordinal);
    for (auto argList : argumentLists) {
      ID.addInteger(0u);
      for (auto arg : argList)
        ID.addPointer(arg);
    }
    
    return ID;
  }

public:
  OpaqueArchetypeTypeRef(
      StringRef id, StringRef description, unsigned ordinal,
      llvm::ArrayRef<llvm::ArrayRef<const TypeRef *>> argumentLists)
      : TypeRef(TypeRefKind::OpaqueArchetype), ID(id), Description(description),
        Ordinal(ordinal) {
    std::vector<unsigned> argumentListLengths;
    
    for (auto argList : argumentLists) {
      argumentListLengths.push_back(argList.size());
      AllArgumentsBuf.insert(AllArgumentsBuf.end(),
                             argList.begin(), argList.end());
    }
    auto *data = AllArgumentsBuf.data();
    for (auto length : argumentListLengths) {
      ArgumentLists.push_back(llvm::ArrayRef<const TypeRef *>(data, length));
      data += length;
    }
    assert(data == AllArgumentsBuf.data() + AllArgumentsBuf.size());
  }

  template <typename Allocator>
  static const OpaqueArchetypeTypeRef *
  create(Allocator &A, StringRef id, StringRef description, unsigned ordinal,
         llvm::ArrayRef<llvm::ArrayRef<const TypeRef *>> arguments) {
    FIND_OR_CREATE_TYPEREF(A, OpaqueArchetypeTypeRef,
                           id, description, ordinal, arguments);
  }

  llvm::ArrayRef<llvm::ArrayRef<const TypeRef *>> getArgumentLists() const {
    return ArgumentLists;
  }

  unsigned getOrdinal() const {
    return Ordinal;
  }
  
  /// A stable identifier for the opaque type.
  StringRef getID() const {
    return ID;
  }
  
  /// A human-digestible, but not necessarily stable, description of the opaque type.
  StringRef getDescription() const {
    return Description;
  }
  
  static bool classof(const TypeRef *T) {
    return T->getKind() == TypeRefKind::OpaqueArchetype;
  }
};

class FunctionTypeRef final : public TypeRef {
  using Param = remote::FunctionParam<const TypeRef *>;

  std::vector<Param> Parameters;
  const TypeRef *Result;
  FunctionTypeFlags Flags;
  FunctionMetadataDifferentiabilityKind DifferentiabilityKind;
  const TypeRef *GlobalActor;

  static TypeRefID Profile(const std::vector<Param> &Parameters,
                           const TypeRef *Result, FunctionTypeFlags Flags,
                           FunctionMetadataDifferentiabilityKind DiffKind,
                           const TypeRef *GlobalActor) {
    TypeRefID ID;
    for (const auto &Param : Parameters) {
      ID.addString(Param.getLabel().str());
      ID.addPointer(Param.getType());
      ID.addInteger(static_cast<uint32_t>(Param.getFlags().getIntValue()));
    }
    ID.addPointer(Result);
    ID.addInteger(static_cast<uint64_t>(Flags.getIntValue()));
    ID.addInteger(static_cast<uint64_t>(DiffKind.getIntValue()));
    ID.addPointer(GlobalActor);

    return ID;
  }

public:
  FunctionTypeRef(std::vector<Param> Params, const TypeRef *Result,
                  FunctionTypeFlags Flags,
                  FunctionMetadataDifferentiabilityKind DiffKind,
                  const TypeRef *GlobalActor)
      : TypeRef(TypeRefKind::Function), Parameters(Params), Result(Result),
        Flags(Flags), DifferentiabilityKind(DiffKind),
        GlobalActor(GlobalActor) {}

  template <typename Allocator>
  static const FunctionTypeRef *create(
      Allocator &A, std::vector<Param> Params, const TypeRef *Result,
      FunctionTypeFlags Flags, FunctionMetadataDifferentiabilityKind DiffKind,
      const TypeRef *GlobalActor) {
    FIND_OR_CREATE_TYPEREF(
        A, FunctionTypeRef, Params, Result, Flags, DiffKind, GlobalActor);
  }

  const std::vector<Param> &getParameters() const { return Parameters; };

  const TypeRef *getResult() const {
    return Result;
  }

  FunctionTypeFlags getFlags() const {
    return Flags;
  }

  FunctionMetadataDifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind;
  }

  const TypeRef *getGlobalActor() const {
    return GlobalActor;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Function;
  }
};

class ProtocolCompositionTypeRef final : public TypeRef {
  std::vector<const TypeRef *> Protocols;
  const TypeRef *Superclass;
  bool HasExplicitAnyObject;

  static TypeRefID Profile(std::vector<const TypeRef *> Protocols,
                           const TypeRef *Superclass,
                           bool HasExplicitAnyObject) {
    TypeRefID ID;
    ID.addInteger((uint32_t)HasExplicitAnyObject);
    for (auto Protocol : Protocols) {
      ID.addPointer(Protocol);
    }
    ID.addPointer(Superclass);
    return ID;
  }

public:
  ProtocolCompositionTypeRef(std::vector<const TypeRef *> Protocols,
                             const TypeRef *Superclass,
                             bool HasExplicitAnyObject)
    : TypeRef(TypeRefKind::ProtocolComposition),
      Protocols(Protocols), Superclass(Superclass),
      HasExplicitAnyObject(HasExplicitAnyObject) {}

  template <typename Allocator>
  static const ProtocolCompositionTypeRef *
  create(Allocator &A, std::vector<const TypeRef *> Protocols,
         const TypeRef *Superclass, bool HasExplicitAnyObject) {
    FIND_OR_CREATE_TYPEREF(A, ProtocolCompositionTypeRef, Protocols,
                           Superclass, HasExplicitAnyObject);
  }

  // These are either NominalTypeRef or ObjCProtocolTypeRef.
  const std::vector<const TypeRef *> &getProtocols() const {
    return Protocols;
  }

  const TypeRef *getSuperclass() const { return Superclass; }

  bool hasExplicitAnyObject() const {
    return HasExplicitAnyObject;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ProtocolComposition;
  }
};

class ConstrainedExistentialTypeRef final : public TypeRef {
  const ProtocolCompositionTypeRef *Base;
  std::vector<TypeRefRequirement> Requirements;

  static TypeRefID Profile(const ProtocolCompositionTypeRef *Protocol,
                           std::vector<TypeRefRequirement> Requirements) {
    TypeRefID ID;
    ID.addPointer(Protocol);
    for (auto reqt : Requirements) {
      ID.addPointer(reqt.getFirstType());
      if (reqt.getKind() != RequirementKind::Layout)
        ID.addPointer(reqt.getSecondType());
      else
        ID.addInteger(
            unsigned(0)); // FIXME: Layout constraints aren't implemented yet
      ID.addInteger(unsigned(reqt.getKind()));
    }
    return ID;
  }

public:
  ConstrainedExistentialTypeRef(const ProtocolCompositionTypeRef *Protocol,
                                std::vector<TypeRefRequirement> Requirements)
      : TypeRef(TypeRefKind::ConstrainedExistential), Base(Protocol),
        Requirements(Requirements) {}

  template <typename Allocator>
  static const ConstrainedExistentialTypeRef *
  create(Allocator &A, const ProtocolCompositionTypeRef *Protocol,
         std::vector<TypeRefRequirement> Requirements) {
    FIND_OR_CREATE_TYPEREF(A, ConstrainedExistentialTypeRef, Protocol,
                           Requirements);
  }

  const ProtocolCompositionTypeRef *getBase() const { return Base; }

  const std::vector<TypeRefRequirement> &getRequirements() const {
    return Requirements;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ConstrainedExistential;
  }
};

class MetatypeTypeRef final : public TypeRef {
  const TypeRef *InstanceType;
  bool WasAbstract;

  static TypeRefID Profile(const TypeRef *InstanceType, bool WasAbstract) {
    TypeRefID ID;
    ID.addPointer(InstanceType);
    ID.addInteger(static_cast<uint32_t>(WasAbstract));
    return ID;
  }
public:
  MetatypeTypeRef(const TypeRef *InstanceType, bool WasAbstract)
    : TypeRef(TypeRefKind::Metatype), InstanceType(InstanceType),
      WasAbstract(WasAbstract) {}

  template <typename Allocator>
  static const MetatypeTypeRef *create(Allocator &A,
                                       const TypeRef *InstanceType,
                                       bool WasAbstract = false) {
    FIND_OR_CREATE_TYPEREF(A, MetatypeTypeRef, InstanceType, WasAbstract);
  }

  bool wasAbstract() const {
    return WasAbstract;
  }

  const TypeRef *getInstanceType() const {
    return InstanceType;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Metatype;
  }
};

class ExistentialMetatypeTypeRef final : public TypeRef {
  const TypeRef *InstanceType;

  static TypeRefID Profile(const TypeRef *InstanceType) {
    TypeRefID ID;
    ID.addPointer(InstanceType);
    return ID;
  }

public:
  ExistentialMetatypeTypeRef(const TypeRef *InstanceType)
    : TypeRef(TypeRefKind::ExistentialMetatype), InstanceType(InstanceType) {}

  template <typename Allocator>
  static const ExistentialMetatypeTypeRef *
  create(Allocator &A, const TypeRef *InstanceType) {
    FIND_OR_CREATE_TYPEREF(A, ExistentialMetatypeTypeRef, InstanceType);
  }

  const TypeRef *getInstanceType() const {
    return InstanceType;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ExistentialMetatype;
  }
};

class GenericTypeParameterTypeRef final : public TypeRef {
  const uint32_t Depth;
  const uint32_t Index;

  static TypeRefID Profile(uint32_t Depth, uint32_t Index) {
    TypeRefID ID;
    ID.addInteger(Depth);
    ID.addInteger(Index);
    return ID;
  }

public:
  GenericTypeParameterTypeRef(uint32_t Depth, uint32_t Index)
    : TypeRef(TypeRefKind::GenericTypeParameter), Depth(Depth), Index(Index) {}

  template <typename Allocator>
  static const GenericTypeParameterTypeRef *
  create(Allocator &A, uint32_t Depth, uint32_t Index) {
    FIND_OR_CREATE_TYPEREF(A, GenericTypeParameterTypeRef, Depth, Index);
  }

  uint32_t getDepth() const {
    return Depth;
  }

  uint32_t getIndex() const {
    return Index;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::GenericTypeParameter;
  }
};

class DependentMemberTypeRef final : public TypeRef {
  std::string Member;
  const TypeRef *Base;
  std::string Protocol;

  static TypeRefID Profile(const std::string &Member, const TypeRef *Base,
                           const std::string &Protocol) {
    TypeRefID ID;
    ID.addString(Member);
    ID.addPointer(Base);
    ID.addString(Protocol);
    return ID;
  }

public:

  DependentMemberTypeRef(const std::string &Member, const TypeRef *Base,
                         const std::string &Protocol)
    : TypeRef(TypeRefKind::DependentMember), Member(Member), Base(Base),
      Protocol(Protocol) {}

  template <typename Allocator>
  static const DependentMemberTypeRef *
  create(Allocator &A, const std::string &Member,
         const TypeRef *Base, const std::string &Protocol) {
    FIND_OR_CREATE_TYPEREF(A, DependentMemberTypeRef, Member, Base, Protocol);
  }

  const std::string &getMember() const {
    return Member;
  }

  const TypeRef *getBase() const {
    return Base;
  }

  const std::string &getProtocol() const {
    return Protocol;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::DependentMember;
  }
};

/// A representation of a dynamically-constructed generic signature.
///
/// \note This class is not a \c TypeRef.
class GenericSignatureRef final {
  std::vector<const GenericTypeParameterTypeRef *> Params;
  std::vector<TypeRefRequirement> Requirements;

public:
  GenericSignatureRef(
      llvm::ArrayRef<const GenericTypeParameterTypeRef *> Params,
      llvm::ArrayRef<TypeRefRequirement> Requirements)
      : Params(Params.begin(), Params.end()),
        Requirements(Requirements.begin(), Requirements.end()) {}

  template <typename Allocator>
  static const GenericSignatureRef *
  create(Allocator &A,
         llvm::ArrayRef<const GenericTypeParameterTypeRef *> Params,
         llvm::ArrayRef<TypeRefRequirement> Requirements) {
    return A.makeGenericSignatureRef(Params, Requirements);
  }

  const llvm::ArrayRef<const GenericTypeParameterTypeRef *> getParams() const {
    return Params;
  }

  const llvm::ArrayRef<TypeRefRequirement> getRequirements() const {
    return Requirements;
  }
};

class ForeignClassTypeRef final : public TypeRef {
  std::string Name;

  static TypeRefID Profile(const std::string &Name) {
    TypeRefID ID;
    ID.addString(Name);
    return ID;
  }

public:
  ForeignClassTypeRef(const std::string &Name)
    : TypeRef(TypeRefKind::ForeignClass), Name(Name) {}

  template <typename Allocator>
  static const ForeignClassTypeRef *create(Allocator &A,
                                           const std::string &Name) {
    FIND_OR_CREATE_TYPEREF(A, ForeignClassTypeRef, Name);
  }

  const std::string &getName() const {
    return Name;
  }


  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ForeignClass;
  }
};

class ObjCClassTypeRef final : public TypeRef {
  std::string Name;
  static const ObjCClassTypeRef *UnnamedSingleton;

  static TypeRefID Profile(const std::string &Name) {
    TypeRefID ID;
    ID.addString(Name);
    return ID;
  }
public:
  ObjCClassTypeRef(const std::string &Name)
    : TypeRef(TypeRefKind::ObjCClass), Name(Name) {}

  static const ObjCClassTypeRef *getUnnamed();

  template <typename Allocator>
  static const ObjCClassTypeRef *create(Allocator &A, const std::string &Name) {
    FIND_OR_CREATE_TYPEREF(A, ObjCClassTypeRef, Name);
  }

  const std::string &getName() const {
    return Name;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ObjCClass;
  }
};

class ObjCProtocolTypeRef final : public TypeRef {
  std::string Name;
  static const ObjCProtocolTypeRef *UnnamedSingleton;

  static TypeRefID Profile(const std::string &Name) {
    TypeRefID ID;
    ID.addString(Name);
    return ID;
  }
public:
  ObjCProtocolTypeRef(const std::string &Name)
    : TypeRef(TypeRefKind::ObjCProtocol), Name(Name) {}

  static const ObjCProtocolTypeRef *getUnnamed();

  template <typename Allocator>
  static const ObjCProtocolTypeRef *create(Allocator &A,
                                           const std::string &Name) {
    FIND_OR_CREATE_TYPEREF(A, ObjCProtocolTypeRef, Name);
  }

  const std::string &getName() const {
    return Name;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::ObjCProtocol;
  }
};

class OpaqueTypeRef final : public TypeRef {
  static const OpaqueTypeRef *Singleton;

  OpaqueTypeRef() : TypeRef(TypeRefKind::Opaque) {}

  static TypeRefID Profile() {
    return TypeRefID();
  }
public:
  static const OpaqueTypeRef *get();

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::Opaque;
  }
};

class ReferenceStorageTypeRef : public TypeRef {
  const TypeRef *Type;

protected:
  ReferenceStorageTypeRef(TypeRefKind Kind, const TypeRef *Type)
    : TypeRef(Kind), Type(Type) {}

  static TypeRefID Profile(const TypeRef *Type) {
    TypeRefID ID;
    ID.addPointer(Type);
    return ID;
  }
public:
  const TypeRef *getType() const {
    return Type;
  }

  static bool classof(const TypeRef *TR) {
    switch (TR->getKind()) {
#define REF_STORAGE(Name, ...) \
    case TypeRefKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
      return true;
    default:
      return false;
    }
  }
};

#define REF_STORAGE(Name, ...) \
  class Name##StorageTypeRef final : public ReferenceStorageTypeRef { \
    using ReferenceStorageTypeRef::Profile; \
  public: \
    Name##StorageTypeRef(const TypeRef *Type) \
      : ReferenceStorageTypeRef(TypeRefKind::Name##Storage, Type) {} \
    template <typename Allocator> \
    static const Name##StorageTypeRef *create(Allocator &A, \
                                              const TypeRef *Type) { \
      FIND_OR_CREATE_TYPEREF(A, Name##StorageTypeRef, Type); \
    } \
    static bool classof(const TypeRef *TR) { \
      return TR->getKind() == TypeRefKind::Name##Storage; \
    } \
  };
#include "swift/AST/ReferenceStorage.def"

class SILBoxTypeRef final : public TypeRef {
  const TypeRef *BoxedType;

  static TypeRefID Profile(const TypeRef *BoxedType) {
    TypeRefID ID;
    ID.addPointer(BoxedType);
    return ID;
  }
public:
  SILBoxTypeRef(const TypeRef *BoxedType)
    : TypeRef(TypeRefKind::SILBox), BoxedType(BoxedType) {}

  template <typename Allocator>
  static const SILBoxTypeRef *create(Allocator &A,
                                     const TypeRef *BoxedType) {
    FIND_OR_CREATE_TYPEREF(A, SILBoxTypeRef, BoxedType);
  }

  const TypeRef *getBoxedType() const {
    return BoxedType;
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::SILBox;
  }
};

class SILBoxTypeWithLayoutTypeRef final : public TypeRef {
public:
  struct Field : public llvm::PointerIntPair<const TypeRef *, 1> {
    Field(const TypeRef *Type, bool Mutable)
        : llvm::PointerIntPair<const TypeRef *, 1>(Type, (unsigned)Mutable) {}
    const TypeRef *getType() const { return getPointer(); }
    bool isMutable() const { return getInt() == 1; }
  };
  using Substitution = std::pair<const TypeRef *, const TypeRef *>;

  const std::vector<Field> &getFields() const { return Fields; }
  const std::vector<Substitution> &getSubstitutions() const {
    return Substitutions;
  }
  const std::vector<TypeRefRequirement> &getRequirements() const {
    return Requirements;
  }
protected:
  std::vector<Field> Fields;
  std::vector<Substitution> Substitutions;
  std::vector<TypeRefRequirement> Requirements;

  static TypeRefID
  Profile(const std::vector<Field> &Fields,
          const std::vector<Substitution> &Substitutions,
          const std::vector<TypeRefRequirement> &Requirements) {
    TypeRefID ID;
    for (auto &f : Fields)
      ID.addPointer(f.getOpaqueValue());
    for (auto &s : Substitutions) {
      ID.addPointer(s.first);
      ID.addPointer(s.second);
    }
    for (auto &r : Requirements) {
      ID.addInteger(uint32_t(r.getKind()));
      ID.addPointer(r.getFirstType());
      if (r.getKind() != RequirementKind::Layout)
        ID.addPointer(r.getSecondType());
      else {
        // FIXME: Implement TypeRefLayoutConstraint
        ID.addInteger(uint32_t(0));
      }
    }
    return ID;
  }

public:
  SILBoxTypeWithLayoutTypeRef(llvm::ArrayRef<Field> Fields,
                              llvm::ArrayRef<Substitution> Substitutions,
                              llvm::ArrayRef<TypeRefRequirement> Requirements)
      : TypeRef(TypeRefKind::SILBoxTypeWithLayout),
        Fields(Fields.begin(), Fields.end()),
        Substitutions(Substitutions.begin(), Substitutions.end()),
        Requirements(Requirements.begin(), Requirements.end()) {}

  template <typename Allocator>
  static const SILBoxTypeWithLayoutTypeRef *
  create(Allocator &A, llvm::ArrayRef<Field> Fields,
         llvm::ArrayRef<Substitution> Substitutions,
         llvm::ArrayRef<TypeRefRequirement> Requirements) {
    FIND_OR_CREATE_TYPEREF(A, SILBoxTypeWithLayoutTypeRef, Fields,
                           Substitutions, Requirements);
  }

  static bool classof(const TypeRef *TR) {
    return TR->getKind() == TypeRefKind::SILBoxTypeWithLayout;
  }
};

template <typename ImplClass, typename RetTy = void, typename... Args>
class TypeRefVisitor {
public:

  RetTy visit(const TypeRef *typeRef, Args... args) {
    // We shouldn't get a NULL typeRef, but handle it gracefully if we do.
    if (!typeRef)
      return RetTy();

    switch (typeRef->getKind()) {
#define TYPEREF(Id, Parent) \
    case TypeRefKind::Id: \
      return static_cast<ImplClass*>(this) \
        ->visit##Id##TypeRef(cast<Id##TypeRef>(typeRef), \
                           ::std::forward<Args>(args)...);
#include "swift/RemoteInspection/TypeRefs.def"
    }

    // We shouldn't get an unknown kind, but bad data might result in an unknown
    // value showing up here. Handle it gracefully when that happens.
    return RetTy();
  }
};

} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_TYPEREF_H
