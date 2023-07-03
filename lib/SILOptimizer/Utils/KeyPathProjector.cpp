//===-- KeyPathProjector.cpp - Project a static key path --------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Utility class to project a statically known key path
/// expression to a direct property access sequence.
///
//===----------------------------------------------------------------------===//


#include "swift/SILOptimizer/Utils/KeyPathProjector.h"

#include "swift/SIL/SILInstruction.h"

using namespace swift;


// Projectors to handle individual key path components.

/// Projects the root of a key path application.
class RootProjector : public KeyPathProjector {
public:
    RootProjector(SILValue root, SILLocation loc, SILBuilder &builder)
        : KeyPathProjector(loc, builder), root(root) {}
    
    void project(AccessType accessType,
                 std::function<void (SILValue)> callback) override {
        if (accessType == AccessType::Set) {
          // We're setting the identity key path (\.self). The callback
          // expects an uninitialized address, so destroy the old value.
          builder.emitDestroyAddr(loc, root);
        }
        callback(root);
    }
    
    bool isStruct() override {
        return root->getType().getStructOrBoundGenericStruct() != nullptr;
    }
private:
    SILValue root;
};

/// Projects a single key path component.
class ComponentProjector : public KeyPathProjector {
protected:
    ComponentProjector(const KeyPathPatternComponent &component,
                       std::unique_ptr<KeyPathProjector> parent,
                       SILLocation loc, SILBuilder &builder)
        : KeyPathProjector(loc, builder),
          component(component), parent(std::move(parent)) {}
    
    /// The key path component.
    const KeyPathPatternComponent &component;
    
    /// The projector for the previous components.
    std::unique_ptr<KeyPathProjector> parent;
    
    bool isStruct() override {
        auto type = component.getComponentType();
        return type.getStructOrBoundGenericStruct() != nullptr;
    }
    
    ~ComponentProjector() override {};
};


/// Ends the begin_access "scope" if a begin_access was inserted for optimizing
/// a keypath pattern.
static void insertEndAccess(BeginAccessInst *&beginAccess,
                            SILBuilder &builder) {
  if (beginAccess) {
    builder.createEndAccess(beginAccess->getLoc(), beginAccess,
                            /*aborted*/ false);
    beginAccess = nullptr;
  }
}

class StoredPropertyProjector : public ComponentProjector {
public:
    StoredPropertyProjector(const KeyPathPatternComponent &component,
                            std::unique_ptr<KeyPathProjector> parent,
                            BeginAccessInst *&beginAccess,
                            SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder),
          beginAccess(beginAccess) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::StoredProperty);
    
    VarDecl *storedProperty = component.getStoredPropertyDecl();
    
    if (parent->isStruct()) {
      // Reading a struct field -> reading the struct
      // Writing or modifying a struct field -> modifying the struct
      AccessType parentAccessType;
      if (accessType == AccessType::Get)
        parentAccessType = AccessType::Get;
      else
        parentAccessType = AccessType::Modify;
      
      parent->project(parentAccessType, [&](SILValue parentValue) {
        auto addr = builder.createStructElementAddr(loc, parentValue, storedProperty);
        // If we're setting, destroy the old value (the callback expects uninitialized memory)
        if (accessType == AccessType::Set)
          builder.createDestroyAddr(loc, addr);
        callback(addr);
      });
    } else {
      // Accessing a class member -> reading the class
      parent->project(AccessType::Get, [&](SILValue parentValue) {
        SingleValueInstruction *Ref = builder.createLoad(loc, parentValue,
                                                         LoadOwnershipQualifier::Unqualified);
        
        // If we were previously accessing a class member, we're done now.
        insertEndAccess(beginAccess, builder);
        
        // Handle the case where the storedProperty is in a super class.
        while (Ref->getType().getClassOrBoundGenericClass() !=
               storedProperty->getDeclContext()) {
          SILType superCl = Ref->getType().getSuperclass();
          if (!superCl) {
            // This should never happen, because the property should be in the
            // decl or in a superclass of it. Just handle this to be on the safe
            // side.
            callback(SILValue());
            return;
          }
          Ref = builder.createUpcast(loc, Ref, superCl);
        }
        
        SILValue addr = builder.createRefElementAddr(loc, Ref, storedProperty);
        
        // Class members need access enforcement.
        if (builder.getModule().getOptions().EnforceExclusivityDynamic) {
          beginAccess = builder.createBeginAccess(loc, addr, SILAccessKind::Read,
                                                  SILAccessEnforcement::Dynamic,
                                                  /*noNestedConflict*/ false,
                                                  /*fromBuiltin*/ false);
          if (accessType != AccessType::Get)
            beginAccess->setAccessKind(SILAccessKind::Modify);
          addr = beginAccess;
        }
        
        // If we're setting, destroy the old value (the callback expects uninitialized memory)
        if (accessType == AccessType::Set)
          builder.createDestroyAddr(loc, addr);
        callback(addr);
        
        // if a child hasn't started a new access (i.e. beginAccess is unchanged),
        // end the access now
        if (beginAccess == addr) {
          insertEndAccess(beginAccess, builder);
        }
      });
    }
  }
private:
  BeginAccessInst *&beginAccess;
};

class TupleElementProjector : public ComponentProjector {
public:
  TupleElementProjector(const KeyPathPatternComponent &component,
                        std::unique_ptr<KeyPathProjector> parent,
                        SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::TupleElement);
    
    // Reading a tuple field -> reading the tuple
    // Writing or modifying a tuple field -> modifying the tuple
    AccessType parentAccessType;
    if (accessType == AccessType::Get)
      parentAccessType = AccessType::Get;
    else
      parentAccessType = AccessType::Modify;
    
    parent->project(parentAccessType, [&](SILValue parentValue) {
      auto addr = builder.createTupleElementAddr(loc, parentValue, component.getTupleIndex());
      // If we're setting, destroy the old value (the callback expects uninitialized memory)
      if (accessType == AccessType::Set)
        builder.createDestroyAddr(loc, addr);
      callback(addr);
    });
  }
};

class GettablePropertyProjector : public ComponentProjector {
public:
    GettablePropertyProjector(KeyPathInst *keyPath,
                              const KeyPathPatternComponent &component,
                              std::unique_ptr<KeyPathProjector> parent,
                              SubstitutionMap subs, BeginAccessInst *&beginAccess,
                              SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder),
          keyPath(keyPath), subs(subs), beginAccess(beginAccess) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::GettableProperty ||
           component.getKind() ==
           KeyPathPatternComponent::Kind::SettableProperty);
    assert(accessType == AccessType::Get && "property is not settable");
    
    parent->project(accessType, [&](SILValue parentValue) {
      auto getter = component.getComputedPropertyGetter();
      
      // The callback expects a memory address it can read from,
      // so allocate a buffer.
      auto &function = builder.getFunction();
      auto substType = component.getComponentType().subst(
          keyPath->getSubstitutions(), llvm::None);
      SILType type = function.getLoweredType(
                         Lowering::AbstractionPattern::getOpaque(), substType);
      auto addr = builder.createAllocStack(loc, type);
      
      assertHasNoContext();
      assert(getter->getConventions().getNumSILArguments());
      
      auto ref = builder.createFunctionRef(loc, getter);
      builder.createApply(loc, ref, subs, {addr, parentValue});
      
      // If we were previously accessing a class member, we're done now.
      insertEndAccess(beginAccess, builder);
      
      callback(addr);
      
      builder.createDestroyAddr(loc, addr);
      builder.createDeallocStack(loc, addr);
    });
    
  }
protected:
  KeyPathInst *keyPath;
  SubstitutionMap subs;
  BeginAccessInst *&beginAccess;
  
  void assertHasNoContext() {
    assert(component.getSubscriptIndices().empty() &&
           component.getExternalSubstitutions().empty() &&
           "cannot yet optimize key path component with external context; "
           "we should have checked for this before trying to project");
  }
};

class SettablePropertyProjector : public GettablePropertyProjector {
public:
    SettablePropertyProjector(KeyPathInst *keyPath,
                              const KeyPathPatternComponent &component,
                              std::unique_ptr<KeyPathProjector> parent,
                              SubstitutionMap subs, BeginAccessInst *&beginAccess,
                              SILLocation loc, SILBuilder &builder)
        : GettablePropertyProjector(keyPath, component, std::move(parent),
                                    subs, beginAccess, loc, builder) {}
  
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::GettableProperty ||
           component.getKind() ==
           KeyPathPatternComponent::Kind::SettableProperty);
    
    switch (accessType) {
      case AccessType::Get:
        GettablePropertyProjector::project(accessType, callback);
        break;
        
      case AccessType::Modify:
      case AccessType::Set:
        AccessType parentAccessType;
        if (component.isComputedSettablePropertyMutating()) {
          // A mutating setter modifies the parent
          parentAccessType = AccessType::Modify;
          if (beginAccess) {
            beginAccess->setAccessKind(SILAccessKind::Modify);
          }
        } else {
            parentAccessType = AccessType::Get;
        }
        
        parent->project(parentAccessType, [&](SILValue parentValue) {
          auto getter = component.getComputedPropertyGetter();
          auto setter = component.getComputedPropertySetter();
          
          // The callback expects a memory address it can write to,
          // so allocate a writeback buffer.
          auto &function = builder.getFunction();
          auto substType = component.getComponentType().subst(
              keyPath->getSubstitutions(), llvm::None);
          SILType type = function.getLoweredType(
                        Lowering::AbstractionPattern::getOpaque(), substType);
          auto addr = builder.createAllocStack(loc, type);

          assertHasNoContext();
          assert(getter->getConventions().getNumSILArguments());
          assert(setter->getConventions().getNumSILArguments());
          
          // If this is a modify, we need to call the getter and
          // store the result in the writeback buffer.
          if (accessType == AccessType::Modify) {
            auto getterRef = builder.createFunctionRef(loc, getter);
            builder.createApply(loc, getterRef, subs, {addr, parentValue});
          }
          
          // The callback function will write into the writeback buffer.
          callback(addr);
          
          // Pass the value from the writeback buffer to the setter.
          auto setterRef = builder.createFunctionRef(loc, setter);
          builder.createApply(loc, setterRef, subs, {addr, parentValue});
          
          // Deallocate the writeback buffer.
          builder.createDestroyAddr(loc, addr);
          builder.createDeallocStack(loc, addr);
        });
        break;
    }
  }
};

class OptionalWrapProjector : public ComponentProjector {
public:
  OptionalWrapProjector(KeyPathInst *kpInst,
                        const KeyPathPatternComponent &component,
                        std::unique_ptr<KeyPathProjector> parent,
                        SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder),
          keyPath(kpInst) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::OptionalWrap);
    assert(accessType == AccessType::Get && "optional wrap components are immutable");
    
    parent->project(AccessType::Get, [&](SILValue parentValue) {
      auto &function = builder.getFunction();
      auto substType = component.getComponentType().subst(
          keyPath->getSubstitutions(), llvm::None);
      SILType optType = function.getLoweredType(
                         Lowering::AbstractionPattern::getOpaque(), substType);
      SILType objType = optType.getOptionalObjectType().getAddressType();
      
      assert(objType && "optional wrap must return an optional");

      // Allocate a buffer for the result.
      auto optAddr = builder.createAllocStack(loc, optType);
      
      // Store the parent result in the enum payload address.
      auto someDecl = builder.getASTContext().getOptionalSomeDecl();
      auto objAddr = builder.createInitEnumDataAddr(loc, optAddr,
                                                    someDecl, objType);
      builder.createCopyAddr(loc, parentValue, objAddr, IsNotTake, IsInitialization);
      
      // Initialize the Optional enum.
      builder.createInjectEnumAddr(loc, optAddr, someDecl);
      
      callback(optAddr);
      
      // Destroy the Optional.
      builder.createDestroyAddr(loc, optAddr);
      builder.createDeallocStack(loc, optAddr);
    });
  }

private:
  KeyPathInst *keyPath;
};

class OptionalForceProjector : public ComponentProjector {
public:
  OptionalForceProjector(const KeyPathPatternComponent &component,
                         std::unique_ptr<KeyPathProjector> parent,
                         SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::OptionalForce);
    
    parent->project(accessType, [&](SILValue optAddr) {
      auto &ctx = builder.getASTContext();
      
      auto noneDecl = ctx.getOptionalNoneDecl();
      auto someDecl = ctx.getOptionalSomeDecl();
      
      SILType optType = optAddr->getType();
      SILType objType = optType.getOptionalObjectType();
      
      if (accessType != AccessType::Set) {
        // We're getting (or modifying), so we need to unwrap the optional.
        auto int1Type = SILType::getBuiltinIntegerType(1, ctx);
        auto falseLiteral = builder.createIntegerLiteral(loc, int1Type, false);
        auto trueLiteral = builder.createIntegerLiteral(loc, int1Type, true);
        
        auto isNil = builder.createSelectEnumAddr(loc, optAddr, int1Type, SILValue(), {
          {noneDecl, trueLiteral}, {someDecl, falseLiteral}
        });
        builder.createCondFail(loc, isNil, "unexpectedly found nil while "
                               "unwrapping an Optional key-path expression");
      }
      
      switch (accessType) {
        case AccessType::Get: {
          // We have to copy the optional, since unwrapping is destructive.
          auto tempAddr = builder.createAllocStack(loc, optType);
          builder.createCopyAddr(loc, optAddr, tempAddr, IsNotTake, IsInitialization);
          
          // Unwrap the optional.
          auto objAddr = builder.createUncheckedTakeEnumDataAddr(loc, tempAddr, someDecl, objType);
          
          callback(objAddr);
          
          builder.createDestroyAddr(loc, objAddr);
          builder.createDeallocStack(loc, tempAddr);
          break;
        }
        case AccessType::Set: {
          // Write the new value directly into optAddr.
          auto objAddr = builder.createInitEnumDataAddr(loc, optAddr, someDecl, objType);
          
          callback(objAddr);
          
          // Finish creating the enum.
          builder.createInjectEnumAddr(loc, optAddr, someDecl);
          break;
        }
        case AccessType::Modify: {
          // We have to copy the old value out, perform the modification,
          // and copy the new value back in.
          auto objAddr = builder.createAllocStack(loc, objType);
          
          // Unwrap the optional and copy it to the new buffer.
          auto unwrappedAddr = builder.createUncheckedTakeEnumDataAddr(loc, optAddr, someDecl, objType);
          builder.createCopyAddr(loc, unwrappedAddr, objAddr, IsTake, IsInitialization);
          
          callback(objAddr);
          
          auto initAddr = builder.createInitEnumDataAddr(loc, optAddr, someDecl, objType);
          builder.createCopyAddr(loc, objAddr, initAddr, IsTake, IsInitialization);
          builder.createDeallocStack(loc, objAddr);
          builder.createInjectEnumAddr(loc, optAddr, someDecl);
          break;
        }
      }
    });
  }
};

class OptionalChainProjector : public ComponentProjector {
public:
  OptionalChainProjector(const KeyPathPatternComponent &component,
                         std::unique_ptr<KeyPathProjector> parent,
                         BeginAccessInst *&beginAccess,
                         SILValue optionalChainResult,
                         SILLocation loc, SILBuilder &builder)
        : ComponentProjector(component, std::move(parent), loc, builder),
          optionalChainResult(optionalChainResult), beginAccess(beginAccess) {}
  
  void project(AccessType accessType,
               std::function<void(SILValue addr)> callback) override {
    assert(component.getKind() ==
           KeyPathPatternComponent::Kind::OptionalChain);
    assert(accessType == AccessType::Get &&
           "Optional chain components are immutable");
    
    parent->project(accessType, [&](SILValue optAddr) {
      auto &ctx = builder.getASTContext();
      
      auto noneDecl = ctx.getOptionalNoneDecl();
      auto someDecl = ctx.getOptionalSomeDecl();
      
      SILType optType = optAddr->getType();
      SILType objType = optType.getOptionalObjectType();
      
      // Continue projecting only if the optional is non-nil
      // i.e. if let objAddr = optAddr {
      auto continuation = builder.splitBlockForFallthrough();
      auto ifSome = builder.getFunction().createBasicBlockAfter(builder.getInsertionBB());
      auto ifNone = builder.getFunction().createBasicBlockAfter(ifSome);
      builder.createSwitchEnumAddr(loc, optAddr, /*defaultBB*/ nullptr,
                                   {{noneDecl, ifNone}, {someDecl, ifSome}});
      
      assert(ifSome->empty());
      builder.setInsertionPoint(ifSome);
      
      // We have to copy the optional, since unwrapping is destructive.
      auto tempAddr = builder.createAllocStack(loc, optType);
      builder.createCopyAddr(loc, optAddr, tempAddr, IsNotTake, IsInitialization);
      
      // Unwrap the optional.
      auto objAddr = builder.createUncheckedTakeEnumDataAddr(loc, tempAddr, someDecl, objType);
      BeginAccessInst *origBeginAccess = beginAccess;
      
      // at the end of the projection, callback will store a value in optionalChainResult
      callback(objAddr);
      
      builder.createDestroyAddr(loc, objAddr);
      builder.createDeallocStack(loc, tempAddr);
      
      builder.createBranch(loc, continuation);
      // else, store nil in the result
      builder.setInsertionPoint(ifNone);

      // If the sub-projection ended the access in the some-branch, we also
      // have to end the access in the none-branch.
      if (origBeginAccess && origBeginAccess != beginAccess)
        builder.createEndAccess(loc, origBeginAccess, /*aborted*/ false);

      builder.createInjectEnumAddr(loc, optionalChainResult, noneDecl);
      
      builder.createBranch(loc, continuation);
      // end if, allow parents to clean up regardless of whether the chain continued
      builder.setInsertionPoint(continuation, continuation->begin());
    });
  }
  
private:
  SILValue optionalChainResult;
  BeginAccessInst *&beginAccess;
};

/// A projector to handle a complete key path.
class CompleteKeyPathProjector : public KeyPathProjector {
public:
  CompleteKeyPathProjector(KeyPathInst *keyPath, SILValue root,
                           SILLocation loc, SILBuilder &builder)
  : KeyPathProjector(loc, builder), keyPath(keyPath), root(root) {}

  void project(AccessType accessType,
               std::function<void (SILValue)> callback) override {
    auto components = keyPath->getPattern()->getComponents();
    
    // Check if the keypath has an optional chain.
    bool isOptionalChain = false;
    for (const KeyPathPatternComponent &comp : components) {
      if (comp.getKind() == KeyPathPatternComponent::Kind::OptionalChain) {
        isOptionalChain = true;
        break;
      }
    }
    
    // Root projector
    auto rootProjector = std::make_unique<RootProjector>(root, loc, builder);
    
    BeginAccessInst *beginAccess = nullptr;
    
    if (isOptionalChain) {
      assert(accessType == AccessType::Get && "Optional chains are read-only");
      
      // If we're reading an optional chain, create an optional result.
      auto resultCanType = components.back().getComponentType();
      auto &function = builder.getFunction();
      auto substType =
          resultCanType.subst(keyPath->getSubstitutions(), llvm::None);
      auto optType = function.getLoweredType(
                         Lowering::AbstractionPattern::getOpaque(), substType);
      
      assert(optType.getOptionalObjectType() &&
             "Optional-chained key path should result in an optional");
      SILValue optionalChainResult = builder.createAllocStack(loc, optType);
      
      // Get the (conditional) result projector.
      auto projector = create(0, std::move(rootProjector),
                              beginAccess, optionalChainResult);
      
      projector->project(accessType, [&](SILValue result) {
        // This will only run if all optional chains succeeded.
        // Store the result in optionalChainResult.
        builder.createCopyAddr(loc, result, optionalChainResult,
                               IsNotTake, IsInitialization);
      });
      
      // If the optional chain succeeded, optionalChainResult will have
      // .some(result). Otherwise, projectOptionalChain will have written .none.
      callback(optionalChainResult);
      builder.createDestroyAddr(loc, optionalChainResult);
      builder.createDeallocStack(loc, optionalChainResult);
    } else {
      // If we're not optional chaining, or we're writing to an optional chain,
      // we don't need an optional result.
      auto projector =  create(0, std::move(rootProjector),
                               beginAccess, /*optionalChainResult*/ nullptr);
      projector->project(accessType, callback);
    }
    assert(beginAccess == nullptr &&
           "key path projector returned with dangling access enforcement");
  }
  
  bool isStruct() override {
    auto components = keyPath->getPattern()->getComponents();
    auto resultType = components.back().getComponentType();
    return resultType.getStructOrBoundGenericStruct() != nullptr;
  }
  
private:
  KeyPathInst *keyPath;
  SILValue root;
  
  /// Recursively creates a chain of key path projectors
  /// for components from index..<components.end()
  std::unique_ptr<KeyPathProjector>
  create(size_t index, std::unique_ptr<KeyPathProjector> parent,
         BeginAccessInst *&beginAccess, SILValue optionalChainResult) {
    auto components = keyPath->getPattern()->getComponents();
    
    if (index >= components.size()) return parent;
    
    auto &comp = components[index];
    std::unique_ptr<KeyPathProjector> projector;
    
    // Create a projector for this component.
    switch (comp.getKind()) {
      case KeyPathPatternComponent::Kind::StoredProperty:
        projector = std::make_unique<StoredPropertyProjector>
            (comp, std::move(parent), beginAccess, loc, builder);
        break;
      case KeyPathPatternComponent::Kind::TupleElement:
        projector = std::make_unique<TupleElementProjector>
            (comp, std::move(parent), loc, builder);
        break;
      case KeyPathPatternComponent::Kind::GettableProperty:
        projector = std::make_unique<GettablePropertyProjector>
            (keyPath, comp, std::move(parent), keyPath->getSubstitutions(),
             beginAccess, loc, builder);
        break;
      case KeyPathPatternComponent::Kind::SettableProperty:
        projector = std::make_unique<SettablePropertyProjector>
            (keyPath, comp, std::move(parent), keyPath->getSubstitutions(),
             beginAccess, loc, builder);
        break;
      case KeyPathPatternComponent::Kind::OptionalWrap:
        projector = std::make_unique<OptionalWrapProjector>
            (keyPath, comp, std::move(parent), loc, builder);
        break;
      case KeyPathPatternComponent::Kind::OptionalForce:
        projector = std::make_unique<OptionalForceProjector>
            (comp, std::move(parent), loc, builder);
        break;
      case KeyPathPatternComponent::Kind::OptionalChain:
        projector = std::make_unique<OptionalChainProjector>
            (comp, std::move(parent), beginAccess, optionalChainResult, loc,
             builder);
        break;
    }
    
    // Project the rest of the chain on top of this component.
    return create(index + 1, std::move(projector),
                  beginAccess, optionalChainResult);
  }
};

KeyPathInst *
KeyPathProjector::getLiteralKeyPath(SILValue keyPath) {
  if (auto *upCast = dyn_cast<UpcastInst>(keyPath))
    keyPath = upCast->getOperand();
  // TODO: Look through other conversions, copies, etc.?
  return dyn_cast<KeyPathInst>(keyPath);
}

std::unique_ptr<KeyPathProjector>
KeyPathProjector::create(SILValue keyPath, SILValue root,
                         SILLocation loc, SILBuilder &builder) {
  // Is it a keypath instruction at all?
  auto *kpInst = getLiteralKeyPath(keyPath);
  if (!kpInst || !kpInst->hasPattern())
    return nullptr;
  
  // Check if the keypath only contains patterns which we support.
  auto components = kpInst->getPattern()->getComponents();
  for (const KeyPathPatternComponent &comp : components) {
    if (comp.getKind() == KeyPathPatternComponent::Kind::GettableProperty ||
        comp.getKind() == KeyPathPatternComponent::Kind::SettableProperty) {
      if (!comp.getExternalSubstitutions().empty() ||
          !comp.getSubscriptIndices().empty()) {
        // TODO: right now we can't optimize computed properties that require
        // additional context for subscript indices or generic environment
        // See https://github.com/apple/swift/pull/28799#issuecomment-570299845
        return nullptr;
      }
    }
  }

  return std::make_unique<CompleteKeyPathProjector>(kpInst, root,
                                                    loc, builder);
}
