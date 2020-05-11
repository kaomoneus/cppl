//===--- C++ Levitation DependenciesSolver.h --------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation DependenciesSolver tool interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEVITATION_DEPENDENCIESSOLVER_H
#define LLVM_CLANG_LEVITATION_DEPENDENCIESSOLVER_H

#include "clang/Levitation/Common/Failable.h"
#include "clang/Levitation/Common/Path.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <memory>

namespace llvm {
  class raw_ostream;
}

namespace clang {
  class FileManager;
}

namespace clang { namespace levitation { namespace dependencies_solver {

class SolvedDependenciesInfo;
class DependenciesSolver : public Failable {
  llvm::StringRef SourcesRoot;
  llvm::StringRef BuildRoot;
  bool Verbose = false;
public:

  using LDepFilesTy = DenseMap<StringID, StringRef>;

  void setVerbose(bool Verbose) {
    DependenciesSolver::Verbose = Verbose;
  }

  void setSourcesRoot(llvm::StringRef SourcesRoot) {
    DependenciesSolver::SourcesRoot = SourcesRoot;
  }

  void setBuildRoot(llvm::StringRef BuildRoot) {
    DependenciesSolver::BuildRoot = BuildRoot;
  }

  // TODO Levitation: pass <PackageID, LDepPath> instead.
  std::shared_ptr<SolvedDependenciesInfo> solve(const LDepFilesTy &LDepsFiles);

  bool solve();

  friend class DependenciesSolverImpl;
};

}}}

#endif //LLVM_CLANG_LEVITATION_DEPENDENCIESSOLVER_H
