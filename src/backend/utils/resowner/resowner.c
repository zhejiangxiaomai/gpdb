/*-------------------------------------------------------------------------
 *
 * resowner.c
 *	  POSTGRES resource owner management code.
 *
 * Query-lifespan resources are tracked by associating them with
 * ResourceOwner objects.  This provides a simple mechanism for ensuring
 * that such resources are freed at the right time.
 * See utils/resowner/README for more info.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/resowner/resowner.c,v 1.18 2006/03/05 15:58:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/resowner.h"
#include "access/gistscan.h"
#include "access/hash.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/relcache.h"


/*
 * ResourceOwner objects look like this
 */
typedef struct ResourceOwnerData
{
	ResourceOwner parent;		/* NULL if no parent (toplevel owner) */
	ResourceOwner firstchild;	/* head of linked list of children */
	ResourceOwner nextchild;	/* next child of same parent */
	const char *name;			/* name (just for debugging) */

	/* We have built-in support for remembering owned buffers */
	int			nbuffers;		/* number of owned buffer pins */
	Buffer	   *buffers;		/* dynamically allocated array */
	int			maxbuffers;		/* currently allocated array size */

	/* We have built-in support for remembering catcache references */
	int			ncatrefs;		/* number of owned catcache pins */
	HeapTuple  *catrefs;		/* dynamically allocated array */
	int			maxcatrefs;		/* currently allocated array size */

	int			ncatlistrefs;	/* number of owned catcache-list pins */
	CatCList  **catlistrefs;	/* dynamically allocated array */
	int			maxcatlistrefs; /* currently allocated array size */

	/* We have built-in support for remembering relcache references */
	int			nrelrefs;		/* number of owned relcache pins */
	Relation   *relrefs;		/* dynamically allocated array */
	int			maxrelrefs;		/* currently allocated array size */
} ResourceOwnerData;


/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

ResourceOwner CurrentResourceOwner = NULL;
ResourceOwner CurTransactionResourceOwner = NULL;
ResourceOwner TopTransactionResourceOwner = NULL;

/*
 * List of add-on callbacks for resource releasing
 */
typedef struct ResourceReleaseCallbackItem
{
	struct ResourceReleaseCallbackItem *next;
	ResourceReleaseCallback callback;
	void	   *arg;
} ResourceReleaseCallbackItem;

static ResourceReleaseCallbackItem *ResourceRelease_callbacks = NULL;


/* Internal routines */
static void ResourceOwnerReleaseInternal(ResourceOwner owner,
							 ResourceReleasePhase phase,
							 bool isCommit,
							 bool isTopLevel);
static void PrintRelCacheLeakWarning(Relation rel);


/*****************************************************************************
 *	  EXPORTED ROUTINES														 *
 *****************************************************************************/


/*
 * ResourceOwnerCreate
 *		Create an empty ResourceOwner.
 *
 * All ResourceOwner objects are kept in TopMemoryContext, since they should
 * only be freed explicitly.
 */
ResourceOwner
ResourceOwnerCreate(ResourceOwner parent, const char *name)
{
	ResourceOwner owner;

	owner = (ResourceOwner) MemoryContextAllocZero(TopMemoryContext,
												   sizeof(ResourceOwnerData));
	owner->name = name;

	if (parent)
	{
		owner->parent = parent;
		owner->nextchild = parent->firstchild;
		parent->firstchild = owner;
	}

	return owner;
}

/*
 * ResourceOwnerRelease
 *		Release all resources owned by a ResourceOwner and its descendants,
 *		but don't delete the owner objects themselves.
 *
 * Note that this executes just one phase of release, and so typically
 * must be called three times.	We do it this way because (a) we want to
 * do all the recursion separately for each phase, thereby preserving
 * the needed order of operations; and (b) xact.c may have other operations
 * to do between the phases.
 *
 * phase: release phase to execute
 * isCommit: true for successful completion of a query or transaction,
 *			false for unsuccessful
 * isTopLevel: true if completing a main transaction, else false
 *
 * isCommit is passed because some modules may expect that their resources
 * were all released already if the transaction or portal finished normally.
 * If so it is reasonable to give a warning (NOT an error) should any
 * unreleased resources be present.  When isCommit is false, such warnings
 * are generally inappropriate.
 *
 * isTopLevel is passed when we are releasing TopTransactionResourceOwner
 * at completion of a main transaction.  This generally means that *all*
 * resources will be released, and so we can optimize things a bit.
 */
void
ResourceOwnerRelease(ResourceOwner owner,
					 ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel)
{
	/* Rather than PG_TRY at every level of recursion, set it up once */
	ResourceOwner save;

	save = CurrentResourceOwner;
	PG_TRY();
	{
		ResourceOwnerReleaseInternal(owner, phase, isCommit, isTopLevel);
	}
	PG_CATCH();
	{
		CurrentResourceOwner = save;
		PG_RE_THROW();
	}
	PG_END_TRY();
	CurrentResourceOwner = save;
}

static void
ResourceOwnerReleaseInternal(ResourceOwner owner,
							 ResourceReleasePhase phase,
							 bool isCommit,
							 bool isTopLevel)
{
	ResourceOwner child;
	ResourceOwner save;
	ResourceReleaseCallbackItem *item;

	/* Recurse to handle descendants */
	for (child = owner->firstchild; child != NULL; child = child->nextchild)
		ResourceOwnerReleaseInternal(child, phase, isCommit, isTopLevel);

	/*
	 * Make CurrentResourceOwner point to me, so that ReleaseBuffer etc don't
	 * get confused.  We needn't PG_TRY here because the outermost level will
	 * fix it on error abort.
	 */
	save = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	if (phase == RESOURCE_RELEASE_BEFORE_LOCKS)
	{
		/*
		 * Release buffer pins.  Note that ReleaseBuffer will remove the
		 * buffer entry from my list, so I just have to iterate till there are
		 * none.
		 *
		 * During a commit, there shouldn't be any remaining pins --- that
		 * would indicate failure to clean up the executor correctly --- so
		 * issue warnings.	In the abort case, just clean up quietly.
		 *
		 * We are careful to do the releasing back-to-front, so as to avoid
		 * O(N^2) behavior in ResourceOwnerForgetBuffer().
		 */
		while (owner->nbuffers > 0)
		{
			if (isCommit)
				PrintBufferLeakWarning(owner->buffers[owner->nbuffers - 1]);
			ReleaseBuffer(owner->buffers[owner->nbuffers - 1]);
		}

		/*
		 * Release relcache references.  Note that RelationClose will remove
		 * the relref entry from my list, so I just have to iterate till there
		 * are none.
		 *
		 * As with buffer pins, warn if any are left at commit time, and
		 * release back-to-front for speed.
		 */
		while (owner->nrelrefs > 0)
		{
			if (isCommit)
				PrintRelCacheLeakWarning(owner->relrefs[owner->nrelrefs - 1]);
			RelationClose(owner->relrefs[owner->nrelrefs - 1]);
		}
	}
	else if (phase == RESOURCE_RELEASE_LOCKS)
	{
		if (isTopLevel)
		{
			/*
			 * For a top-level xact we are going to release all locks (or at
			 * least all non-session locks), so just do a single lmgr call at
			 * the top of the recursion.
			 */
			if (owner == TopTransactionResourceOwner)
				ProcReleaseLocks(isCommit);
		}
		else
		{
			/*
			 * Release locks retail.  Note that if we are committing a
			 * subtransaction, we do NOT release its locks yet, but transfer
			 * them to the parent.
			 */
			Assert(owner->parent != NULL);
			if (isCommit)
				LockReassignCurrentOwner();
			else
				LockReleaseCurrentOwner();
		}
	}
	else if (phase == RESOURCE_RELEASE_AFTER_LOCKS)
	{
		/*
		 * Release catcache references.  Note that ReleaseCatCache will remove
		 * the catref entry from my list, so I just have to iterate till there
		 * are none.  Ditto for catcache lists.
		 *
		 * As with buffer pins, warn if any are left at commit time, and
		 * release back-to-front for speed.
		 */
		while (owner->ncatrefs > 0)
		{
			if (isCommit)
				PrintCatCacheLeakWarning(owner->catrefs[owner->ncatrefs - 1]);
			ReleaseCatCache(owner->catrefs[owner->ncatrefs - 1]);
		}
		while (owner->ncatlistrefs > 0)
		{
			if (isCommit)
				PrintCatCacheListLeakWarning(owner->catlistrefs[owner->ncatlistrefs - 1]);
			ReleaseCatCacheList(owner->catlistrefs[owner->ncatlistrefs - 1]);
		}

		/* Clean up index scans too */
		ReleaseResources_gist();
		ReleaseResources_hash();
	}

	/* Let add-on modules get a chance too */
	for (item = ResourceRelease_callbacks; item; item = item->next)
		(*item->callback) (phase, isCommit, isTopLevel, item->arg);

	CurrentResourceOwner = save;
}

/*
 * ResourceOwnerDelete
 *		Delete an owner object and its descendants.
 *
 * The caller must have already released all resources in the object tree.
 */
void
ResourceOwnerDelete(ResourceOwner owner)
{
	/* We had better not be deleting CurrentResourceOwner ... */
	Assert(owner != CurrentResourceOwner);

	/* And it better not own any resources, either */
	Assert(owner->nbuffers == 0);
	Assert(owner->ncatrefs == 0);
	Assert(owner->ncatlistrefs == 0);
	Assert(owner->nrelrefs == 0);

	/*
	 * Delete children.  The recursive call will delink the child from me, so
	 * just iterate as long as there is a child.
	 */
	while (owner->firstchild != NULL)
		ResourceOwnerDelete(owner->firstchild);

	/*
	 * We delink the owner from its parent before deleting it, so that if
	 * there's an error we won't have deleted/busted owners still attached to
	 * the owner tree.	Better a leak than a crash.
	 */
	ResourceOwnerNewParent(owner, NULL);

	/* And free the object. */
	if (owner->buffers)
		pfree(owner->buffers);
	if (owner->catrefs)
		pfree(owner->catrefs);
	if (owner->catlistrefs)
		pfree(owner->catlistrefs);
	if (owner->relrefs)
		pfree(owner->relrefs);

	pfree(owner);
}

/*
 * Fetch parent of a ResourceOwner (returns NULL if top-level owner)
 */
ResourceOwner
ResourceOwnerGetParent(ResourceOwner owner)
{
	return owner->parent;
}

/*
 * Reassign a ResourceOwner to have a new parent
 */
void
ResourceOwnerNewParent(ResourceOwner owner,
					   ResourceOwner newparent)
{
	ResourceOwner oldparent = owner->parent;

	if (oldparent)
	{
		if (owner == oldparent->firstchild)
			oldparent->firstchild = owner->nextchild;
		else
		{
			ResourceOwner child;

			for (child = oldparent->firstchild; child; child = child->nextchild)
			{
				if (owner == child->nextchild)
				{
					child->nextchild = owner->nextchild;
					break;
				}
			}
		}
	}

	if (newparent)
	{
		Assert(owner != newparent);
		owner->parent = newparent;
		owner->nextchild = newparent->firstchild;
		newparent->firstchild = owner;
	}
	else
	{
		owner->parent = NULL;
		owner->nextchild = NULL;
	}
}

/*
 * Register or deregister callback functions for resource cleanup
 *
 * These functions are intended for use by dynamically loaded modules.
 * For built-in modules we generally just hardwire the appropriate calls.
 *
 * Note that the callback occurs post-commit or post-abort, so the callback
 * functions can only do noncritical cleanup.
 */
void
RegisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;

	item = (ResourceReleaseCallbackItem *)
		MemoryContextAlloc(TopMemoryContext,
						   sizeof(ResourceReleaseCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = ResourceRelease_callbacks;
	ResourceRelease_callbacks = item;
}

void
UnregisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;
	ResourceReleaseCallbackItem *prev;

	prev = NULL;
	for (item = ResourceRelease_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				ResourceRelease_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}


/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * buffer array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 *
 * We allow the case owner == NULL because the bufmgr is sometimes invoked
 * outside any transaction (for example, during WAL recovery).
 */
void
ResourceOwnerEnlargeBuffers(ResourceOwner owner)
{
	int			newmax;

	if (owner == NULL ||
		owner->nbuffers < owner->maxbuffers)
		return;					/* nothing to do */

	if (owner->buffers == NULL)
	{
		newmax = 16;
		owner->buffers = (Buffer *)
			MemoryContextAlloc(TopMemoryContext, newmax * sizeof(Buffer));
		owner->maxbuffers = newmax;
	}
	else
	{
		newmax = owner->maxbuffers * 2;
		owner->buffers = (Buffer *)
			repalloc(owner->buffers, newmax * sizeof(Buffer));
		owner->maxbuffers = newmax;
	}
}

/*
 * Remember that a buffer pin is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeBuffers()
 *
 * We allow the case owner == NULL because the bufmgr is sometimes invoked
 * outside any transaction (for example, during WAL recovery).
 */
void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	if (owner != NULL)
	{
		Assert(owner->nbuffers < owner->maxbuffers);
		owner->buffers[owner->nbuffers] = buffer;
		owner->nbuffers++;
	}
}

/*
 * Forget that a buffer pin is owned by a ResourceOwner
 *
 * We allow the case owner == NULL because the bufmgr is sometimes invoked
 * outside any transaction (for example, during WAL recovery).
 */
void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	if (owner != NULL)
	{
		Buffer	   *buffers = owner->buffers;
		int			nb1 = owner->nbuffers - 1;
		int			i;

		/*
		 * Scan back-to-front because it's more likely we are releasing a
		 * recently pinned buffer.	This isn't always the case of course, but
		 * it's the way to bet.
		 */
		for (i = nb1; i >= 0; i--)
		{
			if (buffers[i] == buffer)
			{
				while (i < nb1)
				{
					buffers[i] = buffers[i + 1];
					i++;
				}
				owner->nbuffers = nb1;
				return;
			}
		}
		elog(ERROR, "buffer %d is not owned by resource owner %s",
			 buffer, owner->name);
	}
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * catcache reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeCatCacheRefs(ResourceOwner owner)
{
	int			newmax;

	if (owner->ncatrefs < owner->maxcatrefs)
		return;					/* nothing to do */

	if (owner->catrefs == NULL)
	{
		newmax = 16;
		owner->catrefs = (HeapTuple *)
			MemoryContextAlloc(TopMemoryContext, newmax * sizeof(HeapTuple));
		owner->maxcatrefs = newmax;
	}
	else
	{
		newmax = owner->maxcatrefs * 2;
		owner->catrefs = (HeapTuple *)
			repalloc(owner->catrefs, newmax * sizeof(HeapTuple));
		owner->maxcatrefs = newmax;
	}
}

/*
 * Remember that a catcache reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeCatCacheRefs()
 */
void
ResourceOwnerRememberCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	Assert(owner->ncatrefs < owner->maxcatrefs);
	owner->catrefs[owner->ncatrefs] = tuple;
	owner->ncatrefs++;
}

/*
 * Forget that a catcache reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	HeapTuple  *catrefs = owner->catrefs;
	int			nc1 = owner->ncatrefs - 1;
	int			i;

	for (i = nc1; i >= 0; i--)
	{
		if (catrefs[i] == tuple)
		{
			while (i < nc1)
			{
				catrefs[i] = catrefs[i + 1];
				i++;
			}
			owner->ncatrefs = nc1;
			return;
		}
	}
	elog(ERROR, "catcache reference %p is not owned by resource owner %s",
		 tuple, owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * catcache-list reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeCatCacheListRefs(ResourceOwner owner)
{
	int			newmax;

	if (owner->ncatlistrefs < owner->maxcatlistrefs)
		return;					/* nothing to do */

	if (owner->catlistrefs == NULL)
	{
		newmax = 16;
		owner->catlistrefs = (CatCList **)
			MemoryContextAlloc(TopMemoryContext, newmax * sizeof(CatCList *));
		owner->maxcatlistrefs = newmax;
	}
	else
	{
		newmax = owner->maxcatlistrefs * 2;
		owner->catlistrefs = (CatCList **)
			repalloc(owner->catlistrefs, newmax * sizeof(CatCList *));
		owner->maxcatlistrefs = newmax;
	}
}

/*
 * Remember that a catcache-list reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeCatCacheListRefs()
 */
void
ResourceOwnerRememberCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	Assert(owner->ncatlistrefs < owner->maxcatlistrefs);
	owner->catlistrefs[owner->ncatlistrefs] = list;
	owner->ncatlistrefs++;
}

/*
 * Forget that a catcache-list reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	CatCList  **catlistrefs = owner->catlistrefs;
	int			nc1 = owner->ncatlistrefs - 1;
	int			i;

	for (i = nc1; i >= 0; i--)
	{
		if (catlistrefs[i] == list)
		{
			while (i < nc1)
			{
				catlistrefs[i] = catlistrefs[i + 1];
				i++;
			}
			owner->ncatlistrefs = nc1;
			return;
		}
	}
	elog(ERROR, "catcache list reference %p is not owned by resource owner %s",
		 list, owner->name);
}

/*
 * Make sure there is room for at least one more entry in a ResourceOwner's
 * relcache reference array.
 *
 * This is separate from actually inserting an entry because if we run out
 * of memory, it's critical to do so *before* acquiring the resource.
 */
void
ResourceOwnerEnlargeRelationRefs(ResourceOwner owner)
{
	int			newmax;

	if (owner->nrelrefs < owner->maxrelrefs)
		return;					/* nothing to do */

	if (owner->relrefs == NULL)
	{
		newmax = 16;
		owner->relrefs = (Relation *)
			MemoryContextAlloc(TopMemoryContext, newmax * sizeof(Relation));
		owner->maxrelrefs = newmax;
	}
	else
	{
		newmax = owner->maxrelrefs * 2;
		owner->relrefs = (Relation *)
			repalloc(owner->relrefs, newmax * sizeof(Relation));
		owner->maxrelrefs = newmax;
	}
}

/*
 * Remember that a relcache reference is owned by a ResourceOwner
 *
 * Caller must have previously done ResourceOwnerEnlargeRelationRefs()
 */
void
ResourceOwnerRememberRelationRef(ResourceOwner owner, Relation rel)
{
	Assert(owner->nrelrefs < owner->maxrelrefs);
	owner->relrefs[owner->nrelrefs] = rel;
	owner->nrelrefs++;
}

/*
 * Forget that a relcache reference is owned by a ResourceOwner
 */
void
ResourceOwnerForgetRelationRef(ResourceOwner owner, Relation rel)
{
	Relation   *relrefs = owner->relrefs;
	int			nr1 = owner->nrelrefs - 1;
	int			i;

	for (i = nr1; i >= 0; i--)
	{
		if (relrefs[i] == rel)
		{
			while (i < nr1)
			{
				relrefs[i] = relrefs[i + 1];
				i++;
			}
			owner->nrelrefs = nr1;
			return;
		}
	}
	elog(ERROR, "relcache reference %s is not owned by resource owner %s",
		 RelationGetRelationName(rel), owner->name);
}

/*
 * Debugging subroutine
 */
static void
PrintRelCacheLeakWarning(Relation rel)
{
	elog(WARNING, "relcache reference leak: relation \"%s\" not closed",
		 RelationGetRelationName(rel));
}
