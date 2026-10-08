#include "dsp.hpp"
#include <algorithm>
#include <cstring>

namespace crossfeed {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CrossfeedDSP::CrossfeedDSP() {
    recompute_coeffs(param_buffers_[0]);
    param_buffers_[1] = param_buffers_[0];
    active_read_idx_.store(0, std::memory_order_relaxed);
    reset_requested_.store(false, std::memory_order_relaxed);
}

void CrossfeedDSP::set_params(float sample_rate, float level_db, float freq_hz, bool enabled) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].sample_rate = std::max(8000.0f, sample_rate);
    param_buffers_[next_idx].level_db = std::clamp(level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    param_buffers_[next_idx].freq_hz = std::clamp(freq_hz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    param_buffers_[next_idx].enabled = enabled;
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_all_params(float sample_rate, float level_db, float freq_hz,
                                 float delay_us, float phase_apf_hz, float center_trim_db,
                                 float shadow_hz, bool advanced_effects, bool enabled) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].sample_rate = std::max(8000.0f, sample_rate);
    param_buffers_[next_idx].level_db = std::clamp(level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    param_buffers_[next_idx].freq_hz = std::clamp(freq_hz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    param_buffers_[next_idx].delay_us = std::clamp(delay_us, MIN_DELAY_US, MAX_DELAY_US);
    param_buffers_[next_idx].phase_apf_hz = std::clamp(phase_apf_hz, MIN_PHASE_APF_HZ, MAX_PHASE_APF_HZ);
    param_buffers_[next_idx].center_trim_db = std::clamp(center_trim_db, MIN_CENTER_TRIM_DB, MAX_CENTER_TRIM_DB);
    param_buffers_[next_idx].shadow_hz = std::clamp(shadow_hz, MIN_SHADOW_HZ, MAX_SHADOW_HZ);
    param_buffers_[next_idx].advanced_effects = advanced_effects;
    param_buffers_[next_idx].enabled = enabled;
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_advanced_effects(bool enabled) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].advanced_effects = enabled;

    active_read_idx_.store(next_idx, std::memory_order_release);
}

bool CrossfeedDSP::get_advanced_effects() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].advanced_effects;
}

void CrossfeedDSP::set_level_db(float level_db) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].level_db = std::clamp(level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_freq_hz(float freq_hz) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].freq_hz = std::clamp(freq_hz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_delay_us(float delay_us) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].delay_us = std::clamp(delay_us, MIN_DELAY_US, MAX_DELAY_US);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_phase_apf_hz(float phase_apf_hz) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].phase_apf_hz = std::clamp(phase_apf_hz, MIN_PHASE_APF_HZ, MAX_PHASE_APF_HZ);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_center_trim_db(float center_trim_db) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].center_trim_db = std::clamp(center_trim_db, MIN_CENTER_TRIM_DB, MAX_CENTER_TRIM_DB);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_shadow_hz(float shadow_hz) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].shadow_hz = std::clamp(shadow_hz, MIN_SHADOW_HZ, MAX_SHADOW_HZ);
    recompute_coeffs(param_buffers_[next_idx]);

    active_read_idx_.store(next_idx, std::memory_order_release);
}

void CrossfeedDSP::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    size_t current_idx = active_read_idx_.load(std::memory_order_relaxed);
    size_t next_idx = 1 - current_idx;

    param_buffers_[next_idx] = param_buffers_[current_idx];
    param_buffers_[next_idx].enabled = enabled;

    active_read_idx_.store(next_idx, std::memory_order_release);
}

bool CrossfeedDSP::is_enabled() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].enabled;
}

float CrossfeedDSP::get_level_db() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].level_db;
}

float CrossfeedDSP::get_freq_hz() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].freq_hz;
}

float CrossfeedDSP::get_delay_us() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].delay_us;
}

float CrossfeedDSP::get_phase_apf_hz() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].phase_apf_hz;
}

float CrossfeedDSP::get_center_trim_db() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].center_trim_db;
}

float CrossfeedDSP::get_shadow_hz() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].shadow_hz;
}

float CrossfeedDSP::get_sample_rate() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return param_buffers_[active_read_idx_.load(std::memory_order_relaxed)].sample_rate;
}

void CrossfeedDSP::reset() {
    reset_requested_.store(true, std::memory_order_release);
}

void CrossfeedDSP::reset_internal() noexcept {
    dir_l_.reset();
    dir_r_.reset();
    cross_l_.reset();
    cross_r_.reset();
    apf_l_.reset();
    apf_r_.reset();
    shadow_l_.reset();
    shadow_r_.reset();
    delay_l_.reset();
    delay_r_.reset();
}

void CrossfeedDSP::recompute_coeffs(FilterParams& params) {
    params.gain2 = std::pow(10.0f, params.level_db / 20.0f);
    params.trim_linear = std::pow(10.0f, params.center_trim_db / 20.0f);
    params.delay_samples = (params.delay_us / 1000000.0f) * params.sample_rate;

    float dir_gain_db = FULL_DIR_GAIN * (params.gain2 / FULL_GAIN2);

    compute_lowshelf(params.dir_coeffs, params.sample_rate, params.freq_hz, 0.7071f, dir_gain_db);
    compute_lowpass(params.cross_coeffs, params.sample_rate, params.freq_hz, 0.5f);
    compute_allpass(params.apf_coeffs, params.sample_rate, params.phase_apf_hz, 0.7071f);
    compute_lowpass(params.shadow_coeffs, params.sample_rate, params.shadow_hz, 0.7071f);
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

void CrossfeedDSP::compute_allpass(BiquadCoeffs& out, float sample_rate, float freq, float q) {
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

    out.b0 = (1.0f - alpha) * inv_a0;
    out.b1 = (-2.0f * cos_w) * inv_a0;
    out.b2 = (1.0f + alpha) * inv_a0;
    out.a1 = (-2.0f * cos_w) * inv_a0;
    out.a2 = (1.0f - alpha) * inv_a0;
}

void CrossfeedDSP::process_interleaved(const float* in, float* out, size_t frames) noexcept {
    if (reset_requested_.exchange(false, std::memory_order_relaxed)) {
        reset_internal();
    }

    const FilterParams& p = param_buffers_[active_read_idx_.load(std::memory_order_acquire)];

    if (!p.enabled) {
        if (in != out) {
            std::memcpy(out, in, frames * 2 * sizeof(float));
        }
        return;
    }

    const float gain2 = p.gain2;
    const float trim = p.trim_linear;
    const float delay_s = p.delay_samples;
    const BiquadCoeffs& dir_c = p.dir_coeffs;
    const BiquadCoeffs& cross_c = p.cross_coeffs;
    const BiquadCoeffs& apf_c = p.apf_coeffs;
    const BiquadCoeffs& shadow_c = p.shadow_coeffs;

    for (size_t i = 0; i < frames; ++i) {
        float in_l = in[2 * i];
        float in_r = in[2 * i + 1];

        float d_l = dir_l_.process(in_l, dir_c);
        float d_r = dir_r_.process(in_r, dir_c);

        float c_l = cross_l_.process(in_l, cross_c);
        float c_r = cross_r_.process(in_r, cross_c);

        if (p.advanced_effects) {
            c_l = shadow_l_.process(c_l, shadow_c);
            c_r = shadow_r_.process(c_r, shadow_c);

            c_l = apf_l_.process(c_l, apf_c);
            c_r = apf_r_.process(c_r, apf_c);

            delay_l_.write(c_l);
            delay_r_.write(c_r);

            float c_del_l = delay_l_.read(delay_s);
            float c_del_r = delay_r_.read(delay_s);

            out[2 * i]     = (d_l + gain2 * c_del_r) * trim;
            out[2 * i + 1] = (d_r + gain2 * c_del_l) * trim;
        } else {
            out[2 * i]     = d_l + gain2 * c_r;
            out[2 * i + 1] = d_r + gain2 * c_l;
        }
    }
}

void CrossfeedDSP::process_planar(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t frames) noexcept {
    if (reset_requested_.exchange(false, std::memory_order_relaxed)) {
        reset_internal();
    }

    const FilterParams& p = param_buffers_[active_read_idx_.load(std::memory_order_acquire)];

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
    const float trim = p.trim_linear;
    const float delay_s = p.delay_samples;
    const BiquadCoeffs& dir_c = p.dir_coeffs;
    const BiquadCoeffs& cross_c = p.cross_coeffs;
    const BiquadCoeffs& apf_c = p.apf_coeffs;
    const BiquadCoeffs& shadow_c = p.shadow_coeffs;

    for (size_t i = 0; i < frames; ++i) {
        float l = in_l[i];
        float r = in_r[i];

        float d_l = dir_l_.process(l, dir_c);
        float d_r = dir_r_.process(r, dir_c);

        float c_l = cross_l_.process(l, cross_c);
        float c_r = cross_r_.process(r, cross_c);

        if (p.advanced_effects) {
            c_l = shadow_l_.process(c_l, shadow_c);
            c_r = shadow_r_.process(c_r, shadow_c);

            c_l = apf_l_.process(c_l, apf_c);
            c_r = apf_r_.process(c_r, apf_c);

            delay_l_.write(c_l);
            delay_r_.write(c_r);

            float c_del_l = delay_l_.read(delay_s);
            float c_del_r = delay_r_.read(delay_s);

            out_l[i] = (d_l + gain2 * c_del_r) * trim;
            out_r[i] = (d_r + gain2 * c_del_l) * trim;
        } else {
            out_l[i] = d_l + gain2 * c_r;
            out_r[i] = d_r + gain2 * c_l;
        }
    }
}

} // namespace crossfeed
