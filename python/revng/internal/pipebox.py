#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

import inspect
import re
import signal
import tarfile
from io import BytesIO

import yaml

from revng.internal.support import import_pipebox
from revng.model import Binary  # type: ignore[attr-defined]
from revng.pypeline.analysis import Analysis
from revng.pypeline.container import Configuration, Container
from revng.pypeline.model import ReadOnlyModel
from revng.pypeline.object import ObjectSet
from revng.pypeline.storage.file_provider import FileProvider, FileRequest
from revng.pypeline.task.pipe import Pipe, PipeDependencies
from revng.pypeline.task.task import TaskArgument, TaskArgumentAccess
from revng.pypeline.utils import fixes
from revng.support import get_root

# A line that starts a new top-level entry of a serialized model
_TOP_LEVEL_KEY = re.compile(r"\n[^\s#-]")

_native_libraries = [get_root() / "lib/librevngPipebox.so"]
_module, _handles = import_pipebox(_native_libraries)

_native_pipes: dict[str, type] = {}
_native_analyses: dict[str, type] = {}


def _collect_native_pipes_and_analyses():
    for entry_name, entry in vars(_module).items():
        is_dunder = entry_name.startswith("__") and entry_name.endswith("__")
        if is_dunder or not inspect.isclass(entry):
            continue

        if issubclass(entry, Pipe):
            _native_pipes[entry.name] = entry
        elif issubclass(entry, Analysis):
            _native_analyses[entry.name] = entry


_collect_native_pipes_and_analyses()


def initialize(argv: list[str] = []):
    _module.initialize(
        {
            signal.SIGINT,
            signal.SIGTERM,
            signal.SIGHUP,
            signal.SIGQUIT,
            signal.SIGCHLD,
            signal.SIGPIPE,
            signal.SIGUSR1,
            signal.SIGUSR2,
        },
        argv,
    )


def argv_hook(input_: list[str]):
    # Check that the command is what we expect
    assert input_[0] == "pype"
    assert input_[1] == "pipeline"
    assert input_[2] in ("run-pipe", "run-analysis")

    if input_[2] == "run-pipe":
        pipe_name = input_[3]
        # Check if the specified pipe is a native pipe, if so then use the
        # `run-pipe-native` command instead of `run-pipe`
        if pipe_name not in _native_pipes:
            return ["revng", *input_[1:]]
        else:
            return ["revng", "pipeline", "run-pipe-native", *input_[3:]]
    else:
        analysis_name = input_[3]
        # Check if the specified analysis is a native analysis, if so then use
        # the `run-analysis-native` command instead of `run-analysis`
        if analysis_name not in _native_analyses:
            return ["revng", *input_[1:]]
        else:
            return ["revng", "pipeline", "run-analysis-native", *input_[3:]]


class ImportFiles(Pipe):
    name = "import-files"

    @classmethod
    def signature(cls) -> tuple[TaskArgument, ...]:
        return (
            TaskArgument(
                "binaries-container",
                _module.BinariesContainer,
                TaskArgumentAccess.WRITE,
                help_text="BinariesContainer container which will be populated",
            ),
        )

    @staticmethod
    def _binaries_section(serialized_model: str) -> str | None:
        """The text of the top-level `Binaries` entry of a serialized model, or
        None if it cannot be told apart from the rest."""
        start = serialized_model.find("\nBinaries:\n")
        if start == -1:
            return None
        start += 1
        match = _TOP_LEVEL_KEY.search(serialized_model, start + len("Binaries:\n"))
        end = match.start() + 1 if match is not None else len(serialized_model)
        return serialized_model[start:end]

    def _get_file_requests(self, model: ReadOnlyModel):
        # TODO: add facilities to `model` that avoid having to serialize the
        # whole thing. Right now we need to serialize the entire model and load
        # it in the python wrapper to be able to access the `/Binaries` list.
        serialized_model = model.serialize().decode()

        indexes: list[int] = []
        requests: list[FileRequest] = []

        # Parsing the whole model in Python takes time proportional to its
        # size (minutes for a large binary) and this pipe only needs one small
        # section of it
        section = None
        if fixes.enabled(fixes.PARTIAL_MODEL_PARSE):
            section = self._binaries_section(serialized_model)

        if section is not None:
            for binary in yaml.safe_load(section)["Binaries"] or []:
                indexes.append(int(binary["Index"]))
                requests.append(
                    FileRequest(
                        str(binary["Hash"]), str(binary["CanonicalPath"]), int(binary["Size"])
                    )
                )
            return (indexes, requests)

        model_obj = Binary.deserialize(serialized_model)
        for binary in model_obj.Binaries:
            indexes.append(binary.Index)
            requests.append(FileRequest(binary.Hash, binary.CanonicalPath, binary.Size))

        return (indexes, requests)

    def run(
        self,
        file_provider: FileProvider,
        model: ReadOnlyModel,
        containers: list[Container],
        incoming: list[ObjectSet],
        outgoing: list[ObjectSet],
        configuration: Configuration,
    ) -> PipeDependencies:
        if len(outgoing[0]) == 0:
            return PipeDependencies([[]])

        assert len(outgoing[0]) == 1
        root_object = list(outgoing[0].objects)[0]

        indexes, requests = self._get_file_requests(model)
        files = file_provider.get_files(requests)
        buffer = BytesIO()
        with tarfile.open(mode="w", fileobj=buffer) as tar:
            for request in requests:
                file = files[request.hash]

                info = tarfile.TarInfo()
                info.size = len(file)
                info.name = f"binaries/{request.hash}"
                info.mode = 0o644
                info.type = tarfile.REGTYPE

                tar.addfile(info, BytesIO(file))

        containers[0].deserialize({root_object: buffer.getvalue()})

        dependencies = [(root_object, f"/Binaries/{i}/Hash") for i in indexes]
        dependencies.append((root_object, "/Binaries"))

        return PipeDependencies([dependencies])

    def needed_files(self, model: ReadOnlyModel) -> list[FileRequest]:
        return self._get_file_requests(model)[1]
