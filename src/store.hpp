#pragma once

#include "json_util.hpp"
#include "ot.hpp"
#include "sync.hpp"

#include <functional>
#include <string>
#include <vector>

namespace collab {

struct Snapshot {
    std::string document;
    std::size_t revision{0};
};

class DocumentStore {
public:
    virtual ~DocumentStore() = default;

    virtual Snapshot snapshot() = 0;
    virtual std::vector<Operation> operations_since(std::size_t revision) = 0;
    virtual Operation commit(Operation op) = 0;
    virtual bool healthy() const = 0;

    virtual void set_broadcast_handler(std::function<void(const Operation&)> handler) {
        broadcast_handler_ = std::move(handler);
    }

protected:
    std::function<void(const Operation&)> broadcast_handler_;
};

class MemoryStore : public DocumentStore {
public:
    Snapshot snapshot() override {
        LockGuard lock(mutex_);
        return {session_.document(), session_.revision()};
    }

    std::vector<Operation> operations_since(std::size_t revision) override {
        LockGuard lock(mutex_);
        const auto& history = session_.history();
        if (revision >= history.size()) {
            return {};
        }
        return {history.begin() + static_cast<std::ptrdiff_t>(revision), history.end()};
    }

    Operation commit(Operation op) override {
        LockGuard lock(mutex_);
        auto committed = session_.commit(op);
        if (broadcast_handler_) {
            broadcast_handler_(committed);
        }
        return committed;
    }

    bool healthy() const override { return true; }

private:
    DocumentSession session_;
    mutable Mutex mutex_;
};

} // namespace collab
