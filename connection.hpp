#include <stdlib.h>
#include <sys/socket.h>

#pragma once

enum class CState {
    ACK,
    STARTED,
    ENDED
};

struct Connection {

    CState state;
    uint16_t cid;

    struct sockaddr sender;

    explicit Connection() {}

    explicit Connection(uint16_t id, sockaddr saddr):
        state( CState::ACK ),
        cid(id),
        sender(saddr) {}
};