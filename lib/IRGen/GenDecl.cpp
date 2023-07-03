//===--- GenDecl.cpp - IR Generation for Declarations ---------------------===//
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
//  This file implements IR generation for local and global
//  declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/IRGen/Linking.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "Callee.h"
#include "ClassTypeInfo.h"
#include "ConformanceDescription.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenCall.h"
#include "GenConstant.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenOpaque.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "MetadataRequest.h"
#include "ProtocolInfo.h"
#include "Signature.h"
#include "StructLayout.h"

using namespace swift;
using namespace irgen;

llvm::cl::opt<bool> UseBasicDynamicReplacement(
    "basic-dynamic-replacement", llvm::cl::init(false),
    llvm::cl::desc("Basic implementation of dynamic replacement"));

namespace {

/// Add methods, properties, and protocol conformances from a JITed extension
/// to an ObjC class using the ObjC runtime.
///
/// This must happen after ObjCProtocolInitializerVisitor if any @objc protocols
/// were defined in the TU.
class CategoryInitializerVisitor
  : public ClassMemberVisitor<CategoryInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;

  FunctionPointer class_replaceMethod;
  FunctionPointer class_addProtocol;

  llvm::Value *classMetadata;
  llvm::Constant *metaclassMetadata;
  
public:
  CategoryInitializerVisitor(IRGenFunction &IGF, ExtensionDecl *ext)
    : IGF(IGF)
  {
    class_replaceMethod = IGM.getClassReplaceMethodFunctionPointer();
    class_addProtocol = IGM.getClassAddProtocolFunctionPointer();

    CanType origTy = ext->getSelfNominalTypeDecl()
        ->getDeclaredType()->getCanonicalType();
    classMetadata = emitClassHeapMetadataRef(IGF, origTy,
                                             MetadataValueType::ObjCClass,
                                             MetadataState::Complete,
                                             /*allow uninitialized*/ false);
    classMetadata = Builder.CreateBitCast(classMetadata, IGM.ObjCClassPtrTy);
    metaclassMetadata = IGM.getAddrOfMetaclassObject(
                          dyn_cast_or_null<ClassDecl>(origTy->getAnyNominal()),
                          NotForDefinition);
    metaclassMetadata = llvm::ConstantExpr::getBitCast(metaclassMetadata,
                                                   IGM.ObjCClassPtrTy);

    // Register ObjC protocol conformances.
    for (auto *p : ext->getLocalProtocols()) {
      if (!p->isObjC())
        continue;

      auto protoRefAddr = IGM.getAddrOfObjCProtocolRef(p, NotForDefinition);
      auto proto = Builder.CreateLoad(protoRefAddr);
      Builder.CreateCall(class_addProtocol, {classMetadata, proto});
    }
  }
  
  void visitMembers(ExtensionDecl *ext) {
    for (Decl *member : ext->getMembers())
      visit(member);
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingDecl(MissingDecl *missing) {
    llvm_unreachable("missing decl in IRGen");
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}

  void visitFuncDecl(FuncDecl *method) {
    if (!requiresObjCMethodDescriptor(method)) return;

    // Don't emit getters/setters for @NSManaged methods.
    if (method->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    auto descriptor = emitObjCMethodDescriptorParts(IGM, method,
                                                    /*concrete*/true);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);

    llvm::Value *args[] = {
      method->isStatic() ? metaclassMetadata : classMetadata,
      sel,
      descriptor.impl,
      descriptor.typeEncoding
    };
    
    Builder.CreateCall(class_replaceMethod, args);
  }

  // Can't be added in an extension.
  void visitDestructorDecl(DestructorDecl *dtor) {}

  void visitConstructorDecl(ConstructorDecl *constructor) {
    if (!requiresObjCMethodDescriptor(constructor)) return;
    auto descriptor = emitObjCMethodDescriptorParts(IGM, constructor,
                                                    /*concrete*/true);

    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);

    llvm::Value *args[] = {
      classMetadata,
      sel,
      descriptor.impl,
      descriptor.typeEncoding
    };

    Builder.CreateCall(class_replaceMethod, args);
  }

  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitVarDecl(VarDecl *prop) {
    if (!requiresObjCPropertyDescriptor(IGM, prop)) return;

    // FIXME: register property metadata in addition to the methods.
    // ObjC doesn't have a notion of class properties, so we'd only do this
    // for instance properties.

    // Don't emit getters/setters for @NSManaged properties.
    if (prop->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    auto descriptor = emitObjCGetterDescriptorParts(IGM, prop);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);
    auto theClass = prop->isStatic() ? metaclassMetadata : classMetadata;
    llvm::Value *getterArgs[] =
      {theClass, sel, descriptor.impl, descriptor.typeEncoding};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (prop->isSettable(prop->getDeclContext())) {
      auto descriptor = emitObjCSetterDescriptorParts(IGM, prop);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFunctionPointer(),
                               descriptor.selectorRef);
      llvm::Value *setterArgs[] =
        {theClass, sel, descriptor.impl, descriptor.typeEncoding};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *subscript) {
    assert(!subscript->isStatic() && "objc doesn't support class subscripts");
    if (!requiresObjCSubscriptDescriptor(IGM, subscript)) return;
    
    auto descriptor = emitObjCGetterDescriptorParts(IGM, subscript);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);
    llvm::Value *getterArgs[] =
      {classMetadata, sel, descriptor.impl, descriptor.typeEncoding};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (subscript->supportsMutation()) {
      auto descriptor = emitObjCSetterDescriptorParts(IGM, subscript);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFunctionPointer(),
                               descriptor.selectorRef);
      llvm::Value *setterArgs[] =
        {classMetadata, sel, descriptor.impl, descriptor.typeEncoding};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }
};

/// Create a descriptor for JITed @objc protocol using the ObjC runtime.
class ObjCProtocolInitializerVisitor
  : public ClassMemberVisitor<ObjCProtocolInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;

  FunctionPointer objc_getProtocol, objc_allocateProtocol,
      objc_registerProtocol, protocol_addMethodDescription,
      protocol_addProtocol;

  llvm::Value *NewProto = nullptr;
  
public:
  ObjCProtocolInitializerVisitor(IRGenFunction &IGF)
    : IGF(IGF)
  {
    objc_getProtocol = IGM.getGetObjCProtocolFunctionPointer();
    objc_allocateProtocol = IGM.getAllocateObjCProtocolFunctionPointer();
    objc_registerProtocol = IGM.getRegisterObjCProtocolFunctionPointer();
    protocol_addMethodDescription =
        IGM.getProtocolAddMethodDescriptionFunctionPointer();
    protocol_addProtocol = IGM.getProtocolAddProtocolFunctionPointer();
  }
  
  void visitMembers(ProtocolDecl *proto) {
    // Check if the ObjC runtime already has a descriptor for this
    // protocol. If so, use it.
    SmallString<32> buf;
    auto protocolName
      = IGM.getAddrOfGlobalString(proto->getObjCRuntimeName(buf));
    
    auto existing = Builder.CreateCall(objc_getProtocol, protocolName);
    auto isNull = Builder.CreateICmpEQ(existing,
                   llvm::ConstantPointerNull::get(IGM.ProtocolDescriptorPtrTy));

    auto existingBB = IGF.createBasicBlock("existing_protocol");
    auto newBB = IGF.createBasicBlock("new_protocol");
    auto contBB = IGF.createBasicBlock("cont");
    Builder.CreateCondBr(isNull, newBB, existingBB);
    
    // Nothing to do if there's already a descriptor.
    Builder.emitBlock(existingBB);
    Builder.CreateBr(contBB);
    
    Builder.emitBlock(newBB);
    
    // Allocate the protocol descriptor.
    NewProto = Builder.CreateCall(objc_allocateProtocol, protocolName);
    
    // Add the parent protocols.
    for (auto parentProto : proto->getInheritedProtocols()) {
      if (!parentProto->isObjC())
        continue;
      auto parentAddr =
          IGM.getAddrOfObjCProtocolRef(parentProto, NotForDefinition);
      llvm::Value *parent = Builder.CreateLoad(parentAddr);
      parent = IGF.Builder.CreateBitCast(parent, IGM.ProtocolDescriptorPtrTy);
      Builder.CreateCall(protocol_addProtocol, {NewProto, parent});
    }
    
    // Add the members.
    for (Decl *member : proto->getMembers())
      visit(member);
    
    // Register it.
    Builder.CreateCall(objc_registerProtocol, NewProto);
    Builder.CreateBr(contBB);
    
    // Store the reference to the runtime's idea of the protocol descriptor.
    Builder.emitBlock(contBB);
    auto result = Builder.CreatePHI(IGM.ProtocolDescriptorPtrTy, 2);
    result->addIncoming(existing, existingBB);
    result->addIncoming(NewProto, newBB);

    llvm::Value *ref =
        IGM.getAddrOfObjCProtocolRef(proto, NotForDefinition).getAddress();
    ref = IGF.Builder.CreateBitCast(ref,
                                  IGM.ProtocolDescriptorPtrTy->getPointerTo());

    Builder.CreateStore(result, ref, IGM.getPointerAlignment());
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingDecl(MissingDecl *missing) {
    llvm_unreachable("missing decl in IRGen");
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}

  void visitAbstractFunctionDecl(AbstractFunctionDecl *method) {
    if (isa<AccessorDecl>(method)) {
      // Accessors are handled as part of their AbstractStorageDecls.
      return;
    }

    auto descriptor = emitObjCMethodDescriptorParts(IGM, method,
                                                    /*concrete*/false);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);

    llvm::Value *args[] = {
      NewProto, sel, descriptor.typeEncoding,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !method->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                   isa<ConstructorDecl>(method) || method->isInstanceMember()),
    };
    
    Builder.CreateCall(protocol_addMethodDescription, args);
  }
  
  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitAbstractStorageDecl(AbstractStorageDecl *prop) {
    // TODO: Add properties to protocol.
    
    auto descriptor = emitObjCGetterDescriptorParts(IGM, prop);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(
        IGM.getObjCSelRegisterNameFunctionPointer(), descriptor.selectorRef);
    llvm::Value *getterArgs[] = {
      NewProto, sel, descriptor.typeEncoding,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !prop->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             prop->isInstanceMember()),
    };
    Builder.CreateCall(protocol_addMethodDescription, getterArgs);
    
    if (prop->isSettable(nullptr)) {
      auto descriptor = emitObjCSetterDescriptorParts(IGM, prop);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFunctionPointer(),
                               descriptor.selectorRef);
      llvm::Value *setterArgs[] = {
        NewProto, sel, descriptor.typeEncoding,
        // required?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               !prop->getAttrs().hasAttribute<OptionalAttr>()),
        // instance?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               prop->isInstanceMember()),
      };
      Builder.CreateCall(protocol_addMethodDescription, setterArgs);
    }
  }
};

} // end anonymous namespace

namespace {

class PrettySourceFileEmission : public llvm::PrettyStackTraceEntry {
  const SourceFile &SF;
public:
  explicit PrettySourceFileEmission(const SourceFile &SF) : SF(SF) {}

  void print(raw_ostream &os) const override {
    os << "While emitting IR for source file " << SF.getFilename() << '\n';
  }
};

class PrettySynthesizedFileUnitEmission : public llvm::PrettyStackTraceEntry {
  const SynthesizedFileUnit &SFU;

public:
  explicit PrettySynthesizedFileUnitEmission(const SynthesizedFileUnit &SFU)
      : SFU(SFU) {}

  void print(raw_ostream &os) const override {
    os << "While emitting IR for synthesized file" << &SFU << "\n";
  }
};

} // end anonymous namespace

/// Emit all the top-level code in the source file.
void IRGenModule::emitSourceFile(SourceFile &SF) {
  // Type-check the file if we haven't already (this may be necessary for .sil
  // files, which don't get fully type-checked by parsing).
  performTypeChecking(SF);

  PrettySourceFileEmission StackEntry(SF);

  // Emit types and other global decls.
  for (auto *decl : SF.getTopLevelDecls())
    emitGlobalDecl(decl);
  for (auto *decl : SF.getHoistedDecls())
    emitGlobalDecl(decl);
  for (auto *localDecl : SF.LocalTypeDecls)
    emitGlobalDecl(localDecl);
  for (auto *opaqueDecl : SF.getOpaqueReturnTypeDecls())
    maybeEmitOpaqueTypeDecl(opaqueDecl);

  SF.collectLinkLibraries([this](LinkLibrary linkLib) {
    this->addLinkLibrary(linkLib);
  });

  if (ObjCInterop)
    this->addLinkLibrary(LinkLibrary("objc", LibraryKind::Library));

  // If C++ interop is enabled, add -lc++ on Darwin and -lstdc++ on linux.
  // Also link with C++ bridging utility module (Cxx) and C++ stdlib overlay
  // (std) if available.
  if (Context.LangOpts.EnableCXXInterop) {
    const llvm::Triple &target = Context.LangOpts.Target;
    if (target.isOSDarwin())
      this->addLinkLibrary(LinkLibrary("c++", LibraryKind::Library));
    else if (target.isOSLinux())
      this->addLinkLibrary(LinkLibrary("stdc++", LibraryKind::Library));

    // Do not try to link Cxx with itself.
    if (!getSwiftModule()->getName().is("Cxx"))
      this->addLinkLibrary(LinkLibrary("swiftCxx", LibraryKind::Library));

    // Only link with CxxStdlib on platforms where the overlay is available.
    // Do not try to link CxxStdlib with itself.
    if ((target.isOSDarwin() || (target.isOSLinux() && !target.isAndroid())) &&
        !getSwiftModule()->getName().is("Cxx") &&
        !getSwiftModule()->getName().is("CxxStdlib") &&
        !getSwiftModule()->getName().is("std")) {
      this->addLinkLibrary(LinkLibrary("swiftCxxStdlib", LibraryKind::Library));
    }
  }

  // FIXME: It'd be better to have the driver invocation or build system that
  // executes the linker introduce these compatibility libraries, since at
  // that point we know whether we're building an executable, which is the only
  // place where the compatibility libraries take effect. For the benefit of
  // build systems that build Swift code, but don't use Swift to drive
  // the linker, we can also use autolinking to pull in the compatibility
  // libraries. This may however cause the library to get pulled in in
  // situations where it isn't useful, such as for dylibs, though this is
  // harmless aside from code size.
  if (!IRGen.Opts.UseJIT) {
    auto addBackDeployLib = [&](llvm::VersionTuple version,
                                StringRef libraryName, bool forceLoad) {
      llvm::Optional<llvm::VersionTuple> compatibilityVersion;
      if (libraryName == "swiftCompatibilityDynamicReplacements") {
        compatibilityVersion = IRGen.Opts.
            AutolinkRuntimeCompatibilityDynamicReplacementLibraryVersion;
      } else if (libraryName == "swiftCompatibilityConcurrency") {
        compatibilityVersion =
            IRGen.Opts.AutolinkRuntimeCompatibilityConcurrencyLibraryVersion;
      } else {
        compatibilityVersion = IRGen.Opts.
            AutolinkRuntimeCompatibilityLibraryVersion;
      }

      if (!compatibilityVersion)
        return;

      if (*compatibilityVersion > version)
        return;

      this->addLinkLibrary(LinkLibrary(libraryName,
                                       LibraryKind::Library,
                                       forceLoad));
    };

#define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName, ForceLoad) \
      addBackDeployLib(llvm::VersionTuple Version, LibraryName, ForceLoad);
    #include "swift/Frontend/BackDeploymentLibs.def"
  }
}

/// Emit all the top-level code in the synthesized file unit.
void IRGenModule::emitSynthesizedFileUnit(SynthesizedFileUnit &SFU) {
  PrettySynthesizedFileUnitEmission StackEntry(SFU);
  for (auto *decl : SFU.getTopLevelDecls())
    emitGlobalDecl(decl);
}

/// Collect elements of an already-existing global list with the given
/// \c name into \c list.
///
/// We use this when Clang code generation might populate the list.
static void collectGlobalList(IRGenModule &IGM,
                              SmallVectorImpl<llvm::WeakTrackingVH> &list,
                              StringRef name) {
  if (auto *existing = IGM.Module.getGlobalVariable(name)) {
    auto *globals = cast<llvm::ConstantArray>(existing->getInitializer());
    for (auto &use : globals->operands()) {
      auto *global = use.get();
      list.push_back(global);
    }
    existing->eraseFromParent();
  }

  std::for_each(list.begin(), list.end(),
                [](const llvm::WeakTrackingVH &global) {
    assert(!isa<llvm::GlobalValue>(global) ||
           !cast<llvm::GlobalValue>(global)->isDeclaration() &&
           "all globals in the 'used' list must be definitions");
  });
}

/// Emit a global list, i.e. a global constant array holding all of a
/// list of values.  Generally these lists are for various LLVM
/// metadata or runtime purposes.
static llvm::GlobalVariable *
emitGlobalList(IRGenModule &IGM, ArrayRef<llvm::WeakTrackingVH> handles,
               StringRef name, StringRef section,
               llvm::GlobalValue::LinkageTypes linkage, llvm::Type *eltTy,
               bool isConstant, bool asContiguousArray, bool canBeStrippedByLinker = false) {
  // Do nothing if the list is empty.
  if (handles.empty()) return nullptr;

  // For global lists that actually get linked (as opposed to notional
  // ones like @llvm.used), it's important to set an explicit alignment
  // so that the linker doesn't accidentally put padding in the list.
  Alignment alignment = IGM.getPointerAlignment();

  if (!asContiguousArray) {
    // Emit as individual globals, which is required for conditional runtime
    // records to work.
    for (auto &handle : handles) {
      llvm::Constant *elt = cast<llvm::Constant>(&*handle);
      std::string eltName = name.str() + "_" + elt->getName().str();
      if (elt->getType() != eltTy)
        elt = llvm::ConstantExpr::getBitCast(elt, eltTy);
      auto var = new llvm::GlobalVariable(IGM.Module, eltTy, isConstant,
                                          linkage, elt, eltName);
      var->setSection(section);
      var->setAlignment(llvm::MaybeAlign(alignment.getValue()));
      disableAddressSanitizer(IGM, var);
      if (llvm::GlobalValue::isLocalLinkage(linkage)) {
        if (canBeStrippedByLinker)
          IGM.addCompilerUsedGlobal(var);
        else
          IGM.addUsedGlobal(var);
      }

      if (IGM.IRGen.Opts.ConditionalRuntimeRecords) {
        // Allow dead-stripping `var` (the runtime record from the global list)
        // when `handle` / `elt` (the underlaying entity) is not referenced.
        IGM.appendLLVMUsedConditionalEntry(var, elt->stripPointerCasts());
      }
    }
    return nullptr;
  }

  // We have an array of value handles, but we need an array of constants.
  SmallVector<llvm::Constant*, 8> elts;
  elts.reserve(handles.size());
  for (auto &handle : handles) {
    auto elt = cast<llvm::Constant>(&*handle);
    if (elt->getType() != eltTy)
      elt = llvm::ConstantExpr::getBitCast(elt, eltTy);
    elts.push_back(elt);
  }

  auto varTy = llvm::ArrayType::get(eltTy, elts.size());
  auto init = llvm::ConstantArray::get(varTy, elts);
  auto var = new llvm::GlobalVariable(IGM.Module, varTy, isConstant, linkage,
                                      init, name);
  var->setSection(section);

  // Do not set alignment and don't set disableAddressSanitizer on @llvm.used
  // and @llvm.compiler.used. Doing so confuses LTO (merging) and they're not
  // going to end up as real global symbols in the binary anyways.
  if (name != "llvm.used" && name != "llvm.compiler.used") {
    var->setAlignment(llvm::MaybeAlign(alignment.getValue()));
    disableAddressSanitizer(IGM, var);
  }

  // Mark the variable as used if doesn't have external linkage.
  // (Note that we'd specifically like to not put @llvm.used in itself.)
  if (llvm::GlobalValue::isLocalLinkage(linkage)) {
    if (canBeStrippedByLinker)
      IGM.addCompilerUsedGlobal(var);
    else
      IGM.addUsedGlobal(var);
  }
  return var;
}

void IRGenModule::emitRuntimeRegistration() {
  // Duck out early if we have nothing to register.
  // Note that we don't consider `RuntimeResolvableTypes2` here because the
  // current Swift runtime is unable to handle move-only types at runtime, and
  // we only use this runtime registration path in JIT mode, so there are no
  // ABI forward compatibility concerns.
  //
  // We should incorporate the types from
  // `RuntimeResolvableTypes2` into the list of types to register when we do
  // have runtime support in place.
  if (SwiftProtocols.empty() && ProtocolConformances.empty() &&
      RuntimeResolvableTypes.empty() &&
      (!ObjCInterop || (ObjCProtocols.empty() && ObjCClasses.empty() &&
                        ObjCCategoryDecls.empty())))
    return;
  
  // Find the entry point.
  SILFunction *EntryPoint = getSILModule().lookUpFunction(
      getSILModule().getASTContext().getEntryPointFunctionName());

  // If we're debugging (and not in the REPL), we don't have a
  // main. Find a function marked with the LLDBDebuggerFunction
  // attribute instead.
  if (!EntryPoint && Context.LangOpts.DebuggerSupport) {
    for (SILFunction &SF : getSILModule()) {
      if (SF.hasLocation()) {
        if (Decl* D = SF.getLocation().getAsASTNode<Decl>()) {
          if (auto *FD = dyn_cast<FuncDecl>(D)) {
            if (FD->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>()) {
              EntryPoint = &SF;
              break;
            }
          }
        }
      }
    }
  }
  
  if (!EntryPoint)
    return;
    
  llvm::Function *EntryFunction = Module.getFunction(EntryPoint->getName());
  if (!EntryFunction)
    return;
  
  // Create a new function to contain our logic.
  auto fnTy = llvm::FunctionType::get(VoidTy, /*varArg*/ false);
  auto RegistrationFunction = llvm::Function::Create(fnTy,
                                           llvm::GlobalValue::PrivateLinkage,
                                           "runtime_registration",
                                           getModule());
  RegistrationFunction->setAttributes(constructInitialAttributes());
  
  // Insert a call into the entry function.
  {
    llvm::BasicBlock *EntryBB = &EntryFunction->getEntryBlock();
    llvm::BasicBlock::iterator IP = EntryBB->getFirstInsertionPt();
    IRBuilder Builder(getLLVMContext(),
                      DebugInfo && !Context.LangOpts.DebuggerSupport);
    Builder.llvm::IRBuilderBase::SetInsertPoint(EntryBB, IP);
    if (DebugInfo && !Context.LangOpts.DebuggerSupport)
      DebugInfo->setEntryPointLoc(Builder);
    Builder.CreateCall(fnTy, RegistrationFunction, {});
  }
  
  IRGenFunction RegIGF(*this, RegistrationFunction);
  if (DebugInfo && !Context.LangOpts.DebuggerSupport)
    DebugInfo->emitArtificialFunction(RegIGF, RegistrationFunction);
  
  // Register ObjC protocols we added.
  if (ObjCInterop) {
    if (!ObjCProtocols.empty()) {
      // We need to initialize ObjC protocols in inheritance order, parents
      // first.
      
      llvm::DenseSet<ProtocolDecl*> protos;
      for (auto &proto : ObjCProtocols)
        protos.insert(proto.first);
      
      llvm::SmallVector<ProtocolDecl*, 4> protoInitOrder;

      std::function<void(ProtocolDecl*)> orderProtocol
        = [&](ProtocolDecl *proto) {
          // Recursively put parents first.
          for (auto parent : proto->getInheritedProtocols())
            orderProtocol(parent);

          // Skip if we don't need to reify this protocol.
          auto found = protos.find(proto);
          if (found == protos.end())
            return;
          protos.erase(found);
          protoInitOrder.push_back(proto);
        };
      
      while (!protos.empty()) {
        orderProtocol(*protos.begin());
      }

      // Visit the protocols in the order we established.
      for (auto *proto : protoInitOrder) {
        ObjCProtocolInitializerVisitor(RegIGF)
          .visitMembers(proto);
      }
    }
  }

  // Register Swift protocols if we added any.
  if (!SwiftProtocols.empty()) {
    llvm::Constant *protocols = emitSwiftProtocols(/*asContiguousArray*/ true);

    llvm::Constant *beginIndices[] = {
        llvm::ConstantInt::get(Int32Ty, 0),
        llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto protocolRecordsTy =
        llvm::ArrayType::get(ProtocolRecordTy, SwiftProtocols.size());
    auto begin = llvm::ConstantExpr::getGetElementPtr(protocolRecordsTy,
                                                      protocols, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, SwiftProtocols.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(protocolRecordsTy,
                                                    protocols, endIndices);

    RegIGF.Builder.CreateCall(getRegisterProtocolsFunctionPointer(),
                              {begin, end});
  }

  // Register Swift protocol conformances if we added any.
  if (llvm::Constant *conformances =
          emitProtocolConformances(/*asContiguousArray*/ true)) {
    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto protocolRecordsTy =
        llvm::ArrayType::get(RelativeAddressTy, ProtocolConformances.size());
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        protocolRecordsTy, conformances, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, ProtocolConformances.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(protocolRecordsTy,
                                                    conformances, endIndices);

    RegIGF.Builder.CreateCall(getRegisterProtocolConformancesFunctionPointer(),
                              {begin, end});
  }

  if (!RuntimeResolvableTypes.empty()) {
    llvm::Constant *records =
        emitTypeMetadataRecords(/*asContiguousArray*/ true);

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto typemetadataRecordsTy = llvm::ArrayType::get(
        TypeMetadataRecordTy, RuntimeResolvableTypes.size());
    auto begin = llvm::ConstantExpr::getGetElementPtr(typemetadataRecordsTy,
                                                      records, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, RuntimeResolvableTypes.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(typemetadataRecordsTy,
                                                    records, endIndices);

    RegIGF.Builder.CreateCall(getRegisterTypeMetadataRecordsFunctionPointer(),
                              {begin, end});
  }

  // Register Objective-C classes and extensions we added.
  if (ObjCInterop) {
    for (llvm::WeakTrackingVH &ObjCClass : ObjCClasses) {
      RegIGF.Builder.CreateCall(getInstantiateObjCClassFunctionPointer(),
                                {ObjCClass});
    }

    for (ExtensionDecl *ext : ObjCCategoryDecls) {
      CategoryInitializerVisitor(RegIGF, ext).visitMembers(ext);
    }
  }

  RegIGF.Builder.CreateRetVoid();
}

/// Return the address of the context descriptor representing the given
/// decl context, used as a parent reference for another decl.
///
/// For a nominal type context, this returns the address of the nominal type
/// descriptor.
/// For an extension context, this returns the address of the extension
/// context descriptor.
/// For a module or file unit context, this returns the address of the module
/// context descriptor.
/// For any other kind of context, this returns an anonymous context descriptor
/// for the context.
ConstantReference
IRGenModule::getAddrOfContextDescriptorForParent(DeclContext *parent,
                                                 DeclContext *ofChild,
                                                 bool fromAnonymousContext) {
  switch (parent->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::Initializer:
  case DeclContextKind::SerializedLocal:
    return {getAddrOfAnonymousContextDescriptor(
              fromAnonymousContext ? parent : ofChild),
            ConstantReference::Direct};

  case DeclContextKind::GenericTypeDecl:
    if (auto nomTy = dyn_cast<NominalTypeDecl>(parent)) {
      return {getAddrOfTypeContextDescriptor(nomTy, DontRequireMetadata),
              ConstantReference::Direct};
    }
    return {getAddrOfAnonymousContextDescriptor(
              fromAnonymousContext ? parent : ofChild),
            ConstantReference::Direct};

  case DeclContextKind::ExtensionDecl: {
    auto ext = cast<ExtensionDecl>(parent);
    // If the extension is equivalent to its extended context (that is, it's
    // in the same module as the original non-protocol type and
    // has no constraints), then we can use the original nominal type context
    // (assuming there is one).
    if (ext->isEquivalentToExtendedContext()) {
      auto nominal = ext->getExtendedNominal();
      // If the extended type is an ObjC class, it won't have a nominal type
      // descriptor, so we'll just emit an extension context.
      auto clazz = dyn_cast<ClassDecl>(nominal);
      if (!clazz || clazz->isForeign() || hasKnownSwiftMetadata(*this, clazz)) {
        IRGen.noteUseOfTypeContextDescriptor(nominal, DontRequireMetadata);
        return getAddrOfLLVMVariableOrGOTEquivalent(
                                LinkEntity::forNominalTypeDescriptor(nominal));
      }
    }
    return {getAddrOfExtensionContextDescriptor(ext),
            ConstantReference::Direct};
  }
      
  case DeclContextKind::Package:
    assert(false && "package decl context kind should not have been reached");
  case DeclContextKind::FileUnit:
  case DeclContextKind::MacroDecl:
    parent = parent->getParentModule();
    LLVM_FALLTHROUGH;
      
  case DeclContextKind::Module:
    if (auto *D = ofChild->getAsDecl()) {
      // If the top-level decl has been marked as moved from another module,
      // using @_originallyDefinedIn, we should emit the original module as
      // the context because all run-time names of this decl are based on the
      // original module name.
      auto OriginalModule = D->getAlternateModuleName();
      if (!OriginalModule.empty()) {
        return {getAddrOfOriginalModuleContextDescriptor(OriginalModule),
          ConstantReference::Direct};
      }
    }
    return {getAddrOfModuleContextDescriptor(cast<ModuleDecl>(parent)),
            ConstantReference::Direct};
  }
  llvm_unreachable("unhandled kind");
}

/// Return the address of the context descriptor representing the parent of
/// the given decl context.
///
/// For a nominal type context, this returns the address of the nominal type
/// descriptor.
/// For an extension context, this returns the address of the extension
/// context descriptor.
/// For a module or file unit context, this returns the address of the module
/// context descriptor.
/// For any other kind of context, this returns an anonymous context descriptor
/// for the context.
ConstantReference
IRGenModule::getAddrOfParentContextDescriptor(DeclContext *from,
                                              bool fromAnonymousContext) {
  // Some types get special treatment.
  if (auto Type = dyn_cast<NominalTypeDecl>(from)) {
    // Use a special module context if we have one.
    if (auto context =
            Mangle::ASTMangler::getSpecialManglingContext(
              Type, /*UseObjCProtocolNames=*/false)) {
      switch (*context) {
      case Mangle::ASTMangler::ObjCContext:
        return {getAddrOfObjCModuleContextDescriptor(),
                ConstantReference::Direct};
      case Mangle::ASTMangler::ClangImporterContext:
        return {getAddrOfClangImporterModuleContextDescriptor(),
                ConstantReference::Direct};
      }
    }

    // Wrap up private types in an anonymous context for the containing file
    // unit so that the runtime knows they have unstable identity.
    if (!fromAnonymousContext && Type->isOutermostPrivateOrFilePrivateScope()
        && !Type->isUsableFromInline())
      return {getAddrOfAnonymousContextDescriptor(Type),
              ConstantReference::Direct};
  }
  
  return getAddrOfContextDescriptorForParent(from->getParent(), from,
                                             fromAnonymousContext);
}

static void markGlobalAsUsedBasedOnLinkage(IRGenModule &IGM, LinkInfo &link,
    llvm::GlobalValue *global) {
  // If we're internalizing public symbols at link time, don't make globals
  // unconditionally externally visible.
  if (IGM.getOptions().InternalizeAtLink)
    return;

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  if (link.isUsed())
    IGM.addUsedGlobal(global);
  else if (!IGM.IRGen.Opts.shouldOptimize() &&
           !IGM.IRGen.Opts.ConditionalRuntimeRecords &&
           !IGM.IRGen.Opts.VirtualFunctionElimination &&
           !IGM.IRGen.Opts.WitnessMethodElimination &&
           !global->isDeclaration()) {
    // llvm's pipeline has decide to run GlobalDCE as part of the O0 pipeline.
    // Mark non public symbols as compiler used to counter act this.
    IGM.addCompilerUsedGlobal(global);
  }
}

bool LinkInfo::isUsed(IRLinkage IRL) {
  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  return IRL.Linkage == llvm::GlobalValue::ExternalLinkage &&
         (IRL.Visibility == llvm::GlobalValue::DefaultVisibility ||
          IRL.Visibility == llvm::GlobalValue::ProtectedVisibility) &&
         (IRL.DLLStorage == llvm::GlobalValue::DefaultStorageClass ||
          IRL.DLLStorage == llvm::GlobalValue::DLLExportStorageClass);
}

/// Add the given global value to @llvm.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addUsedGlobal(llvm::GlobalValue *global) {

  // As of reviews.llvm.org/D97448 "ELF: Create unique SHF_GNU_RETAIN sections
  // for llvm.used global objects" LLVM creates separate sections for globals in
  // llvm.used on ELF.  Therefore we use llvm.compiler.used on ELF instead.
  if (TargetInfo.OutputObjectFormat == llvm::Triple::ELF) {
    addCompilerUsedGlobal(global);
    return;
  }

  LLVMUsed.push_back(global);
}

/// Add the given global value to @llvm.compiler.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addCompilerUsedGlobal(llvm::GlobalValue *global) {
  LLVMCompilerUsed.push_back(global);
}

void IRGenModule::addGenericROData(llvm::Constant *RODataAddr) {
  GenericRODatas.push_back(RODataAddr);
}

/// Add the given global value to the Objective-C class list.
void IRGenModule::addObjCClass(llvm::Constant *classPtr, bool nonlazy) {
  ObjCClasses.push_back(classPtr);
  if (nonlazy)
    ObjCNonLazyClasses.push_back(classPtr);
}

/// Add the given global value to the Objective-C resilient class stub list.
void IRGenModule::addObjCClassStub(llvm::Constant *classPtr) {
  ObjCClassStubs.push_back(classPtr);
}

void IRGenModule::addRuntimeResolvableType(GenericTypeDecl *type) {
  // Collect the nominal type records we emit into a special section.
  if (type->isMoveOnly()) {
    // Older runtimes should not be allowed to discover noncopyable types, since
    // they will try to expose them dynamically as copyable types. Record
    // noncopyable type descriptors in a separate vector so that future
    // noncopyable-type-aware runtimes and reflection libraries can still find
    // them.
    RuntimeResolvableTypes2.push_back(type);
  } else {
    RuntimeResolvableTypes.push_back(type);
  }

  if (auto nominal = dyn_cast<NominalTypeDecl>(type)) {
    // As soon as the type metadata is available, all the type's conformances
    // must be available, too. The reason is that a type (with the help of its
    // metadata) can be checked at runtime if it conforms to a protocol.
    addLazyConformances(nominal);
  }
}

ConstantReference
IRGenModule::getConstantReferenceForProtocolDescriptor(ProtocolDecl *proto) {
  if (proto->isObjC()) {
    // ObjC protocol descriptors don't have a unique address, but get uniqued
    // by the Objective-C runtime at load time.
    // Get the indirected address of the protocol descriptor reference variable
    // that the ObjC runtime uniques.
    auto refVar = getAddrOfObjCProtocolRef(proto, NotForDefinition);
    return ConstantReference(cast<llvm::Constant>(refVar.getAddress()),
                             ConstantReference::Indirect);
  }
  
  // Try to form a direct reference to the nominal type descriptor if it's in
  // the same binary, or use the GOT entry if it's from another binary.
  return getAddrOfLLVMVariableOrGOTEquivalent(
                                     LinkEntity::forProtocolDescriptor(proto));
}

void IRGenModule::addLazyConformances(const IterableDeclContext *idc) {
  for (const ProtocolConformance *conf :
         idc->getLocalConformances(ConformanceLookupKind::All)) {
    IRGen.addLazyWitnessTable(conf);
  }
}

std::string IRGenModule::GetObjCSectionName(StringRef Section,
                                            StringRef MachOAttributes) {
  assert(Section.substr(0, 2) == "__" && "expected the name to begin with __");

  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    return MachOAttributes.empty()
               ? ("__DATA," + Section).str()
               : ("__DATA," + Section + "," + MachOAttributes).str();
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    return Section.substr(2).str();
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    return ("." + Section.substr(2) + "$B").str();
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::SetCStringLiteralSection(llvm::GlobalVariable *GV,
                                           ObjCLabelType Type) {
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    switch (Type) {
    case ObjCLabelType::ClassName:
      GV->setSection("__TEXT,__objc_classname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarName:
      GV->setSection("__TEXT,__objc_methname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarType:
      GV->setSection("__TEXT,__objc_methtype,cstring_literals");
      return;
    case ObjCLabelType::PropertyName:
      GV->setSection("__TEXT,__cstring,cstring_literals");
      return;
    }
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    return;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    return;
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::emitGlobalLists() {
  if (ObjCInterop) {
    if (IRGen.Opts.EmitGenericRODatas) {
      emitGlobalList(
          *this, GenericRODatas, "generic_ro_datas",
          GetObjCSectionName("__swift_rodatas", "regular"),
          llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
          /*asContiguousArray*/ true, /*canBeStrippedByLinker*/ true);
    }

    // Objective-C class references go in a variable with a meaningless
    // name but a magic section.
    emitGlobalList(
        *this, ObjCClasses, "objc_classes",
        GetObjCSectionName("__objc_classlist", "regular,no_dead_strip"),
        llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
        /*asContiguousArray*/ false);

    // So do resilient class stubs.
    emitGlobalList(
        *this, ObjCClassStubs, "objc_class_stubs",
        GetObjCSectionName("__objc_stublist", "regular,no_dead_strip"),
        llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
        /*asContiguousArray*/ true);

    // So do categories.
    emitGlobalList(
        *this, ObjCCategories, "objc_categories",
        GetObjCSectionName("__objc_catlist", "regular,no_dead_strip"),
        llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
        /*asContiguousArray*/ true);

    // And categories on class stubs.
    emitGlobalList(
        *this, ObjCCategoriesOnStubs, "objc_categories_stubs",
        GetObjCSectionName("__objc_catlist2", "regular,no_dead_strip"),
        llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
        /*asContiguousArray*/ true);

    // Emit nonlazily realized class references in a second magic section to
    // make sure they are realized by the Objective-C runtime before any
    // instances are allocated.
    emitGlobalList(
        *this, ObjCNonLazyClasses, "objc_non_lazy_classes",
        GetObjCSectionName("__objc_nlclslist", "regular,no_dead_strip"),
        llvm::GlobalValue::InternalLinkage, Int8PtrTy, /*isConstant*/ false,
        /*asContiguousArray*/ true);
  }

  // @llvm.used

  // Collect llvm.used globals already in the module (coming from ClangCodeGen).
  collectGlobalList(*this, LLVMUsed, "llvm.used");
  emitGlobalList(*this, LLVMUsed, "llvm.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 /*isConstant*/false, /*asContiguousArray*/true);

  // Collect llvm.compiler.used globals already in the module (coming
  // from ClangCodeGen).
  collectGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used");
  emitGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 /*isConstant*/false, /*asContiguousArray*/true);
}

// Eagerly emit functions that are externally visible. Functions that are
// dynamic replacements must also be eagerly emitted.
static bool isLazilyEmittedFunction(SILFunction &f, SILModule &m) {
  if (f.isPossiblyUsedExternally())
    return false;

  if (f.getDynamicallyReplacedFunction())
    return false;

  // Needed by lldb to print global variables which are propagated by the
  // mandatory GlobalOpt.
  if (m.getOptions().OptMode == OptimizationMode::NoOptimization &&
      f.isGlobalInit())
    return false;

  return true;
}

void IRGenerator::emitGlobalTopLevel(
    const std::vector<std::string> &linkerDirectives) {
  // Generate order numbers for the functions in the SIL module that
  // correspond to definitions in the LLVM module.
  unsigned nextOrderNumber = 0;
  for (auto &silFn : PrimaryIGM->getSILModule().getFunctions()) {
    // Don't bother adding external declarations to the function order.
    if (!silFn.isDefinition()) continue;
    FunctionOrder.insert(std::make_pair(&silFn, nextOrderNumber++));
  }

  // Ensure that relative symbols are collocated in the same LLVM module.
  for (auto &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getDeclContext());
    ensureRelativeSymbolCollocation(wt);
  }

  for (auto &wt : PrimaryIGM->getSILModule().getDefaultWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getProtocol()->getDeclContext());
    ensureRelativeSymbolCollocation(wt);
  }
  for (auto &directive: linkerDirectives) {
    createLinkerDirectiveVariable(*PrimaryIGM, directive);
  }
  for (SILGlobalVariable &v : PrimaryIGM->getSILModule().getSILGlobals()) {
    Decl *decl = v.getDecl();
    CurrentIGMPtr IGM = getGenModule(decl ? decl->getDeclContext() : nullptr);
    IGM->emitSILGlobalVariable(&v);
  }
  
  // Emit SIL functions.
  auto &m = PrimaryIGM->getSILModule();
  for (SILFunction &f : m) {
    if (isLazilyEmittedFunction(f, m))
      continue;

    CurrentIGMPtr IGM = getGenModule(&f);
    IGM->emitSILFunction(&f);
  }

  // Emit witness tables.
  for (SILWitnessTable &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getDeclContext());
    if (!canEmitWitnessTableLazily(&wt)) {
      IGM->emitSILWitnessTable(&wt);
    }
  }
  
  // Emit property descriptors.
  for (auto &prop : PrimaryIGM->getSILModule().getPropertyList()) {
    CurrentIGMPtr IGM = getGenModule(prop.getDecl()->getInnermostDeclContext());
    IGM->emitSILProperty(&prop);
  }

  // Emit differentiability witnesses.
  for (auto &dw :
           PrimaryIGM->getSILModule().getDifferentiabilityWitnessList()) {
    // Emit into same IRGenModule as the original function.
    // NOTE(TF-894): Investigate whether `getGenModule(dw.getVJP())` is
    // significant/desirable; `getGenModule` seems relevant for multi-threaded
    // compilation. When the differentiation transform canonicalizes all
    // differentiability witnesses to have JVP/VJP functions, we can assert
    // that JVP/VJP functions exist and use `getGenModule(dw.getVJP())`.
    CurrentIGMPtr IGM = getGenModule(dw.getOriginalFunction());

    IGM->emitSILDifferentiabilityWitness(&dw);
  }

  for (auto Iter : *this) {
    IRGenModule *IGM = Iter.second;
    IGM->finishEmitAfterTopLevel();
  }

  emitEntryPointInfo();
}

void IRGenModule::finishEmitAfterTopLevel() {
  // Emit the implicit import of the swift standard library.
  // FIXME: We'd get the exact set of implicit imports if we went through the
  // SourceFile's getImportedModules instead, but then we'd lose location info
  // for the explicit imports.
  if (DebugInfo) {
    if (ModuleDecl *TheStdlib = Context.getStdlibModule()) {
      if (TheStdlib != getSwiftModule()) {
        Located<swift::Identifier> moduleName[] = {
          { Context.StdlibModuleName, swift::SourceLoc() }
        };

        auto Imp = ImportDecl::create(Context,
                                      getSwiftModule(),
                                      SourceLoc(),
                                      ImportKind::Module, SourceLoc(),
                                      llvm::makeArrayRef(moduleName));
        Imp->setModule(TheStdlib);
        DebugInfo->emitImport(Imp);
      }
    }
  }
}

void IRGenerator::emitSwiftProtocols() {
  for (auto &m : *this) {
    m.second->emitSwiftProtocols(/*asContiguousArray*/ false);
  }
}

void IRGenerator::emitProtocolConformances() {
  for (auto &m : *this) {
    m.second->emitProtocolConformances(/*asContiguousArray*/ false);
  }
}

void IRGenerator::emitTypeMetadataRecords() {
  for (auto &m : *this) {
    m.second->emitTypeMetadataRecords(/*asContiguousArray*/ false);
  }
}

void IRGenerator::emitAccessibleFunctions() {
  for (auto &m : *this)
    m.second->emitAccessibleFunctions();
}

static void
deleteAndReenqueueForEmissionValuesDependentOnCanonicalPrespecializedMetadataRecords(
    IRGenModule &IGM, CanType typeWithCanonicalMetadataPrespecialization,
    NominalTypeDecl &decl) {
  // The accessor depends on the existence of canonical metadata records
  // because their presence determine which runtime function is called.
  auto *accessor = IGM.getAddrOfTypeMetadataAccessFunction(
      decl.getDeclaredType()->getCanonicalType(), NotForDefinition);
  accessor->deleteBody();
  IGM.IRGen.noteUseOfMetadataAccessor(&decl);

  IGM.IRGen.noteLazyReemissionOfNominalTypeDescriptor(&decl);
  // The type context descriptor depends on canonical metadata records because
  // pointers to them are attached as trailing objects to it.
  //
  // Don't call
  //
  //     noteUseOfTypeContextDescriptor
  //
  // here because we don't want to reemit metadata.
  emitLazyTypeContextDescriptor(IGM, &decl, RequireMetadata);
}

/// Emit any lazy definitions (of globals or functions or whatever
/// else) that we require.
void IRGenerator::emitLazyDefinitions() {
  while (!LazyTypeMetadata.empty() ||
         !LazySpecializedTypeMetadataRecords.empty() ||
         !LazyTypeContextDescriptors.empty() ||
         !LazyOpaqueTypeDescriptors.empty() || !LazyFieldDescriptors.empty() ||
         !LazyFunctionDefinitions.empty() || !LazyWitnessTables.empty() ||
         !LazyCanonicalSpecializedMetadataAccessors.empty() ||
         !LazyMetadataAccessors.empty()) {
    // Emit any lazy type metadata we require.
    while (!LazyTypeMetadata.empty()) {
      NominalTypeDecl *type = LazyTypeMetadata.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsMetadataUsed && !entry.IsMetadataEmitted);
      entry.IsMetadataEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeMetadata(*IGM.get(), type);
    }
    while (!LazySpecializedTypeMetadataRecords.empty()) {
      CanType theType;
      TypeMetadataCanonicality canonicality;
      std::tie(theType, canonicality) =
          LazySpecializedTypeMetadataRecords.pop_back_val();
      auto *nominal = theType->getNominalOrBoundGenericNominal();
      CurrentIGMPtr IGMPtr = getGenModule(nominal->getDeclContext());
      auto &IGM = *IGMPtr.get();
      // A new canonical prespecialized metadata changes both the type
      // descriptor (adding a new entry to the trailing list of metadata) and
      // the metadata accessor (calling the appropriate getGenericMetadata
      // variant depending on whether there are any canonical prespecialized
      // metadata records to add to the metadata cache).  Consequently, it is
      // necessary to force these to be reemitted.
      if (canonicality == TypeMetadataCanonicality::Canonical) {
        deleteAndReenqueueForEmissionValuesDependentOnCanonicalPrespecializedMetadataRecords(
            IGM, theType, *nominal);
      }
      emitLazySpecializedGenericTypeMetadata(IGM, theType);
    }
    while (!LazyTypeContextDescriptors.empty()) {
      NominalTypeDecl *type = LazyTypeContextDescriptors.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsDescriptorUsed && !entry.IsDescriptorEmitted);
      entry.IsDescriptorEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeContextDescriptor(*IGM.get(), type,
                                    RequireMetadata_t(entry.IsMetadataUsed));
    }
    while (!LazyOpaqueTypeDescriptors.empty()) {
      OpaqueTypeDecl *type = LazyOpaqueTypeDescriptors.pop_back_val();
      auto &entry = LazyOpaqueTypes.find(type)->second;
      assert(hasLazyMetadata(type));
      assert(entry.IsDescriptorUsed && !entry.IsDescriptorEmitted);
      entry.IsDescriptorEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      IGM->emitOpaqueTypeDecl(type);
    }
    while (!LazyFieldDescriptors.empty()) {
      NominalTypeDecl *type = LazyFieldDescriptors.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      IGM->emitFieldDescriptor(type);
    }
    while (!LazyWitnessTables.empty()) {
      SILWitnessTable *wt = LazyWitnessTables.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(wt->getDeclContext());
      IGM->emitSILWitnessTable(wt);
    }

    // Emit any lazy function definitions we require.
    while (!LazyFunctionDefinitions.empty()) {
      SILFunction *f = LazyFunctionDefinitions.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(f);
      assert(!f->isPossiblyUsedExternally()
             && "function with externally-visible linkage emitted lazily?");
      IGM->emitSILFunction(f);
    }

    while (!LazyCanonicalSpecializedMetadataAccessors.empty()) {
      CanType theType =
          LazyCanonicalSpecializedMetadataAccessors.pop_back_val();
      auto *nominal = theType->getAnyNominal();
      assert(nominal);
      CurrentIGMPtr IGMPtr = getGenModule(nominal->getDeclContext());
      auto &IGM = *IGMPtr.get();
      // TODO: Once non-canonical accessors are available, this variable should
      //       reflect the canonicality of the accessor rather than always being
      //       canonical.
      auto canonicality = TypeMetadataCanonicality::Canonical;
      if (canonicality == TypeMetadataCanonicality::Canonical) {
        deleteAndReenqueueForEmissionValuesDependentOnCanonicalPrespecializedMetadataRecords(
            IGM, theType, *nominal);
      }
      emitLazyCanonicalSpecializedMetadataAccessor(IGM, theType);
    }
    while (!LazyMetadataAccessors.empty()) {
      NominalTypeDecl *nominal = LazyMetadataAccessors.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(nominal->getDeclContext());
      emitLazyMetadataAccessor(*IGM.get(), nominal);
    }
  }

  FinishedEmittingLazyDefinitions = true;
}

void IRGenerator::addLazyFunction(SILFunction *f) {
  // Add it to the queue if it hasn't already been put there.
  if (!LazilyEmittedFunctions.insert(f).second)
    return;

  assert(!FinishedEmittingLazyDefinitions);
  LazyFunctionDefinitions.push_back(f);

  if (const SILFunction *orig = f->getOriginOfSpecialization()) {
    // f is a specialization. Try to emit all specializations of the same
    // original function into the same IGM. This increases the chances that
    // specializations are merged by LLVM's function merging.
    IRGenModule *IGM = CurrentIGM ? CurrentIGM : getPrimaryIGM();
    auto iter = IGMForSpecializations.insert(std::make_pair(orig, IGM)).first;
    DefaultIGMForFunction.insert(std::make_pair(f, iter->second));
    return;
  }

  if (auto *dc = f->getDeclContext())
    if (dc->getParentSourceFile())
      return;

  if (CurrentIGM == nullptr)
    return;

  // Don't update the map if we already have an entry.
  DefaultIGMForFunction.insert(std::make_pair(f, CurrentIGM));
}

bool IRGenerator::hasLazyMetadata(TypeDecl *type) {
  assert(isa<NominalTypeDecl>(type) ||
         isa<OpaqueTypeDecl>(type));
  auto found = HasLazyMetadata.find(type);
  if (found != HasLazyMetadata.end())
    return found->second;

  auto canBeLazy = [&]() -> bool {
    auto *dc = type->getDeclContext();
    if (isa<ClangModuleUnit>(dc->getModuleScopeContext())) {
      if (auto nominal = dyn_cast<NominalTypeDecl>(type)) {
        return requiresForeignTypeMetadata(nominal);
      }
    } else if (dc->getParentModule() == SIL.getSwiftModule()) {
      // When compiling with -Onone keep all metadata for the debugger. Even if
      // it is not used by the program itself.
      if (!Opts.shouldOptimize())
        return false;
      if (Opts.UseJIT)
        return false;

      if (isa<ClassDecl>(type) || isa<ProtocolDecl>(type))
        return false;

      switch (type->getEffectiveAccess()) {
      case AccessLevel::Open:
      case AccessLevel::Public:
      case AccessLevel::Package:
        // We can't remove metadata for externally visible types.
        return false;
      case AccessLevel::Internal:
        // In non-whole-module mode, internal types are also visible externally.
        return SIL.isWholeModule();
      case AccessLevel::FilePrivate:
      case AccessLevel::Private:
        return true;
      }
    }

    return false;
  };

  bool isLazy = canBeLazy();
  HasLazyMetadata[type] = isLazy;

  return isLazy;
}

void IRGenerator::noteUseOfTypeGlobals(NominalTypeDecl *type,
                                       bool isUseOfMetadata,
                                       RequireMetadata_t requireMetadata) {
  if (!type)
    return;

  assert(!Lowering::shouldSkipLowering(type));

  // Force emission of ObjC protocol descriptors used by type refs.
  if (auto proto = dyn_cast<ProtocolDecl>(type)) {
    if (proto->isObjC()) {
      PrimaryIGM->getAddrOfObjCProtocolRecord(proto, NotForDefinition);
      return;
    }
  }

  if (!hasLazyMetadata(type))
    return;

  // If the type can be generated in several TU with weak linkage we don't know
  // which one will be picked up so we have to require the metadata. Otherwise,
  // the situation can arise where one TU contains a type descriptor with a null
  // metadata access function and the other TU which requires metadata has a
  // type descriptor with a valid metadata access function but the linker picks
  // the first one.
  if (isAccessorLazilyGenerated(getTypeMetadataAccessStrategy(
          type->getDeclaredType()->getCanonicalType()))) {
    requireMetadata = RequireMetadata;
  }

  // Try to create a new record of the fact that we used this type.
  auto insertResult = LazyTypeGlobals.try_emplace(type);
  auto &entry = insertResult.first->second;

  bool metadataWasUsed = entry.IsMetadataUsed;
  bool descriptorWasUsed = entry.IsDescriptorUsed;

  bool isNovelUseOfMetadata = false;
  bool isNovelUseOfDescriptor = false;

  // Flag that we have a use of the metadata if
  //   - the reference was directly to the metadata
  //   - the reference was to the descriptor, but it requested the emission
  //     of metadata
  if (!metadataWasUsed && (isUseOfMetadata || requireMetadata)) {
    if (metadataWasUsed) return;
    entry.IsMetadataUsed = true;
    isNovelUseOfMetadata = true;
  }

  if (!descriptorWasUsed && !isUseOfMetadata) {
    if (descriptorWasUsed) return;
    entry.IsDescriptorUsed = true;
    isNovelUseOfDescriptor = true;
  }

  // Enqueue metadata emission if we have a novel use of it.
  if (isNovelUseOfMetadata) {
    assert(!FinishedEmittingLazyDefinitions);
    LazyTypeMetadata.push_back(type);
  }

  // Enqueue descriptor emission if we have a novel use of it or if we
  // need to re-emit it because we're suddenly using metadata for it.
  if (isNovelUseOfDescriptor ||
      (isNovelUseOfMetadata && entry.IsDescriptorEmitted)) {
    entry.IsDescriptorEmitted = false; // clear this in case it was true
    assert(!FinishedEmittingLazyDefinitions);
    LazyTypeContextDescriptors.push_back(type);
  }
}

void IRGenerator::noteUseOfFieldDescriptor(NominalTypeDecl *type) {
  if (!hasLazyMetadata(type))
    return;

  // Imported classes and protocols do not need field descriptors.
  if (type->hasClangNode() &&
      (isa<ClassDecl>(type) ||
       isa<ProtocolDecl>(type)))
    return;

  if (!LazilyEmittedFieldMetadata.insert(type).second)
    return;

  assert(!FinishedEmittingLazyDefinitions);
  LazyFieldDescriptors.push_back(type);
}

void IRGenerator::noteUseOfCanonicalSpecializedMetadataAccessor(
    CanType forType) {
  auto key = forType->getAnyNominal();
  assert(key);
  assert(key->isGenericContext());
  auto &enqueuedSpecializedAccessors =
      CanonicalSpecializedAccessorsForGenericTypes[key];
  if (llvm::all_of(enqueuedSpecializedAccessors,
                   [&](CanType enqueued) { return enqueued != forType; })) {
    assert(!FinishedEmittingLazyDefinitions);
    LazyCanonicalSpecializedMetadataAccessors.insert(forType);
    enqueuedSpecializedAccessors.push_back(forType);
  }
}

static bool typeKindCanBePrespecialized(TypeKind theKind) {
  switch (theKind) {
  case TypeKind::Struct:
  case TypeKind::BoundGenericStruct:
  case TypeKind::Enum:
  case TypeKind::BoundGenericEnum:
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
    return true;
  default:
    return false;
  }
}

void IRGenerator::noteUseOfSpecializedGenericTypeMetadata(
    IRGenModule &IGM, CanType theType, TypeMetadataCanonicality canonicality) {
  assert(typeKindCanBePrespecialized(theType->getKind()));
  auto key = theType->getAnyNominal();
  assert(key);
  assert(key->isGenericContext());
  auto &enqueuedSpecializedTypes =
      MetadataPrespecializationsForGenericTypes[key];
  if (llvm::all_of(enqueuedSpecializedTypes,
                   [&](auto enqueued) { return enqueued.first != theType; })) {
    assert(!FinishedEmittingLazyDefinitions);
    LazySpecializedTypeMetadataRecords.push_back({theType, canonicality});
    enqueuedSpecializedTypes.push_back({theType, canonicality});
  }
}

void IRGenerator::noteUseOfOpaqueTypeDescriptor(OpaqueTypeDecl *opaque) {
  if (!opaque)
    return;

  if (!hasLazyMetadata(opaque))
    return;

  auto insertResult = LazyOpaqueTypes.try_emplace(opaque);
  auto &entry = insertResult.first->second;

  bool isNovelUseOfDescriptor = !entry.IsDescriptorUsed;
  entry.IsDescriptorUsed = true;
  
  if (isNovelUseOfDescriptor) {
    LazyOpaqueTypeDescriptors.push_back(opaque);
  }
}

static std::string getDynamicReplacementSection(IRGenModule &IGM) {
  std::string sectionName;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_replace, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_replace";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5repl$B";
    break;
  }
  return sectionName;
}

static std::string getDynamicReplacementSomeSection(IRGenModule &IGM) {
  std::string sectionName;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_replac2, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_replac2";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5reps$B";
    break;
  }
  return sectionName;
}
llvm::GlobalVariable *IRGenModule::getGlobalForDynamicallyReplaceableThunk(
    LinkEntity &entity, llvm::Type *type, ForDefinition_t forDefinition) {
  return cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo()));
}

/// Creates a dynamic replacement chain entry for \p SILFn that contains either
/// the implementation function pointer \p or a nullptr, the next pointer of the
/// chain entry is set to nullptr.
///   struct ChainEntry {
///      void *funPtr;
///      struct ChainEntry *next;
///   }
static llvm::GlobalVariable *getChainEntryForDynamicReplacement(
    IRGenModule &IGM, LinkEntity entity,
    llvm::Constant *implFunction = nullptr,
    ForDefinition_t forDefinition = ForDefinition) {

  auto linkEntry = IGM.getGlobalForDynamicallyReplaceableThunk(
      entity, IGM.DynamicReplacementLinkEntryTy, forDefinition);
  if (!forDefinition)
    return linkEntry;

  auto *funPtr =
      implFunction ? llvm::ConstantExpr::getBitCast(implFunction, IGM.Int8PtrTy)
                   : llvm::ConstantExpr::getNullValue(IGM.Int8PtrTy);

  if (implFunction) {
    llvm::Constant *indices[] = {llvm::ConstantInt::get(IGM.Int32Ty, 0),
                                 llvm::ConstantInt::get(IGM.Int32Ty, 0)};
    auto *storageAddr = llvm::ConstantExpr::getInBoundsGetElementPtr(
        IGM.DynamicReplacementLinkEntryTy, linkEntry, indices);
    bool isAsyncFunction =
        entity.hasSILFunction() && entity.getSILFunction()->isAsync();
    auto &schema =
        isAsyncFunction
            ? IGM.getOptions().PointerAuth.AsyncSwiftDynamicReplacements
            : IGM.getOptions().PointerAuth.SwiftDynamicReplacements;
    assert(entity.hasSILFunction() || entity.isOpaqueTypeDescriptorAccessor());
    auto authEntity = entity.hasSILFunction()
                          ? PointerAuthEntity(entity.getSILFunction())
                          : PointerAuthEntity::Special::TypeDescriptor;
    funPtr =
        IGM.getConstantSignedPointer(funPtr, schema, authEntity, storageAddr);
  }

  auto *nextEntry =
      llvm::ConstantExpr::getNullValue(IGM.DynamicReplacementLinkEntryPtrTy);
  llvm::Constant *fields[] = {funPtr, nextEntry};
  auto *entry =
      llvm::ConstantStruct::get(IGM.DynamicReplacementLinkEntryTy, fields);
  linkEntry->setInitializer(entry);
  return linkEntry;
}

void IRGenerator::emitDynamicReplacements() {
  if (DynamicReplacements.empty())
    return;

  auto &IGM = *getPrimaryIGM();

  // Collect all the type metadata accessor replacements.
  SmallVector<OpaqueTypeArchetypeType *, 8> newFuncTypes;
  SmallVector<OpaqueTypeArchetypeType *, 8> origFuncTypes;
  llvm::SmallSet<OpaqueTypeArchetypeType *, 8> newUniqueOpaqueTypes;
  llvm::SmallSet<OpaqueTypeArchetypeType *, 8> origUniqueOpaqueTypes;
  for (auto *newFunc : DynamicReplacements) {
    auto newResultTy = newFunc->getLoweredFunctionType()
             ->getAllResultsSubstType(newFunc->getModule(),
                                      TypeExpansionContext::minimal())
             .getASTType();
    if (!newResultTy->hasOpaqueArchetype())
      continue;
    newResultTy.visit([&](CanType ty) {
      if (auto opaque = ty->getAs<OpaqueTypeArchetypeType>())
        if (newUniqueOpaqueTypes.insert(opaque).second)
          newFuncTypes.push_back(opaque);
    });
    auto *origFunc = newFunc->getDynamicallyReplacedFunction();
    assert(origFunc);
    auto origResultTy = origFunc->getLoweredFunctionType()
                  ->getAllResultsSubstType(origFunc->getModule(),
                                           TypeExpansionContext::minimal())
                  .getASTType();
    assert(origResultTy->hasOpaqueArchetype());
    origResultTy.visit([&](CanType ty) {
      if (auto opaque = ty->getAs<OpaqueTypeArchetypeType>())
        if (origUniqueOpaqueTypes.insert(opaque).second)
          origFuncTypes.push_back(opaque);
    });

    assert(origFuncTypes.size() == newFuncTypes.size());
  }

  // struct ReplacementScope {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeIndirectablePointer<KeyEntry, false> replacedFunctionKey;
  //     RelativeDirectPointer<void> newFunction;
  //     RelativeDirectPointer<LinkEntry> replacement;
  //     uint32_t flags; // shouldChain.
  //   }[0]
  // };
  ConstantInitBuilder builder(IGM);
  auto replacementScope = builder.beginStruct();
  replacementScope.addInt32(0); // unused flags.
  replacementScope.addInt32(DynamicReplacements.size() + newFuncTypes.size());

  auto replacementsArray =
      replacementScope.beginArray();
  for (auto *newFunc : DynamicReplacements) {
    LinkEntity entity =
        LinkEntity::forDynamicallyReplaceableFunctionVariable(newFunc);
    auto replacementLinkEntry =
        getChainEntryForDynamicReplacement(IGM, entity);
    // TODO: replacementLinkEntry->setZeroSection()
    auto *origFunc = newFunc->getDynamicallyReplacedFunction();
    assert(origFunc);
    auto keyRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forDynamicallyReplaceableFunctionKey(origFunc));

    llvm::Constant *newFnPtr = llvm::ConstantExpr::getBitCast(
        newFunc->isAsync()
            ? IGM.getAddrOfAsyncFunctionPointer(
                  LinkEntity::forSILFunction(newFunc))
            : IGM.getAddrOfSILFunction(newFunc, NotForDefinition),
        IGM.Int8PtrTy);

    auto replacement = replacementsArray.beginStruct();
    replacement.addRelativeAddress(keyRef); // tagged relative reference.
    replacement.addRelativeAddress(newFnPtr); // direct relative reference.
    replacement.addRelativeAddress(
        replacementLinkEntry); // direct relative reference.
    replacement.addInt32(
        Opts.EnableDynamicReplacementChaining ? 1 : 0);
    replacement.finishAndAddTo(replacementsArray);
  }
  // Emit replacements of the opaque type descriptor accessor.
  for (auto i : indices(origFuncTypes)) {
    LinkEntity entity = LinkEntity::forOpaqueTypeDescriptorAccessorVar(
        newFuncTypes[i]->getDecl());
    auto replacementLinkEntry = getChainEntryForDynamicReplacement(IGM, entity);
    auto keyRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forOpaqueTypeDescriptorAccessorKey(
            origFuncTypes[i]->getDecl()));
    llvm::Constant *newFnPtr = llvm::ConstantExpr::getBitCast(
        IGM.getAddrOfOpaqueTypeDescriptorAccessFunction(
               newFuncTypes[i]->getDecl(), NotForDefinition, false)
            .getDirectPointer(),
        IGM.Int8PtrTy);
    auto replacement = replacementsArray.beginStruct();
    replacement.addRelativeAddress(keyRef);   // tagged relative reference.
    replacement.addRelativeAddress(newFnPtr); // direct relative reference.
    replacement.addRelativeAddress(
        replacementLinkEntry); // direct relative reference.
    replacement.addInt32(0);
    replacement.finishAndAddTo(replacementsArray);
  }
  replacementsArray.finishAndAddTo(replacementScope);

  auto var = replacementScope.finishAndCreateGlobal(
      "\x01l_unnamed_dynamic_replacements", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  IGM.setTrueConstGlobal(var);
  IGM.addUsedGlobal(var);

  // Emit the data for automatic replacement to happen on load.
  // struct AutomaticReplacements {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeDirectPointer<ReplacementScope> replacements;
  //     uint32_t flags; // unused.
  //   }[0]
  // };
  auto autoReplacements = builder.beginStruct();
  autoReplacements.addInt32(0); // unused flags.
  autoReplacements.addInt32(1); // number of replacement entries.
  auto autoReplacementsArray = autoReplacements.beginArray();
  autoReplacementsArray.addRelativeAddress(var);
  autoReplacementsArray.addInt32(0); // unused flags.
  autoReplacementsArray.finishAndAddTo(autoReplacements);
  auto autoReplVar = autoReplacements.finishAndCreateGlobal(
      "\x01l_auto_dynamic_replacements", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  autoReplVar->setSection(getDynamicReplacementSection(IGM));
  IGM.addUsedGlobal(autoReplVar);

  if (origFuncTypes.empty())
    return;
  // Emit records for replacing opaque type descriptor for some types.
  // struct AutomaticReplacementsSome {
  //   uint32t flags; // unused
  //   uint32t numReplacements;
  //   struct Entry {
  //     RelativeIndirectablePointer<OpaqueTypeDescriptor*> orig;
  //     RelativeDirectPointer<OpaqueTypeDescriptor*> replacement;
  //     uint32_t flags; // unused.
  //   }[numEntries]
  // };
  auto autoReplacementsSome = builder.beginStruct();
  autoReplacementsSome.addInt32(0); // unused flags.
  autoReplacementsSome.addInt32(
      origFuncTypes.size()); // number of replacement entries.
  auto someReplacementsArray = autoReplacementsSome.beginArray();
  for (auto i : indices(origFuncTypes)) {
    auto origDesc =
        LinkEntity::forOpaqueTypeDescriptor(origFuncTypes[i]->getDecl());
    auto replDesc =
        LinkEntity::forOpaqueTypeDescriptor(newFuncTypes[i]->getDecl());
    auto replacement = someReplacementsArray.beginStruct();
    replacement.addRelativeAddress(
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(origDesc));
    replacement.addRelativeAddress(
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(replDesc));
    replacement.finishAndAddTo(someReplacementsArray);
  }
  someReplacementsArray.finishAndAddTo(autoReplacementsSome);
  auto autoReplVar2 = autoReplacementsSome.finishAndCreateGlobal(
      "\x01l_auto_dynamic_replacements_some", IGM.getPointerAlignment(),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  autoReplVar2->setSection(getDynamicReplacementSomeSection(IGM));
  IGM.addUsedGlobal(autoReplVar2);
}

void IRGenerator::emitEagerClassInitialization() {
  if (ClassesForEagerInitialization.empty())
    return;

  // Emit the register function in the primary module.
  IRGenModule *IGM = getPrimaryIGM();

  llvm::Function *RegisterFn = llvm::Function::Create(
                                llvm::FunctionType::get(IGM->VoidTy, false),
                                llvm::GlobalValue::PrivateLinkage,
                                "_swift_eager_class_initialization");
  IGM->Module.getFunctionList().push_back(RegisterFn);
  IRGenFunction RegisterIGF(*IGM, RegisterFn);
  if (IGM->DebugInfo)
    IGM->DebugInfo->emitArtificialFunction(RegisterIGF, RegisterIGF.CurFn);
  RegisterFn->setAttributes(IGM->constructInitialAttributes());
  RegisterFn->setCallingConv(IGM->DefaultCC);
  IGM->setColocateMetadataSection(RegisterFn);

  for (ClassDecl *CD : ClassesForEagerInitialization) {
    auto Ty = CD->getDeclaredType()->getCanonicalType();
    llvm::Value *MetaData = RegisterIGF.emitTypeMetadataRef(Ty);
    assert(CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>());

    // Get the metadata to make sure that the class is registered. We need to 
    // add a use (empty inline asm instruction) for the metadata. Otherwise
    // llvm would optimize the metadata accessor call away because it's
    // defined as "readnone".
    llvm::FunctionType *asmFnTy =
      llvm::FunctionType::get(IGM->VoidTy, {MetaData->getType()},
                              false /* = isVarArg */);
    llvm::InlineAsm *inlineAsm =
      llvm::InlineAsm::get(asmFnTy, "", "r", true /* = SideEffects */);
    RegisterIGF.Builder.CreateAsmCall(inlineAsm, MetaData);
  }
  RegisterIGF.Builder.CreateRetVoid();

  // Add the registration function as a static initializer. We use a priority
  // slightly lower than used for C++ global constructors, so that the code is
  // executed before C++ global constructors (in case someone uses archives
  // from a C++ global constructor).
  llvm::appendToGlobalCtors(IGM->Module, RegisterFn, 60000, nullptr);
}

void IRGenerator::emitObjCActorsNeedingSuperclassSwizzle() {
  if (ObjCActorsNeedingSuperclassSwizzle.empty())
    return;

  // Emit the register function in the primary module.
  IRGenModule *IGM = getPrimaryIGM();

  llvm::Function *RegisterFn = llvm::Function::Create(
                                llvm::FunctionType::get(IGM->VoidTy, false),
                                llvm::GlobalValue::PrivateLinkage,
                                "_swift_objc_actor_initialization");
  IGM->Module.getFunctionList().push_back(RegisterFn);
  IRGenFunction RegisterIGF(*IGM, RegisterFn);
  if (IGM->DebugInfo)
    IGM->DebugInfo->emitArtificialFunction(RegisterIGF, RegisterIGF.CurFn);
  RegisterFn->setAttributes(IGM->constructInitialAttributes());
  RegisterFn->setCallingConv(IGM->DefaultCC);
  IGM->setColocateMetadataSection(RegisterFn);

  // Look up the SwiftNativeNSObject class.
  auto swiftNativeNSObjectName =
      IGM->getAddrOfGlobalString("SwiftNativeNSObject");
  auto swiftNativeNSObjectClass = RegisterIGF.Builder.CreateCall(
      RegisterIGF.IGM.getObjCGetRequiredClassFunctionPointer(),
      swiftNativeNSObjectName);

  for (ClassDecl *CD : ObjCActorsNeedingSuperclassSwizzle) {
    // The @objc actor class.
    llvm::Value *classRef = RegisterIGF.emitTypeMetadataRef(
        CD->getDeclaredInterfaceType()->getCanonicalType());
    classRef = RegisterIGF.Builder.CreateBitCast(classRef, IGM->ObjCClassPtrTy);

    // Set its superclass to SwiftNativeNSObject.
    RegisterIGF.Builder.CreateCall(
        RegisterIGF.IGM.getSetSuperclassFunctionPointer(),
        {classRef, swiftNativeNSObjectClass});
  }
  RegisterIGF.Builder.CreateRetVoid();

  // Add the registration function as a static initializer. We use a priority
  // slightly lower than used for C++ global constructors, so that the code is
  // executed before C++ global constructors (in case someone manages to access
  // an @objc actor from a global constructor).
  llvm::appendToGlobalCtors(IGM->Module, RegisterFn, 60000, nullptr);
}

/// Emit symbols for eliminated dead methods, which can still be referenced
/// from other modules. This happens e.g. if a public class contains a (dead)
/// private method.
void IRGenModule::emitVTableStubs() {
  llvm::Function *stub = nullptr;
  for (auto I = getSILModule().zombies_begin();
           I != getSILModule().zombies_end(); ++I) {
    const SILFunction &F = *I;
    if (! F.isExternallyUsedSymbol())
      continue;
    
    if (!stub) {
      // Create a single stub function which calls swift_deletedMethodError().
      stub = llvm::Function::Create(llvm::FunctionType::get(VoidTy, false),
                                    llvm::GlobalValue::InternalLinkage,
                                    "_swift_dead_method_stub");
      stub->setAttributes(constructInitialAttributes());
      Module.getFunctionList().push_back(stub);
      stub->setCallingConv(DefaultCC);
      auto *entry = llvm::BasicBlock::Create(getLLVMContext(), "entry", stub);
      auto *errorFunc = getDeletedMethodErrorFn();
      llvm::CallInst::Create(getDeletedMethodErrorFnType(),
                             errorFunc, ArrayRef<llvm::Value *>(), "", entry);
      new llvm::UnreachableInst(getLLVMContext(), entry);
    }

    // For each eliminated method symbol create an alias to the stub.
    llvm::GlobalValue *alias = nullptr;
    if (F.isAsync()) {
      // TODO: We cannot directly create a pointer to `swift_deletedAsyncMethodError`
      // to workaround a linker crash.
      // Instead use the stub, which calls swift_deletedMethodError. This works because
      // swift_deletedMethodError takes no parameters and simply aborts the program.
      auto asyncLayout = getAsyncContextLayout(*this, const_cast<SILFunction *>(&F));
      auto entity = LinkEntity::forSILFunction(const_cast<SILFunction *>(&F));
      auto *fnPtr = emitAsyncFunctionPointer(*this, stub, entity, asyncLayout.getSize());
      alias = fnPtr;
    } else {
      alias = llvm::GlobalAlias::create(llvm::GlobalValue::ExternalLinkage,
                                        F.getName(), stub);
    }

    if (F.getEffectiveSymbolLinkage() == SILLinkage::Hidden)
      alias->setVisibility(llvm::GlobalValue::HiddenVisibility);
    else
      ApplyIRLinkage(IRGen.Opts.InternalizeSymbols
                        ? IRLinkage{llvm::GlobalValue::ExternalLinkage,
                                    llvm::GlobalValue::HiddenVisibility,
                                    llvm::GlobalValue::DefaultStorageClass}
                        : IRLinkage::ExternalExport).to(alias);
  }
}

static std::string getEntryPointSection(IRGenModule &IGM) {
  std::string sectionName;
  switch (IGM.TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_entry, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_entry";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5entr$B";
    break;
  }
  return sectionName;
}

void IRGenerator::emitEntryPointInfo() {
  SILFunction *entrypoint = nullptr;
  if (!(entrypoint = SIL.lookUpFunction(
            SIL.getASTContext().getEntryPointFunctionName()))) {
    return;
  }
  auto &IGM = *getGenModule(entrypoint);
  ConstantInitBuilder builder(IGM);
  auto entrypointInfo = builder.beginStruct();
  entrypointInfo.addCompactFunctionReference(
      IGM.getAddrOfSILFunction(entrypoint, NotForDefinition));
  uint32_t flags = 0;
  enum EntryPointFlags : unsigned {
    HasAtMainTypeFlag = 1 << 0,
  };
  if (auto *mainTypeDecl = SIL.getSwiftModule()->getMainTypeDecl()) {
    flags |= HasAtMainTypeFlag;
  }
  entrypointInfo.addInt(IGM.Int32Ty, flags);
  auto var = entrypointInfo.finishAndCreateGlobal(
      "\x01l_entry_point", Alignment(4),
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
  var->setSection(getEntryPointSection(IGM));
  IGM.addUsedGlobal(var);
}

static IRLinkage
getIRLinkage(StringRef name, const UniversalLinkageInfo &info,
             SILLinkage linkage, ForDefinition_t isDefinition,
             bool isWeakImported, bool isKnownLocal = false) {
#define RESULT(LINKAGE, VISIBILITY, DLL_STORAGE)                               \
  IRLinkage{llvm::GlobalValue::LINKAGE##Linkage,                               \
            llvm::GlobalValue::VISIBILITY##Visibility,                         \
            llvm::GlobalValue::DLL_STORAGE##StorageClass}

  // This is a synthetic symbol that is referenced for `#dsohandle` and is never
  // a definition but needs to be handled as a definition as it will be provided
  // by the linker. This is a MSVC extension that is honoured by lld as well.
  if (info.IsMSVCEnvironment && name == "__ImageBase")
    return RESULT(External, Default, Default);

  // Use protected visibility for public symbols we define on ELF.  ld.so
  // doesn't support relative relocations at load time, which interferes with
  // our metadata formats.  Default visibility should suffice for other object
  // formats.
  llvm::GlobalValue::VisibilityTypes PublicDefinitionVisibility =
      info.IsELFObject ? llvm::GlobalValue::ProtectedVisibility
                       : llvm::GlobalValue::DefaultVisibility;
  llvm::GlobalValue::DLLStorageClassTypes ExportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLExportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;
  llvm::GlobalValue::DLLStorageClassTypes ImportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLImportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;

  switch (linkage) {
  case SILLinkage::Public:
    return {llvm::GlobalValue::ExternalLinkage, PublicDefinitionVisibility,
            info.Internalize ? llvm::GlobalValue::DefaultStorageClass
                             : ExportedStorage};

  case SILLinkage::PublicNonABI:
    return isDefinition ? RESULT(WeakODR, Hidden, Default)
                        : RESULT(External, Hidden, Default);

  case SILLinkage::Shared:
    return isDefinition ? RESULT(LinkOnceODR, Hidden, Default)
                        : RESULT(External, Hidden, Default);

  case SILLinkage::Hidden:
    return RESULT(External, Hidden, Default);

  case SILLinkage::Private: {
    if (info.forcePublicDecls() && !isDefinition)
      return getIRLinkage(name, info, SILLinkage::PublicExternal, isDefinition,
                          isWeakImported, isKnownLocal);

    auto linkage = info.needLinkerToMergeDuplicateSymbols()
                       ? llvm::GlobalValue::LinkOnceODRLinkage
                       : llvm::GlobalValue::InternalLinkage;
    auto visibility = info.shouldAllPrivateDeclsBeVisibleFromOtherFiles()
                          ? llvm::GlobalValue::HiddenVisibility
                          : llvm::GlobalValue::DefaultVisibility;
    return {linkage, visibility, llvm::GlobalValue::DefaultStorageClass};
  }

  case SILLinkage::PublicExternal: {
    if (isDefinition)
      return RESULT(AvailableExternally, Default, Default);

    auto linkage = isWeakImported ? llvm::GlobalValue::ExternalWeakLinkage
                                  : llvm::GlobalValue::ExternalLinkage;
    return {linkage, llvm::GlobalValue::DefaultVisibility,
            isKnownLocal
                ? llvm::GlobalValue::DefaultStorageClass
                : ImportedStorage};
  }

  case SILLinkage::HiddenExternal:
    if (isDefinition)
      return RESULT(AvailableExternally, Hidden, Default);

    return {llvm::GlobalValue::ExternalLinkage,
            llvm::GlobalValue::DefaultVisibility,
            isKnownLocal
                ? llvm::GlobalValue::DefaultStorageClass
                : ImportedStorage};
  }

  llvm_unreachable("bad SIL linkage");
}

/// Given that we're going to define a global value but already have a
/// forward-declaration of it, update its linkage.
void irgen::updateLinkageForDefinition(IRGenModule &IGM,
                                       llvm::GlobalValue *global,
                                       const LinkEntity &entity) {
  // TODO: there are probably cases where we can avoid redoing the
  // entire linkage computation.
  UniversalLinkageInfo linkInfo(IGM);
  bool weakImported = entity.isWeakImported(IGM.getSwiftModule());

  bool isKnownLocal = entity.isAlwaysSharedLinkage();
  if (const auto *DC = entity.getDeclContextForEmission())
    if (const auto *MD = DC->getParentModule())
      isKnownLocal = IGM.getSwiftModule() == MD || MD->isStaticLibrary();

  auto IRL =
      getIRLinkage(global->hasName() ? global->getName() : StringRef(),
                   linkInfo, entity.getLinkage(ForDefinition), ForDefinition,
                   weakImported, isKnownLocal);
  ApplyIRLinkage(IRL).to(global);

  LinkInfo link = LinkInfo::get(IGM, entity, ForDefinition);
  markGlobalAsUsedBasedOnLinkage(IGM, link, global);
}

LinkInfo LinkInfo::get(IRGenModule &IGM, const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  return LinkInfo::get(UniversalLinkageInfo(IGM),
                       IGM.getSwiftModule(),
                       entity, isDefinition);
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo,
                       ModuleDecl *swiftModule,
                       const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  LinkInfo result;
  entity.mangle(result.Name);

  bool isKnownLocal = entity.isAlwaysSharedLinkage();
  if (const auto *DC = entity.getDeclContextForEmission()) {
    if (const auto *MD = DC->getParentModule())
      isKnownLocal = MD == swiftModule || MD->isStaticLibrary();
  } else if (entity.hasSILFunction()) {
    // SIL serialized entities (functions, witness tables, vtables) do not have
    // an associated DeclContext and are serialized into the current module.  As
    // a result, we explicitly handle SIL Functions here. We do not expect other
    // types to be referenced directly.
    isKnownLocal = entity.getSILFunction()->isStaticallyLinked();
  }

  bool weakImported = entity.isWeakImported(swiftModule);
  result.IRL = getIRLinkage(result.Name, linkInfo,
                            entity.getLinkage(isDefinition), isDefinition,
                            weakImported, isKnownLocal);
  result.ForDefinition = isDefinition;
  return result;
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo, StringRef name,
                       SILLinkage linkage, ForDefinition_t isDefinition,
                       bool isWeakImported) {
  LinkInfo result;
  result.Name += name;
  result.IRL = getIRLinkage(name, linkInfo, linkage, isDefinition,
                            isWeakImported, linkInfo.Internalize);
  result.ForDefinition = isDefinition;
  return result;
}

/// Get or create an LLVM function with these linkage rules.
llvm::Function *irgen::createFunction(IRGenModule &IGM, LinkInfo &linkInfo,
                                      const Signature &signature,
                                      llvm::Function *insertBefore,
                                      OptimizationMode FuncOptMode,
                                      StackProtectorMode stackProtect) {
  auto name = linkInfo.getName();

  llvm::Function *existing = IGM.Module.getFunction(name);
  if (existing) {
    if (existing->getValueType() == signature.getType())
      return cast<llvm::Function>(existing);

    IGM.error(SourceLoc(),
              "program too clever: function collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existing->setName(name + ".unique");
  }

  llvm::Function *fn =
    llvm::Function::Create(signature.getType(), linkInfo.getLinkage(), name);
  fn->setCallingConv(signature.getCallingConv());

  if (insertBefore) {
    IGM.Module.getFunctionList().insert(insertBefore->getIterator(), fn);
  } else {
    IGM.Module.getFunctionList().push_back(fn);
  }

  ApplyIRLinkage({linkInfo.getLinkage(),
                  linkInfo.getVisibility(),
                  linkInfo.getDLLStorage()})
      .to(fn, linkInfo.isForDefinition());

  llvm::AttrBuilder initialAttrs(IGM.getLLVMContext());
  IGM.constructInitialFnAttributes(initialAttrs, FuncOptMode, stackProtect);
  // Merge initialAttrs with attrs.
  auto updatedAttrs = signature.getAttributes().addFnAttributes(
      IGM.getLLVMContext(), initialAttrs);
  if (!updatedAttrs.isEmpty())
    fn->setAttributes(updatedAttrs);

  markGlobalAsUsedBasedOnLinkage(IGM, linkInfo, fn);

  return fn;
}

/// Get or create an LLVM global variable with these linkage rules.
llvm::GlobalVariable *swift::irgen::createVariable(
    IRGenModule &IGM, LinkInfo &linkInfo, llvm::Type *storageType,
    Alignment alignment, DebugTypeInfo DbgTy,
    llvm::Optional<SILLocation> DebugLoc, StringRef DebugName) {
  auto name = linkInfo.getName();
  llvm::GlobalValue *existingValue = IGM.Module.getNamedGlobal(name);
  if (existingValue) {
    auto existingVar = dyn_cast<llvm::GlobalVariable>(existingValue);
    if (existingVar && existingVar->getValueType() == storageType)
      return existingVar;

    IGM.error(SourceLoc(),
              "program too clever: variable collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existingValue->setName(name + ".unique");
  }

  auto var = new llvm::GlobalVariable(IGM.Module, storageType,
                                      /*constant*/ false, linkInfo.getLinkage(),
                                      /*initializer*/ nullptr, name);
  ApplyIRLinkage({linkInfo.getLinkage(),
                  linkInfo.getVisibility(),
                  linkInfo.getDLLStorage()})
      .to(var, linkInfo.isForDefinition());
  var->setAlignment(llvm::MaybeAlign(alignment.getValue()));

  markGlobalAsUsedBasedOnLinkage(IGM, linkInfo, var);

  if (IGM.DebugInfo && !DbgTy.isNull() && linkInfo.isForDefinition())
    IGM.DebugInfo->emitGlobalVariableDeclaration(
        var, DebugName.empty() ? name : DebugName, name, DbgTy,
        var->hasInternalLinkage(), DebugLoc);

  return var;
}

llvm::GlobalVariable *
swift::irgen::createLinkerDirectiveVariable(IRGenModule &IGM, StringRef name) {

  // A prefix of \1 can avoid further mangling of the symbol (prefixing _).
  llvm::SmallString<32> NameWithFlag;
  NameWithFlag.push_back('\1');
  NameWithFlag.append(name);
  name = NameWithFlag.str();
  static const uint8_t Size = 8;
  static const uint8_t Alignment = 8;

  // Use a char type as the type for this linker directive.
  auto ProperlySizedIntTy = SILType::getBuiltinIntegerType(
      Size, IGM.getSwiftModule()->getASTContext());
  auto storageType = IGM.getStorageType(ProperlySizedIntTy);

  llvm::GlobalValue *existingValue = IGM.Module.getNamedGlobal(name);
  if (existingValue) {
    auto existingVar = dyn_cast<llvm::GlobalVariable>(existingValue);
    if (existingVar && existingVar->getValueType() == storageType)
      return existingVar;

    IGM.error(SourceLoc(),
              "program too clever: variable collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existingValue->setName(name + ".unique");
  }

  llvm::GlobalValue::LinkageTypes Linkage =
    llvm::GlobalValue::LinkageTypes::ExternalLinkage;
  auto var = new llvm::GlobalVariable(IGM.Module, storageType, /*constant*/true,
    Linkage, /*Init to zero*/llvm::Constant::getNullValue(storageType), name);
  ApplyIRLinkage({Linkage,
                  llvm::GlobalValue::VisibilityTypes::DefaultVisibility,
        llvm::GlobalValue::DLLStorageClassTypes::DefaultStorageClass}).to(var);
  var->setAlignment(llvm::MaybeAlign(Alignment));
  disableAddressSanitizer(IGM, var);
  IGM.addUsedGlobal(var);
  return var;
}

void swift::irgen::disableAddressSanitizer(IRGenModule &IGM, llvm::GlobalVariable *var) {
  llvm::GlobalVariable::SanitizerMetadata Meta;
  if (var->hasSanitizerMetadata())
    Meta = var->getSanitizerMetadata();
  Meta.IsDynInit = false;
  Meta.NoAddress = true;
  var->setSanitizerMetadata(Meta);
}

/// Emit a global declaration.
void IRGenModule::emitGlobalDecl(Decl *D) {
  if (Lowering::shouldSkipLowering(D))
    return;

  D->visitAuxiliaryDecls([&](Decl *decl) {
    emitGlobalDecl(decl);
  });

  switch (D->getKind()) {
  case DeclKind::Extension:
    return emitExtension(cast<ExtensionDecl>(D));

  case DeclKind::Protocol:
    return emitProtocolDecl(cast<ProtocolDecl>(D));

  case DeclKind::PatternBinding:
    // The global initializations are in SIL.
    return;

  case DeclKind::Param:
    llvm_unreachable("there are no global function parameters");

  case DeclKind::Subscript:
    llvm_unreachable("there are no global subscript operations");
      
  case DeclKind::EnumCase:
  case DeclKind::EnumElement:
    llvm_unreachable("there are no global enum elements");

  case DeclKind::Constructor:
    llvm_unreachable("there are no global constructor");

  case DeclKind::Destructor:
    llvm_unreachable("there are no global destructor");

  case DeclKind::MissingMember:
    llvm_unreachable("there are no global member placeholders");

  case DeclKind::Missing:
    llvm_unreachable("missing decl in IRGen");

  case DeclKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType made it to IRGen");

  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::IfConfig: 
  case DeclKind::PoundDiagnostic:
  case DeclKind::Macro:
    return;

  case DeclKind::Enum:
    return emitEnumDecl(cast<EnumDecl>(D));

  case DeclKind::Struct:
    return emitStructDecl(cast<StructDecl>(D));

  case DeclKind::Class:
    return emitClassDecl(cast<ClassDecl>(D));

  // These declarations are only included in the debug info.
  case DeclKind::Import:
    if (DebugInfo)
      DebugInfo->emitImport(cast<ImportDecl>(D));
    return;

  case DeclKind::Var:
  case DeclKind::Accessor:
  case DeclKind::Func:
    // Handled in SIL.
    return;
  
  case DeclKind::TopLevelCode:
    // All the top-level code will be lowered separately.
    return;
      
  // Operator decls aren't needed for IRGen.
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::PrecedenceGroup:
    return;

  case DeclKind::Module:
    return;
      
  case DeclKind::OpaqueType:
    // TODO: Eventually we'll need to emit descriptors to access the opaque
    // type's metadata.
    return;

  case DeclKind::MacroExpansion:
    // Expansion already visited as auxiliary decls.
    return;
  }

  llvm_unreachable("bad decl kind!");
}

Address IRGenModule::getAddrOfSILGlobalVariable(SILGlobalVariable *var,
                                                const TypeInfo &ti,
                                                ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forSILGlobalVariable(var, *this);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  if (auto clangDecl = var->getClangDecl()) {
    auto addr = getAddrOfClangGlobalDecl(cast<clang::VarDecl>(clangDecl),
                                         forDefinition);

    // Override the linkage computed by Clang if the decl is from another
    // module that imported @_weakLinked.
    //
    // FIXME: We should be able to set the linkage unconditionally here but
    //        some fixes are needed for Cxx interop.
    if (auto globalVar = dyn_cast<llvm::GlobalVariable>(addr)) {
      auto varModule = var->getDecl()->getModuleContext();
      if (getSwiftModule()->isImportedAsWeakLinked(varModule))
        globalVar->setLinkage(link.getLinkage());
    }

    // If we're not emitting this to define it, make sure we cast it to the
    // right type.
    if (!forDefinition) {
      auto ptrTy = ti.getStorageType()->getPointerTo();
      addr = llvm::ConstantExpr::getBitCast(addr, ptrTy);
    }

    auto alignment =
      Alignment(getClangASTContext().getDeclAlign(clangDecl).getQuantity());
    return Address(addr, ti.getStorageType(), alignment);
  }

  ResilienceExpansion expansion = getResilienceExpansionForLayout(var);

  llvm::Type *storageType;
  llvm::Type *castStorageToType = nullptr;
  Size fixedSize;
  Alignment fixedAlignment;
  bool inFixedBuffer = false;

  if (var->isInitializedObject()) {
    assert(ti.isFixedSize(expansion));
    StructLayout *Layout = StaticObjectLayouts[var].get();
    if (!Layout) {
      // Create the layout (includes the llvm type) for the statically
      // initialized object and store it for later.
      ObjectInst *OI = cast<ObjectInst>(var->getStaticInitializerValue());
      llvm::SmallVector<SILType, 16> TailTypes;
      for (SILValue TailOp : OI->getTailElements()) {
        TailTypes.push_back(TailOp->getType());
      }
      Layout = getClassLayoutWithTailElems(*this,
                                           var->getLoweredType(), TailTypes);
      StaticObjectLayouts[var] = std::unique_ptr<StructLayout>(Layout);
    }
    storageType = Layout->getType();
    fixedSize = Layout->getSize();
    fixedAlignment = Layout->getAlignment();
    castStorageToType = cast<ClassTypeInfo>(ti).getStorageType();
    assert(fixedAlignment >= TargetInfo.HeapObjectAlignment);
  } else if (ti.isFixedSize(expansion)) {
    // Allocate static storage.
    auto &fixedTI = cast<FixedTypeInfo>(ti);
    storageType = fixedTI.getStorageType();
    fixedSize = fixedTI.getFixedSize();
    fixedAlignment = fixedTI.getFixedAlignment();
  } else {
    // Allocate a fixed-size buffer and possibly heap-allocate a payload at
    // runtime if the runtime size of the type does not fit in the buffer.
    inFixedBuffer = true;
    storageType = getFixedBufferTy();
    fixedSize = Size(DataLayout.getTypeAllocSize(storageType));
    fixedAlignment = getFixedBufferAlignment(*this);
  }

  llvm::Constant *initVal = nullptr;

  // Check whether we've created the global variable already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  auto gvar = Module.getGlobalVariable(link.getName(), /*allowInternal*/ true);
  if (gvar) {
    if (forDefinition) {
      updateLinkageForDefinition(*this, gvar, entity);

      if (var->getStaticInitializerValue()) {
        assert(gvar->hasInitializer() &&
               "global variable referenced before created");
      }
    }
    if (forDefinition && !gvar->hasInitializer())
      initVal = getGlobalInitValue(var, storageType, fixedAlignment);
  } else {
    // The global doesn't exist yet. Create it.

    initVal = getGlobalInitValue(var, storageType, fixedAlignment);
    llvm::Type *globalTy = initVal ? initVal->getType() : storageType;

    if (var->isInitializedObject()) {
      // An initialized object is always a compiler-generated "outlined"
      // variable. Therefore we don't generate debug info for it.
      gvar = createVariable(*this, link, globalTy, fixedAlignment);
    } else {
      // Create a global variable with debug info.
      StringRef name;
      llvm::Optional<SILLocation> loc;
      if (var->getDecl()) {
        // Use the VarDecl for more accurate debugging information.
        loc = var->getDecl();
        name = var->getDecl()->getName().str();
      } else {
        if (var->hasLocation())
          loc = var->getLocation();
        name = var->getName();
      }

      DebugTypeInfo DbgTy =
          inFixedBuffer
              ? DebugTypeInfo::getGlobalFixedBuffer(
                    var, globalTy, fixedSize, fixedAlignment)
              : DebugTypeInfo::getGlobal(var, globalTy, *this);

      gvar = createVariable(*this, link, globalTy, fixedAlignment, DbgTy, loc, name);
    }
    if (!forDefinition)
      gvar->setComdat(nullptr);

    // Mark as llvm.used if @_used, set section if @_section
    if (var->markedAsUsed())
      addUsedGlobal(gvar);
    if (auto *sectionAttr = var->getSectionAttr())
      gvar->setSection(sectionAttr->Name);
  }
  if (forDefinition && !gvar->hasInitializer()) {
    if (initVal) {
      gvar->setInitializer(initVal);
      if (var->isLet() ||
          (var->isInitializedObject() && canMakeStaticObjectsReadOnly())) {
        gvar->setConstant(true);
      }
    } else {
      /// Add a zero initializer.
      gvar->setInitializer(llvm::Constant::getNullValue(storageType));
    }
  }

  llvm::Constant *addr = gvar;
  if (var->isInitializedObject() && !canMakeStaticObjectsReadOnly()) {
    // Project out the object from the container.
    llvm::Constant *Indices[2] = {
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 0)),
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 1))
    };
    // Return the address of the initialized object itself (and not the address
    // to a reference to it).
    addr = llvm::ConstantExpr::getGetElementPtr(
      gvar->getValueType(), gvar, Indices);
  }

  addr = llvm::ConstantExpr::getBitCast(
      addr,
      castStorageToType ? castStorageToType : storageType->getPointerTo());
  if (castStorageToType)
    storageType = cast<ClassTypeInfo>(ti).getClassLayoutType();

  return Address(addr, storageType, Alignment(gvar->getAlignment()));
}

llvm::Constant *IRGenModule::getGlobalInitValue(SILGlobalVariable *var,
                                                llvm::Type *storageType,
                                                Alignment alignment) {
  if (var->isInitializedObject()) {
    StructLayout *layout = StaticObjectLayouts[var].get();
    ObjectInst *oi = cast<ObjectInst>(var->getStaticInitializerValue());
    llvm::Constant *initVal = emitConstantObject(*this, oi, layout);
    if (!canMakeStaticObjectsReadOnly()) {
      // A statically initialized object must be placed into a container struct
      // because the swift_initStaticObject needs a swift_once_t at offset -1:
      //     struct Container {
      //       swift_once_t token[fixedAlignment / sizeof(swift_once_t)];
      //       HeapObject object;
      //     };
      std::string typeName = storageType->getStructName().str() + 'c';
      assert(alignment >= getPointerAlignment());
      unsigned numTokens = alignment.getValue() /
                           getPointerAlignment().getValue();
      auto *containerTy = llvm::StructType::create(getLLVMContext(),
              {llvm::ArrayType::get(OnceTy, numTokens), initVal->getType()},
              typeName);
      auto *zero = llvm::ConstantAggregateZero::get(containerTy->getElementType(0));
      initVal = llvm::ConstantStruct::get(containerTy, {zero , initVal});
    }
    return initVal;
  }
  if (SILInstruction *initInst = var->getStaticInitializerValue()) {
    Explosion initExp = emitConstantValue(*this,
                                  cast<SingleValueInstruction>(initInst));
    if (initExp.size() == 1) {
      return initExp.claimNextConstant();
    }
    // In case of enums, the initializer might contain multiple constants,
    // which does not match with the storage type.
    ArrayRef<llvm::Value *> elements = initExp.claimAll();
    ArrayRef<llvm::Constant *> constElements(
                  (llvm::Constant *const*)elements.data(), elements.size());
    return llvm::ConstantStruct::getAnon(constElements, /*Packed=*/ true);
  }
  return nullptr;
}

/// Return True if the function \p f is a 'readonly' function. Checking
/// for the SIL @_effects(readonly) attribute is not enough because this
/// definition does not match the definition of the LLVM readonly function
/// attribute. In this function we do the actual check.
static bool isReadOnlyFunction(SILFunction *f) {
  // Check if the function has any 'owned' parameters. Owned parameters may
  // call the destructor of the object which could violate the readonly-ness
  // of the function.
  if (f->hasOwnedParameters() || f->hasIndirectFormalResults())
    return false;

  auto Eff = f->getEffectsKind();

  // Swift's readonly does not automatically match LLVM's readonly.
  // Swift SIL optimizer relies on @_effects(readonly) to remove e.g.
  // dead code remaining from initializers of strings or dictionaries
  // of variables that are not used. But those initializers are often
  // not really readonly in terms of LLVM IR. For example, the
  // Dictionary.init() is marked as @_effects(readonly) in Swift, but
  // it does invoke reference-counting operations.
  if (Eff == EffectsKind::ReadOnly || Eff == EffectsKind::ReadNone) {
    // TODO: Analyze the body of function f and return true if it is
    // really readonly.
    return false;
  }

  return false;
}

static clang::GlobalDecl getClangGlobalDeclForFunction(const clang::Decl *decl) {
  if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(decl))
    return clang::GlobalDecl(ctor, clang::Ctor_Complete);
  if (auto dtor = dyn_cast<clang::CXXDestructorDecl>(decl))
    return clang::GlobalDecl(dtor, clang::Dtor_Complete);
  return clang::GlobalDecl(cast<clang::FunctionDecl>(decl));
}

static void addLLVMFunctionAttributes(SILFunction *f, Signature &signature) {
  auto &attrs = signature.getMutableAttributes();
  switch (f->getInlineStrategy()) {
  case NoInline:
    attrs = attrs.addFnAttribute(signature.getType()->getContext(),
                                 llvm::Attribute::NoInline);
    break;
  case AlwaysInline:
    // FIXME: We do not currently transfer AlwaysInline since doing so results
    // in test failures, which must be investigated first.
  case InlineDefault:
    break;
  }

  if (isReadOnlyFunction(f)) {
    attrs = attrs.addFnAttribute(signature.getType()->getContext(),
                                 llvm::Attribute::ReadOnly);
  }
}

/// Create a key entry for a dynamic function replacement. A key entry refers to
/// the link entry for the dynamic replaceable function.
/// struct KeyEntry {
///   RelativeDirectPointer<LinkEntry> linkEntry;
///   int32_t flags;
/// }
static llvm::GlobalVariable *createGlobalForDynamicReplacementFunctionKey(
    IRGenModule &IGM, LinkEntity keyEntity, llvm::GlobalVariable *linkEntry) {
  auto key = IGM.getGlobalForDynamicallyReplaceableThunk(
      keyEntity, IGM.DynamicReplacementKeyTy, ForDefinition);

  ConstantInitBuilder builder(IGM);
  auto B = builder.beginStruct(IGM.DynamicReplacementKeyTy);
  B.addRelativeAddress(linkEntry);
  bool isAsyncFunction =
      keyEntity.hasSILFunction() && keyEntity.getSILFunction()->isAsync();
  auto schema = isAsyncFunction
                    ? IGM.getOptions().PointerAuth.AsyncSwiftDynamicReplacements
                    : IGM.getOptions().PointerAuth.SwiftDynamicReplacements;
  if (schema) {
    assert(keyEntity.hasSILFunction() ||
           keyEntity.isOpaqueTypeDescriptorAccessor());
    auto authEntity = keyEntity.hasSILFunction()
                          ? PointerAuthEntity(keyEntity.getSILFunction())
                          : PointerAuthEntity::Special::TypeDescriptor;
    B.addInt32((uint32_t)PointerAuthInfo::getOtherDiscriminator(IGM, schema,
                                                                authEntity)
                   ->getZExtValue() |
               ((uint32_t)isAsyncFunction ? 0x10000 : 0x0));
  } else
    B.addInt32(0);
  B.finishAndSetAsInitializer(key);
  key->setConstant(true);
  IGM.setTrueConstGlobal(key);
  return key;
}

/// Creates the prolog for a dynamically replaceable function.
/// It checks if the replaced function or the original function should be called
/// (by calling the swift_getFunctionReplacement runtime function). In case of
/// the original function, it just jumps to the "real" code of the function,
/// otherwise it tail calls the replacement.
void IRGenModule::createReplaceableProlog(IRGenFunction &IGF, SILFunction *f) {
  LinkEntity varEntity =
      LinkEntity::forDynamicallyReplaceableFunctionVariable(f);
  LinkEntity keyEntity =
      LinkEntity::forDynamicallyReplaceableFunctionKey(f);
  auto silFunctionType = f->getLoweredFunctionType();
  Signature signature = getSignature(silFunctionType);

  // Create and initialize the first link entry for the chain of replacements.
  // The first implementation is initialized with 'implFn'.
  auto *funPtr =
      f->isAsync()
          ? IGF.IGM.getAddrOfAsyncFunctionPointer(LinkEntity::forSILFunction(f))
          : IGF.CurFn;
  auto linkEntry =
      getChainEntryForDynamicReplacement(*this, varEntity, funPtr);

  // Create the key data structure. This is used from other modules to refer to
  // the chain of replacements.
  createGlobalForDynamicReplacementFunctionKey(*this, keyEntity, linkEntry);

  llvm::Constant *indices[] = {llvm::ConstantInt::get(Int32Ty, 0),
                               llvm::ConstantInt::get(Int32Ty, 0)};

  auto *fnPtrAddr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      linkEntry->getValueType(), linkEntry, indices);

  auto *ReplAddr =
    llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(fnPtrAddr,
      FunctionPtrTy->getPointerTo());

  auto *FnAddr = llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
      funPtr, FunctionPtrTy);

  auto &schema = f->isAsync()
                     ? getOptions().PointerAuth.AsyncSwiftDynamicReplacements
                     : getOptions().PointerAuth.SwiftDynamicReplacements;
  llvm::Value *ReplFn = nullptr, *hasReplFn = nullptr;

  if (UseBasicDynamicReplacement) {
    ReplFn = IGF.Builder.CreateLoad(
        Address(fnPtrAddr, FunctionPtrTy, getPointerAlignment()));
    llvm::Value *lhs = ReplFn;
    if (schema.isEnabled()) {
      lhs = emitPointerAuthStrip(IGF, lhs, schema.getKey());
    }
    hasReplFn = IGF.Builder.CreateICmpEQ(lhs, FnAddr);
  } else {
    // Call swift_getFunctionReplacement to check which function to call.
    auto *callRTFunc = IGF.Builder.CreateCall(
        getGetReplacementFunctionPointer(), {ReplAddr, FnAddr});
    callRTFunc->setDoesNotThrow();
    ReplFn = callRTFunc;
    hasReplFn = IGF.Builder.CreateICmpEQ(ReplFn,
                  llvm::ConstantExpr::getNullValue(ReplFn->getType()));
  }

  auto *replacedBB = IGF.createBasicBlock("forward_to_replaced");
  auto *origEntryBB = IGF.createBasicBlock("original_entry");
  IGF.Builder.CreateCondBr(hasReplFn, origEntryBB, replacedBB);

  IGF.Builder.emitBlock(replacedBB);
  if (f->isAsync()) {
    auto &IGM = IGF.IGM;
    auto &Builder = IGF.Builder;
    PrologueLocation AutoRestore(IGM.DebugInfo.get(), Builder);
    auto authEntity = PointerAuthEntity(f);
    auto authInfo = PointerAuthInfo::emit(IGF, schema, fnPtrAddr, authEntity);
    auto *fnType = signature.getType()->getPointerTo();
    auto *realReplFn = Builder.CreateBitCast(ReplFn, fnType);
    auto asyncFnPtr = FunctionPointer::createSigned(silFunctionType, realReplFn,
                                                    authInfo, signature);
    // We never emit a function that uses special conventions, so we
    // can just use the default async kind.
    FunctionPointerKind fpKind =
        FunctionPointerKind::AsyncFunctionPointer;
    PointerAuthInfo codeAuthInfo =
        asyncFnPtr.getAuthInfo().getCorrespondingCodeAuthInfo();
    auto newFnPtr = FunctionPointer::createSigned(
        FunctionPointer::Kind::Function, asyncFnPtr.getPointer(IGF),
        codeAuthInfo, Signature::forAsyncAwait(IGM, silFunctionType, fpKind));
    SmallVector<llvm::Value *, 16> forwardedArgs;
    for (auto &arg : IGF.CurFn->args())
      forwardedArgs.push_back(&arg);
    auto layout = getAsyncContextLayout(
        IGM, silFunctionType, silFunctionType,
        f->getForwardingSubstitutionMap());
    llvm::Value *dynamicContextSize32;
    llvm::Value *calleeFunction;
    std::tie(calleeFunction, dynamicContextSize32) = getAsyncFunctionAndSize(
        IGF, silFunctionType->getRepresentation(), asyncFnPtr, nullptr,
        std::make_pair(false, true));
    auto *dynamicContextSize =
        Builder.CreateZExt(dynamicContextSize32, IGM.SizeTy);
    auto calleeContextBuffer = emitAllocAsyncContext(IGF, dynamicContextSize);
    auto calleeContext =
        layout.emitCastTo(IGF, calleeContextBuffer.getAddress());
    auto saveValue = [&](ElementLayout layout, Explosion &explosion) -> void {
      Address addr = layout.project(IGF, calleeContext, /*offsets*/ llvm::None);
      auto &ti = cast<LoadableTypeInfo>(layout.getType());
      ti.initialize(IGF, explosion, addr, /*isOutlined*/ false);
    };

    // Set caller info into the context.
    { // caller context
      Explosion explosion;
      auto fieldLayout = layout.getParentLayout();
      auto *context = IGF.getAsyncContext();
      if (auto schema = IGM.getOptions().PointerAuth.AsyncContextParent) {
        Address fieldAddr =
            fieldLayout.project(IGF, calleeContext, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        context = emitPointerAuthSign(IGF, context, authInfo);
      }
      explosion.add(context);
      saveValue(fieldLayout, explosion);
    }

    auto currentResumeFn =
        Builder.CreateIntrinsicCall(llvm::Intrinsic::coro_async_resume, {});
    { // Return to caller function.
      auto fieldLayout = layout.getResumeParentLayout();
      llvm::Value *fnVal = currentResumeFn;
      // Sign the pointer.
      if (auto schema = IGM.getOptions().PointerAuth.AsyncContextResume) {
        Address fieldAddr =
            fieldLayout.project(IGF, calleeContext, /*offsets*/ llvm::None);
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, fieldAddr.getAddress(), PointerAuthEntity());
        fnVal = emitPointerAuthSign(IGF, fnVal, authInfo);
      }
      fnVal = Builder.CreateBitCast(fnVal, IGM.TaskContinuationFunctionPtrTy);
      Explosion explosion;
      explosion.add(fnVal);
      saveValue(fieldLayout, explosion);
    }

    // Setup the suspend point.
    SmallVector<llvm::Value *, 8> arguments;
    auto signature = newFnPtr.getSignature();
    auto asyncContextIndex = signature.getAsyncContextIndex();
    auto paramAttributeFlags =
        asyncContextIndex |
        (signature.getAsyncResumeFunctionSwiftSelfIndex() << 8);
    // Index of swiftasync context | ((index of swiftself) << 8).
    arguments.push_back(IGM.getInt32(paramAttributeFlags));
    arguments.push_back(currentResumeFn);
    auto resumeProjFn = IGF.getOrCreateResumePrjFn(true /*forProlog*/);
    arguments.push_back(
        Builder.CreateBitOrPointerCast(resumeProjFn, IGM.Int8PtrTy));
    auto dispatchFn = IGF.createAsyncDispatchFn(
        getFunctionPointerForDispatchCall(IGM, newFnPtr), forwardedArgs);
    arguments.push_back(
        Builder.CreateBitOrPointerCast(dispatchFn, IGM.Int8PtrTy));
    arguments.push_back(Builder.CreateBitOrPointerCast(newFnPtr.getRawPointer(),
                                                       IGM.Int8PtrTy));
    if (auto authInfo = newFnPtr.getAuthInfo()) {
      arguments.push_back(newFnPtr.getAuthInfo().getDiscriminator());
    }
    unsigned argIdx = 0;
    for (auto arg : forwardedArgs) {
      // Replace the context argument.
      if (argIdx == asyncFnPtr.getSignature().getAsyncContextIndex())
        arguments.push_back(Builder.CreateBitOrPointerCast(
            calleeContextBuffer.getAddress(), IGM.SwiftContextPtrTy));
      else
        arguments.push_back(arg);
      argIdx++;
    }
    auto resultTy =
        cast<llvm::StructType>(signature.getType()->getReturnType());
    auto suspend = IGF.emitSuspendAsyncCall(
        asyncContextIndex, resultTy, arguments, false /*restore context*/);
    { // Restore the context.
      llvm::Value *calleeContext =
          Builder.CreateExtractValue(suspend, asyncContextIndex);
      auto context = IGF.emitAsyncResumeProjectContext(calleeContext);
      IGF.storeCurrentAsyncContext(context);
    }

    emitDeallocAsyncContext(IGF, calleeContextBuffer);
    forwardAsyncCallResult(IGF, silFunctionType, layout, suspend);
  } else {
    // Call the replacement function.
    SmallVector<llvm::Value *, 16> forwardedArgs;
    for (auto &arg : IGF.CurFn->args())
      forwardedArgs.push_back(&arg);
    auto *fnType = signature.getType()->getPointerTo();
    auto *realReplFn = IGF.Builder.CreateBitCast(ReplFn, fnType);

    auto authEntity = PointerAuthEntity(f);
    auto authInfo = PointerAuthInfo::emit(IGF, schema, fnPtrAddr, authEntity);

    auto *Res = IGF.Builder.CreateCall(
        FunctionPointer::createSigned(silFunctionType, realReplFn, authInfo,
                                      signature)
            .getAsFunction(IGF),
        forwardedArgs);
    if (Res->getCallingConv() == llvm::CallingConv::SwiftTail &&
        Res->getCaller()->getCallingConv() == llvm::CallingConv::SwiftTail) {
      Res->setTailCallKind(IGF.IGM.AsyncTailCallKind);
    } else {
      Res->setTailCall();
    }
    if (IGF.CurFn->getReturnType()->isVoidTy())
      IGF.Builder.CreateRetVoid();
    else
      IGF.Builder.CreateRet(Res);
  }
  IGF.Builder.emitBlock(origEntryBB);

}

/// Emit the thunk that dispatches to the dynamically replaceable function.
static void emitDynamicallyReplaceableThunk(IRGenModule &IGM,
                                            LinkEntity varEntity,
                                            LinkEntity keyEntity,
                                            llvm::Function *dispatchFn,
                                            llvm::Function *implFn,
                                            Signature &signature) {

  // Create and initialize the first link entry for the chain of replacements.
  // The first implementation is initialized with 'implFn'.
  auto linkEntry = getChainEntryForDynamicReplacement(IGM, varEntity, implFn);

  // Create the key data structure. This is used from other modules to refer to
  // the chain of replacements.
  createGlobalForDynamicReplacementFunctionKey(IGM, keyEntity, linkEntry);

  // We should never inline the implementation function.
  implFn->addFnAttr(llvm::Attribute::NoInline);

  // Load the function and dispatch to it forwarding our arguments.
  IRGenFunction IGF(IGM, dispatchFn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, dispatchFn);
  llvm::Constant *indices[] = {llvm::ConstantInt::get(IGM.Int32Ty, 0),
                               llvm::ConstantInt::get(IGM.Int32Ty, 0)};

  auto *fnPtrAddr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      linkEntry->getValueType(), linkEntry, indices);
  auto *fnPtr = IGF.Builder.CreateLoad(
      Address(fnPtrAddr, IGM.FunctionPtrTy, IGM.getPointerAlignment()));
  auto *typeFnPtr = IGF.Builder.CreateBitOrPointerCast(fnPtr, implFn->getType());

  SmallVector<llvm::Value *, 16> forwardedArgs;
  for (auto &arg : dispatchFn->args())
    forwardedArgs.push_back(&arg);
  bool isAsyncFunction =
      keyEntity.hasSILFunction() && keyEntity.getSILFunction()->isAsync();
  auto &schema =
      isAsyncFunction
          ? IGM.getOptions().PointerAuth.AsyncSwiftDynamicReplacements
          : IGM.getOptions().PointerAuth.SwiftDynamicReplacements;
  assert(keyEntity.hasSILFunction() ||
         keyEntity.isOpaqueTypeDescriptorAccessor());
  auto authEntity = keyEntity.hasSILFunction()
                        ? PointerAuthEntity(keyEntity.getSILFunction())
                        : PointerAuthEntity::Special::TypeDescriptor;
  auto authInfo = PointerAuthInfo::emit(IGF, schema, fnPtrAddr, authEntity);
  auto *Res = IGF.Builder.CreateCall(
      FunctionPointer::createSigned(FunctionPointer::Kind::Function, typeFnPtr,
                                    authInfo, signature),
      forwardedArgs);

  Res->setTailCall();
  if (implFn->getReturnType()->isVoidTy())
    IGF.Builder.CreateRetVoid();
  else
    IGF.Builder.CreateRet(Res);
}

void IRGenModule::emitOpaqueTypeDescriptorAccessor(OpaqueTypeDecl *opaque) {
  auto *namingDecl = opaque->getNamingDecl();
  auto *abstractStorage = dyn_cast<AbstractStorageDecl>(namingDecl);

  bool isNativeDynamic = false;
  const bool isDynamicReplacement = namingDecl->getDynamicallyReplacedDecl();

  // Don't emit accessors for abstract storage that is not dynamic or a dynamic
  // replacement.
  if (abstractStorage) {
    isNativeDynamic = abstractStorage->hasAnyNativeDynamicAccessors();
    if (!isNativeDynamic && !isDynamicReplacement)
      return;
  }

  // Don't emit accessors for functions that are not dynamic or dynamic
  // replacements.
  if (!abstractStorage) {
    isNativeDynamic = namingDecl->shouldUseNativeDynamicDispatch();
    if (!isNativeDynamic && !isDynamicReplacement)
      return;
  }

  auto accessor = cast<llvm::Function>(
      getAddrOfOpaqueTypeDescriptorAccessFunction(opaque, ForDefinition, false)
          .getDirectPointer());

  if (isNativeDynamic) {
    auto thunk = accessor;
    auto impl = cast<llvm::Function>(
        getAddrOfOpaqueTypeDescriptorAccessFunction(opaque, ForDefinition, true)
            .getDirectPointer());
    auto varEntity = LinkEntity::forOpaqueTypeDescriptorAccessorVar(opaque);
    auto keyEntity = LinkEntity::forOpaqueTypeDescriptorAccessorKey(opaque);

    auto fnType = llvm::FunctionType::get(OpaqueTypeDescriptorPtrTy, {}, false);
    Signature signature(fnType, llvm::AttributeList(), SwiftCC);
    emitDynamicallyReplaceableThunk(*this, varEntity, keyEntity, thunk, impl,
                                    signature);
    // We should never inline the thunk function.
    thunk->addFnAttr(llvm::Attribute::NoInline);
    accessor = impl;
  }

  // The implementation just returns the opaque type descriptor.
  llvm::BasicBlock *entryBB =
      llvm::BasicBlock::Create(getLLVMContext(), "entry", accessor);
  IRBuilder B(getLLVMContext(), false);
  B.SetInsertPoint(entryBB);
  if (DebugInfo)
    DebugInfo->emitArtificialFunction(B, accessor);
  auto *desc = getAddrOfOpaqueTypeDescriptor(opaque, ConstantInit());
  B.CreateRet(desc);
}

/// Calls the previous implementation before this dynamic replacement became
/// active.
void IRGenModule::emitDynamicReplacementOriginalFunctionThunk(SILFunction *f) {
  assert(f->getDynamicallyReplacedFunction());

  if (UseBasicDynamicReplacement)
    return;

  auto entity = LinkEntity::forSILFunction(f, true);

  auto fnType = f->getLoweredFunctionType();
  Signature signature = getSignature(fnType);
  addLLVMFunctionAttributes(f, signature);

  LinkInfo implLink = LinkInfo::get(*this, entity, ForDefinition);
  auto implFn =
      createFunction(*this, implLink, signature, nullptr /*insertBefore*/,
                     f->getOptimizationMode(), shouldEmitStackProtector(f));
  implFn->addFnAttr(llvm::Attribute::NoInline);

  IRGenFunction IGF(*this, implFn);
  if (DebugInfo)
    DebugInfo->emitArtificialFunction(IGF, implFn);

  LinkEntity varEntity =
      LinkEntity::forDynamicallyReplaceableFunctionVariable(f);
  auto linkEntry = getChainEntryForDynamicReplacement(*this, varEntity, nullptr,
                                                      NotForDefinition);

  // Load the function and dispatch to it forwarding our arguments.
  llvm::Constant *indices[] = {llvm::ConstantInt::get(Int32Ty, 0),
                               llvm::ConstantInt::get(Int32Ty, 0)};

  auto *fnPtrAddr = llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
      llvm::ConstantExpr::getInBoundsGetElementPtr(linkEntry->getValueType(),
                                                   linkEntry, indices),
      FunctionPtrTy->getPointerTo());

  auto *OrigFn = IGF.Builder.CreateCall(
      getGetOrigOfReplaceableFunctionPointer(), {fnPtrAddr});

  OrigFn->setDoesNotThrow();

  auto *typeFnPtr =
      IGF.Builder.CreateBitOrPointerCast(OrigFn, implFn->getType());

  SmallVector<llvm::Value *, 16> forwardedArgs;
  for (auto &arg : implFn->args())
    forwardedArgs.push_back(&arg);

  auto &schema = f->isAsync()
                     ? getOptions().PointerAuth.AsyncSwiftDynamicReplacements
                     : getOptions().PointerAuth.SwiftDynamicReplacements;
  auto authInfo = PointerAuthInfo::emit(
      IGF, schema, fnPtrAddr,
      PointerAuthEntity(f->getDynamicallyReplacedFunction()));
  auto *Res = IGF.Builder.CreateCall(
      FunctionPointer::createSigned(fnType, typeFnPtr, authInfo, signature)
          .getAsFunction(IGF),
      forwardedArgs);
  Res->setTailCall();
  if (f->isAsync()) {
    Res->setTailCallKind(IGF.IGM.AsyncTailCallKind);
  }

  if (implFn->getReturnType()->isVoidTy())
    IGF.Builder.CreateRetVoid();
  else
    IGF.Builder.CreateRet(Res);
}

llvm::Constant *swift::irgen::emitCXXConstructorThunkIfNeeded(
    IRGenModule &IGM, Signature signature,
    const clang::CXXConstructorDecl *ctor, StringRef name,
    llvm::Constant *ctorAddress) {
  llvm::FunctionType *assumedFnType = signature.getType();
  auto *clangFunc = cast<llvm::Function>(ctorAddress->stripPointerCasts());
  llvm::FunctionType *ctorFnType =
      cast<llvm::FunctionType>(clangFunc->getValueType());

  if (assumedFnType == ctorFnType) {
    return ctorAddress;
  }

  llvm::Function *thunk = llvm::Function::Create(
      assumedFnType, llvm::Function::PrivateLinkage, name, &IGM.Module);

  thunk->setCallingConv(IGM.getOptions().PlatformCCallingConvention);

  llvm::AttrBuilder attrBuilder(IGM.getLLVMContext());
  IGM.constructInitialFnAttributes(attrBuilder);
  attrBuilder.addAttribute(llvm::Attribute::AlwaysInline);
  llvm::AttributeList attr = signature.getAttributes().addFnAttributes(
      IGM.getLLVMContext(), attrBuilder);
  thunk->setAttributes(attr);

  IRGenFunction subIGF(IGM, thunk);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(subIGF, thunk);

  SmallVector<llvm::Value *, 8> Args;
  for (auto i = thunk->arg_begin(), e = thunk->arg_end(); i != e; ++i) {
    auto *argTy = i->getType();
    auto *paramTy = ctorFnType->getParamType(i - thunk->arg_begin());
    if (paramTy != argTy)
      Args.push_back(subIGF.coerceValue(i, paramTy, IGM.DataLayout));
    else
      Args.push_back(i);
  }

  clang::CodeGen::ImplicitCXXConstructorArgs implicitArgs =
      clang::CodeGen::getImplicitCXXConstructorArgs(IGM.ClangCodeGen->CGM(),
                                                    ctor);
  for (size_t i = 0; i < implicitArgs.Prefix.size(); ++i) {
    Args.insert(Args.begin() + 1 + i, implicitArgs.Prefix[i]);
  }
  for (const auto &arg : implicitArgs.Suffix) {
    Args.push_back(arg);
  }

  auto *call =
      emitCXXConstructorCall(subIGF, ctor, ctorFnType, ctorAddress, Args);
  if (isa<llvm::InvokeInst>(call))
    IGM.emittedForeignFunctionThunksWithExceptionTraps.insert(thunk);
  subIGF.Builder.CreateRetVoid();

  return thunk;
}

llvm::CallBase *swift::irgen::emitCXXConstructorCall(
    IRGenFunction &IGF, const clang::CXXConstructorDecl *ctor,
    llvm::FunctionType *ctorFnType, llvm::Constant *ctorAddress,
    llvm::ArrayRef<llvm::Value *> args) {
  bool canThrow =
      IGF.IGM.isForeignExceptionHandlingEnabled() &&
      !IGF.IGM.isCxxNoThrow(const_cast<clang::CXXConstructorDecl *>(ctor));
  if (!canThrow)
    return IGF.Builder.CreateCall(ctorFnType, ctorAddress, args);
  llvm::CallBase *result;
  IGF.createExceptionTrapScope([&](llvm::BasicBlock *invokeNormalDest,
                                   llvm::BasicBlock *invokeUnwindDest) {
    result = IGF.Builder.createInvoke(ctorFnType, ctorAddress, args,
                                      invokeNormalDest, invokeUnwindDest);
  });
  return result;
}

StackProtectorMode IRGenModule::shouldEmitStackProtector(SILFunction *f) {
  const SILOptions &opts = IRGen.SIL.getOptions();
  return (opts.EnableStackProtection && f->needsStackProtection()) ?
    StackProtectorMode::StackProtector : StackProtectorMode::NoStackProtector;
}

/// Find the entry point for a SIL function.
llvm::Function *IRGenModule::getAddrOfSILFunction(
    SILFunction *f, ForDefinition_t forDefinition,
    bool isDynamicallyReplaceableImplementation,
    bool shouldCallPreviousImplementation) {
  assert(forDefinition || !isDynamicallyReplaceableImplementation);
  assert(!forDefinition || !shouldCallPreviousImplementation);

  LinkEntity entity =
      LinkEntity::forSILFunction(f, shouldCallPreviousImplementation);

  // Check whether we've created the function already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  llvm::Function *fn = Module.getFunction(entity.mangleAsString());
  if (fn) {
    if (forDefinition) {
      updateLinkageForDefinition(*this, fn, entity);
    }
    return fn;
  }

  // If it's a Clang declaration, ask Clang to generate the IR declaration.
  // This might generate new functions, so we should do it before computing
  // the insert-before point.
  llvm::Constant *clangAddr = nullptr;
  bool isObjCDirect = false;
  if (auto clangDecl = f->getClangDecl()) {
    // If we have an Objective-C Clang declaration, it must be a direct
    // method and we want to generate the IR declaration ourselves.
    if (auto objcDecl = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
      isObjCDirect = true; 
      assert(objcDecl->isDirectMethod());
    } else {
      auto globalDecl = getClangGlobalDeclForFunction(clangDecl);
      clangAddr = getAddrOfClangGlobalDecl(globalDecl, forDefinition);
    }

    if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(clangDecl)) {
      Signature signature = getSignature(f->getLoweredFunctionType());

      // The thunk has private linkage, so it doesn't need to have a predictable
      // mangled name -- we just need to make sure the name is unique.
      llvm::SmallString<32> name;
      llvm::raw_svector_ostream stream(name);
      stream << "__swift_cxx_ctor";
      entity.mangle(stream);

      clangAddr = emitCXXConstructorThunkIfNeeded(*this, signature, ctor, name,
                                                  clangAddr);
    }
  }

  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  bool isDefinition = f->isDefinition();
  bool hasOrderNumber =
      isDefinition && !shouldCallPreviousImplementation;
  unsigned orderNumber = ~0U;
  llvm::Function *insertBefore = nullptr;

  // If the SIL function has a definition, we should have an order
  // number for it; make sure to insert it in that position relative
  // to other ordered functions.
  if (hasOrderNumber) {
    orderNumber = IRGen.getFunctionOrder(f);
    if (auto emittedFunctionIterator
          = EmittedFunctionsByOrder.findLeastUpperBound(orderNumber))
      insertBefore = *emittedFunctionIterator;
  }

  // If it's a Clang declaration, check whether Clang gave us a declaration.
  if (clangAddr) {
    fn = dyn_cast<llvm::Function>(clangAddr->stripPointerCasts());

    if (fn) {
      if (!forDefinition) {
        // Override the linkage computed by Clang if the decl is from another
        // module that imported @_weakLinked.
        //
        // FIXME: We should be able to set the linkage unconditionally here but
        //        some fixes are needed for Cxx interop.
        if (auto *parentModule = f->getParentModule())
          if (getSwiftModule()->isImportedAsWeakLinked(parentModule))
            fn->setLinkage(link.getLinkage());
      }

      // If we have a function, move it to the appropriate position.
      if (hasOrderNumber) {
        auto &fnList = Module.getFunctionList();
        fnList.remove(fn);
        fnList.insert(llvm::Module::iterator(insertBefore), fn);

        EmittedFunctionsByOrder.insert(orderNumber, fn);
      }
      return fn;
    }

  // Otherwise, if we have a lazy definition for it, be sure to queue that up.
  } else if (isDefinition && !forDefinition &&
             isLazilyEmittedFunction(*f, getSILModule())) {
    IRGen.addLazyFunction(f);
  }
  auto fpKind = irgen::classifyFunctionPointerKind(f);
  Signature signature =
      getSignature(f->getLoweredFunctionType(), fpKind, isObjCDirect);
  addLLVMFunctionAttributes(f, signature);

  fn = createFunction(*this, link, signature, insertBefore,
                      f->getOptimizationMode(), shouldEmitStackProtector(f));

  // Mark as llvm.used if @_used, set section if @_section
  if (f->markedAsUsed())
    addUsedGlobal(fn);
  if (!f->section().empty())
    fn->setSection(f->section());

  // If `hasCReferences` is true, then the function is either marked with
  // @_silgen_name OR @_cdecl.  If it is the latter, it must have a definition
  // associated with it.  The combination of the two allows us to identify the
  // @_silgen_name functions.  These are locally defined function thunks used in
  // the standard library.  Do not give them DLLImport DLL Storage.
  if (!forDefinition) {
    fn->setComdat(nullptr);
    if (f->hasCReferences())
      fn->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  }

  // If we have an order number for this function, set it up as appropriate.
  if (hasOrderNumber) {
    EmittedFunctionsByOrder.insert(orderNumber, fn);
  }
  return fn;
}

static llvm::GlobalVariable *createGOTEquivalent(IRGenModule &IGM,
                                                 llvm::Constant *global,
                                                 LinkEntity entity)
{
  // Determine the name of this entity.
  llvm::SmallString<64> globalName;
  entity.mangle(globalName);

  if (IGM.Triple.getObjectFormat() == llvm::Triple::COFF) {
    if (cast<llvm::GlobalValue>(global)->hasDLLImportStorageClass()) {
      // Add the user label prefix *prior* to the introduction of the linker
      // synthetic marker `__imp_`.
      // Failure to do so will re-decorate the generated symbol and miss the
      // user label prefix, generating e.g. `___imp_$sBoW` instead of
      // `__imp__$sBoW`.
      if (auto prefix = IGM.DataLayout.getGlobalPrefix())
        globalName = (llvm::Twine(prefix) + globalName).str();
      // Indicate to LLVM that the symbol should not be re-decorated.
      llvm::GlobalVariable *GV =
          new llvm::GlobalVariable(IGM.Module, global->getType(),
                                   /*Constant=*/true,
                                   llvm::GlobalValue::ExternalLinkage, nullptr,
                                   "\01__imp_" + globalName);
      GV->setExternallyInitialized(true);
      return GV;
    }
  }

  auto gotEquivalent = new llvm::GlobalVariable(IGM.Module,
                                      global->getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      global,
                                      llvm::Twine("got.") + globalName);
  
  // rdar://problem/53836960: i386 ld64 also mis-links relative references
  // to GOT entries.
  // rdar://problem/59782487: issue with on-device JITd expressions.
  // The JIT gets confused by private vars accessed across object files.
  if (!IGM.getOptions().UseJIT &&
      (!IGM.Triple.isOSDarwin() || IGM.Triple.getArch() != llvm::Triple::x86)) {
    gotEquivalent->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  } else {
    ApplyIRLinkage(IRLinkage::InternalLinkOnceODR)
      .to(gotEquivalent);
  }

  // Context descriptor pointers need to be signed.
  // TODO: We should really sign a pointer to *any* code entity or true-const
  // metadata structure that may reference data structures with function
  // pointers inside them.
  if (entity.isContextDescriptor()) {
    auto schema = IGM.getOptions().PointerAuth.TypeDescriptors;
    if (schema) {
      auto signedValue = IGM.getConstantSignedPointer(
          global, schema, PointerAuthEntity::Special::TypeDescriptor,
          /*storageAddress*/ gotEquivalent);
      gotEquivalent->setInitializer(signedValue);
    }
  } else if (entity.isDynamicallyReplaceableKey()) {
    auto schema = IGM.getOptions().PointerAuth.SwiftDynamicReplacementKeys;
    if (schema) {
      auto signedValue = IGM.getConstantSignedPointer(
          global, schema, PointerAuthEntity::Special::DynamicReplacementKey,
          /*storageAddress*/ gotEquivalent);
      gotEquivalent->setInitializer(signedValue);
    }
  }

  return gotEquivalent;
}

llvm::Constant *IRGenModule::getOrCreateGOTEquivalent(llvm::Constant *global,
                                                      LinkEntity entity) {
  auto &gotEntry = GlobalGOTEquivalents[entity];
  if (gotEntry) {
    return gotEntry;
  }

  if (auto *Stats = Context.Stats)
    ++Stats->getFrontendCounters().NumGOTEntries;

  // Use the global as the initializer for an anonymous constant. LLVM can treat
  // this as equivalent to the global's GOT entry.
  auto gotEquivalent = createGOTEquivalent(*this, global, entity);
  gotEntry = gotEquivalent;
  return gotEquivalent;
}

static llvm::Constant *getElementBitCast(llvm::GlobalValue *ptr,
                                         llvm::Type *newEltType) {
  if (ptr->getValueType() == newEltType) {
    return ptr;
  } else {
    auto newPtrType = newEltType->getPointerTo(
        cast<llvm::PointerType>(ptr->getType())->getAddressSpace());
    return llvm::ConstantExpr::getBitCast(ptr, newPtrType);
  }
}

llvm::Constant *
IRGenModule::getOrCreateLazyGlobalVariable(LinkEntity entity,
    llvm::function_ref<ConstantInitFuture(ConstantInitBuilder &)> build,
    llvm::function_ref<void(llvm::GlobalVariable *)> finish) {
  auto defaultType = entity.getDefaultDeclarationType(*this);

  LazyConstantInitializer lazyInitializer = {
    defaultType, build, finish
  };
  return getAddrOfLLVMVariable(entity,
                               ConstantInit::getLazy(&lazyInitializer),
                               DebugTypeInfo(), defaultType);
}

/// Return a reference to an object that's suitable for being used for
/// the given kind of reference.
///
/// Note that, if the requested reference kind is a relative reference.
/// the returned constant will not actually be a relative reference.
/// To form the actual relative reference, you must pass the returned
/// result to emitRelativeReference, passing the correct base-address
/// information.
ConstantReference
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ConstantInit definition,
                                   DebugTypeInfo debugType,
                                   SymbolReferenceKind refKind,
                                   llvm::Type *overrideDeclType) {
  switch (refKind) {
  case SymbolReferenceKind::Relative_Direct:
  case SymbolReferenceKind::Far_Relative_Direct:
    assert(!definition);
    // FIXME: don't just fall through; force the creation of a weak
    // definition so that we can emit a relative reference.
    LLVM_FALLTHROUGH;

  case SymbolReferenceKind::Absolute:
    return { getAddrOfLLVMVariable(entity, definition, debugType,
                                   overrideDeclType),
             ConstantReference::Direct };


  case SymbolReferenceKind::Relative_Indirectable:
  case SymbolReferenceKind::Far_Relative_Indirectable:
    assert(!definition);
    return getAddrOfLLVMVariableOrGOTEquivalent(entity);
  }
  llvm_unreachable("bad reference kind");
}

/// A convenient wrapper around getAddrOfLLVMVariable which uses the
/// default type as the definition type.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ForDefinition_t forDefinition,
                                   DebugTypeInfo debugType) {
  auto definition = forDefinition
      ? ConstantInit::getDelayed(entity.getDefaultDeclarationType(*this))
      : ConstantInit();
  return getAddrOfLLVMVariable(entity, definition, debugType);
}

/// Get or create an llvm::GlobalVariable.
///
/// If a definition type is given, the result will always be an
/// llvm::GlobalVariable of that type.  Otherwise, the result will
/// have type pointerToDefaultType and may involve bitcasts.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity,
                                   ConstantInit definition,
                                   DebugTypeInfo DbgTy,
                                   llvm::Type *overrideDeclType) {
  // This function assumes that 'globals' only contains GlobalValue
  // values for the entities that it will look up.

  llvm::Type *definitionType = (definition ? definition.getType() : nullptr);
  auto defaultType = overrideDeclType
    ? overrideDeclType
    : entity.getDefaultDeclarationType(*this);
  
  auto existingGlobal = GlobalVars[entity];
  if (existingGlobal) {
    auto existing = cast<llvm::GlobalValue>(existingGlobal);

    // If we're looking to define something, we may need to replace a
    // forward declaration.
    if (!definition.isLazy() && definitionType) {
      assert(existing->isDeclaration() && "already defined");
      updateLinkageForDefinition(*this, existing, entity);

      // If the existing entry is a variable of the right type,
      // set the initializer on it and return.
      if (auto var = dyn_cast<llvm::GlobalVariable>(existing)) {
        if (definitionType == var->getValueType()) {
          if (definition.hasInit())
            definition.getInit().installInGlobal(var);
          return var;
        }
      }

      // Fall out to the case below, clearing the name so that
      // createVariable doesn't detect a collision.
      existingGlobal->setName("");

    // Otherwise, we have a previous declaration or definition which
    // we need to ensure has the right type.
    } else {
      return getElementBitCast(existing, defaultType);
    }
  }

  ForDefinition_t forDefinition = (ForDefinition_t) (definitionType != nullptr);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  // Clang may have defined the variable already.
  if (auto existing = Module.getNamedGlobal(link.getName()))
    return getElementBitCast(existing, defaultType);

  const LazyConstantInitializer *lazyInitializer = nullptr;
  llvm::Optional<ConstantInitBuilder> lazyBuilder;
  if (definition.isLazy()) {
    lazyInitializer = definition.getLazy();
    lazyBuilder.emplace(*this);

    // Build the lazy initializer as a future.
    auto future = lazyInitializer->Build(*lazyBuilder);

    // Set the future as our definition and set its type as the
    // definition type.
    definitionType = future.getType();
    definition = future;
  }

  // If we're not defining the object now, forward declare it with the default
  // type.
  if (!definitionType) definitionType = defaultType;

  // Create the variable.
  auto var = createVariable(*this, link, definitionType,
                            entity.getAlignment(*this), DbgTy);

  // Install the concrete definition if we have one.
  if (definition && definition.hasInit()) {
    definition.getInit().installInGlobal(var);
  }

  // Call the creation callback.
  if (lazyInitializer) {
    lazyInitializer->Create(var);
  }

  if (lazyInitializer) {
    // Protect against self-references that might've been created during
    // the lazy emission.
    existingGlobal = GlobalVars[entity];
  }

  // If we have an existing entry, destroy it, replacing it with the
  // new variable.  We only really have to do 
  if (existingGlobal) {
    auto existing = cast<llvm::GlobalValue>(existingGlobal);
    auto castVar = llvm::ConstantExpr::getBitCast(var, existing->getType());
    existing->replaceAllUsesWith(castVar);
    existing->eraseFromParent();
  }

  // If there's also an existing GOT-equivalent entry, rewrite it too, since
  // LLVM won't recognize a global with bitcasts in its initializers as GOT-
  // equivalent. rdar://problem/22388190
  auto foundGOTEntry = GlobalGOTEquivalents.find(entity);
  if (foundGOTEntry != GlobalGOTEquivalents.end() && foundGOTEntry->second) {
    auto existingGOTEquiv = cast<llvm::GlobalVariable>(foundGOTEntry->second);

    // Make a new GOT equivalent referring to the new variable with its
    // definition type.
    auto newGOTEquiv = createGOTEquivalent(*this, var, entity);
    auto castGOTEquiv = llvm::ConstantExpr::getBitCast(newGOTEquiv,
                                                   existingGOTEquiv->getType());
    existingGOTEquiv->replaceAllUsesWith(castGOTEquiv);
    existingGOTEquiv->eraseFromParent();
    GlobalGOTEquivalents[entity] = newGOTEquiv;
  }

  // Cache and return.
  GlobalVars[entity] = var;
  return var;
}

/// Get or create a "GOT equivalent" llvm::GlobalVariable, if applicable.
///
/// Creates a private, unnamed constant containing the address of another
/// global variable. LLVM can replace relative references to this variable with
/// relative references to the GOT entry for the variable in the object file.
ConstantReference
IRGenModule::getAddrOfLLVMVariableOrGOTEquivalent(LinkEntity entity) {
  auto canDirectlyReferenceSILFunction = [&](SILFunction *silFn) {
    return (silFn->isDefinition() &&
           !isAvailableExternally(silFn->getLinkage()) &&
           this == IRGen.getGenModule(silFn));
  };

  // Handle SILFunctions specially, because unlike other entities they aren't
  // variables and aren't kept in the GlobalVars table.
  if (entity.isSILFunction()) {
    auto *silFn = entity.getSILFunction();
    auto fn = getAddrOfSILFunction(silFn, NotForDefinition);
    if (canDirectlyReferenceSILFunction(silFn)) {
      return {fn, ConstantReference::Direct};
    }
    
    auto gotEquivalent = getOrCreateGOTEquivalent(fn, entity);
    return {gotEquivalent, ConstantReference::Indirect};
  }
  
  // ObjC class references can always be directly referenced, even in
  // the weird cases where we don't see a definition.
  if (entity.isObjCClassRef()) {
    auto value = getAddrOfObjCClassRef(
      const_cast<ClassDecl *>(cast<ClassDecl>(entity.getDecl())));
    return { cast<llvm::Constant>(value.getAddress()),
             ConstantReference::Direct };
  }
  
  // Ensure the variable is at least forward-declared.
  getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());

  auto entry = GlobalVars[entity];
  
  /// Returns a direct reference.
  auto direct = [&]() -> ConstantReference {
    // FIXME: Relative references to aliases break MC on 32-bit Mach-O
    // platforms (rdar://problem/22450593 ), so substitute an alias with its
    // aliasee to work around that.
    if (auto alias = dyn_cast<llvm::GlobalAlias>(entry))
      return {alias->getAliasee(), ConstantReference::Direct};
    return {entry, ConstantReference::Direct};
  };
  
  /// Returns an indirect reference.
  auto indirect = [&]() -> ConstantReference {
    auto gotEquivalent = getOrCreateGOTEquivalent(
      cast<llvm::GlobalValue>(entry), entity);
    return {gotEquivalent, ConstantReference::Indirect};
  };

  // Dynamically replaceable function keys are stored in the GlobalVars
  // table, but they don't have an associated Decl, so they require
  // special treatment here.
  if (entity.isDynamicallyReplaceableFunctionKey()) {
    auto *silFn = entity.getSILFunction();
    if (canDirectlyReferenceSILFunction(silFn))
      return direct();

    return indirect();
  }

  if (auto *entityDC = entity.getDeclContextForEmission()) {
    auto *entitySF = entityDC->getModuleScopeContext();
    bool clangImportedEntity = isa<ClangModuleUnit>(entitySF);

    auto &mod = getSILModule();

    if (!mod.isWholeModule()) {
      // In non-WMO builds, the associated context of the SILModule must
      // be a source file. Every source file is its own translation unit.
      auto *modDC = mod.getAssociatedContext();
      auto *modSF = modDC->getModuleScopeContext();
      assert(modSF != nullptr);

      // Imported entities are in a different Swift module, but are emitted
      // on demand and can be referenced directly. Entities in the same
      // source file can also be referenced directly.
      if (clangImportedEntity ||
          modSF == entitySF)
        return direct();

      // Everything else must be referenced indirectly.
      return indirect();
    }

    // We're performing a WMO build.
    //
    // The associated context of the SILModule is the entire AST ModuleDecl,
    // but we might be doing a multi-threaded IRGen build, in which case
    // there is one translation unit per source file.

    // Imported entities are in a different Swift module and are emitted
    // on demand. In multi-threaded builds, they will be emitted into one
    // translation unit only.
    if (clangImportedEntity ||
        entitySF->getParentModule() == mod.getSwiftModule()) {
      // If we're doing a single-threaded WMO build, or if the entity is
      // scheduled to be emitted in the same translation unit, reference
      // it directly.
      if (this == IRGen.getGenModule(entitySF))
        return direct();
    }
  }

  // Fall back to an indirect reference if we can't establish that a direct
  // reference is OK.
  return indirect();
}

static TypeEntityReference
getContextDescriptorEntityReference(IRGenModule &IGM, const LinkEntity &entity){
  // TODO: consider using a symbolic reference (i.e. a symbol string
  // to be looked up dynamically) for types defined outside the module.
  auto ref = IGM.getAddrOfLLVMVariableOrGOTEquivalent(entity);
  auto kind = ref.isIndirect()
                ? TypeReferenceKind::IndirectTypeDescriptor
                : TypeReferenceKind::DirectTypeDescriptor;
  return TypeEntityReference(kind, ref.getValue());
}

static TypeEntityReference
getTypeContextDescriptorEntityReference(IRGenModule &IGM,
                                        NominalTypeDecl *decl) {
  auto entity = LinkEntity::forNominalTypeDescriptor(decl);
  IGM.IRGen.noteUseOfTypeContextDescriptor(decl, DontRequireMetadata);
  return getContextDescriptorEntityReference(IGM, entity);
}

static TypeEntityReference
getProtocolDescriptorEntityReference(IRGenModule &IGM, ProtocolDecl *protocol) {
  assert(!protocol->hasClangNode() &&
         "objc protocols don't have swift protocol descriptors");
  auto entity = LinkEntity::forProtocolDescriptor(protocol);
  return getContextDescriptorEntityReference(IGM, entity);
}

static TypeEntityReference
getObjCClassByNameReference(IRGenModule &IGM, ClassDecl *cls) {
  auto kind = TypeReferenceKind::DirectObjCClassName;
  SmallString<64> objcRuntimeNameBuffer;
  auto ref = IGM.getAddrOfGlobalString(
                                 cls->getObjCRuntimeName(objcRuntimeNameBuffer),
                                 /*willBeRelativelyAddressed=*/true);

  return TypeEntityReference(kind, ref);
}

TypeEntityReference
IRGenModule::getTypeEntityReference(GenericTypeDecl *decl) {
  if (auto protocol = dyn_cast<ProtocolDecl>(decl)) {
    assert(!protocol->hasClangNode() && "imported protocols not handled here");
    return getProtocolDescriptorEntityReference(*this, protocol);
  }
  
  if (auto opaque = dyn_cast<OpaqueTypeDecl>(decl)) {
    auto entity = LinkEntity::forOpaqueTypeDescriptor(opaque);
    IRGen.noteUseOfOpaqueTypeDescriptor(opaque);
    return getContextDescriptorEntityReference(*this, entity);
  }

  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    auto clazz = dyn_cast<ClassDecl>(decl);
    if (!clazz || clazz->isForeignReferenceType()) {
      return getTypeContextDescriptorEntityReference(*this, nominal);
    }

    switch (clazz->getForeignClassKind()) {
    case ClassDecl::ForeignKind::RuntimeOnly:
      return getObjCClassByNameReference(*this, clazz);

    case ClassDecl::ForeignKind::CFType:
      return getTypeContextDescriptorEntityReference(*this, clazz);

    case ClassDecl::ForeignKind::Normal:
      if (hasKnownSwiftMetadata(*this, clazz)) {
        return getTypeContextDescriptorEntityReference(*this, clazz);
      }

      // Note: we would like to use an Objective-C class reference, but the
      // Darwin linker currently has a bug where it will coalesce these symbols
      // *after* computing a relative offset, causing incorrect relative
      // offsets in the metadata. Therefore, reference Objective-C classes by
      // their runtime names.
      return getObjCClassByNameReference(*this, clazz);
    }
  }
  llvm_unreachable("bad foreign type kind");
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices) of `base`) and `target`.
llvm::Constant *
IRGenModule::emitRelativeReference(ConstantReference target,
                                   llvm::GlobalValue *base,
                                   ArrayRef<unsigned> baseIndices) {
  llvm::Constant *relativeAddr =
    emitDirectRelativeReference(target.getValue(), base, baseIndices);

  // If the reference is indirect, flag it by setting the low bit.
  // (All of the base, direct target, and GOT entry need to be pointer-aligned
  // for this to be OK.)
  if (target.isIndirect()) {
    relativeAddr = llvm::ConstantExpr::getAdd(relativeAddr,
                             llvm::ConstantInt::get(RelativeAddressTy, 1));
  }

  return relativeAddr;
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices...) of `base`) and `target`.  For now,
/// for this to succeed portably, both need to be globals defined in the
/// current translation unit.
llvm::Constant *
IRGenModule::emitDirectRelativeReference(llvm::Constant *target,
                                         llvm::GlobalValue *base,
                                         // llvm::Constant *base,
                                         ArrayRef<unsigned> baseIndices) {
  // Convert the target to an integer.
  auto targetAddr = llvm::ConstantExpr::getPtrToInt(target, SizeTy);

  SmallVector<llvm::Constant*, 4> indices;
  indices.push_back(llvm::ConstantInt::get(Int32Ty, 0));
  for (unsigned baseIndex : baseIndices) {
    indices.push_back(llvm::ConstantInt::get(Int32Ty, baseIndex));
  };

  // Drill down to the appropriate address in the base, then convert
  // that to an integer.
  auto baseElt = llvm::ConstantExpr::getInBoundsGetElementPtr(
      base->getValueType(), base, indices);
  auto baseAddr = llvm::ConstantExpr::getPtrToInt(baseElt, SizeTy);

  // The relative address is the difference between those.
  auto relativeAddr = llvm::ConstantExpr::getSub(targetAddr, baseAddr);

  // Relative addresses can be 32-bit even on 64-bit platforms.
  if (SizeTy != RelativeAddressTy)
    relativeAddr = llvm::ConstantExpr::getTrunc(relativeAddr,
                                                RelativeAddressTy);

  return relativeAddr;
}

/// Expresses that `var` is removable (dead-strippable) when `dependsOn` is not
/// referenced.
void IRGenModule::appendLLVMUsedConditionalEntry(llvm::GlobalVariable *var,
                                                 llvm::Constant *dependsOn) {
  llvm::Metadata *metadata[] = {
      // (1) which variable is being conditionalized, "target"
      llvm::ConstantAsMetadata::get(var),
      // (2) type, not relevant for a single-edge condition
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt32Ty(Module.getContext()), 0)),
      // (3) the "edge" that holds the target alive, if it's missing the target
      // is allowed to be removed
      llvm::MDNode::get(Module.getContext(),
                        {
                            llvm::ConstantAsMetadata::get(dependsOn),
                        }),
  };
  UsedConditionals.push_back(llvm::MDNode::get(Module.getContext(), metadata));
}

/// Expresses that `var` is removable (dead-strippable) when either the protocol
/// from `record` is not referenced or the type from `record` is not referenced.
void IRGenModule::appendLLVMUsedConditionalEntry(
    llvm::GlobalVariable *var, const ProtocolConformance *conformance) {
  auto *protocol = getAddrOfProtocolDescriptor(conformance->getProtocol())
                       ->stripPointerCasts();
  auto *type = getAddrOfTypeContextDescriptor(
                   conformance->getType()->getAnyNominal(), DontRequireMetadata)
                   ->stripPointerCasts();

  llvm::Metadata *metadata[] = {
      // (1) which variable is being conditionalized, "target"
      llvm::ConstantAsMetadata::get(var),
      // (2) type, "1" = if either edge is missing, the target is allowed to be
      // removed.
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt32Ty(Module.getContext()), 1)),
      // (3) list of edges
      llvm::MDNode::get(Module.getContext(),
                        {
                            llvm::ConstantAsMetadata::get(protocol),
                            llvm::ConstantAsMetadata::get(type),
                        }),
  };
  UsedConditionals.push_back(llvm::MDNode::get(Module.getContext(), metadata));
}

void IRGenModule::emitUsedConditionals() {
  if (UsedConditionals.empty())
    return;

  auto *usedConditional =
      Module.getOrInsertNamedMetadata("llvm.used.conditional");

  for (auto *M : UsedConditionals) {
    // Process the dependencies ("edges") and strip any pointer casts on them.
    // Those might appear when a dependency is originally added against a
    // declaration only, and later the declaration is RAUW'd with a definition
    // causing a bitcast to get added to the metadata entry in the dependency.
    auto *DependenciesMD =
        dyn_cast_or_null<llvm::MDNode>(M->getOperand(2).get());
    for (unsigned int I = 0; I < DependenciesMD->getNumOperands(); I++) {
      auto *Dependency = DependenciesMD->getOperand(I).get();
      auto *C = llvm::mdconst::extract_or_null<llvm::Constant>(Dependency)
                    ->stripPointerCasts();
      DependenciesMD->replaceOperandWith(I, llvm::ConstantAsMetadata::get(C));
    }

    usedConditional->addOperand(M);
  }
}

/// Emit the protocol descriptors list and return it (if asContiguousArray is
/// true, otherwise the descriptors are emitted as individual globals and
/// nullptr is returned).
llvm::Constant *IRGenModule::emitSwiftProtocols(bool asContiguousArray) {
  if (SwiftProtocols.empty())
    return nullptr;

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit protocols for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_protos, regular";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_protocols";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5prt$B";
    break;
  }

  // For JIT, emit the protocol list as a single global array, and return it.
  if (asContiguousArray) {
    ConstantInitBuilder builder(*this);
    auto recordsArray = builder.beginArray(ProtocolRecordTy);
    for (auto *protocol : SwiftProtocols) {
      auto record = recordsArray.beginStruct(ProtocolRecordTy);
      // Relative reference to the protocol descriptor.
      auto descriptorRef = getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forProtocolDescriptor(protocol));
      record.addRelativeAddress(descriptorRef);
      record.finishAndAddTo(recordsArray);
    }

    // Define the global variable for the protocol list.
    // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
    // resolve relocations relative to it.
    auto var = recordsArray.finishAndCreateGlobal(
        "\x01l_protocols", Alignment(4),
        /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
    var->setSection(sectionName);
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);

    return var;
  }

  // In non-JIT mode, emit the protocol records as individual globals.
  for (auto *protocol : SwiftProtocols) {
    auto entity = LinkEntity::forProtocolDescriptor(protocol);
    auto link = LinkInfo::get(*this, entity, NotForDefinition);
    auto recordMangledName =
        LinkEntity::forProtocolDescriptorRecord(protocol).mangleAsString();
    auto var =
        new llvm::GlobalVariable(Module, ProtocolRecordTy, /*isConstant*/ true,
                                 llvm::GlobalValue::PrivateLinkage,
                                 /*initializer*/ nullptr, recordMangledName);

    auto descriptorRef = getAddrOfLLVMVariableOrGOTEquivalent(entity);
    llvm::Constant *relativeAddr =
        emitDirectRelativeReference(descriptorRef.getValue(), var, {0});

    llvm::Constant *recordFields[] = {relativeAddr};
    auto record = llvm::ConstantStruct::get(ProtocolRecordTy, recordFields);
    var->setInitializer(record);

    var->setSection(sectionName);
    var->setAlignment(llvm::MaybeAlign(4));
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);

    if (IRGen.Opts.ConditionalRuntimeRecords) {
      // Allow dead-stripping `var` (the protocol record) when the protocol
      // (descriptorRef) is not referenced.
      appendLLVMUsedConditionalEntry(var, descriptorRef.getValue());
    }
  }

  return nullptr;
}

void IRGenModule::addProtocolConformance(ConformanceDescription &&record) {

  emitProtocolConformance(record);

  // Add this conformance to the conformance list.
  ProtocolConformances.push_back(std::move(record));
}

void IRGenModule::addAccessibleFunction(SILFunction *func) {
  AccessibleFunctions.push_back(func);
}

/// Emit the protocol conformance list and return it (if asContiguousArray is
/// true, otherwise the records are emitted as individual globals and
/// nullptr is returned).
llvm::Constant *IRGenModule::emitProtocolConformances(bool asContiguousArray) {
  if (ProtocolConformances.empty())
    return nullptr;

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit protocol conformances for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_proto, regular";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_protocol_conformances";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5prtc$B";
    break;
  }

  // For JIT, emit the protocol conformance list as a single global array, and
  // return it.
  if (asContiguousArray) {
    ConstantInitBuilder builder(*this);
    auto descriptorArray = builder.beginArray(RelativeAddressTy);
    for (const auto &record : ProtocolConformances) {
      auto conformance = record.conformance;
      auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
      auto descriptor =
        getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
      descriptorArray.addRelativeAddress(descriptor);
    }

    // Define the global variable for the conformance list.
    // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
    // resolve relocations relative to it.
    auto var = descriptorArray.finishAndCreateGlobal(
        "\x01l_protocol_conformances", Alignment(4),
        /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage);
    var->setSection(sectionName);
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);

    return var;
  }

  // In non-JIT mode, emit the protocol conformance records as individual
  // globals.
  for (const auto &record : ProtocolConformances) {
    auto entity =
        LinkEntity::forProtocolConformanceDescriptor(record.conformance);
    auto link = LinkInfo::get(*this, entity, NotForDefinition);
    auto recordMangledName =
        LinkEntity::forProtocolConformanceDescriptorRecord(record.conformance)
            .mangleAsString();
    auto var = new llvm::GlobalVariable(
        Module, RelativeAddressTy, /*isConstant*/ true,
        llvm::GlobalValue::PrivateLinkage, /*initializer*/ nullptr,
        recordMangledName);

    auto descriptorRef = getAddrOfLLVMVariableOrGOTEquivalent(entity);
    llvm::Constant *relativeAddr =
        emitDirectRelativeReference(descriptorRef.getValue(), var, {});
    var->setInitializer(relativeAddr);

    var->setSection(sectionName);
    var->setAlignment(llvm::MaybeAlign(4));
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);

    if (IRGen.Opts.ConditionalRuntimeRecords) {
      // Allow dead-stripping `var` (the conformance record) when the protocol
      // or type (from the conformance) is not referenced.
      appendLLVMUsedConditionalEntry(var, record.conformance);
    }
  }

  return nullptr;
}


/// Emit list of type metadata records for types that might not have explicit
/// protocol conformances, and return it (if asContiguousArray is true,
/// otherwise the descriptors are emitted as individual globals and nullptr is
/// returned).
llvm::Constant *IRGenModule::emitTypeMetadataRecords(bool asContiguousArray) {
  if (RuntimeResolvableTypes.empty()
      && RuntimeResolvableTypes2.empty())
    return nullptr;

  std::string sectionName;
  std::string section2Name;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_types, regular";
    section2Name = "__TEXT, __swift5_types2, regular";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_type_metadata";
    section2Name = "swift5_type_metadata_2";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5tymd$B";
    section2Name = ".sw5tym2$B";
    break;
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit type metadata table for "
                     "the selected object format.");
  }

  auto generateRecord = [this](TypeEntityReference ref,
                               llvm::GlobalVariable *var,
                               ArrayRef<unsigned> baseIndices) {
    // Form the relative address, with the type reference kind in the low bits.
    llvm::Constant *relativeAddr =
        emitDirectRelativeReference(ref.getValue(), var, baseIndices);
    unsigned lowBits = static_cast<unsigned>(ref.getKind());
    if (lowBits != 0) {
      relativeAddr = llvm::ConstantExpr::getAdd(
          relativeAddr, llvm::ConstantInt::get(RelativeAddressTy, lowBits));
    }

    llvm::Constant *recordFields[] = {relativeAddr};
    auto record = llvm::ConstantStruct::get(TypeMetadataRecordTy, recordFields);
    return record;
  };

  // For JIT, emit the type list as a single global array, and return it.
  if (asContiguousArray) {
    // Define the global variable for the list of types.
    // We have to do this before defining the initializer since the entries will
    // contain offsets relative to themselves.
    // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
    // resolve relocations relative to it.
    auto arrayTy = llvm::ArrayType::get(TypeMetadataRecordTy,
                                        RuntimeResolvableTypes.size());
    auto var = new llvm::GlobalVariable(
        Module, arrayTy,
        /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage,
        /*initializer*/ nullptr, "\x01l_type_metadata_table");

    SmallVector<llvm::Constant *, 8> elts;
    for (auto type : RuntimeResolvableTypes) {
      auto ref = getTypeEntityReference(type);
      unsigned arrayIdx = elts.size();
      auto record = generateRecord(ref, var, { arrayIdx, 0 });
      elts.push_back(record);
    }

    auto initializer = llvm::ConstantArray::get(arrayTy, elts);
    var->setInitializer(initializer);
    var->setSection(sectionName);
    var->setAlignment(llvm::MaybeAlign(4));
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);

    return var;
  }

  // In non-JIT mode, emit the type records as individual globals.

  auto generateGlobalTypeList = [&](ArrayRef<GenericTypeDecl *> typesList,
                                    StringRef section) {
    if (typesList.empty()) {
      return;
    }
                                    
    for (auto type : typesList) {
      auto ref = getTypeEntityReference(type);

      std::string recordMangledName;
      if (auto opaque = dyn_cast<OpaqueTypeDecl>(type)) {
        recordMangledName =
            LinkEntity::forOpaqueTypeDescriptorRecord(opaque).mangleAsString();
      } else if (auto nominal = dyn_cast<NominalTypeDecl>(type)) {
        recordMangledName =
            LinkEntity::forNominalTypeDescriptorRecord(nominal).mangleAsString();
      } else {
        llvm_unreachable("bad type in RuntimeResolvableTypes");
      }

      auto var = new llvm::GlobalVariable(
          Module, TypeMetadataRecordTy, /*isConstant*/ true,
          llvm::GlobalValue::PrivateLinkage, /*initializer*/ nullptr,
          recordMangledName);

      auto record = generateRecord(ref, var, {0});
      var->setInitializer(record);

      var->setSection(section);
      var->setAlignment(llvm::MaybeAlign(4));
      disableAddressSanitizer(*this, var);
      addUsedGlobal(var);

      if (IRGen.Opts.ConditionalRuntimeRecords) {
        // Allow dead-stripping `var` (the type record) when the type (`ref`) is
        // not referenced.
        appendLLVMUsedConditionalEntry(var, ref.getValue());
      }
    }
  };

  generateGlobalTypeList(RuntimeResolvableTypes, sectionName);
  generateGlobalTypeList(RuntimeResolvableTypes2, section2Name);

  return nullptr;
}

void IRGenModule::emitAccessibleFunctions() {
  if (AccessibleFunctions.empty())
    return;

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit accessible functions for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_acfuncs, regular";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_accessible_functions";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5acfn$B";
    break;
  }

  for (auto *func : AccessibleFunctions) {
    auto mangledRecordName =
        LinkEntity::forAccessibleFunctionRecord(func).mangleAsString();

    auto var = new llvm::GlobalVariable(
        Module, AccessibleFunctionRecordTy, /*isConstant*/ true,
        llvm::GlobalValue::PrivateLinkage, /*initializer*/ nullptr,
        mangledRecordName);

    ConstantInitBuilder builder(*this);
    ConstantStructBuilder fields =
        builder.beginStruct(AccessibleFunctionRecordTy);

    std::string mangledFunctionName =
        LinkEntity::forSILFunction(func).mangleAsString();
    llvm::Constant *name = getAddrOfGlobalString(
        mangledFunctionName, /*willBeRelativelyAddressed*/ true);
    fields.addRelativeAddress(name);

    llvm::Constant *genericEnvironment = nullptr;

    GenericSignature signature;
    if (auto *env = func->getGenericEnvironment()) {
      // Drop all of the marker protocols because they are effect-less
      // at runtime.
      signature = env->getGenericSignature().withoutMarkerProtocols();

      genericEnvironment =
          getAddrOfGenericEnvironment(signature.getCanonicalSignature());
    }

    fields.addRelativeAddressOrNull(genericEnvironment);

    llvm::Constant *type = getTypeRef(func->getLoweredFunctionType(), signature,
                                      MangledTypeRefRole::Metadata)
                               .first;
    fields.addRelativeAddress(type);

    llvm::Constant *funcAddr = nullptr;
    if (func->isDistributed()) {
      funcAddr = getAddrOfAsyncFunctionPointer(
          LinkEntity::forDistributedTargetAccessor(func));
    } else if (func->isAsync()) {
      funcAddr = getAddrOfAsyncFunctionPointer(func);
    } else {
      funcAddr = getAddrOfSILFunction(func, NotForDefinition);
    }

    fields.addRelativeAddress(funcAddr);

    AccessibleFunctionFlags flags;
    flags.setDistributed(func->isDistributed());
    fields.addInt32(flags.getOpaqueValue());

    fields.finishAndSetAsInitializer(var);
    var->setSection(sectionName);
    var->setAlignment(llvm::MaybeAlign(4));
    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);
  }
}

void IRGenModule::emitRuntimeDiscoverableAttributes(
    TinyPtrVector<FileUnit *> &filesToEmit) {
  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("Don't know how to emit attribute section for "
                     "the selected object format.");
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift5_rattrs, regular";
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    sectionName = "swift5_runtime_attributes";
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    sectionName = ".sw5ratt$B";
    break;
  }

  // Map attribute type to each declaration it's attached to
  // and a corresponding generator function.
  llvm::MapVector<NominalTypeDecl *, SmallVector<SILDeclRef, 2>> attributes;
  {
    for (auto *fileUnit : filesToEmit) {
      auto *SF = dyn_cast<SourceFile>(fileUnit);
      if (!SF)
        continue;

      for (auto *decl : SF->getDeclsWithRuntimeDiscoverableAttrs()) {
        for (auto *attr : decl->getRuntimeDiscoverableAttrs()) {
          auto *attrType = decl->getRuntimeDiscoverableAttrTypeDecl(attr);
          attributes[attrType].push_back(
              SILDeclRef::getRuntimeAttributeGenerator(attr, decl)
                  .asRuntimeAccessible());
        }
      }
    }
  }

  auto &SM = getSILModule();

  for (auto &attr : attributes) {
    auto *attrType = attr.first;
    const auto &attachedTo = attr.second;

    auto mangledRecordName =
        LinkEntity::forRuntimeDiscoverableAttributeRecord(attrType)
            .mangleAsString();

    ConstantInitBuilder builder(*this);
    ConstantStructBuilder B = builder.beginStruct();

    // Flags
    B.addInt32(0);

    // Attribute metadata descriptor
    B.addRelativeAddress(getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forNominalTypeDescriptor(attrType)));

    // Number of types it's attached to.
    B.addInt32(attachedTo.size());

    // Emit all of the trailing objects
    for (auto &ref : attachedTo) {
      auto type = ref.getDecl()->getInterfaceType()->getCanonicalType();
      auto *generator = SM.lookUpFunction(ref);

      B.addRelativeAddress(
          getTypeRef(type, /*genericSig=*/nullptr, MangledTypeRefRole::Metadata)
              .first);
      B.addRelativeAddressOrNull(getAddrOfAccessibleFunctionRecord(generator));
    }

    B.suggestType(RuntimeDiscoverableAttributeTy);

    auto entity = LinkEntity::forRuntimeDiscoverableAttributeRecord(attrType);

    auto *var = cast<llvm::GlobalVariable>(getAddrOfLLVMVariable(
        entity, B.finishAndCreateFuture(), DebugTypeInfo()));

    var->setConstant(true);
    setTrueConstGlobal(var);

    var->setSection(sectionName);
    var->setAlignment(llvm::MaybeAlign(4));

    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);
  }
}

/// Fetch a global reference to a reference to the given Objective-C class.
/// The result is of type ObjCClassPtrTy->getPointerTo().
Address IRGenModule::getAddrOfObjCClassRef(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of ObjC class ref in no-interop mode");

  LinkEntity entity = LinkEntity::forObjCClassRef(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, ConstantInit(), DbgTy);

  // Define it lazily.
  if (auto global = dyn_cast<llvm::GlobalVariable>(addr)) {
    if (global->isDeclaration()) {
      global->setSection(GetObjCSectionName("__objc_classrefs",
                                            "regular,no_dead_strip"));
      global->setLinkage(llvm::GlobalVariable::PrivateLinkage);
      global->setExternallyInitialized(true);
      global->setInitializer(getAddrOfObjCClass(theClass, NotForDefinition));
      addCompilerUsedGlobal(global);
    }
  }

  return Address(addr, ObjCClassPtrTy, entity.getAlignment(*this));
}

/// Fetch a global reference to the given Objective-C class.  The
/// result is of type ObjCClassPtrTy.
llvm::Constant *IRGenModule::getAddrOfObjCClass(ClassDecl *theClass,
                                                ForDefinition_t forDefinition) {
  assert(ObjCInterop && "getting address of ObjC class in no-interop mode");
  assert(!theClass->isForeign());
  LinkEntity entity = LinkEntity::forObjCClass(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, forDefinition, DbgTy);
  return addr;
}

/// Fetch the declaration of a metaclass object. The result is always a
/// GlobalValue of ObjCClassPtrTy, and is either the Objective-C metaclass or
/// the Swift metaclass stub, depending on whether the class is published as an
/// ObjC class.
llvm::Constant *
IRGenModule::getAddrOfMetaclassObject(ClassDecl *decl,
                                      ForDefinition_t forDefinition) {
  assert((!decl->isGenericContext() || decl->hasClangNode()) &&
         "generic classes do not have a static metaclass object");

  auto entity = decl->getMetaclassKind() == ClassDecl::MetaclassKind::ObjC
                    ? LinkEntity::forObjCMetaclass(decl)
                    : LinkEntity::forSwiftMetaclassStub(decl);

  auto DbgTy = DebugTypeInfo::getObjCClass(
      decl, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, forDefinition, DbgTy);
  return addr;
}

llvm::Constant *
IRGenModule::getAddrOfCanonicalSpecializedGenericMetaclassObject(
    CanType concreteType, ForDefinition_t forDefinition) {
  auto *theClass = concreteType->getClassOrBoundGenericClass();
  assert(theClass && "only classes have metaclasses");
  assert(concreteType->getClassOrBoundGenericClass()->isGenericContext());

  auto entity =
      LinkEntity::forSpecializedGenericSwiftMetaclassStub(concreteType);

  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, forDefinition, DbgTy);
  return addr;
}

/// Fetch the declaration of an Objective-C metadata update callback.
llvm::Function *
IRGenModule::getAddrOfObjCMetadataUpdateFunction(ClassDecl *classDecl,
                                                 ForDefinition_t forDefinition) {
  assert(ObjCInterop);

  LinkEntity entity = LinkEntity::forObjCMetadataUpdateFunction(classDecl);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // Class _Nullable callback(Class _Nonnull cls, void * _Nullable arg);
  Signature signature(ObjCUpdateCallbackTy, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the declaration of an Objective-C resilient class stub.
llvm::Constant *
IRGenModule::getAddrOfObjCResilientClassStub(ClassDecl *classDecl,
                                             ForDefinition_t forDefinition,
                                             TypeMetadataAddress addr) {
  assert(ObjCInterop);
  assert(getClassMetadataStrategy(classDecl) ==
         ClassMetadataStrategy::Resilient);

  LinkEntity entity = LinkEntity::forObjCResilientClassStub(classDecl, addr);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

llvm::IntegerType *IRGenModule::getTypeMetadataRequestParamTy() {
  return SizeTy;
}

llvm::StructType *IRGenModule::getTypeMetadataResponseTy() {
  return TypeMetadataResponseTy;
}

/// Fetch the type metadata access function for a non-generic type.
llvm::Function *
IRGenModule::getAddrOfTypeMetadataAccessFunction(CanType type,
                                              ForDefinition_t forDefinition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  NominalTypeDecl *Nominal = type->getNominalOrBoundGenericNominal();
  IRGen.noteUseOfTypeMetadata(Nominal);

  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *params[] = {getTypeMetadataRequestParamTy()}; // MetadataRequest
  auto fnType =
      llvm::FunctionType::get(getTypeMetadataResponseTy(), params, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the opaque type descriptor access function.
FunctionPointer IRGenModule::getAddrOfOpaqueTypeDescriptorAccessFunction(
    OpaqueTypeDecl *decl, ForDefinition_t forDefinition, bool implementation) {
  IRGen.noteUseOfOpaqueTypeDescriptor(decl);

  LinkEntity entity =
      implementation ? LinkEntity::forOpaqueTypeDescriptorAccessorImpl(decl)
                     : LinkEntity::forOpaqueTypeDescriptorAccessor(decl);

  auto fnType = llvm::FunctionType::get(OpaqueTypeDescriptorPtrTy, {}, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition)
      updateLinkageForDefinition(*this, entry, entity);
    return FunctionPointer::forDirect(FunctionPointer::Kind::Function, entry,
                                      nullptr, signature);
  }

  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return FunctionPointer::forDirect(FunctionPointer::Kind::Function, entry,
                                    nullptr, signature);
}

/// Fetch the type metadata access function for the given generic type.
llvm::Function *
IRGenModule::getAddrOfGenericTypeMetadataAccessFunction(
                                           NominalTypeDecl *nominal,
                                           ArrayRef<llvm::Type *> genericArgs,
                                           ForDefinition_t forDefinition) {
  assert(nominal->isGenericContext());
  assert(!genericArgs.empty() ||
         nominal->getGenericSignature()->areAllParamsConcrete());
  IRGen.noteUseOfTypeMetadata(nominal);

  auto type = nominal->getDeclaredType()->getCanonicalType();
  assert(type->hasUnboundGenericType());
  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // If we have more arguments than can be passed directly, all of the
  // generic arguments are passed as an array.
  llvm::Type *paramTypesArray[NumDirectGenericTypeMetadataAccessFunctionArgs+1];

  paramTypesArray[0] = SizeTy; // MetadataRequest
  size_t numParams = 1;

  size_t numGenericArgs = genericArgs.size();
  if (numGenericArgs > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    paramTypesArray[1] = Int8PtrPtrTy;
    ++numParams;
  } else {
    for (size_t i : indices(genericArgs))
      paramTypesArray[i + 1] = genericArgs[i];
    numParams += numGenericArgs;
  }

  auto paramTypes = llvm::makeArrayRef(paramTypesArray, numParams);
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        paramTypes, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfCanonicalSpecializedGenericTypeMetadataAccessFunction(
    CanType theType, ForDefinition_t forDefinition) {
  assert(shouldPrespecializeGenericMetadata());
  assert(!theType->hasUnboundGenericType());
  auto *nominal = theType->getAnyNominal();
  assert(nominal);
  assert(nominal->isGenericContext());

  IRGen.noteUseOfCanonicalSpecializedMetadataAccessor(theType);

  LinkEntity entity =
      LinkEntity::forPrespecializedTypeMetadataAccessFunction(theType);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition)
      updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *paramTypesArray[1];
  paramTypesArray[0] = SizeTy; // MetadataRequest

  auto paramTypes = llvm::makeArrayRef(paramTypesArray, 1);
  auto functionType =
      llvm::FunctionType::get(TypeMetadataResponseTy, paramTypes, false);
  Signature signature(functionType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Get or create a type metadata cache variable.  These are an
/// implementation detail of type metadata access functions.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataLazyCacheVariable(CanType type) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forTypeMetadataLazyCacheVariable(type);
  auto variable =
    getAddrOfLLVMVariable(entity, ForDefinition, DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  cast<llvm::GlobalVariable>(variable)->setInitializer(
    llvm::ConstantPointerNull::get(TypeMetadataPtrTy));

  return variable;
}

llvm::Constant *
IRGenModule::getAddrOfCanonicalPrespecializedGenericTypeCachingOnceToken(
    NominalTypeDecl *decl) {
  assert(decl->isGenericContext());
  LinkEntity entity =
      LinkEntity::forCanonicalPrespecializedGenericTypeCachingOnceToken(decl);
  if (auto &entry = GlobalVars[entity]) {
    return entry;
  }
  auto variable = getAddrOfLLVMVariable(entity, ForDefinition, DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  cast<llvm::GlobalVariable>(variable)->setInitializer(
      llvm::ConstantInt::get(OnceTy, 0));

  return variable;
}

llvm::Constant *
IRGenModule::getAddrOfNoncanonicalSpecializedGenericTypeMetadataCacheVariable(CanType type) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forNoncanonicalSpecializedGenericTypeMetadataCacheVariable(type);
  if (auto &entry = GlobalVars[entity]) {
    return entry;
  }
  auto variable =
    getAddrOfLLVMVariable(entity, ForDefinition, DebugTypeInfo());

  cast<llvm::GlobalVariable>(variable)->setInitializer(
    llvm::ConstantPointerNull::get(TypeMetadataPtrTy));

  return variable;
}

/// Get or create a type metadata cache variable.  These are an
/// implementation detail of type metadata access functions.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataDemanglingCacheVariable(CanType type,
                                                      ConstantInit definition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forTypeMetadataDemanglingCacheVariable(type);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataSingletonInitializationCache(
                                           NominalTypeDecl *D,
                                           ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forTypeMetadataSingletonInitializationCache(D);
  auto variable =
    getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  if (forDefinition) {
    auto globalVar = cast<llvm::GlobalVariable>(variable);
    globalVar->setInitializer(
        llvm::Constant::getNullValue(globalVar->getValueType()));
  }

  return variable;
}

llvm::GlobalValue *IRGenModule::defineAlias(LinkEntity entity,
                                            llvm::Constant *definition,
                                            llvm::Type *typeOfValue) {
  // Check for an existing forward declaration of the alias.
  auto &entry = GlobalVars[entity];
  llvm::GlobalValue *existingVal = nullptr;
  if (entry) {
    existingVal = cast<llvm::GlobalValue>(entry);
    // Clear the existing value's name so we can steal it.
    existingVal->setName("");
  }

  LinkInfo link = LinkInfo::get(*this, entity, ForDefinition);
  auto *ptrTy = cast<llvm::PointerType>(definition->getType());
  auto *alias = llvm::GlobalAlias::create(typeOfValue, ptrTy->getAddressSpace(),
                                          link.getLinkage(), link.getName(),
                                          definition, &Module);
  ApplyIRLinkage({link.getLinkage(), link.getVisibility(), link.getDLLStorage()})
      .to(alias);

  markGlobalAsUsedBasedOnLinkage(*this, link, alias);

  // Replace an existing external declaration for the address point.
  if (entry) {
    auto existingVal = cast<llvm::GlobalValue>(entry);

    for (auto iterator = std::begin(LLVMUsed); iterator < std::end(LLVMUsed); ++iterator) {
      llvm::Value *thisValue = *iterator;
      if (thisValue == existingVal) {
        LLVMUsed.erase(iterator);
      }
    }
    for (auto iterator = std::begin(LLVMCompilerUsed); iterator < std::end(LLVMCompilerUsed); ++iterator) {
      llvm::Value *thisValue = *iterator;
      if (thisValue == existingVal) {
        LLVMCompilerUsed.erase(iterator);
      }
    }

    // FIXME: MC breaks when emitting alias references on some platforms
    // (rdar://problem/22450593 ). Work around this by referring to the aliasee
    // instead.
    llvm::Constant *aliasCast = alias->getAliasee();
    aliasCast = llvm::ConstantExpr::getBitCast(aliasCast,
                                               entry->getType());
    existingVal->replaceAllUsesWith(aliasCast);
    existingVal->eraseFromParent();
  }
  entry = alias;

  return alias;
}

/// Define the metadata for a type.
///
/// Some type metadata has information before the address point that the
/// public symbol for the metadata references. This function will rewrite any
/// existing external declaration to the address point as an alias into the
/// full metadata object.
llvm::GlobalValue *IRGenModule::defineTypeMetadata(
    CanType concreteType, bool isPattern, bool isConstant,
    ConstantInitFuture init, llvm::StringRef section,
    SmallVector<std::pair<Size, SILDeclRef>, 8> vtableEntries) {
  assert(init);

  auto concreteTypeDecl = concreteType->getAnyGeneric();
  auto isPrespecialized = concreteTypeDecl &&
                          concreteTypeDecl->isGenericContext();
  bool isObjCImpl = concreteTypeDecl &&
                    concreteTypeDecl->getObjCImplementationDecl();

  if (isPattern) {
    assert(isConstant && "Type metadata patterns must be constant");
    auto addr = getAddrOfTypeMetadataPattern(concreteType->getAnyNominal(),
                                             init, section);

    return cast<llvm::GlobalValue>(addr);
  }

  auto entity =
      (isPrespecialized &&
       !irgen::isCanonicalInitializableTypeMetadataStaticallyAddressable(
           *this, concreteType))
          ? LinkEntity::forNoncanonicalSpecializedGenericTypeMetadata(
                concreteType)
          : (isObjCImpl
                ? LinkEntity::forObjCClass(
                      concreteType->getClassOrBoundGenericClass())
                : LinkEntity::forTypeMetadata(
                      concreteType, TypeMetadataAddress::FullMetadata));

  auto DbgTy = DebugTypeInfo::getGlobalMetadata(
      MetatypeType::get(concreteType),
      entity.getDefaultDeclarationType(*this)->getPointerTo(), Size(0),
      Alignment(1));

  // Define the variable.
  llvm::GlobalVariable *var = cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, init, DbgTy));

  var->setConstant(isConstant);
  if (!section.empty())
    var->setSection(section);

  if (getOptions().VirtualFunctionElimination) {
    if (auto classDecl = concreteType->getClassOrBoundGenericClass()) {
      addVTableTypeMetadata(classDecl, var, vtableEntries);
    }
  }

  LinkInfo link = LinkInfo::get(*this, entity, ForDefinition);
  markGlobalAsUsedBasedOnLinkage(*this, link, var);

  /// For concrete metadata, we want to use the initializer on the
  /// "full metadata", and define the "direct" address point as an alias.
  unsigned adjustmentIndex = MetadataAdjustmentIndex::ValueType;

  if (auto nominal = concreteType->getAnyNominal()) {
    // Keep type metadata around for all types (except @_objcImplementation,
    // since we're using ObjC metadata for that).
    if (!isObjCImpl)
      addRuntimeResolvableType(nominal);

    // Don't define the alias for foreign type metadata, prespecialized
    // generic metadata, or @_objcImplementation classes, since they're not ABI.
    if (requiresForeignTypeMetadata(nominal) || isPrespecialized || isObjCImpl)
      return var;

    // Native Swift class metadata has a destructor before the address point.
    if (isa<ClassDecl>(nominal)) {
      adjustmentIndex = MetadataAdjustmentIndex::Class;
    }

    if (concreteType->is<TupleType>()) {
      adjustmentIndex = MetadataAdjustmentIndex::NoTypeLayoutString;
    }
  }

  llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)};
  auto addr = llvm::ConstantExpr::getInBoundsGetElementPtr(var->getValueType(),
                                                           var, indices);
  addr = llvm::ConstantExpr::getBitCast(addr, TypeMetadataPtrTy);

  // For concrete metadata, declare the alias to its address point.
  auto directEntity = LinkEntity::forTypeMetadata(
      concreteType, TypeMetadataAddress::AddressPoint);
  return defineAlias(directEntity, addr, TypeMetadataStructTy);
}

/// Fetch the declaration of the (possibly uninitialized) metadata for a type.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadata(CanType concreteType,
                                   TypeMetadataCanonicality canonicality) {
  return getAddrOfTypeMetadata(concreteType, SymbolReferenceKind::Absolute,
                               canonicality)
      .getDirectValue();
}

ConstantReference
IRGenModule::getAddrOfTypeMetadata(CanType concreteType,
                                   SymbolReferenceKind refKind,
                                   TypeMetadataCanonicality canonicality) {
  assert(!isa<UnboundGenericType>(concreteType));

  auto nominal = concreteType->getAnyNominal();

  bool foreign = nominal && requiresForeignTypeMetadata(nominal);

  // Foreign classes and prespecialized generic types do not use an alias into
  // the full metadata and therefore require a GEP.
  bool fullMetadata =
      foreign || (concreteType->getAnyGeneric() &&
                  concreteType->getAnyGeneric()->isGenericContext());

  llvm::Type *defaultVarTy;
  unsigned adjustmentIndex;
  if (concreteType->isAny() || concreteType->isAnyObject() || concreteType->isVoid() || concreteType->is<TupleType>() || concreteType->is<BuiltinType>()) {
    defaultVarTy = FullExistentialTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::NoTypeLayoutString;
  } else if (fullMetadata) {
    defaultVarTy = FullTypeMetadataStructTy;
    if (concreteType->getClassOrBoundGenericClass() && !foreign) {
      adjustmentIndex = MetadataAdjustmentIndex::Class;
    } else {
      adjustmentIndex = MetadataAdjustmentIndex::ValueType;
    }
  } else if (nominal) {
    // The symbol for native non-generic nominal type metadata is generated at
    // the aliased address point (see defineTypeMetadata() above).
    if (nominal->getObjCImplementationDecl()) {
      defaultVarTy = ObjCClassStructTy;
    } else {
      assert(!nominal->hasClangNode());
      defaultVarTy = TypeMetadataStructTy;
    }
    adjustmentIndex = 0;
  } else {
    // FIXME: Non-nominal metadata provided by the C++ runtime is exported
    // with the address of the start of the full metadata object, since
    // Clang doesn't provide an easy way to emit symbols aliasing into the
    // middle of an object.
    defaultVarTy = FullTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  }

  // If this is a use, and the type metadata is emitted lazily,
  // trigger lazy emission of the metadata.
  if (NominalTypeDecl *nominal = concreteType->getAnyNominal()) {
    IRGen.noteUseOfTypeMetadata(nominal);
  }

  if (shouldPrespecializeGenericMetadata()) {
    if (auto nominal = concreteType->getAnyNominal()) {
      if (nominal->isGenericContext()) {
        IRGen.noteUseOfSpecializedGenericTypeMetadata(*this, concreteType,
                                                      canonicality);
      }
    }
  }

  llvm::Optional<LinkEntity> entity;
  DebugTypeInfo DbgTy;

  switch (canonicality) {
  case TypeMetadataCanonicality::Canonical: {
    auto classDecl = concreteType->getClassOrBoundGenericClass();
    if (classDecl && classDecl->getObjCImplementationDecl()) {
      entity = LinkEntity::forObjCClass(classDecl);
    } else {
      entity = LinkEntity::forTypeMetadata(
          concreteType, fullMetadata ? TypeMetadataAddress::FullMetadata
                                     : TypeMetadataAddress::AddressPoint);
    }
    break;
  }
  case TypeMetadataCanonicality::Noncanonical:
    entity =
        LinkEntity::forNoncanonicalSpecializedGenericTypeMetadata(concreteType);
    break;
  }
  DbgTy = DebugTypeInfo::getGlobalMetadata(MetatypeType::get(concreteType),
                                           defaultVarTy->getPointerTo(),
                                           Size(0), Alignment(1));

  ConstantReference addr;
  llvm::Type *typeOfValue = nullptr;
  if (fullMetadata && !foreign) {
    addr = getAddrOfLLVMVariable(*entity, ConstantInit(), DbgTy, refKind,
                                 /*overrideDeclType=*/nullptr);
    typeOfValue = entity->getDefaultDeclarationType(*this);
  } else {
    addr = getAddrOfLLVMVariable(*entity, ConstantInit(), DbgTy, refKind,
                                 /*overrideDeclType=*/defaultVarTy);
    typeOfValue = defaultVarTy;
  }

  if (auto *GV = dyn_cast<llvm::GlobalVariable>(addr.getValue()))
    if (GV->isDeclaration())
      GV->setComdat(nullptr);

  // FIXME: MC breaks when emitting alias references on some platforms
  // (rdar://problem/22450593 ). Work around this by referring to the aliasee
  // instead.
  if (auto alias = dyn_cast<llvm::GlobalAlias>(addr.getValue())) {
    addr = ConstantReference(alias->getAliasee(), addr.isIndirect());
  }

  // Adjust if necessary.
  if (adjustmentIndex) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = ConstantReference(llvm::ConstantExpr::getInBoundsGetElementPtr(
                                 typeOfValue, addr.getValue(), indices),
                             addr.isIndirect());
  }
  
  return addr;
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D) {
  return getAddrOfTypeMetadataPattern(D, ConstantInit(), "");
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D,
                                          ConstantInit init,
                                          StringRef section) {
  if (!init)
    IRGen.noteUseOfTypeMetadata(D);

  LinkEntity entity = LinkEntity::forTypeMetadataPattern(D);
  auto addr = getAddrOfLLVMVariable(entity, init, DebugTypeInfo());

  if (init) {
    auto var = cast<llvm::GlobalVariable>(addr);
    var->setConstant(true);
    if (!section.empty())
      var->setSection(section);

    // Keep type metadata around for all types.
    addRuntimeResolvableType(D);
  }

  return addr;
}

/// Returns the address of a class metadata base offset.
llvm::Constant *
IRGenModule::getAddrOfClassMetadataBounds(ClassDecl *D,
                                          ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forClassMetadataBaseOffset(D);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

/// Return the address of a generic type's metadata instantiation cache.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataInstantiationCache(NominalTypeDecl *D,
                                                ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forTypeMetadataInstantiationCache(D);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataInstantiationFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataInstantiationFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // This function is used in two cases -- allocating generic type metadata,
  // and relocating non-generic resilient class metadata.
  llvm::FunctionType *fnType;

  if (D->isGenericContext()) {
    // MetadataInstantiator in ABI/Metadata.h
    llvm::Type *argTys[] = {
      /// Type descriptor.
      TypeContextDescriptorPtrTy,
      /// Generic arguments.
      Int8PtrPtrTy,
      /// Generic metadata pattern.
      Int8PtrTy
    };

    fnType = llvm::FunctionType::get(TypeMetadataPtrTy, argTys,
                                     /*isVarArg*/ false);
  } else {
    assert(isa<ClassDecl>(D));

    // MetadataRelocator in ABI/Metadata.h
    llvm::Type *argTys[] = {
      /// Type descriptor.
      TypeContextDescriptorPtrTy,
      /// Resilient metadata pattern.
      Int8PtrTy
    };

    fnType = llvm::FunctionType::get(TypeMetadataPtrTy, argTys,
                                     /*isVarArg*/ false);
  }

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataCompletionFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataCompletionFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *argTys[] = {
    /// Type metadata.
    TypeMetadataPtrTy,
    /// Metadata completion context.
    Int8PtrTy,
    /// Generic metadata pattern.
    Int8PtrPtrTy
  };
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        argTys, /*isVarArg*/ false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Return the address of a nominal type descriptor.
llvm::Constant *IRGenModule::getAddrOfTypeContextDescriptor(NominalTypeDecl *D,
                                              RequireMetadata_t requireMetadata,
                                              ConstantInit definition) {
  IRGen.noteUseOfTypeContextDescriptor(D, requireMetadata);

  auto entity = LinkEntity::forNominalTypeDescriptor(D);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfOpaqueTypeDescriptor(
                                               OpaqueTypeDecl *opaqueType,
                                               ConstantInit definition) {
  IRGen.noteUseOfOpaqueTypeDescriptor(opaqueType);
  auto entity = LinkEntity::forOpaqueTypeDescriptor(opaqueType);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionBuiltinDescriptor(CanType type,
                                     ConstantInit definition) {
  auto entity = LinkEntity::forReflectionBuiltinDescriptor(type);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionFieldDescriptor(CanType type,
                                   ConstantInit definition) {
  auto entity = LinkEntity::forReflectionFieldDescriptor(type);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfReflectionAssociatedTypeDescriptor(const ProtocolConformance *c,
                                            ConstantInit definition) {
  auto entity = LinkEntity::forReflectionAssociatedTypeDescriptor(c);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

/// Return the address of a property descriptor.
llvm::Constant *IRGenModule::getAddrOfPropertyDescriptor(AbstractStorageDecl *D,
                                                      ConstantInit definition) {
  auto entity = LinkEntity::forPropertyDescriptor(D);
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolDescriptor(ProtocolDecl *D,
                                                      ConstantInit definition) {
  if (D->isObjC()) {
    assert(!definition &&
           "cannot define an @objc protocol descriptor this way");
    return getAddrOfObjCProtocolRecord(D, NotForDefinition);
  }

  auto entity = LinkEntity::forProtocolDescriptor(D);
  return getAddrOfLLVMVariable(entity, definition,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolRequirementsBaseDescriptor(
                                                         ProtocolDecl *proto) {
  auto entity = LinkEntity::forProtocolRequirementsBaseDescriptor(proto);
  return getAddrOfLLVMVariable(entity, ConstantInit(),
                               DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineProtocolRequirementsBaseDescriptor(
                                                ProtocolDecl *proto,
                                                llvm::Constant *definition) {
  auto entity = LinkEntity::forProtocolRequirementsBaseDescriptor(proto);
  return defineAlias(entity, definition,
                     entity.getDefaultDeclarationType(*this));
}

llvm::Constant *IRGenModule::getAddrOfAssociatedTypeDescriptor(
                                               AssociatedTypeDecl *assocType) {
  auto entity = LinkEntity::forAssociatedTypeDescriptor(assocType);
  return getAddrOfLLVMVariable(entity, ConstantInit(),
                               DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineAssociatedTypeDescriptor(
                                                 AssociatedTypeDecl *assocType,
                                                 llvm::Constant *definition) {
  auto entity = LinkEntity::forAssociatedTypeDescriptor(assocType);
  return defineAlias(entity, definition,
                     entity.getDefaultDeclarationType(*this));
}

llvm::Constant *IRGenModule::getAddrOfAssociatedConformanceDescriptor(
                                          AssociatedConformance conformance) {
  auto entity = LinkEntity::forAssociatedConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineAssociatedConformanceDescriptor(
                                            AssociatedConformance conformance,
                                            llvm::Constant *definition) {
  auto entity = LinkEntity::forAssociatedConformanceDescriptor(conformance);
  return defineAlias(entity, definition,
                     entity.getDefaultDeclarationType(*this));
}

llvm::Constant *IRGenModule::getAddrOfBaseConformanceDescriptor(
                                                BaseConformance conformance) {
  auto entity = LinkEntity::forBaseConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
}

llvm::GlobalValue *IRGenModule::defineBaseConformanceDescriptor(
                                            BaseConformance conformance,
                                            llvm::Constant *definition) {
  auto entity = LinkEntity::forBaseConformanceDescriptor(conformance);
  return defineAlias(entity, definition,
                     entity.getDefaultDeclarationType(*this));
}

llvm::Constant *IRGenModule::getAddrOfProtocolConformanceDescriptor(
                                const RootProtocolConformance *conformance,
                                ConstantInit definition) {
  IRGen.addLazyWitnessTable(conformance);

  auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, definition,
                               DebugTypeInfo());
}

/// Fetch the declaration of the ivar initializer for the given class.
llvm::Optional<llvm::Function *>
IRGenModule::getAddrOfIVarInitDestroy(ClassDecl *cd, bool isDestroyer,
                                      bool isForeign,
                                      ForDefinition_t forDefinition) {
  auto silRef = SILDeclRef(cd,
                           isDestroyer
                           ? SILDeclRef::Kind::IVarDestroyer
                           : SILDeclRef::Kind::IVarInitializer)
    .asForeign(isForeign);

  // Find the SILFunction for the ivar initializer or destroyer.
  if (auto silFn = getSILModule().lookUpFunction(silRef)) {
    return getAddrOfSILFunction(silFn, forDefinition);
  }

  return llvm::None;
}

/// Returns the address of a value-witness function.
llvm::Function *IRGenModule::getAddrOfValueWitness(CanType abstractType,
                                                   ValueWitness index,
                                                ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forValueWitness(abstractType, index);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getValueWitnessSignature(index);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Returns the address of a value-witness table.  If a definition
/// type is provided, the table is created with that type; the return
/// value will be an llvm::GlobalValue.  Otherwise, the result will
/// have type WitnessTablePtrTy.
llvm::Constant *
IRGenModule::getAddrOfValueWitnessTable(CanType concreteType,
                                        ConstantInit definition) {
  LinkEntity entity = LinkEntity::forValueWitnessTable(concreteType);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

static Address getAddrOfSimpleVariable(IRGenModule &IGM,
                             llvm::DenseMap<LinkEntity, llvm::Constant*> &cache,
                             LinkEntity entity,
                             ForDefinition_t forDefinition) {
  auto alignment = entity.getAlignment(IGM);
  auto type = entity.getDefaultDeclarationType(IGM);
  
  // Check whether it's already cached.
  llvm::Constant *&entry = cache[entity];
  if (entry) {
    auto existing = cast<llvm::GlobalVariable>(entry);
    assert(alignment == Alignment(existing->getAlignment()));
    if (forDefinition) updateLinkageForDefinition(IGM, existing, entity);
    return Address(entry, type, alignment);
  }

  // Otherwise, we need to create it.
  LinkInfo link = LinkInfo::get(IGM, entity, forDefinition);
  auto addr = createVariable(IGM, link, type, alignment);

  entry = addr;
  return Address(addr, type, alignment);
}

/// getAddrOfFieldOffset - Get the address of the global variable
/// which contains an offset to apply to either an object (if direct)
/// or a metadata object in order to find an offset to apply to an
/// object (if indirect).
///
/// The result is always a GlobalValue.
Address IRGenModule::getAddrOfFieldOffset(VarDecl *var,
                                          ForDefinition_t forDefinition) {
  assert(!Lowering::shouldSkipLowering(var));

  LinkEntity entity = LinkEntity::forFieldOffset(var);
  return getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                 forDefinition);
}

Address IRGenModule::getAddrOfEnumCase(EnumElementDecl *Case,
                                       ForDefinition_t forDefinition) {
  assert(!Lowering::shouldSkipLowering(Case));

  LinkEntity entity = LinkEntity::forEnumCase(Case);
  auto addr = getAddrOfSimpleVariable(*this, GlobalVars, entity, forDefinition);

  auto *global = cast<llvm::GlobalVariable>(addr.getAddress());
  global->setConstant(true);

  return addr;
}

void IRGenModule::emitNestedTypeDecls(DeclRange members) {
  for (Decl *member : members) {
    if (Lowering::shouldSkipLowering(member))
      continue;

    member->visitAuxiliaryDecls([&](Decl *decl) {
      emitNestedTypeDecls({decl, nullptr});
    });
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::Param:
    case DeclKind::Module:
    case DeclKind::PrecedenceGroup:
      llvm_unreachable("decl not allowed in type context");

    case DeclKind::BuiltinTuple:
      llvm_unreachable("BuiltinTupleType made it to IRGen");

    case DeclKind::Missing:
      llvm_unreachable("missing decl in IRGen");

    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
    case DeclKind::Macro:
      continue;

    case DeclKind::Func:
    case DeclKind::Var:
    case DeclKind::Subscript:
      // Handled in SIL.
      continue;
        
    case DeclKind::PatternBinding:
    case DeclKind::Accessor:
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::EnumCase:
    case DeclKind::EnumElement:
    case DeclKind::MissingMember:
      // Skip non-type members.
      continue;

    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      // Do nothing.
      continue;

    case DeclKind::TypeAlias:
    case DeclKind::OpaqueType:
      // Do nothing.
      continue;

    case DeclKind::Enum:
      emitEnumDecl(cast<EnumDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::MacroExpansion:
      // Expansion already visited as auxiliary decls.
      continue;
    }
  }
}

static bool shouldEmitCategory(IRGenModule &IGM, ExtensionDecl *ext) {
  if (ext->isObjCImplementation()) {
    assert(ext->getCategoryNameForObjCImplementation() != Identifier());
    return true;
  }

  for (auto conformance : ext->getLocalConformances()) {
    if (conformance->getProtocol()->isObjC())
      return true;
  }

  for (auto member : ext->getAllMembers()) {
    if (auto func = dyn_cast<FuncDecl>(member)) {
      if (requiresObjCMethodDescriptor(func))
        return true;
    } else if (auto constructor = dyn_cast<ConstructorDecl>(member)) {
      if (requiresObjCMethodDescriptor(constructor))
        return true;
    } else if (auto var = dyn_cast<VarDecl>(member)) {
      if (requiresObjCPropertyDescriptor(IGM, var))
        return true;
    } else if (auto subscript = dyn_cast<SubscriptDecl>(member)) {
      if (requiresObjCSubscriptDescriptor(IGM, subscript))
        return true;
    }
  }

  return false;
}

void IRGenModule::emitExtension(ExtensionDecl *ext) {
  emitNestedTypeDecls(ext->getMembers());

  addLazyConformances(ext);

  // Generate a category if the extension either introduces a
  // conformance to an ObjC protocol or introduces a method
  // that requires an Objective-C entry point.
  ClassDecl *origClass = ext->getSelfClassDecl();
  if (!origClass)
    return;

  if (ext->isObjCImplementation()
        && ext->getCategoryNameForObjCImplementation() == Identifier()) {
    // This is the @_objcImplementation for the class--generate its class
    // metadata.
    emitClassDecl(origClass);
  } else if (shouldEmitCategory(*this, ext)) {
    assert(origClass && !origClass->isForeign() &&
           "foreign types cannot have categories emitted");
    llvm::Constant *category = emitCategoryData(*this, ext);
    category = llvm::ConstantExpr::getBitCast(category, Int8PtrTy);

    auto *theClass = ext->getSelfClassDecl();

    // Categories on class stubs are added to a separate list.
    if (theClass->checkAncestry(AncestryFlags::ResilientOther))
      ObjCCategoriesOnStubs.push_back(category);
    else
      ObjCCategories.push_back(category);

    ObjCCategoryDecls.push_back(ext);
  }
}


/// Create an allocation on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca =
      new llvm::AllocaInst(type, IGM.DataLayout.getAllocaAddrSpace(), name,
                           AllocaIP);
  alloca->setAlignment(llvm::MaybeAlign(alignment.getValue()).valueOrOne());
  return Address(alloca, type, alignment);
}

/// Create an allocation of an array on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    llvm::Value *ArraySize,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca = new llvm::AllocaInst(
      type, IGM.DataLayout.getAllocaAddrSpace(), ArraySize,
      llvm::MaybeAlign(alignment.getValue()).valueOrOne(), name, AllocaIP);
  return Address(alloca, type, alignment);
}

/// Get or create a global string constant.
///
/// \returns an i8* with a null terminator; note that embedded nulls
///   are okay
///
/// FIXME: willBeRelativelyAddressed is only needed to work around an ld64 bug
/// resolving relative references to coalesceable symbols.
/// It should be removed when fixed. rdar://problem/22674524
llvm::Constant *IRGenModule::getAddrOfGlobalString(StringRef data,
                                               bool willBeRelativelyAddressed) {
  // Check whether this string already exists.
  auto &entry = GlobalStrings[data];
  if (entry.second) {
    // FIXME: Clear unnamed_addr if the global will be relative referenced
    // to work around an ld64 bug. rdar://problem/22674524
    if (willBeRelativelyAddressed)
      entry.first->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    return entry.second;
  }

  SmallString<64> name;
  (llvm::Twine(".str.") + llvm::Twine(data.size()) + "." + data).toVector(name);
  
  // \0 is not allowed in variable names. Rewrite any \0s into _s and append
  // information about their original locations so the name remains unique.
  for (size_t i = name.find('\0');
       i != StringRef::npos;
       i = name.find('\0', i)) {
    name[i] = '_';
    (llvm::Twine(".nul") + llvm::Twine(i)).toVector(name);
  }
  
  entry = createStringConstant(data, willBeRelativelyAddressed,
                               /*sectionName*/ "", name);
  return entry.second;
}

/// Get or create a global UTF-16 string constant.
///
/// \returns an i16* with a null terminator; note that embedded nulls
///   are okay
llvm::Constant *IRGenModule::getAddrOfGlobalUTF16String(StringRef utf8) {
  // Check whether this string already exists.
  auto &entry = GlobalUTF16Strings[utf8];
  if (entry) return entry;

  // If not, first transcode it to UTF16.
  SmallVector<llvm::UTF16, 128> buffer(utf8.size() + 1); // +1 for ending nulls.
  const llvm::UTF8 *fromPtr = (const llvm::UTF8 *) utf8.data();
  llvm::UTF16 *toPtr = &buffer[0];
  (void) ConvertUTF8toUTF16(&fromPtr, fromPtr + utf8.size(),
                            &toPtr, toPtr + utf8.size(),
                            llvm::strictConversion);

  // The length of the transcoded string in UTF-8 code points.
  size_t utf16Length = toPtr - &buffer[0];

  // Null-terminate the UTF-16 string.
  *toPtr = 0;
  ArrayRef<llvm::UTF16> utf16(&buffer[0], utf16Length + 1);

  auto init = llvm::ConstantDataArray::get(getLLVMContext(), utf16);
  auto global = new llvm::GlobalVariable(
      Module, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init,
      ".str" /* match how Clang creates strings */);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Drill down to make an i16*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
      global->getValueType(), global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Can not treat a treat the layout of a class as resilient if the current
///    class is defined in an external module and
///    not publicly accessible (e.g private or internal).
/// This would normally not happen except if we compile theClass's module with
/// enable-testing.
/// Do we have to use resilient access patterns when working with this
/// declaration?
///
/// IRGen is primarily concerned with resilient handling of the following:
/// - For structs, a struct's size might change
/// - For enums, new cases can be added
/// - For classes, the superclass might change the size or number
///   of stored properties
bool IRGenModule::isResilient(NominalTypeDecl *D,
                              ResilienceExpansion expansion,
                              ClassDecl *asViewedFromRootClass) {
  assert(!asViewedFromRootClass || isa<ClassDecl>(D));

  if (D->getModuleContext()->getBypassResilience())
    return false;
  if (expansion == ResilienceExpansion::Maximal &&
      Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile) {
    return false;
  }

  return D->isResilient(getSwiftModule(), expansion);
}

/// Do we have to use resilient access patterns when working with this
/// class?
///
/// For classes, this means that virtual method calls use dispatch thunks
/// rather than accessing metadata members directly.
bool IRGenModule::hasResilientMetadata(ClassDecl *D,
                                       ResilienceExpansion expansion,
                                       ClassDecl *asViewedFromRootClass) {
  if (expansion == ResilienceExpansion::Maximal &&
      Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile) {
    return false;
  }

  return D->hasResilientMetadata(getSwiftModule(), expansion);
}

// The most general resilience expansion where the given declaration is visible.
ResilienceExpansion
IRGenModule::getResilienceExpansionForAccess(NominalTypeDecl *decl) {
  if (decl->getModuleContext() == getSwiftModule() &&
      decl->getEffectiveAccess() < AccessLevel::Public)
    return ResilienceExpansion::Maximal;
  return ResilienceExpansion::Minimal;
}

// The most general resilience expansion which has knowledge of the declaration's
// layout. Calling isResilient() with this scope will always return false.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(NominalTypeDecl *decl) {
  if (Types.getLoweringMode() == TypeConverter::Mode::CompletelyFragile)
    return ResilienceExpansion::Minimal;

  if (isResilient(decl, ResilienceExpansion::Minimal))
    return ResilienceExpansion::Maximal;

  return getResilienceExpansionForAccess(decl);
}

// The most general resilience expansion which has knowledge of the global
// variable's layout.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(SILGlobalVariable *global) {
  if (hasPublicVisibility(global->getLinkage()))
    return ResilienceExpansion::Minimal;
  return ResilienceExpansion::Maximal;
}

llvm::Function *
IRGenModule::getAddrOfGenericWitnessTableInstantiationFunction(
                                      const NormalProtocolConformance *conf) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forGenericProtocolWitnessTableInstantiationFunction(conf);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto fnType = llvm::FunctionType::get(
      VoidTy, {WitnessTablePtrTy, TypeMetadataPtrTy, Int8PtrPtrTy},
      /*varargs*/ false);
  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the lazy witness table access function for a protocol conformance.
llvm::Function *
IRGenModule::getAddrOfWitnessTableLazyAccessFunction(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyAccessFunction(conf, conformingType);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::FunctionType *fnType
    = llvm::FunctionType::get(WitnessTablePtrTy, false);

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  ApplyIRLinkage({link.getLinkage(), link.getVisibility(), link.getDLLStorage()})
      .to(entry, link.isForDefinition());
  return entry;
}

/// Get or create a witness table cache variable.  These are an
/// implementation detail of witness table lazy access functions.
llvm::Constant *
IRGenModule::getAddrOfWitnessTableLazyCacheVariable(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  assert(!conformingType->hasArchetype());
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyCacheVariable(conf, conformingType);
  auto variable = getAddrOfLLVMVariable(entity,
                                        forDefinition,
                                        DebugTypeInfo());

  // Zero-initialize if we're asking for a definition.
  if (forDefinition) {
    cast<llvm::GlobalVariable>(variable)->setInitializer(
      llvm::ConstantPointerNull::get(WitnessTablePtrTy));
  }

  return variable;
}

/// Look up the address of a witness table.
///
/// This can only be used with non-dependent conformances.
llvm::Constant*
IRGenModule::getAddrOfWitnessTable(const RootProtocolConformance *conf,
                                   ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forProtocolWitnessTable(conf);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

/// Look up the address of a witness table pattern.
///
/// This can only be used with dependent conformances from inside the
/// defining module.
llvm::Constant*
IRGenModule::getAddrOfWitnessTablePattern(const NormalProtocolConformance *conf,
                                          ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forProtocolWitnessTablePattern(conf);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

/// Look up the address of a differentiability witness.
llvm::Constant *IRGenModule::getAddrOfDifferentiabilityWitness(
    const SILDifferentiabilityWitness *witness, ConstantInit definition) {
  auto entity = LinkEntity::forDifferentiabilityWitness(witness);
  return getAddrOfLLVMVariable(entity, definition, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfAssociatedTypeWitnessTableAccessFunction(
                                  const NormalProtocolConformance *conformance,
                                  const AssociatedConformance &association) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forAssociatedTypeWitnessTableAccessFunction(conformance,
                                                            association);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeWitnessTableAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfDefaultAssociatedConformanceAccessor(
                                         AssociatedConformance requirement) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forDefaultAssociatedConformanceAccessor(requirement);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeWitnessTableAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfContinuationPrototype(CanSILFunctionType fnType) {
  LinkEntity entity = LinkEntity::forCoroutineContinuationPrototype(fnType);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  auto signature = Signature::forCoroutineContinuation(*this, fnType);
  LinkInfo link = LinkInfo::get(*this, entity, NotForDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Should we be defining the given helper function?
static llvm::Function *shouldDefineHelper(IRGenModule &IGM,
                                          llvm::Constant *fn,
                                          bool setIsNoInline) {
  auto *def = dyn_cast<llvm::Function>(fn);
  if (!def) return nullptr;
  if (!def->empty()) return nullptr;

  def->setAttributes(IGM.constructInitialAttributes());
  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(def);
  def->setDoesNotThrow();
  def->setCallingConv(IGM.DefaultCC);
  if (setIsNoInline)
    def->addFnAttr(llvm::Attribute::NoInline);
  return def;
}

/// Get or create a helper function with the given name and type, lazily
/// using the given generation function to fill in its body.
///
/// The helper function will be shared between translation units within the
/// current linkage unit, so choose the name carefully to ensure that it
/// does not collide with any other helper function.  In general, it should
/// be a Swift-specific C reserved name; that is, it should start with
//  "__swift".
llvm::Constant *
IRGenModule::getOrCreateHelperFunction(StringRef fnName, llvm::Type *resultTy,
                                       ArrayRef<llvm::Type*> paramTys,
                        llvm::function_ref<void(IRGenFunction &IGF)> generate,
                        bool setIsNoInline,
                        bool forPrologue) {
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(resultTy, paramTys, false);

  llvm::Constant *fn =
      cast<llvm::Constant>(
          Module.getOrInsertFunction(fnName, fnTy).getCallee());

  if (llvm::Function *def = shouldDefineHelper(*this, fn, setIsNoInline)) {
    IRGenFunction IGF(*this, def);
    if (DebugInfo && !forPrologue)
      DebugInfo->emitArtificialFunction(IGF, def);
    generate(IGF);
  }

  return fn;
}

void IRGenModule::setColocateTypeDescriptorSection(llvm::GlobalVariable *v) {
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    if (IRGen.Opts.ColocateTypeDescriptors)
      v->setSection("__TEXT,__constg_swiftt");
    else
      setTrueConstGlobal(v);
    break;
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
  case llvm::Triple::Wasm:
  case llvm::Triple::ELF:
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    setTrueConstGlobal(v);
    break;
  }

}
void IRGenModule::setColocateMetadataSection(llvm::Function *f) {
  if (!IRGen.Opts.CollocatedMetadataFunctions)
    return;

  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    f->setSection("__TEXT, __textg_swiftm, regular, pure_instructions");
    break;
  case llvm::Triple::DXContainer:
  case llvm::Triple::GOFF:
  case llvm::Triple::SPIRV:
  case llvm::Triple::UnknownObjectFormat:
  case llvm::Triple::Wasm:
  case llvm::Triple::ELF:
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    break;
  }
}
