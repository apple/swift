SWIFT_SRCROOT=${CURDIR}/../..
SRCROOT=${SWIFT_SRCROOT}/..

HEADERS=${SWIFT_SRCROOT}/include/swift/Basic/HeaderFooterLayout.h

CXXFLAGS=-Wall -std=c++17 -stdlib=libc++ -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -I${OBJROOT}/include -I${SWIFT_SRCROOT}/include

TestHeaderFooterLayout: TestHeaderFooterLayout.o
	$(CXX) $< -o $@

TestHeaderFooterLayout.o: ${HEADERS}
