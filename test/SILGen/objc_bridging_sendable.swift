// RUN: %target-swift-emit-silgen(mock-sdk: %clang-importer-sdk) -import-objc-header %S/Inputs/objc_bridging_sendable.h %s | %FileCheck %s

// REQUIRES: objc_interop

// CHECK-LABEL: sil [ossa] @$s22objc_bridging_sendable18passSendableToObjCyys0E0_pF : $@convention(thin) (@in_guaranteed any Sendable) -> () {
// CHECK:  function_ref @$ss27_bridgeAnythingToObjectiveCyyXlxlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> @owned AnyObject
// CHECK: objc_method {{%.*}} : $NSBlah, #NSBlah.takeSendable!foreign : (NSBlah) -> ((any Sendable)?) -> (), $@convention(objc_method) (Optional<AnyObject>, NSBlah) -> ()
// CHECK: return
public func passSendableToObjC(_ s: Sendable) {
  NSBlah().takeSendable(s)
}

public func useSendableProperty(_ ns: NSBlah) {
  _ = ns.x
  let _: (Int, Any, String, [Any]) = (42, ns.x, "", [1, 2, 3])
}

// CHECK-LABEL: sil private [ossa] @$s22objc_bridging_sendable23test_use_of_buffer_inityyKFypSo6NSBlahCKXEfU_ : $@convention(thin) @substituted <τ_0_0> (@guaranteed NSBlah) -> (@out τ_0_0, @error any Error) for <Any>
// CHECK: bb0(%0 : $*Any, %1 : @guaranteed $NSBlah):
// CHECK: [[TEST_REF:%.*]] = objc_method %1 : $NSBlah, #NSBlah.test!foreign : (NSBlah) -> () throws -> any Sendable, $@convention(objc_method) (Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>, NSBlah) -> @autoreleased Optional<AnyObject>
// CHECK: [[RESULT:%.*]] = apply [[TEST_REF]]({{.*}}, %1) : $@convention(objc_method) (Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>, NSBlah) -> @autoreleased Optional<AnyObject>
// CHECK: switch_enum [[RESULT]] : $Optional<AnyObject>, case #Optional.some!enumelt: bb1, case #Optional.none!enumelt: bb2
// CHECK: bb1([[SUCCESS:%.*]] : @owned $AnyObject)
// CHECK-NEXT: [[OPT_RESULT_VALUE:%.*]] = unchecked_ref_cast [[SUCCESS]] : $AnyObject to $Optional<AnyObject>
// CHECK-NEXT: // function_ref _bridgeAnyObjectToAny(_:)
// CHECK-NEXT: [[BRIDGE_INTRINSIC_REF:%.*]] = function_ref @$ss018_bridgeAnyObjectToB0yypyXlSgF : $@convention(thin) (@guaranteed Optional<AnyObject>) -> @out Any
// CHECK-NEXT:  apply [[BRIDGE_INTRINSIC_REF]](%0, [[OPT_RESULT_VALUE]]) : $@convention(thin) (@guaranteed Optional<AnyObject>) -> @out Any
func test_use_of_buffer_init() throws {
  func test<T: Sendable>(_: (NSBlah) throws -> T) rethrows -> T {
    fatalError()
  }

  let _: Any = try test {
      try $0.test()
  }
}
