// RUN: %target-swift-emit-sil %s -target %target-cpu-apple-macosx10.50 -verify
// RUN: %target-swift-emit-silgen %s -target %target-cpu-apple-macosx10.50 | %FileCheck %s

// REQUIRES: OS=macosx

// CHECK-LABEL: sil{{.+}}@main{{.*}} {

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 53
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 8
// CHECK: [[FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK-NOT: {{.*}}integer_literal $Builtin.Int1, -1
// CHECK-NOT: builtin "xor_Int1"{{.*}}
if #available(OSX 10.53.8, iOS 7.1, *) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 53
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 8
// CHECK: [[FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[MINUSONE:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: [[QUERY_INVERSION:%.*]] = builtin "xor_Int1"([[QUERY_RESULT]] : $Builtin.Int1, [[MINUSONE]] : $Builtin.Int1) : $Builtin.Int1
if #unavailable(OSX 10.53.8, iOS 7.1) {
}

// CHECK: [[TRUE:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_br [[TRUE]]
// Since we are compiling for an unmentioned platform (OS X), we check against the minimum
// deployment target, which is 10.50
if #available(iOS 7.1, *) {
}

// CHECK: [[FALSE:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK: cond_br [[FALSE]]
// Since we are compiling for an unmentioned platform (OS X), we check against the minimum
// deployment target, which is 10.50
if #unavailable(iOS 7.1) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 52
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK-NOT: {{.*}}integer_literal $Builtin.Int1, -1
// CHECK-NOT: builtin "xor_Int1"{{.*}}
if #available(OSX 10.52, *) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 52
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[MINUSONE:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: [[QUERY_INVERSION:%.*]] = builtin "xor_Int1"([[QUERY_RESULT]] : $Builtin.Int1, [[MINUSONE]] : $Builtin.Int1) : $Builtin.Int1
if #unavailable(OSX 10.52) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 52
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK-NOT: {{.*}}integer_literal $Builtin.Int1, -1
// CHECK-NOT: builtin "xor_Int1"{{.*}}
if #available(macOS 10.52, *) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 52
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[MINUSONE:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: [[QUERY_INVERSION:%.*]] = builtin "xor_Int1"([[QUERY_RESULT]] : $Builtin.Int1, [[MINUSONE]] : $Builtin.Int1) : $Builtin.Int1
if #unavailable(macOS 10.52) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK-NOT: {{.*}}integer_literal $Builtin.Int1, -1
// CHECK-NOT: builtin "xor_Int1"{{.*}}
if #available(OSX 10, *) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@convention(thin) (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[MINUSONE:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: [[QUERY_INVERSION:%.*]] = builtin "xor_Int1"([[QUERY_RESULT]] : $Builtin.Int1, [[MINUSONE]] : $Builtin.Int1) : $Builtin.Int1
if #unavailable(OSX 10) {
}

// CHECK: }

func doThing() {}

func testUnreachableVersionAvailable(condition: Bool) {
  if #available(OSX 10.0, *) {
    doThing() // no-warning
    return
  } else {
    doThing() // no-warning
  }

  if #unavailable(OSX 10.0) {
    doThing() // no-warning
  } else {
    doThing() // no-warning
    return
  }

  if true {
    doThing() // no-warning
  }
  if false { // expected-note {{condition always evaluates to false}}
    doThing() // expected-warning {{will never be executed}}
  }
}

func testUnreachablePlatformAvailable(condition: Bool) {
  if #available(iOS 7.1, *) {
    doThing() // no-warning
    return
  } else {
    doThing() // no-warning
  }

  if #unavailable(iOS 7.1) {
    doThing() // no-warning
  } else {
    doThing() // no-warning
    return
  }

  if true {
    doThing() // no-warning
  }
  if false { // expected-note {{condition always evaluates to false}}
    doThing() // expected-warning {{will never be executed}}
  }
}

func testUnreachablePlatformAvailableGuard() {
  guard #available(iOS 7.1, *) else {
    doThing() // no-warning
    return
  }

  guard #unavailable(iOS 7.1) else {
    doThing() // no-warning
    return
  }

  doThing() // no-warning
}
