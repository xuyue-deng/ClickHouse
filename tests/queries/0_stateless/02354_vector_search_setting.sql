-- Tags: no-fasttest, no-ordinary-database

-- Tests that CREATE TABLE and ADD INDEX respect setting 'enable_vector_similarity_index'.

DROP TABLE IF EXISTS tab;

-- Test CREATE TABLE

SET enable_vector_similarity_index = 0;
CREATE TABLE tab (id UInt32, vec Array(Float32), INDEX idx vec TYPE vector_similarity('hnsw', 'L2Distance', 1)) ENGINE = MergeTree ORDER BY tuple(); -- { serverError SUPPORT_IS_DISABLED }

SET enable_vector_similarity_index = 1;
CREATE TABLE tab (id UInt32, vec Array(Float32), INDEX idx vec TYPE vector_similarity('hnsw', 'L2Distance', 1)) ENGINE = MergeTree ORDER BY tuple();
DROP TABLE tab;

-- Test ADD INDEX

CREATE TABLE tab (id UInt32, vec Array(Float32)) ENGINE = MergeTree ORDER BY tuple();

SET enable_vector_similarity_index = 0;
ALTER TABLE tab ADD INDEX idx vec TYPE vector_similarity('hnsw', 'L2Distance', 1);  -- { serverError SUPPORT_IS_DISABLED }

SET enable_vector_similarity_index = 1;
ALTER TABLE tab ADD INDEX idx vec TYPE vector_similarity('hnsw', 'L2Distance', 1);

-- Other index DDL must work regardless of the setting
SET enable_vector_similarity_index = 0;
ALTER TABLE tab MATERIALIZE INDEX idx;
-- ALTER TABLE tab CLEAR INDEX idx; -- <-- Should work but doesn't w/o enabled setting. Unexpected but not terrible.
ALTER TABLE tab DROP INDEX idx;

DROP TABLE tab;
