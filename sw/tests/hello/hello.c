#include "soc.h"

int main(void) {
    printf("Hello from Pulse!\n");
    uint32_t sum = 0;
    for (uint32_t i = 1; i <= 100; i++) {
        sum += i * i;
    }
    printf("sum of squares 1..100 = %u\n", sum);
    return sum == 338350 ? 0 : 1;
}
