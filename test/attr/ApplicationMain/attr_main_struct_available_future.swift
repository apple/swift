// RUN: %target-swift-frontend -typecheck -parse-as-library -verify %s

// REQUIRES: OS=macosx

@main // expected-error {{'main()' is only available in macOS 10.99 or newer}}
// expected-note @-1 {{change the @available attribute of the struct on macOS from 10.0 to 10.99}}
@available(OSX 10.0, *)
struct EntryPoint {
  @available(OSX 10.99, *)
  static func main() {
  }
}


