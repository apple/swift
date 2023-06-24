# If you update this, you also need to update the CMakeLists.txt file in
# stdlib/public/Threading AND the one in stdlib/toolchain/CompatibilityThreading

# Note that it is *not* an error that Errors.cpp is only listed here.
# It shouldn't be in the other places because those are OBJECT_LIBRARYs
# and things that use them should be defining their own fatal error handler.

add_swift_host_library(swiftThreading STATIC
  C11.cpp
  Linux.cpp
  Pthreads.cpp
  Win32.cpp
  Errors.cpp
  ThreadSanitizer.cpp)
