// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "audio_core/hle/source.h"
#include "core/core.h"
#include "core/memory.h"

TEST_CASE("Incremental ADPCM preserves prediction and interpolation across blocks and loops",
          "[audio_core][hle][streaming]") {
    using Configuration = AudioCore::HLE::SourceConfiguration::Configuration;
    Core::System system;
    Memory::MemorySystem memory{system};
    AudioCore::HLE::Source source{0};
    source.SetMemory(memory);
    constexpr u32 length = 448;
    const u32 start = GENERATE(0u, 31u);
    const float rate = GENERATE(1.0f, 44100.0f / AudioCore::native_sample_rate);
    auto* encoded = memory.GetFCRAMPointer(0);
    for (u32 block = 0; block < 32; ++block) {
        encoded[block * 8] = 4;
        for (u32 byte = 1; byte < 8; ++byte) {
            encoded[block * 8 + byte] = static_cast<u8>(block * 7 + byte * 23);
        }
    }
    s16_le coefficients[16]{};
    coefficients[0] = 1024;
    coefficients[1] = -512;
    std::array<s16, 16> reference_coefficients{};
    reference_coefficients[0] = 1024;
    reference_coefficients[1] = -512;
    AudioCore::StereoBuffer16 reference;
    for (u32 loop = 0; loop < 16; ++loop) {
        AudioCore::Codec::ADPCMState predictor{};
        auto decoded = AudioCore::Codec::DecodeADPCM(encoded, length, reference_coefficients,
                                                    predictor);
        reference.insert(reference.end(), decoded.begin() + (loop == 0 ? start : 0),
                         decoded.end());
    }
    Configuration config{};
    config.gain[0][0] = 1.0f;
    config.gain_0_dirty.Assign(1);
    config.rate_multiplier = rate;
    config.rate_multiplier_dirty.Assign(1);
    config.interpolation_mode = Configuration::InterpolationMode::Linear;
    config.interpolation_dirty.Assign(1);
    config.adpcm_coefficients_dirty.Assign(1);
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Mono);
    config.mono_or_stereo_dirty.Assign(1);
    config.format.Assign(Configuration::Format::ADPCM);
    config.format_dirty.Assign(1);
    source.Tick(config, coefficients);
    AudioCore::QuadFrame32 mix{};
    source.MixInto(mix, 0);
    config.physical_address = Memory::FCRAM_PADDR;
    config.length = length;
    config.buffer_id = 1;
    config.is_looping.Assign(1);
    config.adpcm_dirty.Assign(1);
    config.play_position = start;
    config.play_position_dirty.Assign(1);
    config.embedded_buffer_dirty.Assign(1);
    config.enable = 1;
    config.enable_dirty.Assign(1);
    AudioCore::AudioInterp::State reference_interpolation{};
    for (u32 frame = 0; frame < 24; ++frame) {
        source.Tick(config, coefficients);
        mix = {};
        source.MixInto(mix, 0);
        AudioCore::StereoFrame16 expected{};
        std::size_t position = 0;
        AudioCore::AudioInterp::Linear(reference_interpolation, reference, rate, expected, position);
        REQUIRE(position == expected.size());
        for (u32 i = 0; i < AudioCore::samples_per_frame; ++i) {
            CAPTURE(frame, i, start, rate);
            REQUIRE(mix[i][0] == expected[i][0]);
        }
    }
}

TEST_CASE("Looping ADPCM reads ring refills as playback reaches each block",
          "[audio_core][hle][streaming]") {
    using Configuration = AudioCore::HLE::SourceConfiguration::Configuration;
    Core::System system;
    Memory::MemorySystem memory{system};
    AudioCore::HLE::Source source{0};
    source.SetMemory(memory);
    constexpr u32 length = 448;
    auto* encoded = memory.GetFCRAMPointer(0);
    const auto refill = [&](u32 first, u32 end, u8 value) {
        for (u32 block = first; block < end; ++block) {
            encoded[block * 8] = 0;
            std::fill_n(encoded + block * 8 + 1, 7, value);
        }
    };
    refill(0, 32, 0x11);
    Configuration config{};
    const s16_le coefficients[16]{};
    config.gain[0][0] = 1.0f;
    config.gain_0_dirty.Assign(1);
    config.rate_multiplier = 1.0f;
    config.rate_multiplier_dirty.Assign(1);
    config.interpolation_mode = Configuration::InterpolationMode::None;
    config.interpolation_dirty.Assign(1);
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Mono);
    config.mono_or_stereo_dirty.Assign(1);
    config.format.Assign(Configuration::Format::ADPCM);
    config.format_dirty.Assign(1);
    source.Tick(config, coefficients);
    AudioCore::QuadFrame32 mix{};
    source.MixInto(mix, 0);
    config.physical_address = Memory::FCRAM_PADDR;
    config.length = length;
    config.buffer_id = 1;
    config.is_looping.Assign(1);
    config.embedded_buffer_dirty.Assign(1);
    config.enable = 1;
    config.enable_dirty.Assign(1);
    for (u32 frame = 0; frame < 16; ++frame) {
        // Refill ahead of playback, then refill the consumed half for the next lap.
        if (frame == 1) {
            refill(16, 32, 0x22);
        }
        if (frame == 2) {
            refill(0, 16, 0x33);
        }
        const auto status = source.Tick(config, coefficients);
        REQUIRE(status.is_enabled);
        mix = {};
        source.MixInto(mix, 0);
        for (u32 i = 0; i < AudioCore::samples_per_frame; ++i) {
            const u32 absolute = frame * AudioCore::samples_per_frame + i;
            const s32 expected = absolute < 2 ? 0 :
                (absolute - 2) % length >= 224 ? 2 : absolute - 2 >= length ? 3 : 1;
            CAPTURE(frame, i, status.buffer_position);
            REQUIRE(mix[i][0] == expected);
        }
    }
}

TEST_CASE("HLE sources play available buffers without an empty startup frame",
          "[audio_core][hle][streaming]") {
    using Configuration = AudioCore::HLE::SourceConfiguration::Configuration;
    Core::System system;
    Memory::MemorySystem memory{system};
    AudioCore::HLE::Source source{0};
    source.SetMemory(memory);

    constexpr s16 amplitude = 1200;
    constexpr u32 length = 320;
    auto* samples = reinterpret_cast<s16*>(memory.GetFCRAMPointer(0));
    std::fill_n(samples, length * 2, amplitude);

    Configuration config{};
    const s16_le coefficients[16]{};
    config.gain[0][0] = 1.0f;
    config.gain[0][1] = 1.0f;
    config.gain_0_dirty.Assign(1);
    config.rate_multiplier = 1.0f;
    config.rate_multiplier_dirty.Assign(1);
    config.interpolation_mode = Configuration::InterpolationMode::None;
    config.interpolation_dirty.Assign(1);
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Stereo);
    config.mono_or_stereo_dirty.Assign(1);
    config.format.Assign(Configuration::Format::PCM16);
    config.format_dirty.Assign(1);
    source.Tick(config, coefficients);
    AudioCore::QuadFrame32 mix{};
    source.MixInto(mix, 0);

    SECTION("embedded looping buffer") {
        config.physical_address = Memory::FCRAM_PADDR;
        config.length = length;
        config.buffer_id = 1;
        config.is_looping.Assign(1);
        config.embedded_buffer_dirty.Assign(1);
    }
    SECTION("queued streaming buffers") {
        for (u32 i = 0; i < 4; ++i) {
            config.buffers[i].physical_address = Memory::FCRAM_PADDR;
            config.buffers[i].length = length;
            config.buffers[i].buffer_id = static_cast<u16>(i + 1);
        }
        config.buffers_dirty = 0xf;
        config.buffer_queue_dirty.Assign(1);
    }
    config.enable = 1;
    config.enable_dirty.Assign(1);

    for (u32 frame = 0; frame < 6; ++frame) {
        const auto status = source.Tick(config, coefficients);
        REQUIRE(status.is_enabled);
        mix = {};
        source.MixInto(mix, 0);
        for (u32 i = 0; i < AudioCore::samples_per_frame; ++i) {
            CAPTURE(frame, i);
            // The interpolator has a two-sample history delay.
            const s32 expected = frame == 0 && i < 2 ? 0 : amplitude;
            REQUIRE(mix[i][0] == expected);
            REQUIRE(mix[i][1] == expected);
        }
    }
}

TEST_CASE("Looping PCM streams stay continuous across DSP frame boundaries",
          "[audio_core][hle][streaming]") {
    using Configuration = AudioCore::HLE::SourceConfiguration::Configuration;
    Core::System system;
    Memory::MemorySystem memory{system};
    AudioCore::HLE::Source source{0};
    source.SetMemory(memory);

    const u32 length = GENERATE(320u, 2048u, 8192u);
    const float rate = GENERATE(1.0f, 44100.0f / AudioCore::native_sample_rate);
    constexpr s16 amplitude = 1200;
    auto* samples = reinterpret_cast<s16*>(memory.GetFCRAMPointer(0));
    std::fill_n(samples, length * 2, amplitude);

    Configuration config{};
    const s16_le coefficients[16]{};
    config.gain[0][0] = 1.0f;
    config.gain[0][1] = 1.0f;
    config.gain_0_dirty.Assign(1);
    config.rate_multiplier = rate;
    config.rate_multiplier_dirty.Assign(1);
    config.interpolation_mode = Configuration::InterpolationMode::None;
    config.interpolation_dirty.Assign(1);
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Stereo);
    config.mono_or_stereo_dirty.Assign(1);
    config.format.Assign(Configuration::Format::PCM16);
    config.format_dirty.Assign(1);
    // Settle the gain ramp before starting the stream.
    source.Tick(config, coefficients);
    AudioCore::QuadFrame32 mix{};
    source.MixInto(mix, 0);

    config.physical_address = Memory::FCRAM_PADDR;
    config.length = length;
    config.buffer_id = 1;
    config.is_looping.Assign(1);
    config.embedded_buffer_dirty.Assign(1);
    config.enable = 1;
    config.enable_dirty.Assign(1);

    u32 silent_frames = 0;
    u32 incorrect_samples = 0;
    for (u32 frame = 0; frame < 1024; ++frame) {
        const auto status = source.Tick(config, coefficients);
        REQUIRE(status.is_enabled);
        mix = {};
        source.MixInto(mix, 0);
        if (std::all_of(mix.begin(), mix.end(), [](const auto& sample) {
                return sample[0] == 0 && sample[1] == 0;
            })) {
            ++silent_frames;
        }
        for (u32 i = 0; i < AudioCore::samples_per_frame; ++i) {
            // Ignore only the interpolator's initial two-sample history.
            if (frame == 0 && i < 2) {
                continue;
            }
            incorrect_samples += mix[i][0] != amplitude || mix[i][1] != amplitude;
        }
    }
    CAPTURE(length, rate, silent_frames, incorrect_samples);
    REQUIRE(silent_frames == 0);
    REQUIRE(incorrect_samples == 0);
}
