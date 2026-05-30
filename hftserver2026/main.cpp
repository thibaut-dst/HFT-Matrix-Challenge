#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>
#include <sstream>
#include <climits>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

using namespace std;
using namespace std::chrono;
using json = nlohmann::json;

#define PORT 12345
#define BUFFER_SIZE 65536
#define RESULTS_FILE "/tmp/results.json"
#define MATRIX_SIZE 128
#define MODULO 997

struct ClientInfo {
    int socket;
    string name;
    // clientThread removed from active use to avoid dangling references
};

vector<unique_ptr<ClientInfo>> clients;
mutex clientsMutex;

mutex logMutex;
atomic<int> challengeId{1};

struct ChallengeState {
    int id;
    vector<vector<int>> A;
    vector<vector<int>> B;
    steady_clock::time_point startTime;
    unordered_map<string, int> latencies;                 // ms
    unordered_map<string, int> answers;                   // checksum sent by client
};
mutex challengeMutex;
ChallengeState currentChallenge;

// Generate random matrix NxN with values in [0, MODULO-1]
vector<vector<int>> generateMatrix(int N) {
    vector<vector<int>> mat(N, vector<int>(N));
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            mat[i][j] = rand() % MODULO;
    return mat;
}

// Compute checksum: sum of all entries of C = A * B (mod MODULO), result in [0, MODULO-1]
int computeChecksum(const vector<vector<int>>& A, const vector<vector<int>>& B) {
    int N = (int)A.size();
    // We'll compute C row by row without storing full matrix to reduce memory churn
    int checksum = 0;
    for (int i = 0; i < N; ++i) {
        vector<int> rowC(N, 0);
        for (int k = 0; k < N; ++k) {
            int a = A[i][k];
            for (int j = 0; j < N; ++j) {
                rowC[j] = (rowC[j] + (int)((1LL * a * B[k][j]) % MODULO)) % MODULO;
            }
        }
        for (int j = 0; j < N; ++j) {
            checksum = (checksum + rowC[j]) % MODULO;
        }
    }
    return checksum;
}

void logChallengeResult(
    int cid,
    const unordered_map<string, int>& latencies,
    const unordered_map<string, int>& answers,
    int correctAnswer
) {
    json entry;
    entry["challenge_id"] = cid;
    entry["correct_answer"] = correctAnswer;

    string winner = "";
    int bestLatency = INT_MAX;

    // Determine winner: fastest correct answer
    for (const auto& [name, latency] : latencies) {
        auto it = answers.find(name);
        if (it == answers.end()) continue;
        int ans = it->second;
        bool isCorrect = (ans == correctAnswer);
        if (isCorrect && latency < bestLatency) {
            bestLatency = latency;
            winner = name;
        }
    }

    if (winner.empty()) {
        entry["winner"] = nullptr;
    } else {
        entry["winner"] = winner;
    }

    // Log all players
    for (const auto& [name, latency] : latencies) {
        json player;
        player["name"] = name;
        player["latency_ms"] = latency;

        auto it = answers.find(name);
        if (it != answers.end()) {
            player["answer"] = it->second;
            player["correct"] = (it->second == correctAnswer);
        } else {
            player["answer"] = nullptr;
            player["correct"] = false;
        }

        entry["players"].push_back(player);
    }

    json allResults = json::array();
    ifstream in(RESULTS_FILE);
    if (in) {
        try {
            in >> allResults;
            if (!allResults.is_array()) allResults = json::array();
        } catch (...) {
            allResults = json::array();
        }
    }
    in.close();

    allResults.push_back(entry);

    ofstream out(RESULTS_FILE);
    out << allResults.dump(2);
    out.close();
}

// Helper: trim trailing whitespace/newlines
static inline void rtrim(string &s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
}

void handleClient(ClientInfo* client) {
    char buffer[BUFFER_SIZE];

    // Receive group name (first message)
    memset(buffer, 0, BUFFER_SIZE);
    int bytesReceived = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytesReceived <= 0) {
        cerr << "❌ Failed to receive client name." << endl;
        close(client->socket);
        return;
    }

    client->name = string(buffer, bytesReceived);
    rtrim(client->name);
    cout << "👤 Registered client: " << client->name << endl;

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        bytesReceived = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived <= 0) {
            cerr << "❌ Client " << client->name << " disconnected." << endl;
            break;
        }

        string msg(buffer, bytesReceived);
        rtrim(msg);

        // Protocol: prefer "challenge_id answer" (two integers).
        // If client sends a single integer, we accept it and associate with current challenge (backwards-compatible).
        int parsedCid = -1;
        int parsedAnswer = 0;
        int nParsed = 0;

        // Try to parse two integers
        {
            const char* cstr = msg.c_str();
            int a = 0, b = 0;
            nParsed = sscanf(cstr, "%d %d", &a, &b);
            if (nParsed == 2) {
                parsedCid = a;
                parsedAnswer = b;
            } else {
                // Try single integer
                nParsed = sscanf(cstr, "%d", &a);
                if (nParsed == 1) {
                    parsedCid = -1; // will use currentChallenge.id
                    parsedAnswer = a;
                } else {
                    // invalid message
                    cerr << "⚠️ Invalid answer format from " << client->name << ": '" << msg << "'" << endl;
                    continue;
                }
            }
        }

        auto now = steady_clock::now();

        lock_guard<mutex> lock(challengeMutex);
        int associatedCid = parsedCid;
        if (associatedCid == -1) associatedCid = currentChallenge.id;

        // Only accept answers for the active challenge id
        if (associatedCid != currentChallenge.id) {
            // Ignore answers for old/unknown challenges
            cout << "ℹ️ Ignoring answer from " << client->name << " for challenge " << associatedCid
                 << " (current " << currentChallenge.id << ")" << endl;
            continue;
        }

        int latency = duration_cast<milliseconds>(now - currentChallenge.startTime).count();
        currentChallenge.latencies[client->name] = latency;
        currentChallenge.answers[client->name] = parsedAnswer;

        cout << "📤 " << client->name << " answered " << parsedAnswer
             << " in " << latency << " ms (challenge " << currentChallenge.id << ")" << endl;
    }

    close(client->socket);
}

void broadcastChallengeLoop() {
    while (true) {
        int cid = challengeId++;
        vector<vector<int>> A = generateMatrix(MATRIX_SIZE);
        vector<vector<int>> B = generateMatrix(MATRIX_SIZE);

        int correctAnswer = computeChecksum(A, B);

        {
            lock_guard<mutex> lock(challengeMutex);
            currentChallenge.id = cid;
            currentChallenge.A = A;
            currentChallenge.B = B;
            currentChallenge.startTime = steady_clock::now();
            currentChallenge.latencies.clear();
            currentChallenge.answers.clear();
        }

        // Serialize challenge: cid, size, A values, B values
        stringstream ss;
        ss << cid << "\n";
        ss << MATRIX_SIZE << "\n";
        for (const auto& row : A) {
            for (int val : row) ss << val << " ";
        }
        ss << "\n";
        for (const auto& row : B) {
            for (int val : row) ss << val << " ";
        }
        ss << "\n";

        string payload = ss.str();

        {
            lock_guard<mutex> lock(clientsMutex);
            for (auto& client : clients) {
                // best-effort send; ignore errors here
                send(client->socket, payload.c_str(), payload.size(), 0);
            }
        }

        cout << "📢 Broadcasted matrix challenge " << cid << " to all clients (checksum=" << correctAnswer << ")" << endl;

        // Wait for answers (10 seconds)
        this_thread::sleep_for(seconds(10));

        unordered_map<string, int> snapshotLatencies;
        unordered_map<string, int> snapshotAnswers;
        {
            lock_guard<mutex> lock(challengeMutex);
            snapshotLatencies = currentChallenge.latencies;
            snapshotAnswers = currentChallenge.answers;
        }

        {
            lock_guard<mutex> lock(logMutex);
            logChallengeResult(cid, snapshotLatencies, snapshotAnswers, correctAnswer);
        }
    }
}

void startServer() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    inet_pton(AF_INET, "0.0.0.0", &serverAddr.sin_addr);

    if (::bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Bind failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    if (listen(serverSocket, 10) < 0) {
        perror("Listen failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    cout << "🚀 Server is listening on 0.0.0.0:" << PORT << endl;

    thread broadcaster(broadcastChallengeLoop);
    broadcaster.detach();

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientLen);
        if (clientSocket < 0) {
            perror("Accept failed");
            continue;
        }

        cout << "📡 Client connected: " << inet_ntoa(clientAddr.sin_addr) << endl;

        auto client = make_unique<ClientInfo>();
        client->socket = clientSocket;
        ClientInfo* clientPtr = client.get();

        {
            lock_guard<mutex> lock(clientsMutex);
            clients.push_back(std::move(client));
        }

        thread t(handleClient, clientPtr);
        t.detach();
    }

    close(serverSocket);
}

int main() {
    srand((unsigned)time(nullptr));
    startServer();
    return 0;
}