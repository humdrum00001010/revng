//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <string>

#include "revng/CliftPipes/EmitCAsSingleFile.h"
#include "revng/Model/NameBuilder.h"
#include "revng/PTML/CTokenEmitter.h"
#include "revng/PTML/Constants.h"
#include "revng/PTML/PTMLEmitter.h"

namespace revng::pypeline::piperuns {

EmitCAsSingleFile::EmitCAsSingleFile(const class Model &Model,
                                     llvm::StringRef Configuration,
                                     llvm::StringRef DynamicConfig,
                                     const PTMLCFunctionContainer &Input,
                                     PTMLCContainer &Output) :
  Binary(*Model.get().get()),
  Input(Input),
  Output(Output),
  Configuration(parseCEmissionPipeConfiguration(Configuration)) {
}

void EmitCAsSingleFile::run() {
  ptml::Tagging Tags = Configuration.DisableMarkup ? ptml::Tagging::Disabled :
                                                     ptml::Tagging::Enabled;

  std::string Header;
  {
    ptml::CTokenEmitter Tokens(Tags);

    Tokens.emitIncludeDirective("types-and-globals.h",
                                "",
                                ptml::CTokenEmitter::IncludeMode::Quote);
    Tokens.emitIncludeDirective("helpers.h",
                                "",
                                ptml::CTokenEmitter::IncludeMode::Quote);
    Tokens.emitNewline();

    Header = Tokens.extract();
  }

  // Wrap the includes and the function bodies (already emitted and reformatted
  // upstream) in a single root element, as a PTML document requires.
  //
  // The wrapping tag is emitted by hand because CTokenEmitter intentionally
  // only supports emitting clift constructs (which means the rendered strings
  // `Input.objects()` contains are not emittable).
  std::unique_ptr<llvm::raw_pwrite_stream> Out = Output.getOStream(ObjectID());
  auto Mode = Tags == ptml::Tagging::Enabled ? ptml::EmissionMode::Tags :
                                               ptml::EmissionMode::PlainText;
  ptml::PTMLStreamEmitter Root(*Out, Mode);
  ptml::PTMLTagEmitter Document = Root.initializeOpenTag(ptml::tags::Div);
  Document.finalizeOpenTag();

  // A function with a broken return is, as the model puts it, not a function
  // at all: it leaves the stack pointer where no function does, so its code
  // only makes sense in its callers, which all have it. On its own it has no
  // body to show, unless a call to it is left somewhere.
  std::map<ObjectID, std::string> Fragments;
  {
    model::CNameBuilder NameBuilder(Binary);
    using model::FunctionAttribute::HasOneBrokenReturn;
    for (const model::Function &Function : Binary.Functions())
      if (Function.Attributes().contains(HasOneBrokenReturn))
        Fragments.emplace(ObjectID(Function.Entry()),
                          NameBuilder.name(Function));
  }

  auto IsCalled = [this](const ObjectID &Fragment, llvm::StringRef Name) {
    for (const auto &Object : Input.objects())
      if (Object != Fragment
          and Input.getMemoryBuffer(Object)->getBuffer().contains(Name))
        return true;
    return false;
  };

  *Out << Header << '\n';
  for (const auto &Object : Input.objects()) {
    auto It = Fragments.find(Object);
    if (It != Fragments.end() and not IsCalled(Object, It->second)) {
      *Out << "/* " << It->second
           << " is not a function: it returns leaving the stack as its caller"
              " does not expect.\n   Its code is part of each of its callers."
              " */\n\n";
      continue;
    }

    auto Buffer = Input.getMemoryBuffer(Object);
    *Out << Buffer->getBuffer() << "\n";
  }
}

} // namespace revng::pypeline::piperuns
