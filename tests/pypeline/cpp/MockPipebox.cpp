//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/PipeboxCommon/Helpers/Registrars.h"

#include "MockPipeboxImpl.h"

static RegisterContainer<StringContainer> X;
static RegisterContainer<RootStringContainer> X2;
static RegisterPipe<AppendFooPipe> Y;
static RegisterPipe<CreateEntryPipe> Y2;
static RegisterPipe<AppendFooPipe2> Y3;
static RegisterAnalysis<AppendFooLibAnalysis> Z;
static RegisterAnalysis<AppendFooLibAnalysis2> Z2;

namespace {

revng::pypeline::PipeOutput makeTestPipeOutput(bool Empty) {
  using namespace revng::pypeline;
  if (Empty)
    return { {}, {} };

  ObjectID Root = ObjectID::root();
  auto Function = llvm::cantFail(ObjectID::deserialize("/function/"
                                                       "0x400000:Code_x86_64"));
  auto Type = llvm::cantFail(ObjectID::deserialize("/type-definition/"
                                                   "1001-StructDefinition"));
  std::string FirstPath = "/TypeDefinitions/1001-StructDefinition/Fields/0/"
                          "Comment";
  std::string SecondPath = "/Functions/0x400000:Code_x86_64/Comments/0/Body";

  PipeOutput Result;
  Result.Dependencies = {
    { { Root, FirstPath },
      { Function, SecondPath },
      { Root, SecondPath },
      { Function, FirstPath },
      { Root, FirstPath } },
    {},
    { { Function, SecondPath },
      { Root, FirstPath },
      { Type, "" },
      { Type, FirstPath } },
  };

  std::string Payload("\0a\0\xff", 4);
  Result.CustomInvalidation.resize(3);
  Result.CustomInvalidation[1].emplace_back(Function,
                                            Buffer(Payload.begin(),
                                                   Payload.end()));
  return Result;
}

struct RegisterPipeOutputTest {
  RegisterPipeOutputTest() {
    namespace python = revng::pypeline::helpers::python;
    python::Registry.registerModuleInitializer([](nanobind::module_ &Module,
                                                  python::BaseClasses &) {
      Module.def("make_test_pipe_output", &makeTestPipeOutput);
    });
  }
};

static RegisterPipeOutputTest PipeOutputTest;

} // namespace
