# Pulse reference ISS (docs/spec.md sections 1, 2, 3, 5 and 7).
#
#   make          build $(BUILD_DIR)/iss/iss
#   make test     build and run the unit tests
#   make bench    run a long firmware workload and report the simulation speed
#
# Plain C++17 with no dependencies, built with the host compiler.

include ../../mk/toolchain.mk

# Make's built-in default is g++; use the platform compiler unless the user
# picked one (Apple clang on macOS, g++ or clang on Linux).
ifeq ($(origin CXX),default)
  CXX := c++
endif
OPT       ?= -O2
CXXFLAGS  += -std=c++17 $(OPT) -Wall -Wextra -Werror -I. -I$(REPO_ROOT)/sw/common -MMD -MP

OUT      := $(BUILD_DIR)/iss
OBJ_DIR  := $(OUT)/obj
LIB_SRCS := commit_log.cpp cpu.cpp decode.cpp elf.cpp memory.cpp npu.cpp soc.cpp
LIB_OBJS := $(addprefix $(OBJ_DIR)/,$(LIB_SRCS:.cpp=.o))
TEST_OBJS := $(addprefix $(OBJ_DIR)/,test_main.o test_cpu.o test_npu.o)
ISS      := $(OUT)/iss
TEST_BIN := $(OUT)/iss_test

.PHONY: all test bench clean
all: $(ISS)

$(ISS): $(LIB_OBJS) $(OBJ_DIR)/main.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(TEST_BIN): $(LIB_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: test/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

test: $(TEST_BIN)
	$(TEST_BIN)

# The benchmark is ordinary firmware built with the sw/common runtime. The
# second run adds commit-log formatting (to /dev/null, so no disk I/O).
SW_COMMON := $(REPO_ROOT)/sw/common
BENCH_ELF := $(OUT)/bench/workload.elf

$(BENCH_ELF): bench/workload.c $(SW_COMMON)/crt0.S $(SW_COMMON)/lib.c $(SW_COMMON)/link.ld
	@mkdir -p $(dir $@)
	$(RV_CC) $(RV_FLAGS) -O2 -I$(SW_COMMON) $(RV_LDFLAGS) -T$(SW_COMMON)/link.ld \
	  $(SW_COMMON)/crt0.S $(SW_COMMON)/lib.c bench/workload.c -o $@

bench: $(ISS) $(BENCH_ELF)
	$(ISS) --stats --max-cycles 10000000000 $(BENCH_ELF)
	$(ISS) --stats --max-cycles 10000000000 --commit-log /dev/null $(BENCH_ELF)

clean:
	rm -rf $(OUT)

-include $(wildcard $(OBJ_DIR)/*.d)
