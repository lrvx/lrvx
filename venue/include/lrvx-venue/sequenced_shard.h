/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "lrvx-venue/checkpoint_lane.h"
#include "lrvx-venue/control_plane.h"
#include "lrvx-venue/journal.h"
#include "lrvx-venue/matching_book.h"
#include "lrvx-venue/matching_engine.h"
#include "lrvx-venue/messages.h"
#include "lrvx-venue/shard_events.h"
#include "lrvx/util/file_io.h"

#include "lrvx/util/concurrency/thread_body.h"
#include "lrvx/util/eventing/event_bus.h"
#include "lrvx/util/eventing/wake_set.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace lrvx::venue
{

// ---- ingress: normalized commands from every gateway ----

}  // namespace lrvx::venue

namespace lrvx
{
template <>
struct EventDispatcher<lrvx::venue::InboundCommandEvent>
{
  static void dispatch(const lrvx::venue::InboundCommandEvent& ev, lrvx::venue::ICommandListener& l)
  {
    l.onCommand(ev);
  }
  static void endOfBatch(lrvx::venue::ICommandListener& l) { l.onBatchEnd(); }
};
template <>
struct EventDispatcher<lrvx::venue::EngineEventMsg>
{
  static void dispatch(const lrvx::venue::EngineEventMsg& ev, lrvx::venue::IEngineEventListener& l)
  {
    l.onEngineEvent(ev);
  }
};
}  // namespace lrvx

namespace lrvx::venue
{

// Checkpoint / journal-rotation policy of a SequencedShard.
struct CheckpointConfig
{
  // Auto-trigger thresholds on the CURRENT journal segment, checked by the
  // idle sweeper thread -- automatic checkpoints therefore require
  // idleSweepIntervalNs > 0 (checkpointNow() works regardless). 0 disables
  // the corresponding threshold.
  uint64_t maxSegmentRecords{1'000'000};
  uint64_t maxSegmentBytes{256ULL << 20};
  // Per-shard spread on those thresholds, in percent (0 = off, the default).
  // The thresholds are the same for every shard, so equally loaded shards
  // cross them together and their pauses arrive together -- which matters
  // only once shards share a driver, and costs nothing to prevent: each shard
  // takes its own cut below the configured threshold, deterministically from
  // its symbol, re-rolled after every checkpoint.
  unsigned triggerJitterPct{0};
  // Snapshot+journal generations kept on disk after a checkpoint (>= 1). The
  // pre-checkpoint single-file journal counts as the oldest generation and is
  // deleted once `retainGenerations` snapshot generations exist.
  int retainGenerations{2};
};

template <class Book = MatchingBook, size_t IngressCap = 1 << 16, size_t OutboundCap = 1 << 16>
class SequencedShard
{
 public:
  // ---- on-disk layout ----
  // Before the first checkpoint the shard journals into the single file the
  // caller named (`<base>`, the historical layout). Every checkpoint at
  // boundary ts atomically publishes `<base>.snapshot.<ts>` and rotates the
  // journal onto `<base>.journal.<ts>`; a journal segment's numeric suffix
  // names its base snapshot, so the naming convention IS the manifest (plus
  // per-record CRC) -- no separate SegmentHeader record or manifest file to
  // tear. Recovery replays the newest snapshot that validates end-to-end
  // (structure + stateHash) plus every segment with ts >= that snapshot's;
  // an invalid snapshot falls back a generation, and with no valid snapshot
  // at all recovery replays the legacy file plus all segments from scratch.
  static std::string snapshotPath(const std::string& base, int64_t ts)
  {
    return base + ".snapshot." + std::to_string(ts);
  }
  static std::string segmentPath(const std::string& base, int64_t ts)
  {
    return base + ".journal." + std::to_string(ts);
  }

  struct Generations
  {
    std::vector<int64_t> snapshots;  // ascending checkpoint ts
    std::vector<int64_t> segments;   // ascending checkpoint ts
  };

  static Generations scanGenerations(const std::string& base)
  {
    namespace fs = std::filesystem;
    Generations g;
    const fs::path bp(base);
    fs::path dir = bp.parent_path();
    if (dir.empty())
    {
      dir = ".";
    }
    const std::string snapPrefix = bp.filename().string() + ".snapshot.";
    const std::string segPrefix = bp.filename().string() + ".journal.";
    const auto tsOf = [](const std::string& name, const std::string& prefix) -> int64_t
    {
      if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
      {
        return -1;
      }
      int64_t v = 0;
      for (size_t i = prefix.size(); i < name.size(); ++i)
      {
        if (name[i] < '0' || name[i] > '9')
        {
          return -1;  // excludes ".tmp" staging files and foreign names
        }
        v = v * 10 + (name[i] - '0');
      }
      return v;
    };
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; ec == std::error_code{} && it != end;
         it.increment(ec))
    {
      const std::string name = it->path().filename().string();
      if (const int64_t ts = tsOf(name, snapPrefix); ts >= 0)
      {
        g.snapshots.push_back(ts);
      }
      else if (const int64_t ts = tsOf(name, segPrefix); ts >= 0)
      {
        g.segments.push_back(ts);
      }
    }
    std::sort(g.snapshots.begin(), g.snapshots.end());
    std::sort(g.segments.begin(), g.segments.end());
    return g;
  }

  using IngressBus = lrvx::EventBus<InboundCommandEvent, IngressCap, 4>;
  using OutboundBus = lrvx::EventBus<EngineEventMsg, OutboundCap, 8>;

  // Sequencer clock: nanosecond timestamps stamped on every accepted command,
  // journaled AND fed to the engine as the same value. Injectable so tests are
  // deterministic; defaults to system wall time.
  using TimeSource = std::function<int64_t()>;

  static int64_t systemNowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // journalSync defaults to Full: the shard WAL is fsync-durable before a
  // command is applied (production posture). Pass Journal::Sync::Off for
  // throughput-oriented runs where power-loss durability is not required.
  //
  // The journal opens in Append mode: a restart never erases the log it must
  // recover from. start() replays any existing records before serving.
  //
  // idleSweepIntervalNs > 0 arms the idle time sweeper: a background thread
  // that, while last-look holds are open, injects a TimeTick command through
  // the normal sequenced path at most once per interval. That is what expires
  // a hold on a QUIET symbol -- expiry otherwise runs only when traffic
  // arrives. Going through the command stream (journaled, timestamped by the
  // same sequencer clock) keeps replay deterministic: the sweep is a record
  // like any other. 0 = off (the historical behavior; tests drive time by
  // submitting commands or calling engine().tick()).
  SequencedShard(SymbolConfig cfg, const std::string& journalPath, Book book = Book{},
                 Journal::Sync journalSync = Journal::Sync::Full,
                 TimeSource clock = &SequencedShard::systemNowNs,
                 int64_t idleSweepIntervalNs = 0, CheckpointConfig checkpointCfg = {})
      : journalPath_(journalPath),
        cfg_(cfg),
        bookProto_(book),  // pristine (empty) copy: scratch snapshot validation clones it
        checkpointCfg_(checkpointCfg),
        startGenerations_(scanGenerations(journalPath)),
        // Append to the newest journal segment when checkpoint generations
        // exist, else to the legacy single file (historical layout).
        journal_(initialJournalPath(journalPath, *startGenerations_), journalSync,
                 Journal::OpenMode::Append),
        consumer_(cfg, journal_, outbound_, std::move(book), clock, this),
        symbol_(cfg.id),
        sweepClock_(std::move(clock)),
        idleSweepNs_(idleSweepIntervalNs)
  {
    ingress_.enableDrainOnStop();
    outbound_.enableDrainOnStop();
    jitterState_ = 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(symbol_);
    rollThresholds();
    if (journalSync == Journal::Sync::Group)
    {
      consumer_.enableGroupCommit();
    }
  }

  ~SequencedShard()
  {
    stopIdleSweeper();
    waitCheckpointPublish();  // never destroy members under a live background writer
  }

  // Subscribe an outbound consumer (exec-report, market-data, ...). Must be
  // called before start().
  bool subscribeOutbound(IEngineEventListener* l, bool required = true)
  {
    return outbound_.subscribe(l, required, waitMode_);
  }

  // Checkpoint hook: runs on the consumer thread after each checkpoint
  // generation is published (snapshot renamed in, journal rotated), with the
  // boundary ts of the new generation. The gateway harness uses it to persist
  // session-layer sidecars at the same durability point -- e.g. the FIX
  // session sidecar (FixSessionSidecar::write to
  // FixSessionSidecar::pathFor(journal base), i.e. `<base>.fixsessions`).
  // Who runs this shard's consumers.
  //
  // By default the shard owns three threads -- the matching consumer, the
  // outbound subscribers and the idle sweeper -- which is the right shape for
  // a shard on a machine it owns. A process holding hundreds of shards cannot
  // afford three threads each, so it turns this off and calls pollOnce() and
  // sweepOnce() from a thread of its own, over as many shards as it likes.
  //
  // Set before start(). With threads off, nothing runs unless somebody steps
  // it: a shard nobody steps accepts commands and matches none of them.
  void setOwnThreads(bool own) noexcept { ownThreads_ = own; }
  bool ownThreads() const noexcept { return ownThreads_; }

  // Keep an InstrumentRegistry in step with this shard's command stream.
  //
  // "The WAL is the configuration source of truth, not an external store" is
  // the contract on InstrumentRegistry::apply, and apply() is the only thing
  // that can keep it. Nothing called it: the shard replayed its journal into
  // the engine and into nothing else, so a restarted venue came up with an
  // engine that knew all of its state and a registry that knew no
  // instruments -- and the control plane validates every operator request
  // against that registry. Wired here, every record the shard applies is
  // offered to apply(): the snapshot's config section, each replayed segment
  // and every command sequenced afterwards, in stream order.
  //
  // Set before start(), and null (the default) leaves the shard as it was.
  //
  // Thread rule: the registry is written on the CONSUMER thread from here on.
  // A deployment that also serves a ControlApi against the same registry must
  // reach it from that thread too -- the control server on the thread that
  // steps the shard -- or the operator's read races the replay's write.
  void setRegistry(InstrumentRegistry* reg) noexcept { consumer_.setRegistry(reg); }

  // One pass over this shard: the matching consumer, then every outbound
  // subscriber. Returns true if anything was delivered -- a driver that gets
  // false from every shard it owns is the one that should wait.
  //
  // Only for a shard with setOwnThreads(false), and only from one thread at a
  // time: this is the ring's single reader.
  bool pollOnce()
  {
    bool any = ingress_.pollConsumer(0);
    const uint32_t n = outbound_.consumerCount();
    for (uint32_t i = 0; i < n; ++i)
    {
      any = outbound_.pollConsumer(i) || any;
    }
    return any;
  }

  // Where the thread stepping this shard goes to sleep.
  //
  // A shard with no threads of its own has nowhere to wait: both of its buses
  // are private, so the driver could not have reached their wait points even
  // if they had one that suited it -- and neither does, because a bus's
  // condition variable wakes a consumer of THAT bus, and this driver steps
  // many shards. So the driver either spun a core per group of shards, or ran
  // a clock of its own and hopped every couple of milliseconds, which is the
  // same cost paid in latency instead.
  //
  // Point the shard at a WakeSet and both of its buses wake it: a command
  // published into ingress from a gateway thread, and an engine event
  // published outbound from the driver's own step. One set, however many
  // shards hang off it. Set before start(), with the buses: after that the
  // publish path is fixed and the call does nothing (EventBus::setWakeSet).
  //
  // A shard nobody points at a set is the shard as it was.
  void setWakeSet(lrvx::WakeSet* set) noexcept
  {
    ingress_.setWakeSet(set);
    outbound_.setWakeSet(set);
  }

  lrvx::WakeSet* wakeSet() const noexcept { return ingress_.wakeSet(); }

  // Is there anything for a step to do right now? Exactly the look pollOnce()
  // starts with -- the matching consumer, then every outbound subscriber --
  // without the delivery, which is what makes it usable as the predicate of a
  // WakeSet park: that predicate runs under the set's mutex, where delivering
  // anything would hold up every publisher trying to wake the set.
  //
  // Reads the stepping driver's own cursors, so it is for the driver of this
  // shard to call and nobody else -- the same single-reader contract as
  // pollOnce().
  bool hasPending() const noexcept
  {
    if (ingress_.consumerHasPending(0))
    {
      return true;
    }
    const uint32_t n = outbound_.consumerCount();
    for (uint32_t i = 0; i < n; ++i)
    {
      if (outbound_.consumerHasPending(i))
      {
        return true;
      }
    }
    return false;
  }

  // How this shard's consumers wait when there is nothing to do. Active
  // waiting is the default and the right answer for a shard on a machine it
  // owns: it is the lowest latency there is. It is the wrong answer for a
  // process holding hundreds of shards, where the idle spinning is the whole
  // CPU budget. Set before start() and before subscribing outbound
  // consumers -- it applies to the subscriptions taken after it.
  void setWaitMode(lrvx::ConsumerWaitMode mode) noexcept { waitMode_ = mode; }

  // The lane this shard takes its checkpoint pauses on, shared with every
  // other shard driven by the same thread. nullptr (the default) means the
  // shard pauses whenever it likes, which is right when it owns a thread.
  // Set before start().
  void setCheckpointLane(CheckpointLane* lane) noexcept { lane_ = lane; }

  // Automatic checkpoints that found the lane, or their own previous
  // snapshot, still busy. The shard asks again at its next boundary.
  uint64_t checkpointsSkippedBusy() const noexcept
  {
    return checkpointsSkippedBusy_.load(std::memory_order_acquire);
  }

  // Every pause this shard has taken for a checkpoint, added up. One shard on
  // one thread cares about the last one; a thread carrying several cares about
  // the total, because that is how long it was not matching anything.
  int64_t checkpointPauseTotalNs() const noexcept
  {
    return checkpointPauseTotalNs_.load(std::memory_order_acquire);
  }

  // The thresholds this shard actually uses, after its jitter cut.
  uint64_t effectiveMaxSegmentRecords() const noexcept
  {
    return effMaxRecords_.load(std::memory_order_relaxed);
  }
  uint64_t effectiveMaxSegmentBytes() const noexcept
  {
    return effMaxBytes_.load(std::memory_order_relaxed);
  }

  // Keep it fast: matching is paused for its duration. Set before start().
  void onCheckpoint(std::function<void(int64_t boundaryTs)> hook)
  {
    checkpointHook_ = std::move(hook);
  }

  void start()
  {
    // Recovery FIRST: newest valid snapshot + its tail segments (or, without
    // one, the full journal history) replayed into the engine before any new
    // command is served. Replayed events are not re-published outbound --
    // reconnecting clients reconcile via snapshots, not a re-broadcast of
    // history.
    recovered_ = recoverAll();
    outbound_.setOwnConsumerThreads(ownThreads_);
    ingress_.setOwnConsumerThreads(ownThreads_);
    outbound_.start();  // must be live before the matching thread publishes
    ingress_.subscribe(&consumer_, true, waitMode_);
    ingress_.start();
    if (idleSweepNs_ > 0 && ownThreads_)
    {
      startIdleSweeper();
    }
    ready_ = true;
  }

  // Recovery gate: callers hold traffic until ready(); recoveredCommands()
  // reports how much of the journal was replayed on start().
  bool ready() const noexcept { return ready_; }
  uint64_t recoveredCommands() const noexcept { return recovered_; }

  // The shard's engine, for wiring (setLedger, fees, MMP) before start() and
  // for off-hot-path queries (book, snapshots) after quiescence.
  MatchingEngine<Book>& engine() noexcept { return consumer_.engine(); }
  const MatchingEngine<Book>& engine() const noexcept { return consumer_.engine(); }

  // Producer side (gateway). Returns the ingress sequence number.
  // `recvMonoNs` is the steady-clock moment the producer took the command off
  // the wire; pass 0 when there is no wire (tests, replay drivers).
  // Returns kSubmitRejected without enqueueing when the shard has failed: the
  // consumer would only drop it, and queueing work for a shard that cannot
  // apply it hides the failure behind a growing backlog.
  static constexpr int64_t kSubmitRejected = -1;

  int64_t submit(const InboundCommand& cmd, int64_t recvMonoNs = 0)
  {
    if (failed())
    {
      return kSubmitRejected;
    }
    InboundCommandEvent ev{cmd};
    ev.recvMonoNs = recvMonoNs;
    ev.ingressMonoNs = venueMonoNs();
    return ingress_.publish(std::move(ev));
  }

  // One sweep: request a checkpoint if the segment has grown past its
  // threshold, and nudge expiry if holds are open and the interval has
  // passed. Cheap and non-blocking -- both arms end in submit(), which
  // publishes to the ingress ring.
  //
  // Takes no time argument on purpose: the clock is the shard's own (the
  // sequencer clock, which is what makes the sweep replay deterministically),
  // and it is read only when there is actually something to expire, so a
  // caller sweeping hundreds of shards does not pay a clock read per shard
  // per pass.
  //
  // Returns true if it submitted anything.
  bool sweepOnce()
  {
    bool submitted = false;
    // Checkpoint auto-trigger: the current segment crossed its record/byte
    // threshold. Only requests (exchange guards against re-requesting); the
    // snapshot itself runs on the consumer thread at the next command
    // boundary, and the TimeTick nudge guarantees one on a quiet symbol.
    if (((checkpointCfg_.maxSegmentRecords > 0 &&
          journal_.count() >= effMaxRecords_.load(std::memory_order_relaxed)) ||
         (checkpointCfg_.maxSegmentBytes > 0 &&
          journal_.bytes() >= effMaxBytes_.load(std::memory_order_relaxed))) &&
        !checkpointRequested_.exchange(true, std::memory_order_acq_rel))
    {
      submit(InboundCommand{TimeTick{symbol_}});
      submitted = true;
    }
    if (consumer_.engine().openHolds() == 0)
    {
      return submitted;
    }
    const int64_t now = sweepClock_();
    if (now - lastSweepNs_ < idleSweepNs_)
    {
      return submitted;
    }
    lastSweepNs_ = now;
    submit(InboundCommand{TimeTick{symbol_}});
    return true;
  }

  // Waits until everything submitted so far has been matched and published.
  //
  // With no threads of its own the shard has to be stepped to get there, and
  // the caller of flush() is the only thread that can be doing it -- an
  // executor calling flush() on one of its own shards would be waiting for
  // itself. So flush() steps.
  void flush()
  {
    if (!ownThreads_)
    {
      while (pollOnce())
      {
      }
    }
    ingress_.flush();
    outbound_.flush();
  }

  void stop()
  {
    stopIdleSweeper();
    flush();
    ingress_.stop();
    outbound_.flush();
    outbound_.stop();
    waitCheckpointPublish();  // a checkpoint in flight publishes before shutdown
    journal_.flush();
  }

  // The shard stopped applying because a command threw part-way through. Its
  // journal is the authority from here: restart and recover rather than trust
  // what is in memory.
  bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

  // Timestamp of the command that failed, or 0 if none has.
  int64_t failedAtTs() const noexcept { return failedAtTs_.load(std::memory_order_acquire); }

  void noteApplyFailure(int64_t ts, const char* why) noexcept
  {
    failedAtTs_.store(ts, std::memory_order_release);
    failed_.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "lrvx-venue: ERROR shard for symbol %u stopped applying at ts=%lld: %s; "
                 "in-memory state no longer matches the journal, recover from disk\n",
                 static_cast<unsigned>(symbol_), static_cast<long long>(ts), why ? why : "?");
  }

  uint64_t journaled() const noexcept { return journal_.count(); }

  // Durability barriers taken by the journal (Sync::Group). One per drained
  // ingress batch rather than one per command, which is the whole trade.
  uint64_t journalSyncs() const noexcept { return journal_.syncs(); }

  // Checkpoint on demand (the control-plane SnapshotNow verb): requests a
  // snapshot at the next command boundary on the consumer thread -- the only
  // point of natural quiescence -- and waits for it. The TimeTick nudge
  // guarantees a boundary even on a quiet symbol. Deliberately NOT a
  // journaled verb: a snapshot must never be replay-visible (the nudge itself
  // is an ordinary, harmless time sweep record). The consumer pause covers
  // only the state CLONE + journal rotation; this call additionally waits for
  // the background publish, so it still returns true only once the new
  // generation is on disk.
  bool checkpointNow()
  {
    if (!ready_)
    {
      return false;
    }
    const uint64_t before = checkpoints_.load(std::memory_order_acquire);
    // Drain FIRST: the request must be observed no earlier than the boundary
    // of the last command submitted before this call -- otherwise a lagging
    // consumer would snapshot an earlier boundary (correct but surprising).
    flush();
    // Somebody is waiting for the answer, so this one waits for the lane
    // instead of skipping when it is busy.
    checkpointMandatory_.store(true, std::memory_order_release);
    checkpointRequested_.store(true, std::memory_order_release);
    submit(InboundCommand{TimeTick{symbol_}});
    flush();  // the boundary ran: clone taken, background publish spawned
    waitCheckpointPublish();
    return checkpoints_.load(std::memory_order_acquire) > before;
  }

  uint64_t checkpointsTaken() const noexcept
  {
    return checkpoints_.load(std::memory_order_acquire);
  }

  // Background snapshot publishes that did not produce a snapshot. Nonzero
  // means recovery will replay from an older generation than the newest
  // boundary the shard reached.
  uint64_t checkpointPublishFailures() const noexcept
  {
    return checkpointPublishFailures_.load(std::memory_order_acquire);
  }

  // Consumer-thread stall of the most recent checkpoint: everything between
  // the moment the consumer stops matching and the moment it resumes -- the
  // wait for the previous publish and for the checkpoint lane, the state
  // clone, the journal rotation, the checkpoint hook, and the mutex and thread
  // spawn that hand the snapshot to the background. Serialization itself runs
  // on that background thread and is not part of it. Observability gauge: an
  // operator sizes the venue's worst-case matching gap from this number, so
  // everything the consumer spends inside a checkpoint has to be in it.
  int64_t lastCheckpointPauseNs() const noexcept
  {
    return lastCheckpointPauseNs_.load(std::memory_order_acquire);
  }

  // Records recovery applied from a snapshot file (0 = no snapshot was
  // consumed: fresh start or full-history replay). Lets callers -- and tests
  // -- prove recovery really went through the snapshot path instead of
  // silently replaying the whole journal history.
  uint64_t recoveredFromSnapshotRecords() const noexcept { return recoveredSnapshot_; }

 private:
  class MatchingConsumer : public ICommandListener
  {
   public:
    MatchingConsumer(SymbolConfig cfg, Journal& journal, OutboundBus& out, Book book,
                     TimeSource clock, SequencedShard* owner)
        : journal_(journal),
          out_(out),
          clock_(std::move(clock)),
          owner_(owner),
          engine_(cfg, [this](const OutboundEvent& ev)
                  {
                    if (replaying_)
                    {
                      return;
                    }
                    EngineEventMsg msg{ev};
                    // Provenance of the command being applied, captured at
                    // stage time -- under group commit the publish happens
                    // later, in onBatchEnd, when these members already
                    // describe a different command.
                    msg.causeRecvMonoNs = causeRecvMonoNs_;
                    msg.causeIngressMonoNs = causeIngressMonoNs_;
                    if (staged_ != nullptr)
                    {
                      // Group commit: an event is a promise, and a promise
                      // made before the record behind it is durable is the
                      // promise this mode exists to keep. Held until the
                      // barrier at the end of the batch. publishMonoNs stays
                      // 0 until the actual publish -- stamping it here would
                      // hide the barrier wait, which is the whole cost of
                      // this mode and exactly what the stamp exists to show.
                      staged_->push_back(std::move(msg));
                      return;
                    }
                    msg.publishMonoNs = venueMonoNs();
                    out_.publish(std::move(msg)); }, std::move(book), cfg.matchPolicy)
    {
    }

    // Replay the journal's intact prefix into the engine (suppressing outbound
    // re-publication) and seed the timestamp floor, so records appended after
    // recovery stay monotonic relative to the recovered stream.
    uint64_t recover(const std::string& path)
    {
      const auto records = Journal::loadTimed(path);
      replaying_ = true;
      for (const auto& [ts, cmd] : records)
      {
        engine_.submit(cmd, ts);
        offerToRegistry(cmd);
        if (ts > lastTs_)
        {
          lastTs_ = ts;
        }
      }
      replaying_ = false;
      return records.size();
    }

    // Apply a snapshot file the shard has ALREADY validated end-to-end into
    // the real engine (recovery path: outbound suppressed like journal
    // replay, timestamp floor seeded from the snapshot records).
    uint64_t applySnapshot(const std::string& path)
    {
      const auto records = Journal::loadTimed(path);
      replaying_ = true;
      for (const auto& [ts, cmd] : records)
      {
        engine_.applySnapshotRecord(cmd, ts);
        // The snapshot's config section is where an instrument listed before
        // the oldest surviving segment still lives, so recovery through a
        // checkpoint rebuilds the registry the same way a full replay does.
        offerToRegistry(cmd);
        if (ts > lastTs_)
        {
          lastTs_ = ts;
        }
      }
      replaying_ = false;
      return records.size();
    }

    void onCommand(const InboundCommandEvent& ev) override
    {
      if (owner_->failed())
      {
        // Already diverged. Anything still in the ingress from before the
        // failure is dropped rather than journaled: a record appended now
        // would describe a state transition this engine never made.
        return;
      }
      causeRecvMonoNs_ = ev.recvMonoNs;
      causeIngressMonoNs_ = ev.ingressMonoNs;
      const int64_t ts = nextTs();
      // The engine is not transactional: there is no rollback, so a throw
      // part-way through applying leaves memory holding half a command while
      // the journal -- written first, deliberately -- holds the whole of it.
      // Restarting from the journal is correct. Carrying on is not: the shard
      // would keep serving a state that disagrees with what it would recover
      // into, and nothing would say so. Stop here instead.
      try
      {
        journal_.append(ev.cmd, ts);  // write-ahead, before applying
        engine_.submit(ev.cmd, ts);   // the SAME timestamp the journal holds
        offerToRegistry(ev.cmd);      // the same record a restart would replay
      }
      catch (const std::exception& e)
      {
        owner_->noteApplyFailure(ts, e.what());
        return;
      }
      catch (...)
      {
        owner_->noteApplyFailure(ts, "unknown exception");
        return;
      }
      lastBatchTs_ = ts;
      if (staged_ == nullptr)
      {
        owner_->maybeCheckpoint(ts);  // command boundary: natural quiescence
      }
    }

    // The ingress is drained. Under group commit this is the durability
    // barrier: one fsync for the whole batch, and only then are the events it
    // produced allowed out. Under the other modes the records are already as
    // durable as they are going to get and nothing was staged, so this costs a
    // branch.
    void onBatchEnd() override
    {
      if (staged_ == nullptr)
      {
        return;
      }
      journal_.sync();
      const int64_t published = venueMonoNs();  // one stamp: one barrier
      for (EngineEventMsg& m : *staged_)
      {
        m.publishMonoNs = published;
        out_.publish(m);
      }
      staged_->clear();
      // Checkpointing is deferred to here for the same reason: a snapshot
      // taken mid-batch would capture state whose journal records are not
      // durable yet.
      if (lastBatchTs_ != 0)
      {
        owner_->maybeCheckpoint(lastBatchTs_);
      }
    }

    // Called once at construction when the journal batches its barrier.
    void enableGroupCommit() { staged_ = &stagedStorage_; }

    void setRegistry(InstrumentRegistry* reg) noexcept { registry_ = reg; }

    MatchingEngine<Book>& engine() noexcept { return engine_; }
    const MatchingEngine<Book>& engine() const noexcept { return engine_; }

   private:
    // Every applied record, offered to the registry in the order the engine
    // saw it. apply() answers false for anything that is not configuration --
    // an order, a Restore* record, a re-listing of an instrument it already
    // holds -- and that is not an error here: the stream carries both kinds
    // and the registry is the one deciding which is which.
    void offerToRegistry(const InboundCommand& cmd)
    {
      if (registry_ != nullptr)
      {
        (void)registry_->apply(cmd);
      }
    }

    // Strictly monotonic, non-zero sequencer time: a stalled or backwards
    // clock still yields lastTs_ + 1, so replay ordering is unambiguous.
    int64_t nextTs()
    {
      int64_t t = clock_();
      if (t <= lastTs_)
      {
        t = lastTs_ + 1;
      }
      lastTs_ = t;
      return t;
    }

    Journal& journal_;
    OutboundBus& out_;
    int64_t causeRecvMonoNs_ = 0;
    int64_t causeIngressMonoNs_ = 0;
    std::vector<EngineEventMsg> stagedStorage_;
    std::vector<EngineEventMsg>* staged_{nullptr};  // non-null = group commit
    int64_t lastBatchTs_{0};
    TimeSource clock_;
    SequencedShard* owner_;
    InstrumentRegistry* registry_{nullptr};
    bool replaying_{false};
    int64_t lastTs_{0};
    MatchingEngine<Book> engine_;
  };

  // ---- checkpoint machinery (consumer thread unless noted) ----

  static std::string initialJournalPath(const std::string& base, const Generations& g)
  {
    return g.segments.empty() ? base : segmentPath(base, g.segments.back());
  }

  // Recovery: newest snapshot that validates end-to-end, plus every journal
  // segment at or after it; an invalid snapshot logs and falls back a
  // generation; no valid snapshot at all -> full-history replay (legacy
  // single file, then all segments in order).
  uint64_t recoverAll()
  {
    // The listing the constructor chose the journal segment from, taken once:
    // the directory is the whole venue's, so a second scan here listed every
    // shard's files again to learn what the first already had. Nothing writes
    // a generation of this shard between construction and its first start. A
    // later start() scans afresh, the checkpoints of the run before it having
    // added to the directory.
    const Generations g = startGenerations_ ? std::move(*startGenerations_)
                                            : scanGenerations(journalPath_);
    startGenerations_.reset();
    int64_t chosen = -1;
    for (auto it = g.snapshots.rbegin(); it != g.snapshots.rend(); ++it)
    {
      if (validateSnapshot(snapshotPath(journalPath_, *it)))
      {
        chosen = *it;
        break;
      }
      std::fprintf(stderr, "lrvx-venue: WARN snapshot %s invalid, falling back a generation\n",
                   snapshotPath(journalPath_, *it).c_str());
    }
    uint64_t n = 0;
    if (chosen >= 0)
    {
      recoveredSnapshot_ = consumer_.applySnapshot(snapshotPath(journalPath_, chosen));
      n += recoveredSnapshot_;
      for (const int64_t ts : g.segments)
      {
        if (ts >= chosen)
        {
          n += consumer_.recover(segmentPath(journalPath_, ts));
        }
      }
      // Continue appending to the newest segment of the recovered history. A
      // crash between snapshot publish and segment rotation leaves a snapshot
      // with no segment of its own -- open (create) that segment now, so new
      // records never land in a segment older than the snapshot they follow.
      const int64_t cur =
          (!g.segments.empty() && g.segments.back() >= chosen) ? g.segments.back() : chosen;
      journal_.reopen(segmentPath(journalPath_, cur), Journal::OpenMode::Append);
      lastCheckpointTs_ = chosen;
    }
    else
    {
      // Falling back a generation is unbounded, but what pruning keeps is not.
      // Once no snapshot validates, the only thing covering history before the
      // first checkpoint is the pre-checkpoint journal -- and pruning deletes
      // it as soon as `retainGenerations` snapshots exist. Replaying what is
      // left would rebuild a state that looks plausible, is missing everything
      // before the oldest surviving segment, and would then be served. Refuse
      // instead: a venue that will not start is an incident, a venue that
      // starts on a truncated ledger is a disaster.
      const bool haveLegacy = std::filesystem::exists(journalPath_);
      if (!haveLegacy && (!g.snapshots.empty() || !g.segments.empty()))
      {
        throw std::runtime_error(
            "lrvx-venue: no snapshot validates and the pre-checkpoint journal " + journalPath_ +
            " is gone, so history cannot be replayed in full. Refusing to start on a "
            "truncated state -- restore a good snapshot generation from backup.");
      }
      if (!g.snapshots.empty())
      {
        std::fprintf(stderr, "lrvx-venue: WARN no valid snapshot for %s, full-history replay\n",
                     journalPath_.c_str());
      }
      n += consumer_.recover(journalPath_);  // legacy single file (absent -> 0 records)
      for (const int64_t ts : g.segments)
      {
        n += consumer_.recover(segmentPath(journalPath_, ts));
      }
    }
    return n;
  }

  // Scratch-validate a snapshot file end-to-end -- framing/CRC via loadTimed,
  // SnapshotBegin/SnapshotEnd structure (a torn tail always lacks the End
  // record, which is written last), every Restore* invariant, and the state
  // hash at SnapshotEnd -- against a throwaway engine + ledger. The real
  // engine is touched only after the whole file verified, so a corrupt
  // snapshot can never pollute it.
  bool validateSnapshot(const std::string& path)
  {
    std::vector<std::pair<int64_t, InboundCommand>> records;
    try
    {
      records = Journal::loadTimed(path);
    }
    catch (const JournalFormatError& e)
    {
      // A snapshot in a format this build does not read is one more generation
      // that does not validate. Falling back is already the designed answer to
      // that; the reason is printed so the fallback is not read as bit rot.
      std::fprintf(stderr, "lrvx-venue: WARN %s\n", e.what());
      return false;
    }
    if (records.size() < 2 || !std::holds_alternative<SnapshotBegin>(records.front().second) ||
        !std::holds_alternative<SnapshotEnd>(records.back().second))
    {
      return false;
    }
    Ledger scratch;
    // The probe matches under the instrument's own allocation rule: a
    // snapshot is validated by an engine configured like the one that wrote
    // it, and configHash folds the policy, so a price-time probe would refuse
    // every pro-rata generation this shard ever published.
    MatchingEngine<Book> probe(cfg_, [](const OutboundEvent&) {}, Book{bookProto_}, cfg_.matchPolicy);
    if (consumer_.engine().ledger() != nullptr)
    {
      probe.setLedger(&scratch, consumer_.engine().venueAccount());
    }
    for (const auto& [ts, cmd] : records)
    {
      if (!probe.applySnapshotRecord(cmd, ts))
      {
        return false;
      }
    }
    return true;
  }

  // Consumer-thread entry at every command boundary (see onCommand).
  void maybeCheckpoint(int64_t boundaryTs)
  {
    if (!checkpointRequested_.load(std::memory_order_acquire))
    {
      return;
    }
    checkpointRequested_.store(false, std::memory_order_release);
    doCheckpoint(boundaryTs, checkpointMandatory_.exchange(false, std::memory_order_acq_rel));
  }

  // Consumer thread only. ASYNCHRONOUS checkpoint: under the pause only the
  // engine state is CLONED (deep copy, ledger by value) and the journal is
  // rotated onto the segment named by the boundary ts -- the segment boundary
  // and the snapshot content are fixed at the same command boundary.
  // Serialization + fsync + rename (atomic publish) + retention pruning run on
  // a background thread against the clone, so matching resumes after an
  // O(clone) pause instead of O(serialize+fsync). fork()-based copy-on-write
  // was rejected: this process is multi-threaded (see the rationale at
  // MatchingEngine::cloneForSnapshot).
  //
  // Crash window: between rotation and the background rename the disk holds
  // "segment ts exists, snapshot ts absent (only a .tmp at most)". recoverAll
  // tolerates exactly that: the .tmp never parses as a generation, recovery
  // picks the PREVIOUS valid snapshot and replays BOTH tail segments (ts-1's
  // and ts's) after it, reproducing the state; the snapshot only ever appears
  // atomically via rename. A failed background publish logs a WARN and leaves
  // the same recoverable layout.
  void doCheckpoint(int64_t ts, bool mandatory)
  {
    if (ts <= lastCheckpointTs_)
    {
      return;  // repeated request inside one boundary: state already on disk
    }
    // No snapshot queue: at most one background publish in flight. What
    // differs between the two kinds of request is what to do when the
    // previous one has not finished, or when another shard on this driver is
    // in its own pause.
    //
    // An automatic checkpoint SKIPS. Waiting here would put a disk wait
    // inside the pause -- and under a shared driver, every shard behind this
    // one waits for that disk too. The threshold that triggered it has not
    // gone away, so the sweep asks again within its next interval. A skipped
    // checkpoint is a snapshot not taken, never a record not written: the
    // journal is untouched and recovery replays further, which is exactly
    // what it does between any two checkpoints.
    //
    // A checkpoint asked for by name waits, because somebody is waiting for
    // the answer.
    //
    // The pause opens HERE, before the wait and before the lane, because both
    // run on the consumer thread with matching stopped. A shard crowded out of
    // the lane stands still for as long as whoever holds it, and a gauge that
    // starts after that wait reports a pause the shard did not have. The
    // skipping branch below returns without recording anything: an automatic
    // checkpoint that skips takes no pause worth the name.
    const auto pause0 = std::chrono::steady_clock::now();
    if (mandatory)
    {
      waitCheckpointPublish();
      if (lane_ != nullptr)
      {
        lane_->enterBlocking();
      }
    }
    else
    {
      if (publishInFlight())
      {
        noteCheckpointSkipped();
        return;
      }
      if (lane_ != nullptr && !lane_->tryEnter())
      {
        noteCheckpointSkipped();
        return;
      }
    }
    auto clone = consumer_.engine().cloneForSnapshot(Book{bookProto_});
    journal_.flush();
    journal_.reopen(segmentPath(journalPath_, ts), Journal::OpenMode::Truncate);
    syncDir();
    lastCheckpointTs_ = ts;
    if (checkpointHook_)
    {
      checkpointHook_(ts);  // sidecar persistence rides the same boundary (consumer thread)
    }
    // The next threshold is a fresh cut, so shards that drifted into step
    // during this segment do not stay there.
    rollThresholds();
    auto publish = [this, ts, lane = lane_, cl = std::move(clone)]() mutable -> bool
    {
      // Whatever happens below, the lane is handed back: it covers the
      // snapshot write as well as the pause, so that one lane means one
      // snapshot on the disk at a time and one publish thread, not seventy.
      struct LaneGuard
      {
        CheckpointLane* l;
        ~LaneGuard()
        {
          if (l != nullptr)
          {
            l->leave();
          }
        }
      } laneGuard{lane};
      const std::string snap = snapshotPath(journalPath_, ts);
      // Everything below runs on a task whose result is only ever waited on,
      // never got: an exception escaping here would be stored in the future
      // and destroyed with it, taking the only evidence that the checkpoint
      // did not happen. Writing the snapshot can throw for ordinary reasons --
      // the Journal constructor throws when it cannot open the file, and the
      // generation sweep touches the filesystem -- so the task converts every
      // failure into the same visible shape instead: a WARN naming the
      // snapshot and the reason, and a bump of checkpointPublishFailures_.
      try
      {
        const std::string tmp = snap + ".tmp";
        {
          Journal out(tmp, Journal::Sync::Off, Journal::OpenMode::Truncate);
          cl.engine->writeSnapshot(out);
          out.flush();  // durable BEFORE the rename publishes it
        }
        std::error_code ec;
        std::filesystem::rename(tmp, snap, ec);
        if (ec)
        {
          return notePublishFailure(snap, ec.message().c_str());
        }
        syncDir();
        pruneGenerations();
        checkpoints_.fetch_add(1, std::memory_order_release);
        return true;
      }
      catch (const std::exception& e)
      {
        return notePublishFailure(snap, e.what());
      }
      catch (...)
      {
        return notePublishFailure(snap, "unknown exception");
      }
    };
    {
      // Still the consumer thread, still not matching: the mutex and the
      // thread the spawn creates are part of the stall, and a thread spawn is
      // tens of microseconds -- the same order as the clone this gauge was
      // built to report. The pause closes after the spawn returns.
      std::lock_guard<std::mutex> lk(ckptMx_);
      ckptPending_ = std::async(std::launch::async, std::move(publish)).share();
    }
    const int64_t pauseNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pause0)
                                .count();
    lastCheckpointPauseNs_.store(pauseNs, std::memory_order_release);
    checkpointPauseTotalNs_.fetch_add(pauseNs, std::memory_order_relaxed);
    if (lane_ != nullptr)
    {
      // How long the DRIVER was stopped, by anybody on it. Per-shard pauses
      // do not add up to anything an operator can act on once shards share a
      // thread.
      lane_->notePause(pauseNs);
    }
  }

  // A failed publish leaves the previous generation as the newest valid one,
  // which recovery already tolerates: both tail segments replay after it. That
  // is correct but invisible, so the failure is counted where an operator can
  // see it -- a venue whose snapshots have quietly stopped working is one
  // whose next recovery replays further than anybody expects.
  bool notePublishFailure(const std::string& snap, const char* why) noexcept
  {
    checkpointPublishFailures_.fetch_add(1, std::memory_order_release);
    std::fprintf(stderr, "lrvx-venue: WARN checkpoint publish failed for %s: %s\n", snap.c_str(),
                 why ? why : "?");
    return false;
  }

  // Is the previous snapshot still being written? Asked instead of waited on
  // by every automatic checkpoint.
  bool publishInFlight()
  {
    std::shared_future<bool> f;
    {
      std::lock_guard<std::mutex> lk(ckptMx_);
      f = ckptPending_;
    }
    return f.valid() && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
  }

  void noteCheckpointSkipped() noexcept
  {
    checkpointsSkippedBusy_.fetch_add(1, std::memory_order_release);
    if (lane_ != nullptr)
    {
      lane_->noteSkipped();
    }
  }

  // Deterministic per-shard cut below the configured thresholds. Deterministic
  // because a venue that checkpoints at different points on every run is a
  // venue whose recoveries cannot be compared; per-shard because the whole
  // point is that two shards must not cross their thresholds together.
  static uint64_t splitmix64(uint64_t& state) noexcept
  {
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  static uint64_t cut(uint64_t threshold, unsigned pct, uint64_t r) noexcept
  {
    if (threshold == 0 || pct == 0)
    {
      return threshold;
    }
    const unsigned capped = pct > 90 ? 90 : pct;
    // Up to `capped` percent off, never to zero: a threshold of zero means
    // "disabled" everywhere else in this file and must not appear by accident.
    const uint64_t span = threshold / 100 * capped;
    const uint64_t off = span == 0 ? 0 : r % (span + 1);
    const uint64_t v = threshold - off;
    return v == 0 ? 1 : v;
  }

  // Written on the consumer thread (after a checkpoint) and read by whoever
  // sweeps -- the sweeper thread, or an external driver. Atomic and relaxed:
  // the reader wants a threshold, not a particular one, and reading the
  // previous segment's cut one sweep longer costs a sweep interval of delay
  // on a checkpoint request.
  void rollThresholds() noexcept
  {
    effMaxRecords_.store(cut(checkpointCfg_.maxSegmentRecords, checkpointCfg_.triggerJitterPct,
                             splitmix64(jitterState_)),
                         std::memory_order_relaxed);
    effMaxBytes_.store(cut(checkpointCfg_.maxSegmentBytes, checkpointCfg_.triggerJitterPct,
                           splitmix64(jitterState_)),
                       std::memory_order_relaxed);
  }

  // Wait for the in-flight background snapshot publish, if any. Safe from any
  // thread; the consumer thread calls it before starting the next checkpoint.
  // wait() rather than get() is deliberate and only sound because the task
  // above cannot throw: it catches everything and reports through the counter.
  void waitCheckpointPublish()
  {
    std::shared_future<bool> f;
    {
      std::lock_guard<std::mutex> lk(ckptMx_);
      f = ckptPending_;
    }
    if (f.valid())
    {
      f.wait();
    }
  }

  void pruneGenerations()
  {
    namespace fs = std::filesystem;
    const Generations g = scanGenerations(journalPath_);
    const size_t retain =
        checkpointCfg_.retainGenerations < 1 ? 1 : static_cast<size_t>(checkpointCfg_.retainGenerations);
    std::error_code ec;
    if (g.snapshots.size() > retain)
    {
      const int64_t keepFrom = g.snapshots[g.snapshots.size() - retain];
      for (const int64_t ts : g.snapshots)
      {
        if (ts < keepFrom)
        {
          fs::remove(snapshotPath(journalPath_, ts), ec);
        }
      }
      for (const int64_t ts : g.segments)
      {
        if (ts < keepFrom)
        {
          fs::remove(segmentPath(journalPath_, ts), ec);
        }
      }
    }
    if (g.snapshots.size() >= retain)
    {
      fs::remove(journalPath_, ec);  // legacy pre-checkpoint file: oldest generation
    }
  }

  void syncDir() const
  {
    std::filesystem::path dir = std::filesystem::path(journalPath_).parent_path();
    const std::string d = dir.empty() ? "." : dir.string();
    // On POSIX a renamed file can exist while the directory entry naming it
    // does not, so the directory is synced too. Windows has no directory
    // handle and orders the rename itself; see lrvx/util/file_io.h.
    lrvx::fileio::syncDirectory(d);
  }

  // Idle sweeper: while holds are open, inject a TimeTick through the normal
  // ingress path at most once per idleSweepNs_ (per the injected TimeSource).
  // The consumer thread stamps and journals it like any other command, so the
  // sweep replays deterministically. The engine's openHolds() gauge keeps the
  // journal free of ticks when nothing is pending.
  //
  // The work is one call, and the thread below is only a way to call it. A
  // process holding hundreds of shards drives sweepOnce() from whatever it
  // already has going round -- see startIdleSweeper() for the default shape.
  void startIdleSweeper()
  {
    sweepStop_.store(false, std::memory_order_release);
    sweeper_ = makeThread(
        "venue.shard.sweeper", [this]
        {
          while (!sweepStop_.load(std::memory_order_acquire))
          {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            sweepOnce();
          } });
  }

  void stopIdleSweeper()
  {
    if (sweeper_.joinable())
    {
      sweepStop_.store(true, std::memory_order_release);
      sweeper_.join();
    }
  }

  std::string journalPath_;
  SymbolConfig cfg_;  // construction config (scratch validation engines)
  Book bookProto_;    // pristine copy of the ctor book, cloned per validation
  CheckpointConfig checkpointCfg_;
  // The constructor's directory listing, kept for the first recoverAll().
  // Declared before journal_, which is opened from it.
  std::optional<Generations> startGenerations_;
  Journal journal_;
  OutboundBus outbound_;
  MatchingConsumer consumer_;
  IngressBus ingress_;
  SymbolId symbol_{};
  TimeSource sweepClock_;
  int64_t idleSweepNs_{0};
  std::thread sweeper_;
  std::atomic<bool> sweepStop_{false};
  // Sweep cursor. Private to whoever calls sweepOnce(): the sweeper thread
  // when there is one, the external driver when there is not.
  int64_t lastSweepNs_{0};
  std::atomic<bool> checkpointRequested_{false};
  std::atomic<bool> checkpointMandatory_{false};
  std::atomic<uint64_t> checkpointsSkippedBusy_{0};
  CheckpointLane* lane_{nullptr};
  lrvx::ConsumerWaitMode waitMode_{lrvx::ConsumerWaitMode::ACTIVE};
  bool ownThreads_{true};
  uint64_t jitterState_{0};
  std::atomic<uint64_t> effMaxRecords_{0};
  std::atomic<uint64_t> effMaxBytes_{0};
  std::function<void(int64_t)> checkpointHook_;
  std::atomic<uint64_t> checkpoints_{0};
  std::atomic<uint64_t> checkpointPublishFailures_{0};
  std::atomic<bool> failed_{false};
  std::atomic<int64_t> failedAtTs_{0};
  int64_t lastCheckpointTs_{0};  // consumer thread (and pre-start recovery) only
  // Background snapshot publish: at most one in flight (doCheckpoint waits for
  // the previous). Guarded by ckptMx_ against checkpointNow()/stop() readers.
  std::mutex ckptMx_;
  std::shared_future<bool> ckptPending_;
  std::atomic<int64_t> lastCheckpointPauseNs_{0};
  std::atomic<int64_t> checkpointPauseTotalNs_{0};
  uint64_t recovered_{0};
  uint64_t recoveredSnapshot_{0};
  bool ready_{false};
};

}  // namespace lrvx::venue
