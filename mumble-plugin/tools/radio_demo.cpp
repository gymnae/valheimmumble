// Offline listening test for the radio effect.
//
//   radio_demo out.wav                 -> synthetic "voice", speaker walks from 0 m to 100 m
//   radio_demo out.wav in.wav          -> your own 16-bit PCM WAV (mono or stereo, any rate)
//   radio_demo out.wav in.wav 30 60    -> custom radio start / full distances
//
// Prints the output level per distance band so the transition can be sanity-checked.

#include "RadioFx.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Wav {
	uint32_t rate     = 48000;
	uint16_t channels = 1;
	std::vector< float > data; // interleaved
};

bool readWav(const char *path, Wav &w) {
	FILE *f = std::fopen(path, "rb");
	if (!f) {
		return false;
	}
	char id[4];
	uint32_t size;
	if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4) || std::fread(&size, 4, 1, f) != 1
		|| std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4)) {
		std::fclose(f);
		return false;
	}
	uint16_t fmt = 0, bits = 0;
	while (std::fread(id, 1, 4, f) == 4 && std::fread(&size, 4, 1, f) == 1) {
		if (!std::memcmp(id, "fmt ", 4)) {
			std::vector< uint8_t > c(size);
			if (std::fread(c.data(), 1, size, f) != size) {
				break;
			}
			std::memcpy(&fmt, &c[0], 2);
			std::memcpy(&w.channels, &c[2], 2);
			std::memcpy(&w.rate, &c[4], 4);
			std::memcpy(&bits, &c[14], 2);
		} else if (!std::memcmp(id, "data", 4)) {
			if (fmt != 1 || bits != 16) {
				std::fprintf(stderr, "only 16-bit PCM WAV supported\n");
				break;
			}
			std::vector< int16_t > s(size / 2);
			const size_t n = std::fread(s.data(), 2, s.size(), f);
			w.data.resize(n);
			for (size_t i = 0; i < n; ++i) {
				w.data[i] = s[i] / 32768.f;
			}
			std::fclose(f);
			return true;
		} else {
			std::fseek(f, size + (size & 1), SEEK_CUR);
		}
	}
	std::fclose(f);
	return false;
}

bool writeWav(const char *path, const Wav &w) {
	FILE *f = std::fopen(path, "wb");
	if (!f) {
		return false;
	}
	const uint32_t dataBytes = static_cast< uint32_t >(w.data.size() * 2);
	const uint32_t riff = 36 + dataBytes, fmtSize = 16, byteRate = w.rate * w.channels * 2;
	const uint16_t pcm = 1, align = static_cast< uint16_t >(w.channels * 2), bits = 16;
	std::fwrite("RIFF", 1, 4, f);
	std::fwrite(&riff, 4, 1, f);
	std::fwrite("WAVEfmt ", 1, 8, f);
	std::fwrite(&fmtSize, 4, 1, f);
	std::fwrite(&pcm, 2, 1, f);
	std::fwrite(&w.channels, 2, 1, f);
	std::fwrite(&w.rate, 4, 1, f);
	std::fwrite(&byteRate, 4, 1, f);
	std::fwrite(&align, 2, 1, f);
	std::fwrite(&bits, 2, 1, f);
	std::fwrite("data", 1, 4, f);
	std::fwrite(&dataBytes, 4, 1, f);
	for (float v : w.data) {
		const int16_t s = static_cast< int16_t >(std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f));
		std::fwrite(&s, 2, 1, f);
	}
	std::fclose(f);
	return true;
}

// Crude vowel-ish source: pulse train with vibrato through three formant resonators, chopped into syllables.
Wav synthVoice(float seconds) {
	Wav w;
	const float fs = 48000.f;
	w.data.resize(static_cast< size_t >(seconds * fs));
	vr::Biquad f1, f2, f3;
	const float formants[5][3] = { { 730, 1090, 2440 }, { 270, 2290, 3010 }, { 530, 1840, 2480 },
								   { 300, 870, 2240 }, { 660, 1720, 2410 } };
	float phase = 0.f;
	for (size_t i = 0; i < w.data.size(); ++i) {
		const float t = static_cast< float >(i) / fs;
		const int vowel = static_cast< int >(t * 3.f) % 5;
		if (i % 480 == 0) {
			f1.peak(formants[vowel][0], 6.f, 18.f, fs);
			f2.peak(formants[vowel][1], 8.f, 14.f, fs);
			f3.peak(formants[vowel][2], 10.f, 10.f, fs);
		}
		const float f0 = 120.f + 8.f * std::sin(2.f * 3.14159f * 5.f * t) + 20.f * std::sin(2.f * 3.14159f * 0.3f * t);
		phase += f0 / fs;
		float src = 0.f;
		if (phase >= 1.f) {
			phase -= 1.f;
			src = 1.f;
		}
		src -= f0 / fs; // remove DC
		const float syl = std::pow(std::fmax(0.f, std::sin(3.14159f * std::fmod(t * 3.f, 1.f))), 0.6f);
		const float pause = std::fmod(t, 4.f) < 3.4f ? 1.f : 0.f; // short pauses -> new transmissions
		w.data[i] = 0.9f * f3.process(f2.process(f1.process(src))) * syl * pause;
	}
	return w;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s out.wav [in.wav] [start_m full_m]\n", argv[0]);
		return 1;
	}
	Wav w;
	if (argc >= 3) {
		if (!readWav(argv[2], w)) {
			std::fprintf(stderr, "could not read %s\n", argv[2]);
			return 1;
		}
	} else {
		w = synthVoice(24.f);
	}
	const float start = argc >= 5 ? std::strtof(argv[3], nullptr) : 30.f;
	const float full  = argc >= 5 ? std::strtof(argv[4], nullptr) : 60.f;

	vr::RadioVoice voice;
	vr::RadioParams params;
	const uint32_t block = w.rate / 100; // 10 ms like Mumble
	const size_t frames  = w.data.size() / w.channels;
	double rmsIn[10] = {}, rmsOut[10] = {};
	size_t cnt[10] = {};
	float peak = 0.f;

	for (size_t f0 = 0; f0 < frames; f0 += block) {
		const uint32_t n     = static_cast< uint32_t >(std::min< size_t >(block, frames - f0));
		const float progress = static_cast< float >(f0) / static_cast< float >(frames);
		const float distance = 100.f * progress;
		float *pcm           = &w.data[f0 * w.channels];

		// Skip silent blocks entirely, like Mumble does between transmissions.
		float e = 0.f;
		for (uint32_t i = 0; i < n * w.channels; ++i) {
			e = std::fmax(e, std::fabs(pcm[i]));
		}
		if (e < 1e-5f) {
			continue;
		}

		const int band = std::min(9, static_cast< int >(distance / 10.f));
		for (uint32_t i = 0; i < n * w.channels; ++i) {
			rmsIn[band] += pcm[i] * pcm[i];
		}
		const double now = static_cast< double >(f0) / w.rate;
		voice.process(pcm, n, w.channels, w.rate, vr::radioAmount(distance, start, full), 1.f, now, params);
		for (uint32_t i = 0; i < n * w.channels; ++i) {
			if (!std::isfinite(pcm[i])) {
				std::fprintf(stderr, "non-finite sample at frame %zu\n", f0);
				return 2;
			}
			rmsOut[band] += pcm[i] * pcm[i];
			peak = std::fmax(peak, std::fabs(pcm[i]));
		}
		cnt[band] += n * w.channels;
	}

	std::printf("distance   wet    in dBFS   out dBFS\n");
	for (int b = 0; b < 10; ++b) {
		if (!cnt[b]) {
			continue;
		}
		const float d = b * 10.f + 5.f;
		std::printf("%3d-%3d m  %.2f  %7.1f   %7.1f\n", b * 10, b * 10 + 10, vr::radioAmount(d, start, full),
					10 * std::log10(rmsIn[b] / cnt[b] + 1e-12), 10 * std::log10(rmsOut[b] / cnt[b] + 1e-12));
	}
	std::printf("peak %.3f\n", peak);
	return writeWav(argv[1], w) ? 0 : 1;
}
