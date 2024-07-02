//===--- ToolChains.h - Platform-specific ToolChain logic -------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_TOOLCHAINS_H
#define SWIFT_DRIVER_TOOLCHAINS_H

#include "swift/Basic/LLVM.h"
#include "swift/Driver/ToolChain.h"
#include "clang/Basic/DarwinSDKInfo.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Compiler.h"

namespace swift {
class DiagnosticEngine;

namespace driver {
namespace toolchains {

class LLVM_LIBRARY_VISIBILITY Darwin : public ToolChain {
protected:

  void addLinkerInputArgs(InvocationInfo &II,
                          const JobContext &context) const;

  void addSanitizerArgs(llvm::opt::ArgStringList &Arguments,
                        const DynamicLinkJobAction &job,
                        const JobContext &context) const;

  void addArgsToLinkStdlib(llvm::opt::ArgStringList &Arguments,
                           const DynamicLinkJobAction &job,
                           const JobContext &context) const;

  void addProfileGenerationArgs(llvm::opt::ArgStringList &Arguments,
                                const JobContext &context) const;

  void addDeploymentTargetArgs(llvm::opt::ArgStringList &Arguments,
                               const JobContext &context) const;

  void addLTOLibArgs(llvm::opt::ArgStringList &Arguments,
                     const JobContext &context) const;

  void addCommonFrontendArgs(
      const OutputInfo &OI, const CommandOutput &output,
      const llvm::opt::ArgList &inputArgs,
      llvm::opt::ArgStringList &arguments) const override;

  void addPlatformSpecificPluginFrontendArgs(
      const OutputInfo &OI,
      const CommandOutput &output,
      const llvm::opt::ArgList &inputArgs,
      llvm::opt::ArgStringList &arguments) const override;

  InvocationInfo constructInvocation(const InterpretJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const DynamicLinkJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const StaticLinkJobAction &job,
                                     const JobContext &context) const override;

  void validateArguments(DiagnosticEngine &diags,
                         const llvm::opt::ArgList &args,
                         StringRef defaultTarget) const override;

  void validateOutputInfo(DiagnosticEngine &diags,
                          const OutputInfo &outputInfo) const override;

  std::string findProgramRelativeToSwiftImpl(StringRef name) const override;

  bool shouldStoreInvocationInDebugInfo() const override;
  std::string getGlobalDebugPathRemapping() const override;
  
  /// Retrieve the target SDK version for the given target triple.
  std::optional<llvm::VersionTuple>
  getTargetSDKVersion(const llvm::Triple &triple) const;

  /// Information about the SDK that the application is being built against.
  /// This information is only used by the linker, so it is only populated
  /// when there will be a linker job.
  mutable std::optional<clang::DarwinSDKInfo> SDKInfo;

  const std::optional<llvm::Triple> TargetVariant;

public:
  Darwin(const Driver &D, const llvm::Triple &Triple,
         const std::optional<llvm::Triple> &TargetVariant)
      : ToolChain(D, Triple), TargetVariant(TargetVariant) {}

  ~Darwin() = default;
  std::string sanitizerRuntimeLibName(StringRef Sanitizer,
                                      bool shared = true) const override;

  std::optional<llvm::Triple> getTargetVariant() const { return TargetVariant; }
};

class LLVM_LIBRARY_VISIBILITY Windows : public ToolChain {
protected:
  InvocationInfo constructInvocation(const DynamicLinkJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const StaticLinkJobAction &job,
                                     const JobContext &context) const override;

public:
  Windows(const Driver &D, const llvm::Triple &Triple) : ToolChain(D, Triple) {}
  ~Windows() = default;

  std::string sanitizerRuntimeLibName(StringRef Sanitizer,
                                      bool shared = true) const override;
};

class LLVM_LIBRARY_VISIBILITY WebAssembly : public ToolChain {
protected:
  InvocationInfo constructInvocation(const AutolinkExtractJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const DynamicLinkJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const StaticLinkJobAction &job,
                                     const JobContext &context) const override;
  void validateArguments(DiagnosticEngine &diags,
                         const llvm::opt::ArgList &args,
                         StringRef defaultTarget) const override;

public:
  WebAssembly(const Driver &D, const llvm::Triple &Triple) : ToolChain(D, Triple) {}
  ~WebAssembly() = default;
  std::string sanitizerRuntimeLibName(StringRef Sanitizer,
                                      bool shared = true) const override;
};


class LLVM_LIBRARY_VISIBILITY GenericUnix : public ToolChain {
protected:
  InvocationInfo constructInvocation(const InterpretJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const AutolinkExtractJobAction &job,
                                     const JobContext &context) const override;

  /// If provided, and if the user has not already explicitly specified a
  /// linker to use via the "-fuse-ld=" option, this linker will be passed to
  /// the compiler invocation via "-fuse-ld=". Return an empty string to not
  /// specify any specific linker (the "-fuse-ld=" option will not be
  /// specified).
  ///
  /// The default behavior is to use the gold linker on ARM architectures,
  /// and to not provide a specific linker otherwise.
  virtual std::string getDefaultLinker() const;

  bool addRuntimeRPath(const llvm::Triple &T,
                       const llvm::opt::ArgList &Args) const;

  InvocationInfo constructInvocation(const DynamicLinkJobAction &job,
                                     const JobContext &context) const override;
  InvocationInfo constructInvocation(const StaticLinkJobAction &job,
                                     const JobContext &context) const override;

public:
  GenericUnix(const Driver &D, const llvm::Triple &Triple)
      : ToolChain(D, Triple) {}
  ~GenericUnix() = default;
  std::string sanitizerRuntimeLibName(StringRef Sanitizer,
                                      bool shared = true) const override;
};

class LLVM_LIBRARY_VISIBILITY Android : public GenericUnix {
public:
  Android(const Driver &D, const llvm::Triple &Triple)
      : GenericUnix(D, Triple) {}
  ~Android() = default;
};

class LLVM_LIBRARY_VISIBILITY Cygwin : public GenericUnix {
protected:
  std::string getDefaultLinker() const override;

public:
  Cygwin(const Driver &D, const llvm::Triple &Triple)
      : GenericUnix(D, Triple) {}
  ~Cygwin() = default;
};

class LLVM_LIBRARY_VISIBILITY OpenBSD : public GenericUnix {
protected:
  std::string getDefaultLinker() const override;

public:
  OpenBSD(const Driver &D, const llvm::Triple &Triple)
      : GenericUnix(D, Triple) {}
  ~OpenBSD() = default;
};

class LLVM_LIBRARY_VISIBILITY FreeBSD : public GenericUnix {
protected:
  std::string getDefaultLinker() const override;

public:
  FreeBSD(const Driver &D, const llvm::Triple &Triple)
      : GenericUnix(D, Triple) {}
  ~FreeBSD() = default;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace swift

#endif
