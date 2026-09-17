#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc != 3) {
        cerr << "Usage Format: " << argv[0] << " serverIPAddr:port fileToReceive" << endl;
        exit(1);
    }

    // Parse server address and port
    char* server_addr = strtok(argv[1], ":");
    char* server_port_str = strtok(NULL, ":");
    int server_port = atoi(server_port_str);
    string fileToReceive = argv[2];

    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    // Connect to server
    struct sockaddr_in server_addr_info;
    memset(&server_addr_info, 0, sizeof(server_addr_info));
    server_addr_info.sin_family = AF_INET;
    server_addr_info.sin_port = htons(server_port);
    inet_pton(AF_INET, server_addr, &server_addr_info.sin_addr);
    if (connect(sockfd, (struct sockaddr*)&server_addr_info, sizeof(server_addr_info)) < 0) {
        cerr << "Error: Failed to connect to server" << endl;
        exit(2);
    }

    // Print connection status
    cout << "ConnectDone: " << server_addr << ":" << server_port << endl;

    string get = "get";
    string fileAddr = get + " " + fileToReceive;
    char* char_array = new char[fileAddr.length() + 1];
    strcpy(char_array, fileAddr.c_str());

    if (send(sockfd, char_array, fileAddr.length() + 1, 0) < 0) {
        cerr << "Error: could not send filename" << endl;
        exit(2);
    }

    // Open file for writing
    ofstream outfile(argv[2], ios::out | ios::binary);
    if (!outfile.is_open()) {
        cerr << "Error: Failed to open file " << argv[2] << " for writing" << endl;
        exit(3);
    }
    // Receive file data
    char buffer[1024];
    int bytes_received = 0;
    int total_bytes_received = 0;
    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
        outfile.write(buffer, bytes_received);
        total_bytes_received += bytes_received;
    }
    if (bytes_received < 0) {
        cerr << "Error: Failed to receive file data" << endl;
        exit(3);
    }

    // Close file and socket
    outfile.close();
    close(sockfd);

    // Print file reception status
    cout << "FileWritten: " << total_bytes_received << " bytes" << endl;

    return 0;
}
