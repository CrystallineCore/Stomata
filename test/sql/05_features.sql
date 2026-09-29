-- Planner integration, operators, options, NULLs, collations and misuse.

CREATE TABLE f (id int, w text, v varchar(40));
INSERT INTO f SELECT i, 'item-' || i, 'v' || (i % 97) FROM generate_series(1, 2000) i;
INSERT INTO f VALUES (0, NULL, NULL);
CREATE INDEX f_w ON f USING stomata (w);
CREATE INDEX f_v ON f USING stomata (v);
CREATE INDEX f_lower ON f USING stomata (lower(w));
CREATE INDEX f_part ON f USING stomata (w) WHERE id > 1000;
ANALYZE f;
SET enable_seqscan = off;

EXPLAIN (COSTS OFF) SELECT id FROM f WHERE w LIKE 'item-12%';
SELECT id FROM f WHERE w LIKE 'item-12_' ORDER BY id;

-- varchar columns use the text operator class
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE v LIKE 'v9%';
SELECT count(*) FROM f WHERE v LIKE 'v9_';

-- expression and partial indexes
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE lower(w) LIKE 'item-1999';
SELECT id FROM f WHERE lower(w) LIKE 'item-1999';
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE w LIKE '%-150_' AND id > 1000;
SELECT id FROM f WHERE w LIKE '%-150_' AND id > 1000 ORDER BY id;

-- several LIKE conditions on one column are ANDed inside one index scan
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE w LIKE 'item-1%' AND w LIKE '%99';
SELECT id FROM f WHERE w LIKE 'item-1%' AND w LIKE '%99' ORDER BY id;

-- runtime parameters (prepared statement, generic plan)
PREPARE q(text) AS SELECT count(*) FROM f WHERE w LIKE $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE q('item-7%');
EXECUTE q('item-7%');
EXECUTE q('%-7');
EXECUTE q(NULL);
RESET plan_cache_mode;

-- ESCAPE clause is folded into a backslash pattern
SELECT count(*) FROM f WHERE w LIKE 'item#-1%' ESCAPE '#';
-- a trailing escape is still an error, as without the index
SELECT count(*) FROM f WHERE w LIKE 'item\';

-- NOT LIKE is not indexable
RESET enable_seqscan;
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE w NOT LIKE 'item%';
SET enable_seqscan = off;

-- ILIKE is indexed (strategy 2), alone and mixed with LIKE
EXPLAIN (COSTS OFF) SELECT id FROM f WHERE w ILIKE 'ITEM-1_';
SELECT count(*) FROM f WHERE w ILIKE 'ITEM-1_';
SELECT count(*) FROM f WHERE w ILIKE 'ITEM-1%' AND w LIKE '%9';
SELECT count(*) FROM f WHERE w ~~* '%-19__';
SELECT stomata_candidate_pages('f_w', 'ITEM-1_', true) > 0 AS ilike_candidates;

-- NULLs are not indexed and never match
SELECT count(*) FROM f WHERE w LIKE '%';
SELECT count(*) FROM f WHERE w IS NULL;

-- empty table, then data
CREATE TABLE e (w text);
CREATE INDEX e_w ON e USING stomata (w);
SELECT count(*) FROM e WHERE w LIKE 'a%';
INSERT INTO e VALUES ('abc');
SELECT w FROM e WHERE w LIKE 'a%';
SELECT stomata_verify('e_w');

-- unlogged table
CREATE UNLOGGED TABLE u (w text);
INSERT INTO u SELECT 'u' || i FROM generate_series(1, 500) i;
CREATE INDEX u_w ON u USING stomata (w);
SELECT count(*) FROM u WHERE w LIKE 'u4%';
SELECT stomata_verify('u_w');

-- reloption validation
CREATE INDEX bad ON f USING stomata (w) WITH (k = 0);
CREATE INDEX bad ON f USING stomata (w) WITH (rollup = 5);
CREATE INDEX bad ON f USING stomata (w) WITH (stop_threshold = 1.5);
CREATE INDEX bad ON f USING stomata (w) WITH (nonsense = 1);
CREATE INDEX bad ON f USING stomata (w) WITH (anchor_len = 5);
CREATE INDEX bad ON f USING stomata (w) WITH (skip_depth = 9);
-- reverse_depth is clamped to cap
CREATE INDEX f_clamp ON f USING stomata (w) WITH (cap = 1, reverse_depth = 5);
SELECT cap, reverse_depth FROM stomata_index_info('f_clamp');
DROP INDEX f_clamp;

-- unsupported shapes
CREATE INDEX bad ON f USING stomata (w, v);
CREATE INDEX bad ON f USING stomata (id);
CREATE UNIQUE INDEX bad ON f USING stomata (w);

-- dictionary composition
SELECT family, tier, keys > 0 AS has_keys FROM stomata_key_stats('f_w') ORDER BY family, tier;

-- functions reject other index types and non-indexes
CREATE INDEX f_btree ON f (w);
SELECT stomata_index_info('f_btree');
SELECT stomata_verify('f');
DROP INDEX f_btree;

-- an explicit deterministic collation keeps using the index
SELECT count(*) FROM f WHERE w COLLATE "C" LIKE 'item-1%';

RESET enable_seqscan;
