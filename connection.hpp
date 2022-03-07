#include <stdlib.h>
#include <sys/socket.h>
#include <map>
#include <string> 

#pragma once

using namespace std;

enum class CState {
    ACK,
    STARTED,
    ENDED
};

struct DataPacket {
    uint32_t seq;
    uint32_t size;
    string payload;

    DataPacket(uint32_t s, uint32_t si, string p):
        seq(s),
        size(si),
        payload(std::move(p)) {}
};

struct Connection {

    CState state;
    uint16_t cid;

    struct sockaddr sender;

    // Out of order packets ordered 
    map<uint32_t, DataPacket> queue;
    
    uint32_t head;

    FILE* file;

    explicit Connection() {}

    explicit Connection(uint16_t id, sockaddr saddr):
        state( CState::ACK ),
        cid(id),
        sender(saddr),
        head(12346),
        file(nullptr) {}
};