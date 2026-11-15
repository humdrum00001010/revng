#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>

#include "revng/PipeboxCommon/Concepts.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/TupleTree/TrackingImpl.h"

//
// ObjectDependenciesHelper
//

/// Helper class that handles most of the heavy lifting around generating a
/// `ObjectDependencies` object. This is model-aware and uses the model tracking
/// functionality to automatically populate the `ObjectDependencies` object.
class ObjectDependenciesHelper {
private:
  using Group = revng::ObjectTracker<model::Binary>::Group;

private:
  const model::Binary &Binary;
  const revng::pypeline::Request &Outgoing;
  revng::ObjectTracker<model::Binary> Tracker;

  /// For each container, the objects committed, each with the groups of paths
  /// it read (see `ObjectTracker`)
  std::vector<std::vector<std::pair<ObjectID, std::vector<Group>>>> Objects;
  bool DependenciesTaken = false;

  /// Whether what is being read is for an object, see `pushReadFields`
  bool InObject = false;

public:
  ObjectDependenciesHelper(const Model &Model,
                           const revng::pypeline::Request &Outgoing,
                           size_t DependenciesSize) :
    Binary(*Model.get().get()),
    Outgoing(Outgoing),
    Tracker(Binary),
    Objects(DependenciesSize) {
    revng::Tracking::clearAndResume(Binary);
  }

  ~ObjectDependenciesHelper() {
    revng_assert(DependenciesTaken);
    revng::Tracking::stop(Binary);
  }

  ObjectDependenciesHelper(const ObjectDependenciesHelper &) = delete;
  ObjectDependenciesHelper &
  operator=(const ObjectDependenciesHelper &) = delete;
  ObjectDependenciesHelper(ObjectDependenciesHelper &&) = delete;
  ObjectDependenciesHelper &operator=(ObjectDependenciesHelper &&) = delete;

  /// Given the ArgumentIndex-th container, add all the read paths obtained by
  /// tracking to the specified ObjectID
  void commit(const ObjectID &Object, size_t ArgumentIndex) {
    revng_assert(ArgumentIndex < Objects.size());
    Objects[ArgumentIndex].push_back({ Object, getReadGroups() });
  }

  /// Given the ArgumentIndex-th container, add all the read paths obtained by
  /// tracking to all the requested Outgoing objects
  void commitAllFor(size_t ArgumentIndex) {
    revng_assert(ArgumentIndex < Objects.size());
    std::vector<Group> ReadGroups = getReadGroups();
    for (const ObjectID *Object : Outgoing[ArgumentIndex])
      Objects[ArgumentIndex].push_back({ *Object, ReadGroups });
  }

  /// In the special-case that a single ObjectID is requested, add all the read
  /// paths to it as a dependency
  void commitUniqueTarget(size_t ArgumentIndex) {
    revng_assert(ArgumentIndex < Objects.size());
    revng_assert(Outgoing[ArgumentIndex].size() == 1);
    commitAllFor(ArgumentIndex);
  }

  /// Add a "layer" to the tracking, the paths read by the `commit*` functions
  /// will only be present until the call to `popReadFields`
  void pushReadFields() {
    revng_assert(not InObject);
    InObject = true;
    Tracker.startObject();
  }

  /// Remove a "layer" to the tracking, paths read since the last
  /// `pushReadFields` will no longer be present when `commit*` is called
  void popReadFields() {
    revng_assert(InObject);
    InObject = false;
  }

  /// Retrieve the ObjectDependencies. This needs to be called exactly once,
  /// otherwise an assertion will be hit.
  ///
  /// The groups of paths that are read by exactly the same objects are merged
  /// into a class, since no change to the model can tell them apart.
  [[nodiscard]] revng::pypeline::ObjectDependencies takeDependencies() {
    revng_assert(not DependenciesTaken);
    DependenciesTaken = true;

    revng::pypeline::ObjectDependencies Result;
    const std::vector<ReadFields> &Groups = Tracker.groups();

    // For each group, the objects that read it, numbered in the order they
    // were committed
    std::vector<std::vector<uint32_t>> Readers(Groups.size());
    uint32_t ObjectIndex = 0;
    for (const auto &Committed : Objects) {
      for (const auto &[Object, ReadGroups] : Committed) {
        for (Group ReadGroup : ReadGroups)
          Readers[ReadGroup].push_back(ObjectIndex);
        ++ObjectIndex;
      }
    }

    // A class for each distinct set of readers, with the paths of its groups
    std::map<std::vector<uint32_t>, uint32_t> ClassOfReaders;
    std::vector<uint32_t> ClassOfGroup(Groups.size());
    std::map<std::string, uint32_t> PathIndices;
    for (Group I = 0; I < Groups.size(); ++I) {
      if (Readers[I].empty())
        continue;

      auto [It, New] = ClassOfReaders.try_emplace(std::move(Readers[I]),
                                                  Result.Classes.size());
      if (New)
        Result.Classes.emplace_back();
      ClassOfGroup[I] = It->second;

      auto AddPath = [&](const TupleTreePath &Path) {
        std::string Text = *pathAsString<model::Binary>(Path);
        auto [Index, New] = PathIndices.try_emplace(Text, Result.Paths.size());
        if (New)
          Result.Paths.push_back(std::move(Text));
        Result.Classes[ClassOfGroup[I]].push_back(Index->second);
      };
      llvm::for_each(Groups[I].Read, AddPath);
      llvm::for_each(Groups[I].ExactVectors, AddPath);
    }

    for (std::vector<uint32_t> &Class : Result.Classes) {
      llvm::sort(Class);
      Class.erase(std::unique(Class.begin(), Class.end()), Class.end());
    }

    // The classes of each object
    for (auto &Committed : Objects) {
      auto &ObjectsOfContainer = Result.Objects.emplace_back();
      for (auto &[Object, ReadGroups] : Committed) {
        std::vector<uint32_t> Classes;
        for (Group ReadGroup : ReadGroups)
          Classes.push_back(ClassOfGroup[ReadGroup]);
        llvm::sort(Classes);
        Classes.erase(std::unique(Classes.begin(), Classes.end()),
                      Classes.end());
        ObjectsOfContainer.emplace_back(std::move(Object), std::move(Classes));
      }
    }

    return Result;
  }

private:
  /// RAII object that automatically does `pushReadFields` on construction and
  /// `commit`, `popReadFields` on destruction.
  class Committer {
  private:
    ObjectDependenciesHelper &ODH;
    const ObjectID &Object;
    size_t ArgumentIndex;

  public:
    Committer(ObjectDependenciesHelper &ODH,
              const ObjectID &Object,
              size_t ArgumentIndex) :
      ODH(ODH), Object(Object), ArgumentIndex(ArgumentIndex) {
      ODH.pushReadFields();
    }

    ~Committer() {
      ODH.commit(Object, ArgumentIndex);
      ODH.popReadFields();
    }

    Committer(const Committer &) = delete;
    Committer &operator=(const Committer &) = delete;
    Committer(Committer &&) = delete;
    Committer &operator=(Committer &&) = delete;
  };

public:
  [[nodiscard]] Committer getCommitterFor(const ObjectID &Object,
                                          size_t ArgumentIndex) {
    return Committer(*this, Object, ArgumentIndex);
  }

private:
  std::vector<Group> getReadGroups() {
    return InObject ? Tracker.finishObject() : Tracker.collect();
  }
};

//
// Define the two concepts IsSingleObjectPipeRun and IsMultipleObjectsPipeRun
// which are the two categories of Pipe Runs
//

namespace detail {

template<typename T>
concept IsPipeRunArgument = requires {
  requires IsPipeArgument<T>;
  requires IsContainerArgument<typename T::Type>;
};

template<SpecializationOf<TypeList> List>
inline constexpr bool checkPipeRunArguments() {
  constexpr bool Size = std::tuple_size_v<List>;
  return compile_time::repeatAnd<Size>([]<size_t I>() {
    return IsPipeRunArgument<std::tuple_element_t<I, List>>;
  });
}

template<typename T>
concept HasPipeRunArguments = requires {
  requires StrictSpecializationOf<typename T::Arguments, TypeList>;
  requires checkPipeRunArguments<typename T::Arguments>();
};

} // namespace detail

//
// PipeRunContainerTypes
//

namespace detail {

template<detail::HasPipeRunArguments T>
struct PipeRunContainerTypesImpl {
private:
  template<typename... Args>
  static TypeList<typename Args::Type...> toContainerTypes(TypeList<Args...>);

public:
  using ContainerTypes = decltype(toContainerTypes(std::declval<
                                                   typename T::Arguments>()));
};

} // namespace detail

/// This using will return a TypeList of container types (including const) used
/// by a Pipe Run class.
template<typename T>
using PipeRunContainerTypes = detail::PipeRunContainerTypesImpl<
  T>::ContainerTypes;

//
// Concepts and helpers used around hasConstructor
//

namespace detail {

template<typename T, typename TL, size_t... I>
concept HasConstructor = requires {
  { T{ std::declval<std::tuple_element_t<I, TL>>()... } } -> std::same_as<T>;
};

template<typename... Args>
TypeList<Args &...> addReference(TypeList<Args...>);

template<typename T>
using AddReference = decltype(addReference(std::declval<T>()));

} // namespace detail

/// This bool will be true if T can be constructed with values specified in the
/// TypeList TL. The main use is in concept to assert the shape of the
/// constructor.
template<typename T, SpecializationOf<TypeList> TL>
constexpr bool hasConstructor() {
  return compile_time::callWithIndexSequence<TL>([]<size_t... I>() {
    return detail::HasConstructor<T, TL, I...>;
  });
};

/// Helper using that will return a TypeList of ContainerTypes with `&` added
template<typename T>
using ConstructorContainerArguments = detail::AddReference<
  PipeRunContainerTypes<T>>;

template<typename T>
concept IsBasePipeRun = requires {
  requires HasName<T>;
  requires detail::HasPipeRunArguments<T>;
  requires hasConstructor<
    T,
    // The Structure of the constructor is:
    // {Model, StaticConfiguration, Configuration, Containers...}
    concat<TypeList<const Model &, llvm::StringRef, llvm::StringRef>,
           ConstructorContainerArguments<T>>>();
};
