// RUN: %target-swift-frontend -emit-ir -target %target-cpu-apple-macos99.99 %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-PRESENT %s
// RUN: %target-swift-frontend -emit-ir -target %target-cpu-apple-macos14.4 %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-SUPPRESSED %s

// REQUIRES: OS=macosx
// UNSUPPORTED: CPU=arm64e

public struct MyStruct {
  let fn: @isolated(any) () -> ()
}

// Make sure that we only emit a demangling-based type description
// for @isolated(any) types when deploying to runtimes that support it.
// If we don't, we fall back on using a type metadata accessor, which
// is fine.  Since this is for reflective metadata, we could go a step
// further if we decide we really only care about layout equivalence for
// these; if so, we could just suppress the @isolated(any) part of the
// type completely, since it has the the same external layout as an
// ordinary function type.
// rdar://129861211

// CHECK-LABEL: @"$s32reflection_metadata_isolated_any8MyStructVMF" = internal constant
// CHECK-PRESENT-SAME: ptr @"symbolic yyYAc"
// CHECK-SUPPRESSED-SAME: ptr @"get_type_metadata yyYAc.1"
