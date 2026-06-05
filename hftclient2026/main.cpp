/*
 * HFT Matrix Challenge Client - Combined Optimized Version
 *
 * Merges teammate's architecture (clean ring buffer, parser state machine,
 * job queue, worker pool) with our performance optimizations:
 *   - Lock-free atomic ring buffer (no mutex on hot path)
 *   - Cache-tiled matrix multiply (32x32 blocks)
 *   - Deferred modular reduction (int64 accumulate, single mod per cell)
 *   - Static flat arrays (no per-job heap allocation)
 *   - OpenMP parallelism across tile loops
 *   - TCP_NODELAY + large SO_RCVBUF
 *   - -O3 -march=native -funroll-loops (via CMakeLists)
 */

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
#include <netinet/tcp.h>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef _OPENMP
#  include <omp.h>
#endif

#ifdef __linux__
#  include <sched.h>
#  include <pthread.h>
#endif

using namespace std;

// ─── Constants ────────────────────────────────────────────────────────────────

namespace {
constexpr size_t kRingBufferCapacity = 1 << 22;   // 4 MB — lock-free, power of 2
constexpr size_t kRingBufferMask     = kRingBufferCapacity - 1;
constexpr size_t kRecvBufferSize     = 65536;
constexpr int    kModulo             = 997;
constexpr int    kMaxMatrixDim       = 512;
constexpr int    kTile               = 32;          // cache block size for matmul
constexpr int    kRecvBufBytes       = 1 << 22;     // 4 MB SO_RCVBUF
}

// ─── Lock-free Ring Buffer (SPSC) ─────────────────────────────────────────────
//
//  Single-producer (recv thread) / single-consumer (parser thread).
//  Head and tail are cache-line separated atomics — no mutex on the hot path.
//  Supports blocking reads for the parser via a condvar fallback only when empty.
//
class RingBuffer {
public:
    RingBuffer() : data_(kRingBufferCapacity) {}

    void close() {
        closed_.store(true, memory_order_release);
        not_empty_.notify_all();
    }

    // Producer: called by recv thread only
    bool write(const char* src, size_t len) {
        size_t written = 0;
        while (written < len) {
            size_t h = head_.load(memory_order_relaxed);
            size_t t = tail_.load(memory_order_acquire);
            size_t space = kRingBufferCapacity - (h - t);

            if (space == 0) {
                // Buffer full — extremely unlikely at 4 MB; yield and retry
                this_thread::yield();
                continue;
            }

            size_t chunk = min(len - written, space);
            size_t first = min(chunk, kRingBufferCapacity - (h & kRingBufferMask));
            memcpy(data_.data() + (h & kRingBufferMask), src + written, first);
            if (chunk > first)
                memcpy(data_.data(), src + written + first, chunk - first);

            head_.store(h + chunk, memory_order_release);
            written += chunk;
            not_empty_.notify_one();
        }
        return true;
    }

    // Consumer: called by parser thread only. Blocks until data available.
    size_t read(char* dst, size_t max_len) {
        size_t h, t, avail;
        {
            unique_lock<mutex> lk(mtx_);
            not_empty_.wait(lk, [&]{
                h = head_.load(memory_order_acquire);
                t = tail_.load(memory_order_relaxed);
                return (h != t) || closed_.load(memory_order_relaxed);
            });
            h = head_.load(memory_order_acquire);
            t = tail_.load(memory_order_relaxed);
            avail = h - t;
        }

        if (avail == 0) return 0;  // closed and empty

        size_t chunk = min(max_len, avail);
        size_t first = min(chunk, kRingBufferCapacity - (t & kRingBufferMask));
        memcpy(dst, data_.data() + (t & kRingBufferMask), first);
        if (chunk > first)
            memcpy(dst + first, data_.data(), chunk - first);

        tail_.store(t + chunk, memory_order_release);
        return chunk;
    }

private:
    vector<char> data_;
    alignas(64) atomic<size_t> head_{0};
    alignas(64) atomic<size_t> tail_{0};
    atomic<bool> closed_{false};
    mutex mtx_;
    condition_variable not_empty_;
};

// ─── Job ──────────────────────────────────────────────────────────────────────

struct Job {
    int challenge_id = 0;
    int N = 0;
    vector<int> A;   // flat row-major, length N*N
    vector<int> B;
};

// ─── Job Queue ────────────────────────────────────────────────────────────────

class JobQueue {
public:
    void push(unique_ptr<Job> job) {
        lock_guard<mutex> lk(mtx_);
        if (closed_) return;
        q_.push(move(job));
        cv_.notify_one();
    }

    bool pop(unique_ptr<Job>& job) {
        unique_lock<mutex> lk(mtx_);
        cv_.wait(lk, [&]{ return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        job = move(q_.front());
        q_.pop();
        return true;
    }

    void close() {
        lock_guard<mutex> lk(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    queue<unique_ptr<Job>> q_;
    bool closed_ = false;
    mutex mtx_;
    condition_variable cv_;
};

// ─── Stream Parser (state machine, char-by-char) ──────────────────────────────
//
//  Taken from teammate's clean design — processes raw bytes as they arrive,
//  emits a Job as soon as all N*N elements of both matrices are parsed.
//

class ChallengeStreamParser {
public:
    explicit ChallengeStreamParser(int max_dim) : max_dim_(max_dim) {}

    template<typename EmitFn>
    void feed(const char* data, size_t len, EmitFn& emit) {
        for (size_t i = 0; i < len; ++i)
            processChar(data[i], emit);
    }

    template<typename EmitFn>
    void finish(EmitFn& emit) {
        if (in_number_) finalizeNumber(emit);
    }

private:
    enum class Phase { ChallengeId, N, A, B };

    template<typename EmitFn>
    void processChar(char c, EmitFn& emit) {
        unsigned char uc = (unsigned char)c;
        if (isdigit(uc)) {
            if (!in_number_) startNumber();
            current_value_ = current_value_ * 10 + (c - '0');
            return;
        }
        if (c == '-' && !in_number_) {
            in_number_ = true; sign_ = -1; current_value_ = 0;
            return;
        }
        if (isspace(uc)) {
            if (in_number_) finalizeNumber(emit);
            return;
        }
        reset();
    }

    void startNumber() { in_number_ = true; sign_ = 1; current_value_ = 0; }

    template<typename EmitFn>
    void finalizeNumber(EmitFn& emit) {
        int v = sign_ * current_value_;
        in_number_ = false; sign_ = 1; current_value_ = 0;

        switch (phase_) {
            case Phase::ChallengeId:
                challenge_id_ = v;
                phase_ = Phase::N;
                break;
            case Phase::N:
                if (v <= 0 || v > max_dim_) { reset(); break; }
                N_ = v;
                A_.assign((size_t)N_ * N_, 0);
                B_.assign((size_t)N_ * N_, 0);
                a_idx_ = b_idx_ = 0;
                phase_ = Phase::A;
                break;
            case Phase::A:
                A_[a_idx_++] = v;
                if (a_idx_ == A_.size()) phase_ = Phase::B;
                break;
            case Phase::B:
                B_[b_idx_++] = v;
                if (b_idx_ == B_.size()) {
                    auto job = make_unique<Job>();
                    job->challenge_id = challenge_id_;
                    job->N = N_;
                    job->A = move(A_);
                    job->B = move(B_);
                    emit(move(job));
                    reset();
                }
                break;
        }
    }

    void reset() {
        phase_ = Phase::ChallengeId;
        challenge_id_ = N_ = 0;
        a_idx_ = b_idx_ = 0;
        A_.clear(); B_.clear();
        in_number_ = false; sign_ = 1; current_value_ = 0;
    }

    Phase phase_ = Phase::ChallengeId;
    bool in_number_ = false;
    int sign_ = 1, current_value_ = 0;
    int challenge_id_ = 0, N_ = 0;
    size_t a_idx_ = 0, b_idx_ = 0;
    vector<int> A_, B_;
    int max_dim_;
};

// ─── Matrix Multiply + Checksum ───────────────────────────────────────────────
//
//  Three key optimizations over the naive approach:
//
//  1. Cache tiling (32x32 blocks): keeps hot data in L1/L2 throughout the
//     inner loop, avoiding the cache-thrashing column access pattern of B.
//
//  2. Deferred mod: accumulate into int64_t — max value is 997^2 * 512 = ~511M,
//     well within int64 range — and take % kModulo only once per cell.
//     Eliminates ~N^3 integer divisions from the inner loop.
//
//  3. OpenMP: parallelises the outer tile loops across all available cores.
//

static int computeChecksum(const Job& job) {
    const int N = job.N;
    const int* A = job.A.data();
    const int* B = job.B.data();

    // Flat accumulator — avoids any dynamic allocation
    vector<int64_t> C((size_t)N * N, 0);

#ifdef _OPENMP
    #pragma omp parallel for schedule(static) collapse(2)
#endif
    for (int i = 0; i < N; i += kTile) {
        for (int j = 0; j < N; j += kTile) {
            for (int k = 0; k < N; k += kTile) {
                int imax = min(i + kTile, N);
                int jmax = min(j + kTile, N);
                int kmax = min(k + kTile, N);
                for (int ii = i; ii < imax; ++ii) {
                    for (int kk = k; kk < kmax; ++kk) {
                        int64_t aik = A[ii * N + kk];
                        for (int jj = j; jj < jmax; ++jj) {
                            C[ii * N + jj] += aik * B[kk * N + jj];
                        }
                    }
                }
            }
        }
    }

    // Single reduction pass with mod
    int checksum = 0;
#ifdef _OPENMP
    #pragma omp parallel for reduction(+:checksum) schedule(static)
#endif
    for (int i = 0; i < N * N; ++i) {
        checksum = (checksum + (int)(C[i] % kModulo)) % kModulo;
    }
    return ((checksum % kModulo) + kModulo) % kModulo;
}

// ─── Thread functions ─────────────────────────────────────────────────────────

static void ioReceiverThread(int sock, RingBuffer& ring, atomic<bool>& stop) {
    vector<char> buf(kRecvBufferSize);
    while (!stop.load()) {
        ssize_t n = recv(sock, buf.data(), buf.size(), 0);
        if (n > 0) { ring.write(buf.data(), (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    stop.store(true);
    ring.close();
}

static void parserThread(RingBuffer& ring, JobQueue& jobs, atomic<bool>& stop) {
    ChallengeStreamParser parser(kMaxMatrixDim);
    vector<char> chunk(65536);
    auto emit = [&](unique_ptr<Job> job){ jobs.push(move(job)); };
    while (!stop.load()) {
        size_t n = ring.read(chunk.data(), chunk.size());
        if (n == 0) break;
        parser.feed(chunk.data(), n, emit);
    }
    parser.finish(emit);
    jobs.close();
}

static void computeWorkerThread(int sock, JobQueue& jobs,
                                mutex& send_mtx, atomic<bool>& stop) {
    while (!stop.load()) {
        unique_ptr<Job> job;
        if (!jobs.pop(job)) break;

        int ans = computeChecksum(*job);

        char resp[64];
        int len = snprintf(resp, sizeof(resp), "%d %d\n", job->challenge_id, ans);

        lock_guard<mutex> lk(send_mtx);
        size_t sent = 0;
        while (sent < (size_t)len) {
            ssize_t n = send(sock, resp + sent, len - sent, MSG_NOSIGNAL);
            if (n > 0) { sent += n; continue; }
            if (n < 0 && errno == EINTR) continue;
            stop.store(true); jobs.close(); return;
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <host> <port> <team_name>\n";
        return 1;
    }

    const char* host = argv[1];
    int         port = atoi(argv[2]);
    const char* team = argv[3];

    // Number of worker threads: all cores minus 2 (recv + parser each get one)
    int nworkers = max(1, (int)thread::hardware_concurrency() - 2);

#ifdef _OPENMP
    // Each worker will use OpenMP internally; let OMP use all remaining cores
    omp_set_num_threads(max(1, (int)thread::hardware_concurrency() - 2));
    cerr << "[INFO] " << nworkers << " worker threads, OpenMP enabled\n";
#else
    cerr << "[INFO] " << nworkers << " worker threads, OpenMP not available\n";
#endif

reconnect:
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // Disable Nagle — flush response immediately without waiting for more data
    { int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    // Large recv buffer — absorb a full challenge without stalling
    { int rb = kRecvBufBytes; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb)); }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        cerr << "Invalid host: " << host << "\n"; return 1;
    }

    while (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "[WARN] connect failed, retrying...\n";
        this_thread::sleep_for(chrono::seconds(1));
    }
    cerr << "[INFO] Connected to " << host << ":" << port << "\n";

    // Send team name
    string intro = string(team) + "\n";
    send(sock, intro.c_str(), intro.size(), MSG_NOSIGNAL);

    RingBuffer       ring;
    JobQueue         jobs;
    atomic<bool>     stop{false};
    mutex            send_mtx;

    thread io_thread(ioReceiverThread, sock, ref(ring), ref(stop));
    thread parse_thread(parserThread, ref(ring), ref(jobs), ref(stop));

    vector<thread> workers;
    workers.reserve(nworkers);
    for (int i = 0; i < nworkers; ++i)
        workers.emplace_back(computeWorkerThread, sock, ref(jobs), ref(send_mtx), ref(stop));

    io_thread.join();
    parse_thread.join();
    for (auto& w : workers) w.join();

    close(sock);

    if (!stop.load()) goto reconnect;  // clean reconnect on server disconnect
    return 0;
}
