// RUN: %empty-directory(%t)

// RUN: touch %t/empty.swift
// RUN: echo "func foo() {}" >> %t/func.swift

// Edit previously did not update the syntax info. Cursor info was using its
// buffer to calculate line and column (before rdar://78161348).
// RUN: %sourcekitd-test \
// RUN:   -req=open -text-input %t/empty.swift %t/func.swift -- %t/func.swift == \
// RUN:   -req=edit -offset=0 -length=0 -replace="func foo() {}" -req-opts=enablesyntaxmap=0,enablesubstructure=0,enablediagnostics=0 %t/func.swift -- %t/func.swift == \
// RUN:   -req=cursor -offset=5 %t/func.swift -- %t/func.swift
