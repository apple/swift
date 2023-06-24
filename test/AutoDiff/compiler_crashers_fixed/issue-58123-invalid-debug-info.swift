// RUN: %target-swift-frontend -emit-ir -O -g %s | %FileCheck %s

// https://github.com/apple/swift/issues/58123
// Mutating functions with control flow can cause assertion failure for
// conflicting debug variable type

// CHECK-LABEL: define internal swiftcc float @"$s4main8TestTypeV24doDifferentiableTimeStep04timeG0ySf_tFTJpSSpSrTA"
// CHECK: [[SELF:%.*]] = alloca %T4main8TestTypeV06ManualB7TangentV
// CHECK: call void @llvm.dbg.declare(metadata ptr [[SELF]]

import _Differentiation

public protocol TestInterface {
    mutating func doDifferentiableTimeStep(timeStep: Float)

    var zeroTangentVectorInitializer: () -> TestInterfaceTangentVector { get }
    mutating func move(by offset: TestInterfaceTangentVector)
}

public protocol TestInterfaceTangentVector {
    static var zero: Self { get }
    static func add(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> TestInterfaceTangentVector
    static func subtract(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> TestInterfaceTangentVector
    static func equals(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> Bool
}

public extension TestInterface {
    var zeroTangentVector: TestInterfaceTangentVector { zeroTangentVectorInitializer() }
}

private typealias InitFunction = @convention(c) () -> UnsafeMutableRawPointer

public protocol HasZeroTangentVectorDuplicate: Differentiable {
    var duplicateZeroTangentVectorInitializer: () -> TangentVector { get }
}

public extension HasZeroTangentVectorDuplicate {
    var zeroTangentVector: TangentVector { duplicateZeroTangentVectorInitializer() }
}

public extension HasZeroTangentVectorDuplicate {
    var duplicateZeroTangentVectorInitializer: () -> TangentVector {
        { Self.TangentVector.zero }
    }
}

struct TestType: TestInterface {
	struct TestState: Differentiable {
		public struct TangentVector: Differentiable, AdditiveArithmetic {
			public typealias TangentVector = TestState.TangentVector
			public var property0: Float.TangentVector
			public var time: Float.TangentVector
			public var property1: Float.TangentVector
		}

		public mutating func move(by offset: TangentVector) {
			self.property0.move(by: offset.property0)
			self.time.move(by: offset.time)
			self.property1.move(by: offset.property1)
		}

		@noDerivative
		var needUpdate: Bool
		@noDerivative
		var initialConditionsAreStale: Bool
		var property0: Float
		var time: Float
		var property1: Float

		init() {
			self.needUpdate = true
			self.initialConditionsAreStale = true
			self.property0 = 0.01
			self.time = 0.01
			self.property1 = 0.01
		}
	}

	var state = TestState()

	@differentiable(reverse)
	mutating func doDifferentiableTimeStep(timeStep: Float) {
        if state.needUpdate {
            differentiableDoFlow()
        }
        if state.initialConditionsAreStale {
            doInit()
        }
	}

	@differentiable(reverse)
	mutating func differentiableDoFlow() {
        state.property1 = 1.2
        state.property0 = 2.3
        state.needUpdate = false
	}
	mutating func doInit() {
		state.initialConditionsAreStale = false
	}

}

extension TestType: Differentiable {
   struct ManualTestTangent: Differentiable & AdditiveArithmetic {
       var state: TestState.TangentVector
   }
   typealias TangentVector = ManualTestTangent

   mutating func move(by offset: ManualTestTangent) {
       self.state.move(by: offset.state)
   }
}
extension TestType: HasZeroTangentVectorDuplicate {}


extension TestType {
   mutating func move(by offset: TestInterfaceTangentVector) {
       self.move(by: offset as! Self.TangentVector)
   }

   var zeroTangentVectorInitializer: () -> TestInterfaceTangentVector {
       let initializer: () -> TangentVector = self.duplicateZeroTangentVectorInitializer
       return initializer
   }
}

extension TestType.TangentVector: TestInterfaceTangentVector {
   static func add(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> TestInterfaceTangentVector {
       return (lhs as! Self) + (rhs as! Self)
   }

   static func subtract(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> TestInterfaceTangentVector {
       return (lhs as! Self) - (rhs as! Self)
   }

   static func equals(lhs: TestInterfaceTangentVector, rhs: TestInterfaceTangentVector) -> Bool {
       return (lhs as! Self) == (rhs as! Self)
   }
}
