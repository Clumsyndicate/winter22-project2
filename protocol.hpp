#include "util.hpp"
#include <iostream>

using namespace std;


#pragma once
struct header_t {
    uint32_t seq;
    uint32_t ack;
    uint16_t cid;
    bool a,s,f;
};

#define MASK_A 0b100
#define MASK_S 0b010
#define MASK_F 0b001

header_t getHeader(char* buf, ssize_t size) {
    auto flags = (uint16_t) buf2int(buf, 10, 12);
    header_t h { 
        buf2int(buf, 0, 4),
        buf2int(buf, 4, 8),
        (uint16_t) buf2int(buf, 8, 10),
        (bool) (flags & MASK_A),
        (bool) (flags & MASK_S),
        (bool) (flags & MASK_F),
    };
    // debug
    cout << "Recv'd packet" << endl;
    cout << "Size: " << size << ", Seq: " << h.seq << ", Ack: " << h.ack << ", Cid: " << h.cid << endl;
    cout << "A: " << h.a << " S: " << h.s << " F: " << h.f << endl;
    return h;
}

string getPayload(char* buf, ssize_t size) {
    string payload {buf+12, (size_t) size-12};
    if (size > 0) {
        cout << "Payload: " << endl;
        cout << payload << endl << endl;
    }
    return payload;
}

size_t formatSendPacket(char *buf, header_t header, const char* payload, ssize_t payloadSize) {
    int2buf(buf, header.seq, 0, 4);
    int2buf(buf, header.ack, 4, 8);
    int2buf(buf, header.cid, 8, 10);
    buf[11] = MASK_A * header.a + MASK_S * header.s + MASK_F * header.f;
    
    if (payload != nullptr && payloadSize != 0)
        memcpy(buf + 12, payload, payloadSize);

    return payloadSize + 12;
}