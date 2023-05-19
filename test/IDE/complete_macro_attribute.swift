// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t/output


@freestanding(#^FREESTANDING_ROLE^#)
macro FreestandingMacro

// FREESTANDING_ROLE: Begin completions, 2 items
// FREESTANDING_ROLE-DAG: Keyword/None:                       expression; name=expression
// FREESTANDING_ROLE-DAG: Keyword/None:                       declaration; name=declaration

@attached(#^ATTACHED_ROLE^#)
macro AttachedMacro

// ATTACHED_ROLE: Begin completions, 5 items
// ATTACHED_ROLE-DAG: Keyword/None:                       accessor; name=accessor
// ATTACHED_ROLE-DAG: Keyword/None:                       memberAttribute; name=memberAttribute
// ATTACHED_ROLE-DAG: Keyword/None:                       member; name=member
// ATTACHED_ROLE-DAG: Keyword/None:                       peer; name=peer
// ATTACHED_ROLE-DAG: Keyword/None:                       conformance; name=conformance

@freestanding(declaration, #^NAMES_POSITION^#)
macro FreestandingDeclarationMacro

// NAMES_POSITION: Begin completions, 1 item
// NAMES_POSITION-DAG: Keyword/None:                       names: [#Specify declared names#]; name=names

@attached(member, names: #^NAMES_ARGUMENT^#)

// NAMES_ARGUMENT: Begin completions, 5 items
// NAMES_ARGUMENT-DAG: Keyword/None:                       named({#(name)#}); name=named()
// NAMES_ARGUMENT-DAG: Keyword/None:                       overloaded; name=overloaded
// NAMES_ARGUMENT-DAG: Keyword/None:                       prefixed({#(name)#}); name=prefixed()
// NAMES_ARGUMENT-DAG: Keyword/None:                       suffixed({#(name)#}); name=suffixed()
// NAMES_ARGUMENT-DAG: Keyword/None:                       arbitrary; name=arbitrary

