-- The index must actually filter: candidate heap pages for selective
-- patterns are a small fraction of the table.

CREATE TABLE sel (id int, w text);
INSERT INTO sel SELECT i, 'user_' || lpad(i::text, 6, '0') || '@' ||
  (ARRAY['example.com', 'mail.org', 'corp.net', 'uni.edu'])[1 + i % 4]
FROM generate_series(1, 20000) i;
CREATE INDEX sel_w ON sel USING stomata (w);
-- a deeper positional window (cap) sharpens long anchored patterns
CREATE INDEX sel_w4 ON sel USING stomata (w) WITH (cap = 4, reverse_depth = 3);

SELECT pg_relation_size('sel') / current_setting('block_size')::int AS heap_pages;
SELECT heap_pages FROM stomata_index_info('sel_w');

SELECT p, stomata_candidate_pages('sel_w', p) AS candidate_pages,
       stomata_candidate_pages('sel_w4', p) AS candidate_pages_cap4,
       (SELECT count(DISTINCT (ctid::text::point)[0]) FROM sel WHERE w LIKE p) AS pages_with_match
FROM (VALUES ('user\_000042@%'), ('%@uni.edu'), ('user\_0000__@corp.net'),
             ('%123%'), ('%.org'), ('u%'), ('zzz%'), ('%')) t(p)
ORDER BY p;

-- index is much smaller than the heap for this column
SELECT pg_relation_size('sel_w') < pg_relation_size('sel') AS smaller_than_heap;

-- The exact tier (default) rechecks far fewer rows than a page-only index
-- (exact = false) for the same heap pages.
CREATE INDEX sel_p ON sel USING stomata (w) WITH (exact = false);

CREATE FUNCTION rechecked(keep regclass, pat text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
  r bigint;
  j json;
  i regclass;
BEGIN
  BEGIN
    FOR i IN SELECT indexrelid::regclass FROM pg_index
             WHERE indrelid = 'sel'::regclass AND indexrelid <> keep LOOP
      EXECUTE 'DROP INDEX ' || i;
    END LOOP;
    SET LOCAL enable_seqscan = off;
    EXECUTE format('EXPLAIN (ANALYZE, FORMAT JSON) SELECT * FROM sel WHERE w LIKE %L', pat) INTO j;
    r := COALESCE((j->0->'Plan'->>'Rows Removed by Index Recheck')::bigint, 0);
    RAISE EXCEPTION 'undo';
  EXCEPTION WHEN raise_exception THEN
    NULL;
  END;
  RETURN r;
END $$;

SELECT p, rechecked('sel_p', p) AS rechecked_page_mode, rechecked('sel_w', p) AS rechecked_exact
FROM (VALUES ('%123%'), ('user\_000042@%'), ('%99@corp.net'), ('%0_0_@mail.org')) t(p)
ORDER BY p;
