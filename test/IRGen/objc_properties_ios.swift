// RUN: %swift -target x86_64-apple-ios9-simulator %S/objc_properties.swift -disable-target-os-checking -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-NEW %S/objc_properties.swift
// RUN: %swift -target x86_64-apple-ios8-simulator %S/objc_properties.swift -disable-target-os-checking -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-OLD %S/objc_properties.swift

// REQUIRES: OS=ios
// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop
