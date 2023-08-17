// RUN: %target-swift-ide-test -print-module -module-to-print=UnsafeProjections -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: struct View {
// CHECK:   func data() -> UnsafeMutableRawPointer!
// CHECK:   func empty() -> UnsafeMutableRawPointer!
// CHECK:   func name() -> std{{.*}}string
// CHECK:   func nested() -> NestedSelfContained
// CHECK:   func explicitSelfContained() -> ExplicitSelfContained
// CHECK:   func explicitNested() -> NestedExplicitSelfContained
// CHECK: }

// CHECK: struct SelfContained {
// CHECK:   func name() -> std{{.*}}string
// CHECK:   func selfContained() -> SelfContained
// CHECK:   func nested() -> NestedSelfContained
// CHECK:   func empty() -> Empty
// CHECK:   func value() -> Int32
// CHECK:   func __viewUnsafe() -> View
// CHECK:   func __pointerUnsafe() -> UnsafeMutablePointer<Int32>!
// CHECK:   func explicitSelfContained() -> ExplicitSelfContained
// CHECK:   func explicitNested() -> NestedExplicitSelfContained
// CHECK: }

// CHECK: struct NestedSelfContained {
// CHECK:   func name() -> std{{.*}}string
// CHECK:   func selfContained() -> SelfContained
// CHECK:   func nested() -> NestedSelfContained
// CHECK:   func empty() -> Empty
// CHECK:   func value() -> Int32
// CHECK:   func __viewUnsafe() -> View
// CHECK:   func __pointerUnsafe() -> UnsafeMutablePointer<Int32>!
// CHECK:   func explicitSelfContained() -> ExplicitSelfContained
// CHECK:   func explicitNested() -> NestedExplicitSelfContained
// CHECK: }

// CHECK: struct InheritSelfContained {
// CHECK:   func name() -> std{{.*}}string
// CHECK:   func selfContained() -> SelfContained
// CHECK:   func nested() -> NestedSelfContained
// CHECK:   func empty() -> Empty
// CHECK:   func value() -> Int32
// CHECK:   func __viewUnsafe() -> View
// CHECK:   func __pointerUnsafe() -> UnsafeMutablePointer<Int32>!
// CHECK:   func explicitSelfContained() -> ExplicitSelfContained
// CHECK:   func explicitNested() -> NestedExplicitSelfContained
// CHECK: }

// CHECK: struct ExplicitSelfContained {
// CHECK:   func __pointerUnsafe() -> UnsafeMutableRawPointer!
// CHECK:   func __viewUnsafe() -> View
// CHECK:   func nested() -> NestedSelfContained
// CHECK: }

// CHECK: struct NestedExplicitSelfContained {
// CHECK:   func selfContained() -> SelfContained
// CHECK:   func nested() -> NestedSelfContained
// CHECK:   func value() -> Int32
// CHECK:   func __viewUnsafe() -> View
// CHECK:   func __pointerUnsafe() -> UnsafeMutablePointer<Int32>!
// CHECK: }

// CHECK: struct Empty {
// CHECK:   func empty() -> Empty
// CHECK:   func pointer() -> UnsafeMutableRawPointer!
// CHECK:   func selfContained() -> SelfContained
// CHECK: }

// CHECK: struct IntPair {
// CHECK:   func first() -> Int32
// CHECK:   func pointer() -> UnsafeMutableRawPointer!
// CHECK:   func selfContained() -> SelfContained
// CHECK: }
