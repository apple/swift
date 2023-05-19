// REQUIRES: objc_interop

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -import-objc-header %S/Inputs/nested_protocol_name.h -typecheck -verify %s

// RUN: echo '#include "nested_protocol_name.h"' > %t.m
// RUN: %target-swift-ide-test -source-filename %s -print-header -header-to-print %S/Inputs/nested_protocol_name.h -import-objc-header %S/Inputs/nested_protocol_name.h -print-regular-comments --cc-args %target-cc-options -fsyntax-only %t.m -I %S/Inputs > %t.txt
// RUN: %FileCheck -check-prefix=HEADER %s < %t.txt

// rdar://59431058
// Let's make sure this works, but let's not encourage its spread...

// HEADER: class Trunk {
// HEADER:   init!()
// HEADER:   class func addLimb(_ limb: (any Trunk.Branch)!)
// HEADER:   func addLimb(_ limb: (any Trunk.Branch)!)
// HEADER:   func addLimbs(_ limbs: [any Trunk.Branch]!)
// HEADER: }
// HEADER: // NS_SWIFT_NAME(Trunk.Branch)
// HEADER: protocol Branch {
// HEADER:   func flower()
// HEADER: }

func grow(_ branch: Trunk.Branch, from trunk: Trunk) {
  branch.flower()
  trunk.addLimb(branch)
}

// rdar://95084142 - crash while matching existential types
func grow_multiple(_ branches: [Trunk.Branch], from trunk: Trunk) {
  trunk.addLimbs(branches) // ok
}

class SturdyBranch: Trunk.Branch {
  func flower() {}
}

// FIXME: Odd that name lookup can't find this...
class NormalBranch: Branch { // expected-error {{cannot find type 'Branch' in scope}}
  func flower() {}
}

class WeakBranch: TrunkBranchProtocol { // expected-error {{'TrunkBranchProtocol' has been renamed to 'Trunk.Branch'}}
  func flower() {}
}
