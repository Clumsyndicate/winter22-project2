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

void processPacket(char* buf, ssize_t size) {
    auto flags = (uint16_t) convert_slice(buf, 10, 12);
    header_t h { 
        convert_slice(buf, 0, 4),
        convert_slice(buf, 4, 8),
        (uint16_t) convert_slice(buf, 8, 10),
        flags & MASK_A,
        flags & MASK_S,
        flags & MASK_F,
    };

    // debug
    cout << "Seq: " << h.seq << endl;
    cout << "Ack: " << h.ack << endl;
    cout << "Cid: " << h.cid << endl;
    cout << "A: " << h.a << " S: " << h.s << " F: " << h.f << endl;
}

void sendPacket(header_t header, char* payload) {
    
}