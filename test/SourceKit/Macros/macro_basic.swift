@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

func testStringify(a: Int, b: Int) {
  _ = #stringify(a + b).1
}

@attached(memberAttribute)
@attached(member)
macro myTypeWrapper() = #externalMacro(module: "MacroDefinition", type: "TypeWrapperMacro")
@attached(accessor) macro accessViaStorage() = #externalMacro(module: "MacroDefinition", type: "AccessViaStorageMacro")

struct _Storage {
  var x: Int = 0 {
    willSet { print("setting \(newValue)") }
  }
  var y: Int = 0 {
    willSet { print("setting \(newValue)") }
  }
}

@myTypeWrapper
struct S {
  var x: Int
  var y: Int
}

struct S2 {
  private var _storage = _Storage()

  @accessViaStorage
  var x: Int

  @accessViaStorage
  var y: Int = 17
}

@attached(peer)
macro addCompletionHandler() = #externalMacro(module: "MacroDefinition", type: "AddCompletionHandler")

@available(macOS 10.15, *)
struct S3 {
  @addCompletionHandler
  func f(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }
}

@attached(extension, conformances: Hashable)
macro Hashable() = #externalMacro(module: "MacroDefinition", type: "HashableMacro")

@Hashable
struct S4 { }

@freestanding(declaration)
macro anonymousTypes(_: () -> String) = #externalMacro(module: "MacroDefinition", type: "DefineAnonymousTypesMacro")

#anonymousTypes { "hello" }

// This should fails to typecheck because `#assert("foo")` is expanded to `assert("foo")`, but `assert(_:)` expects 'Bool' argument
@freestanding(expression) macro assert(_: String) = #externalMacro(module: "MacroDefinition", type: "AssertMacro")
#assert("foobar")

@attached(peer, names: named(_foo))
macro AddPeerStoredProperty() = #externalMacro(module: "MacroDefinition", type: "AddPeerStoredPropertyMacro")
struct S5 {
  @AddPeerStoredProperty
  var test: Int = 10
}

@attached(preamble)
macro Traced() = #externalMacro(module: "MacroDefinition", type: "TracedPreambleMacro")

@attached(preamble, names: named(logger))
macro Logged() = #externalMacro(module: "MacroDefinition", type: "LoggerMacro")

@Traced
@Logged
func doubleTheValue(value: Int) -> Int {
  return value * 2
}

@attached(body)
macro Remote() = #externalMacro(module: "MacroDefinition", type: "RemoteBodyMacro")

@available(SwiftStdlib 5.1, *)
@Remote
func f(a: Int, b: String) async throws -> String

protocol ConjureRemoteValue {
  static func conjureValue() -> Self
}

extension String: ConjureRemoteValue {
  static func conjureValue() -> String { "" }
}

struct Logger {
  func log(entering function: String) {
    print("Logger entering \(function)")
  }

  func log(_ message: String) {
    print("--- \(message)")
  }

  func log(exiting function: String) {
    print("Logger exiting \(function)")
  }
}

func log(_ message: String) {
  print(message)
}

@available(SwiftStdlib 5.1, *)
func remoteCall<Result: ConjureRemoteValue>(function: String, arguments: [String: Any]) async throws -> Result {
  let printedArgs = arguments.keys.sorted().map { key in
    "\(key): \(arguments[key]!)"
  }.joined(separator: ", ")
  print("Remote call \(function)(\(printedArgs))")
  return Result.conjureValue()
}

// REQUIRES: swift_swift_parser, executable_test, shell, asserts

// RUN: %empty-directory(%t)

//##-- Prepare the macro plugin.
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/../../Macros/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: COMPILER_ARGS_WITHOUT_SOURCE=( \
// RUN:   -swift-version 5 \
// RUN:   -load-plugin-library %t/%target-library-name(MacroDefinition) \
// RUN:   -module-name MacroUser \
// RUN:   -enable-experimental-feature PreambleMacros \
// RUN: )

// RUN: COMPILER_ARGS=( \
// RUN:   ${COMPILER_ARGS_WITHOUT_SOURCE[@]} \
// RUN:   %s \
// RUN: )

//##-- cursor-info on macro expression
// RUN: %sourcekitd-test -req=cursor -pos=4:7 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CURSOR_MACRO_EXPR %s
// RUN: %sourcekitd-test -req=cursor -pos=4:8 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CURSOR_MACRO_EXPR %s
// CURSOR_MACRO_EXPR: source.lang.swift.ref.macro (1:33-1:42)
// CURSOR_MACRO_EXPR: (T) -> (T, String)
// CURSOR_MACRO_EXPR: <Declaration>@freestanding(expression) macro stringify&lt;T&gt;(_ value: <Type usr="s:9MacroUser1TL_xmfp">T</Type>) -&gt; (<Type usr="s:9MacroUser1TL_xmfp">T</Type>, <Type usr="s:SS">String</Type>) = #externalMacro(module: &quot;MacroDefinition&quot;, type: &quot;StringifyMacro&quot;)</Declaration>
// CURSOR_MACRO_EXPR: <decl.macro><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@freestanding</syntaxtype.attribute.name>(expression)</syntaxtype.attribute.builtin> <syntaxtype.keyword>macro</syntaxtype.keyword> <decl.name>stringify</decl.name>&lt;<decl.generic_type_param usr="s:9MacroUser1TL_xmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>&gt;(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>value</decl.var.parameter.name>: <decl.var.parameter.type><ref.generic_type_param usr="s:9MacroUser1TL_xmfp">T</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><tuple>(<tuple.element><tuple.element.type><ref.generic_type_param usr="s:9MacroUser1TL_xmfp">T</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.struct usr="s:SS">String</ref.struct></tuple.element.type></tuple.element>)</tuple></decl.function.returntype> = #externalMacro(module: &quot;MacroDefinition&quot;, type: &quot;StringifyMacro&quot;)</decl.macro>
// CURSOR_MACRO_EXPR-LABEL: SYMBOL GRAPH BEGIN
// CURSOR_MACRO_EXPR: "identifier": {
// CURSOR_MACRO_EXPR-NEXT:   "interfaceLanguage": "swift",
// CURSOR_MACRO_EXPR-NEXT:   "precise": "s:9MacroUser9stringifyyx_SStxclufm"
// CURSOR_MACRO_EXPR-NEXT: },
// CURSOR_MACRO_EXPR-NEXT: "kind": {
// CURSOR_MACRO_EXPR-NEXT:   "displayName": "Macro",
// CURSOR_MACRO_EXPR-NEXT:   "identifier": "swift.macro"
// CURSOR_MACRO_EXPR-NEXT: },
// CURSOR_MACRO_EXPR: SYMBOL GRAPH END
// CURSOR_MACRO_EXPR-LABEL: ACTIONS BEGIN
// CURSOR_MACRO_EXPR: source.refactoring.kind.rename.global
// CURSOR_MACRO_EXPR-NEXT: Global Rename
// CURSOR_MACRO_EXPR: source.refactoring.kind.inline.macro
// CURSOR_MACRO_EXPR-NEXT: Inline Macro
// CURSOR_MACRO_EXPR: ACTIONS END

//##-- Expansion on macro expression
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=4:7 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=EXPAND %s
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=4:8 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=EXPAND %s
// EXPAND: source.edit.kind.active:
// EXPAND-NEXT: 4:7-4:24 (@__swiftmacro_9MacroUser0022macro_basicswift_tiAIefMX3_6_9stringifyfMf_.swift) "(a + b, "a + b")"

//##-- cursor-info on macro declaration
// RUN: %sourcekitd-test -req=cursor -pos=57:1 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} -parse-as-library | %FileCheck -check-prefix=CURSOR_MACRO_DECL %s
// RUN: %sourcekitd-test -req=cursor -pos=57:2 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} -parse-as-library | %FileCheck -check-prefix=CURSOR_MACRO_DECL %s
// CURSOR_MACRO_DECL: source.lang.swift.ref.macro (55:7-55:21)
// CURSOR_MACRO_DECL-LABEL: SYMBOL GRAPH BEGIN
// CURSOR_MACRO_DECL: "identifier": {
// CURSOR_MACRO_DECL-NEXT:   "interfaceLanguage": "swift",
// CURSOR_MACRO_DECL-NEXT:   "precise": "s:9MacroUser14anonymousTypesyySSyXEcfm"
// CURSOR_MACRO_DECL-NEXT: },
// CURSOR_MACRO_DECL-NEXT: "kind": {
// CURSOR_MACRO_DECL-NEXT:   "displayName": "Macro",
// CURSOR_MACRO_DECL-NEXT:   "identifier": "swift.macro"
// CURSOR_MACRO_DECL-NEXT: },
// CURSOR_MACRO_DECL: SYMBOL GRAPH END
// CURSOR_MACRO_DECL-LABEL: ACTIONS BEGIN
// CURSOR_MACRO_DECL: source.refactoring.kind.inline.macro
// CURSOR_MACRO_DECL-NEXT: Inline Macro
// CURSOR_MACRO_DECL: ACTIONS END

//##-- Expansion on macro declaration
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=57:1 %s -- ${COMPILER_ARGS[@]} -parse-as-library | %FileCheck -check-prefix=EXPAND_MACRO_DECL %s
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=57:2 %s -- ${COMPILER_ARGS[@]} -parse-as-library | %FileCheck -check-prefix=EXPAND_MACRO_DECL %s
// EXPAND_MACRO_DECL: source.edit.kind.active:
// EXPAND_MACRO_DECL-NEXT: 57:1-57:28 (@__swiftmacro_9MacroUser0022macro_basicswift_tiAIefMX56_0_33_70D4178875715FB9B8B50C58F66F8D53Ll14anonymousTypesfMf_.swift) "class $s9MacroUser0022macro_basicswift_tiAIefMX56_0_33_70D4178875715FB9B8B50C58F66F8D53Ll14anonymousTypesfMf_4namefMu_ {
// EXPAND_MACRO_DECL-NEXT:   func hello() -> String {
// EXPAND_MACRO_DECL-NEXT:     "hello"
// EXPAND_MACRO_DECL-NEXT:   }
// EXPAND_MACRO_DECL-EMPTY:
// EXPAND_MACRO_DECL-NEXT:   func getSelf() -> Any.Type {
// EXPAND_MACRO_DECL-NEXT:      return Self.self
// EXPAND_MACRO_DECL-NEXT:   }
// EXPAND_MACRO_DECL-NEXT: }
// EXPAND_MACRO_DECL-NEXT: enum $s9MacroUser0022macro_basicswift_tiAIefMX56_0_33_70D4178875715FB9B8B50C58F66F8D53Ll14anonymousTypesfMf_4namefMu0_ {
// EXPAND_MACRO_DECL-NEXT:   case apple
// EXPAND_MACRO_DECL-NEXT:   case banana
// EXPAND_MACRO_DECL-EMPTY:
// EXPAND_MACRO_DECL-NEXT:   func hello() -> String {
// EXPAND_MACRO_DECL-NEXT:     "hello"
// EXPAND_MACRO_DECL-NEXT:   }
// EXPAND_MACRO_DECL-NEXT: }
// EXPAND_MACRO_DECL-NEXT: struct $s9MacroUser0022macro_basicswift_tiAIefMX56_0_33_70D4178875715FB9B8B50C58F66F8D53Ll14anonymousTypesfMf_4namefMu1_: Equatable {
// EXPAND_MACRO_DECL-NEXT:   static func == (lhs: Self, rhs: Self) -> Bool {
// EXPAND_MACRO_DECL-NEXT:     false
// EXPAND_MACRO_DECL-NEXT:   }
// EXPAND_MACRO_DECL-NEXT: }"

//##-- cursor-info on attached macro
// RUN: %sourcekitd-test -req=cursor -pos=21:1 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CURSOR_ATTACHED %s
// RUN: %sourcekitd-test -req=cursor -pos=21:2 -cursor-action -req-opts=retrieve_symbol_graph=1 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CURSOR_ATTACHED %s
// CURSOR_ATTACHED: source.lang.swift.ref.macro (9:7-9:20)
// CURSOR_ATTACHED-LABEL: SYMBOL GRAPH BEGIN
// CURSOR_ATTACHED: "identifier": {
// CURSOR_ATTACHED-NEXT:   "interfaceLanguage": "swift",
// CURSOR_ATTACHED-NEXT:   "precise": "s:9MacroUser13myTypeWrapperyycfm"
// CURSOR_ATTACHED-NEXT: },
// CURSOR_ATTACHED-NEXT: "kind": {
// CURSOR_ATTACHED-NEXT:   "displayName": "Macro",
// CURSOR_ATTACHED-NEXT:   "identifier": "swift.macro"
// CURSOR_ATTACHED-NEXT: },
// CURSOR_ATTACHED: SYMBOL GRAPH END
// CURSOR_ATTACHED-LABEL: ACTIONS BEGIN
// CURSOR_ATTACHED: source.refactoring.kind.inline.macro
// CURSOR_ATTACHED-NEXT: Inline Macro
// CURSOR_ATTACHED: ACTIONS END

//##-- Expansion on attached macro
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=21:1 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=ATTACHED_EXPAND %s
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=21:2 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=ATTACHED_EXPAND %s
// ATTACHED_EXPAND: source.edit.kind.active:
// ATTACHED_EXPAND-NEXT: 23:3-23:3 (@__swiftmacro_9MacroUser1SV1x13myTypeWrapperfMr_.swift) "@accessViaStorage"
// ATTACHED_EXPAND-NEXT: source.edit.kind.active:
// ATTACHED_EXPAND-NEXT: 24:3-24:3 (@__swiftmacro_9MacroUser1SV1y13myTypeWrapperfMr0_.swift) "@accessViaStorage"
// ATTACHED_EXPAND-NEXT: source.edit.kind.active:
// ATTACHED_EXPAND-NEXT: 25:1-25:1 (@__swiftmacro_9MacroUser1S13myTypeWrapperfMm_.swift) "private var _storage = _Storage()"
// ATTACHED_EXPAND-NEXT: source.edit.kind.active:
// ATTACHED_EXPAND-NEXT: 21:1-21:15 ""

//##-- Cursor info on the attribute expanded by @myTypeWrapper
// RUN: %sourcekitd-test -req=cursor -cursor-action -req-opts=retrieve_symbol_graph=1 -offset=1 @__swiftmacro_9MacroUser1SV1x13myTypeWrapperfMr_.swift -primary-file %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=NESTED_ATTACHED_CURSOR %s
// NESTED_ATTACHED_CURSOR: source.lang.swift.ref.macro
// NESTED_ATTACHED_CURSOR-SAME: macro_basic.swift:10:27-10:43
// NESTED_ATTACHED_CURSOR-LABEL: SYMBOL GRAPH BEGIN
// NESTED_ATTACHED_CURSOR: "identifier": {
// NESTED_ATTACHED_CURSOR-NEXT:   "interfaceLanguage": "swift",
// NESTED_ATTACHED_CURSOR-NEXT:   "precise": "s:9MacroUser16accessViaStorageyycfm"
// NESTED_ATTACHED_CURSOR-NEXT: },
// NESTED_ATTACHED_CURSOR-NEXT: "kind": {
// NESTED_ATTACHED_CURSOR-NEXT:   "displayName": "Macro",
// NESTED_ATTACHED_CURSOR-NEXT:   "identifier": "swift.macro"
// NESTED_ATTACHED_CURSOR-NEXT: },
// NESTED_ATTACHED_CURSOR: SYMBOL GRAPH END
// NESTED_ATTACHED_CURSOR-LABEL: ACTIONS BEGIN
// NESTED_ATTACHED_CURSOR-NEXT: source.refactoring.kind.inline.macro
// NESTED_ATTACHED_CURSOR-NEXT: Inline Macro
// NESTED_ATTACHED_CURSOR-NEXT: ACTIONS END

//##-- Expansion on the attribute expanded by @myTypeWrapper
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=1:1 @__swiftmacro_9MacroUser1SV1x13myTypeWrapperfMr_.swift -primary-file %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=NESTED_ATTACHED_EXPAND %s
// NESTED_ATTACHED_EXPAND: source.edit.kind.active:
// NESTED_ATTACHED_EXPAND-NEXT: Macros/macro_basic.swift 23:13-23:13 (@__swiftmacro_9MacroUser1SV1x16accessViaStoragefMa_.swift) "{
// NESTED_ATTACHED_EXPAND-NEXT:  get {
// NESTED_ATTACHED_EXPAND-NEXT:    _storage.x
// NESTED_ATTACHED_EXPAND-NEXT:  }
// NESTED_ATTACHED_EXPAND-NEXT:  set {
// NESTED_ATTACHED_EXPAND-NEXT:    _storage.x = newValue
// NESTED_ATTACHED_EXPAND-NEXT:  }
// NESTED_ATTACHED_EXPAND-NEXT: }"
// NESTED_ATTACHED_EXPAND-NEXT: source.edit.kind.active:
// NESTED_ATTACHED_EXPAND-NEXT: 1:1-1:18 ""

//##-- Expansion on the first accessor macro
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=30:4 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=ACCESSOR1_EXPAND %s
// ACCESSOR1_EXPAND: source.edit.kind.active:
// ACCESSOR1_EXPAND-NEXT: 31:13-31:13 (@__swiftmacro_9MacroUser2S2V1x16accessViaStoragefMa_.swift) "{
// ACCESSOR1_EXPAND-NEXT:  get {
// ACCESSOR1_EXPAND-NEXT:    _storage.x
// ACCESSOR1_EXPAND-NEXT:  }
// ACCESSOR1_EXPAND-NEXT:  set {
// ACCESSOR1_EXPAND-NEXT:    _storage.x = newValue
// ACCESSOR1_EXPAND-NEXT:  }
// ACCESSOR1_EXPAND-NEXT: }"
// ACCESSOR1_EXPAND-NEXT: source.edit.kind.active:
// ACCESSOR1_EXPAND-NEXT: 30:3-30:20 ""

//##-- Expansion on the second accessor macro
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=33:13 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=ACCESSOR2_EXPAND %s
// ACCESSOR2_EXPAND: source.edit.kind.active:
// ACCESSOR2_EXPAND-NEXT: 34:14-34:18 (@__swiftmacro_9MacroUser2S2V1y16accessViaStoragefMa_.swift) "{
// ACCESSOR2_EXPAND-NEXT:  get {
// ACCESSOR2_EXPAND-NEXT:    _storage.y
// ACCESSOR2_EXPAND-NEXT:  }
// ACCESSOR2_EXPAND-NEXT:  set {
// ACCESSOR2_EXPAND-NEXT:    _storage.y = newValue
// ACCESSOR2_EXPAND-NEXT:  }
// ACCESSOR2_EXPAND-NEXT: }"
// ACCESSOR2_EXPAND-NEXT: source.edit.kind.active:
// ACCESSOR2_EXPAND-NEXT: 33:3-33:20 ""

//##-- Expansion on the addCompletionHandler macro.
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=42:5 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=PEER_EXPAND %s
// PEER_EXPAND: source.edit.kind.active:
// PEER_EXPAND-NEXT: 45:4-45:4 (@__swiftmacro_9MacroUser2S3V1f20addCompletionHandlerfMp_.swift) "func f(a: Int, for b: String, _ value: Double, completionHandler: @escaping (String) -> Void) {
// PEER_EXPAND-NEXT:  Task {
// PEER_EXPAND-NEXT:    completionHandler(await f(a: a, for: b, value))
// PEER_EXPAND-NEXT:  }
// PEER_EXPAND-NEXT: }"
// PEER_EXPAND-NEXT: source.edit.kind.active:
// PEER_EXPAND-NEXT: 42:3-42:24 ""

//##-- Expansion on a conformance macro.
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=51:5 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CONFORMANCE_EXPAND %s
// CONFORMANCE_EXPAND: source.edit.kind.active:
// CONFORMANCE_EXPAND-NEXT: 52:14-52:14 (@__swiftmacro_9MacroUser2S48HashablefMe_.swift) "extension S4: Hashable {
// CONFORMANCE_EXPAND-NEXT: }"
// CONFORMANCE_EXPAND-NEXT: source.edit.kind.active:
// CONFORMANCE_EXPAND-NEXT: 51:1-51:10 ""

//##-- Doc info, mostly just checking we don't crash because of the separate buffers
// RUN: %sourcekitd-test -req=doc-info %s -- ${COMPILER_ARGS_WITHOUT_SOURCE[@]} | %FileCheck -check-prefix=DOCINFO %s
// DOCINFO: key.name: "myTypeWrapper()"
// DOCINFO-NEXT: key.usr: "s:9MacroUser13myTypeWrapperyycfm"
// DOCINFO-NEXT: key.offset: 623
// DOCINFO: key.name: "myTypeWrapper()"
// DOCINFO-NEXT: key.usr: "s:9MacroUser13myTypeWrapperyycfm"
// DOCINFO-NEXT: key.offset: 252

//##-- Formatting shouldn't include the added attribute (or crash)
// RUN: %sourcekitd-test -req=format -line=23 -length=1 %s | %FileCheck -check-prefix=FORMATTED %s
// FORMATTED: "  var x: Int"

//##-- Expansion on "fails to typecheck" macro expression
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=61:2 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=ERRONEOUS_EXPAND %s
// ERRONEOUS_EXPAND: 61:1-61:18 (@__swiftmacro_{{.+}}.swift) "assert("foobar")"

//##-- Cursor-info on a decl where a peer macro attached.
// RUN: %sourcekitd-test -req=cursor -pos=67:7 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=CURSOR_ON_DECL_WITH_PEER %s
// CURSOR_ON_DECL_WITH_PEER: <decl.var.instance><syntaxtype.keyword>var</syntaxtype.keyword> <decl.name>test</decl.name>: <decl.var.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.type></decl.var.instance>
// CURSOR_ON_DECL_WITH_PEER-NOT: _foo

//##-- Expansion on the peer macro attached to pattern binding decl
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=66:4 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=EXPAND_PEER_ON_VAR %s
// EXPAND_PEER_ON_VAR: 67:21-67:21 (@__swiftmacro_9MacroUser2S5V4test21AddPeerStoredPropertyfMp_.swift) "public var _foo: Int = 100"

//##-- Expansion on a preamble macro.
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=76:5 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=PREAMBLE_EXPAND %s
// PREAMBLE_EXPAND: source.edit.kind.active:
// PREAMBLE_EXPAND-NEXT: 78:40-78:40 (@__swiftmacro_9MacroUser14doubleTheValue6TracedfMq_.swift) "log("Entering doubleTheValue(value: \(value))")
// PREAMBLE_EXPAND: defer {
// PREAMBLE_EXPAND-NEXT:  log("Exiting doubleTheValue(value:)")
// PREAMBLE_EXPAND-NEXT: }"
// PREAMBLE_EXPAND-NEXT: source.edit.kind.active

// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=77:5 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=PREAMBLE2_EXPAND %s
// PREAMBLE2_EXPAND: source.edit.kind.active:
// PREAMBLE2_EXPAND-NEXT: 78:40-78:40 (@__swiftmacro_9MacroUser14doubleTheValue6LoggedfMq_.swift) "let logger = Logger()
// PREAMBLE2_EXPAND-NEXT:logger.log(entering: "doubleTheValue(value: \(value))")
// PREAMBLE2_EXPAND-NEXT:defer {
// PREAMBLE2_EXPAND-NEXT:  logger.log(exiting: "doubleTheValue(value:)")
// PREAMBLE2_EXPAND-NEXT:}"
// PREAMBLE2_EXPAND-NEXT:source.edit.kind.active:

//##-- Expansion on a body macro
// RUN: %sourcekitd-test -req=refactoring.expand.macro -pos=86:5 %s -- ${COMPILER_ARGS[@]} | %FileCheck -check-prefix=BODY_EXPAND %s
// BODY_EXPAND: source.edit.kind.active:
// BODY_EXPAND-NEXT: 87:49-87:49 (@__swiftmacro_9MacroUser1f6RemotefMb_.swift) "{
// BODY_EXPAND-NEXT: return try await remoteCall(function: "f", arguments: ["a": a, "b": b])
// BODY_EXPAND-NEXT: }"
// BODY_EXPAND-NEXT: source.edit.kind.active:
