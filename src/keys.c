/*-------------------------------------------------------------------------
 *
 * keys.c
 *	  Key extraction (index side) and LIKE-pattern compilation (query side).
 *
 * The two halves must agree exactly: every key the compiler emits for a
 * pattern P must be emitted by the extractor for every string matching P.
 * That is the whole soundness argument, so both halves share the helpers
 * below and apply the same caps, heads and clamping.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"

#include "stomata.h"

#define TERM_CHAR(c)	((c).len == 1 && (c).b[0] == 0)

static const StomataChar term_char = {1, {0, 0, 0, 0}};

/* ------------------------------------------------------------------------
 * Characters
 * ------------------------------------------------------------------------
 */

/*
 * Decode a string in the server encoding into characters, folding ASCII
 * upper case to lower case.  Folding is per character and length
 * preserving, so positions seen by the index equal positions seen by LIKE.
 */
void
stomata_decode_string(const char *s, int len, StomataChar **chars, int *nchars)
{
	StomataChar *out = palloc(sizeof(StomataChar) * (len + 1));
	int			n = 0;
	int			i = 0;

	while (i < len)
	{
		int			l = pg_mblen(s + i);

		if (l < 1)
			l = 1;
		if (l > 4 || i + l > len)
			l = Min(len - i, 1);	/* defensive: treat as a single byte */
		out[n].len = (uint8) l;
		memset(out[n].b, 0, 4);
		memcpy(out[n].b, s + i, l);
		if (l == 1 && out[n].b[0] >= 'A' && out[n].b[0] <= 'Z')
			out[n].b[0] += 'a' - 'A';
		n++;
		i += l;
	}
	*chars = out;
	*nchars = n;
}

static inline void
make_key(StomataKey *key, uint8 tag, uint8 sub, uint32 num,
		 const StomataChar *const *cs, int nc)
{
	int			pos = 4;

	memset(key, 0, sizeof(StomataKey));
	if (num > 0xFFFF)
		num = 0xFFFF;			/* clamped identically on both sides: sound */
	key->data[0] = tag;
	key->data[1] = sub;
	key->data[2] = (uint8) (num >> 8);
	key->data[3] = (uint8) (num & 0xFF);
	for (int i = 0; i < nc; i++)
	{
		memcpy(key->data + pos, cs[i]->b, cs[i]->len);
		pos += cs[i]->len;
	}
	key->len = (uint8) pos;
}

static inline void
emit1(StomataKeySink *sink, uint8 tag, uint8 sub, uint32 num, const StomataChar *a)
{
	StomataKey	k;
	const StomataChar *cs[1] = {a};

	make_key(&k, tag, sub, num, cs, 1);
	sink->emit(sink, &k);
}

static inline void
emit2(StomataKeySink *sink, uint8 tag, uint8 sub, uint32 num,
	  const StomataChar *a, const StomataChar *b)
{
	StomataKey	k;
	const StomataChar *cs[2] = {a, b};

	make_key(&k, tag, sub, num, cs, 2);
	sink->emit(sink, &k);
}

static inline void
emit3(StomataKeySink *sink, uint8 tag, uint8 sub, uint32 num,
	  const StomataChar *a, const StomataChar *b, const StomataChar *c)
{
	StomataKey	k;
	const StomataChar *cs[3] = {a, b, c};

	make_key(&k, tag, sub, num, cs, 3);
	sink->emit(sink, &k);
}

static inline void
emit4(StomataKeySink *sink, uint8 tag, uint8 sub, uint32 num,
	  const StomataChar *a, const StomataChar *b,
	  const StomataChar *c, const StomataChar *d)
{
	StomataKey	k;
	const StomataChar *cs[4] = {a, b, c, d};

	make_key(&k, tag, sub, num, cs, 4);
	sink->emit(sink, &k);
}

static inline void
emitn(StomataKeySink *sink, uint8 tag, uint8 sub, uint32 num, const StomataChar *run, int n)
{
	StomataKey	k;
	const StomataChar *cs[4];

	Assert(n >= 1 && n <= 4);
	for (int i = 0; i < n; i++)
		cs[i] = &run[i];
	make_key(&k, tag, sub, num, cs, n);
	sink->emit(sink, &k);
}

static inline int
dir_limit(const StomataParams *p, uint8 dir)
{
	if (dir == SK_DIR_F)
		return p->cap * p->k;
	return Min(p->reverse_depth, p->cap) * p->k;
}

static inline int
dir_head(const StomataParams *p, uint8 dir)
{
	return Min(p->k, dir_limit(p, dir));
}

/* ------------------------------------------------------------------------
 * Extraction (index side)
 * ------------------------------------------------------------------------
 */
static void
extract_positional(const StomataParams *p, uint8 dir, const StomataChar *t, int tn,
				   StomataKeySink *sink)
{
	int			k = p->k;
	int			lim = dir_limit(p, dir);
	int			head = dir_head(p, dir);
	int			m = Min(tn, lim);

	for (int i = 0; i < m; i++)
	{
		emit1(sink, SK_TAG_U, dir, i / k, &t[i]);
		if (i < head)
			emit1(sink, SK_TAG_P, dir, i, &t[i]);
	}
	for (int i = 0; i + 1 < m; i++)
	{
		if (i / k == (i + 1) / k)
			emit2(sink, SK_TAG_T, dir, i / k, &t[i], &t[i + 1]);
		else if (p->cross_edges)
			emit2(sink, SK_TAG_C, dir, i / k, &t[i], &t[i + 1]);
	}
}

void
stomata_extract_keys(const char *s, int len, const StomataParams *p, StomataKeySink *sink)
{
	StomataChar *f;
	StomataChar *t;
	int			n;
	int			k = p->k;

	stomata_decode_string(s, len, &f, &n);
	t = palloc(sizeof(StomataChar) * (n + 1));

	/*
	 * ILIKE lower-cases with the collation's rules, which can map non-ASCII
	 * characters in ways the ASCII fold does not.  Rows with any non-ASCII
	 * byte are marked, and ILIKE treats them as candidates.
	 */
	for (int i = 0; i < len; i++)
		if ((unsigned char) s[i] >= 0x80)
		{
			StomataKey	nk;

			stomata_pseudo_key(&nk, SK_TAG_N);
			sink->emit(sink, &nk);
			break;
		}

	/* forward: f || '\0' */
	memcpy(t, f, sizeof(StomataChar) * n);
	t[n] = term_char;
	extract_positional(p, SK_DIR_F, t, n + 1, sink);

	/* the length key survives the cap */
	if (n + 1 > dir_limit(p, SK_DIR_F))
		emit1(sink, SK_TAG_U, SK_DIR_F, n / k, &term_char);

	/* global rollups over f || '\0' */
	if (p->rollup >= 1)
		for (int i = 0; i <= n; i++)
			emit1(sink, SK_TAG_G, '1', 0, &t[i]);
	if (p->rollup >= 2)
		for (int i = 0; i + 1 <= n; i++)
			emit2(sink, SK_TAG_G, '2', 0, &t[i], &t[i + 1]);
	if (p->rollup >= 3)
		for (int i = 0; i + 2 <= n; i++)
			emit3(sink, SK_TAG_G, '3', 0, &t[i], &t[i + 1], &t[i + 2]);
	if (p->rollup >= 4)
		for (int i = 0; i + 3 <= n; i++)
			emitn(sink, SK_TAG_G, '4', 0, &t[i], 4);

	/* skip-grams: x at i, y at i+d, for d = 2..skip_depth */
	for (int d = 2; d <= p->skip_depth; d++)
		for (int i = 0; i + d <= n; i++)
			emit2(sink, SK_TAG_S, (uint8) ('0' + d), 0, &t[i], &t[i + d]);

	/* whole anchored prefixes of f || '\0', lengths 2..anchor_len */
	for (int L = 2; L <= p->anchor_len && L <= n + 1; L++)
		emitn(sink, SK_TAG_H, SK_DIR_F, L, t, L);

	/* reverse: reverse(f) || '\0' */
	for (int i = 0; i < n; i++)
		t[i] = f[n - 1 - i];
	t[n] = term_char;
	extract_positional(p, SK_DIR_R, t, n + 1, sink);
	for (int L = 2; L <= p->anchor_len && L <= n + 1; L++)
		emitn(sink, SK_TAG_H, SK_DIR_R, L, t, L);

	/* composite keys bind head, tail and length to one row */
	if (p->composite && n > 0)
	{
		uint32		L = n / k;

		emit2(sink, SK_TAG_X, SK_X_HT1, 0, &f[0], &f[n - 1]);
		emit1(sink, SK_TAG_X, SK_X_HL, L, &f[0]);
		emit1(sink, SK_TAG_X, SK_X_TL, L, &f[n - 1]);
		if (n >= 2)
		{
			emit4(sink, SK_TAG_X, SK_X_HT2, 0, &f[0], &f[1], &f[n - 2], &f[n - 1]);
			emit3(sink, SK_TAG_X, SK_X_H2T1, 0, &f[0], &f[1], &f[n - 1]);
			emit3(sink, SK_TAG_X, SK_X_H1T2, 0, &f[0], &f[n - 2], &f[n - 1]);
		}
	}

	pfree(t);
	pfree(f);
}

/* ------------------------------------------------------------------------
 * Key lists
 * ------------------------------------------------------------------------
 */
void
stomata_keylist_init(StomataKeyList *l)
{
	l->nkeys = 0;
	l->maxkeys = 16;
	l->keys = palloc(sizeof(StomataKey) * l->maxkeys);
}

void
stomata_keylist_add(StomataKeyList *l, const StomataKey *k)
{
	if (l->nkeys >= l->maxkeys)
	{
		l->maxkeys *= 2;
		l->keys = repalloc(l->keys, sizeof(StomataKey) * l->maxkeys);
	}
	l->keys[l->nkeys++] = *k;
}

int
stomata_key_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(StomataKey));
}

void
stomata_keylist_sort_unique(StomataKeyList *l)
{
	int			w = 0;

	if (l->nkeys <= 1)
		return;
	qsort(l->keys, l->nkeys, sizeof(StomataKey), stomata_key_cmp);
	for (int r = 0; r < l->nkeys; r++)
	{
		if (w == 0 || stomata_key_cmp(&l->keys[w - 1], &l->keys[r]) != 0)
			l->keys[w++] = l->keys[r];
	}
	l->nkeys = w;
}

bool
stomata_keylist_contains(const StomataKeyList *sorted, const StomataKey *k)
{
	return bsearch(k, sorted->keys, sorted->nkeys, sizeof(StomataKey),
				   stomata_key_cmp) != NULL;
}

static void
keylist_sink_emit(StomataKeySink *sink, const StomataKey *key)
{
	stomata_keylist_add((StomataKeyList *) sink->arg, key);
}

void
stomata_extract_keylist(const char *s, int len, const StomataParams *p, StomataKeyList *out)
{
	StomataKeySink sink;

	sink.emit = keylist_sink_emit;
	sink.arg = out;
	stomata_extract_keys(s, len, p, &sink);
	stomata_keylist_sort_unique(out);
}

/* ------------------------------------------------------------------------
 * Pattern compilation (query side)
 * ------------------------------------------------------------------------
 */
#define IT_LIT	0
#define IT_ONE	1
#define IT_ANY	2

typedef struct PatItem
{
	int			type;
	StomataChar c;
} PatItem;

/*
 * Keys implied by `items` occupying positions start.. in direction dir.
 * Mirrors extract_positional() and the length-key rule.
 */
static void
compile_positional(const StomataParams *p, uint8 dir, const PatItem *items, int n,
				   int start, StomataKeySink *sink)
{
	int			k = p->k;
	int			lim = dir_limit(p, dir);
	int			head = dir_head(p, dir);
	int			prevq = -2;
	const StomataChar *prevc = NULL;

	for (int j = 0; j < n; j++)
	{
		int			q = start + j;
		const StomataChar *c;

		if (items[j].type != IT_LIT)
		{
			prevc = NULL;
			continue;
		}
		c = &items[j].c;
		if (!(q < lim || (dir == SK_DIR_F && TERM_CHAR(*c))))
		{
			prevc = NULL;
			continue;
		}
		if (q < head)
			emit1(sink, SK_TAG_P, dir, q, c);
		else
			emit1(sink, SK_TAG_U, dir, q / k, c);

		if (prevc != NULL && prevq == q - 1 && q < lim)
		{
			if (prevq / k == q / k)
				emit2(sink, SK_TAG_T, dir, prevq / k, prevc, c);
			else if (p->cross_edges)
				emit2(sink, SK_TAG_C, dir, prevq / k, prevc, c);
		}
		prevc = c;
		prevq = q;
	}
}

static void
compile_rollup(const StomataParams *p, const PatItem *items, int n, bool term_after,
			   StomataKeySink *sink)
{
	int			j = 0;
	int			total = n + (term_after ? 1 : 0);
	PatItem    *it = palloc(sizeof(PatItem) * (total + 1));

	memcpy(it, items, sizeof(PatItem) * n);
	if (term_after)
	{
		it[n].type = IT_LIT;
		it[n].c = term_char;
	}

	while (j < total)
	{
		int			s;
		int			len;
		int			g;

		if (it[j].type != IT_LIT)
		{
			j++;
			continue;
		}
		s = j;
		while (j < total && it[j].type == IT_LIT)
			j++;
		len = j - s;
		g = Min(p->rollup, len);
		if (g == 1)
			for (int i = s; i < j; i++)
				emit1(sink, SK_TAG_G, '1', 0, &it[i].c);
		else if (g == 2)
			for (int i = s; i + 1 < j; i++)
				emit2(sink, SK_TAG_G, '2', 0, &it[i].c, &it[i + 1].c);
		else if (g == 3)
			for (int i = s; i + 2 < j; i++)
				emit3(sink, SK_TAG_G, '3', 0, &it[i].c, &it[i + 1].c, &it[i + 2].c);
		else if (g >= 4)
			for (int i = s; i + 3 < j; i++)
			{
				StomataChar run[4] = {it[i].c, it[i + 1].c, it[i + 2].c, it[i + 3].c};

				emitn(sink, SK_TAG_G, '4', 0, run, 4);
			}
	}

	/* skip-grams between literals d = 2..skip_depth apart (gaps may be '_' or literals) */
	for (int a = 0; a < total; a++)
	{
		if (it[a].type != IT_LIT)
			continue;
		for (int d = 2; d <= p->skip_depth && a + d < total; d++)
			if (it[a + d].type == IT_LIT)
				emit2(sink, SK_TAG_S, (uint8) ('0' + d), 0, &it[a].c, &it[a + d].c);
	}
	pfree(it);
}

/*
 * Whole anchored prefix key for a piece anchored at position 0 of a direction
 * string.  `items` are in direction order; `to_end` means the piece is the
 * whole string, so the terminator follows it.
 */
static void
compile_anchor(const StomataParams *p, uint8 dir, const PatItem *items, int n, bool to_end,
			   StomataKeySink *sink)
{
	StomataChar run[8];
	int			r = 0;

	while (r < n && r < p->anchor_len && items[r].type == IT_LIT)
	{
		run[r] = items[r].c;
		r++;
	}
	if (r == n && to_end && r < p->anchor_len)
		run[r++] = term_char;
	if (r >= 2)
		emitn(sink, SK_TAG_H, dir, r, run, r);
}

/*
 * Compile a LIKE pattern (backslash escape) into a conjunction of keys.
 * An empty result means "no usable keys": the caller must scan every page.
 */
/* one-character pseudo keys (N, A, W) */
void
stomata_pseudo_key(StomataKey *k, uint8 tag)
{
	memset(k, 0, sizeof(StomataKey));
	k->len = 1;
	k->data[0] = tag;
}

/*
 * Is this key kept in the exact (row TID) tier?  Row-bound, selective keys
 * are: trigrams and 4-grams, whole anchored prefixes/suffixes, the first and
 * last character, and the non-ASCII marker.  Everything else is page tier.
 */
bool
stomata_key_exact(const StomataKey *k, const StomataParams *p)
{
	uint8		tag = k->data[0];
	uint8		sub = k->data[1];
	uint32		num = ((uint32) k->data[2] << 8) | k->data[3];

	if (!p->exact)
		return false;
	switch (tag)
	{
		case SK_TAG_G:
			return sub == '3' || sub == '4' || (sub == '2' && p->exact_bigrams);
		case SK_TAG_H:
		case SK_TAG_N:
			return true;
		case SK_TAG_P:
			return num == 0;
		default:
			return false;
	}
}

bool
stomata_compile_like(const char *pat, int len, const StomataParams *p, bool icase,
					 StomataKeyList *out)
{
	StomataChar *pc;
	int			npc;
	PatItem    *items;
	int			nitems = 0;
	int		   *pstart;
	int		   *plen;
	int			npieces = 0;
	bool		exact;
	StomataKeySink sink;
	int			k = p->k;

	sink.emit = keylist_sink_emit;
	sink.arg = out;

	/* decode + fold; escapes are resolved on the character stream */
	stomata_decode_string(pat, len, &pc, &npc);
	items = palloc(sizeof(PatItem) * (npc + 2));
	for (int i = 0; i < npc; i++)
	{
		StomataChar c = pc[i];

		if (c.len == 1 && c.b[0] == '\\' && i + 1 == npc)
		{
			/*
			 * A trailing escape is an error in LIKE.  Emit no keys, so every
			 * page is a candidate and the executor raises the usual error.
			 */
			pfree(items);
			pfree(pc);
			return true;
		}
		if (c.len == 1 && c.b[0] == '\\' && i + 1 < npc)
		{
			items[nitems].type = IT_LIT;
			items[nitems].c = pc[++i];
			if (icase && (items[nitems].c.len > 1 || items[nitems].c.b[0] >= 0x80))
				items[nitems].type = IT_ONE;
			nitems++;
		}
		else if (c.len == 1 && c.b[0] == '%')
		{
			if (nitems == 0 || items[nitems - 1].type != IT_ANY)
				items[nitems++].type = IT_ANY;
		}
		else if (c.len == 1 && c.b[0] == '_')
			items[nitems++].type = IT_ONE;
		else
		{
			items[nitems].type = IT_LIT;
			items[nitems].c = c;
			/* ILIKE: a non-ASCII literal may fold unpredictably; keep its position only */
			if (icase && (c.len > 1 || c.b[0] >= 0x80))
				items[nitems].type = IT_ONE;
			nitems++;
		}
	}

	/* split on '%' */
	pstart = palloc(sizeof(int) * (nitems + 2));
	plen = palloc(sizeof(int) * (nitems + 2));
	pstart[0] = 0;
	for (int i = 0; i < nitems; i++)
	{
		if (items[i].type == IT_ANY)
		{
			plen[npieces] = i - pstart[npieces];
			npieces++;
			pstart[npieces] = i + 1;
		}
	}
	plen[npieces] = nitems - pstart[npieces];
	npieces++;
	exact = (npieces == 1);

	{
		PatItem    *head = items + pstart[0];
		int			nhead = plen[0];
		PatItem    *tail = items + pstart[npieces - 1];
		int			ntail = plen[npieces - 1];
		PatItem    *tmp = palloc(sizeof(PatItem) * (Max(nhead, ntail) + 2));

		if (exact)
		{
			/* forward: whole || '\0' */
			memcpy(tmp, head, sizeof(PatItem) * nhead);
			tmp[nhead].type = IT_LIT;
			tmp[nhead].c = term_char;
			compile_positional(p, SK_DIR_F, tmp, nhead + 1, 0, &sink);
			/* reverse: reverse(whole) || '\0' */
			for (int i = 0; i < nhead; i++)
				tmp[i] = head[nhead - 1 - i];
			tmp[nhead].type = IT_LIT;
			tmp[nhead].c = term_char;
			compile_positional(p, SK_DIR_R, tmp, nhead + 1, 0, &sink);
			compile_anchor(p, SK_DIR_R, tmp, nhead, true, &sink);
			compile_anchor(p, SK_DIR_F, head, nhead, true, &sink);
		}
		else
		{
			if (nhead > 0)
			{
				compile_positional(p, SK_DIR_F, head, nhead, 0, &sink);
				compile_anchor(p, SK_DIR_F, head, nhead, false, &sink);
			}
			if (ntail > 0)
			{
				for (int i = 0; i < ntail; i++)
					tmp[i] = tail[ntail - 1 - i];
				compile_positional(p, SK_DIR_R, tmp, ntail, 0, &sink);
				compile_anchor(p, SK_DIR_R, tmp, ntail, false, &sink);
			}
		}

		/* composite keys */
		if (p->composite && (nhead > 0 || exact))
		{
			int			hl = 0;
			int			tl = 0;

			while (hl < nhead && head[hl].type == IT_LIT)
				hl++;
			if (exact || ntail > 0)
				while (tl < ntail && tail[ntail - 1 - tl].type == IT_LIT)
					tl++;

			if (hl >= 1 && tl >= 1)
			{
				emit2(&sink, SK_TAG_X, SK_X_HT1, 0, &head[0].c, &tail[ntail - 1].c);
				if (hl >= 2 && tl >= 2)
					emit4(&sink, SK_TAG_X, SK_X_HT2, 0, &head[0].c, &head[1].c,
						  &tail[ntail - 2].c, &tail[ntail - 1].c);
				if (hl >= 2)
					emit3(&sink, SK_TAG_X, SK_X_H2T1, 0, &head[0].c, &head[1].c,
						  &tail[ntail - 1].c);
				if (tl >= 2)
					emit3(&sink, SK_TAG_X, SK_X_H1T2, 0, &head[0].c,
						  &tail[ntail - 2].c, &tail[ntail - 1].c);
			}
			if (exact && nhead > 0)
			{
				uint32		L = nhead / k;

				if (hl >= 1)
					emit1(&sink, SK_TAG_X, SK_X_HL, L, &head[0].c);
				if (tl >= 1)
					emit1(&sink, SK_TAG_X, SK_X_TL, L, &tail[ntail - 1].c);
			}
		}

		/* rollups over every piece; an end-anchored piece is followed by '\0' */
		if (p->rollup > 0 || p->skip_depth >= 2)
		{
			bool		ends_open = (nitems > 0 && items[nitems - 1].type == IT_ANY);

			for (int i = 0; i < npieces; i++)
			{
				bool		term_after = (i == npieces - 1) && !ends_open;

				compile_rollup(p, items + pstart[i], plen[i], term_after, &sink);
			}
		}
		pfree(tmp);
	}

	stomata_keylist_sort_unique(out);
	pfree(pstart);
	pfree(plen);
	pfree(items);
	pfree(pc);
	return true;
}

/* ------------------------------------------------------------------------
 * Rendering (diagnostics and regression tests)
 * ------------------------------------------------------------------------
 */
static void
render_chars(StringInfo buf, const uint8 *d, int len, const char *sep, int nsep_after)
{
	int			i = 0;
	int			nc = 0;

	while (i < len)
	{
		int			l;

		if (d[i] == 0)
		{
			appendStringInfoString(buf, "\\0");
			i++;
		}
		else
		{
			l = pg_mblen((const char *) d + i);
			if (l < 1 || i + l > len)
				l = 1;
			appendBinaryStringInfo(buf, (const char *) d + i, l);
			i += l;
		}
		nc++;
		if (sep && nc == nsep_after && i < len)
			appendStringInfoString(buf, sep);
	}
}

char *
stomata_key_to_cstring(const StomataKey *k)
{
	StringInfoData buf;
	uint8		tag = k->data[0];
	uint8		sub = k->data[1];
	uint32		num = ((uint32) k->data[2] << 8) | k->data[3];
	const uint8 *chars = k->data + 4;
	int			clen = k->len - 4;

	initStringInfo(&buf);
	switch (tag)
	{
		case SK_TAG_P:
		case SK_TAG_U:
		case SK_TAG_T:
		case SK_TAG_C:
			appendStringInfo(&buf, "%c:%c:%u:", tag, sub, num);
			render_chars(&buf, chars, clen, NULL, 0);
			break;
		case SK_TAG_G:
			appendStringInfo(&buf, "G%c:", sub);
			render_chars(&buf, chars, clen, NULL, 0);
			break;
		case SK_TAG_H:
			appendStringInfo(&buf, "H:%c:", sub);
			render_chars(&buf, chars, clen, NULL, 0);
			break;
		case SK_TAG_S:
			appendStringInfo(&buf, "S%c:", sub);
			render_chars(&buf, chars, clen, NULL, 0);
			break;
		case SK_TAG_X:
			switch (sub)
			{
				case SK_X_HT1:
					appendStringInfoString(&buf, "X.ht:");
					render_chars(&buf, chars, clen, ",", 1);
					break;
				case SK_X_HT2:
					appendStringInfoString(&buf, "X.HT:");
					render_chars(&buf, chars, clen, ",", 2);
					break;
				case SK_X_H2T1:
					appendStringInfoString(&buf, "X.Ht:");
					render_chars(&buf, chars, clen, ",", 2);
					break;
				case SK_X_H1T2:
					appendStringInfoString(&buf, "X.hT:");
					render_chars(&buf, chars, clen, ",", 1);
					break;
				case SK_X_HL:
					appendStringInfoString(&buf, "X.HL:");
					render_chars(&buf, chars, clen, NULL, 0);
					appendStringInfo(&buf, ":%u", num);
					break;
				default:
					appendStringInfoString(&buf, "X.TL:");
					render_chars(&buf, chars, clen, NULL, 0);
					appendStringInfo(&buf, ":%u", num);
					break;
			}
			break;
		case SK_TAG_A:
			appendStringInfoString(&buf, "A");
			break;
		case SK_TAG_N:
			appendStringInfoString(&buf, "N");
			break;
		case SK_TAG_W:
			appendStringInfoString(&buf, "W");
			break;
		default:
			appendStringInfo(&buf, "?%d", tag);
	}
	return buf.data;
}
