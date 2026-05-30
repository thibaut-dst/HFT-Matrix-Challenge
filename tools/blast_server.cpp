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
#include <queue>
#include <condition_variable>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

#define PORT 12345
#define BUFFER_SIZE 65536
#define RESULTS_FILE "/tmp/results.json"
#define MODULO 997

// -------------------------------------------------------------
// Configurable blast parameters
// -------------------------------------------------------------
struct BlastConfig {
    int matrixSize = 128;
    int challengesPerSecond = 50;
    int answerWindowMs = 30;

    // 0 = normal, 1 = heavy, 2 = ultra
    int mode = 0;
};

BlastConfig gConfig;

// -------------------------------------------------------------
// Client structure
// -------------------------------------------------------------
struct ClientInfo {
    int socket;
    string name;

    mutex qMutex;
    queue<string> outbox;
    condition_variable cv;
    bool running = true;
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
    unordered_map<string, int> latencies;
    unordered_map<string, int> answers;
};
mutex challengeMutex;
ChallengeState currentChallenge;

// -------------------------------------------------------------
// Matrix generation
// -------------------------------------------------------------
vector<vector<int>> generateMatrix(int N) {
    vector<vector<int>> mat(N, vector<int>(N));
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            mat[i][j] = rand() % MODULO;
    return mat;
}

int computeChecksum(const vector<vector<int>>& A, const vector<vector<int>>& B) {
    int N = (int)A.size();
    int checksum = 0;

    vector<int> rowC(N);

    for (int i = 0; i < N; ++i) {
        fill(rowC.begin(), rowC.end(), 0);
        const int* rowA = &A[i][0];

        for (int k = 0; k < N; ++k) {
            int a = rowA[k];
            const int* rowB = &B[k][0];
            for (int j = 0; j < N; ++j) {
                rowC[j] = (rowC[j] + (int)((1LL * a * rowB[j]) % MODULO)) % MODULO;
            }
        }

        for (int j = 0; j < N; ++j)
            checksum = (checksum + rowC[j]) % MODULO;
    }

    return checksum;
}

// -------------------------------------------------------------
// Logging
// -------------------------------------------------------------
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

    for (auto& [name, latency] : latencies) {
        auto it = answers.find(name);
        if (it == answers.end()) continue;
        if (it->second == correctAnswer && latency < bestLatency) {
            bestLatency = latency;
            winner = name;
        }
    }

    entry["winner"] = winner.empty() ? nullptr : json(winner);

    for (auto& [name, latency] : latencies) {
        json p;
        p["name"] = name;
        p["latency_ms"] = latency;
        if (answers.count(name)) {
            p["answer"] = answers.at(name);
            p["correct"] = (answers.at(name) == correctAnswer);
        } else {
            p["answer"] = nullptr;
            p["correct"] = false;
        }
        entry["players"].push_back(p);
    }

    json all = json::array();
    ifstream in(RESULTS_FILE);
    if (in) {
        try { in >> all; } catch (...) {}
    }

    all.push_back(entry);
    ofstream out(RESULTS_FILE);
    out << all.dump(2);
}

// -------------------------------------------------------------
// Sender thread
// -------------------------------------------------------------
void senderThread(ClientInfo* client) {
    while (client->running) {
        unique_lock<mutex> lock(client->qMutex);
        client->cv.wait(lock, [&]{ return !client->outbox.empty() || !client->running; });

        while (!client->outbox.empty()) {
            string msg = std::move(client->outbox.front());
            client->outbox.pop();
            lock.unlock();

            send(client->socket, msg.c_str(), msg.size(), 0);

            lock.lock();
        }
    }
}

// -------------------------------------------------------------
// Helper
// -------------------------------------------------------------
static inline void rtrim(string &s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
}

// -------------------------------------------------------------
// Client handler
// -------------------------------------------------------------
void handleClient(ClientInfo* client) {
    char buffer[BUFFER_SIZE];

    // Receive name
    int bytes = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes <= 0) return;
    client->name = string(buffer, bytes);
    rtrim(client->name);
    cout << "👤 Registered client: " << client->name << endl;

    thread sender(senderThread, client);
    sender.detach();

    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        bytes = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;

        string msg(buffer, bytes);
        rtrim(msg);

        int cid = -1, ans = 0;
        int n = sscanf(msg.c_str(), "%d %d", &cid, &ans);
        if (n == 1) { ans = cid; cid = -1; }
        else if (n != 2) continue;

        auto now = steady_clock::now();

        lock_guard<mutex> lock(challengeMutex);
        int active = currentChallenge.id;
        if (cid == -1) cid = active;
        if (cid != active) continue;

        int latency = duration_cast<milliseconds>(now - currentChallenge.startTime).count();
        currentChallenge.latencies[client->name] = latency;
        currentChallenge.answers[client->name] = ans;
    }

    client->running = false;
    client->cv.notify_all();
    close(client->socket);
}

// -------------------------------------------------------------
// Challenge broadcaster (with modes)
// -------------------------------------------------------------
void broadcastChallengeLoop() {
    while (true) {
        int cid = challengeId++;

        int N = gConfig.matrixSize;
        int cps = gConfig.challengesPerSecond;
        int windowMs = gConfig.answerWindowMs;

        int interval_us = cps > 0 ? 1'000'000 / cps : 1'000'000;

        // Generate matrices (parallel)
        vector<vector<int>> A, B;
        thread t1([&]{ A = generateMatrix(N); });
        thread t2([&]{ B = generateMatrix(N); });
        t1.join();
        t2.join();

        int correct = computeChecksum(A, B);

        {
            lock_guard<mutex> lock(challengeMutex);
            currentChallenge.id = cid;
            currentChallenge.A = A;
            currentChallenge.B = B;
            currentChallenge.startTime = steady_clock::now();
            currentChallenge.latencies.clear();
            currentChallenge.answers.clear();
        }

        // Serialize challenge
        stringstream ss;
        ss << cid << "\n";
        ss << N << "\n";
        for (auto& r : A) for (int v : r) ss << v << " ";
        ss << "\n";
        for (auto& r : B) for (int v : r) ss << v << " ";
        ss << "\n";

        string payload = ss.str();

        {
            lock_guard<mutex> lock(clientsMutex);
            for (auto& c : clients) {
                lock_guard<mutex> ql(c->qMutex);
                c->outbox.push(payload);
                c->cv.notify_one();
            }
        }

        // Answer window
        this_thread::sleep_for(milliseconds(windowMs));

        unordered_map<string,int> lat, ans;
        {
            lock_guard<mutex> lock(challengeMutex);
            lat = currentChallenge.latencies;
            ans = currentChallenge.answers;
        }

        {
            lock_guard<mutex> lock(logMutex);
            logChallengeResult(cid, lat, ans, correct);
        }

        // Mode-dependent timing behavior
        if (gConfig.mode == 0) {
            // Normal: fixed interval
            this_thread::sleep_for(microseconds(interval_us));
        } else if (gConfig.mode == 1) {
            // Heavy: small jitter
            int jitter = (rand() % 400) - 200; // -200..+199 us
            this_thread::sleep_for(microseconds(max(100, interval_us + jitter)));
        } else {
            // Ultra: bursts + jitter + partial overlap
            static int counter = 0;
            counter++;

            if (counter % 50 == 0) {
                // Burst: 500 challenges in ~100ms
                for (int i = 0; i < 20; ++i) {
                    int burstCid = challengeId++;
                    vector<vector<int>> BA = generateMatrix(N);
                    vector<vector<int>> BB = generateMatrix(N);
                    int bCorrect = computeChecksum(BA, BB);

                    stringstream bss;
                    bss << burstCid << "\n";
                    bss << N << "\n";
                    for (auto& r : BA) for (int v : r) bss << v << " ";
                    bss << "\n";
                    for (auto& r : BB) for (int v : r) bss << v << " ";
                    bss << "\n";

                    string bPayload = bss.str();

                    {
                        lock_guard<mutex> lock(clientsMutex);
                        for (auto& c : clients) {
                            lock_guard<mutex> ql(c->qMutex);
                            c->outbox.push(bPayload);
                            c->cv.notify_one();
                        }
                    }

                    // Log burst challenge with empty answers (optional)
                    {
                        lock_guard<mutex> lock(logMutex);
                        unordered_map<string,int> emptyLat, emptyAns;
                        logChallengeResult(burstCid, emptyLat, emptyAns, bCorrect);
                    }
                }
            }

            int jitter = (rand() % 1000) - 500; // -500..+499 us
            int sleepUs = max(50, interval_us / 2 + jitter);
            this_thread::sleep_for(microseconds(sleepUs));
        }
    }
}

// -------------------------------------------------------------
// Server
// -------------------------------------------------------------
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

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    getsockname(serverSocket, (sockaddr*)&actual, &len);
    cout << "🚀 Server successfully bound to port " << ntohs(actual.sin_port) << endl;

    if (listen(serverSocket, 64) < 0) {
        perror("Listen failed");
        close(serverSocket);
        exit(EXIT_FAILURE);
    }

    cout << "MatrixSize=" << gConfig.matrixSize
         << " CPS=" << gConfig.challengesPerSecond
         << " WindowMs=" << gConfig.answerWindowMs
         << " Mode=" << gConfig.mode << endl;

    thread broadcaster(broadcastChallengeLoop);
    broadcaster.detach();

    while (true) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cs = accept(serverSocket, (sockaddr*)&caddr, &clen);
        if (cs < 0) continue;

        auto client = make_unique<ClientInfo>();
        client->socket = cs;
        ClientInfo* ptr = client.get();

        {
            lock_guard<mutex> lock(clientsMutex);
            clients.push_back(std::move(client));
        }

        thread t(handleClient, ptr);
        t.detach();
    }
}

// -------------------------------------------------------------
// Simple CLI parsing
// -------------------------------------------------------------
void parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        auto nextInt = [&](int& dst) {
            if (i + 1 < argc) {
                dst = stoi(argv[++i]);
            }
        };

        if (arg == "--rate" || arg == "-r") {
            nextInt(gConfig.challengesPerSecond);
        } else if (arg == "--window" || arg == "-w") {
            nextInt(gConfig.answerWindowMs);
        } else if (arg == "--size" || arg == "-s") {
            nextInt(gConfig.matrixSize);
        } else if (arg == "--mode" || arg == "-m") {
            nextInt(gConfig.mode);
        }
    }
}

int main(int argc, char** argv) {
    srand((unsigned)time(nullptr));

    // Defaults: mild blast
    gConfig.matrixSize = 128;
    gConfig.challengesPerSecond = 50;
    gConfig.answerWindowMs = 30;
    gConfig.mode = 0;

    parseArgs(argc, argv);

    startServer();
    return 0;
}
