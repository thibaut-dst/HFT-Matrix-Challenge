#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

namespace {
constexpr size_t kRingBufferCapacity = 1u << 20; // 1 MiB
constexpr int kModulo = 997;
constexpr int kComputeWorkers = 7;
constexpr int kMaxMatrixDim = 512;
constexpr size_t kMaxCells = static_cast<size_t>(kMaxMatrixDim) * static_cast<size_t>(kMaxMatrixDim);
} // namespace

struct SharedJob {
    int challenge_id = 0;
    int N = 0;
    vector<int> A;
    vector<int> B;
    array<int, kComputeWorkers> partials{};
    atomic<uint64_t> epoch{0};
    atomic<int> done_count{0};

    SharedJob() : A(kMaxCells), B(kMaxCells) {}
};

class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacity)
        : buffer_(capacity), mask_(capacity - 1) {}

    char* reserveWriteSpan(size_t& span_out) {
        if (closed_.load(memory_order_acquire)) {
            span_out = 0;
            return nullptr;
        }

        const size_t head = head_.load(memory_order_relaxed);
        const size_t tail = tail_.load(memory_order_acquire);
        const size_t used = head - tail;
        if (used == buffer_.size()) {
            writer_waits_.fetch_add(1, memory_order_relaxed);
            span_out = 0;
            return nullptr;
        }

        const size_t free = buffer_.size() - used;
        const size_t idx = head & mask_;
        span_out = min(free, buffer_.size() - idx);
        return buffer_.data() + idx;
    }

    void commitWrite(size_t n) {
        head_.fetch_add(n, memory_order_release);
    }

    const char* peekReadSpan(size_t& span_out) {
        const size_t tail = tail_.load(memory_order_relaxed);
        const size_t head = head_.load(memory_order_acquire);
        const size_t used = head - tail;
        if (used == 0) {
            if (closed_.load(memory_order_acquire)) {
                span_out = 0;
                return nullptr;
            }
            reader_waits_.fetch_add(1, memory_order_relaxed);
            span_out = 0;
            return nullptr;
        }

        const size_t idx = tail & mask_;
        span_out = min(used, buffer_.size() - idx);
        return buffer_.data() + idx;
    }

    void consume(size_t n) {
        tail_.fetch_add(n, memory_order_release);
    }

    void close() {
        closed_.store(true, memory_order_release);
    }

    bool closed() const {
        return closed_.load(memory_order_acquire);
    }

    uint64_t writerWaits() const {
        return writer_waits_.load(memory_order_relaxed);
    }

    uint64_t readerWaits() const {
        return reader_waits_.load(memory_order_relaxed);
    }

private:
    vector<char> buffer_;
    const size_t mask_;
    atomic<size_t> head_{0}; // producer-owned
    atomic<size_t> tail_{0}; // consumer-owned
    atomic<bool> closed_{false};
    atomic<uint64_t> writer_waits_{0};
    atomic<uint64_t> reader_waits_{0};
};

class ChallengeParser {
public:
    explicit ChallengeParser(SharedJob& job) : job_(job) {}

    size_t feed(const char* data, size_t len) {
        size_t i = 0;
        for (; i < len && !completed_; ++i) {
            processChar(data[i]);
        }
        return i;
    }

    bool completed() const {
        return completed_;
    }

    void reset() {
        phase_ = Phase::ChallengeId;
        in_number_ = false;
        sign_ = 1;
        current_value_ = 0;
        a_index_ = 0;
        b_index_ = 0;
        completed_ = false;
    }

private:
    enum class Phase {
        ChallengeId,
        N,
        A,
        B
    };

    void processChar(char c) {
        const unsigned char uc = static_cast<unsigned char>(c);

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
                finalizeNumber();
            }
            return;
        }

        reset();
    }

    void startNumber() {
        in_number_ = true;
        sign_ = 1;
        current_value_ = 0;
    }

    size_t totalCells() const {
        return static_cast<size_t>(job_.N) * static_cast<size_t>(job_.N);
    }

    void finalizeNumber() {
        const int value = sign_ * current_value_;
        in_number_ = false;
        sign_ = 1;
        current_value_ = 0;

        switch (phase_) {
            case Phase::ChallengeId:
                job_.challenge_id = value;
                phase_ = Phase::N;
                break;

            case Phase::N:
                if (value <= 0 || value > kMaxMatrixDim) {
                    reset();
                    break;
                }
                job_.N = value;
                a_index_ = 0;
                b_index_ = 0;
                phase_ = Phase::A;
                break;

            case Phase::A: {
                const size_t total = totalCells();
                if (a_index_ < total) {
                    job_.A[a_index_++] = value;
                }
                if (a_index_ == total) {
                    phase_ = Phase::B;
                }
                break;
            }

            case Phase::B: {
                const size_t total = totalCells();
                if (b_index_ < total) {
                    job_.B[b_index_++] = value;
                }
                if (b_index_ == total) {
                    completed_ = true;
                }
                break;
            }
        }
    }

    SharedJob& job_;
    Phase phase_ = Phase::ChallengeId;
    bool in_number_ = false;
    int sign_ = 1;
    int current_value_ = 0;
    size_t a_index_ = 0;
    size_t b_index_ = 0;
    bool completed_ = false;
};

static bool sendAll(int sock, const string& msg) {
    size_t sent = 0;
    while (sent < msg.size()) {
        const ssize_t n = send(sock, msg.data() + sent, msg.size() - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static int computePartialChecksum(const SharedJob& job, int worker_id) {
    const int N = job.N;
    const size_t start_row = (static_cast<size_t>(worker_id) * static_cast<size_t>(N)) / kComputeWorkers;
    const size_t end_row = (static_cast<size_t>(worker_id + 1) * static_cast<size_t>(N)) / kComputeWorkers;

    int partial = 0;
    for (size_t i = start_row; i < end_row; ++i) {
        const size_t row_base = i * static_cast<size_t>(N);
        for (int j = 0; j < N; ++j) {
            int cij = 0;
            for (int k = 0; k < N; ++k) {
                const int term = static_cast<int>((1LL * job.A[row_base + static_cast<size_t>(k)] * job.B[static_cast<size_t>(k) * static_cast<size_t>(N) + static_cast<size_t>(j)]) % kModulo);
                cij += term;
                if (cij >= kModulo) {
                    cij -= kModulo;
                }
            }
            partial += cij;
            if (partial >= kModulo) {
                partial -= kModulo;
            }
        }
    }

    return partial;
}

static void ioReceiverThread(int sock, SpscRingBuffer& ring, atomic<bool>& stop_requested) {
    while (!stop_requested.load(memory_order_relaxed)) {
        size_t span = 0;
        char* dst = ring.reserveWriteSpan(span);
        if (span == 0) {
            if (ring.closed()) {
                break;
            }
            this_thread::yield();
            continue;
        }

        const ssize_t n = recv(sock, dst, span, 0);
        if (n > 0) {
            ring.commitWrite(static_cast<size_t>(n));
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    stop_requested.store(true, memory_order_relaxed);
    ring.close();
}

static void workerThread(int worker_id, SharedJob& job, atomic<bool>& stop_requested) {
    uint64_t seen_epoch = 0;

    while (!stop_requested.load(memory_order_relaxed)) {
        const uint64_t epoch = job.epoch.load(memory_order_acquire);
        if (epoch == seen_epoch) {
            this_thread::yield();
            continue;
        }

        seen_epoch = epoch;
        if (stop_requested.load(memory_order_relaxed)) {
            break;
        }

        job.partials[worker_id] = computePartialChecksum(job, worker_id);
        job.done_count.fetch_add(1, memory_order_release);
    }
}

static void parserThread(SpscRingBuffer& ring, SharedJob& job, atomic<bool>& stop_requested, int sock) {
    ChallengeParser parser(job);

    while (!stop_requested.load(memory_order_relaxed)) {
        size_t span = 0;
        const char* src = ring.peekReadSpan(span);
        if (span == 0) {
            if (ring.closed()) {
                break;
            }
            this_thread::yield();
            continue;
        }

        const size_t consumed = parser.feed(src, span);
        ring.consume(consumed);

        if (!parser.completed()) {
            continue;
        }

        job.done_count.store(0, memory_order_release);
        job.epoch.fetch_add(1, memory_order_release);

        while (job.done_count.load(memory_order_acquire) < kComputeWorkers) {
            if (stop_requested.load(memory_order_relaxed)) {
                break;
            }
            this_thread::yield();
        }

        int checksum = 0;
        for (int i = 0; i < kComputeWorkers; ++i) {
            checksum += job.partials[i];
            if (checksum >= kModulo) {
                checksum -= kModulo;
            }
        }

        const string response = to_string(job.challenge_id) + " " + to_string(checksum) + "\n";
        if (!sendAll(sock, response)) {
            stop_requested.store(true, memory_order_relaxed);
            break;
        }

        parser.reset();
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <host> <port> <team_name>\n";
        return 1;
    }

    const string host = argv[1];
    const int port = stoi(argv[2]);
    const string team = argv[3];

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
    if (!sendAll(sock, intro)) {
        cerr << "Failed to send team name.\n";
        close(sock);
        return 1;
    }

    cout << "Connected to server at " << host << ":" << port << '\n';
    cout << "Waiting for challenges...\n";

    SharedJob job;
    SpscRingBuffer ring(kRingBufferCapacity);
    atomic<bool> stop_requested{false};

    array<thread, kComputeWorkers> workers;
    for (int i = 0; i < kComputeWorkers; ++i) {
        workers[i] = thread(workerThread, i, ref(job), ref(stop_requested));
    }

    thread io_thread(ioReceiverThread, sock, ref(ring), ref(stop_requested));
    thread parse_thread(parserThread, ref(ring), ref(job), ref(stop_requested), sock);

    io_thread.join();
    parse_thread.join();

    stop_requested.store(true, memory_order_relaxed);
    job.epoch.fetch_add(1, memory_order_release);

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    close(sock);

    cout << "Ring writer waits: " << ring.writerWaits() << '\n';
    cout << "Ring reader waits: " << ring.readerWaits() << '\n';
    return 0;
}
