// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-silgen %s > %t/fragile-out.sil
// %FileCheck --check-prefix=POS %s < %t/fragile-out.sil
// %FileCheck --check-prefix=NEG %s < %t/fragile-out.sil
// RUN: %target-swift-emit-silgen -enable-library-evolution %s > %t/resilient-out.sil
// %FileCheck --check-prefix=POS %s < %t/resilient-out.sil
// %FileCheck --check-prefix=NEG %s < %t/resilient-out.sil

@frozen
public struct IsCopyable {
    public init() {}

    // Shouldn't get a property descriptor since property type is noncopyable
    // NEG-NOT: sil_property #IsCopyable.noncopyable
    public var noncopyable: IsntCopyable {
        get { IsntCopyable() }
        set { }
    }

    // Should get a property descriptor, copyable container and property
    // POS: sil_property #IsCopyable.copyable
    public var copyable: IsCopyable {
        get { IsCopyable() }
        set { }
    }

    // Shouldn't get a property descriptor since it's static
    // NEG-NOT: sil_property #IsCopyable.staticCopyable
    public static var staticCopyable: IsCopyable = IsCopyable()

    // Shouldn't get a property descriptor since it's static
    // NEG-NOT: sil_property #IsCopyable.staticNoncopyable
    public static var staticNoncopyable: IsntCopyable = IsntCopyable()
}

@frozen
public struct IsntCopyable: ~Copyable {
    public init() {}

    // Shouldn't get a property descriptor since container and property type are both noncopyable
    // NEG-NOT: sil_property #IsntCopyable.noncopyable
    public var noncopyable: IsntCopyable {
        get { IsntCopyable() }
        set { }
    }

    // Shouldn't get a property descriptor since container type is noncopyable
    // NEG-NOT: sil_property #IsntCopyable.copyable
    public var copyable: IsCopyable {
        get { IsCopyable() }
        set { }
    }

    // Shouldn't get a property descriptor since it's static
    // NEG-NOT: sil_property #IsntCopyable.staticCopyable
    public static var staticCopyable: IsCopyable = IsCopyable()

    // Shouldn't get a property descriptor since it's static
    // NEG-NOT: sil_property #IsntCopyable.staticNoncopyable
    public static var staticNoncopyable: IsntCopyable = IsntCopyable()
}

// Shouldn't get a property descriptor since it's global
// NEG-NOT: sil_property #{{.*}}globalCopyable
public var globalCopyable: IsCopyable = IsCopyable()

// Shouldn't get a property descriptor since it's global
// NEG-NOT: sil_property #{{.*}}globalNoncopyable
public var globalNoncopyable: IsntCopyable = IsntCopyable()
