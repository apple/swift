//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import Foundation // Clang module

#if os(macOS)

private let ε: CGFloat = 2.22045e-16

public struct AffineTransform : ReferenceConvertible, Hashable, CustomStringConvertible {
    public var m11, m12, m21, m22, tX, tY: CGFloat
    
    public typealias ReferenceType = NSAffineTransform
    
    /**
     Creates an affine transformation.
    */
    public init(m11: CGFloat, m12: CGFloat, m21: CGFloat, m22: CGFloat, tX: CGFloat, tY: CGFloat) {
        self.m11 = m11
        self.m12 = m12
        self.m21 = m21
        self.m22 = m22
        self.tX = tX
        self.tY = tY
    }
    
    private init(reference: __shared NSAffineTransform) {
        m11 = reference.transformStruct.m11
        m12 = reference.transformStruct.m12
        m21 = reference.transformStruct.m21
        m22 = reference.transformStruct.m22
        tX = reference.transformStruct.tX
        tY = reference.transformStruct.tY
    }
    
    private var reference: NSAffineTransform {
        let ref = NSAffineTransform()
        ref.transformStruct = NSAffineTransformStruct(m11: m11, m12: m12, m21: m21, m22: m22, tX: tX, tY: tY)
        return ref
    }
    
    /**
     Creates an affine transformation matrix with identity values.
     - seealso: identity
    */
    public init() {
        self.init(m11: CGFloat(1.0), m12: CGFloat(0.0),
                  m21: CGFloat(0.0), m22: CGFloat(1.0),
                  tX: CGFloat(0.0), tY: CGFloat(0.0))
    }
    
    /**
     Creates an affine transformation matrix from translation values.
     The matrix takes the following form:
     
         [ 1  0  0 ]
         [ 0  1  0 ]
         [ x  y  1 ]
     */
    public init(translationByX x: CGFloat, byY y: CGFloat) {
        self.init(m11: CGFloat(1.0), m12: CGFloat(0.0),
                  m21: CGFloat(0.0), m22: CGFloat(1.0),
                   tX: x,             tY: y)
    }
    
    /**
     Creates an affine transformation matrix from scaling values.
     The matrix takes the following form:
     
         [ x  0  0 ]
         [ 0  y  0 ]
         [ 0  0  1 ]
     */
    public init(scaleByX x: CGFloat, byY y: CGFloat) {
        self.init(m11: x,            m12: CGFloat(0.0),
                  m21: CGFloat(0.0), m22: y,
                   tX: CGFloat(0.0),  tY: CGFloat(0.0))
    }
    
    /**
     Creates an affine transformation matrix from scaling a single value.
     The matrix takes the following form:
     
         [ f  0  0 ]
         [ 0  f  0 ]
         [ 0  0  1 ]
     */
    public init(scale factor: CGFloat) {
        self.init(scaleByX: factor, byY: factor)
    }
    
    /**
     Creates an affine transformation matrix from rotation value (angle in radians).
     The matrix takes the following form:
     
         [  cos α   sin α  0 ]
         [ -sin α   cos α  0 ]
         [    0       0    1 ]
     */
    public init(rotationByRadians angle: CGFloat) {
        let α = Double(angle)
        
        let sine = CGFloat(sin(α))
        let cosine = CGFloat(cos(α))
        
        self.init(m11: cosine, m12: sine, m21: -sine, m22: cosine, tX: 0, tY: 0)
    }
    
    /**
     Creates an affine transformation matrix from a rotation value (angle in degrees).
     The matrix takes the following form:
     
         [  cos α   sin α  0 ]
         [ -sin α   cos α  0 ]
         [    0       0    1 ]
     */
    public init(rotationByDegrees angle: CGFloat) {
        let α = angle * .pi / 180.0
        self.init(rotationByRadians: α)
    }
    
    /**
     An identity affine transformation matrix
     
         [ 1  0  0 ]
         [ 0  1  0 ]
         [ 0  0  1 ]
     */
    public static let identity = AffineTransform(m11: 1, m12: 0, m21: 0, m22: 1, tX: 0, tY: 0)
    
    // Translating
    
    public mutating func translate(x: CGFloat, y: CGFloat) {
        tX += m11 * x + m21 * y
        tY += m12 * x + m22 * y
    }
    
    /**
     Mutates an affine transformation matrix from a rotation value (angle α in degrees).
     The matrix takes the following form:
     
         [  cos α   sin α  0 ]
         [ -sin α   cos α  0 ]
         [    0       0    1 ]
     */
    public mutating func rotate(byDegrees angle: CGFloat) {
        let α = angle * .pi / 180.0
        return rotate(byRadians: α)
    }
    
    /**
     Mutates an affine transformation matrix from a rotation value (angle α in radians).
     The matrix takes the following form:
     
         [  cos α   sin α  0 ]
         [ -sin α   cos α  0 ]
         [    0       0    1 ]
     */
    public mutating func rotate(byRadians angle: CGFloat) {
        let t2 = self
        let t1 = AffineTransform(rotationByRadians: angle)

        var t = AffineTransform.identity

        t.m11 = t1.m11 * t2.m11 + t1.m12 * t2.m21
        t.m12 = t1.m11 * t2.m12 + t1.m12 * t2.m22
        t.m21 = t1.m21 * t2.m11 + t1.m22 * t2.m21
        t.m22 = t1.m21 * t2.m12 + t1.m22 * t2.m22
        t.tX = t1.tX * t2.m11 + t1.tY * t2.m21 + t2.tX
        t.tY = t1.tX * t2.m12 + t1.tY * t2.m22 + t2.tY

        self = t
    }
    
    /**
     Creates an affine transformation matrix by combining the receiver with `transformStruct`.
     That is, it computes `T * M` and returns the result, where `T` is the receiver's and `M` is
     the `transformStruct`'s affine transformation matrix.
     The resulting matrix takes the following form:
     
                 [ m11_T  m12_T  0 ] [ m11_M  m12_M  0 ]
         T * M = [ m21_T  m22_T  0 ] [ m21_M  m22_M  0 ]
                 [  tX_T   tY_T  1 ] [  tX_M   tY_M  1 ]
     
                 [    (m11_T*m11_M + m12_T*m21_M)       (m11_T*m12_M + m12_T*m22_M)    0 ]
               = [    (m21_T*m11_M + m22_T*m21_M)       (m21_T*m12_M + m22_T*m22_M)    0 ]
                 [ (tX_T*m11_M + tY_T*m21_M + tX_M)  (tX_T*m12_M + tY_T*m22_M + tY_M)  1 ]
     */
    private func concatenated(_ m: AffineTransform) -> AffineTransform {
        // this could be optimized with a vector version
        return AffineTransform(
            m11: (self.m11 * m.m11) + (self.m12 * m.m21), m12: (self.m11 * m.m12) + (self.m12 * m.m22),
            m21: (self.m21 * m.m11) + (self.m22 * m.m21), m22: (self.m21 * m.m12) + (self.m22 * m.m22),
            tX: (self.tX * m.m11) + (self.tY * m.m21) + m.tX,
            tY: (self.tX * m.m12) + (self.tY * m.m22) + m.tY
        )
    }
    
    // Scaling
    public mutating func scale(_ scale: CGFloat) {
        self.scale(x: scale, y: scale)
    }
    
    public mutating func scale(x: CGFloat, y: CGFloat) {
        m11 *= x
        m12 *= x
        m21 *= y
        m22 *= y
    }
    
    /**
     Inverts the transformation matrix if possible. Matrices with a determinant that is less than
     the smallest valid representation of a double value greater than zero are considered to be 
     invalid for representing as an inverse. If the input AffineTransform can potentially fall into
     this case then the inverted() method is suggested to be used instead since that will return
     an optional value that will be nil in the case that the matrix cannot be inverted.
     
     D = (m11 * m22) - (m12 * m21)
     
     D < ε the inverse is undefined and will be nil
    */
    public mutating func invert() {
        guard let inverse = inverted() else {
            fatalError("Transform has no inverse")
        }
        self = inverse
    }
    
    public func inverted() -> AffineTransform? {
        let determinant = (m11 * m22) - (m12 * m21)
        if abs(determinant) <= ε {
            return nil
        }
        var inverse = AffineTransform()
        inverse.m11 = m22 / determinant
        inverse.m12 = -m12 / determinant
        inverse.m21 = -m21 / determinant
        inverse.m22 = m11 / determinant
        inverse.tX = (m21 * tY - m22 * tX) / determinant
        inverse.tY = (m12 * tX - m11 * tY) / determinant
        return inverse
    }
    
    // Transforming with transform
    public mutating func append(_ transform: AffineTransform) {
        self = concatenated(transform)
    }
    
    public mutating func prepend(_ transform: AffineTransform) {
        self = transform.concatenated(self)
    }
    
    // Transforming points and sizes
    public func transform(_ point: NSPoint) -> NSPoint {
        var newPoint = NSPoint()
        newPoint.x = (m11 * point.x) + (m21 * point.y) + tX
        newPoint.y = (m12 * point.x) + (m22 * point.y) + tY
        return newPoint
    }
    
    public func transform(_ size: NSSize) -> NSSize {
        var newSize = NSSize()
        newSize.width = (m11 * size.width) + (m21 * size.height)
        newSize.height = (m12 * size.width) + (m22 * size.height)
        return newSize
    }
    
    public func hash(into hasher: inout Hasher) {
        hasher.combine(m11)
        hasher.combine(m12)
        hasher.combine(m21)
        hasher.combine(m22)
        hasher.combine(tX)
        hasher.combine(tY)
    }
    
    public var description: String {
        return "{m11:\(m11), m12:\(m12), m21:\(m21), m22:\(m22), tX:\(tX), tY:\(tY)}"
    }
    
    public var debugDescription: String {
        return description
    }

    public static func ==(lhs: AffineTransform, rhs: AffineTransform) -> Bool {
        return lhs.m11 == rhs.m11 && lhs.m12 == rhs.m12 &&
               lhs.m21 == rhs.m21 && lhs.m22 == rhs.m22 &&
               lhs.tX == rhs.tX && lhs.tY == rhs.tY
    }

}

extension AffineTransform : _ObjectiveCBridgeable {
    public static func _getObjectiveCType() -> Any.Type {
        return NSAffineTransform.self
    }
    
    @_semantics("convertToObjectiveC")
    public func _bridgeToObjectiveC() -> NSAffineTransform {
        return self.reference
    }
    
    public static func _forceBridgeFromObjectiveC(_ x: NSAffineTransform, result: inout AffineTransform?) {
        if !_conditionallyBridgeFromObjectiveC(x, result: &result) {
            fatalError("Unable to bridge type")
        }
    }
    
    public static func _conditionallyBridgeFromObjectiveC(_ x: NSAffineTransform, result: inout AffineTransform?) -> Bool {
        result = AffineTransform(reference: x)
        return true // Can't fail
    }

    @_effects(readonly)
    public static func _unconditionallyBridgeFromObjectiveC(_ x: NSAffineTransform?) -> AffineTransform {
        guard let src = x else { return AffineTransform.identity }
        return AffineTransform(reference: src)
    }
}

extension NSAffineTransform : _HasCustomAnyHashableRepresentation {
    // Must be @nonobjc to avoid infinite recursion during bridging.
    @nonobjc
    public func _toCustomAnyHashable() -> AnyHashable? {
        return AnyHashable(self as AffineTransform)
    }
}

extension AffineTransform : Codable {
    public init(from decoder: Decoder) throws {
        var container = try decoder.unkeyedContainer()
        m11 = try container.decode(CGFloat.self)
        m12 = try container.decode(CGFloat.self)
        m21 = try container.decode(CGFloat.self)
        m22 = try container.decode(CGFloat.self)
        tX  = try container.decode(CGFloat.self)
        tY  = try container.decode(CGFloat.self)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.unkeyedContainer()
        try container.encode(self.m11)
        try container.encode(self.m12)
        try container.encode(self.m21)
        try container.encode(self.m22)
        try container.encode(self.tX)
        try container.encode(self.tY)
    }
}

#endif
