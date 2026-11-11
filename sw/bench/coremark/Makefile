# EEMBC CoreMark (submodule in coremark/) with the bare-metal Pulse port in
# port/. ITERATIONS defaults to 10, which runs in a few million cycles; the
# objects do not depend on it, so change it with "make clean all ITERATIONS=n".

PROG := coremark

CM_DIR := coremark
SRCS := $(CM_DIR)/core_list_join.c $(CM_DIR)/core_main.c $(CM_DIR)/core_matrix.c \
        $(CM_DIR)/core_state.c $(CM_DIR)/core_util.c port/core_portme.c

ITERATIONS ?= 10
EXTRA_CFLAGS += -Iport -I$(CM_DIR) -DITERATIONS=$(ITERATIONS) \
                -DFLAGS_STR='"-O2 -march=rv32im_zicsr"'

include ../../lib/lib.mk
include ../../common/common.mk
