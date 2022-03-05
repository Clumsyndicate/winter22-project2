#include <stdlib.h>

uint32_t convert_slice(const char *s, size_t a, size_t b) {
    int val = 0;
    while (a < b) {
       val = val * 256 + s[a++];
    }
    return val;
}