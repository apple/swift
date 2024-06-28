// RUN: %empty-directory(%t)
// RUN: split-file %s %t --leading-lines

/// Build the library.
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -module-name Lib1 -o %t
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -module-name Lib2 -o %t
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -module-name Lib3 -o %t
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -module-name Lib4 -o %t
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -module-name Lib5 -o %t

/// Test main cases.
// RUN: %target-swift-frontend -typecheck -verify %t/Client.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/Client_Scoped.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/Client_Clang.swift -I %t

/// Test language mode specific variants.
// RUN: %target-swift-frontend -typecheck -verify %t/Client_Swift6.swift -I %t \
// RUN:   -enable-upcoming-feature InternalImportsByDefault
// RUN: %target-swift-frontend -typecheck -verify %t/Client_Swift5.swift -I %t \
// RUN:   -swift-version 5

//--- Lib.swift

public struct Type1 {}
public struct Type2 {}

//--- Client.swift

/// Simple public vs internal.
public import Lib1 // expected-note {{imported 'public' here}}
internal import Lib1 // expected-warning {{module 'Lib1' is imported as 'public' from the same file; this 'internal' access level will be ignored}}

/// Simple public vs internal, inverted.
internal import Lib2 // expected-warning {{module 'Lib2' is imported as 'public' from the same file; this 'internal' access level will be ignored}}
public import Lib2 // expected-note {{imported 'public' here}}

/// 3 different ones.
public import Lib3 // expected-note 2 {{imported 'public' here}}
internal import Lib3 // expected-warning {{module 'Lib3' is imported as 'public' from the same file; this 'internal' access level will be ignored}}
private import Lib3 // expected-warning {{module 'Lib3' is imported as 'public' from the same file; this 'private' access level will be ignored}}

/// private vs fileprivate, we don't really need this warning but it may point to unintended stylistic inconsistencies.
fileprivate import Lib4 // expected-note {{imported 'fileprivate' here}}
private import Lib4 // expected-warning {{module 'Lib4' is imported as 'fileprivate' from the same file; this 'private' access level will be ignored}}

// Don't complain about repeated imports. As far as this diagnostic
// is concerned we may see this with scoped imports.
internal import Lib5
internal import Lib5
internal import Lib5
internal import Lib5

public func dummyAPI(t1: Lib1.Type1, t2: Lib2.Type1, t3: Lib3.Type1) {}

//--- Client_Swift5.swift

/// Simple public vs internal, imports defaults to public.
import Lib1 // expected-note {{imported 'public' here}}
// expected-error @-1 {{ambiguous implicit access level for import of 'Lib1'; it is imported as 'internal' elsewhere}}
// expected-note @-2 {{silence these warnings by adopting the upcoming feature 'InternalImportsByDefault'}}
internal import Lib1 // expected-warning {{module 'Lib1' is imported as 'public' from the same file; this 'internal' access level will be ignored}}
// expected-note @-1 {{imported 'internal' here}}

// There's no warning about "will be ignored" for a matching implicit access level.
public import Lib2
import Lib2

public func dummyAPI(t: Lib1.Type1, t2: Lib2.Type1) {}

//--- Client_Swift6.swift

/// Simple public vs internal, imports default to internal.
public import Lib1 // expected-note {{imported 'public' here}}
import Lib1 // expected-warning {{module 'Lib1' is imported as 'public' from the same file; this 'internal' access level will be ignored}}

// There's no warning about "will be ignored" for a matching implicit access level.
import Lib2
internal import Lib2

public func dummyAPI(t: Lib1.Type1) {}

//--- Client_Scoped.swift

/// Access level on scoped imports still import the whole module.
public import struct Lib1.Type1 // expected-note {{imported 'public' here}}
internal import struct Lib1.Type2 // expected-warning {{module 'Lib1' is imported as 'public' from the same file; this 'internal' access level will be ignored}}

public func dummyAPI(t: Lib1.Type1) {}

//--- Client_Clang.swift

public import ClangLib.Sub1 // expected-note {{imported 'public' here}}
private import ClangLib.Sub1 // expected-warning {{module 'Sub1' is imported as 'public' from the same file; this 'private' access level will be ignored}}
internal import ClangLib.Sub2

public func dummyAPI(t1: ClangType1, t2: ClangType2) {}

//--- module.modulemap

module ClangLib {
  header "ClangLib1.h"

  explicit module Sub1 {
    header "ClangLib2.h"
  }

  explicit module Sub2 {
    header "ClangLib3.h"
  }
}

//--- ClangLib1.h
struct ClangType1 {};
//--- ClangLib2.h
struct ClangType2 {};
//--- ClangLib3.h
