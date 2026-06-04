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

struct ChallengeData {
    int challenge_id = 0;
    int N = 0;
    vector<vector<int>> A;
    vector<vector<int>> B;
};

static bool parseChallengeFromBuffer(const string& pending, ChallengeData& out, size_t& consumed) {
    istringstream iss(pending);

    int challenge_id = 0;
    int N = 0;
    if (!(iss >> challenge_id >> N)) {
        return false;
    }

    if (N <= 0) {
        return false;
    }

    vector<vector<int>> A(N, vector<int>(N));
    vector<vector<int>> B(N, vector<int>(N));

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (!(iss >> A[i][j])) {
                return false;
            }
        }
    }

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (!(iss >> B[i][j])) {
                return false;
            }
        }
    }

    out.challenge_id = challenge_id;
    out.N = N;
    out.A = std::move(A);
    out.B = std::move(B);

    consumed = static_cast<size_t>(iss.tellg());
    if (consumed == string::npos) {
        consumed = pending.size();
    }

    while (consumed < pending.size() && isspace(static_cast<unsigned char>(pending[consumed]))) {
        ++consumed;
    }

    return true;
}

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
    string pending;

    while (true) {
        int n = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) {
            cout << "Disconnected from server.\n";
            break;
        }
        pending.append(buffer, n);

        while (true) {
            ChallengeData challenge;
            size_t consumed = 0;
            if (!parseChallengeFromBuffer(pending, challenge, consumed)) {
                break;
            }

            pending.erase(0, consumed);

            vector<vector<int>> C(challenge.N, vector<int>(challenge.N, 0));

            for (int i = 0; i < challenge.N; i++) {
                for (int k = 0; k < challenge.N; k++) {
                    int aik = challenge.A[i][k];
                    for (int j = 0; j < challenge.N; j++) {
                        C[i][j] = (C[i][j] + aik * challenge.B[k][j]) % 997;
                    }
                }
            }

            int checksum = 0;
            for (int i = 0; i < challenge.N; i++) {
                for (int j = 0; j < challenge.N; j++) {
                    checksum = (checksum + C[i][j]) % 997;
                }
            }

            string response = to_string(challenge.challenge_id) + " " + to_string(checksum) + "\n";
            send(sock, response.c_str(), response.size(), 0);
        }

    }

    close(sock);
    return 0;
}
