Team Member: Qiaosong Zhou (605376815), Yicheng Zhu (505124173), Zihan Liu (105144205)

Contribution:
Qiaosong Zhou: Reading and writing buffer, header construction, connection management (server) , large file transmission.
Yicheng Zhu: worked on congestion control, client-side timeout (work mostly in the cc branch)
Zihan Liu: Zihan worked on the file transfer, the FIN process, and server-side timeout.

High-Level designs:

The server maintains a hashmap of cid keys to connection information. The server uses these stateful information to keep track of the file pointers and connection status of each client. The server, most notably, does not use any multithreading. It simply has a infinite while loop that process the incoming packet, rather like a DFA.

The congestion control is implemented by using a vector to maintain the currently unack’d but sent packets. Every time a ack is inbound, the vector deletes ack’d packets. Every time packets are sent, new information is added to the vector. 

Problems encountered:
A current problem is that the client may receive an acknowledge number that does not match with any packets it sends out 

Additional libraries:

<poll.h> for keeping track of time outs. 
<chrono> for computing time elapsed.








