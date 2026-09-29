-- Inserts go to the pending list; VACUUM (or stomata_merge_pending) folds them
-- into a new posting-blob generation.  Deletes never clear bits; results stay
-- exact because the executor rechecks.

CREATE TABLE dml (id int PRIMARY KEY, w text) WITH (fillfactor = 70);
INSERT INTO dml SELECT id, w FROM words WHERE id <= 1500;
CREATE INDEX dml_w ON dml USING stomata (w) WITH (pending_limit = 0);
SELECT generation, pending_records FROM stomata_index_info('dml_w');

-- compare against a sequential scan of the same table
CREATE FUNCTION dml_ids(pat text) RETURNS int[] LANGUAGE sql STABLE
AS $$ SELECT array_agg(id ORDER BY id) FROM dml WHERE w LIKE pat $$;
CREATE FUNCTION dml_mismatches() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  CREATE TEMP TABLE dml_truth AS SELECT p, dml_ids(p) AS ids FROM pats;
  SET LOCAL enable_bitmapscan = on;
  SET LOCAL enable_seqscan = off;
  SELECT count(*) INTO n FROM dml_truth WHERE dml_ids(p) IS DISTINCT FROM ids;
  DROP TABLE dml_truth;
  RETURN n;
END $$;

-- inserts (pending), including a value too long for the pending list
INSERT INTO dml SELECT id, w FROM words WHERE id > 1500;
INSERT INTO dml VALUES (5000, repeat('long', 1000) || 'end'), (5001, 'hello again');
SELECT pending_records > 0 AS has_pending FROM stomata_index_info('dml_w');
SELECT dml_mismatches(), stomata_verify('dml_w');

-- updates: non-HOT (key change grows the row) and HOT-eligible
UPDATE dml SET w = w || 'zz' WHERE id % 5 = 0;
UPDATE dml SET w = upper(w) WHERE id % 7 = 0;
SELECT dml_mismatches(), stomata_verify('dml_w');

-- deletes
DELETE FROM dml WHERE id % 3 = 0;
SELECT dml_mismatches(), stomata_verify('dml_w');

-- VACUUM merges the pending list into a new generation
VACUUM dml;
SELECT generation > 1 AS merged, pending_records FROM stomata_index_info('dml_w');
SELECT dml_mismatches(), stomata_verify('dml_w');

-- explicit merge
INSERT INTO dml VALUES (6000, 'fresh value'), (6001, 'another fresh one');
SELECT stomata_merge_pending('dml_w');
SELECT stomata_merge_pending('dml_w');
SELECT pending_records FROM stomata_index_info('dml_w');
SELECT id FROM dml WHERE w LIKE 'fresh%' ORDER BY id;
SELECT dml_mismatches(), stomata_verify('dml_w');

-- inline merge once the pending list exceeds pending_limit (kB)
ALTER INDEX dml_w SET (pending_limit = 8);
REINDEX INDEX dml_w;
INSERT INTO dml SELECT 10000 + i, 'bulk ' || i || repeat('x', i % 50) FROM generate_series(1, 600) i;
SELECT pending_pages * 8 <= 16 AS pending_bounded, generation > 1 AS merged_inline
FROM stomata_index_info('dml_w');
SELECT dml_mismatches(), stomata_verify('dml_w');

-- free pages from old generations are reused
SELECT index_pages < 4 * blob_pages + 20 AS pages_reused FROM stomata_index_info('dml_w');

-- TRUNCATE and rebuild
TRUNCATE dml;
SELECT heap_pages, keys, pending_records FROM stomata_index_info('dml_w');
INSERT INTO dml VALUES (1, 'after truncate');
SELECT id FROM dml WHERE w LIKE '%trunc%';
SELECT stomata_verify('dml_w');
