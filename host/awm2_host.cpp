// awm2_host.cpp — off-device verification harness for the Yamaha MU100 host.
//
// Boots MAME's mu100 machine headless (shared Mu100OsdBase), captures the SWP30
// stereo master mix, and renders a MIDI file (or the built-in --live schedule)
// to WAV from our own code. Output is bit-identical to MAME's own -wavwrite from
// the same swp30-fixed libs — see verify.sh / verify_live.sh.
//
// The same OSD base and MidiInjector drive the Move module (src/dsp/awm2_plugin.cpp);
// there the WAV writer becomes a ring buffer and the schedule becomes realtime MIDI.

#include "mu100_osd.h"
#include "frontend/mame/clifront.h"      // cli_frontend

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using awm2::MidiInjector;
using awm2::Mu100OsdBase;

namespace {

// ----------------------------------------------------------------------------
// Captured audio sink (unbounded — offline rendering)
// ----------------------------------------------------------------------------
struct AudioCapture {
	std::vector<int16_t> samples;   // interleaved, `channels` per frame
	int  channels = 0;
	int  rate     = 0;
	long frames   = 0;
};

class CaptureOsd : public Mu100OsdBase
{
public:
	CaptureOsd(osd_options &options, AudioCapture &cap, MidiInjector *inj)
		: Mu100OsdBase(options, inj), m_cap(cap) {}

protected:
	virtual void on_audio(const int16_t *buffer, int frames, int channels, int rate) override
	{
		m_cap.channels = channels;
		m_cap.rate     = rate;
		const size_t n = size_t(frames) * size_t(channels);
		m_cap.samples.insert(m_cap.samples.end(), buffer, buffer + n);
		m_cap.frames += frames;
	}

private:
	AudioCapture &m_cap;
};

// ----------------------------------------------------------------------------
// Canonical 16-bit PCM WAV writer (matches MAME's wav_open layout)
// ----------------------------------------------------------------------------
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
//   so -midiin1 must name a non-file sentinel (auto-added if absent).
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
		CaptureOsd osd(options, cap, inj_ptr);
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
