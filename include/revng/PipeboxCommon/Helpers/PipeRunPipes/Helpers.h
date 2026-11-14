#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "revng/PipeboxCommon/Concepts.h"
#include "revng/PipeboxCommon/Model.h"
#include "revng/Support/Debug.h"
#include "revng/Support/PipeFixes.h"
#include "revng/TupleTree/TrackingImpl.h"

//
// ObjectDependenciesHelper
//

/// Helper class that handles most of the heavy lifting around generating a
/// `ObjectDependencies` object. This is model-aware and uses the model tracking
/// functionality to automatically populate the `ObjectDependencies` object.
class ObjectDependenciesHelper {
private:
  const model::Binary &Binary;
  const revng::pypeline::Request &Outgoing;
  revng::pypeline::ObjectDependencies Dependencies;
  bool DependenciesTaken = false;

  /// When engaged, the fields read by each object are collected by visiting
  /// only what was read (see TouchedTracking) instead of the whole model
  std::optional<revng::TouchedTracking<model::Binary>> Touched;

  /// Whether an object is being produced, i.e., a Committer is alive
  bool InObject = false;

  /// An object and the groups of paths (see TouchedTracking) it read
  struct GroupedEntry {
    ObjectID Object;
    std::vector<uint32_t> Groups;
  };

  /// When the dependencies are handed over as classes (see
  /// revng::pypeline::DependencyClasses), what each object read is kept here,
  /// per container, instead of in Dependencies
  std::vector<std::vector<GroupedEntry>> Grouped;
  bool UseClasses = false;

  /// With REVNG_CHECK_DEPENDENCY_CLASSES in the environment, the paths each
  /// object would have been reported with as pairs, in the order of Grouped:
  /// the classes are checked against them before they are handed over
  bool CheckClasses = false;
  std::vector<std::vector<std::vector<std::string>>> ExpectedPaths;

public:
  ObjectDependenciesHelper(const Model &Model,
                           const revng::pypeline::Request &Outgoing,
                           size_t DependenciesSize) :
    Binary(*Model.get().get()),
    Outgoing(Outgoing),
    Dependencies(DependenciesSize) {
    revng::Tracking::clearAndResume(Binary);
    if (pipefix::enabled(pipefix::TouchedTracking))
      Touched.emplace(Binary);

    UseClasses = Touched.has_value() and pipefix::DependencyClassesWanted
                 and pipefix::enabled(pipefix::DependencyClasses);
    if (UseClasses) {
      Grouped.resize(DependenciesSize);
      CheckClasses = std::getenv("REVNG_CHECK_DEPENDENCY_CLASSES") != nullptr;
      if (CheckClasses)
        ExpectedPaths.resize(DependenciesSize);
    }
  }

  ~ObjectDependenciesHelper() {
    revng_assert(DependenciesTaken);
    Touched.reset();
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
    revng_assert(ArgumentIndex < Dependencies.size());
    if (UseClasses) {
      std::vector<std::string> Expected;
      Grouped[ArgumentIndex].push_back({ Object, getReadGroups(&Expected) });
      if (CheckClasses)
        ExpectedPaths[ArgumentIndex].push_back(std::move(Expected));
      return;
    }

    std::vector<std::string> ReadPaths = getReadPaths();
    for (std::string &Path : ReadPaths)
      Dependencies[ArgumentIndex].push_back({ Object, Path });
  }

  /// Given the ArgumentIndex-th container, add all the read paths obtained by
  /// tracking to all the requested Outgoing objects
  void commitAllFor(size_t ArgumentIndex) {
    revng_assert(ArgumentIndex < Dependencies.size());
    if (UseClasses) {
      std::vector<std::string> Expected;
      std::vector<uint32_t> Groups = getReadGroups(&Expected);
      for (const ObjectID *Object : Outgoing[ArgumentIndex]) {
        Grouped[ArgumentIndex].push_back({ *Object, Groups });
        if (CheckClasses)
          ExpectedPaths[ArgumentIndex].push_back(Expected);
      }
      return;
    }

    std::vector<std::string> ReadPaths = getReadPaths();
    for (const ObjectID *Object : Outgoing[ArgumentIndex])
      for (std::string &Path : ReadPaths)
        Dependencies[ArgumentIndex].push_back({ *Object, Path });
  }

  /// In the special-case that a single ObjectID is requested, add all the read
  /// paths to it as a dependency
  void commitUniqueTarget(size_t ArgumentIndex) {
    revng_assert(ArgumentIndex < Dependencies.size());
    revng_assert(Outgoing[ArgumentIndex].size() == 1);
    commitAllFor(ArgumentIndex);
  }

  /// Add a "layer" to the tracking, the paths read by the `commit*` functions
  /// will only be present until the call to `popReadFields`
  void pushReadFields() {
    revng_assert(not InObject);
    InObject = true;
    if (Touched.has_value())
      Touched->startObject();
    else
      revng::Tracking::push(Binary);
  }

  /// Remove a "layer" to the tracking, paths read since the last
  /// `pushReadFields` will no longer be present when `commit*` is called
  void popReadFields() {
    revng_assert(InObject);
    InObject = false;
    if (not Touched.has_value())
      revng::Tracking::pop(Binary);
  }

  /// Retrieve the ObjectDependencies. This needs to be called exactly once,
  /// otherwise an assertion will be hit.
  [[nodiscard]] revng::pypeline::ObjectDependencies &&takeDependencies() {
    revng_assert(not DependenciesTaken);
    DependenciesTaken = true;
    if (UseClasses)
      Dependencies.Classes = makeClasses();
    return std::move(Dependencies);
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
  /// What was read, as groups of paths
  ///
  /// \param Expected when the classes are to be checked, it receives the
  ///        paths getReadPaths would have returned
  std::vector<uint32_t> getReadGroups(std::vector<std::string> *Expected) {
    std::vector<uint32_t> Result;
    ReadFields ExpectedFields;
    if (InObject) {
      Touched->finishObject(Result, CheckClasses ? &ExpectedFields : nullptr);
    } else {
      ReadFields Fields = revng::Tracking::collect(Binary);
      if (CheckClasses)
        ExpectedFields = Fields;
      if (not Fields.Read.empty() or not Fields.ExactVectors.empty())
        Result.push_back(Touched->addGroup(std::move(Fields)));
    }

    if (CheckClasses) {
      for (const TupleTreePath &Path : ExpectedFields.Read)
        Expected->push_back(*pathAsString<model::Binary>(Path));
      for (const TupleTreePath &Path : ExpectedFields.ExactVectors)
        Expected->push_back(*pathAsString<model::Binary>(Path));
    }

    std::sort(Result.begin(), Result.end());
    Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
    return Result;
  }

  /// Turn the groups each object read into classes: the groups that are read
  /// by exactly the same objects are merged, since no change to the model can
  /// tell them apart. Each object is left with a handful of classes, and each
  /// path is turned into text once.
  std::shared_ptr<revng::pypeline::DependencyClasses> makeClasses() {
    using revng::pypeline::DependencyClasses;
    auto Result = std::make_shared<DependencyClasses>();
    const std::vector<ReadFields> &Groups = Touched->groups();

    // For each group, the objects that read it. Objects are numbered across
    // the containers, in the order they were committed.
    std::vector<std::vector<uint32_t>> Readers(Groups.size());
    uint32_t EntryIndex = 0;
    for (const std::vector<GroupedEntry> &Entries : Grouped) {
      for (const GroupedEntry &Entry : Entries) {
        for (uint32_t Group : Entry.Groups)
          Readers[Group].push_back(EntryIndex);
        ++EntryIndex;
      }
    }

    // One class per distinct set of readers
    constexpr uint32_t NoClass = ~uint32_t(0);
    std::vector<uint32_t> ClassOfGroup(Groups.size(), NoClass);
    std::map<std::vector<uint32_t>, uint32_t> ClassOfReaders;
    for (uint32_t Group = 0; Group < Groups.size(); ++Group) {
      if (Readers[Group].empty())
        continue;

      uint32_t New = ClassOfReaders.size();
      auto It = ClassOfReaders.try_emplace(std::move(Readers[Group]), New)
                  .first;
      ClassOfGroup[Group] = It->second;
    }

    // The paths of each class
    Result->Classes.resize(ClassOfReaders.size());
    std::unordered_map<std::string, uint32_t> PathIndex;
    auto AddPath = [&](const TupleTreePath &Path, uint32_t Class) {
      std::string Text = *pathAsString<model::Binary>(Path);
      auto [It, New] = PathIndex.try_emplace(Text, Result->Paths.size());
      if (New)
        Result->Paths.push_back(std::move(Text));
      Result->Classes[Class].push_back(It->second);
    };
    for (uint32_t Group = 0; Group < Groups.size(); ++Group) {
      uint32_t Class = ClassOfGroup[Group];
      if (Class == NoClass)
        continue;

      for (const TupleTreePath &Path : Groups[Group].Read)
        AddPath(Path, Class);
      for (const TupleTreePath &Path : Groups[Group].ExactVectors)
        AddPath(Path, Class);
    }
    for (std::vector<uint32_t> &Class : Result->Classes) {
      std::sort(Class.begin(), Class.end());
      Class.erase(std::unique(Class.begin(), Class.end()), Class.end());
    }

    // The classes of each object
    Result->Arguments.resize(Grouped.size());
    for (size_t I = 0; I < Grouped.size(); ++I) {
      for (GroupedEntry &Entry : Grouped[I]) {
        std::vector<uint32_t> Classes;
        for (uint32_t Group : Entry.Groups)
          Classes.push_back(ClassOfGroup[Group]);
        std::sort(Classes.begin(), Classes.end());
        Classes.erase(std::unique(Classes.begin(), Classes.end()),
                      Classes.end());
        Result->Arguments[I].push_back({ Entry.Object, std::move(Classes) });
      }
    }

    if (CheckClasses)
      checkClasses(*Result);

    return Result;
  }

  /// Abort unless, for every object, the paths of its classes are the paths it
  /// would have been reported with as pairs
  void checkClasses(const revng::pypeline::DependencyClasses &Classes) const {
    size_t Objects = 0;
    size_t Pairs = 0;
    size_t ClassReferences = 0;
    for (size_t I = 0; I < Grouped.size(); ++I) {
      revng_assert(Classes.Arguments[I].size() == ExpectedPaths[I].size());
      for (size_t J = 0; J < ExpectedPaths[I].size(); ++J) {
        std::set<std::string> Expected(ExpectedPaths[I][J].begin(),
                                       ExpectedPaths[I][J].end());
        std::set<std::string> Actual;
        for (uint32_t Class : Classes.Arguments[I][J].Classes)
          for (uint32_t Path : Classes.Classes[Class])
            Actual.insert(Classes.Paths[Path]);
        revng_assert(Expected == Actual,
                     "The dependency classes of an object do not stand for the "
                     "paths it read");
        ++Objects;
        Pairs += Expected.size();
        ClassReferences += Classes.Arguments[I][J].Classes.size();
      }
    }

    size_t ClassPaths = 0;
    for (const std::vector<uint32_t> &Class : Classes.Classes)
      ClassPaths += Class.size();
    dbg << "dependency classes verified: " << Objects << " objects, " << Pairs
        << " (object, path) pairs as " << Classes.Classes.size() << " classes, "
        << ClassReferences << " (object, class) pairs, " << ClassPaths
        << " (class, path) pairs, " << Classes.Paths.size() << " paths\n";
  }

  std::vector<std::string> getReadPaths() {
    std::vector<std::string> Result;
    ReadFields ReadFieldsStruct = (Touched.has_value() and InObject) ?
                                    Touched->finishObject() :
                                    revng::Tracking::collect(Binary);
    for (const TupleTreePath &Path : ReadFieldsStruct.Read)
      Result.push_back(*pathAsString<model::Binary>(Path));
    for (const TupleTreePath &Path : ReadFieldsStruct.ExactVectors)
      Result.push_back(*pathAsString<model::Binary>(Path));
    return Result;
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
