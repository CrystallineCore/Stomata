/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  On-disk layout of a STOMATA index: pages, runs, lookups, pending list.
 *
 *	  block 0         metapage: parameters, run list, pending-list pointers
 *	  run             immutable blob chain (entries sorted by key:
 *	                  key -> container) plus a directory chain (first key on
 *	                  each blob page)
 *	  pending chains  (heap TID, value) records appended by aminsert: the
 *	                  active list, and the frozen list a merge is folding
 *
 * Concurrency.  The metapage buffer lock is held only briefly: scans copy
 * the metapage and then read runs and pending lists without it; inserts
 * append under it.  Merges are serialized by a heavyweight lock on the
 * metapage (see lsm.c).  Runs are never modified in place.  Pages of runs
 * and pending lists that a merge replaced are stamped with the next full
 * transaction id and reused only once every snapshot that could have seen
 * them is gone (GlobalVisCheckRemovableFullXid), so a scan that copied an
 * older metapage keeps reading intact pages.  Every page change is
 * WAL-logged with generic WAL.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/nbtxlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/transam.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "stomata.h"

#define BLOB_HEADER_MAGIC	0x424C4F42	/* "BLOB" */
#define BLOB_HEADER_SIZE	12			/* magic, npages, reserved */

/* ------------------------------------------------------------------------
 * Parameters and metapage
 * ------------------------------------------------------------------------
 */
void
stomata_params_from_options(Relation index, StomataParams *p)
{
	StomataOptions *o = (StomataOptions *) index->rd_options;

	p->k = 3;
	p->cap = 2;
	p->rollup = 3;
	p->reverse_depth = 1;
	p->composite = true;
	p->cross_edges = false;
	p->stop_threshold = 0.0;
	p->pending_limit = 4096;
	p->anchor_len = 4;
	p->skip_depth = 4;
	p->exact = true;
	p->exact_bigrams = false;
	if (o)
	{
		p->k = o->k;
		p->cap = o->cap;
		p->rollup = o->rollup;
		p->reverse_depth = o->reverse_depth;
		p->composite = o->composite;
		p->cross_edges = o->cross_edges;
		p->stop_threshold = (float4) o->stop_threshold;
		p->pending_limit = o->pending_limit;
		p->anchor_len = o->anchor_len;
		p->skip_depth = o->skip_depth;
		p->exact = o->exact;
		p->exact_bigrams = o->exact_bigrams;
	}
	if (p->reverse_depth > p->cap)
		p->reverse_depth = p->cap;
}

void
stomata_init_metapage(Page page, const StomataParams *p)
{
	StomataPageOpaque op;
	StomataMetaPageData *m;

	PageInit(page, BLCKSZ, sizeof(StomataPageOpaqueData));
	op = StomataPageGetOpaque(page);
	op->next = InvalidBlockNumber;
	op->flags = STOMATA_PAGE_META;
	op->stomata_page_id = STOMATA_PAGE_ID;
	op->delete_fxid = 0;

	m = StomataPageGetMeta(page);
	memset(m, 0, sizeof(StomataMetaPageData));
	m->magic = STOMATA_MAGIC;
	m->version = STOMATA_VERSION;
	m->params = *p;
	m->generation = 1;
	m->next_run_id = 1;
	m->pending_head = InvalidBlockNumber;
	m->pending_tail = InvalidBlockNumber;
	m->frozen_head = InvalidBlockNumber;
	((PageHeader) page)->pd_lower = ((char *) m + sizeof(StomataMetaPageData)) - (char *) page;
}

void
stomata_check_meta(Relation index, const StomataMetaPageData *m)
{
	if (m->magic != STOMATA_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a stomata index",
						RelationGetRelationName(index))));
	if (m->version != STOMATA_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has on-disk version %u, expected %u",
						RelationGetRelationName(index), m->version, STOMATA_VERSION),
				 errhint("REINDEX the index.")));
	if (m->nruns > STOMATA_MAX_RUNS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has a corrupt run list", RelationGetRelationName(index))));
}

void
stomata_read_meta(Relation index, StomataMetaPageData *meta)
{
	Buffer		buf = ReadBuffer(index, STOMATA_METAPAGE_BLKNO);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(meta, StomataPageGetMeta(BufferGetPage(buf)), sizeof(StomataMetaPageData));
	UnlockReleaseBuffer(buf);
	stomata_check_meta(index, meta);
}

/* ------------------------------------------------------------------------
 * Page allocation and deferred recycling
 * ------------------------------------------------------------------------
 */

/* May this page be (re)used?  New pages, and deleted pages nobody can see. */
bool
stomata_page_recyclable(Page page)
{
	StomataPageOpaque op;
	FullTransactionId fxid;

	if (PageIsNew(page))
		return true;
	op = StomataPageGetOpaque(page);
	if (op->stomata_page_id != STOMATA_PAGE_ID || !(op->flags & STOMATA_PAGE_DELETED))
		return false;
	fxid = FullTransactionIdFromU64(op->delete_fxid);
	if (!FullTransactionIdIsValid(fxid))
		return true;
	return GlobalVisCheckRemovableFullXid(NULL, fxid);
}

/*
 * Deleted pages taken from the free space map that some snapshot may still
 * read.  They are kept out of the map until the current operation finishes
 * (stomata_release_skipped), so the allocator does not meet them again and
 * again; if the operation fails they stay out until VACUUM puts them back.
 */
#define MAX_SKIPPED 4096
static Oid	skipped_rel = InvalidOid;
static int	nskipped = 0;
static BlockNumber skipped[MAX_SKIPPED];

void
stomata_release_skipped(Relation index)
{
	if (skipped_rel == RelationGetRelid(index))
		for (int i = 0; i < nskipped; i++)
			RecordFreeIndexPage(index, skipped[i]);
	nskipped = 0;
	skipped_rel = InvalidOid;
}

/*
 * Before a deleted page is overwritten, tell hot standbys: queries there
 * whose snapshots could still read the page's old contents must be
 * cancelled (or, with hot_standby_feedback, never exist).  This is exactly
 * what nbtree does when it reuses a page, so we emit nbtree's REUSE_PAGE
 * record, whose redo only resolves that recovery conflict and touches no
 * page.  Horizons only move forward, so one record per new horizon suffices.
 */
static Oid	reuse_logged_rel = InvalidOid;
static uint64 reuse_logged_fxid = 0;

static void
log_page_reuse(Relation index, BlockNumber blkno, uint64 fxid)
{
	xl_btree_reuse_page xlrec;

	if (fxid == 0 || !RelationNeedsWAL(index) || !XLogStandbyInfoActive())
		return;
	if (reuse_logged_rel == RelationGetRelid(index) && fxid <= reuse_logged_fxid)
		return;
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.locator = index->rd_locator;
	xlrec.block = blkno;
	xlrec.snapshotConflictHorizon = FullTransactionIdFromU64(fxid);
	xlrec.isCatalogRel = false;
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfBtreeReusePage);
	XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE);
	reuse_logged_rel = RelationGetRelid(index);
	reuse_logged_fxid = fxid;
}

/*
 * Get an exclusively locked page: a recyclable one from the free space map,
 * or a new one.  Safe to call concurrently: a page is only taken while its
 * exclusive lock shows it recyclable, and it is initialised before the lock
 * is released.
 */
Buffer
stomata_new_buffer(Relation index)
{
	if (skipped_rel != RelationGetRelid(index))
	{
		nskipped = 0;
		skipped_rel = RelationGetRelid(index);
	}
	while (nskipped < MAX_SKIPPED)
	{
		BlockNumber blkno = GetFreeIndexPage(index);
		Buffer		buf;

		if (blkno == InvalidBlockNumber)
			break;
		buf = ReadBuffer(index, blkno);
		if (ConditionalLockBuffer(buf))
		{
			Page		page = BufferGetPage(buf);

			if (stomata_page_recyclable(page))
			{
				if (!PageIsNew(page))
					log_page_reuse(index, blkno, StomataPageGetOpaque(page)->delete_fxid);
				return buf;
			}
			/* a deleted page some snapshot may still read: later */
			if (!PageIsNew(page) &&
				(StomataPageGetOpaque(page)->flags & STOMATA_PAGE_DELETED))
				skipped[nskipped++] = blkno;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		ReleaseBuffer(buf);
	}
	return ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
}

/*
 * Mark every page of a chain deleted as of `fxid` (invalid: reusable at
 * once) and hand it to the free space map.  Contents and chain links stay
 * intact, so scans that still follow the chain are unaffected.  The caller
 * must already have switched the metapage so the chain is unreachable.
 */
void
stomata_free_chain(Relation index, BlockNumber head, FullTransactionId fxid)
{
	BlockNumber blk = head;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint32		guard = 0;

	while (BlockNumberIsValid(blk) && blk != STOMATA_METAPAGE_BLKNO && blk < nblocks)
	{
		Buffer		buf = ReadBuffer(index, blk);
		StomataPageOpaque op;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		op = StomataPageGetOpaque(BufferGetPage(buf));
		if (PageIsNew(BufferGetPage(buf)) || (op->flags & STOMATA_PAGE_DELETED))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		next = op->next;

		/*
		 * The deletion mark is only a hint, like a heap hint bit: it is not
		 * WAL-logged.  If a crash loses it, the page is unreachable but
		 * unmarked; the allocator will not take it, and VACUUM reclaims it as
		 * an orphan.  Nothing else about the page changes.
		 */
		op->flags |= STOMATA_PAGE_DELETED;
		op->delete_fxid = U64FromFullTransactionId(fxid);
		MarkBufferDirtyHint(buf, true);
		UnlockReleaseBuffer(buf);
		RecordFreeIndexPage(index, blk);
		blk = next;
		if (++guard > nblocks)
			break;
	}
}

static void
init_chain_page(Page page, uint16 flags)
{
	StomataPageOpaque op;

	PageInit(page, BLCKSZ, sizeof(StomataPageOpaqueData));
	op = StomataPageGetOpaque(page);
	op->next = InvalidBlockNumber;
	op->flags = flags;
	op->stomata_page_id = STOMATA_PAGE_ID;
	op->delete_fxid = 0;
}

static inline uint32
page_used(Page page)
{
	return ((PageHeader) page)->pd_lower - MAXALIGN(SizeOfPageHeaderData);
}

static inline void
page_set_used(Page page, uint32 used)
{
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + used;
}

/* ------------------------------------------------------------------------
 * Chain writer
 * ------------------------------------------------------------------------
 */
typedef struct BlobWriter
{
	Relation	index;
	Buffer		buf;
	GenericXLogState *xlog;
	Page		page;
	uint32		used;
	BlockNumber head;
	uint32		npages;
	uint64		nbytes;
} BlobWriter;

static void
bw_start_page(BlobWriter *bw, Buffer buf)
{
	bw->buf = buf;
	bw->xlog = GenericXLogStart(bw->index);
	bw->page = GenericXLogRegisterBuffer(bw->xlog, buf, GENERIC_XLOG_FULL_IMAGE);
	init_chain_page(bw->page, STOMATA_PAGE_BLOB);
	bw->used = 0;
	bw->npages++;
}

static void
bw_begin(BlobWriter *bw, Relation index)
{
	Buffer		first;

	memset(bw, 0, sizeof(*bw));
	bw->index = index;
	first = stomata_new_buffer(index);
	bw->head = BufferGetBlockNumber(first);
	bw_start_page(bw, first);
}

static void
bw_finish_page(BlobWriter *bw, BlockNumber next)
{
	StomataPageGetOpaque(bw->page)->next = next;
	page_set_used(bw->page, bw->used);
	GenericXLogFinish(bw->xlog);
	UnlockReleaseBuffer(bw->buf);
}

static void
bw_next_page(BlobWriter *bw)
{
	Buffer		nxt = stomata_new_buffer(bw->index);

	bw_finish_page(bw, BufferGetBlockNumber(nxt));
	bw_start_page(bw, nxt);
}

static void
bw_put(BlobWriter *bw, const void *data, uint32 len)
{
	const char *p = data;

	while (len > 0)
	{
		uint32		space = StomataPageCapacity - bw->used;
		uint32		chunk;

		if (space == 0)
		{
			bw_next_page(bw);
			space = StomataPageCapacity;
		}
		chunk = Min(space, len);
		memcpy(PageGetContents(bw->page) + bw->used, p, chunk);
		bw->used += chunk;
		bw->nbytes += chunk;
		p += chunk;
		len -= chunk;
	}
}

static void
bw_put_u32(BlobWriter *bw, uint32 v)
{
	bw_put(bw, &v, sizeof(uint32));
}

/* ------------------------------------------------------------------------
 * Run writer: entries must be added in strictly increasing key order
 * ------------------------------------------------------------------------
 */
struct StomataRunWriter
{
	BlobWriter	bw;
	StomataDirRec *dir;
	uint32		ndir;
	uint32		maxdir;
	BlockNumber last_dir_blk;
	StomataKey	last_key;
	StomataRun	info;
};

StomataRunWriter *
stomata_rw_begin(Relation index, uint32 npages)
{
	StomataRunWriter *rw = palloc0(sizeof(StomataRunWriter));

	bw_begin(&rw->bw, index);
	rw->maxdir = 64;
	rw->dir = palloc(sizeof(StomataDirRec) * rw->maxdir);
	rw->last_dir_blk = InvalidBlockNumber;
	rw->info.npages = npages;
	bw_put_u32(&rw->bw, BLOB_HEADER_MAGIC);
	bw_put_u32(&rw->bw, npages);
	bw_put_u32(&rw->bw, 0);
	return rw;
}

void
stomata_rw_add(StomataRunWriter *rw, const StomataKey *key, uint8 ctype,
			   const uint8 *data, uint32 len)
{
	BlobWriter *bw = &rw->bw;

	Assert(rw->info.nkeys == 0 || stomata_key_cmp(&rw->last_key, key) < 0);
	if (bw->used >= StomataPageCapacity)
		bw_next_page(bw);
	if (BufferGetBlockNumber(bw->buf) != rw->last_dir_blk)
	{
		if (rw->ndir >= rw->maxdir)
		{
			rw->maxdir *= 2;
			rw->dir = repalloc_huge(rw->dir, sizeof(StomataDirRec) * rw->maxdir);
		}
		rw->last_dir_blk = BufferGetBlockNumber(bw->buf);
		rw->dir[rw->ndir].key = *key;
		rw->dir[rw->ndir].blk = rw->last_dir_blk;
		rw->dir[rw->ndir].off = bw->used;
		rw->ndir++;
	}
	bw_put(bw, &key->len, 1);
	bw_put(bw, key->data, key->len);
	bw_put(bw, &ctype, 1);
	bw_put_u32(bw, len);
	if (len > 0)
		bw_put(bw, data, len);
	rw->info.nkeys++;
	if (ctype == STOMATA_CT_STOP)
		rw->info.nstopped++;
	rw->last_key = *key;
}

void
stomata_rw_finish(StomataRunWriter *rw, StomataRun *out)
{
	BlobWriter	dw;

	bw_finish_page(&rw->bw, InvalidBlockNumber);
	rw->info.blob_head = rw->bw.head;
	rw->info.blob_pages = rw->bw.npages;
	rw->info.blob_bytes = rw->bw.nbytes;

	bw_begin(&dw, rw->bw.index);
	if (rw->ndir > 0)
		bw_put(&dw, rw->dir, sizeof(StomataDirRec) * rw->ndir);
	bw_finish_page(&dw, InvalidBlockNumber);
	rw->info.dir_head = dw.head;
	rw->info.dir_pages = dw.npages;
	rw->info.ndir = rw->ndir;

	*out = rw->info;
	pfree(rw->dir);
	pfree(rw);
}

/* ------------------------------------------------------------------------
 * Sequential reader over a page chain
 * ------------------------------------------------------------------------
 */
typedef struct ChainReader
{
	Relation	index;
	BlockNumber blk;
	BlockNumber next;
	uint32		off;
	uint32		used;
	char		buf[BLCKSZ];
} ChainReader;

static void
cr_load(ChainReader *cr, BlockNumber blk)
{
	Buffer		b = ReadBuffer(cr->index, blk);
	Page		page;

	LockBuffer(b, BUFFER_LOCK_SHARE);
	page = BufferGetPage(b);
	cr->used = Min(page_used(page), StomataPageCapacity);
	memcpy(cr->buf, PageGetContents(page), cr->used);
	cr->next = StomataPageGetOpaque(page)->next;
	UnlockReleaseBuffer(b);
	cr->blk = blk;
}

/* read (dst != NULL) or skip n bytes; false at end of chain */
static bool
cr_read(ChainReader *cr, void *dst, uint32 n)
{
	char	   *d = dst;

	while (n > 0)
	{
		uint32		chunk;

		if (cr->off >= cr->used)
		{
			if (!BlockNumberIsValid(cr->next))
				return false;
			cr_load(cr, cr->next);
			cr->off = 0;
			continue;
		}
		chunk = Min(n, cr->used - cr->off);
		if (d)
		{
			memcpy(d, cr->buf + cr->off, chunk);
			d += chunk;
		}
		cr->off += chunk;
		n -= chunk;
	}
	return true;
}

/* ------------------------------------------------------------------------
 * Entry sources
 * ------------------------------------------------------------------------
 */
void
stomata_src_reserve(StomataSrc *s, uint32 len)
{
	if (len <= s->cap && s->data)
		return;
	s->cap = Max(len, Max(s->cap * 2, 256));
	if (s->data)
		pfree(s->data);
	s->data = palloc_extended(s->cap, MCXT_ALLOC_HUGE);
}

typedef struct RunSrc
{
	StomataSrc	s;
	Relation	index;
	ChainReader *cr;
	uint64		remaining;
} RunSrc;

static void
runsrc_read(RunSrc *r, void *dst, uint32 n)
{
	if (n > r->remaining || !cr_read(r->cr, dst, n))
		elog(ERROR, "stomata: corrupt run in index \"%s\"", RelationGetRelationName(r->index));
	r->remaining -= n;
}

static bool
runsrc_next(StomataSrc *s)
{
	RunSrc	   *r = (RunSrc *) s;
	uint8		klen;

	if (r->remaining == 0)
		return false;
	runsrc_read(r, &klen, 1);
	if (klen > STOMATA_KEY_MAXLEN)
		elog(ERROR, "stomata: corrupt run in index \"%s\"", RelationGetRelationName(r->index));
	memset(&s->key, 0, sizeof(s->key));
	s->key.len = klen;
	runsrc_read(r, s->key.data, klen);
	runsrc_read(r, &s->ctype, 1);
	runsrc_read(r, &s->len, 4);
	stomata_src_reserve(s, s->len);
	runsrc_read(r, s->data, s->len);
	return true;
}

static void
runsrc_close(StomataSrc *s)
{
	RunSrc	   *r = (RunSrc *) s;

	if (s->data)
		pfree(s->data);
	pfree(r->cr);
	pfree(r);
}

StomataSrc *
stomata_src_run(Relation index, const StomataRun *run)
{
	RunSrc	   *r = palloc0(sizeof(RunSrc));

	r->s.next = runsrc_next;
	r->s.close = runsrc_close;
	r->index = index;
	r->cr = palloc(sizeof(ChainReader));
	r->cr->index = index;
	r->cr->off = 0;
	r->cr->used = 0;
	r->cr->next = run->blob_head;
	r->remaining = BlockNumberIsValid(run->blob_head) ? run->blob_bytes : 0;
	if (r->remaining >= BLOB_HEADER_SIZE)
	{
		uint32		magic;

		runsrc_read(r, &magic, 4);
		runsrc_read(r, NULL, BLOB_HEADER_SIZE - 4);
		if (magic != BLOB_HEADER_MAGIC)
			elog(ERROR, "stomata: bad run header in index \"%s\"", RelationGetRelationName(index));
	}
	else
		r->remaining = 0;
	return &r->s;
}

/* ------------------------------------------------------------------------
 * Key lookup: one run through its directory, or all runs
 * ------------------------------------------------------------------------
 */
void
stomata_lookup(Relation index, const StomataImage *img, const StomataKey *key,
			   StomataPosting *out)
{
	int			lo = 0;
	int			hi = (int) img->ndir - 1;
	int			start = -1;
	ChainReader *cr;

	memset(out, 0, sizeof(*out));
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (stomata_key_cmp(&img->dir[mid].key, key) <= 0)
		{
			start = mid;
			lo = mid + 1;
		}
		else
			hi = mid - 1;
	}
	if (start < 0)
		return;

	cr = palloc(sizeof(ChainReader));
	cr->index = index;
	cr_load(cr, img->dir[start].blk);
	cr->off = img->dir[start].off;
	for (;;)
	{
		StomataKey	k;
		uint8		ctype;
		uint32		plen;
		int			c;

		memset(&k, 0, sizeof(k));
		if (!cr_read(cr, &k.len, 1))
			break;
		if (k.len > STOMATA_KEY_MAXLEN ||
			!cr_read(cr, k.data, k.len) ||
			!cr_read(cr, &ctype, 1) ||
			!cr_read(cr, &plen, 4))
			elog(ERROR, "stomata: corrupt run in index \"%s\"", RelationGetRelationName(index));
		c = stomata_key_cmp(&k, key);
		if (c == 0)
		{
			out->found = true;
			out->ctype = ctype;
			out->len = plen;
			out->data = palloc_extended(Max(plen, 1), MCXT_ALLOC_HUGE);
			if (!cr_read(cr, out->data, plen))
				elog(ERROR, "stomata: truncated run in index \"%s\"", RelationGetRelationName(index));
			break;
		}
		if (c > 0)
			break;				/* passed it: not present */
		if (!cr_read(cr, NULL, plen))
			break;
	}
	pfree(cr);
}

/*
 * Planner statistics of one key in one run: whether it is present, its
 * container type, payload bytes and number of values (TIDs or pages), and
 * the blob pages a scan reads to fetch it.  Reads the payload in place
 * instead of copying it.
 */
void
stomata_lookup_stats(Relation index, const StomataImage *img, const StomataKey *key,
					 StomataKeyStats *out)
{
	int			lo = 0;
	int			hi = (int) img->ndir - 1;
	int			start = -1;
	ChainReader *cr;

	memset(out, 0, sizeof(*out));
	out->pages = 1;				/* the probe reads one page even on a miss */
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (stomata_key_cmp(&img->dir[mid].key, key) <= 0)
		{
			start = mid;
			lo = mid + 1;
		}
		else
			hi = mid - 1;
	}
	if (start < 0)
		return;

	cr = palloc(sizeof(ChainReader));
	cr->index = index;
	cr_load(cr, img->dir[start].blk);
	cr->off = img->dir[start].off;
	for (;;)
	{
		StomataKey	k;
		uint8		ctype;
		uint32		plen;
		int			c;

		memset(&k, 0, sizeof(k));
		if (!cr_read(cr, &k.len, 1))
			break;
		if (k.len > STOMATA_KEY_MAXLEN ||
			!cr_read(cr, k.data, k.len) ||
			!cr_read(cr, &ctype, 1) ||
			!cr_read(cr, &plen, 4))
			elog(ERROR, "stomata: corrupt run in index \"%s\"", RelationGetRelationName(index));
		c = stomata_key_cmp(&k, key);
		if (c == 0)
		{
			uint32		left = plen;
			uint32		seen = 0;
			uint64		n = 0;

			out->found = true;
			out->ctype = ctype;
			out->bytes = plen;

			/*
			 * Count what is on the pages already read, at most the first
			 * STOMATA_STATS_SAMPLE bytes, and extrapolate the rest of a long
			 * array from its density; the planner must not read megabytes.
			 * Bitmaps are read whole (npages/8 bytes).
			 */
			while (left > 0)
			{
				uint32		chunk;

				if (cr->off >= cr->used)
				{
					if (ctype != STOMATA_CT_BITMAP && seen >= STOMATA_STATS_SAMPLE)
						break;
					if (!BlockNumberIsValid(cr->next))
						elog(ERROR, "stomata: truncated run in index \"%s\"",
							 RelationGetRelationName(index));
					cr_load(cr, cr->next);
					cr->off = 0;
					out->pages++;
					continue;
				}
				chunk = Min(left, cr->used - cr->off);
				n += stomata_container_count((const uint8 *) cr->buf + cr->off, chunk, ctype);
				cr->off += chunk;
				left -= chunk;
				seen += chunk;
			}
			if (left > 0 && seen > 0)
			{
				n = (uint64) ((double) n * plen / seen + 0.5);
				out->pages += (left + BLCKSZ - 1 - MAXALIGN(SizeOfPageHeaderData)) /
					(BLCKSZ - MAXALIGN(SizeOfPageHeaderData));
				out->estimated = true;
			}
			out->count = n;
			break;
		}
		if (c > 0)
			break;
		if (!cr_read(cr, NULL, plen))
			break;
	}
	pfree(cr);
}

void
stomata_lookup_multi(Relation index, const StomataRunSet *rs, const StomataKey *key,
					 StomataMulti *out)
{
	out->found = false;
	out->stop = false;
	out->n = 0;
	out->len = 0;
	for (int i = 0; i < rs->nruns; i++)
	{
		StomataPosting po;

		stomata_lookup(index, rs->img[i], key, &po);
		if (!po.found)
			continue;
		out->found = true;
		if (po.ctype == STOMATA_CT_STOP)
			out->stop = true;
		out->len += po.len;
		out->part[out->n++] = po;
	}
}

void
stomata_multi_free(StomataMulti *m)
{
	for (int i = 0; i < m->n; i++)
		if (m->part[i].data)
			pfree(m->part[i].data);
	m->n = 0;
}

/* acc := acc UNION x (both sorted, duplicate-free); frees x */
void
stomata_vals_union(StomataVals *acc, StomataVals *x)
{
	uint64	   *out = palloc_extended(sizeof(uint64) * Max(acc->n + x->n, 1), MCXT_ALLOC_HUGE);
	uint32		i = 0,
				j = 0,
				n = 0;

	while (i < acc->n && j < x->n)
	{
		if (acc->v[i] < x->v[j])
			out[n++] = acc->v[i++];
		else if (acc->v[i] > x->v[j])
			out[n++] = x->v[j++];
		else
		{
			out[n++] = acc->v[i++];
			j++;
		}
	}
	while (i < acc->n)
		out[n++] = acc->v[i++];
	while (j < x->n)
		out[n++] = x->v[j++];
	pfree(acc->v);
	pfree(x->v);
	acc->v = out;
	acc->n = n;
}

/*
 * Number of values in a container without decoding it: an array holds one
 * value per byte with the high bit clear (varbyte), a bitmap one per set bit.
 */
uint64
stomata_container_count(const uint8 *d, uint32 len, uint8 ctype)
{
	uint64		n = 0;
	uint32		i = 0;

	if (ctype == STOMATA_CT_STOP)
		return 0;
	if (ctype == STOMATA_CT_BITMAP)
	{
		for (; i + 8 <= len; i += 8)
		{
			uint64		w;

			memcpy(&w, d + i, 8);
			n += pg_popcount64(w);
		}
		for (; i < len; i++)
			n += pg_number_of_ones[d[i]];
		return n;
	}
	for (; i + 8 <= len; i += 8)
	{
		uint64		w;

		memcpy(&w, d + i, 8);
		n += 8 - pg_popcount64(w & UINT64CONST(0x8080808080808080));
	}
	for (; i < len; i++)
		n += (d[i] & 0x80) ? 0 : 1;
	return n;
}

/* union of the (exact-tier) lists of every run */
void
stomata_multi_vals(const StomataMulti *m, StomataVals *out)
{
	out->v = NULL;
	out->n = 0;
	for (int i = 0; i < m->n; i++)
	{
		StomataVals v;

		stomata_container_vals(m->part[i].data, m->part[i].len, m->part[i].ctype, &v);
		if (i == 0)
			*out = v;
		else
			stomata_vals_union(out, &v);
	}
	if (out->v == NULL)
		out->v = palloc(sizeof(uint64));
}

/* union of the (page-tier) sets of every run, ANDed or ORed into out */
void
stomata_multi_pages(const StomataMulti *m, StomataBits *out, bool and_into)
{
	StomataBits tmp;

	if (m->stop)
		return;
	if (m->n == 1 || !and_into)
	{
		for (int i = 0; i < m->n; i++)
			stomata_decode_pages(m->part[i].data, m->part[i].len, m->part[i].ctype, out,
								 and_into);
		return;
	}
	stomata_bits_init(&tmp, out->nbits, false);
	for (int i = 0; i < m->n; i++)
		stomata_decode_pages(m->part[i].data, m->part[i].len, m->part[i].ctype, &tmp, false);
	for (uint32 wi = 0; wi < out->nwords; wi++)
		out->w[wi] &= tmp.w[wi];
	pfree(tmp.w);
}

/* ------------------------------------------------------------------------
 * Per-backend cache of run directories (runs are immutable)
 * ------------------------------------------------------------------------
 */
typedef struct RunCacheEntry
{
	Oid			relid;			/* hash key */
	RelFileNumber relnumber;
	int			n;
	StomataImage *img[STOMATA_MAX_RUNS];
} RunCacheEntry;

static HTAB *run_cache = NULL;

static StomataImage *
load_directory(Relation index, const StomataRun *run)
{
	MemoryContext cxt = AllocSetContextCreate(TopMemoryContext, "stomata run directory",
											  ALLOCSET_SMALL_SIZES);
	StomataImage *img = MemoryContextAllocZero(cxt, sizeof(StomataImage));

	PG_TRY();
	{
		img->cxt = cxt;
		img->run_id = run->run_id;
		img->npages = run->npages;
		img->ndir = run->ndir;
		img->dir = MemoryContextAllocExtended(cxt, sizeof(StomataDirRec) * Max(run->ndir, 1),
											  MCXT_ALLOC_HUGE);
		if (run->ndir > 0)
		{
			ChainReader *cr = palloc(sizeof(ChainReader));

			cr->index = index;
			cr_load(cr, run->dir_head);
			cr->off = 0;
			if (!cr_read(cr, img->dir, sizeof(StomataDirRec) * run->ndir))
				elog(ERROR, "stomata: truncated directory in index \"%s\"",
					 RelationGetRelationName(index));
			pfree(cr);
		}
	}
	PG_CATCH();
	{
		MemoryContextDelete(cxt);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return img;
}

/*
 * Directories of the runs listed in `meta`.  Loading one reads pages that a
 * concurrent merge may already have replaced; they are not recycled while
 * our snapshot is alive.
 */
void
stomata_get_runset(Relation index, const StomataMetaPageData *meta, StomataRunSet *rs)
{
	RunCacheEntry *ce;
	bool		found;
	StomataImage *next[STOMATA_MAX_RUNS];
	bool		fresh[STOMATA_MAX_RUNS];
	int			nfresh = 0;

	if (run_cache == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(RunCacheEntry);
		ctl.hcxt = TopMemoryContext;
		run_cache = hash_create("stomata run cache", 16, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	ce = hash_search(run_cache, &RelationGetRelid(index), HASH_ENTER, &found);
	if (!found)
		ce->n = 0;
	if (found && ce->relnumber != index->rd_locator.relNumber)
	{
		for (int i = 0; i < ce->n; i++)
			MemoryContextDelete(ce->img[i]->cxt);
		ce->n = 0;
	}
	ce->relnumber = index->rd_locator.relNumber;

	PG_TRY();
	{
		for (uint32 r = 0; r < meta->nruns; r++)
		{
			next[r] = NULL;
			fresh[r] = false;
			for (int i = 0; i < ce->n; i++)
				if (ce->img[i] && ce->img[i]->run_id == meta->runs[r].run_id)
				{
					next[r] = ce->img[i];
					break;
				}
			if (next[r] == NULL)
			{
				next[r] = load_directory(index, &meta->runs[r]);
				fresh[r] = true;
				nfresh++;
			}
		}
	}
	PG_CATCH();
	{
		for (uint32 r = 0; r < meta->nruns; r++)
			if (fresh[r] && next[r])
				MemoryContextDelete(next[r]->cxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* drop directories of runs that no longer exist */
	for (int i = 0; i < ce->n; i++)
	{
		bool		keep = false;

		for (uint32 r = 0; r < meta->nruns && !keep; r++)
			keep = (next[r] == ce->img[i]);
		if (!keep)
			MemoryContextDelete(ce->img[i]->cxt);
	}
	ce->n = (int) meta->nruns;
	for (uint32 r = 0; r < meta->nruns; r++)
		ce->img[r] = next[r];

	rs->nruns = (int) meta->nruns;
	rs->npages = meta->npages;
	for (uint32 r = 0; r < meta->nruns; r++)
		rs->img[r] = next[r];
	(void) nfresh;
}

/* ------------------------------------------------------------------------
 * Containers
 * ------------------------------------------------------------------------
 */
void
stomata_bits_init(StomataBits *b, uint32 nbits, bool fill)
{
	b->nbits = nbits;
	b->nwords = (nbits + 63) / 64;
	b->w = palloc_extended(sizeof(uint64) * Max(b->nwords, 1), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	if (fill && nbits > 0)
	{
		memset(b->w, 0xFF, sizeof(uint64) * b->nwords);
		if (nbits % 64)
			b->w[b->nwords - 1] = (UINT64CONST(1) << (nbits % 64)) - 1;
	}
}

/* varbyte-coded deltas (first value + 1) */
static inline bool
next_delta(const uint8 *d, uint32 len, uint32 *i, uint64 *v)
{
	uint64		x = 0;
	int			shift = 0;

	if (*i >= len)
		return false;
	for (;;)
	{
		uint8		c = d[(*i)++];

		x |= (uint64) (c & 0x7F) << shift;
		if (!(c & 0x80) || *i >= len)
			break;
		shift += 7;
	}
	*v = x;
	return true;
}

static void
varbyte_append64(StringInfo s, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(s, (char) ((v & 0x7F) | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(s, (char) v);
}

/* Decode a page-tier container into page bits; AND into or OR into `out`. */
void
stomata_decode_pages(const uint8 *d, uint32 len, uint8 ctype, StomataBits *out, bool and_into)
{
	if (ctype == STOMATA_CT_STOP)
		return;
	if (ctype == STOMATA_CT_BITMAP)
	{
		for (uint32 wi = 0; wi < out->nwords; wi++)
		{
			uint64		v = 0;

			for (int b = 0; b < 8; b++)
			{
				uint32		byte = wi * 8 + b;

				if (byte < len)
					v |= ((uint64) d[byte]) << (8 * b);
			}
			if (and_into)
				out->w[wi] &= v;
			else
				out->w[wi] |= v;
		}
		return;
	}
	else
	{
		StomataBits tmp;
		StomataBits *dst = out;
		uint32		i = 0;
		uint64		delta;
		uint64		cur = 0;
		bool		firstv = true;

		if (and_into)
		{
			stomata_bits_init(&tmp, out->nbits, false);
			dst = &tmp;
		}
		while (next_delta(d, len, &i, &delta))
		{
			cur = firstv ? delta - 1 : cur + delta;
			firstv = false;
			if (cur < dst->nbits)
				dst->w[cur >> 6] |= UINT64CONST(1) << (cur & 63);
		}
		if (and_into)
		{
			for (uint32 wi = 0; wi < out->nwords; wi++)
				out->w[wi] &= tmp.w[wi];
			pfree(tmp.w);
		}
	}
}

/* Decode an array container into a sorted array of values. */
void
stomata_decode_vals(const uint8 *d, uint32 len, StomataVals *out)
{
	uint32		i = 0;
	uint32		max = Max(len / 2, 16);
	uint64		delta;
	uint64		cur = 0;
	bool		firstv = true;

	out->n = 0;
	out->v = palloc_extended(sizeof(uint64) * max, MCXT_ALLOC_HUGE);
	while (next_delta(d, len, &i, &delta))
	{
		cur = firstv ? delta - 1 : cur + delta;
		firstv = false;
		if (out->n >= max)
		{
			max *= 2;
			out->v = repalloc_huge(out->v, sizeof(uint64) * max);
		}
		out->v[out->n++] = cur;
	}
}

/* Any container (array or bitmap) as a sorted value array; STOP gives none. */
void
stomata_container_vals(const uint8 *d, uint32 len, uint8 ctype, StomataVals *out)
{
	if (ctype == STOMATA_CT_ARRAY)
	{
		stomata_decode_vals(d, len, out);
		return;
	}
	out->n = 0;
	if (ctype != STOMATA_CT_BITMAP)
	{
		out->v = palloc(sizeof(uint64));
		return;
	}
	{
		uint32		cnt = 0;

		for (uint32 byte = 0; byte < len; byte++)
			cnt += pg_popcount32(d[byte]);
		out->v = palloc_extended(sizeof(uint64) * Max(cnt, 1), MCXT_ALLOC_HUGE);
		for (uint32 byte = 0; byte < len; byte++)
		{
			uint32		v = d[byte];

			while (v)
			{
				out->v[out->n++] = (uint64) byte * 8 + pg_rightmost_one_pos32(v);
				v &= v - 1;
			}
		}
	}
}

bool
stomata_vals_contains(const StomataVals *v, uint64 x)
{
	uint32		lo = 0;
	uint32		hi = v->n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (v->v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < v->n && v->v[lo] == x;
}

/*
 * Encode a sorted, duplicate-free list into buf and return its container
 * type.  Exact-tier lists are always arrays of TID values.  Page-tier lists
 * become a stop-key when denser than stop_threshold (if apply_stop), else
 * the smaller of a delta array and a bitmap over npages.
 */
uint8
stomata_encode_list(StringInfo buf, const StomataKey *key, const uint64 *v, uint32 n,
					uint32 npages, const StomataParams *p, bool apply_stop, bool allow_bitmap)
{
	bool		exact = stomata_key_exact(key, p);
	uint8		tag = key->data[0];
	uint32		bitmap_bytes;
	uint64		prev = 0;

	resetStringInfo(buf);
	if (!exact && apply_stop && tag != SK_TAG_A && tag != SK_TAG_W &&
		p->stop_threshold > 0.0 && p->stop_threshold < 1.0 && npages > 0 &&
		(double) n > (double) p->stop_threshold * npages)
		return STOMATA_CT_STOP;

	for (uint32 i = 0; i < n; i++)
	{
		/* first value stored +1 so that 0 never appears as a delta */
		varbyte_append64(buf, i == 0 ? v[i] + 1 : v[i] - prev);
		prev = v[i];
	}
	if (exact || !allow_bitmap || n == 0 || v[n - 1] >= npages)
		return STOMATA_CT_ARRAY;
	bitmap_bytes = (npages + 7) / 8;
	if ((uint32) buf->len <= bitmap_bytes)
		return STOMATA_CT_ARRAY;

	resetStringInfo(buf);
	enlargeStringInfo(buf, bitmap_bytes);
	memset(buf->data, 0, bitmap_bytes);
	for (uint32 i = 0; i < n; i++)
		buf->data[v[i] >> 3] |= (char) (1 << (v[i] & 7));
	buf->len = bitmap_bytes;
	return STOMATA_CT_BITMAP;
}

/* ------------------------------------------------------------------------
 * Pending list
 * ------------------------------------------------------------------------
 */
void
stomata_pending_append(Relation index, BlockNumber heapblk, OffsetNumber hoff,
					   const char *val, int len)
{
	Buffer		metabuf;
	Page		metapage;
	StomataMetaPageData *meta;
	GenericXLogState *xlog;
	uint16		rlen = (len > STOMATA_PENDING_MAXVAL) ? STOMATA_PENDING_LONG : (uint16) len;
	uint32		need = STOMATA_PENDING_HDR + (rlen == STOMATA_PENDING_LONG ? 0 : rlen);
	Buffer		tailbuf = InvalidBuffer;
	Buffer		newbuf = InvalidBuffer;
	bool		merge_now = false;

	metabuf = ReadBuffer(index, STOMATA_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	meta = StomataPageGetMeta(BufferGetPage(metabuf));
	stomata_check_meta(index, meta);

	if (BlockNumberIsValid(meta->pending_tail))
	{
		tailbuf = ReadBuffer(index, meta->pending_tail);
		LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
	}

	/* allocate before starting the WAL record (allocation may extend) */
	if (!BufferIsValid(tailbuf) ||
		page_used(BufferGetPage(tailbuf)) + need > StomataPageCapacity)
		newbuf = stomata_new_buffer(index);

	xlog = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(xlog, metabuf, 0);
	meta = StomataPageGetMeta(metapage);

	if (!BufferIsValid(newbuf))
	{
		Page		tp = GenericXLogRegisterBuffer(xlog, tailbuf, 0);
		uint32		used = page_used(tp);
		char	   *dst = PageGetContents(tp) + used;

		memcpy(dst, &heapblk, 4);
		memcpy(dst + 4, &hoff, 2);
		memcpy(dst + 6, &rlen, 2);
		if (rlen != STOMATA_PENDING_LONG)
			memcpy(dst + STOMATA_PENDING_HDR, val, rlen);
		page_set_used(tp, used + need);
	}
	else
	{
		Page		np;
		char	   *dst;

		np = GenericXLogRegisterBuffer(xlog, newbuf, GENERIC_XLOG_FULL_IMAGE);
		init_chain_page(np, STOMATA_PAGE_PENDING);
		dst = PageGetContents(np);
		memcpy(dst, &heapblk, 4);
		memcpy(dst + 4, &hoff, 2);
		memcpy(dst + 6, &rlen, 2);
		if (rlen != STOMATA_PENDING_LONG)
			memcpy(dst + STOMATA_PENDING_HDR, val, rlen);
		page_set_used(np, need);

		if (BufferIsValid(tailbuf))
		{
			Page		tp = GenericXLogRegisterBuffer(xlog, tailbuf, 0);

			StomataPageGetOpaque(tp)->next = BufferGetBlockNumber(newbuf);
		}
		else
			meta->pending_head = BufferGetBlockNumber(newbuf);
		meta->pending_tail = BufferGetBlockNumber(newbuf);
		meta->pending_pages++;
	}
	meta->pending_records++;
	meta->indexed_rows += 1;
	GenericXLogFinish(xlog);
	if (BufferIsValid(newbuf))
	{
		UnlockReleaseBuffer(newbuf);
		stomata_release_skipped(index);
	}

	meta = StomataPageGetMeta(BufferGetPage(metabuf));
	if (meta->params.pending_limit > 0 &&
		(uint64) meta->pending_pages * (BLCKSZ / 1024) > (uint64) meta->params.pending_limit)
		merge_now = true;

	if (BufferIsValid(tailbuf))
		UnlockReleaseBuffer(tailbuf);
	UnlockReleaseBuffer(metabuf);

	if (merge_now)
		stomata_merge(index, STOMATA_MERGE_INLINE);
}

void
stomata_pending_scan(Relation index, BlockNumber head,
					 StomataPendingVisitor visit, void *arg)
{
	BlockNumber blk = head;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		uint32		used;
		uint32		off = 0;
		char	   *base;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		used = Min(page_used(page), StomataPageCapacity);
		base = PageGetContents(page);
		while (off + STOMATA_PENDING_HDR <= used)
		{
			BlockNumber hb;
			OffsetNumber ho;
			uint16		rlen;

			memcpy(&hb, base + off, 4);
			memcpy(&ho, base + off + 4, 2);
			memcpy(&rlen, base + off + 6, 2);
			off += STOMATA_PENDING_HDR;
			if (rlen == STOMATA_PENDING_LONG)
				visit(hb, ho, NULL, -1, arg);
			else
			{
				if (off + rlen > used)
					elog(ERROR, "stomata: corrupt pending page %u", blk);
				visit(hb, ho, base + off, rlen, arg);
				off += rlen;
			}
		}
		blk = StomataPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
}
