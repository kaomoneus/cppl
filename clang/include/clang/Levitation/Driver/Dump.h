//===--- Dump.h - C++ Dump class --------------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file contains phase dumping utility methods
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DRIVERDUMP_H
#define LLVM_LEVITATION_DRIVERDUMP_H

#include "llvm/ADT/StringRef.h"
#include "clang/Levitation/Common/CreatableSingleton.h"
#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Common/WithOperator.h"

namespace clang { namespace levitation { namespace tools {

  struct DriverPhaseDump {
    static void buildPreamble(
      llvm::StringRef PreambleSource,
      llvm::StringRef PreambleOut
    ) {
      with (auto info = log::Logger::get().acquire(log::Level::Info)) {
        auto &LogInfo = info.s;
        LogInfo
            << "PREAMBLE " << PreambleSource << " -> "
            << "preamble out: " << PreambleOut << "\n";
      }
    }

    static void parse(
        llvm::StringRef OutASTFile,
        llvm::StringRef OutLDepsFile,
        llvm::StringRef SourceFile
    ) {
      with (auto info = log::Logger::get().acquire(log::Level::Info)) {
        auto &LogInfo = info.s;
        LogInfo
            << "PARSE     " << SourceFile << " -> "
            << "(ast:" << OutASTFile << ", "
            << "ldeps: " << OutLDepsFile << ")"
            << "\n";
      }
    }

    static void parseImport(
        llvm::StringRef OutLDepsFile,
        llvm::StringRef SourceFile
    ) {
      with (auto info = log::Logger::get().acquire(log::Level::Info)) {
        auto &LogInfo = info.s;
        LogInfo
            << "PARSE IMP " << SourceFile << " -> "
            << "(ldeps: " << OutLDepsFile << ")"
            << "\n";
      }
    }

    static void buildDecl(
        llvm::StringRef OutDeclASTFile,
        llvm::StringRef InputObject,
        const Paths &Deps
    ) {
      assert(OutDeclASTFile.size() && InputObject.size());

      action(OutDeclASTFile, InputObject, Deps, "BUILD DECL", "decl-ast");
    }

    static void buildObject(
        llvm::StringRef OutObjFile,
        llvm::StringRef InputObject,
        const Paths &Deps
    ) {
      assert(OutObjFile.size() && InputObject.size());

      action(OutObjFile, InputObject, Deps, "BUILD OBJ ", "object");
    }

    static void action(
        llvm::raw_ostream &out,
        llvm::StringRef OutDeclASTFile,
        llvm::StringRef InputObject,
        const Paths &Deps,
        llvm::StringRef ActionName,
        llvm::StringRef OutputName
    ) {
      assert(OutDeclASTFile.size() && InputObject.size());

      out << ActionName << " " << InputObject;

      out << ", ";
      LDepsFiles(out, Deps);

      out << " -> " << OutputName << ": " << OutDeclASTFile << "\n";
    }

    static void action(
        llvm::StringRef OutDeclASTFile,
        llvm::StringRef InputObject,
        const Paths &Deps,
        llvm::StringRef ActionName,
        llvm::StringRef OutputName
    ) {
      with (auto info = log::Logger::get().acquire(log::Level::Info)) {
        auto &out = info.s;
        action(
            out,
            OutDeclASTFile,
            InputObject,
            Deps,
            ActionName,
            OutputName
        );
      }
    }

    static void LDepsFiles(
        raw_ostream &Out,
        const Paths &Deps
    ) {
      pathsArray(Out, Deps, "deps");
    }

    static void link(llvm::StringRef OutputFile, const Paths &ObjectFiles) {
      assert(OutputFile.size() && ObjectFiles.size());

      with (auto info = log::Logger::get().acquire(log::Level::Info)) {
        auto &LogInfo = info.s;

        LogInfo << "LINK ";
        objectFiles(LogInfo, ObjectFiles);
        LogInfo << " -> " << OutputFile << "\n";
      }
    }

    static void objectFiles(
        raw_ostream &Out,
        const Paths &ObjectFiles
    ) {
      pathsArray(Out, ObjectFiles, "objects");
    }

    static void pathsArray(
        raw_ostream &Out,
        const Paths &ObjectFiles,
        llvm::StringRef ArrayName
    ) {
      Out << ArrayName << ": ";

      if (ObjectFiles.size()) {
        Out << "(";
        for (size_t i = 0, e = ObjectFiles.size(); i != e; ++i) {
          Out << ObjectFiles[i];
          if (i + 1 != e)
            Out << ", ";
        }
        Out << ")";
      } else {
        Out << "<empty>";
      }
    }
  };
}}}

#endif //LLVM_LEVITATION_DRIVERDUMP_H
