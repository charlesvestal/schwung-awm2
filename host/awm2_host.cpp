// awm2_host.cpp — headless MAME host for the Yamaha MU100 (H8 + SWP30).
//
// This is the integration core for the schwung-awm2 module. It boots the MAME
// `mu100` machine through a custom osd_common_t subclass (no SDL UI), captures
// the SWP30 stereo master mix via the add_audio_to_recording OSD hook, and
// writes it to a WAV file from our own code.
//
// Verification target (step 1): rendering a MIDI file this way must be
// bit-identical to MAME's own -wavwrite output from the SAME (swp30-fixed)
// static libs. That proves our audio tap is correct and is the foundation for
// the live Move audio bridge (which replaces the WAV writer with a ring buffer).
//
// Pattern mirrors ../ensoniq-sd1-source/Source/PluginProcessor.cpp
// (VstOsdInterface : public osd_common_t + cli_frontend::execute).

#include "emu.h"
#include "emuopts.h"
#include "osdepend.h"
#include "render.h"                       // render_manager, render_target
#include "interface/midiport.h"           // osd::midi_input_port
#include "modules/lib/osdobj_common.h"   // osd_common_t, osd_options
#include "frontend/mame/clifront.h"      // cli_frontend

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// Captured audio sink
// ----------------------------------------------------------------------------
namespace {

struct AudioCapture {
	std::vector<int16_t> samples;   // interleaved, `channels` per frame
	int  channels = 0;
	int  rate     = 0;
	long frames   = 0;
};

// ----------------------------------------------------------------------------
// Live MIDI injector — feeds raw MIDI bytes into the H8 SCI via the OSD MIDI
// input port, replacing the -midiin1 SMF image device.
//
// Two feed modes share one byte stream:
//   * scheduled  — a sorted (emu-time, byte) list released when machine time
//                  reaches each entry. Deterministic, used for off-device
//                  verification (bit-identical across runs).
//   * realtime   — a single-producer/single-consumer ring pushed from another
//                  thread, released immediately. This is the path the Move
//                  module will use; unused in offline rendering.
// Both are drained by the midiin device's 1500 Hz poll and clocked onto the
// SCI rxd line at 31250 baud by MAME's existing serial machinery.
// ----------------------------------------------------------------------------
struct MidiInjector {
	struct Ev { double t; uint8_t b; };

	running_machine *machine = nullptr;

	// scheduled (offline, deterministic)
	std::vector<Ev> sched;     // sorted ascending by t
	size_t          sched_idx = 0;

	// realtime ring (module use)
	static constexpr uint32_t RING = 1u << 14;   // 16384, power of two
	uint8_t               ring[RING];
	std::atomic<uint32_t> wr{0};
	std::atomic<uint32_t> rd{0};

	double now() const { return machine ? machine->time().as_double() : 0.0; }

	// --- producer (control thread) ---
	void push_realtime(uint8_t b)
	{
		const uint32_t w = wr.load(std::memory_order_relaxed);
		const uint32_t n = (w + 1) & (RING - 1);
		if (n != rd.load(std::memory_order_acquire)) {
			ring[w] = b;
			wr.store(n, std::memory_order_release);
		}
	}

	// --- consumer (emu thread, via the OSD port) ---
	bool ready()
	{
		if (sched_idx < sched.size() && now() >= sched[sched_idx].t)
			return true;
		return rd.load(std::memory_order_acquire) != wr.load(std::memory_order_acquire);
	}

	int next(uint8_t *out)
	{
		if (sched_idx < sched.size() && now() >= sched[sched_idx].t) {
			*out = sched[sched_idx++].b;
			return 1;
		}
		const uint32_t r = rd.load(std::memory_order_relaxed);
		if (r != wr.load(std::memory_order_acquire)) {
			*out = ring[r];
			rd.store((r + 1) & (RING - 1), std::memory_order_release);
			return 1;
		}
		return 0;
	}

	void schedule_note(double t_on, double t_off, uint8_t note, uint8_t vel, uint8_t chan = 0)
	{
		sched.push_back({ t_on,  uint8_t(0x90 | (chan & 0x0f)) });
		sched.push_back({ t_on,  note });
		sched.push_back({ t_on,  vel  });
		sched.push_back({ t_off, uint8_t(0x80 | (chan & 0x0f)) });
		sched.push_back({ t_off, note });
		sched.push_back({ t_off, uint8_t(0) });
	}
};

// MAME OSD MIDI input port backed by the injector.
class LiveMidiPort : public osd::midi_input_port
{
public:
	explicit LiveMidiPort(MidiInjector &inj) : m_inj(inj) {}
	virtual bool poll() override            { return m_inj.ready(); }
	virtual int  read(uint8_t *out) override { return m_inj.next(out); }
private:
	MidiInjector &m_inj;
};

} // namespace

// ----------------------------------------------------------------------------
// Minimal headless OSD: just enough of osd_interface to boot mu100 and tap audio
// ----------------------------------------------------------------------------
class Mu100Osd : public osd_common_t
{
public:
	Mu100Osd(osd_options &options, AudioCapture &cap, MidiInjector *inj)
		: osd_common_t(options), m_cap(cap), m_inj(inj) {}

	// --- lifecycle -----------------------------------------------------------
	virtual void init(running_machine &machine) override
	{
		// REQUIRED: initialises MAME's core sound/input/font modules.
		osd_common_t::init(machine);
		m_machine = &machine;

		// Select & initialise the OSD provider modules (font/render/sound/input/
		// midi/...). The base init() does NOT do this — the concrete OSD must.
		// With -video none/-sound none these resolve to the "none" providers.
		init_subsystems();

		// The UI/render path dereferences a render target (ui_aspect); with
		// -video none nothing allocates one, so we must (mirrors SD-1).
		m_target = machine.render().target_alloc();
		m_target->set_bounds(640, 480);
		m_target->set_view(0);
	}

	virtual void osd_exit() override
	{
		if (m_machine != nullptr && m_target != nullptr) {
			m_machine->render().target_free(m_target);
			m_target = nullptr;
		}
		osd_common_t::osd_exit();
	}

	virtual void update(bool /*skip_redraw*/) override
	{
		// -seconds_to_run drives termination; nothing to do per-frame.
	}

	// --- input / focus (pure in osd_common_t, must stub) ---------------------
	virtual void input_update(bool /*relative_reset*/) override {}
	virtual void check_osd_inputs() override {}
	virtual void process_events() override {}
	virtual bool has_focus() const override { return true; }

	// --- sound ---------------------------------------------------------------
	// Force the master mix to be computed (m_record_buffer populated) regardless
	// of the chosen OSD sound module, exactly like the SD-1 reference.
	virtual bool no_sound() override { return false; }

	// The SWP30 stereo master mix arrives here every sound update, as
	// `samples_this_frame` frames of `outputs_count()` interleaved s16 channels.
	// This is the SAME buffer MAME's -wavwrite path consumes in the same call.
	virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override
	{
		if (m_cap.channels == 0 && m_machine != nullptr) {
			m_cap.channels = int(m_machine->sound().outputs_count());
			m_cap.rate     = m_machine->sample_rate();
		}
		if (m_cap.channels <= 0 || samples_this_frame <= 0)
			return;
		const size_t n = size_t(samples_this_frame) * size_t(m_cap.channels);
		m_cap.samples.insert(m_cap.samples.end(), buffer, buffer + n);
		m_cap.frames += samples_this_frame;
	}

	// --- live MIDI -----------------------------------------------------------
	// Called by the midiin image device's call_load() when -midiin1 names
	// something that isn't a loadable SMF. We hand back a port backed by our
	// injector; the device then polls it at 1500 Hz and clocks bytes onto the
	// H8 SCI rxd line. Falls back to the base (no-op) port when not in live mode.
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override
	{
		if (m_inj) {
			m_inj->machine = m_machine;
			return std::make_unique<LiveMidiPort>(*m_inj);
		}
		return osd_common_t::create_midi_input(name);
	}

private:
	running_machine *m_machine = nullptr;
	render_target   *m_target  = nullptr;
	AudioCapture    &m_cap;
	MidiInjector    *m_inj     = nullptr;
};

// ----------------------------------------------------------------------------
// Canonical 16-bit PCM WAV writer (matches MAME's wav_open layout)
// ----------------------------------------------------------------------------
namespace {

static void put_u32(FILE *f, uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) }; fwrite(b,1,4,f); }
static void put_u16(FILE *f, uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v>>8) }; fwrite(b,1,2,f); }

static bool write_wav(const char *path, const AudioCapture &cap)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "awm2_host: cannot open %s for writing\n", path); return false; }

	const uint32_t bytes_per_sample = 2;
	const uint32_t data_bytes = uint32_t(cap.samples.size()) * bytes_per_sample;
	const uint32_t byte_rate  = uint32_t(cap.rate) * cap.channels * bytes_per_sample;
	const uint16_t block_align = uint16_t(cap.channels * bytes_per_sample);

	fwrite("RIFF", 1, 4, f);
	put_u32(f, 36 + data_bytes);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	put_u32(f, 16);                 // PCM fmt chunk size
	put_u16(f, 1);                  // PCM
	put_u16(f, uint16_t(cap.channels));
	put_u32(f, uint32_t(cap.rate));
	put_u32(f, byte_rate);
	put_u16(f, block_align);
	put_u16(f, 16);                 // bits per sample
	fwrite("data", 1, 4, f);
	put_u32(f, data_bytes);
	if (!cap.samples.empty())
		fwrite(cap.samples.data(), bytes_per_sample, cap.samples.size(), f);
	fclose(f);
	return true;
}

} // namespace

// ----------------------------------------------------------------------------
// main: awm2_host [--live] <out.wav> -- <mame args...>
//   Everything after `--` is passed verbatim to the MAME frontend, e.g.:
//     awm2_host out.wav -- mu100 -rompath roms -video none -sound none \
//         -seconds_to_run 24 -nothrottle -midiin1 song.mid -samplerate 48000
//
//   With --live, MIDI is injected from our own code (no SMF). We build a
//   deterministic demo schedule and feed it through the OSD MIDI input port,
//   so -midiin1 must name a non-file sentinel (auto-added if absent). This
//   exercises the same live path the Move module will drive in real time.
// ----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	// Headless: never touch the display. Force SDL's dummy video driver so the
	// linked SDL3 OSD can't open (or fullscreen-grab) a window. Audio output is
	// unaffected. (0 = don't override if the caller already set it.)
	setenv("SDL_VIDEODRIVER", "dummy", 0);

	const char *out_wav = nullptr;
	bool live = false;
	std::vector<std::string> mame_args;
	// MAME's command-line parser expects args[0] to be the program name and
	// begins parsing at args[1]; supply it so the system name isn't consumed.
	mame_args.push_back("awm2_host");
	bool after_sep = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (!after_sep && a == "--") { after_sep = true; continue; }
		if (!after_sep && a == "--live") { live = true; continue; }
		if (!after_sep && !out_wav) { out_wav = argv[i]; continue; }
		if (after_sep) mame_args.push_back(a);
	}
	if (!out_wav || mame_args.size() <= 1) {
		fprintf(stderr,
			"usage: %s [--live] <out.wav> -- mu100 -rompath <dir> -video none -sound none \\\n"
			"           -seconds_to_run <N> -nothrottle [-midiin1 <file.mid>] -samplerate <R>\n",
			argv[0]);
		return 2;
	}

	// Live mode: build a deterministic demo schedule (an ascending scale after
	// the firmware boot window) and ensure a non-file -midiin1 sentinel so the
	// midiin device routes to our OSD port instead of parsing an SMF.
	MidiInjector injector;
	MidiInjector *inj_ptr = nullptr;
	if (live) {
		const uint8_t scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };  // C major
		double t = 5.0;  // start after the ~4.5s firmware boot
		for (uint8_t n : scale) { injector.schedule_note(t, t + 0.4, n, 100); t += 0.5; }
		bool has_midiin1 = false;
		for (auto &a : mame_args) if (a == "-midiin1") { has_midiin1 = true; break; }
		if (!has_midiin1) { mame_args.push_back("-midiin1"); mame_args.push_back("awm2live"); }
		inj_ptr = &injector;
		fprintf(stderr, "awm2_host: live MIDI mode (%zu scheduled bytes)\n", injector.sched.size());
	}

	AudioCapture cap;

	int res = 0;
	{
		osd_options options;
		Mu100Osd osd(options, cap, inj_ptr);
		osd.register_options();
		cli_frontend frontend(options, osd);
		res = frontend.execute(mame_args);
	}

	fprintf(stderr, "awm2_host: captured %ld frames @ %d Hz x %d ch (%zu samples)\n",
		cap.frames, cap.rate, cap.channels, cap.samples.size());

	if (cap.channels <= 0 || cap.samples.empty()) {
		fprintf(stderr, "awm2_host: no audio captured (frontend rc=%d)\n", res);
		return res ? res : 1;
	}

	if (!write_wav(out_wav, cap))
		return 1;

	fprintf(stderr, "awm2_host: wrote %s\n", out_wav);
	return res;
}
