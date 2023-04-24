// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir %s | %FileCheck %s

public class Foo {
  @MainActor
  deinit {}
}

// CHECK: @"$s20deinit_isolation_tbd3FooCfZ"
// CHECK: @"$s20deinit_isolation_tbd3FooCfD"
