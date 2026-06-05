#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

namespace {
constexpr size_t kRingBufferCapacity = 256 * 1024;
constexpr size_t kRecvBufferSize = 16 * 1024;
constexpr size_t kParseChunkSize = 8192;
constexpr int kModulo = 997;
constexpr int kComputeWorkers = 7;
constexpr int kMaxMatrixDim = 512;
}  // namespace

struct Job {
    int challenge_id = 0;
    int N = 0;
    vector<int> A;
    vector<int> B;
};

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : buffer_(capacity) {}

    void close() {
        lock_guard<mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool write(const char* data, size_t len) {
        size_t written = 0;
        unique_lock<mutex> lock(mutex_);

        while (written < len) {
            not_full_.wait(lock, [&] { return closed_ || size_used_ < buffer_.size(); });
            if (closed_) {
                return false;
            }

            size_t space = buffer_.size() - size_used_;
            if (space == 0) {
                continue;
            }

            size_t chunk = min(space, len - written);
            size_t first = min(chunk, buffer_.size() - write_pos_);
            memcpy(buffer_.data() + write_pos_, data + written, first);
            write_pos_ = (write_pos_ + first) % buffer_.size();
            size_used_ += first;
            written += first;

            size_t second = chunk - first;
            if (second > 0) {
                memcpy(buffer_.data() + write_pos_, data + written, second);
                write_pos_ = (write_pos_ + second) % buffer_.size();
                size_used_ += second;
                written += second;
            }

            not_empty_.notify_one();
        }

        return true;
    }

    size_t read(char* out, size_t max_len) {
        unique_lock<mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || size_used_ > 0; });

        if (size_used_ == 0 && closed_) {
            return 0;
        }

        size_t chunk = min(max_len, size_used_);
        size_t first = min(chunk, buffer_.size() - read_pos_);
        memcpy(out, buffer_.data() + read_pos_, first);
        read_pos_ = (read_pos_ + first) % buffer_.size();
        size_used_ -= first;

        size_t second = chunk - first;
        if (second > 0) {
            memcpy(out + first, buffer_.data() + read_pos_, second);
            read_pos_ = (read_pos_ + second) % buffer_.size();
            size_used_ -= second;
        }

        not_full_.notify_one();
        return chunk;
    }

private:
    vector<char> buffer_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    size_t size_used_ = 0;
    bool closed_ = false;
    mutex mutex_;
    condition_variable not_empty_;
    condition_variable not_full_;
};

class JobQueue {
public:
    void push(unique_ptr<Job> job) {
        lock_guard<mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push(std::move(job));
        cv_.notify_one();
    }

    bool pop(unique_ptr<Job>& job) {
        unique_lock<mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        job = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close() {
        lock_guard<mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    queue<unique_ptr<Job>> queue_;
    bool closed_ = false;
    mutex mutex_;
    condition_variable cv_;
};

class ChallengeStreamParser {
public:
    explicit ChallengeStreamParser(int max_matrix_dim)
        : max_matrix_dim_(max_matrix_dim) {}

    template <typename EmitFn>
    void feed(const char* data, size_t len, EmitFn& emit) {
        for (size_t i = 0; i < len; ++i) {
            processChar(data[i], emit);
        }
    }

    template <typename EmitFn>
    void finish(EmitFn& emit) {
        if (in_number_) {
            finalizeNumber(emit);
        }
    }

private:
    enum class Phase {
        ChallengeId,
        N,
        A,
        B
    };

    template <typename EmitFn>
    void processChar(char c, EmitFn& emit) {
        unsigned char uc = static_cast<unsigned char>(c);

        if (isdigit(uc)) {
            if (!in_number_) {
                startNumber();
            }
            current_value_ = current_value_ * 10 + (c - '0');
            return;
        }

        if (c == '-' && !in_number_) {
            in_number_ = true;
            sign_ = -1;
            current_value_ = 0;
            return;
        }

        if (isspace(uc)) {
            if (in_number_) {
                finalizeNumber(emit);
            }
            return;
        }

        resetChallenge();
    }

    void startNumber() {
        in_number_ = true;
        sign_ = 1;
        current_value_ = 0;
    }

    template <typename EmitFn>
    void finalizeNumber(EmitFn& emit) {
        int value = sign_ * current_value_;
        in_number_ = false;
        sign_ = 1;
        current_value_ = 0;

        switch (phase_) {
            case Phase::ChallengeId:
                challenge_id_ = value;
                phase_ = Phase::N;
                break;

            case Phase::N: {
                if (value <= 0 || value > max_matrix_dim_) {
                    resetChallenge();
                    break;
                }

                N_ = value;
                const size_t total = static_cast<size_t>(N_) * static_cast<size_t>(N_);
                A_.assign(total, 0);
                B_.assign(total, 0);
                a_index_ = 0;
                b_index_ = 0;
                phase_ = Phase::A;
                break;
            }

            case Phase::A:
                if (a_index_ < A_.size()) {
                    A_[a_index_++] = value;
                }
                if (a_index_ == A_.size()) {
                    phase_ = Phase::B;
                }
                break;

            case Phase::B:
                if (b_index_ < B_.size()) {
                    B_[b_index_++] = value;
                }
                if (b_index_ == B_.size()) {
                    auto job = make_unique<Job>();
                    job->challenge_id = challenge_id_;
                    job->N = N_;
                    job->A = std::move(A_);
                    job->B = std::move(B_);
                    emit(std::move(job));
                    resetChallenge();
                }
                break;
        }
    }

    void resetChallenge() {
        phase_ = Phase::ChallengeId;
        challenge_id_ = 0;
        N_ = 0;
        a_index_ = 0;
        b_index_ = 0;
        A_.clear();
        B_.clear();
        in_number_ = false;
        sign_ = 1;
        current_value_ = 0;
    }

    Phase phase_ = Phase::ChallengeId;
    bool in_number_ = false;
    int sign_ = 1;
    int current_value_ = 0;
    int challenge_id_ = 0;
    int N_ = 0;
    size_t a_index_ = 0;
    size_t b_index_ = 0;
    vector<int> A_;
    vector<int> B_;
    int max_matrix_dim_ = 0;
};

static bool sendAll(int sock, const string& msg, mutex& send_mutex, atomic<bool>& stop_requested) {
    lock_guard<mutex> lock(send_mutex);

    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = send(sock, msg.data() + sent, msg.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }

        stop_requested.store(true);
        return false;
    }

    return true;
}

static int computeChecksum(const Job& job) {
    const int N = job.N;
    const vector<int>& A = job.A;
    const vector<int>& B = job.B;

    int checksum = 0;
    for (int i = 0; i < N; ++i) {
        const int row_base = i * N;
        for (int j = 0; j < N; ++j) {
            int cij = 0;
            for (int k = 0; k < N; ++k) {
                int term = static_cast<int>((1LL * A[row_base + k] * B[k * N + j]) % kModulo);
                cij += term;
                if (cij >= kModulo) {
                    cij -= kModulo;
                }
            }
            checksum += cij;
            if (checksum >= kModulo) {
                checksum -= kModulo;
            }
        }
    }

    return checksum;
}

static void ioReceiverThread(int sock, RingBuffer& ring, atomic<bool>& stop_requested) {
    vector<char> recv_buffer(kRecvBufferSize);

    while (!stop_requested.load()) {
        ssize_t n = recv(sock, recv_buffer.data(), recv_buffer.size(), 0);
        if (n > 0) {
            if (!ring.write(recv_buffer.data(), static_cast<size_t>(n))) {
                break;
            }
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    stop_requested.store(true);
    ring.close();
}

static void parserThread(RingBuffer& ring, JobQueue& jobs, atomic<bool>& stop_requested) {
    ChallengeStreamParser parser(kMaxMatrixDim);
    vector<char> chunk(kParseChunkSize);

    auto emit = [&](unique_ptr<Job> job) {
        jobs.push(std::move(job));
    };

    while (!stop_requested.load()) {
        size_t n = ring.read(chunk.data(), chunk.size());
        if (n == 0) {
            break;
        }

        parser.feed(chunk.data(), n, emit);
    }

    parser.finish(emit);
    jobs.close();
}

static void computeWorkerThread(int sock, JobQueue& jobs, mutex& send_mutex, atomic<bool>& stop_requested) {
    while (!stop_requested.load()) {
        unique_ptr<Job> job;
        if (!jobs.pop(job)) {
            break;
        }

        int checksum = computeChecksum(*job);
        string response = to_string(job->challenge_id) + " " + to_string(checksum) + "\n";

        if (!sendAll(sock, response, send_mutex, stop_requested)) {
            jobs.close();
            break;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <host> <port> <team_name>\n";
        return 1;
    }

    string host = argv[1];
    int port = stoi(argv[2]);
    string team = argv[3];

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        cerr << "Invalid host address: " << host << '\n';
        close(sock);
        return 1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    const string intro = team + "\n";
    size_t intro_sent = 0;
    while (intro_sent < intro.size()) {
        ssize_t n = send(sock, intro.data() + intro_sent, intro.size() - intro_sent, 0);
        if (n > 0) {
            intro_sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        perror("send");
        close(sock);
        return 1;
    }

    cout << "Connected to server at " << host << ":" << port << '\n';
    cout << "Waiting for challenges...\n";

    RingBuffer ring(kRingBufferCapacity);
    JobQueue jobs;
    atomic<bool> stop_requested{false};
    mutex send_mutex;

    thread io_thread(ioReceiverThread, sock, ref(ring), ref(stop_requested));
    thread parse_thread(parserThread, ref(ring), ref(jobs), ref(stop_requested));

    vector<thread> workers;
    workers.reserve(kComputeWorkers);
    for (int i = 0; i < kComputeWorkers; ++i) {
        workers.emplace_back(computeWorkerThread, sock, ref(jobs), ref(send_mutex), ref(stop_requested));
    }

    io_thread.join();
    parse_thread.join();
    for (auto& worker : workers) {
        worker.join();
    }

    close(sock);
    return 0;
}
