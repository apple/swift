//===--- Action.h - Abstract compilation steps ------------------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_ACTION_H
#define SWIFT_DRIVER_ACTION_H

#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Driver/Util.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Chrono.h"

namespace llvm {
namespace opt {
  class Arg;
}
}

namespace swift {
namespace driver {
  class Action;

class Action {
public:
  using size_type = ArrayRef<const Action *>::size_type;
  using iterator = ArrayRef<const Action *>::iterator;
  using const_iterator = ArrayRef<const Action *>::const_iterator;

  enum class Kind : unsigned {
    Input = 0,
    CompileJob,
    InterpretJob,
    BackendJob,
    MergeModuleJob,
    ModuleWrapJob,
    AutolinkExtractJob,
    REPLJob,
    DynamicLinkJob,
    StaticLinkJob,
    GenerateDSYMJob,
    VerifyDebugInfoJob,
    GeneratePCHJob,
    VerifyModuleInterfaceJob,

    JobFirst = CompileJob,
    JobLast = VerifyModuleInterfaceJob
  };

  static const char *getClassName(Kind AC);

private:
  unsigned RawKind : 4;
  unsigned Type : 28;

  friend class Compilation;
  /// Actions must be created through Compilation::createAction.
  void *operator new(size_t size) { return ::operator new(size); };

protected:
  Action(Kind K, file_types::ID Type)
      : RawKind(unsigned(K)), Type(Type) {
    assert(K == getKind() && "not enough bits");
    assert(Type == getType() && "not enough bits");
  }

public:
  virtual ~Action() = default;

  const char *getClassName() const { return Action::getClassName(getKind()); }

  Kind getKind() const { return static_cast<Kind>(RawKind); }
  file_types::ID getType() const { return static_cast<file_types::ID>(Type); }
};

class InputAction : public Action {
  virtual void anchor();
  const llvm::opt::Arg &Input;

public:
  InputAction(const llvm::opt::Arg &Input, file_types::ID Type)
      : Action(Action::Kind::Input, Type), Input(Input) {}
  const llvm::opt::Arg &getInputArg() const { return Input; }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::Input;
  }
};

class JobAction : public Action {
  TinyPtrVector<const Action *> Inputs;
  virtual void anchor();
protected:
  JobAction(Kind Kind, ArrayRef<const Action *> Inputs,
            file_types::ID Type)
      : Action(Kind, Type), Inputs(Inputs) {}

public:
  ArrayRef<const Action *> getInputs() const { return Inputs; }
  void addInput(const Action *Input) { Inputs.push_back(Input); }

  size_type size() const { return Inputs.size(); }

  iterator begin() { return Inputs.begin(); }
  iterator end() { return Inputs.end(); }
  const_iterator begin() const { return Inputs.begin(); }
  const_iterator end() const { return Inputs.end(); }

  // Returns the index of the Input action's output file which is used as
  // (single) input to this action. Most actions produce only a single output
  // file, so we return 0 by default.
  virtual size_t getInputIndex() const { return 0; }

  static bool classof(const Action *A) {
    return (A->getKind() >= Kind::JobFirst &&
            A->getKind() <= Kind::JobLast);
  }
};

class IncrementalJobAction : public JobAction {
public:
  struct InputInfo {
    /// The status of an input known to the driver. These are used to affect
    /// the scheduling decisions made during an incremental build.
    ///
    /// \Note The order of cases matters. They are ordered from least to
    /// greatest impact on the incremental build schedule.
    enum class Status {
      /// The input to this job is up to date.
      UpToDate,
      /// The input to this job has changed in a way that requires this job to
      /// be rerun, but not in such a way that it requires a cascading rebuild.
      NeedsNonCascadingBuild,
      /// The input to this job has changed in a way that requires this job to
      /// be rerun, and in such a way that all jobs dependent upon this one
      /// must be scheduled as well.
      NeedsCascadingBuild,
      /// The input to this job was not known to the driver when it was last
      /// run.
      NewlyAdded
    };

  public:
    Status status = Status::UpToDate;
    llvm::sys::TimePoint<> previousModTime;

    InputInfo() = default;
    InputInfo(Status stat, llvm::sys::TimePoint<> time)
        : status(stat), previousModTime(time) {}

    static InputInfo makeNewlyAdded() {
      return {Status::NewlyAdded, llvm::sys::TimePoint<>::max()};
    }

    static InputInfo makeNeedsCascadingRebuild() {
      return {Status::NeedsCascadingBuild, llvm::sys::TimePoint<>::min()};
    }
  };

private:
  virtual void anchor() override;
  InputInfo inputInfo;

public:
  IncrementalJobAction(Kind Kind, ArrayRef<const Action *> Inputs,
                       file_types::ID Type, InputInfo info)
      : JobAction(Kind, Inputs, Type), inputInfo(info) {}

public:
  InputInfo getInputInfo() const {
    return inputInfo;
  }

public:
  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::CompileJob ||
           A->getKind() == Action::Kind::MergeModuleJob;
  }
};

class CompileJobAction : public IncrementalJobAction {
private:
  virtual void anchor() override;

public:
  CompileJobAction(file_types::ID OutputType)
      : IncrementalJobAction(Action::Kind::CompileJob, llvm::None, OutputType,
                             {}) {}
  CompileJobAction(Action *Input, file_types::ID OutputType, InputInfo info)
      : IncrementalJobAction(Action::Kind::CompileJob, Input, OutputType,
                             info) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::CompileJob;
  }

  /// Return a _single_ TY_Swift InputAction, if one exists;
  /// if 0 or >1 such inputs exist, return nullptr.
  const InputAction *findSingleSwiftInput() const {
    auto Inputs = getInputs();
    const InputAction *IA = nullptr;
    for (auto const *I : Inputs) {
      if (auto const *S = dyn_cast<InputAction>(I)) {
        if (S->getType() == file_types::TY_Swift) {
          if (IA == nullptr) {
            IA = S;
          } else {
            // Already found one, two is too many.
            return nullptr;
          }
        }
      }
    }
    return IA;
  }
};

class InterpretJobAction : public JobAction {
private:
  virtual void anchor() override;

public:
  explicit InterpretJobAction()
      : JobAction(Action::Kind::InterpretJob, llvm::None,
                  file_types::TY_Nothing) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::InterpretJob;
  }
};

class BackendJobAction : public JobAction {
private:
  virtual void anchor() override;
  
  // In case of multi-threaded compilation, the compile-action produces multiple
  // output bitcode-files. For each bitcode-file a BackendJobAction is created.
  // This index specifies which of the files to select for the input.
  size_t InputIndex;
public:
  BackendJobAction(const Action *Input, file_types::ID OutputType,
                   size_t InputIndex)
      : JobAction(Action::Kind::BackendJob, Input, OutputType),
        InputIndex(InputIndex) {}
  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::BackendJob;
  }
  
  virtual size_t getInputIndex() const override { return InputIndex; }
};

class REPLJobAction : public JobAction {
public:
  enum class Mode {
    Integrated,
    PreferLLDB,
    RequireLLDB
  };
private:
  virtual void anchor() override;
  Mode RequestedMode;
public:
  REPLJobAction(Mode mode)
      : JobAction(Action::Kind::REPLJob, llvm::None,
                  file_types::TY_Nothing),
        RequestedMode(mode) {}

  Mode getRequestedMode() const { return RequestedMode; }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::REPLJob;
  }
};

class MergeModuleJobAction : public IncrementalJobAction {
  virtual void anchor() override;
public:
  MergeModuleJobAction(ArrayRef<const Action *> Inputs, InputInfo input)
      : IncrementalJobAction(Action::Kind::MergeModuleJob, Inputs,
                             file_types::TY_SwiftModuleFile, input) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::MergeModuleJob;
  }
};

class ModuleWrapJobAction : public JobAction {
  virtual void anchor() override;
public:
  ModuleWrapJobAction(ArrayRef<const Action *> Inputs)
      : JobAction(Action::Kind::ModuleWrapJob, Inputs,
                  file_types::TY_Object) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::ModuleWrapJob;
  }
};

class AutolinkExtractJobAction : public JobAction {
  virtual void anchor() override;
public:
  AutolinkExtractJobAction(ArrayRef<const Action *> Inputs)
      : JobAction(Action::Kind::AutolinkExtractJob, Inputs,
                  file_types::TY_AutolinkFile) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::AutolinkExtractJob;
  }
};

class GenerateDSYMJobAction : public JobAction {
  virtual void anchor() override;
public:
  explicit GenerateDSYMJobAction(const Action *Input)
      : JobAction(Action::Kind::GenerateDSYMJob, Input,
                  file_types::TY_dSYM) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::GenerateDSYMJob;
  }
};

class VerifyDebugInfoJobAction : public JobAction {
  virtual void anchor() override;
public:
  explicit VerifyDebugInfoJobAction(const Action *Input)
      : JobAction(Action::Kind::VerifyDebugInfoJob, Input,
                  file_types::TY_Nothing) {}

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::VerifyDebugInfoJob;
  }
};
  
class GeneratePCHJobAction : public JobAction {
  std::string PersistentPCHDir;

  virtual void anchor() override;
public:
  GeneratePCHJobAction(const Action *Input, StringRef persistentPCHDir)
      : JobAction(Action::Kind::GeneratePCHJob, Input,
                  persistentPCHDir.empty() ? file_types::TY_PCH
                                           : file_types::TY_Nothing),
        PersistentPCHDir(persistentPCHDir) {}

  bool isPersistentPCH() const { return !PersistentPCHDir.empty(); }
  StringRef getPersistentPCHDir() const { return PersistentPCHDir; }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::GeneratePCHJob;
  }
};

class DynamicLinkJobAction : public JobAction {
  virtual void anchor() override;
  LinkKind Kind;
  bool ShouldPerformLTO;

public:
  DynamicLinkJobAction(ArrayRef<const Action *> Inputs, LinkKind K,
                       bool ShouldPerformLTO)
      : JobAction(Action::Kind::DynamicLinkJob, Inputs, file_types::TY_Image),
        Kind(K), ShouldPerformLTO(ShouldPerformLTO) {
    assert(Kind != LinkKind::None && Kind != LinkKind::StaticLibrary);
  }

  LinkKind getKind() const { return Kind; }

  bool shouldPerformLTO() const { return ShouldPerformLTO; }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::DynamicLinkJob;
  }
};

class StaticLinkJobAction : public JobAction {
  virtual void anchor() override;

public:
  StaticLinkJobAction(ArrayRef<const Action *> Inputs, LinkKind K)
      : JobAction(Action::Kind::StaticLinkJob, Inputs, file_types::TY_Image) {
    assert(K == LinkKind::StaticLibrary);
  }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::StaticLinkJob;
  }
};

class VerifyModuleInterfaceJobAction : public JobAction {
  virtual void anchor() override;
  file_types::ID inputType;

public:
  VerifyModuleInterfaceJobAction(const Action * ModuleEmitter,
                                 file_types::ID inputType)
    : JobAction(Action::Kind::VerifyModuleInterfaceJob, { ModuleEmitter },
                file_types::TY_Nothing), inputType(inputType) {
    assert(inputType == file_types::TY_SwiftModuleInterfaceFile ||
           inputType == file_types::TY_PrivateSwiftModuleInterfaceFile);
  }

  file_types::ID getInputType() const { return inputType; }

  static bool classof(const Action *A) {
    return A->getKind() == Action::Kind::VerifyModuleInterfaceJob;
  }
};

} // end namespace driver
} // end namespace swift

#endif
