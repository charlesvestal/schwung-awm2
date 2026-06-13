// mu100_osd.h — shared headless MAME host pieces for the Yamaha MU100.
//
// Used by both the off-device verification harness (awm2_host.cpp) and the Move
// module (src/dsp/awm2_plugin.cpp). Provides:
//   * MidiInjector  — feeds raw MIDI bytes into the H8 SCI (scheduled + realtime)
//   * LiveMidiPort  — osd::midi_input_port backed by the injector
//   * Mu100OsdBase  — osd_common_t subclass that boots mu100 with no SDL UI and
//                     routes the SWP30 master mix to an on_audio() hook.
//
// Subclasses implement on_audio(); optionally override should_exit()/on_booted().

#pragma once

#include "emu.h"
#include "emuopts.h"
#include "osdepend.h"
#include "render.h"                       // render_manager, render_target
#include "interface/midiport.h"           // osd::midi_input_port
#include "modules/lib/osdobj_common.h"   // osd_common_t, osd_options

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace awm2 {

// ----------------------------------------------------------------------------
// Live MIDI injector — replaces the -midiin1 SMF image device.
//
// Two feed modes share one byte stream:
//   * scheduled  — a sorted (emu-time, byte) list released when machine time
//                  reaches each entry. Deterministic; used for off-device
//                  verification (bit-identical across runs).
//   * realtime   — a single-producer/single-consumer ring pushed from another
//                  thread, released immediately. This is the module's path.
// Both are drained by the midiin device's 1500 Hz poll and clocked onto the SCI
// rxd line at 31250 baud by MAME's existing serial machinery.
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
	void push_realtime(const uint8_t *msg, int len)
	{
		for (int i = 0; i < len; i++) {
			const uint32_t w = wr.load(std::memory_order_relaxed);
			const uint32_t n = (w + 1) & (RING - 1);
			if (n == rd.load(std::memory_order_acquire))
				return;   // ring full, drop
			ring[w] = msg[i];
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
	virtual bool poll() override             { return m_inj.ready(); }
	virtual int  read(uint8_t *out) override { return m_inj.next(out); }
private:
	MidiInjector &m_inj;
};

// ----------------------------------------------------------------------------
// Headless OSD base: boots mu100 with no SDL UI and forwards the SWP30 stereo
// master mix to on_audio(). Subclass for capture (file) or streaming (module).
//
// The vendored MAME tree's register_options() is patched to register only a
// minimal SDL-free provider set (incl. a headless monitor), so this links with
// no libSDL3/OpenGL/bgfx/CoreAudio — a self-contained dsp.so like the other
// Move modules. See patches/0002-headless-sdl-free-osd.patch.
// ----------------------------------------------------------------------------
class Mu100OsdBase : public osd_common_t
{
public:
	Mu100OsdBase(osd_options &options, MidiInjector *inj)
		: osd_common_t(options), m_inj(inj) {}

	// --- lifecycle ---
	virtual void init(running_machine &machine) override
	{
		osd_common_t::init(machine);     // core sound/input/font init
		m_machine = &machine;
		init_subsystems();               // select provider modules (the base init does not)

		// The UI/render path dereferences a render target (render_manager::
		// ui_aspect) even with -video none, so allocate one (nothing is shown).
		m_target = machine.render().target_alloc();
		m_target->set_bounds(640, 480);
		m_target->set_view(0);

		on_booted();
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
		if (m_machine != nullptr && should_exit())
			m_machine->schedule_exit();
	}

	// --- input / focus (pure in osd_common_t, must stub) ---
	virtual void input_update(bool /*relative_reset*/) override {}
	virtual void check_osd_inputs() override {}
	virtual void process_events() override {}
	virtual bool has_focus() const override { return true; }

	// Force the master mix to be computed regardless of the (none) OSD sound sink.
	virtual bool no_sound() override { return false; }

	// SWP30 stereo master mix: `frames` frames of outputs_count() interleaved
	// s16 channels — the same buffer MAME's -wavwrite path consumes.
	virtual void add_audio_to_recording(const int16_t *buffer, int frames) override
	{
		if (m_channels == 0 && m_machine != nullptr) {
			m_channels = int(m_machine->sound().outputs_count());
			m_rate     = m_machine->sample_rate();
		}
		if (m_channels <= 0 || frames <= 0)
			return;
		on_audio(buffer, frames, m_channels, m_rate);
	}

	// Route the midiin device to our injector-backed port (live MIDI).
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override
	{
		if (m_inj) {
			m_inj->machine = m_machine;
			return std::make_unique<LiveMidiPort>(*m_inj);
		}
		return osd_common_t::create_midi_input(name);
	}

protected:
	// Subclass hooks.
	virtual void on_audio(const int16_t *buffer, int frames, int channels, int rate) = 0;
	virtual bool should_exit() { return false; }   // streaming: return on teardown
	virtual void on_booted() {}                     // streaming: signal readiness

	running_machine *machine() const { return m_machine; }

	running_machine *m_machine = nullptr;
	render_target   *m_target  = nullptr;
	MidiInjector    *m_inj     = nullptr;
	int              m_channels = 0;
	int              m_rate     = 0;
};

} // namespace awm2
