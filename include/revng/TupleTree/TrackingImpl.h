#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include "revng/Support/AccessTracker.h"
#include "revng/TupleTree/Tracking.h"
#include "revng/TupleTree/Visits.h"

namespace revng {

struct TrackingImpl {
  struct PopVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.trackingPop();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().pop();
    }
  };

  struct PushVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.trackingPush();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().push();
    }
  };

  struct ClearVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.clearTracking();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().clear();
    }
  };

  struct StopTrackingVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.stopTracking();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().stopTracking();
    }
  };

  // Uncomment the following for easier debugging:
  // #define RECURSIVE_COLLECT_TUPLE

#ifdef RECURSIVE_COLLECT_TUPLE
  template<typename M, size_t I = 0, typename T>
  static void
  collectTuple(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    if constexpr (I < std::tuple_size_v<T>) {

      Stack.push_back(size_t(I));
      if (LHS.template getTracker<I>().isSet()) {
        Info.Read.push_back(Stack);
      }
      collectImpl<M>(LHS.template untrackedGet<I>(), Stack, Info);
      Stack.pop_back();

      // Recur
      collectTuple<M, I + 1>(LHS, Stack, Info);
    }
  }
#endif

  template<typename M, StrictSpecializationOf<UpcastablePointer> T>
  static void collectImpl(const T &UP, TupleTreePath &Stack, ReadFields &Info) {
    if (!UP.isEmpty()) {
      auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
      UP.upcast([&](auto &Upcasted) {
        // Don't forget to add the kind of the polymorphic object to the stack.
        static_assert(!std::is_const_v<
                      std::remove_reference_t<decltype(Upcasted)>>);
        Stack.push_back(Upcasted.Kind());
        collectImpl<M>(Upcasted, Stack, Info);
        Stack.pop_back();
      });
    }
  }

#ifdef RECURSIVE_COLLECT_TUPLE
  template<typename M, TupleSizeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTuple<M>(LHS, Stack, Info);
  }

#else
  // collectImpl remains unchanged
  template<typename M, TupleSizeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTuple<M>(LHS, Stack, Info);
  }

  // Helper function with index sequence
  template<typename M, typename T, size_t... Is>
  static void collectTupleImpl(const T &LHS,
                               TupleTreePath &Stack,
                               ReadFields &Info,
                               std::index_sequence<Is...>) {
    // Use a fold expression to process each index
    (...,
     (Stack.push_back(Is),
      (LHS.template getTracker<Is>().isSet() ?
         (Info.Read.push_back(Stack), void()) :
         void()),
      collectImpl<M>(LHS.template untrackedGet<Is>(), Stack, Info),
      Stack.pop_back()));
  }

  // Main function using index sequence
  template<typename M, typename T>
  static void
  collectTuple(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectTupleImpl<M>(LHS,
                        Stack,
                        Info,
                        std::make_index_sequence<std::tuple_size_v<T>>{});
  }
#endif

  template<typename M, revng::SetOrKOC T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    typename T::TrackingResult TrackingResult = LHS.getTrackingResult();
    if (TrackingResult.Exact)
      Info.ExactVectors.push_back(Stack);

    using KeyType = std::remove_cv_t<typename T::key_type>;
    for (KeyType Key : TrackingResult.InspectedKeys) {
      Stack.push_back(Key);
      Info.Read.push_back(Stack);
      Stack.pop_back();
    }
    for (auto &LHSElement : LHS.Content) {
      using value_type = typename T::value_type;

      using Type = decltype(LHSElement);
      auto &Mutable = const_cast<std::remove_cvref_t<Type> &>(LHSElement);
      static_assert(!std::is_const_v<
                    std::remove_reference_t<decltype(Mutable)>>);
      Stack.push_back(KeyedObjectTraits<value_type>::key(Mutable));

      collectImpl<M>(LHSElement, Stack, Info);
      Stack.pop_back();
    }
  }

  template<typename M, NotTupleTreeCompatible T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {}

  template<typename M, typename Visitor, size_t I = 0, typename T>
  static void visitTuple(const T &LHS) {
    if constexpr (I < std::tuple_size_v<T>) {

      visitImpl<M, Visitor>(LHS.template untrackedGet<I>());
      Visitor::template visitTupleElement<T, I>(LHS);

      // Recur
      visitTuple<M, Visitor, I + 1, T>(LHS);
    }
  }

  template<typename M,
           typename Visitor,
           StrictSpecializationOf<UpcastablePointer> T>
  static void visitImpl(T &UP) {
    if (!UP.isEmpty()) {
      auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
      UP.upcast([&](const auto &Upcasted) { visitImpl<M, Visitor>(Upcasted); });
    }
  }

  template<typename M, typename Visitor, TupleSizeCompatible T>
  static void visitImpl(const T &LHS) {
    visitTuple<M, Visitor>(LHS);
  }

  template<typename M, typename Visitor, revng::SetOrKOC T>
  static void visitImpl(const T &LHS) {
    for (auto &LHSElement : LHS.Content) {
      visitImpl<M, Visitor>(LHSElement);
    }
    Visitor::template visitKeyedObjectContainer<T>(LHS);
  }

  template<typename M, typename Visitor, NotTupleTreeCompatible T>
  static void visitImpl(const T &LHS) {}

  //
  // Support for visiting only a part of the tree: the root object, the fields
  // of the root that are not containers, the containers of the root themselves
  // and, among the elements of those containers, only the selected ones.
  //

  struct ResumeTrackingVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.resumeTracking();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      CurrentItem.template getTracker<FieldIndex>().resumeTracking();
    }
  };

  struct DropInspectedKeysVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.dropInspectedKeys();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &) {}
  };

  /// Identifier of an element of a container that is a field of the root
  using ElementID = uint32_t;

  /// The identifier of everything that is not in such an element
  static constexpr ElementID RestOfTheTree = ~ElementID(0);

  /// The part of what collectPartially produced that is about one element
  struct Segment {
    ElementID Element;
    uint32_t ReadBegin;
    uint32_t ReadEnd;
    uint32_t ExactBegin;
    uint32_t ExactEnd;
  };

  struct ElementLocation {
    /// Index of the field of the root holding the container
    uint32_t Field;

    /// The element, as a pointer to the `value_type` of that container
    const void *Element;
  };

  /// Records the address of every tracker in the visited part of the tree
  struct AddressCollector {
    static inline std::vector<std::pair<const void *, ElementID>>
      *Out = nullptr;
    static inline ElementID Current = RestOfTheTree;

    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      Out->emplace_back(CurrentItem.trackingIdentity(), Current);
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &CurrentItem) {
      Out->emplace_back(&CurrentItem.template getTracker<FieldIndex>(),
                        Current);
    }
  };

  /// Collect the address of all the trackers of the tree, each with the
  /// element it belongs to, and the location of all the elements
  template<typename M, size_t I = 0>
  static void index(const M &Root,
                    std::vector<std::pair<const void *, ElementID>> &Addresses,
                    std::vector<ElementLocation> &Elements) {
    if constexpr (I < std::tuple_size_v<M>) {
      AddressCollector::Out = &Addresses;

      const auto &Field = Root.template untrackedGet<I>();
      using FieldType = std::remove_cvref_t<decltype(Field)>;
      if constexpr (revng::SetOrKOC<FieldType>) {
        for (const auto &Element : Field.Content) {
          AddressCollector::Current = Elements.size();
          Elements.push_back({ uint32_t(I), &Element });
          visitImpl<M, AddressCollector>(Element);
        }
        AddressCollector::Current = RestOfTheTree;
        AddressCollector::template visitKeyedObjectContainer<FieldType>(Field);
      } else {
        AddressCollector::Current = RestOfTheTree;
        visitImpl<M, AddressCollector>(Field);
      }

      AddressCollector::Current = RestOfTheTree;
      AddressCollector::template visitTupleElement<M, I>(Root);

      index<M, I + 1>(Root, Addresses, Elements);
    }
  }

  /// Apply \p Visitor to the root, to its fields that are not containers, to
  /// its containers and to the elements listed in \p Selected
  ///
  /// \param Selected identifiers of elements, sorted
  /// \param Next index in \p Selected of the first element not visited yet
  template<typename M, typename Visitor, size_t I = 0>
  static void visitPartially(const M &Root,
                             const std::vector<ElementLocation> &Elements,
                             const std::vector<ElementID> &Selected,
                             size_t &Next) {
    if constexpr (I < std::tuple_size_v<M>) {
      const auto &Field = Root.template untrackedGet<I>();
      using FieldType = std::remove_cvref_t<decltype(Field)>;
      if constexpr (revng::SetOrKOC<FieldType>) {
        using ValueType = typename FieldType::value_type;
        while (Next < Selected.size() and Elements[Selected[Next]].Field == I) {
          const void *Pointer = Elements[Selected[Next]].Element;
          visitImpl<M, Visitor>(*static_cast<const ValueType *>(Pointer));
          ++Next;
        }
        Visitor::template visitKeyedObjectContainer<FieldType>(Field);
      } else {
        visitImpl<M, Visitor>(Field);
      }

      Visitor::template visitTupleElement<M, I>(Root);

      visitPartially<M, Visitor, I + 1>(Root, Elements, Selected, Next);
    }
  }

  /// Like collectImpl on the root, but descending only in the elements listed
  /// in \p Selected. The paths it produces for what it visits are the ones
  /// collectImpl produces.
  template<typename M, size_t I = 0>
  static void collectPartially(const M &Root,
                               const std::vector<ElementLocation> &Elements,
                               const std::vector<ElementID> &Selected,
                               size_t &Next,
                               TupleTreePath &Stack,
                               ReadFields &Info,
                               std::vector<Segment> *Segments = nullptr) {
    if constexpr (I < std::tuple_size_v<M>) {
      Stack.push_back(size_t(I));
      if (Root.template getTracker<I>().isSet())
        Info.Read.push_back(Stack);

      const auto &Field = Root.template untrackedGet<I>();
      using FieldType = std::remove_cvref_t<decltype(Field)>;
      if constexpr (revng::SetOrKOC<FieldType>) {
        typename FieldType::TrackingResult Result = Field.getTrackingResult();
        if (Result.Exact)
          Info.ExactVectors.push_back(Stack);

        using KeyType = std::remove_cv_t<typename FieldType::key_type>;
        for (KeyType Key : Result.InspectedKeys) {
          Stack.push_back(Key);
          Info.Read.push_back(Stack);
          Stack.pop_back();
        }

        using ValueType = typename FieldType::value_type;
        while (Next < Selected.size() and Elements[Selected[Next]].Field == I) {
          const void *Pointer = Elements[Selected[Next]].Element;
          const auto &Element = *static_cast<const ValueType *>(Pointer);
          auto &Mutable = const_cast<ValueType &>(Element);
          Stack.push_back(KeyedObjectTraits<ValueType>::key(Mutable));
          uint32_t ReadBegin = Info.Read.size();
          uint32_t ExactBegin = Info.ExactVectors.size();
          collectImpl<M>(Element, Stack, Info);
          if (Segments != nullptr) {
            Segments->push_back({ Selected[Next],
                                  ReadBegin,
                                  uint32_t(Info.Read.size()),
                                  ExactBegin,
                                  uint32_t(Info.ExactVectors.size()) });
          }
          Stack.pop_back();
          ++Next;
        }
      } else {
        collectImpl<M>(Field, Stack, Info);
      }

      Stack.pop_back();

      collectPartially<M, I + 1>(Root,
                                 Elements,
                                 Selected,
                                 Next,
                                 Stack,
                                 Info,
                                 Segments);
    }
  }
};

/// Per-object collection of the fields read, for a consumer that brackets the
/// production of each object with startObject and finishObject.
///
/// finishObject returns what the sequence
///
///     Tracking::push, <production of the object>, Tracking::collect,
///     Tracking::pop
///
/// returns, and leaves the trackers in the state that sequence leaves them in,
/// but visits only the root, its fields that are not containers, its
/// containers and those of their elements in which something was read, instead
/// of walking the whole tree five times. To know which elements those are, the
/// trackers log themselves when they get marked (see AccessLogs), and the
/// address of every tracker is mapped to its element once, when the first
/// object starts.
///
/// What is read while no object is being produced (for instance by the
/// constructor of a pipe) belongs to every object, as it does with the
/// sequence above: its paths are computed once and added to each result.
/// As with that sequence, the keys looked up in containers in that phase are
/// only reported for the first object that follows.
template<typename M>
class TouchedTracking {
private:
  using ElementID = TrackingImpl::ElementID;

private:
  const M &Root;
  AccessLogs Logs;
  AccessLogs *PreviousLogs = nullptr;

  /// Address of each tracker, sorted, with the element it is in
  std::vector<std::pair<const void *, ElementID>> Addresses;
  std::vector<TrackingImpl::ElementLocation> Elements;
  bool Indexed = false;

  /// Elements with something read while no object was being produced, sorted
  std::vector<ElementID> OutsideElements;

  /// Trackers marked while no object was being produced, sorted by element
  std::vector<std::pair<ElementID, AccessTracker *>> OutsideTrackers;

  /// What was read while no object was being produced: with and without the
  /// keys looked up in containers
  ReadFields OutsideWithKeys;
  ReadFields OutsideWithoutKeys;
  bool KeysPending = false;

public:
  /// Identifier of a set of paths (see the finishObject that returns groups)
  using GroupID = uint32_t;

private:
  /// Sets of paths, each stored once: what an object reads is the union of a
  /// few of them, most of which it shares with other objects
  std::vector<ReadFields> Groups;

  /// For each element, the groups made of paths in it. The same element read
  /// in the same way by another object gives the same paths: they are looked
  /// for here before a new group is made.
  std::unordered_map<ElementID, llvm::SmallVector<GroupID, 2>> GroupsOfElement;

  /// The groups of what was read while no object was being produced
  std::optional<GroupID> OutsideWithKeysGroup;
  std::optional<GroupID> OutsideWithoutKeysGroup;

  /// Above this number of different ways an element has been read in, no
  /// attempt is made to find one of them again
  static constexpr size_t MaxGroupsPerElement = 16;

public:
  TouchedTracking(const M &Root) : Root(Root) {
    PreviousLogs = ActiveAccessLogs;
    ActiveAccessLogs = &Logs;
  }

  ~TouchedTracking() { ActiveAccessLogs = PreviousLogs; }

  TouchedTracking(const TouchedTracking &) = delete;
  TouchedTracking &operator=(const TouchedTracking &) = delete;

public:
  void startObject() {
    if (not Indexed) {
      TrackingImpl::index<M>(Root, Addresses, Elements);
      std::sort(Addresses.begin(), Addresses.end());
      Indexed = true;
      refreshOutside();
    } else if (not Logs.Trackers.empty() or not Logs.KeyLookups.empty()) {
      refreshOutside();
    }
  }

  ReadFields finishObject() {
    ReadFields Info;
    collectObject(Info, nullptr);

    const ReadFields &Outside = KeysPending ? OutsideWithKeys :
                                              OutsideWithoutKeys;
    KeysPending = false;
    Info.Read.append(Outside.Read.begin(), Outside.Read.end());
    Info.ExactVectors.append(Outside.ExactVectors.begin(),
                             Outside.ExactVectors.end());
    Info.deduplicate();

    return Info;
  }

  /// Same as finishObject, but what was read comes as a list of groups (see
  /// groups): the paths are the union of the paths of the groups.
  ///
  /// The paths about one element form a group, and so do the paths that are
  /// not about any element and what was read while no object was being
  /// produced. A group that an earlier object already had is not built again:
  /// with many objects reading the same large part of the model, this is what
  /// keeps the total size from growing with the product of the two.
  ///
  /// \param Expected if not null, it receives what the other finishObject
  ///        would have returned, for who wants to check the groups against it
  void finishObject(std::vector<GroupID> &Result,
                    ReadFields *Expected = nullptr) {
    ReadFields Info;
    std::vector<TrackingImpl::Segment> Segments;
    collectObject(Info, &Segments);

    if (Expected != nullptr) {
      const ReadFields &Outside = KeysPending ? OutsideWithKeys :
                                                OutsideWithoutKeys;
      *Expected = Info;
      Expected->Read.append(Outside.Read.begin(), Outside.Read.end());
      Expected->ExactVectors.append(Outside.ExactVectors.begin(),
                                    Outside.ExactVectors.end());
      Expected->deduplicate();
    }

    using PathList = llvm::ArrayRef<TupleTreePath>;
    PathList Read(Info.Read);
    PathList Exact(Info.ExactVectors);

    ReadFields Rest;
    size_t ReadDone = 0;
    size_t ExactDone = 0;
    for (const TrackingImpl::Segment &S : Segments) {
      Rest.Read.append(Read.begin() + ReadDone, Read.begin() + S.ReadBegin);
      Rest.ExactVectors.append(Exact.begin() + ExactDone,
                               Exact.begin() + S.ExactBegin);
      ReadDone = S.ReadEnd;
      ExactDone = S.ExactEnd;

      addGroup(S.Element,
               Read.slice(S.ReadBegin, S.ReadEnd - S.ReadBegin),
               Exact.slice(S.ExactBegin, S.ExactEnd - S.ExactBegin),
               Result);
    }
    Rest.Read.append(Read.begin() + ReadDone, Read.end());
    Rest.ExactVectors.append(Exact.begin() + ExactDone, Exact.end());
    addGroup(TrackingImpl::RestOfTheTree, Rest.Read, Rest.ExactVectors, Result);

    bool WithKeys = KeysPending;
    KeysPending = false;
    const ReadFields &Outside = WithKeys ? OutsideWithKeys : OutsideWithoutKeys;
    std::optional<GroupID> &OutsideGroup = WithKeys ? OutsideWithKeysGroup :
                                                      OutsideWithoutKeysGroup;
    if (not Outside.Read.empty() or not Outside.ExactVectors.empty()) {
      // The same group as long as what was read outside does not change
      if (not OutsideGroup.has_value()
          or Groups[*OutsideGroup].Read != Outside.Read
          or Groups[*OutsideGroup].ExactVectors != Outside.ExactVectors) {
        OutsideGroup = Groups.size();
        Groups.push_back(Outside);
      }
      Result.push_back(*OutsideGroup);
    }
  }

  /// Add a group made of \p Fields, for paths collected in some other way
  GroupID addGroup(ReadFields &&Fields) {
    Groups.push_back(std::move(Fields));
    return Groups.size() - 1;
  }

  /// The groups: the identifiers finishObject returns are indices here
  const std::vector<ReadFields> &groups() const { return Groups; }

private:
  void addGroup(ElementID Element,
                llvm::ArrayRef<TupleTreePath> Read,
                llvm::ArrayRef<TupleTreePath> Exact,
                std::vector<GroupID> &Result) {
    if (Read.empty() and Exact.empty())
      return;

    auto &Known = GroupsOfElement[Element];
    for (GroupID ID : Known) {
      const ReadFields &Group = Groups[ID];
      if (llvm::ArrayRef<TupleTreePath>(Group.Read) == Read
          and llvm::ArrayRef<TupleTreePath>(Group.ExactVectors) == Exact) {
        Result.push_back(ID);
        return;
      }
    }

    GroupID New = Groups.size();
    Groups.emplace_back();
    Groups.back().Read.append(Read.begin(), Read.end());
    Groups.back().ExactVectors.append(Exact.begin(), Exact.end());
    if (Known.size() < MaxGroupsPerElement)
      Known.push_back(New);
    Result.push_back(New);
  }

  /// What finishObject does but for the part about what was read while no
  /// object was being produced: collect what was read in the object, clear it
  /// and resume tracking
  void collectObject(ReadFields &Info,
                     std::vector<TrackingImpl::Segment> *Segments) {
    // The elements in which something was read while producing this object
    std::vector<ElementID> Touched;
    for (AccessTracker *Tracker : Logs.Trackers)
      addElementOf(Tracker, Touched);
    for (const AccessTracker *Identity : Logs.KeyLookups)
      addElementOf(Identity, Touched);
    Logs.Trackers.clear();
    Logs.KeyLookups.clear();
    sortAndDeduplicate(Touched);

    // Stop, collect, clear and resume: what Tracking::collect does, on the
    // part of the tree where it can find something
    visit<TrackingImpl::StopTrackingVisitor>(Touched);

    collect(Touched, Info, Segments);

    visit<TrackingImpl::ClearVisitor>(Touched);

    // Clearing also removed the marks made while no object was being produced:
    // put them back
    auto It = OutsideTrackers.begin();
    for (ElementID ID : Touched) {
      It = std::lower_bound(It,
                            OutsideTrackers.end(),
                            std::pair<ElementID, AccessTracker *>(ID, nullptr));
      for (; It != OutsideTrackers.end() and It->first == ID; ++It)
        It->second->restoreMark();
    }
    auto RestOfTheTree = TrackingImpl::RestOfTheTree;
    It = std::lower_bound(OutsideTrackers.begin(),
                          OutsideTrackers.end(),
                          std::pair<ElementID, AccessTracker *>(RestOfTheTree,
                                                                nullptr));
    for (; It != OutsideTrackers.end(); ++It)
      It->second->restoreMark();

    // The trackers that logged themselves during the visits above are not
    // marked any longer
    Logs.Trackers.clear();
    Logs.KeyLookups.clear();
  }

private:
  static void sortAndDeduplicate(std::vector<ElementID> &IDs) {
    std::sort(IDs.begin(), IDs.end());
    IDs.erase(std::unique(IDs.begin(), IDs.end()), IDs.end());
  }

  /// \return the element \p Address is in, RestOfTheTree if it is in the tree
  ///         but not in an element, nothing if it is not in the tree (as the
  ///         trackers of a copy of a part of the tree)
  std::optional<ElementID> elementOf(const void *Address) const {
    auto It = std::lower_bound(Addresses.begin(),
                               Addresses.end(),
                               std::pair<const void *, ElementID>(Address, 0));
    if (It == Addresses.end() or It->first != Address)
      return std::nullopt;
    return It->second;
  }

  void addElementOf(const void *Address, std::vector<ElementID> &IDs) const {
    std::optional<ElementID> ID = elementOf(Address);
    if (ID.has_value() and *ID != TrackingImpl::RestOfTheTree)
      IDs.push_back(*ID);
  }

  template<typename Visitor>
  void visit(const std::vector<ElementID> &Selected) const {
    size_t Next = 0;
    TrackingImpl::visitPartially<M, Visitor>(Root, Elements, Selected, Next);
    revng_assert(Next == Selected.size());
  }

  void collect(const std::vector<ElementID> &Selected,
               ReadFields &Info,
               std::vector<TrackingImpl::Segment> *Segments = nullptr) const {
    size_t Next = 0;
    TupleTreePath Stack;
    TrackingImpl::collectPartially<M>(Root,
                                      Elements,
                                      Selected,
                                      Next,
                                      Stack,
                                      Info,
                                      Segments);
    revng_assert(Next == Selected.size());
  }

  /// Account for what was read since the last object was finished
  void refreshOutside() {
    for (AccessTracker *Tracker : Logs.Trackers) {
      std::optional<ElementID> ID = elementOf(Tracker);
      if (ID.has_value()) {
        OutsideTrackers.emplace_back(*ID, Tracker);
        if (*ID != TrackingImpl::RestOfTheTree)
          OutsideElements.push_back(*ID);
      }
    }
    for (const AccessTracker *Identity : Logs.KeyLookups)
      addElementOf(Identity, OutsideElements);
    Logs.Trackers.clear();
    Logs.KeyLookups.clear();

    std::sort(OutsideTrackers.begin(), OutsideTrackers.end());
    OutsideTrackers.erase(std::unique(OutsideTrackers.begin(),
                                      OutsideTrackers.end()),
                          OutsideTrackers.end());
    sortAndDeduplicate(OutsideElements);

    visit<TrackingImpl::StopTrackingVisitor>(OutsideElements);

    OutsideWithKeys = {};
    collect(OutsideElements, OutsideWithKeys);

    // The keys looked up so far are reported once, then forgotten
    visit<TrackingImpl::DropInspectedKeysVisitor>(OutsideElements);

    OutsideWithoutKeys = {};
    collect(OutsideElements, OutsideWithoutKeys);

    visit<TrackingImpl::ResumeTrackingVisitor>(OutsideElements);

    KeysPending = true;

    Logs.Trackers.clear();
    Logs.KeyLookups.clear();
  }
};

template<typename M>
ReadFields Tracking::collect(const M &LHS) {
  stop(LHS);
  TupleTreePath Stack;
  ReadFields Info;
  TrackingImpl::collectImpl<M>(LHS, Stack, Info);
  clearAndResume(LHS);
  Info.deduplicate();
  return Info;
}

template<typename M>
void Tracking::clearAndResume(const M &LHS) {
  TrackingImpl::visitTuple<M, TrackingImpl::ClearVisitor>(LHS);
}

template<typename M>
void Tracking::push(const M &LHS) {
  TrackingImpl::visitTuple<M, TrackingImpl::PushVisitor>(LHS);
}

template<typename M>
void Tracking::pop(const M &LHS) {
  TrackingImpl::visitTuple<M, TrackingImpl::PopVisitor>(LHS);
}

template<typename M>
void Tracking::stop(const M &LHS) {
  TrackingImpl::visitTuple<M, TrackingImpl::StopTrackingVisitor>(LHS);
}

} // namespace revng
