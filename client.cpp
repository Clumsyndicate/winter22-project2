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
#include <thread>

#include "protocol.hpp"

using namespace std;

struct meta_t {
    uint32_t offset;
    uint32_t size;
    uint32_t seq;
    uint32_t expected_ack;
    chrono::system_clock::time_point time;
};

// Abort connection after 10 seconds of silence from server. Closes socket.
int abort_connection(int sock) {
    close(sock);
    exit(1);
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
            std::cerr << "ERROR: Non-positive port number." << endl;;
            exit(1);
        }
    } catch (std::exception const &e) {
        std::cerr << "ERROR: Port number specified cannot be parsed." << endl;;
        exit(1);
    }
    
    string hostname(argv[1]);
    if (hostname != "localhost" && hostname != "node1" && hostname != "node2") {
        std::cerr << "ERROR: Hostname doesn't make sense" << endl;
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

    auto packetSize = formatSendPacket(buffer, header, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }
    logClientSend(header, MIN_CWND, INIT_SS_THRESH, false);

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
    logClientRecv(synHeader, MIN_CWND, INIT_SS_THRESH);
    auto my_cid = synHeader.cid; // use server-assigned connection ID

    ////////////////////////////////////////////////
    // Set up congestion control
    
    // Start the file transfer process. First open the file and get its size.
    FILE *fd = fopen(argv[3], "rb");
    fseek(fd, 0, SEEK_END);
    
    // Maximum file size 100MB. Using int is fine.
    int file_size = ftell(fd);
    fseek(fd, 0, SEEK_SET);

    // Set socket to be non-blocking for parallel packet timeout monitoring
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);

    // Initialize parameters
    auto cwnd = MIN_CWND;
    auto ss_thresh = INIT_SS_THRESH;
    uint32_t sent_bytes = 0;        // the first byte that is not yet sent
    uint32_t transmitted_bytes = 0; // the first byte that is not successfully transmitted

    const auto seq_startpoint = synHeader.ack;
    uint32_t curr_received_seq = synHeader.seq + 1;

    // bool retransmission_triggered = false;
    bool firstDataPacket = true;


    // Data structure for keeping track of timestamp, include:
    // sent_bytes (file offset for start of this packet)
    // actual_payload_size
    // seq
    // ack
    // timestamp
    vector<meta_t> packet_info;

    // uint32_t outbound_seq = 0;
    uint32_t received_ack = 0;


    ////////////////////////////////////////////////
    // Send payload with congestion control

    while (transmitted_bytes < (uint32_t) file_size) {

        ////////////////////////////////////////////////
        // Sending Packets with CWND

        // Reposition the file descriptor to be at the start of data to be sent
        fseek(fd, sent_bytes, SEEK_SET);        //TODO: not optimal, moving fd every time

        // Congestion window: send a total of cwnd bytes in several packets
        while (transmitted_bytes + cwnd > sent_bytes) {
            // If have cwnd quota left but ran out of file, exit this sending loop
            if (sent_bytes >= (uint32_t) file_size) {
                break;
            }

            // Expected payload size is either max UDP payload size or the remaining cwnd quota
            auto expected_payload_size = min((uint32_t) MAX_PAYLOAD_SIZE, transmitted_bytes + cwnd - sent_bytes);

            // ofstream myfile;
            // myfile.open ("debug.txt", std::ios_base::app);
            // myfile << expected_payload_size << " expected paylod size \n";
            // myfile << " ---------- \n";

           
            char buffer[MAX_PACKET_SIZE];
            char * payloadBuffer = new char [expected_payload_size];
            bzero(payloadBuffer, expected_payload_size);

            // actual_size might be smaller than the expected payload_size, since we may reach the EOF
            uint32_t actual_payload_size = fread(payloadBuffer, 1, expected_payload_size, fd);
            if (actual_payload_size < 0) {
                std::cerr << "ERROR: Failed to read from file.";
                fclose(fd);
                abort_connection(sock);
            }

            // Construct header
            header_t payloadHeader {
                (seq_startpoint + sent_bytes) % MAX_SEQ_NUM,
                curr_received_seq,
                my_cid,
                false, false, false
            };

            if (firstDataPacket) {
                firstDataPacket = false;
                payloadHeader.a = true;
            }

            // Buffer will hold the entire packet (header + payload).
            packetSize = formatSendPacket(buffer, payloadHeader, payloadBuffer, actual_payload_size);
            sendto(sock, buffer, packetSize, 0, (struct sockaddr*)&socketAddress, sizeof socketAddress);
            logClientSend(payloadHeader, cwnd, ss_thresh, false);
 
            // Construct meta struct
            meta_t meta {
                sent_bytes,
                (uint32_t) actual_payload_size,
                payloadHeader.seq,
                payloadHeader.seq + actual_payload_size,
                chrono::system_clock::now()
            };


            // ofstream myfile;
            // myfile.open ("debug.txt", std::ios_base::app);
            // myfile << meta.expected_ack << " hi \n";
            // myfile << " ---------- \n";


            // Record time in meta data struct
            packet_info.push_back(meta);

            sent_bytes += actual_payload_size;
        }

        // ofstream myfile;
        // myfile.open ("debug.txt", std::ios_base::app);
        // myfile << transmitted_bytes << " transmitted \n";
        // myfile << sent_bytes << " sent \n";
        // myfile << " ---------- \n";

        ////////////////////////////////////////////////
        // Check Timeout

        auto it = packet_info.begin();
        while (it != packet_info.end())
        {
            auto time_elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - it->time).count();
            
            //cout << time_elapsed << " time elapsed\n";

            if (time_elapsed > RETRANSMISSION_TIMER) {
                // If detected timeout
                sent_bytes = it->offset;
                ss_thresh = cwnd / 2;
                cwnd = MIN_CWND;
                // retransmission_triggered = true;
                cout << "Retransmitting from \n" << sent_bytes << " seq: " << it->seq << endl;
                packet_info.clear();
                break;
            }
            else if (it->expected_ack == received_ack) {
                transmitted_bytes = it->offset + it->size;
                packet_info.erase(packet_info.begin(), it + 1);
                break;
            }
            ++it;
        }

        ////////////////////////////////////////////////
        // Receive Ack

        ssize_t received_size;
        while ((received_size = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0)) > 0) {
            auto ackHeader = getHeader(buffer, received_size);
            logClientRecv(ackHeader, cwnd, ss_thresh);

            received_ack = ackHeader.ack;
            
            // Adjust parameters for successful transmission of one packet
            if (cwnd < ss_thresh) {
                cwnd += MAX_PAYLOAD_SIZE;
            } else {
                cwnd += MAX_PAYLOAD_SIZE * MAX_PAYLOAD_SIZE / cwnd;
            }

            continue;   //TODO: not sure if we should continue or just let it flow to the sending loop
        }
    }
    
    
    // File transfer completed. Start disconnecting.
    header_t finHeader {
            received_ack,
            0,
            my_cid,
            false, false, true
        };
        
    // Don't include anything in the payload
    packetSize = formatSendPacket(buffer, finHeader, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    logClientSend(finHeader, cwnd, ss_thresh, false);
        
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
        logClientRecv(finAckHeader, cwnd, ss_thresh);
        
        // Makes it look like reference client.
        header_t ackFinAckHeader {
            finAckHeader.ack,
            finAckHeader.seq + (uint32_t) 1,
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
        
        logClientSend(ackFinAckHeader, cwnd, ss_thresh, false);
    }
    
    // Wait for two seconds, responde every FIN packet with an ACK and drop all others.
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    
    // Until 2000 mili seconds passed
    while (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() < 2000) {
        recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);
        header_t finWaitHeader;
        
        if (recsize > 0) {
            finWaitHeader = getHeader(buffer, recsize);
            logClientRecv(finWaitHeader, cwnd, ss_thresh);

            if (finWaitHeader.f) {
                // Weird name but meh
                header_t ackFinAckHeader {
                    finWaitHeader.ack,
                    finWaitHeader.seq + (uint32_t) 1,
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
                logClientSend(ackFinAckHeader, cwnd, ss_thresh, false);
            }
        }
        // Only responds to a FIN packet
        
        
        end = std::chrono::steady_clock::now();
    }

    fclose(fd);
    close(sock);
    return 0;
}
