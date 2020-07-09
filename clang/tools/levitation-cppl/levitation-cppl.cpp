//===--- C++ Levitation levitation-deps.cpp --------------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines C++ Levitation Driver tool main file.
//
//===----------------------------------------------------------------------===//

#include "clang/Levitation/CommandLineTool/CommandLineTool.h"
#include "clang/Levitation/Driver/Driver.h"
#include "clang/Levitation/FileExtensions.h"
#include "clang/Levitation/Common/SimpleLogger.h"
#include "clang/Levitation/Serialization.h"

#include <functional>
#include <tuple>

using namespace llvm;
using namespace clang;
using namespace clang::levitation;
using namespace clang::levitation::command_line_tool;

static const int RES_WRONG_ARGUMENTS = 1;
static const int RES_FAILED_TO_RUN = 2;

static const int RES_SUCCESS = 0;

std::string getCommandPath(const char *argv0);

std::string unquote(StringRef arg);

int levitation_driver_main(int argc, char **argv) {

  auto CommandPath = getCommandPath(argv[0]);

  clang::levitation::tools::LevitationDriver Driver(CommandPath.c_str());

  // Available driver parameters:
  // -root=<path> Specify source root (project) directory. Default value: '.'
  // -main=<path> Specify main source file. Default value: 'main.cpp'
  // -j<N> Maximum jobs number. Default value: 'j1'
  // -h=<path> Header file name to be generated.
  //    If not specified, then header is not generated.
  // -c Compile sources without linking.
  // -o Output file or directory. If -c is not specified,
  //    then it specifies output executable file, with 'a.out' by default.
  //    If -c is specified then it specifies output directory for object files,
  //    with a.dir by default.

  return CommandLineTool<KeyEqValueParser>(argc, argv)
      .description(
          "Is a C++ Levitation Compiler. Depending on mode it's "
          "ran in, it can go through preamble compilation, "
          "initial parsing, dependencies solving, "
          "code generation, and finally linker stages."
      )
      .registerParser<KeySpaceValueParser>()
      .registerParser<KeyValueInOneWordParser>()
      .optional(
          "-root", "<directory>",
          "Source root (project) directory.",
          [&](StringRef v) { Driver.setSourcesRoot(v); }
      )
      .optional(
          "-buildRoot", "<directory>",
          "Build root directory.",
          [&](StringRef v) { Driver.setBuildRoot(v); }
      )
      .optional(
          "-preamble", "<path>",
          "Path to preamble. If specified, then preamble compilation stage "
          "will be enabled.",
          [&](StringRef v) { Driver.setPreambleSource(v); }
      )
      .optional(
          "-h", "<path>",
          "Path to headers root directory. If specified, then headers "
          "generation stage will be added to the compilation pipeline.",
          [&](StringRef v) { Driver.setOutputHeadersDir(v); }
      )
      .optional(
          "-decl-out", "<path>",
          "Path to declarations root directory. If specified, then declarations "
          "generation stage will be added to the compilation pipeline.",
          [&](StringRef v) { Driver.setOutputDeclsDir(v); }
      )
      .optional(
          "-stdlib", "<std lib name>",
          "Name of standard library, usually 'libc++' or 'stdlibc++'.",
          [&](StringRef v) { Driver.setStdLib(v); }
      )
      .optional()
          .name("-j")
          .valueHint("<N>")
          .description("Maximum jobs number.")
          .action<int>([&](int v) { Driver.setJobsNumber(v); })
          .useParser<KeyValueInOneWordParser>()
      .done()
      .optional()
          .name("-o")
          .valueHint("<directory>")
          .description(
              "Output file or directory. If -c is not specified,"
              "then it specifies output executable file, with "
              "'a.out' by default. If -c is specified then it "
              "specifies output directory for object files,"
              "with a.dir by default."
          )
          .action([&](StringRef v) { Driver.setOutput(v); })
          .useParser<KeySpaceValueParser>()
      .done()
      .flag()
          .name("-c")
          .description("Compile sources without linking.")
          .action([&](llvm::StringRef) { Driver.disableLinkPhase(); })
      .done()
      .flag()
          .name("--verbose")
          .description("Enables verbose mode.")
          .action([&](llvm::StringRef) { Driver.setVerbose(); })
      .done()
      .flag()
          .name("--trace")
          .description("Enables trace mode.")
          .action([&](llvm::StringRef) { Driver.setTrace(); })
      .done()
      .flag()
          .name("-###")
          .description(
              "Toggle dry run mode. "
              "Prints commands to be executed without "
              "execution itself."
          )
          .action([&](llvm::StringRef) { Driver.setDryRun(); })
      .done()
      .flag()
          .name("-lit")
          .description(
              "Currently we hardcoded use of libstdc++. "
              "llvm-lit though conflicts with this lib under darwin "
              "when calling linker, for llvm-lit exports SDKROOT variable. "
              "This option indicates that we "
              "run driver from lit, and thus shouldn't pass -stdlib=libstdc++ "
              "during linker stage."
          )
          .action([&](llvm::StringRef) { Driver.disableUseLibStdCppForLinker(); })
      .done()
      .optional()
          .name("-FH")
          .valueHint("<args>")
          .description(
              "Extra args for preamble phase. Those flags are passed"
              "directly to frontend tool (clang -cc1)."
          )
          .useParser<KeySpaceValueParser>()
          .action([&](StringRef v) { Driver.setExtraPreambleArgs(v); })
      .done()
      .optional()
          .name("-FP")
          .valueHint("<args>")
          .description(
              "Extra args for parser phase. Those flags are passed"
              "directly to frontend tool (clang -cc1)."
          )
          .useParser<KeySpaceValueParser>()
          .action([&](StringRef v) { Driver.setExtraParserArgs(v); })
      .done()
      .optional()
          .name("-FC")
          .valueHint("<args>")
          .description(
              "Extra args for codegen phase. Those flags are passed"
              "directly to frontend tool (clang -cc1)."
          )
          .useParser<KeySpaceValueParser>()
          .action([&](StringRef v) { Driver.setExtraCodeGenArgs(v); })
      .done()
      .optional()
          .name("-FL")
          .valueHint("<args>")
          .description(
              "Extra args for link phase. Those flags are passed"
              "directly to frontend tool (clang -cc1)."
          )
          .useParser<KeySpaceValueParser>()
          .action([&](StringRef v) { Driver.setExtraLinkerArgs(v); })
      .done()
      .optional()
          .multi()
          .name("+I")
          .valueHint("<path>")
          .description(
              "Add path to levitation library"
          )
          .useParser<KeyValueInOneWordParser>()
          .action([&](StringRef v) { Driver.addLevitationLibPath(v); })
      .done()
      .optional()
          .multi()
          .name("-I")
          .valueHint("<path>")
          .description(
              "Add include directory."
          )
          .useParser<KeyValueInOneWordParser>()
          .action([&](StringRef v) { Driver.addInclude(unquote(v)); })
      .done()
      .helpParameter(
          "--help",
          "Shows this help text.",
          /*Print help if no params*/false
      )
      .onWrongArgsReturn(RES_WRONG_ARGUMENTS)
      .run([&] {
        if (!Driver.run())
          return RES_FAILED_TO_RUN;

        return RES_SUCCESS;
      });
}

int main(int argc, char **argv) {
  return levitation_driver_main(argc, argv);
}

std::string getCommandPath(const char *argv0) {
  void *MainAddr = (void*) (intptr_t) levitation_driver_main;
  return llvm::sys::fs::getMainExecutable(argv0, MainAddr);
}

std::string unquote(StringRef arg) {
  std::string v = arg.str();

  if (v.size() > 2 && v.front() == '"' && v.back() == '"')
    return v.substr(1, v.size()-1);
  return v;
}
