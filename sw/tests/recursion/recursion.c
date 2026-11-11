/*
 * Deep and branchy recursion: naive Fibonacci (checked against the iterative
 * sequence), Ackermann, Towers of Hanoi and a recursive-descent expression
 * evaluator. Stresses calls, returns (jalr), stack traffic and the branch
 * predictor. Arguments come through volatiles so the compiler cannot fold
 * the results at build time.
 */
#include "soc.h"

static uint32_t fib(uint32_t n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static uint32_t ackermann(uint32_t m, uint32_t n) {
    if (m == 0) {
        return n + 1;
    }
    if (n == 0) {
        return ackermann(m - 1, 1);
    }
    return ackermann(m - 1, ackermann(m, n - 1));
}

/* Moves n discs from peg `from` to peg `to`; returns the number of moves and
 * tracks the disc count on each peg, which must end up all on `to`. */
static uint32_t hanoi(uint32_t n, int from, int to, int via, uint32_t pegs[3]) {
    if (n == 0) {
        return 0;
    }
    uint32_t moves = hanoi(n - 1, from, via, to, pegs);
    pegs[from]--;
    pegs[to]++;
    return moves + 1 + hanoi(n - 1, via, to, from, pegs);
}

/* expr := term (('+'|'-') term)*,  term := factor (('*'|'/') factor)*,
 * factor := number | '-' factor | '(' expr ')' */
static const char *cursor;

static int32_t expr(void);

static int32_t factor(void) {
    if (*cursor == '(') {
        cursor++;
        int32_t v = expr();
        cursor++; /* ')' */
        return v;
    }
    if (*cursor == '-') {
        cursor++;
        return -factor();
    }
    int32_t v = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        v = v * 10 + (*cursor++ - '0');
    }
    return v;
}

static int32_t term(void) {
    int32_t v = factor();
    while (*cursor == '*' || *cursor == '/') {
        char op = *cursor++;
        int32_t rhs = factor();
        v = op == '*' ? v * rhs : v / rhs;
    }
    return v;
}

static int32_t expr(void) {
    int32_t v = term();
    while (*cursor == '+' || *cursor == '-') {
        char op = *cursor++;
        int32_t rhs = term();
        v = op == '+' ? v + rhs : v - rhs;
    }
    return v;
}

static int32_t eval(const char *s) {
    cursor = s;
    return expr();
}

static volatile uint32_t fib_n = 24;
static volatile uint32_t ack_m = 2, ack_n = 3, ack_m2 = 3, ack_n2 = 4;
static volatile uint32_t hanoi_n = 12;

static int report(const char *what, int32_t got, int32_t want) {
    int ok = got == want;
    printf("%s = %d %s\n", what, got, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    int failures = 0;
    printf("recursion: fib, ackermann, hanoi, expression parser\n");

    uint32_t a = 0, b = 1, fib_ok = 1;
    for (uint32_t n = 0; n <= 20; n++) {
        fib_ok &= fib(n) == a;
        uint32_t next = a + b;
        a = b;
        b = next;
    }
    failures += report("fib(0..20) == iterative", (int32_t)fib_ok, 1);
    failures += report("fib(24)", (int32_t)fib(fib_n), 46368);

    failures += report("ackermann(2, 3)", (int32_t)ackermann(ack_m, ack_n), 9);
    failures += report("ackermann(3, 4)", (int32_t)ackermann(ack_m2, ack_n2), 125);

    uint32_t pegs[3] = {hanoi_n, 0, 0};
    uint32_t moves = hanoi(hanoi_n, 0, 2, 1, pegs);
    failures += report("hanoi(12) moves", (int32_t)moves, 4095);
    failures += report("hanoi(12) discs on target peg", (int32_t)pegs[2], 12);

    failures += report("(2+3)*(7-4)", eval("(2+3)*(7-4)"), 15);
    failures += report("100/7/2-3*-4", eval("100/7/2-3*-4"), 19);
    failures += report("((((1+2)*3)-4)*5)/-3", eval("((((1+2)*3)-4)*5)/-3"), -8);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
