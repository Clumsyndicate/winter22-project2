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
#include <map>

#include "protocol.hpp"

using namespace std;

struct meta_t {
    uint32_t seq;
    chrono::system_clock::time_point time;
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

    auto ret = getaddrinfo(argv[1], argv[2], &hints, &res);
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
//    string payload { "hello world!" };
    
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

    auto packet_size = formatSendPacket(buffer, header, nullptr, 0);
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
    auto my_cid = synHeader.cid; // use server-assigned connection ID

    ////////////////////////////////////////////////
    // Send Ack packet (no payload)
    
    cout << "d" << endl;

    header_t ackHeader {
        synHeader.ack,
        synHeader.seq + 1,
        my_cid,
        true, false, false
    };

    packet_size = formatSendPacket(buffer, ackHeader, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packet_size, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }

    cout << "e" << endl;

    ////////////////////////////////////////////////
    // Set up congestion control
    
    // Start the file transfer process. First open the file and get its size.
    FILE *fd = fopen(argv[3], "rb");
    fseek(fd, 0, SEEK_END);
    
    cout << "f" << endl;

    // Maximum file size 100MB. Using int is fine.
    auto file_size = ftell(fd);
    fseek(fd, 0, SEEK_SET);

    // Set socket to be non-blocking for parallel packet timeout monitoring
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);

    cout << "g" << endl;

    // Initialize parameters
    auto cwnd = MIN_CWND;
    auto ss_thresh = INIT_SS_THRESH;
    uint32_t sending_startpoint = 0;        // the first byte that is not yet sent
    auto transmitted_startpoint = 0; // the first byte that is not successfully transmitted

    const auto seq_startpoint = 0;
    uint32_t cum_ack = 0;
    uint32_t curr_received_seq = 0;

    cout << "h" << endl;

    bool retransmission_triggered = false;

    // Data structure for keeping track of timestamp
    // expected_acknum -> (seqnum, timestamp)
    map<uint32_t, meta_t> acknum_map;
    
    ////////////////////////////////////////////////
    // Send payload with congestion control
    
    cout << "a" << endl;;

    while (transmitted_startpoint < file_size) {

        // Reset retransmission flag
        retransmission_triggered = false;

        // Check timeout
        for (auto i = acknum_map.begin(); i != acknum_map.end(); i++) {
            if (chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - i->second.time).count() >= RETRANSMISSION_TIMER) {
                // If detected timeout
                sending_startpoint = i->second.seq; // starting byte (seq) of last untransmitted packet
                ss_thresh = cwnd / 2;
                cwnd = MIN_CWND;
                retransmission_triggered = true;
                acknum_map.clear();
                break;
            }
        }

        // Start new loop if triggered retransmission
        if (retransmission_triggered) {
            cout << "c" << endl;
            continue;
        }

        // Check arrival in socket
        auto received_size = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);

        // If received something, start processing ack packet to determine new starting point
        if (received_size > 0) {
            ackHeader = getHeader(buffer, received_size);
            cum_ack = ackHeader.ack;
            curr_received_seq = ackHeader.seq;

            // Update startpoint for successful transmission
            transmitted_startpoint = cum_ack;

            // Update packet metainfo map, remove successfully transmitted ones
            
            vector<uint32_t> indices;
            
            for (const auto &key_val : acknum_map) {
                if (key_val.first <= cum_ack) {
                    indices.push_back(key_val.first);
                }
            }
            
            for (uint32_t key : indices) {
                acknum_map.erase(key);
            }

            // Adjust parameters for successful transmission of one packet
            if (cwnd < ss_thresh) {
                cwnd += MAX_PAYLOAD_SIZE;
            } else {
                cwnd += MAX_PAYLOAD_SIZE * MAX_PAYLOAD_SIZE / cwnd;
            }

            continue;   //TODO: not sure if we should continue or just let it flow to the sending loop
        }

        ////////////////////////////////////////////////
        // Sending logic

        // Reposition the file descriptor to be at the start of data to be sent
        fseek(fd, sending_startpoint, SEEK_SET);        //TODO: not optimal, moving fd every time

        // Keep track of how many bytes left in current cwnd
        auto cwnd_left = cwnd;

        // Congestion window: send a total of cwnd bytes in several packets
        while (cwnd_left > 0) {

            // If have cwnd quota left but ran out of file, exit this sending loop
            if (sending_startpoint >= file_size) {
                break;
            }

            // Expected payload size is either max UDP payload size or the remaining cwnd quota
            auto expected_payload_size = min(MAX_PAYLOAD_SIZE, cwnd_left);
            char buffer[MAX_PACKET_SIZE];
            char * payloadBuffer = new char [expected_payload_size];
            bzero(payloadBuffer, expected_payload_size);

            // actual_size might be smaller than the expected payload_size, since we may reach the EOF
            auto actual_payload_size = fread(payloadBuffer, 1, expected_payload_size, fd);
            if (actual_payload_size < 0) {
                std::cerr << "ERROR: Failed to read from file.";
                fclose(fd);
                abort_connection(sock);
            }

            // Construct header
            header_t payloadHeader {
                seq_startpoint + sending_startpoint,
                curr_received_seq,
                my_cid,
                false, false, false
            };

            cout << "=================\n";
            cout << payloadHeader.seq << '\n';
            cout << payloadHeader.ack << '\n';
            cout << payloadHeader.cid << '\n';
            cout << payloadHeader.a << '\n';
            cout << payloadHeader.s << '\n';
            cout << payloadHeader.f << '\n';
            cout << "what the fuck" << '\n';
            cout << actual_payload_size << '\n';
            cout << "=================\n";

            // Buffer will hold the entire packet (header + payload).
            packet_size = formatSendPacket(buffer, payloadHeader, payloadBuffer, actual_payload_size);
            sendto(sock, buffer, packet_size, 0, (struct sockaddr*)&socketAddress, sizeof socketAddress);
            
            // Construct meta struct
            meta_t meta {
                payloadHeader.seq,
                chrono::system_clock::now()
            };

            // Record time in meta data struct
            auto expected_acknum = payloadHeader.seq + actual_payload_size;
            acknum_map[expected_acknum] = meta;

            sending_startpoint += actual_payload_size;
            cwnd_left -= actual_payload_size;
        }
    }
    
    // File transfer completed. Start disconnecting.
    header_t finHeader {
            0,
            0,
            my_cid,
            false, false, true
        };
        
    // Don't include anything in the payload
    auto packetSize = formatSendPacket(buffer, finHeader, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    
    cout << "Sent fin packet" << endl;
    
    if (bytes_sent < 0) {
        std::cerr << "Failed to send FIN packet." << endl;
        close(sock);
        exit(1);
    }
    
    // Expect an ACK packet
    // todo: what if never gets one? Should we include this part in the 2 seconds wait and close the connection after?
    recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);
    header_t finAckHeader;
    if (recsize > 0) {
        finAckHeader = getHeader(buffer, recsize);
    }
    
    
    if (finAckHeader.f) {
        //todo: what to do? Spec doesn't say a thing lmao
        cout << "Received FIN-ACK from server." << endl;
    }
    
    // Wait for two seconds, responde every FIN packet with an ACK and drop all others.
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    
    // Until 2000 mili seconds passed
    while (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() < 2000) {
        recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);
        header_t finWaitHeader;
        
        if (recsize > 1) {
            cout << "a" << endl;
            finWaitHeader = getHeader(buffer, recsize);
            
            if (finWaitHeader.f) {
                // Weird name but meh
                header_t ackFinAckHeader {
                    0,
                    0,
                    my_cid,
                    true, false, false
                };
                
                packetSize = formatSendPacket(buffer, ackFinAckHeader, nullptr, 0);
                bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
                
                if (bytes_sent < 0) {
                    std::cerr << "Failed to send the ACK for the FIN-ACK packet.";
                    close(sock);
                    exit(1);
                }
            }
        }
        // Only responds to a FIN packet
        
        
        end = std::chrono::steady_clock::now();
    }

    fclose(fd);
    close(sock);
    return 0;
}
