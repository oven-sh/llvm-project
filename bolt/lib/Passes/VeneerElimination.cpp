//===- bolt/Passes/VeneerElimination.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This class implements a pass that removes linker-inserted veneers from the
// code and redirects veneer callers to call to veneers destinations
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/VeneerElimination.h"
#define DEBUG_TYPE "veneer-elim"

using namespace llvm;

namespace opts {

extern cl::OptionCategory BoltOptCategory;

static llvm::cl::opt<bool>
    EliminateVeneers("elim-link-veneers",
                     cl::desc("run veneer elimination pass"), cl::init(true),
                     cl::Hidden, cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

// Linkers work around Cortex-A53 erratum 843419 (--fix-cortex-a53-843419) by
// moving a load/store that follows an ADRP at the end of a 4KiB page out of
// line: the instruction is replaced with `B <veneer>`, and the veneer is
// `<the instruction>; B <back>` (lld names it __CortexA53843419_<addr>, GNU ld
// e843419@...). Both branches are plain B, so unlike a call veneer nothing may
// be clobbered: any register, X16/X17 included, can be live across it.
//
// Left as is, BOLT models the site as a tail call plus a secondary entry point
// at the return address, and may later route either branch through a stub that
// uses X16 — corrupting a live register. The new layout makes the workaround
// void anyway (the ADRP moves), so undo it: put the instruction back and drop
// the veneer.
static bool isErratum843419VeneerName(StringRef Name) {
  return Name.starts_with("__CortexA53843419_") || Name.starts_with("e843419@");
}

static uint64_t undoErratum843419Veneers(BinaryContext &BC) {
  uint64_t Undone = 0;
  for (BinaryFunction &Veneer : llvm::make_second_range(BC.getBinaryFunctions())) {
    if (Veneer.isIgnored() || !Veneer.hasCFG() || Veneer.empty() ||
        !llvm::any_of(Veneer.getNames(), isErratum843419VeneerName))
      continue;
    // Exactly: <one non-control-flow instruction>; B <ReturnSymbol>
    if (Veneer.size() != 1 || Veneer.front().size() != 2)
      continue;
    MCInst &MemInst = *Veneer.front().begin();
    MCInst &Back = *std::next(Veneer.front().begin());
    if (BC.MIB->isBranch(MemInst) || BC.MIB->isCall(MemInst) ||
        BC.MIB->isReturn(MemInst) || !BC.MIB->isTailCall(Back) ||
        BC.MIB->isIndirectBranch(Back) || BC.MIB->isIndirectCall(Back))
      continue;
    const MCSymbol *ReturnSymbol = BC.MIB->getTargetSymbol(Back);
    BinaryFunction *Patched =
        ReturnSymbol ? BC.getFunctionForSymbol(ReturnSymbol) : nullptr;
    if (!Patched || Patched == &Veneer || !Patched->hasCFG() ||
        !Patched->isSimple())
      continue;

    // The block the veneer returns to (a secondary entry point of Patched, or
    // its primary one), and the block right before it, which must end in the
    // branch to the veneer.
    BinaryBasicBlock *ReturnBB = nullptr;
    for (BinaryBasicBlock &BB : *Patched) {
      if (Patched->getSecondaryEntryPointSymbol(BB) == ReturnSymbol ||
          BB.getLabel() == ReturnSymbol ||
          (Patched->getSymbol() == ReturnSymbol && &BB == &Patched->front())) {
        ReturnBB = &BB;
        break;
      }
    }
    if (!ReturnBB || ReturnBB->getInputOffset() < 4)
      continue;
    BinaryBasicBlock *SiteBB =
        Patched->getBasicBlockContainingOffset(ReturnBB->getInputOffset() - 4);
    if (!SiteBB || SiteBB == ReturnBB || SiteBB->empty())
      continue;
    MCInst &ToVeneer = *std::prev(SiteBB->end());
    const MCSymbol *ToVeneerTarget = BC.MIB->getTargetSymbol(ToVeneer);
    if (!(BC.MIB->isTailCall(ToVeneer) || BC.MIB->isBranch(ToVeneer)) ||
        !ToVeneerTarget || BC.getFunctionForSymbol(ToVeneerTarget) != &Veneer)
      continue;

    // Splice: the site's branch becomes the original instruction and falls
    // through to the return block; the veneer is no longer part of the program.
    MCInst Restored = MemInst;
    BC.MIB->stripAnnotations(Restored);
    SiteBB->eraseInstruction(std::prev(SiteBB->end()));
    SiteBB->addInstruction(std::move(Restored));
    if (!SiteBB->getSuccessor(ReturnBB->getLabel()))
      SiteBB->addSuccessor(ReturnBB, SiteBB->getKnownExecutionCount(), 0);
    Veneer.setPseudo(true);
    ++Undone;
  }
  return Undone;
}

Error VeneerElimination::runOnFunctions(BinaryContext &BC) {
  if (!opts::EliminateVeneers || !BC.isAArch64())
    return Error::success();

  if (uint64_t Undone = undoErratum843419Veneers(BC))
    BC.outs() << "BOLT-INFO: number of Cortex-A53 erratum 843419 veneers "
                 "folded back into their functions: "
              << Undone << '\n';

  std::unordered_map<const MCSymbol *, const MCSymbol *> VeneerDestinations;
  uint64_t NumEliminatedVeneers = 0;
  for (BinaryFunction &BF : llvm::make_second_range(BC.getBinaryFunctions())) {
    if (!BF.isPossibleVeneer())
      continue;

    if (BF.isIgnored())
      continue;

    MCInst &FirstInstruction = *(BF.begin()->begin());
    const MCSymbol *VeneerTargetSymbol = 0;
    uint64_t TargetAddress;
    if (BC.MIB->isTailCall(FirstInstruction)) {
      VeneerTargetSymbol = BC.MIB->getTargetSymbol(FirstInstruction);
    } else if (BC.MIB->matchAbsLongVeneer(BF, TargetAddress)) {
      if (BinaryFunction *TargetBF =
              BC.getBinaryFunctionAtAddress(TargetAddress))
        VeneerTargetSymbol = TargetBF->getSymbol();
    } else if (BC.MIB->hasAnnotation(FirstInstruction, "AArch64Veneer")) {
      VeneerTargetSymbol = BC.MIB->getTargetSymbol(FirstInstruction, 1);
    }

    if (!VeneerTargetSymbol)
      continue;

    for (const MCSymbol *Symbol : BF.getSymbols())
      VeneerDestinations[Symbol] = VeneerTargetSymbol;

    NumEliminatedVeneers++;
    BF.setPseudo(true);
  }

  BC.outs() << "BOLT-INFO: number of removed linker-inserted veneers: "
            << NumEliminatedVeneers << '\n';

  // Handle veneers to veneers in case they occur
  for (auto &Entry : VeneerDestinations) {
    const MCSymbol *Src = Entry.first;
    const MCSymbol *Dest = Entry.second;
    while (VeneerDestinations.find(Dest) != VeneerDestinations.end())
      Dest = VeneerDestinations[Dest];

    VeneerDestinations[Src] = Dest;
  }

  uint64_t VeneerCallers = 0;
  for (BinaryFunction &BF : llvm::make_second_range(BC.getBinaryFunctions())) {
    for (BinaryBasicBlock &BB : BF) {
      for (MCInst &Instr : BB) {
        if (!BC.MIB->isCall(Instr) || BC.MIB->isIndirectCall(Instr))
          continue;

        const MCSymbol *TargetSymbol = BC.MIB->getTargetSymbol(Instr, 0);
        auto It = VeneerDestinations.find(TargetSymbol);
        if (It == VeneerDestinations.end())
          continue;

        VeneerCallers++;
        BC.MIB->replaceBranchTarget(Instr, It->second, BC.Ctx.get());
      }
    }
  }

  LLVM_DEBUG(
      dbgs() << "BOLT-INFO: number of linker-inserted veneers call sites: "
             << VeneerCallers << "\n");
  (void)VeneerCallers;
  return Error::success();
}

} // namespace bolt
} // namespace llvm
