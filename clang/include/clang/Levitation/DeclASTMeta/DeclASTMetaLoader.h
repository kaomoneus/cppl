//===--- DeclASTMeta.h - C++ DeclASTMeta class ------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file contains root clas for Decl AST meta data
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DECLASTMETALOADER_H
#define LLVM_LEVITATION_DECLASTMETALOADER_H

#include "clang/Basic/FileManager.h"
#include "clang/Levitation/DeclASTMeta/DeclASTMeta.h"
#include "clang/Levitation/Common/CreatableSingleton.h"
#include "clang/Levitation/Common/Failable.h"
#include "clang/Levitation/Common/File.h"
#include "clang/Levitation/Common/Path.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Serialization.h"

#include "llvm/Support/MemoryBuffer.h"

namespace clang { namespace levitation {

  class DeclASTMetaLoader {

  public:

    static bool fromFile(
        DeclASTMeta &Meta, StringRef BuildRoot, StringRef FileName
    ) {
      auto &FM = CreatableSingleton<FileManager>::get();

      auto &Log = log::Logger::get();

      if (auto Buffer = FM.getBufferForFile(FileName)) {
        llvm::MemoryBuffer &MemBuf = *Buffer.get();

        if (!fromBuffer(Meta, MemBuf))
          Log.log_error("Failed to read dependencies for '", FileName);
      } else
       Log.log_error("Failed to open file '", FileName);

      return true;
    }

    static bool fromBuffer(DeclASTMeta &Meta, const MemoryBuffer &MemBuf) {
      auto Reader = CreateMetaBitstreamReader(MemBuf);

      auto &Log = log::Logger::get();

      if (!Reader->read(Meta)) {
        Log.log_error(Reader->getStatus().getErrorMessage());
        return false;
      }

      if (Reader->getStatus().hasWarnings()) {
        Log.log_warning(Reader->getStatus().getWarningMessage());
      }

      return true;
    }
  };
}}

#endif //LLVM_LEVITATION_DECLASTMETALOADER_H
