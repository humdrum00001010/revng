//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#ifndef MLIR_CLIFT_DIALECT
#define MLIR_CLIFT_DIALECT

// Include the definition of the necessary tablegen constructs for defining our
// dialect
include "mlir/IR/DialectBase.td"

def Clift_Dialect : Dialect {
  let summary = "Clift dialect.";
  let description = [{
    very important
  }];

  // This is the namespace of the dialect. It is used to encapsulate the
  // sub-components of the dialect, such as operations ("my_dialect.foo")
  let name = "clift";

  // The C++ namespace that the dialect, and its sub-components, get placed in
  let cppNamespace = "clift";
  let useDefaultTypePrinterParser = 0;
  let useDefaultAttributePrinterParser = 0;
  let hasOperationAttrVerify = 1;
  let useFoldAPI = kEmitFoldAdaptorFolder;
  let hasNonDefaultDestructor = 1;

  let extraClassDeclaration = [{
    CliftDialectImpl* Impl;

    void registerTypes();
    void registerAttributes();
    void registerOperations();

    static llvm::StringRef getModuleAttrName() {
      return "clift.module";
    }

    static llvm::StringRef getDataModelAttrName() {
      return "clift.data_model";
    }

    const CDataModel *getDefaultDataModel() const;
    void setDefaultDataModel(const CDataModel &DataModel);

    /// The class types (by their definition attribute) that the verifier of a
    /// module has found not to contain themselves by value. The definition of
    /// a class type does not change once set, so this holds for the life of
    /// the context.
    bool isKnownNonRecursive(const void *ClassDefinition) const;
    void setKnownNonRecursive(const void *ClassDefinition);

    mlir::Attribute parseAttribute(mlir::DialectAsmParser &Parser,
                                   mlir::Type Type) const override;

    void printAttribute(mlir::Attribute Attr,
                        mlir::DialectAsmPrinter &Printer) const override;

    mlir::Type parseType(mlir::DialectAsmParser &Parser) const override;

    void printType(mlir::Type Type,
                   mlir::DialectAsmPrinter &Printer) const override;
  }];
}
#endif
