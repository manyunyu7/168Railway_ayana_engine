// The one door to the engine on a host with threads (Android; the Wasm host is single-threaded and
// does not need it). `eng_*` assumes a single caller, so exactly one thread — the render thread —
// ever touches it; every other thread posts work here.
//
//   post(fn)        fire and forget (writes: eng_set_state, eng_pointer, ...)
//   run/callInt/callFloat/callString(fn)   block until the render thread ran it and hand the result back
//                   (reads: eng_pick, eng_stats, ...). The render thread services the queue between
//                   frames, so a read answers within one frame.
//   pump/waitPump   called by the render thread only: execute what is queued.
//   push/popRequest the outgoing asset queue: the engine's request(kind, path) hook lands here and the
//                   host (Dart/Kotlin) drains it and answers with eng_terrain_tile_rgba / eng_model_begin.
//   shutdown()      called by the render thread when its loop ended: unblocks everybody; queued and later
//                   calls return the fallback at once, so a host thread never deadlocks on a dead render thread.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace eng::host {

struct AssetRequest { std::string kind, path; };

class RenderQueue {
public:
  RenderQueue();
  ~RenderQueue();
  RenderQueue(const RenderQueue&) = delete;
  RenderQueue& operator=(const RenderQueue&) = delete;

  void post(std::function<void()> fn);
  void run(std::function<void()> fn);                                  // blocking, no result
  int callInt(std::function<int()> fn, int fallback = 0);
  float callFloat(std::function<float()> fn, float fallback = 0);
  std::string callString(std::function<std::string()> fn);

  // Render thread. pump() runs everything queued now and returns how many commands ran;
  // waitPump() blocks until there is work or `timeoutMs` elapsed, then pumps. Both return 0 after shutdown.
  size_t pump();
  size_t waitPump(int timeoutMs);

  void pushRequest(std::string kind, std::string path);   // engine (render) thread
  bool popRequest(AssetRequest& out);                     // host thread
  size_t pendingRequests() const;

  void shutdown();                 // wakes the render thread and releases every blocked caller
  // Arms the queue again for a NEW render thread (Android: the Texture is disposed when the page is left
  // and created again on the next visit). Anything still queued from the old life is dropped, the outbox
  // too (its requests belong to a world that no longer exists). Without this, every call after the first
  // shutdown returned its fallback for good: eng_load_world "failed" with an empty error on the second visit.
  void restart();
  bool isShutdown() const;

private:
  void submit(std::function<void()> fn, bool blocking);
  struct Impl;
  Impl* impl_ = nullptr;   // out of line so <mutex>/<condition_variable> stay in the .cpp
};

// Process-wide queue: engine_api.cpp's request() hook writes into this one, and the Android render
// thread pumps it. A single engine instance per process, exactly like the ABI itself.
RenderQueue& queue();

// Frame pacing: dt in seconds between calls, clamped so a stall does not teleport the world, and the
// number of milliseconds to sleep to hold `targetFps` (0 = free running / vsync does the pacing).
class FramePacer {
public:
  explicit FramePacer(float targetFps = 0) : targetFps_(targetFps) {}
  void setTargetFps(float fps) { targetFps_ = fps; }
  float tick();          // seconds since the previous tick, clamped to [0, 0.25]
  int sleepMs() const;   // what to wait before the next frame to hold the target (0 when free running)
private:
  float targetFps_ = 0;
  double last_ = 0, frameStart_ = 0;
};

} // namespace eng::host
