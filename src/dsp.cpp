#include "dsp.hpp"
#include <algorithm>
#include <cstring>

namespace crossfeed {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CrossfeedDSP::CrossfeedDSP() {
    recompute_coeffs_locked();
}

void CrossfeedDSP::set_params(float sample_rate, float level_db, float freq_hz, bool enabled) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    params_.sample_rate = std::max(8000.0f, sample_rate);
    params_.level_db = std::clamp(level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    params_.freq_hz = std::clamp(freq_hz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    params_.enabled = enabled;
    recompute_coeffs_locked();
}

void CrossfeedDSP::set_level_db(float level_db) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    params_.level_db = std::clamp(level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    recompute_coeffs_locked();
}

void CrossfeedDSP::set_freq_hz(float freq_hz) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    params_.freq_hz = std::clamp(freq_hz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    recompute_coeffs_locked();
}

void CrossfeedDSP::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    params_.enabled = enabled;
}

bool CrossfeedDSP::is_enabled() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.enabled;
}

float CrossfeedDSP::get_level_db() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.level_db;
}

float CrossfeedDSP::get_freq_hz() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.freq_hz;
}

float CrossfeedDSP::get_sample_rate() const {
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.sample_rate;
}

void CrossfeedDSP::reset() {
    dir_l_.reset();
    dir_r_.reset();
    cross_l_.reset();
    cross_r_.reset();
}

void CrossfeedDSP::recompute_coeffs_locked() {
    params_.gain2 = std::pow(10.0f, params_.level_db / 20.0f);
    float dir_gain_db = FULL_DIR_GAIN * (params_.gain2 / FULL_GAIN2);

    compute_lowshelf(params_.dir_coeffs, params_.sample_rate, params_.freq_hz, 0.7071f, dir_gain_db);
    compute_lowpass(params_.cross_coeffs, params_.sample_rate, params_.freq_hz, 0.5f);
}

void CrossfeedDSP::compute_lowshelf(BiquadCoeffs& out, float sample_rate, float freq, float q, float gain_db) {
    float A = std::pow(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / sample_rate;
    float cos_w = std::cos(w0);
    float sin_w = std::sin(w0);
    float alpha = sin_w / (2.0f * q);
    float sqrtA = std::sqrt(A);

    float a0 = (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha;
    if (std::abs(a0) < 1e-9f) {
        out = BiquadCoeffs{};
        return;
    }
    float inv_a0 = 1.0f / a0;

    out.b0 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha)) * inv_a0;
    out.b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w)) * inv_a0;
    out.b2 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha)) * inv_a0;
    out.a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w)) * inv_a0;
    out.a2 = ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha) * inv_a0;
}

void CrossfeedDSP::compute_lowpass(BiquadCoeffs& out, float sample_rate, float freq, float q) {
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / sample_rate;
    float cos_w = std::cos(w0);
    float sin_w = std::sin(w0);
    float alpha = sin_w / (2.0f * q);

    float a0 = 1.0f + alpha;
    if (std::abs(a0) < 1e-9f) {
        out = BiquadCoeffs{};
        return;
    }
    float inv_a0 = 1.0f / a0;

    out.b0 = ((1.0f - cos_w) * 0.5f) * inv_a0;
    out.b1 = (1.0f - cos_w) * inv_a0;
    out.b2 = ((1.0f - cos_w) * 0.5f) * inv_a0;
    out.a1 = (-2.0f * cos_w) * inv_a0;
    out.a2 = (1.0f - alpha) * inv_a0;
}

void CrossfeedDSP::process_interleaved(const float* in, float* out, size_t frames) noexcept {
    FilterParams p;
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        p = params_;
    }

    if (!p.enabled) {
        if (in != out) {
            for (size_t i = 0; i < frames * 2; ++i) {
                out[i] = in[i];
            }
        }
        return;
    }

    const float gain2 = p.gain2;
    const BiquadCoeffs& dir_c = p.dir_coeffs;
    const BiquadCoeffs& cross_c = p.cross_coeffs;

    for (size_t i = 0; i < frames; ++i) {
        float in_l = in[2 * i];
        float in_r = in[2 * i + 1];

        float d_l = dir_l_.process(in_l, dir_c);
        float d_r = dir_r_.process(in_r, dir_c);

        float c_l = cross_l_.process(in_l, cross_c);
        float c_r = cross_r_.process(in_r, cross_c);

        out[2 * i]     = d_l + gain2 * c_r;
        out[2 * i + 1] = d_r + gain2 * c_l;
    }
}

void CrossfeedDSP::process_planar(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t frames) noexcept {
    FilterParams p;
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        p = params_;
    }

    if (!p.enabled) {
        if (in_l != out_l) {
            std::memcpy(out_l, in_l, frames * sizeof(float));
        }
        if (in_r != out_r) {
            std::memcpy(out_r, in_r, frames * sizeof(float));
        }
        return;
    }

    const float gain2 = p.gain2;
    const BiquadCoeffs& dir_c = p.dir_coeffs;
    const BiquadCoeffs& cross_c = p.cross_coeffs;

    for (size_t i = 0; i < frames; ++i) {
        float l = in_l[i];
        float r = in_r[i];

        float d_l = dir_l_.process(l, dir_c);
        float d_r = dir_r_.process(r, dir_c);

        float c_l = cross_l_.process(l, cross_c);
        float c_r = cross_r_.process(r, cross_c);

        out_l[i] = d_l + gain2 * c_r;
        out_r[i] = d_r + gain2 * c_l;
    }
}

} // namespace crossfeed
