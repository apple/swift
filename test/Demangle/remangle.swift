; This is not really a Swift source file: -*- Text -*-

%t.input: "A ---> B" ==> "A"
RUN: sed -ne '/--->/s/ *--->.*$//p' < %S/Inputs/manglings.txt > %t.input

RUN: swift-demangle -test-remangle < %t.input > %t.output
RUN: diff %t.input %t.output

// CHECK: Swift.(Mystruct in _7B40D7ED6632C2BEA2CA3BFFD57E3435)
RUN: swift-demangle -remangle-objc-rt '$ss8Mystruct33_7B40D7ED6632C2BEA2CA3BFFD57E3435LLV' | %FileCheck %s

// CHECK-OLD: r in _.?(Swift.UnsafeRawPointer) -> Swift.UnsafeRawPointer
RUN: swift-demangle -remangle-objc-rt '$s1_1?1PSVSVF1rP' | %FileCheck -check-prefix CHECK-OLD %s

// CHECK-OLD2: Swift.Int.related decl 'B' for protocol self-conformance descriptor for Swift.IteratorProtocol
RUN: swift-demangle -remangle-objc-rt '$sSiStMSLB_p' | %FileCheck -check-prefix CHECK-OLD2 %s

// CHECK-OLD3: Swift.related decl 'H' for partial apply forwarder
RUN: swift-demangle -remangle-objc-rt '$ssTALHP' | %FileCheck -check-prefix CHECK-OLD3 %s

// CHECK-OLD4: foobar in Swift.DefaultIndices.subscript : A
RUN: swift-demangle -remangle-objc-rt '$sSIxip6foobarP' | %FileCheck -check-prefix CHECK-OLD4 %s

// CHECK-GENERICEXT: Swift._ContiguousArrayStorage<(extension in Swift):Swift.FlattenSequence<StdlibCollectionUnittest.MinimalBidirectionalCollection<StdlibCollectionUnittest.MinimalBidirectionalCollection<Swift.Int>>>.Index>
RUN: swift-demangle -remangle-objc-rt '$ss23_ContiguousArrayStorageCys15FlattenSequenceVsE5IndexVy24StdlibCollectionUnittest020MinimalBidirectionalH0VyAIySiGG_GGD' | %FileCheck -check-prefix CHECK-GENERICEXT %s
