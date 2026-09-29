/*-------------------------------------------------------------------------
 *
 * stomata.c
 *	  Index access method handler, build, insert, vacuum and bitmap scan.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "nodes/tidbitmap.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/builtins.h"
#include "catalog/pg_collation.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "optimizer/optimizer.h"
#include "nodes/nodeFuncs.h"
#include "access/htup_details.h"
#include "access/heapam.h"
#include <math.h>

#include "stomata.h"

PG_MODULE_MAGIC;

static relopt_kind stomata_relopt_kind;

/* testing only: pause scans after they copy the metapage (see replication.sh) */
static int	stomata_test_scan_delay_ms = 0;
static relopt_parse_elt stomata_relopt_tab[12];

void
_PG_init(void)
{
	DefineCustomIntVariable("stomata.test_scan_delay_ms",
							"Testing only: pause every index scan after it reads the run list.",
							"Widens the window in which a merge can replace the runs a scan reads.",
							&stomata_test_scan_delay_ms, 0, 0, 60000, PGC_SUSET,
							GUC_UNIT_MS | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("stomata.estimate_budget",
							"Posting values the planner may decode to count a pattern's candidates.",
							"Selective patterns are evaluated exactly at plan time; beyond this many "
							"values the count is estimated from key statistics.  0 = statistics only.",
							&stomata_estimate_budget, 50000, 0, INT_MAX, PGC_USERSET,
							0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("stomata");
	stomata_relopt_kind = add_reloption_kind();

	add_int_reloption(stomata_relopt_kind, "k",
					  "Segment width in characters", 3, 1, 8, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "cap",
					  "Forward positional depth in segments", 2, 1, 64, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "rollup",
					  "Order of global gram keys (0-4)", 3, 0, 4, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "reverse_depth",
					  "Reverse positional depth in segments", 1, 0, 64, AccessExclusiveLock);
	add_bool_reloption(stomata_relopt_kind, "composite",
					   "Store record-level composite keys", true, AccessExclusiveLock);
	add_bool_reloption(stomata_relopt_kind, "cross_edges",
					   "Store cross-segment bigram keys", false, AccessExclusiveLock);
	add_real_reloption(stomata_relopt_kind, "stop_threshold",
					   "Drop page-tier keys present on more than this fraction of pages (0 = off)",
					   0.0, 0.0, 1.0, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "pending_limit",
					  "Pending list size in kB that triggers a merge on insert (0 = only VACUUM)",
					  4096, 0, 2097151, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "anchor_len",
					  "Store whole prefixes and suffixes up to this length (0 or 2-4)",
					  4, 0, 4, AccessExclusiveLock);
	add_int_reloption(stomata_relopt_kind, "skip_depth",
					  "Store skip-grams for character gaps 2..skip_depth (0 = off)",
					  4, 0, 8, AccessExclusiveLock);
	add_bool_reloption(stomata_relopt_kind, "exact",
					   "Keep selective keys at row (TID) level", true, AccessExclusiveLock);
	add_bool_reloption(stomata_relopt_kind, "exact_bigrams",
					   "Keep bigrams at row level too", false, AccessExclusiveLock);

	stomata_relopt_tab[0] = (relopt_parse_elt) {"k", RELOPT_TYPE_INT, offsetof(StomataOptions, k)};
	stomata_relopt_tab[1] = (relopt_parse_elt) {"cap", RELOPT_TYPE_INT, offsetof(StomataOptions, cap)};
	stomata_relopt_tab[2] = (relopt_parse_elt) {"rollup", RELOPT_TYPE_INT, offsetof(StomataOptions, rollup)};
	stomata_relopt_tab[3] = (relopt_parse_elt) {"reverse_depth", RELOPT_TYPE_INT, offsetof(StomataOptions, reverse_depth)};
	stomata_relopt_tab[4] = (relopt_parse_elt) {"composite", RELOPT_TYPE_BOOL, offsetof(StomataOptions, composite)};
	stomata_relopt_tab[5] = (relopt_parse_elt) {"cross_edges", RELOPT_TYPE_BOOL, offsetof(StomataOptions, cross_edges)};
	stomata_relopt_tab[6] = (relopt_parse_elt) {"stop_threshold", RELOPT_TYPE_REAL, offsetof(StomataOptions, stop_threshold)};
	stomata_relopt_tab[7] = (relopt_parse_elt) {"pending_limit", RELOPT_TYPE_INT, offsetof(StomataOptions, pending_limit)};
	stomata_relopt_tab[8] = (relopt_parse_elt) {"anchor_len", RELOPT_TYPE_INT, offsetof(StomataOptions, anchor_len)};
	stomata_relopt_tab[9] = (relopt_parse_elt) {"skip_depth", RELOPT_TYPE_INT, offsetof(StomataOptions, skip_depth)};
	stomata_relopt_tab[10] = (relopt_parse_elt) {"exact", RELOPT_TYPE_BOOL, offsetof(StomataOptions, exact)};
	stomata_relopt_tab[11] = (relopt_parse_elt) {"exact_bigrams", RELOPT_TYPE_BOOL, offsetof(StomataOptions, exact_bigrams)};
}

static bytea *
stomata_options(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate, stomata_relopt_kind,
									  sizeof(StomataOptions), stomata_relopt_tab,
									  lengthof(stomata_relopt_tab));
}

/* ------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------
 */
typedef struct BuildState
{
	StomataCollector *c;
	MemoryContext tmpcxt;		/* per-tuple */
	StomataParams params;
	double		indtuples;
	BlockNumber blk;
	uint64		tidval;
} BuildState;

static void
build_sink_emit(StomataKeySink *sink, const StomataKey *key)
{
	BuildState *bs = (BuildState *) sink->arg;

	stomata_collector_add(bs->c, key, bs->blk, bs->tidval);
}

static void
build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
			   bool tupleIsAlive, void *state)
{
	BuildState *bs = (BuildState *) state;
	MemoryContext old;
	text	   *t;
	StomataKeySink sink;

	if (isnull[0])
		return;
	old = MemoryContextSwitchTo(bs->tmpcxt);
	t = DatumGetTextPP(values[0]);
	/* for HOT chains the build scan reports the root line pointer */
	bs->blk = ItemPointerGetBlockNumber(tid);
	bs->tidval = stomata_tid_value(bs->blk, ItemPointerGetOffsetNumber(tid));
	sink.emit = build_sink_emit;
	sink.arg = bs;
	stomata_extract_keys(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &bs->params, &sink);
	bs->indtuples += 1;
	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
	stomata_collector_check_memory(bs->c);
}

/*
 * Build: collect keys (spilling sorted runs to temporary files beyond
 * maintenance_work_mem) and merge them into the index's first run.
 */
static IndexBuildResult *
stomata_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BuildState	bs;
	double		reltuples;
	Buffer		metabuf;
	GenericXLogState *xlog;
	Page		metapage;
	StomataMetaPageData *m;
	uint32		npages;
	StomataRun	run;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	stomata_params_from_options(index, &bs.params);
	bs.c = stomata_collector_create(&bs.params, (Size) maintenance_work_mem * 1024);
	bs.tmpcxt = AllocSetContextCreate(CurrentMemoryContext, "stomata build tuple", ALLOCSET_DEFAULT_SIZES);

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   build_callback, &bs, NULL);

	npages = Max(RelationGetNumberOfBlocks(heap), stomata_collector_maxblk(bs.c));

	/* block 0 is the metapage: allocate it first, fill it last */
	metabuf = stomata_new_buffer(index);
	Assert(BufferGetBlockNumber(metabuf) == STOMATA_METAPAGE_BLKNO);

	stomata_collector_to_run(bs.c, index, npages, NULL, 0, &run);
	run.run_id = 1;
	run.rows = bs.indtuples;

	xlog = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(xlog, metabuf, GENERIC_XLOG_FULL_IMAGE);
	stomata_init_metapage(metapage, &bs.params);
	m = StomataPageGetMeta(metapage);
	m->runs[0] = run;
	m->nruns = 1;
	m->next_run_id = 2;
	m->npages = npages;
	m->indexed_rows = bs.indtuples;
	GenericXLogFinish(xlog);
	UnlockReleaseBuffer(metabuf);

	stomata_collector_destroy(bs.c);
	MemoryContextDelete(bs.tmpcxt);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;
	return result;
}

static void
stomata_buildempty(Relation index)
{
	StomataParams p;
	Buffer		metabuf;

	/*
	 * The init fork holds only a metapage.  An unlogged index is reset to it
	 * after a crash; the first scan then sees no blob and falls back to the
	 * (empty) pending list, which is correct for an empty heap.
	 */
	stomata_params_from_options(index, &p);
	metabuf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	START_CRIT_SECTION();
	stomata_init_metapage(BufferGetPage(metabuf), &p);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(metabuf);
}

/* ------------------------------------------------------------------------
 * Insert, vacuum
 * ------------------------------------------------------------------------
 */
static bool
stomata_insert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
			   Relation heapRel, IndexUniqueCheck checkUnique,
			   bool indexUnchanged, IndexInfo *indexInfo)
{
	text	   *t;
	BlockNumber blk = ItemPointerGetBlockNumber(ht_ctid);
	OffsetNumber off = ItemPointerGetOffsetNumber(ht_ctid);

	if (isnull[0])
		return false;

	/*
	 * Being a summarizing access method, we are also called for HOT updates,
	 * with the TID of a heap-only tuple.  Scans cannot enter a HOT chain in
	 * the middle, so the tuple is indexed under the root line pointer of its
	 * chain, exactly as CREATE INDEX does.  If the root cannot be found (or
	 * the table is not a heap), it is recorded for its whole page (offset 0).
	 */
	if (heapRel != NULL)
	{
		Buffer		hbuf = ReadBuffer(heapRel, blk);
		Page		hpage;

		LockBuffer(hbuf, BUFFER_LOCK_SHARE);
		hpage = BufferGetPage(hbuf);
		if (off <= PageGetMaxOffsetNumber(hpage))
		{
			ItemId		lp = PageGetItemId(hpage, off);

			if (ItemIdIsNormal(lp) &&
				HeapTupleHeaderIsHeapOnly((HeapTupleHeader) PageGetItem(hpage, lp)))
			{
				OffsetNumber root = InvalidOffsetNumber;

				if (heapRel->rd_tableam == GetHeapamTableAmRoutine())
				{
					OffsetNumber *roots = palloc(sizeof(OffsetNumber) * MaxHeapTuplesPerPage);

					heap_get_root_tuples(hpage, roots);
					root = roots[off - 1];
					pfree(roots);
				}
				off = OffsetNumberIsValid(root) ? root : InvalidOffsetNumber;
			}
		}
		else
			off = InvalidOffsetNumber;
		UnlockReleaseBuffer(hbuf);
	}

	t = DatumGetTextPP(values[0]);
	stomata_pending_append(index, blk, off, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	return false;
}

/*
 * Dead rows are removed from the exact (TID) tier by rewriting the runs
 * where they are a noticeable share; page-tier postings are monotone
 * summaries (a stale page only costs a recheck) and stay.
 */
static IndexBulkDeleteResult *
stomata_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				   IndexBulkDeleteCallback callback, void *callback_state)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->tuples_removed += stomata_purge(info->index, callback, callback_state);
	return stats;
}

static IndexBulkDeleteResult *
stomata_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	stomata_merge(info->index, STOMATA_MERGE_NORMAL);
	{
		BlockNumber nfree;
		BlockNumber ndeleted;

		stomata_recycle_pages(info->index, &nfree, &ndeleted);
		stats->pages_free = nfree;
		stats->pages_deleted = nfree + ndeleted;
	}

	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	stats->num_index_tuples = info->num_heap_tuples;
	stats->estimated_count = info->estimated_count;
	return stats;
}

/* ------------------------------------------------------------------------
 * Planner support
 * ------------------------------------------------------------------------
 */
/*
 * Fallback cost model, used when the patterns are not constants (generic
 * plans, parameters): row selectivity from the operators' estimators and
 * the index parameters.  See cost.c for the statistics-based model.
 */
void
stomata_costestimate_generic(PlannerInfo *root, IndexPath *path, double loop_count,
							 Cost *indexStartupCost, Cost *indexTotalCost,
							 Selectivity *indexSelectivity)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	GenericCosts costs;
	StomataParams p;
	Relation	irel;
	double		sel;
	double		tpp;
	bool		row_level;

	memset(&costs, 0, sizeof(costs));
	genericcostestimate(root, path, loop_count, &costs);
	sel = Min(costs.indexSelectivity, 1.0);

	irel = index_open(index->indexoid, NoLock);
	stomata_params_from_options(irel, &p);
	index_close(irel, NoLock);

	/*
	 * Does some constant pattern have a row-level (exact-tier) key?  If none
	 * does (a single character, say), the candidates are whole pages.
	 */
	{
		ListCell   *lc;
		bool		known = false;

		row_level = false;
		foreach(lc, path->indexclauses)
		{
			IndexClause *ic = lfirst_node(IndexClause, lc);
			Expr	   *clause = ic->rinfo->clause;
			OpExpr	   *op;
			Node	   *rhs;
			int			strategy;

			if (!IsA(clause, OpExpr) || list_length(((OpExpr *) clause)->args) != 2)
				continue;
			op = (OpExpr *) clause;
			rhs = strip_implicit_coercions(lsecond(op->args));
			if (!IsA(rhs, Const) || ((Const *) rhs)->constisnull)
				continue;
			strategy = get_op_opfamily_strategy(op->opno, index->opfamily[0]);
			if (strategy != STOMATA_LIKE_STRATEGY && strategy != STOMATA_ILIKE_STRATEGY)
				continue;
			{
				text	   *pat = DatumGetTextPP(((Const *) rhs)->constvalue);
				StomataKeyList kl;

				known = true;
				stomata_keylist_init(&kl);
				stomata_compile_like(VARDATA_ANY(pat), VARSIZE_ANY_EXHDR(pat), &p,
									 strategy == STOMATA_ILIKE_STRATEGY, &kl);
				for (int i = 0; i < kl.nkeys && !row_level; i++)
					row_level = stomata_key_exact(&kl.keys[i], &p);
			}
		}
		if (!known)
			row_level = p.exact;	/* parameters: assume the usual case */
	}

	/*
	 * With the exact tier most selective patterns come back as row TIDs; allow
	 * for some extra rechecks.  Without it, candidates are whole pages, and a
	 * page holding one match makes all of its rows rechecked: 1-(1-sel)^rows.
	 */
	tpp = (baserel->pages > 0) ? baserel->tuples / baserel->pages : 1.0;
	if (p.exact && row_level)
		*indexSelectivity = Min(1.0, sel * 2.0);
	else
		*indexSelectivity = Max(sel, 1.0 - pow(1.0 - sel, Max(tpp, 1.0)));

	/*
	 * Only the directory is cached per backend; posting lists are read through
	 * shared buffers.  Charge a share of the index pages plus the list work.
	 */
	*indexStartupCost = 0;
	*indexTotalCost = 0.05 * index->pages * seq_page_cost +
		cpu_operator_cost * Max(baserel->pages, 1) / 64.0 *
		Max(list_length(path->indexclauses), 1) * 8 +
		cpu_operator_cost * sel * Max(baserel->tuples, 1);
	/*
	 * Lossy pages are processed more slowly by a bitmap heap scan than by a
	 * sequential scan (measured: about 1.5x when most pages qualify), which
	 * the bitmap heap scan's own costing does not know.  Charge half a
	 * sequential scan per fraction of pages returned whole.
	 */
	if (!(p.exact && row_level))
		*indexTotalCost += 0.5 * (*indexSelectivity) *
			(Max(baserel->pages, 1) * seq_page_cost + Max(baserel->tuples, 1) * cpu_tuple_cost);
}

static bool
stomata_validate(Oid opclassoid)
{
	/* the only operator is LIKE (~~) on text, strategy 1; nothing to check */
	return true;
}

/* ------------------------------------------------------------------------
 * Scan
 * ------------------------------------------------------------------------
 */
static IndexScanDesc
stomata_beginscan(Relation r, int nkeys, int norderbys)
{
	return RelationGetIndexScan(r, nkeys, norderbys);
}

static void
stomata_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			   ScanKey orderbys, int norderbys)
{
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));
}

static void
stomata_endscan(IndexScanDesc scan)
{
}

/* ------------------------------------------------------------------------
 * Candidate computation
 * ------------------------------------------------------------------------
 */

/* result under construction: exact TID values and lossy pages */
typedef struct Cand
{
	uint64	   *tids;
	uint32		ntids;
	uint32		maxtids;
	StomataBits lossy;			/* grows as needed */
} Cand;

static void
cand_add_tid(Cand *c, uint64 v)
{
	if (c->ntids >= c->maxtids)
	{
		c->maxtids = c->maxtids ? c->maxtids * 2 : 256;
		c->tids = c->tids ? repalloc_huge(c->tids, sizeof(uint64) * c->maxtids)
			: palloc_extended(sizeof(uint64) * c->maxtids, MCXT_ALLOC_HUGE);
	}
	c->tids[c->ntids++] = v;
}

static void
cand_add_page(Cand *c, BlockNumber blk)
{
	if (blk >= c->lossy.nbits)
	{
		StomataBits nb;
		uint32		nbits = Max(blk + 1, c->lossy.nbits * 2);

		stomata_bits_init(&nb, nbits, false);
		if (c->lossy.w)
		{
			memcpy(nb.w, c->lossy.w, sizeof(uint64) * c->lossy.nwords);
			pfree(c->lossy.w);
		}
		c->lossy = nb;
	}
	c->lossy.w[blk >> 6] |= UINT64CONST(1) << (blk & 63);
}

static int
u64cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* intersect two sorted value arrays, in place (frees nothing) */
void
stomata_vals_intersect(StomataVals *acc, const StomataVals *x)
{
	uint32		i = 0;
	uint32		j = 0;
	uint32		w = 0;

	while (i < acc->n && j < x->n)
	{
		if (acc->v[i] < x->v[j])
			i++;
		else if (acc->v[i] > x->v[j])
			j++;
		else
		{
			acc->v[w++] = acc->v[i];
			i++;
			j++;
		}
	}
	acc->n = w;
}

/*
 * Evaluate one conjunction of keys against the dictionary.  Exact-tier keys
 * are intersected at row level, page-tier keys at page level.  `rrows`,
 * when given, limits the rows (used for ILIKE's non-ASCII fallback).  Adds
 * results to `c`.
 */
static void
eval_conj(Relation index, const StomataRunSet *rs, const StomataParams *p,
		  const StomataKeyList *keys, const StomataVals *rrows, Cand *c)
{
	StomataBits pages;
	StomataVals acc;
	bool		have_exact = false;
	bool		empty = false;

	acc.v = NULL;
	acc.n = 0;
	stomata_bits_init(&pages, rs->npages, true);

	if (rrows)
	{
		acc.v = palloc_extended(sizeof(uint64) * Max(rrows->n, 1), MCXT_ALLOC_HUGE);
		memcpy(acc.v, rrows->v, sizeof(uint64) * rrows->n);
		acc.n = rrows->n;
		have_exact = true;
	}

	/*
	 * Fetch every key's postings from every run first (cheap: a directory
	 * probe and a few buffer reads each), then intersect the exact tier
	 * smallest-first so an empty result is found before large lists are
	 * decoded.  A key missing from every run matches nothing; a stop-key in
	 * any run is treated as TRUE.
	 */
	{
		StomataMulti *po = palloc0(sizeof(StomataMulti) * Max(keys->nkeys, 1));
		int		   *order = palloc(sizeof(int) * Max(keys->nkeys, 1));
		int			nexact = 0;

		for (int i = 0; i < keys->nkeys; i++)
		{
			stomata_lookup_multi(index, rs, &keys->keys[i], &po[i]);
			if (!po[i].found)
			{
				empty = true;
				break;
			}
			if (!po[i].stop && stomata_key_exact(&keys->keys[i], p))
				order[nexact++] = i;
		}
		/* insertion sort by payload length (few keys) */
		for (int x = 1; x < nexact; x++)
			for (int y = x; y > 0 && po[order[y]].len < po[order[y - 1]].len; y--)
			{
				int			t = order[y];

				order[y] = order[y - 1];
				order[y - 1] = t;
			}

		for (int x = 0; x < nexact && !empty; x++)
		{
			StomataVals v;

			stomata_multi_vals(&po[order[x]], &v);
			if (!have_exact)
			{
				acc = v;
				have_exact = true;
			}
			else
			{
				stomata_vals_intersect(&acc, &v);
				pfree(v.v);
			}
			if (acc.n == 0)
				empty = true;
		}
		for (int i = 0; i < keys->nkeys && !empty; i++)
			if (po[i].found && !po[i].stop && !stomata_key_exact(&keys->keys[i], p))
				stomata_multi_pages(&po[i], &pages, true);

		for (int i = 0; i < keys->nkeys; i++)
			stomata_multi_free(&po[i]);
		pfree(po);
		pfree(order);
	}

	if (!empty)
	{
		if (have_exact)
		{
			for (uint32 i = 0; i < acc.n; i++)
			{
				BlockNumber pg = (BlockNumber) (acc.v[i] / STOMATA_EXS);

				if (pg < pages.nbits && ((pages.w[pg >> 6] >> (pg & 63)) & 1))
					cand_add_tid(c, acc.v[i]);
			}
		}
		else
		{
			for (uint32 wi = 0; wi < pages.nwords; wi++)
			{
				uint64		v = pages.w[wi];

				while (v)
				{
					cand_add_page(c, wi * 64 + pg_rightmost_one_pos64(v));
					v &= v - 1;
				}
			}
		}
	}
	if (acc.v)
		pfree(acc.v);
	pfree(pages.w);
}

/* lossy pages of a page-tier pseudo key (A, W) */
static void
add_pseudo_pages(Relation index, const StomataRunSet *rs, uint8 tag, Cand *c)
{
	StomataKey	k;
	StomataMulti mp;
	StomataBits b;

	stomata_pseudo_key(&k, tag);
	stomata_lookup_multi(index, rs, &k, &mp);
	if (!mp.found)
		return;
	stomata_bits_init(&b, rs->npages, false);
	stomata_multi_pages(&mp, &b, false);
	for (uint32 wi = 0; wi < b.nwords; wi++)
	{
		uint64		v = b.w[wi];

		while (v)
		{
			cand_add_page(c, wi * 64 + pg_rightmost_one_pos64(v));
			v &= v - 1;
		}
	}
	pfree(b.w);
	stomata_multi_free(&mp);
}

typedef struct PendLike
{
	const StomataPatterns *pats;
	Cand	   *c;
	text	   *buf;
} PendLike;

/* pending records carry their value: test them exactly with LIKE / ILIKE */
static void
pending_like_visit(BlockNumber blk, OffsetNumber off, const char *val, int len, void *arg)
{
	PendLike   *pl = (PendLike *) arg;

	if (len >= 0 && pl->pats != NULL)
	{
		SET_VARSIZE(pl->buf, len + VARHDRSZ);
		memcpy(VARDATA(pl->buf), val, len);
		for (int i = 0; i < pl->pats->n; i++)
		{
			Datum		r;

			if (!pl->pats->eval[i])
				continue;
			if (pl->pats->icase[i])
				r = DirectFunctionCall2Coll(texticlike, pl->pats->coll[i],
											PointerGetDatum(pl->buf), pl->pats->pat[i]);
			else
				r = DirectFunctionCall2Coll(textlike, pl->pats->coll[i],
											PointerGetDatum(pl->buf), pl->pats->pat[i]);
			if (!DatumGetBool(r))
				return;
		}
	}
	if (off == InvalidOffsetNumber || len < 0)
		cand_add_page(pl->c, blk);
	else
		cand_add_tid(pl->c, stomata_tid_value(blk, off));
}

static BlockNumber
heap_nblocks(Relation index, Relation heap)
{
	BlockNumber nblocks;

	if (heap != NULL)
		return RelationGetNumberOfBlocks(heap);
	else
	{
		/* bitmap scans do not carry the heap relation */
		Oid			heapoid = IndexGetRelation(RelationGetRelid(index), false);
		Relation	h = table_open(heapoid, AccessShareLock);

		nblocks = RelationGetNumberOfBlocks(h);
		table_close(h, AccessShareLock);
	}
	return nblocks;
}

/*
 * Compute the candidates of a query and add them to tbm (exact TIDs with
 * recheck, or lossy pages), or just count them when tbm is NULL.  Returns
 * the number of distinct heap pages involved.
 */
/* tuples emitted by the last stomata_candidates() call (EXPLAIN rows) */
static int64 stomata_last_ntuples = 0;

static int64
lossy_tuple_estimate(Relation heap, int64 nlossy)
{
	double		tpp = 10.0;

	if (nlossy <= 0)
		return 0;
	if (heap && heap->rd_rel->relpages > 0 && heap->rd_rel->reltuples > 0)
		tpp = heap->rd_rel->reltuples / heap->rd_rel->relpages;
	return (int64) (nlossy * tpp + 0.5);
}

int64
stomata_candidates(Relation index, Relation heap, const StomataQuery *q, TIDBitmap *tbm)
{
	StomataMetaPageData meta;
	StomataRunSet rs;
	Cand		c;
	int64		npages = 0;
	int64		ntuples = 0;
	int64		nlossy = 0;

	stomata_last_ntuples = 0;
	if (q->impossible)
		return 0;
	if (q->full_scan)
	{
		BlockNumber nblocks = heap_nblocks(index, heap);

		if (tbm)
			for (BlockNumber b = 0; b < nblocks; b++)
				tbm_add_page(tbm, b);
		stomata_last_ntuples = lossy_tuple_estimate(heap, nblocks);
		return nblocks;
	}

	/*
	 * Copy the metapage and let go of it: runs are immutable and replaced
	 * pages are not recycled while our snapshot is alive, so everything the
	 * copy points to stays readable without holding the lock.
	 */
	memset(&c, 0, sizeof(c));
	stomata_read_meta(index, &meta);
	stomata_get_runset(index, &meta, &rs);
	for (int ms = 0; ms < stomata_test_scan_delay_ms; ms += 10)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(10000L);
	}

	eval_conj(index, &rs, &meta.params, &q->all, NULL, &c);

	/*
	 * ILIKE: rows with non-ASCII bytes may fold differently from the index,
	 * so they only have to satisfy the case-sensitive (LIKE) conditions.
	 */
	if (q->pats.any_icase)
	{
		StomataKey	nk;
		StomataMulti mp;

		stomata_pseudo_key(&nk, SK_TAG_N);
		stomata_lookup_multi(index, &rs, &nk, &mp);
		if (mp.found)
		{
			StomataVals nv;

			stomata_multi_vals(&mp, &nv);
			eval_conj(index, &rs, &meta.params, &q->like_only, &nv, &c);
			pfree(nv.v);
		}
		stomata_multi_free(&mp);
	}

	add_pseudo_pages(index, &rs, SK_TAG_A, &c);
	add_pseudo_pages(index, &rs, SK_TAG_W, &c);

	{
		PendLike	pl;

		pl.pats = &q->pats;
		pl.c = &c;
		pl.buf = palloc(STOMATA_PENDING_MAXVAL + VARHDRSZ);
		/* the list a merge is folding in, then the active one */
		stomata_pending_scan(index, meta.frozen_head, pending_like_visit, &pl);
		stomata_pending_scan(index, meta.pending_head, pending_like_visit, &pl);
		pfree(pl.buf);
	}

	/* emit: lossy pages, then exact TIDs grouped by page */
	{
		StomataBits seen;
		uint32		maxpage = c.lossy.nbits;
		ItemPointerData *tids = palloc(sizeof(ItemPointerData) * MaxHeapTuplesPerPage);
		int			n = 0;
		BlockNumber cur = InvalidBlockNumber;

		if (c.ntids > 1)
			qsort(c.tids, c.ntids, sizeof(uint64), u64cmp);
		if (c.ntids > 0)
			maxpage = Max(maxpage, (uint32) (c.tids[c.ntids - 1] / STOMATA_EXS) + 1);
		stomata_bits_init(&seen, Max(maxpage, 1), false);

		for (uint32 wi = 0; wi < c.lossy.nwords; wi++)
		{
			uint64		v = c.lossy.w[wi];

			while (v)
			{
				BlockNumber b = wi * 64 + pg_rightmost_one_pos64(v);

				v &= v - 1;
				seen.w[b >> 6] |= UINT64CONST(1) << (b & 63);
				nlossy++;
				if (tbm)
					tbm_add_page(tbm, b);
			}
		}
		for (uint32 i = 0; i < c.ntids; i++)
		{
			BlockNumber b = (BlockNumber) (c.tids[i] / STOMATA_EXS);
			OffsetNumber o = (OffsetNumber) (c.tids[i] % STOMATA_EXS) + 1;

			if (i > 0 && c.tids[i] == c.tids[i - 1])
				continue;
			if (!(b < c.lossy.nbits &&
				  (c.lossy.w[b >> 6] & (UINT64CONST(1) << (b & 63)))))
				ntuples++;
			if (b != cur)
			{
				if (tbm && n > 0)
					tbm_add_tuples(tbm, tids, n, true);
				n = 0;
				cur = b;
			}
			seen.w[b >> 6] |= UINT64CONST(1) << (b & 63);
			if (n < MaxHeapTuplesPerPage)
				ItemPointerSet(&tids[n++], b, o);
		}
		if (tbm && n > 0)
			tbm_add_tuples(tbm, tids, n, true);
		for (uint32 wi = 0; wi < seen.nwords; wi++)
			npages += pg_popcount64(seen.w[wi]);
		stomata_last_ntuples = ntuples + lossy_tuple_estimate(heap, nlossy);
		pfree(tids);
		pfree(seen.w);
	}
	if (c.tids)
		pfree(c.tids);
	if (c.lossy.w)
		pfree(c.lossy.w);
	return npages;
}

/*
 * Compile all scan keys.  Sets impossible for NULL patterns and full_scan
 * when no key restricts the result.
 */
static void
compile_scankeys(Relation index, ScanKey skeys, int nkeys, StomataQuery *q)
{
	StomataMetaPageData meta;

	stomata_read_meta(index, &meta);
	memset(q, 0, sizeof(*q));
	stomata_keylist_init(&q->all);
	stomata_keylist_init(&q->like_only);
	q->pats.pat = palloc(sizeof(Datum) * Max(nkeys, 1));
	q->pats.coll = palloc(sizeof(Oid) * Max(nkeys, 1));
	q->pats.icase = palloc(sizeof(bool) * Max(nkeys, 1));
	q->pats.eval = palloc(sizeof(bool) * Max(nkeys, 1));

	for (int i = 0; i < nkeys; i++)
	{
		ScanKey		sk = &skeys[i];
		text	   *pat;
		bool		icase;
		int			pi;

		if (sk->sk_flags & SK_ISNULL)
		{
			q->impossible = true;
			return;
		}
		if (sk->sk_strategy != STOMATA_LIKE_STRATEGY && sk->sk_strategy != STOMATA_ILIKE_STRATEGY)
			elog(ERROR, "stomata: unsupported strategy %d", sk->sk_strategy);
		icase = (sk->sk_strategy == STOMATA_ILIKE_STRATEGY);

		pi = q->pats.n++;
		q->pats.pat[pi] = sk->sk_argument;
		q->pats.coll[pi] = OidIsValid(sk->sk_collation) ? sk->sk_collation : DEFAULT_COLLATION_OID;
		q->pats.icase[pi] = icase;
		q->pats.eval[pi] = true;
		if (icase)
			q->pats.any_icase = true;

		/* bytewise reasoning is only valid for deterministic collations */
		if (OidIsValid(sk->sk_collation) && !get_collation_isdeterministic(sk->sk_collation))
		{
			q->pats.eval[pi] = false;
			continue;
		}
		pat = DatumGetTextPP(sk->sk_argument);
		stomata_compile_like(VARDATA_ANY(pat), VARSIZE_ANY_EXHDR(pat), &meta.params, icase, &q->all);
		if (!icase)
			stomata_compile_like(VARDATA_ANY(pat), VARSIZE_ANY_EXHDR(pat), &meta.params, false,
								 &q->like_only);
	}
	stomata_keylist_sort_unique(&q->all);
	stomata_keylist_sort_unique(&q->like_only);
	q->full_scan = (q->all.nkeys == 0);
}

static int64
stomata_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	StomataQuery q;
	int64		npages;

	pgstat_count_index_scan(scan->indexRelation);
	compile_scankeys(scan->indexRelation, scan->keyData, scan->numberOfKeys, &q);
	npages = stomata_candidates(scan->indexRelation, scan->heapRelation, &q, tbm);
	(void) npages;
	return stomata_last_ntuples;	/* exact TIDs + estimate for lossy pages */
}

/* ------------------------------------------------------------------------
 * Handler
 * ------------------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(stomata_handler);

Datum
stomata_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 2;
	amroutine->amsupport = 0;
	amroutine->amoptsprocnum = 0;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = true;	/* HOT stays possible; see stomata_insert */
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;
#if PG_VERSION_NUM >= 170000
	amroutine->amcanbuildparallel = false;
	amroutine->aminsertcleanup = NULL;
#endif

	amroutine->ambuild = stomata_build;
	amroutine->ambuildempty = stomata_buildempty;
	amroutine->aminsert = stomata_insert;
	amroutine->ambulkdelete = stomata_bulkdelete;
	amroutine->amvacuumcleanup = stomata_vacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = stomata_costestimate;
	amroutine->amoptions = stomata_options;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = stomata_validate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = stomata_beginscan;
	amroutine->amrescan = stomata_rescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = stomata_getbitmap;
	amroutine->amendscan = stomata_endscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/* exported for funcs.c */
bool		stomata_is_stomata_index(Relation index);

bool
stomata_is_stomata_index(Relation index)
{
	return index->rd_rel->relkind == RELKIND_INDEX &&
		index->rd_indam != NULL &&
		index->rd_indam->amgetbitmap == stomata_getbitmap;
}

void		stomata_compile_for_index(Relation index, text *pattern, bool icase,
									  StomataQuery *q);

void
stomata_compile_for_index(Relation index, text *pattern, bool icase, StomataQuery *q)
{
	ScanKeyData sk;

	memset(&sk, 0, sizeof(sk));
	sk.sk_attno = 1;
	sk.sk_strategy = icase ? STOMATA_ILIKE_STRATEGY : STOMATA_LIKE_STRATEGY;
	sk.sk_collation = InvalidOid;
	sk.sk_argument = PointerGetDatum(pattern);
	compile_scankeys(index, &sk, 1, q);
}
