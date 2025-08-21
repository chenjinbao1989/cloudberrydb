MODULE_big = yagp_hooks_collector
EXTENSION  = yagp_hooks_collector
DATA       = $(wildcard *--*.sql)
REGRESS    = yagp_cursors yagp_dist yagp_select yagp_utf8_trim yagp_utility yagp_guc_cache yagp_uds yagp_locale

PROTO_BASES = yagpcc_plan yagpcc_metrics yagpcc_set_service
PROTO_OBJS  = $(patsubst %,src/protos/%.pb.o,$(PROTO_BASES))

C_OBJS   = $(patsubst %.c,%.o,$(wildcard src/*.c src/*/*.c))
CPP_OBJS = $(patsubst %.cpp,%.o,$(wildcard src/*.cpp src/log/*.cpp src/memory/*.cpp))
OBJS     = $(C_OBJS) $(CPP_OBJS) $(PROTO_OBJS)

PG_CXXFLAGS += -Werror -Wall -Wno-unused-but-set-variable -std=c++17 -Isrc/protos -Isrc -Iinclude -DGPBUILD
SHLIB_LINK += -lprotobuf -lstdc++
EXTRA_CLEAN = src/protos

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = gpcontrib/yagp_hooks_collector
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

src/protos/.done: $(wildcard protos/*.proto)
	@mkdir -p src/protos
	sed -i 's/optional //g' $^
	sed -i 's|cloud/mdb/yagpcc/api/proto/common/|protos/|g' $^
	protoc -I /usr/include -I /usr/local/include -I . --cpp_out=src $^
	for f in src/protos/*.pb.cc; do mv "$$f" "$${f%.cc}.cpp"; done
	touch $@

src/protos/%.pb.cpp src/protos/%.pb.h: src/protos/.done ;
$(CPP_OBJS): src/protos/.done
