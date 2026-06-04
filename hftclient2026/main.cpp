#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

int main(int argc, char** argv) {
    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <host> <port> <team_name>\n";
        return 1;
    }

    string host = argv[1];
    int port = stoi(argv[2]);
    string team = argv[3];

    // cout << "HFT Client Template\n";
    // cout << "This client connects but does NOT solve challenges.\n";
    // cout << "You must implement the real logic.\n\n";

    // --- Connect to server ---
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    // Send team name
    string intro = team + "\n";
    send(sock, intro.c_str(), intro.size(), 0);

    cout << "Connected to server at " << host << ":" << port << "\n";
    cout << "Waiting for challenges...\n";

    char buffer[262144];

    while (true) {
        int n = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) {
            cout << "Disconnected from server.\n";
            break;
        }
        buffer[n] = '\0';
        string msg(buffer);




        istringstream iss(msg); // iss = input string stream - allows us to parse the message easily
        int challenge_id, N;
        iss >> challenge_id >> N;

        vector<vector<int>> A(N, vector<int>(N));
        vector<vector<int>> B(N, vector<int>(N));

        for (int i = 0; i < N; i++){
            for (int j = 0; j < N; j++){
                iss >> A[i][j];
            }
        }
        for (int i = 0; i < N; i++){
            for (int j = 0; j < N; j++){
                iss >> B[i][j];
            }
        }

        vector<vector<int>> C(N, vector<int>(N, 0));

        for (int i = 0; i < N; i++) {
            for (int k = 0; k < N; k++) {
                int aik = A[i][k];
                for (int j = 0; j < N; j++) {
                    C[i][j] = (C[i][j] + aik * B[k][j]) % 997;
                }
            }
        }

        // checksum
        int checksum = 0;
        for (int i = 0; i < N; i++){
            for (int j = 0; j < N; j++){
                checksum = (checksum + C[i][j]) % 997;
            }
        }
        

        string response = to_string(challenge_id) + " " + to_string(checksum) + "\n";
        send(sock, response.c_str(), response.size(), 0);

    }

    close(sock);
    return 0;
}
