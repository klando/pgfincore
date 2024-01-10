#ifndef PGFINCORE_H
#define PGFINCORE_H
/*
 * PG_RETURN_UINT64 appears in PostgreSQL 11
 * and UInt64GetDatum in 10 ...
 * it results in dropping pgfincore support for 9.4-9.6
 */
#if !defined(PG_RETURN_UINT64)
#define PG_RETURN_UINT64(x)  return UInt64GetDatum(x)
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
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("relation cannot be NULL"),
				errhint("check parameters"));

	return PG_GETARG_OID(p);
}

static inline text *pgArgForkName(FunctionCallInfo fcinfo, int p)
{
	if (PG_ARGISNULL(p))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("relation fork cannot be NULL"),
				errhint("check parameters"));

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
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("starting block number cannot be negative"),
					errhint("check parameters"));
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
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("number of blocks cannot be negative"),
					errhint("check parameters"));
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
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("block range cannot be negative or 0"),
						errhint("check parameters"));
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
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("fork \"%s\" does not exist for the relation \"%s\"",
					   text_to_cstring(forkName), get_rel_name(relOid)),
				errhint("check parameters"));
}

/*
 * Inline functions for file descriptors
 */
static inline int getFileDescriptor(SMgrRelation reln, BlockNumber forkNum,
									BlockNumber segno)
{
	char	*fullpath = _mdfd_segpath(reln, forkNum, segno);
	int		fd = OpenTransientFile(fullpath, O_RDONLY | PG_BINARY);
	pfree(fullpath);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					errcode_for_file_access(),
					errmsg("could not read segment %u", segno));
	}
	return fd;
}

static inline void closeFileDescriptor(int fd, BlockNumber segno)
{
	if (CloseTransientFile(fd) != 0)
		ereport(WARNING,
				errcode_for_file_access(),
				errmsg("could not close segment %u", segno));
}
#endif
