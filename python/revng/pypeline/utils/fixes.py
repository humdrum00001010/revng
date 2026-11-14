#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

"""
Switches for the changes that cut the cost of the runner on large binaries.
Every change is on by default; the environment variable REVNG_PY_FIXES holds a
bit mask selecting which ones are active, so that the same installation can run
with and without each of them (0 restores the previous behavior).
"""

import os

# Savepoints: hand the dependency rows and the objects to SQLite in batches
# instead of one statement per row
BATCHED_INSERTS = 0

# Model updates: hand the changed paths to SQLite in one batch
BATCHED_INVALIDATION_INPUT = 1

# Model updates: find the objects to invalidate through the indexes instead of
# comparing every stored object with every matching dependency
INDEXED_INVALIDATION = 2

# import-files: read the `Binaries` section of the model instead of parsing
# the whole model in Python
PARTIAL_MODEL_PARSE = 3

# Temporary storage (deleted when the command ends): no durability is needed,
# so keep the rollback journal in memory and do not wait for the disk
TEMPORARY_DB_SETTINGS = 4

# Savepoints: when a pipe reports the dependencies of its objects as classes
# of model paths, store them that way instead of as one row per object and path
DEPENDENCY_CLASSES = 5

# Scheduling: a pipe from which no object is requested in the containers it
# writes does not run; do not ask its predecessors for what it would have
# needed, which made them run for nothing
SKIP_PREREQUISITES_OF_SKIPPED_PIPES = 6

# Savepoints: serialize and compress the objects of a container a chunk at a
# time while the storage provider writes them, instead of first serializing all
# of them and then making a compressed copy of all of them, both kept in memory
# until the end of the savepoint
STREAMED_SAVEPOINTS = 7

# Savepoints: store the containers that support it (the Clift ones) in their
# shared form, where the definitions of the types are stored once, as parts
# that the objects refer to by hash, instead of in every object that uses them.
# Reading recognizes the form of each object whatever the setting.
SHARED_CLIFT_TYPES = 8

_MASK = int(os.environ.get("REVNG_PY_FIXES", "-1"), 0)


def enabled(bit: int) -> bool:
    return ((_MASK >> bit) & 1) == 1
