//===--- GenObjC.cpp - Objective-C interaction ----------------------------===//
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
//  This file implements bridging to Objective-C.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InlineAsm.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/Basic/CharInfo.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"

#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"

#include "CallEmission.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenFunc.h"
#include "GenHeap.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MetadataRequest.h"
#include "NativeConventionSchema.h"
#include "ScalarTypeInfo.h"
#include "StructLayout.h"

#include "GenObjC.h"

using namespace swift;
using namespace irgen;

namespace {

/// A utility class that saves the original type of a value in its constructor,
/// casts the value to i8*, and then allows values later to be casted to the
/// original type.
struct CastToInt8PtrTy {
  llvm::Type *OrigTy;

  CastToInt8PtrTy(IRGenFunction &IGF, llvm::Value *&value)
      : OrigTy(value->getType()) {
    if (OrigTy->isPointerTy())
      value = IGF.Builder.CreateBitCast(value, IGF.IGM.Int8PtrTy);
    else
      value = IGF.Builder.CreateIntToPtr(value, IGF.IGM.Int8PtrTy);
  }

  llvm::Value *restore(IRGenFunction &IGF, llvm::Value *value) const {
    assert(value->getType() == IGF.IGM.Int8PtrTy);
    if (OrigTy->isPointerTy())
      return IGF.Builder.CreateBitCast(value, OrigTy);
    else
      return IGF.Builder.CreatePtrToInt(value, OrigTy);
  }
};

}

void IRGenFunction::emitObjCStrongRelease(llvm::Value *value) {
  CastToInt8PtrTy savedType(*this, value);
  Builder.CreateIntrinsicCall(llvm::Intrinsic::objc_release, value);
}

void IRGenFunction::emitObjCStrongRetain(llvm::Value *v) {
  emitObjCRetainCall(v);
}

llvm::Value *IRGenFunction::emitObjCRetainCall(llvm::Value *value) {
  CastToInt8PtrTy savedType(*this, value);
  auto call = Builder.CreateIntrinsicCall(llvm::Intrinsic::objc_retain, value);
  return savedType.restore(*this, call);
}

llvm::Value *IRGenFunction::emitObjCAutoreleaseCall(llvm::Value *value) {
  CastToInt8PtrTy savedType(*this, value);
  auto call = Builder.CreateIntrinsicCall(llvm::Intrinsic::objc_autorelease,
                                          value);
  return savedType.restore(*this, call);
}

llvm::InlineAsm *IRGenModule::getObjCRetainAutoreleasedReturnValueMarker() {
  // Check to see if we've already computed the market.  Note that we
  // might have cached a null marker, and that's fine.
  auto &cache = ObjCRetainAutoreleasedReturnValueMarker;
  if (cache.has_value())
    return cache.value();

  // Ask the target for the string.
  StringRef asmString = TargetInfo.ObjCRetainAutoreleasedReturnValueMarker;

  // If the string is empty, just leave, remembering that we did all this.
  if (asmString.empty()) {
    cache = nullptr;
    return nullptr;
  }

  // If we're emitting optimized code, record the string in the module
  // and let the late ARC pass insert it, but don't generate any calls
  // right now.
  if (IRGen.Opts.shouldOptimize()) {
    const char *markerKey = "clang.arc.retainAutoreleasedReturnValueMarker";
    if (!Module.getModuleFlag(markerKey)) {
      auto *str = llvm::MDString::get(getLLVMContext(), asmString);
      Module.addModuleFlag(llvm::Module::Error, markerKey, str);
    }

    cache = nullptr;

  // Otherwise, create the module
  } else {
    llvm::FunctionType *type =
      llvm::FunctionType::get(VoidTy, /*variadic*/false);
    cache = llvm::InlineAsm::get(type, asmString, "", /*sideeffects*/ true);
  }

  return cache.value();
}

/// Reclaim an autoreleased return value.
llvm::Value *irgen::emitObjCRetainAutoreleasedReturnValue(IRGenFunction &IGF,
                                                          llvm::Value *value) {
  // Call the inline-assembly marker if we need one.
  if (auto marker = IGF.IGM.getObjCRetainAutoreleasedReturnValueMarker()) {
    IGF.Builder.CreateAsmCall(marker, {});
  }

  CastToInt8PtrTy savedType(IGF, value);

  auto call = IGF.Builder.CreateIntrinsicCall(
                     llvm::Intrinsic::objc_retainAutoreleasedReturnValue, value);

  const llvm::Triple &triple = IGF.IGM.Context.LangOpts.Target;
  if (triple.getArch() == llvm::Triple::x86_64) {
    // Don't tail call objc_retainAutoreleasedReturnValue. This blocks the
    // autoreleased return optimization.
    // callq  0x01ec08 ; symbol stub for: objc_msgSend
    // movq   %rax, %rdi
    // popq   %rbp  ;<== Blocks the handshake from objc_autoreleaseReturnValue
    // jmp    0x01ec20 ; symbol stub for: objc_retainAutoreleasedReturnValue
    call->setTailCallKind(llvm::CallInst::TCK_NoTail);
  }

  return savedType.restore(IGF, call);
}

/// Autorelease a return value.
llvm::Value *irgen::emitObjCAutoreleaseReturnValue(IRGenFunction &IGF,
                                                   llvm::Value *value) {
  CastToInt8PtrTy savedType(IGF, value);

  auto call = IGF.Builder.CreateIntrinsicCall(
                llvm::Intrinsic::objc_autoreleaseReturnValue, value);
  call->setDoesNotThrow();
  call->setTailCall(); // force tail calls at -O0
  return savedType.restore(IGF, call);
}

namespace {
  /// A type-info implementation suitable for AnyObject on platforms with ObjC
  /// interop.
  class UnknownTypeInfo : public HeapTypeInfo<UnknownTypeInfo> {
  public:
    UnknownTypeInfo(llvm::PointerType *storageType, Size size,
                 SpareBitVector spareBits, Alignment align)
      : HeapTypeInfo(ReferenceCounting::Unknown, storageType, size, spareBits,
                     align) {
    }

    /// AnyObject requires ObjC reference-counting.
    ReferenceCounting getReferenceCounting() const {
      return ReferenceCounting::Unknown;
    }
  };
} // end anonymous namespace

const LoadableTypeInfo *TypeConverter::convertBuiltinUnknownObject() {
  // UnknownObject is only interestingly different from NativeObject on
  // platforms with ObjC interop.
  if (IGM.Context.LangOpts.EnableObjCInterop) {
    return new UnknownTypeInfo(IGM.ObjCPtrTy, IGM.getPointerSize(),
                               IGM.getHeapObjectSpareBits(),
                               IGM.getPointerAlignment());
  }
  
  // Without ObjC interop, UnknownObject handles just like a NativeObject.
  return convertBuiltinNativeObject();
}

namespace {
  /// A type info implementation for BridgeObject
  class BridgeObjectTypeInfo : public HeapTypeInfo<BridgeObjectTypeInfo> {
  public:
    BridgeObjectTypeInfo(llvm::PointerType *storageType, Size size,
                 SpareBitVector spareBits, Alignment align)
      : HeapTypeInfo(ReferenceCounting::Bridge, storageType, size, spareBits,
                     align) {}

    /// Builtin.BridgeObject uses its own specialized refcounting implementation.
    ReferenceCounting getReferenceCounting() const {
      return ReferenceCounting::Bridge;
    }
  };
} // end anonymous namespace


const LoadableTypeInfo *TypeConverter::convertBuiltinBridgeObject() {
  return new BridgeObjectTypeInfo(IGM.BridgeObjectPtrTy, IGM.getPointerSize(),
      SpareBitVector::getConstant(IGM.getPointerSize().getValueInBits(), false),
                                  IGM.getPointerAlignment());
}

const TypeInfo &IRGenModule::getObjCClassPtrTypeInfo() {
  return Types.getObjCClassPtrTypeInfo();
}

const TypeInfo &TypeConverter::getObjCClassPtrTypeInfo() {
  // ObjC class pointers look like unmanaged (untagged) object references.
  if (ObjCClassPtrTI) return *ObjCClassPtrTI;
  ObjCClassPtrTI =
    createUnmanagedStorageType(IGM.ObjCClassPtrTy, ReferenceCounting::ObjC,
                               /*isOptional*/false);
  ObjCClassPtrTI->NextConverted = FirstType;
  FirstType = ObjCClassPtrTI;
  return *ObjCClassPtrTI;
}

/// Get or create a global Objective-C method name.  Always returns an i8*.
llvm::Constant *IRGenModule::getAddrOfObjCMethodName(StringRef selector) {
  // Check whether this selector already exists.
  auto &entry = ObjCMethodNames[selector];
  if (entry) return entry;

  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(getLLVMContext(), selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), false,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init,
                          llvm::Twine("\01L_selector_data(") + selector + ")");
  SetCStringLiteralSection(global, ObjCLabelType::MethodVarName);
  global->setAlignment(llvm::MaybeAlign(1));
  addCompilerUsedGlobal(global);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
      init->getType(), global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Get or create an Objective-C selector reference.  Always returns
/// an i8**.  The design is that the compiler will emit a load of this
/// pointer, and the linker will ensure that pointer is unique.
llvm::Constant *IRGenModule::getAddrOfObjCSelectorRef(StringRef selector) {
  // Check whether a reference for this selector already exists.
  auto &entry = ObjCSelectorRefs[selector];
  if (entry) return entry;

  // If not, create it.  The initializer is just a pointer to the
  // method name.  Note that the label here is unimportant, so we
  // choose something descriptive to make the IR readable.
  auto init = getAddrOfObjCMethodName(selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), false,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init,
                                llvm::Twine("\01L_selector(") + selector + ")");
  global->setExternallyInitialized(true);
  global->setAlignment(llvm::MaybeAlign(getPointerAlignment().getValue()));

  // This section name is magical for the Darwin static and dynamic linkers.
  global->setSection(GetObjCSectionName("__objc_selrefs",
                                        "literal_pointers,no_dead_strip"));

  // Make sure that this reference does not get optimized away.
  addCompilerUsedGlobal(global);

  // Cache and return.
  entry = global;
  return global;
}

/// Get or create an ObjC protocol record. Always returns an i8*. We lazily
/// create ObjC protocol_t records for protocols, storing references to the
/// record into the __objc_protolist and __objc_protorefs sections to be
/// fixed up by the runtime.
///
/// It is not correct to use this value as a Protocol* reference directly. The
/// ObjC runtime requires protocol references to be loaded from an
/// indirect variable, the address of which is given by
/// getAddrOfObjCProtocolRef.
llvm::Constant *
IRGenModule::getAddrOfObjCProtocolRecord(ProtocolDecl *proto,
                                         ForDefinition_t forDefinition) {
  return const_cast<llvm::Constant*>
    (cast<llvm::Constant>(getObjCProtocolGlobalVars(proto).record));
}

/// Get or create an ObjC protocol reference. Always returns an i8**. We lazily
/// create ObjC protocol_t records for protocols, storing references to the
/// record into the __objc_protolist and __objc_protorefs sections to be
/// fixed up by the runtime.
Address IRGenModule::getAddrOfObjCProtocolRef(ProtocolDecl *proto,
                                              ForDefinition_t forDefinition) {
  return Address(const_cast<llvm::Constant *>(cast<llvm::Constant>(
                     getObjCProtocolGlobalVars(proto).ref)),
                 Int8PtrTy, getPointerAlignment());
}

IRGenModule::ObjCProtocolPair
IRGenModule::getObjCProtocolGlobalVars(ProtocolDecl *proto) {
  // See whether we already emitted this protocol reference.
  auto found = ObjCProtocols.find(proto);
  if (found != ObjCProtocols.end()) {
    return found->second;
  }
  
  // Create a placeholder protocol record.
  llvm::Constant *protocolRecord =
    new llvm::GlobalVariable(Module, Int8Ty, /*constant*/ false,
                             llvm::GlobalValue::PrivateLinkage, nullptr);
  LazyObjCProtocolDefinitions.push_back(proto);

  // Introduce a variable to label the protocol.
  llvm::SmallString<64> nameBuffer;
  StringRef protocolName = proto->getObjCRuntimeName(nameBuffer);
  auto *protocolLabel
    = new llvm::GlobalVariable(Module, Int8PtrTy,
                               /*constant*/ false,
                               llvm::GlobalValue::WeakAnyLinkage,
                               protocolRecord,
                               llvm::Twine("\01l_OBJC_LABEL_PROTOCOL_$_")
                                 + protocolName);
  protocolLabel->setAlignment(
      llvm::MaybeAlign(getPointerAlignment().getValue()));
  protocolLabel->setVisibility(llvm::GlobalValue::HiddenVisibility);
  protocolLabel->setSection(GetObjCSectionName("__objc_protolist",
                                               "coalesced,no_dead_strip"));

  // Mark used to prevent DCE of public unreferenced protocols to ensure
  // that they are available for external use when a used module is used
  // as a library.
  addUsedGlobal(protocolLabel);

  // Introduce a variable to reference the protocol.
  auto *protocolRef =
      new llvm::GlobalVariable(Module, Int8PtrTy, /*constant*/ false,
                               llvm::GlobalValue::WeakAnyLinkage,
                               protocolRecord,
                               llvm::Twine("\01l_OBJC_PROTOCOL_REFERENCE_$_") + protocolName);
  protocolRef->setAlignment(llvm::MaybeAlign(getPointerAlignment().getValue()));
  protocolRef->setVisibility(llvm::GlobalValue::HiddenVisibility);
  protocolRef->setSection(GetObjCSectionName("__objc_protorefs",
                                             "coalesced,no_dead_strip"));

  // Mark used to prevent DCE of public unreferenced protocols to ensure
  // that they are available for external use when a used module is used
  // as a library.
  addUsedGlobal(protocolRef);

  ObjCProtocolPair pair{protocolRecord, protocolRef};
  ObjCProtocols.insert({proto, pair});
  
  return pair;
}

static std::pair<uint64_t, llvm::ConstantArray *>
getProtocolRefsList(llvm::Constant *protocol) {
  // We expect to see a structure like this.
  // @"_OBJC_PROTOCOL_$_MyProto" = weak hidden global %struct._protocol_t {
  //  i8* null,
  //  i8* getelementptr inbounds ([8 x i8],
  //    [8 x i8]* @OBJC_CLASS_NAME_, i32 0, i32 0),
  //  %struct._objc_protocol_list* bitcast (
  //    { i64, [2 x %struct._protocol_t*] }*
  //       @"_OBJC_$_PROTOCOL_REFS_MyProto" to %struct._objc_protocol_list*),
  //  %struct.__method_list_t* null,
  //  %struct.__method_list_t* null,
  //  %struct.__method_list_t* null,
  //  %struct.__method_list_t* null,
  //  %struct._prop_list_t* null, i32 96, i32 0,
  //  i8** getelementptr inbounds ([1 x i8*],
  //    [1 x i8*]* @"_OBJC_$_PROTOCOL_METHOD_TYPES_MyProto", i32 0, i32 0),
  //  i8* null, %struct._prop_list_t* null }, align 8
  auto protocolVar = cast<llvm::GlobalVariable>(protocol);
  auto protocolStruct =
      cast<llvm::ConstantStruct>(protocolVar->getInitializer());
  auto objCProtocolList = cast<llvm::Constant>(protocolStruct->getOperand(2));
  if (objCProtocolList->isNullValue()) {
    return std::make_pair(0, nullptr);
  }

  if (!protocol->getContext().supportsTypedPointers()) {
    auto protocolRefsVar = cast<llvm::GlobalVariable>(objCProtocolList);
    auto sizeListPair =
        cast<llvm::ConstantStruct>(protocolRefsVar->getInitializer());
    auto size =
        cast<llvm::ConstantInt>(sizeListPair->getOperand(0))->getZExtValue();
    auto protocolRefsList =
        cast<llvm::ConstantArray>(sizeListPair->getOperand(1));
    return std::make_pair(size, protocolRefsList);
  }

  auto bitcast = cast<llvm::ConstantExpr>(objCProtocolList);
  assert(bitcast->getOpcode() == llvm::Instruction::BitCast);
  auto protocolRefsVar = cast<llvm::GlobalVariable>(bitcast->getOperand(0));
  auto sizeListPair =
      cast<llvm::ConstantStruct>(protocolRefsVar->getInitializer());
  auto size =
      cast<llvm::ConstantInt>(sizeListPair->getOperand(0))->getZExtValue();
  auto protocolRefsList =
      cast<llvm::ConstantArray>(sizeListPair->getOperand(1));
  return std::make_pair(size, protocolRefsList);
}

static void appendNonRuntimeImpliedProtocols(
  clang::ObjCProtocolDecl *proto,
  llvm::SetVector<clang::ObjCProtocolDecl *> &nonRuntimeImpliedProtos) {

  if (!proto->isNonRuntimeProtocol()) {
    nonRuntimeImpliedProtos.insert(proto->getCanonicalDecl());
    return;
  }

  for (auto *parent : proto->protocols())
    appendNonRuntimeImpliedProtocols(parent, nonRuntimeImpliedProtos);
}

// Get runtime protocol list used during emission of objective-c protocol
// metadata taking non-runtime protocols into account.
static std::vector<clang::ObjCProtocolDecl *>
getRuntimeProtocolList(clang::ObjCProtocolDecl::protocol_range protocols) {

  llvm::DenseSet<clang::ObjCProtocolDecl *> nonRuntimeProtocols;
  std::vector<clang::ObjCProtocolDecl*> runtimeProtocols;
  for (auto p: protocols) {
    auto *proto = p->getCanonicalDecl();
    if (proto->isNonRuntimeProtocol())
      nonRuntimeProtocols.insert(proto);
    else
      runtimeProtocols.push_back(proto);
  }

  if (nonRuntimeProtocols.empty())
    return runtimeProtocols;

  // Find the non-runtime implied protocols: protocols that occur in the closest
  // ancestry of a non-runtime protocol.
  llvm::SetVector<clang::ObjCProtocolDecl *> nonRuntimeImpliedProtos;
  for (auto *nonRuntimeProto : nonRuntimeProtocols) {
    appendNonRuntimeImpliedProtocols(nonRuntimeProto, nonRuntimeImpliedProtos);
  }

  // Subtract the implied protocols of the runtime protocols and non runtime
  // protoocls implied protocols form the non runtime implied protocols.
  llvm::DenseSet<const clang::ObjCProtocolDecl *> impliedProtocols;
  for (auto *p : runtimeProtocols) {
    impliedProtocols.insert(p);
    p->getImpliedProtocols(impliedProtocols);
  }

  for (auto *p : nonRuntimeImpliedProtos) {
    p->getImpliedProtocols(impliedProtocols);
  }

  for (auto *p : nonRuntimeImpliedProtos) {
    if (!impliedProtocols.contains(p)) {
      runtimeProtocols.push_back(p);
    }
  }

  return runtimeProtocols;
}

static void updateProtocolRefs(IRGenModule &IGM,
                               const clang::ObjCProtocolDecl *objcProtocol,
                               llvm::Constant *protocol) {

  // Get the clang importer to map ObjCProtocolDecl to ProtocolDecl.
  auto &astContext = IGM.getSwiftModule()->getASTContext();
  auto *clangImporter =
      static_cast<ClangImporter *>(astContext.getClangModuleLoader());
  assert(clangImporter && "Must have a clang importer");

  // Get the array containining the protocol refs.
  unsigned protocolRefsSize = getProtocolRefsList(protocol).first;
  unsigned currentIdx = 0;
  auto inheritedObjCProtocols = getRuntimeProtocolList(objcProtocol->protocols());
  for (auto inheritedObjCProtocol : inheritedObjCProtocols) {
    assert(currentIdx < protocolRefsSize);

    // Getting the `protocolRefs` constant must not be hoisted out of the loop
    // because this constant might be deleted by
    // `oldVar->replaceAllUsesWith(newOpd)` below.
    llvm::ConstantArray *protocolRefs = getProtocolRefsList(protocol).second;
    auto oldVar = protocolRefs->getOperand(currentIdx);
    // Map the objc protocol to swift protocol.
    auto optionalDecl = clangImporter->importDeclCached(inheritedObjCProtocol);
    // This should not happen but the compiler currently silently accepts
    // protocol forward declarations without definitions (102058759).
    if (!optionalDecl || *optionalDecl == nullptr) {
      ++currentIdx;
      continue;
    }
    auto inheritedSwiftProtocol = cast<ProtocolDecl>(*optionalDecl);
    // Get the objc protocol record we use in Swift.
    auto record = IGM.getAddrOfObjCProtocolRecord(inheritedSwiftProtocol,
                                                  NotForDefinition);
    auto newOpd = llvm::ConstantExpr::getBitCast(record, oldVar->getType());
    if (newOpd != oldVar) {
      oldVar->replaceUsesWithIf(newOpd, [protocol](llvm::Use &U) -> bool {
                                return U.getUser() == getProtocolRefsList(protocol).second;
                                });
    }
    ++currentIdx;
  }
  assert(currentIdx == protocolRefsSize);
}

llvm::Constant *IRGenModule::emitClangProtocolObject(
    const clang::ObjCProtocolDecl *objcProtocol) {
  auto clangProto =
      clang::CodeGen::emitObjCProtocolObject(getClangCGM(), objcProtocol);
  updateProtocolRefs(*this, objcProtocol, clangProto);
  return clangProto;
}

void IRGenModule::emitLazyObjCProtocolDefinition(ProtocolDecl *proto) {
  // Emit the real definition.
  auto record = cast<llvm::GlobalVariable>(emitObjCProtocolData(*this, proto));

  // Find the placeholder.  It should always still be a placeholder,
  // because it was created as an anonymous symbol and nobody should
  // ever be randomly messing with those.
  auto placeholder =
    cast<llvm::GlobalVariable>(ObjCProtocols.find(proto)->second.record);

  // Move the new record to the placeholder's position.
  Module.getGlobalList().remove(record);
  Module.getGlobalList().insertAfter(placeholder->getIterator(), record);

  // Replace and destroy the placeholder.
  placeholder->replaceAllUsesWith(
                            llvm::ConstantExpr::getBitCast(record, Int8PtrTy));
  placeholder->eraseFromParent();
}

void IRGenModule::emitLazyObjCProtocolDefinitions() {
  // Emit any lazy ObjC protocol definitions we require.  Try to do
  // this in the order in which we needed them, since they can require
  // other protocol definitions recursively.
  for (size_t i = 0; i != LazyObjCProtocolDefinitions.size(); ++i) {
    ProtocolDecl *protocol = LazyObjCProtocolDefinitions[i];
    emitLazyObjCProtocolDefinition(protocol);
  }
}

namespace {
  class Selector {
    
    llvm::SmallString<80> Buffer;
    StringRef Text;

  public:

    static constexpr struct ForGetter_t { } ForGetter{};
    static constexpr struct ForSetter_t { } ForSetter{};
    
    Selector() = default;

    Selector(FuncDecl *method) {
      Text = method->getObjCSelector().getString(Buffer);
    }
    
    Selector(ConstructorDecl *ctor) {
      Text = ctor->getObjCSelector().getString(Buffer);
    }
    
    Selector(ValueDecl *methodOrCtorOrDtor) {
      if (auto *method = dyn_cast<FuncDecl>(methodOrCtorOrDtor)) {
        Text = method->getObjCSelector().getString(Buffer);
      } else if (auto *ctor = dyn_cast<ConstructorDecl>(methodOrCtorOrDtor)) {
        Text = ctor->getObjCSelector().getString(Buffer);
      } else if (isa<DestructorDecl>(methodOrCtorOrDtor)) {
        Text = "dealloc";
      } else {
        llvm_unreachable("property or subscript selector should be generated "
                         "using ForGetter or ForSetter constructors");
      }
    }
    
    Selector(AbstractStorageDecl *asd, ForGetter_t) {
      Text = asd->getObjCGetterSelector().getString(Buffer);
    }

    Selector(AbstractStorageDecl *asd, ForSetter_t) {
      Text = asd->getObjCSetterSelector().getString(Buffer);
    }

    Selector(SILDeclRef ref) {
      switch (ref.kind) {
      case SILDeclRef::Kind::DefaultArgGenerator:
      case SILDeclRef::Kind::StoredPropertyInitializer:
      case SILDeclRef::Kind::EnumElement:
      case SILDeclRef::Kind::GlobalAccessor:
      case SILDeclRef::Kind::PropertyWrapperBackingInitializer:
      case SILDeclRef::Kind::PropertyWrapperInitFromProjectedValue:
      case SILDeclRef::Kind::EntryPoint:
      case SILDeclRef::Kind::AsyncEntryPoint:
      case SILDeclRef::Kind::RuntimeAttributeGenerator:
        llvm_unreachable("Method does not have a selector");

      case SILDeclRef::Kind::Destroyer:
      case SILDeclRef::Kind::Deallocator:
        Text = "dealloc";
        break;
          
      case SILDeclRef::Kind::Func:
        Text = cast<FuncDecl>(ref.getDecl())->getObjCSelector()
                 .getString(Buffer);
        break;

      case SILDeclRef::Kind::Allocator:
      case SILDeclRef::Kind::Initializer:
        Text = cast<ConstructorDecl>(ref.getDecl())->getObjCSelector()
                 .getString(Buffer);
        break;

      case SILDeclRef::Kind::IVarInitializer:
        Text = ".cxx_construct";
        break;

      case SILDeclRef::Kind::IVarDestroyer:
        Text = ".cxx_destruct";
        break;
      }
    }

    StringRef str() const {
      return Text;
    }
  };
} // end anonymous namespace

llvm::Constant *IRGenModule::getAddrOfObjCSelectorRef(SILDeclRef method) {
  assert(method.isForeign);
  return getAddrOfObjCSelectorRef(Selector(method).str());
}

std::string IRGenModule::getObjCSelectorName(SILDeclRef method) {
  assert(method.isForeign);
  return Selector(method).str().str();
}

static llvm::Value *emitSuperArgument(IRGenFunction &IGF,
                                      bool isInstanceMethod,
                                      llvm::Value *selfValue,
                                      CanType searchClass) {
  // Allocate an objc_super struct.
  Address super = IGF.createAlloca(IGF.IGM.ObjCSuperStructTy,
                                   IGF.IGM.getPointerAlignment(),
                                   "objc_super");
  // TODO: Track lifetime markers for function args.
  llvm::Value *self = IGF.Builder.CreateBitCast(selfValue,
                                                IGF.IGM.ObjCPtrTy);
  
  // Generate the search class object reference.
  llvm::Value *searchValue;
  if (isInstanceMethod) {
    searchValue = emitClassHeapMetadataRef(IGF, searchClass,
                                           MetadataValueType::ObjCClass,
                                           MetadataState::Complete,
                                           /*allow uninitialized*/ true);
  } else {
    searchClass = cast<MetatypeType>(searchClass).getInstanceType();
    ClassDecl *searchClassDecl = searchClass.getClassOrBoundGenericClass();
    switch (IGF.IGM.getClassMetadataStrategy(searchClassDecl)) {
    case ClassMetadataStrategy::Resilient:
    case ClassMetadataStrategy::Singleton:
    case ClassMetadataStrategy::Update:
    case ClassMetadataStrategy::FixedOrUpdate:
      searchValue = emitClassHeapMetadataRef(IGF, searchClass,
                                             MetadataValueType::ObjCClass,
                                             MetadataState::Complete,
                                             /*allow uninitialized*/ true);
      searchValue = emitLoadOfObjCHeapMetadataRef(IGF, searchValue);
      searchValue = IGF.Builder.CreateBitCast(searchValue, IGF.IGM.ObjCClassPtrTy);
      break;

    case ClassMetadataStrategy::Fixed:
      searchValue = IGF.IGM.getAddrOfMetaclassObject(searchClassDecl,
                                                     NotForDefinition);
      break;
    }
  }
  
  // Store the receiver and class to the struct.
  Address selfAddr = IGF.Builder.CreateStructGEP(super, 0, Size(0));
  IGF.Builder.CreateStore(self, selfAddr);

  Address searchAddr =
    IGF.Builder.CreateStructGEP(super, 1, IGF.IGM.getPointerSize());
  IGF.Builder.CreateStore(searchValue, searchAddr);
  
  // Pass a pointer to the objc_super struct to the messenger.
  // Project the ownership semantics of 'self' to the super argument.
  return super.getAddress();
}

static llvm::FunctionType *getMsgSendSuperTy(IRGenModule &IGM,
                                             llvm::FunctionType *fnTy,
                                             bool indirectResult) {
  SmallVector<llvm::Type*, 4> args(fnTy->param_begin(), fnTy->param_end());
  if (indirectResult)
    args[1] = IGM.ObjCSuperPtrTy;
  else
    args[0] = IGM.ObjCSuperPtrTy;
  return llvm::FunctionType::get(fnTy->getReturnType(), args, fnTy->isVarArg());
}

Callee irgen::getObjCMethodCallee(IRGenFunction &IGF,
                                  const ObjCMethod &methodInfo,
                                  llvm::Value *selfValue,
                                  CalleeInfo &&info) {
  SILDeclRef method = methodInfo.getMethod();
  assert((method.kind == SILDeclRef::Kind::Initializer
          || method.kind == SILDeclRef::Kind::Allocator
          || method.kind == SILDeclRef::Kind::Func
          || method.kind == SILDeclRef::Kind::Destroyer
          || method.kind == SILDeclRef::Kind::Deallocator) &&
         "objc method call must be to a func/initializer/getter/setter/dtor");

  auto kind = methodInfo.getMessageKind();

  Signature sig = IGF.IGM.getSignature(info.OrigFnType);
  bool indirectResult =
    sig.getForeignInfo().ClangInfo->getReturnInfo().isIndirect();
  if (kind != ObjCMessageKind::Normal) {
    sig.setType(getMsgSendSuperTy(IGF.IGM, sig.getType(), indirectResult));
  }

  // Create the appropriate messenger function.
  // FIXME: this needs to be target-specific.  Ask Clang for it!
  llvm::Constant *messenger = [&]() -> llvm::Constant* {
    if (indirectResult && IGF.IGM.TargetInfo.ObjCUseStret) {
      switch (kind) {
      case ObjCMessageKind::Normal:
        return IGF.IGM.getObjCMsgSendStretFn();

      case ObjCMessageKind::Peer:
        return IGF.IGM.getObjCMsgSendSuperStretFn();

      case ObjCMessageKind::Super:
        return IGF.IGM.getObjCMsgSendSuperStret2Fn();
      }
      llvm_unreachable("unhandled kind");
    } else {
      switch (kind) {
      case ObjCMessageKind::Normal:
        return IGF.IGM.getObjCMsgSendFn();

      case ObjCMessageKind::Peer:
        return IGF.IGM.getObjCMsgSendSuperFn();

      case ObjCMessageKind::Super:
        return IGF.IGM.getObjCMsgSendSuper2Fn();
      }
      llvm_unreachable("unhandled kind");
    }
  }();

  messenger = llvm::ConstantExpr::getBitCast(messenger,
                                             sig.getType()->getPointerTo());

  // super.constructor references an instance method (even though the
  // decl is really a 'static' member). Similarly, destructors refer
  // to the instance method -dealloc.
  bool isInstanceMethod
    = method.kind == SILDeclRef::Kind::Initializer
      || method.kind == SILDeclRef::Kind::Deallocator
      || method.getDecl()->isInstanceMember();

  llvm::Value *receiverValue;
  if (auto searchType = methodInfo.getSearchType()) {
    receiverValue =
      emitSuperArgument(IGF, isInstanceMethod, selfValue,
                        searchType.getASTType());
  } else {
    receiverValue = selfValue;
  }

  // Compute the selector.
  Selector selector(method);
  llvm::Value *selectorValue = IGF.emitObjCSelectorRefLoad(selector.str());

  auto fn =
      FunctionPointer::forDirect(FunctionPointer::Kind::Function, messenger,
                                 /*secondaryValue*/ nullptr, sig,
                                 /*useSignature*/ true);
  return Callee(std::move(info), fn, receiverValue, selectorValue);
}

Callee irgen::getObjCDirectMethodCallee(CalleeInfo &&info, const FunctionPointer &fn,
                                        llvm::Value *selfValue) {
  // Direct calls to Objective-C methods don't have a selector value.
  return Callee(std::move(info), fn, selfValue, nullptr);
}

/// Call [self allocWithZone: nil].
llvm::Value *irgen::emitObjCAllocObjectCall(IRGenFunction &IGF,
                                            llvm::Value *self,
                                            SILType selfType) {
  // Get an appropriately-cast function pointer.
  auto fn = IGF.IGM.getObjCAllocWithZoneFn();
  auto fnType = IGF.IGM.getObjCAllocWithZoneFnType();

  if (self->getType() != IGF.IGM.ObjCClassPtrTy) {
    fnType = llvm::FunctionType::get(self->getType(), self->getType(), false);
    fn = llvm::ConstantExpr::getBitCast(fn, fnType->getPointerTo());
  }

  auto call = IGF.Builder.CreateCall(fnType, fn, self);

  // Cast the returned pointer to the right type.
  auto &classTI = IGF.getTypeInfo(selfType);
  llvm::Type *destType = classTI.getStorageType();
  return IGF.Builder.CreateBitCast(call, destType);
}

static llvm::Function *emitObjCPartialApplicationForwarder(IRGenModule &IGM,
                                                           ObjCMethod method,
                                            CanSILFunctionType origMethodType,
                                            CanSILFunctionType resultType,
                                            const HeapLayout &layout,
                                            SILType selfType) {
  auto &selfTI = IGM.getTypeInfo(selfType);
 
  assert(resultType->getRepresentation()
           == SILFunctionType::Representation::Thick);

  llvm::AttributeList attrs;
  llvm::FunctionType *fwdTy = IGM.getFunctionType(resultType, attrs);
  // FIXME: Give the thunk a real name.
  // FIXME: Maybe cache the thunk by function and closure types?
  llvm::Function *fwd =
    llvm::Function::Create(fwdTy, llvm::Function::InternalLinkage,
                           MANGLE_AS_STRING(OBJC_PARTIAL_APPLY_THUNK_SYM),
                           &IGM.Module);
  fwd->setCallingConv(expandCallingConv(
      IGM, SILFunctionTypeRepresentation::Thick, false/*isAsync*/));

  fwd->setAttributes(attrs);
  // Merge initial attributes with attrs.
  llvm::AttrBuilder b(IGM.getLLVMContext());
  IGM.constructInitialFnAttributes(b);
  fwd->addFnAttrs(b);

  IRGenFunction subIGF(IGM, fwd);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(subIGF, fwd);
  
  // Do we need to lifetime-extend self?
  bool lifetimeExtendsSelf;
  auto results = origMethodType->getResults();
  if (results.size() == 1) {
    switch (results[0].getConvention()) {
    case ResultConvention::UnownedInnerPointer:
      lifetimeExtendsSelf = true;
      break;

    case ResultConvention::Indirect:
    case ResultConvention::Unowned:
    case ResultConvention::Owned:
    case ResultConvention::Autoreleased:
    case ResultConvention::Pack:
      lifetimeExtendsSelf = false;
      break;
    }
  } else {
    lifetimeExtendsSelf = false;
  }
  
  // Do we need to retain self before calling, and/or release it after?
  bool retainsSelf;
  switch (origMethodType->getParameters().back().getConvention()) {
  case ParameterConvention::Direct_Unowned:
    retainsSelf = false;
    break;
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Owned:
    retainsSelf = true;
    break;
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    llvm_unreachable("self passed indirectly?!");
  }
  
  // Recover 'self' from the context.
  Explosion params = subIGF.collectParameters();
  llvm::Value *context = params.takeLast();
  Address dataAddr = layout.emitCastTo(subIGF, context);
  auto &fieldLayout = layout.getElement(0);
  Address selfAddr = fieldLayout.project(subIGF, dataAddr, llvm::None);
  Explosion selfParams;
  if (retainsSelf)
    cast<LoadableTypeInfo>(selfTI).loadAsCopy(subIGF, selfAddr, selfParams);
  else
    cast<LoadableTypeInfo>(selfTI).loadAsTake(subIGF, selfAddr, selfParams);
  llvm::Value *self = selfParams.claimNext();
  
  // Save off the forwarded indirect return address if we have one.
  llvm::Value *formalIndirectResult = nullptr;
  llvm::Value *indirectedDirectResult = nullptr;
  const LoadableTypeInfo *indirectedResultTI = nullptr;
  if (origMethodType->hasIndirectFormalResults()) {
    // We should never import an ObjC method as returning a tuple which
    // would get broken up into multiple results like this.
    assert(origMethodType->getNumIndirectFormalResults() == 1);
    formalIndirectResult = params.claimNext();
  } else {
    SILType appliedResultTy = origMethodType->getDirectFormalResultsType(
        IGM.getSILModule(), IGM.getMaximalTypeExpansionContext());
    indirectedResultTI =
      &cast<LoadableTypeInfo>(IGM.getTypeInfo(appliedResultTy));
    auto &nativeSchema = indirectedResultTI->nativeReturnValueSchema(IGM);
    if (nativeSchema.requiresIndirect()) {
      indirectedDirectResult = params.claimNext();
    }
  }

  // Translate direct parameters passed indirectly.
  Explosion translatedParams;

  // Add the formal indirect return here.
  if (formalIndirectResult)
    translatedParams.add(formalIndirectResult);

  // We already handled self.
  assert(origMethodType->hasSelfParam());
  auto origParamInfos = origMethodType->getParameters();
  origParamInfos = origParamInfos.drop_back();

  for (auto info : origParamInfos) {
    // Addresses consist of a single pointer argument.
    if (isIndirectFormalParameter(info.getConvention())) {
      translatedParams.add(params.claimNext());
      continue;
    }
    // Otherwise, we have a loadable type that can either be passed directly or
    // indirectly.
    assert(info.getSILStorageType(IGM.getSILModule(), origMethodType,
                                  IGM.getMaximalTypeExpansionContext())
               .isObject());
    auto curSILType =
        info.getSILStorageType(IGM.getSILModule(), origMethodType,
                               IGM.getMaximalTypeExpansionContext());
    auto &ti = cast<LoadableTypeInfo>(IGM.getTypeInfo(curSILType));

    // Load the indirectly passed parameter.
    auto &nativeSchema = ti.nativeParameterValueSchema(IGM);
    if (nativeSchema.requiresIndirect()) {
      Address paramAddr = ti.getAddressForPointer(params.claimNext());
      ti.loadAsTake(subIGF, paramAddr, translatedParams);
      continue;
    }
    // Map from the native calling convention into the explosion schema.
    auto &nativeParamSchema = ti.nativeParameterValueSchema(IGM);
    Explosion nativeParam;
    params.transferInto(nativeParam, nativeParamSchema.size());
    Explosion nonNativeParam = nativeParamSchema.mapFromNative(
        subIGF.IGM, subIGF, nativeParam, curSILType);
    assert(nativeParam.empty());

    // Pass along the value.
    ti.reexplode(nonNativeParam, translatedParams);
  }

  // Prepare the call to the underlying method.
  auto emission = getCallEmission(
      subIGF, self,
      getObjCMethodCallee(subIGF, method, self,
                          CalleeInfo(origMethodType, origMethodType, {})));
  emission->begin();

  emission->setArgs(translatedParams, false, /*witnessMetadata*/ nullptr);

  // Cleanup that always has to occur after the function call.
  auto cleanup = [&]{
    // Lifetime-extend 'self' by sending it to the autorelease pool if need be.
    if (lifetimeExtendsSelf) {
      subIGF.emitObjCRetainCall(self);
      subIGF.emitObjCAutoreleaseCall(self);
    }
    // Release the context.
    if (!resultType->isCalleeGuaranteed())
      subIGF.emitNativeStrongRelease(context, subIGF.getDefaultAtomicity());
  };
  
   // Emit the call and produce the return value.
  if (indirectedDirectResult) {
    Address addr =
      indirectedResultTI->getAddressForPointer(indirectedDirectResult);
    emission->emitToMemory(addr, *indirectedResultTI, false);
    emission->end();
    cleanup();
    subIGF.Builder.CreateRetVoid();
  } else {
    Explosion result;
    emission->emitToExplosion(result, false);
    emission->end();
    cleanup();
    auto &callee = emission->getCallee();
    auto resultType = callee.getOrigFunctionType()->getDirectFormalResultsType(
        IGM.getSILModule(), IGM.getMaximalTypeExpansionContext());
    subIGF.emitScalarReturn(resultType, resultType, result,
                            true /*isSwiftCCReturn*/, false);
  }
  
  return fwd;
}

void irgen::emitObjCPartialApplication(IRGenFunction &IGF,
                                       ObjCMethod method,
                                       CanSILFunctionType origMethodType,
                                       CanSILFunctionType resultType,
                                       llvm::Value *self,
                                       SILType selfType,
                                       Explosion &out) {
  // Create a heap object to contain the self argument.
  // TODO: If function context arguments were given objc retain counts,
  // we wouldn't need to create a separate heap object here.
  auto *selfTypeInfo = &IGF.getTypeInfo(selfType);
  HeapLayout layout(IGF.IGM, LayoutStrategy::Optimal,
                    selfType, selfTypeInfo);

  // FIXME: Either emit a descriptor for this or create a metadata kind
  // that indicates its trivial layout.
  auto Descriptor
    = llvm::ConstantPointerNull::get(IGF.IGM.CaptureDescriptorPtrTy);
  llvm::Value *data = IGF.emitUnmanagedAlloc(layout, "closure",
                                             Descriptor);
  // FIXME: non-fixed offsets
  NonFixedOffsets offsets = llvm::None;
  Address dataAddr = layout.emitCastTo(IGF, data);
  auto &fieldLayout = layout.getElement(0);
  auto &fieldType = layout.getElementTypes()[0];
  Address fieldAddr = fieldLayout.project(IGF, dataAddr, offsets);
  Explosion selfParams;
  selfParams.add(self);
  fieldLayout.getType().initializeFromParams(IGF, selfParams, fieldAddr,
                                             fieldType, false);

  // Create the forwarding stub.
  llvm::Value *forwarder = emitObjCPartialApplicationForwarder(IGF.IGM,
                                                                method,
                                                                origMethodType,
                                                                resultType,
                                                                layout,
                                                                selfType);
  forwarder =
    IGF.IGM.getConstantSignedFunctionPointer(cast<llvm::Constant>(forwarder),
                                             resultType);
  forwarder = IGF.Builder.CreateBitCast(forwarder, IGF.IGM.Int8PtrTy);

  // Emit the result explosion.
  out.add(forwarder);
  out.add(data);
}

/// Create the LLVM function declaration for a thunk that acts like
/// an Objective-C method for a Swift method implementation.
static llvm::Constant *findSwiftAsObjCThunk(IRGenModule &IGM, SILDeclRef ref,
                                            SILFunction *&SILFn) {
  SILFn = IGM.getSILModule().lookUpFunction(ref);
  assert(SILFn && "no IR function for swift-as-objc thunk");
  auto fn = IGM.getAddrOfSILFunction(SILFn, NotForDefinition);
  // Don't add the unnamed_addr attribute: in some places Foundation is
  // comparing ObjC method pointers. Therefore LLVM's function merging pass must
  // not create aliases for identical functions, but create thunks.
  // This can be ensured if ObjC methods are not created with the unnamed_addr
  // attribute.
  return llvm::ConstantExpr::getBitCast(fn, IGM.Int8PtrTy);
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given property's getter method implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCGetterPointer(IRGenModule &IGM,
                                            AbstractStorageDecl *property,
                                            SILFunction *&silFn) {
  // Protocol properties have no impl.
  if (isa<ProtocolDecl>(property->getDeclContext()))
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

  SILDeclRef getter = SILDeclRef(property->getOpaqueAccessor(AccessorKind::Get),
                                 SILDeclRef::Kind::Func)
    .asForeign();

  return findSwiftAsObjCThunk(IGM, getter, silFn);
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given property's setter method implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCSetterPointer(IRGenModule &IGM,
                                            AbstractStorageDecl *property,
                                            SILFunction *&silFn) {
  // Protocol properties have no impl.
  if (isa<ProtocolDecl>(property->getDeclContext()))
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

  assert(property->isSettable(property->getDeclContext()) &&
         "property is not settable?!");
  
  SILDeclRef setter = SILDeclRef(property->getOpaqueAccessor(AccessorKind::Set),
                                 SILDeclRef::Kind::Func)
    .asForeign();
  return findSwiftAsObjCThunk(IGM, setter, silFn);
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given method implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCMethodPointer(IRGenModule &IGM,
                                            FuncDecl *method,
                                            SILFunction *&silFn) {
  // Protocol methods have no impl.
  if (isa<ProtocolDecl>(method->getDeclContext()))
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

  SILDeclRef declRef = SILDeclRef(method, SILDeclRef::Kind::Func)
    .asForeign();

  return findSwiftAsObjCThunk(IGM, declRef, silFn);
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given constructor implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCMethodPointer(IRGenModule &IGM,
                                            ConstructorDecl *constructor,
                                            SILFunction *&silFn) {
  // Protocol methods have no impl.
  if (isa<ProtocolDecl>(constructor->getDeclContext()))
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

  SILDeclRef declRef = SILDeclRef(constructor, SILDeclRef::Kind::Initializer)
    .asForeign();

  return findSwiftAsObjCThunk(IGM, declRef, silFn);
}

/// Produce a function pointer, suitable for invocation by
/// objc_msgSend, for the given destructor implementation.
///
/// Returns a value of type i8*.
static llvm::Constant *getObjCMethodPointer(IRGenModule &IGM,
                                            DestructorDecl *destructor,
                                            SILFunction *&silFn) {
  SILDeclRef declRef = SILDeclRef(destructor, SILDeclRef::Kind::Deallocator)
    .asForeign();

  return findSwiftAsObjCThunk(IGM, declRef, silFn);
}

static SILDeclRef getObjCMethodRef(AbstractFunctionDecl *method) {
  if (isa<ConstructorDecl>(method))
    return SILDeclRef(method, SILDeclRef::Kind::Initializer).asForeign();
  if (isa<DestructorDecl>(method))
    return SILDeclRef(method, SILDeclRef::Kind::Deallocator).asForeign();
  return SILDeclRef(method, SILDeclRef::Kind::Func).asForeign();
}

static CanSILFunctionType getObjCMethodType(IRGenModule &IGM,
                                            AbstractFunctionDecl *method) {
  return IGM.getSILTypes().getConstantFunctionType(
      TypeExpansionContext::minimal(), getObjCMethodRef(method));
}

static clang::CanQualType getObjCPropertyType(IRGenModule &IGM,
                                              VarDecl *property) {
  // Use the lowered return type of the foreign getter.
  auto getter = property->getOpaqueAccessor(AccessorKind::Get);
  CanSILFunctionType methodTy = getObjCMethodType(IGM, getter);
  return IGM.getClangType(
    methodTy->getFormalCSemanticResult(IGM.getSILModule()).getASTType());
}

void irgen::getObjCEncodingForPropertyType(IRGenModule &IGM,
                                           VarDecl *property, std::string &s) {
  // FIXME: Property encoding differs in slight ways that aren't publicly
  // exposed from Clang.
  IGM.getClangASTContext()
    .getObjCEncodingForPropertyType(getObjCPropertyType(IGM, property), s);
}

static void
HelperGetObjCEncodingForType(const clang::ASTContext &Context,
                             clang::CanQualType T,
                             std::string &S, bool Extended) {
  
  Context.getObjCEncodingForMethodParameter(clang::Decl::OBJC_TQ_None,
                                            T, S, Extended);
}

static llvm::Constant *getObjCEncodingForTypes(IRGenModule &IGM,
                                               CanSILFunctionType fnType,
                                               ArrayRef<SILParameterInfo> params,
                                               StringRef fixedParamsString,
                                               Size::int_type parmOffset,
                                               bool useExtendedEncoding) {
  auto resultType = fnType->getFormalCSemanticResult(IGM.getSILModule());
  auto &clangASTContext = IGM.getClangASTContext();
  
  std::string encodingString;

  // Return type.
  {
    auto clangType = IGM.getClangType(resultType.getASTType());
    if (clangType.isNull())
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    HelperGetObjCEncodingForType(clangASTContext, clangType, encodingString,
                                 useExtendedEncoding);
  }

  // Parameter types.
  // TODO. Encode type qualifier, 'in', 'inout', etc. for the parameter.
  std::string paramsString;
  for (auto param : params) {
    auto clangType = IGM.getClangType(param.getArgumentType(
        IGM.getSILModule(), fnType, IGM.getMaximalTypeExpansionContext()));
    if (clangType.isNull())
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    
    // TODO. Some stuff related to Array and Function type is missing.
    // TODO. Encode type qualifier, 'in', 'inout', etc. for the parameter.
    HelperGetObjCEncodingForType(clangASTContext, clangType, paramsString,
                                 useExtendedEncoding);
    paramsString += llvm::itostr(parmOffset);
    clang::CharUnits sz = clangASTContext.getObjCEncodingTypeSize(clangType);
    parmOffset += sz.getQuantity();
  }
  
  encodingString += llvm::itostr(parmOffset);
  encodingString += fixedParamsString;
  encodingString += paramsString;
  return IGM.getAddrOfGlobalString(encodingString);
}

static llvm::Constant *
getObjectEncodingFromClangNode(IRGenModule &IGM, Decl *d,
                               bool useExtendedEncoding) {
  // Use the clang node's encoding if there is a clang node.
  if (d->getClangNode()) {
    auto clangDecl = d->getClangNode().castAsDecl();
    auto &clangASTContext = IGM.getClangASTContext();
    std::string typeStr;
    if (auto objcMethodDecl = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
      typeStr = clangASTContext.getObjCEncodingForMethodDecl(
          objcMethodDecl, useExtendedEncoding /*extended*/);
    }
    if (auto objcPropertyDecl = dyn_cast<clang::ObjCPropertyDecl>(clangDecl)) {
      typeStr = clangASTContext.getObjCEncodingForPropertyDecl(objcPropertyDecl,
                                                               nullptr);
    }
    if (!typeStr.empty()) {
      return IGM.getAddrOfGlobalString(typeStr.c_str());
    }
  }
  return nullptr;
}

static llvm::Constant *getObjCEncodingForMethod(IRGenModule &IGM,
                                                CanSILFunctionType fnType,
                                                bool useExtendedEncoding,
                                                Decl *optionalDecl) {
  // Use the decl's ClangNode to get the encoding if possible.
  if (optionalDecl) {
    if (auto *enc = getObjectEncodingFromClangNode(IGM, optionalDecl,
                                                   useExtendedEncoding)) {
      return enc;
    }
  }

  // Get the inputs without 'self'.
  auto inputs = fnType->getParameters().drop_back();

  // Include the encoding for 'self' and '_cmd'.
  llvm::SmallString<8> specialParams;
  specialParams += "@0:";
  auto ptrSize = IGM.getPointerSize().getValue();
  specialParams += llvm::itostr(ptrSize);
  GenericContextScope scope(IGM, fnType->getInvocationGenericSignature());
  return getObjCEncodingForTypes(IGM, fnType, inputs, specialParams,
                                 ptrSize * 2, useExtendedEncoding);
}

/// Emit the components of an Objective-C method descriptor: its selector,
/// type encoding, and IMP pointer.
ObjCMethodDescriptor
irgen::emitObjCMethodDescriptorParts(IRGenModule &IGM,
                                     AbstractFunctionDecl *method,
                                     bool concrete) {
  ObjCMethodDescriptor descriptor{};
  Selector selector(method);
  
  /// The first element is the selector.
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(selector.str());
  
  /// The second element is the method signature. A method signature is made
  /// of the return type @encoding and every parameter type @encoding, glued
  /// with numbers that used to represent stack offsets for each of these
  /// elements.
  CanSILFunctionType methodType = getObjCMethodType(IGM, method);
  
  bool useExtendedEncoding =
    method->hasAsync() && !isa<ProtocolDecl>(method->getDeclContext());
  descriptor.typeEncoding = getObjCEncodingForMethod(
      IGM, methodType, /*extended*/ useExtendedEncoding, method);
  /// The third element is the method implementation pointer.
  if (!concrete) {
    descriptor.impl = nullptr;
    descriptor.silFunction = nullptr;
    return descriptor;
  }
  descriptor.silFunction = nullptr;

  if (auto func = dyn_cast<FuncDecl>(method))
    descriptor.impl = getObjCMethodPointer(IGM, func, descriptor.silFunction);
  else if (auto ctor = dyn_cast<ConstructorDecl>(method))
    descriptor.impl = getObjCMethodPointer(IGM, ctor, descriptor.silFunction);
  else
    descriptor.impl = getObjCMethodPointer(IGM, cast<DestructorDecl>(method),
                                           descriptor.silFunction);
  return descriptor;
}

/// Emit the components of an Objective-C method descriptor for a
/// property getter method.
ObjCMethodDescriptor
irgen::emitObjCGetterDescriptorParts(IRGenModule &IGM, VarDecl *property) {
  Selector getterSel(property, Selector::ForGetter);
  ObjCMethodDescriptor descriptor{};
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(getterSel.str());
  
  auto clangType = getObjCPropertyType(IGM, property);
  if (clangType.isNull()) {
    descriptor.typeEncoding = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    descriptor.silFunction = nullptr;
    return descriptor;
  }

  auto &clangASTContext = IGM.getClangASTContext();
  std::string TypeStr;
  clangASTContext.getObjCEncodingForType(clangType, TypeStr);
  
  Size PtrSize = IGM.getPointerSize();
  Size::int_type ParmOffset = 2 * PtrSize.getValue();
  
  TypeStr += llvm::itostr(ParmOffset);
  TypeStr += "@0:";
  TypeStr += llvm::itostr(PtrSize.getValue());
  descriptor.typeEncoding = IGM.getAddrOfGlobalString(TypeStr.c_str());
  descriptor.silFunction = nullptr;
  descriptor.impl = getObjCGetterPointer(IGM, property, descriptor.silFunction);
  return descriptor;
}

/// Emit the components of an Objective-C method descriptor for a
/// subscript getter method.
ObjCMethodDescriptor
irgen::emitObjCGetterDescriptorParts(IRGenModule &IGM,
                                     SubscriptDecl *subscript) {
  Selector getterSel(subscript, Selector::ForGetter);
  ObjCMethodDescriptor descriptor{};
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(getterSel.str());

  auto methodTy =
      getObjCMethodType(IGM, subscript->getOpaqueAccessor(AccessorKind::Get));
  descriptor.typeEncoding =
      getObjCEncodingForMethod(IGM, methodTy,
                               /*extended*/ false, subscript);

  descriptor.silFunction = nullptr;
  descriptor.impl = getObjCGetterPointer(IGM, subscript,
                                         descriptor.silFunction);
  return descriptor;
}

ObjCMethodDescriptor
irgen::emitObjCGetterDescriptorParts(IRGenModule &IGM,
                                     AbstractStorageDecl *decl) {
  if (auto sub = dyn_cast<SubscriptDecl>(decl)) {
    return emitObjCGetterDescriptorParts(IGM, sub);
  }
  if (auto var = dyn_cast<VarDecl>(decl)) {
    return emitObjCGetterDescriptorParts(IGM, var);
  }
  llvm_unreachable("unknown storage!");
}

/// Emit the components of an Objective-C method descriptor for a
/// property getter method.
ObjCMethodDescriptor
irgen::emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                     VarDecl *property) {
  // Optional properties support mutation on the Objective-C side, but not the
  // Swift side.
  assert((property->getAttrs().hasAttribute<OptionalAttr>() ||
          property->isSettable(property->getDeclContext())) &&
         "not a settable property?!");

  Selector setterSel(property, Selector::ForSetter);
  ObjCMethodDescriptor descriptor{};
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(setterSel.str());
  
  auto &clangASTContext = IGM.getClangASTContext();
  std::string TypeStr;
  auto clangType = clangASTContext.VoidTy;
  clangASTContext.getObjCEncodingForType(clangType, TypeStr);
  
  Size PtrSize = IGM.getPointerSize();
  Size::int_type ParmOffset = 2 * PtrSize.getValue();

  clangType = getObjCPropertyType(IGM, property);
  if (clangType.isNull()) {
    descriptor.typeEncoding = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    descriptor.silFunction = nullptr;
    return descriptor;
  }
  clang::CharUnits sz = clangASTContext.getObjCEncodingTypeSize(clangType);
  if (!sz.isZero())
    ParmOffset += sz.getQuantity();
  TypeStr += llvm::itostr(ParmOffset);
  TypeStr += "@0:";
  TypeStr += llvm::itostr(PtrSize.getValue());
  ParmOffset = 2 * PtrSize.getValue();
  clangASTContext.getObjCEncodingForType(clangType, TypeStr);
  TypeStr += llvm::itostr(ParmOffset);
  descriptor.typeEncoding = IGM.getAddrOfGlobalString(TypeStr.c_str());
  descriptor.silFunction = nullptr;
  descriptor.impl = getObjCSetterPointer(IGM, property, descriptor.silFunction);
  return descriptor;
}

/// Emit the components of an Objective-C method descriptor for a
/// subscript getter method.
ObjCMethodDescriptor
irgen::emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                     SubscriptDecl *subscript) {
  // Optional subscripts support mutation on the Objective-C side, but not the
  // Swift side.
  assert((subscript->getAttrs().hasAttribute<OptionalAttr>() ||
          subscript->supportsMutation()) &&
         "not a settable subscript?!");

  Selector setterSel(subscript, Selector::ForSetter);
  ObjCMethodDescriptor descriptor{};
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(setterSel.str());
  auto methodTy = getObjCMethodType(IGM,
                              subscript->getOpaqueAccessor(AccessorKind::Set));
  descriptor.typeEncoding =
      getObjCEncodingForMethod(IGM, methodTy,
                               /*extended*/ false, subscript);
  descriptor.silFunction = nullptr;
  descriptor.impl = getObjCSetterPointer(IGM, subscript,
                                         descriptor.silFunction);
  return descriptor;
}

ObjCMethodDescriptor
irgen::emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                     AbstractStorageDecl *decl) {
  if (auto sub = dyn_cast<SubscriptDecl>(decl)) {
    return emitObjCSetterDescriptorParts(IGM, sub);
  }
  if (auto var = dyn_cast<VarDecl>(decl)) {
    return emitObjCSetterDescriptorParts(IGM, var);
  }
  llvm_unreachable("unknown storage!");
}

static void buildMethodDescriptor(IRGenModule &IGM,
                                  ConstantArrayBuilder &descriptors,
                                  ObjCMethodDescriptor &parts) {
  auto descriptor = descriptors.beginStruct();
  descriptor.add(parts.selectorRef);
  descriptor.add(parts.typeEncoding);
  if (parts.impl->isNullValue()) {
    descriptor.add(parts.impl);
  } else {
    descriptor.addSignedPointer(parts.impl,
               IGM.getOptions().PointerAuth.ObjCMethodListFunctionPointers,
                                PointerAuthEntity());
  }
  descriptor.finishAndAddTo(descriptors);
}

static void emitObjCDescriptor(IRGenModule &IGM,
                               ConstantArrayBuilder &descriptors,
                               ObjCMethodDescriptor &descriptor) {
  buildMethodDescriptor(IGM, descriptors, descriptor);
  auto *silFn = descriptor.silFunction;
  if (silFn && silFn->hasObjCReplacement()) {
    auto replacedSelector =
      IGM.getAddrOfObjCMethodName(silFn->getObjCReplacement().str());
    descriptor.selectorRef = replacedSelector;
    buildMethodDescriptor(IGM, descriptors, descriptor);
  }
}

/// Emit an Objective-C method descriptor for the given method.
/// struct method_t {
///   SEL name;
///   const char *types;
///   IMP imp;
/// };
void irgen::emitObjCMethodDescriptor(IRGenModule &IGM,
                                     ConstantArrayBuilder &descriptors,
                                     AbstractFunctionDecl *method) {
  ObjCMethodDescriptor descriptor(
    emitObjCMethodDescriptorParts(IGM, method, /*concrete*/ true));
  emitObjCDescriptor(IGM, descriptors, descriptor);
}

void irgen::emitObjCIVarInitDestroyDescriptor(IRGenModule &IGM,
                                              ConstantArrayBuilder &descriptors,
                                              ClassDecl *cd,
                                              llvm::Function *objcImpl,
                                              bool isDestroyer) {
  /// The first element is the selector.
  SILDeclRef declRef = SILDeclRef(cd, 
                                  isDestroyer? SILDeclRef::Kind::IVarDestroyer
                                             : SILDeclRef::Kind::IVarInitializer,
                                  /*foreign*/ true);
  Selector selector(declRef);
  ObjCMethodDescriptor descriptor{};
  descriptor.selectorRef = IGM.getAddrOfObjCMethodName(selector.str());
  
  /// The second element is the method signature. A method signature is made of
  /// the return type @encoding and every parameter type @encoding, glued with
  /// numbers that used to represent stack offsets for each of these elements.
  auto ptrSize = IGM.getPointerSize().getValue();
  llvm::SmallString<8> signature;
  signature = "v" + llvm::itostr(ptrSize * 2) + "@0:" + llvm::itostr(ptrSize);
  descriptor.typeEncoding = IGM.getAddrOfGlobalString(signature);

  /// The third element is the method implementation pointer.
  descriptor.impl = llvm::ConstantExpr::getBitCast(objcImpl, IGM.Int8PtrTy);

  // Form the method_t instance.
  buildMethodDescriptor(IGM, descriptors, descriptor);
}


llvm::Constant *
irgen::getMethodTypeExtendedEncoding(IRGenModule &IGM,
                                     AbstractFunctionDecl *method) {
  CanSILFunctionType methodType = getObjCMethodType(IGM, method);
  return getObjCEncodingForMethod(IGM, methodType, true /*Extended*/, method);
}

llvm::Constant *
irgen::getBlockTypeExtendedEncoding(IRGenModule &IGM,
                                    CanSILFunctionType invokeTy) {
  // Skip the storage pointer, which is encoded as '@?' to avoid the infinite
  // recursion of the usual '@?<...>' rule for blocks.
  auto paramTypes = invokeTy->getParameters().slice(1);
  
  return getObjCEncodingForTypes(IGM, invokeTy, paramTypes,
                                 "@?0", IGM.getPointerSize().getValue(),
                                 /*extended*/ true);
}

void irgen::emitObjCGetterDescriptor(IRGenModule &IGM,
                                     ConstantArrayBuilder &descriptors,
                                     AbstractStorageDecl *storage) {
  ObjCMethodDescriptor descriptor(emitObjCGetterDescriptorParts(IGM, storage));
  emitObjCDescriptor(IGM, descriptors, descriptor);
}

void irgen::emitObjCSetterDescriptor(IRGenModule &IGM,
                                     ConstantArrayBuilder &descriptors,
                                     AbstractStorageDecl *storage) {
  ObjCMethodDescriptor descriptor(emitObjCSetterDescriptorParts(IGM, storage));
  emitObjCDescriptor(IGM, descriptors, descriptor);
}

static bool isObjCGenericClassExtension(ValueDecl *decl) {
  // Don't emit category entries for @objc methods in extensions they would
  // normally be disallowed except for @_dynamicReplacement(for:) methods that
  // use the native dynamic replacement mechanism instead of objc categories.
  auto *DC = decl->getDeclContext();
  if (!isa<ExtensionDecl>(DC))
    return false;
  return decl->isNativeMethodReplacement();
}

bool irgen::requiresObjCMethodDescriptor(FuncDecl *method) {
  // Property accessors should be generated alongside the property.
  if (isa<AccessorDecl>(method))
    return false;

  return method->isObjC() && !isObjCGenericClassExtension(method);
}

bool irgen::requiresObjCMethodDescriptor(ConstructorDecl *constructor) {
  return constructor->isObjC();
}

bool irgen::requiresObjCPropertyDescriptor(IRGenModule &IGM,
                                           VarDecl *property) {
  // Don't generate a descriptor for a property without any accessors.
  // This is only possible in SIL files because Sema will normally
  // implicitly synthesize accessors for @objc properties.
  return property->isObjC() && property->requiresOpaqueAccessors() &&
         !isObjCGenericClassExtension(property);
}

bool irgen::requiresObjCSubscriptDescriptor(IRGenModule &IGM,
                                            SubscriptDecl *subscript) {
  return subscript->isObjC() && !isObjCGenericClassExtension(subscript);
}

llvm::Value *IRGenFunction::emitBlockCopyCall(llvm::Value *value) {
  // Get an appropriately-cast function pointer.
  auto fn = IGM.getBlockCopyFn();
  auto fnType = IGM.getBlockCopyFnType();

  if (value->getType() != IGM.ObjCBlockPtrTy) {
    fnType = llvm::FunctionType::get(value->getType(), value->getType(), false);
    fn = llvm::ConstantExpr::getBitCast(fn, fnType->getPointerTo());
  }

  auto call = Builder.CreateCall(fnType, fn, value);
  return call;
}

void IRGenFunction::emitBlockRelease(llvm::Value *value) {
  // Get an appropriately-cast function pointer.
  auto fn = IGM.getBlockReleaseFn();
  auto fnType = IGM.getBlockReleaseFnType();
  if (value->getType() != IGM.ObjCBlockPtrTy) {
    fnType = llvm::FunctionType::get(IGM.VoidTy, value->getType(), false);
    fn = llvm::ConstantExpr::getBitCast(fn, fnType->getPointerTo());
  }
  auto call = Builder.CreateCall(fnType, fn, value);
  call->setDoesNotThrow();
}

void IRGenFunction::emitForeignReferenceTypeLifetimeOperation(
    ValueDecl *fn, llvm::Value *value) {
  assert(fn->getClangDecl() && isa<clang::FunctionDecl>(fn->getClangDecl()));

  auto clangFn = cast<clang::FunctionDecl>(fn->getClangDecl());
  auto llvmFn = cast<llvm::Function>(
      IGM.getAddrOfClangGlobalDecl(clangFn, ForDefinition));

  auto argType =
      cast<llvm::FunctionType>(llvmFn->getFunctionType())->getParamType(0);
  value = Builder.CreateBitCast(value, argType);

  auto call = Builder.CreateCall(llvmFn->getFunctionType(), llvmFn, value);
  call->setDoesNotThrow();
}
