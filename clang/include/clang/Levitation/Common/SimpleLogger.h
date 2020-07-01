//===--- C++ Levitation SimpleLogger.h ------------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation very simple Logger class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEVITATION_SIMPLELOGGER_H
#define LLVM_CLANG_LEVITATION_SIMPLELOGGER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <mutex>
#include <functional>
namespace clang { namespace levitation { namespace log {

enum class Level {
  Error,
  Warning,
  Info,
  Verbose,
  Trace
};

using manipulator_t = std::function<void(llvm::raw_ostream &out)>;

/// Logger is a simple logger implementation.
/// Example of use:
///
///   // main.cpp:
///   int main(/*...*/) {
///     // ...
///     log::Logger::createLogger(log::Level::Warning);
///     // ...
///   }
///
///  // MySource.cpp
///  void f() {
///    auto &Log = log::Logger::get();
///    Log.info() << "Hello world!\n";
///  }

class Logger {
  Level LogLevel;
  llvm::raw_ostream &Out;
  std::mutex Locker;

  Logger(Level LogLevel, llvm::raw_ostream &Out)
  : LogLevel(LogLevel), Out(Out)
  {}

protected:
  static std::unique_ptr<Logger> &accessLoggerPtr() {
    static std::unique_ptr<Logger> LoggerPtr;
    return LoggerPtr;
  }

public:

  static Logger &createLogger(Level LogLevel = Level::Error) {

    llvm::raw_ostream &Out = LogLevel > Level::Warning ?
        llvm::outs() : llvm::errs();

    accessLoggerPtr() = std::unique_ptr<Logger>(new Logger(LogLevel, Out));

    return get();
  }

  void setLogLevel(Level L) {
    LogLevel = L;
  }

  static Logger &get() {
    auto &LoggerPtr = accessLoggerPtr();
    assert(LoggerPtr && "Logger should be created");
    return *LoggerPtr;
  }

  llvm::raw_ostream &error() {
    return getStream(Level::Error);
  }

  llvm::raw_ostream &warning() {
    return getStream(Level::Warning);
  }

  llvm::raw_ostream &info() {
    return getStream(Level::Info);
  }

  llvm::raw_ostream &verbose() {
    return getStream(Level::Verbose);
  }

  llvm::raw_ostream &trace() {
    return getStream(Level::Trace);
  }

  std::unique_lock<std::mutex> lock() {
    return std::unique_lock<std::mutex>(Locker);
  }

  template <typename ...ArgsT>
  void log(Level level, ArgsT&&...args) {
    logImpl(level, std::forward<ArgsT>(args)...);
  }

  template <typename ...ArgsT>
  void log_trace(ArgsT&&...args) {
    logImpl(Level::Trace, std::forward<ArgsT>(args)...);
  }

  template <typename ...ArgsT>
  void log_verbose(ArgsT&&...args) {
    logImpl(Level::Verbose, std::forward<ArgsT>(args)...);
  }

  template <typename ...ArgsT>
  void log_info(ArgsT&&...args) {
    logImpl(Level::Info, std::forward<ArgsT>(args)...);
  }

  template <typename ...ArgsT>
  void log_warning(ArgsT&&...args) {
    logImpl(Level::Warning, std::forward<ArgsT>(args)...);
  }

  template <typename ...ArgsT>
  void log_error(ArgsT&&...args) {
    logImpl(Level::Error, std::forward<ArgsT>(args)...);
  }

protected:

  template <typename ...ArgsT>
  void logImpl(Level level, ArgsT &&...args) {
    auto _ = lock();
    logSuffix(level, std::forward<ArgsT>(args)...);
    (getStream(level) << "\n").flush();
  }

  void logSuffix(Level level) {
  }

  void logSuffix(Level level, manipulator_t Arg) {
    Arg(getStream(level));
  }

  template <typename FirstArgT>
  void logSuffix(Level level, FirstArgT Arg) {
    getStream(level) << Arg;
  }

  template <typename FirstArgT, typename ...ArgsT>
  void logSuffix(Level level, FirstArgT first, ArgsT&&...args) {
    logSuffix(level, first);
    logSuffix(level, std::forward<ArgsT>(args)...);
  }

  llvm::raw_ostream &getStream(Level ForLevel) {
    if (ForLevel <= LogLevel)
      return Out;
    return llvm::nulls();
  }
};

}}}

#endif //LLVM_CLANG_LEVITATION_SIMPLELOGGER_H
