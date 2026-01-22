# atomsnap/example/urcu/Makefile

# ---------------- Toolchain ----------------
CXX    ?= g++
CC     ?= gcc
AR     ?= ar
RANLIB ?= ranlib

# ---------------- Paths ----------------
THIS_DIR    := $(strip $(abspath $(dir $(lastword $(MAKEFILE_LIST)))))
ATOMSNAP_DIR:= $(strip $(abspath $(THIS_DIR)/../..))

# atomsnap headers: atomsnap.h is expected here
ATOMSNAP_INC:= $(ATOMSNAP_DIR)

# Prebuilt libs in atomsnap/
ATOMSNAP_A  := $(ATOMSNAP_DIR)/libatomsnap.a
ATOMSNAP_SO := $(ATOMSNAP_DIR)/libatomsnap.so

# Optional local build from atomsnap.c (only if LOCAL_BUILD=1)
ATOMSNAP_SRC:= $(ATOMSNAP_DIR)/atomsnap.c
ATOMSNAP_OBJ:= $(ATOMSNAP_DIR)/atomsnap.o

# ---------------- Benchmark sources ----------------
BIN_DIR     := bin
TARGET      := $(BIN_DIR)/bench_all

BENCH_SRCS  := bench_all.cpp
BENCH_HDRS  := bench_common.hpp lf_pool.hpp

# ---------------- Build options ----------------
STD     := -std=c++20
OPT     ?= -O2
WARN    := -Wall -Wextra -Wpedantic
PTHREAD := -pthread

INCLUDES:= -I$(THIS_DIR) -I$(ATOMSNAP_INC)

CXXFLAGS?= $(STD) $(OPT) $(WARN) $(PTHREAD) $(INCLUDES)
CFLAGS  ?= -O2 -fPIC -I$(ATOMSNAP_INC)
LDFLAGS ?= $(PTHREAD)

# liburcu (memb flavor)
URCU_LIBS := -lurcu-memb -lurcu

# ---------------- Link mode ----------------
# MODE=auto|shared|static
MODE ?= auto

# If libs do not exist, you can set LOCAL_BUILD=1 to build libatomsnap.a from atomsnap.c
LOCAL_BUILD ?= 0

# Shared link uses rpath so you don't need LD_LIBRARY_PATH
RPATH := -Wl,-rpath,$(ATOMSNAP_DIR)

# ---------------- Internal (resolved later) ----------------
ATOMSNAP_DEP  :=
ATOMSNAP_LINK :=

# Decide linking strategy
ifeq ($(MODE),shared)
  ATOMSNAP_DEP  := $(ATOMSNAP_SO)
  ATOMSNAP_LINK := -L$(ATOMSNAP_DIR) -latomsnap $(RPATH)
else ifeq ($(MODE),static)
  ATOMSNAP_DEP  := $(ATOMSNAP_A)
  ATOMSNAP_LINK := $(ATOMSNAP_A)
else ifeq ($(MODE),auto)
  ifneq ("$(wildcard $(ATOMSNAP_SO))","")
    ATOMSNAP_DEP  := $(ATOMSNAP_SO)
    ATOMSNAP_LINK := -L$(ATOMSNAP_DIR) -latomsnap $(RPATH)
  else ifneq ("$(wildcard $(ATOMSNAP_A))","")
    ATOMSNAP_DEP  := $(ATOMSNAP_A)
    ATOMSNAP_LINK := $(ATOMSNAP_A)
  else
    ifeq ($(LOCAL_BUILD),1)
      ATOMSNAP_DEP  := $(ATOMSNAP_A)
      ATOMSNAP_LINK := $(ATOMSNAP_A)
    else
      $(error Neither libatomsnap.so nor libatomsnap.a found in $(ATOMSNAP_DIR). Set LOCAL_BUILD=1 or build the library first.)
    endif
  endif
else
  $(error Invalid MODE=$(MODE). Use MODE=auto|shared|static)
endif

# ---------------- Targets ----------------
.PHONY: all clean distclean help print-vars run

all: $(TARGET)

help:
	@echo "Targets:"
	@echo "  all        Build $(TARGET)"
	@echo "  clean      Remove benchmark binary"
	@echo "  distclean  clean + remove locally-built atomsnap.o/libatomsnap.a (only if LOCAL_BUILD=1 used)"
	@echo ""
	@echo "Options:"
	@echo "  MODE=auto|shared|static   (default: auto)"
	@echo "  LOCAL_BUILD=0|1           Build libatomsnap.a from ../../atomsnap.c if needed (default: 0)"
	@echo "  OPT=-O2|-O3|-Og           Optimization (default: -O2)"
	@echo ""
	@echo "Example:"
	@echo "  make MODE=shared"
	@echo "  make MODE=static"
	@echo "  make LOCAL_BUILD=1 MODE=auto"
	@echo ""
	@echo "Run:"
	@echo "  ./bin/bench_all --backend=urcu --readers=32 --writers=1 --duration=10 --cs-ns=200 --payload=64 --reclaim=async --updates-per-sec=0 --shards=1 --pin=1 --csv=1"

print-vars:
	@printf "THIS_DIR     = [%s]\n" "$(THIS_DIR)"
	@printf "ATOMSNAP_DIR = [%s]\n" "$(ATOMSNAP_DIR)"
	@printf "ATOMSNAP_A   = [%s]\n" "$(ATOMSNAP_A)"
	@printf "ATOMSNAP_SO  = [%s]\n" "$(ATOMSNAP_SO)"
	@printf "MODE         = [%s]\n" "$(MODE)"
	@printf "LOCAL_BUILD  = [%s]\n" "$(LOCAL_BUILD)"
	@printf "ATOMSNAP_DEP = [%s]\n" "$(ATOMSNAP_DEP)"
	@printf "ATOMSNAP_LINK= [%s]\n" "$(ATOMSNAP_LINK)"
	@ls -l "$(ATOMSNAP_DIR)" 2>/dev/null | sed -n '1,5p' || true
	@ls -l "$(ATOMSNAP_A)" "$(ATOMSNAP_SO)" 2>/dev/null || true

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Optional local build of libatomsnap.a from atomsnap.c
ifeq ($(LOCAL_BUILD),1)
$(ATOMSNAP_OBJ): $(ATOMSNAP_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(ATOMSNAP_A): $(ATOMSNAP_OBJ)
	$(AR) rcs $@ $<
	$(RANLIB) $@
endif

$(TARGET): $(BENCH_SRCS) $(BENCH_HDRS) $(ATOMSNAP_DEP) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDFLAGS) $(ATOMSNAP_LINK) $(URCU_LIBS)

run: $(TARGET)
	@$(TARGET) --backend=urcu --readers=8 --writers=1 --duration=3 --cs-ns=0 --payload=64 --reclaim=async --updates-per-sec=0 --shards=1 --pin=0 --csv=0

clean:
	@rm -rf $(BIN_DIR)

distclean: clean
ifeq ($(LOCAL_BUILD),1)
	@rm -f $(ATOMSNAP_OBJ) $(ATOMSNAP_A)
endif

