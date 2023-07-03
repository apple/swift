//===--- ScopeOptUtils.h --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// A simple scope like construct for use in the SILOptimizer.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_SCOPEOPTUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_SCOPEOPTUTILS_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include <functional>

namespace swift {
namespace siloptimizer {

template <typename Result, typename... Args>
struct Cleanup {
  std::function<Result(Args...)> func;
};

/// A simple stack data structure that manages a stack of std::function cleanups
/// that can be invalidated by index.
///
/// It is called asserting, since we force our user to explicitly pop the scope
/// and abort otherwise.
template <typename Result, typename... Args>
class AssertingScope {
  using ScopeCleanup = Cleanup<Result, Args...>;
  SmallVector<llvm::Optional<ScopeCleanup>, 64> cleanups;
  bool didPop = false;

protected:
  ~AssertingScope() { assert(didPop && "Did not pop scope?!"); }

  AssertingScope() {}

  /// Push a new SIL function based cleanup. Meant to be very
  /// lightweight. Returns the index of the cleanup.
  unsigned pushScope(std::function<Result(Args...)> func) {
    assert(!didPop && "Can not reuse scope once used?!");
    unsigned index = cleanups.size();
    cleanups.emplace_back(ScopeCleanup{func});
    return index;
  }

  /// Invalidate the cleanup at the specific index. Asserts on index too big and
  /// on Optional has value.
  void invalidateCleanup(unsigned cleanupIndex) {
    assert(!didPop && "Should not invalidate once popped?!");
    assert(cleanups[cleanupIndex].has_value());
    cleanups[cleanupIndex] = llvm::None;
  }

  /// Pop the scope, running all non-Optional cleanups, and setting didPop. Once
  /// this is done the scope has been consumed and no longer has any state.
  ///
  /// Can only be done once the scope has been moved.
  void pop(Args... args) && {
    while (cleanups.size()) {
      auto val = cleanups.pop_back_val();
      if (val) {
        val->func(std::forward<Args>(args)...);
      }
    }
    didPop = true;
  }

  /// Can not be called once the scope is moved.
  void nonDestructivePop(Args... args) const {
    for (auto val : llvm::reverse(cleanups)) {
      if (val) {
        val->func(std::forward<Args>(args)...);
      }
    }
  }
};

class ScopedValue;
class SILOptScope;

/// Part of the SILRAIIScope interface that is known to ScopedValue.
class SILOptScopeBase : public AssertingScope<void, SILInstruction *> {
protected:
  SILOptScopeBase() {}

public:
  using Index = unsigned;

  /// Destructive pop at end of scope.
  void popAtEndOfScope(SILInstruction *insertPt) && {
    std::move(*this).pop(insertPt);
  }

  /// Non-destructive pop along a control flow path that is serving as an early
  /// exit.
  void popAtEarlyExit(SILInstruction *insertPt) const {
    nonDestructivePop(insertPt);
  }

private:
  friend class ScopedValue;

  /// Invalidate the passed in scoped value destructively.
  void invalidateValue(ScopedValue &&value);
};

/// A light weight wrapper around a SILValue that also contains information
/// about a Cleanup closure that ends the lifetime of the given value.
///
/// In order to make it really easy to access the underlying value, we define
/// operator * and operator-> as returning the underlying SILValue. It also
/// defines explicit operator bool() so one should be able to use if statements
/// to perform conditional initialization by returning an empty ScopedValue().
///
/// If one realizes that one does not actually need to cleanup a value, one can
/// destructively forward the ScopedValue by writing:
///
///   std::move(scopedValue).forward();
///
/// This will cause the cleanup in the SILOptScopeBase to be invalidated and
/// thus will not be emitted when the scope is popped.
class ScopedValue {
  friend class SILOptScopeBase;
  friend class SILOptScope;

  /// A back pointer to the scope we are managed by so that we can invalidate
  /// the cleanup associated with us. Also used to invalidate the underlying
  /// ScopedValue by setting this to nullptr.
  SILOptScopeBase *scope;

  /// The index in our parent scope object of our cleanup. This is used to
  /// enable invalidation.
  llvm::Optional<unsigned> scopeIndex;

  /// The underlying SILValue that we are working with.
  SILValue value;

  /// A scoped value that has a cleanup associated with it at the index \p
  /// scopeIndex within \p scope.
  ScopedValue(SILOptScopeBase &scope, SILValue value, unsigned scopeIndex)
      : scope(&scope), scopeIndex(scopeIndex), value(value) {}

  /// A scoped value that does not have an associated cleanup in the scope.
  ScopedValue(SILOptScopeBase &scope, SILValue value)
      : scope(&scope), scopeIndex(llvm::None), value(value) {}

public:
  /// Default empty constructor. Should be used in combination with operator
  /// bool() to perform conditional initialization.
  ScopedValue() : scope(nullptr), scopeIndex(), value() {}

  /// Explicit operator that says if this is a valid scoped Value.
  ///
  /// Allows for conditional initialization via return value:
  ///
  ///   if (auto value = getScopedValue(...)) {
  ///     ...
  ///   }
  explicit operator bool() const { return isValid(); }

  /// Check just for the validity of the value, this doesn't
  bool isValid() const { return scope && value; }

  /// Returns true if this scoped value has a cleanup associated with it that
  /// can be invalidated.
  ///
  /// Example: Result of SILOptScope::copyValue would return true, but result of
  /// a SILOptScope::borrowValue would return false since we do not allow for
  /// borrows to be cancelled.
  bool hasInvalidatableCleanup() const {
    assert(isValid());
    return bool(scopeIndex);
  }

  /// Operator to access internal SILValue.
  SILValue operator*() const {
    assert(isValid());
    return value;
  }

  /// Operator to call methods on internal SILValue.
  SILValue operator->() const {
    assert(isValid());
    return value;
  }

  /// If this scope has a scoped index, invalidate the scoped value so that its
  /// cleanup is not emitted at end of scope.
  void invalidate() && {
    assert(isValid());
    // If we have an invalidated cleanup, do the invalidation.
    if (hasInvalidatableCleanup()) {
      scope->invalidateValue(std::move(*this));
    }
    // No matter what we do, make it so that if this object is used again, we
    // get an assert.
    scope = nullptr;
    scopeIndex.reset();
  }
};

inline void SILOptScopeBase::invalidateValue(ScopedValue &&value) {
  if (value.scopeIndex)
    invalidateCleanup(*value.scopeIndex);
}

/// A scope like stack data structure that one can use to manage cleanups for
/// SILValues using light weight ScopedValue wrappers. This is done by using the
/// copyValue/borrowValue methods. These copy/borrow the value and then push a
/// destroy_value/end_borrow cleanup and return a ScopedValue containing that
/// the user can then manipulate the value without needing to worry about the
/// cleanup. If the user then decides a specific ScopedValue should be able to
/// be cancelled, we allow for the ScopedValue to be invalidated
class SILOptScope : public SILOptScopeBase {
public:
  SILOptScope() {}

  /// Create a destroy_value cleanup for \p value. Returns a scoped value for \p
  /// value with a live cleanup.
  ScopedValue pushDestroyValue(SILValue value) {
    // We need to capture value by value, not by address.
    auto func = std::function<void(SILInstruction *)>(
        [innerValue = value](SILInstruction *insertPt) {
          SILBuilderWithScope builder(insertPt);
          builder.emitDestroyValue(RegularLocation::getAutoGeneratedLocation(),
                                   innerValue);
        });
    return {*this, value, pushScope(func)};
  }

  /// Copy \p value at \p insertPt and push a destroy_value cleanup. Return the
  /// new copied value as a ScopedValue with cleanup.
  ScopedValue copyValue(SILInstruction *insertPt, SILValue value) {
    SILValue copy = SILBuilderWithScope(insertPt).emitCopyValueOperation(
        insertPt->getLoc(), value);
    return pushDestroyValue(copy);
  }

  ScopedValue pushEndBorrow(SILValue value) {
    // We need to capture value by value, not by address.
    auto func = std::function<void(SILInstruction *)>(
        [innerValue = value](SILInstruction *insertPt) {
          SILBuilderWithScope builder(insertPt);
          builder.emitEndBorrowOperation(
              RegularLocation::getAutoGeneratedLocation(), innerValue);
        });
    // We do not return the index of our end_borrow since an end_borrow should
    // never have its cleanup cancelled.
    pushScope(func);
    return {*this, value};
  }

  /// Borrow \p value at \p insertPt and push an end_borrow cleanup. Return the
  /// new borrowed value as a ScopedValue without cleanup. The cleanup is not
  /// returned since end_borrows should not be cancellable.
  ScopedValue borrowValue(SILInstruction *insertPt, SILValue value) {
    if (!insertPt->getFunction()->hasOwnership() ||
        value->getOwnershipKind().isCompatibleWith(OwnershipKind::Guaranteed))
      return {};
    SILValue borrow = SILBuilderWithScope(insertPt).emitBeginBorrowOperation(
        insertPt->getLoc(), value);
    return pushEndBorrow(borrow);
  }
};

} // namespace siloptimizer
} // namespace swift

#endif
