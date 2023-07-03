// RUN: %target-swift-frontend -primary-file %s -emit-ir > %t.ll
// RUN: %FileCheck %s -check-prefix=GLOBAL < %t.ll
// RUN: %FileCheck %s < %t.ll

// RUN: %target-swift-frontend -primary-file %s -emit-ir -wmo -num-threads 1 > %t.ll.wmo
// RUN: %FileCheck %s -check-prefix=GLOBAL < %t.ll.wmo
// RUN: %FileCheck %s < %t.ll.wmo
// REQUIRES: CPU=x86_64

protocol P {}
protocol Q {}

protocol Assocked {
  associatedtype Assoc : P, Q
}

struct Universal : P, Q {}


// CHECK-LABEL: @"symbolic _____ 23associated_type_witness12OuterPrivate{{.*}}InnermostV" = linkonce_odr hidden constant
// CHECK-SAME: @"$s23associated_type_witness12OuterPrivate{{.*}}5InnerE0V9InnermostVMn"
private struct OuterPrivate {
  struct InnerPrivate: HasSimpleAssoc {
    struct Innermost { }
    typealias Assoc = Innermost
  }
}

// CHECK: [[ASSOC_TYPE_NAMES:@.*]] = private constant [29 x i8] c"OneAssoc TwoAssoc ThreeAssoc\00"
// CHECK: @"$s23associated_type_witness18HasThreeAssocTypesMp" =
// CHECK-SAME: [[ASSOC_TYPE_NAMES]] to i64

protocol HasThreeAssocTypes {
  associatedtype OneAssoc
  associatedtype TwoAssoc
  associatedtype ThreeAssoc
}

//   Witness table for WithUniversal : Assocked.
// GLOBAL-LABEL: @"$s23associated_type_witness13WithUniversalVAA8AssockedAAWP" = hidden global [4 x ptr] [
// GLOBAL-SAME:    @"$s23associated_type_witness13WithUniversalVAA8AssockedAAMc"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness13WithUniversalVAA8AssockedAA5AssocAaDP_AA1P",
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness13WithUniversalVAA8AssockedAA5AssocAaDP_AA1Q"
// GLOBAL-SAME:    @"symbolic{{.*}}23associated_type_witness9UniversalV"
// GLOBAL-SAME:  ]
struct WithUniversal : Assocked {
  typealias Assoc = Universal
}

//   Witness table for GenericWithUniversal : Assocked.
// GLOBAL-LABEL: @"$s23associated_type_witness20GenericWithUniversalVyxGAA8AssockedAAWP" = hidden global [4 x ptr] [
// GLOBAL-SAME:    @"$s23associated_type_witness20GenericWithUniversalVyxGAA8AssockedAAMc"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness20GenericWithUniversalVyxGAA8AssockedAA5AssocAaEP_AA1P"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness20GenericWithUniversalVyxGAA8AssockedAA5AssocAaEP_AA1Q"
// GLOBAL-SAME:    @"symbolic{{.*}}23associated_type_witness9UniversalV"
// GLOBAL-SAME:  ]
struct GenericWithUniversal<T> : Assocked {
  typealias Assoc = Universal
}

//   Witness table for Fulfilled : Assocked.
// GLOBAL-LABEL: @"$s23associated_type_witness9FulfilledVyxGAA8AssockedAAWp" = internal global [4 x ptr] [
// GLOBAL-SAME:    @"$s23associated_type_witness9FulfilledVyxGAA8AssockedAAMc"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness9FulfilledVyxGAA8AssockedAA5AssocAaEP_AA1P"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness9FulfilledVyxGAA8AssockedAA5AssocAaEP_AA1Q"
// GLOBAL-SAME:    @"symbolic x"
// GLOBAL-SAME:  ]
struct Fulfilled<T : P & Q> : Assocked {
  typealias Assoc = T
}

//   Associated type witness table access function for Fulfilled.Assoc : P.
// CHECK-LABEL:  define internal swiftcc ptr @"$s23associated_type_witness9FulfilledVyxGAA8AssockedAA5AssocAaEP_AA1PPWT"(ptr %"Fulfilled<T>.Assoc", ptr %"Fulfilled<T>", ptr %"Fulfilled<T>.Assocked")
// CHECK:    [[T1:%.*]] = getelementptr inbounds ptr, ptr %"Fulfilled<T>", i64 3
// CHECK-NEXT:    [[T2:%.*]] = load ptr, ptr [[T1]], align 8, !invariant.load
// CHECK-NEXT:    ret ptr [[T2]]

//   Associated type witness table access function for Fulfilled.Assoc : Q.
// CHECK-LABEL:  define internal swiftcc ptr @"$s23associated_type_witness9FulfilledVyxGAA8AssockedAA5AssocAaEP_AA1QPWT"(ptr %"Fulfilled<T>.Assoc", ptr %"Fulfilled<T>", ptr %"Fulfilled<T>.Assocked")
// CHECK:    [[T1:%.*]] = getelementptr inbounds ptr, ptr %"Fulfilled<T>", i64 4
// CHECK-NEXT:    [[T2:%.*]] = load ptr, ptr [[T1]], align 8, !invariant.load
// CHECK-NEXT:    ret ptr [[T2]]

struct Pair<T, U> : P, Q {}

//   Generic witness table pattern for Computed : Assocked.
// GLOBAL-LABEL: @"$s23associated_type_witness8ComputedVyxq_GAA8AssockedAAWp" = internal global [4 x ptr] [
// GLOBAL-SAME:    @"$s23associated_type_witness8ComputedVyxq_GAA8AssockedAAMc"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness8ComputedVyxq_GAA8AssockedAA5AssocAaEP_AA1P"
// GLOBAL-SAME:    @"associated conformance 23associated_type_witness8ComputedVyxq_GAA8AssockedAA5AssocAaEP_AA1Q"
// GLOBAL-SAME:    @"symbolic{{.*}}23associated_type_witness4PairV{{.*}}"
// GLOBAL-SAME:  ]

//   Protocol conformance descriptor for Computed : Assocked.
// GLOBAL-LABEL: @"$s23associated_type_witness8ComputedVyxq_GAA8AssockedAAMc" = hidden constant
// GLOBAL-SAME:    i16 4,
// GLOBAL-SAME:    i16 1,

//    No instantiator function
// GLOBAL-SAME:    i32 0,
// GLOBAL-SAME:    i32 trunc (i64 sub (i64 ptrtoint (ptr [[PRIVATE:@.*]] to i64), i64 ptrtoint
// GLOBAL-SAME:  }

struct Computed<T, U> : Assocked {
  typealias Assoc = Pair<T, U>
}

//   Instantiation function for GenericComputed : DerivedFromSimpleAssoc.
// CHECK-LABEL: define internal void @"$s23associated_type_witness15GenericComputedVyxGAA22DerivedFromSimpleAssocAAWI"(ptr %0, ptr %"GenericComputed<T>", ptr %1)
// CHECK:         [[T0:%.*]] = call ptr @swift_getWitnessTable({{.*}}@"$s23associated_type_witness15GenericComputedVyxGAA14HasSimpleAssocAAMc"
// CHECK-NEXT:    [[T2:%.*]] = getelementptr inbounds ptr, ptr %0, i32 1
// CHECK-NEXT:    store ptr [[T0]], ptr [[T2]], align 8
// CHECK-NEXT:    ret void


struct PBox<T: P> {}
protocol HasSimpleAssoc {
  associatedtype Assoc
}
protocol DerivedFromSimpleAssoc : HasSimpleAssoc {}

//   Generic witness table pattern for GenericComputed : DerivedFromSimpleAssoc.
// GLOBAL-LABEL: @"$s23associated_type_witness15GenericComputedVyxGAA22DerivedFromSimpleAssocAAWp" = internal constant [2 x ptr]
// GLOBAL-SAME: @"$s23associated_type_witness15GenericComputedVyxGAA22DerivedFromSimpleAssocAAMc"
// GLOBAL-SAME: ptr null

//   Protocol conformance descriptor for GenericComputed : DerivedFromSimpleAssoc.
// GLOBAL-LABEL: @"$s23associated_type_witness15GenericComputedVyxGAA22DerivedFromSimpleAssocAAMc" = hidden constant
// GLOBAL-SAME:    i16 2,
// GLOBAL-SAME:    i16 1,

//   Relative reference to instantiator function
// GLOBAL-SAME:    i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s23associated_type_witness15GenericComputedVyxGAA22DerivedFromSimpleAssocAAWI" to i64),

//   Relative reference to private data
struct GenericComputed<T: P> : DerivedFromSimpleAssoc {
  typealias Assoc = PBox<T>
}

protocol HasAssocked {
  associatedtype Contents : Assocked
}
struct FulfilledFromAssociatedType<T : HasAssocked> : HasSimpleAssoc {
  typealias Assoc = PBox<T.Contents.Assoc>
}

struct UsesVoid : HasSimpleAssoc {
  typealias Assoc = ()
}

// https://github.com/apple/swift/issues/54052
// Failure to canonicalize type in associated type witness

struct Validator<T> {
  let validatorFailureType: Any.Type
}


protocol ValidatorType {
  associatedtype Data
  associatedtype Failure
  func validator() -> Validator<Data>
}


extension ValidatorType {
  func validator() -> Validator<Data> {
    .init(validatorFailureType: Failure.self)
  }
}


// MARK: Failing example
extension Validator where T == String {
  struct V: ValidatorType {
    typealias Data = T // or String

    struct Failure {}
  }
}

// GLOBAL-LABEL: @"symbolic _____ySS__G 23associated_type_witness9ValidatorVAASSRszlE1VV7FailureV"


