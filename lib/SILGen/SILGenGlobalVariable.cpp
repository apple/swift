//===--- SILGenGlobalVariable.cpp - Lowering for global variables ---------===//
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

#include "SILGenFunction.h"
#include "ManagedValue.h"
#include "Scope.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/GenericSignature.h"
#include "swift/SIL/FormalLinkage.h"

using namespace swift;
using namespace Lowering;

/// Get or create SILGlobalVariable for a given global VarDecl.
SILGlobalVariable *SILGenModule::getSILGlobalVariable(VarDecl *gDecl,
                                                      ForDefinition_t forDef) {
  // First, get a mangled name for the declaration.
  std::string mangledName;

  {
    auto SILGenName = gDecl->getAttrs().getAttribute<SILGenNameAttr>();
    if (SILGenName && !SILGenName->Name.empty()) {
      mangledName = SILGenName->Name.str();
    } else {
      Mangle::ASTMangler NewMangler;
      mangledName = NewMangler.mangleGlobalVariableFull(gDecl);
    }
  }

  // Get the linkage for SILGlobalVariable.
  FormalLinkage formalLinkage;
  if (gDecl->isResilient())
    formalLinkage = FormalLinkage::Private;
  else
    formalLinkage = getDeclLinkage(gDecl);
  auto silLinkage = getSILLinkage(formalLinkage, forDef);

  // Check if it is already created, and update linkage if necessary.
  if (auto gv = M.lookUpGlobalVariable(mangledName)) {
    // Update the SILLinkage here if this is a definition.
    if (forDef == ForDefinition) {
      gv->setLinkage(silLinkage);
      gv->setDeclaration(false);
    }
    return gv;
  }

  SILType silTy = SILType::getPrimitiveObjectType(
    M.Types.getLoweredTypeOfGlobal(gDecl));

  auto *silGlobal = SILGlobalVariable::create(M, silLinkage, IsNotSerialized,
                                              mangledName, silTy,
                                              None, gDecl);
  silGlobal->setDeclaration(!forDef);

  return silGlobal;
}

ManagedValue
SILGenFunction::emitGlobalVariableRef(SILLocation loc, VarDecl *var) {
  assert(!VarLocs.count(var));

  if (var->isLazilyInitializedGlobal()) {
    // Call the global accessor to get the variable's address.
    SILFunction *accessorFn = SGM.getFunction(
                            SILDeclRef(var, SILDeclRef::Kind::GlobalAccessor),
                                                  NotForDefinition);
    SILValue accessor = B.createFunctionRefFor(loc, accessorFn);
    SILValue addr = B.createApply(loc, accessor, SubstitutionMap(), {});
    // FIXME: It'd be nice if the result of the accessor was natively an
    // address.
    addr = B.createPointerToAddress(
      loc, addr, getLoweredType(var->getInterfaceType()).getAddressType(),
      /*isStrict*/ true, /*isInvariant*/ false);
    return ManagedValue::forLValue(addr);
  }

  // Global variables can be accessed directly with global_addr.  Emit this
  // instruction into the prolog of the function so we can memoize/CSE it in
  // VarLocs.
  auto *entryBB = &*getFunction().begin();
  SILGenBuilder prologueB(*this, entryBB, entryBB->begin());
  prologueB.setTrackingList(B.getTrackingList());

  auto *silG = SGM.getSILGlobalVariable(var, NotForDefinition);
  SILValue addr = prologueB.createGlobalAddr(var, silG);

  VarLocs[var] = SILGenFunction::VarLoc::get(addr);
  return ManagedValue::forLValue(addr);
}

//===----------------------------------------------------------------------===//
// Global initialization
//===----------------------------------------------------------------------===//

namespace {

/// A visitor for traversing a pattern, creating
/// global accessor functions for all of the global variables declared inside.
struct GenGlobalAccessors : public PatternVisitor<GenGlobalAccessors>
{
  /// The module generator.
  SILGenModule &SGM;
  /// The Builtin.once token guarding the global initialization.
  SILGlobalVariable *OnceToken;
  /// The function containing the initialization code.
  SILFunction *OnceFunc;

  /// A reference to the Builtin.once declaration.
  FuncDecl *BuiltinOnceDecl;

  GenGlobalAccessors(SILGenModule &SGM,
                     SILGlobalVariable *OnceToken,
                     SILFunction *OnceFunc)
    : SGM(SGM), OnceToken(OnceToken), OnceFunc(OnceFunc)
  {
    // Find Builtin.once.
    auto &C = SGM.M.getASTContext();
    SmallVector<ValueDecl*, 2> found;
    C.TheBuiltinModule->lookupValue(C.getIdentifier("once"),
                                    NLKind::QualifiedLookup, found);

    assert(found.size() == 1 && "didn't find Builtin.once?!");

    BuiltinOnceDecl = cast<FuncDecl>(found[0]);
  }

  // Walk through non-binding patterns.
  void visitParenPattern(ParenPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitTypedPattern(TypedPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitBindingPattern(BindingPattern *P) {
    return visit(P->getSubPattern());
  }
  void visitTuplePattern(TuplePattern *P) {
    for (auto &elt : P->getElements())
      visit(elt.getPattern());
  }
  void visitAnyPattern(AnyPattern *P) {}

  // When we see a variable binding, emit its global accessor.
  void visitNamedPattern(NamedPattern *P) {
    SGM.emitGlobalAccessor(P->getDecl(), OnceToken, OnceFunc);
  }

#define INVALID_PATTERN(Id, Parent) \
  void visit##Id##Pattern(Id##Pattern *) { \
    llvm_unreachable("pattern not valid in argument or var binding"); \
  }
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) INVALID_PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
#undef INVALID_PATTERN
};

} // end anonymous namespace

/// Emit a global initialization.
void SILGenModule::emitGlobalInitialization(PatternBindingDecl *pd,
                                            unsigned pbdEntry) {
  // Generic and dynamic static properties require lazy initialization, which
  // isn't implemented yet.
  if (pd->isStatic()) {
    assert(!pd->getDeclContext()->isGenericContext()
           || pd->getDeclContext()->getGenericSignatureOfContext()
                ->areAllParamsConcrete());
  }

  Mangle::ASTMangler TokenMangler;
  std::string onceTokenBuffer = TokenMangler.mangleGlobalInit(pd, pbdEntry,
                                                              false);
  
  auto onceTy = BuiltinIntegerType::getWordType(M.getASTContext());
  auto onceSILTy
    = SILType::getPrimitiveObjectType(onceTy->getCanonicalType());

  // TODO: include the module in the onceToken's name mangling.
  // Then we can make it fragile.
  auto onceToken = SILGlobalVariable::create(M, SILLinkage::Private,
                                             IsNotSerialized,
                                             onceTokenBuffer, onceSILTy);
  onceToken->setDeclaration(false);

  // Emit the initialization code into a function.
  Mangle::ASTMangler FuncMangler;
  std::string onceFuncBuffer = FuncMangler.mangleGlobalInit(pd, pbdEntry,
                                                            true);
  
  SILFunction *onceFunc = emitLazyGlobalInitializer(onceFuncBuffer, pd,
                                                    pbdEntry);

  // Generate accessor functions for all of the declared variables, which
  // Builtin.once the lazy global initializer we just generated then return
  // the address of the individual variable.
  GenGlobalAccessors(*this, onceToken, onceFunc)
    .visit(pd->getPattern(pbdEntry));
}

void SILGenFunction::emitLazyGlobalInitializer(PatternBindingDecl *binding,
                                               unsigned pbdEntry) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(binding->getDeclContext());

  ASTContext &C = getASTContext();
  // wasm: Lazy global init is used with swift_once which passes a context
  // pointer parameter. If lazy global init doesn't take a context argument,
  // caller and callee signatures are mismatched and it causes runtime
  // exception on WebAssembly runtime. So we need to add dummy argument
  // to consume the context pointer.
  // See also: emitLazyGlobalInitializer
  if (C.LangOpts.Target.isOSBinFormatWasm()) {
    auto UnsafeRawPointer = C.getUnsafeRawPointerDecl();
    auto UnsafeRawPtrTy = getLoweredType(UnsafeRawPointer->getDeclaredInterfaceType());
    F.front().createFunctionArgument(UnsafeRawPtrTy);
  }

  {
    Scope scope(Cleanups, binding);

    // Emit the initialization sequence.
    emitPatternBinding(binding, pbdEntry);
  }

  // Return void.
  auto ret = emitEmptyTuple(binding);
  B.createReturn(ImplicitReturnLocation(binding), ret);
}

static void emitOnceCall(SILGenFunction &SGF, VarDecl *global,
                         SILGlobalVariable *onceToken, SILFunction *onceFunc) {
  SILType rawPointerSILTy
    = SGF.getLoweredLoadableType(SGF.getASTContext().TheRawPointerType);

  // Emit a reference to the global token.
  SILValue onceTokenAddr = SGF.B.createGlobalAddr(global, onceToken);
  onceTokenAddr = SGF.B.createAddressToPointer(global, onceTokenAddr,
                                               rawPointerSILTy);

  // Emit a reference to the function to execute.
  SILValue onceFuncRef = SGF.B.createFunctionRefFor(global, onceFunc);

  // Call Builtin.once.
  SILValue onceArgs[] = {onceTokenAddr, onceFuncRef};
  SGF.B.createBuiltin(global, SGF.getASTContext().getIdentifier("once"),
                      SGF.SGM.Types.getEmptyTupleType(), {}, onceArgs);
}

void SILGenFunction::emitGlobalAccessor(VarDecl *global,
                                        SILGlobalVariable *onceToken,
                                        SILFunction *onceFunc) {
  emitOnceCall(*this, global, onceToken, onceFunc);

  // Return the address of the global variable.
  // FIXME: It'd be nice to be able to return a SIL address directly.
  auto *silG = SGM.getSILGlobalVariable(global, NotForDefinition);
  SILValue addr = B.createGlobalAddr(global, silG);

  SILType rawPointerSILTy
    = getLoweredLoadableType(getASTContext().TheRawPointerType);
  addr = B.createAddressToPointer(global, addr, rawPointerSILTy);
  auto *ret = B.createReturn(global, addr);
  (void)ret;
  assert(ret->getDebugScope() && "instruction without scope");
}

