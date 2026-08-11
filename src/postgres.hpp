#pragma once

#include "ot.hpp"
#include "sync.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifdef COLLAB_WITH_POSTGRES
#include <libpq-fe.h>
#endif

namespace collab {

struct PersistedDocument {
    std::string document;
    std::size_t revision{0};
    std::vector<Operation> history;
};

class PostgresStore {
public:
    explicit PostgresStore(std::string connection_uri) : connection_uri_(std::move(connection_uri)) {}

    ~PostgresStore() {
#ifdef COLLAB_WITH_POSTGRES
        if (connection_ != nullptr) {
            PQfinish(connection_);
            connection_ = nullptr;
        }
#endif
    }

    bool enabled() const { return !connection_uri_.empty(); }

    bool connect() {
        if (!enabled()) {
            return true;
        }
#ifdef COLLAB_WITH_POSTGRES
        LockGuard lock(mutex_);
        connection_ = PQconnectdb(connection_uri_.c_str());
        if (PQstatus(connection_) != CONNECTION_OK) {
            std::cerr << "PostgreSQL connect failed: " << PQerrorMessage(connection_) << "\n";
            PQfinish(connection_);
            connection_ = nullptr;
            return false;
        }
        return migrate();
#else
        std::cerr << "PostgreSQL requested but server was built without COLLAB_WITH_POSTGRES\n";
        return false;
#endif
    }

    bool healthy() const {
        if (!enabled()) {
            return true;
        }
#ifdef COLLAB_WITH_POSTGRES
        LockGuard lock(mutex_);
        if (connection_ == nullptr) {
            return false;
        }
        return PQstatus(connection_) == CONNECTION_OK;
#else
        return false;
#endif
    }

    bool load(const std::string& document_id, PersistedDocument& loaded) {
        if (!enabled()) {
            return false;
        }
#ifdef COLLAB_WITH_POSTGRES
        LockGuard lock(mutex_);
        if (connection_ == nullptr) {
            return false;
        }

        const char* doc_params[1] = {document_id.c_str()};
        PGresult* doc = PQexecParams(connection_,
                                     "SELECT content, revision FROM documents WHERE id = $1",
                                     1, nullptr, doc_params, nullptr, nullptr, 0);
        if (PQresultStatus(doc) != PGRES_TUPLES_OK || PQntuples(doc) == 0) {
            PQclear(doc);
            return false;
        }

        loaded = PersistedDocument{};
        loaded.document = PQgetvalue(doc, 0, 0);
        loaded.revision = static_cast<std::size_t>(std::stoull(PQgetvalue(doc, 0, 1)));
        PQclear(doc);

        PGresult* ops = PQexecParams(connection_,
                                     "SELECT revision, op_id, client_id, op_type, position, text, op_count "
                                     "FROM operations WHERE document_id = $1 ORDER BY revision ASC",
                                     1, nullptr, doc_params, nullptr, nullptr, 0);
        if (PQresultStatus(ops) == PGRES_TUPLES_OK) {
            const int rows = PQntuples(ops);
            loaded.history.reserve(static_cast<std::size_t>(rows));
            for (int i = 0; i < rows; ++i) {
                Operation op;
                op.revision = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 0)));
                op.op_id = PQgetvalue(ops, i, 1);
                op.client_id = PQgetvalue(ops, i, 2);
                op.type = std::string(PQgetvalue(ops, i, 3)) == "delete" ? OpType::Delete : OpType::Insert;
                op.position = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 4)));
                op.text = PQgetvalue(ops, i, 5);
                op.count = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 6)));
                op.base_revision = op.revision > 0 ? op.revision - 1 : 0;
                loaded.history.push_back(op);
            }
        }
        PQclear(ops);
        return true;
#else
        (void)document_id;
        (void)loaded;
        return false;
#endif
    }

    void persist(const std::string& document_id, const std::string& content, const Operation& op) {
        if (!enabled()) {
            return;
        }
#ifdef COLLAB_WITH_POSTGRES
        LockGuard lock(mutex_);
        if (connection_ == nullptr) {
            return;
        }

        const auto revision = std::to_string(op.revision);
        const auto position = std::to_string(op.position);
        const auto count = std::to_string(op.count);
        const auto type = op.type == OpType::Insert ? "insert" : "delete";

        const char* upsert_params[3] = {document_id.c_str(), content.c_str(), revision.c_str()};
        PGresult* upsert = PQexecParams(
            connection_,
            "INSERT INTO documents(id, content, revision) VALUES($1, $2, $3) "
            "ON CONFLICT(id) DO UPDATE SET content = EXCLUDED.content, revision = EXCLUDED.revision, "
            "updated_at = NOW()",
            3, nullptr, upsert_params, nullptr, nullptr, 0);
        if (PQresultStatus(upsert) != PGRES_COMMAND_OK) {
            std::cerr << "PostgreSQL document upsert failed: " << PQerrorMessage(connection_) << "\n";
        }
        PQclear(upsert);

        const char* op_params[8] = {
            document_id.c_str(), revision.c_str(), op.op_id.c_str(), op.client_id.c_str(),
            type, position.c_str(), op.text.c_str(), count.c_str()};
        PGresult* insert_op = PQexecParams(
            connection_,
            "INSERT INTO operations(document_id, revision, op_id, client_id, op_type, position, text, op_count) "
            "VALUES($1, $2, $3, $4, $5, $6, $7, $8) "
            "ON CONFLICT(document_id, revision) DO NOTHING",
            8, nullptr, op_params, nullptr, nullptr, 0);
        if (PQresultStatus(insert_op) != PGRES_COMMAND_OK) {
            std::cerr << "PostgreSQL operation insert failed: " << PQerrorMessage(connection_) << "\n";
        }
        PQclear(insert_op);
#else
        (void)document_id;
        (void)content;
        (void)op;
#endif
    }

    std::vector<Operation> operations_since(const std::string& document_id, std::size_t revision) {
        if (!enabled()) {
            return {};
        }
#ifdef COLLAB_WITH_POSTGRES
        LockGuard lock(mutex_);
        if (connection_ == nullptr) {
            return {};
        }
        const auto rev = std::to_string(revision);
        const char* params[2] = {document_id.c_str(), rev.c_str()};
        PGresult* ops = PQexecParams(
            connection_,
            "SELECT revision, op_id, client_id, op_type, position, text, op_count "
            "FROM operations WHERE document_id = $1 AND revision > $2 ORDER BY revision ASC",
            2, nullptr, params, nullptr, nullptr, 0);
        std::vector<Operation> result;
        if (PQresultStatus(ops) == PGRES_TUPLES_OK) {
            const int rows = PQntuples(ops);
            result.reserve(static_cast<std::size_t>(rows));
            for (int i = 0; i < rows; ++i) {
                Operation op;
                op.revision = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 0)));
                op.op_id = PQgetvalue(ops, i, 1);
                op.client_id = PQgetvalue(ops, i, 2);
                op.type = std::string(PQgetvalue(ops, i, 3)) == "delete" ? OpType::Delete : OpType::Insert;
                op.position = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 4)));
                op.text = PQgetvalue(ops, i, 5);
                op.count = static_cast<std::size_t>(std::stoull(PQgetvalue(ops, i, 6)));
                result.push_back(op);
            }
        }
        PQclear(ops);
        return result;
#else
        (void)document_id;
        (void)revision;
        return {};
#endif
    }

private:
#ifdef COLLAB_WITH_POSTGRES
    bool exec_sql(const char* sql) {
        PGresult* result = PQexec(connection_, sql);
        const auto status = PQresultStatus(result);
        const bool ok = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
        if (!ok) {
            std::cerr << "PostgreSQL migrate failed: " << PQerrorMessage(connection_) << "\n";
        }
        PQclear(result);
        return ok;
    }

    bool migrate() {
        return exec_sql(
                   "CREATE TABLE IF NOT EXISTS documents ("
                   "  id TEXT PRIMARY KEY,"
                   "  content TEXT NOT NULL,"
                   "  revision BIGINT NOT NULL DEFAULT 0,"
                   "  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
                   ")") &&
               exec_sql(
                   "CREATE TABLE IF NOT EXISTS operations ("
                   "  id BIGSERIAL PRIMARY KEY,"
                   "  document_id TEXT NOT NULL REFERENCES documents(id),"
                   "  revision BIGINT NOT NULL,"
                   "  op_id TEXT NOT NULL,"
                   "  client_id TEXT NOT NULL,"
                   "  op_type TEXT NOT NULL,"
                   "  position BIGINT NOT NULL,"
                   "  text TEXT NOT NULL DEFAULT '',"
                   "  op_count BIGINT NOT NULL DEFAULT 0,"
                   "  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
                   "  UNIQUE(document_id, revision)"
                   ")") &&
               exec_sql("CREATE INDEX IF NOT EXISTS operations_doc_rev_idx "
                        "ON operations(document_id, revision)");
    }

    PGconn* connection_{nullptr};
#endif
    mutable Mutex mutex_;
    std::string connection_uri_;
};

} // namespace collab
