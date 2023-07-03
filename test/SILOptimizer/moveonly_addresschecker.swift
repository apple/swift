// RUN: %target-swift-emit-sil -O -sil-verify-all -verify -enable-experimental-feature MoveOnlyPartialConsumption -enable-experimental-feature NoImplicitCopy -enable-experimental-feature MoveOnlyClasses %s

// This file contains tests that used to crash due to verifier errors. It must
// be separate from moveonly_addresschecker_diagnostics since when we fail on
// the diagnostics in that file, we do not actually run the verifier.

struct TestTrivialReturnValue : ~Copyable {
    var i: Int = 5

    // We used to error on return buffer.
    consuming func drain() -> Int {
        let buffer = (consume self).i
        self = .init(i: 5)
        return buffer
    }
}
