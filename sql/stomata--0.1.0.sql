/* stomata--0.1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION stomata" to load this file. \quit

CREATE FUNCTION stomata_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD stomata TYPE INDEX HANDLER stomata_handler;
COMMENT ON ACCESS METHOD stomata IS 'segmented automaton index for LIKE and ILIKE';

CREATE OPERATOR CLASS stomata_text_ops
DEFAULT FOR TYPE text USING stomata AS
    OPERATOR 1 ~~ (text, text),
    OPERATOR 2 ~~* (text, text);

-- Diagnostics -------------------------------------------------------------

CREATE FUNCTION stomata_keys(value text,
                             k int DEFAULT 3, cap int DEFAULT 2, rollup int DEFAULT 3,
                             reverse_depth int DEFAULT 1, composite bool DEFAULT true,
                             cross_edges bool DEFAULT false, anchor_len int DEFAULT 4,
                             skip_depth int DEFAULT 4)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'stomata_keys'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION stomata_keys(text, int, int, int, int, bool, bool, int, int)
IS 'keys a stomata index with these parameters stores for a value';

CREATE FUNCTION stomata_pattern_keys(pattern text,
                                     k int DEFAULT 3, cap int DEFAULT 2, rollup int DEFAULT 3,
                                     reverse_depth int DEFAULT 1, composite bool DEFAULT true,
                                     cross_edges bool DEFAULT false, anchor_len int DEFAULT 4,
                                     skip_depth int DEFAULT 4, ilike bool DEFAULT false)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'stomata_pattern_keys'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION stomata_pattern_keys(text, int, int, int, int, bool, bool, int, int, bool)
IS 'keys a LIKE (or ILIKE) pattern requires (an empty set means a full scan)';

CREATE FUNCTION stomata_index_info(index regclass,
    OUT generation bigint, OUT heap_pages bigint, OUT runs int, OUT keys bigint, OUT stopped_keys bigint,
    OUT blob_pages bigint, OUT blob_bytes bigint, OUT pending_records bigint,
    OUT pending_pages bigint, OUT index_pages bigint,
    OUT k int, OUT cap int, OUT rollup int, OUT reverse_depth int,
    OUT composite bool, OUT cross_edges bool, OUT stop_threshold real,
    OUT pending_limit int, OUT anchor_len int, OUT skip_depth int,
    OUT exact bool, OUT exact_bigrams bool, OUT format_version int)
RETURNS record
AS 'MODULE_PATHNAME', 'stomata_index_info'
LANGUAGE C STRICT;

CREATE FUNCTION stomata_runs(index regclass,
    OUT run int, OUT run_id bigint, OUT rows bigint, OUT keys bigint,
    OUT pages bigint, OUT bytes bigint, OUT heap_pages bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'stomata_runs'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_runs(regclass)
IS 'the sorted runs of a stomata index, oldest first';

CREATE FUNCTION stomata_key_stats(index regclass,
    OUT family text, OUT tier text, OUT keys bigint, OUT array_lists bigint,
    OUT bitmap_lists bigint, OUT stop_keys bigint, OUT bytes bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'stomata_key_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_key_stats(regclass)
IS 'dictionary size by key family and tier';

CREATE FUNCTION stomata_candidate_pages(index regclass, pattern text, ilike bool DEFAULT false)
RETURNS bigint
AS 'MODULE_PATHNAME', 'stomata_candidate_pages_sql'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_candidate_pages(regclass, text, bool)
IS 'number of heap pages a scan for this pattern would touch';

-- Maintenance -------------------------------------------------------------

CREATE FUNCTION stomata_merge_pending(index regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'stomata_merge_pending'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_merge_pending(regclass)
IS 'fold the pending list into a new run and compact similar-sized runs; returns records merged';

CREATE FUNCTION stomata_compact(index regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'stomata_compact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_compact(regclass)
IS 'fold the pending list and merge all runs into one; returns records merged';

CREATE FUNCTION stomata_verify(index regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'stomata_verify'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_verify(regclass)
IS 'number of visible tuples the index fails to cover (0 means sound)';

-- Planner diagnostics ------------------------------------------------

CREATE FUNCTION stomata_pattern_stats(index regclass, pattern text, ilike bool DEFAULT false,
    OUT key text, OUT tier text, OUT runs int, OUT stopped bool, OUT entries bigint, OUT bytes bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'stomata_pattern_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_pattern_stats(regclass, text, bool)
IS 'the keys a pattern needs, with the rows (exact tier) or heap pages (page tier) each one has';

CREATE FUNCTION stomata_estimate(index regclass, pattern text, ilike bool DEFAULT false,
    OUT tids float8, OUT lossy_pages float8, OUT exact bool, OUT keys int,
    OUT values_decoded float8, OUT index_pages float8, OUT rows float8, OUT heap_pages float8)
RETURNS record
AS 'MODULE_PATHNAME', 'stomata_estimate'
LANGUAGE C STRICT;

COMMENT ON FUNCTION stomata_estimate(regclass, text, bool)
IS 'the planner''s view of a pattern: candidate rows and lossy pages, and the work of the scan';
