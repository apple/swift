// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s --check-prefix=CHECK --check-prefix=ONONE
// RUN: %target-swift-frontend -O -disable-llvm-optzns -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-ir %s | %FileCheck %s --check-prefix=CHECK --check-prefix=O

@_silgen_name("useMetadata")
func useMetadata<T>(_: T.Type)

// TODO: Although this is not explicitly final, class hierarchy analysis
// should figure out that it's effectively final because it has no
// subclasses.
private class PrivateEffectivelyFinal<T, U, V> {
  final func butts() {
    useMetadata(PrivateEffectivelyFinal<T, U, V>.self)
    useMetadata(PrivateEffectivelyFinal<Int, String, V>.self)
  }

  // CHECK-LABEL: define {{.*}}PrivateEffectivelyFinal{{.*}}cfC
  // CHECK:         call {{.*}}@swift_allocObject(ptr %0
}

// The class is not final and has subclasses, so we can only peephole
// metadata requests in limited circumstances.
private class PrivateNonfinal<T, U, V> {
  // The designated init allocating entry point is always overridden
  // by subclasses, so it can use the self metadata it was passed.

  // Methods in general on nonfinal classes cannot use the self metadata as
  // is.
  // CHECK-LABEL: define {{.*}}15PrivateNonfinal{{.*}}buttsyyF{{.*}}"
  @inline(never)
  final func butts() {
    // CHECK: [[INSTANTIATED_TYPE_RESPONSE:%.*]] = call {{.*}} @{{.*}}15PrivateNonfinal{{.*}}Ma
    // CHECK-NEXT: [[INSTANTIATED_TYPE:%.*]] = extractvalue {{.*}} [[INSTANTIATED_TYPE_RESPONSE]]
    // CHECK-NEXT: call {{.*}} @useMetadata(ptr [[INSTANTIATED_TYPE]], ptr [[INSTANTIATED_TYPE]])
    useMetadata(PrivateNonfinal<T, U, V>.self)
    // CHECK: [[INSTANTIATED_TYPE_RESPONSE:%.*]] = call {{.*}} @{{.*}}15PrivateNonfinal{{.*}}Ma
    // CHECK-NEXT: [[INSTANTIATED_TYPE:%.*]] = extractvalue {{.*}} [[INSTANTIATED_TYPE_RESPONSE]]
    // CHECK-NEXT: call {{.*}} @useMetadata(ptr [[INSTANTIATED_TYPE]], ptr [[INSTANTIATED_TYPE]])
    useMetadata(PrivateNonfinal<Int, String, V>.self)
  }

  // CHECK-LABEL: define {{.*}}15PrivateNonfinal{{.*}}cfC
  // CHECK:         call {{.*}}@swift_allocObject(ptr %0
}

// TODO: Although this is not explicitly final, class hierarchy analysis
// should figure out that it's effectively final because it has no
// subclasses.
private class PrivateNonfinalSubclass: PrivateNonfinal<Int, String, Float> {
  // CHECK-O-LABEL: define {{.*}}PrivateNonfinalSubclass{{.*}}cfC
  // CHECK-O:         call {{.*}}@swift_allocObject(ptr %0

  @inline(never)
  final func borts() {
    useMetadata(PrivateNonfinalSubclass.self)
  }

  // CHECK-ONONE-LABEL: define {{.*}}PrivateNonfinalSubclass{{.*}}cfC
  // CHECK-ONONE:         call {{.*}}@swift_allocObject(ptr %0
}

final private class FinalPrivateNonfinalSubclass<U>: PrivateNonfinal<U, String, Float> {
  // The class is final, so we can always form metadata for
  // FinalPrivateNonfinalSubclass<U> from the self argument.

  // CHECK-O-LABEL: define {{.*}}FinalPrivateNonfinalSubclass{{.*}}cfC"
  // CHECK-O:         call {{.*}}@swift_allocObject(ptr %0

  // CHECK-LABEL: define {{.*}}FinalPrivateNonfinalSubclass{{.*}}burts
  @inline(never)
  final func burts() {
    // CHECK: [[TYPE:%.*]] = load {{.*}} %0
    // CHECK: call {{.*}} @useMetadata(ptr [[TYPE]], ptr [[TYPE]])
    useMetadata(FinalPrivateNonfinalSubclass<U>.self)
    // CHECK: [[INSTANTIATED_TYPE:%.*]] = call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}}FinalPrivateNonfinalSubclass
    // CHECK: call {{.*}} @useMetadata(ptr [[INSTANTIATED_TYPE]], ptr [[INSTANTIATED_TYPE]])
    useMetadata(FinalPrivateNonfinalSubclass<Int>.self)
  }

  // CHECK-ONONE-LABEL: define {{.*}}FinalPrivateNonfinalSubclass{{.*}}cfC"
  // CHECK-ONONE:         call {{.*}}@swift_allocObject(ptr %0
}

  // CHECK-O-LABEL: define {{.*}}FinalPrivateNonfinalSubclass{{.*}}cfC"
  // CHECK-O:         call {{.*}}@swift_allocObject(ptr %0

final private class PrivateFinal<T, U, V> {
  // The class is final, so we can always form metadata for
  // PrivateFinal<T, U, V> from the self argument.

  // CHECK-LABEL: define {{.*}}PrivateFinal{{.*}}butts
  func butts() {
    // CHECK: [[TYPE:%.*]] = load {{.*}} %0
    // CHECK: call {{.*}} @useMetadata(ptr [[TYPE]], ptr [[TYPE]])
    useMetadata(PrivateFinal<T, U, V>.self)
    // CHECK: [[INSTANTIATED_TYPE:%.*]] = call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}}PrivateFinal
    // CHECK: call {{.*}} @useMetadata(ptr [[INSTANTIATED_TYPE]], ptr [[INSTANTIATED_TYPE]])
    useMetadata(PrivateFinal<Int, String, Float>.self)
  }

  // CHECK-LABEL: define {{.*}}PrivateFinal{{.*}}cfC"
  // CHECK:         call {{.*}}@swift_allocObject(ptr %0
}

public func useStuff<T, U, V>(_: T, _: U, _: V) {
  PrivateEffectivelyFinal<T, U, V>().butts()
  PrivateNonfinal<T, U, V>().butts()
  PrivateNonfinalSubclass().borts()
  FinalPrivateNonfinalSubclass<U>().burts()
  PrivateFinal<T, U, V>().butts()
}


