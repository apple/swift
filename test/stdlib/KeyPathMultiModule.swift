// -- Test with resilience enabled
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -whole-module-optimization -enable-library-evolution -module-name KeyPathMultiModule_b -c -o %t/KeyPathMultiModule_b.o -emit-module-path %t/KeyPathMultiModule_b.swiftmodule -parse-as-library %S/Inputs/KeyPathMultiModule_b.swift
// RUN: %target-build-swift -g %t/KeyPathMultiModule_b.o -I %t %s -o %t/a.out.resilient
// RUN: %target-codesign %t/a.out.resilient
// RUN: %target-run %t/a.out.resilient

// -- Test again with resilience disabled, which changes the circumstances under
//    which we emit and use property descriptors
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -whole-module-optimization -module-name KeyPathMultiModule_b -c -o %t/KeyPathMultiModule_b.o -emit-module-path %t/KeyPathMultiModule_b.swiftmodule -parse-as-library %S/Inputs/KeyPathMultiModule_b.swift
// RUN: %target-build-swift %t/KeyPathMultiModule_b.o -I %t %s -o %t/a.out.fragile
// RUN: %target-codesign %t/a.out.fragile
// RUN: %target-run %t/a.out.fragile

// REQUIRES: executable_test

import KeyPathMultiModule_b
import StdlibUnittest

var keyPathMultiModule = TestSuite("key paths across multiple modules")

func expectEqualWithHashes<T: Hashable>(_ x: T, _ y: T,
                                        file: String = #file,
                                        line: UInt = #line) {
  expectEqual(x, y, file: file, line: line)
  expectEqual(x.hashValue, y.hashValue, file: file, line: line)
}

class LocalSub: ResilientSub {
  override var virtual: String {
    get {
      return "bas"
    }
    set {
    }
  }

  final var storedD: String = "zim"
  final var storedE: String = "zang"

  var localSubA: String {
    get { return "zung" }
  }
  var localSubB: String {
    get { return "zipiti" }
  }
}

keyPathMultiModule.test("identity across multiple modules") {
  // Do this twice, to ensure that fully constant key paths remain stable
  // after one-time instantiation of the object
  for _ in 1...2 {
    expectEqualWithHashes(A_x_keypath(), \A.x)
    expectEqualWithHashes(A_y_keypath(), \A.y)
    expectEqualWithHashes(A_z_keypath(), \A.z)
    expectEqualWithHashes(A_w_keypath(), \A.w)
    expectEqualWithHashes(A_immutable_keypath(), \A.immutable)
    expectEqualWithHashes(A_subscript_withGeneric_keypath(index: 0), \A.[withGeneric: 0])
    expectEqualWithHashes(A_subscript_withGeneric_keypath(index: "butt"),
                \A.[withGeneric: "butt"])
    expectEqualWithHashes(A_subscript_withGeneric_butt_keypath(),
                \A.[withGeneric: "pomeranian's big butt"])

    expectEqualWithHashes(A_subscript_withGenericSettable_keypath(index: 0),
                \A.[withGenericSettable: 0])
    expectEqualWithHashes(A_subscript_withGenericSettable_keypath(index: "butt"),
                \A.[withGenericSettable: "butt"])

    expectEqualWithHashes(A_subscript_withGenericPrivateSet_keypath(index: 0),
                \A.[withGenericPrivateSet: 0])
    expectEqualWithHashes(A_subscript_withGenericPrivateSet_keypath(index: "butt"),
                \A.[withGenericPrivateSet: "butt"])

    do {
      let lifetimeTracker = LifetimeTracked(679)
      expectEqualWithHashes(A_subscript_withGeneric_keypath(index: lifetimeTracker),
                  \A.[withGeneric: lifetimeTracker])
      expectEqualWithHashes(A_subscript_withGenericSettable_keypath(index: lifetimeTracker),
                  \A.[withGenericSettable: lifetimeTracker])
      expectEqualWithHashes(A_subscript_withGenericPrivateSet_keypath(index: lifetimeTracker),
                  \A.[withGenericPrivateSet: lifetimeTracker])

      expectEqualWithHashes(\A.[withGeneric: lifetimeTracker].appendTest,
                  (\A.[withGeneric: lifetimeTracker])
                    .appending(path: \LifetimeTracked.appendTest))
      expectEqualWithHashes(\A.[withGenericSettable: lifetimeTracker].appendTest,
                  (\A.[withGenericSettable: lifetimeTracker])
                    .appending(path: \LifetimeTracked.appendTest))
      expectEqualWithHashes(\A.[withGenericPrivateSet: lifetimeTracker].appendTest,
                  (\A.[withGenericPrivateSet: lifetimeTracker])
                    .appending(path: \LifetimeTracked.appendTest))
    }

    expectEqualWithHashes(B_x_keypath(Double.self), \B<Double>.x)
    expectEqualWithHashes(B_Int_x_keypath(), \B<Int>.x)
    expectEqualWithHashes(B_y_keypath(Double.self), \B<Double>.y)
    expectEqualWithHashes(B_Int_y_keypath(), \B<Int>.y)
    expectEqualWithHashes(B_z_keypath(Double.self), \B<Double>.z)
    expectEqualWithHashes(B_Int_z_keypath(), \B<Int>.z)
    expectEqualWithHashes(B_subscript_withInt_keypath(Double.self, index: 1738),
                \B<Double>.[withInt: 1738])
    expectEqualWithHashes(B_subscript_withInt_keypath(Double.self, index: 679),
                \B<Double>.[withInt: 679])
    expectEqualWithHashes(B_Double_subscript_withInt_0_keypath(),
                \B<Double>.[withInt: 0])
    expectEqualWithHashes(B_subscript_withGeneric_keypath(Double.self, index: "buttt"),
                \B<Double>.[withGeneric: "buttt"])
    expectEqualWithHashes(B_Double_subscript_withGeneric_butt_keypath(),
                \B<Double>.[withGeneric: "Never is the universal butt type"])

    expectEqualWithHashes(A_storedA_keypath(), \A.storedA)
    expectEqualWithHashes(A_storedA_storedB_keypath(), \A.storedA.storedB)
    expectEqualWithHashes(A_storedB_keypath(), \A.storedB)
    expectEqualWithHashes(B_storedA_keypath(Double.self), \B<Double>.storedA)
    expectEqualWithHashes(B_storedB_keypath(Double.self), \B<Double>.storedB)
    expectEqualWithHashes(B_Int_storedA_keypath(), \B<Int>.storedA)
    expectEqualWithHashes(B_Int_storedB_keypath(), \B<Int>.storedB)

    func testInGenericContext<X, Y: Hashable, Z>(x: X, y: Y,
                                              appending: KeyPath<Y, Z>) {
      expectEqualWithHashes(A_subscript_withGeneric_keypath(index: y),
                            \A.[withGeneric: y])
      expectEqualWithHashes(A_subscript_withGenericSettable_keypath(index: y),
                            \A.[withGenericSettable: y])
      expectEqualWithHashes(A_subscript_withGenericPrivateSet_keypath(index: y),
                            \A.[withGenericPrivateSet: y])

      _ = (\A.[withGeneric: y]).appending(path: appending)
      _ = (\A.[withGenericSettable: y]).appending(path: appending)
      _ = (\A.[withGenericPrivateSet: y]).appending(path: appending)

      expectEqualWithHashes(B_x_keypath(X.self), \B<X>.x)
      expectEqualWithHashes(B_y_keypath(X.self), \B<X>.y)
      expectEqualWithHashes(B_z_keypath(X.self), \B<X>.z)
      expectEqualWithHashes(B_subscript_withInt_keypath(X.self, index: 0),
                  \B<X>.[withInt: 0])
      expectEqualWithHashes(B_subscript_withGeneric_keypath(X.self, index: y),
                  \B<X>.[withGeneric: y])
      expectEqualWithHashes(B_subscript_withGenericSettable_keypath(X.self, index: y),
                  \B<X>.[withGenericSettable: y])
      expectEqualWithHashes(B_subscript_withGenericPrivateSet_keypath(X.self, index: y),
                  \B<X>.[withGenericPrivateSet: y])

      _ = (\B<X>.[withGeneric: y]).appending(path: appending)
      _ = (\B<X>.[withGenericSettable: y]).appending(path: appending)
      _ = (\B<X>.[withGenericPrivateSet: y]).appending(path: appending)

      expectEqualWithHashes(B_storedA_keypath(X.self), \B<X>.storedA)
      expectEqualWithHashes(B_storedB_keypath(X.self), \B<X>.storedB)
    }

    testInGenericContext(x: 0.0, y: 42, appending: \Int.appendTest)
    testInGenericContext(x: "pomeranian", y: "big butt",
                         appending: \String.appendTest)
    testInGenericContext(x: LifetimeTracked(17), y: LifetimeTracked(38),
                         appending: \LifetimeTracked.appendTest)

    expectEqualWithHashes(ResilientRoot_storedA_keypath(),
                          \ResilientRoot.storedA)
    expectEqualWithHashes(ResilientRoot_storedB_keypath(),
                          \ResilientRoot.storedB)
    expectEqualWithHashes(ResilientRoot_storedLet_keypath(),
                          \ResilientRoot.storedLet)
    expectEqualWithHashes(ResilientRoot_virtual_keypath(),
                          \ResilientRoot.virtual)
    expectEqualWithHashes(ResilientRoot_virtualRO_keypath(),
                          \ResilientRoot.virtualRO)
    expectEqualWithHashes(ResilientRoot_final_keypath(),
                          \ResilientRoot.final)
    expectEqualWithHashes(ResilientSub_storedA_keypath(),
                          \ResilientSub.storedA)
    expectEqualWithHashes(ResilientSub_storedB_keypath(),
                          \ResilientSub.storedB)
    expectEqualWithHashes(ResilientSub_storedC_keypath(),
                          \ResilientSub.storedC)
    expectEqualWithHashes(ResilientSub_virtual_keypath(),
                          \ResilientSub.virtual)
    expectEqualWithHashes(ResilientSub_virtualRO_keypath(),
                          \ResilientSub.virtualRO)
    expectEqualWithHashes(ResilientSub_final_keypath(),
                          \ResilientSub.final)
    expectEqualWithHashes(ResilientSub_sub_keypath(),
                          \ResilientSub.sub)
    expectEqualWithHashes(ResilientSub_subRO_keypath(),
                          \ResilientSub.subRO)

    // Ensure that we can instantiate key paths on a local subclass of a
    // resilient class, and that they have distinct values.
    let kps: [PartialKeyPath<LocalSub>] = [
      \LocalSub.storedA,
      \LocalSub.storedB,
      \LocalSub.storedC,
      \LocalSub.storedD,
      \LocalSub.storedE,
      \LocalSub.virtual,
      \LocalSub.sub,
      \LocalSub.localSubA,
      \LocalSub.localSubB,
    ]

    for i in kps.indices {
      for j in kps.indices {
        if i == j { continue }
        expectNotEqual(kps[i], kps[j])
      }
    }

    func testInGenericContext2<W: ResilientSubProto>(_: W.Type) {
      expectEqualWithHashes(ResilientRootProto_root_keypath(W.self),
                            \W.root)
      expectEqualWithHashes(ResilientSubProto_sub_keypath(W.self),
                            \W.sub)
    }

    testInGenericContext2(Int.self)
  }
}

runAllTests()
