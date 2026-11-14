#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdio>
#include <cstdlib>

#include "revng/Support/Assert.h"

/// Switches for the changes that cut the cost of the value materializer during
/// lift. The environment variable REVNG_VM_FIXES holds a bit mask selecting
/// which ones are active, so that the same binary can run with and without
/// each of them (0 restores the previous behavior). When it is not set, the
/// changes that compute what the previous code computed are on, and the one
/// that does not (StackPopNotFollowed) is off.
namespace vmfix {

enum Values : unsigned {
  /// AdvancedValueInfo: collect the trunc/zext candidates of a tracked
  /// instruction once instead of walking all its users on every application of
  /// the transfer function.
  CandidateCache = 0,

  /// nodesBetween: do not visit the whole graph from the target when the
  /// intersection can be computed inside the first visit.
  BoundedNodesBetween = 1,

  /// ControlFlowEdgesGraph::fromNodeSet: do not scan all the successors of a
  /// switch with many cases.
  EdgesFromUses = 2,

  /// Do not follow, in the data-flow graph, a load that pops the program
  /// counter from the top of the stack. Unlike the others, this is not the
  /// same analysis: the graphs of the queries behind returns get smaller. No
  /// lifted module was seen to change with it, and it saves little once the
  /// others are in: off unless REVNG_VM_FIXES asks for it.
  StackPopNotFollowed = 3,

  /// promoteCSVsToAlloca: visit the function once instead of once per CSV.
  SingleSweepPromotion = 4
};

/// What is active when REVNG_VM_FIXES is not set
inline constexpr unsigned long DefaultMask = ~(1ul << StackPopNotFollowed);

inline bool enabled(Values Fix) {
  static const unsigned long Mask = [] {
    const char *Env = std::getenv("REVNG_VM_FIXES");
    return Env != nullptr ? std::strtoul(Env, nullptr, 0) : DefaultMask;
  }();
  return ((Mask >> Fix) & 1) != 0;
}

/// Self-check mode (REVNG_VM_FIXES_VERIFY=1): wherever one of the changes
/// above replaces a computation, the previous computation is performed too and
/// the two results are required to be identical (same elements, same order).
inline bool verifying() {
  static const bool Verify = [] {
    const char *Env = std::getenv("REVNG_VM_FIXES_VERIFY");
    return Env != nullptr and std::strtoul(Env, nullptr, 0) != 0;
  }();
  return Verify;
}

/// Counts the comparisons performed in self-check mode and reports them when
/// the process exits, so that a run can show the checks actually took place.
struct VerifyCounter {
  const char *Name;
  unsigned long Compared = 0;
  unsigned long Elements = 0;

  explicit VerifyCounter(const char *Name) : Name(Name) {}
  ~VerifyCounter() {
    if (Compared != 0)
      std::fprintf(stderr,
                   "[vmfix-verify] %s: %lu comparisons (%lu elements), all "
                   "identical\n",
                   Name,
                   Compared,
                   Elements);
  }

  void record(unsigned long Size) {
    ++Compared;
    Elements += Size;
  }
};

[[noreturn]] inline void verificationFailed(const char *Name) {
  std::fprintf(stderr, "[vmfix-verify] MISMATCH in %s\n", Name);
  revng_abort("A switchable change of the value materializer changed a result");
}

} // namespace vmfix
