#include "streaming/stream_resampler.h"

#include <algorithm>
#include <cmath>

#include "CDSPResampler.h"
#include "util/exception.h"

namespace sonare::streaming_detail {

namespace {
/// Block size fed to the resampler per process() iteration. Matches the value
/// used by the one-shot core resampler so the underlying filter is built with
/// the same MaxInLen characteristics.
constexpr int kBlockSize = 1024;
}  // namespace

struct StreamResampler::Impl {
  int src_sr;
  int dst_sr;
  bool passthrough;
  std::unique_ptr<r8b::CDSPResampler24> resampler;
  std::vector<double> input_block;  ///< Reused double-precision input scratch.
  std::uint64_t total_input = 0;
  std::uint64_t total_output = 0;
  int input_latency = 0;
};

StreamResampler::StreamResampler(int src_sr, int dst_sr) : impl_(std::make_unique<Impl>()) {
  SONARE_CHECK(src_sr > 0 && dst_sr > 0, ErrorCode::InvalidParameter);
  impl_->src_sr = src_sr;
  impl_->dst_sr = dst_sr;
  impl_->passthrough = (src_sr == dst_sr);
  if (!impl_->passthrough) {
    impl_->resampler = std::make_unique<r8b::CDSPResampler24>(
        static_cast<double>(src_sr), static_cast<double>(dst_sr), kBlockSize);
    impl_->input_latency = impl_->resampler->getInputRequiredForOutput(1);
  }
  impl_->input_block.reserve(kBlockSize);
}

StreamResampler::~StreamResampler() = default;
StreamResampler::StreamResampler(StreamResampler&&) noexcept = default;
StreamResampler& StreamResampler::operator=(StreamResampler&&) noexcept = default;

void StreamResampler::process(const float* samples, size_t n_samples, std::vector<float>& out) {
  if (samples == nullptr || n_samples == 0) {
    return;
  }

  if (impl_->passthrough) {
    out.insert(out.end(), samples, samples + n_samples);
    impl_->total_input += n_samples;
    impl_->total_output += n_samples;
    return;
  }
  impl_->total_input += n_samples;

  // Feed the persistent resampler in fixed-size blocks. Because the same
  // resampler instance is reused across process() calls, its poly-phase filter
  // history carries over and chunk boundaries join seamlessly (no click, no
  // drift). Output emerges with a constant start-up latency that we do not
  // attempt to flush away — flushing with zeros is exactly what would corrupt a
  // continuous stream.
  size_t offset = 0;
  while (offset < n_samples) {
    const int block_len =
        static_cast<int>(std::min(n_samples - offset, static_cast<size_t>(kBlockSize)));

    impl_->input_block.resize(static_cast<size_t>(block_len));
    for (int i = 0; i < block_len; ++i) {
      impl_->input_block[static_cast<size_t>(i)] = static_cast<double>(samples[offset + i]);
    }

    double* output_ptr = nullptr;
    const int output_len =
        impl_->resampler->process(impl_->input_block.data(), block_len, output_ptr);

    if (output_len > 0 && output_ptr != nullptr) {
      const size_t prev = out.size();
      out.resize(prev + static_cast<size_t>(output_len));
      for (int i = 0; i < output_len; ++i) {
        out[prev + static_cast<size_t>(i)] = static_cast<float>(output_ptr[i]);
      }
      impl_->total_output += static_cast<std::uint64_t>(output_len);
    }

    offset += static_cast<size_t>(block_len);
  }
}

std::uint64_t StreamResampler::expected_output_samples() const noexcept {
  return static_cast<std::uint64_t>(std::llround(
      static_cast<long double>(impl_->total_input) * impl_->dst_sr / impl_->src_sr));
}

int StreamResampler::input_latency_samples() const noexcept {
  return impl_->input_latency;
}

void StreamResampler::flush(std::vector<float>& out) {
  const auto expected = expected_output_samples();
  if (impl_->passthrough || impl_->total_output >= expected) return;
  std::vector<float> zeros(static_cast<std::size_t>(kBlockSize), 0.0f);
  // Preserve the finite stream's analytic length while allowing r8brain's
  // internally consumed startup history to emerge. The guard is deliberately
  // bounded so corrupt rate state cannot spin forever.
  for (int guard = 0; guard < 64 && impl_->total_output < expected; ++guard) {
    const auto beforeInput = impl_->total_input;
    const auto beforeSize = out.size();
    process(zeros.data(), zeros.size(), out);
    // Zero padding is not part of the logical input duration.
    impl_->total_input = beforeInput;
    if (impl_->total_output > expected) {
      const auto excess = impl_->total_output - expected;
      if (excess <= out.size() - beforeSize) out.resize(out.size() - excess);
      impl_->total_output = expected;
    }
  }
}

void StreamResampler::reset() {
  if (impl_->resampler) {
    impl_->resampler->clear();
  }
  impl_->total_input = 0;
  impl_->total_output = 0;
}

}  // namespace sonare::streaming_detail
