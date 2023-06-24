// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_NOMINAL_TOP | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_NOMINAL_IN_IF | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_NOMINAL_IN_ELSEIF | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_NOMINAL_IN_ELSE | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_GLOBAL_TOP | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_GLOBAL_IN_IF | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_GLOBAL_IN_ELSEIF | %FileCheck %s -check-prefix=POUND_DIRECTIVE
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=POUND_GLOBAL_IN_ELSE | %FileCheck %s -check-prefix=POUND_DIRECTIVE

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_NOMINAL_1 | %FileCheck %s -check-prefix=CONDITION -check-prefix=NOFLAG
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_NOMINAL_1 -D FOO -D BAR | %FileCheck %s -check-prefix=CONDITION -check-prefix=WITHFLAG
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_NOMINAL_2 | %FileCheck %s -check-prefix=CONDITION -check-prefix=NOFLAG
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_GLOBAL_1 | %FileCheck %s -check-prefix=CONDITION -check-prefix=NOFLAG
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_GLOBAL_2 | %FileCheck %s -check-prefix=CONDITION -check-prefix=NOFLAG
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-sourcetext -code-completion-token=CONDITION_GLOBAL_2 -D FOO -D BAR | %FileCheck %s -check-prefix=CONDITION -check-prefix=WITHFLAG

// POUND_DIRECTIVE-DAG: Keyword[#sourceLocation]/None:      sourceLocation(file: {#String#}, line: {#Int#}); name=sourceLocation(file:line:); sourcetext=sourceLocation(file: <#T##String#>, line: <#T##Int#>)
// POUND_DIRECTIVE-DAG: Keyword[#if]/None:                  if {#(condition)#}; name=if ; sourcetext=if <#T##condition#>
// POUND_DIRECTIVE-DAG: Keyword[#elseif]/None:              elseif {#(condition)#}; name=elseif ; sourcetext=elseif <#T##condition#>
// POUND_DIRECTIVE-DAG: Keyword[#else]/None:                else; name=else; sourcetext=else
// POUND_DIRECTIVE-DAG: Keyword[#endif]/None:               endif; name=endif; sourcetext=endif
// TODO: These currently do not match between when macros are enabled and when
// they aren't. Update to macros when
// POUND_DIRECTIVE-DAG: name=warning
// POUND_DIRECTIVE-DAG: name=error

class C {
##^POUND_NOMINAL_TOP^#

#if true
  ##^POUND_NOMINAL_IN_IF^#
#elseif true
  ##^POUND_NOMINAL_IN_ELSEIF^#
#else
  ##^POUND_NOMINAL_IN_ELSE^#
#endif
}

##^POUND_GLOBAL_TOP^#

#if false
  ##^POUND_GLOBAL_IN_IF^#
#elseif false
  ##^POUND_GLOBAL_IN_ELSEIF^#
#else
  ##^POUND_GLOBAL_IN_ELSE^#
#endif

// CONDITION-NOT: globalVar
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               os({#(name)#}); name=os(); sourcetext=os(<#T##name#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               arch({#(name)#}); name=arch(); sourcetext=arch(<#T##name#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               canImport({#(module)#}); name=canImport(); sourcetext=canImport(<#T##module#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               targetEnvironment(simulator); name=targetEnvironment(simulator); sourcetext=targetEnvironment(simulator)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               targetEnvironment(macCatalyst); name=targetEnvironment(macCatalyst); sourcetext=targetEnvironment(macCatalyst)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               swift(>={#(version)#}); name=swift(>=); sourcetext=swift(>=<#T##version#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               swift(<{#(version)#}); name=swift(<); sourcetext=swift(<<#T##version#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               compiler(>={#(version)#}); name=compiler(>=); sourcetext=compiler(>=<#T##version#>)
// CONDITION-DAG: Pattern/CurrModule/Flair[ExprSpecific]:               compiler(<{#(version)#}); name=compiler(<); sourcetext=compiler(<<#T##version#>)
// CONDITION-DAG: Keyword[true]/None:                 true[#Bool#]; name=true; sourcetext=true
// CONDITION-DAG: Keyword[false]/None:                false[#Bool#]; name=false; sourcetext=false
// CONDITION-NOT: globalVar

// WITHFLAG: Keyword/CurrModule/Flair[ExprSpecific]:               FOO; name=FOO; sourcetext=FOO
// WITHFLAG: Keyword/CurrModule/Flair[ExprSpecific]:               BAR; name=BAR; sourcetext=BAR

// NOFLAG-NOT: FOO 
// NOFLAG-NOT: BAR

var globalVar = 1
extension C {
#if #^CONDITION_NOMINAL_1^#
#endif

#if true
#elseif false || (#^CONDITION_NOMINAL_2^#)
#endif
}

#if swift(>=1000) && #^CONDITION_GLOBAL_1^#
#endif

#if false
#elseif #^CONDITION_GLOBAL_2^#
// This '#if' is intentionally not closed with '#endif'
