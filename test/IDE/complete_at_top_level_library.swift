// RUN: %empty-directory(%t.ccp)

// Repeat twice to ensure completion caches works correctly. i.e. no sticky flairs
// RUN: %target-swift-ide-test -code-completion -source-filename %s -completion-cache-path=%t.ccp -code-completion-token=TOPLEVEL -parse-as-library | %FileCheck %s -check-prefix=LIBRARY
// RUN: %target-swift-ide-test -code-completion -source-filename %s -completion-cache-path=%t.ccp -code-completion-token=TOPLEVEL | %FileCheck %s -check-prefix=SCRIPT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -completion-cache-path=%t.ccp -code-completion-token=TOPLEVEL -parse-as-library | %FileCheck %s -check-prefix=LIBRARY
// RUN: %target-swift-ide-test -code-completion -source-filename %s -completion-cache-path=%t.ccp -code-completion-token=TOPLEVEL | %FileCheck %s -check-prefix=SCRIPT

struct MyStruct {}
protocol MyProtocol {}

#^TOPLEVEL^#

// LIBRARY-DAG: Keyword[associatedtype]/None:       associatedtype; name=associatedtype
// LIBRARY-DAG: Keyword[class]/None/Flair[CommonKeyword]: class; name=class
// LIBRARY-DAG: Keyword[deinit]/None:               deinit; name=deinit
// LIBRARY-DAG: Keyword[enum]/None/Flair[CommonKeyword]: enum; name=enum
// LIBRARY-DAG: Keyword[extension]/None/Flair[CommonKeyword]: extension; name=extension
// LIBRARY-DAG: Keyword[func]/None:                 func; name=func
// LIBRARY-DAG: Keyword[import]/None:               import; name=import
// LIBRARY-DAG: Keyword[init]/None:                 init; name=init
// LIBRARY-DAG: Keyword[operator]/None:             operator; name=operator
// LIBRARY-DAG: Keyword[precedencegroup]/None:      precedencegroup; name=precedencegroup
// LIBRARY-DAG: Keyword[protocol]/None/Flair[CommonKeyword]: protocol; name=protocol
// LIBRARY-DAG: Keyword[struct]/None/Flair[CommonKeyword]: struct; name=struct
// LIBRARY-DAG: Keyword[subscript]/None:            subscript; name=subscript
// LIBRARY-DAG: Keyword[typealias]/None:            typealias; name=typealias
// LIBRARY-DAG: Keyword[fileprivate]/None:          fileprivate; name=fileprivate
// LIBRARY-DAG: Keyword[internal]/None:             internal; name=internal
// LIBRARY-DAG: Keyword[private]/None:              private; name=private
// LIBRARY-DAG: Keyword[public]/None:               public; name=public
// LIBRARY-DAG: Keyword[static]/None:               static; name=static
// LIBRARY-DAG: Keyword/None:                       final; name=final
// LIBRARY-DAG: Keyword/None:                       required; name=required
// LIBRARY-DAG: Keyword/None:                       optional; name=optional
// LIBRARY-DAG: Keyword/None:                       lazy; name=lazy
// LIBRARY-DAG: Keyword/None:                       dynamic; name=dynamic
// LIBRARY-DAG: Keyword/None:                       infix; name=infix
// LIBRARY-DAG: Keyword/None:                       prefix; name=prefix
// LIBRARY-DAG: Keyword/None:                       postfix; name=postfix
// LIBRARY-DAG: Keyword/None:                       mutating; name=mutating
// LIBRARY-DAG: Keyword/None:                       nonmutating; name=nonmutating
// LIBRARY-DAG: Keyword/None:                       convenience; name=convenience
// LIBRARY-DAG: Keyword/None:                       override; name=override
// LIBRARY-DAG: Keyword/None:                       open; name=open
// LIBRARY-DAG: Keyword/None:                       weak; name=weak
// LIBRARY-DAG: Keyword/None:                       unowned; name=unowned
// LIBRARY-DAG: Keyword/None:                       indirect; name=indirect
// LIBRARY-DAG: Keyword/None:                       nonisolated; name=nonisolated
// LIBRARY-DAG: Keyword[defer]/None/Flair[ExprAtFileScope]: defer; name=defer
// LIBRARY-DAG: Keyword[if]/None/Flair[ExprAtFileScope]: if; name=if
// LIBRARY-DAG: Keyword[guard]/None/Flair[ExprAtFileScope]: guard; name=guard
// LIBRARY-DAG: Keyword[do]/None/Flair[ExprAtFileScope]: do; name=do
// LIBRARY-DAG: Keyword[repeat]/None/Flair[ExprAtFileScope]: repeat; name=repeat
// LIBRARY-DAG: Keyword[else]/None/Flair[ExprAtFileScope]: else; name=else
// LIBRARY-DAG: Keyword[for]/None/Flair[ExprAtFileScope]: for; name=for
// LIBRARY-DAG: Keyword[while]/None/Flair[ExprAtFileScope]: while; name=while
// LIBRARY-DAG: Keyword[break]/None/Flair[ExprAtFileScope]: break; name=break
// LIBRARY-DAG: Keyword[continue]/None/Flair[ExprAtFileScope]: continue; name=continue
// LIBRARY-DAG: Keyword[fallthrough]/None/Flair[ExprAtFileScope]: fallthrough; name=fallthrough
// LIBRARY-DAG: Keyword[switch]/None/Flair[ExprAtFileScope]: switch; name=switch
// LIBRARY-DAG: Keyword[let]/None:                  let; name=let
// LIBRARY-DAG: Keyword[var]/None:                  var; name=var
// LIBRARY-DAG: Keyword[try]/None/Flair[ExprAtFileScope]: try; name=try
// LIBRARY-DAG: Keyword[try]/None/Flair[ExprAtFileScope]: try!; name=try!
// LIBRARY-DAG: Keyword[try]/None/Flair[ExprAtFileScope]: try?; name=try?
// LIBRARY-DAG: Keyword/None/Flair[ExprAtFileScope]: await; name=await
// LIBRARY-DAG: Literal[Integer]/None/Flair[ExprAtFileScope]: 0[#Int#]; name=0
// LIBRARY-DAG: Literal[Boolean]/None/Flair[ExprAtFileScope]: true[#Bool#]; name=true
// LIBRARY-DAG: Literal[Boolean]/None/Flair[ExprAtFileScope]: false[#Bool#]; name=false
// LIBRARY-DAG: Literal[Nil]/None/Flair[ExprAtFileScope]: nil; name=nil
// LIBRARY-DAG: Literal[String]/None/Flair[ExprAtFileScope]: "{#(abc)#}"[#String#]; name=""
// LIBRARY-DAG: Literal[Array]/None/Flair[ExprAtFileScope]: [{#(values)#}][#Array#]; name=[]
// LIBRARY-DAG: Literal[Dictionary]/None/Flair[ExprAtFileScope]: [{#(key)#}: {#(value)#}][#Dictionary#]; name=[: ]
// LIBRARY-DAG: Literal[Tuple]/None/Flair[ExprAtFileScope]: ({#(values)#}); name=()
// LIBRARY-DAG: Decl[Struct]/CurrModule/Flair[ExprAtFileScope]: MyStruct[#MyStruct#]; name=MyStruct
// LIBRARY-DAG: Decl[Protocol]/CurrModule/Flair[RareType,ExprAtFileScope]: MyProtocol[#MyProtocol#]; name=MyProtocol
// LIBRARY-DAG: Decl[Struct]/OtherModule[Swift]/Flair[ExprAtFileScope]/IsSystem: Int[#Int#]; name=Int

// SCRIPT-DAG: Keyword[associatedtype]/None:       associatedtype; name=associatedtype
// SCRIPT-DAG: Keyword[class]/None:                class; name=class
// SCRIPT-DAG: Keyword[deinit]/None:               deinit; name=deinit
// SCRIPT-DAG: Keyword[enum]/None:                 enum; name=enum
// SCRIPT-DAG: Keyword[extension]/None:            extension; name=extension
// SCRIPT-DAG: Keyword[func]/None:                 func; name=func
// SCRIPT-DAG: Keyword[import]/None:               import; name=import
// SCRIPT-DAG: Keyword[init]/None:                 init; name=init
// SCRIPT-DAG: Keyword[operator]/None:             operator; name=operator
// SCRIPT-DAG: Keyword[precedencegroup]/None:      precedencegroup; name=precedencegroup
// SCRIPT-DAG: Keyword[protocol]/None:             protocol; name=protocol
// SCRIPT-DAG: Keyword[struct]/None:                struct; name=struct
// SCRIPT-DAG: Keyword[subscript]/None:            subscript; name=subscript
// SCRIPT-DAG: Keyword[typealias]/None:            typealias; name=typealias
// SCRIPT-DAG: Keyword[fileprivate]/None:          fileprivate; name=fileprivate
// SCRIPT-DAG: Keyword[internal]/None:             internal; name=internal
// SCRIPT-DAG: Keyword[private]/None:              private; name=private
// SCRIPT-DAG: Keyword[public]/None:               public; name=public
// SCRIPT-DAG: Keyword[static]/None:               static; name=static
// SCRIPT-DAG: Keyword/None:                       final; name=final
// SCRIPT-DAG: Keyword/None:                       required; name=required
// SCRIPT-DAG: Keyword/None:                       optional; name=optional
// SCRIPT-DAG: Keyword/None:                       lazy; name=lazy
// SCRIPT-DAG: Keyword/None:                       dynamic; name=dynamic
// SCRIPT-DAG: Keyword/None:                       infix; name=infix
// SCRIPT-DAG: Keyword/None:                       prefix; name=prefix
// SCRIPT-DAG: Keyword/None:                       postfix; name=postfix
// SCRIPT-DAG: Keyword/None:                       mutating; name=mutating
// SCRIPT-DAG: Keyword/None:                       nonmutating; name=nonmutating
// SCRIPT-DAG: Keyword/None:                       convenience; name=convenience
// SCRIPT-DAG: Keyword/None:                       override; name=override
// SCRIPT-DAG: Keyword/None:                       open; name=open
// SCRIPT-DAG: Keyword/None:                       weak; name=weak
// SCRIPT-DAG: Keyword/None:                       unowned; name=unowned
// SCRIPT-DAG: Keyword/None:                       indirect; name=indirect
// SCRIPT-DAG: Keyword/None:                       nonisolated; name=nonisolated
// SCRIPT-DAG: Keyword[defer]/None:                defer; name=defer
// SCRIPT-DAG: Keyword[if]/None:                   if; name=if
// SCRIPT-DAG: Keyword[guard]/None:                guard; name=guard
// SCRIPT-DAG: Keyword[do]/None:                   do; name=do
// SCRIPT-DAG: Keyword[repeat]/None:               repeat; name=repeat
// SCRIPT-DAG: Keyword[else]/None:                 else; name=else
// SCRIPT-DAG: Keyword[for]/None:                  for; name=for
// SCRIPT-DAG: Keyword[while]/None:                while; name=while
// SCRIPT-DAG: Keyword[break]/None:                break; name=break
// SCRIPT-DAG: Keyword[continue]/None:             continue; name=continue
// SCRIPT-DAG: Keyword[fallthrough]/None:          fallthrough; name=fallthrough
// SCRIPT-DAG: Keyword[switch]/None:               switch; name=switch
// SCRIPT-DAG: Keyword[let]/None:                  let; name=let
// SCRIPT-DAG: Keyword[var]/None:                  var; name=var
// SCRIPT-DAG: Keyword[try]/None:                  try; name=try
// SCRIPT-DAG: Keyword[try]/None:                  try!; name=try!
// SCRIPT-DAG: Keyword[try]/None:                  try?; name=try?
// SCRIPT-DAG: Keyword/None:                       await; name=await
// SCRIPT-DAG: Literal[Integer]/None: 0[#Int#]; name=0
// SCRIPT-DAG: Literal[Boolean]/None: true[#Bool#]; name=true
// SCRIPT-DAG: Literal[Boolean]/None: false[#Bool#]; name=false
// SCRIPT-DAG: Literal[Nil]/None: nil; name=nil
// SCRIPT-DAG: Literal[String]/None: "{#(abc)#}"[#String#]; name=""
// SCRIPT-DAG: Literal[Array]/None: [{#(values)#}][#Array#]; name=[]
// SCRIPT-DAG: Literal[Dictionary]/None: [{#(key)#}: {#(value)#}][#Dictionary#]; name=[: ]
// SCRIPT-DAG: Literal[Tuple]/None: ({#(values)#}); name=()
// SCRIPT-DAG: Decl[Struct]/CurrModule: MyStruct[#MyStruct#]; name=MyStruct
// SCRIPT-DAG: Decl[Protocol]/CurrModule/Flair[RareType]: MyProtocol[#MyProtocol#]; name=MyProtocol
// SCRIPT-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: Int[#Int#]; name=Int
