func renameInClosure() {
  // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):10 %s -- %s | %FileCheck %s --check-prefix=CLOSURE
  // CLOSURE: [[# @LINE+1]]:10-[[# @LINE+1]]:18 source.refactoring.range.kind.basename
  _ = { (toRename: Int?) in
    // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):12 %s -- %s | %FileCheck %s --check-prefix=CLOSURE
    // CLOSURE: [[# @LINE+1]]:12-[[# @LINE+1]]:20 source.refactoring.range.kind.basename
    if let toRename {
      // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):11 %s -- %s | %FileCheck %s --check-prefix=CLOSURE
      // CLOSURE: [[# @LINE+1]]:11-[[# @LINE+1]]:19 source.refactoring.range.kind.basename
      _ = toRename
    }
  }
}

func renameNonOptional() {
  // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):7 %s -- %s | %FileCheck %s --check-prefix=OPTIONAL
  // OPTIONAL: [[# @LINE+1]]:7-[[# @LINE+1]]:18 source.refactoring.range.kind.basename
  let nonOptional = 1
  // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):10 %s -- %s | %FileCheck %s --check-prefix=OPTIONAL
  // OPTIONAL: [[# @LINE+1]]:10-[[# @LINE+1]]:21 source.refactoring.range.kind.basename
  if let nonOptional {
    // RUN: %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+2):9 %s -- %s | %FileCheck %s --check-prefix=OPTIONAL
    // OPTIONAL: [[# @LINE+1]]:9-[[# @LINE+1]]:20 source.refactoring.range.kind.basename
    _ = nonOptional
  }
}

// Not ideal - we should allow renaming this locally. But it's an error case anyway.
func renameBuiltinMacroWithoutHash() {
  // RUN: not %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+1):10 %s -- %s 2>&1 | %FileCheck %s --check-prefix=BUILTIN
  if let file {
    // RUN: not %sourcekitd-test -req=find-local-rename-ranges -pos=%(line+1):9 %s -- %s 2>&1 | %FileCheck %s --check-prefix=BUILTIN
    _ = file
  }
  // BUILTIN: error: cannot rename system symbol 'file()'
}


// REQUIRES: swift_swift_parser
// FIXME: Swift parser is not enabled on Linux CI yet.
// REQUIRES: OS=macosx
