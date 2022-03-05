#include <stdlib.h>

uint32_t buf2int(const char *s, size_t a, size_t b) {
    int val = 0;
    while (a < b) {
       val = val * 256 + s[a++];
    }
    return val;
}

void int2buf(char *buf, uint32_t num, size_t a, size_t b) {
    for (int i=b-1; i>=(int) a; i--) {
        buf[i] = num % 256;
        num /= 256;
    }
}