// RUN: %target-swift-frontend -typecheck -parse-as-library -verify %s

// REQUIRES: OS=macosx

@main // expected-error {{'main()' is only available in macOS 10.99 or newer}}
@available(OSX 10.0, *)
struct EntryPoint { // expected-note {{update @available attribute for macOS from '10.0' to '10.99' to meet the requirements of '@main'}} {{6:16-20=10.99}}
  @available(OSX 10.99, *)
  static func main() {
  }
}


