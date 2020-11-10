//===--- Deserialization.cpp - Loading a serialized AST -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "BCReadingExtras.h"
#include "DeserializationErrors.h"
#include "ModuleFile.h"
#include "ModuleFormat.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/ClangImporter/SwiftAbstractBasicReader.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "Serialization"

STATISTIC(NumDeclsLoaded, "# of decls deserialized");
STATISTIC(NumMemberListsLoaded,
          "# of nominals/extensions whose members were loaded");
STATISTIC(NumNormalProtocolConformancesLoaded,
          "# of normal protocol conformances deserialized");
STATISTIC(NumNormalProtocolConformancesCompleted,
          "# of normal protocol conformances completed");
STATISTIC(NumNestedTypeShortcuts,
          "# of nested types resolved without full lookup");

using namespace swift;
using namespace swift::serialization;
using llvm::Expected;

StringRef swift::getNameOfModule(const ModuleFile *MF) {
  return MF->getName();
}

namespace {
  struct DeclAndOffset {
    const Decl *D;
    uint64_t offset;
  };

  static raw_ostream &operator<<(raw_ostream &os, DeclAndOffset &&pair) {
    return os << Decl::getKindName(pair.D->getKind())
              << "Decl @ " << pair.offset;
  }

  class PrettyDeclDeserialization : public llvm::PrettyStackTraceEntry {
    const ModuleFile *MF;
    const ModuleFile::Serialized<Decl*> &DeclOrOffset;
    uint64_t offset;
    decls_block::RecordKind Kind;
  public:
    PrettyDeclDeserialization(ModuleFile *module,
                              const ModuleFile::Serialized<Decl*> &declOrOffset,
                              decls_block::RecordKind kind)
      : MF(module), DeclOrOffset(declOrOffset), offset(declOrOffset),
        Kind(kind) {
    }

    static const char *getRecordKindString(decls_block::RecordKind Kind) {
      switch (Kind) {
#define RECORD(Id) case decls_block::Id: return #Id;
#include "DeclTypeRecordNodes.def"
      }

      llvm_unreachable("Unhandled RecordKind in switch.");
    }

    void print(raw_ostream &os) const override {
      if (!DeclOrOffset.isComplete()) {
        os << "While deserializing decl @ " << offset << " ("
           << getRecordKindString(Kind) << ")";
      } else {
        os << "While deserializing ";

        if (auto VD = dyn_cast<ValueDecl>(DeclOrOffset.get())) {
          os << "'" << VD->getBaseName() << "' (" << DeclAndOffset{VD, offset}
             << ")";
        } else if (auto ED = dyn_cast<ExtensionDecl>(DeclOrOffset.get())) {
          os << "extension of '" << ED->getExtendedType() << "' ("
             << DeclAndOffset{ED, offset} << ")";
        } else {
          os << DeclAndOffset{DeclOrOffset.get(), offset};
        }
      }
      os << " in '" << getNameOfModule(MF) << "'\n";
    }
  };

  class PrettySupplementalDeclNameTrace : public llvm::PrettyStackTraceEntry {
    DeclName name;
  public:
    PrettySupplementalDeclNameTrace(DeclName name)
      : name(name) { }

    void print(raw_ostream &os) const override {
      os << "    ...decl is named '" << name << "'\n";
    }
  };

  class PrettyXRefTrace :
      public llvm::PrettyStackTraceEntry,
      public XRefTracePath {
  public:
    explicit PrettyXRefTrace(ModuleDecl &M) : XRefTracePath(M) {}

    void print(raw_ostream &os) const override {
      XRefTracePath::print(os, "\t");
    }
  };
} // end anonymous namespace

const char DeclDeserializationError::ID = '\0';
void DeclDeserializationError::anchor() {}
const char XRefError::ID = '\0';
void XRefError::anchor() {}
const char XRefNonLoadedModuleError::ID = '\0';
void XRefNonLoadedModuleError::anchor() {}
const char OverrideError::ID = '\0';
void OverrideError::anchor() {}
const char TypeError::ID = '\0';
void TypeError::anchor() {}
const char ExtensionError::ID = '\0';
void ExtensionError::anchor() {}
const char DeclAttributesDidNotMatch::ID = '\0';
void DeclAttributesDidNotMatch::anchor() {}

/// Skips a single record in the bitstream.
///
/// Destroys the stream position if the next entry is not a record.
static void skipRecord(llvm::BitstreamCursor &cursor, unsigned recordKind) {
  auto next = llvm::cantFail<llvm::BitstreamEntry>(
      cursor.advance(AF_DontPopBlockAtEnd));
  assert(next.Kind == llvm::BitstreamEntry::Record);

  unsigned kind = llvm::cantFail<unsigned>(cursor.skipRecord(next.ID));
  assert(kind == recordKind);
  (void)kind;
}

void ModuleFile::fatal(llvm::Error error) {
  if (FileContext) {
    getContext().Diags.diagnose(SourceLoc(), diag::serialization_fatal, Core->Name);
    getContext().Diags.diagnose(SourceLoc(), diag::serialization_misc_version,
      Core->Name, Core->MiscVersion);

    if (!Core->CompatibilityVersion.empty()) {
      if (getContext().LangOpts.EffectiveLanguageVersion
            != Core->CompatibilityVersion) {
        SmallString<16> effectiveVersionBuffer, compatVersionBuffer;
        {
          llvm::raw_svector_ostream out(effectiveVersionBuffer);
          out << getContext().LangOpts.EffectiveLanguageVersion;
        }
        {
          llvm::raw_svector_ostream out(compatVersionBuffer);
          out << Core->CompatibilityVersion;
        }
        getContext().Diags.diagnose(
            SourceLoc(), diag::serialization_compatibility_version_mismatch,
            effectiveVersionBuffer, Core->Name, compatVersionBuffer);
      }
    }
  }

  ModuleFileSharedCore::fatal(std::move(error));
}

static Optional<swift::AccessorKind>
getActualAccessorKind(uint8_t raw) {
  switch (serialization::AccessorKind(raw)) {
#define ACCESSOR(ID) \
  case serialization::AccessorKind::ID: return swift::AccessorKind::ID;
#include "swift/AST/AccessorKinds.def"
  }

  return None;
}

/// Translate from the serialization DefaultArgumentKind enumerators, which are
/// guaranteed to be stable, to the AST ones.
static Optional<swift::DefaultArgumentKind>
getActualDefaultArgKind(uint8_t raw) {
  switch (static_cast<serialization::DefaultArgumentKind>(raw)) {
#define CASE(X) \
  case serialization::DefaultArgumentKind::X: \
    return swift::DefaultArgumentKind::X;
  CASE(None)
  CASE(Normal)
  CASE(Inherited)
  CASE(Column)
  CASE(FileID)
  CASE(FileIDSpelledAsFile)
  CASE(FilePath)
  CASE(FilePathSpelledAsFile)
  CASE(Line)
  CASE(Function)
  CASE(DSOHandle)
  CASE(NilLiteral)
  CASE(EmptyArray)
  CASE(EmptyDictionary)
  CASE(StoredProperty)
#undef CASE
  }
  return None;
}

static Optional<StableSerializationPath::ExternalPath::ComponentKind>
getActualClangDeclPathComponentKind(uint64_t raw) {
  switch (static_cast<serialization::ClangDeclPathComponentKind>(raw)) {
#define CASE(ID) \
  case serialization::ClangDeclPathComponentKind::ID: \
    return StableSerializationPath::ExternalPath::ID;
  CASE(Record)
  CASE(Enum)
  CASE(Namespace)
  CASE(Typedef)
  CASE(TypedefAnonDecl)
  CASE(ObjCInterface)
  CASE(ObjCProtocol)
#undef CASE
  }
  return None;
}

ParameterList *ModuleFile::readParameterList() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;
  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  unsigned recordID =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, scratch));
  assert(recordID == PARAMETERLIST);
  (void) recordID;

  ArrayRef<uint64_t> rawMemberIDs;
  decls_block::ParameterListLayout::readRecord(scratch, rawMemberIDs);

  SmallVector<ParamDecl *, 8> params;
  for (DeclID paramID : rawMemberIDs)
    params.push_back(cast<ParamDecl>(getDecl(paramID)));

  return ParameterList::create(getContext(), params);
}

Expected<Pattern *> ModuleFile::readPattern(DeclContext *owningDC) {
  // Currently, the only case in which this function can fail (return an error)
  // is when reading a pattern for a single variable declaration.

  using namespace decls_block;

  auto readPatternUnchecked = [this](DeclContext *owningDC) -> Pattern * {
    Expected<Pattern *> deserialized = readPattern(owningDC);
    if (!deserialized) {
      fatal(deserialized.takeError());
    }
    assert(deserialized.get());
    return deserialized.get();
  };

  SmallVector<uint64_t, 8> scratch;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    fatal();

  /// Local function to record the type of this pattern.
  auto recordPatternType = [&](Pattern *pattern, Type type) {
    if (type->hasTypeParameter())
      pattern->setDelayedInterfaceType(type, owningDC);
    else
      pattern->setType(type);
  };

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch));
  switch (kind) {
  case decls_block::PAREN_PATTERN: {
    Pattern *subPattern = readPatternUnchecked(owningDC);

    auto result = ParenPattern::createImplicit(getContext(), subPattern);

    if (Type interfaceType = subPattern->getDelayedInterfaceType())
      result->setDelayedInterfaceType(ParenType::get(getContext(),
                                                     interfaceType), owningDC);
    else
      result->setType(ParenType::get(getContext(), subPattern->getType()));
    restoreOffset.reset();
    return result;
  }
  case decls_block::TUPLE_PATTERN: {
    TypeID tupleTypeID;
    unsigned count;

    TuplePatternLayout::readRecord(scratch, tupleTypeID, count);

    SmallVector<TuplePatternElt, 8> elements;
    for ( ; count > 0; --count) {
      scratch.clear();
      next = fatalIfUnexpected(DeclTypeCursor.advance());
      assert(next.Kind == llvm::BitstreamEntry::Record);

      kind = fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch));
      assert(kind == decls_block::TUPLE_PATTERN_ELT);

      // FIXME: Add something for this record or remove it.
      IdentifierID labelID;
      TuplePatternEltLayout::readRecord(scratch, labelID);
      Identifier label = getIdentifier(labelID);

      Pattern *subPattern = readPatternUnchecked(owningDC);
      elements.push_back(TuplePatternElt(label, SourceLoc(), subPattern));
    }

    auto result = TuplePattern::createImplicit(getContext(), elements);
    recordPatternType(result, getType(tupleTypeID));
    restoreOffset.reset();
    return result;
  }
  case decls_block::NAMED_PATTERN: {
    DeclID varID;
    TypeID typeID;
    NamedPatternLayout::readRecord(scratch, varID, typeID);

    auto deserialized = getDeclChecked(varID);
    if (!deserialized) {
      // Pass through the error. It's too bad that it affects the whole pattern,
      // but that's what we get.
      return deserialized.takeError();
    }

    auto var = cast<VarDecl>(deserialized.get());
    auto result = NamedPattern::createImplicit(getContext(), var);
    recordPatternType(result, getType(typeID));
    restoreOffset.reset();
    return result;
  }
  case decls_block::ANY_PATTERN: {
    TypeID typeID;

    AnyPatternLayout::readRecord(scratch, typeID);
    auto result = AnyPattern::createImplicit(getContext());
    recordPatternType(result, getType(typeID));
    restoreOffset.reset();
    return result;
  }
  case decls_block::TYPED_PATTERN: {
    TypeID typeID;
    TypedPatternLayout::readRecord(scratch, typeID);

    Expected<Pattern *> subPattern = readPattern(owningDC);
    if (!subPattern) {
      // Pass through any errors.
      return subPattern;
    }

    auto type = getType(typeID);
    auto result = TypedPattern::createImplicit(getContext(),
                                               subPattern.get(), type);
    recordPatternType(result, type);
    restoreOffset.reset();
    return result;
  }
  case decls_block::VAR_PATTERN: {
    bool isLet;
    BindingPatternLayout::readRecord(scratch, isLet);

    Pattern *subPattern = readPatternUnchecked(owningDC);

    auto result =
        BindingPattern::createImplicit(getContext(), isLet, subPattern);
    if (Type interfaceType = subPattern->getDelayedInterfaceType())
      result->setDelayedInterfaceType(interfaceType, owningDC);
    else
      result->setType(subPattern->getType());
    restoreOffset.reset();
    return result;
  }

  default:
    return nullptr;
  }
}

SILLayout *ModuleFile::readSILLayout(llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  SmallVector<uint64_t, 16> scratch;

  llvm::BitstreamEntry next =
      fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
  assert(next.Kind == llvm::BitstreamEntry::Record);

  unsigned kind = fatalIfUnexpected(Cursor.readRecord(next.ID, scratch));
  switch (kind) {
  case decls_block::SIL_LAYOUT: {
    GenericSignatureID rawGenericSig;
    unsigned numFields;
    ArrayRef<uint64_t> types;
    decls_block::SILLayoutLayout::readRecord(scratch, rawGenericSig,
                                             numFields, types);
    
    SmallVector<SILField, 4> fields;
    for (auto fieldInfo : types.slice(0, numFields)) {
      bool isMutable = fieldInfo & 0x80000000U;
      auto typeId = fieldInfo & 0x7FFFFFFFU;
      fields.push_back(
        SILField(getType(typeId)->getCanonicalType(),
                 isMutable));
    }
    
    CanGenericSignature canSig;
    if (auto sig = getGenericSignature(rawGenericSig))
      canSig = sig.getCanonicalSignature();
    return SILLayout::get(getContext(), canSig, fields);
  }
  default:
    fatal();
  }
}

ProtocolConformanceRef ModuleFile::readConformance(
                                             llvm::BitstreamCursor &Cursor,
                                             GenericEnvironment *genericEnv) {
  auto conformance = readConformanceChecked(Cursor, genericEnv);
  if (!conformance)
    fatal(conformance.takeError());
  return conformance.get();
}

Expected<ProtocolConformanceRef>
ModuleFile::readConformanceChecked(llvm::BitstreamCursor &Cursor,
                                   GenericEnvironment *genericEnv) {
  using namespace decls_block;

  SmallVector<uint64_t, 16> scratch;

  llvm::BitstreamEntry next =
      fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
  assert(next.Kind == llvm::BitstreamEntry::Record);

  if (auto *Stats = getContext().Stats)
    ++Stats->getFrontendCounters().NumConformancesDeserialized;

  unsigned kind = fatalIfUnexpected(Cursor.readRecord(next.ID, scratch));
  switch (kind) {
  case INVALID_PROTOCOL_CONFORMANCE: {
    return ProtocolConformanceRef::forInvalid();
  }

  case ABSTRACT_PROTOCOL_CONFORMANCE: {
    DeclID protoID;
    AbstractProtocolConformanceLayout::readRecord(scratch, protoID);

    auto decl = getDeclChecked(protoID);
    if (!decl)
      return decl.takeError();

    auto proto = cast<ProtocolDecl>(decl.get());
    return ProtocolConformanceRef(proto);
  }

  case SELF_PROTOCOL_CONFORMANCE: {
    DeclID protoID;
    SelfProtocolConformanceLayout::readRecord(scratch, protoID);

    auto decl = getDeclChecked(protoID);
    if (!decl)
      return decl.takeError();

    auto proto = cast<ProtocolDecl>(decl.get());
    auto conformance = getContext().getSelfConformance(proto);
    return ProtocolConformanceRef(conformance);
  }

  case SPECIALIZED_PROTOCOL_CONFORMANCE: {
    TypeID conformingTypeID;
    SubstitutionMapID substitutionMapID;
    SpecializedProtocolConformanceLayout::readRecord(scratch, conformingTypeID,
                                                     substitutionMapID);

    ASTContext &ctx = getContext();
    Type conformingType = getType(conformingTypeID);
    if (genericEnv) {
      conformingType = genericEnv->mapTypeIntoContext(conformingType);
    }

    PrettyStackTraceType trace(getAssociatedModule()->getASTContext(),
                               "reading specialized conformance for",
                               conformingType);

    auto subMapOrError = getSubstitutionMapChecked(substitutionMapID);
    if (!subMapOrError)
      return subMapOrError.takeError();
    auto subMap = subMapOrError.get();

    ProtocolConformanceRef genericConformance =
      readConformance(Cursor, genericEnv);
    PrettyStackTraceDecl traceTo("... to", genericConformance.getRequirement());

    assert(genericConformance.isConcrete() && "Abstract generic conformance?");
    auto conformance =
           ctx.getSpecializedConformance(conformingType,
                                         genericConformance.getConcrete(),
                                         subMap);
    return ProtocolConformanceRef(conformance);
  }

  case INHERITED_PROTOCOL_CONFORMANCE: {
    TypeID conformingTypeID;
    InheritedProtocolConformanceLayout::readRecord(scratch, conformingTypeID);

    ASTContext &ctx = getContext();
    Type conformingType = getType(conformingTypeID);
    if (genericEnv) {
      conformingType = genericEnv->mapTypeIntoContext(conformingType);
    }

    PrettyStackTraceType trace(getAssociatedModule()->getASTContext(),
                               "reading inherited conformance for",
                               conformingType);

    ProtocolConformanceRef inheritedConformance =
      readConformance(Cursor, genericEnv);
    PrettyStackTraceDecl traceTo("... to",
                                 inheritedConformance.getRequirement());

    assert(inheritedConformance.isConcrete() &&
           "Abstract inherited conformance?");
    auto conformance =
      ctx.getInheritedConformance(conformingType,
                                  inheritedConformance.getConcrete());
    return ProtocolConformanceRef(conformance);
  }

  case NORMAL_PROTOCOL_CONFORMANCE_ID: {
    NormalConformanceID conformanceID;
    NormalProtocolConformanceIdLayout::readRecord(scratch, conformanceID);

    auto conformance = readNormalConformanceChecked(conformanceID);
    if (!conformance)
      return conformance.takeError();
    return ProtocolConformanceRef(conformance.get());
  }

  case PROTOCOL_CONFORMANCE_XREF: {
    DeclID protoID;
    DeclID nominalID;
    ModuleID moduleID;
    ProtocolConformanceXrefLayout::readRecord(scratch, protoID, nominalID,
                                              moduleID);

    auto maybeNominal = getDeclChecked(nominalID);
    if (!maybeNominal)
      return maybeNominal.takeError();

    auto nominal = cast<NominalTypeDecl>(maybeNominal.get());
    PrettyStackTraceDecl trace("cross-referencing conformance for", nominal);
    auto proto = cast<ProtocolDecl>(getDecl(protoID));
    PrettyStackTraceDecl traceTo("... to", proto);
    auto module = getModule(moduleID);

    // FIXME: If the module hasn't been loaded, we probably don't want to fall
    // back to the current module like this.
    if (!module)
      module = getAssociatedModule();

    SmallVector<ProtocolConformance *, 2> conformances;
    nominal->lookupConformance(module, proto, conformances);
    PrettyStackTraceModuleFile traceMsg(
        "If you're seeing a crash here, check that your SDK and dependencies "
        "are at least as new as the versions used to build", *this);
    // This would normally be an assertion but it's more useful to print the
    // PrettyStackTrace here even in no-asserts builds.
    if (conformances.empty())
      abort();
    return ProtocolConformanceRef(conformances.front());
  }

  // Not a protocol conformance.
  default:
    fatal();
  }
}

Expected<NormalProtocolConformance *> ModuleFile::readNormalConformanceChecked(
                             NormalConformanceID conformanceID) {
  auto &conformanceEntry = NormalConformances[conformanceID-1];
  if (conformanceEntry.isComplete()) {
    return conformanceEntry.get();
  }

  using namespace decls_block;

  // Find the conformance record.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(conformanceEntry));
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());
  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  DeclID protoID;
  DeclContextID contextID;
  unsigned valueCount, typeCount, conformanceCount;
  ArrayRef<uint64_t> rawIDs;
  SmallVector<uint64_t, 16> scratch;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, scratch));
  if (kind != NORMAL_PROTOCOL_CONFORMANCE)
    fatal();

  NormalProtocolConformanceLayout::readRecord(scratch, protoID,
                                              contextID, typeCount,
                                              valueCount, conformanceCount,
                                              rawIDs);

  ASTContext &ctx = getContext();
  auto doOrError = getDeclContextChecked(contextID);
  if (!doOrError)
    return doOrError.takeError();
  DeclContext *dc = doOrError.get();

  assert(!isa<ClangModuleUnit>(dc->getModuleScopeContext())
         && "should not have serialized a conformance from a clang module");
  Type conformingType = dc->getDeclaredInterfaceType();
  PrettyStackTraceType trace(ctx, "reading conformance for", conformingType);

  auto protoOrError = getDeclChecked(protoID);
  if (!protoOrError)
    return protoOrError.takeError();
  auto proto = cast<ProtocolDecl>(protoOrError.get());

  PrettyStackTraceDecl traceTo("... to", proto);
  ++NumNormalProtocolConformancesLoaded;

  auto conformance = ctx.getConformance(conformingType, proto, SourceLoc(), dc,
                                        ProtocolConformanceState::Incomplete);

  // Record this conformance.
  if (conformanceEntry.isComplete())
    return conformance;

  uint64_t offset = conformanceEntry;
  conformanceEntry = conformance;

  dc->getSelfNominalTypeDecl()->registerProtocolConformance(conformance);

  // If the conformance is complete, we're done.
  if (conformance->isComplete())
    return conformance;

  conformance->setState(ProtocolConformanceState::Complete);
  conformance->setLazyLoader(this, offset);
  return conformance;
}

GenericParamList *ModuleFile::maybeReadGenericParams(DeclContext *DC) {
  using namespace decls_block;

  assert(DC && "need a context for the decls in the list");

  BCOffsetRAII lastRecordOffset(DeclTypeCursor);
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return nullptr;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch, &blobData));
  if (kind != GENERIC_PARAM_LIST)
    return nullptr;
  lastRecordOffset.reset();

  SmallVector<GenericTypeParamDecl *, 8> params;

  ArrayRef<uint64_t> paramIDs;
  GenericParamListLayout::readRecord(scratch, paramIDs);
  for (DeclID nextParamID : paramIDs) {
    auto genericParam = cast<GenericTypeParamDecl>(getDecl(nextParamID));
    params.push_back(genericParam);
  }

  // Don't create empty generic parameter lists. (This should never happen in
  // practice, but it doesn't hurt to be defensive.)
  if (params.empty())
    return nullptr;

  return GenericParamList::create(getContext(), SourceLoc(),
                                  params, SourceLoc(), { },
                                  SourceLoc());
}

void ModuleFile::readGenericRequirements(
                   SmallVectorImpl<Requirement> &requirements,
                   llvm::BitstreamCursor &Cursor) {
  auto error = readGenericRequirementsChecked(requirements, Cursor);
  if (error)
    fatal(std::move(error));
}

llvm::Error ModuleFile::readGenericRequirementsChecked(
                   SmallVectorImpl<Requirement> &requirements,
                   llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  BCOffsetRAII lastRecordOffset(Cursor);
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  while (true) {
    lastRecordOffset.reset();
    bool shouldContinue = true;

    llvm::BitstreamEntry entry =
        fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
    if (entry.Kind != llvm::BitstreamEntry::Record)
      break;

    scratch.clear();
    unsigned recordID = fatalIfUnexpected(
        Cursor.readRecord(entry.ID, scratch, &blobData));
    switch (recordID) {
    case GENERIC_REQUIREMENT: {
      uint8_t rawKind;
      uint64_t rawTypeIDs[2];
      GenericRequirementLayout::readRecord(scratch, rawKind,
                                           rawTypeIDs[0], rawTypeIDs[1]);

      switch (rawKind) {
      case GenericRequirementKind::Conformance: {
        auto subject = getTypeChecked(rawTypeIDs[0]);
        if (!subject)
          return subject.takeError();

        auto constraint = getTypeChecked(rawTypeIDs[1]);
        if (!constraint)
          return constraint.takeError();

        requirements.push_back(Requirement(RequirementKind::Conformance,
                                           subject.get(), constraint.get()));
        break;
      }
      case GenericRequirementKind::Superclass: {
        auto subject = getTypeChecked(rawTypeIDs[0]);
        if (!subject)
          return subject.takeError();

        auto constraint = getTypeChecked(rawTypeIDs[1]);
        if (!constraint)
          return constraint.takeError();

        requirements.push_back(Requirement(RequirementKind::Superclass,
                                           subject.get(), constraint.get()));
        break;
      }
      case GenericRequirementKind::SameType: {
        auto first = getTypeChecked(rawTypeIDs[0]);
        if (!first)
          return first.takeError();

        auto second = getTypeChecked(rawTypeIDs[1]);
        if (!second)
          return second.takeError();

        requirements.push_back(Requirement(RequirementKind::SameType,
                                           first.get(), second.get()));
        break;
      }
      default:
        // Unknown requirement kind.
        fatal();
      }
      break;
      }
    case LAYOUT_REQUIREMENT: {
      uint8_t rawKind;
      uint64_t rawTypeID;
      uint32_t size;
      uint32_t alignment;
      LayoutRequirementLayout::readRecord(scratch, rawKind, rawTypeID,
                                          size, alignment);

      auto first = getTypeChecked(rawTypeID);
      if (!first)
        return first.takeError();

      LayoutConstraint layout;
      LayoutConstraintKind kind = LayoutConstraintKind::UnknownLayout;
      switch (rawKind) {
      default:
        // Unknown layout requirement kind.
        fatal();
      case LayoutRequirementKind::NativeRefCountedObject:
        kind = LayoutConstraintKind::NativeRefCountedObject;
        break;
      case LayoutRequirementKind::RefCountedObject:
        kind = LayoutConstraintKind::RefCountedObject;
        break;
      case LayoutRequirementKind::Trivial:
        kind = LayoutConstraintKind::Trivial;
        break;
      case LayoutRequirementKind::TrivialOfExactSize:
        kind = LayoutConstraintKind::TrivialOfExactSize;
        break;
      case LayoutRequirementKind::TrivialOfAtMostSize:
        kind = LayoutConstraintKind::TrivialOfAtMostSize;
        break;
      case LayoutRequirementKind::Class:
        kind = LayoutConstraintKind::Class;
        break;
      case LayoutRequirementKind::NativeClass:
        kind = LayoutConstraintKind::NativeClass;
        break;
      case LayoutRequirementKind::UnknownLayout:
        kind = LayoutConstraintKind::UnknownLayout;
        break;
      }

      ASTContext &ctx = getContext();
      if (kind != LayoutConstraintKind::TrivialOfAtMostSize &&
          kind != LayoutConstraintKind::TrivialOfExactSize)
        layout = LayoutConstraint::getLayoutConstraint(kind, ctx);
      else
        layout =
            LayoutConstraint::getLayoutConstraint(kind, size, alignment, ctx);

      requirements.push_back(
          Requirement(RequirementKind::Layout, first.get(), layout));
      break;
      }
    default:
      // This record is not part of the GenericParamList.
      shouldContinue = false;
      break;
    }

    if (!shouldContinue)
      break;
  }

  return llvm::Error::success();
}

/// Advances past any records that might be part of a requirement signature.
static llvm::Error skipGenericRequirements(llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  BCOffsetRAII lastRecordOffset(Cursor);

  while (true) {
    Expected<llvm::BitstreamEntry> maybeEntry =
        Cursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry)
      return maybeEntry.takeError();
    llvm::BitstreamEntry entry = maybeEntry.get();
    if (entry.Kind != llvm::BitstreamEntry::Record)
      break;

    Expected<unsigned> maybeRecordID = Cursor.skipRecord(entry.ID);
    if (!maybeRecordID)
      return maybeRecordID.takeError();
    switch (maybeRecordID.get()) {
    case GENERIC_REQUIREMENT:
    case LAYOUT_REQUIREMENT:
      break;

    default:
      // This record is not a generic requirement.
      return llvm::Error::success();
    }

    lastRecordOffset.reset();
  }
  return llvm::Error::success();
}

GenericSignature ModuleFile::getGenericSignature(
    serialization::GenericSignatureID ID) {
  auto signature = getGenericSignatureChecked(ID);
  if (!signature)
    fatal(signature.takeError());
  return signature.get();
}

Expected<GenericSignature>
ModuleFile::getGenericSignatureChecked(serialization::GenericSignatureID ID) {
  using namespace decls_block;

  // Zero is a sentinel for having no generic signature.
  if (ID == 0) return nullptr;

  assert(ID <= GenericSignatures.size() &&
         "invalid GenericSignature ID");
  auto &sigOffset = GenericSignatures[ID-1];

  // If we've already deserialized this generic signature, return it.
  if (sigOffset.isComplete())
    return sigOffset.get();

  // Read the generic signature.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(sigOffset));

  // Read the parameter types.
  SmallVector<GenericTypeParamType *, 4> paramTypes;
  StringRef blobData;
  SmallVector<uint64_t, 8> scratch;

  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  switch (recordID) {
  case GENERIC_SIGNATURE: {
    ArrayRef<uint64_t> rawParamIDs;
    GenericSignatureLayout::readRecord(scratch, rawParamIDs);

    for (unsigned i = 0, n = rawParamIDs.size(); i != n; ++i) {
      auto paramTy = getType(rawParamIDs[i])->castTo<GenericTypeParamType>();
      paramTypes.push_back(paramTy);
    }
    break;
  }

  case SIL_GENERIC_SIGNATURE: {
    ArrayRef<uint64_t> rawParamIDs;
    SILGenericSignatureLayout::readRecord(scratch, rawParamIDs);

    if (rawParamIDs.size() % 2 != 0)
      fatal();

    for (unsigned i = 0, n = rawParamIDs.size(); i != n; i += 2) {
      Identifier name = getIdentifier(rawParamIDs[i]);
      auto paramTy = getType(rawParamIDs[i+1])->castTo<GenericTypeParamType>();

      if (!name.empty()) {
        auto paramDecl =
          createDecl<GenericTypeParamDecl>(getAssociatedModule(),
                                           name,
                                           SourceLoc(),
                                           paramTy->getDepth(),
                                           paramTy->getIndex());
        paramTy = paramDecl->getDeclaredInterfaceType()
                   ->castTo<GenericTypeParamType>();
      }

      paramTypes.push_back(paramTy);
    }
    break;
  }
  default:
    // Not a generic signature; no way to recover.
    fatal();
  }

  // Read the generic requirements.
  SmallVector<Requirement, 4> requirements;
  auto error = readGenericRequirementsChecked(requirements, DeclTypeCursor);
  if (error)
    return std::move(error);

  // If we've already deserialized this generic signature, start over to return
  // it directly.
  // FIXME: Is this kind of re-entrancy actually possible?
  if (sigOffset.isComplete())
    return getGenericSignature(ID);

  // Construct the generic signature from the loaded parameters and
  // requirements.
  auto signature = GenericSignature::get(paramTypes, requirements);
  sigOffset = signature;
  return signature;
}

SubstitutionMap ModuleFile::getSubstitutionMap(
                                        serialization::SubstitutionMapID id) {
  auto map = getSubstitutionMapChecked(id);
  if (!map)
    fatal(map.takeError());
  return map.get();
}

Expected<SubstitutionMap>
ModuleFile::getSubstitutionMapChecked(serialization::SubstitutionMapID id) {
  using namespace decls_block;

  // Zero is a sentinel for having an empty substitution map.
  if (id == 0) return SubstitutionMap();

  assert(id <= SubstitutionMaps.size() && "invalid SubstitutionMap ID");
  auto &substitutionsOrOffset = SubstitutionMaps[id-1];

  // If we've already deserialized this substitution map, return it.
  if (substitutionsOrOffset.isComplete()) {
    return substitutionsOrOffset.get();
  }

  // Read the substitution map.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(substitutionsOrOffset));

  // Read the substitution map.
  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  StringRef blobData;
  SmallVector<uint64_t, 8> scratch;
  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  if (recordID != SUBSTITUTION_MAP)
    fatal();

  GenericSignatureID genericSigID;
  uint64_t numConformances;
  ArrayRef<uint64_t> replacementTypeIDs;
  SubstitutionMapLayout::readRecord(scratch, genericSigID, numConformances,
                                    replacementTypeIDs);

  // Generic signature.
  auto genericSigOrError = getGenericSignatureChecked(genericSigID);
  if (!genericSigOrError)
    return genericSigOrError.takeError();

  auto genericSig = genericSigOrError.get();
  if (!genericSig)
    fatal();

  // Load the replacement types.
  SmallVector<Type, 4> replacementTypes;
  replacementTypes.reserve(replacementTypeIDs.size());
  for (auto typeID : replacementTypeIDs) {
    replacementTypes.push_back(getType(typeID));
  }

  // Read the conformances.
  SmallVector<ProtocolConformanceRef, 4> conformances;
  conformances.reserve(numConformances);
  for (unsigned i : range(numConformances)) {
    (void)i;
    auto conformanceOrError = readConformanceChecked(DeclTypeCursor);
    if (!conformanceOrError)
      return conformanceOrError.takeError();
    conformances.push_back(conformanceOrError.get());
  }

  // Form the substitution map and record it.
  auto substitutions =
    SubstitutionMap::get(genericSig, ArrayRef<Type>(replacementTypes),
                         ArrayRef<ProtocolConformanceRef>(conformances));
  substitutionsOrOffset = substitutions;
  return substitutions;
}

bool ModuleFile::readDefaultWitnessTable(ProtocolDecl *proto) {
  using namespace decls_block;

  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return true;

  SmallVector<uint64_t, 16> witnessIDBuffer;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, witnessIDBuffer));
  assert(kind == DEFAULT_WITNESS_TABLE);
  (void)kind;

  ArrayRef<uint64_t> rawWitnessIDs;
  decls_block::DefaultWitnessTableLayout::readRecord(
      witnessIDBuffer, rawWitnessIDs);

  if (rawWitnessIDs.empty())
    return false;

  unsigned e = rawWitnessIDs.size();
  assert(e % 2 == 0 && "malformed default witness table");
  (void) e;

  for (unsigned i = 0, e = rawWitnessIDs.size(); i < e; i += 2) {
    ValueDecl *requirement = cast<ValueDecl>(getDecl(rawWitnessIDs[i]));
    assert(requirement && "unable to deserialize next requirement");
    ValueDecl *witness = cast<ValueDecl>(getDecl(rawWitnessIDs[i + 1]));
    assert(witness && "unable to deserialize next witness");
    assert(requirement->getDeclContext() == proto);

    proto->setDefaultWitness(requirement, witness);
  }

  return false;
}

static Optional<swift::CtorInitializerKind>
getActualCtorInitializerKind(uint8_t raw) {
  switch (serialization::CtorInitializerKind(raw)) {
#define CASE(NAME) \
  case serialization::CtorInitializerKind::NAME: \
    return swift::CtorInitializerKind::NAME;
  CASE(Designated)
  CASE(Convenience)
  CASE(Factory)
  CASE(ConvenienceFactory)
#undef CASE
  }
  return None;
}

static bool isReExportedToModule(const ValueDecl *value,
                                 const ModuleDecl *expectedModule) {
  const DeclContext *valueDC = value->getDeclContext();
  auto fromClangModule
      = dyn_cast<ClangModuleUnit>(valueDC->getModuleScopeContext());
  if (!fromClangModule)
    return false;
  StringRef exportedName = fromClangModule->getExportedModuleName();

  auto toClangModule
      = dyn_cast<ClangModuleUnit>(expectedModule->getFiles().front());
  if (toClangModule)
    return exportedName == toClangModule->getExportedModuleName();
  return exportedName == expectedModule->getName().str();
}

/// Remove values from \p values that don't match the expected type or module.
///
/// Any of \p expectedTy, \p expectedModule, or \p expectedGenericSig can be
/// omitted, in which case any type or module is accepted. Values imported
/// from Clang can also appear in any module.
static void filterValues(Type expectedTy, ModuleDecl *expectedModule,
                         CanGenericSignature expectedGenericSig, bool isType,
                         bool inProtocolExt, bool importedFromClang,
                         bool isStatic,
                         Optional<swift::CtorInitializerKind> ctorInit,
                         SmallVectorImpl<ValueDecl *> &values) {
  CanType canTy;
  if (expectedTy)
    canTy = expectedTy->getCanonicalType();

  auto newEnd = std::remove_if(values.begin(), values.end(),
                               [=](ValueDecl *value) {
    // Ignore anything that was parsed (vs. deserialized), because a serialized
    // module cannot refer to it.
    if (value->getDeclContext()->getParentSourceFile())
      return true;

    if (isType != isa<TypeDecl>(value))
      return true;

    // If we're expecting a type, make sure this decl has the expected type.
    if (canTy && !value->getInterfaceType()->isEqual(canTy))
      return true;

    if (value->isStatic() != isStatic)
      return true;
    if (value->hasClangNode() != importedFromClang)
      return true;

    if (value->getAttrs().hasAttribute<ForbidSerializingReferenceAttr>())
      return true;

    // FIXME: Should be able to move a value from an extension in a derived
    // module to the original definition in a base module.
    if (expectedModule && !value->hasClangNode() &&
        value->getModuleContext() != expectedModule &&
        !isReExportedToModule(value, expectedModule))
      return true;

    // If we're expecting a member within a constrained extension with a
    // particular generic signature, match that signature.
    if (expectedGenericSig &&
        value->getDeclContext()
                ->getGenericSignatureOfContext()
                .getCanonicalSignature() != expectedGenericSig)
      return true;

    // If we don't expect a specific generic signature, ignore anything from a
    // constrained extension.
    if (!expectedGenericSig &&
        isa<ExtensionDecl>(value->getDeclContext()) &&
        cast<ExtensionDecl>(value->getDeclContext())->isConstrainedExtension())
      return true;

    // If we're looking at members of a protocol or protocol extension,
    // filter by whether we expect to find something in a protocol extension or
    // not. This lets us distinguish between a protocol member and a protocol
    // extension member that have the same type.
    if (value->getDeclContext()->getSelfProtocolDecl() &&
        (bool)value->getDeclContext()->getExtendedProtocolDecl()
          != inProtocolExt)
      return true;

    // If we're expecting an initializer with a specific kind, and this is not
    // an initializer with that kind, remove it.
    if (ctorInit) {
      if (!isa<ConstructorDecl>(value) ||
          cast<ConstructorDecl>(value)->getInitKind() != *ctorInit)
        return true;
    }
    return false;
  });
  values.erase(newEnd, values.end());
}

static TypeDecl *
findNestedTypeDeclInModule(FileUnit *thisFile, ModuleDecl *extensionModule,
                           Identifier name, NominalTypeDecl *parent)  {
  assert(extensionModule && "NULL is not a valid module");
  for (FileUnit *file : extensionModule->getFiles()) {
    if (file == thisFile)
      continue;

    if (auto nestedType = file->lookupNestedType(name, parent)) {
      return nestedType;
    }
  }
  return nullptr;
}

Expected<Decl *>
ModuleFile::resolveCrossReference(ModuleID MID, uint32_t pathLen) {
  using namespace decls_block;

  ModuleDecl *baseModule = getModule(MID);
  if (!baseModule) {
    return llvm::make_error<XRefNonLoadedModuleError>(getIdentifier(MID));
  }

  assert(baseModule && "missing dependency");
  PrettyXRefTrace pathTrace(*baseModule);

  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  SmallVector<ValueDecl *, 8> values;
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  // Read the first path piece. This one is special because lookup is performed
  // against the base module, rather than against the previous link in the path.
  // In particular, operator path pieces represent actual operators here, but
  // filters on operator functions when they appear later on.
  scratch.clear();
  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  switch (recordID) {
  case XREF_TYPE_PATH_PIECE:
  case XREF_VALUE_PATH_PIECE: {
    IdentifierID IID;
    IdentifierID privateDiscriminator = 0;
    TypeID TID = 0;
    bool isType = (recordID == XREF_TYPE_PATH_PIECE);
    bool inProtocolExt = false;
    bool importedFromClang = false;
    bool isStatic = false;
    if (isType)
      XRefTypePathPieceLayout::readRecord(scratch, IID, privateDiscriminator,
                                          inProtocolExt, importedFromClang);
    else
      XRefValuePathPieceLayout::readRecord(scratch, TID, IID, inProtocolExt,
                                           importedFromClang, isStatic);

    DeclBaseName name = getDeclBaseName(IID);
    pathTrace.addValue(name);
    if (privateDiscriminator)
      pathTrace.addValue(getIdentifier(privateDiscriminator));

    Type filterTy;
    if (!isType) {
      auto maybeType = getTypeChecked(TID);
      if (!maybeType) {
        // FIXME: Don't throw away the inner error's information.
        consumeError(maybeType.takeError());
        return llvm::make_error<XRefError>("couldn't decode type",
                                           pathTrace, name);
      }
      filterTy = maybeType.get();
      pathTrace.addType(filterTy);
    }

    if (privateDiscriminator) {
      baseModule->lookupMember(values, baseModule, name,
                               getIdentifier(privateDiscriminator));
    } else {
      baseModule->lookupQualified(baseModule, DeclNameRef(name),
                                  NL_QualifiedDefault,
                                  values);
    }
    filterValues(filterTy, nullptr, nullptr, isType, inProtocolExt,
                 importedFromClang, isStatic, None, values);
    break;
  }
      
  case XREF_OPAQUE_RETURN_TYPE_PATH_PIECE: {
    IdentifierID DefiningDeclNameID;
    
    XRefOpaqueReturnTypePathPieceLayout::readRecord(scratch, DefiningDeclNameID);
    
    auto name = getIdentifier(DefiningDeclNameID);
    pathTrace.addOpaqueReturnType(name);
    
    if (auto opaque = baseModule->lookupOpaqueResultType(name.str())) {
      values.push_back(opaque);
    }
    break;
  }

  case XREF_EXTENSION_PATH_PIECE:
    llvm_unreachable("can only extend a nominal");

  case XREF_OPERATOR_OR_ACCESSOR_PATH_PIECE: {
    IdentifierID IID;
    uint8_t rawOpKind;
    XRefOperatorOrAccessorPathPieceLayout::readRecord(scratch, IID, rawOpKind);

    Identifier opName = getIdentifier(IID);
    pathTrace.addOperator(opName);

    auto &ctx = getContext();
    auto desc = OperatorLookupDescriptor::forModule(baseModule, opName);
    switch (rawOpKind) {
    case OperatorKind::Infix:
    case OperatorKind::Prefix:
    case OperatorKind::Postfix: {
      auto req = DirectOperatorLookupRequest{
          desc, getASTOperatorFixity(static_cast<OperatorKind>(rawOpKind))};
      auto results = evaluateOrDefault(ctx.evaluator, req, {});
      if (results.size() != 1) {
        return llvm::make_error<XRefError>("operator not found", pathTrace,
                                           opName);
      }
      return results[0];
    }
    case OperatorKind::PrecedenceGroup: {
      auto results = evaluateOrDefault(
          ctx.evaluator, DirectPrecedenceGroupLookupRequest{desc}, {});
      if (results.size() != 1) {
        return llvm::make_error<XRefError>("precedencegroup not found",
                                           pathTrace, opName);
      }
      return results[0];
    }
    default:
      // Unknown operator kind.
      fatal();
    }
    llvm_unreachable("Unhandled case in switch!");
  }

  case XREF_GENERIC_PARAM_PATH_PIECE:
  case XREF_INITIALIZER_PATH_PIECE:
    llvm_unreachable("only in a nominal or function");

  default:
    // Unknown xref kind.
    pathTrace.addUnknown(recordID);
    fatal();
  }

  auto getXRefDeclNameForError = [&]() -> DeclName {
    DeclName result = pathTrace.getLastName();
    while (--pathLen) {
      llvm::BitstreamEntry entry =
          fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        return Identifier();

      scratch.clear();
      unsigned recordID = fatalIfUnexpected(
          DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
      switch (recordID) {
      case XREF_TYPE_PATH_PIECE: {
        IdentifierID IID;
        XRefTypePathPieceLayout::readRecord(scratch, IID, None, None, None);
        result = getIdentifier(IID);
        break;
      }
      case XREF_VALUE_PATH_PIECE: {
        IdentifierID IID;
        XRefValuePathPieceLayout::readRecord(scratch, None, IID, None, None,
                                             None);
        result = getIdentifier(IID);
        break;
      }
      case XREF_OPAQUE_RETURN_TYPE_PATH_PIECE: {
        IdentifierID IID;
        XRefOpaqueReturnTypePathPieceLayout::readRecord(scratch, IID);
        auto mangledName = getIdentifier(IID);
        
        SmallString<64> buf;
        {
          llvm::raw_svector_ostream os(buf);
          os << "<<opaque return type of ";
          os << mangledName.str();
          os << ">>";
        }
        
        result = getContext().getIdentifier(buf);
        break;
      }
      case XREF_INITIALIZER_PATH_PIECE:
        result = DeclBaseName::createConstructor();
        break;

      case XREF_EXTENSION_PATH_PIECE:
      case XREF_OPERATOR_OR_ACCESSOR_PATH_PIECE:
        break;

      case XREF_GENERIC_PARAM_PATH_PIECE:
        // Can't get the name without deserializing.
        result = Identifier();
        break;

      default:
        // Unknown encoding.
        return Identifier();
      }
    }
    return result;
  };

  if (values.empty()) {
    return llvm::make_error<XRefError>("top-level value not found", pathTrace,
                                       getXRefDeclNameForError());
  }

  // Filters for values discovered in the remaining path pieces.
  ModuleDecl *M = nullptr;
  CanGenericSignature genericSig = CanGenericSignature();

  // For remaining path pieces, filter or drill down into the results we have.
  while (--pathLen) {
    llvm::BitstreamEntry entry =
        fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
    if (entry.Kind != llvm::BitstreamEntry::Record)
      fatal();

    scratch.clear();
    unsigned recordID = fatalIfUnexpected(
        DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
    switch (recordID) {
    case XREF_TYPE_PATH_PIECE: {
      if (values.size() == 1 && isa<NominalTypeDecl>(values.front())) {
        // Fast path for nested types that avoids deserializing all
        // members of the parent type.
        IdentifierID IID;
        IdentifierID privateDiscriminator;
        bool importedFromClang = false;
        bool inProtocolExt = false;
        XRefTypePathPieceLayout::readRecord(scratch, IID, privateDiscriminator,
                                            inProtocolExt, importedFromClang);
        if (privateDiscriminator)
          goto giveUpFastPath;

        Identifier memberName = getIdentifier(IID);
        pathTrace.addValue(memberName);

        auto *baseType = cast<NominalTypeDecl>(values.front());
        ModuleDecl *extensionModule = M;
        if (!extensionModule)
          extensionModule = baseType->getModuleContext();

        // Fault in extensions, then ask every file in the module.
        (void)baseType->getExtensions();
        auto *nestedType =
            findNestedTypeDeclInModule(getFile(), extensionModule,
                                       memberName, baseType);

        // For clang module units, also search tables in the overlays.
        if (!nestedType) {
          if (auto LF =
                  dyn_cast<LoadedFile>(baseType->getModuleScopeContext())) {
            if (auto overlayModule = LF->getOverlayModule()) {
              nestedType = findNestedTypeDeclInModule(getFile(), overlayModule,
                                                      memberName, baseType);
            } else if (LF->getParentModule() != extensionModule) {
              nestedType = findNestedTypeDeclInModule(getFile(),
                                                      LF->getParentModule(),
                                                      memberName, baseType);
            }
          }
        }

        if (nestedType) {
          SmallVector<ValueDecl *, 1> singleValueBuffer{nestedType};
          filterValues(/*expectedTy*/Type(), extensionModule, genericSig,
                       /*isType*/true, inProtocolExt, importedFromClang,
                       /*isStatic*/false, /*ctorInit*/None, singleValueBuffer);
          if (!singleValueBuffer.empty()) {
            values.assign({nestedType});
            ++NumNestedTypeShortcuts;
            break;
          }
        }

        pathTrace.removeLast();
      }
giveUpFastPath:
      LLVM_FALLTHROUGH;
    }
    case XREF_VALUE_PATH_PIECE:
    case XREF_INITIALIZER_PATH_PIECE: {
      TypeID TID = 0;
      DeclBaseName memberName;
      Identifier privateDiscriminator;
      Optional<swift::CtorInitializerKind> ctorInit;
      bool isType = false;
      bool inProtocolExt = false;
      bool importedFromClang = false;
      bool isStatic = false;
      switch (recordID) {
      case XREF_TYPE_PATH_PIECE: {
        IdentifierID IID, discriminatorID;
        XRefTypePathPieceLayout::readRecord(scratch, IID, discriminatorID,
                                            inProtocolExt, importedFromClang);
        memberName = getDeclBaseName(IID);
        privateDiscriminator = getIdentifier(discriminatorID);
        isType = true;
        break;
      }

      case XREF_VALUE_PATH_PIECE: {
        IdentifierID IID;
        XRefValuePathPieceLayout::readRecord(scratch, TID, IID, inProtocolExt,
                                             importedFromClang, isStatic);
        memberName = getDeclBaseName(IID);
        break;
      }

      case XREF_INITIALIZER_PATH_PIECE: {
        uint8_t kind;
        XRefInitializerPathPieceLayout::readRecord(scratch, TID, inProtocolExt,
                                                   importedFromClang, kind);
        memberName = DeclBaseName::createConstructor();
        ctorInit = getActualCtorInitializerKind(kind);
        break;
      }
        
      default:
        llvm_unreachable("Unhandled path piece");
      }

      pathTrace.addValue(memberName);
      if (!privateDiscriminator.empty())
        pathTrace.addPrivateDiscriminator(privateDiscriminator);

      Type filterTy;
      if (!isType) {
        auto maybeType = getTypeChecked(TID);
        if (!maybeType) {
          // FIXME: Don't throw away the inner error's information.
          consumeError(maybeType.takeError());
          return llvm::make_error<XRefError>("couldn't decode type",
                                             pathTrace, memberName);
        }
        filterTy = maybeType.get();
        pathTrace.addType(filterTy);
      }

      if (values.size() != 1) {
        return llvm::make_error<XRefError>("multiple matching base values",
                                           pathTrace,
                                           getXRefDeclNameForError());
      }

      auto nominal = dyn_cast<NominalTypeDecl>(values.front());
      values.clear();

      if (!nominal) {
        return llvm::make_error<XRefError>("base is not a nominal type",
                                           pathTrace,
                                           getXRefDeclNameForError());
      }

      if (memberName.getKind() == DeclBaseName::Kind::Destructor) {
        assert(isa<ClassDecl>(nominal));
        // Force creation of an implicit destructor
        auto CD = dyn_cast<ClassDecl>(nominal);
        values.push_back(CD->getDestructor());
        break;
      }

      if (!privateDiscriminator.empty()) {
        ModuleDecl *searchModule = M;
        if (!searchModule)
          searchModule = nominal->getModuleContext();
        searchModule->lookupMember(values, nominal, memberName,
                                   privateDiscriminator);

      } else {
        auto members = nominal->lookupDirect(memberName);
        values.append(members.begin(), members.end());
      }
      filterValues(filterTy, M, genericSig, isType, inProtocolExt,
                   importedFromClang, isStatic, ctorInit, values);
      break;
    }

    case XREF_EXTENSION_PATH_PIECE: {
      ModuleID ownerID;
      GenericSignatureID rawGenericSig;
      XRefExtensionPathPieceLayout::readRecord(scratch, ownerID, rawGenericSig);
      M = getModule(ownerID);
      if (!M) {
        return llvm::make_error<XRefError>("module is not loaded",
                                           pathTrace, getIdentifier(ownerID));
      }
      pathTrace.addExtension(M);

      // Read the generic signature, if we have one.
      genericSig = CanGenericSignature(getGenericSignature(rawGenericSig));

      continue;
    }

    case XREF_OPERATOR_OR_ACCESSOR_PATH_PIECE: {
      uint8_t rawKind;
      XRefOperatorOrAccessorPathPieceLayout::readRecord(scratch, None,
                                                        rawKind);
      if (values.empty())
        break;

      if (!values.front()->getBaseName().isOperator()) {
        pathTrace.addAccessor(rawKind);
        if (auto storage = dyn_cast<AbstractStorageDecl>(values.front())) {
          auto actualKind = getActualAccessorKind(rawKind);
          if (!actualKind) {
            // Unknown accessor kind.
            fatal();
          }
          values.front() = storage->getAccessor(*actualKind);
          if (!values.front()) {
            return llvm::make_error<XRefError>("missing accessor",
                                               pathTrace,
                                               getXRefDeclNameForError());

          }
        }
        break;
      }

      pathTrace.addOperatorFilter(rawKind);

      auto newEnd = std::remove_if(values.begin(), values.end(),
                                   [=](ValueDecl *value) {
        auto fn = dyn_cast<FuncDecl>(value);
        if (!fn)
          return true;
        if (!fn->getOperatorDecl())
          return true;
        if (getStableFixity(fn->getOperatorDecl()->getFixity()) != rawKind)
          return true;
        return false;
      });
      values.erase(newEnd, values.end());
      break;
    }

    case XREF_GENERIC_PARAM_PATH_PIECE: {
      if (values.size() != 1) {
        return llvm::make_error<XRefError>("multiple matching base values",
                                           pathTrace,
                                           getXRefDeclNameForError());
      }

      uint32_t depth, paramIndex;
      XRefGenericParamPathPieceLayout::readRecord(scratch, depth, paramIndex);

      pathTrace.addGenericParam(paramIndex);

      ValueDecl *base = values.front();

      GenericSignature currentSig;
      if (auto nominal = dyn_cast<NominalTypeDecl>(base)) {
        if (genericSig) {
          // Find an extension in the requested module that has the
          // correct generic signature.
          for (auto ext : nominal->getExtensions()) {
            if (ext->getModuleContext() == M &&
                ext->getGenericSignature().getCanonicalSignature() ==
                    genericSig) {
              currentSig = ext->getGenericSignature();
              break;
            }
          }
          assert(currentSig && "Couldn't find constrained extension");
        } else {
          // Simple case: use the nominal type's generic parameters.
          currentSig = nominal->getGenericSignature();
        }
      } else if (auto alias = dyn_cast<TypeAliasDecl>(base)) {
        currentSig = alias->getGenericSignature();
      } else if (auto fn = dyn_cast<AbstractFunctionDecl>(base)) {
        currentSig = fn->getGenericSignature();
      } else if (auto subscript = dyn_cast<SubscriptDecl>(base)) {
        currentSig = subscript->getGenericSignature();
      } else if (auto opaque = dyn_cast<OpaqueTypeDecl>(base)) {
        currentSig = opaque->getGenericSignature();
      }

      if (!currentSig) {
        return llvm::make_error<XRefError>(
            "cross-reference to generic param for non-generic type",
            pathTrace, getXRefDeclNameForError());
      }

      bool found = false;
      for (auto paramTy : currentSig->getGenericParams()) {
        if (paramTy->getIndex() == paramIndex &&
            paramTy->getDepth() == depth) {
          values.clear();
          values.push_back(paramTy->getDecl());
          found = true;
          break;
        }
      }

      if (!found) {
        return llvm::make_error<XRefError>(
            "invalid generic argument index or depth",
            pathTrace, getXRefDeclNameForError());
      }

      break;
    }
        
    case XREF_OPAQUE_RETURN_TYPE_PATH_PIECE: {
      values.clear();
      IdentifierID DefiningDeclNameID;
      
      XRefOpaqueReturnTypePathPieceLayout::readRecord(scratch, DefiningDeclNameID);
      
      auto name = getIdentifier(DefiningDeclNameID);
      pathTrace.addOpaqueReturnType(name);
    
      auto lookupModule = M ? M : baseModule;
      if (auto opaqueTy = lookupModule->lookupOpaqueResultType(name.str())) {
        values.push_back(opaqueTy);
      }
      break;
    }

    default:
      // Unknown xref path piece.
      pathTrace.addUnknown(recordID);
      fatal();
    }

    Optional<PrettyStackTraceModuleFile> traceMsg;
    if (M != getAssociatedModule()) {
      traceMsg.emplace("If you're seeing a crash here, check that your SDK "
                         "and dependencies match the versions used to build",
                       *this);
    }

    if (values.empty()) {
      return llvm::make_error<XRefError>("result not found", pathTrace,
                                         getXRefDeclNameForError());
    }

    // Reset the module filter.
    M = nullptr;
    genericSig = nullptr;
  }

  // Make sure we /used/ the last module filter we got.
  // This catches the case where the last path piece we saw was an Extension
  // path piece, which is not a valid way to end a path. (Cross-references to
  // extensions are not allowed because they cannot be uniquely named.)
  if (M)
    fatal();

  // When all is said and done, we should have a single value here to return.
  if (values.size() != 1) {
    return llvm::make_error<XRefError>("result is ambiguous", pathTrace,
                                       getXRefDeclNameForError());
  }

  assert(values.front() != nullptr);
  return values.front();
}

DeclBaseName ModuleFile::getDeclBaseName(IdentifierID IID) {
  if (IID == 0)
    return Identifier();

  if (IID < NUM_SPECIAL_IDS) {
    switch (static_cast<SpecialIdentifierID>(static_cast<uint8_t>(IID))) {
    case BUILTIN_MODULE_ID:
    case CURRENT_MODULE_ID:
    case OBJC_HEADER_MODULE_ID:
        llvm_unreachable("Cannot get DeclBaseName of special module id");
    case SUBSCRIPT_ID:
      return DeclBaseName::createSubscript();
    case serialization::CONSTRUCTOR_ID:
      return DeclBaseName::createConstructor();
    case serialization::DESTRUCTOR_ID:
      return DeclBaseName::createDestructor();
    case NUM_SPECIAL_IDS:
      llvm_unreachable("implementation detail only");
    }
  }

  size_t rawID = IID - NUM_SPECIAL_IDS;
  assert(rawID < Identifiers.size() && "invalid identifier ID");
  auto &identRecord = Identifiers[rawID];

  if (identRecord.Ident.empty()) {
    StringRef text = getIdentifierText(IID);
    identRecord.Ident = getContext().getIdentifier(text);
  }
  return identRecord.Ident;
}

Identifier ModuleFile::getIdentifier(IdentifierID IID) {
  auto name = getDeclBaseName(IID);
  assert(!name.isSpecial());
  return name.getIdentifier();
}

StringRef ModuleFile::getIdentifierText(IdentifierID IID) {
  if (IID == 0)
    return StringRef();

  assert(IID >= NUM_SPECIAL_IDS);

  size_t rawID = IID - NUM_SPECIAL_IDS;
  assert(rawID < Identifiers.size() && "invalid identifier ID");
  auto identRecord = Identifiers[rawID];

  if (!identRecord.Ident.empty())
    return identRecord.Ident.str();

  return Core->getIdentifierText(IID);
}

DeclContext *ModuleFile::getLocalDeclContext(LocalDeclContextID DCID) {
  assert(DCID != 0 && "invalid local DeclContext ID 0");
  auto &declContextOrOffset = LocalDeclContexts[DCID-1];

  if (declContextOrOffset.isComplete())
    return declContextOrOffset;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(declContextOrOffset));
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  ASTContext &ctx = getContext();
  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;

  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  switch(recordID) {
  case decls_block::ABSTRACT_CLOSURE_EXPR_CONTEXT: {
    TypeID closureTypeID;
    unsigned discriminator = 0;
    bool implicit = false;
    DeclContextID parentID;

    decls_block::AbstractClosureExprLayout::readRecord(scratch,
                                                       closureTypeID,
                                                       implicit,
                                                       discriminator,
                                                       parentID);
    DeclContext *parent = getDeclContext(parentID);
    auto type = getType(closureTypeID);

    declContextOrOffset = new (ctx)
      SerializedAbstractClosureExpr(type, implicit, discriminator, parent);
    break;
  }

  case decls_block::TOP_LEVEL_CODE_DECL_CONTEXT: {
    DeclContextID parentID;
    decls_block::TopLevelCodeDeclContextLayout::readRecord(scratch,
                                                           parentID);
    DeclContext *parent = getDeclContext(parentID);

    declContextOrOffset = new (ctx) SerializedTopLevelCodeDeclContext(parent);
    break;
  }

  case decls_block::PATTERN_BINDING_INITIALIZER_CONTEXT: {
    DeclID bindingID;
    uint32_t bindingIndex;
    decls_block::PatternBindingInitializerLayout::readRecord(scratch,
                                                             bindingID,
                                                             bindingIndex);
    auto decl = getDecl(bindingID);
    PatternBindingDecl *binding = cast<PatternBindingDecl>(decl);

    if (!declContextOrOffset.isComplete())
      declContextOrOffset = new (ctx)
        SerializedPatternBindingInitializer(binding, bindingIndex);

    if (!blobData.empty())
      binding->setInitStringRepresentation(bindingIndex, blobData);
    break;
  }

  case decls_block::DEFAULT_ARGUMENT_INITIALIZER_CONTEXT: {
    DeclContextID parentID;
    unsigned index = 0;
    decls_block::DefaultArgumentInitializerLayout::readRecord(scratch,
                                                              parentID,
                                                              index);
    DeclContext *parent = getDeclContext(parentID);

    declContextOrOffset = new (ctx)
      SerializedDefaultArgumentInitializer(index, parent);
    break;
  }

  default:
    llvm_unreachable("Unknown record ID found when reading local DeclContext.");
  }
  return declContextOrOffset;
}

DeclContext *ModuleFile::getDeclContext(DeclContextID DCID) {
  auto deserialized = getDeclContextChecked(DCID);
  if (!deserialized) {
    fatal(deserialized.takeError());
  }
  return deserialized.get();
}

Expected<DeclContext *> ModuleFile::getDeclContextChecked(DeclContextID DCID) {
  if (!DCID)
    return FileContext;

  if (Optional<LocalDeclContextID> contextID = DCID.getAsLocalDeclContextID())
    return getLocalDeclContext(contextID.getValue());

  auto deserialized = getDeclChecked(DCID.getAsDeclID().getValue());
  if (!deserialized)
    return deserialized.takeError();

  auto D = deserialized.get();
  if (auto GTD = dyn_cast<GenericTypeDecl>(D))
    return GTD;
  if (auto ED = dyn_cast<ExtensionDecl>(D))
    return ED;
  if (auto AFD = dyn_cast<AbstractFunctionDecl>(D))
    return AFD;
  if (auto SD = dyn_cast<SubscriptDecl>(D))
    return SD;
  if (auto EED = dyn_cast<EnumElementDecl>(D))
    return EED;

  llvm_unreachable("Unknown Decl : DeclContext kind");
}

ModuleDecl *ModuleFile::getModule(ModuleID MID) {
  if (MID < NUM_SPECIAL_IDS) {
    switch (static_cast<SpecialIdentifierID>(static_cast<uint8_t>(MID))) {
    case BUILTIN_MODULE_ID:
      return getContext().TheBuiltinModule;
    case CURRENT_MODULE_ID:
      return FileContext->getParentModule();
    case OBJC_HEADER_MODULE_ID: {
      auto clangImporter =
        static_cast<ClangImporter *>(getContext().getClangModuleLoader());
      return clangImporter->getImportedHeaderModule();
    }
    case SUBSCRIPT_ID:
    case CONSTRUCTOR_ID:
    case DESTRUCTOR_ID:
      llvm_unreachable("Modules cannot be named with special names");
    case NUM_SPECIAL_IDS:
      llvm_unreachable("implementation detail only");
    }
  }
  return getModule(ImportPath::Module::Builder(getIdentifier(MID)).get(),
                   getContext().LangOpts.AllowDeserializingImplementationOnly);
}

ModuleDecl *ModuleFile::getModule(ImportPath::Module name,
                                  bool allowLoading) {
  if (name.empty() || name.front().Item.empty())
    return getContext().TheBuiltinModule;

  // FIXME: duplicated from ImportResolver::getModule
  if (name.size() == 1 &&
      name.front().Item == FileContext->getParentModule()->getName()) {
    if (!UnderlyingModule && allowLoading) {
      auto importer = getContext().getClangModuleLoader();
      assert(importer && "no way to import shadowed module");
      UnderlyingModule =
          importer->loadModule(SourceLoc(), name.getTopLevelPath());
    }

    return UnderlyingModule;
  }

  if (allowLoading)
    return getContext().getModule(name);
  return getContext().getLoadedModule(name);
}


/// Translate from the Serialization associativity enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::Associativity> getActualAssociativity(uint8_t assoc) {
  switch (assoc) {
  case serialization::Associativity::LeftAssociative:
    return swift::Associativity::Left;
  case serialization::Associativity::RightAssociative:
    return swift::Associativity::Right;
  case serialization::Associativity::NonAssociative:
    return swift::Associativity::None;
  default:
    return None;
  }
}

static Optional<swift::StaticSpellingKind>
getActualStaticSpellingKind(uint8_t raw) {
  switch (serialization::StaticSpellingKind(raw)) {
  case serialization::StaticSpellingKind::None:
    return swift::StaticSpellingKind::None;
  case serialization::StaticSpellingKind::KeywordStatic:
    return swift::StaticSpellingKind::KeywordStatic;
  case serialization::StaticSpellingKind::KeywordClass:
    return swift::StaticSpellingKind::KeywordClass;
  }
  return None;
}

static bool isDeclAttrRecord(unsigned ID) {
  using namespace decls_block;
  switch (ID) {
#define DECL_ATTR(NAME, CLASS, ...) case CLASS##_DECL_ATTR: return true;
#include "DeclTypeRecordNodes.def"
  default: return false;
  }
}

static Optional<swift::AccessLevel> getActualAccessLevel(uint8_t raw) {
  switch (serialization::AccessLevel(raw)) {
#define CASE(NAME) \
  case serialization::AccessLevel::NAME: \
    return swift::AccessLevel::NAME;
  CASE(Private)
  CASE(FilePrivate)
  CASE(Internal)
  CASE(Public)
  CASE(Open)
#undef CASE
  }
  return None;
}

static Optional<swift::SelfAccessKind>
getActualSelfAccessKind(uint8_t raw) {
  switch (serialization::SelfAccessKind(raw)) {
  case serialization::SelfAccessKind::NonMutating:
    return swift::SelfAccessKind::NonMutating;
  case serialization::SelfAccessKind::Mutating:
    return swift::SelfAccessKind::Mutating;
  case serialization::SelfAccessKind::Consuming:
    return swift::SelfAccessKind::Consuming;
  }
  return None;
}

/// Translate from the serialization VarDeclSpecifier enumerators, which are
/// guaranteed to be stable, to the AST ones.
static Optional<swift::ParamDecl::Specifier>
getActualParamDeclSpecifier(serialization::ParamDeclSpecifier raw) {
  switch (raw) {
#define CASE(ID) \
  case serialization::ParamDeclSpecifier::ID: \
    return swift::ParamDecl::Specifier::ID;
  CASE(Default)
  CASE(InOut)
  CASE(Shared)
  CASE(Owned)
  }
#undef CASE
  return None;
}

static Optional<swift::VarDecl::Introducer>
getActualVarDeclIntroducer(serialization::VarDeclIntroducer raw) {
  switch (raw) {
#define CASE(ID) \
  case serialization::VarDeclIntroducer::ID: \
    return swift::VarDecl::Introducer::ID;
  CASE(Let)
  CASE(Var)
  }
#undef CASE
  return None;
}

static Optional<swift::OpaqueReadOwnership>
getActualOpaqueReadOwnership(unsigned rawKind) {
  switch (serialization::OpaqueReadOwnership(rawKind)) {
#define CASE(KIND)                               \
  case serialization::OpaqueReadOwnership::KIND: \
    return swift::OpaqueReadOwnership::KIND;
  CASE(Owned)
  CASE(Borrowed)
  CASE(OwnedOrBorrowed)
#undef CASE
  }
  return None;
}

static Optional<swift::ReadImplKind>
getActualReadImplKind(unsigned rawKind) {
  switch (serialization::ReadImplKind(rawKind)) {
#define CASE(KIND)                        \
  case serialization::ReadImplKind::KIND: \
    return swift::ReadImplKind::KIND;
  CASE(Stored)
  CASE(Get)
  CASE(Inherited)
  CASE(Address)
  CASE(Read)
#undef CASE
  }
  return None;
}

static Optional<swift::WriteImplKind>
getActualWriteImplKind(unsigned rawKind) {
  switch (serialization::WriteImplKind(rawKind)) {
#define CASE(KIND)                         \
  case serialization::WriteImplKind::KIND: \
    return swift::WriteImplKind::KIND;
  CASE(Immutable)
  CASE(Stored)
  CASE(Set)
  CASE(StoredWithObservers)
  CASE(InheritedWithObservers)
  CASE(MutableAddress)
  CASE(Modify)
#undef CASE
  }
  return None;
}

static Optional<swift::ReadWriteImplKind>
getActualReadWriteImplKind(unsigned rawKind) {
  switch (serialization::ReadWriteImplKind(rawKind)) {
#define CASE(KIND)                             \
  case serialization::ReadWriteImplKind::KIND: \
    return swift::ReadWriteImplKind::KIND;
  CASE(Immutable)
  CASE(Stored)
  CASE(MutableAddress)
  CASE(MaterializeToTemporary)
  CASE(Modify)
  CASE(StoredWithDidSet)
  CASE(InheritedWithDidSet)
#undef CASE
  }
  return None;
}

/// Translate from the serialization DifferentiabilityKind enumerators, which
/// are guaranteed to be stable, to the AST ones.
static Optional<swift::AutoDiffDerivativeFunctionKind>
getActualAutoDiffDerivativeFunctionKind(uint8_t raw) {
  switch (serialization::AutoDiffDerivativeFunctionKind(raw)) {
#define CASE(ID)                                                               \
  case serialization::AutoDiffDerivativeFunctionKind::ID:                      \
    return {swift::AutoDiffDerivativeFunctionKind::ID};
  CASE(JVP)
  CASE(VJP)
#undef CASE
  }
  return None;
}

void ModuleFile::configureStorage(AbstractStorageDecl *decl,
                                  uint8_t rawOpaqueReadOwnership,
                                  uint8_t rawReadImplKind,
                                  uint8_t rawWriteImplKind,
                                  uint8_t rawReadWriteImplKind,
                                  AccessorRecord &rawIDs) {
  auto opaqueReadOwnership =
    getActualOpaqueReadOwnership(rawOpaqueReadOwnership);
  if (!opaqueReadOwnership)
    return;
  decl->setOpaqueReadOwnership(*opaqueReadOwnership);

  auto readImpl = getActualReadImplKind(rawReadImplKind);
  if (!readImpl) return;

  auto writeImpl = getActualWriteImplKind(rawWriteImplKind);
  if (!writeImpl) return;

  auto readWriteImpl = getActualReadWriteImplKind(rawReadWriteImplKind);
  if (!readWriteImpl) return;

  SmallVector<AccessorDecl*, 8> accessors;
  for (DeclID id : rawIDs.IDs) {
    auto accessor = dyn_cast_or_null<AccessorDecl>(getDecl(id));
    if (!accessor) return;
    accessors.push_back(accessor);
  }

  auto implInfo = StorageImplInfo(*readImpl, *writeImpl, *readWriteImpl);
  decl->setImplInfo(implInfo);

  if (implInfo.isSimpleStored() && accessors.empty())
    return;

  // We currently don't serialize these locations.
  SourceLoc beginLoc, endLoc;

  decl->setAccessors(beginLoc, accessors, endLoc);
}

template <typename T, typename ...Args>
T *ModuleFile::createDecl(Args &&... args) {
  // Note that this method is not used for all decl kinds.
  static_assert(std::is_base_of<Decl, T>::value, "not a Decl");
  return new (getContext()) T(std::forward<Args>(args)...);
}

static const uint64_t lazyConformanceContextDataPositionMask = 0xFFFFFFFFFFFF;

/// Decode the context data for lazily-loaded conformances.
static std::pair<uint64_t, uint64_t> decodeLazyConformanceContextData(
                                       uint64_t contextData) {
  return std::make_pair(contextData >> 48,
                        contextData & lazyConformanceContextDataPositionMask);
}

/// Encode the context data for lazily-loaded conformances.
static uint64_t encodeLazyConformanceContextData(uint64_t numProtocols,
                                                 uint64_t bitPosition) {
  assert(numProtocols < 0xFFFF);
  assert(bitPosition < lazyConformanceContextDataPositionMask);
  return (numProtocols << 48) | bitPosition;
}

template <typename DERIVED>
static bool attributeChainContains(DeclAttribute *attr) {
  DeclAttributes tempAttrs;
  tempAttrs.setRawAttributeChain(attr);
  static_assert(std::is_trivially_destructible<DeclAttributes>::value,
                "must not try to destroy the attribute chain");
  return tempAttrs.hasAttribute<DERIVED>();
}

// Set original declaration and parameter indices in `@differentiable`
// attributes.
//
// Serializing/deserializing the original declaration DeclID in
// `@differentiable` attributes does not work because it causes
// `@differentiable` attribute deserialization to enter an infinite loop.
//
// Instead, call this ad-hoc function after deserializing a declaration to set
// the original declaration and parameter indices for its `@differentiable`
// attributes.
static void setOriginalDeclarationAndParameterIndicesInDifferentiableAttributes(
    Decl *decl, DeclAttribute *attrs,
    llvm::DenseMap<DifferentiableAttr *, IndexSubset *>
        &diffAttrParamIndicesMap) {
  DeclAttributes tempAttrs;
  tempAttrs.setRawAttributeChain(attrs);
  for (auto *attr : tempAttrs.getAttributes<DifferentiableAttr>()) {
    auto *diffAttr = const_cast<DifferentiableAttr *>(attr);
    diffAttr->setOriginalDeclaration(decl);
    diffAttr->setParameterIndices(diffAttrParamIndicesMap[diffAttr]);
  }
}

Decl *ModuleFile::getDecl(DeclID DID) {
  Expected<Decl *> deserialized = getDeclChecked(DID);
  if (!deserialized) {
    fatal(deserialized.takeError());
  }
  return deserialized.get();
}

/// Used to split up methods that would otherwise live in ModuleFile.
namespace swift {
class DeclDeserializer {
  template <typename T>
  using Serialized = ModuleFile::Serialized<T>;
  using TypeID = serialization::TypeID;

  ModuleFile &MF;
  ASTContext &ctx;
  Serialized<Decl *> &declOrOffset;

  DeclAttribute *DAttrs = nullptr;
  DeclAttribute **AttrsNext = &DAttrs;

  Identifier privateDiscriminator;
  unsigned localDiscriminator = 0;
  StringRef filenameForPrivate;

  // Auxiliary map for deserializing `@differentiable` attributes.
  llvm::DenseMap<DifferentiableAttr *, IndexSubset *> diffAttrParamIndicesMap;

  void AddAttribute(DeclAttribute *Attr) {
    // Advance the linked list.
    // This isn't just using DeclAttributes because that would result in the
    // attributes getting reversed.
    // FIXME: If we reverse them at serialization time we could get rid of this.
    *AttrsNext = Attr;
    AttrsNext = Attr->getMutableNext();
  };

  void handleInherited(llvm::PointerUnion<TypeDecl *, ExtensionDecl *> decl,
                       ArrayRef<uint64_t> rawInheritedIDs) {
    SmallVector<TypeLoc, 2> inheritedTypes;
    for (auto rawID : rawInheritedIDs) {
      auto maybeType = MF.getTypeChecked(rawID);
      if (!maybeType) {
        llvm::consumeError(maybeType.takeError());
        continue;
      }
      inheritedTypes.push_back(TypeLoc::withoutLoc(MF.getType(rawID)));
    }

    auto inherited = ctx.AllocateCopy(inheritedTypes);
    if (auto *typeDecl = decl.dyn_cast<TypeDecl *>())
      typeDecl->setInherited(inherited);
    else
      decl.get<ExtensionDecl *>()->setInherited(inherited);
  }

public:
  DeclDeserializer(ModuleFile &MF, Serialized<Decl *> &declOrOffset)
      : MF(MF), ctx(MF.getContext()), declOrOffset(declOrOffset) {}

  ~DeclDeserializer() {
    if (!declOrOffset.isComplete()) {
      // We failed to deserialize this declaration.
      return;
    }

    Decl *decl = declOrOffset.get();
    if (!decl)
      return;

    if (DAttrs)
      decl->getAttrs().setRawAttributeChain(DAttrs);

    if (auto value = dyn_cast<ValueDecl>(decl)) {
      if (!privateDiscriminator.empty())
        MF.PrivateDiscriminatorsByValue[value] = privateDiscriminator;

      if (localDiscriminator != 0)
        value->setLocalDiscriminator(localDiscriminator);

      if (!filenameForPrivate.empty())
        MF.FilenamesForPrivateValues[value] = filenameForPrivate;
    }
  }

  /// Deserializes decl attribute and attribute-like records from
  /// \c MF.DeclTypesCursor until a non-attribute record is found,
  /// passing each one to AddAttribute.
  llvm::Error deserializeDeclAttributes();

  Expected<Decl *> getDeclCheckedImpl(
    llvm::function_ref<bool(DeclAttributes)> matchAttributes = nullptr);

  Expected<Decl *> deserializeTypeAlias(ArrayRef<uint64_t> scratch,
                                        StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    TypeID underlyingTypeID, interfaceTypeID;
    bool isImplicit;
    GenericSignatureID genericSigID;
    uint8_t rawAccessLevel;
    ArrayRef<uint64_t> dependencyIDs;

    decls_block::TypeAliasLayout::readRecord(scratch, nameID, contextID,
                                             underlyingTypeID, interfaceTypeID,
                                             isImplicit, genericSigID,
                                             rawAccessLevel, dependencyIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID : dependencyIDs) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()));
      }
    }

    auto DC = MF.getDeclContext(contextID);

    auto genericParams = MF.maybeReadGenericParams(DC);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto alias = MF.createDecl<TypeAliasDecl>(SourceLoc(), SourceLoc(), name,
                                              SourceLoc(), genericParams, DC);
    declOrOffset = alias;

    auto genericSig = MF.getGenericSignature(genericSigID);
    alias->setGenericSignature(genericSig);

    auto underlying = MF.getType(underlyingTypeID);
    alias->setUnderlyingType(underlying);
    
    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      alias->setAccess(*accessLevel);
    else
      MF.fatal();

    if (isImplicit)
      alias->setImplicit();

    return alias;
  }

  Expected<Decl *>
  deserializeGenericTypeParamDecl(ArrayRef<uint64_t> scratch,
                                  StringRef blobData) {
    IdentifierID nameID;
    bool isImplicit;
    unsigned depth;
    unsigned index;

    decls_block::GenericTypeParamDeclLayout::readRecord(scratch, nameID,
                                                        isImplicit,
                                                        depth,
                                                        index);

    // Always create GenericTypeParamDecls in the associated file; the real
    // context will reparent them.
    auto *DC = MF.getFile();
    auto genericParam = MF.createDecl<GenericTypeParamDecl>(
        DC, MF.getIdentifier(nameID), SourceLoc(), depth, index);
    declOrOffset = genericParam;

    if (isImplicit)
      genericParam->setImplicit();

    return genericParam;
  }

  Expected<Decl *>
  deserializeAssociatedTypeDecl(ArrayRef<uint64_t> scratch,
                                StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    TypeID defaultDefinitionID;
    bool isImplicit;
    ArrayRef<uint64_t> rawOverriddenIDs;

    decls_block::AssociatedTypeDeclLayout::readRecord(scratch, nameID,
                                                      contextID,
                                                      defaultDefinitionID,
                                                      isImplicit,
                                                      rawOverriddenIDs);

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    // The where-clause information is pushed up into the protocol
    // (specifically, into its requirement signature) and
    // serialized/deserialized there, so the actual Decl doesn't need to store
    // it.
    TrailingWhereClause *trailingWhere = nullptr;
    auto assocType = MF.createDecl<AssociatedTypeDecl>(
        DC, SourceLoc(), MF.getIdentifier(nameID), SourceLoc(), trailingWhere,
        &MF, defaultDefinitionID);
    declOrOffset = assocType;

    assert(!assocType->getDeclaredInterfaceType()->hasError() &&
           "erroneous associated type");

    AccessLevel parentAccess = cast<ProtocolDecl>(DC)->getFormalAccess();
    assocType->setAccess(std::max(parentAccess, AccessLevel::Internal));
    if (isImplicit)
      assocType->setImplicit();

    // Overridden associated types.
    SmallVector<ValueDecl *, 2> overriddenAssocTypes;
    for (auto overriddenID : rawOverriddenIDs) {
      if (auto overriddenAssocType =
              dyn_cast_or_null<AssociatedTypeDecl>(MF.getDecl(overriddenID))) {
        overriddenAssocTypes.push_back(overriddenAssocType);
      }
    }
    assocType->setOverriddenDecls(overriddenAssocTypes);

    return assocType;
  }

  Expected<Decl *> deserializeStruct(ArrayRef<uint64_t> scratch,
                                     StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    bool isImplicit;
    bool isObjC;
    GenericSignatureID genericSigID;
    uint8_t rawAccessLevel;
    unsigned numConformances, numInheritedTypes;
    ArrayRef<uint64_t> rawInheritedAndDependencyIDs;

    decls_block::StructLayout::readRecord(scratch, nameID, contextID,
                                          isImplicit, isObjC, genericSigID,
                                          rawAccessLevel,
                                          numConformances, numInheritedTypes,
                                          rawInheritedAndDependencyIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID :
           rawInheritedAndDependencyIDs.slice(numInheritedTypes)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()));
      }
    }

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto genericParams = MF.maybeReadGenericParams(DC);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto theStruct = MF.createDecl<StructDecl>(SourceLoc(), name, SourceLoc(),
                                               None, genericParams, DC);
    declOrOffset = theStruct;

    // Read the generic environment.
    theStruct->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theStruct->setAccess(*accessLevel);
    else
      MF.fatal();

    theStruct->setAddedImplicitInitializers();
    if (isImplicit)
      theStruct->setImplicit();
    theStruct->setIsObjC(isObjC);

    handleInherited(theStruct,
                    rawInheritedAndDependencyIDs.slice(0, numInheritedTypes));

    theStruct->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theStruct->setConformanceLoader(
      &MF,
      encodeLazyConformanceContextData(numConformances,
                                       MF.DeclTypeCursor.GetCurrentBitNo()));

    return theStruct;
  }

  Expected<Decl *> deserializeConstructor(ArrayRef<uint64_t> scratch,
                                          StringRef blobData) {
    DeclContextID contextID;
    bool isIUO, isFailable;
    bool isImplicit, isObjC, hasStubImplementation, throws;
    GenericSignatureID genericSigID;
    uint8_t storedInitKind, rawAccessLevel;
    DeclID overriddenID;
    bool needsNewVTableEntry, firstTimeRequired;
    unsigned numArgNames;
    ArrayRef<uint64_t> argNameAndDependencyIDs;

    decls_block::ConstructorLayout::readRecord(scratch, contextID,
                                               isFailable, isIUO, isImplicit,
                                               isObjC, hasStubImplementation,
                                               throws, storedInitKind,
                                               genericSigID,
                                               overriddenID,
                                               rawAccessLevel,
                                               needsNewVTableEntry,
                                               firstTimeRequired,
                                               numArgNames,
                                               argNameAndDependencyIDs);

    // Resolve the name ids.
    SmallVector<Identifier, 2> argNames;
    for (auto argNameID : argNameAndDependencyIDs.slice(0, numArgNames))
      argNames.push_back(MF.getIdentifier(argNameID));
    DeclName name(ctx, DeclBaseName::createConstructor(), argNames);
    PrettySupplementalDeclNameTrace trace(name);

    Optional<swift::CtorInitializerKind> initKind =
        getActualCtorInitializerKind(storedInitKind);

    DeclDeserializationError::Flags errorFlags;
    unsigned numVTableEntries = 0;
    if (initKind == CtorInitializerKind::Designated)
      errorFlags |= DeclDeserializationError::DesignatedInitializer;
    if (needsNewVTableEntry) {
      numVTableEntries = 1;
      DeclAttributes attrs;
      attrs.setRawAttributeChain(DAttrs);
    }

    auto overridden = MF.getDeclChecked(overriddenID);
    if (!overridden) {
      llvm::consumeError(overridden.takeError());
      return llvm::make_error<OverrideError>(
          name, errorFlags, numVTableEntries);
    }

    for (auto dependencyID : argNameAndDependencyIDs.slice(numArgNames)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()),
            errorFlags, numVTableEntries);
      }
    }

    auto parent = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto *genericParams = MF.maybeReadGenericParams(parent);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto ctor = MF.createDecl<ConstructorDecl>(name, SourceLoc(), isFailable,
                                               /*FailabilityLoc=*/SourceLoc(),
                                               /*Throws=*/throws,
                                               /*ThrowsLoc=*/SourceLoc(),
                                               /*BodyParams=*/nullptr,
                                               genericParams, parent);
    declOrOffset = ctor;

    ctor->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      ctor->setAccess(*accessLevel);
    else
      MF.fatal();

    auto *bodyParams = MF.readParameterList();
    assert(bodyParams && "missing parameters for constructor");
    ctor->setParameters(bodyParams);

    if (auto errorConvention = MF.maybeReadForeignErrorConvention())
      ctor->setForeignErrorConvention(*errorConvention);

    if (auto bodyText = MF.maybeReadInlinableBodyText())
      ctor->setBodyStringRepresentation(*bodyText);

    if (isImplicit)
      ctor->setImplicit();
    ctor->setIsObjC(isObjC);
    if (hasStubImplementation)
      ctor->setStubImplementation(true);
    if (initKind.hasValue())
      ctx.evaluator.cacheOutput(InitKindRequest{ctor},
                                std::move(initKind.getValue()));
    ctx.evaluator.cacheOutput(NeedsNewVTableEntryRequest{ctor},
                              std::move(needsNewVTableEntry));

    ctor->setOverriddenDecl(cast_or_null<ConstructorDecl>(overridden.get()));
    if (auto *overridden = ctor->getOverriddenDecl()) {
      if (!attributeChainContains<RequiredAttr>(DAttrs) ||
          !overridden->isRequired()) {
        // FIXME: why is a convenience init considered overridden when the
        // overriding init can't be marked overriding in source?
        if (!overridden->isConvenienceInit())
          AddAttribute(new (ctx) OverrideAttr(SourceLoc()));
      }
    }

    ctor->setImplicitlyUnwrappedOptional(isIUO);

    return ctor;
  }

  Expected<Decl *> deserializeVar(ArrayRef<uint64_t> scratch,
                                  StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    bool isImplicit, isObjC, isStatic;
    uint8_t rawIntroducer;
    bool isGetterMutating, isSetterMutating;
    bool isLazyStorageProperty;
    bool isTopLevelGlobal;
    DeclID lazyStorageID;
    unsigned numAccessors, numBackingProperties;
    uint8_t readImpl, writeImpl, readWriteImpl, opaqueReadOwnership;
    uint8_t rawAccessLevel, rawSetterAccessLevel;
    TypeID interfaceTypeID;
    bool isIUO;
    ModuleFile::AccessorRecord accessors;
    DeclID overriddenID, opaqueReturnTypeID;
    unsigned numVTableEntries;
    ArrayRef<uint64_t> arrayFieldIDs;

    decls_block::VarLayout::readRecord(scratch, nameID, contextID,
                                       isImplicit, isObjC, isStatic, rawIntroducer,
                                       isGetterMutating, isSetterMutating,
                                       isLazyStorageProperty,
                                       isTopLevelGlobal,
                                       lazyStorageID,
                                       opaqueReadOwnership,
                                       readImpl, writeImpl, readWriteImpl,
                                       numAccessors,
                                       interfaceTypeID,
                                       isIUO,
                                       overriddenID,
                                       rawAccessLevel, rawSetterAccessLevel,
                                       opaqueReturnTypeID,
                                       numBackingProperties,
                                       numVTableEntries,
                                       arrayFieldIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto getErrorFlags = [&]() {
      // Stored properties in classes still impact class object layout because
      // their offset is computed and stored in the field offset vector.
      DeclDeserializationError::Flags errorFlags;

      if (!isStatic) {
        auto actualReadImpl = getActualReadImplKind(readImpl);
        if (actualReadImpl && *actualReadImpl == ReadImplKind::Stored) {
          errorFlags |= DeclDeserializationError::Flag::NeedsFieldOffsetVectorEntry;
        }
      }

      return errorFlags;
    };

    Expected<Decl *> overridden = MF.getDeclChecked(overriddenID);
    if (!overridden) {
      llvm::consumeError(overridden.takeError());

      return llvm::make_error<OverrideError>(
          name, getErrorFlags(), numVTableEntries);
    }

    // Extract the accessor IDs.
    for (DeclID accessorID : arrayFieldIDs.slice(0, numAccessors)) {
      accessors.IDs.push_back(accessorID);
    }
    arrayFieldIDs = arrayFieldIDs.slice(numAccessors);

    // Extract the backing property IDs.
    auto backingPropertyIDs = arrayFieldIDs.slice(0, numBackingProperties);
    arrayFieldIDs = arrayFieldIDs.slice(numBackingProperties);

    for (TypeID dependencyID : arrayFieldIDs) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()),
            getErrorFlags(), numVTableEntries);
      }
    }

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto introducer = getActualVarDeclIntroducer(
        (serialization::VarDeclIntroducer) rawIntroducer);
    if (!introducer)
      MF.fatal();

    auto var = MF.createDecl<VarDecl>(/*IsStatic*/ isStatic, *introducer,
                                      SourceLoc(), name, DC);
    var->setIsGetterMutating(isGetterMutating);
    var->setIsSetterMutating(isSetterMutating);
    declOrOffset = var;

    auto interfaceTypeOrError = MF.getTypeChecked(interfaceTypeID);
    if (!interfaceTypeOrError)
      return interfaceTypeOrError.takeError();
    Type interfaceType = interfaceTypeOrError.get();
    var->setInterfaceType(interfaceType);
    var->setImplicitlyUnwrappedOptional(isIUO);

    if (auto referenceStorage = interfaceType->getAs<ReferenceStorageType>())
      AddAttribute(
          new (ctx) ReferenceOwnershipAttr(referenceStorage->getOwnership()));

    MF.configureStorage(var, opaqueReadOwnership,
                        readImpl, writeImpl, readWriteImpl, accessors);
    auto accessLevel = getActualAccessLevel(rawAccessLevel);
    if (!accessLevel)
      MF.fatal();

    var->setAccess(*accessLevel);

    if (var->isSettable(nullptr)) {
      auto setterAccess = getActualAccessLevel(rawSetterAccessLevel);
      if (!setterAccess)
        MF.fatal();
      var->setSetterAccess(*setterAccess);

      // If we have a less-accessible setter, honor that by adding the
      // setter access attribute.
      if (*setterAccess < *accessLevel) {
        AddAttribute(
          new (ctx) SetterAccessAttr(SourceLoc(), SourceLoc(),
                                     *setterAccess, /*implicit*/true));
      }
    }

    if (isImplicit)
      var->setImplicit();
    var->setIsObjC(isObjC);

    var->setOverriddenDecl(cast_or_null<VarDecl>(overridden.get()));
    if (var->getOverriddenDecl())
      AddAttribute(new (ctx) OverrideAttr(SourceLoc()));

    // Add the @_hasStorage attribute if this var has storage.
    if (var->hasStorage())
      AddAttribute(new (ctx) HasStorageAttr(/*isImplicit:*/true));

    if (opaqueReturnTypeID) {
      ctx.evaluator.cacheOutput(
          OpaqueResultTypeRequest{var},
          cast<OpaqueTypeDecl>(MF.getDecl(opaqueReturnTypeID)));
    }

    // If this is a lazy property, record its backing storage.
    if (lazyStorageID) {
      VarDecl *storage = cast<VarDecl>(MF.getDecl(lazyStorageID));
      ctx.evaluator.cacheOutput(
          LazyStoragePropertyRequest{var}, std::move(storage));
    }

    var->setLazyStorageProperty(isLazyStorageProperty);
    var->setTopLevelGlobal(isTopLevelGlobal);

    // If there are any backing properties, record them.
    if (numBackingProperties > 0) {
      auto backingDecl = MF.getDeclChecked(backingPropertyIDs[0]);
      if (!backingDecl) {
        // FIXME: This is actually wrong. We can't just drop stored properties
        // willy-nilly if the struct is @frozen.
        consumeError(backingDecl.takeError());
        return var;
      }

      VarDecl *backingVar = cast<VarDecl>(backingDecl.get());
      VarDecl *projectionVar = nullptr;
      if (numBackingProperties > 1) {
        projectionVar = cast<VarDecl>(MF.getDecl(backingPropertyIDs[1]));
      }

      PropertyWrapperBackingPropertyInfo info(
          backingVar, projectionVar, nullptr, nullptr);
      ctx.evaluator.cacheOutput(
          PropertyWrapperBackingPropertyInfoRequest{var}, std::move(info));
      ctx.evaluator.cacheOutput(
          PropertyWrapperBackingPropertyTypeRequest{var},
          backingVar->getInterfaceType());
      backingVar->setOriginalWrappedProperty(var);

      if (projectionVar)
        projectionVar->setOriginalWrappedProperty(var);
    }

    return var;
  }

  Expected<Decl *> deserializeParam(ArrayRef<uint64_t> scratch,
                                    StringRef blobData) {
    IdentifierID argNameID, paramNameID;
    DeclContextID contextID;
    unsigned rawSpecifier;
    TypeID interfaceTypeID;
    bool isIUO;
    bool isVariadic;
    bool isAutoClosure;
    uint8_t rawDefaultArg;

    decls_block::ParamLayout::readRecord(scratch, argNameID, paramNameID,
                                         contextID, rawSpecifier,
                                         interfaceTypeID, isIUO, isVariadic,
                                         isAutoClosure, rawDefaultArg);

    auto argName = MF.getIdentifier(argNameID);
    auto paramName = MF.getIdentifier(paramNameID);
    PrettySupplementalDeclNameTrace trace(paramName);

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto specifier = getActualParamDeclSpecifier(
                              (serialization::ParamDeclSpecifier)rawSpecifier);
    if (!specifier)
      MF.fatal();

    auto param = MF.createDecl<ParamDecl>(SourceLoc(), SourceLoc(), argName,
                                          SourceLoc(), paramName, DC);
    param->setSpecifier(*specifier);

    declOrOffset = param;

    auto paramTy = MF.getType(interfaceTypeID);
    if (paramTy->hasError()) {
      // FIXME: This should never happen, because we don't serialize
      // error types.
      DC->printContext(llvm::errs());
      paramTy->dump(llvm::errs());
      MF.fatal();
    }

    param->setInterfaceType(paramTy);
    param->setImplicitlyUnwrappedOptional(isIUO);
    param->setVariadic(isVariadic);
    param->setAutoClosure(isAutoClosure);

    // Decode the default argument kind.
    // FIXME: Default argument expression, if available.
    if (auto defaultArg = getActualDefaultArgKind(rawDefaultArg)) {
      param->setDefaultArgumentKind(*defaultArg);
      if (!blobData.empty())
        param->setDefaultValueStringRepresentation(blobData);
    }
    return param;
  }

  Expected<Decl *> deserializeAnyFunc(ArrayRef<uint64_t> scratch,
                                      StringRef blobData,
                                      bool isAccessor) {
    DeclContextID contextID;
    bool isImplicit;
    bool isStatic;
    uint8_t rawStaticSpelling, rawAccessLevel, rawMutModifier;
    uint8_t rawAccessorKind;
    bool isObjC, hasForcedStaticDispatch, async, throws;
    unsigned numNameComponentsBiased;
    GenericSignatureID genericSigID;
    TypeID resultInterfaceTypeID;
    bool isIUO;
    DeclID associatedDeclID;
    DeclID overriddenID;
    DeclID accessorStorageDeclID;
    bool overriddenAffectsABI, needsNewVTableEntry, isTransparent;
    DeclID opaqueReturnTypeID;
    bool isUserAccessible;
    ArrayRef<uint64_t> nameAndDependencyIDs;

    if (!isAccessor) {
      decls_block::FuncLayout::readRecord(scratch, contextID, isImplicit,
                                          isStatic, rawStaticSpelling, isObjC,
                                          rawMutModifier,
                                          hasForcedStaticDispatch,
                                          async, throws,
                                          genericSigID,
                                          resultInterfaceTypeID,
                                          isIUO,
                                          associatedDeclID, overriddenID,
                                          overriddenAffectsABI,
                                          numNameComponentsBiased,
                                          rawAccessLevel,
                                          needsNewVTableEntry,
                                          opaqueReturnTypeID,
                                          isUserAccessible,
                                          nameAndDependencyIDs);
    } else {
      decls_block::AccessorLayout::readRecord(scratch, contextID, isImplicit,
                                              isStatic, rawStaticSpelling, isObjC,
                                              rawMutModifier,
                                              hasForcedStaticDispatch, throws,
                                              genericSigID,
                                              resultInterfaceTypeID,
                                              isIUO,
                                              overriddenID,
                                              overriddenAffectsABI,
                                              accessorStorageDeclID,
                                              rawAccessorKind,
                                              rawAccessLevel,
                                              needsNewVTableEntry,
                                              isTransparent,
                                              nameAndDependencyIDs);
    }

    DeclDeserializationError::Flags errorFlags;
    unsigned numVTableEntries = needsNewVTableEntry ? 1 : 0;

    // Parse the accessor-specific fields.
    AbstractStorageDecl *storage = nullptr;
    AccessorKind accessorKind;
    if (isAccessor) {
      auto storageResult = MF.getDeclChecked(accessorStorageDeclID);
      if (!storageResult ||
          !(storage =
              dyn_cast_or_null<AbstractStorageDecl>(storageResult.get()))) {
        // FIXME: "TypeError" isn't exactly correct for this.
        return llvm::make_error<TypeError>(
            DeclName(), takeErrorInfo(storageResult.takeError()),
            errorFlags, numVTableEntries);
      }

      if (auto accessorKindResult = getActualAccessorKind(rawAccessorKind))
        accessorKind = *accessorKindResult;
      else
        MF.fatal();

      // Deserializing the storage declaration will cause a recurrence
      // into this code.  When we come out, don't create the accessor twice.
      // TODO: find some better way of breaking this cycle, like lazily
      // deserializing the accessors.
      if (auto accessor = storage->getAccessor(accessorKind))
        return accessor;
    }

    // Resolve the name ids.
    DeclName name;
    ArrayRef<uint64_t> dependencyIDs;
    if (isAccessor) {
      dependencyIDs = nameAndDependencyIDs;
    } else {
      Identifier baseName = MF.getIdentifier(nameAndDependencyIDs.front());
      if (numNameComponentsBiased != 0) {
        SmallVector<Identifier, 2> names;
        for (auto nameID : nameAndDependencyIDs.slice(1,
                                                      numNameComponentsBiased-1)){
          names.push_back(MF.getIdentifier(nameID));
        }
        name = DeclName(ctx, baseName, names);
        dependencyIDs = nameAndDependencyIDs.slice(numNameComponentsBiased);
      } else {
        name = baseName;
        dependencyIDs = nameAndDependencyIDs.drop_front();
      }
    }
    PrettySupplementalDeclNameTrace trace(name);

    Expected<Decl *> overriddenOrError = MF.getDeclChecked(overriddenID);
    Decl *overridden;
    if (overriddenOrError) {
      overridden = overriddenOrError.get();
    } else {
      llvm::consumeError(overriddenOrError.takeError());

      if (overriddenAffectsABI || !ctx.LangOpts.EnableDeserializationRecovery) {
        return llvm::make_error<OverrideError>(
            name, errorFlags, numVTableEntries);
      }

      overridden = nullptr;
    }

    for (TypeID dependencyID : dependencyIDs) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()),
            errorFlags, numVTableEntries);
      }
    }

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    // Read generic params before reading the type, because the type may
    // reference generic parameters, and we want them to have a dummy
    // DeclContext for now.
    GenericParamList *genericParams = MF.maybeReadGenericParams(DC);

    auto staticSpelling = getActualStaticSpellingKind(rawStaticSpelling);
    if (!staticSpelling.hasValue())
      MF.fatal();

    if (declOrOffset.isComplete())
      return declOrOffset;

    const auto resultType = MF.getType(resultInterfaceTypeID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    FuncDecl *fn;
    if (!isAccessor) {
      fn = FuncDecl::createDeserialized(ctx, staticSpelling.getValue(), name,
                                        async, throws, genericParams,
                                        resultType, DC);
    } else {
      auto *accessor = AccessorDecl::createDeserialized(
          ctx, accessorKind, storage, staticSpelling.getValue(),
          /*Throws=*/throws, genericParams, resultType, DC);
      accessor->setIsTransparent(isTransparent);

      fn = accessor;
    }
    declOrOffset = fn;

    fn->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      fn->setAccess(*accessLevel);
    else
      MF.fatal();

    if (auto SelfAccessKind = getActualSelfAccessKind(rawMutModifier))
      fn->setSelfAccessKind(*SelfAccessKind);
    else
      MF.fatal();

    if (!isAccessor) {
      if (Decl *associated = MF.getDecl(associatedDeclID)) {
        if (auto op = dyn_cast<OperatorDecl>(associated)) {
          ctx.evaluator.cacheOutput(FunctionOperatorRequest{fn},
                                    std::move(op));

          if (isa<PrefixOperatorDecl>(op))
            fn->getAttrs().add(new (ctx) PrefixAttr(/*implicit*/false));
          else if (isa<PostfixOperatorDecl>(op))
            fn->getAttrs().add(new (ctx) PostfixAttr(/*implicit*/false));
          // Note that an explicit 'infix' is not required.
        }
        // Otherwise, unknown associated decl kind.
      }
    }

    fn->setStatic(isStatic);
    fn->setImplicitlyUnwrappedOptional(isIUO);

    ParameterList *paramList = MF.readParameterList();
    fn->setParameters(paramList);

    if (auto errorConvention = MF.maybeReadForeignErrorConvention())
      fn->setForeignErrorConvention(*errorConvention);

    if (auto bodyText = MF.maybeReadInlinableBodyText())
      fn->setBodyStringRepresentation(*bodyText);

    fn->setOverriddenDecl(cast_or_null<FuncDecl>(overridden));
    if (fn->getOverriddenDecl())
      AddAttribute(new (ctx) OverrideAttr(SourceLoc()));

    if (isImplicit)
      fn->setImplicit();
    fn->setIsObjC(isObjC);
    fn->setForcedStaticDispatch(hasForcedStaticDispatch);
    ctx.evaluator.cacheOutput(NeedsNewVTableEntryRequest{fn},
                              std::move(needsNewVTableEntry));

    if (opaqueReturnTypeID) {
      ctx.evaluator.cacheOutput(
          OpaqueResultTypeRequest{fn},
          cast<OpaqueTypeDecl>(MF.getDecl(opaqueReturnTypeID)));
    }

    if (!isAccessor)
      fn->setUserAccessible(isUserAccessible);

    return fn;
  }

  Expected<Decl *> deserializeFunc(ArrayRef<uint64_t> scratch,
                                   StringRef blobData) {
    return deserializeAnyFunc(scratch, blobData, /*isAccessor*/false);
  }
  Expected<Decl *> deserializeAccessor(ArrayRef<uint64_t> scratch,
                                       StringRef blobData) {
    return deserializeAnyFunc(scratch, blobData, /*isAccessor*/true);
  }
      
  Expected<Decl *> deserializeOpaqueType(ArrayRef<uint64_t> scratch,
                                         StringRef blobData) {
    DeclID namingDeclID;
    DeclContextID contextID;
    GenericSignatureID interfaceSigID;
    TypeID interfaceTypeID;
    GenericSignatureID genericSigID;
    SubstitutionMapID underlyingTypeID;
    uint8_t rawAccessLevel;
    decls_block::OpaqueTypeLayout::readRecord(scratch, contextID,
                                              namingDeclID, interfaceSigID,
                                              interfaceTypeID, genericSigID,
                                              underlyingTypeID, rawAccessLevel);
    
    auto declContext = MF.getDeclContext(contextID);
    auto interfaceSig = MF.getGenericSignature(interfaceSigID);
    auto interfaceType = MF.getType(interfaceTypeID)
                            ->castTo<GenericTypeParamType>();
    
    // Check for reentrancy.
    if (declOrOffset.isComplete())
      return cast<OpaqueTypeDecl>(declOrOffset.get());
      
    // Create the decl.
    auto opaqueDecl =
      new (ctx) OpaqueTypeDecl(nullptr, nullptr, declContext,
                               interfaceSig, interfaceType);
    declOrOffset = opaqueDecl;

    auto namingDecl = cast<ValueDecl>(MF.getDecl(namingDeclID));
    opaqueDecl->setNamingDecl(namingDecl);

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      opaqueDecl->setAccess(*accessLevel);
    else
      MF.fatal();

    if (auto genericParams = MF.maybeReadGenericParams(opaqueDecl)) {
      ctx.evaluator.cacheOutput(GenericParamListRequest{opaqueDecl},
                                std::move(genericParams));
    }

    auto genericSig = MF.getGenericSignature(genericSigID);
    if (genericSig)
      opaqueDecl->setGenericSignature(genericSig);
    if (underlyingTypeID) {
      auto subMapOrError = MF.getSubstitutionMapChecked(underlyingTypeID);
      if (!subMapOrError)
        return subMapOrError.takeError();
      opaqueDecl->setUnderlyingTypeSubstitutions(subMapOrError.get());
    }
    SubstitutionMap subs;
    if (genericSig) {
      subs = genericSig->getIdentitySubstitutionMap();
    }
    auto opaqueTy = OpaqueTypeArchetypeType::get(opaqueDecl, subs);
    auto metatype = MetatypeType::get(opaqueTy);
    opaqueDecl->setInterfaceType(metatype);
    return opaqueDecl;
  }

  Expected<Decl *> deserializePatternBinding(ArrayRef<uint64_t> scratch,
                                             StringRef blobData) {
    DeclContextID contextID;
    bool isImplicit;
    bool isStatic;
    uint8_t RawStaticSpelling;
    unsigned numPatterns;
    ArrayRef<uint64_t> initContextIDs;

    decls_block::PatternBindingLayout::readRecord(scratch, contextID,
                                                  isImplicit,
                                                  isStatic,
                                                  RawStaticSpelling,
                                                  numPatterns,
                                                  initContextIDs);
    auto StaticSpelling = getActualStaticSpellingKind(RawStaticSpelling);
    if (!StaticSpelling.hasValue())
      MF.fatal();

    auto dc = MF.getDeclContext(contextID);

    SmallVector<std::pair<Pattern *, DeclContextID>, 4> patterns;
    for (unsigned i = 0; i != numPatterns; ++i) {
      auto pattern = MF.readPattern(dc);
      if (!pattern) {
        // Silently drop the pattern...
        llvm::consumeError(pattern.takeError());
        // ...but continue to read any further patterns we're expecting.
        continue;
      }

      patterns.emplace_back(pattern.get(), DeclContextID());
      if (!initContextIDs.empty()) {
        patterns.back().second =
            DeclContextID::getFromOpaqueValue(initContextIDs[i]);
      }
    }

    auto binding =
      PatternBindingDecl::createDeserialized(ctx, SourceLoc(),
                                             StaticSpelling.getValue(),
                                             SourceLoc(), patterns.size(), dc);
    declOrOffset = binding;

    binding->setStatic(isStatic);

    if (isImplicit)
      binding->setImplicit();

    for (unsigned i = 0; i != patterns.size(); ++i) {
      DeclContext *initContext = MF.getDeclContext(patterns[i].second);
      binding->setPattern(i, patterns[i].first, initContext);
    }

    return binding;
  }

  Expected<Decl *> deserializeProtocol(ArrayRef<uint64_t> scratch,
                                       StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    bool isImplicit, isClassBounded, isObjC, existentialTypeSupported;
    uint8_t rawAccessLevel;
    unsigned numInheritedTypes;
    ArrayRef<uint64_t> rawInheritedAndDependencyIDs;

    decls_block::ProtocolLayout::readRecord(scratch, nameID, contextID,
                                            isImplicit, isClassBounded, isObjC,
                                            existentialTypeSupported,
                                            rawAccessLevel, numInheritedTypes,
                                            rawInheritedAndDependencyIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID :
           rawInheritedAndDependencyIDs.slice(numInheritedTypes)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()));
      }
    }

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto proto = MF.createDecl<ProtocolDecl>(DC, SourceLoc(), SourceLoc(), name,
                                             None, /*TrailingWhere=*/nullptr);
    declOrOffset = proto;

    ctx.evaluator.cacheOutput(ProtocolRequiresClassRequest{proto},
                              std::move(isClassBounded));
    ctx.evaluator.cacheOutput(ExistentialTypeSupportedRequest{proto},
                              std::move(existentialTypeSupported));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      proto->setAccess(*accessLevel);
    else
      MF.fatal();

    auto genericParams = MF.maybeReadGenericParams(DC);
    assert(genericParams && "protocol with no generic parameters?");
    ctx.evaluator.cacheOutput(GenericParamListRequest{proto},
                              std::move(genericParams));

    handleInherited(proto,
                    rawInheritedAndDependencyIDs.slice(0, numInheritedTypes));

    if (isImplicit)
      proto->setImplicit();
    proto->setIsObjC(isObjC);

    proto->setLazyRequirementSignature(&MF,
                                       MF.DeclTypeCursor.GetCurrentBitNo());
    if (llvm::Error Err = skipGenericRequirements(MF.DeclTypeCursor))
      MF.fatal(std::move(Err));

    proto->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());

    return proto;
  }

  template <typename OperatorLayout, typename OperatorDecl>
  Expected<Decl *> deserializeUnaryOperator(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    ArrayRef<uint64_t> designatedNominalTypeDeclIDs;

    OperatorLayout::readRecord(scratch, nameID, contextID,
                               designatedNominalTypeDeclIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto DC = MF.getDeclContext(contextID);

    SmallVector<NominalTypeDecl *, 1> designatedNominalTypes;
    for (auto id : designatedNominalTypeDeclIDs) {
      Expected<Decl *> nominal = MF.getDeclChecked(id);
      if (!nominal)
        return nominal.takeError();
      designatedNominalTypes.push_back(cast<NominalTypeDecl>(nominal.get()));
    }

    auto result = MF.createDecl<OperatorDecl>(
        DC, SourceLoc(), name, SourceLoc(),
        ctx.AllocateCopy(designatedNominalTypes));

    declOrOffset = result;
    return result;
  }

  Expected<Decl *> deserializePrefixOperator(ArrayRef<uint64_t> scratch,
                                             StringRef blobData) {
    return deserializeUnaryOperator<decls_block::PrefixOperatorLayout,
                                    PrefixOperatorDecl>(scratch, blobData);
  }

  Expected<Decl *> deserializePostfixOperator(ArrayRef<uint64_t> scratch,
                                              StringRef blobData) {
    return deserializeUnaryOperator<decls_block::PostfixOperatorLayout,
                                    PostfixOperatorDecl>(scratch, blobData);
  }

  Expected<Decl *> deserializeInfixOperator(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    DeclID precedenceGroupID;
    ArrayRef<uint64_t> designatedNominalTypeDeclIDs;

    decls_block::InfixOperatorLayout::readRecord(scratch, nameID, contextID,
                                                 precedenceGroupID,
                                                 designatedNominalTypeDeclIDs);
    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    Expected<Decl *> precedenceGroup = MF.getDeclChecked(precedenceGroupID);
    if (!precedenceGroup)
      return precedenceGroup.takeError();

    auto DC = MF.getDeclContext(contextID);

    SmallVector<NominalTypeDecl *, 1> designatedNominalTypes;
    for (auto id : designatedNominalTypeDeclIDs) {
      Expected<Decl *> nominal = MF.getDeclChecked(id);
      if (!nominal)
        return nominal.takeError();
      designatedNominalTypes.push_back(cast<NominalTypeDecl>(nominal.get()));
    }

    auto result = MF.createDecl<InfixOperatorDecl>(
        DC, SourceLoc(), name, SourceLoc(), SourceLoc(),
        ArrayRef<Located<Identifier>>{});
    result->setDesignatedNominalTypes(ctx.AllocateCopy(designatedNominalTypes));
    ctx.evaluator.cacheOutput(
        OperatorPrecedenceGroupRequest{result},
        std::move(cast_or_null<PrecedenceGroupDecl>(precedenceGroup.get())));
    
    declOrOffset = result;
    return result;
  }

  Expected<Decl *> deserializePrecedenceGroup(ArrayRef<uint64_t> scratch,
                                              StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    uint8_t rawAssociativity;
    bool assignment;
    unsigned numHigherThan;
    ArrayRef<uint64_t> rawRelations;

    decls_block::PrecedenceGroupLayout::readRecord(scratch, nameID, contextID,
                                                   rawAssociativity,
                                                   assignment, numHigherThan,
                                                   rawRelations);

    auto DC = MF.getDeclContext(contextID);

    auto associativity = getActualAssociativity(rawAssociativity);
    if (!associativity.hasValue())
      MF.fatal();

    if (numHigherThan > rawRelations.size())
      MF.fatal();

    SmallVector<PrecedenceGroupDecl::Relation, 4> higherThan;
    for (auto relID : rawRelations.slice(0, numHigherThan)) {
      PrecedenceGroupDecl *rel = nullptr;
      if (relID)
        rel = dyn_cast_or_null<PrecedenceGroupDecl>(MF.getDecl(relID));
      if (!rel)
        MF.fatal();

      higherThan.push_back({SourceLoc(), rel->getName(), rel});
    }

    SmallVector<PrecedenceGroupDecl::Relation, 4> lowerThan;
    for (auto relID : rawRelations.slice(numHigherThan)) {
      PrecedenceGroupDecl *rel = nullptr;
      if (relID)
        rel = dyn_cast_or_null<PrecedenceGroupDecl>(MF.getDecl(relID));
      if (!rel)
        MF.fatal();

      lowerThan.push_back({SourceLoc(), rel->getName(), rel});
    }

    declOrOffset = PrecedenceGroupDecl::create(DC, SourceLoc(), SourceLoc(),
                                               MF.getIdentifier(nameID),
                                               SourceLoc(),
                                               SourceLoc(), SourceLoc(),
                                               *associativity,
                                               SourceLoc(), SourceLoc(),
                                               assignment,
                                               SourceLoc(), higherThan,
                                               SourceLoc(), lowerThan,
                                               SourceLoc());
    return declOrOffset.get();
  }

  Expected<Decl *> deserializeClass(ArrayRef<uint64_t> scratch,
                                    StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    bool isImplicit, isObjC;
    bool inheritsSuperclassInitializers;
    bool hasMissingDesignatedInits;
    GenericSignatureID genericSigID;
    TypeID superclassID;
    uint8_t rawAccessLevel;
    unsigned numConformances, numInheritedTypes;
    ArrayRef<uint64_t> rawInheritedAndDependencyIDs;
    decls_block::ClassLayout::readRecord(scratch, nameID, contextID,
                                         isImplicit, isObjC,
                                         inheritsSuperclassInitializers,
                                         hasMissingDesignatedInits,
                                         genericSigID, superclassID,
                                         rawAccessLevel, numConformances,
                                         numInheritedTypes,
                                         rawInheritedAndDependencyIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID :
           rawInheritedAndDependencyIDs.slice(numInheritedTypes)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()));
      }
    }

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto genericParams = MF.maybeReadGenericParams(DC);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto theClass = MF.createDecl<ClassDecl>(SourceLoc(), name, SourceLoc(),
                                             None, genericParams, DC);
    declOrOffset = theClass;

    theClass->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theClass->setAccess(*accessLevel);
    else
      MF.fatal();

    theClass->setAddedImplicitInitializers();
    if (isImplicit)
      theClass->setImplicit();
    theClass->setIsObjC(isObjC);
    theClass->setSuperclass(MF.getType(superclassID));
    ctx.evaluator.cacheOutput(InheritsSuperclassInitializersRequest{theClass},
                              std::move(inheritsSuperclassInitializers));
    ctx.evaluator.cacheOutput(HasMissingDesignatedInitializersRequest{theClass},
                              std::move(hasMissingDesignatedInits));

    handleInherited(theClass,
                    rawInheritedAndDependencyIDs.slice(0, numInheritedTypes));

    theClass->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theClass->setConformanceLoader(
      &MF,
      encodeLazyConformanceContextData(numConformances,
                                       MF.DeclTypeCursor.GetCurrentBitNo()));
    return theClass;
  }

  Expected<Decl *> deserializeEnum(ArrayRef<uint64_t> scratch,
                                   StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;
    bool isImplicit;
    bool isObjC;
    GenericSignatureID genericSigID;
    TypeID rawTypeID;
    uint8_t rawAccessLevel;
    unsigned numConformances, numInherited;
    ArrayRef<uint64_t> rawInheritedAndDependencyIDs;

    decls_block::EnumLayout::readRecord(scratch, nameID, contextID,
                                        isImplicit, isObjC, genericSigID,
                                        rawTypeID, rawAccessLevel,
                                        numConformances, numInherited,
                                        rawInheritedAndDependencyIDs);

    if (declOrOffset.isComplete())
      return declOrOffset;

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID :
           rawInheritedAndDependencyIDs.slice(numInherited)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()));
      }
    }

    auto DCOrError = MF.getDeclContextChecked(contextID);
    if (!DCOrError)
      return DCOrError.takeError();
    auto DC = DCOrError.get();

    auto genericParams = MF.maybeReadGenericParams(DC);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto theEnum = MF.createDecl<EnumDecl>(SourceLoc(), name, SourceLoc(), None,
                                           genericParams, DC);

    declOrOffset = theEnum;

    theEnum->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theEnum->setAccess(*accessLevel);
    else
      MF.fatal();

    theEnum->setAddedImplicitInitializers();
    // @objc enums have all their raw values checked.
    if (isObjC) {
      theEnum->setHasFixedRawValues();
    }
    
    if (isImplicit)
      theEnum->setImplicit();
    theEnum->setIsObjC(isObjC);

    theEnum->setRawType(MF.getType(rawTypeID));

    auto rawInheritedIDs = rawInheritedAndDependencyIDs.slice(0, numInherited);
    handleInherited(theEnum, rawInheritedIDs);

    theEnum->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theEnum->setConformanceLoader(
      &MF,
      encodeLazyConformanceContextData(numConformances,
                                       MF.DeclTypeCursor.GetCurrentBitNo()));
    return theEnum;
  }

  Expected<Decl *> deserializeEnumElement(ArrayRef<uint64_t> scratch,
                                          StringRef blobData) {
    DeclContextID contextID;
    bool isImplicit, hasPayload, isRawValueImplicit, isNegative;
    unsigned rawValueKindID;
    IdentifierID rawValueData;
    unsigned numArgNames;
    ArrayRef<uint64_t> argNameAndDependencyIDs;

    decls_block::EnumElementLayout::readRecord(scratch, contextID,
                                               isImplicit, hasPayload,
                                               rawValueKindID,
                                               isRawValueImplicit, isNegative,
                                               rawValueData,
                                               numArgNames,
                                               argNameAndDependencyIDs);

    // Resolve the name ids.
    Identifier baseName = MF.getIdentifier(argNameAndDependencyIDs.front());
    SmallVector<Identifier, 2> argNames;
    for (auto argNameID : argNameAndDependencyIDs.slice(1, numArgNames-1))
      argNames.push_back(MF.getIdentifier(argNameID));
    DeclName compoundName(ctx, baseName, argNames);
    DeclName name = argNames.empty() ? baseName : compoundName;
    PrettySupplementalDeclNameTrace trace(name);

    for (TypeID dependencyID : argNameAndDependencyIDs.slice(numArgNames)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        // Enum elements never introduce missing members in their parent enum.
        //
        // A frozen enum cannot be laid out if its missing cases anyway,
        // so the dependency mechanism ensures the entire enum fails to
        // deserialize.
        //
        // For a resilient enum, we don't care and just drop the element
        // and continue.
        return llvm::make_error<TypeError>(
          name, takeErrorInfo(dependency.takeError()));
      }
    }

    DeclContext *DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto elem = MF.createDecl<EnumElementDecl>(SourceLoc(),
                                               name,
                                               nullptr,
                                               SourceLoc(),
                                               nullptr,
                                               DC);
    declOrOffset = elem;

    // Read payload parameter list, if it exists.
    if (hasPayload) {
      auto *paramList = MF.readParameterList();
      elem->setParameterList(paramList);
    }

    // Deserialize the literal raw value, if any.
    switch ((EnumElementRawValueKind)rawValueKindID) {
    case EnumElementRawValueKind::None:
      break;
    case EnumElementRawValueKind::IntegerLiteral: {
      auto literalText = MF.getIdentifierText(rawValueData);
      auto literal = new (ctx) IntegerLiteralExpr(literalText, SourceLoc(),
                                                  isRawValueImplicit);
      if (isNegative)
        literal->setNegative(SourceLoc());
      elem->setRawValueExpr(literal);
    }
    }

    if (isImplicit)
      elem->setImplicit();
    elem->setAccess(std::max(cast<EnumDecl>(DC)->getFormalAccess(),
                             AccessLevel::Internal));

    return elem;
  }

  Expected<Decl *> deserializeSubscript(ArrayRef<uint64_t> scratch,
                                        StringRef blobData) {
    DeclContextID contextID;
    bool isImplicit, isObjC, isGetterMutating, isSetterMutating;
    GenericSignatureID genericSigID;
    TypeID elemInterfaceTypeID;
    bool isIUO;
    ModuleFile::AccessorRecord accessors;
    DeclID overriddenID, opaqueReturnTypeID;
    uint8_t rawAccessLevel, rawSetterAccessLevel, rawStaticSpelling;
    uint8_t opaqueReadOwnership, readImpl, writeImpl, readWriteImpl;
    unsigned numArgNames, numAccessors;
    unsigned numVTableEntries;
    ArrayRef<uint64_t> argNameAndDependencyIDs;

    decls_block::SubscriptLayout::readRecord(scratch, contextID,
                                             isImplicit, isObjC,
                                             isGetterMutating, isSetterMutating,
                                             opaqueReadOwnership,
                                             readImpl, writeImpl, readWriteImpl,
                                             numAccessors,
                                             genericSigID,
                                             elemInterfaceTypeID,
                                             isIUO,
                                             overriddenID, rawAccessLevel,
                                             rawSetterAccessLevel,
                                             rawStaticSpelling, numArgNames,
                                             opaqueReturnTypeID,
                                             numVTableEntries,
                                             argNameAndDependencyIDs);
    // Resolve the name ids.
    SmallVector<Identifier, 2> argNames;
    for (auto argNameID : argNameAndDependencyIDs.slice(0, numArgNames))
      argNames.push_back(MF.getIdentifier(argNameID));
    DeclName name(ctx, DeclBaseName::createSubscript(), argNames);
    PrettySupplementalDeclNameTrace trace(name);

    argNameAndDependencyIDs = argNameAndDependencyIDs.slice(numArgNames);

    // Exctract the accessor IDs.
    for (DeclID accessorID : argNameAndDependencyIDs.slice(0, numAccessors)) {
      accessors.IDs.push_back(accessorID);
    }
    argNameAndDependencyIDs = argNameAndDependencyIDs.slice(numAccessors);

    Expected<Decl *> overridden = MF.getDeclChecked(overriddenID);
    if (!overridden) {
      llvm::consumeError(overridden.takeError());

      DeclDeserializationError::Flags errorFlags;
      return llvm::make_error<OverrideError>(
          name, errorFlags, numVTableEntries);
    }

    for (TypeID dependencyID : argNameAndDependencyIDs) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        DeclDeserializationError::Flags errorFlags;
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()),
            errorFlags, numVTableEntries);
      }
    }

    auto parent = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto *genericParams = MF.maybeReadGenericParams(parent);
    if (declOrOffset.isComplete())
      return declOrOffset;
    
    auto staticSpelling = getActualStaticSpellingKind(rawStaticSpelling);
    if (!staticSpelling.hasValue())
      MF.fatal();

    const auto elemInterfaceType = MF.getType(elemInterfaceTypeID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto *const subscript = SubscriptDecl::createDeserialized(
        ctx, name, *staticSpelling, elemInterfaceType, parent, genericParams);
    subscript->setIsGetterMutating(isGetterMutating);
    subscript->setIsSetterMutating(isSetterMutating);
    declOrOffset = subscript;

    subscript->setGenericSignature(MF.getGenericSignature(genericSigID));

    subscript->setIndices(MF.readParameterList());

    MF.configureStorage(subscript, opaqueReadOwnership,
                        readImpl, writeImpl, readWriteImpl, accessors);

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      subscript->setAccess(*accessLevel);
    else
      MF.fatal();

    if (subscript->supportsMutation()) {
      if (auto setterAccess = getActualAccessLevel(rawSetterAccessLevel))
        subscript->setSetterAccess(*setterAccess);
      else
        MF.fatal();
    }

    subscript->setImplicitlyUnwrappedOptional(isIUO);

    if (isImplicit)
      subscript->setImplicit();
    subscript->setIsObjC(isObjC);
    subscript->setOverriddenDecl(cast_or_null<SubscriptDecl>(overridden.get()));
    if (subscript->getOverriddenDecl())
      AddAttribute(new (ctx) OverrideAttr(SourceLoc()));
    
    if (opaqueReturnTypeID) {
      ctx.evaluator.cacheOutput(
          OpaqueResultTypeRequest{subscript},
          cast<OpaqueTypeDecl>(MF.getDecl(opaqueReturnTypeID)));
    }
    
    return subscript;
  }

  Expected<Decl *> deserializeExtension(ArrayRef<uint64_t> scratch,
                                        StringRef blobData) {
    TypeID extendedTypeID;
    DeclID extendedNominalID;
    DeclContextID contextID;
    bool isImplicit;
    GenericSignatureID genericSigID;
    unsigned numConformances, numInherited;
    ArrayRef<uint64_t> inheritedAndDependencyIDs;

    decls_block::ExtensionLayout::readRecord(scratch, extendedTypeID,
                                             extendedNominalID, contextID,
                                             isImplicit, genericSigID,
                                             numConformances, numInherited,
                                             inheritedAndDependencyIDs);

    auto DC = MF.getDeclContext(contextID);

    for (TypeID dependencyID : inheritedAndDependencyIDs.slice(numInherited)) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        return llvm::make_error<ExtensionError>(
            takeErrorInfo(dependency.takeError()));
      }
    }

    if (declOrOffset.isComplete())
      return declOrOffset;

    auto extension = ExtensionDecl::create(ctx, SourceLoc(), nullptr, { },
                                           DC, nullptr);
    declOrOffset = extension;

    // Generic parameter lists are written from outermost to innermost.
    // Keep reading until we run out of generic parameter lists.
    GenericParamList *outerParams = nullptr;
    while (auto *genericParams = MF.maybeReadGenericParams(DC)) {
      genericParams->setOuterParameters(outerParams);

      // Set up the DeclContexts for the GenericTypeParamDecls in the list.
      for (auto param : *genericParams)
        param->setDeclContext(extension);

      outerParams = genericParams;
    }
    ctx.evaluator.cacheOutput(GenericParamListRequest{extension},
                              std::move(outerParams));

    extension->setGenericSignature(MF.getGenericSignature(genericSigID));

    auto extendedType = MF.getType(extendedTypeID);
    ctx.evaluator.cacheOutput(ExtendedTypeRequest{extension},
                              std::move(extendedType));
    auto nominal = dyn_cast<NominalTypeDecl>(MF.getDecl(extendedNominalID));
    ctx.evaluator.cacheOutput(ExtendedNominalRequest{extension},
                              std::move(nominal));

    if (isImplicit)
      extension->setImplicit();

    auto rawInheritedIDs = inheritedAndDependencyIDs.slice(0, numInherited);
    handleInherited(extension, rawInheritedIDs);

    extension->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    extension->setConformanceLoader(
      &MF,
      encodeLazyConformanceContextData(numConformances,
                                       MF.DeclTypeCursor.GetCurrentBitNo()));

    nominal->addExtension(extension);

#ifndef NDEBUG
    if (outerParams) {
      unsigned paramCount = 0;
      for (auto *paramList = outerParams;
           paramList != nullptr;
           paramList = paramList->getOuterParameters()) {
        paramCount += paramList->size();
      }
      assert(paramCount ==
             extension->getGenericSignature()->getGenericParams().size());
    }
#endif

    return extension;
  }

  Expected<Decl *> deserializeDestructor(ArrayRef<uint64_t> scratch,
                                         StringRef blobData) {
    DeclContextID contextID;
    bool isImplicit, isObjC;
    GenericSignatureID genericSigID;

    decls_block::DestructorLayout::readRecord(scratch, contextID,
                                              isImplicit, isObjC,
                                              genericSigID);

    DeclContext *DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto dtor = MF.createDecl<DestructorDecl>(SourceLoc(), DC);
    declOrOffset = dtor;

    if (auto bodyText = MF.maybeReadInlinableBodyText())
      dtor->setBodyStringRepresentation(*bodyText);

    dtor->setGenericSignature(MF.getGenericSignature(genericSigID));

    dtor->setAccess(std::max(cast<ClassDecl>(DC)->getFormalAccess(),
                             AccessLevel::Internal));

    if (isImplicit)
      dtor->setImplicit();
    dtor->setIsObjC(isObjC);

    return dtor;
  }
};
}

Expected<Decl *>
ModuleFile::getDeclChecked(
    DeclID DID,
    llvm::function_ref<bool(DeclAttributes)> matchAttributes) {
  if (DID == 0)
    return nullptr;

  assert(DID <= Decls.size() && "invalid decl ID");
  auto &declOrOffset = Decls[DID-1];

  if (!declOrOffset.isComplete()) {
    ++NumDeclsLoaded;
    BCOffsetRAII restoreOffset(DeclTypeCursor);
    fatalIfNotSuccess(DeclTypeCursor.JumpToBit(declOrOffset));

    Expected<Decl *> deserialized =
      DeclDeserializer(*this, declOrOffset).getDeclCheckedImpl(
        matchAttributes);
    if (!deserialized)
      return deserialized;
  } else if (matchAttributes) {
    // Decl was cached but we may need to filter it
    if (!matchAttributes(declOrOffset.get()->getAttrs()))
      return llvm::make_error<DeclAttributesDidNotMatch>();
  }

  // Tag every deserialized ValueDecl coming out of getDeclChecked with its ID.
  assert(declOrOffset.isComplete());
  if (auto *IDC = dyn_cast_or_null<IterableDeclContext>(declOrOffset.get())) {
    // Only set the DeclID on the returned Decl if it's one that was loaded
    // and _wasn't_ one that had its DeclID set elsewhere (a followed XREF).
    if (IDC->wasDeserialized() &&
        static_cast<uint32_t>(IDC->getDeclID()) == 0) {
      IDC->setDeclID(DID);
    }
  }
  return declOrOffset;
}

llvm::Error DeclDeserializer::deserializeDeclAttributes() {
  using namespace decls_block;

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  while (true) {
    BCOffsetRAII restoreOffset(MF.DeclTypeCursor);
    llvm::BitstreamEntry entry =
        MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());
    if (entry.Kind != llvm::BitstreamEntry::Record) {
      // We don't know how to serialize decls represented by sub-blocks.
      MF.fatal();
    }

    unsigned recordID = MF.fatalIfUnexpected(
        MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

    if (isDeclAttrRecord(recordID)) {
      DeclAttribute *Attr = nullptr;
      bool skipAttr = false;
      switch (recordID) {
      case decls_block::SILGenName_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::SILGenNameDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) SILGenNameAttr(blobData, isImplicit);
        break;
      }

      case decls_block::CDecl_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::CDeclDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) CDeclAttr(blobData, isImplicit);
        break;
      }

      case decls_block::Alignment_DECL_ATTR: {
        bool isImplicit;
        unsigned alignment;
        serialization::decls_block::AlignmentDeclAttrLayout::readRecord(
            scratch, isImplicit, alignment);
        Attr = new (ctx) AlignmentAttr(alignment, SourceLoc(), SourceRange(),
                                       isImplicit);
        break;
      }

      case decls_block::SwiftNativeObjCRuntimeBase_DECL_ATTR: {
        bool isImplicit;
        IdentifierID nameID;
        serialization::decls_block::SwiftNativeObjCRuntimeBaseDeclAttrLayout
          ::readRecord(scratch, isImplicit, nameID);

        auto name = MF.getIdentifier(nameID);
        Attr = new (ctx) SwiftNativeObjCRuntimeBaseAttr(name, SourceLoc(),
                                                        SourceRange(),
                                                        isImplicit);
        break;
      }

      case decls_block::Semantics_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::SemanticsDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) SemanticsAttr(blobData, isImplicit);
        break;
      }

      case decls_block::Inline_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::InlineDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) InlineAttr((InlineKind)kind);
        break;
      }

      case decls_block::ActorIndependent_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::ActorIndependentDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) ActorIndependentAttr((ActorIndependentKind)kind);
        break;
      }

      case decls_block::Optimize_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::OptimizeDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) OptimizeAttr((OptimizationMode)kind);
        break;
      }

      case decls_block::Effects_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::EffectsDeclAttrLayout::readRecord(scratch,
                                                                      kind);
        Attr = new (ctx) EffectsAttr((EffectsKind)kind);
        break;
      }
      case decls_block::OriginallyDefinedIn_DECL_ATTR: {
        bool isImplicit;
        unsigned Platform;
        DEF_VER_TUPLE_PIECES(MovedVer);
        // Decode the record, pulling the version tuple information.
        serialization::decls_block::OriginallyDefinedInDeclAttrLayout::readRecord(
           scratch,
           isImplicit,
           LIST_VER_TUPLE_PIECES(MovedVer),
           Platform);
        llvm::VersionTuple MovedVer;
        DECODE_VER_TUPLE(MovedVer)
        auto ModuleNameEnd = blobData.find('\0');
        assert(ModuleNameEnd != StringRef::npos);
        auto ModuleName = blobData.slice(0, ModuleNameEnd);
        Attr = new (ctx) OriginallyDefinedInAttr(SourceLoc(), SourceRange(),
                                                 ModuleName,
                                                 (PlatformKind)Platform,
                                                 MovedVer,
                                                 isImplicit);
        break;
      }

      case decls_block::Available_DECL_ATTR: {
        bool isImplicit;
        bool isUnavailable;
        bool isDeprecated;
        bool isPackageDescriptionVersionSpecific;
        DEF_VER_TUPLE_PIECES(Introduced);
        DEF_VER_TUPLE_PIECES(Deprecated);
        DEF_VER_TUPLE_PIECES(Obsoleted);
        unsigned platform, messageSize, renameSize;
        // Decode the record, pulling the version tuple information.
        serialization::decls_block::AvailableDeclAttrLayout::readRecord(
            scratch, isImplicit, isUnavailable, isDeprecated,
            isPackageDescriptionVersionSpecific,
            LIST_VER_TUPLE_PIECES(Introduced),
            LIST_VER_TUPLE_PIECES(Deprecated),
            LIST_VER_TUPLE_PIECES(Obsoleted),
            platform, messageSize, renameSize);

        StringRef message = blobData.substr(0, messageSize);
        blobData = blobData.substr(messageSize);
        StringRef rename = blobData.substr(0, renameSize);
        llvm::VersionTuple Introduced, Deprecated, Obsoleted;
        DECODE_VER_TUPLE(Introduced)
        DECODE_VER_TUPLE(Deprecated)
        DECODE_VER_TUPLE(Obsoleted)

        PlatformAgnosticAvailabilityKind platformAgnostic;
        if (isUnavailable)
          platformAgnostic = PlatformAgnosticAvailabilityKind::Unavailable;
        else if (isDeprecated)
          platformAgnostic = PlatformAgnosticAvailabilityKind::Deprecated;
        else if (((PlatformKind)platform) == PlatformKind::none &&
                 (!Introduced.empty() ||
                  !Deprecated.empty() ||
                  !Obsoleted.empty()))
          platformAgnostic = isPackageDescriptionVersionSpecific ?
            PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
            PlatformAgnosticAvailabilityKind::SwiftVersionSpecific;
        else
          platformAgnostic = PlatformAgnosticAvailabilityKind::None;

        Attr = new (ctx) AvailableAttr(
          SourceLoc(), SourceRange(),
          (PlatformKind)platform, message, rename,
          Introduced, SourceRange(),
          Deprecated, SourceRange(),
          Obsoleted, SourceRange(),
          platformAgnostic, isImplicit);
        break;
      }

      case decls_block::ObjC_DECL_ATTR: {
        bool isImplicit;
        bool isImplicitName;
        bool isSwift3Inferred;
        uint64_t numArgs;
        ArrayRef<uint64_t> rawPieceIDs;
        serialization::decls_block::ObjCDeclAttrLayout::readRecord(
          scratch, isImplicit, isSwift3Inferred, isImplicitName, numArgs,
          rawPieceIDs);

        SmallVector<Identifier, 4> pieces;
        for (auto pieceID : rawPieceIDs)
          pieces.push_back(MF.getIdentifier(pieceID));

        if (numArgs == 0)
          Attr = ObjCAttr::create(ctx, None, isImplicitName);
        else
          Attr = ObjCAttr::create(ctx, ObjCSelector(ctx, numArgs-1, pieces),
                                  isImplicitName);
        Attr->setImplicit(isImplicit);
        cast<ObjCAttr>(Attr)->setSwift3Inferred(isSwift3Inferred);
        break;
      }

      case decls_block::Specialize_DECL_ATTR: {
        unsigned exported;
        SpecializeAttr::SpecializationKind specializationKind;
        unsigned specializationKindVal;
        GenericSignatureID specializedSigID;

        ArrayRef<uint64_t> rawPieceIDs;
        uint64_t numArgs;
        uint64_t numSPIGroups;
        DeclID targetFunID;

        serialization::decls_block::SpecializeDeclAttrLayout::readRecord(
            scratch, exported, specializationKindVal, specializedSigID,
            targetFunID, numArgs, numSPIGroups, rawPieceIDs);

        assert(rawPieceIDs.size() == numArgs + numSPIGroups ||
               rawPieceIDs.size() == (numArgs - 1 + numSPIGroups));
        specializationKind = specializationKindVal
                                 ? SpecializeAttr::SpecializationKind::Partial
                                 : SpecializeAttr::SpecializationKind::Full;
        // The 'target' parameter.
        DeclNameRef replacedFunctionName;
        if (numArgs) {
          bool numArgumentLabels = (numArgs == 1) ? 0 : numArgs - 2;
          auto baseName = MF.getDeclBaseName(rawPieceIDs[0]);
          SmallVector<Identifier, 4> pieces;
          if (numArgumentLabels) {
            for (auto pieceID : rawPieceIDs.slice(1, numArgumentLabels))
              pieces.push_back(MF.getIdentifier(pieceID));
          }
          replacedFunctionName = (numArgs == 1)
                                     ? DeclNameRef({baseName}) // simple name
                                     : DeclNameRef({ctx, baseName, pieces});
        }

        SmallVector<Identifier, 4> spis;
        if (numSPIGroups) {
          auto numTargetFunctionPiecesToSkip =
              (rawPieceIDs.size() == numArgs + numSPIGroups) ? numArgs
                                                             : numArgs - 1;
          for (auto id : rawPieceIDs.slice(numTargetFunctionPiecesToSkip))
            spis.push_back(MF.getIdentifier(id));
        }

        auto specializedSig = MF.getGenericSignature(specializedSigID);
        Attr = SpecializeAttr::create(ctx, exported != 0, specializationKind,
                                      spis, specializedSig,
                                      replacedFunctionName, &MF, targetFunID);
        break;
      }

      case decls_block::DynamicReplacement_DECL_ATTR: {
        bool isImplicit;
        uint64_t numArgs;
        ArrayRef<uint64_t> rawPieceIDs;
        DeclID replacedFunID;
        serialization::decls_block::DynamicReplacementDeclAttrLayout::
            readRecord(scratch, isImplicit, replacedFunID, numArgs, rawPieceIDs);

        auto moduleSelector = MF.getIdentifier(rawPieceIDs[0]);
        auto baseName = MF.getDeclBaseName(rawPieceIDs[1]);
        SmallVector<Identifier, 4> pieces;
        for (auto pieceID : rawPieceIDs.slice(2))
          pieces.push_back(MF.getIdentifier(pieceID));

        DeclNameRef replacedFunName(ctx, moduleSelector, baseName, pieces);

        assert(numArgs != 0);
        assert(!isImplicit && "Need to update for implicit");
        Attr = DynamicReplacementAttr::create(ctx, replacedFunName, &MF,
                                              replacedFunID);
        break;
      }

      case decls_block::TypeEraser_DECL_ATTR: {
        bool isImplicit;
        TypeID typeEraserID;
        serialization::decls_block::TypeEraserDeclAttrLayout::readRecord(
            scratch, isImplicit, typeEraserID);

        assert(!isImplicit);
        Attr = TypeEraserAttr::create(ctx, &MF, typeEraserID);
        break;
      }

      case decls_block::Custom_DECL_ATTR: {
        bool isImplicit;
        TypeID typeID;
        serialization::decls_block::CustomDeclAttrLayout::readRecord(
          scratch, isImplicit, typeID);

        Expected<Type> deserialized = MF.getTypeChecked(typeID);
        if (!deserialized) {
          if (deserialized.errorIsA<XRefNonLoadedModuleError>()) {
            // A custom attribute defined behind an implementation-only import
            // is safe to drop when it can't be deserialized.
            // rdar://problem/56599179
            consumeError(deserialized.takeError());
            skipAttr = true;
          } else
            return deserialized.takeError();
        } else {
          auto *TE = TypeExpr::createImplicit(deserialized.get(), ctx);
          Attr = CustomAttr::create(ctx, SourceLoc(), TE, isImplicit);
        }
        break;
      }

      case decls_block::ProjectedValueProperty_DECL_ATTR: {
        bool isImplicit;
        IdentifierID nameID;
        serialization::decls_block::ProjectedValuePropertyDeclAttrLayout
            ::readRecord(scratch, isImplicit, nameID);

        auto name = MF.getIdentifier(nameID);
        Attr = new (ctx) ProjectedValuePropertyAttr(
            name, SourceLoc(), SourceRange(), isImplicit);
        break;
      }

      case decls_block::Differentiable_DECL_ATTR: {
        bool isImplicit;
        bool linear;
        GenericSignatureID derivativeGenSigId;
        ArrayRef<uint64_t> parameters;

        serialization::decls_block::DifferentiableDeclAttrLayout::readRecord(
            scratch, isImplicit, linear, derivativeGenSigId, parameters);

        auto derivativeGenSig = MF.getGenericSignature(derivativeGenSigId);
        llvm::SmallBitVector parametersBitVector(parameters.size());
        for (unsigned i : indices(parameters))
          parametersBitVector[i] = parameters[i];
        auto *indices = IndexSubset::get(ctx, parametersBitVector);
        auto *diffAttr = DifferentiableAttr::create(
            ctx, isImplicit, SourceLoc(), SourceRange(), linear,
            /*parsedParameters*/ {}, /*trailingWhereClause*/ nullptr);

        // Cache parameter indices so that they can set later.
        // `DifferentiableAttr::setParameterIndices` cannot be called here
        // because it requires `DifferentiableAttr::setOriginalDeclaration` to
        // be called first. `DifferentiableAttr::setOriginalDeclaration` cannot
        // be called here because the original declaration is not accessible in
        // this function (`DeclDeserializer::deserializeDeclAttributes`).
        diffAttrParamIndicesMap[diffAttr] = indices;
        diffAttr->setDerivativeGenericSignature(derivativeGenSig);
        Attr = diffAttr;
        break;
      }

      case decls_block::Derivative_DECL_ATTR: {
        bool isImplicit;
        uint64_t origModuleSelectorId;
        uint64_t origNameId;
        bool hasAccessorKind;
        uint64_t rawAccessorKind;
        DeclID origDeclId;
        uint64_t rawDerivativeKind;
        ArrayRef<uint64_t> parameters;

        serialization::decls_block::DerivativeDeclAttrLayout::readRecord(
            scratch, isImplicit, origModuleSelectorId, origNameId,
            hasAccessorKind, rawAccessorKind, origDeclId, rawDerivativeKind,
            parameters);

        Optional<AccessorKind> accessorKind = None;
        if (hasAccessorKind) {
          auto maybeAccessorKind = getActualAccessorKind(rawAccessorKind);
          if (!maybeAccessorKind)
            MF.fatal();
          accessorKind = *maybeAccessorKind;
        }

        Identifier origModuleSelector = MF.getIdentifier(origModuleSelectorId);
        DeclBaseName origBaseName = MF.getDeclBaseName(origNameId);
        DeclNameRef origName(ctx, origModuleSelector, origBaseName);
        DeclNameRefWithLoc origNameWithLoc{origName, DeclNameLoc(),
                                           accessorKind};

        auto derivativeKind =
            getActualAutoDiffDerivativeFunctionKind(rawDerivativeKind);
        if (!derivativeKind)
          MF.fatal();
        llvm::SmallBitVector parametersBitVector(parameters.size());
        for (unsigned i : indices(parameters))
          parametersBitVector[i] = parameters[i];
        auto *indices = IndexSubset::get(ctx, parametersBitVector);

        auto *derivativeAttr =
            DerivativeAttr::create(ctx, isImplicit, SourceLoc(), SourceRange(),
                                   /*baseType*/ nullptr, origNameWithLoc,
                                   indices);
        derivativeAttr->setOriginalFunctionResolver(&MF, origDeclId);
        derivativeAttr->setDerivativeKind(*derivativeKind);
        Attr = derivativeAttr;
        break;
      }

      case decls_block::Transpose_DECL_ATTR: {
        bool isImplicit;
        uint64_t origNameId;
        DeclID origDeclId;
        ArrayRef<uint64_t> parameters;

        serialization::decls_block::TransposeDeclAttrLayout::readRecord(
            scratch, isImplicit, origNameId, origDeclId, parameters);

        DeclNameRefWithLoc origName{
            DeclNameRef(MF.getDeclBaseName(origNameId)), DeclNameLoc(), None};
        auto *origDecl = cast<AbstractFunctionDecl>(MF.getDecl(origDeclId));
        llvm::SmallBitVector parametersBitVector(parameters.size());
        for (unsigned i : indices(parameters))
          parametersBitVector[i] = parameters[i];
        auto *indices = IndexSubset::get(ctx, parametersBitVector);
        auto *transposeAttr =
            TransposeAttr::create(ctx, isImplicit, SourceLoc(), SourceRange(),
                                  /*baseTypeRepr*/ nullptr, origName, indices);
        transposeAttr->setOriginalFunction(origDecl);
        Attr = transposeAttr;
        break;
      }

      case decls_block::SPIAccessControl_DECL_ATTR: {
        ArrayRef<uint64_t> spiIds;
        serialization::decls_block::SPIAccessControlDeclAttrLayout::readRecord(
                                                               scratch, spiIds);

        SmallVector<Identifier, 4> spis;
        for (auto id : spiIds)
          spis.push_back(MF.getIdentifier(id));

        Attr = SPIAccessControlAttr::create(ctx, SourceLoc(),
                                            SourceRange(), spis);
        break;
      }

#define SIMPLE_DECL_ATTR(NAME, CLASS, ...) \
      case decls_block::CLASS##_DECL_ATTR: { \
        bool isImplicit; \
        serialization::decls_block::CLASS##DeclAttrLayout::readRecord( \
            scratch, isImplicit); \
        Attr = new (ctx) CLASS##Attr(isImplicit); \
        break; \
      }
#include "swift/AST/Attr.def"

      default:
        // We don't know how to deserialize this kind of attribute.
        MF.fatal();
      }

      if (!skipAttr) {
        if (!Attr)
          return llvm::Error::success();

        AddAttribute(Attr);
      }

    } else if (recordID == decls_block::PRIVATE_DISCRIMINATOR) {
      IdentifierID discriminatorID;
      decls_block::PrivateDiscriminatorLayout::readRecord(scratch,
                                                          discriminatorID);
      privateDiscriminator = MF.getIdentifier(discriminatorID);

    } else if (recordID == decls_block::LOCAL_DISCRIMINATOR) {
      unsigned discriminator;
      decls_block::LocalDiscriminatorLayout::readRecord(scratch, discriminator);
      localDiscriminator = discriminator;
    } else if (recordID == decls_block::FILENAME_FOR_PRIVATE) {
      IdentifierID filenameID;
      decls_block::FilenameForPrivateLayout::readRecord(scratch, filenameID);
      filenameForPrivate = MF.getIdentifierText(filenameID);
    } else {
      return llvm::Error::success();
    }

    // Prepare to read the next record.
    restoreOffset.cancel();
    scratch.clear();
  }
}

Expected<Decl *>
DeclDeserializer::getDeclCheckedImpl(
  llvm::function_ref<bool(DeclAttributes)> matchAttributes) {

  auto attrError = deserializeDeclAttributes();
  if (attrError)
    return std::move(attrError);

  if (matchAttributes) {
    // Deserialize the full decl only if matchAttributes finds a match.
    DeclAttributes attrs = DeclAttributes();
    attrs.setRawAttributeChain(DAttrs);
    if (!matchAttributes(attrs))
      return llvm::make_error<DeclAttributesDidNotMatch>();
  }

  if (auto s = ctx.Stats)
    ++s->getFrontendCounters().NumDeclsDeserialized;

  // FIXME: @_dynamicReplacement(for:) includes a reference to another decl,
  // usually in the same type, and that can result in this decl being
  // re-entrantly deserialized. If that happens, don't fail here.
  if (declOrOffset.isComplete())
    return declOrOffset;

  llvm::BitstreamEntry entry =
      MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());
  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize decls represented by sub-blocks.
    MF.fatal();
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = MF.fatalIfUnexpected(
      MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

  PrettyDeclDeserialization stackTraceEntry(
     &MF, declOrOffset, static_cast<decls_block::RecordKind>(recordID));

  switch (recordID) {
#define CASE(RECORD_NAME) \
  case decls_block::RECORD_NAME##Layout::Code: {\
    auto decl = deserialize##RECORD_NAME(scratch, blobData); \
    if (decl) { \
      /* \
      // Set original declaration and parameter indices in `@differentiable` \
      // attributes. \
      */ \
      setOriginalDeclarationAndParameterIndicesInDifferentiableAttributes(\
          decl.get(), DAttrs, diffAttrParamIndicesMap); \
    } \
    return decl; \
  }

  CASE(TypeAlias)
  CASE(GenericTypeParamDecl)
  CASE(AssociatedTypeDecl)
  CASE(Struct)
  CASE(Constructor)
  CASE(Var)
  CASE(Param)
  CASE(Func)
  CASE(OpaqueType)
  CASE(Accessor)
  CASE(PatternBinding)
  CASE(Protocol)
  CASE(PrefixOperator)
  CASE(PostfixOperator)
  CASE(InfixOperator)
  CASE(PrecedenceGroup)
  CASE(Class)
  CASE(Enum)
  CASE(EnumElement)
  CASE(Subscript)
  CASE(Extension)
  CASE(Destructor)
#undef CASE

  case decls_block::XREF: {
    assert(DAttrs == nullptr);
    ModuleID baseModuleID;
    uint32_t pathLen;
    decls_block::XRefLayout::readRecord(scratch, baseModuleID, pathLen);
    auto resolved = MF.resolveCrossReference(baseModuleID, pathLen);
    if (resolved)
      declOrOffset = resolved.get();
    return resolved;
  }
  
  default:
    // We don't know how to deserialize this kind of decl.
    MF.fatal();
  }
}

/// Translate from the Serialization function type repr enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::FunctionType::Representation>
getActualFunctionTypeRepresentation(uint8_t rep) {
  switch (rep) {
#define CASE(THE_CC) \
  case (uint8_t)serialization::FunctionTypeRepresentation::THE_CC: \
    return swift::FunctionType::Representation::THE_CC;
  CASE(Swift)
  CASE(Block)
  CASE(Thin)
  CASE(CFunctionPointer)
#undef CASE
  default:
    return None;
  }
}

/// Translate from the Serialization differentiability kind enum values to the
/// AST strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::DifferentiabilityKind>
getActualDifferentiabilityKind(uint8_t rep) {
  switch (rep) {
#define CASE(THE_CC) \
  case (uint8_t)serialization::DifferentiabilityKind::THE_CC: \
    return swift::DifferentiabilityKind::THE_CC;
  CASE(NonDifferentiable)
  CASE(Normal)
  CASE(Linear)
#undef CASE
  default:
    return None;
  }
}

/// Translate from the Serialization function type repr enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::SILFunctionType::Representation>
getActualSILFunctionTypeRepresentation(uint8_t rep) {
  switch (rep) {
#define CASE(THE_CC) \
  case (uint8_t)serialization::SILFunctionTypeRepresentation::THE_CC: \
    return swift::SILFunctionType::Representation::THE_CC;
  CASE(Thick)
  CASE(Block)
  CASE(Thin)
  CASE(CFunctionPointer)
  CASE(Method)
  CASE(ObjCMethod)
  CASE(WitnessMethod)
#undef CASE
  default:
    return None;
  }
}

/// Translate from the Serialization coroutine kind enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::SILCoroutineKind>
getActualSILCoroutineKind(uint8_t rep) {
  switch (rep) {
#define CASE(KIND) \
  case (uint8_t)serialization::SILCoroutineKind::KIND: \
    return swift::SILCoroutineKind::KIND;
  CASE(None)
  CASE(YieldOnce)
  CASE(YieldMany)
#undef CASE
  default:
    return None;
  }
}

/// Translate from the serialization ReferenceOwnership enumerators, which are
/// guaranteed to be stable, to the AST ones.
static Optional<swift::ReferenceOwnership>
getActualReferenceOwnership(serialization::ReferenceOwnership raw) {
  switch (raw) {
  case serialization::ReferenceOwnership::Strong:
    return swift::ReferenceOwnership::Strong;
#define REF_STORAGE(Name, ...) \
  case serialization::ReferenceOwnership::Name: \
    return swift::ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
  }
  return None;
}

/// Translate from the serialization ValueOwnership enumerators, which are
/// guaranteed to be stable, to the AST ones.
static Optional<swift::ValueOwnership>
getActualValueOwnership(serialization::ValueOwnership raw) {
  switch (raw) {
#define CASE(ID) \
  case serialization::ValueOwnership::ID: \
    return swift::ValueOwnership::ID;
  CASE(Default)
  CASE(InOut)
  CASE(Shared)
  CASE(Owned)
#undef CASE
  }
  return None;
}

/// Translate from the serialization ParameterConvention enumerators,
/// which are guaranteed to be stable, to the AST ones.
static
Optional<swift::ParameterConvention> getActualParameterConvention(uint8_t raw) {
  switch (serialization::ParameterConvention(raw)) {
#define CASE(ID) \
  case serialization::ParameterConvention::ID: \
    return swift::ParameterConvention::ID;
  CASE(Indirect_In)
  CASE(Indirect_Inout)
  CASE(Indirect_InoutAliasable)
  CASE(Indirect_In_Guaranteed)
  CASE(Indirect_In_Constant)
  CASE(Direct_Owned)
  CASE(Direct_Unowned)
  CASE(Direct_Guaranteed)
#undef CASE
  }
  return None;
}

/// Translate from the serialization SILParameterDifferentiability enumerators,
/// which are guaranteed to be stable, to the AST ones.
static Optional<swift::SILParameterDifferentiability>
getActualSILParameterDifferentiability(uint8_t raw) {
  switch (serialization::SILParameterDifferentiability(raw)) {
#define CASE(ID)                                                               \
  case serialization::SILParameterDifferentiability::ID:                       \
    return swift::SILParameterDifferentiability::ID;
  CASE(DifferentiableOrNotApplicable)
  CASE(NotDifferentiable)
#undef CASE
  }
  return None;
}

/// Translate from the serialization ResultConvention enumerators,
/// which are guaranteed to be stable, to the AST ones.
static
Optional<swift::ResultConvention> getActualResultConvention(uint8_t raw) {
  switch (serialization::ResultConvention(raw)) {
#define CASE(ID) \
  case serialization::ResultConvention::ID: return swift::ResultConvention::ID;
  CASE(Indirect)
  CASE(Owned)
  CASE(Unowned)
  CASE(UnownedInnerPointer)
  CASE(Autoreleased)
#undef CASE
  }
  return None;
}

/// Translate from the serialization SILResultDifferentiability enumerators,
/// which are guaranteed to be stable, to the AST ones.
static Optional<swift::SILResultDifferentiability>
getActualSILResultDifferentiability(uint8_t raw) {
  switch (serialization::SILResultDifferentiability(raw)) {
#define CASE(ID)                                                               \
  case serialization::SILResultDifferentiability::ID:                          \
    return swift::SILResultDifferentiability::ID;
    CASE(DifferentiableOrNotApplicable)
    CASE(NotDifferentiable)
#undef CASE
  }
  return None;
}

Type ModuleFile::getType(TypeID TID) {
  Expected<Type> deserialized = getTypeChecked(TID);
  if (!deserialized) {
    fatal(deserialized.takeError());
  }
  return deserialized.get();
}

namespace swift {
class TypeDeserializer {
  using TypeID = serialization::TypeID;

  ModuleFile &MF;
  ASTContext &ctx;
public:
  explicit TypeDeserializer(ModuleFile &MF)
      : MF(MF), ctx(MF.getContext()) {}

  Expected<Type> getTypeCheckedImpl();

  Expected<Type> deserializeBuiltinAliasType(ArrayRef<uint64_t> scratch,
                                             StringRef blobData) {
    DeclID underlyingID;
    TypeID canonicalTypeID;
    decls_block::BuiltinAliasTypeLayout::readRecord(scratch, underlyingID,
                                                    canonicalTypeID);
    auto aliasOrError = MF.getDeclChecked(underlyingID);
    if (!aliasOrError)
      return aliasOrError.takeError();
    auto alias = dyn_cast<TypeAliasDecl>(aliasOrError.get());

    if (ctx.LangOpts.EnableDeserializationRecovery) {
      Expected<Type> expectedType = MF.getTypeChecked(canonicalTypeID);
      if (!expectedType)
        return expectedType.takeError();
      if (expectedType.get()) {
        if (!alias ||
            !alias->getDeclaredInterfaceType()->isEqual(expectedType.get())) {
          // Fall back to the canonical type.
          return expectedType.get();
        }
      }
    }

    // Look through compatibility aliases that are now unavailable.
    if (alias->getAttrs().isUnavailable(ctx) &&
        alias->isCompatibilityAlias()) {
      return alias->getUnderlyingType();
    }

    return alias->getDeclaredInterfaceType();
  }

  Expected<Type> deserializeTypeAliasType(ArrayRef<uint64_t> scratch,
                                          StringRef blobData) {
    DeclID typealiasID;
    TypeID parentTypeID;
    TypeID underlyingTypeID;
    TypeID substitutedTypeID;
    SubstitutionMapID substitutionsID;
    decls_block::TypeAliasTypeLayout::readRecord(scratch, typealiasID,
                                                 parentTypeID,
                                                 underlyingTypeID,
                                                 substitutedTypeID,
                                                 substitutionsID);

    TypeAliasDecl *alias = nullptr;
    Type underlyingType;
    if (ctx.LangOpts.EnableDeserializationRecovery) {
      auto underlyingTypeOrError = MF.getTypeChecked(underlyingTypeID);
      if (!underlyingTypeOrError) {
        // If we can't deserialize the underlying type, we can't be sure the
        // actual typealias hasn't changed.
        return underlyingTypeOrError.takeError();
      }

      underlyingType = underlyingTypeOrError.get();

      if (auto aliasOrError = MF.getDeclChecked(typealiasID)) {
        alias = dyn_cast<TypeAliasDecl>(aliasOrError.get());
      } else {
        // We're going to recover by falling back to the underlying type, so
        // just ignore the error.
        llvm::consumeError(aliasOrError.takeError());
      }

      if (!alias ||
          !alias->getDeclaredInterfaceType()->isEqual(underlyingType)) {
        // Fall back to the canonical type.
        return underlyingType;
      }

    } else {
      alias = dyn_cast<TypeAliasDecl>(MF.getDecl(typealiasID));
      underlyingType = MF.getType(underlyingTypeID);
    }

    // Read the substituted type.
    auto substitutedTypeOrError = MF.getTypeChecked(substitutedTypeID);
    if (!substitutedTypeOrError)
      return substitutedTypeOrError.takeError();

    auto substitutedType = substitutedTypeOrError.get();

    // Read the substitutions.
    auto subMapOrError = MF.getSubstitutionMapChecked(substitutionsID);
    if (!subMapOrError)
      return subMapOrError.takeError();

    auto parentTypeOrError = MF.getTypeChecked(parentTypeID);
    if (!parentTypeOrError)
      return underlyingType;

    // Look through compatibility aliases that are now unavailable.
    if (alias &&
        alias->getAttrs().isUnavailable(ctx) &&
        alias->isCompatibilityAlias()) {
      return alias->getUnderlyingType().subst(subMapOrError.get());
    }

    auto parentType = parentTypeOrError.get();
    return TypeAliasType::get(alias, parentType, subMapOrError.get(),
                              substitutedType);
  }

  Expected<Type> deserializeNominalType(ArrayRef<uint64_t> scratch,
                                        StringRef blobData) {
    DeclID declID;
    TypeID parentID;
    decls_block::NominalTypeLayout::readRecord(scratch, declID, parentID);

    Expected<Type> parentTy = MF.getTypeChecked(parentID);
    if (!parentTy)
      return parentTy.takeError();

    auto nominalOrError = MF.getDeclChecked(declID);
    if (!nominalOrError)
      return nominalOrError.takeError();

    // Look through compatibility aliases.
    if (auto *alias = dyn_cast<TypeAliasDecl>(nominalOrError.get())) {
      // Reminder: TypeBase::getAs will look through sugar. But we don't want to
      // do that here, so we do isa<> checks on the TypeBase itself instead of
      // using the Type wrapper.
      const TypeBase *underlyingTy = nullptr;
      while (alias->isCompatibilityAlias()) {
        underlyingTy = alias->getUnderlyingType().getPointer();

        // If the underlying type is itself a typealias, it might be another
        // compatibility alias, meaning we need to go around the loop again.
        auto aliasTy = dyn_cast<TypeAliasType>(underlyingTy);
        if (!aliasTy)
          break;
        alias = aliasTy->getDecl();
      }

      // We only want to use the type we found if it's a simple non-generic
      // nominal type.
      if (auto simpleNominalTy = dyn_cast_or_null<NominalType>(underlyingTy)) {
        nominalOrError = simpleNominalTy->getDecl();
        (void)!nominalOrError; // "Check" the llvm::Expected<> value.
      }
    }

    auto nominal = dyn_cast<NominalTypeDecl>(nominalOrError.get());
    if (!nominal) {
      XRefTracePath tinyTrace{*nominalOrError.get()->getModuleContext()};
      const DeclName fullName =
          cast<ValueDecl>(nominalOrError.get())->getName();
      tinyTrace.addValue(fullName.getBaseIdentifier());
      return llvm::make_error<XRefError>("declaration is not a nominal type",
                                         tinyTrace, fullName);
    }
    return NominalType::get(nominal, parentTy.get(), ctx);
  }

  Expected<Type> deserializeParenType(ArrayRef<uint64_t> scratch,
                                      StringRef blobData) {
    TypeID underlyingID;
    decls_block::ParenTypeLayout::readRecord(scratch, underlyingID);

    auto underlyingTy = MF.getTypeChecked(underlyingID);
    if (!underlyingTy)
      return underlyingTy.takeError();

    return ParenType::get(ctx, underlyingTy.get());
  }

  Expected<Type> deserializeTupleType(SmallVectorImpl<uint64_t> &scratch,
                                      StringRef blobData) {
    // The tuple record itself is empty. Read all trailing elements.
    SmallVector<TupleTypeElt, 8> elements;
    while (true) {
      llvm::BitstreamEntry entry =
          MF.fatalIfUnexpected(MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();
      unsigned recordID = MF.fatalIfUnexpected(
          MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
      if (recordID != decls_block::TUPLE_TYPE_ELT)
        break;

      IdentifierID nameID;
      TypeID typeID;
      decls_block::TupleTypeEltLayout::readRecord(scratch, nameID, typeID);

      auto elementTy = MF.getTypeChecked(typeID);
      if (!elementTy)
        return elementTy.takeError();

      elements.emplace_back(elementTy.get(), MF.getIdentifier(nameID));
    }

    return TupleType::get(elements, ctx);
  }

  Expected<Type> deserializeAnyFunctionType(SmallVectorImpl<uint64_t> &scratch,
                                            StringRef blobData,
                                            bool isGeneric) {
    TypeID resultID;
    uint8_t rawRepresentation, rawDiffKind;
    bool noescape = false, async, throws;
    GenericSignature genericSig;
    TypeID clangTypeID;

    if (!isGeneric) {
      decls_block::FunctionTypeLayout::readRecord(
          scratch, resultID, rawRepresentation, clangTypeID,
          noescape, async, throws, rawDiffKind);
    } else {
      GenericSignatureID rawGenericSig;
      decls_block::GenericFunctionTypeLayout::readRecord(
          scratch, resultID, rawRepresentation, async, throws, rawDiffKind,
          rawGenericSig);
      genericSig = MF.getGenericSignature(rawGenericSig);
      clangTypeID = 0;
    }

    auto representation = getActualFunctionTypeRepresentation(rawRepresentation);
    if (!representation.hasValue())
      MF.fatal();

    auto diffKind = getActualDifferentiabilityKind(rawDiffKind);
    if (!diffKind.hasValue())
      MF.fatal();

    const clang::Type *clangFunctionType = nullptr;
    if (clangTypeID) {
      auto loadedClangType = MF.getClangType(clangTypeID);
      if (!loadedClangType)
        return loadedClangType.takeError();
      clangFunctionType = loadedClangType.get();
    }

    auto info = FunctionType::ExtInfoBuilder(*representation, noescape, throws,
                                             *diffKind, clangFunctionType)
                    .withAsync(async)
                    .build();

    auto resultTy = MF.getTypeChecked(resultID);
    if (!resultTy)
      return resultTy.takeError();

    SmallVector<AnyFunctionType::Param, 8> params;
    while (true) {
      llvm::BitstreamEntry entry =
          MF.fatalIfUnexpected(MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();
      unsigned recordID = MF.fatalIfUnexpected(
          MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
      if (recordID != decls_block::FUNCTION_PARAM)
        break;

      IdentifierID labelID;
      TypeID typeID;
      bool isVariadic, isAutoClosure, isNonEphemeral, isNoDerivative;
      unsigned rawOwnership;
      decls_block::FunctionParamLayout::readRecord(
          scratch, labelID, typeID, isVariadic, isAutoClosure, isNonEphemeral,
          rawOwnership, isNoDerivative);

      auto ownership =
          getActualValueOwnership((serialization::ValueOwnership)rawOwnership);
      if (!ownership)
        MF.fatal();

      auto paramTy = MF.getTypeChecked(typeID);
      if (!paramTy)
        return paramTy.takeError();

      params.emplace_back(paramTy.get(), MF.getIdentifier(labelID),
                          ParameterTypeFlags(isVariadic, isAutoClosure,
                                             isNonEphemeral, *ownership,
                                             isNoDerivative));
    }

    if (!isGeneric) {
      assert(genericSig.isNull());
      return FunctionType::get(params, resultTy.get(), info);
    }

    assert(!genericSig.isNull());
    return GenericFunctionType::get(genericSig, params, resultTy.get(), info);
  }

  Expected<Type> deserializeFunctionType(SmallVectorImpl<uint64_t> &scratch,
                                         StringRef blobData) {
    return deserializeAnyFunctionType(scratch, blobData, /*isGeneric*/false);
  }

  Expected<Type>
  deserializeGenericFunctionType(SmallVectorImpl<uint64_t> &scratch,
                                 StringRef blobData) {
    return deserializeAnyFunctionType(scratch, blobData, /*isGeneric*/true);
  }

  template <typename Layout, typename ASTType, bool CanBeThin>
  Expected<Type> deserializeAnyMetatypeType(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    TypeID instanceID;
    uint8_t repr;
    Layout::readRecord(scratch, instanceID, repr);

    auto instanceType = MF.getTypeChecked(instanceID);
    if (!instanceType)
      return instanceType.takeError();

    switch (repr) {
    case serialization::MetatypeRepresentation::MR_None:
      return ASTType::get(instanceType.get());

    case serialization::MetatypeRepresentation::MR_Thin:
      if (!CanBeThin)
        MF.fatal();
      return ASTType::get(instanceType.get(),
                          MetatypeRepresentation::Thin);

    case serialization::MetatypeRepresentation::MR_Thick:
      return ASTType::get(instanceType.get(),
                          MetatypeRepresentation::Thick);

    case serialization::MetatypeRepresentation::MR_ObjC:
      return ASTType::get(instanceType.get(),
                          MetatypeRepresentation::ObjC);

    default:
      MF.fatal();
    }
  }

  Expected<Type>
  deserializeExistentialMetatypeType(ArrayRef<uint64_t> scratch,
                                     StringRef blobData) {
    return
        deserializeAnyMetatypeType<decls_block::ExistentialMetatypeTypeLayout,
                                   ExistentialMetatypeType, /*CanBeThin*/false>(
        scratch, blobData);
  }

  Expected<Type> deserializeMetatypeType(ArrayRef<uint64_t> scratch,
                                         StringRef blobData) {
    return deserializeAnyMetatypeType<decls_block::MetatypeTypeLayout,
                                      MetatypeType, /*CanBeThin*/true>(
        scratch, blobData);
  }

  Expected<Type> deserializeDynamicSelfType(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    TypeID selfID;
    decls_block::DynamicSelfTypeLayout::readRecord(scratch, selfID);
    return DynamicSelfType::get(MF.getType(selfID), ctx);
  }

  Expected<Type> deserializeReferenceStorageType(ArrayRef<uint64_t> scratch,
                                                 StringRef blobData) {
    uint8_t rawOwnership;
    TypeID objectTypeID;
    decls_block::ReferenceStorageTypeLayout::readRecord(scratch, rawOwnership,
                                                        objectTypeID);

    auto ownership = getActualReferenceOwnership(
        (serialization::ReferenceOwnership)rawOwnership);
    if (!ownership.hasValue())
      MF.fatal();

    auto objectTy = MF.getTypeChecked(objectTypeID);
    if (!objectTy)
      return objectTy.takeError();

    return ReferenceStorageType::get(objectTy.get(), ownership.getValue(), ctx);
  }

  Expected<Type> deserializePrimaryArchetypeType(ArrayRef<uint64_t> scratch,
                                                 StringRef blobData) {
    GenericSignatureID sigID;
    unsigned depth, index;

    decls_block::PrimaryArchetypeTypeLayout::readRecord(scratch, sigID,
                                                        depth, index);

    auto sig = MF.getGenericSignature(sigID);
    if (!sig)
      MF.fatal();

    Type interfaceType = GenericTypeParamType::get(depth, index, ctx);
    Type contextType = sig->getGenericEnvironment()
        ->mapTypeIntoContext(interfaceType);

    if (contextType->hasError())
      MF.fatal();

    return contextType;
  }

  Expected<Type> deserializeOpenedArchetypeType(ArrayRef<uint64_t> scratch,
                                                StringRef blobData) {
    TypeID existentialID;

    decls_block::OpenedArchetypeTypeLayout::readRecord(scratch,
                                                       existentialID);

    return OpenedArchetypeType::get(MF.getType(existentialID));
  }
      
  Expected<Type> deserializeOpaqueArchetypeType(ArrayRef<uint64_t> scratch,
                                                StringRef blobData) {
    DeclID opaqueDeclID;
    SubstitutionMapID subsID;
    decls_block::OpaqueArchetypeTypeLayout::readRecord(scratch,
                                                       opaqueDeclID, subsID);

    auto opaqueTypeOrError = MF.getDeclChecked(opaqueDeclID);
    if (!opaqueTypeOrError)
      return opaqueTypeOrError.takeError();

    auto opaqueDecl = cast<OpaqueTypeDecl>(opaqueTypeOrError.get());
    auto subsOrError = MF.getSubstitutionMapChecked(subsID);
    if (!subsOrError)
      return subsOrError.takeError();

    return OpaqueTypeArchetypeType::get(opaqueDecl, subsOrError.get());
  }
      
  Expected<Type> deserializeNestedArchetypeType(ArrayRef<uint64_t> scratch,
                                                StringRef blobData) {
    TypeID rootID, interfaceTyID;
    decls_block::NestedArchetypeTypeLayout::readRecord(scratch,
                                                       rootID, interfaceTyID);
    
    auto rootTy = MF.getType(rootID)->castTo<ArchetypeType>();
    auto interfaceTy = MF.getType(interfaceTyID)->castTo<DependentMemberType>();
    return rootTy->getGenericEnvironment()->mapTypeIntoContext(interfaceTy);
  }

  Expected<Type> deserializeGenericTypeParamType(ArrayRef<uint64_t> scratch,
                                                 StringRef blobData) {
    DeclID declIDOrDepth;
    unsigned indexPlusOne;

    decls_block::GenericTypeParamTypeLayout::readRecord(scratch, declIDOrDepth,
                                                        indexPlusOne);

    if (indexPlusOne == 0) {
      auto genericParam
        = dyn_cast_or_null<GenericTypeParamDecl>(MF.getDecl(declIDOrDepth));

      if (!genericParam)
        MF.fatal();

      return genericParam->getDeclaredInterfaceType();
    }

    return GenericTypeParamType::get(declIDOrDepth,indexPlusOne-1,ctx);
  }

  Expected<Type> deserializeProtocolCompositionType(ArrayRef<uint64_t> scratch,
                                                    StringRef blobData) {
    bool hasExplicitAnyObject;
    ArrayRef<uint64_t> rawProtocolIDs;

    decls_block::ProtocolCompositionTypeLayout::readRecord(scratch,
                                                           hasExplicitAnyObject,
                                                           rawProtocolIDs);
    SmallVector<Type, 4> protocols;
    for (TypeID protoID : rawProtocolIDs) {
      auto protoTy = MF.getTypeChecked(protoID);
      if (!protoTy)
        return protoTy.takeError();
      protocols.push_back(protoTy.get());
    }

    return ProtocolCompositionType::get(ctx, protocols, hasExplicitAnyObject);
  }

  Expected<Type> deserializeDependentMemberType(ArrayRef<uint64_t> scratch,
                                                StringRef blobData) {
    TypeID baseID;
    DeclID assocTypeID;

    decls_block::DependentMemberTypeLayout::readRecord(scratch, baseID,
                                                       assocTypeID);
    auto assocType = MF.getDeclChecked(assocTypeID);
    if (!assocType)
      return assocType.takeError();

    return DependentMemberType::get(
        MF.getType(baseID),
        cast<AssociatedTypeDecl>(assocType.get()));
  }

  Expected<Type> deserializeBoundGenericType(ArrayRef<uint64_t> scratch,
                                             StringRef blobData) {
    DeclID declID;
    TypeID parentID;
    ArrayRef<uint64_t> rawArgumentIDs;

    decls_block::BoundGenericTypeLayout::readRecord(scratch, declID, parentID,
                                                    rawArgumentIDs);

    auto nominalOrError = MF.getDeclChecked(declID);
    if (!nominalOrError)
      return nominalOrError.takeError();
    auto nominal = cast<NominalTypeDecl>(nominalOrError.get());

    // FIXME: Check this?
    auto parentTy = MF.getType(parentID);

    SmallVector<Type, 8> genericArgs;
    for (TypeID ID : rawArgumentIDs) {
      auto argTy = MF.getTypeChecked(ID);
      if (!argTy)
        return argTy.takeError();

      genericArgs.push_back(argTy.get());
    }

    return BoundGenericType::get(nominal, parentTy, genericArgs);
  }

  Expected<Type> deserializeSILBlockStorageType(ArrayRef<uint64_t> scratch,
                                                StringRef blobData) {
    TypeID captureID;
    decls_block::SILBlockStorageTypeLayout::readRecord(scratch, captureID);
    return SILBlockStorageType::get(MF.getType(captureID)->getCanonicalType());
  }

  Expected<Type> deserializeSILBoxType(ArrayRef<uint64_t> scratch,
                                       StringRef blobData) {
    SILLayoutID layoutID;
    SubstitutionMapID subMapID;
    decls_block::SILBoxTypeLayout::readRecord(scratch, layoutID, subMapID);

    // Get the layout.
    auto getLayout = [this](SILLayoutID layoutID) -> SILLayout * {
      assert(layoutID > 0 && layoutID <= MF.SILLayouts.size()
             && "invalid layout ID");

      auto &layoutOrOffset = MF.SILLayouts[layoutID - 1];
      if (layoutOrOffset.isComplete()) {
        return layoutOrOffset;
      }

      BCOffsetRAII saveOffset(MF.DeclTypeCursor);
      MF.fatalIfNotSuccess(MF.DeclTypeCursor.JumpToBit(layoutOrOffset));
      auto layout = MF.readSILLayout(MF.DeclTypeCursor);
      if (!layout)
        MF.fatal();
      layoutOrOffset = layout;
      return layout;
    };

    auto layout = getLayout(layoutID);
    if (!layout)
      return nullptr;

    auto subMapOrError = MF.getSubstitutionMapChecked(subMapID);
    if (!subMapOrError)
      return subMapOrError.takeError();

    return SILBoxType::get(ctx, layout, subMapOrError.get());
  }

  Expected<Type> deserializeSILFunctionType(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    bool async;
    uint8_t rawCoroutineKind;
    uint8_t rawCalleeConvention;
    uint8_t rawRepresentation;
    uint8_t rawDiffKind;
    bool pseudogeneric = false;
    bool noescape;
    bool hasErrorResult;
    unsigned numParams;
    unsigned numYields;
    unsigned numResults;
    GenericSignatureID rawInvocationGenericSig;
    SubstitutionMapID rawInvocationSubs;
    SubstitutionMapID rawPatternSubs;
    ArrayRef<uint64_t> variableData;
    ClangTypeID clangFunctionTypeID;

    decls_block::SILFunctionTypeLayout::readRecord(scratch,
                                             async,
                                             rawCoroutineKind,
                                             rawCalleeConvention,
                                             rawRepresentation,
                                             pseudogeneric,
                                             noescape,
                                             rawDiffKind,
                                             hasErrorResult,
                                             numParams,
                                             numYields,
                                             numResults,
                                             rawInvocationGenericSig,
                                             rawInvocationSubs,
                                             rawPatternSubs,
                                             clangFunctionTypeID,
                                             variableData);

    // Process the ExtInfo.
    auto representation
      = getActualSILFunctionTypeRepresentation(rawRepresentation);
    if (!representation.hasValue())
      MF.fatal();

    auto diffKind = getActualDifferentiabilityKind(rawDiffKind);
    if (!diffKind.hasValue())
      MF.fatal();

    const clang::Type *clangFunctionType = nullptr;
    if (clangFunctionTypeID) {
      auto clangType = MF.getClangType(clangFunctionTypeID);
      if (!clangType)
        return clangType.takeError();
      clangFunctionType = clangType.get();
    }

    auto extInfo =
        SILFunctionType::ExtInfoBuilder(*representation, pseudogeneric,
                                        noescape, async, *diffKind, 
                                        clangFunctionType)
            .build();

    // Process the coroutine kind.
    auto coroutineKind = getActualSILCoroutineKind(rawCoroutineKind);
    if (!coroutineKind.hasValue())
      MF.fatal();

    // Process the callee convention.
    auto calleeConvention = getActualParameterConvention(rawCalleeConvention);
    if (!calleeConvention.hasValue())
      MF.fatal();

    auto processParameter =
        [&](TypeID typeID, uint64_t rawConvention,
            uint64_t rawDifferentiability) -> llvm::Expected<SILParameterInfo> {
      auto convention = getActualParameterConvention(rawConvention);
      if (!convention)
        MF.fatal();
      auto type = MF.getTypeChecked(typeID);
      if (!type)
        return type.takeError();
      auto differentiability =
          swift::SILParameterDifferentiability::DifferentiableOrNotApplicable;
      if (diffKind != DifferentiabilityKind::NonDifferentiable) {
        auto differentiabilityOpt =
            getActualSILParameterDifferentiability(rawDifferentiability);
        if (!differentiabilityOpt)
          MF.fatal();
        differentiability = *differentiabilityOpt;
      }
      return SILParameterInfo(type.get()->getCanonicalType(), *convention,
                              differentiability);
    };

    auto processYield = [&](TypeID typeID, uint64_t rawConvention)
                                  -> llvm::Expected<SILYieldInfo> {
      auto convention = getActualParameterConvention(rawConvention);
      if (!convention)
        MF.fatal();
      auto type = MF.getTypeChecked(typeID);
      if (!type)
        return type.takeError();
      return SILYieldInfo(type.get()->getCanonicalType(), *convention);
    };

    auto processResult =
        [&](TypeID typeID, uint64_t rawConvention,
            uint64_t rawDifferentiability) -> llvm::Expected<SILResultInfo> {
      auto convention = getActualResultConvention(rawConvention);
      if (!convention)
        MF.fatal();
      auto type = MF.getTypeChecked(typeID);
      if (!type)
        return type.takeError();
      auto differentiability =
          swift::SILResultDifferentiability::DifferentiableOrNotApplicable;
      if (diffKind != DifferentiabilityKind::NonDifferentiable) {
        auto differentiabilityOpt =
            getActualSILResultDifferentiability(rawDifferentiability);
        if (!differentiabilityOpt)
          MF.fatal();
        differentiability = *differentiabilityOpt;
      }
      return SILResultInfo(type.get()->getCanonicalType(), *convention,
                           differentiability);
    };

    // Bounds check.  FIXME: overflow
    unsigned entriesPerParam =
        diffKind != DifferentiabilityKind::NonDifferentiable ? 3 : 2;
    if (entriesPerParam * numParams + 2 * numResults +
            2 * unsigned(hasErrorResult) >
        variableData.size()) {
      MF.fatal();
    }

    unsigned nextVariableDataIndex = 0;

    // Process the parameters.
    SmallVector<SILParameterInfo, 8> allParams;
    allParams.reserve(numParams);
    for (unsigned i = 0; i != numParams; ++i) {
      auto typeID = variableData[nextVariableDataIndex++];
      auto rawConvention = variableData[nextVariableDataIndex++];
      uint64_t rawDifferentiability = 0;
      if (diffKind != DifferentiabilityKind::NonDifferentiable)
        rawDifferentiability = variableData[nextVariableDataIndex++];
      auto param =
          processParameter(typeID, rawConvention, rawDifferentiability);
      if (!param)
        return param.takeError();
      allParams.push_back(param.get());
    }

    // Process the yields.
    SmallVector<SILYieldInfo, 8> allYields;
    allYields.reserve(numYields);
    for (unsigned i = 0; i != numYields; ++i) {
      auto typeID = variableData[nextVariableDataIndex++];
      auto rawConvention = variableData[nextVariableDataIndex++];
      auto yield = processYield(typeID, rawConvention);
      if (!yield)
        return yield.takeError();
      allYields.push_back(yield.get());
    }

    // Process the results.
    SmallVector<SILResultInfo, 8> allResults;
    allParams.reserve(numResults);
    for (unsigned i = 0; i != numResults; ++i) {
      auto typeID = variableData[nextVariableDataIndex++];
      auto rawConvention = variableData[nextVariableDataIndex++];
      uint64_t rawDifferentiability = 0;
      if (diffKind != DifferentiabilityKind::NonDifferentiable)
        rawDifferentiability = variableData[nextVariableDataIndex++];
      auto result = processResult(typeID, rawConvention, rawDifferentiability);
      if (!result)
        return result.takeError();
      allResults.push_back(result.get());
    }

    // Process the error result.
    Optional<SILResultInfo> errorResult;
    if (hasErrorResult) {
      auto typeID = variableData[nextVariableDataIndex++];
      auto rawConvention = variableData[nextVariableDataIndex++];
      uint64_t rawDifferentiability = 0;
      auto maybeErrorResult =
          processResult(typeID, rawConvention, rawDifferentiability);
      if (!maybeErrorResult)
        return maybeErrorResult.takeError();
      errorResult = maybeErrorResult.get();
    }

    ProtocolConformanceRef witnessMethodConformance;
    if (*representation == SILFunctionTypeRepresentation::WitnessMethod) {
      witnessMethodConformance = MF.readConformance(MF.DeclTypeCursor);
    }

    GenericSignature invocationSig =
      MF.getGenericSignature(rawInvocationGenericSig);
    auto invocationSubsOrErr = MF.getSubstitutionMapChecked(rawInvocationSubs);
    if (!invocationSubsOrErr)
      return invocationSubsOrErr.takeError();
    auto patternSubsOrErr = MF.getSubstitutionMapChecked(rawPatternSubs);
    if (!patternSubsOrErr)
      return patternSubsOrErr.takeError();

    return SILFunctionType::get(invocationSig, extInfo, coroutineKind.getValue(),
                                calleeConvention.getValue(),
                                allParams, allYields, allResults,
                                errorResult,
                                patternSubsOrErr.get().getCanonical(),
                                invocationSubsOrErr.get().getCanonical(),
                                ctx, witnessMethodConformance);
  }

  Expected<Type> deserializeArraySliceType(ArrayRef<uint64_t> scratch,
                                           StringRef blobData) {
    TypeID baseID;
    decls_block::ArraySliceTypeLayout::readRecord(scratch, baseID);

    auto baseTy = MF.getTypeChecked(baseID);
    if (!baseTy)
      return baseTy.takeError();

    return ArraySliceType::get(baseTy.get());
  }

  Expected<Type> deserializeDictionaryType(ArrayRef<uint64_t> scratch,
                                           StringRef blobData) {
    TypeID keyID, valueID;
    decls_block::DictionaryTypeLayout::readRecord(scratch, keyID, valueID);

    auto keyTy = MF.getTypeChecked(keyID);
    if (!keyTy)
      return keyTy.takeError();

    auto valueTy = MF.getTypeChecked(valueID);
    if (!valueTy)
      return valueTy.takeError();

    return DictionaryType::get(keyTy.get(), valueTy.get());
  }

  Expected<Type> deserializeOptionalType(ArrayRef<uint64_t> scratch,
                                         StringRef blobData) {
    TypeID baseID;
    decls_block::OptionalTypeLayout::readRecord(scratch, baseID);

    auto baseTy = MF.getTypeChecked(baseID);
    if (!baseTy)
      return baseTy.takeError();

    return OptionalType::get(baseTy.get());
  }

  Expected<Type> deserializeUnboundGenericType(ArrayRef<uint64_t> scratch,
                                               StringRef blobData) {
    DeclID genericID;
    TypeID parentID;
    decls_block::UnboundGenericTypeLayout::readRecord(scratch,
                                                      genericID, parentID);

    auto nominalOrError = MF.getDeclChecked(genericID);
    if (!nominalOrError)
      return nominalOrError.takeError();
    auto genericDecl = cast<GenericTypeDecl>(nominalOrError.get());

    // FIXME: Check this?
    auto parentTy = MF.getType(parentID);

    return UnboundGenericType::get(genericDecl, parentTy, ctx);
  }
};
}

Expected<Type> ModuleFile::getTypeChecked(TypeID TID) {
  if (TID == 0)
    return Type();

  assert(TID <= Types.size() && "invalid type ID");
  auto &typeOrOffset = Types[TID-1];

  if (typeOrOffset.isComplete())
    return typeOrOffset;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(typeOrOffset));

  auto result = TypeDeserializer(*this).getTypeCheckedImpl();
  if (!result)
    return result;
  typeOrOffset = result.get();

#ifndef NDEBUG
  PrettyStackTraceType trace(getContext(), "deserializing", typeOrOffset.get());
  if (typeOrOffset.get()->hasError()) {
    typeOrOffset.get()->dump(llvm::errs());
    llvm_unreachable("deserialization produced an invalid type "
                     "(rdar://problem/30382791)");
  }
#endif

  // Invoke the callback on the deserialized type.
  DeserializedTypeCallback(typeOrOffset.get());
  return typeOrOffset.get();
}

Expected<Type> TypeDeserializer::getTypeCheckedImpl() {
  if (auto s = ctx.Stats)
    ++s->getFrontendCounters().NumTypesDeserialized;

  llvm::BitstreamEntry entry =
      MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize types represented by sub-blocks.
    MF.fatal();
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = MF.fatalIfUnexpected(
      MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

  switch (recordID) {
#define CASE(RECORD_NAME) \
  case decls_block::RECORD_NAME##TypeLayout::Code: \
    return deserialize##RECORD_NAME##Type(scratch, blobData);

  CASE(BuiltinAlias)
  CASE(TypeAlias)
  CASE(Nominal)
  CASE(Paren)
  CASE(Tuple)
  CASE(Function)
  CASE(GenericFunction)
  CASE(ExistentialMetatype)
  CASE(Metatype)
  CASE(DynamicSelf)
  CASE(ReferenceStorage)
  CASE(PrimaryArchetype)
  CASE(OpaqueArchetype)
  CASE(OpenedArchetype)
  CASE(NestedArchetype)
  CASE(GenericTypeParam)
  CASE(ProtocolComposition)
  CASE(DependentMember)
  CASE(BoundGeneric)
  CASE(SILBlockStorage)
  CASE(SILBox)
  CASE(SILFunction)
  CASE(ArraySlice)
  CASE(Dictionary)
  CASE(Optional)
  CASE(UnboundGeneric)

#undef CASE

  default:
    // We don't know how to deserialize this kind of type.
    MF.fatal();
  }
}

namespace {

class SwiftToClangBasicReader :
    public swift::DataStreamBasicReader<SwiftToClangBasicReader> {

  ModuleFile &MF;
  ClangModuleLoader &ClangLoader;
  ArrayRef<uint64_t> Record;

public:
  SwiftToClangBasicReader(ModuleFile &MF, ClangModuleLoader &clangLoader,
                          ArrayRef<uint64_t> record)
    : DataStreamBasicReader(clangLoader.getClangASTContext()),
      MF(MF), ClangLoader(clangLoader), Record(record) {}

  uint64_t readUInt64() {
    uint64_t value = Record[0];
    Record = Record.drop_front();
    return value;
  }

  Identifier readSwiftIdentifier() {
    return MF.getIdentifier(IdentifierID(readUInt64()));
  }

  clang::IdentifierInfo *readIdentifier() {
    Identifier swiftIdent = readSwiftIdentifier();
    return &getASTContext().Idents.get(swiftIdent.str());
  }

  clang::Stmt *readStmtRef() {
    // Should only be allowed with null statements.
    return nullptr;
  }

  clang::Decl *readDeclRef() {
    uint64_t refKind = readUInt64();

    // Null reference.
    if (refKind == 0) return nullptr;

    // Swift declaration.
    if (refKind == 1) {
      swift::Decl *swiftDecl = MF.getDecl(DeclID(readUInt64()));
      return const_cast<clang::Decl*>(
        ClangLoader.resolveStableSerializationPath(swiftDecl));
    }

    // External path.
    if (refKind == 2) {
      using ExternalPath = StableSerializationPath::ExternalPath;
      ExternalPath path;
      uint64_t length = readUInt64();
      path.Path.reserve(length);
      for (uint64_t i = 0; i != length; ++i) {
        auto kind = getActualClangDeclPathComponentKind(readUInt64());
        if (!kind) return nullptr;
        Identifier name = ExternalPath::requiresIdentifier(*kind)
                            ? readSwiftIdentifier()
                            : Identifier();
        path.add(*kind, name);
      }
      return const_cast<clang::Decl*>(
        ClangLoader.resolveStableSerializationPath(std::move(path)));
    }

    // Unknown kind?
    return nullptr;
  }
};

} // end anonymous namespace

llvm::Expected<const clang::Type *>
ModuleFile::getClangType(ClangTypeID TID) {
  if (!getContext().LangOpts.UseClangFunctionTypes)
    return nullptr;

  if (TID == 0)
    return nullptr;

  assert(TID <= ClangTypes.size() && "invalid type ID");
  auto &typeOrOffset = ClangTypes[TID-1];

  if (typeOrOffset.isComplete())
    return typeOrOffset;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(typeOrOffset));

  llvm::BitstreamEntry entry =
    fatalIfUnexpected(DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    fatal();
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = fatalIfUnexpected(
    DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

  if (recordID != decls_block::CLANG_TYPE)
    fatal();

  auto &clangLoader = *getContext().getClangModuleLoader();
  auto clangType =
    SwiftToClangBasicReader(*this, clangLoader, scratch).readTypeRef()
      .getTypePtr();
  typeOrOffset = clangType;
  return clangType;
}

Decl *handleErrorAndSupplyMissingClassMember(ASTContext &context,
                                             llvm::Error &&error,
                                             ClassDecl *containingClass) {
  Decl *suppliedMissingMember = nullptr;
  auto handleMissingClassMember = [&](const DeclDeserializationError &error) {
    if (error.isDesignatedInitializer())
      context.evaluator.cacheOutput(
          HasMissingDesignatedInitializersRequest{containingClass}, true);
    if (error.getNumberOfVTableEntries() > 0)
      containingClass->setHasMissingVTableEntries();

    suppliedMissingMember = MissingMemberDecl::create(
        context, containingClass, error.getName(),
        error.getNumberOfVTableEntries(),
        error.needsFieldOffsetVectorEntry());
  };
  llvm::handleAllErrors(std::move(error), handleMissingClassMember);
  return suppliedMissingMember;
}

Decl *handleErrorAndSupplyMissingProtoMember(ASTContext &context,
                                             llvm::Error &&error,
                                             ProtocolDecl *containingProto) {
  Decl *suppliedMissingMember = nullptr;

  auto handleMissingProtocolMember =
      [&](const DeclDeserializationError &error) {
        assert(error.needsFieldOffsetVectorEntry() == 0);

        if (error.getNumberOfVTableEntries() > 0)
          containingProto->setHasMissingRequirements(true);

        suppliedMissingMember = MissingMemberDecl::create(
            context, containingProto, error.getName(),
            error.getNumberOfVTableEntries(), 0);
      };
  llvm::handleAllErrors(std::move(error), handleMissingProtocolMember);
  return suppliedMissingMember;
}

Decl *handleErrorAndSupplyMissingMiscMember(llvm::Error &&error) {
  llvm::consumeError(std::move(error));
  return nullptr;
}

Decl *handleErrorAndSupplyMissingMember(ASTContext &context, Decl *container,
                                        llvm::Error &&error) {
  // Drop the member if it had a problem.
  // FIXME: Handle overridable members in class extensions too, someday.
  if (auto *containingClass = dyn_cast<ClassDecl>(container)) {
    return handleErrorAndSupplyMissingClassMember(context, std::move(error),
                                                  containingClass);
  }
  if (auto *containingProto = dyn_cast<ProtocolDecl>(container)) {
    return handleErrorAndSupplyMissingProtoMember(context, std::move(error),
                                                  containingProto);
  }
  return handleErrorAndSupplyMissingMiscMember(std::move(error));
}

void ModuleFile::loadAllMembers(Decl *container, uint64_t contextData) {
  PrettyStackTraceDecl trace("loading members for", container);
  ++NumMemberListsLoaded;

  IterableDeclContext *IDC;
  if (auto *nominal = dyn_cast<NominalTypeDecl>(container))
    IDC = nominal;
  else
    IDC = cast<ExtensionDecl>(container);

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(contextData));
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());
  if (entry.Kind != llvm::BitstreamEntry::Record)
    fatal();

  SmallVector<uint64_t, 16> memberIDBuffer;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, memberIDBuffer));
  assert(kind == decls_block::MEMBERS);
  (void)kind;

  ArrayRef<uint64_t> rawMemberIDs;
  decls_block::MembersLayout::readRecord(memberIDBuffer, rawMemberIDs);

  if (rawMemberIDs.empty())
    return;

  SmallVector<Decl *, 16> members;
  members.reserve(rawMemberIDs.size());
  for (DeclID rawID : rawMemberIDs) {
    Expected<Decl *> next = getDeclChecked(rawID);
    if (next) {
      assert(next.get() && "unchecked error deserializing next member");
      members.push_back(next.get());
    } else {
      if (!getContext().LangOpts.EnableDeserializationRecovery)
        fatal(next.takeError());

      Decl *suppliedMissingMember = handleErrorAndSupplyMissingMember(
          getContext(), container, next.takeError());
      if (suppliedMissingMember)
        members.push_back(suppliedMissingMember);
    }
  }

  for (auto member : members)
    IDC->addMember(member);

  if (auto *proto = dyn_cast<ProtocolDecl>(container)) {
    PrettyStackTraceDecl trace("reading default witness table for", proto);
    bool Err = readDefaultWitnessTable(proto);
    assert(!Err && "unable to read default witness table");
    (void)Err;
  }
}

static llvm::Error consumeErrorIfXRefNonLoadedModule(llvm::Error &&error) {
    // Missing module errors are most likely caused by an
    // implementation-only import hiding types and decls.
    // rdar://problem/60291019
    if (error.isA<XRefNonLoadedModuleError>()) {
      consumeError(std::move(error));
      return llvm::Error::success();
    }

    // Some of these errors may manifest as a TypeError with an
    // XRefNonLoadedModuleError underneath. Catch those as well.
    // rdar://66491720
    if (error.isA<TypeError>()) {
      auto errorInfo = takeErrorInfo(std::move(error));
      auto *TE = static_cast<TypeError*>(errorInfo.get());

      if (TE->underlyingReasonIsA<XRefNonLoadedModuleError>()) {
        consumeError(std::move(errorInfo));
        return llvm::Error::success();
      }

      return std::move(errorInfo);
    }

    return std::move(error);
}

void
ModuleFile::loadAllConformances(const Decl *D, uint64_t contextData,
                          SmallVectorImpl<ProtocolConformance*> &conformances) {
  PrettyStackTraceDecl trace("loading conformances for", D);

  uint64_t numConformances;
  uint64_t bitPosition;
  std::tie(numConformances, bitPosition)
    = decodeLazyConformanceContextData(contextData);

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(bitPosition));

  while (numConformances--) {
    auto conformance = readConformanceChecked(DeclTypeCursor);

    if (!conformance) {
      auto unconsumedError =
        consumeErrorIfXRefNonLoadedModule(conformance.takeError());
      if (unconsumedError)
        fatal(std::move(unconsumedError));
      return;
    }

    if (conformance.get().isConcrete())
      conformances.push_back(conformance.get().getConcrete());
  }
}

Type
ModuleFile::loadAssociatedTypeDefault(const swift::AssociatedTypeDecl *ATD,
                                      uint64_t contextData) {
  return getType(contextData);
}

ValueDecl *ModuleFile::loadDynamicallyReplacedFunctionDecl(
    const DynamicReplacementAttr *DRA, uint64_t contextData) {
  return cast<ValueDecl>(getDecl(contextData));
}

AbstractFunctionDecl *
ModuleFile::loadReferencedFunctionDecl(const DerivativeAttr *DA,
                                       uint64_t contextData) {
  return cast<AbstractFunctionDecl>(getDecl(contextData));
}

ValueDecl *ModuleFile::loadTargetFunctionDecl(const SpecializeAttr *attr,
                                              uint64_t contextData) {
  if (contextData == 0)
    return nullptr;
  return cast<AbstractFunctionDecl>(getDecl(contextData));
}

Type ModuleFile::loadTypeEraserType(const TypeEraserAttr *TRA,
                                    uint64_t contextData) {
  return getType(contextData);
}

void ModuleFile::finishNormalConformance(NormalProtocolConformance *conformance,
                                         uint64_t contextData) {
  using namespace decls_block;

  PrettyStackTraceModuleFile traceModule("While reading from", *this);
  PrettyStackTraceConformance trace(getAssociatedModule()->getASTContext(),
                                    "finishing conformance for",
                                    conformance);
  ++NumNormalProtocolConformancesCompleted;

  assert(conformance->isComplete());

  conformance->setState(ProtocolConformanceState::Incomplete);
  SWIFT_DEFER { conformance->setState(ProtocolConformanceState::Complete); };

  // Find the conformance record.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(contextData));
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());
  assert(entry.Kind == llvm::BitstreamEntry::Record &&
         "registered lazy loader incorrectly");

  DeclID protoID;
  DeclContextID contextID;
  unsigned valueCount, typeCount, conformanceCount;
  ArrayRef<uint64_t> rawIDs;
  SmallVector<uint64_t, 16> scratch;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, scratch));
  (void) kind;
  assert(kind == NORMAL_PROTOCOL_CONFORMANCE &&
         "registered lazy loader incorrectly");
  NormalProtocolConformanceLayout::readRecord(scratch, protoID,
                                              contextID, typeCount,
                                              valueCount, conformanceCount,
                                              rawIDs);

  // Read requirement signature conformances.
  const ProtocolDecl *proto = conformance->getProtocol();
  SmallVector<ProtocolConformanceRef, 4> reqConformances;

  if (proto->isObjC() && getContext().LangOpts.EnableDeserializationRecovery) {
    // Don't crash if inherited protocols are added or removed.
    // This is limited to Objective-C protocols because we know their only
    // conformance requirements are on Self. This isn't actually a /safe/ change
    // even in Objective-C, but we mostly just don't want to crash.

    llvm::SmallDenseMap<ProtocolDecl *, ProtocolConformanceRef, 16>
        conformancesForProtocols;
    while (conformanceCount--) {
      ProtocolConformanceRef nextConformance = readConformance(DeclTypeCursor);
      ProtocolDecl *confProto = nextConformance.getRequirement();
      conformancesForProtocols[confProto] = nextConformance;
    }

    for (const auto &req : proto->getRequirementSignature()) {
      if (req.getKind() != RequirementKind::Conformance)
        continue;
      ProtocolDecl *proto =
          req.getSecondType()->castTo<ProtocolType>()->getDecl();
      auto iter = conformancesForProtocols.find(proto);
      if (iter != conformancesForProtocols.end()) {
        reqConformances.push_back(iter->getSecond());
      } else {
        // Put in an abstract conformance as a placeholder. This is a lie, but
        // there's not much better we can do. We're relying on the fact that
        // the rest of the compiler doesn't actually need to check the
        // conformance to an Objective-C protocol for anything important.
        // There are no associated types and we don't emit a Swift conformance
        // record.
        reqConformances.push_back(ProtocolConformanceRef(proto));
      }
    }

  } else {
    auto isConformanceReq = [](const Requirement &req) {
      return req.getKind() == RequirementKind::Conformance;
    };
    if (conformanceCount != llvm::count_if(proto->getRequirementSignature(),
                                           isConformanceReq)) {
      fatal(llvm::make_error<llvm::StringError>(
          "serialized conformances do not match requirement signature",
          llvm::inconvertibleErrorCode()));
    }
    while (conformanceCount--)
      reqConformances.push_back(readConformance(DeclTypeCursor));
  }
  conformance->setSignatureConformances(reqConformances);

  ArrayRef<uint64_t>::iterator rawIDIter = rawIDs.begin();

  TypeWitnessMap typeWitnesses;
  while (typeCount--) {
    // FIXME: We don't actually want to allocate an archetype here; we just
    // want to get an access path within the protocol.
    auto first = cast<AssociatedTypeDecl>(getDecl(*rawIDIter++));
    auto second = getType(*rawIDIter++);
    auto third = cast_or_null<TypeDecl>(getDecl(*rawIDIter++));
    if (third &&
        isa<TypeAliasDecl>(third) &&
        third->getModuleContext() != getAssociatedModule() &&
        !third->getDeclaredInterfaceType()->isEqual(second)) {
      // Conservatively drop references to typealiases in other modules
      // that may have changed. This may also drop references to typealiases
      // that /haven't/ changed but just happen to have generics in them, but
      // in practice having a declaration here isn't actually required by the
      // rest of the compiler.
      third = nullptr;
    }
    typeWitnesses[first] = {second, third};
  }
  assert(rawIDIter <= rawIDs.end() && "read too much");

  // Set type witnesses.
  for (auto typeWitness : typeWitnesses) {
    conformance->setTypeWitness(typeWitness.first,
                                typeWitness.second.getWitnessType(),
                                typeWitness.second.getWitnessDecl());
  }

  // An imported requirement may have changed type between Swift versions.
  // In this situation we need to do a post-pass to fill in missing
  // requirements with opaque witnesses.
  bool needToFillInOpaqueValueWitnesses = false;
  while (valueCount--) {
    ValueDecl *req;
    
    auto trySetWitness = [&](Witness w) {
      if (req)
        conformance->setWitness(req, w);
    };
    
    auto deserializedReq = getDeclChecked(*rawIDIter++);
    if (deserializedReq) {
      req = cast_or_null<ValueDecl>(*deserializedReq);
    } else if (getContext().LangOpts.EnableDeserializationRecovery) {
      consumeError(deserializedReq.takeError());
      req = nullptr;
      needToFillInOpaqueValueWitnesses = true;
    } else {
      fatal(deserializedReq.takeError());
    }
    
    bool isOpaque = false;
    ValueDecl *witness;
    auto deserializedWitness = getDeclChecked(*rawIDIter++);
    if (deserializedWitness) {
      witness = cast_or_null<ValueDecl>(*deserializedWitness);
    // Across language compatibility versions, the witnessing decl may have
    // changed its signature as seen by the current compatibility version.
    // In that case, we want the conformance to still be available, but
    // we can't make use of the relationship to the underlying decl.
    } else if (getContext().LangOpts.EnableDeserializationRecovery) {
      consumeError(deserializedWitness.takeError());
      isOpaque = true;
      witness = nullptr;
    } else {
      fatal(deserializedWitness.takeError());
    }
    
    assert(!req || isOpaque || witness ||
           req->getAttrs().hasAttribute<OptionalAttr>() ||
           req->getAttrs().isUnavailable(getContext()));
    if (!witness && !isOpaque) {
      trySetWitness(Witness());
      continue;
    }

    auto trySetOpaqueWitness = [&]{
      if (!req)
        return;

      conformance->setWitness(req, Witness::forOpaque(req));
    };

    // Witness substitutions.
    auto witnessSubstitutions = getSubstitutionMapChecked(*rawIDIter++);
    if (!witnessSubstitutions) {
      // Missing module errors are most likely caused by an
      // implementation-only import hiding types and decls.
      // rdar://problem/52837313
      if (witnessSubstitutions.errorIsA<XRefNonLoadedModuleError>()) {
        consumeError(witnessSubstitutions.takeError());
        isOpaque = true;
      }
      else
        fatal(witnessSubstitutions.takeError());
    }

    // Handle opaque witnesses that couldn't be deserialized.
    if (isOpaque) {
      trySetOpaqueWitness();
      continue;
    }

    // Set the witness.
    trySetWitness(Witness::forDeserialized(witness, witnessSubstitutions.get()));
  }
  assert(rawIDIter <= rawIDs.end() && "read too much");
  
  // Fill in opaque value witnesses if we need to.
  if (needToFillInOpaqueValueWitnesses) {
    for (auto member : proto->getMembers()) {
      // We only care about non-associated-type requirements.
      auto valueMember = dyn_cast<ValueDecl>(member);
      if (!valueMember || !valueMember->isProtocolRequirement()
          || isa<AssociatedTypeDecl>(valueMember))
        continue;
      
      if (!conformance->hasWitness(valueMember))
        conformance->setWitness(valueMember, Witness::forOpaque(valueMember));
    }
  }
}

void ModuleFile::loadRequirementSignature(const ProtocolDecl *decl,
                                          uint64_t contextData,
                                          SmallVectorImpl<Requirement> &reqs) {
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  fatalIfNotSuccess(DeclTypeCursor.JumpToBit(contextData));
  readGenericRequirements(reqs, DeclTypeCursor);
}

static Optional<ForeignErrorConvention::Kind>
decodeRawStableForeignErrorConventionKind(uint8_t kind) {
  switch (kind) {
  case static_cast<uint8_t>(ForeignErrorConventionKind::ZeroResult):
    return ForeignErrorConvention::ZeroResult;
  case static_cast<uint8_t>(ForeignErrorConventionKind::NonZeroResult):
    return ForeignErrorConvention::NonZeroResult;
  case static_cast<uint8_t>(ForeignErrorConventionKind::ZeroPreservedResult):
    return ForeignErrorConvention::ZeroPreservedResult;
  case static_cast<uint8_t>(ForeignErrorConventionKind::NilResult):
    return ForeignErrorConvention::NilResult;
  case static_cast<uint8_t>(ForeignErrorConventionKind::NonNilError):
    return ForeignErrorConvention::NonNilError;
  default:
    return None;
  }
}

Optional<StringRef> ModuleFile::maybeReadInlinableBodyText() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  StringRef blobData;

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return None;

  unsigned recKind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch, &blobData));
  if (recKind != INLINABLE_BODY_TEXT)
    return None;

  restoreOffset.reset();
  return blobData;
}

Optional<ForeignErrorConvention> ModuleFile::maybeReadForeignErrorConvention() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;

  BCOffsetRAII restoreOffset(DeclTypeCursor);

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return None;

  unsigned recKind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch));
  switch (recKind) {
  case FOREIGN_ERROR_CONVENTION:
    restoreOffset.reset();
    break;

  default:
    return None;
  }

  uint8_t rawKind;
  bool isOwned;
  bool isReplaced;
  unsigned errorParameterIndex;
  TypeID errorParameterTypeID;
  TypeID resultTypeID;
  ForeignErrorConventionLayout::readRecord(scratch, rawKind,
                                           isOwned, isReplaced,
                                           errorParameterIndex,
                                           errorParameterTypeID,
                                           resultTypeID);

  ForeignErrorConvention::Kind kind;
  if (auto optKind = decodeRawStableForeignErrorConventionKind(rawKind))
    kind = *optKind;
  else
    fatal();

  Type errorParameterType = getType(errorParameterTypeID);
  CanType canErrorParameterType;
  if (errorParameterType)
    canErrorParameterType = errorParameterType->getCanonicalType();

  Type resultType = getType(resultTypeID);
  CanType canResultType;
  if (resultType)
    canResultType = resultType->getCanonicalType();

  auto owned = isOwned ? ForeignErrorConvention::IsOwned
                       : ForeignErrorConvention::IsNotOwned;
  auto replaced = ForeignErrorConvention::IsReplaced_t(isOwned);
  switch (kind) {
  case ForeignErrorConvention::ZeroResult:
    return ForeignErrorConvention::getZeroResult(errorParameterIndex,
                                                 owned, replaced,
                                                 canErrorParameterType,
                                                 canResultType);

  case ForeignErrorConvention::NonZeroResult:
    return ForeignErrorConvention::getNonZeroResult(errorParameterIndex,
                                                    owned, replaced,
                                                    canErrorParameterType,
                                                    canResultType);

  case ForeignErrorConvention::ZeroPreservedResult:
    return ForeignErrorConvention::getZeroPreservedResult(errorParameterIndex,
                                                          owned, replaced,
                                                       canErrorParameterType);

  case ForeignErrorConvention::NilResult:
    return ForeignErrorConvention::getNilResult(errorParameterIndex,
                                                owned, replaced,
                                                canErrorParameterType);

  case ForeignErrorConvention::NonNilError:
    return ForeignErrorConvention::getNonNilError(errorParameterIndex,
                                                  owned, replaced,
                                                  canErrorParameterType);
  }

  llvm_unreachable("Unhandled ForeignErrorConvention in switch.");
}
