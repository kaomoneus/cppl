//===------- SemaLevitation.cpp - Semantic Analysis for C++ Levitation ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ Levitation.
//===----------------------------------------------------------------------===//

#include "clang/Sema/Sema.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ASTMutationListener.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/Template.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"

#include <iterator>
#include <utility>

using namespace clang;
using namespace sema;

//===--------------------------------------------------------------------===//
// Helpers
//

#if 0
#define DUMP_SOURCE_FRAGMENTS
#endif

std::pair<unsigned, unsigned> levitationGetDeclaratorID(const Declarator &D) {
  const auto &SR = D.getSourceRange();
  return {
    SR.getBegin().getRawEncoding(),
    SR.getEnd().getRawEncoding()
  };
}

bool Sema::levitationMayBeSkipVarDefinition(
    const Declarator &D,
    const DeclContext *DC,
    bool IsVariableTemplate,
    bool IsRedeclaration,
    clang::StorageClass SC) {

  if (!isLevitationMode(
      LangOptions::LBSK_BuildPreamble,
      LangOptions::LBSK_BuildDeclAST
  ))
    return false;

  if (!CurContext->isFileContext())
    return false;

  bool IsStaticMember = DC->isRecord();
  bool IsFileVar = DC->isFileContext();
  bool IsStatic =
    SC == SC_Static ||
    (
      SC != StorageClass::SC_Extern &&
      D.getDeclSpec().getConstSpecLoc().isValid()
    );

  auto SkipAction = LevitationVarSkipAction::None;

  if (!IsVariableTemplate) {
    if (IsStaticMember && !DC->isDependentContext()) {
      SkipAction = LevitationVarSkipAction::Skip;
    } else if (IsFileVar) {
      if (IsRedeclaration) {
        // For continue parsing for static redeclarations,
        // that should force diagnostics, for it is a wrong static use-case.
        if (!IsStatic)
          SkipAction = LevitationVarSkipAction::Skip;
      } else if (!IsStatic)
        SkipAction = LevitationVarSkipAction::SkipInit;
    }
  }

  if (SkipAction != LevitationVarSkipAction::None) {
    LevitationVarSkipActions.try_emplace(
        levitationGetDeclaratorID(D), SkipAction
    );
    if (SkipAction == LevitationVarSkipAction::Skip)
      return true;
  }

  return false;
}

bool Sema::levitationMayBeSkipFunctionDefinition(const Decl *D) {
  auto *FunctionDecl = D->getAsFunction();
  return
    FunctionDecl &&
    !FunctionDecl->isInlined() &&
    !FunctionDecl->isTemplated();
}

Sema::LevitationVarSkipAction Sema::levitationGetSkipActionFor(
    const Declarator &D
) {
  auto Found = LevitationVarSkipActions.find(levitationGetDeclaratorID(D));
  if (Found != LevitationVarSkipActions.end())
    return Found->second;
  return LevitationVarSkipAction::None;
}

void Sema::levitationAddSkippedSourceFragment(
    const clang::SourceLocation &Start,
    const clang::SourceLocation &End,
    bool ReplaceWithSemicolon
) {

  levitation::SourceFragmentAction Action = ReplaceWithSemicolon ?
      levitation::SourceFragmentAction::ReplaceWithSemicolon :
      levitation::SourceFragmentAction::Skip;

  auto StartSLoc = getSourceManager().getDecomposedLoc(Start);
  auto EndSLoc = getSourceManager().getDecomposedLoc(End);

  if(
    !getSourceManager().isInMainFile(Start) ||
    !getSourceManager().isInMainFile(End)
  )
    return;

  if (LevitationSkippedFragments.size()) {
    auto &Last = LevitationSkippedFragments.back();
    if (Last.End >= StartSLoc.second) {
      Last.End = EndSLoc.second;
      Last.Action = Action;

      #ifdef DUMP_SOURCE_FRAGMENTS
        llvm::errs() << "Extended skipped fragment "
                     << (ReplaceWithSemicolon ? "BURN:\n" : ":\n");

        llvm::errs() << "Bytes: 0x";
        llvm::errs().write_hex(Last.Start) << " : 0x";
        llvm::errs().write_hex(Last.End) << "\n";

        llvm::errs() << " extension range:\n";

        Start.dump(getSourceManager());
        End.dump(getSourceManager());

        llvm::errs() << "\n";
      #endif

      return;
    }
  }

  LevitationSkippedFragments.push_back({
    StartSLoc.second,
    EndSLoc.second,
    Action
  });

#ifdef DUMP_SOURCE_FRAGMENTS
  llvm::errs() << "Added skipped fragment "
               << (ReplaceWithSemicolon ? "BURN:\n" : ":\n");

  llvm::errs() << "Bytes: 0x";
  llvm::errs().write_hex(StartSLoc.second) << " : 0x";
  llvm::errs().write_hex(EndSLoc.second) << "\n";

  Start.dump(getSourceManager());
  End.dump(getSourceManager());

  llvm::errs() << "\n";
#endif
}

StringRef sourceFragmentActionToStr(levitation::SourceFragmentAction Action) {
  switch (Action) {
    case levitation::SourceFragmentAction::Skip:
      return "Skip";
    case levitation::SourceFragmentAction::SkipInHeaderOnly:
      return "SkipInHeaderOnly";
    case levitation::SourceFragmentAction::ReplaceWithSemicolon:
      return "ReplaceWithSemicolon";
    case levitation::SourceFragmentAction::PutExtern:
      return "PutExtern";
    case levitation::SourceFragmentAction::StartUnit:
      return "StartUnit";
    case levitation::SourceFragmentAction::StartUnitFirstDecl:
      return "StartUnitFirstDecl";
    case levitation::SourceFragmentAction::EndUnit:
      return "EndUnit";
    case levitation::SourceFragmentAction::EndUnitEOF:
      return "EndUnitEOF";
  }
}

static bool areAntonymActions(
    levitation::SourceFragmentAction Target,
    levitation::SourceFragmentAction New
) {
  switch (Target) {
    case levitation::SourceFragmentAction::EndUnit:
      return New == levitation::SourceFragmentAction::StartUnit;

    case levitation::SourceFragmentAction::StartUnit:
    case levitation::SourceFragmentAction::StartUnitFirstDecl:
      return
        New == levitation::SourceFragmentAction::EndUnit ||
        New == levitation::SourceFragmentAction::EndUnitEOF;
    default:
      return false;
  }
}

void Sema::levitationAddSourceFragmentAction(
    const clang::SourceLocation &Start,
    const clang::SourceLocation &End,
    levitation::SourceFragmentAction Action
) {

  auto StartSLoc = getSourceManager().getDecomposedLoc(Start);
  auto EndSLoc = getSourceManager().getDecomposedLoc(End);

  if(
    !getSourceManager().isWrittenInMainFile(Start) ||
    !getSourceManager().isWrittenInMainFile(End)
  )
    llvm_unreachable("Source fragment should be in main file");

  if (!LevitationSkippedFragments.empty()) {
    const auto &LastFragment = LevitationSkippedFragments.back();
    if (
        LastFragment.End == StartSLoc.second &&
        areAntonymActions(LastFragment.Action, Action)
    ) {
#ifdef DUMP_SOURCE_FRAGMENTS
      llvm::errs() << "Annihilated source fragment Idx="
                   << LevitationSkippedFragments.size()
                   << ":\n";
#endif
      LevitationSkippedFragments.pop_back();
      return;
    }
    if (LastFragment.End > StartSLoc.second)
      llvm_unreachable("Can't handle overlapping actions");
  }

  LevitationSkippedFragments.push_back({
    StartSLoc.second,
    EndSLoc.second,
    Action
  });

#ifdef DUMP_SOURCE_FRAGMENTS
  llvm::errs() << "Added source fragment: "
               << sourceFragmentActionToStr(Action) << ":\n";

  llvm::errs() << "Bytes: 0x";
  llvm::errs().write_hex(StartSLoc.second) << " : 0x";
  llvm::errs().write_hex(EndSLoc.second) << "\n";

  Start.dump(getSourceManager());
  End.dump(getSourceManager());

  llvm::errs() << "\n";
#endif
}

void Sema::levitationReplaceLastSkippedSourceFragments(
    const clang::SourceLocation &Start,
    const clang::SourceLocation &End
) {

  auto StartSLoc = getSourceManager().getDecomposedLoc(Start);
  auto EndSLoc = getSourceManager().getDecomposedLoc(End);

  size_t StartOffset = StartSLoc.second;
  size_t EndOffset = EndSLoc.second;

  auto MainFileID = getSourceManager().getMainFileID();

  if(
      StartSLoc.first != MainFileID ||
      EndSLoc.first != MainFileID
  )
    llvm_unreachable("Skipped fragment can only be a part of main file.");

  assert(
      !LevitationSkippedFragments.empty() &&
      "Fragments merging applied for non empty "
      "LevitationSkippedFragments collection only"
  );

  // Assuming that StartOffset is somewhere in middle of
  // whole LevitationSkippedFragments set.
  // Lookup for first fragment to be replaced
  size_t i = LevitationSkippedFragments.size();
  while (i)
  {
    size_t prev = i-1;
    if (StartOffset > LevitationSkippedFragments[prev].End)
      break;
    i = prev;
  }

  LevitationSkippedFragments.resize(i);

  LevitationSkippedFragments.push_back({
    StartOffset, EndOffset, levitation::SourceFragmentAction::Skip
  });

#ifdef DUMP_SOURCE_FRAGMENTS
  llvm::errs() << "Merged skipped fragment\n"
               << "  replaced fragments from idx = " << i
               << "\n";

  llvm::errs() << "New bytes: 0x";
  llvm::errs().write_hex(StartSLoc.second) << " : 0x";
  llvm::errs().write_hex(EndSLoc.second) << "\n";

  Start.dump(getSourceManager());
  End.dump(getSourceManager());

  llvm::errs() << "\n";
#endif
}

void Sema::levitationInsertExternForHeader(
    const clang::SourceLocation Start
) {

  auto StartSLoc = getSourceManager().getDecomposedLoc(Start);

  size_t StartOffset = StartSLoc.second;

  auto MainFileID = getSourceManager().getMainFileID();

  if (StartSLoc.first != MainFileID)
    llvm_unreachable("Position to insert should belong to main file");

  // Lookup for first fragment to be replaced
  size_t NumSkippedFragments = LevitationSkippedFragments.size();
  size_t InsertPos = NumSkippedFragments;

  // Note, if LevitationSkippedFragments.size() is 0, then we skip this
  // loop, and insert extern to the end of skipped fragments collection.
  for (size_t i = NumSkippedFragments; i;) { --i;
    if (LevitationSkippedFragments[i].End <= StartOffset)
      break;
    InsertPos = i;
  }

  assert(
    !InsertPos ||
    LevitationSkippedFragments[InsertPos-1].End <= StartOffset &&
    "'extern' is about to be inserted at wrong place"
  );

  LevitationSkippedFragments.insert(
      LevitationSkippedFragments.begin() + InsertPos,
      {
        StartOffset, StartOffset, levitation::SourceFragmentAction::PutExtern
      }
  );


  levitation::DeclASTMeta::FragmentTy *Prev = nullptr;
  if (InsertPos)
    Prev = &LevitationSkippedFragments[InsertPos-1];

#ifdef DUMP_SOURCE_FRAGMENTS
  llvm::errs() << "Inserted extern keyword at Idx=" << InsertPos << "\n";

  llvm::errs() << "New bytes: 0x";
  llvm::errs().write_hex(StartSLoc.second);
  llvm::errs() << "\n";

  Start.dump(getSourceManager());

  llvm::errs() << "\n";
#endif
}

void checkSortedNotOverlapped(const levitation::DeclASTMeta::FragmentsVectorTy& Fragments) {
  // Make sure, that fragments are sorted.
  if (!Fragments.empty()) {
    for (size_t i = 1, e = Fragments.size(); i != e; ++i) {
      const auto &LHS = Fragments[i - 1];
      const auto &RHS = Fragments[i];
      if (LHS.End > RHS.Start)
        llvm_unreachable("Fragments are not sorted.");
    }
  }
}

levitation::DeclASTMeta::FragmentsVectorTy
Sema::levitationGetSourceFragments() const {

  checkSortedNotOverlapped(getPreprocessor().getLevitationSkippedFragments());
  checkSortedNotOverlapped(LevitationSkippedFragments);

  levitation::DeclASTMeta::FragmentsVectorTy Fragments(
      getPreprocessor().getLevitationSkippedFragments()
  );

  Fragments.append(
      LevitationSkippedFragments.begin(),
      LevitationSkippedFragments.end()
  );

  std::sort(Fragments.begin(), Fragments.end(),
      [] (
          const levitation::DeclASTMeta::FragmentTy& LHS,
          const levitation::DeclASTMeta::FragmentTy& RHS
      ){
        return LHS.Start < RHS.Start;
      }
  );

  checkSortedNotOverlapped(Fragments);

  return Fragments;
}

// C++ Levitation Unit

void Sema::levitationActOnEnterUnit(
    const SourceLocation &StartLoc,
    const SourceLocation &EndLoc,
    const NamespaceDecl *UnitScope,
    bool AtTUBounds
) {
  LevitationUnitScope = UnitScope;
  levitationAddSourceFragmentAction(
      StartLoc, EndLoc,
      AtTUBounds ?
        levitation::SourceFragmentAction::StartUnitFirstDecl :
        levitation::SourceFragmentAction::StartUnit
  );
}

void Sema::levitationActOnLeaveUnit(
    const SourceLocation &StartLoc,
    const SourceLocation &EndLoc,
    bool AtTUBounds
) {
  levitationAddSourceFragmentAction(
      StartLoc, EndLoc,
      AtTUBounds ?
        levitation::SourceFragmentAction::EndUnitEOF :
        levitation::SourceFragmentAction::EndUnit
  );
}

bool Sema::levitationUnitScopeNotEmpty() const {
  return LevitationUnitScope && !LevitationUnitScope->decls_empty();
}
