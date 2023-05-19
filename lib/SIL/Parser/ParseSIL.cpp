//===--- ParseSIL.cpp - SIL File Parsing logic ----------------------------===//
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

#include "SILParserFunctionBuilder.h"
#include "SILParserState.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/ParseSILSupport.h"
#include "swift/Parse/Parser.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Sema/SILTypeResolutionContext.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

static llvm::cl::opt<bool>
ParseSerializedSIL("parse-serialized-sil",
                   llvm::cl::desc("Parse the output of a serialized module"));

static llvm::cl::opt<bool>
    DisableInputVerify("sil-disable-input-verify",
                       llvm::cl::desc("Disable verification of input SIL"),
                       llvm::cl::init(false));

// Option for testing -silgen-cleanup -enable-complete-ossa
static llvm::cl::opt<bool>
ParseIncompleteOSSA("parse-incomplete-ossa",
                    llvm::cl::desc("Parse OSSA with incomplete lifetimes"));

static llvm::cl::opt<bool> DisablePopulateOwnershipFlags(
    "disable-populate-ownership-flags",
    llvm::cl::desc("Disable populating ownership flags"),
    llvm::cl::init(false));

//===----------------------------------------------------------------------===//
// SILParserState implementation
//===----------------------------------------------------------------------===//

SILParserState::~SILParserState() {
  if (!ForwardRefFns.empty()) {
    for (auto Entry : ForwardRefFns) {
      if (Entry.second.Loc.isValid()) {
        M.getASTContext().Diags.diagnose(Entry.second.Loc,
                                         diag::sil_use_of_undefined_value,
                                         Entry.first.str());
      }
    }
  }

  // Turn any debug-info-only function declarations into zombies.
  for (auto *Fn : PotentialZombieFns)
    if (Fn->isExternalDeclaration()) {
      Fn->setInlined();
      M.eraseFunction(Fn);
    }
}

std::unique_ptr<SILModule>
ParseSILModuleRequest::evaluate(Evaluator &evaluator,
                                ASTLoweringDescriptor desc) const {
  auto *SF = desc.getSourceFileToParse();
  assert(SF);

  auto bufferID = SF->getBufferID();
  assert(bufferID);

  // For leak detection.
  SILInstruction::resetInstructionCounts();

  auto silMod = SILModule::createEmptyModule(desc.context, desc.conv,
                                             desc.opts);
  SILParserState parserState(*silMod.get());
  Parser parser(*bufferID, *SF, &parserState);
  PrettyStackTraceParser StackTrace(parser);

  if (ParseSerializedSIL) {
    silMod.get()->setParsedAsSerializedSIL();
  }

  auto hadError = parser.parseTopLevelSIL();
  if (hadError) {
    // The rest of the SIL pipeline expects well-formed SIL, so if we encounter
    // a parsing error, just return an empty SIL module.
    //
    // Because the SIL parser's notion of failing with an error is distinct from
    // the ASTContext's notion of having emitted a diagnostic, it's possible for
    // the parser to fail silently without emitting a diagnostic. This assertion
    // ensures that +asserts builds will fail fast. If you crash here, please go
    // back and add a diagnostic after identifying where the SIL parser failed.
    assert(SF->getASTContext().hadError() &&
           "Failed to parse SIL but did not emit any errors!");
    return SILModule::createEmptyModule(desc.context, desc.conv, desc.opts);
  }
  // If SIL parsing succeeded, verify the generated SIL.
  if (!parser.Diags.hadAnyError() && !DisableInputVerify) {
    silMod->verify(/*SingleFunction=*/true, !ParseIncompleteOSSA);
  }

  return silMod;
}

//===----------------------------------------------------------------------===//
// SILParser
//===----------------------------------------------------------------------===//

namespace {
  struct ParsedSubstitution {
    SourceLoc loc;
    Type replacement;
  };

  struct ParsedSpecAttr {
    ArrayRef<RequirementRepr> requirements;
    bool exported;
    SILSpecializeAttr::SpecializationKind kind;
    SILFunction *target = nullptr;
    Identifier spiGroupID;
    ModuleDecl *spiModule;
    AvailabilityContext availability = AvailabilityContext::alwaysAvailable();
  };
} // namespace

namespace swift {
  /// The parser for an individual SIL function.
  class SILParser {
    friend SILParserState;
  public:
    Parser &P;
    SILModule &SILMod;
    SILParserState &TUState;
    SILFunction *F = nullptr;
    GenericSignature ContextGenericSig;
    GenericParamList *ContextGenericParams = nullptr;

  private:
    /// HadError - Have we seen an error parsing this function?
    bool HadError = false;

    /// Transient state for parsing a multiple optional attributes
    /// in parseSpecificSILInstruction:
    ///   <regular syntax>[, keyword1][, keyword2]
    bool parsedComma = false;

    /// Data structures used to perform name lookup of basic blocks.
    llvm::DenseMap<Identifier, SILBasicBlock*> BlocksByName;
    llvm::DenseMap<SILBasicBlock*,
                   Located<Identifier>> UndefinedBlocks;

    /// The set of opened packs in the function, indexed by UUID.
    /// Note that we don't currently support parsing references to
    /// opened packs prior to their instruction, although this is
    /// theoretically possible if basic blocks are not sorted in
    /// dominance order.
    SILTypeResolutionContext::OpenedPackElementsMap OpenedPackElements;

    /// Data structures used to perform name lookup for local values.
    llvm::StringMap<ValueBase*> LocalValues;
    llvm::StringMap<SourceLoc> ForwardRefLocalValues;

    Type performTypeResolution(TypeRepr *TyR, bool IsSILType,
                               GenericSignature GenericSig,
                               GenericParamList *GenericParams);

    void convertRequirements(ArrayRef<RequirementRepr> From,
                             SmallVectorImpl<Requirement> &To,
                             SmallVectorImpl<Type> &typeErasedParams);

    ProtocolConformanceRef parseProtocolConformanceHelper(
        ProtocolDecl *&proto,
        GenericSignature GenericSig,
        GenericParamList *WitnessParams);

  public:
    SILParser(Parser &P)
        : P(P), SILMod(static_cast<SILParserState *>(P.SIL)->M),
          TUState(*static_cast<SILParserState *>(P.SIL)) {}

    ~SILParser();

    /// diagnoseProblems - After a function is fully parse, emit any diagnostics
    /// for errors and return true if there were any.
    bool diagnoseProblems();

    /// getGlobalNameForReference - Given a reference to a global name, look it
    /// up and return an appropriate SIL function.
    SILFunction *getGlobalNameForReference(Identifier Name,
                                           CanSILFunctionType Ty,
                                           SourceLoc Loc,
                                           bool IgnoreFwdRef = false);
    /// getGlobalNameForDefinition - Given a definition of a global name, look
    /// it up and return an appropriate SIL function.
    SILFunction *getGlobalNameForDefinition(Identifier Name,
                                            CanSILFunctionType Ty,
                                            SourceLoc Loc);

    /// getBBForDefinition - Return the SILBasicBlock for a definition of the
    /// specified block.
    SILBasicBlock *getBBForDefinition(Identifier Name, SourceLoc Loc);
    
    /// getBBForReference - return the SILBasicBlock of the specified name.  The
    /// source location is used to diagnose a failure if the block ends up never
    /// being defined.
    SILBasicBlock *getBBForReference(Identifier Name, SourceLoc Loc);

    struct UnresolvedValueName {
      StringRef Name;
      SourceLoc NameLoc;

      bool isUndef() const { return Name == "undef"; }
    };

    /// getLocalValue - Get a reference to a local value with the specified name
    /// and type.
    SILValue getLocalValue(UnresolvedValueName Name, SILType Type,
                           SILLocation L, SILBuilder &B);

    /// setLocalValue - When an instruction or block argument is defined, this
    /// method is used to register it and update our symbol table.
    void setLocalValue(ValueBase *Value, StringRef Name, SourceLoc NameLoc);

    SILDebugLocation getDebugLoc(SILBuilder & B, SILLocation Loc) {
      return SILDebugLocation(Loc, F->getDebugScope());
    }

    /// @{ Primitive parsing.

    /// \verbatim
    ///   sil-identifier ::= [A-Za-z_0-9]+
    /// \endverbatim
    bool parseSILIdentifier(Identifier &Result, SourceLoc &Loc,
                            const Diagnostic &D);

    template<typename ...DiagArgTypes, typename ...ArgTypes>
    bool parseSILIdentifier(Identifier &Result, Diag<DiagArgTypes...> ID,
                            ArgTypes... Args) {
      SourceLoc L;
      return parseSILIdentifier(Result, L, Diagnostic(ID, Args...));
    }

    template <typename T, typename... DiagArgTypes, typename... ArgTypes>
    bool parseSILIdentifierSwitch(T &Result, ArrayRef<StringRef> Strings,
                                  Diag<DiagArgTypes...> ID, ArgTypes... Args) {
      Identifier TmpResult;
      SourceLoc L;
      if (parseSILIdentifier(TmpResult, L, Diagnostic(ID, Args...))) {
        return true;
      }

      auto Iter = std::find(Strings.begin(), Strings.end(), TmpResult.str());
      if (Iter == Strings.end()) {
        P.diagnose(P.Tok, Diagnostic(ID, Args...));
        return true;
      }

      Result = T(*Iter);
      return false;
    }

    template<typename ...DiagArgTypes, typename ...ArgTypes>
    bool parseSILIdentifier(Identifier &Result, SourceLoc &L,
                            Diag<DiagArgTypes...> ID, ArgTypes... Args) {
      return parseSILIdentifier(Result, L, Diagnostic(ID, Args...));
    }

    template <typename T>
    bool
    parseSILQualifier(Optional<T> &result,
                      llvm::function_ref<Optional<T>(StringRef)> parseName);

    bool parseVerbatim(StringRef identifier);

    template <typename T>
    bool parseInteger(T &Result, const Diagnostic &D) {
      if (!P.Tok.is(tok::integer_literal)) {
        P.diagnose(P.Tok, D);
        return true;
      }
      bool error = parseIntegerLiteral(P.Tok.getText(), 0, Result);
      P.consumeToken(tok::integer_literal);
      return error;
    }

    template <typename T>
    bool parseIntegerLiteral(StringRef text, unsigned radix, T &result) {
      text = prepareIntegerLiteralForParsing(text);
      return text.getAsInteger(radix, result);
    }

    StringRef prepareIntegerLiteralForParsing(StringRef text) {
      // tok::integer_literal can contain characters that the library
      // parsing routines don't expect.
      if (text.contains('_'))
        text = P.copyAndStripUnderscores(text);
      return text;
    }

    /// @}

    /// @{ Type parsing.
    bool parseASTType(CanType &result,
                      GenericSignature genericSig=GenericSignature(),
                      GenericParamList *genericParams=nullptr,
                      bool forceContextualType = false);
    bool parseASTType(CanType &result,
                      SourceLoc &TypeLoc,
                      GenericSignature genericSig=GenericSignature(),
                      GenericParamList *genericParams=nullptr,
                      bool forceContextualType = false) {
      TypeLoc = P.Tok.getLoc();
      return parseASTType(result, genericSig, genericParams,
                          forceContextualType);
    }

    bool parseASTPackType(CanPackType &result) {
      SourceLoc loc;
      CanType rawType;
      if (parseASTType(rawType, loc))
        return true;
      result = dyn_cast<PackType>(rawType);
      if (!result) {
        P.diagnose(loc, diag::expected_sil_type_kind, "match $Pack{...}");
        return true;
      }
      return false;
    }

    Optional<StringRef> parseOptionalAttribute(ArrayRef<StringRef> expected) {
      // We parse here @ <identifier>.
      if (P.Tok.getKind() != tok::at_sign)
        return llvm::None;

      auto name = P.peekToken().getText();
      if (!is_contained(expected, name))
        return llvm::None;

      // Ok, we can do this.
      P.consumeToken(tok::at_sign);
      P.consumeToken(tok::identifier);
      return name;
    }

    bool parseSILOwnership(ValueOwnershipKind &OwnershipKind) {
      // We parse here @ <identifier>.
      if (!P.consumeIf(tok::at_sign)) {
        // If we fail, we must have @any ownership. We check elsewhere in the
        // parser that this matches what the function signature wants.
        OwnershipKind = OwnershipKind::None;
        return false;
      }

      StringRef AllOwnershipKinds[4] = {"unowned", "owned", "guaranteed",
                                        "none"};
      return parseSILIdentifierSwitch(OwnershipKind, AllOwnershipKinds,
                                      diag::expected_sil_value_ownership_kind);
    }
    void bindSILGenericParams(TypeRepr *TyR);
    bool parseSILType(SILType &Result,
                      GenericSignature &parsedGenericSig,
                      GenericParamList *&parsedGenericParams,
                      bool IsFuncDecl=false,
                      GenericSignature parentGenericSig=GenericSignature(),
                      GenericParamList *parentGenericParams=nullptr);
    bool parseSILType(SILType &Result) {
      GenericSignature IgnoredSig;
      GenericParamList *IgnoredParams = nullptr;
      return parseSILType(Result, IgnoredSig, IgnoredParams);
    }
    bool parseSILType(SILType &Result, SourceLoc &TypeLoc) {
      TypeLoc = P.Tok.getLoc();
      return parseSILType(Result);
    }
    bool parseSILType(SILType &Result, SourceLoc &TypeLoc,
                      GenericSignature &parsedGenericSig,
                      GenericParamList *&parsedGenericParams,
                      GenericSignature parentGenericSig=GenericSignature(),
                      GenericParamList *parentGenericParams=nullptr) {
      TypeLoc = P.Tok.getLoc();
      return parseSILType(Result, parsedGenericSig, parsedGenericParams,
                          false, parentGenericSig, parentGenericParams);
    }
    /// @}

    bool parseSILDottedPath(ValueDecl *&Decl,
                            SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDottedPath(ValueDecl *&Decl) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDottedPath(Decl, values);
    }
    bool parseSILDottedPathWithoutPound(ValueDecl *&Decl,
                                        SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDottedPathWithoutPound(ValueDecl *&Decl) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDottedPathWithoutPound(Decl, values);
    }
    /// At the time of calling this function, we may not have the type of the
    /// Decl yet. So we return a SILDeclRef on the first lookup result and also
    /// return all the lookup results. After parsing the expected type, the
    /// caller of this function can choose the one that has the expected type.
    bool parseSILDeclRef(SILDeclRef &Result,
                         SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDeclRef(SILDeclRef &Result) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDeclRef(Result, values);
    }
    bool parseSILDeclRef(SILDeclRef &Member, bool FnTypeRequired);
    bool parseGlobalName(Identifier &Name);
    bool parseValueName(UnresolvedValueName &Name);
    bool parseValueRef(SILValue &Result, SILType Ty, SILLocation Loc,
                       SILBuilder &B);
    bool parseTypedValueRef(SILValue &Result, SourceLoc &Loc, SILBuilder &B);
    bool parseTypedValueRef(SILValue &Result, SILBuilder &B) {
      SourceLoc Tmp;
      return parseTypedValueRef(Result, Tmp, B);
    }
    bool parseSILOpcode(SILInstructionKind &Opcode, SourceLoc &OpcodeLoc,
                        StringRef &OpcodeName);
    bool parseSILDebugVar(SILDebugVariable &Var);

    bool parseSILDebugInfoExpression(SILDebugInfoExpression &DIExpr);

    /// Parses the basic block arguments as part of branch instruction.
    bool parseSILBBArgsAtBranch(SmallVector<SILValue, 6> &Args, SILBuilder &B);

    bool parseSILLocation(SILLocation &L);
    bool parseScopeRef(SILDebugScope *&DS);
    bool parseForwardingOwnershipKind(ValueOwnershipKind &forwardingKind);
    bool parseSILDebugLocation(SILLocation &L, SILBuilder &B);
    bool parseSpecificSILInstruction(SILBuilder &B, SILInstructionKind Opcode,
                                     SourceLoc OpcodeLoc, StringRef OpcodeName,
                                     SILInstruction *&ResultVal);

    bool parseSILInstruction(SILBuilder &B);
    bool parseCallInstruction(SILLocation InstLoc,
                              SILInstructionKind Opcode, SILBuilder &B,
                              SILInstruction *&ResultVal);
    bool parseSILFunctionRef(SILLocation InstLoc, SILFunction *&ResultFn);

    bool parseSILBasicBlock(SILBuilder &B);
    bool parseKeyPathPatternComponent(KeyPathPatternComponent &component,
                                      SmallVectorImpl<SILType> &operandTypes,
                                      SourceLoc componentLoc,
                                      Identifier componentKind,
                                      SILLocation InstLoc,
                                      GenericSignature patternSig,
                                      GenericParamList *patternParams);
    bool isStartOfSILInstruction();

    bool parseSubstitutions(SmallVectorImpl<ParsedSubstitution> &parsed,
                            GenericSignature GenericSig=GenericSignature(),
                            GenericParamList *GenericParams=nullptr);

    ProtocolConformanceRef parseProtocolConformance(
        ProtocolDecl *&proto,
        GenericSignature &genericSig,
        GenericParamList *&genericParams);
    ProtocolConformanceRef
    parseProtocolConformance() {
      ProtocolDecl *dummy = nullptr;
      GenericSignature genericSig;
      GenericParamList *genericParams = nullptr;
      return parseProtocolConformance(dummy, genericSig, genericParams);
    }

    Optional<llvm::coverage::Counter>
    parseSILCoverageExpr(llvm::coverage::CounterExpressionBuilder &Builder);

    template <class T>
    struct ParsedEnum {
      Optional<T> Value;
      StringRef Name;
      SourceLoc Loc;

      bool isSet() const { return Value.has_value(); }
      T operator*() const { return *Value; }
    };

    template <class T>
    void setEnum(ParsedEnum<T> &existing,
                 T value, StringRef name, SourceLoc loc) {
      if (existing.Value) {
        if (*existing.Value == value) {
          P.diagnose(loc, diag::duplicate_attribute, /*modifier*/ 1);
        } else {
          P.diagnose(loc, diag::mutually_exclusive_attrs, name,
                     existing.Name, /*modifier*/ 1);
        }
        P.diagnose(existing.Loc, diag::previous_attribute, /*modifier*/ 1);
      }
      existing.Value = value;
      existing.Name = name;
      existing.Loc = loc;
    }

    template <class T>
    void maybeSetEnum(bool allowed, ParsedEnum<T> &existing,
                      T value, StringRef name, SourceLoc loc) {
      if (allowed)
        setEnum(existing, value, name, loc);
      else
        P.diagnose(loc, diag::unknown_attribute, name);
    }
  };
} // namespace swift

bool SILParser::parseSILIdentifier(Identifier &Result, SourceLoc &Loc,
                                   const Diagnostic &D) {
  switch (P.Tok.getKind()) {
  case tok::identifier:
  case tok::dollarident:
    Result = P.Context.getIdentifier(P.Tok.getText());
    break;
  case tok::string_literal: {
    // Drop the double quotes.
    StringRef rawString = P.Tok.getText().drop_front().drop_back();
    Result = P.Context.getIdentifier(rawString);
    break;
  }
  case tok::oper_binary_unspaced:  // fixme?
  case tok::oper_binary_spaced:
  case tok::kw_init:
    // A binary operator or `init` can be part of a SILDeclRef.
    Result = P.Context.getIdentifier(P.Tok.getText());
    break;
  default:
    // If it's some other keyword, grab an identifier for it.
    if (P.Tok.isKeyword()) {
      Result = P.Context.getIdentifier(P.Tok.getText());
      break;
    }
    P.diagnose(P.Tok, D);
    return true;
  }

  Loc = P.Tok.getLoc();
  P.consumeToken();
  return false;
}

bool SILParser::parseVerbatim(StringRef name) {
  Identifier tok;
  SourceLoc loc;

  if (parseSILIdentifier(tok, loc, diag::expected_tok_in_sil_instr, name)) {
    return true;
  }
  if (tok.str() != name) {
    P.diagnose(loc, diag::expected_tok_in_sil_instr, name);
    return true;
  }
  return false;
}

SILParser::~SILParser() {
  for (auto &Entry : ForwardRefLocalValues) {
    if (ValueBase *dummyVal = LocalValues[Entry.first()]) {
      dummyVal->replaceAllUsesWith(SILUndef::get(dummyVal->getType(), SILMod));
      ::delete cast<PlaceholderValue>(dummyVal);
    }
  }
}


/// diagnoseProblems - After a function is fully parse, emit any diagnostics
/// for errors and return true if there were any.
bool SILParser::diagnoseProblems() {
  // Check for any uses of basic blocks that were not defined.
  if (!UndefinedBlocks.empty()) {
    // FIXME: These are going to come out in nondeterministic order.
    for (auto Entry : UndefinedBlocks)
      P.diagnose(Entry.second.Loc, diag::sil_undefined_basicblock_use,
                 Entry.second.Item);

    HadError = true;
  }
  
  if (!ForwardRefLocalValues.empty()) {
    // FIXME: These are going to come out in nondeterministic order.
    for (auto &Entry : ForwardRefLocalValues)
      P.diagnose(Entry.second, diag::sil_use_of_undefined_value,
                 Entry.first());
    HadError = true;
  }
  
  return HadError;
}

/// getGlobalNameForDefinition - Given a definition of a global name, look
/// it up and return an appropriate SIL function.
SILFunction *SILParser::getGlobalNameForDefinition(Identifier name,
                                                   CanSILFunctionType ty,
                                                   SourceLoc sourceLoc) {
  SILParserFunctionBuilder builder(SILMod);
  auto silLoc = RegularLocation(sourceLoc);

  // Check to see if a function of this name has been forward referenced.  If so
  // complete the forward reference.
  auto iter = TUState.ForwardRefFns.find(name);
  if (iter != TUState.ForwardRefFns.end()) {
    SILFunction *fn = iter->second.Item;

    // Verify that the types match up.
    if (fn->getLoweredFunctionType() != ty) {
      P.diagnose(sourceLoc, diag::sil_value_use_type_mismatch, name.str(),
                 fn->getLoweredFunctionType(), ty);
      P.diagnose(iter->second.Loc, diag::sil_prior_reference);
      fn = builder.createFunctionForForwardReference("" /*name*/, ty, silLoc);
    }

    assert(fn->isExternalDeclaration() && "Forward defns cannot have bodies!");
    TUState.ForwardRefFns.erase(iter);

    // Move the function to this position in the module.
    //
    // FIXME: Should we move this functionality into SILParserFunctionBuilder?
    SILMod.getFunctionList().remove(fn);
    SILMod.getFunctionList().push_back(fn);

    return fn;
  }

  // If we don't have a forward reference, make sure the function hasn't been
  // defined already.
  if (SILMod.lookUpFunction(name.str()) != nullptr) {
    P.diagnose(sourceLoc, diag::sil_value_redefinition, name.str());
    return builder.createFunctionForForwardReference("" /*name*/, ty, silLoc);
  }

  // Otherwise, this definition is the first use of this name.
  return builder.createFunctionForForwardReference(name.str(), ty, silLoc);
}

/// getGlobalNameForReference - Given a reference to a global name, look it
/// up and return an appropriate SIL function.
SILFunction *SILParser::getGlobalNameForReference(Identifier name,
                                                  CanSILFunctionType funcTy,
                                                  SourceLoc sourceLoc,
                                                  bool ignoreFwdRef) {
  SILParserFunctionBuilder builder(SILMod);
  auto silLoc = RegularLocation(sourceLoc);

  // Check to see if we have a function by this name already.
  if (SILFunction *fn = SILMod.lookUpFunction(name.str())) {
    // If so, check for matching types.
    if (fn->getLoweredFunctionType() == funcTy) {
      return fn;
    }

    P.diagnose(sourceLoc, diag::sil_value_use_type_mismatch, name.str(),
               fn->getLoweredFunctionType(), funcTy);

    return builder.createFunctionForForwardReference("" /*name*/, funcTy,
                                                     silLoc);
  }
  
  // If we didn't find a function, create a new one - it must be a forward
  // reference.
  auto *fn =
      builder.createFunctionForForwardReference(name.str(), funcTy, silLoc);
  TUState.ForwardRefFns[name] = {fn, ignoreFwdRef ? SourceLoc() : sourceLoc};
  return fn;
}


/// getBBForDefinition - Return the SILBasicBlock for a definition of the
/// specified block.
SILBasicBlock *SILParser::getBBForDefinition(Identifier Name, SourceLoc Loc) {
  // If there was no name specified for this block, just create a new one.
  if (Name.empty())
    return F->createBasicBlock();

  SILBasicBlock *&BB = BlocksByName[Name];
  // If the block has never been named yet, just create it.
  if (BB == nullptr)
    return BB = F->createBasicBlock();

  // If it already exists, it was either a forward reference or a redefinition.
  // If it is a forward reference, it should be in our undefined set.
  if (!UndefinedBlocks.erase(BB)) {
    // If we have a redefinition, return a new BB to avoid inserting
    // instructions after the terminator.
    P.diagnose(Loc, diag::sil_basicblock_redefinition, Name);
    HadError = true;
    return F->createBasicBlock();
  }

  // FIXME: Splice the block to the end of the function so they come out in the
  // right order.
  return BB;
}

/// getBBForReference - return the SILBasicBlock of the specified name.  The
/// source location is used to diagnose a failure if the block ends up never
/// being defined.
SILBasicBlock *SILParser::getBBForReference(Identifier Name, SourceLoc Loc) {
  // If the block has already been created, use it.
  SILBasicBlock *&BB = BlocksByName[Name];
  if (BB != nullptr)
    return BB;

  // Otherwise, create it and remember that this is a forward reference so
  // that we can diagnose use without definition problems.
  BB = F->createBasicBlock();
  UndefinedBlocks[BB] = {Name, Loc};
  return BB;
}

///   sil-global-name:
///     '@' identifier
bool SILParser::parseGlobalName(Identifier &Name) {
  return P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
         parseSILIdentifier(Name, diag::expected_sil_value_name);
}

/// getLocalValue - Get a reference to a local value with the specified name
/// and type.
SILValue SILParser::getLocalValue(UnresolvedValueName Name, SILType Type,
                                  SILLocation Loc, SILBuilder &B) {
  if (Name.isUndef())
    return SILUndef::get(Type, B.getFunction());

  // Check to see if this is already defined.
  ValueBase *&Entry = LocalValues[Name.Name];

  if (Entry) {
    // If this value is already defined, check it to make sure types match.
    SILType EntryTy = Entry->getType();

    if (EntryTy != Type) {
      HadError = true;
      P.diagnose(Name.NameLoc, diag::sil_value_use_type_mismatch, Name.Name,
                 EntryTy.getRawASTType(), Type.getRawASTType());
      // Make sure to return something of the requested type.
      return SILUndef::get(Type, B.getFunction());
    }

    return SILValue(Entry);
  }
  
  // Otherwise, this is a forward reference.  Create a dummy node to represent
  // it until we see a real definition.
  ForwardRefLocalValues[Name.Name] = Name.NameLoc;

  Entry = ::new PlaceholderValue(Type);
  return Entry;
}

/// setLocalValue - When an instruction or block argument is defined, this
/// method is used to register it and update our symbol table.
void SILParser::setLocalValue(ValueBase *Value, StringRef Name,
                              SourceLoc NameLoc) {
  ValueBase *&Entry = LocalValues[Name];

  // If this value was already defined, it is either a redefinition, or a
  // specification for a forward referenced value.
  if (Entry) {
    if (!ForwardRefLocalValues.erase(Name)) {
      P.diagnose(NameLoc, diag::sil_value_redefinition, Name);
      HadError = true;
      return;
    }

    // If the forward reference was of the wrong type, diagnose this now.
    if (Entry->getType() != Value->getType()) {
      P.diagnose(NameLoc, diag::sil_value_def_type_mismatch, Name,
                 Entry->getType().getRawASTType(),
                 Value->getType().getRawASTType());
      HadError = true;
    } else {
      // Forward references only live here if they have a single result.
      Entry->replaceAllUsesWith(Value);
      ::delete cast<PlaceholderValue>(Entry);
    }
    Entry = Value;
    return;
  }

  // Otherwise, just store it in our map.
  Entry = Value;
}


//===----------------------------------------------------------------------===//
// SIL Parsing Logic
//===----------------------------------------------------------------------===//

/// parseSILLinkage - Parse a linkage specifier if present.
///   sil-linkage:
///     /*empty*/          // default depends on whether this is a definition
///     'public'
///     'hidden'
///     'shared'
///     'private'
///     'public_external'
///     'hidden_external'
///     'private_external'
static bool parseSILLinkage(Optional<SILLinkage> &Result, Parser &P) {
  // Begin by initializing result to our base value of None.
  Result = None;

  // Unfortunate collision with access control keywords.
  if (P.Tok.is(tok::kw_public)) {
    Result = SILLinkage::Public;
    P.consumeToken();
    return false;
  }

  // Unfortunate collision with access control keywords.
  if (P.Tok.is(tok::kw_private)) {
    Result = SILLinkage::Private;
    P.consumeToken();
    return false;
  }

  // If we do not have an identifier, bail. All SILLinkages that we are parsing
  // are identifiers.
  if (P.Tok.isNot(tok::identifier))
    return false;

  // Then use a string switch to try and parse the identifier.
  Result = llvm::StringSwitch<Optional<SILLinkage>>(P.Tok.getText())
    .Case("non_abi", SILLinkage::PublicNonABI)
    .Case("hidden", SILLinkage::Hidden)
    .Case("shared", SILLinkage::Shared)
    .Case("public_external", SILLinkage::PublicExternal)
    .Case("hidden_external", SILLinkage::HiddenExternal)
    .Default(None);

  // If we succeed, consume the token.
  if (Result) {
    P.consumeToken(tok::identifier);
  }

  return false;
}

/// Given whether it's known to be a definition, resolve an optional
/// SIL linkage to a real one.
static SILLinkage resolveSILLinkage(Optional<SILLinkage> linkage,
                                    bool isDefinition) {
  if (linkage.has_value()) {
    return linkage.value();
  } else if (isDefinition) {
    return SILLinkage::DefaultForDefinition;
  } else {
    return SILLinkage::DefaultForDeclaration;
  }
}

// Returns false if no optional exists. Returns true on both success and
// failure. On success, the Result string is nonempty. If the optional is
// assigned to an integer value, \p value contains the parsed value. Otherwise,
// value is set to the maximum uint64_t.
static bool parseSILOptional(StringRef &Result, uint64_t &value, SourceLoc &Loc,
                             SILParser &SP) {
  if (!SP.P.consumeIf(tok::l_square))
    return false;

  value = ~uint64_t(0);

  Identifier Id;
  if (SP.parseSILIdentifier(Id, Loc, diag::expected_in_attribute_list))
    return true;

  if (SP.P.consumeIf(tok::equal)) {
    if (SP.parseInteger(value, diag::expected_in_attribute_list))
      return true;
  }
  if (SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list))
    return true;

  Result = Id.str();
  return true;
}

static bool parseSILOptional(StringRef &Result, SourceLoc &Loc, SILParser &SP) {
  uint64_t value;
  return parseSILOptional(Result, value, Loc, SP);
}

static bool parseSILOptional(StringRef &Result, SILParser &SP) {
  SourceLoc Loc;
  return parseSILOptional(Result, Loc, SP);
}

/// Parse an option attribute ('[' Expected ']')?
static bool parseSILOptional(bool &Result, SILParser &SP, StringRef Expected) {
  StringRef Optional;
  SourceLoc Loc;
  if (parseSILOptional(Optional, Loc, SP)) {
    if (Optional != Expected) {
      SP.P.diagnose(Loc, diag::sil_invalid_attribute_for_expected, Optional,
                    Expected);
      return true;
    }
    Result = true;
  }
  return false;
}

// If the qualifier string is unrecognized, then diagnose and fail.
//
// If the qualifier is absent, then succeed and set the result to None.
// The caller can decide how to proceed with an absent qualifier.
//
// Usage:
// auto parseQualifierName = [](StringRef Str) {
//   return llvm::StringSwitch<Optional<SomeQualifier>>(Str)
//       .Case("one", SomeQualifier::One)
//       .Case("two", SomeQualifier::Two)
//       .Default(None);
// };
// if (parseSILQualifier<SomeQualifier>(Qualifier, parseQualifierName))
//   return true;
template <typename T>
bool SILParser::parseSILQualifier(
    Optional<T> &result, llvm::function_ref<Optional<T>(StringRef)> parseName) {
  auto loc = P.Tok.getLoc();
  StringRef Str;
  // If we do not parse '[' ... ']',
  if (!parseSILOptional(Str, *this)) {
    result = None;
    return false;
  }
  result = parseName(Str);
  if (!result) {
    P.diagnose(loc, Diagnostic(diag::unrecognized_sil_qualifier));
    return true;
  }
  return false;
}

/// Remap RequirementReps to Requirements.
void SILParser::convertRequirements(ArrayRef<RequirementRepr> From,
                                    SmallVectorImpl<Requirement> &To,
                                    SmallVectorImpl<Type> &typeErasedParams) {
  if (From.empty()) {
    To.clear();
    return;
  }

  // Use parser lexical scopes to resolve references
  // to the generic parameters.
  auto ResolveToInterfaceType = [&](TypeRepr *TyR) -> Type {
    return performTypeResolution(TyR, /*IsSILType=*/false, ContextGenericSig,
                                 ContextGenericParams);
  };

  for (auto &Req : From) {
    if (Req.getKind() == RequirementReprKind::SameType) {
      auto FirstType = ResolveToInterfaceType(Req.getFirstTypeRepr());
      auto SecondType = ResolveToInterfaceType(Req.getSecondTypeRepr());
      Requirement ConvertedRequirement(RequirementKind::SameType, FirstType,
                                       SecondType);
      To.push_back(ConvertedRequirement);
      continue;
    }

    if (Req.getKind() == RequirementReprKind::TypeConstraint) {
      auto Subject = ResolveToInterfaceType(Req.getSubjectRepr());
      auto Constraint = ResolveToInterfaceType(Req.getConstraintRepr());
      Requirement ConvertedRequirement(RequirementKind::Conformance, Subject,
                                       Constraint);
      To.push_back(ConvertedRequirement);
      continue;
    }

    if (Req.getKind() == RequirementReprKind::LayoutConstraint) {
      auto Subject = ResolveToInterfaceType(Req.getSubjectRepr());
      Requirement ConvertedRequirement(RequirementKind::Layout, Subject,
                                       Req.getLayoutConstraint());
      To.push_back(ConvertedRequirement);

      if (SILMod.getASTContext().LangOpts.hasFeature(Feature::LayoutPrespecialization)) {
        if (auto *attributedTy = dyn_cast<AttributedTypeRepr>(Req.getSubjectRepr())) {
          if (attributedTy->getAttrs().has(TAK__noMetadata)) {
            typeErasedParams.push_back(Subject);
          }
        }
      }

      continue;
    }

    llvm_unreachable("Unsupported requirement kind");
  }
}

static bool parseDeclSILOptional(bool *isTransparent,
                                 IsSerialized_t *isSerialized,
                                 bool *isCanonical,
                                 bool *hasOwnershipSSA,
                                 IsThunk_t *isThunk,
                                 IsDynamicallyReplaceable_t *isDynamic,
                                 IsDistributed_t *isDistributed,
                                 IsRuntimeAccessible_t *isRuntimeAccessible,
                                 ForceEnableLexicalLifetimes_t *forceEnableLexicalLifetimes,
                                 IsExactSelfClass_t *isExactSelfClass,
                                 SILFunction **dynamicallyReplacedFunction,
                                 SILFunction **usedAdHocRequirementWitness,
                                 Identifier *objCReplacementFor,
                                 SILFunction::Purpose *specialPurpose,
                                 Inline_t *inlineStrategy,
                                 OptimizationMode *optimizationMode,
                                 PerformanceConstraints *perfConstraints,
                                 bool *isLet,
                                 bool *isWeakImported,
                                 bool *needStackProtection,
                                 AvailabilityContext *availability,
                                 bool *isWithoutActuallyEscapingThunk,
                                 SmallVectorImpl<std::string> *Semantics,
                                 SmallVectorImpl<ParsedSpecAttr> *SpecAttrs,
                                 ValueDecl **ClangDecl,
                                 EffectsKind *MRK,
                                 SILParser &SP, SILModule &M) {
  while (SP.P.consumeIf(tok::l_square)) {
    if (isLet && SP.P.Tok.is(tok::kw_let)) {
      *isLet = true;
      SP.P.consumeToken(tok::kw_let);
      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    }
    else if (SP.P.Tok.isNot(tok::identifier)) {
      SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
      return true;
    } else if (isTransparent && SP.P.Tok.getText() == "transparent")
      *isTransparent = true;
    else if (isSerialized && SP.P.Tok.getText() == "serialized")
      *isSerialized = IsSerialized;
    else if (isDynamic && SP.P.Tok.getText() == "dynamically_replacable")
      *isDynamic = IsDynamic;
    else if (isDistributed && SP.P.Tok.getText() == "distributed")
      *isDistributed = IsDistributed;
    else if (isRuntimeAccessible && SP.P.Tok.getText() == "runtime_accessible")
      *isRuntimeAccessible = IsRuntimeAccessible;
    else if (forceEnableLexicalLifetimes &&
             SP.P.Tok.getText() == "lexical_lifetimes")
      *forceEnableLexicalLifetimes = DoForceEnableLexicalLifetimes;
    else if (isExactSelfClass && SP.P.Tok.getText() == "exact_self_class")
      *isExactSelfClass = IsExactSelfClass;
    else if (isCanonical && SP.P.Tok.getText() == "canonical")
      *isCanonical = true;
    else if (hasOwnershipSSA && SP.P.Tok.getText() == "ossa")
      *hasOwnershipSSA = true;
    else if (needStackProtection && SP.P.Tok.getText() == "stack_protection")
      *needStackProtection = true;
    else if (isThunk && SP.P.Tok.getText() == "thunk")
      *isThunk = IsThunk;
    else if (isThunk && SP.P.Tok.getText() == "signature_optimized_thunk")
      *isThunk = IsSignatureOptimizedThunk;
    else if (isThunk && SP.P.Tok.getText() == "reabstraction_thunk")
      *isThunk = IsReabstractionThunk;
    else if (isWithoutActuallyEscapingThunk
             && SP.P.Tok.getText() == "without_actually_escaping")
      *isWithoutActuallyEscapingThunk = true;
    else if (specialPurpose && SP.P.Tok.getText() == "global_init")
      *specialPurpose = SILFunction::Purpose::GlobalInit;
    else if (specialPurpose && SP.P.Tok.getText() == "lazy_getter")
      *specialPurpose = SILFunction::Purpose::LazyPropertyGetter;
    else if (specialPurpose && SP.P.Tok.getText() == "global_init_once_fn")
      *specialPurpose = SILFunction::Purpose::GlobalInitOnceFunction;
    else if (isWeakImported && SP.P.Tok.getText() == "weak_imported") {
      if (M.getASTContext().LangOpts.Target.isOSBinFormatCOFF())
        SP.P.diagnose(SP.P.Tok, diag::attr_unsupported_on_target,
                      SP.P.Tok.getText(),
                      M.getASTContext().LangOpts.Target.str());
      else
        *isWeakImported = true;
    } else if (availability && SP.P.Tok.getText() == "available") {
      SP.P.consumeToken(tok::identifier);

      SourceRange range;
      llvm::VersionTuple version;
      if (SP.P.parseVersionTuple(version, range,
                                 diag::sil_availability_expected_version))
        return true;

      *availability = AvailabilityContext(VersionRange::allGTE(version));

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (inlineStrategy && SP.P.Tok.getText() == "noinline")
      *inlineStrategy = NoInline;
    else if (optimizationMode && SP.P.Tok.getText() == "Onone")
      *optimizationMode = OptimizationMode::NoOptimization;
    else if (optimizationMode && SP.P.Tok.getText() == "Ospeed")
      *optimizationMode = OptimizationMode::ForSpeed;
    else if (optimizationMode && SP.P.Tok.getText() == "Osize")
      *optimizationMode = OptimizationMode::ForSize;
    else if (perfConstraints && SP.P.Tok.getText() == "no_locks")
      *perfConstraints = PerformanceConstraints::NoLocks;
    else if (perfConstraints && SP.P.Tok.getText() == "no_allocation")
      *perfConstraints = PerformanceConstraints::NoAllocation;
    else if (inlineStrategy && SP.P.Tok.getText() == "always_inline")
      *inlineStrategy = AlwaysInline;
    else if (MRK && SP.P.Tok.getText() == "readnone")
      *MRK = EffectsKind::ReadNone;
    else if (MRK && SP.P.Tok.getText() == "readonly")
      *MRK = EffectsKind::ReadOnly;
    else if (MRK && SP.P.Tok.getText() == "readwrite")
      *MRK = EffectsKind::ReadWrite;
    else if (MRK && SP.P.Tok.getText() == "releasenone")
      *MRK = EffectsKind::ReleaseNone;
    else if (dynamicallyReplacedFunction && SP.P.Tok.getText() == "dynamic_replacement_for") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
      // Drop the double quotes.
      StringRef replacedFunc = SP.P.Tok.getText().drop_front().drop_back();
      SILFunction *Func = M.lookUpFunction(replacedFunc.str());
      if (!Func) {
        Identifier Id = SP.P.Context.getIdentifier(replacedFunc);
        SP.P.diagnose(SP.P.Tok, diag::sil_dynamically_replaced_func_not_found,
                      Id);
        return true;
      }
      *dynamicallyReplacedFunction = Func;
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (usedAdHocRequirementWitness && SP.P.Tok.getText() == "ref_adhoc_requirement_witness") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
      // Drop the double quotes.
      StringRef witnessFunc = SP.P.Tok.getText().drop_front().drop_back();
      SILFunction *Func = M.lookUpFunction(witnessFunc.str());
      if (!Func) {
        Identifier Id = SP.P.Context.getIdentifier(witnessFunc);
        SP.P.diagnose(SP.P.Tok, diag::sil_adhoc_requirement_witness_func_not_found,
                      Id);
        return true;
      }
      *usedAdHocRequirementWitness = Func;
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (objCReplacementFor &&
               SP.P.Tok.getText() == "objc_replacement_for") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
      // Drop the double quotes.
      StringRef replacedFunc = SP.P.Tok.getText().drop_front().drop_back();
      *objCReplacementFor = SP.P.Context.getIdentifier(replacedFunc);
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (Semantics && SP.P.Tok.getText() == "_semantics") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
  
      // Drop the double quotes.
      StringRef rawString = SP.P.Tok.getText().drop_front().drop_back();
      Semantics->push_back(rawString.str());
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (SpecAttrs && SP.P.Tok.getText() == "_specialize") {
      SourceLoc AtLoc = SP.P.Tok.getLoc();
      SourceLoc Loc(AtLoc);

      // Parse a _specialized attribute, building a parsed substitution list
      // and pushing a new ParsedSpecAttr on the SpecAttrs list. Conformances
      // cannot be generated until the function declaration is fully parsed so
      // that the function's generic signature can be consulted.
      ParsedSpecAttr SpecAttr;
      SpecAttr.requirements = {};
      SpecAttr.exported = false;
      SpecAttr.kind = SILSpecializeAttr::SpecializationKind::Full;
      SpecializeAttr *Attr;
      StringRef targetFunctionName;
      ModuleDecl *module = nullptr;
      AvailabilityContext availability = AvailabilityContext::alwaysAvailable();
      if (!SP.P.parseSpecializeAttribute(
              tok::r_square, AtLoc, Loc, Attr, &availability,
              [&targetFunctionName](Parser &P) -> bool {
                if (P.Tok.getKind() != tok::string_literal) {
                  P.diagnose(P.Tok, diag::expected_in_attribute_list);
                  return true;
                }
                // Drop the double quotes.
                targetFunctionName = P.Tok.getText().drop_front().drop_back();

                P.consumeToken(tok::string_literal);
                return true;
              },
              [&module](Parser &P) -> bool {
                if (P.Tok.getKind() != tok::identifier) {
                  P.diagnose(P.Tok, diag::expected_in_attribute_list);
                  return true;
                }
                auto ident = P.Context.getIdentifier(P.Tok.getText());
                module = P.Context.getModuleByIdentifier(ident);
                assert(module);
                P.consumeToken();
                return true;
              }))
        return true;
      SILFunction *targetFunction = nullptr;
      if (!targetFunctionName.empty()) {
        targetFunction = M.lookUpFunction(targetFunctionName.str());
        if (!targetFunction) {
          Identifier Id = SP.P.Context.getIdentifier(targetFunctionName);
          SP.P.diagnose(SP.P.Tok, diag::sil_specialize_target_func_not_found,
                        Id);
          return true;
        }
      }
      // Convert SpecializeAttr into ParsedSpecAttr.
      SpecAttr.requirements = Attr->getTrailingWhereClause()->getRequirements();
      SpecAttr.kind = Attr->getSpecializationKind() ==
                              swift::SpecializeAttr::SpecializationKind::Full
                          ? SILSpecializeAttr::SpecializationKind::Full
                          : SILSpecializeAttr::SpecializationKind::Partial;
      SpecAttr.exported = Attr->isExported();
      SpecAttr.target = targetFunction;
      SpecAttr.availability = availability;
      SpecAttrs->emplace_back(SpecAttr);
      if (!Attr->getSPIGroups().empty()) {
        SpecAttr.spiGroupID = Attr->getSPIGroups()[0];
      }
      continue;
    }
    else if (ClangDecl && SP.P.Tok.getText() == "clang") {
      SP.P.consumeToken(tok::identifier);
      if (SP.parseSILDottedPathWithoutPound(*ClangDecl))
        return true;

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else {
      SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
      return true;
    }
    SP.P.consumeToken(tok::identifier);
    SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
  }
  return false;
}

Type SILParser::performTypeResolution(TypeRepr *TyR, bool IsSILType,
                                      GenericSignature GenericSig,
                                      GenericParamList *GenericParams) {
  if (!GenericSig)
    GenericSig = ContextGenericSig;

  SILTypeResolutionContext SILContext(IsSILType, GenericParams,
                                      &OpenedPackElements);

  return swift::performTypeResolution(TyR, P.Context, GenericSig,
                                      &SILContext, &P.SF);
}

/// Find the top-level ValueDecl or Module given a name.
static llvm::PointerUnion<ValueDecl *, ModuleDecl *>
lookupTopDecl(Parser &P, DeclBaseName Name, bool typeLookup) {
  // Use UnqualifiedLookup to look through all of the imports.
  UnqualifiedLookupOptions options;
  if (typeLookup)
    options |= UnqualifiedLookupFlags::TypeLookup;

  auto &ctx = P.SF.getASTContext();
  auto descriptor = UnqualifiedLookupDescriptor(DeclNameRef(Name), &P.SF);
  auto lookup = evaluateOrDefault(ctx.evaluator,
                                  UnqualifiedLookupRequest{descriptor}, {});
  lookup.filter([](LookupResultEntry entry, bool isOuter) -> bool {
    return !isa<MacroDecl>(entry.getValueDecl());
  });

  assert(lookup.size() == 1);

  return lookup.back().getValueDecl();
}

/// Find the ValueDecl given an interface type and a member name.
static ValueDecl *lookupMember(Parser &P, Type Ty, DeclBaseName Name,
                               SourceLoc Loc,
                               SmallVectorImpl<ValueDecl *> &Lookup,
                               bool ExpectMultipleResults) {
  Type CheckTy = Ty;
  if (auto MetaTy = CheckTy->getAs<AnyMetatypeType>())
    CheckTy = MetaTy->getInstanceType();

  if (auto nominal = CheckTy->getAnyNominal()) {
    if (Name == DeclBaseName::createDestructor() &&
        isa<ClassDecl>(nominal)) {
      auto *classDecl = cast<ClassDecl>(nominal);
      Lookup.push_back(classDecl->getDestructor());
    } else {
      auto found = nominal->lookupDirect(Name);
      Lookup.append(found.begin(), found.end());
    }
  } else if (auto moduleTy = CheckTy->getAs<ModuleType>()) {
    moduleTy->getModule()->lookupValue(Name, NLKind::QualifiedLookup, Lookup);
  } else {
    P.diagnose(Loc, diag::sil_member_lookup_bad_type, Name, Ty);
    return nullptr;
  }

  if (Lookup.empty() || (!ExpectMultipleResults && Lookup.size() != 1)) {
    P.diagnose(Loc, diag::sil_named_member_decl_not_found, Name, Ty);
    return nullptr;
  }
  return Lookup[0];
}

bool SILParser::parseASTType(CanType &result,
                             GenericSignature genericSig,
                             GenericParamList *genericParams,
                             bool forceContextualType) {
  ParserResult<TypeRepr> parsedType = P.parseType();
  if (parsedType.isNull()) return true;

  // If we weren't given a specific generic context to resolve the type
  // within, use the contextual generic parameters and always produce
  // a contextual type.  Otherwise, produce a contextual type only if
  // we were asked for one.
  bool wantContextualType = forceContextualType;
  if (!genericSig) {
    genericSig = ContextGenericSig;
    wantContextualType = true;
  }
  if (genericParams == nullptr)
    genericParams = ContextGenericParams;

  bindSILGenericParams(parsedType.get());

  auto resolvedType = performTypeResolution(
      parsedType.get(), /*isSILType=*/false, genericSig, genericParams);
  if (wantContextualType && genericSig) {
    resolvedType = genericSig.getGenericEnvironment()
        ->mapTypeIntoContext(resolvedType);
  }

  if (resolvedType->hasError())
    return true;

  result = resolvedType->getCanonicalType();

  return false;
}

void SILParser::bindSILGenericParams(TypeRepr *TyR) {
  // Resolve the generic environments for parsed generic function and box types.
  class HandleSILGenericParamsWalker : public ASTWalker {
    SourceFile *SF;

  public:
    HandleSILGenericParamsWalker(SourceFile *SF) : SF(SF) {}

    /// Walk everything in a macro
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      if (auto fnType = dyn_cast<FunctionTypeRepr>(T)) {
        if (auto *genericParams = fnType->getGenericParams()) {
          auto sig = handleSILGenericParams(genericParams, SF);
          fnType->setGenericSignature(sig);
        }

        if (auto *genericParams = fnType->getPatternGenericParams()) {
          auto sig = handleSILGenericParams(genericParams, SF);
          fnType->setPatternGenericSignature(sig);
        }
      }

      if (auto boxType = dyn_cast<SILBoxTypeRepr>(T)) {
        if (auto *genericParams = boxType->getGenericParams()) {
          auto sig = handleSILGenericParams(genericParams, SF);
          boxType->setGenericSignature(sig);
        }
      }

      return Action::Continue();
    }
  };

  TyR->walk(HandleSILGenericParamsWalker(&P.SF));
}

///   sil-type:
///     '$' '*'? attribute-list (generic-params)? type
///
bool SILParser::parseSILType(SILType &Result,
                             GenericSignature &ParsedGenericSig,
                             GenericParamList *&ParsedGenericParams,
                             bool IsFuncDecl,
                             GenericSignature OuterGenericSig,
                             GenericParamList *OuterGenericParams) {
  ParsedGenericSig = GenericSignature();
  ParsedGenericParams = nullptr;

  if (!OuterGenericSig)
    OuterGenericSig = ContextGenericSig;
  if (OuterGenericParams == nullptr)
    OuterGenericParams = ContextGenericParams;

  if (P.parseToken(tok::sil_dollar, diag::expected_sil_type))
    return true;

  // If we have a '*', then this is an address type.
  SILValueCategory category = SILValueCategory::Object;
  if (P.Tok.isAnyOperator() && P.Tok.getText().startswith("*")) {
    category = SILValueCategory::Address;
    P.consumeStartingCharacterOfCurrentToken();
  }

  // Parse attributes.
  ParamDecl::Specifier specifier;
  SourceLoc specifierLoc;
  SourceLoc isolatedLoc;
  SourceLoc constLoc;
  TypeAttributes attrs;
  P.parseTypeAttributeList(specifier, specifierLoc, isolatedLoc, constLoc, attrs);

  // Global functions are implicitly @convention(thin) if not specified otherwise.
  if (IsFuncDecl && !attrs.has(TAK_convention)) {
    // Use a random location.
    attrs.setAttr(TAK_convention, P.PreviousLoc);
    attrs.ConventionArguments =
      TypeAttributes::Convention::makeSwiftConvention("thin");
  }

  ParserResult<TypeRepr> TyR = P.parseType(diag::expected_sil_type);

  if (TyR.isNull())
    return true;

  bindSILGenericParams(TyR.get());
  
  // Apply attributes to the type.
  auto *attrRepr =
      P.applyAttributeToType(
        TyR.get(), attrs, specifier, specifierLoc, isolatedLoc, constLoc);
  auto Ty = performTypeResolution(attrRepr, /*IsSILType=*/true, OuterGenericSig,
                                  OuterGenericParams);
  if (OuterGenericSig) {
    Ty = OuterGenericSig.getGenericEnvironment()->mapTypeIntoContext(Ty);
  }

  if (Ty->hasError())
    return true;

  // Save the top-level function generic environment if there was one.
  if (auto fnType = dyn_cast<FunctionTypeRepr>(TyR.get())) {
    if (auto genericSig = fnType->getGenericSignature())
      ParsedGenericSig = genericSig;
    if (auto *genericParams = fnType->getGenericParams())
      ParsedGenericParams = genericParams;
  }

  Result = SILType::getPrimitiveType(Ty->getCanonicalType(),
                                     category);

  return false;
}

bool SILParser::parseSILDottedPath(ValueDecl *&Decl,
                                   SmallVectorImpl<ValueDecl *> &values) {
  if (P.parseToken(tok::pound, diag::expected_sil_constant))
    return true;
  return parseSILDottedPathWithoutPound(Decl, values);
}

bool SILParser::parseSILDottedPathWithoutPound(ValueDecl *&Decl,
                                   SmallVectorImpl<ValueDecl *> &values) {
  // Handle sil-dotted-path.
  Identifier Id;
  SmallVector<DeclBaseName, 4> FullName;
  SmallVector<SourceLoc, 4> Locs;
  do {
    Locs.push_back(P.Tok.getLoc());
    switch (P.Tok.getKind()) {
    case tok::kw_subscript:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createSubscript());
      break;
    case tok::kw_init:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createConstructor());
      break;
    case tok::kw_deinit:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createDestructor());
      break;
    default:
      if (parseSILIdentifier(Id, diag::expected_sil_constant))
        return true;
      FullName.push_back(Id);
      break;
    }
  } while (P.consumeIf(tok::period));

  // Look up ValueDecl from a dotted path. If there are multiple components,
  // the first one must be a type declaration.
  ValueDecl *VD;
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res = lookupTopDecl(
    P, FullName[0], /*typeLookup=*/FullName.size() > 1);
  // It is possible that the last member lookup can return multiple lookup
  // results. One example is the overloaded member functions.
  if (Res.is<ModuleDecl*>()) {
    assert(FullName.size() > 1 &&
           "A single module is not a full path to SILDeclRef");
    auto Mod = Res.get<ModuleDecl*>();
    values.clear();
    VD = lookupMember(P, ModuleType::get(Mod), FullName[1], Locs[1], values,
                      FullName.size() == 2/*ExpectMultipleResults*/);
    for (unsigned I = 2, E = FullName.size(); I < E; ++I) {
      values.clear();
      VD = lookupMember(P, VD->getInterfaceType(), FullName[I], Locs[I], values,
                        I == FullName.size() - 1/*ExpectMultipleResults*/);
    }
  } else {
    VD = Res.get<ValueDecl*>();
    for (unsigned I = 1, E = FullName.size(); I < E; ++I) {
      values.clear();
      VD = lookupMember(P, VD->getInterfaceType(), FullName[I], Locs[I], values,
                        I == FullName.size() - 1/*ExpectMultipleResults*/);
    }
  }
  Decl = VD;
  return false;
}

static Optional<AccessorKind> getAccessorKind(StringRef ident) {
  return llvm::StringSwitch<Optional<AccessorKind>>(ident)
           .Case("getter", AccessorKind::Get)
           .Case("setter", AccessorKind::Set)
           .Case("addressor", AccessorKind::Address)
           .Case("mutableAddressor", AccessorKind::MutableAddress)
           .Case("read", AccessorKind::Read)
           .Case("modify", AccessorKind::Modify)
           .Default(None);
}

///  sil-decl-ref ::= '#' sil-identifier ('.' sil-identifier)* sil-decl-subref?
///  sil-decl-subref ::= '!' sil-decl-subref-part ('.' sil-decl-lang)?
///                      ('.' sil-decl-autodiff)?
///  sil-decl-subref ::= '!' sil-decl-lang
///  sil-decl-subref ::= '!' sil-decl-autodiff
///  sil-decl-subref-part ::= 'getter'
///  sil-decl-subref-part ::= 'setter'
///  sil-decl-subref-part ::= 'allocator'
///  sil-decl-subref-part ::= 'initializer'
///  sil-decl-subref-part ::= 'enumelt'
///  sil-decl-subref-part ::= 'destroyer'
///  sil-decl-subref-part ::= 'globalaccessor'
///  sil-decl-lang ::= 'foreign'
///  sil-decl-autodiff ::= sil-decl-autodiff-kind '.' sil-decl-autodiff-indices
///  sil-decl-autodiff-kind ::= 'jvp'
///  sil-decl-autodiff-kind ::= 'vjp'
///  sil-decl-autodiff-indices ::= [SU]+
bool SILParser::parseSILDeclRef(SILDeclRef &Result,
                                SmallVectorImpl<ValueDecl *> &values) {
  ValueDecl *VD;
  if (parseSILDottedPath(VD, values))
    return true;

  // Initialize SILDeclRef components.
  SILDeclRef::Kind Kind = SILDeclRef::Kind::Func;
  bool IsObjC = false;
  AutoDiffDerivativeFunctionIdentifier *DerivativeId = nullptr;

  if (!P.consumeIf(tok::sil_exclamation)) {
    // Construct SILDeclRef.
    Result = SILDeclRef(VD, Kind, IsObjC,
                        /*distributed=*/false, /*knownToBeLocal=*/false,
                        /*runtimeAccessible=*/false,
                        SILDeclRef::BackDeploymentKind::None, DerivativeId);
    return false;
  }

  // Handle SILDeclRef components. ParseState tracks the last parsed component.
  //
  // When ParseState is 0, accept kind (`func|getter|setter|...`) and set
  // ParseState to 1.
  //
  // Always accept `foreign` and derivative function identifier.
  unsigned ParseState = 0;
  Identifier Id;
  do {
    if (P.Tok.is(tok::identifier)) {
      auto IdLoc = P.Tok.getLoc();
      if (parseSILIdentifier(Id, diag::expected_sil_constant))
        return true;
      Optional<AccessorKind> accessorKind;
      if (!ParseState && Id.str() == "func") {
        Kind = SILDeclRef::Kind::Func;
        ParseState = 1;
      } else if (!ParseState &&
                 (accessorKind = getAccessorKind(Id.str())).has_value()) {
        // Drill down to the corresponding accessor for each declaration,
        // compacting away decls that lack it.
        size_t destI = 0;
        for (size_t srcI = 0, e = values.size(); srcI != e; ++srcI) {
          if (auto storage = dyn_cast<AbstractStorageDecl>(values[srcI]))
            if (auto accessor = storage->getOpaqueAccessor(*accessorKind))
              values[destI++] = accessor;
        }
        values.resize(destI);

        // Complain if none of the decls had a corresponding accessor.
        if (destI == 0) {
          P.diagnose(IdLoc, diag::referenced_value_no_accessor, 0);
          return true;
        }

        Kind = SILDeclRef::Kind::Func;
        VD = values[0];
        ParseState = 1;
      } else if (!ParseState && Id.str() == "allocator") {
        Kind = SILDeclRef::Kind::Allocator;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "initializer") {
        Kind = SILDeclRef::Kind::Initializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "enumelt") {
        Kind = SILDeclRef::Kind::EnumElement;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "destroyer") {
        Kind = SILDeclRef::Kind::Destroyer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "deallocator") {
        Kind = SILDeclRef::Kind::Deallocator;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "globalaccessor") {
        Kind = SILDeclRef::Kind::GlobalAccessor;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "ivardestroyer") {
        Kind = SILDeclRef::Kind::IVarDestroyer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "ivarinitializer") {
        Kind = SILDeclRef::Kind::IVarInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "defaultarg") {
        Kind = SILDeclRef::Kind::IVarInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "attrgenerator") {
        Kind = SILDeclRef::Kind::RuntimeAttributeGenerator;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "propertyinit") {
        Kind = SILDeclRef::Kind::StoredPropertyInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "backinginit") {
        Kind = SILDeclRef::Kind::PropertyWrapperBackingInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "projectedvalueinit") {
        Kind = SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue;
        ParseState = 1;
      } else if (Id.str() == "foreign") {
        IsObjC = true;
        break;
      } else if (Id.str() == "jvp" || Id.str() == "vjp") {
        IndexSubset *parameterIndices = nullptr;
        GenericSignature derivativeGenSig;
        // Parse derivative function kind.
        AutoDiffDerivativeFunctionKind derivativeKind(Id.str());
        if (!P.consumeIf(tok::period)) {
          P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ".");
          return true;
        }
        // Parse parameter indices.
        parameterIndices =
            IndexSubset::getFromString(SILMod.getASTContext(), P.Tok.getText());
        if (!parameterIndices) {
          P.diagnose(P.Tok, diag::invalid_index_subset);
          return true;
        }
        P.consumeToken();
        // Parse derivative generic signature (optional).
        if (P.Tok.is(tok::oper_binary_unspaced) && P.Tok.getText() == ".<") {
          P.consumeStartingCharacterOfCurrentToken(tok::period);
          // Create a new scope to avoid type redefinition errors.
          auto *genericParams = P.maybeParseGenericParams().getPtrOrNull();
          assert(genericParams);
          derivativeGenSig = handleSILGenericParams(genericParams, &P.SF);
        }
        DerivativeId = AutoDiffDerivativeFunctionIdentifier::get(
            derivativeKind, parameterIndices, derivativeGenSig,
            SILMod.getASTContext());
        break;
      } else {
        break;
      }
    } else
      break;

  } while (P.consumeIf(tok::period));

  // Construct SILDeclRef.
  Result = SILDeclRef(VD, Kind, IsObjC,
                      /*distributed=*/false, /*knownToBeLocal=*/false,
                      /*runtimeAccessible=*/false,
                      SILDeclRef::BackDeploymentKind::None, DerivativeId);
  return false;
}

/// parseValueName - Parse a value name without a type available yet.
///
///     sil-value-name:
///       sil-local-name
///       'undef'
///
bool SILParser::parseValueName(UnresolvedValueName &Result) {
  Result.Name = P.Tok.getText();

  if (P.Tok.is(tok::kw_undef)) {
    Result.NameLoc = P.consumeToken(tok::kw_undef);
    return false;
  }

  // Parse the local-name.
  if (P.parseToken(tok::sil_local_name, Result.NameLoc,
                   diag::expected_sil_value_name))
    return true;

  return false;
}

/// parseValueRef - Parse a value, given a contextual type.
///
///     sil-value-ref:
///       sil-local-name
///
bool SILParser::parseValueRef(SILValue &Result, SILType Ty,
                              SILLocation Loc, SILBuilder &B) {
  UnresolvedValueName Name;
  if (parseValueName(Name)) return true;
  Result = getLocalValue(Name, Ty, Loc, B);
  return false;
}

/// parseTypedValueRef - Parse a type/value reference pair.
///
///    sil-typed-valueref:
///       sil-value-ref ':' sil-type
///
bool SILParser::parseTypedValueRef(SILValue &Result, SourceLoc &Loc,
                                   SILBuilder &B) {
  Loc = P.Tok.getLoc();

  UnresolvedValueName Name;
  SILType Ty;
  if (parseValueName(Name) ||
      P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
      parseSILType(Ty))
    return true;
  
  Result = getLocalValue(Name, Ty, RegularLocation(Loc), B);
  return false;
}

/// Look up whether the given string corresponds to a SIL opcode.
static Optional<SILInstructionKind> getOpcodeByName(StringRef OpcodeName) {
  return llvm::StringSwitch<Optional<SILInstructionKind>>(OpcodeName)
  #define FULL_INST(Id, TextualName, Parent, MemBehavior, MayRelease) \
    .Case(#TextualName, SILInstructionKind::Id)
  #include "swift/SIL/SILNodes.def"
    .Default(None);
}

/// getInstructionKind - This method maps the string form of a SIL instruction
/// opcode to an enum.
bool SILParser::parseSILOpcode(SILInstructionKind &Opcode, SourceLoc &OpcodeLoc,
                               StringRef &OpcodeName) {
  OpcodeLoc = P.Tok.getLoc();
  OpcodeName = P.Tok.getText();
  // Parse this textually to avoid Swift keywords (like 'return') from
  // interfering with opcode recognition.
  auto MaybeOpcode = getOpcodeByName(OpcodeName);
  if (!MaybeOpcode) {
    P.diagnose(OpcodeLoc, diag::expected_sil_instr_opcode);
    return true;
  }

  Opcode = MaybeOpcode.value();
  P.consumeToken();
  return false;
}

bool SILParser::parseSILDebugInfoExpression(SILDebugInfoExpression &DIExpr) {
  if (P.Tok.getText() != "expr")
    return true;

  // All operators that we currently support
  static const SILDIExprOperator AllOps[] = {
    SILDIExprOperator::Dereference,
    SILDIExprOperator::Fragment,
    SILDIExprOperator::Plus,
    SILDIExprOperator::Minus,
    SILDIExprOperator::ConstUInt,
    SILDIExprOperator::ConstSInt
  };

  do {
    P.consumeToken();
    bool FoundOp = false;
    auto OpLoc = P.Tok.getLoc();
    for (const auto &Op : AllOps) {
      const auto *ExprInfo = SILDIExprInfo::get(Op);
      auto OpText = ExprInfo->OpText;
      if (OpText != P.Tok.getText())
        continue;
      auto NewOperator = SILDIExprElement::createOperator(Op);
      DIExpr.push_back(NewOperator);
      P.consumeToken();

      // Ready to parse the operands
      for (const auto &OpKind : ExprInfo->OperandKinds) {
        if (P.parseToken(tok::colon, diag::expected_sil_colon,
                         "debug info expression operand"))
          return true;

        switch (OpKind) {
        case SILDIExprElement::DeclKind: {
          SILDeclRef Result;
          if (parseSILDeclRef(Result) || !Result.hasDecl()) {
            P.diagnose(P.Tok.getLoc(), diag::sil_dbg_expr_expect_operand_kind,
                       OpText, "declaration");
            return true;
          }
          auto NewOperand = SILDIExprElement::createDecl(Result.getDecl());
          DIExpr.push_back(NewOperand);
          break;
        }
        case SILDIExprElement::ConstIntKind: {
          bool IsNegative = false;
          if (P.Tok.is(tok::oper_prefix) && P.Tok.getRawText() == "-") {
            P.consumeToken();
            IsNegative = true;
          }
          int64_t Val;
          if (parseInteger(Val, diag::sil_invalid_constant))
            return true;
          if (IsNegative)
            Val = -Val;
          auto NewOperand =
            SILDIExprElement::createConstInt(static_cast<uint64_t>(Val));
          DIExpr.push_back(NewOperand);
          break;
        }
        default:
          P.diagnose(P.Tok.getLoc(), diag::sil_dbg_unknown_expr_part,
                     "operand kind");
          return true;
        }
      }
      FoundOp = true;
      break;
    }

    if (!FoundOp) {
      P.diagnose(OpLoc, diag::sil_dbg_unknown_expr_part, "operator");
      return true;
    }
  } while (P.Tok.is(tok::colon));

  return false;
}

static bool peekSILDebugLocation(Parser &P) {
  auto T = P.peekToken().getText();
  return P.Tok.is(tok::comma) && (T == "loc" || T == "scope");
}

bool SILParser::parseSILDebugVar(SILDebugVariable &Var) {
  auto parseVariableName = [&, this](bool Consume) -> bool {
    if (Consume)
      P.consumeToken();
    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
      return true;
    }
    // Drop the double quotes.
    StringRef Val = P.Tok.getText().drop_front().drop_back();
    Var.Name = Val;
    return false;
  };

  while (P.Tok.is(tok::comma) && !peekSILDebugLocation(P)) {
    P.consumeToken();
    StringRef Key = P.Tok.getText();
    bool NoConsume = false;
    if (P.consumeIf(tok::l_paren)) {
      if (parseVerbatim("name"))
        return true;
      if (parseVariableName(/*Consume=*/false))
        return true;
      P.consumeToken();

      // Optional operands
      if (peekSILDebugLocation(P)) {
        P.consumeToken(tok::comma);

        bool requireScope = false;
        if (P.Tok.getText() == "loc") {
          SILLocation VarLoc = RegularLocation::getAutoGeneratedLocation();
          if (parseSILLocation(VarLoc))
            return true;
          Var.Loc = VarLoc;
          requireScope = P.consumeIf(tok::comma);
        }

        if (P.Tok.getText() == "scope" || requireScope) {
          parseVerbatim("scope");
          SILDebugScope *DS = nullptr;
          if (parseScopeRef(DS))
            return true;
          if (DS)
            Var.Scope = DS;
        }
      }

      if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
        return true;

      NoConsume = true;
    } else if (Key == "name") {
      if (parseVariableName(/*Consume=*/true))
        return true;
    } else if (Key == "argno") {
      P.consumeToken();
      if (P.Tok.getKind() != tok::integer_literal) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
        return true;
      }
      uint16_t ArgNo;
      if (parseIntegerLiteral(P.Tok.getText(), 0, ArgNo))
        return true;
      Var.ArgNo = ArgNo;
    } else if (Key == "expr") {
      if (parseSILDebugInfoExpression(Var.DIExpr))
        return true;
      NoConsume = true;
    } else if (Key == "type") {
      // Auxiliary type information
      P.consumeToken();
      SILType Ty;
      if (parseSILType(Ty))
        return true;
      Var.Type = Ty;
      NoConsume = true;
    } else if (Key == "let") {
      Var.Constant = true;
    } else if (Key == "var") {
      Var.Constant = false;
    } else if (Key == "loc") {
      Var.Constant = false;
    } else if (Key == "implicit") {
      Var.Implicit = true;
    } else {
      P.diagnose(P.Tok, diag::sil_dbg_unknown_key, Key);
      return true;
    }

    if (!NoConsume)
      P.consumeToken();
  }
  return false;
}

bool SILParser::parseSILBBArgsAtBranch(SmallVector<SILValue, 6> &Args,
                                       SILBuilder &B) {
  if (P.Tok.is(tok::l_paren)) {
    SourceLoc LParenLoc = P.consumeToken(tok::l_paren);
    SourceLoc RParenLoc;

    bool HasError = false;
    if (P.parseList(tok::r_paren, LParenLoc, RParenLoc,
                    /*AllowSepAfterLast=*/false,
                    diag::sil_basicblock_arg_rparen,
                    [&]() -> ParserStatus {
                      SILValue Arg;
                      SourceLoc ArgLoc;
                      if (parseTypedValueRef(Arg, ArgLoc, B)) {
                        HasError = true;
                        return makeParserError();
                      }
                      Args.push_back(Arg);
                      return makeParserSuccess();
                    }).isErrorOrHasCompletion() || HasError)
      return true;
  }
  return false;
}

/// Parse the substitution list for an apply instruction or
/// specialized protocol conformance.
bool SILParser::parseSubstitutions(SmallVectorImpl<ParsedSubstitution> &parsed,
                                   GenericSignature GenericSig,
                                   GenericParamList *GenericParams) {
  // Check for an opening '<' bracket.
  if (!P.startsWithLess(P.Tok))
    return false;
  
  if (!GenericSig)
    GenericSig = ContextGenericSig;
  if (GenericParams == nullptr)
    GenericParams = ContextGenericParams;

  P.consumeStartingLess();
  
  // Parse a list of Substitutions.
  do {
    SourceLoc Loc = P.Tok.getLoc();

    // Parse substitution as AST type.
    ParserResult<TypeRepr> TyR = P.parseType();
    if (TyR.isNull())
      return true;

    auto Ty = performTypeResolution(TyR.get(), /*IsSILType=*/false, GenericSig,
                                    GenericParams);
    if (GenericSig) {
      Ty = GenericSig.getGenericEnvironment()->mapTypeIntoContext(Ty);
    }

    if (Ty->hasError())
      return true;
    parsed.push_back({Loc, Ty});
  } while (P.consumeIf(tok::comma));
  
  // Consume the closing '>'.
  if (!P.startsWithGreater(P.Tok)) {
    P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ">");
    return true;
  }
  P.consumeStartingGreater();
  
  return false;
}

/// Collect conformances by looking up the conformance from replacement
/// type and protocol decl.
static bool getConformancesForSubstitution(Parser &P,
              ArrayRef<ProtocolDecl*> protocols,
              Type subReplacement,
              SourceLoc loc,
              SmallVectorImpl<ProtocolConformanceRef> &conformances) {
  auto M = P.SF.getParentModule();

  for (auto protoDecl : protocols) {
    auto conformance = M->lookupConformance(subReplacement, protoDecl);
    if (conformance.isInvalid()) {
      P.diagnose(loc, diag::sil_substitution_mismatch, subReplacement,
                 protoDecl->getDeclaredInterfaceType());
      return true;
    }
    conformances.push_back(conformance);
  }

  return false;
}

/// Reconstruct an AST substitution map from parsed substitutions.
SubstitutionMap getApplySubstitutionsFromParsed(
                             SILParser &SP,
                             GenericSignature genericSig,
                             ArrayRef<ParsedSubstitution> parses) {
  if (parses.empty()) {
    assert(!genericSig);
    return SubstitutionMap();
  }

  assert(genericSig);

  auto loc = parses[0].loc;

  // Ensure that we have the right number of type arguments.
  if (parses.size() != genericSig.getGenericParams().size()) {
    bool hasTooFew = parses.size() < genericSig.getGenericParams().size();
    SP.P.diagnose(loc,
                  hasTooFew ? diag::sil_missing_substitutions
                            : diag::sil_too_many_substitutions);
    return SubstitutionMap();
  }

  bool failed = false;
  auto subMap = SubstitutionMap::get(
      genericSig,
      [&](SubstitutableType *type) -> Type {
        auto genericParam = dyn_cast<GenericTypeParamType>(type);
        if (!genericParam)
          return nullptr;

        auto index = genericSig->getGenericParamOrdinal(genericParam);
        assert(index < genericSig.getGenericParams().size());
        assert(index < parses.size());

        // Provide the replacement type.
        return parses[index].replacement;
      },
      [&](CanType dependentType, Type replacementType,
          ProtocolDecl *proto) -> ProtocolConformanceRef {
        auto M = SP.P.SF.getParentModule();
        if (auto conformance = M->lookupConformance(replacementType, proto))
          return conformance;

        SP.P.diagnose(loc, diag::sil_substitution_mismatch, replacementType,
                      proto->getDeclaredInterfaceType());
        failed = true;

        return ProtocolConformanceRef(proto);
      });

  return failed ? SubstitutionMap() : subMap;
}

static ArrayRef<ProtocolConformanceRef>
collectExistentialConformances(Parser &P, CanType conformingType, SourceLoc loc,
                               CanType protocolType) {
  auto layout = protocolType.getExistentialLayout();

  if (layout.requiresClass()) {
    if (!conformingType->mayHaveSuperclass() &&
        !conformingType->isObjCExistentialType()) {
      P.diagnose(loc, diag::sil_not_class, conformingType);
    }
  }

  // FIXME: Check superclass also.

  auto protocols = layout.getProtocols();
  if (protocols.empty())
    return {};

  SmallVector<ProtocolConformanceRef, 2> conformances;
  getConformancesForSubstitution(P, protocols, conformingType,
                                 loc, conformances);

  return P.Context.AllocateCopy(conformances);
}

/// sil-loc ::= 'loc' string-literal ':' [0-9]+ ':' [0-9]+
bool SILParser::parseSILLocation(SILLocation &Loc) {
  if (parseVerbatim("loc"))
    return true;

  bool isAutoGenerated = false;
  if (P.Tok.isAnyOperator() && P.Tok.getText().startswith("*")) {
    isAutoGenerated = true;
    P.consumeStartingCharacterOfCurrentToken();
  }

  if (P.Tok.getKind() != tok::string_literal) {
    P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
    return true;
  }
  // Drop the double quotes.
  StringRef File = P.Tok.getText().drop_front().drop_back();
  P.consumeToken(tok::string_literal);
  if (P.parseToken(tok::colon, diag::expected_colon_in_sil_location))
    return true;
  unsigned Line = 0;
  if (parseInteger(Line, diag::sil_invalid_line_in_sil_location))
    return true;
  if (P.parseToken(tok::colon, diag::expected_colon_in_sil_location))
    return true;
  unsigned Column = 0;
  if (parseInteger(Column, diag::sil_invalid_column_in_sil_location))
    return true;

  auto fnl = SILLocation::FilenameAndLocation::alloc(Line, Column,
    P.Context.getIdentifier(File).str().data(), SILMod);

  Loc = RegularLocation(fnl);

  if (isAutoGenerated)
    Loc.markAutoGenerated();

  return false;
}

bool SILParser::parseScopeRef(SILDebugScope *&DS) {
  unsigned Slot;
  SourceLoc SlotLoc = P.Tok.getLoc();
  if (parseInteger(Slot, diag::sil_invalid_scope_slot))
    return true;

  DS = TUState.ScopeSlots[Slot];
  if (!DS) {
    P.diagnose(SlotLoc, diag::sil_scope_undeclared, Slot);
    return true;
  }
  return false;
}

bool SILParser::parseForwardingOwnershipKind(
    ValueOwnershipKind &forwardingKind) {
  if (P.Tok.is(tok::comma)) {
    P.consumeToken();
    parsedComma = true;
  }
  if (!parsedComma)
    return false;

  if (P.Tok.is(tok::identifier) && P.Tok.getText() == "forwarding") {
    parsedComma = false;
    P.consumeToken();
    return P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
           || parseSILOwnership(forwardingKind);
  }
  return false;
}

///  (',' sil-loc)? (',' sil-scope-ref)?
bool SILParser::parseSILDebugLocation(SILLocation &L, SILBuilder &B) {
  // Parse the debug information, if any.
  if (P.Tok.is(tok::comma)) {
    P.consumeToken();
    parsedComma = true;
  }
  if (!parsedComma)
    return false;

  bool requireScope = false;
  if (P.Tok.getText() == "loc") {
    parsedComma = false;
    if (parseSILLocation(L))
      return true;

    if (P.Tok.is(tok::comma)) {
      P.consumeToken();
      requireScope = true;
    }
  }
  if (P.Tok.getText() == "scope" || requireScope) {
    parsedComma = false;
    parseVerbatim("scope");
    SILDebugScope *DS = nullptr;
    if (parseScopeRef(DS))
      return true;
    if (DS)
      B.setCurrentDebugScope(DS);
  }
  return false;
}

static bool parseAssignByWrapperMode(AssignByWrapperInst::Mode &Result,
                                          SILParser &P) {
  StringRef Str;
  // If we do not parse '[' ... ']', we have unknown. Set value and return.
  if (!parseSILOptional(Str, P)) {
    Result = AssignByWrapperInst::Unknown;
    return false;
  }

  // Then try to parse one of our other initialization kinds. We do not support
  // parsing unknown here so we use that as our fail value.
  auto Tmp = llvm::StringSwitch<AssignByWrapperInst::Mode>(Str)
        .Case("init", AssignByWrapperInst::Initialization)
        .Case("assign", AssignByWrapperInst::Assign)
        .Case("assign_wrapped_value", AssignByWrapperInst::AssignWrappedValue)
        .Default(AssignByWrapperInst::Unknown);

  // Thus return true (following the conventions in this file) if we fail.
  if (Tmp == AssignByWrapperInst::Unknown)
    return true;

  // Otherwise, assign Result and return false.
  Result = Tmp;
  return false;
}

// Parse a list of integer indices, prefaced with the given string label.
// Returns true on error.
static bool parseIndexList(Parser &P, StringRef label,
                           SmallVectorImpl<unsigned> &indices,
                           const Diagnostic &parseIndexDiag) {
  SourceLoc loc;
  // Parse `[<label> <integer_literal>...]`.
  if (P.parseToken(tok::l_square, diag::sil_autodiff_expected_lsquare,
                   "index list") ||
      P.parseSpecificIdentifier(
          label, diag::sil_autodiff_expected_index_list_label, label))
    return true;
  while (P.Tok.is(tok::integer_literal)) {
    unsigned index;
    if (P.parseUnsignedInteger(index, loc, parseIndexDiag))
      return true;
    indices.push_back(index);
  }
  if (P.parseToken(tok::r_square, diag::sil_autodiff_expected_rsquare,
                   "index list"))
    return true;
  return false;
}

/// Parse a differentiability kind, an autodiff config, and a function name for
/// a differentiability witness. Returns true on error.
///
/// sil-differentiability-witness-config-and-function ::=
///   '[' differentiability-kind ']'
///   '[' 'parameters' index-subset ']'
///   '[' 'results' index-subset ']'
///   ('<' 'where' derivative-generic-signature-requirements '>')?
///   sil-function-ref
///
/// e.g. [reverse] [parameters 0 1] [results 0] <T where T: Differentiable>
///      @foo : <T> $(T) -> T
static bool parseSILDifferentiabilityWitnessConfigAndFunction(
    Parser &P, SILParser &SP, SILLocation L,
    DifferentiabilityKind &resultDiffKind, AutoDiffConfig &resultConfig,
    SILFunction *&resultOrigFn) {
  // Parse differentiability kind.
  if (P.parseToken(tok::l_square, diag::sil_autodiff_expected_lsquare,
                   "differentiability kind"))
    return true;
  resultDiffKind = llvm::StringSwitch<DifferentiabilityKind>(P.Tok.getText())
      .Case("forward", DifferentiabilityKind::Forward)
      .Case("reverse", DifferentiabilityKind::Reverse)
      .Case("normal", DifferentiabilityKind::Normal)
      .Case("linear", DifferentiabilityKind::Linear)
      .Default(DifferentiabilityKind::NonDifferentiable);
  if (resultDiffKind == DifferentiabilityKind::NonDifferentiable) {
    P.diagnose(P.Tok, diag::sil_diff_witness_unknown_kind, P.Tok.getText());
    return true;
  }
  P.consumeToken(tok::identifier);
  if (P.parseToken(tok::r_square, diag::sil_autodiff_expected_rsquare,
                   "differentiability kind"))
    return true;
  // Parse parameter and result indices.
  SmallVector<unsigned, 8> rawParameterIndices;
  SmallVector<unsigned, 8> rawResultIndices;
  if (parseIndexList(P, "parameters", rawParameterIndices,
                     diag::sil_autodiff_expected_parameter_index))
    return true;
  if (parseIndexList(P, "results", rawResultIndices,
                     diag::sil_autodiff_expected_result_index))
    return true;
  // Parse witness generic parameter clause.
  GenericSignature witnessGenSig = GenericSignature();
  SourceLoc witnessGenSigStartLoc = P.getEndOfPreviousLoc();
  {
    auto *genericParams = P.maybeParseGenericParams().getPtrOrNull();
    if (genericParams) {
      witnessGenSig = handleSILGenericParams(genericParams, &P.SF);
    }
  }
  // Parse original function name and type.
  if (SP.parseSILFunctionRef(L, resultOrigFn))
    return true;
  // Resolve parsed witness generic signature.
  if (witnessGenSig) {
    auto origGenSig =
        resultOrigFn->getLoweredFunctionType()->getSubstGenericSignature();
    // Check whether original function generic signature and parsed witness
    // generic have the same generic parameters.
    auto areGenericParametersConsistent = [&]() {
      llvm::SmallDenseSet<GenericParamKey, 4> genericParamKeys;
      for (auto origGP : origGenSig.getGenericParams())
        genericParamKeys.insert(GenericParamKey(origGP.getPointer()));
      for (auto *witnessGP : witnessGenSig.getGenericParams())
        if (!genericParamKeys.erase(GenericParamKey(witnessGP)))
          return false;
      return genericParamKeys.empty();
    };
    if (!areGenericParametersConsistent()) {
      P.diagnose(witnessGenSigStartLoc,
                 diag::sil_diff_witness_invalid_generic_signature,
                 witnessGenSig->getAsString(), origGenSig->getAsString());
      return true;
    }
    // Combine parsed witness requirements with original function generic
    // signature requirements to form full witness generic signature.
    SmallVector<Requirement, 4> witnessRequirements(
        witnessGenSig.getRequirements().begin(),
        witnessGenSig.getRequirements().end());
    witnessGenSig = buildGenericSignature(
        P.Context, origGenSig,
        /*addedGenericParams=*/{},
        std::move(witnessRequirements));
  }
  auto origFnType = resultOrigFn->getLoweredFunctionType();
  auto *parameterIndices = IndexSubset::get(
      P.Context, origFnType->getNumParameters(), rawParameterIndices);
  auto *resultIndices = IndexSubset::get(P.Context,
                                         origFnType->getNumResults() +
                                         origFnType->getNumIndirectMutatingParameters(),
                                         rawResultIndices);
  resultConfig = AutoDiffConfig(parameterIndices, resultIndices, witnessGenSig);
  return false;
}

bool SILParser::parseSILDeclRef(SILDeclRef &Member, bool FnTypeRequired) {
  SourceLoc TyLoc;
  SmallVector<ValueDecl *, 4> values;
  if (parseSILDeclRef(Member, values))
    return true;

  // : ( or : < means that what follows is function type.
  if (!P.Tok.is(tok::colon))
    return false;

  if (FnTypeRequired &&
      !P.peekToken().is(tok::l_paren) &&
      !P.startsWithLess(P.peekToken()))
    return false;

  // Type of the SILDeclRef is optional to be compatible with the old format.
  if (!P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")) {
    // Parse the type for SILDeclRef.
    ParserResult<TypeRepr> TyR = P.parseType();
    if (TyR.isNull())
      return true;

    bindSILGenericParams(TyR.get());

    // The type can be polymorphic.
    GenericSignature genericSig;
    GenericParamList *genericParams = nullptr;
    if (auto *fnType = dyn_cast<FunctionTypeRepr>(TyR.get())) {
      genericSig = fnType->getGenericSignature();
      genericParams = fnType->getGenericParams();
    }

    const auto Ty = performTypeResolution(TyR.get(), /*IsSILType=*/false,
                                          genericSig, genericParams);
    if (Ty->hasError())
      return true;

    // Pick the ValueDecl that has the right type.
    ValueDecl *TheDecl = nullptr;
    auto declTy = Ty->getCanonicalType();
    for (unsigned I = 0, E = values.size(); I < E; ++I) {
      auto *decl = values[I];

      auto lookupTy =
        decl->getInterfaceType()
            ->removeArgumentLabels(decl->getNumCurryLevels());
      if (declTy == lookupTy->getCanonicalType()) {
        TheDecl = decl;
        // Update SILDeclRef to point to the right Decl.
        Member.loc = decl;
        break;
      }
      if (values.size() == 1 && !TheDecl) {
        P.diagnose(TyLoc, diag::sil_member_decl_type_mismatch, declTy,
                   lookupTy);
        return true;
      }
    }
    if (!TheDecl) {
      P.diagnose(TyLoc, diag::sil_member_decl_not_found);
      return true;
    }
  }
  return false;
}

bool
SILParser::parseKeyPathPatternComponent(KeyPathPatternComponent &component,
                                        SmallVectorImpl<SILType> &operandTypes,
                                        SourceLoc componentLoc,
                                        Identifier componentKind,
                                        SILLocation InstLoc,
                                        GenericSignature patternSig,
                                        GenericParamList *patternParams) {
   auto parseComponentIndices =
     [&](SmallVectorImpl<KeyPathPatternComponent::Index> &indexes) -> bool {
       while (true) {
         unsigned index;
         CanType formalTy;
         SILType loweredTy;
         if (P.parseToken(tok::oper_prefix,
                          diag::expected_tok_in_sil_instr, "%")
             || P.parseToken(tok::sil_dollar,
                             diag::expected_tok_in_sil_instr, "$"))
           return true;
         
         if (!P.Tok.is(tok::integer_literal)
             || parseIntegerLiteral(P.Tok.getText(), 0, index))
           return true;
         
         P.consumeToken(tok::integer_literal);
         
         SourceLoc formalTyLoc;
         SourceLoc loweredTyLoc;
         GenericSignature ignoredParsedSig;
         GenericParamList *ignoredParsedParams = nullptr;
         if (P.parseToken(tok::colon,
                          diag::expected_tok_in_sil_instr, ":")
             || P.parseToken(tok::sil_dollar,
                             diag::expected_tok_in_sil_instr, "$")
             || parseASTType(formalTy, formalTyLoc,
                             patternSig, patternParams)
             || P.parseToken(tok::colon,
                             diag::expected_tok_in_sil_instr, ":")
             || parseSILType(loweredTy, loweredTyLoc,
                             ignoredParsedSig, ignoredParsedParams,
                             patternSig, patternParams))
           return true;
         
         if (patternSig)
           loweredTy = SILType::getPrimitiveType(loweredTy.getRawASTType()
                                                     ->mapTypeOutOfContext()
                                                     ->getCanonicalType(),
                                                 loweredTy.getCategory());

         // Formal type must be hashable.
         auto proto = P.Context.getProtocol(KnownProtocolKind::Hashable);
         Type contextFormalTy = formalTy;
         if (patternSig) {
           contextFormalTy = patternSig.getGenericEnvironment()
              ->mapTypeIntoContext(formalTy);
         }
         auto lookup = P.SF.getParentModule()->lookupConformance(
                                                 contextFormalTy, proto);
         if (lookup.isInvalid()) {
           P.diagnose(formalTyLoc,
                      diag::sil_keypath_index_not_hashable,
                      formalTy);
           return true;
         }
         auto conformance = ProtocolConformanceRef(lookup);

         indexes.push_back({index, formalTy, loweredTy, conformance});
         
         if (operandTypes.size() <= index)
           operandTypes.resize(index+1);
         if (operandTypes[index] && operandTypes[index] != loweredTy) {
           P.diagnose(loweredTyLoc,
                      diag::sil_keypath_index_operand_type_conflict, index,
                      operandTypes[index].getRawASTType(),
                      loweredTy.getRawASTType());
           return true;
         }
         operandTypes[index] = loweredTy;
         
         if (P.consumeIf(tok::comma))
           continue;
         if (P.consumeIf(tok::r_square))
           break;
         return true;
       }
       return false;
     };
  
  if (componentKind.str() == "stored_property") {
    ValueDecl *prop;
    CanType ty;
    if (parseSILDottedPath(prop)
        || P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar,
                        diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternSig, patternParams))
      return true;
    component =
      KeyPathPatternComponent::forStoredProperty(cast<VarDecl>(prop), ty);
    return false;
  } else if (componentKind.str() == "gettable_property"
             || componentKind.str() == "settable_property") {
    bool isSettable = componentKind.str()[0] == 's';
    
    CanType componentTy;
    if (P.parseToken(tok::sil_dollar,diag::expected_tok_in_sil_instr,"$")
        || parseASTType(componentTy, patternSig, patternParams)
        || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
    
    SILFunction *idFn = nullptr;
    SILDeclRef idDecl;
    VarDecl *idProperty = nullptr;
    SILFunction *getter = nullptr;
    SILFunction *setter = nullptr;
    SILFunction *equals = nullptr;
    SILFunction *hash = nullptr;
    AbstractStorageDecl *externalDecl = nullptr;
    SubstitutionMap externalSubs;
    SmallVector<KeyPathPatternComponent::Index, 4> indexes;
    while (true) {
      Identifier subKind;
      SourceLoc subKindLoc;
      if (parseSILIdentifier(subKind, subKindLoc,
                             diag::sil_keypath_expected_component_kind))
        return true;

      if (subKind.str() == "id") {
        // The identifier can be either a function ref, a SILDeclRef
        // to a class or protocol method, or a decl ref to a property:
        // @static_fn_ref : $...
        // #Type.method!whatever : (T) -> ...
        // ##Type.property
        if (P.Tok.is(tok::at_sign)) {
          if (parseSILFunctionRef(InstLoc, idFn))
            return true;
        } else if (P.Tok.is(tok::pound)) {
          if (P.peekToken().is(tok::pound)) {
            ValueDecl *propertyValueDecl;
            P.consumeToken(tok::pound);
            if (parseSILDottedPath(propertyValueDecl))
              return true;
            idProperty = cast<VarDecl>(propertyValueDecl);
          } else if (parseSILDeclRef(idDecl, /*fnType*/ true))
            return true;
        } else {
          P.diagnose(subKindLoc, diag::expected_tok_in_sil_instr, "# or @");
          return true;
        }
      } else if (subKind.str() == "getter" || subKind.str() == "setter") {
        bool isSetter = subKind.str()[0] == 's';
        if (parseSILFunctionRef(InstLoc, isSetter ? setter : getter))
          return true;
      } else if (subKind.str() == "indices") {
        if (P.parseToken(tok::l_square,
                         diag::expected_tok_in_sil_instr, "[")
            || parseComponentIndices(indexes))
          return true;
      } else if (subKind.str() == "indices_equals") {
        if (parseSILFunctionRef(InstLoc, equals))
          return true;
      } else if (subKind.str() == "indices_hash") {
        if (parseSILFunctionRef(InstLoc, hash))
          return true;
      } else if (subKind.str() == "external") {
        ValueDecl *parsedExternalDecl;
        SmallVector<ParsedSubstitution, 4> parsedSubs;

        if (parseSILDottedPath(parsedExternalDecl)
            || parseSubstitutions(parsedSubs, patternSig, patternParams))
          return true;

        externalDecl = cast<AbstractStorageDecl>(parsedExternalDecl);

        if (!parsedSubs.empty()) {
          auto genericSig = externalDecl->getInnermostDeclContext()
                                        ->getGenericSignatureOfContext();
          if (!genericSig) {
            P.diagnose(P.Tok,
                       diag::sil_substitutions_on_non_polymorphic_type);
            return true;
          }
          externalSubs = getApplySubstitutionsFromParsed(*this, genericSig,
                                                         parsedSubs);
          if (!externalSubs) return true;

          // Map the substitutions out of the pattern context so that they
          // use interface types.
          externalSubs =
            externalSubs.mapReplacementTypesOutOfContext().getCanonical();
        }

      } else {
        P.diagnose(subKindLoc, diag::sil_keypath_unknown_component_kind,
                   subKind);
        return true;
      }
      
      if (!P.consumeIf(tok::comma))
        break;
    }
    
    if ((idFn == nullptr && idDecl.isNull() && idProperty == nullptr)
        || getter == nullptr
        || (isSettable && setter == nullptr)) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
      return true;
    }
    
    if ((idFn != nullptr) + (!idDecl.isNull()) + (idProperty != nullptr)
          != 1) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
      return true;
    }
    
    KeyPathPatternComponent::ComputedPropertyId id;
    if (idFn)
      id = idFn;
    else if (!idDecl.isNull())
      id = idDecl;
    else if (idProperty)
      id = idProperty;
    else
      llvm_unreachable("no id?!");
    
    auto indexesCopy = P.Context.AllocateCopy(indexes);
    
    if (!indexes.empty() && (!equals || !hash)) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
    }
    
    if (isSettable) {
      component = KeyPathPatternComponent::forComputedSettableProperty(
                             id, getter, setter,
                             indexesCopy, equals, hash,
                             externalDecl, externalSubs, componentTy);
    } else {
      component = KeyPathPatternComponent::forComputedGettableProperty(
                             id, getter,
                             indexesCopy, equals, hash,
                             externalDecl, externalSubs, componentTy);
    }
    return false;
  } else if (componentKind.str() == "optional_wrap"
               || componentKind.str() == "optional_chain"
               || componentKind.str() == "optional_force") {
    CanType ty;
    if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternSig, patternParams))
      return true;
    KeyPathPatternComponent::Kind kind;
    
    if (componentKind.str() == "optional_wrap") {
      kind = KeyPathPatternComponent::Kind::OptionalWrap;
    } else if (componentKind.str() == "optional_chain") {
      kind = KeyPathPatternComponent::Kind::OptionalChain;
    } else if (componentKind.str() == "optional_force") {
      kind = KeyPathPatternComponent::Kind::OptionalForce;
    } else {
      llvm_unreachable("unpossible");
    }
    
    component = KeyPathPatternComponent::forOptional(kind, ty);
    return false;
  } else if (componentKind.str() == "tuple_element") {
    unsigned tupleIndex;
    CanType ty;

    if (P.parseToken(tok::pound, diag::expected_sil_constant)
        || parseInteger(tupleIndex, diag::expected_sil_tuple_index)
        || P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternSig, patternParams))
      return true;
      
    component = KeyPathPatternComponent::forTupleElement(tupleIndex, ty);
    return false;
  } else {
    P.diagnose(componentLoc, diag::sil_keypath_unknown_component_kind,
               componentKind);
    return true;
  }
}

bool SILParser::parseSpecificSILInstruction(SILBuilder &B,
                                            SILInstructionKind Opcode,
                                            SourceLoc OpcodeLoc,
                                            StringRef OpcodeName,
                                            SILInstruction *&ResultVal) {
  SmallVector<SILValue, 4> OpList;
  SILValue Val;
  SILType Ty;
  SILLocation InstLoc = RegularLocation(OpcodeLoc, /*implicit*/ false);
  this->parsedComma = false;

  auto parseFormalTypeAndValue = [&](CanType &formalType,
                                     SILValue &value) -> bool {
    return (parseASTType(formalType) || parseVerbatim("in")
            || parseTypedValueRef(value, B));
  };

  OpenedExistentialAccess AccessKind;
  auto parseOpenExistAddrKind = [&]() -> bool {
    Identifier accessKindToken;
    SourceLoc accessKindLoc;
    if (parseSILIdentifier(accessKindToken, accessKindLoc,
                           diag::expected_tok_in_sil_instr,
                           "opened existential access kind")) {
      return true;
    }
    auto kind =
        llvm::StringSwitch<Optional<OpenedExistentialAccess>>(
            accessKindToken.str())
            .Case("mutable_access", OpenedExistentialAccess::Mutable)
            .Case("immutable_access", OpenedExistentialAccess::Immutable)
            .Default(None);

    if (kind) {
      AccessKind = kind.value();
      return false;
    }
    P.diagnose(accessKindLoc, diag::expected_tok_in_sil_instr,
               "opened existential access kind");
    return true;
  };

  CanType SourceType, TargetType;
  SILValue SourceAddr, DestAddr;
  auto parseSourceAndDestAddress = [&] {
    return parseFormalTypeAndValue(SourceType, SourceAddr) ||
           parseVerbatim("to") || parseFormalTypeAndValue(TargetType, DestAddr);
  };

  Identifier SuccessBBName, FailureBBName;
  SourceLoc SuccessBBLoc, FailureBBLoc;
  auto parseConditionalBranchDestinations = [&] {
    return P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
           || parseSILIdentifier(SuccessBBName, SuccessBBLoc,
                                 diag::expected_sil_block_name)
           || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
           || parseSILIdentifier(FailureBBName, FailureBBLoc,
                                 diag::expected_sil_block_name);
  };

  // Validate the opcode name, and do opcode-specific parsing logic based on the
  // opcode we find.

  switch (Opcode) {
  case SILInstructionKind::AllocBoxInst: {
    bool hasDynamicLifetime = false;
    bool hasReflection = false;
    bool usesMoveableValueDebugInfo = false;
    StringRef attrName;
    SourceLoc attrLoc;
    while (parseSILOptional(attrName, attrLoc, *this)) {
      if (attrName.equals("dynamic_lifetime")) {
        hasDynamicLifetime = true;
      } else if (attrName.equals("reflection")) {
        hasReflection = true;
      } else if (attrName.equals("moveable_value_debuginfo")) {
        usesMoveableValueDebugInfo = true;
      } else {
        P.diagnose(
            attrLoc, diag::sil_invalid_attribute_for_expected, attrName,
            "dynamic_lifetime, reflection, or usesMoveableValueDebugInfo");
      }
    }

    SILType Ty;
    if (parseSILType(Ty))
      return true;
    SILDebugVariable VarInfo;
    if (parseSILDebugVar(VarInfo))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    if (Ty.isMoveOnly())
      usesMoveableValueDebugInfo = true;

    ResultVal = B.createAllocBox(InstLoc, Ty.castTo<SILBoxType>(), VarInfo,
                                 hasDynamicLifetime, hasReflection,
                                 usesMoveableValueDebugInfo);
    break;
  }
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::TryApplyInst:
    if (parseCallInstruction(InstLoc, Opcode, B, ResultVal))
      return true;
    break;
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::EndApplyInst: {
    UnresolvedValueName argName;
    if (parseValueName(argName))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    SILType expectedTy = SILType::getSILTokenType(P.Context);
    SILValue op = getLocalValue(argName, expectedTy, InstLoc, B);

    if (Opcode == SILInstructionKind::AbortApplyInst) {
      ResultVal = B.createAbortApply(InstLoc, op);
    } else {
      ResultVal = B.createEndApply(InstLoc, op);
    }
    break;
  }
  case SILInstructionKind::IntegerLiteralInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    bool Negative = false;
    if (P.Tok.isAnyOperator() && P.Tok.getText() == "-") {
      Negative = true;
      P.consumeToken();
    }
    if (P.Tok.getKind() != tok::integer_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }

    auto intTy = Ty.getAs<AnyBuiltinIntegerType>();
    if (!intTy) {
      P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
      return true;
    }

    StringRef text = prepareIntegerLiteralForParsing(P.Tok.getText());

    bool error;
    APInt value = intTy->getWidth().parse(text, 0, Negative, &error);
    if (error) {
      P.diagnose(P.Tok, diag::sil_integer_literal_not_well_formed, intTy);
      return true;
    }

    P.consumeToken(tok::integer_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIntegerLiteral(InstLoc, Ty, value);
    break;
  }
  case SILInstructionKind::FloatLiteralInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    // The value is expressed as bits.
    if (P.Tok.getKind() != tok::integer_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }

    auto floatTy = Ty.getAs<BuiltinFloatType>();
    if (!floatTy) {
      P.diagnose(P.Tok, diag::sil_float_literal_not_float_type);
      return true;
    }

    StringRef text = prepareIntegerLiteralForParsing(P.Tok.getText());

    APInt bits(floatTy->getBitWidth(), 0);
    bool error = text.getAsInteger(0, bits);
    assert(!error && "float_literal token did not parse as APInt?!");
    (void)error;

    if (bits.getBitWidth() != floatTy->getBitWidth())
      bits = bits.zextOrTrunc(floatTy->getBitWidth());

    APFloat value(floatTy->getAPFloatSemantics(), bits);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createFloatLiteral(InstLoc, Ty, value);
    P.consumeToken(tok::integer_literal);
    break;
  }
  case SILInstructionKind::StringLiteralInst: {
    if (P.Tok.getKind() != tok::identifier) {
      P.diagnose(P.Tok, diag::sil_string_no_encoding);
      return true;
    }

    StringLiteralInst::Encoding encoding;
    if (P.Tok.getText() == "utf8") {
      encoding = StringLiteralInst::Encoding::UTF8;
    } else if (P.Tok.getText() == "objc_selector") {
      encoding = StringLiteralInst::Encoding::ObjCSelector;
    } else if (P.Tok.getText() == "bytes") {
      encoding = StringLiteralInst::Encoding::Bytes;
    } else {
      P.diagnose(P.Tok, diag::sil_string_invalid_encoding, P.Tok.getText());
      return true;
    }
    P.consumeToken(tok::identifier);

    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
      return true;
    }

    // Parse the string.
    SmallVector<Lexer::StringSegment, 1> segments;
    P.L->getStringLiteralSegments(P.Tok, segments);
    assert(segments.size() == 1);

    P.consumeToken(tok::string_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    SmallVector<char, 128> stringBuffer;

    if (encoding == StringLiteralInst::Encoding::Bytes) {
      // Decode hex bytes.
      CharSourceRange rawStringRange(segments.front().Loc,
                                     segments.front().Length);
      StringRef rawString = P.SourceMgr.extractText(rawStringRange);
      if (rawString.size() & 1) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,
                   "even number of hex bytes");
        return true;
      }
      while (!rawString.empty()) {
        unsigned byte1 = llvm::hexDigitValue(rawString[0]);
        unsigned byte2 = llvm::hexDigitValue(rawString[1]);
        if (byte1 == -1U || byte2 == -1U) {
          P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,
                     "hex bytes should contain 0-9, a-f, A-F only");
          return true;
        }
        stringBuffer.push_back((unsigned char)(byte1 << 4) | byte2);
        rawString = rawString.drop_front(2);
      }

      ResultVal = B.createStringLiteral(InstLoc, stringBuffer, encoding);
      break;
    }

    StringRef string =
        P.L->getEncodedStringSegment(segments.front(), stringBuffer);
    ResultVal = B.createStringLiteral(InstLoc, string, encoding);
    break;
  }

  case SILInstructionKind::CondFailInst: {

    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<char, 128> stringBuffer;
    StringRef message;
    if (P.consumeIf(tok::comma)) {
      // Parse the string.
      if (P.Tok.getKind() != tok::string_literal) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
        return true;
      }
      SmallVector<Lexer::StringSegment, 1> segments;
      P.L->getStringLiteralSegments(P.Tok, segments);
      assert(segments.size() == 1);

      P.consumeToken(tok::string_literal);
      message = P.L->getEncodedStringSegment(segments.front(), stringBuffer);
    }
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCondFail(InstLoc, Val, message);
    break;
  }

  case SILInstructionKind::IncrementProfilerCounterInst: {
    // First argument is the counter index.
    unsigned CounterIdx;
    if (parseInteger(CounterIdx, diag::expected_sil_profiler_counter_idx))
      return true;

    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    // Parse the PGO function name.
    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_sil_profiler_counter_pgo_func_name);
      return true;
    }
    // Drop the double quotes.
    auto FuncName = P.Tok.getText().drop_front().drop_back();
    P.consumeToken(tok::string_literal);

    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    // Parse the number of counters.
    if (parseVerbatim("num_counters"))
      return true;

    unsigned NumCounters;
    if (parseInteger(NumCounters, diag::expected_sil_profiler_counter_total))
      return true;

    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    // Parse the PGO function hash.
    if (parseVerbatim("hash"))
      return true;

    uint64_t Hash;
    if (parseInteger(Hash, diag::expected_sil_profiler_counter_hash))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createIncrementProfilerCounter(InstLoc, CounterIdx, FuncName,
                                                 NumCounters, Hash);
    break;
  }

  case SILInstructionKind::ProjectBoxInst: {
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    if (!P.Tok.is(tok::integer_literal)) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }

    unsigned Index;
    bool error = parseIntegerLiteral(P.Tok.getText(), 0, Index);
    assert(!error && "project_box index did not parse as integer?!");
    (void)error;

    P.consumeToken(tok::integer_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createProjectBox(InstLoc, Val, Index);
    break;
  }

  case SILInstructionKind::ProjectExistentialBoxInst: {
    SILType Ty;
    if (parseSILType(Ty) || parseVerbatim("in") || parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createProjectExistentialBox(InstLoc, Ty, Val);
    break;
  }

  case SILInstructionKind::FunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::DynamicFunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) || parseSILDebugLocation(InstLoc, B))
      return true;
    // Set a forward reference's dynamic property for the first time.
    if (!Fn->isDynamicallyReplaceable()) {
      if (!Fn->empty()) {
        P.diagnose(P.Tok, diag::expected_dynamic_func_attr);
        return true;
      }
      Fn->setIsDynamic();
    }
    ResultVal = B.createDynamicFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::PreviousDynamicFunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createPreviousDynamicFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::BuiltinInst: {
    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "builtin name");
      return true;
    }
    StringRef Str = P.Tok.getText();
    Identifier Id = P.Context.getIdentifier(Str.substr(1, Str.size() - 2));
    P.consumeToken(tok::string_literal);

    // Find the builtin in the Builtin module
    SmallVector<ValueDecl *, 2> foundBuiltins;
    P.Context.TheBuiltinModule->lookupMember(
        foundBuiltins, P.Context.TheBuiltinModule, Id, Identifier());
    if (foundBuiltins.empty()) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "builtin name");
      return true;
    }
    assert(foundBuiltins.size() == 1 && "ambiguous builtin name?!");

    auto *builtinFunc = cast<FuncDecl>(foundBuiltins[0]);
    GenericSignature genericSig = builtinFunc->getGenericSignature();

    SmallVector<ParsedSubstitution, 4> parsedSubs;
    SubstitutionMap subMap;
    if (parseSubstitutions(parsedSubs))
      return true;

    if (!parsedSubs.empty()) {
      if (!genericSig) {
        P.diagnose(P.Tok, diag::sil_substitutions_on_non_polymorphic_type);
        return true;
      }
      subMap = getApplySubstitutionsFromParsed(*this, genericSig, parsedSubs);
      if (!subMap)
        return true;
    }

    if (P.Tok.getKind() != tok::l_paren) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "(");
      return true;
    }
    P.consumeToken(tok::l_paren);

    SmallVector<SILValue, 4> Args;
    while (true) {
      if (P.consumeIf(tok::r_paren))
        break;

      SILValue Val;
      if (parseTypedValueRef(Val, B))
        return true;
      Args.push_back(Val);
      if (P.consumeIf(tok::comma))
        continue;
      if (P.consumeIf(tok::r_paren))
        break;
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ")' or ',");
      return true;
    }

    if (P.Tok.getKind() != tok::colon) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ":");
      return true;
    }
    P.consumeToken(tok::colon);

    SILType ResultTy;
    if (parseSILType(ResultTy))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createBuiltin(InstLoc, Id, ResultTy, subMap, Args);
    break;
  }
  case SILInstructionKind::OpenExistentialAddrInst:
    if (parseOpenExistAddrKind() || parseTypedValueRef(Val, B) ||
        parseVerbatim("to") || parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createOpenExistentialAddr(InstLoc, Val, Ty, AccessKind);
    break;

  case SILInstructionKind::OpenExistentialBoxInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createOpenExistentialBox(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialBoxValueInst: {
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal =
        B.createOpenExistentialBoxValue(InstLoc, Val, Ty, forwardingOwnership);
    break;
  }

  case SILInstructionKind::OpenExistentialMetatypeInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createOpenExistentialMetatype(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialRefInst: {
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal =
        B.createOpenExistentialRef(InstLoc, Val, Ty, forwardingOwnership);
    break;
  }

  case SILInstructionKind::OpenExistentialValueInst: {
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal =
        B.createOpenExistentialValue(InstLoc, Val, Ty, forwardingOwnership);
    break;
  }
  case SILInstructionKind::PackLengthInst: {
    CanPackType packType;
    if (P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTPackType(packType))
      return true;
    ResultVal = B.createPackLength(InstLoc, packType);
    break;
  }
  case SILInstructionKind::DynamicPackIndexInst: {
    CanPackType packType;
    if (parseValueRef(Val, SILType::getBuiltinWordType(P.Context), InstLoc, B) ||
        parseVerbatim("of") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTPackType(packType))
      return true;
    ResultVal =
        B.createDynamicPackIndex(InstLoc, Val, packType);
    break;
  }
  case SILInstructionKind::PackPackIndexInst: {
    unsigned componentIndex = 0;
    CanPackType packType;
    if (parseInteger(componentIndex, diag::expected_sil_constant) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseValueRef(Val, SILType::getPackIndexType(P.Context), InstLoc, B) ||
        parseVerbatim("of") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTPackType(packType))
      return true;
    ResultVal =
        B.createPackPackIndex(InstLoc, componentIndex, Val, packType);
    break;
  }
  case SILInstructionKind::ScalarPackIndexInst: {
    unsigned componentIndex = 0;
    CanPackType packType;
    if (parseInteger(componentIndex, diag::expected_sil_constant) ||
        parseVerbatim("of") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTPackType(packType))
      return true;
    ResultVal =
        B.createScalarPackIndex(InstLoc, componentIndex, packType);
    break;
  }
  case SILInstructionKind::OpenPackElementInst: {
    if (parseValueRef(Val, SILType::getPackIndexType(P.Context), InstLoc, B) ||
        parseVerbatim("of"))
      return true;

    // Parse the generic parameters for the environment being opened.
    // This does not include the opened generic parameters.
    GenericParamList *openedGenerics; {
      if (!P.startsWithLess(P.Tok)) {
        P.diagnose(P.Tok, diag::expected_generic_signature);
        return true;
      }
      openedGenerics = P.maybeParseGenericParams().getPtrOrNull();
      if (!openedGenerics)
        return true;
    }

    // Resolve a generic signature from those parameters.
    auto openedGenericsSig = handleSILGenericParams(openedGenerics, &P.SF);
    if (!openedGenericsSig) return true;

    // Parse the substitutions for the environment being opened.
    SubstitutionMap openedSubMap; {
      if (parseVerbatim("at"))
        return true;

      // The substitutions are not contextual within the signature
      // we just parsed.
      SmallVector<ParsedSubstitution> parsedOpenedSubs;
      if (parseSubstitutions(parsedOpenedSubs))
        return true;

      // We do need those substitutions to resolve a SubstitutionMap,
      // though.
      openedSubMap =
        getApplySubstitutionsFromParsed(*this, openedGenericsSig,
                                        parsedOpenedSubs);
      if (!openedSubMap)
        return true;
    }

    // Parse the shape class that should be opened.  This is a contextual
    // type within the signature we just parsed.
    CanType shapeClass;
    SourceLoc shapeClassLoc;
    if (!P.consumeIf(tok::comma) ||
        parseVerbatim("shape") ||
        P.parseToken(tok::sil_dollar,
                     diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(shapeClass, shapeClassLoc, openedGenericsSig,
                     openedGenerics, /*wantContextualType*/ true))
      return true;

    // Map it out of context.  It should be a type pack parameter.
    shapeClass = shapeClass->mapTypeOutOfContext()->getCanonicalType();
    auto shapeParam = dyn_cast<GenericTypeParamType>(shapeClass);
    if (!shapeParam || !shapeParam->isParameterPack()) {
      P.diagnose(shapeClassLoc, diag::opened_shape_class_not_pack_param);
      return true;
    }

    // Parse the UUID for the opening.
    UUID uuid;
    if (!P.consumeIf(tok::comma) ||
        parseVerbatim("uuid") ||
        P.parseUUIDString(uuid, diag::sil_expected_uuid))
      return true;

    // Build the opened-element signature, which adds the parameters for
    // the opened elements to the signature we parsed above.
    auto openedElementSig =
      P.Context.getOpenedElementSignature(
        openedGenericsSig.getCanonicalSignature(), shapeParam);

    auto openedEnv = GenericEnvironment::forOpenedElement(openedElementSig,
                         uuid, shapeParam, openedSubMap);

    auto openInst = B.createOpenPackElement(InstLoc, Val, openedEnv);
    ResultVal = openInst;

    auto &entry = OpenedPackElements[uuid];
    if (entry.DefinitionPoint.isValid()) {
      P.diagnose(OpcodeLoc, diag::multiple_open_pack_element);
      P.diagnose(entry.DefinitionPoint, diag::sil_previous_instruction);
    } else {
      entry.DefinitionPoint = OpcodeLoc;
      entry.Params = openedGenerics;
      entry.Environment = openedEnv;
    }
    break;
  }
  case SILInstructionKind::PackElementGetInst: {
    SILValue index, pack;
    SILType elementType;
    if (parseValueRef(index, SILType::getPackIndexType(P.Context), InstLoc, B) ||
        parseVerbatim("of") ||
        parseTypedValueRef(pack, B) ||
        parseVerbatim("as") ||
        parseSILType(elementType))
      return true;
    ResultVal = B.createPackElementGet(InstLoc, index, pack, elementType);
    break;
  }
  case SILInstructionKind::PackElementSetInst: {
    SILValue value, index, pack;
    if (parseTypedValueRef(value, B) ||
        parseVerbatim("into") ||
        parseValueRef(index, SILType::getPackIndexType(P.Context), InstLoc, B) ||
        parseVerbatim("of") ||
        parseTypedValueRef(pack, B))
      return true;
    ResultVal = B.createPackElementSet(InstLoc, value, index, pack);
    break;
  }
  case SILInstructionKind::TuplePackElementAddrInst: {
    SILValue index, tuple;
    SILType elementType;
    if (parseValueRef(index, SILType::getPackIndexType(P.Context), InstLoc, B) ||
        parseVerbatim("of") ||
        parseTypedValueRef(tuple, B) ||
        parseVerbatim("as") ||
        parseSILType(elementType))
      return true;
    ResultVal = B.createTuplePackElementAddr(InstLoc, index, tuple, elementType);
    break;
  }

#define UNARY_INSTRUCTION(ID)                                                  \
  case SILInstructionKind::ID##Inst:                                           \
    if (parseTypedValueRef(Val, B))                                            \
      return true;                                                             \
    if (parseSILDebugLocation(InstLoc, B))                                     \
      return true;                                                             \
    ResultVal = B.create##ID(InstLoc, Val);                                    \
    break;

#define REFCOUNTING_INSTRUCTION(ID)                                            \
  case SILInstructionKind::ID##Inst: {                                         \
    Atomicity atomicity = Atomicity::Atomic;                                   \
    StringRef Optional;                                                        \
    if (parseSILOptional(Optional, *this)) {                                   \
      if (Optional == "nonatomic") {                                           \
        atomicity = Atomicity::NonAtomic;                                      \
      } else {                                                                 \
        return true;                                                           \
      }                                                                        \
    }                                                                          \
    if (parseTypedValueRef(Val, B))                                            \
      return true;                                                             \
    if (parseSILDebugLocation(InstLoc, B))                                     \
      return true;                                                             \
    ResultVal = B.create##ID(InstLoc, Val, atomicity);                         \
  } break;

    UNARY_INSTRUCTION(ClassifyBridgeObject)
    UNARY_INSTRUCTION(ValueToBridgeObject)
    UNARY_INSTRUCTION(FixLifetime)
    UNARY_INSTRUCTION(EndLifetime)
    UNARY_INSTRUCTION(CopyBlock)
    UNARY_INSTRUCTION(IsUnique)
    UNARY_INSTRUCTION(DestroyAddr)
    UNARY_INSTRUCTION(CopyValue)
    UNARY_INSTRUCTION(ExplicitCopyValue)
    UNARY_INSTRUCTION(EndBorrow)
    UNARY_INSTRUCTION(DestructureStruct)
    UNARY_INSTRUCTION(DestructureTuple)
    UNARY_INSTRUCTION(ExtractExecutor)
    REFCOUNTING_INSTRUCTION(UnmanagedReleaseValue)
    REFCOUNTING_INSTRUCTION(UnmanagedRetainValue)
    REFCOUNTING_INSTRUCTION(UnmanagedAutoreleaseValue)
    REFCOUNTING_INSTRUCTION(StrongRetain)
    REFCOUNTING_INSTRUCTION(StrongRelease)
    REFCOUNTING_INSTRUCTION(AutoreleaseValue)
    REFCOUNTING_INSTRUCTION(SetDeallocating)
    REFCOUNTING_INSTRUCTION(ReleaseValue)
    REFCOUNTING_INSTRUCTION(RetainValue)
    REFCOUNTING_INSTRUCTION(ReleaseValueAddr)
    REFCOUNTING_INSTRUCTION(RetainValueAddr)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  UNARY_INSTRUCTION(StrongCopy##Name##Value)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  REFCOUNTING_INSTRUCTION(StrongRetain##Name)                                  \
  REFCOUNTING_INSTRUCTION(Name##Retain)                                        \
  REFCOUNTING_INSTRUCTION(Name##Release)                                       \
  UNARY_INSTRUCTION(StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"
#undef UNARY_INSTRUCTION
#undef REFCOUNTING_INSTRUCTION

  case SILInstructionKind::HopToExecutorInst: {
    bool mandatory = false;
    if (parseSILOptional(mandatory, *this, "mandatory")
        || parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createHopToExecutor(InstLoc, Val, mandatory);
    break;
  }
  case SILInstructionKind::DestroyValueInst: {
    bool poisonRefs = false;
    if (parseSILOptional(poisonRefs, *this, "poison")
        || parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDestroyValue(InstLoc, Val, poisonRefs);
    break;
  }
  case SILInstructionKind::BeginCOWMutationInst: {
    bool native = false;
    if (parseSILOptional(native, *this, "native") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createBeginCOWMutation(InstLoc, Val, native);
    break;
  }
  case SILInstructionKind::EndCOWMutationInst: {
    bool keepUnique = false;
    if (parseSILOptional(keepUnique, *this, "keep_unique") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createEndCOWMutation(InstLoc, Val, keepUnique);
    break;
  }
  case SILInstructionKind::IsEscapingClosureInst: {
    bool IsObjcVerificationType = false;
    if (parseSILOptional(IsObjcVerificationType, *this, "objc"))
      return true;
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIsEscapingClosure(
        InstLoc, Val,
        IsObjcVerificationType ? IsEscapingClosureInst::ObjCEscaping
                              : IsEscapingClosureInst::WithoutActuallyEscaping);
    break;
  }

  case SILInstructionKind::DebugValueInst: {
    bool poisonRefs = false;
    bool hasTrace = false;
    bool usesMoveableValueDebugInfo = false;
    SILDebugVariable VarInfo;

    // Allow for poison and moved to be in either order.
    StringRef attributeName;
    SourceLoc attributeLoc;
    while (parseSILOptional(attributeName, attributeLoc, *this)) {
      if (attributeName == "poison")
        poisonRefs = true;
      else if (attributeName == "trace")
        hasTrace = true;
      else if (attributeName == "moveable_value_debuginfo")
        usesMoveableValueDebugInfo = true;
      else {
        P.diagnose(attributeLoc, diag::sil_invalid_attribute_for_instruction,
                   attributeName, "debug_value");
        return true;
      }
    }

    if (parseTypedValueRef(Val, B) || parseSILDebugVar(VarInfo) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    if (Val->getType().isAddress())
      assert(!poisonRefs && "debug_value w/ address value does not support poison");

    if (Val->getType().isMoveOnly())
      usesMoveableValueDebugInfo = true;

    ResultVal = B.createDebugValue(InstLoc, Val, VarInfo, poisonRefs,
                                   usesMoveableValueDebugInfo, hasTrace);
    break;
  }

  case SILInstructionKind::DebugStepInst:
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDebugStep(InstLoc);
    break;

  case SILInstructionKind::TestSpecificationInst: {
    // Parse the specification string.
    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_sil_test_specification_body);
      return true;
    }
    // Drop the double quotes.
    unsigned numQuotes = P.Tok.isMultilineString() ? 4 : 1;
    auto ArgumentsSpecification =
      P.Tok.getText().drop_front(numQuotes).drop_back(numQuotes).trim();
    P.consumeToken(tok::string_literal);
    ResultVal = B.createTestSpecificationInst(InstLoc, ArgumentsSpecification);
    break;
  }

    // unchecked_ownership_conversion <reg> : <type>, <ownership> to <ownership>
  case SILInstructionKind::UncheckedOwnershipConversionInst: {
    ValueOwnershipKind LHSKind = OwnershipKind::None;
    ValueOwnershipKind RHSKind = OwnershipKind::None;
    SourceLoc Loc;

    if (parseTypedValueRef(Val, Loc, B) ||
        P.parseToken(tok::comma, diag::expected_sil_colon,
                     "unchecked_ownership_conversion value ownership kind "
                     "conversion specification") ||
        parseSILOwnership(LHSKind) || parseVerbatim("to") ||
        parseSILOwnership(RHSKind) || parseSILDebugLocation(InstLoc, B)) {
      return true;
    }

    if (Val->getOwnershipKind() != LHSKind) {
      return true;
    }

    ResultVal = B.createUncheckedOwnershipConversion(InstLoc, Val, RHSKind);
    break;
  }

  case SILInstructionKind::MoveValueInst: {
    bool allowsDiagnostics = false;
    bool isLexical = false;

    StringRef AttrName;
    SourceLoc AttrLoc;
    while (parseSILOptional(AttrName, AttrLoc, *this)) {
      if (AttrName == "allows_diagnostics")
        allowsDiagnostics = true;
      else if (AttrName == "lexical")
        isLexical = true;
      else {
        P.diagnose(InstLoc.getSourceLoc(),
                   diag::sil_invalid_attribute_for_instruction, AttrName,
                   "move_value");
        return true;
      }
    }

    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    auto *MVI = B.createMoveValue(InstLoc, Val, isLexical);
    MVI->setAllowsDiagnostics(allowsDiagnostics);
    ResultVal = MVI;
    break;
  }

  case SILInstructionKind::DropDeinitInst: {
    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDropDeinit(InstLoc, Val);
    break;
  }

  case SILInstructionKind::MarkMustCheckInst: {
    StringRef AttrName;
    if (!parseSILOptional(AttrName, *this)) {
      auto diag = diag::sil_markmustcheck_requires_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag);
      return true;
    }

    using CheckKind = MarkMustCheckInst::CheckKind;
    CheckKind CKind =
        llvm::StringSwitch<CheckKind>(AttrName)
            .Case("consumable_and_assignable",
                  CheckKind::ConsumableAndAssignable)
            .Case("no_consume_or_assign", CheckKind::NoConsumeOrAssign)
            .Case("assignable_but_not_consumable",
                  CheckKind::AssignableButNotConsumable)
            .Case("initable_but_not_consumable",
                  CheckKind::InitableButNotConsumable)
            .Default(CheckKind::Invalid);

    if (CKind == CheckKind::Invalid) {
      auto diag = diag::sil_markmustcheck_invalid_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag, AttrName);
      return true;
    }

    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    auto *MVI = B.createMarkMustCheckInst(InstLoc, Val, CKind);
    ResultVal = MVI;
    break;
  }

  case SILInstructionKind::MarkUnresolvedReferenceBindingInst: {
    StringRef AttrName;
    if (!parseSILOptional(AttrName, *this)) {
      auto diag = diag::sil_markuncheckedreferencebinding_requires_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag);
      return true;
    }

    using Kind = MarkUnresolvedReferenceBindingInst::Kind;
    Kind CKind = llvm::StringSwitch<Kind>(AttrName)
                     .Case("inout", Kind::InOut)
                     .Default(Kind::Invalid);

    if (CKind == Kind::Invalid) {
      auto diag = diag::sil_markuncheckedreferencebinding_invalid_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag, AttrName);
      return true;
    }

    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    auto *MVI = B.createMarkUnresolvedReferenceBindingInst(InstLoc, Val, CKind);
    ResultVal = MVI;
    break;
  }

  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst: {
    StringRef AttrName;
    if (!parseSILOptional(AttrName, *this)) {
      auto diag = diag::sil_moveonlytocopyable_requires_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag);
      return true;
    }

    OwnershipKind OwnershipKind =
        llvm::StringSwitch<ValueOwnershipKind>(AttrName)
            .Case("owned", OwnershipKind::Owned)
            .Case("guaranteed", OwnershipKind::Guaranteed)
            .Default(OwnershipKind::None);

    if (OwnershipKind == OwnershipKind::None) {
      auto diag = diag::sil_moveonlytocopyable_invalid_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag, AttrName);
      return true;
    }

    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    if (OwnershipKind == OwnershipKind::Owned)
      ResultVal = B.createOwnedCopyableToMoveOnlyWrapperValue(InstLoc, Val);
    else
      ResultVal =
          B.createGuaranteedCopyableToMoveOnlyWrapperValue(InstLoc, Val);
    break;
  }

  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst: {
    StringRef AttrName;
    if (!parseSILOptional(AttrName, *this)) {
      auto diag = diag::sil_moveonlytocopyable_requires_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag);
      return true;
    }

    OwnershipKind OwnershipKind =
        llvm::StringSwitch<ValueOwnershipKind>(AttrName)
            .Case("owned", OwnershipKind::Owned)
            .Case("guaranteed", OwnershipKind::Guaranteed)
            .Default(OwnershipKind::None);

    if (OwnershipKind == OwnershipKind::None) {
      auto diag = diag::sil_moveonlytocopyable_invalid_attribute;
      P.diagnose(InstLoc.getSourceLoc(), diag, AttrName);
      return true;
    }

    if (parseTypedValueRef(Val, B))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    if (OwnershipKind == OwnershipKind::Owned)
      ResultVal = B.createOwnedMoveOnlyWrapperToCopyableValue(InstLoc, Val);
    else
      ResultVal =
          B.createGuaranteedMoveOnlyWrapperToCopyableValue(InstLoc, Val);
    break;
  }

  case SILInstructionKind::LoadInst: {
    Optional<LoadOwnershipQualifier> Qualifier;
    SourceLoc AddrLoc;
    auto parseLoadOwnership = [](StringRef Str) {
      return llvm::StringSwitch<Optional<LoadOwnershipQualifier>>(Str)
          .Case("take", LoadOwnershipQualifier::Take)
          .Case("copy", LoadOwnershipQualifier::Copy)
          .Case("trivial", LoadOwnershipQualifier::Trivial)
          .Default(None);
    };
    if (parseSILQualifier<LoadOwnershipQualifier>(Qualifier, parseLoadOwnership)
        || parseTypedValueRef(Val, AddrLoc, B)
        || parseSILDebugLocation(InstLoc, B)) {
      return true;
    }
    if (!Qualifier)
      Qualifier = LoadOwnershipQualifier::Unqualified;
    ResultVal = B.createLoad(InstLoc, Val, Qualifier.value());
    break;
  }

  case SILInstructionKind::LoadBorrowInst: {
    SourceLoc AddrLoc;

    if (parseTypedValueRef(Val, AddrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createLoadBorrow(InstLoc, Val);
    break;
  }

  case SILInstructionKind::BeginBorrowInst: {
    SourceLoc AddrLoc;

    bool isLexical = false;
    if (parseSILOptional(isLexical, *this, "lexical"))
      return true;

    if (parseTypedValueRef(Val, AddrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createBeginBorrow(InstLoc, Val, isLexical);
    break;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Load##Name##Inst: {                                 \
    bool isTake = false;                                                       \
    SourceLoc addrLoc;                                                         \
    if (parseSILOptional(isTake, *this, "take") ||                             \
        parseTypedValueRef(Val, addrLoc, B) ||                                 \
        parseSILDebugLocation(InstLoc, B))                                     \
      return true;                                                             \
    if (!Val->getType().is<Name##StorageType>()) {                             \
      P.diagnose(addrLoc, diag::sil_operand_not_ref_storage_address, "source", \
                 OpcodeName, ReferenceOwnership::Name);                        \
    }                                                                          \
    ResultVal = B.createLoad##Name(InstLoc, Val, IsTake_t(isTake));            \
    break;                                                                     \
  }
#include "swift/AST/ReferenceStorage.def"

  case SILInstructionKind::CopyBlockWithoutEscapingInst: {
    SILValue Closure;
    if (parseTypedValueRef(Val, B) || parseVerbatim("withoutEscaping") ||
        parseTypedValueRef(Closure, B) || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCopyBlockWithoutEscaping(InstLoc, Val, Closure);
    break;
  }

  case SILInstructionKind::MarkDependenceInst: {
    SILValue Base;
    if (parseTypedValueRef(Val, B) || parseVerbatim("on") ||
        parseTypedValueRef(Base, B))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createMarkDependence(InstLoc, Val, Base, forwardingOwnership);
    break;
  }

  case SILInstructionKind::KeyPathInst: {
    SmallVector<KeyPathPatternComponent, 4> components;
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    GenericParamList *patternParams = nullptr;
    GenericSignature patternSig;
    CanType rootType;
    StringRef objcString;
    SmallVector<SILType, 4> operandTypes;
    {
      patternParams = P.maybeParseGenericParams().getPtrOrNull();
      patternSig = handleSILGenericParams(patternParams, &P.SF);

      if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
        return true;

      while (true) {
        Identifier componentKind;
        SourceLoc componentLoc;
        if (parseSILIdentifier(componentKind, componentLoc,
                               diag::sil_keypath_expected_component_kind))
          return true;

        if (componentKind.str() == "root") {
          if (P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr,
                           "$") ||
              parseASTType(rootType, patternSig, patternParams))
            return true;
        } else if (componentKind.str() == "objc") {
          auto tok = P.Tok;
          if (P.parseToken(tok::string_literal, diag::expected_tok_in_sil_instr,
                           "string literal"))
            return true;

          auto objcStringValue = tok.getText().drop_front().drop_back();
          objcString =
              StringRef(P.Context.AllocateCopy<char>(objcStringValue.begin(),
                                                     objcStringValue.end()),
                        objcStringValue.size());
        } else {
          KeyPathPatternComponent component;
          if (parseKeyPathPatternComponent(component, operandTypes,
                                           componentLoc, componentKind, InstLoc,
                                           patternSig, patternParams))
            return true;
          components.push_back(component);
        }

        if (!P.consumeIf(tok::semi))
          break;
      }

      if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")") ||
          parseSILDebugLocation(InstLoc, B))
        return true;
    }

    if (rootType.isNull())
      P.diagnose(InstLoc.getSourceLoc(), diag::sil_keypath_no_root);

    SmallVector<ParsedSubstitution, 4> parsedSubs;
    if (parseSubstitutions(parsedSubs, ContextGenericSig, ContextGenericParams))
      return true;

    SubstitutionMap subMap;
    if (!parsedSubs.empty()) {
      if (!patternSig) {
        P.diagnose(InstLoc.getSourceLoc(),
                   diag::sil_substitutions_on_non_polymorphic_type);
        return true;
      }

      subMap = getApplySubstitutionsFromParsed(*this, patternSig, parsedSubs);
      if (!subMap)
        return true;
    }

    SmallVector<SILValue, 4> operands;

    if (P.consumeIf(tok::l_paren)) {
      while (true) {
        SILValue v;

        if (operands.size() >= operandTypes.size() ||
            !operandTypes[operands.size()]) {
          P.diagnose(P.Tok, diag::sil_keypath_no_use_of_operand_in_pattern,
                     operands.size());
          return true;
        }

        auto ty = operandTypes[operands.size()].subst(SILMod, subMap);

        if (parseValueRef(v, ty, RegularLocation(P.Tok.getLoc()), B))
          return true;
        operands.push_back(v);

        if (P.consumeIf(tok::comma))
          continue;
        if (P.consumeIf(tok::r_paren))
          break;
        return true;
      }
    }

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    CanGenericSignature canSig;
    if (patternSig) {
      canSig = patternSig.getCanonicalSignature();
    }
    CanType leafType;
    if (!components.empty())
      leafType = components.back().getComponentType();
    else
      leafType = rootType;
    auto pattern = KeyPathPattern::get(B.getModule(), canSig, rootType,
                                       leafType, components, objcString);

    ResultVal = B.createKeyPath(InstLoc, pattern, subMap, operands, Ty);
    break;
  }

    // Conversion instructions.
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UncheckedValueCastInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::BridgeObjectToWordInst:
  case SILInstructionKind::RefToRawPointerInst:
  case SILInstructionKind::RawPointerToRefInst:
#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  case SILInstructionKind::RefTo##Name##Inst:                                  \
  case SILInstructionKind::Name##ToRefInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::ThinToThickFunctionInst:
  case SILInstructionKind::ThickToObjCMetatypeInst:
  case SILInstructionKind::ObjCToThickMetatypeInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::ConvertEscapeToNoEscapeInst:
  case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
  case SILInstructionKind::ObjCMetatypeToObjectInst: {
    SILType Ty;
    Identifier ToToken;
    SourceLoc ToLoc;
    bool not_guaranteed = false;
    bool without_actually_escaping = false;
    bool needsStackProtection = false;
    if (Opcode == SILInstructionKind::ConvertEscapeToNoEscapeInst) {
      StringRef attrName;
      if (parseSILOptional(attrName, *this)) {
        if (attrName.equals("not_guaranteed"))
          not_guaranteed = true;
        else
          return true;
      }
    } if (Opcode == SILInstructionKind::AddressToPointerInst) {
      if (parseSILOptional(needsStackProtection, *this, "stack_protection"))
        return true;
    }
  
    if (parseTypedValueRef(Val, B) ||
        parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                           "to"))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }
    if (Opcode == SILInstructionKind::ConvertFunctionInst) {
      StringRef attrName;
      if (parseSILOptional(attrName, *this)) {
        if (attrName.equals("without_actually_escaping"))
          without_actually_escaping = true;
        else
          return true;
      }
    }
    if (parseSILType(Ty))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (OwnershipForwardingMixin::isa(Opcode)) {
      if (parseForwardingOwnershipKind(forwardingOwnership))
        return true;
    }

    if (parseSILDebugLocation(InstLoc, B)) {
      return true;
    }

    switch (Opcode) {
    default:
      llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::UncheckedRefCastInst:
      ResultVal =
          B.createUncheckedRefCast(InstLoc, Val, Ty, forwardingOwnership);
      break;
    case SILInstructionKind::UncheckedAddrCastInst:
      ResultVal = B.createUncheckedAddrCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedTrivialBitCastInst:
      ResultVal = B.createUncheckedTrivialBitCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedBitwiseCastInst:
      ResultVal = B.createUncheckedBitwiseCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedValueCastInst:
      ResultVal =
          B.createUncheckedValueCast(InstLoc, Val, Ty, forwardingOwnership);
      break;
    case SILInstructionKind::UpcastInst:
      ResultVal = B.createUpcast(InstLoc, Val, Ty, forwardingOwnership);
      break;
    case SILInstructionKind::ConvertFunctionInst:
      ResultVal = B.createConvertFunction(
          InstLoc, Val, Ty, without_actually_escaping, forwardingOwnership);
      break;
    case SILInstructionKind::ConvertEscapeToNoEscapeInst:
      ResultVal =
          B.createConvertEscapeToNoEscape(InstLoc, Val, Ty, !not_guaranteed);
      break;
    case SILInstructionKind::AddressToPointerInst:
      ResultVal = B.createAddressToPointer(InstLoc, Val, Ty, needsStackProtection);
      break;
    case SILInstructionKind::BridgeObjectToRefInst:
      ResultVal =
          B.createBridgeObjectToRef(InstLoc, Val, Ty, forwardingOwnership);
      break;
    case SILInstructionKind::BridgeObjectToWordInst:
      ResultVal = B.createBridgeObjectToWord(InstLoc, Val);
      break;
    case SILInstructionKind::RefToRawPointerInst:
      ResultVal = B.createRefToRawPointer(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::RawPointerToRefInst:
      ResultVal = B.createRawPointerToRef(InstLoc, Val, Ty);
      break;
#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  case SILInstructionKind::RefTo##Name##Inst:                                  \
    ResultVal = B.createRefTo##Name(InstLoc, Val, Ty);                         \
    break;                                                                     \
  case SILInstructionKind::Name##ToRefInst:                                    \
    ResultVal = B.create##Name##ToRef(InstLoc, Val, Ty);                       \
    break;
#include "swift/AST/ReferenceStorage.def"
    case SILInstructionKind::ThinToThickFunctionInst:
      ResultVal =
          B.createThinToThickFunction(InstLoc, Val, Ty, forwardingOwnership);
      break;
    case SILInstructionKind::ThickToObjCMetatypeInst:
      ResultVal = B.createThickToObjCMetatype(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCToThickMetatypeInst:
      ResultVal = B.createObjCToThickMetatype(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCMetatypeToObjectInst:
      ResultVal = B.createObjCMetatypeToObject(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
      ResultVal = B.createObjCExistentialMetatypeToObject(InstLoc, Val, Ty);
      break;
    }
    break;
  }
  case SILInstructionKind::PointerToAddressInst: {
    SILType Ty;
    Identifier ToToken;
    SourceLoc ToLoc;
    StringRef attr;
    if (parseTypedValueRef(Val, B) ||
        parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                           "to"))
      return true;

    bool isStrict = false;
    bool isInvariant = false;
    llvm::MaybeAlign alignment;
    uint64_t parsedValue = 0;
    while (parseSILOptional(attr, parsedValue, ToLoc, *this)) {
      if (attr.empty())
        return true;

      if (attr.equals("strict"))
        isStrict = true;

      if (attr.equals("invariant"))
        isInvariant = true;

      if (attr.equals("align"))
        alignment = llvm::Align(parsedValue);
    }

    if (parseSILType(Ty) || parseSILDebugLocation(InstLoc, B))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    ResultVal = B.createPointerToAddress(InstLoc, Val, Ty, isStrict,
                                         isInvariant, alignment);
    break;
  }
  case SILInstructionKind::RefToBridgeObjectInst: {
    SILValue BitsVal;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(BitsVal, B))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal =
        B.createRefToBridgeObject(InstLoc, Val, BitsVal, forwardingOwnership);
    break;
  }

  case SILInstructionKind::CheckedCastAddrBranchInst: {
    Identifier consumptionKindToken;
    SourceLoc consumptionKindLoc;
    if (parseSILIdentifier(consumptionKindToken, consumptionKindLoc,
                           diag::expected_tok_in_sil_instr,
                           "cast consumption kind")) {
      return true;
    }
    // NOTE: BorrowAlways is not a supported cast kind for address types, so we
    // purposely do not parse it here.
    auto kind = llvm::StringSwitch<Optional<CastConsumptionKind>>(
                    consumptionKindToken.str())
                    .Case("take_always", CastConsumptionKind::TakeAlways)
                    .Case("take_on_success", CastConsumptionKind::TakeOnSuccess)
                    .Case("copy_on_success", CastConsumptionKind::CopyOnSuccess)
                    .Default(None);

    if (!kind) {
      P.diagnose(consumptionKindLoc, diag::expected_tok_in_sil_instr,
                 "cast consumption kind");
      return true;
    }
    auto consumptionKind = kind.value();

    if (parseSourceAndDestAddress() || parseConditionalBranchDestinations() ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCheckedCastAddrBranch(
        InstLoc, consumptionKind, SourceAddr, SourceType, DestAddr, TargetType,
        getBBForReference(SuccessBBName, SuccessBBLoc),
        getBBForReference(FailureBBName, FailureBBLoc));
    break;
  }
  case SILInstructionKind::UncheckedRefCastAddrInst:
    if (parseSourceAndDestAddress() || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUncheckedRefCastAddr(InstLoc, SourceAddr, SourceType,
                                             DestAddr, TargetType);
    break;

  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
    if (parseSourceAndDestAddress() || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUnconditionalCheckedCastAddr(
        InstLoc, SourceAddr, SourceType, DestAddr, TargetType);
    break;

  case SILInstructionKind::UnconditionalCheckedCastInst: {
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") ||
        parseASTType(TargetType))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    auto opaque = Lowering::AbstractionPattern::getOpaque();
    ResultVal = B.createUnconditionalCheckedCast(
        InstLoc, Val, F->getLoweredType(opaque, TargetType), TargetType,
        forwardingOwnership);
    break;
  }

  case SILInstructionKind::CheckedCastBranchInst: {
    bool isExact = false;
    if (Opcode == SILInstructionKind::CheckedCastBranchInst &&
        parseSILOptional(isExact, *this, "exact"))
      return true;

    if (parseTypedValueRef(Val, B) || parseVerbatim("to") ||
        parseASTType(TargetType) || parseConditionalBranchDestinations())
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B)) {
      return true;
    }

    auto opaque = Lowering::AbstractionPattern::getOpaque();
    ResultVal = B.createCheckedCastBranch(
        InstLoc, isExact, Val, F->getLoweredType(opaque, TargetType),
        TargetType, getBBForReference(SuccessBBName, SuccessBBLoc),
        getBBForReference(FailureBBName, FailureBBLoc), forwardingOwnership);
    break;
  }
  case SILInstructionKind::MarkUninitializedInst: {
    if (P.parseToken(tok::l_square, diag::expected_tok_in_sil_instr, "["))
      return true;

    Identifier KindId;
    SourceLoc KindLoc = P.Tok.getLoc();
    if (P.consumeIf(tok::kw_var))
      KindId = P.Context.getIdentifier("var");
    else if (P.parseIdentifier(KindId, KindLoc, /*diagnoseDollarPrefix=*/false,
                               diag::expected_tok_in_sil_instr, "kind"))
      return true;

    if (P.parseToken(tok::r_square, diag::expected_tok_in_sil_instr, "]"))
      return true;

    MarkUninitializedInst::Kind Kind;
    if (KindId.str() == "var")
      Kind = MarkUninitializedInst::Var;
    else if (KindId.str() == "rootself")
      Kind = MarkUninitializedInst::RootSelf;
    else if (KindId.str() == "crossmodulerootself")
      Kind = MarkUninitializedInst::CrossModuleRootSelf;
    else if (KindId.str() == "derivedself")
      Kind = MarkUninitializedInst::DerivedSelf;
    else if (KindId.str() == "derivedselfonly")
      Kind = MarkUninitializedInst::DerivedSelfOnly;
    else if (KindId.str() == "delegatingself")
      Kind = MarkUninitializedInst::DelegatingSelf;
    else if (KindId.str() == "delegatingselfallocated")
      Kind = MarkUninitializedInst::DelegatingSelfAllocated;
    else {
      P.diagnose(KindLoc, diag::expected_tok_in_sil_instr,
                 "var, rootself, crossmodulerootself, derivedself, "
                 "derivedselfonly, delegatingself, or delegatingselfallocated");
      return true;
    }

    if (parseTypedValueRef(Val, B))
      return true;

    ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
    if (parseForwardingOwnershipKind(forwardingOwnership)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal =
        B.createMarkUninitialized(InstLoc, Val, Kind, forwardingOwnership);
    break;
  }

  case SILInstructionKind::MarkFunctionEscapeInst: {
    SmallVector<SILValue, 4> OpList;
    do {
      if (parseTypedValueRef(Val, B))
        return true;
      OpList.push_back(Val);
    } while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma));

    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createMarkFunctionEscape(InstLoc, OpList);
    break;
  }

  case SILInstructionKind::AssignInst:
  case SILInstructionKind::StoreInst: {
    UnresolvedValueName From;
    SourceLoc ToLoc, AddrLoc;
    Identifier ToToken;
    SILValue AddrVal;
    Optional<StoreOwnershipQualifier> StoreQualifier;
    Optional<AssignOwnershipQualifier> AssignQualifier;
    bool IsStore = Opcode == SILInstructionKind::StoreInst;
    bool IsAssign = Opcode == SILInstructionKind::AssignInst;
    if (parseValueName(From) ||
        parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                           "to"))
      return true;

    auto parseStoreOwnership = [](StringRef Str) {
      return llvm::StringSwitch<Optional<StoreOwnershipQualifier>>(Str)
          .Case("init", StoreOwnershipQualifier::Init)
          .Case("assign", StoreOwnershipQualifier::Assign)
          .Case("trivial", StoreOwnershipQualifier::Trivial)
          .Default(None);
    };
    if (IsStore
        && parseSILQualifier<StoreOwnershipQualifier>(StoreQualifier,
                                                      parseStoreOwnership))
      return true;

    auto parseAssignOwnership = [](StringRef Str) {
      return llvm::StringSwitch<Optional<AssignOwnershipQualifier>>(Str)
          .Case("reassign", AssignOwnershipQualifier::Reassign)
          .Case("reinit", AssignOwnershipQualifier::Reinit)
          .Case("init", AssignOwnershipQualifier::Init)
          .Default(None);
    };
    if (IsAssign
        && parseSILQualifier<AssignOwnershipQualifier>(AssignQualifier,
                                                       parseAssignOwnership))
      return true;

    if (parseTypedValueRef(AddrVal, AddrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    if (!AddrVal->getType().isAddress()) {
      P.diagnose(AddrLoc, diag::sil_operand_not_address, "destination",
                 OpcodeName);
      return true;
    }

    SILType ValType = AddrVal->getType().getObjectType();

    if (IsStore) {
      if (!StoreQualifier)
        StoreQualifier = StoreOwnershipQualifier::Unqualified;
      ResultVal =
          B.createStore(InstLoc, getLocalValue(From, ValType, InstLoc, B),
                        AddrVal, StoreQualifier.value());
    } else {
      assert(IsAssign);
      if (!AssignQualifier)
        AssignQualifier = AssignOwnershipQualifier::Unknown;

      ResultVal =
          B.createAssign(InstLoc, getLocalValue(From, ValType, InstLoc, B),
                         AddrVal, AssignQualifier.value());
    }

    break;
  }

  case SILInstructionKind::AssignByWrapperInst: {
    SILValue Src, DestAddr, InitFn, SetFn;
    SourceLoc DestLoc;
    AssignByWrapperInst::Mode mode;

    if (parseTypedValueRef(Src, B) || parseVerbatim("to") ||
        parseAssignByWrapperMode(mode, *this) ||
        parseTypedValueRef(DestAddr, DestLoc, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("init") || parseTypedValueRef(InitFn, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("set") || parseTypedValueRef(SetFn, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (!DestAddr->getType().isAddress()) {
      P.diagnose(DestLoc, diag::sil_operand_not_address, "destination",
                 OpcodeName);
      return true;
    }

    ResultVal = B.createAssignByWrapper(InstLoc, Src, DestAddr,
                                        InitFn, SetFn, mode);
    break;
  }

  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginUnpairedAccessInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndUnpairedAccessInst: {
    ParsedEnum<SILAccessKind> kind;
    ParsedEnum<SILAccessEnforcement> enforcement;
    ParsedEnum<bool> aborting;
    ParsedEnum<bool> noNestedConflict;
    ParsedEnum<bool> fromBuiltin;

    bool isBeginAccess =
        (Opcode == SILInstructionKind::BeginAccessInst ||
         Opcode == SILInstructionKind::BeginUnpairedAccessInst);
    bool wantsEnforcement =
        (isBeginAccess || Opcode == SILInstructionKind::EndUnpairedAccessInst);

    while (P.consumeIf(tok::l_square)) {
      Identifier ident;
      SourceLoc identLoc;
      if (parseSILIdentifier(ident, identLoc,
                             diag::expected_in_attribute_list)) {
        if (P.consumeIf(tok::r_square)) {
          continue;
        } else {
          return true;
        }
      }
      StringRef attr = ident.str();

      auto setEnforcement = [&](SILAccessEnforcement value) {
        maybeSetEnum(wantsEnforcement, enforcement, value, attr, identLoc);
      };
      auto setKind = [&](SILAccessKind value) {
        maybeSetEnum(isBeginAccess, kind, value, attr, identLoc);
      };
      auto setAborting = [&](bool value) {
        maybeSetEnum(!isBeginAccess, aborting, value, attr, identLoc);
      };
      auto setNoNestedConflict = [&](bool value) {
        maybeSetEnum(isBeginAccess, noNestedConflict, value, attr, identLoc);
      };
      auto setFromBuiltin = [&](bool value) {
        maybeSetEnum(Opcode != SILInstructionKind::EndAccessInst, fromBuiltin,
                     value, attr, identLoc);
      };

      if (attr == "unknown") {
        setEnforcement(SILAccessEnforcement::Unknown);
      } else if (attr == "static") {
        setEnforcement(SILAccessEnforcement::Static);
      } else if (attr == "dynamic") {
        setEnforcement(SILAccessEnforcement::Dynamic);
      } else if (attr == "unsafe") {
        setEnforcement(SILAccessEnforcement::Unsafe);
      } else if (attr == "signed") {
        setEnforcement(SILAccessEnforcement::Signed);
      } else if (attr == "init") {
        setKind(SILAccessKind::Init);
      } else if (attr == "read") {
        setKind(SILAccessKind::Read);
      } else if (attr == "modify") {
        setKind(SILAccessKind::Modify);
      } else if (attr == "deinit") {
        setKind(SILAccessKind::Deinit);
      } else if (attr == "abort") {
        setAborting(true);
      } else if (attr == "no_nested_conflict") {
        setNoNestedConflict(true);
      } else if (attr == "builtin") {
        setFromBuiltin(true);
      } else {
        P.diagnose(identLoc, diag::unknown_attribute, attr);
      }

      if (!P.consumeIf(tok::r_square))
        return true;
    }

    if (isBeginAccess && !kind.isSet()) {
      P.diagnose(OpcodeLoc, diag::sil_expected_access_kind, OpcodeName);
      kind.Value = SILAccessKind::Read;
    }

    if (wantsEnforcement && !enforcement.isSet()) {
      P.diagnose(OpcodeLoc, diag::sil_expected_access_enforcement, OpcodeName);
      enforcement.Value = SILAccessEnforcement::Unsafe;
    }

    if (!isBeginAccess && !aborting.isSet())
      aborting.Value = false;

    if (isBeginAccess && !noNestedConflict.isSet())
      noNestedConflict.Value = false;

    if (!fromBuiltin.isSet())
      fromBuiltin.Value = false;

    SILValue addrVal;
    SourceLoc addrLoc;
    if (parseTypedValueRef(addrVal, addrLoc, B))
      return true;

    SILValue bufferVal;
    SourceLoc bufferLoc;
    if (Opcode == SILInstructionKind::BeginUnpairedAccessInst &&
        (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
         parseTypedValueRef(bufferVal, bufferLoc, B)))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    if (!addrVal->getType().isAddress()) {
      P.diagnose(addrLoc, diag::sil_operand_not_address, "operand", OpcodeName);
      return true;
    }

    if (Opcode == SILInstructionKind::BeginAccessInst) {
      ResultVal = B.createBeginAccess(InstLoc, addrVal, *kind, *enforcement,
                                      *noNestedConflict, *fromBuiltin);
    } else if (Opcode == SILInstructionKind::EndAccessInst) {
      ResultVal = B.createEndAccess(InstLoc, addrVal, *aborting);
    } else if (Opcode == SILInstructionKind::BeginUnpairedAccessInst) {
      ResultVal = B.createBeginUnpairedAccess(InstLoc, addrVal, bufferVal,
                                              *kind, *enforcement,
                                              *noNestedConflict, *fromBuiltin);
    } else {
      ResultVal = B.createEndUnpairedAccess(InstLoc, addrVal, *enforcement,
                                            *aborting, *fromBuiltin);
    }
    break;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Store##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StoreBorrowInst: {
    UnresolvedValueName from;
    bool isRefStorage = false;
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  isRefStorage |= Opcode == SILInstructionKind::Store##Name##Inst;
#include "swift/AST/ReferenceStorage.def"

    SourceLoc toLoc, addrLoc;
    Identifier toToken;
    SILValue addrVal;
    bool isInit = false;
    if (parseValueName(from) ||
        parseSILIdentifier(toToken, toLoc, diag::expected_tok_in_sil_instr,
                           "to") ||
        (isRefStorage && parseSILOptional(isInit, *this, "init")) ||
        parseTypedValueRef(addrVal, addrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (toToken.str() != "to") {
      P.diagnose(toLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    if (!addrVal->getType().isAddress()) {
      P.diagnose(addrLoc, diag::sil_operand_not_address, "destination",
                 OpcodeName);
      return true;
    }

    if (Opcode == SILInstructionKind::StoreBorrowInst) {
      SILType valueTy = addrVal->getType().getObjectType();
      ResultVal = B.createStoreBorrow(
          InstLoc, getLocalValue(from, valueTy, InstLoc, B), addrVal);
      break;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  if (Opcode == SILInstructionKind::Store##Name##Inst) {                       \
    auto refType = addrVal->getType().getAs<Name##StorageType>();              \
    if (!refType) {                                                            \
      P.diagnose(addrLoc, diag::sil_operand_not_ref_storage_address,           \
                 "destination", OpcodeName, ReferenceOwnership::Name);         \
      return true;                                                             \
    }                                                                          \
    auto valueTy = SILType::getPrimitiveObjectType(refType.getReferentType()); \
    ResultVal =                                                                \
        B.createStore##Name(InstLoc, getLocalValue(from, valueTy, InstLoc, B), \
                            addrVal, IsInitialization_t(isInit));              \
    break;                                                                     \
  }
#include "swift/AST/ReferenceStorage.def"

    break;
  }
  case SILInstructionKind::AllocPackInst: {
    SILType Ty;
    if (parseSILType(Ty))
      return true;

    ResultVal = B.createAllocPack(InstLoc, Ty);
    break;
  }
  case SILInstructionKind::AllocStackInst: {
    bool hasDynamicLifetime = false;
    bool isLexical = false;
    bool usesMoveableValueDebugInfo = false;

    StringRef attributeName;
    SourceLoc attributeLoc;
    while (parseSILOptional(attributeName, attributeLoc, *this)) {
      if (attributeName == "dynamic_lifetime")
        hasDynamicLifetime = true;
      else if (attributeName == "lexical")
        isLexical = true;
      else if (attributeName == "moveable_value_debuginfo")
        usesMoveableValueDebugInfo = true;
      else {
        P.diagnose(attributeLoc, diag::sil_invalid_attribute_for_instruction,
                   attributeName, "alloc_stack");
        return true;
      }
    }

    SILType Ty;
    if (parseSILType(Ty))
      return true;

    SILDebugVariable VarInfo;
    if (parseSILDebugVar(VarInfo) || parseSILDebugLocation(InstLoc, B))
      return true;

    if (Ty.isMoveOnly())
      usesMoveableValueDebugInfo = true;

    // It doesn't make sense to attach a debug var info if the name is empty
    if (VarInfo.Name.size())
      ResultVal = B.createAllocStack(InstLoc, Ty, VarInfo, hasDynamicLifetime,
                                     isLexical, usesMoveableValueDebugInfo);
    else
      ResultVal = B.createAllocStack(InstLoc, Ty, {}, hasDynamicLifetime,
                                     isLexical, usesMoveableValueDebugInfo);
    break;
  }
  case SILInstructionKind::MetatypeInst: {
    SILType Ty;
    if (parseSILType(Ty))
      return true;

    assert(Opcode == SILInstructionKind::MetatypeInst);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createMetatype(InstLoc, Ty);
    break;
  }
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst: {
    bool IsObjC = false;
    bool OnStack = false;
    SmallVector<SILType, 2> ElementTypes;
    SmallVector<SILValue, 2> ElementCounts;
    while (P.consumeIf(tok::l_square)) {
      Identifier Id;
      parseSILIdentifier(Id, diag::expected_in_attribute_list);
      StringRef Optional = Id.str();
      if (Optional == "objc") {
        IsObjC = true;
      } else if (Optional == "stack") {
        OnStack = true;
      } else if (Optional == "tail_elems") {
        SILType ElemTy;
        if (parseSILType(ElemTy) || !P.Tok.isAnyOperator() ||
            P.Tok.getText() != "*")
          return true;
        P.consumeToken();

        SILValue ElemCount;
        if (parseTypedValueRef(ElemCount, B))
          return true;

        ElementTypes.push_back(ElemTy);
        ElementCounts.push_back(ElemCount);
      } else {
        return true;
      }
      P.parseToken(tok::r_square, diag::expected_in_attribute_list);
    }
    SILValue Metadata;
    if (Opcode == SILInstructionKind::AllocRefDynamicInst) {
      if (parseTypedValueRef(Metadata, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
        return true;
    }

    SILType ObjectType;
    if (parseSILType(ObjectType))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    if (IsObjC && !ElementTypes.empty()) {
      P.diagnose(P.Tok, diag::sil_objc_with_tail_elements);
      return true;
    }
    if (Opcode == SILInstructionKind::AllocRefDynamicInst) {
      ResultVal = B.createAllocRefDynamic(InstLoc, Metadata, ObjectType, IsObjC,
                                          OnStack,
                                          ElementTypes, ElementCounts);
    } else {
      ResultVal = B.createAllocRef(InstLoc, ObjectType, IsObjC, OnStack,
                                   ElementTypes, ElementCounts);
    }
    break;
  }

  case SILInstructionKind::DeallocStackInst:
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocStack(InstLoc, Val);
    break;
  case SILInstructionKind::DeallocStackRefInst:
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocStackRef(InstLoc, Val);
    break;
  case SILInstructionKind::DeallocPackInst:
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocPack(InstLoc, Val);
    break;
  case SILInstructionKind::DeallocRefInst: {
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocRef(InstLoc, Val);
    break;
  }
  case SILInstructionKind::DeallocPartialRefInst: {
    SILValue Metatype, Instance;
    if (parseTypedValueRef(Instance, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(Metatype, B) || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createDeallocPartialRef(InstLoc, Instance, Metatype);
    break;
  }
    case SILInstructionKind::DeallocBoxInst:
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createDeallocBox(InstLoc, Val);
      break;
    case SILInstructionKind::ValueMetatypeInst:
    case SILInstructionKind::ExistentialMetatypeInst: {
      SILType Ty;
      if (parseSILType(Ty) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      switch (Opcode) {
      default:
        llvm_unreachable("Out of sync with parent switch");
      case SILInstructionKind::ValueMetatypeInst:
        ResultVal = B.createValueMetatype(InstLoc, Ty, Val);
        break;
      case SILInstructionKind::ExistentialMetatypeInst:
        ResultVal = B.createExistentialMetatype(InstLoc, Ty, Val);
        break;
      case SILInstructionKind::DeallocBoxInst:
        ResultVal = B.createDeallocBox(InstLoc, Val);
        break;
      }
      break;
    }
    case SILInstructionKind::DeallocExistentialBoxInst: {
      CanType ConcreteTy;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(ConcreteTy) || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createDeallocExistentialBox(InstLoc, ConcreteTy, Val);
      break;
    }
    case SILInstructionKind::TupleInst: {
      // Tuple instructions have two different syntaxes, one for simple tuple
      // types, one for complicated ones.
      if (P.Tok.isNot(tok::sil_dollar)) {
        // If there is no type, parse the simple form.
        if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
          return true;

        // TODO: Check for a type here.  This is how tuples with "interesting"
        // types are described.

        // This form is used with tuples that have elements with no names or
        // default values.
        SmallVector<TupleTypeElt, 4> TypeElts;
        if (P.Tok.isNot(tok::r_paren)) {
          do {
            if (parseTypedValueRef(Val, B))
              return true;
            OpList.push_back(Val);
            TypeElts.push_back(Val->getType().getRawASTType());
          } while (P.consumeIf(tok::comma));
        }
        HadError |=
            P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")");

        auto Ty = TupleType::get(TypeElts, P.Context);
        auto Ty2 = SILType::getPrimitiveObjectType(Ty->getCanonicalType());

        ValueOwnershipKind forwardingOwnership =
            F && F->hasOwnership() ? mergeSILValueOwnership(OpList)
                                   : ValueOwnershipKind(OwnershipKind::None);

        if (parseForwardingOwnershipKind(forwardingOwnership)
            || parseSILDebugLocation(InstLoc, B))
          return true;

        ResultVal = B.createTuple(InstLoc, Ty2, OpList, forwardingOwnership);
        break;
      }

      // Otherwise, parse the fully general form.
      SILType Ty;
      if (parseSILType(Ty) ||
          P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
        return true;

      TupleType *TT = Ty.getAs<TupleType>();
      if (TT == nullptr) {
        P.diagnose(OpcodeLoc, diag::expected_tuple_type_in_tuple);
        return true;
      }

      SmallVector<TupleTypeElt, 4> TypeElts;
      if (P.Tok.isNot(tok::r_paren)) {
        do {
          if (TypeElts.size() > TT->getNumElements()) {
            P.diagnose(P.Tok, diag::sil_tuple_inst_wrong_value_count,
                       TT->getNumElements());
            return true;
          }
          Type EltTy = TT->getElement(TypeElts.size()).getType();
          if (parseValueRef(
                  Val,
                  SILType::getPrimitiveObjectType(EltTy->getCanonicalType()),
                  RegularLocation(P.Tok.getLoc()), B))
            return true;
          OpList.push_back(Val);
          TypeElts.push_back(Val->getType().getRawASTType());
        } while (P.consumeIf(tok::comma));
      }
      HadError |= P.parseToken(tok::r_paren,
                               diag::expected_tok_in_sil_instr,")");

      if (TypeElts.size() != TT->getNumElements()) {
        P.diagnose(OpcodeLoc, diag::sil_tuple_inst_wrong_value_count,
                   TT->getNumElements());
        return true;
      }

      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createTuple(InstLoc, Ty, OpList);
      break;
    }
    case SILInstructionKind::EnumInst: {
      SILType Ty;
      SILDeclRef Elt;
      SILValue Operand;
      if (parseSILType(Ty) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDeclRef(Elt))
        return true;

      if (P.Tok.is(tok::comma) && !peekSILDebugLocation(P)) {
        P.consumeToken(tok::comma);
        if (parseTypedValueRef(Operand, B))
          return true;
      }

      if (parseSILDebugLocation(InstLoc, B))
        return true;

      ValueOwnershipKind forwardingOwnership =
          Operand ? Operand->getOwnershipKind()
                  : ValueOwnershipKind(OwnershipKind::None);

      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal =
          B.createEnum(InstLoc, Operand, cast<EnumElementDecl>(Elt.getDecl()),
                       Ty, forwardingOwnership);
      break;
    }
    case SILInstructionKind::InitEnumDataAddrInst:
    case SILInstructionKind::UncheckedEnumDataInst:
    case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
      SILValue Operand;
      SILDeclRef EltRef;
      if (parseTypedValueRef(Operand, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDeclRef(EltRef))
        return true;

      ValueOwnershipKind forwardingOwnership = Operand->getOwnershipKind();
      if (Opcode == SILInstructionKind::UncheckedEnumDataInst)
        parseForwardingOwnershipKind(forwardingOwnership);

      if (parseSILDebugLocation(InstLoc, B))
        return true;
      EnumElementDecl *Elt = cast<EnumElementDecl>(EltRef.getDecl());
      auto ResultTy = Operand->getType().getEnumElementType(
          Elt, SILMod, B.getTypeExpansionContext());

      switch (Opcode) {
      case swift::SILInstructionKind::InitEnumDataAddrInst:
        ResultVal = B.createInitEnumDataAddr(InstLoc, Operand, Elt, ResultTy);
        break;
      case swift::SILInstructionKind::UncheckedTakeEnumDataAddrInst:
        ResultVal =
            B.createUncheckedTakeEnumDataAddr(InstLoc, Operand, Elt, ResultTy);
        break;
      case swift::SILInstructionKind::UncheckedEnumDataInst: {
        ResultVal = B.createUncheckedEnumData(InstLoc, Operand, Elt, ResultTy,
                                              forwardingOwnership);
        break;
      }
      default:
        llvm_unreachable("switch out of sync");
      }
      break;
    }
    case SILInstructionKind::InjectEnumAddrInst: {
      SILValue Operand;
      SILDeclRef EltRef;
      if (parseTypedValueRef(Operand, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDeclRef(EltRef) || parseSILDebugLocation(InstLoc, B))
        return true;

      EnumElementDecl *Elt = cast<EnumElementDecl>(EltRef.getDecl());
      ResultVal = B.createInjectEnumAddr(InstLoc, Operand, Elt);
      break;
    }
    case SILInstructionKind::TupleElementAddrInst:
    case SILInstructionKind::TupleExtractInst: {
      SourceLoc NameLoc;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
        return true;

      unsigned Field = 0;
      TupleType *TT = Val->getType().castTo<TupleType>();
      if (P.Tok.isNot(tok::integer_literal) ||
          parseIntegerLiteral(P.Tok.getText(), 10, Field) ||
          Field >= TT->getNumElements()) {
        P.diagnose(P.Tok, diag::sil_tuple_inst_wrong_field);
        return true;
      }
      P.consumeToken(tok::integer_literal);
      ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();

      if (Opcode == SILInstructionKind::TupleExtractInst) {
        if (parseForwardingOwnershipKind(forwardingOwnership))
          return true;
      }

      if (parseSILDebugLocation(InstLoc, B))
        return true;
      auto ResultTy = TT->getElement(Field).getType()->getCanonicalType();
      if (Opcode == SILInstructionKind::TupleElementAddrInst)
        ResultVal = B.createTupleElementAddr(
            InstLoc, Val, Field, SILType::getPrimitiveAddressType(ResultTy));
      else {
        ResultVal = B.createTupleExtract(
            InstLoc, Val, Field, SILType::getPrimitiveObjectType(ResultTy),
            forwardingOwnership);
      }
      break;
    }
    case SILInstructionKind::ReturnInst: {
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createReturn(InstLoc, Val);
      break;
    }
    case SILInstructionKind::ThrowInst: {
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createThrow(InstLoc, Val);
      break;
    }
    case SILInstructionKind::UnwindInst: {
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createUnwind(InstLoc);
      break;
    }
    case SILInstructionKind::YieldInst: {
      SmallVector<SILValue, 6> values;

      // Parse a parenthesized (unless length-1), comma-separated list
      // of yielded values.
      if (P.consumeIf(tok::l_paren)) {
        if (!P.Tok.is(tok::r_paren)) {
          do {
            if (parseTypedValueRef(Val, B))
              return true;
            values.push_back(Val);
          } while (P.consumeIf(tok::comma));
        }

        if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
          return true;

      } else {
        if (parseTypedValueRef(Val, B))
          return true;
        values.push_back(Val);
      }

      Identifier resumeName, unwindName;
      SourceLoc resumeLoc, unwindLoc;
      if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseVerbatim("resume") ||
          parseSILIdentifier(resumeName, resumeLoc,
                             diag::expected_sil_block_name) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseVerbatim("unwind") ||
          parseSILIdentifier(unwindName, unwindLoc,
                             diag::expected_sil_block_name) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      auto resumeBB = getBBForReference(resumeName, resumeLoc);
      auto unwindBB = getBBForReference(unwindName, unwindLoc);
      ResultVal = B.createYield(InstLoc, values, resumeBB, unwindBB);
      break;
    }
    case SILInstructionKind::BranchInst: {
      Identifier BBName;
      SourceLoc NameLoc;
      if (parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name))
        return true;

      SmallVector<SILValue, 6> Args;
      if (parseSILBBArgsAtBranch(Args, B))
        return true;

      if (parseSILDebugLocation(InstLoc, B))
        return true;

      // Note, the basic block here could be a reference to an undefined
      // basic block, which will be parsed later on.
      ResultVal =
          B.createBranch(InstLoc, getBBForReference(BBName, NameLoc), Args);
      break;
    }
    case SILInstructionKind::CondBranchInst: {
      UnresolvedValueName Cond;
      Identifier BBName, BBName2;
      SourceLoc NameLoc, NameLoc2;
      SmallVector<SILValue, 6> Args, Args2;
      if (parseValueName(Cond) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name) ||
          parseSILBBArgsAtBranch(Args, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(BBName2, NameLoc2,
                             diag::expected_sil_block_name) ||
          parseSILBBArgsAtBranch(Args2, B) || parseSILDebugLocation(InstLoc, B))
        return true;

      auto I1Ty = SILType::getBuiltinIntegerType(1, SILMod.getASTContext());
      SILValue CondVal = getLocalValue(Cond, I1Ty, InstLoc, B);
      ResultVal = B.createCondBranch(
          InstLoc, CondVal, getBBForReference(BBName, NameLoc), Args,
          getBBForReference(BBName2, NameLoc2), Args2);
      break;
    }
    case SILInstructionKind::UnreachableInst:
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createUnreachable(InstLoc);
      break;

    case SILInstructionKind::ClassMethodInst:
    case SILInstructionKind::SuperMethodInst:
    case SILInstructionKind::ObjCMethodInst:
    case SILInstructionKind::ObjCSuperMethodInst: {
      SILDeclRef Member;
      SILType MethodTy;
      SourceLoc TyLoc;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
        return true;

      if (parseSILDeclRef(Member, true))
        return true;

      if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(MethodTy, TyLoc) || parseSILDebugLocation(InstLoc, B))
        return true;

      switch (Opcode) {
      default:
        llvm_unreachable("Out of sync with parent switch");
      case SILInstructionKind::ClassMethodInst:
        ResultVal = B.createClassMethod(InstLoc, Val, Member, MethodTy);
        break;
      case SILInstructionKind::SuperMethodInst:
        ResultVal = B.createSuperMethod(InstLoc, Val, Member, MethodTy);
        break;
      case SILInstructionKind::ObjCMethodInst:
        ResultVal = B.createObjCMethod(InstLoc, Val, Member, MethodTy);
        break;
      case SILInstructionKind::ObjCSuperMethodInst:
        ResultVal = B.createObjCSuperMethod(InstLoc, Val, Member, MethodTy);
        break;
      }
      break;
    }
    case SILInstructionKind::WitnessMethodInst: {
      CanType LookupTy;
      SILDeclRef Member;
      SILType MethodTy;
      SourceLoc TyLoc;
      if (P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(LookupTy) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
        return true;
      if (parseSILDeclRef(Member, true))
        return true;
      // Optional operand.
      SILValue Operand;
      if (P.Tok.is(tok::comma)) {
        P.consumeToken(tok::comma);
        if (parseTypedValueRef(Operand, B))
          return true;
      }
      if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(MethodTy, TyLoc) || parseSILDebugLocation(InstLoc, B))
        return true;

      // If LookupTy is a non-archetype, look up its conformance.
      ProtocolDecl *proto =
          dyn_cast<ProtocolDecl>(Member.getDecl()->getDeclContext());
      if (!proto) {
        P.diagnose(TyLoc, diag::sil_witness_method_not_protocol);
        return true;
      }
      auto conformance =
          P.SF.getParentModule()->lookupConformance(LookupTy, proto);
      if (conformance.isInvalid()) {
        P.diagnose(TyLoc, diag::sil_witness_method_type_does_not_conform);
        return true;
      }

      ResultVal = B.createWitnessMethod(InstLoc, LookupTy, conformance, Member,
                                        MethodTy);
      break;
    }
    case SILInstructionKind::CopyAddrInst: {
      bool IsTake = false, IsInit = false;
      UnresolvedValueName SrcLName;
      SILValue DestLVal;
      SourceLoc ToLoc, DestLoc;
      Identifier ToToken;
      if (parseSILOptional(IsTake, *this, "take") || parseValueName(SrcLName) ||
          parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                             "to") ||
          parseSILOptional(IsInit, *this, "init") ||
          parseTypedValueRef(DestLVal, DestLoc, B) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      if (ToToken.str() != "to") {
        P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
        return true;
      }

      if (!DestLVal->getType().isAddress()) {
        P.diagnose(DestLoc, diag::sil_invalid_instr_operands);
        return true;
      }

      SILValue SrcLVal =
          getLocalValue(SrcLName, DestLVal->getType(), InstLoc, B);
      ResultVal = B.createCopyAddr(InstLoc, SrcLVal, DestLVal, IsTake_t(IsTake),
                                   IsInitialization_t(IsInit));
      break;
    }
    case SILInstructionKind::ExplicitCopyAddrInst: {
      bool IsTake = false, IsInit = false;
      UnresolvedValueName SrcLName;
      SILValue DestLVal;
      SourceLoc ToLoc, DestLoc;
      Identifier ToToken;
      if (parseSILOptional(IsTake, *this, "take") || parseValueName(SrcLName) ||
          parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                             "to") ||
          parseSILOptional(IsInit, *this, "init") ||
          parseTypedValueRef(DestLVal, DestLoc, B) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      if (ToToken.str() != "to") {
        P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
        return true;
      }

      if (!DestLVal->getType().isAddress()) {
        P.diagnose(DestLoc, diag::sil_invalid_instr_operands);
        return true;
      }

      SILValue SrcLVal =
          getLocalValue(SrcLName, DestLVal->getType(), InstLoc, B);
      ResultVal =
          B.createExplicitCopyAddr(InstLoc, SrcLVal, DestLVal, IsTake_t(IsTake),
                                   IsInitialization_t(IsInit));
      break;
    }
    case SILInstructionKind::MarkUnresolvedMoveAddrInst: {
      UnresolvedValueName SrcLName;
      SILValue DestLVal;
      SourceLoc ToLoc, DestLoc;
      Identifier ToToken;
      if (parseValueName(SrcLName) ||
          parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                             "to") ||
          parseTypedValueRef(DestLVal, DestLoc, B) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      if (ToToken.str() != "to") {
        P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
        return true;
      }

      if (!DestLVal->getType().isAddress()) {
        P.diagnose(DestLoc, diag::sil_invalid_instr_operands);
        return true;
      }

      SILValue SrcLVal =
          getLocalValue(SrcLName, DestLVal->getType(), InstLoc, B);
      ResultVal = B.createMarkUnresolvedMoveAddr(InstLoc, SrcLVal, DestLVal);
      break;
    }

    case SILInstructionKind::BindMemoryInst: {
      SILValue IndexVal;
      SILType EltTy;
      if (parseTypedValueRef(Val, B)
          || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
          || parseTypedValueRef(IndexVal, B)
          || parseVerbatim("to")
          || parseSILType(EltTy)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createBindMemory(InstLoc, Val, IndexVal, EltTy);
      break;
    }
    case SILInstructionKind::RebindMemoryInst: {
      SILValue InToken;
      if (parseTypedValueRef(Val, B)
          || parseVerbatim("to")
          || parseTypedValueRef(InToken, B)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createRebindMemory(InstLoc, Val, InToken);
      break;
    }
    case SILInstructionKind::ObjectInst:
    case SILInstructionKind::StructInst: {
      SILType Ty;
      if (parseSILType(Ty) ||
          P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
        return true;

      // Parse a list of SILValue.
      bool OpsAreTailElems = false;
      unsigned NumBaseElems = 0;
      if (P.Tok.isNot(tok::r_paren)) {
        do {
          if (Opcode == SILInstructionKind::ObjectInst) {
            if (parseSILOptional(OpsAreTailElems, *this, "tail_elems"))
              return true;
          }
          if (parseTypedValueRef(Val, B))
            return true;
          OpList.push_back(Val);
          if (!OpsAreTailElems)
            NumBaseElems = OpList.size();
        } while (P.consumeIf(tok::comma));
      }
      if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
        return true;

      ValueOwnershipKind forwardingOwnership =
          F && F->hasOwnership() ? mergeSILValueOwnership(OpList)
                                 : ValueOwnershipKind(OwnershipKind::None);
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B)) {
        return true;
      }
      if (Opcode == SILInstructionKind::StructInst) {
        ResultVal = B.createStruct(InstLoc, Ty, OpList, forwardingOwnership);
      } else {
        ResultVal = B.createObject(InstLoc, Ty, OpList, NumBaseElems,
                                   forwardingOwnership);
      }
      break;
    }
    case SILInstructionKind::StructElementAddrInst:
    case SILInstructionKind::StructExtractInst: {
      ValueDecl *FieldV;
      SourceLoc NameLoc = P.Tok.getLoc();
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDottedPath(FieldV))
        return true;

      ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
      if (Opcode == SILInstructionKind::StructExtractInst) {
        if (parseForwardingOwnershipKind(forwardingOwnership))
          return true;
      }

      if (parseSILDebugLocation(InstLoc, B))
        return true;
      if (!FieldV || !isa<VarDecl>(FieldV)) {
        P.diagnose(NameLoc, diag::sil_struct_inst_wrong_field);
        return true;
      }
      VarDecl *Field = cast<VarDecl>(FieldV);

      // FIXME: substitution means this type should be explicit to improve
      // performance.
      auto ResultTy = Val->getType().getFieldType(Field, SILMod,
                                                  B.getTypeExpansionContext());
      if (Opcode == SILInstructionKind::StructElementAddrInst)
        ResultVal = B.createStructElementAddr(InstLoc, Val, Field,
                                              ResultTy.getAddressType());
      else {
        ResultVal = B.createStructExtract(
            InstLoc, Val, Field, ResultTy.getObjectType(), forwardingOwnership);
      }
      break;
    }
    case SILInstructionKind::RefElementAddrInst: {
      ValueDecl *FieldV;
      SourceLoc NameLoc;
      bool IsImmutable = false;
      if (parseSILOptional(IsImmutable, *this, "immutable") ||
          parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDottedPath(FieldV) || parseSILDebugLocation(InstLoc, B))
        return true;
      if (!FieldV || !isa<VarDecl>(FieldV)) {
        P.diagnose(NameLoc, diag::sil_ref_inst_wrong_field);
        return true;
      }
      VarDecl *Field = cast<VarDecl>(FieldV);
      auto ResultTy = Val->getType().getFieldType(Field, SILMod,
                                                  B.getTypeExpansionContext());
      ResultVal = B.createRefElementAddr(InstLoc, Val, Field, ResultTy,
                                         IsImmutable);
      break;
    }
    case SILInstructionKind::RefTailAddrInst: {
      SourceLoc NameLoc;
      SILType ResultObjTy;
      bool IsImmutable = false;
      if (parseSILOptional(IsImmutable, *this, "immutable") ||
          parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(ResultObjTy) || parseSILDebugLocation(InstLoc, B))
        return true;
      SILType ResultTy = ResultObjTy.getAddressType();
      ResultVal = B.createRefTailAddr(InstLoc, Val, ResultTy, IsImmutable);
      break;
    }
    case SILInstructionKind::IndexAddrInst: {
      SILValue IndexVal;
      bool needsStackProtection = false;
      if (parseSILOptional(needsStackProtection, *this, "stack_protection") ||
          parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseTypedValueRef(IndexVal, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createIndexAddr(InstLoc, Val, IndexVal, needsStackProtection);
      break;
    }
    case SILInstructionKind::TailAddrInst: {
      SILValue IndexVal;
      SILType ResultObjTy;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseTypedValueRef(IndexVal, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(ResultObjTy) || parseSILDebugLocation(InstLoc, B))
        return true;
      SILType ResultTy = ResultObjTy.getAddressType();
      ResultVal = B.createTailAddr(InstLoc, Val, IndexVal, ResultTy);
      break;
    }
    case SILInstructionKind::IndexRawPointerInst: {
      SILValue IndexVal;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseTypedValueRef(IndexVal, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createIndexRawPointer(InstLoc, Val, IndexVal);
      break;
    }
    case SILInstructionKind::ObjCProtocolInst: {
      Identifier ProtocolName;
      SILType Ty;
      if (P.parseToken(tok::pound, diag::expected_sil_constant) ||
          parseSILIdentifier(ProtocolName, diag::expected_sil_constant) ||
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(Ty) || parseSILDebugLocation(InstLoc, B))
        return true;
      // Find the decl for the protocol name.
      ValueDecl *VD;
      SmallVector<ValueDecl *, 4> CurModuleResults;
      // Perform a module level lookup on the first component of the
      // fully-qualified name.
      P.SF.getParentModule()->lookupValue(
          ProtocolName, NLKind::UnqualifiedLookup, CurModuleResults);
      assert(CurModuleResults.size() == 1);
      VD = CurModuleResults[0];
      ResultVal = B.createObjCProtocol(InstLoc, cast<ProtocolDecl>(VD), Ty);
      break;
    }
    case SILInstructionKind::AllocGlobalInst: {
      Identifier GlobalName;
      SourceLoc IdLoc;
      if (P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
          parseSILIdentifier(GlobalName, IdLoc,
                             diag::expected_sil_value_name) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      // Go through list of global variables in the SILModule.
      SILGlobalVariable *global = SILMod.lookUpGlobalVariable(GlobalName.str());
      if (!global) {
        P.diagnose(IdLoc, diag::sil_global_variable_not_found, GlobalName);
        return true;
      }

      ResultVal = B.createAllocGlobal(InstLoc, global);
      break;
    }
    case SILInstructionKind::GlobalAddrInst:
    case SILInstructionKind::GlobalValueInst: {
      Identifier GlobalName;
      SourceLoc IdLoc;
      SILType Ty;
      if (P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
          parseSILIdentifier(GlobalName, IdLoc,
                             diag::expected_sil_value_name) ||
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(Ty) || parseSILDebugLocation(InstLoc, B))
        return true;

      // Go through list of global variables in the SILModule.
      SILGlobalVariable *global = SILMod.lookUpGlobalVariable(GlobalName.str());
      if (!global) {
        P.diagnose(IdLoc, diag::sil_global_variable_not_found, GlobalName);
        return true;
      }

      SILType expectedType = (Opcode == SILInstructionKind::GlobalAddrInst
                                  ? global->getLoweredType().getAddressType()
                                  : global->getLoweredType());
      if (expectedType != Ty) {
        P.diagnose(IdLoc, diag::sil_value_use_type_mismatch, GlobalName.str(),
                   global->getLoweredType().getRawASTType(),
                   Ty.getRawASTType());
        return true;
      }

      if (Opcode == SILInstructionKind::GlobalAddrInst) {
        ResultVal = B.createGlobalAddr(InstLoc, global);
      } else {
        ResultVal = B.createGlobalValue(InstLoc, global);
      }
      break;
    }
    case SILInstructionKind::BaseAddrForOffsetInst: {
      SILType Ty;
      if (parseSILType(Ty))
        return true;
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createBaseAddrForOffset(InstLoc, Ty);
      break;
    }
    case SILInstructionKind::SelectEnumInst:
    case SILInstructionKind::SelectEnumAddrInst: {
      if (parseTypedValueRef(Val, B))
        return true;

      SmallVector<std::pair<EnumElementDecl *, UnresolvedValueName>, 4>
          CaseValueNames;
      Optional<UnresolvedValueName> DefaultValueName;
      while (P.consumeIf(tok::comma)) {
        Identifier BBName;
        SourceLoc BBLoc;
        // Parse 'default' sil-value.
        UnresolvedValueName tmp;
        if (P.consumeIf(tok::kw_default)) {
          if (parseValueName(tmp))
            return true;
          DefaultValueName = tmp;
          break;
        }

        // Parse 'case' sil-decl-ref ':' sil-value.
        if (P.consumeIf(tok::kw_case)) {
          SILDeclRef ElemRef;
          if (parseSILDeclRef(ElemRef))
            return true;
          assert(ElemRef.hasDecl() && isa<EnumElementDecl>(ElemRef.getDecl()));
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
          parseValueName(tmp);
          CaseValueNames.push_back(
              std::make_pair(cast<EnumElementDecl>(ElemRef.getDecl()), tmp));
          continue;
        }

        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
        return true;
      }

      // Parse the type of the result operands.
      SILType ResultType;
      if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(ResultType))
        return true;

      ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
      if (Opcode == SILInstructionKind::SelectEnumInst) {
        if (parseForwardingOwnershipKind(forwardingOwnership))
          return true;
      }
      if (parseSILDebugLocation(InstLoc, B))
        return true;

      // Resolve the results.
      SmallVector<std::pair<EnumElementDecl *, SILValue>, 4> CaseValues;
      SILValue DefaultValue;
      if (DefaultValueName)
        DefaultValue = getLocalValue(*DefaultValueName, ResultType, InstLoc, B);
      for (auto &caseName : CaseValueNames)
        CaseValues.push_back(std::make_pair(
            caseName.first,
            getLocalValue(caseName.second, ResultType, InstLoc, B)));

      if (Opcode == SILInstructionKind::SelectEnumInst) {
        ResultVal = B.createSelectEnum(InstLoc, Val, ResultType, DefaultValue,
                                       CaseValues, None, ProfileCounter(),
                                       forwardingOwnership);
      } else
        ResultVal = B.createSelectEnumAddr(InstLoc, Val, ResultType,
                                           DefaultValue, CaseValues);
      break;
    }

    case SILInstructionKind::SwitchEnumInst:
    case SILInstructionKind::SwitchEnumAddrInst: {
      if (parseTypedValueRef(Val, B))
        return true;

      SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 4> CaseBBs;
      SILBasicBlock *DefaultBB = nullptr;
      ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
      while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma)) {
        parsedComma = true;

        Identifier BBName;
        SourceLoc BBLoc;
        // Parse 'case' sil-decl-ref ':' sil-identifier.
        if (P.consumeIf(tok::kw_case)) {
          parsedComma = false;
          if (DefaultBB) {
            P.diagnose(P.Tok, diag::case_after_default);
            return true;
          }
          SILDeclRef ElemRef;
          if (parseSILDeclRef(ElemRef))
            return true;
          assert(ElemRef.hasDecl() && isa<EnumElementDecl>(ElemRef.getDecl()));
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
          parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
          CaseBBs.push_back({cast<EnumElementDecl>(ElemRef.getDecl()),
                             getBBForReference(BBName, BBLoc)});
          continue;
        }

        // Parse 'default' sil-identifier.
        if (P.consumeIf(tok::kw_default)) {
          parsedComma = false;
          parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
          DefaultBB = getBBForReference(BBName, BBLoc);
          continue;
        }
        break;
      }
      if (Opcode == SILInstructionKind::SwitchEnumInst
          && parseForwardingOwnershipKind(forwardingOwnership)) {
        return true;
      }
      if (parseSILDebugLocation(InstLoc, B))
        return true;

      if (parsedComma || (CaseBBs.empty() && !DefaultBB)) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
        return true;
      }

      if (Opcode == SILInstructionKind::SwitchEnumInst) {
        ResultVal = B.createSwitchEnum(InstLoc, Val, DefaultBB, CaseBBs, None,
                                       ProfileCounter(), forwardingOwnership);
      } else
        ResultVal = B.createSwitchEnumAddr(InstLoc, Val, DefaultBB, CaseBBs);
      break;
    }
    case SILInstructionKind::SwitchValueInst: {
      if (parseTypedValueRef(Val, B))
        return true;

      SmallVector<std::pair<SILValue, SILBasicBlock *>, 4> CaseBBs;
      SILBasicBlock *DefaultBB = nullptr;
      while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma)) {
        Identifier BBName;
        SourceLoc BBLoc;
        SILValue CaseVal;

        // Parse 'default' sil-identifier.
        if (P.consumeIf(tok::kw_default)) {
          parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
          DefaultBB = getBBForReference(BBName, BBLoc);
          break;
        }

        // Parse 'case' value-ref ':' sil-identifier.
        if (P.consumeIf(tok::kw_case)) {
          if (parseValueRef(CaseVal, Val->getType(),
                            RegularLocation(P.Tok.getLoc()), B)) {
            // TODO: Issue a proper error message here
            P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,
                       "reference to a value");
            return true;
          }

          auto intTy = Val->getType().getAs<BuiltinIntegerType>();
          auto functionTy = Val->getType().getAs<SILFunctionType>();
          if (!intTy && !functionTy) {
            P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
            return true;
          }

          if (intTy) {
            // If it is a switch on an integer type, check that all case values
            // are integer literals or undef.
            if (!isa<SILUndef>(CaseVal)) {
              auto *IL = dyn_cast<IntegerLiteralInst>(CaseVal);
              if (!IL) {
                P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
                return true;
              }
              APInt CaseValue = IL->getValue();

              if (CaseValue.getBitWidth() != intTy->getGreatestWidth())
                CaseVal = B.createIntegerLiteral(
                    IL->getLoc(), Val->getType(),
                    CaseValue.zextOrTrunc(intTy->getGreatestWidth()));
            }
          }

          if (functionTy) {
            // If it is a switch on a function type, check that all case values
            // are function references or undef.
            if (!isa<SILUndef>(CaseVal)) {
              auto *FR = dyn_cast<FunctionRefInst>(CaseVal);
              if (!FR) {
                if (auto *CF = dyn_cast<ConvertFunctionInst>(CaseVal)) {
                  FR = dyn_cast<FunctionRefInst>(CF->getOperand());
                }
              }
              if (!FR) {
                P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
                return true;
              }
            }
          }

          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
          parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
          CaseBBs.push_back({CaseVal, getBBForReference(BBName, BBLoc)});
          continue;
        }

        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
        return true;
      }
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createSwitchValue(InstLoc, Val, DefaultBB, CaseBBs);
      break;
    }
    case SILInstructionKind::SelectValueInst: {
      if (parseTypedValueRef(Val, B))
        return true;

      SmallVector<std::pair<UnresolvedValueName, UnresolvedValueName>, 4>
          CaseValueAndResultNames;
      Optional<UnresolvedValueName> DefaultResultName;
      while (P.consumeIf(tok::comma)) {
        Identifier BBName;
        SourceLoc BBLoc;
        // Parse 'default' sil-value.
        UnresolvedValueName tmp;
        if (P.consumeIf(tok::kw_default)) {
          if (parseValueName(tmp))
            return true;
          DefaultResultName = tmp;
          break;
        }

        // Parse 'case' sil-decl-ref ':' sil-value.
        if (P.consumeIf(tok::kw_case)) {
          UnresolvedValueName casevalue;
          parseValueName(casevalue);
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
          parseValueName(tmp);
          CaseValueAndResultNames.push_back(std::make_pair(casevalue, tmp));
          continue;
        }

        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
        return true;
      }

      if (!DefaultResultName) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "default");
        return true;
      }

      // Parse the type of the result operands.
      SILType ResultType;
      if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(ResultType) || parseSILDebugLocation(InstLoc, B))
        return true;

      // Resolve the results.
      SmallVector<std::pair<SILValue, SILValue>, 4> CaseValues;
      SILValue DefaultValue;
      if (DefaultResultName)
        DefaultValue =
            getLocalValue(*DefaultResultName, ResultType, InstLoc, B);
      SILType ValType = Val->getType();
      for (auto &caseName : CaseValueAndResultNames)
        CaseValues.push_back(std::make_pair(
            getLocalValue(caseName.first, ValType, InstLoc, B),
            getLocalValue(caseName.second, ResultType, InstLoc, B)));

      ResultVal = B.createSelectValue(InstLoc, Val, ResultType, DefaultValue,
                                      CaseValues);
      break;
    }
    case SILInstructionKind::DeinitExistentialAddrInst: {
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createDeinitExistentialAddr(InstLoc, Val);
      break;
    }
    case SILInstructionKind::DeinitExistentialValueInst: {
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createDeinitExistentialValue(InstLoc, Val);
      break;
    }
    case SILInstructionKind::InitExistentialAddrInst: {
      CanType Ty;
      SourceLoc TyLoc;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(Ty, TyLoc) || parseSILDebugLocation(InstLoc, B))
        return true;

      // Lower the type at the abstraction level of the existential.
      auto archetype =
          OpenedArchetypeType::get(Val->getType().getASTType(),
                                   B.getFunction().getGenericSignature())
              ->getCanonicalType();

      auto &F = B.getFunction();
      SILType LoweredTy =
          F.getLoweredType(Lowering::AbstractionPattern(archetype), Ty)
              .getAddressType();

      // Collect conformances for the type.
      ArrayRef<ProtocolConformanceRef> conformances =
          collectExistentialConformances(P, Ty, TyLoc,
                                         Val->getType().getASTType());

      ResultVal = B.createInitExistentialAddr(InstLoc, Val, Ty, LoweredTy,
                                              conformances);
      break;
    }
    case SILInstructionKind::InitExistentialValueInst: {
      CanType FormalConcreteTy;
      SILType ExistentialTy;
      SourceLoc TyLoc;

      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(FormalConcreteTy, TyLoc) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(ExistentialTy) || parseSILDebugLocation(InstLoc, B))
        return true;

      ArrayRef<ProtocolConformanceRef> conformances =
          collectExistentialConformances(P, FormalConcreteTy, TyLoc,
                                         ExistentialTy.getASTType());

      ResultVal = B.createInitExistentialValue(
          InstLoc, ExistentialTy, FormalConcreteTy, Val, conformances);
      break;
    }
    case SILInstructionKind::AllocExistentialBoxInst: {
      SILType ExistentialTy;
      CanType ConcreteFormalTy;
      SourceLoc TyLoc;

      if (parseSILType(ExistentialTy) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(ConcreteFormalTy, TyLoc) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      // Collect conformances for the type.
      ArrayRef<ProtocolConformanceRef> conformances =
          collectExistentialConformances(P, ConcreteFormalTy, TyLoc,
                                         ExistentialTy.getASTType());

      ResultVal = B.createAllocExistentialBox(InstLoc, ExistentialTy,
                                              ConcreteFormalTy, conformances);

      break;
    }
    case SILInstructionKind::InitExistentialRefInst: {
      CanType FormalConcreteTy;
      SILType ExistentialTy;
      SourceLoc TyLoc;

      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
          parseASTType(FormalConcreteTy, TyLoc) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(ExistentialTy))
        return true;

      ValueOwnershipKind forwardingOwnership = Val->getOwnershipKind();
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      ArrayRef<ProtocolConformanceRef> conformances =
          collectExistentialConformances(P, FormalConcreteTy, TyLoc,
                                         ExistentialTy.getASTType());

      // FIXME: Conformances in InitExistentialRefInst is currently not included
      // in SIL.rst.
      ResultVal =
          B.createInitExistentialRef(InstLoc, ExistentialTy, FormalConcreteTy,
                                     Val, conformances, forwardingOwnership);
      break;
    }
    case SILInstructionKind::InitExistentialMetatypeInst: {
      SourceLoc TyLoc;
      SILType ExistentialTy;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILType(ExistentialTy, TyLoc) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      auto baseExType = ExistentialTy.getASTType();
      auto formalConcreteType = Val->getType().getASTType();
      while (auto instExType = dyn_cast<ExistentialMetatypeType>(baseExType)) {
        baseExType = instExType.getInstanceType();
        formalConcreteType =
            cast<MetatypeType>(formalConcreteType).getInstanceType();
      }

      ArrayRef<ProtocolConformanceRef> conformances =
          collectExistentialConformances(P, formalConcreteType, TyLoc,
                                         baseExType);

      ResultVal = B.createInitExistentialMetatype(InstLoc, Val, ExistentialTy,
                                                  conformances);
      break;
    }
    case SILInstructionKind::DynamicMethodBranchInst: {
      SILDeclRef Member;
      Identifier BBName, BBName2;
      SourceLoc NameLoc, NameLoc2;
      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILDeclRef(Member) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(BBName2, NameLoc2,
                             diag::expected_sil_block_name) ||
          parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createDynamicMethodBranch(
          InstLoc, Val, Member, getBBForReference(BBName, NameLoc),
          getBBForReference(BBName2, NameLoc2));
      break;
    }
    case SILInstructionKind::ProjectBlockStorageInst: {
      if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createProjectBlockStorage(InstLoc, Val);
      break;
    }
    case SILInstructionKind::InitBlockStorageHeaderInst: {
      Identifier invoke, type;
      SourceLoc invokeLoc, typeLoc;

      UnresolvedValueName invokeName;
      SILType invokeTy;
      GenericSignature invokeGenericSig;
      GenericParamList *invokeGenericParams = nullptr;

      SILType blockType;
      SmallVector<ParsedSubstitution, 4> parsedSubs;

      if (parseTypedValueRef(Val, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(invoke, invokeLoc, diag::expected_tok_in_sil_instr,
                             "invoke") ||
          parseValueName(invokeName) || parseSubstitutions(parsedSubs) ||
          P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
          parseSILType(invokeTy, invokeGenericSig, invokeGenericParams) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
          parseSILIdentifier(type, typeLoc, diag::expected_tok_in_sil_instr,
                             "type") ||
          parseSILType(blockType) || parseSILDebugLocation(InstLoc, B))
        return true;

      if (invoke.str() != "invoke") {
        P.diagnose(invokeLoc, diag::expected_tok_in_sil_instr, "invoke");
        return true;
      }
      if (type.str() != "type") {
        P.diagnose(invokeLoc, diag::expected_tok_in_sil_instr, "type");
        return true;
      }

      auto invokeVal = getLocalValue(invokeName, invokeTy, InstLoc, B);

      SubstitutionMap subMap;
      if (!parsedSubs.empty()) {
        if (!invokeGenericSig) {
          P.diagnose(typeLoc, diag::sil_substitutions_on_non_polymorphic_type);
          return true;
        }

        subMap = getApplySubstitutionsFromParsed(*this, invokeGenericSig,
                                                 parsedSubs);
        if (!subMap)
          return true;
      }

      ResultVal = B.createInitBlockStorageHeader(InstLoc, Val, invokeVal,
                                                 blockType, subMap);
      break;
    }
    case SILInstructionKind::DifferentiableFunctionInst: {
      // e.g. differentiable_function [parameters 0 1 2] [results 0] %0 : $T
      //
      // e.g. differentiable_function [parameters 0 1 2] [results 0] %0 : $T
      //          with_derivative {%1 : $T, %2 : $T}
      //                           ^~ jvp   ^~ vjp
      // Parse `[parameters <integer_literal>...]`.
      SmallVector<unsigned, 8> rawParameterIndices;
      if (parseIndexList(P, "parameters", rawParameterIndices,
                         diag::sil_autodiff_expected_parameter_index))
        return true;
      SmallVector<unsigned, 2> rawResultIndices;
      if (parseIndexList(P, "results", rawResultIndices,
                         diag::sil_autodiff_expected_result_index))
        return true;
      // Parse the original function value.
      SILValue original;
      SourceLoc originalOperandLoc;
      if (parseTypedValueRef(original, originalOperandLoc, B))
        return true;
      auto fnType = original->getType().getAs<SILFunctionType>();
      if (!fnType) {
        P.diagnose(originalOperandLoc,
                   diag::sil_inst_autodiff_expected_function_type_operand);
        return true;
      }
      Optional<std::pair<SILValue, SILValue>> derivativeFunctions = None;
      // Parse an optional operand list
      //   `with_derivative { <operand> , <operand> }`.
      if (P.Tok.is(tok::identifier) && P.Tok.getText() == "with_derivative") {
        P.consumeToken(tok::identifier);
        // Parse derivative function values as an operand list.
        // FIXME(rxwei): Change this to *not* require a type signature once
        // we can infer derivative function types.
        derivativeFunctions = std::make_pair(SILValue(), SILValue());
        if (P.parseToken(
                tok::l_brace,
                diag::sil_inst_autodiff_operand_list_expected_lbrace) ||
            parseTypedValueRef(derivativeFunctions->first, B) ||
            P.parseToken(tok::comma,
                         diag::sil_inst_autodiff_operand_list_expected_comma) ||
            parseTypedValueRef(derivativeFunctions->second, B) ||
            P.parseToken(tok::r_brace,
                         diag::sil_inst_autodiff_operand_list_expected_rbrace))
          return true;
      }

      ValueOwnershipKind forwardingOwnership(OwnershipKind::None);
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;
      auto *parameterIndices = IndexSubset::get(
          P.Context, fnType->getNumParameters(), rawParameterIndices);
      auto *resultIndices = IndexSubset::get(
          P.Context,
          fnType->getNumResults() + fnType->getNumIndirectMutatingParameters(),
          rawResultIndices);
      if (forwardingOwnership != OwnershipKind::None) {
        ResultVal = B.createDifferentiableFunction(
            InstLoc, parameterIndices, resultIndices, original,
            derivativeFunctions, forwardingOwnership);
      } else {
        ResultVal = B.createDifferentiableFunction(InstLoc, parameterIndices,
                                                   resultIndices, original,
                                                   derivativeFunctions);
      }
      break;
    }
    case SILInstructionKind::LinearFunctionInst: {
      // e.g. linear_function [parameters 0 1 2] %0 : $T
      // e.g. linear_function [parameters 0 1 2] %0 : $T with_transpose %1 : $T
      // Parse `[parameters <integer_literal>...]`.
      SmallVector<unsigned, 8> rawParameterIndices;
      if (parseIndexList(P, "parameters", rawParameterIndices,
                         diag::sil_autodiff_expected_parameter_index))
        return true;
      // Parse the original function value.
      SILValue original;
      SourceLoc originalOperandLoc;
      if (parseTypedValueRef(original, originalOperandLoc, B))
        return true;
      auto fnType = original->getType().getAs<SILFunctionType>();
      if (!fnType) {
        P.diagnose(originalOperandLoc,
                   diag::sil_inst_autodiff_expected_function_type_operand);
        return true;
      }
      // Parse an optional transpose function.
      Optional<SILValue> transpose = None;
      if (P.Tok.is(tok::identifier) && P.Tok.getText() == "with_transpose") {
        P.consumeToken(tok::identifier);
        transpose = SILValue();
        if (parseTypedValueRef(*transpose, B))
          return true;
      }

      ValueOwnershipKind forwardingOwnership(OwnershipKind::None);
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      auto *parameterIndicesSubset = IndexSubset::get(
          P.Context, fnType->getNumParameters(), rawParameterIndices);

      if (forwardingOwnership != OwnershipKind::None) {
        ResultVal =
            B.createLinearFunction(InstLoc, parameterIndicesSubset, original,
                                   forwardingOwnership, transpose);
      } else {
        ResultVal = B.createLinearFunction(InstLoc, parameterIndicesSubset,
                                           original, transpose);
      }
      break;
    }
    case SILInstructionKind::DifferentiableFunctionExtractInst: {
      // Parse the rest of the instruction: an extractee, a differentiable
      // function operand, an optional explicit extractee type, and a debug
      // location.
      NormalDifferentiableFunctionTypeComponent extractee;
      StringRef extracteeNames[3] = {"original", "jvp", "vjp"};
      SILValue functionOperand;
      SourceLoc lastLoc;
      if (P.parseToken(
              tok::l_square,
              diag::sil_inst_autodiff_expected_differentiable_extractee_kind) ||
          parseSILIdentifierSwitch(
              extractee, extracteeNames,
              diag::sil_inst_autodiff_expected_differentiable_extractee_kind) ||
          P.parseToken(tok::r_square, diag::sil_autodiff_expected_rsquare,
                       "extractee kind"))
        return true;
      if (parseTypedValueRef(functionOperand, B))
        return true;
      // Parse an optional explicit extractee type.
      Optional<SILType> extracteeType = None;
      if (P.consumeIf(tok::kw_as)) {
        extracteeType = SILType();
        if (parseSILType(*extracteeType))
          return true;
      }

      ValueOwnershipKind forwardingOwnership =
          functionOperand->getOwnershipKind();
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;

      ResultVal = B.createDifferentiableFunctionExtract(
          InstLoc, extractee, functionOperand, forwardingOwnership,
          extracteeType);
      break;
    }
    case SILInstructionKind::LinearFunctionExtractInst: {
      // Parse the rest of the instruction: an extractee, a linear function
      // operand, and a debug location.
      LinearDifferentiableFunctionTypeComponent extractee;
      StringRef extracteeNames[2] = {"original", "transpose"};
      SILValue functionOperand;
      SourceLoc lastLoc;
      if (P.parseToken(tok::l_square,
              diag::sil_inst_autodiff_expected_linear_extractee_kind) ||
          parseSILIdentifierSwitch(extractee, extracteeNames,
              diag::sil_inst_autodiff_expected_linear_extractee_kind) ||
          P.parseToken(tok::r_square, diag::sil_autodiff_expected_rsquare,
                       "extractee kind"))
        return true;
      if (parseTypedValueRef(functionOperand, B))
        return true;

      ValueOwnershipKind forwardingOwnership =
          functionOperand->getOwnershipKind();
      if (parseForwardingOwnershipKind(forwardingOwnership)
          || parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createLinearFunctionExtract(
          InstLoc, extractee, functionOperand, forwardingOwnership);
      break;
    }
    case SILInstructionKind::DifferentiabilityWitnessFunctionInst: {
      // e.g. differentiability_witness_function
      //      [jvp] [parameters 0 1] [results 0] <T where T: Differentiable>
      //      @foo : <T> $(T) -> T
      DifferentiabilityWitnessFunctionKind witnessKind;
      StringRef witnessKindNames[3] = {"jvp", "vjp", "transpose"};
      if (P.parseToken(
              tok::l_square,
              diag::
                  sil_inst_autodiff_expected_differentiability_witness_kind) ||
          parseSILIdentifierSwitch(
              witnessKind, witnessKindNames,
              diag::
                  sil_inst_autodiff_expected_differentiability_witness_kind) ||
          P.parseToken(tok::r_square, diag::sil_autodiff_expected_rsquare,
                       "differentiability witness function kind"))
        return true;
      SourceLoc keyStartLoc = P.Tok.getLoc();
      DifferentiabilityKind diffKind;
      AutoDiffConfig config;
      SILFunction *originalFn = nullptr;
      if (parseSILDifferentiabilityWitnessConfigAndFunction(
              P, *this, InstLoc, diffKind, config, originalFn))
        return true;
      auto *witness = SILMod.lookUpDifferentiabilityWitness(
          {originalFn->getName(), diffKind, config});
      if (!witness) {
        P.diagnose(keyStartLoc, diag::sil_diff_witness_undefined);
        return true;
      }
      // Parse an optional explicit function type.
      Optional<SILType> functionType = None;
      if (P.consumeIf(tok::kw_as)) {
        functionType = SILType();
        if (parseSILType(*functionType))
          return true;
      }
      ResultVal = B.createDifferentiabilityWitnessFunction(
          InstLoc, witnessKind, witness, functionType);
      break;
    }
    case SILInstructionKind::AwaitAsyncContinuationInst: {
      // 'await_async_continuation' operand, 'resume' bb, 'error' bb
      Identifier ResumeBBName, ErrorBBName{};
      SourceLoc ResumeNameLoc, ErrorNameLoc{};
      if (parseTypedValueRef(Val, B)
          || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
          || P.parseSpecificIdentifier("resume", diag::expected_tok_in_sil_instr, "resume")
          || parseSILIdentifier(ResumeBBName, ResumeNameLoc, diag::expected_sil_block_name)) {
        return true;
      }
      
      if (P.consumeIf(tok::comma)) {
          if (P.parseSpecificIdentifier("error", diag::expected_tok_in_sil_instr, "error")
              || parseSILIdentifier(ErrorBBName, ErrorNameLoc, diag::expected_sil_block_name)
              || parseSILDebugLocation(InstLoc, B)) {
            return true;
          }
      }
      
      SILBasicBlock *resumeBB, *errorBB = nullptr;
      resumeBB = getBBForReference(ResumeBBName, ResumeNameLoc);
      if (!ErrorBBName.empty()) {
        errorBB = getBBForReference(ErrorBBName, ErrorNameLoc);
      }
      ResultVal = B.createAwaitAsyncContinuation(InstLoc, Val, resumeBB, errorBB);
      break;
    }
    case SILInstructionKind::GetAsyncContinuationInst:
    case SILInstructionKind::GetAsyncContinuationAddrInst: {
      // 'get_async_continuation'      '[throws]'? type
      // 'get_async_continuation_addr' '[throws]'? type ',' operand
      bool throws = false;
      if (P.consumeIf(tok::l_square)) {
        if (P.parseToken(tok::kw_throws, diag::expected_tok_in_sil_instr, "throws")
            || P.parseToken(tok::r_square, diag::expected_tok_in_sil_instr, "]"))
          return true;
        
        throws = true;
      }
      
      CanType resumeTy;
      if (parseASTType(resumeTy)) {
        return true;
      }
      
      SILValue resumeBuffer;
      if (Opcode == SILInstructionKind::GetAsyncContinuationAddrInst) {
        if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
            || parseTypedValueRef(resumeBuffer, B)) {
          return true;
        }
      }
      
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      
      if (Opcode == SILInstructionKind::GetAsyncContinuationAddrInst) {
        ResultVal = B.createGetAsyncContinuationAddr(InstLoc, resumeBuffer,
                                                     resumeTy, throws);
      } else {
        ResultVal = B.createGetAsyncContinuation(InstLoc, resumeTy, throws);
      }
      break;
    }
    case SILInstructionKind::HasSymbolInst: {
      // 'has_symbol' sil-decl-ref
      SILDeclRef declRef;
      if (parseSILDeclRef(declRef))
        return true;

      ResultVal = B.createHasSymbol(InstLoc, declRef.getDecl());
      break;
    }

    }

    return false;
}

/// sil-instruction-result ::= sil-value-name '='
/// sil-instruction-result ::= '(' sil-value-name? ')'
/// sil-instruction-result ::= '(' sil-value-name (',' sil-value-name)* ')'
/// sil-instruction-source-info ::= (',' sil-scope-ref)? (',' sil-loc)?
/// sil-instruction-def ::=
///   (sil-instruction-result '=')? sil-instruction sil-instruction-source-info
bool SILParser::parseSILInstruction(SILBuilder &B) {
  // We require SIL instructions to be at the start of a line to assist
  // recovery.
  if (!P.Tok.isAtStartOfLine()) {
    P.diagnose(P.Tok, diag::expected_sil_instr_start_of_line);
    return true;
  }

  if (!B.hasValidInsertionPoint()) {
    P.diagnose(P.Tok, diag::expected_sil_block_name);
    return true;
  }

  SmallVector<Located<StringRef>, 4> resultNames;
  SourceLoc resultClauseBegin;

  // If the instruction has a name '%foo =', parse it.
  if (P.Tok.is(tok::sil_local_name)) {
    resultClauseBegin = P.Tok.getLoc();
    resultNames.push_back({P.Tok.getText(), P.Tok.getLoc()});
    P.consumeToken(tok::sil_local_name);

    // If the instruction has a '(%foo, %bar) = ', parse it.
  } else if (P.consumeIf(tok::l_paren)) {
    resultClauseBegin = P.PreviousLoc;

    if (!P.consumeIf(tok::r_paren)) {
      while (true) {
        if (!P.Tok.is(tok::sil_local_name)) {
          P.diagnose(P.Tok, diag::expected_sil_value_name);
          return true;
        }

        resultNames.push_back({P.Tok.getText(), P.Tok.getLoc()});
        P.consumeToken(tok::sil_local_name);

        if (P.consumeIf(tok::comma))
          continue;
        if (P.consumeIf(tok::r_paren))
          break;

        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ",");
        return true;
      }
    }
  }

  if (resultClauseBegin.isValid()) {
    if (P.parseToken(tok::equal, diag::expected_equal_in_sil_instr))
      return true;
  }

  SILInstructionKind Opcode;
  SourceLoc OpcodeLoc;
  StringRef OpcodeName;

  // Parse the opcode name.
  if (parseSILOpcode(Opcode, OpcodeLoc, OpcodeName))
    return true;

  // Perform opcode specific parsing.
  SILInstruction *ResultVal;
  if (parseSpecificSILInstruction(B, Opcode, OpcodeLoc, OpcodeName, ResultVal))
    return true;

  // Match the results clause if we had one.
  if (resultClauseBegin.isValid()) {
    auto results = ResultVal->getResults();
    if (results.size() != resultNames.size()) {
      P.diagnose(resultClauseBegin, diag::wrong_result_count_in_sil_instr,
                 results.size());
    } else {
      for (size_t i : indices(results)) {
        setLocalValue(results[i], resultNames[i].Item, resultNames[i].Loc);
      }
    }
  }

  return false;
}

bool SILParser::parseCallInstruction(SILLocation InstLoc,
                                     SILInstructionKind Opcode, SILBuilder &B,
                                     SILInstruction *&ResultVal) {
  UnresolvedValueName FnName;
  SmallVector<UnresolvedValueName, 4> ArgNames;

  auto PartialApplyConvention = ParameterConvention::Direct_Owned;
  ApplyOptions ApplyOpts;
  bool IsNoEscape = false;
  StringRef AttrName;

  while (parseSILOptional(AttrName, *this)) {
    if (AttrName.equals("nothrow"))
      ApplyOpts |= ApplyFlags::DoesNotThrow;
    else if (AttrName.equals("noasync"))
      ApplyOpts |= ApplyFlags::DoesNotAwait;
    else if (AttrName.equals("callee_guaranteed"))
      PartialApplyConvention = ParameterConvention::Direct_Guaranteed;
    else if (AttrName.equals("on_stack"))
      IsNoEscape = true;
    else
      return true;
  }
  
  if (parseValueName(FnName))
    return true;
  SmallVector<ParsedSubstitution, 4> parsedSubs;
  if (parseSubstitutions(parsedSubs))
    return true;
    
  if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
    return true;

  if (P.Tok.isNot(tok::r_paren)) {
    do {
      UnresolvedValueName Arg;
      if (parseValueName(Arg)) return true;
      ArgNames.push_back(Arg);
    } while (P.consumeIf(tok::comma));
  }

  SILType Ty;
  SourceLoc TypeLoc;
  GenericSignature GenericSig;
  GenericParamList *GenericParams = nullptr;
  if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")") ||
      P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
      parseSILType(Ty, TypeLoc, GenericSig, GenericParams))
    return true;

  auto FTI = Ty.getAs<SILFunctionType>();
  if (!FTI) {
    P.diagnose(TypeLoc, diag::expected_sil_type_kind, "be a function");
    return true;
  }

  SubstitutionMap subs;
  if (!parsedSubs.empty()) {
    if (!GenericSig) {
      P.diagnose(TypeLoc, diag::sil_substitutions_on_non_polymorphic_type);
      return true;
    }
    subs = getApplySubstitutionsFromParsed(*this, GenericSig, parsedSubs);
    if (!subs)
      return true;
  }

  SILValue FnVal = getLocalValue(FnName, Ty, InstLoc, B);

  SILType FnTy = FnVal->getType();
  CanSILFunctionType substFTI = FTI;
  if (!subs.empty()) {
    auto silFnTy = FnTy.castTo<SILFunctionType>();
    substFTI =
        silFnTy->substGenericArgs(SILMod, subs, B.getTypeExpansionContext());
    FnTy = SILType::getPrimitiveObjectType(substFTI);
  }
  SILFunctionConventions substConv(substFTI, B.getModule());

  // Validate the operand count.
  if (substConv.getNumSILArguments() != ArgNames.size() &&
      Opcode != SILInstructionKind::PartialApplyInst) {
    P.diagnose(TypeLoc, diag::expected_sil_type_kind,
               "to have the same number of arg names as arg types");
    return true;
  }

  // Validate the coroutine kind.
  if (Opcode == SILInstructionKind::ApplyInst ||
      Opcode == SILInstructionKind::TryApplyInst) {
    if (FTI->getCoroutineKind() != SILCoroutineKind::None) {
      P.diagnose(TypeLoc, diag::expected_sil_type_kind,
                 "to not be a coroutine");
      return true;
    }
  } else if (Opcode == SILInstructionKind::BeginApplyInst) {
    if (FTI->getCoroutineKind() != SILCoroutineKind::YieldOnce) {
      P.diagnose(TypeLoc, diag::expected_sil_type_kind,
                 "to be a yield_once coroutine");
      return true;
    }
  } else {
    assert(Opcode == SILInstructionKind::PartialApplyInst);
    // partial_apply accepts all kinds of function
  }

  switch (Opcode) {
  default: llvm_unreachable("Unexpected case");
  case SILInstructionKind::ApplyInst : {
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    unsigned ArgNo = 0;
    SmallVector<SILValue, 4> Args;
    for (auto &ArgName : ArgNames) {
      SILType expectedTy =
          substConv.getSILArgumentType(ArgNo++, B.getTypeExpansionContext());
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    ResultVal = B.createApply(InstLoc, FnVal, subs, Args, ApplyOpts);
    break;
  }
  case SILInstructionKind::BeginApplyInst: {
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    
    unsigned ArgNo = 0;
    SmallVector<SILValue, 4> Args;
    for (auto &ArgName : ArgNames) {
      SILType expectedTy =
          substConv.getSILArgumentType(ArgNo++, B.getTypeExpansionContext());
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    ResultVal =
      B.createBeginApply(InstLoc, FnVal, subs, Args, ApplyOpts);
    break;
  }
  case SILInstructionKind::PartialApplyInst: {
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    // Compute the result type of the partial_apply, based on which arguments
    // are getting applied.
    SmallVector<SILValue, 4> Args;
    unsigned ArgNo = substConv.getNumSILArguments() - ArgNames.size();
    for (auto &ArgName : ArgNames) {
      SILType expectedTy =
          substConv.getSILArgumentType(ArgNo++, B.getTypeExpansionContext());
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    // FIXME: Why the arbitrary order difference in IRBuilder type argument?
    ResultVal = B.createPartialApply(
        InstLoc, FnVal, subs, Args, PartialApplyConvention,
        IsNoEscape ? PartialApplyInst::OnStackKind::OnStack
                   : PartialApplyInst::OnStackKind::NotOnStack);
    break;
  }
  case SILInstructionKind::TryApplyInst: {
    Identifier normalBBName, errorBBName;
    SourceLoc normalBBLoc, errorBBLoc;
    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("normal") ||
        parseSILIdentifier(normalBBName, normalBBLoc,
                           diag::expected_sil_block_name) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("error") ||
        parseSILIdentifier(errorBBName, errorBBLoc,
                           diag::expected_sil_block_name) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    unsigned argNo = 0;
    SmallVector<SILValue, 4> args;
    for (auto &argName : ArgNames) {
      SILType expectedTy =
          substConv.getSILArgumentType(argNo++, B.getTypeExpansionContext());
      args.push_back(getLocalValue(argName, expectedTy, InstLoc, B));
    }

    SILBasicBlock *normalBB = getBBForReference(normalBBName, normalBBLoc);
    SILBasicBlock *errorBB = getBBForReference(errorBBName, errorBBLoc);
    ResultVal = B.createTryApply(InstLoc, FnVal, subs, args, normalBB, errorBB,
                                 ApplyOpts);
    break;
  }
  }
  return false;
}

bool SILParser::parseSILFunctionRef(SILLocation InstLoc,
                                    SILFunction *&ResultFn) {
  Identifier Name;
  SILType Ty;
  SourceLoc Loc = P.Tok.getLoc();
  if (parseGlobalName(Name) ||
      P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
      parseSILType(Ty))
    return true;

  auto FnTy = Ty.getAs<SILFunctionType>();
  if (!FnTy || !Ty.isObject()) {
    P.diagnose(Loc, diag::expected_sil_function_type);
    return true;
  }
  
  ResultFn = getGlobalNameForReference(Name, FnTy, Loc);
  return false;
}

/// True if the current token sequence looks like the start of a SIL
/// instruction. This can be one of:
///
/// 1. %name
/// 2. ()
/// 3. (%name1
/// 4. identifier | keyword
///   where the identifier is not followed by a ':' or '(', or it is
///   followed by '(' and is an instruction name.  The exceptions here
///   are for recognizing block names.
bool SILParser::isStartOfSILInstruction() {
  if (P.Tok.is(tok::sil_local_name))
    return true;
  if (P.Tok.is(tok::l_paren) &&
      (P.peekToken().is(tok::sil_local_name) || P.peekToken().is(tok::r_paren)))
    return true;
  if (P.Tok.is(tok::identifier) || P.Tok.isKeyword()) {
    auto &peek = P.peekToken();
    if (peek.is(tok::l_paren))
      return getOpcodeByName(P.Tok.getText()).has_value();
    return !peek.is(tok::colon);
  }
  return false;
}

///   sil-basic-block:
///     sil-instruction+
///     identifier sil-bb-argument-list? ':' sil-instruction+
///   sil-bb-argument-list:
///     '(' sil-typed-valueref (',' sil-typed-valueref)+ ')'
bool SILParser::parseSILBasicBlock(SILBuilder &B) {
  SILBasicBlock *BB;

  // The basic block name is optional.
  if (P.Tok.is(tok::sil_local_name)) {
    BB = getBBForDefinition(Identifier(), SourceLoc());
  } else {
    Identifier BBName;
    SourceLoc NameLoc;
    if (parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name))
      return true;

    BB = getBBForDefinition(BBName, NameLoc);
    // For now, since we always assume that PhiArguments have
    // OwnershipKind::None, do not parse or do anything special. Eventually
    // we will parse the convention.
    bool IsEntry = BB->isEntry();

    // If there is a basic block argument list, process it.
    if (P.consumeIf(tok::l_paren)) {
      do {
        SILType Ty;
        ValueOwnershipKind OwnershipKind = OwnershipKind::None;
        SourceLoc NameLoc;
        StringRef Name = P.Tok.getText();
        if (P.parseToken(tok::sil_local_name, NameLoc,
                         diag::expected_sil_value_name) ||
            P.parseToken(tok::colon, diag::expected_sil_colon_value_ref))
          return true;

        bool foundNoImplicitCopy = false;
        bool foundClosureCapture = false;
        bool foundLexical = false;
        bool foundEagerMove = false;
        bool foundReborrow = false;
        bool foundEscaping = false;
        while (auto attributeName = parseOptionalAttribute(
                   {"noImplicitCopy", "_lexical", "_eagerMove",
                    "closureCapture", "reborrow", "escaping"})) {
          if (*attributeName == "noImplicitCopy")
            foundNoImplicitCopy = true;
          else if (*attributeName == "_lexical")
            foundLexical = true;
          else if (*attributeName == "_eagerMove")
            foundEagerMove = true;
          else if (*attributeName == "closureCapture")
            foundClosureCapture = true;
          else if (*attributeName == "reborrow")
            foundReborrow = true;
          else if (*attributeName == "escaping")
            foundEscaping = true;
          else {
            llvm_unreachable("Unexpected attribute!");
          }
        }

        LifetimeAnnotation lifetime = LifetimeAnnotation::None;
        if (foundEagerMove && foundLexical) {
          P.diagnose(NameLoc, diag::sil_arg_both_lexical_and_eagerMove);
          return true;
        } else if (foundEagerMove) {
          lifetime = LifetimeAnnotation::EagerMove;
        } else if (foundLexical) {
          lifetime = LifetimeAnnotation::Lexical;
        }

        // If SILOwnership is enabled and we are not assuming that we are
        // parsing unqualified SIL, look for printed value ownership kinds.
        if (F->hasOwnership() &&
            parseSILOwnership(OwnershipKind))
          return true;

        if (parseSILType(Ty))
          return true;

        SILArgument *Arg;
        if (IsEntry) {
          auto *fArg = BB->createFunctionArgument(Ty);
          fArg->setNoImplicitCopy(foundNoImplicitCopy);
          fArg->setClosureCapture(foundClosureCapture);
          fArg->setLifetimeAnnotation(lifetime);
          fArg->setReborrow(foundReborrow);
          fArg->setEscaping(foundEscaping);
          Arg = fArg;

          // Today, we construct the ownership kind straight from the function
          // type. Make sure they are in sync, otherwise bail. We want this to
          // be an exact check rather than a compatibility check since we do not
          // want incompatibilities in between @any and other types of ownership
          // to be ignored.
          if (F->hasOwnership() && Arg->getOwnershipKind() != OwnershipKind) {
            auto diagID =
                diag::silfunc_and_silarg_have_incompatible_sil_value_ownership;
            P.diagnose(NameLoc, diagID, Arg->getOwnershipKind().asString(),
                       OwnershipKind.asString());
            return true;
          }
        } else {
          Arg = BB->createPhiArgument(Ty, OwnershipKind, /*decl*/ nullptr,
                                      foundReborrow, foundEscaping);
        }
        setLocalValue(Arg, Name, NameLoc);
      } while (P.consumeIf(tok::comma));
      
      if (P.parseToken(tok::r_paren, diag::sil_basicblock_arg_rparen))
        return true;
    }
    
    if (P.parseToken(tok::colon, diag::expected_sil_block_colon))
      return true;
  }
  
  // Make sure the block is at the end of the function so that forward
  // references don't affect block layout.
  F->moveBlockBefore(BB, F->end());

  B.setInsertionPoint(BB);
  do {
    if (parseSILInstruction(B))
      return true;
  } while (isStartOfSILInstruction());

  return false;
}

static void populateOwnershipFlags(SILFunction *func) {
  for (auto &bb : *func) {
    for (auto &arg : bb.getArguments()) {
      if (computeIsReborrow(arg)) {
        arg->setReborrow(true);
      }
    }
  }
}

///   decl-sil:   [[only in SIL mode]]
///     'sil' sil-linkage '@' identifier ':' sil-type decl-sil-body?
///   decl-sil-body:
///     '{' sil-basic-block+ '}'
bool SILParserState::parseDeclSIL(Parser &P) {
  // Inform the lexer that we're lexing the body of the SIL declaration.  Do
  // this before we consume the 'sil' token so that all later tokens are
  // properly handled.
  Lexer::SILBodyRAII Tmp(*P.L);

  P.consumeToken(tok::kw_sil);

  SILParser FunctionState(P);

  Optional<SILLinkage> FnLinkage;
  Identifier FnName;
  SILType FnType;
  SourceLoc FnNameLoc;

  bool isTransparent = false;
  IsSerialized_t isSerialized = IsNotSerialized;
  bool isCanonical = false;
  IsDynamicallyReplaceable_t isDynamic = IsNotDynamic;
  IsDistributed_t isDistributed = IsNotDistributed;
  IsRuntimeAccessible_t isRuntimeAccessible = IsNotRuntimeAccessible;
  ForceEnableLexicalLifetimes_t forceEnableLexicalLifetimes =
      DoNotForceEnableLexicalLifetimes;
  IsExactSelfClass_t isExactSelfClass = IsNotExactSelfClass;
  bool hasOwnershipSSA = false;
  IsThunk_t isThunk = IsNotThunk;
  SILFunction::Purpose specialPurpose = SILFunction::Purpose::None;
  bool isWeakImported = false;
  bool needStackProtection = false;
  AvailabilityContext availability = AvailabilityContext::alwaysAvailable();
  bool isWithoutActuallyEscapingThunk = false;
  Inline_t inlineStrategy = InlineDefault;
  OptimizationMode optimizationMode = OptimizationMode::NotSet;
  PerformanceConstraints perfConstr = PerformanceConstraints::None;
  SmallVector<std::string, 1> Semantics;
  SmallVector<ParsedSpecAttr, 4> SpecAttrs;
  ValueDecl *ClangDecl = nullptr;
  EffectsKind MRK = EffectsKind::Unspecified;
  SILFunction *DynamicallyReplacedFunction = nullptr;
  SILFunction *AdHocWitnessFunction = nullptr;
  Identifier objCReplacementFor;
  if (parseSILLinkage(FnLinkage, P) ||
      parseDeclSILOptional(
          &isTransparent, &isSerialized, &isCanonical, &hasOwnershipSSA,
          &isThunk, &isDynamic, &isDistributed, &isRuntimeAccessible,
          &forceEnableLexicalLifetimes, &isExactSelfClass,
          &DynamicallyReplacedFunction, &AdHocWitnessFunction,
          &objCReplacementFor, &specialPurpose, &inlineStrategy,
          &optimizationMode, &perfConstr, nullptr, &isWeakImported,
          &needStackProtection, &availability, &isWithoutActuallyEscapingThunk,
          &Semantics, &SpecAttrs, &ClangDecl, &MRK, FunctionState, M) ||
      P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
      P.parseIdentifier(FnName, FnNameLoc, /*diagnoseDollarPrefix=*/false,
                        diag::expected_sil_function_name) ||
      P.parseToken(tok::colon, diag::expected_sil_type))
    return true;
  {
    // Construct a Scope for the function body so TypeAliasDecl can be added to
    // the scope.
    GenericSignature GenericSig;
    GenericParamList *GenericParams = nullptr;
    if (FunctionState.parseSILType(FnType, GenericSig, GenericParams,
                                   true /*IsFuncDecl*/))
      return true;
    auto SILFnType = FnType.getAs<SILFunctionType>();
    if (!SILFnType || !FnType.isObject()) {
      P.diagnose(FnNameLoc, diag::expected_sil_function_type);
      return true;
    }
  
    FunctionState.F =
      FunctionState.getGlobalNameForDefinition(FnName, SILFnType, FnNameLoc);
    FunctionState.F->setBare(IsBare);
    FunctionState.F->setTransparent(IsTransparent_t(isTransparent));
    FunctionState.F->setSerialized(IsSerialized_t(isSerialized));
    FunctionState.F->setWasDeserializedCanonical(isCanonical);
    if (!hasOwnershipSSA)
      FunctionState.F->setOwnershipEliminated();
    FunctionState.F->setThunk(IsThunk_t(isThunk));
    FunctionState.F->setIsDynamic(isDynamic);
    FunctionState.F->setIsDistributed(isDistributed);
    FunctionState.F->setIsRuntimeAccessible(isRuntimeAccessible);
    FunctionState.F->setForceEnableLexicalLifetimes(
        forceEnableLexicalLifetimes);
    FunctionState.F->setIsExactSelfClass(isExactSelfClass);
    FunctionState.F->setDynamicallyReplacedFunction(
        DynamicallyReplacedFunction);
    FunctionState.F->setReferencedAdHocRequirementWitnessFunction(
        AdHocWitnessFunction);
    if (!objCReplacementFor.empty())
      FunctionState.F->setObjCReplacement(objCReplacementFor);
    FunctionState.F->setSpecialPurpose(specialPurpose);
    FunctionState.F->setIsAlwaysWeakImported(isWeakImported);
    FunctionState.F->setAvailabilityForLinkage(availability);
    FunctionState.F->setWithoutActuallyEscapingThunk(
      isWithoutActuallyEscapingThunk);
    FunctionState.F->setInlineStrategy(inlineStrategy);
    FunctionState.F->setOptimizationMode(optimizationMode);
    FunctionState.F->setPerfConstraints(perfConstr);
    FunctionState.F->setEffectsKind(MRK);

    if (ClangDecl)
      FunctionState.F->setClangNodeOwner(ClangDecl);
    for (auto &Attr : Semantics) {
      FunctionState.F->addSemanticsAttr(Attr);
    }
    // Now that we have a SILFunction parse the body, if present.

    bool isDefinition = false;
    SourceLoc LBraceLoc = P.Tok.getLoc();

    if (P.consumeIf(tok::l_brace)) {
      while (P.consumeIf(tok::l_square)) {
        SourceLoc effectsStart = P.Tok.getLoc();
        while (P.Tok.isNot(tok::r_square, tok::eof)) {
          P.consumeToken();
        }
        SourceLoc effectsEnd = P.Tok.getLoc();
        P.consumeToken();
        StringRef effectsStr = P.SourceMgr.extractText(
                      CharSourceRange(P.SourceMgr, effectsStart, effectsEnd));

        auto error = FunctionState.F->parseMultipleEffectsFromSIL(effectsStr);
        if (error.first) {
          SourceLoc loc = effectsStart;
          if (loc.isValid())
            loc = loc.getAdvancedLoc(error.second);
          P.diagnose(loc, diag::error_in_effects_attribute, StringRef(error.first));
          return true;
        }
      }
      if (!P.consumeIf(tok::r_brace)) {
        isDefinition = true;

        FunctionState.ContextGenericSig = GenericSig;
        FunctionState.ContextGenericParams = GenericParams;
        FunctionState.F->setGenericEnvironment(GenericSig.getGenericEnvironment());

        if (GenericSig && !SpecAttrs.empty()) {
          for (auto &Attr : SpecAttrs) {
            SmallVector<Requirement, 2> requirements;
            SmallVector<Type, 2> typeErasedParams;
            // Resolve types and convert requirements.
            FunctionState.convertRequirements(Attr.requirements, requirements, typeErasedParams);
            auto *fenv = FunctionState.F->getGenericEnvironment();
            auto genericSig = buildGenericSignature(P.Context,
                                                    fenv->getGenericSignature(),
                                                    /*addedGenericParams=*/{ },
                                                    std::move(requirements));
            FunctionState.F->addSpecializeAttr(SILSpecializeAttr::create(
                FunctionState.F->getModule(), genericSig, typeErasedParams, Attr.exported,
                Attr.kind, Attr.target, Attr.spiGroupID, Attr.spiModule, Attr.availability));
          }
        }

        // Parse the basic block list.
        SILBuilder B(*FunctionState.F);

        do {
          if (FunctionState.parseSILBasicBlock(B))
            return true;
        } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));

        // OSSA uses flags to represent "reborrow", "escaping" etc.
        // These flags are populated here as a shortcut to rewriting all
        // existing OSSA tests with flags.
        if (!DisablePopulateOwnershipFlags) {
          populateOwnershipFlags(FunctionState.F);
        }

        SourceLoc RBraceLoc;
        P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                             LBraceLoc);

        // Check that there are no unresolved forward definitions of local
        // archetypes.
        if (M.hasUnresolvedLocalArchetypeDefinitions())
          llvm_unreachable(
              "All forward definitions of local archetypes should be resolved");
      }
    }

    FunctionState.F->setLinkage(resolveSILLinkage(FnLinkage, isDefinition));
  }

  if (FunctionState.diagnoseProblems())
    return true;

  return false;
}

///   decl-sil-stage:   [[only in SIL mode]]
///     'sil_stage' ('raw' | 'canonical')
bool SILParserState::parseDeclSILStage(Parser &P) {
  SourceLoc stageLoc = P.consumeToken(tok::kw_sil_stage);
  if (!P.Tok.is(tok::identifier)) {
    P.diagnose(P.Tok, diag::expected_sil_stage_name);
    return true;
  }
  SILStage stage;
  if (P.Tok.isContextualKeyword("raw")) {
    stage = SILStage::Raw;
    P.consumeToken();
  } else if (P.Tok.isContextualKeyword("canonical")) {
    stage = SILStage::Canonical;
    P.consumeToken();
  } else if (P.Tok.isContextualKeyword("lowered")) {
    stage = SILStage::Lowered;
    P.consumeToken();
  } else {
    P.diagnose(P.Tok, diag::expected_sil_stage_name);
    P.consumeToken();
    return true;
  }
  
  if (DidParseSILStage) {
    P.diagnose(stageLoc, diag::multiple_sil_stage_decls);
    return false;
  }
  
  M.setStage(stage);
  if (M.getOptions().EnableSILOpaqueValues) {
    M.setLoweredAddresses(stage != SILStage::Raw);
  }
  DidParseSILStage = true;
  return false;
}

/// Lookup a global variable declaration from its demangled name.
///
/// A variable declaration exists for all sil_global variables defined in
/// Swift. A Swift global defined outside this module will be exposed
/// via an addressor rather than as a sil_global. Globals imported
/// from clang will produce a sil_global but will not have any corresponding
/// VarDecl.
///
/// FIXME: lookupGlobalDecl() can handle collisions between private or
/// fileprivate global variables in the same SIL Module, but the typechecker
/// will still incorrectly diagnose this as an "invalid redeclaration" and give
/// all but the first declaration an error type.
static Optional<VarDecl *> lookupGlobalDecl(Identifier GlobalName,
                                            SILLinkage GlobalLinkage,
                                            SILType GlobalType, Parser &P) {
  // Create a set of DemangleOptions to produce the global variable's
  // identifier, which is used as a search key in the declaration context.
  Demangle::DemangleOptions demangleOpts;
  demangleOpts.QualifyEntities = false;
  demangleOpts.ShowPrivateDiscriminators = false;
  demangleOpts.DisplayEntityTypes = false;
  std::string GlobalDeclName = Demangle::demangleSymbolAsString(
    GlobalName.str(), demangleOpts);

  SmallVector<ValueDecl *, 4> CurModuleResults;
  P.SF.getParentModule()->lookupValue(
      P.Context.getIdentifier(GlobalDeclName), NLKind::UnqualifiedLookup,
      CurModuleResults);
  // Bail-out on clang-imported globals.
  if (CurModuleResults.empty())
    return nullptr;

  // private and fileprivate globals of the same name may be merged into a
  // single SIL module. Find the declaration with the correct type and
  // linkage. (If multiple globals have the same type and linkage then it
  // doesn't matter which declaration we use).
  for (ValueDecl *ValDecl : CurModuleResults) {
    auto *VD = cast<VarDecl>(ValDecl);
    CanType DeclTy = VD->getType()->getCanonicalType();
    if (DeclTy == GlobalType.getASTType()
        && getDeclSILLinkage(VD) == GlobalLinkage) {
      return VD;
    }
  }
  return None;
}

/// decl-sil-global: [[only in SIL mode]]
///   'sil_global' sil-linkage @name : sil-type [external]
bool SILParserState::parseSILGlobal(Parser &P) {
  // Inform the lexer that we're lexing the body of the SIL declaration.
  Lexer::SILBodyRAII Tmp(*P.L);

  P.consumeToken(tok::kw_sil_global);
  Optional<SILLinkage> GlobalLinkage;
  Identifier GlobalName;
  SILType GlobalType;
  SourceLoc NameLoc;
  IsSerialized_t isSerialized = IsNotSerialized;
  bool isLet = false;

  SILParser State(P);
  if (parseSILLinkage(GlobalLinkage, P) ||
      parseDeclSILOptional(nullptr, &isSerialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &isLet, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, State, M) ||
      P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
      P.parseIdentifier(GlobalName, NameLoc, /*diagnoseDollarPrefix=*/false,
                        diag::expected_sil_value_name) ||
      P.parseToken(tok::colon, diag::expected_sil_type))
    return true;

  if (State.parseSILType(GlobalType))
    return true;

  // Non-external global variables are definitions by default.
  if (!GlobalLinkage.has_value())
    GlobalLinkage = SILLinkage::DefaultForDefinition;

  // Lookup the global variable declaration for this sil_global.
  auto VD =
      lookupGlobalDecl(GlobalName, GlobalLinkage.value(), GlobalType, P);
  if (!VD) {
    P.diagnose(NameLoc, diag::sil_global_variable_not_found, GlobalName);
    return true;
  }
  auto *GV = SILGlobalVariable::create(
      M, GlobalLinkage.value(), isSerialized, GlobalName.str(), GlobalType,
      RegularLocation(NameLoc), VD.value());

  GV->setLet(isLet);
  // Parse static initializer if exists.
  if (State.P.consumeIf(tok::equal) && State.P.consumeIf(tok::l_brace)) {
    SILBuilder B(GV);
    do {
      State.parseSILInstruction(B);
    } while (! State.P.consumeIf(tok::r_brace));
  }
  return false;
}

/// decl-sil-property: [[only in SIL mode]]
///   'sil_property' sil-decl-ref '(' sil-key-path-pattern-component ')'

bool SILParserState::parseSILProperty(Parser &P) {
  Lexer::SILBodyRAII Tmp(*P.L);

  auto loc = P.consumeToken(tok::kw_sil_property);
  auto InstLoc = RegularLocation(loc);
  SILParser SP(P);
  
  IsSerialized_t Serialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &Serialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, SP, M))
    return true;
  
  ValueDecl *VD;
  
  if (SP.parseSILDottedPath(VD))
    return true;
  
  GenericParamList *patternParams;
  patternParams = P.maybeParseGenericParams().getPtrOrNull();
  auto patternSig = handleSILGenericParams(patternParams, &P.SF);

  if (patternSig) {
    if (patternSig.getCanonicalSignature() !=
        VD->getInnermostDeclContext()
            ->getGenericSignatureOfContext()
            .getCanonicalSignature()) {
      P.diagnose(loc, diag::sil_property_generic_signature_mismatch);
      return true;
    }
  } else {
    if (VD->getInnermostDeclContext()->getGenericSignatureOfContext()) {
      P.diagnose(loc, diag::sil_property_generic_signature_mismatch);
      return true;
    }
  }

  Identifier ComponentKind;
  Optional<KeyPathPatternComponent> Component;
  SourceLoc ComponentLoc;
  SmallVector<SILType, 4> OperandTypes;

  if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
    return true;
  
  if (!P.consumeIf(tok::r_paren)) {
    KeyPathPatternComponent parsedComponent;
    if (P.parseIdentifier(ComponentKind, ComponentLoc,
                          /*diagnoseDollarPrefix=*/false,
                          diag::expected_tok_in_sil_instr, "component kind")
        || SP.parseKeyPathPatternComponent(parsedComponent, OperandTypes,
                 ComponentLoc, ComponentKind, InstLoc,
                 patternSig, patternParams)
        || P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
      return true;
    
    Component = std::move(parsedComponent);
  }
  
  SILProperty::create(M, Serialized,
                      cast<AbstractStorageDecl>(VD), Component);
  return false;
}

/// decl-sil-vtable: [[only in SIL mode]]
///   'sil_vtable' ClassName decl-sil-vtable-body
/// decl-sil-vtable-body:
///   '{' sil-vtable-entry* '}'
/// sil-vtable-entry:
///   SILDeclRef ':' SILFunctionName
bool SILParserState::parseSILVTable(Parser &P) {
  P.consumeToken(tok::kw_sil_vtable);
  SILParser VTableState(P);

  IsSerialized_t Serialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &Serialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, VTableState, M))
    return true;

  // Parse the class name.
  Identifier Name;
  SourceLoc Loc;
  if (VTableState.parseSILIdentifier(Name, Loc,
                                     diag::expected_sil_value_name))
    return true;

  // Find the class decl.
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res =
    lookupTopDecl(P, Name, /*typeLookup=*/true);
  assert(Res.is<ValueDecl*>() && "Class look-up should return a Decl");
  ValueDecl *VD = Res.get<ValueDecl*>();
  if (!VD) {
    P.diagnose(Loc, diag::sil_vtable_class_not_found, Name);
    return true;
  }

  auto *theClass = dyn_cast<ClassDecl>(VD);
  if (!theClass) {
    P.diagnose(Loc, diag::sil_vtable_class_not_found, Name);
    return true;
  }

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);
  // Parse the entry list.
  std::vector<SILVTable::Entry> vtableEntries;
  if (P.Tok.isNot(tok::r_brace)) {
    do {
      SILDeclRef Ref;
      Identifier FuncName;
      SourceLoc FuncLoc;
      if (VTableState.parseSILDeclRef(Ref, true))
        return true;
      SILFunction *Func = nullptr;
      if (P.Tok.is(tok::kw_nil)) {
        P.consumeToken();
      } else {
        if (P.parseToken(tok::colon, diag::expected_sil_vtable_colon) ||
            P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
            VTableState.parseSILIdentifier(FuncName, FuncLoc,
                                           diag::expected_sil_value_name))
        return true;
        Func = M.lookUpFunction(FuncName.str());
        if (!Func) {
          P.diagnose(FuncLoc, diag::sil_vtable_func_not_found, FuncName);
          return true;
        }
      }

      auto Kind = SILVTable::Entry::Kind::Normal;
      bool NonOverridden = false;
      while (P.Tok.is(tok::l_square)) {
        P.consumeToken(tok::l_square);
        if (P.Tok.isNot(tok::identifier)) {
          P.diagnose(P.Tok.getLoc(), diag::sil_vtable_bad_entry_kind);
          return true;
        }

        if (P.Tok.getText() == "override") {
          P.consumeToken();
          Kind = SILVTable::Entry::Kind::Override;
        } else if (P.Tok.getText() == "inherited") {
          P.consumeToken();
          Kind = SILVTable::Entry::Kind::Inherited;
        } else if (P.Tok.getText() == "nonoverridden") {
          P.consumeToken();
          NonOverridden = true;
        } else {
          P.diagnose(P.Tok.getLoc(), diag::sil_vtable_bad_entry_kind);
          return true;
        }

        if (P.parseToken(tok::r_square, diag::sil_vtable_expect_rsquare))
          return true;
      }

      vtableEntries.emplace_back(Ref, Func, Kind, NonOverridden);
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  SILVTable::create(M, theClass, Serialized, vtableEntries);
  return false;
}

/// decl-sil-vtable: [[only in SIL mode]]
///   'sil_vtable' ClassName decl-sil-vtable-body
/// decl-sil-vtable-body:
///   '{' sil-vtable-entry* '}'
/// sil-vtable-entry:
///   SILDeclRef ':' SILFunctionName
bool SILParserState::parseSILMoveOnlyDeinit(Parser &parser) {
  parser.consumeToken(tok::kw_sil_moveonlydeinit);
  SILParser moveOnlyDeinitTableState(parser);

  IsSerialized_t Serialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &Serialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, moveOnlyDeinitTableState,
                           M))
    return true;

  // Parse the class name.
  Identifier name;
  SourceLoc loc;
  if (moveOnlyDeinitTableState.parseSILIdentifier(
          name, loc, diag::expected_sil_value_name))
    return true;

  // Find the nominal decl.
  llvm::PointerUnion<ValueDecl *, ModuleDecl *> res =
      lookupTopDecl(parser, name, /*typeLookup=*/true);
  assert(res.is<ValueDecl *>() && "Class Nominal-up should return a Decl");
  ValueDecl *varDecl = res.get<ValueDecl *>();
  if (!varDecl) {
    parser.diagnose(loc, diag::sil_moveonlydeinit_nominal_not_found, name);
    return true;
  }

  auto *theNominalDecl = dyn_cast<NominalTypeDecl>(varDecl);
  if (!theNominalDecl) {
    parser.diagnose(loc, diag::sil_moveonlydeinit_nominal_not_found, name);
    return true;
  }

  SourceLoc lBraceLoc = parser.Tok.getLoc();
  parser.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII tmp(*parser.L);
  SILFunction *func = nullptr;

  if (parser.Tok.isNot(tok::r_brace)) {
    Identifier funcName;
    SourceLoc funcLoc;
    if (parser.Tok.is(tok::kw_nil)) {
      parser.consumeToken();
    } else {
      if (parser.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
          moveOnlyDeinitTableState.parseSILIdentifier(
              funcName, funcLoc, diag::expected_sil_value_name))
        return true;
      func = M.lookUpFunction(funcName.str());
      if (!func) {
        parser.diagnose(funcLoc, diag::sil_moveonlydeinit_func_not_found,
                        funcName);
        return true;
      }
    }
  }

  SourceLoc RBraceLoc;
  parser.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                            lBraceLoc);

  SILMoveOnlyDeinit::create(M, theNominalDecl, Serialized, func);
  return false;
}

static ProtocolDecl *parseProtocolDecl(Parser &P, SILParser &SP) {
  Identifier DeclName;
  SourceLoc DeclLoc;
  if (SP.parseSILIdentifier(DeclName, DeclLoc, diag::expected_sil_value_name))
    return nullptr;

  // Find the protocol decl. The protocol can be imported.
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res =
    lookupTopDecl(P, DeclName, /*typeLookup=*/true);
  assert(Res.is<ValueDecl*>() && "Protocol look-up should return a Decl");
  ValueDecl *VD = Res.get<ValueDecl*>();
  if (!VD) {
    P.diagnose(DeclLoc, diag::sil_witness_protocol_not_found, DeclName);
    return nullptr;
  }
  auto *proto = dyn_cast<ProtocolDecl>(VD);
  if (!proto)
    P.diagnose(DeclLoc, diag::sil_witness_protocol_not_found, DeclName);
  return proto;
}

static AssociatedTypeDecl *parseAssociatedTypeDecl(Parser &P, SILParser &SP,
                                                   ProtocolDecl *proto) {
  Identifier DeclName;
  SourceLoc DeclLoc;
  if (SP.parseSILIdentifier(DeclName, DeclLoc, diag::expected_sil_value_name))
    return nullptr;
  // We can return multiple decls, for now, we use the first lookup result.
  // One example is two decls when searching for Generator of Sequence:
  // one from Sequence, the other from _Sequence_Type.
  SmallVector<ValueDecl *, 4> values;
  auto VD = lookupMember(P, proto->getInterfaceType(), DeclName, DeclLoc,
                         values, true/*ExpectMultipleResults*/);
  if (!VD) {
    P.diagnose(DeclLoc, diag::sil_witness_assoc_not_found, DeclName);
    return nullptr;
  }
  return dyn_cast<AssociatedTypeDecl>(VD);
}

static bool parseAssociatedTypePath(SILParser &SP,
                                    SmallVectorImpl<Identifier> &path) {
  do {
    Identifier name;
    SourceLoc loc;
    if (SP.parseSILIdentifier(name, loc, diag::expected_sil_value_name))
      return false;
    path.push_back(name);
  } while (SP.P.consumeIf(tok::period));

  return true;
}

static bool matchesAssociatedTypePath(CanType assocType,
                                      ArrayRef<Identifier> path) {
  if (auto memberType = dyn_cast<DependentMemberType>(assocType)) {
    return (!path.empty() &&
            memberType->getName() == path.back() &&
            matchesAssociatedTypePath(memberType.getBase(), path.drop_back()));
  } else {
    assert(isa<GenericTypeParamType>(assocType));
    return path.empty();
  }
}

static CanType parseAssociatedTypePath(Parser &P, SILParser &SP,
                                       ProtocolDecl *proto) {
  SourceLoc loc = SP.P.Tok.getLoc();
  SmallVector<Identifier, 4> path;
  if (!parseAssociatedTypePath(SP, path))
    return CanType();

  // This is only used for parsing associated conformances, so we can
  // go ahead and just search the requirement signature for something that
  // matches the path.
  for (auto &reqt : proto->getRequirementSignature().getRequirements()) {
    if (reqt.getKind() != RequirementKind::Conformance)
      continue;
    CanType assocType = reqt.getFirstType()->getCanonicalType();
    if (matchesAssociatedTypePath(assocType, path))
      return assocType;
  }

  SmallString<128> name;
  name += path[0].str();
  for (auto elt : makeArrayRef(path).slice(1)) {
    name += '.';
    name += elt.str();
  }
  P.diagnose(loc, diag::sil_witness_assoc_conf_not_found, name);
  return CanType();
}

static ProtocolConformanceRef
parseRootProtocolConformance(Parser &P, SILParser &SP, Type ConformingTy,
                             ProtocolDecl *&proto) {
  const StringRef ModuleKeyword = "module";
  Identifier ModuleName;
  SourceLoc Loc, KeywordLoc;
  proto = parseProtocolDecl(P, SP);
  if (!proto)
    return ProtocolConformanceRef();

  if (P.parseSpecificIdentifier(
          ModuleKeyword, KeywordLoc,
          Diagnostic(diag::expected_tok_in_sil_instr, ModuleKeyword)) ||
      SP.parseSILIdentifier(ModuleName, Loc, diag::expected_sil_value_name))
    return ProtocolConformanceRef();

  // Calling lookupConformance on a BoundGenericType will return a specialized
  // conformance. We use UnboundGenericType to find the normal conformance.
  Type lookupTy = ConformingTy;
  if (auto bound = lookupTy->getAs<BoundGenericType>())
    lookupTy = bound->getDecl()->getDeclaredType();
  auto lookup = P.SF.getParentModule()->lookupConformance(lookupTy, proto);
  if (lookup.isInvalid()) {
    P.diagnose(KeywordLoc, diag::sil_witness_protocol_conformance_not_found);
    return ProtocolConformanceRef();
  }

  return lookup;
}

///  protocol-conformance ::= normal-protocol-conformance
///  protocol-conformance ::=
///    generic-parameter-list? type: 'inherit' '(' protocol-conformance ')'
///  protocol-conformance ::=
///    generic-parameter-list? type: 'specialize' '<' substitution* '>'
///    '(' protocol-conformance ')'
///  normal-protocol-conformance ::=
///    generic-parameter-list? type: protocolName module ModuleName
/// Note that generic-parameter-list is already parsed before calling this.
ProtocolConformanceRef SILParser::parseProtocolConformance(
    ProtocolDecl *&proto,
    GenericSignature &genericSig,
    GenericParamList *&genericParams) {
  // Make sure we don't leave it uninitialized in the caller
  genericSig = GenericSignature();

  genericParams = P.maybeParseGenericParams().getPtrOrNull();
  if (genericParams) {
    genericSig = handleSILGenericParams(genericParams, &P.SF);
  }

  return parseProtocolConformanceHelper(proto, genericSig, genericParams);
}

ProtocolConformanceRef SILParser::parseProtocolConformanceHelper(
    ProtocolDecl *&proto,
    GenericSignature witnessSig,
    GenericParamList *witnessParams) {
  // Parse AST type.
  ParserResult<TypeRepr> TyR = P.parseType();
  if (TyR.isNull())
    return ProtocolConformanceRef();

  if (!witnessSig)
    witnessSig = ContextGenericSig;
  if (witnessParams == nullptr)
    witnessParams = ContextGenericParams;

  auto ConformingTy = performTypeResolution(TyR.get(), /*IsSILType=*/false,
                                            witnessSig, witnessParams);
  if (witnessSig) {
    ConformingTy = witnessSig.getGenericEnvironment()
        ->mapTypeIntoContext(ConformingTy);
  }

  if (ConformingTy->hasError())
    return ProtocolConformanceRef();

  if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
    return ProtocolConformanceRef();

  if (P.Tok.is(tok::identifier) && P.Tok.getText() == "specialize") {
    P.consumeToken();

    // Parse substitutions for specialized conformance.
    SmallVector<ParsedSubstitution, 4> parsedSubs;
    if (parseSubstitutions(parsedSubs, witnessSig, witnessParams))
      return ProtocolConformanceRef();

    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return ProtocolConformanceRef();
    ProtocolDecl *dummy;
    GenericSignature specializedSig;
    GenericParamList *specializedParams = nullptr;
    auto genericConform =
        parseProtocolConformance(dummy, specializedSig, specializedParams);
    if (genericConform.isInvalid() || !genericConform.isConcrete())
      return ProtocolConformanceRef();
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen))
      return ProtocolConformanceRef();

    SubstitutionMap subMap =
      getApplySubstitutionsFromParsed(*this, specializedSig, parsedSubs);
    if (!subMap)
      return ProtocolConformanceRef();

    auto *rootConform = cast<RootProtocolConformance>(
        genericConform.getConcrete());
    auto result = P.Context.getSpecializedConformance(
        ConformingTy, rootConform, subMap);
    return ProtocolConformanceRef(result);
  }

  if (P.Tok.is(tok::identifier) && P.Tok.getText() == "inherit") {
    P.consumeToken();

    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return ProtocolConformanceRef();
    auto baseConform = parseProtocolConformance();
    if (baseConform.isInvalid() || !baseConform.isConcrete())
      return ProtocolConformanceRef();
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen))
      return ProtocolConformanceRef();

    auto result = P.Context.getInheritedConformance(ConformingTy,
                                                    baseConform.getConcrete());
    return ProtocolConformanceRef(result);
  }

  auto retVal =
    parseRootProtocolConformance(P, *this, ConformingTy, proto);
  return retVal;
}

/// Parser a single SIL vtable entry and add it to either \p witnessEntries
/// or \c conditionalConformances.
static bool parseSILWitnessTableEntry(
         Parser &P,
         SILModule &M,
         ProtocolDecl *proto,
         GenericSignature witnessSig,
         GenericParamList *witnessParams,
         SILParser &witnessState,
         std::vector<SILWitnessTable::Entry> &witnessEntries,
         std::vector<SILWitnessTable::ConditionalConformance>
           &conditionalConformances) {
  Identifier EntryKeyword;
  SourceLoc KeywordLoc;
  if (P.parseIdentifier(EntryKeyword, KeywordLoc,
                        /*diagnoseDollarPrefix=*/false,
                        diag::expected_tok_in_sil_instr,
                        "method, associated_type, associated_type_protocol"
                        ", base_protocol, no_default"))
    return true;

  if (EntryKeyword.str() == "no_default") {
    witnessEntries.push_back(SILDefaultWitnessTable::Entry());
    return false;
  }

  if (EntryKeyword.str() == "base_protocol") {
    ProtocolDecl *proto = parseProtocolDecl(P, witnessState);
    if (!proto)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;
    auto conform =
      witnessState.parseProtocolConformance();
    // Ignore invalid and abstract witness entries.
    if (conform.isInvalid() || !conform.isConcrete())
      return false;

    witnessEntries.push_back(
        SILWitnessTable::BaseProtocolWitness{proto, conform.getConcrete()});
    return false;
  }

  if (EntryKeyword.str() == "associated_type_protocol" ||
      EntryKeyword.str() == "conditional_conformance") {
    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return true;
    CanType assocOrSubject;
    if (EntryKeyword.str() == "associated_type_protocol") {
      assocOrSubject = parseAssociatedTypePath(P, witnessState, proto);
    } else {
      // Parse AST type.
      ParserResult<TypeRepr> TyR = P.parseType();
      if (TyR.isNull())
        return true;

      SILTypeResolutionContext silContext(/*isSILType=*/false,
                                          witnessParams,
                                          /*openedPacks=*/nullptr);
      auto Ty =
          swift::performTypeResolution(TyR.get(), P.Context,
                                       witnessSig, &silContext,
                                       &P.SF);
      if (witnessSig) {
        Ty = witnessSig.getGenericEnvironment()->mapTypeIntoContext(Ty);
      }

      if (Ty->hasError())
        return true;

      assocOrSubject = Ty->getCanonicalType();
    }
    if (!assocOrSubject)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;
    ProtocolDecl *proto = parseProtocolDecl(P, witnessState);
    if (!proto)
      return true;
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen) ||
        P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;

    ProtocolConformanceRef conformance(proto);
    if (P.Tok.getText() != "dependent") {
      auto concrete =
        witnessState.parseProtocolConformance();
      // Ignore invalid and abstract witness entries.
      if (concrete.isInvalid() || !concrete.isConcrete())
        return false;
      conformance = concrete;
    } else {
      P.consumeToken();
    }

    if (EntryKeyword.str() == "associated_type_protocol")
      witnessEntries.push_back(
          SILWitnessTable::AssociatedTypeProtocolWitness{assocOrSubject,
                                                         proto,
                                                         conformance});
    else
      conditionalConformances.push_back(
          SILWitnessTable::ConditionalConformance{assocOrSubject,
                                                  conformance});

    return false;
  }

  if (EntryKeyword.str() == "associated_type") {
    AssociatedTypeDecl *assoc = parseAssociatedTypeDecl(P, witnessState,
                                                        proto);
    if (!assoc)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;

    // Parse AST type.
    ParserResult<TypeRepr> TyR = P.parseType();
    if (TyR.isNull())
      return true;

    SILTypeResolutionContext silContext(/*isSILType=*/false,
                                        witnessParams,
                                        /*openedPacks=*/nullptr);
    auto Ty =
        swift::performTypeResolution(TyR.get(), P.Context,
                                     witnessSig, &silContext,
                                     &P.SF);
    if (witnessSig) {
      Ty = witnessSig.getGenericEnvironment()->mapTypeIntoContext(Ty);
    }

    if (Ty->hasError())
      return true;

    witnessEntries.push_back(
        SILWitnessTable::AssociatedTypeWitness{assoc, Ty->getCanonicalType()});
    return false;
  }

  if (EntryKeyword.str() != "method") {
    P.diagnose(KeywordLoc, diag::expected_tok_in_sil_instr, "method");
    return true;
  }

  SILDeclRef Ref;
  Identifier FuncName;
  SourceLoc FuncLoc;
  if (witnessState.parseSILDeclRef(Ref, true) ||
      P.parseToken(tok::colon, diag::expected_sil_witness_colon))
    return true;

  SILFunction *Func = nullptr;
  if (P.Tok.is(tok::kw_nil)) {
    P.consumeToken();
  } else {
    if (P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
        witnessState.parseSILIdentifier(FuncName, FuncLoc,
                                        diag::expected_sil_value_name))
      return true;

    Func = M.lookUpFunction(FuncName.str());
    if (!Func) {
      P.diagnose(FuncLoc, diag::sil_witness_func_not_found, FuncName);
      return true;
    }
  }
  witnessEntries.push_back(SILWitnessTable::MethodWitness{
    Ref, Func
  });

  return false;
}

/// decl-sil-witness ::= 'sil_witness_table' sil-linkage?
///                      normal-protocol-conformance decl-sil-witness-body
/// normal-protocol-conformance ::=
///   generic-parameter-list? type: protocolName module ModuleName
/// decl-sil-witness-body:
///   '{' sil-witness-entry* '}'
/// sil-witness-entry:
///   method SILDeclRef ':' @SILFunctionName
///   associated_type AssociatedTypeDeclName: Type
///   associated_type_protocol (AssocName: ProtocolName):
///                              protocol-conformance|dependent
///   base_protocol ProtocolName: protocol-conformance
bool SILParserState::parseSILWitnessTable(Parser &P) {
  P.consumeToken(tok::kw_sil_witness_table);
  SILParser WitnessState(P);
  
  // Parse the linkage.
  Optional<SILLinkage> Linkage;
  parseSILLinkage(Linkage, P);
  
  IsSerialized_t isSerialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &isSerialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, WitnessState, M))
    return true;

  // Parse the protocol conformance.
  ProtocolDecl *proto;
  GenericSignature witnessSig;
  GenericParamList *witnessParams = nullptr;
  auto conf = WitnessState.parseProtocolConformance(proto,
                                                    witnessSig,
                                                    witnessParams);
  WitnessState.ContextGenericSig = witnessSig;
  WitnessState.ContextGenericParams = witnessParams;

  // FIXME: should we really allow a specialized or inherited conformance here?
  RootProtocolConformance *theConformance = nullptr;
  if (conf.isConcrete())
    theConformance = conf.getConcrete()->getRootConformance();

  SILWitnessTable *wt = nullptr;
  if (theConformance) {
    wt = M.lookUpWitnessTable(theConformance);
    assert((!wt || wt->isDeclaration()) &&
           "Attempting to create duplicate witness table.");
  }

  // If we don't have an lbrace, then this witness table is a declaration.
  if (P.Tok.getKind() != tok::l_brace) {
    // Default to public external linkage.
    if (!Linkage)
      Linkage = SILLinkage::PublicExternal;
    // We ignore empty witness table without normal protocol conformance.
    if (!wt && theConformance)
      wt = SILWitnessTable::create(M, *Linkage, theConformance);
    return false;
  }

  if (!theConformance) {
    P.diagnose(P.Tok, diag::sil_witness_protocol_conformance_not_found);
    return true;
  }

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);
  // Parse the entry list.
  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;

  if (P.Tok.isNot(tok::r_brace)) {
    do {
      if (parseSILWitnessTableEntry(P, M, proto, witnessSig, witnessParams,
                                    WitnessState, witnessEntries,
                                    conditionalConformances))
        return true;
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);
  
  // Default to public linkage.
  if (!Linkage)
    Linkage = SILLinkage::Public;

  if (!wt)
    wt = SILWitnessTable::create(M, *Linkage, theConformance);
  else
    wt->setLinkage(*Linkage);
  wt->convertToDefinition(witnessEntries, conditionalConformances,
                          isSerialized);
  return false;
}

/// decl-sil-default-witness ::= 'sil_default_witness_table' 
///                              sil-linkage identifier
///                              decl-sil-default-witness-body
/// decl-sil-default-witness-body:
///   '{' sil-default-witness-entry* '}'
/// sil-default-witness-entry:
///   sil-witness-entry
///   'no_default'
bool SILParserState::parseSILDefaultWitnessTable(Parser &P) {
  P.consumeToken(tok::kw_sil_default_witness_table);
  SILParser WitnessState(P);
  
  // Parse the linkage.
  Optional<SILLinkage> Linkage;
  parseSILLinkage(Linkage, P);
  
  // Parse the protocol.
  ProtocolDecl *protocol = parseProtocolDecl(P, WitnessState);
  if (!protocol)
    return true;

  WitnessState.ContextGenericSig = protocol->getGenericSignature();
  WitnessState.ContextGenericParams = protocol->getGenericParams();

  // Parse the body.
  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);

  // Parse the entry list.
  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;

  if (P.Tok.isNot(tok::r_brace)) {
    do {
      if (parseSILWitnessTableEntry(P, M, protocol,
                                    protocol->getGenericSignature(),
                                    protocol->getGenericParams(),
                                    WitnessState, witnessEntries,
                                    conditionalConformances))
        return true;
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);
  
  // Default to public linkage.
  if (!Linkage)
    Linkage = SILLinkage::Public;

  SILDefaultWitnessTable::create(M, *Linkage, protocol, witnessEntries);
  return false;
}

/// decl-sil-differentiability-witness ::=
///   'sil_differentiability_witness'
///   ('[' 'serialized' ']')?
///   sil-linkage?
///   sil-differentiability-witness-config-and-function
///   decl-sil-differentiability-witness-body?
///
/// decl-sil-differentiability-witness-body ::=
///   '{'
///   ('jvp' sil-function-name ':' sil-type)?
///   ('vjp' sil-function-name ':' sil-type)?
///   '}'
///
/// index-subset ::=
///   [0-9]+ (' ' [0-9]+)*
bool SILParserState::parseSILDifferentiabilityWitness(Parser &P) {
  auto loc = P.consumeToken(tok::kw_sil_differentiability_witness);
  auto silLoc = RegularLocation(loc);
  SILParser State(P);

  // Parse the linkage.
  Optional<SILLinkage> linkage;
  if (parseSILLinkage(linkage, P))
    return true;

  // Parse '[serialized]' flag (optional).
  bool isSerialized = false;
  SourceLoc serializedTokLoc;
  if (P.Tok.is(tok::l_square) && P.isIdentifier(P.peekToken(), "serialized")) {
    isSerialized = true;
    serializedTokLoc = P.Tok.getLoc();
    P.consumeToken(tok::l_square);
    P.consumeToken(tok::identifier);
    if (P.parseToken(tok::r_square, diag::sil_diff_witness_expected_token, "]"))
      return true;
  }

  // We need to turn on InSILBody to parse the function references.
  Lexer::SILBodyRAII tmp(*P.L);

  DifferentiabilityKind kind;
  AutoDiffConfig config;
  SILFunction *originalFn;
  if (parseSILDifferentiabilityWitnessConfigAndFunction(
          P, State, silLoc, kind, config, originalFn))
    return true;

  // If this is just a declaration, create the declaration now and return.
  if (!P.Tok.is(tok::l_brace)) {
    if (isSerialized) {
      P.diagnose(serializedTokLoc,
                 diag::sil_diff_witness_serialized_declaration);
      return true;
    }

    SILDifferentiabilityWitness::createDeclaration(
        M, linkage ? *linkage : SILLinkage::DefaultForDeclaration, originalFn,
        kind, config.parameterIndices, config.resultIndices,
        config.derivativeGenericSignature);
    return false;
  }

  // This is a definition, so parse differentiability witness body.
  SILFunction *jvp = nullptr;
  SILFunction *vjp = nullptr;
  if (P.Tok.is(tok::l_brace)) {
    // Parse '{'.
    SourceLoc lBraceLoc;
    P.consumeIf(tok::l_brace, lBraceLoc);
    // Parse JVP (optional).
    if (P.isIdentifier(P.Tok, "jvp")) {
      P.consumeToken(tok::identifier);
      if (P.parseToken(tok::colon, diag::sil_diff_witness_expected_token, ":"))
        return true;
      if (State.parseSILFunctionRef(silLoc, jvp))
        return true;
    }
    // Parse VJP (optional).
    if (P.isIdentifier(P.Tok, "vjp")) {
      P.consumeToken(tok::identifier);
      if (P.parseToken(tok::colon, diag::sil_diff_witness_expected_token, ":"))
        return true;
      if (State.parseSILFunctionRef(silLoc, vjp))
        return true;
    }
    // Parse '}'.
    SourceLoc rBraceLoc;
    if (P.parseMatchingToken(tok::r_brace, rBraceLoc, diag::expected_sil_rbrace,
                             lBraceLoc))
      return true;
  }

  SILDifferentiabilityWitness::createDefinition(
      M, linkage ? *linkage : SILLinkage::DefaultForDefinition, originalFn,
      kind, config.parameterIndices, config.resultIndices,
      config.derivativeGenericSignature, jvp, vjp, isSerialized);
  return false;
}

llvm::Optional<llvm::coverage::Counter> SILParser::parseSILCoverageExpr(
    llvm::coverage::CounterExpressionBuilder &Builder) {
  if (P.Tok.is(tok::integer_literal)) {
    unsigned CounterId;
    if (parseInteger(CounterId, diag::sil_coverage_invalid_counter))
      return None;
    return llvm::coverage::Counter::getCounter(CounterId);
  }

  if (P.Tok.is(tok::identifier)) {
    Identifier Zero;
    SourceLoc Loc;
    if (parseSILIdentifier(Zero, Loc, diag::sil_coverage_invalid_counter))
      return None;
    if (Zero.str() != "zero") {
      P.diagnose(Loc, diag::sil_coverage_invalid_counter);
      return None;
    }
    return llvm::coverage::Counter::getZero();
  }

  if (P.Tok.is(tok::l_paren)) {
    P.consumeToken(tok::l_paren);
    auto LHS = parseSILCoverageExpr(Builder);
    if (!LHS)
      return None;
    const Identifier Operator = P.Context.getIdentifier(P.Tok.getText());
    const SourceLoc Loc = P.consumeToken();
    if (Operator.str() != "+" && Operator.str() != "-") {
      P.diagnose(Loc, diag::sil_coverage_invalid_operator);
      return None;
    }
    auto RHS = parseSILCoverageExpr(Builder);
    if (!RHS)
      return None;
    if (P.parseToken(tok::r_paren, diag::sil_coverage_expected_rparen))
      return None;

    if (Operator.str() == "+")
      return Builder.add(*LHS, *RHS);
    return Builder.subtract(*LHS, *RHS);
  }

  P.diagnose(P.Tok, diag::sil_coverage_invalid_counter);
  return None;
}

/// decl-sil-coverage-map ::= 'sil_coverage_map' CoveredName PGOFuncName CoverageHash
///                           decl-sil-coverage-body
/// decl-sil-coverage-body:
///   '{' sil-coverage-entry* '}'
/// sil-coverage-entry:
///   sil-coverage-loc ':' sil-coverage-expr
/// sil-coverage-loc:
///   StartLine ':' StartCol '->' EndLine ':' EndCol
/// sil-coverage-expr:
///   ...
bool SILParserState::parseSILCoverageMap(Parser &P) {
  P.consumeToken(tok::kw_sil_coverage_map);
  SILParser State(P);

  // Parse the filename.
  Identifier Filename;
  SourceLoc FileLoc;
  if (State.parseSILIdentifier(Filename, FileLoc,
                               diag::expected_sil_value_name))
    return true;

  // Parse the covered name.
  if (!P.Tok.is(tok::string_literal)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_quote);
    return true;
  }
  StringRef FuncName = P.Tok.getText().drop_front().drop_back();
  P.consumeToken();

  // Parse the PGO func name.
  if (!P.Tok.is(tok::string_literal)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_quote);
    return true;
  }
  StringRef PGOFuncName = P.Tok.getText().drop_front().drop_back();
  P.consumeToken();

  uint64_t Hash;
  if (State.parseInteger(Hash, diag::sil_coverage_invalid_hash))
    return true;

  if (!P.Tok.is(tok::l_brace)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_lbrace);
    return true;
  }
  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  llvm::coverage::CounterExpressionBuilder Builder;
  std::vector<SILCoverageMap::MappedRegion> Regions;
  bool BodyHasError = false;
  if (P.Tok.isNot(tok::r_brace)) {
    do {
      unsigned StartLine, StartCol, EndLine, EndCol;
      if (State.parseInteger(StartLine, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::colon, diag::sil_coverage_expected_loc) ||
          State.parseInteger(StartCol, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::arrow, diag::sil_coverage_expected_arrow) ||
          State.parseInteger(EndLine, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::colon, diag::sil_coverage_expected_loc) ||
          State.parseInteger(EndCol, diag::sil_coverage_expected_loc)) {
        BodyHasError = true;
        break;
      }

      if (P.parseToken(tok::colon, diag::sil_coverage_expected_colon)) {
        BodyHasError = true;
        break;
      }

      auto Counter = State.parseSILCoverageExpr(Builder);
      if (!Counter) {
        BodyHasError = true;
        break;
      }

      Regions.emplace_back(StartLine, StartCol, EndLine, EndCol, *Counter);
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }
  if (BodyHasError)
    P.skipUntilDeclRBrace();

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  if (!BodyHasError)
    SILCoverageMap::create(M, &P.SF, Filename.str(), FuncName.str(),
                           PGOFuncName.str(), Hash, Regions,
                           Builder.getExpressions());
  return false;
}

/// sil-scope-ref ::= 'scope' [0-9]+
/// sil-scope ::= 'sil_scope' [0-9]+ '{'
///                 debug-loc
///                 'parent' scope-parent
///                 ('inlined_at' sil-scope-ref)?
///               '}'
/// scope-parent ::= sil-function-name ':' sil-type
/// scope-parent ::= sil-scope-ref
/// debug-loc ::= 'loc' string-literal ':' [0-9]+ ':' [0-9]+
bool SILParserState::parseSILScope(Parser &P) {
  P.consumeToken(tok::kw_sil_scope);
  SILParser ScopeState(P);

  SourceLoc SlotLoc = P.Tok.getLoc();
  unsigned Slot;
  if (ScopeState.parseInteger(Slot, diag::sil_invalid_scope_slot))
    return true;

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  StringRef Key = P.Tok.getText();
  SILLocation Loc = SILLocation::invalid();
  if (Key == "loc")
    if (ScopeState.parseSILLocation(Loc))
      return true;
  ScopeState.parseVerbatim("parent");
  Identifier FnName;
  SILDebugScope *Parent = nullptr;
  SILFunction *ParentFn = nullptr;
  if (P.Tok.is(tok::integer_literal)) {
    /// scope-parent ::= sil-scope-ref
    if (ScopeState.parseScopeRef(Parent))
      return true;
  } else {
    /// scope-parent ::= sil-function-name
    SILType Ty;
    SourceLoc FnLoc = P.Tok.getLoc();
    // We need to turn on InSILBody to parse the function reference.
    Lexer::SILBodyRAII Tmp(*P.L);
    GenericSignature IgnoredSig;
    GenericParamList *IgnoredParams = nullptr;
    if ((ScopeState.parseGlobalName(FnName)) ||
        P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
        ScopeState.parseSILType(Ty, IgnoredSig, IgnoredParams, true))
      return true;

    // The function doesn't exist yet. Create a zombie forward declaration.
    auto FnTy = Ty.getAs<SILFunctionType>();
    if (!FnTy || !Ty.isObject()) {
      P.diagnose(FnLoc, diag::expected_sil_function_type);
      return true;
    }
    ParentFn = ScopeState.getGlobalNameForReference(FnName, FnTy, FnLoc, true);
    ScopeState.TUState.PotentialZombieFns.insert(ParentFn);
  }

  SILDebugScope *InlinedAt = nullptr;
  if (P.Tok.getText() == "inlined_at") {
    P.consumeToken();
    if (ScopeState.parseScopeRef(InlinedAt))
      return true;
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  auto &Scope = ScopeSlots[Slot];
  if (Scope) {
    P.diagnose(SlotLoc, diag::sil_scope_redefined, Slot);
    return true;
  }

  Scope = new (M) SILDebugScope(Loc, ParentFn, Parent, InlinedAt);
  return false;
}
