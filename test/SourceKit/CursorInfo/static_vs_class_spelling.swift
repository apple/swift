// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/Modules)
// RUN: split-file --leading-lines %s %t

// RUN: %target-swift-frontend \
// RUN:     -emit-module \
// RUN:     -module-name MyModule \
// RUN:     -emit-module-path %t/Modules/MyModule.swiftmodule \
// RUN:     %t/MyModule.swift

//--- MyModule.swift
public class UserCollection {
  public static let sharedStatic = UserCollection()
  public class var sharedComputedClass: UserCollection { UserCollection() }
}

//--- test.swift
import MyModule

func application() {
  // RUN: %sourcekitd-test -req=cursor -pos=%(line+1):18 %t/test.swift -- %t/test.swift -I %t/Modules -target %target-triple -Xfrontend -sil-verify-none | %FileCheck %s --check-prefix=SHARED_STATIC
  UserCollection.sharedStatic
  // FIXME: This should be reported as 'static var' rdar://105239467
  // SHARED_STATIC: <Declaration>class let sharedStatic: <Type usr="s:8MyModule14UserCollectionC">UserCollection</Type></Declaration>
  // SHARED_STATIC: <decl.var.class><syntaxtype.keyword>class</syntaxtype.keyword> <syntaxtype.keyword>let</syntaxtype.keyword> <decl.name>sharedStatic</decl.name>: <decl.var.type><ref.class usr="s:8MyModule14UserCollectionC">UserCollection</ref.class></decl.var.type></decl.var.class>

  // RUN: %sourcekitd-test -req=cursor -pos=%(line+1):18 %t/test.swift -- %t/test.swift -I %t/Modules -target %target-triple | %FileCheck %s --check-prefix=SHARED_COMPUTED_CLASS
  UserCollection.sharedComputedClass
  // SHARED_COMPUTED_CLASS: <Declaration>class var sharedComputedClass: <Type usr="s:8MyModule14UserCollectionC">UserCollection</Type> { get }</Declaration>
  // SHARED_COMPUTED_CLASS: <decl.var.class><syntaxtype.keyword>class</syntaxtype.keyword> <syntaxtype.keyword>var</syntaxtype.keyword> <decl.name>sharedComputedClass</decl.name>: <decl.var.type><ref.class usr="s:8MyModule14UserCollectionC">UserCollection</ref.class></decl.var.type> { <syntaxtype.keyword>get</syntaxtype.keyword> }</decl.var.class>
}
