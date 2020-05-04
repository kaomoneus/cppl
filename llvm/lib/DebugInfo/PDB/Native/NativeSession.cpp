//===- NativeSession.cpp - Native implementation of IPDBSession -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Native/NativeSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"
#include "llvm/DebugInfo/PDB/IPDBSourceFile.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeCompilandSymbol.h"
#include "llvm/DebugInfo/PDB/Native/NativeEnumInjectedSources.h"
#include "llvm/DebugInfo/PDB/Native/NativeEnumTypes.h"
#include "llvm/DebugInfo/PDB/Native/NativeExeSymbol.h"
#include "llvm/DebugInfo/PDB/Native/NativeTypeBuiltin.h"
#include "llvm/DebugInfo/PDB/Native/NativeTypeEnum.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/RawError.h"
#include "llvm/DebugInfo/PDB/Native/SymbolCache.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDBSymbolCompiland.h"
#include "llvm/DebugInfo/PDB/PDBSymbolExe.h"
#include "llvm/DebugInfo/PDB/PDBSymbolTypeEnum.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

using namespace llvm;
using namespace llvm::msf;
using namespace llvm::pdb;

static DbiStream *getDbiStreamPtr(PDBFile &File) {
  Expected<DbiStream &> DbiS = File.getPDBDbiStream();
  if (DbiS)
    return &DbiS.get();

  consumeError(DbiS.takeError());
  return nullptr;
}

NativeSession::NativeSession(std::unique_ptr<PDBFile> PdbFile,
                             std::unique_ptr<BumpPtrAllocator> Allocator)
    : Pdb(std::move(PdbFile)), Allocator(std::move(Allocator)),
      Cache(*this, getDbiStreamPtr(*Pdb)) {}

NativeSession::~NativeSession() = default;

Error NativeSession::createFromPdb(std::unique_ptr<MemoryBuffer> Buffer,
                                   std::unique_ptr<IPDBSession> &Session) {
  StringRef Path = Buffer->getBufferIdentifier();
  auto Stream = std::make_unique<MemoryBufferByteStream>(
      std::move(Buffer), llvm::support::little);

  auto Allocator = std::make_unique<BumpPtrAllocator>();
  auto File = std::make_unique<PDBFile>(Path, std::move(Stream), *Allocator);
  if (auto EC = File->parseFileHeaders())
    return EC;
  if (auto EC = File->parseStreamData())
    return EC;

  Session =
      std::make_unique<NativeSession>(std::move(File), std::move(Allocator));

  return Error::success();
}

static Expected<std::unique_ptr<PDBFile>>
loadPdbFile(StringRef PdbPath, std::unique_ptr<BumpPtrAllocator> &Allocator) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> ErrorOrBuffer =
      MemoryBuffer::getFile(PdbPath, /*FileSize=*/-1,
                            /*RequiresNullTerminator=*/false);
  if (!ErrorOrBuffer)
    return make_error<RawError>(ErrorOrBuffer.getError());
  std::unique_ptr<llvm::MemoryBuffer> Buffer = std::move(*ErrorOrBuffer);

  PdbPath = Buffer->getBufferIdentifier();
  file_magic Magic;
  auto EC = identify_magic(PdbPath, Magic);
  if (EC || Magic != file_magic::pdb)
    return make_error<RawError>(EC);

  auto Stream = std::make_unique<MemoryBufferByteStream>(std::move(Buffer),
                                                         llvm::support::little);

  auto File = std::make_unique<PDBFile>(PdbPath, std::move(Stream), *Allocator);
  if (auto EC = File->parseFileHeaders())
    return std::move(EC);

  if (auto EC = File->parseStreamData())
    return std::move(EC);

  return std::move(File);
}

Error NativeSession::createFromPdbPath(StringRef PdbPath,
                                       std::unique_ptr<IPDBSession> &Session) {
  auto Allocator = std::make_unique<BumpPtrAllocator>();
  auto PdbFile = loadPdbFile(PdbPath, Allocator);
  if (!PdbFile)
    return PdbFile.takeError();

  Session = std::make_unique<NativeSession>(std::move(PdbFile.get()),
                                            std::move(Allocator));
  return Error::success();
}

static Expected<std::string> getPdbPathFromExe(StringRef ExePath) {
  Expected<object::OwningBinary<object::Binary>> BinaryFile =
      object::createBinary(ExePath);
  if (!BinaryFile)
    return BinaryFile.takeError();

  const object::COFFObjectFile *ObjFile =
      dyn_cast<object::COFFObjectFile>(BinaryFile->getBinary());
  if (!ObjFile)
    return make_error<RawError>(raw_error_code::invalid_format);

  StringRef PdbPath;
  const llvm::codeview::DebugInfo *PdbInfo = nullptr;
  if (auto EC = ObjFile->getDebugPDBInfo(PdbInfo, PdbPath))
    return make_error<RawError>(EC);

  return std::string(PdbPath);
}

Error NativeSession::createFromExe(StringRef ExePath,
                                   std::unique_ptr<IPDBSession> &Session) {
  Expected<std::string> PdbPath = getPdbPathFromExe(ExePath);
  if (!PdbPath)
    return PdbPath.takeError();

  file_magic Magic;
  auto EC = identify_magic(PdbPath.get(), Magic);
  if (EC || Magic != file_magic::pdb)
    return make_error<RawError>(EC);

  auto Allocator = std::make_unique<BumpPtrAllocator>();
  auto File = loadPdbFile(PdbPath.get(), Allocator);
  if (!File)
    return File.takeError();

  Session = std::make_unique<NativeSession>(std::move(File.get()),
                                            std::move(Allocator));

  return Error::success();
}

Expected<std::string>
NativeSession::searchForPdb(const PdbSearchOptions &Opts) {
  Expected<std::string> PathOrErr = getPdbPathFromExe(Opts.ExePath);
  if (!PathOrErr)
    return PathOrErr.takeError();
  StringRef PathFromExe = PathOrErr.get();
  sys::path::Style Style = PathFromExe.startswith("/")
                               ? sys::path::Style::posix
                               : sys::path::Style::windows;
  StringRef PdbName = sys::path::filename(PathFromExe, Style);

  // Check if pdb exists in the executable directory.
  SmallString<128> PdbPath = StringRef(Opts.ExePath);
  sys::path::remove_filename(PdbPath);
  sys::path::append(PdbPath, PdbName);

  auto Allocator = std::make_unique<BumpPtrAllocator>();

  if (auto File = loadPdbFile(PdbPath, Allocator))
    return std::string(PdbPath);
  else
    return File.takeError();

  return make_error<RawError>("PDB not found");
}

uint64_t NativeSession::getLoadAddress() const { return LoadAddress; }

bool NativeSession::setLoadAddress(uint64_t Address) {
  LoadAddress = Address;
  return true;
}

std::unique_ptr<PDBSymbolExe> NativeSession::getGlobalScope() {
  return PDBSymbol::createAs<PDBSymbolExe>(*this, getNativeGlobalScope());
}

std::unique_ptr<PDBSymbol>
NativeSession::getSymbolById(SymIndexId SymbolId) const {
  return Cache.getSymbolById(SymbolId);
}

bool NativeSession::addressForVA(uint64_t VA, uint32_t &Section,
                                 uint32_t &Offset) const {
  uint32_t RVA = VA - getLoadAddress();
  return addressForRVA(RVA, Section, Offset);
}

bool NativeSession::addressForRVA(uint32_t RVA, uint32_t &Section,
                                  uint32_t &Offset) const {
  auto Dbi = Pdb->getPDBDbiStream();
  if (!Dbi)
    return false;

  Section = 0;
  Offset = 0;

  if ((int32_t)RVA < 0)
    return true;

  Offset = RVA;
  for (; Section < Dbi->getSectionHeaders().size(); ++Section) {
    auto &Sec = Dbi->getSectionHeaders()[Section];
    if (RVA < Sec.VirtualAddress)
      return true;
    Offset = RVA - Sec.VirtualAddress;
  }
  return true;
}

std::unique_ptr<PDBSymbol>
NativeSession::findSymbolByAddress(uint64_t Address, PDB_SymType Type) const {
  return nullptr;
}

std::unique_ptr<PDBSymbol>
NativeSession::findSymbolByRVA(uint32_t RVA, PDB_SymType Type) const {
  return nullptr;
}

std::unique_ptr<PDBSymbol>
NativeSession::findSymbolBySectOffset(uint32_t Sect, uint32_t Offset,
                                      PDB_SymType Type) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumLineNumbers>
NativeSession::findLineNumbers(const PDBSymbolCompiland &Compiland,
                               const IPDBSourceFile &File) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumLineNumbers>
NativeSession::findLineNumbersByAddress(uint64_t Address,
                                        uint32_t Length) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumLineNumbers>
NativeSession::findLineNumbersByRVA(uint32_t RVA, uint32_t Length) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumLineNumbers>
NativeSession::findLineNumbersBySectOffset(uint32_t Section, uint32_t Offset,
                                           uint32_t Length) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumSourceFiles>
NativeSession::findSourceFiles(const PDBSymbolCompiland *Compiland,
                               StringRef Pattern,
                               PDB_NameSearchFlags Flags) const {
  return nullptr;
}

std::unique_ptr<IPDBSourceFile>
NativeSession::findOneSourceFile(const PDBSymbolCompiland *Compiland,
                                 StringRef Pattern,
                                 PDB_NameSearchFlags Flags) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumChildren<PDBSymbolCompiland>>
NativeSession::findCompilandsForSourceFile(StringRef Pattern,
                                           PDB_NameSearchFlags Flags) const {
  return nullptr;
}

std::unique_ptr<PDBSymbolCompiland>
NativeSession::findOneCompilandForSourceFile(StringRef Pattern,
                                             PDB_NameSearchFlags Flags) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumSourceFiles> NativeSession::getAllSourceFiles() const {
  return nullptr;
}

std::unique_ptr<IPDBEnumSourceFiles> NativeSession::getSourceFilesForCompiland(
    const PDBSymbolCompiland &Compiland) const {
  return nullptr;
}

std::unique_ptr<IPDBSourceFile>
NativeSession::getSourceFileById(uint32_t FileId) const {
  return nullptr;
}

std::unique_ptr<IPDBEnumDataStreams> NativeSession::getDebugStreams() const {
  return nullptr;
}

std::unique_ptr<IPDBEnumTables> NativeSession::getEnumTables() const {
  return nullptr;
}

std::unique_ptr<IPDBEnumInjectedSources>
NativeSession::getInjectedSources() const {
  auto ISS = Pdb->getInjectedSourceStream();
  if (!ISS) {
    consumeError(ISS.takeError());
    return nullptr;
  }
  auto Strings = Pdb->getStringTable();
  if (!Strings) {
    consumeError(Strings.takeError());
    return nullptr;
  }
  return std::make_unique<NativeEnumInjectedSources>(*Pdb, *ISS, *Strings);
}

std::unique_ptr<IPDBEnumSectionContribs>
NativeSession::getSectionContribs() const {
  return nullptr;
}

std::unique_ptr<IPDBEnumFrameData>
NativeSession::getFrameData() const {
  return nullptr;
}

void NativeSession::initializeExeSymbol() {
  if (ExeSymbol == 0)
    ExeSymbol = Cache.createSymbol<NativeExeSymbol>();
}

NativeExeSymbol &NativeSession::getNativeGlobalScope() const {
  const_cast<NativeSession &>(*this).initializeExeSymbol();

  return Cache.getNativeSymbolById<NativeExeSymbol>(ExeSymbol);
}
