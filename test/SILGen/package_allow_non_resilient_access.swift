// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Utils module needs to opt in to allow non-resilient access from clients.
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON
// CHECK-ON: ALLOW_NON_RESILIENT_ACCESS

/// By default, Client accesses decls in Utils resiliently.
// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg | %FileCheck %s --check-prefixes=CHECK,CHECK-DEFAULT
// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg -enable-library-evolution | %FileCheck %s --check-prefixes=CHECK,CHECK-DEFAULT

/// To bypass resilience at use site, Client needs to be in the same package as its
/// loaded module and also opt in with -experimental-package-bypass-resilience.
// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg -experimental-package-bypass-resilience | %FileCheck %s --check-prefixes=CHECK,CHECK-ACCESS
// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg -experimental-package-bypass-resilience -enable-library-evolution | %FileCheck %s --check-prefixes=CHECK,CHECK-ACCESS

/// Utils can be built with both -enable-testing and -experimental-allow-non-resilient-access.
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-testing \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON
// CHECK-ON: ALLOW_NON_RESILIENT_ACCESS

// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg -experimental-package-bypass-resilience | %FileCheck %s --check-prefixes=CHECK,CHECK-NONRES

/// Opting in for non-resilient access should override skipping non-exportable
/// decls, with a warning.
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-skip-non-exportable-decls \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-1
// CHECK-DIAG-1: warning: ignoring -experimental-skip-non-exportable-decls (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Override -experimental-skip-non-inlinable-function-bodies-without-types with warning
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-skip-non-inlinable-function-bodies-without-types \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-2
// CHECK-DIAG-2: warning: ignoring -experimental-skip-non-inlinable-function-bodies-without-types (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Override -experimental-skip-non-inlinable-function-bodies with warning
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-skip-non-inlinable-function-bodies \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-3
// CHECK-DIAG-3: warning: ignoring -experimental-skip-non-inlinable-function-bodies (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Override -experimental-skip-all-function-bodies with warning
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-skip-all-function-bodies \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-4
// CHECK-DIAG-4: warning: ignoring -experimental-skip-all-function-bodies (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Override -experimental-lazy-typecheck with warning
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -experimental-lazy-typecheck \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-5
// CHECK-DIAG-5: warning: ignoring -experimental-lazy-typecheck (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Override -tbd-is-installapi with warning
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -tbd-is-installapi \
// RUN:   -experimental-allow-non-resilient-access \
// RUN:   -emit-module -emit-module-path %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-TBD
// CHECK-DIAG-TBD: warning: ignoring -tbd-is-installapi (overriden by -experimental-allow-non-resilient-access)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-ON

/// Build Utils interface files.
// RUN: rm -rf %t/Utils.swiftmodule
// RUN: %target-swift-frontend %t/Utils.swift \
// RUN:   -module-name Utils -swift-version 5 -I %t \
// RUN:   -package-name mypkg \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module \
// RUN:   -emit-module-interface-path %t/Utils.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/Utils.private.swiftinterface \
// RUN:   -emit-package-module-interface-path %t/Utils.package.swiftinterface

/// If built from interface, non-resilient access option is ignored with a waring.
// RUN: %target-swift-frontend -compile-module-from-interface \
// RUN: -module-name Utils \
// RUN: -package-name mpkg \
// RUN: -experimental-allow-non-resilient-access \
// RUN: %t/Utils.package.swiftinterface -o %t/Utils.swiftmodule \
// RUN: 2>&1 | %FileCheck %s --check-prefix=CHECK-DIAG-INTERFACE
// CHECK-DIAG-INTERFACE: warning: ignoring -experimental-allow-non-resilient-access (overriden by -compile-module-from-interface or -typecheck-module-from-interface)
// RUN: llvm-bcanalyzer --dump %t/Utils.swiftmodule | %FileCheck %s --check-prefix=CHECK-OFF
// CHECK-OFF-NOT: ALLOW_NON_RESILIENT_ACCESS

/// Client can't bypass resilience when accessing decls in an interface module.
// RUN: %target-swift-frontend -emit-silgen %t/Client.swift -I %t -module-name Client -package-name mypkg -experimental-package-interface-load -experimental-package-bypass-resilience | %FileCheck %s --check-prefixes=CHECK,CHECK-DEFAULT

//--- Utils.swift
package struct PkgStruct {
  package var pkgVar = 1
  package init() {}
}

public struct PubStruct {
  public var pubVar = 1
  public init() {}
}

//--- Client.swift
import Utils

func foo() {
  print(PkgStruct().pkgVar)
}

// CHECK: sil hidden [ossa] @$s6Client3fooyyF : $@convention(thin) () -> () {
// CHECK-DEFAULT: function_ref @$s5Utils9PkgStructV6pkgVarSivg : $@convention(method) (@in_guaranteed PkgStruct) -> Int
// CHECK-DEFAULT: sil package_external @$s5Utils9PkgStructV6pkgVarSivg : $@convention(method) (@in_guaranteed PkgStruct) -> Int
// CHECK-ACCESS:  function_ref @$s5Utils9PkgStructV6pkgVarSivg
// CHECK-NONRES: struct_extract {{.*}} : $PkgStruct, #PkgStruct.pkgVar

func bar() {
  print(PubStruct().pubVar)
}

// CHECK: sil hidden [ossa] @$s6Client3baryyF : $@convention(thin) () -> () {
// CHECK-DEFAULT: function_ref @$s5Utils9PubStructV6pubVarSivg : $@convention(method) (@in_guaranteed PubStruct) -> Int
// CHECK-DEFAULT: sil @$s5Utils9PubStructV6pubVarSivg : $@convention(method) (@in_guaranteed PubStruct) -> Int
// CHECK-ACCESS: function_ref @$s5Utils9PubStructV6pubVarSivg
// CHECK-NONRES: struct_extract {{.*}} : $PubStruct, #PubStruct.pubVar
