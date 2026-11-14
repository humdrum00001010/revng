#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

from collections.abc import Buffer
from typing import Mapping, Sequence, final

from revng.pypeline.container import ConfigurationId, ContainerDeclaration, ContainerSet
from revng.pypeline.object import ObjectID, ObjectSet
from revng.pypeline.storage.storage_provider import ContainerLocation, PipeDependencies
from revng.pypeline.storage.storage_provider import SavePointsRange, StorageProvider
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
        for decl in self.to_save:
            location = ContainerLocation(
                savepoint_id=savepoint_range.start,
                container_id=decl.name,
                configuration_id=configuration_id,
            )
            container_incoming = incoming.get(decl)
            if len(container_incoming) > 0 and fixes.enabled(fixes.STREAMED_SAVEPOINTS):
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

        storage_provider.add_objects(pipes_dependencies, objects)

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
                container_data = storage_provider.get(location, container_outgoing)
                compression = containers[decl].get_compression()
                if compression is not None:
                    containers[decl].deserialize(
                        {k: compression.decompress(v) for k, v in container_data.items()}
                    )
                else:
                    containers[decl].deserialize(container_data)
