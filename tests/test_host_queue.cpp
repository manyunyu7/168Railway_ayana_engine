// The render-thread queue (engine/api/host/host_queue.h): commands from other threads run on the queue
// thread, in order; blocking calls hand a value back; the asset outbox drains; shutdown never deadlocks.
#include "engine/api/host/host_queue.h"
#include "tests/check.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace eng::host;

int main() {
  RenderQueue q;
  std::thread::id qThread;
  std::vector<int> order;
  std::atomic<bool> stop{false}, started{false};

  // The "render thread": the only one that pumps, and the only one that touches `order`.
  std::thread render([&] {
    qThread = std::this_thread::get_id();
    started.store(true);
    while (!stop.load()) q.waitPump(5);
    q.pump();
  });
  while (!started.load()) std::this_thread::yield();

  // fire and forget, from another thread, in order
  for (int i = 0; i < 100; ++i) q.post([&, i] { order.push_back(i); });
  // a blocking call flushes everything queued before it
  int n = q.callInt([&] { return (int)order.size(); });
  CHECK(n == 100);
  for (int i = 0; i < 100; ++i) CHECK(order[(size_t)i] == i);

  // blocking calls run ON the queue thread and return the value
  CHECK(q.callInt([&] { return std::this_thread::get_id() == qThread ? 7 : -1; }) == 7);
  CHECK(q.callFloat([] { return 1.5f; }) == 1.5f);
  CHECK(q.callString([] { return std::string("halo"); }) == "halo");
  bool ran = false;
  q.run([&] { ran = true; });
  CHECK(ran);

  // several host threads at once: every command runs exactly once, on the queue thread
  std::atomic<int> total{0};
  std::vector<std::thread> callers;
  for (int t = 0; t < 4; ++t)
    callers.emplace_back([&] { for (int i = 0; i < 50; ++i) total += q.callInt([&] { return std::this_thread::get_id() == qThread ? 1 : 0; }); });
  for (std::thread& t : callers) t.join();
  CHECK(total.load() == 200);

  // the asset outbox: written by the engine thread, drained by the host
  CHECK(q.pendingRequests() == 0);
  q.run([&] { q.pushRequest("model", "cc203"); q.pushRequest("terrain", "mjk/index.json"); });
  CHECK(q.pendingRequests() == 2);
  AssetRequest r;
  CHECK(q.popRequest(r) && r.kind == "model" && r.path == "cc203");
  CHECK(q.popRequest(r) && r.kind == "terrain" && r.path == "mjk/index.json");
  CHECK(!q.popRequest(r));

  stop.store(true);
  q.post([] {});
  render.join();

  // after shutdown nothing blocks: calls return their fallback immediately
  q.shutdown();
  CHECK(q.isShutdown());
  CHECK(q.callInt([] { return 5; }, -1) == -1);
  q.post([] { CHECK(false); });   // dropped, never run
  auto t0 = std::chrono::steady_clock::now();
  q.run([] { CHECK(false); });
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

  // the frame pacer: a stall is clamped, a target fps leaves a budget
  FramePacer pacer(60);
  pacer.tick();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  float dt = pacer.tick();
  CHECK(dt > 0.001f && dt < 0.25f);
  CHECK(pacer.sleepMs() >= 0 && pacer.sleepMs() <= 17);
  FramePacer free(0);
  free.tick();
  CHECK(free.sleepMs() == 0);

  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}
