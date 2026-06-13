// awm2_plugin.cpp — Move Anything module glue for the Yamaha MU100 (H8 + SWP30).
//
// Boots MAME's mu100 machine once on a background thread (no -seconds_to_run,
// -nothrottle). The SWP30 stereo master mix flows through add_audio_to_recording
// into a ring buffer whose backpressure throttles the emulation to real time;
// render_block() drains it. on_midi()/set_param() inject MIDI into the H8 SCI in
// real time via the shared MidiInjector. Native rate is 44100 = the Move rate,
// so no resampling is needed.
//
// Shares the OSD/MIDI core with the off-device harness (../../host/mu100_osd.h).

#ifdef __linux__
#define _GNU_SOURCE 1
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#include "mu100_osd.h"
#include "frontend/mame/clifront.h"      // cli_frontend

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using awm2::MidiInjector;
using awm2::Mu100OsdBase;

// ============================================================================
// Move Anything plugin API v2 (from the host runtime; mirrors braids)
// ============================================================================
extern "C" {
#define MOVE_SAMPLE_RATE        44100
#define MOVE_FRAMES_PER_BLOCK   128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
	uint32_t api_version;
	int sample_rate;
	int frames_per_block;
	uint8_t *mapped_memory;
	int audio_out_offset;
	int audio_in_offset;
	void (*log)(const char *msg);
	int (*midi_send_internal)(const uint8_t *msg, int len);
	int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
	uint32_t api_version;
	void* (*create_instance)(const char *module_dir, const char *json_defaults);
	void (*destroy_instance)(void *instance);
	void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
	void (*set_param)(void *instance, const char *key, const char *val);
	int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
	int (*get_error)(void *instance, char *buf, int buf_len);
	void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

static const host_api_v1_t *g_host = nullptr;
static void hlog(const char *m) { if (g_host && g_host->log) g_host->log(m); }

namespace {

// ----------------------------------------------------------------------------
// SPSC stereo ring buffer (frames of interleaved s16 LR)
// ----------------------------------------------------------------------------
class StereoRing {
public:
	explicit StereoRing(size_t frames) : m_cap(frames + 1), m_buf(m_cap * 2, 0) {}

	// producer: copy up to n frames in; returns frames actually written
	size_t write(const int16_t *in, size_t n)
	{
		const size_t w = m_wr.load(std::memory_order_relaxed);
		const size_t r = m_rd.load(std::memory_order_acquire);
		size_t space = (r + m_cap - w - 1) % m_cap;
		size_t cnt = n < space ? n : space;
		for (size_t i = 0; i < cnt; i++) {
			size_t idx = ((w + i) % m_cap) * 2;
			m_buf[idx]     = in[i * 2];
			m_buf[idx + 1] = in[i * 2 + 1];
		}
		m_wr.store((w + cnt) % m_cap, std::memory_order_release);
		return cnt;
	}

	// consumer: read n frames; zero-pad any shortfall (underrun = silence)
	void read(int16_t *out, size_t n)
	{
		const size_t r = m_rd.load(std::memory_order_relaxed);
		const size_t w = m_wr.load(std::memory_order_acquire);
		size_t avail = (w + m_cap - r) % m_cap;
		size_t cnt = n < avail ? n : avail;
		for (size_t i = 0; i < cnt; i++) {
			size_t idx = ((r + i) % m_cap) * 2;
			out[i * 2]     = m_buf[idx];
			out[i * 2 + 1] = m_buf[idx + 1];
		}
		for (size_t i = cnt; i < n; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
		m_rd.store((r + cnt) % m_cap, std::memory_order_release);
	}

private:
	size_t               m_cap;
	std::vector<int16_t> m_buf;
	std::atomic<size_t>  m_wr{0};
	std::atomic<size_t>  m_rd{0};
};

// ----------------------------------------------------------------------------
// Streaming OSD: routes SWP30 audio into the ring with realtime backpressure
// ----------------------------------------------------------------------------
struct Awm2Instance;   // fwd

class StreamOsd : public Mu100OsdBase {
public:
	StreamOsd(osd_options &options, MidiInjector *inj, StereoRing &ring,
	          std::atomic<bool> &running, std::atomic<bool> &booted)
		: Mu100OsdBase(options, inj), m_ring(ring), m_running(running), m_booted(booted) {}

protected:
	virtual void on_booted() override { m_booted.store(true, std::memory_order_release); }
	virtual bool should_exit() override { return !m_running.load(std::memory_order_acquire); }

	virtual void on_audio(const int16_t *buffer, int frames, int channels, int /*rate*/) override
	{
		// MU100 is stereo; if channels != 2 take the first two.
		static thread_local std::vector<int16_t> tmp;
		const int16_t *lr = buffer;
		if (channels != 2) {
			tmp.resize(size_t(frames) * 2);
			for (int i = 0; i < frames; i++) {
				tmp[i * 2]     = buffer[i * channels];
				tmp[i * 2 + 1] = channels > 1 ? buffer[i * channels + 1] : buffer[i * channels];
			}
			lr = tmp.data();
		}
		if (getenv("AWM2_DEBUG")) {
			static int calls = 0; static long tot = 0; tot += frames;
			if (calls++ < 5 || (calls % 200) == 0)
				fprintf(stderr, "[awm2] on_audio #%d frames=%d ch=%d total=%ld\n", calls, frames, channels, tot);
		}
		// Backpressure: block until the consumer drains, unless tearing down.
		size_t off = 0, total = size_t(frames);
		while (off < total) {
			if (!m_running.load(std::memory_order_acquire)) return;   // drop on teardown
			off += m_ring.write(lr + off * 2, total - off);
			if (off < total)
				std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}

private:
	StereoRing        &m_ring;
	std::atomic<bool> &m_running;
	std::atomic<bool> &m_booted;
};

// ----------------------------------------------------------------------------
// Parameter store (echoed by get_param; mapped to MIDI by set_param)
// ----------------------------------------------------------------------------
struct Params {
	int   program = 0, bank_msb = 0, bank_lsb = 0;
	float reverb = 0.31f, chorus = 0.0f, variation = 0.0f;
	float cutoff = 0.5f, resonance = 0.5f, attack = 0.5f, release = 0.5f;
	float volume = 0.8f;
};

// ----------------------------------------------------------------------------
// Instance
// ----------------------------------------------------------------------------
struct Awm2Instance {
	std::string       module_dir;
	std::string       error;

	// Ring depth sets output latency: with -nothrottle the emulation outruns the
	// consumer and backpressure pins the ring full, so steady-state latency ≈
	// ring capacity. MAME delivers audio in ~20 ms (882-frame) chunks, so this
	// must hold a few chunks. ~93 ms balances latency vs underrun safety on the
	// CM4 (where the producer is only modestly faster than real time); revisit
	// once the step-4 perf work (SCHED_FIFO, H8 SLEEP fast-forward) lands.
	static constexpr int RING_FRAMES = 4096;

	MidiInjector      injector;
	StereoRing        ring{RING_FRAMES};
	std::atomic<bool> running{true};
	std::atomic<bool> booted{false};
	std::thread       thread;

	Params            params;

	// Pin the emulation thread to a dedicated core and request SCHED_FIFO. On the
	// CM4 this lifts uncontended throughput markedly (~56%->~69% in tests).
	// Affinity needs no privilege; RT needs an rtprio limit (best-effort, ignored
	// if denied). Overridable via AWM2_CPU / AWM2_RTPRIO.
	static void pin_and_rt_self()
	{
#ifdef __linux__
		long nc = sysconf(_SC_NPROCESSORS_ONLN);
		// Pin to a COMPUTE core, not the last one: on the Move (/etc/init.d/move)
		// the audio DMA/SPI IRQs are pinned to the top core (3), so we'd fight
		// them there. Use the second-from-top compute core by default.
		int core = (nc >= 2) ? int(nc - 2) : 0;
		if (const char *e = getenv("AWM2_CPU")) core = atoi(e);
		cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core, &cs);
		pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
		int prio = 70; if (const char *e = getenv("AWM2_RTPRIO")) prio = atoi(e);
		if (prio > 0) {
			sched_param sp{}; sp.sched_priority = prio;
			hlog(pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0
				? "awm2: emulation thread pinned + SCHED_FIFO"
				: "awm2: emulation thread pinned (RT denied; normal prio)");
		}
#endif
	}

	void run_mame()
	{
		pin_and_rt_self();
		// Flush denormals to zero (FTZ/DAZ). MAME 0.288 mixes audio in float and
		// reverb/decay tails generate denormals, which are pathologically slow on
		// ARM — a likely cause of the emulation running far below realtime.
#if defined(__aarch64__)
		if (!getenv("AWM2_NOFTZ")) {
			uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
			fpcr |= (1u << 24); /* FZ */ __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
		}
#endif
		setenv("SDL_VIDEODRIVER", "dummy", 0);
		// Search both the module's roms/ subdir (install convention) and the
		// module dir itself for the user-supplied romset (mu100/swp30/mulcd .zip).
		const std::string rompath = module_dir + "/roms;" + module_dir;
		// Prefer the screenless mu100b (identical XG engine, no LCD/SVG rendering
		// = less CPU) when its romset is present; otherwise the standard mu100.
		auto have = [&](const char *z) {
			for (const std::string &d : { module_dir + "/roms/", module_dir + "/" }) {
				FILE *f = fopen((d + z).c_str(), "rb"); if (f) { fclose(f); return true; }
			} return false;
		};
		const char *machine = have("mu100b.zip") ? "mu100b" : "mu100";
		std::vector<std::string> args = {
			"awm2", machine,
			"-rompath", rompath,
			"-video", "none", "-sound", "none",
			"-nothrottle",
			"-samplerate", std::to_string(MOVE_SAMPLE_RATE),
			"-midiin1", "awm2live",
		};
		int rc = 0;
		{
			osd_options options;
			StreamOsd osd(options, &injector, ring, running, booted);
			osd.register_options();
			cli_frontend frontend(options, osd);
			rc = frontend.execute(args);
		}
		if (!booted.load() && error.empty())
			error = "mu100 failed to boot (rc=" + std::to_string(rc) +
			        "); check MU100 ROMs (mu100.zip, swp30.zip, mulcd.zip) in " + module_dir;
		booted.store(true, std::memory_order_release);   // unblock create() on failure
	}
};

// --- MIDI helpers ---
static inline uint8_t f2cc(float v) {
	int c = int(v * 127.0f + 0.5f);
	return uint8_t(c < 0 ? 0 : (c > 127 ? 127 : c));
}
static void send_cc(Awm2Instance *in, uint8_t cc, uint8_t val, uint8_t ch = 0) {
	uint8_t m[3] = { uint8_t(0xB0 | (ch & 0x0f)), cc, val };
	in->injector.push_realtime(m, 3);
}
static void send_program(Awm2Instance *in, uint8_t ch = 0) {
	send_cc(in, 0,  uint8_t(in->params.bank_msb), ch);   // bank select MSB
	send_cc(in, 32, uint8_t(in->params.bank_lsb), ch);   // bank select LSB
	uint8_t pc[2] = { uint8_t(0xC0 | (ch & 0x0f)), uint8_t(in->params.program) };
	in->injector.push_realtime(pc, 2);
}

// --- tiny JSON number extractor for json_defaults ("key":num) ---
static bool json_num(const char *json, const char *key, double *out) {
	if (!json) return false;
	char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(json, pat);
	if (!p) return false;
	p = strchr(p + strlen(pat), ':');
	if (!p) return false;
	*out = atof(p + 1);
	return true;
}

// ============================================================================
// API implementation
// ============================================================================
static void apply_param(Awm2Instance *in, const std::string &key, const std::string &val);

static void *v2_create_instance(const char *module_dir, const char *json_defaults)
{
	Awm2Instance *in = new Awm2Instance();
	in->module_dir = module_dir ? module_dir : ".";

	// Seed params from json_defaults (if provided).
	double d;
	auto seti = [&](const char *k, int &f){ if (json_num(json_defaults, k, &d)) f = int(d); };
	auto setf = [&](const char *k, float &f){ if (json_num(json_defaults, k, &d)) f = float(d); };
	seti("program", in->params.program);
	seti("bank_msb", in->params.bank_msb);
	seti("bank_lsb", in->params.bank_lsb);
	setf("reverb_send", in->params.reverb);
	setf("chorus_send", in->params.chorus);
	setf("variation_send", in->params.variation);
	setf("cutoff", in->params.cutoff);
	setf("resonance", in->params.resonance);
	setf("attack", in->params.attack);
	setf("release", in->params.release);
	setf("volume", in->params.volume);

	in->thread = std::thread([in]{ in->run_mame(); });

	// Wait for boot (machine start = ROM load); bounded so a missing-ROM
	// failure still returns a (silent) instance whose get_error explains it.
	for (int i = 0; i < 1500 && !in->booted.load(std::memory_order_acquire); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	if (!in->error.empty()) hlog(in->error.c_str());
	else hlog("awm2: mu100 booted");
	return in;
}

static void v2_destroy_instance(void *instance)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (!in) return;
	in->running.store(false, std::memory_order_release);   // stops machine + unblocks producer
	if (in->thread.joinable()) in->thread.join();
	delete in;
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int /*source*/)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (in && msg && len > 0) in->injector.push_realtime(msg, len);
}

static void v2_set_param(void *instance, const char *key, const char *val)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (in && key && val) apply_param(in, key, val);
}

static void apply_param(Awm2Instance *in, const std::string &key, const std::string &val)
{
	const float fv = float(atof(val.c_str()));
	const int   iv = atoi(val.c_str());
	Params &p = in->params;

	if      (key == "program")        { p.program  = iv; send_program(in); }
	else if (key == "bank_msb")       { p.bank_msb = iv; send_program(in); }
	else if (key == "bank_lsb")       { p.bank_lsb = iv; send_program(in); }
	else if (key == "reverb_send")    { p.reverb    = fv; send_cc(in, 91, f2cc(fv)); }
	else if (key == "chorus_send")    { p.chorus    = fv; send_cc(in, 93, f2cc(fv)); }
	else if (key == "variation_send") { p.variation = fv; send_cc(in, 94, f2cc(fv)); }
	else if (key == "cutoff")         { p.cutoff    = fv; send_cc(in, 74, f2cc(fv)); }
	else if (key == "resonance")      { p.resonance = fv; send_cc(in, 71, f2cc(fv)); }
	else if (key == "attack")         { p.attack    = fv; send_cc(in, 73, f2cc(fv)); }
	else if (key == "release")        { p.release   = fv; send_cc(in, 72, f2cc(fv)); }
	else if (key == "volume")         { p.volume    = fv; send_cc(in,  7, f2cc(fv)); }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (!in || !key || !buf || buf_len <= 0) return 0;
	const Params &p = in->params;
	int n = 0;
	if      (!strcmp(key, "program"))        n = snprintf(buf, buf_len, "%d", p.program);
	else if (!strcmp(key, "bank_msb"))       n = snprintf(buf, buf_len, "%d", p.bank_msb);
	else if (!strcmp(key, "bank_lsb"))       n = snprintf(buf, buf_len, "%d", p.bank_lsb);
	else if (!strcmp(key, "reverb_send"))    n = snprintf(buf, buf_len, "%.4f", p.reverb);
	else if (!strcmp(key, "chorus_send"))    n = snprintf(buf, buf_len, "%.4f", p.chorus);
	else if (!strcmp(key, "variation_send")) n = snprintf(buf, buf_len, "%.4f", p.variation);
	else if (!strcmp(key, "cutoff"))         n = snprintf(buf, buf_len, "%.4f", p.cutoff);
	else if (!strcmp(key, "resonance"))      n = snprintf(buf, buf_len, "%.4f", p.resonance);
	else if (!strcmp(key, "attack"))         n = snprintf(buf, buf_len, "%.4f", p.attack);
	else if (!strcmp(key, "release"))        n = snprintf(buf, buf_len, "%.4f", p.release);
	else if (!strcmp(key, "volume"))         n = snprintf(buf, buf_len, "%.4f", p.volume);
	else return 0;
	return n < 0 ? 0 : (n < buf_len ? n : buf_len - 1);
}

static int v2_get_error(void *instance, char *buf, int buf_len)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (!in || !buf || buf_len <= 0) return 0;
	if (in->error.empty()) { buf[0] = 0; return 0; }
	int n = snprintf(buf, buf_len, "%s", in->error.c_str());
	return n < 0 ? 0 : (n < buf_len ? n : buf_len - 1);
}

static void v2_render_block(void *instance, int16_t *out, int frames)
{
	Awm2Instance *in = static_cast<Awm2Instance *>(instance);
	if (!in || !out || frames <= 0) {
		if (out && frames > 0) std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
		return;
	}
	in->ring.read(out, size_t(frames));
	if (getenv("AWM2_DEBUG")) {
		static int calls = 0; long e = 0; for (int i = 0; i < frames*2; i++) e += out[i] < 0 ? -out[i] : out[i];
		if (calls++ < 3 || (calls % 400) == 0)
			fprintf(stderr, "[awm2] render #%d sumabs=%ld\n", calls, e);
	}
}

} // namespace

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
	g_host = host;
	static plugin_api_v2_t api = {
		MOVE_PLUGIN_API_VERSION_2,
		v2_create_instance,
		v2_destroy_instance,
		v2_on_midi,
		v2_set_param,
		v2_get_param,
		v2_get_error,
		v2_render_block,
	};
	return &api;
}
