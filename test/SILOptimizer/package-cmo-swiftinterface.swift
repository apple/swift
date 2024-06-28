// RUN: %empty-directory(%t)

/// First Test: Check `@_usableFromInline` is not added to types in PackageCMO mode.
// RUN: %target-swift-frontend -parse-as-library %s -O -wmo -enable-library-evolution -experimental-allow-non-resilient-access -experimental-package-cmo -module-name=Lib -package-name pkg -emit-module -o %t/Lib-package-cmo.swiftmodule
// RUN: %target-sil-opt -module-name Lib -enable-sil-verify-all %t/Lib-package-cmo.swiftmodule -o %t/Lib-package-cmo.sil
// RUN: %FileCheck %s < %t/Lib-package-cmo.sil

/// Second Test: Check .swiftinterface files with and without PackageCMO have the same decl signatures without `@_usableFromInline`.
// RUN: %target-swift-frontend -emit-module %s -I %t \
// RUN:   -module-name Lib -package-name pkg \
// RUN:   -enable-library-evolution -swift-version 6 \
// RUN:   -emit-module-path %t/Lib.swiftmodule \
// RUN:   -emit-module-interface-path %t/Lib.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/Lib.private.swiftinterface \
// RUN:   -emit-package-module-interface-path %t/Lib.package.swiftinterface
// RUN: %FileCheck %s --check-prefixes=CHECK-PKG-INTERFACE,CHECK-INTERFACE < %t/Lib.package.swiftinterface
// RUN: %FileCheck %s --check-prefix=CHECK-INTERFACE < %t/Lib.swiftinterface

// RUN: rm -rf %t/Lib.swiftmodule
// RUN: rm -rf %t/Lib.swiftinterface
// RUN: rm -rf %t/Lib.private.swiftinterface
// RUN: rm -rf %t/Lib.package.swiftinterface

// RUN: %target-swift-frontend -emit-module %s -I %t \
// RUN:   -module-name Lib -package-name pkg \
// RUN:   -enable-library-evolution -swift-version 6 \
// RUN:   -O -wmo \
// RUN:   -experimental-allow-non-resilient-access -experimental-package-cmo \
// RUN:   -emit-module-path %t/Lib.swiftmodule \
// RUN:   -emit-module-interface-path %t/Lib.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/Lib.private.swiftinterface \
// RUN:   -emit-package-module-interface-path %t/Lib.package.swiftinterface
// RUN: %FileCheck %s --check-prefixes=CHECK-PKG-INTERFACE,CHECK-INTERFACE < %t/Lib.package.swiftinterface
// RUN: %FileCheck %s --check-prefix=CHECK-INTERFACE < %t/Lib.swiftinterface

// REQUIRES: swift_in_compiler

// CHECK-NOT: @usableFromInline
final class InternalKlass: PkgKlass {
    @inline(never)
    override func bar() -> Int { return 13 }
}

package class PkgKlass {
    @inline(never)
    package func bar() -> Int { return 11 }
}

// CHECK-NOT: sil package [serialized_for_package] [noinline] [canonical] @$s3Lib3fooySiSgAA8PkgKlassCF : $@convention(thin) (@guaranteed PkgKlass) -> Optional<Int> {
// CHECK-NOT: checked_cast_br PkgKlass in {{.*}} : $PkgKlass to InternalKlass
@inline(never)
package func foo(_ arg: PkgKlass) -> Int? {
    let x = arg as? InternalKlass
    return x?.bar()
}

public func run() -> Int {
  return PkgKlass().bar()
}

// CHECK-PKG-INTERFACE-NOT: @usableFromInline
// CHECK-INTERFACE-NOT: @usableFromInline
// CHECK-PKG-INTERFACE: package class PkgKlass {
// CHECK-PKG-INTERFACE:   @inline(never) package func bar() -> Swift.Int
// CHECK-PKG-INTERFACE:   @inline(never) package func foo(_ arg: Lib.PkgKlass) -> Swift.Int?
// CHECK-INTERFACE: public func run() -> Swift.Int
