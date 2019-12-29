//===--- Driver.h - C++ Driver class ----------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation Driver class.
//  It is a public driver interface. Most of implementation is present
//  in .cpp file as separate classes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DRIVER_H
#define LLVM_LEVITATION_DRIVER_H

#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/StringOrRef.h"
#include "clang/Levitation/Driver/DriverDefaults.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace clang { namespace levitation { namespace log {
  class Logger;
}}}

namespace clang { namespace levitation { namespace tools {

  class LevitationDriver {
  public:
    using Args = llvm::SmallVector<StringOrRef, 8>;
  private:

    bool Verbose = false;

    levitation::SinglePath BinDir;
    llvm::StringRef SourcesRoot = DriverDefaults::SOURCES_ROOT;
    llvm::StringRef BuildRoot = DriverDefaults::BUILD_ROOT;

    llvm::StringRef PreambleSource;
    levitation::SinglePath PreambleOutput;

    int JobsNumber = DriverDefaults::JOBS_NUMBER;

    bool OutputHeadersDirDefault = true;
    levitation::SinglePath OutputHeadersDir;
    llvm::StringRef Output;

    bool LinkPhaseEnabled = true;

    bool DryRun;

    llvm::StringRef StdLib = DriverDefaults::STDLIB;
    bool CanUseLibStdCppForLinker = true;

    Args ExtraPreambleArgs;
    Args ExtraParseArgs;
    Args ExtraParseImportArgs;
    Args ExtraCodeGenArgs;
    Args ExtraLinkerArgs;

  public:

    LevitationDriver(llvm::StringRef CommandPath);

    bool isVerbose() const {
      return Verbose;
    }

    void setVerbose(bool Verbose) {
      LevitationDriver::Verbose = Verbose;
    }

    llvm::StringRef getSourcesRoot() const {
      return SourcesRoot;
    }

    void setSourcesRoot(llvm::StringRef SourcesRoot) {
      LevitationDriver::SourcesRoot = SourcesRoot;
    }

    void setBuildRoot(llvm::StringRef BuildRoot) {
      LevitationDriver::BuildRoot = BuildRoot;
      if (OutputHeadersDirDefault)
        OutputHeadersDir = levitation::Path::getPath<SinglePath>(
            BuildRoot, DriverDefaults::HEADER_DIR_SUFFIX
        );
    }

    llvm::StringRef getPreambleSource() const {
      return PreambleSource;
    }

    bool isPreambleCompilationRequested() const {
      return PreambleSource.size();
    }

    void setPreambleSource(llvm::StringRef PreambleSource) {
      LevitationDriver::PreambleSource = PreambleSource;
    }

    void setStdLib(llvm::StringRef StdLib) {
      LevitationDriver::StdLib = StdLib;
    }

    int getJobsNumber() const {
      return JobsNumber;
    }

    void setJobsNumber(int JobsNumber) {
      LevitationDriver::JobsNumber = JobsNumber;
    }

    llvm::StringRef getOutput() const {
      return Output;
    }

    void setOutput(llvm::StringRef Output) {
      LevitationDriver::Output = Output;
    }

    void setOutputHeadersDir(llvm::StringRef h) {
      OutputHeadersDir = h;
      OutputHeadersDirDefault = false;
    }

    llvm::StringRef getOutputHeadersDir() const {
      return OutputHeadersDir;
    }

    bool shouldCreateHeaders() const {
      return !LinkPhaseEnabled;
    }

    bool isLinkPhaseEnabled() const {
      return LinkPhaseEnabled;
    }

    void disableLinkPhase() {
      LinkPhaseEnabled = false;
    }

    bool isDryRun() const {
      return DryRun;
    }

    void setDryRun() {
      DryRun = true;
    }

    void disableUseLibStdCppForLinker() {
      LevitationDriver::CanUseLibStdCppForLinker = false;
    }

    void setExtraPreambleArgs(StringRef Args);
    void setExtraParserArgs(StringRef Args);
    void setExtraCodeGenArgs(StringRef Args);
    void setExtraLinkerArgs(StringRef Args);

    bool run();

    friend class LevitationDriverImpl;

  protected:

    void initParameters();
    void dumpParameters();
    void dumpExtraFlags(StringRef Phase, const Args &args);
  };
}}}

#endif //LLVM_LEVITATION_DRIVER_H
