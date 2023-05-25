// RUN: %target-swift-emit-silgen -enable-experimental-feature NoImplicitCopy -enable-library-evolution %s | %FileCheck %s
// RUN: %target-swift-emit-sil -O -sil-verify-all -enable-experimental-feature NoImplicitCopy -enable-library-evolution %s

////////////////////////
// MARK: Declarations //
////////////////////////

public struct EmptyStruct : ~Copyable {}
public struct NonEmptyStruct : ~Copyable {
    var e = EmptyStruct()
}
public class CopyableKlass {
    var s = NonEmptyStruct()

    let letStruct = NonEmptyStruct()
}

public func borrowVal(_ x: borrowing EmptyStruct) {}

//////////////////////
// MARK: DeinitTest //
//////////////////////

// CHECK-LABEL: sil [ossa] @$s26moveonly_library_evolution10DeinitTestVfD : $@convention(method) (@in DeinitTest) -> () {
// CHECK: bb0([[ARG:%.*]] : $*DeinitTest):
// CHECK:   drop_deinit [[ARG]]
// CHECK: } // end sil function '$s26moveonly_library_evolution10DeinitTestVfD'
public struct DeinitTest : ~Copyable {
    deinit {
    }
}

//////////////////////////////////////////
// MARK: Caller Argument Spilling Tests //
//////////////////////////////////////////

// CHECK-LABEL: sil [ossa] @$s26moveonly_library_evolution29callerArgumentSpillingTestArgyyAA13CopyableKlassCF : $@convention(thin) (@guaranteed CopyableKlass) -> () {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $CopyableKlass):
// CHECK:    [[ADDR:%.*]] = ref_element_addr [[ARG]]
// CHECK:    [[MARKED_ADDR:%.*]] = mark_must_check [no_consume_or_assign] [[ADDR]]
// CHECK:    [[LOADED_VALUE:%.*]] = load [copy] [[MARKED_ADDR]]
// CHECK:    [[BORROWED_LOADED_VALUE:%.*]] = begin_borrow [[LOADED_VALUE]]
// CHECK:    [[EXT:%.*]] = struct_extract [[BORROWED_LOADED_VALUE]]
// CHECK:    [[SPILL:%.*]] = alloc_stack $EmptyStruct
// CHECK:    [[STORE_BORROW:%.*]] = store_borrow [[EXT]] to [[SPILL]]
// CHECK:    apply {{%.*}}([[STORE_BORROW]]) : $@convention(thin) (@in_guaranteed EmptyStruct) -> ()
// CHECK:    end_borrow [[STORE_BORROW]]
// CHECK:    end_borrow [[BORROWED_LOADED_VALUE]]
// CHECK: } // end sil function '$s26moveonly_library_evolution29callerArgumentSpillingTestArgyyAA13CopyableKlassCF'
public func callerArgumentSpillingTestArg(_ x: CopyableKlass) {
    borrowVal(x.letStruct.e)
}
