#include "rt/oversampler.h"

#include <algorithm>

#include "util/exception.h"

namespace sonare::rt {
namespace {

bool is_supported_factor(int factor) {
  return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
}

}  // namespace

Oversampler::Oversampler(int factor) { set_factor(factor); }

void Oversampler::set_factor(int factor) {
  SONARE_CHECK(is_supported_factor(factor), ErrorCode::InvalidParameter);
  factor_ = factor;
  if (factor_ == 1) {
    decimation_taps_ = {1.0f};
    fir_ = {};
    return;
  }
  decimation_taps_ = design_windowed_sinc_lowpass(12 * factor_, factor_, 7.85726, true);
  fir_ = build_polyphase(decimation_taps_, factor_);
}

std::vector<float> Oversampler::upsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> output(size * static_cast<size_t>(factor_));
  upsample_to(input, size, output.data(), output.size());
  return output;
}

std::vector<float> Oversampler::upsample(const std::vector<float>& input) const {
  return upsample(input.data(), input.size());
}

void Oversampler::upsample_to(const float* input, size_t size, float* output,
                              size_t output_size) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  const size_t out_size = size * static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  for (size_t i = 0; i < size; ++i) {
    for (int phase = 0; phase < factor_; ++phase) {
      output[i * static_cast<size_t>(factor_) + static_cast<size_t>(phase)] =
          interpolate_polyphase_sample(input, size, i, phase, fir_);
    }
  }
}

std::vector<float> Oversampler::downsample(const float* input, size_t size) const {
  if (size == 0) return {};
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);

  const size_t out_size = size / static_cast<size_t>(factor_);
  std::vector<float> output(out_size);
  downsample_to(input, size, output.data(), output.size());
  return output;
}

void Oversampler::downsample_to(const float* input, size_t size, float* output,
                                size_t output_size) const {
  if (size == 0) return;
  SONARE_CHECK(input != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(output != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(size % static_cast<size_t>(factor_) == 0, ErrorCode::InvalidParameter);
  const size_t out_size = size / static_cast<size_t>(factor_);
  SONARE_CHECK(output_size >= out_size, ErrorCode::InvalidParameter);
  if (factor_ == 1) {
    std::copy_n(input, out_size, output);
    return;
  }
  const int half = static_cast<int>(decimation_taps_.size() / 2);
  for (size_t i = 0; i < out_size; ++i) {
    const long center = static_cast<long>(i * static_cast<size_t>(factor_));
    const long first = center - static_cast<long>(half);
    const long last = first + static_cast<long>(decimation_taps_.size());
    // The entire processing contract is float32. Keeping this accumulator in
    // float lets the compiler vectorize the 48-tap 4x decimation loop used by
    // the realtime true-peak limiter; double accumulation added CPU cost whose
    // extra bits were discarded immediately on store.
    float accum = 0.0f;
    // Nearly the entire block has a complete decimation kernel.  Avoid a
    // bounds comparison for every coefficient there; the boundary branch
    // below remains bit-identical for the first/last samples.
    if (first >= 0 && last <= static_cast<long>(size)) {
      const float* samples = input + first;
      for (size_t tap = 0; tap < decimation_taps_.size(); ++tap) {
        accum += samples[tap] * decimation_taps_[tap];
      }
      output[i] = accum / static_cast<float>(factor_);
      continue;
    }
    for (size_t tap = 0; tap < decimation_taps_.size(); ++tap) {
      const long src = first + static_cast<long>(tap);
      if (src < 0 || src >= static_cast<long>(size)) {
        continue;
      }
      accum += input[static_cast<size_t>(src)] * decimation_taps_[tap];
    }
    // The decimation taps share the interpolation kernel (DC gain = factor_), so
    // dividing by factor_ normalizes the decimated output to unity DC gain.
    output[i] = accum / static_cast<float>(factor_);
  }
}

std::vector<float> Oversampler::downsample(const std::vector<float>& input) const {
  return downsample(input.data(), input.size());
}

}  // namespace sonare::rt
