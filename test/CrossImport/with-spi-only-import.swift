/// Check cross-import overlays with @_spiOnly imports.

// RUN: %empty-directory(%t)
// RUN: cp -r %S/Inputs/lib-templates/* %t/
// RUN: split-file --leading-lines %s %t

//--- BothPublic.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/BothPublic.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -emit-private-module-interface-path %t.private.swiftinterface -swift-version 6
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %target-swift-typecheck-module-from-interface(%t.private.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %FileCheck %t/BothPublic.swift < %t.swiftinterface
// RUN: %FileCheck %t/BothPublic.swift < %t.private.swiftinterface

import DeclaringLibrary
import BystandingLibrary

// CHECK: // swift-interface-format-version
// CHECK: // swift-module-flags: {{.*}} -module-name ClientLibrary
// CHECK-DAG: import Swift
// CHECK-DAG: import DeclaringLibrary
// CHECK-DAG: import BystandingLibrary
// CHECK-DAG: import _OverlayLibrary


//--- BothHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/BothHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -emit-private-module-interface-path %t.private.swiftinterface -swift-version 6
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %target-swift-typecheck-module-from-interface(%t.private.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %FileCheck %t/BothHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefix NEGATIVE %t/BothHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefixes CHECK,PRIVATE %t/BothHidden.swift < %t.private.swiftinterface

@_spiOnly import DeclaringLibrary
@_spiOnly import BystandingLibrary

// CHECK: // swift-interface-format-version
// CHECK: // swift-module-flags: {{.*}} -module-name ClientLibrary
// CHECK-DAG: import Swift
// NEGATIVE-NOT: import DeclaringLibrary
// NEGATIVE-NOT: import BystandingLibrary
// NEGATIVE-NOT: import _OverlayLibrary
// PRIVATE-DAG: import DeclaringLibrary
// PRIVATE-DAG: import BystandingLibrary
// PRIVATE-DAG: import _OverlayLibrary


//--- FirstHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/FirstHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -emit-private-module-interface-path %t.private.swiftinterface -swift-version 6
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %target-swift-typecheck-module-from-interface(%t.private.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %FileCheck %t/FirstHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefix NEGATIVE %t/FirstHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefixes CHECK,PRIVATE %t/FirstHidden.swift < %t.private.swiftinterface

@_spiOnly import DeclaringLibrary
import BystandingLibrary

// CHECK: // swift-interface-format-version
// CHECK: // swift-module-flags: {{.*}} -module-name ClientLibrary
// CHECK-DAG: import Swift
// CHECK-DAG: import BystandingLibrary
// NEGATIVE-NOT: import DeclaringLibrary
// NEGATIVE-NOT: import _OverlayLibrary
// PRIVATE-DAG: import DeclaringLibrary
// PRIVATE-DAG: import _OverlayLibrary


//--- SecondHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/SecondHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -emit-private-module-interface-path %t.private.swiftinterface -swift-version 6
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %target-swift-typecheck-module-from-interface(%t.private.swiftinterface) -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary
// RUN: %FileCheck %t/SecondHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefix NEGATIVE %t/SecondHidden.swift < %t.swiftinterface
// RUN: %FileCheck -check-prefixes CHECK,PRIVATE %t/SecondHidden.swift < %t.private.swiftinterface

import DeclaringLibrary
@_spiOnly import BystandingLibrary

// CHECK: // swift-interface-format-version
// CHECK: // swift-module-flags: {{.*}} -module-name ClientLibrary
// CHECK-DAG: import Swift
// CHECK-DAG: import DeclaringLibrary
// NEGATIVE-NOT: import BystandingLibrary
// NEGATIVE-NOT: import _OverlayLibrary
// PRIVATE-DAG: import BystandingLibrary
// PRIVATE-DAG: import _OverlayLibrary
