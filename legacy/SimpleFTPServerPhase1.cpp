#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage Format: " << argv[0] << " portNum fileToTransfer" << endl;
        exit(1);
    }

    int port = atoi(argv[1]);
    string file_name = argv[2];

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    // Bind the socket to a port
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Error: could not bind to port " << port << endl;
        exit(2);
    }
    cout << "BindDone: " << port << endl;
    ifstream file_in(file_name, ios::binary);
    if (!file_in.is_open()) {
        cerr << "Error: file not found or not readable" << endl;
        exit(3);
    }
    // Listen for incoming connections
    if (listen(sockfd, 1) < 0) {
        cerr << "Error: could not listen on socket" << endl;
        exit(2);
    }
    cout << "ListenDone: " << port << endl;

    // Accept incoming connections
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int newsockfd = accept(sockfd, (struct sockaddr*)&cli_addr, &cli_len);
    if (newsockfd < 0) {
        cerr << "Error: could not accept incoming connection" << endl;
        exit(2);
    }
    char cli_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(cli_addr.sin_addr), cli_ip, INET_ADDRSTRLEN);
    cout << "Client: " << cli_ip << ":" << (int)ntohs(cli_addr.sin_port) << endl;

    // Check if file exists and is readable
    file_in.seekg(0, ios::end);
    int file_size = file_in.tellg();
    file_in.seekg(0, ios::beg);
    char* buffer = new char[file_size];
    file_in.read(buffer, file_size);
    if (send(newsockfd, buffer, file_size, 0) < 0) {
        cerr << "Error: could not send file" << endl;
        exit(2);
    }
    file_in.close();
    delete[] buffer;
    close(newsockfd);
    close(sockfd);
    // Print transfer status
    cout << "TransferDone: " << file_size << " bytes" << endl;
    return 0;
}