//===--- Driver.h - C++ DriverDefaults class --------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file contains default values for C++ Levitation Driver class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_DRIVERDEFAULTS_H
#define LLVM_LEVITATION_DRIVERDEFAULTS_H

#include "llvm/ADT/StringRef.h"

namespace clang { namespace levitation { namespace tools {

  struct DriverDefaults  {
      static constexpr char BIN_DIR [] = ".";
      static constexpr char HEADER_DIR_SUFFIX [] = "include";
      static constexpr char DECLS_DIR_SUFFIX [] = "declaration";
      static constexpr char SOURCES_ROOT [] = ".";
      static constexpr char BUILD_ROOT [] = ".build";
      static constexpr char LIBS_OUTPUT_SUBDIR [] = "levitation-libs";
      static constexpr char STDLIB[] = "";
      static constexpr int JOBS_NUMBER = 1;
      static constexpr char OUTPUT_EXECUTABLE [] = "a.out";
      static constexpr char OUTPUT_OBJECTS_DIR [] = "a.dir";
      static constexpr char PREAMBLE_OUT [] = "preamble.pch";
      static constexpr char PREAMBLE_OUT_META [] = "preamble.meta";
  };
}}}

#endif //LLVM_LEVITATION_DRIVERDEFAULTS_H
