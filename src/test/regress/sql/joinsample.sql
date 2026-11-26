--
-- joinsample statistics: coordinated samples of the combinations of columns,
-- for estimating equijoins on several columns.
--
create schema joinsample;
set search_path = joinsample;
set optimizer = off;

-- Return the estimated rows of the first Hash Join in a plan (per segment).
create function js_join_rows(query text) returns int language plpgsql as $$
declare
  l text;
begin
  for l in execute query loop
    if l ~ 'Hash Join' then
      return substring(l from 'rows=(\d+)')::int;
    end if;
  end loop;
  return null;
end $$;

-- (item, ord) is a composite key of js_sales, and js_returns has 3000 of its
-- rows. js_fake has the same numbers of distinct values as js_returns, but
-- unrelated combinations, and only 25 rows match.
create table js_sales (item int, ord int, v int) distributed by (ord);
create table js_returns (item int, ord int, v int) distributed by (ord);
create table js_fake (item int, ord int) distributed by (ord);
insert into js_sales select i % 1000, i / 3, i from generate_series(1, 30000) i;
insert into js_returns select item, ord, v from js_sales where v % 10 = 0;
insert into js_fake select (abs(hashint4(ord)) % 100) * 10, ord from js_returns;
-- Independent columns of 2000 values each: their combinations are almost all
-- unique, and 103 rows match.
create table js_a1 (x int, y int) distributed randomly;
create table js_a2 (x int, y int) distributed randomly;
insert into js_a1 select abs(hashint4(i)) % 2000, abs(hashint4(i + 7000000)) % 2000 from generate_series(1, 20000) i;
insert into js_a2 select abs(hashint4(i + 3000000)) % 2000, abs(hashint4(i + 9000000)) % 2000 from generate_series(1, 20000) i;
analyze js_sales;
analyze js_returns;
analyze js_fake;
analyze js_a1;
analyze js_a2;

-- Without joinsample statistics, the selectivities of the clauses are
-- multiplied, and the key join is estimated at far fewer than 3000 rows.
select js_join_rows('explain select * from js_sales s join js_returns r on s.item = r.item and s.ord = r.ord') < 100 as underestimated;
select count(*) from js_sales s join js_returns r on s.item = r.item and s.ord = r.ord;
select count(*) from js_sales s join js_fake r on s.item = r.item and s.ord = r.ord;
select count(*) from js_a1 join js_a2 using (x, y);

create statistics js_sales_js (joinsample) on item, ord from js_sales;
create statistics js_returns_js (joinsample) on item, ord from js_returns;
create statistics js_fake_js (joinsample) on item, ord from js_fake;
create statistics js_a1_js (joinsample) on x, y from js_a1;
create statistics js_a2_js (joinsample) on x, y from js_a2;
select pg_get_statisticsobjdef(oid) from pg_statistic_ext where stxname = 'js_a1_js';
-- Not built by default
create statistics js_a1_default on x, y from js_a1;
select stxkind from pg_statistic_ext where stxname = 'js_a1_default';
drop statistics js_a1_default;
analyze js_sales;
analyze js_returns;
analyze js_fake;
analyze js_a1;
analyze js_a2;
select s.stxname, d.stxdinherit, d.stxdjoinsample is not null as built
  from pg_statistic_ext s join pg_statistic_ext_data d on d.stxoid = s.oid
 where s.stxname like 'js\_%' order by 1;

-- With them, the joins are estimated from the combinations the samples have
-- in common.
select js_join_rows('explain select * from js_sales s join js_returns r on s.item = r.item and s.ord = r.ord') between 500 and 2000 as close_to_actual;
select js_join_rows('explain select * from js_sales s join js_fake r on s.item = r.item and s.ord = r.ord') < 100 as close_to_actual;
select js_join_rows('explain select * from js_a1 join js_a2 using (x, y)') between 10 and 100 as close_to_actual;
-- Also when joining {x, r} to s first, which gets "x.ord = s.ord" from the
-- equivalence class instead of "r.ord = s.ord".
create table js_x (ord int, w int) distributed by (ord);
insert into js_x select i, i from generate_series(0, 10000) i;
analyze js_x;
select js_join_rows('explain select * from js_x x join js_returns r on x.ord = r.ord join js_sales s on s.item = r.item and s.ord = r.ord') between 500 and 2000 as close_to_actual;
select count(*) from js_x x join js_returns r on x.ord = r.ord join js_sales s on s.item = r.item and s.ord = r.ord;

-- Only if both sides have joinsample statistics on exactly the columns of the
-- clauses, which correspond in the order of their attribute numbers.
create table js_swapped (ord int, item int) distributed by (ord);
insert into js_swapped select ord, item from js_returns;
create statistics js_swapped_js (joinsample) on ord, item from js_swapped;
analyze js_swapped;
select js_join_rows('explain select * from js_sales s join js_swapped r on s.item = r.item and s.ord = r.ord') < 100 as not_used;
select js_join_rows('explain select * from js_sales s join js_returns r on s.item = r.item and s.ord = r.ord and s.v = r.v') < 100 as not_used;

-- A relation bigger than ANALYZE's sample is scanned to count the
-- combinations. ANALYZE samples 3000 rows with a statistics target of 10; the
-- joinsample keeps 10 combinations per unit of its own target.
create table js_big (item int, ord int) distributed by (ord);
insert into js_big select item, ord from js_sales;
create statistics js_big_js (joinsample) on item, ord from js_big;
alter statistics js_big_js set statistics 100;
set default_statistics_target = 10;
analyze js_big;
reset default_statistics_target;
select js_join_rows('explain select * from js_big s join js_returns r on s.item = r.item and s.ord = r.ord') between 500 and 2000 as close_to_actual;
select js_join_rows('explain select * from js_big s join js_fake r on s.item = r.item and s.ord = r.ord') < 100 as close_to_actual;

-- Partitioned tables
create table js_part (item int, ord int) distributed by (ord)
  partition by range (ord) (start (0) end (10001) every (2500));
insert into js_part select item, ord from js_returns;
create statistics js_part_js (joinsample) on item, ord from js_part;
analyze js_part;
select js_join_rows('explain select * from js_sales s join js_part r on s.item = r.item and s.ord = r.ord') between 500 and 2000 as close_to_actual;

-- Only on two or more plain columns
create statistics js_bad (joinsample) on (x + 1), y from js_a1;
create statistics js_bad (joinsample, ndistinct) on x, (y + 1) from js_a1;

reset optimizer;
reset search_path;
drop schema joinsample cascade;
