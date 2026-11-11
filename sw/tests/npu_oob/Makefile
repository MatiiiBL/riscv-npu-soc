PROG := npu_oob
SRCS := npu_oob.c
# The test uses the last bytes of DMEM, so the stack starts 256 bytes below
# the end of DMEM (0x1004_0000) instead of at it.
LDFLAGS += -Wl,--defsym=STACK_TOP=0x1003FF00
include ../../lib/lib.mk
include ../../common/common.mk
