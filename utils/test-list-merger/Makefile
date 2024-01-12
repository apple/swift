SWIFT_SRCROOT=${CURDIR}/../..
SRCROOT=${SWIFT_SRCROOT}/..
LLVM_SRCROOT=${SRCROOT}/llvm/
LLVM_OBJROOT=${SRCROOT}/build/Ninja-DebugAssert/llvm-macosx-$(shell uname -m)

HEADERS=${SWIFT_SRCROOT}/include/swift/Basic/ListMerger.h

CXXFLAGS=-Wall -std=c++17 -stdlib=libc++ -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -I${OBJROOT}/include -I${SWIFT_SRCROOT}/include -I${LLVM_SRCROOT}/include -I${LLVM_OBJROOT}/include

TestListMerger: TestListMerger.o
	$(CXX) -L${LLVM_OBJROOT}/lib -lLLVMSupport $< -o $@

TestListMerger.o: ${HEADERS}
