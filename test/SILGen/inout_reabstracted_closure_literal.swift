// RUN: %target-swift-emit-silgen %s

func bar<T>(f: (inout T) throws -> Void) {}

func condition() -> Bool { fatalError() }
func error() -> Error { fatalError() }

func foo(x: @escaping () -> Void) {
    // CHECK-LABEL: sil private [ossa] @${{.*}}3foo{{.*}}U_ :
    // CHECK:       bb0(
    // CHECK-SAME:    [[ARG:%.*]] = $*@callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <()>,
    // CHECK-SAME:    [[CAPTURE:%.*]] = @closureCapture @guaranteed $@callee_guaranteed () -> ()

    // Reabstract the initial value of the inout parameter
    // CHECK:         [[INITIAL:%.*]] = load [take] [[ARG]]
    // CHECK:         [[INITIAL_CONV:%.*]] = convert_function [[INITIAL]]
    // CHECK:         [[THUNK:%.*]] = function_ref @{{.*}}_TR
    // CHECK:         [[INITIAL_REAB:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[INITIAL_CONV]])
    // CHECK:         [[REAB_ARG:%.*]] = alloc_stack
    // CHECK:         store [[INITIAL_REAB]] to [[REAB_ARG]]
    bar {
        // Read from the reabstracted buffer
        //  CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[REAB_ARG]]
        //  CHECK: [[COPY]] = load [copy] [[ACCESS]]
        _ = $0 as () -> Void

        // Assign to the reabstracted buffer
        $0 = x

        // Writeback occurs no matter how we exit the function
        // CHECK:   cond_br {{%.*}}, [[THEN1:bb[0-9]+]], [[ELSE1:bb[0-9]+]]
        if condition() {
            // Throwing:
            // CHECK: [[THEN1]]:
            // Take final reabstracted value from the reabstracted buffer
            // CHECK:   [[FINAL_REAB:%.*]] = load [take] [[REAB_ARG]]
            // Reabstract back to original representation and store back to
            // original buffer
            // CHECK:   [[THUNK_ORIG:%.*]] = function_ref @{{.*}}_TR
            // CHECK:   [[FINAL_CONV:%.*]] = partial_apply [callee_guaranteed] [[THUNK_ORIG]]([[FINAL_REAB]])
            // CHECK:   [[FINAL:%.*]] = convert_function [[FINAL_CONV]]
            // CHECK:   store [[FINAL]] to [[ARG]]
            // CHECK:   throw
            throw error()
        // CHECK: [[ELSE1]]:
        // CHECK:   cond_br {{%.*}}, [[THEN2:bb[0-9]+]], [[ELSE2:bb[0-9]+]]
        } else if condition() {
            // Explicit return
            // CHECK: [[THEN1]]:
            // CHECK:   br [[EPILOG:bb[0-9]+]]
            return
        } else {
            // Fall through
        }
        // CHECK: [[EPILOG]]:
        // Take final reabstracted value from the reabstracted buffer
        // CHECK:   [[FINAL_REAB:%.*]] = load [take] [[REAB_ARG]]
        // Reabstract back to original representation and store back to
        // original buffer
        // CHECK:   [[THUNK_ORIG:%.*]] = function_ref @{{.*}}_TR
        // CHECK:   [[FINAL_CONV:%.*]] = partial_apply [callee_guaranteed] [[THUNK_ORIG]]([[FINAL_REAB]])
        // CHECK:   [[FINAL:%.*]] = convert_function [[FINAL_CONV]]
        // CHECK:   store [[FINAL]] to [[ARG]]
        // CHECK:   return
    }
}
