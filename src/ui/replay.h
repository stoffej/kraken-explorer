#pragma once

// Replay View: loads a candump / ASC / pcap / pcapng trace and plays it back on the
// interfaces (or into the trace) with the original timing, on its own jthread.

#include <atomic>
#include <cstdint>
#include <deque>
#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/bus_message.h"
#include "core/frame_cache_rec.h"
#include "core/trace_file_format.h"
#include "ui/file_dialog.h"

struct App;
struct FrameCache;
struct Iface;
struct Tasks;
struct WorkspaceTab;

inline constexpr uint32_t replay_error_id = 0xFFFFFFFF; // filter key of error frames
inline constexpr int replay_trace_only = -1;            // channel mapping: no interface

// A parsed trace file. frames[i].iface is an index into channels (e.g. "vcan0", "CH 1").
struct ReplayFile
{
    std::vector<BusMessage> frames;
    std::vector<std::string> channels;
    std::size_t last_channel = SIZE_MAX; // parser scratch: the channel of the previous frame
    // BLF parsed in blocks (frame cache build): a container stream cut by a block boundary leaves
    // an object's tail before this block's first object and its head after the last one.
    std::string blf_head;
    std::string blf_tail;
};

// Optional hooks for parsing on a loader thread: fraction gets bytes parsed / data size now
// and then, and a stop request ends the parse early (the partial file is returned).
struct ReplayParseProgress
{
    std::stop_token stop;
    std::atomic<float>* fraction = nullptr;  // set by the frame cache build (bytes done / size)
    std::atomic<uint64_t>* frames = nullptr; // frames read so far (frame cache build)
};

// Parses a whole file's bytes; frames without a usable line are skipped. VectorMdf reads ASAM
// MDF4 CAN bus logging (and the app's own export); frames of MF4 files come out per channel group.
[[nodiscard]] ReplayFile replay_parse(std::string_view data, TraceFileFormat format,
                                      const ReplayParseProgress& progress = {});
// The same into f, reusing its memory (cleared first).
void replay_parse_into(std::string_view data, TraceFileFormat format, ReplayFile& f, const ReplayParseProgress& progress = {});

// Bytes of the file header every block of records needs: pcap's 24-byte global header (byte
// order, time unit), BLF's "LOGG" header (start time), ASC's lines through "date" (the time the
// frame times count from). 0 for the other formats or a file too short to have one. The frame
// cache build parses pcap, BLF and ASC in blocks of whole records:
// replay_parse_into(header, records, ...); with an empty header, data is a whole file.
[[nodiscard]] std::size_t replay_header_size(std::string_view data, TraceFileFormat format);
void replay_parse_into(std::string_view header, std::string_view data, TraceFileFormat format, ReplayFile& f,
                       const ReplayParseProgress& progress = {});

// One row of the filter tree: a (channel, id) pair seen in the file.
struct ReplayIdRow
{
    uint16_t channel = 0;
    uint32_t id = 0; // replay_error_id for error frames
    bool extended = false; // of the first frame with this id, for the DBC lookup
    int count = 0;
    bool has_rx = false;
    bool has_tx = false;
    bool rx_on = false; // replayed when the frame's direction is enabled
    bool tx_on = false;
    bool brk = false;   // breakpoint: the player pauses before every replayed frame of this row
};

// Rows sorted by (channel, id), all enabled.
[[nodiscard]] std::vector<ReplayIdRow> replay_id_rows(const ReplayFile& file);

// A frame to send at `at_ns` after the start (speed 1). iface = target interface, or the
// file channel when target == replay_trace_only.
struct ReplayStep
{
    BusMessage msg;
    int64_t at_ns = 0;
    int target = replay_trace_only;
};

// The step of frame m (t0 = the first frame's timestamp), false when its row is disabled.
// mapping[channel] is an App::ifaces index or replay_trace_only; LIN and error frames always go
// to the trace only.
[[nodiscard]] bool replay_step(const BusMessage& m, int64_t t0, const std::vector<ReplayIdRow>& rows,
                               const std::vector<int>& mapping, ReplayStep& step);

// What the loader thread hands to the main thread (through Replay::loading).
struct ReplayLoaded
{
    std::string path; // empty on error
    std::string info;
    std::vector<std::string> channels; // the cache's channel names
    std::vector<ReplayIdRow> rows;
    std::vector<char> channel_lin;
    std::shared_ptr<const FrameCache> cache; // the frames of a loaded file (file.frames stays empty)
    bool parsed = false; // the cache was built now (the file was read), not just opened
};

// frames[from, to]: times since the first frame as parse_duration reads them ("90", "1:30",
// "2d 1:02:03"); empty or malformed = the start / end.
[[nodiscard]] std::span<const FrameCacheRec> replay_range(std::span<const FrameCacheRec> frames, std::string_view from,
                                                          std::string_view to);

// The loaded frames: the cache's records (empty without a cache).
[[nodiscard]] std::span<const FrameCacheRec> replay_frames(const ReplayLoaded& d);

// Replay::hold: the player runs, waits before its next frame, or sends one frame and waits again.
inline constexpr int replay_playing = 0;
inline constexpr int replay_paused = 1;
inline constexpr int replay_stepping = 2;

// State of one Replay View. Not movable (thread, atomics).
struct Replay
{
    bool open = false;
    FileDialog load_dialog;
    FileDialog db_dialog; // asked right after a trace is chosen: the CAN databases to decode it with
    ReplayLoaded data;        // the loaded file (data.info: name, count, duration or the load error)
    std::vector<int> mapping; // per file channel
    float speed = 1.0f; // 1 = the file's own timing
    bool fast = false;  // as fast as possible: no timing at all
    std::string range_from; // part of the file to play, times since its first frame ("" = start / end)
    std::string range_to;
    std::string break_at;   // breakpoints by time: times since the first frame, comma separated
    bool autoplay = false;
    bool loop = false;
    bool was_measuring = false;
    bool was_open = false;     // open last frame: closing the window cancels a load it started

    // Snapshot for the player thread, only touched while it is not running. It walks the frames
    // in place (no copy: a file can be bigger than RAM) and filters with play_rows.
    std::span<const FrameCacheRec> play_frames;
    std::shared_ptr<const FrameCache> play_cache; // keeps play_frames mapped, holds their overflow payloads
    std::vector<ReplayIdRow> play_rows;
    std::vector<int> play_mapping;
    std::vector<std::size_t> play_breaks; // break_at as indices into play_frames, sorted
    std::atomic<int> hold{replay_playing}; // replay_pause / replay_resume / replay_single_step, and breakpoints
    std::atomic<std::size_t> position{0};
    std::atomic<bool> running{false};
    // Replay window's frames/s: sampled every half second while playing; when the run ends, its
    // average (a fast run can be over in under a second). Main thread only.
    std::chrono::steady_clock::time_point rate_started{};
    std::chrono::steady_clock::time_point rate_at{};
    std::size_t rate_from = 0;
    double rate_fps = 0.0;
    bool rate_running = false;
    std::jthread player; // joined before the members it uses go away

    // Loader thread: reads + parses off the main thread. loader.joinable() == loading
    // (only the main thread touches the jthread object).
    std::atomic<float> load_fraction{0.0f}; // bytes parsed / file size, 1 when done
    std::atomic<bool> load_changed{false};  // the loader found the file changed since it was cached
    std::atomic<uint64_t> load_frames{0};   // messages read so far by the loader
    std::chrono::steady_clock::time_point load_started{}; // load timer (Replay info: "Loaded in ...")
    uint64_t load_bytes = 0;                // size of the file being loaded, for MB/s
    int64_t watched_mtime = 0;              // of data.path when loaded: a change reloads it
    uint64_t watched_size = 0;
    std::chrono::steady_clock::time_point watched{};
    std::future<bool> watch_check;          // size/mtime moved: does the content hash differ? (off the main thread)
    std::future<ReplayLoaded> loading; // set once by the loader; never set when stopped
    std::jthread loader; // last: stopped and joined first
};

// Player thread body: sends each enabled frame of play_frames when due (start + at_ns / speed;
// speed 0 = as fast as possible, no waiting), steps that cannot be
// sent on their interface go to the trace through tasks. Loops if asked. Clears running.
// A breakpoint (a row's brk, or an index in play_breaks) sets hold to replay_paused before its
// frame and wakes the main loop; the frame goes out on resume or step, at once.
void replay_run(std::stop_token stop, Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, double speed, bool loop);

// Starts reading and parsing a file (format from the extension, ASC otherwise) on r.loader;
// a load already running is cancelled. r.load_fraction shows the progress. Returns at once.
void replay_load(App& app, Replay& r, const std::string& path);

// Main thread: if the loader has finished, takes its result, maps each channel to the
// interface of the same name and returns true (starts autoplay if measuring). Errors end up
// in r.data.info. draw_replay calls it every frame.
bool replay_load_poll(App& app, Replay& r);

// Adds CAN databases to the network the replay sends into (the first network when it only
// goes to the trace), rebuilding the setup cache; failures go to the status bar.
void replay_add_databases(App& app, const Replay& r, const std::vector<std::string>& paths);

// Cancels a running load (joins the loader) and discards its result.
void replay_load_cancel(Replay& r);

// Builds the plan and starts the player; replay_stop joins it. hold = replay_stepping: the first
// frame, then paused.
void replay_start(Replay& r, std::deque<Iface>& ifaces, Tasks& tasks, int hold = replay_playing);
void replay_stop(Replay& r);

// While playing: pause before the next frame (when it is due), go on, or send one frame and stay
// paused. Time spent paused is not caught up.
void replay_pause(Replay& r);
void replay_resume(Replay& r);
void replay_single_step(Replay& r);

// The once-a-second check of the loaded file on disk (size/mtime, then its content hash off the
// main thread); a changed file is reloaded. draw_replay calls it; the idle main loop calls it
// without drawing a frame (main.cpp), so a loaded file costs no redraws.
void replay_watch(App& app, Replay& r);

// Runs autoplay (start/stop with the measurement) and, while r.open and tab is the current
// workspace tab, draws its "Replay" window. Call for every tab that has a Replay.
void draw_replay(App& app, const WorkspaceTab& tab, Replay& r);
