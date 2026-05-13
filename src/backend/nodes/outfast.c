/*-------------------------------------------------------------------------
 *
 * outfast.c
 *	  Fast serialization functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * NOTES
 *	  Every node type that can appear in an Greenplum Database serialized query or plan
 *    tree must have an output function defined here.
 *
 * 	  There *MUST* be a one-to-one correspondence between this routine
 *    and readfast.c.  If not, you will likely crash the system.
 *
 *     By design, the only user of these routines is the function
 *     serializeNode in cdbsrlz.c.  Other callers beware.
 *
 *    Like readfast.c, this file borrows the definitions of most functions
 *    from outfuncs.c.
 *
 * 	  Rather than serialize to a (somewhat human-readable) string, these
 *    routines create a binary serialization via a simple depth-first walk
 *    of the tree.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "lib/stringinfo.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "cdb/cdbgang.h"
#include "utils/workfile_mgr.h"
#include "parser/parsetree.h"

/*
 * Macros to simplify output of different kinds of fields.	Use these
 * wherever possible to reduce the chance for silly typos.	Note that these
 * hard-wire conventions about the names of the local variables in an Out
 * routine.
 */

/*
 * Write the label for the node type.  nodelabel is accepted for
 * compatibility with outfuncs.c, but is ignored
 */
#define WRITE_NODE_TYPE(nodelabel) \
	{ int16 nt =nodeTag(node); appendBinaryStringInfo(str, (const char *)&nt, sizeof(int16)); }

/* Write an integer field  */
#define WRITE_INT_FIELD(fldname) \
	{ appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int)); }

/* Write an integer field  */
#define WRITE_INT8_FIELD(fldname) \
	{ appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int8)); }

/* Write an integer field  */
#define WRITE_INT16_FIELD(fldname) \
	{ appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int16)); }

/* Write an unsigned integer field */
#define WRITE_UINT_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int))

/* Write an uint64 field */
#define WRITE_UINT64_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(uint64))

/* Write a 64-bit integer field */
#define WRITE_INT64_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int64))

/* Write an OID field (don't hard-wire assumption that OID is same as uint) */
#define WRITE_OID_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(Oid))

/* Write a long-integer field */
#define WRITE_LONG_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(long))

/* Write a char field (ie, one ascii character) */
#define WRITE_CHAR_FIELD(fldname) \
	appendBinaryStringInfo(str, &node->fldname, 1)

/* Write an enumerated-type field as an integer code */
#define WRITE_ENUM_FIELD(fldname, enumtype) \
	{ int16 en=node->fldname; appendBinaryStringInfo(str, (const char *)&en, sizeof(int16)); }

/* Write a float field --- the format is accepted but ignored (for compat with outfuncs.c)  */
#define WRITE_FLOAT_FIELD(fldname) \
	appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(double))

/* Write a boolean field */
#define WRITE_BOOL_FIELD(fldname) \
	{ \
		char b = node->fldname ? 1 : 0; \
		appendBinaryStringInfo(str, (const char *)&b, 1); }

/* Write a character-string (possibly NULL) varable */
#define WRITE_STRING_VAR(var) \
	{ int slen = var != NULL ? strlen(var) : 0; \
		appendBinaryStringInfo(str, (const char *)&slen, sizeof(int)); \
		if (slen>0) appendBinaryStringInfo(str, var, slen);}

/* Write a character-string (possibly NULL) field */
#define WRITE_STRING_FIELD(fldname)  WRITE_STRING_VAR(node->fldname)

/* Write a parse location field (actually same as INT case) */
#define WRITE_LOCATION_FIELD(fldname) \
	{ appendBinaryStringInfo(str, (const char *)&node->fldname, sizeof(int)); }

/*
 * Write a Node field
 *
 * If compiled with GP_SERIALIZATION_DEBUG, write the field name in the
 * serialized form, and check that it matches in the read function
 * (READ_NODE_FIELD in readfast.c). That makes it much easier to debug bugs
 * where the out and read functions are not in sync, as you get an error
 * much earlier, and it can print the field name where the mismatch occurred.
 * It makes the serialized plans much larger, though, so we don't want to do
 * it production.
 */
#ifdef GP_SERIALIZATION_DEBUG
#define WRITE_NODE_FIELD(fldname) \
	do { \
		const char *xx = CppAsString(fldname); \
		appendBinaryStringInfo(str, xx, strlen(xx) + 1); \
		(_outNode(str, node->fldname)); \
	} while (0)
#else
#define WRITE_NODE_FIELD(fldname) \
	(_outNode(str, node->fldname))
#endif


/* Write a bitmapset field */
#define WRITE_BITMAPSET_FIELD(fldname) \
	 _outBitmapset(str, node->fldname)

/* Write a binary field */
#define WRITE_BINARY_FIELD(fldname, sz) \
{ appendBinaryStringInfo(str, (const char *) &node->fldname, (sz)); }

/* Write a bytea field */
#define WRITE_BYTEA_FIELD(fldname) \
	(_outDatum(str, PointerGetDatum(node->fldname), -1, false))

/* Write a dummy field -- value not displayable or copyable */
#define WRITE_DUMMY_FIELD(fldname) \
	{ /*int * dummy = 0; appendBinaryStringInfo(str,(const char *)&dummy, sizeof(int *)) ;*/ }

	/* Read an integer array */
#define WRITE_INT_ARRAY(fldname, count) \
	StaticAssertStmt(sizeof(node->fldname[0]) == sizeof(int32), \
					 "WRITE_INT_ARRAY() used on wrong array type"); \
	if ( (count) > 0 ) \
	{ \
		int i; \
		for(i = 0; i < (count); i++) \
		{ \
			appendBinaryStringInfo(str, (const char *)&node->fldname[i], sizeof(int32)); \
		} \
	}

/* Write a boolean array  */
#define WRITE_BOOL_ARRAY(fldname, count) \
	if ( (count) > 0 ) \
	{ \
		int i; \
		for(i = 0; i < (count); i++) \
		{ \
			char b = node->fldname[i] ? 1 : 0;								\
			appendBinaryStringInfo(str, (const char *)&b, 1); \
		} \
	}

/* Write an Trasnaction ID array  */
#define WRITE_XID_ARRAY(fldname, count) \
	if ( (count) > 0 ) \
	{ \
		int i; \
		for(i = 0; i < (count); i++) \
		{ \
			appendBinaryStringInfo(str, (const char *)&node->fldname[i], sizeof(TransactionId)); \
		} \
	}

/* Write an AttrNumber array  */
#define WRITE_ATTRNUMBER_ARRAY(fldname, count) \
	if ( (count) > 0 ) \
	{ \
		int i; \
		for(i = 0; i < (count); i++) \
		{ \
			appendBinaryStringInfo(str, (const char *)&node->fldname[i], sizeof(AttrNumber)); \
		} \
	}

/* Write an Oid array  */
#define WRITE_OID_ARRAY(fldname, count) \
	if ( (count) > 0 ) \
	{ \
		int i; \
		for(i = 0; i < (count); i++) \
		{ \
			appendBinaryStringInfo(str, (const char *)&node->fldname[i], sizeof(Oid)); \
		} \
	}

static void _outNode(StringInfo str, void *obj);

#define outDatum(str, value, typlen, typbyval) _outDatum(str, value, typlen, typbyval)

static void
_outList(StringInfo str, List *node)
{
	ListCell   *lc;

	if (node == NULL)
	{
		int16 tg = 0;
		appendBinaryStringInfo(str, (const char *)&tg, sizeof(int16));
		return;
	}

	WRITE_NODE_TYPE("");
    WRITE_INT_FIELD(length);

	foreach(lc, node)
	{

		if (IsA(node, List))
		{
			_outNode(str, lfirst(lc));
		}
		else if (IsA(node, IntList))
		{
			int n = lfirst_int(lc);
			appendBinaryStringInfo(str, (const char *)&n, sizeof(int));
		}
		else if (IsA(node, OidList))
		{
			Oid n = lfirst_oid(lc);
			appendBinaryStringInfo(str, (const char *)&n, sizeof(Oid));
		}
	}
}

/*
 * _outBitmapset -
 *	   converts a bitmap set of integers
 *
 * Currently bitmapsets do not appear in any node type that is stored in
 * rules, so there is no support in readfast.c for reading this format.
 */
static void
_outBitmapset(StringInfo str, const Bitmapset *bms)
{
	int			i;
	int			nwords = 0;

	if (bms)
		nwords = bms->nwords;

	appendBinaryStringInfo(str, (char *) &nwords, sizeof(int));
	for (i = 0; i < nwords; i++)
	{
		appendBinaryStringInfo(str, (char *) &bms->words[i], sizeof(bitmapword));
	}

}

/*
 * Print the value of a Datum given its type.
 */
static void
_outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length;
	char	   *s;

	if (typbyval)
	{
		s = (char *) (&value);
		appendBinaryStringInfo(str, s, sizeof(Datum));
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
		{
			length = 0;
			appendBinaryStringInfo(str, (char *)&length, sizeof(Size));
		}
		else
		{
			if (typlen == -1 && VARATT_IS_EXTERNAL_EXPANDED(s))
			{
				ExpandedObjectHeader *eoh = DatumGetEOHP(value);
				Size		resultsize;
				char		*resultptr;

				resultsize = EOH_get_flat_size(eoh);
				resultptr = (char *) palloc(resultsize);
				EOH_flatten_into(eoh, (void *) resultptr, resultsize);
				appendBinaryStringInfo(str, (char *)&resultsize, sizeof(Size));
				appendBinaryStringInfo(str, resultptr, resultsize);
			}
			else
			{
				length = datumGetSize(value, typbyval, typlen);
				appendBinaryStringInfo(str, (char *)&length, sizeof(Size));
				appendBinaryStringInfo(str, s, length);
			}
		}
	}
}

#define COMPILING_BINARY_FUNCS
#include "outfuncs.c"

/* generated binary out functions */
#include "outfuncs.funcs.c"

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

static void
_outConst(StringInfo str, Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	if (!node->constisnull)
		_outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outBoolExpr(StringInfo str, BoolExpr *node)
{
	WRITE_NODE_TYPE("BOOLEXPR");
	WRITE_ENUM_FIELD(boolop, BoolExprType);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

/*****************************************************************************
 *
 *	Stuff from extensible.h
 *
 *****************************************************************************/

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;
	StringInfoData buf;
	initStringInfo(&buf);

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(&buf, node);

	WRITE_STRING_VAR(buf.data);
}

/*****************************************************************************
 *
 *	Stuff from parsenodes.h.
 *
 *****************************************************************************/

static void
_outQuery(StringInfo str, Query *node)
{
	WRITE_NODE_TYPE("QUERY");

	WRITE_ENUM_FIELD(commandType, CmdType);
	WRITE_ENUM_FIELD(querySource, QuerySource);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_NODE_FIELD(utilityStmt);
	WRITE_INT_FIELD(resultRelation);
	WRITE_BOOL_FIELD(hasAggs);
	WRITE_BOOL_FIELD(hasWindowFuncs);
	WRITE_BOOL_FIELD(hasSubLinks);
	WRITE_BOOL_FIELD(hasDynamicFunctions);
	WRITE_BOOL_FIELD(hasFuncsWithExecRestrictions);
	WRITE_BOOL_FIELD(hasDistinctOn);
	WRITE_BOOL_FIELD(hasRecursive);
	WRITE_BOOL_FIELD(hasModifyingCTE);
	WRITE_BOOL_FIELD(hasForUpdate);
	WRITE_BOOL_FIELD(hasRowSecurity);
	WRITE_BOOL_FIELD(canOptSelectLockingClause);
	WRITE_NODE_FIELD(cteList);
	WRITE_NODE_FIELD(rtable);
	WRITE_NODE_FIELD(rteperminfos);
	WRITE_NODE_FIELD(jointree);
	WRITE_NODE_FIELD(mergeActionList);
	WRITE_BOOL_FIELD(mergeUseOuterJoin);
	WRITE_NODE_FIELD(targetList);
	WRITE_NODE_FIELD(withCheckOptions);
	WRITE_NODE_FIELD(onConflict);
	WRITE_NODE_FIELD(returningList);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(groupingSets);
	WRITE_NODE_FIELD(havingQual);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(scatterClause);
	WRITE_BOOL_FIELD(isTableValueSelect);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(setOperations);
	WRITE_NODE_FIELD(constraintDeps);
	WRITE_BOOL_FIELD(parentStmtType);

	/* Don't serialize policy */
}

static void
_outA_Expr(StringInfo str, A_Expr *node)
{
	WRITE_NODE_TYPE("AEXPR");
	WRITE_ENUM_FIELD(kind, A_Expr_Kind);

	switch (node->kind)
	{
		case AEXPR_OP:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:

			WRITE_NODE_FIELD(name);

			break;
		case AEXPR_OP_ALL:

			WRITE_NODE_FIELD(name);

			break;
		case AEXPR_DISTINCT:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:

			WRITE_NODE_FIELD(name);
			break;

		case AEXPR_IN:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:

			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:

			WRITE_NODE_FIELD(name);
			break;

		default:

			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outInteger(StringInfo str, const Integer *node)
{
	int16 vt = T_Integer;
	appendBinaryStringInfo(str, (const char *)&vt, sizeof(int16));
	appendBinaryStringInfo(str, (const char *)&node->ival, sizeof(int));
}


static void
_outFloat(StringInfo str, const Float *node)
{
	int16 vt = T_Float;
	int slen;

	appendBinaryStringInfo(str, (const char *) &vt, sizeof(int16));
	slen = (node->fval != NULL ? strlen(node->fval) : 0);
	appendBinaryStringInfo(str, (const char *)&slen, sizeof(int));
	if (slen > 0)
		appendBinaryStringInfo(str, node->fval, slen);
}

static void
_outBoolean(StringInfo str, const Boolean *node)
{
	int16 vt = T_Boolean;
	appendBinaryStringInfo(str, (const char *)&vt, sizeof(int16));
	appendBinaryStringInfo(str, (const char *)&node->boolval, sizeof(bool));
}


static void
_outString(StringInfo str, const String *node)
{
	int16 vt = T_String;
	int slen;

	appendBinaryStringInfo(str, (const char *) &vt, sizeof(int16));
	slen = (node->sval != NULL ? strlen(node->sval) : 0);
	appendBinaryStringInfo(str, (const char *)&slen, sizeof(int));
	if (slen > 0)
		appendBinaryStringInfo(str, node->sval, slen);
}

static void
_outBitString(StringInfo str, const BitString *node)
{
	int16 vt = T_BitString;
	int slen;

	appendBinaryStringInfo(str, (const char *) &vt, sizeof(int16));
	slen = (node->bsval != NULL ? strlen(node->bsval) : 0);
	appendBinaryStringInfo(str, (const char *)&slen, sizeof(int));
	if (slen > 0)
		appendBinaryStringInfo(str, node->bsval, slen);
}


static void
_outA_Const(StringInfo str, A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");
	WRITE_BOOL_FIELD(isnull);
	if (!node->isnull)
		_outNode(str, &node->val);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCookedConstraint(StringInfo str, CookedConstraint *node)
{
	WRITE_NODE_TYPE("COOKEDCONSTRAINT");

	WRITE_ENUM_FIELD(contype,ConstrType);
	WRITE_STRING_FIELD(name);
	WRITE_INT_FIELD(attnum);
	WRITE_NODE_FIELD(expr);
	WRITE_BOOL_FIELD(is_local);
	WRITE_INT_FIELD(inhcount);
	WRITE_BOOL_FIELD(is_no_inherit);
}

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
	WRITE_STRING_FIELD(methods->CustomName);	
}

/*
 * _outNode -
 *	  converts a Node into binary string and append it to 'str'
 */
static void
_outNode(StringInfo str, void *obj)
{
	if (obj == NULL)
	{
		int16 tg = 0;
		appendBinaryStringInfo(str, (const char *)&tg, sizeof(int16));
	}
	else if (IsA(obj, List) ||IsA(obj, IntList) || IsA(obj, OidList))
		_outList(str, obj);
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
		_outBitmapset(str, (Bitmapset *) obj);
	else
	{
		switch (nodeTag(obj))
		{
#include "outfast.switch.c"

			/* custom_read_write nodes with binary implementations */
			case T_Const:
				_outConst(str, obj);
				break;
			case T_BoolExpr:
				_outBoolExpr(str, obj);
				break;
			case T_Query:
				_outQuery(str, obj);
				break;
			case T_ExtensibleNode:
				_outExtensibleNode(str, obj);
				break;
			case T_CustomScan:
				_outCustomScan(str, obj);
				break;
			case T_A_Expr:
				_outA_Expr(str, obj);
				break;
			case T_A_Const:
				_outA_Const(str, obj);
				break;
			case T_InsertStmt:
				_outInsertStmt(str, obj);
				break;
			case T_CopyStmt:
				_outCopyStmt(str, obj);
				break;
			case T_CreateTrigStmt:
				_outCreateTrigStmt(str, obj);
				break;
			case T_AlterTSConfigurationStmt:
				_outAlterTSConfigurationStmt(str, obj);
				break;
			case T_SelectStmt:
				_outSelectStmt(str, obj);
				break;
			case T_CreateStmt:
				_outCreateStmt(str, obj);
				break;
			case T_CreateForeignTableStmt:
				_outCreateForeignTableStmt(str, obj);
				break;
			case T_CreateDirectoryTableStmt:
				_outCreateDirectoryTableStmt(str, obj);
				break;
			case T_Constraint:
				_outConstraint(str, obj);
				break;

			/* handwritten plan/scan/join nodes from outfuncs.c */
			case T_PlannedStmt:
				_outPlannedStmt(str, obj);
				break;
			case T_Result:
				_outResult(str, obj);
				break;
			case T_ProjectSet:
				_outProjectSet(str, obj);
				break;
			case T_ModifyTable:
				_outModifyTable(str, obj);
				break;
			case T_Append:
				_outAppend(str, obj);
				break;
			case T_MergeAppend:
				_outMergeAppend(str, obj);
				break;
			case T_RecursiveUnion:
				_outRecursiveUnion(str, obj);
				break;
			case T_BitmapAnd:
				_outBitmapAnd(str, obj);
				break;
			case T_BitmapOr:
				_outBitmapOr(str, obj);
				break;
			case T_Gather:
				_outGather(str, obj);
				break;
			case T_GatherMerge:
				_outGatherMerge(str, obj);
				break;
			case T_SeqScan:
				_outSeqScan(str, obj);
				break;
			case T_DynamicSeqScan:
				_outDynamicSeqScan(str, obj);
				break;
			case T_SampleScan:
				_outSampleScan(str, obj);
				break;
			case T_IndexScan:
				_outIndexScan(str, obj);
				break;
			case T_IndexOnlyScan:
				_outIndexOnlyScan(str, obj);
				break;
			case T_DynamicIndexScan:
				_outDynamicIndexScan(str, obj);
				break;
			case T_DynamicIndexOnlyScan:
				_outDynamicIndexOnlyScan(str, obj);
				break;
			case T_BitmapIndexScan:
				_outBitmapIndexScan(str, obj);
				break;
			case T_DynamicBitmapIndexScan:
				_outDynamicBitmapIndexScan(str, obj);
				break;
			case T_BitmapHeapScan:
				_outBitmapHeapScan(str, obj);
				break;
			case T_DynamicBitmapHeapScan:
				_outDynamicBitmapHeapScan(str, obj);
				break;
			case T_TidScan:
				_outTidScan(str, obj);
				break;
			case T_SubqueryScan:
				_outSubqueryScan(str, obj);
				break;
			case T_FunctionScan:
				_outFunctionScan(str, obj);
				break;
			case T_TableFuncScan:
				_outTableFuncScan(str, obj);
				break;
			case T_ValuesScan:
				_outValuesScan(str, obj);
				break;
			case T_CteScan:
				_outCteScan(str, obj);
				break;
			case T_NamedTuplestoreScan:
				_outNamedTuplestoreScan(str, obj);
				break;
			case T_WorkTableScan:
				_outWorkTableScan(str, obj);
				break;
			case T_ForeignScan:
				_outForeignScan(str, obj);
				break;
			case T_DynamicForeignScan:
				_outDynamicForeignScan(str, obj);
				break;
			case T_NestLoop:
				_outNestLoop(str, obj);
				break;
			case T_MergeJoin:
				_outMergeJoin(str, obj);
				break;
			case T_HashJoin:
				_outHashJoin(str, obj);
				break;
			case T_Agg:
				_outAgg(str, obj);
				break;
			case T_WindowAgg:
				_outWindowAgg(str, obj);
				break;
			case T_WindowHashAgg:
				_outWindowHashAgg(str, obj);
				break;
			case T_Group:
				_outGroup(str, obj);
				break;
			case T_Material:
				_outMaterial(str, obj);
				break;
			case T_Sort:
				_outSort(str, obj);
				break;
			case T_IncrementalSort:
				_outIncrementalSort(str, obj);
				break;
			case T_Unique:
				_outUnique(str, obj);
				break;
			case T_Hash:
				_outHash(str, obj);
				break;
			case T_SetOp:
				_outSetOp(str, obj);
				break;
			case T_LockRows:
				_outLockRows(str, obj);
				break;
			case T_RuntimeFilter:
				_outRuntimeFilter(str, obj);
				break;
			case T_Limit:
				_outLimit(str, obj);
				break;
			/* handwritten parse-tree nodes from outfuncs.c */
			case T_FuncCall:
				_outFuncCall(str, obj);
				break;
			case T_DefElem:
				_outDefElem(str, obj);
				break;
			case T_TableLikeClause:
				_outTableLikeClause(str, obj);
				break;
			case T_LockingClause:
				_outLockingClause(str, obj);
				break;
			case T_XmlSerialize:
				_outXmlSerialize(str, obj);
				break;
			case T_TriggerTransition:
				_outTriggerTransition(str, obj);
				break;
			case T_ColumnDef:
				_outColumnDef(str, obj);
				break;
			case T_TypeName:
				_outTypeName(str, obj);
				break;
			case T_TypeCast:
				_outTypeCast(str, obj);
				break;
			case T_CollateClause:
				_outCollateClause(str, obj);
				break;
			case T_IndexElem:
				_outIndexElem(str, obj);
				break;
			case T_StatsElem:
				_outStatsElem(str, obj);
				break;
			case T_RangeTblEntry:
				_outRangeTblEntry(str, obj);
				break;
			case T_RangeTblFunction:
				_outRangeTblFunction(str, obj);
				break;

			/* CBDB-specific plan nodes from outfuncs_common.c */
			case T_Sequence:
				_outSequence(str, obj);
				break;
			case T_TupleSplit:
				_outTupleSplit(str, obj);
				break;
			case T_TableFunctionScan:
				_outTableFunctionScan(str, obj);
				break;
			case T_ShareInputScan:
				_outShareInputScan(str, obj);
				break;
			case T_Motion:
				_outMotion(str, obj);
				break;
			case T_SplitUpdate:
				_outSplitUpdate(str, obj);
				break;
			case T_SplitMerge:
				_outSplitMerge(str, obj);
				break;
			case T_AssertOp:
				_outAssertOp(str, obj);
				break;
			case T_PartitionSelector:
				_outPartitionSelector(str, obj);
				break;
			case T_Memoize:
				_outMemoize(str, obj);
				break;
			case T_TidRangeScan:
				_outTidRangeScan(str, obj);
				break;

			/* CBDB-specific binary nodes */
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
			case T_IndexClause:
				_outIndexClause(str, obj);
				break;
			case T_RestrictInfo:
				_outRestrictInfo(str, obj);
				break;
			case T_SpecialJoinInfo:
				_outSpecialJoinInfo(str, obj);
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
			case T_SliceTable:
				_outSliceTable(str, obj);
				break;
			case T_CursorPosInfo:
				_outCursorPosInfo(str, obj);
				break;
			case T_CdbProcess:
				_outCdbProcess(str, obj);
				break;
			case T_TupleDescNode:
				_outTupleDescNode(str, obj);
				break;
			case T_EphemeralNamedRelationInfo:
				_outEphemeralNamedRelationInfo(str, obj);
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
			case T_CookedConstraint:
				_outCookedConstraint(str, obj);
				break;
			default:
				elog(WARNING, "could not deserialize unrecognized node type: %d",
					 (int) nodeTag(obj));
				break;
		}
	}
}

/*
 * nodeToBinaryStringFast -
 *	   returns a binary representation of the Node as a palloc'd string
 */
char *
nodeToBinaryStringFast(void *obj, int *length)
{
	StringInfoData str;
	int16 tg = (int16) 0xDEAD;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfoOfSize(&str, 4096);

	_outNode(&str, obj);

	/* Add something special at the end that we can check in readfast.c */
	appendBinaryStringInfo(&str, (const char *)&tg, sizeof(int16));

	*length = str.len;
	return str.data;
}
