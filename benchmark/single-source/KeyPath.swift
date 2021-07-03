//===--- KeyPath.swift -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// These tests check the performance of KeyPath access
import TestsUtils

var fixedSizeArray10 = initializeFixedSizeArray10()

let kp0 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 0))
let kp1 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 1))
let kp2 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 2))
let kp3 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 3))
let kp4 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 4))
let kp5 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 5))
let kp6 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 6))
let kp7 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 7))
let kp8 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 8))
let kp9 = identity(FixedSizeArray10<Double>.getKeypathToElement(index: 9))

public let KeyPath = [
    BenchmarkInfo(name: "StructKeyPathComputation", runFunction: runStructKeyPathComputation, tags: [.validation, .api]),
]

@inline(never)
public func runStructKeyPathComputation(N: Int) {
    let iters = 50000
    
    for n in 0..<iters {
        fixedSizeArray10[keyPath: kp5] += fixedSizeArray10[keyPath: kp1] + Double(n)
        fixedSizeArray10[keyPath: kp5] += fixedSizeArray10[keyPath: kp7] - fixedSizeArray10[keyPath: kp5]
        fixedSizeArray10[keyPath: kp4] += fixedSizeArray10[keyPath: kp3] * fixedSizeArray10[keyPath: kp6]
        fixedSizeArray10[keyPath: kp1] += fixedSizeArray10[keyPath: kp3] / fixedSizeArray10[keyPath: kp4]
        fixedSizeArray10[keyPath: kp0] += fixedSizeArray10[keyPath: kp2] + fixedSizeArray10[keyPath: kp1]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp6] - fixedSizeArray10[keyPath: kp5]
        fixedSizeArray10[keyPath: kp3] += fixedSizeArray10[keyPath: kp2] * fixedSizeArray10[keyPath: kp6]
        fixedSizeArray10[keyPath: kp2] += fixedSizeArray10[keyPath: kp5] / fixedSizeArray10[keyPath: kp5]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp8] + fixedSizeArray10[keyPath: kp9]
        fixedSizeArray10[keyPath: kp9] += fixedSizeArray10[keyPath: kp4] - fixedSizeArray10[keyPath: kp4]
        fixedSizeArray10[keyPath: kp9] += fixedSizeArray10[keyPath: kp7] * fixedSizeArray10[keyPath: kp6]
        fixedSizeArray10[keyPath: kp5] += fixedSizeArray10[keyPath: kp7] / fixedSizeArray10[keyPath: kp3]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp5] + fixedSizeArray10[keyPath: kp8]
        fixedSizeArray10[keyPath: kp9] += fixedSizeArray10[keyPath: kp8] - fixedSizeArray10[keyPath: kp2]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp8] * fixedSizeArray10[keyPath: kp6]
        fixedSizeArray10[keyPath: kp5] += fixedSizeArray10[keyPath: kp1] / fixedSizeArray10[keyPath: kp6]
        fixedSizeArray10[keyPath: kp1] += fixedSizeArray10[keyPath: kp6] + fixedSizeArray10[keyPath: kp2]
        fixedSizeArray10[keyPath: kp2] += fixedSizeArray10[keyPath: kp9] - fixedSizeArray10[keyPath: kp0]
        fixedSizeArray10[keyPath: kp7] += fixedSizeArray10[keyPath: kp5] * fixedSizeArray10[keyPath: kp3]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp8] / fixedSizeArray10[keyPath: kp9]
        fixedSizeArray10[keyPath: kp6] += fixedSizeArray10[keyPath: kp4] + fixedSizeArray10[keyPath: kp0]
        fixedSizeArray10[keyPath: kp8] += fixedSizeArray10[keyPath: kp2] - fixedSizeArray10[keyPath: kp4]
        fixedSizeArray10[keyPath: kp6] += fixedSizeArray10[keyPath: kp1] * fixedSizeArray10[keyPath: kp7]
        fixedSizeArray10[keyPath: kp3] += fixedSizeArray10[keyPath: kp8] / fixedSizeArray10[keyPath: kp1]
        fixedSizeArray10[keyPath: kp7] += fixedSizeArray10[keyPath: kp4] + fixedSizeArray10[keyPath: kp3]
        fixedSizeArray10[keyPath: kp1] += fixedSizeArray10[keyPath: kp9] - fixedSizeArray10[keyPath: kp3]
    }
    
    blackHole(fixedSizeArray10)
}

public struct FixedSizeArray10<Element>: Sequence, IteratorProtocol {
    public init(
        element0: Element,
        element1: Element,
        element2: Element,
        element3: Element,
        element4: Element,
        element5: Element,
        element6: Element,
        element7: Element,
        element8: Element,
        element9: Element
    ) {
        self.element0 = element0
        self.element1 = element1
        self.element2 = element2
        self.element3 = element3
        self.element4 = element4
        self.element5 = element5
        self.element6 = element6
        self.element7 = element7
        self.element8 = element8
        self.element9 = element9
    }
    
    let count: Int = 10
    
    public var element0: Element
    public var element1: Element
    public var element2: Element
    public var element3: Element
    public var element4: Element
    public var element5: Element
    public var element6: Element
    public var element7: Element
    public var element8: Element
    public var element9: Element
    
    public static func getKeypathToElement(index: Int) -> WritableKeyPath<FixedSizeArray10<Element>, Element> {
        switch index {
            case 0:
                return \FixedSizeArray10.element0
            case 1:
                return \FixedSizeArray10.element1
            case 2:
                return \FixedSizeArray10.element2
            case 3:
                return \FixedSizeArray10.element3
            case 4:
                return \FixedSizeArray10.element4
            case 5:
                return \FixedSizeArray10.element5
            case 6:
                return \FixedSizeArray10.element6
            case 7:
                return \FixedSizeArray10.element7
            case 8:
                return \FixedSizeArray10.element8
            case 9:
                return \FixedSizeArray10.element9
            default:
                fatalError()
        }
    }
    
    public mutating func next() -> Element? {
        var iter_count = 0
        if iter_count == count {
            return nil
        }
        else {
            defer { iter_count += 1 }
            return self[keyPath: FixedSizeArray10.getKeypathToElement(index: iter_count)]
        }
    }
}

func initializeFixedSizeArray10() -> FixedSizeArray10<Double> {
    return FixedSizeArray10<Double>(element0: 0, element1: 1, element2: 2, element3: 3, element4: 4, element5: 5, element6: 6, element7: 7, element8: 8, element9: 9)
}
