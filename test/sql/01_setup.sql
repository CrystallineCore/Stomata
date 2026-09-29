-- Access method, operator class and deterministic test data.
-- The extension itself is loaded by pg_regress (--load-extension=stomata).

SELECT amname, amtype FROM pg_am WHERE amname = 'stomata';

SELECT opc.opcname, opc.opcdefault, t.typname
FROM pg_opclass opc
JOIN pg_am am ON am.oid = opc.opcmethod
JOIN pg_type t ON t.oid = opc.opcintype
WHERE am.amname = 'stomata';

SELECT amopstrategy, amopopr::regoperator
FROM pg_amop ao JOIN pg_am am ON am.oid = ao.amopmethod
WHERE am.amname = 'stomata';

SELECT amvalidate(opc.oid)
FROM pg_opclass opc JOIN pg_am am ON am.oid = opc.opcmethod
WHERE am.amname = 'stomata';

-- Escape LIKE metacharacters so values can be turned into literal patterns.
CREATE FUNCTION esc(t text) RETURNS text LANGUAGE sql IMMUTABLE STRICT
AS $$ SELECT replace(replace(replace(t, '\', '\\'), '%', '\%'), '_', '\_') $$;

-- Words over a small alphabet so that patterns collide often.
CREATE TABLE words (id int PRIMARY KEY, w text);
INSERT INTO words
SELECT i, translate(substr(md5(i::text) || md5((i * 7)::text), 1 + i % 5, 2 + (i * 13) % 17),
                    '0123456789abcdef', 'aeilnorstuAEbc_%')
FROM generate_series(1, 3000) i;

-- Special values: empty, NULL, escapes, mixed case, multibyte, long.
INSERT INTO words VALUES
  (3001, ''), (3002, NULL), (3003, 'hello'), (3004, 'pablo'), (3005, 'Hello World'),
  (3006, 'apples'), (3007, 'a\b'), (3008, '100%'), (3009, 'snake_case'),
  (3010, 'café'), (3011, '日本語テキスト'), (3012, 'straße'), (3013, 'ÉCOLE'),
  (3014, repeat('ab', 1500)), (3015, repeat('xyz', 40) || 'tail'), (3016, 'a'),
  (3017, 'ab'), (3018, 'abc'), (3019, '%'), (3020, '_'), (3021, '\'),
  (3022, 'İstanbul'), (3023, 'Kelvin'), (3024, 'MiXeD CaSe'), (3025, 'ÅNGSTRÖM');

-- Patterns: derived from the data (escaped), plus hand-written edge cases.
CREATE TABLE pats (p text PRIMARY KEY);
INSERT INTO pats
SELECT DISTINCT p FROM (
  SELECT esc(left(w, 1 + id % 4)) || '%' AS p FROM words WHERE id % 7 = 0 AND w <> ''
  UNION ALL
  SELECT '%' || esc(right(w, 1 + id % 3)) FROM words WHERE id % 11 = 0 AND w <> ''
  UNION ALL
  SELECT '%' || esc(substr(w, 2, 1 + id % 4)) || '%' FROM words WHERE id % 13 = 0 AND length(w) > 3
  UNION ALL
  SELECT (SELECT string_agg(CASE WHEN (i + id) % 3 = 0 THEN esc(substr(w, i, 1)) ELSE '_' END, ''
                            ORDER BY i)
          FROM generate_series(1, length(w)) i)
  FROM words WHERE id % 17 = 0 AND w <> ''
  UNION ALL
  SELECT esc(left(w, 1)) || '__' || esc(substr(w, 4, 1)) || '%' FROM words WHERE id % 19 = 0 AND length(w) >= 4
  UNION ALL
  SELECT '%' || esc(substr(w, 2, 1)) || '_' || esc(substr(w, 4, 1)) || '%' FROM words WHERE id % 23 = 0 AND length(w) >= 4
  UNION ALL
  SELECT esc(left(w, 2)) || '%' || esc(substr(w, length(w) / 2, 2)) || '%' || esc(right(w, 2))
  FROM words WHERE id % 29 = 0 AND length(w) >= 6
  UNION ALL
  SELECT esc(w) FROM words WHERE id % 31 = 0
  UNION ALL
  SELECT unnest(ARRAY[
    '', '%', '%%', '_', '__', '___%', '%___', 'a%', '%a', '%a%', 'A%', 'hel%', '%lo', 'h___o',
    '%el%', 'H%', '\%%', '%\_%', '%\\%', '%\\', 'caf_', 'café', '%é', '日%', '%テキ%', '%ß%',
    'stra_e', '__ole', 'É%', 'ab%', '%ab', 'a_', '_b', 'abc', 'ab', 'a', '%ba%ba%', '%tail',
    'xyz%tail', '%yzt%', 'a%b%c', '%e%e%e%', 'zz%', '%qq%', '_%_', '%\%', '\_'])
) s;

SELECT count(*) AS words, count(DISTINCT w) AS distinct_words FROM words;
SELECT count(*) > 500 AS enough_patterns FROM pats;

-- Ground truth from a sequential scan.
CREATE FUNCTION like_ids(pat text) RETURNS int[] LANGUAGE sql STABLE
AS $$ SELECT array_agg(id ORDER BY id) FROM words WHERE w LIKE pat $$;

SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TABLE truth AS SELECT p, like_ids(p) AS ids FROM pats;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- ILIKE ground truth (same patterns, case-insensitive)
CREATE FUNCTION ilike_ids(pat text) RETURNS int[] LANGUAGE sql STABLE
AS $$ SELECT array_agg(id ORDER BY id) FROM words WHERE w ILIKE pat $$;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TABLE itruth AS SELECT p, ilike_ids(p) AS ids FROM pats
  UNION ALL SELECT p, ilike_ids(p) FROM unnest(ARRAY['İ%', 'i%', '%KELVIN', '%ström', 'mixed%', '%CASE', 'É%', 'école', 'ÉCOLE']) p;
RESET enable_indexscan;
RESET enable_bitmapscan;

CREATE FUNCTION imismatches() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = off;
  SELECT count(*) INTO n FROM itruth WHERE ilike_ids(p) IS DISTINCT FROM ids;
  RETURN n;
END $$;

-- Compare index results against the truth; must return 0 mismatches.
CREATE FUNCTION mismatches() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = off;
  SELECT count(*) INTO n FROM truth WHERE like_ids(p) IS DISTINCT FROM ids;
  RETURN n;
END $$;
