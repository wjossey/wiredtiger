/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define	FORALL_CURSORS(clsm, c, i)					\
	for ((i) = (clsm)->nchunks - 1; (i) >= 0; (i)--)		\
		if (((c) = (clsm)->cursors[i]) != NULL)

#define	WT_LSM_CMP(s, lsm_tree, k1, k2, cmp)				\
	(((lsm_tree)->collator == NULL) ?				\
	(((cmp) = __wt_btree_lex_compare((k1), (k2))), 0) :		\
	(lsm_tree)->collator->compare((lsm_tree)->collator,		\
	    &(s)->iface, (k1), (k2), &(cmp)))

#define	WT_LSM_CURCMP(s, lsm_tree, c1, c2, cmp)				\
	WT_LSM_CMP(s, lsm_tree, &(c1)->key, &(c2)->key, cmp)

/*
 * LSM API enter/leave: check that the cursor is in sync with the tree.
 */
#define	WT_LSM_ENTER(clsm, cursor, session, n)				\
	clsm = (WT_CURSOR_LSM *)cursor;					\
	CURSOR_API_CALL_NOCONF(cursor, session, n, NULL);		\
	WT_ERR(__clsm_enter(clsm))

#define	WT_LSM_END(clsm, session)					\
	API_END(session)

static int __clsm_open_cursors(WT_CURSOR_LSM *);
static int __clsm_search(WT_CURSOR *);

static inline int
__clsm_enter(WT_CURSOR_LSM *clsm)
{
	if (!F_ISSET(clsm, WT_CLSM_MERGE) &&
	    clsm->dsk_gen != clsm->lsm_tree->dsk_gen)
		WT_RET(__clsm_open_cursors(clsm));

	return (0);
}

/*
 * TODO: use something other than an empty value as a tombstone: we need
 * to support empty values from the application.
 */
static WT_ITEM __lsm_tombstone = { "", 0, 0, NULL, 0 };

#define	WT_LSM_NEEDVALUE(c) do {					\
	WT_CURSOR_NEEDVALUE(c);						\
	if (__clsm_deleted(&(c)->value))				\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 0));		\
} while (0)

/*
 * __clsm_deleted --
 *	Check whether the current value is a tombstone.
 */
static inline int
__clsm_deleted(WT_ITEM *item)
{
	return (item->size == 0);
}

/*
 * __clsm_close_cursors --
 *	Close all of the btree cursors currently open.
 */
static int
__clsm_close_cursors(WT_CURSOR_LSM *clsm)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	int i;

	if (clsm->cursors == NULL)
		return (0);

	/* Detach from our old primary. */
	if (clsm->primary_chunk != NULL) {
		WT_ATOMIC_SUB(clsm->primary_chunk->ncursor, 1);
		clsm->primary_chunk = NULL;
	}

	FORALL_CURSORS(clsm, c, i) {
		clsm->cursors[i] = NULL;
		WT_RET(c->close(c));
		if ((bloom = clsm->blooms[i]) != NULL) {
			clsm->blooms[i] = NULL;
			WT_RET(__wt_bloom_close(bloom));
		}
	}

	clsm->current = NULL;
	return (0);
}

/*
 * __clsm_open_cursors --
 *	Open cursors for the current set of files.
 */
static int
__clsm_open_cursors(WT_CURSOR_LSM *clsm)
{
	WT_CURSOR *c, **cp;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	const char *ckpt_cfg[] = { "checkpoint=WiredTigerCheckpoint", NULL };
	int i, nchunks;

	session = (WT_SESSION_IMPL *)clsm->iface.session;
	lsm_tree = clsm->lsm_tree;
	c = &clsm->iface;

	/* Copy the key, so we don't lose the cursor position. */
	if (F_ISSET(c, WT_CURSTD_KEY_SET)) {
		if (c->key.data != c->key.mem)
			WT_RET(__wt_buf_set(
			    session, &c->key, c->key.data, c->key.size));
		F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);
	}

	WT_RET(__clsm_close_cursors(clsm));

	__wt_spin_lock(session, &lsm_tree->lock);
	/* Merge cursors have already figured out how many chunks they need. */
	if (F_ISSET(clsm, WT_CLSM_MERGE))
		nchunks = clsm->nchunks;
	else
		nchunks = lsm_tree->nchunks;

	if (clsm->cursors == NULL || nchunks > clsm->nchunks) {
		WT_ERR(__wt_realloc(session, NULL,
		    nchunks * sizeof(WT_BLOOM *), &clsm->blooms));
		WT_ERR(__wt_realloc(session, NULL,
		    nchunks * sizeof(WT_CURSOR *), &clsm->cursors));
	}
	clsm->nchunks = nchunks;

	for (i = 0, cp = clsm->cursors; i != clsm->nchunks; i++, cp++) {
		/*
		 * Read from the checkpoint if the file has been written.
		 * Once all cursors switch, the in-memory tree can be evicted.
		 */
		chunk = lsm_tree->chunk[i];
		WT_ERR(__wt_curfile_open(session,
		    chunk->uri, &clsm->iface,
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? ckpt_cfg : NULL, cp));
		if (chunk->bloom_uri != NULL && !F_ISSET(clsm, WT_CLSM_MERGE))
			WT_ERR(__wt_bloom_open(session, chunk->bloom_uri,
			    lsm_tree->bloom_bit_count,
			    lsm_tree->bloom_hash_count,
			    c, &clsm->blooms[i]));

		/* Child cursors always use overwrite and raw mode. */
		F_SET(*cp, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);
	}

	/* The last chunk is our new primary. */
	WT_ASSERT(session,
	    !F_ISSET(clsm, WT_CLSM_UPDATED) ||
	    !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK));

	clsm->primary_chunk = chunk;
	WT_ATOMIC_ADD(clsm->primary_chunk->ncursor, 1);

	/* Peek into the btree layer to track the in-memory size. */
	if (lsm_tree->memsizep == NULL)
		(void)__wt_btree_get_memsize(
		    session, session->btree, &lsm_tree->memsizep);

	clsm->dsk_gen = lsm_tree->dsk_gen;
err:	__wt_spin_unlock(session, &lsm_tree->lock);
	return (ret);
}

/* __wt_clsm_init_merge --
 *	Initialize an LSM cursor for a (major) merge.
 */
int
__wt_clsm_init_merge(WT_CURSOR *cursor, int nchunks)
{
	WT_CURSOR_LSM *clsm;

	clsm = (WT_CURSOR_LSM *)cursor;
	F_SET(clsm, WT_CLSM_MERGE);
	clsm->nchunks = nchunks;

	return (__clsm_open_cursors(clsm));
}

/*
 * __clsm_get_current --
 *	Find the smallest / largest of the cursors and copy its key/value.
 */
static int
__clsm_get_current(
    WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, int smallest, int *deletedp)
{
	WT_CURSOR *c, *current;
	int i;
	int cmp, multiple;

	current = NULL;
	FORALL_CURSORS(clsm, c, i) {
		if (!F_ISSET(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET))
			continue;
		if (current == NULL) {
			cmp = (smallest ? -1 : 1);
		} else
			WT_RET(WT_LSM_CURCMP(session,
			    clsm->lsm_tree, c, current, cmp));
		if (smallest ? cmp < 0 : cmp > 0) {
			current = c;
			multiple = 0;
		} else if (cmp == 0)
			multiple = 1;
	}

	c = &clsm->iface;
	if ((clsm->current = current) == NULL) {
		F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		return (WT_NOTFOUND);
	}

	if (multiple)
		F_SET(clsm, WT_CLSM_MULTIPLE);
	else
		F_CLR(clsm, WT_CLSM_MULTIPLE);

	WT_RET(current->get_key(current, &c->key));
	WT_RET(current->get_value(current, &c->value));

	if ((*deletedp = __clsm_deleted(&c->value)) == 0)
		F_SET(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	return (0);
}

/*
 * __clsm_compare --
 *	WT_CURSOR->compare implementation for the LSM cursor type.
 */
static int
__clsm_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_CURSOR_LSM *alsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int cmp;

	/* There's no need to sync with the LSM tree, avoid WT_LSM_ENTER. */
	alsm = (WT_CURSOR_LSM *)a;
	CURSOR_API_CALL_NOCONF(a, session, compare, NULL);

	/*
	 * Confirm both cursors refer to the same source and have keys, then
	 * compare the keys.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "comparison method cursors must reference the same object");

	WT_ERR(WT_CURSOR_NEEDKEY(a));
	WT_ERR(WT_CURSOR_NEEDKEY(b));

	WT_ERR(WT_LSM_CMP(session, alsm->lsm_tree, &a->key, &b->key, cmp));
	*cmpp = cmp;

err:	API_END(session);
	return (ret);
}

/*
 * __clsm_next --
 *	WT_CURSOR->next method for the LSM cursor type.
 */
static int
__clsm_next(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int check, cmp, deleted, i;

	WT_LSM_ENTER(clsm, cursor, session, next);

	/* If we aren't positioned for a forward scan, get started. */
	if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT)) {
		F_CLR(clsm, WT_CLSM_MULTIPLE);
		FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->next(c);
			} else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp < 0)
						ret = c->next(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm,
							    WT_CLSM_MULTIPLE);
					}
				}
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_NEXT);
		F_CLR(clsm, WT_CLSM_ITERATE_PREV);

		/* We just positioned *at* the key, now move. */
		if (clsm->current != NULL)
			goto retry;
	} else {
retry:		/*
		 * If there are multiple cursors on that key, move them
		 * forward.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c,
				    WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET))
					continue;
				if (check) {
					WT_ERR(WT_LSM_CURCMP(session,
					    clsm->lsm_tree, c, clsm->current,
					    cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->next(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor forward. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->next(c));
	}

	/* Find the cursor(s) with the smallest key. */
	if ((ret = __clsm_get_current(session, clsm, 1, &deleted)) == 0 &&
	    deleted)
		goto retry;
err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_prev --
 *	WT_CURSOR->prev method for the LSM cursor type.
 */
static int
__clsm_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int check, cmp, deleted, i;

	WT_LSM_ENTER(clsm, cursor, session, next);

	/* If we aren't positioned for a reverse scan, get started. */
	if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_PREV)) {
		F_CLR(clsm, WT_CLSM_MULTIPLE);
		FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->prev(c);
			} else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp > 0)
						ret = c->prev(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm,
							    WT_CLSM_MULTIPLE);
					}
				}
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_PREV);
		F_CLR(clsm, WT_CLSM_ITERATE_NEXT);

		/* We just positioned *at* the key, now move. */
		if (clsm->current != NULL)
			goto retry;
	} else {
retry:		/*
		 * If there are multiple cursors on that key, move them
		 * backwards.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c,
				    WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET))
					continue;
				if (check) {
					WT_ERR(WT_LSM_CURCMP(session,
					    clsm->lsm_tree, c, clsm->current,
					    cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->prev(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor backwards. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->prev(c));
	}

	/* Find the cursor(s) with the largest key. */
	if ((ret = __clsm_get_current(session, clsm, 0, &deleted)) == 0 &&
	    deleted)
		goto retry;
err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_reset --
 *	WT_CURSOR->reset method for the LSM cursor type.
 */
static int
__clsm_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_ENTER(clsm, cursor, session, reset);
	if ((c = clsm->current) != NULL) {
		ret = c->reset(c);
		clsm->current = NULL;
	}
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);
err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_search --
 *	WT_CURSOR->search method for the LSM cursor type.
 */
static int
__clsm_search(WT_CURSOR *cursor)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int i;

	WT_LSM_ENTER(clsm, cursor, session, search);
	WT_CURSOR_NEEDKEY(cursor);
	FORALL_CURSORS(clsm, c, i) {
		/* If there is a Bloom filter, see if we can skip the read. */
		if ((bloom = clsm->blooms[i]) != NULL) {
			ret = __wt_bloom_get(bloom, &cursor->key);
			if (ret == WT_NOTFOUND)
				continue;
			WT_RET(ret);
		}
		c->set_key(c, &cursor->key);
		if ((ret = c->search(c)) == 0) {
			WT_ERR(c->get_key(c, &cursor->key));
			WT_ERR(c->get_value(c, &cursor->value));
			clsm->current = c;
			if (__clsm_deleted(&cursor->value))
				ret = WT_NOTFOUND;
			goto done;
		} else if (ret != WT_NOTFOUND)
			goto err;
	}
	ret = WT_NOTFOUND;

done:
err:	WT_LSM_END(clsm, session);
	if (ret == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	return (ret);
}

/*
 * __clsm_search_near --
 *	WT_CURSOR->search_near method for the LSM cursor type.
 */
static int
__clsm_search_near(WT_CURSOR *cursor, int *exactp)
{
	WT_CURSOR *c, *larger, *smaller;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_ITEM v;
	WT_SESSION_IMPL *session;
	int cmp, deleted, i;

	WT_LSM_ENTER(clsm, cursor, session, search_near);
	WT_CURSOR_NEEDKEY(cursor);

	/*
	 * search_near is somewhat fiddly: we can't just return a nearby key
	 * from the in-memory chunk because there could be a closer key on
	 * disk.
	 *
	 * As we search down the chunks, we stop as soon as we find an exact
	 * match.  Otherwise, we maintain the smallest cursor larger than the
	 * search key and the largest cursor smaller than the search key.  At
	 * the bottom, if one of those is set, we use it, otherwise we return
	 * WT_NOTFOUND.
	 */
	larger = smaller = NULL;
	FORALL_CURSORS(clsm, c, i) {
		c->set_key(c, &cursor->key);
		if ((ret = c->search_near(c, &cmp)) == WT_NOTFOUND) {
			ret = 0;
			continue;
		} else if (ret != 0)
			goto err;

		WT_ERR(c->get_value(c, &v));
		deleted = __clsm_deleted(&v);

		if (cmp == 0 && !deleted) {
			clsm->current = c;
			*exactp = 0;
			goto done;
		}

		/*
		 * If we land on a deleted item, try going forwards or
		 * backwards to find one that isn't deleted.
		 */
		while (deleted && (ret = c->next(c)) == 0) {
			cmp = 1;
			WT_ERR(c->get_value(c, &v));
			deleted = __clsm_deleted(&v);
		}
		WT_ERR_NOTFOUND_OK(ret);
		while (deleted && (ret = c->prev(c)) == 0) {
			cmp = -1;
			WT_ERR(c->get_value(c, &v));
			deleted = __clsm_deleted(&v);
		}
		WT_ERR_NOTFOUND_OK(ret);
		if (deleted)
			continue;
		if (cmp > 0) {
			if (larger == NULL)
				larger = c;
			else {
				WT_ERR(WT_LSM_CURCMP(session,
				    clsm->lsm_tree, c, larger, cmp));
				if (cmp < 0)
					larger = c;
			}
		} else {
			if (smaller == NULL)
				smaller = c;
			else {
				WT_ERR(WT_LSM_CURCMP(session,
				    clsm->lsm_tree, c, smaller, cmp));
				if (cmp > 0)
					smaller = c;
			}
		}
	}

	if (smaller != NULL) {
		clsm->current = smaller;
		*exactp = -1;
	} else if (larger != NULL) {
		clsm->current = larger;
		*exactp = 1;
	} else
		ret = WT_NOTFOUND;

done:
err:	WT_LSM_END(clsm, session);
	if (ret == 0) {
		c = clsm->current;
		WT_TRET(c->get_key(c, &cursor->key));
		WT_TRET(c->get_value(c, &cursor->value));
	}
	if (ret == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	return (ret);
}

/*
 * __clsm_put --
 *	Put an entry into the in-memory tree, trigger a file switch if
 *	necessary.
 */
static inline int
__clsm_put(
    WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, WT_ITEM *key, WT_ITEM *value)
{
	WT_BTREE *btree;
	WT_CURSOR *primary;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	uint32_t *memsizep;

	lsm_tree = clsm->lsm_tree;

	/*
	 * If this is the first update in this cursor, check if a new in-memory
	 * chunk is needed.
	 */
	if (!F_ISSET(clsm, WT_CLSM_UPDATED)) {
		__wt_spin_lock(session, &lsm_tree->lock);
		if (clsm->dsk_gen == lsm_tree->dsk_gen)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_lsm_tree_switch(session, lsm_tree));
		__wt_spin_unlock(session, &lsm_tree->lock);
		WT_RET(ret);
		F_SET(clsm, WT_CLSM_UPDATED);

		/* We changed the structure, or someone else did: update. */
		WT_RET(__clsm_enter(clsm));
	}

	primary = clsm->cursors[clsm->nchunks - 1];
	primary->set_key(primary, key);
	primary->set_value(primary, value);
	WT_RET(primary->insert(primary));

	/*
	 * The count is in a shared structure, but it's only approximate, so
	 * don't worry about protecting access.
	 */
	++clsm->primary_chunk->count;

	/*
	 * Set the position for future scans.  If we were already positioned in
	 * a non-primary chunk, we may now have multiple cursors matching the
	 * key.
	 */
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);
	clsm->current = primary;

	if ((memsizep = lsm_tree->memsizep) != NULL &&
	    *memsizep > lsm_tree->threshold) {
		/*
		 * Close our cursors: if we are the only open cursor, this
		 * means the btree handle is unlocked.
		 *
		 * XXX this is insufficient if multiple cursors are open, need
		 * to move some operations (such as clearing the
		 * "cache_resident" flag) into the worker thread.
		 */
		btree = ((WT_CURSOR_BTREE *)primary)->btree;
		WT_RET(__wt_btree_release_memsize(session, btree));

		/*
		 * Take the LSM lock first: we can't acquire it while
		 * holding the schema lock, or we will deadlock.
		 */
		__wt_spin_lock(session, &lsm_tree->lock);
		/* Make sure we don't race. */
		if (clsm->dsk_gen == lsm_tree->dsk_gen)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_lsm_tree_switch(session, lsm_tree));
		__wt_spin_unlock(session, &lsm_tree->lock);
	}

	return (ret);
}

/*
 * __clsm_insert --
 *	WT_CURSOR->insert method for the LSM cursor type.
 */
static int
__clsm_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_ENTER(clsm, cursor, session, insert);
	WT_CURSOR_NEEDKEY(cursor);
	WT_LSM_NEEDVALUE(cursor);

	if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
	    (ret = __clsm_search(cursor)) != WT_NOTFOUND) {
		if (ret == 0)
			ret = WT_DUPLICATE_KEY;
		return (ret);
	}

	ret = __clsm_put(session, clsm, &cursor->key, &cursor->value);

err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_update --
 *	WT_CURSOR->update method for the LSM cursor type.
 */
static int
__clsm_update(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_ENTER(clsm, cursor, session, update);
	WT_CURSOR_NEEDKEY(cursor);
	WT_LSM_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) ||
	    (ret = __clsm_search(cursor)) == 0)
		ret = __clsm_put(session, clsm, &cursor->key, &cursor->value);

err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_remove --
 *	WT_CURSOR->remove method for the LSM cursor type.
 */
static int
__clsm_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_ENTER(clsm, cursor, session, remove);
	WT_CURSOR_NEEDKEY(cursor);

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) ||
	    (ret = __clsm_search(cursor)) == 0)
		ret = __clsm_put(session, clsm, &cursor->key, &__lsm_tombstone);

err:	WT_LSM_END(clsm, session);

	return (ret);
}

/*
 * __clsm_close --
 *	WT_CURSOR->close method for the LSM cursor type.
 */
static int
__clsm_close(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * Don't use the normal __clsm_enter path: that is wasted work when
	 * closing, and the cursor may never have been used.
	 */
	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, close, NULL);
	WT_TRET(__clsm_close_cursors(clsm));
	__wt_free(session, clsm->blooms);
	__wt_free(session, clsm->cursors);
	/* The WT_LSM_TREE owns the URI. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));
	API_END(session);

	return (ret);
}

/*
 * __wt_clsm_open --
 *	WT_SESSION->open_cursor method for LSM cursors.
 */
int
__wt_clsm_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__clsm_compare,
		__clsm_next,
		__clsm_prev,
		__clsm_reset,
		__clsm_search,
		__clsm_search_near,
		__clsm_insert,
		__clsm_update,
		__clsm_remove,
		__clsm_close,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* raw recno buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	clsm = NULL;

	if (!WT_PREFIX_MATCH(uri, "lsm:"))
		return (EINVAL);

	/* Get the LSM tree. */
	WT_RET(__wt_lsm_tree_get(session, uri, &lsm_tree));

	WT_RET(__wt_calloc_def(session, 1, &clsm));

	cursor = &clsm->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = lsm_tree->name;
	cursor->key_format = lsm_tree->key_format;
	cursor->value_format = lsm_tree->value_format;

	clsm->lsm_tree = lsm_tree;

	/*
	 * The tree's dsk_gen starts at one, so starting the cursor on zero
	 * will force a call into open_cursors on the first operation.
	 */
	clsm->dsk_gen = 0;

	STATIC_ASSERT(offsetof(WT_CURSOR_LSM, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, NULL, cfg, cursorp));

	/*
	 * LSM cursors default to overwrite: if no setting was supplied, turn
	 * it on.
	 */
	if (cfg[1] != NULL || __wt_config_getones(
	    session, cfg[1], "overwrite", &cval) == WT_NOTFOUND)
		F_SET(cursor, WT_CURSTD_OVERWRITE);

	if (0) {
err:		(void)__clsm_close(cursor);
	}

	return (ret);
}