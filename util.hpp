#include <stdlib.h>

const int MAX_PACKET_SIZE = 524; // 512 bytes of payload + 12 bytes of header
const int MAX_PAYLOAD_SIZE = 512; // 512 bytes
const int MAX_SEQ_NUM = 102401;
const int MAX_ACK_NUM = 102401;
const double RETRANSMISSION_TIMER = 5.0e8; // 0.5 seconds
const int MIN_CWND = 512; // bytes
const int TIMEOUT_TIMER = 1e4; // 10 seconds or 10000 miliseconds
const int MAX_CWND = 51200; // bytes
const int RWND = 51200; // bytes
const int INIT_SS_THRESH = 10000; // bytes


uint32_t buf2int(const char *s, size_t a, size_t b) {
    int val = 0;

    while (a < b) {
        const unsigned char cur = s[a];
        unsigned int curInt =  cur;
        
        val = val * 256 + curInt;
        a ++;
    }

    return val;
}

void int2buf(char *buf, uint32_t num, size_t a, size_t b) {
    for (long i=b-1; i>=(int) a; i--) {
        buf[i] = num % 256;
        num /= 256;
    }
}
