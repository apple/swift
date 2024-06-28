// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// REQUIRES: objc_interop

import Foundation

func foo(_ x: CGFloat) {}
func foo(_ x: Double) {}

// Make sure we suggest completions for both CGFloat and Double.
foo(.#^FOO^#)
// FOO-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#CGFloat#]; name=init()
// FOO-DAG: Decl[Constructor]/CurrNominal/IsSystem/TypeRelation[Convertible]: init()[#Double#]; name=init()
