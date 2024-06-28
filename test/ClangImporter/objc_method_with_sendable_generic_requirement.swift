// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-silgen %t/main.swift -import-objc-header %t/Test.h | %FileCheck %t/main.swift

// REQUIRES: objc_interop
// REQUIRES: concurrency


// rdar://127520993

//--- Test.h
#import <Foundation/Foundation.h>

#define SWIFT_SENDABLE __attribute__((__swift_attr__("@Sendable")))

@interface Test<N : id SWIFT_SENDABLE> : NSObject
- (void)luckWithNumber:(nullable N)number;
@end

//--- main.swift
import Foundation

Test<NSNumber>().luck(withNumber: 5)
// CHECK-LABEL: sil [ossa] @main : $@convention(c) (Int32, UnsafeMutablePointer<Optional<UnsafeMutablePointer<Int8>>>) -> Int32
// CHECK: [[NS_NUMBER_INIT:%.*]] = function_ref @$sSo8NSNumberC10FoundationE14integerLiteralABSi_tcfC : $@convention(method) (Int, @thick NSNumber.Type) -> @owned NSNumber
// CHECK-NEXT:  [[NS_NUMBER:%.*]] = apply [[NS_NUMBER_INIT]]({{.*}}) : $@convention(method) (Int, @thick NSNumber.Type) -> @owned NSNumber
// CHECK-NEXT:  [[OPT_NS_NUMBER:%.*]] = enum $Optional<NSNumber>, #Optional.some!enumelt, [[NS_NUMBER]] : $NSNumber
// CHECK-NEXT:  [[LUCK_METHOD_REF:%.*]] = objc_method %4 : $Test<NSNumber>, #Test.luck!foreign : <N where N : AnyObject, N : Sendable> (Test<N>) -> (N?) -> (), $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject, τ_0_0 : Sendable> (Optional<τ_0_0>, Test<τ_0_0>) -> ()
// CHECK-NEXT:  %14 = apply [[LUCK_METHOD_REF]]<NSNumber>([[OPT_NS_NUMBER]], {{.*}}) : $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject, τ_0_0 : Sendable> (Optional<τ_0_0>, Test<τ_0_0>) -> ()
