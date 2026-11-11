#!/usr/bin/env python3
"""Constrained-random RV32IM program generator for differential testing.

    rvgen.py --seed S --length N -o out.S

Writes one self-contained assembly program (its own _start, no crt0 or
libc) for the RTL and the ISS to run side by side; tools/difftest.py then
compares their commit logs. Every generated program

- terminates: control flow only moves forward, except loops with a bounded
  counter and calls to straight-line subroutines that return;
- is deterministic: the text depends only on --seed and --length, never on
  the Python version or platform;
- stays inside the behavior docs/spec.md defines: loads and stores are
  aligned and go to a scratch buffer in DMEM or to fixed addresses (NPU and
  MMIO registers with word accesses, never NPU START; unbacked and
  region-edge addresses that read 0; DMEM's first and last words; IMEM,
  whose data-port stores are dropped); no cycle/time CSR reads (they differ
  between the RTL and the ISS by design), no traps;
- ends with `sw zero` to SIM_EXIT, or (about a quarter of the seeds) with
  ECALL, EBREAK, an illegal instruction or a fetch outside IMEM or from a
  misaligned pc. The header names the exit code both models must report.

The program is a sequence of "snippets" (an ALU op, a load followed by a
dependent use, a forward branch, a loop, ...). Branch targets are only ever
placed between snippets, so a branch can never jump into the middle of a
loop and skip its counter setup. Build with the firmware flags of
mk/toolchain.mk, sw/common/link.ld and -Isw/common (for soc_map.h).
"""

import argparse
import sys

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

SCRATCH_BYTES = 4096  # the base register points at its middle
MAX_LENGTH = 12000    # IMEM holds 16 Ki instructions; leave room for setup

R_OPS = ["add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and"]
I_OPS = ["addi", "slti", "sltiu", "xori", "ori", "andi"]
SHIFT_I_OPS = ["slli", "srli", "srai"]
SHIFT_R_OPS = ["sll", "srl", "sra"]
M_OPS = ["mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"]
BRANCH_OPS = ["beq", "bne", "blt", "bge", "bltu", "bgeu"]
LOADS = [("lb", 1), ("lbu", 1), ("lh", 2), ("lhu", 2), ("lw", 4)]
STORES = [("sb", 1), ("sh", 2), ("sw", 4)]

# Corner operands for M ops, shift amounts and register initialization.
CORNERS = [0, 1, 2, 3, MASK32, 0xFFFFFFFE, 0x80000000, 0x80000001,
           0x7FFFFFFF, 0x0000FFFF, 0x00008000, 0xFFFF8000, 31, 32]
IMM_CORNERS = [0, 1, -1, 2047, -2048, 31, 32]
SHIFT_AMOUNTS = [0, 31, 32, 33, 63, MASK32, 0xFFFFFFE0]

# instret/instreth and their machine aliases must match between the models;
# cycle and time never appear. minstret and the other CSRs are read-only or
# read 0 on Pulse, so writing them must have no effect.
COUNTER_CSRS = [0xC02, 0xC82, 0xB02, 0xB82]
WRITABLE_CSRS = [0xB02, 0xB82, 0x300, 0x340, 0x341]
ZERO_CSRS = [0x300, 0x301, 0x304, 0x305, 0x340, 0x342, 0xF11, 0xF14, 0x7C0]

FENCES = ["fence", "fence rw, rw", "fence iorw, iorw", "fence w, r",
          ".insn i 0x0f, 1, x0, x0, 0"]  # fence.i, spelled so every LLVM accepts it

# Fixed addresses outside the scratch buffer, written with the soc_map.h
# names so the memory map has one source. MMIO and NPU registers take word
# accesses only (spec 3). No store ever sets CTRL.START, so the NPU stays idle
# and every register read is timing-independent.
NPU_RW_REGS = ["NPU_A_ADDR", "NPU_B_ADDR", "NPU_C_ADDR", "NPU_M", "NPU_N", "NPU_K", "NPU_A_STRIDE",
               "NPU_B_STRIDE", "NPU_C_STRIDE", "NPU_QUANT", "NPU_BIAS_ADDR", "NPU_QMULT"]
NPU_READ_ONLY = ["NPU_STATUS", "NPU_CYCLES", "NPU_INFO", "NPU_CTRL"]  # CTRL reads 0
# Words in the MMIO and NPU regions that are not registers: undefined
# offsets, the last word of each window and the first words past it (which
# must not alias the registers, so a store "to SIM_EXIT" there is ignored).
ZERO_WORDS = ["MMIO_BASE + 0x8", "MMIO_BASE + 0xffc", "UART_TX + 0x1000", "SIM_EXIT + 0x1000",
              "LEDS + 0x1000", "NPU_BASE + 0x40", "NPU_BASE + 0xffc", "NPU_CTRL + 0x1000", "NPU_INFO + 0x1000"]
# Unbacked addresses that read 0 for any width and drop writes: IMEM through
# the data port, just past IMEM and DMEM, and the unmapped regions.
ZERO_ANY = ["IMEM_BASE", "IMEM_BASE + IMEM_SIZE - 4", "IMEM_BASE + IMEM_SIZE", "DMEM_BASE + DMEM_SIZE",
            "0x40000000", "0x7ffffffc", "-0x80000000", "-4"]
# The first and last words of DMEM (the scratch buffer starts at the first).
DMEM_EDGES = ["DMEM_BASE", "DMEM_BASE + DMEM_SIZE - 4"]

# How a program ends, with the exit code both models must report (spec 7.1).
# Most end with SIM_EXIT = 0; the rest halt so those paths are compared too.
ENDINGS = [(75, "exit"), (5, "ecall"), (5, "ebreak"), (5, "illegal"), (5, "fetch_outside"),
           (5, "fetch_misaligned")]
EXIT_CODES = {"exit": 0, "ecall": 2, "ebreak": 2, "illegal": 3, "fetch_outside": 3, "fetch_misaligned": 3}
# Reserved encodings: all zeros, all ones, MRET and WFI (no privileged mode),
# a custom-0 opcode, and an RV64-only shift amount.
ILLEGAL_WORDS = [0x00000000, 0xFFFFFFFF, 0x30200073, 0x10500073, 0x0000000B, 0x02009093]
# Fetch targets outside IMEM: just past it, in DMEM, and the top of memory.
BAD_FETCH_TARGETS = ["IMEM_BASE + IMEM_SIZE", "DMEM_BASE", "-4"]


class Rng:
    """splitmix64. Python only promises a stable random.random() sequence
    across versions, so the generator carries its own small PRNG."""

    def __init__(self, seed):
        self.state = seed & MASK64

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def below(self, n):
        return (self.next() * n) >> 64

    def range(self, lo, hi):
        """Uniform integer in [lo, hi]."""
        return lo + self.below(hi - lo + 1)

    def chance(self, percent):
        return self.below(100) < percent

    def choice(self, seq):
        return seq[self.below(len(seq))]

    def weighted(self, table):
        total = sum(weight for weight, _ in table)
        r = self.below(total)
        for weight, value in table:
            if r < weight:
                return value
            r -= weight
        raise AssertionError("unreachable")

    def u32(self):
        return self.next() & MASK32


def signed32(v):
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


def x(r):
    return f"x{r}"


class Generator:
    def __init__(self, seed, length, ending=None):
        self.rng = Rng(seed)
        self.seed = seed
        self.length = length
        # Drawn first and always, so the ending does not depend on --length
        # and forcing one with --ending leaves the rest of the program as is.
        drawn = self.rng.weighted(ENDINGS)
        self.ending = ending or drawn
        self.lines = []
        self.count = 0          # instructions emitted so far (all blocks)
        self.label_ids = {}
        regs = list(range(1, 32))
        self.base = self.rng.choice(regs)
        regs.remove(self.base)
        self.counter = self.rng.choice(regs)
        regs.remove(self.counter)
        # Registers random code may write. The scratch base and the loop
        # counter are reserved for the whole program; a subroutine also
        # reserves its link register while its body is generated.
        self.writable = regs
        self.reserved = set()
        self.recent = []        # recently written registers, preferred as sources
        self.pending = []       # [label, snippets left] of forward branches
        self.hot = []           # recently used scratch offsets, to create RAW hazards
        self.subs = []          # (name, link register, lines)

    # -- output -----------------------------------------------------------

    def emit(self, text):
        self.lines.append("    " + text)
        self.count += 1

    def label(self, name):
        self.lines.append(f"{name}:")

    def new_label(self, kind):
        n = self.label_ids.get(kind, 0)
        self.label_ids[kind] = n + 1
        return f"{kind}{n}"

    # -- operands ---------------------------------------------------------

    def dest(self, allow_x0=True):
        if allow_x0 and self.rng.chance(4):
            return 0
        r = self.rng.choice([r for r in self.writable if r not in self.reserved])
        self.recent = ([r] + [q for q in self.recent if q != r])[:4]
        return r

    def src(self):
        """Any register (x0, the base and the counter included), biased
        toward the ones just written to make back-to-back dependencies."""
        if self.recent and self.rng.chance(50):
            return self.rng.choice(self.recent)
        return self.rng.below(32)

    def value(self):
        kind = self.rng.below(10)
        if kind < 4:
            return self.rng.choice(CORNERS)
        if kind < 5:
            return self.rng.range(-16, 16) & MASK32
        return self.rng.u32()

    def imm12(self):
        if self.rng.chance(25):
            return self.rng.choice(IMM_CORNERS)
        return self.rng.range(-2048, 2047)

    def shamt(self):
        return self.rng.choice([0, 31, self.rng.range(1, 30)])

    def li(self, r, v):
        self.emit(f"li {x(r)}, {signed32(v)}")

    def offset(self, width):
        """A width-aligned offset from the base register that stays inside
        the scratch buffer, often reusing a recently touched word."""
        if self.hot and self.rng.chance(40):
            word = self.rng.choice(self.hot)
            off = word + self.rng.below(4 // width) * width
        else:
            off = self.rng.range(-2048 // width, (2048 - width) // width) * width
        self.hot = ([off & ~3] + [w for w in self.hot if w != off & ~3])[:8]
        return off

    # -- snippets ---------------------------------------------------------

    def alu_rr(self):
        self.emit(f"{self.rng.choice(R_OPS)} {x(self.dest())}, {x(self.src())}, {x(self.src())}")

    def alu_ri(self):
        if self.rng.chance(30):
            op, imm = self.rng.choice(SHIFT_I_OPS), self.shamt()
        else:
            op, imm = self.rng.choice(I_OPS), self.imm12()
        self.emit(f"{op} {x(self.dest())}, {x(self.src())}, {imm}")

    def upper(self):
        op = self.rng.choice(["lui", "auipc"])
        imm = self.rng.choice([0, 0xFFFFF, 0x80000, self.rng.below(1 << 20)])
        self.emit(f"{op} {x(self.dest())}, {imm}")

    def use(self, r):
        """An instruction that consumes r immediately (a RAW hazard)."""
        kind = self.rng.below(4)
        if kind == 0:
            self.emit(f"{self.rng.choice(R_OPS)} {x(self.dest())}, {x(r)}, {x(self.src())}")
        elif kind == 1:
            self.emit(f"{self.rng.choice(I_OPS)} {x(self.dest())}, {x(r)}, {self.imm12()}")
        elif kind == 2:
            self.forward_branch(r, self.src())
        else:
            op, width = self.rng.choice(STORES)
            self.emit(f"{op} {x(r)}, {self.offset(width)}({x(self.base)})")

    def muldiv(self):
        if self.rng.chance(50):
            a, b = self.dest(False), self.dest(False)
            self.li(a, self.rng.choice(CORNERS))
            self.li(b, self.rng.choice(CORNERS) if self.rng.chance(70) else self.rng.u32())
        else:
            a, b = self.src(), self.src()
        rd = self.dest()
        self.emit(f"{self.rng.choice(M_OPS)} {x(rd)}, {x(a)}, {x(b)}")
        if self.rng.chance(40):
            self.emit(f"{self.rng.choice(M_OPS)} {x(self.dest())}, {x(rd)}, {x(self.src())}")
        elif self.rng.chance(50):
            self.use(rd)

    def shift_reg(self):
        amount = self.dest(False)
        self.li(amount, self.rng.choice(SHIFT_AMOUNTS + [self.rng.u32()]))
        self.emit(f"{self.rng.choice(SHIFT_R_OPS)} {x(self.dest())}, {x(self.src())}, {x(amount)}")

    def load(self):
        op, width = self.rng.choice(LOADS)
        rd = self.dest()
        self.emit(f"{op} {x(rd)}, {self.offset(width)}({x(self.base)})")
        if self.rng.chance(50):
            self.use(rd)

    def store(self):
        op, width = self.rng.choice(STORES)
        if self.rng.chance(40):
            data = self.dest(False)
            self.emit(f"{self.rng.choice(R_OPS)} {x(data)}, {x(self.src())}, {x(self.src())}")
        else:
            data = self.src()
        off = self.offset(width)
        self.emit(f"{op} {x(data)}, {off}({x(self.base)})")
        if self.rng.chance(40):
            # Read the same word back with a different width.
            lop, lwidth = self.rng.choice(LOADS)
            loff = (off & ~3) + self.rng.below(4 // lwidth) * lwidth
            self.emit(f"{lop} {x(self.dest())}, {loff}({x(self.base)})")

    def computed_address(self):
        """addi into a temporary, then a load or store through it."""
        if self.rng.chance(50):
            op, width = self.rng.choice(LOADS)
        else:
            op, width = self.rng.choice(STORES)
        final = self.offset(width)
        delta = self.rng.range(max(-2048, final - 2047), min(2047, final + 2048))
        t = self.dest(False)
        self.emit(f"addi {x(t)}, {x(self.base)}, {delta}")
        reg = self.dest() if op.startswith("l") else self.src()
        self.emit(f"{op} {x(reg)}, {final - delta}({x(t)})")

    def pointer_chase(self):
        """Load an address from memory and use it at once (load-use into
        the address of the next access)."""
        slot = self.offset(4)
        t = self.dest(False)
        self.emit(f"sw {x(self.base)}, {slot}({x(self.base)})")
        self.emit(f"lw {x(t)}, {slot}({x(self.base)})")
        op, width = self.rng.choice(LOADS)
        self.emit(f"{op} {x(self.dest())}, {self.offset(width)}({x(t)})")

    def forward_branch(self, a=None, b=None):
        if a is None:
            mode = self.rng.below(4)
            a = self.src()
            if mode == 0:
                b = a                       # always equal
            elif mode == 1:
                b = 0
            elif mode == 2:
                b = self.dest(False)        # freshly computed operand
                self.emit(f"{self.rng.choice(I_OPS)} {x(b)}, {x(self.src())}, {self.imm12()}")
            else:
                b = self.src()
        if self.rng.chance(50):
            a, b = b, a
        target = self.forward_label()
        self.emit(f"{self.rng.choice(BRANCH_OPS)} {x(a)}, {x(b)}, {target}")

    def jal(self):
        self.emit(f"jal {x(self.dest())}, {self.forward_label()}")

    def jalr(self):
        target = self.forward_label()
        t = self.dest(False)
        kind = self.rng.below(5)
        if kind == 0:
            self.emit(f"lla {x(t)}, {target}")
            self.emit(f"jalr {x(self.dest())}, 0({x(t)})")
        elif kind == 1:
            off = self.rng.range(-64, 64) * 4
            self.emit(f"lla {x(t)}, {target}{-off:+d}")
            self.emit(f"jalr {x(self.dest())}, {off}({x(t)})")
        elif kind == 2:
            # JALR clears bit 0 of the target.
            self.emit(f"lla {x(t)}, {target}")
            self.emit(f"addi {x(t)}, {x(t)}, 1")
            self.emit(f"jalr {x(self.dest())}, 0({x(t)})")
        elif kind == 3:
            # Target loaded from memory: load-use into JALR.
            slot = self.offset(4)
            u = self.dest(False)
            self.emit(f"lla {x(t)}, {target}")
            self.emit(f"sw {x(t)}, {slot}({x(self.base)})")
            self.emit(f"lw {x(u)}, {slot}({x(self.base)})")
            self.emit(f"jalr {x(self.dest())}, 0({x(u)})")
        else:
            # rd == rs1: the link must not clobber the target first.
            self.emit(f"lla {x(t)}, {target}")
            self.emit(f"jalr {x(t)}, 0({x(t)})")

    def csr(self):
        kind = self.rng.below(10)
        rd = self.dest()
        if kind < 5:
            csr = self.rng.choice(COUNTER_CSRS)
            form = self.rng.below(4)
            if form == 0:
                self.emit(f"csrrs {x(rd)}, {csr:#x}, x0")
            elif form == 1:
                self.emit(f"csrrc {x(rd)}, {csr:#x}, x0")
            else:
                self.emit(f"{'csrrsi' if form == 2 else 'csrrci'} {x(rd)}, {csr:#x}, 0")
        elif kind < 8:
            csr = self.rng.choice(WRITABLE_CSRS)
            if self.rng.chance(50):
                self.emit(f"{self.rng.choice(['csrrw', 'csrrs', 'csrrc'])} {x(rd)}, {csr:#x}, {x(self.src())}")
            else:
                op = self.rng.choice(["csrrwi", "csrrsi", "csrrci"])
                self.emit(f"{op} {x(rd)}, {csr:#x}, {self.rng.range(0, 31)}")
        else:
            self.emit(f"csrrs {x(rd)}, {self.rng.choice(ZERO_CSRS):#x}, x0")
        if self.rng.chance(50):
            self.use(rd)

    def chain(self):
        """Back-to-back dependent ALU and multiply ops."""
        prev = self.src()
        for _ in range(self.rng.range(2, 5)):
            rd = self.dest()
            if self.rng.chance(25):
                op = self.rng.choice(M_OPS)
            else:
                op = self.rng.choice(R_OPS)
            other = self.src()
            if self.rng.chance(50):
                self.emit(f"{op} {x(rd)}, {x(prev)}, {x(other)}")
            else:
                self.emit(f"{op} {x(rd)}, {x(other)}, {x(prev)}")
            prev = rd

    def uart(self):
        t = self.dest(False)
        self.emit(f"li {x(t)}, MMIO_BASE")
        if self.rng.chance(70):
            self.emit(f"sw {x(self.src())}, 0({x(t)})")   # UART_TX
        else:
            self.emit(f"lw {x(self.dest())}, 4({x(t)})")  # UART_STATUS

    def device(self):
        """A load or store at a fixed address outside the scratch buffer."""
        t = self.dest(False)
        kind = self.rng.below(7)
        if kind == 0:
            # An NPU register reads back the full 32 bits written (spec 5).
            self.emit(f"li {x(t)}, {self.rng.choice(NPU_RW_REGS)}")
            self.emit(f"sw {x(self.src())}, 0({x(t)})")
            if self.rng.chance(60):
                self.emit(f"lw {x(self.dest())}, 0({x(t)})")
        elif kind == 1:
            self.emit(f"li {x(t)}, {self.rng.choice(NPU_RW_REGS + NPU_READ_ONLY)}")
            rd = self.dest()
            self.emit(f"lw {x(rd)}, 0({x(t)})")
            if self.rng.chance(40):
                self.use(rd)
        elif kind == 2:
            # LEDS keeps only its low 8 bits (spec 3).
            self.emit(f"li {x(t)}, LEDS")
            self.emit(f"sw {x(self.src())}, 0({x(t)})")
            self.emit(f"lw {x(self.dest())}, 0({x(t)})")
        elif kind == 3:
            # CTRL, or a word that reads 0 and ignores writes, always written
            # with bit 0 clear: on CTRL or a CTRL alias that bit is START.
            addr = self.rng.choice(["NPU_CTRL"] + ZERO_WORDS)
            self.emit(f"li {x(t)}, {addr}")
            if addr != "NPU_CTRL" and self.rng.chance(50):
                self.emit(f"lw {x(self.dest())}, 0({x(t)})")
            else:
                # The value register must not be the one holding the address.
                outer = self.reserved
                self.reserved = outer | {t}
                value = self.dest(False)
                self.reserved = outer
                self.emit(f"andi {x(value)}, {x(self.src())}, -2")
                self.emit(f"sw {x(value)}, 0({x(t)})")
        elif kind in (4, 5):
            # Any aligned width at an unbacked address or a DMEM edge word.
            self.emit(f"li {x(t)}, {self.rng.choice(ZERO_ANY + DMEM_EDGES)}")
            if kind == 4:
                op, width = self.rng.choice(LOADS)
                reg = self.dest()
            else:
                op, width = self.rng.choice(STORES)
                reg = self.src()
            self.emit(f"{op} {x(reg)}, {self.rng.below(4 // width) * width}({x(t)})")
        else:
            # A store over the instruction right after it. IMEM is not
            # writable from the data port, so the program runs on unchanged.
            self.emit(f"auipc {x(t)}, 0")
            self.emit(f"sw {x(self.src())}, 8({x(t)})")

    def fence(self):
        self.emit(self.rng.choice(FENCES))

    def loop(self):
        """A short backward loop; the body cannot write the counter."""
        iters = self.rng.range(1, 5)
        head = self.new_label("loop")
        c = x(self.counter)
        count_up = self.rng.chance(30)
        self.emit(f"li {c}, {-iters if count_up else iters}")
        self.label(head)
        self.block(self.rng.range(2, 6), "loop")
        if count_up:
            self.emit(f"addi {c}, {c}, 1")
            self.emit(f"{self.rng.choice(['blt', 'bne'])} {c}, x0, {head}")
        else:
            self.emit(f"addi {c}, {c}, -1")
            close = self.rng.choice([f"bne {c}, x0", f"blt x0, {c}", f"bltu x0, {c}"])
            self.emit(f"{close}, {head}")

    def call(self):
        if not self.subs or (len(self.subs) < 4 and self.rng.chance(30)):
            self.make_sub()
        name, link, _ = self.rng.choice(self.subs)
        if self.rng.chance(50):
            self.emit(f"jal {x(link)}, {name}")
        else:
            t = self.dest(False)
            self.emit(f"lla {x(t)}, {name}")
            self.emit(f"jalr {x(link)}, 0({x(t)})")

    def make_sub(self):
        """A straight-line subroutine, placed after the exit store so it is
        only reached by a call."""
        name = self.new_label("sub")
        link = self.rng.choice(self.writable)
        saved = (self.lines, self.reserved, self.recent)
        self.lines, self.reserved, self.recent = [], {link}, []
        self.label(name)
        self.block(self.rng.range(2, 6), "sub")
        self.emit(f"jalr x0, 0({x(link)})")
        self.subs.append((name, link, self.lines))
        self.lines, self.reserved, self.recent = saved

    SNIPPETS = [
        (14, alu_rr), (12, alu_ri), (3, upper), (9, muldiv), (3, shift_reg),
        (10, load), (9, store), (3, computed_address), (2, pointer_chase),
        (9, forward_branch), (3, jal), (3, jalr), (3, csr), (6, chain),
        (1, uart), (3, device), (1, fence),
    ]

    def snippet(self, context):
        table = list(self.SNIPPETS)
        if context == "top":
            table += [(4, Generator.loop), (3, Generator.call)]
        elif context == "loop":
            table += [(3, Generator.call)]
        self.rng.weighted(table)(self)

    # -- control flow -----------------------------------------------------

    def forward_label(self):
        label = self.new_label("fwd")
        self.pending.append([label, self.rng.range(0, 4)])
        return label

    def place_due_labels(self):
        waiting = []
        for item in self.pending:
            if item[1] == 0:
                self.label(item[0])
            else:
                item[1] -= 1
                waiting.append(item)
        self.pending = waiting

    def flush_labels(self):
        for name, _ in self.pending:
            self.label(name)
        self.pending = []

    def block(self, snippets, context):
        """Snippets whose forward branches all land inside the block."""
        outer = self.pending
        self.pending = []
        for _ in range(snippets):
            self.snippet(context)
            self.place_due_labels()
        self.flush_labels()
        self.pending = outer

    # -- program ----------------------------------------------------------

    def finish(self):
        """The program's last instructions: SIM_EXIT = 0 or a halt."""
        t = self.rng.choice(self.writable)
        if self.ending == "exit":
            self.emit(f"li {x(t)}, SIM_EXIT")
            self.emit(f"sw x0, 0({x(t)})")
        elif self.ending in ("ecall", "ebreak"):
            self.emit(self.ending)
        elif self.ending == "illegal":
            self.emit(f".word {self.rng.choice(ILLEGAL_WORDS):#010x}")
        elif self.ending == "fetch_outside":
            self.emit(f"li {x(t)}, {self.rng.choice(BAD_FETCH_TARGETS)}")
            self.emit(f"jalr {x(self.dest())}, 0({x(t)})")
        else:
            # JALR clears only bit 0 of the target, so offset 2 leaves it
            # misaligned; the core halts at the target (spec 2).
            self.emit(f"lla {x(t)}, halt")
            self.emit(f"jalr {x(self.dest())}, 2({x(t)})")
        self.label("halt")
        self.emit("j halt")

    def program(self):
        self.lines.append(f"    lla {x(self.base)}, scratch + {SCRATCH_BYTES // 2}")
        for r in range(1, 32):
            if r != self.base:
                self.li(r, self.value())
        while self.count < self.length:
            self.snippet("top")
            self.place_due_labels()
        self.flush_labels()
        self.finish()
        for _, _, lines in self.subs:
            self.lines.extend(lines)

        # The buffer contents come from their own stream, so a shorter
        # --length yields a prefix of the same program with the same data:
        # a failing seed can be shrunk by bisecting --length.
        data_rng = Rng(self.seed ^ 0xD1B54A32D192ED03)
        words = [data_rng.u32() for _ in range(SCRATCH_BYTES // 4)]
        data = []
        for i in range(0, len(words), 8):
            data.append("    .word " + ", ".join(f"{w:#010x}" for w in words[i:i + 8]))

        header = [
            f"/* Generated by tools/rvgen/rvgen.py --seed {self.seed} --length {self.length}.",
            f" * Reserved: {x(self.base)} = scratch + {SCRATCH_BYTES // 2}, "
            f"{x(self.counter)} = loop counter.",
            f" * Ends with {self.ending}: both models must exit with code {EXIT_CODES[self.ending]}. */",
            '#include "soc_map.h"',
            "",
            '    .section .text.init, "ax"',
            "    .globl _start",
            "_start:",
        ]
        footer = ["", "    .data", "    .balign 4", "scratch:"] + data
        return "\n".join(header + self.lines + footer) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seed", type=int, required=True, help="random seed (any integer)")
    parser.add_argument("--length", type=int, default=2000,
                        help="approximate number of instructions (default 2000)")
    parser.add_argument("--ending", choices=list(EXIT_CODES),
                        help="how the program ends (default: chosen by the seed)")
    parser.add_argument("-o", "--output", default="-", help="output .S file (default stdout)")
    args = parser.parse_args()
    if not 1 <= args.length <= MAX_LENGTH:
        parser.error(f"--length must be between 1 and {MAX_LENGTH}")

    text = Generator(args.seed, args.length, args.ending).program()
    if args.output == "-":
        sys.stdout.write(text)
    else:
        with open(args.output, "w", encoding="ascii") as f:
            f.write(text)


if __name__ == "__main__":
    main()
