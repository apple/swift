// REQUIRES: objc_interop
// REQUIRES: OS=macosx
// RUN: rm -rf %t && mkdir -p %t/stats-pre && mkdir -p %t/stats-post
//
// Prime module cache
// RUN: %target-swift-frontend -typecheck -I %S/Inputs/NamedLazyMembers -typecheck %s
//
// Check that without lazy named member loading, we're importing too much.
// RUN: %target-swift-frontend -typecheck -I %S/Inputs/NamedLazyMembers -stats-output-dir %t/stats-pre %s
// RUN: %utils/process-stats-dir.py --evaluate 'NumTotalClangImportedEntities > 20' %t/stats-pre
//
// Check that with lazy named member loading, we're importing less.
// RUN: %target-swift-frontend -typecheck -I %S/Inputs/NamedLazyMembers -enable-named-lazy-member-loading -stats-output-dir %t/stats-post %s
// RUN: %utils/process-stats-dir.py --evaluate 'NumTotalClangImportedEntities < 10' %t/stats-post

import NamedLazyMembers

public func foo(d: SimpleDoer) {
  let _ = d.simplyDoSomeWork()
  let _ = d.simplyDoSomeWork(withSpeed:10)
  let _ = d.simplyDoVeryImportantWork(speed:10, thoroughness:12)
  let _ = d.simplyDoSomeWorkWithSpeed(speed:10, levelOfAlacrity:12)
}
