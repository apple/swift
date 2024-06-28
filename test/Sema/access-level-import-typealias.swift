// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-emit-module-interface(%t/Original.swiftinterface) %t/Original.swift
// RUN: %target-swift-typecheck-module-from-interface(%t/Original.swiftinterface)

// RUN: %target-swift-emit-module-interface(%t/Aliases.swiftinterface) %t/Aliases.swift -I %t
// RUN: %target-swift-typecheck-module-from-interface(%t/Aliases.swiftinterface) -I %t

// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesNoImport.swift -I %t \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesNoImport.swift -I %t \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -enable-upcoming-feature InternalImportsByDefault

//--- Original.swift
open class Clazz {}

//--- Aliases.swift
import Original
public typealias ClazzAlias = Clazz

//--- UsesAliasesNoImport.swift
public import Aliases
internal import Original // expected-note 2 {{class 'Clazz' imported as 'internal' from 'Original' here}}

// expected-error@+1 {{'ClazzAlias' aliases 'Original.Clazz' and cannot be used in a public or '@usableFromInline' conformance because 'Original' was not imported publicly}}
public class InheritsFromClazzAlias: ClazzAlias {}

@inlinable public func inlinableFunc() {
  // expected-error@+1 {{'ClazzAlias' aliases 'Original.Clazz' and cannot be used in an '@inlinable' function because 'Original' was not imported publicly}}
  _ = ClazzAlias.self
}
