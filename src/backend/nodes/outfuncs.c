/*-------------------------------------------------------------------------
 *
 * outfuncs.c
 *	  Output functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/outfuncs.c
 *
 * NOTES
 *	  Every node type that can appear in stored rules' parsetrees *must*
 *	  have an output function defined here (as well as an input function
 *	  in readfuncs.c).  In addition, plan nodes should have input and
 *	  output functions so that they can be sent to parallel workers.
 *
 *	  For use in debugging, we also provide output functions for nodes
 *	  that appear in raw parsetrees and planner Paths.  These node types
 *	  need not have input functions.  Output support for raw parsetrees
 *	  is somewhat incomplete, too; in particular, utility statements are
 *	  almost entirely unsupported.  We try to support everything that can
 *	  appear in a raw SELECT, though.
 *
 *    N.B. Faster variants of these functions (producing illegible output)
 *         are supplied in outfast.c for use in Greenplum Database serialization.  The
 *         function in this file are intended to produce legible output.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "common/shortest_dec.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "utils/datum.h"
#include "utils/rel.h"

#include "cdb/cdbgang.h"
#include "nodes/altertablenodes.h"

/*
 * outfuncs.c is compiled normally into outfuncs.o, but it's also
 * #included from outfast.c. When #included, outfast.c defines
 * COMPILING_BINARY_FUNCS, and provides replacements WRITE_* macros. See
 * comments at top of readfast.c.
 */
#ifndef COMPILING_BINARY_FUNCS
static void outChar(StringInfo str, char c);
static void outDouble(StringInfo str, double d);

/*
 * Macros to simplify output of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.  Note that these
 * hard-wire conventions about the names of the local variables in an Out
 * routine.
 */

/* Write the label for the node type */
#define WRITE_NODE_TYPE(nodelabel) \
	appendStringInfoString(str, nodelabel)

/* Write an integer field (anything written as ":fldname %d") */
#define WRITE_INT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write an unsigned integer field (anything written as ":fldname %u") */
#define WRITE_UINT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write an unsigned integer field (anything written with UINT64_FORMAT) */
#define WRITE_UINT64_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " " UINT64_FORMAT, \
					 node->fldname)

/* Write a 64-bit integer field (anything written with INT64_FORMAT) */
#define WRITE_INT64_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " " INT64_FORMAT, \
					 node->fldname)

/* Write an OID field (don't hard-wire assumption that OID is same as uint) */
#define WRITE_OID_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write a long-integer field */
#define WRITE_LONG_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %ld", node->fldname)

/* Write a char field (ie, one ascii character) */
#define WRITE_CHAR_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outChar(str, node->fldname))

/* Write an enumerated-type field as an integer code */
#define WRITE_ENUM_FIELD(fldname, enumtype) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", \
					 (int) node->fldname)

/* Write a float field (actually, they're double) */
#define WRITE_FLOAT_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outDouble(str, node->fldname))

/* Write a boolean field */
#define WRITE_BOOL_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %s", \
					 booltostr(node->fldname))

/* Write a character-string (possibly NULL) field */
#define WRITE_STRING_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outToken(str, node->fldname))

/* Write a parse location field (actually same as INT case) */
#define WRITE_LOCATION_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write a Node field */
#define WRITE_NODE_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outNode(str, node->fldname))

/* Write a bitmapset field */
#define WRITE_BITMAPSET_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outBitmapset(str, node->fldname))

/* Write a variable-length array (not a List) of Node pointers */
#define WRITE_NODE_ARRAY(fldname, len) \
        (appendStringInfoString(str, " :" CppAsString(fldname) " "), \
         writeNodeArray(str, (const Node * const *) node->fldname, len))

#define WRITE_ATTRNUMBER_ARRAY(fldname, len) \
	do { \
		appendStringInfoString(str, " :" CppAsString(fldname) " "); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, " %d", node->fldname[i]); \
	} while(0)

#define WRITE_OID_ARRAY(fldname, len) \
	do { \
		appendStringInfoString(str, " :" CppAsString(fldname) " "); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, " %u", node->fldname[i]); \
	} while(0)

#define WRITE_INT_ARRAY(fldname, len) \
	do { \
		appendStringInfoString(str, " :" CppAsString(fldname) " "); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, " %d", node->fldname[i]); \
	} while(0)

#define WRITE_BOOL_ARRAY(fldname, len) \
	do { \
		appendStringInfoString(str, " :" CppAsString(fldname) " "); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, " %s", booltostr(node->fldname[i])); \
	} while(0)

/* Write a bytea field */
#define WRITE_BYTEA_FIELD(fldname) \
	(_outDatum(str, PointerGetDatum(node->fldname), -1, false))

/* Write a dummy field -- value not displayable or copyable */
#define WRITE_DUMMY_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outToken(str, NULL))

static void _outCdbPathLocus(StringInfo str, const CdbPathLocus *node);
#endif /* COMPILING_BINARY_FUNCS */

#define booltostr(x)  ((x) ? "true" : "false")


#ifndef COMPILING_BINARY_FUNCS

/*
 * Print an array (not a List) of Node pointers.
 *
 * The decoration is identical to that of scalar arrays, but we can't
 * quite use appendStringInfo() in the loop.
 */
static void
writeNodeArray(StringInfo str, const Node *const *arr, int len)
{
	if (arr != NULL)
	{
		appendStringInfoChar(str, '(');
		for (int i = 0; i < len; i++)
		{
			appendStringInfoChar(str, ' ');
			outNode(str, arr[i]);
		}
		appendStringInfoChar(str, ')');
	}
	else
		appendStringInfoString(str, "<>");
}

/*
 * outToken
 *	  Convert an ordinary string (eg, an identifier) into a form that
 *	  will be decoded back to a plain token by read.c's functions.
 *
 *	  If a null or empty string is given, it is encoded as "<>".
 */
void
outToken(StringInfo str, const char *s)
{
	if (s == NULL || *s == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}
	if (*s == '\0')
	{
		appendStringInfoString(str, "\"\"");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by read.c
	 * (either in pg_strtok() or in nodeRead()), and therefore need a
	 * protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

/*
 * Convert one char.  Goes through outToken() so that special characters are
 * escaped.
 */
static void
outChar(StringInfo str, char c)
{
	char		in[2];

	/* Traditionally, we've represented \0 as <>, so keep doing that */
	if (c == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}

	in[0] = c;
	in[1] = '\0';

	outToken(str, in);
}

/*
 * Convert a double value, attempting to ensure the value is preserved exactly.
 */
static void
outDouble(StringInfo str, double d)
{
	char		buf[DOUBLE_SHORTEST_DECIMAL_LEN];

	double_to_shortest_decimal_buf(d, buf);
	appendStringInfoString(str, buf);
}

static void
_outList(StringInfo str, const List *node)
{
	const ListCell *lc;

	appendStringInfoChar(str, '(');

	if (IsA(node, IntList))
		appendStringInfoChar(str, 'i');
	else if (IsA(node, OidList))
		appendStringInfoChar(str, 'o');
	else if (IsA(node, XidList))
		appendStringInfoChar(str, 'x');

	foreach(lc, node)
	{
		/*
		 * For the sake of backward compatibility, we emit a slightly
		 * different whitespace format for lists of nodes vs. other types of
		 * lists. XXX: is this necessary?
		 */
		if (IsA(node, List))
		{
			outNode(str, lfirst(lc));
			if (lnext(node, lc))
				appendStringInfoChar(str, ' ');
		}
		else if (IsA(node, IntList))
			appendStringInfo(str, " %d", lfirst_int(lc));
		else if (IsA(node, OidList))
			appendStringInfo(str, " %u", lfirst_oid(lc));
		else if (IsA(node, XidList))
			appendStringInfo(str, " %u", lfirst_xid(lc));
		else
			elog(ERROR, "unrecognized list node type: %d",
				 (int) node->type);
	}

	appendStringInfoChar(str, ')');
}

/*
 * outBitmapset -
 *	   converts a bitmap set of integers
 *
 * Note: the output format is "(b int int ...)", similar to an integer List.
 */
void
outBitmapset(StringInfo str, const Bitmapset *bms)
{
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	x = -1;
	while ((x = bms_next_member(bms, x)) >= 0)
		appendStringInfo(str, " %d", x);
	appendStringInfoChar(str, ')');
}

static void
_outBitmapset(StringInfo str, const Bitmapset *node)
{
	outBitmapset(str, node);
}

/*
 * Print the value of a Datum given its type.
 */
void
outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, "%u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfoChar(str, ']');
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfoString(str, "0 [ ]");
		else
		{
			appendStringInfo(str, "%u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfoChar(str, ']');
		}
	}
}

#endif /* COMPILING_BINARY_FUNCS */

/* ****************************************************************
 *				 generated out functions
 * ****************************************************************
 */
#ifndef COMPILING_BINARY_FUNCS
#include "outfuncs.funcs.c"
#endif /* COMPILING_BINARY_FUNCS */


/*
 *	Stuff from plannodes.h
 */

static void
_outPlannedStmt(StringInfo str, const PlannedStmt *node)
{
	WRITE_NODE_TYPE("PLANNEDSTMT");

	WRITE_ENUM_FIELD(commandType, CmdType);
	WRITE_ENUM_FIELD(planGen, PlanGenerator);
	WRITE_UINT64_FIELD(queryId);
	WRITE_BOOL_FIELD(hasReturning);
	WRITE_BOOL_FIELD(hasModifyingCTE);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_BOOL_FIELD(transientPlan);
	WRITE_BOOL_FIELD(oneoffPlan);
	WRITE_OID_FIELD(simplyUpdatableRel);
	WRITE_BOOL_FIELD(dependsOnRole);
	WRITE_BOOL_FIELD(parallelModeNeeded);
	WRITE_INT_FIELD(jitFlags);
	WRITE_NODE_FIELD(planTree);
	WRITE_NODE_FIELD(rtable);
	WRITE_NODE_FIELD(permInfos);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(appendRelations);
	WRITE_NODE_FIELD(subplans);
	WRITE_BITMAPSET_FIELD(rewindPlanIDs);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(relationOids);
	/*
	 * Don't serialize invalItems when dispatching. The TIDs of the invalidated items wouldn't
	 * make sense in segments.
	 */
#ifndef COMPILING_BINARY_FUNCS
	WRITE_NODE_FIELD(invalItems);
#endif /* COMPILING_BINARY_FUNCS */
	WRITE_NODE_FIELD(paramExecTypes);
	WRITE_NODE_FIELD(utilityStmt);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);

	WRITE_INT_ARRAY(subplan_sliceIds, list_length(node->subplans));

	WRITE_INT_FIELD(numSlices);
	for (int i = 0; i < node->numSlices; i++)
	{
		WRITE_INT_FIELD(slices[i].sliceIndex);
		WRITE_INT_FIELD(slices[i].parentIndex);
		WRITE_INT_FIELD(slices[i].gangType);
		WRITE_INT_FIELD(slices[i].numsegments);
		WRITE_INT_FIELD(slices[i].parallel_workers);
		WRITE_INT_FIELD(slices[i].segindex);
		WRITE_BOOL_FIELD(slices[i].directDispatch.isDirectDispatch);
		WRITE_NODE_FIELD(slices[i].directDispatch.contentIds);
	}

	WRITE_BITMAPSET_FIELD(rewindPlanIDs);

	WRITE_NODE_FIELD(intoPolicy);

	WRITE_UINT64_FIELD(query_mem);

	WRITE_NODE_FIELD(intoClause);
	WRITE_NODE_FIELD(copyIntoClause);
	WRITE_NODE_FIELD(refreshClause);
	WRITE_INT_FIELD(metricsQueryType);
	WRITE_NODE_FIELD(extensionContext);

}

/*
 * print the basic stuff of all nodes that inherit from Plan
 */
static void
_outPlanInfo(StringInfo str, const Plan *node)
{
	WRITE_FLOAT_FIELD(startup_cost);
	WRITE_FLOAT_FIELD(total_cost);
	WRITE_FLOAT_FIELD(plan_rows);
	WRITE_INT_FIELD(plan_width);
	WRITE_BOOL_FIELD(parallel_aware);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_BOOL_FIELD(async_capable);
	WRITE_INT_FIELD(plan_node_id);
	WRITE_NODE_FIELD(targetlist);
	WRITE_NODE_FIELD(qual);
	WRITE_NODE_FIELD(lefttree);
	WRITE_NODE_FIELD(righttree);
	WRITE_NODE_FIELD(initPlan);
	WRITE_BITMAPSET_FIELD(extParam);
	WRITE_BITMAPSET_FIELD(allParam);
	/* 'flow' is only needed during planning. */
#ifndef COMPILING_BINARY_FUNCS
	WRITE_NODE_FIELD(flow);
#endif /* COMPILING_BINARY_FUNCS */

	WRITE_UINT64_FIELD(operatorMemKB);
}

/*
 * print the basic stuff of all nodes that inherit from Scan
 */
static void
_outScanInfo(StringInfo str, const Scan *node)
{
	_outPlanInfo(str, (const Plan *) node);

	WRITE_UINT_FIELD(scanrelid);
}

/*
 * print the basic stuff of all nodes that inherit from Join
 */
static void
_outJoinPlanInfo(StringInfo str, const Join *node)
{
	_outPlanInfo(str, (const Plan *) node);

	WRITE_BOOL_FIELD(prefetch_inner);
	WRITE_BOOL_FIELD(prefetch_joinqual);
	WRITE_BOOL_FIELD(prefetch_qual);

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(inner_unique);
	WRITE_NODE_FIELD(joinqual);
}


static void
_outPlan(StringInfo str, const Plan *node)
{
	WRITE_NODE_TYPE("PLAN");

	_outPlanInfo(str, (const Plan *) node);
}

static void
_outResult(StringInfo str, const Result *node)
{
	WRITE_NODE_TYPE("RESULT");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(resconstantqual);

	WRITE_INT_FIELD(numHashFilterCols);
	WRITE_ATTRNUMBER_ARRAY(hashFilterColIdx, node->numHashFilterCols);
	WRITE_OID_ARRAY(hashFilterFuncs, node->numHashFilterCols);
}

static void
_outProjectSet(StringInfo str, const ProjectSet *node)
{
	WRITE_NODE_TYPE("PROJECTSET");

	_outPlanInfo(str, (const Plan *) node);
}

static void
_outModifyTable(StringInfo str, const ModifyTable *node)
{
	WRITE_NODE_TYPE("MODIFYTABLE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_ENUM_FIELD(operation, CmdType);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_UINT_FIELD(nominalRelation);
	WRITE_UINT_FIELD(rootRelation);
	WRITE_BOOL_FIELD(partColsUpdated);
	WRITE_BOOL_FIELD(splitUpdate);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(updateColnosLists);
	WRITE_NODE_FIELD(withCheckOptionLists);
	WRITE_NODE_FIELD(returningLists);
	WRITE_NODE_FIELD(fdwPrivLists);
	WRITE_BITMAPSET_FIELD(fdwDirectModifyPlans);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
	WRITE_ENUM_FIELD(onConflictAction, OnConflictAction);
	WRITE_NODE_FIELD(arbiterIndexes);
	WRITE_NODE_FIELD(onConflictSet);
	WRITE_NODE_FIELD(onConflictCols);
	WRITE_NODE_FIELD(onConflictWhere);
	WRITE_UINT_FIELD(exclRelRTI);
	WRITE_NODE_FIELD(exclRelTlist);
	WRITE_BOOL_FIELD(forceTupleRouting);
	WRITE_NODE_FIELD(mergeActionLists);
}

static void
_outAppend(StringInfo str, const Append *node)
{
	WRITE_NODE_TYPE("APPEND");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_BITMAPSET_FIELD(apprelids);
	WRITE_NODE_FIELD(appendplans);
	WRITE_INT_FIELD(nasyncplans);
	WRITE_INT_FIELD(first_partial_plan);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outMergeAppend(StringInfo str, const MergeAppend *node)
{
	WRITE_NODE_TYPE("MERGEAPPEND");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_BITMAPSET_FIELD(apprelids);
	WRITE_NODE_FIELD(mergeplans);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numCols);
	WRITE_OID_ARRAY(sortOperators, node->numCols);
	WRITE_OID_ARRAY(collations, node->numCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numCols);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outRecursiveUnion(StringInfo str, const RecursiveUnion *node)
{
	WRITE_NODE_TYPE("RECURSIVEUNION");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(wtParam);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(dupColIdx, node->numCols);
	WRITE_OID_ARRAY(dupOperators, node->numCols);
	WRITE_OID_ARRAY(dupCollations, node->numCols);
	WRITE_LONG_FIELD(numGroups);
}

static void
_outBitmapAnd(StringInfo str, const BitmapAnd *node)
{
	WRITE_NODE_TYPE("BITMAPAND");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(bitmapplans);
}

static void
_outBitmapOr(StringInfo str, const BitmapOr *node)
{
	WRITE_NODE_TYPE("BITMAPOR");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_BOOL_FIELD(isshared);
	WRITE_NODE_FIELD(bitmapplans);
}

static void
_outGather(StringInfo str, const Gather *node)
{
	WRITE_NODE_TYPE("GATHER");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(num_workers);
	WRITE_INT_FIELD(rescan_param);
	WRITE_BOOL_FIELD(single_copy);
	WRITE_BOOL_FIELD(invisible);
	WRITE_BITMAPSET_FIELD(initParam);
}

static void
_outGatherMerge(StringInfo str, const GatherMerge *node)
{
	WRITE_NODE_TYPE("GATHERMERGE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(num_workers);
	WRITE_INT_FIELD(rescan_param);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numCols);
	WRITE_OID_ARRAY(sortOperators, node->numCols);
	WRITE_OID_ARRAY(collations, node->numCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numCols);
	WRITE_BITMAPSET_FIELD(initParam);
}

static void
_outScan(StringInfo str, const Scan *node)
{
	WRITE_NODE_TYPE("SCAN");

	_outScanInfo(str, node);
}

static void
_outSeqScan(StringInfo str, const SeqScan *node)
{
	WRITE_NODE_TYPE("SEQSCAN");

	_outScanInfo(str, (const Scan *) node);
}

static void
_outDynamicSeqScan(StringInfo str, const DynamicSeqScan *node)
{
	WRITE_NODE_TYPE("DYNAMICSEQSCAN");

	_outScanInfo(str, (Scan *)node);
	WRITE_NODE_FIELD(partOids);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outSampleScan(StringInfo str, const SampleScan *node)
{
	WRITE_NODE_TYPE("SAMPLESCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(tablesample);
}

static void
outIndexScanFields(StringInfo str, const IndexScan *node)
{
	_outScanInfo(str, (const Scan *) node);

	WRITE_OID_FIELD(indexid);
	WRITE_NODE_FIELD(indexqual);
	WRITE_NODE_FIELD(indexqualorig);
	WRITE_NODE_FIELD(indexorderby);
	WRITE_NODE_FIELD(indexorderbyorig);
	WRITE_NODE_FIELD(indexorderbyops);
	WRITE_ENUM_FIELD(indexorderdir, ScanDirection);
}

static void
_outIndexScan(StringInfo str, const IndexScan *node)
{
	WRITE_NODE_TYPE("INDEXSCAN");

	outIndexScanFields(str, node);
}

static void
outIndexOnlyScanFields(StringInfo str, const IndexOnlyScan *node)
{
	_outScanInfo(str, (const Scan *) node);

	WRITE_OID_FIELD(indexid);
	WRITE_NODE_FIELD(indexqual);
	WRITE_NODE_FIELD(recheckqual);
	WRITE_NODE_FIELD(indexorderby);
	WRITE_NODE_FIELD(indextlist);
	WRITE_ENUM_FIELD(indexorderdir, ScanDirection);
}

static void
_outIndexOnlyScan(StringInfo str, const IndexOnlyScan *node)
{
	WRITE_NODE_TYPE("INDEXONLYSCAN");

	outIndexOnlyScanFields(str, node);
}

static void
_outDynamicIndexScan(StringInfo str, const DynamicIndexScan *node)
{
	WRITE_NODE_TYPE("DYNAMICINDEXSCAN");

	outIndexScanFields(str, &node->indexscan);
	WRITE_NODE_FIELD(partOids);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outDynamicIndexOnlyScan(StringInfo str, const DynamicIndexOnlyScan *node)
{
	WRITE_NODE_TYPE("DYNAMICINDEXONLYSCAN");

	outIndexOnlyScanFields(str, &node->indexscan);
	WRITE_NODE_FIELD(partOids);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outBitmapIndexScanFields(StringInfo str, const BitmapIndexScan *node)
{
	_outScanInfo(str, (Scan *) node);

	WRITE_OID_FIELD(indexid);
	WRITE_BOOL_FIELD(isshared);
	WRITE_NODE_FIELD(indexqual);
	WRITE_NODE_FIELD(indexqualorig);
}

static void
_outBitmapIndexScan(StringInfo str, const BitmapIndexScan *node)
{
	WRITE_NODE_TYPE("BITMAPINDEXSCAN");

	_outBitmapIndexScanFields(str, node);
}

static void
_outDynamicBitmapIndexScan(StringInfo str, const DynamicBitmapIndexScan *node)
{
	WRITE_NODE_TYPE("DYNAMICBITMAPINDEXSCAN");

	_outBitmapIndexScanFields(str, &node->biscan);
}

static void
outBitmapHeapScanFields(StringInfo str, const BitmapHeapScan *node)
{
	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(bitmapqualorig);
}

static void
_outBitmapHeapScan(StringInfo str, const BitmapHeapScan *node)
{
	WRITE_NODE_TYPE("BITMAPHEAPSCAN");

	outBitmapHeapScanFields(str, node);
}

static void
_outDynamicBitmapHeapScan(StringInfo str, const DynamicBitmapHeapScan *node)
{
	WRITE_NODE_TYPE("DYNAMICBITMAPHEAPSCAN");

	outBitmapHeapScanFields(str, &node->bitmapheapscan);
	WRITE_NODE_FIELD(partOids);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
}

static void
_outTidScan(StringInfo str, const TidScan *node)
{
	WRITE_NODE_TYPE("TIDSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(tidquals);
}

static void
_outSubqueryScan(StringInfo str, const SubqueryScan *node)
{
	WRITE_NODE_TYPE("SUBQUERYSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(subplan);
}

static void
_outFunctionScan(StringInfo str, const FunctionScan *node)
{
	WRITE_NODE_TYPE("FUNCTIONSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(functions);
	WRITE_BOOL_FIELD(funcordinality);
	WRITE_NODE_FIELD(param);
	WRITE_BOOL_FIELD(resultInTupleStore);
	WRITE_INT_FIELD(initplanId);
}

static void
_outTableFuncScan(StringInfo str, const TableFuncScan *node)
{
	WRITE_NODE_TYPE("TABLEFUNCSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(tablefunc);
}

static void
_outValuesScan(StringInfo str, const ValuesScan *node)
{
	WRITE_NODE_TYPE("VALUESSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_NODE_FIELD(values_lists);
}

static void
_outCteScan(StringInfo str, const CteScan *node)
{
	WRITE_NODE_TYPE("CTESCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_INT_FIELD(ctePlanId);
	WRITE_INT_FIELD(cteParam);
}

static void
_outNamedTuplestoreScan(StringInfo str, const NamedTuplestoreScan *node)
{
	WRITE_NODE_TYPE("NAMEDTUPLESTORESCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_STRING_FIELD(enrname);
}

static void
_outWorkTableScan(StringInfo str, const WorkTableScan *node)
{
	WRITE_NODE_TYPE("WORKTABLESCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_INT_FIELD(wtParam);
}

static void
outForeignScanFields(StringInfo str, const ForeignScan *node)
{
	_outScanInfo(str, (const Scan *) node);

	WRITE_ENUM_FIELD(operation, CmdType);
	WRITE_UINT_FIELD(resultRelation);
	WRITE_OID_FIELD(fs_server);
	WRITE_NODE_FIELD(fdw_exprs);
	WRITE_NODE_FIELD(fdw_private);
	WRITE_NODE_FIELD(fdw_scan_tlist);
	WRITE_NODE_FIELD(fdw_recheck_quals);
	WRITE_BITMAPSET_FIELD(fs_relids);
	WRITE_BOOL_FIELD(fsSystemCol);
}

static void
_outForeignScan(StringInfo str, const ForeignScan *node)
{
	WRITE_NODE_TYPE("FOREIGNSCAN");

	outForeignScanFields(str, node);
}

static void
_outDynamicForeignScan(StringInfo str, const DynamicForeignScan *node)
{
	WRITE_NODE_TYPE("DYNAMICFOREIGNSCAN");

	outForeignScanFields(str, &node->foreignscan);
	WRITE_NODE_FIELD(partOids);
	WRITE_NODE_FIELD(part_prune_info);
	WRITE_NODE_FIELD(join_prune_paramids);
	WRITE_NODE_FIELD(fdw_private_list);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outCustomScan(StringInfo str, const CustomScan *node)
{
	WRITE_NODE_TYPE("CUSTOMSCAN");

	_outScanInfo(str, (const Scan *) node);

	WRITE_UINT_FIELD(flags);
	WRITE_NODE_FIELD(custom_plans);
	WRITE_NODE_FIELD(custom_exprs);
	WRITE_NODE_FIELD(custom_private);
	WRITE_NODE_FIELD(custom_scan_tlist);
	WRITE_BITMAPSET_FIELD(custom_relids);
	/* CustomName is a key to lookup CustomScanMethods */
	appendStringInfoString(str, " :methods ");
	outToken(str, node->methods->CustomName);
}
#endif /* COMPILING_BINARY_FUNCS */

static void
_outJoin(StringInfo str, const Join *node)
{
	WRITE_NODE_TYPE("JOIN");

	_outJoinPlanInfo(str, (const Join *) node);
}

static void
_outNestLoop(StringInfo str, const NestLoop *node)
{
	WRITE_NODE_TYPE("NESTLOOP");

	_outJoinPlanInfo(str, (const Join *) node);

	WRITE_NODE_FIELD(nestParams);

	WRITE_BOOL_FIELD(shared_outer);
	WRITE_BOOL_FIELD(singleton_outer); /*CDB-OLAP*/
}

static void
_outMergeJoin(StringInfo str, const MergeJoin *node)
{
	int			numCols;

	WRITE_NODE_TYPE("MERGEJOIN");

	_outJoinPlanInfo(str, (const Join *) node);

	WRITE_BOOL_FIELD(skip_mark_restore);
	WRITE_NODE_FIELD(mergeclauses);

	numCols = list_length(node->mergeclauses);

	WRITE_OID_ARRAY(mergeFamilies, numCols);
	WRITE_OID_ARRAY(mergeCollations, numCols);
	WRITE_INT_ARRAY(mergeStrategies, numCols);
	WRITE_BOOL_ARRAY(mergeNullsFirst, numCols);
    WRITE_BOOL_FIELD(unique_outer);
}

static void
_outHashJoin(StringInfo str, const HashJoin *node)
{
	WRITE_NODE_TYPE("HASHJOIN");

	_outJoinPlanInfo(str, (const Join *) node);

	WRITE_NODE_FIELD(hashclauses);
	WRITE_NODE_FIELD(hashoperators);
	WRITE_NODE_FIELD(hashcollations);
	WRITE_NODE_FIELD(hashkeys);
	WRITE_NODE_FIELD(hashqualclauses);
	WRITE_BOOL_FIELD(batch0_barrier);
	WRITE_BOOL_FIELD(outer_motionhazard);
}

static void
_outAgg(StringInfo str, const Agg *node)
{
	WRITE_NODE_TYPE("AGG");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(grpColIdx, node->numCols);
	WRITE_OID_ARRAY(grpOperators, node->numCols);
	WRITE_OID_ARRAY(grpCollations, node->numCols);
	WRITE_LONG_FIELD(numGroups);
	WRITE_UINT64_FIELD(transitionSpace);
	WRITE_BITMAPSET_FIELD(aggParams);
	WRITE_NODE_FIELD(groupingSets);
	WRITE_NODE_FIELD(chain);
	WRITE_BOOL_FIELD(streaming);
	WRITE_UINT_FIELD(agg_expr_id);
}

static void
_outWindowAgg(StringInfo str, const WindowAgg *node)
{
	WRITE_NODE_TYPE("WINDOWAGG");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_UINT_FIELD(winref);
	WRITE_INT_FIELD(partNumCols);
	WRITE_ATTRNUMBER_ARRAY(partColIdx, node->partNumCols);
	WRITE_OID_ARRAY(partOperators, node->partNumCols);
	WRITE_OID_ARRAY(partCollations, node->partNumCols);
	WRITE_INT_FIELD(ordNumCols);
	WRITE_ATTRNUMBER_ARRAY(ordColIdx, node->ordNumCols);
	WRITE_OID_ARRAY(ordOperators, node->ordNumCols);
	WRITE_OID_ARRAY(ordCollations, node->ordNumCols);
	WRITE_INT_FIELD(firstOrderCol);
	WRITE_OID_FIELD(firstOrderCmpOperator);
	WRITE_BOOL_FIELD(firstOrderNullsFirst);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_NODE_FIELD(runCondition);
	WRITE_NODE_FIELD(runConditionOrig);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
	WRITE_BOOL_FIELD(topWindow);
}

static void
_outWindowHashAgg(StringInfo str, const WindowHashAgg *node)
{
	WRITE_NODE_TYPE("WINDOWHASHAGG");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_UINT_FIELD(winref);
	WRITE_INT_FIELD(partNumCols);
	WRITE_ATTRNUMBER_ARRAY(partColIdx, node->partNumCols);
	WRITE_OID_ARRAY(partOperators, node->partNumCols);
	WRITE_OID_ARRAY(partCollations, node->partNumCols);
	WRITE_INT_FIELD(ordNumCols);
	WRITE_ATTRNUMBER_ARRAY(ordColIdx, node->ordNumCols);
	WRITE_OID_ARRAY(ordOperators, node->ordNumCols);
	WRITE_OID_ARRAY(ordCollations, node->ordNumCols);
	WRITE_BOOL_ARRAY(ordNullsFirst, node->ordNumCols);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
}

static void
_outGroup(StringInfo str, const Group *node)
{
	WRITE_NODE_TYPE("GROUP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(grpColIdx, node->numCols);
	WRITE_OID_ARRAY(grpOperators, node->numCols);
	WRITE_OID_ARRAY(grpCollations, node->numCols);
}

static void
_outMaterial(StringInfo str, const Material *node)
{
	WRITE_NODE_TYPE("MATERIAL");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_BOOL_FIELD(cdb_strict);
	WRITE_BOOL_FIELD(cdb_shield_child_from_rescans);
}

static void
_outSortInfo(StringInfo str, const Sort *node)
{
	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numCols);
	WRITE_OID_ARRAY(sortOperators, node->numCols);
	WRITE_OID_ARRAY(collations, node->numCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numCols);
}

static void
_outSort(StringInfo str, const Sort *node)
{
	WRITE_NODE_TYPE("SORT");

	_outSortInfo(str, node);
}

static void
_outIncrementalSort(StringInfo str, const IncrementalSort *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORT");

	_outSortInfo(str, (const Sort *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outUnique(StringInfo str, const Unique *node)
{
	WRITE_NODE_TYPE("UNIQUE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->numCols);
	WRITE_OID_ARRAY(uniqOperators, node->numCols);
	WRITE_OID_ARRAY(uniqCollations, node->numCols);
}

static void
_outHash(StringInfo str, const Hash *node)
{
	WRITE_NODE_TYPE("HASH");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(hashkeys);
	WRITE_OID_FIELD(skewTable);
	WRITE_INT_FIELD(skewColumn);
	WRITE_BOOL_FIELD(skewInherit);
	WRITE_FLOAT_FIELD(rows_total);
	WRITE_BOOL_FIELD(rescannable);          /*CDB*/
	WRITE_BOOL_FIELD(sync_barrier);
}

static void
_outSetOp(StringInfo str, const SetOp *node)
{
	WRITE_NODE_TYPE("SETOP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(dupColIdx, node->numCols);
	WRITE_OID_ARRAY(dupOperators, node->numCols);
	WRITE_OID_ARRAY(dupCollations, node->numCols);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_LONG_FIELD(numGroups);
}

static void
_outLockRows(StringInfo str, const LockRows *node)
{
	WRITE_NODE_TYPE("LOCKROWS");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outRuntimeFilter(StringInfo str, const RuntimeFilter *node)
{
	WRITE_NODE_TYPE("RUNTIME_FILTER");

	_outPlanInfo(str, (const Plan *) node);
}

static void
_outLimit(StringInfo str, const Limit *node)
{
	WRITE_NODE_TYPE("LIMIT");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_INT_FIELD(uniqNumCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqOperators, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqCollations, node->uniqNumCols);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outPlanInvalItem(StringInfo str, const PlanInvalItem *node)
{
	WRITE_NODE_TYPE("PLANINVALITEM");

	WRITE_INT_FIELD(cacheId);
	WRITE_UINT_FIELD(hashValue);
}
#endif /* COMPILING_BINARY_FUNCS */

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

#ifndef COMPILING_BINARY_FUNCS
static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}
#endif /* COMPILING_BINARY_FUNCS */

#ifndef COMPILING_BINARY_FUNCS
static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}
#endif /* COMPILING_BINARY_FUNCS */

/*****************************************************************************
 *
 *	Stuff from pathnodes.h.
 *
 *****************************************************************************/

/*
 * None of this stuff is needed after planning, and doesn't need to be
 * dispatched to QEs.
 */
#ifndef COMPILING_BINARY_FUNCS

/*
 * print the basic stuff of all nodes that inherit from Path
 *
 * Note we do NOT print the parent, else we'd be in infinite recursion.
 * We can print the parent's relids for identification purposes, though.
 * We print the pathtarget only if it's not the default one for the rel.
 * We also do not print the whole of param_info, since it's printed by
 * _outRelOptInfo; it's sufficient and less cluttering to print just the
 * required outer relids.
 */
static void
_outPathInfo(StringInfo str, const Path *node)
{
	WRITE_ENUM_FIELD(pathtype, NodeTag);
	appendStringInfoString(str, " :parent_relids ");
	outBitmapset(str, node->parent->relids);
	if (node->pathtarget != node->parent->reltarget)
		WRITE_NODE_FIELD(pathtarget);
	appendStringInfoString(str, " :required_outer ");
	if (node->param_info)
		outBitmapset(str, node->param_info->ppi_req_outer);
	else
		outBitmapset(str, NULL);
	WRITE_BOOL_FIELD(parallel_aware);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_INT_FIELD(parallel_workers);
	WRITE_FLOAT_FIELD(rows);
	WRITE_FLOAT_FIELD(startup_cost);
	WRITE_FLOAT_FIELD(total_cost);
    _outCdbPathLocus(str, &node->locus);
	WRITE_NODE_FIELD(pathkeys);
}

/*
 * print the basic stuff of all nodes that inherit from JoinPath
 */
static void
_outJoinPathInfo(StringInfo str, const JoinPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(inner_unique);
	WRITE_NODE_FIELD(outerjoinpath);
	WRITE_NODE_FIELD(innerjoinpath);
	WRITE_NODE_FIELD(joinrestrictinfo);
}

static void
_outPath(StringInfo str, const Path *node)
{
	WRITE_NODE_TYPE("PATH");

	_outPathInfo(str, (const Path *) node);
}

static void
_outIndexPath(StringInfo str, const IndexPath *node)
{
	WRITE_NODE_TYPE("INDEXPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(indexinfo);
	WRITE_NODE_FIELD(indexclauses);
	WRITE_NODE_FIELD(indexorderbys);
	WRITE_NODE_FIELD(indexorderbycols);
	WRITE_ENUM_FIELD(indexscandir, ScanDirection);
	WRITE_FLOAT_FIELD(indextotalcost);
	WRITE_FLOAT_FIELD(indexselectivity);
    WRITE_INT_FIELD(num_leading_eq);
}

static void
_outBitmapHeapPath(StringInfo str, const BitmapHeapPath *node)
{
	WRITE_NODE_TYPE("BITMAPHEAPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapqual);
}

static void
_outBitmapAndPath(StringInfo str, const BitmapAndPath *node)
{
	WRITE_NODE_TYPE("BITMAPANDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity);
}

static void
_outBitmapOrPath(StringInfo str, const BitmapOrPath *node)
{
	WRITE_NODE_TYPE("BITMAPORPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity);
}

static void
_outTidPath(StringInfo str, const TidPath *node)
{
	WRITE_NODE_TYPE("TIDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidquals);
}

static void
_outTidRangePath(StringInfo str, const TidRangePath *node)
{
	WRITE_NODE_TYPE("TIDRANGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidrangequals);
}

static void
_outSubqueryScanPath(StringInfo str, const SubqueryScanPath *node)
{
	WRITE_NODE_TYPE("SUBQUERYSCANPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outForeignPath(StringInfo str, const ForeignPath *node)
{
	WRITE_NODE_TYPE("FOREIGNPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(fdw_outerpath);
	WRITE_NODE_FIELD(fdw_private);
}

static void
_outCustomPath(StringInfo str, const CustomPath *node)
{
	WRITE_NODE_TYPE("CUSTOMPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_UINT_FIELD(flags);
	WRITE_NODE_FIELD(custom_paths);
	WRITE_NODE_FIELD(custom_private);
	appendStringInfoString(str, " :methods ");
	outToken(str, node->methods->CustomName);
}

static void
_outAppendPath(StringInfo str, const AppendPath *node)
{
	WRITE_NODE_TYPE("APPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_INT_FIELD(first_partial_path);
	WRITE_FLOAT_FIELD(limit_tuples);
}

static void
_outMergeAppendPath(StringInfo str, const MergeAppendPath *node)
{
	WRITE_NODE_TYPE("MERGEAPPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_FLOAT_FIELD(limit_tuples);
}

static void
_outGroupResultPath(StringInfo str, const GroupResultPath *node)
{
	WRITE_NODE_TYPE("GROUPRESULTPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(quals);
}

static void
_outMaterialPath(StringInfo str, const MaterialPath *node)
{
	WRITE_NODE_TYPE("MATERIALPATH");

	_outPathInfo(str, (const Path *) node);
	WRITE_BOOL_FIELD(cdb_strict);
	WRITE_BOOL_FIELD(cdb_shield_child_from_rescans);

	WRITE_NODE_FIELD(subpath);
}

static void
_outMemoizePath(StringInfo str, const MemoizePath *node)
{
	WRITE_NODE_TYPE("MEMOIZEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(hash_operators);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_BOOL_FIELD(binary_mode);
	WRITE_FLOAT_FIELD(calls);
	WRITE_UINT_FIELD(est_entries);
}

static void
_outUniquePath(StringInfo str, const UniquePath *node)
{
	WRITE_NODE_TYPE("UNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(umethod, UniquePathMethod);
	WRITE_NODE_FIELD(in_operators);
	WRITE_NODE_FIELD(uniq_exprs);
}

static void
_outGatherPath(StringInfo str, const GatherPath *node)
{
	WRITE_NODE_TYPE("GATHERPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(single_copy);
	WRITE_INT_FIELD(num_workers);
}

static void
_outProjectionPath(StringInfo str, const ProjectionPath *node)
{
	WRITE_NODE_TYPE("PROJECTIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(dummypp);
}

static void
_outProjectSetPath(StringInfo str, const ProjectSetPath *node)
{
	WRITE_NODE_TYPE("PROJECTSETPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPathInfo(StringInfo str, const SortPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPath(StringInfo str, const SortPath *node)
{
	WRITE_NODE_TYPE("SORTPATH");

	_outSortPathInfo(str, node);
}

static void
_outIncrementalSortPath(StringInfo str, const IncrementalSortPath *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORTPATH");

	_outSortPathInfo(str, (const SortPath *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outGroupPath(StringInfo str, const GroupPath *node)
{
	WRITE_NODE_TYPE("GROUPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
}

static void
_outUpperUniquePath(StringInfo str, const UpperUniquePath *node)
{
	WRITE_NODE_TYPE("UPPERUNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(numkeys);
}

static void
_outAggPath(StringInfo str, const AggPath *node)
{
	WRITE_NODE_TYPE("AGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_FLOAT_FIELD(numGroups);
	WRITE_UINT64_FIELD(transitionSpace);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
	WRITE_BOOL_FIELD(streaming);
}

static void
_outRollupData(StringInfo str, const RollupData *node)
{
	WRITE_NODE_TYPE("ROLLUP");

	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(gsets);
	WRITE_NODE_FIELD(gsets_data);
	WRITE_FLOAT_FIELD(numGroups);
	WRITE_BOOL_FIELD(hashable);
	WRITE_BOOL_FIELD(is_hashed);
}

static void
_outGroupingSetData(StringInfo str, const GroupingSetData *node)
{
	WRITE_NODE_TYPE("GSDATA");

	WRITE_NODE_FIELD(set);
	WRITE_FLOAT_FIELD(numGroups);
}

static void
_outGroupingSetsPath(StringInfo str, const GroupingSetsPath *node)
{
	WRITE_NODE_TYPE("GROUPINGSETSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_NODE_FIELD(rollups);
	WRITE_NODE_FIELD(qual);
	WRITE_UINT64_FIELD(transitionSpace);
}

static void
_outMinMaxAggPath(StringInfo str, const MinMaxAggPath *node)
{
	WRITE_NODE_TYPE("MINMAXAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(mmaggregates);
	WRITE_NODE_FIELD(quals);
}

static void
_outWindowAggPath(StringInfo str, const WindowAggPath *node)
{
	WRITE_NODE_TYPE("WINDOWAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(winclause);
}

static void
_outSetOpPath(StringInfo str, const SetOpPath *node)
{
	WRITE_NODE_TYPE("SETOPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_FLOAT_FIELD(numGroups);
}

static void
_outRecursiveUnionPath(StringInfo str, const RecursiveUnionPath *node)
{
	WRITE_NODE_TYPE("RECURSIVEUNIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(leftpath);
	WRITE_NODE_FIELD(rightpath);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(wtParam);
	WRITE_FLOAT_FIELD(numGroups);
}

static void
_outLockRowsPath(StringInfo str, const LockRowsPath *node)
{
	WRITE_NODE_TYPE("LOCKROWSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outModifyTablePath(StringInfo str, const ModifyTablePath *node)
{
	WRITE_NODE_TYPE("MODIFYTABLEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(operation, CmdType);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_UINT_FIELD(nominalRelation);
	WRITE_UINT_FIELD(rootRelation);
	WRITE_BOOL_FIELD(partColsUpdated);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(updateColnosLists);
	WRITE_NODE_FIELD(withCheckOptionLists);
	WRITE_NODE_FIELD(returningLists);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(onconflict);
	WRITE_INT_FIELD(epqParam);
	WRITE_NODE_FIELD(mergeActionLists);
}

static void
_outRuntimeFilterPath(StringInfo str, const LimitPath *node)
{
	WRITE_NODE_TYPE("RUNTIMEFILTER");

	_outPathInfo(str, (const Path *) node);
}

static void
_outLimitPath(StringInfo str, const LimitPath *node)
{
	WRITE_NODE_TYPE("LIMITPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
}

static void
_outGatherMergePath(StringInfo str, const GatherMergePath *node)
{
	WRITE_NODE_TYPE("GATHERMERGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(num_workers);
}

static void
_outNestPath(StringInfo str, const NestPath *node)
{
	WRITE_NODE_TYPE("NESTPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);
}

static void
_outMergePath(StringInfo str, const MergePath *node)
{
	WRITE_NODE_TYPE("MERGEPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_mergeclauses);
	WRITE_NODE_FIELD(outersortkeys);
	WRITE_NODE_FIELD(innersortkeys);
	WRITE_BOOL_FIELD(skip_mark_restore);
	WRITE_BOOL_FIELD(materialize_inner);
}

static void
_outHashPath(StringInfo str, const HashPath *node)
{
	WRITE_NODE_TYPE("HASHPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_hashclauses);
	WRITE_INT_FIELD(num_batches);
	WRITE_FLOAT_FIELD(inner_rows_total);
}

static void
_outPlannerGlobal(StringInfo str, const PlannerGlobal *node)
{
	WRITE_NODE_TYPE("PLANNERGLOBAL");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(subplans);
	WRITE_BITMAPSET_FIELD(rewindPlanIDs);
	WRITE_NODE_FIELD(finalrtable);
	WRITE_NODE_FIELD(finalrowmarks);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(appendRelations);
	WRITE_NODE_FIELD(relationOids);
	WRITE_NODE_FIELD(invalItems);
	WRITE_NODE_FIELD(paramExecTypes);
	WRITE_UINT_FIELD(lastPHId);
	WRITE_UINT_FIELD(lastRowMarkId);
	WRITE_INT_FIELD(lastPlanNodeId);
	WRITE_BOOL_FIELD(transientPlan);
	WRITE_BOOL_FIELD(oneoffPlan);
	WRITE_NODE_FIELD(share.motStack);
	WRITE_BITMAPSET_FIELD(share.qdShares);
	WRITE_BOOL_FIELD(dependsOnRole);
	WRITE_BOOL_FIELD(parallelModeOK);
	WRITE_BOOL_FIELD(parallelModeNeeded);
	WRITE_CHAR_FIELD(maxParallelHazard);
}

static void
_outPlannerInfo(StringInfo str, const PlannerInfo *node)
{
	WRITE_NODE_TYPE("PLANNERINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(parse);
	WRITE_NODE_FIELD(glob);
	WRITE_UINT_FIELD(query_level);
	WRITE_NODE_FIELD(plan_params);
	WRITE_BITMAPSET_FIELD(outer_params);
	WRITE_NODE_ARRAY(simple_rel_array, node->simple_rel_array_size);
	WRITE_INT_FIELD(simple_rel_array_size);
	WRITE_BITMAPSET_FIELD(all_baserels);
	WRITE_BITMAPSET_FIELD(outer_join_rels);
	WRITE_NODE_FIELD(join_rel_list);
	WRITE_NODE_FIELD(join_rel_list);
	WRITE_BOOL_FIELD(setup_agg_pushdown);
	WRITE_NODE_FIELD(grouped_rel_info_list);
	WRITE_INT_FIELD(join_cur_level);
	WRITE_NODE_FIELD(init_plans);
	WRITE_NODE_FIELD(cte_plan_ids);
	WRITE_NODE_FIELD(multiexpr_params);
	WRITE_NODE_FIELD(join_domains);
	WRITE_NODE_FIELD(eq_classes);
	WRITE_BOOL_FIELD(ec_merging_done);
	WRITE_NODE_FIELD(canon_pathkeys);
	WRITE_NODE_FIELD(left_join_clauses);
	WRITE_NODE_FIELD(right_join_clauses);
	WRITE_NODE_FIELD(full_join_clauses);
	WRITE_NODE_FIELD(join_info_list);
	WRITE_INT_FIELD(last_rinfo_serial);
	WRITE_BITMAPSET_FIELD(all_result_relids);
	WRITE_BITMAPSET_FIELD(leaf_result_relids);
	WRITE_NODE_FIELD(append_rel_list);
	WRITE_NODE_FIELD(row_identity_vars);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(placeholder_list);
	WRITE_NODE_FIELD(grouped_var_list);
	WRITE_NODE_FIELD(fkey_list);
	WRITE_NODE_FIELD(query_pathkeys);
	WRITE_NODE_FIELD(group_pathkeys);
	WRITE_INT_FIELD(num_groupby_pathkeys);
	WRITE_NODE_FIELD(window_pathkeys);
	WRITE_NODE_FIELD(distinct_pathkeys);
	WRITE_NODE_FIELD(sort_pathkeys);
	WRITE_NODE_FIELD(processed_groupClause);
	WRITE_NODE_FIELD(processed_distinctClause);
	WRITE_NODE_FIELD(processed_tlist);
	WRITE_INT_FIELD(max_sortgroupref);
	WRITE_NODE_FIELD(update_colnos);
	WRITE_NODE_FIELD(minmax_aggs);
	WRITE_FLOAT_FIELD(total_table_pages);
	WRITE_FLOAT_FIELD(tuple_fraction);
	WRITE_FLOAT_FIELD(limit_tuples);
	WRITE_UINT_FIELD(qual_security_level);
	WRITE_BOOL_FIELD(hasJoinRTEs);
	WRITE_BOOL_FIELD(hasLateralRTEs);
	WRITE_BOOL_FIELD(hasHavingQual);
	WRITE_BOOL_FIELD(hasPseudoConstantQuals);
	WRITE_BOOL_FIELD(hasAlternativeSubPlans);
	WRITE_BOOL_FIELD(placeholdersFrozen);
	WRITE_BOOL_FIELD(hasRecursion);
	WRITE_NODE_FIELD(agginfos);
	WRITE_NODE_FIELD(aggtransinfos);
	WRITE_INT_FIELD(numOrderedAggs);
	WRITE_BOOL_FIELD(hasNonPartialAggs);
	WRITE_BOOL_FIELD(hasNonSerialAggs);
	WRITE_INT_FIELD(wt_param_id);
	WRITE_NODE_FIELD(non_recursive_path);
	WRITE_BITMAPSET_FIELD(curOuterRels);
	WRITE_NODE_FIELD(curOuterParams);
	WRITE_BOOL_FIELD(partColsUpdated);
}

static void
_outRelOptInfo(StringInfo str, const RelOptInfo *node)
{
	WRITE_NODE_TYPE("RELOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_ENUM_FIELD(reloptkind, RelOptKind);
	WRITE_BITMAPSET_FIELD(relids);
	WRITE_FLOAT_FIELD(rows);
	WRITE_BOOL_FIELD(consider_startup);
	WRITE_BOOL_FIELD(consider_param_startup);
	WRITE_BOOL_FIELD(consider_parallel);
	WRITE_NODE_FIELD(reltarget);
	WRITE_NODE_FIELD(pathlist);
	WRITE_NODE_FIELD(ppilist);
	WRITE_NODE_FIELD(partial_pathlist);
	WRITE_NODE_FIELD(cheapest_startup_path);
	WRITE_NODE_FIELD(cheapest_total_path);
	WRITE_NODE_FIELD(cheapest_unique_path);
	WRITE_NODE_FIELD(cheapest_parameterized_paths);
	WRITE_BITMAPSET_FIELD(direct_lateral_relids);
	WRITE_BITMAPSET_FIELD(lateral_relids);
	WRITE_UINT_FIELD(relid);
	WRITE_OID_FIELD(reltablespace);
	WRITE_ENUM_FIELD(rtekind, RTEKind);
	WRITE_INT_FIELD(min_attr);
	WRITE_INT_FIELD(max_attr);
	WRITE_NODE_FIELD(lateral_vars);
	WRITE_BITMAPSET_FIELD(lateral_referencers);
	WRITE_NODE_FIELD(indexlist);
	WRITE_NODE_FIELD(statlist);
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples);
	WRITE_FLOAT_FIELD(allvisfrac);
	WRITE_BITMAPSET_FIELD(eclass_indexes);
	WRITE_NODE_FIELD(subroot);
	WRITE_NODE_FIELD(subplan_params);
	WRITE_INT_FIELD(rel_parallel_workers);
	WRITE_UINT_FIELD(amflags);
	WRITE_OID_FIELD(serverid);
	WRITE_OID_FIELD(userid);
	WRITE_BOOL_FIELD(useridiscurrent);
	/* we don't try to print fdwroutine or fdw_private */
	/* can't print unique_for_rels/non_unique_for_rels; BMSes aren't Nodes */
	WRITE_NODE_FIELD(baserestrictinfo);
	WRITE_UINT_FIELD(baserestrict_min_security);
	WRITE_NODE_FIELD(joininfo);
	WRITE_BOOL_FIELD(has_eclass_joins);
	WRITE_BOOL_FIELD(consider_partitionwise_join);
	WRITE_BITMAPSET_FIELD(top_parent_relids);
	WRITE_BOOL_FIELD(partbounds_merged);
	WRITE_BITMAPSET_FIELD(all_partrels);
}

static void
_outIndexOptInfo(StringInfo str, const IndexOptInfo *node)
{
	WRITE_NODE_TYPE("INDEXOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(indexoid);
	/* Do NOT print rel field, else infinite recursion */
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples);
	WRITE_INT_FIELD(tree_height);
	WRITE_INT_FIELD(ncolumns);
	/* array fields aren't really worth the trouble to print */
	WRITE_OID_FIELD(relam);
	/* indexprs is redundant since we print indextlist */
	WRITE_NODE_FIELD(indpred);
	WRITE_NODE_FIELD(indextlist);
	WRITE_NODE_FIELD(indrestrictinfo);
	WRITE_BOOL_FIELD(predOK);
	WRITE_BOOL_FIELD(unique);
	WRITE_BOOL_FIELD(immediate);
	WRITE_BOOL_FIELD(hypothetical);
	/* we don't bother with fields copied from the index AM's API struct */
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* for compactness, just print the number of matches per column: */
	appendStringInfoString(str, " :eclass");
	for (int i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (int i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outStatisticExtInfo(StringInfo str, const StatisticExtInfo *node)
{
	WRITE_NODE_TYPE("STATISTICEXTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(statOid);
	/* don't write rel, leads to infinite recursion in plan tree dump */
	WRITE_CHAR_FIELD(kind);
	WRITE_BITMAPSET_FIELD(keys);
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * To simplify reading, we just chase up to the topmost merged EC and
	 * print that, without bothering to show the merge-ees separately.
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outEquivalenceMember(StringInfo str, const EquivalenceMember *node)
{
	WRITE_NODE_TYPE("EQUIVALENCEMEMBER");

	WRITE_NODE_FIELD(em_expr);
	WRITE_BITMAPSET_FIELD(em_relids);
	WRITE_BOOL_FIELD(em_is_const);
	WRITE_BOOL_FIELD(em_is_child);
	WRITE_OID_FIELD(em_datatype);
	WRITE_NODE_FIELD(em_jdomain);
}

static void
_outPathKey(StringInfo str, const PathKey *node)
{
	WRITE_NODE_TYPE("PATHKEY");

	WRITE_NODE_FIELD(pk_eclass);
	WRITE_OID_FIELD(pk_opfamily);
	WRITE_INT_FIELD(pk_strategy);
	WRITE_BOOL_FIELD(pk_nulls_first);
}

static void
_outPathTarget(StringInfo str, const PathTarget *node)
{
	WRITE_NODE_TYPE("PATHTARGET");

	WRITE_NODE_FIELD(exprs);
	if (node->sortgrouprefs)
	{
		int			i;

		appendStringInfoString(str, " :sortgrouprefs");
		for (i = 0; i < list_length(node->exprs); i++)
			appendStringInfo(str, " %u", node->sortgrouprefs[i]);
	}
	WRITE_FLOAT_FIELD(cost.startup);
	WRITE_FLOAT_FIELD(cost.per_tuple);
	WRITE_INT_FIELD(width);
	WRITE_ENUM_FIELD(has_volatile_expr, VolatileFunctionStatus);
}

static void
_outParamPathInfo(StringInfo str, const ParamPathInfo *node)
{
	WRITE_NODE_TYPE("PARAMPATHINFO");

	WRITE_BITMAPSET_FIELD(ppi_req_outer);
	WRITE_FLOAT_FIELD(ppi_rows);
	WRITE_NODE_FIELD(ppi_clauses);
}
#endif /* COMPILING_BINARY_FUNCS */

static void
_outRestrictInfo(StringInfo str, const RestrictInfo *node)
{
	WRITE_NODE_TYPE("RESTRICTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(clause);
	WRITE_BOOL_FIELD(is_pushed_down);
	WRITE_BOOL_FIELD(can_join);
	WRITE_BOOL_FIELD(pseudoconstant);
	WRITE_BOOL_FIELD(has_clone);
	WRITE_BOOL_FIELD(is_clone);
	WRITE_BOOL_FIELD(leakproof);
	WRITE_ENUM_FIELD(has_volatile, VolatileFunctionStatus);
	WRITE_UINT_FIELD(security_level);
	WRITE_INT_FIELD(num_base_rels);
	WRITE_BOOL_FIELD(contain_outer_query_references);
	WRITE_BITMAPSET_FIELD(clause_relids);
	WRITE_BITMAPSET_FIELD(required_relids);
	WRITE_BITMAPSET_FIELD(incompatible_relids);
	WRITE_BITMAPSET_FIELD(outer_relids);
	WRITE_BITMAPSET_FIELD(left_relids);
	WRITE_BITMAPSET_FIELD(right_relids);
	WRITE_NODE_FIELD(orclause);
	/* don't write parent_ec, leads to infinite recursion in plan tree dump */
	WRITE_INT_FIELD(rinfo_serial);
	WRITE_FLOAT_FIELD(eval_cost.startup);
	WRITE_FLOAT_FIELD(eval_cost.per_tuple);
	WRITE_FLOAT_FIELD(norm_selec);
	WRITE_FLOAT_FIELD(outer_selec);
	WRITE_NODE_FIELD(mergeopfamilies);
	/* don't write left_ec, leads to infinite recursion in plan tree dump */
	/* don't write right_ec, leads to infinite recursion in plan tree dump */
	WRITE_NODE_FIELD(left_em);
	WRITE_NODE_FIELD(right_em);
	WRITE_BOOL_FIELD(outer_is_left);
	WRITE_OID_FIELD(hashjoinoperator);
	WRITE_FLOAT_FIELD(left_bucketsize);
	WRITE_FLOAT_FIELD(left_mcvfreq);
	WRITE_FLOAT_FIELD(right_mcvfreq);
	WRITE_OID_FIELD(left_hasheqoperator);
	WRITE_OID_FIELD(right_hasheqoperator);
	WRITE_OID_FIELD(hasheqoperator);
}

static void
_outIndexClause(StringInfo str, const IndexClause *node)
{
	WRITE_NODE_TYPE("INDEXCLAUSE");

	WRITE_NODE_FIELD(rinfo);
	WRITE_NODE_FIELD(indexquals);
	WRITE_BOOL_FIELD(lossy);
	WRITE_INT_FIELD(indexcol);
	WRITE_NODE_FIELD(indexcols);
}

static void
_outSpecialJoinInfo(StringInfo str, const SpecialJoinInfo *node)
{
	WRITE_NODE_TYPE("SPECIALJOININFO");

	WRITE_BITMAPSET_FIELD(min_lefthand);
	WRITE_BITMAPSET_FIELD(min_righthand);
	WRITE_BITMAPSET_FIELD(syn_lefthand);
	WRITE_BITMAPSET_FIELD(syn_righthand);
	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_UINT_FIELD(ojrelid);
	WRITE_BITMAPSET_FIELD(commute_above_l);
	WRITE_BITMAPSET_FIELD(commute_above_r);
	WRITE_BITMAPSET_FIELD(commute_below_l);
	WRITE_BITMAPSET_FIELD(commute_below_r);
	WRITE_BOOL_FIELD(lhs_strict);
	WRITE_BOOL_FIELD(semi_can_btree);
	WRITE_BOOL_FIELD(semi_can_hash);
	WRITE_NODE_FIELD(semi_operators);
	WRITE_NODE_FIELD(semi_rhs_exprs);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outRowIdentityVarInfo(StringInfo str, const RowIdentityVarInfo *node)
{
	WRITE_NODE_TYPE("ROWIDENTITYVARINFO");

	WRITE_NODE_FIELD(rowidvar);
	WRITE_INT_FIELD(rowidwidth);
	WRITE_STRING_FIELD(rowidname);
	WRITE_BITMAPSET_FIELD(rowidrels);
}
#endif

static void
_outPlaceHolderInfo(StringInfo str, const PlaceHolderInfo *node)
{
	WRITE_NODE_TYPE("PLACEHOLDERINFO");

	WRITE_UINT_FIELD(phid);
	WRITE_NODE_FIELD(ph_var);
	WRITE_BITMAPSET_FIELD(ph_eval_at);
	WRITE_BITMAPSET_FIELD(ph_lateral);
	WRITE_BITMAPSET_FIELD(ph_needed);
	WRITE_INT_FIELD(ph_width);
}

static void
_outMinMaxAggInfo(StringInfo str, const MinMaxAggInfo *node)
{
	WRITE_NODE_TYPE("MINMAXAGGINFO");

	WRITE_OID_FIELD(aggfnoid);
	WRITE_OID_FIELD(aggsortop);
	WRITE_NODE_FIELD(target);
	/* We intentionally omit subroot --- too large, not interesting enough */
	WRITE_NODE_FIELD(path);
	WRITE_FLOAT_FIELD(pathcost);
	WRITE_NODE_FIELD(param);
}

static void
_outPlannerParamItem(StringInfo str, const PlannerParamItem *node)
{
	WRITE_NODE_TYPE("PLANNERPARAMITEM");

	WRITE_NODE_FIELD(item);
	WRITE_INT_FIELD(paramId);
}

/*****************************************************************************
 *
 *	Stuff from extensible.h
 *
 *****************************************************************************/

#ifndef COMPILING_BINARY_FUNCS
static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(str, node);
}
#endif /* COMPILING_BINARY_FUNCS */

/*****************************************************************************
 *
 *	Stuff from parsenodes.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from CreateStmt
 */
static void
_outCreateStmtInfo(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(tableElts);
	WRITE_NODE_FIELD(inhRelations);
	WRITE_NODE_FIELD(partspec);
	WRITE_NODE_FIELD(partbound);
	WRITE_NODE_FIELD(ofTypename);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(options);
	WRITE_ENUM_FIELD(oncommit, OnCommitAction);
	WRITE_STRING_FIELD(tablespacename);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_BOOL_FIELD(if_not_exists);
	WRITE_ENUM_FIELD(origin, CreateStmtOrigin);

	WRITE_NODE_FIELD(distributedBy);
	WRITE_NODE_FIELD(partitionBy);
	WRITE_CHAR_FIELD(relKind);
	WRITE_OID_FIELD(ownerid);
	WRITE_BOOL_FIELD(buildAoBlkdir);
	WRITE_NODE_FIELD(attr_encodings);
	WRITE_BOOL_FIELD(isCtas);
	WRITE_NODE_FIELD(intoQuery);
	WRITE_NODE_FIELD(intoPolicy);

	WRITE_NODE_FIELD(part_idx_oids);
	WRITE_NODE_FIELD(part_idx_names);
	WRITE_NODE_FIELD(tags);

	/*
	 * Some extra checks to make sure we didn't get lost
	 * during serialization/deserialization
	 */
	Assert(node->relKind != 0);
	Assert(node->oncommit <= ONCOMMIT_DROP);
}

static void
_outCreateStmt(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_TYPE("CREATESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);
}

static void
_outCreateForeignTableStmt(StringInfo str, const CreateForeignTableStmt *node)
{
	WRITE_NODE_TYPE("CREATEFOREIGNTABLESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);

	WRITE_STRING_FIELD(servername);
	WRITE_NODE_FIELD(options);
	WRITE_NODE_FIELD(distributedBy);
}

/*
 * SelectStmt's are never written to the catalog, they only exist
 * between parse and parseTransform.  The only use of this function
 * is for debugging purposes.
 *
 * In GPDB, these are also dispatched from QD to QEs, so we need full
 * out/read support.
 *
 * If the Nodes Struct changed, we need to maintain these funtions.
 */
static void
_outSelectStmt(StringInfo str, const SelectStmt *node)
{
	WRITE_NODE_TYPE("SELECT");

	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(intoClause);
	WRITE_NODE_FIELD(targetList);
	WRITE_NODE_FIELD(fromClause);
	WRITE_NODE_FIELD(whereClause);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(havingClause);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(valuesLists);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(scatterClause);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(lockingClause);
	WRITE_NODE_FIELD(withClause);
	WRITE_ENUM_FIELD(op, SetOperation);
	WRITE_BOOL_FIELD(all);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
	WRITE_BOOL_FIELD(disableLockingOptimization);
}

#ifndef COMPILING_BINARY_FUNCS

static void
_outPLAssignStmt(StringInfo str, const PLAssignStmt *node)
{
	WRITE_NODE_TYPE("PLASSIGN");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(indirection);
	WRITE_INT_FIELD(nnames);
	WRITE_NODE_FIELD(val);
	WRITE_LOCATION_FIELD(location);
}
#endif

static void
_outFuncCall(StringInfo str, const FuncCall *node)
{
	WRITE_NODE_TYPE("FUNCCALL");

	WRITE_NODE_FIELD(funcname);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(agg_order);
	WRITE_NODE_FIELD(agg_filter);
	WRITE_NODE_FIELD(over);
	WRITE_BOOL_FIELD(agg_within_group);
	WRITE_BOOL_FIELD(agg_star);
	WRITE_BOOL_FIELD(agg_distinct);
	WRITE_BOOL_FIELD(func_variadic);
	WRITE_ENUM_FIELD(funcformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outDefElem(StringInfo str, const DefElem *node)
{
	WRITE_NODE_TYPE("DEFELEM");

	WRITE_STRING_FIELD(defnamespace);
	WRITE_STRING_FIELD(defname);
	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(defaction, DefElemAction);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTableLikeClause(StringInfo str, const TableLikeClause *node)
{
	WRITE_NODE_TYPE("TABLELIKECLAUSE");

	WRITE_NODE_FIELD(relation);
	WRITE_UINT_FIELD(options);
	WRITE_OID_FIELD(relationOid);
}

static void
_outLockingClause(StringInfo str, const LockingClause *node)
{
	WRITE_NODE_TYPE("LOCKINGCLAUSE");

	WRITE_NODE_FIELD(lockedRels);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
}

static void
_outXmlSerialize(StringInfo str, const XmlSerialize *node)
{
	WRITE_NODE_TYPE("XMLSERIALIZE");

	WRITE_ENUM_FIELD(xmloption, XmlOptionType);
	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTriggerTransition(StringInfo str, const TriggerTransition *node)
{
	WRITE_NODE_TYPE("TRIGGERTRANSITION");

	WRITE_STRING_FIELD(name);
	WRITE_BOOL_FIELD(isNew);
	WRITE_BOOL_FIELD(isTable);
}

static void
_outColumnDef(StringInfo str, const ColumnDef *node)
{
	WRITE_NODE_TYPE("COLUMNDEF");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_STRING_FIELD(compression);
	WRITE_INT_FIELD(inhcount);
	WRITE_BOOL_FIELD(is_local);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_BOOL_FIELD(is_from_type);
	WRITE_INT_FIELD(attnum);
	WRITE_INT_FIELD(storage);
	WRITE_STRING_FIELD(storage_name);
	WRITE_NODE_FIELD(raw_default);
	WRITE_NODE_FIELD(cooked_default);

	WRITE_BOOL_FIELD(hasCookedMissingVal);
	WRITE_BOOL_FIELD(missingIsNull);
	if (node->hasCookedMissingVal && !node->missingIsNull)
		outDatum(str, node->missingVal, -1, false);

	WRITE_CHAR_FIELD(identity);
	WRITE_NODE_FIELD(identitySequence);
	WRITE_CHAR_FIELD(generated);
	WRITE_NODE_FIELD(collClause);
	WRITE_OID_FIELD(collOid);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(encoding);
	WRITE_NODE_FIELD(fdwoptions);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeName(StringInfo str, const TypeName *node)
{
	WRITE_NODE_TYPE("TYPENAME");

	WRITE_NODE_FIELD(names);
	WRITE_OID_FIELD(typeOid);
	WRITE_BOOL_FIELD(setof);
	WRITE_BOOL_FIELD(pct_type);
	WRITE_NODE_FIELD(typmods);
	WRITE_INT_FIELD(typemod);
	WRITE_NODE_FIELD(arrayBounds);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeCast(StringInfo str, const TypeCast *node)
{
	WRITE_NODE_TYPE("TYPECAST");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCollateClause(StringInfo str, const CollateClause *node)
{
	WRITE_NODE_TYPE("COLLATECLAUSE");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(collname);
	WRITE_LOCATION_FIELD(location);
}

static void
_outIndexElem(StringInfo str, const IndexElem *node)
{
	WRITE_NODE_TYPE("INDEXELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
	WRITE_STRING_FIELD(indexcolname);
	WRITE_NODE_FIELD(collation);
	WRITE_NODE_FIELD(opclass);
	WRITE_NODE_FIELD(opclassopts);
	WRITE_ENUM_FIELD(ordering, SortByDir);
	WRITE_ENUM_FIELD(nulls_ordering, SortByNulls);
}

static void
_outStatsElem(StringInfo str, const StatsElem *node)
{
	WRITE_NODE_TYPE("STATSELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outQuery(StringInfo str, const Query *node)
{
	WRITE_NODE_TYPE("QUERY");

	WRITE_ENUM_FIELD(commandType, CmdType);
	WRITE_ENUM_FIELD(querySource, QuerySource);
	/* we intentionally do not print the queryId field */
	WRITE_BOOL_FIELD(canSetTag);

	/*
	 * Hack to work around missing outfuncs routines for a lot of the
	 * utility-statement node types.  (The only one we actually *need* for
	 * rules support is NotifyStmt.)  Someday we ought to support 'em all, but
	 * for the meantime do this to avoid getting lots of warnings when running
	 * with debug_print_parse on.
	 */
	if (node->utilityStmt)
	{
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
			case T_CreateExternalStmt:
			case T_DropStmt:
			case T_TruncateStmt:
			case T_AlterTableStmt:
			case T_AlterTableCmd:
			case T_ViewStmt:
			case T_RuleStmt:

			case T_CreateRoleStmt:
			case T_AlterRoleStmt:
			case T_AlterRoleSetStmt:
			case T_DropRoleStmt:

			case T_CreateProfileStmt:
			case T_AlterProfileStmt:
			case T_DropProfileStmt:

			case T_CreateSchemaStmt:
			case T_AlterSchemaStmt:
			case T_CreatePLangStmt:
			case T_AlterOwnerStmt:
			case T_AlterObjectSchemaStmt:

			case T_CreateTableSpaceStmt:

			case T_RenameStmt:
			case T_IndexStmt:
			case T_NotifyStmt:
			case T_DeclareCursorStmt:
			case T_VacuumStmt:
			case T_CreateSeqStmt:
			case T_AlterSeqStmt:
			case T_CreatedbStmt:
			case T_AlterDatabaseSetStmt:
			case T_DropdbStmt:
			case T_CreateDomainStmt:
			case T_AlterDomainStmt:
			case T_ClusterStmt:

			case T_CreateFunctionStmt:
			case T_AlterFunctionStmt:

			case T_TransactionStmt:
			case T_GrantStmt:
			case T_GrantRoleStmt:
			case T_LockStmt:
			case T_CopyStmt:
			case T_ReindexStmt:
			case T_ConstraintsSetStmt:
			case T_VariableSetStmt:
			case T_CreateTrigStmt:
			case T_DefineStmt:
			case T_CompositeTypeStmt:
			case T_CreateCastStmt:
			case T_CreateOpClassStmt:
			case T_CreateOpClassItem:
			case T_CreateConversionStmt:
				WRITE_NODE_FIELD(utilityStmt);
				break;
			default:
				appendStringInfoString(str, " :utilityStmt ?");
				appendStringInfo(str, "%u", nodeTag(node->utilityStmt));
				break;
		}
	}
	else
		appendStringInfoString(str, " :utilityStmt <>");

	WRITE_INT_FIELD(resultRelation);
	WRITE_BOOL_FIELD(hasAggs);
	WRITE_BOOL_FIELD(hasWindowFuncs);
	WRITE_BOOL_FIELD(hasTargetSRFs);
	WRITE_BOOL_FIELD(hasSubLinks);
	WRITE_BOOL_FIELD(hasDynamicFunctions);
	WRITE_BOOL_FIELD(hasFuncsWithExecRestrictions);
	WRITE_BOOL_FIELD(hasDistinctOn);
	WRITE_BOOL_FIELD(hasRecursive);
	WRITE_BOOL_FIELD(hasModifyingCTE);
	WRITE_BOOL_FIELD(hasForUpdate);
	WRITE_BOOL_FIELD(hasRowSecurity);
	WRITE_BOOL_FIELD(canOptSelectLockingClause);
	WRITE_BOOL_FIELD(isReturn);
	WRITE_NODE_FIELD(cteList);
	WRITE_NODE_FIELD(rtable);
	WRITE_NODE_FIELD(rteperminfos);
	WRITE_NODE_FIELD(jointree);
	WRITE_NODE_FIELD(mergeActionList);
	WRITE_BOOL_FIELD(mergeUseOuterJoin);
	WRITE_NODE_FIELD(targetList);
	WRITE_ENUM_FIELD(override, OverridingKind);
	WRITE_NODE_FIELD(onConflict);
	WRITE_NODE_FIELD(returningList);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(groupingSets);
	WRITE_NODE_FIELD(havingQual);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(scatterClause);
	WRITE_BOOL_FIELD(isTableValueSelect);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(setOperations);
	WRITE_NODE_FIELD(constraintDeps);
	WRITE_NODE_FIELD(withCheckOptions);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);
	WRITE_BOOL_FIELD(parentStmtType);

	/* Don't serialize policy */
}
#endif /* COMPILING_BINARY_FUNCS */

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RTE");

	/* put alias + eref first to make dump more legible */
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);
	WRITE_BOOL_FIELD(relisivm);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_NODE_FIELD(tablesample);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNCTION:
			WRITE_NODE_FIELD(subquery);
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples);
			WRITE_OID_FIELD(relid);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_RESULT:
			/* no extra fields */
			break;
        case RTE_VOID:                                                  /*CDB*/
            break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inh);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_NODE_FIELD(securityQuals);

	WRITE_BOOL_FIELD(forceDistRandom);
}

static void
_outRangeTblFunction(StringInfo str, const RangeTblFunction *node)
{
	WRITE_NODE_TYPE("RANGETBLFUNCTION");

	WRITE_NODE_FIELD(funcexpr);
	WRITE_INT_FIELD(funccolcount);
	WRITE_NODE_FIELD(funccolnames);
	WRITE_NODE_FIELD(funccoltypes);
	WRITE_NODE_FIELD(funccoltypmods);
	WRITE_NODE_FIELD(funccolcollations);
	/* funcuserdata is only serialized in binary out/read functions */
#ifdef COMPILING_BINARY_FUNCS
	WRITE_BYTEA_FIELD(funcuserdata);
#endif
	WRITE_BITMAPSET_FIELD(funcparams);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("AEXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ANY ");
			break;
		case AEXPR_OP_ALL:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ALL ");
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		default:
			appendStringInfoString(str, " ??");
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

#endif /* COMPILING_BINARY_FUNCS */

/*
 * Node types found in raw parse trees (supported for debug purposes)
 */

#ifndef COMPILING_BINARY_FUNCS


static void
_outInteger(StringInfo str, const Integer *node)
{
	appendStringInfo(str, "%d", node->ival);
}

static void
_outFloat(StringInfo str, const Float *node)
{
	/*
	 * We assume the value is a valid numeric literal and so does not need
	 * quoting.
	 */
	appendStringInfoString(str, node->fval);
}

static void
_outBoolean(StringInfo str, const Boolean *node)
{
	appendStringInfoString(str, node->boolval ? "true" : "false");
}

static void
_outString(StringInfo str, const String *node)
{
	/*
	 * We use outToken to provide escaping of the string's content, but we
	 * don't want it to convert an empty string to '""', because we're putting
	 * double quotes around the string already.
	 */
	appendStringInfoChar(str, '"');
	if (node->sval[0] != '\0')
		outToken(str, node->sval);
	appendStringInfoChar(str, '"');
}

static void
_outBitString(StringInfo str, const BitString *node)
{
	/*
	 * The lexer will always produce a string starting with 'b' or 'x'.  There
	 * might be characters following that that need escaping, but outToken
	 * won't escape the 'b' or 'x'.  This is relied on by nodeTokenType.
	 */
	Assert(node->bsval[0] == 'b' || node->bsval[0] == 'x');
	outToken(str, node->bsval);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	if (node->isnull)
		appendStringInfoString(str, " NULL");
	else
	{
		appendStringInfoString(str, " :val ");
		outNode(str, &node->val);
	}
	WRITE_LOCATION_FIELD(location);
}
#endif /* COMPILING_BINARY_FUNCS */

#ifndef COMPILING_BINARY_FUNCS

static void
_outRangeSubselect(StringInfo str, const RangeSubselect *node)
{
	WRITE_NODE_TYPE("RANGESUBSELECT");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(subquery);
	WRITE_NODE_FIELD(alias);
}

static void
_outRangeFunction(StringInfo str, const RangeFunction *node)
{
	WRITE_NODE_TYPE("RANGEFUNCTION");

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(ordinality);
	WRITE_BOOL_FIELD(is_rowsfrom);
	WRITE_NODE_FIELD(functions);
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(coldeflist);
}

static void
_outRangeTableFunc(StringInfo str, const RangeTableFunc *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNC");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(docexpr);
	WRITE_NODE_FIELD(rowexpr);
	WRITE_NODE_FIELD(namespaces);
	WRITE_NODE_FIELD(columns);
	WRITE_NODE_FIELD(alias);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeTableFuncCol(StringInfo str, const RangeTableFuncCol *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNCCOL");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_BOOL_FIELD(for_ordinality);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_NODE_FIELD(colexpr);
	WRITE_NODE_FIELD(coldefexpr);
	WRITE_LOCATION_FIELD(location);
}
#endif /* COMPILING_BINARY_FUNCS */

static void
_outConstraint(StringInfo str, const Constraint *node)
{
	WRITE_NODE_TYPE("CONSTRAINT");

	WRITE_ENUM_FIELD(contype, ConstrType);
	/* name, or NULL if unnamed */
	WRITE_STRING_FIELD(conname);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_LOCATION_FIELD(location);

	WRITE_BOOL_FIELD(is_no_inherit);
	WRITE_NODE_FIELD(raw_expr);
	WRITE_STRING_FIELD(cooked_expr);
	WRITE_CHAR_FIELD(generated_when);
	WRITE_BOOL_FIELD(nulls_not_distinct);

	WRITE_NODE_FIELD(keys);
	WRITE_NODE_FIELD(including);

	WRITE_NODE_FIELD(exclusions);

	WRITE_NODE_FIELD(options);
	WRITE_STRING_FIELD(indexname);
	WRITE_STRING_FIELD(indexspace);
	WRITE_BOOL_FIELD(reset_default_tblspc);

	WRITE_STRING_FIELD(access_method);
	WRITE_NODE_FIELD(where_clause);

	WRITE_NODE_FIELD(pktable);
	WRITE_NODE_FIELD(fk_attrs);
	WRITE_NODE_FIELD(pk_attrs);
	WRITE_CHAR_FIELD(fk_matchtype);
	WRITE_CHAR_FIELD(fk_upd_action);
	WRITE_CHAR_FIELD(fk_del_action);
	WRITE_NODE_FIELD(old_conpfeqop);
	WRITE_OID_FIELD(old_pktable_oid);

	WRITE_BOOL_FIELD(skip_validation);
	WRITE_BOOL_FIELD(initially_valid);
}

#ifndef COMPILING_BINARY_FUNCS
static void
_outForeignKeyCacheInfo(StringInfo str, const ForeignKeyCacheInfo *node)
{
	WRITE_NODE_TYPE("FOREIGNKEYCACHEINFO");

	WRITE_OID_FIELD(conoid);
	WRITE_OID_FIELD(conrelid);
	WRITE_OID_FIELD(confrelid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
}
#endif /* COMPILING_BINARY_FUNCS */

static void
_outCreateDirectoryTableStmt(StringInfo str, const CreateDirectoryTableStmt *node)
{
	WRITE_NODE_TYPE("CREATEDIRECTORYTABLESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);
	WRITE_STRING_FIELD(tablespacename);
	WRITE_STRING_FIELD(location);
}

#include "outfuncs_common.c"
#ifndef COMPILING_BINARY_FUNCS
/*
 * outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	if (obj == NULL)
		appendStringInfoString(str, "<>");
	else if (IsA(obj, List) || IsA(obj, IntList) || IsA(obj, OidList) ||
			 IsA(obj, XidList))
		_outList(str, obj);
	/* nodeRead does not want to see { } around these! */
	else if (IsA(obj, Integer))
		_outInteger(str, (Integer *) obj);
	else if (IsA(obj, Float))
		_outFloat(str, (Float *) obj);
	else if (IsA(obj, Boolean))
		_outBoolean(str, (Boolean *) obj);
	else if (IsA(obj, String))
		_outString(str, (String *) obj);
	else if (IsA(obj, BitString))
		_outBitString(str, (BitString *) obj);
	else if (IsA(obj, Bitmapset))
		outBitmapset(str, (Bitmapset *) obj);
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
#include "outfuncs.switch.c"

			case T_QueryDispatchDesc:
				_outQueryDispatchDesc(str, obj);
				break;
			case T_SerializedParams:
				_outSerializedParams(str, obj);
				break;
			case T_OidAssignment:
				_outOidAssignment(str, obj);
				break;
			case T_Plan:
				_outPlan(str, obj);
				break;
			case T_Scan:
				_outScan(str, obj);
				break;
			case T_Join:
				_outJoin(str, obj);
				break;
		    case T_DQAExpr:
                _outDQAExpr(str, obj);
                break;
			case T_Path:
				_outPath(str, obj);
				break;
			case T_IndexPath:
				_outIndexPath(str, obj);
				break;
			case T_BitmapHeapPath:
				_outBitmapHeapPath(str, obj);
				break;
			case T_BitmapAndPath:
				_outBitmapAndPath(str, obj);
				break;
			case T_BitmapOrPath:
				_outBitmapOrPath(str, obj);
				break;
			case T_TidPath:
				_outTidPath(str, obj);
				break;
			case T_TidRangePath:
				_outTidRangePath(str, obj);
				break;
			case T_SubqueryScanPath:
				_outSubqueryScanPath(str, obj);
				break;
			case T_TableFunctionScanPath:
				_outTableFunctionScanPath(str, obj);
				break;
			case T_ForeignPath:
				_outForeignPath(str, obj);
				break;
			case T_CustomPath:
				_outCustomPath(str, obj);
				break;
			case T_AppendPath:
				_outAppendPath(str, obj);
				break;
			case T_MergeAppendPath:
				_outMergeAppendPath(str, obj);
				break;
			case T_AppendOnlyPath:
				_outAppendOnlyPath(str, obj);
				break;
			case T_AOCSPath:
				_outAOCSPath(str, obj);
				break;
			case T_GroupResultPath:
				_outGroupResultPath(str, obj);
				break;
			case T_MaterialPath:
				_outMaterialPath(str, obj);
				break;
			case T_MemoizePath:
				_outMemoizePath(str, obj);
				break;
			case T_UniquePath:
				_outUniquePath(str, obj);
				break;
			case T_GatherPath:
				_outGatherPath(str, obj);
				break;
			case T_ProjectionPath:
				_outProjectionPath(str, obj);
				break;
			case T_ProjectSetPath:
				_outProjectSetPath(str, obj);
				break;
			case T_SortPath:
				_outSortPath(str, obj);
				break;
			case T_IncrementalSortPath:
				_outIncrementalSortPath(str, obj);
				break;
			case T_GroupPath:
				_outGroupPath(str, obj);
				break;
			case T_UpperUniquePath:
				_outUpperUniquePath(str, obj);
				break;
			case T_AggPath:
				_outAggPath(str, obj);
				break;
			case T_GroupingSetsPath:
				_outGroupingSetsPath(str, obj);
				break;
			case T_MinMaxAggPath:
				_outMinMaxAggPath(str, obj);
				break;
			case T_WindowAggPath:
				_outWindowAggPath(str, obj);
				break;
			case T_SetOpPath:
				_outSetOpPath(str, obj);
				break;
			case T_RecursiveUnionPath:
				_outRecursiveUnionPath(str, obj);
				break;
			case T_LockRowsPath:
				_outLockRowsPath(str, obj);
				break;
			case T_ModifyTablePath:
				_outModifyTablePath(str, obj);
				break;
			case T_RuntimeFilterPath:
				_outRuntimeFilterPath(str, obj);
				break;
			case T_LimitPath:
				_outLimitPath(str, obj);
				break;
			case T_GatherMergePath:
				_outGatherMergePath(str, obj);
				break;
			case T_NestPath:
				_outNestPath(str, obj);
				break;
			case T_MergePath:
				_outMergePath(str, obj);
				break;
			case T_HashPath:
				_outHashPath(str, obj);
				break;
            case T_CdbMotionPath:
                _outCdbMotionPath(str, obj);
                break;
			case T_PlannerGlobal:
				_outPlannerGlobal(str, obj);
				break;
			case T_PlannerInfo:
				_outPlannerInfo(str, obj);
				break;
			case T_RelOptInfo:
				_outRelOptInfo(str, obj);
				break;
			case T_IndexOptInfo:
				_outIndexOptInfo(str, obj);
				break;
			case T_ForeignKeyOptInfo:
				_outForeignKeyOptInfo(str, obj);
				break;
			case T_EquivalenceClass:
				_outEquivalenceClass(str, obj);
				break;
			case T_EquivalenceMember:
				_outEquivalenceMember(str, obj);
				break;
			case T_PathKey:
				_outPathKey(str, obj);
				break;
			case T_PathTarget:
				_outPathTarget(str, obj);
				break;
			case T_ParamPathInfo:
				_outParamPathInfo(str, obj);
				break;
			case T_RestrictInfo:
				_outRestrictInfo(str, obj);
				break;
			case T_IndexClause:
				_outIndexClause(str, obj);
				break;
			case T_SpecialJoinInfo:
				_outSpecialJoinInfo(str, obj);
				break;
			case T_RowIdentityVarInfo:
				_outRowIdentityVarInfo(str, obj);
				break;
			case T_PlaceHolderInfo:
				_outPlaceHolderInfo(str, obj);
				break;
			case T_MinMaxAggInfo:
				_outMinMaxAggInfo(str, obj);
				break;
			case T_PlannerParamItem:
				_outPlannerParamItem(str, obj);
				break;
			case T_RollupData:
				_outRollupData(str, obj);
				break;
			case T_GroupingSetData:
				_outGroupingSetData(str, obj);
				break;
			case T_StatisticExtInfo:
				_outStatisticExtInfo(str, obj);
				break;

			case T_AlteredTableInfo:
				_outAlteredTableInfo(str, obj);
				break;
			case T_NewConstraint:
				_outNewConstraint(str, obj);
				break;
			case T_NewColumnValue:
				_outNewColumnValue(str, obj);
				break;

			case T_Null:
				_outNull(str, obj);
				break;
			case T_ForeignKeyCacheInfo:
				_outForeignKeyCacheInfo(str, obj);
				break;
			case T_CdbProcess:
				_outCdbProcess(str, obj);
				break;
			case T_SliceTable:
				_outSliceTable(str, obj);
				break;
			case T_CursorPosInfo:
				_outCursorPosInfo(str, obj);
				break;
			case T_TupleDescNode:
				_outTupleDescNode(str, obj);
				break;

			case T_EphemeralNamedRelationInfo:
				_outEphemeralNamedRelationInfo(str, obj);
				break;
			default:

				/*
				 * This should be an ERROR, but it's too useful to be able to
				 * dump structures that outNode only understands part of.
				 */
				elog(WARNING, "could not dump unrecognized node type: %d",
					 (int) nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}

/*
 * nodeToString -
 *	   returns the ascii representation of the Node as a palloc'd string
 */
char *
nodeToString(const void *obj)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outNode(&str, obj);
	return str.data;
}

/*
 * bmsToString -
 *	   returns the ascii representation of the Bitmapset as a palloc'd string
 */
char *
bmsToString(const Bitmapset *bms)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outBitmapset(&str, bms);
	return str.data;
}
#endif /* COMPILING_BINARY_FUNCS */
