
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// RUN: %empty-directory(%t)
//
// RUN: %target-clang %S/Inputs/FoundationBridge/FoundationBridge.m -c -o %t/FoundationBridgeObjC.o -g
// RUN: %target-build-swift %s -I %S/Inputs/FoundationBridge/ -Xlinker %t/FoundationBridgeObjC.o -o %t/TestData
// RUN: %target-codesign %t/TestData

// RUN: %target-run %t/TestData
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation
import Dispatch
import ObjectiveC
import FoundationBridgeObjC

#if FOUNDATION_XCTEST
import XCTest
class TestDataSuper : XCTestCase { }
#else
import StdlibUnittest
class TestDataSuper { }
#endif

class TestData : TestDataSuper {

    class AllOnesImmutableData : NSData {
        private var _length : Int
        var _pointer : UnsafeMutableBufferPointer<UInt8>? {
            willSet {
                if let p = _pointer { free(p.baseAddress) }
            }
        }
        
        init(length : Int) {
            _length = length
            super.init()
        }
        
        required init?(coder aDecoder: NSCoder) {
            // Not tested
            fatalError()
        }
        
        deinit {
            if let p = _pointer {
                free(p.baseAddress)
            }
        }
        
        override var length : Int {
            get {
                return _length
            }
        }
        
        override var bytes : UnsafeRawPointer {
            if let d = _pointer {
                return UnsafeRawPointer(d.baseAddress!)
            } else {
                // Need to allocate the buffer now.
                // It doesn't matter if the buffer is uniquely referenced or not here.
                let buffer = malloc(length)
                memset(buffer, 1, length)
                let bytePtr = buffer!.bindMemory(to: UInt8.self, capacity: length)
                let result = UnsafeMutableBufferPointer(start: bytePtr, count: length)
                _pointer = result
                return UnsafeRawPointer(result.baseAddress!)
            }
        }
        
        override func getBytes(_ buffer: UnsafeMutableRawPointer, length: Int) {
            if let d = _pointer {
                // Get the real data from the buffer
                memmove(buffer, d.baseAddress, length)
            } else {
                // A more efficient implementation of getBytes in the case where no one has asked for our backing bytes
                memset(buffer, 1, length)
            }
        }
        
        override func copy(with zone: NSZone? = nil) -> Any {
            return self
        }
        
        override func mutableCopy(with zone: NSZone? = nil) -> Any {
            return AllOnesData(length: _length)
        }
    }
    
    
    class AllOnesData : NSMutableData {
        
        private var _length : Int
        var _pointer : UnsafeMutableBufferPointer<UInt8>? {
            willSet {
                if let p = _pointer { free(p.baseAddress) }
            }
        }
        
        override init(length : Int) {
            _length = length
            super.init()
        }
        
        required init?(coder aDecoder: NSCoder) {
            // Not tested
            fatalError()
        }
        
        deinit {
            if let p = _pointer {
                free(p.baseAddress)
            }
        }
        
        override var length : Int {
            get {
                return _length
            }
            set {
                if let ptr = _pointer {
                    // Copy the data to our new length buffer
                    let newBuffer = malloc(newValue)!
                    if newValue <= _length {
                        memmove(newBuffer, ptr.baseAddress, newValue)
                    } else if newValue > _length {
                        memmove(newBuffer, ptr.baseAddress, _length)
                        memset(newBuffer + _length, 1, newValue - _length)
                    }
                    let bytePtr = newBuffer.bindMemory(to: UInt8.self, capacity: newValue)
                    _pointer = UnsafeMutableBufferPointer(start: bytePtr, count: newValue)
                }
                _length = newValue
            }
        }
        
        override var bytes : UnsafeRawPointer {
            if let d = _pointer {
                return UnsafeRawPointer(d.baseAddress!)
            } else {
                // Need to allocate the buffer now.
                // It doesn't matter if the buffer is uniquely referenced or not here.
                let buffer = malloc(length)
                memset(buffer, 1, length)
                let bytePtr = buffer!.bindMemory(to: UInt8.self, capacity: length)
                let result = UnsafeMutableBufferPointer(start: bytePtr, count: length)
                _pointer = result
                return UnsafeRawPointer(result.baseAddress!)
            }
        }
        
        override var mutableBytes: UnsafeMutableRawPointer {
            let newBufferLength = _length
            let newBuffer = malloc(newBufferLength)
            if let ptr = _pointer {
                // Copy the existing data to the new box, then return its pointer
                memmove(newBuffer, ptr.baseAddress, newBufferLength)
            } else {
                // Set new data to 1s
                memset(newBuffer, 1, newBufferLength)
            }
            let bytePtr = newBuffer!.bindMemory(to: UInt8.self, capacity: newBufferLength)
            let result = UnsafeMutableBufferPointer(start: bytePtr, count: newBufferLength)
            _pointer = result
            _length = newBufferLength
            return UnsafeMutableRawPointer(result.baseAddress!)
        }
        
        override func getBytes(_ buffer: UnsafeMutableRawPointer, length: Int) {
            if let d = _pointer {
                // Get the real data from the buffer
                memmove(buffer, d.baseAddress, length)
            } else {
                // A more efficient implementation of getBytes in the case where no one has asked for our backing bytes
                memset(buffer, 1, length)
            }
        }
    }

    var heldData: Data?
    
    // this holds a reference while applying the function which forces the internal ref type to become non-uniquely referenced
    func holdReference(_ data: Data, apply: () -> Void) {
        heldData = data
        apply()
        heldData = nil
    }

    // MARK: -
    
    // String of course has its own way to get data, but this way tests our own data struct
    func dataFrom(_ string : String) -> Data {
        // Create a Data out of those bytes
        return string.utf8CString.withUnsafeBufferPointer { (ptr) in
            ptr.baseAddress!.withMemoryRebound(to: UInt8.self, capacity: ptr.count) {
                // Subtract 1 so we don't get the null terminator byte. This matches NSString behavior.
                return Data(bytes: $0, count: ptr.count - 1)
            }
        }
    }
    
    // MARK: -
    
    func testBasicConstruction() {

        // Make sure that we were able to create some data
        let hello = dataFrom("hello")
        let helloLength = hello.count
        expectEqual(hello[0], 0x68, "Unexpected first byte")
        
        let world = dataFrom(" world")
        var helloWorld = hello
        world.withUnsafeBytes {
            helloWorld.append($0, count: world.count)
        }
                
        expectEqual(hello[0], 0x68, "First byte should not have changed")
        expectEqual(hello.count, helloLength, "Length of first data should not have changed")
        expectEqual(helloWorld.count, hello.count + world.count, "The total length should include both buffers")
    }
    
    func testInitializationWithArray() {
        let data = Data(bytes: [1, 2, 3])
        expectEqual(3, data.count)
        
        let data2 = Data(bytes: [1, 2, 3].filter { $0 >= 2 })
        expectEqual(2, data2.count)
        
        let data3 = Data(bytes: [1, 2, 3, 4, 5][1..<3])
        expectEqual(2, data3.count)
    }

    func testInitializationWithBufferPointer() {
        let nilBuffer = UnsafeBufferPointer<UInt8>(start: nil, count: 0)
        let data = Data(buffer: nilBuffer)
        expectEqual(data, Data())

        let validPointer = UnsafeMutablePointer<UInt8>.allocate(capacity: 2)
        validPointer[0] = 0xCA
        validPointer[1] = 0xFE
        defer { validPointer.deallocate() }

        let emptyBuffer = UnsafeBufferPointer<UInt8>(start: validPointer, count: 0)
        let data2 = Data(buffer: emptyBuffer)
        expectEqual(data2, Data())

        let shortBuffer = UnsafeBufferPointer<UInt8>(start: validPointer, count: 1)
        let data3 = Data(buffer: shortBuffer)
        expectEqual(data3, Data([0xCA]))

        let fullBuffer = UnsafeBufferPointer<UInt8>(start: validPointer, count: 2)
        let data4 = Data(buffer: fullBuffer)
        expectEqual(data4, Data([0xCA, 0xFE]))

        let tuple: (UInt16, UInt16, UInt16, UInt16) = (0xFF, 0xFE, 0xFD, 0xFC)
        withUnsafeBytes(of: tuple) {
            // If necessary, port this to big-endian.
            let tupleBuffer: UnsafeBufferPointer<UInt8> = $0.bindMemory(to: UInt8.self)
            let data5 = Data(buffer: tupleBuffer)
            expectEqual(data5, Data([0xFF, 0x00, 0xFE, 0x00, 0xFD, 0x00, 0xFC, 0x00]))
        }
    }

    func testInitializationWithMutableBufferPointer() {
        let nilBuffer = UnsafeMutableBufferPointer<UInt8>(start: nil, count: 0)
        let data = Data(buffer: nilBuffer)
        expectEqual(data, Data())

        let validPointer = UnsafeMutablePointer<UInt8>.allocate(capacity: 2)
        validPointer[0] = 0xCA
        validPointer[1] = 0xFE
        defer { validPointer.deallocate() }

        let emptyBuffer = UnsafeMutableBufferPointer<UInt8>(start: validPointer, count: 0)
        let data2 = Data(buffer: emptyBuffer)
        expectEqual(data2, Data())

        let shortBuffer = UnsafeMutableBufferPointer<UInt8>(start: validPointer, count: 1)
        let data3 = Data(buffer: shortBuffer)
        expectEqual(data3, Data([0xCA]))

        let fullBuffer = UnsafeMutableBufferPointer<UInt8>(start: validPointer, count: 2)
        let data4 = Data(buffer: fullBuffer)
        expectEqual(data4, Data([0xCA, 0xFE]))

        var tuple: (UInt16, UInt16, UInt16, UInt16) = (0xFF, 0xFE, 0xFD, 0xFC)
        withUnsafeMutableBytes(of: &tuple) {
            // If necessary, port this to big-endian.
            let tupleBuffer: UnsafeMutableBufferPointer<UInt8> = $0.bindMemory(to: UInt8.self)
            let data5 = Data(buffer: tupleBuffer)
            expectEqual(data5, Data([0xFF, 0x00, 0xFE, 0x00, 0xFD, 0x00, 0xFC, 0x00]))
        }
    }
    
    func testMutableData() {
        let hello = dataFrom("hello")
        let helloLength = hello.count
        expectEqual(hello[0], 0x68, "Unexpected first byte")

        // Double the length
        var mutatingHello = hello
        mutatingHello.count *= 2
        
        expectEqual(hello.count, helloLength, "The length of the initial data should not have changed")
        expectEqual(mutatingHello.count, helloLength * 2, "The length should have changed")
        
        // Get the underlying data for hello2
        mutatingHello.withUnsafeMutableBytes { (bytes : UnsafeMutablePointer<UInt8>) in
            expectEqual(bytes.pointee, 0x68, "First byte should be 0x68")
            
            // Mutate it
            bytes.pointee = 0x67
            expectEqual(bytes.pointee, 0x67, "First byte should be 0x67")

            // Verify that the first data is still correct
            expectEqual(hello[0], 0x68, "The first byte should still be 0x68")
        }
    }
    
    func testCustomData() {
        let length = 5
        let allOnesData = Data(referencing: AllOnesData(length: length))
        expectEqual(1, allOnesData[0], "First byte of all 1s data should be 1")
        
        // Double the length
        var allOnesCopyToMutate = allOnesData
        allOnesCopyToMutate.count = allOnesData.count * 2
        
        expectEqual(allOnesData.count, length, "The length of the initial data should not have changed")
        expectEqual(allOnesCopyToMutate.count, length * 2, "The length should have changed")
        
        // Force the second data to create its storage
        allOnesCopyToMutate.withUnsafeMutableBytes { (bytes : UnsafeMutablePointer<UInt8>) in
            expectEqual(bytes.pointee, 1, "First byte should be 1")
            
            // Mutate the second data
            bytes.pointee = 0
            expectEqual(bytes.pointee, 0, "First byte should be 0")
            
            // Verify that the first data is still 1
            expectEqual(allOnesData[0], 1, "The first byte should still be 1")
        }
        
    }
    
    func testBridgingDefault() {
        let hello = dataFrom("hello")
        // Convert from struct Data to NSData
        if let s = NSString(data: hello, encoding: String.Encoding.utf8.rawValue) {
            expectTrue(s.isEqual(to: "hello"), "The strings should be equal")
        }
        
        // Convert from NSData to struct Data
        let goodbye = dataFrom("goodbye")
        if let resultingData = NSString(string: "goodbye").data(using: String.Encoding.utf8.rawValue) {
            expectEqual(resultingData[0], goodbye[0], "First byte should be equal")
        }
    }
    
    func testBridgingMutable() {
        // Create a mutable data
        var helloWorld = dataFrom("hello")
        helloWorld.append(dataFrom("world"))
        
        // Convert from struct Data to NSData
        if let s = NSString(data: helloWorld, encoding: String.Encoding.utf8.rawValue) {
            expectTrue(s.isEqual(to: "helloworld"), "The strings should be equal")
        }

    }
    
    func testBridgingCustom() {
        // Let's use an AllOnesData with some Objective-C code
        let allOnes = AllOnesData(length: 64)
        
        // Type-erased
        let data = Data(referencing: allOnes)
        
        // Create a home for our test data
        let dirPath = (NSTemporaryDirectory() as NSString).appendingPathComponent(NSUUID().uuidString)
        try! FileManager.default.createDirectory(atPath: dirPath, withIntermediateDirectories: true, attributes: nil)
        let filePath = (dirPath as NSString).appendingPathComponent("temp_file")
        guard FileManager.default.createFile(atPath: filePath, contents: nil, attributes: nil) else { expectTrue(false, "Unable to create temporary file"); return}
        guard let fh = FileHandle(forWritingAtPath: filePath) else { expectTrue(false, "Unable to open temporary file"); return }
        defer { try! FileManager.default.removeItem(atPath: dirPath) }
        
        // Now use this data with some Objective-C code that takes NSData arguments
        fh.write(data)
        
        // Get the data back
        do {
            let url = URL(fileURLWithPath: filePath)
            let readData = try Data.init(contentsOf: url)
            expectEqual(data.count, readData.count, "The length of the data is not the same")
        } catch {
            expectTrue(false, "Unable to read back data")
            return
        }
    }
    
    func testEquality() {
        let d1 = dataFrom("hello")
        let d2 = dataFrom("hello")
        
        // Use == explicitly here to make sure we're calling the right methods
        expectTrue(d1 == d2, "Data should be equal")
    }
    
    func testDataInSet() {
        let d1 = dataFrom("Hello")
        let d2 = dataFrom("Hello")
        let d3 = dataFrom("World")
        
        var s = Set<Data>()
        s.insert(d1)
        s.insert(d2)
        s.insert(d3)
        
        expectEqual(s.count, 2, "Expected only two entries in the Set")
    }
    
    func testReplaceSubrange() {
        var hello = dataFrom("Hello")
        let world = dataFrom("World")
        
        hello[0] = world[0]
        expectEqual(hello[0], world[0])
        
        var goodbyeWorld = dataFrom("Hello World")
        let goodbye = dataFrom("Goodbye")
        let expected = dataFrom("Goodbye World")
        
        goodbyeWorld.replaceSubrange(0..<5, with: goodbye)
        expectEqual(goodbyeWorld, expected)
    }
    
    func testReplaceSubrange2() {
        let hello = dataFrom("Hello")
        let world = dataFrom(" World")
        let goodbye = dataFrom("Goodbye")
        let expected = dataFrom("Goodbye World")
        
        var mutateMe = hello
        mutateMe.append(world)

        if let found = mutateMe.range(of: hello) {
            mutateMe.replaceSubrange(found, with: goodbye)
        }
        expectEqual(mutateMe, expected)
    }
    
    func testReplaceSubrange3() {
        // The expected result
        let expectedBytes : [UInt8] = [1, 2, 9, 10, 11, 12, 13]
        let expected = expectedBytes.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        
        // The data we'll mutate
        let someBytes : [UInt8] = [1, 2, 3, 4, 5]
        var a = someBytes.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        
        // The bytes we'll insert
        let b : [UInt8] = [9, 10, 11, 12, 13]
        b.withUnsafeBufferPointer {
            a.replaceSubrange(2..<5, with: $0)
        }
        expectEqual(expected, a)
    }
    
    func testReplaceSubrange4() {
        let expectedBytes : [UInt8] = [1, 2, 9, 10, 11, 12, 13]
        let expected = Data(bytes: expectedBytes)
        
        // The data we'll mutate
        let someBytes : [UInt8] = [1, 2, 3, 4, 5]
        var a = Data(bytes: someBytes)
        
        // The bytes we'll insert
        let b : [UInt8] = [9, 10, 11, 12, 13]
        a.replaceSubrange(2..<5, with: b)
        expectEqual(expected, a)
    }
    
    func testReplaceSubrange5() {
        var d = Data(bytes: [1, 2, 3])
        d.replaceSubrange(0..<0, with: [4])
        expectEqual(Data(bytes: [4, 1, 2, 3]), d)
        
        d.replaceSubrange(0..<4, with: [9])
        expectEqual(Data(bytes: [9]), d)
        
        d.replaceSubrange(0..<d.count, with: [])
        expectEqual(Data(), d)
        
        d.replaceSubrange(0..<0, with: [1, 2, 3, 4])
        expectEqual(Data(bytes: [1, 2, 3, 4]), d)
        
        d.replaceSubrange(1..<3, with: [9, 8])
        expectEqual(Data(bytes: [1, 9, 8, 4]), d)
        
        d.replaceSubrange(d.count..<d.count, with: [5])
        expectEqual(Data(bytes: [1, 9, 8, 4, 5]), d)
    }

    func testRange() {
        let helloWorld = dataFrom("Hello World")
        let goodbye = dataFrom("Goodbye")
        let hello = dataFrom("Hello")

        do {
            let found = helloWorld.range(of: goodbye)
            expectNil(found)
        }

        do {
            let found = helloWorld.range(of: goodbye, options: .anchored)
            expectNil(found)
        }

        do {
            let found = helloWorld.range(of: hello, in: 7..<helloWorld.count)
            expectNil(found)
        }
    }
    
    func testInsertData() {
        let hello = dataFrom("Hello")
        let world = dataFrom(" World")
        let expected = dataFrom("Hello World")
        var helloWorld = dataFrom("")
        
        helloWorld.replaceSubrange(0..<0, with: world)
        helloWorld.replaceSubrange(0..<0, with: hello)
        
        expectEqual(helloWorld, expected)
    }
    
    func testLoops() {
        let hello = dataFrom("Hello")
        var count = 0
        for _ in hello {
            count += 1
        }
        expectEqual(count, 5)        
    }
    
    func testGenericAlgorithms() {
        let hello = dataFrom("Hello World")
        
        let isCapital = { (byte : UInt8) in byte >= 65 && byte <= 90 }
        
        let allCaps = hello.filter(isCapital)
        expectEqual(allCaps.count, 2)
        
        let capCount = hello.reduce(0) { isCapital($1) ? $0 + 1 : $0 }
        expectEqual(capCount, 2)
        
        let allLower = hello.map { isCapital($0) ? $0 + 31 : $0 }
        expectEqual(allLower.count, hello.count)
    }
    
    func testCustomDeallocator() {
        var deallocatorCalled = false
        
        // Scope the data to a block to control lifecycle
        autoreleasepool {
            let buffer = malloc(16)!
            let bytePtr = buffer.bindMemory(to: UInt8.self, capacity: 16)
            var data = Data(bytesNoCopy: bytePtr, count: 16, deallocator: .custom({ (ptr, size) in
                deallocatorCalled = true
                free(UnsafeMutableRawPointer(ptr))
            }))
            // Use the data
            data[0] = 1
         }
        
        expectTrue(deallocatorCalled, "Custom deallocator was never called")
    }
    
    func testCopyBytes() {
        let c = 10
        let underlyingBuffer = malloc(c * MemoryLayout<UInt16>.stride)!
        let u16Ptr = underlyingBuffer.bindMemory(to: UInt16.self, capacity: c)
        let buffer = UnsafeMutableBufferPointer<UInt16>(start: u16Ptr, count: c)
        
        buffer[0] = 0
        buffer[1] = 0
        
        var data = Data(capacity: c * MemoryLayout<UInt16>.stride)
        data.resetBytes(in: 0..<c * MemoryLayout<UInt16>.stride)
        data[0] = 0xFF
        data[1] = 0xFF
        let copiedCount = data.copyBytes(to: buffer)
        expectEqual(copiedCount, c * MemoryLayout<UInt16>.stride)
        
        expectEqual(buffer[0], 0xFFFF)
        free(underlyingBuffer)
    }
    
    func testCopyBytes_undersized() {
        let a : [UInt8] = [1, 2, 3, 4, 5]
        var data = a.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        let expectedSize = MemoryLayout<UInt8>.stride * a.count
        expectEqual(expectedSize, data.count)
        
        let underlyingBuffer = unsafeBitCast(malloc(expectedSize - 1)!, to: UnsafeMutablePointer<UInt8>.self)
        let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: expectedSize - 1)
        
        // We should only copy in enough bytes that can fit in the buffer
        let copiedCount = data.copyBytes(to: buffer)
        expectEqual(expectedSize - 1, copiedCount)
        
        var index = 0
        for v in a[0..<expectedSize-1] {
            expectEqual(v, buffer[index])
            index += 1
        }
        
        free(underlyingBuffer)
    }

    func testCopyBytes_oversized() {
        let a : [Int32] = [1, 0, 1, 0, 1]
        var data = a.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        let expectedSize = MemoryLayout<Int32>.stride * a.count
        expectEqual(expectedSize, data.count)
        
        let underlyingBuffer = unsafeBitCast(malloc(expectedSize + 1)!, to: UnsafeMutablePointer<UInt8>.self)
        let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: expectedSize + 1)
        
        let copiedCount = data.copyBytes(to: buffer)
        expectEqual(expectedSize, copiedCount)

        free(underlyingBuffer)
    }

    func testCopyBytes_ranges() {
        
        do {
            // Equal sized buffer, data
            let a : [UInt8] = [1, 2, 3, 4, 5]
            var data = a.withUnsafeBufferPointer {
                return Data(buffer: $0)
            }
            
            let underlyingBuffer = unsafeBitCast(malloc(data.count)!, to: UnsafeMutablePointer<UInt8>.self)
            let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: data.count)
            
            var copiedCount : Int
            
            copiedCount = data.copyBytes(to: buffer, from: 0..<0)
            expectEqual(0, copiedCount)
            
            copiedCount = data.copyBytes(to: buffer, from: 1..<1)
            expectEqual(0, copiedCount)
            
            copiedCount = data.copyBytes(to: buffer, from: 0..<3)
            expectEqual((0..<3).count, copiedCount)
            
            var index = 0
            for v in a[0..<3] {
                expectEqual(v, buffer[index])
                index += 1
            }
            free(underlyingBuffer)
        }
        
        do {
            // Larger buffer than data
            let a : [UInt8] = [1, 2, 3, 4]
            let data = a.withUnsafeBufferPointer {
                return Data(buffer: $0)
            }
            
            let underlyingBuffer = unsafeBitCast(malloc(10)!, to: UnsafeMutablePointer<UInt8>.self)
            let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: 10)
            
            var copiedCount : Int
            
            copiedCount = data.copyBytes(to: buffer, from: 0..<3)
            expectEqual((0..<3).count, copiedCount)
            
            var index = 0
            for v in a[0..<3] {
                expectEqual(v, buffer[index])
                index += 1
            }
            free(underlyingBuffer)
        }
        
        do {
            // Larger data than buffer
            let a : [UInt8] = [1, 2, 3, 4, 5, 6]
            let data = a.withUnsafeBufferPointer {
                return Data(buffer: $0)
            }
            
            let underlyingBuffer = unsafeBitCast(malloc(4)!, to: UnsafeMutablePointer<UInt8>.self)
            let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: 4)
            
            var copiedCount : Int
            
            copiedCount = data.copyBytes(to: buffer, from: 0..<data.index(before: data.endIndex))
            expectEqual(4, copiedCount)
            
            var index = 0
            for v in a[0..<4] {
                expectEqual(v, buffer[index])
                index += 1
            }
            free(underlyingBuffer)

        }
    }

    func test_base64Data_small() {
        let data = "Hello World".data(using: .utf8)!
        let base64 = data.base64EncodedString()
        expectEqual("SGVsbG8gV29ybGQ=", base64, "trivial base64 conversion should work")
    }

    func test_base64Data_medium() {
        let data = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Ut at tincidunt arcu. Suspendisse nec sodales erat, sit amet imperdiet ipsum. Etiam sed ornare felis. Nunc mauris turpis, bibendum non lectus quis, malesuada placerat turpis. Nam adipiscing non massa et semper. Nulla convallis semper bibendum. Aliquam dictum nulla cursus mi ultricies, at tincidunt mi sagittis. Nulla faucibus at dui quis sodales. Morbi rutrum, dui id ultrices venenatis, arcu urna egestas felis, vel suscipit mauris arcu quis risus. Nunc venenatis ligula at orci tristique, et mattis purus pulvinar. Etiam ultricies est odio. Nunc eleifend malesuada justo, nec euismod sem ultrices quis. Etiam nec nibh sit amet lorem faucibus dapibus quis nec leo. Praesent sit amet mauris vel lacus hendrerit porta mollis consectetur mi. Donec eget tortor dui. Morbi imperdiet, arcu sit amet elementum interdum, quam nisl tempor quam, vitae feugiat augue purus sed lacus. In ac urna adipiscing purus venenatis volutpat vel et metus. Nullam nec auctor quam. Phasellus porttitor felis ac nibh gravida suscipit tempus at ante. Nunc pellentesque iaculis sapien a mattis. Aenean eleifend dolor non nunc laoreet, non dictum massa aliquam. Aenean quis turpis augue. Praesent augue lectus, mollis nec elementum eu, dignissim at velit. Ut congue neque id ullamcorper pellentesque. Maecenas euismod in elit eu vehicula. Nullam tristique dui nulla, nec convallis metus suscipit eget. Cras semper augue nec cursus blandit. Nulla rhoncus et odio quis blandit. Praesent lobortis dignissim velit ut pulvinar. Duis interdum quam adipiscing dolor semper semper. Nunc bibendum convallis dui, eget mollis magna hendrerit et. Morbi facilisis, augue eu fringilla convallis, mauris est cursus dolor, eu posuere odio nunc quis orci. Ut eu justo sem. Phasellus ut erat rhoncus, faucibus arcu vitae, vulputate erat. Aliquam nec magna viverra, interdum est vitae, rhoncus sapien. Duis tincidunt tempor ipsum ut dapibus. Nullam commodo varius metus, sed sollicitudin eros. Etiam nec odio et dui tempor blandit posuere.".data(using: .utf8)!
        let base64 = data.base64EncodedString()
        expectEqual("TG9yZW0gaXBzdW0gZG9sb3Igc2l0IGFtZXQsIGNvbnNlY3RldHVyIGFkaXBpc2NpbmcgZWxpdC4gVXQgYXQgdGluY2lkdW50IGFyY3UuIFN1c3BlbmRpc3NlIG5lYyBzb2RhbGVzIGVyYXQsIHNpdCBhbWV0IGltcGVyZGlldCBpcHN1bS4gRXRpYW0gc2VkIG9ybmFyZSBmZWxpcy4gTnVuYyBtYXVyaXMgdHVycGlzLCBiaWJlbmR1bSBub24gbGVjdHVzIHF1aXMsIG1hbGVzdWFkYSBwbGFjZXJhdCB0dXJwaXMuIE5hbSBhZGlwaXNjaW5nIG5vbiBtYXNzYSBldCBzZW1wZXIuIE51bGxhIGNvbnZhbGxpcyBzZW1wZXIgYmliZW5kdW0uIEFsaXF1YW0gZGljdHVtIG51bGxhIGN1cnN1cyBtaSB1bHRyaWNpZXMsIGF0IHRpbmNpZHVudCBtaSBzYWdpdHRpcy4gTnVsbGEgZmF1Y2lidXMgYXQgZHVpIHF1aXMgc29kYWxlcy4gTW9yYmkgcnV0cnVtLCBkdWkgaWQgdWx0cmljZXMgdmVuZW5hdGlzLCBhcmN1IHVybmEgZWdlc3RhcyBmZWxpcywgdmVsIHN1c2NpcGl0IG1hdXJpcyBhcmN1IHF1aXMgcmlzdXMuIE51bmMgdmVuZW5hdGlzIGxpZ3VsYSBhdCBvcmNpIHRyaXN0aXF1ZSwgZXQgbWF0dGlzIHB1cnVzIHB1bHZpbmFyLiBFdGlhbSB1bHRyaWNpZXMgZXN0IG9kaW8uIE51bmMgZWxlaWZlbmQgbWFsZXN1YWRhIGp1c3RvLCBuZWMgZXVpc21vZCBzZW0gdWx0cmljZXMgcXVpcy4gRXRpYW0gbmVjIG5pYmggc2l0IGFtZXQgbG9yZW0gZmF1Y2lidXMgZGFwaWJ1cyBxdWlzIG5lYyBsZW8uIFByYWVzZW50IHNpdCBhbWV0IG1hdXJpcyB2ZWwgbGFjdXMgaGVuZHJlcml0IHBvcnRhIG1vbGxpcyBjb25zZWN0ZXR1ciBtaS4gRG9uZWMgZWdldCB0b3J0b3IgZHVpLiBNb3JiaSBpbXBlcmRpZXQsIGFyY3Ugc2l0IGFtZXQgZWxlbWVudHVtIGludGVyZHVtLCBxdWFtIG5pc2wgdGVtcG9yIHF1YW0sIHZpdGFlIGZldWdpYXQgYXVndWUgcHVydXMgc2VkIGxhY3VzLiBJbiBhYyB1cm5hIGFkaXBpc2NpbmcgcHVydXMgdmVuZW5hdGlzIHZvbHV0cGF0IHZlbCBldCBtZXR1cy4gTnVsbGFtIG5lYyBhdWN0b3IgcXVhbS4gUGhhc2VsbHVzIHBvcnR0aXRvciBmZWxpcyBhYyBuaWJoIGdyYXZpZGEgc3VzY2lwaXQgdGVtcHVzIGF0IGFudGUuIE51bmMgcGVsbGVudGVzcXVlIGlhY3VsaXMgc2FwaWVuIGEgbWF0dGlzLiBBZW5lYW4gZWxlaWZlbmQgZG9sb3Igbm9uIG51bmMgbGFvcmVldCwgbm9uIGRpY3R1bSBtYXNzYSBhbGlxdWFtLiBBZW5lYW4gcXVpcyB0dXJwaXMgYXVndWUuIFByYWVzZW50IGF1Z3VlIGxlY3R1cywgbW9sbGlzIG5lYyBlbGVtZW50dW0gZXUsIGRpZ25pc3NpbSBhdCB2ZWxpdC4gVXQgY29uZ3VlIG5lcXVlIGlkIHVsbGFtY29ycGVyIHBlbGxlbnRlc3F1ZS4gTWFlY2VuYXMgZXVpc21vZCBpbiBlbGl0IGV1IHZlaGljdWxhLiBOdWxsYW0gdHJpc3RpcXVlIGR1aSBudWxsYSwgbmVjIGNvbnZhbGxpcyBtZXR1cyBzdXNjaXBpdCBlZ2V0LiBDcmFzIHNlbXBlciBhdWd1ZSBuZWMgY3Vyc3VzIGJsYW5kaXQuIE51bGxhIHJob25jdXMgZXQgb2RpbyBxdWlzIGJsYW5kaXQuIFByYWVzZW50IGxvYm9ydGlzIGRpZ25pc3NpbSB2ZWxpdCB1dCBwdWx2aW5hci4gRHVpcyBpbnRlcmR1bSBxdWFtIGFkaXBpc2NpbmcgZG9sb3Igc2VtcGVyIHNlbXBlci4gTnVuYyBiaWJlbmR1bSBjb252YWxsaXMgZHVpLCBlZ2V0IG1vbGxpcyBtYWduYSBoZW5kcmVyaXQgZXQuIE1vcmJpIGZhY2lsaXNpcywgYXVndWUgZXUgZnJpbmdpbGxhIGNvbnZhbGxpcywgbWF1cmlzIGVzdCBjdXJzdXMgZG9sb3IsIGV1IHBvc3VlcmUgb2RpbyBudW5jIHF1aXMgb3JjaS4gVXQgZXUganVzdG8gc2VtLiBQaGFzZWxsdXMgdXQgZXJhdCByaG9uY3VzLCBmYXVjaWJ1cyBhcmN1IHZpdGFlLCB2dWxwdXRhdGUgZXJhdC4gQWxpcXVhbSBuZWMgbWFnbmEgdml2ZXJyYSwgaW50ZXJkdW0gZXN0IHZpdGFlLCByaG9uY3VzIHNhcGllbi4gRHVpcyB0aW5jaWR1bnQgdGVtcG9yIGlwc3VtIHV0IGRhcGlidXMuIE51bGxhbSBjb21tb2RvIHZhcml1cyBtZXR1cywgc2VkIHNvbGxpY2l0dWRpbiBlcm9zLiBFdGlhbSBuZWMgb2RpbyBldCBkdWkgdGVtcG9yIGJsYW5kaXQgcG9zdWVyZS4=", base64, "medium base64 conversion should work")
    }

    func test_discontiguousEnumerateBytes() {
        let dataToEncode = "Hello World".data(using: .utf8)!

        let subdata1 = dataToEncode.withUnsafeBytes { bytes in
            return DispatchData(bytes: UnsafeBufferPointer(start: bytes, count: dataToEncode.count))
        }
        let subdata2 = dataToEncode.withUnsafeBytes { bytes in
            return DispatchData(bytes: UnsafeBufferPointer(start: bytes, count: dataToEncode.count))
        }
        var data = subdata1
        data.append(subdata2)

        var numChunks = 0
        var offsets = [Int]()
        data.enumerateBytes() { buffer, offset, stop in
            numChunks += 1
            offsets.append(offset)
        }

        expectEqual(2, numChunks, "composing two dispatch_data should enumerate as structural data as 2 chunks")
        expectEqual(0, offsets[0], "composing two dispatch_data should enumerate as structural data with the first offset as the location of the region")
        expectEqual(dataToEncode.count, offsets[1], "composing two dispatch_data should enumerate as structural data with the first offset as the location of the region")
    }

    func test_basicReadWrite() {
        let url = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true).appendingPathComponent("testfile")

        let count = 1 << 24
        let randomMemory = malloc(count)!
        let ptr = randomMemory.bindMemory(to: UInt8.self, capacity: count)
        let data = Data(bytesNoCopy: ptr, count: count, deallocator: .free)
        do {
            try data.write(to: url)
            let readData = try Data(contentsOf: url)
            expectEqual(data, readData)
        } catch {
            expectTrue(false, "Should not have thrown")
        }
        
        do {
            try FileManager.default.removeItem(at: url)
        } catch {
            // ignore
        }
    }

    func test_writeFailure() {
        let url = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true).appendingPathComponent("testfile")
        
        let data = Data()
        do {
            try data.write(to: url)
        } catch let error as NSError {
            print(error)
            expectTrue(false, "Should not have thrown")
        }
        
        do {
            try data.write(to: url, options: [.withoutOverwriting])
            expectTrue(false, "Should have thrown")
        } catch let error as NSError {
            expectEqual(error.code, NSFileWriteFileExistsError)
        }
        
        do {
            try FileManager.default.removeItem(at: url)
        } catch {
            // ignore
        }
        
        // Make sure clearing the error condition allows the write to succeed
        do {
            try data.write(to: url, options: [.withoutOverwriting])
        } catch {
            expectTrue(false, "Should not have thrown")
        }
        
        do {
            try FileManager.default.removeItem(at: url)
        } catch {
            // ignore
        }
    }
    
    func test_genericBuffers() {
        let a : [Int32] = [1, 0, 1, 0, 1]
        var data = a.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        
        var expectedSize = MemoryLayout<Int32>.stride * a.count
        expectEqual(expectedSize, data.count)
        
        [false, true].withUnsafeBufferPointer {
            data.append($0)
        }
        
        expectedSize += MemoryLayout<Bool>.stride * 2
        expectEqual(expectedSize, data.count)
        
        let underlyingBuffer = unsafeBitCast(malloc(expectedSize)!, to: UnsafeMutablePointer<UInt8>.self)
        
        let buffer = UnsafeMutableBufferPointer(start: underlyingBuffer, count: expectedSize)
        let copiedCount = data.copyBytes(to: buffer)
        expectEqual(copiedCount, expectedSize)
        
        free(underlyingBuffer)
    }

    func test_basicDataMutation() {
        let object = ImmutableDataVerifier()

        object.verifier.reset()
        var data = object as Data
        expectTrue(object.verifier.wasCopied)
        expectFalse(object.verifier.wasMutableCopied, "Expected an invocation to mutableCopy")

        object.verifier.reset()
        expectTrue(data.count == object.length)
        expectFalse(object.verifier.wasCopied, "Expected an invocation to copy")
    }

    func test_basicMutableDataMutation() {
        let object = MutableDataVerifier()
        
        object.verifier.reset()
        var data = object as Data
        expectTrue(object.verifier.wasCopied)
        expectFalse(object.verifier.wasMutableCopied, "Expected an invocation to mutableCopy")
        
        object.verifier.reset()
        expectTrue(data.count == object.length)
        expectFalse(object.verifier.wasCopied, "Expected an invocation to copy")
    }

    func test_passing() {
        let object = ImmutableDataVerifier()
        let object_notPeepholed = object as Data
        takesData(object_notPeepholed)
        expectTrue(object.verifier.wasCopied)
    }

    func test_passing_peepholed() {
        let object = ImmutableDataVerifier()
        takesData(object as Data)
        expectFalse(object.verifier.wasCopied) // because of the peephole
    }
    
    // intentionally structured so sizeof() != strideof()
    struct MyStruct {
        var time: UInt64
        let x: UInt32
        let y: UInt32
        let z: UInt32
        init() {
            time = 0
            x = 1
            y = 2
            z = 3
        }
    }
    
    func test_bufferSizeCalculation() {
        // Make sure that Data is correctly using strideof instead of sizeof.
        // n.b. if sizeof(MyStruct) == strideof(MyStruct), this test is not as useful as it could be
        
        // init
        let stuff = [MyStruct(), MyStruct(), MyStruct()]
        var data = stuff.withUnsafeBufferPointer {
            return Data(buffer: $0)
        }
        
        expectEqual(data.count, MemoryLayout<MyStruct>.stride * 3)
        
        
        // append
        stuff.withUnsafeBufferPointer {
            data.append($0)
        }
        
        expectEqual(data.count, MemoryLayout<MyStruct>.stride * 6)

        // copyBytes
        do {
            // equal size
            let underlyingBuffer = malloc(6 * MemoryLayout<MyStruct>.stride)!
            defer { free(underlyingBuffer) }

            let ptr = underlyingBuffer.bindMemory(to: MyStruct.self, capacity: 6)
            let buffer = UnsafeMutableBufferPointer<MyStruct>(start: ptr, count: 6)
            
            let byteCount = data.copyBytes(to: buffer)
            expectEqual(6 * MemoryLayout<MyStruct>.stride, byteCount)
        }
        
        do {
            // undersized
            let underlyingBuffer = malloc(3 * MemoryLayout<MyStruct>.stride)!
            defer { free(underlyingBuffer) }

            let ptr = underlyingBuffer.bindMemory(to: MyStruct.self, capacity: 3)
            let buffer = UnsafeMutableBufferPointer<MyStruct>(start: ptr, count: 3)
            
            let byteCount = data.copyBytes(to: buffer)
            expectEqual(3 * MemoryLayout<MyStruct>.stride, byteCount)
        }
        
        do {
            // oversized
            let underlyingBuffer = malloc(12 * MemoryLayout<MyStruct>.stride)!
            defer { free(underlyingBuffer) }
            
            let ptr = underlyingBuffer.bindMemory(to: MyStruct.self, capacity: 6)
            let buffer = UnsafeMutableBufferPointer<MyStruct>(start: ptr, count: 6)
            
            let byteCount = data.copyBytes(to: buffer)
            expectEqual(6 * MemoryLayout<MyStruct>.stride, byteCount)
        }
    }
    

    // MARK: -
    func test_classForCoder() {
        // confirm internal bridged impl types are not exposed to archival machinery
        let d = Data() as NSData
        let expected: AnyClass = NSData.self as AnyClass
        expectTrue(d.classForCoder == expected)
        expectTrue(d.classForKeyedArchiver == expected)
    }

    func test_AnyHashableContainingData() {
        let values: [Data] = [
            Data(base64Encoded: "AAAA")!,
            Data(base64Encoded: "AAAB")!,
            Data(base64Encoded: "AAAB")!,
        ]
        let anyHashables = values.map(AnyHashable.init)
        expectEqual(Data.self, type(of: anyHashables[0].base))
        expectEqual(Data.self, type(of: anyHashables[1].base))
        expectEqual(Data.self, type(of: anyHashables[2].base))
        expectNotEqual(anyHashables[0], anyHashables[1])
        expectEqual(anyHashables[1], anyHashables[2])
    }

    func test_AnyHashableCreatedFromNSData() {
        let values: [NSData] = [
            NSData(base64Encoded: "AAAA")!,
            NSData(base64Encoded: "AAAB")!,
            NSData(base64Encoded: "AAAB")!,
        ]
        let anyHashables = values.map(AnyHashable.init)
        expectEqual(Data.self, type(of: anyHashables[0].base))
        expectEqual(Data.self, type(of: anyHashables[1].base))
        expectEqual(Data.self, type(of: anyHashables[2].base))
        expectNotEqual(anyHashables[0], anyHashables[1])
        expectEqual(anyHashables[1], anyHashables[2])
    }

    func test_noCopyBehavior() {
        let ptr = UnsafeMutableRawPointer.allocate(byteCount: 17, alignment: 1)
        
        var deallocated = false
        autoreleasepool {
            let data = Data(bytesNoCopy: ptr, count: 17, deallocator: .custom({ (bytes, length) in
                deallocated = true
                ptr.deallocate()
            }))
            expectFalse(deallocated)
            let equal = data.withUnsafeBytes { (bytes: UnsafePointer<UInt8>) -> Bool in
                return ptr == UnsafeMutableRawPointer(mutating: bytes)
            }
            
            expectTrue(equal)
        }
        expectTrue(deallocated)
    }

    func test_doubleDeallocation() {
        let data = "12345679".data(using: .utf8)!
        let len = data.withUnsafeBytes { (bytes: UnsafePointer<UInt8>) -> Int in
            let slice = Data(bytesNoCopy: UnsafeMutablePointer(mutating: bytes), count: 1, deallocator: .none)
            return slice.count
        }
        expectEqual(len, 1)
    }

    func test_repeatingValueInitialization() {
        var d = Data(repeating: 0x01, count: 3)
        let elements = repeatElement(UInt8(0x02), count: 3) // ensure we fall into the sequence case
        d.append(contentsOf: elements)

        expectEqual(d[0], 0x01)
        expectEqual(d[1], 0x01)
        expectEqual(d[2], 0x01)

        expectEqual(d[3], 0x02)
        expectEqual(d[4], 0x02)
        expectEqual(d[5], 0x02)
    }

    func test_rangeSlice() {
        var a: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7]
        var d = Data(bytes: a)
        for i in 0..<d.count {
            for j in i..<d.count {
                let slice = d[i..<j]
                expectEqual(slice.count, j - i, "where index range is \(i)..<\(j)")
                expectEqual(slice.map { $0 }, a[i..<j].map { $0 }, "where index range is \(i)..<\(j)")
                expectEqual(slice.startIndex, i, "where index range is \(i)..<\(j)")
                expectEqual(slice.endIndex, j, "where index range is \(i)..<\(j)")
                for n in slice.startIndex..<slice.endIndex {
                    let p = slice[n]
                    let q = a[n]
                    expectEqual(p, q, "where index range is \(i)..<\(j) at index \(n)")
                }
            }
        }
    }

    func test_rangeZoo() {
        let r1: Range = 0..<1
        let r2: Range = 0..<1
        let r3 = ClosedRange(0..<1)
        let r4 = ClosedRange(0..<1)

        let data = Data(bytes: [8, 1, 2, 3, 4])
        let slice1: Data = data[r1]
        let slice2: Data = data[r2]
        let slice3: Data = data[r3]
        let slice4: Data = data[r4]
        expectEqual(slice1[0], 8)
        expectEqual(slice2[0], 8)
        expectEqual(slice3[0], 8)
        expectEqual(slice4[0], 8)
    }

    func test_rangeOfDataProtocol() {
        // https://bugs.swift.org/browse/SR-10689
        guard #available(macOS 11, iOS 14, watchOS 7, tvOS 14, *) else { return }

        let base = Data([0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x02, 0x03,
                         0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x02, 0x03])
        let subdata = base[10..<13] // [0x02, 0x03, 0x00]
        let oneByte = base[14..<15] // [0x02]
        
        do { // firstRange(of:in:)
            func assertFirstRange(_ data: Data, _ fragment: Data, range: ClosedRange<Int>? = nil,
                                  expectedStartIndex: Int?,
                                  _ message: @autoclosure () -> String = "",
                                  file: String = #file, line: UInt = #line) {
                if let index = expectedStartIndex {
                    let expectedRange: Range<Int> = index..<(index + fragment.count)
                    if let someRange = range {
                        expectEqual(data.firstRange(of: fragment, in: someRange), expectedRange, message(), file: file, line: line)
                    } else {
                        expectEqual(data.firstRange(of: fragment), expectedRange, message(), file: file, line: line)
                    }
                } else {
                    if let someRange = range {
                        expectNil(data.firstRange(of: fragment, in: someRange), message(), file: file, line: line)
                    } else {
                        expectNil(data.firstRange(of: fragment), message(), file: file, line: line)
                    }
                }
            }
            
            assertFirstRange(base, base, expectedStartIndex: base.startIndex)
            assertFirstRange(base, subdata, expectedStartIndex: 2)
            assertFirstRange(base, oneByte, expectedStartIndex: 2)
            
            assertFirstRange(subdata, base, expectedStartIndex: nil)
            assertFirstRange(subdata, subdata, expectedStartIndex: subdata.startIndex)
            assertFirstRange(subdata, oneByte, expectedStartIndex: subdata.startIndex)
            
            assertFirstRange(oneByte, base, expectedStartIndex: nil)
            assertFirstRange(oneByte, subdata, expectedStartIndex: nil)
            assertFirstRange(oneByte, oneByte, expectedStartIndex: oneByte.startIndex)
            
            assertFirstRange(base, subdata, range: 1...14, expectedStartIndex: 2)
            assertFirstRange(base, subdata, range: 6...8, expectedStartIndex: 6)
            assertFirstRange(base, subdata, range: 8...10, expectedStartIndex: nil)
            
            assertFirstRange(base, oneByte, range: 1...14, expectedStartIndex: 2)
            assertFirstRange(base, oneByte, range: 6...6, expectedStartIndex: 6)
            assertFirstRange(base, oneByte, range: 8...9, expectedStartIndex: nil)
        }
        
        do { // lastRange(of:in:)
            func assertLastRange(_ data: Data, _ fragment: Data, range: ClosedRange<Int>? = nil,
                                 expectedStartIndex: Int?,
                                 _ message: @autoclosure () -> String = "",
                                 file: String = #file, line: UInt = #line) {
                if let index = expectedStartIndex {
                    let expectedRange: Range<Int> = index..<(index + fragment.count)
                    if let someRange = range {
                        expectEqual(data.lastRange(of: fragment, in: someRange), expectedRange, message(), file: file, line: line)
                    } else {
                        expectEqual(data.lastRange(of: fragment), expectedRange, message(), file: file, line: line)
                    }
                } else {
                  if let someRange = range {
                      expectNil(data.lastRange(of: fragment, in: someRange), message(), file: file, line: line)
                  } else {
                      expectNil(data.lastRange(of: fragment), message(), file: file, line: line)
                  }
                }
            }
            
            assertLastRange(base, base, expectedStartIndex: base.startIndex)
            assertLastRange(base, subdata, expectedStartIndex: 10)
            assertLastRange(base, oneByte, expectedStartIndex: 14)
            
            assertLastRange(subdata, base, expectedStartIndex: nil)
            assertLastRange(subdata, subdata, expectedStartIndex: subdata.startIndex)
            assertLastRange(subdata, oneByte, expectedStartIndex: subdata.startIndex)
            
            assertLastRange(oneByte, base, expectedStartIndex: nil)
            assertLastRange(oneByte, subdata, expectedStartIndex: nil)
            assertLastRange(oneByte, oneByte, expectedStartIndex: oneByte.startIndex)
            
            assertLastRange(base, subdata, range: 1...14, expectedStartIndex: 10)
            assertLastRange(base, subdata, range: 6...8, expectedStartIndex: 6)
            assertLastRange(base, subdata, range: 8...10, expectedStartIndex: nil)
            
            assertLastRange(base, oneByte, range: 1...14, expectedStartIndex: 14)
            assertLastRange(base, oneByte, range: 6...6, expectedStartIndex: 6)
            assertLastRange(base, oneByte, range: 8...9, expectedStartIndex: nil)
        }
    }

    func test_sliceAppending() {
        // https://bugs.swift.org/browse/SR-4473
        var fooData = Data()
        let barData = Data([0, 1, 2, 3, 4, 5])
        let slice = barData.suffix(from: 3)
        fooData.append(slice)
        expectEqual(fooData[0], 0x03)
        expectEqual(fooData[1], 0x04)
        expectEqual(fooData[2], 0x05)
    }
    
    func test_replaceSubrange() {
        // https://bugs.swift.org/browse/SR-4462
        let data = Data(bytes: [0x01, 0x02])
        var dataII = Data(base64Encoded: data.base64EncodedString())!
        dataII.replaceSubrange(0..<1, with: Data())
        expectEqual(dataII[0], 0x02)
    }
    
    func test_sliceWithUnsafeBytes() {
        let base = Data([0, 1, 2, 3, 4, 5])
        let slice = base[2..<4]
        let segment = slice.withUnsafeBytes { (ptr: UnsafePointer<UInt8>) -> [UInt8] in
            return [ptr.pointee, ptr.advanced(by: 1).pointee]
        }
        expectEqual(segment, [UInt8(2), UInt8(3)])
    }

    func test_sliceIteration() {
        let base = Data([0, 1, 2, 3, 4, 5])
        let slice = base[2..<4]
        var found = [UInt8]()
        for byte in slice {
            found.append(byte)
        }
        expectEqual(found[0], 2)
        expectEqual(found[1], 3)
    }
  
    func test_unconditionallyBridgeFromObjectiveC() {
        expectEqual(Data(), Data._unconditionallyBridgeFromObjectiveC(nil))
    }

    func test_sliceIndexing() {
        let d = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])
        let slice = d[5..<10]
        expectEqual(slice[5], d[5])
    }
    
    func test_sliceEquality() {
        let d = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])
        let slice = d[5..<7]
        let expected = Data(bytes: [5, 6])
        expectEqual(expected, slice)
    }
    
    func test_sliceEquality2() {
        let d = Data(bytes: [5, 6, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])
        let slice1 = d[0..<2]
        let slice2 = d[5..<7]
        expectEqual(slice1, slice2)
    }
    
    func test_splittingHttp() {
        func split(_ data: Data, on delimiter: String) -> [Data] {
            let dataDelimiter = delimiter.data(using: .utf8)!
            var found = [Data]()
            let start = data.startIndex
            let end = data.endIndex.advanced(by: -dataDelimiter.count)
            guard end >= start else { return [data] }
            var index = start
            var previousIndex = index
            while index < end {
                let slice = data[index..<index.advanced(by: dataDelimiter.count)]
                
                if slice == dataDelimiter {
                    found.append(data[previousIndex..<index])
                    previousIndex = index + dataDelimiter.count
                }
                
                index = index.advanced(by: 1)
            }
            if index < data.endIndex { found.append(data[index..<index]) }
            return found
        }
        let data = "GET /index.html HTTP/1.1\r\nHost: www.example.com\r\n\r\n".data(using: .ascii)!
        let fields = split(data, on: "\r\n")
        let splitFields = fields.map { String(data:$0, encoding: .utf8)! }
        expectEqual([
            "GET /index.html HTTP/1.1",
            "Host: www.example.com",
            ""
        ], splitFields)
    }

    func test_map() {
        let d1 = Data(bytes: [81, 0, 0, 0, 14])
        let d2 = d1[1...4]
        expectEqual(4, d2.count)
        let expected: [UInt8] = [0, 0, 0, 14]
        let actual = d2.map { $0 }
        expectEqual(expected, actual)
    }

    func test_dropFirst() {
        var data = Data([0, 1, 2, 3, 4, 5])
        let sliced = data.dropFirst()
        expectEqual(data.count - 1, sliced.count)
        expectEqual(UInt8(1), sliced[1])
        expectEqual(UInt8(2), sliced[2])
        expectEqual(UInt8(3), sliced[3])
        expectEqual(UInt8(4), sliced[4])
        expectEqual(UInt8(5), sliced[5])
    }

    func test_dropFirst2() {
        var data = Data([0, 1, 2, 3, 4, 5])
        let sliced = data.dropFirst(2)
        expectEqual(data.count - 2, sliced.count)
        expectEqual(UInt8(2), sliced[2])
        expectEqual(UInt8(3), sliced[3])
        expectEqual(UInt8(4), sliced[4])
        expectEqual(UInt8(5), sliced[5])
    }

    func test_copyBytes1() {
        var array: [UInt8] = [0, 1, 2, 3]
        var data = Data(bytes: array)
        
        array.withUnsafeMutableBufferPointer {
            data[1..<3].copyBytes(to: $0.baseAddress!, from: 1..<3)
        }
        expectEqual([UInt8(1), UInt8(2), UInt8(2), UInt8(3)], array)
    }
    
    func test_copyBytes2() {
        let array: [UInt8] = [0, 1, 2, 3]
        var data = Data(bytes: array)
        
        let expectedSlice = array[1..<3]
        
        let start = data.index(after: data.startIndex)
        let end = data.index(before: data.endIndex)
        let slice = data[start..<end]
        
        expectEqual(expectedSlice[expectedSlice.startIndex], slice[slice.startIndex])
    }

    func test_sliceOfSliceViaRangeExpression() {
        let data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])

        let slice = data[2..<7]

        let sliceOfSlice1 = slice[..<(slice.startIndex + 2)] // this triggers the range expression
        let sliceOfSlice2 = slice[(slice.startIndex + 2)...] // also triggers range expression

        expectEqual(Data(bytes: [2, 3]), sliceOfSlice1)
        expectEqual(Data(bytes: [4, 5, 6]), sliceOfSlice2)
    }

    func test_appendingSlices() {
        let d1 = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let slice = d1[1..<2]
        var d2 = Data()
        d2.append(slice)
        expectEqual(Data(bytes: [1]), slice)
    }

    // This test uses `repeatElement` to produce a sequence -- the produced sequence reports its actual count as its `.underestimatedCount`.
    func test_appendingNonContiguousSequence_exactCount() {
        var d = Data()

        // d should go from .empty representation to .inline.
        // Appending a small enough sequence to fit in .inline should actually be copied.
        d.append(contentsOf: 0x00...0x01)
        expectEqual(Data([0x00, 0x01]), d)

        // Appending another small sequence should similarly still work.
        d.append(contentsOf: 0x02...0x02)
        expectEqual(Data([0x00, 0x01, 0x02]), d)

        // If we append a sequence of elements larger than a single InlineData, the internal append here should buffer.
        // We want to make sure that buffering in this way does not accidentally drop trailing elements on the floor.
        d.append(contentsOf: 0x03...0x2F)
        expectEqual(Data([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
                          0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                          0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F]), d)
    }

    // This test is like test_appendingNonContiguousSequence_exactCount but uses a sequence which reports 0 for its `.underestimatedCount`.
    // This attempts to hit the worst-case scenario of `Data.append<S>(_:)` -- a discontiguous sequence of unknown length.
    func test_appendingNonContiguousSequence_underestimatedCount() {
        var d = Data()

        // d should go from .empty representation to .inline.
        // Appending a small enough sequence to fit in .inline should actually be copied.
        d.append(contentsOf: (0x00...0x01).makeIterator()) // `.makeIterator()` produces a sequence whose `.underestimatedCount` is 0.
        expectEqual(Data([0x00, 0x01]), d)

        // Appending another small sequence should similarly still work.
        d.append(contentsOf: (0x02...0x02).makeIterator()) // `.makeIterator()` produces a sequence whose `.underestimatedCount` is 0.
        expectEqual(Data([0x00, 0x01, 0x02]), d)

        // If we append a sequence of elements larger than a single InlineData, the internal append here should buffer.
        // We want to make sure that buffering in this way does not accidentally drop trailing elements on the floor.
        d.append(contentsOf: (0x03...0x2F).makeIterator()) // `.makeIterator()` produces a sequence whose `.underestimatedCount` is 0.
        expectEqual(Data([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
                          0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                          0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F]), d)
    }

    func test_sequenceInitializers() {
        let seq = repeatElement(UInt8(0x02), count: 3) // ensure we fall into the sequence case
        
        let dataFromSeq = Data(seq)
        expectEqual(3, dataFromSeq.count)
        expectEqual(UInt8(0x02), dataFromSeq[0])
        expectEqual(UInt8(0x02), dataFromSeq[1])
        expectEqual(UInt8(0x02), dataFromSeq[2])

        let array: [UInt8] = [0, 1, 2, 3, 4, 5, 6]

        let dataFromArray = Data(array)
        expectEqual(array.count, dataFromArray.count)
        expectEqual(array[0], dataFromArray[0])
        expectEqual(array[1], dataFromArray[1])
        expectEqual(array[2], dataFromArray[2])
        expectEqual(array[3], dataFromArray[3])

        let slice = array[1..<4]
        
        let dataFromSlice = Data(slice)
        expectEqual(slice.count, dataFromSlice.count)
        expectEqual(slice.first, dataFromSlice.first)
        expectEqual(slice.last, dataFromSlice.last)

        let data = Data(bytes: [1, 2, 3, 4, 5, 6, 7, 8, 9])

        let dataFromData = Data(data)
        expectEqual(data, dataFromData)

        let sliceOfData = data[1..<3]

        let dataFromSliceOfData = Data(sliceOfData)
        expectEqual(sliceOfData, dataFromSliceOfData)
    }

    func test_reversedDataInit() {
        let data = Data(bytes: [1, 2, 3, 4, 5, 6, 7, 8, 9])
        let reversedData = Data(data.reversed())
        let expected = Data(bytes: [9, 8, 7, 6, 5, 4, 3, 2, 1])
        expectEqual(expected, reversedData)
    }

    func test_replaceSubrangeReferencingMutable() {
        let mdataObj = NSMutableData(bytes: [0x01, 0x02, 0x03, 0x04], length: 4)
        var data = Data(referencing: mdataObj)
        let expected = data.count
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
    }

    func test_replaceSubrangeReferencingImmutable() {
        let dataObj = NSData(bytes: [0x01, 0x02, 0x03, 0x04], length: 4)
        var data = Data(referencing: dataObj)
        let expected = data.count
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
    }

    func test_replaceSubrangeFromBridged() {
        var data = NSData(bytes: [0x01, 0x02, 0x03, 0x04], length: 4) as Data
        let expected = data.count
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
        data.replaceSubrange(4 ..< 4, with: Data(bytes: []))
        expectEqual(expected, data.count)
    }

        func test_validateMutation_withUnsafeMutableBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 5).pointee = 0xFF
        }
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 0xFF, 6, 7, 8, 9]))
    }
    
    func test_validateMutation_appendBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        data.append("hello", count: 5)
        expectEqual(data[data.startIndex.advanced(by: 5)], 0x5)
    }
    
    func test_validateMutation_appendData() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let other = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        data.append(other)
        expectEqual(data[data.startIndex.advanced(by: 9)], 9)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0)
    }
    
    func test_validateMutation_appendBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data[data.startIndex.advanced(by: 9)], 9)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0)
    }
    
    func test_validateMutation_appendSequence() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let seq = repeatElement(UInt8(1), count: 10)
        data.append(contentsOf: seq)
        expectEqual(data[data.startIndex.advanced(by: 9)], 9)
        expectEqual(data[data.startIndex.advanced(by: 10)], 1)
    }
    
    func test_validateMutation_appendContentsOf() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        data.append(contentsOf: bytes)
        expectEqual(data[data.startIndex.advanced(by: 9)], 9)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0)
    }
    
    func test_validateMutation_resetBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 0, 0, 0, 8, 9]))
    }
    
    func test_validateMutation_replaceSubrange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_replaceSubrangeRange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_replaceSubrangeWithBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer {
            data.replaceSubrange(range, with: $0)
        }
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_replaceSubrangeWithCollection() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with: bytes)
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_replaceSubrangeWithBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBytes {
            data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
        }
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_slice_withUnsafeMutableBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data, Data(bytes: [4, 0xFF, 6, 7, 8]))
    }
    
    func test_validateMutation_slice_appendBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_appendData() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let other = Data(bytes: [0xFF, 0xFF])
        data.append(other)
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_appendBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_appendSequence() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let seq = repeatElement(UInt8(0xFF), count: 2)
        data.append(contentsOf: seq)
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_appendContentsOf() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        data.append(contentsOf: bytes)
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_resetBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
    }
    
    func test_validateMutation_slice_replaceSubrange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_replaceSubrangeRange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_replaceSubrangeWithBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer {
            data.replaceSubrange(range, with: $0)
        }
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_replaceSubrangeWithCollection() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with: bytes)
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_replaceSubrangeWithBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBytes {
            data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
        }
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_cow_withUnsafeMutableBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 5).pointee = 0xFF
            }
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 0xFF, 6, 7, 8, 9]))
        }
    }
    
    func test_validateMutation_cow_appendBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            data.append("hello", count: 5)
            expectEqual(data[data.startIndex.advanced(by: 9)], 0x9)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x68)
        }
    }
    
    func test_validateMutation_cow_appendData() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let other = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
            data.append(other)
            expectEqual(data[data.startIndex.advanced(by: 9)], 9)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0)
        }
    }
    
    func test_validateMutation_cow_appendBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data[data.startIndex.advanced(by: 9)], 9)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0)
        }
    }
    
    func test_validateMutation_cow_appendSequence() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let seq = repeatElement(UInt8(1), count: 10)
            data.append(contentsOf: seq)
            expectEqual(data[data.startIndex.advanced(by: 9)], 9)
            expectEqual(data[data.startIndex.advanced(by: 10)], 1)
        }
    }
    
    func test_validateMutation_cow_appendContentsOf() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
            data.append(contentsOf: bytes)
            expectEqual(data[data.startIndex.advanced(by: 9)], 9)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0)
        }
    }
    
    func test_validateMutation_cow_resetBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 0, 0, 0, 8, 9]))
        }
    }
    
    func test_validateMutation_cow_replaceSubrange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
        }
    }
    
    func test_validateMutation_cow_replaceSubrangeRange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
        }
    }
    
    func test_validateMutation_cow_replaceSubrangeWithBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer {
                data.replaceSubrange(range, with: $0)
            }
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
        }
    }
    
    func test_validateMutation_cow_replaceSubrangeWithCollection() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.replaceSubrange(range, with: bytes)
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
        }
    }
    
    func test_validateMutation_cow_replaceSubrangeWithBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
            }
            expectEqual(data, Data(bytes: [0, 1, 2, 3, 0xFF, 0xFF, 9]))
        }
    }
    
    func test_validateMutation_slice_cow_withUnsafeMutableBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 1).pointee = 0xFF
            }
            expectEqual(data, Data(bytes: [4, 0xFF, 6, 7, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_appendBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            data.append("hello", count: 5)
            expectEqual(data[data.startIndex.advanced(by: 4)], 0x8)
            expectEqual(data[data.startIndex.advanced(by: 5)], 0x68)
        }
    }
    
    func test_validateMutation_slice_cow_appendData() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let other = Data(bytes: [0xFF, 0xFF])
            data.append(other)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_appendBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_appendSequence() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let seq = repeatElement(UInt8(0xFF), count: 2)
            data.append(contentsOf: seq)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_appendContentsOf() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.append(contentsOf: bytes)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_resetBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_replaceSubrange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_replaceSubrangeRange() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_replaceSubrangeWithBuffer() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer {
                data.replaceSubrange(range, with: $0)
            }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_replaceSubrangeWithCollection() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.replaceSubrange(range, with: bytes)
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_replaceSubrangeWithBytes() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
            }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_immutableBacking_withUnsafeMutableBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 5).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
    }
    
    func test_validateMutation_immutableBacking_appendBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append("hello", count: 5)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
        expectEqual(data[data.startIndex.advanced(by: 11)], 0x68)
    }
    
    func test_validateMutation_immutableBacking_appendData() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let other = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
        data.append(other)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
        expectEqual(data[data.startIndex.advanced(by: 11)], 0)
    }
    
    func test_validateMutation_immutableBacking_appendBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
        expectEqual(data[data.startIndex.advanced(by: 11)], 0)
    }
    
    func test_validateMutation_immutableBacking_appendSequence() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let seq = repeatElement(UInt8(1), count: 10)
        data.append(contentsOf: seq)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
        expectEqual(data[data.startIndex.advanced(by: 11)], 1)
    }
    
    func test_validateMutation_immutableBacking_appendContentsOf() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        data.append(contentsOf: bytes)
        expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
        expectEqual(data[data.startIndex.advanced(by: 11)], 0)
    }
    
    func test_validateMutation_immutableBacking_resetBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x72, 0x6c, 0x64]))
    }
    
    func test_validateMutation_immutableBacking_replaceSubrange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xFF, 0xFF, 0x6c, 0x64]))
    }
    
    func test_validateMutation_immutableBacking_replaceSubrangeRange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xFF, 0xFF, 0x6c, 0x64]))
    }
    
    func test_validateMutation_immutableBacking_replaceSubrangeWithBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer {
            data.replaceSubrange(range, with: $0)
        }
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xFF, 0xFF, 0x6c, 0x64]))
    }
    
    func test_validateMutation_immutableBacking_replaceSubrangeWithCollection() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with: bytes)
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xFF, 0xFF, 0x6c, 0x64]))
    }
    
    func test_validateMutation_immutableBacking_replaceSubrangeWithBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
        let bytes: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with: bytes)
        expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xFF, 0xFF, 0x6c, 0x64]))
    }
    
    func test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes() {
        var data = (NSData(bytes: "hello world", length: 11) as Data)[4..<9]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_immutableBacking_appendBytes() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_appendData() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_appendBuffer() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_appendSequence() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_appendContentsOf() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_resetBytes() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
    }
    
    func test_validateMutation_slice_immutableBacking_replaceSubrange() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_immutableBacking_replaceSubrangeRange() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_immutableBacking_replaceSubrangeWithBuffer() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let replacement: [UInt8] = [0xFF, 0xFF]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        replacement.withUnsafeBufferPointer { (buffer: UnsafeBufferPointer<UInt8>) in
            data.replaceSubrange(range, with: buffer)
        }
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_immutableBacking_replaceSubrangeWithCollection() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with:replacement)
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_slice_immutableBacking_replaceSubrangeWithBytes() {
        let base: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = base.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        let replacement: [UInt8] = [0xFF, 0xFF]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        replacement.withUnsafeBytes {
            data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
        }
        expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
    }
    
    func test_validateMutation_cow_immutableBacking_withUnsafeMutableBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 5).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_appendBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            data.append("hello", count: 5)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
            expectEqual(data[data.startIndex.advanced(by: 11)], 0x68)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_appendData() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let other = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
            data.append(other)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
            expectEqual(data[data.startIndex.advanced(by: 11)], 0)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_appendBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let bytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
            expectEqual(data[data.startIndex.advanced(by: 11)], 0)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_appendSequence() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let seq = repeatElement(UInt8(1), count: 10)
            data.append(contentsOf: seq)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
            expectEqual(data[data.startIndex.advanced(by: 11)], 1)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_appendContentsOf() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let bytes: [UInt8] = [1, 1, 2, 3, 4, 5, 6, 7, 8, 9]
            data.append(contentsOf: bytes)
            expectEqual(data[data.startIndex.advanced(by: 10)], 0x64)
            expectEqual(data[data.startIndex.advanced(by: 11)], 1)
        }
    }
    
    func test_validateMutation_cow_immutableBacking_resetBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x72, 0x6c, 0x64]))
        }
    }
    
    func test_validateMutation_cow_immutableBacking_replaceSubrange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64]))
        }
    }
    
    func test_validateMutation_cow_immutableBacking_replaceSubrangeRange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64]))
        }
    }
    
    func test_validateMutation_cow_immutableBacking_replaceSubrangeWithBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let replacement: [UInt8] = [0xFF, 0xFF]
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            replacement.withUnsafeBufferPointer { (buffer: UnsafeBufferPointer<UInt8>) in
                data.replaceSubrange(range, with: buffer)
            }
            expectEqual(data, Data(bytes: [0x68, 0xff, 0xff, 0x64]))
        }
    }
    
    func test_validateMutation_cow_immutableBacking_replaceSubrangeWithCollection() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let replacement: [UInt8] = [0xFF, 0xFF]
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0xff, 0xff, 0x64]))
        }
    }
    
    func test_validateMutation_cow_immutableBacking_replaceSubrangeWithBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        holdReference(data) {
            let replacement: [UInt8] = [0xFF, 0xFF]
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            replacement.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
            }
            expectEqual(data, Data(bytes: [0x68, 0xff, 0xff, 0x64]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_withUnsafeMutableBytes() {
        var data = (NSData(bytes: "hello world", length: 11) as Data)[4..<9]
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 1).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_appendBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_appendData() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_appendBuffer() {
        var data = (NSData(bytes: "hello world", length: 11) as Data)[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer{ data.append($0) }
            expectEqual(data, Data(bytes: [0x6f, 0x20, 0x77, 0x6f, 0x72, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_appendSequence() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let bytes = repeatElement(UInt8(0xFF), count: 2)
            data.append(contentsOf: bytes)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_appendContentsOf() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.append(contentsOf: bytes)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_resetBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_replaceSubrange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_replaceSubrangeRange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBuffer() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithCollection() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.replaceSubrange(range, with: bytes)
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        var data = baseBytes.withUnsafeBufferPointer {
            return (NSData(bytes: $0.baseAddress!, length: $0.count) as Data)[4..<9]
        }
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBytes { data.replaceSubrange(range, with: $0.baseAddress!, count: 2) }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_mutableBacking_withUnsafeMutableBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 5).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
    }
    
    func test_validateMutation_mutableBacking_appendBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_mutableBacking_appendData() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_mutableBacking_appendBuffer() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_mutableBacking_appendSequence() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_mutableBacking_appendContentsOf() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_mutableBacking_resetBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [0, 1, 2, 3, 4, 0, 0, 0, 8, 9]))
    }
    
    func test_validateMutation_mutableBacking_replaceSubrange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_mutableBacking_replaceSubrangeRange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement = Data(bytes: [0xFF, 0xFF])
        data.replaceSubrange(range, with: replacement)
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_mutableBacking_replaceSubrangeWithBuffer() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer {
            data.replaceSubrange(range, with: $0)
        }
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_mutableBacking_replaceSubrangeWithCollection() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_mutableBacking_replaceSubrangeWithBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var data = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        data.append(contentsOf: [7, 8, 9])
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBytes {
            data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count)
        }
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 9]))
    }
    
    func test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_mutableBacking_appendBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_mutableBacking_appendData() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_mutableBacking_appendBuffer() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let bytes: [UInt8] = [1, 2, 3]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [0x6f, 0x20, 0x77, 0x6f, 0x72, 0x1, 0x2, 0x3]))
    }
    
    func test_validateMutation_slice_mutableBacking_appendSequence() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let seq = repeatElement(UInt8(1), count: 3)
        data.append(contentsOf: seq)
        expectEqual(data, Data(bytes: [0x6f, 0x20, 0x77, 0x6f, 0x72, 0x1, 0x1, 0x1]))
    }
    
    func test_validateMutation_slice_mutableBacking_appendContentsOf() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let bytes: [UInt8] = [1, 2, 3]
        data.append(contentsOf: bytes)
        expectEqual(data, Data(bytes: [0x6f, 0x20, 0x77, 0x6f, 0x72, 0x1, 0x2, 0x3]))
    }
    
    func test_validateMutation_slice_mutableBacking_resetBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
    }
    
    func test_validateMutation_slice_mutableBacking_replaceSubrange() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0x6f, 0xFF, 0xFF, 0x72]))
    }
    
    func test_validateMutation_slice_mutableBacking_replaceSubrangeRange() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0x6f, 0xFF, 0xFF, 0x72]))
    }
    
    func test_validateMutation_slice_mutableBacking_replaceSubrangeWithBuffer() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let replacement: [UInt8] = [0xFF, 0xFF]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        replacement.withUnsafeBufferPointer { (buffer: UnsafeBufferPointer<UInt8>) in
            data.replaceSubrange(range, with: buffer)
        }
        expectEqual(data, Data(bytes: [0x6f, 0xFF, 0xFF, 0x72]))
    }
    
    func test_validateMutation_slice_mutableBacking_replaceSubrangeWithCollection() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let replacement: [UInt8] = [0xFF, 0xFF]
        data.replaceSubrange(range, with:replacement)
        expectEqual(data, Data(bytes: [0x6f, 0xFF, 0xFF, 0x72]))
    }
    
    func test_validateMutation_slice_mutableBacking_replaceSubrangeWithBytes() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        let replacement: [UInt8] = [0xFF, 0xFF]
        let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        replacement.withUnsafeBytes {
            data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
        }
        expectEqual(data, Data(bytes: [0x6f, 0xFF, 0xFF, 0x72]))
    }
    
    func test_validateMutation_cow_mutableBacking_withUnsafeMutableBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 5).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_appendBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            data.append("hello", count: 5)
            expectEqual(data[data.startIndex.advanced(by: 16)], 6)
            expectEqual(data[data.startIndex.advanced(by: 17)], 0x68)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_appendData() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            data.append("hello", count: 5)
            expectEqual(data[data.startIndex.advanced(by: 16)], 6)
            expectEqual(data[data.startIndex.advanced(by: 17)], 0x68)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_appendBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let other = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
            data.append(other)
            expectEqual(data[data.startIndex.advanced(by: 16)], 6)
            expectEqual(data[data.startIndex.advanced(by: 17)], 0)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_appendSequence() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let seq = repeatElement(UInt8(1), count: 10)
            data.append(contentsOf: seq)
            expectEqual(data[data.startIndex.advanced(by: 16)], 6)
            expectEqual(data[data.startIndex.advanced(by: 17)], 1)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_appendContentsOf() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let bytes: [UInt8] = [1, 1, 2, 3, 4, 5, 6, 7, 8, 9]
            data.append(contentsOf: bytes)
            expectEqual(data[data.startIndex.advanced(by: 16)], 6)
            expectEqual(data[data.startIndex.advanced(by: 17)], 1)
        }
    }
    
    func test_validateMutation_cow_mutableBacking_resetBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x72, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_cow_mutableBacking_replaceSubrange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_cow_mutableBacking_replaceSubrangeRange() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement = Data(bytes: [0xFF, 0xFF])
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_cow_mutableBacking_replaceSubrangeWithBuffer() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            let replacement: [UInt8] = [0xFF, 0xFF]
            replacement.withUnsafeBufferPointer { (buffer: UnsafeBufferPointer<UInt8>) in
                data.replaceSubrange(range, with: buffer)
            }
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_cow_mutableBacking_replaceSubrangeWithCollection() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let replacement: [UInt8] = [0xFF, 0xFF]
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            data.replaceSubrange(range, with: replacement)
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_cow_mutableBacking_replaceSubrangeWithBytes() {
        var data = NSData(bytes: "hello world", length: 11) as Data
        data.append(contentsOf: [1, 2, 3, 4, 5, 6])
        holdReference(data) {
            let replacement: [UInt8] = [0xFF, 0xFF]
            let range: Range<Data.Index> = data.startIndex.advanced(by: 4)..<data.startIndex.advanced(by: 9)
            replacement.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: 2)
            }
            expectEqual(data, Data(bytes: [0x68, 0x65, 0x6c, 0x6c, 0xff, 0xff, 0x6c, 0x64, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_withUnsafeMutableBytes() {
        var base = NSData(bytes: "hello world", length: 11) as Data
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<9]
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 1).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_appendBytes() {
        let bytes: [UInt8] = [0, 1, 2]
        var base = bytes.withUnsafeBytes { (ptr) in
            return NSData(bytes: ptr.baseAddress!, length: ptr.count) as Data
        }
        base.append(contentsOf: [3, 4, 5])
        var data = base[1..<4]
        holdReference(data) {
            let bytesToAppend: [UInt8] = [6, 7, 8]
            bytesToAppend.withUnsafeBytes { (ptr) in
                data.append(ptr.baseAddress!.assumingMemoryBound(to: UInt8.self), count: ptr.count)
            }
            expectEqual(data, Data(bytes: [1, 2, 3, 6, 7, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_appendData() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_appendBuffer() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer{ data.append($0) }
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_appendSequence() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let bytes = repeatElement(UInt8(0xFF), count: 2)
            data.append(contentsOf: bytes)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_appendContentsOf() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.append(contentsOf: bytes)
            expectEqual(data, Data(bytes: [4, 5, 6, 7, 8, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_resetBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [4, 0, 0, 0, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_replaceSubrange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_replaceSubrangeRange() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBuffer() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithCollection() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            data.replaceSubrange(range, with: bytes)
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBytes() {
        let baseBytes: [UInt8] = [0, 1, 2, 3, 4, 5, 6]
        var base = baseBytes.withUnsafeBufferPointer {
            return NSData(bytes: $0.baseAddress!, length: $0.count) as Data
        }
        base.append(contentsOf: [7, 8, 9])
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Data.Index> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBytes { data.replaceSubrange(range, with: $0.baseAddress!, count: 2) }
            expectEqual(data, Data(bytes: [4, 0xFF, 0xFF, 8]))
        }
    }
    
    func test_validateMutation_customBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 5).pointee = 0xFF
        }
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_customBacking_appendBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customBacking_appendData() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customBacking_appendBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { (buffer) in
            data.append(buffer)
        }
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        
    }
    
    func test_validateMutation_customBacking_appendSequence() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customBacking_resetBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0, 0, 0, 1, 1]))
    }
    
    func test_validateMutation_customBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let range: Range<Int> = 1..<4
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1, 1, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_customBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let range: Range<Int> = 1..<4
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1, 1, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_customBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let bytes: [UInt8] = [0xFF, 0xFF]
        let range: Range<Int> = 1..<4
        bytes.withUnsafeBufferPointer { (buffer) in
            data.replaceSubrange(range, with: buffer)
        }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1, 1, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_customBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let range: Range<Int> = 1..<4
        data.replaceSubrange(range, with: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1, 1, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_customBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        let bytes: [UInt8] = [0xFF, 0xFF]
        let range: Range<Int> = 1..<5
        bytes.withUnsafeBufferPointer { (buffer) in
            data.replaceSubrange(range, with: buffer.baseAddress!, count: buffer.count)
        }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1, 1, 1, 1, 1]))
    }
    
    func test_validateMutation_slice_customBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_customBacking_appendBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBytes { ptr in
            data.append(ptr.baseAddress!.assumingMemoryBound(to: UInt8.self), count: ptr.count)
        }
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customBacking_appendData() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customBacking_appendBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { (buffer) in
            data.append(buffer)
        }
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customBacking_appendSequence() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let seq = repeatElement(UInt8(0xFF), count: 2)
        data.append(contentsOf: seq)
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customBacking_resetBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        data.resetBytes(in: 5..<8)
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 1]))
    }
    
    func test_validateMutation_slice_customBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
    }
    
    func test_validateMutation_slice_customBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
    }
    
    func test_validateMutation_slice_customBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { (buffer) in
            data.replaceSubrange(range, with: buffer)
        }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
    }
    
    func test_validateMutation_slice_customBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
    }
    
    func test_validateMutation_slice_customBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBytes { buffer in
            data.replaceSubrange(range, with: buffer.baseAddress!, count: 2)
        }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
    }
    
    func test_validateMutation_cow_customBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 5).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
        }
    }
    
    func test_validateMutation_cow_customBacking_appendBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { (buffer) in
                data.append(buffer.baseAddress!, count: buffer.count)
            }
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customBacking_appendData() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customBacking_appendBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customBacking_appendSequence() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            data.append(contentsOf: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customBacking_resetBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0, 0, 0, 1, 1]))
        }
    }
    
    func test_validateMutation_cow_customBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_cow_customBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_cow_customBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_cow_customBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_cow_customBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            bytes.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count)
            }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 1).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_appendBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { (buffer) in
                data.append(buffer.baseAddress!, count: buffer.count)
            }
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_appendData() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_appendBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_appendSequence() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            data.append(contentsOf: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 1, 1, 1, 1, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_resetBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBytes {
                data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count)
            }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 1]))
        }
    }
    
    func test_validateMutation_customMutableBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 5).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
    }
    
    func test_validateMutation_customMutableBacking_appendBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customMutableBacking_appendData() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customMutableBacking_appendBuffer() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customMutableBacking_appendSequence() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customMutableBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_customMutableBacking_resetBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        data.resetBytes(in: 5..<8)
        expectEqual(data.count, 10)
        expectEqual(data[data.startIndex.advanced(by: 0)], 1)
        expectEqual(data[data.startIndex.advanced(by: 5)], 0)
        expectEqual(data[data.startIndex.advanced(by: 6)], 0)
        expectEqual(data[data.startIndex.advanced(by: 7)], 0)
    }
    
    func test_validateMutation_customMutableBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_customMutableBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_customMutableBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_customMutableBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_customMutableBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_customMutableBacking_appendBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customMutableBacking_appendData() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        data.append(Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customMutableBacking_appendBuffer() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customMutableBacking_appendSequence() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.append($0) }
        expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customMutableBacking_appendContentsOf() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        data.append(contentsOf: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
    }
    
    func test_validateMutation_slice_customMutableBacking_resetBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        data.resetBytes(in: 5..<8)
        
        expectEqual(data[data.startIndex.advanced(by: 1)], 0)
        expectEqual(data[data.startIndex.advanced(by: 2)], 0)
        expectEqual(data[data.startIndex.advanced(by: 3)], 0)
    }
    
    func test_validateMutation_slice_customMutableBacking_replaceSubrange() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_slice_customMutableBacking_replaceSubrangeRange() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBuffer() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_slice_customMutableBacking_replaceSubrangeWithCollection() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        data.replaceSubrange(range, with: [0xFF, 0xFF])
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
        let bytes: [UInt8] = [0xFF, 0xFF]
        bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count) }
        expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
    }
    
    func test_validateMutation_cow_customMutableBacking_withUnsafeMutableBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 5).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 5)], 0xFF)
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_appendBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_appendData() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_appendBuffer() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_appendSequence() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_appendContentsOf() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            data.append(contentsOf: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_resetBytes() {
        var data = Data(referencing: AllOnesData(length: 10))
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data.count, 10)
            expectEqual(data[data.startIndex.advanced(by: 0)], 1)
            expectEqual(data[data.startIndex.advanced(by: 5)], 0)
            expectEqual(data[data.startIndex.advanced(by: 6)], 0)
            expectEqual(data[data.startIndex.advanced(by: 7)], 0)
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_replaceSubrange() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0])) 
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_replaceSubrangeRange() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0])) 
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBuffer() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0])) 
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_replaceSubrangeWithCollection() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0])) 
        }
    }
    
    func test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBytes() {
        var data = Data(referencing: AllOnesData(length: 1))
        data.count = 10
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count) }
            expectEqual(data, Data(bytes: [1, 0xFF, 0xFF, 0])) 
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_withUnsafeMutableBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
                ptr.advanced(by: 1).pointee = 0xFF
            }
            expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_appendBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0.baseAddress!, count: $0.count) }
            expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_appendData() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            data.append(Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_appendBuffer() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.append($0) }
            expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_appendSequence() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            data.append(contentsOf: repeatElement(UInt8(0xFF), count: 2))
            expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_appendContentsOf() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            data.append(contentsOf: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [0, 0, 0, 0, 0, 0xFF, 0xFF]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_resetBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            data.resetBytes(in: 5..<8)
            expectEqual(data[data.startIndex.advanced(by: 1)], 0)
            expectEqual(data[data.startIndex.advanced(by: 2)], 0)
            expectEqual(data[data.startIndex.advanced(by: 3)], 0)
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_replaceSubrange() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeRange() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: Data(bytes: [0xFF, 0xFF]))
            expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBuffer() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0) }
            expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithCollection() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            data.replaceSubrange(range, with: [0xFF, 0xFF])
            expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
        }
    }
    
    func test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBytes() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<9]
        holdReference(data) {
            let range: Range<Int> = data.startIndex.advanced(by: 1)..<data.endIndex.advanced(by: -1)
            let bytes: [UInt8] = [0xFF, 0xFF]
            bytes.withUnsafeBufferPointer { data.replaceSubrange(range, with: $0.baseAddress!, count: $0.count) }
            expectEqual(data, Data(bytes: [0, 0xFF, 0xFF, 0]))
        }
    }
    
    func test_sliceHash() {
        let base1 = Data(bytes: [0, 0xFF, 0xFF, 0])
        let base2 = Data(bytes: [0, 0xFF, 0xFF, 0])
        let base3 = Data(bytes: [0xFF, 0xFF, 0xFF, 0])
        let sliceEmulation = Data(bytes: [0xFF, 0xFF])
        expectEqual(base1.hashValue, base2.hashValue)
        let slice1 = base1[base1.startIndex.advanced(by: 1)..<base1.endIndex.advanced(by: -1)]
        let slice2 = base2[base2.startIndex.advanced(by: 1)..<base2.endIndex.advanced(by: -1)]
        let slice3 = base3[base3.startIndex.advanced(by: 1)..<base3.endIndex.advanced(by: -1)]
        expectEqual(slice1.hashValue, sliceEmulation.hashValue)
        expectEqual(slice1.hashValue, slice2.hashValue)
        expectEqual(slice2.hashValue, slice3.hashValue)
    }

    func test_slice_resize_growth() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<9]
        data.resetBytes(in: data.endIndex.advanced(by: -1)..<data.endIndex.advanced(by: 1))
        expectEqual(data, Data(bytes: [4, 5, 6, 7, 0, 0]))
    }

    func test_hashEmptyData() {
        let d1 = Data()
        let h1 = d1.hashValue

        let d2 = NSData() as Data
        let h2 = d2.hashValue
        expectEqual(h1, h2)

        let data = Data(bytes: [0, 1, 2, 3, 4, 5, 6])
        let d3 = data[4..<4]
        let h3 = d3.hashValue
        expectEqual(h1, h3)
    }
    
    func test_validateMutation_slice_withUnsafeMutableBytes_lengthLessThanLowerBound() {
        var data = Data(bytes: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9])[4..<6]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data, Data(bytes: [4, 0xFF]))
    }
    
    func test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() {
        var data = Data(referencing: NSData(bytes: "hello world", length: 11))[4..<6]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() {
        var base = Data(referencing: NSData(bytes: "hello world", length: 11))
        base.append(contentsOf: [1, 2, 3, 4, 5, 6])
        var data = base[4..<6]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_customBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() {
        var data = Data(referencing: AllOnesImmutableData(length: 10))[4..<6]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }
    
    func test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() {
        var base = Data(referencing: AllOnesData(length: 1))
        base.count = 10
        var data = base[4..<6]
        data.withUnsafeMutableBytes { (ptr: UnsafeMutablePointer<UInt8>) in
            ptr.advanced(by: 1).pointee = 0xFF
        }
        expectEqual(data[data.startIndex.advanced(by: 1)], 0xFF)
    }

    func test_byte_access_of_discontiguousData() {
        var d = DispatchData.empty
        let bytes: [UInt8] = [0, 1, 2, 3, 4, 5]
        for _ in 0..<3 {
            bytes.withUnsafeBufferPointer {
                d.append($0)
            }
        }
        let ref = d as AnyObject
        let data = ref as! Data

        let cnt = data.count - 4
        let t = data.dropFirst(4).withUnsafeBytes { (bytes: UnsafePointer<UInt8>) in
            return Data(bytes: bytes, count: cnt)
        }

        expectEqual(Data(bytes: [4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5]), t)
    }

    func test_rangeOfSlice() {
        let data = "FooBar".data(using: .ascii)!
        let slice = data[3...] // Bar

        let range = slice.range(of: "a".data(using: .ascii)!)
        expectEqual(range, 4..<5)
    }

    func test_nsdataSequence() {
        if #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) {
            let bytes: [UInt8] = Array(0x00...0xFF)
            let data = bytes.withUnsafeBytes { NSData(bytes: $0.baseAddress, length: $0.count) }

            for byte in bytes {
                expectEqual(data[Int(byte)], byte)
            }
        }
    }

    func test_dispatchSequence() {
        if #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) {
            let bytes1: [UInt8] = Array(0x00..<0xF0)
            let bytes2: [UInt8] = Array(0xF0..<0xFF)
            var data = DispatchData.empty
            bytes1.withUnsafeBytes {
                data.append($0)
            }
            bytes2.withUnsafeBytes {
                data.append($0)
            }

            for byte in bytes1 {
                expectEqual(data[Int(byte)], byte)
            }
            for byte in bytes2 {
                expectEqual(data[Int(byte)], byte)
            }
        }
    }

    func test_increaseCount() {
        guard #available(macOS 10.16, iOS 14.0, watchOS 7.0, tvOS 14.0, *) else { return }
        let initials: [Range<UInt8>] = [
            0..<0,
            0..<2,
            0..<4,
            0..<8,
            0..<16,
            0..<32,
            0..<64
        ]
        let diffs = [0, 1, 2, 4, 8, 16, 32]
        for initial in initials {
            for diff in diffs {
                var data = Data(initial)
                data.count += diff
                expectEqualSequence(
                    Array(initial) + Array(repeating: 0, count: diff),
                    data)
            }
        }
    }

    func test_decreaseCount() {
        guard #available(macOS 10.16, iOS 14.0, watchOS 7.0, tvOS 14.0, *) else { return }
        let initials: [Range<UInt8>] = [
            0..<0,
            0..<2,
            0..<4,
            0..<8,
            0..<16,
            0..<32,
            0..<64
        ]
        let diffs = [0, 1, 2, 4, 8, 16, 32]
        for initial in initials {
            for diff in diffs {
                guard initial.count >= diff else { continue }
                var data = Data(initial)
                data.count -= diff
                expectEqualSequence(
                    initial.dropLast(diff),
                    data)
            }
        }
    }

    // This is a (potentially invalid) sequence that produces a configurable number of 42s and has a freely customizable `underestimatedCount`.
    struct TestSequence: Sequence {
        typealias Element = UInt8
        struct Iterator: IteratorProtocol {
            var _remaining: Int
            init(_ count: Int) {
                _remaining = count
            }
            mutating func next() -> UInt8? {
                guard _remaining > 0 else { return nil }
                _remaining -= 1
                return 42
            }
        }
        let underestimatedCount: Int
        let count: Int

        func makeIterator() -> Iterator {
            return Iterator(count)
        }
    }

    func test_init_TestSequence() {
        // Underestimated count
        do {
            let d = Data(TestSequence(underestimatedCount: 0, count: 10))
            expectEqual(10, d.count)
            expectEqual(Array(repeating: 42 as UInt8, count: 10), Array(d))
        }

        // Very underestimated count (to exercise realloc path)
        do {
            let d = Data(TestSequence(underestimatedCount: 0, count: 1000))
            expectEqual(1000, d.count)
            expectEqual(Array(repeating: 42 as UInt8, count: 1000), Array(d))
        }

        // Exact count
        do {
            let d = Data(TestSequence(underestimatedCount: 10, count: 10))
            expectEqual(10, d.count)
            expectEqual(Array(repeating: 42 as UInt8, count: 10), Array(d))
        }

        // Overestimated count. This is an illegal case, so trapping would be fine.
        // However, for compatibility with the implementation in Swift 5, Data
        // handles this case by simply truncating itself to the actual size.
        do {
            let d = Data(TestSequence(underestimatedCount: 20, count: 10))
            expectEqual(10, d.count)
            expectEqual(Array(repeating: 42 as UInt8, count: 10), Array(d))
        }
    }

    func test_append_TestSequence() {
        let base = Data(Array(repeating: 23 as UInt8, count: 10))

        // Underestimated count
        do {
            var d = base
            d.append(contentsOf: TestSequence(underestimatedCount: 0, count: 10))
            expectEqual(20, d.count)
            expectEqual(Array(base) + Array(repeating: 42 as UInt8, count: 10),
                           Array(d))
        }

        // Very underestimated count (to exercise realloc path)
        do {
            var d = base
            d.append(contentsOf: TestSequence(underestimatedCount: 0, count: 1000))
            expectEqual(1010, d.count)
            expectEqual(Array(base) + Array(repeating: 42 as UInt8, count: 1000), Array(d))
        }

        // Exact count
        do {
            var d = base
            d.append(contentsOf: TestSequence(underestimatedCount: 10, count: 10))
            expectEqual(20, d.count)
            expectEqual(Array(base) + Array(repeating: 42 as UInt8, count: 10), Array(d))
        }

        // Overestimated count. This is an illegal case, so trapping would be fine.
        // However, for compatibility with the implementation in Swift 5, Data
        // handles this case by simply truncating itself to the actual size.
        do {
            var d = base
            d.append(contentsOf: TestSequence(underestimatedCount: 20, count: 10))
            expectEqual(20, d.count)
            expectEqual(Array(base) + Array(repeating: 42 as UInt8, count: 10), Array(d))
        }
    }
}

#if !FOUNDATION_XCTEST
var DataTests = TestSuite("TestData")
DataTests.test("testBasicConstruction") { TestData().testBasicConstruction() }
DataTests.test("testInitializationWithArray") { TestData().testInitializationWithArray() }
DataTests.test("testInitializationWithBufferPointer") { TestData().testInitializationWithBufferPointer() }
DataTests.test("testInitializationWithMutableBufferPointer") { TestData().testInitializationWithMutableBufferPointer() }
DataTests.test("testMutableData") { TestData().testMutableData() }
DataTests.test("testCustomData") { TestData().testCustomData() }
DataTests.test("testBridgingDefault") { TestData().testBridgingDefault() }
DataTests.test("testBridgingMutable") { TestData().testBridgingMutable() }
DataTests.test("testBridgingCustom") { TestData().testBridgingCustom() }
DataTests.test("testEquality") { TestData().testEquality() }
DataTests.test("testDataInSet") { TestData().testDataInSet() }
DataTests.test("testReplaceSubrange") { TestData().testReplaceSubrange() }
DataTests.test("testReplaceSubrange2") { TestData().testReplaceSubrange2() }
DataTests.test("testReplaceSubrange3") { TestData().testReplaceSubrange3() }
DataTests.test("testReplaceSubrange4") { TestData().testReplaceSubrange4() }
DataTests.test("testReplaceSubrange5") { TestData().testReplaceSubrange5() }
DataTests.test("testRange") { TestData().testRange() }
DataTests.test("testInsertData") { TestData().testInsertData() }
DataTests.test("testLoops") { TestData().testLoops() }
DataTests.test("testGenericAlgorithms") { TestData().testGenericAlgorithms() }
DataTests.test("testCustomDeallocator") { TestData().testCustomDeallocator() }
DataTests.test("testCopyBytes") { TestData().testCopyBytes() }
DataTests.test("testCopyBytes_undersized") { TestData().testCopyBytes_undersized() }
DataTests.test("testCopyBytes_oversized") { TestData().testCopyBytes_oversized() }
DataTests.test("testCopyBytes_ranges") { TestData().testCopyBytes_ranges() }
DataTests.test("test_base64Data_small") { TestData().test_base64Data_small() }
DataTests.test("test_base64Data_medium") { TestData().test_base64Data_medium() }
DataTests.test("test_discontiguousEnumerateBytes") { TestData().test_discontiguousEnumerateBytes() }
DataTests.test("test_basicReadWrite") { TestData().test_basicReadWrite() }
DataTests.test("test_writeFailure") { TestData().test_writeFailure() }
DataTests.test("test_genericBuffers") { TestData().test_genericBuffers() }
DataTests.test("test_basicDataMutation") { TestData().test_basicDataMutation() }
DataTests.test("test_basicMutableDataMutation") { TestData().test_basicMutableDataMutation() }
DataTests.test("test_passing") { TestData().test_passing() }
DataTests.test("test_bufferSizeCalculation") { TestData().test_bufferSizeCalculation() }
DataTests.test("test_classForCoder") { TestData().test_classForCoder() }
DataTests.test("test_AnyHashableContainingData") { TestData().test_AnyHashableContainingData() }
DataTests.test("test_AnyHashableCreatedFromNSData") { TestData().test_AnyHashableCreatedFromNSData() }
DataTests.test("test_noCopyBehavior") { TestData().test_noCopyBehavior() }
DataTests.test("test_doubleDeallocation") { TestData().test_doubleDeallocation() }
DataTests.test("test_repeatingValueInitialization") { TestData().test_repeatingValueInitialization() }
DataTests.test("test_rangeZoo") { TestData().test_rangeZoo() }
DataTests.test("test_rangeOfDataProtocol") { TestData().test_rangeOfDataProtocol() }
DataTests.test("test_sliceAppending") { TestData().test_sliceAppending() }
DataTests.test("test_replaceSubrange") { TestData().test_replaceSubrange() }
DataTests.test("test_sliceWithUnsafeBytes") { TestData().test_sliceWithUnsafeBytes() }
DataTests.test("test_sliceIteration") { TestData().test_sliceIteration() }
DataTests.test("test_unconditionallyBridgeFromObjectiveC") { TestData().test_unconditionallyBridgeFromObjectiveC() }
DataTests.test("test_sliceIndexing") { TestData().test_sliceIndexing() }
DataTests.test("test_sliceEquality") { TestData().test_sliceEquality() }
DataTests.test("test_sliceEquality2") { TestData().test_sliceEquality2() }
DataTests.test("test_splittingHttp") { TestData().test_splittingHttp() }
DataTests.test("test_map") { TestData().test_map() }
DataTests.test("test_dropFirst") { TestData().test_dropFirst() }
DataTests.test("test_dropFirst2") { TestData().test_dropFirst2() }
DataTests.test("test_copyBytes1") { TestData().test_copyBytes1() }
DataTests.test("test_copyBytes2") { TestData().test_copyBytes2() }
DataTests.test("test_sliceOfSliceViaRangeExpression") { TestData().test_sliceOfSliceViaRangeExpression() }
DataTests.test("test_appendingSlices") { TestData().test_appendingSlices() }
DataTests.test("test_appendingNonContiguousSequence_exactCount") { TestData().test_appendingNonContiguousSequence_exactCount() }
DataTests.test("test_appendingNonContiguousSequence_underestimatedCount") { TestData().test_appendingNonContiguousSequence_underestimatedCount() }
DataTests.test("test_sequenceInitializers") { TestData().test_sequenceInitializers() }
DataTests.test("test_reversedDataInit") { TestData().test_reversedDataInit() }
DataTests.test("test_replaceSubrangeReferencingMutable") { TestData().test_replaceSubrangeReferencingMutable() }
DataTests.test("test_replaceSubrangeReferencingImmutable") { TestData().test_replaceSubrangeReferencingImmutable() }
DataTests.test("test_replaceSubrangeFromBridged") { TestData().test_replaceSubrangeFromBridged() }
DataTests.test("test_validateMutation_withUnsafeMutableBytes") { TestData().test_validateMutation_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_appendBytes") { TestData().test_validateMutation_appendBytes() }
DataTests.test("test_validateMutation_appendData") { TestData().test_validateMutation_appendData() }
DataTests.test("test_validateMutation_appendBuffer") { TestData().test_validateMutation_appendBuffer() }
DataTests.test("test_validateMutation_appendSequence") { TestData().test_validateMutation_appendSequence() }
DataTests.test("test_validateMutation_appendContentsOf") { TestData().test_validateMutation_appendContentsOf() }
DataTests.test("test_validateMutation_resetBytes") { TestData().test_validateMutation_resetBytes() }
DataTests.test("test_validateMutation_replaceSubrange") { TestData().test_validateMutation_replaceSubrange() }
DataTests.test("test_validateMutation_replaceSubrangeRange") { TestData().test_validateMutation_replaceSubrangeRange() }
DataTests.test("test_validateMutation_replaceSubrangeWithBuffer") { TestData().test_validateMutation_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_replaceSubrangeWithCollection") { TestData().test_validateMutation_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_replaceSubrangeWithBytes") { TestData().test_validateMutation_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_appendBytes") { TestData().test_validateMutation_slice_appendBytes() }
DataTests.test("test_validateMutation_slice_appendData") { TestData().test_validateMutation_slice_appendData() }
DataTests.test("test_validateMutation_slice_appendBuffer") { TestData().test_validateMutation_slice_appendBuffer() }
DataTests.test("test_validateMutation_slice_appendSequence") { TestData().test_validateMutation_slice_appendSequence() }
DataTests.test("test_validateMutation_slice_appendContentsOf") { TestData().test_validateMutation_slice_appendContentsOf() }
DataTests.test("test_validateMutation_slice_resetBytes") { TestData().test_validateMutation_slice_resetBytes() }
DataTests.test("test_validateMutation_slice_replaceSubrange") { TestData().test_validateMutation_slice_replaceSubrange() }
DataTests.test("test_validateMutation_slice_replaceSubrangeRange") { TestData().test_validateMutation_slice_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_cow_withUnsafeMutableBytes") { TestData().test_validateMutation_cow_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_cow_appendBytes") { TestData().test_validateMutation_cow_appendBytes() }
DataTests.test("test_validateMutation_cow_appendData") { TestData().test_validateMutation_cow_appendData() }
DataTests.test("test_validateMutation_cow_appendBuffer") { TestData().test_validateMutation_cow_appendBuffer() }
DataTests.test("test_validateMutation_cow_appendSequence") { TestData().test_validateMutation_cow_appendSequence() }
DataTests.test("test_validateMutation_cow_appendContentsOf") { TestData().test_validateMutation_cow_appendContentsOf() }
DataTests.test("test_validateMutation_cow_resetBytes") { TestData().test_validateMutation_cow_resetBytes() }
DataTests.test("test_validateMutation_cow_replaceSubrange") { TestData().test_validateMutation_cow_replaceSubrange() }
DataTests.test("test_validateMutation_cow_replaceSubrangeRange") { TestData().test_validateMutation_cow_replaceSubrangeRange() }
DataTests.test("test_validateMutation_cow_replaceSubrangeWithBuffer") { TestData().test_validateMutation_cow_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_cow_replaceSubrangeWithCollection") { TestData().test_validateMutation_cow_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_cow_replaceSubrangeWithBytes") { TestData().test_validateMutation_cow_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_cow_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_cow_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_cow_appendBytes") { TestData().test_validateMutation_slice_cow_appendBytes() }
DataTests.test("test_validateMutation_slice_cow_appendData") { TestData().test_validateMutation_slice_cow_appendData() }
DataTests.test("test_validateMutation_slice_cow_appendBuffer") { TestData().test_validateMutation_slice_cow_appendBuffer() }
DataTests.test("test_validateMutation_slice_cow_appendSequence") { TestData().test_validateMutation_slice_cow_appendSequence() }
DataTests.test("test_validateMutation_slice_cow_appendContentsOf") { TestData().test_validateMutation_slice_cow_appendContentsOf() }
DataTests.test("test_validateMutation_slice_cow_resetBytes") { TestData().test_validateMutation_slice_cow_resetBytes() }
DataTests.test("test_validateMutation_slice_cow_replaceSubrange") { TestData().test_validateMutation_slice_cow_replaceSubrange() }
DataTests.test("test_validateMutation_slice_cow_replaceSubrangeRange") { TestData().test_validateMutation_slice_cow_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_cow_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_cow_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_cow_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_cow_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_cow_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_cow_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_immutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_immutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_immutableBacking_appendBytes") { TestData().test_validateMutation_immutableBacking_appendBytes() }
DataTests.test("test_validateMutation_immutableBacking_appendData") { TestData().test_validateMutation_immutableBacking_appendData() }
DataTests.test("test_validateMutation_immutableBacking_appendBuffer") { TestData().test_validateMutation_immutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_immutableBacking_appendSequence") { TestData().test_validateMutation_immutableBacking_appendSequence() }
DataTests.test("test_validateMutation_immutableBacking_appendContentsOf") { TestData().test_validateMutation_immutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_immutableBacking_resetBytes") { TestData().test_validateMutation_immutableBacking_resetBytes() }
DataTests.test("test_validateMutation_immutableBacking_replaceSubrange") { TestData().test_validateMutation_immutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_immutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_immutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_immutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_immutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_immutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_immutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_immutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_immutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_immutableBacking_appendBytes") { TestData().test_validateMutation_slice_immutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_immutableBacking_appendData") { TestData().test_validateMutation_slice_immutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_immutableBacking_appendBuffer") { TestData().test_validateMutation_slice_immutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_immutableBacking_appendSequence") { TestData().test_validateMutation_slice_immutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_immutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_immutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_immutableBacking_resetBytes") { TestData().test_validateMutation_slice_immutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_immutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_immutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_immutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_immutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_immutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_immutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_immutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_immutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_immutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_immutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_cow_immutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_cow_immutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_cow_immutableBacking_appendBytes") { TestData().test_validateMutation_cow_immutableBacking_appendBytes() }
DataTests.test("test_validateMutation_cow_immutableBacking_appendData") { TestData().test_validateMutation_cow_immutableBacking_appendData() }
DataTests.test("test_validateMutation_cow_immutableBacking_appendBuffer") { TestData().test_validateMutation_cow_immutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_cow_immutableBacking_appendSequence") { TestData().test_validateMutation_cow_immutableBacking_appendSequence() }
DataTests.test("test_validateMutation_cow_immutableBacking_appendContentsOf") { TestData().test_validateMutation_cow_immutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_cow_immutableBacking_resetBytes") { TestData().test_validateMutation_cow_immutableBacking_resetBytes() }
DataTests.test("test_validateMutation_cow_immutableBacking_replaceSubrange") { TestData().test_validateMutation_cow_immutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_cow_immutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_cow_immutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_cow_immutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_cow_immutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_cow_immutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_cow_immutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_cow_immutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_cow_immutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_cow_immutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_appendBytes") { TestData().test_validateMutation_slice_cow_immutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_appendData") { TestData().test_validateMutation_slice_cow_immutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_appendBuffer") { TestData().test_validateMutation_slice_cow_immutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_appendSequence") { TestData().test_validateMutation_slice_cow_immutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_cow_immutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_resetBytes") { TestData().test_validateMutation_slice_cow_immutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_cow_immutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_cow_immutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_cow_immutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_mutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_mutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_mutableBacking_appendBytes") { TestData().test_validateMutation_mutableBacking_appendBytes() }
DataTests.test("test_validateMutation_mutableBacking_appendData") { TestData().test_validateMutation_mutableBacking_appendData() }
DataTests.test("test_validateMutation_mutableBacking_appendBuffer") { TestData().test_validateMutation_mutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_mutableBacking_appendSequence") { TestData().test_validateMutation_mutableBacking_appendSequence() }
DataTests.test("test_validateMutation_mutableBacking_appendContentsOf") { TestData().test_validateMutation_mutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_mutableBacking_resetBytes") { TestData().test_validateMutation_mutableBacking_resetBytes() }
DataTests.test("test_validateMutation_mutableBacking_replaceSubrange") { TestData().test_validateMutation_mutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_mutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_mutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_mutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_mutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_mutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_mutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_mutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_mutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_mutableBacking_appendBytes") { TestData().test_validateMutation_slice_mutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_mutableBacking_appendData") { TestData().test_validateMutation_slice_mutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_mutableBacking_appendBuffer") { TestData().test_validateMutation_slice_mutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_mutableBacking_appendSequence") { TestData().test_validateMutation_slice_mutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_mutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_mutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_mutableBacking_resetBytes") { TestData().test_validateMutation_slice_mutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_mutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_mutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_mutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_mutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_mutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_mutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_mutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_mutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_mutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_mutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_cow_mutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_cow_mutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_cow_mutableBacking_appendBytes") { TestData().test_validateMutation_cow_mutableBacking_appendBytes() }
DataTests.test("test_validateMutation_cow_mutableBacking_appendData") { TestData().test_validateMutation_cow_mutableBacking_appendData() }
DataTests.test("test_validateMutation_cow_mutableBacking_appendBuffer") { TestData().test_validateMutation_cow_mutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_cow_mutableBacking_appendSequence") { TestData().test_validateMutation_cow_mutableBacking_appendSequence() }
DataTests.test("test_validateMutation_cow_mutableBacking_appendContentsOf") { TestData().test_validateMutation_cow_mutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_cow_mutableBacking_resetBytes") { TestData().test_validateMutation_cow_mutableBacking_resetBytes() }
DataTests.test("test_validateMutation_cow_mutableBacking_replaceSubrange") { TestData().test_validateMutation_cow_mutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_cow_mutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_cow_mutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_cow_mutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_cow_mutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_cow_mutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_cow_mutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_cow_mutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_cow_mutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_cow_mutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_appendBytes") { TestData().test_validateMutation_slice_cow_mutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_appendData") { TestData().test_validateMutation_slice_cow_mutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_appendBuffer") { TestData().test_validateMutation_slice_cow_mutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_appendSequence") { TestData().test_validateMutation_slice_cow_mutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_cow_mutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_resetBytes") { TestData().test_validateMutation_slice_cow_mutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_cow_mutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_cow_mutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_cow_mutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_customBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_customBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_customBacking_appendBytes") { TestData().test_validateMutation_customBacking_appendBytes() }
DataTests.test("test_validateMutation_customBacking_appendData") { TestData().test_validateMutation_customBacking_appendData() }
DataTests.test("test_validateMutation_customBacking_appendBuffer") { TestData().test_validateMutation_customBacking_appendBuffer() }
DataTests.test("test_validateMutation_customBacking_appendSequence") { TestData().test_validateMutation_customBacking_appendSequence() }
DataTests.test("test_validateMutation_customBacking_appendContentsOf") { TestData().test_validateMutation_customBacking_appendContentsOf() }
DataTests.test("test_validateMutation_customBacking_resetBytes") { TestData().test_validateMutation_customBacking_resetBytes() }
DataTests.test("test_validateMutation_customBacking_replaceSubrange") { TestData().test_validateMutation_customBacking_replaceSubrange() }
DataTests.test("test_validateMutation_customBacking_replaceSubrangeRange") { TestData().test_validateMutation_customBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_customBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_customBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_customBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_customBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_customBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_customBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_customBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_customBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_customBacking_appendBytes") { TestData().test_validateMutation_slice_customBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_customBacking_appendData") { TestData().test_validateMutation_slice_customBacking_appendData() }
DataTests.test("test_validateMutation_slice_customBacking_appendBuffer") { TestData().test_validateMutation_slice_customBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_customBacking_appendSequence") { TestData().test_validateMutation_slice_customBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_customBacking_appendContentsOf") { TestData().test_validateMutation_slice_customBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_customBacking_resetBytes") { TestData().test_validateMutation_slice_customBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_customBacking_replaceSubrange") { TestData().test_validateMutation_slice_customBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_customBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_customBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_customBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_customBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_customBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_customBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_customBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_customBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_cow_customBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_cow_customBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_cow_customBacking_appendBytes") { TestData().test_validateMutation_cow_customBacking_appendBytes() }
DataTests.test("test_validateMutation_cow_customBacking_appendData") { TestData().test_validateMutation_cow_customBacking_appendData() }
DataTests.test("test_validateMutation_cow_customBacking_appendBuffer") { TestData().test_validateMutation_cow_customBacking_appendBuffer() }
DataTests.test("test_validateMutation_cow_customBacking_appendSequence") { TestData().test_validateMutation_cow_customBacking_appendSequence() }
DataTests.test("test_validateMutation_cow_customBacking_appendContentsOf") { TestData().test_validateMutation_cow_customBacking_appendContentsOf() }
DataTests.test("test_validateMutation_cow_customBacking_resetBytes") { TestData().test_validateMutation_cow_customBacking_resetBytes() }
DataTests.test("test_validateMutation_cow_customBacking_replaceSubrange") { TestData().test_validateMutation_cow_customBacking_replaceSubrange() }
DataTests.test("test_validateMutation_cow_customBacking_replaceSubrangeRange") { TestData().test_validateMutation_cow_customBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_cow_customBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_cow_customBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_cow_customBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_cow_customBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_cow_customBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_cow_customBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_cow_customBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_cow_customBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_cow_customBacking_appendBytes") { TestData().test_validateMutation_slice_cow_customBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_cow_customBacking_appendData") { TestData().test_validateMutation_slice_cow_customBacking_appendData() }
DataTests.test("test_validateMutation_slice_cow_customBacking_appendBuffer") { TestData().test_validateMutation_slice_cow_customBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_cow_customBacking_appendSequence") { TestData().test_validateMutation_slice_cow_customBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_cow_customBacking_appendContentsOf") { TestData().test_validateMutation_slice_cow_customBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_cow_customBacking_resetBytes") { TestData().test_validateMutation_slice_cow_customBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_cow_customBacking_replaceSubrange") { TestData().test_validateMutation_slice_cow_customBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_cow_customBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_cow_customBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_cow_customBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_cow_customBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_cow_customBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_customMutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_customMutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_customMutableBacking_appendBytes") { TestData().test_validateMutation_customMutableBacking_appendBytes() }
DataTests.test("test_validateMutation_customMutableBacking_appendData") { TestData().test_validateMutation_customMutableBacking_appendData() }
DataTests.test("test_validateMutation_customMutableBacking_appendBuffer") { TestData().test_validateMutation_customMutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_customMutableBacking_appendSequence") { TestData().test_validateMutation_customMutableBacking_appendSequence() }
DataTests.test("test_validateMutation_customMutableBacking_appendContentsOf") { TestData().test_validateMutation_customMutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_customMutableBacking_resetBytes") { TestData().test_validateMutation_customMutableBacking_resetBytes() }
DataTests.test("test_validateMutation_customMutableBacking_replaceSubrange") { TestData().test_validateMutation_customMutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_customMutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_customMutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_customMutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_customMutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_customMutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_customMutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_customMutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_customMutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_customMutableBacking_appendBytes") { TestData().test_validateMutation_slice_customMutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_customMutableBacking_appendData") { TestData().test_validateMutation_slice_customMutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_customMutableBacking_appendBuffer") { TestData().test_validateMutation_slice_customMutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_customMutableBacking_appendSequence") { TestData().test_validateMutation_slice_customMutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_customMutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_customMutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_customMutableBacking_resetBytes") { TestData().test_validateMutation_slice_customMutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_customMutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_customMutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_customMutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_customMutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_customMutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_customMutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_customMutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_cow_customMutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_cow_customMutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_cow_customMutableBacking_appendBytes") { TestData().test_validateMutation_cow_customMutableBacking_appendBytes() }
DataTests.test("test_validateMutation_cow_customMutableBacking_appendData") { TestData().test_validateMutation_cow_customMutableBacking_appendData() }
DataTests.test("test_validateMutation_cow_customMutableBacking_appendBuffer") { TestData().test_validateMutation_cow_customMutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_cow_customMutableBacking_appendSequence") { TestData().test_validateMutation_cow_customMutableBacking_appendSequence() }
DataTests.test("test_validateMutation_cow_customMutableBacking_appendContentsOf") { TestData().test_validateMutation_cow_customMutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_cow_customMutableBacking_resetBytes") { TestData().test_validateMutation_cow_customMutableBacking_resetBytes() }
DataTests.test("test_validateMutation_cow_customMutableBacking_replaceSubrange") { TestData().test_validateMutation_cow_customMutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_cow_customMutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_cow_customMutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_cow_customMutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_cow_customMutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_cow_customMutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_withUnsafeMutableBytes") { TestData().test_validateMutation_slice_cow_customMutableBacking_withUnsafeMutableBytes() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_appendBytes") { TestData().test_validateMutation_slice_cow_customMutableBacking_appendBytes() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_appendData") { TestData().test_validateMutation_slice_cow_customMutableBacking_appendData() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_appendBuffer") { TestData().test_validateMutation_slice_cow_customMutableBacking_appendBuffer() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_appendSequence") { TestData().test_validateMutation_slice_cow_customMutableBacking_appendSequence() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_appendContentsOf") { TestData().test_validateMutation_slice_cow_customMutableBacking_appendContentsOf() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_resetBytes") { TestData().test_validateMutation_slice_cow_customMutableBacking_resetBytes() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_replaceSubrange") { TestData().test_validateMutation_slice_cow_customMutableBacking_replaceSubrange() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeRange") { TestData().test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeRange() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBuffer") { TestData().test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBuffer() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithCollection") { TestData().test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithCollection() }
DataTests.test("test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBytes") { TestData().test_validateMutation_slice_cow_customMutableBacking_replaceSubrangeWithBytes() }
DataTests.test("test_sliceHash") { TestData().test_sliceHash() }
DataTests.test("test_slice_resize_growth") { TestData().test_slice_resize_growth() }
DataTests.test("test_hashEmptyData") { TestData().test_hashEmptyData() }
DataTests.test("test_validateMutation_slice_withUnsafeMutableBytes_lengthLessThanLowerBound") { TestData().test_validateMutation_slice_withUnsafeMutableBytes_lengthLessThanLowerBound() }
DataTests.test("test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound") { TestData().test_validateMutation_slice_immutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() }
DataTests.test("test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound") { TestData().test_validateMutation_slice_mutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() }
DataTests.test("test_validateMutation_slice_customBacking_withUnsafeMutableBytes_lengthLessThanLowerBound") { TestData().test_validateMutation_slice_customBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() }
DataTests.test("test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound") { TestData().test_validateMutation_slice_customMutableBacking_withUnsafeMutableBytes_lengthLessThanLowerBound() }
DataTests.test("test_byte_access_of_discontiguousData") { TestData().test_byte_access_of_discontiguousData() }
DataTests.test("test_rangeOfSlice") { TestData().test_rangeOfSlice() }
if #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) {
    DataTests.test("test_nsdataSequence") { TestData().test_nsdataSequence() }
    DataTests.test("test_dispatchSequence") { TestData().test_dispatchSequence() }
}
DataTests.test("test_increaseCount") { TestData().test_increaseCount() }
DataTests.test("test_decreaseCount") { TestData().test_decreaseCount() }
DataTests.test("test_increaseCount") { TestData().test_init_TestSequence() }
DataTests.test("test_decreaseCount") { TestData().test_append_TestSequence() }


// XCTest does not have a crash detection, whereas lit does
DataTests.test("bounding failure subdata") {
    let data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    let c = data.subdata(in: 5..<200)
}

DataTests.test("bounding failure replace") {
    var data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    data.replaceSubrange(5..<200, with: Data())
}

DataTests.test("bounding failure replace2") {
    var data = "a".data(using: .utf8)!
    var bytes : [UInt8] = [1, 2, 3]
    expectCrashLater()
    bytes.withUnsafeBufferPointer {
        // lowerBound ok, upperBound after end of data
        data.replaceSubrange(0..<2, with: $0)
    }
}

DataTests.test("bounding failure replace3") {
    var data = "a".data(using: .utf8)!
    var bytes : [UInt8] = [1, 2, 3]
    expectCrashLater()
    bytes.withUnsafeBufferPointer {
        // lowerBound is > length
        data.replaceSubrange(2..<4, with: $0)
    }
}

DataTests.test("bounding failure replace4") {
    var data = "a".data(using: .utf8)!
    var bytes : [UInt8] = [1, 2, 3]
    expectCrashLater()
    // lowerBound is > length
    data.replaceSubrange(2..<4, with: bytes)
}

DataTests.test("bounding failure reset range") {
    var data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    data.resetBytes(in: 100..<200)
}

DataTests.test("bounding failure append bad length") {
    var data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    data.append("hello", count: -2)
}

DataTests.test("bounding failure append absurd length") {
    var data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    data.append("hello", count: Int.min)
}

DataTests.test("bounding failure subscript") {
    var data = "Hello World".data(using: .utf8)!
    expectCrashLater()
    data[100] = 4
}


runAllTests()
#endif
