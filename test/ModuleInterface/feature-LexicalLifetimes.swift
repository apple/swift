// RUN: %empty-directory(%t)

// RUN: %target-swift-emit-module-interface(%t/FeatureTest.swiftinterface) %s -module-name FeatureTest -disable-availability-checking
// RUN: %target-swift-typecheck-module-from-interface(%t/FeatureTest.swiftinterface) -module-name FeatureTest -disable-availability-checking
// RUN: %FileCheck %s < %t/FeatureTest.swiftinterface

// CHECK: #if compiler(>=5.3) && $LexicalLifetimes
// CHECK-NEXT: @_noEagerMove public struct Permanent {
// CHECK-NEXT: }
// CHECK-NEXT: #else
// CHECK-NEXT: public struct Permanent {
// CHECK-NEXT: }
// CHECK-NEXT: #endif
@_noEagerMove
public struct Permanent {}

// CHECK: #if compiler(>=5.3) && $LexicalLifetimes
// CHECK-NEXT: @_hasMissingDesignatedInitializers @_eagerMove public class Transient {
// CHECK-NEXT:   deinit
// CHECK-NEXT: }
// CHECK-NEXT: #else
// CHECK-NEXT: @_hasMissingDesignatedInitializers public class Transient {
// CHECK-NEXT:   deinit
// CHECK-NEXT: }
// CHECK-NEXT: #endif
@_eagerMove 
public class Transient {}

// CHECK: #if compiler(>=5.3) && $LexicalLifetimes
// CHECK-NEXT: @_lexicalLifetimes public func lexicalInAModuleWithoutLexicalLifetimes(_ t: FeatureTest.Transient)
// CHECK-NEXT: #else
// CHECK-NEXT: public func lexicalInAModuleWithoutLexicalLifetimes(_ t: FeatureTest.Transient)
// CHECK-NEXT: #endif
@_lexicalLifetimes
public func lexicalInAModuleWithoutLexicalLifetimes(_ t: Transient) {}
