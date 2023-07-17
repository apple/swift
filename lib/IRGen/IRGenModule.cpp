//===--- IRGenModule.cpp - Swift Global LLVM IR Generation ----------------===//
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
//  This file implements IR generation for global declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Availability.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/Linking.h"
#include "swift/Runtime/RuntimeFnWrappersGen.h"
#include "swift/Runtime/Config.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/CodeGen/SwiftCallingConv.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Basic/CodeGenOptions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MD5.h"

#include "Callee.h"
#include "ConformanceDescription.h"
#include "GenDecl.h"
#include "GenEnum.h"
#include "GenMeta.h"
#include "GenPointerAuth.h"
#include "GenIntegerLiteral.h"
#include "GenType.h"
#include "IRGenModule.h"
#include "IRGenDebugInfo.h"
#include "ProtocolInfo.h"
#include "StructLayout.h"

#include <initializer_list>

using namespace swift;
using namespace irgen;
using llvm::Attribute;

const unsigned DefaultAS = 0;

/// A helper for creating LLVM struct types.
static llvm::StructType *createStructType(IRGenModule &IGM,
                                          StringRef name,
                                  std::initializer_list<llvm::Type*> types,
                                          bool packed = false) {
  return llvm::StructType::create(IGM.getLLVMContext(),
                                  ArrayRef<llvm::Type*>(types.begin(),
                                                        types.size()),
                                  name, packed);
}

static clang::CodeGenerator *createClangCodeGenerator(ASTContext &Context,
                                                      llvm::LLVMContext &LLVMContext,
                                                      const IRGenOptions &Opts,
                                                      StringRef ModuleName,
                                                      StringRef PD) {
  auto Loader = Context.getClangModuleLoader();
  auto *Importer = static_cast<ClangImporter*>(&*Loader);
  assert(Importer && "No clang module loader!");
  auto &ClangContext = Importer->getClangASTContext();

  auto &CGO = Importer->getCodeGenOpts();
  if (CGO.OpaquePointers) {
    LLVMContext.setOpaquePointers(true);
  } else {
    LLVMContext.setOpaquePointers(false);
  }

  CGO.OptimizationLevel = Opts.shouldOptimize() ? 3 : 0;

  CGO.DebugTypeExtRefs = !Opts.DisableClangModuleSkeletonCUs;
  CGO.DiscardValueNames = !Opts.shouldProvideValueNames();
  switch (Opts.DebugInfoLevel) {
  case IRGenDebugInfoLevel::None:
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::NoDebugInfo);
    break;
  case IRGenDebugInfoLevel::LineTables:
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::DebugLineTablesOnly);
    break;
  case IRGenDebugInfoLevel::ASTTypes:
  case IRGenDebugInfoLevel::DwarfTypes:
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::FullDebugInfo);
    break;
  }
  switch (Opts.DebugInfoFormat) {
  case IRGenDebugInfoFormat::None:
    break;
  case IRGenDebugInfoFormat::DWARF:
    CGO.DebugCompilationDir = Opts.DebugCompilationDir;
    CGO.DwarfVersion = Opts.DWARFVersion;
    CGO.DwarfDebugFlags =
        Opts.getDebugFlags(PD, Context.LangOpts.EnableCXXInterop);
    break;
  case IRGenDebugInfoFormat::CodeView:
    CGO.EmitCodeView = true;
    CGO.DebugCompilationDir = Opts.DebugCompilationDir;
    // This actually contains the debug flags for codeview.
    CGO.DwarfDebugFlags =
        Opts.getDebugFlags(PD, Context.LangOpts.EnableCXXInterop);
    break;
  }
  if (!Opts.TrapFuncName.empty()) {
    CGO.TrapFuncName = Opts.TrapFuncName;
  }

  // We don't need to perform coverage mapping for any Clang decls we've
  // synthesized, as they have no user-written code. This is also needed to
  // avoid a Clang crash when attempting to emit coverage for decls without
  // source locations (rdar://100172217).
  CGO.CoverageMapping = false;

  auto &VFS = Importer->getClangInstance().getVirtualFileSystem();
  auto &HSI = Importer->getClangPreprocessor()
                  .getHeaderSearchInfo()
                  .getHeaderSearchOpts();
  auto &PPO = Importer->getClangPreprocessor().getPreprocessorOpts();
  auto *ClangCodeGen = clang::CreateLLVMCodeGen(ClangContext.getDiagnostics(),
                                                ModuleName, &VFS, HSI, PPO, CGO,
                                                LLVMContext);
  ClangCodeGen->Initialize(ClangContext);
  return ClangCodeGen;
}

#ifndef NDEBUG
static ValueDecl *lookupSimple(ModuleDecl *module, ArrayRef<StringRef> declPath) {
  DeclContext *dc = module;
  for (;; declPath = declPath.drop_front()) {
    SmallVector<ValueDecl*, 1> results;
    module->lookupMember(results, dc, module->getASTContext().getIdentifier(declPath.front()), Identifier());
    if (results.size() != 1) return nullptr;
    if (declPath.size() == 1) return results.front();
    dc = dyn_cast<DeclContext>(results.front());
    if (!dc) return nullptr;
  }
}

static void checkPointerAuthWitnessDiscriminator(IRGenModule &IGM, ArrayRef<StringRef> declPath, uint16_t expected) {
  auto &schema = IGM.getOptions().PointerAuth.ProtocolWitnesses;
  if (!schema.isEnabled()) return;

  auto decl = lookupSimple(IGM.getSwiftModule(), declPath);
  assert(decl && "decl not found");
  auto discriminator = PointerAuthInfo::getOtherDiscriminator(IGM, schema, SILDeclRef(decl));
  assert(discriminator->getZExtValue() == expected && "discriminator value doesn't match");
}

static void checkPointerAuthAssociatedTypeDiscriminator(IRGenModule &IGM, ArrayRef<StringRef> declPath, uint16_t expected) {
  auto &schema = IGM.getOptions().PointerAuth.ProtocolAssociatedTypeAccessFunctions;
  if (!schema.isEnabled()) return;

  auto decl = dyn_cast_or_null<AssociatedTypeDecl>(lookupSimple(IGM.getSwiftModule(), declPath));
  assert(decl && "decl not found");
  auto discriminator = PointerAuthInfo::getOtherDiscriminator(IGM, schema, AssociatedType(decl));
  assert(discriminator->getZExtValue() == expected && "discriminator value doesn't match");
}

static void sanityCheckStdlib(IRGenModule &IGM) {
  if (!IGM.getSwiftModule()->isStdlibModule()) return;

  // Only run the sanity check when we're building the real stdlib.
  if (!lookupSimple(IGM.getSwiftModule(), { "String" })) return;

  checkPointerAuthAssociatedTypeDiscriminator(IGM, { "_ObjectiveCBridgeable", "_ObjectiveCType" }, SpecialPointerAuthDiscriminators::ObjectiveCTypeDiscriminator);
  checkPointerAuthWitnessDiscriminator(IGM, { "_ObjectiveCBridgeable", "_bridgeToObjectiveC" }, SpecialPointerAuthDiscriminators::bridgeToObjectiveCDiscriminator);
  checkPointerAuthWitnessDiscriminator(IGM, { "_ObjectiveCBridgeable", "_forceBridgeFromObjectiveC" }, SpecialPointerAuthDiscriminators::forceBridgeFromObjectiveCDiscriminator);
  checkPointerAuthWitnessDiscriminator(IGM, { "_ObjectiveCBridgeable", "_conditionallyBridgeFromObjectiveC" }, SpecialPointerAuthDiscriminators::conditionallyBridgeFromObjectiveCDiscriminator);
}
#endif

IRGenModule::IRGenModule(IRGenerator &irgen,
                         std::unique_ptr<llvm::TargetMachine> &&target,
                         SourceFile *SF, StringRef ModuleName,
                         StringRef OutputFilename,
                         StringRef MainInputFilenameForDebugInfo,
                         StringRef PrivateDiscriminator)
    : LLVMContext(new llvm::LLVMContext()), IRGen(irgen),
      Context(irgen.SIL.getASTContext()),
      // The LLVMContext (and the IGM itself) will get deleted by the IGMDeleter
      // as long as the IGM is registered with the IRGenerator.
      ClangCodeGen(createClangCodeGenerator(Context, *LLVMContext, irgen.Opts,
                                            ModuleName, PrivateDiscriminator)),
      Module(*ClangCodeGen->GetModule()),
      DataLayout(irgen.getClangDataLayoutString()),
      Triple(irgen.getEffectiveClangTriple()), TargetMachine(std::move(target)),
      silConv(irgen.SIL), OutputFilename(OutputFilename),
      MainInputFilenameForDebugInfo(MainInputFilenameForDebugInfo),
      TargetInfo(SwiftTargetInfo::get(*this)), DebugInfo(nullptr),
      ModuleHash(nullptr), ObjCInterop(Context.LangOpts.EnableObjCInterop),
      UseDarwinPreStableABIBit(Context.LangOpts.UseDarwinPreStableABIBit),
      Types(*new TypeConverter(*this)) {
  irgen.addGenModule(SF, this);

  auto &opts = irgen.Opts;

  EnableValueNames = opts.shouldProvideValueNames();
  
  VoidTy = llvm::Type::getVoidTy(getLLVMContext());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMContext());
  Int8Ty = llvm::Type::getInt8Ty(getLLVMContext());
  Int16Ty = llvm::Type::getInt16Ty(getLLVMContext());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMContext());
  Int32PtrTy = Int32Ty->getPointerTo();
  Int64Ty = llvm::Type::getInt64Ty(getLLVMContext());
  Int8PtrTy = llvm::Type::getInt8PtrTy(getLLVMContext());
  Int8PtrPtrTy = Int8PtrTy->getPointerTo(0);
  SizeTy = DataLayout.getIntPtrType(getLLVMContext(), /*addrspace*/ 0);

  // For the relative address type, we want to use the int32 bit type
  // on most architectures, e.g. x86_64, because it produces valid
  // fixups/relocations. The exception is 16-bit architectures,
  // so we shorten the relative address type there.
  if (SizeTy->getBitWidth()<32) {
    RelativeAddressTy = SizeTy;
  } else {
    RelativeAddressTy = Int32Ty;
  }

  RelativeAddressPtrTy = RelativeAddressTy->getPointerTo();

  FloatTy = llvm::Type::getFloatTy(getLLVMContext());
  DoubleTy = llvm::Type::getDoubleTy(getLLVMContext());

  auto CI = static_cast<ClangImporter*>(&*Context.getClangModuleLoader());
  assert(CI && "no clang module loader");
  auto &clangASTContext = CI->getClangASTContext();

  ObjCBoolTy = Int1Ty;
  if (clangASTContext.getTargetInfo().useSignedCharForObjCBool())
    ObjCBoolTy = Int8Ty;

  RefCountedStructTy =
    llvm::StructType::create(getLLVMContext(), "swift.refcounted");
  RefCountedPtrTy = RefCountedStructTy->getPointerTo(/*addrspace*/ 0);
  RefCountedNull = llvm::ConstantPointerNull::get(RefCountedPtrTy);

  // For now, references storage types are just pointers.
#define CHECKED_REF_STORAGE(Name, name, ...)                                   \
  Name##ReferenceStructTy =                                                    \
      createStructType(*this, "swift." #name, {RefCountedPtrTy});              \
  Name##ReferencePtrTy = Name##ReferenceStructTy->getPointerTo(0);
#include "swift/AST/ReferenceStorage.def"

  // A type metadata record is the structure pointed to by the canonical
  // address point of a type metadata.  This is at least one word, and
  // potentially more than that, past the start of the actual global
  // structure.
  TypeMetadataStructTy = createStructType(*this, "swift.type", {
    MetadataKindTy          // MetadataKind Kind;
  });
  TypeMetadataPtrTy = TypeMetadataStructTy->getPointerTo(DefaultAS);
  TypeMetadataPtrPtrTy = TypeMetadataPtrTy->getPointerTo(DefaultAS);

  TypeMetadataResponseTy = createStructType(*this, "swift.metadata_response", {
    TypeMetadataPtrTy,
    SizeTy
  });

  OffsetPairTy = llvm::StructType::get(getLLVMContext(), { SizeTy, SizeTy });

  // The TypeLayout structure, including all possible trailing components.
  FullTypeLayoutTy = createStructType(*this, "swift.full_type_layout", {
    SizeTy, // size
    SizeTy, // flags
    SizeTy, // alignment
    SizeTy  // extra inhabitant flags (optional)
  });

  TypeLayoutTy = createStructType(*this, "swift.type_layout", {
    SizeTy, // size
    SizeTy, // stride
    Int32Ty, // flags
    Int32Ty // extra inhabitant count
  });

  // A protocol descriptor describes a protocol. It is not type metadata in
  // and of itself, but is referenced in the structure of existential type
  // metadata records.
  ProtocolDescriptorStructTy = createStructType(*this, "swift.protocol", {
    Int8PtrTy,              // objc isa
    Int8PtrTy,              // name
    Int8PtrTy,              // inherited protocols
    Int8PtrTy,              // required objc instance methods
    Int8PtrTy,              // required objc class methods
    Int8PtrTy,              // optional objc instance methods
    Int8PtrTy,              // optional objc class methods
    Int8PtrTy,              // objc properties
    Int32Ty,                // size
    Int32Ty,                // flags
    Int32Ty,                // total requirement count
    Int32Ty,                // requirements array
    RelativeAddressTy,      // superclass
    RelativeAddressTy       // associated type names
  });
  
  ProtocolDescriptorPtrTy = ProtocolDescriptorStructTy->getPointerTo();

  ProtocolRequirementStructTy =
      createStructType(*this, "swift.protocol_requirement", {
    Int32Ty,                // flags
    RelativeAddressTy,      // default implementation
  });
  
  // A tuple type metadata record has a couple extra fields.
  auto tupleElementTy = createStructType(*this, "swift.tuple_element_type", {
    TypeMetadataPtrTy,      // Metadata *Type;
    Int32Ty                 // int32_t Offset;
  });
  TupleTypeMetadataTy = createStructType(
      *this, "swift.tuple_type",
      {
          TypeMetadataStructTy,                   // (base)
          SizeTy,                                 // size_t NumElements;
          Int8PtrTy,                              // const char *Labels;
          llvm::ArrayType::get(tupleElementTy, 0) // Element Elements[];
      });
  TupleTypeMetadataPtrTy = TupleTypeMetadataTy->getPointerTo();
  // A full type metadata record is basically just an adjustment to the
  // address point of a type metadata.  Resilience may cause
  // additional data to be laid out prior to this address point.
  static_assert(MetadataAdjustmentIndex::ValueType == 2,
                "Adjustment index must be synchronized with this layout");
  FullTypeMetadataStructTy = createStructType(*this, "swift.full_type", {
    Int8PtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullTypeMetadataPtrTy = FullTypeMetadataStructTy->getPointerTo(DefaultAS);

  FullForeignTypeMetadataStructTy = createStructType(*this, "swift.full_foreign_type", {
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });

  DeallocatingDtorTy = llvm::FunctionType::get(VoidTy, RefCountedPtrTy, false);
  llvm::Type *dtorPtrTy = DeallocatingDtorTy->getPointerTo();

  FullExistentialTypeMetadataStructTy = createStructType(*this, "swift.full_existential_type", {
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });

  // A full heap metadata is basically just an additional small prefix
  // on a full metadata, used for metadata corresponding to heap
  // allocations.
  static_assert(MetadataAdjustmentIndex::Class == 3,
                "Adjustment index must be synchronized with this layout");
  FullHeapMetadataStructTy =
                  createStructType(*this, "swift.full_heapmetadata", {
    Int8PtrTy,
    dtorPtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullHeapMetadataPtrTy = FullHeapMetadataStructTy->getPointerTo(DefaultAS);

  // A full box metadata is non-type heap metadata for a heap allocation of a
  // single value. The box tracks the offset to the value inside the box.
  FullBoxMetadataStructTy =
                  createStructType(*this, "swift.full_boxmetadata", {
    dtorPtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy,
    Int32Ty,
    CaptureDescriptorPtrTy,
  });
  FullBoxMetadataPtrTy = FullBoxMetadataStructTy->getPointerTo(DefaultAS);

  // This must match struct HeapObject in the runtime.
  llvm::Type *refCountedElts[] = {TypeMetadataPtrTy, IntPtrTy};
  RefCountedStructTy->setBody(refCountedElts);
  RefCountedStructSize =
    Size(DataLayout.getStructLayout(RefCountedStructTy)->getSizeInBytes());

  PtrSize = Size(DataLayout.getPointerSize(DefaultAS));

  FunctionPairTy = createStructType(*this, "swift.function", {
    FunctionPtrTy,
    RefCountedPtrTy,
  });

  OpaqueTy = llvm::StructType::create(getLLVMContext(), "swift.opaque");
  OpaquePtrTy = OpaqueTy->getPointerTo(DefaultAS);
  NoEscapeFunctionPairTy = createStructType(*this, "swift.noescape.function", {
    FunctionPtrTy,
    OpaquePtrTy,
  });

  ProtocolRecordTy =
    createStructType(*this, "swift.protocolref", {
      RelativeAddressTy
    });
  ProtocolRecordPtrTy = ProtocolRecordTy->getPointerTo();

  ProtocolConformanceDescriptorTy
    = createStructType(*this, "swift.protocol_conformance_descriptor", {
      RelativeAddressTy,
      RelativeAddressTy,
      RelativeAddressTy,
      Int32Ty
    });
  ProtocolConformanceDescriptorPtrTy
    = ProtocolConformanceDescriptorTy->getPointerTo(DefaultAS);

  TypeContextDescriptorTy
    = llvm::StructType::create(getLLVMContext(), "swift.type_descriptor");
  TypeContextDescriptorPtrTy
    = TypeContextDescriptorTy->getPointerTo(DefaultAS);

  ClassContextDescriptorTy =
        llvm::StructType::get(getLLVMContext(), {
    Int32Ty, // context flags
    Int32Ty, // parent
    Int32Ty, // name
    Int32Ty, // kind
    Int32Ty, // accessor function
    Int32Ty, // num fields
    Int32Ty, // field offset vector
    Int32Ty, // is_reflectable flag
    Int32Ty, // (Generics Descriptor) argument offset
    Int32Ty, // (Generics Descriptor) num params
    Int32Ty, // (Generics Descriptor) num requirements
    Int32Ty, // (Generics Descriptor) num key arguments
    Int32Ty, // (Generics Descriptor) num extra arguments
    Int32Ty, // (VTable Descriptor) offset
    Int32Ty, // (VTable Descriptor) size
    Int32Ty, // (Methods Descriptor) accessor
    Int32Ty, // (Methods Descriptor) flags
  }, /*packed=*/true);

  MethodDescriptorStructTy
    = createStructType(*this, "swift.method_descriptor", {
      Int32Ty,
      RelativeAddressTy,
    });

  MethodOverrideDescriptorStructTy
    = createStructType(*this, "swift.method_override_descriptor", {
      RelativeAddressTy,
      RelativeAddressTy,
      RelativeAddressTy
    });

  TypeMetadataRecordTy
    = createStructType(*this, "swift.type_metadata_record", {
      RelativeAddressTy
    });
  TypeMetadataRecordPtrTy
    = TypeMetadataRecordTy->getPointerTo(DefaultAS);

  FieldDescriptorTy
    = llvm::StructType::create(getLLVMContext(), "swift.field_descriptor");
  FieldDescriptorPtrTy = FieldDescriptorTy->getPointerTo(DefaultAS);
  FieldDescriptorPtrPtrTy = FieldDescriptorPtrTy->getPointerTo(DefaultAS);

  FixedBufferTy = nullptr;
  for (unsigned i = 0; i != MaxNumValueWitnesses; ++i)
    ValueWitnessTys[i] = nullptr;

  ObjCPtrTy = llvm::StructType::create(getLLVMContext(), "objc_object")
                ->getPointerTo(DefaultAS);
  BridgeObjectPtrTy = llvm::StructType::create(getLLVMContext(), "swift.bridge")
                ->getPointerTo(DefaultAS);

  ObjCClassStructTy = llvm::StructType::create(getLLVMContext(), "objc_class");
  ObjCClassPtrTy = ObjCClassStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcClassElts[] = {
    ObjCClassPtrTy,
    ObjCClassPtrTy,
    OpaquePtrTy,
    OpaquePtrTy,
    IntPtrTy
  };
  ObjCClassStructTy->setBody(objcClassElts);

  ObjCSuperStructTy = llvm::StructType::create(getLLVMContext(), "objc_super");
  ObjCSuperPtrTy = ObjCSuperStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcSuperElts[] = {
    ObjCPtrTy,
    ObjCClassPtrTy
  };
  ObjCSuperStructTy->setBody(objcSuperElts);
  
  ObjCBlockStructTy = llvm::StructType::create(getLLVMContext(), "objc_block");
  ObjCBlockPtrTy = ObjCBlockStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcBlockElts[] = {
    ObjCClassPtrTy, // isa
    Int32Ty,        // flags
    Int32Ty,        // reserved
    FunctionPtrTy,  // invoke function pointer
    Int8PtrTy,      // TODO: block descriptor pointer.
                    // We will probably need a struct type for that at some
                    // point too.
  };
  ObjCBlockStructTy->setBody(objcBlockElts);

  // Class _Nullable callback(Class _Nonnull cls, void * _Nullable arg);
  llvm::Type *params[] = { ObjCClassPtrTy, Int8PtrTy };
  ObjCUpdateCallbackTy = llvm::FunctionType::get(ObjCClassPtrTy, params, false);

  // The full class stub structure, including a word before the address point.
  ObjCFullResilientClassStubTy = createStructType(*this, "objc_full_class_stub", {
    SizeTy, // zero padding to appease the linker
    SizeTy, // isa pointer -- always 1
    ObjCUpdateCallbackTy->getPointerTo() // the update callback
  });

  // What we actually export.
  ObjCResilientClassStubTy = createStructType(*this, "objc_class_stub", {
    SizeTy, // isa pointer -- always 1
    ObjCUpdateCallbackTy->getPointerTo() // the update callback
  });

  auto ErrorStructTy = llvm::StructType::create(getLLVMContext(), "swift.error");
  // ErrorStruct is currently opaque to the compiler.
  ErrorPtrTy = ErrorStructTy->getPointerTo(DefaultAS);
  
  llvm::Type *openedErrorTriple[] = {
    OpaquePtrTy,
    TypeMetadataPtrTy,
    WitnessTablePtrTy,
  };
  OpenedErrorTripleTy = llvm::StructType::get(getLLVMContext(),
                                              openedErrorTriple,
                                              /*packed*/ false);
  OpenedErrorTriplePtrTy = OpenedErrorTripleTy->getPointerTo(DefaultAS);

  WitnessTablePtrPtrTy = WitnessTablePtrTy->getPointerTo(DefaultAS);
  
  // todo
  OpaqueTypeDescriptorTy = TypeContextDescriptorTy;
  OpaqueTypeDescriptorPtrTy = OpaqueTypeDescriptorTy->getPointerTo();

  InvariantMetadataID = getLLVMContext().getMDKindID("invariant.load");
  InvariantNode = llvm::MDNode::get(getLLVMContext(), {});
  DereferenceableID = getLLVMContext().getMDKindID("dereferenceable");
  
  C_CC = getOptions().PlatformCCallingConvention;
  // TODO: use "tinycc" on platforms that support it
  DefaultCC = SWIFT_DEFAULT_LLVM_CC;
  SwiftCC = llvm::CallingConv::Swift;

  bool isAsyncCCSupported =
    clangASTContext.getTargetInfo().checkCallingConvention(clang::CC_SwiftAsync)
    == clang::TargetInfo::CCCR_OK;
  if (isAsyncCCSupported) {
    SwiftAsyncCC = llvm::CallingConv::SwiftTail;
    AsyncTailCallKind = llvm::CallInst::TCK_MustTail;
  } else {
    SwiftAsyncCC = SwiftCC;
    AsyncTailCallKind = llvm::CallInst::TCK_Tail;
  }

  if (opts.DebugInfoLevel > IRGenDebugInfoLevel::None)
    DebugInfo = IRGenDebugInfo::createIRGenDebugInfo(IRGen.Opts, *CI, *this,
                                                     Module,
                                                 MainInputFilenameForDebugInfo,
                                                     PrivateDiscriminator);

  if (auto loader = Context.getClangModuleLoader()) {
    ClangASTContext =
        &static_cast<ClangImporter *>(loader)->getClangASTContext();
  }

  if (ClangASTContext) {
    auto atomicBoolTy = ClangASTContext->getAtomicType(ClangASTContext->BoolTy);
    AtomicBoolSize = Size(ClangASTContext->getTypeSize(atomicBoolTy));
    AtomicBoolAlign = Alignment(ClangASTContext->getTypeSize(atomicBoolTy));
  }
  // On WebAssembly, tail optional arguments are not allowed because Wasm requires
  // callee and caller signature to be the same. So LLVM adds dummy arguments for
  // `swiftself` and `swifterror`. If there is `swiftself` but is no `swifterror` in
  // a swiftcc function or invocation, then LLVM adds dummy `swifterror` parameter or
  // argument. To count up how many dummy arguments should be added, we need to mark
  // it as `swifterror` even though it's not in register.
  ShouldUseSwiftError =
    clang::CodeGen::swiftcall::isSwiftErrorLoweredInRegister(
      ClangCodeGen->CGM()) || TargetInfo.OutputObjectFormat == llvm::Triple::Wasm;

#ifndef NDEBUG
  sanityCheckStdlib(*this);
#endif

  DynamicReplacementsTy =
      llvm::StructType::get(getLLVMContext(), {Int8PtrPtrTy, Int8PtrTy});
  DynamicReplacementsPtrTy = DynamicReplacementsTy->getPointerTo(DefaultAS);

  DynamicReplacementLinkEntryTy =
      llvm::StructType::create(getLLVMContext(), "swift.dyn_repl_link_entry");
  DynamicReplacementLinkEntryPtrTy =
      DynamicReplacementLinkEntryTy->getPointerTo(DefaultAS);
  llvm::Type *linkEntryFields[] = {
    Int8PtrTy, // function pointer.
    DynamicReplacementLinkEntryPtrTy // next.
  };
  DynamicReplacementLinkEntryTy->setBody(linkEntryFields);

  DynamicReplacementKeyTy = createStructType(*this, "swift.dyn_repl_key",
                                             {RelativeAddressTy, Int32Ty});

  AccessibleFunctionRecordTy =
      createStructType(*this, "swift.accessible_function",
                       {RelativeAddressTy, RelativeAddressTy, RelativeAddressTy,
                        RelativeAddressTy, Int32Ty});

  RuntimeDiscoverableAttributeTy =
    createStructType(*this, "swift.runtime_attr",
                     {Int32Ty, RelativeAddressTy, Int32Ty});

  AsyncFunctionPointerTy = createStructType(*this, "swift.async_func_pointer",
                                            {RelativeAddressTy, Int32Ty}, true);
  SwiftContextTy = llvm::StructType::create(getLLVMContext(), "swift.context");
  SwiftContextPtrTy = SwiftContextTy->getPointerTo(DefaultAS);

  // This must match the definition of class AsyncTask in swift/ABI/Task.h.
  SwiftTaskTy = createStructType(*this, "swift.task", {
    RefCountedStructTy,   // object header
    Int8PtrTy, Int8PtrTy, // Job.SchedulerPrivate
    Int32Ty,              // Job.Flags
    Int32Ty,              // Job.ID
    Int8PtrTy, Int8PtrTy, // Reserved
    FunctionPtrTy,        // Job.RunJob/Job.ResumeTask
    SwiftContextPtrTy,    // Task.ResumeContext
    IntPtrTy              // Task.Status
  });

  AsyncFunctionPointerPtrTy = AsyncFunctionPointerTy->getPointerTo(DefaultAS);
  SwiftTaskPtrTy = SwiftTaskTy->getPointerTo(DefaultAS);
  SwiftAsyncLetPtrTy = Int8PtrTy; // we pass it opaquely (AsyncLet*)
  SwiftTaskOptionRecordPtrTy = SizeTy; // Builtin.RawPointer? that we get as (TaskOptionRecord*)
  SwiftTaskGroupPtrTy = Int8PtrTy; // we pass it opaquely (TaskGroup*)
  SwiftTaskOptionRecordTy = createStructType(*this, "swift.task_option", {
    SizeTy,                     // Flags
    SwiftTaskOptionRecordPtrTy, // Parent
  });
  SwiftTaskGroupTaskOptionRecordTy = createStructType(
      *this, "swift.task_group_task_option", {
    SwiftTaskOptionRecordTy,    // Base option record
    SwiftTaskGroupPtrTy,        // Task group
  });
  ExecutorFirstTy = SizeTy;
  ExecutorSecondTy = SizeTy;
  SwiftExecutorTy = createStructType(*this, "swift.executor", {
    ExecutorFirstTy,      // identity
    ExecutorSecondTy,     // implementation
  });
  SwiftJobTy = createStructType(*this, "swift.job", {
    RefCountedStructTy,   // object header
    Int8PtrTy, Int8PtrTy, // SchedulerPrivate
    Int32Ty,              // flags
    Int32Ty,              // ID
    Int8PtrTy, Int8PtrTy, // Reserved
    FunctionPtrTy,        // RunJob/ResumeTask
  });
  SwiftJobPtrTy = SwiftJobTy->getPointerTo(DefaultAS);

  // using TaskContinuationFunction =
  //   SWIFT_CC(swift) void (SWIFT_ASYNC_CONTEXT AsyncContext *);
  TaskContinuationFunctionTy = llvm::FunctionType::get(
      VoidTy, {SwiftContextPtrTy}, /*isVarArg*/ false);
  TaskContinuationFunctionPtrTy = TaskContinuationFunctionTy->getPointerTo();

  SwiftContextTy->setBody({
    SwiftContextPtrTy,             // Parent
    TaskContinuationFunctionPtrTy, // ResumeParent
  });

  AsyncTaskAndContextTy = createStructType(
      *this, "swift.async_task_and_context",
      { SwiftTaskPtrTy, SwiftContextPtrTy });

  if (Context.LangOpts.isConcurrencyModelTaskToThread()) {
    ContinuationAsyncContextTy = createStructType(
        *this, "swift.continuation_context",
        {SwiftContextTy,       // AsyncContext header
         SizeTy,               // flags
         SizeTy,               // await synchronization
         ErrorPtrTy,           // error result pointer
         OpaquePtrTy,          // normal result address
         SwiftExecutorTy,      // resume to executor
         SizeTy                // pointer to condition variable
         });
  } else {
    ContinuationAsyncContextTy = createStructType(
        *this, "swift.continuation_context",
        {SwiftContextTy,       // AsyncContext header
         SizeTy,               // flags
         SizeTy,               // await synchronization
         ErrorPtrTy,           // error result pointer
         OpaquePtrTy,          // normal result address
         SwiftExecutorTy       // resume to executor
         });
  }
  ContinuationAsyncContextPtrTy =
    ContinuationAsyncContextTy->getPointerTo(DefaultAS);

  ClassMetadataBaseOffsetTy = llvm::StructType::get(
      getLLVMContext(), {
                            SizeTy,  // Immediate members offset
                            Int32Ty, // Negative size in words
                            Int32Ty  // Positive size in words
                        });

  DifferentiabilityWitnessTy = createStructType(
      *this, "swift.differentiability_witness", {Int8PtrTy, Int8PtrTy});
}

IRGenModule::~IRGenModule() {
  destroyMetadataLayoutMap();
  destroyPointerAuthCaches();
  delete &Types;
}

// Explicitly listing these constants is an unfortunate compromise for
// making the database file much more compact.
//
// They have to be non-local because otherwise we'll get warnings when
// a particular x-macro expansion doesn't use one.
namespace RuntimeConstants {
  const auto ReadNone = llvm::Attribute::ReadNone;
  const auto ReadOnly = llvm::Attribute::ReadOnly;
  const auto ArgMemOnly = llvm::Attribute::ArgMemOnly;
  const auto NoReturn = llvm::Attribute::NoReturn;
  const auto NoUnwind = llvm::Attribute::NoUnwind;
  const auto ZExt = llvm::Attribute::ZExt;
  const auto FirstParamReturned = llvm::Attribute::Returned;
  const auto WillReturn = llvm::Attribute::WillReturn;

#ifdef CHECK_RUNTIME_EFFECT_ANALYSIS
  const auto NoEffect = RuntimeEffect::NoEffect;
  const auto Locking = RuntimeEffect::Locking;
  const auto Allocating = RuntimeEffect::Allocating;
  const auto Deallocating = RuntimeEffect::Deallocating;
  const auto RefCounting = RuntimeEffect::RefCounting;
  const auto ObjectiveC = RuntimeEffect::ObjectiveC;
  const auto Concurrency = RuntimeEffect::Concurrency;
  const auto AutoDiff = RuntimeEffect::AutoDiff;
  const auto MetaData = RuntimeEffect::MetaData;
  const auto Casting = RuntimeEffect::Casting;
  const auto ExclusivityChecking = RuntimeEffect::ExclusivityChecking;
#endif

  RuntimeAvailability AlwaysAvailable(ASTContext &Context) {
    return RuntimeAvailability::AlwaysAvailable;
  }

  bool
  isDeploymentAvailabilityContainedIn(ASTContext &Context,
                                      AvailabilityContext featureAvailability) {
    auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(Context);
    return deploymentAvailability.isContainedIn(featureAvailability);
  }

  RuntimeAvailability OpaqueTypeAvailability(ASTContext &Context) {
    auto featureAvailability = Context.getOpaqueTypeAvailability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  GetTypesInAbstractMetadataStateAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getTypesInAbstractMetadataStateAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability DynamicReplacementAvailability(ASTContext &Context) {
    auto featureAvailability = Context.getSwift51Availability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::AvailableByCompatibilityLibrary;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  CompareTypeContextDescriptorsAvailability(ASTContext &Context) {
    auto featureAvailability =
        Context.getCompareTypeContextDescriptorsAvailability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  CompareProtocolConformanceDescriptorsAvailability(ASTContext &Context) {
    auto featureAvailability =
        Context.getCompareProtocolConformanceDescriptorsAvailability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  GetCanonicalSpecializedMetadataAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getIntermodulePrespecializedGenericMetadataAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  GetCanonicalPrespecializedGenericMetadataAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getPrespecializedGenericMetadataAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability ConcurrencyAvailability(ASTContext &context) {
    auto featureAvailability = context.getConcurrencyAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability DifferentiationAvailability(ASTContext &context) {
    auto featureAvailability = context.getDifferentiationAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  MultiPayloadEnumTagSinglePayloadAvailability(ASTContext &context) {
    auto featureAvailability = context.getMultiPayloadEnumTagSinglePayload();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  ObjCIsUniquelyReferencedAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getObjCIsUniquelyReferencedAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability SignedConformsToProtocolAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getSignedConformsToProtocolAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability SignedDescriptorAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getSignedDescriptorAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability TaskRunInlineAvailability(ASTContext &context) {
    if (context.LangOpts.isConcurrencyModelTaskToThread()) {
      return RuntimeAvailability::AlwaysAvailable;
    }
    // swift_task_run_inline is only available under task-to-thread execution
    // model.
    return RuntimeAvailability::ConditionallyAvailable;
  }

} // namespace RuntimeConstants

// We don't use enough attributes to justify generalizing the
// RuntimeFunctions.def FUNCTION macro. Instead, special case the one attribute
// associated with the return type not the function type.
static bool isReturnAttribute(llvm::Attribute::AttrKind Attr) {
  return Attr == llvm::Attribute::ZExt;
}
// Similar to the 'return' attribute we assume that the 'returned' attributed is
// associated with the first function parameter.
static bool isReturnedAttribute(llvm::Attribute::AttrKind Attr) {
  return Attr == llvm::Attribute::Returned;
}

namespace {
bool isStandardLibrary(const llvm::Module &M) {
  if (auto *Flags = M.getNamedMetadata("swift.module.flags")) {
    for (const auto *F : Flags->operands()) {
      const auto *Key = dyn_cast_or_null<llvm::MDString>(F->getOperand(0));
      if (!Key)
        continue;

      const auto *Value =
          dyn_cast_or_null<llvm::ConstantAsMetadata>(F->getOperand(1));
      if (!Value)
        continue;

      if (Key->getString() == "standard-library")
        return cast<llvm::ConstantInt>(Value->getValue())->isOne();
    }
  }
  return false;
}
}

bool IRGenModule::isStandardLibrary() const {
  return ::isStandardLibrary(Module);
}

llvm::FunctionType *swift::getRuntimeFnType(llvm::Module &Module,
                                           llvm::ArrayRef<llvm::Type*> retTypes,
                                           llvm::ArrayRef<llvm::Type*> argTypes) {
  llvm::Type *retTy;
  if (retTypes.size() == 1)
    retTy = *retTypes.begin();
  else
    retTy = llvm::StructType::get(Module.getContext(),
                                  {retTypes.begin(), retTypes.end()},
                                  /*packed*/ false);
  return llvm::FunctionType::get(retTy, {argTypes.begin(), argTypes.end()},
                                 /*isVararg*/ false);
}

llvm::Constant *swift::getRuntimeFn(llvm::Module &Module,
                      llvm::Constant *&cache,
                      const char *name,
                      llvm::CallingConv::ID cc,
                      RuntimeAvailability availability,
                      llvm::ArrayRef<llvm::Type*> retTypes,
                      llvm::ArrayRef<llvm::Type*> argTypes,
                      ArrayRef<Attribute::AttrKind> attrs,
                      IRGenModule *IGM) {

  if (cache)
    return cache;

  bool isWeakLinked = false;
  std::string functionName(name);

  switch (availability) {
  case RuntimeAvailability::AlwaysAvailable:
    // Nothing to do.
    break;
  case RuntimeAvailability::ConditionallyAvailable: {
    isWeakLinked = true;
    break;
  }
  case RuntimeAvailability::AvailableByCompatibilityLibrary: {
    functionName.append("50");
    break;
  }
  }

  llvm::Type *retTy;
  if (retTypes.size() == 1)
    retTy = *retTypes.begin();
  else
    retTy = llvm::StructType::get(Module.getContext(),
                                  {retTypes.begin(), retTypes.end()},
                                  /*packed*/ false);
  auto fnTy = llvm::FunctionType::get(retTy,
                                      {argTypes.begin(), argTypes.end()},
                                      /*isVararg*/ false);

  auto addr = Module.getOrInsertFunction(functionName.c_str(), fnTy).getCallee();
  auto fnptr = addr;
  // Strip off any bitcast we might have due to this function being declared of
  // a different type previously.
  if (auto bitcast = dyn_cast<llvm::BitCastInst>(fnptr))
    fnptr = cast<llvm::Constant>(bitcast->getOperand(0));
  cache = cast<llvm::Constant>(addr);

  // Add any function attributes and set the calling convention.
  if (auto fn = dyn_cast<llvm::Function>(fnptr)) {
    fn->setCallingConv(cc);

    bool IsExternal =
        fn->getLinkage() == llvm::GlobalValue::AvailableExternallyLinkage ||
        (fn->getLinkage() == llvm::GlobalValue::ExternalLinkage &&
         fn->isDeclaration());

    if (!isStandardLibrary(Module) && IsExternal &&
        ::useDllStorage(llvm::Triple(Module.getTargetTriple())))
      fn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    
    if (IsExternal && isWeakLinked
        && !::useDllStorage(llvm::Triple(Module.getTargetTriple())))
      fn->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);

    llvm::AttrBuilder buildFnAttr(Module.getContext());
    llvm::AttrBuilder buildRetAttr(Module.getContext());
    llvm::AttrBuilder buildFirstParamAttr(Module.getContext());

    for (auto Attr : attrs) {
      if (isReturnAttribute(Attr))
        buildRetAttr.addAttribute(Attr);
      else if (isReturnedAttribute(Attr))
        buildFirstParamAttr.addAttribute(Attr);
      else
        buildFnAttr.addAttribute(Attr);
    }
    fn->addFnAttrs(buildFnAttr);
    fn->addRetAttrs(buildRetAttr);
    fn->addParamAttrs(0, buildFirstParamAttr);

    // Add swiftself and swifterror attributes only when swift_willThrow
    // swift_willThrow is defined in RuntimeFunctions.def, but due to the
    // DSL limitation, arguments attributes are not set.
    // On the other hand, caller of `swift_willThrow` assumes that it's attributed
    // with `swiftself` and `swifterror`.
    // This mismatch of attributes would be issue when lowering to WebAssembly.
    // While lowering, LLVM counts how many dummy params are necessary to match
    // callee and caller signature. So we need to add them correctly.
    if (functionName == "swift_willThrow") {
      assert(IGM && "IGM is required for swift_willThrow.");
      fn->addParamAttr(0, Attribute::AttrKind::SwiftSelf);
      if (IGM->ShouldUseSwiftError) {
        fn->addParamAttr(1, Attribute::AttrKind::SwiftError);
      }
    }
  }

  return cache;
}

llvm::Constant *IRGenModule::getDeletedAsyncMethodErrorAsyncFunctionPointer() {
  return getAddrOfLLVMVariableOrGOTEquivalent(
      LinkEntity::forKnownAsyncFunctionPointer("swift_deletedAsyncMethodError")).getValue();
}

static bool isReturnAttribute(llvm::Attribute::AttrKind Attr);
static bool isReturnedAttribute(llvm::Attribute::AttrKind Attr);

#ifdef CHECK_RUNTIME_EFFECT_ANALYSIS
void IRGenModule::registerRuntimeEffect(ArrayRef<RuntimeEffect> effect,
                                         const char *funcName) {
  for (RuntimeEffect rt : effect) {
    effectOfRuntimeFuncs = RuntimeEffect((unsigned)effectOfRuntimeFuncs |
                                         (unsigned)rt);
    emittedRuntimeFuncs.push_back(funcName);
  }
}
#else
#define registerRuntimeEffect(...)
#endif

#define QUOTE(...) __VA_ARGS__
#define STR(X)     #X

#define FUNCTION(ID, NAME, CC, AVAILABILITY, RETURNS, ARGS, ATTRS, EFFECT) \
  FUNCTION_IMPL(ID, NAME, CC, AVAILABILITY, QUOTE(RETURNS), QUOTE(ARGS), \
                QUOTE(ATTRS), QUOTE(EFFECT))

#define RETURNS(...) { __VA_ARGS__ }
#define ARGS(...) { __VA_ARGS__ }
#define NO_ARGS {}
#define ATTRS(...) { __VA_ARGS__ }
#define NO_ATTRS {}
#define EFFECT(...) { __VA_ARGS__ }

#define FUNCTION_IMPL(ID, NAME, CC, AVAILABILITY, RETURNS, ARGS, ATTRS,        \
                      EFFECT)                                                  \
  llvm::Constant *IRGenModule::get##ID##Fn() {                                 \
    using namespace RuntimeConstants;                                          \
    registerRuntimeEffect(EFFECT, #NAME);                                      \
    return getRuntimeFn(Module, ID##Fn, #NAME, CC,                             \
                        AVAILABILITY(this->Context), RETURNS, ARGS, ATTRS,     \
                        this);                                                 \
  }                                                                            \
  FunctionPointer IRGenModule::get##ID##FunctionPointer() {                    \
    using namespace RuntimeConstants;                                          \
    auto fn = get##ID##Fn();                                                   \
    auto fnTy = get##ID##FnType();                                             \
    llvm::AttributeList attrs;                                                 \
    SmallVector<llvm::Attribute::AttrKind, 8> theAttrs(ATTRS);                 \
    for (auto Attr : theAttrs) {                                               \
      if (isReturnAttribute(Attr))                                             \
        attrs = attrs.addRetAttribute(getLLVMContext(), Attr);                 \
      else if (isReturnedAttribute(Attr))                                      \
        attrs = attrs.addParamAttribute(getLLVMContext(), 0, Attr);            \
      else                                                                     \
        attrs = attrs.addFnAttribute(getLLVMContext(), Attr);                  \
    }                                                                          \
    auto sig = Signature(fnTy, attrs, CC);                                     \
    return FunctionPointer::forDirect(FunctionPointer::Kind::Function, fn,     \
                                      nullptr, sig);                           \
  }                                                                            \
  llvm::FunctionType *IRGenModule::get##ID##FnType() {                         \
    return getRuntimeFnType(Module, RETURNS, ARGS);                            \
  }

#include "swift/Runtime/RuntimeFunctions.def"

std::pair<llvm::GlobalVariable *, llvm::Constant *>
IRGenModule::createStringConstant(StringRef Str, bool willBeRelativelyAddressed,
                                  StringRef sectionName, StringRef name) {
  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(getLLVMContext(), Str);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init, name);
  // FIXME: ld64 crashes resolving relative references to coalesceable symbols.
  // rdar://problem/22674524
  // If we intend to relatively address this string, don't mark it with
  // unnamed_addr to prevent it from going into the cstrings section and getting
  // coalesced.
  if (!willBeRelativelyAddressed)
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  if (!sectionName.empty())
    global->setSection(sectionName);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
    global->getValueType(), global, indices);

  return { global, address };
}

#define KNOWN_METADATA_ACCESSOR(NAME, SYM)                                     \
  llvm::Constant *IRGenModule::get##NAME() {                                   \
    if (NAME)                                                                  \
      return NAME;                                                             \
    NAME = Module.getOrInsertGlobal(SYM, FullExistentialTypeMetadataStructTy);            \
    if (useDllStorage() && !isStandardLibrary())                               \
      ApplyIRLinkage(IRLinkage::ExternalImport)                                \
          .to(cast<llvm::GlobalVariable>(NAME));                               \
    return NAME;                                                               \
  }

KNOWN_METADATA_ACCESSOR(EmptyTupleMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(EMPTY_TUPLE_MANGLING)))
KNOWN_METADATA_ACCESSOR(AnyExistentialMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(ANY_MANGLING)))
KNOWN_METADATA_ACCESSOR(AnyObjectExistentialMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(ANYOBJECT_MANGLING)))

#undef KNOWN_METADATA_ACCESSOR

llvm::Constant *IRGenModule::getObjCEmptyCachePtr() {
  if (ObjCEmptyCachePtr)
    return ObjCEmptyCachePtr;

  if (ObjCInterop) {
    // struct objc_cache _objc_empty_cache;
    ObjCEmptyCachePtr = Module.getOrInsertGlobal("_objc_empty_cache", OpaqueTy);
    ApplyIRLinkage(IRLinkage::ExternalImport)
        .to(cast<llvm::GlobalVariable>(ObjCEmptyCachePtr));
  } else {
    // FIXME: Remove even the null value per rdar://problem/18801263
    ObjCEmptyCachePtr = llvm::ConstantPointerNull::get(OpaquePtrTy);
  }
  return ObjCEmptyCachePtr;
}

llvm::Constant *IRGenModule::getObjCEmptyVTablePtr() {
  // IMP _objc_empty_vtable;

  // On recent Darwin platforms, this symbol is defined at
  // runtime as an absolute symbol with the value of null. Older ObjCs
  // didn't guarantee _objc_empty_vtable to be nil, but Swift doesn't
  // deploy far enough back for that to be a concern.

  // FIXME: When !ObjCInterop, we should remove even the null value per
  // rdar://problem/18801263

  if (!ObjCEmptyVTablePtr)
    ObjCEmptyVTablePtr = llvm::ConstantPointerNull::get(OpaquePtrTy);

  return ObjCEmptyVTablePtr;
}

Address IRGenModule::getAddrOfObjCISAMask() {
  // This symbol is only exported by the runtime if the platform uses
  // isa masking.
  assert(TargetInfo.hasISAMasking());
  if (!ObjCISAMaskPtr) {
    ObjCISAMaskPtr = Module.getOrInsertGlobal("swift_isaMask", IntPtrTy);
    ApplyIRLinkage(IRLinkage::ExternalImport)
        .to(cast<llvm::GlobalVariable>(ObjCISAMaskPtr));
  }
  return Address(ObjCISAMaskPtr, IntPtrTy, getPointerAlignment());
}

llvm::Constant *
IRGenModule::getAddrOfAccessibleFunctionRecord(SILFunction *accessibleFn) {
  auto entity = LinkEntity::forAccessibleFunctionRecord(accessibleFn);
  return getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo());
}

ModuleDecl *IRGenModule::getSwiftModule() const {
  return IRGen.SIL.getSwiftModule();
}

AvailabilityContext IRGenModule::getAvailabilityContext() const {
  return AvailabilityContext::forDeploymentTarget(Context);
}

Lowering::TypeConverter &IRGenModule::getSILTypes() const {
  return IRGen.SIL.Types;
}

clang::CodeGen::CodeGenModule &IRGenModule::getClangCGM() const {
  return ClangCodeGen->CGM();
}

llvm::Module *IRGenModule::getModule() const {
  return ClangCodeGen->GetModule();
}

GeneratedModule IRGenModule::intoGeneratedModule() && {
  return GeneratedModule{
    std::move(LLVMContext),
    std::unique_ptr<llvm::Module>{ClangCodeGen->ReleaseModule()},
    std::move(TargetMachine)
  };
}

bool IRGenerator::canEmitWitnessTableLazily(SILWitnessTable *wt) {
  if (Opts.UseJIT)
    return false;

  // Regardless of the access level, if the witness table is shared it means
  // we can safely not emit it. Every other module which needs it will generate
  // its own shared copy of it.
  if (wt->getLinkage() == SILLinkage::Shared)
    return true;

  NominalTypeDecl *ConformingTy =
    wt->getConformingType()->getNominalOrBoundGenericNominal();

  switch (ConformingTy->getEffectiveAccess()) {
    case AccessLevel::Private:
    case AccessLevel::FilePrivate:
      return true;

    case AccessLevel::Internal:
      return PrimaryIGM->getSILModule().isWholeModule();

    default:
      return false;
  }
  llvm_unreachable("switch does not handle all cases");
}

void IRGenerator::addLazyWitnessTable(const ProtocolConformance *Conf) {
  if (auto *wt = SIL.lookUpWitnessTable(Conf)) {
    // Add it to the queue if it hasn't already been put there.
    if (canEmitWitnessTableLazily(wt) &&
        LazilyEmittedWitnessTables.insert(wt).second) {
      assert(!FinishedEmittingLazyDefinitions);
      LazyWitnessTables.push_back(wt);
    }
  }
}

void IRGenerator::addClassForEagerInitialization(ClassDecl *ClassDecl) {
  if (!ClassDecl->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
    return;

  assert(!ClassDecl->isGenericContext());
  assert(!ClassDecl->hasClangNode());

  ClassesForEagerInitialization.push_back(ClassDecl);
}

void IRGenerator::addBackDeployedObjCActorInitialization(ClassDecl *ClassDecl) {
  if (!ClassDecl->isActor())
    return;

  if (!ClassDecl->isObjC())
    return;

  // If we are not back-deploying concurrency, there's nothing to do.
  ASTContext &ctx = ClassDecl->getASTContext();
  auto deploymentAvailability = AvailabilityContext::forDeploymentTarget(ctx);
  if (deploymentAvailability.isContainedIn(ctx.getConcurrencyAvailability()))
    return;

  ObjCActorsNeedingSuperclassSwizzle.push_back(ClassDecl);
}

llvm::AttributeList IRGenModule::getAllocAttrs() {
  if (AllocAttrs.isEmpty()) {
    AllocAttrs = llvm::AttributeList().addRetAttribute(
        getLLVMContext(), llvm::Attribute::NoAlias);
    AllocAttrs =
        AllocAttrs.addFnAttribute(getLLVMContext(), llvm::Attribute::NoUnwind);
  }
  return AllocAttrs;
}

/// Disable thumb-mode until debugger support is there.
bool swift::irgen::shouldRemoveTargetFeature(StringRef feature) {
  return feature == "+thumb-mode";
}

void IRGenModule::setHasNoFramePointer(llvm::AttrBuilder &Attrs) {
  Attrs.addAttribute("frame-pointer", "non-leaf");
}

void IRGenModule::setHasNoFramePointer(llvm::Function *F) {
  llvm::AttrBuilder b(getLLVMContext());
  setHasNoFramePointer(b);
  F->addFnAttrs(b);
}

/// Construct initial function attributes from options.
void IRGenModule::constructInitialFnAttributes(
    llvm::AttrBuilder &Attrs, OptimizationMode FuncOptMode,
    StackProtectorMode stackProtector) {
  // Add the default attributes for the Clang configuration.
  clang::CodeGen::addDefaultFunctionDefinitionAttributes(getClangCGM(), Attrs);

  // Add/remove MinSize based on the appropriate setting.
  if (FuncOptMode == OptimizationMode::NotSet)
    FuncOptMode = IRGen.Opts.OptMode;
  if (FuncOptMode == OptimizationMode::ForSize) {
    Attrs.addAttribute(llvm::Attribute::OptimizeForSize);
    Attrs.addAttribute(llvm::Attribute::MinSize);
  } else {
    Attrs.removeAttribute(llvm::Attribute::MinSize);
    Attrs.removeAttribute(llvm::Attribute::OptimizeForSize);
  }
  if (stackProtector == StackProtectorMode::StackProtector) {
    Attrs.addAttribute(llvm::Attribute::StackProtectReq);
    Attrs.addAttribute("stack-protector-buffer-size", llvm::utostr(8));
  }
}

llvm::AttributeList IRGenModule::constructInitialAttributes() {
  llvm::AttrBuilder b(getLLVMContext());
  constructInitialFnAttributes(b);
  return llvm::AttributeList().addFnAttributes(getLLVMContext(), b);
}

llvm::ConstantInt *IRGenModule::getInt32(uint32_t value) {
  return llvm::ConstantInt::get(Int32Ty, value);
}

llvm::ConstantInt *IRGenModule::getSize(Size size) {
  return llvm::ConstantInt::get(SizeTy, size.getValue());
}

llvm::Constant *IRGenModule::getOpaquePtr(llvm::Constant *ptr) {
  return llvm::ConstantExpr::getBitCast(ptr, Int8PtrTy);
}

static void appendEncodedName(raw_ostream &os, StringRef name) {
  if (clang::isValidAsciiIdentifier(name)) {
    os << "_" << name;
  } else {
    for (auto c : name)
      os.write_hex(static_cast<uint8_t>(c));
  }
}

static void appendEncodedName(llvm::SmallVectorImpl<char> &buf,
                              StringRef name) {
  llvm::raw_svector_ostream os{buf};
  appendEncodedName(os, name);
}

StringRef
swift::irgen::encodeForceLoadSymbolName(llvm::SmallVectorImpl<char> &buf,
                                        StringRef name) {
  llvm::raw_svector_ostream os{buf};
  os << "_swift_FORCE_LOAD_$";
  appendEncodedName(os, name);
  return os.str();
}

llvm::SmallString<32> getTargetDependentLibraryOption(const llvm::Triple &T,
                                                      StringRef library) {
  llvm::SmallString<32> buffer;

  if (T.isWindowsMSVCEnvironment() || T.isWindowsItaniumEnvironment()) {
    bool quote = library.contains(' ');

    buffer += "/DEFAULTLIB:";
    if (quote)
      buffer += '"';
    buffer += library;
    if (!library.endswith_insensitive(".lib"))
      buffer += ".lib";
    if (quote)
      buffer += '"';
  } else if (T.isPS4()) {
    bool quote = library.contains(' ');

    buffer += "\01";
    if (quote)
      buffer += '"';
    buffer += library;
    if (quote)
      buffer += '"';
  } else {
    buffer += "-l";
    buffer += library;
  }

  return buffer;
}

void IRGenModule::addLinkLibrary(const LinkLibrary &linkLib) {
  // The debugger gets the autolink information directly from
  // the LinkLibraries of the module, so there's no reason to
  // emit it into the IR of debugger expressions.
  if (Context.LangOpts.DebuggerSupport)
    return;
  
  switch (linkLib.getKind()) {
  case LibraryKind::Library: {
    AutolinkEntries.emplace_back(linkLib);
    break;
  }
  case LibraryKind::Framework: {
    // If we're supposed to disable autolinking of this framework, bail out.
    auto &frameworks = IRGen.Opts.DisableAutolinkFrameworks;
    if (std::find(frameworks.begin(), frameworks.end(), linkLib.getName())
          != frameworks.end())
      return;
    AutolinkEntries.emplace_back(linkLib);
    break;
  }
  }

  if (linkLib.shouldForceLoad()) {
    llvm::SmallString<64> buf;
    encodeForceLoadSymbolName(buf, linkLib.getName());
    auto ForceImportThunk = cast<llvm::Function>(
        Module.getOrInsertFunction(buf, llvm::FunctionType::get(VoidTy, false))
            .getCallee());

    const IRLinkage IRL =
        llvm::Triple(Module.getTargetTriple()).isOSBinFormatCOFF()
            ? IRLinkage::ExternalImport
            : IRLinkage::ExternalWeakImport;
    ApplyIRLinkage(IRL).to(cast<llvm::GlobalValue>(ForceImportThunk));

    buf += "_$";
    appendEncodedName(buf, IRGen.Opts.ModuleName);

    if (!Module.getGlobalVariable(buf.str())) {
      auto ref = new llvm::GlobalVariable(Module, ForceImportThunk->getType(),
                                          /*isConstant=*/true,
                                          llvm::GlobalValue::WeakODRLinkage,
                                          ForceImportThunk, buf.str());
      ApplyIRLinkage(IRLinkage::InternalWeakODR).to(ref);
      auto casted = llvm::ConstantExpr::getBitCast(ref, Int8PtrTy);
      LLVMUsed.push_back(casted);
    }
  }
}

static bool replaceModuleFlagsEntry(llvm::LLVMContext &Ctx,
                                    llvm::Module &Module, StringRef EntryName,
                                    llvm::Module::ModFlagBehavior Behavior,
                                    llvm::Metadata *Val) {
  auto *ModuleFlags = Module.getModuleFlagsMetadata();

  for (unsigned I = 0, E = ModuleFlags->getNumOperands(); I != E; ++I) {
    llvm::MDNode *Op = ModuleFlags->getOperand(I);
    llvm::MDString *ID = cast<llvm::MDString>(Op->getOperand(1));

    if (ID->getString().equals(EntryName)) {

      // Create the new entry.
      llvm::Type *Int32Ty = llvm::Type::getInt32Ty(Ctx);
      llvm::Metadata *Ops[3] = {llvm::ConstantAsMetadata::get(
                                    llvm::ConstantInt::get(Int32Ty, Behavior)),
                                llvm::MDString::get(Ctx, EntryName), Val};

      ModuleFlags->setOperand(I, llvm::MDNode::get(Ctx, Ops));
      return true;
    }
  }
  llvm_unreachable("Could not replace old linker options entry?");
}

static bool
doesTargetAutolinkUsingAutolinkExtract(const SwiftTargetInfo &TargetInfo,
                                       const llvm::Triple &Triple) {
  if (TargetInfo.OutputObjectFormat == llvm::Triple::ELF && !Triple.isPS4())
    return true;

  if (TargetInfo.OutputObjectFormat == llvm::Triple::Wasm)
    return true;

  if (Triple.isOSCygMing())
    return true;

  return false;
}

namespace {

struct AutolinkKind {
  enum ValueTy {
    LLVMLinkerOptions,
    LLVMDependentLibraries,
    SwiftAutoLinkExtract,
  };

  ValueTy Value;

  AutolinkKind(ValueTy value) : Value(value) {}
  AutolinkKind(const AutolinkKind &kind) : Value(kind.Value) {}

  StringRef getSectionNameMetadata();

  template <typename Vector, typename Set>
  void collectEntriesFromLibraries(llvm::SetVector<llvm::MDNode *, Vector, Set> &Entries,
                                   ArrayRef<LinkLibrary> AutolinkEntries,
                                   IRGenModule &IGM);

  template <typename Vector, typename Set>
  void writeEntries(llvm::SetVector<llvm::MDNode *, Vector, Set> Entries,
                    llvm::NamedMDNode *Metadata, IRGenModule &IGM);

  static AutolinkKind create(const SwiftTargetInfo &TargetInfo,
                             llvm::Triple Triple, IRGenLLVMLTOKind LLVMLTOKind);
};

} // anonymous namespace

StringRef AutolinkKind::getSectionNameMetadata() {
  // FIXME: This constant should be vended by LLVM somewhere.
  switch (Value) {
  case AutolinkKind::LLVMDependentLibraries:
    return "llvm.dependent-libraries";
  case AutolinkKind::LLVMLinkerOptions:
  case AutolinkKind::SwiftAutoLinkExtract:
    return "llvm.linker.options";
  }

  llvm_unreachable("Unhandled AutolinkKind in switch.");
}

template <typename Vector, typename Set>
void AutolinkKind::collectEntriesFromLibraries(
    llvm::SetVector<llvm::MDNode *, Vector, Set> &Entries,
    ArrayRef<LinkLibrary> AutolinkEntries, IRGenModule &IGM) {
  llvm::LLVMContext &ctx = IGM.getLLVMContext();

  switch (Value) {
  case AutolinkKind::LLVMLinkerOptions: {
    // On platforms that support autolinking, continue to use the metadata.
    for (LinkLibrary linkLib : AutolinkEntries) {
      switch (linkLib.getKind()) {
      case LibraryKind::Library: {
        llvm::SmallString<32> opt =
            getTargetDependentLibraryOption(IGM.Triple, linkLib.getName());
        Entries.insert(llvm::MDNode::get(ctx, llvm::MDString::get(ctx, opt)));
        continue;
      }
      case LibraryKind::Framework: {
        llvm::Metadata *args[] = {llvm::MDString::get(ctx, "-framework"),
                                  llvm::MDString::get(ctx, linkLib.getName())};
        Entries.insert(llvm::MDNode::get(ctx, args));
        continue;
      }
      }
      llvm_unreachable("Unhandled LibraryKind in switch.");
    }
    return;
  }
  case AutolinkKind::SwiftAutoLinkExtract: {
    // On platforms that support autolinking, continue to use the metadata.
    for (LinkLibrary linkLib : AutolinkEntries) {
      switch (linkLib.getKind()) {
      case LibraryKind::Library: {
        llvm::SmallString<32> opt =
            getTargetDependentLibraryOption(IGM.Triple, linkLib.getName());
        Entries.insert(llvm::MDNode::get(ctx, llvm::MDString::get(ctx, opt)));
        continue;
      }
      case LibraryKind::Framework:
        // Frameworks are not supported with Swift Autolink Extract.
        continue;
      }
      llvm_unreachable("Unhandled LibraryKind in switch.");
    }
    return;
  }
  case AutolinkKind::LLVMDependentLibraries: {
    for (LinkLibrary linkLib : AutolinkEntries) {
      switch (linkLib.getKind()) {
      case LibraryKind::Library: {
        Entries.insert(llvm::MDNode::get(
            ctx, llvm::MDString::get(ctx, linkLib.getName())));
        continue;
      }
      case LibraryKind::Framework: {
        llvm_unreachable(
            "llvm.dependent-libraries doesn't support framework dependency");
      }
      }
      llvm_unreachable("Unhandled LibraryKind in switch.");
    }
    return;
  }
  }
  llvm_unreachable("Unhandled AutolinkKind in switch.");
}

template <typename Vector, typename Set>
void AutolinkKind::writeEntries(llvm::SetVector<llvm::MDNode *, Vector, Set> Entries,
                                llvm::NamedMDNode *Metadata, IRGenModule &IGM) {
  switch (Value) {
  case AutolinkKind::LLVMLinkerOptions:
  case AutolinkKind::LLVMDependentLibraries: {
    // On platforms that support autolinking, continue to use the metadata.
    Metadata->clearOperands();
    for (auto *Entry : Entries)
      Metadata->addOperand(Entry);
    return;
  }
  case AutolinkKind::SwiftAutoLinkExtract: {
    // Merge the entries into null-separated string.
    llvm::SmallString<64> EntriesString;
    for (auto EntryNode : Entries) {
      const auto *MD = cast<llvm::MDNode>(EntryNode);
      for (auto &Entry : MD->operands()) {
        const llvm::MDString *MS = cast<llvm::MDString>(Entry);
        EntriesString += MS->getString();
        EntriesString += '\0';
      }
    }
    auto EntriesConstant = llvm::ConstantDataArray::getString(
        IGM.getLLVMContext(), EntriesString, /*AddNull=*/false);
    auto var =
        new llvm::GlobalVariable(*IGM.getModule(), EntriesConstant->getType(),
                                 true, llvm::GlobalValue::PrivateLinkage,
                                 EntriesConstant, "_swift1_autolink_entries");
    var->setSection(".swift1_autolink_entries");
    var->setAlignment(llvm::MaybeAlign(IGM.getPointerAlignment().getValue()));

    disableAddressSanitizer(IGM, var);
    IGM.addUsedGlobal(var);
    return;
  }
  }
  llvm_unreachable("Unhandled AutolinkKind in switch.");
}

AutolinkKind AutolinkKind::create(const SwiftTargetInfo &TargetInfo,
                                  llvm::Triple Triple,
                                  IRGenLLVMLTOKind LLVMLTOKind) {
  // When performing LTO, we always use lld that supports auto linking
  // mechanism with ELF. So embed dependent libraries names in
  // "llvm.dependent-libraries" instead of "llvm.linker.options".
  if (TargetInfo.OutputObjectFormat == llvm::Triple::ELF &&
      LLVMLTOKind != IRGenLLVMLTOKind::None) {
    return AutolinkKind::LLVMDependentLibraries;
  }

  if (doesTargetAutolinkUsingAutolinkExtract(TargetInfo, Triple)) {
    return AutolinkKind::SwiftAutoLinkExtract;
  }

  return AutolinkKind::LLVMLinkerOptions;
}

static llvm::GlobalObject *createForceImportThunk(IRGenModule &IGM) {
  llvm::SmallString<64> buf;
  encodeForceLoadSymbolName(buf, IGM.IRGen.Opts.ForceLoadSymbolName);
  if (IGM.Triple.isOSBinFormatMachO()) {
    // On Mach-O targets, emit a common force-load symbol that resolves to
    // a global variable so it will be coalesced at static link time into a
    // single external symbol.
    //
    // Looks like C's tentative definitions are good for something after all.
    auto ForceImportThunk =
        new llvm::GlobalVariable(IGM.Module,
                                 IGM.Int1Ty,
                                 /*isConstant=*/false,
                                 llvm::GlobalValue::CommonLinkage,
                                 llvm::Constant::getNullValue(IGM.Int1Ty),
                                 buf.str());
    ApplyIRLinkage(IRLinkage::ExternalCommon).to(ForceImportThunk);
    IGM.addUsedGlobal(ForceImportThunk);
    return ForceImportThunk;
  } else {
    // On all other targets, emit an external symbol that resolves to a
    // function definition. On Windows, linkonce_odr is basically a no-op and
    // the COMDAT we set by applying linkage gives us the desired coalescing
    // behavior.
    auto ForceImportThunk =
        llvm::Function::Create(llvm::FunctionType::get(IGM.VoidTy, false),
                               llvm::GlobalValue::LinkOnceODRLinkage, buf,
                               &IGM.Module);
    ForceImportThunk->setAttributes(IGM.constructInitialAttributes());
    ApplyIRLinkage(IGM.IRGen.Opts.InternalizeSymbols
                      ? IRLinkage::Internal
                      : IRLinkage::ExternalExport).to(ForceImportThunk);
    if (IGM.Triple.supportsCOMDAT())
      if (auto *GO = cast<llvm::GlobalObject>(ForceImportThunk))
        GO->setComdat(IGM.Module.getOrInsertComdat(ForceImportThunk->getName()));

    auto BB = llvm::BasicBlock::Create(IGM.getLLVMContext(), "", ForceImportThunk);
    llvm::IRBuilder<> IRB(BB);
    IRB.CreateRetVoid();
    IGM.addUsedGlobal(ForceImportThunk);
    return ForceImportThunk;
  }
}

void IRGenModule::emitAutolinkInfo() {
  auto Autolink =
      AutolinkKind::create(TargetInfo, Triple, IRGen.Opts.LLVMLTOKind);

  StringRef AutolinkSectionName = Autolink.getSectionNameMetadata();

  auto *Metadata = Module.getOrInsertNamedMetadata(AutolinkSectionName);
  llvm::SmallSetVector<llvm::MDNode *, 4> Entries;

  // Collect the linker options already in the module (from ClangCodeGen).
  for (auto Entry : Metadata->operands()) {
    Entries.insert(Entry);
  }

  Autolink.collectEntriesFromLibraries(Entries, AutolinkEntries, *this);

  Autolink.writeEntries(Entries, Metadata, *this);

  if (!IRGen.Opts.ForceLoadSymbolName.empty()) {
    (void) createForceImportThunk(*this);
  }
}

void emitSwiftVersionNumberIntoModule(llvm::Module *Module) {
  // LLVM's object-file emission collects a fixed set of keys for the
  // image info.
  // Using "Objective-C Garbage Collection" as the key here is a hack,
  // but LLVM's object-file emission isn't general enough to collect
  // arbitrary keys to put in the image info.

  const char *ObjectiveCGarbageCollection = "Objective-C Garbage Collection";
  uint8_t Major, Minor;
  std::tie(Major, Minor) = version::getSwiftNumericVersion();
  uint32_t Value =
      (Major << 24) | (Minor << 16) | (IRGenModule::swiftVersion << 8);
  auto &llvmContext = Module->getContext();
  if (Module->getModuleFlag(ObjectiveCGarbageCollection)) {
    bool FoundOldEntry = replaceModuleFlagsEntry(
        llvmContext, *Module, ObjectiveCGarbageCollection,
        llvm::Module::Override,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(llvmContext), Value)));

    (void)FoundOldEntry;
    assert(FoundOldEntry && "Could not replace old module flag entry?");
  } else
    Module->addModuleFlag(llvm::Module::Override, ObjectiveCGarbageCollection,
                          Value);
}

void IRGenModule::cleanupClangCodeGenMetadata() {
  // Remove llvm.ident that ClangCodeGen might have left in the module.
  auto *LLVMIdent = Module.getNamedMetadata("llvm.ident");
  if (LLVMIdent)
    Module.eraseNamedMetadata(LLVMIdent);
  emitSwiftVersionNumberIntoModule(&Module);
}

bool IRGenModule::finalize() {
  const char *ModuleHashVarName = "llvm.swift_module_hash";
  if (IRGen.Opts.OutputKind == IRGenOutputKind::ObjectFile &&
      !Module.getGlobalVariable(ModuleHashVarName)) {
    // Create a global variable into which we will store the hash of the
    // module (used for incremental compilation).
    // We have to create the variable now (before we emit the global lists).
    // But we want to calculate the hash later because later we can do it
    // multi-threaded.
    llvm::MD5::MD5Result zero{};
    ArrayRef<uint8_t> ZeroArr(reinterpret_cast<uint8_t *>(&zero), sizeof(zero));
    auto *ZeroConst = llvm::ConstantDataArray::get(Module.getContext(), ZeroArr);
    ModuleHash = new llvm::GlobalVariable(Module, ZeroConst->getType(), true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          ZeroConst, ModuleHashVarName);
    switch (TargetInfo.OutputObjectFormat) {
    case llvm::Triple::MachO:
      // On Darwin the linker ignores the __LLVM segment.
      ModuleHash->setSection("__LLVM,__swift_modhash");
      break;
    case llvm::Triple::ELF:
    case llvm::Triple::Wasm:
      ModuleHash->setSection(".swift_modhash");
      break;
    case llvm::Triple::COFF:
      ModuleHash->setSection(".sw5hash");
      break;
    default:
      llvm_unreachable("Don't know how to emit the module hash for the selected"
                       "object format.");
    }
    addUsedGlobal(ModuleHash);
  }
  emitLazyPrivateDefinitions();

  // Finalize clang IR-generation.
  finalizeClangCodeGen();

  // If that failed, report failure up and skip the final clean-up.
  if (!ClangCodeGen->GetModule())
    return false;

  emitSwiftAsyncExtendedFrameInfoWeakRef();
  emitAutolinkInfo();
  emitGlobalLists();
  emitUsedConditionals();
  if (DebugInfo)
    DebugInfo->finalize();
  cleanupClangCodeGenMetadata();

  // Clean up DSOLocal & DLLImport attributes, they cannot be applied together.
  // The imported declarations are marked as DSO local by default.
  for (auto &GV : Module.globals())
    if (GV.hasDLLImportStorageClass())
      GV.setDSOLocal(false);

  for (auto &F : Module.functions())
    if (F.hasDLLImportStorageClass())
      F.setDSOLocal(false);

  return true;
}

/// Emit lazy definitions that have to be emitted in this specific
/// IRGenModule.
void IRGenModule::emitLazyPrivateDefinitions() {
  emitLazyObjCProtocolDefinitions();
}

llvm::MDNode *IRGenModule::createProfileWeights(uint64_t TrueCount,
                                                uint64_t FalseCount) const {
  uint64_t MaxWeight = std::max(TrueCount, FalseCount);
  uint64_t Scale = (MaxWeight > UINT32_MAX) ? UINT32_MAX : 1;
  uint32_t ScaledTrueCount = (TrueCount / Scale) + 1;
  uint32_t ScaledFalseCount = (FalseCount / Scale) + 1;
  llvm::MDBuilder MDHelper(getLLVMContext());
  return MDHelper.createBranchWeights(ScaledTrueCount, ScaledFalseCount);
}

void IRGenModule::unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
}

void IRGenModule::fatal_unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
  llvm::report_fatal_error(llvm::Twine("unimplemented IRGen feature! ") +
                             message);
}

void IRGenModule::error(SourceLoc loc, const Twine &message) {
  SmallVector<char, 128> buffer;
  Context.Diags.diagnose(loc, diag::irgen_failure,
                         message.toStringRef(buffer));
}

bool IRGenModule::useDllStorage() { return ::useDllStorage(Triple); }

bool IRGenModule::shouldPrespecializeGenericMetadata() {
  auto canPrespecializeTarget =
      (Triple.isOSDarwin() || Triple.isOSWindows() ||
       (Triple.isOSLinux() && !(Triple.isARM() && Triple.isArch32Bit())));
  if (canPrespecializeTarget && isStandardLibrary()) {
    return IRGen.Opts.PrespecializeGenericMetadata;
  }
  auto &context = getSwiftModule()->getASTContext();
  auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(context);
  return IRGen.Opts.PrespecializeGenericMetadata &&
         deploymentAvailability.isContainedIn(
             context.getPrespecializedGenericMetadataAvailability()) &&
         canPrespecializeTarget;
}

bool IRGenModule::canMakeStaticObjectsReadOnly() {
  // Unconditionally disable this until we can fix the metadata.
  // The trick of using the Empty array metadata for static arrays
  // breaks Obj-C interop quite badly.
  // rdar://101126543
  return false;

#if 0
  if (getOptions().DisableReadonlyStaticObjects)
    return false;

  // TODO: Support constant static arrays on other platforms, too.
  // See also the comment in GlobalObjects.cpp.
  if (!Triple.isOSDarwin())
    return false;

  return getAvailabilityContext().isContainedIn(
          Context.getImmortalRefCountSymbolsAvailability());
#endif
}

void IRGenerator::addGenModule(SourceFile *SF, IRGenModule *IGM) {
  assert(GenModules.count(SF) == 0);
  GenModules[SF] = IGM;
  if (!PrimaryIGM) {
    PrimaryIGM = IGM;
  }
  Queue.push_back(IGM);
}

IRGenModule *IRGenerator::getGenModule(SourceFile *SF) {
  // If we're emitting for a single module, or a single file, we always use the
  // primary IGM.
  if (GenModules.size() == 1)
    return getPrimaryIGM();

 IRGenModule *IGM = GenModules[SF];
 assert(IGM);
 return IGM;
}

IRGenModule *IRGenerator::getGenModule(DeclContext *ctxt) {
  if (GenModules.size() == 1 || !ctxt) {
    return getPrimaryIGM();
  }
  SourceFile *SF = ctxt->getOutermostParentSourceFile();
  if (!SF) {
    return getPrimaryIGM();
  }

  IRGenModule *IGM = GenModules[SF];
  assert(IGM);
  return IGM;
}

IRGenModule *IRGenerator::getGenModule(SILFunction *f) {
  if (GenModules.size() == 1) {
    return getPrimaryIGM();
  }

  auto found = DefaultIGMForFunction.find(f);
  if (found != DefaultIGMForFunction.end())
    return found->second;

  if (auto *dc = f->getDeclContext())
    return getGenModule(dc);

  return getPrimaryIGM();
}

uint32_t swift::irgen::getSwiftABIVersion() {
  return IRGenModule::swiftVersion;
}

llvm::Triple IRGenerator::getEffectiveClangTriple() {
  auto CI = static_cast<ClangImporter *>(
      &*SIL.getASTContext().getClangModuleLoader());
  assert(CI && "no clang module loader");
  return llvm::Triple(CI->getTargetInfo().getTargetOpts().Triple);
}

const llvm::StringRef IRGenerator::getClangDataLayoutString() {
  return static_cast<ClangImporter *>(
             SIL.getASTContext().getClangModuleLoader())
      ->getTargetInfo()
      .getDataLayoutString();
}

TypeExpansionContext IRGenModule::getMaximalTypeExpansionContext() const {
  return TypeExpansionContext::maximal(getSwiftModule(),
                                       getSILModule().isWholeModule());
}

const TypeLayoutEntry
&IRGenModule::getTypeLayoutEntry(SILType T, bool useStructLayouts) {
  return Types.getTypeLayoutEntry(T, useStructLayouts);
}


void IRGenModule::emitSwiftAsyncExtendedFrameInfoWeakRef() {
  if (!hasSwiftAsyncFunctionDef || extendedFramePointerFlagsWeakRef)
    return;
  if (IRGen.Opts.SwiftAsyncFramePointer !=
      SwiftAsyncFramePointerKind::Auto)
    return;
  if (isConcurrencyAvailable())
    return;

  // Emit a weak reference to the `swift_async_extendedFramePointerFlags` symbol
  // needed by Swift async functions.
  auto symbolName = "swift_async_extendedFramePointerFlags";
  if ((extendedFramePointerFlagsWeakRef = Module.getGlobalVariable(symbolName)))
    return;
  extendedFramePointerFlagsWeakRef = new llvm::GlobalVariable(Module, Int8PtrTy, false,
                                         llvm::GlobalValue::ExternalWeakLinkage, nullptr,
                                         symbolName);

  // The weak imported extendedFramePointerFlagsWeakRef gets optimized out
  // before being added back as a strong import.
  // Declarations can't be added to the used list, so we create a little
  // global that can't be used from the program, but can be in the used list to
  // avoid optimizations.
  llvm::GlobalVariable *usage = new llvm::GlobalVariable(
      Module, extendedFramePointerFlagsWeakRef->getType(), false,
      llvm::GlobalValue::LinkOnceODRLinkage,
      static_cast<llvm::GlobalVariable *>(extendedFramePointerFlagsWeakRef),
      "_swift_async_extendedFramePointerFlagsUser");
  usage->setVisibility(llvm::GlobalValue::HiddenVisibility);
  addUsedGlobal(usage);
}

bool IRGenModule::isConcurrencyAvailable() {
  auto &ctx = getSwiftModule()->getASTContext();
  auto deploymentAvailability =
    AvailabilityContext::forDeploymentTarget(ctx);
  return deploymentAvailability.isContainedIn(ctx.getConcurrencyAvailability());
}

/// Pretend the other files that drivers/build systems expect exist by
/// creating empty files. Used by UseSingleModuleLLVMEmission when
/// num-threads > 0.
bool swift::writeEmptyOutputFilesFor(
  const ASTContext &Context,
  std::vector<std::string>& ParallelOutputFilenames,
  const IRGenOptions &IRGenOpts) {
  for (auto fileName : ParallelOutputFilenames) {
    // The first output file, was use for genuine output.
    if (fileName == ParallelOutputFilenames[0])
      continue;

    std::unique_ptr<llvm::LLVMContext> llvmContext(new llvm::LLVMContext());
    std::unique_ptr<clang::CodeGenerator> clangCodeGen(
      createClangCodeGenerator(const_cast<ASTContext&>(Context),
                               *llvmContext, IRGenOpts, fileName, ""));
    auto *llvmModule = clangCodeGen->GetModule();

    auto *clangImporter = static_cast<ClangImporter *>(
      Context.getClangModuleLoader());
    llvmModule->setTargetTriple(
      clangImporter->getTargetInfo().getTargetOpts().Triple);

    // Add LLVM module flags.
    auto &clangASTContext = clangImporter->getClangASTContext();
    clangCodeGen->HandleTranslationUnit(
        const_cast<clang::ASTContext &>(clangASTContext));

    emitSwiftVersionNumberIntoModule(llvmModule);

    swift::performLLVM(IRGenOpts, const_cast<ASTContext&>(Context),
                       llvmModule, fileName);
  }
  return false;
}
