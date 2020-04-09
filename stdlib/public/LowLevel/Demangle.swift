//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

/// Given a mangled Swift symbol, demangle it into a human readable format.
///
/// Valid Swift symbols begin with the following prefixes:
///   ┌─────────────────────╥────────┐
///   │ Swift Version       ║ Prefix │
///   ╞═════════════════════╬════════╡
///   │ Swift 3 and below   ║   _T   │
///   ├─────────────────────╫────────┤
///   │ Swift 4             ║  _T0   │
///   ├─────────────────────╫────────┤
///   │ Swift 4.x           ║   $S   │
///   ├─────────────────────╫────────┤
///   │ Swift 5+            ║   $s   │
///   └─────────────────────╨────────┘
///
/// - Parameters:
///   - mangledName: A mangled Swift symbol.
/// - Returns: A human readable demangled Swift symbol.
public func demangle(
  _ mangledName: String,
  simplified: Bool = false
) -> String? {
  return mangledName.utf8CString.withUnsafeBufferPointer {
    let demangledPtr = _swift_stdlib_demangle(
      /* mangledName */ $0.baseAddress,
      /* mangledNameLength */ $0.count - 1,
      /* outputBuffer */ nil,
      /* outputBufferSize */ nil,
      /* flags */ simplified ? 0x2 : 0x1
    )

    guard demangledPtr != nil else {
      return nil
    }

    let demangledName = String(cString: demangledPtr!)
    _swift_stdlib_free(demangledPtr!)
    return demangledName
  }
}
