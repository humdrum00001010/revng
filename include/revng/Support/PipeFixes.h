#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdlib>

/// Switches for the changes that cut the cost of the pipeline after lift. Every
/// change is on by default; the environment variable REVNG_PIPE_FIXES holds a
/// bit mask selecting which ones are active, so that the same binary can run
/// with and without each of them (0 restores the previous behavior).
namespace pipefix {

enum Values : unsigned {
  /// Per-object dependency collection: visit only the parts of the model that
  /// were read while the object was being produced, instead of walking the
  /// whole model five times per object.
  TouchedTracking = 0,

  /// isolate: build the CodeExtractorAnalysisCache over `root` once per run
  /// instead of once per function.
  SharedExtractorCache = 1,

  /// isolate: look for the `newpc` calls of the function at hand among its
  /// own instructions instead of among all the callers of `newpc`.
  LocalNewPCScan = 2,

  /// cloneFiltered: back up the metadata of the functions that will be in the
  /// clone, instead of the metadata of every function of the source module.
  FilteredMetadataBackup = 3,

  /// isolate: when a function is cloned into its own module, visit the global
  /// values that end up in that module instead of asking, for every global
  /// value of the program, whether it does.
  SelectiveModuleClone = 4,

  /// convert-functions-to-cabi: keep track of where the references to each
  /// type definition are, so that replacing the references to a converted
  /// prototype does not take a visit of the whole model per prototype.
  IndexedReferenceReplacement = 5,

  /// lift, custom invalidation: read the stored map of jump targets entry by
  /// entry instead of through `llvm::yaml::Input`, which at the end of a
  /// mapping looks every key up in a vector.
  LinearJumpTargetsRead = 6,

  /// Dependencies of the objects a pipe produces: hand them over as classes
  /// of model paths (the paths read by exactly the same objects, stored once)
  /// with the classes each object depends on, instead of one (object, path)
  /// pair per path read. Only for a consumer that asked for it (see
  /// DependencyClassesWanted).
  DependencyClasses = 7,

  /// OpaqueRegisterUser: look the function for a register up by name when it
  /// is needed, instead of collecting, every time one is constructed (once per
  /// function analyzed), all the functions of the module that carry its tags.
  LazyOpaqueFunctionLookup = 8,

  /// detect-abi, propagation of prototypes to wrappers: deduplicate the names
  /// in the model (a visit of the whole model) only if something changed since
  /// the last time, instead of after every wrapper candidate.
  DeduplicateNamesWhenNeeded = 9,

  /// isolate: collect the functions that produce opaque return addresses once
  /// per run instead of once per function (a visit of all the functions of
  /// the module, which grows by one function at each step).
  SharedOpaqueReturnAddressPool = 10,

  /// analyze-data-layout: when a link is added between two nodes of the type
  /// system, do not search the set of all nodes for each of them (a check that
  /// they belong to it), and find the tag of equality links once instead of
  /// searching the set of tags at every link.
  CheaperLayoutLinks = 11,

  /// analyze-data-layout: all the nodes that point to the same node are
  /// meant to become one node. Say so with a chain of equality links (one
  /// link per node) built when all of them are known, instead of linking
  /// every access node to all the nodes that pointed to its pointee before
  /// it: all the modules share one LLVMContext, so a constant is one value,
  /// one pointee, for the whole program, and the number of links grows with
  /// the square of the number of stores of that constant.
  ChainedAccessEquality = 12,

  /// analyze-data-layout, two visits of the type system that start from
  /// every node (or every root) in turn and each time walk again whatever
  /// the earlier ones walked:
  /// - the search for strongly connected components (CollapseEqualitySCC,
  ///   CollapseInstanceAtOffset0SCC) keeps one record of the nodes seen for
  ///   all the starting points, as a single run of the algorithm over
  ///   several roots does;
  /// - the removal of back edges from the loops made of instance edges
  ///   remembers the nodes reached by a visit that found no loop: nothing
  ///   reachable from them is on a loop, and no later visit needs to enter
  ///   them.
  VisitLayoutNodesOnce = 15
};

/// Whether whoever receives the output of the pipes can take the dependencies
/// as classes. Set by the Python module; the command line tools that run a
/// single pipe leave it unset and get the list of pairs.
extern bool DependencyClassesWanted;

inline bool enabled(Values Fix) {
  static const unsigned long Mask = [] {
    const char *Env = std::getenv("REVNG_PIPE_FIXES");
    return Env != nullptr ? std::strtoul(Env, nullptr, 0) : ~0ul;
  }();
  return ((Mask >> Fix) & 1) != 0;
}

} // namespace pipefix
