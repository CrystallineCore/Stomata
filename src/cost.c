/*-------------------------------------------------------------------------
 *
 * cost.c
 *	  Planner cost estimation from the index's own statistics.
 *
 * The planner compares a stomata bitmap scan with a sequential scan and with
 * other indexes, so the estimate has to be in the same currency as theirs:
 * pages at seq_page_cost / random_page_cost, work at cpu_operator_cost, heap
 * access left to cost_bitmap_heap_scan.  What only the access method can
 * supply is how many heap rows and pages the bitmap will hold, and that is
 * where the estimate comes from:
 *
 *   1. Every constant pattern is compiled with the index's parameters, and
 *      each key is looked up in the runs (directory probe, payload counted in
 *      place, never decoded) - the number of TIDs of an exact-tier key, the
 *      number of heap pages of a page-tier key.
 *   2. The page-tier keys are ANDed exactly (a bitmap of a few kB per key).
 *      The exact-tier lists are intersected rarest first, as the scan does,
 *      while the values decoded stay within stomata.estimate_budget.  Lists
 *      left out can only remove candidates, so the count is exact for
 *      selective patterns and an upper bound otherwise.
 *   3. When even the rarest list is over the budget (every key common), the
 *      pattern is split into its literal pieces ('a%son' -> 'a%', '%son'):
 *      pieces are independent, and a piece allows at most as many rows as
 *      its rarest key has.
 *   4. Exact-tier candidates are rows; without exact-tier keys the candidates
 *      are whole (lossy) pages, every row of which is rechecked.
 *
 * indexSelectivity is the fraction of heap rows in the bitmap (candidates,
 * not matches), which is what cost_bitmap_heap_scan needs.  The index cost is
 * the pages read (a random page per key and run, sequential pages for the
 * rest of a long posting list), the values decoded, and the TIDs emitted.
 *
 * Per-key statistics and evaluated conjunctions are cached per backend for
 * the current run list, so planning the same pattern again costs a hash
 * lookup.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_collation.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include <math.h>

#include "stomata.h"

/* ------------------------------------------------------------------------
 * Per-backend key statistics cache
 * ------------------------------------------------------------------------
 */
typedef struct KeyStatTag
{
	Oid			relid;
	RelFileNumber relnumber;
	uint32		generation;
	StomataKey	key;
} KeyStatTag;

typedef struct KeyStat
{
	KeyStatTag	tag;			/* hash key */
	uint32		nfound;			/* runs holding the key */
	bool		stop;			/* a stop-key in some run */
	bool		exact;
	uint32		pages;			/* blob pages read, all runs */
	uint64		bytes;
	uint64		count;			/* TIDs (exact) or pages (page tier), summed */
} KeyStat;

#define KEYSTAT_CACHE_MAX	65536

static HTAB *keystat_cache = NULL;

static void
keystat_cache_reset(void)
{
	HASHCTL		ctl;

	if (keystat_cache)
		hash_destroy(keystat_cache);
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(KeyStatTag);
	ctl.entrysize = sizeof(KeyStat);
	ctl.hcxt = TopMemoryContext;
	keystat_cache = hash_create("stomata key statistics", 1024, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static const KeyStat *
key_stat(Relation index, const StomataMetaPageData *meta, const StomataRunSet *rs,
		 const StomataKey *key)
{
	KeyStatTag	tag;
	KeyStat    *e;
	bool		found;

	if (keystat_cache == NULL || hash_get_num_entries(keystat_cache) >= KEYSTAT_CACHE_MAX)
		keystat_cache_reset();

	memset(&tag, 0, sizeof(tag));
	tag.relid = RelationGetRelid(index);
	tag.relnumber = index->rd_locator.relNumber;
	tag.generation = meta->generation;
	tag.key.len = key->len;
	memcpy(tag.key.data, key->data, key->len);

	e = hash_search(keystat_cache, &tag, HASH_ENTER, &found);
	if (!found)
	{
		e->nfound = 0;
		e->stop = false;
		e->pages = 0;
		e->bytes = 0;
		e->count = 0;
		e->exact = stomata_key_exact(key, &meta->params);
		PG_TRY();
		{
			for (int r = 0; r < rs->nruns; r++)
			{
				StomataKeyStats ks;

				stomata_lookup_stats(index, rs->img[r], key, &ks);
				e->pages += ks.pages;
				if (!ks.found)
					continue;
				e->nfound++;
				e->bytes += ks.bytes;
				if (ks.ctype == STOMATA_CT_STOP)
					e->stop = true;
				else
					e->count += ks.count;
			}
		}
		PG_CATCH();
		{
			hash_search(keystat_cache, &tag, HASH_REMOVE, NULL);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	return e;
}

/* ------------------------------------------------------------------------
 * Estimation
 * ------------------------------------------------------------------------
 */

int			stomata_estimate_budget = 50000;

/* decoding one posting value, as a fraction of cpu_operator_cost */
#define STOMATA_DECODE_COST		0.25

typedef struct EstCtx
{
	Relation	index;
	StomataMetaPageData meta;
	StomataRunSet rs;
	double		nrows;			/* rows in the runs */
	double		npages;			/* heap pages the runs cover */
} EstCtx;

/* one conjunction: its keys, the statistical fallback, and the scan's work */
typedef struct Est
{
	bool		empty;			/* some required key exists nowhere */
	bool		icase;			/* some ILIKE: rows marked N are candidates */
	StomataKeyList keys;		/* every key of every pattern */
	/* fallback: product over literal pieces */
	double		exact_frac;
	bool		have_exact;
	double		page_frac;
	bool		have_page;
	/* work of the scan */
	double		probes;			/* random index page reads */
	double		seqpages;		/* sequential index page reads */
	double		decoded;		/* values decoded */
	double		pagekeys;		/* page-tier keys ANDed (bitmap words each) */
	int			npatterns;
} Est;

static void
est_init(Est *e)
{
	memset(e, 0, sizeof(*e));
	stomata_keylist_init(&e->keys);
	e->exact_frac = 1.0;
	e->page_frac = 1.0;
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a;
	double		y = *(const double *) b;

	return (x > y) - (x < y);
}

/*
 * Fraction of rows (exact tier) and of pages (page tier) that one literal
 * piece allows: its rarest key.  The keys of one literal are strongly
 * correlated (every row with 'anderson' has 'son'), so the rarest key is a
 * tight upper bound far more often than a product is a good estimate.
 * Returns false if a key exists nowhere.
 */
static bool
piece_fractions(EstCtx *cx, const StomataKeyList *kl, double *fe, double *fp)
{
	*fe = -1;
	*fp = -1;
	for (int i = 0; i < kl->nkeys; i++)
	{
		const KeyStat *ks = key_stat(cx->index, &cx->meta, &cx->rs, &kl->keys[i]);
		double		f;

		if (ks->stop)
			continue;
		if (ks->nfound == 0)
			return false;
		if (ks->exact)
		{
			f = Min((double) ks->count / cx->nrows, 1.0);
			*fe = (*fe < 0) ? f : Min(*fe, f);
		}
		else
		{
			f = Min((double) ks->count / cx->npages, 1.0);
			*fp = (*fp < 0) ? f : Min(*fp, f);
		}
	}
	return true;
}

/*
 * Split a LIKE pattern at unescaped '%' into pieces, each rebuilt as a
 * pattern with its own anchoring.
 */
static List *
split_pieces(const char *pat, int len)
{
	List	   *out = NIL;
	int			start = 0;
	int			i = 0;
	bool		lead = (len == 0 || pat[0] != '%');

	while (i <= len)
	{
		if (i == len || pat[i] == '%')
		{
			if (i > start)
			{
				bool		anchored_start = (start == 0) && lead;
				bool		anchored_end = (i == len);
				StringInfoData s;

				initStringInfo(&s);
				if (!anchored_start)
					appendStringInfoChar(&s, '%');
				appendBinaryStringInfo(&s, pat + start, i - start);
				if (!anchored_end)
					appendStringInfoChar(&s, '%');
				out = lappend(out, s.data);
			}
			start = i + 1;
			i++;
			continue;
		}
		if (pat[i] == '\\' && i + 1 < len)
			i++;				/* escaped character: part of the literal */
		i++;
	}
	return out;
}

/* add one pattern (a conjunct) to the estimate */
static void
est_add_pattern(EstCtx *cx, Est *e, const char *pat, int len, bool icase)
{
	StomataKeyList whole;
	List	   *pieces;
	ListCell   *lc;

	e->npatterns++;
	if (icase)
		e->icase = true;

	/* what the scan does: every key of the pattern */
	stomata_keylist_init(&whole);
	stomata_compile_like(pat, len, &cx->meta.params, icase, &whole);
	stomata_keylist_sort_unique(&whole);
	for (int i = 0; i < whole.nkeys; i++)
	{
		const KeyStat *ks = key_stat(cx->index, &cx->meta, &cx->rs, &whole.keys[i]);

		stomata_keylist_add(&e->keys, &whole.keys[i]);
		/* a probe per run; the rest of a long list is read sequentially */
		e->probes += cx->rs.nruns;
		e->seqpages += Max((double) ks->pages - cx->rs.nruns, 0);
		if (ks->exact)
			e->decoded += (double) ks->count;
		else
		{
			e->pagekeys += 1;
			if (ks->bytes > 0 && ks->bytes < cx->npages / 8.0)
				e->decoded += (double) ks->count;	/* array containers */
		}
		if (!ks->stop && ks->nfound == 0)
			e->empty = true;
	}
	if (e->empty || whole.nkeys == 0)
		return;

	/* the fallback: independent pieces */
	pieces = split_pieces(pat, len);
	if (list_length(pieces) <= 1)
		pieces = list_make1(pnstrdup(pat, len));
	foreach(lc, pieces)
	{
		char	   *sp = (char *) lfirst(lc);
		StomataKeyList kl;
		double		fe,
					fp;

		stomata_keylist_init(&kl);
		stomata_compile_like(sp, strlen(sp), &cx->meta.params, icase, &kl);
		stomata_keylist_sort_unique(&kl);
		if (kl.nkeys == 0 || !piece_fractions(cx, &kl, &fe, &fp))
			continue;
		if (fe >= 0)
		{
			e->exact_frac *= fe;
			e->have_exact = true;
		}
		else if (fp >= 0)
		{
			e->page_frac *= fp;
			e->have_page = true;
		}
	}
}

/* ------------------------------------------------------------------------
 * Evaluating a conjunction at plan time
 * ------------------------------------------------------------------------
 */
typedef struct EvalTag
{
	Oid			relid;
	RelFileNumber relnumber;
	uint32		generation;
	uint32		nkeys;
	uint64		hash;
} EvalTag;

typedef struct EvalEntry
{
	EvalTag		tag;
	double		tids;
	double		lossy;
	bool		exact;			/* fully evaluated */
	int			budget;			/* evaluated under this budget */
} EvalEntry;

static HTAB *eval_cache = NULL;

typedef struct ExactKey
{
	int			i;
	double		count;
} ExactKey;

static int
cmp_exactkey(const void *a, const void *b)
{
	return cmp_double(&((const ExactKey *) a)->count, &((const ExactKey *) b)->count);
}

/*
 * The scan's candidate computation, for the keys whose postings fit in the
 * budget: page-tier keys ANDed (always; a bitmap is npages/8 bytes), the
 * exact-tier lists intersected rarest first while the values decoded stay
 * within the budget.  Keys left out are applied statistically.  Sets
 * *complete when every key was applied.
 */
static void
evaluate(EstCtx *cx, Est *e, double *tids, double *lossy, bool *complete)
{
	StomataKeyList *kl = &e->keys;
	StomataBits pages;
	ExactKey   *ex;
	int			nex = 0;
	double		used = 0;
	MemoryContext cxt,
				old;
	EvalTag		tag;
	EvalEntry  *ce;
	bool		found;

	stomata_keylist_sort_unique(kl);

	/* cached? */
	if (eval_cache == NULL || hash_get_num_entries(eval_cache) >= KEYSTAT_CACHE_MAX)
	{
		HASHCTL		ctl;

		if (eval_cache)
			hash_destroy(eval_cache);
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(EvalTag);
		ctl.entrysize = sizeof(EvalEntry);
		ctl.hcxt = TopMemoryContext;
		eval_cache = hash_create("stomata evaluated conjunctions", 256, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	memset(&tag, 0, sizeof(tag));
	tag.relid = RelationGetRelid(cx->index);
	tag.relnumber = cx->index->rd_locator.relNumber;
	tag.generation = cx->meta.generation;
	tag.nkeys = kl->nkeys;
	tag.hash = hash_bytes_extended((const unsigned char *) kl->keys,
								   sizeof(StomataKey) * kl->nkeys, 0);
	ce = hash_search(eval_cache, &tag, HASH_FIND, NULL);
	if (ce && ce->budget == stomata_estimate_budget)
	{
		*tids = ce->tids;
		*lossy = ce->lossy;
		*complete = ce->exact;
		return;
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext, "stomata estimate", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);

	stomata_bits_init(&pages, cx->rs.npages, true);
	ex = palloc(sizeof(ExactKey) * Max(kl->nkeys, 1));
	*complete = true;
	for (int i = 0; i < kl->nkeys; i++)
	{
		const KeyStat *ks = key_stat(cx->index, &cx->meta, &cx->rs, &kl->keys[i]);

		if (ks->stop)
			continue;
		if (ks->exact)
		{
			ex[nex].i = i;
			ex[nex].count = (double) ks->count;
			nex++;
		}
		else
		{
			StomataMulti mp;

			stomata_lookup_multi(cx->index, &cx->rs, &kl->keys[i], &mp);
			stomata_multi_pages(&mp, &pages, true);
			stomata_multi_free(&mp);
		}
	}

	if (nex == 0)
	{
		uint64		n = 0;

		for (uint32 w = 0; w < pages.nwords; w++)
			n += pg_popcount64(pages.w[w]);
		*tids = 0;
		*lossy = (double) n;
	}
	else
	{
		StomataVals acc = {0};
		int			k = 0;
		double		t;

		qsort(ex, nex, sizeof(ExactKey), cmp_exactkey);
		for (; k < nex; k++)
		{
			StomataMulti mp;
			StomataVals v;

			if (used + ex[k].count > stomata_estimate_budget || (k > 0 && acc.n == 0))
				break;
			stomata_lookup_multi(cx->index, &cx->rs, &kl->keys[ex[k].i], &mp);
			stomata_multi_vals(&mp, &v);
			stomata_multi_free(&mp);
			used += ex[k].count;
			if (k == 0)
				acc = v;
			else
				stomata_vals_intersect(&acc, &v);
		}
		if (k == 0)
		{
			/* the rarest list is over the budget: statistics only */
			*complete = false;
			t = cx->nrows * e->exact_frac * (e->have_page ? e->page_frac : 1.0);
			t = Min(t, ex[0].count);
		}
		else
		{
			uint64		n = 0;

			for (uint32 j = 0; j < acc.n; j++)
			{
				BlockNumber pg = (BlockNumber) (acc.v[j] / STOMATA_EXS);

				if (pg < pages.nbits && ((pages.w[pg >> 6] >> (pg & 63)) & 1))
					n++;
			}
			/*
			 * Lists left out can only remove candidates: an upper bound.
			 * The statistical estimate may be tighter.
			 */
			t = (double) n;
			if (acc.n > 0 && k < nex)
			{
				double		st = cx->nrows * e->exact_frac * (e->have_page ? e->page_frac : 1.0);

				*complete = false;
				t = Min(t, Max(st, 1));
			}
		}
		*tids = t;
		*lossy = 0;
	}

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);

	ce = hash_search(eval_cache, &tag, HASH_ENTER, &found);
	ce->tids = *tids;
	ce->lossy = *lossy;
	ce->exact = *complete;
	ce->budget = stomata_estimate_budget;
}

/* candidates of a finished conjunction, in index rows and index pages */
static void
est_result(EstCtx *cx, Est *e, double *tids, double *lossy, bool *complete)
{
	*tids = 0;
	*lossy = 0;
	*complete = true;
	if (e->empty)
		return;
	if (e->keys.nkeys == 0)
	{
		*lossy = cx->npages;	/* full scan */
		return;
	}
	evaluate(cx, e, tids, lossy, complete);
}

/* count of a pseudo key (A, W: pages; N: rows) */
static double
pseudo_count(EstCtx *cx, uint8 tag)
{
	StomataKey	k;

	stomata_pseudo_key(&k, tag);
	return (double) key_stat(cx->index, &cx->meta, &cx->rs, &k)->count;
}

/* ------------------------------------------------------------------------
 * Clause extraction
 * ------------------------------------------------------------------------
 */
typedef struct PatClause
{
	bool		icase;
	bool		usable;			/* constant pattern under a deterministic collation */
	List	   *elems;			/* text * patterns (several for = ANY) */
	bool		is_array;
} PatClause;

static bool
collation_ok(Oid coll)
{
	return !OidIsValid(coll) || get_collation_isdeterministic(coll);
}

static bool
extract_clauses(IndexPath *path, List **out)
{
	IndexOptInfo *index = path->indexinfo;
	ListCell   *lc;
	int			narrays = 0;

	*out = NIL;
	foreach(lc, path->indexclauses)
	{
		IndexClause *ic = lfirst_node(IndexClause, lc);
		ListCell   *lq;

		foreach(lq, ic->indexquals)
		{
			RestrictInfo *ri = lfirst_node(RestrictInfo, lq);
			Expr	   *clause = ri->clause;
			PatClause  *pc = palloc0(sizeof(PatClause));
			Oid			opno;
			Oid			coll;
			Node	   *rhs;
			int			strategy;

			if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
			{
				opno = ((OpExpr *) clause)->opno;
				coll = ((OpExpr *) clause)->inputcollid;
				rhs = strip_implicit_coercions(lsecond(((OpExpr *) clause)->args));
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;

				if (!sa->useOr)
					return false;
				opno = sa->opno;
				coll = sa->inputcollid;
				rhs = strip_implicit_coercions(lsecond(sa->args));
				pc->is_array = true;
				narrays++;
			}
			else
				return false;

			strategy = get_op_opfamily_strategy(opno, index->opfamily[0]);
			if (strategy != STOMATA_LIKE_STRATEGY && strategy != STOMATA_ILIKE_STRATEGY)
				return false;
			pc->icase = (strategy == STOMATA_ILIKE_STRATEGY);
			if (!IsA(rhs, Const))
				return false;	/* parameter: estimate from statistics */
			if (((Const *) rhs)->constisnull)
				pc->usable = true;	/* matches nothing: no elements */
			else if (!collation_ok(coll))
				pc->usable = false; /* restricts nothing */
			else
			{
				pc->usable = true;
				if (pc->is_array)
				{
					ArrayType  *arr = DatumGetArrayTypeP(((Const *) rhs)->constvalue);
					Datum	   *elems;
					bool	   *nulls;
					int			n;

					deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
					for (int i = 0; i < n; i++)
						if (!nulls[i])
							pc->elems = lappend(pc->elems, DatumGetTextPP(elems[i]));
				}
				else
					pc->elems = list_make1(DatumGetTextPP(((Const *) rhs)->constvalue));
			}
			*out = lappend(*out, pc);
		}
	}
	/* one = ANY clause is a union of scans; more would multiply out */
	return *out != NIL && narrays <= 1;
}

/* ------------------------------------------------------------------------
 * amcostestimate
 * ------------------------------------------------------------------------
 */
void
stomata_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					 Cost *indexStartupCost, Cost *indexTotalCost,
					 Selectivity *indexSelectivity, double *indexCorrelation,
					 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	double		heap_rows = Max(baserel->tuples, 1);
	double		heap_pages = Max(baserel->pages, 1);
	double		tpp = heap_rows / heap_pages;
	double		spc_random_page_cost;
	double		spc_seq_page_cost;
	List	   *clauses;
	EstCtx		cx;
	Relation	irel;
	double		tids = 0,
				lossy = 0,
				io,
				cpu;
	Est			work;
	PatClause  *arrclause = NULL;
	ListCell   *lc;
	int			nscans = 1;

	get_tablespace_page_costs(index->reltablespace, &spc_random_page_cost, &spc_seq_page_cost);
	*indexCorrelation = 0;
	*indexPages = index->pages;
	*indexStartupCost = 0;

	/*
	 * Reading the runs needs a snapshot: it keeps the pages of runs that a
	 * concurrent merge retires from being reused (see stomata_candidates).
	 * Planning normally has one; without it, fall back to statistics.
	 */
	if (!ActiveSnapshotSet() || !extract_clauses(path, &clauses))
	{
		stomata_costestimate_generic(root, path, loop_count, indexStartupCost, indexTotalCost,
									 indexSelectivity);
		return;
	}

	irel = index_open(index->indexoid, NoLock);
	cx.index = irel;
	stomata_read_meta(irel, &cx.meta);
	stomata_get_runset(irel, &cx.meta, &cx.rs);
	cx.nrows = 0;
	for (uint32 r = 0; r < cx.meta.nruns; r++)
		cx.nrows += cx.meta.runs[r].rows;
	cx.nrows = Max(cx.nrows, 1);
	cx.npages = Max((double) cx.meta.npages, 1);

	foreach(lc, clauses)
		if (((PatClause *) lfirst(lc))->is_array)
			arrclause = lfirst(lc);

	/*
	 * Work and candidates of each scan.  = ANY runs one scan per element
	 * and ORs the bitmaps.
	 */
	est_init(&work);
	{
		List	   *arrelems = arrclause ? arrclause->elems : list_make1(NULL);
		ListCell   *le;

		nscans = Max(list_length(arrelems), 1);
		if (arrclause && arrelems == NIL)
			nscans = 0;			/* empty array: nothing to scan */
		foreach(le, arrelems)
		{
			Est			e;
			double		t,
						l;
			bool		nothing = false;
			bool		complete;

			est_init(&e);
			foreach(lc, clauses)
			{
				PatClause  *pc = lfirst(lc);
				text	   *pat;

				if (!pc->usable)
					continue;
				if (pc == arrclause)
					pat = (text *) lfirst(le);
				else if (pc->elems == NIL)
				{
					nothing = true; /* NULL pattern */
					break;
				}
				else
					pat = (text *) linitial(pc->elems);
				est_add_pattern(&cx, &e, VARDATA_ANY(pat), VARSIZE_ANY_EXHDR(pat), pc->icase);
			}
			if (nothing)
				continue;
			est_result(&cx, &e, &t, &l, &complete);
			if (e.icase)
				t += pseudo_count(&cx, SK_TAG_N);
			tids += t;
			lossy += l;
			work.probes += e.probes;
			work.seqpages += e.seqpages;
			work.decoded += e.decoded;
			work.pagekeys += e.pagekeys;
			work.npatterns = Max(work.npatterns, e.npatterns);
		}
	}

	/* pages the scan always returns (long values, unmapped HOT tuples) */
	if (nscans > 0)
	{
		lossy += pseudo_count(&cx, SK_TAG_A) + pseudo_count(&cx, SK_TAG_W);
		work.probes += 2 * cx.rs.nruns;
	}
	lossy = Min(lossy, cx.npages);
	tids = Min(tids, cx.nrows);

	/*
	 * Counts are absolute (rows and pages of the heap), which is also right
	 * for a partial index; rows added since the last merge are in the
	 * pending list, below.
	 */
	lossy = Min(lossy, heap_pages);

	/* pending records are tested with LIKE itself: matches only */
	{
		double		pend = (double) cx.meta.pending_records + cx.meta.frozen_records;
		double		ppages = (double) cx.meta.pending_pages + cx.meta.frozen_pages;
		double		frac = Min((tids + lossy * tpp) / heap_rows, 1.0);

		tids += pend * frac;
		work.seqpages += ppages * nscans;
		cpu = pend * nscans * (cpu_tuple_cost + Max(work.npatterns, 1) * cpu_operator_cost);
	}

	*indexSelectivity = Min((tids + lossy * tpp) / heap_rows, 1.0);

	/*
	 * Index cost: a random read per key probe, sequential reads for the rest
	 * of long posting lists, and CPU for the values decoded, the page-tier
	 * bitmaps ANDed (64 pages a word) and the TIDs emitted.  The CPU rates
	 * are measured against what PostgreSQL charges elsewhere: decoding a
	 * varbyte delta takes about 4 ns, a quarter of an operator call (a LIKE
	 * evaluation, cpu_operator_cost); sorting and adding a TID to the bitmap
	 * about 25 ns, what btree charges per index tuple (cpu_index_tuple_cost).
	 */
	io = work.probes * spc_random_page_cost + work.seqpages * spc_seq_page_cost;
	if (work.probes + work.seqpages > index->pages && index->pages > 0)
	{
		/* a small index: every page is read at most once */
		io *= index->pages / (work.probes + work.seqpages);
	}
	if (loop_count > 1)
	{
		/* repeated scans find their pages cached, as in genericcostestimate */
		double		pages = work.probes + work.seqpages;
		double		fetched = index_pages_fetched(pages * loop_count, index->pages,
												  index->pages, root);

		if (pages > 0)
			io *= fetched / (pages * loop_count);
	}
	cpu += work.decoded * cpu_operator_cost * STOMATA_DECODE_COST +
		work.pagekeys * (cx.npages / 64.0) * cpu_operator_cost +
		tids * cpu_index_tuple_cost +
		lossy * cpu_operator_cost;
	*indexTotalCost = io + cpu;

	/*
	 * Rows on lossy pages are all visited by the bitmap heap scan, which
	 * cost_bitmap_heap_scan charges like fetched tuples.  Per row, that path
	 * (visibility check and recheck of every tuple) costs about twice what a
	 * sequential scan spends; charge the difference here.
	 */
	*indexTotalCost += lossy * tpp * cpu_tuple_cost;

	index_close(irel, NoLock);
}

/* ------------------------------------------------------------------------
 * stomata_estimate(): the planner's view of one pattern, for diagnostics
 * ------------------------------------------------------------------------
 */
void
stomata_estimate_pattern(Relation index, text *pattern, bool icase, StomataEstimate *out)
{
	EstCtx		cx;
	Est			e;
	double		t,
				l;
	bool		complete;

	cx.index = index;
	stomata_read_meta(index, &cx.meta);
	stomata_get_runset(index, &cx.meta, &cx.rs);
	cx.nrows = 0;
	for (uint32 r = 0; r < cx.meta.nruns; r++)
		cx.nrows += cx.meta.runs[r].rows;
	cx.nrows = Max(cx.nrows, 1);
	cx.npages = Max((double) cx.meta.npages, 1);

	est_init(&e);
	est_add_pattern(&cx, &e, VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern), icase);
	est_result(&cx, &e, &t, &l, &complete);
	if (e.icase)
		t += pseudo_count(&cx, SK_TAG_N);
	l += pseudo_count(&cx, SK_TAG_A) + pseudo_count(&cx, SK_TAG_W);
	out->tids = Min(t, cx.nrows);
	out->lossy_pages = Min(l, cx.npages);
	out->complete = complete;
	out->keys = e.keys.nkeys;
	out->values_decoded = e.decoded;
	out->index_pages = e.probes + e.seqpages;
	out->rows = cx.nrows;
	out->pages = cx.npages;
}
