--
-- This file is distributed under the MIT License. See LICENSE.md for details.
--

-- Dependencies in the form of classes. The model paths that are read by
-- exactly the same objects are a class: `dependency_class_paths` lists the
-- paths of each class once, `dependency_class_objects` the classes each object
-- depends on. An object depends on a path if the path is in one of its
-- classes: together the two tables stand for the rows the `dependencies` table
-- would hold, without their number growing with the product of the objects
-- and of the paths each one reads. `dependencies` keeps the dependencies that
-- are reported one (object, path) pair at a time.
--
-- Nothing has to be cleared: the existing rows of `dependencies` stay valid.

CREATE TABLE IF NOT EXISTS dependency_class_paths(
    class_id     INT NOT NULL,
    model_path   TEXT NOT NULL,
    PRIMARY KEY (class_id, model_path)
) STRICT, WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS model_path_on_dependency_class_paths
    ON dependency_class_paths(model_path);

CREATE TABLE IF NOT EXISTS dependency_class_objects(
    savepoint_id_start   INT NOT NULL,
    savepoint_id_end     INT NOT NULL,
    container_id         TEXT NOT NULL,
    configuration_hash   TEXT NOT NULL,
    object_id            BLOB NOT NULL,
    class_id             INT NOT NULL,
    PRIMARY KEY (savepoint_id_start, savepoint_id_end, container_id,
                 configuration_hash, object_id, class_id)
) STRICT, WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS class_id_on_dependency_class_objects
    ON dependency_class_objects(class_id);
