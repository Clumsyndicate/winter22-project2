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

#include "protocol.hpp"

using namespace std;


int main(int argc, const char * argv[]) {
    
    int portNumber, sock, bytes_sent;
    uint16_t cid;
    struct sockaddr_in socketAddress;
    char buffer[200];
    
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
    
    // Initialize socket, send dummy data to server.
    
    // strcpy(buffer, "hello world!");
    // string payload { "hello world!" };
     
    
    memset(&socketAddress, 0, sizeof socketAddress);

    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = inet_addr(host);
    socketAddress.sin_port = htons(portNumber);

    // Format buffer

    // Send UDP packet src-ip=DEFAULT, src-port=DEFAULT, dst-ip=HOSTNAME-OR-IP, dst-port=PORT with SYN flag set, Connection ID initialized to 0, Sequence Number set to 12345, and Acknowledgement Number set to 0

    header_t header {
        12345,
        0,
        0,
        false, true, false
    };

    
    auto packetSize = formatSendPacket(buffer, header, nullptr, 0);
    cout << "Final packet size: " << packetSize << endl;
    bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }

    // Listens for server response
    auto recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);

    if (recsize < 0) {
        std::cerr << "Negative receive size.";
        exit(1);
    }

    auto synHeader = getHeader(buffer, recsize);
    cid = synHeader.cid;
    
    header_t ackHeader {
        synHeader.ack,
        synHeader.seq + 1,
        cid,
        true, false, false
    };

    packetSize = formatSendPacket(buffer, ackHeader, nullptr, 0);
    bytes_sent = sendto(sock, buffer, packetSize, 0,(struct sockaddr*)&socketAddress, sizeof socketAddress);
    if (bytes_sent < 0) {
        perror("Sending to server failed.");
        exit(1);
    }
    
    
    // Start the file transfer process. First open the file and get its size.
    FILE *fd = fopen(argv[3], "rb");
    fseek(fd, 0, SEEK_END);
    
    // Maximum file size 100MB. Using int is fine.
    long fileSize = ftell(fd);
    cout << "File size is " << fileSize << endl;
    fseek(fd, 0, SEEK_SET);
    
    // Initialize cwnd;
    int cwnd = 512;

    header_t payloadHeader {
        ackHeader.seq,
        ackHeader.ack,
        cid,
        false, false, false
    };

    size_t bytesRead = 1;
    
    while(bytesRead > 0) {
        // Assuming that cwnd is properly handled, and is no larger than the maximum size allowed.
        int payloadSize = cwnd - 12;
        char buffer[cwnd];
        char * payloadBuffer = new char [payloadSize];
        bzero(payloadBuffer, payloadSize);
        if ((bytesRead = fread(payloadBuffer, 1, payloadSize, fd)) < 0) {
            std::cerr << "ERROR: Failed to read from file.";
            close(sock);
            fclose(fd);
            exit(1);
        }
        // cout << "read " << bytesRead << " bytes from file" << endl;
        
        // Buffer will hold the entire packet (header + payload).
        packetSize = formatSendPacket(buffer, payloadHeader, payloadBuffer, bytesRead);
        bytes_sent = sendto(sock, buffer, packetSize, 0, (struct sockaddr*)&socketAddress, sizeof socketAddress);

        fileSize -= bytesRead;

        payloadHeader.seq += payloadSize;
        cout << "New seq " << payloadHeader.seq << endl;
    }

    cout << "Finished sending the file" << endl;
    cout << "File size is " << fileSize << endl;

    sleep(10000);
    
    // Payload sent, disconnect
    header_t finHeader {
        0,
        0,
        cid,
        false, false, true
    };
    
    // Don't include anything in the payload;
    packetSize = formatSendPacket(buffer, finHeader, nullptr, 0);
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
    auto finAckHeader = getHeader(buffer, recsize);
    
    if (finAckHeader.f) {
        //todo: what to do? Spec doesn't say a thing lmao
        cout << "Received FIN-ACK from server." << endl;
    }
    
    // Wait for two seconds, responde every FIN packet with an ACK and drop all others.
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    
    // Until 2000 mili seconds passed
    while (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() < 2000) {
        cout << "a";
        recsize = recvfrom(sock, buffer, sizeof buffer, 0, nullptr, 0);
        auto finWaitHeader = getHeader(buffer, recsize);
        
        // Only responds to a FIN packet
        if (finWaitHeader.f) {
            // Weird name but meh
            header_t ackFinAckHeader {
                0,
                0,
                cid,
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
        
        end = std::chrono::steady_clock::now();
    }
    
    // After two seconds, close the connection.
    fclose(fd);
    close(sock);
    return 0;
}
