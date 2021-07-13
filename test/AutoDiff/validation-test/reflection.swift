// REQUIRES: no_asan
// RUN: %empty-directory(%t)
import _Differentiation

// SR-12893
// XFAIL: freebsd

// RUN: %target-build-swift -Xfrontend -enable-anonymous-context-mangled-names %S/Inputs/AutoDiffTypes.swift -parse-as-library -emit-module -emit-library -module-name TypesToReflect -o %t/%target-library-name(TypesToReflect)
// RUN: %target-build-swift -Xfrontend -enable-anonymous-context-mangled-names %S/Inputs/AutoDiffTypes.swift %S/Inputs/main.swift -emit-module -emit-executable -module-name TypesToReflect -o %t/TypesToReflect

// RUN: %target-swift-reflection-dump -binary-filename %t/%target-library-name(TypesToReflect) | %FileCheck %s
// RUN: %target-swift-reflection-dump -binary-filename %t/TypesToReflect | %FileCheck %s

// CHECK: FIELDS:
// CHECK: =======
// CHECK: TypesToReflect.HasAutoDiffTypes
// CHECK: aFunction: @differentiable(reverse) (Swift.Float) -> Swift.Float
// CHECK: (function differentiable=reverse

