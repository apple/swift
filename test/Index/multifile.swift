// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// Validate that we're not looking into typealises across files / modules which leads to an assertion

// RUN: %target-swift-frontend -index-store-path %t/idx -o %t/file.o -typecheck -primary-file %t/file2.swift %t/file1.swift -verify
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %t/file2.swift %t/file1.swift > %t/output.txt
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %t/file1.swift %t/file2.swift >> %t/output.txt
// RUN: %FileCheck %s < %t/output.txt

//--- file1.swift

typealias Bar = [Int] // CHECK: 2:11 | type-alias/Swift | Bar | [[Bar_USR:.*]] | Def | rel: 0

//--- file2.swift

func foo() -> Bar { [] } // CHECK: 2:15 | type-alias/Swift | Bar | [[Bar_USR]] | Ref,RelCont | rel: 1
extension Optional where Wrapped == Bar {} // CHECK: 3:37 | type-alias/Swift | Bar | [[Bar_USR]] | Ref | rel: 0
