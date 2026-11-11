# The NPU driver, the software reference kernels and the small helpers shared
# by the programs in tests/ and bench/. Include it before common.mk:
#
#   PROG := mnist
#   SRCS := mnist.c
#   include ../../lib/lib.mk
#   include ../../common/common.mk
#
# Unused functions cost nothing: everything is linked with --gc-sections.

SW_LIB := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

SRCS         += $(SW_LIB)/npu.c $(SW_LIB)/sw_gemm.c $(SW_LIB)/bench.c
EXTRA_CFLAGS += -I$(SW_LIB)
