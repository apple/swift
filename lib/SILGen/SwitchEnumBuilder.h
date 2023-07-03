//===--- SwitchEnumBuilder.h ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_SWITCHENUMBUILDER_H
#define SWIFT_SILGEN_SWITCHENUMBUILDER_H

#include "ArgumentScope.h"
#include "Scope.h"

namespace swift {
namespace Lowering {

class SILGenFunction;

/// TODO: std::variant.
struct SwitchCaseBranchDest {
  llvm::Optional<JumpDest> jumpDest;
  NullablePtr<SILBasicBlock> block;

  SwitchCaseBranchDest() : jumpDest(), block() {}

  /// Implicit conversion.
  SwitchCaseBranchDest(JumpDest jumpDest) : jumpDest(jumpDest), block() {}

  /// Implicit conversion.
  SwitchCaseBranchDest(SILBasicBlock *block) : jumpDest(), block(block) {}

  explicit operator bool() const {
    return jumpDest.has_value() || block.isNonNull();
  }

  bool hasJumpDest() const { return jumpDest.has_value(); }
  bool hasBlock() const { return bool(block); }

  SILBasicBlock *getBlock() { return block.getPtrOrNull(); }
  JumpDest &getJumpDest() { return jumpDest.value(); }
};

/// A cleanup scope RAII object, like FullExpr, that comes with a JumpDest for a
/// continuation block. It is intended to be used to handle switch cases.
///
/// You *must* call exit() at some point.
///
/// This scope is also exposed to the debug info.
class SwitchCaseFullExpr {
  SILGenFunction &SGF;
  ArgumentScope scope;
  CleanupLocation loc;
  SwitchCaseBranchDest branchDest;

public:
  SwitchCaseFullExpr(SILGenFunction &SGF, CleanupLocation loc);
  SwitchCaseFullExpr(SILGenFunction &SGF, CleanupLocation loc,
                     SwitchCaseBranchDest branchDest);

  ~SwitchCaseFullExpr();

  SwitchCaseFullExpr(const SwitchCaseFullExpr &) = delete;
  SwitchCaseFullExpr &operator=(const SwitchCaseFullExpr &) = delete;

  /// Pop the scope and branch to the branch destination. If this case has an
  /// associated JumpDest as its branch destination, then this will cause
  /// cleanups associated with the jump dest to be emitted.
  void exitAndBranch(SILLocation loc, ArrayRef<SILValue> result = {});

  /// Pop the scope and do not branch to the branch destination. This is
  /// intended to be used in situations where one wants to model a switch region
  /// that ends midway through one of the case blocks.
  void exit();

  /// Do not pop the scope and do not branch to the branch destination. But do
  /// invalidate the scope. This can occur when emitting unreachables.
  void unreachableExit();
};

/// A class for building switch enums that handles all of the ownership
/// requirements for the user.
///
/// It assumes that the user passes in a block that takes in a ManagedValue and
/// returns a ManagedValue for the blocks exit argument. Should return an empty
/// ManagedValue to signal no result.
///
/// TODO: Allow cases to take JumpDest as continuation blocks and then
/// force exitBranchAndCleanup to be run.
class SwitchEnumBuilder {
public:
  // The ManagedValue argument will be invalid for no-payload cases.
  using NormalCaseHandler =
      std::function<void(ManagedValue, SwitchCaseFullExpr &&)>;
  using DefaultCaseHandler =
      std::function<void(ManagedValue, SwitchCaseFullExpr &&)>;

  enum class DefaultDispatchTime { BeforeNormalCases, AfterNormalCases };

private:
  struct NormalCaseData {
    EnumElementDecl *decl;
    SILBasicBlock *block;
    SwitchCaseBranchDest branchDest;
    NormalCaseHandler handler;
    ProfileCounter count;

    NormalCaseData(EnumElementDecl *decl, SILBasicBlock *block,
                   SwitchCaseBranchDest branchDest, NormalCaseHandler handler,
                   ProfileCounter count)
        : decl(decl), block(block), branchDest(branchDest), handler(handler),
          count(count) {}
    ~NormalCaseData() = default;
  };

  struct DefaultCaseData {
    SILBasicBlock *block;
    SwitchCaseBranchDest branchDest;
    DefaultCaseHandler handler;
    DefaultDispatchTime dispatchTime;
    ProfileCounter count;

    DefaultCaseData(SILBasicBlock *block, SwitchCaseBranchDest branchDest,
                    DefaultCaseHandler handler,
                    DefaultDispatchTime dispatchTime, ProfileCounter count)
        : block(block), branchDest(branchDest), handler(handler),
          dispatchTime(dispatchTime), count(count) {}
    ~DefaultCaseData() = default;
  };

  SILGenBuilder &builder;
  SILLocation loc;
  ManagedValue subjectExprOperand;
  llvm::Optional<DefaultCaseData> defaultBlockData;
  llvm::SmallVector<NormalCaseData, 8> caseDataArray;

public:
  SwitchEnumBuilder(SILGenBuilder &builder, SILLocation loc,
                    ManagedValue subjectExprOperand)
      : builder(builder), loc(loc), subjectExprOperand(subjectExprOperand) {}

  void addDefaultCase(
      SILBasicBlock *defaultBlock, SwitchCaseBranchDest branchDest,
      DefaultCaseHandler handle,
      DefaultDispatchTime dispatchTime = DefaultDispatchTime::AfterNormalCases,
      ProfileCounter count = ProfileCounter()) {
    defaultBlockData.emplace(defaultBlock, branchDest, handle, dispatchTime,
                             count);
  }

  void addCase(EnumElementDecl *decl, SILBasicBlock *caseBlock,
               SwitchCaseBranchDest branchDest, NormalCaseHandler handle,
               ProfileCounter count = ProfileCounter()) {
    caseDataArray.emplace_back(decl, caseBlock, branchDest, handle, count);
  }

  void addOptionalSomeCase(SILBasicBlock *caseBlock) {
    auto *decl = getSGF().getASTContext().getOptionalSomeDecl();
    caseDataArray.emplace_back(
        decl, caseBlock, nullptr,
        [](ManagedValue mv, SwitchCaseFullExpr &&expr) { expr.exit(); },
        ProfileCounter());
  }

  void addOptionalNoneCase(SILBasicBlock *caseBlock) {
    auto *decl = getSGF().getASTContext().getOptionalNoneDecl();
    caseDataArray.emplace_back(
        decl, caseBlock, nullptr,
        [](ManagedValue mv, SwitchCaseFullExpr &&expr) { expr.exit(); },
        ProfileCounter());
  }

  void addOptionalSomeCase(SILBasicBlock *caseBlock,
                           SwitchCaseBranchDest branchDest,
                           NormalCaseHandler handle,
                           ProfileCounter count = ProfileCounter()) {
    auto *decl = getSGF().getASTContext().getOptionalSomeDecl();
    caseDataArray.emplace_back(decl, caseBlock, branchDest, handle, count);
  }

  void addOptionalNoneCase(SILBasicBlock *caseBlock,
                           SwitchCaseBranchDest branchDest,
                           NormalCaseHandler handle,
                           ProfileCounter count = ProfileCounter()) {
    auto *decl = getSGF().getASTContext().getOptionalNoneDecl();
    caseDataArray.emplace_back(decl, caseBlock, branchDest, handle, count);
  }

  void emit() &&;

private:
  SILGenFunction &getSGF() const { return builder.getSILGenFunction(); }

  ManagedValue emitDefaultCase(SwitchEnumInst *switchEnum);
};

} // end Lowering namespace
} // end swift namespace

#endif
