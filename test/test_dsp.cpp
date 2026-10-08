#include "dsp.hpp"
#include "config.hpp"

#include <iostream>
#include <cmath>
#include <cassert>
#include <complex>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace fs = std::filesystem;
using namespace crossfeed;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Utility: bit-level IEEE-754 float inspection (immune to -ffast-math optimizations)
static inline bool is_nan_or_inf(float x) noexcept {
    uint32_t b;
    std::memcpy(&b, &x, sizeof(b));
    return ((b >> 23) & 0xFF) == 0xFF;
}

static inline bool is_denormal(float x) noexcept {
    uint32_t b;
    std::memcpy(&b, &x, sizeof(b));
    return ((b >> 23) & 0xFF) == 0 && (b & 0x7FFFFF) != 0;
}

static inline bool is_invalid_float(float x) noexcept {
    return is_nan_or_inf(x) || is_denormal(x);
}

// Compute frequency response H(e^{j*w}) of biquad filter
static std::complex<double> biquad_transfer_function(const BiquadCoeffs& c, double f, double fs) {
    double w = 2.0 * M_PI * f / fs;
    std::complex<double> z1(std::cos(-w), std::sin(-w));
    std::complex<double> z2(std::cos(-2.0 * w), std::sin(-2.0 * w));
    std::complex<double> num = static_cast<double>(c.b0) + static_cast<double>(c.b1) * z1 + static_cast<double>(c.b2) * z2;
    std::complex<double> den = 1.0 + static_cast<double>(c.a1) * z1 + static_cast<double>(c.a2) * z2;
    return num / den;
}

static void test_biquad_lowshelf() {
    std::cout << "[TEST] Biquad Low-Shelf Filter..." << std::endl;

    const float sample_rates[] = {44100.0f, 48000.0f, 96000.0f};
    const float gain_dbs[] = {-12.0f, -6.0f, -1.5f, 3.0f};
    const float cutoffs[] = {500.0f, 700.0f, 1200.0f};

    for (float fs : sample_rates) {
        for (float gain_db : gain_dbs) {
            for (float fc : cutoffs) {
                BiquadCoeffs coeffs;
                CrossfeedDSP::compute_lowshelf(coeffs, fs, fc, 0.7071f, gain_db);

                // 1. DC gain check (f -> 0)
                double h_dc = std::abs(biquad_transfer_function(coeffs, 0.0, fs));
                double expected_dc_gain = std::pow(10.0, gain_db / 20.0);
                assert(std::abs(h_dc - expected_dc_gain) < 5e-4);

                // 2. Nyquist gain check (f -> fs / 2): should be ~0 dB (ratio 1.0 +/- 0.05)
                double h_nyq = std::abs(biquad_transfer_function(coeffs, fs / 2.0, fs));
                assert(std::abs(h_nyq - 1.0) <= 0.05);

                // 3. Time-domain simulation verification with Biquad::process
                Biquad biquad;
                float dc_out = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    dc_out = biquad.process(1.0f, coeffs);
                }
                assert(std::abs(dc_out - static_cast<float>(expected_dc_gain)) < 5e-4f);

                // Nyquist alternating signal (+1, -1, +1, -1, ...)
                biquad.reset();
                float nyq_out = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    float in = (i % 2 == 0) ? 1.0f : -1.0f;
                    nyq_out = biquad.process(in, coeffs);
                }
                assert(std::abs(std::abs(nyq_out) - 1.0f) <= 0.05f);
            }
        }
    }

    std::cout << "  PASS: Low-shelf DC gain matches 10^(gain_db/20) and Nyquist gain is ~1.0 (0 dB)" << std::endl;
}

static void test_biquad_lowpass() {
    std::cout << "[TEST] Biquad Low-Pass Filter..." << std::endl;

    const float sample_rates[] = {44100.0f, 48000.0f, 96000.0f};
    const float cutoffs[] = {400.0f, 700.0f, 2000.0f};

    for (float fs : sample_rates) {
        for (float fc : cutoffs) {
            BiquadCoeffs coeffs;
            CrossfeedDSP::compute_lowpass(coeffs, fs, fc, 0.5f);

            // 1. DC gain check (f = 0): passes unattenuated (gain ~ 1.0)
            double h_dc = std::abs(biquad_transfer_function(coeffs, 0.0, fs));
            assert(std::abs(h_dc - 1.0) < 1e-4);

            // 2. Nyquist gain check (f = fs / 2): strongly attenuated (< 0.1)
            double h_nyq = std::abs(biquad_transfer_function(coeffs, fs / 2.0, fs));
            assert(h_nyq < 0.1);

            // 3. Time-domain simulation
            Biquad biquad;
            float dc_out = 0.0f;
            for (int i = 0; i < 2000; ++i) {
                dc_out = biquad.process(1.0f, coeffs);
            }
            assert(std::abs(dc_out - 1.0f) < 1e-4f);

            biquad.reset();
            float nyq_out = 0.0f;
            for (int i = 0; i < 2000; ++i) {
                float in = (i % 2 == 0) ? 1.0f : -1.0f;
                nyq_out = biquad.process(in, coeffs);
            }
            assert(std::abs(nyq_out) < 0.1f);
        }
    }

    std::cout << "  PASS: Low-pass DC passes unattenuated (~1.0) and Nyquist is attenuated (<0.1)" << std::endl;
}

static void test_biquad_allpass() {
    std::cout << "[TEST] Biquad All-Pass Filter..." << std::endl;

    const float fs = 48000.0f;
    const float f0 = 1500.0f;
    const float q = 0.7071f;

    BiquadCoeffs coeffs;
    CrossfeedDSP::compute_allpass(coeffs, fs, f0, q);

    // Verify unit magnitude across spectrum
    const double test_freqs[] = {20.0, 100.0, 500.0, 1000.0, 1500.0, 3000.0, 8000.0, 15000.0, 22000.0};
    for (double f : test_freqs) {
        auto H = biquad_transfer_function(coeffs, f, fs);
        double mag = std::abs(H);
        assert(std::abs(mag - 1.0) < 1e-4);
    }

    // Verify phase shift is introduced across frequencies
    auto H_low = biquad_transfer_function(coeffs, 50.0, fs);
    auto H_f0 = biquad_transfer_function(coeffs, f0, fs);
    auto H_high = biquad_transfer_function(coeffs, 10000.0, fs);

    double phase_low = std::arg(H_low);
    double phase_f0 = std::arg(H_f0);
    double phase_high = std::arg(H_high);

    // Near cutoff frequency f0, APF introduces ~ -pi phase shift
    double diff_from_pi = std::min(std::abs(phase_f0 - (-M_PI)), std::abs(phase_f0 - M_PI));
    assert(diff_from_pi < 0.05);

    // Phase shift is non-zero and varies across spectrum
    assert(phase_low != phase_f0);
    assert(phase_f0 != phase_high);

    // Time-domain verification with sine wave
    Biquad biquad;
    float peak_amp = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        float in = std::sin(2.0 * static_cast<float>(M_PI) * f0 * static_cast<float>(i) / fs);
        float out = biquad.process(in, coeffs);
        if (i > 3000) {
            peak_amp = std::max(peak_amp, std::abs(out));
        }
    }
    assert(std::abs(peak_amp - 1.0f) < 0.05f);

    std::cout << "  PASS: All-pass preserves unit magnitude while introducing phase shift" << std::endl;
}

static void test_delay_line_interpolation_and_wrap() {
    std::cout << "[TEST] DelayLine Sub-Sample Interpolation and Wrap-Around..." << std::endl;

    DelayLine dl;
    dl.reset();

    // 1. Basic write and integer delays
    dl.write(10.0f); // 4 steps ago
    dl.write(20.0f); // 3 steps ago
    dl.write(30.0f); // 2 steps ago
    dl.write(40.0f); // 1 step ago
    dl.write(50.0f); // 0 steps ago (most recent)

    assert(dl.read(0.0f) == 50.0f);
    assert(dl.read(1.0f) == 40.0f);
    assert(dl.read(2.0f) == 30.0f);
    assert(dl.read(3.0f) == 20.0f);
    assert(dl.read(4.0f) == 10.0f);

    // 2. Sub-sample linear interpolation at 2.5f
    // sample 2 is 30.0f, sample 3 is 20.0f -> 0.5 * 30.0 + 0.5 * 20.0 = 25.0f
    float read_2_5 = dl.read(2.5f);
    assert(std::abs(read_2_5 - 25.0f) < 1e-6f);

    // Fractional interpolation weights
    float read_2_25 = dl.read(2.25f); // 0.75 * 30.0 + 0.25 * 20.0 = 27.5f
    assert(std::abs(read_2_25 - 27.5f) < 1e-6f);

    float read_2_75 = dl.read(2.75f); // 0.25 * 30.0 + 0.75 * 20.0 = 22.5f
    assert(std::abs(read_2_75 - 22.5f) < 1e-6f);

    // 3. Step impulse test
    dl.reset();
    dl.write(0.0f);
    dl.write(0.0f);
    dl.write(1.0f); // impulse at delay 2
    dl.write(0.0f);
    dl.write(0.0f);

    assert(dl.read(2.0f) == 1.0f);
    assert(dl.read(3.0f) == 0.0f);
    assert(std::abs(dl.read(2.5f) - 0.5f) < 1e-6f);

    // 4. Circular buffer wrap-around past index 2048
    dl.reset();
    constexpr size_t TOTAL_SAMPLES = 5000;
    for (size_t i = 0; i < TOTAL_SAMPLES; ++i) {
        dl.write(static_cast<float>(i));
    }
    // Most recently written sample is (TOTAL_SAMPLES - 1)
    const float last_val = static_cast<float>(TOTAL_SAMPLES - 1);

    for (int d = 0; d < 1000; ++d) {
        float expected = last_val - static_cast<float>(d);
        float actual = dl.read(static_cast<float>(d));
        assert(std::abs(actual - expected) < 1e-4f);
    }

    // Fractional read across buffer boundaries
    float wrap_frac = dl.read(2.5f);
    assert(std::abs(wrap_frac - (last_val - 2.5f)) < 1e-4f);

    std::cout << "  PASS: DelayLine fractional interpolation and circular wrap verified" << std::endl;
}

static void test_crossfeed_dsp_stability() {
    std::cout << "[TEST] CrossfeedDSP Processing Stability (48,000 frames)..." << std::endl;

    CrossfeedDSP dsp;
    constexpr size_t FRAMES = 48000;

    for (bool adv : {true, false}) {
        dsp.set_all_params(48000.0f, -10.0f, 700.0f, 280.0f, 1500.0f, -1.5f, 3000.0f, adv, true);

        // --- 1. Interleaved Silence ---
        dsp.reset();
        std::vector<float> in_silence(FRAMES * 2, 0.0f);
        std::vector<float> out_silence(FRAMES * 2, 0.0f);
        dsp.process_interleaved(in_silence.data(), out_silence.data(), FRAMES);
        for (size_t i = 0; i < FRAMES * 2; ++i) {
            assert(!is_invalid_float(out_silence[i]));
            assert(out_silence[i] == 0.0f);
        }

        // --- 2. Interleaved Impulse ---
        dsp.reset();
        std::vector<float> in_impulse(FRAMES * 2, 0.0f);
        in_impulse[0] = 1.0f; // Left impulse
        in_impulse[1] = 0.5f; // Right impulse
        std::vector<float> out_impulse(FRAMES * 2, 0.0f);
        dsp.process_interleaved(in_impulse.data(), out_impulse.data(), FRAMES);
        for (size_t i = 0; i < FRAMES * 2; ++i) {
            assert(!is_invalid_float(out_impulse[i]));
        }

        // --- 3. Planar Silence ---
        dsp.reset();
        std::vector<float> in_l_silence(FRAMES, 0.0f), in_r_silence(FRAMES, 0.0f);
        std::vector<float> out_l_silence(FRAMES, 0.0f), out_r_silence(FRAMES, 0.0f);
        dsp.process_planar(in_l_silence.data(), in_r_silence.data(),
                           out_l_silence.data(), out_r_silence.data(), FRAMES);
        for (size_t i = 0; i < FRAMES; ++i) {
            assert(!is_invalid_float(out_l_silence[i]));
            assert(!is_invalid_float(out_r_silence[i]));
            assert(out_l_silence[i] == 0.0f);
            assert(out_r_silence[i] == 0.0f);
        }

        // --- 4. Planar Impulse ---
        dsp.reset();
        std::vector<float> in_l_impulse(FRAMES, 0.0f), in_r_impulse(FRAMES, 0.0f);
        in_l_impulse[0] = 1.0f;
        in_r_impulse[0] = 0.0f;
        std::vector<float> out_l_impulse(FRAMES, 0.0f), out_r_impulse(FRAMES, 0.0f);
        dsp.process_planar(in_l_impulse.data(), in_r_impulse.data(),
                           out_l_impulse.data(), out_r_impulse.data(), FRAMES);
        for (size_t i = 0; i < FRAMES; ++i) {
            assert(!is_invalid_float(out_l_impulse[i]));
            assert(!is_invalid_float(out_r_impulse[i]));
        }
    }

    // --- 5. Bypass mode check ---
    dsp.set_enabled(false);
    assert(!dsp.is_enabled());
    std::vector<float> test_in(256 * 2, 0.75f);
    std::vector<float> test_out(256 * 2, 0.0f);
    dsp.process_interleaved(test_in.data(), test_out.data(), 256);
    for (size_t i = 0; i < 256 * 2; ++i) {
        assert(test_out[i] == 0.75f);
    }

    std::cout << "  PASS: Silence and impulse processed with no NaN, Inf, or denormals" << std::endl;
}

static void test_config_state_roundtrip() {
    std::cout << "[TEST] Config State Round-Trip..." << std::endl;

    // Use isolated temp directory for XDG_CONFIG_HOME
    fs::path tmp_dir = fs::temp_directory_path() / "pipewire_crossfeed_test_config";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);

    const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string prev_xdg_val = prev_xdg ? prev_xdg : "";
    setenv("XDG_CONFIG_HOME", tmp_dir.c_str(), 1);

    ConfigState orig;
    orig.enabled = false;
    orig.level_db = -18.5f;
    orig.freq_hz = 620.0f;
    orig.delay_us = 340.0f;
    orig.phase_apf_hz = 1350.0f;
    orig.center_trim_db = -3.2f;
    orig.shadow_hz = 4800.0f;
    orig.advanced_effects = false;
    orig.backend = "alsa";
    orig.target_sink = "test_sink_device_0";
    orig.sample_rate = 96000;
    orig.buffer_frames = 1024;

    bool saved = Config::save_state(orig);
    assert(saved);
    assert(fs::exists(Config::get_state_file_path()));

    ConfigState loaded;
    // Overwrite with initial dummies to ensure values are actually read from file
    loaded.enabled = true;
    loaded.level_db = 0.0f;
    loaded.freq_hz = 0.0f;
    loaded.delay_us = 0.0f;
    loaded.phase_apf_hz = 0.0f;
    loaded.center_trim_db = 0.0f;
    loaded.shadow_hz = 0.0f;
    loaded.advanced_effects = true;
    loaded.backend = "";
    loaded.target_sink = "";
    loaded.sample_rate = 0;
    loaded.buffer_frames = 0;

    bool loaded_ok = Config::load_state(loaded);
    assert(loaded_ok);

    assert(loaded.enabled == orig.enabled);
    assert(std::abs(loaded.level_db - orig.level_db) < 1e-4f);
    assert(std::abs(loaded.freq_hz - orig.freq_hz) < 1e-4f);
    assert(std::abs(loaded.delay_us - orig.delay_us) < 1e-4f);
    assert(std::abs(loaded.phase_apf_hz - orig.phase_apf_hz) < 1e-4f);
    assert(std::abs(loaded.center_trim_db - orig.center_trim_db) < 1e-4f);
    assert(std::abs(loaded.shadow_hz - orig.shadow_hz) < 1e-4f);
    assert(loaded.advanced_effects == orig.advanced_effects);
    assert(loaded.backend == orig.backend);
    assert(loaded.target_sink == orig.target_sink);
    assert(loaded.sample_rate == orig.sample_rate);
    assert(loaded.buffer_frames == orig.buffer_frames);

    // Cleanup
    fs::remove_all(tmp_dir);
    if (!prev_xdg_val.empty()) {
        setenv("XDG_CONFIG_HOME", prev_xdg_val.c_str(), 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }

    std::cout << "  PASS: ConfigState successfully saved, reloaded, and matched" << std::endl;
}

int main() {
    // Enable FTZ/DAZ mode if hardware supports it (standard for audio DSP)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1 << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "Running Crossfeed DSP & Core Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    test_biquad_lowshelf();
    test_biquad_lowpass();
    test_biquad_allpass();
    test_delay_line_interpolation_and_wrap();
    test_crossfeed_dsp_stability();
    test_config_state_roundtrip();

    std::cout << "========================================" << std::endl;
    std::cout << "All DSP and core characterization tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
