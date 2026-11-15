#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/ADT/STLExtras.h"
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

  /// Collect whether \p LHS was read as a whole and the keys looked up in it
  template<revng::SetOrKOC T>
  static void
  collectContainer(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    typename T::TrackingResult TrackingResult = LHS.getTrackingResult();
    if (TrackingResult.Exact)
      Info.ExactVectors.push_back(Stack);

    using KeyType = std::remove_cv_t<typename T::key_type>;
    for (KeyType Key : TrackingResult.InspectedKeys) {
      Stack.push_back(Key);
      Info.Read.push_back(Stack);
      Stack.pop_back();
    }
  }

  /// Collect what was read in \p LHSElement, an element of a `T`
  template<typename M, revng::SetOrKOC T>
  static void collectElement(const typename T::value_type &LHSElement,
                             TupleTreePath &Stack,
                             ReadFields &Info) {
    using value_type = typename T::value_type;
    auto &Mutable = const_cast<value_type &>(LHSElement);
    Stack.push_back(KeyedObjectTraits<value_type>::key(Mutable));

    collectImpl<M>(LHSElement, Stack, Info);
    Stack.pop_back();
  }

  template<typename M, revng::SetOrKOC T>
  static void
  collectImpl(const T &LHS, TupleTreePath &Stack, ReadFields &Info) {
    collectContainer(LHS, Stack, Info);
    forEachSelected(LHS, [&](auto &LHSElement) {
      collectElement<M, T>(LHSElement, Stack, Info);
    });
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
    forEachSelected(LHS, [](auto &LHSElement) {
      visitImpl<M, Visitor>(LHSElement);
    });
    Visitor::template visitKeyedObjectContainer<T>(LHS);
  }

  template<typename M, typename Visitor, NotTupleTreeCompatible T>
  static void visitImpl(const T &LHS) {}

  //
  // Visits of a part of the tree
  //

  /// The elements to visit of each container, which is identified by its
  /// `Exact` tracker
  using Selection = std::unordered_map<const AccessTracker *,
                                       std::vector<const void *>>;

  /// When not null, the visits and the collection only enter, of each
  /// container, the elements listed here for it
  static inline const Selection *Selected = nullptr;

  template<revng::SetOrKOC T, typename CallableType>
  static void forEachSelected(const T &Container, CallableType &&Callable) {
    if (Selected == nullptr) {
      for (auto &Element : Container.Content)
        Callable(Element);
    } else if (auto It = Selected->find(&Container.Exact);
               It != Selected->end()) {
      for (const void *Element : It->second)
        Callable(*static_cast<const typename T::value_type *>(Element));
    }
  }

  struct DropInspectedKeysVisitor {
    template<revng::SetOrKOC Type>
    static void visitKeyedObjectContainer(const Type &CurrentItem) {
      CurrentItem.dropInspectedKeys();
    }

    template<typename Type, size_t FieldIndex>
    static void visitTupleElement(const Type &) {}
  };

  /// Index of an element in `Index::Elements`
  using ElementIndex = uint32_t;

  /// Stands for the part of the tree that is not in any element
  static constexpr ElementIndex NoElement = ~ElementIndex(0);

  /// The elements of the containers of a tree and where its objects are
  struct Index {
    struct Element {
      /// The element the container is in
      ElementIndex Parent;

      /// The container, identified by its `Exact` tracker
      const AccessTracker *Container;

      const void *Pointer;
    };

    /// An object with its own storage: the root, an element of a container
    /// or what an `UpcastablePointer` points to. The trackers of its fields
    /// and of the containers among them are within it.
    struct Object {
      const char *Begin;
      const char *End;
      ElementIndex Element;
    };

    /// Each element comes after the one its container is in, and the
    /// elements of a container are in its order
    std::vector<Element> Elements;

    /// Sorted by address
    std::vector<Object> Objects;

    /// \return the element the object holding \p Address is in, if any
    ElementIndex elementOf(const void *Address) const {
      auto StartsAfter = [](const void *Address, const Object &Candidate) {
        return std::less<const void *>()(Address, Candidate.Begin);
      };
      auto It = std::upper_bound(Objects.begin(),
                                 Objects.end(),
                                 Address,
                                 StartsAfter);
      if (It == Objects.begin()
          or not std::less<const void *>()(Address, std::prev(It)->End))
        return NoElement;

      return std::prev(It)->Element;
    }
  };

  template<typename M, typename T>
  static void
  indexObject(const T &Object, ElementIndex Current, Index &Result) {
    if constexpr (TupleSizeCompatible<T>) {
      const char *Begin = reinterpret_cast<const char *>(&Object);
      Result.Objects.push_back({ Begin, Begin + sizeof(T), Current });
    }

    indexImpl<M>(Object, Current, Result);
  }

  template<typename M, StrictSpecializationOf<UpcastablePointer> T>
  static void indexImpl(const T &UP, ElementIndex Current, Index &Result) {
    if (!UP.isEmpty()) {
      auto KindTrackingSuspender = UP.get()->KindTracker.suspend();
      UP.upcast([&](auto &Upcasted) {
        indexObject<M>(Upcasted, Current, Result);
      });
    }
  }

  template<typename M, TupleSizeCompatible T>
  static void indexImpl(const T &LHS, ElementIndex Current, Index &Result) {
    auto IndexFields = [&]<size_t... Is>(std::index_sequence<Is...>) {
      (indexImpl<M>(LHS.template untrackedGet<Is>(), Current, Result), ...);
    };
    IndexFields(std::make_index_sequence<std::tuple_size_v<T>>{});
  }

  template<typename M, revng::SetOrKOC T>
  static void indexImpl(const T &LHS, ElementIndex Current, Index &Result) {
    for (auto &LHSElement : LHS.Content) {
      Result.Elements.push_back({ Current, &LHS.Exact, &LHSElement });
      indexObject<M>(LHSElement, Result.Elements.size() - 1, Result);
    }
  }

  template<typename M, NotTupleTreeCompatible T>
  static void indexImpl(const T &, ElementIndex, Index &) {}

  /// What collectImpl does on \p Root, keeping apart what was read in each
  /// element of the containers that are its fields
  ///
  /// \param InElements each of those elements, with what was read in it
  /// \param Elsewhere what was read in the rest of the tree
  template<typename M, size_t I = 0>
  static void
  collectByElement(const M &Root,
                   std::vector<std::pair<const void *, ReadFields>> &InElements,
                   ReadFields &Elsewhere,
                   TupleTreePath &Stack) {
    if constexpr (I < std::tuple_size_v<M>) {
      Stack.push_back(size_t(I));
      if (Root.template getTracker<I>().isSet())
        Elsewhere.Read.push_back(Stack);

      const auto &Field = Root.template untrackedGet<I>();
      using FieldType = std::remove_cvref_t<decltype(Field)>;
      if constexpr (revng::SetOrKOC<FieldType>) {
        collectContainer(Field, Stack, Elsewhere);
        forEachSelected(Field, [&](auto &Element) {
          ReadFields
            &InElement = InElements.emplace_back(&Element, ReadFields()).second;
          collectElement<M, FieldType>(Element, Stack, InElement);
        });
      } else {
        collectImpl<M>(Field, Stack, Elsewhere);
      }

      Stack.pop_back();

      collectByElement<M, I + 1>(Root, InElements, Elsewhere, Stack);
    }
  }
};

/// Collects what is read in a tree while each of a sequence of objects is
/// being produced.
///
/// `finishObject` returns what
///
///     Tracking::push, <production of the object>, Tracking::collect,
///     Tracking::pop
///
/// returns, but where each of those visits the whole tree, it only enters the
/// elements of the containers in which something was read: the trackers that
/// get marked are logged (see `AccessLog`), and the address of a tracker tells
/// the element it is in. The tree must not change while this object exists.
///
/// What is read while no object is being produced, for instance by the
/// constructor of a pipe, belongs to every object that follows, as it does
/// with the sequence above.
///
/// What was read comes as a list of groups of paths, see `groups`, whose
/// union it is: the paths in one element of a container of the root are a
/// group, and so are those in the rest of the tree and those read while no
/// object was being produced. A group an earlier object already had is not
/// made again, so many objects reading the same large part of the tree do not
/// take a size that is the product of the two.
template<typename M>
class ObjectTracker {
public:
  /// Index of a group in `groups`
  using Group = uint32_t;

private:
  /// Above this number of different ways an element has been read in, no
  /// attempt is made to find one of them again
  static constexpr size_t MaxGroupsPerElement = 16;

private:
  const M &Root;
  std::vector<const AccessTracker *> Log;
  std::vector<const AccessTracker *> *PreviousLog;

  /// Filled when the first object starts
  std::optional<TrackingImpl::Index> Index;

  std::vector<ReadFields> Groups;

  /// For each element of a container of the root, the groups of paths in it.
  /// Null stands for the rest of the tree.
  std::unordered_map<const void *, llvm::SmallVector<Group, 2>> GroupsOf;

  /// What was read while no object was being produced, and its group
  ReadFields Outside;
  std::optional<Group> OutsideGroup;

  /// The same, with the keys looked up in containers, for the first object
  /// that follows: `Tracking::collect` forgets those keys in all the layers,
  /// so they are reported once
  std::optional<Group> OutsideOnceGroup;

public:
  ObjectTracker(const M &Root) : Root(Root), PreviousLog(AccessLog) {
    AccessLog = &Log;
  }

  ~ObjectTracker() { AccessLog = PreviousLog; }

  ObjectTracker(const ObjectTracker &) = delete;
  ObjectTracker &operator=(const ObjectTracker &) = delete;

public:
  void startObject() {
    if (not Index.has_value()) {
      Index.emplace();
      TrackingImpl::indexObject<M>(Root, TrackingImpl::NoElement, *Index);
      llvm::sort(Index->Objects, [](const auto &LHS, const auto &RHS) {
        return std::less<const void *>()(LHS.Begin, RHS.Begin);
      });
    }

    if (Log.empty())
      return;

    // Something was read since the last object
    TrackingImpl::Selection Touched = takeTouchedElements();
    TrackingImpl::Selected = &Touched;
    Tracking::stop(Root);
    OutsideOnceGroup = addGroup(collectTouched());
    using DropKeys = TrackingImpl::DropInspectedKeysVisitor;
    TrackingImpl::visitTuple<M, DropKeys>(Root);
    Outside.append(collectTouched());
    Outside.deduplicate();
    OutsideGroup = addGroup(ReadFields(Outside));
    Tracking::clearAndResume(Root);
    TrackingImpl::Selected = nullptr;
  }

  std::vector<Group> finishObject() {
    TrackingImpl::Selection Touched = takeTouchedElements();
    TrackingImpl::Selected = &Touched;
    Tracking::stop(Root);
    std::vector<std::pair<const void *, ReadFields>> InElements;
    ReadFields Elsewhere;
    TupleTreePath Stack;
    TrackingImpl::collectByElement(Root, InElements, Elsewhere, Stack);
    Tracking::clearAndResume(Root);
    TrackingImpl::Selected = nullptr;

    std::vector<Group> Result;
    for (auto &[Element, Fields] : InElements)
      addGroupOf(Element, std::move(Fields), Result);
    addGroupOf(nullptr, std::move(Elsewhere), Result);
    addOutsideGroups(Result);
    OutsideOnceGroup.reset();

    return Result;
  }

  /// \return what was read since the last object, or since the beginning.
  ///         As after `Tracking::collect`, nothing is left marked.
  std::vector<Group> collect() {
    std::vector<Group> Result;
    if (std::optional<Group> New = addGroup(Tracking::collect(Root)))
      Result.push_back(*New);
    Log.clear();

    addOutsideGroups(Result);
    Outside = {};
    OutsideGroup.reset();
    OutsideOnceGroup.reset();

    return Result;
  }

  /// The groups of paths `finishObject` and `collect` refer to
  const std::vector<ReadFields> &groups() const { return Groups; }

private:
  /// \return the new group made of \p Fields, unless it is empty
  std::optional<Group> addGroup(ReadFields &&Fields) {
    if (Fields.empty())
      return std::nullopt;

    Groups.push_back(std::move(Fields));
    return Groups.size() - 1;
  }

  /// Add to \p Result the group made of \p Fields, which is what was read in
  /// \p Element
  void addGroupOf(const void *Element,
                  ReadFields &&Fields,
                  std::vector<Group> &Result) {
    if (Fields.empty())
      return;

    // The same element read in the same way gives the same paths
    auto &Known = GroupsOf[Element];
    for (Group Candidate : Known) {
      if (Groups[Candidate] == Fields) {
        Result.push_back(Candidate);
        return;
      }
    }

    Group New = *addGroup(std::move(Fields));
    Result.push_back(New);
    if (Known.size() < MaxGroupsPerElement)
      Known.push_back(New);
  }

  void addOutsideGroups(std::vector<Group> &Result) const {
    if (OutsideGroup.has_value())
      Result.push_back(*OutsideGroup);
    if (OutsideOnceGroup.has_value())
      Result.push_back(*OutsideOnceGroup);
  }

  /// \return the elements holding the trackers in the log, which is emptied,
  ///         and the elements those are in. A tracker that is not in the tree
  ///         is in a copy of a part of it.
  TrackingImpl::Selection takeTouchedElements() {
    using ElementIndex = TrackingImpl::ElementIndex;
    std::vector<ElementIndex> Touched;
    for (const AccessTracker *Tracker : Log) {
      ElementIndex Element = Index->elementOf(Tracker);
      while (Element != TrackingImpl::NoElement) {
        Touched.push_back(Element);
        Element = Index->Elements[Element].Parent;
      }
    }
    Log.clear();

    llvm::sort(Touched);
    Touched.erase(std::unique(Touched.begin(), Touched.end()), Touched.end());

    TrackingImpl::Selection Result;
    for (ElementIndex Element : Touched) {
      const auto &[_, Container, Pointer] = Index->Elements[Element];
      Result[Container].push_back(Pointer);
    }

    return Result;
  }

  /// \return what was read in the elements being visited and in the rest of
  ///         the tree
  ReadFields collectTouched() const {
    ReadFields Result;
    TupleTreePath Stack;
    TrackingImpl::collectImpl<M>(Root, Stack, Result);
    return Result;
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
