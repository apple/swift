//===--- Deserialization.cpp - Loading a serialized AST -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
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
#include "swift/AST/Attr.h"
#include "swift/AST/AttrKind.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/ClangImporter/SwiftAbstractBasicReader.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Signals.h"
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
      os << " in '" << MF->getName() << "'\n";
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
const char InvalidRecordKindError::ID = '\0';
void InvalidRecordKindError::anchor() {}
const char UnsafeDeserializationError::ID = '\0';
void UnsafeDeserializationError::anchor() {}
const char ModularizationError::ID = '\0';
void ModularizationError::anchor() {}

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

char FatalDeserializationError::ID;

void ModuleFile::fatal(llvm::Error error) const {
  Core->fatal(diagnoseFatal(std::move(error)));
}

SourceLoc ModuleFile::getSourceLoc() const {
  auto &SourceMgr = getContext().Diags.SourceMgr;
  auto filename = getModuleFilename();
  auto bufferID = SourceMgr.getIDForBufferIdentifier(filename);
  if (!bufferID)
    bufferID = SourceMgr.addMemBufferCopy("<binary format>", filename);
  return SourceMgr.getLocForBufferStart(*bufferID);
}

SourceLoc ModularizationError::getSourceLoc() const {
  auto &SourceMgr = referenceModule->getContext().Diags.SourceMgr;
  auto filename = referenceModule->getModuleFilename();

  // Synthesize some context. We don't have an actual decl here
  // so try to print a simple representation of the reference.
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << expectedModule->getName() << "." << name;

  // If we enable these remarks by default we may want to reuse these buffers.
  auto bufferID = SourceMgr.addMemBufferCopy(S, filename);
  return SourceMgr.getLocForBufferStart(bufferID);
}

void
ModularizationError::diagnose(const ModuleFile *MF,
                              DiagnosticBehavior limit) const {
  auto &ctx = MF->getContext();

  auto diagnoseError = [&](Kind errorKind) {
    switch (errorKind) {
    case Kind::DeclMoved:
      return ctx.Diags.diagnose(getSourceLoc(),
                                diag::modularization_issue_decl_moved,
                                declIsType, name, expectedModule,
                                foundModule);
    case Kind::DeclKindChanged:
      return
        ctx.Diags.diagnose(getSourceLoc(),
                           diag::modularization_issue_decl_type_changed,
                           declIsType, name, expectedModule,
                           referenceModule->getName(), foundModule,
                           foundModule != expectedModule);
    case Kind::DeclNotFound:
      return ctx.Diags.diagnose(getSourceLoc(),
                                diag::modularization_issue_decl_not_found,
                                declIsType, name, expectedModule);
    }
    llvm_unreachable("Unhandled ModularizationError::Kind in switch.");
  };

  auto inFlight = diagnoseError(errorKind);
  inFlight.limitBehavior(limit);
  inFlight.flush();

  // We could pass along the `path` information through notes.
  // However, for a top-level decl a path would just duplicate the
  // expected module name and the decl name from the diagnostic.

  // Show context with relevant file paths.
  ctx.Diags.diagnose(SourceLoc(),
                     diag::modularization_issue_note_expected,
                     declIsType, expectedModule,
                     expectedModule->getModuleSourceFilename());

  const clang::Module *expectedUnderlying =
                                   expectedModule->findUnderlyingClangModule();
  if (!expectedModule->isNonSwiftModule() &&
      expectedUnderlying) {
    auto CML = ctx.getClangModuleLoader();
    auto &CSM = CML->getClangASTContext().getSourceManager();
    StringRef filename = CSM.getFilename(expectedUnderlying->DefinitionLoc);
    ctx.Diags.diagnose(SourceLoc(),
                       diag::modularization_issue_note_expected_underlying,
                       expectedUnderlying->Name,
                       filename);
  }

  if (foundModule)
    ctx.Diags.diagnose(SourceLoc(),
                       diag::modularization_issue_note_found,
                       declIsType, foundModule,
                       foundModule->getModuleSourceFilename());

  // A Swift language version mismatch could lead to a different set of rules
  // from APINotes files being applied when building the module vs when reading
  // from it.
  version::Version
    moduleLangVersion = referenceModule->getCompatibilityVersion(),
    clientLangVersion = MF->getContext().LangOpts.EffectiveLanguageVersion;
  ModuleDecl *referenceModuleDecl = referenceModule->getAssociatedModule();
  if (clientLangVersion != moduleLangVersion) {
    ctx.Diags.diagnose(SourceLoc(),
                       diag::modularization_issue_swift_version,
                       referenceModuleDecl, moduleLangVersion,
                       clientLangVersion);
  }

  // If the error is in a resilient swiftmodule adjacent to a swiftinterface,
  // deleting the module to rebuild from the swiftinterface may fix the issue.
  // Limit this suggestion to distributed Swift modules to not hint at
  // deleting local caches and such.
  bool referenceModuleIsDistributed = referenceModuleDecl &&
                                      referenceModuleDecl->isNonUserModule();
  if (referenceModule->getResilienceStrategy() ==
                                               ResilienceStrategy::Resilient &&
      referenceModuleIsDistributed) {
    ctx.Diags.diagnose(SourceLoc(),
                       diag::modularization_issue_stale_module,
                       referenceModuleDecl,
                       referenceModule->getModuleFilename());
  }

  // If the missing decl was expected to be in a clang module,
  // it may be hidden by some clang defined passed via `-Xcc` affecting how
  // headers are seen.
  if (expectedUnderlying) {
    ctx.Diags.diagnose(SourceLoc(),
                       diag::modularization_issue_audit_headers,
                       expectedModule->isNonSwiftModule(), expectedModule);
  }

  // If the reference goes from a distributed module to a local module,
  // the compiler may have picked up an undesired module. We usually expect
  // distributed modules to only reference other distributed modules.
  // Local modules can reference both local modules and distributed modules.
  if (referenceModuleIsDistributed) {
    if (!expectedModule->isNonUserModule()) {
      ctx.Diags.diagnose(SourceLoc(),
                         diag::modularization_issue_layering_expected_local,
                         referenceModuleDecl, expectedModule);
    } else if (foundModule && !foundModule->isNonUserModule()) {
      ctx.Diags.diagnose(SourceLoc(),
                         diag::modularization_issue_layering_found_local,
                         referenceModuleDecl, foundModule);
    }
  }

  // If a type moved between MyModule and MyModule_Private, it can be caused
  // by the use of `-Xcc -D` to change the API of the modules, leading to
  // decls moving between both modules.
  if (errorKind == Kind::DeclMoved ||
      errorKind == Kind::DeclKindChanged) {
    StringRef foundModuleName = foundModule->getName().str();
    StringRef expectedModuleName = expectedModule->getName().str();
    if (foundModuleName != expectedModuleName &&
        (foundModuleName.startswith(expectedModuleName) ||
         expectedModuleName.startswith(foundModuleName)) &&
        (expectedUnderlying ||
         expectedModule->findUnderlyingClangModule())) {
      ctx.Diags.diagnose(SourceLoc(),
                         diag::modularization_issue_related_modules,
                         declIsType, name);
    }
  }
}

void TypeError::diagnose(const ModuleFile *MF) const {
  MF->getContext().Diags.diagnose(MF->getSourceLoc(),
                                  diag::modularization_issue_side_effect_type_error,
                                  name);
}

void ExtensionError::diagnose(const ModuleFile *MF) const {
  MF->getContext().Diags.diagnose(MF->getSourceLoc(),
                       diag::modularization_issue_side_effect_extension_error);
}

llvm::Error
ModuleFile::diagnoseModularizationError(llvm::Error error,
                                        DiagnosticBehavior limit) const {
  auto handleModularizationError =
    [&](const ModularizationError &modularError) -> llvm::Error {
      modularError.diagnose(this, limit);
      return llvm::Error::success();
    };
  llvm::Error outError = llvm::handleErrors(std::move(error),
        handleModularizationError,
        [&](TypeError &typeError) -> llvm::Error {
          if (typeError.diagnoseUnderlyingReason(handleModularizationError)) {
            typeError.diagnose(this);
            return llvm::Error::success();
          }
          return llvm::make_error<TypeError>(std::move(typeError));
        },
        [&](ExtensionError &extError) -> llvm::Error {
          if (extError.diagnoseUnderlyingReason(handleModularizationError)) {
            extError.diagnose(this);
            return llvm::Error::success();
          }
          return llvm::make_error<ExtensionError>(std::move(extError));
        });

  return outError;
}

llvm::Error ModuleFile::diagnoseFatal(llvm::Error error) const {

  auto &ctx = getContext();
  if (FileContext) {
    if (ctx.LangOpts.EnableDeserializationRecovery) {
      error = diagnoseModularizationError(std::move(error));

      // If no error is left, it was reported as a diagnostic. There's no
      // need to crash.
      if (!error)
        return llvm::Error::success();
    }

    // General deserialization failure message.
    ctx.Diags.diagnose(getSourceLoc(), diag::serialization_fatal, Core->Name);
  }
  // Unless in the debugger, crash. ModuleFileSharedCore::fatal() calls abort().
  // This allows aggregation of crash logs for compiler development, but in a
  // long-running process like LLDB this is undesirable. Only abort() if not in
  // the debugger.
  if (!ctx.LangOpts.DebuggerSupport)
    Core->fatal(std::move(error));

  // Otherwise, augment the error with contextual information at this point
  // of failure and pass it back to be reported later.
  std::string msg;
  {
    llvm::raw_string_ostream os(msg);
    Core->outputDiagnosticInfo(os);
    os << '\n';
    llvm::sys::PrintStackTrace(os, 128);
  }
  msg += ": " + toString(std::move(error));
  return llvm::make_error<FatalDeserializationError>(msg);
}

void ModuleFile::outputDiagnosticInfo(llvm::raw_ostream &os) const {
  Core->outputDiagnosticInfo(os);
}

static llvm::Optional<swift::AccessorKind> getActualAccessorKind(uint8_t raw) {
  switch (serialization::AccessorKind(raw)) {
#define ACCESSOR(ID) \
  case serialization::AccessorKind::ID: return swift::AccessorKind::ID;
#include "swift/AST/AccessorKinds.def"
  }

  return llvm::None;
}

/// Translate from the serialization DefaultArgumentKind enumerators, which are
/// guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::DefaultArgumentKind>
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
  return llvm::None;
}

static llvm::Optional<StableSerializationPath::ExternalPath::ComponentKind>
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
  return llvm::None;
}

ParameterList *ModuleFile::readParameterList() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;
  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  unsigned recordID =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, scratch));
  if (recordID != PARAMETERLIST)
    fatal(llvm::make_error<InvalidRecordKindError>(recordID));

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
    return diagnoseFatal();

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
      if (kind != decls_block::TUPLE_PATTERN_ELT)
        fatal(llvm::make_error<InvalidRecordKindError>(kind));

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
    bool isAsyncLet;

    AnyPatternLayout::readRecord(scratch, typeID, isAsyncLet);
    auto result = AnyPattern::createImplicit(getContext());
    if (isAsyncLet) {
      result->setIsAsyncLet();
    }
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

    auto result = BindingPattern::createImplicit(
        getContext(),
        isLet ? VarDecl::Introducer::Let : VarDecl::Introducer::Var,
        subPattern);
    if (Type interfaceType = subPattern->getDelayedInterfaceType())
      result->setDelayedInterfaceType(interfaceType, owningDC);
    else
      result->setType(subPattern->getType());
    restoreOffset.reset();
    return result;
  }

  default:
    return llvm::make_error<InvalidRecordKindError>(kind);
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
    bool capturesGenerics;
    unsigned numFields;
    ArrayRef<uint64_t> types;
    decls_block::SILLayoutLayout::readRecord(scratch, rawGenericSig,
                                             capturesGenerics,
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
    return SILLayout::get(getContext(), canSig, fields, capturesGenerics);
  }
  default:
    fatal(llvm::make_error<InvalidRecordKindError>(kind));
  }
}

namespace swift {
class ProtocolConformanceDeserializer {
  ModuleFile &MF;
  ASTContext &ctx;

  using TypeID = serialization::TypeID;
  using ProtocolConformanceID = serialization::ProtocolConformanceID;
public:
  ProtocolConformanceDeserializer(ModuleFile &mf)
    : MF(mf), ctx(mf.getContext()) {}

  Expected<ProtocolConformance *>
  read(ModuleFile::Serialized<ProtocolConformance *> &entry);

  Expected<ProtocolConformance *>
  readSelfProtocolConformance(ArrayRef<uint64_t> data);
  Expected<ProtocolConformance *>
  readSpecializedProtocolConformance(ArrayRef<uint64_t> data);
  Expected<ProtocolConformance *>
  readInheritedProtocolConformance(ArrayRef<uint64_t> data);
  Expected<ProtocolConformance *>
  readBuiltinProtocolConformance(ArrayRef<uint64_t> data);
  Expected<ProtocolConformance *>
  readNormalProtocolConformance(ArrayRef<uint64_t> data,
                    ModuleFile::Serialized<ProtocolConformance *> &entry);
  Expected<ProtocolConformance *>
  readNormalProtocolConformanceXRef(ArrayRef<uint64_t> data);

  Expected<PackConformance *>
  read(ModuleFile::Serialized<PackConformance *> &entry);
};
} // end namespace swift

Expected<ProtocolConformance*>
ProtocolConformanceDeserializer::read(
          ModuleFile::Serialized<ProtocolConformance *> &conformanceEntry) {
  SmallVector<uint64_t, 16> scratch;

  if (auto s = ctx.Stats)
    ++s->getFrontendCounters().NumConformancesDeserialized;

  llvm::BitstreamEntry entry =
      MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize types represented by sub-blocks.
    return MF.diagnoseFatal();
  }

  StringRef blobData;
  unsigned kind = MF.fatalIfUnexpected(
      MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  assert(blobData.empty());

  switch (kind) {
  case decls_block::SELF_PROTOCOL_CONFORMANCE:
    return readSelfProtocolConformance(scratch);
  case decls_block::SPECIALIZED_PROTOCOL_CONFORMANCE:
    return readSpecializedProtocolConformance(scratch);
  case decls_block::INHERITED_PROTOCOL_CONFORMANCE:
    return readInheritedProtocolConformance(scratch);
  case decls_block::BUILTIN_PROTOCOL_CONFORMANCE:
    return readBuiltinProtocolConformance(scratch);
  case decls_block::NORMAL_PROTOCOL_CONFORMANCE:
    return readNormalProtocolConformance(scratch, conformanceEntry);
  case decls_block::PROTOCOL_CONFORMANCE_XREF:
    return readNormalProtocolConformanceXRef(scratch);

  // Not a protocol conformance.
  default:
    return MF.diagnoseFatal(llvm::make_error<InvalidRecordKindError>(kind));
  }
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readSelfProtocolConformance(
                                              ArrayRef<uint64_t> scratch) {
  using namespace decls_block;

  DeclID protoID;
  SelfProtocolConformanceLayout::readRecord(scratch, protoID);

  auto decl = MF.getDeclChecked(protoID);
  if (!decl)
    return decl.takeError();

  auto proto = cast<ProtocolDecl>(decl.get());
  auto conformance = ctx.getSelfConformance(proto);
  return conformance;
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readSpecializedProtocolConformance(
                                              ArrayRef<uint64_t> scratch) {
  using namespace decls_block;

  ProtocolConformanceID conformanceID;
  TypeID conformingTypeID;
  SubstitutionMapID substitutionMapID;
  SpecializedProtocolConformanceLayout::readRecord(scratch,
                                                   conformanceID,
                                                   conformingTypeID,
                                                   substitutionMapID);

  Type conformingType = MF.getType(conformingTypeID);

  PrettyStackTraceType trace(MF.getAssociatedModule()->getASTContext(),
                             "reading specialized conformance for",
                             conformingType);

  auto subMapOrError = MF.getSubstitutionMapChecked(substitutionMapID);
  if (!subMapOrError)
    return subMapOrError.takeError();
  auto subMap = subMapOrError.get();

  auto genericConformance = MF.getConformance(conformanceID);

  PrettyStackTraceDecl traceTo("... to", genericConformance.getRequirement());
  ++NumNormalProtocolConformancesLoaded;

  auto *rootConformance = cast<RootProtocolConformance>(
      genericConformance.getConcrete());

  auto conformance =
         ctx.getSpecializedConformance(conformingType, rootConformance, subMap);
  return conformance;
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readInheritedProtocolConformance(
                                              ArrayRef<uint64_t> scratch) {
  using namespace decls_block;

  ProtocolConformanceID conformanceID;
  TypeID conformingTypeID;
  InheritedProtocolConformanceLayout::readRecord(scratch, conformanceID,
                                                 conformingTypeID);

  Type conformingType = MF.getType(conformingTypeID);

  PrettyStackTraceType trace(ctx, "reading inherited conformance for",
                             conformingType);

  ProtocolConformanceRef inheritedConformance =
    MF.getConformance(conformanceID);
  PrettyStackTraceDecl traceTo("... to",
                               inheritedConformance.getRequirement());

  assert(inheritedConformance.isConcrete() &&
         "Abstract inherited conformance?");
  auto conformance =
    ctx.getInheritedConformance(conformingType,
                                inheritedConformance.getConcrete());
  return conformance;
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readBuiltinProtocolConformance(
                                              ArrayRef<uint64_t> scratch) {
  using namespace decls_block;

  TypeID conformingTypeID;
  DeclID protoID;
  GenericSignatureID genericSigID;
  unsigned builtinConformanceKind;
  ArrayRef<uint64_t> data;
  BuiltinProtocolConformanceLayout::readRecord(scratch, conformingTypeID,
                                               protoID, genericSigID,
                                               builtinConformanceKind,
                                               data);

  Type conformingType = MF.getType(conformingTypeID);

  auto decl = MF.getDeclChecked(protoID);
  if (!decl)
    return decl.takeError();

  auto proto = cast<ProtocolDecl>(decl.get());
  auto genericSig = MF.getGenericSignatureChecked(genericSigID);
  if (!genericSig)
    return genericSig.takeError();

  // Read the conditional requirements.
  SmallVector<Requirement, 4> conditionalRequirements;
  unsigned nextDataIndex = 0;
  auto error = MF.deserializeGenericRequirementsChecked(
      data, nextDataIndex, conditionalRequirements);
  if (error)
    return std::move(error);
  if (nextDataIndex != data.size())
    return MF.diagnoseFatal();

  auto conformance = ctx.getBuiltinConformance(
      conformingType, proto, *genericSig, conditionalRequirements,
      static_cast<BuiltinConformanceKind>(builtinConformanceKind));
  return conformance;
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readNormalProtocolConformanceXRef(
                                              ArrayRef<uint64_t> scratch) {
  using namespace decls_block;

  DeclID protoID;
  DeclID nominalID;
  ModuleID moduleID;
  ProtocolConformanceXrefLayout::readRecord(scratch, protoID, nominalID,
                                            moduleID);

  auto maybeNominal = MF.getDeclChecked(nominalID);
  if (!maybeNominal)
    return maybeNominal.takeError();

  auto nominal = cast<NominalTypeDecl>(maybeNominal.get());
  PrettyStackTraceDecl trace("cross-referencing conformance for", nominal);
  auto proto = cast<ProtocolDecl>(MF.getDecl(protoID));
  PrettyStackTraceDecl traceTo("... to", proto);
  auto module = MF.getModule(moduleID);

  // FIXME: If the module hasn't been loaded, we probably don't want to fall
  // back to the current module like this.
  if (!module)
    module = MF.getAssociatedModule();

  SmallVector<ProtocolConformance *, 2> conformances;
  nominal->lookupConformance(proto, conformances);
  PrettyStackTraceModuleFile traceMsg(
      "If you're seeing a crash here, check that your SDK and dependencies "
      "are at least as new as the versions used to build", MF);
  // This would normally be an assertion but it's more useful to print the
  // PrettyStackTrace here even in no-asserts builds.
  if (conformances.empty())
    abort();
  return conformances.front();
}

Expected<ProtocolConformance *>
ProtocolConformanceDeserializer::readNormalProtocolConformance(
                                              ArrayRef<uint64_t> scratch,
            ModuleFile::Serialized<ProtocolConformance *> &conformanceEntry) {
  using namespace decls_block;

  DeclID protoID;
  DeclContextID contextID;
  unsigned valueCount, typeCount, conformanceCount, isUnchecked;
  ArrayRef<uint64_t> rawIDs;

  NormalProtocolConformanceLayout::readRecord(scratch, protoID,
                                              contextID, typeCount,
                                              valueCount, conformanceCount,
                                              isUnchecked,
                                              rawIDs);

  auto doOrError = MF.getDeclContextChecked(contextID);
  if (!doOrError)
    return doOrError.takeError();
  DeclContext *dc = doOrError.get();

  assert(!isa<ClangModuleUnit>(dc->getModuleScopeContext())
         && "should not have serialized a conformance from a clang module");
  Type conformingType = dc->getDeclaredInterfaceType();
  PrettyStackTraceType trace(ctx, "reading conformance for", conformingType);

  auto protoOrError = MF.getDeclChecked(protoID);
  if (!protoOrError)
    return protoOrError.takeError();
  auto proto = cast<ProtocolDecl>(protoOrError.get());

  PrettyStackTraceDecl traceTo("... to", proto);

  auto conformance = ctx.getConformance(conformingType, proto, SourceLoc(), dc,
                                        ProtocolConformanceState::Incomplete,
                                        isUnchecked);
  // Record this conformance.
  if (conformanceEntry.isComplete()) {
    assert(conformanceEntry.get() == conformance);
    return conformance;
  }

  uint64_t offset = conformanceEntry;
  conformanceEntry = conformance;

  dc->getSelfNominalTypeDecl()->registerProtocolConformance(conformance);

  // If the conformance is complete, we're done.
  if (conformance->isComplete())
    return conformance;

  conformance->setState(ProtocolConformanceState::Complete);
  conformance->setLazyLoader(&MF, offset);
  return conformance;
}

Expected<PackConformance*>
ProtocolConformanceDeserializer::read(
          ModuleFile::Serialized<PackConformance *> &conformanceEntry) {
  using namespace decls_block;

  SmallVector<uint64_t, 16> scratch;

  llvm::BitstreamEntry entry =
      MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize types represented by sub-blocks.
    return MF.diagnoseFatal();
  }

  StringRef blobData;
  unsigned kind = MF.fatalIfUnexpected(
      MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  assert(blobData.empty());

  if (kind != decls_block::PACK_CONFORMANCE)
    return MF.diagnoseFatal(llvm::make_error<InvalidRecordKindError>(kind));

  TypeID patternTypeID;
  DeclID protocolID;
  ArrayRef<uint64_t> patternConformanceIDs;
  PackConformanceLayout::readRecord(scratch,
                                    patternTypeID, protocolID,
                                    patternConformanceIDs);

  auto patternTypeOrError = MF.getTypeChecked(patternTypeID);
  if (!patternTypeOrError)
    return patternTypeOrError.takeError();
  auto patternType = patternTypeOrError.get();

  auto protocolOrError = MF.getDeclChecked(protocolID);
  if (!protocolOrError)
    return protocolOrError.takeError();
  auto *protocol = protocolOrError.get();

  PrettyStackTraceType trace(MF.getAssociatedModule()->getASTContext(),
                             "reading pack conformance for",
                             patternType);

  SmallVector<ProtocolConformanceRef, 4> patternConformances;
  for (auto confID : patternConformanceIDs) {
    auto confOrError = MF.getConformanceChecked(confID);
    if (!confOrError)
      return confOrError.takeError();
    patternConformances.push_back(confOrError.get());
  }

  auto conformance =
         PackConformance::get(patternType->castTo<PackType>(),
                              cast<ProtocolDecl>(protocol),
                              patternConformances);
  return conformance;
}

ProtocolConformanceRef
ModuleFile::getConformance(ProtocolConformanceID id) {
  auto conformance = getConformanceChecked(id);
  if (!conformance)
    fatal(conformance.takeError());
  return conformance.get();
}

Expected<ProtocolConformanceRef>
ModuleFile::getConformanceChecked(ProtocolConformanceID conformanceID) {
  using namespace decls_block;

  if (conformanceID == 0) return ProtocolConformanceRef::forInvalid();

  switch (conformanceID & SerializedProtocolConformanceKind::Mask) {
  case SerializedProtocolConformanceKind::Abstract: {
    auto protocolID = conformanceID >> SerializedProtocolConformanceKind::Shift;
    auto maybeProtocol = getDeclChecked(protocolID);
    if (!maybeProtocol)
      return maybeProtocol.takeError();
    auto proto = cast<ProtocolDecl>(maybeProtocol.get());
    return ProtocolConformanceRef(proto);
  }

  case SerializedProtocolConformanceKind::Concrete: {
    auto conformanceIndex = (conformanceID >> SerializedProtocolConformanceKind::Shift) - 1;
    assert(conformanceIndex < Conformances.size() && "invalid conformance ID");
    auto &conformanceOrOffset = Conformances[conformanceIndex];
    if (!conformanceOrOffset.isComplete()) {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      if (auto error = diagnoseFatalIfNotSuccess(
              DeclTypeCursor.JumpToBit(conformanceOrOffset)))
        return std::move(error);

      auto result =
        ProtocolConformanceDeserializer(*this).read(conformanceOrOffset);
      if (!result)
        return result.takeError();

      conformanceOrOffset = result.get();
    }
    auto conformance = conformanceOrOffset.get();
    return ProtocolConformanceRef(conformance);
  }

  case SerializedProtocolConformanceKind::Pack: {
    auto conformanceIndex = (conformanceID >> SerializedProtocolConformanceKind::Shift) - 1;
    assert(conformanceIndex < PackConformances.size() && "invalid pack conformance ID");
    auto &conformanceOrOffset = PackConformances[conformanceIndex];
    if (!conformanceOrOffset.isComplete()) {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      if (auto error = diagnoseFatalIfNotSuccess(
              DeclTypeCursor.JumpToBit(conformanceOrOffset)))
        return std::move(error);

      auto result =
        ProtocolConformanceDeserializer(*this).read(conformanceOrOffset);
      if (!result)
        return result.takeError();

      conformanceOrOffset = result.get();
    }
    auto conformance = conformanceOrOffset.get();
    return ProtocolConformanceRef(conformance);
  }

  default:
    llvm_unreachable("Invalid conformance");
  }
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

/// Translate from the requirement kind to the Serialization enum
/// values, which are guaranteed to be stable.
static llvm::Optional<RequirementKind>
getActualRequirementKind(uint64_t rawKind) {
#define CASE(KIND)                   \
  case GenericRequirementKind::KIND: \
    return RequirementKind::KIND;

  switch (rawKind) {
  CASE(SameShape)
  CASE(Conformance)
  CASE(Superclass)
  CASE(SameType)
  CASE(Layout)
  }
#undef CASE

  return llvm::None;
}

/// Translate from the requirement kind to the Serialization enum
/// values, which are guaranteed to be stable.
static llvm::Optional<LayoutConstraintKind>
getActualLayoutConstraintKind(uint64_t rawKind) {
#define CASE(KIND)                     \
  case LayoutRequirementKind::KIND:    \
    return LayoutConstraintKind::KIND;

  switch (rawKind) {
  CASE(NativeRefCountedObject)
  CASE(RefCountedObject)
  CASE(Trivial)
  CASE(TrivialOfExactSize)
  CASE(TrivialOfAtMostSize)
  CASE(Class)
  CASE(NativeClass)
  CASE(UnknownLayout)
  }
#undef CASE

  return llvm::None;
}

void ModuleFile::deserializeGenericRequirements(
                   ArrayRef<uint64_t> scratch,
                   unsigned &nextIndex,
                   SmallVectorImpl<Requirement> &requirements) {
  auto error = deserializeGenericRequirementsChecked(scratch, nextIndex,
                                                     requirements);
  if (error)
    fatal(std::move(error));
}

llvm::Error ModuleFile::deserializeGenericRequirementsChecked(
                   ArrayRef<uint64_t> scratch,
                   unsigned &nextIndex,
                   SmallVectorImpl<Requirement> &requirements) {
  using namespace decls_block;

  auto numRequirements = scratch[nextIndex++];
  requirements.reserve(numRequirements);

  for (unsigned i = 0; i != numRequirements; ++i) {
    auto maybeReqKind = getActualRequirementKind(scratch[nextIndex++]);
    if (!maybeReqKind)
      return diagnoseFatal();
    auto reqKind = *maybeReqKind;

    // General case
    if (reqKind != RequirementKind::Layout) {
      auto firstType = getTypeChecked(scratch[nextIndex++]);
      if (!firstType) return firstType.takeError();

      auto secondType = getTypeChecked(scratch[nextIndex++]);
      if (!secondType) return secondType.takeError();

      requirements.push_back(
        Requirement(reqKind, firstType.get(), secondType.get()));

    // Layout constraints
    } else {
      auto maybeKind =
        getActualLayoutConstraintKind(scratch[nextIndex++]);
      if (!maybeKind)
        return diagnoseFatal();
      auto kind = *maybeKind;

      auto type = getTypeChecked(scratch[nextIndex++]);
      if (!type) return type.takeError();

      uint32_t size = scratch[nextIndex++];
      uint32_t alignment = scratch[nextIndex++];

      ASTContext &ctx = getContext();
      LayoutConstraint layout;
      if (kind != LayoutConstraintKind::TrivialOfAtMostSize &&
          kind != LayoutConstraintKind::TrivialOfExactSize)
        layout = LayoutConstraint::getLayoutConstraint(kind, ctx);
      else
        layout =
            LayoutConstraint::getLayoutConstraint(kind, size, alignment, ctx);

      requirements.push_back(Requirement(reqKind, type.get(), layout));
    }
  }

  return llvm::Error::success();
}

void ModuleFile::readRequirementSignature(
                   SmallVectorImpl<Requirement> &requirements,
                   SmallVectorImpl<ProtocolTypeAlias> &typeAliases,
                   llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  // Tentatively advance from this point to see if there's a
  // REQUIREMENT_SIGNATURE record.
  BCOffsetRAII lastRecordOffset(Cursor);

  llvm::BitstreamEntry entry =
      fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return;

  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;
  unsigned recordID = fatalIfUnexpected(
      Cursor.readRecord(entry.ID, scratch, &blobData));

  // This record is not part of the protocol requirement signature.
  if (recordID != REQUIREMENT_SIGNATURE)
    return;

  // Accept the tentative advance.
  lastRecordOffset.reset();

  unsigned nextIndex = 0;
  ArrayRef<uint64_t> rawData;
  RequirementSignatureLayout::readRecord(scratch, rawData);

  deserializeGenericRequirements(rawData, nextIndex, requirements);
  rawData = rawData.slice(nextIndex);

  if (rawData.size() % 2 != 0)
    return diagnoseAndConsumeFatal();

  while (!rawData.empty()) {
    auto name = getIdentifier(rawData[0]);
    auto underlyingType = getType(rawData[1]);
    typeAliases.emplace_back(name, underlyingType);

    rawData = rawData.slice(2);
  }
}

void ModuleFile::readAssociatedTypes(
                   SmallVectorImpl<AssociatedTypeDecl *> &assocTypes,
                   llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  BCOffsetRAII lastRecordOffset(Cursor);
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  while (true) {
    lastRecordOffset.reset();

    llvm::BitstreamEntry entry =
        fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
    if (entry.Kind != llvm::BitstreamEntry::Record)
      break;

    scratch.clear();
    unsigned recordID = fatalIfUnexpected(
        Cursor.readRecord(entry.ID, scratch, &blobData));
    if (recordID != ASSOCIATED_TYPE)
      break;

    DeclID declID;
    AssociatedTypeLayout::readRecord(scratch, declID);

    assocTypes.push_back(cast<AssociatedTypeDecl>(getDecl(declID)));
  }
}

void ModuleFile::readPrimaryAssociatedTypes(
                   SmallVectorImpl<AssociatedTypeDecl *> &assocTypes,
                   llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  BCOffsetRAII lastRecordOffset(Cursor);
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  while (true) {
    lastRecordOffset.reset();

    llvm::BitstreamEntry entry =
        fatalIfUnexpected(Cursor.advance(AF_DontPopBlockAtEnd));
    if (entry.Kind != llvm::BitstreamEntry::Record)
      break;

    scratch.clear();
    unsigned recordID = fatalIfUnexpected(
        Cursor.readRecord(entry.ID, scratch, &blobData));
    if (recordID != PRIMARY_ASSOCIATED_TYPE)
      break;

    DeclID declID;
    PrimaryAssociatedTypeLayout::readRecord(scratch, declID);

    assocTypes.push_back(cast<AssociatedTypeDecl>(getDecl(declID)));
  }
}

static llvm::Error skipRecords(llvm::BitstreamCursor &Cursor, unsigned kind) {
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
    if (maybeRecordID.get() != kind)
      return llvm::Error::success();

    lastRecordOffset.reset();
  }

  return llvm::Error::success();
}

/// Advances past any records that might be part of a protocol requirement
/// signature, which consists of generic requirements together with protocol
/// typealias records.
static llvm::Error skipRequirementSignature(llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  return skipRecords(Cursor, REQUIREMENT_SIGNATURE);
}

/// Advances past any lazy associated type member records.
static llvm::Error skipAssociatedTypeMembers(llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  return skipRecords(Cursor, ASSOCIATED_TYPE);
}

/// Advances past any lazy primary associated type member records.
static llvm::Error skipPrimaryAssociatedTypeMembers(
    llvm::BitstreamCursor &Cursor) {
  using namespace decls_block;

  return skipRecords(Cursor, PRIMARY_ASSOCIATED_TYPE);
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
  if (auto error =
          diagnoseFatalIfNotSuccess(DeclTypeCursor.JumpToBit(sigOffset)))
    return std::move(error);

  // Read the parameter types.
  SmallVector<GenericTypeParamType *, 4> paramTypes;
  StringRef blobData;
  SmallVector<uint64_t, 8> scratch;

  auto entry_or_err = DeclTypeCursor.advance(AF_DontPopBlockAtEnd);
  if (!entry_or_err)
    return diagnoseFatal(entry_or_err.takeError());
  llvm::BitstreamEntry entry = *entry_or_err;

  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseFatal();

  // Helper function to read the generic requirements off the
  // front of the given opaque record.
  SmallVector<Requirement, 4> requirements;
  auto readGenericRequirements =
    [&](ArrayRef<uint64_t> &rawParamIDs) -> llvm::Error {
    unsigned nextIndex = 0;
    auto error = deserializeGenericRequirementsChecked(rawParamIDs, nextIndex,
                                                       requirements);
    rawParamIDs = rawParamIDs.slice(nextIndex);
    return error;
  };

  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  switch (recordID) {
  case GENERIC_SIGNATURE: {
    ArrayRef<uint64_t> rawParamIDs;
    GenericSignatureLayout::readRecord(scratch, rawParamIDs);

    if (auto error = readGenericRequirements(rawParamIDs))
      return std::move(error);

    for (unsigned i = 0, n = rawParamIDs.size(); i != n; ++i) {
      auto paramTy = getType(rawParamIDs[i])->castTo<GenericTypeParamType>();
      paramTypes.push_back(paramTy);
    }
    break;
  }

  case SIL_GENERIC_SIGNATURE: {
    ArrayRef<uint64_t> rawParamIDs;
    SILGenericSignatureLayout::readRecord(scratch, rawParamIDs);

    if (auto error = readGenericRequirements(rawParamIDs))
      return std::move(error);

    if (rawParamIDs.size() % 2 != 0)
      return diagnoseFatal();

    for (unsigned i = 0, n = rawParamIDs.size(); i != n; i += 2) {
      Identifier name = getIdentifier(rawParamIDs[i]);
      auto paramTy = getType(rawParamIDs[i+1])->castTo<GenericTypeParamType>();

      if (!name.empty()) {
        auto *paramDecl = GenericTypeParamDecl::createDeserialized(
            getAssociatedModule(), name, paramTy->getDepth(),
            paramTy->getIndex(), paramTy->isParameterPack(),
            /*isOpaqueType*/ false);
        paramTy = paramDecl->getDeclaredInterfaceType()
                   ->castTo<GenericTypeParamType>();
      }

      paramTypes.push_back(paramTy);
    }
    break;
  }
  default:
    // Not a generic signature; no way to recover.
    fatal(llvm::make_error<InvalidRecordKindError>(recordID));
  }

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

Expected<GenericEnvironment *> ModuleFile::getGenericEnvironmentChecked(
    serialization::GenericEnvironmentID ID) {
  using namespace decls_block;

  assert(ID <= GenericEnvironments.size() &&
         "invalid GenericEnvironment ID");
  auto &envOffset = GenericEnvironments[ID-1];

  // If we've already deserialized this generic environment, return it.
  if (envOffset.isComplete())
    return envOffset.get();

  // Read the generic environment.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (auto error =
          diagnoseFatalIfNotSuccess(DeclTypeCursor.JumpToBit(envOffset)))
    return std::move(error);

  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseFatal();

  StringRef blobData;
  SmallVector<uint64_t, 8> scratch;
  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  if (recordID != GENERIC_ENVIRONMENT)
    fatal(llvm::make_error<InvalidRecordKindError>(recordID));

  unsigned kind;
  GenericSignatureID parentSigID;
  TypeID existentialOrShapeID;
  SubstitutionMapID subsID;
  GenericEnvironmentLayout::readRecord(scratch, kind, existentialOrShapeID,
                                       parentSigID, subsID);

  auto existentialOrShapeTypeOrError = getTypeChecked(existentialOrShapeID);
  if (!existentialOrShapeTypeOrError)
    return existentialOrShapeTypeOrError.takeError();

  auto parentSigOrError = getGenericSignatureChecked(parentSigID);
  if (!parentSigOrError)
    return parentSigOrError.takeError();

  auto contextSubsOrError = getSubstitutionMapChecked(subsID);
  if (!contextSubsOrError)
    return contextSubsOrError.takeError();

  GenericEnvironment *genericEnv = nullptr;
  switch (GenericEnvironmentKind(kind)) {
  case GenericEnvironmentKind::OpenedExistential:
    genericEnv = GenericEnvironment::forOpenedExistential(
        existentialOrShapeTypeOrError.get(), parentSigOrError.get(), UUID::fromTime());
    break;

  case GenericEnvironmentKind::OpenedElement:
    genericEnv = GenericEnvironment::forOpenedElement(
        parentSigOrError.get(), UUID::fromTime(),
        cast<GenericTypeParamType>(
          existentialOrShapeTypeOrError.get()->getCanonicalType()),
        contextSubsOrError.get());
  }

  envOffset = genericEnv;

  return genericEnv;
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
  if (auto error = diagnoseFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(substitutionsOrOffset)))
    return std::move(error);

  // Read the substitution map.
  llvm::BitstreamEntry entry =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseFatal();

  StringRef blobData;
  SmallVector<uint64_t, 8> scratch;
  unsigned recordID = fatalIfUnexpected(
      DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
  if (recordID != SUBSTITUTION_MAP)
    return diagnoseFatal(llvm::make_error<InvalidRecordKindError>(recordID));

  GenericSignatureID genericSigID;
  uint64_t numReplacementIDs;
  ArrayRef<uint64_t> typeAndConformanceIDs;
  SubstitutionMapLayout::readRecord(scratch, genericSigID, numReplacementIDs,
                                    typeAndConformanceIDs);
  auto replacementTypeIDs = typeAndConformanceIDs.slice(0, numReplacementIDs);
  auto conformanceIDs = typeAndConformanceIDs.slice(numReplacementIDs);

  // Generic signature.
  auto genericSigOrError = getGenericSignatureChecked(genericSigID);
  if (!genericSigOrError)
    return genericSigOrError.takeError();

  auto genericSig = genericSigOrError.get();
  if (!genericSig)
    return diagnoseFatal();

  // Load the replacement types.
  SmallVector<Type, 4> replacementTypes;
  replacementTypes.reserve(replacementTypeIDs.size());
  for (auto typeID : replacementTypeIDs) {
    auto typeOrError = getTypeChecked(typeID);
    if (!typeOrError) {
      diagnoseAndConsumeError(typeOrError.takeError());
      continue;
    }
    replacementTypes.push_back(typeOrError.get());
  }

  // Read the conformances.
  SmallVector<ProtocolConformanceRef, 4> conformances;
  conformances.reserve(conformanceIDs.size());
  for (unsigned conformanceID : conformanceIDs) {
    auto conformanceOrError = getConformanceChecked(conformanceID);
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

static llvm::Optional<swift::CtorInitializerKind>
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
  return llvm::None;
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
                         llvm::Optional<swift::CtorInitializerKind> ctorInit,
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

/// Look for nested types in all files of \p extensionModule except from the \p thisFile.
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

/// Look for nested types in all files of \p extensionModule.
static TypeDecl *
findNestedTypeDeclInModule(ModuleDecl *extensionModule,
                           Identifier name, NominalTypeDecl *parent)  {
  return findNestedTypeDeclInModule(nullptr, extensionModule, name, parent);
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
    return diagnoseFatal();

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
        // Pass through deserialization errors.
        if (maybeType.errorIsA<FatalDeserializationError>())
          return maybeType.takeError();
        // FIXME: Don't throw away the inner error's information.
        diagnoseAndConsumeError(maybeType.takeError());
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
                                  SourceLoc(), NL_QualifiedDefault,
                                  values);
    }
    filterValues(filterTy, nullptr, nullptr, isType, inProtocolExt,
                 importedFromClang, isStatic, llvm::None, values);
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
      return diagnoseFatal();
    }
    llvm_unreachable("Unhandled case in switch!");
  }

  case XREF_GENERIC_PARAM_PATH_PIECE:
  case XREF_INITIALIZER_PATH_PIECE:
    llvm_unreachable("only in a nominal or function");

  default:
    // Unknown xref kind.
    pathTrace.addUnknown(recordID);
    fatal(llvm::make_error<InvalidRecordKindError>(recordID));
  }

  auto getXRefDeclNameForError = [&]() -> DeclName {
    BCOffsetRAII restoreOffset(DeclTypeCursor);
    DeclName result = pathTrace.getLastName();
    uint32_t namePathLen = pathLen;
    while (--namePathLen) {
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
        XRefTypePathPieceLayout::readRecord(scratch, IID, llvm::None,
                                            llvm::None, llvm::None);
        result = getIdentifier(IID);
        break;
      }
      case XREF_VALUE_PATH_PIECE: {
        IdentifierID IID;
        XRefValuePathPieceLayout::readRecord(
            scratch, llvm::None, IID, llvm::None, llvm::None, llvm::None);
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
    // Couldn't resolve the reference. Try to explain the problem and leave it
    // up to the caller to recover if possible.

    // Look for types and value decls in other modules. This extra information
    // will be used for diagnostics by the caller logic.
    SmallVector<char, 64> strScratch;

    auto errorKind = ModularizationError::Kind::DeclNotFound;
    ModuleDecl *foundIn = nullptr;
    bool isType = false;

    if (recordID == XREF_TYPE_PATH_PIECE ||
        recordID == XREF_VALUE_PATH_PIECE) {
      auto &ctx = getContext();
      for (auto nameAndModule : ctx.getLoadedModules()) {
        auto otherModule = nameAndModule.second;

        IdentifierID IID;
        IdentifierID privateDiscriminator = 0;
        TypeID TID = 0;
        isType = (recordID == XREF_TYPE_PATH_PIECE);
        bool inProtocolExt = false;
        bool importedFromClang = false;
        bool isStatic = false;
        if (isType) {
          XRefTypePathPieceLayout::readRecord(scratch, IID, privateDiscriminator,
                                              inProtocolExt, importedFromClang);
        } else {
          XRefValuePathPieceLayout::readRecord(scratch, TID, IID, inProtocolExt,
                                               importedFromClang, isStatic);
        }

        DeclBaseName name = getDeclBaseName(IID);
        Type filterTy;
        if (!isType) {
          auto maybeType = getTypeChecked(TID);
          // Any error here would have been handled previously.
          if (maybeType) {
            filterTy = maybeType.get();
          }
        }

        values.clear();
        if (privateDiscriminator) {
          otherModule->lookupMember(values, otherModule, name,
                                   getIdentifier(privateDiscriminator));
        } else {
          otherModule->lookupQualified(otherModule, DeclNameRef(name),
                                      SourceLoc(), NL_QualifiedDefault,
                                      values);
        }

        bool hadAMatchBeforeFiltering = !values.empty();
        filterValues(filterTy, nullptr, nullptr, isType, inProtocolExt,
                     importedFromClang, isStatic, llvm::None, values);

        strScratch.clear();
        if (!values.empty()) {
          // Found a full match in a different module. It should be a different
          // one because otherwise it would have succeeded on the first search.
          // This is usually caused by the use of poorly modularized headers.
          errorKind = ModularizationError::Kind::DeclMoved;
          foundIn = otherModule;
          break;
        } else if (hadAMatchBeforeFiltering) {
          // Found a match that was filtered out. This may be from the same
          // expected module if there's a type difference. This can be caused
          // by the use of different Swift language versions between a library
          // with serialized SIL and a client.
          errorKind = ModularizationError::Kind::DeclKindChanged;
          foundIn = otherModule;
          break;
        }
      }
    }

    auto declName = getXRefDeclNameForError();
    auto error = llvm::make_error<ModularizationError>(declName,
                                                       isType,
                                                       errorKind,
                                                       baseModule,
                                                       this,
                                                       foundIn,
                                                       pathTrace);

    // If we want to workaround broken modularization, we can keep going if
    // we found a matching top-level decl in a different module. This is
    // obviously dangerous as it could just be some other decl that happens to
    // match.
    if (getContext().LangOpts.ForceWorkaroundBrokenModules &&
        errorKind == ModularizationError::Kind::DeclMoved &&
        !values.empty()) {
      // Print the error as a warning and notify of the recovery attempt.
      llvm::handleAllErrors(std::move(error),
        [&](const ModularizationError &modularError) {
          modularError.diagnose(this, DiagnosticBehavior::Warning);
        });
      getContext().Diags.diagnose(SourceLoc(),
                                  diag::modularization_issue_worked_around);
    } else {
      return std::move(error);
    }
  }

  // Filters for values discovered in the remaining path pieces.
  ModuleDecl *M = nullptr;
  CanGenericSignature genericSig = CanGenericSignature();

  // For remaining path pieces, filter or drill down into the results we have.
  while (--pathLen) {
    llvm::BitstreamEntry entry =
        fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
    if (entry.Kind != llvm::BitstreamEntry::Record)
      return diagnoseFatal();

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
        // We include the current file in the search because the cross reference
        // may involve a nested type of this file.
        (void)baseType->getExtensions();
        auto *nestedType =
            findNestedTypeDeclInModule(extensionModule, memberName, baseType);

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
          filterValues(/*expectedTy*/ Type(), extensionModule, genericSig,
                       /*isType*/ true, inProtocolExt, importedFromClang,
                       /*isStatic*/ false, /*ctorInit*/ llvm::None,
                       singleValueBuffer);
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
      llvm::Optional<swift::CtorInitializerKind> ctorInit;
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
        fatal(llvm::make_error<InvalidRecordKindError>(recordID,
                                                       "Unhandled path piece"));
      }

      pathTrace.addValue(memberName);
      if (!privateDiscriminator.empty())
        pathTrace.addPrivateDiscriminator(privateDiscriminator);

      Type filterTy;
      if (!isType) {
        auto maybeType = getTypeChecked(TID);
        if (!maybeType) {
          // Pass through deserialization errors.`
          if (maybeType.errorIsA<FatalDeserializationError>())
            return maybeType.takeError();

          // FIXME: Don't throw away the inner error's information.
          diagnoseAndConsumeError(maybeType.takeError());
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
        return llvm::make_error<XRefError>("module with extension is not loaded",
                                           pathTrace, getIdentifier(ownerID));
      }
      pathTrace.addExtension(M);

      // Read the generic signature, if we have one.
      genericSig = CanGenericSignature(getGenericSignature(rawGenericSig));

      continue;
    }

    case XREF_OPERATOR_OR_ACCESSOR_PATH_PIECE: {
      uint8_t rawKind;
      XRefOperatorOrAccessorPathPieceLayout::readRecord(scratch, llvm::None,
                                                        rawKind);
      if (values.empty())
        break;

      if (!values.front()->getBaseName().isOperator()) {
        pathTrace.addAccessor(rawKind);
        if (auto storage = dyn_cast<AbstractStorageDecl>(values.front())) {
          auto actualKind = getActualAccessorKind(rawKind);
          if (!actualKind) {
            // Unknown accessor kind.
            return diagnoseFatal();
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
      for (auto paramTy : currentSig.getGenericParams()) {
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
      fatal(llvm::make_error<InvalidRecordKindError>(recordID));
    }

    llvm::Optional<PrettyStackTraceModuleFile> traceMsg;
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
    return diagnoseFatal();

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

Expected<DeclContext *>ModuleFile::getLocalDeclContext(LocalDeclContextID DCID) {
  assert(DCID != 0 && "invalid local DeclContext ID 0");
  auto &declContextOrOffset = LocalDeclContexts[DCID-1];

  if (declContextOrOffset.isComplete())
    return declContextOrOffset;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (auto error = diagnoseFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(declContextOrOffset)))
    return std::move(error);
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseFatal();

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
    fatal(llvm::make_error<InvalidRecordKindError>(recordID,
                   "Unknown record ID found when reading local DeclContext."));
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

  if (llvm::Optional<LocalDeclContextID> contextID =
          DCID.getAsLocalDeclContextID())
    return getLocalDeclContext(contextID.value());

  auto deserialized = getDeclChecked(DCID.getAsDeclID().value());
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
  if (auto MD = dyn_cast<MacroDecl>(D))
    return MD;

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
  Identifier parentName = FileContext->getParentModule()->getName();
  if (name.size() == 1 &&
      name.front().Item == getContext().getRealModuleName(parentName)) {
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
static llvm::Optional<swift::Associativity>
getActualAssociativity(uint8_t assoc) {
  switch (assoc) {
  case serialization::Associativity::LeftAssociative:
    return swift::Associativity::Left;
  case serialization::Associativity::RightAssociative:
    return swift::Associativity::Right;
  case serialization::Associativity::NonAssociative:
    return swift::Associativity::None;
  default:
    return llvm::None;
  }
}

static llvm::Optional<swift::StaticSpellingKind>
getActualStaticSpellingKind(uint8_t raw) {
  switch (serialization::StaticSpellingKind(raw)) {
  case serialization::StaticSpellingKind::None:
    return swift::StaticSpellingKind::None;
  case serialization::StaticSpellingKind::KeywordStatic:
    return swift::StaticSpellingKind::KeywordStatic;
  case serialization::StaticSpellingKind::KeywordClass:
    return swift::StaticSpellingKind::KeywordClass;
  }
  return llvm::None;
}

static bool isDeclAttrRecord(unsigned ID) {
  using namespace decls_block;
  switch (ID) {
#define DECL_ATTR(NAME, CLASS, ...) case CLASS##_DECL_ATTR: return true;
#include "DeclTypeRecordNodes.def"
  default: return false;
  }
}

static llvm::Optional<swift::AccessLevel> getActualAccessLevel(uint8_t raw) {
  switch (serialization::AccessLevel(raw)) {
#define CASE(NAME) \
  case serialization::AccessLevel::NAME: \
    return swift::AccessLevel::NAME;
  CASE(Private)
  CASE(FilePrivate)
  CASE(Internal)
  CASE(Package)
  CASE(Public)
  CASE(Open)
#undef CASE
  }
  return llvm::None;
}

static llvm::Optional<swift::SelfAccessKind>
getActualSelfAccessKind(uint8_t raw) {
  switch (serialization::SelfAccessKind(raw)) {
  case serialization::SelfAccessKind::NonMutating:
    return swift::SelfAccessKind::NonMutating;
  case serialization::SelfAccessKind::Mutating:
    return swift::SelfAccessKind::Mutating;
  case serialization::SelfAccessKind::LegacyConsuming:
    return swift::SelfAccessKind::LegacyConsuming;
  case serialization::SelfAccessKind::Consuming:
    return swift::SelfAccessKind::Consuming;
  case serialization::SelfAccessKind::Borrowing:
    return swift::SelfAccessKind::Borrowing;
  }
  return llvm::None;
}

/// Translate from the serialization VarDeclSpecifier enumerators, which are
/// guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::ParamDecl::Specifier>
getActualParamDeclSpecifier(serialization::ParamDeclSpecifier raw) {
  switch (raw) {
#define CASE(ID) \
  case serialization::ParamDeclSpecifier::ID: \
    return swift::ParamDecl::Specifier::ID;
  CASE(Default)
  CASE(InOut)
  CASE(Borrowing)
  CASE(Consuming)
  CASE(LegacyShared)
  CASE(LegacyOwned)
  }
#undef CASE
  return llvm::None;
}

static llvm::Optional<swift::VarDecl::Introducer>
getActualVarDeclIntroducer(serialization::VarDeclIntroducer raw) {
  switch (raw) {
#define CASE(ID) \
  case serialization::VarDeclIntroducer::ID: \
    return swift::VarDecl::Introducer::ID;
  CASE(Let)
  CASE(Var)
  CASE(InOut)
  }
#undef CASE
  return llvm::None;
}

static llvm::Optional<swift::OpaqueReadOwnership>
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
  return llvm::None;
}

static llvm::Optional<swift::ReadImplKind>
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
  return llvm::None;
}

static llvm::Optional<swift::WriteImplKind>
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
  return llvm::None;
}

static llvm::Optional<swift::ReadWriteImplKind>
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
  return llvm::None;
}

/// Translate from the serialization DifferentiabilityKind enumerators, which
/// are guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::AutoDiffDerivativeFunctionKind>
getActualAutoDiffDerivativeFunctionKind(uint8_t raw) {
  switch (serialization::AutoDiffDerivativeFunctionKind(raw)) {
#define CASE(ID)                                                               \
  case serialization::AutoDiffDerivativeFunctionKind::ID:                      \
    return {swift::AutoDiffDerivativeFunctionKind::ID};
  CASE(JVP)
  CASE(VJP)
#undef CASE
  }
  return llvm::None;
}

/// Translate from the Serialization differentiability kind enum values to the
/// AST strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static llvm::Optional<swift::DifferentiabilityKind>
getActualDifferentiabilityKind(uint8_t diffKind) {
  switch (diffKind) {
#define CASE(THE_DK) \
  case (uint8_t)serialization::DifferentiabilityKind::THE_DK: \
    return swift::DifferentiabilityKind::THE_DK;
  CASE(NonDifferentiable)
  CASE(Forward)
  CASE(Reverse)
  CASE(Normal)
  CASE(Linear)
#undef CASE
  default:
    return llvm::None;
  }
}

static llvm::Optional<swift::MacroRole> getActualMacroRole(uint8_t context) {
  switch (context) {
#define CASE(THE_DK) \
  case (uint8_t)serialization::MacroRole::THE_DK: \
    return swift::MacroRole::THE_DK;
  CASE(Expression)
  CASE(Declaration)
  CASE(Accessor)
  CASE(MemberAttribute)
  CASE(Member)
  CASE(Peer)
  CASE(Conformance)
  CASE(CodeItem)
  CASE(Extension)
#undef CASE
  }
  return llvm::None;
}

static llvm::Optional<swift::MacroIntroducedDeclNameKind>
getActualMacroIntroducedDeclNameKind(uint8_t context) {
  switch (context) {
#define CASE(THE_DK) \
  case (uint8_t)serialization::MacroIntroducedDeclNameKind::THE_DK: \
    return swift::MacroIntroducedDeclNameKind::THE_DK;
  CASE(Named)
  CASE(Overloaded)
  CASE(Prefixed)
  CASE(Suffixed)
  CASE(Arbitrary)
#undef CASE
  default:
    return llvm::None;
  }
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

  auto implInfo = StorageImplInfo(*readImpl, *writeImpl, *readWriteImpl);
  decl->setImplInfo(implInfo);

  decl->getASTContext().evaluator.cacheOutput(HasStorageRequest{decl}, implInfo.hasStorage());

  SmallVector<AccessorDecl*, 8> accessors;
  for (DeclID id : rawIDs.IDs) {
    auto accessor = dyn_cast_or_null<AccessorDecl>(getDecl(id));
    if (!accessor) return;
    accessors.push_back(accessor);
  }

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
  for (auto *attr : tempAttrs.getAttributes<DerivativeAttr>()) {
    auto *derAttr = const_cast<DerivativeAttr *>(attr);
    derAttr->setOriginalDeclaration(decl);
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

  bool IsInvalid = false;

  DeclAttribute *DAttrs = nullptr;
  DeclAttribute **AttrsNext = &DAttrs;
  SmallVector<serialization::BitOffset> customAttrOffsets;

  Identifier privateDiscriminator;
  unsigned localDiscriminator = ValueDecl::InvalidDiscriminator;
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
    SmallVector<InheritedEntry, 2> inheritedTypes;
    for (auto rawID : rawInheritedIDs) {
      // The low bit indicates "@unchecked".
      bool isUnchecked = rawID & 0x01;
      TypeID typeID = rawID >> 1;

      auto maybeType = MF.getTypeChecked(typeID);
      if (!maybeType) {
        MF.diagnoseAndConsumeError(maybeType.takeError());
        continue;
      }
      inheritedTypes.push_back(
          InheritedEntry(TypeLoc::withoutLoc(maybeType.get()), isUnchecked));
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

    if (IsInvalid) {
      decl->setInvalidBit();

      DeclName name;
      if (auto *VD = dyn_cast<ValueDecl>(decl)) {
        name = VD->getName();
      }

      auto diagId = MF.allowCompilerErrors()
                        ? diag::serialization_allowing_invalid_decl
                        : diag::serialization_invalid_decl;
      ctx.Diags.diagnose(SourceLoc(), diagId, name,
                         decl->getDescriptiveKind(),
                         MF.getAssociatedModule()->getNameStr());
    }

    if (DAttrs)
      decl->getAttrs().setRawAttributeChain(DAttrs);

    if (auto value = dyn_cast<ValueDecl>(decl)) {
      if (!privateDiscriminator.empty())
        MF.PrivateDiscriminatorsByValue[value] = privateDiscriminator;

      if (localDiscriminator != ValueDecl::InvalidDiscriminator)
        value->setLocalDiscriminator(localDiscriminator);

      if (!filenameForPrivate.empty())
        MF.FilenamesForPrivateValues[value] = filenameForPrivate;
    }
  }

  /// Deserializes records common to all decls from \c MF.DeclTypesCursor (ie.
  /// the invalid flag, attributes, and discriminators)
  ///
  /// Reads all attributes except for custom attributes that are skipped and
  /// their offsets added to \c customAttrOffsets.
  llvm::Error deserializeDeclCommon();

  /// Deserializes the custom attributes from \c MF.DeclTypesCursor, using the
  /// offsets in \c customAttrOffsets.
  llvm::Error deserializeCustomAttrs();

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
      return MF.diagnoseFatal();

    if (isImplicit)
      alias->setImplicit();

    return alias;
  }

  Expected<Decl *>
  deserializeGenericTypeParamDecl(ArrayRef<uint64_t> scratch,
                                  StringRef blobData) {
    IdentifierID nameID;
    bool isImplicit;
    bool isParameterPack;
    unsigned depth;
    unsigned index;
    bool isOpaqueType;

    decls_block::GenericTypeParamDeclLayout::readRecord(
        scratch, nameID, isImplicit, isParameterPack, depth, index,
        isOpaqueType);

    // Always create GenericTypeParamDecls in the associated file; the real
    // context will reparent them.
    auto *DC = MF.getFile();
    auto *genericParam = GenericTypeParamDecl::createDeserialized(
        DC, MF.getIdentifier(nameID), depth, index, isParameterPack,
        isOpaqueType);
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
    ArrayRef<uint64_t> rawIDs;

    decls_block::StructLayout::readRecord(scratch, nameID, contextID,
                                          isImplicit, isObjC, genericSigID,
                                          rawAccessLevel,
                                          numConformances, numInheritedTypes,
                                          rawIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto conformanceIDs = rawIDs.slice(0, numConformances);
    rawIDs = rawIDs.slice(numConformances);
    auto inheritedIDs = rawIDs.slice(0, numInheritedTypes);
    auto dependencyIDs = rawIDs.slice(numInheritedTypes);

    for (TypeID dependencyID : dependencyIDs) {
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
                                               llvm::None, genericParams, DC);
    declOrOffset = theStruct;

    // Read the generic environment.
    theStruct->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theStruct->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    theStruct->setAddedImplicitInitializers();
    if (isImplicit)
      theStruct->setImplicit();
    theStruct->setIsObjC(isObjC);

    handleInherited(theStruct, inheritedIDs);

    theStruct->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theStruct->setConformanceLoader(
      &MF, MF.createLazyConformanceLoaderToken(conformanceIDs));

    return theStruct;
  }

  Expected<Decl *> deserializeConstructor(ArrayRef<uint64_t> scratch,
                                          StringRef blobData) {
    DeclContextID contextID;
    bool isIUO, isFailable;
    bool isImplicit, isObjC, hasStubImplementation, throws, async;
    GenericSignatureID genericSigID;
    uint8_t storedInitKind, rawAccessLevel;
    DeclID overriddenID;
    bool overriddenAffectsABI, needsNewVTableEntry, firstTimeRequired;
    unsigned numArgNames;
    ArrayRef<uint64_t> argNameAndDependencyIDs;

    decls_block::ConstructorLayout::readRecord(scratch, contextID,
                                               isFailable, isIUO, isImplicit,
                                               isObjC, hasStubImplementation,
                                               async, throws, storedInitKind,
                                               genericSigID,
                                               overriddenID,
                                               overriddenAffectsABI,
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

    llvm::Optional<swift::CtorInitializerKind> initKind =
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

    Expected<Decl *> overriddenOrError = MF.getDeclChecked(overriddenID);
    Decl *overridden;
    if (overriddenOrError) {
      overridden = overriddenOrError.get();
    } else if (overriddenOrError.errorIsA<FatalDeserializationError>()) {
      // Pass through fatal deserialization errors.
      return overriddenOrError.takeError();
    } else if (MF.allowCompilerErrors()) {
      // Drop overriding relationship when allowing errors.
      MF.diagnoseAndConsumeError(overriddenOrError.takeError());
      overridden = nullptr;
    } else {
      MF.diagnoseAndConsumeError(overriddenOrError.takeError());
      if (overriddenAffectsABI || !ctx.LangOpts.EnableDeserializationRecovery) {
        return llvm::make_error<OverrideError>(name, errorFlags,
                                               numVTableEntries);
      }

      overridden = nullptr;
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
                                               /*Async=*/async,
                                               /*AsyncLoc=*/SourceLoc(),
                                               /*Throws=*/throws,
                                               /*ThrowsLoc=*/SourceLoc(),
                                               /*BodyParams=*/nullptr,
                                               genericParams, parent);
    declOrOffset = ctor;

    ctor->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      ctor->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    auto *bodyParams = MF.readParameterList();
    assert(bodyParams && "missing parameters for constructor");
    ctor->setParameters(bodyParams);

    if (auto errorConvention = MF.maybeReadForeignErrorConvention())
      ctor->setForeignErrorConvention(*errorConvention);
    if (auto asyncConvention = MF.maybeReadForeignAsyncConvention())
      ctor->setForeignAsyncConvention(*asyncConvention);

    if (auto bodyText = MF.maybeReadInlinableBodyText())
      ctor->setBodyStringRepresentation(*bodyText);

    if (isImplicit)
      ctor->setImplicit();
    ctor->setIsObjC(isObjC);
    if (hasStubImplementation)
      ctor->setStubImplementation(true);
    if (initKind.has_value())
      ctx.evaluator.cacheOutput(InitKindRequest{ctor},
                                std::move(initKind.value()));
    ctx.evaluator.cacheOutput(NeedsNewVTableEntryRequest{ctor},
                              std::move(needsNewVTableEntry));

    ctor->setOverriddenDecl(cast_or_null<ConstructorDecl>(overridden));
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
      // Pass through deserialization errors.
      if (overridden.errorIsA<FatalDeserializationError>())
        return overridden.takeError();

      MF.diagnoseAndConsumeError(overridden.takeError());

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
      return MF.diagnoseFatal();

    auto var = MF.createDecl<VarDecl>(/*IsStatic*/ isStatic, *introducer,
                                      SourceLoc(), name, DC);
    var->setIsGetterMutating(isGetterMutating);
    var->setIsSetterMutating(isSetterMutating);
    declOrOffset = var;

    MF.configureStorage(var, opaqueReadOwnership,
                        readImpl, writeImpl, readWriteImpl, accessors);

    auto interfaceTypeOrError = MF.getTypeChecked(interfaceTypeID);
    if (!interfaceTypeOrError)
      return interfaceTypeOrError.takeError();
    Type interfaceType = interfaceTypeOrError.get();
    var->setInterfaceType(interfaceType);
    var->setImplicitlyUnwrappedOptional(isIUO);

    if (auto referenceStorage = interfaceType->getAs<ReferenceStorageType>())
      AddAttribute(
          new (ctx) ReferenceOwnershipAttr(referenceStorage->getOwnership()));

    auto accessLevel = getActualAccessLevel(rawAccessLevel);
    if (!accessLevel)
      return MF.diagnoseFatal();

    var->setAccess(*accessLevel);

    if (var->isSettable(nullptr)) {
      auto setterAccess = getActualAccessLevel(rawSetterAccessLevel);
      if (!setterAccess)
        return MF.diagnoseFatal();
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
        // Pass through deserialization errors.
        if (backingDecl.errorIsA<FatalDeserializationError>())
          return backingDecl.takeError();

        // FIXME: This is actually wrong. We can't just drop stored properties
        // willy-nilly if the struct is @frozen.
        MF.diagnoseAndConsumeError(backingDecl.takeError());
        return var;
      }

      VarDecl *backingVar = cast<VarDecl>(backingDecl.get());
      VarDecl *projectionVar = nullptr;
      if (numBackingProperties > 1) {
        projectionVar = cast<VarDecl>(MF.getDecl(backingPropertyIDs[1]));
      }

      PropertyWrapperAuxiliaryVariables vars(backingVar, projectionVar);
      ctx.evaluator.cacheOutput(
          PropertyWrapperAuxiliaryVariablesRequest{var}, std::move(vars));
      ctx.evaluator.cacheOutput(
          PropertyWrapperInitializerInfoRequest{var},
          PropertyWrapperInitializerInfo());
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
    bool isIsolated;
    bool isCompileTimeConst;
    uint8_t rawDefaultArg;
    TypeID defaultExprType;

    decls_block::ParamLayout::readRecord(scratch, argNameID, paramNameID,
                                         contextID, rawSpecifier,
                                         interfaceTypeID, isIUO, isVariadic,
                                         isAutoClosure, isIsolated,
                                         isCompileTimeConst,
                                         rawDefaultArg,
                                         defaultExprType);

    auto argName = MF.getIdentifier(argNameID);
    auto paramName = MF.getIdentifier(paramNameID);
    PrettySupplementalDeclNameTrace trace(paramName);

    auto DC = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto specifier = getActualParamDeclSpecifier(
                              (serialization::ParamDeclSpecifier)rawSpecifier);
    if (!specifier)
      return MF.diagnoseFatal();

    auto param = MF.createDecl<ParamDecl>(SourceLoc(), SourceLoc(), argName,
                                          SourceLoc(), paramName, DC);
    param->setSpecifier(*specifier);

    declOrOffset = param;

    auto paramTy = MF.getType(interfaceTypeID);
    if (paramTy->hasError() && !MF.allowCompilerErrors()) {
      // FIXME: This should never happen, because we don't serialize
      // error types.
      DC->printContext(llvm::errs());
      paramTy->dump(llvm::errs());
      return MF.diagnoseFatal();
    }

    param->setInterfaceType(paramTy);
    param->setImplicitlyUnwrappedOptional(isIUO);
    param->setVariadic(isVariadic);
    param->setAutoClosure(isAutoClosure);
    param->setIsolated(isIsolated);
    param->setCompileTimeConst(isCompileTimeConst);

    // Decode the default argument kind.
    // FIXME: Default argument expression, if available.
    if (auto defaultArg = getActualDefaultArgKind(rawDefaultArg)) {
      param->setDefaultArgumentKind(*defaultArg);

      if (auto exprType = MF.getType(defaultExprType))
        param->setDefaultExprType(exprType);

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
    bool isDistributedThunk;
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
                                          isDistributedThunk,
                                          nameAndDependencyIDs);
    } else {
      decls_block::AccessorLayout::readRecord(scratch, contextID, isImplicit,
                                              isStatic, rawStaticSpelling, isObjC,
                                              rawMutModifier,
                                              hasForcedStaticDispatch,
                                              async, throws,
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
                                              isDistributedThunk,
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
        return MF.diagnoseFatal();

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
      if (overriddenAffectsABI || !ctx.LangOpts.EnableDeserializationRecovery) {
        MF.diagnoseAndConsumeError(overriddenOrError.takeError());
        return llvm::make_error<OverrideError>(name, errorFlags,
                                               numVTableEntries);
      }
      // Pass through deserialization errors.
      if (overriddenOrError.errorIsA<FatalDeserializationError>())
        return overriddenOrError.takeError();

      MF.diagnoseAndConsumeError(overriddenOrError.takeError());
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
    if (!staticSpelling.has_value())
      return MF.diagnoseFatal();

    if (declOrOffset.isComplete())
      return declOrOffset;

    auto resultTypeOrError = MF.getTypeChecked(resultInterfaceTypeID);
    if (!resultTypeOrError)
      return resultTypeOrError.takeError();
    const auto resultType = resultTypeOrError.get();
    if (declOrOffset.isComplete())
      return declOrOffset;

    FuncDecl *fn;
    if (!isAccessor) {
      fn = FuncDecl::createDeserialized(ctx, staticSpelling.value(), name,
                                        async, throws, genericParams,
                                        resultType, DC);
    } else {
      auto *accessor = AccessorDecl::createDeserialized(
          ctx, accessorKind, storage, staticSpelling.value(), async, throws,
          resultType, DC);
      accessor->setIsTransparent(isTransparent);

      fn = accessor;
    }
    declOrOffset = fn;

    fn->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      fn->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    if (auto SelfAccessKind = getActualSelfAccessKind(rawMutModifier))
      fn->setSelfAccessKind(*SelfAccessKind);
    else
      return MF.diagnoseFatal();

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
    if (auto asyncConvention = MF.maybeReadForeignAsyncConvention())
      fn->setForeignAsyncConvention(*asyncConvention);

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
      auto declOrError = MF.getDeclChecked(opaqueReturnTypeID);
      if (!declOrError)
        return declOrError.takeError();

      ctx.evaluator.cacheOutput(
          OpaqueResultTypeRequest{fn},
          cast<OpaqueTypeDecl>(declOrError.get()));
    }

    if (!isAccessor)
      fn->setUserAccessible(isUserAccessible);

    fn->setDistributedThunk(isDistributedThunk);

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

  void deserializeConditionalSubstitutionConditions(
      SmallVectorImpl<OpaqueTypeDecl::AvailabilityCondition> &conditions) {
    using namespace decls_block;

    SmallVector<uint64_t, 4> scratch;
    StringRef blobData;

    while (true) {
      llvm::BitstreamEntry entry =
          MF.fatalIfUnexpected(MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();

      unsigned recordID = MF.fatalIfUnexpected(
          MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
      if (recordID != decls_block::CONDITIONAL_SUBSTITUTION_COND)
        break;

      bool isUnavailability;
      DEF_VER_TUPLE_PIECES(condition);

      ConditionalSubstitutionConditionLayout::readRecord(
          scratch, isUnavailability, LIST_VER_TUPLE_PIECES(condition));

      llvm::VersionTuple condition;
      DECODE_VER_TUPLE(condition);

      conditions.push_back(std::make_pair(VersionRange::allGTE(condition),
                                          isUnavailability));
    }
  }

  void deserializeConditionalSubstitutions(
      SmallVectorImpl<OpaqueTypeDecl::ConditionallyAvailableSubstitutions *>
          &limitedAvailability) {
    SmallVector<uint64_t, 4> scratch;
    StringRef blobData;

    while (true) {
      llvm::BitstreamEntry entry =
          MF.fatalIfUnexpected(MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();
      unsigned recordID = MF.fatalIfUnexpected(
          MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
      if (recordID != decls_block::CONDITIONAL_SUBSTITUTION)
        break;

      SubstitutionMapID substitutionMapRef;

      decls_block::ConditionalSubstitutionLayout::readRecord(
          scratch, substitutionMapRef);

      SmallVector<OpaqueTypeDecl::AvailabilityCondition, 2> conditions;
      deserializeConditionalSubstitutionConditions(conditions);

      if (conditions.empty())
        return MF.diagnoseAndConsumeFatal();

      auto subMapOrError = MF.getSubstitutionMapChecked(substitutionMapRef);
      if (!subMapOrError)
        return MF.diagnoseAndConsumeFatal();

      limitedAvailability.push_back(
          OpaqueTypeDecl::ConditionallyAvailableSubstitutions::get(
              ctx, conditions, subMapOrError.get()));
    }
  }

  Expected<Decl *> deserializeOpaqueType(ArrayRef<uint64_t> scratch,
                                         StringRef blobData) {
    DeclID namingDeclID;
    DeclContextID contextID;
    GenericSignatureID interfaceSigID;
    TypeID interfaceTypeID;
    GenericSignatureID genericSigID;
    SubstitutionMapID underlyingTypeSubsID;
    uint8_t rawAccessLevel;
    decls_block::OpaqueTypeLayout::readRecord(scratch, contextID,
                                              namingDeclID, interfaceSigID,
                                              interfaceTypeID, genericSigID,
                                              underlyingTypeSubsID,
                                              rawAccessLevel);
    
    auto declContext = MF.getDeclContext(contextID);
    auto interfaceSig = MF.getGenericSignature(interfaceSigID);

    // Check for reentrancy.
    if (declOrOffset.isComplete())
      return cast<OpaqueTypeDecl>(declOrOffset.get());

    auto genericParams = MF.maybeReadGenericParams(declContext);

    // Create the decl.
    auto opaqueDecl = OpaqueTypeDecl::get(
        /*NamingDecl=*/ nullptr, genericParams, declContext,
        interfaceSig, /*OpaqueReturnTypeReprs*/ { });
    declOrOffset = opaqueDecl;

    auto namingDecl = cast<ValueDecl>(MF.getDecl(namingDeclID));
    opaqueDecl->setNamingDecl(namingDecl);

    auto interfaceType = MF.getType(interfaceTypeID);
    opaqueDecl->setInterfaceType(MetatypeType::get(interfaceType));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      opaqueDecl->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    auto genericSig = MF.getGenericSignature(genericSigID);
    if (genericSig)
      opaqueDecl->setGenericSignature(genericSig);
    else
      opaqueDecl->setGenericSignature(GenericSignature());

    auto *AFD = dyn_cast<AbstractFunctionDecl>(namingDecl);
    if (MF.getResilienceStrategy() == ResilienceStrategy::Resilient &&
        !MF.FileContext->getParentModule()->isMainModule() &&
        AFD && AFD->getResilienceExpansion() != ResilienceExpansion::Minimal) {
      // Do not try to read the underlying type information if the function
      // is not inlinable in clients. This reflects the swiftinterface behavior
      // in where clients are only aware of the underlying type when the body
      // of the function is public.

    } else if (underlyingTypeSubsID) {
      auto subMapOrError = MF.getSubstitutionMapChecked(underlyingTypeSubsID);
      if (!subMapOrError) {
        // If the underlying type references internal details, ignore it.
        auto unconsumedError =
          MF.consumeExpectedError(subMapOrError.takeError());
        if (unconsumedError)
          return std::move(unconsumedError);
      } else {
        // Check whether there are any conditionally available substitutions.
        // If there are, it means that "unique" we just read is a universally
        // available substitution.
        SmallVector<OpaqueTypeDecl::ConditionallyAvailableSubstitutions *>
            limitedAvailability;

        deserializeConditionalSubstitutions(limitedAvailability);

        if (limitedAvailability.empty()) {
          opaqueDecl->setUniqueUnderlyingTypeSubstitutions(subMapOrError.get());
        } else {
          limitedAvailability.push_back(
              OpaqueTypeDecl::ConditionallyAvailableSubstitutions::get(
                  ctx, {{VersionRange::empty(), /*unavailability=*/false}},
                  subMapOrError.get()));

          opaqueDecl->setConditionallyAvailableSubstitutions(limitedAvailability);
        }
      }
    }
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
    if (!StaticSpelling.has_value())
      return MF.diagnoseFatal();

    auto dc = MF.getDeclContext(contextID);

    SmallVector<std::pair<Pattern *, DeclContextID>, 4> patterns;
    for (unsigned i = 0; i != numPatterns; ++i) {
      auto pattern = MF.readPattern(dc);
      if (!pattern) {
        // Pass through deserialization errors.
        if (pattern.errorIsA<FatalDeserializationError>())
          return pattern.takeError();

        // Silently drop the pattern...
        MF.diagnoseAndConsumeError(pattern.takeError());
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
                                             StaticSpelling.value(),
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
    bool isImplicit, isClassBounded, isObjC, hasSelfOrAssocTypeRequirements;
    uint8_t rawAccessLevel;
    unsigned numInheritedTypes;
    ArrayRef<uint64_t> rawInheritedAndDependencyIDs;

    decls_block::ProtocolLayout::readRecord(scratch, nameID, contextID,
                                            isImplicit, isClassBounded, isObjC,
                                            hasSelfOrAssocTypeRequirements,
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

    auto proto = MF.createDecl<ProtocolDecl>(
        DC, SourceLoc(), SourceLoc(), name,
        ArrayRef<PrimaryAssociatedTypeName>(), llvm::None,
        /*TrailingWhere=*/nullptr);
    declOrOffset = proto;

    ctx.evaluator.cacheOutput(ProtocolRequiresClassRequest{proto},
                              std::move(isClassBounded));
    ctx.evaluator.cacheOutput(HasSelfOrAssociatedTypeRequirementsRequest{proto},
                              std::move(hasSelfOrAssocTypeRequirements));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      proto->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    auto genericParams = MF.maybeReadGenericParams(DC);
    assert(genericParams && "protocol with no generic parameters?");
    ctx.evaluator.cacheOutput(GenericParamListRequest{proto},
                              std::move(genericParams));

    handleInherited(proto,
                    rawInheritedAndDependencyIDs.slice(0, numInheritedTypes));

    if (isImplicit)
      proto->setImplicit();
    proto->setIsObjC(isObjC);

    proto->setLazyRequirementSignature(
        &MF, MF.DeclTypeCursor.GetCurrentBitNo());
    if (llvm::Error Err = skipRequirementSignature(MF.DeclTypeCursor))
      MF.fatal(std::move(Err));

    proto->setLazyAssociatedTypeMembers(
        &MF, MF.DeclTypeCursor.GetCurrentBitNo());
    if (llvm::Error Err = skipAssociatedTypeMembers(MF.DeclTypeCursor))
      MF.fatal(std::move(Err));

    proto->setLazyPrimaryAssociatedTypeMembers(
        &MF, MF.DeclTypeCursor.GetCurrentBitNo());
    if (llvm::Error Err = skipPrimaryAssociatedTypeMembers(MF.DeclTypeCursor))
      MF.fatal(std::move(Err));

    proto->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());

    return proto;
  }

  template <typename OperatorLayout, typename OperatorDecl>
  Expected<Decl *> deserializeUnaryOperator(ArrayRef<uint64_t> scratch,
                                            StringRef blobData) {
    IdentifierID nameID;
    DeclContextID contextID;

    OperatorLayout::readRecord(scratch, nameID, contextID);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto DC = MF.getDeclContext(contextID);

    auto result = MF.createDecl<OperatorDecl>(
        DC, SourceLoc(), name, SourceLoc());

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

    decls_block::InfixOperatorLayout::readRecord(scratch, nameID, contextID,
                                                 precedenceGroupID);
    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    Expected<Decl *> precedenceGroup = MF.getDeclChecked(precedenceGroupID);
    if (!precedenceGroup)
      return precedenceGroup.takeError();

    auto DC = MF.getDeclContext(contextID);

    auto result = MF.createDecl<InfixOperatorDecl>(
        DC, SourceLoc(), name, SourceLoc(), SourceLoc(), Identifier(),
        SourceLoc());
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
    if (!associativity.has_value())
      return MF.diagnoseFatal();

    if (numHigherThan > rawRelations.size())
      return MF.diagnoseFatal();

    SmallVector<PrecedenceGroupDecl::Relation, 4> higherThan;
    for (auto relID : rawRelations.slice(0, numHigherThan)) {
      PrecedenceGroupDecl *rel = nullptr;
      if (relID)
        rel = dyn_cast_or_null<PrecedenceGroupDecl>(MF.getDecl(relID));
      if (!rel)
        return MF.diagnoseFatal();

      higherThan.push_back({SourceLoc(), rel->getName(), rel});
    }

    SmallVector<PrecedenceGroupDecl::Relation, 4> lowerThan;
    for (auto relID : rawRelations.slice(numHigherThan)) {
      PrecedenceGroupDecl *rel = nullptr;
      if (relID)
        rel = dyn_cast_or_null<PrecedenceGroupDecl>(MF.getDecl(relID));
      if (!rel)
        return MF.diagnoseFatal();

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
    bool isExplicitActorDecl;
    bool inheritsSuperclassInitializers;
    bool hasMissingDesignatedInits;
    GenericSignatureID genericSigID;
    TypeID superclassID;
    uint8_t rawAccessLevel;
    unsigned numConformances, numInheritedTypes;
    ArrayRef<uint64_t> rawIDs;
    decls_block::ClassLayout::readRecord(scratch, nameID, contextID,
                                         isImplicit, isObjC,
                                         isExplicitActorDecl,
                                         inheritsSuperclassInitializers,
                                         hasMissingDesignatedInits,
                                         genericSigID, superclassID,
                                         rawAccessLevel, numConformances,
                                         numInheritedTypes,
                                         rawIDs);

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto conformanceIDs = rawIDs.slice(0, numConformances);
    rawIDs = rawIDs.slice(numConformances);
    auto inheritedIDs = rawIDs.slice(0, numInheritedTypes);
    auto dependencyIDs = rawIDs.slice(numInheritedTypes);

    for (TypeID dependencyID : dependencyIDs) {
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

    auto theClass =
        MF.createDecl<ClassDecl>(SourceLoc(), name, SourceLoc(), llvm::None,
                                 genericParams, DC, isExplicitActorDecl);
    declOrOffset = theClass;

    theClass->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theClass->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    theClass->setAddedImplicitInitializers();
    if (isImplicit)
      theClass->setImplicit();
    theClass->setIsObjC(isObjC);
    theClass->setSuperclass(MF.getType(superclassID));
    ctx.evaluator.cacheOutput(InheritsSuperclassInitializersRequest{theClass},
                              std::move(inheritsSuperclassInitializers));
    ctx.evaluator.cacheOutput(HasMissingDesignatedInitializersRequest{theClass},
                              std::move(hasMissingDesignatedInits));

    handleInherited(theClass, inheritedIDs);

    theClass->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theClass->setConformanceLoader(
      &MF, MF.createLazyConformanceLoaderToken(conformanceIDs));

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
    ArrayRef<uint64_t> rawIDs;

    decls_block::EnumLayout::readRecord(scratch, nameID, contextID,
                                        isImplicit, isObjC, genericSigID,
                                        rawTypeID, rawAccessLevel,
                                        numConformances, numInherited,
                                        rawIDs);

    if (declOrOffset.isComplete())
      return declOrOffset;

    Identifier name = MF.getIdentifier(nameID);
    PrettySupplementalDeclNameTrace trace(name);

    auto conformanceIDs = rawIDs.slice(0, numConformances);
    rawIDs = rawIDs.slice(numConformances);
    auto inheritedIDs = rawIDs.slice(0, numInherited);
    auto dependencyIDs = rawIDs.slice(numInherited);

    for (TypeID dependencyID : dependencyIDs) {
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

    auto theEnum = MF.createDecl<EnumDecl>(SourceLoc(), name, SourceLoc(),
                                           llvm::None, genericParams, DC);

    declOrOffset = theEnum;

    theEnum->setGenericSignature(MF.getGenericSignature(genericSigID));

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      theEnum->setAccess(*accessLevel);
    else
      return MF.diagnoseFatal();

    theEnum->setAddedImplicitInitializers();
    // @objc enums have all their raw values checked.
    if (isObjC) {
      theEnum->setHasFixedRawValues();
    }
    
    if (isImplicit)
      theEnum->setImplicit();
    theEnum->setIsObjC(isObjC);

    theEnum->setRawType(MF.getType(rawTypeID));

    handleInherited(theEnum, inheritedIDs);

    theEnum->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    theEnum->setConformanceLoader(
      &MF, MF.createLazyConformanceLoaderToken(conformanceIDs));
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

    // Extract the accessor IDs.
    for (DeclID accessorID : argNameAndDependencyIDs.slice(0, numAccessors)) {
      accessors.IDs.push_back(accessorID);
    }
    argNameAndDependencyIDs = argNameAndDependencyIDs.slice(numAccessors);

    Expected<Decl *> overridden = MF.getDeclChecked(overriddenID);
    if (!overridden) {
      // Pass through deserialization errors.
      if (overridden.errorIsA<FatalDeserializationError>())
        return overridden.takeError();
      MF.diagnoseAndConsumeError(overridden.takeError());

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
    if (!staticSpelling.has_value())
      return MF.diagnoseFatal();

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
      return MF.diagnoseFatal();

    if (subscript->supportsMutation()) {
      if (auto setterAccess = getActualAccessLevel(rawSetterAccessLevel))
        subscript->setSetterAccess(*setterAccess);
      else
        return MF.diagnoseFatal();
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
    ArrayRef<uint64_t> data;

    decls_block::ExtensionLayout::readRecord(scratch, extendedTypeID,
                                             extendedNominalID, contextID,
                                             isImplicit, genericSigID,
                                             numConformances, numInherited,
                                             data);

    auto DC = MF.getDeclContext(contextID);

    auto conformanceIDs = data.slice(0, numConformances);
    data = data.slice(numConformances);
    auto inheritedIDs = data.slice(0, numInherited);
    data = data.slice(numInherited);
    auto dependencyIDs = data;

    for (TypeID dependencyID : dependencyIDs) {
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
    auto nominal = dyn_cast_or_null<NominalTypeDecl>(MF.getDecl(extendedNominalID));
    ctx.evaluator.cacheOutput(ExtendedNominalRequest{extension},
                              std::move(nominal));

    if (isImplicit)
      extension->setImplicit();

    handleInherited(extension, inheritedIDs);

    extension->setMemberLoader(&MF, MF.DeclTypeCursor.GetCurrentBitNo());
    skipRecord(MF.DeclTypeCursor, decls_block::MEMBERS);
    extension->setConformanceLoader(
      &MF, MF.createLazyConformanceLoaderToken(conformanceIDs));

    if (nominal) {
      nominal->addExtension(extension);
    }

#ifndef NDEBUG
    if (outerParams) {
      unsigned paramCount = 0;
      for (auto *paramList = outerParams;
           paramList != nullptr;
           paramList = paramList->getOuterParameters()) {
        paramCount += paramList->size();
      }
      assert(paramCount ==
             extension->getGenericSignature().getGenericParams().size());
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

    auto *nom = cast<NominalTypeDecl>(DC->getImplementedObjCContext());
    dtor->setAccess(std::max(nom->getFormalAccess(), AccessLevel::Internal));

    if (isImplicit)
      dtor->setImplicit();
    dtor->setIsObjC(isObjC);

    return dtor;
  }

  Expected<Decl *> deserializeMacro(ArrayRef<uint64_t> scratch,
                                    StringRef blobData) {
    DeclContextID contextID;
    bool isImplicit, hasParameterList;
    GenericSignatureID genericSigID;
    TypeID resultInterfaceTypeID;
    uint8_t rawAccessLevel;
    unsigned numArgNames;
    unsigned builtinID;
    uint8_t hasExpandedMacroDefinition;
    IdentifierID externalModuleNameID;
    IdentifierID externalMacroTypeNameID;

    ArrayRef<uint64_t> argNameAndDependencyIDs;

    decls_block::MacroLayout::readRecord(scratch, contextID,
                                         isImplicit,
                                         genericSigID,
                                         hasParameterList,
                                         resultInterfaceTypeID,
                                         rawAccessLevel,
                                         numArgNames,
                                         builtinID,
                                         hasExpandedMacroDefinition,
                                         externalModuleNameID,
                                         externalMacroTypeNameID,
                                         argNameAndDependencyIDs);

    // Get the base name.
    DeclBaseName baseName = MF.getDeclBaseName(argNameAndDependencyIDs.front());
    argNameAndDependencyIDs = argNameAndDependencyIDs.drop_front();

    // Resolve the name ids.
    DeclName name;
    SmallVector<Identifier, 2> argNames;
    for (auto argNameID : argNameAndDependencyIDs.slice(0, numArgNames))
      argNames.push_back(MF.getIdentifier(argNameID));
    name = DeclName(ctx, baseName, argNames);
    PrettySupplementalDeclNameTrace trace(name);

    argNameAndDependencyIDs = argNameAndDependencyIDs.slice(numArgNames);

    // Check dependency types.
    for (TypeID dependencyID : argNameAndDependencyIDs) {
      auto dependency = MF.getTypeChecked(dependencyID);
      if (!dependency) {
        DeclDeserializationError::Flags errorFlags;
        return llvm::make_error<TypeError>(
            name, takeErrorInfo(dependency.takeError()),
            errorFlags);
      }
    }

    auto parent = MF.getDeclContext(contextID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto *genericParams = MF.maybeReadGenericParams(parent);
    if (declOrOffset.isComplete())
      return declOrOffset;

    const auto resultInterfaceType = MF.getType(resultInterfaceTypeID);
    if (declOrOffset.isComplete())
      return declOrOffset;

    auto *macro = new (ctx) MacroDecl(SourceLoc(), name, SourceLoc(),
                                      genericParams, nullptr,
                                      SourceLoc(),
                                      nullptr,
                                      nullptr,
                                      parent);
    declOrOffset = macro;

    macro->setGenericSignature(MF.getGenericSignature(genericSigID));
    macro->resultType.setType(resultInterfaceType);

    if (hasParameterList)
      macro->parameterList = MF.readParameterList();

    if (auto accessLevel = getActualAccessLevel(rawAccessLevel))
      macro->setAccess(*accessLevel);
    else
      MF.fatal();

    if (isImplicit)
      macro->setImplicit();

    // Record definition
    if (builtinID) {
      llvm::Optional<BuiltinMacroKind> builtinKind;
      switch (builtinID) {
      case 1:
        builtinKind = BuiltinMacroKind::ExternalMacro;
        break;

      default:
        break;
      }

      if (builtinKind) {
        ctx.evaluator.cacheOutput(
            MacroDefinitionRequest{macro},
            MacroDefinition::forBuiltin(*builtinKind)
        );
      }
    } else if (externalModuleNameID > 0) {
      ctx.evaluator.cacheOutput(
          MacroDefinitionRequest{macro},
          MacroDefinition::forExternal(
            MF.getIdentifier(externalModuleNameID),
            MF.getIdentifier(externalMacroTypeNameID)
         )
      );
    } else if (hasExpandedMacroDefinition) {
      // Macro expansion definition block.
      llvm::BitstreamEntry entry =
          MF.fatalIfUnexpected(MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
      if (entry.Kind != llvm::BitstreamEntry::Record)
        return macro;

      SmallVector<uint64_t, 16> scratch;
      scratch.clear();
      StringRef expansionText;
      unsigned recordID = MF.fatalIfUnexpected(
          MF.DeclTypeCursor.readRecord(entry.ID, scratch, &expansionText));
      if (recordID != decls_block::EXPANDED_MACRO_DEFINITION)
        return macro;

      uint8_t hasReplacements;
      decls_block::ExpandedMacroDefinitionLayout::readRecord(
          scratch, hasReplacements);

      // Macro replacements block.
      SmallVector<ExpandedMacroReplacement, 2> replacements;
      if (hasReplacements) {
        llvm::BitstreamEntry entry =
            MF.fatalIfUnexpected(
              MF.DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
        if (entry.Kind == llvm::BitstreamEntry::Record) {
          scratch.clear();
          unsigned recordID = MF.fatalIfUnexpected(
              MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
          if (recordID != decls_block::EXPANDED_MACRO_REPLACEMENTS)
            return macro;

          ArrayRef<uint64_t> serializedReplacements;
          decls_block::ExpandedMacroReplacementsLayout::readRecord(
              scratch, serializedReplacements);
          if (serializedReplacements.size() % 3 == 0) {
            for (unsigned i : range(0, serializedReplacements.size() / 3)) {
              ExpandedMacroReplacement replacement{
                static_cast<unsigned>(serializedReplacements[3*i]),
                static_cast<unsigned>(serializedReplacements[3*i + 1]),
                static_cast<unsigned>(serializedReplacements[3*i + 2])
              };
              replacements.push_back(replacement);
            }
          }
        }
      }

      ctx.evaluator.cacheOutput(
          MacroDefinitionRequest{macro},
          MacroDefinition::forExpanded(ctx, expansionText, replacements)
      );
    }

    return macro;
  }

  AvailableAttr *readAvailable_DECL_ATTR(SmallVectorImpl<uint64_t> &scratch,
                                         StringRef blobData);
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
    if (auto error =
            diagnoseFatalIfNotSuccess(DeclTypeCursor.JumpToBit(declOrOffset)))
      return std::move(error);

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

  LLVM_DEBUG(
    if (auto *VD = dyn_cast_or_null<ValueDecl>(declOrOffset.get())) {
      llvm::dbgs() << "Deserialized: '";
      llvm::dbgs() << VD->getName();
      llvm::dbgs() << "'\n";
    });

  return declOrOffset;
}

AvailableAttr *
DeclDeserializer::readAvailable_DECL_ATTR(SmallVectorImpl<uint64_t> &scratch,
                                          StringRef blobData) {
  bool isImplicit;
  bool isUnavailable;
  bool isDeprecated;
  bool isNoAsync;
  bool isPackageDescriptionVersionSpecific;
  bool isSPI;
  DEF_VER_TUPLE_PIECES(Introduced);
  DEF_VER_TUPLE_PIECES(Deprecated);
  DEF_VER_TUPLE_PIECES(Obsoleted);
  DeclID renameDeclID;
  unsigned platform, messageSize, renameSize;

  // Decode the record, pulling the version tuple information.
  serialization::decls_block::AvailableDeclAttrLayout::readRecord(
      scratch, isImplicit, isUnavailable, isDeprecated, isNoAsync,
      isPackageDescriptionVersionSpecific, isSPI, LIST_VER_TUPLE_PIECES(Introduced),
      LIST_VER_TUPLE_PIECES(Deprecated), LIST_VER_TUPLE_PIECES(Obsoleted),
      platform, renameDeclID, messageSize, renameSize);

  ValueDecl *renameDecl = nullptr;
  if (renameDeclID) {
    renameDecl = cast<ValueDecl>(MF.getDecl(renameDeclID));
  }

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
  else if (isNoAsync)
    platformAgnostic = PlatformAgnosticAvailabilityKind::NoAsync;
  else if (((PlatformKind)platform) == PlatformKind::none &&
           (!Introduced.empty() || !Deprecated.empty() || !Obsoleted.empty()))
    platformAgnostic =
        isPackageDescriptionVersionSpecific
            ? PlatformAgnosticAvailabilityKind::
                  PackageDescriptionVersionSpecific
            : PlatformAgnosticAvailabilityKind::SwiftVersionSpecific;
  else
    platformAgnostic = PlatformAgnosticAvailabilityKind::None;

  auto attr = new (ctx) AvailableAttr(
      SourceLoc(), SourceRange(), (PlatformKind)platform, message, rename,
      renameDecl, Introduced, SourceRange(), Deprecated, SourceRange(),
      Obsoleted, SourceRange(), platformAgnostic, isImplicit, isSPI);
  return attr;
}

llvm::Error DeclDeserializer::deserializeCustomAttrs() {
  using namespace decls_block;

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  for (auto attrOffset : customAttrOffsets) {
    if (auto error =
          MF.diagnoseFatalIfNotSuccess(MF.DeclTypeCursor.JumpToBit(attrOffset)))
      return error;

    llvm::BitstreamEntry entry =
        MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());
    if (entry.Kind != llvm::BitstreamEntry::Record) {
      // We don't know how to serialize decls represented by sub-blocks.
      return MF.diagnoseFatal();
    }

    unsigned recordID = MF.fatalIfUnexpected(
        MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
    assert(recordID == decls_block::Custom_DECL_ATTR &&
        "expecting only custom attributes in deserializeCustomAttrs");

    bool isImplicit;
    bool isArgUnsafe;
    TypeID typeID;
    serialization::decls_block::CustomDeclAttrLayout::readRecord(
      scratch, isImplicit, typeID, isArgUnsafe);

    Expected<Type> deserialized = MF.getTypeChecked(typeID);
    if (!deserialized) {
      if (deserialized.errorIsA<XRefNonLoadedModuleError>() ||
          MF.allowCompilerErrors()) {
        // A custom attribute defined behind an implementation-only import
        // is safe to drop when it can't be deserialized.
        // rdar://problem/56599179. When allowing errors we're doing a best
        // effort to create a module, so ignore in that case as well.
        MF.diagnoseAndConsumeError(deserialized.takeError());
      } else
        return deserialized.takeError();
    } else if (!deserialized.get() && MF.allowCompilerErrors()) {
      // Serialized an invalid attribute, just skip it when allowing errors
    } else {
      auto *TE = TypeExpr::createImplicit(deserialized.get(), ctx);
      auto custom = CustomAttr::create(ctx, SourceLoc(), TE, isImplicit);
      custom->setArgIsUnsafe(isArgUnsafe);
      AddAttribute(custom);
    }
    scratch.clear();
  }

  return llvm::Error::success();
}

llvm::Error DeclDeserializer::deserializeDeclCommon() {
  using namespace decls_block;

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  while (true) {
    BCOffsetRAII restoreOffset(MF.DeclTypeCursor);
    serialization::BitOffset attrOffset = MF.DeclTypeCursor.GetCurrentBitNo();
    llvm::BitstreamEntry entry =
        MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());
    if (entry.Kind != llvm::BitstreamEntry::Record) {
      // We don't know how to serialize decls represented by sub-blocks.
      return MF.diagnoseFatal();
    }

    unsigned recordID = MF.fatalIfUnexpected(
        MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

    if (recordID == ERROR_FLAG) {
      assert(!IsInvalid && "Error flag written multiple times");
      IsInvalid = true;
    } else if (isDeclAttrRecord(recordID)) {
      DeclAttribute *Attr = nullptr;
      bool skipAttr = false;
      switch (recordID) {
      case decls_block::SILGenName_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::SILGenNameDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) SILGenNameAttr(blobData, /*raw*/ false, isImplicit);
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

      case decls_block::NonSendable_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::NonSendableDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) NonSendableAttr((NonSendableKind)kind);
        break;
      }

      case decls_block::Optimize_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::OptimizeDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) OptimizeAttr((OptimizationMode)kind);
        break;
      }

      case decls_block::Exclusivity_DECL_ATTR: {
        unsigned kind;
        serialization::decls_block::ExclusivityDeclAttrLayout::readRecord(
            scratch, kind);
        Attr = new (ctx) ExclusivityAttr((ExclusivityAttr::Mode)kind);
        break;
      }

      case decls_block::Effects_DECL_ATTR: {
        unsigned kind;
        IdentifierID customStringID;
        serialization::decls_block::EffectsDeclAttrLayout::
          readRecord(scratch, kind, customStringID);
        if (customStringID) {
          assert((EffectsKind)kind == EffectsKind::Custom);
          Attr = new (ctx) EffectsAttr(MF.getIdentifier(customStringID).str());
        } else {
          Attr = new (ctx) EffectsAttr((EffectsKind)kind);
        }
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
        Attr = readAvailable_DECL_ATTR(scratch, blobData);
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
          Attr = ObjCAttr::create(ctx, llvm::None, isImplicitName);
        else
          Attr = ObjCAttr::create(ctx, ObjCSelector(ctx, numArgs-1, pieces),
                                  isImplicitName);
        Attr->setImplicit(isImplicit);
        cast<ObjCAttr>(Attr)->setSwift3Inferred(isSwift3Inferred);
        break;
      }

      case decls_block::MainType_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::MainTypeDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) MainTypeAttr(isImplicit);
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
        uint64_t numAvailabilityAttrs;
        uint64_t numTypeErasedParams;
        DeclID targetFunID;

        serialization::decls_block::SpecializeDeclAttrLayout::readRecord(
            scratch, exported, specializationKindVal, specializedSigID,
            targetFunID, numArgs, numSPIGroups, numAvailabilityAttrs,
            numTypeErasedParams, rawPieceIDs);

        assert(rawPieceIDs.size() == numArgs + numSPIGroups + numTypeErasedParams ||
               rawPieceIDs.size() == (numArgs - 1 + numSPIGroups + numTypeErasedParams));
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
              (rawPieceIDs.size() == numArgs + numSPIGroups + numTypeErasedParams) ? numArgs
                                                             : numArgs - 1;
          for (auto id : rawPieceIDs.slice(numTargetFunctionPiecesToSkip))
            spis.push_back(MF.getIdentifier(id));
        }

        SmallVector<Type, 4> typeErasedParams;
        if (numTypeErasedParams) {
          auto numTargetFunctionPiecesToSkip =
              (rawPieceIDs.size() == numArgs + numSPIGroups + numTypeErasedParams) ? numArgs + numSPIGroups
                                                             : numArgs - 1 + numSPIGroups;
          for (auto id : rawPieceIDs.slice(numTargetFunctionPiecesToSkip))
            typeErasedParams.push_back(MF.getType(id));
        }

        SmallVector<AvailableAttr *, 4> availabilityAttrs;
        while (numAvailabilityAttrs) {
          // Prepare to read the next record.
          restoreOffset.cancel();
          scratch.clear();

          // TODO: deserialize them.
          BCOffsetRAII restoreOffset2(MF.DeclTypeCursor);
          llvm::BitstreamEntry entry =
              MF.fatalIfUnexpected(MF.DeclTypeCursor.advance());
          if (entry.Kind != llvm::BitstreamEntry::Record) {
            // We don't know how to serialize decls represented by sub-blocks.
            return MF.diagnoseFatal();
          }
          unsigned recordID = MF.fatalIfUnexpected(
              MF.DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));
          if (recordID != decls_block::Available_DECL_ATTR) {
            return MF.diagnoseFatal();
          }

          auto attr = readAvailable_DECL_ATTR(scratch, blobData);
          availabilityAttrs.push_back(attr);
          restoreOffset2.cancel();
          --numAvailabilityAttrs;
        }

        auto specializedSig = MF.getGenericSignature(specializedSigID);
        Attr = SpecializeAttr::create(ctx, exported != 0, specializationKind,
                                      spis, availabilityAttrs, typeErasedParams,
                                      specializedSig, replacedFunctionName, &MF,
                                      targetFunID);
        break;
      }

      case decls_block::Initializes_DECL_ATTR: {
        ArrayRef<uint64_t> rawPropertyIDs;
        serialization::decls_block::InitializesDeclAttrLayout::
            readRecord(scratch, rawPropertyIDs);

        SmallVector<Identifier, 4> properties;
        for (auto rawID : rawPropertyIDs) {
          properties.push_back(MF.getIdentifier(rawID));
        }

        Attr = InitializesAttr::create(ctx, SourceLoc(), SourceRange(),
                                       properties);
        break;
      }

      case decls_block::Accesses_DECL_ATTR: {
        ArrayRef<uint64_t> rawPropertyIDs;
        serialization::decls_block::AccessesDeclAttrLayout::
            readRecord(scratch, rawPropertyIDs);

        SmallVector<Identifier, 4> properties;
        for (auto rawID : rawPropertyIDs) {
          properties.push_back(MF.getIdentifier(rawID));
        }

        Attr = AccessesAttr::create(ctx, SourceLoc(), SourceRange(),
                                    properties);
        break;
      }

      case decls_block::DynamicReplacement_DECL_ATTR: {
        bool isImplicit;
        uint64_t numArgs;
        ArrayRef<uint64_t> rawPieceIDs;
        DeclID replacedFunID;
        serialization::decls_block::DynamicReplacementDeclAttrLayout::
            readRecord(scratch, isImplicit, replacedFunID, numArgs, rawPieceIDs);

        auto baseName = MF.getDeclBaseName(rawPieceIDs[0]);
        SmallVector<Identifier, 4> pieces;
        for (auto pieceID : rawPieceIDs.slice(1))
          pieces.push_back(MF.getIdentifier(pieceID));

        assert(numArgs != 0);
        assert(!isImplicit && "Need to update for implicit");
        Attr = DynamicReplacementAttr::create(
            ctx, DeclNameRef({ ctx, baseName, pieces }), &MF, replacedFunID);
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
        // Deserialize the custom attributes after the attached decl,
        // skip for now.
        customAttrOffsets.push_back(attrOffset);
        skipAttr = true;
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
        uint64_t rawDiffKind;
        GenericSignatureID derivativeGenSigId;
        ArrayRef<uint64_t> parameters;

        serialization::decls_block::DifferentiableDeclAttrLayout::readRecord(
            scratch, isImplicit, rawDiffKind, derivativeGenSigId,
            parameters);

        auto diffKind = getActualDifferentiabilityKind(rawDiffKind);
        if (!diffKind)
          return MF.diagnoseFatal();
        auto derivativeGenSig = MF.getGenericSignature(derivativeGenSigId);
        llvm::SmallBitVector parametersBitVector(parameters.size());
        for (unsigned i : indices(parameters))
          parametersBitVector[i] = parameters[i];
        auto *indices = IndexSubset::get(ctx, parametersBitVector);
        auto *diffAttr = DifferentiableAttr::create(
            ctx, isImplicit, SourceLoc(), SourceRange(), *diffKind,
            /*parsedParameters*/ {}, /*trailingWhereClause*/ nullptr);

        // Cache parameter indices so that they can set later.
        // `DifferentiableAttr::setParameterIndices` cannot be called here
        // because it requires `DifferentiableAttr::setOriginalDeclaration` to
        // be called first. `DifferentiableAttr::setOriginalDeclaration` cannot
        // be called here because the original declaration is not accessible in
        // this function (`DeclDeserializer::deserializeDeclCommon`).
        diffAttrParamIndicesMap[diffAttr] = indices;
        diffAttr->setDerivativeGenericSignature(derivativeGenSig);
        Attr = diffAttr;
        break;
      }

      case decls_block::Derivative_DECL_ATTR: {
        bool isImplicit;
        uint64_t origNameId;
        bool hasAccessorKind;
        uint64_t rawAccessorKind;
        DeclID origDeclId;
        uint64_t rawDerivativeKind;
        ArrayRef<uint64_t> parameters;

        serialization::decls_block::DerivativeDeclAttrLayout::readRecord(
            scratch, isImplicit, origNameId, hasAccessorKind, rawAccessorKind,
            origDeclId, rawDerivativeKind, parameters);

        llvm::Optional<AccessorKind> accessorKind = llvm::None;
        if (hasAccessorKind) {
          auto maybeAccessorKind = getActualAccessorKind(rawAccessorKind);
          if (!maybeAccessorKind)
            return MF.diagnoseFatal();
          accessorKind = *maybeAccessorKind;
        }

        DeclNameRefWithLoc origName{DeclNameRef(MF.getDeclBaseName(origNameId)),
                                    DeclNameLoc(), accessorKind};
        auto derivativeKind =
            getActualAutoDiffDerivativeFunctionKind(rawDerivativeKind);
        if (!derivativeKind)
          return MF.diagnoseFatal();
        llvm::SmallBitVector parametersBitVector(parameters.size());
        for (unsigned i : indices(parameters))
          parametersBitVector[i] = parameters[i];
        auto *indices = IndexSubset::get(ctx, parametersBitVector);

        auto *derivativeAttr =
            DerivativeAttr::create(ctx, isImplicit, SourceLoc(), SourceRange(),
                                   /*baseType*/ nullptr, origName, indices);
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

        DeclNameRefWithLoc origName{DeclNameRef(MF.getDeclBaseName(origNameId)),
                                    DeclNameLoc(), llvm::None};
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

      case decls_block::UnavailableFromAsync_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::UnavailableFromAsyncDeclAttrLayout::
            readRecord(scratch, isImplicit);
        Attr = new (ctx) UnavailableFromAsyncAttr(blobData, isImplicit);
        break;
      }

      case decls_block::BackDeployed_DECL_ATTR: {
        bool isImplicit;
        unsigned Platform;
        DEF_VER_TUPLE_PIECES(Version);
        serialization::decls_block::BackDeployedDeclAttrLayout::readRecord(
            scratch, isImplicit, LIST_VER_TUPLE_PIECES(Version), Platform);
        llvm::VersionTuple Version;
        DECODE_VER_TUPLE(Version)
        Attr = new (ctx)
            BackDeployedAttr(SourceLoc(), SourceRange(), (PlatformKind)Platform,
                             Version, isImplicit);
        break;
      }

      case decls_block::Expose_DECL_ATTR: {
        bool isImplicit;
        serialization::decls_block::ExposeDeclAttrLayout::readRecord(
            scratch, isImplicit);
        Attr = new (ctx) ExposeAttr(blobData, isImplicit);
        break;
      }

      case decls_block::Documentation_DECL_ATTR: {
        bool isImplicit;
        uint64_t CategoryID;
        bool hasVisibility;
        uint8_t visibilityID;
        serialization::decls_block::DocumentationDeclAttrLayout::readRecord(
            scratch, isImplicit, CategoryID, hasVisibility, visibilityID);
        StringRef CategoryText = MF.getIdentifierText(CategoryID);
        llvm::Optional<swift::AccessLevel> realVisibility = llvm::None;
        if (hasVisibility)
          realVisibility = getActualAccessLevel(visibilityID);
        Attr = new (ctx) DocumentationAttr(CategoryText, realVisibility, isImplicit);
        break;
      }

      case decls_block::ObjCImplementation_DECL_ATTR: {
        bool isImplicit;
        bool isCategoryNameInvalid;
        uint64_t categoryNameID;
        serialization::decls_block::ObjCImplementationDeclAttrLayout::
            readRecord(scratch, isImplicit, isCategoryNameInvalid,
                       categoryNameID);
        Identifier categoryName = MF.getIdentifier(categoryNameID);
        Attr = new (ctx) ObjCImplementationAttr(categoryName, SourceLoc(),
                                                SourceRange(), isImplicit,
                                                isCategoryNameInvalid);
        break;
      }

      case decls_block::MacroRole_DECL_ATTR: {
        bool isImplicit;
        uint8_t rawMacroSyntax;
        uint8_t rawMacroRole;
        uint64_t numNames;
        uint64_t numConformances;
        ArrayRef<uint64_t> introducedDeclNames;
        serialization::decls_block::MacroRoleDeclAttrLayout::
            readRecord(scratch, isImplicit, rawMacroSyntax, rawMacroRole,
                       numNames, numConformances, introducedDeclNames);
        auto role = *getActualMacroRole(rawMacroRole);
        SmallVector<MacroIntroducedDeclName, 1> names;
        unsigned nameIdx = 0;
        while (nameIdx < numNames) {
          auto kind = getActualMacroIntroducedDeclNameKind(
              (uint8_t)introducedDeclNames[nameIdx++]);
          auto baseName =
              MF.getDeclBaseName(IdentifierID(introducedDeclNames[nameIdx++]));

          unsigned numArgs = introducedDeclNames[nameIdx++];
          if (numArgs == 0) {
            names.push_back(MacroIntroducedDeclName(*kind, baseName));
            continue;
          }

          SmallVector<Identifier, 2> argLabels;
          for (unsigned i : range(0, numArgs - 1)) {
            (void)i;
            argLabels.push_back(
                MF.getDeclBaseName(
                    IdentifierID(introducedDeclNames[nameIdx++]))
                  .getIdentifier());
          }

          DeclName name(ctx, baseName, argLabels);
          names.push_back(MacroIntroducedDeclName(*kind, name));
        }

        introducedDeclNames = introducedDeclNames.slice(numNames);
        SmallVector<TypeExpr *, 1> conformances;
        for (TypeID conformanceID : introducedDeclNames) {
          auto conformance = MF.getTypeChecked(conformanceID);
          if (!conformance) {
            return conformance.takeError();
          }

          conformances.push_back(
              TypeExpr::createImplicit(conformance.get(), ctx));
        }

        Attr = MacroRoleAttr::create(
            ctx, SourceLoc(), SourceRange(),
            static_cast<MacroSyntax>(rawMacroSyntax), SourceLoc(), role, names,
            conformances, SourceLoc(), isImplicit);
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
        MF.fatal(llvm::make_error<InvalidRecordKindError>(recordID));
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
    } else if (recordID == decls_block::DESERIALIZATION_SAFETY) {
      IdentifierID declID;
      decls_block::DeserializationSafetyLayout::readRecord(scratch, declID);

      if (MF.getResilienceStrategy() == ResilienceStrategy::Resilient &&
          MF.getContext().LangOpts.EnableDeserializationSafety) {
        auto name = MF.getIdentifier(declID);
        LLVM_DEBUG(llvm::dbgs() << "Skipping unsafe deserialization: '"
                                << name << "'\n");
        return llvm::make_error<UnsafeDeserializationError>(name);
      }
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

  auto commonError = deserializeDeclCommon();
  if (commonError)
    return std::move(commonError);

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
    return MF.diagnoseFatal();
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
    auto declOrError = deserialize##RECORD_NAME(scratch, blobData); \
    if (declOrError) { \
      /* \
      // Set original declaration and parameter indices in `@differentiable` \
      // attributes. \
      */ \
      setOriginalDeclarationAndParameterIndicesInDifferentiableAttributes(\
          declOrError.get(), DAttrs, diffAttrParamIndicesMap); \
    } \
    if (!declOrError) \
      return declOrError; \
    declOrOffset = declOrError.get(); \
    break; \
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
  CASE(Macro)
#undef CASE

  case decls_block::XREF: {
    assert(DAttrs == nullptr);
    ModuleID baseModuleID;
    uint32_t pathLen;
    decls_block::XRefLayout::readRecord(scratch, baseModuleID, pathLen);
    auto resolved = MF.resolveCrossReference(baseModuleID, pathLen);
    if (!resolved)
      return resolved;
    declOrOffset = resolved.get();
    break;
  }
  
  default:
    // We don't know how to deserialize this kind of decl.
    MF.fatal(llvm::make_error<InvalidRecordKindError>(recordID));
  }

  auto attrError = deserializeCustomAttrs();
  if (attrError)
    return std::move(attrError);
  return declOrOffset;
}

/// Translate from the Serialization function type repr enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static llvm::Optional<swift::FunctionType::Representation>
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
    return llvm::None;
  }
}

/// Translate from the Serialization function type repr enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static llvm::Optional<swift::SILFunctionType::Representation>
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
  CASE(CXXMethod)
#undef CASE
  default:
    return llvm::None;
  }
}

/// Translate from the Serialization coroutine kind enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static llvm::Optional<swift::SILCoroutineKind>
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
    return llvm::None;
  }
}

/// Translate from the serialization ReferenceOwnership enumerators, which are
/// guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::ReferenceOwnership>
getActualReferenceOwnership(serialization::ReferenceOwnership raw) {
  switch (raw) {
  case serialization::ReferenceOwnership::Strong:
    return swift::ReferenceOwnership::Strong;
#define REF_STORAGE(Name, ...) \
  case serialization::ReferenceOwnership::Name: \
    return swift::ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
  }
  return llvm::None;
}

/// Translate from the serialization ParameterConvention enumerators,
/// which are guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::ParameterConvention>
getActualParameterConvention(uint8_t raw) {
  switch (serialization::ParameterConvention(raw)) {
#define CASE(ID) \
  case serialization::ParameterConvention::ID: \
    return swift::ParameterConvention::ID;
  CASE(Indirect_In)
  CASE(Indirect_Inout)
  CASE(Indirect_InoutAliasable)
  CASE(Indirect_In_Guaranteed)
  case serialization::ParameterConvention::Indirect_In_Constant: \
    return swift::ParameterConvention::Indirect_In;
  CASE(Direct_Owned)
  CASE(Direct_Unowned)
  CASE(Direct_Guaranteed)
  CASE(Pack_Inout)
  CASE(Pack_Guaranteed)
  CASE(Pack_Owned)
#undef CASE
  }
  return llvm::None;
}

/// Translate from the serialization SILParameterDifferentiability enumerators,
/// which are guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::SILParameterDifferentiability>
getActualSILParameterDifferentiability(uint8_t raw) {
  switch (serialization::SILParameterDifferentiability(raw)) {
#define CASE(ID)                                                               \
  case serialization::SILParameterDifferentiability::ID:                       \
    return swift::SILParameterDifferentiability::ID;
  CASE(DifferentiableOrNotApplicable)
  CASE(NotDifferentiable)
#undef CASE
  }
  return llvm::None;
}

/// Translate from the serialization ResultConvention enumerators,
/// which are guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::ResultConvention>
getActualResultConvention(uint8_t raw) {
  switch (serialization::ResultConvention(raw)) {
#define CASE(ID) \
  case serialization::ResultConvention::ID: return swift::ResultConvention::ID;
  CASE(Indirect)
  CASE(Owned)
  CASE(Unowned)
  CASE(UnownedInnerPointer)
  CASE(Autoreleased)
  CASE(Pack)
#undef CASE
  }
  return llvm::None;
}

/// Translate from the serialization SILResultDifferentiability enumerators,
/// which are guaranteed to be stable, to the AST ones.
static llvm::Optional<swift::SILResultDifferentiability>
getActualSILResultDifferentiability(uint8_t raw) {
  switch (serialization::SILResultDifferentiability(raw)) {
#define CASE(ID)                                                               \
  case serialization::SILResultDifferentiability::ID:                          \
    return swift::SILResultDifferentiability::ID;
    CASE(DifferentiableOrNotApplicable)
    CASE(NotDifferentiable)
#undef CASE
  }
  return llvm::None;
}

Type ModuleFile::getType(TypeID TID) {
  Expected<Type> deserialized = getTypeChecked(TID);
  if (!deserialized) {
    fatal(deserialized.takeError());
  }
  return deserialized.get();
}

namespace swift {
namespace serialization {
namespace decls_block {

#define DESERIALIZE_TYPE(TYPE_ID)                                              \
  detail::TypeRecordDispatch<detail::TypeRecords::TYPE_ID>::deserialize

Expected<Type>
DESERIALIZE_TYPE(BUILTIN_ALIAS_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  DeclID underlyingID;
  TypeID canonicalTypeID;
  decls_block::BuiltinAliasTypeLayout::readRecord(scratch, underlyingID,
                                                  canonicalTypeID);
  auto aliasOrError = MF.getDeclChecked(underlyingID);
  if (!aliasOrError)
    return aliasOrError.takeError();
  auto alias = dyn_cast<TypeAliasDecl>(aliasOrError.get());

  if (MF.getContext().LangOpts.EnableDeserializationRecovery) {
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
  if (alias->getAttrs().isUnavailable(MF.getContext()) &&
      alias->isCompatibilityAlias()) {
    return alias->getUnderlyingType();
  }

  return alias->getDeclaredInterfaceType();
}

Expected<Type> DESERIALIZE_TYPE(NAME_ALIAS_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  DeclID typealiasID;
  TypeID parentTypeID;
  TypeID underlyingTypeID;
  TypeID substitutedTypeID;
  SubstitutionMapID substitutionsID;
  decls_block::TypeAliasTypeLayout::readRecord(
      scratch, typealiasID, parentTypeID, underlyingTypeID, substitutedTypeID,
      substitutionsID);

  TypeAliasDecl *alias = nullptr;
  Type underlyingType;
  if (MF.getContext().LangOpts.EnableDeserializationRecovery) {
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
      // Pass through deserialization errors.
        if (aliasOrError.errorIsA<FatalDeserializationError>())
          return aliasOrError.takeError();

      // We're going to recover by falling back to the underlying type, so
      // just ignore the error.
      MF.diagnoseAndConsumeError(aliasOrError.takeError());
    }

    if (!alias || !alias->getDeclaredInterfaceType()->isEqual(underlyingType)) {
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
  if (alias && alias->getAttrs().isUnavailable(MF.getContext()) &&
      alias->isCompatibilityAlias()) {
    return alias->getUnderlyingType().subst(subMapOrError.get());
  }

  auto parentType = parentTypeOrError.get();
  return TypeAliasType::get(alias, parentType, subMapOrError.get(),
                            substitutedType);
}

Expected<Type> DESERIALIZE_TYPE(NOMINAL_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
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
    const DeclName fullName = cast<ValueDecl>(nominalOrError.get())->getName();
    tinyTrace.addValue(fullName.getBaseIdentifier());
    return llvm::make_error<XRefError>("declaration is not a nominal type",
                                       tinyTrace, fullName);
  }
  return NominalType::get(nominal, parentTy.get(), MF.getContext());
}

Expected<Type> DESERIALIZE_TYPE(PAREN_TYPE)(ModuleFile &MF,
                                            SmallVectorImpl<uint64_t> &scratch,
                                            StringRef blobData) {
  TypeID underlyingID;
  decls_block::ParenTypeLayout::readRecord(scratch, underlyingID);

  auto underlyingTy = MF.getTypeChecked(underlyingID);
  if (!underlyingTy)
    return underlyingTy.takeError();

  return ParenType::get(MF.getContext(), underlyingTy.get());
}

Expected<Type> DESERIALIZE_TYPE(TUPLE_TYPE)(ModuleFile &MF,
                                            SmallVectorImpl<uint64_t> &scratch,
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

  return TupleType::get(elements, MF.getContext());
}

Expected<Type>
detail::function_deserializer::deserialize(ModuleFile &MF,
                                           SmallVectorImpl<uint64_t> &scratch,
                                           StringRef blobData, bool isGeneric) {
  TypeID resultID;
  uint8_t rawRepresentation, rawDiffKind;
  bool noescape = false, concurrent, async, throws;
  GenericSignature genericSig;
  TypeID clangTypeID;
  TypeID globalActorTypeID;

  if (!isGeneric) {
    decls_block::FunctionTypeLayout::readRecord(
        scratch, resultID, rawRepresentation, clangTypeID, noescape, concurrent,
        async, throws, rawDiffKind, globalActorTypeID);
  } else {
    GenericSignatureID rawGenericSig;
    decls_block::GenericFunctionTypeLayout::readRecord(
        scratch, resultID, rawRepresentation, concurrent, async, throws,
        rawDiffKind, globalActorTypeID, rawGenericSig);
    genericSig = MF.getGenericSignature(rawGenericSig);
    clangTypeID = 0;
  }

  auto representation = getActualFunctionTypeRepresentation(rawRepresentation);
  if (!representation.has_value())
    return MF.diagnoseFatal();

  auto diffKind = getActualDifferentiabilityKind(rawDiffKind);
  if (!diffKind.has_value())
    return MF.diagnoseFatal();

  const clang::Type *clangFunctionType = nullptr;
  if (clangTypeID) {
    auto loadedClangType = MF.getClangType(clangTypeID);
    if (!loadedClangType)
      return loadedClangType.takeError();
    clangFunctionType = loadedClangType.get();
  }

  Type globalActor;
  if (globalActorTypeID) {
    auto globalActorTy = MF.getTypeChecked(globalActorTypeID);
    if (!globalActorTy)
      return globalActorTy.takeError();

    globalActor = globalActorTy.get();
  }

  auto info =
      FunctionType::ExtInfoBuilder(*representation, noescape, throws, *diffKind,
                                   clangFunctionType, globalActor)
          .withConcurrent(concurrent)
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
    IdentifierID internalLabelID;
    TypeID typeID;
    bool isVariadic, isAutoClosure, isNonEphemeral, isIsolated,
        isCompileTimeConst;
    bool isNoDerivative;
    unsigned rawOwnership;
    decls_block::FunctionParamLayout::readRecord(
        scratch, labelID, internalLabelID, typeID, isVariadic, isAutoClosure,
        isNonEphemeral, rawOwnership, isIsolated, isNoDerivative,
        isCompileTimeConst);

    auto ownership = getActualParamDeclSpecifier(
      (serialization::ParamDeclSpecifier)rawOwnership);
    if (!ownership)
      return MF.diagnoseFatal();

    auto paramTy = MF.getTypeChecked(typeID);
    if (!paramTy)
      return paramTy.takeError();

    params.emplace_back(paramTy.get(), MF.getIdentifier(labelID),
                        ParameterTypeFlags(isVariadic, isAutoClosure,
                                           isNonEphemeral, *ownership,
                                           isIsolated, isNoDerivative,
                                           isCompileTimeConst),
                        MF.getIdentifier(internalLabelID));
  }

  if (!isGeneric) {
    assert(genericSig.isNull());
    return FunctionType::get(params, resultTy.get(), info);
  }

  assert(!genericSig.isNull());
  return GenericFunctionType::get(genericSig, params, resultTy.get(), info);
}

Expected<Type> DESERIALIZE_TYPE(FUNCTION_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  return detail::function_deserializer::deserialize(MF, scratch, blobData, /*isGeneric*/ false);
}

Expected<Type> DESERIALIZE_TYPE(GENERIC_FUNCTION_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  return detail::function_deserializer::deserialize(MF, scratch, blobData, /*isGeneric*/ true);
}

template <typename Layout, typename ASTType, bool CanBeThin>
Expected<Type> deserializeAnyMetatypeType(ModuleFile &MF,
                                          ArrayRef<uint64_t> scratch,
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
      return MF.diagnoseFatal();
    return ASTType::get(instanceType.get(),
                        swift::MetatypeRepresentation::Thin);

  case serialization::MetatypeRepresentation::MR_Thick:
    return ASTType::get(instanceType.get(),
                        swift::MetatypeRepresentation::Thick);

  case serialization::MetatypeRepresentation::MR_ObjC:
    return ASTType::get(instanceType.get(),
                        swift::MetatypeRepresentation::ObjC);

  default:
    return MF.diagnoseFatal();
  }
}

Expected<Type> DESERIALIZE_TYPE(EXISTENTIAL_METATYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  return deserializeAnyMetatypeType<decls_block::ExistentialMetatypeTypeLayout,
                                    ExistentialMetatypeType,
                                    /*CanBeThin*/ false>(MF, scratch, blobData);
}

Expected<Type> DESERIALIZE_TYPE(METATYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  return deserializeAnyMetatypeType<decls_block::MetatypeTypeLayout,
                                    MetatypeType, /*CanBeThin*/ true>(
      MF, scratch, blobData);
}

Expected<Type> DESERIALIZE_TYPE(DYNAMIC_SELF_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID selfID;
  decls_block::DynamicSelfTypeLayout::readRecord(scratch, selfID);
  return DynamicSelfType::get(MF.getType(selfID), MF.getContext());
}

Expected<Type> DESERIALIZE_TYPE(REFERENCE_STORAGE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  uint8_t rawOwnership;
  TypeID objectTypeID;
  decls_block::ReferenceStorageTypeLayout::readRecord(scratch, rawOwnership,
                                                      objectTypeID);

  auto ownership = getActualReferenceOwnership(
      (serialization::ReferenceOwnership)rawOwnership);
  if (!ownership.has_value())
    return MF.diagnoseFatal();

  auto objectTy = MF.getTypeChecked(objectTypeID);
  if (!objectTy)
    return objectTy.takeError();

  return ReferenceStorageType::get(objectTy.get(), ownership.value(),
                                   MF.getContext());
}

Expected<Type> DESERIALIZE_TYPE(PRIMARY_ARCHETYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  GenericSignatureID sigID;
  TypeID interfaceTypeID;

  decls_block::PrimaryArchetypeTypeLayout::readRecord(scratch, sigID,
                                                      interfaceTypeID);

  auto sigOrError = MF.getGenericSignatureChecked(sigID);
  if (!sigOrError)
    return sigOrError.takeError();

  auto interfaceTypeOrError = MF.getTypeChecked(interfaceTypeID);
  if (!interfaceTypeOrError)
    return interfaceTypeOrError.takeError();

  Type contextType =
      sigOrError.get().getGenericEnvironment()->mapTypeIntoContext(
          interfaceTypeOrError.get());

  if (contextType->hasError())
    return MF.diagnoseFatal();

  return contextType;
}

Expected<Type> DESERIALIZE_TYPE(OPENED_ARCHETYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID interfaceID;
  GenericEnvironmentID genericEnvID;

  decls_block::OpenedArchetypeTypeLayout::readRecord(scratch,
                                                     interfaceID,
                                                     genericEnvID);

  auto interfaceTypeOrError = MF.getTypeChecked(interfaceID);
  if (!interfaceTypeOrError)
    return interfaceTypeOrError.takeError();

  auto envOrError = MF.getGenericEnvironmentChecked(genericEnvID);
  if (!envOrError)
    return envOrError.takeError();

  return envOrError.get()->mapTypeIntoContext(interfaceTypeOrError.get());
}

Expected<Type> DESERIALIZE_TYPE(OPAQUE_ARCHETYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  DeclID opaqueDeclID;
  TypeID interfaceTypeID;
  SubstitutionMapID subsID;
  decls_block::OpaqueArchetypeTypeLayout::readRecord(scratch, opaqueDeclID,
                                                     interfaceTypeID, subsID);

  auto opaqueTypeOrError = MF.getDeclChecked(opaqueDeclID);
  if (!opaqueTypeOrError)
    return opaqueTypeOrError.takeError();

  auto interfaceTypeOrError = MF.getTypeChecked(interfaceTypeID);
  if (!interfaceTypeOrError)
    return interfaceTypeOrError.takeError();

  auto opaqueDecl = cast<OpaqueTypeDecl>(opaqueTypeOrError.get());
  auto subsOrError = MF.getSubstitutionMapChecked(subsID);
  if (!subsOrError)
    return subsOrError.takeError();

  return OpaqueTypeArchetypeType::get(opaqueDecl, interfaceTypeOrError.get(),
                                      subsOrError.get());
}

Expected<Type> DESERIALIZE_TYPE(PACK_ARCHETYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  GenericSignatureID sigID;
  TypeID interfaceTypeID;

  decls_block::PackArchetypeTypeLayout::readRecord(scratch, sigID,
                                                   interfaceTypeID);

  auto sig = MF.getGenericSignature(sigID);
  if (!sig)
    return MF.diagnoseFatal();

  Type interfaceType = MF.getType(interfaceTypeID);
  Type contextType =
      sig.getGenericEnvironment()->mapTypeIntoContext(interfaceType);

  if (contextType->hasError())
    return MF.diagnoseFatal();

  return contextType;
}

Expected<Type> DESERIALIZE_TYPE(ELEMENT_ARCHETYPE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID interfaceID;
  GenericEnvironmentID genericEnvID;

  decls_block::ElementArchetypeTypeLayout::readRecord(scratch,
                                                      interfaceID,
                                                      genericEnvID);

  auto interfaceTypeOrError = MF.getTypeChecked(interfaceID);
  if (!interfaceTypeOrError)
    return interfaceTypeOrError.takeError();

  auto envOrError = MF.getGenericEnvironmentChecked(genericEnvID);
  if (!envOrError)
    return envOrError.takeError();

  return envOrError.get()->mapTypeIntoContext(interfaceTypeOrError.get());
}

Expected<Type>
DESERIALIZE_TYPE(GENERIC_TYPE_PARAM_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  bool parameterPack;
  DeclID declIDOrDepth;
  unsigned indexPlusOne;

  decls_block::GenericTypeParamTypeLayout::readRecord(
      scratch, parameterPack, declIDOrDepth, indexPlusOne);

  if (indexPlusOne == 0) {
    auto genericParamOrError = MF.getDeclChecked(declIDOrDepth);
    if (!genericParamOrError)
      return genericParamOrError.takeError();

    auto genericParam =
        dyn_cast_or_null<GenericTypeParamDecl>(genericParamOrError.get());
    if (!genericParam)
      return MF.diagnoseFatal();

    return genericParam->getDeclaredInterfaceType();
  }

  return GenericTypeParamType::get(parameterPack, declIDOrDepth,
                                   indexPlusOne - 1, MF.getContext());
}

Expected<Type> DESERIALIZE_TYPE(PROTOCOL_COMPOSITION_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  bool hasExplicitAnyObject;
  ArrayRef<uint64_t> rawProtocolIDs;

  decls_block::ProtocolCompositionTypeLayout::readRecord(
      scratch, hasExplicitAnyObject, rawProtocolIDs);
  SmallVector<Type, 4> protocols;
  for (TypeID protoID : rawProtocolIDs) {
    auto protoTy = MF.getTypeChecked(protoID);
    if (!protoTy)
      return protoTy.takeError();
    protocols.push_back(protoTy.get());
  }

  return ProtocolCompositionType::get(MF.getContext(), protocols,
                                      hasExplicitAnyObject);
}

Expected<Type> DESERIALIZE_TYPE(PARAMETERIZED_PROTOCOL_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {

  uint64_t baseTyID;
  ArrayRef<uint64_t> rawArgIDs;

  decls_block::ParameterizedProtocolTypeLayout::readRecord(scratch, baseTyID,
                                                           rawArgIDs);

  auto baseTy = MF.getTypeChecked(baseTyID);
  if (!baseTy)
    return baseTy.takeError();

  SmallVector<Type, 4> args;
  for (TypeID argID : rawArgIDs) {
    auto argTy = MF.getTypeChecked(argID);
    if (!argTy)
      return argTy.takeError();
    args.push_back(argTy.get());
  }

  return ParameterizedProtocolType::get(
      MF.getContext(), (*baseTy)->castTo<ProtocolType>(), args);
}

Expected<Type> DESERIALIZE_TYPE(EXISTENTIAL_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID constraintID;
  decls_block::ExistentialTypeLayout::readRecord(scratch, constraintID);

  auto constraintType = MF.getTypeChecked(constraintID);
  if (!constraintType)
    return constraintType.takeError();

  return ExistentialType::get(constraintType.get());
}

Expected<Type> DESERIALIZE_TYPE(DEPENDENT_MEMBER_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID baseID;
  DeclID assocTypeID;

  decls_block::DependentMemberTypeLayout::readRecord(scratch, baseID,
                                                     assocTypeID);
  auto assocType = MF.getDeclChecked(assocTypeID);
  if (!assocType)
    return assocType.takeError();

  return DependentMemberType::get(MF.getType(baseID),
                                  cast<AssociatedTypeDecl>(assocType.get()));
}

Expected<Type> DESERIALIZE_TYPE(BOUND_GENERIC_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
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

  if (auto clangDecl = nominal->getClangDecl()) {
    if (auto ctd = dyn_cast<clang::ClassTemplateDecl>(clangDecl)) {
      auto clangImporter = static_cast<ClangImporter *>(
          nominal->getASTContext().getClangModuleLoader());

      SmallVector<Type, 2> typesOfGenericArgs;
      for (auto arg : genericArgs) {
        typesOfGenericArgs.push_back(arg);
      }

      SmallVector<clang::TemplateArgument, 2> templateArguments;
      std::unique_ptr<TemplateInstantiationError> error =
          MF.getContext().getClangTemplateArguments(
              ctd->getTemplateParameters(), typesOfGenericArgs,
              templateArguments);

      auto instantiation = clangImporter->instantiateCXXClassTemplate(
          const_cast<clang::ClassTemplateDecl *>(ctd), templateArguments);

      instantiation->setTemplateInstantiationType(
          BoundGenericType::get(nominal, parentTy, genericArgs));
      return instantiation->getDeclaredInterfaceType();
    }
  }

  return BoundGenericType::get(nominal, parentTy, genericArgs);
}

Expected<Type> DESERIALIZE_TYPE(SIL_BLOCK_STORAGE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID captureID;
  decls_block::SILBlockStorageTypeLayout::readRecord(scratch, captureID);
  return SILBlockStorageType::get(MF.getType(captureID)->getCanonicalType());
}

Expected<Type> DESERIALIZE_TYPE(SIL_MOVE_ONLY_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID innerType;
  decls_block::SILMoveOnlyWrappedTypeLayout::readRecord(scratch, innerType);
  return SILMoveOnlyWrappedType::get(MF.getType(innerType)->getCanonicalType());
}

Expected<Type> DESERIALIZE_TYPE(SIL_BOX_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  SILLayoutID layoutID;
  SubstitutionMapID subMapID;
  decls_block::SILBoxTypeLayout::readRecord(scratch, layoutID, subMapID);

  // Get the layout.
  auto getLayout = [&MF](SILLayoutID layoutID) -> Expected<SILLayout *> {
    assert(layoutID > 0 && layoutID <= MF.SILLayouts.size() &&
           "invalid layout ID");

    auto &layoutOrOffset = MF.SILLayouts[layoutID - 1];
    if (layoutOrOffset.isComplete()) {
      return layoutOrOffset;
    }

    BCOffsetRAII saveOffset(MF.DeclTypeCursor);
    if (auto error = MF.diagnoseFatalIfNotSuccess(
            MF.DeclTypeCursor.JumpToBit(layoutOrOffset)))
      return std::move(error);
    auto layout = MF.readSILLayout(MF.DeclTypeCursor);
    if (!layout)
      return MF.diagnoseFatal();
    layoutOrOffset = layout;
    return layout;
  };

  auto layout = getLayout(layoutID);
  if (!layout)
    return layout.takeError();
  if (!*layout)
    return nullptr;

  auto subMapOrError = MF.getSubstitutionMapChecked(subMapID);
  if (!subMapOrError)
    return subMapOrError.takeError();

  return SILBoxType::get(MF.getContext(), *layout, subMapOrError.get());
}

Expected<Type> DESERIALIZE_TYPE(SIL_FUNCTION_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  bool async;
  uint8_t rawCoroutineKind;
  uint8_t rawCalleeConvention;
  uint8_t rawRepresentation;
  uint8_t rawDiffKind;
  bool pseudogeneric = false;
  bool concurrent;
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

  decls_block::SILFunctionTypeLayout::readRecord(
      scratch, concurrent, async, rawCoroutineKind, rawCalleeConvention,
      rawRepresentation, pseudogeneric, noescape, rawDiffKind, hasErrorResult,
      numParams, numYields, numResults, rawInvocationGenericSig,
      rawInvocationSubs, rawPatternSubs, clangFunctionTypeID, variableData);

  // Process the ExtInfo.
  auto representation =
      getActualSILFunctionTypeRepresentation(rawRepresentation);
  if (!representation.has_value())
    return MF.diagnoseFatal();

  auto diffKind = getActualDifferentiabilityKind(rawDiffKind);
  if (!diffKind.has_value())
    return MF.diagnoseFatal();

  const clang::Type *clangFunctionType = nullptr;
  if (clangFunctionTypeID) {
    auto clangType = MF.getClangType(clangFunctionTypeID);
    if (!clangType)
      return clangType.takeError();
    clangFunctionType = clangType.get();
  }

  auto extInfo = SILFunctionType::ExtInfoBuilder(*representation, pseudogeneric,
                                                 noescape, concurrent, async,
                                                 *diffKind, clangFunctionType)
                     .build();

  // Process the coroutine kind.
  auto coroutineKind = getActualSILCoroutineKind(rawCoroutineKind);
  if (!coroutineKind.has_value())
    return MF.diagnoseFatal();

  // Process the callee convention.
  auto calleeConvention = getActualParameterConvention(rawCalleeConvention);
  if (!calleeConvention.has_value())
    return MF.diagnoseFatal();

  auto processParameter =
      [&](TypeID typeID, uint64_t rawConvention,
          uint64_t rawDifferentiability) -> llvm::Expected<SILParameterInfo> {
    auto convention = getActualParameterConvention(rawConvention);
    if (!convention)
      return MF.diagnoseFatal();
    auto type = MF.getTypeChecked(typeID);
    if (!type)
      return type.takeError();
    auto differentiability =
        swift::SILParameterDifferentiability::DifferentiableOrNotApplicable;
    if (diffKind != swift::DifferentiabilityKind::NonDifferentiable) {
      auto differentiabilityOpt =
          getActualSILParameterDifferentiability(rawDifferentiability);
      if (!differentiabilityOpt)
        return MF.diagnoseFatal();
      differentiability = *differentiabilityOpt;
    }
    return SILParameterInfo(type.get()->getCanonicalType(), *convention,
                            differentiability);
  };

  auto processYield =
      [&](TypeID typeID,
          uint64_t rawConvention) -> llvm::Expected<SILYieldInfo> {
    auto convention = getActualParameterConvention(rawConvention);
    if (!convention)
      return MF.diagnoseFatal();
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
      return MF.diagnoseFatal();
    auto type = MF.getTypeChecked(typeID);
    if (!type)
      return type.takeError();
    auto differentiability =
        swift::SILResultDifferentiability::DifferentiableOrNotApplicable;
    if (diffKind != swift::DifferentiabilityKind::NonDifferentiable) {
      auto differentiabilityOpt =
          getActualSILResultDifferentiability(rawDifferentiability);
      if (!differentiabilityOpt)
        return MF.diagnoseFatal();
      differentiability = *differentiabilityOpt;
    }
    return SILResultInfo(type.get()->getCanonicalType(), *convention,
                         differentiability);
  };

  // Bounds check.  FIXME: overflow
  unsigned entriesPerParam =
      diffKind != swift::DifferentiabilityKind::NonDifferentiable ? 3 : 2;
  if (entriesPerParam * numParams + 2 * numResults +
          2 * unsigned(hasErrorResult) >
      variableData.size()) {
    return MF.diagnoseFatal();
  }

  unsigned nextVariableDataIndex = 0;

  // Process the parameters.
  SmallVector<SILParameterInfo, 8> allParams;
  allParams.reserve(numParams);
  for (unsigned i = 0; i != numParams; ++i) {
    auto typeID = variableData[nextVariableDataIndex++];
    auto rawConvention = variableData[nextVariableDataIndex++];
    uint64_t rawDifferentiability = 0;
    if (diffKind != swift::DifferentiabilityKind::NonDifferentiable)
      rawDifferentiability = variableData[nextVariableDataIndex++];
    auto param = processParameter(typeID, rawConvention, rawDifferentiability);
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
    if (diffKind != swift::DifferentiabilityKind::NonDifferentiable)
      rawDifferentiability = variableData[nextVariableDataIndex++];
    auto result = processResult(typeID, rawConvention, rawDifferentiability);
    if (!result)
      return result.takeError();
    allResults.push_back(result.get());
  }

  // Process the error result.
  llvm::Optional<SILResultInfo> errorResult;
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
  if (*representation == swift::SILFunctionTypeRepresentation::WitnessMethod) {
    auto conformanceID = variableData[nextVariableDataIndex++];
    witnessMethodConformance = MF.getConformance(conformanceID);
  }

  GenericSignature invocationSig =
      MF.getGenericSignature(rawInvocationGenericSig);
  auto invocationSubsOrErr = MF.getSubstitutionMapChecked(rawInvocationSubs);
  if (!invocationSubsOrErr)
    return invocationSubsOrErr.takeError();
  auto patternSubsOrErr = MF.getSubstitutionMapChecked(rawPatternSubs);
  if (!patternSubsOrErr)
    return patternSubsOrErr.takeError();

  return SILFunctionType::get(invocationSig, extInfo, coroutineKind.value(),
                              calleeConvention.value(), allParams, allYields,
                              allResults, errorResult,
                              patternSubsOrErr.get().getCanonical(),
                              invocationSubsOrErr.get().getCanonical(),
                              MF.getContext(), witnessMethodConformance);
}

Expected<Type> DESERIALIZE_TYPE(ARRAY_SLICE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID baseID;
  decls_block::ArraySliceTypeLayout::readRecord(scratch, baseID);

  auto baseTy = MF.getTypeChecked(baseID);
  if (!baseTy)
    return baseTy.takeError();

  return ArraySliceType::get(baseTy.get());
}

Expected<Type> DESERIALIZE_TYPE(DICTIONARY_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
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

Expected<Type> DESERIALIZE_TYPE(OPTIONAL_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID baseID;
  decls_block::OptionalTypeLayout::readRecord(scratch, baseID);

  auto baseTy = MF.getTypeChecked(baseID);
  if (!baseTy)
    return baseTy.takeError();

  return OptionalType::get(baseTy.get());
}

Expected<Type> DESERIALIZE_TYPE(VARIADIC_SEQUENCE_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID baseID;
  decls_block::VariadicSequenceTypeLayout::readRecord(scratch, baseID);

  auto baseTy = MF.getTypeChecked(baseID);
  if (!baseTy)
    return baseTy.takeError();

  return VariadicSequenceType::get(baseTy.get());
}

Expected<Type> DESERIALIZE_TYPE(UNBOUND_GENERIC_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  DeclID genericID;
  TypeID parentID;
  decls_block::UnboundGenericTypeLayout::readRecord(scratch, genericID,
                                                    parentID);

  auto nominalOrError = MF.getDeclChecked(genericID);
  if (!nominalOrError)
    return nominalOrError.takeError();
  auto genericDecl = cast<GenericTypeDecl>(nominalOrError.get());

  // FIXME: Check this?
  auto parentTy = MF.getType(parentID);

  return UnboundGenericType::get(genericDecl, parentTy, MF.getContext());
}

Expected<Type> DESERIALIZE_TYPE(PACK_EXPANSION_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID patternID;
  TypeID countID;
  decls_block::PackExpansionTypeLayout::readRecord(scratch, patternID, countID);

  auto patternTy = MF.getTypeChecked(patternID);
  if (!patternTy)
    return patternTy.takeError();
  auto countTy = MF.getTypeChecked(countID);
  if (!countTy)
    return countTy.takeError();

  return PackExpansionType::get(patternTy.get(), countTy.get());
}

Expected<Type> DESERIALIZE_TYPE(PACK_ELEMENT_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  TypeID packID;
  unsigned level;
  decls_block::PackElementTypeLayout::readRecord(scratch, packID, level);

  auto packType = MF.getTypeChecked(packID);
  if (!packType)
    return packType.takeError();

  return PackElementType::get(packType.get(), level);
}

Expected<Type> DESERIALIZE_TYPE(PACK_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  ArrayRef<uint64_t> elementTypeIDs;
  decls_block::PackTypeLayout::readRecord(scratch, elementTypeIDs);

  SmallVector<Type, 8> elementTypes;
  for (auto elementTypeID : elementTypeIDs) {
    auto elementType = MF.getTypeChecked(elementTypeID);
    if (!elementType)
      return elementType.takeError();
    elementTypes.push_back(elementType.get());
  }

  return PackType::get(MF.getContext(), elementTypes);
}

Expected<Type> DESERIALIZE_TYPE(SIL_PACK_TYPE)(
    ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch, StringRef blobData) {
  unsigned elementIsAddress;
  ArrayRef<uint64_t> elementTypeIDs;
  decls_block::SILPackTypeLayout::readRecord(scratch, elementIsAddress,
                                             elementTypeIDs);

  SmallVector<CanType, 8> elementTypes;
  for (auto elementTypeID : elementTypeIDs) {
    auto elementType = MF.getTypeChecked(elementTypeID);
    if (!elementType)
      return elementType.takeError();
    elementTypes.push_back(elementType.get()->getCanonicalType());
  }

  return SILPackType::get(MF.getContext(),
                          SILPackType::ExtInfo{bool(elementIsAddress)},
                          elementTypes);
}

Expected<Type> DESERIALIZE_TYPE(ERROR_TYPE)(ModuleFile &MF,
                                            SmallVectorImpl<uint64_t> &scratch,
                                            StringRef blobData) {
  auto &ctx = MF.getContext();
  TypeID origID;
  decls_block::ErrorTypeLayout::readRecord(scratch, origID);

  auto origTyOrError = MF.getTypeChecked(origID);
  if (!origTyOrError)
    return origTyOrError.takeError();

  auto origTy = *origTyOrError;
  auto diagId = MF.allowCompilerErrors()
                    ? diag::serialization_allowing_error_type
                    : diag::serialization_error_type;
  // Generally not a super useful diagnostic, so only output once if there
  // hasn't been any other diagnostic yet to ensure nothing slips by and
  // causes SILGen to crash.
  if (!ctx.hadError()) {
    ctx.Diags.diagnose(SourceLoc(), diagId, StringRef(origTy.getString()),
                       MF.getAssociatedModule()->getNameStr());
  }

  if (!origTy)
    return ErrorType::get(ctx);
  return ErrorType::get(origTy);
}
} // namespace decls_block
} // namespace serialization
}

Expected<Type> ModuleFile::getTypeChecked(TypeID TID) {
  if (TID == 0)
    return Type();

  assert(TID <= Types.size() && "invalid type ID");
  auto &typeOrOffset = Types[TID-1];

  if (typeOrOffset.isComplete())
    return typeOrOffset;

  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (auto error =
          diagnoseFatalIfNotSuccess(DeclTypeCursor.JumpToBit(typeOrOffset)))
    return std::move(error);

  {
    if (auto s = getContext().Stats)
      ++s->getFrontendCounters().NumTypesDeserialized;

    llvm::BitstreamEntry entry =
        fatalIfUnexpected(DeclTypeCursor.advance());

    if (entry.Kind != llvm::BitstreamEntry::Record) {
      // We don't know how to serialize types represented by sub-blocks.
      return diagnoseFatal();
    }

    SmallVector<uint64_t, 64> scratch;
    StringRef blobData;
    unsigned recordID = fatalIfUnexpected(
        DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

    switch (recordID) {
  #define TYPE(TYPE_ID)                                                        \
    case decls_block::detail::TypeRecords::TYPE_ID##_TYPE: {                   \
      auto result = decls_block::detail::TypeRecordDispatch<                   \
          decls_block::detail::TypeRecords::TYPE_ID##_TYPE>::                  \
          deserialize(*this, scratch, blobData);                               \
      if (!result)                                                             \
        return result;                                                         \
      typeOrOffset = result.get();                                             \
    }                                                                          \
      break;
  #include "DeclTypeRecordNodes.def"
  #undef TYPE
    default:
      // We don't know how to deserialize this kind of type.
      return diagnoseFatal(llvm::make_error<InvalidRecordKindError>(recordID));
    }
  }

#ifndef NDEBUG
  PrettyStackTraceType trace(getContext(), "deserializing", typeOrOffset.get());
  if (typeOrOffset.get()->hasError() && !allowCompilerErrors()) {
    typeOrOffset.get()->dump(llvm::errs());
    llvm_unreachable("deserialization produced an invalid type "
                     "(rdar://problem/30382791)");
  }
#endif

  return typeOrOffset.get();
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
  if (auto error =
          diagnoseFatalIfNotSuccess(DeclTypeCursor.JumpToBit(typeOrOffset)))
    return std::move(error);

  llvm::BitstreamEntry entry =
    fatalIfUnexpected(DeclTypeCursor.advance());

  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseFatal();

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = fatalIfUnexpected(
    DeclTypeCursor.readRecord(entry.ID, scratch, &blobData));

  if (recordID != decls_block::CLANG_TYPE)
    fatal(llvm::make_error<InvalidRecordKindError>(recordID));

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

Decl *
ModuleFile::handleErrorAndSupplyMissingMiscMember(llvm::Error &&error) const {
  diagnoseAndConsumeError(std::move(error));
  return nullptr;
}

Decl *
ModuleFile::handleErrorAndSupplyMissingMember(ASTContext &context,
                                              Decl *container,
                                              llvm::Error &&error) const {
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
  if (diagnoseAndConsumeFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(contextData)))
    return;
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return diagnoseAndConsumeFatal();

  SmallVector<uint64_t, 16> memberIDBuffer;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, memberIDBuffer));
  if (kind != decls_block::MEMBERS)
    fatal(llvm::make_error<InvalidRecordKindError>(kind));

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

llvm::Error ModuleFile::consumeExpectedError(llvm::Error &&error) {
    // Missing module errors are most likely caused by an
    // implementation-only import hiding types and decls.
    // rdar://problem/60291019
    if (error.isA<XRefNonLoadedModuleError>() ||
        error.isA<UnsafeDeserializationError>() ||
        error.isA<ModularizationError>()) {
      diagnoseAndConsumeError(std::move(error));
      return llvm::Error::success();
    }

    // Some of these errors may manifest as a TypeError with an
    // XRefNonLoadedModuleError underneath. Catch those as well.
    // rdar://66491720
    if (error.isA<TypeError>()) {
      auto errorInfo = takeErrorInfo(std::move(error));
      auto *TE = static_cast<TypeError*>(errorInfo.get());

      if (TE->underlyingReasonIsA<XRefNonLoadedModuleError>() ||
          TE->underlyingReasonIsA<UnsafeDeserializationError>() ||
          TE->underlyingReasonIsA<ModularizationError>()) {
        diagnoseAndConsumeError(std::move(errorInfo));
        return llvm::Error::success();
      }

      return std::move(errorInfo);
    }

    return std::move(error);
}

void ModuleFile::diagnoseAndConsumeError(llvm::Error error) const {
  auto &ctx = getContext();
  if (ctx.LangOpts.EnableModuleRecoveryRemarks) {
    error = diagnoseModularizationError(std::move(error),
                                        DiagnosticBehavior::Remark);
    // If error was already diagnosed it was also consumed.
    if (!error)
      return;
  }

  consumeError(std::move(error));
}

namespace {
class LazyConformanceLoaderInfo final
    : llvm::TrailingObjects<LazyConformanceLoaderInfo,
                            ProtocolConformanceID> {
  friend TrailingObjects;

  size_t NumConformances;
  size_t numTrailingObjects(OverloadToken<ProtocolConformanceID>) {
    return NumConformances;
  }

  LazyConformanceLoaderInfo(ArrayRef<uint64_t> ids)
    : NumConformances(ids.size()) {
    auto buffer = getTrailingObjects<ProtocolConformanceID>();
    for (unsigned i = 0, e = ids.size(); i != e; ++i)
      buffer[i] = ProtocolConformanceID(ids[i]);
  }

public:
  static LazyConformanceLoaderInfo *create(ModuleFile &mf,
                                           ArrayRef<uint64_t> ids) {
    size_t size = totalSizeToAlloc<ProtocolConformanceID>(ids.size());
    size_t align = alignof(LazyConformanceLoaderInfo);

    // TODO: maybe don't permanently allocate this?
    void *memory = mf.getContext().Allocate(size, align); 
    return new (memory) LazyConformanceLoaderInfo(ids);
  }

  ArrayRef<ProtocolConformanceID> claim() {
    // TODO: free the memory here (if it's not used in multiple places?)
    return llvm::makeArrayRef(getTrailingObjects<ProtocolConformanceID>(),
                              NumConformances);
  }
};
} // end anonymous namespace

uint64_t ModuleFile::createLazyConformanceLoaderToken(ArrayRef<uint64_t> ids) {
  if (ids.empty()) return 0;

  auto info = LazyConformanceLoaderInfo::create(*this, ids);
  return reinterpret_cast<uintptr_t>(info);
}

ArrayRef<ProtocolConformanceID>
ModuleFile::claimLazyConformanceLoaderToken(uint64_t token) {
  if (token == 0) return {};

  auto info = reinterpret_cast<LazyConformanceLoaderInfo*>(token);
  return info->claim();
}

void
ModuleFile::loadAllConformances(const Decl *D, uint64_t contextData,
                          SmallVectorImpl<ProtocolConformance*> &conformances) {
  PrettyStackTraceDecl trace("loading conformances for", D);

  auto conformanceIDs = claimLazyConformanceLoaderToken(contextData);
  for (auto conformanceID : conformanceIDs) {
    auto conformance = getConformanceChecked(conformanceID);

    if (!conformance) {
      auto unconsumedError =
        consumeExpectedError(conformance.takeError());
      if (unconsumedError) {
        // Ignore if allowing errors, it's just doing a best effort to produce
        // *some* module anyway.
        if (allowCompilerErrors())
          diagnoseAndConsumeError(std::move(unconsumedError));
        else
          fatal(std::move(unconsumedError));
      }
      continue;
    }

    // FIXME: why is suppressing abstract conformances generally
    // acceptable here?
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

  PrettyStackTraceModuleFile traceModule(*this);
  PrettyStackTraceConformance trace("finishing conformance for",
                                    conformance);
  ++NumNormalProtocolConformancesCompleted;

  assert(conformance->isComplete());

  conformance->setState(ProtocolConformanceState::Incomplete);
  SWIFT_DEFER { conformance->setState(ProtocolConformanceState::Complete); };

  // Find the conformance record.
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (diagnoseAndConsumeFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(contextData)))
    return;
  llvm::BitstreamEntry entry = fatalIfUnexpected(DeclTypeCursor.advance());
  assert(entry.Kind == llvm::BitstreamEntry::Record &&
         "registered lazy loader incorrectly");

  DeclID protoID;
  DeclContextID contextID;
  unsigned valueCount, typeCount, conformanceCount, isUnchecked;
  ArrayRef<uint64_t> rawIDs;
  SmallVector<uint64_t, 16> scratch;

  unsigned kind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(entry.ID, scratch));
  if (kind != NORMAL_PROTOCOL_CONFORMANCE)
    fatal(llvm::make_error<InvalidRecordKindError>(kind,
                    "registered lazy loader incorrectly"));

  NormalProtocolConformanceLayout::readRecord(scratch, protoID,
                                              contextID, typeCount,
                                              valueCount, conformanceCount,
                                              isUnchecked, rawIDs);

  // Read requirement signature conformances.
  SmallVector<ProtocolConformanceRef, 4> reqConformances;
  for (auto conformanceID : rawIDs.slice(0, conformanceCount)) {
    auto maybeConformance = getConformanceChecked(conformanceID);
    if (maybeConformance) {
      reqConformances.push_back(maybeConformance.get());
    } else if (allowCompilerErrors()) {
      diagnoseAndConsumeError(maybeConformance.takeError());
      reqConformances.push_back(ProtocolConformanceRef::forInvalid());
    } else {
      fatal(maybeConformance.takeError());
    }
  }

  const ProtocolDecl *proto = conformance->getProtocol();
  if (proto->isObjC() && getContext().LangOpts.EnableDeserializationRecovery) {
    // Don't crash if inherited protocols are added or removed.
    // This is limited to Objective-C protocols because we know their only
    // conformance requirements are on Self. This isn't actually a /safe/ change
    // even in Objective-C, but we mostly just don't want to crash.

    llvm::SmallDenseMap<ProtocolDecl *, ProtocolConformanceRef, 16>
        conformancesForProtocols;
    for (auto nextConformance : reqConformances) {
      ProtocolDecl *confProto = nextConformance.getRequirement();
      conformancesForProtocols[confProto] = nextConformance;
    }

    // Reset and rebuild the conformances from what we have.
    reqConformances.clear();
    for (const auto &req : proto->getRequirementSignature().getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance)
        continue;
      ProtocolDecl *proto = req.getProtocolDecl();
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
    auto requirements = proto->getRequirementSignature().getRequirements();
    if (!allowCompilerErrors() &&
        conformanceCount != llvm::count_if(requirements,
                                           isConformanceReq)) {
      fatal(llvm::make_error<llvm::StringError>(
          "serialized conformances do not match requirement signature",
          llvm::inconvertibleErrorCode()));
    }
  }
  conformance->setSignatureConformances(reqConformances);

  ArrayRef<uint64_t>::iterator rawIDIter = rawIDs.begin() + conformanceCount;

  TypeWitnessMap typeWitnesses;
  while (typeCount--) {
    // FIXME: We don't actually want to allocate an archetype here; we just
    // want to get an access path within the protocol.
    auto first = cast<AssociatedTypeDecl>(getDecl(*rawIDIter++));
    auto secondOrError = getTypeChecked(*rawIDIter++);
    Type second;
    if (secondOrError) {
      second = *secondOrError;
    } else if (getContext().LangOpts.EnableDeserializationRecovery) {
      second = ErrorType::get(getContext());
      diagnoseAndConsumeError(secondOrError.takeError());
    } else {
      fatal(secondOrError.takeError());
    }
    auto thirdOrError = getDeclChecked(*rawIDIter++);
    TypeDecl *third;
    if (thirdOrError) {
      third = cast_or_null<TypeDecl>(*thirdOrError);
    } else if (getContext().LangOpts.EnableDeserializationRecovery) {
      third = nullptr;
      diagnoseAndConsumeError(thirdOrError.takeError());
    } else {
      fatal(thirdOrError.takeError());
    }
    if (isa_and_nonnull<TypeAliasDecl>(third) &&
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
      diagnoseAndConsumeError(deserializedReq.takeError());
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
      diagnoseAndConsumeError(deserializedWitness.takeError());
      isOpaque = true;
      witness = nullptr;
    } else {
      fatal(deserializedWitness.takeError());
    }

    assert(allowCompilerErrors() || !req || isOpaque || witness ||
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
      // rdar://problem/52837313. Ignore completely if allowing
      // errors - we're just doing a best effort to create the
      // module in that case.
      if (witnessSubstitutions.errorIsA<XRefNonLoadedModuleError>() ||
          witnessSubstitutions.errorIsA<UnsafeDeserializationError>() ||
          allowCompilerErrors()) {
        diagnoseAndConsumeError(witnessSubstitutions.takeError());
        isOpaque = true;
      }
      else
        fatal(witnessSubstitutions.takeError());
    }

    // Determine whether we need to enter the actor isolation of the witness.
    llvm::Optional<ActorIsolation> enterIsolation;
    if (*rawIDIter++) {
      enterIsolation = getActorIsolation(witness);
    }

    // Handle opaque witnesses that couldn't be deserialized.
    if (isOpaque) {
      trySetOpaqueWitness();
      continue;
    }

    // Set the witness.
    trySetWitness(
        Witness::forDeserialized(
          witness, witnessSubstitutions.get(), enterIsolation));
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
                                          SmallVectorImpl<Requirement> &reqs,
                                          SmallVectorImpl<ProtocolTypeAlias> &typeAliases) {
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (diagnoseAndConsumeFatalIfNotSuccess(
          (DeclTypeCursor.JumpToBit(contextData))))
    return;
  readRequirementSignature(reqs, typeAliases, DeclTypeCursor);
}

void ModuleFile::loadAssociatedTypes(const ProtocolDecl *decl,
                                     uint64_t contextData,
                           SmallVectorImpl<AssociatedTypeDecl *> &assocTypes) {
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (diagnoseAndConsumeFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(contextData)))
    return;
  readAssociatedTypes(assocTypes, DeclTypeCursor);
}

void ModuleFile::loadPrimaryAssociatedTypes(const ProtocolDecl *decl,
                                            uint64_t contextData,
                           SmallVectorImpl<AssociatedTypeDecl *> &assocTypes) {
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  if (diagnoseAndConsumeFatalIfNotSuccess(
          DeclTypeCursor.JumpToBit(contextData)))
    return;
  readPrimaryAssociatedTypes(assocTypes, DeclTypeCursor);
}

static llvm::Optional<ForeignErrorConvention::Kind>
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
    return llvm::None;
  }
}

llvm::Optional<StringRef> ModuleFile::maybeReadInlinableBodyText() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;
  BCOffsetRAII restoreOffset(DeclTypeCursor);
  StringRef blobData;

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return llvm::None;

  unsigned recKind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch, &blobData));
  if (recKind != INLINABLE_BODY_TEXT)
    return llvm::None;

  restoreOffset.reset();
  return blobData;
}

llvm::Optional<ForeignErrorConvention>
ModuleFile::maybeReadForeignErrorConvention() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;

  BCOffsetRAII restoreOffset(DeclTypeCursor);

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return llvm::None;

  unsigned recKind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch));
  switch (recKind) {
  case FOREIGN_ERROR_CONVENTION:
    restoreOffset.reset();
    break;

  default:
    return llvm::None;
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
  else {
    diagnoseAndConsumeFatal();
    return llvm::None;
  }

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

llvm::Optional<ForeignAsyncConvention>
ModuleFile::maybeReadForeignAsyncConvention() {
  using namespace decls_block;

  SmallVector<uint64_t, 8> scratch;

  BCOffsetRAII restoreOffset(DeclTypeCursor);

  llvm::BitstreamEntry next =
      fatalIfUnexpected(DeclTypeCursor.advance(AF_DontPopBlockAtEnd));
  if (next.Kind != llvm::BitstreamEntry::Record)
    return llvm::None;

  unsigned recKind =
      fatalIfUnexpected(DeclTypeCursor.readRecord(next.ID, scratch));
  switch (recKind) {
  case FOREIGN_ASYNC_CONVENTION:
    restoreOffset.reset();
    break;

  default:
    return llvm::None;
  }

  TypeID completionHandlerTypeID;
  unsigned completionHandlerParameterIndex;
  unsigned rawErrorParameterIndex;
  unsigned rawErrorFlagParameterIndex;
  bool errorFlagPolarity;
  ForeignAsyncConventionLayout::readRecord(scratch,
                                           completionHandlerTypeID,
                                           completionHandlerParameterIndex,
                                           rawErrorParameterIndex,
                                           rawErrorFlagParameterIndex,
                                           errorFlagPolarity);

  Type completionHandlerType = getType(completionHandlerTypeID);
  CanType canCompletionHandlerType;
  if (completionHandlerType)
    canCompletionHandlerType = completionHandlerType->getCanonicalType();

  // Decode the error and flag parameters.
  llvm::Optional<unsigned> completionHandlerErrorParamIndex;
  if (rawErrorParameterIndex > 0)
    completionHandlerErrorParamIndex = rawErrorParameterIndex - 1;
  llvm::Optional<unsigned> completionHandlerErrorFlagParamIndex;
  if (rawErrorFlagParameterIndex > 0)
    completionHandlerErrorFlagParamIndex = rawErrorFlagParameterIndex - 1;

  return ForeignAsyncConvention(
      canCompletionHandlerType, completionHandlerParameterIndex,
      completionHandlerErrorParamIndex,
      completionHandlerErrorFlagParamIndex,
      errorFlagPolarity);
}
