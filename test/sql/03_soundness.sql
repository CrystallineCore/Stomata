-- Index results must equal sequential-scan results for every pattern, across
-- parameter settings.  mismatches() returns the number of patterns whose
-- result differs; stomata_verify() the number of tuples the index misses.

CREATE INDEX words_w ON words USING stomata (w);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM words WHERE w LIKE 'hel%';
SELECT id, w FROM words WHERE w LIKE 'hel%' ORDER BY id;
RESET enable_seqscan;
SELECT mismatches(), imismatches(), stomata_verify('words_w');
-- ILIKE is indexable (on this 18-page table a sequential scan is as cheap)
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM words WHERE w ILIKE 'HEL%';
RESET enable_seqscan;
SELECT heap_pages, pending_records, k, cap, rollup, reverse_depth, composite, cross_edges
FROM stomata_index_info('words_w');
DROP INDEX words_w;

-- candidate_pct: candidate pages over all patterns, as a percentage of
-- (patterns x heap pages); 100 would mean the index filters nothing.
CREATE FUNCTION check_variant(opts text)
RETURNS TABLE(options text, mismatches bigint, ilike_mismatches bigint, uncovered bigint, candidate_pct int)
LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format('CREATE INDEX words_v ON words USING stomata (w) WITH (%s)', opts);
  options := opts;
  mismatches := mismatches();
  ilike_mismatches := imismatches();
  uncovered := stomata_verify('words_v');
  SELECT round(100.0 * sum(stomata_candidate_pages('words_v', p)) /
               (count(*) * (pg_relation_size('words') / current_setting('block_size')::int)))
    INTO candidate_pct FROM pats;
  DROP INDEX words_v;
  RETURN NEXT;
END $$;

SELECT * FROM check_variant('k = 3');
SELECT * FROM check_variant('k = 1');
SELECT * FROM check_variant('k = 2, cap = 3');
SELECT * FROM check_variant('k = 4, cap = 1');
SELECT * FROM check_variant('k = 8, cap = 1, rollup = 2');
SELECT * FROM check_variant('reverse_depth = 0');
SELECT * FROM check_variant('reverse_depth = 2');
SELECT * FROM check_variant('rollup = 0');
SELECT * FROM check_variant('rollup = 1');
SELECT * FROM check_variant('composite = false');
SELECT * FROM check_variant('cross_edges = true');
SELECT * FROM check_variant('stop_threshold = 0.3');
SELECT * FROM check_variant('stop_threshold = 0.05, rollup = 0, composite = false');
-- anchors, skip-grams, 4-grams
SELECT * FROM check_variant('rollup = 4');
SELECT * FROM check_variant('anchor_len = 0, skip_depth = 0');
SELECT * FROM check_variant('anchor_len = 2, skip_depth = 8');
-- tiers
SELECT * FROM check_variant('exact = false');
SELECT * FROM check_variant('exact_bigrams = true');
SELECT * FROM check_variant('exact = false, stop_threshold = 0.2');
