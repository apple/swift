//===--- GenPack.h - Swift IR Generation For Variadic Generics --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for type and value packs in Swift.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENPACK_H
#define SWIFT_IRGEN_GENPACK_H

#include "IRGen.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {

class Value;

} // end namespace llvm

namespace swift {

namespace irgen {
class Address;
class IRGenFunction;
class IRGenModule;
class DynamicMetadataRequest;
class MetadataResponse;
class StackAddress;

MetadataResponse
emitPackArchetypeMetadataRef(IRGenFunction &IGF,
                             CanPackArchetypeType type,
                             DynamicMetadataRequest request);

std::pair<StackAddress, llvm::Value *>
emitTypeMetadataPack(IRGenFunction &IGF, CanPackType packType,
                     DynamicMetadataRequest request);

MetadataResponse
emitTypeMetadataPackRef(IRGenFunction &IGF,
                        CanPackType packType,
                        DynamicMetadataRequest request);

/// Given a pointer to a potentially heap-allocated pack of metadata/wtables,
/// mask off the bit that indicates whether it is heap allocated.
llvm::Value *maskMetadataPackPointer(IRGenFunction &IGF, llvm::Value *);

void bindOpenedElementArchetypesAtIndex(IRGenFunction &IGF,
                                        GenericEnvironment *env,
                                        llvm::Value *index);

llvm::Value *
emitTypeMetadataPackElementRef(IRGenFunction &IGF, CanPackType packType,
                               ArrayRef<ProtocolConformanceRef> conformances,
                               llvm::Value *index,
                               DynamicMetadataRequest request,
                               llvm::SmallVectorImpl<llvm::Value *> &wtables);

void cleanupTypeMetadataPack(IRGenFunction &IGF, StackAddress pack,
                             llvm::Value *shape);

std::pair<StackAddress, llvm::Value *>
emitWitnessTablePack(IRGenFunction &IGF, CanPackType packType,
                     PackConformance *conformance);

llvm::Value *emitWitnessTablePackRef(IRGenFunction &IGF, CanPackType packType,
                                     PackConformance *conformance);

void cleanupWitnessTablePack(IRGenFunction &IGF, StackAddress pack,
                             llvm::Value *shape);

/// An on-stack pack metadata/wtable allocation.
///
/// Includes the stack address, the element count, and the kind of requirement
/// (a GenericRequirement::Kind represented as a raw uint8_t).
using StackPackAlloc =
    std::tuple<StackAddress, /*shape*/ llvm::Value *, /*kind*/ uint8_t>;

/// Emits cleanups for an array of on-stack pack metadata/wtable allocations in
/// reverse order.
void cleanupStackAllocPacks(IRGenFunction &IGF,
                            ArrayRef<StackPackAlloc> allocs);

/// Emit the dynamic index of a particular structural component
/// of the given pack type.  If the component is a pack expansion, this
/// is the index of the first element of the pack (or where it would be
/// if it had any elements).
llvm::Value *emitIndexOfStructuralPackComponent(IRGenFunction &IGF,
                                                CanPackType packType,
                                                unsigned componentIndex);

/// Emit the address that stores the given pack element.
///
/// For indirect packs, note that this is the address of the pack
/// array element, not the address stored in the pack array element.
Address emitStorageAddressOfPackElement(IRGenFunction &IGF, Address pack,
                                        llvm::Value *index, SILType elementType,
                                        CanSILPackType packType);

Size getPackElementSize(IRGenModule &, CanSILPackType ty);

StackAddress allocatePack(IRGenFunction &IGF, CanSILPackType packType);

void deallocatePack(IRGenFunction &IGF, StackAddress addr, CanSILPackType packType);

llvm::Optional<StackAddress>
emitDynamicTupleTypeLabels(IRGenFunction &IGF,
                           CanTupleType tupleType,
                           CanPackType packType,
                           llvm::Value *shapeExpression);

StackAddress
emitDynamicFunctionParameterFlags(IRGenFunction &IGF,
                                  AnyFunctionType::CanParamArrayRef params,
                                  CanPackType packType,
                                  llvm::Value *shapeExpression);

} // end namespace irgen
} // end namespace swift

#endif
