// test_plugin.cpp — minimal Move-runtime stand-in for off-device validation of
// the awm2 module .dylib/.so. dlopen's the plugin, boots an instance, renders
// past the firmware boot window, injects a note via on_midi(), keeps rendering,
// writes a WAV, and checks the note actually sounds.
//
// usage: test_plugin <dsp.dylib> <romdir> <out.wav>

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

#define SR 44100
#define BLK 128

typedef struct host_api_v1 {
	uint32_t api_version; int sample_rate; int frames_per_block;
	uint8_t *mapped_memory; int audio_out_offset; int audio_in_offset;
	void (*log)(const char *msg);
	int (*midi_send_internal)(const uint8_t *, int);
	int (*midi_send_external)(const uint8_t *, int);
} host_api_v1_t;

typedef struct plugin_api_v2 {
	uint32_t api_version;
	void* (*create_instance)(const char *, const char *);
	void (*destroy_instance)(void *);
	void (*on_midi)(void *, const uint8_t *, int, int);
	void (*set_param)(void *, const char *, const char *);
	int (*get_param)(void *, const char *, char *, int);
	int (*get_error)(void *, char *, int);
	void (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

static void hlog(const char *m) { fprintf(stderr, "[host] %s\n", m); }

// Pace render_block calls at real time: the plugin throttles the emulation to
// the consumer via ring backpressure, so consuming at real time keeps emulated
// time and audio coupled (and makes injected-MIDI timing meaningful).
static void render(plugin_api_v2_t *api, void *inst, std::vector<int16_t> &acc, double secs) {
	using namespace std::chrono;
	int blocks = int(secs * SR / BLK);
	int16_t buf[BLK * 2];
	auto next = steady_clock::now();
	const auto blk_dur = duration_cast<steady_clock::duration>(duration<double>(double(BLK) / SR));
	for (int b = 0; b < blocks; b++) {
		api->render_block(inst, buf, BLK);
		acc.insert(acc.end(), buf, buf + BLK * 2);
		next += blk_dur;
		std::this_thread::sleep_until(next);
	}
}

static double rms_window(const std::vector<int16_t> &a, double t0, double t1) {
	size_t i0 = size_t(t0 * SR) * 2, i1 = size_t(t1 * SR) * 2;
	if (i1 > a.size()) i1 = a.size();
	double s = 0; size_t n = 0;
	for (size_t i = i0; i + 1 < i1; i += 2) { double m = (a[i] + a[i+1]) * 0.5; s += m*m; n++; }
	return n ? std::sqrt(s / n) : 0;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s <dsp> <romdir> <out.wav>\n", argv[0]); return 2; }
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	auto init = (plugin_api_v2_t *(*)(const host_api_v1_t *))dlsym(h, "move_plugin_init_v2");
	if (!init) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

	host_api_v1_t host = {};
	host.api_version = 1; host.sample_rate = SR; host.frames_per_block = BLK; host.log = hlog;
	plugin_api_v2_t *api = init(&host);
	fprintf(stderr, "plugin api_version=%u\n", api->api_version);

	fprintf(stderr, "creating instance (boots mu100; ~seconds)...\n");
	void *inst = api->create_instance(argv[2], nullptr);
	char err[512]; if (api->get_error(inst, err, sizeof(err)) > 0) fprintf(stderr, "get_error: %s\n", err);

	// Param round-trip (also exercises the param->MIDI emit path).
	api->set_param(inst, "program", "12");
	api->set_param(inst, "reverb_send", "0.5");
	char pv[64] = {0}; api->get_param(inst, "program", pv, sizeof(pv));
	int param_ok = (strcmp(pv, "12") == 0);
	fprintf(stderr, "param round-trip: program=%s (%s)\n", pv, param_ok ? "OK" : "MISMATCH");

	std::vector<int16_t> acc;
	render(api, inst, acc, 6.0);                 // pass the ~4.5s boot + click settle
	double t_note = acc.size() / 2.0 / SR;
	uint8_t on[3]  = { 0x90, 60, 100 };          // C4
	api->on_midi(inst, on, 3, 0);
	render(api, inst, acc, 1.0);
	uint8_t off[3] = { 0x80, 60, 0 };
	api->on_midi(inst, off, 3, 0);
	render(api, inst, acc, 1.0);

	// WAV out
	FILE *f = fopen(argv[3], "wb");
	uint32_t data = uint32_t(acc.size()) * 2, br = SR * 2 * 2; uint16_t ba = 4;
	auto u32 = [&](uint32_t v){ uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; fwrite(b,1,4,f); };
	auto u16 = [&](uint16_t v){ uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; fwrite(b,1,2,f); };
	fwrite("RIFF",1,4,f); u32(36+data); fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f);
	u32(16); u16(1); u16(2); u32(SR); u32(br); u16(ba); u16(16);
	fwrite("data",1,4,f); u32(data); fwrite(acc.data(),2,acc.size(),f); fclose(f);

	// Quiet window: after the boot click (~4.5s) but before the note.
	double pre = rms_window(acc, t_note - 0.9, t_note - 0.2);
	// Loud: scan 50 ms windows after injection (covers the ring latency) for the peak.
	double note = 0;
	for (double t = t_note; t < t_note + 0.9; t += 0.05) {
		double r = rms_window(acc, t, t + 0.05);
		if (r > note) note = r;
	}
	fprintf(stderr, "note injected at %.2fs | pre-RMS=%.1f peak-note-RMS=%.1f | wrote %s\n",
		t_note, pre, note, argv[3]);

	int ok = (note > 50.0 && note > pre * 4.0) && param_ok;
	fprintf(stderr, "RESULT: %s\n", ok ? "PASS (note audible; quiet before; params ok)" : "FAIL");

	api->destroy_instance(inst);
	dlclose(h);
	return ok ? 0 : 1;
}
