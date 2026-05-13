/*-------------------------------------------------------------------------
 *
 * outfuncs_common.c
 *	  Common serialization functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * NOTES
 *    This file contains all common serialization functions for both
 *    binary and text representations, and serialization functions
 *    that are Cloudberry-specific for text representation.
 *
 *    All text-representation only functions are guarded by the macro
 *    `COMPILING_BINARY_FUNCS`, they appear first in this file.
 *
 *    This C source file SHOULD not be compiled alone, it MUST be
 *    only be included by outfuncs.c, so we intended to not complete
 *    the header files.
 *
 *    When you consider adding new serialization functions, you should
 *    folow these rules:
 *    1. Do not add any functions(Cloudberry-specific) to outfuncs.c
 *    2. Add functions to outfuncs_common.c that can be used for both
 *       binary and text representations.
 *    3. Add functions to outfast.c if these functions can only be used
 *       for binary representation.
 *    4. Add functions to outfuncs_common.c if these functions can only
 *       be used for text representation.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMPILING_BINARY_FUNCS
/* 'flow' is only needed during planning. */
static void
_outFlow(StringInfo str, const Flow *node)
{
	WRITE_NODE_TYPE("FLOW");

	WRITE_ENUM_FIELD(flotype, FlowType);
	WRITE_ENUM_FIELD(locustype, CdbLocusType);
	WRITE_INT_FIELD(segindex);
	WRITE_INT_FIELD(numsegments);
}

static void
_outCdbPathLocus(StringInfo str, const CdbPathLocus *node)
{
	WRITE_ENUM_FIELD(locustype, CdbLocusType);
	WRITE_NODE_FIELD(distkey);
	WRITE_INT_FIELD(numsegments);
}

static void
_outTableFunctionScanPath(StringInfo str, const TableFunctionScanPath *node)
{
	WRITE_NODE_TYPE("TABLEFUNCTIONSCANPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outAppendOnlyPath(StringInfo str, const AppendOnlyPath *node)
{
	WRITE_NODE_TYPE("APPENDONLYPATH");

	_outPathInfo(str, (Path *) node);
}

static void
_outAOCSPath(StringInfo str, const AOCSPath *node)
{
	WRITE_NODE_TYPE("APPENDONLYPATH");

	_outPathInfo(str, (Path *) node);
}

static void
_outCdbMotionPath(StringInfo str, const CdbMotionPath *node)
{
    WRITE_NODE_TYPE("MOTIONPATH");

    _outPathInfo(str, &node->path);

    WRITE_NODE_FIELD(subpath);
}

static void
_outNull(StringInfo str, const Node *n pg_attribute_unused())
{
	WRITE_NODE_TYPE("NULL");
}

#endif /* COMPILING_BINARY_FUNCS */

static void
_outCopyStmt(StringInfo str, const CopyStmt *node)
{
	WRITE_NODE_TYPE("COPYSTMT");
	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(attlist);
	WRITE_BOOL_FIELD(is_from);
	WRITE_BOOL_FIELD(is_program);
	WRITE_STRING_FIELD(filename);
	WRITE_STRING_FIELD(dirfilename);
	WRITE_NODE_FIELD(options);
	WRITE_NODE_FIELD(sreh);
}

static void
_outQueryDispatchDesc(StringInfo str, const QueryDispatchDesc *node)
{
	WRITE_NODE_TYPE("QUERYDISPATCHDESC");

	WRITE_NODE_FIELD(intoCreateStmt);
	WRITE_NODE_FIELD(paramInfo);
	WRITE_NODE_FIELD(oidAssignments);
	WRITE_NODE_FIELD(sliceTable);
	WRITE_NODE_FIELD(cursorPositions);
	WRITE_STRING_FIELD(parallelCursorName);
	WRITE_BOOL_FIELD(useChangedAOOpts);
	WRITE_INT_FIELD(secContext);
	WRITE_NODE_FIELD(namedRelList);
	WRITE_OID_FIELD(matviewOid);
	WRITE_OID_FIELD(tableid);
	WRITE_INT_FIELD(snaplen);
	WRITE_STRING_FIELD(snapname);
}

static void
_outTupleDescNode(StringInfo str, const TupleDescNode *node)
{
	int			i;

	Assert(node->tuple->tdtypeid == RECORDOID);

	WRITE_NODE_TYPE("TUPLEDESCNODE");
	WRITE_INT_FIELD(natts);
	WRITE_INT_FIELD(tuple->natts);

	for (i = 0; i < node->tuple->natts; i++)
		appendBinaryStringInfo(str, (char *) &node->tuple->attrs[i], ATTRIBUTE_FIXED_PART_SIZE);

	Assert(node->tuple->constr == NULL);

	WRITE_OID_FIELD(tuple->tdtypeid);
	WRITE_INT_FIELD(tuple->tdtypmod);
	WRITE_INT_FIELD(tuple->tdrefcount);
}

static void
_outSerializedParams(StringInfo str, const SerializedParams *node)
{
	WRITE_NODE_TYPE("SERIALIZEDPARAMS");

	WRITE_INT_FIELD(nExternParams);
	for (int i = 0; i < node->nExternParams; i++)
	{
		WRITE_BOOL_FIELD(externParams[i].isnull);
		WRITE_INT_FIELD(externParams[i].pflags);
		WRITE_OID_FIELD(externParams[i].ptype);
		WRITE_INT_FIELD(externParams[i].plen);
		WRITE_BOOL_FIELD(externParams[i].pbyval);

		if (!node->externParams[i].isnull)
			outDatum(str,
					 node->externParams[i].value,
					 node->externParams[i].plen,
					 node->externParams[i].pbyval);
	}

	WRITE_INT_FIELD(nExecParams);
	for (int i = 0; i < node->nExecParams; i++)
	{
		WRITE_BOOL_FIELD(execParams[i].isnull);
		WRITE_BOOL_FIELD(execParams[i].isvalid);
		WRITE_INT_FIELD(execParams[i].plen);
		WRITE_BOOL_FIELD(execParams[i].pbyval);

		if (node->execParams[i].isvalid && !node->execParams[i].isnull)
			outDatum(str,
					 node->execParams[i].value,
					 node->execParams[i].plen,
					 node->execParams[i].pbyval);
		WRITE_BOOL_FIELD(execParams[i].pbyval);
	}

	/*
	 * No text output function for TupleDescNodes. But that's OK, we
	 * only support text output for debugging purposes.
	 */
#ifdef COMPILING_BINARY_FUNCS
	WRITE_NODE_FIELD(transientTypes);
#endif
}

static void
_outOidAssignment(StringInfo str, const OidAssignment *node)
{
	WRITE_NODE_TYPE("OIDASSIGNMENT");

	WRITE_OID_FIELD(catalog);
	WRITE_STRING_FIELD(objname);
	WRITE_OID_FIELD(namespaceOid);
	WRITE_OID_FIELD(keyOid1);
	WRITE_OID_FIELD(keyOid2);
	WRITE_OID_FIELD(oid);
}

static void
_outSequence(StringInfo str, const Sequence *node)
{
	WRITE_NODE_TYPE("SEQUENCE");
	_outPlanInfo(str, (Plan *)node);
	WRITE_NODE_FIELD(subplans);
}

static void
_outDQAExpr(StringInfo str, const DQAExpr *node)
{
    WRITE_NODE_TYPE("DQAExpr");

    WRITE_INT_FIELD(agg_expr_id);
    WRITE_BITMAPSET_FIELD(agg_args_id_bms);
    WRITE_NODE_FIELD(agg_filter);
}

static void
_outTupleSplit(StringInfo str, const TupleSplit *node)
{
	WRITE_NODE_TYPE("TupleSplit");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(grpColIdx, node->numCols);
	WRITE_NODE_FIELD(dqa_expr_lst);
}

static void
_outTableFunctionScan(StringInfo str, const TableFunctionScan *node)
{
	WRITE_NODE_TYPE("TABLEFUNCTIONSCAN");

	_outScanInfo(str, (Scan *) node);

	WRITE_NODE_FIELD(function);
}

static void
_outShareInputScan(StringInfo str, const ShareInputScan *node)
{
	WRITE_NODE_TYPE("SHAREINPUTSCAN");

	WRITE_BOOL_FIELD(cross_slice);
	WRITE_INT_FIELD(share_id);
	WRITE_INT_FIELD(producer_slice_id);
	WRITE_INT_FIELD(this_slice_id);
	WRITE_INT_FIELD(nconsumers);
	WRITE_BOOL_FIELD(discard_output);
	WRITE_BOOL_FIELD(ref_set);

	_outPlanInfo(str, (Plan *) node);
}

static void
_outMotion(StringInfo str, const Motion *node)
{
	WRITE_NODE_TYPE("MOTION");

	WRITE_INT_FIELD(motionID);
	WRITE_ENUM_FIELD(motionType, MotionType);

	WRITE_BOOL_FIELD(sendSorted);

	WRITE_NODE_FIELD(hashExprs);
	WRITE_OID_ARRAY(hashFuncs, list_length(node->hashExprs));
	WRITE_INT_FIELD(numSortCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numSortCols);
	WRITE_INT_ARRAY(sortOperators, node->numSortCols);
	WRITE_INT_ARRAY(collations, node->numSortCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numSortCols);
	WRITE_INT_FIELD(segidColIdx);

	WRITE_INT_FIELD(numHashSegments);

	/* senderSliceInfo is intentionally omitted. It's only used during planning */

	_outPlanInfo(str, (Plan *) node);
}

/*
 * _outSplitUpdate
 */
static void
_outSplitUpdate(StringInfo str, const SplitUpdate *node)
{
	WRITE_NODE_TYPE("SplitUpdate");

	WRITE_INT_FIELD(actionColIdx);
	WRITE_NODE_FIELD(insertColIdx);
	WRITE_NODE_FIELD(deleteColIdx);

	WRITE_INT_FIELD(numHashSegments);
	WRITE_INT_FIELD(numHashAttrs);
	WRITE_ATTRNUMBER_ARRAY(hashAttnos, node->numHashAttrs);
	WRITE_OID_ARRAY(hashFuncs, node->numHashAttrs);

	_outPlanInfo(str, (Plan *) node);
}

/*
 * _outSplitMerge
 */
static void
_outSplitMerge(StringInfo str, const SplitMerge *node)
{
	WRITE_NODE_TYPE("SplitMerge");

	WRITE_INT_FIELD(numHashSegments);
	WRITE_INT_FIELD(numHashAttrs);
	WRITE_ATTRNUMBER_ARRAY(hashAttnos, node->numHashAttrs);
	WRITE_OID_ARRAY(hashFuncs, node->numHashAttrs);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(mergeActionLists);

	_outPlanInfo(str, (Plan *) node);
}

/*
 * _outAssertOp
 */
static void
_outAssertOp(StringInfo str, const AssertOp *node)
{
	WRITE_NODE_TYPE("AssertOp");

	WRITE_NODE_FIELD(errmessage);
	WRITE_INT_FIELD(errcode);

	_outPlanInfo(str, (Plan *) node);
}

/*
 * _outPartitionSelector
 */
static void
_outPartitionSelector(StringInfo str, const PartitionSelector *node)
{
	WRITE_NODE_TYPE("PartitionSelector");

	WRITE_INT_FIELD(paramid);
	WRITE_NODE_FIELD(part_prune_info);

	_outPlanInfo(str, (Plan *) node);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/


static void
wrapStringList(List *list)
{
	ListCell *lc;

	foreach(lc, list)
	{
		char	   *str = (char *) lfirst(lc);

		lfirst(lc) = makeString(str);
	}
}

static void
unwrapStringList(List *list)
{
	ListCell *lc;

	foreach(lc, list)
	{
		String	   *val = lfirst(lc);

		lfirst(lc) = strVal(val);
		pfree(val);
	}
}

static void
_outAlteredTableInfo(StringInfo str, const AlteredTableInfo *node)
{
	ListCell   *lc;

	WRITE_NODE_TYPE("ALTEREDTABLEINFO");

	WRITE_OID_FIELD(relid);
	WRITE_CHAR_FIELD(relkind);
	/* oldDesc is omitted */

	for (int i = 0; i < AT_NUM_PASSES; i++)
    {
		WRITE_NODE_FIELD(subcmds[i]);
    }

	/*
	 * These aren't Nodes in upstream, so make sure the node tags
	 * are set correctly before trying to serialize them.
	 */
	foreach(lc, node->constraints)
	{
		NewConstraint *e = (NewConstraint *) lfirst(lc);
		e->type = T_NewConstraint;
	}
	foreach(lc, node->newvals)
	{
		NewColumnValue *e = (NewColumnValue *) lfirst(lc);
		e->type = T_NewColumnValue;
	}

	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(newvals);
	WRITE_NODE_FIELD(afterStmts);
	WRITE_BOOL_FIELD(verify_new_notnull);
	WRITE_INT_FIELD(rewrite);
	WRITE_OID_FIELD(newAccessMethod);
	WRITE_BOOL_FIELD(dist_opfamily_changed);
	WRITE_OID_FIELD(new_opclass);
	/*
	 * NB: newTableSpace is excluded, it will be assigned in phase 1 of AlterTable.
	 * If newTableSpace is required, refer to the name in its corresponding cmd.
	 * If newTableSpace is strongly required in serialization, please add it
	 * and update `ATPrepSetTableSpace()` to avoid error.
	 */
	WRITE_BOOL_FIELD(chgPersistence);
	WRITE_CHAR_FIELD(newrelpersistence);
	WRITE_NODE_FIELD(partition_constraint);
	WRITE_BOOL_FIELD(validate_default);
	WRITE_NODE_FIELD(changedConstraintOids);

	/* node->changedConstraintDefs is a list of naked strings, so
	 * we can't use WRITE_NODE_FIELD on it. Temporarily wrap them in Values.
	 */
	wrapStringList(node->changedConstraintDefs);
	WRITE_NODE_FIELD(changedConstraintDefs);
	/* unwrap them again */
	unwrapStringList(node->changedConstraintDefs);

	WRITE_NODE_FIELD(changedIndexOids);
	wrapStringList(node->changedIndexDefs);
	WRITE_NODE_FIELD(changedIndexDefs);
	unwrapStringList(node->changedIndexDefs);
	WRITE_NODE_FIELD(beforeStmtLists);
	WRITE_NODE_FIELD(constraintLists);
}

static void
_outNewConstraint(StringInfo str, const NewConstraint *node)
{
	WRITE_NODE_TYPE("NEWCONSTRAINT");

	WRITE_STRING_FIELD(name);
	WRITE_ENUM_FIELD(contype, ConstrType);
	WRITE_OID_FIELD(refrelid);
	WRITE_OID_FIELD(refindid);
	WRITE_OID_FIELD(conid);
	WRITE_NODE_FIELD(qual);
	/* can't serialize qualstate */
}

static void
_outNewColumnValue(StringInfo str, const NewColumnValue *node)
{
	WRITE_NODE_TYPE("NEWCOLUMNVALUE");

	WRITE_INT_FIELD(attnum);
	WRITE_NODE_FIELD(expr);
	/* can't serialize exprstate */
	WRITE_BOOL_FIELD(is_generated);
}

static void
_outInsertStmt(StringInfo str, const InsertStmt *node)
{
	WRITE_NODE_TYPE("INSERT");

	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(cols);
	WRITE_NODE_FIELD(selectStmt);
	WRITE_NODE_FIELD(returningList);
	WRITE_NODE_FIELD(withClause);
}

static void
_outCdbProcess(StringInfo str, const CdbProcess *node)
{
	WRITE_NODE_TYPE("CDBPROCESS");
	WRITE_STRING_FIELD(listenerAddr);
	WRITE_INT_FIELD(listenerPort);
	WRITE_INT_FIELD(pid);
	WRITE_INT_FIELD(contentid);
	WRITE_INT_FIELD(dbid);
}

static void
_outSliceTable(StringInfo str, const SliceTable *node)
{
	WRITE_NODE_TYPE("SLICETABLE");

	WRITE_INT_FIELD(localSlice);
	WRITE_INT_FIELD(numSlices);
	for (int i = 0; i < node->numSlices; i++)
	{
		WRITE_INT_FIELD(slices[i].sliceIndex);
		WRITE_INT_FIELD(slices[i].rootIndex);
		WRITE_INT_FIELD(slices[i].parentIndex);
		WRITE_INT_FIELD(slices[i].planNumSegments);
		WRITE_NODE_FIELD(slices[i].children); /* List of int index */
		WRITE_ENUM_FIELD(slices[i].gangType, GangType);
		WRITE_NODE_FIELD(slices[i].segments); /* List of int */
		WRITE_BOOL_FIELD(slices[i].useMppParallelMode);
		WRITE_INT_FIELD(slices[i].parallel_workers);
		WRITE_DUMMY_FIELD(slices[i].primaryGang);
		WRITE_NODE_FIELD(slices[i].primaryProcesses); /* List of (CDBProcess *) */
		WRITE_BITMAPSET_FIELD(slices[i].processesMap);
	}
	WRITE_BOOL_FIELD(hasMotions);
	WRITE_INT_FIELD(instrument_options);
	WRITE_INT_FIELD(ic_instance_id);
}

static void
_outCursorPosInfo(StringInfo str, const CursorPosInfo *node)
{
	WRITE_NODE_TYPE("CURSORPOSINFO");

	WRITE_STRING_FIELD(cursor_name);
	WRITE_INT_FIELD(gp_segment_id);
	WRITE_UINT_FIELD(ctid.ip_blkid.bi_hi);
	WRITE_UINT_FIELD(ctid.ip_blkid.bi_lo);
	WRITE_UINT_FIELD(ctid.ip_posid);
	WRITE_OID_FIELD(table_oid);
}

static void
_outCreateTrigStmt(StringInfo str, const CreateTrigStmt *node)
{
	WRITE_NODE_TYPE("CREATETRIGSTMT");

	WRITE_BOOL_FIELD(replace);
	WRITE_STRING_FIELD(trigname);
	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(funcname);
	WRITE_NODE_FIELD(args);
	WRITE_BOOL_FIELD(row);
	WRITE_INT_FIELD(timing);
	WRITE_INT_FIELD(events);
	WRITE_NODE_FIELD(columns);
	WRITE_NODE_FIELD(whenClause);
	WRITE_BOOL_FIELD(isconstraint);
	WRITE_NODE_FIELD(transitionRels);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_NODE_FIELD(constrrel);
	WRITE_OID_FIELD(matviewId);
}


static void
_outAlterTSConfigurationStmt(StringInfo str, const AlterTSConfigurationStmt *node)
{
	WRITE_NODE_TYPE("ALTERTSCONFIGURATIONSTMT");

	WRITE_NODE_FIELD(cfgname);
	WRITE_NODE_FIELD(tokentype);
	WRITE_NODE_FIELD(dicts);
	WRITE_BOOL_FIELD(override);
	WRITE_BOOL_FIELD(replace);
	WRITE_BOOL_FIELD(missing_ok);
}

static void
_outMemoize(StringInfo str, const Memoize *node)
{
	WRITE_NODE_TYPE("MEMOIZE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numKeys);
	WRITE_OID_ARRAY(hashOperators, node->numKeys);
	WRITE_OID_ARRAY(collations, node->numKeys);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_BOOL_FIELD(binary_mode);
	WRITE_UINT_FIELD(est_entries);
	WRITE_BITMAPSET_FIELD(keyparamids);
}

static void
_outTidRangeScan(StringInfo str, const TidRangeScan *node)
{
	WRITE_NODE_TYPE("TIDRANGESCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(tidrangequals);
}

static void
_outEphemeralNamedRelationInfo(StringInfo str, const EphemeralNamedRelationInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("EphemeralNamedRelationInfo");
	WRITE_STRING_FIELD(name);
	WRITE_OID_FIELD(reliddesc);
	WRITE_INT_FIELD(natts);
	WRITE_INT_FIELD(tuple->natts);

	for (i = 0; i < node->tuple->natts; i++)
		appendBinaryStringInfo(str, (char *) &node->tuple->attrs[i], ATTRIBUTE_FIXED_PART_SIZE);

	WRITE_OID_FIELD(tuple->tdtypeid);
	WRITE_INT_FIELD(tuple->tdtypmod);
	WRITE_INT_FIELD(tuple->tdrefcount);
	WRITE_ENUM_FIELD(enrtype, EphemeralNameRelationType);
	WRITE_FLOAT_FIELD(enrtuples);
}


