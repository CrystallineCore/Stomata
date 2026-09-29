/*-------------------------------------------------------------------------
 *
 * stomata.h
 *	  STOMATA: a segmented, bidirectional automaton index for SQL LIKE and
 *	  ILIKE.  Shared declarations.
 *
 * Every indexed string s (n characters) is case-folded per character (ASCII
 * only, length preserving) and terminated:
 *		F = fold(s)          || '\0'
 *		R = reverse(fold(s)) || '\0'
 * Position i belongs to segment i / k.  Keys derived from those strings are
 * stored in one of two tiers:
 *
 *	exact tier   row TIDs (like a trigram index): trigrams, whole anchored
 *				 prefixes/suffixes, first and last character;
 *	page tier    heap pages: positions, segments, bigrams, skip-grams, length.
 *
 * A scan intersects the exact tier at row level and filters the result with
 * the page tier; a pattern with no exact-tier key returns lossy pages.  The
 * executor always rechecks.
 *
 * Storage is log-structured: inserts append to a pending list; a merge turns
 * the pending list into a new immutable sorted *run* and merges runs of
 * similar size by streaming (size-tiered compaction).  Scans read every run
 * plus the pending list.  Freed pages are recycled only once no running
 * snapshot can still be reading them.
 *
 * Correctness invariant:  Matches(P) is a subset of Candidates(P).
 *
 *-------------------------------------------------------------------------
 */
#ifndef STOMATA_H
#define STOMATA_H

#include "postgres.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/reloptions.h"
#include "access/transam.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"

#if PG_VERSION_NUM < 160000
#error "stomata requires PostgreSQL 16 or later"
#endif

#define STOMATA_MAGIC			0x53544F4D	/* "STOM" */
#define STOMATA_VERSION			1
#define STOMATA_MAX_RUNS		16
#define STOMATA_METAPAGE_BLKNO	0

#define STOMATA_LIKE_STRATEGY	1
#define STOMATA_ILIKE_STRATEGY	2

/* exact-tier value of a TID: blk * STOMATA_EXS + (off - 1) */
#define STOMATA_EXS		((uint64) MaxHeapTuplesPerPage)

/* ---------------------------------------------------------------------
 * Parameters (reloptions, copied into the metapage at build time)
 * ---------------------------------------------------------------------
 */
typedef struct StomataParams
{
	int32		k;				/* segment width */
	int32		cap;			/* forward positional depth, in segments */
	int32		rollup;			/* 0..4: global gram order */
	int32		reverse_depth;	/* reverse positional depth, in segments */
	bool		composite;		/* record-level composite keys */
	bool		cross_edges;	/* cross-segment bigram keys */
	float4		stop_threshold; /* 0 = off; else drop page keys denser than this */
	int32		pending_limit;	/* kB of pending list before inline merge */
	int32		anchor_len;		/* whole prefix/suffix keys up to this length */
	int32		skip_depth;		/* skip-gram keys for gaps 2..skip_depth */
	bool		exact;			/* exact (row-level) tier on */
	bool		exact_bigrams;	/* bigrams in the exact tier too */
} StomataParams;

typedef struct StomataOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		k;
	int32		cap;
	int32		rollup;
	int32		reverse_depth;
	bool		composite;
	bool		cross_edges;
	double		stop_threshold;
	int32		pending_limit;
	int32		anchor_len;
	int32		skip_depth;
	bool		exact;
	bool		exact_bigrams;
} StomataOptions;

/* ---------------------------------------------------------------------
 * Characters and keys
 * ---------------------------------------------------------------------
 */
typedef struct StomataChar
{
	uint8		len;			/* bytes, 1..4; the terminator is len 1, b[0]=0 */
	uint8		b[4];
} StomataChar;

#define STOMATA_KEY_MAXLEN	23

/* Fixed-size, zero-padded key: usable as a HASH_BLOBS hash key. */
typedef struct StomataKey
{
	uint8		len;
	uint8		data[STOMATA_KEY_MAXLEN];
} StomataKey;

/* key tags (data[0]) */
#define SK_TAG_P	'P'			/* exact position (head)          */
#define SK_TAG_U	'U'			/* char in segment                */
#define SK_TAG_T	'T'			/* intra-segment bigram           */
#define SK_TAG_C	'C'			/* cross-segment bigram           */
#define SK_TAG_G	'G'			/* global gram (subtype 1..4)     */
#define SK_TAG_X	'X'			/* composite (subtype a..f)       */
#define SK_TAG_A	'A'			/* page holds a value too long for the pending list */
#define SK_TAG_W	'W'			/* page holds a heap-only tuple (HOT) */
#define SK_TAG_N	'N'			/* row has non-ASCII bytes (for ILIKE) */
#define SK_TAG_H	'H'			/* whole anchored prefix/suffix    */
#define SK_TAG_S	'S'			/* skip-gram: x, then y d later    */

#define SK_DIR_F	'F'
#define SK_DIR_R	'R'

#define SK_X_HT1	'a'			/* first char, last char            */
#define SK_X_HT2	'b'			/* first two chars, last two chars  */
#define SK_X_HL		'c'			/* first char, length bucket        */
#define SK_X_TL		'd'			/* last char, length bucket         */
#define SK_X_H2T1	'e'			/* first two chars, last char       */
#define SK_X_H1T2	'f'			/* first char, last two chars       */

/* A sink receives keys produced by the extractor. */
typedef struct StomataKeySink
{
	void		(*emit) (struct StomataKeySink *sink, const StomataKey *key);
	void	   *arg;
} StomataKeySink;

/* A flat, growable list of keys (used by the compiler and for row checks). */
typedef struct StomataKeyList
{
	int			nkeys;
	int			maxkeys;
	StomataKey *keys;
} StomataKeyList;

/* keys.c */
extern void stomata_decode_string(const char *s, int len, StomataChar **chars, int *nchars);
extern void stomata_extract_keys(const char *s, int len, const StomataParams *p,
								 StomataKeySink *sink);
extern void stomata_keylist_init(StomataKeyList *l);
extern void stomata_keylist_add(StomataKeyList *l, const StomataKey *k);
extern void stomata_keylist_sort_unique(StomataKeyList *l);
extern bool stomata_keylist_contains(const StomataKeyList *sorted, const StomataKey *k);
extern void stomata_extract_keylist(const char *s, int len, const StomataParams *p,
									StomataKeyList *out);
extern bool stomata_compile_like(const char *pat, int len, const StomataParams *p,
								 bool icase, StomataKeyList *out);
extern char *stomata_key_to_cstring(const StomataKey *k);
extern int	stomata_key_cmp(const void *a, const void *b);
extern bool stomata_key_exact(const StomataKey *k, const StomataParams *p);
extern void stomata_pseudo_key(StomataKey *k, uint8 tag);

/* ---------------------------------------------------------------------
 * On-disk format
 * ---------------------------------------------------------------------
 */
#define STOMATA_PAGE_META		0x0001
#define STOMATA_PAGE_BLOB		0x0002
#define STOMATA_PAGE_PENDING	0x0004
#define STOMATA_PAGE_DELETED	0x0008

typedef struct StomataPageOpaqueData
{
	BlockNumber next;			/* next page of the same chain */
	uint16		flags;
	uint16		stomata_page_id;	/* for identification of STOMATA indexes */
	uint64		delete_fxid;	/* DELETED pages: reusable once this is old */
} StomataPageOpaqueData;

typedef StomataPageOpaqueData *StomataPageOpaque;

#define STOMATA_PAGE_ID		0xFF83

#define StomataPageGetOpaque(page) ((StomataPageOpaque) PageGetSpecialPointer(page))
#define StomataPageCapacity \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(StomataPageOpaqueData)))

/* one immutable sorted run: a key-sorted blob plus its page directory */
typedef struct StomataRun
{
	uint32		run_id;			/* unique within the index (cache key) */
	uint32		npages;			/* heap pages covered: width of its bitmaps */
	BlockNumber blob_head;
	uint32		blob_pages;
	uint64		blob_bytes;
	uint32		nkeys;			/* posting lists */
	uint32		nstopped;		/* stop-keys (treated as TRUE) */
	BlockNumber dir_head;
	uint32		dir_pages;
	uint32		ndir;
	uint32		pad;
	double		rows;			/* index tuples merged into this run */
} StomataRun;

typedef struct StomataMetaPageData
{
	uint32		magic;
	uint32		version;
	StomataParams params;
	uint32		generation;		/* bumped whenever the run list changes */
	uint32		next_run_id;
	uint32		npages;			/* heap pages covered by all runs */
	uint32		nruns;
	StomataRun	runs[STOMATA_MAX_RUNS]; /* oldest (largest) first */
	BlockNumber pending_head;	/* active pending list (appended by inserts) */
	BlockNumber pending_tail;
	uint32		pending_records;
	uint32		pending_pages;
	BlockNumber frozen_head;	/* pending list being merged (still scanned) */
	uint32		frozen_records;
	uint32		frozen_pages;
	double		indexed_rows;
} StomataMetaPageData;

#define StomataPageGetMeta(page) ((StomataMetaPageData *) PageGetContents(page))

/* blob container types */
#define STOMATA_CT_ARRAY	0		/* varbyte-coded deltas (pages or exact TIDs) */
#define STOMATA_CT_BITMAP	1		/* page tier: plain bitmap over heap pages */
#define STOMATA_CT_STOP		2		/* stop-key: no payload, treated as TRUE */

/*
 * pending-list record: uint32 heap block, uint16 offset, uint16 length, bytes.
 * offset 0 marks a heap-only tuple (its own line pointer is not a valid entry
 * point), which is indexed for its whole page.
 */
#define STOMATA_PENDING_HDR		8
#define STOMATA_PENDING_LONG	0xFFFF	/* value too long: page is always a candidate */
#define STOMATA_PENDING_MAXVAL	2000

/* ---------------------------------------------------------------------
 * Scan-side views of the runs
 * ---------------------------------------------------------------------
 */
/* directory record: first entry starting on a blob page */
typedef struct StomataDirRec
{
	StomataKey	key;
	BlockNumber blk;
	uint32		off;
} StomataDirRec;

/* page directory of one run (cached per backend; runs are immutable) */
typedef struct StomataImage
{
	MemoryContext cxt;
	uint32		run_id;
	uint32		npages;
	StomataDirRec *dir;
	uint32		ndir;
} StomataImage;

typedef struct StomataRunSet
{
	int			nruns;
	uint32		npages;			/* width for page bitmaps */
	StomataImage *img[STOMATA_MAX_RUNS];
} StomataRunSet;

/* dense bitmap over heap pages */
typedef struct StomataBits
{
	uint32		nbits;
	uint32		nwords;
	uint64	   *w;
} StomataBits;

/* sorted array of posting values */
typedef struct StomataVals
{
	uint64	   *v;
	uint32		n;
} StomataVals;

/* a posting list fetched by key from one run */
typedef struct StomataPosting
{
	bool		found;
	uint8		ctype;
	uint32		len;
	uint8	   *data;
} StomataPosting;

/* a key's postings across all runs */
typedef struct StomataMulti
{
	bool		found;			/* present in some run */
	bool		stop;			/* a stop-key in some run: treat as TRUE */
	int			n;
	uint64		len;			/* total payload bytes (for ordering) */
	StomataPosting part[STOMATA_MAX_RUNS];
} StomataMulti;

/* storage.c: pages, metapage */
extern void stomata_params_from_options(Relation index, StomataParams *p);
extern void stomata_init_metapage(Page page, const StomataParams *p);
extern void stomata_check_meta(Relation index, const StomataMetaPageData *m);
extern void stomata_read_meta(Relation index, StomataMetaPageData *meta);
extern Buffer stomata_new_buffer(Relation index);
extern void stomata_release_skipped(Relation index);
extern bool stomata_page_recyclable(Page page);
extern void stomata_free_chain(Relation index, BlockNumber head, FullTransactionId fxid);

/* storage.c: run writer and readers */
typedef struct StomataRunWriter StomataRunWriter;
extern StomataRunWriter *stomata_rw_begin(Relation index, uint32 npages);
extern void stomata_rw_add(StomataRunWriter *rw, const StomataKey *key, uint8 ctype,
						   const uint8 *data, uint32 len);
extern void stomata_rw_finish(StomataRunWriter *rw, StomataRun *out);

/* a key-ordered stream of (key, container) entries */
typedef struct StomataSrc
{
	bool		(*next) (struct StomataSrc *s);
	void		(*close) (struct StomataSrc *s);
	StomataKey	key;
	uint8		ctype;
	uint32		len;
	uint8	   *data;
	uint32		cap;
} StomataSrc;

extern StomataSrc *stomata_src_run(Relation index, const StomataRun *run);
extern void stomata_src_reserve(StomataSrc *s, uint32 len);

/* storage.c: lookup */
extern void stomata_get_runset(Relation index, const StomataMetaPageData *meta,
							   StomataRunSet *rs);
extern void stomata_lookup(Relation index, const StomataImage *img, const StomataKey *key,
						   StomataPosting *out);
extern void stomata_lookup_multi(Relation index, const StomataRunSet *rs,
								 const StomataKey *key, StomataMulti *out);
extern void stomata_multi_vals(const StomataMulti *m, StomataVals *out);
extern void stomata_multi_pages(const StomataMulti *m, StomataBits *out, bool and_into);
extern void stomata_multi_free(StomataMulti *m);

/* planner statistics of one key in one run (stomata_lookup_stats) */
typedef struct StomataKeyStats
{
	bool		found;
	uint8		ctype;
	uint32		pages;			/* blob pages read to fetch it */
	uint64		bytes;			/* payload bytes */
	uint64		count;			/* TIDs or heap pages */
	bool		estimated;		/* count extrapolated from a sample */
} StomataKeyStats;

#define STOMATA_STATS_SAMPLE	8192	/* array bytes counted per key and run */

extern void stomata_lookup_stats(Relation index, const StomataImage *img,
								 const StomataKey *key, StomataKeyStats *out);
extern uint64 stomata_container_count(const uint8 *d, uint32 len, uint8 ctype);

/* storage.c: containers */
extern void stomata_bits_init(StomataBits *b, uint32 nbits, bool fill);
extern void stomata_decode_pages(const uint8 *d, uint32 len, uint8 ctype,
								 StomataBits *out, bool and_into);
extern void stomata_decode_vals(const uint8 *d, uint32 len, StomataVals *out);
extern void stomata_container_vals(const uint8 *d, uint32 len, uint8 ctype, StomataVals *out);
extern bool stomata_vals_contains(const StomataVals *v, uint64 x);
extern void stomata_vals_intersect(StomataVals *acc, const StomataVals *x);
extern void stomata_vals_union(StomataVals *acc, StomataVals *x);
extern uint8 stomata_encode_list(StringInfo buf, const StomataKey *key, const uint64 *v,
								 uint32 n, uint32 npages, const StomataParams *p,
								 bool apply_stop, bool allow_bitmap);

/* storage.c: pending list */
extern void stomata_pending_append(Relation index, BlockNumber heapblk, OffsetNumber off,
								   const char *val, int len);

typedef void (*StomataPendingVisitor) (BlockNumber blk, OffsetNumber off,
									   const char *val, int len, void *arg);
extern void stomata_pending_scan(Relation index, BlockNumber head,
								 StomataPendingVisitor visit, void *arg);

/* build-time posting lists */
typedef struct StomataBuildEntry
{
	StomataKey	key;			/* hash key: must be first */
	uint32		n;
	uint32		max;
	uint64	   *vals;			/* heap pages, or exact TID values */
} StomataBuildEntry;

/* lsm.c: sorted runs from unsorted keys, streaming merges, maintenance */
typedef struct StomataCollector StomataCollector;
extern StomataCollector *stomata_collector_create(const StomataParams *p, Size mem_limit);
extern void stomata_collector_add(StomataCollector *c, const StomataKey *k, BlockNumber blk,
								  uint64 tidval);
extern void stomata_collector_check_memory(StomataCollector *c);
extern void stomata_collector_to_run(StomataCollector *c, Relation index, uint32 npages,
									 StomataSrc **extra, int nextra, StomataRun *out);
extern BlockNumber stomata_collector_maxblk(StomataCollector *c);
extern void stomata_collector_destroy(StomataCollector *c);

#define STOMATA_MERGE_INLINE	0	/* from an insert: skip if busy, bounded work */
#define STOMATA_MERGE_NORMAL	1	/* VACUUM / stomata_merge_pending(): wait */
#define STOMATA_MERGE_FULL		2	/* stomata_compact(): everything into one run */

extern int64 stomata_merge(Relation index, int mode);
#include "access/genam.h"
extern int64 stomata_purge(Relation index, IndexBulkDeleteCallback cb, void *cbstate);
extern void stomata_recycle_pages(Relation index, BlockNumber *free_pages,
								  BlockNumber *deleted_pages);

static inline uint64
stomata_tid_value(BlockNumber blk, OffsetNumber off)
{
	return (uint64) blk * STOMATA_EXS + (uint64) (off - 1);
}

/* stomata.c */
extern PGDLLEXPORT Datum stomata_handler(PG_FUNCTION_ARGS);
extern void _PG_init(void);

/* LIKE / ILIKE patterns of a scan, used to test pending records exactly */
typedef struct StomataPatterns
{
	int			n;
	Datum	   *pat;
	Oid		   *coll;
	bool	   *icase;
	bool	   *eval;			/* false: cannot evaluate (nondeterministic) */
	bool		any_icase;
} StomataPatterns;

typedef struct StomataQuery
{
	StomataKeyList all;			/* keys of every pattern */
	StomataKeyList like_only;	/* keys of the LIKE (case-sensitive) patterns */
	StomataPatterns pats;
	bool		full_scan;
	bool		impossible;
} StomataQuery;

/* cost.c, stomata.c */
extern int	stomata_estimate_budget;

typedef struct StomataEstimate
{
	double		tids;			/* exact candidates (rows) */
	double		lossy_pages;	/* whole pages */
	bool		complete;		/* counted exactly at plan time */
	int			keys;
	double		values_decoded; /* by the scan */
	double		index_pages;	/* read by the scan */
	double		rows;			/* rows in the runs */
	double		pages;			/* heap pages covered */
} StomataEstimate;

extern void stomata_estimate_pattern(Relation index, text *pattern, bool icase,
									 StomataEstimate *out);
struct PlannerInfo;
struct IndexPath;
extern void stomata_costestimate(struct PlannerInfo *root, struct IndexPath *path,
								 double loop_count, Cost *indexStartupCost,
								 Cost *indexTotalCost, Selectivity *indexSelectivity,
								 double *indexCorrelation, double *indexPages);
extern void stomata_costestimate_generic(struct PlannerInfo *root, struct IndexPath *path,
										 double loop_count, Cost *indexStartupCost,
										 Cost *indexTotalCost, Selectivity *indexSelectivity);

extern int64 stomata_candidates(Relation index, Relation heap, const StomataQuery *q,
								TIDBitmap *tbm);

#endif							/* STOMATA_H */
