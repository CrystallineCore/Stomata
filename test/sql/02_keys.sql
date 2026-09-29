-- Key extraction and pattern compilation (default parameters: k=3, cap=2,
-- rollup=3, reverse_depth=1, composite, no cross edges).

SELECT k FROM stomata_keys('hello') k ORDER BY 1;
SELECT count(*) FROM stomata_keys('');
SELECT k FROM stomata_keys('HeLLo') k EXCEPT SELECT k FROM stomata_keys('hello') k;

-- Anchored prefix, suffix, exact-with-underscores, infix.
SELECT k FROM stomata_pattern_keys('hel%') k ORDER BY 1;
SELECT k FROM stomata_pattern_keys('%lo') k ORDER BY 1;
SELECT k FROM stomata_pattern_keys('h___o') k ORDER BY 1;
SELECT k FROM stomata_pattern_keys('%el%') k ORDER BY 1;
SELECT k FROM stomata_pattern_keys('ab%cd%ef') k ORDER BY 1;

-- Every pattern key must be among the value's keys when the value matches.
SELECT v, p, v LIKE p AS matches,
       NOT EXISTS (SELECT stomata_pattern_keys(p) EXCEPT SELECT stomata_keys(v)) AS keys_subset
FROM (VALUES ('hello', 'hel%'), ('hello', '%lo'), ('hello', 'h___o'), ('hello', '%el%'),
             ('pablo', 'pab__'), ('apples', 'app%s'), ('snake_case', 'snake\_%'),
             ('café', 'caf_'), ('日本語テキスト', '日%スト'), ('100%', '100\%')) t(v, p);

-- Patterns with no usable keys compile to an empty set (full scan).
SELECT p, count(k) FROM (VALUES ('%'), ('%%'), ('_%'), ('')) t(p)
LEFT JOIN LATERAL stomata_pattern_keys(p) k ON true GROUP BY p ORDER BY p;
SELECT count(*) FROM stomata_pattern_keys('abc\');

-- Parameters change the key set.
SELECT count(*) FROM stomata_keys('hello', k => 1, cap => 8);
SELECT count(*) FROM stomata_keys('hello', rollup => 0, composite => false);
SELECT k FROM stomata_keys('abcdef', cross_edges => true) k WHERE k LIKE 'C:%' ORDER BY 1;
SELECT count(*) FROM stomata_keys('x', k => 9);
SELECT count(*) FROM stomata_keys('x', rollup => 5);

-- Rows with non-ASCII bytes carry the N marker (ILIKE fallback).
SELECT v, 'N' IN (SELECT stomata_keys(v)) AS marked
FROM (VALUES ('hello'), ('café'), ('日本')) t(v);

-- ILIKE folds ASCII in the pattern and turns non-ASCII literals into '_'.
SELECT k FROM stomata_pattern_keys('HeL%', ilike => true) k ORDER BY 1;
SELECT k FROM stomata_pattern_keys('caf_', ilike => true) k
EXCEPT SELECT k FROM stomata_pattern_keys('café', ilike => true) k ORDER BY 1;
