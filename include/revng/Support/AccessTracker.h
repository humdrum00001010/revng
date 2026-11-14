#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>
#include <vector>

#include "llvm/ADT/StringRef.h"

#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

void onFieldAccess(llvm::StringRef FieldName,
                   llvm::StringRef StructName) debug_function;

namespace revng {

class AccessTracker;

/// Logs of the trackers that get marked, for consumers that want to visit only
/// what was read instead of the whole tree (see TouchedTracking). Both are
/// null unless such a consumer is active. Not thread safe, like the trackers.
struct AccessLogs {
  /// Trackers whose top bit went from clear to set
  std::vector<AccessTracker *> Trackers;

  /// Containers in which a key was looked up, identified by their `Exact`
  /// tracker, logged once per container until its tracking is cleared
  std::vector<const AccessTracker *> KeyLookups;
};

extern AccessLogs *ActiveAccessLogs;

///
/// A AccessCounter is optimized std::stack<bool> that can contain up to 8
/// elements.
///
class AccessTracker {
private:
  std::uint8_t Counter = 0;
  bool IsTracking = false;

public:
  class TrackingSuspender {
  private:
    AccessTracker &Tracker;
    bool WasTracking = false;

  public:
    TrackingSuspender(AccessTracker &Tracker) : Tracker(Tracker) {
      WasTracking = Tracker.IsTracking;
      Tracker.IsTracking = false;
    }

    ~TrackingSuspender() { Tracker.IsTracking = WasTracking; }

    TrackingSuspender(const TrackingSuspender &) = delete;
    TrackingSuspender(TrackingSuspender &&) = delete;
    TrackingSuspender &operator=(const TrackingSuspender &) = delete;
    TrackingSuspender &operator=(TrackingSuspender &&) = delete;
  };

public:
  AccessTracker(bool StartsActive) { IsTracking = StartsActive; }

public:
  bool operator==(const AccessTracker &Other) const = default;
  bool operator!=(const AccessTracker &Other) const = default;

public:
  void clear() {
    Counter &= ~0x1;
    IsTracking = true;
  }

  void access() {
    if (IsTracking and (Counter & 0x1) == 0) {
      Counter |= 0x1;
      if (ActiveAccessLogs != nullptr) [[unlikely]]
        ActiveAccessLogs->Trackers.push_back(this);
    }
  }

  /// Mark as read without logging: used to put back a mark that a partial
  /// clear removed
  void restoreMark() { Counter |= 0x1; }

  void resumeTracking() { IsTracking = true; }

  void push() {
    bool HasLeadingZeroes = llvm::countLeadingZeros(Counter) != 0;
    revng_assert(HasLeadingZeroes, "More than 8 pushes have been performed");
    Counter = Counter << 1;
  }

  void pop() { Counter = Counter >> 1; }
  bool front() const { return (Counter & 0x1) == 0; }
  bool peak() const { return Counter & 0x1; }
  bool isSet() const { return Counter; }
  void stopTracking() { IsTracking = false; }

  TrackingSuspender suspend() { return TrackingSuspender(*this); }
};

} // namespace revng
