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
#include <unordered_map>

#include "protocol.hpp"

using namespace std;


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
    int sent_bytes = 0;
    int sent_through_bytes = 0;   
    bool retransmission_triggered = false;

    // Set socket to be non-blocking for parallel packet timeout monitoring
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);
    
    while (sent_through_bytes < file_size) {

        // Reposition the file descriptor to be at a certain offset (for retransmission)
        fseek(fd, sent_through_bytes, SEEK_SET);

        // Reset retransmission flag
        retransmission_triggered = false;

        // Tracks how many packets are sent in this congestion window
        int packets_sent = 0;

        // Data structure for keeping track of timestamp
        unordered_map<int, chrono::system_clock::time_point> acknum_time_map;
        
        // Congestion Window: send a total of cwnd bytes in several packets
        while (cwnd > 0) {

            // If have cwnd quota left but ran out of file, exit this loop and go to the ack loop
            if (sent_bytes >= file_size) {
                break;
            }

            // Expected payload size is either max UDP payload size or the remaining cwnd quota
            int expected_payload_size = min(MAX_PAYLOAD_SIZE, cwnd);
            char buffer[MAX_PACKET_SIZE];
            char * payloadBuffer = new char [expected_payload_size];
            bzero(payloadBuffer, expected_payload_size);            

            // actual_size might be smaller than the expected payload_size, since we may reach the EOF
            int actual_payload_size = fread(payloadBuffer, 1, expected_payload_size, fd);
            if (actual_payload_size < 0) {
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
            packet_size = formatSendPacket(buffer, payloadHeader, payloadBuffer, actual_payload_size);
            sendto(sock, buffer, packet_size, 0, (struct sockaddr*)&socketAddress, sizeof socketAddress);
            
            // Record time in meta data struct
            int expected_acknum = payloadHeader.seq + actual_payload_size;
            acknum_time_map[expected_acknum] = chrono::system_clock::now();

            sent_bytes += actual_payload_size;
            cwnd -= actual_payload_size;
            packets_sent += 1;
        }

        // Listens for ack. For each expected ack, adjst parameter if received.
        // Retransmit (and adjust parameter) if timeout through polling after 0.5 seconds
        // Keep iterating until the list of outstanding expected ack is gone
        while (!acknum_time_map.empty()) {

            int received_size = 0;

            // Check if any packet timeout, then check if received anything from socket
            while (true) {
                for (auto i : acknum_time_map) {
                    if (chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - i.second).count() >= RETRANSMISSION_TIMER) {
                        // If detected timeout
                        acknum_time_map.clear();
                        sent_through_bytes = 0;
                        ss_thresh = cwnd / 2;
                        cwnd = MIN_CWND;
                        retransmission_triggered = true;
                        break;
                    }
                }

                // Break out of loop if triggered retransmission
                if (retransmission_triggered) {
                    break;
                }

                // Check if received something
                received_size = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0); // TODO: problem, what if packets queue up
                if (received_size < 0) {
                    std::cerr << "Negative receive size.";
                    exit(1);
                } else if (received_size > 0) {
                    // Received something
                    break;
                }
            }

            if (retransmission_triggered) {
                break;
            }

            auto ackHeader = getHeader(buffer, received_size);
            int cum_ack = ackHeader.ack;

            if (acknum_time_map.find(cum_ack) == acknum_time_map.end()) {
                // ack num not expected
                fclose(fd);
                abort_connection(sock);
            } else {
                // pop all records with expected ack number through the cumulative ack
                for (const auto &key_val : acknum_time_map) {
                    if (key_val.first <= cum_ack) {
                        acknum_time_map.erase(key_val.first);
                    }
                }
            }

            // Adjust parameters for successful transmission of one packet
            if (cwnd < ss_thresh) {
                cwnd += MAX_PACKET_SIZE;
            } else {
                cwnd += MAX_PACKET_SIZE * MAX_PACKET_SIZE / cwnd;
            }

            // This value is to locate the point in the file to restart transmission if retransmission is triggered.
            // Only the last packet could have a size that is not MAX_PAYLOAD_SIZE, 
            sent_through_bytes += MAX_PAYLOAD_SIZE;
        }
    }

    fclose(fd);
    close(sock);
    return 0;
}
