#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"

#include <string>
#include <vector>

namespace rsx
{
#ifdef RPCS3_UWP
	// Xbox hitch sampler (RSXThread.cpp): a helper thread reads every 1 ms what the RSX thread is
	// doing, as phase << 16 | the FIFO method being run, and logs where slow frames spent their time.
	enum uwp_rsx_phase : u32
	{
		uwp_phase_fifo = 1, uwp_phase_fifo_idle, uwp_phase_method, uwp_phase_flip, uwp_phase_local_task,
		uwp_phase_zcull, uwp_phase_fifo_wait, uwp_phase_ext_pause, uwp_phase_semaphore, uwp_phase_frame_limit,
		uwp_phase_submit, uwp_phase_query, uwp_phase_fence, uwp_phase_invalidate, uwp_phase_fifo_fetch, uwp_phase_fifo_spin,
		uwp_phase_count
	};

	extern atomic_t<u32> g_uwp_rsx_where;

	// The semaphore an NV406E acquire is waiting on right now (0 = none) and the value it waits for.
	extern atomic_t<u32> g_uwp_sema_wait_addr, g_uwp_sema_wait_value;

	class thread;

	// For the app's "no new frame" stall report: what the RSX thread is doing (phase, method, exact FIFO
	// position, CPU time since the last call, any semaphore wait). Called from one thread only.
	std::string uwp_rsx_stall_text(const thread* rsx);

	// FIFO work counters, written by the RSX thread only (logged per slow frame and per 10 s).
	struct uwp_fifo_counters
	{
		u64 commands = 0;  // commands run (methods and register writes)
		u64 refills = 0;   // command cache refills from guest memory
		u64 refill_bytes = 0;
		u64 retries = 0;   // cache lines re-read because the guest was writing them
		u64 spins = 0;     // 200-cycle waits for such lines
		u64 forced = 0;    // lines copied plainly after 50 us of failed re-reads
		u64 jumps = 0, calls = 0, empty = 0;
	};

	extern uwp_fifo_counters g_uwp_fifo;
	extern bool g_uwp_fifo_partial_line, g_uwp_fifo_retry_cap; // RSXFIFO.cpp
	extern thread_local bool g_uwp_on_rsx_thread;

	// Marks a phase on the RSX thread (no-op elsewhere), keeping the method it runs inside.
	struct uwp_rsx_scope
	{
		u32 prev = 0;
		bool active;

		uwp_rsx_scope(uwp_rsx_phase phase) : active(g_uwp_on_rsx_thread)
		{
			if (active)
			{
				prev = g_uwp_rsx_where.observe();
				g_uwp_rsx_where.release((u32{phase} << 16) | (prev & 0xffff));
			}
		}

		~uwp_rsx_scope()
		{
			if (active)
				g_uwp_rsx_where.release(prev);
		}
	};

	// Performance report for telemetry, made every 10 s on the RSX thread (RSXThread.cpp flip) and
	// handed to g_uwp_perf_sink (set by the app, null = off). Times are per frame unless noted.
	struct uwp_perf_stutter
	{
		f64 at_s = 0;          // system time of the frame's end
		u32 frame = 0;         // frame number, to find the sampler's text
		f64 frame_ms = 0, avg_ms = 0;
		u64 new_pipelines = 0; // pipelines (shaders) created during the frame
		f64 submit_ms = 0, present_wait_ms = 0, present_copy_ms = 0, rsx_idle_ms = 0, gpu_wait_ms = 0, rsx_cpu_ms = -1;
		f64 setup_ms = 0, vertex_ms = 0, texture_ms = 0, draw_ms = 0;
		u32 draws = 0, submits = 0;
		f64 mem_poll_ago_ms = -1;
		std::string rsx_phases; // the 1 ms sampler's top phases, e.g. "fifo idle 61%; semaphore 22%;"
	};

	struct uwp_perf_window
	{
		f64 secs = 0, fps = 0, frame_max_ms = 0;
		u32 frames = 0, frames_50ms = 0, frames_100ms = 0;
		f64 rsx_busy = 0, rsx_idle = 0; // percent; busy = CPU time of the RSX thread (includes spinning)
		f64 draws = 0, submits = 0, setup_ms = 0, vertex_ms = 0, texture_ms = 0, draw_ms = 0;
		f64 gpu_wait_ms = 0, submit_ms = 0, present_wait_ms = 0;
		u64 new_pipelines = 0;
		std::vector<std::pair<std::string, f64>> ppu; // thread name, % of one core; busiest first
		std::vector<f64> spu;                        // % of one core; busiest first
		std::vector<uwp_perf_stutter> stutters;      // longest frames (>= 100 ms and >= 2.5x the average), at most 5
	};

	extern void (*g_uwp_perf_sink)(const uwp_perf_window&);
	extern atomic_t<u32> g_uwp_rsx_runs; // RSX thread starts, so the flip statistics restart with each game
#endif
}
