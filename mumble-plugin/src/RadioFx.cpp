#include "RadioFx.h"

#include <algorithm>
#include <cmath>

namespace vr {

namespace {
	constexpr float kPi = 3.14159265358979f;

	// 4th-order Butterworth = two cascaded biquads with these Qs.
	constexpr float kQ1 = 0.5412f;
	constexpr float kQ2 = 1.3066f;

	// Gap after which incoming audio counts as a new transmission.
	constexpr double kNewTransmissionGap = 0.25;

	inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
	inline float expLerp(float a, float b, float t) { return a * std::pow(b / a, t); }
} // namespace

float radioAmount(float distance, float start, float full) {
	if (!(full > start)) {
		return distance >= start ? 1.f : 0.f;
	}
	float t = (distance - start) / (full - start);
	t       = std::clamp(t, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

static void rbjCommon(float freq, float q, float rate, float &cosw, float &alpha) {
	freq        = std::clamp(freq, 10.f, 0.45f * rate);
	const float w0 = 2.f * kPi * freq / rate;
	cosw        = std::cos(w0);
	alpha       = std::sin(w0) / (2.f * q);
}

void Biquad::lowpass(float freq, float q, float rate) {
	float c, a;
	rbjCommon(freq, q, rate, c, a);
	const float a0 = 1.f + a;
	b0             = (1.f - c) * 0.5f / a0;
	b1             = (1.f - c) / a0;
	b2             = b0;
	a1             = -2.f * c / a0;
	a2             = (1.f - a) / a0;
}

void Biquad::highpass(float freq, float q, float rate) {
	float c, a;
	rbjCommon(freq, q, rate, c, a);
	const float a0 = 1.f + a;
	b0             = (1.f + c) * 0.5f / a0;
	b1             = -(1.f + c) / a0;
	b2             = b0;
	a1             = -2.f * c / a0;
	a2             = (1.f - a) / a0;
}

void Biquad::peak(float freq, float q, float gainDb, float rate) {
	float c, a;
	rbjCommon(freq, q, rate, c, a);
	const float A  = std::pow(10.f, gainDb / 40.f);
	const float a0 = 1.f + a / A;
	b0             = (1.f + a * A) / a0;
	b1             = -2.f * c / a0;
	b2             = (1.f - a * A) / a0;
	a1             = b1;
	a2             = (1.f - a / A) / a0;
}

float RadioVoice::white() {
	// xorshift32 -> [-1, 1)
	m_rng ^= m_rng << 13;
	m_rng ^= m_rng >> 17;
	m_rng ^= m_rng << 5;
	return static_cast< float >(static_cast< int32_t >(m_rng)) * (1.f / 2147483648.f);
}

void RadioVoice::resetState() {
	for (int c = 0; c < kMaxCh; ++c) {
		m_hp[c][0].reset();
		m_hp[c][1].reset();
		m_lp[c][0].reset();
		m_lp[c][1].reset();
		m_peak[c].reset();
	}
	m_noiseHp.reset();
	m_noiseLp.reset();
	m_env        = 0.f;
	m_crackleAmp = 0.f;
}

void RadioVoice::designFixed(uint32_t rate) {
	m_rate = rate;
	m_noiseHp.highpass(400.f, 0.707f, static_cast< float >(rate));
	m_noiseLp.lowpass(3500.f, 0.707f, static_cast< float >(rate));
	resetState();
}

void RadioVoice::process(float *pcm, uint32_t frames, uint16_t channels, uint32_t rate, float targetWet,
						 float targetComp, double nowSec, const RadioParams &p) {
	if (frames == 0 || channels == 0 || rate == 0) {
		return;
	}
	const int ch = std::min< int >(channels, kMaxCh);
	if (rate != m_rate) {
		designFixed(rate);
	}
	const float fs = static_cast< float >(rate);

	// New transmission? Jump straight to the target instead of fading from stale values, and start
	// with a squelch burst if the speaker is far away.
	if (nowSec - m_lastSeen > kNewTransmissionGap) {
		resetState();
		m_wet  = targetWet;
		m_comp = targetComp;
		if (p.squelch && targetWet > 0.3f) {
			m_squelchTotal = m_squelchLeft = static_cast< uint32_t >(0.11f * fs);
			m_squelchAmp                   = 0.22f * targetWet;
		}
	}
	m_lastSeen = nowSec;

	// One-pole smoothing per block, linearly ramped inside the block.
	const float k = 1.f - std::exp(-static_cast< float >(frames) / (fs * std::max(p.smoothingSec, 0.01f)));
	const float wet0 = m_wet, comp0 = m_comp;
	const float wet1  = wet0 + (targetWet - wet0) * k;
	const float comp1 = comp0 + (targetComp - comp0) * k;
	m_wet             = wet1;
	m_comp            = comp1;

	if (wet0 < 1e-4f && wet1 < 1e-4f && m_squelchLeft == 0) {
		// Close range: no effect, only the distance gain. Keep filters cleared so a later fade-in starts clean.
		resetState();
		if (std::fabs(comp0 - 1.f) > 1e-4f || std::fabs(comp1 - 1.f) > 1e-4f) {
			const float inv = 1.f / static_cast< float >(frames);
			for (uint32_t i = 0; i < frames; ++i) {
				const float g = lerp(comp0, comp1, static_cast< float >(i + 1) * inv);
				for (uint16_t c = 0; c < channels; ++c) {
					pcm[i * channels + c] *= g;
				}
			}
		}
		return;
	}

	// Filter shape follows the block's mid-point wet value (changes per 10 ms block are tiny).
	const float wm   = 0.5f * (wet0 + wet1);
	const float hpF  = expLerp(20.f, 350.f, wm);
	const float lpF  = expLerp(20000.f, 2800.f, wm);
	const float honk = 7.f * wm;
	for (int c = 0; c < ch; ++c) {
		m_hp[c][0].highpass(hpF, kQ1, fs);
		m_hp[c][1].highpass(hpF, kQ2, fs);
		m_lp[c][0].lowpass(lpF, kQ1, fs);
		m_lp[c][1].lowpass(lpF, kQ2, fs);
		m_peak[c].peak(1700.f, 1.1f, honk, fs);
	}

	const float envAttack  = 1.f - std::exp(-1.f / (0.005f * fs));
	const float envRelease = 1.f - std::exp(-1.f / (0.120f * fs));
	const float crackleDecay = std::exp(-1.f / (0.0015f * fs));
	const float invFrames  = 1.f / static_cast< float >(frames);

	for (uint32_t i = 0; i < frames; ++i) {
		const float t    = static_cast< float >(i + 1) * invFrames;
		const float w    = lerp(wet0, wet1, t);
		const float comp = lerp(comp0, comp1, t);
		const float gain = comp;
		const float gd   = 1.f + p.drive * w;
		const float satNorm = std::pow(gd, -0.62f);

		// --- static: hiss (ducked by the voice like a radio AGC), crackle, squelch burst ---
		float noiseIn = white() * p.noise * w * w * (1.f - 0.6f * std::min(1.f, m_env * 8.f));
		if (p.crackle > 0.f) {
			if ((m_rng & 0xFFFFu) < static_cast< uint32_t >(65536.f * 0.00025f * p.crackle * w * w)) {
				m_crackleAmp = 0.035f + 0.07f * std::fabs(white()); // quiet pops, well under the voice
			}
			noiseIn += m_crackleAmp * white();
			m_crackleAmp *= crackleDecay;
		}
		if (m_squelchLeft > 0) {
			const float e = static_cast< float >(m_squelchLeft) / static_cast< float >(m_squelchTotal);
			noiseIn += white() * m_squelchAmp * std::sqrt(e);
			--m_squelchLeft;
		}
		const float noise = m_noiseLp.process(m_noiseHp.process(noiseIn));

		float level = 0.f;
		for (int c = 0; c < ch; ++c) {
			float &s = pcm[i * channels + c];
			float y  = m_hp[c][1].process(m_hp[c][0].process(s));
			y        = m_lp[c][1].process(m_lp[c][0].process(y));
			y        = m_peak[c].process(y);
			const float sat = std::tanh(gd * y) * satNorm;
			y += w * (sat - y);
			level = std::max(level, std::fabs(y));
			y     = (y + noise) * gain;
			if (!std::isfinite(y)) {
				y = 0.f;
				resetState();
			}
			s = y;
		}
		// Extra channels beyond kMaxCh (shouldn't happen for speech): just apply gain.
		for (int c = ch; c < channels; ++c) {
			pcm[i * channels + c] *= gain;
		}
		m_env += (level - m_env) * (level > m_env ? envAttack : envRelease);
	}
}

} // namespace vr
