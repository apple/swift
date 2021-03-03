// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-silgen -primary-file %S/Inputs/constant_evaluable.swift -o %t/constant_evaluable_subset_test_silgen.sil
//
// Run the (mandatory) passes on which constant evaluator depends, and test the
// constant evaluator on the SIL produced after the dependent passes are run.
//
// RUN: not %target-sil-opt -silgen-cleanup -raw-sil-inst-lowering -allocbox-to-stack -mandatory-inlining -constexpr-limit 3000 -test-constant-evaluable-subset %t/constant_evaluable_subset_test_silgen.sil > %t/constant_evaluable_subset_test.sil 2> %t/error-output
//
// RUN: %FileCheck %S/Inputs/constant_evaluable.swift < %t/error-output
//
// Test the constant evaluator on the output of the mandatory pipeline. This is
// to test that constant evaluability is not affected by mandatory
// optimizations. Note that it can be affected by non-mandatory optimizations,
// especially performance inlining as it inlines functions such as String.+=
// that the evaluator has special knowledge about.
//
// RUN: not %target-sil-opt -silgen-cleanup -diagnose-invalid-escaping-captures -diagnose-static-exclusivity -capture-promotion -access-enforcement-selection -allocbox-to-stack -noreturn-folding -definite-init -raw-sil-inst-lowering -closure-lifetime-fixup -semantic-arc-opts -destroy-hoisting -ownership-model-eliminator -mandatory-inlining -predictable-memaccess-opts -os-log-optimization -diagnostic-constant-propagation -predictable-deadalloc-elim -mandatory-arc-opts -diagnose-unreachable -diagnose-infinite-recursion -yield-once-check -dataflow-diagnostics -split-non-cond_br-critical-edges -constexpr-limit 3000 -test-constant-evaluable-subset %t/constant_evaluable_subset_test_silgen.sil > /dev/null 2> %t/error-output-mandatory
//
// RUN: %FileCheck %S/Inputs/constant_evaluable.swift < %t/error-output-mandatory
