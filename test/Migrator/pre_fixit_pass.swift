// REQUIRES: objc_interop
// RUN: rm -rf %t && mkdir -p %t && not %target-swift-frontend -c -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/API.json -emit-migrated-file-path %t/pre_fixit_pass.swift.result -o /dev/null
// RUN: diff -u %S/pre_fixit_pass.swift.expected %t/pre_fixit_pass.swift.result

import Bar

struct New {}
@available(*, unavailable, renamed: "New")
struct Old {}
Old()

func foo(_ a : PropertyUserInterface) {
  a.setField(1)
  _ = a.field()
}
