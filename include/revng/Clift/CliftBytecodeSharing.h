#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <atomic>
#include <cstdint>

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

#include "mlir/IR/Attributes.h"

namespace clift::bytecode {

/// While a SharingScope is alive on a thread, the bytecode of Clift class types
/// (structs and unions) written or read on that thread has one more field: a
/// flag telling whether the definition follows (as usual) or the type is only
/// a reference to its handle. A reference is read as `StructType::get(Context,
/// Handle)` (or the union equivalent), so its definition must already be in
/// the context when the type is used.
///
/// When writing, the definitions in `Inline` are written in full, every other
/// class type as a reference, and the definitions of the types written as
/// references are collected in `Referenced` (in the order they are met).
///
/// Outside of a SharingScope nothing changes: the format is the usual one.
class SharingScope {
public:
  /// The definitions (StructAttr or UnionAttr) to write in full
  llvm::DenseSet<mlir::Attribute> Inline;

  /// The definitions of the class types written as references
  llvm::SetVector<mlir::Attribute> Referenced;

private:
  SharingScope *Previous = nullptr;

public:
  SharingScope();
  ~SharingScope();
  SharingScope(const SharingScope &) = delete;
  SharingScope &operator=(const SharingScope &) = delete;

  static SharingScope *current();
};

/// Counts kept for tracing, for the whole process.
///
/// How many definitions of class types and how many references to class types
/// the bytecode reader and writer handled, and what became of the definitions
/// that were read: stored in the context (the first definition of a class
/// type) or only compared with the one already there (every later one; a
/// different one is a fatal error). The writer handles each definition twice:
/// once to number what it refers to, once to write it.
///
/// How many times a name or a comment (MutableStringAttr) was set, and how
/// many of those times it already had that value.
///
/// How many times the verifier of a module started looking for a class type
/// inside itself, and how many class types it went through doing so.
struct Counters {
  std::atomic<uint64_t> DefinitionsRead{ 0 };
  std::atomic<uint64_t> DefinitionsWritten{ 0 };
  std::atomic<uint64_t> ReferencesRead{ 0 };
  std::atomic<uint64_t> ReferencesWritten{ 0 };
  std::atomic<uint64_t> DefinitionsStored{ 0 };
  std::atomic<uint64_t> DefinitionsCompared{ 0 };
  std::atomic<uint64_t> NamesSet{ 0 };
  std::atomic<uint64_t> NamesUnchanged{ 0 };
  std::atomic<uint64_t> RecursionChecks{ 0 };
  std::atomic<uint64_t> RecursionCheckVisits{ 0 };

  static void count(std::atomic<uint64_t> &Counter) {
    Counter.fetch_add(1, std::memory_order_relaxed);
  }
};

extern Counters TheCounters;

} // namespace clift::bytecode
