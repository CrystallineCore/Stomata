/*-------------------------------------------------------------------------
 *
 * funcs.c
 *	  SQL-callable diagnostics and maintenance functions.
 *
 *	  stomata_keys(value, ...)              keys the index stores for a value
 *	  stomata_pattern_keys(pattern, ...)    keys a LIKE / ILIKE pattern requires
 *	  stomata_index_info(index)             metapage summary
 *	  stomata_key_stats(index)              dictionary size by key family and tier
 *	  stomata_candidate_pages(index, p, i)  heap pages a scan would touch
 *	  stomata_runs(index)                   the sorted runs, oldest first
 *	  stomata_merge_pending(index)          fold the pending list now
 *	  stomata_compact(index)                merge everything into one run
 *	  stomata_verify(index)                 live tuples the index fails to cover
 *	  stomata_pattern_stats(index, p, i)    postings of each key a pattern needs
 *	  stomata_estimate(index, p, i)         the planner's candidate estimate
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "stomata.h"

extern bool stomata_is_stomata_index(Relation index);
extern void stomata_compile_for_index(Relation index, text *pattern, bool icase,
									  StomataQuery *q);

PG_FUNCTION_INFO_V1(stomata_keys);
PG_FUNCTION_INFO_V1(stomata_pattern_keys);
PG_FUNCTION_INFO_V1(stomata_index_info);
PG_FUNCTION_INFO_V1(stomata_candidate_pages_sql);
PG_FUNCTION_INFO_V1(stomata_merge_pending);
PG_FUNCTION_INFO_V1(stomata_compact);
PG_FUNCTION_INFO_V1(stomata_runs);
PG_FUNCTION_INFO_V1(stomata_verify);
PG_FUNCTION_INFO_V1(stomata_key_stats);
PG_FUNCTION_INFO_V1(stomata_pattern_stats);
PG_FUNCTION_INFO_V1(stomata_estimate);

static void
params_from_args(FunctionCallInfo fcinfo, StomataParams *p)
{
	memset(p, 0, sizeof(*p));
	p->k = PG_GETARG_INT32(1);
	p->cap = PG_GETARG_INT32(2);
	p->rollup = PG_GETARG_INT32(3);
	p->reverse_depth = PG_GETARG_INT32(4);
	p->composite = PG_GETARG_BOOL(5);
	p->cross_edges = PG_GETARG_BOOL(6);
	p->anchor_len = PG_GETARG_INT32(7);
	p->skip_depth = PG_GETARG_INT32(8);
	p->exact = true;
	if (p->k < 1 || p->k > 8)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("k must be between 1 and 8")));
	if (p->cap < 1 || p->cap > 64)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("cap must be between 1 and 64")));
	if (p->rollup < 0 || p->rollup > 4)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rollup must be between 0 and 4")));
	if (p->reverse_depth < 0 || p->reverse_depth > 64)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("reverse_depth must be between 0 and 64")));
	if (p->anchor_len < 0 || p->anchor_len > 4)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("anchor_len must be between 0 and 4")));
	if (p->skip_depth < 0 || p->skip_depth > 8)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("skip_depth must be between 0 and 8")));
	if (p->reverse_depth > p->cap)
		p->reverse_depth = p->cap;
}

static void
emit_keylist(FunctionCallInfo fcinfo, StomataKeyList *l)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (int i = 0; i < l->nkeys; i++)
	{
		Datum		v = CStringGetTextDatum(stomata_key_to_cstring(&l->keys[i]));
		bool		n = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &v, &n);
	}
}

Datum
stomata_keys(PG_FUNCTION_ARGS)
{
	text	   *v = PG_GETARG_TEXT_PP(0);
	StomataParams p;
	StomataKeyList l;

	params_from_args(fcinfo, &p);
	stomata_keylist_init(&l);
	stomata_extract_keylist(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), &p, &l);
	emit_keylist(fcinfo, &l);
	return (Datum) 0;
}

Datum
stomata_pattern_keys(PG_FUNCTION_ARGS)
{
	text	   *v = PG_GETARG_TEXT_PP(0);
	StomataParams p;
	StomataKeyList l;
	bool		icase = PG_GETARG_BOOL(9);

	params_from_args(fcinfo, &p);
	stomata_keylist_init(&l);
	stomata_compile_like(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), &p, icase, &l);
	stomata_keylist_sort_unique(&l);
	emit_keylist(fcinfo, &l);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------ */

static Relation
open_stomata_index(Oid indexoid, LOCKMODE lockmode, Oid *heapoid, AclMode heap_acl)
{
	Relation	index;
	Oid			hoid = IndexGetRelation(indexoid, true);

	if (!OidIsValid(hoid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index",
						get_rel_name(indexoid) ? get_rel_name(indexoid) : "?")));
	if (heap_acl != ACL_NO_RIGHTS &&
		pg_class_aclcheck(hoid, GetUserId(), heap_acl) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(hoid));

	index = index_open(indexoid, lockmode);
	if (!stomata_is_stomata_index(index))
	{
		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a stomata index", get_rel_name(indexoid))));
	}
	*heapoid = hoid;
	return index;
}

#define INFO_COLS 23

Datum
stomata_index_info(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	StomataMetaPageData m;
	TupleDesc	tupdesc;
	Datum		values[INFO_COLS];
	bool		nulls[INFO_COLS];
	int			i = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (tupdesc->natts != INFO_COLS)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("stomata_index_info() definition is out of date"),
				 errhint("Run ALTER EXTENSION stomata UPDATE.")));

	stomata_read_meta(index, &m);
	memset(nulls, 0, sizeof(nulls));
	{
		int64		keys = 0,
					stopped = 0,
					bpages = 0,
					bbytes = 0;

		for (uint32 r = 0; r < m.nruns; r++)
		{
			keys += m.runs[r].nkeys;
			stopped += m.runs[r].nstopped;
			bpages += m.runs[r].blob_pages + m.runs[r].dir_pages;
			bbytes += (int64) m.runs[r].blob_bytes;
		}
		values[i++] = Int64GetDatum(m.generation);
		values[i++] = Int64GetDatum(m.npages);
		values[i++] = Int32GetDatum((int32) m.nruns);
		values[i++] = Int64GetDatum(keys);
		values[i++] = Int64GetDatum(stopped);
		values[i++] = Int64GetDatum(bpages);
		values[i++] = Int64GetDatum(bbytes);
		values[i++] = Int64GetDatum((int64) m.pending_records + m.frozen_records);
		values[i++] = Int64GetDatum((int64) m.pending_pages + m.frozen_pages);
	}
	values[i++] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	values[i++] = Int32GetDatum(m.params.k);
	values[i++] = Int32GetDatum(m.params.cap);
	values[i++] = Int32GetDatum(m.params.rollup);
	values[i++] = Int32GetDatum(m.params.reverse_depth);
	values[i++] = BoolGetDatum(m.params.composite);
	values[i++] = BoolGetDatum(m.params.cross_edges);
	values[i++] = Float4GetDatum(m.params.stop_threshold);
	values[i++] = Int32GetDatum(m.params.pending_limit);
	values[i++] = Int32GetDatum(m.params.anchor_len);
	values[i++] = Int32GetDatum(m.params.skip_depth);
	values[i++] = BoolGetDatum(m.params.exact);
	values[i++] = BoolGetDatum(m.params.exact_bigrams);
	values[i++] = Int32GetDatum(m.version);
	index_close(index, AccessShareLock);
	tupdesc = BlessTupleDesc(tupdesc);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

Datum
stomata_candidate_pages_sql(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	text	   *pattern = PG_GETARG_TEXT_PP(1);
	bool		icase = PG_GETARG_BOOL(2);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	Relation	heap = table_open(heapoid, AccessShareLock);
	StomataQuery q;
	int64		n;

	stomata_compile_for_index(index, pattern, icase, &q);
	n = stomata_candidates(index, heap, &q, NULL);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64(n);
}

Datum
stomata_merge_pending(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index;
	int64		n;

	index = open_stomata_index(indexoid, RowExclusiveLock, &heapoid, ACL_NO_RIGHTS);
	if (!object_ownercheck(RelationRelationId, heapoid, GetUserId()))
	{
		index_close(index, RowExclusiveLock);
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(heapoid));
	}
	n = stomata_merge(index, STOMATA_MERGE_NORMAL);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

Datum
stomata_compact(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index;
	int64		n;

	index = open_stomata_index(indexoid, RowExclusiveLock, &heapoid, ACL_NO_RIGHTS);
	if (!object_ownercheck(RelationRelationId, heapoid, GetUserId()))
	{
		index_close(index, RowExclusiveLock);
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(heapoid));
	}
	n = stomata_merge(index, STOMATA_MERGE_FULL);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT64(n);
}

Datum
stomata_runs(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StomataMetaPageData m;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	stomata_read_meta(index, &m);
	for (uint32 r = 0; r < m.nruns; r++)
	{
		Datum		v[7];
		bool		n[7] = {0};

		v[0] = Int32GetDatum((int32) r + 1);
		v[1] = Int64GetDatum(m.runs[r].run_id);
		v[2] = Int64GetDatum((int64) m.runs[r].rows);
		v[3] = Int64GetDatum(m.runs[r].nkeys);
		v[4] = Int64GetDatum(m.runs[r].blob_pages + m.runs[r].dir_pages);
		v[5] = Int64GetDatum((int64) m.runs[r].blob_bytes);
		v[6] = Int64GetDatum(m.runs[r].npages);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * stomata_key_stats: dictionary composition by key family and tier
 * ------------------------------------------------------------------------
 */
Datum
stomata_key_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StomataMetaPageData meta;
	struct
	{
		char		family[8];
		bool		exact;
		int64		keys,
					arrays,
					bitmaps,
					stops,
					bytes;
	}			fam[64];
	int			nfam = 0;
	Snapshot	snap;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	/* runs we read stay intact while this snapshot is registered */
	snap = RegisterSnapshot(GetTransactionSnapshot());
	stomata_read_meta(index, &meta);

	for (uint32 r = 0; r < meta.nruns; r++)
	{
	StomataSrc *src = stomata_src_run(index, &meta.runs[r]);
	StomataSrc *e = src;

	while (src->next(src))
	{
		char		f[8];
		int			i;
		uint8		tag = e->key.data[0];
		uint8		sub = e->key.data[1];
		bool		exact = stomata_key_exact(&e->key, &meta.params);

		memset(f, 0, sizeof(f));
		if (tag == SK_TAG_G || tag == SK_TAG_S)
			snprintf(f, sizeof(f), "%c%c", tag, sub);
		else if (tag == SK_TAG_X)
			snprintf(f, sizeof(f), "X.%c", sub);
		else if (tag == SK_TAG_A || tag == SK_TAG_W || tag == SK_TAG_N)
			snprintf(f, sizeof(f), "%c", tag);
		else if (tag == SK_TAG_P)
			snprintf(f, sizeof(f), "P:%c%s", sub,
					 ((((uint32) e->key.data[2] << 8) | e->key.data[3]) == 0) ? "0" : "");
		else
			snprintf(f, sizeof(f), "%c:%c", tag, sub);
		for (i = 0; i < nfam; i++)
			if (strcmp(fam[i].family, f) == 0 && fam[i].exact == exact)
				break;
		if (i == nfam)
		{
			if (nfam == lengthof(fam))
				continue;
			memset(&fam[nfam], 0, sizeof(fam[0]));
			strlcpy(fam[nfam].family, f, sizeof(fam[0].family));
			fam[nfam].exact = exact;
			nfam++;
		}
		fam[i].keys++;
		fam[i].bytes += e->len + e->key.len + 6;
		if (e->ctype == STOMATA_CT_ARRAY)
			fam[i].arrays++;
		else if (e->ctype == STOMATA_CT_BITMAP)
			fam[i].bitmaps++;
		else
			fam[i].stops++;
	}
	src->close(src);
	}
	UnregisterSnapshot(snap);
	for (int i = 0; i < nfam; i++)
	{
		Datum		v[7];
		bool		n[7] = {0};

		v[0] = CStringGetTextDatum(fam[i].family);
		v[1] = CStringGetTextDatum(fam[i].exact ? "exact" : "page");
		v[2] = Int64GetDatum(fam[i].keys);
		v[3] = Int64GetDatum(fam[i].arrays);
		v[4] = Int64GetDatum(fam[i].bitmaps);
		v[5] = Int64GetDatum(fam[i].stops);
		v[6] = Int64GetDatum(fam[i].bytes);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * stomata_verify: every live tuple must be covered by the index
 * ------------------------------------------------------------------------
 */
typedef struct PendingRec
{
	BlockNumber blk;			/* hash key part 1 */
	uint32		hash;			/* hash key part 2 */
	int32		len;			/* hash key part 3 (-1 = long value) */
} PendingRec;

typedef struct KeyEntry
{
	StomataKey	key;
	bool		missing;
	bool		stopped;
	bool		exact;
	StomataVals vals;			/* exact tier */
	StomataBits pages;			/* page tier */
} KeyEntry;

typedef struct VerifyState
{
	Relation	index;
	StomataMetaPageData meta;
	StomataRunSet rs;
	HTAB	   *pending;
	HTAB	   *keys;
	StomataBits always;			/* A and W pages */
	MemoryContext cxt;
	MemoryContext tmpcxt;
	int64		checked;
	int64		uncovered;
} VerifyState;

static void
verify_pending_visit(BlockNumber blk, OffsetNumber off, const char *val, int len, void *arg)
{
	VerifyState *vs = (VerifyState *) arg;
	PendingRec	r;

	memset(&r, 0, sizeof(r));
	r.blk = blk;
	r.len = len;
	r.hash = (len >= 0) ? hash_bytes((const unsigned char *) val, len) : 0;
	hash_search(vs->pending, &r, HASH_ENTER, NULL);
}

static bool
page_bit(const StomataBits *b, BlockNumber blk)
{
	return blk < b->nbits && ((b->w[blk >> 6] >> (blk & 63)) & 1);
}

static void
verify_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
				bool tupleIsAlive, void *state)
{
	VerifyState *vs = (VerifyState *) state;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	uint64		tidval = stomata_tid_value(blk, ItemPointerGetOffsetNumber(tid));
	text	   *t;
	const char *s;
	int			len;
	PendingRec	r;
	bool		covered = true;
	MemoryContext old;

	if (isnull[0])
		return;
	vs->checked++;
	if (page_bit(&vs->always, blk))
		return;

	old = MemoryContextSwitchTo(vs->tmpcxt);
	t = DatumGetTextPP(values[0]);
	s = VARDATA_ANY(t);
	len = VARSIZE_ANY_EXHDR(t);

	memset(&r, 0, sizeof(r));
	r.blk = blk;
	r.len = len;
	r.hash = hash_bytes((const unsigned char *) s, len);
	if (hash_search(vs->pending, &r, HASH_FIND, NULL) == NULL)
	{
		StomataKeyList row;

		stomata_keylist_init(&row);
		stomata_extract_keylist(s, len, &vs->meta.params, &row);
		for (int i = 0; i < row.nkeys && covered; i++)
		{
			bool		found;
			KeyEntry   *ke;

			MemoryContextSwitchTo(vs->cxt);
			ke = hash_search(vs->keys, &row.keys[i], HASH_ENTER, &found);
			if (!found)
			{
				StomataMulti mp;

				stomata_lookup_multi(vs->index, &vs->rs, &row.keys[i], &mp);
				ke->missing = !mp.found;
				ke->stopped = mp.stop;
				ke->exact = stomata_key_exact(&row.keys[i], &vs->meta.params);
				ke->vals.v = NULL;
				ke->vals.n = 0;
				ke->pages.w = NULL;
				if (mp.found && !ke->stopped)
				{
					if (ke->exact)
						stomata_multi_vals(&mp, &ke->vals);
					else
					{
						stomata_bits_init(&ke->pages, vs->rs.npages, false);
						stomata_multi_pages(&mp, &ke->pages, false);
					}
				}
				stomata_multi_free(&mp);
			}
			MemoryContextSwitchTo(vs->tmpcxt);
			if (ke->stopped)
				continue;
			if (ke->missing)
				covered = false;
			else if (ke->exact)
				covered = stomata_vals_contains(&ke->vals, tidval);
			else
				covered = page_bit(&ke->pages, blk);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(vs->tmpcxt);

	if (!covered)
	{
		vs->uncovered++;
		if (vs->uncovered <= 5)
			ereport(NOTICE,
					(errmsg("stomata_verify: tuple (%u,%u) is not covered by index \"%s\"",
							blk, ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(vs->index))));
	}
}

static void
verify_add_pseudo(VerifyState *vs, uint8 tag)
{
	StomataKey	k;
	StomataMulti mp;

	stomata_pseudo_key(&k, tag);
	stomata_lookup_multi(vs->index, &vs->rs, &k, &mp);
	if (mp.found)
		stomata_multi_pages(&mp, &vs->always, false);
	stomata_multi_free(&mp);
}

Datum
stomata_verify(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	Relation	heap = table_open(heapoid, AccessShareLock);
	IndexInfo  *ii = BuildIndexInfo(index);
	Snapshot	snapshot;
	TableScanDesc scan;
	VerifyState vs;
	HASHCTL		ctl;

	memset(&vs, 0, sizeof(vs));
	vs.index = index;
	vs.cxt = AllocSetContextCreate(CurrentMemoryContext, "stomata verify", ALLOCSET_DEFAULT_SIZES);
	vs.tmpcxt = AllocSetContextCreate(CurrentMemoryContext, "stomata verify tuple", ALLOCSET_DEFAULT_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(PendingRec);
	ctl.entrysize = sizeof(PendingRec);
	ctl.hcxt = vs.cxt;
	vs.pending = hash_create("stomata verify pending", 256, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(StomataKey);
	ctl.entrysize = sizeof(KeyEntry);
	ctl.hcxt = vs.cxt;
	vs.keys = hash_create("stomata verify keys", 1024, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* take the snapshot first: every tuple it sees was indexed before it */
	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	stomata_read_meta(index, &vs.meta);
	stomata_get_runset(index, &vs.meta, &vs.rs);
	{
		MemoryContext old = MemoryContextSwitchTo(vs.cxt);

		stomata_pending_scan(index, vs.meta.frozen_head, verify_pending_visit, &vs);
		stomata_pending_scan(index, vs.meta.pending_head, verify_pending_visit, &vs);
		stomata_bits_init(&vs.always, vs.rs.npages, false);
		verify_add_pseudo(&vs, SK_TAG_A);
		verify_add_pseudo(&vs, SK_TAG_W);
		MemoryContextSwitchTo(old);
	}

	ii->ii_Concurrent = true;	/* use the MVCC snapshot below */
	scan = table_beginscan_strat(heap, snapshot, 0, NULL, true, false);
	table_index_build_scan(heap, index, ii, true, false, verify_callback, &vs, scan);
	UnregisterSnapshot(snapshot);

	MemoryContextDelete(vs.tmpcxt);
	MemoryContextDelete(vs.cxt);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64(vs.uncovered);
}

/* ------------------------------------------------------------------------
 * stomata_pattern_stats: the postings behind a pattern (what the planner sees)
 * ------------------------------------------------------------------------
 */
Datum
stomata_pattern_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	text	   *pattern = PG_GETARG_TEXT_PP(1);
	bool		icase = PG_GETARG_BOOL(2);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StomataMetaPageData meta;
	StomataRunSet rs;
	StomataQuery q;
	Snapshot	snap;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	snap = RegisterSnapshot(GetTransactionSnapshot());
	stomata_compile_for_index(index, pattern, icase, &q);
	stomata_read_meta(index, &meta);
	stomata_get_runset(index, &meta, &rs);
	for (int i = 0; i < q.all.nkeys; i++)
	{
		StomataMulti mp;
		Datum		v[6];
		bool		n[6] = {0};
		int64		entries = 0;

		stomata_lookup_multi(index, &rs, &q.all.keys[i], &mp);
		for (int r = 0; r < mp.n; r++)
			entries += stomata_container_count(mp.part[r].data, mp.part[r].len, mp.part[r].ctype);
		v[0] = CStringGetTextDatum(stomata_key_to_cstring(&q.all.keys[i]));
		v[1] = CStringGetTextDatum(stomata_key_exact(&q.all.keys[i], &meta.params) ? "exact" : "page");
		v[2] = Int32GetDatum(mp.n);
		v[3] = BoolGetDatum(mp.stop);
		v[4] = Int64GetDatum(entries);
		v[5] = Int64GetDatum((int64) mp.len);
		if (mp.stop || !mp.found)
			n[4] = mp.stop;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
		stomata_multi_free(&mp);
	}
	UnregisterSnapshot(snap);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

Datum
stomata_estimate(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	text	   *pattern = PG_GETARG_TEXT_PP(1);
	bool		icase = PG_GETARG_BOOL(2);
	Oid			heapoid;
	Relation	index = open_stomata_index(indexoid, AccessShareLock, &heapoid, ACL_SELECT);
	StomataEstimate est;
	TupleDesc	tupdesc;
	Datum		v[8];
	bool		n[8] = {0};
	Snapshot	snap;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	snap = RegisterSnapshot(GetTransactionSnapshot());
	stomata_estimate_pattern(index, pattern, icase, &est);
	UnregisterSnapshot(snap);
	index_close(index, AccessShareLock);
	v[0] = Float8GetDatum(est.tids);
	v[1] = Float8GetDatum(est.lossy_pages);
	v[2] = BoolGetDatum(est.complete);
	v[3] = Int32GetDatum(est.keys);
	v[4] = Float8GetDatum(est.values_decoded);
	v[5] = Float8GetDatum(est.index_pages);
	v[6] = Float8GetDatum(est.rows);
	v[7] = Float8GetDatum(est.pages);
	tupdesc = BlessTupleDesc(tupdesc);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, n)));
}
