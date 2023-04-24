// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir %s | %FileCheck %s

public class Foo {
  @MainActor
  deinit {}
}

public class Bar {
  @MainActor
  deinit async {}
}

// CHECK: @"$s20deinit_isolation_tbd3FooCfZ"
// CHECK: @"$s20deinit_isolation_tbd3FooCfD"
// CHECK: @"$s20deinit_isolation_tbd3BarCfZ"
// CHECK: @"$s20deinit_isolation_tbd3BarCfD"
