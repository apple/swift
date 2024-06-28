// RUN: %target-swift-frontend -enable-emit-generic-class-ro_t-list -S %s -o - | %FileCheck %s
// REQUIRES: objc_interop
// REQUIRES: CPU=x86_64 || CPU=arm64
// REQUIRES: OS=macosx

// CHECK: ___unnamed_1:
// CHECK:         .quad   _OBJC_METACLASS_$__TtCs12_SwiftObject
// CHECK:         .quad   0
// CHECK:         .quad   __objc_empty_cache
// CHECK:         .quad   0
// CHECK:         .quad   0
// Start of rodata_t
// CHECK:         .long   128
// CHECK:         .long   16
// CHECK:         .long   16
// CHECK:         .long   0
// CHECK:         .quad   0
// CHECK:         .quad   0
// CHECK:         .quad   __INSTANCE_METHODS__TtC25generic_class_rodata_list9Something
// CHECK:         .quad   0
// CHECK:         .quad   0
// CHECK:         .quad   0
// CHECK:         .quad   0
// Start of rodata_t
// CHECK:         .long   129
// CHECK:         .long   40
// CHECK:         .long   40
// CHECK:         .long   0
// CHECK:         .quad   0
// CHECK:         .quad   0
// CHECK:         .quad   __CLASS_METHODS__TtC25generic_class_rodata_list9Somethin

// CHECK:        .section        __DATA,__objc_clsrolist
// CHECK:        .p2align        3
// CHECK:_generic_ro_datas:
// CHECK:        .quad   ___unnamed_1+40
// CHECK:        .quad   ___unnamed_1+112

import Foundation

public class Something<T> {
  @objc
  public func myMethod() { print("hello") }
  @objc
  public static func myStaticMethod() { print("static") }
}
