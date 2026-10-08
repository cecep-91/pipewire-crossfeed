#pragma once

#include <cstddef>
#include <cmath>
#include <mutex>
#include <atomic>

namespace crossfeed {

constexpr float DEFAULT_LEVEL_DB = -10.0f;
constexpr float DEFAULT_FREQ_HZ = 700.0f;
constexpr float MIN_LEVEL_DB = -30.0f;
constexpr float MAX_LEVEL_DB = -6.0f;
constexpr float MIN_FREQ_HZ = 200.0f;
constexpr float MAX_FREQ_HZ = 2000.0f;
constexpr float FULL_GAIN2 = 0.316f;
constexpr float FULL_DIR_GAIN = -1.5f;

struct BiquadCoeffs {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

class Biquad {
public:
    inline float process(float in, const BiquadCoeffs& c) noexcept {
        float out = c.b0 * in + s1_;
        s1_ = c.b1 * in - c.a1 * out + s2_;
        s2_ = c.b2 * in - c.a2 * out;
        return out;
    }

    void reset() noexcept {
        s1_ = 0.0f;
        s2_ = 0.0f;
    }

private:
    float s1_ = 0.0f;
    float s2_ = 0.0f;
};

struct FilterParams {
    float sample_rate = 48000.0f;
    float level_db = DEFAULT_LEVEL_DB;
    float freq_hz = DEFAULT_FREQ_HZ;
    bool enabled = true;

    BiquadCoeffs dir_coeffs;
    BiquadCoeffs cross_coeffs;
    float gain2 = FULL_GAIN2;
};

class CrossfeedDSP {
public:
    CrossfeedDSP();

    void set_params(float sample_rate, float level_db, float freq_hz, bool enabled);
    void set_level_db(float level_db);
    void set_freq_hz(float freq_hz);
    void set_enabled(bool enabled);
    bool is_enabled() const;
    float get_level_db() const;
    float get_freq_hz() const;
    float get_sample_rate() const;

    void reset();

    // In-place or separate buffer processing of interleaved 2-channel 32-bit float audio
    void process_interleaved(const float* in, float* out, size_t frames) noexcept;

    // Zero-allocation planar (separate L and R channel buffers) processing for native PipeWire graph
    void process_planar(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t frames) noexcept;

    static void compute_lowshelf(BiquadCoeffs& out, float sample_rate, float freq, float q, float gain_db);
    static void compute_lowpass(BiquadCoeffs& out, float sample_rate, float freq, float q);

private:
    void recompute_coeffs_locked();

    mutable std::mutex params_mutex_;
    FilterParams params_;

    // DSP state (accessed only by audio thread)
    Biquad dir_l_;
    Biquad dir_r_;
    Biquad cross_l_;
    Biquad cross_r_;
};

} // namespace crossfeed
