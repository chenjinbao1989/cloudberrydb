/*-------------------------------------------------------------------------
 *
 * equalfuncs.c
 *	  Equality functions to compare node trees.
 *
 * NOTE: we currently support comparing all node types found in parse
 * trees.  We do not support comparing executor state trees; there
 * is no need for that, and no point in maintaining all the code that
 * would be needed.  We also do not support comparing Path trees, mainly
 * because the circular linkages between RelOptInfo and Path nodes can't
 * be handled easily in a simple depth-first traversal.
 *
 * Currently, in fact, equal() doesn't know how to compare Plan trees
 * either.  This might need to be fixed someday.
 *
 * NOTE: it is intentional that parse location fields (in nodes that have
 * one) are not compared.  This is because we want, for example, a variable
 * "x" to be considered equal() to another reference to "x" in the query.
 *
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/equalfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "utils/datum.h"
#include "catalog/gp_distribution_policy.h"


/*
 * Macros to simplify comparison of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.  Note that these
 * hard-wire the convention that the local variables in an Equal routine are
 * named 'a' and 'b'.
 */

/* Compare a simple scalar field (int, float, bool, enum, etc) */
#define COMPARE_SCALAR_FIELD(fldname) \
	do { \
		if (a->fldname != b->fldname) \
			return false; \
	} while (0)

/* Compare a field that is a pointer to some kind of Node or Node tree */
#define COMPARE_NODE_FIELD(fldname) \
	do { \
		if (!equal(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* Compare a field that is a pointer to a Bitmapset */
#define COMPARE_BITMAPSET_FIELD(fldname) \
	do { \
		if (!bms_equal(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* Compare a field that is a pointer to a C string, or perhaps NULL */
#define COMPARE_STRING_FIELD(fldname) \
	do { \
		if (!equalstr(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* Macro for comparing string fields that might be NULL */
#define equalstr(a, b)	\
	(((a) != NULL && (b) != NULL) ? (strcmp(a, b) == 0) : (a) == (b))

/* Compare a field that is a pointer to a simple palloc'd object of size sz */
#define COMPARE_POINTER_FIELD(fldname, sz) \
	do { \
		if (memcmp(a->fldname, b->fldname, (sz)) != 0) \
			return false; \
	} while (0)

/*
 * Compare a field that is a varlena datum to the other.
 * Note the result will be false if one is toasted and the other is untoasted.
 * It depends on the context if we can say those are equal or not.
 */
#define COMPARE_VARLENA_FIELD(fldname, len) \
	do{ \
		if (a->fldname != b->fldname) \
		{ \
			if (a->fldname == NULL || b->fldname == NULL) \
				return false; \
			if (!datumIsEqual(PointerGetDatum(a->fldname), \
							  PointerGetDatum(b->fldname), false, len)) \
				return false; \
		} \
	} while (0)

/* Compare a parse location field (this is a no-op, per note above) */
#define COMPARE_LOCATION_FIELD(fldname) \
	((void) 0)

/* Compare a CoercionForm field (also a no-op, per comment in primnodes.h) */
#define COMPARE_COERCIONFORM_FIELD(fldname) \
	((void) 0)

/* Compare a field that is an inline array */
#define COMPARE_ARRAY_FIELD(fldname) \
	do { \
		if (memcmp(a->fldname, b->fldname, sizeof(a->fldname)) != 0) \
			return false; \
	} while (0)


/* ****************************************************************
 *					 generated equal functions
 * ****************************************************************
 */
#include "equalfuncs.funcs.c"


/*
 * _equalConst: custom because constvalue is a Datum, compared via
 * datumIsEqual rather than a simple scalar comparison.
 */
static bool
_equalConst(const Const *a, const Const *b)
{
	COMPARE_SCALAR_FIELD(consttype);
	COMPARE_SCALAR_FIELD(consttypmod);
	COMPARE_SCALAR_FIELD(constcollid);
	COMPARE_SCALAR_FIELD(constlen);
	COMPARE_SCALAR_FIELD(constisnull);
	COMPARE_SCALAR_FIELD(constbyval);
	COMPARE_LOCATION_FIELD(location);

	/*
	 * We treat all NULL constants of the same type as equal. Someday this
	 * might need to change?  But datumIsEqual doesn't work on nulls, so...
	 */
	if (a->constisnull)
		return true;
	return datumIsEqual(a->constvalue, b->constvalue,
						a->constbyval, a->constlen);
}

/*
 * _equalA_Const: custom because the val field is an inline union, compared
 * by recursively calling equal() on the embedded Node.
 */
static bool
_equalA_Const(const A_Const *a, const A_Const *b)
{
	if (!equal(&a->val, &b->val))	/* hack for in-line Value field */
		return false;
	COMPARE_LOCATION_FIELD(location);

	return true;
}

/*
 * _equalColumnDef: custom because missingVal (a Datum) and encoding are
 * intentionally skipped during equality comparison.
 */
static bool
_equalColumnDef(const ColumnDef *a, const ColumnDef *b)
{
	COMPARE_STRING_FIELD(colname);
	COMPARE_NODE_FIELD(typeName);
	COMPARE_STRING_FIELD(compression);
	COMPARE_SCALAR_FIELD(inhcount);
	COMPARE_SCALAR_FIELD(is_local);
	COMPARE_SCALAR_FIELD(is_not_null);
	COMPARE_SCALAR_FIELD(is_from_type);
	COMPARE_SCALAR_FIELD(attnum);
	COMPARE_SCALAR_FIELD(storage);
	COMPARE_NODE_FIELD(raw_default);
	COMPARE_NODE_FIELD(cooked_default);
	/* ignore cooked missingval */
	COMPARE_SCALAR_FIELD(identity);
	COMPARE_NODE_FIELD(identitySequence);
	COMPARE_SCALAR_FIELD(generated);
	COMPARE_NODE_FIELD(collClause);
	COMPARE_SCALAR_FIELD(collOid);
	COMPARE_NODE_FIELD(constraints);
	/* GPDB_90_MERGE_FIXME: should we be comparing encoding? */
	COMPARE_NODE_FIELD(fdwoptions);
	COMPARE_LOCATION_FIELD(location);

	return true;
}

/*
 * _equalExtensibleNode: custom because comparison is delegated to the
 * node's registered nodeEqual method.
 */
static bool
_equalExtensibleNode(const ExtensibleNode *a, const ExtensibleNode *b)
{
	const ExtensibleNodeMethods *methods;

	COMPARE_STRING_FIELD(extnodename);

	/* At this point, we know extnodename is the same for both nodes. */
	methods = GetExtensibleNodeMethods(a->extnodename, false);

	/* compare the private fields */
	if (!methods->nodeEqual(a, b))
		return false;

	return true;
}

/*
 * _equalBitmapset: custom because Bitmapset is not a standard node tree;
 * comparison is delegated to bms_equal.
 */
static bool
_equalBitmapset(const Bitmapset *a, const Bitmapset *b)
{
	return bms_equal(a, b);
}

/*
 * Stuff from pg_list.h
 */

static bool
_equalList(const List *a, const List *b)
{
	const ListCell *item_a;
	const ListCell *item_b;

	/*
	 * Try to reject by simple scalar checks before grovelling through all the
	 * list elements...
	 */
	COMPARE_SCALAR_FIELD(type);
	COMPARE_SCALAR_FIELD(length);

	/*
	 * We place the switch outside the loop for the sake of efficiency; this
	 * may not be worth doing...
	 */
	switch (a->type)
	{
		case T_List:
			forboth(item_a, a, item_b, b)
			{
				if (!equal(lfirst(item_a), lfirst(item_b)))
					return false;
			}
			break;
		case T_IntList:
			forboth(item_a, a, item_b, b)
			{
				if (lfirst_int(item_a) != lfirst_int(item_b))
					return false;
			}
			break;
		case T_OidList:
			forboth(item_a, a, item_b, b)
			{
				if (lfirst_oid(item_a) != lfirst_oid(item_b))
					return false;
			}
			break;
		default:
			elog(ERROR, "unrecognized list node type: %d",
				 (int) a->type);
			return false;		/* keep compiler quiet */
	}

	/*
	 * If we got here, we should have run out of elements of both lists
	 */
	Assert(item_a == NULL);
	Assert(item_b == NULL);

	return true;
}


/*
 * equal
 *	  returns whether two nodes are equal
 */
bool
equal(const void *a, const void *b)
{
	bool		retval;

	if (a == b)
		return true;

	/*
	 * note that a!=b, so only one of them can be NULL
	 */
	if (a == NULL || b == NULL)
		return false;

	/*
	 * are they the same type of nodes?
	 */
	if (nodeTag(a) != nodeTag(b))
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(a))
	{
#include "equalfuncs.switch.c"

		case T_List:
		case T_IntList:
		case T_OidList:
			retval = _equalList(a, b);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(a));
			retval = false;		/* keep compiler quiet */
			break;
	}

	return retval;
}
