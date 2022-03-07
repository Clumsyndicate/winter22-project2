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
#include <netdb.h>
#include <chrono>
#include <poll.h>
#include <list>

#include "protocol.hpp"

using namespace std;

#pragma once
struct packet_meta_t {
    header_t header;
    chrono::system_clock::time_point timestamp;
};

// Abort connection after 10 seconds of silence from server. Closes socket.
int abort_connection(int sock) {
    close(sock);
    exit(-1);
}

int main(int argc, const char * argv[]) {
    
    int portNumber, sock;
    struct sockaddr_in socketAddress;
    int bytes_sent;
    char buffer[200];
    
    ////////////////////////////////////////////////
    // Validate cml arguments.
    
    if (argc != 4) {
        std::cerr << "ERROR: Invalid number of arguments. Need IP address, port number, and filename to send";
        
        exit(1);
    }
    
    try {
        portNumber = std::stoi(argv[2]);
        if (portNumber <= 0) {
            std::cerr << "ERROR: Non-positive port number.";
            exit(1);
        }
    } catch (std::exception const &e) {
        std::cerr << "ERROR: Port number specified cannot be parsed.";
        exit(1);
    }

    ////////////////////////////////////////////////
    // validate hostname or IP address.
    
    struct addrinfo hints;
    struct addrinfo *res, *tmp;
    char host[256];

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;

    int ret = getaddrinfo(argv[1], argv[2], &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        perror("Failed to create socket.");
    }

    for (tmp = res; tmp != NULL; tmp = tmp->ai_next) {
        getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
        break;
    }

    freeaddrinfo(res);
    
    ////////////////////////////////////////////////
    // Initialize socket, send dummy data to server.
    
    // strcpy(buffer, "hello world!");
    string payload { "hello world!" };
    
    memset(&socketAddress, 0, sizeof socketAddress);

    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = inet_addr(host);
    socketAddress.sin_port = htons(portNumber);

    ////////////////////////////////////////////////
    // Format buffer
    // Send UDP packet src-ip=DEFAULT, src-port=DEFAULT, dst-ip=HOSTNAME-OR-IP, dst-port=PORT with SYN flag set,
    // Connection ID initialized to 0, Sequence Number set to 12345, and Acknowledgement Number set to 0

    header_t header {
        12345,
        0,
        0,
        false, true, false
    };

    auto packet_size = formatSendPacket(buffer, header, payload.c_str(), payload.size());
    cout << "Final packet size: " << packet_size << endl;
    bytes_sent = sendto(sock, buffer, packet_size, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }

    // Timeout after 10 seconds of inactivity from server
    struct pollfd pfd = {.fd = sock, .events = POLLIN};
    ret = poll(&pfd, 1, TIMEOUT_TIMER);
    if (ret <= 0) {
        // if polling error or timeout
        abort_connection(sock);
    }

    ////////////////////////////////////////////////
    // Listens for server response

    auto recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);

    if (recsize < 0) {
        std::cerr << "Negative receive size.";
        exit(1);
    }

    auto synHeader = getHeader(buffer, recsize);

    ////////////////////////////////////////////////
    // Send Ack packet (no payload)

    header_t ackHeader {
        synHeader.ack,
        synHeader.seq + 1,
        synHeader.cid, // use server-assigned connection ID
        true, false, false
    };

    packet_size = formatSendPacket(buffer, ackHeader, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packet_size, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }

    // Timeout after 10 seconds of inactivity from server
    ret = poll(&pfd, 1, TIMEOUT_TIMER);
    if (ret <= 0) {
        // if polling error or timeout
        abort_connection(sock);
    }

    ////////////////////////////////////////////////
    // Send payload with congestion control
    
    // Start the file transfer process. First open the file and get its size.
    FILE *fd = fopen(argv[3], "rb");
    fseek(fd, 0, SEEK_END);
    
    // Maximum file size 100MB. Using int is fine.
    int file_size = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    
    // Initialize parameters
    int cwnd = MIN_CWND;
    int ss_thresh = INIT_SS_THRESH;
    int acked_bytes = 0;
    int sent_bytes = 0;
    
    while (sent_bytes < file_size) {

        // Tracks how many packets are sent in this congestion window
        int packets_sent = 0;

        // Data structure for keeping track of timestamp
        list<packet_meta_t> packet_meta_list;
        
        // Congestion Window: send a total of cwnd bytes in several packets
        while (cwnd > 0) {

            // If have cwnd quota left but ran out of file, exit this loop and go to the ack loop
            if (sent_bytes >= file_size) {
                break;
            }

            // Expected payload size is either max UDP payload size or the remaining cwnd quota
            int payload_size = min(MAX_PAYLOAD_SIZE, cwnd);
            char buffer[MAX_PACKET_SIZE];
            char * payloadBuffer = new char [payload_size];
            bzero(payloadBuffer, payload_size);            

            // actual_size might be smaller than the expected payload_size, since we may reach the EOF
            int actual_size = fread(payloadBuffer, 1, payload_size, fd);
            if (actual_size < 0) {
                std::cerr << "ERROR: Failed to read from file.";
                close(sock);
                fclose(fd);
                exit(1);
            }

            // Construct header
            header_t payloadHeader {
                ackHeader.seq + packets_sent * MAX_PAYLOAD_SIZE, // account for prior packets send in the same window
                ackHeader.ack,
                ackHeader.cid,
                false, false, false
            };

            // Buffer will hold the entire packet (header + payload).
            packet_size = formatSendPacket(buffer, payloadHeader, payloadBuffer, actual_size);
            bytes_sent = sendto(sock, buffer, packet_size, 0, (struct sockaddr*)&socketAddress, sizeof socketAddress);
            
            // Record time in meta data struct
            auto currTimestamp = chrono::system_clock::now();
            packet_meta_t packet_meta {
                payloadHeader,
                currTimestamp
            };
            packet_meta_list.push_back(packet_meta);

            sent_bytes += actual_size;
            packets_sent += 1;
        }

        // Listens for ack. For each expected ack, adjst parameter if received. Retransmit (and adjust parameter) if timeout through polling after 0.5 seconds
        while (packets_sent > 0) {
            auto recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);

            if (recsize < 0) {
                std::cerr << "Negative receive size.";
                exit(1);
            }

            auto synHeader = getHeader(buffer, recsize);
            // TODO: Check ack number / seq number

            // Adjust parameters for successful transmission of one packet
            if (cwnd < ss_thresh) {
                cwnd += MAX_PACKET_SIZE;
            } else {
                cwnd += MAX_PACKET_SIZE * MAX_PACKET_SIZE / cwnd;
            }

            packets_sent -= 1;
            acked_bytes += MAX_PAYLOAD_SIZE;
        }  
    }

    fclose(fd);
    close(sock);
    return 0;
}
