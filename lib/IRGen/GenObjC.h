//===--- GenObjC.h - Swift IR generation for Objective-C --------*- C++ -*-===//
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
//  This file provides the private interface to Objective-C emission code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENOBJC_H
#define SWIFT_IRGEN_GENOBJC_H

#include "swift/SIL/SILDeclRef.h"

namespace llvm {
  class Type;
  class Value;
}

namespace swift {
  class CanType;
  class FuncDecl;
  struct SILDeclRef;
  class SILFunction;
  class SILType;

namespace irgen {
  class Callee;
  class CalleeInfo;
  class ConstantArrayBuilder;
  class IRGenFunction;
  class IRGenModule;

  /// The kind of message to send through the Objective-C runtime.
  enum class ObjCMessageKind {
    /// A normally-dispatched call.
    Normal,
    /// A call to a superclass method.
    Super,
    /// A call to a peer method.
    Peer
  };

  /// Represents an ObjC method reference that will be invoked by a form of
  /// objc_msgSend.
  class ObjCMethod {
    /// The SILDeclRef declaring the method.
    SILDeclRef method;
    /// For a bounded call, the static type that provides the lower bound for
    /// the search. Null for unbounded calls that will look for the method in
    /// the dynamic type of the object.
    llvm::PointerIntPair<SILType, 1, bool> searchTypeAndSuper;

  public:
    ObjCMethod(SILDeclRef method, SILType searchType, bool startAtSuper)
      : method(method), searchTypeAndSuper(searchType, startAtSuper)
    {}
    
    SILDeclRef getMethod() const { return method; }
    SILType getSearchType() const { return searchTypeAndSuper.getPointer(); }
    bool shouldStartAtSuper() const { return searchTypeAndSuper.getInt(); }
    
    /// FIXME: Thunk down to a Swift function value?
    llvm::Value *getExplosionValue(IRGenFunction &IGF) const {
      llvm_unreachable("thunking unapplied objc method to swift function "
                       "not yet implemented");
    }
    
    /// Determine the kind of message that should be sent to this
    /// method.
    ObjCMessageKind getMessageKind() const {
      // Determine the kind of message send to perform.
      if (!getSearchType()) return ObjCMessageKind::Normal;

      return shouldStartAtSuper()? ObjCMessageKind::Super
                                 : ObjCMessageKind::Peer;
    }
  };

  /// Prepare a callee for an Objective-C method.
  Callee getObjCMethodCallee(IRGenFunction &IGF, const ObjCMethod &method,
                             llvm::Value *selfValue, CalleeInfo &&info);

  /// Prepare a callee for an Objective-C method with the `objc_direct` attribute.
  Callee getObjCDirectMethodCallee(CalleeInfo &&info, const FunctionPointer &fn,
                                   llvm::Value *selfValue);

  /// Emit a partial application of an Objective-C method to its 'self'
  /// argument.
  void emitObjCPartialApplication(IRGenFunction &IGF,
                                  ObjCMethod method,
                                  CanSILFunctionType origType,
                                  CanSILFunctionType partialAppliedType,
                                  llvm::Value *self,
                                  SILType selfType,
                                  Explosion &out);

  /// Reclaim an autoreleased return value.
  llvm::Value *emitObjCRetainAutoreleasedReturnValue(IRGenFunction &IGF,
                                                     llvm::Value *value);

  /// Autorelease a return value.
  llvm::Value *emitObjCAutoreleaseReturnValue(IRGenFunction &IGF,
                                              llvm::Value *value);

  struct ObjCMethodDescriptor {
    llvm::Constant *selectorRef = nullptr;
    llvm::Constant *typeEncoding = nullptr;
    llvm::Constant *impl = nullptr;
    SILFunction *silFunction = nullptr;
  };

  /// Build the components of an Objective-C method descriptor for the given
  /// method or constructor implementation.
  ObjCMethodDescriptor
  emitObjCMethodDescriptorParts(IRGenModule &IGM,
                                AbstractFunctionDecl *method,
                                bool concrete);

  /// Build the components of an Objective-C method descriptor for the given
  /// property's method implementations.
  ObjCMethodDescriptor emitObjCGetterDescriptorParts(IRGenModule &IGM,
                                                     VarDecl *property);

  /// Build the components of an Objective-C method descriptor for the given
  /// subscript's method implementations.
  ObjCMethodDescriptor emitObjCGetterDescriptorParts(IRGenModule &IGM,
                                                     SubscriptDecl *property);

  /// Build the components of an Objective-C method descriptor if the abstract
  /// storage refers to a property or a subscript.
  ObjCMethodDescriptor
  emitObjCGetterDescriptorParts(IRGenModule &IGM,
                                AbstractStorageDecl *subscript);

  /// Build the components of an Objective-C method descriptor for the given
  /// property's method implementations.
  ObjCMethodDescriptor emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                                     VarDecl *property);

  /// Build the components of an Objective-C method descriptor for the given
  /// subscript's method implementations.
  ObjCMethodDescriptor emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                                     SubscriptDecl *property);

  /// Build the components of an Objective-C method descriptor if the abstract
  /// storage refers to a property or a subscript.
  ObjCMethodDescriptor
  emitObjCSetterDescriptorParts(IRGenModule &IGM,
                                AbstractStorageDecl *subscript);

  /// Build an Objective-C method descriptor for the given method,
  /// constructor, or destructor implementation.
  void emitObjCMethodDescriptor(IRGenModule &IGM,
                                ConstantArrayBuilder &descriptors,
                                AbstractFunctionDecl *method);

  /// Build an Objective-C method descriptor for the ivar initializer
  /// or destroyer of a class (-.cxx_construct or -.cxx_destruct).
  void emitObjCIVarInitDestroyDescriptor(IRGenModule &IGM,
                                         ConstantArrayBuilder &descriptors,
                                         ClassDecl *cd,
                                         llvm::Function *impl,
                                         bool isDestroyer);

  /// Get the type encoding for an ObjC property.
  void getObjCEncodingForPropertyType(IRGenModule &IGM, VarDecl *property,
                                      std::string &s);
  
  /// Produces extended encoding of ObjC block signature.
  /// \returns the encoded type.
  llvm::Constant *getBlockTypeExtendedEncoding(IRGenModule &IGM,
                                               CanSILFunctionType invokeTy);

  std::string getBlockTypeExtendedEncodingString(IRGenModule &IGM,
                                                 CanSILFunctionType invokeTy);

  /// Produces extended encoding of method type.
  /// \returns the encoded type.
  llvm::Constant *getMethodTypeExtendedEncoding(IRGenModule &IGM,
                                                AbstractFunctionDecl *method);
  
  /// Build an Objective-C method descriptor for the given getter method.
  void emitObjCGetterDescriptor(IRGenModule &IGM,
                                ConstantArrayBuilder &descriptors,
                                AbstractStorageDecl *storage);

  /// Build an Objective-C method descriptor for the given setter method.
  void emitObjCSetterDescriptor(IRGenModule &IGM,
                                ConstantArrayBuilder &descriptors,
                                AbstractStorageDecl *storage);

  /// True if the FuncDecl requires an ObjC method descriptor.
  bool requiresObjCMethodDescriptor(FuncDecl *method);

  /// True if the ConstructorDecl requires an ObjC method descriptor.
  bool requiresObjCMethodDescriptor(ConstructorDecl *constructor);

  /// True if the VarDecl requires ObjC accessor methods and a property
  /// descriptor.
  bool requiresObjCPropertyDescriptor(IRGenModule &IGM,
                                      VarDecl *property);

  /// True if the SubscriptDecl requires ObjC accessor methods.
  bool requiresObjCSubscriptDescriptor(IRGenModule &IGM,
                                       SubscriptDecl *subscript);

  /// Allocate an Objective-C object.
  llvm::Value *emitObjCAllocObjectCall(IRGenFunction &IGF,
                                       llvm::Value *classPtr,
                                       SILType resultType);

} // end namespace irgen
} // end namespace swift

#endif
