//===- tools/dsymutil/DwarfLinker.cpp - Dwarf debug info linker -----------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "DebugMap.h"
#include "BinaryHolder.h"
#include "DebugMap.h"
#include "dsymutil.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugInfoEntry.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/LEB128.h"
#include <string>

namespace llvm {
namespace dsymutil {

namespace {

/// \brief Stores all information relating to a compile unit, be it in
/// its original instance in the object file to its brand new cloned
/// and linked DIE tree.
class CompileUnit {
public:
  /// \brief Information gathered about a DIE in the object file.
  struct DIEInfo {
    uint64_t Address;   ///< Linked address of the described entity.
    uint32_t ParentIdx; ///< The index of this DIE's parent.
    bool Keep;          ///< Is the DIE part of the linked output?
    bool InDebugMap;    ///< Was this DIE's entity found in the map?
  };

  CompileUnit(DWARFUnit &OrigUnit) : OrigUnit(OrigUnit) {
    Info.resize(OrigUnit.getNumDIEs());
  }

  DWARFUnit &getOrigUnit() const { return OrigUnit; }

  DIEInfo &getInfo(unsigned Idx) { return Info[Idx]; }
  const DIEInfo &getInfo(unsigned Idx) const { return Info[Idx]; }

private:
  DWARFUnit &OrigUnit;
  std::vector<DIEInfo> Info; ///< DIE info indexed by DIE index.
};

/// \brief The core of the Dwarf linking logic.
///
/// The link of the dwarf information from the object files will be
/// driven by the selection of 'root DIEs', which are DIEs that
/// describe variables or functions that are present in the linked
/// binary (and thus have entries in the debug map). All the debug
/// information that will be linked (the DIEs, but also the line
/// tables, ranges, ...) is derived from that set of root DIEs.
///
/// The root DIEs are identified because they contain relocations that
/// correspond to a debug map entry at specific places (the low_pc for
/// a function, the location for a variable). These relocations are
/// called ValidRelocs in the DwarfLinker and are gathered as a very
/// first step when we start processing a DebugMapObject.
class DwarfLinker {
public:
  DwarfLinker(StringRef OutputFilename, bool Verbose)
      : OutputFilename(OutputFilename), Verbose(Verbose), BinHolder(Verbose) {}

  /// \brief Link the contents of the DebugMap.
  bool link(const DebugMap &);

private:
  /// \brief Called at the start of a debug object link.
  void startDebugObject(DWARFContext &);

  /// \brief Called at the end of a debug object link.
  void endDebugObject();

  /// \defgroup FindValidRelocations Translate debug map into a list
  /// of relevant relocations
  ///
  /// @{
  struct ValidReloc {
    uint32_t Offset;
    uint32_t Size;
    uint64_t Addend;
    const DebugMapObject::DebugMapEntry *Mapping;

    ValidReloc(uint32_t Offset, uint32_t Size, uint64_t Addend,
               const DebugMapObject::DebugMapEntry *Mapping)
        : Offset(Offset), Size(Size), Addend(Addend), Mapping(Mapping) {}

    bool operator<(const ValidReloc &RHS) const { return Offset < RHS.Offset; }
  };

  /// \brief The valid relocations for the current DebugMapObject.
  /// This vector is sorted by relocation offset.
  std::vector<ValidReloc> ValidRelocs;

  /// \brief Index into ValidRelocs of the next relocation to
  /// consider. As we walk the DIEs in acsending file offset and as
  /// ValidRelocs is sorted by file offset, keeping this index
  /// uptodate is all we have to do to have a cheap lookup during the
  /// root DIE selection.
  unsigned NextValidReloc;

  bool findValidRelocsInDebugInfo(const object::ObjectFile &Obj,
                                  const DebugMapObject &DMO);

  bool findValidRelocs(const object::SectionRef &Section,
                       const object::ObjectFile &Obj,
                       const DebugMapObject &DMO);

  void findValidRelocsMachO(const object::SectionRef &Section,
                            const object::MachOObjectFile &Obj,
                            const DebugMapObject &DMO);
  /// @}

  /// \defgroup FindRootDIEs Find DIEs corresponding to debug map entries.
  ///
  /// @{
  /// \brief Recursively walk the \p DIE tree and look for DIEs to
  /// keep. Store that information in \p CU's DIEInfo.
  void lookForDIEsToKeep(const DWARFDebugInfoEntryMinimal &DIE,
                         const DebugMapObject &DMO, CompileUnit &CU,
                         unsigned Flags);

  /// \brief Flags passed to DwarfLinker::lookForDIEsToKeep
  enum TravesalFlags {
    TF_Keep = 1 << 0,            ///< Mark the traversed DIEs as kept.
    TF_InFunctionScope = 1 << 1, ///< Current scope is a fucntion scope.
    TF_DependencyWalk = 1 << 2,  ///< Walking the dependencies of a kept DIE.
    TF_ParentWalk = 1 << 3,      ///< Walking up the parents of a kept DIE.
  };

  /// \brief Mark the passed DIE as well as all the ones it depends on
  /// as kept.
  void keepDIEAndDenpendencies(const DWARFDebugInfoEntryMinimal &DIE,
                               CompileUnit::DIEInfo &MyInfo,
                               const DebugMapObject &DMO, CompileUnit &CU,
                               unsigned Flags);

  unsigned shouldKeepDIE(const DWARFDebugInfoEntryMinimal &DIE,
                         CompileUnit &Unit, CompileUnit::DIEInfo &MyInfo,
                         unsigned Flags);

  unsigned shouldKeepVariableDIE(const DWARFDebugInfoEntryMinimal &DIE,
                                 CompileUnit &Unit,
                                 CompileUnit::DIEInfo &MyInfo, unsigned Flags);

  unsigned shouldKeepSubprogramDIE(const DWARFDebugInfoEntryMinimal &DIE,
                                   CompileUnit &Unit,
                                   CompileUnit::DIEInfo &MyInfo,
                                   unsigned Flags);

  bool hasValidRelocation(uint32_t StartOffset, uint32_t EndOffset,
                          CompileUnit::DIEInfo &Info);
  /// @}

  /// \defgroup Helpers Various helper methods.
  ///
  /// @{
  const DWARFDebugInfoEntryMinimal *
  resolveDIEReference(DWARFFormValue &RefValue, const DWARFUnit &Unit,
                      const DWARFDebugInfoEntryMinimal &DIE,
                      CompileUnit *&ReferencedCU);

  CompileUnit *getUnitForOffset(unsigned Offset);

  void reportWarning(const Twine &Warning, const DWARFUnit *Unit = nullptr,
                     const DWARFDebugInfoEntryMinimal *DIE = nullptr);
  /// @}

private:
  std::string OutputFilename;
  bool Verbose;
  BinaryHolder BinHolder;

  /// The units of the current debug map object.
  std::vector<CompileUnit> Units;

  /// The debug map object curently under consideration.
  DebugMapObject *CurrentDebugObject;
};

/// \brief Similar to DWARFUnitSection::getUnitForOffset(), but
/// returning our CompileUnit object instead.
CompileUnit *DwarfLinker::getUnitForOffset(unsigned Offset) {
  auto CU =
      std::upper_bound(Units.begin(), Units.end(), Offset,
                       [](uint32_t LHS, const CompileUnit &RHS) {
                         return LHS < RHS.getOrigUnit().getNextUnitOffset();
                       });
  return CU != Units.end() ? &*CU : nullptr;
}

/// \brief Resolve the DIE attribute reference that has been
/// extracted in \p RefValue. The resulting DIE migh be in another
/// CompileUnit which is stored into \p ReferencedCU.
/// \returns null if resolving fails for any reason.
const DWARFDebugInfoEntryMinimal *DwarfLinker::resolveDIEReference(
    DWARFFormValue &RefValue, const DWARFUnit &Unit,
    const DWARFDebugInfoEntryMinimal &DIE, CompileUnit *&RefCU) {
  assert(RefValue.isFormClass(DWARFFormValue::FC_Reference));
  uint64_t RefOffset = *RefValue.getAsReference(&Unit);

  if ((RefCU = getUnitForOffset(RefOffset)))
    if (const auto *RefDie = RefCU->getOrigUnit().getDIEForOffset(RefOffset))
      return RefDie;

  reportWarning("could not find referenced DIE", &Unit, &DIE);
  return nullptr;
}

/// \brief Report a warning to the user, optionaly including
/// information about a specific \p DIE related to the warning.
void DwarfLinker::reportWarning(const Twine &Warning, const DWARFUnit *Unit,
                                const DWARFDebugInfoEntryMinimal *DIE) {
  if (CurrentDebugObject)
    errs() << Twine("while processing ") +
                  CurrentDebugObject->getObjectFilename() + ":\n";
  errs() << Twine("warning: ") + Warning + "\n";

  if (!Verbose || !DIE)
    return;

  errs() << "    in DIE:\n";
  DIE->dump(errs(), const_cast<DWARFUnit *>(Unit), 0 /* RecurseDepth */,
            6 /* Indent */);
}

/// \brief Recursive helper to gather the child->parent relationships in the
/// original compile unit.
static void gatherDIEParents(const DWARFDebugInfoEntryMinimal *DIE,
                             unsigned ParentIdx, CompileUnit &CU) {
  unsigned MyIdx = CU.getOrigUnit().getDIEIndex(DIE);
  CU.getInfo(MyIdx).ParentIdx = ParentIdx;

  if (DIE->hasChildren())
    for (auto *Child = DIE->getFirstChild(); Child && !Child->isNULL();
         Child = Child->getSibling())
      gatherDIEParents(Child, MyIdx, CU);
}

static bool dieNeedsChildrenToBeMeaningful(uint32_t Tag) {
  switch (Tag) {
  default:
    return false;
  case dwarf::DW_TAG_subprogram:
  case dwarf::DW_TAG_lexical_block:
  case dwarf::DW_TAG_subroutine_type:
  case dwarf::DW_TAG_structure_type:
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_union_type:
    return true;
  }
  llvm_unreachable("Invalid Tag");
}

void DwarfLinker::startDebugObject(DWARFContext &Dwarf) {
  Units.reserve(Dwarf.getNumCompileUnits());
  NextValidReloc = 0;
}

void DwarfLinker::endDebugObject() {
  Units.clear();
  ValidRelocs.clear();
}

/// \brief Iterate over the relocations of the given \p Section and
/// store the ones that correspond to debug map entries into the
/// ValidRelocs array.
void DwarfLinker::findValidRelocsMachO(const object::SectionRef &Section,
                                       const object::MachOObjectFile &Obj,
                                       const DebugMapObject &DMO) {
  StringRef Contents;
  Section.getContents(Contents);
  DataExtractor Data(Contents, Obj.isLittleEndian(), 0);

  for (const object::RelocationRef &Reloc : Section.relocations()) {
    object::DataRefImpl RelocDataRef = Reloc.getRawDataRefImpl();
    MachO::any_relocation_info MachOReloc = Obj.getRelocation(RelocDataRef);
    unsigned RelocSize = 1 << Obj.getAnyRelocationLength(MachOReloc);
    uint64_t Offset64;
    if ((RelocSize != 4 && RelocSize != 8) || Reloc.getOffset(Offset64)) {
      reportWarning(" unsupported relocation in debug_info section.");
      continue;
    }
    uint32_t Offset = Offset64;
    // Mach-o uses REL relocations, the addend is at the relocation offset.
    uint64_t Addend = Data.getUnsigned(&Offset, RelocSize);

    auto Sym = Reloc.getSymbol();
    if (Sym != Obj.symbol_end()) {
      StringRef SymbolName;
      if (Sym->getName(SymbolName)) {
        reportWarning("error getting relocation symbol name.");
        continue;
      }
      if (const auto *Mapping = DMO.lookupSymbol(SymbolName))
        ValidRelocs.emplace_back(Offset64, RelocSize, Addend, Mapping);
    } else if (const auto *Mapping = DMO.lookupObjectAddress(Addend)) {
      // Do not store the addend. The addend was the address of the
      // symbol in the object file, the address in the binary that is
      // stored in the debug map doesn't need to be offseted.
      ValidRelocs.emplace_back(Offset64, RelocSize, 0, Mapping);
    }
  }
}

/// \brief Dispatch the valid relocation finding logic to the
/// appropriate handler depending on the object file format.
bool DwarfLinker::findValidRelocs(const object::SectionRef &Section,
                                  const object::ObjectFile &Obj,
                                  const DebugMapObject &DMO) {
  // Dispatch to the right handler depending on the file type.
  if (auto *MachOObj = dyn_cast<object::MachOObjectFile>(&Obj))
    findValidRelocsMachO(Section, *MachOObj, DMO);
  else
    reportWarning(Twine("unsupported object file type: ") + Obj.getFileName());

  if (ValidRelocs.empty())
    return false;

  // Sort the relocations by offset. We will walk the DIEs linearly in
  // the file, this allows us to just keep an index in the relocation
  // array that we advance during our walk, rather than resorting to
  // some associative container. See DwarfLinker::NextValidReloc.
  std::sort(ValidRelocs.begin(), ValidRelocs.end());
  return true;
}

/// \brief Look for relocations in the debug_info section that match
/// entries in the debug map. These relocations will drive the Dwarf
/// link by indicating which DIEs refer to symbols present in the
/// linked binary.
/// \returns wether there are any valid relocations in the debug info.
bool DwarfLinker::findValidRelocsInDebugInfo(const object::ObjectFile &Obj,
                                             const DebugMapObject &DMO) {
  // Find the debug_info section.
  for (const object::SectionRef &Section : Obj.sections()) {
    StringRef SectionName;
    Section.getName(SectionName);
    SectionName = SectionName.substr(SectionName.find_first_not_of("._"));
    if (SectionName != "debug_info")
      continue;
    return findValidRelocs(Section, Obj, DMO);
  }
  return false;
}

/// \brief Checks that there is a relocation against an actual debug
/// map entry between \p StartOffset and \p NextOffset.
///
/// This function must be called with offsets in strictly ascending
/// order because it never looks back at relocations it already 'went past'.
/// \returns true and sets Info.InDebugMap if it is the case.
bool DwarfLinker::hasValidRelocation(uint32_t StartOffset, uint32_t EndOffset,
                                     CompileUnit::DIEInfo &Info) {
  assert(NextValidReloc == 0 ||
         StartOffset > ValidRelocs[NextValidReloc - 1].Offset);
  if (NextValidReloc >= ValidRelocs.size())
    return false;

  uint64_t RelocOffset = ValidRelocs[NextValidReloc].Offset;

  // We might need to skip some relocs that we didn't consider. For
  // example the high_pc of a discarded DIE might contain a reloc that
  // is in the list because it actually corresponds to the start of a
  // function that is in the debug map.
  while (RelocOffset < StartOffset && NextValidReloc < ValidRelocs.size() - 1)
    RelocOffset = ValidRelocs[++NextValidReloc].Offset;

  if (RelocOffset < StartOffset || RelocOffset >= EndOffset)
    return false;

  const auto &ValidReloc = ValidRelocs[NextValidReloc++];
  if (Verbose)
    outs() << "Found valid debug map entry: " << ValidReloc.Mapping->getKey()
           << " " << format("\t%016" PRIx64 " => %016" PRIx64,
                            ValidReloc.Mapping->getValue().ObjectAddress,
                            ValidReloc.Mapping->getValue().BinaryAddress);

  Info.Address =
      ValidReloc.Mapping->getValue().BinaryAddress + ValidReloc.Addend;
  Info.InDebugMap = true;
  return true;
}

/// \brief Get the starting and ending (exclusive) offset for the
/// attribute with index \p Idx descibed by \p Abbrev. \p Offset is
/// supposed to point to the position of the first attribute described
/// by \p Abbrev.
/// \return [StartOffset, EndOffset) as a pair.
static std::pair<uint32_t, uint32_t>
getAttributeOffsets(const DWARFAbbreviationDeclaration *Abbrev, unsigned Idx,
                    unsigned Offset, const DWARFUnit &Unit) {
  DataExtractor Data = Unit.getDebugInfoExtractor();

  for (unsigned i = 0; i < Idx; ++i)
    DWARFFormValue::skipValue(Abbrev->getFormByIndex(i), Data, &Offset, &Unit);

  uint32_t End = Offset;
  DWARFFormValue::skipValue(Abbrev->getFormByIndex(Idx), Data, &End, &Unit);

  return std::make_pair(Offset, End);
}

/// \brief Check if a variable describing DIE should be kept.
/// \returns updated TraversalFlags.
unsigned DwarfLinker::shouldKeepVariableDIE(
    const DWARFDebugInfoEntryMinimal &DIE, CompileUnit &Unit,
    CompileUnit::DIEInfo &MyInfo, unsigned Flags) {
  const auto *Abbrev = DIE.getAbbreviationDeclarationPtr();

  // Global variables with constant value can always be kept.
  if (!(Flags & TF_InFunctionScope) &&
      Abbrev->findAttributeIndex(dwarf::DW_AT_const_value) != -1U) {
    MyInfo.InDebugMap = true;
    return Flags | TF_Keep;
  }

  uint32_t LocationIdx = Abbrev->findAttributeIndex(dwarf::DW_AT_location);
  if (LocationIdx == -1U)
    return Flags;

  uint32_t Offset = DIE.getOffset() + getULEB128Size(Abbrev->getCode());
  const DWARFUnit &OrigUnit = Unit.getOrigUnit();
  uint32_t LocationOffset, LocationEndOffset;
  std::tie(LocationOffset, LocationEndOffset) =
      getAttributeOffsets(Abbrev, LocationIdx, Offset, OrigUnit);

  // See if there is a relocation to a valid debug map entry inside
  // this variable's location. The order is important here. We want to
  // always check in the variable has a valid relocation, so that the
  // DIEInfo is filled. However, we don't want a static variable in a
  // function to force us to keep the enclosing function.
  if (!hasValidRelocation(LocationOffset, LocationEndOffset, MyInfo) ||
      (Flags & TF_InFunctionScope))
    return Flags;

  if (Verbose)
    DIE.dump(outs(), const_cast<DWARFUnit *>(&OrigUnit), 0, 8 /* Indent */);

  return Flags | TF_Keep;
}

/// \brief Check if a function describing DIE should be kept.
/// \returns updated TraversalFlags.
unsigned DwarfLinker::shouldKeepSubprogramDIE(
    const DWARFDebugInfoEntryMinimal &DIE, CompileUnit &Unit,
    CompileUnit::DIEInfo &MyInfo, unsigned Flags) {
  const auto *Abbrev = DIE.getAbbreviationDeclarationPtr();

  Flags |= TF_InFunctionScope;

  uint32_t LowPcIdx = Abbrev->findAttributeIndex(dwarf::DW_AT_low_pc);
  if (LowPcIdx == -1U)
    return Flags;

  uint32_t Offset = DIE.getOffset() + getULEB128Size(Abbrev->getCode());
  const DWARFUnit &OrigUnit = Unit.getOrigUnit();
  uint32_t LowPcOffset, LowPcEndOffset;
  std::tie(LowPcOffset, LowPcEndOffset) =
      getAttributeOffsets(Abbrev, LowPcIdx, Offset, OrigUnit);

  uint64_t LowPc =
      DIE.getAttributeValueAsAddress(&OrigUnit, dwarf::DW_AT_low_pc, -1ULL);
  assert(LowPc != -1ULL && "low_pc attribute is not an address.");
  if (LowPc == -1ULL ||
      !hasValidRelocation(LowPcOffset, LowPcEndOffset, MyInfo))
    return Flags;

  if (Verbose)
    DIE.dump(outs(), const_cast<DWARFUnit *>(&OrigUnit), 0, 8 /* Indent */);

  return Flags | TF_Keep;
}

/// \brief Check if a DIE should be kept.
/// \returns updated TraversalFlags.
unsigned DwarfLinker::shouldKeepDIE(const DWARFDebugInfoEntryMinimal &DIE,
                                    CompileUnit &Unit,
                                    CompileUnit::DIEInfo &MyInfo,
                                    unsigned Flags) {
  switch (DIE.getTag()) {
  case dwarf::DW_TAG_constant:
  case dwarf::DW_TAG_variable:
    return shouldKeepVariableDIE(DIE, Unit, MyInfo, Flags);
  case dwarf::DW_TAG_subprogram:
    return shouldKeepSubprogramDIE(DIE, Unit, MyInfo, Flags);
  case dwarf::DW_TAG_module:
  case dwarf::DW_TAG_imported_module:
  case dwarf::DW_TAG_imported_declaration:
  case dwarf::DW_TAG_imported_unit:
    // We always want to keep these.
    return Flags | TF_Keep;
  }

  return Flags;
}


/// \brief Mark the passed DIE as well as all the ones it depends on
/// as kept.
///
/// This function is called by lookForDIEsToKeep on DIEs that are
/// newly discovered to be needed in the link. It recursively calls
/// back to lookForDIEsToKeep while adding TF_DependencyWalk to the
/// TraversalFlags to inform it that it's not doing the primary DIE
/// tree walk.
void DwarfLinker::keepDIEAndDenpendencies(const DWARFDebugInfoEntryMinimal &DIE,
                                          CompileUnit::DIEInfo &MyInfo,
                                          const DebugMapObject &DMO,
                                          CompileUnit &CU, unsigned Flags) {
  const DWARFUnit &Unit = CU.getOrigUnit();
  MyInfo.Keep = true;

  // First mark all the parent chain as kept.
  unsigned AncestorIdx = MyInfo.ParentIdx;
  while (!CU.getInfo(AncestorIdx).Keep) {
    lookForDIEsToKeep(*Unit.getDIEAtIndex(AncestorIdx), DMO, CU,
                      TF_ParentWalk | TF_Keep | TF_DependencyWalk);
    AncestorIdx = CU.getInfo(AncestorIdx).ParentIdx;
  }

  // Then we need to mark all the DIEs referenced by this DIE's
  // attributes as kept.
  DataExtractor Data = Unit.getDebugInfoExtractor();
  const auto *Abbrev = DIE.getAbbreviationDeclarationPtr();
  uint32_t Offset = DIE.getOffset() + getULEB128Size(Abbrev->getCode());

  // Mark all DIEs referenced through atttributes as kept.
  for (const auto &AttrSpec : Abbrev->attributes()) {
    DWARFFormValue Val(AttrSpec.Form);

    if (!Val.isFormClass(DWARFFormValue::FC_Reference)) {
      DWARFFormValue::skipValue(AttrSpec.Form, Data, &Offset, &Unit);
      continue;
    }

    Val.extractValue(Data, &Offset, &Unit);
    CompileUnit *ReferencedCU;
    if (const auto *RefDIE = resolveDIEReference(Val, Unit, DIE, ReferencedCU))
      lookForDIEsToKeep(*RefDIE, DMO, *ReferencedCU,
                        TF_Keep | TF_DependencyWalk);
  }
}

/// \brief Recursively walk the \p DIE tree and look for DIEs to
/// keep. Store that information in \p CU's DIEInfo.
///
/// This function is the entry point of the DIE selection
/// algorithm. It is expected to walk the DIE tree in file order and
/// (though the mediation of its helper) call hasValidRelocation() on
/// each DIE that might be a 'root DIE' (See DwarfLinker class
/// comment).
/// While walking the dependencies of root DIEs, this function is
/// also called, but during these dependency walks the file order is
/// not respected. The TF_DependencyWalk flag tells us which kind of
/// traversal we are currently doing.
void DwarfLinker::lookForDIEsToKeep(const DWARFDebugInfoEntryMinimal &DIE,
                                    const DebugMapObject &DMO, CompileUnit &CU,
                                    unsigned Flags) {
  unsigned Idx = CU.getOrigUnit().getDIEIndex(&DIE);
  CompileUnit::DIEInfo &MyInfo = CU.getInfo(Idx);
  bool AlreadyKept = MyInfo.Keep;

  // If the Keep flag is set, we are marking a required DIE's
  // dependencies. If our target is already marked as kept, we're all
  // set.
  if ((Flags & TF_DependencyWalk) && AlreadyKept)
    return;

  // We must not call shouldKeepDIE while called from keepDIEAndDenpendencies,
  // because it would screw up the relocation finding logic.
  if (!(Flags & TF_DependencyWalk))
    Flags = shouldKeepDIE(DIE, CU, MyInfo, Flags);

  // If it is a newly kept DIE mark it as well as all its dependencies as kept.
  if (!AlreadyKept && (Flags & TF_Keep))
    keepDIEAndDenpendencies(DIE, MyInfo, DMO, CU, Flags);

  // The TF_ParentWalk flag tells us that we are currently walking up
  // the parent chain of a required DIE, and we don't want to mark all
  // the children of the parents as kept (consider for example a
  // DW_TAG_namespace node in the parent chain). There are however a
  // set of DIE types for which we want to ignore that directive and still
  // walk their children.
  if (dieNeedsChildrenToBeMeaningful(DIE.getTag()))
    Flags &= ~TF_ParentWalk;

  if (!DIE.hasChildren() || (Flags & TF_ParentWalk))
    return;

  for (auto *Child = DIE.getFirstChild(); Child && !Child->isNULL();
       Child = Child->getSibling())
    lookForDIEsToKeep(*Child, DMO, CU, Flags);
}

bool DwarfLinker::link(const DebugMap &Map) {

  if (Map.begin() == Map.end()) {
    errs() << "Empty debug map.\n";
    return false;
  }

  for (const auto &Obj : Map.objects()) {
    CurrentDebugObject = Obj.get();

    if (Verbose)
      outs() << "DEBUG MAP OBJECT: " << Obj->getObjectFilename() << "\n";
    auto ErrOrObj = BinHolder.GetObjectFile(Obj->getObjectFilename());
    if (std::error_code EC = ErrOrObj.getError()) {
      reportWarning(Twine(Obj->getObjectFilename()) + ": " + EC.message());
      continue;
    }

    // Look for relocations that correspond to debug map entries.
    if (!findValidRelocsInDebugInfo(*ErrOrObj, *Obj)) {
      if (Verbose)
        outs() << "No valid relocations found. Skipping.\n";
      continue;
    }

    // Setup access to the debug info.
    DWARFContextInMemory DwarfContext(*ErrOrObj);
    startDebugObject(DwarfContext);

    // In a first phase, just read in the debug info and store the DIE
    // parent links that we will use during the next phase.
    for (const auto &CU : DwarfContext.compile_units()) {
      auto *CUDie = CU->getCompileUnitDIE(false);
      if (Verbose) {
        outs() << "Input compilation unit:";
        CUDie->dump(outs(), CU.get(), 0);
      }
      Units.emplace_back(*CU);
      gatherDIEParents(CUDie, 0, Units.back());
    }

    // Then mark all the DIEs that need to be present in the linked
    // output and collect some information about them. Note that this
    // loop can not be merged with the previous one becaue cross-cu
    // references require the ParentIdx to be setup for every CU in
    // the object file before calling this.
    for (auto &CurrentUnit : Units)
      lookForDIEsToKeep(*CurrentUnit.getOrigUnit().getCompileUnitDIE(), *Obj,
                        CurrentUnit, 0);

    // Clean-up before starting working on the next object.
    endDebugObject();
  }

  return true;
}
}

bool linkDwarf(StringRef OutputFilename, const DebugMap &DM, bool Verbose) {
  DwarfLinker Linker(OutputFilename, Verbose);
  return Linker.link(DM);
}
}
}
