// RUN: %target-swift-frontend -swift-version 4 -enforce-exclusivity=checked %s -emit-ir -module-name CurrentModule -D CURRENT_MODULE | %FileCheck %s --check-prefix=CHECK-COMMON --check-prefix=CHECK-CURRENT --check-prefix=CHECK-CURRENT-%target-ptrsize
// RUN: %target-swift-frontend -swift-version 4 -enforce-exclusivity=checked %s -emit-ir -module-name OriginalModule | %FileCheck %s --check-prefix=CHECK-COMMON --check-prefix=CHECK-ORIGINAL --check-prefix=CHECK-ORIGINAL-%target-ptrsize
// REQUIRES: OS=macosx

#if CURRENT_MODULE

@available(OSX 10.8, *)
@_originallyDefinedIn(module: "OriginalModule", macOS 10.15)
public struct Entity {
	public func addEntity(_ e: Entity) {}
	public func removeEntity(_ e: Entity) {}
}

@available(OSX 10.8, *)
@_originallyDefinedIn(module: "OriginalModule", macOS 10.15)
public protocol Movable {
	func MovableFuncFoo()
}

public protocol Unmoveable {}

@available(OSX 10.8, *)
@_originallyDefinedIn(module: "OriginalModule", macOS 10.15)
public class MovedClass: Movable, Unmoveable {
	public func MovableFuncFoo() {}
}

public class UnmovableClass {}

#else

public struct Entity {
	public func addEntity(_ e: Entity) {}
	public func removeEntity(_ e: Entity) {}
}

public protocol Movable {
	func MovableFuncFoo()
}

public protocol Unmoveable {}

public class MovedClass: Movable, Unmoveable {
	public func MovableFuncFoo() {}
}

public class UnmovableClass {}

#endif


func entityClient() {
	let root = Entity()
	// CHECK-COMMON: call swiftcc void @"$s14OriginalModule6EntityVACycfC"()
	let leaf = Entity()
	// CHECK-COMMON: call swiftcc void @"$s14OriginalModule6EntityVACycfC"()
	root.addEntity(leaf)
	// CHECK-COMMON: call swiftcc void @"$s14OriginalModule6EntityV03addC0yyACF"()
	let moved = MovedClass()
	// CHECK-COMMON: call swiftcc ptr @"$s14OriginalModule10MovedClassCACycfC"
	moved.MovableFuncFoo()
	// CHECK-COMMON: call swiftcc void @"$s14OriginalModule10MovedClassC14MovableFuncFooyyF"
}

public func unmovableClient() {
	let unmovable = UnmovableClass()
	// CHECK-CURRENT-64: call swiftcc %swift.metadata_response @"$s13CurrentModule14UnmovableClassCMa"(i64 0)
	// CHECK-ORIGINAL-64: call swiftcc %swift.metadata_response @"$s14OriginalModule14UnmovableClassCMa"(i64 0)
	// CHECK-CURRENT-32: call swiftcc %swift.metadata_response @"$s13CurrentModule14UnmovableClassCMa"(i32 0)
	// CHECK-ORIGINAL-32: call swiftcc %swift.metadata_response @"$s14OriginalModule14UnmovableClassCMa"(i32 0)
}

entityClient()
unmovableClient()
