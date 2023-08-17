// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %swift -target arm64-apple-macos13.0 -target-variant arm64-apple-ios16.0-macabi -typecheck -parse-as-library %t/test.swift -emit-tbd -emit-tbd-path %t/zippered.tbd
// RUN: %llvm-readtapi %t/zippered.tbd %t/expected.tbd

//--- test.swift
public func foo() -> Bool {
    return true
}

//--- expected.tbd
--- !tapi-tbd
tbd-version:     4
targets:         [ arm64-macos, arm64-maccatalyst ]
flags:           [ not_app_extension_safe ]
install-name:    ''
current-version: 0
compatibility-version: 0
swift-abi-version: 7
exports:
  - targets:         [ arm64-macos, arm64-maccatalyst ]
    symbols:         [ '_$s4test3fooSbyF' ]
...
