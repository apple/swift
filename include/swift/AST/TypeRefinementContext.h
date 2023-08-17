//===--- TypeRefinementContext.h - Swift Refinement Context -----*- C++ -*-===//
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
// This file defines the TypeRefinementContext class.  A TypeRefinementContext
// is the semantic construct that refines a type within its lexical scope.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPEREFINEMENTCONTEXT_H
#define SWIFT_TYPEREFINEMENTCONTEXT_H

#include "swift/AST/Identifier.h"
#include "swift/AST/Availability.h"
#include "swift/AST/Stmt.h" // for PoundAvailableInfo
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {
  class BraceStmt;
  class Decl;
  class IfStmt;
  class GuardStmt;
  class SourceFile;
  class Stmt;
  class Expr;
  class StmtConditionElement;

/// Represents a lexical context in which types are refined. For now,
/// types are refined solely for API availability checking, based on
/// the operating system versions that the refined context may execute
/// upon.
///
/// These refinement contexts form a lexical tree parallel to the AST but much
/// more sparse: we only introduce refinement contexts when there is something
/// to refine.
class TypeRefinementContext : public ASTAllocated<TypeRefinementContext> {

public:
  /// Describes the reason a type refinement context was introduced.
  enum class Reason {
    /// The root refinement context.
    Root,

    /// The context was introduced by a declaration with an explicit
    /// availability attribute. The context contains both the signature and the
    /// body of the declaration.
    Decl,

    /// The context was introduced implicitly by a declaration. The context may
    /// cover the entire declaration or it may cover a subset of it. For
    /// example, a public, non-inlinable function declaration in an API module
    /// will have at least two associated contexts: one for the entire
    /// declaration at the declared availability of the API and a nested
    /// implicit context for the body of the function, which will always run at
    /// the deployment target of the library.
    DeclImplicit,

    /// The context was introduced for the Then branch of an IfStmt.
    IfStmtThenBranch,

    /// The context was introduced for the Else branch of an IfStmt.
    IfStmtElseBranch,

    /// The context was introduced for the remaining StmtConditionElements
    /// following an #available(...) query in a StmtCondition.
    /// For example, in the IfStmt below, the optional binding let x = expr()
    /// would be contained in this kind of context:
    ///
    /// if #available(...),
    ///    let x = expr() {
    ///  }
    ConditionFollowingAvailabilityQuery,

    /// The context was introduced for the fallthrough flow of a guard
    /// statement.
    GuardStmtFallthrough,

    /// The context was introduced for the else flow of a guard
    /// statement.
    GuardStmtElseBranch,

    // The context was introduced for the body of a while statement.
    WhileStmtBody
  };

private:

  /// Represents the AST node that introduced a refinement context.
  class IntroNode {
    Reason IntroReason;
    union {
      SourceFile *SF;
      Decl *D;
      IfStmt *IS;
      PoundAvailableInfo *PAI;
      GuardStmt *GS;
      WhileStmt *WS;
    };

  public:
    IntroNode(SourceFile *SF) : IntroReason(Reason::Root), SF(SF) {}
    IntroNode(Decl *D, Reason introReason = Reason::Decl)
        : IntroReason(introReason), D(D) {
      (void)getAsDecl();    // check that assertion succeeds
    }
    IntroNode(IfStmt *IS, bool IsThen) :
    IntroReason(IsThen ? Reason::IfStmtThenBranch : Reason::IfStmtElseBranch),
                IS(IS) {}
    IntroNode(PoundAvailableInfo *PAI)
        : IntroReason(Reason::ConditionFollowingAvailabilityQuery), PAI(PAI) {}
    IntroNode(GuardStmt *GS, bool IsFallthrough)
        : IntroReason(IsFallthrough ? Reason::GuardStmtFallthrough
                                    : Reason::GuardStmtElseBranch),
          GS(GS) {}
    IntroNode(WhileStmt *WS) : IntroReason(Reason::WhileStmtBody), WS(WS) {}

    Reason getReason() const { return IntroReason; }

    SourceFile *getAsSourceFile() const {
      assert(IntroReason == Reason::Root);
      return SF;
    }

    Decl *getAsDecl() const {
      assert(IntroReason == Reason::Decl ||
             IntroReason == Reason::DeclImplicit);
      return D;
    }

    IfStmt *getAsIfStmt() const {
      assert(IntroReason == Reason::IfStmtThenBranch ||
             IntroReason == Reason::IfStmtElseBranch);
      return IS;
    }

    PoundAvailableInfo *getAsPoundAvailableInfo() const {
      assert(IntroReason == Reason::ConditionFollowingAvailabilityQuery);
      return PAI;
    }

    GuardStmt *getAsGuardStmt() const {
      assert(IntroReason == Reason::GuardStmtFallthrough ||
             IntroReason == Reason::GuardStmtElseBranch);
      return GS;
    }

    WhileStmt *getAsWhileStmt() const {
      assert(IntroReason == Reason::WhileStmtBody);
      return WS;
    }
  };

  /// The AST node that introduced this context.
  IntroNode Node;

  SourceRange SrcRange;

  /// A canonical availability info for this context, computed top-down from the
  /// root context.
  AvailabilityContext AvailabilityInfo;

  /// If this context was annotated with an availability attribute, this property captures that.
  /// It differs from the above `AvailabilityInfo` by being independent of the deployment target,
  /// and is used for providing availability attribute redundancy warning diagnostics.
  AvailabilityContext ExplicitAvailabilityInfo;

  std::vector<TypeRefinementContext *> Children;

  TypeRefinementContext(ASTContext &Ctx, IntroNode Node,
                        TypeRefinementContext *Parent, SourceRange SrcRange,
                        const AvailabilityContext &Info,
                        const AvailabilityContext &ExplicitInfo);

public:
  
  /// Create the root refinement context for the given SourceFile.
  static TypeRefinementContext *createRoot(SourceFile *SF,
                                           const AvailabilityContext &Info);

  /// Create a refinement context for the given declaration.
  static TypeRefinementContext *createForDecl(ASTContext &Ctx, Decl *D,
                                              TypeRefinementContext *Parent,
                                              const AvailabilityContext &Info,
                                              const AvailabilityContext &ExplicitInfo,
                                              SourceRange SrcRange);

  /// Create a refinement context for the given declaration.
  static TypeRefinementContext *
  createForDeclImplicit(ASTContext &Ctx, Decl *D, TypeRefinementContext *Parent,
                        const AvailabilityContext &Info, SourceRange SrcRange);

  /// Create a refinement context for the Then branch of the given IfStmt.
  static TypeRefinementContext *
  createForIfStmtThen(ASTContext &Ctx, IfStmt *S, TypeRefinementContext *Parent,
                      const AvailabilityContext &Info);

  /// Create a refinement context for the Else branch of the given IfStmt.
  static TypeRefinementContext *
  createForIfStmtElse(ASTContext &Ctx, IfStmt *S, TypeRefinementContext *Parent,
                      const AvailabilityContext &Info);

  /// Create a refinement context for the true-branch control flow to
  /// further StmtConditionElements following a #available() query in
  /// a StmtCondition.
  static TypeRefinementContext *
  createForConditionFollowingQuery(ASTContext &Ctx, PoundAvailableInfo *PAI,
                                   const StmtConditionElement &LastElement,
                                   TypeRefinementContext *Parent,
                                   const AvailabilityContext &Info);

  /// Create a refinement context for the fallthrough of a GuardStmt.
  static TypeRefinementContext *
  createForGuardStmtFallthrough(ASTContext &Ctx, GuardStmt *RS,
                                  BraceStmt *ContainingBraceStmt,
                                  TypeRefinementContext *Parent,
                                  const AvailabilityContext &Info);

  /// Create a refinement context for the else branch of a GuardStmt.
  static TypeRefinementContext *
  createForGuardStmtElse(ASTContext &Ctx, GuardStmt *RS,
                         TypeRefinementContext *Parent,
                         const AvailabilityContext &Info);

  /// Create a refinement context for the body of a WhileStmt.
  static TypeRefinementContext *
  createForWhileStmtBody(ASTContext &Ctx, WhileStmt *WS,
                         TypeRefinementContext *Parent,
                         const AvailabilityContext &Info);

  /// Returns the reason this context was introduced.
  Reason getReason() const;
  
  /// Returns the AST node that introduced this refinement context. Note that
  /// this node may be different than the refined range. For example, a
  /// refinement context covering an IfStmt Then branch will have the
  /// IfStmt as the introduction node (and its reason as IfStmtThenBranch)
  /// but its source range will cover the Then branch.
  IntroNode getIntroductionNode() const { return Node; }
  
  /// Returns the location of the node that introduced this refinement context
  /// or an invalid location if the context reflects the minimum deployment
  // target.
  SourceLoc getIntroductionLoc() const;

  /// Returns the source range covering a _single_ decl-attribute or statement
  /// condition that introduced the refinement context for a given platform
  /// version; if zero or multiple such responsible attributes or statements
  /// exist, returns an invalid SourceRange.
  SourceRange
  getAvailabilityConditionVersionSourceRange(
      PlatformKind Platform,
      const llvm::VersionTuple &Version) const;

  /// Returns the source range on which this context refines types.
  SourceRange getSourceRange() const { return SrcRange; }

  /// Returns the information on what can be assumed present at run time when
  /// running code contained in this context.
  const AvailabilityContext &getAvailabilityInfo() const {
    return AvailabilityInfo;
  }

  /// Returns the information on what availability was specified by the programmer
  /// on this context (if any).
  const AvailabilityContext &getExplicitAvailabilityInfo() const {
    return ExplicitAvailabilityInfo;
  }

  /// Adds a child refinement context.
  void addChild(TypeRefinementContext *Child) {
    assert(Child->getSourceRange().isValid());
    Children.push_back(Child);
  }

  /// Returns the inner-most TypeRefinementContext descendant of this context
  /// for the given source location.
  TypeRefinementContext *findMostRefinedSubContext(SourceLoc Loc,
                                                   SourceManager &SM);

  SWIFT_DEBUG_DUMPER(dump(SourceManager &SrcMgr));
  void dump(raw_ostream &OS, SourceManager &SrcMgr) const;
  void print(raw_ostream &OS, SourceManager &SrcMgr, unsigned Indent = 0) const;
  
  static StringRef getReasonName(Reason R);
};

void simple_display(llvm::raw_ostream &out,
                    const TypeRefinementContext *trc);

} // end namespace swift

#endif
