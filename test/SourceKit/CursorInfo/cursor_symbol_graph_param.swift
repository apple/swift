func foo(_ fn: (Int) -> Void) {}

foo { bar in
}

// RUN: %empty-directory(%t)
// RUN:  %sourcekitd-test -req=cursor -pos=1:12 -req-opts=retrieve_symbol_graph=1 %s -- %s -target %target-triple | %FileCheck -check-prefix=CHECK_FN %s
// RUN:  %sourcekitd-test -req=cursor -pos=3:7 -req-opts=retrieve_symbol_graph=1 %s -- %s -target %target-triple | %FileCheck -check-prefix=CHECK_BAR %s

// CHECK_FN: SYMBOL GRAPH BEGIN
// CHECK_FN: {
// CHECK_FN:   "symbols": [
// CHECK_FN:     {
// CHECK_FN:       "identifier": {
// CHECK_FN:         "interfaceLanguage": "swift",
// CHECK_FN:         "precise": "s:25cursor_symbol_graph_param3fooyyySiXEF2fnL_yySiXEvp"
// CHECK_FN:       },
// CHECK_FN:       "kind": {
// CHECK_FN:         "displayName": "Global Variable",
// CHECK_FN:         "identifier": "swift.var"
// CHECK_FN:       },
// CHECK_FN:       "pathComponents": [
// CHECK_FN:         "foo(_:)",
// CHECK_FN:         "fn"
// CHECK_FN:       ]
// CHECK_FN:     }
// CHECK_FN:   ]
// CHECK_FN: }
// CHECK_FN: SYMBOL GRAPH END

// CHECK_BAR: SYMBOL GRAPH BEGIN
// CHECK_BAR: {
// CHECK_BAR:   "symbols": [
// CHECK_BAR:     {
// CHECK_BAR:       "identifier": {
// CHECK_BAR:         "interfaceLanguage": "swift",
// CHECK_BAR:         "precise": "s:25cursor_symbol_graph_paramySiXEfU_3barL_Sivp"
// CHECK_BAR:       },
// CHECK_BAR:       "kind": {
// CHECK_BAR:         "displayName": "Global Variable",
// CHECK_BAR:         "identifier": "swift.var"
// CHECK_BAR:       },
// CHECK_BAR:       "pathComponents": [
// CHECK_BAR:         "bar"
// CHECK_BAR:       ]
// CHECK_BAR:     }
// CHECK_BAR:   ]
// CHECK_BAR: }
// CHECK_BAR: SYMBOL GRAPH END
