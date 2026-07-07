// ============================================================================
// pg_connection_pool_test.cpp — unit-тест cex::common::BasicPgConnectionPool.
//
// Без живой БД: подставляем FakeConn + FakeTraits через шаблон. Проверяем:
//   * acquire создаёт соединение лениво;
//   * release возвращает соединение в пул (idle растёт);
//   * повторный acquire переиспользует то же соединение (reuse);
//   * пул не открывает больше max_size одновременно;
//   * Acquire с таймаутом бросает, когда пул исчерпан;
//   * «протухшее» (is_open()==false) соединение не выдаётся — открывается новое.
// Hand-rolled harness в стиле decimal_test.cpp.
// ============================================================================

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "cex/common/pg_connection_pool.hpp"

namespace {

// Fake "connection": tracks a global open count and per-instance liveness.
struct FakeConn {
  static std::atomic<int> live_instances;  // currently constructed
  static std::atomic<int> total_opened;    // ever constructed
  int id;
  bool alive{true};

  explicit FakeConn(const std::string& /*dsn*/) {
    id = ++total_opened;
    ++live_instances;
  }
  ~FakeConn() { --live_instances; }
  bool is_open() const { return alive; }
};

std::atomic<int> FakeConn::live_instances{0};
std::atomic<int> FakeConn::total_opened{0};

using Pool = cex::common::BasicPgConnectionPool<FakeConn>;

int g_failures = 0;
void check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAILED: " << msg << '\n';
    ++g_failures;
  }
}

void test_acquire_release_reuse() {
  FakeConn::total_opened = 0;
  Pool pool("fake-dsn", /*max_size=*/4);

  check(pool.open_count() == 0, "lazy: no connections before first acquire");

  int first_id = -1;
  {
    auto c = pool.Acquire();
    check(static_cast<bool>(c), "acquire yields a connection");
    check(pool.open_count() == 1, "one open after first acquire");
    check(pool.idle_count() == 0, "no idle while held");
    first_id = c->id;
  }  // release here

  check(pool.idle_count() == 1, "release returns connection to pool");
  check(pool.open_count() == 1, "open count stays 1 after release");

  {
    auto c = pool.Acquire();
    check(c->id == first_id, "reuse: same connection handed back out");
  }
  check(FakeConn::total_opened == 1, "reuse: no new connection opened");
}

void test_max_size_and_timeout() {
  Pool pool("fake-dsn", /*max_size=*/2,
            /*acquire_timeout=*/std::chrono::milliseconds(50));

  auto a = pool.Acquire();
  auto b = pool.Acquire();
  check(pool.open_count() == 2, "two connections open at max_size");

  bool threw = false;
  try {
    auto c = pool.Acquire();  // pool exhausted, must time out
    (void)c;
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "Acquire times out / throws when pool exhausted");
}

void test_stale_connection_reopened() {
  FakeConn::total_opened = 0;
  Pool pool("fake-dsn", /*max_size=*/2);

  int dead_id = -1;
  {
    auto c = pool.Acquire();
    dead_id = c->id;
    c->alive = false;  // simulate disconnect while held
  }  // release: dead conn must NOT be retained as idle

  check(pool.idle_count() == 0, "dead connection not returned to idle");
  check(pool.open_count() == 0, "dead connection releases its slot");

  {
    auto c = pool.Acquire();  // must open a fresh one
    check(c->id != dead_id, "stale connection replaced by a fresh one");
    check(c->is_open(), "fresh connection is open");
  }
  check(FakeConn::total_opened == 2, "exactly one reopen after disconnect");
}

}  // namespace

int main() {
  test_acquire_release_reuse();
  test_max_size_and_timeout();
  test_stale_connection_reopened();

  if (g_failures == 0) {
    std::cout << "pg_connection_pool_test: all checks passed\n";
    return 0;
  }
  std::cerr << "pg_connection_pool_test: " << g_failures << " failure(s)\n";
  return 1;
}
