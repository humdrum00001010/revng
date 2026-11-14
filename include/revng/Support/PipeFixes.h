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

  /// Dependencies of the objects a pipe produces: hand them over as classes
  /// of model paths (the paths read by exactly the same objects, stored once)
  /// with the classes each object depends on, instead of one (object, path)
  /// pair per path read. Only for a consumer that asked for it (see
  /// DependencyClassesWanted).
  DependencyClasses = 7
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
