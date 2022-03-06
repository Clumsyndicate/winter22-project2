#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <csignal>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <unordered_map>

#include "protocol.hpp"
#include "connection.hpp"

using namespace std;

int sock;

unordered_map<int, Connection> connections;

void signalHandler(int sig) {
    // todo: clean up, graceful exit.    
    close(sock);

    // exit with code zero, as specified by the spec.
    exit(0);
}

int main(int argc, const char * argv[]) {
    cout << "Hi, welcome to this dysfunctional udp server" << endl;
    int portNumber;
    struct sockaddr_in socketAddress;
    char buffer[1024];
    ssize_t recsize;
    socklen_t fromlen;
    
    // Validate cml arguments. Port number needs to be postive integer.
    // Destination directory is guaranteed to be correct.
    
    if (argc != 3) {
        std::cerr << "ERROR: Invalid number of arguments. Need port number and destination directory.";
        exit(1);
    }
    
    try {
        portNumber = std::stoi(argv[1]);
        if (portNumber <= 0) {
            std::cerr << "ERROR: Non-positive port number.";
            exit(1);
        }
    } catch (std::exception const &e) {
        std::cerr << "ERROR: Port number specified cannot be parsed.";
        exit(1);
    }
    
    // Register signal handler to handle SIGQUIT and SIGTERM
    
    signal(SIGQUIT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Initial like socket, bind, and receive.
    
    memset(&socketAddress, 0, sizeof socketAddress);
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    socketAddress.sin_port = htons(portNumber);
    fromlen = sizeof socketAddress;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bind(sock, (struct sockaddr *)&socketAddress, sizeof socketAddress) == -1) {
        std::cerr << "ERROR: Failed to bind socket.";
        close(sock);
        exit(1);
    }
    // Uncomment to make recvfrom nonblocking
    /*
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout); */

    for (;;) {
        recsize = recvfrom(sock, (void*)buffer, sizeof buffer, 0, (struct sockaddr*)&socketAddress, &fromlen);
        if (recsize < 0) {
            std::cerr << "Negative receive size.";
            exit(1);
        }
        printf("recsize: %d\n ", (int)recsize);
        printf("datagram: %.*s\n", (int)recsize, buffer);

        auto header = getHeader(buffer, recsize);
        auto payload = getPayload(buffer, recsize);


        if (header.s) {
            // After receiving a packet with SYN flag, the server should create state for the connection ID and proceed with 3-way handshake for this connection. Server should use 4321 as initial sequence number.



            
        }
    }
    
    return 0;
}
