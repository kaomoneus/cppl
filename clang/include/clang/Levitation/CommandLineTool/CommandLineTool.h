//===--- ArgumentsParser.h - C++ CommandLineTool class ---------------*- C++ -*-===//
//
// Part of the C++ Levitation Project,
// under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines CommandLineTool class which implements simple command
//  line parser.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LEVITATION_COMMANDLINETOOL_H
#define LLVM_LEVITATION_COMMANDLINETOOL_H

#include "clang/Levitation/CommandLineTool/AlignedPrinter.h"
#include "clang/Levitation/CommandLineTool/ArgsParser.h"
#include "clang/Levitation/CommandLineTool/Parameter.h"
#include "clang/Levitation/CommandLineTool/ParameterValueHandling.h"
#include "clang/Levitation/CommandLineTool/ParameterBuilder.h"
#include "clang/Levitation/Common/Failable.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <memory>
#include <utility>

namespace clang { namespace levitation { namespace command_line_tool {

template <typename DefaultParserTy>
class CommandLineTool : public Failable {
  int Argc;
  char **Argv;

  llvm::StringRef Name;
  llvm::StringRef Description;

  size_t TitleIndent = 2;
  size_t ParameterNameIndent = 2;
  size_t ParameterDescriptionIndent = 4;
  size_t RightBorder = 70;

  llvm::SmallVector<llvm::StringRef, 4> ParsersInOriginalOrder;
  llvm::DenseMap<llvm::StringRef, std::unique_ptr<ArgumentsParser>> Parsers;

  ArgumentsParser *DefaultParser;

  llvm::SmallVector<llvm::StringRef, 16> ParametersInOriginalOrder;
  llvm::DenseMap<llvm::StringRef, std::unique_ptr<Parameter>> Parameters;

  llvm::DenseSet<llvm::StringRef> Visited;
  llvm::DenseSet<llvm::StringRef> Optional;

  int WrongArgumentsResult = -1;
  bool PrintHelpIfNoParams = false;
  bool DisableMainAction = false;

public:

  template<typename T>
  using HandleValueFn = ParameterValueHandling::HandleFn<T>;

  CommandLineTool(int argc, char **argv)
  : Argc(argc), Argv(argv)
  {
    if (Argc) {
      auto Path = Argv[0];
      Name = llvm::sys::path::filename(Path);
    }
    defaultParser();
  }

  CommandLineTool(const CommandLineTool &) = delete;

  template<class ParserTy>
  CommandLineTool &registerParser() {
    createParser<ParserTy>();
    ParsersInOriginalOrder.push_back(ParserTy::getName());
    return *this;
  }

  CommandLineTool &description(llvm::StringRef description) {
    Description = description;
    return *this;
  }

  CommandLineTool &onWrongArgsReturn(int res) {
    WrongArgumentsResult = res;
    return *this;
  }

  using ParameterBuilderTy = ParameterBuilder<CommandLineTool>;

  ParameterBuilderTy parameter() {

    ParameterBuilderTy Builder(
        [&] (std::unique_ptr<Parameter> &&P) -> CommandLineTool& {

          if (P->Optional || P->IsFlag)
            Optional.insert(P->Name);

          ParametersInOriginalOrder.push_back(P->Name);
          auto InsertRes = Parameters.insert({
              P->Name, std::move(P)
          });

          if(!InsertRes.second)
            llvm_unreachable("Parameter should be unique and identified by its name");

          return *this;
        }
    );

    return Builder;
  }

  CommandLineTool &parameter(
      llvm::StringRef Name,
      std::string Description,
      HandleValueFn<llvm::StringRef> &&HandleFunction,
      llvm::StringRef DefaultValue = ""
  ) {
    return parameter()
        .name(Name)
        .description(Description)
        .action(std::move(HandleFunction))
    .done();
  }

  CommandLineTool &optional(
      llvm::StringRef Name,
      llvm::StringRef ValueHint,
      llvm::StringRef Description,
      HandleValueFn<llvm::StringRef> &&HandleFunction
  ) {
    return parameter()
        .optional()
        .name(Name)
        .valueHint(ValueHint)
        .description(Description)
        .action(std::move(HandleFunction))
    .done();
  }

  ParameterBuilderTy optional() {
    auto Builder = parameter();
    Builder.optional();
    return Builder;
  }

  ParameterBuilderTy flag() {
    auto Builder = parameter();
    Builder.flag();
    return Builder;
  }

  CommandLineTool &helpParameter(
      llvm::StringRef Name,
      llvm::StringRef Description,
      bool printOnNoParams = true
  ) {
    return flag()
        .name(Name)
        .description(Description)
        .action([&] (llvm::StringRef v) {
          printHelp(llvm::outs());
          DisableMainAction = true;
        })
    .done();
  }

  CommandLineTool& done() {
    return *this;
  }

  int run(std::function<int()> &&Action) {
    if (!parse())
      return WrongArgumentsResult;

    if (!DisableMainAction)
      return Action();
    return 0;
  }

protected:

  CommandLineTool &defaultParser() {
    DefaultParser = createParser<DefaultParserTy>();
    return *this;
  }

  bool parse() {
      // If we have no parameters passed, then do default action.
      if (Argc == 1 && PrintHelpIfNoParams) {
        printHelp(llvm::outs());
        return false;
      }

      // Sort parameters by parsers.

      assert(DefaultParser && "Default Parser should be registered first");

      for (auto &ParamKV : Parameters) {
        auto &Param = *ParamKV.second;
        for (auto ParserName : Param.EnabledForParsers) {
          getParser(ParserName).registerParameter(&Param);
        }
        DefaultParser->registerParameter(&Param);
      }

      // Go through command line arguments and parse them.
      // First. We try to use parsers in order they were added.
      // If non of them succeed, then we use default parser.

      ArgumentsParser::Context ParserContext = {
          Argc,
          Argv,
          /*FailableInstance*/ *this,
          /*VisitedArguments*/ llvm::DenseSet<int>(),
          /*VisitedParameters*/ llvm::DenseSet<llvm::StringRef>()
      };

      for (auto &P : ParsersInOriginalOrder) {
        auto &Parser = getParser(P);
        Parser.parse(ParserContext);
      }

      DefaultParser->parse(ParserContext);

      // Check whether we haven't met some of required parameters.
      bool HasMissedParameters = false;
      for (auto &P : Parameters) {
        auto ParameterName = P.first;

        if (Optional.count(ParameterName))
          continue;

        if (!ParserContext.VisitedParameters.count(ParameterName)) {
          reportMissedParameter(ParameterName);
          HasMissedParameters = true;
        }
      }

      for (int i = 1; i != Argc; ++i) {
        if (!ParserContext.VisitedArguments.count(i)) {
          llvm::errs()
          << "Warning: unknown argument '" << Argv[i] << "'.\n";
        }
      }

      if (!isValid() || HasMissedParameters) {
        llvm::errs()
        << "Error: " << getErrorMessage() << "\n";
        printHelp(llvm::errs());
        return false;
      }

      return true;
  }

  template<class ParserTy>
  ArgumentsParser *createParser() {
    llvm::StringRef ParserName = ParserTy::getName();
    auto P = std::make_unique<ParserTy>();
    auto InsertRes = Parsers.insert({ParserName, std::move(P)});
    return InsertRes.first->second.get();
  }

  ArgumentsParser &getParser(llvm::StringRef Name) {
    auto Found = Parsers.find(Name);
    if (Found == Parsers.end())
      llvm_unreachable("Attempt to access parser that wasn't registered.");

    return *Found->second;
  }

  void printDescription(llvm::raw_ostream &out, llvm::StringRef Description) const {
    AlignedPrinter(out)
      .indent(ParameterDescriptionIndent)
      .rightBorder(RightBorder)
      .print(Description);
  }

  void printParameterHelp(llvm::raw_ostream &out, const Parameter &P) const {
    out.indent(ParameterNameIndent) << P.Name << "\n";
    printDescription(out, P.Description);
    out << "\n";
  }

  void printHelp(llvm::raw_ostream &out) const {
    out << "\n";
    out.indent((unsigned)TitleIndent) << Name << " - " << Description << "\n\n";
    for (auto P : ParametersInOriginalOrder) {
      auto Found = Parameters.find(P);
      assert(
          Found != Parameters.end() &&
          "ParametersInOriginalOrder should contain same values as "
          "Parameters keys set."
      );

      printParameterHelp(out, *Found->second);
    }
  }

  void reportMissedParameter(llvm::StringRef P) {
    llvm::errs() << "Missed parameter: '" << P << "'\n";
  }
};

}}} // end of clang::levitation::command_line_tool namespace

#endif //LLVM_LEVITATION_COMMANDLINETOOL_H
