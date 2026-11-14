#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// The shared form of the objects of a Clift container: the definitions of the
// class types (structs and unions) are stored once, in content-addressed parts,
// instead of in every object that uses them.
//
// Objects. Usually an object is the MLIR bytecode of a self-contained module:
// it carries the definitions of every class type reachable from it, which for a
// function of a large binary are thousands. In the shared form an object is
//
//   "CLSHOBJ1", u32 N (little endian), N part hashes (32 bytes each), bytecode
//
// where the bytecode writes every class type as a reference to its handle (see
// clift/CliftBytecodeSharing.h) and the N parts hold the definitions of the
// class types the module refers to directly.
//
// Parts. The class types reachable from the objects are divided in strongly
// connected components (a struct and the structs it reaches through pointers
// and that reach it back). A part holds the definitions of one component:
//
//   "CLSHPRT1", u32 N, N part hashes, bytecode
//
// where the N parts are those of the components its definitions refer to, and
// the bytecode is that of a module whose attribute `clift.shared_types` lists
// the class types of the component, written in full, every other class type as
// a reference. The hash of a part is the SHA-256 of all its bytes, so it covers
// the hashes of the parts it refers to: the same hash means the same
// definitions, all the way down.
//
// Reading an object reads its parts first, each after the parts it refers to,
// which defines the class types in the context exactly as reading their
// definitions inside the object would; then the module is read with the
// references resolved to those types. The module is the same.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/SubElementInterfaces.h"
#include "mlir/Parser/Parser.h"

#include "revng/Clift/Clift.h"
#include "revng/Clift/CliftBytecodeSharing.h"
#include "revng/Support/Assert.h"

namespace revng::pypeline::clift_shared {

inline constexpr llvm::StringLiteral ObjectMagic = "CLSHOBJ1";
inline constexpr llvm::StringLiteral PartMagic = "CLSHPRT1";
inline constexpr llvm::StringLiteral TypesAttributeName = "clift.shared_types";
inline constexpr size_t HashSize = 32;

/// A buffer in the shared form: its hashes and its bytecode
struct Parsed {
  std::vector<std::string> Hashes;
  llvm::StringRef Bytecode;
};

inline bool hasMagic(llvm::StringRef Buffer, llvm::StringRef Magic) {
  return Buffer.starts_with(Magic);
}

inline Parsed parse(llvm::StringRef Buffer, llvm::StringRef Magic) {
  revng_check(Buffer.starts_with(Magic));
  size_t Offset = Magic.size();
  revng_check(Buffer.size() >= Offset + 4);
  uint32_t Count = 0;
  for (unsigned I = 0; I < 4; ++I)
    Count |= uint32_t(uint8_t(Buffer[Offset + I])) << (8 * I);
  Offset += 4;
  revng_check(Buffer.size() >= Offset + size_t(Count) * HashSize);
  Parsed Result;
  for (uint32_t I = 0; I < Count; ++I) {
    Result.Hashes.push_back(Buffer.substr(Offset, HashSize).str());
    Offset += HashSize;
  }
  Result.Bytecode = Buffer.substr(Offset);
  return Result;
}

inline std::string compose(llvm::StringRef Magic,
                           llvm::ArrayRef<std::string> Hashes,
                           llvm::StringRef Bytecode) {
  std::string Result;
  Result.reserve(Magic.size() + 4 + Hashes.size() * HashSize + Bytecode.size());
  Result += Magic;
  uint32_t Count = Hashes.size();
  for (unsigned I = 0; I < 4; ++I)
    Result += char((Count >> (8 * I)) & 0xff);
  for (const std::string &Hash : Hashes) {
    revng_assert(Hash.size() == HashSize);
    Result += Hash;
  }
  Result += Bytecode;
  return Result;
}

inline llvm::StringRef handleOf(mlir::Attribute Definition) {
  if (auto S = mlir::dyn_cast<clift::StructAttr>(Definition))
    return S.getHandle();
  return mlir::cast<clift::UnionAttr>(Definition).getHandle();
}

inline mlir::Type typeOf(mlir::Attribute Definition) {
  if (auto S = mlir::dyn_cast<clift::StructAttr>(Definition))
    return clift::StructType::get(S);
  return clift::UnionType::get(mlir::cast<clift::UnionAttr>(Definition));
}

/// The definitions of the class types a type or an attribute refers to without
/// going through another class type
class ClassReferences {
  llvm::DenseSet<const void *> Visited;

public:
  llvm::SetVector<mlir::Attribute> Found;

  void visit(mlir::Type Type) {
    if (not Visited.insert(Type.getAsOpaquePointer()).second)
      return;
    if (auto S = mlir::dyn_cast<clift::StructType>(Type)) {
      Found.insert(S.getDefinition());
      return;
    }
    if (auto U = mlir::dyn_cast<clift::UnionType>(Type)) {
      Found.insert(U.getDefinition());
      return;
    }
    if (auto I = mlir::dyn_cast<mlir::SubElementTypeInterface>(Type))
      I.walkImmediateSubElements([&](mlir::Attribute A) { visit(A); },
                                 [&](mlir::Type T) { visit(T); });
  }

  void visit(mlir::Attribute Attr) {
    if (not Visited.insert(Attr.getAsOpaquePointer()).second)
      return;
    if (auto I = mlir::dyn_cast<mlir::SubElementAttrInterface>(Attr))
      I.walkImmediateSubElements([&](mlir::Attribute A) { visit(A); },
                                 [&](mlir::Type T) { visit(T); });
  }

  /// What the definition of a class type refers to: its fields
  void visitDefinition(mlir::Attribute Definition) {
    auto VisitClass = [&](auto Class) {
      for (clift::FieldAttr Field : Class.getFields())
        visit(Field.getType());
      if (auto List = Class.getCAttributeList())
        visit(List);
    };
    if (auto S = mlir::dyn_cast<clift::StructAttr>(Definition)) {
      VisitClass(clift::StructType::get(S));
    } else {
      auto U = mlir::cast<clift::UnionAttr>(Definition);
      VisitClass(clift::UnionType::get(U));
    }
  }
};

struct Statistics {
  uint64_t Objects = 0;
  uint64_t ObjectBytes = 0;
  uint64_t Roots = 0;
  uint64_t Components = 0;
  uint64_t PartsEmitted = 0;
  uint64_t PartBytesEmitted = 0;
  uint64_t PartsLoaded = 0;
  uint64_t PartsSkipped = 0;
};

/// Turns modules into objects in the shared form, and collects the parts they
/// need. The parts are computed once for the whole session: the definitions of
/// the class types cannot change while a container is being serialized.
class Writer {
  mlir::MLIRContext *Context;
  llvm::DenseMap<mlir::Attribute, std::vector<mlir::Attribute>> Edges;
  llvm::DenseMap<mlir::Attribute, std::string> PartOf;
  std::set<std::string> Emitted;
  Statistics &Stats;

public:
  /// Parts produced since the last time they were taken
  std::map<std::string, std::string> NewParts;

public:
  Writer(mlir::MLIRContext *Context, Statistics &Stats) :
    Context(Context), Stats(Stats) {}

  std::string writeObject(mlir::ModuleOp Module) {
    std::string Bytecode;
    llvm::SetVector<mlir::Attribute> Roots;
    {
      clift::bytecode::SharingScope Scope;
      llvm::raw_string_ostream OS(Bytecode);
      mlir::writeBytecodeToFile(Module, OS);
      OS.flush();
      Roots = std::move(Scope.Referenced);
    }

    std::set<std::string> Hashes;
    for (mlir::Attribute Root : Roots) {
      ensurePart(Root);
      revng_assert(PartOf.count(Root));
      Hashes.insert(PartOf.lookup(Root));
    }

    std::vector<std::string> Sorted(Hashes.begin(), Hashes.end());
    std::string Result = compose(ObjectMagic, Sorted, Bytecode);
    ++Stats.Objects;
    Stats.ObjectBytes += Result.size();
    Stats.Roots += Roots.size();
    return Result;
  }

private:
  const std::vector<mlir::Attribute> &edges(mlir::Attribute Definition) {
    auto It = Edges.find(Definition);
    if (It != Edges.end())
      return It->second;
    ClassReferences References;
    References.visitDefinition(Definition);
    std::vector<mlir::Attribute> Result(References.Found.begin(),
                                        References.Found.end());
    return Edges.try_emplace(Definition, std::move(Result)).first->second;
  }

  /// Tarjan's algorithm, iterative, from Root, over the class types whose part
  /// is not known yet: the components are found children first, so the parts
  /// they refer to are always known when they are written.
  void ensurePart(mlir::Attribute Root) {
    if (PartOf.count(Root))
      return;

    llvm::DenseMap<mlir::Attribute, unsigned> Index;
    llvm::DenseMap<mlir::Attribute, unsigned> Low;
    llvm::DenseSet<mlir::Attribute> OnStack;
    std::vector<mlir::Attribute> Stack;
    struct Frame {
      mlir::Attribute Node;
      size_t NextEdge;
    };
    std::vector<Frame> CallStack;
    unsigned Counter = 0;

    auto Enter = [&](mlir::Attribute Node) {
      Index[Node] = Low[Node] = Counter++;
      Stack.push_back(Node);
      OnStack.insert(Node);
      CallStack.push_back({ Node, 0 });
    };

    Enter(Root);
    while (not CallStack.empty()) {
      mlir::Attribute Node = CallStack.back().Node;
      size_t EdgeIndex = CallStack.back().NextEdge;
      if (EdgeIndex < edges(Node).size()) {
        ++CallStack.back().NextEdge;
        mlir::Attribute Next = edges(Node)[EdgeIndex];
        if (PartOf.count(Next))
          continue;
        if (not Index.count(Next)) {
          Enter(Next);
        } else if (OnStack.count(Next)) {
          Low[Node] = std::min(Low[Node], Index[Next]);
        }
        continue;
      }

      CallStack.pop_back();
      if (not CallStack.empty()) {
        mlir::Attribute Parent = CallStack.back().Node;
        Low[Parent] = std::min(Low[Parent], Low[Node]);
      }

      if (Low[Node] == Index[Node]) {
        std::vector<mlir::Attribute> Members;
        mlir::Attribute Member;
        do {
          Member = Stack.back();
          Stack.pop_back();
          OnStack.erase(Member);
          Members.push_back(Member);
        } while (Member != Node);
        writePart(Members);
      }
    }
  }

  void writePart(std::vector<mlir::Attribute> &Members) {
    llvm::sort(Members, [](mlir::Attribute L, mlir::Attribute R) {
      bool LStruct = mlir::isa<clift::StructAttr>(L);
      bool RStruct = mlir::isa<clift::StructAttr>(R);
      if (LStruct != RStruct)
        return LStruct;
      return handleOf(L) < handleOf(R);
    });

    llvm::DenseSet<mlir::Attribute> InComponent(Members.begin(), Members.end());
    llvm::DenseSet<mlir::Attribute> Expected;
    std::set<std::string> ChildHashes;
    for (mlir::Attribute Member : Members) {
      for (mlir::Attribute Child : edges(Member)) {
        if (InComponent.count(Child))
          continue;
        Expected.insert(Child);
        revng_assert(PartOf.count(Child));
        ChildHashes.insert(PartOf.lookup(Child));
      }
    }

    llvm::SmallVector<mlir::Attribute> Types;
    for (mlir::Attribute Member : Members)
      Types.push_back(mlir::TypeAttr::get(typeOf(Member)));

    std::string Bytecode;
    {
      mlir::OwningOpRef<mlir::ModuleOp>
        Scratch = mlir::ModuleOp::create(mlir::UnknownLoc::get(Context));
      Scratch->getOperation()->setAttr(TypesAttributeName,
                                       mlir::ArrayAttr::get(Context, Types));
      clift::bytecode::SharingScope Scope;
      Scope.Inline.insert(Members.begin(), Members.end());
      llvm::raw_string_ostream OS(Bytecode);
      mlir::writeBytecodeToFile(*Scratch, OS);
      OS.flush();

      // What the writer wrote as references must be exactly the class types
      // the component refers to, whose parts are the children of this one
      bool Same = Scope.Referenced.size() == Expected.size();
      for (mlir::Attribute Referenced : Scope.Referenced)
        Same = Same and Expected.count(Referenced);
      revng_check(Same,
                  "The class types written as references in a shared part "
                  "are not those its definitions refer to.");
    }

    std::vector<std::string> Children(ChildHashes.begin(), ChildHashes.end());
    std::string Part = compose(PartMagic, Children, Bytecode);
    const auto *PartData = reinterpret_cast<const uint8_t *>(Part.data());
    llvm::ArrayRef<uint8_t> PartBytes(PartData, Part.size());
    auto Digest = llvm::SHA256::hash(PartBytes);
    std::string Hash(reinterpret_cast<const char *>(Digest.data()),
                     Digest.size());

    for (mlir::Attribute Member : Members)
      PartOf[Member] = Hash;
    ++Stats.Components;
    if (Emitted.insert(Hash).second) {
      ++Stats.PartsEmitted;
      Stats.PartBytesEmitted += Part.size();
      NewParts.emplace(Hash, std::move(Part));
    }
  }
};

/// Reads the parts an object needs (children first), then the object
class Reader {
  mlir::MLIRContext *Context;
  const std::map<std::string, std::string> &Available;
  std::set<std::string> &Loaded;
  Statistics &Stats;

public:
  Reader(mlir::MLIRContext *Context,
         const std::map<std::string, std::string> &Available,
         std::set<std::string> &Loaded,
         Statistics &Stats) :
    Context(Context), Available(Available), Loaded(Loaded), Stats(Stats) {}

  mlir::OwningOpRef<mlir::ModuleOp> readObject(llvm::StringRef Buffer) {
    Parsed Object = parse(Buffer, ObjectMagic);
    for (const std::string &Hash : Object.Hashes)
      loadPart(Hash);

    // A copy, so that the bytecode starts at an aligned address
    std::string Bytecode = Object.Bytecode.str();
    clift::bytecode::SharingScope Scope;
    const mlir::ParserConfig Config(Context);
    return mlir::parseSourceString<mlir::ModuleOp>(Bytecode, Config);
  }

private:
  /// Reads a part after the parts it refers to (not recursively: a chain of
  /// parts can be as long as a chain of types)
  void loadPart(const std::string &Root) {
    struct Frame {
      std::string Hash;
      Parsed Part;
      size_t Next = 0;
    };
    std::vector<Frame> Stack;
    std::set<std::string> InProgress;

    auto Enter = [&](const std::string &Hash) {
      if (Loaded.count(Hash)) {
        ++Stats.PartsSkipped;
        return;
      }

      // The hash of a part covers the hashes of the parts it refers to: a part
      // cannot refer to itself, however indirectly
      revng_check(InProgress.insert(Hash).second,
                  "A part of the shared form of a Clift container refers to "
                  "itself.");
      auto It = Available.find(Hash);
      revng_check(It != Available.end(),
                  "A part of the shared form of a Clift container is missing.");
      Stack.push_back({ Hash, parse(It->second, PartMagic), 0 });
    };

    Enter(Root);
    while (not Stack.empty()) {
      if (Stack.back().Next < Stack.back().Part.Hashes.size()) {
        // A copy: entering a part may move the frames
        std::string Child = Stack.back().Part.Hashes[Stack.back().Next++];
        Enter(Child);
        continue;
      }

      readPart(Stack.back().Hash, Stack.back().Part);
      InProgress.erase(Stack.back().Hash);
      Stack.pop_back();
    }
  }

  void readPart(const std::string &Hash, const Parsed &Part) {
    std::string Bytecode = Part.Bytecode.str();
    {
      clift::bytecode::SharingScope Scope;
      // The scratch module is not an IR module: it only carries the types
      const mlir::ParserConfig Config(Context, /*verifyAfterParse=*/false);
      auto Scratch = mlir::parseSourceString<mlir::ModuleOp>(Bytecode, Config);
      revng_check(Scratch);
      mlir::Operation *Op = Scratch->getOperation();
      auto Types = Op->getAttrOfType<mlir::ArrayAttr>(TypesAttributeName);
      revng_check(Types);
      for (mlir::Attribute Type : Types) {
        auto Class = mlir::cast<mlir::TypeAttr>(Type).getValue();
        if (auto S = mlir::dyn_cast<clift::StructType>(Class))
          revng_check(S.isComplete());
        else
          revng_check(mlir::cast<clift::UnionType>(Class).isComplete());
      }
    }
    Loaded.insert(Hash);
    ++Stats.PartsLoaded;
  }
};

} // namespace revng::pypeline::clift_shared
