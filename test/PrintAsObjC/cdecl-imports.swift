// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck %s -parse-as-library -emit-objc-header-path %t/swift.h
// RUN: %FileCheck %s < %t/swift.h
// RUN: %check-in-clang %t/swift.h

// REQUIRES: objc_interop

import CoreGraphics
import Foundation

// CHECK: @import CoreGraphics;
// CHECK-NOT: @import Foundation;

// CHECK: @class Bee;
// CHECK-LABEL: Bee * _Nonnull fwd_declares_bee(void) SWIFT_WARN_UNUSED_RESULT;

@_cdecl("fwd_declares_bee")
public func fwdDeclaresBee() -> Bee { fatalError() }

// CHECK: @class Hive;
// CHECK-LABEL: void fwd_declares_hive(SWIFT_NOESCAPE Hive * _Nonnull (* _Nonnull bzzz)(Bee * _Nonnull));

@_cdecl("fwd_declares_hive")
public func fwdDeclaresHive(bzzz: @convention(c) (Bee) -> Hive) { fatalError() }

// CHECK: @protocol NSWobbling;
// CHECK-LABEL: void fwd_declares_wobble(id <NSWobbling> _Nonnull wobbler);

@_cdecl("fwd_declares_wobble")
public func fwdDeclaresWobble(wobbler: NSWobbling) { fatalError() }

@_cdecl("imports_cgpoint")
public func importsCGPoint(pt: CGPoint) { fatalError() }
