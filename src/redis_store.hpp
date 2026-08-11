#pragma once

#include "postgres.hpp"
#include "redis_client.hpp"
#include "redis_lua.hpp"
#include "store.hpp"
#include "sync.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <thread>
#endif

namespace collab {

namespace {
const char* kDefaultDocument =
    "Welcome to the C++ collaborative editor.\n"
    "Open this page in two browser windows and type at the same time.\n";

constexpr int kLockTtlMs = 30000;
constexpr int kLockRenewMs = 5000;
constexpr int kLockAcquireAttempts = 100;
} // namespace

class RedisStore : public DocumentStore {
public:
    RedisStore(std::string host, uint16_t port, std::string document_id, PostgresStore* postgres)
        : host_(std::move(host)),
          port_(port),
          document_id_(std::move(document_id)),
          postgres_(postgres),
          content_key_("doc:" + document_id_ + ":content"),
          revision_key_("doc:" + document_id_ + ":revision"),
          history_key_("doc:" + document_id_ + ":history"),
          opids_key_("doc:" + document_id_ + ":opids"),
          lock_key_("doc:" + document_id_ + ":lock"),
          channel_("doc:" + document_id_ + ":ops") {}

    ~RedisStore() override { stop(); }

    bool start() {
        if (!client_.connect(host_, port_)) {
            std::cerr << "Redis connect failed: " << host_ << ":" << port_ << "\n";
            return false;
        }
        redis_available_ = true;
        ensure_initialized();
        running_ = true;
        if (postgres_ != nullptr) {
            pg_running_ = true;
#ifdef _WIN32
            pg_thread_ = CreateThread(nullptr, 0, pg_worker_entry, this, 0, nullptr);
#else
            pg_thread_ = std::thread([this] { pg_worker_loop(); });
#endif
        }
#ifdef _WIN32
        subscriber_handle_ = CreateThread(nullptr, 0, subscriber_entry, this, 0, nullptr);
        return subscriber_handle_ != nullptr;
#else
        subscriber_ = std::thread([this] { subscribe_loop(); });
        return true;
#endif
    }

    void stop() {
        running_ = false;
        pg_running_ = false;
        RedisClient killer;
        if (killer.connect(host_, port_)) {
            killer.command({"PUBLISH", channel_, "{\"kind\":\"shutdown\"}"});
        }
#ifdef _WIN32
        if (subscriber_handle_ != nullptr) {
            WaitForSingleObject(subscriber_handle_, 5000);
            CloseHandle(subscriber_handle_);
            subscriber_handle_ = nullptr;
        }
        if (pg_thread_ != nullptr) {
            WaitForSingleObject(pg_thread_, 10000);
            CloseHandle(pg_thread_);
            pg_thread_ = nullptr;
        }
#else
        if (subscriber_.joinable()) {
            subscriber_.join();
        }
        if (pg_thread_.joinable()) {
            pg_thread_.join();
        }
#endif
        // Drain any remaining persistence jobs synchronously on shutdown.
        while (true) {
            PersistJob job;
            {
                LockGuard lock(pg_mutex_);
                if (pg_queue_.empty()) break;
                job = pg_queue_.front();
                pg_queue_.pop_front();
            }
            if (postgres_ != nullptr) {
                postgres_->persist(document_id_, job.document, job.op);
            }
        }
        client_.disconnect();
        redis_available_ = false;
    }

    Snapshot snapshot() override {
        LockGuard lock(mutex_);
        ensure_recovery_locked();
        auto content = client_.command({"GET", content_key_});
        auto revision = client_.command({"GET", revision_key_});
        Snapshot snap;
        snap.document = content.type == RedisValue::Type::Bulk ? content.bulk : kDefaultDocument;
        snap.revision = revision.type == RedisValue::Type::Bulk
                            ? static_cast<std::size_t>(std::stoull(revision.bulk))
                            : 0;
        return snap;
    }

    std::vector<Operation> operations_since(std::size_t revision) override {
        LockGuard lock(mutex_);
        ensure_recovery_locked();
        if (revision == 0) {
            auto all = client_.command({"LRANGE", history_key_, "0", "-1"});
            return parse_history(all);
        }
        const auto start = static_cast<long long>(revision);
        auto slice = client_.command({"LRANGE", history_key_, std::to_string(start), "-1"});
        return parse_history(slice);
    }

    Operation commit(Operation op) override {
        if (op.op_id.empty()) {
            throw std::runtime_error("operation missing opId");
        }

        Operation committed;
        std::string document_for_pg;

        // One Redis command connection is shared: all RESP I/O must hold mutex_.
        {
            LockGuard lock(mutex_);
            ensure_recovery_locked();

            auto existing = client_.command({"HGET", opids_key_, op.op_id});
            if (existing.type == RedisValue::Type::Bulk) {
                return op_from_json(existing.bulk);
            }

            if (!acquire_lock()) {
                throw std::runtime_error("could not acquire redis document lock");
            }

            try {
                existing = client_.command({"HGET", opids_key_, op.op_id});
                if (existing.type == RedisValue::Type::Bulk) {
                    release_lock();
                    return op_from_json(existing.bulk);
                }

                renew_lock();

                auto content_reply = client_.command({"GET", content_key_});
                auto revision_reply = client_.command({"GET", revision_key_});
                std::string document = content_reply.type == RedisValue::Type::Bulk
                                           ? content_reply.bulk
                                           : kDefaultDocument;
                const std::size_t current_revision =
                    revision_reply.type == RedisValue::Type::Bulk
                        ? static_cast<std::size_t>(std::stoull(revision_reply.bulk))
                        : 0;

                const auto start = std::min(op.base_revision, current_revision);
                if (start < current_revision) {
                    auto history = client_.command({"LRANGE", history_key_, std::to_string(start),
                                                    std::to_string(current_revision - 1)});
                    for (const auto& previous : parse_history(history)) {
                        op = transform(op, previous);
                    }
                }

                clamp(op, document.size());
                apply(document, op);
                op.revision = current_revision + 1;
                const auto payload = op_to_json(op);

                renew_lock();

                const std::vector<std::string> keys = {content_key_, revision_key_, history_key_,
                                                       opids_key_, channel_};
                const std::vector<std::string> args = {document, std::to_string(op.revision), payload,
                                                       op.op_id};
                auto result = client_.eval(redis_lua::kLuaCommitOp, keys, args);
                if (result.type != RedisValue::Type::Bulk) {
                    throw std::runtime_error("redis atomic commit failed: " + result.bulk);
                }

                release_lock();
                committed = op_from_json(result.bulk);
                document_for_pg = document;
            } catch (...) {
                release_lock();
                throw;
            }
        }

        // Persist off the ACK path – Redis is the hot-path source of truth.
        enqueue_persist(document_for_pg, committed);
        return committed;
    }

    bool healthy() const override {
        RedisClient probe;
        if (!probe.connect(host_, port_)) {
            redis_available_ = false;
            return false;
        }
        auto reply = probe.command({"PING"});
        const bool ok = reply.type == RedisValue::Type::Status && reply.bulk == "PONG";
        if (!ok) {
            redis_available_ = false;
            return false;
        }
        // Detect recovery: Redis came back after being unavailable.
        if (!redis_available_.load()) {
            redis_available_ = true;
            const_cast<RedisStore*>(this)->on_redis_recovered();
        }
        return true;
    }

private:
    struct PersistJob {
        std::string document;
        Operation op;
    };

#ifdef _WIN32
    static DWORD WINAPI subscriber_entry(LPVOID param) {
        static_cast<RedisStore*>(param)->subscribe_loop();
        return 0;
    }
    static DWORD WINAPI pg_worker_entry(LPVOID param) {
        static_cast<RedisStore*>(param)->pg_worker_loop();
        return 0;
    }
#endif

    void enqueue_persist(const std::string& document, const Operation& op) {
        if (postgres_ == nullptr) {
            return;
        }
        LockGuard lock(pg_mutex_);
        pg_queue_.push_back(PersistJob{document, op});
        // Bound memory if Postgres lags.
        while (pg_queue_.size() > 10000) {
            pg_queue_.pop_front();
        }
    }

    void pg_worker_loop() {
        while (pg_running_ || true) {
            PersistJob job;
            bool have = false;
            {
                LockGuard lock(pg_mutex_);
                if (!pg_queue_.empty()) {
                    job = pg_queue_.front();
                    pg_queue_.pop_front();
                    have = true;
                } else if (!pg_running_) {
                    break;
                }
            }
            if (!have) {
                sleep_ms(5);
                continue;
            }
            if (postgres_ != nullptr) {
                postgres_->persist(document_id_, job.document, job.op);
            }
        }
    }
    void on_redis_recovered() {
        LockGuard lock(mutex_);
        if (!client_.connected() && !client_.connect(host_, port_)) {
            return;
        }
        std::cerr << "Redis recovered – checking state reload from PostgreSQL\n";
        ensure_recovery_locked(true);
    }

    void ensure_initialized() {
        LockGuard lock(mutex_);
        auto exists = client_.command({"EXISTS", revision_key_});
        if (exists.type == RedisValue::Type::Integer && exists.integer == 0) {
            load_state_into_redis(false);
        }
    }

    void ensure_recovery_locked(bool force_reload = false) {
        if (!client_.connected()) {
            if (!client_.connect(host_, port_)) {
                return;
            }
        }
        auto exists = client_.command({"EXISTS", revision_key_});
        const bool redis_empty = exists.type == RedisValue::Type::Integer && exists.integer == 0;
        if (!redis_empty && !force_reload) {
            return;
        }
        if (postgres_ == nullptr) {
            if (redis_empty) {
                load_state_into_redis(false);
            }
            return;
        }
        PersistedDocument loaded;
        if (!postgres_->load(document_id_, loaded)) {
            if (redis_empty) {
                load_state_into_redis(false);
            }
            return;
        }
        std::cerr << "Reloading Redis from PostgreSQL (revision " << loaded.revision << ")\n";
        load_state_into_redis(true, &loaded);
    }

    void load_state_into_redis(bool force_reload, PersistedDocument* from_pg = nullptr) {
        std::string document = kDefaultDocument;
        std::size_t revision = 0;
        std::vector<Operation> history;

        if (from_pg != nullptr) {
            document = from_pg->document;
            revision = from_pg->revision;
            history = from_pg->history;
        } else if (postgres_ != nullptr) {
            PersistedDocument loaded;
            if (postgres_->load(document_id_, loaded)) {
                document = loaded.document;
                revision = loaded.revision;
                history = loaded.history;
            }
        }

        std::vector<std::string> keys = {content_key_, revision_key_, history_key_, opids_key_};
        std::vector<std::string> args = {document, std::to_string(revision)};
        for (const auto& op : history) {
            const auto payload = op_to_json(op);
            args.push_back(op.op_id);
            args.push_back(payload);
        }

        const char* script = force_reload ? redis_lua::kLuaReloadState : redis_lua::kLuaInitState;
        client_.eval(script, keys, args);
    }

    bool acquire_lock() {
        lock_token_ = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < kLockAcquireAttempts; ++attempt) {
            auto reply = client_.eval(
                redis_lua::kLuaAcquireLock, {lock_key_},
                {lock_token_, std::to_string(kLockTtlMs)});
            if (reply.type == RedisValue::Type::Integer && reply.integer == 1) {
                return true;
            }
            sleep_ms(20);
        }
        return false;
    }

    void renew_lock() {
        client_.eval(redis_lua::kLuaRenewLock, {lock_key_},
                     {lock_token_, std::to_string(kLockTtlMs)});
    }

    void release_lock() {
        client_.eval(redis_lua::kLuaReleaseLock, {lock_key_}, {lock_token_});
        lock_token_.clear();
    }

    static std::vector<Operation> parse_history(const RedisValue& value) {
        std::vector<Operation> ops;
        if (value.type != RedisValue::Type::Array) {
            return ops;
        }
        for (const auto& item : value.array) {
            if (item.type == RedisValue::Type::Bulk) {
                ops.push_back(op_from_json(item.bulk));
            }
        }
        return ops;
    }

    void subscribe_loop() {
        RedisClient sub;
        if (!sub.connect(host_, port_)) {
            std::cerr << "Redis subscriber connect failed\n";
            return;
        }
        if (!sub.subscribe(channel_)) {
            std::cerr << "Redis SUBSCRIBE failed\n";
            return;
        }

        while (running_) {
            auto message = sub.next_message();
            if (message.type == RedisValue::Type::Error) {
                if (!running_) break;
                redis_available_ = false;
                sleep_ms(200);
                if (!sub.connect(host_, port_) || !sub.subscribe(channel_)) {
                    continue;
                }
                redis_available_ = true;
                on_redis_recovered();
                continue;
            }
            if (message.type != RedisValue::Type::Array || message.array.size() < 3) {
                continue;
            }
            if (message.array[0].as_string() != "message") {
                continue;
            }
            const auto payload = message.array[2].as_string();
            if (payload.find("\"kind\":\"shutdown\"") != std::string::npos) {
                if (!running_) break;
                continue;
            }
            if (broadcast_handler_) {
                broadcast_handler_(op_from_json(payload));
            }
        }
    }

    std::string host_;
    uint16_t port_;
    std::string document_id_;
    PostgresStore* postgres_;
    std::string content_key_;
    std::string revision_key_;
    std::string history_key_;
    std::string opids_key_;
    std::string lock_key_;
    std::string channel_;
    std::string lock_token_;

    RedisClient client_;
    mutable Mutex mutex_;
    mutable std::atomic<bool> redis_available_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> pg_running_{false};
    Mutex pg_mutex_;
    std::deque<PersistJob> pg_queue_;
#ifdef _WIN32
    HANDLE subscriber_handle_{nullptr};
    HANDLE pg_thread_{nullptr};
#else
    std::thread subscriber_;
    std::thread pg_thread_;
#endif
};

} // namespace collab
