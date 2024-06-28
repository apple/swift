
// RUN: %target-swift-emit-silgen %s | %FileCheck %s

// CHECK-LABEL: sil {{.*}} @${{.*}}3foo{{.*}}F : {{.*}} {
func foo() {
    return bar(Baz())

    struct Baz {
        // CHECK-LABEL: sil {{.*}} @{{.*}}3foo{{.*}}3Baz{{.*}}C : {{.*}} {
        init() {}
    }

    // CHECK-LABEL: sil {{.*}} @{{.*}}3foo{{.*}}3bar{{.*}}F : {{.*}} {
    func bar(_: Any) {}

    // Check that we don't crash here
    lazy var v = 42
}
