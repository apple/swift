//===--- SILRemarkStreamer.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILRemarkStreamer.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Assertions.h"
#include "llvm/IR/LLVMContext.h"

using namespace swift;

SILRemarkStreamer::SILRemarkStreamer(
    std::unique_ptr<llvm::remarks::RemarkStreamer> &&streamer,
    std::unique_ptr<llvm::raw_fd_ostream> &&stream, const ASTContext &Ctx)
    : owner(Owner::SILModule), streamer(std::move(streamer)), context(nullptr),
      remarkStream(std::move(stream)), ctx(Ctx) { }

llvm::remarks::RemarkStreamer &SILRemarkStreamer::getLLVMStreamer() {
  switch (owner) {
  case Owner::SILModule:
    return *streamer.get();
  case Owner::LLVM:
    return *context->getMainRemarkStreamer();
  }
  return *streamer.get();
}

const llvm::remarks::RemarkStreamer &
SILRemarkStreamer::getLLVMStreamer() const {
  switch (owner) {
  case Owner::SILModule:
    return *streamer.get();
  case Owner::LLVM:
    return *context->getMainRemarkStreamer();
  }
  return *streamer.get();
}

void SILRemarkStreamer::intoLLVMContext(llvm::LLVMContext &Ctx) & {
  assert(owner == Owner::SILModule);
  Ctx.setMainRemarkStreamer(std::move(streamer));
  context = &Ctx;
  owner = Owner::LLVM;
}

std::unique_ptr<SILRemarkStreamer>
SILRemarkStreamer::create(SILModule &silModule) {
  StringRef filename = silModule.getOptions().OptRecordFile;
  const auto format = silModule.getOptions().OptRecordFormat;
  if (filename.empty())
    return nullptr;

  auto &diagEngine = silModule.getASTContext().Diags;
  std::error_code errorCode;
  auto file = std::make_unique<llvm::raw_fd_ostream>(filename, errorCode,
                                                     llvm::sys::fs::OF_None);
  if (errorCode) {
    diagEngine.diagnose(SourceLoc(), diag::cannot_open_file, filename,
                        errorCode.message());
    return nullptr;
  }

  llvm::Expected<std::unique_ptr<llvm::remarks::RemarkSerializer>>
      remarkSerializerOrErr = llvm::remarks::createRemarkSerializer(
          format, llvm::remarks::SerializerMode::Separate, *file);
  if (llvm::Error err = remarkSerializerOrErr.takeError()) {
    diagEngine.diagnose(SourceLoc(), diag::error_creating_remark_serializer,
                        toString(std::move(err)));
    return nullptr;
  }

  auto mainRS = std::make_unique<llvm::remarks::RemarkStreamer>(
      std::move(*remarkSerializerOrErr), filename);

  const auto passes = silModule.getOptions().OptRecordPasses;
  if (!passes.empty()) {
    if (llvm::Error err = mainRS->setFilter(passes)) {
      diagEngine.diagnose(SourceLoc(), diag::error_creating_remark_serializer,
                          toString(std::move(err)));
      return nullptr;
    }
  }

  // N.B. We should be able to use std::make_unique here, but I prefer correctly
  // encapsulating the constructor over elegance.
  // Besides, this isn't going to throw an exception.
  return std::unique_ptr<SILRemarkStreamer>(new SILRemarkStreamer(
      std::move(mainRS), std::move(file), silModule.getASTContext()));
}
