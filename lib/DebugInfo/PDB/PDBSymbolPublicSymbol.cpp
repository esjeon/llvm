//===- PDBSymbolPublicSymbol.cpp - ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/PDBSymbolPublicSymbol.h"

#include "llvm/DebugInfo/PDB/PDBSymbol.h"

#include <utility>

using namespace llvm;

PDBSymbolPublicSymbol::PDBSymbolPublicSymbol(
    const IPDBSession &PDBSession, std::unique_ptr<IPDBRawSymbol> Symbol)
    : PDBSymbol(PDBSession, std::move(Symbol)) {}

void PDBSymbolPublicSymbol::dump(raw_ostream &OS, int Indent,
                                 PDB_DumpLevel Level, PDB_DumpFlags Flags) const {
  OS << stream_indent(Indent);
  OS << "public symbol: " << getName();
}
