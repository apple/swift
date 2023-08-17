//===--- TypeCheckSwitchStmt.cpp - Switch Exhaustiveness and Type Checks --===//
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
//
// This file contains an algorithm for checking the exhaustiveness of switches.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Pattern.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/STLExtras.h"

#include "swift/Basic/APIntMap.h"
#include <llvm/ADT/APFloat.h>

#include <forward_list>
#include <iterator>
#include <numeric>
#include <utility>

using namespace swift;

#define DEBUG_TYPE "TypeCheckSwitchStmt"

namespace {
  struct DenseMapAPFloatKeyInfo {
    static inline APFloat getEmptyKey() { return APFloat(APFloat::Bogus(), 1); }
    static inline APFloat getTombstoneKey() { return APFloat(APFloat::Bogus(), 2); }
    
    static unsigned getHashValue(const APFloat &Key) {
      return static_cast<unsigned>(hash_value(Key));
    }
    
    static bool isEqual(const APFloat &LHS, const APFloat &RHS) {
      return LHS.bitwiseIsEqual(RHS);
    }
  };
}

namespace {

  /// The SpaceEngine encapsulates
  ///
  /// 1. An algorithm for computing the exhaustiveness of a switch statement
  ///    using an algebra of spaces based on Fengyun Liu's
  ///    "A Generic Algorithm for Checking Exhaustivity of Pattern Matching".
  /// 2. An algorithm for computing warnings for pattern matching based on
  ///    Luc Maranget's "Warnings for pattern matching".
  ///
  /// The main algorithm centers around the computation of the difference and
  /// the containment of the "Spaces" given in each case, which reduces the
  /// definition of exhaustiveness to checking if the difference of the space
  /// 'S' of the user's written patterns and the space 'T' of the pattern
  /// condition is empty.
  struct SpaceEngine {
    enum class SpaceKind : uint8_t {
      Empty,
      Type,
      Constructor,
      Disjunct,
      BooleanConstant,
      UnknownCase,
    };

    // The order of cases is used for disjunctions: a disjunction's
    // DowngradeToWarning condition is the std::min of its spaces'.
    enum class DowngradeToWarning {
      No,
      ForUnknownCase,

      LAST = ForUnknownCase
    };

    enum UnknownCase_t {
      UnknownCase
    };

    /// A data structure for conveniently pattern-matching on the kinds of
    /// two spaces.
    struct PairSwitch {
    public:
      constexpr PairSwitch(SpaceKind pair1, SpaceKind pair2)
        : Data(static_cast<uint8_t>(pair1) | (static_cast<uint8_t>(pair2) << 8))
        {}

      constexpr bool operator==(const PairSwitch other) const {
        return Data == other.Data;
      }

      constexpr operator int() const {
        return Data;
      }

    private:
      uint16_t Data;

      PairSwitch (const PairSwitch&) = delete;
      PairSwitch& operator= (const PairSwitch&) = delete;
    };

  #define PAIRCASE(XS, YS) case PairSwitch(XS, YS)
    
    class Space final : public RelationalOperationsBase<Space> {
    private:
      SpaceKind Kind;
      llvm::PointerIntPair<Type, 1, bool> TypeAndVal;

      // In type space, we reuse HEAD to help us print meaningful name, e.g.,
      // tuple element name in fixits.
      DeclName Head;
      std::forward_list<Space> Spaces;

      size_t computeSize(const DeclContext *DC,
                         SmallPtrSetImpl<TypeBase *> &cache) const {
        switch (getKind()) {
        case SpaceKind::Empty:
          return 0;
        case SpaceKind::BooleanConstant:
          return 1;
        case SpaceKind::UnknownCase:
          return isAllowedButNotRequired() ? 0 : 1;
        case SpaceKind::Type: {
          if (!canDecompose(getType())) {
            return 1;
          }
          cache.insert(getType().getPointer());

          SmallVector<Space, 4> spaces;
          decomposeDisjuncts(DC, getType(), {}, spaces);
          size_t acc = 0;
          for (auto &sp : spaces) {
            // Decomposed pattern spaces grow with the sum of the subspaces.
            acc += sp.computeSize(DC, cache);
          }
          
          cache.erase(getType().getPointer());
          return acc;
        }
        case SpaceKind::Constructor: {
          size_t acc = 1;
          for (auto &sp : getSpaces()) {
            // Break self-recursive references among enum arguments.
            if (sp.getKind() == SpaceKind::Type
                  && cache.count(sp.getType().getPointer())) {
              continue;
            }
            
            // Constructor spaces grow with the product of their arguments.
            acc *= sp.computeSize(DC, cache);
          }
          return acc;
        }
        case SpaceKind::Disjunct: {
          size_t acc = 0;
          for (auto &sp : getSpaces()) {
            // Disjoint grow with the sum of the subspaces.
            acc += sp.computeSize(DC, cache);
          }
          return acc;
        }
        }
        llvm_unreachable("unhandled kind");
      }

      explicit Space(Type T, DeclName NameForPrinting)
          : Kind(SpaceKind::Type), TypeAndVal(T), Head(NameForPrinting),
            Spaces({}) {}
      explicit Space(UnknownCase_t, bool allowedButNotRequired)
        : Kind(SpaceKind::UnknownCase),
          TypeAndVal(Type(), allowedButNotRequired), Head(Identifier()),
          Spaces({}) {}
      explicit Space(Type T, DeclName H, ArrayRef<Space> SP)
          : Kind(SpaceKind::Constructor), TypeAndVal(T), Head(H),
            Spaces(SP.begin(), SP.end()) {}
      explicit Space(Type T, DeclName H, std::forward_list<Space> SP)
          : Kind(SpaceKind::Constructor), TypeAndVal(T), Head(H), Spaces(SP) {}
      explicit Space(ArrayRef<Space> SP)
        : Kind(SpaceKind::Disjunct), TypeAndVal(Type()),
          Head(Identifier()), Spaces(SP.begin(), SP.end()) {}
      explicit Space(bool C)
        : Kind(SpaceKind::BooleanConstant), TypeAndVal(Type(), C),
          Head(Identifier()), Spaces({}) {}
    public:
      explicit Space()
        : Kind(SpaceKind::Empty), TypeAndVal(Type()), Head(Identifier()),
          Spaces({}) {}

      static Space forType(Type T, DeclName NameForPrinting) {
        if (T->isStructurallyUninhabited())
          return Space();
        return Space(T, NameForPrinting);
      }
      static Space forUnknown(bool allowedButNotRequired) {
        return Space(UnknownCase, allowedButNotRequired);
      }
      static Space forConstructor(Type T, DeclName H, ArrayRef<Space> SP) {
        if (llvm::any_of(SP, std::mem_fn(&Space::isEmpty))) {
          // A constructor with an unconstructable parameter can never actually
          // be used.
          return Space();
        }
        return Space(T, H, SP);
      }
      static Space forBool(bool C) {
        return Space(C);
      }
      static Space forDisjunct(ArrayRef<Space> SP) {
        SmallVector<Space, 4> spaces(SP.begin(), SP.end());
        spaces.erase(
            std::remove_if(spaces.begin(), spaces.end(),
                           [](const Space &space) { return space.isEmpty(); }),
            spaces.end());

        if (spaces.empty())
          return Space();
        if (spaces.size() == 1)
          return spaces.front();

        return Space(spaces);
      }

      bool operator==(const Space &other) const {
        return Kind == other.Kind && TypeAndVal == other.TypeAndVal &&
               Head == other.Head && Spaces == other.Spaces;
      }

      SpaceKind getKind() const { return Kind; }

      SWIFT_DEBUG_DUMP;

      size_t getSize(const DeclContext *DC) const {
        SmallPtrSet<TypeBase *, 4> cache;
        return computeSize(DC, cache);
      }

      bool isEmpty() const { return getKind() == SpaceKind::Empty; }

      bool isAllowedButNotRequired() const {
        assert(getKind() == SpaceKind::UnknownCase
               && "Wrong kind of space tried to access not-required flag");
        return TypeAndVal.getInt();
      }

      Type getType() const {
        assert((getKind() == SpaceKind::Type
                || getKind() == SpaceKind::Constructor)
               && "Wrong kind of space tried to access space type");
        return TypeAndVal.getPointer();
      }

      DeclName getHead() const {
        assert(getKind() == SpaceKind::Constructor
               && "Wrong kind of space tried to access head");
        return Head;
      }

      Identifier getPrintingName() const {
        assert(getKind() == SpaceKind::Type
               && "Wrong kind of space tried to access printing name");
        return Head.getBaseIdentifier();
      }

      const std::forward_list<Space> &getSpaces() const {
        assert((getKind() == SpaceKind::Constructor
                || getKind() == SpaceKind::Disjunct)
               && "Wrong kind of space tried to access subspace list");
        return Spaces;
      }

      bool getBoolValue() const {
        assert(getKind() == SpaceKind::BooleanConstant
                && "Wrong kind of space tried to access bool value");
        return TypeAndVal.getInt();
      }

      // An optimization that computes if the difference of this space and
      // another space is empty.
      bool isSubspace(const Space &other, const DeclContext *DC) const {
        if (this->isEmpty()) {
          return true;
        }

        if (other.isEmpty()) {
          return false;
        }

        switch (PairSwitch(getKind(), other.getKind())) {
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Type):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::UnknownCase): {
          // (S1 | ... | Sn) <= S iff (S1 <= S) && ... && (Sn <= S)
          for (auto &space : this->getSpaces()) {
            if (!space.isSubspace(other, DC)) {
              return false;
            }
          }
          return true;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Type): {
          // Optimization: Are the types equal?  If so, the space is covered.
          if (this->getType()->isEqual(other.getType())) {
            return true;
          }

          // (_ : Ty1) <= (_ : Ty2) iff D(Ty1) == D(Ty2)
          if (canDecompose(this->getType())) {
            Space or1Space = decompose(DC, this->getType(), {});
            if (or1Space.isSubspace(other, DC)) {
              return true;
            }
          }

          if (canDecompose(other.getType())) {
            Space or2Space = decompose(DC, other.getType(), {});
            return this->isSubspace(or2Space, DC);
          }

          return false;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Disjunct): {
          // (_ : Ty1) <= (S1 | ... | Sn) iff (S1 <= S) || ... || (Sn <= S)
          for (auto &dis : other.getSpaces()) {
            if (this->isSubspace(dis, DC)) {
              return true;
            }
          }

          // (_ : Ty1) <= (S1 | ... | Sn) iff D(Ty1) <= (S1 | ... | Sn)
          if (!canDecompose(this->getType())) {
            return false;
          }
          Space or1Space = decompose(DC, this->getType(), {});
          return or1Space.isSubspace(other, DC);
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Constructor): {
          // (_ : Ty1) <= H(p1 | ... | pn) iff D(Ty1) <= H(p1 | ... | pn)
          if (canDecompose(this->getType())) {
            Space or1Space = decompose(DC, this->getType(), {});
            return or1Space.isSubspace(other, DC);
          }
          // An undecomposable type is always larger than its constructor space.
          return false;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::UnknownCase):
          return false;

        PAIRCASE (SpaceKind::Constructor, SpaceKind::Type):
          // Typechecking guaranteed this constructor is a subspace of the type.
          return true;
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Type):
          return true;
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Type):
          return other.getType()->isBool();
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Constructor): {
          // Optimization: If the constructor heads don't match, subspace is
          // impossible.

          if (this->Head != other.Head) {
            return false;
          }

          // Special Case: Short-circuit comparisons with payload-less
          // constructors.
          if (other.getSpaces().empty()) {
            return true;
          }

          // H(a1, ..., an) <= H(b1, ..., bn) iff a1 <= b1 && ... && an <= bn
          auto i = this->getSpaces().begin();
          auto j = other.getSpaces().begin();
          for (; i != this->getSpaces().end() && j != other.getSpaces().end();
               ++i, ++j) {
            if (!(*i).isSubspace(*j, DC)) {
              return false;
            }
          }
          return true;
        }
        PAIRCASE (SpaceKind::Constructor, SpaceKind::UnknownCase):
          for (auto &param : this->getSpaces()) {
            if (param.isSubspace(other, DC)) {
              return true;
            }
          }
          return false;

        PAIRCASE (SpaceKind::Constructor, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Disjunct): {
          // S <= (S1 | ... | Sn) <= S iff (S <= S1) || ... || (S <= Sn)
          for (auto &param : other.getSpaces()) {
            if (this->isSubspace(param, DC)) {
              return true;
            }
          }
          return false;
        }

        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::BooleanConstant):
          return this->getBoolValue() == other.getBoolValue();

        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::UnknownCase):
          return false;

        PAIRCASE (SpaceKind::Empty, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Type, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::BooleanConstant):
          return false;

        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Constructor):
          return false;

        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::UnknownCase):
          if (other.isAllowedButNotRequired())
            return this->isAllowedButNotRequired();
          return true;

        default:
          llvm_unreachable("Uncovered pair found while computing subspaces?");
        }
      }

      // Returns the result of subtracting the other space from this space.  The
      // result is empty if the other space completely covers this space, or
      // non-empty if there were any uncovered cases.  The difference of spaces
      // is the smallest uncovered set of cases. The result is absent if the
      // computation had to be abandoned.
      //
      // \p minusCount is an optional pointer counting the number of
      // remaining calls to minus before the computation times out.
      // Returns None if the computation "timed out".
      llvm::Optional<Space> minus(const Space &other, const DeclContext *DC,
                                  unsigned *minusCount) const {
        if (minusCount && (*minusCount)-- == 0)
          return llvm::None;

        if (this->isEmpty()) {
          return Space();
        }

        if (other.isEmpty()) {
          return *this;
        }

        switch (PairSwitch(this->getKind(), other.getKind())) {
        PAIRCASE (SpaceKind::Type, SpaceKind::Type): {
          if (this->getType()->isEqual(other.getType())) {
            return Space();
          }
          return *this;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Constructor): {
          if (canDecompose(this->getType())) {
            auto decomposition = decompose(DC, this->getType(), {});
            return decomposition.minus(other, DC, minusCount);
          } else {
            return *this;
          }
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::UnknownCase):
          // Note: This is not technically correct for decomposable types, but
          // you'd only get "typeSpace - unknownCaseSpace" if you haven't tried
          // to match any of the decompositions of the space yet. In that case,
          // we'd rather not expand the type, because it might be infinitely
          // decomposable.
          return *this;

        PAIRCASE (SpaceKind::Type, SpaceKind::Disjunct): {
          // Optimize for the common case of a type minus a disjunct of
          // constructor subspaces. This form of subtraction is guaranteed to
          // happen very early on, and we can eliminate a huge part of the
          // pattern space by only decomposing the parts of the type space that
          // aren't actually covered by the disjunction.
          if (canDecompose(this->getType())) {
            llvm::StringSet<> otherConstructors;
            for (auto s : other.getSpaces()) {
              // Filter for constructor spaces with no payloads.
              if (s.getKind() != SpaceKind::Constructor) {
                continue;
              }

              if (!s.getSpaces().empty()) {
                continue;
              }

              otherConstructors.insert(s.Head.getBaseIdentifier().str());
            }

            auto decomposition = decompose(DC, this->getType(),
                                           otherConstructors);
            return decomposition.minus(other, DC, minusCount);
          } else {
            // If the type isn't decomposable then there's no way we can
            // subtract from it. Report the total space as uncovered.
            return *this;
          }
        }

        PAIRCASE (SpaceKind::Empty, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Disjunct): {
          Space tot = *this;
          for (auto s : other.getSpaces()) {
            if (auto diff = tot.minus(s, DC, minusCount))
              tot = *diff;
            else
              return llvm::None;
          }
          return tot;
        }

        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Empty):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Type):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::UnknownCase): {
          SmallVector<Space, 4> smallSpaces;
          for (auto s : this->getSpaces()) {
            auto diff = s.minus(other, DC, minusCount);
            if (!diff)
              return llvm::None;
            if (diff->getKind() == SpaceKind::Disjunct) {
              smallSpaces.append(diff->getSpaces().begin(),
                                 diff->getSpaces().end());
            } else {
              smallSpaces.push_back(*diff);
            }
          }

          // Remove any of the later spaces that are contained entirely in an
          // earlier one. Since we're not sorting by size, this isn't
          // guaranteed to give us a minimal set, but it'll still reduce the
          // general (A, B, C) - ((.a1, .b1, .c1) | (.a1, .b1, .c2)) problem.
          // This is a quadratic operation but it saves us a LOT of work
          // overall.
          SmallVector<Space, 4> usefulSmallSpaces;
          for (const Space &space : smallSpaces) {
            bool alreadyHandled = llvm::any_of(usefulSmallSpaces,
                                               [&](const Space &previousSpace) {
              return space.isSubspace(previousSpace, DC);
            });
            if (alreadyHandled)
              continue;
            usefulSmallSpaces.push_back(space);
          }

          return Space::forDisjunct(usefulSmallSpaces);
        }
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Type):
          return Space();
        PAIRCASE (SpaceKind::Constructor, SpaceKind::UnknownCase): {
          SmallVector<Space, 4> newSubSpaces;
          for (auto subSpace : this->getSpaces()) {
            auto nextSpace = subSpace.minus(other, DC, minusCount);
            if (!nextSpace)
              return llvm::None;
            if (nextSpace.value().isEmpty())
              return Space();
            newSubSpaces.push_back(nextSpace.value());
          }
          return Space::forConstructor(this->getType(), this->getHead(),
                                       newSubSpaces);
        }

        PAIRCASE (SpaceKind::Constructor, SpaceKind::Constructor): {
          // Optimization: If the heads of the constructors don't match then
          // the two are disjoint and their difference is the first space.
          if (this->Head.getBaseIdentifier() !=
              other.Head.getBaseIdentifier()) {
            return *this;
          }

          // Special Case: Short circuit patterns without payloads.  Their
          // difference is empty.
          if (other.getSpaces().empty()) {
            return Space();
          }
          
          SmallVector<Space, 4> constrSpaces;
          bool foundBad = false;
          auto i = this->getSpaces().begin();
          auto j = other.getSpaces().begin();
          for (auto idx = 0;
               i != this->getSpaces().end() && j != other.getSpaces().end();
               ++i, ++j, ++idx) {
            auto &s1 = *i;
            auto &s2 = *j;

            // If one constructor parameter doesn't cover the other then we've
            // got to report the uncovered cases in a user-friendly way.
            if (!s1.isSubspace(s2, DC)) {
              foundBad = true;
            }
            // Copy the params and replace the parameter at each index with the
            // difference of the two spaces.  This unpacks one constructor head
            // into each parameter.
            SmallVector<Space, 4> copyParams(this->getSpaces().begin(),
                                             this->getSpaces().end());

            auto reducedSpaceOrNone = s1.minus(s2, DC, minusCount);
            if (!reducedSpaceOrNone)
              return llvm::None;
            auto reducedSpace = *reducedSpaceOrNone;
            
            // If one of the constructor parameters is empty it means
            // the whole constructor space is empty as well, so we can
            // safely skip it.
            if (reducedSpace.isEmpty())
              continue;

            // If reduced produced the same space as original one, we
            // should return it directly instead of trying to create
            // a disjunction of its sub-spaces because nothing got reduced.
            // This is especially helpful when dealing with `unknown` case
            // in parameter positions.
            if (s1 == reducedSpace)
              return *this;

            copyParams[idx] = reducedSpace;
            Space CS = Space::forConstructor(this->getType(), this->getHead(),
                                             copyParams);
            constrSpaces.push_back(CS);
          }

          if (foundBad) {
            return Space::forDisjunct(constrSpaces);
          }
          return Space();
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::BooleanConstant): {
          // The difference of boolean constants depends on their values.
          if (this->getBoolValue() == other.getBoolValue()) {
            return Space();
          } else {
            return *this;
          }
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Type): {
          if (other.getType()->isBool()) {
            return (getKind() == SpaceKind::BooleanConstant)  ? Space() : *this;
          }

          if (canDecompose(other.getType())) {
            auto decomposition = decompose(DC, other.getType(), {});
            return this->minus(decomposition, DC, minusCount);
          }
          return *this;
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Empty):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::UnknownCase):
          return *this;

        PAIRCASE (SpaceKind::Type, SpaceKind::BooleanConstant): {
          if (canDecompose(this->getType())) {
            auto orSpace = decompose(DC, this->getType(), {});
            return orSpace.minus(other, DC, minusCount);
          } else {
            return *this;
          }
        }

        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Type):
          return Space();
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::Constructor):
          return *this;
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::UnknownCase):
          if (other.isAllowedButNotRequired() &&
              !this->isAllowedButNotRequired()) {
            return *this;
          }
          return Space();

        PAIRCASE (SpaceKind::Empty, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::UnknownCase, SpaceKind::BooleanConstant):
          return *this;
        default:
          llvm_unreachable("Uncovered pair found while computing difference?");
        }
      }

      void show(llvm::raw_ostream &buffer, bool forDisplay = true) const {
        switch (getKind()) {
        case SpaceKind::Empty:
          if (forDisplay) {
            buffer << "_";
          } else {
            buffer << "[EMPTY]";
          }
          break;
        case SpaceKind::Disjunct: {
          if (forDisplay) {
            llvm_unreachable("Attempted to display disjunct to user!");
          } else {
            buffer << "DISJOIN(";
            llvm::interleave(Spaces, [&](const Space &sp) {
              sp.show(buffer, forDisplay);
            }, [&buffer]() { buffer << " |\n"; });
            buffer << ")";
          }
        }
          break;
        case SpaceKind::BooleanConstant:
          buffer << (getBoolValue() ? "true" : "false");
          break;
        case SpaceKind::Constructor: {
          if (!Head.getBaseIdentifier().empty()) {
            buffer << ".";
            buffer << Head.getBaseIdentifier().str();
          }

          if (Spaces.empty()) {
            return;
          }

          auto args = Head.getArgumentNames().begin();
          auto argEnd = Head.getArgumentNames().end();

          // FIXME: Clean up code for performance
          buffer << "(";
          llvm::SmallVector<std::pair<Identifier, Space>, 4> labelSpaces;
          for (auto param : Spaces) {
            if (args != argEnd) {
              labelSpaces.push_back(
                  std::pair<Identifier, Space>(*args, param));
              ++args;
            } else
              labelSpaces.push_back(
                  std::pair<Identifier, Space>(Identifier(), param));
          }
          interleave(
              labelSpaces,
              [&](const std::pair<Identifier, Space> &param) {
                if (!param.first.empty()) {
                  buffer << param.first;
                  buffer << ": ";
                }
                param.second.show(buffer, forDisplay);
              },
              [&buffer]() { buffer << ", "; });
          buffer << ")";
        }
          break;
        case SpaceKind::Type: {
          Identifier Name = getPrintingName();
          if (Name.empty())
            buffer << "_";
          else
            buffer << tok::kw_let << " " << Name.str();
          if (!forDisplay) {
            buffer << ": ";
            getType()->print(buffer);
          }
        }
          break;
        case SpaceKind::UnknownCase:
          if (forDisplay) {
            // We special-case this to use "@unknown default" at the top level.
            buffer << "_";
          } else {
            buffer << "UNKNOWN";
            if (isAllowedButNotRequired())
              buffer << "(not_required)";
          }
          break;
        }
      }

      /// Use this if you're doing getAs<TupleType> on a Type (and it succeeds)
      /// to compute the spaces for it. Handy for disambiguating fields
      /// that are tuples from associated values.
      ///
      ///    .e((a: X, b: X)) -> ((a: X, b: X))
      /// vs .f(a: X, b: X)   -> (a: X, b: X)
      static void getTupleTypeSpaces(Type &outerType,
                                     TupleType *tty,
                                     SmallVectorImpl<Space> &spaces) {
        ArrayRef<TupleTypeElt> ttyElts = tty->getElements();
        if (isa<ParenType>(outerType.getPointer())) {
          // We had an actual tuple!
          SmallVector<Space, 4> innerSpaces;
          for (auto &elt: ttyElts)
            innerSpaces.push_back(Space::forType(elt.getType(), elt.getName()));
          spaces.push_back(
            Space::forConstructor(tty, Identifier(), innerSpaces));
        } else {
          // We're looking at the fields of a constructor here.
          for (auto &elt: ttyElts)
            spaces.push_back(Space::forType(elt.getType(), elt.getName()));
        }
      };

      // Decompose a type into its component spaces, ignoring any enum
      // cases that have no payloads and are also in the `voidList`. Membership
      // there means the space is guaranteed by the subtraction procedure to be
      // covered, so there's no reason to include it. Note that this *only*
      // works for constructor spaces with no payloads as these cannot be
      // overloaded and there is no further recursive structure to subtract
      // into.
      static void decomposeDisjuncts(const DeclContext *DC, Type tp,
                                     const llvm::StringSet<> &voidList,
                                     SmallVectorImpl<Space> &arr) {
        assert(canDecompose(tp) && "Non-decomposable type?");

        if (tp->isBool()) {
          arr.push_back(Space::forBool(true));
          arr.push_back(Space::forBool(false));
        } else if (auto *E = tp->getEnumOrBoundGenericEnum()) {
          // Look into each case of the enum and decompose it in turn.
          auto children = E->getAllElements();
          llvm::transform(
              children, std::back_inserter(arr), [&](EnumElementDecl *eed) {
                // Don't force people to match unavailable cases since they
                // should not be instantiated at run time.
                if (AvailableAttr::isUnavailable(eed)) {
                  return Space();
                }

                // If we're guaranteed a match from a subtraction, don't include
                // the space at all. See the `Type - Disjunct` case of
                // subtraction for when this optimization applies.
                if (!eed->hasAssociatedValues() &&
                    voidList.contains(eed->getBaseIdentifier().str())) {
                  return Space();
                }

                // .e(a: X, b: X)   -> (a: X, b: X)
                // .f((a: X, b: X)) -> ((a: X, b: X)
                SmallVector<Space, 4> constElemSpaces;
                if (auto payloadTy = eed->getArgumentInterfaceType()) {
                  auto eedTy = tp->getCanonicalType()->getTypeOfMember(
                      E->getModuleContext(), eed, payloadTy);
                  if (auto *TTy = eedTy->getAs<TupleType>()) {
                    Space::getTupleTypeSpaces(eedTy, TTy, constElemSpaces);
                  } else if (auto *TTy =
                                 dyn_cast<ParenType>(eedTy.getPointer())) {
                    constElemSpaces.push_back(
                        Space::forType(TTy->getUnderlyingType(), Identifier()));
                  }
                }
                return Space::forConstructor(tp, eed->getName(),
                                             constElemSpaces);
              });

          if (!E->isFormallyExhaustive(DC)) {
            arr.push_back(Space::forUnknown(/*allowedButNotRequired*/false));
          } else if (!E->getAttrs().hasAttribute<FrozenAttr>()) {
            arr.push_back(Space::forUnknown(/*allowedButNotRequired*/true));
          }

        } else if (auto *TTy = tp->castTo<TupleType>()) {
          // Decompose each of the elements into its component type space.
          SmallVector<Space, 4> constElemSpaces;
          llvm::transform(TTy->getElements(),
                          std::back_inserter(constElemSpaces),
                          [&](TupleTypeElt elt) {
            return Space::forType(elt.getType(), elt.getName());
          });
          // Create an empty constructor head for the tuple space.
          arr.push_back(Space::forConstructor(tp, Identifier(),
                                              constElemSpaces));
        } else {
          llvm_unreachable("Can't decompose type?");
        }
      }

      static Space decompose(const DeclContext *DC,
                             Type type,
                             const llvm::StringSet<> &voidList) {
        SmallVector<Space, 4> spaces;
        decomposeDisjuncts(DC, type, voidList, spaces);
        return Space::forDisjunct(spaces);
      }

      static bool canDecompose(Type tp) {
        return tp->is<TupleType>() || tp->isBool() ||
               tp->getEnumOrBoundGenericEnum();
      }

      // Search the space for a reason to downgrade exhaustiveness errors to
      // a warning e.g. 'unknown case' statements.
      DowngradeToWarning checkDowngradeToWarning() const {
        switch (getKind()) {
        case SpaceKind::Type:
        case SpaceKind::BooleanConstant:
        case SpaceKind::Empty:
          return DowngradeToWarning::No;
        case SpaceKind::UnknownCase:
          return DowngradeToWarning::ForUnknownCase;
        case SpaceKind::Constructor: {
          auto result = DowngradeToWarning::No;
          // Traverse the constructor and its subspaces.
          for (const Space &space : getSpaces())
            result = std::max(result, space.checkDowngradeToWarning());
          return result;
        }
        case SpaceKind::Disjunct: {
          if (getSpaces().empty())
            return DowngradeToWarning::No;
          // Traverse the disjunct's subspaces.
          auto result = DowngradeToWarning::LAST;
          for (const Space &space : getSpaces())
            result = std::min(result, space.checkDowngradeToWarning());
          return result;
        }
        }
        llvm_unreachable("unhandled kind");
      }
    };

    ASTContext &Context;
    const SwitchStmt *Switch;
    const DeclContext *DC;
    APIntMap<Expr *> IntLiteralCache;
    llvm::DenseMap<APFloat, Expr *, ::DenseMapAPFloatKeyInfo> FloatLiteralCache;
    llvm::DenseMap<StringRef, Expr *> StringLiteralCache;
    
    SpaceEngine(ASTContext &C, const SwitchStmt *SS, const DeclContext *DC)
        : Context(C), Switch(SS), DC(DC) {}
    
    bool checkRedundantLiteral(const Pattern *Pat, Expr *&PrevPattern) {
      if (Pat->getKind() != PatternKind::Expr) {
          return false;
      }
      auto *ExprPat = cast<ExprPattern>(Pat);
      auto *MatchExpr = ExprPat->getSubExpr();
      if (!MatchExpr || !isa<LiteralExpr>(MatchExpr)) {
          return false;
      }
      auto *EL = cast<LiteralExpr>(MatchExpr);
      switch (EL->getKind()) {
      case ExprKind::StringLiteral: {
        auto *SLE = cast<StringLiteralExpr>(EL);
        auto cacheVal =
            StringLiteralCache.insert({SLE->getValue(), SLE});
        PrevPattern = (cacheVal.first != StringLiteralCache.end())
                    ? cacheVal.first->getSecond()
                    : nullptr;
        return !cacheVal.second;
      }
      case ExprKind::IntegerLiteral: {
        auto *ILE = cast<IntegerLiteralExpr>(EL);
        auto cacheVal = IntLiteralCache.insert({ILE->getRawValue(), ILE});
        PrevPattern = (cacheVal.first != IntLiteralCache.end())
                    ? cacheVal.first->getSecond()
                    : nullptr;
        return !cacheVal.second;
      }
      case ExprKind::FloatLiteral: {
        // FIXME: Pessimistically using IEEEquad here is bad and we should
        // actually figure out the bitwidth.  But it's too early in Sema.
        auto *FLE = cast<FloatLiteralExpr>(EL);
        auto cacheVal =
            FloatLiteralCache.insert(
                   {FLE->getValue(FLE->getDigitsText(),
                                  APFloat::IEEEquad(), FLE->isNegative()), FLE});
        PrevPattern = (cacheVal.first != FloatLiteralCache.end())
                    ? cacheVal.first->getSecond()
                    : nullptr;
        return !cacheVal.second;
      }
      default:
        return false;
      }
    }

    void checkExhaustiveness(bool limitedChecking) {
      // If the type of the scrutinee is uninhabited, we're already dead.
      // Allow any well-typed patterns through.
      auto subjectType = Switch->getSubjectExpr()->getType();
      if (subjectType && subjectType->isStructurallyUninhabited()) {
        return;
      }

      // If the switch body fails to typecheck, end analysis here.
      if (limitedChecking) {
        // Reject switch statements with empty blocks.
        if (Switch->getCases().empty())
          diagnoseMissingCases(RequiresDefault::EmptySwitchBody, Space());
        return;
      }

      const CaseStmt *unknownCase = nullptr;
      SmallVector<Space, 4> spaces;
      auto &DE = Context.Diags;
      for (auto *caseBlock : Switch->getCases()) {
        if (caseBlock->hasUnknownAttr()) {
          assert(unknownCase == nullptr && "multiple unknown cases");
          unknownCase = caseBlock;
          continue;
        }

        for (auto &caseItem : caseBlock->getCaseLabelItems()) {
          // 'where'-clauses on cases mean the case does not contribute to
          // the exhaustiveness of the pattern.
          if (caseItem.getGuardExpr())
            continue;

          // Space is trivially covered with a default clause.
          if (caseItem.isDefault())
            return;

          Space projection = projectPattern(caseItem.getPattern());
          bool isRedundant = !projection.isEmpty() &&
                             llvm::any_of(spaces, [&](const Space &handled) {
            return projection.isSubspace(handled, DC);
          });
          if (isRedundant) {
            DE.diagnose(caseItem.getStartLoc(),
                          diag::redundant_particular_case)
              .highlight(caseItem.getSourceRange());
            continue;
          }

          Expr *cachedExpr = nullptr;
          if (checkRedundantLiteral(caseItem.getPattern(), cachedExpr)) {
            assert(cachedExpr && "Cache found hit but no expr?");
            DE.diagnose(caseItem.getStartLoc(),
                        diag::redundant_particular_literal_case)
              .highlight(caseItem.getSourceRange());
            DE.diagnose(cachedExpr->getLoc(),
                        diag::redundant_particular_literal_case_here)
              .highlight(cachedExpr->getSourceRange());
            continue;
          }

          if (!projection.isEmpty())
            spaces.push_back(projection);
        }
      }

      Space totalSpace = Space::forType(subjectType, Identifier());
      Space coveredSpace = Space::forDisjunct(spaces);

      unsigned minusCount
        = Context.TypeCheckerOpts.SwitchCheckingInvocationThreshold;
      auto diff = totalSpace.minus(coveredSpace, DC, &minusCount);
      if (!diff) {
        diagnoseMissingCases(RequiresDefault::SpaceTooLarge, Space(),
                             unknownCase);
        return;
      }
      
      auto uncovered = diff.value();
      if (unknownCase && uncovered.isEmpty()) {
        DE.diagnose(unknownCase->getLoc(), diag::redundant_particular_case)
          .highlight(unknownCase->getSourceRange());
      }

      // Account for unknown cases. If the developer wrote `unknown`, they're
      // all handled; otherwise, we ignore the ones that were added for enums
      // that are implicitly frozen.
      uncovered = *uncovered.minus(Space::forUnknown(unknownCase == nullptr),
                                   DC, /*&minusCount*/ nullptr);

      if (uncovered.isEmpty())
        return;

      // If the entire space is left uncovered we have two choices: We can
      // decompose the type space and offer them as fixits, or simply offer
      // to insert a `default` clause.
      if (uncovered.getKind() == SpaceKind::Type) {
        if (Space::canDecompose(uncovered.getType())) {
          SmallVector<Space, 4> spaces;
          Space::decomposeDisjuncts(DC, uncovered.getType(), {}, spaces);
          diagnoseMissingCases(RequiresDefault::No, Space::forDisjunct(spaces),
                               unknownCase);
        } else {
          diagnoseMissingCases(Switch->getCases().empty()
                                 ? RequiresDefault::EmptySwitchBody
                                 : RequiresDefault::UncoveredSwitch,
                               uncovered, unknownCase);
        }
        return;
      }

      diagnoseMissingCases(RequiresDefault::No, uncovered, unknownCase);
    }
    
    enum class RequiresDefault {
      No,
      EmptySwitchBody,
      UncoveredSwitch,
      SpaceTooLarge,
    };

    void diagnoseMissingCases(RequiresDefault defaultReason, Space uncovered,
                              const CaseStmt *unknownCase = nullptr) {
      if (!Switch->getLBraceLoc().isValid()) {
        // There is no '{' in the switch statement, which we already diagnosed
        // in the parser. So there's no real body to speak of and it doesn't
        // make sense to emit diagnostics about missing cases.
        return;
      }
      auto &DE = Context.Diags;
      SourceLoc startLoc = Switch->getStartLoc();
      SourceLoc insertLoc;
      if (unknownCase)
        insertLoc = unknownCase->getStartLoc();
      else
        insertLoc = Switch->getEndLoc();
      StringRef placeholder = getCodePlaceholder();
      llvm::SmallString<128> buffer;
      llvm::raw_svector_ostream OS(buffer);

      bool InEditor = Context.LangOpts.DiagnosticsEditorMode;

      // Decide whether we want an error or a warning.
      llvm::Optional<decltype(diag::non_exhaustive_switch)> mainDiagType =
          diag::non_exhaustive_switch;
      if (unknownCase) {
        switch (defaultReason) {
        case RequiresDefault::EmptySwitchBody:
          llvm_unreachable("there's an @unknown case; the body can't be empty");
        case RequiresDefault::No:
          if (!uncovered.isEmpty())
            mainDiagType = diag::non_exhaustive_switch_warn;
          break;
        case RequiresDefault::UncoveredSwitch:
        case RequiresDefault::SpaceTooLarge: {
          auto diagnostic = defaultReason == RequiresDefault::UncoveredSwitch
                                ? diag::non_exhaustive_switch
                                : diag::possibly_non_exhaustive_switch;
          DE.diagnose(startLoc, diagnostic);
          DE.diagnose(unknownCase->getLoc(),
                      diag::non_exhaustive_switch_drop_unknown)
            .fixItRemoveChars(unknownCase->getStartLoc(),
                              unknownCase->getLoc());
          return;
        }
        }
      }

      switch (uncovered.checkDowngradeToWarning()) {
      case DowngradeToWarning::No:
        break;
      case DowngradeToWarning::ForUnknownCase: {
        if (Context.LangOpts.DebuggerSupport ||
            Context.LangOpts.Playground ||
            !Context.LangOpts.EnableNonFrozenEnumExhaustivityDiagnostics) {
          // Don't require covering unknown cases in the debugger or in
          // playgrounds.
          return;
        }
        assert(defaultReason == RequiresDefault::No);
        Type subjectType = Switch->getSubjectExpr()->getType();
        bool shouldIncludeFutureVersionComment = false;
        if (auto *theEnum = subjectType->getEnumOrBoundGenericEnum()) {
          shouldIncludeFutureVersionComment =
              theEnum->getParentModule()->isSystemModule();
        }
        DE.diagnose(startLoc, diag::non_exhaustive_switch_unknown_only,
                    subjectType, shouldIncludeFutureVersionComment);
        mainDiagType = llvm::None;
      }
        break;
      }

      switch (defaultReason) {
      case RequiresDefault::No:
        break;
      case RequiresDefault::EmptySwitchBody: {
        OS << tok::kw_default << ":\n" << placeholder << "\n";
        DE.diagnose(startLoc, diag::empty_switch_stmt)
          .fixItInsert(insertLoc, buffer.str());
      }
        return;
      case RequiresDefault::UncoveredSwitch: {
        OS << tok::kw_default << ":\n" << placeholder << "\n";
        DE.diagnose(startLoc, mainDiagType.value());
        DE.diagnose(startLoc, diag::missing_several_cases, /*default*/true)
          .fixItInsert(insertLoc, buffer.str());
      }
        return;
      case RequiresDefault::SpaceTooLarge: {
        OS << tok::kw_default << ":\n" << "<#fatalError()#>" << "\n";
        DE.diagnose(startLoc, diag::possibly_non_exhaustive_switch);
        DE.diagnose(startLoc, diag::missing_several_cases, /*default*/true)
          .fixItInsert(insertLoc, buffer.str());
      }
        return;
      }

      // If there's nothing else to diagnose, bail.
      if (uncovered.isEmpty()) return;

      // Check if we still have to emit the main diagnostic.
      if (mainDiagType.has_value())
        DE.diagnose(startLoc, mainDiagType.value());

      // Add notes to explain what's missing.
      auto processUncoveredSpaces =
          [&](llvm::function_ref<void(const Space &space,
                                      bool onlyOneUncoveredSpace)> process) {

        // Flatten away all disjunctions.
        SmallVector<Space, 4> flats;
        flatten(uncovered, flats);

        // Then figure out which of the remaining spaces are interesting.
        // To do this, we sort by size, largest spaces first...
        SmallVector<const Space *, 4> flatsSortedBySize;
        flatsSortedBySize.reserve(flats.size());
        for (const Space &space : flats)
          flatsSortedBySize.push_back(&space);
        std::stable_sort(flatsSortedBySize.begin(), flatsSortedBySize.end(),
                         [&](const Space *left, const Space *right) {
          return left->getSize(DC) > right->getSize(DC);
        });

        // ...and then remove any of the later spaces that are contained
        // entirely in an earlier one.
        SmallPtrSet<const Space *, 4> flatsToEmit;
        for (const Space *space : flatsSortedBySize) {
          bool alreadyHandled =
              llvm::any_of(flatsToEmit, [&](const Space *previousSpace) {
            return space->isSubspace(*previousSpace, DC);
          });
          if (alreadyHandled)
            continue;
          flatsToEmit.insert(space);
        }

        // Finally we can iterate over the flat spaces in their original order,
        // but only emit the interesting ones.
        for (const Space &flat : flats) {
          if (!flatsToEmit.count(&flat))
            continue;

          if (flat.getKind() == SpaceKind::UnknownCase) {
            assert(&flat == &flats.back() && "unknown case must be last");
            if (unknownCase) {
              // This can occur if the /only/ case in the switch is 'unknown'.
              // In that case we won't do any analysis on the input space, but
              // will later decompose the space into cases.
              continue;
            }
            if (!Context.LangOpts.EnableNonFrozenEnumExhaustivityDiagnostics)
              continue;

            // This can occur if the switch is empty and the subject type is an
            // enum. If decomposing the enum type yields an unknown space that
            // is not required, don't suggest adding it in the fix-it.
            if (flat.isAllowedButNotRequired())
              continue;
          }

          process(flat, flats.size() == 1);
        }
      };

      // If editing is enabled, emit a formatted error of the form:
      //
      // switch must be exhaustive, do you want to add missing cases?
      //     case (.none, .some(_)):
      //       <#code#>
      //     case (.some(_), .none):
      //       <#code#>
      //
      // else:
      //
      // switch must be exhaustive, consider adding missing cases:
      //
      // missing case '(.none, .some(_))'
      // missing case '(.some(_), .none)'
      if (InEditor) {
        buffer.clear();

        bool alreadyEmittedSomething = false;
        processUncoveredSpaces([&](const Space &space,
                                   bool onlyOneUncoveredSpace) {
          if (space.getKind() == SpaceKind::UnknownCase) {
            OS << "@unknown " << tok::kw_default;
            if (onlyOneUncoveredSpace) {
              OS << ":\n<#fatalError()#>\n";
              DE.diagnose(startLoc, diag::missing_unknown_case)
                .fixItInsert(insertLoc, buffer.str());
              alreadyEmittedSomething = true;
              return;
            }
          } else {
            OS << tok::kw_case << " ";
            space.show(OS);
          }
          OS << ":\n" << placeholder << "\n";
        });

        if (!alreadyEmittedSomething) {
          DE.diagnose(startLoc, diag::missing_several_cases, false)
            .fixItInsert(insertLoc, buffer.str());
        }

      } else {
        processUncoveredSpaces([&](const Space &space,
                                   bool onlyOneUncoveredSpace) {
          if (space.getKind() == SpaceKind::UnknownCase) {
            auto note = DE.diagnose(startLoc, diag::missing_unknown_case);
            if (onlyOneUncoveredSpace)
              note.fixItInsert(insertLoc, "@unknown default:\n<#fatalError#>()\n");
            return;
          }

          buffer.clear();
          space.show(OS);
          DE.diagnose(startLoc, diag::missing_particular_case, buffer.str());
        });
      }
    }

  private:
    // Recursively unpacks a space of disjunctions or constructor parameters
    // into its component parts such that the resulting array of flattened
    // spaces contains no further disjunctions.  The resulting flattened array
    // will never be empty.
    static void flatten(const Space space, SmallVectorImpl<Space> &flats) {
      switch (space.getKind()) {
      case SpaceKind::Constructor: {
        // Optimization: If this space is just a constructor head, it is already
        // flat.
        if (space.getSpaces().empty()) {
          flats.push_back(space);
          return;
        }
        
        // To recursively recover a pattern matrix from a bunch of disjuncts:
        // 1) Unpack the arguments to the constructor under scrutiny.
        // 2) Traverse each argument in turn.
        // 3) Flatten the argument space into a column vector.
        // 4) Extend the existing pattern matrix by a factor of the size of
        //    the column vector and copy each previous component.
        // 5) Extend the expanded matrix with multiples of the column vector's
        //    components until filled.
        // 6) Wrap each matrix row in the constructor under scrutiny.
        size_t multiplier = 1;
        SmallVector<SmallVector<Space, 4>, 2> matrix;
        for (auto &subspace : space.getSpaces()) {
          SmallVector<Space, 4> columnVect;
          flatten(subspace, columnVect);

          size_t startSize = matrix.size();
          if (!matrix.empty() && columnVect.size() > 1) {
            size_t oldCount = matrix.size();
            matrix.reserve(oldCount * columnVect.size());
            // Indexing starts at 1, we already have 'startSize'-many elements
            // in the matrix; multiplies by 1 are no-ops.
            for (size_t i = 1; i < columnVect.size(); ++i) {
              std::copy_n(matrix.begin(), oldCount, std::back_inserter(matrix));
            }
          }

          if (matrix.empty()) {
            // Get the empty matrix setup with its starting row vectors.
            for (auto &vspace : columnVect) {
              matrix.push_back({});
              matrix.back().push_back(vspace);
            }
          } else {
            // Given a matrix of 'n' rows and '(m-1)*k' columns, to make a
            // matrix of size 'n' by 'm*k' we need to copy each element of the
            // column vector into a row 'm' times - as many times as there were
            // elements of the original matrix before multiplication.
            size_t stride = multiplier;
            if (startSize == 1) {
              // Special case: If the column vector is bigger than the matrix
              // before multiplication, we need to index it linearly
              stride = 1;
            } else if (columnVect.size() == 1) {
              // Special case: If the column vector has size 1 then we needn't
              // stride at all.
              stride = matrix.size();
            }

            for (size_t rowIdx = 0, colIdx = 0; rowIdx < matrix.size(); ++rowIdx) {
              if (rowIdx != 0 && (rowIdx % stride) == 0) {
                ++colIdx;
              }

              matrix[rowIdx].push_back(columnVect[colIdx]);
            }
          }
          
          // Pattern matrices grow quasi-factorially in the size of the
          // input space.
          multiplier *= columnVect.size();
        }

        // Wrap the matrix rows into this constructor.
        for (auto &row : matrix) {
          flats.push_back(Space::forConstructor(space.getType(),
                                                space.getHead(),
                                                row));
        }
      }
        break;
      case SpaceKind::Disjunct: {
        for (auto &subspace : space.getSpaces()) {
          SmallVector<Space, 4> buf;
          flatten(subspace, buf);
          flats.append(buf.begin(), buf.end());
        }
      }
        break;
      default:
        flats.push_back(space);
        break;
      }
    }

    /// Recursively project a pattern into a Space.
    static Space projectPattern(const Pattern *item) {
      switch (item->getKind()) {
      case PatternKind::Any:
        return Space::forType(item->getType(), Identifier());
      case PatternKind::Named:
        return Space::forType(item->getType(),
                              cast<NamedPattern>(item)->getBoundName());
      case PatternKind::Bool:
        return Space::forBool(cast<BoolPattern>(item)->getValue());
      case PatternKind::Is: {
        auto *IP = cast<IsPattern>(item);
        switch (IP->getCastKind()) {
        case CheckedCastKind::Coercion:
        case CheckedCastKind::BridgingCoercion: {
          if (auto *subPattern = IP->getSubPattern()) {
            // Project the cast target's subpattern.
            Space castSubSpace = projectPattern(subPattern);
            // If we received a type space from a named pattern or a wildcard
            // we have to re-project with the cast's target type to maintain
            // consistency with the scrutinee's type.
            if (castSubSpace.getKind() == SpaceKind::Type) {

              return Space::forType(IP->getType(),
                                    castSubSpace.getPrintingName());
            }
            return castSubSpace;
          }

          // With no subpattern coercions are irrefutable.  Project with the
          // original type instead of the cast's target type to maintain
          // consistency with the scrutinee's type.
          return Space::forType(IP->getType(), Identifier());
        }
        case CheckedCastKind::Unresolved:
        case CheckedCastKind::ValueCast:
        case CheckedCastKind::ArrayDowncast:
        case CheckedCastKind::DictionaryDowncast:
        case CheckedCastKind::SetDowncast:
            return Space();
        }
      }
      case PatternKind::Typed:
        llvm_unreachable("cannot appear in case patterns");
      case PatternKind::Expr:
        return Space();
      case PatternKind::Binding: {
        auto *VP = cast<BindingPattern>(item);
        return projectPattern(VP->getSubPattern());
      }
      case PatternKind::Paren: {
        auto *PP = cast<ParenPattern>(item);
        return projectPattern(PP->getSubPattern());
      }
      case PatternKind::OptionalSome: {
        auto *OSP = cast<OptionalSomePattern>(item);
        const Identifier name = OSP->getElementDecl()->getBaseIdentifier();

        auto subSpace = projectPattern(OSP->getSubPattern());
        // To match patterns like (_, _, ...)?, we must rewrite the underlying
        // tuple pattern to .some(_, _, ...) first.
        if (subSpace.getKind() == SpaceKind::Constructor &&
            subSpace.getHead().getBaseIdentifier().empty()) {
          return Space::forConstructor(item->getType(), name,
                                       {subSpace});
        }
        return Space::forConstructor(item->getType(), name, subSpace);
      }
      case PatternKind::EnumElement: {
        auto *VP = cast<EnumElementPattern>(item);
        auto *SP = VP->getSubPattern();
        if (!SP) {
          // If there's no sub-pattern then there's no further recursive
          // structure here.  Yield the constructor space.
          // FIXME: Compound names.
          return Space::forConstructor(
              item->getType(), VP->getName().getBaseIdentifier(), llvm::None);
        }

        SmallVector<Space, 4> conArgSpace;
        switch (SP->getKind()) {
        case PatternKind::Tuple: {
          auto *TP = dyn_cast<TuplePattern>(SP);
          llvm::transform(TP->getElements(), std::back_inserter(conArgSpace),
                          [&](TuplePatternElt pate) {
                            return projectPattern(pate.getPattern());
                          });
          // FIXME: Compound names.
          return Space::forConstructor(item->getType(),
                                       VP->getName().getBaseIdentifier(),
                                       conArgSpace);
        }
        case PatternKind::Paren: {
          // If we've got an extra level of parens, we need to flatten that into
          // the enum payload.
          auto *PP = dyn_cast<ParenPattern>(SP);
          auto *SP = PP->getSemanticsProvidingPattern();

          // Special Case: A constructor pattern may have all of its payload
          // matched by a single var pattern.  Project it like the tuple it
          // really is.
          //
          // FIXME: SE-0155 makes this case unreachable.
          if (SP->getKind() == PatternKind::Named
              || SP->getKind() == PatternKind::Any) {
            Type outerType = SP->getType();
            if (auto *TTy = outerType->getAs<TupleType>())
              Space::getTupleTypeSpaces(outerType, TTy, conArgSpace);
            else
              conArgSpace.push_back(projectPattern(SP));
          } else if (SP->getKind() == PatternKind::Tuple) {
            Space argTupleSpace = projectPattern(SP);
            // Tuples are modeled as if they are enums with a single, nameless
            // case, which means argTupleSpace will either be a Constructor or
            // Empty space. If it's empty (i.e. it contributes nothing to the
            // overall exhaustiveness), the entire enum case space is empty.
            if (argTupleSpace.isEmpty())
              return Space();
            assert(argTupleSpace.getKind() == SpaceKind::Constructor);
            conArgSpace.push_back(argTupleSpace);
          } else {
            conArgSpace.push_back(projectPattern(SP));
          }
          // FIXME: Compound names.
          return Space::forConstructor(item->getType(),
                                       VP->getName().getBaseIdentifier(),
                                       conArgSpace);
        }
        default:
          return projectPattern(SP);
        }
      }
      case PatternKind::Tuple: {
        auto *TP = cast<TuplePattern>(item);
        SmallVector<Space, 4> conArgSpace;
        llvm::transform(TP->getElements(), std::back_inserter(conArgSpace),
                        [&](TuplePatternElt pate) {
                          return projectPattern(pate.getPattern());
                        });
        return Space::forConstructor(item->getType(), Identifier(),
                                     conArgSpace);
      }
      }
      llvm_unreachable("unhandled kind");
    }
  };
} // end anonymous namespace

void TypeChecker::checkSwitchExhaustiveness(const SwitchStmt *stmt,
                                            const DeclContext *DC,
                                            bool limited) {
  SpaceEngine(DC->getASTContext(), stmt, DC).checkExhaustiveness(limited);
}

void SpaceEngine::Space::dump() const {
  this->show(llvm::errs(), /*normalize*/ false);
  llvm::errs() << '\n';
}
