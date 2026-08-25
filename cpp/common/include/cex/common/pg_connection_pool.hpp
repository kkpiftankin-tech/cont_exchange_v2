#pragma once
// ============================================================================
// pg_connection_pool.hpp — потокобезопасный пул PostgreSQL-соединений (libpqxx).
//
// ПРОБЛЕМА (P2): ledger и risk открывали новый `pqxx::connection` на каждый
// gRPC/Kafka вызов. При нагрузке (500 conc) это упирается в postgres
// max_connections и даёт connection-storm (≈50% ошибок, p95 5000ms).
//
// РЕШЕНИЕ: фиксированный по верхней границе пул переиспользуемых соединений.
//   * lazy-создание соединений до `max_size` (не открываем всё сразу);
//   * blocking Acquire с таймаутом (std::mutex + std::condition_variable);
//   * RAII-guard `PooledConnection` — соединение возвращается в пул в
//     деструкторе (release), даже при исключении;
//   * health-check: «протухшие» (disconnected) соединения переоткрываются
//     в Acquire прозрачно для вызывающего;
//   * НЕ сериализует всё через один коннект (это анти-паттерн RBAC-repo) —
//     до `max_size` соединений работают параллельно.
//
// Header-only: pqxx подтягивается только в TU, которые include'ят этот файл
// (ledger/risk infra). cex_common как библиотека НЕ линкует libpqxx — поэтому
// сервисы без PG (gateway/order_flow и т.п.) не получают лишней зависимости.
//
// Тестируемость: ядро пула — шаблон BasicPgConnectionPool<Conn, Traits>, что
// позволяет подставить fake-соединение в unit-тесте без живой БД. Production
// alias PgConnectionPool = BasicPgConnectionPool<pqxx::connection>.
//
// CLAUDE.md §12.3: размер пула / DSN читаются через cex::common::Env, не
// хардкодятся. §12.2: lifecycle потоков/ресурсов явный.
// ============================================================================

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pqxx/pqxx>

namespace cex::common {

// Traits по умолчанию для pqxx::connection: создание из DSN + health-check.
// Тест может подставить свой Conn/Traits-тип через шаблон.
template <typename Conn>
struct PgConnTraits {
  static std::unique_ptr<Conn> Open(const std::string& dsn) {
    return std::make_unique<Conn>(dsn);
  }
  static bool IsAlive(Conn& c) {
    try {
      return c.is_open();
    } catch (...) {
      return false;
    }
  }
};

template <typename Conn, typename Traits>
class BasicPgConnectionPool;

// ----------------------------------------------------------------------------
// BasicPooledConnection — RAII-обёртка над заимствованным соединением.
// При уничтожении возвращает соединение обратно в пул. Move-only.
// ----------------------------------------------------------------------------
template <typename Conn, typename Traits>
class BasicPooledConnection {
 public:
  using Pool = BasicPgConnectionPool<Conn, Traits>;

  BasicPooledConnection() = default;
  BasicPooledConnection(Pool* pool, std::unique_ptr<Conn> conn)
      : pool_(pool), conn_(std::move(conn)) {}

  ~BasicPooledConnection() { reset_to_pool(); }

  BasicPooledConnection(const BasicPooledConnection&) = delete;
  BasicPooledConnection& operator=(const BasicPooledConnection&) = delete;

  BasicPooledConnection(BasicPooledConnection&& other) noexcept
      : pool_(other.pool_), conn_(std::move(other.conn_)) {
    other.pool_ = nullptr;
  }
  BasicPooledConnection& operator=(BasicPooledConnection&& other) noexcept {
    if (this != &other) {
      reset_to_pool();
      pool_ = other.pool_;
      conn_ = std::move(other.conn_);
      other.pool_ = nullptr;
    }
    return *this;
  }

  Conn& operator*() const { return *conn_; }
  Conn* operator->() const { return conn_.get(); }
  Conn* get() const { return conn_.get(); }
  explicit operator bool() const { return static_cast<bool>(conn_); }

 private:
  void reset_to_pool() {
    if (pool_ && conn_) {
      pool_->ReturnToPool(std::move(conn_));
    }
    pool_ = nullptr;
    conn_.reset();
  }

  Pool* pool_{nullptr};
  std::unique_ptr<Conn> conn_;
};

// ----------------------------------------------------------------------------
// BasicPgConnectionPool — пул соединений к одному DSN.
//
// Дизайн блокировки:
//   * один std::mutex защищает список свободных соединений (idle_) и счётчик
//     открытых (open_count_);
//   * Acquire: если есть idle — берём; иначе если open_count_ < max — создаём
//     новое; иначе ждём на condition_variable до release или таймаута;
//   * Release: кладём соединение обратно в idle_ и notify_one.
//
// Disconnect-handling: перед выдачей idle-соединения проверяем IsAlive();
// «мёртвые» отбрасываем и создаём свежие. Не отдаём заведомо мёртвый коннект.
// ----------------------------------------------------------------------------
template <typename Conn, typename Traits = PgConnTraits<Conn>>
class BasicPgConnectionPool {
 public:
  using Guard = BasicPooledConnection<Conn, Traits>;

  explicit BasicPgConnectionPool(
      std::string dsn,
      std::size_t max_size = 16,
      std::chrono::milliseconds acquire_timeout = std::chrono::milliseconds(2000))
      : dsn_(std::move(dsn)),
        max_size_(max_size == 0 ? 1 : max_size),
        acquire_timeout_(acquire_timeout) {}

  BasicPgConnectionPool(const BasicPgConnectionPool&) = delete;
  BasicPgConnectionPool& operator=(const BasicPgConnectionPool&) = delete;

  // Заимствует соединение из пула. Блокирует до появления свободного, создания
  // нового (если не достигнут max_size) или истечения таймаута. Бросает
  // std::runtime_error при таймауте / невозможности открыть коннект.
  Guard Acquire() {
    std::unique_lock<std::mutex> lock(mu_);

    for (;;) {
      // 1. Есть свободное соединение — берём, проверяем живость.
      while (!idle_.empty()) {
        std::unique_ptr<Conn> conn = std::move(idle_.back());
        idle_.pop_back();
        if (conn && Traits::IsAlive(*conn)) {
          return Guard(this, std::move(conn));
        }
        --open_count_;  // протухло — освобождаем слот, пробуем ниже.
      }

      // 2. Есть бюджет на новое соединение — открываем (вне lock'а: open() — IO).
      if (open_count_ < max_size_) {
        ++open_count_;
        lock.unlock();
        std::unique_ptr<Conn> conn;
        try {
          conn = Traits::Open(dsn_);
        } catch (...) {
          lock.lock();
          --open_count_;
          cv_.notify_one();
          throw;
        }
        lock.lock();
        return Guard(this, std::move(conn));
      }

      // 3. Пул исчерпан — ждём release с таймаутом.
      if (cv_.wait_for(lock, acquire_timeout_, [this] {
            return !idle_.empty() || open_count_ < max_size_;
          })) {
        continue;
      }
      throw std::runtime_error(
          "PgConnectionPool::Acquire timed out waiting for a free connection");
    }
  }

  std::size_t max_size() const { return max_size_; }

  std::size_t idle_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
  }

  std::size_t open_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return open_count_;
  }

 private:
  friend class BasicPooledConnection<Conn, Traits>;

  // Вызывается из guard'а при release.
  void ReturnToPool(std::unique_ptr<Conn> conn) {
    const bool alive = conn && Traits::IsAlive(*conn);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (alive) {
        idle_.push_back(std::move(conn));
      } else {
        --open_count_;  // мёртвое не возвращаем; conn уничтожится здесь.
      }
    }
    cv_.notify_one();
  }

  const std::string dsn_;
  const std::size_t max_size_;
  const std::chrono::milliseconds acquire_timeout_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Conn>> idle_;
  std::size_t open_count_{0};
};

// Production-типы поверх pqxx::connection.
using PgConnectionPool = BasicPgConnectionPool<pqxx::connection>;
using PooledConnection = BasicPooledConnection<pqxx::connection, PgConnTraits<pqxx::connection>>;

}  // namespace cex::common
