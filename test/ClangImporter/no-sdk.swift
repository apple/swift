// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -typecheck -sdk "" -I %S/Inputs/custom-modules %s

// Verify that we can still import modules even without an SDK.
import ExternIntX

let y: CInt = ExternIntX.x
