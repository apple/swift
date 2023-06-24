// RUN: %empty-directory(%t)

// >> first try when no library evolution is specified
// RUN: %target-swift-frontend -emit-module -o %t/SorryModule.swiftmodule %S/Inputs/discard_module_defining.swift %S/Inputs/discard_module_adjacent.swift
// RUN: %target-typecheck-verify-swift -I %t

// >> now again with library evolution; we expect the same result.
// RUN: %target-swift-frontend -enable-library-evolution -emit-module -o %t/SorryModule.swiftmodule %S/Inputs/discard_module_defining.swift %S/Inputs/discard_module_adjacent.swift -enable-experimental-feature MoveOnlyResilientTypes
// RUN: %target-typecheck-verify-swift -I %t

// "Sorry" is meaningless
import SorryModule

extension Regular {
  __consuming func delete() {
    // FIXME: rdar://108933330 (cannot define struct deinit with -enable-library-evolution)
//    discard self // DISABLED-error {{can only 'discard' from the same module defining type 'Regular'}}
  }
}

extension Frozen {
  __consuming func delete() {
    discard self // expected-error {{can only 'discard' from the same module defining type 'Frozen'}}
  }
}
