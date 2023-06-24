// RUN: %target-swift-frontend -O -emit-ir  -primary-file %s | %FileCheck %s

public struct PublicStruct {
}

public enum PublicEnum {
}

struct RegStruct {
    enum EnumInRegStruct {
        case case1
        case case2
    }

    private var optNode: Any?
    private var ClassInRegStructs = [ClassInRegStruct]()

    var isEmpty: Bool {
        return optNode == nil && ClassInRegStructs.isEmpty
    }

    private func funcInRegStruct() -> RegStruct? {
        var funcInRegStruct = RegStruct()
        return funcInRegStruct
    }

    func func2InRegStruct(boolParam: Bool = false,
                 _ body: (inout Bool) -> Void) {
        var finished = false
        func2InRegStruct(body, boolParam: boolParam, &finished)
    }

    private func func2InRegStruct(_ body: (inout Bool) -> Void,
                         boolParam: Bool = false, _ finished: inout Bool) {
        funcInRegStruct()?.func2InRegStruct(body, boolParam: boolParam, &finished)
    }

    private static func func2InRegStruct(_ ClassInRegStructs: [ClassInRegStruct],
                                _ body: (inout Bool) -> Void,
                                boolParam: Bool, _ finished: inout Bool) {
    }

    func funcInStructAndProtAndExt(_ EnumInRegStruct: EnumInRegStruct, space: PublicEnum,
                 _ body: () -> Void) {
        guard !isEmpty else { return }

        func2InRegStruct(boolParam: !EnumInRegStruct.isDownwards) { finished in
        }
    }

    final private class ClassInRegStruct {
    }
}

extension RegStruct.EnumInRegStruct {
    fileprivate var isDownwards: Bool {
        switch self {
        case .case1:
            return true
        case .case2:
            return false
        }
    }
}

private protocol ApplyRegStruct {
    mutating func applyTransform()
}

protocol RegStructable {
    mutating func funcInStructAndProtAndExt(from space: PublicEnum, transform: RegStruct)
}

extension ApplyRegStruct {
    mutating func funcInStructAndProtAndExt(
        from space: PublicEnum, transform: RegStruct
    ) {
        transform.funcInStructAndProtAndExt(.case2, space: space) {
// CHECK-LABEL: define hidden swiftcc void @"$sSa39devirt_witness_method_empty_conformanceAA12PublicStructVRszlE14applyTransformyyF"(ptr nocapture {{.*}}swiftself dereferenceable
// CHECK-NEXT: entry
// CHECK-NEXT: ret void
            applyTransform()
        }
    }
}

extension Array : ApplyRegStruct, RegStructable where Element == PublicStruct {
    mutating func applyTransform() {
    }
}
