#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <ATen/native/cpu/SoftmaxKernel.h>

#include <algorithm>
#include <iterator>
#include <numeric>

#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/TensorIterator.h>
#include <ATen/core/Tensor.h>
#include <ATen/cpu/vec/functional.h>
#include <ATen/cpu/vec/vec.h>
#include <c10/util/Optional.h>
#include <c10/util/irange.h>

// [Note AVX-SSE transitions] In general we avoid calls into cmath for code
// compiled with AVX/AVX2 This is because of SSE-AVX transitions and a bug in
// Glibc2.23 See https://bugs.launchpad.net/ubuntu/+source/glibc/+bug/1663280
//
// On grainsize: The grainsize is chosen to roughly get GRAIN_SIZE number of
// computations per task. Each task works across dim_size elements. 16 should be
// a very rough approximation of the number of computations per dim_size element
// by counting simple computations (*, +, -) as 1 and exp or log as 4.

namespace at { namespace native {
namespace {

template <typename scalar_t>
inline void _vec_log_softmax_lastdim(
    scalar_t* input_data_base,
    scalar_t* output_data_base,
    int64_t outer_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<vec::vec_scalar_t<scalar_t>>;
  static constexpr int64_t CHUNK_SIZE = (128 / sizeof(scalar_t)) * Vec::size();
  int64_t grain_size = internal::GRAIN_SIZE / (16 * dim_size * CHUNK_SIZE);
  if (grain_size < CHUNK_SIZE)
    grain_size = CHUNK_SIZE;

  parallel_for(
      0,
      outer_size,
      grain_size,
      [&](int64_t begin, int64_t end) {
        // NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
        scalar_t tmp_sum_scalar[CHUNK_SIZE];
        // NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
        scalar_t max_input_arr[CHUNK_SIZE];
        for (int64_t ii = begin; ii < end; ii += CHUNK_SIZE) {
          int64_t loop_end = CHUNK_SIZE;
          if (ii + CHUNK_SIZE > end)
            loop_end = end - ii;
          for (const auto j : c10::irange(loop_end)) {
            int64_t i = ii + j;
            scalar_t* input_data = input_data_base + i * dim_size;
            max_input_arr[j] = vec::reduce_all<scalar_t>(
                [](Vec& x, Vec& y) { return vec::maximum(x, y); },
                input_data,
                dim_size);
          }
          for (const auto j : c10::irange(loop_end)) {
            int64_t i = ii + j;
            scalar_t* input_data = input_data_base + i * dim_size;
            scalar_t max_input = max_input_arr[j];
            tmp_sum_scalar[j] = vec::map_reduce_all<scalar_t>(
                [max_input](Vec x) { return (x - Vec(max_input)).exp(); },
                [](Vec x, Vec y) { return x + y; },
                input_data,
                dim_size);
          }
          // See [Note AVX-SSE transitions] for why this should call the
          // vectorized version (aside from perf improvements).
          vec::map(
              [](Vec x) { return x.log(); },
              tmp_sum_scalar,
              tmp_sum_scalar,
              loop_end);
          for (const auto j : c10::irange(loop_end)) {
            int64_t i = ii + j;
            scalar_t* input_data = input_data_base + i * dim_size;
            scalar_t* output_data = output_data_base + i * dim_size;
            scalar_t tmp_sum = tmp_sum_scalar[j];
            scalar_t max_input = max_input_arr[j];

            // It's necessary to keep the order of the operations below.
            // In some cases that input is large digits and the difference
            // is small, if we compute `max_input` plus `tmp_sum` before,
            // there would be a numerical problem. See an example in
            // https://github.com/pytorch/pytorch/issues/11752#issuecomment-422883379
            vec::map(
                [tmp_sum, max_input](Vec x) { return x - Vec(max_input) - Vec(tmp_sum); },
                output_data,
                input_data,
                dim_size);
          }
        }
      });
}

template <typename scalar_t>
inline void _vec_softmax_lastdim(
    scalar_t* input_data_base,
    scalar_t* output_data_base,
    int64_t outer_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<vec::vec_scalar_t<scalar_t>>;
  int64_t grain_size = internal::GRAIN_SIZE / (16 * dim_size);
  if (grain_size < 1)
    grain_size = 1;

  parallel_for(
      0,
      outer_size,
      grain_size,
      [&](int64_t begin, int64_t end) {
        for (const auto i : c10::irange(begin, end)) {
          scalar_t* input_data = input_data_base + i * dim_size;
          scalar_t* output_data = output_data_base + i * dim_size;
          scalar_t max_input = vec::reduce_all<scalar_t>(
              [](Vec& x, Vec& y) { return vec::maximum(x, y); },
              input_data,
              dim_size);
          vec::map(
              [max_input](Vec x) { return (x - Vec(max_input)).exp(); },
              output_data,
              input_data,
              dim_size);
          scalar_t tmp_sum = vec::reduce_all<scalar_t>(
              [](Vec x, Vec y) { return x + y; }, output_data, dim_size);
          tmp_sum = 1 / tmp_sum;
          vec::map(
              [tmp_sum](Vec x) { return x * Vec(tmp_sum); },
              output_data,
              output_data,
              dim_size);
        }
      });
}

template <typename scalar_t, bool log_softmax>
inline void _vec_host_softmax_backward_lastdim(
    scalar_t* grad_input_data_base,
    scalar_t* grad_data_base,
    scalar_t* output_data_base,
    int64_t outer_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<vec::vec_scalar_t<scalar_t>>;
  int64_t grain_size = internal::GRAIN_SIZE / (16 * dim_size);
  if (grain_size < 1)
    grain_size = 1;

  parallel_for(
      0,
      outer_size,
      grain_size,
      [&](int64_t begin, int64_t end) {
        for (const auto i : c10::irange(begin, end)) {
          scalar_t* grad_input_data = grad_input_data_base + i * dim_size;
          scalar_t* grad_data = grad_data_base + i * dim_size;
          scalar_t* output_data = output_data_base + i * dim_size;
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
          scalar_t sum;
          if (log_softmax) {
            sum = vec::reduce_all<scalar_t>(
                [](Vec& x, Vec& y) { return x + y; }, grad_data, dim_size);
          } else {
            sum = vec::map2_reduce_all<scalar_t>(
                [](Vec x, Vec y) { return x * y; },
                [](Vec x, Vec y) { return x + y; },
                grad_data,
                output_data,
                dim_size);
          }
          if (log_softmax) {
            vec::map2(
                [sum](Vec x, Vec y) { return x - ((y.exp()) * Vec(sum)); },
                grad_input_data,
                grad_data,
                output_data,
                dim_size);
          } else {
            vec::map2(
                [sum](Vec x, Vec y) { return (x - Vec(sum)) * y; },
                grad_input_data,
                grad_data,
                output_data,
                dim_size);
          }
        }
      });
}

template <typename scalar_t, bool LogSoftMax>
struct vec_host_softmax_lastdim {
  static void apply(const Tensor& output, const Tensor& input) {
    int64_t outer_size = 1;
    int64_t dim_size = input.size(input.ndimension() - 1);
    for (int64_t i = 0; i < input.ndimension() - 1; ++i)
      outer_size *= input.size(i);
    scalar_t* input_data_base = input.data_ptr<scalar_t>();
    scalar_t* output_data_base = output.data_ptr<scalar_t>();
    if (LogSoftMax) {
      _vec_log_softmax_lastdim(
          input_data_base, output_data_base, outer_size, dim_size);
    } else {
      _vec_softmax_lastdim(
          input_data_base, output_data_base, outer_size, dim_size);
    }
  }
};

inline void _vec_softmax(
    BFloat16* input_data_base,
    BFloat16* output_data_base,
    int64_t outer_size,
    int64_t inner_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<float>;
  using Vec_bf16 = vec::Vectorized<BFloat16>;
  int64_t dim_stride = inner_size;
  int64_t outer_stride = dim_size * dim_stride;
  int64_t grain_size = std::min(internal::GRAIN_SIZE / dim_size, (int64_t)1);
  int vectorized_step = Vec_bf16().size(); // Currently, we only support BFloat16 in this special implementation
  parallel_for(
      0, outer_size * inner_size, grain_size, [&](int64_t begin, int64_t end) {
        int64_t idx = begin;
        std::unique_ptr<float[]> temp_vec_input(new float[dim_size*vectorized_step*2]());
        std::unique_ptr<float[]> temp_vec_output(new float[dim_size*vectorized_step*2]());
        float* temp_vec_input_data = temp_vec_input.get();
        float* temp_vec_output_data = temp_vec_output.get();
        while (idx < end) {
          int64_t outer_idx = idx / inner_size;
          int64_t inner_idx = idx % inner_size;
          if (((inner_idx + vectorized_step) <= inner_size) && ((idx + vectorized_step) <= end)) {
            // Vectorization
            BFloat16* input_data =
                input_data_base + outer_idx * outer_stride + inner_idx;
            BFloat16* output_data =
                output_data_base + outer_idx * outer_stride + inner_idx;
            // Step 1: Get max Score
            Vec_bf16 max_vec_bf16 = Vec_bf16::loadu(input_data);
            std::tuple<vec::Vectorized<float>, vec::Vectorized<float>> convert_result = convert_bfloat16_float(max_vec_bf16);
            Vec max_vec_o1 = std::get<0>(convert_result);
            Vec max_vec_o2 = std::get<1>(convert_result);
            std::get<0>(convert_result).store(temp_vec_input_data);
            std::get<1>(convert_result).store(temp_vec_input_data + vectorized_step);
            for (const auto d : c10::irange(1, dim_size)) {
              Vec_bf16 input_vec_bf16 = Vec_bf16::loadu(input_data + d * dim_stride);
              convert_result = convert_bfloat16_float(input_vec_bf16);
              max_vec_o1 = vec::maximum(max_vec_o1, std::get<0>(convert_result));
              max_vec_o2 = vec::maximum(max_vec_o2, std::get<1>(convert_result));
              std::get<0>(convert_result).store(temp_vec_input_data + d*vectorized_step*2);
              std::get<1>(convert_result).store(temp_vec_input_data + d*vectorized_step*2 + vectorized_step);
            }
            // Step2: Calculate sum
            Vec sum_vec_o1 = Vec(0.0);
            Vec sum_vec_o2 = Vec(0.0);
            for (const auto d : c10::irange(dim_size)) {
              Vec output_vec_o1 = Vec::loadu(temp_vec_input_data + d*vectorized_step*2);
              Vec output_vec_o2 = Vec::loadu(temp_vec_input_data + d*vectorized_step*2 + vectorized_step);
              output_vec_o1 = (output_vec_o1 - max_vec_o1).exp();
              output_vec_o2 = (output_vec_o2 - max_vec_o2).exp();
              output_vec_o1.store(temp_vec_output_data + d*vectorized_step*2);
              output_vec_o2.store(temp_vec_output_data + d*vectorized_step*2 + vectorized_step);

              sum_vec_o1 = sum_vec_o1 + output_vec_o1;
              sum_vec_o2 = sum_vec_o2 + output_vec_o2;
            }
            // Step3: Unify
            for (const auto d : c10::irange(dim_size)) {
              Vec output_vec_o1 = Vec::loadu(temp_vec_output_data + d*vectorized_step*2);
              Vec output_vec_o2 = Vec::loadu(temp_vec_output_data + d*vectorized_step*2 + vectorized_step);
              output_vec_o1 = output_vec_o1/sum_vec_o1;
              output_vec_o2 = output_vec_o2/sum_vec_o2;
              Vec_bf16 output_vec_bf16 = convert_float_bfloat16(output_vec_o1, output_vec_o2);
              output_vec_bf16.store(output_data + d * dim_stride);
            }
            idx += vectorized_step;
          } else {
            // Tail case(Scalar): it is exactly same logic as host_softmax
            // inside aten/src/ATen/native/SoftMax.cpp. There are 2 kind of
            // cases which will fall through this part:
            // Case 1: For the idx at the end of total chunk for each thread, there are not enough numbers for parallization.
            // Case 2: For the idx at the end of each inner_size inside thread, there are not enough numbers for parallization.
            int64_t tail_number = ((idx+vectorized_step) > end) ? /*Case1*/ (end - idx) : /*Case2*/ (inner_size - inner_idx);
            for (const auto i : c10::irange(tail_number)) {
              outer_idx = (idx + i) / inner_size;
              inner_idx = (idx + i) % inner_size;
              BFloat16* input_data =
                  input_data_base + outer_idx * outer_stride + inner_idx;
              BFloat16* output_data =
                  output_data_base + outer_idx * outer_stride + inner_idx;
              // Step1: Get max score
              float max_input = float(input_data[0]);
              for (const auto d : c10::irange(1, dim_size)) {
                max_input = std::max(max_input, float(input_data[d * dim_stride]));
              }
              // Step2: Calculate the Sum
              float sum_data = 0.0;
              float temp_output_data = 0.0;
              for (const auto d : c10::irange(dim_size)) {
                temp_output_data = std::exp(input_data[d * dim_stride] - max_input);
                sum_data += temp_output_data;
                output_data[d * dim_stride] = c10::BFloat16(temp_output_data);
              }
              // Step3: Unify
              for (const auto d : c10::irange(dim_size)) {
                output_data[d * dim_stride] =
                    c10::BFloat16(float(output_data[d * dim_stride])/sum_data);
              }
            }
            idx += tail_number;
          }
        }
      });
}

template <typename scalar_t>
inline void _vec_softmax(
    scalar_t* input_data_base,
    scalar_t* output_data_base,
    int64_t outer_size,
    int64_t inner_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<scalar_t>;
  int64_t dim_stride = inner_size;
  int64_t outer_stride = dim_size * dim_stride;
  int64_t grain_size = std::min(internal::GRAIN_SIZE / dim_size, (int64_t)1);
  int vectorized_step = Vec().size();
  parallel_for(
      0, outer_size * inner_size, grain_size, [&](int64_t begin, int64_t end) {
        int64_t idx = begin;
        while (idx < end) {
          int64_t outer_idx = idx / inner_size;
          int64_t inner_idx = idx % inner_size;
          if (((inner_idx + vectorized_step) <= inner_size) && ((idx + vectorized_step) <= end)) {
            // Vectorization
            scalar_t* input_data =
                input_data_base + outer_idx * outer_stride + inner_idx;
            scalar_t* output_data =
                output_data_base + outer_idx * outer_stride + inner_idx;
            // Step 1: Get max Score
            Vec max_vec = Vec::loadu(input_data);
            for (const auto d : c10::irange(1, dim_size)) {
              Vec input_vec = Vec::loadu(input_data + d * dim_stride);
              max_vec = vec::maximum(max_vec, input_vec);
            }
            // Step2: Calculate sum
            Vec sum_vec = Vec(0.0);
            for (const auto d : c10::irange(dim_size)) {
              Vec output_vec =
                  (Vec::loadu(input_data + d * dim_stride) - max_vec).exp();
              output_vec.store(output_data + d * dim_stride);
              sum_vec = sum_vec + output_vec;
            }
            // Step3: Unify
            for (const auto d : c10::irange(dim_size)) {
              Vec output_vec =
                  Vec::loadu(output_data + d * dim_stride) / sum_vec;
              output_vec.store(output_data + d * dim_stride);
            }
            idx += vectorized_step;
          } else {
            // Tail case(Scalar): it is exactly same logic as host_softmax
            // inside aten/src/ATen/native/SoftMax.cpp. There are 2 kind of
            // cases which will fall through this part:
            // Case 1: For the idx at the end of total chunk for each thread, there are not enough numbers for parallization.
            // Case 2: For the idx at the end of each inner_size inside thread, there are not enough numbers for parallization.
            int64_t tail_number = ((idx+vectorized_step) > end) ? /*Case1*/ (end - idx) : /*Case2*/ (inner_size - inner_idx);
            for (const auto i : c10::irange(tail_number)) {
              outer_idx = (idx + i) / inner_size;
              inner_idx = (idx + i) % inner_size;
              scalar_t* input_data =
                  input_data_base + outer_idx * outer_stride + inner_idx;
              scalar_t* output_data =
                  output_data_base + outer_idx * outer_stride + inner_idx;
              // Step1: Get max score
              scalar_t max_input = input_data[0];
              for (const auto d : c10::irange(1, dim_size)) {
                max_input = std::max(max_input, input_data[d * dim_stride]);
              }
              // Step2: Calculate the Sum
              scalar_t sum_data = 0;
              for (const auto d : c10::irange(dim_size)) {
                output_data[d * dim_stride] =
                    std::exp(input_data[d * dim_stride] - max_input);
                sum_data += output_data[d * dim_stride];
              }
              // Step3: Unify
              for (const auto d : c10::irange(dim_size)) {
                output_data[d * dim_stride] =
                    output_data[d * dim_stride]/sum_data;
              }
            }
            idx += tail_number;
          }
        }
      });
}

// NB: fast kernel for log_softmax when dim != -1
// input shape is normalized to {outer_size, dim_size, inner_size}
//
// The algorithm requires to load input tensor 3 times, to increase parallelsim
// and cache hit rate, inner_size is blocked as:
//   inner_size: {CHUNK_SIZE, CHUNK_SIZE, ..., Remainder}
//
// Parallel on {outer_size, num_chunks} and do vertical reduction on each block of
// {dim_size, CHUNK_SIZE}, block size (128KB) selected to be L2 hit.
//
template <typename scalar_t>
inline void _vec_logsoftmax(
    scalar_t* input_data_base,
    scalar_t* output_data_base,
    int64_t outer_size,
    int64_t inner_size,
    int64_t dim_size) {
  using Vec = vec::Vectorized<scalar_t>;
  int64_t BLOCK_SIZE = 128 * 1024;
  int64_t CHUNK_SIZE = std::max(int64_t(BLOCK_SIZE / dim_size / sizeof(scalar_t)), (int64_t) Vec::size());
  CHUNK_SIZE = CHUNK_SIZE / Vec::size() * Vec::size();
  int64_t num_chunks = divup(inner_size, CHUNK_SIZE);

  int64_t grain_size = internal::GRAIN_SIZE / (16 * dim_size * CHUNK_SIZE);
  at::parallel_for(0, outer_size * num_chunks, grain_size, [&](int64_t begin, int64_t end) {
    // thread local temp buffer which holds vertical reduction result: max and sum.
    std::unique_ptr<scalar_t []> buffer(new scalar_t[CHUNK_SIZE * 2]);
    scalar_t* input_max_data = buffer.get();
    scalar_t* tmp_sum_data = buffer.get() + CHUNK_SIZE;

    for (int64_t i = begin; i < end; i++) {
      int64_t outer_idx = i / num_chunks;
      int64_t k = i % num_chunks;
      int64_t inner_idx_begin = k * CHUNK_SIZE;
      int64_t size = std::min(CHUNK_SIZE, inner_size - inner_idx_begin);

      // init
      Vec zero_vec = Vec(scalar_t(0));
      Vec min_vec = Vec(-std::numeric_limits<scalar_t>::infinity());
      int64_t d0 = 0;
      for (; d0 < size - (size % Vec::size()); d0 += Vec::size()) {
        min_vec.store(input_max_data + d0);
        zero_vec.store(tmp_sum_data + d0);
      }
      for (; d0 < size; d0++) {
        input_max_data[d0] = -std::numeric_limits<scalar_t>::infinity();
        tmp_sum_data[d0] = scalar_t(0);
      }

      // compute max
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        scalar_t* input_ptr = input_data_base + outer_idx * dim_size * inner_size
            + dim_idx * inner_size + inner_idx_begin;

        int64_t d1 = 0;
        for (; d1 < size - (size % Vec::size()); d1 += Vec::size()) {
          Vec data_vec = Vec::loadu(input_ptr + d1);
          Vec max_vec = Vec::loadu(input_max_data + d1);
          max_vec = Vec::blendv(max_vec, data_vec, data_vec > max_vec);
          max_vec.store(input_max_data + d1);
        }
        for (; d1 < size; d1++) {
          scalar_t data_val = input_ptr[d1];
          scalar_t max_val = input_max_data[d1];
          input_max_data[d1] = data_val > max_val ? data_val : max_val;
        }
      }

      // compute sum of (x - max).exp()
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        scalar_t* input_ptr = input_data_base + outer_idx * dim_size * inner_size
            + dim_idx * inner_size + inner_idx_begin;

        int64_t d2 = 0;
        for (; d2 < size - (size % Vec::size()); d2 += Vec::size()) {
          Vec data_vec = Vec::loadu(input_ptr + d2);
          Vec sum_vec = Vec::loadu(tmp_sum_data + d2);
          Vec max_vec = Vec::loadu(input_max_data + d2);
          sum_vec += (data_vec - max_vec).exp();
          sum_vec.store(tmp_sum_data + d2);
        }
        for (; d2 < size; d2++) {
          scalar_t data_val = input_ptr[d2];
          scalar_t max_val = input_max_data[d2];
          tmp_sum_data[d2] += std::exp(data_val - max_val);
        }
      }

      // apply log
      vec::map([](Vec x) { return x.log(); }, tmp_sum_data, tmp_sum_data, size);

      // compute x - max - sum
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        int64_t offset = outer_idx * dim_size * inner_size + dim_idx * inner_size + inner_idx_begin;
        scalar_t* input_ptr = input_data_base + offset;
        scalar_t* output_ptr = output_data_base + offset;

        int64_t d3 = 0;
        for (; d3 < size - (size % Vec::size()); d3 += Vec::size()) {
          Vec data_vec = Vec::loadu(input_ptr + d3);
          Vec max_vec = Vec::loadu(input_max_data + d3);
          Vec sum_vec = Vec::loadu(tmp_sum_data + d3);
          Vec out_vec = data_vec - max_vec - sum_vec;
          out_vec.store(output_ptr + d3);
        }
        for (; d3 < size; d3++) {
          output_ptr[d3] = input_ptr[d3] - input_max_data[d3] - tmp_sum_data[d3];
        }
      }
    }
  });
}

template <>
inline void _vec_logsoftmax<BFloat16>(
    BFloat16* input_data_base,
    BFloat16* output_data_base,
    int64_t outer_size,
    int64_t inner_size,
    int64_t dim_size) {
  using bVec = vec::Vectorized<BFloat16>;
  using fVec = vec::Vectorized<float>;
  int64_t BLOCK_SIZE = 128 * 1024;
  int64_t CHUNK_SIZE = std::max(int64_t(BLOCK_SIZE / dim_size / sizeof(BFloat16)), (int64_t) bVec::size());
  CHUNK_SIZE = CHUNK_SIZE / bVec::size() * bVec::size();
  int64_t num_chunks = divup(inner_size, CHUNK_SIZE);

  int64_t grain_size = internal::GRAIN_SIZE / (16 * dim_size * CHUNK_SIZE);
  at::parallel_for(0, outer_size * num_chunks, grain_size, [&](int64_t begin, int64_t end) {
    std::unique_ptr<float []> buffer(new float[CHUNK_SIZE * 2]);
    float* input_max_data = buffer.get();
    float* tmp_sum_data = buffer.get() + CHUNK_SIZE;

    // thread local buffer that holds input data in float32 to save next 2 dtype conversion
    std::unique_ptr<float []> input_buffer(new float[dim_size * CHUNK_SIZE]);
    float* input_buffer_data = input_buffer.get();

    // init
    for (int64_t i = begin; i < end; i++) {
      int64_t outer_idx = i / num_chunks;
      int64_t k = i % num_chunks;
      int64_t inner_idx_begin = k * CHUNK_SIZE;
      int64_t size = std::min(CHUNK_SIZE, inner_size - inner_idx_begin);

      fVec zero_fvec = fVec(float(0));
      fVec min_fvec = fVec(-std::numeric_limits<float>::infinity());
      int64_t d0 = 0;
      for (; d0 < size - (size % bVec::size()); d0 += bVec::size()) {
        min_fvec.store(input_max_data + d0);
        min_fvec.store(input_max_data + d0 + fVec::size());
        zero_fvec.store(tmp_sum_data + d0);
        zero_fvec.store(tmp_sum_data + d0 + fVec::size());
      }
      for (; d0 < size; d0++) {
        input_max_data[d0] = -std::numeric_limits<float>::infinity();
        tmp_sum_data[d0] = float(0);
      }

      // compute max
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        BFloat16* input_ptr = input_data_base + outer_idx * dim_size * inner_size
            + dim_idx * inner_size + inner_idx_begin;
        float* input_buffer_ptr = input_buffer_data + dim_idx * CHUNK_SIZE;

        int64_t d1 = 0;
        for (; d1 < size - (size % bVec::size()); d1 += bVec::size()) {
          bVec data_bvec = bVec::loadu(input_ptr + d1);
          fVec data_fvec0, data_fvec1;
          std::tie(data_fvec0, data_fvec1) = convert_bfloat16_float(data_bvec);
          fVec max_fvec0 = fVec::loadu(input_max_data + d1);
          fVec max_fvec1 = fVec::loadu(input_max_data + d1 + fVec::size());
          max_fvec0 = fVec::blendv(max_fvec0, data_fvec0, data_fvec0 > max_fvec0);
          max_fvec1 = fVec::blendv(max_fvec1, data_fvec1, data_fvec1 > max_fvec1);
          max_fvec0.store(input_max_data + d1);
          max_fvec0.store(input_max_data + d1 + fVec::size());

          // cache the 'converted' float input
          data_fvec0.store(input_buffer_ptr + d1);
          data_fvec1.store(input_buffer_ptr + d1 + fVec::size());
        }
        for (; d1 < size; d1++) {
          float data_val = float(input_ptr[d1]);
          float max_val = input_max_data[d1];
          input_max_data[d1] = data_val > max_val ? data_val : max_val;
          input_buffer_ptr[d1] = data_val;
        }
      }

      // compute sum of (x - max).exp()
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        float* input_buffer_ptr = input_buffer_data + dim_idx * CHUNK_SIZE;

        int64_t d2 = 0;
        for (; d2 < size - (size % bVec::size()); d2 += bVec::size()) {
          fVec data_fvec0 = fVec::loadu(input_buffer_ptr + d2);
          fVec data_fvec1 = fVec::loadu(input_buffer_ptr + d2 + fVec::size());
          fVec sum_fvec0 = fVec::loadu(tmp_sum_data + d2);
          fVec sum_fvec1 = fVec::loadu(tmp_sum_data + d2 + fVec::size());
          fVec max_fvec0 = fVec::loadu(input_max_data + d2);
          fVec max_fvec1 = fVec::loadu(input_max_data + d2 + fVec::size());
          sum_fvec0 += (data_fvec0 - max_fvec0).exp();
          sum_fvec1 += (data_fvec1 - max_fvec1).exp();
          sum_fvec0.store(tmp_sum_data + d2);
          sum_fvec1.store(tmp_sum_data + d2 + fVec::size());
        }
        for (; d2 < size; d2++) {
          float data_val = input_buffer_ptr[d2];
          float max_val = input_max_data[d2];
          tmp_sum_data[d2] += std::exp(data_val - max_val);
        }
      }

      // apply log
      vec::map([](fVec x) { return x.log(); }, tmp_sum_data, tmp_sum_data, size);

      // compute x - max - sum
      for (int64_t dim_idx = 0; dim_idx < dim_size; dim_idx++) {
        float* input_buffer_ptr = input_buffer_data + dim_idx * CHUNK_SIZE;
        BFloat16* output_ptr = output_data_base + outer_idx * dim_size * inner_size
            + dim_idx * inner_size + inner_idx_begin;

        int64_t d3 = 0;
        for (; d3 < size - (size % bVec::size()); d3 += bVec::size()) {
          fVec data_fvec0 = fVec::loadu(input_buffer_ptr + d3);
          fVec data_fvec1 = fVec::loadu(input_buffer_ptr + d3 + fVec::size());
          fVec max_fvec0 = fVec::loadu(input_max_data + d3);
          fVec max_fvec1 = fVec::loadu(input_max_data + d3 + fVec::size());
          fVec sum_fvec0 = fVec::loadu(tmp_sum_data + d3);
          fVec sum_fvec1 = fVec::loadu(tmp_sum_data + d3 + fVec::size());
          fVec out_fvec0 = data_fvec0 - max_fvec0 - sum_fvec0;
          fVec out_fvec1 = data_fvec1 - max_fvec1 - sum_fvec1;
          bVec out_bvec = convert_float_bfloat16(out_fvec0, out_fvec1);
          out_bvec.store(output_ptr + d3);
        }
        for (; d3 < size; d3++) {
          output_ptr[d3] = BFloat16(input_buffer_ptr[d3] - input_max_data[d3] - tmp_sum_data[d3]);
        }
      }
    }
  });
}

template <typename scalar_t, bool LogSoftMax>
struct vec_softmax {
  static void apply(const Tensor& output, const Tensor& input, int64_t dim) {
    int64_t outer_size = 1;
    int64_t dim_size = input.size(dim);
    int64_t inner_size = 1;
    for (const auto i : c10::irange(dim))outer_size *= input.size(i);
    for (int64_t i = dim + 1; i < input.dim(); ++i)
      inner_size *= input.size(i);
    scalar_t* input_data_base = input.data_ptr<scalar_t>();
    scalar_t* output_data_base = output.data_ptr<scalar_t>();
    if (LogSoftMax) {
      _vec_logsoftmax(
          input_data_base, output_data_base, outer_size, inner_size, dim_size);
    } else {
      _vec_softmax(
          input_data_base, output_data_base, outer_size, inner_size, dim_size);
    }
  }
};

template <typename scalar_t, bool LogSoftMax>
struct vec_host_softmax_backward_lastdim {
  static void
  apply(const Tensor& grad_input, const Tensor& grad, const Tensor& output) {
    int64_t outer_size = 1;
    int64_t dim_size = grad.size(grad.ndimension() - 1);
    for (int64_t i = 0; i < grad.ndimension() - 1; ++i)
      outer_size *= grad.size(i);
    scalar_t* grad_input_data_base = grad_input.data_ptr<scalar_t>();
    scalar_t* grad_data_base = grad.data_ptr<scalar_t>();
    scalar_t* output_data_base = output.data_ptr<scalar_t>();
    _vec_host_softmax_backward_lastdim<scalar_t, LogSoftMax>(
        grad_input_data_base,
        grad_data_base,
        output_data_base,
        outer_size,
        dim_size);
  }
};

static void softmax_lastdim_kernel_impl(
    const Tensor& result,
    const Tensor& self) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, self.scalar_type(),
      "softmax_lastdim_kernel_impl",
      [&] { vec_host_softmax_lastdim<scalar_t, false>::apply(result, self); });
}

static void softmax_kernel_impl(const Tensor& result, const Tensor& self, int64_t dim) {
  AT_DISPATCH_FLOATING_TYPES_AND(at::ScalarType::BFloat16, self.scalar_type(),
    "softmax_kernel_impl",
    [&] { vec_softmax<scalar_t, false>::apply(result, self, dim); });
}

static void log_softmax_lastdim_kernel_impl(
    const Tensor& result,
    const Tensor& self) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, self.scalar_type(),
      "log_softmax_lastdim_kernel_impl",
      [&] { vec_host_softmax_lastdim<scalar_t, true>::apply(result, self); });
}

static void log_softmax_kernel_impl(const Tensor& result, const Tensor& self, int64_t dim) {
  AT_DISPATCH_FLOATING_TYPES_AND(at::ScalarType::BFloat16, self.scalar_type(),
    "softmax_kernel_impl",
    [&] { vec_softmax<scalar_t, true>::apply(result, self, dim); });
}

static void softmax_backward_lastdim_kernel_impl(
    const Tensor& grad_input,
    const Tensor& grad,
    const Tensor& output) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, grad.scalar_type(),
      "softmax_backward_lastdim_kernel_impl", [&] {
        vec_host_softmax_backward_lastdim<scalar_t, false>::apply(
            grad_input, grad, output);
      });
}

static void log_softmax_backward_lastdim_kernel_impl(
    const Tensor& grad_input,
    const Tensor& grad,
    const Tensor& output) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, grad.scalar_type(),
      "log_softmax_backward_lastdim_kernel_impl", [&] {
        vec_host_softmax_backward_lastdim<scalar_t, true>::apply(
            grad_input, grad, output);
      });
}

} // anonymous namespace

REGISTER_DISPATCH(softmax_lastdim_kernel, &softmax_lastdim_kernel_impl);
REGISTER_DISPATCH(log_softmax_lastdim_kernel, &log_softmax_lastdim_kernel_impl);
REGISTER_DISPATCH(
    softmax_backward_lastdim_kernel,
    &softmax_backward_lastdim_kernel_impl);
REGISTER_DISPATCH(
    log_softmax_backward_lastdim_kernel,
    &log_softmax_backward_lastdim_kernel_impl);

REGISTER_DISPATCH(softmax_kernel, &softmax_kernel_impl);
REGISTER_DISPATCH(log_softmax_kernel, &log_softmax_kernel_impl);

}} // namespace at::native
