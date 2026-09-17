#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
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
    const int Max_Clients = 11;
    if (argc != 2) {
        cerr << "Usage Format: " << argv[0] << " portNum" << endl;
        exit(1);
    }

    int port = atoi(argv[1]);


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
    // Listen for incoming connections
    if (listen(sockfd, Max_Clients) < 0) {
        cerr << "Error: could not listen on socket" << endl;
        exit(2);
    }
    cout << "ListenDone: " << port << endl;
    struct pollfd tester[Max_Clients + 1];
    tester[0].fd = sockfd;
    tester[0].events = POLLIN;
    int pointer = 1;
    while (true) {
        int result = poll(tester, pointer, 200);
        if (result) {
            if (tester[0].revents & POLLIN) {
                // Accept incoming connections
                struct sockaddr_in cli_addr;
                socklen_t cli_len = sizeof(cli_addr);
                int newsockfd = accept(sockfd, (struct sockaddr*)&cli_addr, &cli_len);
                if (newsockfd < 0) {
                    cerr << "Error: could not accept incoming connection" << endl;
                    exit(2);
                }
                tester[pointer].fd = newsockfd;
                tester[pointer].events = POLLIN;
                pointer++;
                char cli_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(cli_addr.sin_addr), cli_ip, INET_ADDRSTRLEN);
                cout << "Client: " << cli_ip << ":" << cli_addr.sin_port << endl;
            }
            for (int i = 1; i < pointer; i++) {
                if (tester[i].revents & POLLIN) {
                    char filereader[1024];
                    if (recv(tester[i].fd, filereader, sizeof(filereader), 0) < 0) {
                        cout << "UnknownCmd" << endl;
                        cerr << "File name not received closing connection" << endl;
                        close(tester[i].fd);
                    }
                    string op = strtok(filereader, " ");
                    string file_name = strtok(NULL, " ");
                    if (op == "get") {
                        ifstream file_in(file_name, ios::binary);
                        if (!file_in.is_open()) {
                            cout << "FileTransferFail" << endl;
                            cerr << "Error: file not found or not readable" << endl;
                            close(tester[i].fd);
                        }

                        file_in.seekg(0, ios::end);
                        int file_size = file_in.tellg();
                        file_in.seekg(0, ios::beg);
                        char* buffer = new char[file_size];
                        file_in.read(buffer, file_size);
                        if (send(tester[i].fd, buffer, file_size, 0) < 0) {
                            cerr << "Error: could not send file" << endl;
                        } else {
                            // Print transfer status
                            cout << "TransferDone: " << file_size << " bytes" << endl;
                        }
                        file_in.close();
                        close(tester[i].fd);
                        delete[] buffer;
                    } else if (op == "put") {
                        ofstream outfile(file_name, ios::out | ios::binary);
                        if (!outfile.is_open()) {
                            cerr << "Error: Failed to open file " << argv[2] << " for writing" << endl;
                            exit(3);
                        }
                        // Receive file data
                        char buffer[1000];
                        int bytes_received = 0;
                        int total_bytes_received = 0;
                        while ((bytes_received = recv(tester[i].fd, buffer, sizeof(buffer), 0)) > 0) {
                            outfile.write(buffer, bytes_received);
                            total_bytes_received += bytes_received;
                        }
                        if (bytes_received < 0) {
                            cerr << "Error: Failed to receive file data" << endl;
                            exit(3);
                        }
                        // Print file reception status
                        cout << "FileWritten: " << total_bytes_received << " bytes" << endl;
                        // Close file and socket
                        outfile.close();
                        close(tester[i].fd);
                    }
                    if (i != pointer - 1) {
                        tester[i].fd = tester[pointer - 1].fd;
                        tester[i].events = tester[pointer - 1].events;
                        i--;
                    }
                    pointer--;
                }
            }
        }
    };
    close(sockfd);
    return 0;
}
