//===--- GenClass.cpp - Swift IR Generation For 'class' Types -------------===//
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
//  This file implements IR generation for class types.
//
//===----------------------------------------------------------------------===//

#include "GenClass.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "swift/ABI/Class.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/AttrKind.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILVTableVisitor.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/DeclObjC.h"

#include "Callee.h"
#include "ClassLayout.h"
#include "ClassTypeInfo.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "GenFunc.h"
#include "GenHeap.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MemberAccessStrategy.h"
#include "MetadataLayout.h"
#include "MetadataRequest.h"

using namespace swift;
using namespace irgen;

/// Return the lowered type for the class's 'self' type within its context.
SILType irgen::getSelfType(const ClassDecl *base) {
  auto loweredTy = base->getDeclaredTypeInContext()->getCanonicalType();
  return SILType::getPrimitiveObjectType(loweredTy);
}

/// If the superclass came from another module, we may have dropped
/// stored properties due to the Swift language version availability of
/// their types. In these cases we can't precisely lay out the ivars in
/// the class object at compile time so we need to do runtime layout.
static bool classHasIncompleteLayout(IRGenModule &IGM,
                                     ClassDecl *theClass) {
  if (theClass->getParentModule() == IGM.getSwiftModule())
    return false;

  for (auto field : theClass->getStoredPropertiesAndMissingMemberPlaceholders())
    if (isa<MissingMemberDecl>(field))
      return true;

  return false;
}

namespace {
  class ClassLayoutBuilder : public StructLayoutBuilder {
    SmallVector<ElementLayout, 8> Elements;
    SmallVector<Field, 8> AllStoredProperties;
    SmallVector<FieldAccess, 8> AllFieldAccesses;

    // If we're building a layout with tail-allocated elements, we do
    // things slightly differently; all fields from the superclass are
    // added before the class fields, and the tail elements themselves
    // come after. We don't make a ClassLayout in this case, only a
    // StructLayout.
    Optional<ArrayRef<SILType>> TailTypes;

    // Normally, Swift only emits static metadata for a class if it has no
    // generic ancestry and no fields with resilient value types, which
    // require dynamic layout.
    //
    // However, for interop with Objective-C, where the runtime does not
    // know how to invoke arbitrary code to initialize class metadata, we
    // ignore resilience and emit a static layout and metadata for classes
    // that would otherwise have static metadata, were it not for any
    // resilient fields.
    //
    // This enables two things:
    //
    // - Objective-C can reference the class symbol by calling a static
    //   method on it, for example +alloc, which requires the InstanceSize
    //   to be known, except for possibly sliding ivars.
    //
    // - Objective-C message sends can call methods defined in categories
    //   emitted by Swift, which again require the class metadata symbol
    //   to have a static address.
    //
    // Note that we don't do this if the class is generic, has generic
    // ancestry, or has a superclass that is itself resilient.
    bool CompletelyFragileLayout;

    ClassMetadataOptions Options;

    Size HeaderSize;

  public:
    ClassLayoutBuilder(IRGenModule &IGM, SILType classType,
                       ReferenceCounting refcounting,
                       bool completelyFragileLayout,
                       Optional<ArrayRef<SILType>> tailTypes = None)
      : StructLayoutBuilder(IGM),
        TailTypes(tailTypes),
        CompletelyFragileLayout(completelyFragileLayout) {
      // Start by adding a heap header.
      switch (refcounting) {
      case ReferenceCounting::Native:
        // For native classes, place a full object header.
        addHeapHeader();
        HeaderSize = CurSize;
        break;
      case ReferenceCounting::ObjC:
        // For ObjC-inheriting classes, we don't reliably know the size of the
        // base class, but NSObject only has an `isa` pointer at most.
        addNSObjectHeader();
        HeaderSize = CurSize;
        break;
      case ReferenceCounting::None:
      case ReferenceCounting::Custom:
        break;
      case ReferenceCounting::Block:
      case ReferenceCounting::Unknown:
      case ReferenceCounting::Bridge:
      case ReferenceCounting::Error:
        llvm_unreachable("not a class refcounting kind");
      }
      
      // Next, add the fields for the given class.
      auto theClass = classType.getClassOrBoundGenericClass();
      assert(theClass);

      if (theClass->getObjCImplementationDecl())
        Options |= ClassMetadataFlags::ClassHasObjCImplementation;

      if (theClass->isGenericContext() && !theClass->hasClangNode())
        Options |= ClassMetadataFlags::ClassIsGeneric;

      addFieldsForClass(theClass, classType, /*superclass=*/false);

      if (TailTypes) {
        // Add the tail elements.
        for (SILType TailTy : *TailTypes) {
          const TypeInfo &tailTI = IGM.getTypeInfo(TailTy);
          addTailElement(ElementLayout::getIncomplete(tailTI));
        }
      }
    }

    /// Return the element layouts.
    ArrayRef<ElementLayout> getElements() const {
      return Elements;
    }

    ClassLayout getClassLayout(llvm::Type *classTy) const {
      assert(!TailTypes);

      auto allStoredProps = IGM.Context.AllocateCopy(AllStoredProperties);
      auto allFieldAccesses = IGM.Context.AllocateCopy(AllFieldAccesses);
      auto allElements = IGM.Context.AllocateCopy(Elements);

      return ClassLayout(*this, Options, classTy,
                         allStoredProps, allFieldAccesses, allElements, HeaderSize);
    }

  private:
    /// Adds a layout of a tail-allocated element.
    void addTailElement(const ElementLayout &Elt) {
      Elements.push_back(Elt);
      if (!addField(Elements.back(), LayoutStrategy::Universal)) {
        // For empty tail allocated elements we still add 1 padding byte.
        assert(cast<FixedTypeInfo>(Elt.getType()).getFixedStride() == Size(1) &&
               "empty elements should have stride 1");
        StructFields.push_back(llvm::ArrayType::get(IGM.Int8Ty, 1));
        CurSize += Size(1);
      }
    }

    /// If 'superclass' is true, we're adding fields for one of our
    /// superclasses, which means they become part of the struct
    /// layout calculation, but are not actually added to any of
    /// the vectors like AllStoredProperties, etc. Also, we don't need
    /// to compute FieldAccesses for them.
    void addFieldsForClass(ClassDecl *theClass, SILType classType,
                           bool superclass) {
      addFieldsForClassImpl(theClass, classType, theClass, classType,
                            superclass);
    }

    void addFieldsForClassImpl(ClassDecl *rootClass, SILType rootClassType,
                               ClassDecl *theClass, SILType classType,
                               bool superclass) {
      if (theClass->hasClangNode() && !theClass->isForeignReferenceType()) {
        Options |= ClassMetadataFlags::ClassHasObjCAncestry;
        if (!theClass->getObjCImplementationDecl())
          return;
      }

      if (theClass->isNativeNSObjectSubclass()) {
        // For layout purposes, we don't have ObjC ancestry.
      } else if (theClass->hasSuperclass()) {
        SILType superclassType = classType.getSuperclass();
        auto superclassDecl = superclassType.getClassOrBoundGenericClass();
        assert(superclassType && superclassDecl);

        if (IGM.hasResilientMetadata(superclassDecl,
                                     ResilienceExpansion::Maximal,
                                     rootClass))
          Options |= ClassMetadataFlags::ClassHasResilientAncestry;

        // If the superclass has resilient storage, don't walk its fields.
        if (IGM.isResilient(superclassDecl, ResilienceExpansion::Maximal,
                            rootClass)) {
          Options |= ClassMetadataFlags::ClassHasResilientMembers;

          // If the superclass is generic, we have to assume that its layout
          // depends on its generic parameters. But this only propagates down to
          // subclasses whose superclass type depends on the subclass's generic
          // context.
          if (superclassType.hasArchetype())
            Options |= ClassMetadataFlags::ClassHasGenericLayout;

          // Since we're not going to visit the superclass, make sure that we still
          // set ClassHasObjCAncestry correctly.
          if (superclassType.getASTType()->getReferenceCounting()
                == ReferenceCounting::ObjC) {
            Options |= ClassMetadataFlags::ClassHasObjCAncestry;
          }
        } else {
          // Otherwise, we are allowed to have total knowledge of the superclass
          // fields, so walk them to compute the layout.
          addFieldsForClassImpl(rootClass, rootClassType, superclassDecl,
                                superclassType, /*superclass=*/true);
        }
      }

      if (theClass->isGenericContext())
        Options |= ClassMetadataFlags::ClassHasGenericAncestry;

      if (classHasIncompleteLayout(IGM, theClass))
        Options |= ClassMetadataFlags::ClassHasMissingMembers;

      if (IGM.hasResilientMetadata(theClass, ResilienceExpansion::Maximal,
                                   rootClass))
        Options |= ClassMetadataFlags::ClassHasResilientAncestry;

      if (IGM.isResilient(theClass, ResilienceExpansion::Maximal, rootClass)) {
        Options |= ClassMetadataFlags::ClassHasResilientMembers;
        return;
      }

      // Collect fields from this class and add them to the layout as a chunk.
      addDirectFieldsFromClass(rootClass, rootClassType, theClass, classType,
                               superclass);
    }

    void maybeAddCxxRecordBases(ClassDecl *cd) {
      auto cxxRecord = dyn_cast_or_null<clang::CXXRecordDecl>(cd->getClangDecl());
      if (!cxxRecord)
        return;

      auto &layout = cxxRecord->getASTContext().getASTRecordLayout(cxxRecord);

      for (auto base : cxxRecord->bases()) {
        if (base.isVirtual())
          continue;

        auto baseType = base.getType().getCanonicalType();

        auto baseRecord = cast<clang::RecordType>(baseType)->getDecl();
        auto baseCxxRecord = cast<clang::CXXRecordDecl>(baseRecord);

        if (baseCxxRecord->isEmpty())
          continue;

        auto offset = Size(layout.getBaseClassOffset(baseCxxRecord).getQuantity());
        auto size = Size(cxxRecord->getASTContext().getTypeSizeInChars(baseType).getQuantity());

        if (offset != CurSize) {
          assert(offset > CurSize);
          auto paddingSize = offset - CurSize;
          auto &opaqueTI = IGM.getOpaqueStorageTypeInfo(paddingSize, Alignment(1));
          auto element = ElementLayout::getIncomplete(opaqueTI);
          addField(element, LayoutStrategy::Universal);
        }

        auto &opaqueTI = IGM.getOpaqueStorageTypeInfo(size, Alignment(1));
        auto element = ElementLayout::getIncomplete(opaqueTI);
        addField(element, LayoutStrategy::Universal);
      }
    }

    void addPaddingBeforeClangField(const clang::FieldDecl *fd) {
      auto offset = Size(fd->getASTContext().toCharUnitsFromBits(
          fd->getASTContext().getFieldOffset(fd)).getQuantity());

      if (offset != CurSize) {
        assert(offset > CurSize);
        auto paddingSize = offset - CurSize;
        auto &opaqueTI = IGM.getOpaqueStorageTypeInfo(paddingSize, Alignment(1));
        auto element = ElementLayout::getIncomplete(opaqueTI);
        addField(element, LayoutStrategy::Universal);
      }
    }

    void addDirectFieldsFromClass(ClassDecl *rootClass, SILType rootClassType,
                                  ClassDecl *theClass, SILType classType,
                                  bool superclass) {
      bool collectStoredProperties =
        !superclass ||
        (rootClass->isGenericContext() && !rootClassType.getASTType()
                                               ->getRecursiveProperties()
                                               .hasUnboundGeneric());

      maybeAddCxxRecordBases(theClass);

      auto fn = [&](Field field) {
        // Ignore missing properties here; we should have flagged these
        // with the classHasIncompleteLayout call above.
        if (!field.isConcrete()) {
          assert(Options & ClassMetadataFlags::ClassHasMissingMembers);
          return;
        }

        // Lower the field type.
        SILType type = field.getType(IGM, classType);
        auto *eltType = &IGM.getTypeInfo(type);
        if (CompletelyFragileLayout && !eltType->isFixedSize()) {
          LoweringModeScope scope(IGM, TypeConverter::Mode::Legacy);
          eltType = &IGM.getTypeInfo(type);
        }

        if (!eltType->isFixedSize()) {
          if (type.hasArchetype())
            Options |= ClassMetadataFlags::ClassHasGenericLayout;
          else
            Options |= ClassMetadataFlags::ClassHasResilientMembers;
        }

        auto element = ElementLayout::getIncomplete(*eltType);
        bool isKnownEmpty = !addField(element, LayoutStrategy::Universal);

        // The 'Elements' list only contains superclass fields when we're
        // building a layout for tail allocation.
        if (collectStoredProperties || TailTypes)
          Elements.push_back(element);

        if (collectStoredProperties) {
          AllStoredProperties.push_back(field);
          AllFieldAccesses.push_back(getFieldAccess(isKnownEmpty));
        }
      };

      auto classDecl = dyn_cast<ClassDecl>(theClass);
      if (classDecl) {
        if (classDecl->isRootDefaultActor()) {
          fn(Field::DefaultActorStorage);
        } else if (classDecl->isNonDefaultExplicitDistributedActor()) {
          fn(Field::NonDefaultDistributedActorStorage);
        }
      }

      for (auto decl :
           theClass->getStoredPropertiesAndMissingMemberPlaceholders()) {
        if (decl->getClangDecl())
          if (auto clangField = cast<clang::FieldDecl>(decl->getClangDecl()))
            addPaddingBeforeClangField(clangField);

        if (auto var = dyn_cast<VarDecl>(decl)) {
          fn(var);
        } else {
          fn(cast<MissingMemberDecl>(decl));
        }
      }

      if (!superclass) {
        // If we're calculating the layout of a specialized generic class type,
        // we cannot use field offset globals for dependently-typed fields,
        // because they will not exist -- we only emit such globals for fields
        // which are not dependent in all instantiations.
        //
        // So make sure to fall back to the fully unsubstituted 'abstract layout'
        // for any fields whose offsets are not completely fixed.
        auto *classTI = &IGM.getTypeInfo(classType).as<ClassTypeInfo>();

        SILType selfType = getSelfType(theClass);
        auto *selfTI = &IGM.getTypeInfo(selfType).as<ClassTypeInfo>();

        // Only calculate an abstract layout if its different than the one
        // being computed now.
        if (classTI != selfTI) {
          auto *abstractLayout = &selfTI->getClassLayout(IGM, selfType,
                                                       CompletelyFragileLayout);

          for (unsigned index : indices(AllFieldAccesses)) {
            auto &access = AllFieldAccesses[index];
            auto field = AllStoredProperties[index];
            if (access == FieldAccess::NonConstantDirect)
              access = abstractLayout->getFieldAccessAndElement(field).first;
          }
        }

        // If the class has Objective-C ancestry and we're doing runtime layout
        // that depends on generic parameters, the Swift runtime will first
        // layout the fields relative to the static instance start offset, and
        // then ask the Objective-C runtime to slide them.
        //
        // However, this means that if some fields have a generic type, their
        // alignment will change the instance start offset between generic
        // instantiations, and we cannot use field offset global variables at
        // all, even for fields that come before any generically-typed fields.
        //
        // For example, the alignment of 'x' and 'y' below might depend on 'T':
        //
        // class Foo<T> : NSFoobar {
        //   var x : AKlass = AKlass()
        //   var y : AKlass = AKlass()
        //   var t : T?
        // }
        if (Options.contains(ClassMetadataFlags::ClassHasGenericLayout) &&
            Options.contains(ClassMetadataFlags::ClassHasObjCAncestry)) {
          for (auto &access : AllFieldAccesses) {
            if (access == FieldAccess::NonConstantDirect)
              access = FieldAccess::ConstantIndirect;
          }
        }
      }
    }

    FieldAccess getFieldAccess(bool isKnownEmpty) {
      // If the field known empty, then its access pattern is always
      // constant-direct.
      if (isKnownEmpty)
        return FieldAccess::ConstantDirect;

      // If layout so far depends on generic parameters, we have to load the
      // offset from the field offset vector in class metadata.
      if (Options.contains(ClassMetadataFlags::ClassHasGenericLayout))
        return FieldAccess::ConstantIndirect;

      // If layout so far doesn't depend on any generic parameters, but it's
      // nonetheless not statically known (because either the stored property
      // layout of a superclass is resilient, or one of our own members is a
      // resilient value type), then we can rely on the existence
      // of a global field offset variable which will be initialized by
      // either the Objective-C or Swift runtime, depending on the
      // class's heritage.
      if (Options.contains(ClassMetadataFlags::ClassHasMissingMembers) ||
          Options.contains(ClassMetadataFlags::ClassHasResilientMembers) ||
          Options.contains(ClassMetadataFlags::ClassHasObjCAncestry))
        return FieldAccess::NonConstantDirect;

      // If the layout so far has a fixed size, the field offset is known
      // statically.
      return FieldAccess::ConstantDirect;
    }
  };
} // end anonymous namespace

ClassLayout ClassTypeInfo::generateLayout(IRGenModule &IGM, SILType classType,
                                          bool completelyFragileLayout) const {
  ClassLayoutBuilder builder(IGM, classType, Refcount, completelyFragileLayout);

  auto *classTy = classLayoutType;

  if (completelyFragileLayout) {
    // Create a name for the new llvm type.
    SmallString<32> typeName = classTy->getName();
    typeName += "_fragile";

    // Create the llvm type.
    classTy = llvm::StructType::create(IGM.getLLVMContext(), typeName.str());
  }

  builder.setAsBodyOfStruct(classTy);

  return builder.getClassLayout(classTy);
}

StructLayout *
ClassTypeInfo::createLayoutWithTailElems(IRGenModule &IGM,
                                         SILType classType,
                                         ArrayRef<SILType> tailTypes) const {
  // Add the elements for the class properties.
  ClassLayoutBuilder builder(IGM, classType, Refcount,
                             /*CompletelyFragileLayout=*/false,
                             tailTypes);

  // Create a name for the new llvm type.
  SmallString<32> typeName;
  llvm::raw_svector_ostream os(typeName);
  os << classLayoutType->getName() << "_tailelems" << IGM.TailElemTypeID++;

  // Create the llvm type.
  llvm::StructType *ResultTy = llvm::StructType::create(IGM.getLLVMContext(),
                                                        os.str());
  builder.setAsBodyOfStruct(ResultTy);

  // Create the StructLayout, which is transferred to the caller (the caller is
  // responsible for deleting it).
  return new StructLayout(builder, classType.getClassOrBoundGenericClass(),
                          ResultTy, builder.getElements());
}

const ClassLayout &
ClassTypeInfo::getClassLayout(IRGenModule &IGM, SILType classType,
                              bool forBackwardDeployment) const {
  // Perform fragile layout only if Objective-C interop is enabled.
  bool completelyFragileLayout = (forBackwardDeployment &&
                                  IGM.Context.LangOpts.EnableObjCInterop);

  // Return the cached layout if available.
  auto &Layout = completelyFragileLayout ? FragileLayout : ResilientLayout;
  if (!Layout) {
    auto NewLayout = generateLayout(IGM, classType, completelyFragileLayout);
    assert(!Layout && "generateLayout() should not call itself recursively");
    Layout = NewLayout;
  }

  return *Layout;
}

/// Cast the base to i8*, apply the given inbounds offset (in bytes,
/// as a size_t), and cast to a pointer to the given type.
llvm::Value *IRGenFunction::emitByteOffsetGEP(llvm::Value *base,
                                              llvm::Value *offset,
                                              llvm::Type *objectType,
                                              const llvm::Twine &name) {
  assert(offset->getType() == IGM.SizeTy || offset->getType() == IGM.Int32Ty);
  auto addr = Builder.CreateBitCast(base, IGM.Int8PtrTy);
  addr = Builder.CreateInBoundsGEP(IGM.Int8Ty, addr, offset);
  return Builder.CreateBitCast(addr, objectType->getPointerTo(), name);
}

/// Cast the base to i8*, apply the given inbounds offset (in bytes,
/// as a size_t), and create an address in the given type.
Address IRGenFunction::emitByteOffsetGEP(llvm::Value *base,
                                         llvm::Value *offset,
                                         const TypeInfo &type,
                                         const llvm::Twine &name) {
  auto addr = emitByteOffsetGEP(base, offset, type.getStorageType(), name);
  return type.getAddressForPointer(addr);
}

/// Emit a field l-value by applying the given offset to the given base.
static OwnedAddress emitAddressAtOffset(IRGenFunction &IGF, SILType baseType,
                                        llvm::Value *base, llvm::Value *offset,
                                        VarDecl *field) {
  auto &fieldTI = IGF.getTypeInfo(baseType.getFieldType(
      field, IGF.getSILModule(), IGF.IGM.getMaximalTypeExpansionContext()));
  auto addr = IGF.emitByteOffsetGEP(base, offset, fieldTI,
                              base->getName() + "." + field->getName().str());
  return OwnedAddress(addr, base);
}

llvm::Constant *irgen::tryEmitConstantClassFragilePhysicalMemberOffset(
    IRGenModule &IGM, SILType baseType, VarDecl *field) {
  auto fieldType = baseType.getFieldType(field, IGM.getSILModule(),
                                         IGM.getMaximalTypeExpansionContext());
  // If the field is empty, its address doesn't matter.
  auto &fieldTI = IGM.getTypeInfo(fieldType);
  if (fieldTI.isKnownEmpty(ResilienceExpansion::Maximal)) {
    return llvm::ConstantInt::get(IGM.SizeTy, 0);
  }

  auto &baseClassTI = IGM.getTypeInfo(baseType).as<ClassTypeInfo>();

  auto &classLayout = baseClassTI.getClassLayout(IGM, baseType,
                                               /*forBackwardDeployment=*/false);

  auto fieldInfo = classLayout.getFieldAccessAndElement(field);
  switch (fieldInfo.first) {
  case FieldAccess::ConstantDirect: {
    auto element = fieldInfo.second;
    return llvm::ConstantInt::get(IGM.SizeTy,
                                  element.getByteOffset().getValue());
  }
  case FieldAccess::NonConstantDirect:
  case FieldAccess::ConstantIndirect:
    return nullptr;
  }
  llvm_unreachable("unhandled access");
}

FieldAccess
irgen::getClassFieldAccess(IRGenModule &IGM, SILType baseType, VarDecl *field) {
  auto &baseClassTI = IGM.getTypeInfo(baseType).as<ClassTypeInfo>();
  auto &classLayout = baseClassTI.getClassLayout(IGM, baseType,
                                               /*forBackwardDeployment=*/false);
  return classLayout.getFieldAccessAndElement(field).first;
}

Size
irgen::getClassFieldOffset(IRGenModule &IGM, SILType baseType, VarDecl *field) {
  auto &baseClassTI = IGM.getTypeInfo(baseType).as<ClassTypeInfo>();

  // FIXME: For now we just assume fragile layout here, because this is used as
  // part of emitting class metadata.
  auto &classLayout = baseClassTI.getClassLayout(IGM, baseType,
                                                /*forBackwardDeployment=*/true);

  auto fieldInfo = classLayout.getFieldAccessAndElement(field);
  auto element = fieldInfo.second;
  assert(element.hasByteOffset());
  return element.getByteOffset();
}

StructLayout *
irgen::getClassLayoutWithTailElems(IRGenModule &IGM, SILType classType,
                                   ArrayRef<SILType> tailTypes) {
  auto &ClassTI = IGM.getTypeInfo(classType).as<ClassTypeInfo>();
  return ClassTI.createLayoutWithTailElems(IGM, classType, tailTypes);
}

OwnedAddress irgen::projectPhysicalClassMemberAddress(IRGenFunction &IGF,
                                                      llvm::Value *base,
                                                      SILType baseType,
                                                      SILType fieldType,
                                                      VarDecl *field,
                                                      GenericSignature fnSig) {
  // If the field is empty, its address doesn't matter.
  auto &fieldTI = IGF.getTypeInfo(fieldType);
  if (fieldTI.isKnownEmpty(ResilienceExpansion::Maximal)) {
    return OwnedAddress(fieldTI.getUndefAddress(), base);
  }
  
  auto &baseClassTI = IGF.getTypeInfo(baseType).as<ClassTypeInfo>();
  ClassDecl *baseClass = baseClassTI.getClass();

  auto &classLayout = baseClassTI.getClassLayout(IGF.IGM, baseType,
                                               /*forBackwardDeployment=*/false);

  auto fieldInfo = classLayout.getFieldAccessAndElement(field);

  switch (fieldInfo.first) {
  case FieldAccess::ConstantDirect: {
    Address baseAddr(base, classLayout.getType(), classLayout.getAlignment());
    auto element = fieldInfo.second;
    Address memberAddr = element.project(IGF, baseAddr, None);
    // We may need to bitcast the address if the field is of a generic type.
    if (memberAddr.getElementType() != fieldTI.getStorageType())
      memberAddr = IGF.Builder.CreateElementBitCast(memberAddr,
                                                    fieldTI.getStorageType());
    return OwnedAddress(memberAddr, base);
  }
    
  case FieldAccess::NonConstantDirect: {
    Address offsetA = IGF.IGM.getAddrOfFieldOffset(field, NotForDefinition);
    auto offset = IGF.Builder.CreateLoad(offsetA, "offset");
    return emitAddressAtOffset(IGF, baseType, base, offset, field);
  }
    
  case FieldAccess::ConstantIndirect: {
    auto metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType, fnSig);
    auto offset = emitClassFieldOffset(IGF, baseClass, field, metadata);
    return emitAddressAtOffset(IGF, baseType, base, offset, field);
  }
  }
  llvm_unreachable("bad field-access strategy");
}

MemberAccessStrategy
irgen::getPhysicalClassMemberAccessStrategy(IRGenModule &IGM,
                                            SILType baseType, VarDecl *field) {
  auto &baseClassTI = IGM.getTypeInfo(baseType).as<ClassTypeInfo>();
  ClassDecl *baseClass = baseType.getClassOrBoundGenericClass();

  auto &classLayout = baseClassTI.getClassLayout(IGM, baseType,
                                               /*forBackwardDeployment=*/false);
  auto fieldInfo = classLayout.getFieldAccessAndElement(field);

  switch (fieldInfo.first) {
  case FieldAccess::ConstantDirect: {
    auto element = fieldInfo.second;
    return MemberAccessStrategy::getDirectFixed(element.getByteOffset());
  }

  case FieldAccess::NonConstantDirect: {
    std::string symbol =
      LinkEntity::forFieldOffset(field).mangleAsString();
    return MemberAccessStrategy::getDirectGlobal(std::move(symbol),
                                 MemberAccessStrategy::OffsetKind::Bytes_Word);
  }

  case FieldAccess::ConstantIndirect: {
    Size indirectOffset = getClassFieldOffsetOffset(IGM, baseClass, field);
    return MemberAccessStrategy::getIndirectFixed(indirectOffset,
                                 MemberAccessStrategy::OffsetKind::Bytes_Word);
  }
  }
  llvm_unreachable("bad field-access strategy");
}

Address irgen::emitTailProjection(IRGenFunction &IGF, llvm::Value *Base,
                                  SILType ClassType,
                                  SILType TailType,
                                  GenericSignature fnSig) {
  const ClassTypeInfo &classTI = IGF.getTypeInfo(ClassType).as<ClassTypeInfo>();

  llvm::Value *Offset = nullptr;
  auto &layout = classTI.getClassLayout(IGF.IGM, ClassType,
                                        /*forBackwardDeployment=*/false);
  Alignment HeapObjAlign = IGF.IGM.TargetInfo.HeapObjectAlignment;
  Alignment Align;

  // Get the size of the class instance.
  if (layout.isFixedLayout()) {
    Size ClassSize = layout.getSize();
    Offset = llvm::ConstantInt::get(IGF.IGM.SizeTy, ClassSize.getValue());
    Align = HeapObjAlign.alignmentAtOffset(ClassSize);
  } else {
    llvm::Value *metadata = emitHeapMetadataRefForHeapObject(IGF, Base,
                                                             ClassType,
                                                             fnSig);
    Offset = emitClassResilientInstanceSizeAndAlignMask(IGF,
                                        ClassType.getClassOrBoundGenericClass(),
                                        metadata).first;
  }
  // Align up to the TailType.
  assert(TailType.isObject());
  const TypeInfo &TailTI = IGF.getTypeInfo(TailType);
  llvm::Value *AlignMask = TailTI.getAlignmentMask(IGF, TailType);
  Offset = IGF.Builder.CreateAdd(Offset, AlignMask);
  llvm::Value *InvertedMask = IGF.Builder.CreateNot(AlignMask);
  Offset = IGF.Builder.CreateAnd(Offset, InvertedMask);

  Address Addr = IGF.emitByteOffsetGEP(Base, Offset, TailTI, "tailaddr");

  if (auto *OffsetConst = dyn_cast<llvm::ConstantInt>(Offset)) {
    // Try to get an accurate alignment (only possible if the Offset is a
    // constant).
    Size TotalOffset(OffsetConst->getZExtValue());
    Align = HeapObjAlign.alignmentAtOffset(TotalOffset);
  }
  return Address(Addr.getAddress(), Addr.getElementType(), Align);
}

/// Try to stack promote a class instance with possible tail allocated arrays.
///
/// Returns the alloca if successful, or nullptr otherwise.
static llvm::Value *stackPromote(IRGenFunction &IGF,
                      const ClassLayout &FieldLayout,
                      int &StackAllocSize,
                      ArrayRef<std::pair<SILType, llvm::Value *>> TailArrays) {
  if (StackAllocSize < 0)
    return nullptr;
  if (!FieldLayout.isFixedLayout())
    return nullptr;

  // Calculate the total size needed.
  // The first part is the size of the class itself.
  Alignment ClassAlign = FieldLayout.getAlignment();
  Size TotalSize = FieldLayout.getSize();

  // Add size for tail-allocated arrays.
  for (const auto &TailArray : TailArrays) {
    SILType ElemTy = TailArray.first;
    llvm::Value *Count = TailArray.second;

    // We can only calculate a constant size if the tail-count is constant.
    auto *CI = dyn_cast<llvm::ConstantInt>(Count);
    if (!CI)
      return nullptr;

    const TypeInfo &ElemTI = IGF.getTypeInfo(ElemTy);
    if (!ElemTI.isFixedSize())
      return nullptr;

    const FixedTypeInfo &ElemFTI = ElemTI.as<FixedTypeInfo>();
    Alignment ElemAlign = ElemFTI.getFixedAlignment();

    // This should not happen - just to be save.
    if (ElemAlign > ClassAlign)
      return nullptr;

    TotalSize = TotalSize.roundUpToAlignment(ElemAlign);
    TotalSize += ElemFTI.getFixedStride() * CI->getValue().getZExtValue();
  }
  if (TotalSize > Size(StackAllocSize))
    return nullptr;
  StackAllocSize = TotalSize.getValue();

  if (TotalSize == FieldLayout.getSize()) {
    // No tail-allocated arrays: we can use the llvm class type for alloca.
    llvm::Type *ClassTy = FieldLayout.getType();
    Address Alloca = IGF.createAlloca(ClassTy, ClassAlign, "reference.raw");
    return Alloca.getAddress();
  }
  // Use a byte-array as type for alloca.
  llvm::Value *SizeVal = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                TotalSize.getValue());
  Address Alloca = IGF.createAlloca(IGF.IGM.Int8Ty, SizeVal, ClassAlign,
                                    "reference.raw");
  return Alloca.getAddress();
}

std::pair<llvm::Value *, llvm::Value *>
irgen::appendSizeForTailAllocatedArrays(IRGenFunction &IGF,
                                    llvm::Value *size, llvm::Value *alignMask,
                                    TailArraysRef TailArrays) {
  for (const auto &TailArray : TailArrays) {
    SILType ElemTy = TailArray.first;
    llvm::Value *Count = TailArray.second;

    const TypeInfo &ElemTI = IGF.getTypeInfo(ElemTy);

    // Align up to the tail-allocated array.
    llvm::Value *ElemStride = ElemTI.getStride(IGF, ElemTy);
    llvm::Value *ElemAlignMask = ElemTI.getAlignmentMask(IGF, ElemTy);
    size = IGF.Builder.CreateAdd(size, ElemAlignMask);
    llvm::Value *InvertedMask = IGF.Builder.CreateNot(ElemAlignMask);
    size = IGF.Builder.CreateAnd(size, InvertedMask);

    // Add the size of the tail allocated array.
    llvm::Value *AllocSize = IGF.Builder.CreateMul(ElemStride, Count);
    size = IGF.Builder.CreateAdd(size, AllocSize);
    alignMask = IGF.Builder.CreateOr(alignMask, ElemAlignMask);
  }
  return {size, alignMask};
}


/// Emit an allocation of a class.
llvm::Value *irgen::emitClassAllocation(IRGenFunction &IGF, SILType selfType,
                                        bool objc, int &StackAllocSize,
                                        TailArraysRef TailArrays) {
  auto &classTI = IGF.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto classType = selfType.getASTType();

  // If we need to use Objective-C allocation, do so.
  // If the root class isn't known to use the Swift allocator, we need
  // to call [self alloc].
  if (objc) {
    llvm::Value *metadata =
      emitClassHeapMetadataRef(IGF, classType, MetadataValueType::ObjCClass,
                               MetadataState::Complete,
                               /*allow uninitialized*/ true);
    StackAllocSize = -1;
    return emitObjCAllocObjectCall(IGF, metadata, selfType);
  }

  llvm::Value *metadata =
    emitClassHeapMetadataRef(IGF, classType, MetadataValueType::TypeMetadata,
                             MetadataState::Complete);

  auto &classLayout = classTI.getClassLayout(IGF.IGM, selfType,
                                             /*forBackwardDeployment=*/false);

  llvm::Type *destType = classLayout.getType()->getPointerTo();
  llvm::Value *val = nullptr;
  if (llvm::Value *Promoted = stackPromote(IGF, classLayout, StackAllocSize,
                                           TailArrays)) {
    val = IGF.Builder.CreateBitCast(Promoted, IGF.IGM.RefCountedPtrTy);
    val = IGF.emitInitStackObjectCall(metadata, val, "reference.new");
  } else {
    llvm::Value *size, *alignMask;
    if (classLayout.isFixedSize()) {
      size = IGF.IGM.getSize(classLayout.getSize());
      alignMask = IGF.IGM.getSize(classLayout.getAlignMask());
    } else {
      std::tie(size, alignMask)
        = emitClassResilientInstanceSizeAndAlignMask(IGF,
                                       selfType.getClassOrBoundGenericClass(),
                                       metadata);
    }

    // Allocate the object on the heap.
    std::tie(size, alignMask)
      = appendSizeForTailAllocatedArrays(IGF, size, alignMask, TailArrays);
    val = IGF.emitAllocObjectCall(metadata, size, alignMask, "reference.new");
    StackAllocSize = -1;
  }
  return IGF.Builder.CreateBitCast(val, destType);
}

llvm::Value *irgen::emitClassAllocationDynamic(IRGenFunction &IGF, 
                                               llvm::Value *metadata,
                                               SILType selfType,
                                               bool objc,
                                               int &StackAllocSize,
                                               TailArraysRef TailArrays) {
  // If we need to use Objective-C allocation, do so.
  if (objc) {
    StackAllocSize = -1;
    return emitObjCAllocObjectCall(IGF, metadata, selfType);
  }

  llvm::Value *Promoted;
  auto &classTI = IGF.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &classLayout = classTI.getClassLayout(IGF.IGM, selfType,
                                          /*forBackwardDeployment=*/false);

  // If we are allowed to allocate on the stack we are allowed to use
  // `selfType`'s size assumptions.
  if (StackAllocSize >= 0 &&
      (Promoted = stackPromote(IGF, classLayout, StackAllocSize,
                                           TailArrays))) {
    llvm::Value *val = IGF.Builder.CreateBitCast(Promoted,
                                                 IGF.IGM.RefCountedPtrTy);
    val = IGF.emitInitStackObjectCall(metadata, val, "reference.new");

    llvm::Type *destType = classLayout.getType()->getPointerTo();
    return IGF.Builder.CreateBitCast(val, destType);
  }

  // Otherwise, allocate using Swift's routines.
  llvm::Value *size, *alignMask;
  std::tie(size, alignMask)
    = emitClassResilientInstanceSizeAndAlignMask(IGF,
                                   selfType.getClassOrBoundGenericClass(),
                                   metadata);
  std::tie(size, alignMask)
    = appendSizeForTailAllocatedArrays(IGF, size, alignMask, TailArrays);

  llvm::Value *val = IGF.emitAllocObjectCall(metadata, size, alignMask,
                                             "reference.new");
  StackAllocSize = -1;
  llvm::Type *destType = classLayout.getType()->getPointerTo();
  return IGF.Builder.CreateBitCast(val, destType);
}

/// Get the instance size and alignment mask for the given class
/// instance.
static void getInstanceSizeAndAlignMask(IRGenFunction &IGF,
                                        SILType selfType,
                                        ClassDecl *selfClass,
                                        llvm::Value *selfValue,
                                        GenericSignature fnSig,
                                        llvm::Value *&size,
                                        llvm::Value *&alignMask) {
  // Try to determine the size of the object we're deallocating.
  auto &info = IGF.IGM.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &layout = info.getClassLayout(IGF.IGM, selfType,
                                     /*forBackwardDeployment=*/false);

  // If it's fixed, emit the constant size and alignment mask.
  if (layout.isFixedLayout()) {
    size = IGF.IGM.getSize(layout.getSize());
    alignMask = IGF.IGM.getSize(layout.getAlignMask());
    return;
  }

  // Otherwise, get them from the metadata.
  llvm::Value *metadata =
    emitHeapMetadataRefForHeapObject(IGF, selfValue, selfType, fnSig);
  std::tie(size, alignMask)
    = emitClassResilientInstanceSizeAndAlignMask(IGF, selfClass, metadata);
}

static llvm::Value *emitCastToHeapObject(IRGenFunction &IGF,
                                         llvm::Value *value) {
  return IGF.Builder.CreateBitCast(value, IGF.IGM.RefCountedPtrTy);
}

void irgen::emitClassDeallocation(IRGenFunction &IGF,
                                  SILType selfType,
                                  llvm::Value *selfValue,
                                  GenericSignature fnSig) {
  auto *theClass = selfType.getClassOrBoundGenericClass();

  // We want to deallocate default actors or potential default
  // actors differently.  We assume that being a default actor
  // is purely a property of the root actor class, so just go to
  // that class.
  if (auto rootActorClass = theClass->getRootActorClass()) {
    // If it's a default actor, use swift_deallocDefaultActor.
    if (rootActorClass->isDefaultActor(IGF.IGM.getSwiftModule(),
                                       ResilienceExpansion::Maximal)) {
      selfValue = emitCastToHeapObject(IGF, selfValue);
      IGF.Builder.CreateCall(IGF.IGM.getDefaultActorDeallocateFunctionPointer(),
                             {selfValue});
      return;
    }

    // If it's possibly a default actor, use a resilient pattern.
    if (!rootActorClass->isForeign() &&
        rootActorClass->isResilient(IGF.IGM.getSwiftModule(),
                                    ResilienceExpansion::Maximal)) {
      selfValue = emitCastToHeapObject(IGF, selfValue);
      IGF.Builder.CreateCall(
          IGF.IGM.getDefaultActorDeallocateResilientFunctionPointer(),
          {selfValue});
      return;
    }

    // Otherwise use the normal path.
  }

  llvm::Value *size, *alignMask;
  getInstanceSizeAndAlignMask(IGF, selfType, theClass, selfValue, fnSig,
                              size, alignMask);

  selfValue = emitCastToHeapObject(IGF, selfValue);
  emitDeallocateClassInstance(IGF, selfValue, size, alignMask);
}

void irgen::emitPartialClassDeallocation(IRGenFunction &IGF,
                                         SILType selfType,
                                         llvm::Value *selfValue,
                                         llvm::Value *metadataValue,
                                         GenericSignature fnSig) {
  auto *theClass = selfType.getClassOrBoundGenericClass();
  assert(theClass->getForeignClassKind() == ClassDecl::ForeignKind::Normal);

  llvm::Value *size, *alignMask;
  getInstanceSizeAndAlignMask(IGF, selfType, theClass, selfValue, fnSig,
                              size, alignMask);

  selfValue = IGF.Builder.CreateBitCast(selfValue, IGF.IGM.RefCountedPtrTy);
  emitDeallocatePartialClassInstance(IGF, selfValue, metadataValue,
                                     size, alignMask);
}

/// emitClassDecl - Emit all the declarations associated with this class type.
void IRGenModule::emitClassDecl(ClassDecl *D) {
  PrettyStackTraceDecl prettyStackTrace("emitting class metadata for", D);

  SILType selfType = getSelfType(D);
  auto &classTI = getTypeInfo(selfType).as<ClassTypeInfo>();

  // Use the fragile layout when emitting metadata.
  auto &fragileLayout =
    classTI.getClassLayout(*this, selfType, /*forBackwardDeployment=*/true);

  // The resilient layout tells us what parts of the metadata can be
  // updated at runtime by the Objective-C metadata update callback.
  auto &resilientLayout =
    classTI.getClassLayout(*this, selfType, /*forBackwardDeployment=*/false);

  // As a matter of policy, class metadata is never emitted lazily for now.
  assert(!IRGen.hasLazyMetadata(D));

  // Emit the class metadata.
  emitClassMetadata(*this, D, fragileLayout, resilientLayout);
  emitFieldDescriptor(D);

  IRGen.addClassForEagerInitialization(D);
  IRGen.addBackDeployedObjCActorInitialization(D);

  emitNestedTypeDecls(D->getMembers());
}

namespace {
  using CategoryNameKey = std::pair<ClassDecl*, ModuleDecl*>;
  /// Used to provide unique names to ObjC categories generated by Swift
  /// extensions. The first category for a class in a module gets the module's
  /// name as its key, e.g., NSObject (MySwiftModule). Another extension of the
  /// same class in the same module gets a category name with a number appended,
  /// e.g., NSObject (MySwiftModule1).
  llvm::DenseMap<CategoryNameKey, unsigned> CategoryCounts;

  /// A class for building ObjC class data (in Objective-C terms, class_ro_t),
  /// category data (category_t), or protocol data (protocol_t).
  class ClassDataBuilder : public ClassMemberVisitor<ClassDataBuilder> {
    IRGenModule &IGM;
    using ClassPair = std::pair<ClassDecl *, CanType>;
    using ClassUnion = TaggedUnion<ClassDecl *, ClassPair>;
    TaggedUnion<ClassUnion, ProtocolDecl *> TheEntity;
    ExtensionDecl *TheExtension;
    const ClassLayout *FieldLayout;

    ClassDecl *getClass() const {
      const ClassUnion *classUnion;
      if (!(classUnion = TheEntity.dyn_cast<ClassUnion>())) {
        return nullptr;
      }
      if (auto *const *theClass = classUnion->dyn_cast<ClassDecl *>()) {
        return *theClass;
      }
      auto pair = classUnion->get<ClassPair>();
      return pair.first;
    }
    ProtocolDecl *getProtocol() const {
      if (auto *const *theProtocol = TheEntity.dyn_cast<ProtocolDecl *>()) {
        return *theProtocol;
      }
      return nullptr;
    }
    Optional<CanType> getSpecializedGenericType() const {
      const ClassUnion *classUnion;
      if (!(classUnion = TheEntity.dyn_cast<ClassUnion>())) {
        return llvm::None;
      }
      const ClassPair *classPair;
      if (!(classPair = classUnion->dyn_cast<ClassPair>())) {
        return llvm::None;
      }
      auto &pair = *classPair;
      return pair.second;
    }

    Optional<StringRef> getObjCImplCategoryName() const {
      if (!TheExtension || !TheExtension->isObjCImplementation())
        return None;
      if (auto ident = TheExtension->getCategoryNameForObjCImplementation()) {
        assert(!ident->empty());
        return ident->str();
      }
      return None;
    }
    bool isBuildingClass() const {
      return TheEntity.isa<ClassUnion>() && !TheExtension;
    }
    bool isBuildingCategory() const {
      return TheEntity.isa<ClassUnion>() && TheExtension;
    }
    bool isBuildingProtocol() const { return TheEntity.isa<ProtocolDecl *>(); }

    bool HasNonTrivialDestructor = false;
    bool HasNonTrivialConstructor = false;

    class MethodDescriptor {
    public:
      enum class Kind {
        Method,
        IVarInitializer,
        IVarDestroyer,
      };

    private:
      llvm::PointerIntPair<void*, 2, Kind> Data;
      static_assert(llvm::PointerLikeTypeTraits<llvm::Function*>
                      ::NumLowBitsAvailable >= 2,
                    "llvm::Function* isn't adequately aligned");
      static_assert(llvm::PointerLikeTypeTraits<AbstractFunctionDecl*>
                      ::NumLowBitsAvailable >= 2,
                    "AbstractFuncDecl* isn't adequately aligned");

      MethodDescriptor(Kind kind, void *ptr) : Data(ptr, kind) {}

    public:
      MethodDescriptor(AbstractFunctionDecl *method)
          : Data(method, Kind::Method) {
        assert(method && "null method provided");
      }
      static MethodDescriptor getIVarInitializer(llvm::Function *fn) {
        assert(fn && "null impl provided");
        return MethodDescriptor(Kind::IVarInitializer, fn);
      }
      static MethodDescriptor getIVarDestroyer(llvm::Function *fn) {
        assert(fn && "null impl provided");
        return MethodDescriptor(Kind::IVarDestroyer, fn);
      }

      Kind getKind() const { return Data.getInt(); }
      AbstractFunctionDecl *getMethod() {
        assert(getKind() == Kind::Method);
        return static_cast<AbstractFunctionDecl*>(Data.getPointer());
      }
      llvm::Function *getImpl() {
        assert(getKind() != Kind::Method);
        return static_cast<llvm::Function*>(Data.getPointer());
      }
    };

    llvm::SmallString<16> CategoryName;
    SmallVector<Field, 8> Ivars;
    SmallVector<MethodDescriptor, 16> InstanceMethods;
    SmallVector<MethodDescriptor, 16> ClassMethods;
    SmallVector<MethodDescriptor, 16> OptInstanceMethods;
    SmallVector<MethodDescriptor, 16> OptClassMethods;
    SmallVector<ProtocolDecl*, 4> Protocols;
    SmallVector<VarDecl*, 8> InstanceProperties;
    SmallVector<VarDecl*, 8> ClassProperties;

    llvm::Constant *Name = nullptr;

    SmallVectorImpl<MethodDescriptor> &getMethodList(ValueDecl *decl) {
      if (decl->getAttrs().hasAttribute<OptionalAttr>()) {
        if (decl->isStatic()) {
          return OptClassMethods;
        } else {
          return OptInstanceMethods;
        }
      } else {
        if (decl->isStatic()) {
          return ClassMethods;
        } else {
          return InstanceMethods;
        }
      }
    }

  public:
    ClassDataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                     const ClassLayout &fieldLayout)
        : ClassDataBuilder(IGM, ClassUnion(theClass), fieldLayout) {}

    ClassDataBuilder(
        IRGenModule &IGM,
        TaggedUnion<ClassDecl *, std::pair<ClassDecl *, CanType>> theUnion,
        const ClassLayout &fieldLayout)
        : IGM(IGM), TheEntity(theUnion), TheExtension(nullptr),
          FieldLayout(&fieldLayout) {
      visitConformances(getClass());

      if (getClass()->isRootDefaultActor()) {
        Ivars.push_back(Field::DefaultActorStorage);
      } else if (getClass()->isNonDefaultExplicitDistributedActor()) {
        Ivars.push_back(Field::NonDefaultDistributedActorStorage);
      }
      visitImplementationMembers(getClass());

      if (Lowering::usesObjCAllocator(getClass())) {
        addIVarInitializer();
        addIVarDestroyer();
      }
    }

    ClassDataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                     ExtensionDecl *theExtension)
        : IGM(IGM), TheEntity(ClassUnion(theClass)), TheExtension(theExtension),
          FieldLayout(nullptr) {
      buildCategoryName(CategoryName);

      visitConformances(theExtension);

      for (Decl *member : TheExtension->getImplementationContext()->getMembers())
        visit(member);
    }

    ClassDataBuilder(IRGenModule &IGM, ProtocolDecl *theProtocol)
      : IGM(IGM), TheEntity(theProtocol), TheExtension(nullptr)
    {
      llvm::SmallSetVector<ProtocolDecl *, 2> protocols;

      // Gather protocol references for all of the directly inherited
      // Objective-C protocol conformances.
      for (ProtocolDecl *p : theProtocol->getInheritedProtocols()) {
        getObjCProtocols(p, protocols);
      }

      // Add any restated Objective-C protocol conformances.
      for (auto *attr :
             theProtocol
               ->getAttrs().getAttributes<RestatedObjCConformanceAttr>()) {
        getObjCProtocols(attr->Proto, protocols);
      }

      for (ProtocolDecl *proto : protocols) {
        Protocols.push_back(proto);
      }

      for (Decl *member : theProtocol->getMembers()) {
        // Async methods coming from ObjC protocols shouldn't be recorded twice.
        // At the moment, the language doesn't allow suppressing the
        // completionHandler-based variant, so this is sufficient.
        if (theProtocol->hasClangNode() && theProtocol->isObjC()) {
          if (auto funcOrAccessor = dyn_cast<AbstractFunctionDecl>(member)) {
            if (funcOrAccessor->isAsyncContext()) {
              continue;
            }
          }
        }
        visit(member);
      }
    }

    /// Gather protocol records for all of the explicitly-specified Objective-C
    /// protocol conformances.
    void visitConformances(const IterableDeclContext *idc) {
      auto dc = idc->getAsGenericContext();
      if (dc->getImplementedObjCContext() != dc) {
        // We want to use the conformance list imported from the ObjC header.
        auto importedIDC = cast<IterableDeclContext>(
                                 dc->getImplementedObjCContext()->getAsDecl());
        visitConformances(importedIDC);
        return;
      }

      llvm::SmallSetVector<ProtocolDecl *, 2> protocols;
      for (auto conformance : idc->getLocalConformances(
                                ConformanceLookupKind::OnlyExplicit)) {
        ProtocolDecl *proto = conformance->getProtocol();
        getObjCProtocols(proto, protocols);
      }

      for (ProtocolDecl *proto : protocols) {
        Protocols.push_back(proto);
      }
    }

    /// Add the protocol to the vector, if it's Objective-C protocol,
    /// or search its superprotocols.
    void getObjCProtocols(ProtocolDecl *proto,
                          llvm::SmallSetVector<ProtocolDecl *, 2> &result) {
      if (proto->isObjC()) {
        result.insert(proto);
      } else {
        for (ProtocolDecl *inherited : proto->getInheritedProtocols()) {
          // Recursively check inherited protocol for objc conformance.
          getObjCProtocols(inherited, result);
        }
      }
    }

    llvm::Constant *getMetaclassRefOrNull(Type specializedGenericType,
                                          ClassDecl *theClass) {
      if (specializedGenericType) {
        return IGM.getAddrOfCanonicalSpecializedGenericMetaclassObject(
            specializedGenericType->getCanonicalType(), NotForDefinition);
      }
      if (theClass->isGenericContext() && !theClass->hasClangNode()) {
        return llvm::ConstantPointerNull::get(IGM.ObjCClassPtrTy);
      } else {
        return IGM.getAddrOfMetaclassObject(theClass, NotForDefinition);
      }
    }

    void buildMetaclassStub() {
      assert(FieldLayout && "can't build a metaclass from a category");

      Optional<CanType> specializedGenericType = getSpecializedGenericType();

      // The isa is the metaclass pointer for the root class.
      auto rootClass = getRootClassForMetaclass(IGM, getClass());
      Type rootType;
      if (specializedGenericType && rootClass->isGenericContext()) {
        rootType =
            (*specializedGenericType)->getRootClass(
               /*useArchetypes=*/false);
      } else {
        rootType = Type();
      }
      auto rootPtr = getMetaclassRefOrNull(rootType, rootClass);

      // The superclass of the metaclass is the metaclass of the
      // superclass.  Note that for metaclass stubs, we can always
      // ignore parent contexts and generic arguments.
      //
      // If this class has no formal superclass, then its actual
      // superclass is SwiftObject, i.e. the root class.
      llvm::Constant *superPtr;
      if (auto base = getSuperclassDeclForMetadata(IGM, getClass())) {
        if (specializedGenericType && base->isGenericContext()) {
          superPtr = getMetaclassRefOrNull(
              getSuperclassForMetadata(IGM, *specializedGenericType,
                                       /*useArchetypes=*/false),
              base);
        } else {
          superPtr = getMetaclassRefOrNull(Type(), base);
        }
      } else {
        superPtr = getMetaclassRefOrNull(
            Type(), IGM.getObjCRuntimeBaseForSwiftRootClass(getClass()));
      }

      auto dataPtr = emitROData(ForMetaClass, DoesNotHaveUpdateCallback);
      dataPtr = llvm::ConstantExpr::getPtrToInt(dataPtr, IGM.IntPtrTy);

      llvm::Constant *fields[] = {
        rootPtr,
        superPtr,
        IGM.getObjCEmptyCachePtr(),
        IGM.getObjCEmptyVTablePtr(),
        dataPtr
      };
      auto init = llvm::ConstantStruct::get(IGM.ObjCClassStructTy,
                                            makeArrayRef(fields));
      llvm::Constant *uncastMetaclass;
      if (specializedGenericType) {
        uncastMetaclass =
            IGM.getAddrOfCanonicalSpecializedGenericMetaclassObject(
                *specializedGenericType, ForDefinition);
      } else {
        uncastMetaclass =
            IGM.getAddrOfMetaclassObject(getClass(), ForDefinition);
      }
      auto metaclass = cast<llvm::GlobalVariable>(uncastMetaclass);
      metaclass->setInitializer(init);
    }
    
  private:
    void buildCategoryName(SmallVectorImpl<char> &s) {
      llvm::raw_svector_ostream os(s);

      if (auto implementationCategoryName = getObjCImplCategoryName()) {
        os << *implementationCategoryName;
        return;
      }

      // Find the module the extension is declared in.
      ModuleDecl *TheModule = TheExtension->getParentModule();

      os << TheModule->getName();
      
      unsigned categoryCount = CategoryCounts[{getClass(), TheModule}]++;
      if (categoryCount > 0)
        os << categoryCount;
    }

    llvm::Constant *getClassMetadataRef() {
      auto *theClass = getClass();

      // If this is truly an imported ObjC class, with no @_objcImplementation,
      // someone else will emit the ObjC metadata symbol and we simply want to
      // use it.
      if (theClass->hasClangNode() && !theClass->getObjCImplementationDecl())
        return IGM.getAddrOfObjCClass(theClass, NotForDefinition);

      // Note that getClassMetadataStrategy() will return
      // ClassMetadataStrategy::Resilient if the class is
      // from another resilience domain, even if inside that
      // resilience domain the class has fixed metadata
      // layout.
      //
      // Since a class only has a class stub if its class
      // hierarchy crosses resilience domains, we use a
      // slightly different query here.
      if (theClass->checkAncestry(AncestryFlags::ResilientOther)) {
        return IGM.getAddrOfObjCResilientClassStub(theClass, NotForDefinition,
                                             TypeMetadataAddress::AddressPoint);
      }

      auto type = getSelfType(theClass).getASTType();
      return tryEmitConstantHeapMetadataRef(IGM, type, /*allowUninit*/ true);
    }

  public:
    llvm::Constant *emitCategory() {
      assert(TheExtension && "can't emit category data for a class");
      ConstantInitBuilder builder(IGM);
      auto fields = builder.beginStruct();
      auto internalLinkage = llvm::GlobalVariable::InternalLinkage;

      // struct category_t {
      //   char const *name;
      fields.add(IGM.getAddrOfGlobalString(CategoryName));
      //   const class_t *theClass;
      fields.add(getClassMetadataRef());
      //   const method_list_t *instanceMethods;
      emitAndAddMethodList(fields, MethodListKind::InstanceMethods,
                           internalLinkage);
      //   const method_list_t *classMethods;
      emitAndAddMethodList(fields, MethodListKind::ClassMethods,
                           internalLinkage);
      //   const protocol_list_t *baseProtocols;
      fields.add(buildProtocolList(internalLinkage));
      //   const property_list_t *properties;
      fields.add(buildPropertyList(ForClass, internalLinkage));
      //   const property_list_t *classProperties;
      fields.add(buildPropertyList(ForMetaClass, internalLinkage));
      //   uint32_t size;
      // FIXME: Clang does this by using non-ad-hoc types for ObjC runtime
      // structures.
      Size size = 7 * IGM.getPointerSize() + Size(4);
      fields.addInt32(size.getValue());
      // };

      assert(fields.getNextOffsetFromGlobal() == size);
      return buildGlobalVariable(fields, "_CATEGORY_", /*const*/ true,
                                 internalLinkage);
    }
    
    llvm::Constant *emitProtocol() {
      ConstantInitBuilder builder(IGM);
      auto fields = builder.beginStruct();
      llvm::SmallString<64> nameBuffer;
      auto weakLinkage = llvm::GlobalVariable::WeakAnyLinkage;

      assert(isBuildingProtocol() && "not emitting a protocol");
      
      // struct protocol_t {
      //   Class super;
      fields.addNullPointer(IGM.Int8PtrTy);
      //   char const *name;
      fields.add(IGM.getAddrOfGlobalString(getEntityName(nameBuffer)));
      //   const protocol_list_t *baseProtocols;
      fields.add(buildProtocolList(weakLinkage));
      //   const method_list_t *requiredInstanceMethods;
      emitAndAddMethodList(fields, MethodListKind::InstanceMethods,
                           weakLinkage);
      //   const method_list_t *requiredClassMethods;
      emitAndAddMethodList(fields, MethodListKind::ClassMethods, weakLinkage);
      //   const method_list_t *optionalInstanceMethods;
      emitAndAddMethodList(fields, MethodListKind::OptionalInstanceMethods,
                           weakLinkage);
      //   const method_list_t *optionalClassMethods;
      emitAndAddMethodList(fields, MethodListKind::OptionalClassMethods,
                           weakLinkage);
      //   const property_list_t *properties;
      fields.add(buildPropertyList(ForClass, weakLinkage));

      //   uint32_t size;
      // FIXME: Clang does this by using non-ad-hoc types for ObjC runtime
      // structures.
      Size size = 11 * IGM.getPointerSize() + 2 * Size(4);
      fields.addInt32(size.getValue());

      //   uint32_t flags;
      auto flags = ProtocolDescriptorFlags()
        .withSwift(!getProtocol()->hasClangNode())
        .withClassConstraint(ProtocolClassConstraint::Class)
        .withDispatchStrategy(ProtocolDispatchStrategy::ObjC)
        .withSpecialProtocol(getSpecialProtocolID(getProtocol()));
      
      fields.addInt32(flags.getIntValue());
      
      //   const char ** extendedMethodTypes;
      fields.add(buildOptExtendedMethodTypes());
      //   const char *demangledName;
      fields.addNullPointer(IGM.Int8PtrTy);
      //   const property_list_t *classProperties;
      fields.add(buildPropertyList(ForMetaClass, weakLinkage));
      // };

      assert(fields.getNextOffsetFromGlobal() == size);
      return buildGlobalVariable(fields, "_PROTOCOL_", /*const*/ true,
                                 weakLinkage);
    }

    void emitRODataFields(ConstantStructBuilder &b,
                          ForMetaClass_t forMeta,
                          HasUpdateCallback_t hasUpdater) {
      assert(FieldLayout && "can't emit rodata for a category");

      // struct _class_ro_t {
      //   uint32_t flags;
      b.addInt32(unsigned(buildFlags(forMeta, hasUpdater)));

      //   uint32_t instanceStart;
      //   uint32_t instanceSize;
      // The runtime requires that the ivar offsets be initialized to
      // a valid layout of the ivars of this class, bounded by these
      // two values.  If the instanceSize of the superclass equals the
      // stored instanceStart of the subclass, the ivar offsets
      // will not be changed.
      Size instanceStart;
      Size instanceSize;
      if (forMeta) {
        assert(!hasUpdater);

        // sizeof(struct class_t)
        instanceSize = Size(5 * IGM.getPointerSize().getValue());
        // historical nonsense
        instanceStart = instanceSize;
      } else {
        instanceSize = FieldLayout->getSize();
        instanceStart = FieldLayout->getInstanceStart();
      }
      b.addInt32(instanceStart.getValue());
      b.addInt32(instanceSize.getValue());

      //   uint32_t reserved;  // only when building for 64bit targets
      if (IGM.getPointerAlignment().getValue() > 4) {
        assert(IGM.getPointerAlignment().getValue() == 8);
        b.addInt32(0);
      }

      // union {
      //     const uint8_t *IvarLayout;
      //     ClassMetadata *NonMetaClass;
      // };
      Optional<CanType> specializedGenericType;
      if ((specializedGenericType = getSpecializedGenericType()) && forMeta) {
        //     ClassMetadata *NonMetaClass;
        b.addBitCast(IGM.getAddrOfTypeMetadata(*specializedGenericType),
                     IGM.Int8PtrTy);
      } else {
        //     const uint8_t *IvarLayout;
        // GC/ARC layout.  TODO.
        b.addNullPointer(IGM.Int8PtrTy);
      }

      //   const char *name;
      // It is correct to use the same name for both class and metaclass.
      b.add(buildName());

      //   const method_list_t *baseMethods;
      emitAndAddMethodList(b,
                           forMeta ? MethodListKind::ClassMethods
                                   : MethodListKind::InstanceMethods,
                           llvm::GlobalVariable::InternalLinkage);

      //   const protocol_list_t *baseProtocols;
      // Apparently, this list is the same in the class and the metaclass.
      b.add(buildProtocolList(llvm::GlobalVariable::InternalLinkage));

      //   const ivar_list_t *ivars;
      if (forMeta) {
        b.addNullPointer(IGM.Int8PtrTy);
      } else {
        b.add(buildIvarList());
      }

      //   const uint8_t *weakIvarLayout;
      // More GC/ARC layout.  TODO.
      b.addNullPointer(IGM.Int8PtrTy);

      //   const property_list_t *baseProperties;
      b.add(buildPropertyList(forMeta, llvm::GlobalVariable::InternalLinkage));

      // If hasUpdater is true, the metadata update callback goes here.
      if (hasUpdater) {
        //   Class _Nullable (*metadataUpdateCallback)(Class _Nonnull cls,
        //                                             void * _Nullable arg);
        auto *impl = IGM.getAddrOfObjCMetadataUpdateFunction(getClass(),
                                                             NotForDefinition);
        const auto &schema =
          IGM.getOptions().PointerAuth.ObjCMethodListFunctionPointers;
        b.addSignedPointer(impl, schema, PointerAuthEntity());
      }

      // };
    }
    
    llvm::Constant *emitROData(ForMetaClass_t forMeta,
                               HasUpdateCallback_t hasUpdater) {
      ConstantInitBuilder builder(IGM);
      auto fields = builder.beginStruct();
      emitRODataFields(fields, forMeta, hasUpdater);
      
      auto dataSuffix = forMeta ? "_METACLASS_DATA_" : "_DATA_";
      
      // The rodata is constant if the object layout is known entirely
      // statically. Otherwise, the ObjC runtime may slide the InstanceSize
      // based on changing base class layout.
      return buildGlobalVariable(fields, dataSuffix,
                               /*const*/ forMeta || FieldLayout->isFixedSize(),
                               llvm::GlobalVariable::InternalLinkage);
    }

  private:
    /// If we should set the forbids associated objects on instances metadata
    /// flag.
    ///
    /// We currently do this on:
    ///
    /// * Actor classes.
    /// * classes marked with @_semantics("objc.forbidAssociatedObjects")
    ///   (for testing purposes)
    ///
    /// TODO: Expand this as appropriate over time.
    bool doesClassForbidAssociatedObjectsOnInstances() const {
      auto *clsDecl = getClass();

      // We ban this on actors without objc ancestry.
      if (clsDecl->isActor() && !clsDecl->checkAncestry(AncestryFlags::ObjC))
        return true;

      // Otherwise, we only do it if our special semantics attribute is on the
      // relevant class. This is for testing purposes.
      if (clsDecl->hasSemanticsAttr(semantics::OBJC_FORBID_ASSOCIATED_OBJECTS))
        return true;

      // TODO: Add new cases here as appropriate over time.
      return false;
    }

    ObjCClassFlags buildFlags(ForMetaClass_t forMeta,
                              HasUpdateCallback_t hasUpdater) {
      ObjCClassFlags flags = ObjCClassFlags::CompiledByARC;

      // Mark metaclasses as appropriate.
      if (forMeta) {
        assert(!hasUpdater);
        flags |= ObjCClassFlags::Meta;

      // Non-metaclasses need us to record things whether primitive
      // construction/destructor is trivial.
      } else if (HasNonTrivialDestructor || HasNonTrivialConstructor) {
        flags |= ObjCClassFlags::HasCXXStructors;
        if (!HasNonTrivialConstructor)
          flags |= ObjCClassFlags::HasCXXDestructorOnly;
      }

      if (hasUpdater)
        flags |= ObjCClassFlags::HasMetadataUpdateCallback;

      // If we know that our class does not support having associated objects
      // placed upon instances, set the forbid associated object flag.
      if (doesClassForbidAssociatedObjectsOnInstances())
        flags |= ObjCClassFlags::ForbidsAssociatedObjects;

      // FIXME: set ObjCClassFlags::Hidden when appropriate
      return flags;
    }

    llvm::Constant *buildName() {
      if (Name) return Name;

      // If the class is generic, we'll instantiate its name at runtime.
      if (getClass()->isGenericContext()) {
        Name = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
        return Name;
      }
      
      llvm::SmallString<64> buffer;
      Name = IGM.getAddrOfGlobalString(getClass()->getObjCRuntimeName(buffer));
      return Name;
    }

    llvm::Constant *null() {
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }

    /*** Methods ***********************************************************/

  public:
    /// Methods need to be collected into the appropriate methods list.
    void visitFuncDecl(FuncDecl *method) {
      if (!isBuildingProtocol() &&
          !requiresObjCMethodDescriptor(method)) return;
      
      // getters and setters funcdecls will be handled by their parent
      // var/subscript.
      if (isa<AccessorDecl>(method)) return;

      // Don't emit getters/setters for @NSManaged methods.
      if (method->getAttrs().hasAttribute<NSManagedAttr>()) return;

      getMethodList(method).push_back(method);
    }

    /// Constructors need to be collected into the appropriate methods list.
    void visitConstructorDecl(ConstructorDecl *constructor) {
      if (!isBuildingProtocol() &&
          !requiresObjCMethodDescriptor(constructor)) return;
      getMethodList(constructor).push_back(constructor);
    }

    /// Determine whether the given destructor has an Objective-C
    /// definition.
    bool hasObjCDeallocDefinition(DestructorDecl *destructor) {
      // If we have the destructor body, we know whether SILGen
      // generated a -dealloc body.
      if (auto braceStmt = destructor->getBody())
        return !braceStmt->empty();

      // We don't have a destructor body, so hunt for the SIL function
      // for it.
      auto dtorRef = SILDeclRef(destructor, SILDeclRef::Kind::Deallocator)
        .asForeign();
      if (auto silFn = IGM.getSILModule().lookUpFunction(dtorRef))
        return silFn->isDefinition();

      // The Objective-C thunk was never even declared, so it is not defined.
      return false;
    }

    /// Destructors need to be collected into the instance methods
    /// list 
    void visitDestructorDecl(DestructorDecl *destructor) {
      auto classDecl = cast<ClassDecl>(destructor->getDeclContext()->getImplementedObjCContext());
      if (Lowering::usesObjCAllocator(classDecl) &&
          hasObjCDeallocDefinition(destructor)) {
        InstanceMethods.push_back(destructor);
      }
    }

    void visitMissingDecl(MissingDecl *missing) {
      llvm_unreachable("missing decl in IRGen");
    }

    void visitMissingMemberDecl(MissingMemberDecl *placeholder) {
      llvm_unreachable("should not IRGen classes with missing members");
    }

    void addIVarInitializer() {
      if (auto fn = IGM.getAddrOfIVarInitDestroy(getClass(),
                                                 /*destroy*/ false,
                                                 /*isForeign=*/ true,
                                                 NotForDefinition)) {
        InstanceMethods.push_back(MethodDescriptor::getIVarInitializer(*fn));
        HasNonTrivialConstructor = true;
      }
    }

    void addIVarDestroyer() {
      if (auto fn = IGM.getAddrOfIVarInitDestroy(getClass(),
                                                 /*destroy*/ true,
                                                 /*isForeign=*/ true,
                                                 NotForDefinition)) {
        InstanceMethods.push_back(MethodDescriptor::getIVarDestroyer(*fn));
        HasNonTrivialDestructor = true;
      }
    }

    void buildMethod(ConstantArrayBuilder &descriptors,
                     MethodDescriptor descriptor) {
      switch (descriptor.getKind()) {
      case MethodDescriptor::Kind::Method:
        return buildMethod(descriptors, descriptor.getMethod());
      case MethodDescriptor::Kind::IVarInitializer:
        emitObjCIVarInitDestroyDescriptor(IGM, descriptors, getClass(),
                                          descriptor.getImpl(), false);
        return;
      case MethodDescriptor::Kind::IVarDestroyer:
        emitObjCIVarInitDestroyDescriptor(IGM, descriptors, getClass(),
                                          descriptor.getImpl(), true);
        return;
      }
      llvm_unreachable("bad method descriptor kind");
    }

    void buildMethod(ConstantArrayBuilder &descriptors,
                     AbstractFunctionDecl *method) {
      if (Lowering::shouldSkipLowering(method))
        return;

      auto accessor = dyn_cast<AccessorDecl>(method);
      if (!accessor)
        return emitObjCMethodDescriptor(IGM, descriptors, method);

      switch (accessor->getAccessorKind()) {
      case AccessorKind::Get:
        return emitObjCGetterDescriptor(IGM, descriptors,
                                        accessor->getStorage());

      case AccessorKind::Set:
        return emitObjCSetterDescriptor(IGM, descriptors,
                                        accessor->getStorage());

#define OBJC_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
      case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
        llvm_unreachable("shouldn't be trying to build this accessor");
      }
      llvm_unreachable("bad accessor kind");
    }

  private:
    StringRef chooseNamePrefix(StringRef forClass,
                               StringRef forCategory,
                               StringRef forProtocol) {
      if (isBuildingCategory())
        return forCategory;
      if (isBuildingClass())
        return forClass;
      if (isBuildingProtocol())
        return forProtocol;
      
      llvm_unreachable("not a class, category, or protocol?!");
    }
    

    enum class MethodListKind : uint8_t {
      ClassMethods,
      InstanceMethods,
      OptionalClassMethods,
      OptionalInstanceMethods
    };

    /// Emit the method list and add the pointer to the `builder`.
    void emitAndAddMethodList(ConstantInitBuilder::StructBuilder &builder,
                              MethodListKind kind,
                              llvm::GlobalValue::LinkageTypes linkage) {
      ArrayRef<MethodDescriptor> methods;
      StringRef namePrefix;
      switch (kind) {
      case MethodListKind::ClassMethods:
        methods = ClassMethods;
        namePrefix = chooseNamePrefix("_CLASS_METHODS_",
                                      "_CATEGORY_CLASS_METHODS_",
                                      "_PROTOCOL_CLASS_METHODS_");
        break;
      case MethodListKind::InstanceMethods:
        methods = InstanceMethods;
        namePrefix = chooseNamePrefix("_INSTANCE_METHODS_",
                                      "_CATEGORY_INSTANCE_METHODS_",
                                      "_PROTOCOL_INSTANCE_METHODS_");
        break;
      case MethodListKind::OptionalClassMethods:
        methods = OptClassMethods;
        namePrefix = "_PROTOCOL_CLASS_METHODS_OPT_";
        break;
      case MethodListKind::OptionalInstanceMethods:
        methods = OptInstanceMethods;
        namePrefix = "_PROTOCOL_INSTANCE_METHODS_OPT_";
        break;
      }
      llvm::Constant *methodListPtr =
          buildMethodList(methods, namePrefix, linkage);
      builder.add(methodListPtr);
    }

    llvm::Constant *buildOptExtendedMethodTypes() {
      assert(isBuildingProtocol());

      ConstantInitBuilder builder(IGM);
      auto array = builder.beginArray();

      buildExtMethodTypes(array, InstanceMethods);
      buildExtMethodTypes(array, ClassMethods);
      buildExtMethodTypes(array, OptInstanceMethods);
      buildExtMethodTypes(array, OptClassMethods);

      if (array.empty()) {
        array.abandon();
        return null();
      }

      return buildGlobalVariable(array, "_PROTOCOL_METHOD_TYPES_",
                                 /*const*/ true,
                                 llvm::GlobalVariable::WeakAnyLinkage);
    }

    void buildExtMethodTypes(ConstantArrayBuilder &array,
                             ArrayRef<MethodDescriptor> methods) {
      assert(isBuildingProtocol());

      for (auto descriptor : methods) {
        assert(descriptor.getKind() == MethodDescriptor::Kind::Method &&
               "cannot emit descriptor for non-method");
        auto method = descriptor.getMethod();
        array.add(getMethodTypeExtendedEncoding(IGM, method));
      }
    }

    /// struct method_list_t {
    ///   uint32_t entsize; // runtime uses low bits for its own purposes
    ///   uint32_t count;
    ///   method_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildMethodList(ArrayRef<MethodDescriptor> methods,
                                    StringRef name,
                                    llvm::GlobalValue::LinkageTypes linkage) {
      return buildOptionalList(
          methods, 3 * IGM.getPointerSize(), name,
          /*isConst*/ false, linkage,
          [&](ConstantArrayBuilder &descriptors, MethodDescriptor descriptor) {
            buildMethod(descriptors, descriptor);
          });
    }

    /*** Protocols *********************************************************/

    /// typedef uintptr_t protocol_ref_t;  // protocol_t*, but unremapped
    llvm::Constant *buildProtocolRef(ProtocolDecl *protocol) {
      assert(protocol->isObjC());
      return IGM.getAddrOfObjCProtocolRecord(protocol, NotForDefinition);
    }
    
    /// struct protocol_list_t {
    ///   uintptr_t count;
    ///   protocol_ref_t[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildProtocolList(llvm::GlobalValue::LinkageTypes linkage) {
      return buildOptionalList(
          Protocols, Size(0),
          chooseNamePrefix("_PROTOCOLS_", "_CATEGORY_PROTOCOLS_",
                           "_PROTOCOL_PROTOCOLS_"),
          /*isConst*/ true, linkage,
          [&](ConstantArrayBuilder &descriptors, ProtocolDecl *protocol) {
            buildProtocol(descriptors, protocol);
          });
    }

    void buildProtocol(ConstantArrayBuilder &array, ProtocolDecl *protocol) {
      array.add(buildProtocolRef(protocol));
    }

    /*** Ivars *************************************************************/

  public:
    /// Variables might be stored or computed.
    void visitVarDecl(VarDecl *var) {
      if (var->hasStorage() && !var->isStatic())
        visitStoredVar(var);
      else
        visitProperty(var);
    }

  private:
    /// Ivars need to be collected in the ivars list, and they also
    /// affect flags.
    void visitStoredVar(VarDecl *var) {
      // FIXME: how to handle ivar extensions in categories?
      if (!FieldLayout)
        return;

      Ivars.push_back(var);

      // Build property accessors for the ivar if necessary.
      visitProperty(var);
    }

    /// struct ivar_t {
    ///   uintptr_t *offset;
    ///   const char *name;
    ///   const char *type;
    ///   uint32_t alignment;    // actually the log2 of the alignment
    ///   uint32_t size;
    /// };
    void buildIvar(ConstantArrayBuilder &ivars, Field field) {
      assert(FieldLayout && "can't build ivar for category");

      // For now, we never try to emit specialized versions of the
      // metadata statically, so compute the field layout using the
      // originally-declared type.
      auto pair = FieldLayout->getFieldAccessAndElement(field);

      llvm::Constant *offsetPtr;
      switch (pair.first) {
      case FieldAccess::ConstantDirect:
      case FieldAccess::NonConstantDirect: {
        // Default actor storage doesn't get an ivar access variable.
        if (field.getKind() == Field::DefaultActorStorage) {
          offsetPtr = nullptr;
          break;
        } else if (field.getKind() == Field::NonDefaultDistributedActorStorage) {
          offsetPtr = nullptr;
          break;
        }

        // Otherwise, we should have a normal stored property.
        auto ivar = field.getVarDecl();
        // If the field offset is fixed relative to the start of the superclass,
        // reference the global from the ivar metadata so that the Objective-C
        // runtime will slide it down.
        auto offsetAddr = IGM.getAddrOfFieldOffset(ivar, NotForDefinition);
        offsetPtr = cast<llvm::Constant>(offsetAddr.getAddress());
        break;
      }
      case FieldAccess::ConstantIndirect:
        // Otherwise, swift_initClassMetadata() will point the Objective-C
        // runtime into the field offset vector of the instantiated metadata.
        offsetPtr = nullptr;
        break;
      }

      StringRef name = field.getName();
      const TypeInfo &storageTI = pair.second.getType();
      auto fields = ivars.beginStruct();

      if (offsetPtr)
        fields.add(offsetPtr);
      else
        fields.addNullPointer(IGM.IntPtrTy->getPointerTo());

      // TODO: clang puts this in __TEXT,__objc_methname,cstring_literals
      fields.add(IGM.getAddrOfGlobalString(name));

      // TODO: clang puts this in __TEXT,__objc_methtype,cstring_literals
      fields.add(IGM.getAddrOfGlobalString(""));

      Size size;
      Alignment alignment;
      if (auto fixedTI = dyn_cast<FixedTypeInfo>(&storageTI)) {
        size = fixedTI->getFixedSize();
        alignment = fixedTI->getFixedAlignment();
      } else {
        size = Size(0);
        alignment = Alignment(1);
      }

      // If the size is larger than we can represent in 32-bits,
      // complain about the unimplementable ivar.
      if (uint32_t(size.getValue()) != size.getValue()) {
        IGM.error(field.getVarDecl()->getLoc(),
                  "ivar size (" + Twine(size.getValue()) +
                  " bytes) overflows Objective-C ivar layout");
        size = Size(0);
      }
      fields.addInt32(alignment.log2());
      fields.addInt32(size.getValue());

      fields.finishAndAddTo(ivars);
    }

    /// struct ivar_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   ivar_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildIvarList() {
      Size eltSize = 3 * IGM.getPointerSize() + Size(8);
      return buildOptionalList(
          Ivars, eltSize, "_IVARS_",
          /*constant*/ true, llvm::GlobalVariable::InternalLinkage,
          [&](ConstantArrayBuilder &descriptors, Field field) {
            buildIvar(descriptors, field);
          });
    }

    /*** Properties ********************************************************/

    /// Properties need to be collected in the properties list.
    void visitProperty(VarDecl *var) {
      if (requiresObjCPropertyDescriptor(IGM, var)) {
        if (var->isStatic()) {
          ClassProperties.push_back(var);
        } else {
          InstanceProperties.push_back(var);
        }

        // Don't emit descriptors for properties without accessors.
        auto getter = var->getOpaqueAccessor(AccessorKind::Get);
        if (!getter)
          return;

        // Don't emit getter/setter descriptors for @NSManaged properties.
        if (var->getAttrs().hasAttribute<NSManagedAttr>())
          return;

        auto &methods = getMethodList(var);
        methods.push_back(getter);

        if (auto setter = var->getOpaqueAccessor(AccessorKind::Set))
          methods.push_back(setter);
      }
    }
    
    /// Build the property attribute string for a property decl.
    void buildPropertyAttributes(VarDecl *prop, SmallVectorImpl<char> &out) {
      llvm::raw_svector_ostream outs(out);

      auto propTy = prop->getValueInterfaceType();
      auto propDC = prop->getDeclContext();

      // Emit the type encoding for the property.
      outs << 'T';
      
      std::string typeEnc;
      getObjCEncodingForPropertyType(IGM, prop, typeEnc);
      outs << typeEnc;
      
      // Emit other attributes.

      // All Swift properties are (nonatomic).
      outs << ",N";
      
      // @NSManaged properties are @dynamic.
      if (prop->getAttrs().hasAttribute<NSManagedAttr>())
        outs << ",D";
      
      auto isObject = propDC->mapTypeIntoContext(propTy)
          ->hasRetainablePointerRepresentation();
      auto hasObjectEncoding = typeEnc[0] == '@';
      
      // Determine the assignment semantics.
      // Get-only properties are (readonly).
      if (!prop->isSettable(propDC))
        outs << ",R";
      // Weak and Unowned properties are (weak).
      else if (prop->getAttrs().hasAttribute<ReferenceOwnershipAttr>())
        outs << ",W";
      // If the property is @NSCopying, or is bridged to a value class, the
      // property is (copy).
      else if (prop->getAttrs().hasAttribute<NSCopyingAttr>()
               || (hasObjectEncoding && !isObject))
        outs << ",C";
      // If it's of a managed object type, it is (retain).
      else if (isObject)
        outs << ",&";
      // Otherwise, the property is of a value type, so it is
      // the default (assign).
      else
        (void)0;
      
      // If the property is an instance property and has storage, and meanwhile
      // its type is trivially representable in ObjC, emit the ivar name last.
      bool isTriviallyRepresentable =
          propTy->isTriviallyRepresentableIn(ForeignLanguage::ObjectiveC,
                                             propDC);
      if (!prop->isStatic() && prop->hasStorage() && isTriviallyRepresentable)
        outs << ",V" << prop->getName();
    }

    /// struct property_t {
    ///   const char *name;
    ///   const char *attributes;
    /// };
    void buildProperty(ConstantArrayBuilder &properties, VarDecl *prop) {
      llvm::SmallString<16> propertyAttributes;
      buildPropertyAttributes(prop, propertyAttributes);
      
      auto fields = properties.beginStruct();
      fields.add(IGM.getAddrOfGlobalString(prop->getObjCPropertyName().str()));
      fields.add(IGM.getAddrOfGlobalString(propertyAttributes));
      fields.finishAndAddTo(properties);
    }

    /// struct property_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   property_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildPropertyList(ForMetaClass_t classOrMeta,
                                      llvm::GlobalValue::LinkageTypes linkage) {
      if (classOrMeta == ForClass) {
        return buildPropertyList(InstanceProperties,
                                 chooseNamePrefix("_PROPERTIES_",
                                                  "_CATEGORY_PROPERTIES_",
                                                  "_PROTOCOL_PROPERTIES_"),
                                 linkage);
      }

      // Older OSs' libobjcs can't handle class property data.
      if ((IGM.Triple.isMacOSX() && IGM.Triple.isMacOSXVersionLT(10, 11)) ||
          (IGM.Triple.isiOS() && IGM.Triple.isOSVersionLT(9))) {
        return null();
      }

      return buildPropertyList(ClassProperties,
                               chooseNamePrefix("_CLASS_PROPERTIES_",
                                                "_CATEGORY_CLASS_PROPERTIES_",
                                                "_PROTOCOL_CLASS_PROPERTIES_"),
                               linkage);
    }

    llvm::Constant *buildPropertyList(ArrayRef<VarDecl *> properties,
                                      StringRef namePrefix,
                                      llvm::GlobalValue::LinkageTypes linkage) {
      Size eltSize = 2 * IGM.getPointerSize();
      return buildOptionalList(
          properties, eltSize, namePrefix,
          /*constant*/ true, linkage,
          [&](ConstantArrayBuilder &descriptors, VarDecl *property) {
            buildProperty(descriptors, property);
          });
    }

    /*** General ***********************************************************/

    /// Build a list structure from the given array of objects.
    /// If the array is empty, use null.  The assumption is that every
    /// initializer has the same size.
    ///
    /// \param optionalEltSize - if non-zero, a size which needs
    ///   to be placed in the list header
    template <class C, class Fn>
    llvm::Constant *buildOptionalList(const C &objects, Size optionalEltSize,
                                      StringRef nameBase, bool isConst,
                                      llvm::GlobalValue::LinkageTypes linkage,
                                      Fn &&buildElement) {
      if (objects.empty())
        return null();

      ConstantInitBuilder builder(IGM);
      auto fields = builder.beginStruct();

      llvm::IntegerType *countType;
      // In all of the foo_list_t structs, either:
      //   - there's a 32-bit entry size and a 32-bit count or
      //   - there's no entry size and a uintptr_t count.
      if (!optionalEltSize.isZero()) {
        fields.addInt32(optionalEltSize.getValue());
        countType = IGM.Int32Ty;
      } else {
        countType = IGM.IntPtrTy;
      }
      auto countPosition = fields.addPlaceholder();

      auto array = fields.beginArray();
      for (auto &element : objects) {
        buildElement(array, element);
      }

      // If we didn't actually make anything, declare that we're done.
      if (array.empty()) {
        array.abandon();
        fields.abandon();
        return null();
      }

      // Otherwise, remember the size of the array and fill the
      // placeholder with it.
      auto count = array.size();
      array.finishAndAddTo(fields);

      fields.fillPlaceholderWithInt(countPosition, countType, count);

      return buildGlobalVariable(fields, nameBase, isConst, linkage);
    }

    /// Get the name of the class or protocol to mangle into the ObjC symbol
    /// name.
    StringRef getEntityName(llvm::SmallVectorImpl<char> &buffer) const {
      if (auto prespecialization = getSpecializedGenericType()) {
        buffer.clear();
        llvm::raw_svector_ostream os(buffer);
        os << LinkEntity::forTypeMetadata(*prespecialization,
                                          TypeMetadataAddress::FullMetadata)
                  .mangleAsString();
        return os.str();
      }

      if (auto theClass = getClass()) {
        return theClass->getObjCRuntimeName(buffer);
      }

      if (auto theProtocol = getProtocol()) {
        return theProtocol->getObjCRuntimeName(buffer);
      }

      llvm_unreachable("not a class or protocol?!");
    }

    /// Build a private global variable as a structure containing the
    /// given fields.
    template <class B>
    llvm::Constant *
    buildGlobalVariable(B &fields, StringRef nameBase, bool isConst,
                        llvm::GlobalValue::LinkageTypes linkage) {
      llvm::SmallString<64> nameBuffer;
      auto var =
        fields.finishAndCreateGlobal(Twine(nameBase) 
                                      + getEntityName(nameBuffer)
                                      + (TheExtension
                                           ? Twine("_$_") + CategoryName.str()
                                           : Twine()),
                                     IGM.getPointerAlignment(),
                                     /*constant*/ true,
                                     linkage);
      if (linkage == llvm::GlobalVariable::WeakAnyLinkage) {
        var->setVisibility(llvm::GlobalValue::HiddenVisibility);
      }

      switch (IGM.TargetInfo.OutputObjectFormat) {
      case llvm::Triple::MachO:
        var->setSection(isConst ? "__DATA, __objc_const"
                                : "__DATA, __objc_data");
        break;
      case llvm::Triple::XCOFF:
      case llvm::Triple::COFF:
        var->setSection(".data");
        break;
      case llvm::Triple::ELF:
      case llvm::Triple::Wasm:
        var->setSection(".data");
        break;
      case llvm::Triple::DXContainer:
      case llvm::Triple::GOFF:
      case llvm::Triple::SPIRV:
      case llvm::Triple::UnknownObjectFormat:
        llvm_unreachable("Don't know how to emit private global constants for "
                         "the selected object format.");
      }
      return var;
    }

  public:
    /// Member types don't get any representation.
    /// Maybe this should change for reflection purposes?
    void visitTypeDecl(TypeDecl *type) {}

    /// Pattern-bindings don't require anything special as long as
    /// these initializations are performed in the constructor, not
    /// .cxx_construct.
    void visitPatternBindingDecl(PatternBindingDecl *binding) {}

    /// Subscripts should probably be collected in extended metadata.
    void visitSubscriptDecl(SubscriptDecl *subscript) {
      if (!requiresObjCSubscriptDescriptor(IGM, subscript)) return;

      auto getter = subscript->getOpaqueAccessor(AccessorKind::Get);
      if (!getter) return;

      auto &methods = getMethodList(subscript);
      methods.push_back(getter);

      if (auto setter = subscript->getOpaqueAccessor(AccessorKind::Set))
        methods.push_back(setter);
    }
    
    SWIFT_DEBUG_DUMP {
      dump(llvm::errs());
      llvm::errs() << "\n";
    }
    
    void dump(llvm::raw_ostream &os) const {
      os << "(class_data_builder";
      
      if (isBuildingClass())
        os << " for_class";
      if (isBuildingCategory())
        os << " for_category";
      if (isBuildingProtocol())
        os << " for_protocol";
      
      if (auto cd = getClass()) {
        os << " class=";
        cd->dumpRef(os);
      }
      if (auto pd = getProtocol()) {
        os << " protocol=";
        pd->dumpRef(os);
      }
      if (auto ty = getSpecializedGenericType()) {
        os << " specialized=";
        ty->print(os);
      }
      if (TheExtension) {
        os << " extension=";
        TheExtension->getExtendedTypeRepr()->print(os);
        os << "@";
        TheExtension->getLoc().print(os, IGM.Context.SourceMgr);
      }
      if (auto name = getObjCImplCategoryName()) {
        os << " objc_impl_category_name=" << *name;
      }
      
      if (HasNonTrivialConstructor)
        os << " has_non_trivial_constructor";
      if (HasNonTrivialDestructor)
        os << " has_non_trivial_destructor";
      
      if (!CategoryName.empty())
        os << " category_name=" << CategoryName;
      
      os << "\n  (ivars";
      for (auto field : Ivars) {
        os << "\n    (";
        switch (field.getKind()) {
          case Field::Kind::Var:
            os << "var ";
            field.getVarDecl()->dumpRef(os);
            break;
          case Field::Kind::MissingMember:
            os << "missing_member";
            break;
          case Field::Kind::DefaultActorStorage:
            os << "default_actor_storage";
            break;
          case Field::Kind::NonDefaultDistributedActorStorage:
            os << "non_default_distributed_actor_storage";
            break;
        }
        os << ")";
      }
      os << ")";
      
      auto printMethodList =
          [&](StringRef label,
              const SmallVectorImpl<MethodDescriptor> &methods) {
        if (methods.empty()) return;
        
        os << "\n  (" << label;
        for (auto method : methods) {
          os << "\n    (";
          switch (method.getKind()) {
            case MethodDescriptor::Kind::Method:
              os << "method ";
              method.getMethod()->dumpRef(os);
              break;
              
            case MethodDescriptor::Kind::IVarInitializer:
              os << "ivar_initializer";
              method.getImpl()->printAsOperand(os);
              break;
              
            case MethodDescriptor::Kind::IVarDestroyer:
              os << "ivar_destroyer";
              method.getImpl()->printAsOperand(os);
              break;
          }
          os << ")";
        }
        os << ")";
      };
      
      printMethodList("instance_methods", InstanceMethods);
      printMethodList("class_methods", ClassMethods);
      printMethodList("opt_instance_methods", OptInstanceMethods);
      printMethodList("opt_class_methods", OptClassMethods);
      
      os << "\n  (protocols";
      for (auto pd : Protocols) {
        os << "\n    (protocol ";
        pd->dumpRef(os);
        os << ")";
      }
      os << ")";
      
      auto printPropertyList = [&](StringRef label,
                                   const SmallVectorImpl<VarDecl *> &props) {
        if (props.empty()) return;
        
        os << "\n  (" << label;
        for (auto prop : props) {
          os << "\n    (property ";
          prop->dumpRef(os);
          os << ")";
        }
        os << ")";
      };
      
      printPropertyList("instance_properties", InstanceProperties);
      printPropertyList("class_properties", ClassProperties);
      
      os << ")";
    }
  };
} // end anonymous namespace

static llvm::Function *emitObjCMetadataUpdateFunction(IRGenModule &IGM,
                                                      ClassDecl *D) {
  llvm::Function *f =
    IGM.getAddrOfObjCMetadataUpdateFunction(D, ForDefinition);
  f->setAttributes(IGM.constructInitialAttributes());
  IGM.setColocateMetadataSection(f);

  IRGenFunction IGF(IGM, f);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, f);

  // Our parameters are the metadata pointer, and an argument for
  // future use. We just ignore them.
  Explosion params = IGF.collectParameters();
  (void) params.claimAll();

  // Just directly call our metadata accessor. This should actually
  // return the same metadata; the Objective-C runtime enforces this.
  auto type = D->getDeclaredType()->getCanonicalType();
  auto *metadata = IGF.emitTypeMetadataRef(type,
                                           MetadataState::Complete)
    .getMetadata();
  IGF.Builder.CreateRet(
    IGF.Builder.CreateBitCast(metadata,
                              IGM.ObjCClassPtrTy));

  return f;
}

/// We emit Objective-C class stubs for non-generic classes with resilient
/// ancestry. This lets us attach categories to the class even though it
/// does not have statically-emitted metadata.
bool IRGenModule::hasObjCResilientClassStub(ClassDecl *D) {
#ifndef NDEBUG
  auto strategy = getClassMetadataStrategy(D);
  assert(strategy == ClassMetadataStrategy::Resilient ||
         strategy == ClassMetadataStrategy::Singleton);
#endif

  return ObjCInterop && !D->isGenericContext();
}

llvm::Constant *IRGenModule::emitObjCResilientClassStub(
    ClassDecl *D, bool isPublic) {
  ConstantInitBuilder builder(*this);
  auto fields = builder.beginStruct(ObjCFullResilientClassStubTy);
  fields.addInt(SizeTy, 0); // reserved
  fields.addInt(SizeTy, 1); // isa
  auto *impl = getAddrOfObjCMetadataUpdateFunction(D, NotForDefinition);
  const auto &schema =
      getOptions().PointerAuth.ResilientClassStubInitCallbacks;
  fields.addSignedPointer(impl, schema, PointerAuthEntity()); // callback

  auto init = fields.finishAndCreateFuture();

  // Define the full stub. This is a private symbol.
  LinkEntity entity = LinkEntity::forObjCResilientClassStub(
      D, TypeMetadataAddress::FullMetadata);
  auto fullObjCStub = cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, init, DebugTypeInfo()));

  // Emit the metadata update function referenced above.
  emitObjCMetadataUpdateFunction(*this, D);

  // Apply the offset.
  auto *objcStub = llvm::ConstantExpr::getBitCast(fullObjCStub, Int8PtrTy);
  objcStub = llvm::ConstantExpr::getInBoundsGetElementPtr(
      Int8Ty, objcStub, getSize(getPointerSize()));
  objcStub = llvm::ConstantExpr::getPointerCast(objcStub,
      ObjCResilientClassStubTy->getPointerTo());

  if (isPublic) {
    entity = LinkEntity::forObjCResilientClassStub(
        D, TypeMetadataAddress::AddressPoint);
    defineAlias(entity, objcStub, ObjCResilientClassStubTy);
  }

  return objcStub;
}

static llvm::Constant *doEmitClassPrivateData(
    IRGenModule &IGM,
    TaggedUnion<ClassDecl *, std::pair<ClassDecl *, CanType>> classUnion) {
  assert(IGM.ObjCInterop && "emitting RO-data outside of interop mode");

  ClassDecl *cls;

  if (auto *theClass = classUnion.dyn_cast<ClassDecl *>()) {
    cls = *theClass;
  } else {
    auto pair = classUnion.get<std::pair<ClassDecl *, CanType>>();
    cls = pair.first;
  }

  SILType selfType = getSelfType(cls);
  auto &classTI = IGM.getTypeInfo(selfType).as<ClassTypeInfo>();

  // FIXME: For now, always use the fragile layout when emitting metadata.
  auto &fieldLayout = classTI.getClassLayout(IGM, selfType,
                                             /*forBackwardDeployment=*/true);
  ClassDataBuilder builder(IGM, classUnion, fieldLayout);

  // First, build the metaclass object.
  builder.buildMetaclassStub();

  HasUpdateCallback_t hasUpdater = DoesNotHaveUpdateCallback;

  switch (IGM.getClassMetadataStrategy(cls)) {
  case ClassMetadataStrategy::Resilient:
  case ClassMetadataStrategy::Singleton:
  case ClassMetadataStrategy::Fixed:
    break;
  case ClassMetadataStrategy::Update:
  case ClassMetadataStrategy::FixedOrUpdate:
    hasUpdater = HasUpdateCallback;
    emitObjCMetadataUpdateFunction(IGM, cls);
    break;
  }

  // Then build the class RO-data.
  return builder.emitROData(ForClass, hasUpdater);
}

llvm::Constant *irgen::emitSpecializedGenericClassPrivateData(
    IRGenModule &IGM, ClassDecl *theClass, CanType theType) {
  assert(theType->getClassOrBoundGenericClass() == theClass);
  assert(theClass->getGenericEnvironment());
  Type ty = theType;
  PrettyStackTraceType stackTraceRAII(theClass->getASTContext(),
                                      "emitting ObjC metadata for", ty);
  return doEmitClassPrivateData(
      IGM, TaggedUnion<ClassDecl *, std::pair<ClassDecl *, CanType>>(
               std::make_pair(theClass, theType)));
}

/// Emit the private data (RO-data) associated with a class.
llvm::Constant *irgen::emitClassPrivateData(IRGenModule &IGM, ClassDecl *cls) {
  PrettyStackTraceDecl stackTraceRAII("emitting ObjC metadata for", cls);
  return doEmitClassPrivateData(
      IGM, TaggedUnion<ClassDecl *, std::pair<ClassDecl *, CanType>>(cls));
}

std::pair<Size, Size>
irgen::emitClassPrivateDataFields(IRGenModule &IGM,
                                  ConstantStructBuilder &init,
                                  ClassDecl *cls) {
  assert(IGM.ObjCInterop && "emitting RO-data outside of interop mode");
  PrettyStackTraceDecl stackTraceRAII("emitting ObjC metadata for", cls);

  // This should only be used with generic classes.
  assert(cls->isGenericContext());

  SILType selfType = getSelfType(cls);
  auto &classTI = IGM.getTypeInfo(selfType).as<ClassTypeInfo>();

  // FIXME: For now, always use the fragile layout when emitting metadata.
  auto &fieldLayout = classTI.getClassLayout(IGM, selfType,
                                             /*forBackwardDeployment=*/true);

  ClassDataBuilder builder(IGM, cls, fieldLayout);

  Size startOfClassRO = init.getNextOffsetFromGlobal();
  assert(startOfClassRO.isMultipleOf(IGM.getPointerSize()));
  {
    auto classRO = init.beginStruct();

    // Note: an update callback is only ever used with the in-place
    // initialization pattern, which precludes generic classes.
    builder.emitRODataFields(classRO,
                             ForClass,
                             DoesNotHaveUpdateCallback);
    classRO.finishAndAddTo(init);
  }

  Size startOfMetaclassRO = init.getNextOffsetFromGlobal();
  assert(startOfMetaclassRO.isMultipleOf(IGM.getPointerSize()));
  {
    auto classRO = init.beginStruct();
    builder.emitRODataFields(classRO, ForMetaClass, DoesNotHaveUpdateCallback);
    classRO.finishAndAddTo(init);
  }

  return std::make_pair(startOfClassRO, startOfMetaclassRO);
}

/// Emit the metadata for an ObjC category.
llvm::Constant *irgen::emitCategoryData(IRGenModule &IGM,
                                        ExtensionDecl *ext) {
  assert(IGM.ObjCInterop && "emitting RO-data outside of interop mode");
  ClassDecl *cls = ext->getSelfClassDecl();
  assert(cls && "generating category metadata for a non-class extension");
  
  PrettyStackTraceDecl stackTraceRAII("emitting ObjC metadata for", ext);
  ClassDataBuilder builder(IGM, cls, ext);
  return builder.emitCategory();
}
  
/// Emit the metadata for an ObjC protocol.
llvm::Constant *irgen::emitObjCProtocolData(IRGenModule &IGM,
                                            ProtocolDecl *proto) {
  assert(proto->isObjC() && "not an objc protocol");
  PrettyStackTraceDecl stackTraceRAII("emitting ObjC metadata for", proto);

  // The linker on older deployment targets does not gracefully handle the
  // situation when both an objective c object and a swift object define the
  // protocol under the same symbol name.
  auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(IGM.Context);
  bool canUseClangEmission = deploymentAvailability.isContainedIn(
    IGM.Context.getSwift58Availability());

  if (llvm::Triple(IGM.Module.getTargetTriple()).isOSDarwin() &&
      canUseClangEmission) {
    // Use the clang to generate the protocol metadata if there is a clang node.
    if (auto clangDecl = proto->getClangDecl()) {
      if (auto objcMethodDecl = dyn_cast<clang::ObjCProtocolDecl>(clangDecl)) {
        return IGM.emitClangProtocolObject(objcMethodDecl);
      }
    }
  }
  ClassDataBuilder builder(IGM, proto);
  return builder.emitProtocol();
}

const TypeInfo *
TypeConverter::convertClassType(CanType type, ClassDecl *D) {
  llvm::StructType *ST = IGM.createNominalType(type);
  llvm::PointerType *irType = ST->getPointerTo();
  ReferenceCounting refcount = type->getReferenceCounting();
  
  SpareBitVector spareBits;
  
  // Classes known to be implemented in Swift can be assumed not to have tagged
  // pointer representations, so we can use spare bits for enum layout with
  // them. We can't make that assumption about imported ObjC types.
  if (D->hasClangNode() && IGM.TargetInfo.hasObjCTaggedPointers())
    spareBits.appendClearBits(IGM.getPointerSize().getValueInBits());
  else
    spareBits = IGM.getHeapObjectSpareBits();

  return new ClassTypeInfo(irType, IGM.getPointerSize(), std::move(spareBits),
                           IGM.getPointerAlignment(), D, refcount, ST);
}

/// Lazily declare a fake-looking class to represent an ObjC runtime base class.
ClassDecl *IRGenModule::getObjCRuntimeBaseClass(Identifier name,
                                                Identifier objcName) {
  auto found = SwiftRootClasses.find(name);
  if (found != SwiftRootClasses.end())
    return found->second;

  // Make a really fake-looking class.
  auto SwiftRootClass = new (Context) ClassDecl(SourceLoc(), name, SourceLoc(),
                                           ArrayRef<InheritedEntry>(),
                                           /*generics*/ nullptr,
                                           Context.TheBuiltinModule,
                                           /*isActor*/false);
  SwiftRootClass->setIsObjC(Context.LangOpts.EnableObjCInterop);
  SwiftRootClass->getAttrs().add(ObjCAttr::createNullary(Context, objcName,
    /*isNameImplicit=*/true));
  SwiftRootClass->setImplicit();
  SwiftRootClass->setAccess(AccessLevel::Open);
  
  SwiftRootClasses.insert({name, SwiftRootClass});
  return SwiftRootClass;
}

/// Lazily declare the ObjC runtime base class for a Swift root class.
ClassDecl *
IRGenModule::getObjCRuntimeBaseForSwiftRootClass(ClassDecl *theClass) {
  assert(!theClass->hasSuperclass() && "must pass a root class");
  
  Identifier name;
  // If the class declares its own ObjC runtime base, use it.
  if (auto baseAttr = theClass->getAttrs()
        .getAttribute<SwiftNativeObjCRuntimeBaseAttr>()) {
    name = baseAttr->BaseClassName;
  } else {
    // Otherwise, use the standard SwiftObject class.
    name = Context.Id_SwiftObject;
  }
  return getObjCRuntimeBaseClass(name, name);
}

/// Lazily declare the base class for a Swift class that inherits from
/// NSObject but uses native reference-counting.
ClassDecl *IRGenModule::getSwiftNativeNSObjectDecl() {
  Identifier name = Context.Id_SwiftNativeNSObject;
  return getObjCRuntimeBaseClass(name, name);
}

ClassDecl *irgen::getRootClassForMetaclass(IRGenModule &IGM, ClassDecl *C) {
  while (auto superclass = C->getSuperclassDecl())
    C = superclass;

  // If the formal root class is imported from Objective-C, then
  // we should use that.  For a class that's really implemented in
  // Objective-C, this is obviously right.  For a class that's
  // really implemented in Swift, but that we're importing via an
  // Objective-C interface, this would be wrong --- except such a
  // class can never be a formal root class, because a Swift class
  // without a formal superclass will actually be parented by
  // SwiftObject (or maybe eventually something else like it),
  // which will be visible in the Objective-C type system.
  if (C->hasClangNode()) return C;
  
  // FIXME: If the root class specifies its own runtime ObjC base class,
  // assume that base class ultimately inherits NSObject.
  if (C->getAttrs().hasAttribute<SwiftNativeObjCRuntimeBaseAttr>())
    return IGM.getObjCRuntimeBaseClass(
             IGM.Context.getSwiftId(KnownFoundationEntity::NSObject),
             IGM.Context.getIdentifier("NSObject"));

  return IGM.getObjCRuntimeBaseClass(IGM.Context.Id_SwiftObject,
                                     IGM.Context.Id_SwiftObject);
}

ClassDecl *
irgen::getSuperclassDeclForMetadata(IRGenModule &IGM, ClassDecl *C) {
  if (C->isNativeNSObjectSubclass()) {
    // When concurrency isn't available in the OS, use NSObject instead.
    if (!IGM.isConcurrencyAvailable()) {
      return IGM.getObjCRuntimeBaseClass(
          IGM.Context.getSwiftId(KnownFoundationEntity::NSObject),
          IGM.Context.getIdentifier("NSObject"));
    }

    return IGM.getSwiftNativeNSObjectDecl();
  }
  return C->getSuperclassDecl();
}

CanType irgen::getSuperclassForMetadata(IRGenModule &IGM, ClassDecl *C) {
  if (C->isNativeNSObjectSubclass()) {
    return getSuperclassDeclForMetadata(IGM, C)->getDeclaredInterfaceType()
                                               ->getCanonicalType();
  }
  if (auto superclass = C->getSuperclass())
    return superclass->getCanonicalType();
  return CanType();
}

CanType irgen::getSuperclassForMetadata(IRGenModule &IGM, CanType type,
                                        bool useArchetypes) {
  auto cls = type->getClassOrBoundGenericClass();
  if (cls->isNativeNSObjectSubclass()) {
    return getSuperclassDeclForMetadata(IGM, cls)->getDeclaredInterfaceType()
                                                 ->getCanonicalType();
  }
  if (auto superclass = type->getSuperclass(useArchetypes))
    return superclass->getCanonicalType();
  return CanType();
}

ClassMetadataStrategy
IRGenModule::getClassMetadataStrategy(const ClassDecl *theClass) {
  SILType selfType = getSelfType(theClass);
  auto &selfTI = getTypeInfo(selfType).as<ClassTypeInfo>();

  auto &resilientLayout = selfTI.getClassLayout(*this, selfType,
                                              /*forBackwardDeployment=*/false);

  if (resilientLayout.doesMetadataRequireRelocation())
    return ClassMetadataStrategy::Resilient;

  // On Windows, we want to force singleton metadata initialization, since
  // fixed class metadata emission requires an absolute global reference to the
  // Builtin.NativeObject value witness table in the runtime, which is something
  // the PE executable format does not support.
  if (IRGen.Opts.LazyInitializeClassMetadata)
    return ClassMetadataStrategy::Singleton;

  // If we have generic ancestry, we have to use the singleton pattern.
  if (resilientLayout.doesMetadataRequireInitialization())
    return ClassMetadataStrategy::Singleton;

  // If we have resiliently-sized fields, we might be able to use the
  // update pattern.
  if (resilientLayout.doesMetadataRequireUpdate()) {
      
    // FixedOrUpdate strategy does not work in JIT mode
    if (IRGen.Opts.UseJIT)
      return ClassMetadataStrategy::Singleton;
      
    // The update pattern only benefits us on platforms with an Objective-C
    // runtime, otherwise just use the singleton pattern.
    if (!Context.LangOpts.EnableObjCInterop)
      return ClassMetadataStrategy::Singleton;

    // If the Objective-C runtime is new enough, we can just use the update
    // pattern unconditionally.
    auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(Context);
    if (deploymentAvailability.isContainedIn(
          Context.getObjCMetadataUpdateCallbackAvailability()))
      return ClassMetadataStrategy::Update;

    // Otherwise, check if we have legacy type info for backward deployment.
    auto &fragileLayout = selfTI.getClassLayout(*this, selfType,
                                               /*forBackwardDeployment=*/true);

    // If we still have resiliently-sized fields even when using the legacy
    // type info, fall back to the singleton pattern.
    if (fragileLayout.doesMetadataRequireUpdate())
      return ClassMetadataStrategy::Singleton;

    // We're going to use the legacy type info on older Objective-C runtimes,
    // and the update callback on newer runtimes.
    return ClassMetadataStrategy::FixedOrUpdate;
  }

  return ClassMetadataStrategy::Fixed;
}

bool irgen::hasKnownSwiftMetadata(IRGenModule &IGM, CanType type) {
  // This needs to be kept up-to-date with getIsaEncodingForType.

  if (ClassDecl *theClass = type.getClassOrBoundGenericClass()) {
    return hasKnownSwiftMetadata(IGM, theClass);
  }

  if (auto archetype = dyn_cast<ArchetypeType>(type)) {
    if (auto superclass = archetype->getSuperclass()) {
      return hasKnownSwiftMetadata(IGM, superclass->getCanonicalType());
    }
  }

  // Class existentials, etc.
  return false;
}

/// Is the given class known to have Swift-compatible metadata?
bool irgen::hasKnownSwiftMetadata(IRGenModule &IGM, ClassDecl *theClass) {
  // Make sure that the fake declarations we make in getObjcRuntimeBaseClass
  // are treated this way.
  if (theClass->getModuleContext()->isBuiltinModule()) {
    assert(IGM.ObjCInterop);
    return false;
  }

  // For now, the fact that a declaration was not implemented in Swift
  // is enough to conclusively force us into a slower path.
  // Eventually we might have an attribute here or something based on
  // the deployment target.
  return theClass->hasKnownSwiftImplementation();
}

std::pair<llvm::Value *, llvm::Value *>
irgen::emitClassResilientInstanceSizeAndAlignMask(IRGenFunction &IGF,
                                                  ClassDecl *theClass,
                                                  llvm::Value *metadata) {
  auto &layout = IGF.IGM.getClassMetadataLayout(theClass);

  Address metadataAsBytes(
      IGF.Builder.CreateBitCast(metadata, IGF.IGM.Int8PtrTy), IGF.IGM.Int8Ty,
      IGF.IGM.getPointerAlignment());

  Address slot = IGF.Builder.CreateConstByteArrayGEP(
      metadataAsBytes,
      layout.getInstanceSizeOffset());
  slot = IGF.Builder.CreateElementBitCast(slot, IGF.IGM.Int32Ty);
  llvm::Value *size = IGF.Builder.CreateLoad(slot);
  if (IGF.IGM.SizeTy != IGF.IGM.Int32Ty)
    size = IGF.Builder.CreateZExt(size, IGF.IGM.SizeTy);

  slot = IGF.Builder.CreateConstByteArrayGEP(
      metadataAsBytes,
      layout.getInstanceAlignMaskOffset());
  slot = IGF.Builder.CreateElementBitCast(slot, IGF.IGM.Int16Ty);
  llvm::Value *alignMask = IGF.Builder.CreateLoad(slot);
  alignMask = IGF.Builder.CreateZExt(alignMask, IGF.IGM.SizeTy);

  return {size, alignMask};
}

llvm::MDString *irgen::typeIdForMethod(IRGenModule &IGM, SILDeclRef method) {
  assert(!method.getOverridden() && "must always be base method");

  auto entity = LinkEntity::forMethodDescriptor(method);
  auto mangled = entity.mangleAsString();
  auto typeId = llvm::MDString::get(*IGM.LLVMContext, mangled);
  return typeId;
}

static llvm::Value *emitVTableSlotLoad(IRGenFunction &IGF, Address slot,
                                       SILDeclRef method, Signature signature) {
  if (IGF.IGM.getOptions().VirtualFunctionElimination) {
    // For LLVM IR VFE, emit a @llvm.type.checked.load with the type of the
    // method.
    auto slotAsPointer = IGF.Builder.CreateElementBitCast(slot, IGF.IGM.Int8Ty);
    auto typeId = typeIdForMethod(IGF.IGM, method);

    // Arguments for @llvm.type.checked.load: 1) target address, 2) offset -
    // always 0 because target address is directly pointing to the right slot,
    // 3) type identifier, i.e. the mangled name of the *base* method.
    SmallVector<llvm::Value *, 8> args;
    args.push_back(slotAsPointer.getAddress());
    args.push_back(llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0));
    args.push_back(llvm::MetadataAsValue::get(*IGF.IGM.LLVMContext, typeId));

    // TODO/FIXME: Using @llvm.type.checked.load loses the "invariant" marker
    // which could mean redundant loads don't get removed.
    llvm::Value *checkedLoad = IGF.Builder.CreateIntrinsicCall(
        llvm::Intrinsic::type_checked_load, args);
    auto fnPtr = IGF.Builder.CreateExtractValue(checkedLoad, 0);
    return IGF.Builder.CreateBitCast(fnPtr,
                                     signature.getType()->getPointerTo());
  }

  // Not doing LLVM IR VFE, can just be a direct load.
  return IGF.emitInvariantLoad(slot);
}

FunctionPointer irgen::emitVirtualMethodValue(IRGenFunction &IGF,
                                              llvm::Value *metadata,
                                              SILDeclRef method,
                                              CanSILFunctionType methodType) {
  Signature signature = IGF.IGM.getSignature(methodType);

  auto classDecl = cast<ClassDecl>(method.getDecl()->getDeclContext());

  // Find the vtable entry we're interested in.
  auto methodInfo =
    IGF.IGM.getClassMetadataLayout(classDecl).getMethodInfo(IGF, method);
  switch (methodInfo.getKind()) {
  case ClassMetadataLayout::MethodInfo::Kind::Offset: {
    auto offset = methodInfo.getOffset();

    auto slot = IGF.emitAddressAtOffset(metadata, offset,
                                        signature.getType()->getPointerTo(),
                                        IGF.IGM.getPointerAlignment());
    auto fnPtr = emitVTableSlotLoad(IGF, slot, method, signature);
    auto &schema = methodType->isAsync()
                       ? IGF.getOptions().PointerAuth.AsyncSwiftClassMethods
                       : IGF.getOptions().PointerAuth.SwiftClassMethods;
    auto authInfo =
      PointerAuthInfo::emit(IGF, schema, slot.getAddress(), method);
    return FunctionPointer::createSigned(methodType, fnPtr, authInfo,
                                         signature, true);
  }
  case ClassMetadataLayout::MethodInfo::Kind::DirectImpl: {
    auto fnPtr = llvm::ConstantExpr::getBitCast(methodInfo.getDirectImpl(),
                                           signature.getType()->getPointerTo());
    llvm::Constant *secondaryValue = nullptr;
    if (cast<AbstractFunctionDecl>(method.getDecl())->hasAsync()) {
      auto *silFn = IGF.IGM.getSILFunctionForAsyncFunctionPointer(
          methodInfo.getDirectImpl());
      secondaryValue = cast<llvm::Constant>(
          IGF.IGM.getAddrOfSILFunction(silFn, NotForDefinition));
    }
    return FunctionPointer::forDirect(methodType, fnPtr, secondaryValue,
                                      signature, true);
  }
  }
  llvm_unreachable("covered switch");
}

FunctionPointer
irgen::emitVirtualMethodValue(IRGenFunction &IGF,
                              llvm::Value *base,
                              SILType baseType,
                              SILDeclRef method,
                              CanSILFunctionType methodType,
                              GenericSignature fnSig,
                              bool useSuperVTable) {
  // Find the metadata.
  llvm::Value *metadata;
  if (useSuperVTable) {
    // For a non-resilient 'super' call, emit a reference to the superclass
    // of the static type of the 'self' value.
    auto instanceTy = baseType.getASTType()->getMetatypeInstanceType();
    auto superTy = instanceTy->getSuperclass();
    metadata = emitClassHeapMetadataRef(IGF,
                                        superTy->getCanonicalType(),
                                        MetadataValueType::TypeMetadata,
                                        MetadataState::Complete);
  } else {
    if (baseType.is<MetatypeType>()) {
      // For a static method call, the 'self' value is already a class metadata.
      metadata = base;
    } else {
      // Otherwise, load the class metadata from the 'self' value's isa pointer.
      metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType, fnSig,
                                                  /*suppress cast*/ true);
    }
  }

  return emitVirtualMethodValue(IGF, metadata, method, methodType);
}
