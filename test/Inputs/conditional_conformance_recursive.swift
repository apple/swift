protocol P1 {
  associatedtype A: P1
}

protocol P2: P1 where A: P2 {
  associatedtype B
}

struct Wrapper<T: P1>: P1 {
  typealias A = Wrapper<T>
}

extension Wrapper: P2 where T: P2 {
  typealias B = T.A
}

protocol P3: P2 where A: P3 { }

extension Wrapper: P3 where T: P3 { }

// associated type witness table accessor for A : P2 in Wrapper<T>: P2
// CHECK-LABEL: define internal swiftcc ptr @"$s33conditional_conformance_recursive7WrapperVyxGAA2P2A2aERzrl1AAA2P1P_AaEPWT"
// CHECK: [[CONDITIONAL_REQ_BUFFER:%.*]] = alloca [1 x ptr]
// CHECK: [[FIRST_REQ:%.*]] = getelementptr inbounds [1 x ptr], ptr [[CONDITIONAL_REQ_BUFFER]]
// CHECK: call ptr @swift_getWitnessTable
