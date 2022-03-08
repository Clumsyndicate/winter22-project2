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
#include <chrono>

#include <unordered_map>

#include "protocol.hpp"
#include "connection.hpp"

using namespace std;

int sock;

unordered_map<uint16_t, Connection> connections;
unordered_map<uint16_t, std::chrono::steady_clock::time_point> lastPacketTimes;
uint16_t connCnt = 1;

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
    if (::bind(sock, (struct sockaddr *)&socketAddress, sizeof socketAddress) == -1) {
        std::cerr << "ERROR: Failed to bind socket.";
        close(sock);
        exit(1);
    }
    // Uncomment to make recvfrom nonblocking
    
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);

    for (;;) {
        struct sockaddr sender;
        recsize = recvfrom(sock, (void*)buffer, sizeof buffer, 0, &sender, &fromlen);
        if (recsize < 0) {
            // comment it out for non blocking.
//            perror("Negative receive size.");
//            exit(1);
            continue;
        }

        auto header = getHeader(buffer, recsize);
        logServerRecv(header);
        auto payload = getPayload(buffer, recsize);
        
        // Determine if the last packet was sent over 10 seconds ago. If so, change CState to ended and write ERROR to
        // corresponding file.
        if (lastPacketTimes.find(header.cid) != lastPacketTimes.end()) {
            std::chrono::steady_clock::time_point current = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(current - lastPacketTimes[header.cid]).count() > 10000) {
                connections[header.cid].state = CState::ENDED;
                string payload {"ERROR"};
                cout << "Connection timeout." << endl;
                
                if (connections[header.cid].file == nullptr) {
                    std::cerr << "File ptr is null when trying to write ERROR from time out!" << endl;
                }
                else if (fwrite(payload.c_str(), 1, payload.size(), connections[header.cid].file) < 0) {
                    std::cerr << "Failed to write to file for a closed connection due to timeout." << endl;
                }
                                
            } else {
                // If no timeout, update the last packet received time.
                lastPacketTimes[header.cid] = std::chrono::steady_clock::now();
            }
        } else {
            // Create the entry if not present.
            lastPacketTimes[header.cid] = std::chrono::steady_clock::now();
        }

        if (header.s) {
            cout << "Received handshake request" << endl;
            // After receiving a packet with SYN flag, the server should create state for the connection ID and proceed with 3-way handshake for this connection. Server should use 4321 as initial sequence number.
            header_t resHeader {
                4321,
                header.seq + 1,
                connCnt,
                true, true, false
            };
            // Create new connection with unique id
            connections.emplace(connCnt, Connection { connCnt, sender });

            char sendBuffer[1024];
            auto packetSize = formatSendPacket(sendBuffer, resHeader, nullptr, 0);

            sendto(sock, sendBuffer, packetSize, 0, &sender, sizeof sender);
            logServerSend(resHeader);
            
            connCnt += 1;
            continue;
        }

        if (header.a) {
            // Client ack
            cout << "Received client " << header.cid << " ack" << endl;
            if (connections.find(header.cid) == connections.end()) {
                // Invalid header cid, not found in connections
                cerr << "Invalid header cid, not found in connections" << endl;
                continue;
            }
            auto& conn = connections[header.cid];
            if (conn.state == CState::ENDED) {
                // Finish up the connection
                fclose(conn.file);
            }
            if (conn.state == CState::ACK) {
                conn.state = CState::STARTED;
                auto path = string(argv[2]) + "/" + to_string(header.cid) + ".file";
                cout << "Saving to path: " << path << endl;
                auto fptr = fopen(path.c_str(), "wb");
                conn.file = fptr;
                // cout << "Init fptr: " << fptr << endl;
            }

            continue;
        }

        // After receiving a FIN, send an ACK and FIN back to back (but not closing the socket).
        if (header.f) {
            cout << "Received Fin request" << endl;
            
            // Only responds to a FIN when its sent by a valid, still open connection.
            // If not, disregard it.
            if (connections.find(header.cid) == connections.end()) {
                cerr << "Invalid header cid, not found in connections" << endl;
                continue;
            }
            
            auto& conn = connections[header.cid];
            if (conn.state == CState::ENDED) {
                continue;
            }
            
            header_t ackHeader {
                header.ack,
                header.seq + 1,
                header.cid,
                true, false, false
            };

            char sendBuffer[1024];
            auto packetSize = formatSendPacket(sendBuffer, ackHeader, nullptr, 0);
            sendto(sock, sendBuffer, packetSize, 0, &sender, sizeof sender);
            logServerSend(ackHeader);

            header_t finHeader {
                header.ack + 1,
                0,
                header.cid,
                false, false, true
            };
            
            packetSize = formatSendPacket(sendBuffer, finHeader, nullptr, 0);
            sendto(sock, sendBuffer, packetSize, 0, &sender, sizeof sender);
            logServerSend(finHeader);
            // Now change the status of this connection to ended.
            connections[header.cid].state = CState::ENDED;
            cout << "Connection closed." << endl;
            
            continue;
        }

        if (connections.find(header.cid) != connections.end()) {
            // Received packet
            auto& conn = connections[header.cid];

            if (conn.state == CState::STARTED) {
                // Connection handshake is appropriate
                
                // Check if file ptr is nullptr, if so wait for ack first.
                if (conn.head == header.seq) {
                    conn.head += payload.size();

                    // If this packet is the next seq expected
                    // cout << "Fptr here: " << conn.file << endl;
                    if (connections[header.cid].file == nullptr) {
                        std::cerr << "File ptr is nullptr when trying to write to file cid=" << conn.cid << " packet queue!" << endl;
                    } else if (fwrite(payload.c_str(), 1, payload.size(), conn.file) < 0) {
                        perror("Write failed");
                        // TODO : handle error
                    }
                    fflush(conn.file);
                } else {
                    conn.queue.emplace(header.seq, DataPacket { header.seq, (uint32_t) payload.size(), payload });
                }

                // Check if out-of-order packets in the queue can now be written
                for (auto& packet: conn.queue) {
                    if (packet.first == conn.head) {
                        if (conn.file == nullptr) {
                            std::cerr << "File ptr is nullptr when trying to process cid=" << conn.cid << " packet queue!" << endl;
                        } else if (fwrite(packet.second.payload.c_str(), 1, payload.size(), conn.file) < 0) {
                            perror("Write failed");
                            // TODO : handle error
                        }
                        fflush(conn.file);
                        conn.head += packet.second.size;
                    }
                }

                cout << "Queue size: " << conn.queue.size() << endl;
                cout << conn.head << " " << conn.queue.begin()->first << endl;

                // Send ACK
                header_t resHeader {
                    header.ack,
                    // header.seq + (uint32_t) payload.size(),
                    conn.head,
                    conn.cid,
                    true, false, false
                };

                char sendBuffer[1024];
                auto packetSize = formatSendPacket(sendBuffer, resHeader, nullptr, 0);

                sendto(sock, sendBuffer, packetSize, 0, &sender, sizeof sender);
                logServerSend(resHeader);
            } else if (conn.state == CState::ENDED) {

                
            }
        } else {
            // Error connection id is not in connections.
            cout << "DROP " << header.seq << " " << header.ack << " " << header.cid;
            if (header.a) 
                cout << " ACK";
            if (header.s)
                cout << " SYN";
            if (header.f)
                cout << " FIN";
            cout << endl;
        }
    }
    
    return 0;
}
