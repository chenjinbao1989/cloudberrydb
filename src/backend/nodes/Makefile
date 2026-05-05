#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    src/backend/nodes/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/nodes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# Generated files are output to the current directory; make sure we can
# #include them from copyfuncs.c etc.
override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	bitmapset.o \
	copyfuncs.o \
	equalfuncs.o \
	extensible.o \
	list.o \
	makefuncs.o \
	multibitmapset.o \
	nodeFuncs.o \
	nodes.o \
	outfast.o \
	outfuncs.o \
	params.o \
	print.o \
	read.o \
	readfast.o \
	readfuncs.o \
	tidbitmap.o \
	value.o

# The principal node-defining headers are listed in their inclusion order,
# to ensure cross-file subtyping works correctly.  The rest can just be
# listed alphabetically.
# Caution: re-ordering this list risks ABI breakage!
node_headers = \
	catalog/gp_distribution_policy.h \
	nodes/nodes.h \
	nodes/primnodes.h \
	nodes/parsenodes.h \
	nodes/pathnodes.h \
	nodes/plannodes.h \
	nodes/execnodes.h \
	access/amapi.h \
	access/sdir.h \
	access/tableam.h \
	access/tsmapi.h \
	commands/event_trigger.h \
	commands/trigger.h \
	executor/tuptable.h \
	foreign/fdwapi.h \
	nodes/bitmapset.h \
	nodes/extensible.h \
	nodes/lockoptions.h \
	nodes/miscnodes.h \
	nodes/replnodes.h \
	nodes/supportnodes.h \
	nodes/value.h \
	utils/rel.h

all: distprep generated-header-symlinks

distprep: node-support-stamp

.PHONY: generated-header-symlinks

generated-header-symlinks: $(top_builddir)/src/include/nodes/header-stamp

# node-support-stamp records the last time we ran gen_node_support.pl.
# We don't rely on the timestamps of the individual output files,
# because the Perl script won't update them if they didn't change (to
# avoid unnecessary recompiles).
node-support-stamp: gen_node_support.pl $(addprefix $(top_srcdir)/src/include/,$(node_headers))
	$(PERL) $^
	touch $@

# Symlink nodetags.h into builddir/src/include/nodes/ so it can be
# found as a system header.  We use header-stamp to record completion
# because the symlinks themselves may appear older than node-support-stamp.
$(top_builddir)/src/include/nodes/header-stamp: node-support-stamp
	prereqdir=`cd '$(dir $<)' >/dev/null && pwd` && \
	cd '$(dir $@)' && for file in nodetags.h; do \
	  rm -f $$file && $(LN_S) "$$prereqdir/$$file" . ; \
	done
	touch $@

# copyfuncs.c #includes copyfuncs.funcs.c and copyfuncs.switch.c,
# so make sure those are generated before compiling.
copyfuncs.o: copyfuncs.c copyfuncs.funcs.c copyfuncs.switch.c | node-support-stamp

# readfast.c #includes readfuncs.c. Same with outfast.c and outfuncs.c
readfast.o: readfuncs.c readfast.c readfuncs_common.c
outfast.o: outfuncs.c outfast.c outfuncs_common.c

include $(top_srcdir)/src/backend/common.mk

maintainer-clean: clean
	rm -f node-support-stamp copyfuncs.funcs.c copyfuncs.switch.c \
		equalfuncs.funcs.c equalfuncs.switch.c nodetags.h
