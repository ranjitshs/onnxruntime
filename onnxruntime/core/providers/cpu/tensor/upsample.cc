// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cpu/tensor/upsample.h"

#include <limits>

#include "core/common/inlined_containers.h"
#include "core/common/safeint.h"
#include "core/platform/threadpool.h"
#include "core/providers/cpu/tensor/upsample_antialias.h"

using namespace onnxruntime::common;
using namespace std;
using onnxruntime::narrow;
namespace onnxruntime {

#define REGISTER_VERSIONED_TYPED_KERNEL(T, start, end)                          \
  ONNX_CPU_OPERATOR_VERSIONED_TYPED_KERNEL(                                     \
      Upsample,                                                                 \
      start,                                                                    \
      end,                                                                      \
      T,                                                                        \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      Upsample<T>)

REGISTER_VERSIONED_TYPED_KERNEL(float, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(int32_t, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(int8_t, 7, 8);
REGISTER_VERSIONED_TYPED_KERNEL(uint8_t, 7, 8);

// Upsample was deprecated in opset 10
REGISTER_VERSIONED_TYPED_KERNEL(float, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(int32_t, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(int8_t, 9, 9);
REGISTER_VERSIONED_TYPED_KERNEL(uint8_t, 9, 9);

void UpsampleBase::AdjustOutputSizeAsPolicy(TensorShapeVector& output_dims, gsl::span<const int64_t> input_dims,
                                            InlinedVector<float>& scales) const {
  // AspectRatioPolicy::STRETCH is default policy when opset < 18
  if (keep_aspect_ratio_policy_ == AspectRatioPolicy::STRETCH) {
    return;
  }

  InlinedHashSet<int64_t> axes_set(axes_.begin(), axes_.end());

  float scale_in_policy = 0.0f;
  if (keep_aspect_ratio_policy_ == AspectRatioPolicy ::NOT_LARGER) {
    scale_in_policy = std::numeric_limits<float>::max();

    for (size_t i = 0; i < scales.size(); i++) {
      if (axes_set.empty() || axes_set.count(i) > 0) {
        scale_in_policy = std::min(scale_in_policy, scales[i]);
      }
    }
  } else if (keep_aspect_ratio_policy_ == AspectRatioPolicy ::NOT_SMALLER) {
    scale_in_policy = std::numeric_limits<float>::min();

    for (size_t i = 0; i < scales.size(); i++) {
      if (axes_set.empty() || axes_set.count(i) > 0) {
        scale_in_policy = std::max(scale_in_policy, scales[i]);
      }
    }
  }

  for (size_t i = 0; i < scales.size(); i++) {
    // if axes is not specified (AKA axes_set.empty()), we apply the policy to all axes
    if (axes_set.empty() || axes_set.count(i) > 0) {
      scales[i] = scale_in_policy;
      output_dims[i] = static_cast<int64_t>(std::round(scales[i] * input_dims[i]));
    } else {
      scales[i] = 1.0f;
      output_dims[i] = input_dims[i];
    }
  }
}

template <typename T>
void UpsampleNearest2x(int64_t batch_size,
                       int64_t num_channels,
                       int64_t input_height,
                       int64_t input_width,
                       const T* input,
                       T* output) {
  const int64_t output_height = input_height * 2;
  const int64_t output_width = input_width * 2;
  for (int64_t n = 0; n < batch_size; ++n) {
    for (int64_t c = 0; c < num_channels; ++c) {
      for (int64_t y = 0; y < output_height; ++y) {
        const int64_t in_y = y / 2;
        for (int64_t x = 0; x < input_width; ++x) {
          const T v = input[in_y * input_width + x];
          const int64_t oidx = output_width * y + x * 2;
          output[oidx + 0] = v;
          output[oidx + 1] = v;
        }
      }

      input += input_height * input_width;
      output += output_height * output_width;
    }
  }
}

static std::vector<int64_t> UpsampleNearestSetupRank1InputMapping(
    int64_t length_original,
    int64_t length_resized,
    float x_scale,
    float roi_start,
    float roi_end,
    bool extrapolation_enabled,
    const GetOriginalCoordinateFunc& get_original_coordinate,
    const GetNearestPixelFunc& get_nearest_pixel) {
  std::vector<int64_t> input_mapping(onnxruntime::narrow<size_t>(length_resized));

  for (int64_t output_dim0_idx = 0; output_dim0_idx < length_resized; ++output_dim0_idx) {
    float original_0_idx = get_original_coordinate(static_cast<float>(output_dim0_idx),
                                                   x_scale,
                                                   static_cast<float>(length_resized),
                                                   static_cast<float>(length_original),
                                                   roi_start, roi_end);
    int64_t input_dim0_idx = -1;
    if (extrapolation_enabled && (original_0_idx < 0 || original_0_idx > length_original - 1)) {
      // leave as -1 to indicate the extrapolation value should be used
    } else {
      input_dim0_idx = get_nearest_pixel(original_0_idx, x_scale < 1);
      if (input_dim0_idx > length_original - 1) input_dim0_idx = length_original - 1;
      if (input_dim0_idx < 0) input_dim0_idx = 0;
    }

    input_mapping[narrow<size_t>(output_dim0_idx)] = input_dim0_idx;
  }

  return input_mapping;
}

static std::vector<std::vector<int64_t>>
UpsampleNearestSetupInputMappings(int64_t n_dim,
                                  const TensorShape& input_shape,
                                  const TensorShape& output_shape,
                                  const std::vector<int64_t>& input_dim_factor,
                                  gsl::span<const float> scales,
                                  gsl::span<const float> roi,
                                  bool extrapolation_enabled,
                                  const GetOriginalCoordinateFunc& get_original_coordinate,
                                  const GetNearestPixelFunc& get_nearest_pixel) {
  std::vector<std::vector<int64_t>> input_mappings(narrow<size_t>(n_dim));

  for (int64_t axis = 0; axis < n_dim; ++axis) {
    std::vector<int64_t>& input_mapping = input_mappings[narrow<size_t>(axis)];
    input_mapping.resize(narrow<size_t>(output_shape[narrow<size_t>(axis)]));

    // When scale is 1.0, there is a one-to-one mapping between the dimension
    // in the input and the output and there is no need to apply the co-ordinate
    // transformation which should only be done when there is "resizing" required
    if (scales[narrow<size_t>(axis)] == 1.0f) {
      for (int64_t dim = 0; dim < output_shape[narrow<size_t>(axis)]; dim++) {
        input_mapping[narrow<size_t>(dim)] = dim * input_dim_factor[narrow<size_t>(axis)];
      }
      continue;
    }

    // scale != 1.0
    const int64_t input_size = input_dim_factor[0] * input_shape[0];
    for (int64_t dim = 0; dim < output_shape[narrow<size_t>(axis)]; dim++) {
      float original_dim = get_original_coordinate(static_cast<float>(dim),
                                                   scales[narrow<size_t>(axis)],
                                                   static_cast<float>(output_shape[narrow<size_t>(axis)]),
                                                   static_cast<float>(input_shape[narrow<size_t>(axis)]),
                                                   roi[narrow<size_t>(axis)], roi[SafeInt<size_t>(n_dim) + axis]);

      bool need_extrapolation = (extrapolation_enabled && (original_dim < 0 || original_dim > input_shape[narrow<size_t>(axis)] - 1));
      int64_t input_dim = get_nearest_pixel(original_dim, scales[narrow<size_t>(axis)] < 1);
      if (input_dim >= input_shape[narrow<size_t>(axis)]) input_dim = input_shape[narrow<size_t>(axis)] - 1;
      if (input_dim < 0) input_dim = 0;

      input_mapping[narrow<size_t>(dim)] = need_extrapolation ? (-input_size) : (input_dim * input_dim_factor[narrow<size_t>(axis)]);
    }
  }

  return input_mappings;
};

template <typename T>
static Status UpsampleNearestImpl(const T* input,
                                  T* output,
                                  const TensorShape& input_shape,
                                  const TensorShape& output_shape,
                                  gsl::span<const float> scales,
                                  gsl::span<const float> roi,
                                  bool extrapolation_enabled,
                                  const T extrapolation_value,
                                  const GetOriginalCoordinateFunc& get_original_coordinate,
                                  const GetNearestPixelFunc& get_nearest_pixel) {
  int64_t n_dim = static_cast<int64_t>(input_shape.NumDimensions());

  std::vector<int64_t> input_dim_counters(narrow<size_t>(n_dim));
  std::vector<int64_t> input_dim_factor(narrow<size_t>(n_dim));
  input_dim_factor[SafeInt<size_t>(n_dim) - 1] = 1;  // initialize dimension factor
  for (int64_t dim_idx = n_dim - 2; dim_idx >= 0; dim_idx--) {
    input_dim_factor[narrow<size_t>(dim_idx)] = input_dim_factor[SafeInt<size_t>(dim_idx) + 1] * input_shape[SafeInt<size_t>(dim_idx) + 1];
  }

  int64_t output_idx = 0;
  int64_t input_idx = 0;

  if (n_dim == 1) {
    std::vector<int64_t> input_mapping = UpsampleNearestSetupRank1InputMapping(input_shape[0],
                                                                               output_shape[0],
                                                                               scales[0],
                                                                               roi[0], roi[narrow<size_t>(n_dim + 0)],
                                                                               extrapolation_enabled,
                                                                               get_original_coordinate,
                                                                               get_nearest_pixel);

    for (int64_t output_dim0_idx = 0; output_dim0_idx < output_shape[0]; output_dim0_idx++) {
      int64_t input_dim0_idx = input_mapping[narrow<size_t>(output_dim0_idx)];
      output[narrow<size_t>(output_dim0_idx)] = input_dim0_idx < 0 ? extrapolation_value : input[input_dim0_idx];
    }

    return Status::OK();
  }

  std::vector<std::vector<int64_t>> input_mappings =
      UpsampleNearestSetupInputMappings(n_dim, input_shape, output_shape, input_dim_factor, scales, roi,
                                        extrapolation_enabled, get_original_coordinate, get_nearest_pixel);

  if (n_dim == 2) {
    const std::vector<int64_t>& input_mapping_0 = input_mappings[0];
    const std::vector<int64_t>& input_mapping_1 = input_mappings[1];

    for (int64_t output_dim0_inx = 0; output_dim0_inx < output_shape[0]; output_dim0_inx++) {
      int64_t input_idx_0 = input_mapping_0[narrow<size_t>(output_dim0_inx)];
      for (int64_t output_dim1_inx = 0; output_dim1_inx < output_shape[1]; output_dim1_inx++) {
        int64_t input_idx_1 = input_idx_0 + input_mapping_1[narrow<size_t>(output_dim1_inx)];
        output[output_idx++] = (input_idx_1 < 0) ? extrapolation_value : input[input_idx_1];
      }
    }
    return Status::OK();
  }

  if (n_dim == 3) {
    const std::vector<int64_t>& input_mapping_0 = input_mappings[0];
    const std::vector<int64_t>& input_mapping_1 = input_mappings[1];
    const std::vector<int64_t>& input_mapping_2 = input_mappings[2];

    for (int64_t output_dim0_inx = 0; output_dim0_inx < output_shape[0]; output_dim0_inx++) {
      int64_t input_idx_0 = input_mapping_0[narrow<size_t>(output_dim0_inx)];
      for (int64_t output_dim1_inx = 0; output_dim1_inx < output_shape[1]; output_dim1_inx++) {
        int64_t input_idx_1 = input_idx_0 + input_mapping_1[narrow<size_t>(output_dim1_inx)];
        for (int64_t output_dim2_inx = 0; output_dim2_inx < output_shape[2]; output_dim2_inx++) {
          int64_t input_idx_2 = input_idx_1 + input_mapping_2[narrow<size_t>(output_dim2_inx)];
          output[output_idx++] = (input_idx_2 < 0) ? extrapolation_value : input[input_idx_2];
        }
      }
    }
    return Status::OK();
  }

  if (n_dim == 4) {
    const std::vector<int64_t>& input_mapping_0 = input_mappings[0];
    const std::vector<int64_t>& input_mapping_1 = input_mappings[1];
    const std::vector<int64_t>& input_mapping_2 = input_mappings[2];
    const std::vector<int64_t>& input_mapping_3 = input_mappings[3];

    for (int64_t output_dim0_inx = 0; output_dim0_inx < output_shape[0]; output_dim0_inx++) {
      int64_t input_idx_0 = input_mapping_0[narrow<size_t>(output_dim0_inx)];
      for (int64_t output_dim1_inx = 0; output_dim1_inx < output_shape[1]; output_dim1_inx++) {
        int64_t input_idx_1 = input_idx_0 + input_mapping_1[narrow<size_t>(output_dim1_inx)];
        for (int64_t output_dim2_inx = 0; output_dim2_inx < output_shape[2]; output_dim2_inx++) {
          int64_t input_idx_2 = input_idx_1 + input_mapping_2[narrow<size_t>(output_dim2_inx)];
          for (int64_t output_dim3_inx = 0; output_dim3_inx < output_shape[3]; output_dim3_inx++) {
            int64_t input_idx_3 = input_idx_2 + input_mapping_3[narrow<size_t>(output_dim3_inx)];
            output[output_idx++] = (input_idx_3 < 0) ? static_cast<T>(extrapolation_value) : input[narrow<size_t>(input_idx_3)];
          }
        }
      }
    }
    return Status::OK();
  }

  std::vector<int64_t> output_dim_counter(onnxruntime::narrow<size_t>(n_dim));
  for (int64_t dim_idx = 0; dim_idx < n_dim; dim_idx++) {
    input_idx += input_mappings[narrow<size_t>(dim_idx)][0 /* output_dim_counter[narrow<size_t>(dim_idx)] */];
  }

  for (int64_t output_size = output_shape.Size(); output_idx < output_size; output_idx++) {
    output[narrow<size_t>(output_idx)] = (input_idx < 0) ? extrapolation_value : input[narrow<size_t>(input_idx)];

    for (int64_t dim_idx = n_dim - 1; dim_idx >= 0; dim_idx--) {
      input_idx -= input_mappings[narrow<size_t>(dim_idx)][narrow<size_t>(output_dim_counter[narrow<size_t>(dim_idx)])];
      if (++output_dim_counter[narrow<size_t>(dim_idx)] < output_shape[narrow<size_t>(dim_idx)]) {
        input_idx += input_mappings[narrow<size_t>(dim_idx)][narrow<size_t>(output_dim_counter[narrow<size_t>(dim_idx)])];
        break;
      }
      output_dim_counter[narrow<size_t>(dim_idx)] = 0;
      input_idx += input_mappings[narrow<size_t>(dim_idx)][0 /* output_dim_counter[narrow<size_t>(dim_idx)] */];
    }
  }

  return Status::OK();
}

static Status ValidateUpsampleInput(const void* input, const void* output,
                                    const TensorShape& input_shape, const TensorShape& output_shape,
                                    bool is_resize) {
  if (!input || !output) {
    return Status(ONNXRUNTIME, FAIL,
                  is_resize ? "Resize: input/output value is nullptr"
                            : "Upsample: input/output value is nullptr");
  }

  if (input_shape.NumDimensions() != output_shape.NumDimensions()) {
    return Status(ONNXRUNTIME, FAIL,
                  is_resize ? "Resize: input/output value's dimension mismatch"
                            : "Upsample: input/output value's dimension mismatch");
  }

  if (input_shape.NumDimensions() == 0) {
    return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT,
                  is_resize ? "Resize: input shape needs to be at least a single dimension"
                            : "Upsample: input shape needs to be at least a single dimension.");
  }

  return Status::OK();
}

template <typename T>
static Status UpsampleNearest(const T* input,
                              T* output,
                              const TensorShape& input_shape,
                              const TensorShape& output_shape,
                              gsl::span<const float> scales,
                              gsl::span<const float> roi,
                              bool is_resize,
                              bool extrapolation_enabled,
                              T extrapolation_value,
                              bool use_nearest2x_optimization,
                              const GetOriginalCoordinateFunc& get_original_coordinate,
                              const GetNearestPixelFunc& get_nearest_pixel) {
  ORT_RETURN_IF_ERROR(ValidateUpsampleInput(input, output, input_shape, output_shape, is_resize));

  // special case with fast path
  if (use_nearest2x_optimization && input_shape.NumDimensions() == 4 &&
      scales[0] == 1 && scales[1] == 1 && scales[2] == 2 && scales[3] == 2) {
    UpsampleNearest2x<T>(input_shape[0], input_shape[1], input_shape[2], input_shape[3], input, output);
    return Status::OK();
  }

  return UpsampleNearestImpl(input, output, input_shape, output_shape, scales, roi,
                             extrapolation_enabled, extrapolation_value,
                             get_original_coordinate, get_nearest_pixel);
}

/*

// This is a generic upsample in linear mode for N-D tensor.
// But what's the correct behavior for linear mode is not clear right now.
// this function is not enabled yet.
// this function is not tested for opset 11 changes yet

static Status UpsampleLinearImpl(const std::function<void(size_t, size_t, float)>& apply,
                                 const TensorShape& input_shape,
                                 const TensorShape& output_shape,
                                 const vector<float>& scales,
                                 bool is_resize,
                                 const std::vector<float>& roi,
                                 const GetOriginalCoordinateFunc& get_original_coordinate) {
  auto n_dim = input_shape.NumDimensions();
  for (size_t i = 0, size = output_shape.Size(); i < size; i++) {
    std::vector<int64_t> val1;
    std::vector<int64_t> val2;
    std::vector<float> d1;
    std::vector<float> d2;
    size_t cur_idx = i;
    //val1, vla2, d1, d2 are in reverse order
    for (auto j = static_cast<int64_t>(n_dim - 1); j >= 0; j--) {
      float resized_index = static_cast<float>(cur_idx % output_shape[j]);
      float original_index = get_original_coordinate(static_cast<float>(resized_index), scales[j],
                                                     static_cast<float>(output_shape[j]),
                                                     static_cast<float>(input_shape[j]),
                                                     roi[j], roi[n_dim + j]);
      float v = std::max(0.0f, std::min(original_index, static_cast<float>(input_shape[j] - 1)));
      auto v1 = std::min(static_cast<int64_t>(v), input_shape[j] - 1);
      auto v2 = std::min(v1 + 1, input_shape[j] - 1);
      if (v1 == v2) {
        d1.push_back(0.5f);
        d2.push_back(0.5f);
      } else {
        d1.push_back(std::abs(v - v1));
        d2.push_back(std::abs(v - v2));
      }
      val1.push_back(v1);
      val2.push_back(v2);
      cur_idx /= output_shape[j];
    }

    // output[narrow<size_t>(i)] = 0;

    int64_t step = (1LL << n_dim) - 1;
    while (step >= 0) {
      auto cur = step;
      float w = 1.0f;
      size_t old_idx = 0;
      size_t base = 1;
      for (auto j = static_cast<int64_t>(n_dim - 1); j >= 0; j--) {
        int64_t reverse_idx = static_cast<int64_t>(n_dim - 1) - j;
        w *= (cur % 2) ? d1[reverse_idx] : d2[reverse_idx];
        old_idx += ((cur % 2) ? val2[reverse_idx] : val1[reverse_idx]) * base;
        base *= input_shape[j];
        cur >>= 1;
      }

      // output[narrow<size_t>(i)] += input[old_idx] * w;
      apply(old_idx, i, w);

      step--;
    }
  }

  return Status::OK();
}

template <typename T>
static Status UpsampleLinear(const T* input,
                             T* output,
                             const TensorShape& input_shape,
                             const TensorShape& output_shape,
                             const vector<float>& scales,
                             bool is_resize,
                             const std::vector<float>& roi,
                             const GetOriginalCoordinateFunc& get_original_coordinate) {
  ORT_RETURN_IF_ERROR(ValidateUpsampleInput(input, output, input_shape, output_shape, is_resize));

  // need to initialize the output to 0 as UpsampleLineary always does += when updating
  std::fill_n(output, output_shape.Size(), T{});

  auto apply = [&input, &output](size_t input_idx, size_t output_idx, float w) {
    output[narrow<size_t>(output_idx)] += input[narrow<size_t>(input_idx)] * w;
  };

  return UpsampleLinearImpl(apply, input_shape, output_shape, scales, is_resize, roi, get_original_coordinate);
}
*/

// The following method supports a 4-D input in 'Linear mode'
// that amounts to 'Bilinear' Upsampling/Resizing in the sense that it assumes
// 1. the scale values for the outermost 2 dimensions are 1 or
// 2. the scale values for the outermost and innermost dimensions are 1
// This is the common use-case where the 4-D input (batched multi-channel images)
// is usually of shapes:
// - [N, C, H, W] and the scales are [1.0, 1.0, height_scale, width_scale]
// - [N, H, W, C] and the scales are [1.0, height_scale, width_scale, 1.0]
BilinearParams SetupUpsampleBilinear(const int32_t input_height,
                                     const int32_t input_width,
                                     const int32_t output_height,
                                     const int32_t output_width,
                                     const float height_scale,
                                     const float width_scale,
                                     gsl::span<const float> roi,
                                     AllocatorPtr& alloc,
                                     const GetOriginalCoordinateFunc& get_original_coordinate,
                                     const bool is_nchw) {
  BilinearParams p;

  p.x_original.reserve(output_width);
  p.y_original.reserve(output_height);

  // For each index in the output height and output width, cache its corresponding indices in the input
  // while multiplying it with the input stride for that dimension (cache because we don't have to re-compute
  // each time we come across the output width/ output height value while iterating the output image tensor
  const SafeInt<size_t> idx_buffer_size = SafeInt<size_t>(2) * sizeof(int32_t) * (output_height + output_width);

  // For each index in the output height and output width, cache its corresponding "weights/scales" for its
  // corresponding indices in the input which proportionately indicates how much they will influence the final
  // pixel value in the output
  // (cache because we don't have to re-compute each time we come across the output width/output height
  // value while iterating the output image tensor
  const SafeInt<size_t> scale_buffer_size = SafeInt<size_t>(2) * sizeof(float) * (output_height + output_width);

  // Limit number of allocations to just 1
  const auto inx_scale_data_buffer = alloc->Alloc(idx_buffer_size + scale_buffer_size);
  p.idx_scale_data_buffer_holder = BufferUniquePtr(inx_scale_data_buffer, BufferDeleter(alloc));

  // Get pointers to appropriate memory locations in the scratch buffer
  auto* const idx_data = static_cast<int32_t*>(p.idx_scale_data_buffer_holder.get());

  // input_width is the stride for the height dimension
  p.input_width_mul_y1 = idx_data;
  p.input_width_mul_y2 = p.input_width_mul_y1 + output_height;

  // stride for width is 1 (no multiplication needed)
  const auto output_height_x2 = output_height * 2;  // this is to make prefast happy
  p.in_x1 = p.input_width_mul_y1 + output_height_x2;
  p.in_x2 = p.in_x1 + output_width;

  auto* const scale_data = reinterpret_cast<float*>(p.in_x2 + output_width);

  p.dy1 = scale_data;
  p.dy2 = p.dy1 + output_height;

  p.dx1 = p.dy1 + output_height_x2;
  p.dx2 = p.dx1 + output_width;

  // Start processing
  const size_t height_rindex = is_nchw ? 1 : 2;
  const auto roi_y_start = roi.size() / 2 - (height_rindex + 1);
  const auto roi_y_end = roi.size() - (height_rindex + 1);
  for (int32_t y = 0; y < output_height; ++y) {
    float in_y = height_scale == 1 ? static_cast<float>(y)
                                   : get_original_coordinate(static_cast<float>(y), height_scale,
                                                             static_cast<float>(output_height),
                                                             static_cast<float>(input_height),
                                                             roi[roi_y_start], roi[roi_y_end]);
    p.y_original.emplace_back(in_y);
    in_y = std::max(0.0f, std::min(in_y, static_cast<float>(input_height - 1)));

    const int32_t in_y1 = std::min(static_cast<int32_t>(in_y), input_height - 1);
    const int32_t in_y2 = std::min(in_y1 + 1, input_height - 1);
    p.dy1[narrow<size_t>(y)] = std::fabs(in_y - in_y1);
    p.dy2[narrow<size_t>(y)] = std::fabs(in_y - in_y2);

    if (in_y1 == in_y2) {
      p.dy1[narrow<size_t>(y)] = 0.5f;
      p.dy2[narrow<size_t>(y)] = 0.5f;
    }

    p.input_width_mul_y1[narrow<size_t>(y)] = input_width * in_y1;
    p.input_width_mul_y2[narrow<size_t>(y)] = input_width * in_y2;
  }

  const size_t width_rindex = is_nchw ? 0 : 1;
  const auto roi_x_start = roi.size() / 2 - (width_rindex + 1);
  const auto roi_x_end = roi.size() - (width_rindex + 1);
  for (int32_t x = 0; x < output_width; ++x) {
    float in_x = width_scale == 1 ? static_cast<float>(x)
                                  : get_original_coordinate(static_cast<float>(x),
                                                            width_scale,
                                                            static_cast<float>(output_width),
                                                            static_cast<float>(input_width),
                                                            roi[roi_x_start], roi[roi_x_end]);
    p.x_original.emplace_back(in_x);
    in_x = std::max(0.0f, std::min(in_x, static_cast<float>(input_width - 1)));

    p.in_x1[narrow<size_t>(x)] = std::min(static_cast<int32_t>(in_x), input_width - 1);
    p.in_x2[narrow<size_t>(x)] = std::min(p.in_x1[narrow<size_t>(x)] + 1, input_width - 1);

    p.dx1[narrow<size_t>(x)] = std::fabs(in_x - p.in_x1[narrow<size_t>(x)]);
    p.dx2[narrow<size_t>(x)] = std::fabs(in_x - p.in_x2[narrow<size_t>(x)]);
    if (p.in_x1[narrow<size_t>(x)] == p.in_x2[narrow<size_t>(x)]) {
      p.dx1[narrow<size_t>(x)] = 0.5f;
      p.dx2[narrow<size_t>(x)] = 0.5f;
    }
  }

  return p;
}

// Same as above, but doesn't use any floating-point for the coefficient (i.e., d*_scale_10) computation
BilinearParamsInteger SetupUpsampleBilinearInteger(const int32_t input_height,
                                                   const int32_t input_width,
                                                   const int32_t output_height,
                                                   const int32_t output_width,
                                                   const float height_scale,
                                                   const float width_scale,
                                                   gsl::span<const float> roi,
                                                   AllocatorPtr& alloc,
                                                   const GetOriginalCoordinateFunc& get_original_coordinate,
                                                   const bool is_nchw) {
  BilinearParamsInteger p;

  p.x_original.reserve(output_width);
  p.y_original.reserve(output_height);

  // For each index in the output height and output width, cache its corresponding indices in the input
  // while multiplying it with the input stride for that dimension (cache because we don't have to re-compute
  // each time we come across the output width/ output height value while iterating the output image tensor
  const SafeInt<size_t> idx_buffer_size = SafeInt<size_t>(2) * sizeof(int32_t) * (output_height + output_width);

  // For each index in the output height and output width, cache its corresponding "weights/scales" for its
  // corresponding indices in the input which proportionately indicates how much they will influence the final
  // pixel value in the output
  // (cache because we don't have to re-compute each time we come across the output width/output height
  // value while iterating the output image tensor
  const SafeInt<size_t> scale_buffer_size = SafeInt<size_t>(2) * sizeof(int32_t) * (output_height + output_width);

  // Limit number of allocations to just 1
  const auto inx_scale_data_buffer = alloc->Alloc(idx_buffer_size + scale_buffer_size);
  p.idx_scale_data_buffer_holder = BufferUniquePtr(inx_scale_data_buffer, BufferDeleter(alloc));

  // Get pointers to appropriate memory locations in the scratch buffer
  auto* const idx_data = static_cast<int32_t*>(p.idx_scale_data_buffer_holder.get());

  // input_width is the stride for the height dimension
  p.input_width_mul_y1 = idx_data;
  p.input_width_mul_y2 = p.input_width_mul_y1 + output_height;

  // stride for width is 1 (no multiplication needed)
  const auto output_height_x2 = output_height * 2;  // this is to make prefast happy
  p.in_x1 = p.input_width_mul_y1 + output_height_x2;
  p.in_x2 = p.in_x1 + output_width;

  auto* const scale_data = reinterpret_cast<int32_t*>(p.in_x2 + output_width);

  p.dy1_scale_10 = scale_data;
  p.dy2_scale_10 = p.dy1_scale_10 + output_height;

  p.dx1_scale_10 = p.dy1_scale_10 + output_height_x2;
  p.dx2_scale_10 = p.dx1_scale_10 + output_width;

  // Start processing
  const size_t height_rindex = is_nchw ? 1 : 2;
  const auto roi_y_start = roi.size() / 2 - (height_rindex + 1);
  const auto roi_y_end = roi.size() - (height_rindex + 1);
  for (int32_t y = 0; y < output_height; ++y) {
    float in_y = height_scale == 1 ? static_cast<float>(y)
                                   : get_original_coordinate(static_cast<float>(y), height_scale,
                                                             static_cast<float>(output_height),
                                                             static_cast<float>(input_height),
                                                             roi[roi_y_start], roi[roi_y_end]);
    p.y_original.emplace_back(in_y);
    in_y = std::max(0.0f, std::min(in_y, static_cast<float>(input_height - 1)));
    int32_t in_y_scale_10 = static_cast<int32_t>(in_y * (1 << 10));

    const int32_t in_y1 = std::min(static_cast<int32_t>(in_y), input_height - 1);
    const int32_t in_y2 = std::min(in_y1 + 1, input_height - 1);
    p.dy1_scale_10[narrow<size_t>(y)] = std::abs(in_y_scale_10 - in_y1 * (1 << 10));
    p.dy2_scale_10[narrow<size_t>(y)] = std::abs(in_y_scale_10 - in_y2 * (1 << 10));

    if (in_y1 == in_y2) {
      p.dy1_scale_10[narrow<size_t>(y)] = static_cast<int32_t>(0.5f * (1 << 10));
      p.dy2_scale_10[narrow<size_t>(y)] = static_cast<int32_t>(0.5f * (1 << 10));
    }

    p.input_width_mul_y1[narrow<size_t>(y)] = input_width * in_y1;
    p.input_width_mul_y2[narrow<size_t>(y)] = input_width * in_y2;
  }

  const size_t width_rindex = is_nchw ? 0 : 1;
  const auto roi_x_start = roi.size() / 2 - (width_rindex + 1);
  const auto roi_x_end = roi.size() - (width_rindex + 1);
  for (int32_t x = 0; x < output_width; ++x) {
    float in_x = width_scale == 1 ? static_cast<float>(x)
                                  : get_original_coordinate(static_cast<float>(x),
                                                            width_scale,
                                                            static_cast<float>(output_width),
                                                            static_cast<float>(input_width),
                                                            roi[roi_x_start], roi[roi_x_end]);
    p.x_original.emplace_back(in_x);
    in_x = std::max(0.0f, std::min(in_x, static_cast<float>(input_width - 1)));
    int32_t in_x_scale_10 = static_cast<int32_t>(in_x * (1 << 10));

    p.in_x1[narrow<size_t>(x)] = std::min(static_cast<int32_t>(in_x), input_width - 1);
    p.in_x2[narrow<size_t>(x)] = std::min(p.in_x1[narrow<size_t>(x)] + 1, input_width - 1);

    p.dx1_scale_10[narrow<size_t>(x)] = std::abs(in_x_scale_10 - p.in_x1[narrow<size_t>(x)] * (1 << 10));
    p.dx2_scale_10[narrow<size_t>(x)] = std::abs(in_x_scale_10 - p.in_x2[narrow<size_t>(x)] * (1 << 10));
    if (p.in_x1[narrow<size_t>(x)] == p.in_x2[narrow<size_t>(x)]) {
      p.dx1_scale_10[narrow<size_t>(x)] = static_cast<int32_t>(0.5f * (1 << 10));
      p.dx2_scale_10[narrow<size_t>(x)] = static_cast<int32_t>(0.5f * (1 << 10));
    }
  }

  return p;
}

struct TrilinearParams {
  std::vector<float> x_original;
  std::vector<float> y_original;
  std::vector<float> z_original;

  BufferUniquePtr idx_scale_data_buffer_holder;

  int64_t* in_x1{nullptr};
  int64_t* in_x2{nullptr};
  int64_t* input_width_mul_y1{nullptr};
  int64_t* input_width_mul_y2{nullptr};
  int64_t* input_height_width_mul_z1{nullptr};
  int64_t* input_height_width_mul_z2{nullptr};

  float* dx1{nullptr};
  float* dx2{nullptr};
  float* dy1{nullptr};
  float* dy2{nullptr};
  float* dz1{nullptr};
  float* dz2{nullptr};
};

static TrilinearParams SetupUpsampleTrilinear(int64_t input_depth,
                                              int64_t input_height,
                                              int64_t input_width,
                                              int64_t output_depth,
                                              int64_t output_height,
                                              int64_t output_width,
                                              float depth_scale,
                                              float height_scale,
                                              float width_scale,
                                              gsl::span<const float> roi,
                                              AllocatorPtr& alloc,
                                              const GetOriginalCoordinateFunc& get_original_coordinate) {
  TrilinearParams p;

  p.z_original.reserve(narrow<size_t>(output_depth));
  p.y_original.reserve(narrow<size_t>(output_height));
  p.x_original.reserve(narrow<size_t>(output_width));

  // For each index in the output height and output width, cache its corresponding indices in the input
  // while multiplying it with the input stride for that dimension (cache because we don't have to re-compute
  // each time we come across the output width/ output height value while iterating the output image tensor
  SafeInt<size_t> idx_buffer_size = SafeInt<size_t>(2) * sizeof(int64_t) *
                                    (output_depth + output_height + output_width);

  // For each index in the output height and output width, cache its corresponding "weights/scales" for its
  // corresponding indices in the input which proportionately indicates how much they will influence the final
  // pixel value in the output
  // (cache because we don't have to re-compute each time we come across the output width/output height value
  // while iterating the output image tensor
  SafeInt<size_t> scale_buffer_size = SafeInt<size_t>(2) * sizeof(float) *
                                      (output_depth + output_height + output_width);

  // Limit number of allocations to just 1
  void* inx_scale_data_buffer = alloc->Alloc(idx_buffer_size + scale_buffer_size);
  p.idx_scale_data_buffer_holder = BufferUniquePtr(inx_scale_data_buffer, BufferDeleter(alloc));

  // Get pointers to appropriate memory locations in the scratch buffer
  auto* idx_data = static_cast<int64_t*>(p.idx_scale_data_buffer_holder.get());

  // input_width * input_height is the stride for the depth dimension
  p.input_height_width_mul_z1 = idx_data;
  p.input_height_width_mul_z2 = p.input_height_width_mul_z1 + output_depth;

  // input_width is the stride for the height dimension
  p.input_width_mul_y1 = p.input_height_width_mul_z1 + 2 * output_depth;
  p.input_width_mul_y2 = p.input_width_mul_y1 + output_height;

  // stride for width is 1 (no multiplication needed)
  p.in_x1 = p.input_width_mul_y1 + 2 * output_height;
  p.in_x2 = p.in_x1 + output_width;

  auto* scale_data = reinterpret_cast<float*>(p.in_x2 + output_width);

  p.dz1 = scale_data;
  p.dz2 = p.dz1 + output_depth;

  p.dy1 = p.dz1 + 2 * output_depth;
  p.dy2 = p.dy1 + output_height;

  p.dx1 = p.dy1 + 2 * output_height;
  p.dx2 = p.dx1 + output_width;

  // Start processing
  auto roi_z_start = roi.size() / 2 - 3;
  auto roi_z_end = roi.size() - 3;
  for (int64_t z = 0; z < output_depth; ++z) {
    float in_z = depth_scale == 1 ? static_cast<float>(z)
                                  : get_original_coordinate(static_cast<float>(z), depth_scale,
                                                            static_cast<float>(output_depth),
                                                            static_cast<float>(input_depth),
                                                            roi[roi_z_start], roi[roi_z_end]);
    p.z_original.emplace_back(in_z);
    in_z = std::max(0.0f, std::min(in_z, static_cast<float>(input_depth - 1)));

    const int64_t in_z1 = std::min(static_cast<int64_t>(in_z), input_depth - 1);
    const int64_t in_z2 = std::min(in_z1 + 1, input_depth - 1);
    p.dz1[narrow<size_t>(z)] = std::fabs(in_z - in_z1);
    p.dz2[narrow<size_t>(z)] = std::fabs(in_z - in_z2);

    if (in_z1 == in_z2) {
      p.dz1[narrow<size_t>(z)] = 0.5f;
      p.dz2[narrow<size_t>(z)] = 0.5f;
    }

    p.input_height_width_mul_z1[narrow<size_t>(z)] = input_height * input_width * in_z1;
    p.input_height_width_mul_z2[narrow<size_t>(z)] = input_height * input_width * in_z2;
  }

  auto roi_y_start = roi.size() / 2 - 2;
  auto roi_y_end = roi.size() - 2;
  for (int64_t y = 0; y < output_height; ++y) {
    float in_y = height_scale == 1 ? static_cast<float>(y)
                                   : get_original_coordinate(static_cast<float>(y), height_scale,
                                                             static_cast<float>(output_height),
                                                             static_cast<float>(input_height),
                                                             roi[roi_y_start], roi[roi_y_end]);
    p.y_original.emplace_back(in_y);
    in_y = std::max(0.0f, std::min(in_y, static_cast<float>(input_height - 1)));

    const int64_t in_y1 = std::min(static_cast<int64_t>(in_y), input_height - 1);
    const int64_t in_y2 = std::min(in_y1 + 1, input_height - 1);
    p.dy1[narrow<size_t>(y)] = std::fabs(in_y - in_y1);
    p.dy2[narrow<size_t>(y)] = std::fabs(in_y - in_y2);

    if (in_y1 == in_y2) {
      p.dy1[narrow<size_t>(y)] = 0.5f;
      p.dy2[narrow<size_t>(y)] = 0.5f;
    }

    p.input_width_mul_y1[narrow<size_t>(y)] = input_width * in_y1;
    p.input_width_mul_y2[narrow<size_t>(y)] = input_width * in_y2;
  }

  auto roi_x_start = roi.size() / 2 - 1;
  auto roi_x_end = roi.size() - 1;
  for (int64_t x = 0; x < output_width; ++x) {
    float in_x = width_scale == 1 ? static_cast<float>(x)
                                  : get_original_coordinate(static_cast<float>(x), width_scale,
                                                            static_cast<float>(output_width),
                                                            static_cast<float>(input_width),
                                                            roi[roi_x_start], roi[roi_x_end]);
    p.x_original.emplace_back(in_x);
    in_x = std::max(0.0f, std::min(in_x, static_cast<float>(input_width - 1)));

    p.in_x1[narrow<size_t>(x)] = std::min(static_cast<int64_t>(in_x), input_width - 1);
    p.in_x2[narrow<size_t>(x)] = std::min(p.in_x1[narrow<size_t>(x)] + 1, input_width - 1);

    p.dx1[narrow<size_t>(x)] = std::fabs(in_x - p.in_x1[narrow<size_t>(x)]);
    p.dx2[narrow<size_t>(x)] = std::fabs(in_x - p.in_x2[narrow<size_t>(x)]);
    if (p.in_x1[narrow<size_t>(x)] == p.in_x2[narrow<size_t>(x)]) {
      p.dx1[narrow<size_t>(x)] = 0.5f;
      p.dx2[narrow<size_t>(x)] = 0.5f;
    }
  }

  return p;
}

// The following method supports a 5-D input in 'Linear mode'
// that amounts to 'Trilinear' Upsampling/Resizing in the sense that it assumes
// the scale values for the outermost 2 dimensions are 1.
// This is the common use-case where the 5-D input (batched multi-channel volumes)
// is usually of shape [N, C, D, H, W] and the scales are [1.0, 1.0, depth_scale, height_scale, width_scale]
template <typename T>
void UpsampleTrilinear(int64_t batch_size,
                       int64_t num_channels,
                       int64_t input_depth,
                       int64_t input_height,
                       int64_t input_width,
                       int64_t output_depth,
                       int64_t output_height,
                       int64_t output_width,
                       float depth_scale,
                       float height_scale,
                       float width_scale,
                       gsl::span<const float> roi,
                       bool use_extrapolation,
                       float extrapolation_value,
                       const T* XdataBase,
                       T* YdataBase,
                       AllocatorPtr& alloc,
                       const GetOriginalCoordinateFunc& get_original_coordinate,
                       concurrency::ThreadPool* tp) {
  TrilinearParams p = SetupUpsampleTrilinear(input_depth, input_height, input_width,
                                             output_depth, output_height, output_width,
                                             depth_scale, height_scale, width_scale, roi,
                                             alloc, get_original_coordinate);

  for (int64_t n = 0; n < batch_size; ++n) {
    concurrency::ThreadPool::TrySimpleParallelFor(
        tp, static_cast<std::ptrdiff_t>(num_channels),
        [&](std::ptrdiff_t c) {
          const T* Xdata = XdataBase + (n * num_channels + c) * (input_depth * input_height * input_width);
          T* Ydata = YdataBase + (n * num_channels + c) * (output_depth * output_height * output_width);
          for (int64_t z = 0; z < output_depth; ++z) {
            for (int64_t y = 0; y < output_height; ++y) {
              for (int64_t x = 0; x < output_width; ++x) {
                // when use_extrapolation is set and original index of x or y is out of the dim range
                // then use extrapolation_value as the output value.
                if (use_extrapolation &&
                    ((p.z_original[narrow<size_t>(z)] < 0 || p.z_original[narrow<size_t>(z)] > static_cast<float>(input_depth - 1)) ||
                     (p.y_original[narrow<size_t>(y)] < 0 || p.y_original[narrow<size_t>(y)] > static_cast<float>(input_height - 1)) ||
                     (p.x_original[narrow<size_t>(x)] < 0 || p.x_original[narrow<size_t>(x)] > static_cast<float>(input_width - 1)))) {
                  Ydata[output_width * output_height * z + output_width * y + x] =
                      static_cast<T>(extrapolation_value);
                  continue;
                }

                // subscript ordering in the variable - (xyz)
                T X111 = Xdata[p.input_height_width_mul_z1[narrow<size_t>(z)] + p.input_width_mul_y1[narrow<size_t>(y)] + p.in_x1[narrow<size_t>(x)]];
                T X211 = Xdata[p.input_height_width_mul_z1[narrow<size_t>(z)] + p.input_width_mul_y1[narrow<size_t>(y)] + p.in_x2[narrow<size_t>(x)]];
                T X121 = Xdata[p.input_height_width_mul_z1[narrow<size_t>(z)] + p.input_width_mul_y2[narrow<size_t>(y)] + p.in_x1[narrow<size_t>(x)]];
                T X221 = Xdata[p.input_height_width_mul_z1[narrow<size_t>(z)] + p.input_width_mul_y2[narrow<size_t>(y)] + p.in_x2[narrow<size_t>(x)]];

                T X112 = Xdata[p.input_height_width_mul_z2[narrow<size_t>(z)] + p.input_width_mul_y1[narrow<size_t>(y)] + p.in_x1[narrow<size_t>(x)]];
                T X212 = Xdata[p.input_height_width_mul_z2[narrow<size_t>(z)] + p.input_width_mul_y1[narrow<size_t>(y)] + p.in_x2[narrow<size_t>(x)]];
                T X122 = Xdata[p.input_height_width_mul_z2[narrow<size_t>(z)] + p.input_width_mul_y2[narrow<size_t>(y)] + p.in_x1[narrow<size_t>(x)]];
                T X222 = Xdata[p.input_height_width_mul_z2[narrow<size_t>(z)] + p.input_width_mul_y2[narrow<size_t>(y)] + p.in_x2[narrow<size_t>(x)]];

                Ydata[output_width * output_height * z + output_width * y + x] =
                    static_cast<T>(p.dx2[narrow<size_t>(x)] * p.dy2[narrow<size_t>(y)] * p.dz2[narrow<size_t>(z)] * X111 +
                                   p.dx1[narrow<size_t>(x)] * p.dy2[narrow<size_t>(y)] * p.dz2[narrow<size_t>(z)] * X211 +
                                   p.dx2[narrow<size_t>(x)] * p.dy1[narrow<size_t>(y)] * p.dz2[narrow<size_t>(z)] * X121 +
                                   p.dx1[narrow<size_t>(x)] * p.dy1[narrow<size_t>(y)] * p.dz2[narrow<size_t>(z)] * X221 +

                                   p.dx2[narrow<size_t>(x)] * p.dy2[narrow<size_t>(y)] * p.dz1[narrow<size_t>(z)] * X112 +
                                   p.dx1[narrow<size_t>(x)] * p.dy2[narrow<size_t>(y)] * p.dz1[narrow<size_t>(z)] * X212 +
                                   p.dx2[narrow<size_t>(x)] * p.dy1[narrow<size_t>(y)] * p.dz1[narrow<size_t>(z)] * X122 +
                                   p.dx1[narrow<size_t>(x)] * p.dy1[narrow<size_t>(y)] * p.dz1[narrow<size_t>(z)] * X222);
              }
            }
          }
          Xdata += input_depth * input_height * input_width;
          Ydata += output_depth * output_width * output_height;
        });
  }
}

// Calculates cubic coeff based on Robert Keys approach
// https://ieeexplore.ieee.org/document/1163711
std::array<float, CubicModeGridLength> GetCubicCoeffs(float s, float cubic_coeff_a = -0.75) {
  auto abs_s = std::abs(s);
  std::array<float, CubicModeGridLength> coeffs;
  coeffs[0] = static_cast<float>(
      ((cubic_coeff_a * (abs_s + 1) - 5 * cubic_coeff_a) * (abs_s + 1) + 8 * cubic_coeff_a) * (abs_s + 1) - 4 * cubic_coeff_a);
  coeffs[1] = static_cast<float>(((cubic_coeff_a + 2) * abs_s - (cubic_coeff_a + 3)) * abs_s * abs_s + 1);
  coeffs[2] = static_cast<float>(((cubic_coeff_a + 2) * (1 - abs_s) - (cubic_coeff_a + 3)) * (1 - abs_s) * (1 - abs_s) + 1);
  coeffs[3] = static_cast<float>(
      ((cubic_coeff_a * (2 - abs_s) - 5 * cubic_coeff_a) * (2 - abs_s) + 8 * cubic_coeff_a) * (2 - abs_s) - 4 * cubic_coeff_a);
  return coeffs;
}

// Get the tensor data at the requested coordinate
template <typename T>
T GetDataForCoordinate(const T* Xdata,
                       int64_t x, int64_t y,
                       int64_t input_height, int64_t input_width) {
  x = std::max(static_cast<int64_t>(0), std::min(x, input_width - 1));
  y = std::max(static_cast<int64_t>(0), std::min(y, input_height - 1));
  return Xdata[y * input_width + x];
}

// Computes cubic convolution interpolation in 1D
template <typename T>
float CubicInterpolation1D(const T* Xdata,
                           int64_t x,
                           int64_t y,
                           int64_t input_height,
                           int64_t input_width,
                           std::array<float, CubicModeGridLength>& coeff_array,
                           float coeff_sum,
                           std::unordered_map<int64_t, float>& cache) {
  // When calculating cubic interpolation we move the 4*4 grid across the original data and therefore there is
  // opportunity to cache the results for previously seen combinations.
  // Check if the result is already available in the cache
  auto grid_start_pos = (y)*input_width + (x - 1);
  if (cache.find(grid_start_pos) != cache.end()) {
    return cache[grid_start_pos];
  }

  // get the neighbors in 1D and find interpolation for this dimension
  // for 1D cubic interpolation 4 samples are used. 2 on the left and 2 on the right of x
  float result = 0;
  for (int i = 0, j = -1; i < static_cast<int>(CubicModeGridLength); i++, j++) {
    auto orig_data = GetDataForCoordinate(Xdata, x + j, y, input_height, input_width);
    result += coeff_array[narrow<size_t>(i)] / coeff_sum * orig_data;
  }
  cache[grid_start_pos] = result;

  return result;
}
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 6001)
#endif
template <typename T>
void ResizeBiCubic(int64_t batch_size,
                   int64_t num_channels,
                   int64_t input_height,
                   int64_t input_width,
                   int64_t output_height,
                   int64_t output_width,
                   float height_scale,
                   float width_scale,
                   float cubic_coeff_a,
                   bool use_extrapolation,
                   float extrapolation_value,
                   bool exclude_outside,
                   gsl::span<const float> roi,
                   const T* Xdata,
                   T* Ydata,
                   const GetOriginalCoordinateFunc& get_original_coordinate) {
  std::vector<float> y_original;
  y_original.reserve(narrow<size_t>(output_height));

  std::vector<float> x_original;
  x_original.reserve(narrow<size_t>(output_width));

  std::unordered_map<float, std::array<float, CubicModeGridLength>> cubic_coeffs;
  std::unordered_map<float, std::unordered_map<int64_t, float>> coeff_to_1Dinterpolation_map;
  auto roi_y_start = roi.size() / 2 - 2;
  auto roi_y_end = roi.size() - 2;
  auto roi_x_start = roi.size() / 2 - 1;
  auto roi_x_end = roi.size() - 1;

  // generate coefficients in y direction
  for (int64_t y = 0; y < output_height; ++y) {
    float in_y = height_scale == 1 ? static_cast<float>(y)
                                   : get_original_coordinate(static_cast<float>(y), height_scale,
                                                             static_cast<float>(output_height),
                                                             static_cast<float>(input_height),
                                                             roi[roi_y_start], roi[roi_y_end]);
    y_original.emplace_back(in_y);
    auto s = y_original[narrow<size_t>(y)] - std::floor(y_original[narrow<size_t>(y)]);
    if (cubic_coeffs.find(s) == cubic_coeffs.end()) {
      cubic_coeffs[s] = GetCubicCoeffs(s, cubic_coeff_a);
      coeff_to_1Dinterpolation_map[s] = {};
    }
  }

  // generate coefficients in x direction
  for (int64_t x = 0; x < output_width; ++x) {
    float in_x = width_scale == 1 ? static_cast<float>(x)
                                  : get_original_coordinate(static_cast<float>(x),
                                                            width_scale,
                                                            static_cast<float>(output_width),
                                                            static_cast<float>(input_width),
                                                            roi[roi_x_start], roi[roi_x_end]);
    x_original.emplace_back(in_x);
    auto s = x_original[narrow<size_t>(x)] - std::floor(x_original[narrow<size_t>(x)]);
    if (cubic_coeffs.find(s) == cubic_coeffs.end()) {
      cubic_coeffs[s] = GetCubicCoeffs(s, cubic_coeff_a);
      coeff_to_1Dinterpolation_map[s] = {};
    }
  }

  // setup up temp arrays to hold coefficients when exclude_outside is set to true
  std::array<float, CubicModeGridLength> y_coeff_holder;
  std::array<float, CubicModeGridLength> x_coeff_holder;
  float y_coeff_sum = 1;
  float x_coeff_sum = 1;

  for (int64_t n = 0; n < batch_size; n++) {
    for (int64_t c = 0; c < num_channels; c++) {
      for (int64_t y = 0; y < output_height; ++y) {
        auto in_y = y_original[narrow<size_t>(y)];

        // when use_extrapolation is set and original index is out of the dim range
        // then use extrapolation_value as the output value.
        if (use_extrapolation && (in_y < 0 || in_y > static_cast<float>(input_height - 1))) {
          for (int64_t x = 0; x < output_width; ++x) {
            Ydata[y * output_width + x] = extrapolation_value;
          }
          continue;
        }

        auto y_int = static_cast<int64_t>(std::floor(in_y));
        auto& coeff_y = exclude_outside ? y_coeff_holder : cubic_coeffs[in_y - y_int];
        y_coeff_sum = 1;

        if (exclude_outside) {
          // When true, the weight of sampling locations outside the grid will be set to 0
          // and the weight will be renormalized so that their sum is 1.0
          y_coeff_sum = 0;
          auto& orig_y_coeffs = cubic_coeffs[in_y - y_int];
          for (int64_t i = 0, y_val = y_int - 1; y_val <= y_int + 2; y_val++, i++) {
            y_coeff_holder[narrow<size_t>(i)] = (y_val < 0 || y_val >= static_cast<float>(input_height)) ? 0.0f : orig_y_coeffs[narrow<size_t>(i)];
            y_coeff_sum += y_coeff_holder[narrow<size_t>(i)];
          }
        }

        for (int64_t x = 0; x < output_width; ++x) {
          auto in_x = x_original[narrow<size_t>(x)];

          // when use_extrapolation is set and original index is out of the dim range
          // then use extrapolation_value as the output value.
          if (use_extrapolation && (in_x < 0 || in_x > static_cast<float>(input_width - 1))) {
            Ydata[y * output_width + x] = extrapolation_value;
            continue;
          }

          auto x_int = static_cast<int64_t>(std::floor(in_x));
          auto s_x = static_cast<float>(in_x - x_int);
          auto& coeff_x = exclude_outside ? x_coeff_holder : cubic_coeffs[s_x];
          x_coeff_sum = 1;

          if (exclude_outside) {
            // When true, the weight of sampling locations outside the grid will be set to 0
            // and the weight will be renormalized so that their sum is 1.0
            x_coeff_sum = 0;
            auto& orig_x_coeff = cubic_coeffs[s_x];
            for (int64_t i = 0, x_val = x_int - 1; x_val <= x_int + 2; x_val++, i++) {
              x_coeff_holder[narrow<size_t>(i)] = (x_val < 0 || x_val >= static_cast<float>(input_width)) ? 0.0f : orig_x_coeff[narrow<size_t>(i)];
              x_coeff_sum += x_coeff_holder[narrow<size_t>(i)];
            }
          }

          // Compute cubic interpolation in x dimension using the x coefficients.
          // From the result of cubic interpolation in x dim, compute cubic interpolation in y dimension
          auto& interpolation_result_cache = coeff_to_1Dinterpolation_map[s_x];
          float result = 0;
          for (int64_t y_val = y_int - 1, i = 0; y_val <= y_int + 2; y_val++, i++) {
            auto x_interpolation_result = CubicInterpolation1D(Xdata, x_int, y_val,
                                                               input_height, input_width, coeff_x, x_coeff_sum,
                                                               interpolation_result_cache);
            result += x_interpolation_result * coeff_y[narrow<size_t>(i)] / y_coeff_sum;
          }

          Ydata[y * output_width + x] = static_cast<T>(result);
        }
      }

      Xdata += input_height * input_width;
      Ydata += output_height * output_width;

      // clear the cache when moving to the next channel
      coeff_to_1Dinterpolation_map.clear();
    }
  }
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <typename T>
Status Upsample<T>::BaseCompute(OpKernelContext* context,
                                gsl::span<const float> roi,
                                gsl::span<const float> scales,
                                gsl::span<const int64_t> output_dims) const {
  const auto* X = context->Input<Tensor>(0);
  auto dims = X->Shape().GetDims();
  ORT_RETURN_IF_NOT(output_dims.size() == dims.size(), "Rank of input and output tensor should be same.");

  Tensor* Y = context->Output(0, output_dims);

  // Return early if the output tensor is going to be of size 0
  if (Y->Shape().Size() == 0) {
    return Status::OK();
  }

  if (dims.size() != scales.size())
    return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                  is_resize_ ? "Resize: input tensor's dimension does not match the scales."
                             : "Upsample: input tensor's dimension does not match the scales.");

  if (roi.size() != 2 * dims.size())
    return Status(ONNXRUNTIME, INVALID_ARGUMENT,
                  "Resize: size of roi array should be 2 * N where N is the rank of input tensor X.");

  bool no_scale = true;
  for (std::size_t i = 0, end = output_dims.size(); i < end; i++) {
    if (no_scale && output_dims[narrow<size_t>(i)] != dims[narrow<size_t>(i)]) no_scale = false;
  }

  if (no_scale) {
    memcpy(Y->MutableDataRaw(), X->DataRaw(), Y->SizeInBytes());
    return Status::OK();
  }
  AllocatorPtr alloc;
  ORT_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));
  switch (mode_) {
    case UpsampleMode::NN:
      return UpsampleNearest<T>(X->Data<T>(), Y->MutableData<T>(), X->Shape(), Y->Shape(),
                                scales, roi, is_resize_, use_extrapolation_, static_cast<T>(extrapolation_value_),
                                use_nearest2x_optimization_, get_original_coordinate_, get_nearest_pixel_);
    case UpsampleMode::LINEAR: {
      // Supports 'bilinear' and 'trilinear' sampling only

      //'bilinear' == 2-D input or 4-D input with outermost 2 scales as 1 or
      // 4-D input with outermost and innermost scales as 1
      if (dims.size() == 2 || dims.size() == 4) {
        bool is_2D = dims.size() == 2;
        bool is_nchw = true;

        int32_t batch_size;
        int32_t num_channels;
        int32_t input_height;
        int32_t input_width;

        int32_t output_height;
        int32_t output_width;

        float height_scale;
        float width_scale;

        if (is_2D) {
          batch_size = 1;
          num_channels = 1;
          input_height = static_cast<int32_t>(dims[0]);
          input_width = static_cast<int32_t>(dims[1]);

          output_height = static_cast<int32_t>(output_dims[0]);
          output_width = static_cast<int32_t>(output_dims[1]);

          height_scale = scales[0];
          width_scale = scales[1];
        } else {
          if (scales[1] == 1.0f) {
            batch_size = static_cast<int32_t>(dims[0]);
            num_channels = static_cast<int32_t>(dims[1]);
            input_height = static_cast<int32_t>(dims[2]);
            input_width = static_cast<int32_t>(dims[3]);

            output_height = static_cast<int32_t>(output_dims[2]);
            output_width = static_cast<int32_t>(output_dims[3]);

            height_scale = scales[2];
            width_scale = scales[3];
          } else {
            ORT_RETURN_IF_NOT(scales[3] == 1.0f, "4-D input with innermost scale (usually channel of NHWC) as 1.");
            is_nchw = false;

            batch_size = static_cast<int32_t>(dims[0]);
            num_channels = static_cast<int32_t>(dims[3]);
            input_height = static_cast<int32_t>(dims[1]);
            input_width = static_cast<int32_t>(dims[2]);

            output_height = static_cast<int32_t>(output_dims[1]);
            output_width = static_cast<int32_t>(output_dims[2]);

            height_scale = scales[1];
            width_scale = scales[2];
          }
        }

        if (is_nchw) {
          if (antialias_) {
            UpsampleBilinearAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                      height_scale, width_scale, roi, use_extrapolation_, extrapolation_value_, exclude_outside_,
                                      X, Y->MutableData<T>(), alloc, get_original_coordinate_,
                                      output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
          } else {
            UpsampleBilinear(batch_size, num_channels, input_height, input_width, output_height, output_width,
                             height_scale, width_scale, roi,
                             use_extrapolation_, extrapolation_value_, X->Data<T>(),
                             Y->MutableData<T>(), alloc, get_original_coordinate_,
                             output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
          }
        } else {
          if (use_extrapolation_) {
            if (antialias_) {
              NhwcUpsampleBilinearAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                            height_scale, width_scale, roi, use_extrapolation_, extrapolation_value_, exclude_outside_,
                                            X, Y->MutableData<T>(), alloc, get_original_coordinate_,
                                            output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
            } else {
              if (!is_2D &&
                  (Y->GetElementType() == ONNX_NAMESPACE::TensorProto_DataType_UINT8 ||
                   Y->GetElementType() == ONNX_NAMESPACE::TensorProto_DataType_INT8)) {
                NhwcUpsampleBilinearInteger<T, true>(
                    batch_size, num_channels, input_height, input_width, output_height, output_width,
                    height_scale, width_scale, roi, extrapolation_value_, X->Data<T>(), Y->MutableData<T>(),
                    alloc, get_original_coordinate_,
                    output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
              } else {
                NhwcUpsampleBilinear<T, true>(
                    batch_size, num_channels, input_height, input_width, output_height, output_width,
                    height_scale, width_scale, roi, extrapolation_value_, X->Data<T>(), Y->MutableData<T>(),
                    alloc, get_original_coordinate_,
                    output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
              }
            }
          } else {
            if (antialias_) {
              NhwcUpsampleBilinearAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                            height_scale, width_scale, roi, use_extrapolation_, extrapolation_value_, exclude_outside_,
                                            X, Y->MutableData<T>(), alloc, get_original_coordinate_,
                                            output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
            } else {
              if (!is_2D &&
                  (Y->GetElementType() == ONNX_NAMESPACE::TensorProto_DataType_UINT8 ||
                   Y->GetElementType() == ONNX_NAMESPACE::TensorProto_DataType_INT8)) {
                NhwcUpsampleBilinearInteger<T, false>(
                    batch_size, num_channels, input_height, input_width, output_height, output_width,
                    height_scale, width_scale, roi, extrapolation_value_, X->Data<T>(), Y->MutableData<T>(),
                    alloc, get_original_coordinate_,
                    output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
              } else {
                NhwcUpsampleBilinear<T, false>(
                    batch_size, num_channels, input_height, input_width, output_height, output_width,
                    height_scale, width_scale, roi, extrapolation_value_, X->Data<T>(), Y->MutableData<T>(),
                    alloc, get_original_coordinate_,
                    output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
              }
            }
          }
        }
        return Status::OK();
      } else if (dims.size() == 3 || dims.size() == 5) {
        //'trilinear' == 3-D input or 5-D input with outermost 2 scales as 1
        bool is_3D = dims.size() == 3;

        const int64_t batch_size = is_3D ? 1 : dims[0];
        const int64_t num_channels = is_3D ? 1 : dims[1];
        const int64_t input_depth = is_3D ? dims[0] : dims[2];
        const int64_t input_height = is_3D ? dims[1] : dims[3];
        const int64_t input_width = is_3D ? dims[2] : dims[4];

        const int64_t output_depth = is_3D ? output_dims[0] : output_dims[2];
        const int64_t output_height = is_3D ? output_dims[1] : output_dims[3];
        const int64_t output_width = is_3D ? output_dims[2] : output_dims[4];

        if (antialias_) {
          UpsampleTrilinearAntiAlias(batch_size, num_channels, input_depth, input_height, input_width,
                                     output_depth, output_height, output_width,
                                     is_3D ? scales[0] : scales[2], is_3D ? scales[1] : scales[3],
                                     is_3D ? scales[2] : scales[4], roi, use_extrapolation_, extrapolation_value_,
                                     exclude_outside_, X, Y->MutableData<T>(), alloc, get_original_coordinate_,
                                     output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
        } else {
          UpsampleTrilinear(batch_size, num_channels, input_depth, input_height, input_width,
                            output_depth, output_height, output_width,
                            is_3D ? scales[0] : scales[2], is_3D ? scales[1] : scales[3],
                            is_3D ? scales[2] : scales[4], roi, use_extrapolation_, extrapolation_value_,
                            X->Data<T>(), Y->MutableData<T>(), alloc, get_original_coordinate_,
                            output_height * output_width > 64 ? context->GetOperatorThreadPool() : nullptr);
        }
        return Status::OK();
      } else {
        // User shouldn't hit this as the check has been performed in ScalesValidation()
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, (is_resize_ ? "Resize" : "Upsample"),
                               ": 'Linear' mode only support 2-D inputs or 3-D inputs ('Bilinear', 'Trilinear') "
                               "or 4-D inputs or 5-D inputs with the corresponding outermost 2 scale values "
                               "being 1.");
      }
    }
    case UpsampleMode::CUBIC: {
      // Supports 'bicubic' sampling only

      // User shouldn't hit this as the check has been performed in ScalesValidation()
      if (dims.size() != 2 && dims.size() != 4) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, (is_resize_ ? "Resize" : "Upsample"),
                               ": 'Cubic' mode only support 2-D inputs ('Bicubic') or 4-D inputs "
                               "with the corresponding outermost 2 scale values being 1.");
      }

      bool is_2D = dims.size() == 2;
      bool is_nchw = is_2D ? true : (scales[1] == 1.0f);

      const int64_t batch_size = is_2D ? 1 : dims[0];
      const int64_t num_channels = is_2D ? 1 : (is_nchw ? dims[1] : dims[3]);
      const int64_t input_height = is_2D ? dims[0] : (is_nchw ? dims[2] : dims[1]);
      const int64_t input_width = is_2D ? dims[1] : (is_nchw ? dims[3] : dims[2]);
      const int64_t output_height = is_2D ? output_dims[0] : (is_nchw ? output_dims[2] : output_dims[1]);
      const int64_t output_width = is_2D ? output_dims[1] : (is_nchw ? output_dims[3] : output_dims[2]);
      const float height_scale = is_2D ? scales[0] : (is_nchw ? scales[2] : scales[1]);
      const float width_scale = is_2D ? scales[1] : (is_nchw ? scales[3] : scales[2]);
      const bool is_upsampling = height_scale >= 1.0f && width_scale >= 1.0f;

      if (antialias_) {
        if (!is_nchw) {
          NhwcResizeBiCubicAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                     height_scale, width_scale, cubic_coeff_a_, use_extrapolation_,
                                     extrapolation_value_, exclude_outside_, roi, X,
                                     Y->MutableData<T>(), alloc, get_original_coordinate_,
                                     output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
        } else {
          ResizeBiCubicAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                 height_scale, width_scale, cubic_coeff_a_, use_extrapolation_,
                                 extrapolation_value_, exclude_outside_, roi, X,
                                 Y->MutableData<T>(), alloc, get_original_coordinate_,
                                 output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
        }
      } else if (!is_nchw && is_upsampling) {
        // Antialiasing has no effect during image upsampling, so the antialiasing logic can be reused as-is.
        // TODO(yilyu): Benchmark whether is_nchw with upscaling should use ResizeBiCubicAntiAlias or ResizeBiCubic.
        NhwcResizeBiCubicAntiAlias(batch_size, num_channels, input_height, input_width, output_height, output_width,
                                   height_scale, width_scale, cubic_coeff_a_, use_extrapolation_,
                                   extrapolation_value_, exclude_outside_, roi, X,
                                   Y->MutableData<T>(), alloc, get_original_coordinate_,
                                   output_height * output_width * num_channels > 64 ? context->GetOperatorThreadPool() : nullptr);
      } else {
        ResizeBiCubic(batch_size, num_channels, input_height, input_width, output_height, output_width,
                      height_scale, width_scale, cubic_coeff_a_, use_extrapolation_,
                      extrapolation_value_, exclude_outside_, roi, X->Data<float>(),
                      Y->MutableData<float>(), get_original_coordinate_);
      }
      return Status::OK();
    }
    default:
      return Status(ONNXRUNTIME, FAIL, is_resize_ ? "Resize: unexpected mode" : "Upsample: unexpected mode");
  }
}

template <typename T>
Status Upsample<T>::Compute(OpKernelContext* context) const {
  const auto* X = context->Input<Tensor>(0);

  auto input_dims = X->Shape().GetDims();
  TensorShapeVector output_dims(input_dims.size());

  // Get roi data
  // Initialize the roi array to all zeros as this will be the most common case
  // Roi data is needed only when coordinate transformation mode is set to tf_crop_and_resize
  // for all other cases we need a 0 initialized roi array
  InlinedVector<float> roi_array(roi_);

  if (!roi_cached_) {
    bool use_default_roi = true;
    if (need_roi_input_) {
      ORT_RETURN_IF_NOT(roi_input_idx_ > 0, "Invalid roi input index.");
      const auto* roi = context->Input<Tensor>(roi_input_idx_);
      if (roi != nullptr) {
        ParseRoiData(roi, roi_array);
        use_default_roi = false;
      }
    }
    if (use_default_roi) {
      // default roi includes ensures all the values in that axis are included in the roi
      // normalized roi is thus : [start, end] = [0, 1]
      size_t input_rank = input_dims.size();
      roi_array.resize(input_rank * 2);
      for (size_t i = 0; i < input_rank; ++i) {
        roi_array[narrow<size_t>(i)] = 0;
        roi_array[i + input_rank] = 1;
      }
    }
  }

  ComputeROIWithAxes(roi_array, input_dims.size());
  // Get scales data
  InlinedVector<float> scales_array(input_dims.size());

  if (OpKernel::Node().InputDefs().size() == 1) {
    // Compute output shape from scales and input dims
    scales_array = scales_;

    ComputeOutputShape(scales_array, input_dims, output_dims);
    return BaseCompute(context, roi_array, scales_array, output_dims);
  }

  const auto* scales = context->Input<Tensor>(scales_input_idx_);
  const auto* sizes = context->Input<Tensor>(sizes_input_idx_);
  // Get scales data

  if (scales_cached_) {
    ORT_RETURN_IF_NOT(sizes == nullptr, "Only one of scales or sizes must be provided as input.");
    scales_array = scales_;
    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_array, input_dims, output_dims);
    return BaseCompute(context, roi_array, scales_array, output_dims);
  }

  if (scales != nullptr && scales->Shape().Size() != 0) {
    ORT_RETURN_IF_NOT(sizes == nullptr, "Only one of scales or sizes must be provided as input.");
    ORT_RETURN_IF_ERROR(ParseScalesData(scales, scales_array, input_dims.size()));

    // Compute output shape from scales and input dims
    ComputeOutputShape(scales_array, input_dims, output_dims);
  } else {
    ORT_RETURN_IF_NOT(sizes != nullptr && sizes->Shape().Size() != 0, "Either scales or sizes MUST be provided as input.");

    ORT_RETURN_IF_ERROR(ParseSizesData(sizes, output_dims, input_dims));

    ORT_RETURN_IF_ERROR(ParseScalesDataAndAdjustOutputSize(output_dims, input_dims, scales_array));
  }

  return BaseCompute(context, roi_array, scales_array, output_dims);
}
}  // namespace onnxruntime
