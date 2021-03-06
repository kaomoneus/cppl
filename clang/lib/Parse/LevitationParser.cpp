//===--- C++ Levitation Parser.cpp ------------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines helper functions for clang::Parser class.
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/LevitationParser.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Levitation/Common/WithOperator.h"

using namespace llvm;

namespace clang {
namespace levitation {

void SkipFunctionBody(
    Preprocessor &PP,
    std::function<void()> &&SkipFunctionBody
) {
  // Prevent PP + Lexer link destruction while we're doing our stuff.

  bool oldIncPr = PP.isIncrementalProcessingEnabled();
  PP.enableIncrementalProcessing(true);
  auto _ = llvm::make_scope_exit([&] {PP.enableIncrementalProcessing(oldIncPr);});

  PP.setLevitationKeepComments(true);

  SkipFunctionBody();

  PP.setLevitationKeepComments(false);
}

}
}

