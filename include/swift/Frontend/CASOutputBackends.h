//===------CASOutputBackends.h-- ---------------------------------*-C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FRONTEND_CASOUTPUTBACKENDS_H
#define SWIFT_FRONTEND_CASOUTPUTBACKENDS_H

#include "swift/Frontend/FrontendInputsAndOutputs.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/VirtualOutputFile.h"

namespace swift::cas {

class SwiftCASOutputBackend : public llvm::vfs::OutputBackend {
  void anchor() override;

protected:
  llvm::IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override;

  llvm::Expected<std::unique_ptr<llvm::vfs::OutputFileImpl>>
  createFileImpl(llvm::StringRef ResolvedPath,
                 std::optional<llvm::vfs::OutputConfig> Config) override;

  virtual llvm::Error storeImpl(llvm::StringRef Path, llvm::StringRef Bytes,
                                unsigned InputIndex, file_types::ID OutputKind);

private:
  file_types::ID getOutputFileType(llvm::StringRef Path) const;

public:
  SwiftCASOutputBackend(llvm::cas::ObjectStore &CAS,
                        llvm::cas::ActionCache &Cache,
                        llvm::cas::ObjectRef BaseKey,
                        const FrontendInputsAndOutputs &InputsAndOutputs,
                        FrontendOptions::ActionType Action);
  ~SwiftCASOutputBackend();

  llvm::Error storeCachedDiagnostics(unsigned InputIndex,
                                     llvm::StringRef Bytes);

  /// Store the MCCAS CASID \p ID as the object file output for the input
  /// denoted by the \p InputIndex.
  llvm::Error storeMCCASObjectID(unsigned InputIndex, llvm::cas::CASID ID);

  /// Get the input index of the \p OutputFilename.
  std::optional<unsigned>
  getInputIndexForOutputFilename(StringRef OutputFilename);

private:
  class Implementation;
  Implementation &Impl;
};

} // end namespace swift::cas

#endif
