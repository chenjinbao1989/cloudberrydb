/*-------------------------------------------------------------------------
 *
 * joinsample.c
 *	  GPDB: coordinated samples of column combinations, for estimating the
 *	  size of equijoins on several columns.
 *
 * For an equijoin on several columns, like
 *
 *		cs.item = cr.item AND cs.ord = cr.ord
 *
 * the number of matching rows depends on which combinations of the columns
 * occur in both relations. No statistics of one relation tell that: a
 * relation whose combinations are taken from the other one (a composite
 * foreign key) and one with the same numbers of distinct values, but
 * unrelated combinations, have the same statistics.
 *
 * A joinsample statistics object keeps, for the columns it is defined on,
 * the combinations with the smallest hash values, up to 10 times the
 * statistics target, each with the number of rows it occurs in. The hash of
 * a combination is the XOR of the extended hashes of its columns, each with
 * its position as the seed, so that (a, b) and (b, a) differ. It is computed
 * the same way for every relation, so a combination kept for one relation is
 * kept for another one too if it occurs there, and its hash is below the
 * thresholds of both samples. The number of matching rows is then estimated as
 *
 *		sum(count1 * count2) / fraction
 *
 * over the combinations in both samples with a hash up to the smaller
 * threshold, where fraction is the share of hash values up to that
 * threshold.
 *
 * Matching combinations have to occur in both samples, which they wouldn't
 * if the samples were taken from the random rows ANALYZE samples. So unless
 * that sample is the whole relation, the combinations are counted by a scan
 * of the whole relation, keeping those with a hash below a threshold chosen
 * from the number of combinations estimated from the row sample. That makes
 * it expensive to build, and it is built only if requested explicitly.
 *
 * Portions Copyright (c) 2026, Apache Software Foundation
 *
 * IDENTIFICATION
 *	  src/backend/statistics/joinsample.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* number of combinations to keep per statistics target */
#define JOINSAMPLE_ITEMS_PER_TARGET 10

/* how many times to scan the relation at most, to get enough combinations */
#define JOINSAMPLE_MAX_SCANS 3

#define SizeOfJoinSample(nitems) \
	(offsetof(MVJoinSample, items) + (nitems) * sizeof(MVJoinSampleItem))

/* hash values are compared as unsigned; SQL compares them as signed int8 */
#define HASH_TO_UNSIGNED(h) ((uint64) (h) ^ (UINT64CONST(1) << 63))
#define UNSIGNED_TO_HASH(u) ((int64) ((u) ^ (UINT64CONST(1) << 63)))

static int
compare_items(const void *a, const void *b)
{
	uint64		ha = ((const MVJoinSampleItem *) a)->hash;
	uint64		hb = ((const MVJoinSampleItem *) b)->hash;

	return (ha > hb) - (ha < hb);
}

static int
compare_uint64(const void *a, const void *b)
{
	uint64		ha = *(const uint64 *) a;
	uint64		hb = *(const uint64 *) b;

	return (ha > hb) - (ha < hb);
}

/*
 * Look up the extended hash function of each column. Returns false if a
 * column's type has none.
 */
static bool
joinsample_hash_procs(StatsBuildData *data, FmgrInfo **procs)
{
	int			i;

	for (i = 0; i < data->nattnums; i++)
	{
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(data->stats[i]->attrtypid,
									 TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(typentry->hash_extended_proc))
			return false;
		procs[i] = &typentry->hash_extended_proc_finfo;
	}
	return true;
}

/*
 * Hash the combinations in the sampled rows, sorted. Rows with a NULL in any
 * of the columns never match in an equijoin, and are left out.
 */
static uint64 *
joinsample_hash_rows(StatsBuildData *data, FmgrInfo **procs, int *nhashes)
{
	uint64	   *hashes = palloc(Max(data->numrows, 1) * sizeof(uint64));
	int			n = 0;
	int			row;

	for (row = 0; row < data->numrows; row++)
	{
		uint64		h = 0;
		int			i;

		for (i = 0; i < data->nattnums; i++)
		{
			uint64		c;

			if (data->nulls[i][row])
				break;
			c = DatumGetUInt64(FunctionCall2Coll(procs[i],
												 data->stats[i]->attrcollid,
												 data->values[i][row],
												 Int64GetDatum(i)));
			h ^= c;
		}
		if (i == data->nattnums)
			hashes[n++] = HASH_TO_UNSIGNED((int64) h);
	}
	qsort(hashes, n, sizeof(uint64), compare_uint64);
	*nhashes = n;
	return hashes;
}

/*
 * Count the combinations of the whole relation with a hash below threshold,
 * with a query. Returns the number of them, and stores them in *items. The
 * number of all rows is stored in *totalrows.
 */
static int
joinsample_scan(Relation rel, bool inh, StatsBuildData *data, FmgrInfo **procs,
				uint64 threshold, bool all, MVJoinSampleItem **items,
				double *totalrows)
{
	StringInfoData expr;
	StringInfoData notnull;
	StringInfoData query;
	MVJoinSampleItem *result;
	int			n;
	int			i;
	int			ret;

	initStringInfo(&expr);
	initStringInfo(&notnull);
	for (i = 0; i < data->nattnums; i++)
	{
		Oid			procoid = procs[i]->fn_oid;
		const char *col;
		char	   *colhash;

		col = quote_identifier(get_attname(RelationGetRelid(rel),
										   data->attnums[i], false));
		colhash = psprintf("%s.%s(%s, %d::pg_catalog.int8)",
						   quote_identifier(get_namespace_name(get_func_namespace(procoid))),
						   quote_identifier(get_func_name(procoid)),
						   col, i);
		if (i == 0)
			appendStringInfoString(&expr, colhash);
		else
		{
			char	   *prev = pstrdup(expr.data);

			resetStringInfo(&expr);
			appendStringInfo(&expr, "pg_catalog.int8xor(%s, %s)", prev, colhash);
		}
		appendStringInfo(&notnull, "%s%s IS NOT NULL", i > 0 ? " AND " : "", col);
	}

	/*
	 * The rows not kept, and those with NULLs, are counted in a group with a
	 * NULL hash.
	 */
	initStringInfo(&query);
	if (all)
		appendStringInfoString(&query, "SELECT h");
	else
		appendStringInfo(&query, "SELECT CASE WHEN h < " INT64_FORMAT " THEN h END",
						 UNSIGNED_TO_HASH(threshold));
	appendStringInfo(&query,
					 ", pg_catalog.count(*)::pg_catalog.float8"
					 " FROM (SELECT CASE WHEN %s THEN %s END AS h FROM %s%s.%s) s"
					 " GROUP BY 1",
					 notnull.data,
					 expr.data,
					 inh ? "" : "ONLY ",
					 quote_identifier(get_namespace_name(RelationGetNamespace(rel))),
					 quote_identifier(RelationGetRelationName(rel)));

	elog(DEBUG1, "joinsample query: %s", query.data);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	ret = SPI_execute(query.data, true, 0);
	if (ret != SPI_OK_SELECT)
		elog(ERROR, "joinsample query failed: %s", SPI_result_code_string(ret));

	/* allocate the result outside of SPI's memory context */
	result = SPI_palloc(Max(SPI_processed, 1) * sizeof(MVJoinSampleItem));
	n = 0;
	*totalrows = 0;
	for (i = 0; i < SPI_processed; i++)
	{
		HeapTuple	tup = SPI_tuptable->vals[i];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		bool		hnull;
		bool		cnull;
		Datum		h = SPI_getbinval(tup, desc, 1, &hnull);
		double		count = DatumGetFloat8(SPI_getbinval(tup, desc, 2, &cnull));

		*totalrows += count;
		if (hnull)
			continue;
		result[n].hash = HASH_TO_UNSIGNED(DatumGetInt64(h));
		result[n].count = count;
		n++;
	}
	SPI_finish();

	*items = result;
	return n;
}

/*
 * The threshold for hash values that keeps about a fraction p of them.
 */
static uint64
joinsample_threshold(double p)
{
	double		t = ldexp(p, 64);

	if (t >= ldexp(1.0, 64))
		return PG_UINT64_MAX;
	return (uint64) t;
}

/*
 * statext_joinsample_build
 *		Build the coordinated sample of the combinations of the columns.
 */
MVJoinSample *
statext_joinsample_build(Relation rel, bool inh, double totalrows,
						 StatsBuildData *data, int stattarget)
{
	FmgrInfo  **procs;
	uint64	   *hashes;
	int			nhashes;
	MVJoinSampleItem *items;
	int			nitems;
	int			k = Max(stattarget, 1) * JOINSAMPLE_ITEMS_PER_TARGET;
	uint64		threshold;
	MVJoinSample *result;
	int			i;

	procs = palloc(data->nattnums * sizeof(FmgrInfo *));
	if (!joinsample_hash_procs(data, procs))
	{
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("joinsample statistics need hashable column types"),
				 errtable(rel)));
		return NULL;
	}

	hashes = joinsample_hash_rows(data, procs, &nhashes);

	if (data->numrows >= totalrows)
	{
		/* The sample is the whole relation: count the combinations in it. */
		items = palloc(Max(nhashes, 1) * sizeof(MVJoinSampleItem));
		nitems = 0;
		for (i = 0; i < nhashes; i++)
		{
			if (nitems > 0 && items[nitems - 1].hash == hashes[i])
				items[nitems - 1].count += 1;
			else
			{
				items[nitems].hash = hashes[i];
				items[nitems].count = 1;
				nitems++;
			}
		}
		threshold = PG_UINT64_MAX;
		/* totalrows may be an estimate; the counts are exact */
		totalrows = data->numrows;
	}
	else
	{
		int			d = 0;
		int			f1 = 0;
		double		ndistinct;
		double		p;
		int			scan;

		/*
		 * Estimate the number of combinations from the sampled rows, as
		 * estimate_ndistinct() in mvdistinct.c does, to choose the threshold.
		 */
		for (i = 0; i < nhashes; i++)
		{
			if (i == 0 || hashes[i] != hashes[i - 1])
			{
				d++;
				if (i + 1 == nhashes || hashes[i + 1] != hashes[i])
					f1++;
			}
		}
		if (nhashes == 0)
			ndistinct = 1.0;
		else
		{
			double		denom = (double) (nhashes - f1) +
				(double) f1 * (double) nhashes / totalrows;

			ndistinct = Min(Max((double) nhashes * d / denom, d), totalrows);
		}

		/* Aim at twice as many combinations as we keep, then trim them. */
		p = Min(1.0, 2.0 * k / Max(ndistinct, 1.0));
		for (scan = 1;; scan++)
		{
			threshold = joinsample_threshold(p);
			nitems = joinsample_scan(rel, inh, data, procs, threshold,
									 p >= 1.0, &items, &totalrows);
			if (nitems >= k || p >= 1.0 || scan >= JOINSAMPLE_MAX_SCANS)
				break;
			/* too few: there are more duplicates than estimated */
			p = Min(1.0, p * Max(2.0, 2.0 * k / Max(nitems, 1)));
		}
		if (p >= 1.0)
			threshold = PG_UINT64_MAX;
		else
			threshold--;		/* the query kept hashes < threshold */
		qsort(items, nitems, sizeof(MVJoinSampleItem), compare_items);
	}

	/* Keep the k smallest hashes. */
	if (nitems > k)
	{
		nitems = k;
		threshold = items[k - 1].hash;
	}

	result = palloc(SizeOfJoinSample(nitems));
	result->magic = STATS_JOINSAMPLE_MAGIC;
	result->type = STATS_JOINSAMPLE_TYPE_BASIC;
	result->nitems = nitems;
	result->totalrows = totalrows;
	result->threshold = threshold;
	memcpy(result->items, items, nitems * sizeof(MVJoinSampleItem));

	return result;
}

/*
 * statext_joinsample_serialize
 *		serialize a joinsample to the on-disk bytea format
 */
bytea *
statext_joinsample_serialize(MVJoinSample *joinsample)
{
	Size		len = SizeOfJoinSample(joinsample->nitems);
	bytea	   *output = palloc(VARHDRSZ + len);

	SET_VARSIZE(output, VARHDRSZ + len);
	memcpy(VARDATA(output), joinsample, len);
	return output;
}

/*
 * statext_joinsample_deserialize
 *		read a serialized joinsample into an MVJoinSample structure
 */
MVJoinSample *
statext_joinsample_deserialize(bytea *data)
{
	MVJoinSample *result;
	Size		len = VARSIZE_ANY_EXHDR(data);
	MVJoinSample header;

	if (len < offsetof(MVJoinSample, items))
		elog(ERROR, "invalid joinsample size %zu", len);
	memcpy(&header, VARDATA_ANY(data), offsetof(MVJoinSample, items));
	if (header.magic != STATS_JOINSAMPLE_MAGIC)
		elog(ERROR, "invalid joinsample magic %08x (expected %08x)",
			 header.magic, STATS_JOINSAMPLE_MAGIC);
	if (header.type != STATS_JOINSAMPLE_TYPE_BASIC)
		elog(ERROR, "invalid joinsample type %d (expected %d)",
			 header.type, STATS_JOINSAMPLE_TYPE_BASIC);
	if (len != SizeOfJoinSample(header.nitems))
		elog(ERROR, "invalid joinsample size %zu (expected %zu)",
			 len, SizeOfJoinSample(header.nitems));

	result = palloc(len);
	memcpy(result, VARDATA_ANY(data), len);
	return result;
}

/*
 * statext_joinsample_load
 *		Load the joinsample for the indicated pg_statistic_ext tuple
 */
MVJoinSample *
statext_joinsample_load(Oid mvoid, bool inh)
{
	MVJoinSample *result;
	bool		isnull;
	Datum		datum;
	HeapTuple	htup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(mvoid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	datum = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							Anum_pg_statistic_ext_data_stxdjoinsample, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_JOINSAMPLE, mvoid);

	result = statext_joinsample_deserialize(DatumGetByteaPP(datum));

	ReleaseSysCache(htup);

	return result;
}

/*
 * joinsample_selectivity
 *		Estimate the selectivity of an equijoin of the columns of two
 *		joinsamples, as a fraction of the product of their rows.
 *
 * The columns must correspond in the order of their attribute numbers, have
 * the same types, and be compared with their types' default equality.
 */
static Selectivity
joinsample_selectivity(MVJoinSample *a, MVJoinSample *b)
{
	uint64		threshold = Min(a->threshold, b->threshold);
	double		fraction;
	double		matches = 0.0;
	double		rows;
	uint32		i = 0;
	uint32		j = 0;

	while (i < a->nitems && j < b->nitems &&
		   a->items[i].hash <= threshold && b->items[j].hash <= threshold)
	{
		if (a->items[i].hash < b->items[j].hash)
			i++;
		else if (a->items[i].hash > b->items[j].hash)
			j++;
		else
		{
			matches += a->items[i].count * b->items[j].count;
			i++;
			j++;
		}
	}

	fraction = (threshold == PG_UINT64_MAX) ? 1.0 :
		ldexp((double) threshold + 1.0, -64);

	/*
	 * No matches in the samples doesn't mean none at all: estimate one, as if
	 * it had just missed the sample.
	 */
	rows = Max(matches, 1.0) / fraction;
	rows = Min(rows, Max(a->totalrows, 1.0) * Max(b->totalrows, 1.0));

	return rows / (Max(a->totalrows, 1.0) * Max(b->totalrows, 1.0));
}

/*
 * One way to see an equijoin clause as "a.x = b.y", with a in the outer and b
 * in the inner relation of a join, where both are base relations with
 * joinsample statistics, and the clause can be estimated with them.
 */
typedef struct JoinSampleTerm
{
	int			outer_relid;
	int			inner_relid;
	AttrNumber	outer_attnum;
	AttrNumber	inner_attnum;
} JoinSampleTerm;

/*
 * Does base relation 'relid' have joinsample statistics?
 */
static bool
rel_has_joinsample(PlannerInfo *root, int relid)
{
	RelOptInfo *rel;
	RangeTblEntry *rte;
	ListCell   *lc;

	if (relid <= 0 || relid >= root->simple_rel_array_size)
		return false;
	rel = root->simple_rel_array[relid];
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL)
		return false;
	rte = planner_rt_fetch(relid, root);
	foreach(lc, rel->statlist)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);

		if (info->kind == STATS_EXT_JOINSAMPLE && info->inherit == rte->inh)
			return true;
	}
	return false;
}

/*
 * If 'expr' is a plain column of a single base relation with joinsample
 * statistics, possibly relabeled, return it.
 */
static Var *
joinsample_column(PlannerInfo *root, Expr *expr)
{
	while (IsA(expr, RelabelType))
		expr = ((RelabelType *) expr)->arg;
	if (!IsA(expr, Var) ||
		((Var *) expr)->varlevelsup != 0 ||
		((Var *) expr)->varattno <= 0 ||
		!rel_has_joinsample(root, ((Var *) expr)->varno))
		return NULL;
	return (Var *) expr;
}

/*
 * Can "outer = inner" be estimated with joinsample statistics on the two
 * columns, with operator 'opno', or any equality operator of 'opfamilies' if
 * that's InvalidOid, and collation 'collid'? The samples hash each column with
 * the default extended hash function of its type, so the two must have the
 * same type and collation, and be compared with the type's default equality.
 */
static bool
joinsample_comparable(Var *outer, Var *inner, Oid opno, List *opfamilies,
					  Oid collid)
{
	TypeCacheEntry *typentry;
	ListCell   *lc;

	if (outer->vartype != inner->vartype ||
		outer->varcollid != inner->varcollid ||
		collid != outer->varcollid)
		return false;

	typentry = lookup_type_cache(outer->vartype,
								 TYPECACHE_EQ_OPR | TYPECACHE_HASH_EXTENDED_PROC);
	if (!OidIsValid(typentry->eq_opr) ||
		!OidIsValid(typentry->hash_extended_proc))
		return false;
	if (OidIsValid(opno))
		return opno == typentry->eq_opr;
	foreach(lc, opfamilies)
	{
		if (op_in_opfamily(typentry->eq_opr, lfirst_oid(lc)))
			return true;
	}
	return false;
}

static List *
add_joinsample_term(List *terms, Var *outer, Var *inner)
{
	JoinSampleTerm *term;
	ListCell   *lc;

	/* Keep one term per pair of relations, the first one */
	foreach(lc, terms)
	{
		term = (JoinSampleTerm *) lfirst(lc);
		if (term->outer_relid == outer->varno && term->inner_relid == inner->varno)
			return terms;
	}

	term = palloc(sizeof(JoinSampleTerm));
	term->outer_relid = outer->varno;
	term->inner_relid = inner->varno;
	term->outer_attnum = outer->varattno;
	term->inner_attnum = inner->varattno;
	return lappend(terms, term);
}

/*
 * Return the ways to see 'rinfo' as an equijoin clause between columns of a
 * base relation of 'outer_relids' and one of 'inner_relids', both with
 * joinsample statistics.
 *
 * The size of a join relation is estimated once, from the first pair of
 * inputs it is built from, so the clauses we see depend on the join order. A
 * clause generated from an EquivalenceClass is only one of the equalities the
 * class implies between the two sides. In "x.ord = r.ord AND s.ord = r.ord
 * AND s.item = r.item", joining {x, r} to s may get "x.ord = s.ord", which
 * doesn't go with "s.item = r.item". So we also return "r.ord = s.ord", and
 * every other pair of columns of the class on the two sides, even if the
 * clause itself is between relations without joinsample statistics.
 */
static List *
joinsample_terms(PlannerInfo *root, RestrictInfo *rinfo,
				 Relids outer_relids, Relids inner_relids)
{
	List	   *terms = NIL;
	OpExpr	   *opexpr;
	Var		   *left;
	Var		   *right;
	EquivalenceClass *ec = rinfo->parent_ec;
	ListCell   *lc;
	ListCell   *lc2;

	if (rinfo->pseudoconstant || rinfo->mergeopfamilies == NIL ||
		!is_opclause(rinfo->clause))
		return NIL;
	opexpr = (OpExpr *) rinfo->clause;
	if (list_length(opexpr->args) != 2)
		return NIL;

	/* The clause itself, if it's between relations with joinsamples */
	left = joinsample_column(root, linitial(opexpr->args));
	right = joinsample_column(root, lsecond(opexpr->args));
	if (left != NULL && right != NULL)
	{
		if (bms_is_member(left->varno, outer_relids) &&
			bms_is_member(right->varno, inner_relids))
		{
			if (joinsample_comparable(left, right, opexpr->opno, NIL,
									  opexpr->inputcollid))
				terms = add_joinsample_term(terms, left, right);
		}
		else if (bms_is_member(right->varno, outer_relids) &&
				 bms_is_member(left->varno, inner_relids))
		{
			if (joinsample_comparable(right, left, opexpr->opno, NIL,
									  opexpr->inputcollid))
				terms = add_joinsample_term(terms, right, left);
		}
	}

	/* Equivalent clauses */
	if (ec == NULL || ec->ec_has_volatile || ec->ec_broken)
		return terms;
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *outer_em = (EquivalenceMember *) lfirst(lc);
		Var		   *outer;

		if (outer_em->em_is_const || outer_em->em_is_child)
			continue;
		outer = joinsample_column(root, outer_em->em_expr);
		if (outer == NULL || !bms_is_member(outer->varno, outer_relids))
			continue;

		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *inner_em = (EquivalenceMember *) lfirst(lc2);
			Var		   *inner;

			if (inner_em->em_is_const || inner_em->em_is_child)
				continue;
			inner = joinsample_column(root, inner_em->em_expr);
			if (inner == NULL || !bms_is_member(inner->varno, inner_relids))
				continue;
			if (joinsample_comparable(outer, inner, InvalidOid,
									  ec->ec_opfamilies, ec->ec_collation))
				terms = add_joinsample_term(terms, outer, inner);
		}
	}

	return terms;
}

/*
 * Find joinsample statistics on exactly the columns 'attnums' of base
 * relation 'relid'.
 */
static StatisticExtInfo *
find_joinsample(PlannerInfo *root, int relid, Bitmapset *attnums)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	RangeTblEntry *rte = planner_rt_fetch(relid, root);
	ListCell   *lc;

	foreach(lc, rel->statlist)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);

		if (info->kind == STATS_EXT_JOINSAMPLE &&
			info->inherit == rte->inh &&
			info->exprs == NIL &&
			bms_equal(info->keys, attnums))
			return info;
	}
	return NULL;
}

/*
 * Estimate the selectivity of the equijoin of the columns of 'terms', all
 * between the same two relations, from joinsample statistics on exactly those
 * columns of each. Returns false if there are none, or the columns don't
 * correspond in the order of their attribute numbers, as the samples combine
 * the hashes of the columns in that order.
 */
static bool
joinsample_group_selectivity(PlannerInfo *root, List *terms, Selectivity *result)
{
	JoinSampleTerm *first = (JoinSampleTerm *) linitial(terms);
	Bitmapset  *outer_set = NULL;
	Bitmapset  *inner_set = NULL;
	StatisticExtInfo *outer_info;
	StatisticExtInfo *inner_info;
	ListCell   *lc;
	ListCell   *lc2;

	foreach(lc, terms)
	{
		JoinSampleTerm *a = (JoinSampleTerm *) lfirst(lc);

		outer_set = bms_add_member(outer_set, a->outer_attnum);
		inner_set = bms_add_member(inner_set, a->inner_attnum);
		for_each_cell(lc2, terms, lnext(terms, lc))
		{
			JoinSampleTerm *b = (JoinSampleTerm *) lfirst(lc2);

			if ((a->outer_attnum < b->outer_attnum) !=
				(a->inner_attnum < b->inner_attnum))
				return false;
		}
	}
	if (bms_num_members(outer_set) != list_length(terms) ||
		bms_num_members(inner_set) != list_length(terms))
		return false;

	outer_info = find_joinsample(root, first->outer_relid, outer_set);
	inner_info = find_joinsample(root, first->inner_relid, inner_set);
	if (outer_info == NULL || inner_info == NULL)
		return false;

	*result = joinsample_selectivity(
		statext_joinsample_load(outer_info->statOid, outer_info->inherit),
		statext_joinsample_load(inner_info->statOid, inner_info->inherit));
	return true;
}

/*
 * statext_joinsample_join_selectivity
 *		GPDB: estimate groups of equijoin clauses between the same two base
 *		relations together, from joinsample statistics on both.
 *
 * With clauses "a.x = b.x AND a.y = b.y", clauselist_selectivity() multiplies
 * the selectivities of the two clauses, assuming that the columns are
 * independent. When (x, y) is in fact a composite key, like (item, order
 * number) in TPC-DS catalog_sales and catalog_returns, that is off by orders
 * of magnitude. If both relations have joinsample statistics on exactly the
 * columns of the clauses, we estimate instead from the combinations the two
 * samples have in common how many rows match (see joinsample_selectivity()).
 *
 * We group the clauses by the pair of base relations they join, taking the
 * pair with the most clauses first, then the next among the remaining
 * clauses, and so on (see joinsample_terms() for why a clause can join
 * several pairs). Groups of two or more clauses with joinsample statistics on
 * exactly their columns are estimated that way.
 *
 * Like get_foreign_key_join_selectivity(), the clauses we estimate this way
 * are removed from *restrictlist, and the returned selectivity applies to
 * them. Only inner joins are handled.
 */
Selectivity
statext_joinsample_join_selectivity(PlannerInfo *root,
									Relids outer_relids,
									Relids inner_relids,
									SpecialJoinInfo *sjinfo,
									List **restrictlist)
{
	Selectivity result = 1.0;
	List	   *rinfos = NIL;	/* candidate clauses */
	List	   *termlists = NIL;	/* their terms */
	bool		outer_found = false;
	bool		inner_found = false;
	ListCell   *lc;
	ListCell   *lc2;
	int			relid;

	if (sjinfo->jointype != JOIN_INNER || list_length(*restrictlist) < 2)
		return 1.0;

	/* Quick exit if one side has no joinsample statistics */
	relid = -1;
	while (!outer_found && (relid = bms_next_member(outer_relids, relid)) >= 0)
		outer_found = rel_has_joinsample(root, relid);
	relid = -1;
	while (!inner_found && (relid = bms_next_member(inner_relids, relid)) >= 0)
		inner_found = rel_has_joinsample(root, relid);
	if (!outer_found || !inner_found)
		return 1.0;

	foreach(lc, *restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		List	   *terms = joinsample_terms(root, rinfo, outer_relids, inner_relids);

		if (terms != NIL)
		{
			rinfos = lappend(rinfos, rinfo);
			termlists = lappend(termlists, terms);
		}
	}

	while (list_length(rinfos) >= 2)
	{
		int			best_outer = 0;
		int			best_inner = 0;
		int			best_count = 0;
		List	   *group = NIL;	/* RestrictInfos */
		List	   *group_terms = NIL;
		List	   *rest_rinfos = NIL;
		List	   *rest_termlists = NIL;
		Selectivity sel;

		/* Find the pair of relations with the most remaining clauses */
		foreach(lc, termlists)
		{
			foreach(lc2, (List *) lfirst(lc))
			{
				JoinSampleTerm *term = (JoinSampleTerm *) lfirst(lc2);
				int			count = 0;
				ListCell   *lc3;
				ListCell   *lc4;

				foreach(lc3, termlists)
				{
					foreach(lc4, (List *) lfirst(lc3))
					{
						JoinSampleTerm *t = (JoinSampleTerm *) lfirst(lc4);

						if (t->outer_relid == term->outer_relid &&
							t->inner_relid == term->inner_relid)
						{
							count++;
							break;
						}
					}
				}
				if (count > best_count)
				{
					best_outer = term->outer_relid;
					best_inner = term->inner_relid;
					best_count = count;
				}
			}
		}
		if (best_count < 2)
			break;

		/* Collect the clauses between them, and keep the others */
		forboth(lc, rinfos, lc2, termlists)
		{
			JoinSampleTerm *found = NULL;
			ListCell   *lc3;

			foreach(lc3, (List *) lfirst(lc2))
			{
				JoinSampleTerm *term = (JoinSampleTerm *) lfirst(lc3);

				if (term->outer_relid == best_outer &&
					term->inner_relid == best_inner)
				{
					found = term;
					break;
				}
			}
			if (found)
			{
				group = lappend(group, lfirst(lc));
				group_terms = lappend(group_terms, found);
			}
			else
			{
				rest_rinfos = lappend(rest_rinfos, lfirst(lc));
				rest_termlists = lappend(rest_termlists, lfirst(lc2));
			}
		}
		rinfos = rest_rinfos;
		termlists = rest_termlists;

		if (!joinsample_group_selectivity(root, group_terms, &sel))
			continue;

		elog(DEBUG1, "joinsample join selectivity: %d clauses between rels %d and %d, %g",
			 list_length(group), best_outer, best_inner, sel);
		*restrictlist = list_difference_ptr(*restrictlist, group);
		result *= sel;
	}

	return result;
}
