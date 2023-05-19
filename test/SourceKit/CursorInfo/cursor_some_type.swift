public protocol Proto {
  associatedtype Assoc
  func method() -> Assoc
}

public class Base {}

public class Derived: Base, Proto {
  public func method() -> Int {}
}

public struct S {
  public func foo<T>(x: T) -> some Base & Proto {
    return Derived()
  }
}

func test(value: S) {
  let _ = value.foo(x: 12)
}

// RUN: %sourcekitd-test -req=cursor -pos=13:15 %s -- %s -module-name Test | %FileCheck %s -check-prefix=DECLSITE
// DECLSITE: source.lang.swift.decl.function.method.instance (13:15-13:27)
// DECLSITE-NEXT: foo(x:)
// DECLSITE-NEXT: s:4Test1SV3foo1xQrx_tlF
// DECLSITE-NEXT: source.lang.swift
// DECLSITE-NEXT: <T> (S) -> (T) -> some Base & Proto
// DECLSITE-NEXT: $s1xQrx_tcluD
// DECLSITE-NEXT: Test{{$}}
// DECLSITE-NEXT: <Declaration>public func foo&lt;T&gt;(x: <Type usr=[[T_USR:.*]]>T</Type>) -&gt; some <Type usr=[[Base_USR:.*]]>Base</Type> &amp; <Type usr=[[Proto_USR:.*]]>Proto</Type></Declaration>
// DECLSITE-NEXT: <decl.function.method.instance><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>foo</decl.name>&lt;<decl.generic_type_param usr=[[T_USR]]><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>&gt;(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.generic_type_param usr=[[T_USR]]>T</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><syntaxtype.keyword>some</syntaxtype.keyword> <ref.class usr=[[Base_USR]]>Base</ref.class> &amp; <ref.protocol usr=[[Proto_USR]]>Proto</ref.protocol></decl.function.returntype></decl.function.method.instance>

// RUN: %sourcekitd-test -req=cursor -pos=13:43 %s -- %s -module-name Test | %FileCheck %s -check-prefix=PROTO_AFTER_SOME
// PROTO_AFTER_SOME: source.lang.swift.ref.protocol (1:17-1:22)
// PROTO_AFTER_SOME-NEXT: Proto
// PROTO_AFTER_SOME-NEXT: s:4Test5ProtoP
// PROTO_AFTER_SOME-NEXT: source.lang.swift
// FIXME: This should be '(any Proto).Type', but InterfaceTypeRequest returns a MetatypeType wrapping a ProtocolType, which is not a sensible type.
// PROTO_AFTER_SOME-NEXT: Proto.Type
// PROTO_AFTER_SOME-NEXT: $s4Test5Proto_pmD
// PROTO_AFTER_SOME-NEXT: Test{{$}}
// PROTO_AFTER_SOME-NEXT: <Declaration>public protocol Proto</Declaration>
// PROTO_AFTER_SOME-NEXT: <decl.protocol><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>protocol</syntaxtype.keyword> <decl.name>Proto</decl.name></decl.protocol>

// RUN: %sourcekitd-test -req=cursor -pos=19:17 %s -- %s -module-name Test | %FileCheck %s -check-prefix=USESITE
// USESITE: source.lang.swift.ref.function.method.instance (13:15-13:27)
// USESITE-NEXT: foo(x:)
// USESITE-NEXT: s:4Test1SV3foo1xQrx_tlF
// USESITE-NEXT: source.lang.swift
// USESITE-NEXT: <T> (S) -> (T) -> some Base & Proto
// USESITE-NEXT: $s1xQrx_tcluD
// USESITE-NEXT: <Container>$s4Test1SVD</Container>
// USESITE-NEXT: Test{{$}}
// USESITE-NEXT: <Declaration>public func foo&lt;T&gt;(x: <Type usr="s:4Test1SV3foo1xQrx_tlFQO1Txmfp">T</Type>) -&gt; some <Type usr=[[Base_USR:.*]]>Base</Type> &amp; <Type usr=[[Proto_USR:.*]]>Proto</Type></Declaration>
// USESITE-NEXT: <decl.function.method.instance><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>foo</decl.name>&lt;<decl.generic_type_param usr="s:4Test1SV3foo1xQrx_tlFQO1Txmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>&gt;(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.generic_type_param usr="s:4Test1SV3foo1xQrx_tlFQO1Txmfp">T</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><syntaxtype.keyword>some</syntaxtype.keyword> <ref.class usr=[[Base_USR]]>Base</ref.class> &amp; <ref.protocol usr=[[Proto_USR]]>Proto</ref.protocol></decl.function.returntype></decl.function.method.instance>
