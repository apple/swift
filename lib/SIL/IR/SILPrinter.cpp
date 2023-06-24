//===--- SILPrinter.cpp - Pretty-printing of SIL Code ---------------------===//
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
///
/// \file
///
/// This file defines the logic to pretty-print SIL, Instructions, etc.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "swift/Basic/QuotedString.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Demangling/Demangle.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILCoverageMap.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILPrintContext.h"
#include "swift/SIL/SILVTable.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/TerminatorUtils.h"
#include "swift/Strings.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include <set>

using namespace swift;
using ID = SILPrintContext::ID;

llvm::cl::opt<bool>
SILPrintNoColor("sil-print-no-color", llvm::cl::init(""),
                llvm::cl::desc("Don't use color when printing SIL"));

llvm::cl::opt<bool>
SILFullDemangle("sil-full-demangle", llvm::cl::init(false),
                llvm::cl::desc("Fully demangle symbol names in SIL output"));

llvm::cl::opt<bool>
SILPrintDebugInfo("sil-print-debuginfo", llvm::cl::init(false),
                llvm::cl::desc("Include debug info in SIL output"));

llvm::cl::opt<bool>
SILPrintSourceInfo("sil-print-sourceinfo", llvm::cl::init(false),
                   llvm::cl::desc("Include source annotation in SIL output"));

llvm::cl::opt<bool> SILPrintGenericSpecializationInfo(
    "sil-print-generic-specialization-info", llvm::cl::init(false),
    llvm::cl::desc("Include generic specialization"
                   "information info in SIL output"));

static std::string demangleSymbol(StringRef Name) {
  if (SILFullDemangle)
    return Demangle::demangleSymbolAsString(Name);
  return Demangle::demangleSymbolAsString(Name,
                    Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
}

enum SILColorKind {
  SC_Type,
};

namespace {
/// RAII based coloring of SIL output.
class SILColor {
  raw_ostream &OS;
  enum raw_ostream::Colors Color;
public:
#define DEF_COL(NAME, RAW) case NAME: Color = raw_ostream::RAW; break;

  explicit SILColor(raw_ostream &OS, SILColorKind K) : OS(OS) {
    if (!OS.has_colors() || SILPrintNoColor)
      return;
    switch (K) {
      DEF_COL(SC_Type, YELLOW)
    }
    OS.resetColor();
    OS.changeColor(Color);
  }

  explicit SILColor(raw_ostream &OS, ID::ID_Kind K) : OS(OS) {
    if (!OS.has_colors() || SILPrintNoColor)
      return;
    switch (K) {
      DEF_COL(ID::SILUndef, RED)
      DEF_COL(ID::SILBasicBlock, GREEN)
      DEF_COL(ID::SSAValue, MAGENTA)
      DEF_COL(ID::Null, YELLOW)
    }
    OS.resetColor();
    OS.changeColor(Color);
  }
  
  ~SILColor() {
    if (!OS.has_colors() || SILPrintNoColor)
      return;
    // FIXME: instead of resetColor(), we can look into
    // capturing the current active color and restoring it.
    OS.resetColor();
  }
#undef DEF_COL
};
} // end anonymous namespace

void SILPrintContext::ID::print(raw_ostream &OS) {
  SILColor C(OS, Kind);
  switch (Kind) {
  case ID::SILUndef:
    OS << "undef";
    return;
  case ID::SILBasicBlock: OS << "bb"; break;
  case ID::SSAValue: OS << '%'; break;
  case ID::Null: OS << "<<NULL OPERAND>>"; return;
  }
  OS << Number;
}

namespace swift {
raw_ostream &operator<<(raw_ostream &OS, SILPrintContext::ID i) {
  i.print(OS);
  return OS;
}
} // namespace swift

/// IDAndType - Used when a client wants to print something like "%0 : $Int".
struct SILValuePrinterInfo {
  ID ValueID;
  SILType Type;
  Optional<ValueOwnershipKind> OwnershipKind;
  bool IsNoImplicitCopy = false;
  LifetimeAnnotation Lifetime = LifetimeAnnotation::None;
  bool IsCapture = false;
  bool IsReborrow = false;
  bool IsEscaping = false;

  SILValuePrinterInfo(ID ValueID) : ValueID(ValueID), Type(), OwnershipKind() {}
  SILValuePrinterInfo(ID ValueID, SILType Type)
      : ValueID(ValueID), Type(Type), OwnershipKind() {}
  SILValuePrinterInfo(ID ValueID, SILType Type,
                      ValueOwnershipKind OwnershipKind)
      : ValueID(ValueID), Type(Type), OwnershipKind(OwnershipKind) {}
  SILValuePrinterInfo(ID ValueID, SILType Type,
                      ValueOwnershipKind OwnershipKind, bool IsNoImplicitCopy,
                      LifetimeAnnotation Lifetime, bool IsCapture,
                      bool IsReborrow, bool IsEscaping)
      : ValueID(ValueID), Type(Type), OwnershipKind(OwnershipKind),
        IsNoImplicitCopy(IsNoImplicitCopy), Lifetime(Lifetime),
        IsCapture(IsCapture), IsReborrow(IsReborrow), IsEscaping(IsEscaping) {}
  SILValuePrinterInfo(ID ValueID, SILType Type, bool IsNoImplicitCopy,
                      LifetimeAnnotation Lifetime, bool IsCapture,
                      bool IsReborrow, bool IsEscaping)
      : ValueID(ValueID), Type(Type), OwnershipKind(),
        IsNoImplicitCopy(IsNoImplicitCopy), Lifetime(Lifetime),
        IsCapture(IsCapture), IsReborrow(IsReborrow), IsEscaping(IsEscaping) {}
  SILValuePrinterInfo(ID ValueID, SILType Type,
                      ValueOwnershipKind OwnershipKind, bool IsReborrow,
                      bool IsEscaping)
      : ValueID(ValueID), Type(Type), OwnershipKind(OwnershipKind),
        IsReborrow(IsReborrow), IsEscaping(IsEscaping) {}
};

/// Return the fully qualified dotted path for DeclContext.
static void printFullContext(const DeclContext *Context, raw_ostream &Buffer) {
  if (!Context)
    return;
  switch (Context->getContextKind()) {
  case swift::DeclContextKind::Package:
    return;
  case DeclContextKind::Module:
    if (Context == cast<ModuleDecl>(Context)->getASTContext().TheBuiltinModule)
      Buffer << cast<ModuleDecl>(Context)->getName() << ".";
    return;

  case DeclContextKind::FileUnit:
    // Ignore the file; just print the module.
    printFullContext(Context->getParent(), Buffer);
    return;

  case DeclContextKind::MacroDecl:
    // Ignore the macro, which won't have anything in it; just print the module.
    printFullContext(Context->getParent(), Buffer);
    return;

  case DeclContextKind::Initializer:
    // FIXME
    Buffer << "<initializer>";
    return;

  case DeclContextKind::AbstractClosureExpr:
    // FIXME
    Buffer << "<anonymous function>";
    return;

  case DeclContextKind::SerializedLocal:
    Buffer << "<serialized local context>";
    return;

  case DeclContextKind::GenericTypeDecl: {
    auto *generic = cast<GenericTypeDecl>(Context);
    printFullContext(generic->getDeclContext(), Buffer);
    Buffer << generic->getName() << ".";
    return;
  }

  case DeclContextKind::ExtensionDecl: {
    const NominalTypeDecl *ExtNominal =
      cast<ExtensionDecl>(Context)->getExtendedNominal();
    printFullContext(ExtNominal->getDeclContext(), Buffer);
    Buffer << ExtNominal->getName() << ".";
    return;
  }
  
  case DeclContextKind::TopLevelCodeDecl:
    // FIXME
    Buffer << "<top level code>";
    return;
  case DeclContextKind::AbstractFunctionDecl:
    // FIXME
    Buffer << "<abstract function>";
    return;
  case DeclContextKind::SubscriptDecl:
    // FIXME
    Buffer << "<subscript>";
    return;
  case DeclContextKind::EnumElementDecl:
    // FIXME
    Buffer << "<enum element>";
    return;
  }
  llvm_unreachable("bad decl context");
}

static void printValueDecl(ValueDecl *Decl, raw_ostream &OS) {
  printFullContext(Decl->getDeclContext(), OS);

  if (!Decl->hasName()) {
    OS << "anonname=" << (const void*)Decl;
  } else if (Decl->isOperator()) {
    OS << '"' << Decl->getBaseName() << '"';
  } else {
    bool shouldEscape = !Decl->getBaseName().isSpecial() &&
        llvm::StringSwitch<bool>(Decl->getBaseName().userFacingName())
            // FIXME: Represent "init" by a special name and remove this case
            .Case("init", false)
#define KEYWORD(kw) \
            .Case(#kw, true)
#include "swift/AST/TokenKinds.def"
            .Default(false);

    if (shouldEscape) {
      OS << '`' << Decl->getBaseName().userFacingName() << '`';
    } else {
      OS << Decl->getBaseName().userFacingName();
    }
  }
}

/// SILDeclRef uses sigil "#" and prints the fully qualified dotted path.
void SILDeclRef::print(raw_ostream &OS) const {
  OS << "#";
  if (isNull()) {
    OS << "<null>";
    return;
  }

  bool isDot = true;
  switch (getLocKind()) {
  case LocKind::Closure:
    OS << "<anonymous function>";
    break;
  case LocKind::File:
    OS << "<file>";
    break;
  case LocKind::Decl: {
    if (kind != Kind::Func) {
      printValueDecl(getDecl(), OS);
      break;
    }

    auto *accessor = dyn_cast<AccessorDecl>(getDecl());
    if (!accessor) {
      printValueDecl(getDecl(), OS);
      isDot = false;
      break;
    }

    printValueDecl(accessor->getStorage(), OS);
    switch (accessor->getAccessorKind()) {
    case AccessorKind::WillSet:
      OS << "!willSet";
      break;
    case AccessorKind::DidSet:
      OS << "!didSet";
      break;
    case AccessorKind::Get:
      OS << "!getter";
      break;
    case AccessorKind::Set:
      OS << "!setter";
      break;
    case AccessorKind::Address:
      OS << "!addressor";
      break;
    case AccessorKind::MutableAddress:
      OS << "!mutableAddressor";
      break;
    case AccessorKind::Read:
      OS << "!read";
      break;
    case AccessorKind::Modify:
      OS << "!modify";
      break;
    case AccessorKind::Init:
      OS << "!init";
      break;
    }
    break;
  }
  }

  switch (kind) {
  case SILDeclRef::Kind::Func:
  case SILDeclRef::Kind::EntryPoint:
  case SILDeclRef::Kind::AsyncEntryPoint:
    break;
  case SILDeclRef::Kind::Allocator:
    OS << "!allocator";
    break;
  case SILDeclRef::Kind::Initializer:
    OS << "!initializer";
    break;
  case SILDeclRef::Kind::EnumElement:
    OS << "!enumelt";
    break;
  case SILDeclRef::Kind::Destroyer:
    OS << "!destroyer";
    break;
  case SILDeclRef::Kind::Deallocator:
    OS << "!deallocator";
    break;
  case SILDeclRef::Kind::IVarInitializer:
    OS << "!ivarinitializer";
    break;
  case SILDeclRef::Kind::IVarDestroyer:
    OS << "!ivardestroyer";
    break;
  case SILDeclRef::Kind::GlobalAccessor:
    OS << "!globalaccessor";
    break;
  case SILDeclRef::Kind::DefaultArgGenerator:
    OS << "!defaultarg" << "." << defaultArgIndex;
    break;
  case SILDeclRef::Kind::StoredPropertyInitializer:
    OS << "!propertyinit";
    break;
  case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
    OS << "!backinginit";
    break;
  case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
    OS << "!projectedvalueinit";
    break;
  case SILDeclRef::Kind::RuntimeAttributeGenerator:
    OS << "!attrgenerator";
    break;
  }

  if (isForeign)
    OS << (isDot ? '.' : '!')  << "foreign";

  if (getDerivativeFunctionIdentifier()) {
    OS << ((isDot || isForeign) ? '.' : '!');
    switch (getDerivativeFunctionIdentifier()->getKind()) {
    case AutoDiffDerivativeFunctionKind::JVP:
      OS << "jvp.";
      break;
    case AutoDiffDerivativeFunctionKind::VJP:
      OS << "vjp.";
      break;
    }
    OS << getDerivativeFunctionIdentifier()->getParameterIndices()->getString();
    if (auto derivativeGenSig =
            getDerivativeFunctionIdentifier()->getDerivativeGenericSignature()) {
      OS << "." << derivativeGenSig;
    }
  }
}

void SILDeclRef::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

/// Pretty-print the generic specialization information.
static void printGenericSpecializationInfo(
    raw_ostream &OS, StringRef Kind, StringRef Name,
    const GenericSpecializationInformation *SpecializationInfo,
    SubstitutionMap Subs = { }) {
  if (!SpecializationInfo && Subs.empty())
    return;

  auto PrintSubstitutions = [&](SubstitutionMap Subs) {
    OS << '<';
    interleave(Subs.getReplacementTypes(),
               [&](Type type) { OS << type; },
               [&] { OS << ", "; });
    OS << '>';
    OS << " conformances <";
    interleave(Subs.getConformances(),
               [&](ProtocolConformanceRef conf) { conf.print(OS); },
               [&] { OS << ", ";});
    OS << '>';
  };

  OS << "// Generic specialization information for " << Kind << " " << Name;
  if (!Subs.empty()) {
    OS << " ";
    PrintSubstitutions(Subs);
  }

  OS << ":\n";

  while (SpecializationInfo) {
    OS << "// Caller: " << SpecializationInfo->getCaller()->getName() << '\n';
    OS << "// Parent: " << SpecializationInfo->getParent()->getName() << '\n';
    OS << "// Substitutions: ";
    PrintSubstitutions(SpecializationInfo->getSubstitutions());
    OS << '\n';
    OS << "//\n";
    if (!SpecializationInfo->getCaller()->isSpecialization())
      return;
    SpecializationInfo =
      SpecializationInfo->getCaller()->getSpecializationInfo();
  }
}

static void print(raw_ostream &OS, SILValueCategory category) {
  switch (category) {
  case SILValueCategory::Object: return;
  case SILValueCategory::Address: OS << '*'; return;
  }
  llvm_unreachable("bad value category!");
}

static StringRef getCastConsumptionKindName(CastConsumptionKind kind) {
  switch (kind) {
  case CastConsumptionKind::TakeAlways: return "take_always";
  case CastConsumptionKind::TakeOnSuccess: return "take_on_success";
  case CastConsumptionKind::CopyOnSuccess: return "copy_on_success";
  case CastConsumptionKind::BorrowAlways: return "borrow_always";
  }
  llvm_unreachable("bad cast consumption kind");
}

static void printSILTypeColorAndSigil(raw_ostream &OS, SILType t) {
  SILColor C(OS, SC_Type);
  OS << '$';
  
  // Potentially add a leading sigil for the value category.
  ::print(OS, t.getCategory());
}

void SILType::print(raw_ostream &OS, const PrintOptions &PO) const {
  printSILTypeColorAndSigil(OS, *this);

  // Print other types as their Swift representation.
  //
  // NOTE: We always print the Raw AST type so we don't look through
  // move-onlyness.
  getRawASTType().print(OS, PO);
}

void SILType::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

/// Prints the name and type of the given SIL function with the given
/// `PrintOptions`. Computes mapping for sugared type names and stores the
/// result in `sugaredTypeNames`.
static void printSILFunctionNameAndType(
    llvm::raw_ostream &OS, const SILFunction *function,
    llvm::DenseMap<CanType, Identifier> &sugaredTypeNames,
    const SILPrintContext *silPrintContext = nullptr) {
  function->printName(OS);
  OS << " : $";
  auto *genEnv = function->getGenericEnvironment();
  GenericSignature genSig;

  // If `genEnv` is defined, get sugared names of generic
  // parameter types for printing.
  if (genEnv) {
    genSig = genEnv->getGenericSignature();

    llvm::DenseSet<Identifier> usedNames;
    llvm::SmallString<16> disambiguatedNameBuf;
    unsigned disambiguatedNameCounter = 1;
    for (auto *paramTy : genSig.getGenericParams()) {
      // Get a uniqued sugared name for the generic parameter type.
      auto sugaredTy = genEnv->getGenericSignature()->getSugaredType(paramTy);
      Identifier name = sugaredTy->getName();
      while (!usedNames.insert(name).second) {
        disambiguatedNameBuf.clear();
        {
          llvm::raw_svector_ostream names(disambiguatedNameBuf);
          names << sugaredTy->getName() << disambiguatedNameCounter++;
        }
        name = function->getASTContext().getIdentifier(disambiguatedNameBuf);
      }
      // If the uniqued sugared name is equal to the sugared name, continue.
      if (name == sugaredTy->getName())
        continue;
      // Otherwise, add sugared name mapping for the type (and its archetype, if
      // defined).
      sugaredTypeNames[paramTy->getCanonicalType()] = name;
      if (auto *archetypeTy =
              genEnv->mapTypeIntoContext(paramTy)->getAs<ArchetypeType>())
        sugaredTypeNames[archetypeTy->getCanonicalType()] = name;
    }
  }
  auto printOptions = PrintOptions::printSIL(silPrintContext);
  printOptions.GenericSig = genSig.getPointer();
  printOptions.AlternativeTypeNames =
      sugaredTypeNames.empty() ? nullptr : &sugaredTypeNames;
  function->getLoweredFunctionType()->print(OS, printOptions);
}

/// Prints the name and type of the given SIL function.
static void printSILFunctionNameAndType(llvm::raw_ostream &OS,
                                        const SILFunction *function) {
  llvm::DenseMap<CanType, Identifier> sugaredTypeNames;
  printSILFunctionNameAndType(OS, function, sugaredTypeNames);
}

namespace {

// 1. Accumulate opcode-specific comments in this stream.
// 2. Start emitting comments: lineComments.start()
// 3. Emit each comment section: lineComments.delim()
// 4. End emitting comments: LineComments::end()
class LineComments : public raw_ostream {
  llvm::formatted_raw_ostream &os;
  // Opcode-specific comments to be printed at the end of the current line.
  std::string opcodeCommentString;
  llvm::raw_string_ostream opcodeCommentStream;
  bool emitting = false;
  bool printedSlashes = false;

public:
  LineComments(llvm::formatted_raw_ostream &os)
      : os(os), opcodeCommentStream(opcodeCommentString) {
    SetUnbuffered(); // pass through to the underlying stream
  }

  // Call to start emitting line comments into the underlying stream.
  void start() {
    emitting = true;
    printedSlashes = false;

    if (opcodeCommentString.empty())
      return;

    delim();
    os << opcodeCommentString;
    opcodeCommentString.clear();
  }
  // Call for each section of line
  void delim() {
    assert(emitting);
    if (printedSlashes) {
      os << "; ";
    } else {
      os.PadToColumn(50);
      os << "// ";
      printedSlashes = true;
    }
  }
  void end() {
    assert(emitting);
    emitting = false;
    printedSlashes = false;
    os << "\n";
  }

protected:
  void write_impl(const char *ptr, size_t size) override {
    if (emitting)
      os.write(ptr, size);
    else
      opcodeCommentStream.write(ptr, size);
  }
  uint64_t current_pos() const override {
    if (emitting)
      return os.tell() - os.GetNumBytesInBuffer();

    return opcodeCommentString.size();
  }
};

} // namespace

namespace swift {

/// SILPrinter class - This holds the internal implementation details of
/// printing SIL structures.
class SILPrinter : public SILInstructionVisitor<SILPrinter> {
  SILPrintContext &Ctx;
  struct {
    llvm::formatted_raw_ostream OS;
    PrintOptions ASTOptions;
  } PrintState;
  LineComments lineComments;
  unsigned LastBufferID;

  // Printers for the underlying stream.
#define SIMPLE_PRINTER(TYPE) \
  SILPrinter &operator<<(TYPE value) { \
    PrintState.OS << value;            \
    return *this;                      \
  }
  SIMPLE_PRINTER(char)
  SIMPLE_PRINTER(unsigned)
  SIMPLE_PRINTER(uint64_t)
  SIMPLE_PRINTER(int64_t)
  SIMPLE_PRINTER(StringRef)
  SIMPLE_PRINTER(Identifier)
  SIMPLE_PRINTER(ID)
  SIMPLE_PRINTER(QuotedString)
  SIMPLE_PRINTER(SILDeclRef)
  SIMPLE_PRINTER(APInt)
  SIMPLE_PRINTER(ValueOwnershipKind)
  SIMPLE_PRINTER(UUID)
  SIMPLE_PRINTER(GenericSignature)
#undef SIMPLE_PRINTER

  SILPrinter &operator<<(SILValuePrinterInfo i) {
    SILColor C(PrintState.OS, SC_Type);
    *this << i.ValueID;
    if (!i.Type)
      return *this;
    *this << " : ";
    if (i.IsNoImplicitCopy)
      *this << "@noImplicitCopy ";
    switch (i.Lifetime) {
    case LifetimeAnnotation::EagerMove:
      *this << "@_eagerMove ";
      break;
    case LifetimeAnnotation::None:
      break;
    case LifetimeAnnotation::Lexical:
      *this << "@_lexical ";
      break;
    }
    if (i.IsCapture)
      *this << "@closureCapture ";
    if (i.IsReborrow)
      *this << "@reborrow ";
    if (i.IsEscaping)
      *this << "@pointer_escape ";
    if (i.OwnershipKind && *i.OwnershipKind != OwnershipKind::None) {
      *this << "@" << i.OwnershipKind.value() << " ";
    }
    return *this << i.Type;
  }

  SILPrinter &operator<<(Type t) {
    // Print the type using our print options.
    t.print(PrintState.OS, PrintState.ASTOptions);
    return *this;
  }
  
  SILPrinter &operator<<(SILType t) {
    printSILTypeColorAndSigil(PrintState.OS, t);
    t.getRawASTType().print(PrintState.OS, PrintState.ASTOptions);
    return *this;
  }
  
public:
  SILPrinter(
      SILPrintContext &PrintCtx,
      llvm::DenseMap<CanType, Identifier> *AlternativeTypeNames = nullptr)
      : Ctx(PrintCtx), PrintState{{PrintCtx.OS()},
                                  PrintOptions::printSIL(&PrintCtx)},
        lineComments(PrintState.OS), LastBufferID(0) {
    PrintState.ASTOptions.AlternativeTypeNames = AlternativeTypeNames;
    PrintState.ASTOptions.PrintForSIL = true;
  }

  SILValuePrinterInfo getIDAndType(SILValue V) {
    return {Ctx.getID(V), V ? V->getType() : SILType()};
  }
  SILValuePrinterInfo getIDAndType(SILFunctionArgument *arg) {
    return {Ctx.getID(arg),          arg->getType(),
            arg->isNoImplicitCopy(), arg->getLifetimeAnnotation(),
            arg->isClosureCapture(), arg->isReborrow(),
            arg->hasPointerEscape()};
  }

  SILValuePrinterInfo getIDAndTypeAndOwnership(SILValue V) {
    return {Ctx.getID(V), V ? V->getType() : SILType(), V->getOwnershipKind()};
  }
  SILValuePrinterInfo getIDAndTypeAndOwnership(SILFunctionArgument *arg) {
    return {Ctx.getID(arg),
            arg->getType(),
            arg->getOwnershipKind(),
            arg->isNoImplicitCopy(),
            arg->getLifetimeAnnotation(),
            arg->isClosureCapture(),
            arg->isReborrow(),
            arg->hasPointerEscape()};
  }
  SILValuePrinterInfo getIDAndTypeAndOwnership(SILArgument *arg) {
    return {Ctx.getID(arg), arg->getType(), arg->getOwnershipKind(),
            arg->isReborrow(), arg->hasPointerEscape()};
  }

  //===--------------------------------------------------------------------===//
  // Big entrypoints.
  void print(const SILFunction *F) {
    // If we are asked to emit sorted SIL, print out our BBs in RPOT order.
    if (Ctx.sortSIL()) {
      std::vector<SILBasicBlock *> RPOT;
      auto *UnsafeF = const_cast<SILFunction *>(F);
      std::copy(po_begin(UnsafeF), po_end(UnsafeF),
                std::back_inserter(RPOT));
      std::reverse(RPOT.begin(), RPOT.end());
      Ctx.initBlockIDs(RPOT);
      interleave(RPOT,
                 [&](SILBasicBlock *B) { print(B); },
                 [&] { *this << '\n'; });
      return;
    }

    interleave(*F,
               [&](const SILBasicBlock &B) { print(&B); },
               [&] { *this << '\n'; });
  }

  void printBlockArgumentUses(const SILBasicBlock *BB) {
    if (BB->args_empty())
      return;

    for (SILArgument *arg : BB->getArguments()) {
      StringRef name;
      if (arg->getDecl() && arg->getDecl()->hasName())
        name = arg->getDecl()->getBaseName().userFacingName();

      if (arg->use_empty() && name.empty())
        continue;

      *this << "// " << Ctx.getID(arg);
      if (!name.empty()) {
        *this << " \"" << name << '\"';
      }
      if (!arg->use_empty()) {
        PrintState.OS.PadToColumn(50);
        *this << "// user";
        if (std::next(arg->use_begin()) != arg->use_end())
          *this << 's';
        *this << ": ";

        llvm::SmallVector<ID, 32> UserIDs;
        for (auto *Op : arg->getUses())
          UserIDs.push_back(Ctx.getID(Op->getUser()));

        // Display the user ids sorted to give a stable use order in the
        // printer's output if we are asked to do so. This makes diffing large
        // sections of SIL significantly easier at the expense of not showing
        // the _TRUE_ order of the users in the use list.
        if (Ctx.sortSIL()) {
          std::sort(UserIDs.begin(), UserIDs.end());
        }

        llvm::interleave(
            UserIDs.begin(), UserIDs.end(), [&](ID id) { *this << id; },
            [&] { *this << ", "; });
      }
      *this << '\n';
    }
  }

  void printBlockArguments(const SILBasicBlock *BB) {
    if (BB->args_empty())
      return;
    *this << '(';
    // If SIL ownership is enabled and the given function has not had ownership
    // stripped out, print out ownership of SILArguments.
    if (BB->getParent()->hasOwnership()) {
      if (BB->isEntry()) {
        auto Args = BB->getSILFunctionArguments();
        *this << getIDAndTypeAndOwnership(Args[0]);
        for (unsigned i : range(1, Args.size())) {
          SILFunctionArgument *Arg = Args[i];
          *this << ", " << getIDAndTypeAndOwnership(Arg);
        }
        *this << ')';
      } else {
        ArrayRef<SILArgument *> Args = BB->getArguments();
        *this << getIDAndTypeAndOwnership(Args[0]);
        for (SILArgument *Arg : Args.drop_front()) {
          *this << ", " << getIDAndTypeAndOwnership(Arg);
        }
        *this << ')';
      }
      return;
    }

    if (BB->isEntry()) {
      auto Args = BB->getSILFunctionArguments();
      *this << getIDAndType(Args[0]);
      for (unsigned i : range(1, Args.size())) {
        SILFunctionArgument *Arg = Args[i];
        *this << ", " << getIDAndType(Arg);
      }
      *this << ')';
      return;
    }

    ArrayRef<SILArgument *> Args = BB->getArguments();
    *this << getIDAndType(Args[0]);
    for (SILArgument *Arg : Args.drop_front()) {
      *this << ", " << getIDAndType(Arg);
    }
    *this << ')';
  }

#ifndef NDEBUG
  void printID(const SILBasicBlock *BB) {
    *this << Ctx.getID(BB) << "\n";
  }
#endif

  void print(const SILBasicBlock *BB) {
    // Output uses for BB arguments. These are put into place as comments before
    // the block header.
    printBlockArgumentUses(BB);

    // If the basic block has a name available, print it as well
    auto debugName = BB->getDebugName();
    if (debugName.has_value()) {
      *this << "// " << debugName.value() << '\n';
    }

    // Then print the name of our block, the arguments, and the block colon.
    *this << Ctx.getID(BB);
    printBlockArguments(BB);
    *this << ":";

    if (!BB->pred_empty()) {
      PrintState.OS.PadToColumn(50);
      
      *this << "// Preds:";

      llvm::SmallVector<ID, 32> PredIDs;
      for (auto *BBI : BB->getPredecessorBlocks())
        PredIDs.push_back(Ctx.getID(BBI));

      // Display the pred ids sorted to give a stable use order in the printer's
      // output if we are asked to do so. This makes diffing large sections of
      // SIL significantly easier at the expense of not showing the _TRUE_ order
      // of the users in the use list.
      if (Ctx.sortSIL()) {
        std::sort(PredIDs.begin(), PredIDs.end());
      }

      for (auto Id : PredIDs)
        *this << ' ' << Id;
    }
    *this << '\n';

    const auto &SM = BB->getModule().getASTContext().SourceMgr;
    Optional<SILLocation> PrevLoc;
    for (const SILInstruction &I : *BB) {
      if (SILPrintSourceInfo) {
        auto CurSourceLoc = I.getLoc().getSourceLoc();
        if (CurSourceLoc.isValid()) {
          if (!PrevLoc ||
              SM.getLineAndColumnInBuffer(CurSourceLoc).first >
                  SM.getLineAndColumnInBuffer(PrevLoc->getSourceLoc()).first) {
            auto Buffer = SM.findBufferContainingLoc(CurSourceLoc);
            auto Line = SM.getLineAndColumnInBuffer(CurSourceLoc).first;
            auto LineLength = SM.getLineLength(Buffer, Line);
            PrintState.OS << "  // "
                          << SM.extractText(
                                 {SM.getLocForLineCol(Buffer, Line, 0),
                                  LineLength.value_or(0)})
                          << "\tSourceLoc: "
                          << SM.getDisplayNameForLoc(CurSourceLoc) << ":"
                          << Line << "\n";
            PrevLoc = I.getLoc();
          }
        }
      }
      Ctx.printInstructionCallBack(&I);
      if (SILPrintGenericSpecializationInfo) {
        if (auto AI = ApplySite::isa(const_cast<SILInstruction *>(&I)))
          if ((AI.getSpecializationInfo() ||
               !AI.getSubstitutionMap().empty()) &&
              AI.getCalleeFunction())
            printGenericSpecializationInfo(
                PrintState.OS, "call-site", AI.getCalleeFunction()->getName(),
                AI.getSpecializationInfo(), AI.getSubstitutionMap());
      }
      print(&I);
    }
  }

  //===--------------------------------------------------------------------===//
  // SILInstruction Printing Logic

  void printTypeDependentOperands(const SILInstruction *I) {
    ArrayRef<Operand> TypeDepOps = I->getTypeDependentOperands();
    if (TypeDepOps.empty())
      return;

    lineComments.delim();
    *this << "type-defs: ";
    interleave(TypeDepOps,
               [&](const Operand &op) { *this << Ctx.getID(op.get()); },
               [&] { *this << ", "; });
  }

  /// Print out the users of the SILValue \p V. Return true if we printed out
  /// either an id or a use list. Return false otherwise.
  void printUsersOfValue(SILValue value) {
    lineComments.start();
    printUserList({value}, value);
    lineComments.end();
  }

  void printUsersOfInstruction(const SILInstruction *inst) {
    llvm::SmallVector<SILValue, 8> values;
    llvm::copy(inst->getResults(), std::back_inserter(values));
    printUserList(values, inst);
    printBranchTargets(inst);
  }

  void printUserList(ArrayRef<SILValue> values, SILNodePointer node) {
    // If the set of values is empty, we need to print the ID of
    // the instruction.  Otherwise, if none of the values has a use,
    // we don't need to do anything.
    if (!values.empty()) {
      bool hasUse = false;
      for (auto value : values) {
        if (!value->use_empty()) hasUse = true;
      }
      if (!hasUse)
        return;
    }
    lineComments.delim();

    if (values.empty()) {
      *this << "id: " << Ctx.getID(node);
      return;
    }

    llvm::SmallVector<ID, 32> UserIDs;
    for (auto value : values)
      for (auto *Op : value->getUses())
        UserIDs.push_back(Ctx.getID(Op->getUser()));

    *this << "user";
    if (UserIDs.size() != 1)
      *this << 's';
    *this << ": ";

    // If we are asked to, display the user ids sorted to give a stable use
    // order in the printer's output. This makes diffing large sections of SIL
    // significantly easier.
    if (Ctx.sortSIL()) {
      std::sort(UserIDs.begin(), UserIDs.end());
    }

    llvm::interleave(
        UserIDs.begin(), UserIDs.end(), [&](ID id) { *this << id; },
        [&] { *this << ", "; });
  }

  void printBranchTargets(const SILInstruction *inst) {
    if (auto condBr = dyn_cast<CondBranchInst>(inst)) {
      if (condBr->getTrueBB()->getDebugName().has_value()) {
        *this << ", true->" << condBr->getTrueBB()->getDebugName().value();
      }
      if (condBr->getFalseBB()->getDebugName().has_value()) {
        *this << ", false->" << condBr->getFalseBB()->getDebugName().value();
      }
    } else if (auto br = dyn_cast<BranchInst>(inst)) {
      if (br->getDestBB()->getDebugName().has_value()) {
        *this << ", dest->" << br->getDestBB()->getDebugName().value();
      }
    } else if (auto termInst = dyn_cast<TermInst>(inst)) {
      // Otherwise, we just print the successors in order without pretty printing
      for (unsigned i = 0, numSuccessors = termInst->getSuccessors().size();
           i != numSuccessors; ++i) {
        auto &successor = termInst->getSuccessors()[i];
        if (successor.getBB()->getDebugName().has_value()) {
          *this << ", #" << i
                << "->" << successor.getBB()->getDebugName().value();
        }
      }
    }
  }

  void printConformances(ArrayRef<ProtocolConformanceRef> conformances) {
    // FIXME: conformances should always be printed and parsed!
    if (!Ctx.printVerbose()) {
      return;
    }
    for (ProtocolConformanceRef conformance : conformances) {
      conformance.dump(lineComments, /*indent*/ 0, /*details*/ false);
    }
  }

  void printDebugLocRef(SILLocation Loc, const SourceManager &SM,
                        bool PrintComma = true) {
    auto DL = Loc.decodeForDebugging(SM);
    if (!DL.filename.empty()) {
      if (PrintComma)
        *this << ", ";
      *this << "loc ";
      if (Loc.isAutoGenerated())
        *this << "* ";
      *this << QuotedString(DL.filename) << ':' << DL.line << ':'
            << (unsigned)DL.column;
    }
  }

  void printDebugScope(const SILDebugScope *DS, const SourceManager &SM) {
    if (!DS)
      return;

    if (!Ctx.hasScopeID(DS)) {
      printDebugScope(DS->Parent.dyn_cast<const SILDebugScope *>(), SM);
      printDebugScope(DS->InlinedCallSite, SM);
      unsigned ID = Ctx.assignScopeID(DS);
      *this << "sil_scope " << ID << " { ";
      printDebugLocRef(DS->Loc, SM, false);
      *this << " parent ";
      if (auto *F = DS->Parent.dyn_cast<SILFunction *>())
        *this << "@" << F->getName() << " : $" << F->getLoweredFunctionType();
      else {
        auto *PS = DS->Parent.get<const SILDebugScope *>();
        *this << Ctx.getScopeID(PS);
      }
      if (auto *CS = DS->InlinedCallSite)
        *this << " inlined_at " << Ctx.getScopeID(CS);
      *this << " }\n";
    }
  }

  void printDebugScopeRef(const SILDebugScope *DS, const SourceManager &SM,
                          bool PrintComma = true) {
    if (DS) {
      if (PrintComma)
        *this << ", ";
      *this << "scope " << Ctx.getScopeID(DS);
    }
  }

  void printSILLocation(SILLocation L, SILModule &M, const SILDebugScope *DS) {
    lineComments.delim();
    if (!L.isNull()) {
      // To minimize output, only print the line and column number for
      // everything but the first instruction.
      L.getSourceLoc().printLineAndColumn(PrintState.OS,
                                          M.getASTContext().SourceMgr);

      // Print the type of location.
      switch (L.getKind()) {
      case SILLocation::RegularKind:
        break;
      case SILLocation::ReturnKind:
        *this << ":return";
        break;
      case SILLocation::ImplicitReturnKind:
        *this << ":imp_return";
        break;
      case SILLocation::InlinedKind:
        *this << ":inlined";
        break;
      case SILLocation::MandatoryInlinedKind:
        *this << ":minlined";
        break;
      case SILLocation::CleanupKind:
        *this << ":cleanup";
        break;
      case SILLocation::ArtificialUnreachableKind:
        *this << ":art_unreach";
        break;
      }
      if (L.isSILFile())
        *this << ":sil";
      if (L.isAutoGenerated())
        *this << ":auto_gen";
      if (L.isInPrologue())
        *this << ":in_prologue";
    } else {
      if (L.isAutoGenerated())
        *this << " auto_gen";
      else
        *this << " no_loc";
      if (L.isInPrologue())
        *this << ":in_prologue";
    }

    if (!DS)
      return;

    // Print inlined-at location, if any.
    const SILDebugScope *CS = DS;
    while ((CS = CS->InlinedCallSite)) {
      *this << ": ";
      if (auto *InlinedF = CS->getInlinedFunction())
        *this << demangleSymbol(InlinedF->getName());
      else
        *this << '?';
      *this << " perf_inlined_at ";
      auto CallSite = CS->Loc;
      if (!CallSite.isNull() && CallSite.isASTNode())
        CallSite.getSourceLoc().print(
            PrintState.OS, M.getASTContext().SourceMgr, LastBufferID);
      else
        *this << "?";
    }
  }

  void printInstOpCode(const SILInstruction *I) {
    *this << getSILInstructionName(I->getKind()) << " ";
  }

  void print(const SILInstruction *I) {
    if (auto *FRI = dyn_cast<FunctionRefInst>(I))
      *this << "  // function_ref "
            << demangleSymbol(FRI->getReferencedFunction()->getName())
            << "\n";
    else if (auto *FRI = dyn_cast<DynamicFunctionRefInst>(I))
      *this << "  // dynamic_function_ref "
            << demangleSymbol(FRI->getInitiallyReferencedFunction()->getName())
            << "\n";
    else if (auto *FRI = dyn_cast<PreviousDynamicFunctionRefInst>(I))
      *this << "  // prev_dynamic_function_ref "
            << demangleSymbol(FRI->getInitiallyReferencedFunction()->getName())
            << "\n";

    *this << "  ";

    // Print results.
    auto results = I->getResults();
    if (results.size() == 1 && !I->isDeleted() && I->isStaticInitializerInst()
        && I == &I->getParent()->back()) {
      *this << "%initval = ";
    } else if (results.size() == 1) {
      ID Name = Ctx.getID(results[0]);
      *this << Name << " = ";
    } else if (results.size() > 1) {
      *this << '(';
      bool first = true;
      for (auto result : results) {
        if (first) {
          first = false;
        } else {
          *this << ", ";
        }
        ID Name = Ctx.getID(result);
        *this << Name;
      }
      *this << ") = ";
    }

    // Print the opcode.
    printInstOpCode(I);

    // Use the visitor to print the rest of the instruction.
    visit(const_cast<SILInstruction*>(I));

    // Maybe print debugging information.
    if (Ctx.printDebugInfo() && !I->isDeleted()
        && !I->isStaticInitializerInst()) {
      auto &SM = I->getModule().getASTContext().SourceMgr;
      printDebugLocRef(I->getLoc(), SM);
      printDebugScopeRef(I->getDebugScope(), SM);
    }

    lineComments.start();

    printTypeDependentOperands(I);

    // Print users, or id for valueless instructions.
    printUsersOfInstruction(I);

    // Print SIL location.
    if (Ctx.printVerbose()) {
      printSILLocation(I->getLoc(), I->getModule(), I->getDebugScope());
    }
    lineComments.end();
  }

  void print(const SILNode *node) {
    switch (node->getKind()) {
#define INST(ID, PARENT) \
    case SILNodeKind::ID:
#include "swift/SIL/SILNodes.def"
      print(cast<SILInstruction>(node));
      return;

#define ARGUMENT(ID, PARENT) \
    case SILNodeKind::ID:
#include "swift/SIL/SILNodes.def"
      printSILArgument(cast<SILArgument>(node));
      return;

    case SILNodeKind::SILUndef:
      printSILUndef(cast<SILUndef>(node));
      return;

    case SILNodeKind::PlaceholderValue:
      // This should really only happen during debugging.
      *this << "placeholder<" << cast<PlaceholderValue>(node)->getType()
            << ">\n";
      return;

    case SILNodeKind::MultipleValueInstructionResult:
      printSILMultipleValueInstructionResult(
          cast<MultipleValueInstructionResult>(node));
      return;
    }
    llvm_unreachable("bad kind");
  }

  void printSILArgument(const SILArgument *arg) {
    // This should really only happen during debugging.
    *this << Ctx.getID(arg) << " = argument of "
          << Ctx.getID(arg->getParent()) << " : " << arg->getType();

    // Print users.
    printUsersOfValue(arg);
  }

  void printSILUndef(const SILUndef *undef) {
    // This should really only happen during debugging.
    *this << "undef<" << undef->getType() << ">\n";
  }

  void printSILMultipleValueInstructionResult(
      const MultipleValueInstructionResult *result) {
    // This should really only happen during debugging.
    if (result->getParent()->getNumResults() == 1) {
      *this << "**" << Ctx.getID(result) << "**";
    } else {
      *this << '(';
      llvm::interleave(
          result->getParent()->getResults(),
          [&](SILValue value) {
            if (value == SILValue(result)) {
              *this << "**" << Ctx.getID(result) << "**";
              return;
            }
            *this << Ctx.getID(value);
          },
          [&] { *this << ", "; });
      *this << ')';
    }

    *this << " = ";
    printInstOpCode(result->getParent());
    auto *nonConstParent =
        const_cast<MultipleValueInstruction *>(result->getParent());
    visit(static_cast<SILInstruction *>(nonConstParent));

    // Print users.
    printUsersOfValue(result);
  }

  void printInContext(const SILNode *node) {
    auto sortByID = [&](SILNodePointer a, SILNodePointer b) {
      return Ctx.getID(a).Number < Ctx.getID(b).Number;
    };

    if (auto *I = dyn_cast<SILInstruction>(node)) {
      auto operands = map<SmallVector<SILValue,4>>(I->getAllOperands(),
                                                   [](Operand const &o) {
                                                     return o.get();
                                                   });
      std::sort(operands.begin(), operands.end(), sortByID);
      for (auto &operand : operands) {
        *this << "   ";
        print(operand);
      }
    }
    
    *this << "-> ";
    print(node);

    if (auto V = dyn_cast<ValueBase>(node)) {    
      auto users = map<SmallVector<const SILInstruction*,4>>(V->getUses(),
                                                       [](Operand *o) {
                                                         return o->getUser();
                                                       });
      std::sort(users.begin(), users.end(), sortByID);
      for (auto &user : users) {
        *this << "   ";
        print(user);
      }
    }
  }

  void printDebugInfoExpression(const SILDebugInfoExpression &DIExpr) {
    assert(DIExpr && "DIExpression empty?");
    *this << ", expr ";
    bool IsFirst = true;
    for (const auto &Operand : DIExpr.operands()) {
      if (IsFirst)
        IsFirst = false;
      else
        *this << ":";

      // Print the operator
      SILDIExprOperator Op = Operand.getOperator();
      assert(Op != SILDIExprOperator::INVALID &&
             "Invalid SILDIExprOperator kind");
      *this << SILDIExprInfo::get(Op)->OpText;

      // Print arguments
      for (const auto &Arg : Operand.args()) {
        *this << ":";
        switch (Arg.getKind()) {
        case SILDIExprElement::OperatorKind:
          llvm_unreachable("Cannot use operator as argument");
          break;
        case SILDIExprElement::DeclKind: {
          const Decl *D = Arg.getAsDecl();
          // FIXME: Can we generalize this special handling for VarDecl
          // to other kinds of Decl?
          if (const auto *VD = dyn_cast<VarDecl>(D)) {
            *this << "#";
            printFullContext(VD->getDeclContext(), PrintState.OS);
            *this << VD->getName().get();
          } else
            D->print(PrintState.OS, PrintState.ASTOptions);
          break;
        }
        case SILDIExprElement::ConstIntKind: {
          uint64_t V = *Arg.getAsConstInt();
          if (Op == SILDIExprOperator::ConstSInt)
            *this << static_cast<int64_t>(V);
          else
            *this << V;
          break;
        }
        }
      }
    }
  }

  void printDebugVar(Optional<SILDebugVariable> Var,
                     const SourceManager *SM = nullptr) {
    if (!Var)
      return;

    if (!Var->Name.empty()) {
      if (Var->Constant)
        *this << ", let";
      else
        *this << ", var";

      if ((Var->Loc || Var->Scope) && SM) {
        *this << ", (name \"" << Var->Name << '"';
        if (Var->Loc)
          printDebugLocRef(*Var->Loc, *SM);
        if (Var->Scope)
          printDebugScopeRef(Var->Scope, *SM);
        *this << ")";
      } else
        *this << ", name \"" << Var->Name << '"';

      if (Var->ArgNo)
        *this << ", argno " << Var->ArgNo;
      if (Var->Implicit)
        *this << ", implicit";
      if (Var->Type) {
        *this << ", type ";
        Var->Type->print(PrintState.OS, PrintState.ASTOptions);
      }
    }
    // Although it's rare in real-world use cases, but during testing,
    // sometimes we want to print out di-expression, even the debug
    // variable name is empty.
    if (Var->DIExpr)
      printDebugInfoExpression(Var->DIExpr);
  }

  void visitAllocStackInst(AllocStackInst *AVI) {
    if (AVI->hasDynamicLifetime())
      *this << "[dynamic_lifetime] ";
    if (AVI->isLexical())
      *this << "[lexical] ";
    if (AVI->getUsesMoveableValueDebugInfo() && !AVI->getType().isMoveOnly())
      *this << "[moveable_value_debuginfo] ";
    *this << AVI->getElementType();
    printDebugVar(AVI->getVarInfo(),
                  &AVI->getModule().getASTContext().SourceMgr);
  }
  void visitAllocPackInst(AllocPackInst *API) {
    *this << API->getType().getObjectType();
  }
  void visitAllocPackMetadataInst(AllocPackMetadataInst *APMI) {
    *this << APMI->getType().getObjectType();
  }

  void printAllocRefInstBase(AllocRefInstBase *ARI) {
    if (ARI->isObjC())
      *this << "[objc] ";
    if (ARI->canAllocOnStack())
      *this << "[stack] ";
    auto Types = ARI->getTailAllocatedTypes();
    auto Counts = ARI->getTailAllocatedCounts();
    for (unsigned Idx = 0, NumTypes = Types.size(); Idx < NumTypes; ++Idx) {
      *this << "[tail_elems " << Types[Idx] << " * "
            << getIDAndType(Counts[Idx].get()) << "] ";
    }
  }

  void visitAllocRefInst(AllocRefInst *ARI) {
    printAllocRefInstBase(ARI);
    *this << ARI->getType();
  }

  void visitAllocRefDynamicInst(AllocRefDynamicInst *ARDI) {
    printAllocRefInstBase(ARDI);
    *this << getIDAndType(ARDI->getMetatypeOperand());
    *this << ", " << ARDI->getType();
  }

  void visitAllocBoxInst(AllocBoxInst *ABI) {
    if (ABI->hasDynamicLifetime())
      *this << "[dynamic_lifetime] ";
    
    if (ABI->emitReflectionMetadata()) {
      *this << "[reflection] ";
    }

    if (ABI->hasPointerEscape()) {
      *this << "[pointer_escape] ";
    }

    if (ABI->getUsesMoveableValueDebugInfo() &&
        !ABI->getAddressType().isMoveOnly()) {
      *this << "[moveable_value_debuginfo] ";
    }

    *this << ABI->getType();
    printDebugVar(ABI->getVarInfo(),
                  &ABI->getModule().getASTContext().SourceMgr);
  }

  void printSubstitutions(SubstitutionMap Subs,
                          GenericSignature Sig = GenericSignature()) {
    if (!Subs.hasAnySubstitutableParams()) return;

    // FIXME: This is a hack to cope with cases where the substitution map uses
    // a generic signature that's close-to-but-not-the-same-as expected.
    auto genericSig = Sig ? Sig : Subs.getGenericSignature();

    *this << '<';
    bool first = true;
    for (auto gp : genericSig.getGenericParams()) {
      if (first) first = false;
      else *this << ", ";

      *this << Type(gp).subst(Subs);
    }
    *this << '>';
  }

  template <class Inst>
  void visitApplyInstBase(Inst *AI) {
    *this << Ctx.getID(AI->getCallee());
    printSubstitutions(AI->getSubstitutionMap(),
                       AI->getOrigCalleeType()->getInvocationGenericSignature());
    *this << '(';
    llvm::interleave(
        AI->getArguments(),
        [&](const SILValue &arg) { *this << Ctx.getID(arg); },
        [&] { *this << ", "; });
    *this << ") : ";
    if (auto callee = AI->getCallee())
      *this << callee->getType();
    else
      *this << "<<NULL CALLEE>>";
  }

  void visitApplyInst(ApplyInst *AI) {
    if (AI->isNonThrowing())
      *this << "[nothrow] ";
    if (AI->isNonAsync())
      *this << "[noasync] ";
    visitApplyInstBase(AI);
  }

  void visitBeginApplyInst(BeginApplyInst *AI) {
    if (AI->isNonThrowing())
      *this << "[nothrow] ";
    visitApplyInstBase(AI);
  }

  void visitTryApplyInst(TryApplyInst *AI) {
    if (AI->isNonAsync())
      *this << "[noasync] ";
    visitApplyInstBase(AI);
    *this << ", normal " << Ctx.getID(AI->getNormalBB());
    *this << ", error " << Ctx.getID(AI->getErrorBB());
  }

  void visitPartialApplyInst(PartialApplyInst *CI) {
    switch (CI->getFunctionType()->getCalleeConvention()) {
    case ParameterConvention::Direct_Owned:
      // Default; do nothing.
      break;
    case ParameterConvention::Direct_Guaranteed:
      *this << "[callee_guaranteed] ";
      break;
    
    // Should not apply to callees.
    case ParameterConvention::Direct_Unowned:
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Inout:
      llvm_unreachable("unexpected callee convention!");
    }
    if (CI->isOnStack())
      *this << "[on_stack] ";
    visitApplyInstBase(CI);
  }

  void visitAbortApplyInst(AbortApplyInst *AI) {
    *this << Ctx.getID(AI->getOperand());
  }

  void visitEndApplyInst(EndApplyInst *AI) {
    *this << Ctx.getID(AI->getOperand());
  }

  void visitFunctionRefInst(FunctionRefInst *FRI) {
    FRI->getReferencedFunction()->printName(PrintState.OS);
    *this << " : " << FRI->getType();
  }
  void visitDynamicFunctionRefInst(DynamicFunctionRefInst *FRI) {
    FRI->getInitiallyReferencedFunction()->printName(PrintState.OS);
    *this << " : " << FRI->getType();
  }
  void
  visitPreviousDynamicFunctionRefInst(PreviousDynamicFunctionRefInst *FRI) {
    FRI->getInitiallyReferencedFunction()->printName(PrintState.OS);
    *this << " : " << FRI->getType();
  }

  void visitBuiltinInst(BuiltinInst *BI) {
    *this << QuotedString(BI->getName().str());
    printSubstitutions(BI->getSubstitutions());
    *this << "(";
    
    llvm::interleave(BI->getArguments(), [&](SILValue v) {
      *this << getIDAndType(v);
    }, [&]{
      *this << ", ";
    });
    
    *this << ") : ";
    *this << BI->getType();
  }
  
  void visitAllocGlobalInst(AllocGlobalInst *AGI) {
    if (AGI->getReferencedGlobal()) {
      AGI->getReferencedGlobal()->printName(PrintState.OS);
    } else {
      *this << "<<placeholder>>";
    }
  }
  
  void visitGlobalAddrInst(GlobalAddrInst *GAI) {
    if (GAI->getReferencedGlobal()) {
      GAI->getReferencedGlobal()->printName(PrintState.OS);
    } else {
      *this << "<<placeholder>>";
    }
    *this << " : " << GAI->getType();
  }

  void visitGlobalValueInst(GlobalValueInst *GVI) {
    GVI->getReferencedGlobal()->printName(PrintState.OS);
    *this << " : " << GVI->getType();
  }

  void visitBaseAddrForOffsetInst(BaseAddrForOffsetInst *BAI) {
    *this << BAI->getType();
  }

  void visitIntegerLiteralInst(IntegerLiteralInst *ILI) {
    const auto &lit = ILI->getValue();
    *this << ILI->getType() << ", " << lit;
  }
  void visitFloatLiteralInst(FloatLiteralInst *FLI) {
    llvm::SmallString<12> hex;
    llvm::SmallString<12> decimal;
    FLI->getBits().toString(hex, 16, /*Signed*/ false);
    FLI->getValue().toString(decimal);
    *this << FLI->getType()
          << (llvm::Twine(", 0x") + hex + " // " + decimal).str();
  }
  static StringRef getStringEncodingName(StringLiteralInst::Encoding kind) {
    switch (kind) {
    case StringLiteralInst::Encoding::Bytes: return "bytes ";
    case StringLiteralInst::Encoding::UTF8: return "utf8 ";
    case StringLiteralInst::Encoding::ObjCSelector: return "objc_selector ";
    }
    llvm_unreachable("bad string literal encoding");
  }

  void visitStringLiteralInst(StringLiteralInst *SLI) {
    *this << getStringEncodingName(SLI->getEncoding());

    if (SLI->getEncoding() != StringLiteralInst::Encoding::Bytes) {
      // FIXME: this isn't correct: this doesn't properly handle translating
      // UTF16 into UTF8, and the SIL parser always parses as UTF8.
      *this << QuotedString(SLI->getValue());
      return;
    }

    // "Bytes" are always output in a hexadecimal form.
    *this << '"' << llvm::toHex(SLI->getValue()) << '"';
  }

  void printLoadOwnershipQualifier(LoadOwnershipQualifier Qualifier) {
    switch (Qualifier) {
    case LoadOwnershipQualifier::Unqualified:
      return;
    case LoadOwnershipQualifier::Take:
      *this << "[take] ";
      return;
    case LoadOwnershipQualifier::Copy:
      *this << "[copy] ";
      return;
    case LoadOwnershipQualifier::Trivial:
      *this << "[trivial] ";
      return;
    }
  }

  void visitLoadInst(LoadInst *LI) {
    printLoadOwnershipQualifier(LI->getOwnershipQualifier());
    *this << getIDAndType(LI->getOperand());
  }

  void visitLoadBorrowInst(LoadBorrowInst *LBI) {
    *this << getIDAndType(LBI->getOperand());
  }

  void visitBeginBorrowInst(BeginBorrowInst *BBI) {
    if (BBI->isLexical()) {
      *this << "[lexical] ";
    }
    if (BBI->hasPointerEscape()) {
      *this << "[pointer_escape] ";
    }
    *this << getIDAndType(BBI->getOperand());
  }

  void printStoreOwnershipQualifier(StoreOwnershipQualifier Qualifier) {
    switch (Qualifier) {
    case StoreOwnershipQualifier::Unqualified:
      return;
    case StoreOwnershipQualifier::Init:
      *this << "[init] ";
      return;
    case StoreOwnershipQualifier::Assign:
      *this << "[assign] ";
      return;
    case StoreOwnershipQualifier::Trivial:
      *this << "[trivial] ";
      return;
    }
  }

  void printAssignOwnershipQualifier(AssignOwnershipQualifier Qualifier) {
    switch (Qualifier) {
    case AssignOwnershipQualifier::Unknown:
      return;
    case AssignOwnershipQualifier::Init:
      *this << "[init] ";
      return;
    case AssignOwnershipQualifier::Reassign:
      *this << "[reassign] ";
      return;
    case AssignOwnershipQualifier::Reinit:
      *this << "[reinit] ";
      return;
    }
  }

  void printForwardingOwnershipKind(ForwardingInstruction *inst, SILValue op) {
    if (!op)
      return;

    if (inst->getForwardingOwnershipKind() != op->getOwnershipKind()) {
      *this << ", forwarding: @" << inst->getForwardingOwnershipKind();
    }
  }

  void visitStoreInst(StoreInst *SI) {
    *this << Ctx.getID(SI->getSrc()) << " to ";
    printStoreOwnershipQualifier(SI->getOwnershipQualifier());
    *this << getIDAndType(SI->getDest());
  }

  void visitStoreBorrowInst(StoreBorrowInst *SI) {
    *this << Ctx.getID(SI->getSrc()) << " to ";
    *this << getIDAndType(SI->getDest());
  }

  void visitEndBorrowInst(EndBorrowInst *EBI) {
    *this << getIDAndType(EBI->getOperand());
  }

  void visitAssignInst(AssignInst *AI) {
    *this << Ctx.getID(AI->getSrc()) << " to ";
    printAssignOwnershipQualifier(AI->getOwnershipQualifier());
    *this << getIDAndType(AI->getDest());
  }

  void visitAssignByWrapperInst(AssignByWrapperInst *AI) {
    *this << getIDAndType(AI->getSrc()) << " to ";
    switch (AI->getMode()) {
    case AssignByWrapperInst::Unknown:
      break;
    case AssignByWrapperInst::Initialization:
      *this << "[init] ";
      break;
    case AssignByWrapperInst::Assign:
      *this << "[assign] ";
      break;
    case AssignByWrapperInst::AssignWrappedValue:
      *this << "[assign_wrapped_value] ";
      break;
    }

    *this << getIDAndType(AI->getDest())
          << ", init " << getIDAndType(AI->getInitializer())
          << ", set " << getIDAndType(AI->getSetter());
  }

  void visitAssignOrInitInst(AssignOrInitInst *AI) {
    switch (AI->getMode()) {
    case AssignOrInitInst::Unknown:
      break;
    case AssignOrInitInst::Init:
      *this << "[init] ";
      break;
    case AssignOrInitInst::Set:
      *this << "[set] ";
      break;
    }

    // Print all of the properties that have been previously initialized.
    for (unsigned i = 0, n = AI->getNumInitializedProperties(); i != n; ++i) {
      if (AI->isPropertyAlreadyInitialized(i)) {
        *this << "[assign=" << i << "] ";
      }
    }

    *this << getIDAndType(AI->getSrc());
    *this << ", init " << getIDAndType(AI->getInitializer())
          << ", set " << getIDAndType(AI->getSetter());
  }

  void visitMarkUninitializedInst(MarkUninitializedInst *MU) {
    switch (MU->getMarkUninitializedKind()) {
    case MarkUninitializedInst::Var: *this << "[var] "; break;
    case MarkUninitializedInst::RootSelf:  *this << "[rootself] "; break;
    case MarkUninitializedInst::CrossModuleRootSelf:
      *this << "[crossmodulerootself] ";
      break;
    case MarkUninitializedInst::DerivedSelf:  *this << "[derivedself] "; break;
    case MarkUninitializedInst::DerivedSelfOnly:
      *this << "[derivedselfonly] ";
      break;
    case MarkUninitializedInst::DelegatingSelf: *this << "[delegatingself] ";break;
    case MarkUninitializedInst::DelegatingSelfAllocated:
      *this << "[delegatingselfallocated] ";
      break;
    case MarkUninitializedInst::Out:
      *this << "[out] ";
      break;
    }
    *this << getIDAndType(MU->getOperand());
    printForwardingOwnershipKind(MU, MU->getOperand());
  }

  void visitMarkFunctionEscapeInst(MarkFunctionEscapeInst *MFE) {
    llvm::interleave(
        MFE->getElements(), [&](SILValue Var) { *this << getIDAndType(Var); },
        [&] { *this << ", "; });
  }

  void visitDebugValueInst(DebugValueInst *DVI) {
    if (DVI->poisonRefs())
      *this << "[poison] ";
    if (DVI->getUsesMoveableValueDebugInfo() &&
        !DVI->getOperand()->getType().isMoveOnly())
      *this << "[moveable_value_debuginfo] ";
    if (DVI->hasTrace())
      *this << "[trace] ";
    *this << getIDAndType(DVI->getOperand());
    printDebugVar(DVI->getVarInfo(),
                  &DVI->getModule().getASTContext().SourceMgr);
  }

  void visitDebugStepInst(DebugStepInst *dsi) {
    // nothing to print other than the instruction name
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  void visitLoad##Name##Inst(Load##Name##Inst *LI) { \
    if (LI->isTake()) \
      *this << "[take] "; \
    *this << getIDAndType(LI->getOperand()); \
  } \
  void visitStore##Name##Inst(Store##Name##Inst *SI) { \
    *this << Ctx.getID(SI->getSrc()) << " to "; \
    if (SI->isInitializationOfDest()) \
      *this << "[init] "; \
    *this << getIDAndType(SI->getDest()); \
  }
#include "swift/AST/ReferenceStorage.def"

  void visitCopyAddrInst(CopyAddrInst *CI) {
    if (CI->isTakeOfSrc())
      *this << "[take] ";
    *this << Ctx.getID(CI->getSrc()) << " to ";
    if (CI->isInitializationOfDest())
      *this << "[init] ";
    *this << getIDAndType(CI->getDest());
  }

  void visitExplicitCopyAddrInst(ExplicitCopyAddrInst *CI) {
    if (CI->isTakeOfSrc())
      *this << "[take] ";
    *this << Ctx.getID(CI->getSrc()) << " to ";
    if (CI->isInitializationOfDest())
      *this << "[init] ";
    *this << getIDAndType(CI->getDest());
  }

  void visitMarkUnresolvedMoveAddrInst(MarkUnresolvedMoveAddrInst *CI) {
    *this << Ctx.getID(CI->getSrc()) << " to ";
    *this << getIDAndType(CI->getDest());
  }

  void visitBindMemoryInst(BindMemoryInst *BI) {
    *this << getIDAndType(BI->getBase()) << ", ";
    *this << getIDAndType(BI->getIndex()) << " to ";
    *this << BI->getBoundType();
  }

  void visitRebindMemoryInst(RebindMemoryInst *BI) {
    *this << getIDAndType(BI->getBase()) << " to ";
    *this << getIDAndType(BI->getInToken());
  }

  void visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *CI) {
    *this << getIDAndType(CI->getOperand()) << " to " << CI->getTargetFormalType();
    printForwardingOwnershipKind(CI, CI->getOperand());
  }
  
  void visitCheckedCastBranchInst(CheckedCastBranchInst *CI) {
    if (CI->isExact())
      *this << "[exact] ";
    *this << getIDAndType(CI->getOperand()) << " to " << CI->getTargetFormalType()
          << ", " << Ctx.getID(CI->getSuccessBB()) << ", "
          << Ctx.getID(CI->getFailureBB());
    if (CI->getTrueBBCount())
      *this << " !true_count(" << CI->getTrueBBCount().getValue() << ")";
    if (CI->getFalseBBCount())
      *this << " !false_count(" << CI->getFalseBBCount().getValue() << ")";
    printForwardingOwnershipKind(CI, CI->getOperand());
  }

  void visitUnconditionalCheckedCastAddrInst(UnconditionalCheckedCastAddrInst *CI) {
    *this << CI->getSourceFormalType() << " in " << getIDAndType(CI->getSrc())
          << " to " << CI->getTargetFormalType() << " in "
          << getIDAndType(CI->getDest());
  }

  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CI) {
    *this << getCastConsumptionKindName(CI->getConsumptionKind()) << ' '
          << CI->getSourceFormalType() << " in " << getIDAndType(CI->getSrc())
          << " to " << CI->getTargetFormalType() << " in "
          << getIDAndType(CI->getDest()) << ", "
          << Ctx.getID(CI->getSuccessBB()) << ", "
          << Ctx.getID(CI->getFailureBB());
    if (CI->getTrueBBCount())
      *this << " !true_count(" << CI->getTrueBBCount().getValue() << ")";
    if (CI->getFalseBBCount())
      *this << " !false_count(" << CI->getFalseBBCount().getValue() << ")";
  }

  void printUncheckedConversionInst(ConversionOperation CI, SILValue operand) {
    *this << getIDAndType(operand) << " to " << CI->getType();
    if (auto *ofci = dyn_cast<OwnershipForwardingSingleValueInstruction>(*CI)) {
      printForwardingOwnershipKind(ofci, ofci->getOperand(0));
    }
  }

  void visitUncheckedOwnershipConversionInst(
      UncheckedOwnershipConversionInst *UOCI) {
    *this << getIDAndType(UOCI->getOperand()) << ", "
          << "@" << UOCI->getOperand()->getOwnershipKind() << " to "
          << "@" << UOCI->getConversionOwnershipKind();
  }

  void visitConvertFunctionInst(ConvertFunctionInst *CI) {
    *this << getIDAndType(CI->getOperand()) << " to ";
    if (CI->withoutActuallyEscaping())
      *this << "[without_actually_escaping] ";
    *this << CI->getType();
    printForwardingOwnershipKind(CI, CI->getOperand());
  }
  void visitConvertEscapeToNoEscapeInst(ConvertEscapeToNoEscapeInst *CI) {
    *this << (CI->isLifetimeGuaranteed() ? "" : "[not_guaranteed] ")
          << getIDAndType(CI->getOperand()) << " to " << CI->getType();
  }
  void visitUpcastInst(UpcastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitAddressToPointerInst(AddressToPointerInst *CI) {
    *this << (CI->needsStackProtection() ? "[stack_protection] " : "");
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitPointerToAddressInst(PointerToAddressInst *CI) {
    *this << getIDAndType(CI->getOperand()) << " to ";
    if (CI->isStrict())
      *this << "[strict] ";
    if (CI->isInvariant())
      *this << "[invariant] ";
    if (CI->alignment())
      *this << "[align=" << CI->alignment()->value() << "] ";
    *this << CI->getType();
  }
  void visitUncheckedRefCastInst(UncheckedRefCastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *CI) {
    *this << ' ' << CI->getSourceFormalType() << " in " << getIDAndType(CI->getSrc())
          << " to " << CI->getTargetFormalType() << " in "
          << getIDAndType(CI->getDest());
  }
  void visitUncheckedAddrCastInst(UncheckedAddrCastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitUncheckedValueCastInst(UncheckedValueCastInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitRefToRawPointerInst(RefToRawPointerInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitRawPointerToRefInst(RawPointerToRefInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }

#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  void visitRefTo##Name##Inst(RefTo##Name##Inst *CI) {                         \
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());   \
  }                                                                            \
  void visit##Name##ToRefInst(Name##ToRefInst *CI) {                           \
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());   \
  }
#include "swift/AST/ReferenceStorage.def"
  void visitThinToThickFunctionInst(ThinToThickFunctionInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitObjCMetatypeToObjectInst(ObjCMetatypeToObjectInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitObjCExistentialMetatypeToObjectInst(
                                      ObjCExistentialMetatypeToObjectInst *CI) {
    printUncheckedConversionInst(ConversionOperation(CI), CI->getOperand());
  }
  void visitObjCProtocolInst(ObjCProtocolInst *CI) {
    *this << "#" << CI->getProtocol()->getName() << " : " << CI->getType();
  }
  
  void visitRefToBridgeObjectInst(RefToBridgeObjectInst *I) {
    *this << getIDAndType(I->getOperand(0)) << ", "
          << getIDAndType(I->getBitsOperand());
    printForwardingOwnershipKind(I, I->getOperand(0));
  }

  void visitBridgeObjectToRefInst(BridgeObjectToRefInst *I) {
    printUncheckedConversionInst(ConversionOperation(I), I->getOperand());
    printForwardingOwnershipKind(I, I->getOperand());
  }
  void visitBridgeObjectToWordInst(BridgeObjectToWordInst *I) {
    printUncheckedConversionInst(ConversionOperation(I), I->getOperand());
  }

  void visitCopyValueInst(CopyValueInst *I) {
    *this << getIDAndType(I->getOperand());
  }

  void visitExplicitCopyValueInst(ExplicitCopyValueInst *I) {
    *this << getIDAndType(I->getOperand());
  }

  void visitMoveValueInst(MoveValueInst *I) {
    if (I->getAllowDiagnostics())
      *this << "[allows_diagnostics] ";
    if (I->isLexical())
      *this << "[lexical] ";
    if (I->hasPointerEscape())
      *this << "[pointer_escape] ";
    *this << getIDAndType(I->getOperand());
  }

  void visitDropDeinitInst(DropDeinitInst *I) {
    *this << getIDAndType(I->getOperand());
  }

  void visitMarkMustCheckInst(MarkMustCheckInst *I) {
    using CheckKind = MarkMustCheckInst::CheckKind;
    switch (I->getCheckKind()) {
    case CheckKind::Invalid:
      llvm_unreachable("Invalid?!");
    case CheckKind::ConsumableAndAssignable:
      *this << "[consumable_and_assignable] ";
      break;
    case CheckKind::NoConsumeOrAssign:
      *this << "[no_consume_or_assign] ";
      break;
    case CheckKind::AssignableButNotConsumable:
      *this << "[assignable_but_not_consumable] ";
      break;
    case CheckKind::InitableButNotConsumable:
      *this << "[initable_but_not_consumable] ";
      break;
    }
    *this << getIDAndType(I->getOperand());
  }

  void visitMarkUnresolvedReferenceBindingInst(
      MarkUnresolvedReferenceBindingInst *I) {
    using Kind = MarkUnresolvedReferenceBindingInst::Kind;
    switch (I->getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::InOut:
      *this << "[inout] ";
      break;
    }
    *this << getIDAndType(I->getOperand());
  }

  void visitCopyableToMoveOnlyWrapperValueInst(
      CopyableToMoveOnlyWrapperValueInst *I) {
    switch (I->getInitialKind()) {
    case CopyableToMoveOnlyWrapperValueInst::Owned:
      *this << "[owned] ";
      break;
    case CopyableToMoveOnlyWrapperValueInst::Guaranteed:
      *this << "[guaranteed] ";
      break;
    }
    *this << getIDAndType(I->getOperand());
  }

  void visitMoveOnlyWrapperToCopyableValueInst(
      MoveOnlyWrapperToCopyableValueInst *I) {
    switch (I->getInitialKind()) {
    case MoveOnlyWrapperToCopyableValueInst::Owned:
      *this << "[owned] ";
      break;
    case MoveOnlyWrapperToCopyableValueInst::Guaranteed:
      *this << "[guaranteed] ";
      break;
    }
    *this << getIDAndType(I->getOperand());
  }

#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  void visitStrongCopy##Name##ValueInst(StrongCopy##Name##ValueInst *I) {      \
    *this << getIDAndType(I->getOperand());                                    \
  }

#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  void visitStrongCopy##Name##ValueInst(StrongCopy##Name##ValueInst *I) {      \
    *this << getIDAndType(I->getOperand());                                    \
  }
#include "swift/AST/ReferenceStorage.def"

  void visitDestroyValueInst(DestroyValueInst *I) {
    if (I->poisonRefs())
      *this << "[poison] ";
    *this << getIDAndType(I->getOperand());
  }

  void visitStructInst(StructInst *SI) {
    *this << SI->getType() << " (";
    llvm::interleave(
        SI->getElements(), [&](const SILValue &V) { *this << getIDAndType(V); },
        [&] { *this << ", "; });
    *this << ')';
  }

  void visitObjectInst(ObjectInst *OI) {
    *this << OI->getType() << " (";
    llvm::interleave(
        OI->getBaseElements(),
        [&](const SILValue &V) { *this << getIDAndType(V); },
        [&] { *this << ", "; });
    if (!OI->getTailElements().empty()) {
      *this << ", [tail_elems] ";
      llvm::interleave(
          OI->getTailElements(),
          [&](const SILValue &V) { *this << getIDAndType(V); },
          [&] { *this << ", "; });
    }
    *this << ')';
  }

  void visitTupleInst(TupleInst *TI) {
    
    // Check to see if the type of the tuple can be inferred accurately from the
    // elements.
    bool SimpleType = true;
    for (auto &Elt : TI->getType().castTo<TupleType>()->getElements()) {
      if (Elt.hasName()) {
        SimpleType = false;
        break;
      }
    }
    
    // If the type is simple, just print the tuple elements.
    if (SimpleType) {
      *this << '(';
      llvm::interleave(
          TI->getElements(),
          [&](const SILValue &V) { *this << getIDAndType(V); },
          [&] { *this << ", "; });
      *this << ')';
    } else {
      // Otherwise, print the type, then each value.
      *this << TI->getType() << " (";
      llvm::interleave(
          TI->getElements(), [&](const SILValue &V) { *this << Ctx.getID(V); },
          [&] { *this << ", "; });
      *this << ')';
    }
  }
  
  void visitEnumInst(EnumInst *UI) {
    *this << UI->getType() << ", "
          << SILDeclRef(UI->getElement(), SILDeclRef::Kind::EnumElement);
    if (UI->hasOperand()) {
      *this << ", " << getIDAndType(UI->getOperand());
      printForwardingOwnershipKind(UI, UI->getOperand());
    }
  }

  void visitInitEnumDataAddrInst(InitEnumDataAddrInst *UDAI) {
    *this << getIDAndType(UDAI->getOperand()) << ", "
          << SILDeclRef(UDAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitUncheckedEnumDataInst(UncheckedEnumDataInst *UDAI) {
    *this << getIDAndType(UDAI->getOperand()) << ", "
          << SILDeclRef(UDAI->getElement(), SILDeclRef::Kind::EnumElement);
    printForwardingOwnershipKind(UDAI, UDAI->getOperand());
  }
  
  void visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *UDAI) {
    *this << getIDAndType(UDAI->getOperand()) << ", "
          << SILDeclRef(UDAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitInjectEnumAddrInst(InjectEnumAddrInst *IUAI) {
    *this << getIDAndType(IUAI->getOperand()) << ", "
          << SILDeclRef(IUAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitTupleExtractInst(TupleExtractInst *EI) {
    *this << getIDAndType(EI->getOperand()) << ", " << EI->getFieldIndex();
    printForwardingOwnershipKind(EI, EI->getOperand());
  }

  void visitTupleElementAddrInst(TupleElementAddrInst *EI) {
    *this << getIDAndType(EI->getOperand()) << ", " << EI->getFieldIndex();
  }
  void visitStructExtractInst(StructExtractInst *EI) {
    *this << getIDAndType(EI->getOperand()) << ", #";
    printFullContext(EI->getField()->getDeclContext(), PrintState.OS);
    *this << EI->getField()->getName().get();
    printForwardingOwnershipKind(EI, EI->getOperand());
  }
  void visitStructElementAddrInst(StructElementAddrInst *EI) {
    *this << getIDAndType(EI->getOperand()) << ", #";
    printFullContext(EI->getField()->getDeclContext(), PrintState.OS);
    *this << EI->getField()->getName().get();
  }
  void visitRefElementAddrInst(RefElementAddrInst *EI) {
    *this << (EI->isImmutable() ? "[immutable] " : "")
          << getIDAndType(EI->getOperand()) << ", #";
    printFullContext(EI->getField()->getDeclContext(), PrintState.OS);
    *this << EI->getField()->getName().get();
  }

  void visitRefTailAddrInst(RefTailAddrInst *RTAI) {
    *this << (RTAI->isImmutable() ? "[immutable] " : "")
          << getIDAndType(RTAI->getOperand()) << ", " << RTAI->getTailType();
  }

  void visitDestructureStructInst(DestructureStructInst *DSI) {
    *this << getIDAndType(DSI->getOperand());
    printForwardingOwnershipKind(DSI, DSI->getOperand());
  }

  void visitDestructureTupleInst(DestructureTupleInst *DTI) {
    *this << getIDAndType(DTI->getOperand());
    printForwardingOwnershipKind(DTI, DTI->getOperand());
  }

  void printMethodInst(MethodInst *I, SILValue Operand) {
    *this << getIDAndType(Operand) << ", " << I->getMember();
  }
  
  void visitClassMethodInst(ClassMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand());
    *this << " : " << AMI->getMember().getDecl()->getInterfaceType();
    *this << ", ";
    *this << AMI->getType();
  }
  void visitSuperMethodInst(SuperMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand());
    *this << " : " << AMI->getMember().getDecl()->getInterfaceType();
    *this << ", ";
    *this << AMI->getType();
  }
  void visitObjCMethodInst(ObjCMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand());
    *this << " : " << AMI->getMember().getDecl()->getInterfaceType();
    *this << ", ";
    *this << AMI->getType();
  }
  void visitObjCSuperMethodInst(ObjCSuperMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand());
    *this << " : " << AMI->getMember().getDecl()->getInterfaceType();
    *this << ", ";
    *this << AMI->getType();
  }
  void visitWitnessMethodInst(WitnessMethodInst *WMI) {
    PrintOptions QualifiedSILTypeOptions =
        PrintOptions::printQualifiedSILType();
    QualifiedSILTypeOptions.CurrentModule = WMI->getModule().getSwiftModule();
    *this << "$" << WMI->getLookupType() << ", " << WMI->getMember() << " : ";
    WMI->getMember().getDecl()->getInterfaceType().print(
        PrintState.OS, QualifiedSILTypeOptions);
    if (!WMI->getTypeDependentOperands().empty()) {
      *this << ", ";
      *this << getIDAndType(WMI->getTypeDependentOperands()[0].get());
    }
    *this << " : " << WMI->getType();
    printConformances({WMI->getConformance()});
  }
  void visitOpenExistentialAddrInst(OpenExistentialAddrInst *OI) {
    if (OI->getAccessKind() == OpenedExistentialAccess::Immutable)
      *this << "immutable_access ";
    else
      *this << "mutable_access ";
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
  }
  void visitOpenExistentialRefInst(OpenExistentialRefInst *OI) {
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
    printForwardingOwnershipKind(OI, OI->getOperand());
  }
  void visitOpenExistentialMetatypeInst(OpenExistentialMetatypeInst *OI) {
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
  }
  void visitOpenExistentialBoxInst(OpenExistentialBoxInst *OI) {
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
  }
  void visitOpenExistentialBoxValueInst(OpenExistentialBoxValueInst *OI) {
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
    printForwardingOwnershipKind(OI, OI->getOperand());
  }
  void visitOpenExistentialValueInst(OpenExistentialValueInst *OI) {
    *this << getIDAndType(OI->getOperand()) << " to " << OI->getType();
    printForwardingOwnershipKind(OI, OI->getOperand());
  }
  void visitInitExistentialAddrInst(InitExistentialAddrInst *AEI) {
    *this << getIDAndType(AEI->getOperand()) << ", $"
          << AEI->getFormalConcreteType();
    printConformances(AEI->getConformances());
  }
  void visitInitExistentialValueInst(InitExistentialValueInst *AEI) {
    *this << getIDAndType(AEI->getOperand()) << ", $"
          << AEI->getFormalConcreteType() << ", " << AEI->getType();
    printConformances(AEI->getConformances());
  }
  void visitInitExistentialRefInst(InitExistentialRefInst *AEI) {
    *this << getIDAndType(AEI->getOperand()) << " : $"
          << AEI->getFormalConcreteType() << ", " << AEI->getType();
    printConformances(AEI->getConformances());
    printForwardingOwnershipKind(AEI, AEI->getOperand());
  }
  void visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *EMI) {
    *this << getIDAndType(EMI->getOperand()) << ", " << EMI->getType();
    printConformances(EMI->getConformances());
  }
  void visitAllocExistentialBoxInst(AllocExistentialBoxInst *AEBI) {
    *this << AEBI->getExistentialType() << ", $"
          << AEBI->getFormalConcreteType();
    printConformances(AEBI->getConformances());
  }
  void visitDeinitExistentialAddrInst(DeinitExistentialAddrInst *DEI) {
    *this << getIDAndType(DEI->getOperand());
  }
  void visitDeinitExistentialValueInst(DeinitExistentialValueInst *DEI) {
    *this << getIDAndType(DEI->getOperand());
  }
  void visitDeallocExistentialBoxInst(DeallocExistentialBoxInst *DEI) {
    *this << getIDAndType(DEI->getOperand()) << ", $" << DEI->getConcreteType();
  }
  void visitPackLengthInst(PackLengthInst *PLI) {
    *this << "$" << PLI->getPackType();
  }
  void visitDynamicPackIndexInst(DynamicPackIndexInst *DPII) {
    *this << Ctx.getID(DPII->getOperand()) << " of $"
          << DPII->getIndexedPackType();
  }
  void visitPackPackIndexInst(PackPackIndexInst *PPII) {
    *this << PPII->getComponentStartIndex() << ", "
          << Ctx.getID(PPII->getOperand()) << " of $"
          << PPII->getIndexedPackType();
  }
  void visitScalarPackIndexInst(ScalarPackIndexInst *SPII) {
    *this << SPII->getComponentIndex() << " of $"
          << SPII->getIndexedPackType();
  }
  void visitOpenPackElementInst(OpenPackElementInst *OPEI) {
    auto env = OPEI->getOpenedGenericEnvironment();
    auto subs = env->getPackElementContextSubstitutions();
    *this << Ctx.getID(OPEI->getIndexOperand())
          << " of " << subs.getGenericSignature()
          << " at ";
    printSubstitutions(subs);
    // The shape class in the opened environment is a canonical interface
    // type, which won't resolve in the generic signature we just printed.
    // Map it back to the sugared generic parameter.
    auto sugaredShapeClass =
      subs.getGenericSignature()->getSugaredType(
        env->getOpenedElementShapeClass());
    *this << ", shape $" << sugaredShapeClass
          << ", uuid \"" << env->getOpenedElementUUID() << "\"";
  }
  void visitPackElementGetInst(PackElementGetInst *I) {
    *this << Ctx.getID(I->getIndex()) << " of "
          << getIDAndType(I->getPack()) << " as "
          << I->getElementType();
  }
  void visitPackElementSetInst(PackElementSetInst *I) {
    *this << getIDAndType(I->getValue()) << " into "
          << Ctx.getID(I->getIndex()) << " of "
          << getIDAndType(I->getPack());
  }
  void visitTuplePackElementAddrInst(TuplePackElementAddrInst *I) {
    *this << Ctx.getID(I->getIndex()) << " of "
          << getIDAndType(I->getTuple()) << " as "
          << I->getElementType();
  }
  void visitProjectBlockStorageInst(ProjectBlockStorageInst *PBSI) {
    *this << getIDAndType(PBSI->getOperand());
  }
  void visitInitBlockStorageHeaderInst(InitBlockStorageHeaderInst *IBSHI) {
    *this << getIDAndType(IBSHI->getBlockStorage()) << ", invoke "
          << Ctx.getID(IBSHI->getInvokeFunction());
    printSubstitutions(IBSHI->getSubstitutions());
    *this << " : " << IBSHI->getInvokeFunction()->getType()
          << ", type " << IBSHI->getType();
  }
  void visitValueMetatypeInst(ValueMetatypeInst *MI) {
    *this << MI->getType() << ", " << getIDAndType(MI->getOperand());
  }
  void visitExistentialMetatypeInst(ExistentialMetatypeInst *MI) {
    *this << MI->getType() << ", " << getIDAndType(MI->getOperand());
  }
  void visitMetatypeInst(MetatypeInst *MI) { *this << MI->getType(); }

  void visitFixLifetimeInst(FixLifetimeInst *RI) {
    *this << getIDAndType(RI->getOperand());
  }

  void visitEndLifetimeInst(EndLifetimeInst *ELI) {
    *this << getIDAndType(ELI->getOperand());
  }
  void visitValueToBridgeObjectInst(ValueToBridgeObjectInst *VBOI) {
    *this << getIDAndType(VBOI->getOperand());
  }
  void visitClassifyBridgeObjectInst(ClassifyBridgeObjectInst *CBOI) {
    *this << getIDAndType(CBOI->getOperand());
  }
  void visitMarkDependenceInst(MarkDependenceInst *MDI) {
    *this << getIDAndType(MDI->getValue()) << " on "
          << getIDAndType(MDI->getBase());
    printForwardingOwnershipKind(MDI, MDI->getValue());
  }
  void visitCopyBlockInst(CopyBlockInst *RI) {
    *this << getIDAndType(RI->getOperand());
  }
  void visitCopyBlockWithoutEscapingInst(CopyBlockWithoutEscapingInst *RI) {
    *this << getIDAndType(RI->getBlock()) << " withoutEscaping "
          << getIDAndType(RI->getClosure());
  }
  void visitRefCountingInst(RefCountingInst *I) {
    if (I->isNonAtomic())
      *this << "[nonatomic] ";
    *this << getIDAndType(I->getOperand(0));
  }
  void visitIsUniqueInst(IsUniqueInst *CUI) {
    *this << getIDAndType(CUI->getOperand());
  }
  void visitBeginCOWMutationInst(BeginCOWMutationInst *BCMI) {
    if (BCMI->isNative())
      *this << "[native] ";
    *this << getIDAndType(BCMI->getOperand());
  }
  void visitEndCOWMutationInst(EndCOWMutationInst *ECMI) {
    if (ECMI->doKeepUnique())
      *this << "[keep_unique] ";
    *this << getIDAndType(ECMI->getOperand());
  }
  void visitIsEscapingClosureInst(IsEscapingClosureInst *CUI) {
    if (CUI->getVerificationType())
      *this << "[objc] ";
    *this << getIDAndType(CUI->getOperand());
  }
  void visitDeallocStackInst(DeallocStackInst *DI) {
    *this << getIDAndType(DI->getOperand());
  }
  void visitDeallocPackInst(DeallocPackInst *DI) {
    *this << getIDAndType(DI->getOperand());
  }
  void visitDeallocPackMetadataInst(DeallocPackMetadataInst *DPMI) {
    *this << getIDAndType(DPMI->getOperand());
  }
  void visitDeallocStackRefInst(DeallocStackRefInst *ESRL) {
    *this << getIDAndType(ESRL->getOperand());
  }
  void visitDeallocRefInst(DeallocRefInst *DI) {
    *this << getIDAndType(DI->getOperand());
  }
  void visitDeallocPartialRefInst(DeallocPartialRefInst *DPI) {
    *this << getIDAndType(DPI->getInstance());
    *this << ", ";
    *this << getIDAndType(DPI->getMetatype());
  }
  void visitDeallocBoxInst(DeallocBoxInst *DI) {
    *this << getIDAndType(DI->getOperand());
  }
  void visitDestroyAddrInst(DestroyAddrInst *DI) {
    *this << getIDAndType(DI->getOperand());
  }
  void visitProjectBoxInst(ProjectBoxInst *PBI) {
    *this << getIDAndType(PBI->getOperand()) << ", " << PBI->getFieldIndex();
  }
  void visitProjectExistentialBoxInst(ProjectExistentialBoxInst *PEBI) {
    *this << PEBI->getType().getObjectType()
          << " in " << getIDAndType(PEBI->getOperand());
  }
  void visitBeginAccessInst(BeginAccessInst *BAI) {
    *this << '[' << getSILAccessKindName(BAI->getAccessKind()) << "] ["
          << getSILAccessEnforcementName(BAI->getEnforcement()) << "] "
          << (BAI->hasNoNestedConflict() ? "[no_nested_conflict] " : "")
          << (BAI->isFromBuiltin() ? "[builtin] " : "")
          << getIDAndType(BAI->getOperand());
  }
  void visitMoveOnlyWrapperToCopyableAddrInst(
      MoveOnlyWrapperToCopyableAddrInst *BAI) {
    *this << getIDAndType(BAI->getOperand());
  }
  void
  visitMoveOnlyWrapperToCopyableBoxInst(MoveOnlyWrapperToCopyableBoxInst *BAI) {
    *this << getIDAndType(BAI->getOperand());
  }
  void visitCopyableToMoveOnlyWrapperAddrInst(
      CopyableToMoveOnlyWrapperAddrInst *BAI) {
    *this << getIDAndType(BAI->getOperand());
  }
  void visitEndAccessInst(EndAccessInst *EAI) {
    *this << (EAI->isAborting() ? "[abort] " : "")
          << getIDAndType(EAI->getOperand());
  }
  void visitBeginUnpairedAccessInst(BeginUnpairedAccessInst *BAI) {
    *this << '[' << getSILAccessKindName(BAI->getAccessKind()) << "] ["
          << getSILAccessEnforcementName(BAI->getEnforcement()) << "] "
          << (BAI->hasNoNestedConflict() ? "[no_nested_conflict] " : "")
          << (BAI->isFromBuiltin() ? "[builtin] " : "")
          << getIDAndType(BAI->getSource()) << ", " 
          << getIDAndType(BAI->getBuffer());
  }
  void visitEndUnpairedAccessInst(EndUnpairedAccessInst *EAI) {
    *this << (EAI->isAborting() ? "[abort] " : "") << '['
          << getSILAccessEnforcementName(EAI->getEnforcement()) << "] "
          << (EAI->isFromBuiltin() ? "[builtin] " : "")
          << getIDAndType(EAI->getOperand());
  }

  void visitCondFailInst(CondFailInst *FI) {
    *this << getIDAndType(FI->getOperand()) << ", "
          << QuotedString(FI->getMessage());
  }

  void visitIncrementProfilerCounterInst(IncrementProfilerCounterInst *IPCI) {
    *this << IPCI->getCounterIndex() << ", "
          << QuotedString(IPCI->getPGOFuncName()) << ", "
          << "num_counters " << IPCI->getNumCounters() << ", "
          << "hash " << IPCI->getPGOFuncHash();
  }

  void visitIndexAddrInst(IndexAddrInst *IAI) {
    *this << (IAI->needsStackProtection() ? "[stack_protection] " : "")
          << getIDAndType(IAI->getBase()) << ", "
          << getIDAndType(IAI->getIndex());
  }

  void visitTailAddrInst(TailAddrInst *TAI) {
    *this << getIDAndType(TAI->getBase()) << ", "
          << getIDAndType(TAI->getIndex()) << ", " << TAI->getTailType();
  }

  void visitIndexRawPointerInst(IndexRawPointerInst *IAI) {
    *this << getIDAndType(IAI->getBase()) << ", "
          << getIDAndType(IAI->getIndex());
  }

  void visitUnreachableInst(UnreachableInst *UI) {}

  void visitReturnInst(ReturnInst *RI) {
    *this << getIDAndType(RI->getOperand());
  }

  void visitTestSpecificationInst(TestSpecificationInst *TSI) {
    *this << QuotedString(TSI->getArgumentsSpecification());
  }

  void visitThrowInst(ThrowInst *TI) {
    *this << getIDAndType(TI->getOperand());
  }

  void visitUnwindInst(UnwindInst *UI) {
    // no operands
  }

  void visitYieldInst(YieldInst *YI) {
    auto values = YI->getYieldedValues();
    if (values.size() != 1) *this << '(';
    llvm::interleave(
        values, [&](SILValue value) { *this << getIDAndType(value); },
        [&] { *this << ", "; });
    if (values.size() != 1) *this << ')';
    *this << ", resume " << Ctx.getID(YI->getResumeBB())
          << ", unwind " << Ctx.getID(YI->getUnwindBB());
  }
  
  void visitGetAsyncContinuationInst(GetAsyncContinuationInst *GI) {
    if (GI->throws())
      *this << "[throws] ";
    *this << GI->getFormalResumeType();
  }

  void visitGetAsyncContinuationAddrInst(GetAsyncContinuationAddrInst *GI) {
    if (GI->throws())
      *this << "[throws] ";
    *this << GI->getFormalResumeType()
          << ", " << getIDAndType(GI->getOperand());
  }
  
  void visitAwaitAsyncContinuationInst(AwaitAsyncContinuationInst *AI) {
    *this << getIDAndType(AI->getOperand())
          << ", resume " << Ctx.getID(AI->getResumeBB());
    
    if (auto errorBB = AI->getErrorBB()) {
      *this << ", error " << Ctx.getID(AI->getErrorBB());
    }
  }

  void visitHopToExecutorInst(HopToExecutorInst *HTEI) {
    if (HTEI->isMandatory())
      *this << "[mandatory] ";
    *this << getIDAndType(HTEI->getTargetExecutor());
  }

  void visitExtractExecutorInst(ExtractExecutorInst *AEI) {
    *this << getIDAndType(AEI->getExpectedExecutor());
  }

  void visitSwitchValueInst(SwitchValueInst *SII) {
    *this << getIDAndType(SII->getOperand());
    for (unsigned i = 0, e = SII->getNumCases(); i < e; ++i) {
      SILValue value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SII->getCase(i);
      *this << ", case " << Ctx.getID(value) << ": " << Ctx.getID(dest);
    }
    if (SII->hasDefault())
      *this << ", default " << Ctx.getID(SII->getDefaultBB());
  }

  void printSwitchEnumInst(SwitchEnumTermInst switchEnum) {
    *this << getIDAndType(switchEnum.getOperand());
    for (unsigned i = 0, e = switchEnum.getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = switchEnum.getCase(i);
      *this << ", case " << SILDeclRef(elt, SILDeclRef::Kind::EnumElement)
            << ": " << Ctx.getID(dest);
      if (switchEnum.getCaseCount(i)) {
        *this << " !case_count(" << switchEnum.getCaseCount(i).getValue()
              << ")";
      }
    }
    if (switchEnum.hasDefault()) {
      *this << ", default " << Ctx.getID(switchEnum.getDefaultBB());
      if (switchEnum.getDefaultCount()) {
        *this << " !default_count(" << switchEnum.getDefaultCount().getValue()
              << ")";
      }
      if (NullablePtr<EnumElementDecl> uniqueCase =
              switchEnum.getUniqueCaseForDefault()) {
        lineComments << SILDeclRef(uniqueCase.get(),
                                   SILDeclRef::Kind::EnumElement);
      }
    }
  }

  void visitSwitchEnumInst(SwitchEnumInst *switchEnum) {
    printSwitchEnumInst(switchEnum);
    printForwardingOwnershipKind(switchEnum, switchEnum->getOperand());
  }
  void visitSwitchEnumAddrInst(SwitchEnumAddrInst *switchEnum) {
    printSwitchEnumInst(switchEnum);
  }

  void printSelectEnumInst(SelectEnumInstBase *SEI) {
    *this << getIDAndType(SEI->getEnumOperand());

    for (unsigned i = 0, e = SEI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILValue result;
      std::tie(elt, result) = SEI->getCase(i);
      *this << ", case " << SILDeclRef(elt, SILDeclRef::Kind::EnumElement)
            << ": " << Ctx.getID(result);
    }
    if (SEI->hasDefault())
      *this << ", default " << Ctx.getID(SEI->getDefaultResult());

    *this << " : " << SEI->getType();
  }

  void visitSelectEnumInst(SelectEnumInst *SEI) {
    printSelectEnumInst(SEI);
    printForwardingOwnershipKind(SEI, SEI->getOperand());
  }
  void visitSelectEnumAddrInst(SelectEnumAddrInst *SEI) {
    printSelectEnumInst(SEI);
  }

  void visitDynamicMethodBranchInst(DynamicMethodBranchInst *DMBI) {
    *this << getIDAndType(DMBI->getOperand()) << ", " << DMBI->getMember()
          << ", " << Ctx.getID(DMBI->getHasMethodBB()) << ", "
          << Ctx.getID(DMBI->getNoMethodBB());
  }

  void printBranchArgs(OperandValueArrayRef args) {
    if (args.empty()) return;

    *this << '(';
    llvm::interleave(
        args, [&](SILValue v) { *this << getIDAndType(v); },
        [&] { *this << ", "; });
    *this << ')';
  }
  
  void visitBranchInst(BranchInst *UBI) {
    *this << Ctx.getID(UBI->getDestBB());
    printBranchArgs(UBI->getArgs());
  }

  void visitCondBranchInst(CondBranchInst *CBI) {
    *this << Ctx.getID(CBI->getCondition()) << ", "
          << Ctx.getID(CBI->getTrueBB());
    printBranchArgs(CBI->getTrueArgs());
    *this << ", " << Ctx.getID(CBI->getFalseBB());
    printBranchArgs(CBI->getFalseArgs());
    if (CBI->getTrueBBCount())
      *this << " !true_count(" << CBI->getTrueBBCount().getValue() << ")";
    if (CBI->getFalseBBCount())
      *this << " !false_count(" << CBI->getFalseBBCount().getValue() << ")";
  }
  
  void visitKeyPathInst(KeyPathInst *KPI) {
    *this << KPI->getType() << ", ";
    
    auto pattern = KPI->getPattern();
    
    if (pattern->getGenericSignature()) {
      pattern->getGenericSignature()->print(PrintState.OS);
      *this << ' ';
    }
    
    *this << "(";
    
    if (!pattern->getObjCString().empty())
      *this << "objc \"" << pattern->getObjCString() << "\"; ";
    
    *this << "root $" << KPI->getPattern()->getRootType();

    for (auto &component : pattern->getComponents()) {
      *this << "; ";

      printKeyPathPatternComponent(component);
    }
    
    *this << ')';
    if (!KPI->getSubstitutions().empty()) {
      *this << ' ';
      printSubstitutions(KPI->getSubstitutions());
    }
    if (!KPI->getPatternOperands().empty()) {
      *this << " (";
      
      interleave(KPI->getPatternOperands(),
        [&](const Operand &operand) {
          *this << Ctx.getID(operand.get());
        }, [&]{
          *this << ", ";
        });
      
      *this << ")";
    }
  }
  
  void
  printKeyPathPatternComponent(const KeyPathPatternComponent &component) {
    auto printComponentIndices =
      [&](ArrayRef<KeyPathPatternComponent::Index> indices) {
        *this << '[';
        interleave(indices,
          [&](const KeyPathPatternComponent::Index &i) {
            *this << "%$" << i.Operand << " : $"
                  << i.FormalType << " : "
                  << i.LoweredType;
          }, [&]{
            *this << ", ";
          });
        *this << ']';
      };

    switch (auto kind = component.getKind()) {
    case KeyPathPatternComponent::Kind::StoredProperty: {
      auto prop = component.getStoredPropertyDecl();
      *this << "stored_property #";
      printValueDecl(prop, PrintState.OS);
      *this << " : $" << component.getComponentType();
      break;
    }
    case KeyPathPatternComponent::Kind::GettableProperty:
    case KeyPathPatternComponent::Kind::SettableProperty: {
      *this << (kind == KeyPathPatternComponent::Kind::GettableProperty
                  ? "gettable_property $" : "settable_property $")
            << component.getComponentType() << ", "
            << " id ";
      auto id = component.getComputedPropertyId();
      switch (id.getKind()) {
      case KeyPathPatternComponent::ComputedPropertyId::DeclRef: {
        auto declRef = id.getDeclRef();
        *this << declRef << " : "
              << declRef.getDecl()->getInterfaceType();
        break;
      }
      case KeyPathPatternComponent::ComputedPropertyId::Function: {
        id.getFunction()->printName(PrintState.OS);
        *this << " : " << id.getFunction()->getLoweredType();
        break;
      }
      case KeyPathPatternComponent::ComputedPropertyId::Property: {
        *this << "##";
        printValueDecl(id.getProperty(), PrintState.OS);
        break;
      }
      }
      *this << ", getter ";
      component.getComputedPropertyGetter()->printName(PrintState.OS);
      *this << " : "
            << component.getComputedPropertyGetter()->getLoweredType();
      if (kind == KeyPathPatternComponent::Kind::SettableProperty) {
        *this << ", setter ";
        component.getComputedPropertySetter()->printName(PrintState.OS);
        *this << " : "
              << component.getComputedPropertySetter()->getLoweredType();
      }
      
      if (!component.getSubscriptIndices().empty()) {
        *this << ", indices ";
        printComponentIndices(component.getSubscriptIndices());
        *this << ", indices_equals ";
        component.getSubscriptIndexEquals()->printName(PrintState.OS);
        *this << " : "
              << component.getSubscriptIndexEquals()->getLoweredType();
        *this << ", indices_hash ";
        component.getSubscriptIndexHash()->printName(PrintState.OS);
        *this << " : "
              << component.getSubscriptIndexHash()->getLoweredType();
      }
      
      if (auto external = component.getExternalDecl()) {
        *this << ", external #";
        printValueDecl(external, PrintState.OS);
        auto subs = component.getExternalSubstitutions();
        if (!subs.empty()) {
          printSubstitutions(subs);
        }
      }
      
      break;
    }
    case KeyPathPatternComponent::Kind::OptionalWrap:
    case KeyPathPatternComponent::Kind::OptionalChain:
    case KeyPathPatternComponent::Kind::OptionalForce: {
      switch (kind) {
      case KeyPathPatternComponent::Kind::OptionalWrap:
        *this << "optional_wrap : $";
        break;
      case KeyPathPatternComponent::Kind::OptionalChain:
        *this << "optional_chain : $";
        break;
      case KeyPathPatternComponent::Kind::OptionalForce:
        *this << "optional_force : $";
        break;
      default:
        llvm_unreachable("out of sync");
      }
      *this << component.getComponentType();
      break;
    }
    case KeyPathPatternComponent::Kind::TupleElement: {
      *this << "tuple_element #" << component.getTupleIndex();
      *this << " : $" << component.getComponentType();
      break;
    }
    }
  }

  void visitDifferentiableFunctionInst(DifferentiableFunctionInst *dfi) {
    *this << "[parameters";
    for (auto i : dfi->getParameterIndices()->getIndices())
      *this << ' ' << i;
    *this << "] ";
    *this << "[results";
    for (auto i : dfi->getResultIndices()->getIndices())
      *this << ' ' << i;
    *this << "] ";
    *this << getIDAndType(dfi->getOriginalFunction());
    if (dfi->hasDerivativeFunctions()) {
      *this << " with_derivative ";
      *this << '{' << getIDAndType(dfi->getJVPFunction()) << ", "
            << getIDAndType(dfi->getVJPFunction()) << '}';
    }
  }

  void visitLinearFunctionInst(LinearFunctionInst *lfi) {
    *this << "[parameters";
    for (auto i : lfi->getParameterIndices()->getIndices())
      *this << ' ' << i;
    *this << "] ";
    *this << getIDAndType(lfi->getOriginalFunction());
    if (lfi->hasTransposeFunction()) {
      *this << " with_transpose ";
      *this << getIDAndType(lfi->getTransposeFunction());
    }
  }

  void visitDifferentiableFunctionExtractInst(
      DifferentiableFunctionExtractInst *dfei) {
    *this << '[';
    switch (dfei->getExtractee()) {
    case NormalDifferentiableFunctionTypeComponent::Original:
      *this << "original";
      break;
    case NormalDifferentiableFunctionTypeComponent::JVP:
      *this << "jvp";
      break;
    case NormalDifferentiableFunctionTypeComponent::VJP:
      *this << "vjp";
      break;
    }
    *this << "] ";
    *this << getIDAndType(dfei->getOperand());
    if (dfei->hasExplicitExtracteeType()) {
      *this << " as ";
      *this << dfei->getType();
    }
    printForwardingOwnershipKind(dfei, dfei->getOperand());
  }

  void visitLinearFunctionExtractInst(LinearFunctionExtractInst *lfei) {
    *this << '[';
    switch (lfei->getExtractee()) {
    case LinearDifferentiableFunctionTypeComponent::Original:
      *this << "original";
      break;
    case LinearDifferentiableFunctionTypeComponent::Transpose:
      *this << "transpose";
      break;
    }
    *this << "] ";
    *this << getIDAndType(lfei->getOperand());
    printForwardingOwnershipKind(lfei, lfei->getOperand());
  }

  void visitDifferentiabilityWitnessFunctionInst(
      DifferentiabilityWitnessFunctionInst *dwfi) {
    auto *witness = dwfi->getWitness();
    *this << '[';
    switch (dwfi->getWitnessKind()) {
    case DifferentiabilityWitnessFunctionKind::JVP:
      *this << "jvp";
      break;
    case DifferentiabilityWitnessFunctionKind::VJP:
      *this << "vjp";
      break;
    case DifferentiabilityWitnessFunctionKind::Transpose:
      *this << "transpose";
      break;
    }
    *this << "] [";
    switch (dwfi->getWitness()->getKind()) {
    case DifferentiabilityKind::Forward:
      *this << "forward";
      break;
    case DifferentiabilityKind::Reverse:
      *this << "reverse";
      break;
    case DifferentiabilityKind::Normal:
      *this << "normal";
      break;
    case DifferentiabilityKind::Linear:
      *this << "linear";
      break;
    case DifferentiabilityKind::NonDifferentiable:
      llvm_unreachable("Impossible case");
    }
    *this << "] [parameters";
    for (auto i : witness->getParameterIndices()->getIndices())
      *this << ' ' << i;
    *this << "] [results";
    for (auto i : witness->getResultIndices()->getIndices())
      *this << ' ' << i;
    *this << "] ";
    if (auto witnessGenSig = witness->getDerivativeGenericSignature()) {
      auto subPrinter = PrintOptions::printSIL();
      witnessGenSig->print(PrintState.OS, subPrinter);
      *this << " ";
    }
    printSILFunctionNameAndType(PrintState.OS, witness->getOriginalFunction());
    if (dwfi->getHasExplicitFunctionType()) {
      *this << " as ";
      *this << dwfi->getType();
    }
  }

  void visitHasSymbolInst(HasSymbolInst *hsi) {
    *this << "#";
    printValueDecl(hsi->getDecl(), PrintState.OS);
  }
};

} // namespace swift

static void printBlockID(raw_ostream &OS, SILBasicBlock *bb) {
  SILPrintContext Ctx(OS);
  OS << Ctx.getID(bb);
}

void SILBasicBlock::printAsOperand(raw_ostream &OS, bool PrintType) {
  printBlockID(OS, this);
}

//===----------------------------------------------------------------------===//
// Printing for SILInstruction, SILBasicBlock, SILFunction, and SILModule
//===----------------------------------------------------------------------===//

void SILNode::dump() const {
  print(llvm::errs());
}

void SILNode::print(raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  SILPrinter(Ctx).print(this);
}

void SILInstruction::dump() const {
  print(llvm::errs());
}

void SingleValueInstruction::dump() const {
  SILInstruction::dump();
}

void SILInstruction::print(raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  SILPrinter(Ctx).print(this);
}

void NonSingleValueInstruction::dump() const {
  SILNode::dump();
}

/// Pretty-print the SILBasicBlock to errs.
void SILBasicBlock::dump() const {
  print(llvm::errs());
}

/// Pretty-print the SILBasicBlock to the designated stream.
void SILBasicBlock::print(raw_ostream &OS) const {
  SILPrintContext Ctx(OS);

  // Print the debug scope (and compute if we didn't do it already).
  auto &SM = this->getParent()->getModule().getASTContext().SourceMgr;
  for (auto &I : *this) {
    SILPrinter P(Ctx);
    P.printDebugScope(I.getDebugScope(), SM);
  }

  SILPrinter(Ctx).print(this);
}

void SILBasicBlock::print(SILPrintContext &Ctx) const {
  SILPrinter(Ctx).print(this);
}

#ifndef NDEBUG
void SILBasicBlock::dumpID() const {
  printID(llvm::errs());
}

void SILBasicBlock::printID(llvm::raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  printID(Ctx);
}

void SILBasicBlock::printID(SILPrintContext &Ctx) const {
  SILPrinter(Ctx).printID(this);
}
#endif

/// Pretty-print the SILFunction to errs.
void SILFunction::dump(bool Verbose) const {
  SILPrintContext Ctx(llvm::errs(), Verbose);
  print(Ctx);
}

// This is out of line so the debugger can find it.
void SILFunction::dump() const {
  dump(false);
}

void SILFunction::dump(const char *FileName) const {
  std::error_code EC;
  llvm::raw_fd_ostream os(FileName, EC, llvm::sys::fs::OpenFlags::OF_None);
  print(os);
}

static StringRef getLinkageString(SILLinkage linkage) {
  switch (linkage) {
  case SILLinkage::Public: return "public ";
  case SILLinkage::PublicNonABI: return "non_abi ";
  case SILLinkage::Hidden: return "hidden ";
  case SILLinkage::Shared: return "shared ";
  case SILLinkage::Private: return "private ";
  case SILLinkage::PublicExternal: return "public_external ";
  case SILLinkage::HiddenExternal: return "hidden_external ";
  }
  llvm_unreachable("bad linkage");
}

static void printLinkage(llvm::raw_ostream &OS, SILLinkage linkage,
                         bool isDefinition) {
  if ((isDefinition && linkage == SILLinkage::DefaultForDefinition) ||
      (!isDefinition && linkage == SILLinkage::DefaultForDeclaration))
    return;

  OS << getLinkageString(linkage);
}

static void printClangQualifiedNameCommentIfPresent(llvm::raw_ostream &OS,
                                                    const clang::Decl *decl) {
  if (decl) {
    if (auto namedDecl = dyn_cast_or_null<clang::NamedDecl>(decl)) {
      OS << "// clang name: ";
      namedDecl->printQualifiedName(OS);
      OS << "\n";
    }
  }
}

/// Pretty-print the SILFunction to the designated stream.
void SILFunction::print(SILPrintContext &PrintCtx) const {
  llvm::raw_ostream &OS = PrintCtx.OS();
  if (PrintCtx.printDebugInfo()) {
    auto &SM = getModule().getASTContext().SourceMgr;
    for (auto &BB : *this)
      for (auto &I : BB) {
        SILPrinter P(PrintCtx);
        P.printDebugScope(I.getDebugScope(), SM);
      }
    OS << "\n";
  }

  if (SILPrintGenericSpecializationInfo) {
    if (isSpecialization()) {
      printGenericSpecializationInfo(OS, "function", getName(),
                                     getSpecializationInfo());
    }
  }

  OS << "// " << demangleSymbol(getName());
  if (PrintCtx.printDebugInfo()) {
    auto &SM = getModule().getASTContext().SourceMgr;
    SILPrinter P(PrintCtx);
    P.printDebugLocRef(getLocation(), SM);
    P.printDebugScopeRef(getDebugScope(), SM);
  }
  OS << '\n';
  printClangQualifiedNameCommentIfPresent(OS, getClangDecl());

  OS << "sil ";
  printLinkage(OS, getLinkage(), isDefinition());

  if (isTransparent())
    OS << "[transparent] ";

  switch (isSerialized()) {
  case IsNotSerialized: break;
  case IsSerialized: OS << "[serialized] "; break;
  }

  switch (isThunk()) {
  case IsNotThunk: break;
  case IsThunk: OS << "[thunk] "; break;
  case IsSignatureOptimizedThunk:
    OS << "[signature_optimized_thunk] ";
    break;
  case IsReabstractionThunk: OS << "[reabstraction_thunk] "; break;
  }
  if (isDynamicallyReplaceable()) {
    OS << "[dynamically_replacable] ";
  }
  if (isDistributed()) {
    OS << "[distributed] ";
  }
  if (isRuntimeAccessible()) {
    OS << "[runtime_accessible] ";
  }
  if (forceEnableLexicalLifetimes()) {
    OS << "[lexical_lifetimes] ";
  }
  if (!useStackForPackMetadata()) {
    OS << "[no_onstack_pack_metadata] ";
  }

  if (isExactSelfClass()) {
    OS << "[exact_self_class] ";
  }
  if (isWithoutActuallyEscapingThunk())
    OS << "[without_actually_escaping] ";

  switch (getSpecialPurpose()) {
  case SILFunction::Purpose::None:
    break;
  case SILFunction::Purpose::GlobalInit:
    OS << "[global_init] ";
    break;
  case SILFunction::Purpose::GlobalInitOnceFunction:
    OS << "[global_init_once_fn] ";
    break;
  case SILFunction::Purpose::LazyPropertyGetter:
    OS << "[lazy_getter] ";
    break;
  }
  if (isAlwaysWeakImported())
    OS << "[weak_imported] ";
  auto availability = getAvailabilityForLinkage();
  if (!availability.isAlwaysAvailable()) {
    auto version = availability.getOSVersion().getLowerEndpoint();
    OS << "[available " << version.getAsString() << "] ";
  }

  switch (getInlineStrategy()) {
    case NoInline: OS << "[noinline] "; break;
    case AlwaysInline: OS << "[always_inline] "; break;
    case InlineDefault: break;
  }

  switch (getOptimizationMode()) {
    case OptimizationMode::NoOptimization: OS << "[Onone] "; break;
    case OptimizationMode::ForSpeed: OS << "[Ospeed] "; break;
    case OptimizationMode::ForSize: OS << "[Osize] "; break;
    default: break;
  }

  PerformanceConstraints perf = getPerfConstraints();
  switch (perf) {
    case PerformanceConstraints::None:         break;
    case PerformanceConstraints::NoLocks:      OS << "[no_locks] "; break;
    case PerformanceConstraints::NoAllocation: OS << "[no_allocation] "; break;
  }

  if (getEffectsKind() == EffectsKind::ReadOnly)
    OS << "[readonly] ";
  else if (getEffectsKind() == EffectsKind::ReadNone)
      OS << "[readnone] ";
  else if (getEffectsKind() == EffectsKind::ReadWrite)
    OS << "[readwrite] ";
  else if (getEffectsKind() == EffectsKind::ReleaseNone)
    OS << "[releasenone] ";

  if (auto *replacedFun = getDynamicallyReplacedFunction()) {
    OS << "[dynamic_replacement_for \"";
    OS << replacedFun->getName();
    OS << "\"] ";
  }

  if (auto *usedFunc = getReferencedAdHocRequirementWitnessFunction()) {
    OS << "[ref_adhoc_requirement_witness \"";
    OS << usedFunc->getName();
    OS << "\"] ";
  }

  if (hasObjCReplacement()) {
    OS << "[objc_replacement_for \"";
    OS << getObjCReplacement().str();
    OS << "\"] ";
  }

  for (auto &Attr : getSemanticsAttrs())
    OS << "[_semantics \"" << Attr << "\"] ";

  for (auto *Attr : getSpecializeAttrs()) {
    OS << "[_specialize "; Attr->print(OS); OS << "] ";
  }

  if (markedAsUsed())
    OS << "[used] ";

  if (!section().empty())
    OS << "[section \"" << section() << "\"] ";

  // TODO: Handle clang node owners which don't have a name.
  if (hasClangNode() && getClangNodeOwner()->hasName()) {
    OS << "[clang ";
    printValueDecl(getClangNodeOwner(), OS);
    OS << "] ";
  }

  // Handle functions that are deserialized from canonical SIL. Normally, we
  // should emit SIL with the correct SIL stage, so preserving this attribute
  // won't be necessary. But consider serializing raw SIL (either textual SIL or
  // SIB) after importing canonical SIL from another module. If the imported
  // functions are reserialized (e.g. shared linkage), then we must preserve
  // this attribute.
  if (WasDeserializedCanonical && getModule().getStage() == SILStage::Raw)
    OS << "[canonical] ";

  // If this function is not an external declaration /and/ is in ownership ssa
  // form, print [ossa].
  if (!isExternalDeclaration() && hasOwnership())
    OS << "[ossa] ";

  if (needsStackProtection())
    OS << "[stack_protection] ";

  llvm::DenseMap<CanType, Identifier> sugaredTypeNames;
  printSILFunctionNameAndType(OS, this, sugaredTypeNames, &PrintCtx);

  if (!isExternalDeclaration()) {
    if (auto eCount = getEntryCount()) {
      OS << " !function_entry_count(" << eCount.getValue() << ")";
    }
    OS << " {\n";
    
    writeEffects(OS);

    SILPrinter(PrintCtx, sugaredTypeNames.empty() ? nullptr : &sugaredTypeNames)
        .print(this);
    OS << "} // end sil function '" << getName() << '\'';
  } else if (hasArgumentEffects()) {
    OS << " {\n";
    writeEffects(OS);
    OS << "} // end sil function '" << getName() << '\'';
  }

  OS << "\n\n";
}
      
/// Pretty-print the SILFunction's name using SIL syntax,
/// '@function_mangled_name'.
void SILFunction::printName(raw_ostream &OS) const { OS << "@" << Name; }

/// Pretty-print a global variable to the designated stream.
void SILGlobalVariable::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "// " << demangleSymbol(getName()) << '\n';
  printClangQualifiedNameCommentIfPresent(OS, getClangDecl());

  OS << "sil_global ";
  // Passing true for 'isDefinition' lets print the (external) linkage if it's
  // not a definition.
  printLinkage(OS, getLinkage(), /*isDefinition*/ true);

  if (isSerialized())
    OS << "[serialized] ";
  
  if (isLet())
    OS << "[let] ";

  printName(OS);
  OS << " : " << LoweredType;

  if (!StaticInitializerBlock.empty()) {
    OS << " = {\n";
    {
      SILPrintContext Ctx(OS);
      SILPrinter Printer(Ctx);
      for (const SILInstruction &I : StaticInitializerBlock) {
        Printer.print(&I);
      }
    }
    OS << "}\n";
  }

  OS << "\n\n";
}

void SILGlobalVariable::dump(bool Verbose) const {
  print(llvm::errs(), Verbose);
}
void SILGlobalVariable::dump() const {
   dump(false);
}

void SILGlobalVariable::printName(raw_ostream &OS) const {
  OS << "@" << Name;
}
      
/// Pretty-print the SILModule to errs.
void SILModule::dump(bool Verbose) const {
  SILPrintContext Ctx(llvm::errs(), Verbose);
  print(Ctx);
}

void SILModule::dump(const char *FileName, bool Verbose,
                     bool PrintASTDecls) const {
  std::error_code EC;
  llvm::raw_fd_ostream os(FileName, EC, llvm::sys::fs::OpenFlags::OF_None);
  SILPrintContext Ctx(os, Verbose);
  print(Ctx, getSwiftModule(), PrintASTDecls);
}

static void printSILGlobals(SILPrintContext &Ctx,
                            const SILModule::GlobalListType &Globals) {
  if (!Ctx.sortSIL()) {
    for (const SILGlobalVariable &g : Globals)
      g.print(Ctx.OS(), Ctx.printVerbose());
    return;
  }

  std::vector<const SILGlobalVariable *> globals;
  globals.reserve(Globals.size());
  for (const SILGlobalVariable &g : Globals)
    globals.push_back(&g);
  std::sort(globals.begin(), globals.end(),
    [] (const SILGlobalVariable *g1, const SILGlobalVariable *g2) -> bool {
      return g1->getName().compare(g2->getName()) == -1;
    }
  );
  for (const SILGlobalVariable *g : globals)
    g->print(Ctx.OS(), Ctx.printVerbose());
}

static void printSILFunctions(SILPrintContext &Ctx,
                              const SILModule::FunctionListType &Functions) {
  if (!Ctx.sortSIL()) {
    for (const SILFunction &f : Functions)
      f.print(Ctx);
    return;
  }

  std::vector<const SILFunction *> functions;
  functions.reserve(Functions.size());
  for (const SILFunction &f : Functions)
    functions.push_back(&f);
  std::sort(functions.begin(), functions.end(),
    [] (const SILFunction *f1, const SILFunction *f2) -> bool {
      return f1->getName().compare(f2->getName()) == -1;
    }
  );
  for (const SILFunction *f : functions)
    f->print(Ctx);
}

static void printSILVTables(SILPrintContext &Ctx,
                            const SILModule::VTableListType &VTables) {
  if (!Ctx.sortSIL()) {
    for (const auto &vt : VTables)
      vt->print(Ctx.OS(), Ctx.printVerbose());
    return;
  }

  std::vector<const SILVTable *> vtables;
  vtables.reserve(VTables.size());
  for (const auto &vt : VTables)
    vtables.push_back(vt);
  std::sort(vtables.begin(), vtables.end(),
    [] (const SILVTable *v1, const SILVTable *v2) -> bool {
      StringRef Name1 = v1->getClass()->getName().str();
      StringRef Name2 = v2->getClass()->getName().str();
      return Name1.compare(Name2) == -1;
    }
  );
  for (const SILVTable *vt : vtables)
    vt->print(Ctx.OS(), Ctx.printVerbose());
}

static void printSILMoveOnlyDeinits(
    SILPrintContext &printCtx,
    const SILModule::SILMoveOnlyDeinitListType &deinitTables) {
  if (!printCtx.sortSIL()) {
    for (const auto &tbl : deinitTables)
      tbl->print(printCtx.OS(), printCtx.printVerbose());
    return;
  }

  std::vector<const SILMoveOnlyDeinit *> sortedTables;
  sortedTables.reserve(deinitTables.size());
  for (const auto &tbl : deinitTables)
    sortedTables.push_back(tbl);
  std::sort(
      sortedTables.begin(), sortedTables.end(),
      [](const SILMoveOnlyDeinit *v1, const SILMoveOnlyDeinit *v2) -> bool {
        StringRef name1 = v1->getNominalDecl()->getName().str();
        StringRef name2 = v2->getNominalDecl()->getName().str();
        return name1.compare(name2) == -1;
      });
  for (const auto *tbl : sortedTables)
    tbl->print(printCtx.OS(), printCtx.printVerbose());
}

static void
printSILWitnessTables(SILPrintContext &Ctx,
                      const SILModule::WitnessTableListType &WTables) {
  if (!Ctx.sortSIL()) {
    for (const SILWitnessTable &wt : WTables)
      wt.print(Ctx.OS(), Ctx.printVerbose());
    return;
  }

  std::vector<const SILWitnessTable *> witnesstables;
  witnesstables.reserve(WTables.size());
  for (const SILWitnessTable &wt : WTables)
    witnesstables.push_back(&wt);
  std::sort(witnesstables.begin(), witnesstables.end(),
    [] (const SILWitnessTable *w1, const SILWitnessTable *w2) -> bool {
      return w1->getName().compare(w2->getName()) == -1;
    }
  );
  for (const SILWitnessTable *wt : witnesstables)
    wt->print(Ctx.OS(), Ctx.printVerbose());
}

static void
printSILDefaultWitnessTables(SILPrintContext &Ctx,
                        const SILModule::DefaultWitnessTableListType &WTables) {
  if (!Ctx.sortSIL()) {
    for (const SILDefaultWitnessTable &wt : WTables)
      wt.print(Ctx.OS(), Ctx.printVerbose());
    return;
  }

  std::vector<const SILDefaultWitnessTable *> witnesstables;
  witnesstables.reserve(WTables.size());
  for (const SILDefaultWitnessTable &wt : WTables)
    witnesstables.push_back(&wt);
  std::sort(witnesstables.begin(), witnesstables.end(),
    [] (const SILDefaultWitnessTable *w1,
        const SILDefaultWitnessTable *w2) -> bool {
      return w1->getProtocol()->getName()
          .compare(w2->getProtocol()->getName()) == -1;
    }
  );
  for (const SILDefaultWitnessTable *wt : witnesstables)
    wt->print(Ctx.OS(), Ctx.printVerbose());
}

static void printSILDifferentiabilityWitnesses(
    SILPrintContext &Ctx,
    const SILModule::DifferentiabilityWitnessListType &diffWitnesses) {
  if (!Ctx.sortSIL()) {
    for (auto &dw : diffWitnesses)
      dw.print(Ctx.OS(), Ctx.printVerbose());
    return;
  }

  std::vector<const SILDifferentiabilityWitness *> sortedDiffWitnesses;
  sortedDiffWitnesses.reserve(diffWitnesses.size());
  for (auto &dw : diffWitnesses)
    sortedDiffWitnesses.push_back(&dw);
  std::sort(sortedDiffWitnesses.begin(), sortedDiffWitnesses.end(),
            [](const SILDifferentiabilityWitness *dw1,
               const SILDifferentiabilityWitness *dw2) -> bool {
              // TODO(TF-893): Sort based on more criteria for deterministic
              // ordering.
              return dw1->getOriginalFunction()->getName().compare(
                         dw2->getOriginalFunction()->getName()) == -1;
            });
  for (auto *dw : sortedDiffWitnesses)
    dw->print(Ctx.OS(), Ctx.printVerbose());
}

static void printSILLinearMapTypes(SILPrintContext &Ctx,
                                   const ModuleDecl *M) {
  auto &OS = Ctx.OS();

  PrintOptions Options = PrintOptions::printSIL();
  Options.TypeDefinitions = true;
  Options.VarInitializers = true;
  Options.ExplodePatternBindingDecls = true;
  Options.SkipImplicit = false;
  Options.PrintGetSetOnRWProperties = true;
  Options.PrintInSILBody = false;

  SmallVector<Decl *, 32> topLevelDecls;
  M->getTopLevelDecls(topLevelDecls);
  for (const Decl *D : topLevelDecls) {
    if (D->getDeclContext() == M)
      continue;

    if (!isa<StructDecl>(D) && !isa<EnumDecl>(D))
      continue;

    StringRef Name = cast<TypeDecl>(D)->getNameStr();
    if (!Name.startswith("_AD__"))
      continue;

    D->print(OS, Options);
    OS << "\n\n";
  }
}

static void
printSILCoverageMaps(SILPrintContext &Ctx,
                     const SILModule::CoverageMapCollectionType &CoverageMaps) {
  if (!Ctx.sortSIL()) {
    for (const auto &M : CoverageMaps)
      M.second->print(Ctx);
    return;
  }

  std::vector<const SILCoverageMap *> Maps;
  Maps.reserve(CoverageMaps.size());
  for (const auto &M : CoverageMaps)
    Maps.push_back(M.second);
  std::sort(Maps.begin(), Maps.end(),
            [](const SILCoverageMap *LHS, const SILCoverageMap *RHS) -> bool {
              return LHS->getName().compare(RHS->getName()) == -1;
            });
  for (const SILCoverageMap *M : Maps)
    M->print(Ctx);
}

using FileIDMap = llvm::StringMap<std::pair<std::string, /*isWinner=*/bool>>;

static void printFileIDMapEntry(SILPrintContext &Ctx,
                                const FileIDMap::MapEntryTy &entry) {
  auto &OS = Ctx.OS();
  OS << "//   '" << std::get<0>(entry.second)
     << "' => '" << entry.first() << "'";

  if (!std::get<1>(entry.second))
    OS << " (alternate)";

  OS << "\n";
}

static void printFileIDMap(SILPrintContext &Ctx, const FileIDMap map) {
  if (map.empty())
    return;

  Ctx.OS() << "\n\n// Mappings from '#fileID' to '#filePath':\n";

  if (Ctx.sortSIL()) {
    llvm::SmallVector<llvm::StringRef, 16> keys;
    llvm::copy(map.keys(), std::back_inserter(keys));

    llvm::sort(keys, [&](StringRef leftKey, StringRef rightKey) -> bool {
      const auto &leftValue = map.find(leftKey)->second;
      const auto &rightValue = map.find(rightKey)->second;

      // Lexicographically earlier #file strings sort earlier.
      if (std::get<0>(leftValue) != std::get<0>(rightValue))
        return std::get<0>(leftValue) < std::get<0>(rightValue);

      // Conflict winners sort before losers.
      if (std::get<1>(leftValue) != std::get<1>(rightValue))
        return std::get<1>(leftValue);

      // Finally, lexicographically earlier #filePath strings sort earlier.
      return leftKey < rightKey;
    });

    for (auto key : keys)
      printFileIDMapEntry(Ctx, *map.find(key));
  } else {
    for (const auto &entry : map)
      printFileIDMapEntry(Ctx, entry);
  }
}

void SILProperty::print(SILPrintContext &Ctx) const {
  PrintOptions Options = PrintOptions::printSIL(&Ctx);

  auto &OS = Ctx.OS();
  OS << "sil_property ";
  if (isSerialized())
    OS << "[serialized] ";
  
  OS << '#';
  printValueDecl(getDecl(), OS);
  if (auto sig = getDecl()->getInnermostDeclContext()
                          ->getGenericSignatureOfContext()) {
    sig.getCanonicalSignature()->print(OS, Options);
  }
  OS << " (";
  if (auto component = getComponent())
    SILPrinter(Ctx).printKeyPathPatternComponent(*component);
  OS << ")\n";
}

void SILProperty::dump() const {
  SILPrintContext context(llvm::errs());
  print(context);
}

static void printSILProperties(SILPrintContext &Ctx,
                               const SILModule::PropertyListType &Properties) {
  for (const SILProperty &P : Properties) {
    P.print(Ctx);
  }
}

static void printExternallyVisibleDecls(SILPrintContext &Ctx,
                                        ArrayRef<ValueDecl *> decls) {
  if (decls.empty())
    return;
  Ctx.OS() << "/* externally visible decls: \n";
  for (ValueDecl *decl : decls) {
    printValueDecl(decl, Ctx.OS());
    Ctx.OS() << '\n';
  }
  Ctx.OS() << "*/\n";
}

/// Pretty-print the SILModule to the designated stream.
void SILModule::print(SILPrintContext &PrintCtx, ModuleDecl *M,
                      bool PrintASTDecls) const {
  llvm::raw_ostream &OS = PrintCtx.OS();
  OS << "sil_stage ";
  switch (Stage) {
  case SILStage::Raw:
    OS << "raw";
    break;
  case SILStage::Canonical:
    OS << "canonical";
    break;
  case SILStage::Lowered:
    OS << "lowered";
    break;
  }
  
  OS << "\n\nimport " << BUILTIN_NAME
     << "\nimport " << STDLIB_NAME
     << "\nimport " << SWIFT_SHIMS_NAME << "\n\n";

  // Print the declarations and types from the associated context (origin module or
  // current file).
  if (M && PrintASTDecls) {
    PrintOptions Options = PrintOptions::printSIL();
    Options.TypeDefinitions = true;
    Options.VarInitializers = true;
    // FIXME: ExplodePatternBindingDecls is incompatible with VarInitializers!
    Options.ExplodePatternBindingDecls = true;
    Options.SkipImplicit = false;
    Options.PrintGetSetOnRWProperties = true;
    Options.PrintInSILBody = false;
    bool WholeModuleMode = (M == AssociatedDeclContext);

    SmallVector<Decl *, 32> topLevelDecls;
    M->getTopLevelDecls(topLevelDecls);
    for (const Decl *D : topLevelDecls) {
      if (!WholeModuleMode && !(D->getDeclContext() == AssociatedDeclContext))
          continue;
      if ((isa<ValueDecl>(D) || isa<OperatorDecl>(D) ||
           isa<ExtensionDecl>(D) || isa<ImportDecl>(D)) &&
          !D->isImplicit()) {
        if (isa<AccessorDecl>(D))
          continue;

        // skip to visit ASTPrinter to avoid sil-opt prints duplicated import declarations
        if (auto importDecl = dyn_cast<ImportDecl>(D)) {
          StringRef importName = importDecl->getModule()->getName().str();
          if (importName == BUILTIN_NAME ||
              importName == STDLIB_NAME ||
              importName == SWIFT_SHIMS_NAME)
            continue;
        }
        D->print(OS, Options);
        OS << "\n\n";
      }
    }
  }

  printSILGlobals(PrintCtx, getSILGlobalList());
  printSILDifferentiabilityWitnesses(PrintCtx,
                                     getDifferentiabilityWitnessList());
  printSILLinearMapTypes(PrintCtx, getSwiftModule());
  printSILFunctions(PrintCtx, getFunctionList());
  printSILVTables(PrintCtx, getVTables());
  printSILWitnessTables(PrintCtx, getWitnessTableList());
  printSILDefaultWitnessTables(PrintCtx, getDefaultWitnessTableList());
  printSILCoverageMaps(PrintCtx, getCoverageMaps());
  printSILProperties(PrintCtx, getPropertyList());
  printSILMoveOnlyDeinits(PrintCtx, getMoveOnlyDeinits());
  printExternallyVisibleDecls(PrintCtx, externallyVisible.getArrayRef());

  if (M)
    printFileIDMap(
        PrintCtx, M->computeFileIDMap(/*shouldDiagnose=*/false));

  OS << "\n\n";
}

void SILNode::dumpInContext() const {
  printInContext(llvm::errs());
}
void SILNode::printInContext(llvm::raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  SILPrinter(Ctx).printInContext(this);
}

void SILInstruction::dumpInContext() const {
  printInContext(llvm::errs());
}
void SILInstruction::printInContext(llvm::raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  SILPrinter(Ctx).printInContext(asSILNode());
}

void SILVTableEntry::print(llvm::raw_ostream &OS) const {
  getMethod().print(OS);
  OS << ": ";

  PrintOptions QualifiedSILTypeOptions = PrintOptions::printQualifiedSILType();
  bool HasSingleImplementation = false;
  switch (getMethod().kind) {
  default:
    break;
  case SILDeclRef::Kind::IVarDestroyer:
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator:
    HasSingleImplementation = true;
  }
  // No need to emit the signature for methods that may have only
  // single implementation, e.g. for destructors.
  if (!HasSingleImplementation) {
    QualifiedSILTypeOptions.CurrentModule =
        getMethod().getDecl()->getDeclContext()->getParentModule();
    getMethod().getDecl()->getInterfaceType().print(
        OS, QualifiedSILTypeOptions);
    OS << " : ";
  }
  OS << '@' << getImplementation()->getName();
  switch (getKind()) {
  case SILVTable::Entry::Kind::Normal:
    break;
  case SILVTable::Entry::Kind::Inherited:
    OS << " [inherited]";
    break;
  case SILVTable::Entry::Kind::Override:
    OS << " [override]";
    break;
  }
  if (isNonOverridden()) {
    OS << " [nonoverridden]";
  }

  OS << "\t// " << demangleSymbol(getImplementation()->getName());
}

void SILVTable::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "sil_vtable ";
  if (isSerialized())
    OS << "[serialized] ";
  OS << getClass()->getName() << " {\n";

  for (auto &entry : getEntries()) {
    OS << "  ";
    entry.print(OS);
    OS << "\n";
  }
  OS << "}\n\n";
}

void SILVTable::dump() const { print(llvm::errs()); }

void SILMoveOnlyDeinit::print(llvm::raw_ostream &OS, bool verbose) const {
  OS << "sil_moveonlydeinit ";
  if (isSerialized())
    OS << "[serialized] ";
  OS << getNominalDecl()->getName() << " {\n";
  OS << "  @" << getImplementation()->getName();
  OS << "\t// " << demangleSymbol(getImplementation()->getName());
  OS << "\n";
  OS << "}\n\n";
}

void SILMoveOnlyDeinit::dump() const { print(llvm::errs(), false); }

/// Returns true if anything was printed.
static bool printAssociatedTypePath(llvm::raw_ostream &OS, CanType path) {
  if (auto memberType = dyn_cast<DependentMemberType>(path)) {
    if (printAssociatedTypePath(OS, memberType.getBase()))
      OS << '.';
    OS << memberType->getName().str();
    return true;
  } else {
    assert(isa<GenericTypeParamType>(path));
    return false;
  }
}

void SILWitnessTable::Entry::print(llvm::raw_ostream &out, bool verbose,
                                   const PrintOptions &options) const {
  PrintOptions QualifiedSILTypeOptions = PrintOptions::printQualifiedSILType();
  out << "  ";
  switch (getKind()) {
  case WitnessKind::Invalid:
    out << "no_default";
    break;
  case WitnessKind::Method: {
    // method #declref: @function
    auto &methodWitness = getMethodWitness();
    out << "method ";
    methodWitness.Requirement.print(out);
    out << ": ";
    QualifiedSILTypeOptions.CurrentModule =
        methodWitness.Requirement.getDecl()
            ->getDeclContext()
            ->getParentModule();
    methodWitness.Requirement.getDecl()->getInterfaceType().print(
        out, QualifiedSILTypeOptions);
    out << " : ";
    if (methodWitness.Witness) {
      methodWitness.Witness->printName(out);
      out << "\t// "
         << demangleSymbol(methodWitness.Witness->getName());
    } else {
      out << "nil";
    }
    break;
  }
  case WitnessKind::AssociatedType: {
    // associated_type AssociatedTypeName: ConformingType
    auto &assocWitness = getAssociatedTypeWitness();
    out << "associated_type ";
    out << assocWitness.Requirement->getName() << ": ";
    assocWitness.Witness->print(out, options);
    break;
  }
  case WitnessKind::AssociatedTypeProtocol: {
    // associated_type_protocol (AssociatedTypeName: Protocol): <conformance>
    auto &assocProtoWitness = getAssociatedTypeProtocolWitness();
    out << "associated_type_protocol (";
    (void) printAssociatedTypePath(out, assocProtoWitness.Requirement);
    out << ": " << assocProtoWitness.Protocol->getName() << "): ";
    if (assocProtoWitness.Witness.isConcrete())
      assocProtoWitness.Witness.getConcrete()->printName(out, options);
    else
      out << "dependent";
    break;
  }
  case WitnessKind::BaseProtocol: {
    // base_protocol Protocol: <conformance>
    auto &baseProtoWitness = getBaseProtocolWitness();
    out << "base_protocol "
        << baseProtoWitness.Requirement->getName() << ": ";
    baseProtoWitness.Witness->printName(out, options);
    break;
  }
  }
  out << '\n';
}

void SILWitnessTable::print(llvm::raw_ostream &OS, bool Verbose) const {
  PrintOptions Options = PrintOptions::printSIL();
  PrintOptions QualifiedSILTypeOptions = PrintOptions::printQualifiedSILType();
  OS << "sil_witness_table ";
  printLinkage(OS, getLinkage(), /*isDefinition*/ isDefinition());
  if (isSerialized())
    OS << "[serialized] ";

  getConformance()->printName(OS, Options);
  Options.GenericSig =
    getConformance()->getDeclContext()->getGenericSignatureOfContext()
      .getPointer();

  if (isDeclaration()) {
    OS << "\n\n";
    return;
  }

  OS << " {\n";
  
  for (auto &witness : getEntries()) {
    witness.print(OS, Verbose, Options);
  }

  for (auto conditionalConformance : getConditionalConformances()) {
    // conditional_conformance (TypeName: Protocol):
    // <conformance>
    OS << "  conditional_conformance (";
    conditionalConformance.Requirement.print(OS, Options);
    OS << ": " << conditionalConformance.Conformance.getRequirement()->getName()
       << "): ";
    if (conditionalConformance.Conformance.isConcrete())
      conditionalConformance.Conformance.getConcrete()->printName(OS, Options);
    else
      OS << "dependent";

    OS << '\n';
  }

  OS << "}\n\n";
}

void SILWitnessTable::dump() const {
  print(llvm::errs());
}

void SILDefaultWitnessTable::print(llvm::raw_ostream &OS, bool Verbose) const {
  // sil_default_witness_table [<Linkage>] <Protocol> <MinSize>
  PrintOptions QualifiedSILTypeOptions = PrintOptions::printQualifiedSILType();
  OS << "sil_default_witness_table ";
  printLinkage(OS, getLinkage(), ForDefinition);
  OS << getProtocol()->getName() << " {\n";
  
  PrintOptions options = PrintOptions::printSIL();
  options.GenericSig = Protocol->getGenericSignatureOfContext().getPointer();

  for (auto &witness : getEntries()) {
    witness.print(OS, Verbose, options);
  }
  
  OS << "}\n\n";
}

void SILDefaultWitnessTable::dump() const {
  print(llvm::errs());
}

void SILDifferentiabilityWitness::print(llvm::raw_ostream &OS,
                                        bool verbose) const {
  OS << "// differentiability witness for "
     << demangleSymbol(getOriginalFunction()->getName()) << '\n';
  PrintOptions qualifiedSILTypeOptions = PrintOptions::printQualifiedSILType();
  // sil_differentiability_witness (linkage)?
  OS << "sil_differentiability_witness ";
  printLinkage(OS, getLinkage(), /*isDefinition*/ isDefinition());
  // ([serialized])?
  if (isSerialized())
    OS << "[serialized] ";
  // Kind
  OS << '[';
  switch (getKind()) {
  case DifferentiabilityKind::Forward:
    OS << "forward";
    break;
  case DifferentiabilityKind::Reverse:
    OS << "reverse";
    break;
  case DifferentiabilityKind::Normal:
    OS << "normal";
    break;
  case DifferentiabilityKind::Linear:
    OS << "linear";
    break;
  case DifferentiabilityKind::NonDifferentiable:
    llvm_unreachable("Impossible case");
  }
  // [parameters ...]
  OS << "] [parameters ";
  interleave(
      getParameterIndices()->getIndices(), [&](unsigned index) { OS << index; },
      [&] { OS << ' '; });
  // [results ...]
  OS << "] [results ";
  interleave(
      getResultIndices()->getIndices(), [&](unsigned index) { OS << index; },
      [&] { OS << ' '; });
  OS << "] ";
  // (<...>)?
  if (auto derivativeGenSig = getDerivativeGenericSignature()) {
    auto subPrinter = PrintOptions::printSIL();
    derivativeGenSig->print(OS, subPrinter);
    OS << " ";
  }
  // @original-function-name : $original-sil-type
  printSILFunctionNameAndType(OS, getOriginalFunction());

  if (isDeclaration()) {
    OS << "\n\n";
    return;
  }

  // {
  //   jvp: @jvp-function-name : $jvp-sil-type
  //   vjp: @vjp-function-name : $vjp-sil-type
  // }
  OS << " {\n";
  if (auto *jvp = getJVP()) {
    OS << "  jvp: ";
    printSILFunctionNameAndType(OS, jvp);
    OS << '\n';
  }
  if (auto *vjp = getVJP()) {
    OS << "  vjp: ";
    printSILFunctionNameAndType(OS, vjp);
    OS << '\n';
  }
  OS << "}\n\n";
}

void SILDifferentiabilityWitness::dump() const {
  print(llvm::errs());
}

void SILCoverageMap::print(SILPrintContext &PrintCtx) const {
  llvm::raw_ostream &OS = PrintCtx.OS();
  OS << "sil_coverage_map " << QuotedString(getFilename()) << " "
     << QuotedString(getName()) << " " << QuotedString(getPGOFuncName()) << " "
     << getHash() << " {\t// " << demangleSymbol(getName()) << "\n";
  if (PrintCtx.sortSIL())
    std::sort(MappedRegions.begin(), MappedRegions.end(),
              [](const MappedRegion &LHS, const MappedRegion &RHS) {
      return std::tie(LHS.StartLine, LHS.StartCol, LHS.EndLine, LHS.EndCol) <
             std::tie(RHS.StartLine, RHS.StartCol, RHS.EndLine, RHS.EndCol);
    });
  for (auto &MR : getMappedRegions()) {
    OS << "  " << MR.StartLine << ":" << MR.StartCol << " -> " << MR.EndLine
       << ":" << MR.EndCol << " : ";
    printCounter(OS, MR.Counter);
    OS << "\n";
  }
  OS << "}\n\n";
}

void SILCoverageMap::dump() const {
  print(llvm::errs());
}

#ifndef NDEBUG
// Disable the "for use only in debugger" warning.
#if SWIFT_COMPILER_IS_MSVC
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

void SILDebugScope::print(SourceManager &SM, llvm::raw_ostream &OS,
                         unsigned Indent) const {
  OS << "{\n";
  OS.indent(Indent);
  if (Loc.isASTNode())
    Loc.getSourceLoc().print(OS, SM);
  OS << "\n";
  OS.indent(Indent + 2);
  OS << " parent: ";
  if (auto *P = Parent.dyn_cast<const SILDebugScope *>()) {
    P->print(SM, OS, Indent + 2);
    OS.indent(Indent + 2);
  }
  else if (auto *F = Parent.dyn_cast<SILFunction *>())
    OS << "@" << F->getName();
  else
    OS << "nullptr";

  OS << "\n";
  OS.indent(Indent + 2);
  if (auto *CS = InlinedCallSite) {
    OS << "inlinedCallSite: ";
    CS->print(SM, OS, Indent + 2);
    OS.indent(Indent + 2);
  }
  OS << "}\n";
}

void SILDebugScope::print(SILModule &Mod) const {
  // We just use the default indent and llvm::errs().
  print(Mod.getASTContext().SourceMgr);
}

#if SWIFT_COMPILER_IS_MSVC
#pragma warning(pop)
#endif

#endif

void SILSpecializeAttr::print(llvm::raw_ostream &OS) const {
  SILPrintContext Ctx(OS);
  // Print other types as their Swift representation.
  PrintOptions SubPrinter = PrintOptions::printSIL();
  auto exported = isExported() ? "true" : "false";
  auto kind = isPartialSpecialization() ? "partial" : "full";

  OS << "exported: " << exported << ", ";
  OS << "kind: " << kind << ", ";
  if (!getSPIGroup().empty()) {
    OS << "spi: " << getSPIGroup() << ", ";
    OS << "spiModule: ";
    getSPIModule()->getReverseFullModuleName().printForward(OS);
    OS << ", ";
  }

  auto *genericEnv = getFunction()->getGenericEnvironment();
  GenericSignature genericSig;
  if (genericEnv)
    genericSig = genericEnv->getGenericSignature();

  auto requirements =
      getUnerasedSpecializedSignature().requirementsNotSatisfiedBy(genericSig);
  if (targetFunction) {
    OS << "target: \"" << targetFunction->getName() << "\", ";
  }
 if (!availability.isAlwaysAvailable()) {
    auto version = availability.getOSVersion().getLowerEndpoint();
    OS << "available: " << version.getAsString() << ", ";
  }
  if (!requirements.empty()) {
    OS << "where ";
    SILFunction *F = getFunction();
    assert(F);
    interleave(
        requirements,
        [&](Requirement req) {
          if (!genericSig) {
            req.print(OS, SubPrinter);
            return;
          }

          // Use GenericEnvironment to produce user-friendly
          // names instead of something like t_0_0.
          auto FirstTy = genericSig->getSugaredType(req.getFirstType());
          auto erasedParams = getTypeErasedParams();
          bool erased = std::any_of(erasedParams.begin(), erasedParams.end(),
              [&](auto Ty) {
            return Ty->isEqual(FirstTy);
          });
          if (erased) {
            OS << " @_noMetadata ";
          }
          if (req.getKind() != RequirementKind::Layout) {
            auto SecondTy = genericSig->getSugaredType(req.getSecondType());
            Requirement ReqWithDecls(req.getKind(), FirstTy, SecondTy);
            ReqWithDecls.print(OS, SubPrinter);
          } else {
            Requirement ReqWithDecls(req.getKind(), FirstTy,
                                     req.getLayoutConstraint());
            auto SubPrinterCopy = SubPrinter;
            SubPrinterCopy.PrintClassLayoutName = erased;
            ReqWithDecls.print(OS, SubPrinterCopy);
          }
        },
        [&] { OS << ", "; });
  }
}

void KeyPathPatternComponent::print(SILPrintContext &ctxt) const {
  SILPrinter printer(ctxt);
  printer.printKeyPathPatternComponent(*this);
}

//===----------------------------------------------------------------------===//
// SILPrintContext members
//===----------------------------------------------------------------------===//

SILPrintContext::SILPrintContext(llvm::raw_ostream &OS, bool Verbose,
                                 bool SortedSIL, bool PrintFullConvention)
    : OutStream(OS), Verbose(Verbose), SortedSIL(SortedSIL),
      DebugInfo(SILPrintDebugInfo), PrintFullConvention(PrintFullConvention) {}

SILPrintContext::SILPrintContext(llvm::raw_ostream &OS, const SILOptions &Opts)
    : OutStream(OS), Verbose(Opts.EmitVerboseSIL),
      SortedSIL(Opts.EmitSortedSIL),
      DebugInfo(Opts.PrintDebugInfo || SILPrintDebugInfo),
      PrintFullConvention(Opts.PrintFullConvention) {}

SILPrintContext::SILPrintContext(llvm::raw_ostream &OS, bool Verbose,
                                 bool SortedSIL, bool DebugInfo,
                                 bool PrintFullConvention)
    : OutStream(OS), Verbose(Verbose), SortedSIL(SortedSIL),
      DebugInfo(DebugInfo), PrintFullConvention(PrintFullConvention) {}

void SILPrintContext::setContext(const void *FunctionOrBlock) {
  if (FunctionOrBlock != ContextFunctionOrBlock) {
    BlocksToIDMap.clear();
    ValueToIDMap.clear();
    ContextFunctionOrBlock = FunctionOrBlock;
  }
}

SILPrintContext::~SILPrintContext() {
}

void SILPrintContext::printInstructionCallBack(const SILInstruction *I) {
}

void SILPrintContext::initBlockIDs(ArrayRef<const SILBasicBlock *> Blocks) {
  if (Blocks.empty())
    return;

  setContext(Blocks[0]->getParent());

  // Initialize IDs so our IDs are in RPOT as well. This is a hack.
  for (unsigned Index : indices(Blocks))
    BlocksToIDMap[Blocks[Index]] = Index;
}

ID SILPrintContext::getID(const SILBasicBlock *Block) {
  setContext(Block->getParent());

  // Lazily initialize the Blocks-to-IDs mapping.
  // If we are asked to emit sorted SIL, print out our BBs in RPOT order.
  if (BlocksToIDMap.empty()) {
    if (sortSIL()) {
      std::vector<SILBasicBlock *> RPOT;
      auto *UnsafeF = const_cast<SILFunction *>(Block->getParent());
      std::copy(po_begin(UnsafeF), po_end(UnsafeF), std::back_inserter(RPOT));
      std::reverse(RPOT.begin(), RPOT.end());
      // Initialize IDs so our IDs are in RPOT as well. This is a hack.
      for (unsigned Index : indices(RPOT))
        BlocksToIDMap[RPOT[Index]] = Index;
    } else {
      unsigned idx = 0;
      for (const SILBasicBlock &B : *Block->getParent())
        BlocksToIDMap[&B] = idx++;
    }
  }
  ID R = {ID::SILBasicBlock, BlocksToIDMap[Block]};
  return R;
}

ID SILPrintContext::getID(SILNodePointer node) {
  if (node == nullptr)
    return {ID::Null, ~0U};

  if (isa<SILUndef>(node.get()))
    return {ID::SILUndef, 0};
  
  SILBasicBlock *BB = node->getParentBlock();
  if (!BB) {
    return { ID::Null, 0 };
  }
  if (SILFunction *F = BB->getParent()) {
    setContext(F);
    // Lazily initialize the instruction -> ID mapping.
    if (ValueToIDMap.empty())
      F->numberValues(ValueToIDMap);
    ID R = {ID::SSAValue, ValueToIDMap[node]};
    return R;
  }

  setContext(BB);

  // Check if we have initialized our ValueToIDMap yet. If we have, just use
  // that.
  if (!ValueToIDMap.empty()) {
    ID R = {ID::SSAValue, ValueToIDMap[node]};
    return R;
  }

  // Otherwise, initialize the instruction -> ID mapping cache.
  unsigned idx = 0;
  for (auto &I : *BB) {
    // Give the instruction itself the next ID.
    ValueToIDMap[I.asSILNode()] = idx;

    // If there are no results, make sure we don't reuse that ID.
    auto results = I.getResults();
    if (results.empty()) {
      ++idx;
      continue;
    }

    // Otherwise, assign all of the results an index.  Note that
    // we'll assign the same ID to both the instruction and the
    // first result.
    for (auto result : results) {
      ValueToIDMap[result] = idx++;
    }
  }

  ID R = {ID::SSAValue, ValueToIDMap[node]};
  return R;
}

PrintOptions PrintOptions::printSIL(const SILPrintContext *ctx) {
  PrintOptions result;
  result.PrintLongAttrsOnSeparateLines = true;
  result.PrintStorageRepresentationAttrs = true;
  result.AbstractAccessors = false;
  result.PrintForSIL = true;
  result.PrintInSILBody = true;
  result.PreferTypeRepr = false;
  result.PrintIfConfig = false;
  result.OpaqueReturnTypePrinting =
     OpaqueReturnTypePrintingMode::StableReference;
  if (ctx && ctx->printFullConvention())
   result.PrintFunctionRepresentationAttrs =
       PrintOptions::FunctionRepresentationMode::Full;
  return result;
}
