/*-------------------------------------------------------------------------
 *
 * statistics.h
 *	  Extended statistics and selectivity estimation functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/statistics/statistics.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATISTICS_H
#define STATISTICS_H

#include "commands/vacuum.h"
#include "nodes/pathnodes.h"

#define STATS_MAX_DIMENSIONS	8	/* max number of attributes */

/* Multivariate distinct coefficients */
#define STATS_NDISTINCT_MAGIC		0xA352BFA4	/* struct identifier */
#define STATS_NDISTINCT_TYPE_BASIC	1	/* struct version */

/* MVNDistinctItem represents a single combination of columns */
typedef struct MVNDistinctItem
{
	double		ndistinct;		/* ndistinct value for this combination */
	int			nattributes;	/* number of attributes */
	AttrNumber *attributes;		/* attribute numbers */
} MVNDistinctItem;

/* A MVNDistinct object, comprising all possible combinations of columns */
typedef struct MVNDistinct
{
	uint32		magic;			/* magic constant marker */
	uint32		type;			/* type of ndistinct (BASIC) */
	uint32		nitems;			/* number of items in the statistic */
	MVNDistinctItem items[FLEXIBLE_ARRAY_MEMBER];
} MVNDistinct;

/* Multivariate functional dependencies */
#define STATS_DEPS_MAGIC		0xB4549A2C	/* marks serialized bytea */
#define STATS_DEPS_TYPE_BASIC	1	/* basic dependencies type */

/*
 * Functional dependencies, tracking column-level relationships (values
 * in one column determine values in another one).
 */
typedef struct MVDependency
{
	double		degree;			/* degree of validity (0-1) */
	AttrNumber	nattributes;	/* number of attributes */
	AttrNumber	attributes[FLEXIBLE_ARRAY_MEMBER];	/* attribute numbers */
} MVDependency;

typedef struct MVDependencies
{
	uint32		magic;			/* magic constant marker */
	uint32		type;			/* type of MV Dependencies (BASIC) */
	uint32		ndeps;			/* number of dependencies */
	MVDependency *deps[FLEXIBLE_ARRAY_MEMBER];	/* dependencies */
} MVDependencies;

/* used to flag stats serialized to bytea */
#define STATS_MCV_MAGIC			0xE1A651C2	/* marks serialized bytea */
#define STATS_MCV_TYPE_BASIC	1	/* basic MCV list type */

/* max items in MCV list (should be equal to max default_statistics_target) */
#define STATS_MCVLIST_MAX_ITEMS        10000

/*
 * Multivariate MCV (most-common value) lists
 *
 * A straightforward extension of MCV items - i.e. a list (array) of
 * combinations of attribute values, together with a frequency and null flags.
 */
typedef struct MCVItem
{
	double		frequency;		/* frequency of this combination */
	double		base_frequency; /* frequency if independent */
	bool	   *isnull;			/* NULL flags */
	Datum	   *values;			/* item values */
} MCVItem;

/* multivariate MCV list - essentially an array of MCV items */
typedef struct MCVList
{
	uint32		magic;			/* magic constant marker */
	uint32		type;			/* type of MCV list (BASIC) */
	uint32		nitems;			/* number of MCV items in the array */
	AttrNumber	ndimensions;	/* number of dimensions */
	Oid			types[STATS_MAX_DIMENSIONS];	/* OIDs of data types */
	MCVItem		items[FLEXIBLE_ARRAY_MEMBER];	/* array of MCV items */
} MCVList;

/* used to flag stats serialized to bytea */
#define STATS_JOINSAMPLE_MAGIC		0x4A53504C	/* marks serialized bytea */
#define STATS_JOINSAMPLE_TYPE_BASIC	1	/* basic joinsample type */

/*
 * GPDB: coordinated sample of the combinations of the columns (see
 * joinsample.c)
 */
typedef struct MVJoinSampleItem
{
	uint64		hash;			/* hash of the combination */
	double		count;			/* number of rows with it */
} MVJoinSampleItem;

typedef struct MVJoinSample
{
	uint32		magic;			/* magic constant marker */
	uint32		type;			/* type of joinsample (BASIC) */
	uint32		nitems;			/* number of items in the array */
	double		totalrows;		/* number of rows in the relation */
	uint64		threshold;		/* all combinations with hash <= this */
	MVJoinSampleItem items[FLEXIBLE_ARRAY_MEMBER];	/* sorted by hash */
} MVJoinSample;


extern MVNDistinct *statext_ndistinct_load(Oid mvoid, bool inh);
extern MVDependencies *statext_dependencies_load(Oid mvoid, bool inh, bool allow_null);
extern MCVList *statext_mcv_load(Oid mvoid, bool inh);
extern MVJoinSample *statext_joinsample_load(Oid mvoid, bool inh);
extern Selectivity statext_joinsample_join_selectivity(PlannerInfo *root,
													   Relids outer_relids,
													   Relids inner_relids,
													   SpecialJoinInfo *sjinfo,
													   List **restrictlist);

extern void BuildRelationExtStatistics(Relation onerel, bool inh, double totalrows,
									   int numrows, HeapTuple *rows,
									   int natts, VacAttrStats **vacattrstats);
extern int	ComputeExtStatisticsRows(Relation onerel,
									 int natts, VacAttrStats **vacattrstats);
extern bool statext_is_kind_built(HeapTuple htup, char type);
extern Selectivity dependencies_clauselist_selectivity(PlannerInfo *root,
													   List *clauses,
													   int varRelid,
													   JoinType jointype,
													   SpecialJoinInfo *sjinfo,
													   RelOptInfo *rel,
													   Bitmapset **estimatedclauses);
extern Selectivity statext_clauselist_selectivity(PlannerInfo *root,
												  List *clauses,
												  int varRelid,
												  JoinType jointype,
												  SpecialJoinInfo *sjinfo,
												  RelOptInfo *rel,
												  Bitmapset **estimatedclauses,
												  bool is_or);
extern bool has_stats_of_kind(List *stats, char requiredkind);
extern StatisticExtInfo *choose_best_statistics(List *stats, char requiredkind,
												bool inh,
												Bitmapset **clause_attnums,
												List **clause_exprs,
												int nclauses);
extern HeapTuple statext_expressions_load(Oid stxoid, bool inh, int idx);

#endif							/* STATISTICS_H */
