#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <sys/qos.h>
#endif

using namespace std;
using namespace std::chrono;

namespace {
constexpr size_t kRingBufferCapacity = 1u << 22; // 4 MiB
constexpr int kModulo = 997;
constexpr int kComputeWorkers = 8;
constexpr int kMaxMatrixDim = 512;
constexpr int kComputeTile = 64;
constexpr int kSocketRecvBufferBytes = 1 << 22;
constexpr size_t kMaxCells = static_cast<size_t>(kMaxMatrixDim) * static_cast<size_t>(kMaxMatrixDim);
} // namespace

static void markComputeThread() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct BenchmarkStats {
    atomic<uint64_t> challenges{0};
    atomic<uint64_t> bytes{0};
    atomic<uint64_t> parse_wall_ns{0};
    atomic<uint64_t> parse_cpu_ns{0};
    atomic<uint64_t> compute_wait_ns{0};
    atomic<uint64_t> send_ns{0};
    atomic<uint64_t> latency_ns{0};
};

struct SharedJob {
    int challenge_id = 0;
    int N = 0;
    vector<int> A;
    vector<int> B;
    array<int, kComputeWorkers> partials{};
    atomic<uint64_t> epoch{0};
    atomic<int> done_count{0};
    atomic<int> next_tile{0};

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
    atomic<size_t> head_{0};
    atomic<size_t> tail_{0};
    atomic<bool> closed_{false};
    atomic<uint64_t> writer_waits_{0};
    atomic<uint64_t> reader_waits_{0};
};

class ChallengeParser {
public:
    explicit ChallengeParser(SharedJob& job, bool benchmark_enabled)
        : job_(job), benchmark_enabled_(benchmark_enabled) {}

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
        challenge_started_ = false;
    }

    bool challengeStarted() const {
        return challenge_started_;
    }

    steady_clock::time_point challengeStartTime() const {
        return challenge_start_;
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

        if (benchmark_enabled_ && !challenge_started_ && !isspace(uc)) {
            challenge_start_ = steady_clock::now();
            challenge_started_ = true;
        }

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
    bool benchmark_enabled_ = false;
    bool challenge_started_ = false;
    steady_clock::time_point challenge_start_{};
};

static bool sendAll(int sock, const char* msg, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const ssize_t n = send(sock, msg + sent, size - sent, 0);
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

static bool sendAll(int sock, const string& msg) {
    return sendAll(sock, msg.data(), msg.size());
}

static int computePartialChecksum(SharedJob& job, int worker_id) {
    (void)worker_id;
    const int N = job.N;
    const int row_tiles = (N + kComputeTile - 1) / kComputeTile;
    const int k_tiles = (N + kComputeTile - 1) / kComputeTile;
    const int j_tiles = (N + kComputeTile - 1) / kComputeTile;
    const int total_tiles = row_tiles * k_tiles * j_tiles;

    int64_t partial = 0;

    while (true) {
        const int tile = job.next_tile.fetch_add(1, memory_order_relaxed);
        if (tile >= total_tiles) {
            break;
        }

        const int j_tile = tile % j_tiles;
        const int k_tile = (tile / j_tiles) % k_tiles;
        const int i_tile = tile / (j_tiles * k_tiles);

        const size_t ii = static_cast<size_t>(i_tile * kComputeTile);
        const int kk = k_tile * kComputeTile;
        const int jj = j_tile * kComputeTile;
        const size_t i_end = min(static_cast<size_t>(N), ii + static_cast<size_t>(kComputeTile));
        const int k_end = min(N, kk + kComputeTile);
        const int j_end = min(N, jj + kComputeTile);

        for (size_t i = ii; i < i_end; ++i) {
            const size_t row_base = i * static_cast<size_t>(N);
            for (int k = kk; k < k_end; ++k) {
                const int a = job.A[row_base + static_cast<size_t>(k)];
                const size_t b_base = static_cast<size_t>(k) * static_cast<size_t>(N);
                if (k + 1 < k_end) {
                    __builtin_prefetch(job.B.data() + static_cast<size_t>(k + 1) * static_cast<size_t>(N) + static_cast<size_t>(jj), 0, 1);
                }
                for (int j = jj; j < j_end; ++j) {
                    partial += static_cast<int64_t>(a) * static_cast<int64_t>(job.B[b_base + static_cast<size_t>(j)]);
                }
            }
        }
    }

    return static_cast<int>(partial % kModulo);
}

static void ioReceiverThread(int sock, SpscRingBuffer& ring, atomic<bool>& stop_requested, BenchmarkStats* stats) {
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
            if (stats != nullptr) {
                stats->bytes.fetch_add(static_cast<uint64_t>(n), memory_order_relaxed);
            }
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
    markComputeThread();

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

static void parserThread(SpscRingBuffer& ring, SharedJob& job, atomic<bool>& stop_requested, int sock, BenchmarkStats* stats) {
    ChallengeParser parser(job, stats != nullptr);

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

        const auto parse_cpu_start = stats != nullptr ? steady_clock::now() : steady_clock::time_point{};
        const size_t consumed = parser.feed(src, span);
        if (stats != nullptr) {
            const auto parse_cpu_end = steady_clock::now();
            stats->parse_cpu_ns.fetch_add(static_cast<uint64_t>(duration_cast<nanoseconds>(parse_cpu_end - parse_cpu_start).count()),
                                          memory_order_relaxed);
        }
        ring.consume(consumed);
        if (!parser.completed()) {
            continue;
        }

        const auto parse_complete = steady_clock::now();
        if (stats != nullptr && parser.challengeStarted()) {
            stats->parse_wall_ns.fetch_add(static_cast<uint64_t>(duration_cast<nanoseconds>(parse_complete - parser.challengeStartTime()).count()),
                                           memory_order_relaxed);
        }

        const auto compute_start = parse_complete;
        job.next_tile.store(0, memory_order_relaxed);
        job.done_count.store(0, memory_order_release);
        job.epoch.fetch_add(1, memory_order_release);

        while (job.done_count.load(memory_order_acquire) < kComputeWorkers) {
            if (stop_requested.load(memory_order_relaxed)) {
                break;
            }
            this_thread::yield();
        }

        if (stats != nullptr) {
            const auto compute_end = steady_clock::now();
            stats->compute_wait_ns.fetch_add(static_cast<uint64_t>(duration_cast<nanoseconds>(compute_end - compute_start).count()),
                                             memory_order_relaxed);
        }

        int checksum = 0;
        for (int i = 0; i < kComputeWorkers; ++i) {
            checksum += job.partials[i];
            if (checksum >= kModulo) {
                checksum -= kModulo;
            }
        }

        char response[64];
        const int response_len = snprintf(response, sizeof(response), "%d %d\n", job.challenge_id, checksum);
        steady_clock::time_point send_start;
        if (stats != nullptr) {
            send_start = steady_clock::now();
        }
        if (response_len <= 0 || !sendAll(sock, response, static_cast<size_t>(response_len))) {
            stop_requested.store(true, memory_order_relaxed);
            break;
        }

        if (stats != nullptr) {
            const auto send_end = steady_clock::now();
            stats->send_ns.fetch_add(static_cast<uint64_t>(duration_cast<nanoseconds>(send_end - send_start).count()),
                                     memory_order_relaxed);
            if (parser.challengeStarted()) {
                stats->latency_ns.fetch_add(static_cast<uint64_t>(duration_cast<nanoseconds>(send_end - parser.challengeStartTime()).count()),
                                            memory_order_relaxed);
            }
            stats->challenges.fetch_add(1, memory_order_relaxed);
        }

        parser.reset();
    }
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <host> <port> <team_name> [--benchmark]\n", argv[0]);
        return 1;
    }

    const string host = argv[1];
    const int port = stoi(argv[2]);
    const string team = argv[3];
    const bool benchmark_enabled = (argc == 5 && string(argv[4]) == "--benchmark");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int recv_buffer = kSocketRecvBufferBytes;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buffer, sizeof(recv_buffer));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid host address: %s\n", host.c_str());
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
        fprintf(stderr, "Failed to send team name.\n");
        close(sock);
        return 1;
    }

    SharedJob job;
    SpscRingBuffer ring(kRingBufferCapacity);
    atomic<bool> stop_requested{false};
    BenchmarkStats benchmark_stats;
    BenchmarkStats* stats_ptr = benchmark_enabled ? &benchmark_stats : nullptr;

    array<thread, kComputeWorkers> workers;
    for (int i = 0; i < kComputeWorkers; ++i) {
        workers[i] = thread(workerThread, i, ref(job), ref(stop_requested));
    }

    thread io_thread(ioReceiverThread, sock, ref(ring), ref(stop_requested), stats_ptr);
    thread parse_thread(parserThread, ref(ring), ref(job), ref(stop_requested), sock, stats_ptr);

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

    if (benchmark_enabled) {
        const uint64_t challenges = benchmark_stats.challenges.load(memory_order_relaxed);
        const uint64_t bytes = benchmark_stats.bytes.load(memory_order_relaxed);
        const uint64_t parse_wall_ns = benchmark_stats.parse_wall_ns.load(memory_order_relaxed);
        const uint64_t parse_cpu_ns = benchmark_stats.parse_cpu_ns.load(memory_order_relaxed);
        const uint64_t compute_wait_ns = benchmark_stats.compute_wait_ns.load(memory_order_relaxed);
        const uint64_t send_ns = benchmark_stats.send_ns.load(memory_order_relaxed);
        const uint64_t latency_ns = benchmark_stats.latency_ns.load(memory_order_relaxed);
        const double avg_latency_ms = challenges == 0 ? 0.0 : (latency_ns / 1e6) / static_cast<double>(challenges);
        const double avg_parse_wall_ms = challenges == 0 ? 0.0 : (parse_wall_ns / 1e6) / static_cast<double>(challenges);
        const double avg_parse_cpu_ms = challenges == 0 ? 0.0 : (parse_cpu_ns / 1e6) / static_cast<double>(challenges);
        const double avg_compute_ms = challenges == 0 ? 0.0 : (compute_wait_ns / 1e6) / static_cast<double>(challenges);
        const double avg_send_ms = challenges == 0 ? 0.0 : (send_ns / 1e6) / static_cast<double>(challenges);

        fprintf(stderr,
                "[bench] challenges=%llu bytes=%llu total_latency_ms=%.3f avg_latency_ms=%.3f parse_wall_ms=%.3f parse_cpu_ms=%.3f compute_ms=%.3f send_ms=%.3f ring_writer_waits=%llu ring_reader_waits=%llu\n",
                static_cast<unsigned long long>(challenges),
                static_cast<unsigned long long>(bytes),
                latency_ns / 1e6,
                avg_latency_ms,
                avg_parse_wall_ms,
                avg_parse_cpu_ms,
                avg_compute_ms,
                avg_send_ms,
                static_cast<unsigned long long>(ring.writerWaits()),
                static_cast<unsigned long long>(ring.readerWaits()));
    }

    return 0;
}
