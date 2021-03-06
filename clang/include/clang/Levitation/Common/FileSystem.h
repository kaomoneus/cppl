//===--- C++ Levitation FileSystem.h ----------------------------------- -----*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation FileSystem class. It contains helpers
//  for file system manipulations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEVITATION_FILESYSTEM_H
#define LLVM_CLANG_LEVITATION_FILESYSTEM_H

#include "clang/Basic/FileManager.h"
#include "clang/Levitation/Common/CreatableSingleton.h"
#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/SimpleLogger.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/Path.h"
#include <memory>

namespace llvm {
  class raw_ostream;
  class raw_fd_ostream;
}

namespace clang {
namespace levitation {

    /*static*/
class FileSystem {
public:

  template <typename FilesVectorTy>
  static void collectFiles(
      FilesVectorTy &Files,
      llvm::StringRef Root,
      llvm::StringRef Extension,
      std::initializer_list<StringRef> IgnoreDirs = {},
      bool IgnoreHidden = false
  ) {
    auto &FM = CreatableSingleton<FileManager>::get();
    auto &FS = FM.getVirtualFileSystem();

    SmallVector<SinglePath, 2> IgnoreDirsAbsVec;
    DenseSet<StringRef> IgnoreDirsAbs;

    for (auto IgnoreDir : IgnoreDirs) {
      auto IgnoreDirAbs = Path::makeAbsolute<SinglePath>(IgnoreDir);
      IgnoreDirsAbsVec.push_back(std::move(IgnoreDirAbs));
      IgnoreDirsAbs.insert(IgnoreDirsAbsVec.back().str());
    }

    Paths SubDirs;
    SubDirs.push_back(Root);

    std::string parsedDepsFileExtension = ".";
    parsedDepsFileExtension += Extension;

    Paths NewSubDirs;
    while (SubDirs.size()) {
      NewSubDirs.clear();
      for (llvm::StringRef CurDir : SubDirs) {
        if (!IgnoreDirsAbs.empty()) {
          auto CurDirAbs = Path::makeAbsolute<SinglePath>(CurDir);
          if (IgnoreDirsAbs.count(CurDirAbs))
            continue;
        }

        collectFilesWithExtension(
            Files,
            NewSubDirs,
            FS,
            CurDir,
            parsedDepsFileExtension
        );
      }
      SubDirs.swap(NewSubDirs);
    }
  }

  static void copy(StringRef Src, StringRef Dest) {
    llvm::sys::fs::copy_file(Src, Dest);
  }

protected:

  template <typename FilesVectorTy>
  static void collectFilesWithExtension(
      FilesVectorTy &Dest,
      FilesVectorTy &NewSubDirs,
      llvm::vfs::FileSystem &FS,
      llvm::StringRef CurDir,
      llvm::StringRef FileExtension
  ) {
    auto &Log = log::Logger::get();
    std::error_code EC;

    for (
      llvm::vfs::directory_iterator Dir = FS.dir_begin(CurDir, EC), e;
      Dir != e && !EC;
      Dir.increment(EC)
    ) {
      llvm::StringRef Path = Dir->path();

      switch (Dir->type()) {
        case llvm::sys::fs::file_type::regular_file:
          if (llvm::sys::path::extension(Path) == FileExtension) {
            Log.log_trace("  Found '", Path, "'...");
            Dest.push_back(Path);
          }
        break;

        case llvm::sys::fs::file_type::directory_file:
          NewSubDirs.push_back(Path);
        break;

        default:
        break;
      }
    }
  }
};

}
}

#endif // LLVM_CLANG_LEVITATION_FILESYSTEM_H
