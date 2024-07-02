#include "swift/Basic/Sandbox.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/StringExtras.h"
#include "llvm/ADT/SmallString.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

using namespace swift;
using namespace Sandbox;

#if defined(__APPLE__) && TARGET_OS_OSX
static StringRef sandboxProfile(llvm::BumpPtrAllocator &Alloc) {
  llvm::SmallString<256> contents;
  contents += "(version 1)\n";

  // Deny everything by default.
  contents += "(deny default)\n";

  // Import the system sandbox profile.
  contents += "(import \"system.sb\")\n";

  // Allow reading file metadata of any files.
  contents += "(allow file-read-metadata)\n";

  // Allow reading dylibs and WebAssembly macros.
  contents += "(allow file-read* (regex #\"\\.(dylib|wasm)$\"))\n";

  // This is required to launch any processes (execve(2)).
  contents += "(allow process-exec*)\n";

  return NullTerminatedStringRef(StringRef(contents), Alloc);
}
#endif

bool swift::Sandbox::apply(llvm::SmallVectorImpl<llvm::StringRef> &command,
                           llvm::BumpPtrAllocator &Alloc) {
#if defined(__APPLE__) && TARGET_OS_OSX
  auto profile = sandboxProfile(Alloc);
  command.insert(command.begin(), {"/usr/bin/sandbox-exec", "-p", profile});
#endif
  return false;
}
