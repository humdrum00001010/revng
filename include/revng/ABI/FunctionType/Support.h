#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <concepts>
#include <map>
#include <set>

#include "llvm/Support/MathExtras.h"

#include "revng/ADT/Concepts.h"
#include "revng/ADT/STLExtras.h"
#include "revng/Model/Binary.h"
#include "revng/Model/DefinedType.h"
#include "revng/Support/AlignmentHelpers.h"
#include "revng/TupleTree/TupleTree.h"
#include "revng/TupleTree/Visits.h"

namespace abi::FunctionType {

/// Replace all the references to a type definition with \p OldKey key with
/// the \p NewType. It also erases the old type definition.
///
/// \param Old The type references of which should be replaced.
/// \param New The new type to replace references to.
inline void replaceTypeDefinition(const model::TypeDefinition::Key &Old,
                                  const model::TypeDefinitionReference &New,
                                  TupleTree<model::Binary> &Binary) {
  using Reference = model::TypeDefinitionReference;
  Binary.replaceReferencesIf(New, [&Old](const Reference &Path) -> bool {
    if (Path.empty())
      return false;

    return Old == Path.getConst()->key();
  });
}
inline void replaceTypeDefinition(const model::TypeDefinition::Key &Old,
                                  const model::DefinedType &New,
                                  TupleTree<model::Binary> &Binary) {
  return replaceTypeDefinition(Old, New.Definition(), Binary);
}
inline void replaceTypeDefinition(const model::TypeDefinition::Key &O,
                                  const model::Type &N,
                                  TupleTree<model::Binary> &B) {
  return replaceTypeDefinition(O, llvm::cast<model::DefinedType>(N), B);
}

/// Where the references to each type definition are.
///
/// `replaceTypeDefinition` visits every reference of the model to find the
/// ones to a single definition: who replaces many definitions, one at a time,
/// can build this once and then only touch the references involved. It has to
/// be told about every definition that enters or leaves the model in the
/// meantime.
class TypeDefinitionReferenceIndex {
public:
  using Reference = model::TypeDefinitionReference;
  using Key = model::TypeDefinition::Key;

private:
  std::map<Key, std::set<Reference *>> Referrers;

public:
  explicit TypeDefinitionReferenceIndex(TupleTree<model::Binary> &Binary) {
    record(*Binary);
  }

  TypeDefinitionReferenceIndex(const TypeDefinitionReferenceIndex &) = delete;
  TypeDefinitionReferenceIndex &
  operator=(const TypeDefinitionReferenceIndex &) = delete;

public:
  /// Record the references in \p Definition, which has just been added to the
  /// model
  void add(model::UpcastableTypeDefinition &Definition) { record(Definition); }

  /// Forget the references in \p Definition, which is about to be removed
  /// from the model
  void remove(model::UpcastableTypeDefinition &Definition) {
    forEachReferenceIn(Definition, [this](Reference &R) {
      Referrers[R.getConst()->key()].erase(&R);
    });
  }

  /// What `replaceTypeDefinition(Old, New, Binary)` does
  void replace(const Key &Old, const Reference &New) {
    auto It = Referrers.find(Old);
    if (It == Referrers.end())
      return;

    std::set<Reference *> ToReplace = std::move(It->second);
    Referrers.erase(It);

    for (Reference *R : ToReplace)
      *R = New;

    if (not New.empty())
      Referrers[New.getConst()->key()].merge(ToReplace);
  }

private:
  void record(auto &Root) {
    forEachReferenceIn(Root, [this](Reference &R) {
      Referrers[R.getConst()->key()].insert(&R);
    });
  }

  static void forEachReferenceIn(auto &Root, auto &&Callable) {
    auto Visitor = [&Callable]<typename T>(T &Element) {
      if constexpr (std::is_same_v<T, Reference>)
        if (not Element.empty())
          Callable(Element);
    };
    visitTupleTree(Root, Visitor, [](auto &) {});
  }
};

/// Filters a list of upcastable types.
///
/// \tparam DerivedType The desired type to filter based on
/// \param Types The list of types to filter
/// \return filtered list
template<std::derived_from<model::TypeDefinition> DerivedType,
         RangeOf<model::UpcastableTypeDefinition> OwningRange,
         RangeOf<model::TypeDefinition *> ViewRange =
           std::vector<model::TypeDefinition *>>
std::vector<DerivedType *>
filterTypes(OwningRange &FilterFrom, const ViewRange &Ignored = {}) {
  std::vector<DerivedType *> Result;
  for (model::UpcastableTypeDefinition &Type : FilterFrom)
    if (Type && !llvm::is_contained(Ignored, &*Type))
      if (auto *Cast = llvm::dyn_cast<DerivedType>(Type.get()))
        Result.emplace_back(Cast);
  return Result;
}

} // namespace abi::FunctionType
