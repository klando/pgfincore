#ifndef PGFINCORE_H
#define PGFINCORE_H

#if PG_VERSION_NUM < 110000
#include "storage/smgr.h"
/*
 * PG_RETURN_UINT64 appears in PostgreSQL 11
 * and UInt64GetDatum in 10 ...
 * it results in dropping pgfincore support for 9.4-9.6
 */
#define PG_RETURN_UINT64(x)  return UInt64GetDatum(x)

/*
 * get_relkind_objtype
 *
 * Return the object type for the relkind given by the caller.
 *
 * If an unexpected relkind is passed, we say OBJECT_TABLE rather than
 * failing.  That's because this is mostly used for generating error messages
 * for failed ACL checks on relations, and we'd rather produce a generic
 * message saying "table" than fail entirely.
 */
static ObjectType get_relkind_objtype(char relkind);
ObjectType
get_relkind_objtype(char relkind)
{
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			return OBJECT_TABLE;
		case RELKIND_INDEX:
			return OBJECT_INDEX;
		case RELKIND_SEQUENCE:
			return OBJECT_SEQUENCE;
		case RELKIND_VIEW:
			return OBJECT_VIEW;
		case RELKIND_MATVIEW:
			return OBJECT_MATVIEW;
		case RELKIND_FOREIGN_TABLE:
			return OBJECT_FOREIGN_TABLE;
		case RELKIND_TOASTVALUE:
			return OBJECT_TABLE;
		default:
			/* Per above, don't raise an error */
			return OBJECT_TABLE;
	}
}

#endif

#if PG_VERSION_NUM < 110000
static inline SMgrRelationData *RelationGetSmgr(Relation rel)
{
	RelationOpenSmgr(rel);
	return rel->rd_smgr;
}
#endif

#if PG_VERSION_NUM < 150000
#define PG_WAIT_EXTENSION	0x07000000U
#define PG_WAIT_IO			0x0A000000U
static inline void pgstat_report_wait_start(int foo){}
static inline void pgstat_report_wait_end(){}

/*
 * InitMaterializedSRF
 *
 * Helper function to build the state of a set-returning function used
 * in the context of a single call with materialize mode.  This code
 * includes sanity checks on ReturnSetInfo, creates the Tuplestore and
 * the TupleDesc used with the function and stores them into the
 * function's ReturnSetInfo.
 *
 * "flags" can be set to MAT_SRF_USE_EXPECTED_DESC, to use the tuple
 * descriptor coming from expectedDesc, which is the tuple descriptor
 * expected by the caller.  MAT_SRF_BLESS can be set to complete the
 * information associated to the tuple descriptor, which is necessary
 * in some cases where the tuple descriptor comes from a transient
 * RECORD datatype.
 */
#define MAT_SRF_USE_EXPECTED_DESC  0x01    /* use expectedDesc as tupdesc. */
#define MAT_SRF_BLESS              0x02    /* "Bless" a tuple descriptor with  BlessTupleDesc(). */
void InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags);
void
InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags)
{
	bool			random_access;
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate	*tupstore;
	MemoryContext	old_context,
					per_query_ctx;
	TupleDesc		stored_tupdesc;

	/* check to see if caller supports returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & MAT_SRF_USE_EXPECTED_DESC) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * Store the tuplestore and the tuple descriptor in ReturnSetInfo.  This
	 * must be done in the per-query memory context.
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_context = MemoryContextSwitchTo(per_query_ctx);

	/* build a tuple descriptor for our result type */
	if ((flags & MAT_SRF_USE_EXPECTED_DESC) != 0)
		stored_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else
	{
		if (get_call_result_type(fcinfo, NULL, &stored_tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
	}

	/* If requested, bless the tuple descriptor */
	if ((flags & MAT_SRF_BLESS) != 0)
		BlessTupleDesc(stored_tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = stored_tupdesc;
	MemoryContextSwitchTo(old_context);
}
#endif

static char     *_mdfd_segpath(SMgrRelation reln, ForkNumber forkNum,
                                                   BlockNumber segno);
/*
 * Return the filename for the specified segment of the relation. The
 * returned string is palloc'd.
 *
 * IMPORTED from PostgreSQL source-code as-is
 */
static char *
_mdfd_segpath(SMgrRelation reln, ForkNumber forkNum, BlockNumber segno)
{
	char	*path,
			*fullpath;

#if PG_VERSION_NUM < 160000
	path = relpath(reln->smgr_rnode, forkNum);
#else
	path = relpath(reln->smgr_rlocator, forkNum);
#endif
	if (segno > 0)
	{
		fullpath = psprintf("%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	return fullpath;
}

/*
 * Inline functions for Datum functions
 */
/* PostgreSQL Segment size */
static inline uint32 pg_SegmentSize()
{
	return RELSEG_SIZE;
}

static inline Oid pgArgRelation(FunctionCallInfo fcinfo, int p)
{
	/* Basic sanity checking. */
	if (PG_ARGISNULL(p))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL"),
				 errhint("check parameters")));

	return PG_GETARG_OID(p);
}

static inline text *pgArgForkName(FunctionCallInfo fcinfo, int p)
{
	if (PG_ARGISNULL(p))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation fork cannot be NULL"),
				 errhint("check parameters")));

	return PG_GETARG_TEXT_PP(p);
}

static inline int64 pgArgBlockOffset(FunctionCallInfo fcinfo, int p)
{
	int64 offset;
	if (PG_ARGISNULL(p))
		offset = -1;
	else
	{
		offset = PG_GETARG_INT64(p);
		if (offset < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number cannot be negative"),
					 errhint("check parameters")));
	}
	return offset;
}

static inline int64 pgArgBlockLength(FunctionCallInfo fcinfo, int p)
{
	int64 length;
	if (PG_ARGISNULL(p))
		length = 0;
	else
	{
		length = PG_GETARG_INT64(p);
		if (length < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("number of blocks cannot be negative"),
					 errhint("check parameters")));
	}
	return length;
}

static inline int64 pgArgBlockRange(FunctionCallInfo fcinfo, int p)
{
	int64 range;
	if (PG_ARGISNULL(p))
		range = pg_SegmentSize();
	else
	{
		range = PG_GETARG_INT64(p);
		if (range <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("block range cannot be negative or 0"),
						 errhint("check parameters")));
	}
	return range;
}

static inline void checkAcl(Oid relOid, Relation rel)
{
	AclResult	aclresult = pg_class_aclcheck(relOid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   get_rel_name(relOid));
}

static inline void checkForkExists(Oid relOid, Relation rel, text *forkName)
{
	/* Check that the fork exists. */
	if (!smgrexists(RelationGetSmgr(rel),
					forkname_to_number(text_to_cstring(forkName))))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fork \"%s\" does not exist for the relation \"%s\"",
						text_to_cstring(forkName), get_rel_name(relOid)),
				 errhint("check parameters")));
}

/*
 * Inline functions for file descriptors (no file creation!)
 */
static inline int getFileDescriptor(SMgrRelation reln, BlockNumber forkNum,
									BlockNumber segno)
{
	char	*fullpath = _mdfd_segpath(reln, forkNum, segno);
#if PG_VERSION_NUM < 110000
	int		fd = OpenTransientFile(fullpath, O_RDONLY | PG_BINARY, 0);
#else
	int		fd = OpenTransientFile(fullpath, O_RDONLY | PG_BINARY);
#endif
	pfree(fullpath);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read segment %u", segno)));
	}
	return fd;
}

static inline void closeFileDescriptor(int fd, BlockNumber segno)
{
	if (CloseTransientFile(fd) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close segment %u", segno)));
}
#endif
