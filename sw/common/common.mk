# Build rules for one firmware program.
#
# Usage from a program directory's Makefile:
#   PROG := hello
#   SRCS := hello.c
#   include ../../common/common.mk
#
# Produces $(BUILD_DIR)/sw/<PROG>.elf plus .dis (disassembly) and .bin.

include $(dir $(lastword $(MAKEFILE_LIST)))../../mk/toolchain.mk

SW_COMMON := $(REPO_ROOT)/sw/common
SW_OUT    := $(BUILD_DIR)/sw
OBJ_DIR   := $(SW_OUT)/obj/$(PROG)

# -MMD -MP write a .d file of header dependencies next to each object, so
# editing any included header (sw/common, sw/lib or the program's own)
# rebuilds what uses it.
OPT      ?= -O2
DEPFLAGS := -MMD -MP
CFLAGS   += $(RV_FLAGS) $(OPT) -g -std=c11 -Wall -Wextra -I$(SW_COMMON) $(EXTRA_CFLAGS) $(DEPFLAGS)
ASFLAGS  += $(RV_FLAGS) -I$(SW_COMMON) $(DEPFLAGS)
LDFLAGS  += $(RV_FLAGS) $(RV_LDFLAGS) -T$(SW_COMMON)/link.ld

COMMON_SRCS := $(SW_COMMON)/crt0.S $(SW_COMMON)/lib.c
ALL_SRCS    := $(COMMON_SRCS) $(SRCS)
OBJS        := $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(notdir $(basename $(ALL_SRCS)))))
ELF         := $(SW_OUT)/$(PROG).elf

vpath %.c $(sort $(dir $(abspath $(ALL_SRCS))))
vpath %.S $(sort $(dir $(abspath $(ALL_SRCS))))

.PHONY: all clean
all: $(ELF) $(ELF:.elf=.dis) $(ELF:.elf=.bin)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(RV_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.S | $(OBJ_DIR)
	$(RV_CC) $(ASFLAGS) -c $< -o $@

$(ELF): $(OBJS) $(SW_COMMON)/link.ld
	$(RV_CC) $(LDFLAGS) $(OBJS) -o $@
	@$(RV_SIZE) $@

%.dis: %.elf
	$(RV_OBJDUMP) -d $< > $@

%.bin: %.elf
	$(RV_OBJCOPY) -O binary $< $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(ELF) $(ELF:.elf=.dis) $(ELF:.elf=.bin)

-include $(OBJS:.o=.d)
