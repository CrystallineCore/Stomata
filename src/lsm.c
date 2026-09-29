/*-------------------------------------------------------------------------
 *
 * lsm.c
 *	  Log-structured maintenance of a STOMATA index.
 *
 *	  Collector   gathers (key -> values) from unsorted input in memory and
 *	              spills sorted runs to temporary files when it exceeds its
 *	              memory limit (maintenance_work_mem).
 *	  Merger      streams any number of key-sorted sources (temp files, the
 *	              in-memory collector, existing index runs) into one new run,
 *	              combining the postings of equal keys.  Memory use is one
 *	              posting list at a time.
 *	  Merge       folds the pending list into a new run and applies
 *	              size-tiered compaction: the newest runs are merged while
 *	              each is not much larger (FANOUT) than what is being merged.
 *	              Scans and inserts continue while it runs; only the two
 *	              metapage switches take the metapage lock.
 *	  Recycling   pages of replaced runs and pending lists are stamped with
 *	              the next full transaction id and reused once no snapshot
 *	              can still see them; VACUUM returns them to the free space
 *	              map and reclaims pages orphaned by a crash.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "lib/stringinfo.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "stomata.h"

#if PG_VERSION_NUM >= 180000
#define STOMATA_VACUUM_DELAY() vacuum_delay_point(false)
#else
#define STOMATA_VACUUM_DELAY() vacuum_delay_point()
#endif

#define FANOUT				4
#define INLINE_LIMIT_MIN	((uint64) 8 * 1024 * 1024)

/* ------------------------------------------------------------------------
 * Collector
 * ------------------------------------------------------------------------
 */
struct StomataCollector
{
	MemoryContext parent;
	MemoryContext cxt;			/* hash table and lists; reset on spill */
	HTAB	   *h;
	StomataParams p;
	Size		mem_limit;
	BufFile   **spills;
	int			nspills;
	int			maxspills;
	BlockNumber maxblk1;		/* highest heap block seen + 1 */
	uint32		since_check;
};

static HTAB *
create_hash(MemoryContext cxt)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(StomataKey);
	ctl.entrysize = sizeof(StomataBuildEntry);
	ctl.hcxt = cxt;
	return hash_create("stomata collector", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

StomataCollector *
stomata_collector_create(const StomataParams *p, Size mem_limit)
{
	StomataCollector *c = palloc0(sizeof(StomataCollector));

	c->parent = CurrentMemoryContext;
	c->cxt = AllocSetContextCreate(CurrentMemoryContext, "stomata collector",
								   ALLOCSET_DEFAULT_SIZES);
	c->h = create_hash(c->cxt);
	c->p = *p;
	c->mem_limit = Max(mem_limit, (Size) 1024 * 1024);
	c->maxspills = 8;
	c->spills = palloc(sizeof(BufFile *) * c->maxspills);
	return c;
}

void
stomata_collector_add(StomataCollector *c, const StomataKey *k, BlockNumber blk, uint64 tidval)
{
	bool		found;
	StomataBuildEntry *e = hash_search(c->h, k, HASH_ENTER, &found);
	uint64		v = stomata_key_exact(k, &c->p) ? tidval : (uint64) blk;

	if (!found)
	{
		e->n = 0;
		e->max = 0;
		e->vals = NULL;
	}
	if (blk + 1 > c->maxblk1)
		c->maxblk1 = blk + 1;
	if (e->n > 0 && e->vals[e->n - 1] == v)
		return;
	if (e->n >= e->max)
	{
		uint32		nmax = e->max ? e->max * 2 : 4;

		if (e->vals)
			e->vals = repalloc_huge(e->vals, sizeof(uint64) * nmax);
		else
			e->vals = MemoryContextAllocHuge(c->cxt, sizeof(uint64) * nmax);
		e->max = nmax;
	}
	e->vals[e->n++] = v;
}

BlockNumber
stomata_collector_maxblk(StomataCollector *c)
{
	return c->maxblk1;
}

static int
u64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

static int
entry_ptr_cmp(const void *a, const void *b)
{
	const StomataBuildEntry *x = *(StomataBuildEntry *const *) a;
	const StomataBuildEntry *y = *(StomataBuildEntry *const *) b;

	return stomata_key_cmp(&x->key, &y->key);
}

/* sort every list, deduplicate, and return the entries sorted by key */
static StomataBuildEntry **
collector_sorted(StomataCollector *c, long *nentries)
{
	HASH_SEQ_STATUS st;
	StomataBuildEntry *e;
	long		n = hash_get_num_entries(c->h);
	long		i = 0;
	StomataBuildEntry **sorted;

	sorted = MemoryContextAllocHuge(c->cxt, sizeof(StomataBuildEntry *) * Max(n, 1));
	hash_seq_init(&st, c->h);
	while ((e = hash_seq_search(&st)) != NULL)
	{
		if (e->n > 1)
		{
			uint32		w = 0;

			qsort(e->vals, e->n, sizeof(uint64), u64_cmp);
			for (uint32 r = 0; r < e->n; r++)
				if (w == 0 || e->vals[w - 1] != e->vals[r])
					e->vals[w++] = e->vals[r];
			e->n = w;
		}
		sorted[i++] = e;
	}
	qsort(sorted, i, sizeof(StomataBuildEntry *), entry_ptr_cmp);
	*nentries = i;
	return sorted;
}

static void
file_put_entry(BufFile *f, const StomataKey *key, uint8 ctype, const uint8 *data, uint32 len)
{
	BufFileWrite(f, &key->len, 1);
	BufFileWrite(f, key->data, key->len);
	BufFileWrite(f, &ctype, 1);
	BufFileWrite(f, &len, 4);
	if (len > 0)
		BufFileWrite(f, data, len);
}

static void
collector_spill(StomataCollector *c)
{
	long		n;
	StomataBuildEntry **sorted;
	BufFile    *f;
	StringInfoData buf;
	MemoryContext old = MemoryContextSwitchTo(c->parent);

	if (hash_get_num_entries(c->h) == 0)
	{
		MemoryContextSwitchTo(old);
		return;
	}
	f = BufFileCreateTemp(false);
	initStringInfo(&buf);
	sorted = collector_sorted(c, &n);
	for (long i = 0; i < n; i++)
	{
		uint8		ctype = stomata_encode_list(&buf, &sorted[i]->key, sorted[i]->vals,
												sorted[i]->n, 0, &c->p, false, false);

		file_put_entry(f, &sorted[i]->key, ctype, (uint8 *) buf.data, buf.len);
	}
	pfree(buf.data);
	if (c->nspills >= c->maxspills)
	{
		c->maxspills *= 2;
		c->spills = repalloc(c->spills, sizeof(BufFile *) * c->maxspills);
	}
	c->spills[c->nspills++] = f;

	MemoryContextReset(c->cxt);
	c->h = create_hash(c->cxt);
	MemoryContextSwitchTo(old);
}

void
stomata_collector_check_memory(StomataCollector *c)
{
	if (++c->since_check < 256)
		return;
	c->since_check = 0;
	if (MemoryContextMemAllocated(c->cxt, true) > c->mem_limit)
		collector_spill(c);
}

void
stomata_collector_destroy(StomataCollector *c)
{
	for (int i = 0; i < c->nspills; i++)
		BufFileClose(c->spills[i]);
	MemoryContextDelete(c->cxt);
	pfree(c->spills);
	pfree(c);
}

/* ------------------------------------------------------------------------
 * Sources: temporary files and the in-memory collector
 * ------------------------------------------------------------------------
 */
typedef struct FileSrc
{
	StomataSrc	s;
	BufFile    *f;
} FileSrc;

static bool
filesrc_next(StomataSrc *s)
{
	FileSrc    *fs = (FileSrc *) s;
	uint8		klen;

	if (BufFileReadMaybeEOF(fs->f, &klen, 1, true) == 0)
		return false;
	if (klen > STOMATA_KEY_MAXLEN)
		elog(ERROR, "stomata: corrupt temporary run");
	memset(&s->key, 0, sizeof(s->key));
	s->key.len = klen;
	BufFileReadExact(fs->f, s->key.data, klen);
	BufFileReadExact(fs->f, &s->ctype, 1);
	BufFileReadExact(fs->f, &s->len, 4);
	stomata_src_reserve(s, s->len);
	if (s->len > 0)
		BufFileReadExact(fs->f, s->data, s->len);
	return true;
}

static void
filesrc_close(StomataSrc *s)
{
	if (s->data)
		pfree(s->data);
	pfree(s);
}

static StomataSrc *
src_file(BufFile *f)
{
	FileSrc    *fs = palloc0(sizeof(FileSrc));

	fs->s.next = filesrc_next;
	fs->s.close = filesrc_close;
	fs->f = f;
	if (BufFileSeek(f, 0, 0, SEEK_SET) != 0)
		elog(ERROR, "stomata: could not rewind temporary run");
	return &fs->s;
}

typedef struct HashSrc
{
	StomataSrc	s;
	StomataBuildEntry **sorted;
	long		n;
	long		i;
	const StomataParams *p;
	StringInfoData buf;
} HashSrc;

static bool
hashsrc_next(StomataSrc *s)
{
	HashSrc    *hs = (HashSrc *) s;
	StomataBuildEntry *e;

	if (hs->i >= hs->n)
		return false;
	e = hs->sorted[hs->i++];
	s->key = e->key;
	s->ctype = stomata_encode_list(&hs->buf, &e->key, e->vals, e->n, 0, hs->p, false, false);
	s->len = hs->buf.len;
	s->data = (uint8 *) hs->buf.data;
	return true;
}

static void
hashsrc_close(StomataSrc *s)
{
	HashSrc    *hs = (HashSrc *) s;

	pfree(hs->buf.data);
	pfree(hs);
}

static StomataSrc *
src_collector(StomataCollector *c)
{
	HashSrc    *hs = palloc0(sizeof(HashSrc));

	hs->s.next = hashsrc_next;
	hs->s.close = hashsrc_close;
	hs->sorted = collector_sorted(c, &hs->n);
	hs->p = &c->p;
	initStringInfo(&hs->buf);
	return &hs->s;
}

/* ------------------------------------------------------------------------
 * Merger: k-way merge of key-sorted sources into a run writer
 * ------------------------------------------------------------------------
 */
typedef struct SrcHeap
{
	int			n;
	int		   *h;
	StomataSrc **src;
} SrcHeap;

static inline bool
heap_less(SrcHeap *hp, int a, int b)
{
	return stomata_key_cmp(&hp->src[hp->h[a]]->key, &hp->src[hp->h[b]]->key) < 0;
}

static void
heap_swap(SrcHeap *hp, int a, int b)
{
	int			t = hp->h[a];

	hp->h[a] = hp->h[b];
	hp->h[b] = t;
}

static void
heap_push(SrcHeap *hp, int si)
{
	int			i = hp->n++;

	hp->h[i] = si;
	while (i > 0 && heap_less(hp, i, (i - 1) / 2))
	{
		heap_swap(hp, i, (i - 1) / 2);
		i = (i - 1) / 2;
	}
}

static int
heap_pop(SrcHeap *hp)
{
	int			top = hp->h[0];
	int			i = 0;

	hp->h[0] = hp->h[--hp->n];
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			m = i;

		if (l < hp->n && heap_less(hp, l, m))
			m = l;
		if (r < hp->n && heap_less(hp, r, m))
			m = r;
		if (m == i)
			break;
		heap_swap(hp, i, m);
		i = m;
	}
	return top;
}

static void
merge_sources(StomataSrc **src, int nsrc, uint32 npages, const StomataParams *p,
			  StomataRunWriter *rw)
{
	SrcHeap		hp;
	int		   *group = palloc(sizeof(int) * Max(nsrc, 1));
	StringInfoData enc;
	uint32		bitmap_bytes = (npages + 7) / 8;
	bool		stop_on = (p->stop_threshold > 0.0 && p->stop_threshold < 1.0);
	MemoryContext tmpcxt = AllocSetContextCreate(CurrentMemoryContext, "stomata merge key",
												 ALLOCSET_DEFAULT_SIZES);

	hp.n = 0;
	hp.h = palloc(sizeof(int) * Max(nsrc, 1));
	hp.src = src;
	for (int i = 0; i < nsrc; i++)
		if (src[i]->next(src[i]))
			heap_push(&hp, i);
	initStringInfo(&enc);

	while (hp.n > 0)
	{
		StomataKey	key;
		int			ng = 0;
		bool		exact;
		bool		stopcheck;

		CHECK_FOR_INTERRUPTS();
		group[ng++] = heap_pop(&hp);
		key = src[group[0]]->key;
		while (hp.n > 0 && stomata_key_cmp(&src[hp.h[0]]->key, &key) == 0)
			group[ng++] = heap_pop(&hp);

		exact = stomata_key_exact(&key, p);
		stopcheck = stop_on && !exact && key.data[0] != SK_TAG_A && key.data[0] != SK_TAG_W;

		if (ng == 1 &&
			(exact || src[group[0]]->ctype == STOMATA_CT_STOP ||
			 (!stopcheck && src[group[0]]->ctype == STOMATA_CT_BITMAP) ||
			 (!stopcheck && src[group[0]]->ctype == STOMATA_CT_ARRAY &&
			  src[group[0]]->len <= bitmap_bytes)))
		{
			StomataSrc *s = src[group[0]];

			stomata_rw_add(rw, &key, s->ctype, s->data, s->len);
		}
		else
		{
			bool		stop = false;
			StomataVals all;
			uint8		ctype;
			MemoryContext old = MemoryContextSwitchTo(tmpcxt);

			all.v = NULL;
			all.n = 0;
			for (int g = 0; g < ng; g++)
				if (src[group[g]]->ctype == STOMATA_CT_STOP)
					stop = true;

			if (!stop && exact)
			{
				/* sorted TID lists: linear union, one source at a time */
				for (int g = 0; g < ng; g++)
				{
					StomataVals v;

					stomata_container_vals(src[group[g]]->data, src[group[g]]->len,
										   src[group[g]]->ctype, &v);
					if (g == 0)
						all = v;
					else
						stomata_vals_union(&all, &v);
				}
			}
			else if (!stop)
			{
				/* page sets: OR into a bitmap, then back to a sorted list */
				uint32		maxv = npages;
				StomataBits bits;

				for (int g = 0; g < ng; g++)
					if (src[group[g]]->ctype == STOMATA_CT_BITMAP)
						maxv = Max(maxv, src[group[g]]->len * 8);
					else
					{
						StomataVals v;

						/* arrays may reach past npages only if heap grew: size up */
						stomata_decode_vals(src[group[g]]->data, src[group[g]]->len, &v);
						if (v.n > 0 && v.v[v.n - 1] + 1 > maxv)
							maxv = (uint32) (v.v[v.n - 1] + 1);
						pfree(v.v);
					}
				stomata_bits_init(&bits, maxv, false);
				for (int g = 0; g < ng; g++)
					stomata_decode_pages(src[group[g]]->data, src[group[g]]->len,
										 src[group[g]]->ctype, &bits, false);
				all.n = 0;
				all.v = palloc_extended(sizeof(uint64) * Max(maxv, 1), MCXT_ALLOC_HUGE);
				for (uint32 wi = 0; wi < bits.nwords; wi++)
				{
					uint64		w = bits.w[wi];

					while (w)
					{
						all.v[all.n++] = (uint64) wi * 64 + pg_rightmost_one_pos64(w);
						w &= w - 1;
					}
				}
			}
			MemoryContextSwitchTo(old);
			if (stop)
				stomata_rw_add(rw, &key, STOMATA_CT_STOP, NULL, 0);
			else
			{
				ctype = stomata_encode_list(&enc, &key, all.v, all.n, npages, p, true, true);
				stomata_rw_add(rw, &key, ctype, (uint8 *) enc.data,
							   ctype == STOMATA_CT_STOP ? 0 : enc.len);
			}
			MemoryContextReset(tmpcxt);
		}
		for (int g = 0; g < ng; g++)
			if (src[group[g]]->next(src[group[g]]))
				heap_push(&hp, group[g]);
	}
	pfree(enc.data);
	pfree(group);
	pfree(hp.h);
	MemoryContextDelete(tmpcxt);
}

/*
 * Write everything the collector holds, merged with the `extra` sources
 * (existing runs), into a new run of the index.
 */
void
stomata_collector_to_run(StomataCollector *c, Relation index, uint32 npages,
						 StomataSrc **extra, int nextra, StomataRun *out)
{
	int			nsrc = c->nspills + 1 + nextra;
	StomataSrc **src = palloc(sizeof(StomataSrc *) * nsrc);
	StomataRunWriter *rw;
	int			n = 0;

	for (int i = 0; i < c->nspills; i++)
		src[n++] = src_file(c->spills[i]);
	src[n++] = src_collector(c);
	for (int i = 0; i < nextra; i++)
		src[n++] = extra[i];

	rw = stomata_rw_begin(index, npages);
	merge_sources(src, n, npages, &c->p, rw);
	stomata_rw_finish(rw, out);

	for (int i = 0; i < n; i++)
		src[i]->close(src[i]);
	pfree(src);
}

/* ------------------------------------------------------------------------
 * Removing dead rows while streaming a run
 * ------------------------------------------------------------------------
 */
/* the forward first-character key: exactly one per row, used for counting */
static inline bool
is_row_key(const StomataKey *k)
{
	return k->len >= 4 && k->data[0] == SK_TAG_P && k->data[1] == SK_DIR_F &&
		k->data[2] == 0 && k->data[3] == 0;
}

static inline ItemPointerData
tid_from_value(uint64 v)
{
	ItemPointerData tid;

	ItemPointerSet(&tid, (BlockNumber) (v / STOMATA_EXS), (OffsetNumber) (v % STOMATA_EXS) + 1);
	return tid;
}

typedef struct PurgeSrc
{
	StomataSrc	s;
	StomataSrc *in;
	const StomataParams *p;
	IndexBulkDeleteCallback cb;
	void	   *cbstate;
	StringInfoData buf;
	int64		removed_rows;
	int64	   *removed_total;	/* optional: also add here */
} PurgeSrc;

/* the entries of a run, with dead TIDs dropped from exact-tier lists */
static bool
purgesrc_next(StomataSrc *s)
{
	PurgeSrc   *ps = (PurgeSrc *) s;

	for (;;)
	{
		StomataSrc *in = ps->in;
		StomataVals v;
		uint32		w = 0;

		if (!in->next(in))
			return false;
		s->key = in->key;
		s->ctype = in->ctype;
		if (in->ctype != STOMATA_CT_ARRAY || !stomata_key_exact(&in->key, ps->p))
		{
			s->data = in->data;
			s->len = in->len;
			return true;
		}
		stomata_decode_vals(in->data, in->len, &v);
		for (uint32 i = 0; i < v.n; i++)
		{
			ItemPointerData tid = tid_from_value(v.v[i]);

			if (!ps->cb(&tid, ps->cbstate))
				v.v[w++] = v.v[i];
		}
		if (is_row_key(&in->key))
		{
			ps->removed_rows += v.n - w;
			if (ps->removed_total)
				*ps->removed_total += v.n - w;
		}
		if (w == v.n)
		{
			pfree(v.v);
			s->data = in->data;
			s->len = in->len;
			return true;
		}
		if (w == 0)
		{
			pfree(v.v);
			continue;			/* no live row has this key in this run */
		}
		s->ctype = stomata_encode_list(&ps->buf, &in->key, v.v, w, 0, ps->p, false, false);
		s->data = (uint8 *) ps->buf.data;
		s->len = ps->buf.len;
		pfree(v.v);
		return true;
	}
}

static void
purgesrc_close(StomataSrc *s)
{
	PurgeSrc   *ps = (PurgeSrc *) s;

	ps->in->close(ps->in);
	pfree(ps->buf.data);
	pfree(ps);
}

static StomataSrc *
purge_src_wrap(StomataSrc *in, const StomataParams *p, IndexBulkDeleteCallback cb,
			   void *cbstate, int64 *removed_total)
{
	PurgeSrc   *ps = palloc0(sizeof(PurgeSrc));

	ps->s.next = purgesrc_next;
	ps->s.close = purgesrc_close;
	ps->in = in;
	ps->p = p;
	ps->cb = cb;
	ps->cbstate = cbstate;
	ps->removed_total = removed_total;
	initStringInfo(&ps->buf);
	return &ps->s;
}

/* ------------------------------------------------------------------------
 * Merge: pending list -> new run, then size-tiered compaction
 * ------------------------------------------------------------------------
 */
typedef struct MergeState
{
	StomataCollector *c;
	const StomataParams *params;
	MemoryContext tmpcxt;
	int64		nrecords;
	BlockNumber blk;
	uint64		tidval;
	IndexBulkDeleteCallback cb; /* VACUUM: drop records of dead rows */
	void	   *cbstate;
	int64		removed;
} MergeState;

static void
merge_sink_emit(StomataKeySink *sink, const StomataKey *key)
{
	MergeState *ms = (MergeState *) sink->arg;

	stomata_collector_add(ms->c, key, ms->blk, ms->tidval);
}

static void
merge_visit(BlockNumber blk, OffsetNumber off, const char *val, int len, void *arg)
{
	MergeState *ms = (MergeState *) arg;
	StomataKey	pk;

	ms->nrecords++;
	if (len < 0)
	{
		/* value too long for the pending list: the page is always a candidate */
		stomata_pseudo_key(&pk, SK_TAG_A);
		stomata_collector_add(ms->c, &pk, blk, 0);
	}
	else if (off == InvalidOffsetNumber)
	{
		/* heap-only tuple: no valid TID of its own, so its page always qualifies */
		stomata_pseudo_key(&pk, SK_TAG_W);
		stomata_collector_add(ms->c, &pk, blk, 0);
	}
	else
	{
		StomataKeySink sink;
		MemoryContext old;

		if (ms->cb)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, blk, off);
			if (ms->cb(&tid, ms->cbstate))
			{
				ms->removed++;
				return;
			}
		}
		old = MemoryContextSwitchTo(ms->tmpcxt);
		ms->blk = blk;
		ms->tidval = stomata_tid_value(blk, off);
		sink.emit = merge_sink_emit;
		sink.arg = ms;
		stomata_extract_keys(val, len, ms->params, &sink);
		MemoryContextSwitchTo(old);
		MemoryContextReset(ms->tmpcxt);
	}
	stomata_collector_check_memory(ms->c);
}

/*
 * Fold the pending list into a new run and compact.  Returns the number of
 * pending records merged; *new_run_id is the run written (0 if none).  With
 * a VACUUM callback, dead rows are dropped from everything merged (*removed
 * counts them).  Runs from index force_from on (if >= 0) are merged in any
 * case.  The caller holds the merge lock.
 */
static int64
merge_locked(Relation index, int mode, IndexBulkDeleteCallback cb, void *cbstate,
			 int force_from, uint32 *new_run_id, int64 *removed)
{
	Buffer		metabuf;
	StomataMetaPageData meta;
	StomataMetaPageData *m;
	GenericXLogState *xlog;
	bool		have_frozen;
	int			k;
	int			ntake = 0;
	uint64		acc;
	uint64		inline_limit;
	MemoryContext cxt;
	MemoryContext old;
	MergeState	ms;
	StomataRun	newrun;
	StomataRun	chosen[STOMATA_MAX_RUNS];
	int			nchosen;
	StomataSrc *extra[STOMATA_MAX_RUNS];
	uint32		npages;
	BlockNumber old_frozen;
	FullTransactionId fxid;

	*new_run_id = 0;

	/* 1. freeze the pending list: new inserts go to a fresh one */
	metabuf = ReadBuffer(index, STOMATA_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	m = StomataPageGetMeta(BufferGetPage(metabuf));
	stomata_check_meta(index, m);
	if (!BlockNumberIsValid(m->frozen_head) && m->pending_records > 0)
	{
		xlog = GenericXLogStart(index);
		m = StomataPageGetMeta(GenericXLogRegisterBuffer(xlog, metabuf, 0));
		m->frozen_head = m->pending_head;
		m->frozen_records = m->pending_records;
		m->frozen_pages = m->pending_pages;
		m->pending_head = InvalidBlockNumber;
		m->pending_tail = InvalidBlockNumber;
		m->pending_records = 0;
		m->pending_pages = 0;
		GenericXLogFinish(xlog);
		m = StomataPageGetMeta(BufferGetPage(metabuf));
	}
	memcpy(&meta, m, sizeof(meta));
	UnlockReleaseBuffer(metabuf);

	/* 2. choose the runs to merge with */
	have_frozen = BlockNumberIsValid(meta.frozen_head);
	inline_limit = Max(INLINE_LIMIT_MIN, (uint64) meta.params.pending_limit * 1024 * 4);
	k = (int) meta.nruns;
	if (have_frozen)
		acc = Max((uint64) meta.frozen_pages * BLCKSZ * 2, 1);
	else
	{
		if (k < 2 && !(force_from >= 0 && k >= 1))
			return 0;
		acc = meta.runs[k - 1].blob_bytes;
		k--;
		ntake = 1;
	}
	while (k > 0)
	{
		const StomataRun *r = &meta.runs[k - 1];
		bool		take = (mode == STOMATA_MERGE_FULL) || r->blob_bytes <= FANOUT * acc ||
			(force_from >= 0 && k - 1 >= force_from);

		if (take && mode == STOMATA_MERGE_INLINE && r->blob_bytes > inline_limit)
			take = false;
		if (!take && k + 1 > STOMATA_MAX_RUNS)
			take = true;		/* never exceed the run limit */
		if (!take)
			break;
		acc += r->blob_bytes;
		k--;
		ntake++;
	}
	if (!have_frozen && ntake < 2 && !(force_from >= 0 && k <= force_from))
		return 0;
	nchosen = (int) meta.nruns - k;
	for (int i = 0; i < nchosen; i++)
		chosen[i] = meta.runs[k + i];

	/* 3. collect the frozen pending list and stream everything into a new run */
	cxt = AllocSetContextCreate(CurrentMemoryContext, "stomata merge", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	ms.c = stomata_collector_create(&meta.params, (Size) maintenance_work_mem * 1024);
	ms.params = &meta.params;
	ms.tmpcxt = AllocSetContextCreate(cxt, "stomata merge tuple", ALLOCSET_DEFAULT_SIZES);
	ms.nrecords = 0;
	ms.cb = cb;
	ms.cbstate = cbstate;
	ms.removed = 0;
	if (have_frozen)
		stomata_pending_scan(index, meta.frozen_head, merge_visit, &ms);

	npages = Max(meta.npages, stomata_collector_maxblk(ms.c));
	for (int i = 0; i < nchosen; i++)
	{
		extra[i] = stomata_src_run(index, &chosen[i]);
		if (cb)
			extra[i] = purge_src_wrap(extra[i], &meta.params, cb, cbstate, &ms.removed);
	}
	stomata_collector_to_run(ms.c, index, npages, extra, nchosen, &newrun);
	newrun.rows = have_frozen ? meta.frozen_records : 0;
	for (int i = 0; i < nchosen; i++)
		newrun.rows += chosen[i].rows;
	newrun.rows = Max(newrun.rows - ms.removed, 0);
	if (removed)
		*removed += ms.removed;
	stomata_collector_destroy(ms.c);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);

	/* 4. switch: replace the chosen runs by the new one */
	metabuf = ReadBuffer(index, STOMATA_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	xlog = GenericXLogStart(index);
	m = StomataPageGetMeta(GenericXLogRegisterBuffer(xlog, metabuf, 0));
	Assert(m->nruns == meta.nruns);
	newrun.run_id = m->next_run_id++;
	*new_run_id = newrun.run_id;
	m->runs[k] = newrun;
	for (int i = k + 1; i < STOMATA_MAX_RUNS; i++)
		memset(&m->runs[i], 0, sizeof(StomataRun));
	m->nruns = (uint32) k + 1;
	m->npages = Max(m->npages, npages);
	m->generation++;
	old_frozen = m->frozen_head;
	if (have_frozen)
	{
		m->frozen_head = InvalidBlockNumber;
		m->frozen_records = 0;
		m->frozen_pages = 0;
	}
	GenericXLogFinish(xlog);
	UnlockReleaseBuffer(metabuf);

	/* 5. retire the replaced pages; reusable once older than every snapshot */
	fxid = ReadNextFullTransactionId();
	if (have_frozen)
		stomata_free_chain(index, old_frozen, fxid);
	for (int i = 0; i < nchosen; i++)
	{
		stomata_free_chain(index, chosen[i].blob_head, fxid);
		stomata_free_chain(index, chosen[i].dir_head, fxid);
	}
	/* make the freed pages findable (upper free space map levels) */
	stomata_release_skipped(index);
	IndexFreeSpaceMapVacuum(index);
	return ms.nrecords;
}

/*
 * Fold the pending list into a new run and compact.  Returns the number of
 * pending records merged.  Only one merge runs at a time (heavyweight lock
 * on the metapage); an inline merge (from an insert) gives up if another is
 * running and never rewrites runs larger than a bounded size.
 */
int64
stomata_merge(Relation index, int mode)
{
	int64		n;
	uint32		run_id;

	if (mode == STOMATA_MERGE_INLINE)
	{
		if (!ConditionalLockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock))
			return 0;
	}
	else
		LockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	n = merge_locked(index, mode, NULL, NULL, -1, &run_id, NULL);
	UnlockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	return n;
}

/* ------------------------------------------------------------------------
 * VACUUM: return recyclable pages to the free space map, reclaim orphans
 * ------------------------------------------------------------------------
 */
static void
mark_chain(Relation index, BlockNumber head, uint8 *reach, BlockNumber nblocks)
{
	BlockNumber blk = head;

	while (BlockNumberIsValid(blk) && blk < nblocks && !reach[blk])
	{
		Buffer		buf = ReadBuffer(index, blk);
		BlockNumber next;

		reach[blk] = 1;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		next = StomataPageGetOpaque(BufferGetPage(buf))->next;
		UnlockReleaseBuffer(buf);
		blk = next;
	}
}

/*
 * Every page is either reachable from the metapage, deleted (possibly not
 * yet recyclable), new, or an orphan: written by a merge that crashed before
 * switching the metapage, or a replaced page whose unlogged deletion mark was
 * lost in a crash.  Merges are excluded by the merge lock.  Inserts may link
 * a recycled page into the pending list while we scan, so orphan candidates
 * are re-checked against the pending list under the metapage lock (inserts
 * allocate and link pages while holding it exclusively) before being freed.
 */
void
stomata_recycle_pages(Relation index, BlockNumber *free_pages, BlockNumber *deleted_pages)
{
	Buffer		metabuf;
	StomataMetaPageData meta;
	BlockNumber nblocks;
	uint8	   *reach;
	BlockNumber *orphans;
	int			norphans = 0;

	*free_pages = 0;
	*deleted_pages = 0;
	LockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	metabuf = ReadBuffer(index, STOMATA_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	memcpy(&meta, StomataPageGetMeta(BufferGetPage(metabuf)), sizeof(meta));
	nblocks = RelationGetNumberOfBlocks(index);
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
	if (meta.magic != STOMATA_MAGIC || meta.version != STOMATA_VERSION)
	{
		ReleaseBuffer(metabuf);
		UnlockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
		return;
	}
	reach = palloc0(Max(nblocks, 1));
	orphans = palloc(sizeof(BlockNumber) * Max(nblocks, 1));
	reach[STOMATA_METAPAGE_BLKNO] = 1;
	for (uint32 r = 0; r < meta.nruns; r++)
	{
		mark_chain(index, meta.runs[r].blob_head, reach, nblocks);
		mark_chain(index, meta.runs[r].dir_head, reach, nblocks);
	}
	mark_chain(index, meta.pending_head, reach, nblocks);
	mark_chain(index, meta.frozen_head, reach, nblocks);

	for (BlockNumber blk = 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;

		if (reach[blk])
			continue;
		STOMATA_VACUUM_DELAY();
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || stomata_page_recyclable(page))
		{
			RecordFreeIndexPage(index, blk);
			(*free_pages)++;
		}
		else if (StomataPageGetOpaque(page)->flags & STOMATA_PAGE_DELETED)
			(*deleted_pages)++;
		else
			orphans[norphans++] = blk;
		UnlockReleaseBuffer(buf);
	}

	if (norphans > 0)
	{
		FullTransactionId fxid = ReadNextFullTransactionId();

		/* the pending list is the only thing that can have grown meanwhile */
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		memcpy(&meta, StomataPageGetMeta(BufferGetPage(metabuf)), sizeof(meta));
		{
			/* walk it all: its head was marked in the first pass */
			BlockNumber blk = meta.pending_head;
			BlockNumber steps = 0;

			while (BlockNumberIsValid(blk) && blk < nblocks && steps++ < nblocks)
			{
				Buffer		buf = ReadBuffer(index, blk);
				BlockNumber next;

				reach[blk] = 1;
				LockBuffer(buf, BUFFER_LOCK_SHARE);
				next = StomataPageGetOpaque(BufferGetPage(buf))->next;
				UnlockReleaseBuffer(buf);
				blk = next;
			}
		}
		for (int i = 0; i < norphans; i++)
		{
			BlockNumber blk = orphans[i];
			Buffer		buf;
			Page		page;

			if (reach[blk])
				continue;
			buf = ReadBuffer(index, blk);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			if (!PageIsNew(page) && !(StomataPageGetOpaque(page)->flags & STOMATA_PAGE_DELETED))
			{
				GenericXLogState *xlog = GenericXLogStart(index);
				Page		wp = GenericXLogRegisterBuffer(xlog, buf, 0);
				StomataPageOpaque op = StomataPageGetOpaque(wp);

				/*
				 * Mark it deleted as of now: if it is a replaced page whose mark
				 * was lost, a standby query may still read it.
				 */
				op->flags |= STOMATA_PAGE_DELETED;
				op->stomata_page_id = STOMATA_PAGE_ID;
				op->delete_fxid = U64FromFullTransactionId(fxid);
				GenericXLogFinish(xlog);
				RecordFreeIndexPage(index, blk);
				(*deleted_pages)++;
			}
			UnlockReleaseBuffer(buf);
		}
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
	}
	ReleaseBuffer(metabuf);
	UnlockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	pfree(reach);
	pfree(orphans);
	IndexFreeSpaceMapVacuum(index);
}

/* ------------------------------------------------------------------------
 * VACUUM: remove dead rows from the exact tier
 * ------------------------------------------------------------------------
 */

/*
 * Count exact-tier entries of a run and how many of them are dead, over
 * every `every`-th posting list (a sample is enough to judge the share).
 */
static void
count_dead(Relation index, const StomataRun *run, const StomataParams *p,
		   IndexBulkDeleteCallback cb, void *cbstate, int every,
		   int64 *total, int64 *dead)
{
	uint64		nlist = 0;

	StomataSrc *src = stomata_src_run(index, run);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "stomata count dead",
											  ALLOCSET_DEFAULT_SIZES);

	*total = 0;
	*dead = 0;
	while (src->next(src))
	{
		StomataVals v;
		MemoryContext old;

		if (src->ctype != STOMATA_CT_ARRAY || !stomata_key_exact(&src->key, p))
			continue;
		if (nlist++ % every != 0)
			continue;
		old = MemoryContextSwitchTo(cxt);
		stomata_decode_vals(src->data, src->len, &v);
		for (uint32 i = 0; i < v.n; i++)
		{
			ItemPointerData tid = tid_from_value(v.v[i]);

			if (cb(&tid, cbstate))
				(*dead)++;
		}
		*total += v.n;
		MemoryContextSwitchTo(old);
		MemoryContextReset(cxt);
		STOMATA_VACUUM_DELAY();
	}
	src->close(src);
	MemoryContextDelete(cxt);
}

/*
 * Rewrite the runs in which enough exact-tier entries belong to dead rows
 * (at least a tenth, or any in a run under 1 MB) without them.  Page-tier
 * postings cannot tell rows on a page apart and stay as they are; dead
 * entries left behind only cost rechecks.  Returns the rows removed.
 */
int64
stomata_purge(Relation index, IndexBulkDeleteCallback cb, void *cbstate)
{
	StomataMetaPageData meta;
	int64		removed = 0;
	uint32		merged_run = 0;

	LockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	stomata_read_meta(index, &meta);
	if (!meta.params.exact)
	{
		/* no row-level postings: just fold the pending list */
		merge_locked(index, STOMATA_MERGE_NORMAL, NULL, NULL, -1, &merged_run, NULL);
		UnlockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
		return 0;
	}

	/*
	 * Find the oldest run with enough dead entries to be worth rewriting
	 * (at least a tenth, or any in a run under 1 MB).  One merge then folds
	 * the pending list and rewrites that run and every newer one, dropping
	 * dead rows from all of them: each run is written once per VACUUM, and
	 * the runs are left as compact as a normal merge would leave them.
	 */
	{
		int			force_from = -1;

		for (uint32 r = 0; r < meta.nruns; r++)
		{
			int64		total;
			int64		dead;

			bool		small = meta.runs[r].blob_bytes <= 1024 * 1024;

			count_dead(index, &meta.runs[r], &meta.params, cb, cbstate, small ? 1 : 8,
					   &total, &dead);
			if (dead > 0 && (dead * 10 >= total || small))
			{
				force_from = (int) r;
				break;
			}
		}
		merge_locked(index, STOMATA_MERGE_NORMAL, cb, cbstate, force_from, &merged_run, &removed);
	}
	IndexFreeSpaceMapVacuum(index);
	UnlockPage(index, STOMATA_METAPAGE_BLKNO, ExclusiveLock);
	return removed;
}
