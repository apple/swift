//===--- Address.h - Address Representation ---------------------*- C++ -*-===//
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
// A structure for holding the address of an object.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_ADDRESS_H
#define SWIFT_IRGEN_ADDRESS_H

#include "IRGen.h"
#include "llvm/ADT/ilist.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

namespace swift {
namespace irgen {

/// The address of an object in memory.
class Address {
  llvm::Value *Addr;
  llvm::Type *ElementType;
  Alignment Align;

public:
  Address() : Addr(nullptr) {}

  Address(llvm::Value *addr, llvm::Type *elementType, Alignment align)
      : Addr(addr), ElementType(elementType), Align(align) {
    if (addr == llvm::DenseMapInfo<llvm::Value *>::getEmptyKey() ||
        llvm::DenseMapInfo<llvm::Value *>::getTombstoneKey())
      return;
    assert(llvm::cast<llvm::PointerType>(addr->getType())
               ->isOpaqueOrPointeeTypeMatches(elementType) &&
           "Incorrect pointer element type");
    assert(addr != nullptr && "building an invalid address");
  }

  llvm::Value *operator->() const {
    assert(isValid());
    return getAddress();
  }

  bool isValid() const { return Addr != nullptr; }

  llvm::Value *getAddress() const { return Addr; }

  Alignment getAlignment() const {
    return Align;
  }
  
  llvm::PointerType *getType() const {
    return cast<llvm::PointerType>(Addr->getType());
  }

  llvm::Type *getElementType() const { return ElementType; }

  bool operator==(Address RHS) const {
    return Addr == RHS.Addr && ElementType == RHS.ElementType &&
           Align == RHS.Align;
  }
  bool operator!=(Address RHS) const { return !(*this == RHS); }
};

/// An address in memory together with the (possibly null) heap
/// allocation which owns it.
class OwnedAddress {
  Address Addr;
  llvm::Value *Owner;

public:
  OwnedAddress() : Owner(nullptr) {}
  OwnedAddress(Address address, llvm::Value *owner)
    : Addr(address), Owner(owner) {}

  llvm::Value *getAddressPointer() const { return Addr.getAddress(); }
  Alignment getAlignment() const { return Addr.getAlignment(); }
  Address getAddress() const { return Addr; }
  llvm::Value *getOwner() const { return Owner; }

  Address getUnownedAddress() const {
    assert(getOwner() == nullptr);
    return getAddress();
  }

  operator Address() const { return getAddress(); }

  bool isValid() const { return Addr.isValid(); }
};

/// An address in memory together with the local allocation which owns it.
class ContainedAddress {
  /// The address of an object of type T.
  Address Addr;

  /// The container of the address.
  Address Container;

public:
  ContainedAddress() {}
  ContainedAddress(Address container, Address address)
    : Addr(address), Container(container) {}

  llvm::Value *getAddressPointer() const { return Addr.getAddress(); }
  Alignment getAlignment() const { return Addr.getAlignment(); }
  Address getAddress() const { return Addr; }
  Address getContainer() const { return Container; }

  bool isValid() const { return Addr.isValid(); }
};

/// An address on the stack together with an optional stack pointer reset
/// location.
class StackAddress {
  /// The address of an object of type T.
  Address Addr;

  /// In a normal function, the result of llvm.stacksave or null.
  /// In a coroutine, the result of llvm.coro.alloca.alloc.
  /// In an async function, the result of the taskAlloc call.
  llvm::Value *ExtraInfo;

public:
  StackAddress() : ExtraInfo(nullptr) {}
  StackAddress(Address address, llvm::Value *extraInfo = nullptr)
    : Addr(address), ExtraInfo(extraInfo) {}

  /// Return a StackAddress with the address changed in some superficial way.
  StackAddress withAddress(Address addr) const {
    return StackAddress(addr, ExtraInfo);
  }

  llvm::Value *getAddressPointer() const { return Addr.getAddress(); }
  Alignment getAlignment() const { return Addr.getAlignment(); }
  Address getAddress() const { return Addr; }
  llvm::Value *getExtraInfo() const { return ExtraInfo; }

  bool isValid() const { return Addr.isValid(); }

  bool operator==(StackAddress RHS) const {
    return Addr == RHS.Addr && ExtraInfo == RHS.ExtraInfo;
  }
  bool operator!=(StackAddress RHS) const { return !(*this == RHS); }
};

} // end namespace irgen
} // end namespace swift

namespace llvm {
template <>
struct DenseMapInfo<swift::irgen::Address> {
  static swift::irgen::Address getEmptyKey() {
    return swift::irgen::Address(DenseMapInfo<llvm::Value *>::getEmptyKey(),
                                 DenseMapInfo<llvm::Type *>::getEmptyKey(),
                                 swift::irgen::Alignment(8));
  }
  static swift::irgen::Address getTombstoneKey() {
    return swift::irgen::Address(DenseMapInfo<llvm::Value *>::getTombstoneKey(),
                                 DenseMapInfo<llvm::Type *>::getTombstoneKey(),
                                 swift::irgen::Alignment(8));
  }
  static unsigned getHashValue(swift::irgen::Address address) {
    return detail::combineHashValue(
        DenseMapInfo<llvm::Value *>::getHashValue(address.getAddress()),
        detail::combineHashValue(
            DenseMapInfo<llvm::Type *>::getHashValue(address.getElementType()),
            DenseMapInfo<swift::irgen::Alignment::int_type>::getHashValue(
                address.getAlignment().getValue())));
  }
  static bool isEqual(swift::irgen::Address LHS, swift::irgen::Address RHS) {
    return LHS == RHS;
  }
};
template <>
struct DenseMapInfo<swift::irgen::StackAddress> {
  static swift::irgen::StackAddress getEmptyKey() {
    return swift::irgen::StackAddress(
        DenseMapInfo<swift::irgen::Address>::getEmptyKey(),
        DenseMapInfo<llvm::Value *>::getEmptyKey());
  }
  static swift::irgen::StackAddress getTombstoneKey() {
    return swift::irgen::StackAddress(
        DenseMapInfo<swift::irgen::Address>::getTombstoneKey(),
        DenseMapInfo<llvm::Value *>::getTombstoneKey());
  }
  static unsigned getHashValue(swift::irgen::StackAddress address) {
    return detail::combineHashValue(
        DenseMapInfo<swift::irgen::Address>::getHashValue(address.getAddress()),
        DenseMapInfo<swift::irgen::Alignment::int_type>::getHashValue(
            address.getAlignment().getValue()));
  }
  static bool isEqual(swift::irgen::StackAddress LHS,
                      swift::irgen::StackAddress RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

#endif
