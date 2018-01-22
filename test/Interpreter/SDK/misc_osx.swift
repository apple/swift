// RUN: %target-build-swift -typecheck %s -Xfrontend -verify
// REQUIRES: executable_test
// REQUIRES: OS=macosx

import CoreServices

func testFSEventStreamRef(stream: FSEventStreamRef) {
  // FIXME: These should be distinct types, constructible from one another.
  _ = stream as ConstFSEventStreamRef // works by coincidence because both are currently UnsafeRawPointer
  _ = ConstFSEventStreamRef(stream)

  // This is not a CF object.
  FSEventStreamRetain(stream) // no-warning
  FSEventStreamRelease(stream)

  let _: AnyObject = stream // expected-error {{value of type 'FSEventStreamRef' (aka 'UnsafeRawPointer') does not conform to specified type 'AnyObject'}}
}
