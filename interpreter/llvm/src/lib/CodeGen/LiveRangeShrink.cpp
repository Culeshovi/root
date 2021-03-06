//===-- LiveRangeShrink.cpp - Move instructions to shrink live range ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
///===---------------------------------------------------------------------===//
///
/// \file
/// This pass moves instructions close to the definition of its operands to
/// shrink live range of the def instruction. The code motion is limited within
/// the basic block. The moved instruction should have 1 def, and more than one
/// uses, all of which are the only use of the def.
///
///===---------------------------------------------------------------------===//
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lrshrink"

STATISTIC(NumInstrsHoistedToShrinkLiveRange,
          "Number of insructions hoisted to shrink live range.");

using namespace llvm;

namespace {
class LiveRangeShrink : public MachineFunctionPass {
public:
  static char ID;

  LiveRangeShrink() : MachineFunctionPass(ID) {
    initializeLiveRangeShrinkPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return "Live Range Shrink"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // End anonymous namespace.

char LiveRangeShrink::ID = 0;
char &llvm::LiveRangeShrinkID = LiveRangeShrink::ID;

INITIALIZE_PASS(LiveRangeShrink, "lrshrink", "Live Range Shrink Pass", false,
                false)
namespace {
typedef DenseMap<MachineInstr *, unsigned> InstOrderMap;

/// Returns \p New if it's dominated by \p Old, otherwise return \p Old.
/// \p M maintains a map from instruction to its dominating order that satisfies
/// M[A] > M[B] guarantees that A is dominated by B.
/// If \p New is not in \p M, return \p Old. Otherwise if \p Old is null, return
/// \p New.
MachineInstr *FindDominatedInstruction(MachineInstr &New, MachineInstr *Old,
                                       const InstOrderMap &M) {
  auto NewIter = M.find(&New);
  if (NewIter == M.end())
    return Old;
  if (Old == nullptr)
    return &New;
  unsigned OrderOld = M.find(Old)->second;
  unsigned OrderNew = NewIter->second;
  if (OrderOld != OrderNew)
    return OrderOld < OrderNew ? &New : Old;
  // OrderOld == OrderNew, we need to iterate down from Old to see if it
  // can reach New, if yes, New is dominated by Old.
  for (MachineInstr *I = Old->getNextNode(); M.find(I)->second == OrderNew;
       I = I->getNextNode())
    if (I == &New)
      return &New;
  return Old;
}

/// Builds Instruction to its dominating order number map \p M by traversing
/// from instruction \p Start.
void BuildInstOrderMap(MachineBasicBlock::iterator Start, InstOrderMap &M) {
  M.clear();
  unsigned i = 0;
  for (MachineInstr &I : make_range(Start, Start->getParent()->end()))
    M[&I] = i++;
}
} // end anonymous namespace

bool LiveRangeShrink::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(*MF.getFunction()))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();

  DEBUG(dbgs() << "**** Analysing " << MF.getName() << '\n');

  InstOrderMap IOM;
  // Map from register to instruction order (value of IOM) where the
  // register is used last. When moving instructions up, we need to
  // make sure all its defs (including dead def) will not cross its
  // last use when moving up.
  DenseMap<unsigned, unsigned> UseMap;

  for (MachineBasicBlock &MBB : MF) {
    if (MBB.empty())
      continue;
    bool SawStore = false;
    BuildInstOrderMap(MBB.begin(), IOM);
    UseMap.clear();

    for (MachineBasicBlock::iterator Next = MBB.begin(); Next != MBB.end();) {
      MachineInstr &MI = *Next;
      ++Next;
      if (MI.isPHI() || MI.isDebugValue())
        continue;
      if (MI.mayStore())
        SawStore = true;

      unsigned CurrentOrder = IOM[&MI];
      unsigned Barrier = 0;
      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || MO.isDebug())
          continue;
        if (MO.isUse())
          UseMap[MO.getReg()] = CurrentOrder;
        else if (MO.isDead() && UseMap.count(MO.getReg()))
          // Barrier is the last instruction where MO get used. MI should not
          // be moved above Barrier.
          Barrier = std::max(Barrier, UseMap[MO.getReg()]);
      }

      if (!MI.isSafeToMove(nullptr, SawStore)) {
        // If MI has side effects, it should become a barrier for code motion.
        // IOM is rebuild from the next instruction to prevent later
        // instructions from being moved before this MI.
        if (MI.hasUnmodeledSideEffects() && Next != MBB.end()) {
          BuildInstOrderMap(Next, IOM);
          SawStore = false;
        }
        continue;
      }

      const MachineOperand *DefMO = nullptr;
      MachineInstr *Insert = nullptr;

      // Number of live-ranges that will be shortened. We do not count
      // live-ranges that are defined by a COPY as it could be coalesced later.
      unsigned NumEligibleUse = 0;

      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || MO.isDead() || MO.isDebug())
          continue;
        unsigned Reg = MO.getReg();
        // Do not move the instruction if it def/uses a physical register,
        // unless it is a constant physical register.
        if (TargetRegisterInfo::isPhysicalRegister(Reg) &&
            !MRI.isConstantPhysReg(Reg)) {
          Insert = nullptr;
          break;
        }
        if (MO.isDef()) {
          // Do not move if there is more than one def.
          if (DefMO) {
            Insert = nullptr;
            break;
          }
          DefMO = &MO;
        } else if (MRI.hasOneNonDBGUse(Reg) && MRI.hasOneDef(Reg)) {
          MachineInstr &DefInstr = *MRI.def_instr_begin(Reg);
          if (!DefInstr.isCopy())
            NumEligibleUse++;
          Insert = FindDominatedInstruction(DefInstr, Insert, IOM);
        } else {
          Insert = nullptr;
          break;
        }
      }
      // Move the instruction when # of shrunk live range > 1.
      if (DefMO && Insert && NumEligibleUse > 1 && Barrier <= IOM[Insert]) {
        MachineBasicBlock::iterator I = std::next(Insert->getIterator());
        // Skip all the PHI and debug instructions.
        while (I != MBB.end() && (I->isPHI() || I->isDebugValue()))
          I = std::next(I);
        if (I == MI.getIterator())
          continue;

        // Update the dominator order to be the same as the insertion point.
        // We do this to maintain a non-decreasing order without need to update
        // all instruction orders after the insertion point.
        unsigned NewOrder = IOM[&*I];
        IOM[&MI] = NewOrder;
        NumInstrsHoistedToShrinkLiveRange++;

        // Find MI's debug value following MI.
        MachineBasicBlock::iterator EndIter = std::next(MI.getIterator());
        if (MI.getOperand(0).isReg())
          for (; EndIter != MBB.end() && EndIter->isDebugValue() &&
                 EndIter->getOperand(0).isReg() &&
                 EndIter->getOperand(0).getReg() == MI.getOperand(0).getReg();
               ++EndIter, ++Next)
            IOM[&*EndIter] = NewOrder;
        MBB.splice(I, &MBB, MI.getIterator(), EndIter);
      }
    }
  }
  return false;
}
