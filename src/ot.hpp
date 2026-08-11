#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace collab {

enum class OpType { Insert, Delete };

struct Operation {
    OpType type{OpType::Insert};
    std::size_t position{0};
    std::string text;
    std::size_t count{0};
    std::size_t base_revision{0};
    std::size_t revision{0};
    std::string client_id;
    std::string op_id;
};

inline bool is_noop(const Operation& op) {
    return (op.type == OpType::Insert && op.text.empty()) ||
           (op.type == OpType::Delete && op.count == 0);
}

inline void clamp(Operation& op, std::size_t document_size) {
    op.position = std::min(op.position, document_size);
    if (op.type == OpType::Delete) {
        op.count = std::min(op.count, document_size - op.position);
    }
}

inline void apply(std::string& document, Operation op) {
    clamp(op, document.size());
    if (op.type == OpType::Insert) {
        document.insert(op.position, op.text);
    } else if (op.count > 0) {
        document.erase(op.position, op.count);
    }
}

inline Operation transform(Operation incoming, const Operation& committed) {
    if (is_noop(incoming) || is_noop(committed)) {
        return incoming;
    }

    if (committed.type == OpType::Insert) {
        const auto inserted = committed.text.size();
        if (incoming.type == OpType::Insert) {
            const bool same_spot = incoming.position == committed.position;
            const bool committed_wins_tie =
                committed.op_id < incoming.op_id ||
                (committed.op_id == incoming.op_id && committed.client_id < incoming.client_id);
            if (incoming.position > committed.position || (same_spot && committed_wins_tie)) {
                incoming.position += inserted;
            }
        } else {
            if (incoming.position >= committed.position) {
                incoming.position += inserted;
            } else if (incoming.position + incoming.count > committed.position) {
                incoming.count += inserted;
            }
        }
        return incoming;
    }

    const auto deleted_start = committed.position;
    const auto deleted_end = committed.position + committed.count;

    if (incoming.type == OpType::Insert) {
        if (incoming.position > deleted_end) {
            incoming.position -= committed.count;
        } else if (incoming.position >= deleted_start) {
            incoming.position = deleted_start;
        }
        return incoming;
    }

    const auto incoming_start = incoming.position;
    const auto incoming_end = incoming.position + incoming.count;

    if (incoming_end <= deleted_start) {
        return incoming;
    }
    if (incoming_start >= deleted_end) {
        incoming.position -= committed.count;
        return incoming;
    }

    const auto overlap_start = std::max(incoming_start, deleted_start);
    const auto overlap_end = std::min(incoming_end, deleted_end);
    const auto overlap = overlap_end > overlap_start ? overlap_end - overlap_start : 0;

    incoming.count -= overlap;
    if (incoming_start >= deleted_start) {
        incoming.position = deleted_start;
    }
    return incoming;
}

class DocumentSession {
public:
    Operation commit(Operation op) {
        const auto start = std::min(op.base_revision, history_.size());
        for (std::size_t i = start; i < history_.size(); ++i) {
            op = transform(op, history_[i]);
        }
        clamp(op, document_.size());
        apply(document_, op);
        op.revision = history_.size() + 1;
        history_.push_back(op);
        return op;
    }

    const std::string& document() const { return document_; }
    std::size_t revision() const { return history_.size(); }
    const std::vector<Operation>& history() const { return history_; }

private:
    std::string document_ =
        "Welcome to the C++ collaborative editor.\n"
        "Open this page in two browser windows and type at the same time.\n";
    std::vector<Operation> history_;
};

} // namespace collab
