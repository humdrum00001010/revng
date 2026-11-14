#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

from collections.abc import Buffer
from typing import Mapping, Sequence, final

from revng.pypeline.container import ConfigurationId, ContainerDeclaration, ContainerSet
from revng.pypeline.object import ObjectID, ObjectSet
from revng.pypeline.storage.storage_provider import ContainerLocation, PipeDependencies
from revng.pypeline.storage.storage_provider import SavePointsRange, SharedParts, StorageProvider
from revng.pypeline.utils import fixes

from .requests import Requests
from .task import TaskArgument, TaskArgumentAccess


class _StreamedObjects(Mapping):
    """
    The serialized (and compressed, if the container asks for it) objects of a
    container, produced a chunk at a time as they are iterated over: what the
    storage provider is given in place of a dict that holds them all.
    """

    CHUNK = 256

    def __init__(self, container, objects: ObjectSet, compression):
        self._container = container
        self._kind = objects.kind
        # In a fixed order (that of their serialized form): the order in which they are
        # written does not matter to the storage, but this way it is the same in every run
        self._objects = sorted(objects, key=lambda o: o.serialize())
        self._set = set(self._objects)
        self._compression = compression

    def _encode(self, buffer):
        if self._compression is None:
            return buffer
        return self._compression.compress(buffer)

    def __len__(self):
        return len(self._objects)

    def __iter__(self):
        return iter(self._objects)

    def __contains__(self, key):
        return key in self._set

    def __getitem__(self, key):
        if key not in self._set:
            raise KeyError(key)
        serialized = self._container.serialize(ObjectSet(self._kind, {key}))
        return self._encode(serialized[key])

    def items(self):  # type: ignore[override]
        for start in range(0, len(self._objects), self.CHUNK):
            chunk = self._objects[start : start + self.CHUNK]
            serialized = self._container.serialize(ObjectSet(self._kind, set(chunk)))
            for object_id in chunk:
                yield object_id, self._encode(serialized[object_id])
            del serialized


# The shared form of a container's objects (see fixes.SHARED_CLIFT_TYPES and
# include/revng/PipeboxCommon/CliftSharedTypes.h): an object or a part starts
# with its magic, then the number of parts it refers to (u32, little endian)
# and their hashes (32 bytes each)
_SHARED_OBJECT_MAGIC = b"CLSHOBJ1"
_SHARED_PART_MAGIC = b"CLSHPRT1"


def _shared_hashes(buffer, magic: bytes) -> bytes:
    """The hashes of the parts an object or a part refers to, one after the other."""
    view = memoryview(buffer)
    assert bytes(view[: len(magic)]) == magic
    offset = len(magic)
    count = int.from_bytes(view[offset : offset + 4], "little")
    offset += 4
    return bytes(view[offset : offset + count * SharedParts.HASH_SIZE])


def _uses_shared_parts(container, storage_provider: StorageProvider) -> bool:
    return (
        fixes.enabled(fixes.SHARED_CLIFT_TYPES)
        and hasattr(container, "begin_shared_parts")
        and storage_provider.supports_shared_parts()
    )


def _serialize_shared(
    container, objects: ObjectSet, compression, chunk: int = 256
) -> tuple[dict[ObjectID, Buffer], dict[bytes, tuple[bytes, Buffer]], dict[ObjectID, bytes]]:
    """
    Serialize the objects in the shared form, a chunk at a time. Returns the
    (compressed) objects, the (compressed) parts they need with the hashes of
    the parts each part refers to, and the hashes of the parts each object
    refers to.
    """

    def encode(buffer):
        return buffer if compression is None else compression.compress(buffer)

    serialized_objects: dict[ObjectID, Buffer] = {}
    parts: dict[bytes, tuple[bytes, Buffer]] = {}
    object_parts: dict[ObjectID, bytes] = {}
    ordered = sorted(objects, key=lambda o: o.serialize())
    container.begin_shared_parts()
    try:
        for start in range(0, len(ordered), chunk):
            some = ordered[start : start + chunk]
            serialized = container.serialize(ObjectSet(objects.kind, set(some)))
            for part_hash, part in container.take_shared_parts().items():
                parts[part_hash] = (_shared_hashes(part, _SHARED_PART_MAGIC), encode(part))
            for object_id in some:
                buffer = serialized[object_id]
                object_parts[object_id] = _shared_hashes(buffer, _SHARED_OBJECT_MAGIC)
                serialized_objects[object_id] = encode(buffer)
            del serialized
    finally:
        container.end_shared_parts()
    return serialized_objects, parts, object_parts


def _load_shared_parts(
    container, location: ContainerLocation, data, compression, storage_provider: StorageProvider
):
    """
    If some of the objects are in the shared form, give the container the parts
    they need before it reads them.
    """
    magic = _SHARED_OBJECT_MAGIC
    shared = [k for k, v in data.items() if bytes(memoryview(v)[: len(magic)]) == magic]
    if not shared:
        return
    assert hasattr(container, "add_shared_parts"), "objects in the shared form, unknown here"
    parts = storage_provider.get_shared_parts(location, shared)
    if compression is not None:
        parts = {k: compression.decompress(v) for k, v in parts.items()}
    container.add_shared_parts(parts)


class SavePoint:
    """
    A SavePoint is a task that satisfies its requests by looking up the requested objects in the
    container it owns.
    """

    __slots__: tuple = ("name", "arguments", "to_save")

    def __init__(
        self,
        name: str,
        to_save: Sequence[ContainerDeclaration],
    ):
        self.name = name
        self.arguments = [
            TaskArgument(
                name=declaration.name,
                container_type=declaration.container_type,
                access=TaskArgumentAccess.READ_WRITE,
            )
            for declaration in to_save
        ]
        self.to_save = to_save

    @final
    def prerequisites_for(
        self,
        requests: Requests,
        configuration_id: ConfigurationId,
        storage_provider: StorageProvider,
        savepoint_range: SavePointsRange,
    ) -> Requests:
        """
        This method has the same semantics as Pipe.prerequisites_for, but it has
        additional arguments that the Pipe should not have.
        """
        result = requests.clone()

        for decl, request in result.items():
            if decl not in self.to_save:
                # This savepoint does not care about this container
                continue
            # We have a match, so we can check if the objects are present
            location = ContainerLocation(
                savepoint_id=savepoint_range.start,
                container_id=decl.name,
                configuration_id=configuration_id,
            )
            found_objs = storage_provider.has(location, request)
            # If so, we should REMOVE the found objects from the request
            for found_obj in found_objs:
                result[decl].remove(found_obj)
        return result

    def run(
        self,
        containers: ContainerSet,
        incoming: Requests,
        outgoing: Requests,
        configuration_id: ConfigurationId,
        storage_provider: StorageProvider,
        savepoint_range: SavePointsRange,
        pipes_dependencies: list[PipeDependencies],
    ):
        """
        The savepoint will cache the incoming containers, and then fill the outgoing
        containers with the cached data.
        """

        assert set(containers.keys()).issuperset(
            set(incoming.keys()) | set(outgoing.keys())
        ), "SavePoint containers must be a subset of incoming and outgoing requests."

        objects: dict[ContainerLocation, Mapping[ObjectID, Buffer]] = {}
        shared_parts: dict[bytes, tuple[bytes, Buffer]] = {}
        shared_objects: dict[ContainerLocation, Mapping[ObjectID, bytes]] = {}
        for decl in self.to_save:
            location = ContainerLocation(
                savepoint_id=savepoint_range.start,
                container_id=decl.name,
                configuration_id=configuration_id,
            )
            container_incoming = incoming.get(decl)
            if len(container_incoming) > 0 and _uses_shared_parts(
                containers[decl], storage_provider
            ):
                objects[location], parts, shared_objects[location] = _serialize_shared(
                    containers[decl], container_incoming, containers[decl].get_compression()
                )
                shared_parts.update(parts)
            elif len(container_incoming) > 0 and fixes.enabled(fixes.STREAMED_SAVEPOINTS):
                objects[location] = _StreamedObjects(
                    containers[decl], container_incoming, containers[decl].get_compression()
                )
            elif len(container_incoming) > 0:
                serialized_container = containers[decl].serialize(container_incoming)
                compression = containers[decl].get_compression()
                if compression is not None:
                    objects[location] = {
                        k: compression.compress(v) for k, v in serialized_container.items()
                    }
                else:
                    objects[location] = serialized_container

        if shared_objects:
            # The parts and the objects that refer to them, in one transaction
            storage_provider.add_objects(  # type: ignore[call-arg]
                pipes_dependencies, objects, shared=SharedParts(shared_parts, shared_objects)
            )
        else:
            storage_provider.add_objects(pipes_dependencies, objects)
        del shared_parts, shared_objects

        for decl in self.to_save:
            # Compute the actual set of objects to load, if an object has
            # already been saved from incoming do not re-load it from storage
            container_outgoing = outgoing.get(decl) - incoming.get(decl)

            if len(container_outgoing) > 0:
                location = ContainerLocation(
                    savepoint_id=savepoint_range.start,
                    container_id=decl.name,
                    configuration_id=configuration_id,
                )
                stored = storage_provider.get(location, container_outgoing)
                compression = containers[decl].get_compression()
                container_data: Mapping[ObjectID, Buffer] = stored
                if compression is not None:
                    container_data = {k: compression.decompress(v) for k, v in stored.items()}
                _load_shared_parts(
                    containers[decl], location, container_data, compression, storage_provider
                )
                containers[decl].deserialize(container_data)
