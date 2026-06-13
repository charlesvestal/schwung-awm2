// perf_probe.cpp — measure the awm2 module's emulation throughput on-device.
// Boots the instance, then drains render_block as fast as possible (no realtime
// pacing) so the emulation thread free-runs at maximum speed. MAME prints its
// own "Average speed N%" at teardown; we also report consumer-side frames/sec.
//
// usage: perf_probe <dsp.so> <module_dir> <seconds>

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

#define SR 44100
#define BLK 128

typedef struct host_api_v1 {
	uint32_t api_version; int sample_rate; int frames_per_block;
	uint8_t *mapped_memory; int audio_out_offset; int audio_in_offset;
	void (*log)(const char *);
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

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s <dsp.so> <module_dir> <seconds>\n", argv[0]); return 2; }
	double secs = atof(argv[3]);
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	auto init = (plugin_api_v2_t *(*)(const host_api_v1_t *))dlsym(h, "move_plugin_init_v2");
	host_api_v1_t host = {}; host.api_version = 1; host.sample_rate = SR; host.frames_per_block = BLK; host.log = hlog;
	plugin_api_v2_t *api = init(&host);

	fprintf(stderr, "booting...\n");
	void *inst = api->create_instance(argv[2], nullptr);
	char err[512]; if (api->get_error(inst, err, sizeof(err)) > 0) fprintf(stderr, "error: %s\n", err);

	// play a sustained chord so the synth is actively voicing during the measurement
	uint8_t on1[3] = {0x90, 60, 100}, on2[3] = {0x90, 64, 100}, on3[3] = {0x90, 67, 100};
	api->on_midi(inst, on1, 3, 0); api->on_midi(inst, on2, 3, 0); api->on_midi(inst, on3, 3, 0);

	// Pace the consumer at real time (sleeping, not hot-spinning) so it doesn't
	// steal a core from the emulation thread. The producer is sub-realtime, so it
	// never gets blocked by backpressure -> MAME's "Average speed" reflects true
	// max throughput, with the one-time boot amortized over the run.
	using clk = std::chrono::steady_clock;
	int16_t buf[BLK * 2];
	long blocks = 0;
	auto t0 = clk::now();
	auto next = t0;
	const auto blk_dur = std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(double(BLK) / SR));
	auto tend = t0 + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(secs));
	while (clk::now() < tend) {
		api->render_block(inst, buf, BLK); blocks++;
		next += blk_dur;
		std::this_thread::sleep_until(next);
	}
	double wall = std::chrono::duration<double>(clk::now() - t0).count();
	fprintf(stderr, "consumer paced realtime: %.2f wall-s, %ld blocks\n", wall, blocks);

	api->destroy_instance(inst);   // MAME prints its own "Average speed" here
	dlclose(h);
	return 0;
}
