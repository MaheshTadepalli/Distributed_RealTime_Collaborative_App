#pragma once

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "sync.hpp"

namespace collab {

class Metrics {
public:
    void inc_ops() { ops_total_.fetch_add(1); }
    void inc_clients() { clients_connected_.fetch_add(1); }
    void dec_clients() { clients_connected_.fetch_sub(1); }
    void observe_latency_ms(double ms) {
        LockGuard lock(mutex_);
        latency_samples_ms_.push_back(ms);
        if (latency_samples_ms_.size() > 2048) {
            latency_samples_ms_.erase(latency_samples_ms_.begin(),
                                      latency_samples_ms_.begin() + 1024);
        }
    }

    std::string render_prometheus(std::size_t revision) const {
        std::ostringstream out;
        out << "# HELP collab_ops_total Total committed operations\n"
            << "# TYPE collab_ops_total counter\n"
            << "collab_ops_total " << ops_total_.load() << "\n"
            << "# HELP collab_clients_connected Currently connected websocket clients\n"
            << "# TYPE collab_clients_connected gauge\n"
            << "collab_clients_connected " << clients_connected_.load() << "\n"
            << "# HELP collab_document_revision Current document revision\n"
            << "# TYPE collab_document_revision gauge\n"
            << "collab_document_revision " << revision << "\n";

        double p50 = 0;
        double p95 = 0;
        double p99 = 0;
        {
            LockGuard lock(mutex_);
            if (!latency_samples_ms_.empty()) {
                auto sorted = latency_samples_ms_;
                // Insertion sort to avoid depending on <algorithm> quirks in old toolchains.
                for (std::size_t i = 1; i < sorted.size(); ++i) {
                    const double key = sorted[i];
                    std::size_t j = i;
                    while (j > 0 && sorted[j - 1] > key) {
                        sorted[j] = sorted[j - 1];
                        --j;
                    }
                    sorted[j] = key;
                }
                p50 = percentile(sorted, 0.50);
                p95 = percentile(sorted, 0.95);
                p99 = percentile(sorted, 0.99);
            }
        }
        out << "# HELP collab_op_latency_ms Operation commit latency\n"
            << "# TYPE collab_op_latency_ms summary\n"
            << "collab_op_latency_ms{quantile=\"0.5\"} " << p50 << "\n"
            << "collab_op_latency_ms{quantile=\"0.95\"} " << p95 << "\n"
            << "collab_op_latency_ms{quantile=\"0.99\"} " << p99 << "\n";
        return out.str();
    }

private:
    static double percentile(const std::vector<double>& sorted, double q) {
        if (sorted.empty()) return 0;
        const auto index = static_cast<std::size_t>(q * (sorted.size() - 1));
        return sorted[index];
    }

    std::atomic<long long> ops_total_{0};
    std::atomic<long long> clients_connected_{0};
    mutable Mutex mutex_;
    std::vector<double> latency_samples_ms_;
};

inline double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

} // namespace collab
