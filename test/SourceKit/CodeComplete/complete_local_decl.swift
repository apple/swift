// rdar://128294522 - Make sure we don't generate USRs for decls in local contexts.

func test1() {
  let loclVar = 0 // This typo is intentional; otherwise mangling applies a substitution for 'local'.
  // RUN: %sourcekitd-test -req=complete -pos=%(line):3 %s -- %s | %FileCheck %s --check-prefix LOCAL_VAR
  // LOCAL_VAR-NOT: key.associated_usrs: "s:{{.*}}loclVar{{.*}}"
  // LOCAL_VAR:     key.name: "loclVar"
  // LOCAL_VAR-NOT: key.associated_usrs: "s:{{.*}}loclVar{{.*}}"
}

func test2() {
  let _ = {
    struct S {
      func nestedMethod() {
        // RUN: %sourcekitd-test -req=complete -pos=%(line):3 %s -- %s | %FileCheck %s --check-prefix LOCAL_METHOD
        // RUN: %sourcekitd-test -req=complete -pos=%(line+1):14 %s -- %s | %FileCheck %s --check-prefix LOCAL_METHOD
        self.
        // LOCAL_METHOD-NOT: key.associated_usrs: "s:{{.*}}nestedMethod{{.*}}"
        // LOCAL_METHOD:     key.name: "nestedMethod()"
        // LOCAL_METHOD-NOT: key.associated_usrs: "s:{{.*}}nestedMethod{{.*}}"
      }
    }
  }
}

// Just to make sure 'key.associated_usrs' is produced for a non-local decl
// so the above CHECK-NOT's are working. If this fails, make sure to update the
// above checks too.
// RUN: %sourcekitd-test -req=complete -pos=%(line):1 %s -- %s | %FileCheck %s --check-prefix NON_LOCAL
// NON_LOCAL: key.associated_usrs: "s:{{.*}}test2{{.*}}"
