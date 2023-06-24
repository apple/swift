//===--- SILGen.cpp - Implements Lowering of ASTs -> SIL ------------------===//
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

#define DEBUG_TYPE "silgen"

#include "ManagedValue.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Evaluator.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ResilienceExpansion.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Statistic.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILProfiler.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
// SILGenModule Class implementation
//===----------------------------------------------------------------------===//

SILGenModule::SILGenModule(SILModule &M, ModuleDecl *SM)
    : M(M), Types(M.Types), SwiftModule(SM), TopLevelSGF(nullptr),
      FileIDsByFilePath(SM->computeFileIDMap(/*shouldDiagnose=*/true)) {
  const SILOptions &Opts = M.getOptions();
  if (!Opts.UseProfile.empty()) {
    // FIXME: Create file system to read the profile. In the future, the vfs
    // needs to come from CompilerInstance.
    auto FS = llvm::vfs::getRealFileSystem();
    auto ReaderOrErr =
        llvm::IndexedInstrProfReader::create(Opts.UseProfile, *FS);
    if (auto E = ReaderOrErr.takeError()) {
      diagnose(SourceLoc(), diag::profile_read_error, Opts.UseProfile,
               llvm::toString(std::move(E)));
    } else {
      M.setPGOReader(std::move(ReaderOrErr.get()));
    }
  }
}

SILGenModule::~SILGenModule() {
  assert(!TopLevelSGF && "active source file lowering!?");

  // Update the linkage of external private functions to public_external,
  // because there is no private_external linkage. External private functions
  // can occur in the following cases:
  //
  // * private class methods which are referenced from the vtable of a derived
  //   class  in a different file/module. Such private methods are always
  //   generated with public linkage in the other file/module.
  //
  // * in lldb: lldb can access private declarations in other files/modules
  //
  // * private functions with a @_silgen_name attribute but without a body
  //
  // * when compiling with -disable-access-control
  //
  for (SILFunction &f : M.getFunctionList()) {
    if (f.getLinkage() == SILLinkage::Private && f.isExternalDeclaration())
      f.setLinkage(SILLinkage::PublicExternal);
  }

  M.verifyIncompleteOSSA();
}

static SILDeclRef
getBridgingFn(Optional<SILDeclRef> &cacheSlot,
              SILGenModule &SGM,
              Identifier moduleName,
              StringRef functionName,
              std::initializer_list<Type> inputTypes,
              Type outputType) {
  if (!cacheSlot) {
    ASTContext &ctx = SGM.M.getASTContext();
    ModuleDecl *mod = ctx.getLoadedModule(moduleName);
    if (!mod) {
      SGM.diagnose(SourceLoc(), diag::bridging_module_missing,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    SmallVector<ValueDecl *, 2> decls;
    mod->lookupValue(ctx.getIdentifier(functionName),
                     NLKind::QualifiedLookup, decls);
    if (decls.empty()) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_missing,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }
    if (decls.size() != 1) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_overloaded,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    auto *fd = dyn_cast<FuncDecl>(decls.front());
    if (!fd) {
      SGM.diagnose(SourceLoc(), diag::bridging_function_not_function,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    // Check that the function takes the expected arguments and returns the
    // expected result type.
    SILDeclRef c(fd);
    auto funcTy =
        SGM.Types.getConstantFunctionType(TypeExpansionContext::minimal(), c);
    SILFunctionConventions fnConv(funcTy, SGM.M);

    auto toSILType = [&SGM](Type ty) {
      return SGM.Types.getLoweredType(ty, TypeExpansionContext::minimal());
    };

    if (fnConv.hasIndirectSILResults() ||
        funcTy->getNumParameters() != inputTypes.size() ||
        !std::equal(
            fnConv.getParameterSILTypes(TypeExpansionContext::minimal())
                .begin(),
            fnConv.getParameterSILTypes(TypeExpansionContext::minimal()).end(),
            makeTransformIterator(inputTypes.begin(), toSILType))) {
      SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    if (fnConv.getSingleSILResultType(TypeExpansionContext::minimal()) !=
        toSILType(outputType)) {
      SGM.diagnose(fd->getLoc(), diag::bridging_function_not_correct_type,
                   moduleName.str(), functionName);
      llvm::report_fatal_error("unable to set up the ObjC bridge!");
    }

    cacheSlot = c;
  }

  LLVM_DEBUG(llvm::dbgs() << "bridging function "
                          << moduleName << '.' << functionName
                          << " mapped to ";
             cacheSlot->print(llvm::dbgs()));

  return *cacheSlot;
}

#define REQUIRED(X) Types.get##X##Type()
#define OPTIONAL(X) OptionalType::get(Types.get##X##Type())
#define EXISTENTIAL(X) getASTContext().get##X##ExistentialType()

#define GET_BRIDGING_FN(Module, FromKind, FromTy, ToKind, ToTy) \
  SILDeclRef SILGenModule::get##FromTy##To##ToTy##Fn() { \
    return getBridgingFn(FromTy##To##ToTy##Fn, *this, \
                         getASTContext().Id_##Module, \
                         "_convert" #FromTy "To" #ToTy, \
                         { FromKind(FromTy) }, \
                         ToKind(ToTy)); \
  }

GET_BRIDGING_FN(Darwin, REQUIRED, Bool, REQUIRED, DarwinBoolean)
GET_BRIDGING_FN(Darwin, REQUIRED, DarwinBoolean, REQUIRED, Bool)
GET_BRIDGING_FN(ObjectiveC, REQUIRED, Bool, REQUIRED, ObjCBool)
GET_BRIDGING_FN(ObjectiveC, REQUIRED, ObjCBool, REQUIRED, Bool)
GET_BRIDGING_FN(Foundation, OPTIONAL, NSError, EXISTENTIAL, Error)
GET_BRIDGING_FN(Foundation, EXISTENTIAL, Error, REQUIRED, NSError)
GET_BRIDGING_FN(WinSDK, REQUIRED, Bool, REQUIRED, WindowsBool)
GET_BRIDGING_FN(WinSDK, REQUIRED, WindowsBool, REQUIRED, Bool)

#undef GET_BRIDGING_FN
#undef REQUIRED
#undef OPTIONAL

static FuncDecl *diagnoseMissingIntrinsic(SILGenModule &sgm,
                                          SILLocation loc,
                                          const char *name) {
  sgm.diagnose(loc, diag::bridging_function_missing,
               sgm.getASTContext().StdlibModuleName.str(), name);
  return nullptr;
}

#define FUNC_DECL(NAME, ID)                             \
  FuncDecl *SILGenModule::get##NAME(SILLocation loc) {  \
    if (auto fn = getASTContext().get##NAME())          \
      return fn;                                        \
    return diagnoseMissingIntrinsic(*this, loc, ID);    \
  }
#include "swift/AST/KnownDecls.def"

#define KNOWN_SDK_FUNC_DECL(MODULE, NAME, ID)                                  \
  FuncDecl *SILGenModule::get##NAME(SILLocation loc) {                         \
    if (ModuleDecl *M = getASTContext().getLoadedModule(                       \
            getASTContext().Id_##MODULE)) {                                    \
      if (auto fn = getASTContext().get##NAME())                               \
        return fn;                                                             \
    }                                                                          \
    return diagnoseMissingIntrinsic(*this, loc, ID);                           \
  }
#include "swift/AST/KnownSDKDecls.def"

ProtocolDecl *SILGenModule::getObjectiveCBridgeable(SILLocation loc) {
  if (ObjectiveCBridgeable)
    return *ObjectiveCBridgeable;

  // Find the _ObjectiveCBridgeable protocol.
  auto &ctx = getASTContext();
  auto proto = ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
  if (!proto)
    diagnose(loc, diag::bridging_objcbridgeable_missing);

  ObjectiveCBridgeable = proto;
  return proto;
}

FuncDecl *SILGenModule::getBridgeToObjectiveCRequirement(SILLocation loc) {
  if (BridgeToObjectiveCRequirement)
    return *BridgeToObjectiveCRequirement;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    BridgeToObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  DeclName name(ctx, ctx.Id_bridgeToObjectiveC, llvm::ArrayRef<Identifier>());
  auto *found = dyn_cast_or_null<FuncDecl>(
    proto->getSingleRequirement(name));

  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, name);

  BridgeToObjectiveCRequirement = found;
  return found;
}

FuncDecl *SILGenModule::getUnconditionallyBridgeFromObjectiveCRequirement(
    SILLocation loc) {
  if (UnconditionallyBridgeFromObjectiveCRequirement)
    return *UnconditionallyBridgeFromObjectiveCRequirement;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    UnconditionallyBridgeFromObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  DeclName name(ctx, ctx.getIdentifier("_unconditionallyBridgeFromObjectiveC"),
                llvm::makeArrayRef(Identifier()));
  auto *found = dyn_cast_or_null<FuncDecl>(
    proto->getSingleRequirement(name));

  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, name);

  UnconditionallyBridgeFromObjectiveCRequirement = found;
  return found;
}

AssociatedTypeDecl *
SILGenModule::getBridgedObjectiveCTypeRequirement(SILLocation loc) {
  if (BridgedObjectiveCType)
    return *BridgedObjectiveCType;

  // Find the _ObjectiveCBridgeable protocol.
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) {
    BridgeToObjectiveCRequirement = nullptr;
    return nullptr;
  }

  // Look for _bridgeToObjectiveC().
  auto &ctx = getASTContext();
  auto *found = proto->getAssociatedType(ctx.Id_ObjectiveCType);
  if (!found)
    diagnose(loc, diag::bridging_objcbridgeable_broken, ctx.Id_ObjectiveCType);

  BridgedObjectiveCType = found;
  return found;
}

ProtocolConformance *
SILGenModule::getConformanceToObjectiveCBridgeable(SILLocation loc, Type type) {
  auto proto = getObjectiveCBridgeable(loc);
  if (!proto) return nullptr;

  // Find the conformance to _ObjectiveCBridgeable.
  auto result = SwiftModule->lookupConformance(type, proto);
  if (result.isInvalid())
    return nullptr;

  return result.getConcrete();
}

ProtocolDecl *SILGenModule::getBridgedStoredNSError(SILLocation loc) {
  if (BridgedStoredNSError)
    return *BridgedStoredNSError;

  // Find the _BridgedStoredNSError protocol.
  auto &ctx = getASTContext();
  auto proto = ctx.getProtocol(KnownProtocolKind::BridgedStoredNSError);
  BridgedStoredNSError = proto;
  return proto;
}

VarDecl *SILGenModule::getNSErrorRequirement(SILLocation loc) {
  if (NSErrorRequirement)
    return *NSErrorRequirement;

  // Find the _BridgedStoredNSError protocol.
  auto proto = getBridgedStoredNSError(loc);
  if (!proto) {
    NSErrorRequirement = nullptr;
    return nullptr;
  }

  // Look for _nsError.
  auto &ctx = getASTContext();
  auto *found = dyn_cast_or_null<VarDecl>(
      proto->getSingleRequirement(ctx.Id_nsError));

  NSErrorRequirement = found;
  return found;
}

ProtocolConformanceRef
SILGenModule::getConformanceToBridgedStoredNSError(SILLocation loc, Type type) {
  auto proto = getBridgedStoredNSError(loc);
  if (!proto)
    return ProtocolConformanceRef::forInvalid();

  // Find the conformance to _BridgedStoredNSError.
  return SwiftModule->lookupConformance(type, proto);
}

static FuncDecl *lookupIntrinsic(ModuleDecl &module,
                                 Optional<FuncDecl *> &cache, Identifier name) {
  if (cache)
    return *cache;

  SmallVector<ValueDecl *, 1> decls;
  module.lookupQualified(&module, DeclNameRef(name), SourceLoc(),
                         NL_QualifiedDefault | NL_IncludeUsableFromInline,
                         decls);
  if (decls.size() != 1) {
    cache = nullptr;
    return nullptr;
  }
  auto func = dyn_cast<FuncDecl>(decls[0]);
  cache = func;
  return func;
}

static FuncDecl *lookupConcurrencyIntrinsic(ASTContext &C,
                                            Optional<FuncDecl *> &cache,
                                            StringRef name) {
  auto *module = C.getLoadedModule(C.Id_Concurrency);
  if (!module) {
    cache = nullptr;
    return nullptr;
  }

  return lookupIntrinsic(*module, cache, C.getIdentifier(name));
}

FuncDecl *
SILGenModule::getAsyncLetStart() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    AsyncLetStart,
                                    "_asyncLetStart");
}

FuncDecl *
SILGenModule::getAsyncLetGet() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    AsyncLetGet,
                                    "_asyncLet_get");
}

FuncDecl *
SILGenModule::getAsyncLetGetThrowing() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    AsyncLetGetThrowing,
                                    "_asyncLet_get_throwing");
}

FuncDecl *
SILGenModule::getFinishAsyncLet() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    EndAsyncLet,
                                    "_asyncLet_finish");
}

FuncDecl *
SILGenModule::getTaskFutureGet() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    TaskFutureGet,
                                    "_taskFutureGet");
}

FuncDecl *
SILGenModule::getTaskFutureGetThrowing() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    TaskFutureGetThrowing,
                                    "_taskFutureGetThrowing");
}

FuncDecl *
SILGenModule::getResumeUnsafeContinuation() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    ResumeUnsafeContinuation,
                                    "_resumeUnsafeContinuation");
}
FuncDecl *
SILGenModule::getResumeUnsafeThrowingContinuation() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    ResumeUnsafeThrowingContinuation,
                                    "_resumeUnsafeThrowingContinuation");
}
FuncDecl *
SILGenModule::getResumeUnsafeThrowingContinuationWithError() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    ResumeUnsafeThrowingContinuationWithError,
                                  "_resumeUnsafeThrowingContinuationWithError");
}
FuncDecl *
SILGenModule::getRunTaskForBridgedAsyncMethod() {
  return lookupConcurrencyIntrinsic(getASTContext(),
                                    RunTaskForBridgedAsyncMethod,
                                    "_runTaskForBridgedAsyncMethod");
}
FuncDecl *
SILGenModule::getCheckExpectedExecutor() {
  return lookupConcurrencyIntrinsic(getASTContext(), CheckExpectedExecutor,
                                    "_checkExpectedExecutor");
}

FuncDecl *SILGenModule::getAsyncMainDrainQueue() {
  return lookupConcurrencyIntrinsic(getASTContext(), AsyncMainDrainQueue,
                                    "_asyncMainDrainQueue");
}

FuncDecl *SILGenModule::getGetMainExecutor() {
  return lookupConcurrencyIntrinsic(getASTContext(), GetMainExecutor,
                                    "_getMainExecutor");
}

FuncDecl *SILGenModule::getSwiftJobRun() {
  return lookupConcurrencyIntrinsic(getASTContext(), SwiftJobRun,
                                    "_swiftJobRun");
}

FuncDecl *SILGenModule::getExit() {
  if (ExitFunc)
    return *ExitFunc;

  ASTContext &C = getASTContext();
  ModuleDecl *concurrencyShims =
      C.getModuleByIdentifier(C.getIdentifier("_SwiftConcurrencyShims"));

  if (!concurrencyShims) {
    ExitFunc = nullptr;
    return nullptr;
  }

  return lookupIntrinsic(*concurrencyShims, ExitFunc, C.getIdentifier("exit"));
}

ProtocolConformance *SILGenModule::getNSErrorConformanceToError() {
  if (NSErrorConformanceToError)
    return *NSErrorConformanceToError;

  auto &ctx = getASTContext();
  auto nsErrorTy = ctx.getNSErrorType();
  if (!nsErrorTy) {
    NSErrorConformanceToError = nullptr;
    return nullptr;
  }

  auto error = ctx.getErrorDecl();
  if (!error) {
    NSErrorConformanceToError = nullptr;
    return nullptr;
  }

  auto conformance =
    SwiftModule->lookupConformance(nsErrorTy, cast<ProtocolDecl>(error));

  if (conformance.isConcrete())
    NSErrorConformanceToError = conformance.getConcrete();
  else
    NSErrorConformanceToError = nullptr;
  return *NSErrorConformanceToError;
}

SILFunction *
SILGenModule::getKeyPathProjectionCoroutine(bool isReadAccess,
                                            KeyPathTypeKind typeKind) {
  bool isBaseInout;
  bool isResultInout;
  StringRef functionName;
  NominalTypeDecl *keyPathDecl;
  if (isReadAccess) {
    assert(typeKind == KPTK_KeyPath ||
           typeKind == KPTK_WritableKeyPath ||
           typeKind == KPTK_ReferenceWritableKeyPath);
    functionName = "swift_readAtKeyPath";
    isBaseInout = false;
    isResultInout = false;
    keyPathDecl = getASTContext().getKeyPathDecl();
  } else if (typeKind == KPTK_WritableKeyPath) {
    functionName = "swift_modifyAtWritableKeyPath";
    isBaseInout = true;
    isResultInout = true;
    keyPathDecl = getASTContext().getWritableKeyPathDecl();
  } else if (typeKind == KPTK_ReferenceWritableKeyPath) {
    functionName = "swift_modifyAtReferenceWritableKeyPath";
    isBaseInout = false;
    isResultInout = true;
    keyPathDecl = getASTContext().getReferenceWritableKeyPathDecl();
  } else {
    llvm_unreachable("bad combination");
  }

  auto fn = M.lookUpFunction(functionName);
  if (fn) return fn;

  auto rootType =
      CanGenericTypeParamType::get(/*isParameterPack*/ false,
                                   /*depth*/ 0, /*index*/ 0, getASTContext());
  auto valueType =
      CanGenericTypeParamType::get(/*isParameterPack*/ false,
                                   /*depth*/ 0, /*index*/ 1, getASTContext());

  // Build the generic signature <A, B>.
  auto sig = GenericSignature::get({rootType, valueType}, {});

  auto keyPathTy = BoundGenericType::get(keyPathDecl, Type(),
                                         { rootType, valueType })
    ->getCanonicalType();

  // (@in_guaranteed/@inout Root, @guaranteed KeyPath<Root, Value>)
  SILParameterInfo params[] = {
    { rootType,
      isBaseInout ? ParameterConvention::Indirect_Inout
                  : ParameterConvention::Indirect_In_Guaranteed },
    { keyPathTy, ParameterConvention::Direct_Guaranteed },
  };

  // -> @yields @in_guaranteed/@inout Value
  SILYieldInfo yields[] = {
    { valueType,
      isResultInout ? ParameterConvention::Indirect_Inout
                    : ParameterConvention::Indirect_In_Guaranteed },
  };

  auto extInfo = SILFunctionType::ExtInfo::getThin();

  auto functionTy = SILFunctionType::get(sig, extInfo,
                                         SILCoroutineKind::YieldOnce,
                                         ParameterConvention::Direct_Unowned,
                                         params,
                                         yields,
                                         /*results*/ {},
                                         /*error result*/ {},
                                         SubstitutionMap(),
                                         SubstitutionMap(),
                                         getASTContext());

  auto env = sig.getGenericEnvironment();

  SILGenFunctionBuilder builder(*this);
  fn = builder.createFunction(SILLinkage::PublicExternal,
                              functionName,
                              functionTy,
                              env,
                              /*location*/ None,
                              IsNotBare,
                              IsNotTransparent,
                              IsNotSerialized,
                              IsNotDynamic,
                              IsNotDistributed,
                              IsNotRuntimeAccessible);

  return fn;
}

SILFunction *SILGenModule::getEmittedFunction(SILDeclRef constant,
                                              ForDefinition_t forDefinition) {
  auto found = emittedFunctions.find(constant);
  if (found != emittedFunctions.end()) {
    SILFunction *F = found->second;
    if (forDefinition) {
      // In all the cases where getConstantLinkage returns something
      // different for ForDefinition, it returns an available-externally
      // linkage.
      if (isAvailableExternally(F->getLinkage())) {
        F->setLinkage(constant.getLinkage(ForDefinition));
      }
    }
    return F;
  }

  return nullptr;
}

static SILFunction *getFunctionToInsertAfter(SILGenModule &SGM,
                                             SILDeclRef insertAfter) {
  // If the decl ref was emitted, emit after its function.
  while (insertAfter) {
    auto found = SGM.emittedFunctions.find(insertAfter);
    if (found != SGM.emittedFunctions.end()) {
      return found->second;
    }

    // Otherwise, try to insert after the function we would be transitively
    // be inserted after.
    auto foundDelayed = SGM.delayedFunctions.find(insertAfter);
    if (foundDelayed != SGM.delayedFunctions.end()) {
      insertAfter = foundDelayed->second;
    } else {
      break;
    }
  }

  // If the decl ref is nil, just insert at the beginning.
  return nullptr;
}

static bool isEmittedOnDemand(SILModule &M, SILDeclRef constant) {
  if (!constant.hasDecl())
    return false;

  if (constant.isForeign)
    return false;

  auto *d = constant.getDecl();
  auto *dc = d->getDeclContext();

  switch (constant.kind) {
  case SILDeclRef::Kind::Func: {
    auto *fd = cast<FuncDecl>(d);
    if (!fd->hasBody())
      return false;

    if (isa<ClangModuleUnit>(dc->getModuleScopeContext()))
      return true;

    if (fd->hasForcedStaticDispatch())
      return true;

    break;
  }
  case SILDeclRef::Kind::Allocator: {
    auto *cd = cast<ConstructorDecl>(d);
    // For factories, we don't need to emit a special thunk; the normal
    // foreign-to-native thunk is sufficient.
    if (isa<ClangModuleUnit>(dc->getModuleScopeContext()) &&
        !cd->isFactoryInit() &&
        (dc->getSelfClassDecl() ||
         cd->hasBody()))
      return true;

    break;
  }
  case SILDeclRef::Kind::EnumElement:
    return true;
  default:
    break;
  }

  return false;
}

SILFunction *SILGenModule::getFunction(SILDeclRef constant,
                                       ForDefinition_t forDefinition) {
  // If we already emitted the function, return it.
  if (auto emitted = getEmittedFunction(constant, forDefinition))
    return emitted;

  auto getBestLocation = [](SILDeclRef ref) -> SILLocation {
    if (ref.hasDecl())
      return ref.getDecl();
    if (ref.loc.isNull())
      return {(Decl *)nullptr};
    if (auto *ace = ref.getAbstractClosureExpr())
      return {ace};
    return {(Decl *)nullptr};
  };

  // Note: Do not provide any SILLocation. You can set it afterwards.
  SILGenFunctionBuilder builder(*this);
  auto &IGM = *this;
  auto *F = builder.getOrCreateFunction(
      getBestLocation(constant), constant, forDefinition,
      [&IGM](SILLocation loc, SILDeclRef constant) -> SILFunction * {
        return IGM.getFunction(constant, NotForDefinition);
      });

  assert(F && "SILFunction should have been defined");

  emittedFunctions[constant] = F;

  auto foundDelayed = delayedFunctions.find(constant);
  if (foundDelayed == delayedFunctions.end()) {
    if (isEmittedOnDemand(M, constant)) {
      if (forcedFunctions.insert(constant).second)
        pendingForcedFunctions.push_back(constant);
      return F;
    }
  }

  // If we delayed emitting this function previously, we need it now.
  if (foundDelayed != delayedFunctions.end()) {
    // Move the function to its proper place within the module.
    M.functions.remove(F);
    SILFunction *insertAfter = getFunctionToInsertAfter(*this,
                                              foundDelayed->second);
    if (!insertAfter) {
      M.functions.push_front(F);
    } else {
      M.functions.insertAfter(insertAfter->getIterator(), F);
    }

    if (forcedFunctions.insert(constant).second)
      pendingForcedFunctions.push_back(constant);
    delayedFunctions.erase(foundDelayed);
  } else {
    // We would have registered a delayed function as "last emitted" when we
    // enqueued. If the function wasn't delayed, then we're emitting it now.
    lastEmittedFunction = constant;
  }

  return F;
}

bool SILGenModule::hasFunction(SILDeclRef constant) {
  return emittedFunctions.count(constant);
}

void SILGenModule::visit(Decl *D) {
  if (Lowering::shouldSkipLowering(D))
    return;

  ASTVisitor::visit(D);
}

void SILGenModule::visitFuncDecl(FuncDecl *fd) { emitFunction(fd); }

void SILGenModule::emitFunctionDefinition(SILDeclRef constant, SILFunction *f) {
  if (!f->empty()) {
    diagnose(constant.getAsRegularLocation(), diag::sil_function_redefinition,
             f->getName());
    return;
  }

  if (constant.isForeignToNativeThunk()) {
    f->setThunk(IsThunk);
    if (constant.asForeign().isClangGenerated())
      f->setSerialized(IsSerialized);

    auto loc = constant.getAsRegularLocation();
    loc.markAutoGenerated();
    auto *dc = loc.getAsDeclContext();
    assert(dc);

    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitForeignToNativeThunk", f);
    SILGenFunction(*this, *f, dc).emitForeignToNativeThunk(constant);
    postEmitFunction(constant, f);
    return;
  }

  if (constant.isNativeToForeignThunk()) {
    auto loc = constant.getAsRegularLocation();
    loc.markAutoGenerated();
    auto *dc = loc.getAsDeclContext();
    assert(dc);

    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitNativeToForeignThunk", f);
    f->setBare(IsBare);
    f->setThunk(IsThunk);
    // If the native function is async, then the foreign entry point is not,
    // so it needs to spawn a detached task in which to run the native
    // implementation, so the actual thunk logic needs to go into a closure
    // implementation function.
    if (constant.hasAsync()) {
      f = SILGenFunction(*this, *f, dc).emitNativeAsyncToForeignThunk(constant);
    }

    SILGenFunction(*this, *f, dc).emitNativeToForeignThunk(constant);

    postEmitFunction(constant, f);
    return;
  }

  if (constant.isDistributedThunk()) {
    auto loc = constant.getAsRegularLocation();
    loc.markAutoGenerated();
    auto *dc = loc.getAsDeclContext();
    assert(dc);

    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitDistributedThunk", f);
    f->setBare(IsBare);
    f->setThunk(IsThunk);
    f->setIsDistributed();

    assert(constant.isDistributedThunk());
    SILGenFunction(*this, *f, constant.getFuncDecl())
        .emitFunction(constant.getFuncDecl());

    postEmitFunction(constant, f);
    return;
  }

  if (constant.isBackDeploymentThunk()) {
    auto loc = constant.getAsRegularLocation();
    loc.markAutoGenerated();
    auto *dc = loc.getAsDeclContext();
    assert(dc);

    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitBackDeploymentThunk", f);
    f->setBare(IsBare);
    f->setThunk(IsThunk);

    SILGenFunction(*this, *f, dc).emitBackDeploymentThunk(constant);

    postEmitFunction(constant, f);
    return;
  }

  switch (constant.kind) {
  case SILDeclRef::Kind::Func: {
    if (auto *ce = constant.getAbstractClosureExpr()) {
      preEmitFunction(constant, f, ce);
      PrettyStackTraceSILFunction X("silgen closureexpr", f);
      f->createProfiler(constant);
      SILGenFunction(*this, *f, ce).emitClosure(ce);
      postEmitFunction(constant, f);
      break;
    }

    if (constant.isInitAccessor()) {
      auto *accessor = cast<AccessorDecl>(constant.getDecl());
      preEmitFunction(constant, f, accessor);
      PrettyStackTraceSILFunction X("silgen init accessor", f);
      f->createProfiler(constant);
      SILGenFunction(*this, *f, accessor).emitInitAccessor(accessor);
      postEmitFunction(constant, f);
      break;
    }

    auto *fd = cast<FuncDecl>(constant.getDecl());

    preEmitFunction(constant, f, fd);
    PrettyStackTraceSILFunction X("silgen emitFunction", f);
    f->createProfiler(constant);
    SILGenFunction(*this, *f, fd).emitFunction(fd);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::Allocator: {
    auto *decl = cast<ConstructorDecl>(constant.getDecl());

    if (decl->getDeclContext()->getSelfClassDecl() &&
        (decl->isDesignatedInit() ||
         decl->isObjC())) {
      preEmitFunction(constant, f, decl);
      PrettyStackTraceSILFunction X("silgen emitClassConstructorAllocator", f);
      SILGenFunction(*this, *f, decl).emitClassConstructorAllocator(decl);
      postEmitFunction(constant, f);
    } else {
      preEmitFunction(constant, f, decl);
      PrettyStackTraceSILFunction X("silgen emitValueConstructor", f);
      f->createProfiler(constant);
      SILGenFunction(*this, *f, decl).emitValueConstructor(decl);
      postEmitFunction(constant, f);
    }
    break;
  }

  case SILDeclRef::Kind::Initializer: {
    auto *decl = cast<ConstructorDecl>(constant.getDecl());
    assert(decl->getDeclContext()->getSelfClassDecl());

    preEmitFunction(constant, f, decl);
    PrettyStackTraceSILFunction X("silgen constructor initializer", f);
    f->createProfiler(constant);
    SILGenFunction(*this, *f, decl).emitClassConstructorInitializer(decl);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::DefaultArgGenerator: {
    auto *decl = constant.getDecl();
    auto *param = getParameterAt(decl, constant.defaultArgIndex);
    assert(param);

    auto *initDC = param->getDefaultArgumentInitContext();

    switch (param->getDefaultArgumentKind()) {
    case DefaultArgumentKind::Normal: {
      auto arg = param->getTypeCheckedDefaultExpr();
      auto loc = RegularLocation::getAutoGeneratedLocation(arg);
      preEmitFunction(constant, f, loc);
      PrettyStackTraceSILFunction X("silgen emitDefaultArgGenerator ", f);
      SILGenFunction SGF(*this, *f, initDC);
      SGF.emitGeneratorFunction(constant, arg);
      postEmitFunction(constant, f);
      break;
    }

    case DefaultArgumentKind::StoredProperty: {
      auto arg = param->getStoredProperty();
      auto loc = RegularLocation::getAutoGeneratedLocation(arg);
      preEmitFunction(constant, f, loc);
      PrettyStackTraceSILFunction X("silgen emitDefaultArgGenerator ", f);
      SILGenFunction SGF(*this, *f, initDC);
      SGF.emitGeneratorFunction(constant, arg);
      postEmitFunction(constant, f);
      break;
    }

    default:
      llvm_unreachable("Bad default argument kind");
    }

    break;
  }

  case SILDeclRef::Kind::StoredPropertyInitializer: {
    auto *var = cast<VarDecl>(constant.getDecl());

    auto *pbd = var->getParentPatternBinding();
    unsigned idx = pbd->getPatternEntryIndexForVarDecl(var);
    auto *initDC = pbd->getInitContext(idx);
    auto captureInfo = pbd->getCaptureInfo(idx);
    auto *init = constant.getInitializationExpr();
    assert(init);

    auto loc = RegularLocation::getAutoGeneratedLocation(init);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitStoredPropertyInitialization", f);
    f->createProfiler(constant);
    SILGenFunction SGF(*this, *f, initDC);

    // If this is a stored property initializer inside a type at global scope,
    // it may close over a global variable. If we're emitting top-level code,
    // then emit a "mark_function_escape" that lists the captured global
    // variables so that definite initialization can reason about this
    // escape point.
    if (!var->getDeclContext()->isLocalContext() &&  TopLevelSGF &&
        TopLevelSGF->B.hasValidInsertionPoint()) {
      emitMarkFunctionEscapeForTopLevelCodeGlobals(var, captureInfo);
    }

    SGF.emitGeneratorFunction(constant, init, /*EmitProfilerIncrement=*/true);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::PropertyWrapperBackingInitializer: {
    auto *var = cast<VarDecl>(constant.getDecl());

    auto loc = RegularLocation::getAutoGeneratedLocation(var);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X(
        "silgen emitPropertyWrapperBackingInitializer", f);

    auto *init = constant.getInitializationExpr();
    assert(init);

    f->createProfiler(constant);
    auto varDC = var->getInnermostDeclContext();
    SILGenFunction SGF(*this, *f, varDC);
    SGF.emitGeneratorFunction(constant, init, /*EmitProfilerIncrement*/ true);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue: {
    auto *var = cast<VarDecl>(constant.getDecl());

    auto loc = RegularLocation::getAutoGeneratedLocation(var);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X(
        "silgen emitPropertyWrapperInitFromProjectedValue", f);

    auto *init = constant.getInitializationExpr();
    assert(init);

    auto varDC = var->getInnermostDeclContext();
    SILGenFunction SGF(*this, *f, varDC);
    SGF.emitGeneratorFunction(constant, init);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::GlobalAccessor: {
    auto *global = cast<VarDecl>(constant.getDecl());
    auto found = delayedGlobals.find(global);
    assert(found != delayedGlobals.end());

    auto *onceToken = found->second.first;
    auto *onceFunc = found->second.second;

    auto loc = RegularLocation::getAutoGeneratedLocation(global);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitGlobalAccessor", f);
    SILGenFunction(*this, *f, global->getDeclContext())
      .emitGlobalAccessor(global, onceToken, onceFunc);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::EnumElement: {
    auto *decl = cast<EnumElementDecl>(constant.getDecl());

    auto loc = RegularLocation::getAutoGeneratedLocation(decl);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen enum constructor", f);
    SILGenFunction(*this, *f, decl->getDeclContext()).emitEnumConstructor(decl);
    postEmitFunction(constant, f);
    break;
  }

  case SILDeclRef::Kind::Destroyer: {
    auto *dd = cast<DestructorDecl>(constant.getDecl());
    preEmitFunction(constant, f, dd);
    PrettyStackTraceSILFunction X("silgen emitDestroyingDestructor", f);
    f->createProfiler(constant);
    SILGenFunction(*this, *f, dd).emitDestroyingDestructor(dd);
    postEmitFunction(constant, f);
    return;
  }

  case SILDeclRef::Kind::Deallocator: {
    auto *dd = cast<DestructorDecl>(constant.getDecl());
    auto *nom = dd->getDeclContext()->getSelfNominalTypeDecl();

    if (auto *cd = dyn_cast<ClassDecl>(nom)) {
      if (usesObjCAllocator(cd)) {
        preEmitFunction(constant, f, dd);
        PrettyStackTraceSILFunction X("silgen emitDestructor -dealloc", f);
        f->createProfiler(constant);
        SILGenFunction(*this, *f, dd).emitObjCDestructor(constant);
        postEmitFunction(constant, f);
        return;
      }
    }

    auto loc = RegularLocation::getAutoGeneratedLocation(dd);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitDeallocatingDestructor", f);
    SILGenFunction(*this, *f, dd).emitDeallocatingDestructor(dd);
    postEmitFunction(constant, f);
    return;
  }

  case SILDeclRef::Kind::IVarInitializer: {
    auto *cd = cast<ClassDecl>(constant.getDecl());
    auto loc = RegularLocation::getAutoGeneratedLocation(cd);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitDestructor ivar initializer", f);
    SILGenFunction(*this, *f, cd).emitIVarInitializer(constant);
    postEmitFunction(constant, f);
    return;
  }

  case SILDeclRef::Kind::IVarDestroyer: {
    auto *cd = cast<ClassDecl>(constant.getDecl());
    auto loc = RegularLocation::getAutoGeneratedLocation(cd);
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitDestructor ivar destroyer", f);
    SILGenFunction(*this, *f, cd).emitIVarDestroyer(constant);
    postEmitFunction(constant, f);
    return;
  }
  case SILDeclRef::Kind::AsyncEntryPoint:
  case SILDeclRef::Kind::EntryPoint: {
    f->setBare(IsBare);

    // TODO: Handle main SourceFile emission (currently done by
    // SourceFileScope).
    auto loc = constant.getAsRegularLocation();
    preEmitFunction(constant, f, loc);
    auto *decl = constant.getDecl();
    auto *dc = decl->getDeclContext();
    PrettyStackTraceSILFunction X("silgen emitArtificialTopLevel", f);
    // In all cases, a constant.kind == EntryPoint indicates the main entrypoint
    // to the program, @main.
    // In the synchronous case, the decl is not async, so emitArtificialTopLevel
    // emits the error unwrapping and call to MainType.$main() into @main.
    //
    // In the async case, emitAsyncMainThreadStart is responsible for generating
    // the contents of @main. This wraps @async_main in a task, passes that task
    // to swift_job_run to execute the first thunk, and starts the runloop to
    // run any additional continuations. The kind is EntryPoint, and the decl is
    // async.
    // When the kind is 'AsyncMain', we are generating @async_main. In this
    // case, emitArtificialTopLevel emits the code for calling MaintType.$main,
    // unwrapping errors, and calling exit(0) into @async_main to run the
    // user-specified main function.
    if (constant.kind == SILDeclRef::Kind::EntryPoint && isa<FuncDecl>(decl) &&
        static_cast<FuncDecl *>(decl)->hasAsync()) {
      SILDeclRef mainEntryPoint = SILDeclRef::getAsyncMainDeclEntryPoint(decl);
      SILGenFunction(*this, *f, dc).emitAsyncMainThreadStart(mainEntryPoint);
    } else {
      SILGenFunction(*this, *f, dc).emitArtificialTopLevel(decl);
    }
    postEmitFunction(constant, f);
    return;
  }

  case SILDeclRef::Kind::RuntimeAttributeGenerator: {
    auto *decl = constant.getDecl();
    auto *attr = constant.pointer.get<CustomAttr *>();
    auto *DC = decl->getDeclContext()->getParentSourceFile();

    auto generator = decl->getRuntimeDiscoverableAttributeGenerator(attr);
    auto loc = RegularLocation::getAutoGeneratedLocation();
    preEmitFunction(constant, f, loc);
    PrettyStackTraceSILFunction X("silgen emitRuntimeAttributeGenerator ", f);
    SILGenFunction SGF(*this, *f, DC);
    SGF.emitGeneratorFunction(constant, generator.second, generator.first,
                              AbstractionPattern::getOpaqueFunction());
    postEmitFunction(constant, f);
    break;
  }
  }
}

/// Emit a function now, if it's externally usable or has been referenced in
/// the current TU, or remember how to emit it later if not.
static void emitOrDelayFunction(SILGenModule &SGM, SILDeclRef constant) {
  assert(!constant.isThunk());
  assert(!constant.isClangImported());

  auto emitAfter = SGM.lastEmittedFunction;

  // Implicit decls may be delayed if they can't be used externally.
  auto linkage = constant.getLinkage(ForDefinition);
  bool mayDelay = !constant.hasUserWrittenCode() &&
                  !constant.isDynamicallyReplaceable() &&
                  !isPossiblyUsedExternally(linkage, SGM.M.isWholeModule());

  if (!mayDelay) {
    SGM.emitFunctionDefinition(constant, SGM.getFunction(constant, ForDefinition));
    return;
  }

  // If the function is already forced then it was previously delayed and then
  // referenced. We don't need to emit or delay it again.
  if (SGM.forcedFunctions.contains(constant))
    return;

  if (auto *f = SGM.getEmittedFunction(constant, ForDefinition)) {
    SGM.emitFunctionDefinition(constant, f);
    return;
  }

  // This is a delayable function so remember how to emit it in case it gets
  // referenced later.
  SGM.delayedFunctions.insert({constant, emitAfter});
  // Even though we didn't emit the function now, update the
  // lastEmittedFunction so that we preserve the original ordering that
  // the symbols would have been emitted in.
  SGM.lastEmittedFunction = constant;
}

void SILGenModule::preEmitFunction(SILDeclRef constant, SILFunction *F,
                                   SILLocation Loc) {
  assert(F->empty() && "already emitted function?!");

  if (F->getLoweredFunctionType()->isPolymorphic())
    F->setGenericEnvironment(Types.getConstantGenericEnvironment(constant));

  // Create a debug scope for the function using astNode as source location.
  F->setDebugScope(new (M) SILDebugScope(Loc, F));

  LLVM_DEBUG(llvm::dbgs() << "lowering ";
             F->printName(llvm::dbgs());
             llvm::dbgs() << " : ";
             F->getLoweredType().print(llvm::dbgs());
             llvm::dbgs() << '\n';
             if (auto *decl = Loc.getAsASTNode<ValueDecl>()) {
               decl->dump(llvm::dbgs());
               llvm::dbgs() << '\n';
             } else if (auto *expr = Loc.getAsASTNode<Expr>()) {
               expr->dump(llvm::dbgs());
               llvm::dbgs() << "\n";
             });
}

void SILGenModule::postEmitFunction(SILDeclRef constant,
                                    SILFunction *F) {
  emitLazyConformancesForFunction(F);

  assert(!F->isExternalDeclaration() && "did not emit any function body?!");
  LLVM_DEBUG(llvm::dbgs() << "lowered sil:\n";
             F->print(llvm::dbgs()));
  F->verifyIncompleteOSSA();

  emitDifferentiabilityWitnessesForFunction(constant, F);
}

void SILGenModule::emitDifferentiabilityWitnessesForFunction(
    SILDeclRef constant, SILFunction *F) {
  // Visit `@derivative` attributes and generate SIL differentiability
  // witnesses.
  // Skip if the SILDeclRef is a:
  // - Default argument generator function.
  // - Thunk.
  if (!constant.hasDecl() || !constant.getAbstractFunctionDecl())
    return;
  if (constant.kind == SILDeclRef::Kind::DefaultArgGenerator ||
      constant.isThunk())
    return;
  auto *AFD = constant.getAbstractFunctionDecl();
  auto emitWitnesses = [&](DeclAttributes &Attrs) {
    for (auto *diffAttr : Attrs.getAttributes<DifferentiableAttr>()) {
      auto *resultIndices = IndexSubset::get(getASTContext(), 1, {0});
      assert((!F->getLoweredFunctionType()->getSubstGenericSignature() ||
              diffAttr->getDerivativeGenericSignature()) &&
             "Type-checking should resolve derivative generic signatures for "
             "all original SIL functions with generic signatures");
      auto witnessGenSig =
          autodiff::getDifferentiabilityWitnessGenericSignature(
              AFD->getGenericSignature(),
              diffAttr->getDerivativeGenericSignature());
      AutoDiffConfig config(diffAttr->getParameterIndices(), resultIndices,
                            witnessGenSig);
      emitDifferentiabilityWitness(AFD, F, DifferentiabilityKind::Reverse,
                                   config, /*jvp*/ nullptr,
                                   /*vjp*/ nullptr, diffAttr);
    }
    for (auto *derivAttr : Attrs.getAttributes<DerivativeAttr>()) {
      SILFunction *jvp = nullptr;
      SILFunction *vjp = nullptr;
      switch (derivAttr->getDerivativeKind()) {
      case AutoDiffDerivativeFunctionKind::JVP:
        jvp = F;
        break;
      case AutoDiffDerivativeFunctionKind::VJP:
        vjp = F;
        break;
      }
      auto *origAFD = derivAttr->getOriginalFunction(getASTContext());
      auto origDeclRef =
          SILDeclRef(origAFD).asForeign(requiresForeignEntryPoint(origAFD));
      auto *origFn = getFunction(origDeclRef, NotForDefinition);
      auto witnessGenSig =
          autodiff::getDifferentiabilityWitnessGenericSignature(
              origAFD->getGenericSignature(), AFD->getGenericSignature());
      auto *resultIndices = IndexSubset::get(getASTContext(), 1, {0});
      AutoDiffConfig config(derivAttr->getParameterIndices(), resultIndices,
                            witnessGenSig);
      emitDifferentiabilityWitness(origAFD, origFn,
                                   DifferentiabilityKind::Reverse, config, jvp,
                                   vjp, derivAttr);
    }
  };
  if (auto *accessor = dyn_cast<AccessorDecl>(AFD))
    if (accessor->isGetter())
      emitWitnesses(accessor->getStorage()->getAttrs());
  emitWitnesses(AFD->getAttrs());
}

void SILGenModule::emitDifferentiabilityWitness(
    AbstractFunctionDecl *originalAFD, SILFunction *originalFunction,
    DifferentiabilityKind diffKind, const AutoDiffConfig &config,
    SILFunction *jvp, SILFunction *vjp, const DeclAttribute *attr) {
  assert(isa<DifferentiableAttr>(attr) || isa<DerivativeAttr>(attr));
  auto *origFnType = originalAFD->getInterfaceType()->castTo<AnyFunctionType>();
  auto origSilFnType = originalFunction->getLoweredFunctionType();
  auto *silParamIndices =
      autodiff::getLoweredParameterIndices(config.parameterIndices, origFnType);
  // NOTE(TF-893): Extending capacity is necessary when `origSilFnType` has
  // parameters corresponding to captured variables. These parameters do not
  // appear in the type of `origFnType`.
  // TODO: If possible, change `autodiff::getLoweredParameterIndices` to
  // take `CaptureInfo` into account.
  if (origSilFnType->getNumParameters() > silParamIndices->getCapacity())
    silParamIndices = silParamIndices->extendingCapacity(
        getASTContext(), origSilFnType->getNumParameters());

  // Get or create new SIL differentiability witness.
  // Witness already exists when there are two `@derivative` attributes
  // (registering JVP and VJP functions) for the same derivative function
  // configuration.
  // Witness JVP and VJP are set below.
  AutoDiffConfig silConfig(silParamIndices, config.resultIndices,
                           config.derivativeGenericSignature);
  SILDifferentiabilityWitnessKey key = {
      originalFunction->getName(), diffKind, silConfig};
  auto *diffWitness = M.lookUpDifferentiabilityWitness(key);
  if (!diffWitness) {
    // Differentiability witnesses have the same linkage as the original
    // function, stripping external.
    auto linkage = stripExternalFromLinkage(originalFunction->getLinkage());
    diffWitness = SILDifferentiabilityWitness::createDefinition(
        M, linkage, originalFunction, diffKind, silConfig.parameterIndices,
        silConfig.resultIndices, config.derivativeGenericSignature,
        /*jvp*/ nullptr, /*vjp*/ nullptr,
        /*isSerialized*/ hasPublicVisibility(originalFunction->getLinkage()),
        attr);
  }

  // Set derivative function in differentiability witness.
  auto setDerivativeInDifferentiabilityWitness =
      [&](AutoDiffDerivativeFunctionKind kind, SILFunction *derivative) {
        auto derivativeThunk = getOrCreateCustomDerivativeThunk(
            originalAFD, originalFunction, derivative, silConfig, kind);
        // Check for existing same derivative.
        // TODO(TF-835): Remove condition below and simplify assertion to
        // `!diffWitness->getDerivative(kind)` after `@derivative` attribute
        // type-checking no longer generates implicit `@differentiable`
        // attributes.
        auto *existingDerivative = diffWitness->getDerivative(kind);
        if (existingDerivative && existingDerivative == derivativeThunk)
          return;
        assert(!existingDerivative &&
               "SIL differentiability witness already has a different existing "
               "derivative");
        diffWitness->setDerivative(kind, derivativeThunk);
      };
  if (jvp)
    setDerivativeInDifferentiabilityWitness(AutoDiffDerivativeFunctionKind::JVP,
                                            jvp);
  if (vjp)
    setDerivativeInDifferentiabilityWitness(AutoDiffDerivativeFunctionKind::VJP,
                                            vjp);
}

void SILGenModule::
emitMarkFunctionEscapeForTopLevelCodeGlobals(SILLocation loc,
                                             CaptureInfo captureInfo) {
  assert(TopLevelSGF && TopLevelSGF->B.hasValidInsertionPoint()
         && "no valid code generator for top-level function?!");

  SmallVector<SILValue, 4> Captures;
  
  for (auto capture : captureInfo.getCaptures()) {
    // Decls captured by value don't escape.
    auto It = TopLevelSGF->VarLocs.find(capture.getDecl());
    if (It == TopLevelSGF->VarLocs.end() ||
        !It->getSecond().value->getType().isAddress())
      continue;
    
    Captures.push_back(It->second.value);
  }
  
  if (!Captures.empty())
    TopLevelSGF->B.createMarkFunctionEscape(loc, Captures);
}

void SILGenModule::emitAbstractFuncDecl(AbstractFunctionDecl *AFD) {
  // Emit default arguments and property wrapper initializers.
  emitArgumentGenerators(AFD, AFD->getParameters());

  // If this is a function at global scope, it may close over a global variable.
  // If we're emitting top-level code, then emit a "mark_function_escape" that
  // lists the captured global variables so that definite initialization can
  // reason about this escape point.
  if (!AFD->getDeclContext()->isLocalContext() &&
      TopLevelSGF && TopLevelSGF->B.hasValidInsertionPoint()) {
    emitMarkFunctionEscapeForTopLevelCodeGlobals(AFD, AFD->getCaptureInfo());
  }
  
  // If the declaration is exported as a C function, emit its native-to-foreign
  // thunk too, if it wasn't already forced.
  if (AFD->getAttrs().hasAttribute<CDeclAttr>()) {
    auto thunk = SILDeclRef(AFD).asForeign();
    if (!hasFunction(thunk))
      emitNativeToForeignThunk(thunk);
  }

  if (auto thunkDecl = AFD->getDistributedThunk()) {
    auto thunk = SILDeclRef(thunkDecl).asDistributed();
    emitFunctionDefinition(SILDeclRef(thunkDecl).asDistributed(),
                           getFunction(thunk, ForDefinition));
  }

  if (AFD->isBackDeployed(M.getASTContext())) {
    // Emit the fallback function that will be used when the original function
    // is unavailable at runtime.
    auto fallback = SILDeclRef(AFD).asBackDeploymentKind(
        SILDeclRef::BackDeploymentKind::Fallback);
    emitFunctionDefinition(fallback, getFunction(fallback, ForDefinition));

    // Emit the thunk that either invokes the original function or the fallback
    // function depending on the availability of the original.
    auto thunk = SILDeclRef(AFD).asBackDeploymentKind(
        SILDeclRef::BackDeploymentKind::Thunk);
    emitBackDeploymentThunk(thunk);
  }
}

void SILGenModule::emitFunction(FuncDecl *fd) {
  Types.setCaptureTypeExpansionContext(SILDeclRef(fd), M);

  SILDeclRef::Loc decl = fd;

  emitAbstractFuncDecl(fd);

  if (fd->hasBody())
    emitOrDelayFunction(*this, SILDeclRef(decl));
}

void SILGenModule::addGlobalVariable(VarDecl *global) {
  // We create SILGlobalVariable here.
  getSILGlobalVariable(global, ForDefinition);
}

void SILGenModule::emitConstructor(ConstructorDecl *decl) {
  // FIXME: Handle 'self' like any other argument here.
  // Emit any default argument getter functions.
  emitAbstractFuncDecl(decl);

  // We never emit constructors in protocols.
  if (isa<ProtocolDecl>(decl->getDeclContext()))
    return;

  SILDeclRef constant(decl);
  DeclContext *declCtx = decl->getDeclContext();

  if (declCtx->getSelfClassDecl()) {
    // Designated initializers for classes, as well as @objc convenience
    // initializers, have separate entry points for allocation and
    // initialization.
    if (decl->isDesignatedInit() || decl->isObjC()) {
      emitOrDelayFunction(*this, constant);

      if (decl->hasBody()) {
        SILDeclRef initConstant(decl, SILDeclRef::Kind::Initializer);
        emitOrDelayFunction(*this, initConstant);
      }

      return;
    }
  }

  // Struct and enum constructors do everything in a single function, as do
  // non-@objc convenience initializers for classes.
  if (decl->hasBody()) {
    emitOrDelayFunction(*this, constant);
  }
}

SILFunction *SILGenModule::emitClosure(AbstractClosureExpr *ce) {
  SILDeclRef constant(ce);
  SILFunction *f = getFunction(constant, ForDefinition);

  // Generate the closure function, if we haven't already.
  //
  // We may visit the same closure expr multiple times in some cases,
  // for instance, when closures appear as in-line initializers of stored
  // properties. In these cases the closure will be emitted into every
  // initializer of the containing type.
  if (!f->isExternalDeclaration())
    return f;

  // Emit property wrapper argument generators.
  emitArgumentGenerators(ce, ce->getParameters());

  emitFunctionDefinition(constant, f);
  return f;
}

/// Determine whether the given class requires a separate instance
/// variable initialization method.
static bool requiresIVarInitialization(SILGenModule &SGM, ClassDecl *cd) {
  if (!cd->requiresStoredPropertyInits())
    return false;

  for (Decl *member : cd->getImplementationContext()->getAllMembers()) {
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd) continue;

    for (auto i : range(pbd->getNumPatternEntries()))
      if (pbd->getExecutableInit(i))
        return true;
  }

  return false;
}

bool SILGenModule::hasNonTrivialIVars(ClassDecl *cd) {
  for (Decl *member : cd->getImplementationContext()->getAllMembers()) {
    auto *vd = dyn_cast<VarDecl>(member);
    if (!vd || !vd->hasStorage()) continue;

    auto &ti = Types.getTypeLowering(
        vd->getType(), TypeExpansionContext::maximalResilienceExpansionOnly());
    if (!ti.isTrivial())
      return true;
  }

  return false;
}

bool SILGenModule::requiresIVarDestroyer(ClassDecl *cd) {
  // Only needed if we have non-trivial ivars, we're not a root class, and
  // the superclass is not @objc.
  return (hasNonTrivialIVars(cd) &&
          cd->getSuperclassDecl() &&
          !cd->getSuperclassDecl()->hasClangNode());
}

/// TODO: This needs a better name.
void SILGenModule::emitObjCAllocatorDestructor(ClassDecl *cd,
                                               DestructorDecl *dd) {
  // Emit the native deallocating destructor for -dealloc.
  // Destructors are a necessary part of class metadata, so can't be delayed.
  if (dd->hasBody()) {
    SILDeclRef dealloc(dd, SILDeclRef::Kind::Deallocator);
    emitFunctionDefinition(dealloc, getFunction(dealloc, ForDefinition));
  }

  // Emit the Objective-C -dealloc entry point if it has
  // something to do beyond messaging the superclass's -dealloc.
  if (dd->hasBody() && !dd->getBody()->empty())
    emitObjCDestructorThunk(dd);

  // Emit the ivar initializer, if needed.
  if (requiresIVarInitialization(*this, cd)) {
    auto ivarInitializer = SILDeclRef(cd, SILDeclRef::Kind::IVarInitializer)
      .asForeign();
    emitFunctionDefinition(ivarInitializer,
                           getFunction(ivarInitializer, ForDefinition));
  }

  // Emit the ivar destroyer, if needed.
  if (hasNonTrivialIVars(cd)) {
    auto ivarDestroyer = SILDeclRef(cd, SILDeclRef::Kind::IVarDestroyer)
      .asForeign();
    emitFunctionDefinition(ivarDestroyer,
                           getFunction(ivarDestroyer, ForDefinition));
  }
}

void SILGenModule::emitDestructor(ClassDecl *cd, DestructorDecl *dd) {
  emitAbstractFuncDecl(dd);
  
  // Emit the ivar destroyer, if needed.
  if (requiresIVarDestroyer(cd)) {
    SILDeclRef ivarDestroyer(cd, SILDeclRef::Kind::IVarDestroyer);
    emitFunctionDefinition(ivarDestroyer,
                           getFunction(ivarDestroyer, ForDefinition));
  }

  // If the class would use the Objective-C allocator, only emit -dealloc.
  if (usesObjCAllocator(cd)) {
    emitObjCAllocatorDestructor(cd, dd);
    return;
  }

  // Emit the destroying destructor.
  // Destructors are a necessary part of class metadata, so can't be delayed.
  if (dd->hasBody()) {
    SILDeclRef destroyer(dd, SILDeclRef::Kind::Destroyer);
    emitFunctionDefinition(destroyer, getFunction(destroyer, ForDefinition));
  }

  // Emit the deallocating destructor.
  {
    SILDeclRef deallocator(dd, SILDeclRef::Kind::Deallocator);
    emitFunctionDefinition(deallocator,
                           getFunction(deallocator, ForDefinition));
  }
}

void SILGenModule::emitMoveOnlyDestructor(NominalTypeDecl *cd,
                                          DestructorDecl *dd) {
  assert(cd->isMoveOnly());

  emitAbstractFuncDecl(dd);

  // Emit the deallocating destructor if we have a body.
  if (dd->hasBody()) {
    SILDeclRef deallocator(dd, SILDeclRef::Kind::Deallocator);
    emitFunctionDefinition(deallocator,
                           getFunction(deallocator, ForDefinition));
  }
}

void SILGenModule::emitDefaultArgGenerator(SILDeclRef constant,
                                           ParamDecl *param) {
  switch (param->getDefaultArgumentKind()) {
  case DefaultArgumentKind::None:
    llvm_unreachable("No default argument here?");

  case DefaultArgumentKind::Normal:
  case DefaultArgumentKind::StoredProperty:
    emitOrDelayFunction(*this, constant);
    break;

  case DefaultArgumentKind::Inherited:
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
  case DefaultArgumentKind::NAME:
#include "swift/AST/MagicIdentifierKinds.def"
  case DefaultArgumentKind::NilLiteral:
  case DefaultArgumentKind::EmptyArray:
  case DefaultArgumentKind::EmptyDictionary:
    break;
  }
}

void SILGenModule::
emitStoredPropertyInitialization(PatternBindingDecl *pbd, unsigned i) {
  auto *var = pbd->getAnchoringVarDecl(i);
  SILDeclRef constant(var, SILDeclRef::Kind::StoredPropertyInitializer);
  emitOrDelayFunction(*this, constant);
}

void SILGenModule::
emitPropertyWrapperBackingInitializer(VarDecl *var) {
  auto initInfo = var->getPropertyWrapperInitializerInfo();

  if (initInfo.hasInitFromWrappedValue()) {
    SILDeclRef constant(var, SILDeclRef::Kind::PropertyWrapperBackingInitializer);
    emitOrDelayFunction(*this, constant);
  }

  if (initInfo.hasInitFromProjectedValue()) {
    SILDeclRef constant(var, SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue);
    emitOrDelayFunction(*this, constant);
  }
}

SILFunction *SILGenModule::emitLazyGlobalInitializer(StringRef funcName,
                                                 PatternBindingDecl *binding,
                                                     unsigned pbdEntry) {
  ASTContext &C = M.getASTContext();
  auto *onceBuiltin =
      cast<FuncDecl>(getBuiltinValueDecl(C, C.getIdentifier("once")));
  auto blockParam = onceBuiltin->getParameters()->get(1);
  auto *initType = blockParam->getType()->castTo<FunctionType>();
  auto initSILType = cast<SILFunctionType>(
      Types.getLoweredRValueType(TypeExpansionContext::minimal(), initType));

  SILGenFunctionBuilder builder(*this);
  auto *f = builder.createFunction(
      SILLinkage::Private, funcName, initSILType, nullptr, SILLocation(binding),
      IsNotBare, IsNotTransparent, IsNotSerialized, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible);
  f->setSpecialPurpose(SILFunction::Purpose::GlobalInitOnceFunction);
  f->setDebugScope(new (M) SILDebugScope(RegularLocation(binding), f));
  auto dc = binding->getDeclContext();
  SILGenFunction(*this, *f, dc).emitLazyGlobalInitializer(binding, pbdEntry);
  emitLazyConformancesForFunction(f);
  f->verifyIncompleteOSSA();

  return f;
}

void SILGenModule::emitGlobalAccessor(VarDecl *global,
                                      SILGlobalVariable *onceToken,
                                      SILFunction *onceFunc) {
  SILDeclRef accessor(global, SILDeclRef::Kind::GlobalAccessor);
  delayedGlobals[global] = std::make_pair(onceToken, onceFunc);
  emitOrDelayFunction(*this, accessor);
}

void SILGenModule::emitArgumentGenerators(SILDeclRef::Loc decl,
                                          ParameterList *paramList) {
  unsigned index = 0;
  for (auto param : *paramList) {
    if (param->isDefaultArgument())
      emitDefaultArgGenerator(SILDeclRef::getDefaultArgGenerator(decl, index),
                              param);

    if (param->hasExternalPropertyWrapper())
      emitPropertyWrapperBackingInitializer(param);

    ++index;
  }
}

void SILGenModule::emitObjCMethodThunk(FuncDecl *method) {
  auto thunk = SILDeclRef(method).asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;

  // ObjC entry points are always externally usable, so can't be delay-emitted.
  emitNativeToForeignThunk(thunk);
}

void SILGenModule::emitObjCPropertyMethodThunks(AbstractStorageDecl *prop) {
  auto *getter = prop->getOpaqueAccessor(AccessorKind::Get);

  // If we don't actually need an entry point for the getter, do nothing.
  if (!getter || !requiresObjCMethodEntryPoint(getter))
    return;

  auto getterRef = SILDeclRef(getter, SILDeclRef::Kind::Func).asForeign();

  // Don't emit the thunks if they already exist.
  if (hasFunction(getterRef))
    return;

  // ObjC entry points are always externally usable, so emitting can't be
  // delayed.
  emitNativeToForeignThunk(getterRef);

  if (!prop->isSettable(prop->getDeclContext()))
    return;

  // FIXME: Add proper location.
  auto *setter = prop->getOpaqueAccessor(AccessorKind::Set);
  auto setterRef = SILDeclRef(setter, SILDeclRef::Kind::Func).asForeign();
  emitNativeToForeignThunk(setterRef);
}

void SILGenModule::emitObjCConstructorThunk(ConstructorDecl *constructor) {
  auto thunk = SILDeclRef(constructor, SILDeclRef::Kind::Initializer)
    .asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;
  // ObjC entry points are always externally usable, so emitting can't be
  // delayed.
  emitNativeToForeignThunk(thunk);
}

void SILGenModule::emitObjCDestructorThunk(DestructorDecl *destructor) {
  auto thunk = SILDeclRef(destructor, SILDeclRef::Kind::Deallocator)
    .asForeign();

  // Don't emit the thunk if it already exists.
  if (hasFunction(thunk))
    return;

  emitNativeToForeignThunk(thunk);
}

void SILGenModule::visitPatternBindingDecl(PatternBindingDecl *pd) {
  assert(!TopLevelSGF && "script mode PBDs should be in TopLevelCodeDecls");
  for (auto i : range(pd->getNumPatternEntries()))
    if (pd->getExecutableInit(i))
      emitGlobalInitialization(pd, i);
}

void SILGenModule::visitVarDecl(VarDecl *vd) {
  if (vd->hasStorage())
    addGlobalVariable(vd);

  vd->visitEmittedAccessors([&](AccessorDecl *accessor) {
    emitFunction(accessor);
  });

  tryEmitPropertyDescriptor(vd);
}

void SILGenModule::visitSubscriptDecl(SubscriptDecl *sd) {
  llvm_unreachable("top-level subscript?");
}

void SILGenModule::visitMissingDecl(MissingDecl *sd) {
  llvm_unreachable("missing decl in SILGen");
}

void SILGenModule::visitMacroDecl(MacroDecl *d) {
  // nothing to emit for macros
}

void SILGenModule::visitMacroExpansionDecl(MacroExpansionDecl *d) {
  // Expansion already visited as auxiliary decls.
}

bool
SILGenModule::canStorageUseStoredKeyPathComponent(AbstractStorageDecl *decl,
                                                  ResilienceExpansion expansion) {
  // If the declaration is resilient, we have to treat the component as
  // computed.
  if (decl->isResilient(M.getSwiftModule(), expansion))
    return false;

  auto strategy = decl->getAccessStrategy(AccessSemantics::Ordinary,
                                          decl->supportsMutation()
                                            ? AccessKind::ReadWrite
                                            : AccessKind::Read,
                                          M.getSwiftModule(),
                                          expansion);
  switch (strategy.getKind()) {
  case AccessStrategy::Storage: {
    // Keypaths rely on accessors to handle the special behavior of weak or
    // unowned properties.
    if (decl->getInterfaceType()->is<ReferenceStorageType>())
      return false;

    // If the field offset depends on the generic instantiation, we have to
    // load it from metadata when instantiating the keypath component.
    //
    // However the metadata offset itself will not be fixed if the superclass
    // is resilient. Fall back to treating the property as computed in this
    // case.
    //
    // See the call to getClassFieldOffsetOffset() inside
    // emitKeyPathComponent().
    if (auto *parentClass = dyn_cast<ClassDecl>(decl->getDeclContext())) {
      if (parentClass->isGeneric()) {
        auto ancestry = parentClass->checkAncestry();
        if (ancestry.contains(AncestryFlags::ResilientOther))
          return false;
      }
    }

    // If the stored value would need to be reabstracted in fully opaque
    // context, then we have to treat the component as computed.
    auto componentObjTy = decl->getValueInterfaceType();
    if (auto genericEnv =
              decl->getInnermostDeclContext()->getGenericEnvironmentOfContext())
      componentObjTy = genericEnv->mapTypeIntoContext(componentObjTy);
    auto storageTy = M.Types.getSubstitutedStorageType(
        TypeExpansionContext::minimal(), decl, componentObjTy);
    auto opaqueTy = M.Types.getLoweredRValueType(
        TypeExpansionContext::noOpaqueTypeArchetypesSubstitution(expansion),
        AbstractionPattern::getOpaque(), componentObjTy);

    return storageTy.getASTType() == opaqueTy;
  }
  case AccessStrategy::DirectToAccessor:
  case AccessStrategy::DispatchToAccessor:
  case AccessStrategy::MaterializeToTemporary:
  case AccessStrategy::DispatchToDistributedThunk:
    return false;
  }
  llvm_unreachable("unhandled strategy");
}

static bool canStorageUseTrivialDescriptor(SILGenModule &SGM,
                                           AbstractStorageDecl *decl) {
  // A property can use a trivial property descriptor if the key path component
  // that an external module would form given publicly-exported information
  // about the property is never equivalent to the canonical component for the
  // key path.
  // This means that the property isn't stored (without promising to be always
  // stored) and doesn't have a setter with less-than-public visibility.
  auto expansion = ResilienceExpansion::Maximal;

  if (!SGM.M.getSwiftModule()->isResilient()) {
    if (SGM.canStorageUseStoredKeyPathComponent(decl, expansion)) {
      // External modules can't directly access storage, unless this is a
      // property in a fixed-layout type.
      return !decl->isFormallyResilient();
    }

    // If the type is computed and doesn't have a setter that's hidden from
    // the public, then external components can form the canonical key path
    // without our help.
    auto *setter = decl->getOpaqueAccessor(AccessorKind::Set);
    if (!setter)
      return true;

    if (setter->getFormalAccessScope(nullptr, true).isPublic())
      return true;

    return false;
  }

  // A resilient module needs to handle binaries compiled against its older
  // versions. This means we have to be a bit more conservative, since in
  // earlier versions, a settable property may have withheld the setter,
  // or a fixed-layout type may not have been.
  // Without availability information, only get-only computed properties
  // can resiliently use trivial descriptors.
  return (!SGM.canStorageUseStoredKeyPathComponent(decl, expansion) &&
          !decl->supportsMutation());
}

void SILGenModule::tryEmitPropertyDescriptor(AbstractStorageDecl *decl) {
  // TODO: Key path code emission doesn't handle opaque values properly yet.
  if (!SILModuleConventions(M).useLoweredAddresses())
    return;
  
  if (!decl->exportsPropertyDescriptor())
    return;

  PrettyStackTraceDecl stackTrace("emitting property descriptor for", decl);

  Type baseTy;
  if (decl->getDeclContext()->isTypeContext()) {
    // TODO: Static properties should eventually be referenceable as
    // keypaths from T.Type -> Element, viz `baseTy = MetatypeType::get(baseTy)`
    assert(!decl->isStatic());
    
    baseTy = decl->getDeclContext()->getSelfInterfaceType()
                 ->getReducedType(decl->getInnermostDeclContext()
                                      ->getGenericSignatureOfContext());
  } else {
    // TODO: Global variables should eventually be referenceable as
    // key paths from (), viz. baseTy = TupleType::getEmpty(getASTContext());
    llvm_unreachable("should not export a property descriptor yet");
  }

  auto genericEnv = decl->getInnermostDeclContext()
                        ->getGenericEnvironmentOfContext();
  unsigned baseOperand = 0;
  bool needsGenericContext = true;
  
  if (canStorageUseTrivialDescriptor(*this, decl)) {
    (void)SILProperty::create(M, /*serialized*/ false, decl, None);
    return;
  }
  
  SubstitutionMap subs;
  if (genericEnv)
    subs = genericEnv->getForwardingSubstitutionMap();
  
  auto component = emitKeyPathComponentForDecl(SILLocation(decl),
                                               genericEnv,
                                               ResilienceExpansion::Maximal,
                                               baseOperand, needsGenericContext,
                                               subs, decl, {},
                                               baseTy->getCanonicalType(),
                                               M.getSwiftModule(),
                                               /*property descriptor*/ true);
  
  (void)SILProperty::create(M, /*serialized*/ false, decl, component);
}

void SILGenModule::visitIfConfigDecl(IfConfigDecl *ICD) {
  // Nothing to do for these kinds of decls - anything active has been added
  // to the enclosing declaration.
}

void SILGenModule::visitPoundDiagnosticDecl(PoundDiagnosticDecl *PDD) {
  // Nothing to do for #error/#warning; they've already been emitted.
}

void SILGenModule::visitTopLevelCodeDecl(TopLevelCodeDecl *td) {
  assert(TopLevelSGF && "top-level code in a non-main source file!");

  if (!TopLevelSGF->B.hasValidInsertionPoint())
    return;

  TopLevelSGF->emitProfilerIncrement(td->getBody());

  DebugScope DS(*TopLevelSGF, CleanupLocation(td));

  for (auto &ESD : td->getBody()->getElements()) {
    if (!TopLevelSGF->B.hasValidInsertionPoint()) {
      if (auto *S = ESD.dyn_cast<Stmt*>()) {
        if (S->isImplicit())
          continue;
      } else if (auto *E = ESD.dyn_cast<Expr*>()) {
        if (E->isImplicit())
          continue;
      }

      diagnose(ESD.getStartLoc(), diag::unreachable_code);
      // There's no point in trying to emit anything else.
      return;
    }

    if (auto *S = ESD.dyn_cast<Stmt*>()) {
      TopLevelSGF->emitStmt(S);
    } else if (auto *E = ESD.dyn_cast<Expr*>()) {
      TopLevelSGF->emitIgnoredExpr(E);
    } else {
      TopLevelSGF->visit(ESD.get<Decl*>());
    }
  }
}

namespace {

/// An RAII class to scope source file codegen.
class SourceFileScope {
  SILGenModule &sgm;
  SILDeclRef EntryRef;
  Optional<Scope> scope;
  bool isAsyncTopLevel = false;
public:
  SourceFileScope(SILGenModule &sgm, SourceFile *sf) : sgm(sgm) {
    // If this is the script-mode file for the module, create a toplevel.
    // TODO: We need to unify emission of the entry point such that we walk
    // all of the TopLevelCodeDecls in one shot. This will be needed in order
    // to requestify entry point emission.
    if (sf->isScriptMode()) {
      assert(!sgm.TopLevelSGF && "already emitted toplevel?!");
      assert(!sgm.M.lookUpFunction(
                 sgm.getASTContext().getEntryPointFunctionName()) &&
             "already emitted toplevel?!");

      auto mainEntryRef = SILDeclRef::getMainFileEntryPoint(sf);
      SILFunction * toplevel = sgm.getFunction(mainEntryRef, ForDefinition);
      toplevel->setBare(IsBare);
      EntryRef = mainEntryRef;

      if (sf->isAsyncContext()) {
        isAsyncTopLevel = true;
        auto asyncEntryRef = SILDeclRef::getAsyncMainFileEntryPoint(sf);
        auto *asyncTopLevel = sgm.getFunction(asyncEntryRef, ForDefinition);
        SILGenFunction(sgm, *toplevel, sf).emitAsyncMainThreadStart(asyncEntryRef);
        toplevel = asyncTopLevel;
        EntryRef = asyncEntryRef;
      }

      toplevel->createProfiler(EntryRef);

      sgm.TopLevelSGF = new SILGenFunction(sgm, *toplevel, sf);
      sgm.TopLevelSGF->MagicFunctionName = sgm.SwiftModule->getName();
      auto moduleCleanupLoc = CleanupLocation::getModuleCleanupLocation();

      sgm.TopLevelSGF->prepareEpilog(None, true, moduleCleanupLoc);

      auto prologueLoc = RegularLocation::getModuleLocation();
      prologueLoc.markAsPrologue();
      if (sf->isAsyncContext()) {
        // emitAsyncMainThreadStart will create argc and argv.
        // Just set the main actor as the expected executor; we should
        // already be running on it.
        SILValue executor = sgm.TopLevelSGF->emitMainExecutor(prologueLoc);
        sgm.TopLevelSGF->ExpectedExecutor =
            sgm.TopLevelSGF->B.createOptionalSome(
                prologueLoc, executor,
                SILType::getOptionalType(executor->getType()));
      } else {
        // Create the argc and argv arguments.
        auto entry = sgm.TopLevelSGF->B.getInsertionBB();
        auto context = sgm.TopLevelSGF->getTypeExpansionContext();
        auto paramTypeIter = sgm.TopLevelSGF->F.getConventions()
                                .getParameterSILTypes(context)
                                .begin();

        entry->createFunctionArgument(*paramTypeIter);
        entry->createFunctionArgument(*std::next(paramTypeIter));
      }
      scope.emplace(sgm.TopLevelSGF->Cleanups, moduleCleanupLoc);
    }
  }

  ~SourceFileScope() {
    if (sgm.TopLevelSGF) {
      scope.reset();

      // Unregister the top-level function emitter.
      auto &SGF = *sgm.TopLevelSGF;
      sgm.TopLevelSGF = nullptr;

      // Write out the epilog.
      auto moduleLoc = RegularLocation::getModuleLocation();
      moduleLoc.markAutoGenerated();
      auto returnInfo = SGF.emitEpilogBB(moduleLoc);
      auto returnLoc = returnInfo.second;
      returnLoc.markAutoGenerated();

      SILFunction *exitFunc = nullptr;

      SILType returnType;
      if (isAsyncTopLevel) {
        FuncDecl *exitFuncDecl = sgm.getExit();
        assert(exitFuncDecl && "Failed to find exit function declaration");
        exitFunc = sgm.getFunction(
            SILDeclRef(exitFuncDecl, SILDeclRef::Kind::Func, /*isForeign*/true),
            NotForDefinition);
        SILFunctionType & funcType = *exitFunc->getLoweredType().getAs<SILFunctionType>();
        returnType = SILType::getPrimitiveObjectType(
            funcType.getParameters().front().getInterfaceType());
      } else {
        returnType = SGF.F.getConventions().getSingleSILResultType(
              SGF.getTypeExpansionContext());
      }

      auto emitTopLevelReturnValue = [&](unsigned value) -> SILValue {
        // Create an integer literal for the value.
        auto litType = SILType::getBuiltinIntegerType(32, sgm.getASTContext());
        SILValue retValue =
          SGF.B.createIntegerLiteral(moduleLoc, litType, value);

        // Wrap that in a struct if necessary.
        if (litType != returnType) {
          retValue = SGF.B.createStruct(moduleLoc, returnType, retValue);
        }
        return retValue;
      };

      // Fallthrough should signal a normal exit by returning 0.
      SILValue returnValue;
      if (SGF.B.hasValidInsertionPoint())
        returnValue = emitTopLevelReturnValue(0);

      // Handle the implicit rethrow block.
      auto rethrowBB = SGF.ThrowDest.getBlock();
      SGF.ThrowDest = JumpDest::invalid();

      // If the rethrow block wasn't actually used, just remove it.
      if (rethrowBB->pred_empty()) {
        SGF.eraseBasicBlock(rethrowBB);

      // Otherwise, we need to produce a unified return block.
      } else {
        auto returnBB = SGF.createBasicBlock();
        if (SGF.B.hasValidInsertionPoint())
          SGF.B.createBranch(returnLoc, returnBB, returnValue);
        returnValue =
            returnBB->createPhiArgument(returnType, OwnershipKind::Owned);
        SGF.B.emitBlock(returnBB);

        // Emit the rethrow block.
        SILGenSavedInsertionPoint savedIP(SGF, rethrowBB,
                                          FunctionSection::Postmatter);

        // Log the error.
        SILValue error = rethrowBB->getArgument(0);
        SGF.B.createBuiltin(moduleLoc,
                            sgm.getASTContext().getIdentifier("errorInMain"),
                            sgm.Types.getEmptyTupleType(), {}, {error});

        // Then end the lifetime of the error.
        //
        // We do this to appease the ownership verifier. We do not care about
        // actually destroying the value since we are going to immediately exit,
        // so this saves us a slight bit of code-size since end_lifetime is
        // stripped out after ownership is removed.
        SGF.B.createEndLifetime(moduleLoc, error);

        // Signal an abnormal exit by returning 1.
        SGF.Cleanups.emitCleanupsForReturn(CleanupLocation(moduleLoc),
                                           IsForUnwind);
        SGF.B.createBranch(returnLoc, returnBB, emitTopLevelReturnValue(1));
      }

      // Return.
      if (SGF.B.hasValidInsertionPoint()) {

        if (isAsyncTopLevel) {
          SILValue exitCall = SGF.B.createFunctionRef(moduleLoc, exitFunc);
          SGF.B.createApply(moduleLoc, exitCall, {}, {returnValue});
          SGF.B.createUnreachable(moduleLoc);
        } else {
          SGF.B.createReturn(returnLoc, returnValue);
        }
      }

      // Okay, we're done emitting the top-level function; destroy the
      // emitter and verify the result.
      SILFunction *toplevel = &SGF.getFunction();
      delete &SGF;

      LLVM_DEBUG(llvm::dbgs() << "lowered toplevel sil:\n";
                 toplevel->print(llvm::dbgs()));
      toplevel->verifyIncompleteOSSA();
      sgm.emitLazyConformancesForFunction(toplevel);
    }
  }
};

// An RAII object that constructs a \c SILGenModule instance.
// On destruction, delayed definitions are automatically emitted.
class SILGenModuleRAII {
  SILGenModule SGM;

public:
  void emitSourceFile(SourceFile *sf) {
    // Type-check the file if we haven't already.
    performTypeChecking(*sf);

    SourceFileScope scope(SGM, sf);
    for (auto *D : sf->getTopLevelDecls()) {
      // Emit auxiliary decls.
      D->visitAuxiliaryDecls([&](Decl *auxiliaryDecl) {
        // Skip extensions decls; they are visited below.
        if (isa<ExtensionDecl>(auxiliaryDecl))
          return;

        FrontendStatsTracer StatsTracer(SGM.getASTContext().Stats,
                                        "SILgen-decl", auxiliaryDecl);
        SGM.visit(auxiliaryDecl);
      });

      FrontendStatsTracer StatsTracer(SGM.getASTContext().Stats,
                                      "SILgen-decl", D);
      SGM.visit(D);
    }

    // FIXME: Visit macro-generated extensions separately.
    //
    // The code below that visits auxiliary decls of the top-level
    // decls in the source file does not work for nested types with
    // attached conformance macros:
    // ```
    // struct Outer {
    //   @AddConformance struct Inner {}
    // }
    // ```
    // Because the attached-to decl is not at the top-level. To fix this,
    // visit the macro-generated conformances that are recorded in the
    // synthesized file unit to cover all macro-generated extension decls.
    if (auto *synthesizedFile = sf->getSynthesizedFile()) {
      for (auto *D : synthesizedFile->getTopLevelDecls()) {
        if (!isa<ExtensionDecl>(D))
          continue;

        auto *sf = D->getInnermostDeclContext()->getParentSourceFile();
        if (sf->getFulfilledMacroRole() != MacroRole::Conformance)
          continue;

        FrontendStatsTracer StatsTracer(SGM.getASTContext().Stats,
                                        "SILgen-decl", D);
        SGM.visit(D);
      }
    }

    for (Decl *D : sf->getHoistedDecls()) {
      FrontendStatsTracer StatsTracer(SGM.getASTContext().Stats,
                                      "SILgen-decl", D);
      SGM.visit(D);
    }

    for (TypeDecl *TD : sf->LocalTypeDecls) {
      FrontendStatsTracer StatsTracer(SGM.getASTContext().Stats,
                                      "SILgen-tydecl", TD);
      // FIXME: Delayed parsing would prevent these types from being added to
      //        the module in the first place.
      if (TD->getDeclContext()->getInnermostSkippedFunctionContext())
        continue;
      SGM.visit(TD);
    }

    for (auto *D : sf->getDeclsWithRuntimeDiscoverableAttrs()) {
      for (auto *attr : D->getRuntimeDiscoverableAttrs()) {
        emitSILFunctionDefinition(
            SILDeclRef::getRuntimeAttributeGenerator(attr, D)
                .asRuntimeAccessible());
      }
    }

    // If the source file contains an artificial main, emit the implicit
    // top-level code.
    if (auto *mainDecl = sf->getMainDecl()) {
      if (isa<FuncDecl>(mainDecl) &&
          static_cast<FuncDecl *>(mainDecl)->hasAsync())
        emitSILFunctionDefinition(
            SILDeclRef::getAsyncMainDeclEntryPoint(mainDecl));
      emitSILFunctionDefinition(SILDeclRef::getMainDeclEntryPoint(mainDecl));
    }
  }

  void emitSILFunctionDefinition(SILDeclRef ref) {
    SGM.emitFunctionDefinition(ref, SGM.getFunction(ref, ForDefinition));
  }

  explicit SILGenModuleRAII(SILModule &M) : SGM{M, M.getSwiftModule()} {}

  ~SILGenModuleRAII() {
    // Emit any delayed definitions that were forced.
    // Emitting these may in turn force more definitions, so we have to take
    // care to keep pumping the queues.
    while (!SGM.pendingForcedFunctions.empty()
           || !SGM.pendingConformances.empty()) {
      while (!SGM.pendingForcedFunctions.empty()) {
        auto &front = SGM.pendingForcedFunctions.front();
        SGM.emitFunctionDefinition(
            front, SGM.getEmittedFunction(front, ForDefinition));
        SGM.pendingForcedFunctions.pop_front();
      }
      while (!SGM.pendingConformances.empty()) {
        SGM.getWitnessTable(SGM.pendingConformances.front());
        SGM.pendingConformances.pop_front();
      }
    }
  }
};
} // end anonymous namespace

std::unique_ptr<SILModule>
ASTLoweringRequest::evaluate(Evaluator &evaluator,
                             ASTLoweringDescriptor desc) const {
  // If we have a .sil file to parse, defer to the parsing request.
  if (desc.getSourceFileToParse()) {
    return llvm::cantFail(evaluator(ParseSILModuleRequest{desc}));
  }

  // For leak detection.
  SILInstruction::resetInstructionCounts();

  auto silMod = SILModule::createEmptyModule(desc.context, desc.conv,
                                             desc.opts, desc.irgenOptions);

  // If all function bodies are being skipped there's no reason to do any
  // SIL generation.
  if (desc.opts.SkipFunctionBodies == FunctionBodySkipping::All)
    return silMod;

  // Skip emitting SIL if there's been any compilation errors
  if (silMod->getASTContext().hadError() &&
      silMod->getASTContext().LangOpts.AllowModuleWithCompilerErrors)
    return silMod;

  SILGenModuleRAII scope(*silMod);

  // Emit a specific set of SILDeclRefs if needed.
  if (auto refs = desc.refsToEmit) {
    for (auto ref : *refs)
      scope.emitSILFunctionDefinition(ref);
  }

  // Emit any whole-files needed.
  for (auto file : desc.getFilesToEmit()) {
    if (auto *nextSF = dyn_cast<SourceFile>(file))
      scope.emitSourceFile(nextSF);
  }

  // Also make sure to process any intermediate files that may contain SIL.
  bool shouldDeserialize =
      llvm::any_of(desc.getFilesToEmit(), [](const FileUnit *File) -> bool {
        return isa<SerializedASTFile>(File);
      });
  if (shouldDeserialize) {
    auto *primary = desc.context.dyn_cast<FileUnit *>();
    silMod->getSILLoader()->getAllForModule(silMod->getSwiftModule()->getName(),
                                            primary);
  }

  return silMod;
}

std::unique_ptr<SILModule>
swift::performASTLowering(ModuleDecl *mod, Lowering::TypeConverter &tc,
                          const SILOptions &options,
                          const IRGenOptions *irgenOptions) {
  auto desc = ASTLoweringDescriptor::forWholeModule(mod, tc, options,
                                                    None, irgenOptions);
  return llvm::cantFail(
      mod->getASTContext().evaluator(ASTLoweringRequest{desc}));
}

std::unique_ptr<SILModule>
swift::performASTLowering(FileUnit &sf, Lowering::TypeConverter &tc,
                          const SILOptions &options,
                          const IRGenOptions *irgenOptions) {
  auto desc = ASTLoweringDescriptor::forFile(sf, tc, options, None, irgenOptions);
  return llvm::cantFail(sf.getASTContext().evaluator(ASTLoweringRequest{desc}));
}
