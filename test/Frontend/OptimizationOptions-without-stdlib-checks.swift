// RUN: %target-swift-frontend -module-name OptimizationOptions -Onone -emit-sil -primary-file %s -o - | %FileCheck %s --check-prefix=DEBUG
// RUN: %target-swift-frontend -module-name OptimizationOptions -O -emit-sil -primary-file %s -o - | %FileCheck %s --check-prefix=RELEASE
// RUN: %target-swift-frontend -module-name OptimizationOptions -Ounchecked -emit-sil -primary-file %s -o - | %FileCheck %s --check-prefix=UNCHECKED
// RUN: %target-swift-frontend -module-name OptimizationOptions -Oplayground -emit-sil -primary-file %s -o - | %FileCheck %s --check-prefix=PLAYGROUND

// REQUIRES: optimized_stdlib
// REQUIRES: swift_stdlib_no_asserts

func test_assert(x: Int, y: Int) -> Int {
  assert(x >= y , "x smaller than y")
  return x + y
}

func test_fatal(x: Int, y: Int) -> Int {
  if x > y {
    return x + y
  }
  preconditionFailure("Human nature ...")
}

func testprecondition_check(x: Int, y: Int) -> Int {
  precondition(x > y, "Test precondition check")
  return x + y
}

func test_partial_safety_check(x: Int, y: Int) -> Int {
  assert(x > y, "Test partial safety check")
  return x + y
}

// In debug mode keep user asserts and runtime checks.
// DEBUG-LABEL: sil hidden @$s19OptimizationOptions11test_assert1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// DEBUG: "x smaller than y"
// DEBUG: "Assertion failed"
// DEBUG: cond_fail
// DEBUG: return

// In playground mode keep user asserts and runtime checks.
// PLAYGROUND-LABEL: sil hidden @$s19OptimizationOptions11test_assert1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// PLAYGROUND: "x smaller than y"
// PLAYGROUND: "Assertion failed"
// PLAYGROUND: cond_fail

// In release mode remove user asserts and keep runtime checks.
// RELEASE-LABEL: sil hidden @$s19OptimizationOptions11test_assert1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// RELEASE-NOT: "x smaller than y"
// RELEASE-NOT: "Assertion failed"
// RELEASE: cond_fail

// In fast mode remove user asserts and runtime checks.
// FAST-LABEL: sil hidden @$s19OptimizationOptions11test_assert1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// FAST-NOT: "x smaller than y"
// FAST-NOT: "Assertion failed"
// FAST-NOT: cond_fail


// In debug mode keep verbose fatal errors.
// DEBUG-LABEL: sil hidden @$s19OptimizationOptions10test_fatal1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// DEBUG-DAG: "Human nature ..."
// DEBUG-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC:.*assertionFailure.*]] : $@convention(thin)
// DEBUG: apply %[[FATAL_ERROR]]({{.*}})
// DEBUG: unreachable

// In playground mode keep verbose fatal errors.
// PLAYGROUND-LABEL: sil hidden @$s19OptimizationOptions10test_fatal1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// PLAYGROUND-DAG: "Human nature ..."
// PLAYGROUND-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC:.*assertionFailure.*]] : $@convention(thin)
// PLAYGROUND: apply %[[FATAL_ERROR]]({{.*}})
// PLAYGROUND: unreachable

// In release mode keep succinct fatal errors (trap).
// RELEASE-LABEL: sil hidden @$s19OptimizationOptions10test_fatal1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// RELEASE-NOT: "Human nature ..."
// RELEASE-NOT: "Fatal error"
// RELEASE: cond_fail
// RELEASE: return

// In fast mode remove fatal errors.
// FAST-LABEL: sil hidden @$s19OptimizationOptions10test_fatal1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// FAST-NOT: "Human nature ..."
// FAST-NOT: "Fatal error"
// FAST-NOT: int_trap

// Precondition safety checks.

// In debug mode keep verbose library precondition checks.
// DEBUG-DAG: "Fatal error"
// DEBUG-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC]]
// DEBUG: apply %[[FATAL_ERROR]]({{.*}})
// DEBUG: unreachable
// DEBUG: return

// In playground mode keep verbose library precondition checks.
// PLAYGROUND-LABEL: sil hidden @$s19OptimizationOptions22testprecondition_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// PLAYGROUND-DAG: "Precondition failed"
// PLAYGROUND-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC]]
// PLAYGROUND: apply %[[FATAL_ERROR]]({{.*}})
// PLAYGROUND: unreachable
// PLAYGROUND: return

// In release mode keep succinct library precondition checks (trap).
// RELEASE-LABEL: sil hidden @$s19OptimizationOptions22testprecondition_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// RELEASE-DAG: "Precondition failed"
// RELEASE-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC:.*assertionFailure.*]]
// RELEASE: apply %[[FATAL_ERROR]]({{.*}})
// RELEASE: unreachable

// In unchecked mode remove library precondition checks.
// UNCHECKED-LABEL: sil hidden @$s19OptimizationOptions22testprecondition_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// UNCHECKED-NOT:  "Fatal error"
// UNCHECKED-NOT:  builtin "int_trap"
// UNCHECKED-NOT:  unreachable
// UNCHECKED:  return

//  Partial safety checks.

// In debug mode keep verbose partial safety checks.
// DEBUG-LABEL: sil hidden @$s19OptimizationOptions25test_partial_safety_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// DEBUG-DAG: "Assertion failed"
// DEBUG-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC]]
// DEBUG: apply %[[FATAL_ERROR]]({{.*}})
// DEBUG: unreachable

// In playground mode keep verbose partial safety checks.
// PLAYGROUND-LABEL: sil hidden @$s19OptimizationOptions25test_partial_safety_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// PLAYGROUND-DAG: "Assertion failed"
// PLAYGROUND-DAG: %[[FATAL_ERROR:.+]] = function_ref @[[FATAL_ERROR_FUNC]]
// PLAYGROUND: apply %[[FATAL_ERROR]]({{.*}})
// PLAYGROUND: unreachable

// In release mode remove partial safety checks.
// RELEASE-LABEL: sil hidden @$s19OptimizationOptions25test_partial_safety_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// RELEASE-NOT:  "Fatal error"
// RELEASE-NOT:  builtin "int_trap"
// RELEASE-NOT:  unreachable
// RELEASE: return

// In fast mode remove partial safety checks.
// FAST-LABEL: sil hidden @$s19OptimizationOptions25test_partial_safety_check1x1yS2i_SitF : $@convention(thin) (Int, Int) -> Int {
// FAST-NOT:  "Fatal error"
// FAST-NOT:  builtin "int_trap"
// FAST-NOT:  unreachable
// FAST: return
