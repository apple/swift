public struct MyStruct<T> {}
public typealias Alias<T> = MyStruct<T>
public typealias Aliased = Alias

// RUN: %sourcekitd-test -req=cursor -pos=3:18 %s -- %s | %FileCheck %s

// CHECK: source.lang.swift.decl.typealias (3:18-3:25)
// CHECK-NEXT: Aliased
// CHECK-NEXT: s:13rdar_343487767Aliaseda
// CHECK-NEXT: source.lang.swift
// CHECK-NEXT: Alias.Type
// CHECK-NEXT: $s13rdar_343487765AliasamD
// CHECK-NEXT: <Declaration>public typealias Aliased = <Type usr="s:13rdar_343487765Aliasa">Alias</Type></Declaration>
// CHECK-NEXT: <decl.typealias><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>Aliased</decl.name> = <ref.typealias usr="s:13rdar_343487765Aliasa">Alias</ref.typealias></decl.typealias>
// CHECK-NEXT: Alias.Type
// CHECK-NEXT: $s13rdar_343487765AliasamD
// CHECK-NEXT: <Declaration>public typealias Aliased = <Type usr="s:13rdar_343487765Aliasa">Alias</Type></Declaration>
// CHECK-NEXT: <decl.typealias><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>Aliased</decl.name> = <ref.typealias usr="s:13rdar_343487765Aliasa">Alias</ref.typealias></decl.typealias>
