//===--- Devirtualize.h - Helper for devirtualizing apply -------*- C++ -*-===//
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
// This contains helper functions that perform the work of devirtualizing a
// given apply when possible.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_DEVIRTUALIZE_H
#define SWIFT_SIL_DEVIRTUALIZE_H

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/ArrayRef.h"

namespace swift {
namespace OptRemark {
class Emitter;
}

/// Compute all subclasses of a given class.
///
/// \p CHA class hierarchy analysis
/// \p CD class declaration
/// \p ClassType type of the instance
/// \p M SILModule
/// \p Subs a container to be used for storing the set of subclasses
void getAllSubclasses(ClassHierarchyAnalysis *CHA,
                      ClassDecl *CD,
                      CanType ClassType,
                      SILModule &M,
                      ClassHierarchyAnalysis::ClassList &Subs);

/// Given an apply instruction of a protocol requirement and a witness method
/// for the requirement, compute a substitution suitable for a direct call
/// to the witness method.
///
/// \p Module SILModule
/// \p AI ApplySite that applies a protocol method
/// \p F SILFunction with convention @convention(witness_method)
/// \p CRef a concrete ProtocolConformanceRef
SubstitutionMap getWitnessMethodSubstitutions(SILModule &Module, ApplySite AI,
                                              SILFunction *F,
                                              ProtocolConformanceRef CRef);

/// Attempt to devirtualize the given apply site.  If this fails,
/// the returned ApplySite will be null.
///
/// If this succeeds, the caller must call deleteDevirtualizedApply on
/// the original apply site.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<ApplySite, bool>
tryDevirtualizeApply(SILPassManager *pm, ApplySite AI, ClassHierarchyAnalysis *CHA,
                     OptRemark::Emitter *ORE = nullptr,
                     bool isMandatory = false);
bool canDevirtualizeApply(FullApplySite AI, ClassHierarchyAnalysis *CHA);
bool canDevirtualizeClassMethod(FullApplySite AI, ClassDecl *CD,
                                CanType ClassType,
                                OptRemark::Emitter *ORE = nullptr,
                                bool isEffectivelyFinalMethod = false);
SILFunction *getTargetClassMethod(SILModule &M, ClassDecl *CD,
                                  CanType ClassType, MethodInst *MI);
CanType getSelfInstanceType(CanType ClassOrMetatypeType);

/// Devirtualize the given apply site, which is known to be devirtualizable.
///
/// The caller must call deleteDevirtualizedApply on the original apply site.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<FullApplySite, bool>
devirtualizeClassMethod(SILPassManager *pm,
                        FullApplySite AI, SILValue ClassInstance, ClassDecl *CD,
                        CanType classType, OptRemark::Emitter *ORE);

/// Attempt to devirtualize the given apply site, which is known to be
/// of a class method.  If this fails, the returned FullApplySite will be null.
///
/// If this succeeds, the caller must call deleteDevirtualizedApply on
/// the original apply site.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<FullApplySite, bool>
tryDevirtualizeClassMethod(SILPassManager *pm,
                           FullApplySite AI, SILValue ClassInstance,
                           ClassDecl *CD, CanType ClassType,
                           OptRemark::Emitter *ORE,
                           bool isEffectivelyFinalMethod = false);

/// Attempt to devirtualize the given apply site, which is known to be
/// of a witness method.  If this fails, the returned FullApplySite
/// will be null.
///
/// If this succeeds, the caller must call deleteDevirtualizedApply on
/// the original apply site.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<ApplySite, bool> tryDevirtualizeWitnessMethod(SILPassManager *pm,
                                                        ApplySite AI,
                                                        OptRemark::Emitter *ORE,
                                                        bool isMandatory);

/// Delete a successfully-devirtualized apply site.  This must always be
/// called after devirtualizing an apply; not only is it not semantically
/// equivalent to leave the old apply in-place, but the SIL isn't necessary
/// well-formed.
///
/// Devirtualization is responsible for replacing uses of the original
/// apply site with uses of the new one.  The only thing this does is delete
/// the instruction and any now-trivially-dead operands; it is separated
/// from the actual devirtualization step only to apply the caller to log
/// information about the original apply site.  This is probably not a
/// good enough reason to complicate the API.
void deleteDevirtualizedApply(ApplySite AI);

} // end namespace swift

#endif
