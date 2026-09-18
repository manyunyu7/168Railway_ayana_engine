#include "engine/api/host/host_queue.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace eng::host {

struct RenderQueue::Impl {
  struct Cmd { std::function<void()> fn; bool blocking = false; bool done = false; };
  mutable std::mutex m;
  std::condition_variable cvWork, cvDone;
  std::deque<Cmd*> cmds;
  std::deque<AssetRequest> outbox;
  bool down = false;
};

RenderQueue::RenderQueue() : impl_(new Impl()) {}
RenderQueue::~RenderQueue() { shutdown(); delete impl_; }

void RenderQueue::submit(std::function<void()> fn, bool blocking) {
  Impl* q = impl_;
  if (!blocking) {
    auto* c = new Impl::Cmd{std::move(fn), false, false};
    std::unique_lock lk(q->m);
    if (q->down) { lk.unlock(); delete c; return; }
    q->cmds.push_back(c);
    q->cvWork.notify_one();
    return;
  }
  Impl::Cmd c{std::move(fn), true, false};
  std::unique_lock lk(q->m);
  if (q->down) return;                       // no render thread: the caller gets its fallback
  q->cmds.push_back(&c);
  q->cvWork.notify_one();
  q->cvDone.wait(lk, [&] { return c.done || q->down; });
}

void RenderQueue::post(std::function<void()> fn) { submit(std::move(fn), false); }
void RenderQueue::run(std::function<void()> fn) { submit(std::move(fn), true); }

int RenderQueue::callInt(std::function<int()> fn, int fallback) {
  int r = fallback;
  run([&] { r = fn(); });
  return r;
}
float RenderQueue::callFloat(std::function<float()> fn, float fallback) {
  float r = fallback;
  run([&] { r = fn(); });
  return r;
}
std::string RenderQueue::callString(std::function<std::string()> fn) {
  std::string r;
  run([&] { r = fn(); });
  return r;
}

size_t RenderQueue::pump() {
  Impl* q = impl_;
  size_t ran = 0;
  for (;;) {
    Impl::Cmd* c = nullptr;
    {
      std::lock_guard lk(q->m);
      if (q->down || q->cmds.empty()) break;
      c = q->cmds.front(); q->cmds.pop_front();
    }
    if (c->fn) c->fn();
    ++ran;
    if (c->blocking) {
      std::lock_guard lk(q->m);
      c->done = true;
      q->cvDone.notify_all();          // the waiter owns `c` (it is on its stack)
    } else {
      delete c;
    }
  }
  return ran;
}

size_t RenderQueue::waitPump(int timeoutMs) {
  Impl* q = impl_;
  {
    std::unique_lock lk(q->m);
    if (q->down) return 0;
    if (q->cmds.empty())
      q->cvWork.wait_for(lk, std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs), [&] { return !q->cmds.empty() || q->down; });
  }
  return pump();
}

void RenderQueue::pushRequest(std::string kind, std::string path) {
  Impl* q = impl_;
  std::lock_guard lk(q->m);
  q->outbox.push_back(AssetRequest{std::move(kind), std::move(path)});
}

bool RenderQueue::popRequest(AssetRequest& out) {
  Impl* q = impl_;
  std::lock_guard lk(q->m);
  if (q->outbox.empty()) return false;
  out = std::move(q->outbox.front());
  q->outbox.pop_front();
  return true;
}

size_t RenderQueue::pendingRequests() const {
  const Impl* q = impl_;
  std::lock_guard lk(q->m);
  return q->outbox.size();
}

void RenderQueue::shutdown() {
  Impl* q = impl_;
  std::deque<Impl::Cmd*> orphans;
  {
    std::lock_guard lk(q->m);
    q->down = true;
    orphans.swap(q->cmds);
    q->cvWork.notify_all();
    q->cvDone.notify_all();
  }
  for (Impl::Cmd* c : orphans) if (!c->blocking) delete c;   // blocking ones live on their caller's stack
}

bool RenderQueue::isShutdown() const {
  const Impl* q = impl_;
  std::lock_guard lk(q->m);
  return q->down;
}

RenderQueue& queue() { static RenderQueue q; return q; }

// ---- frame pacing ----
static double nowSeconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

float FramePacer::tick() {
  double t = nowSeconds();
  frameStart_ = t;
  double dt = last_ > 0 ? t - last_ : 0;
  last_ = t;
  if (dt < 0) dt = 0;
  if (dt > 0.25) dt = 0.25;   // a stall (app resumed) must not teleport the world
  return (float)dt;
}

int FramePacer::sleepMs() const {
  if (targetFps_ <= 0) return 0;
  double budget = 1.0 / targetFps_;
  double spent = nowSeconds() - frameStart_;
  double left = budget - spent;
  return left <= 0 ? 0 : (int)(left * 1000.0);
}

} // namespace eng::host
