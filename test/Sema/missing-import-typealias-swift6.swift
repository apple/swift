// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-emit-module-interface(%t/Original.swiftinterface) %t/Original.swift
// RUN: %target-swift-typecheck-module-from-interface(%t/Original.swiftinterface)

// RUN: %target-swift-emit-module-interface(%t/Aliases.swiftinterface) %t/Aliases.swift -I %t
// RUN: %target-swift-typecheck-module-from-interface(%t/Aliases.swiftinterface) -I %t

// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesNoImport.swift -enable-library-evolution -I %t -swift-version 6

// This test is a simplified version of missing-import-typealias.swift that
// verifies errors are emitted instead of warnings in Swift 6.

//--- Original.swift

open class Clazz {}


//--- Aliases.swift

import Original

public typealias ClazzAlias = Clazz


//--- UsesAliasesNoImport.swift

public import Aliases

// expected-error@+1 {{'ClazzAlias' aliases 'Original.Clazz' and cannot be used in a public or '@usableFromInline' conformance because 'Original' was not imported by this file}}
public class InheritsFromClazzAlias: ClazzAlias {}

