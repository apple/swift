// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

protocol P {}

(any P).#^COMPLETE?^#
// COMPLETE: Begin completions, 3 items
// COMPLETE: Keyword[self]/CurrNominal:          self[#(any P).Type#]; name=self
// COMPLETE: Keyword/CurrNominal:                Protocol[#(any P).Type#]; name=Protocol
// COMPLETE: Keyword/CurrNominal:                Type[#(any P).Type#]; name=Type


P.Type.#^PROTOCOLTYPE_DOT_1?^#
// PROTOCOLTYPE_DOT_1: Begin completions, 3 items
// PROTOCOLTYPE_DOT_1: Keyword[self]/CurrNominal:          self[#(any P.Type).Type#]; name=self
// PROTOCOLTYPE_DOT_1: Keyword/CurrNominal:                Protocol[#(any P.Type).Type#]; name=Protocol
// PROTOCOLTYPE_DOT_1: Keyword/CurrNominal:                Type[#any P.Type.Type#]; name=Type

(P).Type.#^PROTOCOLTYPE_DOT_2?^#
// PROTOCOLTYPE_DOT_2: Begin completions, 3 items
// PROTOCOLTYPE_DOT_2: Keyword[self]/CurrNominal:          self[#(any (P).Type).Type#]; name=self
// PROTOCOLTYPE_DOT_2: Keyword/CurrNominal:                Protocol[#(any (P).Type).Type#]; name=Protocol
// PROTOCOLTYPE_DOT_2: Keyword/CurrNominal:                Type[#any (P).Type.Type#]; name=Type
