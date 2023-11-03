//===--- TypeLowering.cpp - Swift Type Lowering for Reflection ------------===//
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
// Implements logic for computing in-memory layouts from TypeRefs loaded from
// reflection metadata.
//
// This has to match up with layout algorithms used in IRGen and the runtime,
// and a bit of SIL type lowering to boot.
//
//===----------------------------------------------------------------------===//

#if SWIFT_ENABLE_REFLECTION

#include "llvm/Support/MathExtras.h"
#include "swift/ABI/Enum.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/RemoteInspection/TypeInfoBuilders.h"
#include "swift/Basic/Unreachable.h"

#ifdef DEBUG_TYPE_LOWERING
  #define DEBUG_LOG(expr) expr;
#else
  #define DEBUG_LOG(expr)
#endif

namespace swift {
namespace reflection {

void TypeInfo::dump() const {
  dump(std::cerr);
}

namespace {

class PrintTypeInfo {
  std::ostream &stream;
  unsigned Indent;

  std::ostream &indent(unsigned Amount) {
    for (unsigned i = 0; i < Amount; ++i)
      stream << " ";
    return stream;
  }

  std::ostream &printHeader(const std::string &name) {
    indent(Indent) << "(" << name;
    return stream;
  }

  std::ostream &printField(const std::string &name, const std::string &value) {
    if (!name.empty())
      stream << " " << name << "=" << value;
    else
      stream << " " << name;
    return stream;
  }

  void printRec(const TypeInfo &TI) {
    stream << "\n";

    Indent += 2;
    print(TI);
    Indent -= 2;
  }

  void printBasic(const TypeInfo &TI) {
    printField("size", std::to_string(TI.getSize()));
    printField("alignment", std::to_string(TI.getAlignment()));
    printField("stride", std::to_string(TI.getStride()));
    printField("num_extra_inhabitants", std::to_string(TI.getNumExtraInhabitants()));
    printField("bitwise_takable", TI.isBitwiseTakable() ? "1" : "0");
  }

  void printFields(const RecordTypeInfo &TI) {
    Indent += 2;
    for (auto Field : TI.getFields()) {
      stream << "\n";
      printHeader("field");
      if (!Field.Name.empty())
        printField("name", Field.Name);
      printField("offset", std::to_string(Field.Offset));
      printRec(Field.TI);
      stream << ")";
    }
    Indent -= 2;
  }

  void printCases(const EnumTypeInfo &TI) {
    Indent += 2;
    int Index = -1;
    for (auto Case : TI.getCases()) {
      Index += 1;
      stream << "\n";
      printHeader("case");
      if (!Case.Name.empty())
        printField("name", Case.Name);
      printField("index", std::to_string(Index));
      if (Case.TR) {
        printField("offset", std::to_string(Case.Offset));
        printRec(Case.TI);
      }
      stream << ")";
    }
    Indent -= 2;
  }

public:
  PrintTypeInfo(std::ostream &stream, unsigned Indent)
      : stream(stream), Indent(Indent) {}

  void print(const TypeInfo &TI) {
    switch (TI.getKind()) {
    case TypeInfoKind::Invalid:
      printHeader("invalid");
      stream << ")";
      return;

    case TypeInfoKind::Builtin:
      printHeader("builtin");
      printBasic(TI);
      stream << ")";
      return;

    case TypeInfoKind::Record: {
      auto &RecordTI = cast<RecordTypeInfo>(TI);
      switch (RecordTI.getRecordKind()) {
      case RecordKind::Invalid:
        printHeader("invalid");
        break;
      case RecordKind::Struct:
        printHeader("struct");
        break;
      case RecordKind::Tuple:
        printHeader("tuple");
        break;
      case RecordKind::ThickFunction:
        printHeader("thick_function");
        break;
      case RecordKind::OpaqueExistential:
        printHeader("opaque_existential");
        break;
      case RecordKind::ClassExistential:
        printHeader("class_existential");
        break;
      case RecordKind::ErrorExistential:
        printHeader("error_existential");
        break;
      case RecordKind::ExistentialMetatype:
        printHeader("existential_metatype");
        break;
      case RecordKind::ClassInstance:
        printHeader("class_instance");
        break;
      case RecordKind::ClosureContext:
        printHeader("closure_context");
        break;
      }
      printBasic(TI);
      printFields(RecordTI);
      stream << ")";
      return;
    }

    case TypeInfoKind::Enum: {
      auto &EnumTI = cast<EnumTypeInfo>(TI);
      switch (EnumTI.getEnumKind()) {
      case EnumKind::NoPayloadEnum:
        printHeader("no_payload_enum");
        break;
      case EnumKind::SinglePayloadEnum:
        printHeader("single_payload_enum");
        break;
      case EnumKind::MultiPayloadEnum:
        printHeader("multi_payload_enum");
        break;
      }
      printBasic(TI);
      printCases(EnumTI);
      stream << ")";
      return;
    }

    case TypeInfoKind::Reference: {
      printHeader("reference");
      auto &ReferenceTI = cast<ReferenceTypeInfo>(TI);
      switch (ReferenceTI.getReferenceKind()) {
      case ReferenceKind::Strong: printField("kind", "strong"); break;
#define REF_STORAGE(Name, name, ...) \
      case ReferenceKind::Name: printField("kind", #name); break;
#include "swift/AST/ReferenceStorage.def"
      }

      switch (ReferenceTI.getReferenceCounting()) {
      case ReferenceCounting::Native:
        printField("refcounting", "native");
        break;
      case ReferenceCounting::Unknown:
        printField("refcounting", "unknown");
        break;
      }

      stream << ")";
      return;
    }
    }

    swift_unreachable("Bad TypeInfo kind");
  }
};

} // end anonymous namespace

void TypeInfo::dump(std::ostream &stream, unsigned Indent) const {
  PrintTypeInfo(stream, Indent).print(*this);
  stream << "\n";
}

BuiltinTypeInfo::BuiltinTypeInfo(TypeRefBuilder &builder,
                                 RemoteRef<BuiltinTypeDescriptor> descriptor)
    : TypeInfo(TypeInfoKind::Builtin,
               descriptor->Size,
               descriptor->getAlignment(),
               descriptor->Stride,
               descriptor->NumExtraInhabitants,
               descriptor->isBitwiseTakable()),
      Name(builder.getTypeRefString(
              builder.readTypeRef(descriptor, descriptor->TypeName)))
{}

bool BuiltinTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *extraInhabitantIndex) const {
  if (getNumExtraInhabitants() == 0) {
    *extraInhabitantIndex = -1;
    return true;
  }
  // If it has extra inhabitants, it could be an integer type with extra
  // inhabitants (a bool) or a pointer.
  // Check if it's an integer first. The mangling of an integer type is
  // type ::= 'Bi' NATURAL '_'
  llvm::StringRef nameRef(Name);
  if (nameRef.startswith("Bi") && nameRef.endswith("_")) {
    // Drop the front "Bi" and "_" end, check that what we're left with is a
    // bool.
    llvm::StringRef naturalRef = nameRef.drop_front(2).drop_back();
    uint8_t natural;
    if (naturalRef.getAsInteger(10, natural))
      return false;

    assert(natural == 1 &&
           "Reading extra inhabitants of integer with more than 1 byte!");
    if (natural != 1)
      return false;

    assert(getSize() == 1 && "Reading extra inhabitants of integer but size of "
                             "type info is different than 1!");
    if (getSize() != 1)
      return false;

    assert(getNumExtraInhabitants() == 254 &&
           "Boolean type info should have 254 extra inhabitants!");
    if (getNumExtraInhabitants() != 254)
      return false;

    uint8_t rawValue;
    if (!reader.readInteger(address, &rawValue))
      return false;

    // The max valid value, for a bool valid values are 0 or 1, so this would
    // be 1.
    auto maxValidValue = 1;
    // If the raw value falls outside the range of valid values, this is an
    // extra inhabitant.
    if (maxValidValue < rawValue)
      *extraInhabitantIndex = rawValue - maxValidValue - 1;
    else
      *extraInhabitantIndex = -1;
    return true;
  } else if (Name == "yyXf") {
    // But there are two different conventions, one for function pointers:
    return reader.readFunctionPointerExtraInhabitantIndex(address,
                                                          extraInhabitantIndex);
  } else {
    // And one for pointers to heap-allocated blocks of memory
    return reader.readHeapObjectExtraInhabitantIndex(address,
                                                     extraInhabitantIndex);
  }
}

bool RecordTypeInfo::readExtraInhabitantIndex(remote::MemoryReader &reader,
                                              remote::RemoteAddress address,
                                              int *extraInhabitantIndex) const {
  *extraInhabitantIndex = -1;

  switch (SubKind) {
  case RecordKind::Invalid:
  case RecordKind::ClosureContext:
    return false;

  case RecordKind::OpaqueExistential:
  case RecordKind::ExistentialMetatype: {
    if (Fields.size() < 1) {
      return false;
    }
    auto metadata = Fields[0];
    auto metadataFieldAddress = address + metadata.Offset;
    return metadata.TI.readExtraInhabitantIndex(
      reader, metadataFieldAddress, extraInhabitantIndex);
  }

  case RecordKind::ThickFunction: {
    if (Fields.size() < 2) {
      return false;
    }
    auto function = Fields[0];
    auto context = Fields[1];
    if (function.Offset != 0) {
      return false;
    }
    auto functionFieldAddress = address;
    return function.TI.readExtraInhabitantIndex(
      reader, functionFieldAddress, extraInhabitantIndex);
  }

  case RecordKind::ClassExistential:
  case RecordKind::ErrorExistential: {
    if (Fields.size() < 1) {
      return true;
    }
    auto first = Fields[0];
    auto firstFieldAddress = address + first.Offset;
    return first.TI.readExtraInhabitantIndex(reader, firstFieldAddress,
                                             extraInhabitantIndex);
  }

  case RecordKind::ClassInstance:
    // This case seems unlikely to ever happen; if we're using XIs with a
    // class, it'll be with a reference, not with the instance itself (i.e.
    // we'll be in the RecordKind::ClassExistential case).
    return false;

  case RecordKind::Tuple:
  case RecordKind::Struct: {
    if (Fields.size() == 0) {
      return true;
    }
    // Tuples and Structs inherit XIs from their most capacious member
    auto mostCapaciousField = std::max_element(
      Fields.begin(), Fields.end(),
      [](const FieldInfo &lhs, const FieldInfo &rhs) {
        return lhs.TI.getNumExtraInhabitants() < rhs.TI.getNumExtraInhabitants();
      });
    auto fieldAddress = remote::RemoteAddress(address.getAddressData()
                                              + mostCapaciousField->Offset);
    return mostCapaciousField->TI.readExtraInhabitantIndex(
      reader, fieldAddress, extraInhabitantIndex);
  }
  }
  return false;
}

const ReferenceTypeInfo *
TypeConverter::getReferenceTypeInfo(ReferenceKind Kind,
                                    ReferenceCounting Refcounting) {
  auto key = std::make_pair(unsigned(Kind), unsigned(Refcounting));
  auto found = ReferenceCache.find(key);
  if (found != ReferenceCache.end())
    return found->second;

  const TypeRef *TR;
  switch (Refcounting) {
  case ReferenceCounting::Native:
    TR = getNativeObjectTypeRef();
    break;
  case ReferenceCounting::Unknown:
    TR = getUnknownObjectTypeRef();
    break;
  }

  // Unowned and unmanaged references have the same extra inhabitants
  // as the underlying type.
  //
  // Weak references do not have any extra inhabitants.

  auto BuiltinTI = Builder.getBuiltinTypeInfo(TR);
  if (BuiltinTI == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for reference type: "); TR->dump());
    return nullptr;
  }

  unsigned numExtraInhabitants = BuiltinTI->NumExtraInhabitants;
  bool bitwiseTakable = true;

  switch (Kind) {
  case ReferenceKind::Strong:
    break;
  case ReferenceKind::Weak:
    numExtraInhabitants = 0;
    bitwiseTakable = false;
    break;
  case ReferenceKind::Unowned:
    if (Refcounting == ReferenceCounting::Unknown)
      bitwiseTakable = false;
    break;
  case ReferenceKind::Unmanaged:
    break;
  }

  auto *TI = makeTypeInfo<ReferenceTypeInfo>(BuiltinTI->Size,
                                             BuiltinTI->getAlignment(),
                                             BuiltinTI->Stride,
                                             numExtraInhabitants,
                                             bitwiseTakable,
                                             Kind, Refcounting);
  ReferenceCache[key] = TI;
  return TI;
}

/// Thin functions consist of a function pointer. We do not use
/// Builtin.RawPointer here, since the extra inhabitants differ.
const TypeInfo *
TypeConverter::getThinFunctionTypeInfo() {
  if (ThinFunctionTI != nullptr)
    return ThinFunctionTI;

  auto descriptor = getBuilder().getBuiltinTypeInfo(getThinFunctionTypeRef());
  if (descriptor == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for function type\n"));
    return nullptr;
  }

  ThinFunctionTI = makeTypeInfo<BuiltinTypeInfo>(getBuilder(), descriptor);

  return ThinFunctionTI;
}

/// Thick functions consist of a function pointer and nullable retainable
/// context pointer. The context is modeled exactly like a native Swift
/// class reference.
const TypeInfo *TypeConverter::getThickFunctionTypeInfo() {
  if (ThickFunctionTI != nullptr)
    return ThickFunctionTI;

  RecordTypeInfoBuilder builder(*this, RecordKind::ThickFunction);
  builder.addField("function", getThinFunctionTypeRef(), nullptr);
  builder.addField("context", getNativeObjectTypeRef(), nullptr);
  ThickFunctionTI = builder.build();

  return ThickFunctionTI;
}

/// Thick metatypes consist of a single pointer, possibly followed
/// by witness tables. We do not use Builtin.RawPointer here, since
/// the extra inhabitants differ.
const TypeInfo *
TypeConverter::getAnyMetatypeTypeInfo() {
  if (AnyMetatypeTI != nullptr)
    return AnyMetatypeTI;

  auto descriptor = getBuilder().getBuiltinTypeInfo(getAnyMetatypeTypeRef());
  if (descriptor == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for metatype type\n"));
    return nullptr;
  }

  AnyMetatypeTI = makeTypeInfo<BuiltinTypeInfo>(getBuilder(), descriptor);

  return AnyMetatypeTI;
}

const TypeInfo *TypeConverter::getEmptyTypeInfo() {
  if (EmptyTI != nullptr)
    return EmptyTI;

  EmptyTI = makeTypeInfo<BuiltinTypeInfo>();
  return EmptyTI;
}

const TypeRef *TypeConverter::getRawPointerTypeRef() {
  if (RawPointerTR != nullptr)
    return RawPointerTR;

  RawPointerTR = BuiltinTypeRef::create(Builder, "Bp");
  return RawPointerTR;
}

const TypeRef *TypeConverter::getNativeObjectTypeRef() {
  if (NativeObjectTR != nullptr)
    return NativeObjectTR;

  NativeObjectTR = BuiltinTypeRef::create(Builder, "Bo");
  return NativeObjectTR;
}

const TypeRef *TypeConverter::getUnknownObjectTypeRef() {
  if (UnknownObjectTR != nullptr)
    return UnknownObjectTR;

  UnknownObjectTR = BuiltinTypeRef::create(Builder, "BO");
  return UnknownObjectTR;
}

const TypeRef *TypeConverter::getThinFunctionTypeRef() {
  if (ThinFunctionTR != nullptr)
    return ThinFunctionTR;

  ThinFunctionTR = BuiltinTypeRef::create(Builder, "yyXf");
  return ThinFunctionTR;
}

const TypeRef *TypeConverter::getAnyMetatypeTypeRef() {
  if (AnyMetatypeTR != nullptr)
    return AnyMetatypeTR;

  AnyMetatypeTR = BuiltinTypeRef::create(Builder, "ypXp");
  return AnyMetatypeTR;
}

enum class MetatypeRepresentation : unsigned {
  /// Singleton metatype values are empty.
  Thin,

  /// Metatypes containing classes, or where the original unsubstituted
  /// type contains a type parameter, must be represented as pointers
  /// to metadata structures.
  Thick,

  /// Insufficient information to determine which.
  Unknown
};

/// Visitor class to determine if a type has a fixed size.
///
/// Conservative approximation.
class HasFixedSize
  : public TypeRefVisitor<HasFixedSize, bool> {

public:
  HasFixedSize() {}

  using TypeRefVisitor<HasFixedSize, bool>::visit;

  bool visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return true;
  }

  bool visitNominalTypeRef(const NominalTypeRef *N) {
    return true;
  }

  bool visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->isClass())
      return true;
    for (auto Arg : BG->getGenericParams()) {
      if (!visit(Arg))
        return false;
    }
    return true;
  }

  bool visitTupleTypeRef(const TupleTypeRef *T) {
    for (auto Element : T->getElements())
      if (!visit(Element))
        return false;
    return true;
  }

  bool visitFunctionTypeRef(const FunctionTypeRef *F) {
    return true;
  }

  bool
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return true;
  }

  bool visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    return true;
  }

  bool
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return true;
  }

  bool
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return true;
  }

  bool
  visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return true;
  }

  bool visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return true;
  }

  bool
  visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return true;
  }

  bool visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return true;
  }

  bool visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return true;
  }

#define REF_STORAGE(Name, ...) \
  bool \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return true; \
  }
#include "swift/AST/ReferenceStorage.def"

  bool
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    return false;
  }

  bool
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    return false;
  }

  bool visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return false;
  }

  bool visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return false;
  }
};

bool TypeConverter::hasFixedSize(const TypeRef *TR) {
  return HasFixedSize().visit(TR);
}

MetatypeRepresentation combineRepresentations(MetatypeRepresentation rep1,
                                              MetatypeRepresentation rep2) {
  if (rep1 == rep2)
    return rep1;

  if (rep1 == MetatypeRepresentation::Unknown ||
      rep2 == MetatypeRepresentation::Unknown)
    return MetatypeRepresentation::Unknown;

  if (rep1 == MetatypeRepresentation::Thick ||
      rep2 == MetatypeRepresentation::Thick)
    return MetatypeRepresentation::Thick;

  return MetatypeRepresentation::Thin;
}

/// Visitor class to determine if a metatype should use the empty
/// representation.
///
/// This relies on substitution correctly setting wasAbstract() on
/// MetatypeTypeRefs.
class HasSingletonMetatype
  : public TypeRefVisitor<HasSingletonMetatype, MetatypeRepresentation> {

public:
  HasSingletonMetatype() {}

  using TypeRefVisitor<HasSingletonMetatype, MetatypeRepresentation>::visit;

  MetatypeRepresentation visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitNominalTypeRef(const NominalTypeRef *N) {
    if (N->isClass())
      return MetatypeRepresentation::Thick;
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->isClass())
      return MetatypeRepresentation::Thick;
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitTupleTypeRef(const TupleTypeRef *T) {
    auto result = MetatypeRepresentation::Thin;
    for (auto Element : T->getElements())
      result = combineRepresentations(result, visit(Element));
    return result;
  }

  MetatypeRepresentation visitFunctionTypeRef(const FunctionTypeRef *F) {
    auto result = visit(F->getResult());
    for (const auto &Param : F->getParameters())
      result = combineRepresentations(result, visit(Param.getType()));
    return result;
  }

  MetatypeRepresentation
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    if (M->wasAbstract())
      return MetatypeRepresentation::Thick;
    return visit(M->getInstanceType());
  }

  MetatypeRepresentation
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); GTP->dump());
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); DM->dump());
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation
  visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return MetatypeRepresentation::Unknown;
  }

#define REF_STORAGE(Name, ...) \
  MetatypeRepresentation \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return MetatypeRepresentation::Unknown; \
  }
#include "swift/AST/ReferenceStorage.def"

  MetatypeRepresentation visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return MetatypeRepresentation::Unknown;
  }
};

class LowerType
  : public TypeRefVisitor<LowerType, const TypeInfo *> {
  TypeConverter &TC;
  remote::TypeInfoProvider *ExternalTypeInfo;

public:
  using TypeRefVisitor<LowerType, const TypeInfo *>::visit;

  LowerType(TypeConverter &TC, remote::TypeInfoProvider *ExternalTypeInfo)
      : TC(TC), ExternalTypeInfo(ExternalTypeInfo) {}

  const TypeInfo *visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    /// The context field of a thick function is a Builtin.NativeObject.
    /// Since we want this to round-trip, lower these as reference
    /// types.
    if (B->getMangledName() == "Bo") {
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Native);
    } else if (B->getMangledName() == "BO") {
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    }

    /// Otherwise, get the fixed layout information from reflection
    /// metadata.
    auto descriptor = TC.getBuilder().getBuiltinTypeInfo(B);
    if (descriptor == nullptr) {
      DEBUG_LOG(fprintf(stderr, "No TypeInfo for builtin type: "); B->dump());
      return nullptr;
    }
    return TC.makeTypeInfo<BuiltinTypeInfo>(TC.getBuilder(), descriptor);
  }

  const TypeInfo *visitAnyNominalTypeRef(const TypeRef *TR) {
    auto QueryExternalTypeInfoProvider = [&]() -> const TypeInfo * {
      if (ExternalTypeInfo) {
        std::string MangledName;
        if (auto N = dyn_cast<NominalTypeRef>(TR))
          MangledName = N->getMangledName();
        else if (auto BG = dyn_cast<BoundGenericTypeRef>(TR))
          MangledName = BG->getMangledName();
        if (!MangledName.empty())
          if (auto *imported = ExternalTypeInfo->getTypeInfo(MangledName))
            return imported;
      }
      return nullptr;
    };

    auto FD = TC.getBuilder().getFieldTypeInfo(TR);
    if (FD == nullptr || FD->isStruct()) {
      // Maybe this type is opaque -- look for a builtin
      // descriptor to see if we at least know its size
      // and alignment.
      if (auto ImportedTypeDescriptor =
              TC.getBuilder().getBuiltinTypeInfo(TR)) {
        // This might be an external type we treat as opaque (like C structs),
        // the external type info provider might have better type information,
        // so ask it first.
        if (auto External = QueryExternalTypeInfoProvider())
          return External;

        return TC.makeTypeInfo<BuiltinTypeInfo>(TC.getBuilder(),
                                                ImportedTypeDescriptor);
      }

      if (FD == nullptr) {
        // If we still have no type info ask the external provider.
        if (auto External = QueryExternalTypeInfoProvider())
          return External;

        // If the external provider also fails we're out of luck.
        DEBUG_LOG(fprintf(stderr, "No TypeInfo for nominal type: "); TR->dump());
        return nullptr;
      }
    }

    switch (FD->Kind) {
    case FieldDescriptorKind::Class:
      // A value of class type is a single retainable pointer.
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Native);
    case FieldDescriptorKind::Struct: {
      // Lower the struct's fields using substitutions from the
      // TypeRef to make field types concrete.
      RecordTypeInfoBuilder builder(TC, RecordKind::Struct);

      std::vector<FieldTypeInfo> Fields;
      if (!TC.getBuilder().getFieldTypeRefs(TR, FD, ExternalTypeInfo, Fields))
        return nullptr;

      for (auto Field : Fields)
        builder.addField(Field.Name, Field.TR, ExternalTypeInfo);
      return builder.build();
    }
    case FieldDescriptorKind::Enum:
    case FieldDescriptorKind::MultiPayloadEnum: {
      EnumTypeInfoBuilder builder(TC);
      return builder.build(TR, FD, ExternalTypeInfo);
    }
    case FieldDescriptorKind::ObjCClass:
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    case FieldDescriptorKind::ObjCProtocol:
    case FieldDescriptorKind::ClassProtocol:
    case FieldDescriptorKind::Protocol:
      DEBUG_LOG(fprintf(stderr, "Invalid field descriptor: "); TR->dump());
      return nullptr;
    }

    swift_unreachable("Unhandled FieldDescriptorKind in switch.");
  }

  const TypeInfo *visitNominalTypeRef(const NominalTypeRef *N) {
    return visitAnyNominalTypeRef(N);
  }

  const TypeInfo *visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    return visitAnyNominalTypeRef(BG);
  }

  const TypeInfo *visitTupleTypeRef(const TupleTypeRef *T) {
    RecordTypeInfoBuilder builder(TC, RecordKind::Tuple);
    for (auto Element : T->getElements())
      // The label is not going to be relevant/harmful for looking up type info.
      builder.addField("", Element, ExternalTypeInfo);
    return builder.build();
  }

  const TypeInfo *visitFunctionTypeRef(const FunctionTypeRef *F) {
    switch (F->getFlags().getConvention()) {
    case FunctionMetadataConvention::Swift:
      return TC.getThickFunctionTypeInfo();
    case FunctionMetadataConvention::Block:
      // FIXME: Native convention if blocks are ever supported on Linux?
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    case FunctionMetadataConvention::Thin:
    case FunctionMetadataConvention::CFunctionPointer:
      return TC.getTypeInfo(TC.getThinFunctionTypeRef(), ExternalTypeInfo);
    }

    swift_unreachable("Unhandled FunctionMetadataConvention in switch.");
  }

  const TypeInfo *
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    ExistentialTypeInfoBuilder builder(TC);
    builder.addProtocolComposition(PC);
    return builder.build(ExternalTypeInfo);
  }

  const TypeInfo *
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return visitProtocolCompositionTypeRef(CET->getBase());
  }

  const TypeInfo *visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    switch (HasSingletonMetatype().visit(M)) {
    case MetatypeRepresentation::Unknown:
      DEBUG_LOG(fprintf(stderr, "Unknown metatype representation: "); M->dump());
      return nullptr;
    case MetatypeRepresentation::Thin:
      return TC.getEmptyTypeInfo();
    case MetatypeRepresentation::Thick:
      return TC.getTypeInfo(TC.getAnyMetatypeTypeRef(), ExternalTypeInfo);
    }

    swift_unreachable("Unhandled MetatypeRepresentation in switch.");
  }

  const TypeInfo *
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    ExistentialTypeInfoBuilder builder(TC);
    auto *TR = EM->getInstanceType();

    if (auto *PC = dyn_cast<ProtocolCompositionTypeRef>(TR)) {
      builder.addProtocolComposition(PC);
    } else {
      DEBUG_LOG(fprintf(stderr, "Invalid existential metatype: "); EM->dump());
      return nullptr;
    }

    return builder.buildMetatype(ExternalTypeInfo);
  }

  const TypeInfo *
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); GTP->dump());
    return nullptr;
  }

  const TypeInfo *
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); DM->dump());
    return nullptr;
  }

  const TypeInfo *visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  const TypeInfo *visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  const TypeInfo *visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  // Apply a storage qualifier, like 'weak', 'unowned' or 'unowned(unsafe)'
  // to a type with reference semantics, such as a class reference or
  // class-bound existential.
  const TypeInfo *
  rebuildStorageTypeInfo(const TypeInfo *TI, ReferenceKind Kind) {
    // If we can't lower the original storage type, give up.
    if (TI == nullptr) {
      DEBUG_LOG(fprintf(stderr, "Invalid reference type"));
      return nullptr;
    }

    // Simple case: Just change the reference kind
    if (auto *ReferenceTI = dyn_cast<ReferenceTypeInfo>(TI))
      return TC.getReferenceTypeInfo(Kind, ReferenceTI->getReferenceCounting());

    if (auto *EnumTI = dyn_cast<EnumTypeInfo>(TI)) {
      if (EnumTI->isOptional() &&
          (Kind == ReferenceKind::Weak || Kind == ReferenceKind::Unowned ||
           Kind == ReferenceKind::Unmanaged)) {
        auto *TI = TC.getTypeInfo(EnumTI->getCases()[0].TR, ExternalTypeInfo);
        return rebuildStorageTypeInfo(TI, Kind);
      }
    }

    if (auto *RecordTI = dyn_cast<RecordTypeInfo>(TI)) {
      auto SubKind = RecordTI->getRecordKind();
      // Class existentials are represented as record types.
      // Destructure the existential and replace the "object"
      // field with the right reference kind.
      if (SubKind == RecordKind::ClassExistential) {
        bool BitwiseTakable = RecordTI->isBitwiseTakable();
        std::vector<FieldInfo> Fields;
        for (auto &Field : RecordTI->getFields()) {
          if (Field.Name == "object") {
            auto *FieldTI = rebuildStorageTypeInfo(&Field.TI, Kind);
            BitwiseTakable &= FieldTI->isBitwiseTakable();
            Fields.push_back({Field.Name, Field.Offset, /*value=*/-1, Field.TR, *FieldTI});
            continue;
          }
          Fields.push_back(Field);
        }

        return TC.makeTypeInfo<RecordTypeInfo>(
            RecordTI->getSize(),
            RecordTI->getAlignment(),
            RecordTI->getStride(),
            RecordTI->getNumExtraInhabitants(),
            BitwiseTakable,
            SubKind, Fields);
      }
    }

    // Anything else -- give up
    DEBUG_LOG(fprintf(stderr, "Invalid reference type"));
    return nullptr;
  }

  const TypeInfo *
  visitAnyStorageTypeRef(const TypeRef *TR, ReferenceKind Kind) {
    return rebuildStorageTypeInfo(TC.getTypeInfo(TR, ExternalTypeInfo), Kind);
  }

#define REF_STORAGE(Name, name, ...) \
  const TypeInfo * \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return visitAnyStorageTypeRef(US->getType(), ReferenceKind::Name); \
  }
#include "swift/AST/ReferenceStorage.def"

  const TypeInfo *visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Native);
  }

  const TypeInfo *
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Native);
  }

  const TypeInfo *visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    DEBUG_LOG(fprintf(stderr, "Can't lower opaque TypeRef"));
    return nullptr;
  }

  const TypeInfo *visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    // TODO: Provide a hook for the client to try to resolve the opaque archetype
    // with additional information?
    DEBUG_LOG(fprintf(stderr, "Can't lower unresolved opaque archetype TypeRef"));
    return nullptr;
  }
};

const TypeInfo *
TypeConverter::getTypeInfo(const TypeRef *TR,
                           remote::TypeInfoProvider *ExternalTypeInfo) {
  if (!TR) {
    DEBUG_LOG(fprintf(stderr, "null TypeRef"));
    return nullptr;
  }

  auto ExternalTypeInfoId =
      ExternalTypeInfo ? ExternalTypeInfo->getId() : 0;
  // See if we already computed the result
  auto found = Cache.find({TR, ExternalTypeInfoId});
  if (found != Cache.end())
    return found->second;

  // Detect invalid recursive value types (IRGen should not emit
  // them in the first place, but there might be bugs)
  if (!RecursionCheck.insert(TR).second) {
    DEBUG_LOG(fprintf(stderr, "TypeRef recursion detected"));
    return nullptr;
  }

  // Compute the result and cache it
  auto *TI = LowerType(*this, ExternalTypeInfo).visit(TR);
  Cache.insert({{TR, ExternalTypeInfoId}, TI});

  RecursionCheck.erase(TR);

  return TI;
}

const RecordTypeInfo *TypeConverter::getClassInstanceTypeInfo(
    const TypeRef *TR, unsigned start,
    remote::TypeInfoProvider *ExternalTypeInfo) {
  auto FD = getBuilder().getFieldTypeInfo(TR);
  if (FD == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No field descriptor: "); TR->dump());
    return nullptr;
  }

  switch (FD->Kind) {
  case FieldDescriptorKind::Class:
  case FieldDescriptorKind::ObjCClass: {
    // Lower the class's fields using substitutions from the
    // TypeRef to make field types concrete.
    RecordTypeInfoBuilder builder(*this, RecordKind::ClassInstance);

    std::vector<FieldTypeInfo> Fields;
    if (!getBuilder().getFieldTypeRefs(TR, FD, ExternalTypeInfo, Fields))
      return nullptr;

    // Start layout from the given instance start offset. This should
    // be the superclass instance size.
    builder.addField(/*size=*/start,
                     /*alignment=*/1,
                     /*numExtraInhabitants=*/0,
                     /*bitwiseTakable=*/true);

    for (auto Field : Fields)
      builder.addField(Field.Name, Field.TR, ExternalTypeInfo);
    return builder.build();
  }
  case FieldDescriptorKind::Struct:
  case FieldDescriptorKind::Enum:
  case FieldDescriptorKind::MultiPayloadEnum:
  case FieldDescriptorKind::ObjCProtocol:
  case FieldDescriptorKind::ClassProtocol:
  case FieldDescriptorKind::Protocol:
    // Invalid field descriptor.
    DEBUG_LOG(fprintf(stderr, "Invalid field descriptor: "); TR->dump());
    return nullptr;
  }

  swift_unreachable("Unhandled FieldDescriptorKind in switch.");
}

unsigned EnumTypeInfo::getNumNonEmptyPayloadCases() const {
  auto Cases = getCases();
  return std::count_if(Cases.begin(), Cases.end(), [](const FieldInfo &Case) {
    // For our purposes here, assume any case
    // with invalid (missing) typeinfo is non-empty
    return Case.TR != 0 && (Case.TI.getKind() == TypeInfoKind::Invalid ||
                            Case.TI.getSize() > 0);
  });
}

unsigned
EnumTypeInfo::getPayloadSizeForCases(const std::vector<FieldInfo> &Cases) {
  unsigned size = 0;
  for (auto Case : Cases) {
    if (Case.TR != 0 && Case.TI.getSize() > size) {
      size = Case.TI.getSize();
    }
  }
  return size;
}

// (This was factored out of a piece of code that was just
// checking the EnumKind.  This is vastly better than that,
// but could probably be improved further.)
bool EnumTypeInfo::isOptional() const {
  return SubKind == EnumKind::SinglePayloadEnum && Cases.size() == 2 &&
         Cases[0].Name == "some" && Cases[1].Name == "none";
}

bool NoPayloadEnumTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *index) const {
  uint32_t tag = 0;
  if (!reader.readInteger(address, getSize(), &tag)) {
    return false;
  }
  if (tag < getNumCases()) {
    *index = -1;
  } else {
    *index = tag - getNumCases();
  }
  return true;
}

bool NoPayloadEnumTypeInfo::projectEnumValue(remote::MemoryReader &reader,
                                             remote::RemoteAddress address,
                                             int *CaseIndex) const {
  uint32_t tag = 0;
  if (!reader.readInteger(address, getSize(), &tag)) {
    return false;
  }
  if (tag < getNumCases()) {
    *CaseIndex = tag;
    return true;
  } else {
    return false;
  }
}

bool SinglePayloadEnumTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *extraInhabitantIndex) const {
  FieldInfo PayloadCase = getCases()[0];
  if (getSize() < PayloadCase.TI.getSize()) {
    // Single payload enums that use a separate tag don't export any XIs
    // So this is an invalid request.
    return false;
  }

  // Single payload enums inherit XIs from their payload type
  auto NumCases = getNumCases();
  if (NumCases == 1) {
    *extraInhabitantIndex = -1;
    return true;
  } else {
    if (!PayloadCase.TI.readExtraInhabitantIndex(reader, address,
                                                 extraInhabitantIndex)) {
      return false;
    }
    auto NumNonPayloadCases = NumCases - 1;
    if (*extraInhabitantIndex < 0 ||
        (unsigned long)*extraInhabitantIndex < NumNonPayloadCases) {
      *extraInhabitantIndex = -1;
    } else {
      *extraInhabitantIndex -= NumNonPayloadCases;
    }
    return true;
  }
}

// Think of a single-payload enum as being encoded in "pages".
// The discriminator (tag) tells us which page we're on:
// * Page 0 is the payload page which can either store
//   the single payload case (any valid value
//   for the payload) or any of N non-payload cases
//   (encoded as XIs for the payload)
// * Other pages use the payload area to encode non-payload
//   cases.  The number of cases that can be encoded
//   on each such page depends only on the size of the
//   payload area.
//
// The above logic generalizes the following important cases:
// * A payload with XIs will generally have enough to
//   encode all payload cases.  If so, then it will have
//   no discriminator allocated, so the discriminator is
//   always treated as zero.
// * If the payload has no XIs but is not zero-sized, then
//   we'll need a page one.  That page will usually be
//   large enough to encode all non-payload cases.
// * If the payload is zero-sized, then we only have a
//   discriminator.  In effect, the single-payload enum
//   degenerates in this case to a non-payload enum
//   (except for the subtle distinction that the
//   single-payload enum doesn't export XIs).

bool SinglePayloadEnumTypeInfo::projectEnumValue(remote::MemoryReader &reader,
                                                 remote::RemoteAddress address,
                                                 int *CaseIndex) const {
  auto PayloadCase = getCases()[0];
  auto PayloadSize = PayloadCase.TI.getSize();
  auto DiscriminatorAddress = address + PayloadSize;
  auto DiscriminatorSize = getSize() - PayloadSize;
  unsigned discriminator = 0;
  if (DiscriminatorSize > 0) {
    if (!reader.readInteger(DiscriminatorAddress, DiscriminatorSize,
                            &discriminator)) {
      return false;
    }
  }
  unsigned nonPayloadCasesUsingXIs = PayloadCase.TI.getNumExtraInhabitants();
  int ComputedCase = 0;
  if (discriminator == 0) {
    // This is Page 0, which encodes payload case and some additional cases in
    // Xis
    int XITag;
    if (!PayloadCase.TI.readExtraInhabitantIndex(reader, address, &XITag)) {
      return false;
    }
    ComputedCase = XITag < 0 ? 0 : XITag + 1;
  } else {
    // This is some other page, so the entire payload area is just a case
    // index
    unsigned payloadTag;
    if (!reader.readInteger(address, PayloadSize, &payloadTag)) {
      return false;
    }
    auto casesPerNonPayloadPage =
        PayloadSize >= 4 ? ValueWitnessFlags::MaxNumExtraInhabitants
                         : (1UL << (PayloadSize * 8UL));
    ComputedCase =
        1 + nonPayloadCasesUsingXIs                    // Cases on page 0
        + (discriminator - 1) * casesPerNonPayloadPage // Cases on other pages
        + payloadTag;                                  // Cases on this page
  }
  if (static_cast<unsigned>(ComputedCase) < getNumCases()) {
    *CaseIndex = ComputedCase;
    return true;
  }
  *CaseIndex = -1;
  return false;
}

bool TaggedMultiPayloadEnumTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *extraInhabitantIndex) const {
  unsigned long PayloadSize = getPayloadSize();
  unsigned PayloadCount = getNumPayloadCases();
  unsigned TagSize = getSize() - PayloadSize;
  unsigned tag = 0;
  if (!reader.readInteger(address + PayloadSize, getSize() - PayloadSize,
                          &tag)) {
    return false;
  }
  if (tag < PayloadCount + 1) {
    *extraInhabitantIndex = -1; // Valid payload, not an XI
  } else {
    // XIs are coded starting from the highest value that fits
    // E.g., for 1-byte tag, tag 255 == XI #0, tag 254 == XI #1, etc.
    unsigned maxTag = (TagSize >= 4) ? ~0U : (1U << (TagSize * 8U)) - 1;
    *extraInhabitantIndex = maxTag - tag;
  }
  return true;
}

bool TaggedMultiPayloadEnumTypeInfo::projectEnumValue(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *CaseIndex) const {
  unsigned long PayloadSize = getPayloadSize();
  unsigned PayloadCount = NumEffectivePayloadCases;
  unsigned NumCases = getNumCases();
  unsigned TagSize = getSize() - PayloadSize;
  unsigned tag = 0;
  if (!reader.readInteger(address + PayloadSize, getSize() - PayloadSize,
                          &tag)) {
    return false;
  }
  if (tag > ValueWitnessFlags::MaxNumExtraInhabitants) {
    return false;
  } else if (tag < PayloadCount) {
    *CaseIndex = tag;
  } else if (PayloadSize >= 4) {
    unsigned payloadTag = 0;
    if (tag > PayloadCount ||
        !reader.readInteger(address, PayloadSize, &payloadTag) ||
        PayloadCount + payloadTag >= getNumCases()) {
      return false;
    }
    *CaseIndex = PayloadCount + payloadTag;
  } else {
    unsigned payloadTagCount = (1U << (TagSize * 8U)) - 1;
    unsigned maxValidTag =
        (NumCases - PayloadCount) / payloadTagCount + PayloadCount;
    unsigned payloadTag = 0;
    if (tag > maxValidTag ||
        !reader.readInteger(address, PayloadSize, &payloadTag)) {
      return false;
    }
    unsigned ComputedCase =
        PayloadCount + (tag - PayloadCount) * payloadTagCount + payloadTag;
    if (ComputedCase >= NumCases) {
      return false;
    }
    *CaseIndex = ComputedCase;
  }
  return true;
}

BitMask::BitMask(unsigned sizeInBytes) : size(sizeInBytes) {
  // Gracefully fail by constructing an empty mask if we exceed the size
  // limit.
  if (size > maxSize) {
    size = 0;
    mask = nullptr;
    return;
  }

  mask = (uint8_t *)malloc(size);

  if (!mask) {
    // Malloc might fail if size is large due to some bad data. Assert in
    // asserts builds, and fail gracefully in non-asserts builds by
    // constructing an empty BitMask.
    assert(false && "Failed to allocate BitMask");
    size = 0;
    return;
  }

  memset(mask, 0xff, size);
}
// Construct a bitmask of the appropriate number of bytes
// initialized with bits from the specified buffer
BitMask::BitMask(unsigned sizeInBytes, const uint8_t *initialValue,
                 unsigned initialValueBytes, unsigned offset)
    : size(sizeInBytes) {
  // Gracefully fail by constructing an empty mask if we exceed the size
  // limit.
  if (size > maxSize) {
    size = 0;
    mask = nullptr;
    return;
  }

  // Bad data could cause the initial value location to be off the end of our
  // size. If initialValueBytes + offset is beyond sizeInBytes (or overflows),
  // assert in asserts builds, and fail gracefully in non-asserts builds by
  // constructing an empty BitMask.
  bool overflowed = false;
  unsigned initialValueEnd =
      llvm::SaturatingAdd(initialValueBytes, offset, &overflowed);
  if (overflowed) {
    assert(false && "initialValueBytes + offset overflowed");
    size = 0;
    mask = nullptr;
    return;
  }
  assert(initialValueEnd <= sizeInBytes);
  if (initialValueEnd > size) {
    assert(false && "initialValueBytes + offset is greater than size");
    size = 0;
    mask = nullptr;
    return;
  }

  mask = (uint8_t *)calloc(1, size);

  if (!mask) {
    // Malloc might fail if size is large due to some bad data. Assert in
    // asserts builds, and fail gracefully in non-asserts builds by
    // constructing an empty BitMask.
    assert(false && "Failed to allocate BitMask");
    size = 0;
    return;
  }

  memcpy(mask + offset, initialValue, initialValueBytes);
}
// Move constructor moves ownership and zeros the src
BitMask::BitMask(BitMask &&src) noexcept
    : size(src.size), mask(std::move(src.mask)) {
  src.size = 0;
  src.mask = nullptr;
}
// Copy constructor makes a copy of the mask storage
BitMask::BitMask(const BitMask &src) noexcept : size(src.size), mask(nullptr) {
  mask = (uint8_t *)malloc(size);
  memcpy(mask, src.mask, size);
}

std::string BitMask::str() const {
  std::ostringstream buff;
  buff << size << ":0x";
  for (unsigned i = 0; i < size; i++) {
    buff << std::hex << ((mask[i] >> 4) & 0x0f) << (mask[i] & 0x0f);
  }
  return buff.str();
}

bool BitMask::operator==(const BitMask &rhs) const {
  // The two masks may be of different sizes.
  // The common prefix must be identical.
  size_t common = std::min(size, rhs.size);
  if (memcmp(mask, rhs.mask, common) != 0)
    return false;
  // The remainder of the longer mask must be
  // all zero bits.
  unsigned mustBeZeroSize = std::max(size, rhs.size) - common;
  uint8_t *mustBeZero;
  if (size < rhs.size) {
    mustBeZero = rhs.mask + size;
  } else if (size > rhs.size) {
    mustBeZero = mask + rhs.size;
  }
  for (unsigned i = 0; i < mustBeZeroSize; ++i) {
    if (mustBeZero[i] != 0) {
      return false;
    }
  }
  return true;
}

bool BitMask::isZero() const {
  for (unsigned i = 0; i < size; ++i) {
    if (mask[i] != 0) {
      return false;
    }
  }
  return true;
}

void BitMask::complement() {
  for (unsigned i = 0; i < size; ++i) {
    mask[i] = ~mask[i];
  }
}

int BitMask::countSetBits() const {
  static const int counter[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
  int bits = 0;
  for (unsigned i = 0; i < size; ++i) {
    bits += counter[mask[i] >> 4] + counter[mask[i] & 15];
  }
  return bits;
}

int BitMask::countZeroBits() const {
  static const int counter[] = {4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0};
  int bits = 0;
  for (unsigned i = 0; i < size; ++i) {
    bits += counter[mask[i] >> 4] + counter[mask[i] & 15];
  }
  return bits;
}

// Zero all bits except for the `n` most significant ones.
// XXX TODO: Big-endian support?
void BitMask::keepOnlyMostSignificantBits(unsigned n) {
  unsigned count = 0;
  if (size < 1) {
    return;
  }
  unsigned i = size;
  while (i > 0) {
    i -= 1;
    if (count < n) {
      for (int b = 128; b > 0; b >>= 1) {
        if (count >= n) {
          mask[i] &= ~b;
        } else if ((mask[i] & b) != 0) {
          ++count;
        }
      }
    } else {
      mask[i] = 0;
    }
  }
}

unsigned BitMask::numSetBits() const {
  unsigned count = 0;
  for (unsigned i = 0; i < size; ++i) {
    if (mask[i] != 0) {
      for (unsigned b = 1; b < 256; b <<= 1) {
        if ((mask[i] & b) != 0) {
          ++count;
        }
      }
    }
  }
  return count;
}

void BitMask::andMask(void *maskData, unsigned len, unsigned offset) {
  if (offset < size) {
    unsigned common = std::min(len, size - offset);
    uint8_t *maskBytes = (uint8_t *)maskData;
    for (unsigned i = 0; i < common; ++i) {
      mask[i + offset] &= maskBytes[i];
    }
  }
}

void BitMask::andNotMask(void *maskData, unsigned len, unsigned offset) {
  assert(offset < size);
  if (offset < size) {
    unsigned common = std::min(len, size - offset);
    uint8_t *maskBytes = (uint8_t *)maskData;
    for (unsigned i = 0; i < common; ++i) {
      mask[i + offset] &= ~maskBytes[i];
    }
  }
}

bool MultiPayloadEnumTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *extraInhabitantIndex) const {
  unsigned long payloadSize = getPayloadSize();

  // Multi payload enums that use spare bits export unused tag values as XIs.
  uint32_t tag = 0;
  unsigned tagBits = 0;

  // The full tag value is built by combining three sets of bits:
  // Low-order bits: payload tag bits (most-significant spare bits)
  // Middle: spare bits that are not payload tag bits
  // High-order: extra discriminator byte

  auto payloadTagLowBitsMask = getMultiPayloadTagBitsMask();
  auto payloadTagLowBitCount = payloadTagLowBitsMask.countSetBits();
  uint32_t payloadTagLow = 0;
  if (!payloadTagLowBitsMask.readMaskedInteger(reader, address,
                                               &payloadTagLow)) {
    return false;
  }

  // Add the payload tag bits to the growing tag...
  tag = payloadTagLow;
  tagBits = payloadTagLowBitCount;

  // Read the other spare bits
  auto otherSpareBitsMask = spareBitsMask; // copy
  otherSpareBitsMask.andNotMask(payloadTagLowBitsMask, 0);
  auto otherSpareBitsCount = otherSpareBitsMask.countSetBits();
  if (otherSpareBitsCount > 0) {
    // Add other spare bits to the growing tag...
    uint32_t otherSpareBits = 0;
    if (!otherSpareBitsMask.readMaskedInteger(reader, address,
                                              &otherSpareBits)) {
      return false;
    }
    tag |= otherSpareBits << tagBits;
    tagBits += otherSpareBitsCount;
  }

  // If there is an extra discriminator tag, add those bits to the tag
  auto extraTagSize = getSize() - payloadSize;
  unsigned extraTag = 0;
  if (extraTagSize > 0 && tagBits < 32) {
    auto extraTagAddress = address + payloadSize;
    if (!reader.readInteger(extraTagAddress, extraTagSize, &extraTag)) {
      return false;
    }
  }
  tag |= extraTag << tagBits;
  tagBits += extraTagSize * 8;

  // Check whether this tag is used for valid content
  auto payloadCases = getNumPayloadCases();
  auto nonPayloadCases = getNumCases() - payloadCases;
  uint32_t inhabitedTags;
  if (nonPayloadCases == 0) {
    inhabitedTags = payloadCases;
  } else {
    auto payloadBitsForTags = spareBitsMask.countZeroBits();
    uint32_t nonPayloadTags =
        (nonPayloadCases + (1 << payloadBitsForTags) - 1) >> payloadBitsForTags;
    inhabitedTags = payloadCases + nonPayloadTags;
  }

  if (tag < inhabitedTags) {
    *extraInhabitantIndex = -1;
    return true;
  }

  // Transform the tag value into the XI index
  uint32_t maxTag = (tagBits >= 32) ? ~0u : (1UL << tagBits) - 1;
  *extraInhabitantIndex = maxTag - tag;
  return true;
}

bool MultiPayloadEnumTypeInfo::projectEnumValue(remote::MemoryReader &reader,
                                                remote::RemoteAddress address,
                                                int *CaseIndex) const {
  unsigned long payloadSize = getPayloadSize();

  // Extra Tag (if any) holds upper bits of case value
  auto extraTagSize = getSize() - payloadSize;
  unsigned extraTag = 0;
  if (extraTagSize > 0) {
    auto extraTagAddress = address + payloadSize;
    if (!reader.readInteger(extraTagAddress, extraTagSize, &extraTag)) {
      return false;
    }
  }

  // The `payloadTagMask` is a subset of the spare bits
  // where we encode the rest of the case value.
  auto payloadTagMask = getMultiPayloadTagBitsMask();
  auto numPayloadTagBits = payloadTagMask.countSetBits();
  uint64_t payloadTag = 0;
  if (!payloadTagMask.readMaskedInteger(reader, address, &payloadTag)) {
    return false;
  }

  // Combine the extra tag and payload tag info:
  int tagValue = 0;
  if (numPayloadTagBits >= 32) {
    tagValue = payloadTag;
  } else {
    tagValue = (extraTag << numPayloadTagBits) | payloadTag;
  }

  // If the above identifies a payload case, we're done
  if (static_cast<unsigned>(tagValue) < NumEffectivePayloadCases) {
    *CaseIndex = tagValue;
    return true;
  }

  // Otherwise, combine with other payload data to select a non-payload case
  auto occupiedBits = spareBitsMask; // Copy
  occupiedBits.complement();

  auto occupiedBitCount = occupiedBits.countSetBits();
  uint64_t payloadValue = 0;
  if (!occupiedBits.readMaskedInteger(reader, address, &payloadValue)) {
    return false;
  }

  int ComputedCase = 0;
  if (occupiedBitCount >= 32) {
    ComputedCase = payloadValue + NumEffectivePayloadCases;
  } else {
    ComputedCase =
        (((tagValue - NumEffectivePayloadCases) << occupiedBitCount) |
         payloadValue) +
        NumEffectivePayloadCases;
  }

  if (static_cast<unsigned>(ComputedCase) < getNumCases()) {
    *CaseIndex = ComputedCase;
    return true;
  } else {
    *CaseIndex = -1;
    return false;
  }
}

// The case value is stored in three pieces:
// * A separate "discriminator" tag appended to the payload (if necessary)
// * A "payload tag" that uses (a subset of) the spare bits
// * The remainder of the payload bits (for non-payload cases)
// This computes the bits used for the payload tag.
BitMask MultiPayloadEnumTypeInfo::getMultiPayloadTagBitsMask() const {
  auto payloadTagValues = NumEffectivePayloadCases - 1;
  if (getNumCases() > NumEffectivePayloadCases) {
    payloadTagValues += 1;
  }
  int payloadTagBits = 0;
  while (payloadTagValues > 0) {
    payloadTagValues >>= 1;
    payloadTagBits += 1;
  }
  BitMask payloadTagBitsMask = spareBitsMask;
  payloadTagBitsMask.keepOnlyMostSignificantBits(payloadTagBits);
  return payloadTagBitsMask;
}

} // namespace reflection
} // namespace swift

#endif
