// RUN: %target-swift-frontend -Osize -emit-sil -sil-stop-optzns-before-lowering-ownership %s | %FileCheck %s

// End to end test that we can extend lifetimes as appropriate.

public final class Klass {}

public final class InternalState {
    private var _klass = Klass()

    public var klass: Klass {
        _read { yield _klass }
    }
}

public struct InternalStateWrapper {
    private var _state: InternalState = InternalState()
    private var state: InternalState {
        _read { yield _state }
    }

    public var klass : Klass {
        // Make sure there is no ARC traffic in this function and that we have
        // simplified all of the ARC possible. We are relying on the verifier to
        // check that we are properly interleaving the coroutines.
        //
        // CHECK-LABEL: sil [ossa] @$s27coroutine_lifetime_extender20InternalStateWrapperV5klassAA5KlassCvr : $@yield_once @convention(method) (@guaranteed InternalStateWrapper) -> @yields @guaranteed Klass {
        // CHECK-NOT: copy_value
        // CHECK-NOT: begin_borrow
        // CHECK: } // end sil function '$s27coroutine_lifetime_extender20InternalStateWrapperV5klassAA5KlassCvr'
        _read { yield state.klass }
    }
}
