//===------------------------ UnitTestRunner.cpp --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// TO ADD A NEW TEST, search in this file for [new_tests]
//
// Provides a mechanism for doing faux unit tests.  The idea is to emulate the
// basic functionality of calling a function and validating its results/effects.
//
// This is done via the test_specification instruction.  Using one or more
// instances of it in your function, you can specify which test (i.e. UnitTest
// subclass) should be run and what arguments should be provided to it.  The
// test grabs the arguments it expects out of the test::Arguments instance it is
// provided.  It calls some function or functions.  It then prints out
// interesting results.  These results can then be FileCheck'd.
//
// CASE STUDY:
// Here's an example of how it works:
// 1) A test test/SILOptimizer/interesting_functionality.sil runs this pass:
//     // RUN: %target-sil-opt -unit-test-runner %s 2>&1 | %FileCheck %s
// 2) A function in interesting_functionality.sil contains the
//    test_specification instruction.
//      sil @f : $() -> () {
//      ...
//      test_specification "my-neato-utility 43 @trace[2] @function[other_fun]"
//      ...
//      }
// 3) UnitTestRunner sees the name "my-neato-utility", instantiates the
//    corresponding UnitTest subclass MyNeatoUtilityTest, and calls ::invoke()
//    on it, passing an test::Arguments that contains
//      (43 : unsigned long, someValue : SILValue, other_fun : SILFunction *)
//
// 4) MyNeatoUtilityTest calls takeUInt(), takeValue(), and takeFunction() on
//    the test::Arguments instance.
//      auto count = arguments.takeUInt();
//      auto target = arguments.takeValue();
//      auto callee = arguments.takeFunction();
// 5) MyNeatoUtilityTest calls myNeatoUtility, passing these values along.
//      myNeatoUtility(count, target, callee);
// 6) MyNeatoUtilityTest then dumps out the current function, which was modified
//    in the process.
//      getFunction()->dump();
// 7) The test file test/SILOptimizer/interesting_functionality.sil matches the
//    expected contents of the modified function:
//    // CHECK-LABEL: sil @f
//    // CHECK-NOT:     function_ref @other_fun
//
// [new_tests] TESTING MORE FUNCTIONALITY:
//
// 1) Add a new UnitTest subclass.
//    - In the section "Unit Tests Subclasses" section ordered alphabetically.
//    - Add a constructor: NewTest(UnitTestRunner *pass) : UnitTest(pass) {}
//    - Implement invoke: void invoke(test::Arguments &arguments) override {...}
//    - Call the take{TYPE} methods to get the arguments you need.
//    - Call your function with those arguments.
// 2) Add a new ADD_UNIT_TEST_SUBCLASS line.
//    - Ordered alphabetically.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Type.h"
#include "swift/Basic/TaggedUnion.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OSSALifetimeCompletion.h"
#include "swift/SIL/OwnershipLiveness.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/SILArgumentArrayRef.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Transforms/SimplifyCFG.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/ParseTestSpecification.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <memory>

using namespace swift;
using namespace swift::test;

namespace {

class UnitTestRunner;

class UnitTest {
  UnitTestRunner *pass;

protected:
  template <typename Analysis>
  Analysis *getAnalysis();
  UnitTestRunner *getPass() { return pass; }
  SILFunction *getFunction();

public:
  UnitTest(UnitTestRunner *pass) : pass(pass){};
  virtual ~UnitTest() {}
  virtual void invoke(Arguments &arguments) = 0;
};

class UnitTestRunner : public SILFunctionTransform {
  void printTestLifetime(bool begin, unsigned testIndex, unsigned testCount,
                         StringRef name, ArrayRef<StringRef> components) {
    StringRef word = begin ? "\nbegin" : "end";
    llvm::errs() << word << " running test " << testIndex + 1 << " of "
                 << testCount << " on " << getFunction()->getName() << ": "
                 << name << " with: ";
    for (unsigned long index = 0, size = components.size(); index < size;
         ++index) {
      auto componentString = components[index].trim();
      if (componentString.empty())
        continue;

      llvm::errs() << componentString;
      if (index != size - 1) {
        llvm::errs() << ", ";
      }
    }
    llvm::errs() << "\n";
  }

  template <typename Doit>
  void withTest(StringRef name, Doit doit);

  void runTest(StringRef name, Arguments &arguments) {
    withTest(name, [&](auto *test) { test->invoke(arguments); });
  }

  void run() override {
    llvm::SmallVector<UnparsedSpecification, 2> testSpecifications;
    getTestSpecifications(getFunction(), testSpecifications);
    Arguments arguments;
    SmallVector<StringRef, 4> components;
    for (unsigned long index = 0, size = testSpecifications.size();
         index < size; ++index) {
      components.clear();
      arguments.clear();
      auto testSpecification = testSpecifications[index];
      test::parseTestArgumentsFromSpecification(
          getFunction(), testSpecification, arguments, components);
      auto name = arguments.takeString();
      ArrayRef<StringRef> argumentStrings = components;
      argumentStrings = argumentStrings.drop_front();
      printTestLifetime(/*begin=*/true, /*index=*/index, /*size=*/size, name,
                        argumentStrings);
      runTest(name, arguments);
      printTestLifetime(/*begin=*/false, /*index=*/index, /*size=*/size, name,
                        argumentStrings);
    }
  }
  friend class UnitTest;
};

//===----------------------------------------------------------------------===//
// MARK: General Unit Tests
//===----------------------------------------------------------------------===//

// Arguments: NONE
// Dumps:
// - the function
struct DumpFunction : UnitTest {
  DumpFunction(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override { getFunction()->dump(); }
};

// Arguments: NONE
// Dumps: the index of the self argument of the current function
struct FunctionGetSelfArgumentIndex : UnitTest {
  FunctionGetSelfArgumentIndex(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto index = BridgedFunction{getFunction()}.getSelfArgumentIndex();
    llvm::errs() << "self argument index = " << index << "\n";
  }
};

// Arguments:
// - string: list of characters, each of which specifies subsequent arguments
//           - A: (block) argument
//           - F: function
//           - B: block
//           - I: instruction
//           - V: value
//           - O: operand
//           - b: boolean
//           - u: unsigned
//           - s: string
// - ...
// - an argument of the type specified in the initial string
// - ...
// Dumps:
// - for each argument (after the initial string)
//   - its type
//   - something to identify the instance (mostly this means calling dump)
struct TestSpecificationTest : UnitTest {
  TestSpecificationTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto expectedFields = arguments.takeString();
    for (auto expectedField : expectedFields) {
      switch (expectedField) {
      case 'A': {
        auto *argument = arguments.takeBlockArgument();
        llvm::errs() << "argument:\n";
        argument->dump();
        break;
      }
      case 'F': {
        auto *function = arguments.takeFunction();
        llvm::errs() << "function: " << function->getName() << "\n";
        break;
      }
      case 'B': {
        auto *block = arguments.takeBlock();
        llvm::errs() << "block:\n";
        block->dump();
        break;
      }
      case 'I': {
        auto *instruction = arguments.takeInstruction();
        llvm::errs() << "instruction: ";
        instruction->dump();
        break;
      }
      case 'V': {
        auto value = arguments.takeValue();
        llvm::errs() << "value: ";
        value->dump();
        break;
      }
      case 'O': {
        auto *operand = arguments.takeOperand();
        llvm::errs() << "operand: ";
        operand->print(llvm::errs());
        break;
      }
      case 'u': {
        auto u = arguments.takeUInt();
        llvm::errs() << "uint: " << u << "\n";
        break;
      }
      case 'b': {
        auto b = arguments.takeBool();
        llvm::errs() << "bool: " << b << "\n";
        break;
      }
      case 's': {
        auto s = arguments.takeString();
        llvm::errs() << "string: " << s << "\n";
        break;
      }
      default:
        llvm_unreachable("unknown field type was expected?!");
      }
    }
  }
};

// Arguments:
// - value: the value to check for escaping
// Dumps:
// - the value
// - whether it has a pointer escape
struct OwnershipUtilsHasPointerEscape : UnitTest {
  OwnershipUtilsHasPointerEscape(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    auto has = findPointerEscape(value);
    value->print(llvm::errs());
    auto *boolString = has ? "true" : "false";
    llvm::errs() << boolString << "\n";
  }
};

// Arguments:
// - value: whose type will be printed
// Dumps:
// - the type lowering of the type
struct PrintTypeLowering : UnitTest {
  PrintTypeLowering(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    auto ty = value->getType();
    getFunction()->getTypeLowering(ty).print(llvm::dbgs());
  }
};

//===----------------------------------------------------------------------===//
// MARK: OSSA Lifetime Unit Tests
//===----------------------------------------------------------------------===//

// Arguments:
// - the lexical borrow to fold
// Dumps:
// - the function
struct LexicalDestroyFoldingTest : UnitTest {
  LexicalDestroyFoldingTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
    DominanceInfo *domTree = dominanceAnalysis->get(getFunction());
    auto value = arguments.takeValue();
    auto *bbi = cast<BeginBorrowInst>(value);
    InstructionDeleter deleter;
    foldDestroysOfCopiedLexicalBorrow(bbi, *domTree, deleter);
    getFunction()->dump();
  }
};

// Arguments:
// - variadic list of - instruction: a last user
// Dumps:
// - the insertion points
struct PrunedLivenessBoundaryWithListOfLastUsersInsertionPointsTest : UnitTest {
  PrunedLivenessBoundaryWithListOfLastUsersInsertionPointsTest(
      UnitTestRunner *pass)
      : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    PrunedLivenessBoundary boundary;
    while (arguments.hasUntaken()) {
      boundary.lastUsers.push_back(arguments.takeInstruction());
    }
    boundary.visitInsertionPoints(
        [](SILBasicBlock::iterator point) { point->dump(); });
  }
};

// Arguments:
// - SILValue: value to a analyze
// Dumps:
// - the liveness result and boundary
struct SSALivenessTest : UnitTest {
  SSALivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}

  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    assert(!arguments.hasUntaken());
    llvm::outs() << "SSA lifetime analysis: " << value;

    SmallVector<SILBasicBlock *, 8> discoveredBlocks;
    SSAPrunedLiveness liveness(value->getFunction(), &discoveredBlocks);
    liveness.initializeDef(value);
    LiveRangeSummary summary = liveness.computeSimple();
    if (summary.innerBorrowKind == InnerBorrowKind::Reborrowed)
      llvm::outs() << "Incomplete liveness: Reborrowed inner scope\n";

    if (summary.addressUseKind == AddressUseKind::PointerEscape)
      llvm::outs() << "Incomplete liveness: Escaping address\n";
    else if (summary.addressUseKind == AddressUseKind::Unknown)
      llvm::outs() << "Incomplete liveness: Unknown address use\n";

    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - SILValue: value to a analyze
// Dumps:
// - the liveness result and boundary
struct ScopedAddressLivenessTest : UnitTest {
  ScopedAddressLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}

  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    assert(!arguments.hasUntaken());
    llvm::outs() << "Scoped address analysis: " << value;

    ScopedAddressValue scopedAddress(value);
    assert(scopedAddress);

    SmallVector<SILBasicBlock *, 8> discoveredBlocks;
    SSAPrunedLiveness liveness(value->getFunction(), &discoveredBlocks);
    scopedAddress.computeTransitiveLiveness(liveness);
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - variadic list of live-range defining values or instructions
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by finding all their direct SSA
// uses. If the def is an instruction, then all results are considered.
struct MultiDefLivenessTest : UnitTest {
  MultiDefLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}

  void invoke(Arguments &arguments) override {
    SmallVector<SILBasicBlock *, 8> discoveredBlocks;
    MultiDefPrunedLiveness liveness(getFunction(), &discoveredBlocks);

    llvm::outs() << "MultiDef lifetime analysis:\n";
    while (arguments.hasUntaken()) {
      auto argument = arguments.takeArgument();
      if (isa<InstructionArgument>(argument)) {
        auto *instruction = cast<InstructionArgument>(argument).getValue();
        llvm::outs() << "  def instruction: " << instruction;
        liveness.initializeDef(instruction);
      } else {
        SILValue value = cast<ValueArgument>(argument).getValue();
        llvm::outs() << "  def value: " << value;
        liveness.initializeDef(value);
      }
    }
    liveness.computeSimple();
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - the string "defs:"
// - list of live-range defining values or instructions
// - the string "uses:"
// - variadic list of live-range user instructions
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by considering only the
// specified uses. The actual uses of the def nodes are ignored.
//
// This is useful for testing non-ssa liveness, for example, of memory
// locations. In that case, the def nodes may be stores and the uses may be
// destroy_addrs.
struct MultiDefUseLivenessTest : UnitTest {
  MultiDefUseLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}

  void invoke(Arguments &arguments) override {
    SmallVector<SILBasicBlock *, 8> discoveredBlocks;
    MultiDefPrunedLiveness liveness(getFunction(), &discoveredBlocks);

    llvm::outs() << "MultiDef lifetime analysis:\n";
    if (arguments.takeString() != "defs:") {
      llvm::report_fatal_error(
        "test specification expects the 'defs:' label\n");
    }
    while (true) {
      auto argument = arguments.takeArgument();
      if (isa<InstructionArgument>(argument)) {
        auto *instruction = cast<InstructionArgument>(argument).getValue();
        llvm::outs() << "  def instruction: " << *instruction;
        liveness.initializeDef(instruction);
        continue;
      }
      if (isa<ValueArgument>(argument)) {
        SILValue value = cast<ValueArgument>(argument).getValue();
        llvm::outs() << "  def value: " << value;
        liveness.initializeDef(value);
        continue;
      }
      if (cast<StringArgument>(argument).getValue() != "uses:") {
        llvm::report_fatal_error(
          "test specification expects the 'uses:' label\n");
      }
      break;
    }
    while (arguments.hasUntaken()) {
      auto *inst = arguments.takeInstruction();
      // lifetimeEnding has no effects on liveness, it's only a cache for the
      // caller.
      liveness.updateForUse(inst, /*lifetimeEnding*/false);
    }
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - value: entity whose fields' livenesses are being computed
// - string: "defs:"
// - variadic list of triples consisting of 
//   - value: a live-range defining value
//   - int: the beginning of the range of fields defined by the value
//   - int: the end of the range of the fields defined by the value
// - the string "uses:"
// - variadic list of quadruples consisting of
//   - instruction: a live-range user
//   - bool: whether the user is lifetime-ending
//   - int: the beginning of the range of fields used by the instruction
//   - int: the end of the range of fields used by the instruction
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by considering the
// specified uses. The actual uses of the def nodes are ignored.
//
// This is useful for testing non-ssa liveness, for example, of memory
// locations. In that case, the def nodes may be stores and the uses may be
// destroy_addrs.
struct FieldSensitiveMultiDefUseLiveRangeTest : UnitTest {
  FieldSensitiveMultiDefUseLiveRangeTest(UnitTestRunner *pass) : UnitTest(pass) {}

  void invoke(Arguments &arguments) override {
    SmallVector<SILBasicBlock *, 8> discoveredBlocks;
    auto value = arguments.takeValue();
    FieldSensitiveMultiDefPrunedLiveRange liveness(getFunction(), value, &discoveredBlocks);

    llvm::outs() << "FieldSensitive MultiDef lifetime analysis:\n";
    if (arguments.takeString() != "defs:") {
      llvm::report_fatal_error(
        "test specification expects the 'defs:' label\n");
    }
    while (true) {
      auto argument = arguments.takeArgument();
      if (isa<StringArgument>(argument)) {
        if(cast<StringArgument>(argument).getValue() != "uses:") {
          llvm::report_fatal_error(
            "test specification expects the 'uses:' label\n");
        }
        break;
      }
      auto begin = arguments.takeUInt();
      auto end = arguments.takeUInt();
      TypeTreeLeafTypeRange range(begin, end);
      if (isa<InstructionArgument>(argument)) {
        auto *instruction = cast<InstructionArgument>(argument).getValue();
        llvm::outs() << "  def in range [" << begin << ", " << end << ") instruction: " << *instruction;
        liveness.initializeDef(instruction, range);
        continue;
      }
      if (isa<ValueArgument>(argument)) {
        SILValue value = cast<ValueArgument>(argument).getValue();
        llvm::outs() << "  def in range [" << begin << ", " << end << ") value: " << value;
        liveness.initializeDef(value, range);
        continue;
      }
      llvm::report_fatal_error(
        "test specification expects the 'uses:' label\n");
    }
    liveness.finishedInitializationOfDefs();
    while (arguments.hasUntaken()) {
      auto *inst = arguments.takeInstruction();
      auto lifetimeEnding = arguments.takeBool();
      auto begin = arguments.takeUInt();
      auto end = arguments.takeUInt();
      TypeTreeLeafTypeRange range(begin, end);
      liveness.updateForUse(inst, range, lifetimeEnding);
    }
    liveness.print(llvm::errs());

    FieldSensitivePrunedLivenessBoundary boundary(liveness.getNumSubElements());
    liveness.computeBoundary(boundary);
    boundary.print(llvm::errs());
  }
};

// Arguments:
// - bool: pruneDebug
// - bool: maximizeLifetimes
// - bool: "respectAccessScopes", whether to contract lifetimes to end within
//         access scopes which they previously enclosed but can't be hoisted
//         before
// - SILValue: value to canonicalize
// Dumps:
// - function after value canonicalization
struct CanonicalizeOSSALifetimeTest : UnitTest {
  CanonicalizeOSSALifetimeTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *accessBlockAnalysis = getAnalysis<NonLocalAccessBlockAnalysis>();
    auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
    DominanceInfo *domTree = dominanceAnalysis->get(getFunction());
    auto *calleeAnalysis = getAnalysis<BasicCalleeAnalysis>();
    auto pruneDebug = arguments.takeBool();
    auto maximizeLifetimes = arguments.takeBool();
    auto respectAccessScopes = arguments.takeBool();
    InstructionDeleter deleter;
    CanonicalizeOSSALifetime canonicalizer(
        pruneDebug, maximizeLifetimes, getFunction(),
        respectAccessScopes ? accessBlockAnalysis : nullptr, domTree,
        calleeAnalysis, deleter);
    auto value = arguments.takeValue();
    canonicalizer.canonicalizeValueLifetime(value);
    getFunction()->dump();
  }
};

// Arguments:
// - SILValue: value to canonicalize
// Dumps:
// - function after value canonicalization
struct CanonicalizeBorrowScopeTest : UnitTest {
  CanonicalizeBorrowScopeTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    auto borrowedValue = BorrowedValue(value);
    assert(borrowedValue && "specified value isn't a BorrowedValue!?");
    InstructionDeleter deleter;
    CanonicalizeBorrowScope canonicalizer(value->getFunction(), deleter);
    canonicalizer.canonicalizeBorrowScope(borrowedValue);
    getFunction()->dump();
  }
};

// Arguments:
// - instruction
// Dumps:
// - instruction
// - whether it's a deinit barrier
struct IsDeinitBarrierTest : UnitTest {
  IsDeinitBarrierTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *instruction = arguments.takeInstruction();
    auto *analysis = getAnalysis<BasicCalleeAnalysis>();
    auto isBarrier = isDeinitBarrier(instruction, analysis);
    instruction->dump();
    auto *boolString = isBarrier ? "true" : "false";
    llvm::errs() << boolString << "\n";
  }
};

// Arguments:
// - value
// Dumps:
// - value
// - whether it's lexical
struct IsLexicalTest : UnitTest {
  IsLexicalTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    auto isLexical = value->isLexical();
    value->dump();
    auto *boolString = isLexical ? "true" : "false";
    llvm::errs() << boolString << "\n";
  }
};

struct ShrinkBorrowScopeTest : UnitTest {
  ShrinkBorrowScopeTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto instruction = arguments.takeValue();
    auto expected = arguments.takeBool();
    auto *bbi = cast<BeginBorrowInst>(instruction);
    auto *analysis = getAnalysis<BasicCalleeAnalysis>();
    SmallVector<CopyValueInst *, 4> modifiedCopyValueInsts;
    InstructionDeleter deleter(
        InstModCallbacks().onDelete([&](auto *instruction) {
          llvm::errs() << "DELETED:\n";
          instruction->dump();
        }));
    auto shrunk =
        shrinkBorrowScope(*bbi, deleter, analysis, modifiedCopyValueInsts);
    unsigned index = 0;
    for (auto *cvi : modifiedCopyValueInsts) {
      auto expectedCopy = arguments.takeValue();
      llvm::errs() << "rewritten copy " << index << ":\n";
      llvm::errs() << "expected:\n";
      expectedCopy->print(llvm::errs());
      llvm::errs() << "got:\n";
      cvi->dump();
      assert(cvi == expectedCopy);
      ++index;
    }
    assert(expected == shrunk && "didn't shrink expectedly!?");
  }
};

// Arguments:
// - SILValue: phi
// Dumps:
// - function
// - the adjacent phis
struct VisitInnerAdjacentPhisTest : UnitTest {
  VisitInnerAdjacentPhisTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    getFunction()->dump();
    visitInnerAdjacentPhis(cast<SILPhiArgument>(arguments.takeValue()),
                           [](auto *argument) -> bool {
                             argument->dump();
                             return true;
                           });
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the enclosing defs
struct FindEnclosingDefsTest : UnitTest {
  FindEnclosingDefsTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    getFunction()->dump();
    llvm::dbgs() << "Enclosing Defs:\n";
    visitEnclosingDefs(arguments.takeValue(), [](SILValue def) {
      def->dump();
      return true;
    });
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the borrow introducers
struct FindBorrowIntroducers : UnitTest {
  FindBorrowIntroducers(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    getFunction()->dump();
    llvm::dbgs() << "Introducers:\n";
    visitBorrowIntroducers(arguments.takeValue(), [](SILValue def) {
      def->dump();
      return true;
    });
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
struct LinearLivenessTest : UnitTest {
  LinearLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    SILValue value = arguments.takeValue();
    getFunction()->dump();
    llvm::dbgs() << "Linear liveness: " << value;
    LinearLiveness liveness(value);
    liveness.compute();
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.getLiveness().computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
struct InteriorLivenessTest : UnitTest {
  InteriorLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    SILValue value = arguments.takeValue();
    getFunction()->dump();
    llvm::dbgs() << "Interior liveness: " << value;
    auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
    DominanceInfo *domTree = dominanceAnalysis->get(getFunction());
    InteriorLiveness liveness(value);
    auto handleInnerScope = [](SILValue innerBorrow) {
      llvm::outs() << "Inner scope: " << innerBorrow;
    };
    liveness.compute(domTree, handleInnerScope);
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.getLiveness().computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
// - the computed pruned liveness
// - the liveness boundary
struct ExtendedLinearLivenessTest : UnitTest {
  ExtendedLinearLivenessTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    SILValue value = arguments.takeValue();
    getFunction()->dump();
    llvm::dbgs() << "Extended liveness: " << value;
    ExtendedLinearLiveness liveness(value);
    liveness.compute();
    liveness.print(llvm::outs());

    PrunedLivenessBoundary boundary;
    liveness.getLiveness().computeBoundary(boundary);
    boundary.print(llvm::outs());
  }
};

// Arguments:
// - SILValue: value
// Dumps:
// - function
struct OSSALifetimeCompletionTest : UnitTest {
  OSSALifetimeCompletionTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    SILValue value = arguments.takeValue();
    llvm::dbgs() << "OSSA lifetime completion: " << value;
    OSSALifetimeCompletion completion(getFunction(), /*domInfo*/nullptr);
    completion.completeOSSALifetime(value);
    getFunction()->dump();    
  }
};

//===----------------------------------------------------------------------===//
// MARK: SimplifyCFG Unit Tests
//===----------------------------------------------------------------------===//

struct SimplifyCFGSimplifyArgument : UnitTest {
  SimplifyCFGSimplifyArgument(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    auto *block = arguments.takeBlock();
    auto index = arguments.takeUInt();
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifyArgument(block, index);
  }
};

struct SimplifyCFGSimplifyBlockArgs : UnitTest {
  SimplifyCFGSimplifyBlockArgs(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifyBlockArgs();
  }
};

struct SimplifyCFGCanonicalizeSwitchEnum : UnitTest {
  SimplifyCFGCanonicalizeSwitchEnum(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .canonicalizeSwitchEnums();
  }
};

struct SimplifyCFGSimplifySwitchEnumBlock : UnitTest {
  SimplifyCFGSimplifySwitchEnumBlock(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifySwitchEnumBlock(
            cast<SwitchEnumInst>(arguments.takeInstruction()));
  }
};

struct SimplifyCFGSwitchEnumOnObjcClassOptional : UnitTest {
  SimplifyCFGSwitchEnumOnObjcClassOptional(UnitTestRunner *pass)
      : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifySwitchEnumOnObjcClassOptional(
            cast<SwitchEnumInst>(arguments.takeInstruction()));
  }
};

struct SimplifyCFGSimplifySwitchEnumUnreachableBlocks : UnitTest {
  SimplifyCFGSimplifySwitchEnumUnreachableBlocks(UnitTestRunner *pass)
      : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifySwitchEnumUnreachableBlocks(
            cast<SwitchEnumInst>(arguments.takeInstruction()));
  }
};

struct SimplifyCFGSimplifyTermWithIdenticalDestBlocks : UnitTest {
  SimplifyCFGSimplifyTermWithIdenticalDestBlocks(UnitTestRunner *pass)
      : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .simplifyTermWithIdenticalDestBlocks(arguments.takeBlock());
  }
};

struct SimplifyCFGTryJumpThreading : UnitTest {
  SimplifyCFGTryJumpThreading(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
    passToRun->injectPassManager(getPass()->getPassManager());
    passToRun->injectFunction(getFunction());
    SimplifyCFG(*getFunction(), *passToRun, /*VerifyAll=*/false,
                /*EnableJumpThread=*/false)
        .tryJumpThreading(cast<BranchInst>(arguments.takeInstruction()));
  }
};

//===----------------------------------------------------------------------===//
// MARK: AccessPath Unit Tests
//===----------------------------------------------------------------------===//

struct AccessUseTestVisitor : public AccessUseVisitor {
  AccessUseTestVisitor()
    : AccessUseVisitor(AccessUseType::Overlapping,
                       NestedAccessType::IgnoreAccessBegin) {}

  bool visitUse(Operand *op, AccessUseType useTy) override {
    switch (useTy) {
    case AccessUseType::Exact:
      llvm::errs() << "Exact Use: ";
      break;
    case AccessUseType::Inner:
      llvm::errs() << "Inner Use: ";
      break;
    case AccessUseType::Overlapping:
      llvm::errs() << "Overlapping Use ";
      break;
    }
    llvm::errs() << *op->getUser();
    return true;
  }
};

struct AccessPathBaseTest : UnitTest {
  AccessPathBaseTest(UnitTestRunner *pass) : UnitTest(pass) {}
  void invoke(Arguments &arguments) override {
    auto value = arguments.takeValue();
    getFunction()->dump();
    llvm::outs() << "Access path base: " << value;
    auto accessPathWithBase = AccessPathWithBase::compute(value);
    AccessUseTestVisitor visitor;
    visitAccessPathBaseUses(visitor, accessPathWithBase, getFunction());
  }
};

/// [new_tests] Add the new UnitTest subclass above this line. 

//===----------------------------------------------------------------------===//
// MARK: Unit Test Registration
//===----------------------------------------------------------------------===//

template <typename Doit>
void UnitTestRunner::withTest(StringRef name, Doit doit) {
#define ADD_UNIT_TEST_SUBCLASS(STRING, SUBCLASS)                               \
  if (name == STRING) {                                                        \
    SUBCLASS it{this};                                                         \
    doit(&it);                                                                 \
    return;                                                                    \
  }

    // Alphabetical mapping from string to unit test subclass.
    ADD_UNIT_TEST_SUBCLASS("accesspath-base", AccessPathBaseTest)
    ADD_UNIT_TEST_SUBCLASS("canonicalize-ossa-lifetime", CanonicalizeOSSALifetimeTest)
    ADD_UNIT_TEST_SUBCLASS("canonicalize-borrow-scope",
                           CanonicalizeBorrowScopeTest)
    ADD_UNIT_TEST_SUBCLASS("dump-function", DumpFunction)
    ADD_UNIT_TEST_SUBCLASS("extended-liveness", ExtendedLinearLivenessTest)
    ADD_UNIT_TEST_SUBCLASS("find-borrow-introducers", FindBorrowIntroducers)
    ADD_UNIT_TEST_SUBCLASS("find-enclosing-defs", FindEnclosingDefsTest)
    ADD_UNIT_TEST_SUBCLASS("function-get-self-argument-index", FunctionGetSelfArgumentIndex)
    ADD_UNIT_TEST_SUBCLASS("has-pointer-escape", OwnershipUtilsHasPointerEscape)
    ADD_UNIT_TEST_SUBCLASS("print-type-lowering", PrintTypeLowering)
    ADD_UNIT_TEST_SUBCLASS("interior-liveness", InteriorLivenessTest)
    ADD_UNIT_TEST_SUBCLASS("is-deinit-barrier", IsDeinitBarrierTest)
    ADD_UNIT_TEST_SUBCLASS("is-lexical", IsLexicalTest)
    ADD_UNIT_TEST_SUBCLASS("lexical-destroy-folding", LexicalDestroyFoldingTest)
    ADD_UNIT_TEST_SUBCLASS("linear-liveness", LinearLivenessTest)
    ADD_UNIT_TEST_SUBCLASS("multidef-liveness", MultiDefLivenessTest)
    ADD_UNIT_TEST_SUBCLASS("multidefuse-liveness", MultiDefUseLivenessTest)
    ADD_UNIT_TEST_SUBCLASS("fieldsensitive-multidefuse-liverange", FieldSensitiveMultiDefUseLiveRangeTest)
    ADD_UNIT_TEST_SUBCLASS("ossa-lifetime-completion", OSSALifetimeCompletionTest)
    ADD_UNIT_TEST_SUBCLASS("pruned-liveness-boundary-with-list-of-last-users-insertion-points", PrunedLivenessBoundaryWithListOfLastUsersInsertionPointsTest)
    ADD_UNIT_TEST_SUBCLASS("shrink-borrow-scope", ShrinkBorrowScopeTest)

    // SimplifyCFG unit tests
    ADD_UNIT_TEST_SUBCLASS("simplify-cfg-simplify-argument",
                           SimplifyCFGSimplifyArgument)
    ADD_UNIT_TEST_SUBCLASS("simplify-cfg-simplify-block-args",
                           SimplifyCFGSimplifyBlockArgs)
    ADD_UNIT_TEST_SUBCLASS("simplify-cfg-canonicalize-switch-enum",
                           SimplifyCFGCanonicalizeSwitchEnum)
    ADD_UNIT_TEST_SUBCLASS("simplify-cfg-simplify-switch-enum-block",
                           SimplifyCFGSimplifySwitchEnumBlock)
    ADD_UNIT_TEST_SUBCLASS(
        "simplify-cfg-simplify-switch-enum-unreachable-blocks",
        SimplifyCFGSimplifySwitchEnumUnreachableBlocks)
    ADD_UNIT_TEST_SUBCLASS(
        "simplify-cfg-simplify-switch-enum-on-objc-class-optional",
        SimplifyCFGSwitchEnumOnObjcClassOptional)
    ADD_UNIT_TEST_SUBCLASS(
        "simplify-cfg-simplify-term-with-identical-dest-blocks",
        SimplifyCFGSimplifyTermWithIdenticalDestBlocks)
    ADD_UNIT_TEST_SUBCLASS("simplify-cfg-try-jump-threading",
                           SimplifyCFGTryJumpThreading)
    ADD_UNIT_TEST_SUBCLASS("scoped-address-liveness", ScopedAddressLivenessTest)

    ADD_UNIT_TEST_SUBCLASS("ssa-liveness", SSALivenessTest)
    ADD_UNIT_TEST_SUBCLASS("test-specification-parsing", TestSpecificationTest)
    ADD_UNIT_TEST_SUBCLASS("visit-inner-adjacent-phis",
                           VisitInnerAdjacentPhisTest)
    /// [new_tests] Add the new mapping from string to subclass above this line.
    ///             Please sort alphabetically by name to help reduce merge
    ///             conflicts.

#undef ADD_UNIT_TEST_SUBCLASS
    llvm::errs() << "No test named: " << name << "\n";
    assert(false && "Invalid test name");
  }


template <typename Analysis>
Analysis *UnitTest::getAnalysis() {
  return pass->getAnalysis<Analysis>();
}

SILFunction *UnitTest::getFunction() { return getPass()->getFunction(); }

} // anonymous namespace

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

SILTransform *swift::createUnitTestRunner() { return new UnitTestRunner(); }
