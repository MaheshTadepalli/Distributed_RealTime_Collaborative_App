#pragma once

namespace collab {
namespace redis_lua {

// Token-based lock acquire. KEYS[1]=lock, ARGV[1]=token, ARGV[2]=ttl_ms. Returns 1 on success.
static const char kLuaAcquireLock[] =
    "if redis.call('SET', KEYS[1], ARGV[1], 'NX', 'PX', ARGV[2]) then return 1 end "
    "return 0";

// Token-based lock release. KEYS[1]=lock, ARGV[1]=token. Returns 1 if deleted.
static const char kLuaReleaseLock[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) end "
    "return 0";

// Renew lease only if token matches. KEYS[1]=lock, ARGV[1]=token, ARGV[2]=ttl_ms.
static const char kLuaRenewLock[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('PEXPIRE', KEYS[1], ARGV[2]) end "
    "return 0";

// Atomic commit: idempotency + SET + RPUSH + HSET opId + PUBLISH.
static const char kLuaCommitOp[] =
    "local existing = redis.call('HGET', KEYS[4], ARGV[4]) "
    "if existing then return existing end "
    "redis.call('SET', KEYS[1], ARGV[1]) "
    "redis.call('SET', KEYS[2], ARGV[2]) "
    "redis.call('RPUSH', KEYS[3], ARGV[3]) "
    "redis.call('HSET', KEYS[4], ARGV[4], ARGV[3]) "
    "redis.call('PUBLISH', KEYS[5], ARGV[3]) "
    "return ARGV[3]";

// Atomic state init when revision key is missing.
static const char kLuaInitState[] =
    "if redis.call('EXISTS', KEYS[2]) == 1 then return 0 end "
    "redis.call('SET', KEYS[1], ARGV[1]) "
    "redis.call('SET', KEYS[2], ARGV[2]) "
    "local i = 3 "
    "while i <= #ARGV do "
    "  redis.call('RPUSH', KEYS[3], ARGV[i+1]) "
    "  redis.call('HSET', KEYS[4], ARGV[i], ARGV[i+1]) "
    "  i = i + 2 "
    "end "
    "return 1";

// Force-reload state from PostgreSQL (Redis was flushed).
static const char kLuaReloadState[] =
    "redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4]) "
    "redis.call('SET', KEYS[1], ARGV[1]) "
    "redis.call('SET', KEYS[2], ARGV[2]) "
    "local i = 3 "
    "while i <= #ARGV do "
    "  redis.call('RPUSH', KEYS[3], ARGV[i+1]) "
    "  redis.call('HSET', KEYS[4], ARGV[i], ARGV[i+1]) "
    "  i = i + 2 "
    "end "
    "return 1";

} // namespace redis_lua
} // namespace collab
