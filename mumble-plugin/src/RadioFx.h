// Distance-driven "walkie-talkie" effect for a single incoming voice.
//
// The effect morphs continuously: as `wet` goes 0 -> 1 the band-pass closes in from full range to
// ~350 Hz..2.8 kHz, a mid-range "honk" fades in, the voice gets saturated/compressed and band-limited
// hiss + crackle rises. Nothing is cross-faded between two differently-filtered copies, so there is
// no phasey comb-filter zone in the middle of the transition.
#pragma once

#include <cstdint>

namespace vr {

struct RadioParams {
	float noise       = 0.035f; // hiss level at full radio (linear, pre-gain)
	float drive       = 4.0f;   // saturation amount at full radio (0 = clean)
	float crackle     = 1.0f;   // crackle density multiplier (0 = none)
	bool squelch      = true;   // short static burst when a far-away transmission starts
	float smoothingSec = 0.35f; // time constant for distance changes
};

// 0 below `start`, 1 above `full`, smoothstep in between.
float radioAmount(float distance, float start, float full);

struct Biquad {
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	float z1 = 0, z2 = 0;

	inline float process(float x) {
		const float y = b0 * x + z1;
		z1            = b1 * x - a1 * y + z2;
		z2            = b2 * x - a2 * y;
		return y;
	}
	void reset() { z1 = z2 = 0; }
	void lowpass(float freq, float q, float rate);
	void highpass(float freq, float q, float rate);
	void peak(float freq, float q, float gainDb, float rate);
};

class RadioVoice {
public:
	// Processes interleaved PCM in place.
	//  targetWet  : 0..1 how much "radio" this voice should be (from radioAmount()).
	//  targetComp : gain applied to the voice at any wet value (replaces Mumble's distance fade by ours).
	//  nowSec     : monotonic time, used to detect the start of a new transmission.
	void process(float *pcm, uint32_t frames, uint16_t channels, uint32_t rate, float targetWet, float targetComp,
				 double nowSec, const RadioParams &p);

	float currentWet() const { return m_wet; }

private:
	static constexpr int kMaxCh = 2;

	void resetState();
	void designFixed(uint32_t rate);
	float white();

	Biquad m_hp[kMaxCh][2], m_lp[kMaxCh][2], m_peak[kMaxCh];
	Biquad m_noiseHp, m_noiseLp;

	uint32_t m_rate     = 0;
	float m_wet         = 0.f;
	float m_comp        = 1.f;
	float m_env         = 0.f;
	float m_crackleAmp  = 0.f;
	uint32_t m_squelchLeft  = 0;
	uint32_t m_squelchTotal = 0;
	float m_squelchAmp      = 0.f;
	double m_lastSeen       = -1e9;
	uint32_t m_rng          = 0x9E3779B9u;
};

} // namespace vr
