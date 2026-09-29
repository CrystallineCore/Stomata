-- Cost model: the planner's candidate estimate comes from the index itself.
-- Selective patterns are counted exactly at plan time; otherwise the
-- estimate is an upper bound.  The planner uses the index for selective
-- patterns and a sequential scan when most pages would be read.

CREATE TABLE ce (id int, w text);
INSERT INTO ce
SELECT i, (ARRAY['alpha','beta','gamma','delta','omega','kappa','sigma','theta'])[1 + i % 8]
          || '-' || lpad((i * 7919 % 100000)::text, 5, '0')
FROM generate_series(1, 20000) i;
CREATE INDEX ce_w ON ce USING stomata (w);
ANALYZE ce;

-- candidates the scan returns (exact TIDs) vs the planner's count
CREATE FUNCTION ce_check(pat text, OUT pattern text, OUT est_tids float8, OUT est_lossy float8,
                         OUT counted bool, OUT matches bigint, OUT bound_ok bool)
LANGUAGE plpgsql AS $$
BEGIN
  pattern := pat;
  SELECT tids, lossy_pages, exact INTO est_tids, est_lossy, counted FROM stomata_estimate('ce_w', pat);
  EXECUTE 'SELECT count(*) FROM ce WHERE w LIKE $1' INTO matches USING pat;
  -- an estimate of exact candidates never undercounts the matches
  bound_ok := est_lossy > 0 OR est_tids >= matches;
END $$;

SELECT * FROM ce_check('alpha-0%');
SELECT * FROM ce_check('%-12345');
SELECT * FROM ce_check('%ta-1%');
SELECT * FROM ce_check('%zzz%');
SELECT * FROM ce_check('%a%');
SELECT * FROM ce_check('_____-_____');

-- a budget of 0 still gives an upper bound from key statistics
SET stomata.estimate_budget = 0;
SELECT pattern, counted, bound_ok FROM ce_check('alpha-0%');
SELECT pattern, counted, bound_ok FROM ce_check('%ta-1%');
RESET stomata.estimate_budget;

-- per-key statistics behind an estimate
SELECT key, tier, entries FROM stomata_pattern_stats('ce_w', 'omega-4%') ORDER BY key;

-- plan choices (no sequential-scan penalty; the planner decides)
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT count(*) FROM ce WHERE w LIKE 'alpha-0%';
EXPLAIN (COSTS OFF) SELECT count(*) FROM ce WHERE w LIKE '%-12345';
EXPLAIN (COSTS OFF) SELECT count(*) FROM ce WHERE w LIKE '%a%';
EXPLAIN (COSTS OFF) SELECT count(*) FROM ce WHERE w LIKE '%';
EXPLAIN (COSTS OFF) SELECT count(*) FROM ce WHERE w LIKE ANY (ARRAY['%-12345', '%-54321']);
-- parameters: the generic estimate
PREPARE p(text) AS SELECT count(*) FROM ce WHERE w LIKE $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE p('alpha-0%');
EXECUTE p('alpha-0%');
RESET plan_cache_mode;
DEALLOCATE p;

-- the estimate follows the index: new rows are counted once merged
INSERT INTO ce SELECT 20000 + i, 'zeta-' || i FROM generate_series(1, 300) i;
SELECT stomata_merge_pending('ce_w') > 0 AS merged;
SELECT * FROM ce_check('zeta-%');
RESET max_parallel_workers_per_gather;
DROP TABLE ce;
DROP FUNCTION ce_check(text);
