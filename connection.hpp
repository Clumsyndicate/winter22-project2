#include <stdlib.h>

#pragma once

enum class CState {
    ACK,
    STARTED,
    ENDED
};

struct Connection {

    CState state;
    uint16_t cid;

    Connection(uint16_t id):
        state( CState::ACK ),
        cid(id) {}
};