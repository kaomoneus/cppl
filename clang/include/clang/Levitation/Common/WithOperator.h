//===--- C++ Levitation WithOperator.h --------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation WithOperator class.
//
//===----------------------------------------------------------------------===//

// This header adds support for python-like "with" operator.

#ifndef LLVM_CLANG_LEVITATION_WITHOPERATOR_H
#define LLVM_CLANG_LEVITATION_WITHOPERATOR_H

#include "llvm/ADT/ScopeExit.h"

namespace clang {
namespace levitation {

  /// "with" operator.
  ///
  /// * In order to add support of "with" operator for new class,
  /// that class should be inherited from WithOperand class.
  ///
  /// * levitation::ScopeExit is an llvm::detail::scope_exit adaptation
  /// for use with "with" operand.
  #define with if

  /// WithOperand
  /// In order to add support of "with" operator for new class,
  /// that class should be inherited from WithOperand class.
  class WithOperand {
  public:
    constexpr operator bool() const { return true; }
  };

  /// ScopeExit is an llvm::detail::scope_exit adaptation
  /// for use with "with" operand.
  template <typename Callable>
  class ScopeExit : public WithOperand {
    llvm::detail::scope_exit<typename std::decay<Callable>::type> Scope;
  public:
    ScopeExit(Callable &&ExitF) : Scope(llvm::make_scope_exit(std::move(ExitF))) {}
  };

  template <typename Callable>
  ScopeExit<Callable> make_scope_exit(Callable &&ExitF) {
    return ScopeExit<Callable>(std::move(ExitF));
  }

  // Just a shorter synonym
  template <typename Callable>
  ScopeExit<Callable> on_exit(Callable &&ExitF) {
    return ScopeExit<Callable>(std::move(ExitF));
  }

}
}

#endif //LLVM_CLANG_LEVITATION_WITHOPERATOR_H
