# Provides auto-completion for swift tools options and for ninja build targets.
#
# Include this bash source code with the source command, e.g. in your bash ~/.profile file:
#    source swift-autocomplete.bash

_swift_complete()
{
  local tool currentWord prevWord

  tool="${COMP_WORDS[0]}"
  currentWord="${COMP_WORDS[COMP_CWORD]}"
  prevWord="${COMP_WORDS[COMP_CWORD-1]}"

  if [[ ${currentWord} != -* ]] ; then
    COMPREPLY=()
    return 0
  fi

  if [[ ${prevWord} == "-Xllvm" ]] ; then
    # Don't know how to get the help for llvm options automatically.
    # So we use a grep'ed static list.
    COMPREPLY=( $(compgen -W "\
      -aa-kind \
      -allocbox-to-stack-analyze-apply \
      -allow-critical-edges \
      -basic-dynamic-replacement \
      -bug-reducer-tester-failure-kind \
      -bug-reducer-tester-target-func \
      -canonical-ossa-rewrite-borrows \
      -closure-specialize-eliminate-dead-closures \
      -cmo-function-size-limit \
      -constexpr-limit \
      -copy-forward-start \
      -copy-forward-stop \
      -differentiation-skip-folding-differentiable-function-extraction \
      -disable-arc-cm \
      -disable-llvm-arc-opts \
      -disable-sil-cm-rr-cm \
      -disable-sil-ownership-verification \
      -dont-abort-on-memory-lifetime-errors \
      -enable-abc-hoisting \
      -enable-abcopts \
      -enable-accessed-storage-dump-uses \
      -enable-copyforwarding \
      -enable-destroyhoisting \
      -enable-eager-specializer \
      -enable-existential-specializer \
      -enable-expand-all \
      -enable-experimental-linear-map-transposition \
      -enable-loop-arc \
      -enable-rc-identity-arg-strip \
      -enable-sil-passmanager-verifier-analysis \
      -enable-trap-debug-info \
      -escapes-enable-graphwriter \
      -escapes-internal-verify \
      -inline-tree-no-demangle \
      -keep-will-throw-call \
      -looprotate-single-block-loop \
      -looprotate-size-limit \
      -max-local-apply-recur-depth \
      -max-partial-store-count \
      -optimize-opaque-address-lowering \
      -optremarkgen-declless-debugvalue-use-sildebugvar-info \
      -optremarkgen-visit-implicit-autogen-funcs \
      -print-shortest-path-info \
      -print-swift-mangling-stats \
      -sil-abcopts-report \
      -sil-aggressive-inline \
      -sil-assert-on-exclusivity-failure \
      -sil-break-on-function \
      -sil-break-on-pass \
      -sil-closure-lifetime-fixup-reverse-phi-order \
      -sil-combine-disable-alloc-stack-opts \
      -sil-cross-module-serialize-all \
      -sil-di-assert-on-failure \
      -sil-disable-convert-escape-to-noescape-switch-peephole \
      -sil-disable-skipping-passes \
      -sil-disable-typelowering-constantinfo-cache \
      -sil-dump-before-ome-to-path \
      -sil-dump-functions-before-outliner \
      -sil-fso-disable-arg-explosion \
      -sil-fso-disable-dead-argument \
      -sil-fso-disable-owned-to-guaranteed \
      -sil-fso-enable-generics \
      -sil-fso-optimize-if-not-called \
      -sil-full-demangle \
      -sil-generic-verify-after-specialization \
      -sil-inline-generics \
      -sil-inline-verify-after-inline \
      -sil-loop-region-view-cfg-only-function \
      -sil-loop-region-view-cfg-only-functions \
      -sil-lower-agg-instrs-expand-all \
      -sil-merge-stack-slots \
      -sil-opt-pass-count \
      -sil-opt-remark-ignore-always-infer \
      -sil-optimized-access-markers \
      -sil-ownership-verifier-enable-testing \
      -sil-partial-specialization \
      -sil-partial-specialization-with-generic-substitutions \
      -sil-print-all \
      -sil-print-debuginfo \
      -sil-print-function \
      -sil-print-functions \
      -sil-print-generic-specialization-info \
      -sil-print-generic-specialization-loops \
      -sil-print-no-color \
      -sil-print-on-error \
      -sil-print-pass-name \
      -sil-print-pass-time \
      -sil-print-sourceinfo \
      -sil-semantic-arc-opts-verify-after-transform \
      -sil-stats-block-count-delta-threshold \
      -sil-stats-dump-all \
      -sil-stats-func-block-count-delta-threshold \
      -sil-stats-func-block-count-min-threshold \
      -sil-stats-func-inst-count-delta-threshold \
      -sil-stats-func-inst-count-min-threshold \
      -sil-stats-function-count-delta-threshold \
      -sil-stats-functions \
      -sil-stats-inst-count-delta-threshold \
      -sil-stats-modules \
      -sil-stats-only-function \
      -sil-stats-only-functions \
      -sil-stats-output-file \
      -sil-stats-used-memory-delta-threshold \
      -sil-stats-used-memory-min-threshold \
      -sil-verify-force-analysis \
      -sil-verify-without-invalidation \
      -sil-view-cfg \
      -sil-view-cfg-only-function \
      -sil-view-cfg-only-functions \
      -sil-view-guaranteed-cfg \
      -sil-view-silgen-cfg \
      -silcombine-owned-code-sinking \
      -simplify-cfg-simplify-unconditional-branches \
      -sroa-args-remove-dead-args-after \
      -swift-diagnostics-assert-on-error \
      -swift-diagnostics-assert-on-warning \
      -verify-abort-on-failure \
      -verify-arc-loop-summary \
      -verify-continue-on-failure \
      -verify-di-holes \
      -verify-dump-module-on-failure \
      -verify-linetable \
      -verify-skip-convert-escape-to-noescape-attributes \
      -view-cfg-long-line-behavior \
      -view-cfg-max-columns \
      -view-cfg-only-for-function \
      -view-cfg-remove-use-list-comments \
      -view-loop-regions-long-line-behavior \
      -view-loop-regions-max-columns \
      -view-loop-regions-only-for-function \
      -view-loop-regions-remove-use-list-comments \
      -debug-only \
      " -- ${currentWord}) )
  else
    frontendOption=""
    if [[ ${prevWord} == "-Xfrontend" ]] || echo ${COMP_WORDS[@]} | grep --quiet -e '-frontend' ; then
      frontendOption="-frontend"
    fi
    toolOptions=`${tool} ${frontendOption} -help-hidden 2>/dev/null | grep '^  *-[a-zA-Z-]' | sed -E 's/^  *(-[a-zA-Z=-]+).*/\1/'`
    COMPREPLY=( $(compgen -W "${toolOptions}" -- ${currentWord}) )
  fi
  return 0
}

_ninja_complete()
{
  local currentWord prevWord

  tool="${COMP_WORDS[0]}"
  currentWord="${COMP_WORDS[COMP_CWORD]}"
  prevWord="${COMP_WORDS[COMP_CWORD-1]}"

  if [[ ${prevWord} == -[Cfjlkdt] ]] ; then
    COMPREPLY=( $(compgen -f ${currentWord}) )
    return 0
  fi

  targets=`${tool} -t targets 2>/dev/null | grep  -v -e '\.' -e 'swift[A-Z]' -e 'cmake_order' | sed s/:.*//`
  COMPREPLY=( $(compgen -W "${targets}" -- ${currentWord}) )
  return 0
}

complete -o default -F _swift_complete swiftc
complete -o default -F _swift_complete swift
complete -o default -F _swift_complete swift-frontend
complete -o default -F _swift_complete sil-opt
complete -o default -F _swift_complete sil-func-extractor
complete -o default -F _swift_complete swift-demangle
complete -o default -F _swift_complete swift-llvm-opt
complete -o default -F _swift_complete swift-ide-test
complete -o default -F _swift_complete swift-ios-test
complete -o default -F _swift_complete swift-sdk-analyzer
complete -o default -F _swift_complete swift-stdlib-tool
complete -o default -F _swift_complete lldb-moduleimport-test
complete -o default -F _ninja_complete ninja

