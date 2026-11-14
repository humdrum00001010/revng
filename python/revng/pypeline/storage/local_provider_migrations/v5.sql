--
-- This file is distributed under the MIT License. See LICENSE.md for details.
--

-- Parts of objects that are stored once instead of in every object that needs
-- them (see `SHARED_CLIFT_TYPES` in revng/pypeline/utils/fixes.py).
--
-- `shared_parts` holds each part once: the key is the hash of the part, the
-- content the (compressed) part, `children` the hashes of the parts it refers
-- to, one after the other. A part never changes: the same hash means the same
-- content.
--
-- `shared_object_parts` lists, for each object stored in the shared form, the
-- hashes of the parts it refers to directly, one after the other.
--
-- A part is needed as long as a stored object refers to it, directly or
-- through other parts; the parts that are no longer needed are deleted when
-- objects are invalidated.

CREATE TABLE IF NOT EXISTS shared_parts(
    hash       BLOB PRIMARY KEY,
    children   BLOB NOT NULL,
    content    BLOB NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS shared_object_parts(
    savepoint_id         INT NOT NULL,
    container_id         TEXT NOT NULL,
    configuration_hash   TEXT NOT NULL,
    object_id            BLOB NOT NULL,
    hashes               BLOB NOT NULL,
    PRIMARY KEY (savepoint_id, container_id, configuration_hash, object_id)
) STRICT;
