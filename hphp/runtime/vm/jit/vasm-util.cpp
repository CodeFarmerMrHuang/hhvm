/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/vasm-util.h"

#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/dataflow-worklist.h"

namespace HPHP { namespace jit {

namespace {

//////////////////////////////////////////////////////////////////////

bool is_nop(const copy& i) { return i.s == i.d; }
bool is_nop(const copy2& i) { return i.s0 == i.d0 && i.s1 == i.d1; }

// movb r,r is a nop, however movl is not since it zeros upper bits.
bool is_nop(const movb& i) { return i.s == i.d; }

bool is_nop(const lea& i) {
  if (i.s.disp != 0) return false;
  return
    (i.s.base == i.d && !i.s.index.isValid()) ||
    (!i.s.base.isValid() && i.s.index == i.d && i.s.scale == 1 &&
      i.s.seg == Vptr::DS);
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

bool is_trivial_nop(const Vinstr& inst) {
  return
    (inst.op == Vinstr::copy && is_nop(inst.copy_)) ||
    (inst.op == Vinstr::copy2 && is_nop(inst.copy2_)) ||
    (inst.op == Vinstr::lea && is_nop(inst.lea_)) ||
    (inst.op == Vinstr::movb && is_nop(inst.movb_)) ||
    inst.op == Vinstr::nop;
}

//////////////////////////////////////////////////////////////////////

namespace {

/*
 * Add a jump from middleLabel to destLabel, taking into account any special
 * instructions at the beginning of destLabel.
 */
void forwardJmp(Vunit& unit, jit::flat_set<size_t>& catch_blocks,
                Vlabel middleLabel, Vlabel destLabel) {
  auto& middle = unit.blocks[middleLabel];
  auto& dest = unit.blocks[destLabel];

  auto& headInst = dest.code.front();
  auto irctx = headInst.irctx();
  if (headInst.op == Vinstr::phidef) {
    // We need to preserve any phidefs in the forwarding block if they're
    // present in the original destination block.
    auto tuple = headInst.phidef_.defs;
    auto forwardedRegs = unit.tuples[tuple];
    VregList regs(forwardedRegs.size());
    for (unsigned i = 0; i < forwardedRegs.size(); ++i) {
      regs[i] = unit.makeReg();
    }
    middle.code.emplace_back(phidef{unit.makeTuple(regs)}, irctx);
    middle.code.emplace_back(phijmp{destLabel, unit.makeTuple(regs)}, irctx);
    return;
  } else if (headInst.op == Vinstr::landingpad) {
    // If the dest started with a landingpad, copy it to middle. The dest's
    // will be erased at the end of the pass.
    catch_blocks.insert(destLabel);
    assertx(middle.code.empty());
    middle.code.emplace_back(headInst);
  }

  middle.code.emplace_back(jmp{destLabel}, irctx);
}

}

/*
 * Splits the critical edges in `unit', if any.
 * Returns true iff the unit was modified.
 */
bool splitCriticalEdges(Vunit& unit) {
  jit::vector<unsigned> preds(unit.blocks.size());
  jit::flat_set<size_t> catch_blocks;

  for (size_t b = 0; b < unit.blocks.size(); b++) {
    auto succlist = succs(unit.blocks[b]);
    for (auto succ : succlist) {
      preds[succ]++;
    }
  }

  auto changed = false;
  for (size_t pred = 0; pred < unit.blocks.size(); pred++) {
    auto succlist = succs(unit.blocks[pred]);
    if (succlist.size() <= 1) continue;
    for (auto& succ : succlist) {
      if (preds[succ] <= 1) continue;
      // Split the critical edge. Use the colder of the predecessor and
      // successor to select the area of the new block.
      auto middle = unit.makeBlock(
        std::max(unit.blocks[pred].area_idx, unit.blocks[succ].area_idx),
        unit.blocks[succ].weight
      );
      forwardJmp(unit, catch_blocks, middle, succ);
      succ = middle;
      changed = true;
    }
  }

  // Remove any landingpad{} instructions that were hoisted to split edges.
  for (auto block : catch_blocks) {
    auto& code = unit.blocks[block].code;
    assertx(code.front().op == Vinstr::landingpad);
    code.front() = nop{};
  }

  return changed;
}

Vloc make_const(Vunit& unit, Type type) {
  if (type.subtypeOfAny(TUninit, TInitNull)) {
    // Return undefined value.
    return Vloc{unit.makeConst(Vconst::Quad)};
  }
  if (type <= TNullptr) return Vloc{unit.makeConst(0)};

  assertx(type.hasConstVal());
  if (type <= TBool) return Vloc{unit.makeConst(type.boolVal())};
  if (type <= TDbl) return Vloc{unit.makeConst(type.dblVal())};
  if (wide_tv_val && type <= TLvalToGen) {
    auto const rval = tv_rval{type.ptrVal()};
    auto const typeReg = unit.makeConst(&rval.type());
    auto const valReg = unit.makeConst(&rval.val());
    return Vloc{
      tv_lval::type_idx == 0 ? typeReg : valReg,
      tv_lval::val_idx == 1 ? valReg : typeReg,
    };
  }
  return Vloc{unit.makeConst(type.rawVal())};
}

//////////////////////////////////////////////////////////////////////

bool dominates(Vlabel b1, Vlabel b2, const VIdomVector& idoms) {
  assertx(b1.isValid() && b2.isValid());
  for (auto b = b2; b.isValid(); b = idoms[b]) {
    if (b == b1) return true;
  }
  return false;
}

VIdomVector findDominators(const Vunit& unit,
                           const jit::vector<Vlabel>& rpo) {
  assertx(!rpo.empty() && rpo[0] == unit.entry);

  auto const preds = computePreds(unit);
  VIdomVector idom(unit.blocks.size());

  jit::vector<size_t> rpoOrder(unit.blocks.size());
  for (size_t i = 0; i < rpo.size(); ++i) rpoOrder[rpo[i]] = i;

  dataflow_worklist<size_t> worklist(rpo.size());

  idom[unit.entry] = unit.entry;
  for (auto const succ : succs(unit.blocks[0])) {
    worklist.push(rpoOrder[succ]);
  }

  while (!worklist.empty()) {
    auto const block = rpo[worklist.pop()];
    auto const& blockPreds = preds[block];

    // Find the first already processed predecessor (there must be at least one
    // because we shouldn't be on the worklist otherwise).
    auto predIt = std::find_if(
      blockPreds.begin(),
      blockPreds.end(),
      [&] (Vlabel p) { return idom[p].isValid(); }
    );
    assertx(predIt != blockPreds.end());
    auto p1 = *predIt;

    // For all other already processed predecessors
    for (++predIt; predIt != blockPreds.end(); ++predIt) {
      auto p2 = *predIt;
      if (p2 == p1 || !idom[p2].isValid()) continue;
      do {
        // Find earliest common predecessor of p1 and p2
        while (rpoOrder[p1] < rpoOrder[p2]) p2 = idom[p2];
        while (rpoOrder[p2] < rpoOrder[p1]) p1 = idom[p1];
      } while (p1 != p2);
    }

    if (!idom[block].isValid() || idom[block] != p1) {
      idom[block] = p1;
      for (auto const succ : succs(unit.blocks[block])) {
        worklist.push(rpoOrder[succ]);
      }
    }
  }

  idom[unit.entry] = Vlabel{}; // entry has no dominator
  return idom;
}

//////////////////////////////////////////////////////////////////////

BackEdgeVector findBackEdges(const Vunit& unit,
                             const jit::vector<Vlabel>& rpo,
                             const VIdomVector& idoms) {
  BackEdgeVector backEdges;

  boost::dynamic_bitset<> seen(unit.blocks.size());
  for (auto const b : rpo) {
    seen[b] = true;
    for (auto const succ : succs(unit.blocks[b])) {
      if (!seen[succ]) continue; // If we haven't seen it, it can't dominate b,
                                 // so skip the dominator check.
      if (!dominates(succ, b, idoms)) continue;
      backEdges.emplace_back(b, succ);
    }
  }

  return backEdges;
}

LoopBlocks findLoopBlocks(const Vunit& unit,
                          const PredVector& preds,
                          const BackEdgeVector& backEdges) {

  jit::fast_map<Vlabel, jit::vector<Vlabel>> headers;
  for (auto const& edge : backEdges) {
    headers[edge.second].emplace_back(edge.first);
  }

  /*
   * Use a flood-fill algorithm, starting at the first node of the back-edge
   * (which is a predecessor of the loop header inside the loop). Any node which
   * is reachable via predecessors from that node (not passing through the
   * header block) is considered part of the loop. This may not be true for
   * irreducible loops.
   */
  auto const fillBlocks = [&] (Vlabel header,
                               const jit::vector<Vlabel>& edgePreds) {
    boost::dynamic_bitset<> visited(unit.blocks.size());
    visited[header] = true;

    jit::vector<Vlabel> blocks;
    blocks.emplace_back(header);

    jit::stack<Vlabel> worklist;
    for (auto const pred : edgePreds) worklist.push(pred);

    while (!worklist.empty()) {
      auto const block = worklist.top();
      worklist.pop();

      if (visited[block]) continue;
      visited[block] = true;

      for (auto const pred : preds[block]) worklist.push(pred);
      blocks.emplace_back(block);
    }

    return blocks;
  };

  jit::fast_map<Vlabel, jit::vector<Vlabel>> loopBlocks;
  for (auto const& p : headers) {
    loopBlocks[p.first] = fillBlocks(p.first, p.second);
  }

  return loopBlocks;
}
}}
