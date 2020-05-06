#include <array>
#include <vector>

#include <ATen/ATen.h>
#include <torch/library.h>
#include <ATen/cpp_custom_type_hack.h>
#include <ATen/native/quantized/cpu/fbgemm_utils.h>
#include <ATen/native/quantized/cpu/init_qnnpack.h>
#include <ATen/native/quantized/cpu/qnnpack_utils.h>
#include <ATen/quantized/Quantizer.h>

#include "_common_utils.h"

namespace caffe2 {

#ifdef USE_FBGEMM
// Required for cpp_custom_type_hack to work
CAFFE_KNOWN_TYPE(PackedConvWeight<1>);
CAFFE_KNOWN_TYPE(PackedConvWeight<2>);
CAFFE_KNOWN_TYPE(PackedConvWeight<3>);
#endif

#ifdef USE_PYTORCH_QNNPACK
// Required for cpp_custom_type_hack to work
CAFFE_KNOWN_TYPE(PackedConvWeightsQnnp);
#endif // USE_PYTORCH_QNNPACK

} // namespace caffe2

namespace at {
namespace native {
namespace {

template <int kSpatialDim = 2>
class QConvPackWeightInt8 final {
 public:
  static Tensor run_conv(Tensor weight,
                         c10::optional<Tensor> bias,
                         torch::List<int64_t> stride,
                         torch::List<int64_t> padding,
                         torch::List<int64_t> dilation,
                         int64_t groups) {
    return _run(weight,
               bias,
               stride,
               padding,
               /*output_padding=*/torch::List<int64_t>({0, 0}),
               dilation,
               groups,
               /*transpose=*/false);
  }

  static Tensor run_deconv(Tensor weight,
                           c10::optional<Tensor> bias,
                           torch::List<int64_t> stride,
                           torch::List<int64_t> input_padding,
                           torch::List<int64_t> output_padding,
                           torch::List<int64_t> dilation,
                           int64_t groups) {
    return _run(weight,
               bias,
               stride,
               input_padding,
               output_padding,
               dilation,
               groups,
               /*transpose=*/true);
  }

 private:
  static Tensor _run(
      Tensor weight,
      c10::optional<Tensor> bias,
      torch::List<int64_t> stride,
      torch::List<int64_t> input_padding,
      torch::List<int64_t> output_padding,
      torch::List<int64_t> dilation,
      int64_t groups,
      bool transpose) {
    auto& ctx = at::globalContext();
#ifdef USE_FBGEMM
    if (ctx.qEngine() == at::QEngine::FBGEMM) {
      TORCH_CHECK(kSpatialDim != 1, "FPGEMM Doesn't support 1D conv prepack yet.");
      TORCH_CHECK(!transpose, "FBGEMM prepacking for conv_transpose is not"
                  "implemented yet")
      return fbgemm_conv_prepack(
          weight, bias, stride, input_padding, dilation, groups);
    }
#endif

#ifdef USE_PYTORCH_QNNPACK
    if (ctx.qEngine() == at::QEngine::QNNPACK) {
      TORCH_CHECK(
          kSpatialDim == 1 || kSpatialDim == 2,
          "quantized::conv_prepack (qnnpack): QNNPACK only supports Conv1d "
          "and Conv2d now.");
      if (kSpatialDim == 1) {
        if (weight.dim() == 3) {
          weight = weight.unsqueeze(_internal::kConv1dSqueezeDim + 2);
        }
        stride = _internal::MakeArgForConv1d(stride, 1);
        input_padding = _internal::MakeArgForConv1d(input_padding, 0);
        output_padding = _internal::MakeArgForConv1d(output_padding, 0);
        dilation = _internal::MakeArgForConv1d(dilation, 1);
      }
      return qnnpack_conv_prepack(weight, bias, stride, input_padding,
                                  output_padding, dilation, groups, transpose);
    }
#endif

    TORCH_CHECK(
        false,
        "Didn't find engine for operation quantized::conv2d_prepack ",
        toString(ctx.qEngine()));
  }

#ifdef USE_FBGEMM
  static Tensor fbgemm_conv_prepack(
      Tensor weight,
      c10::optional<Tensor> bias,
      torch::List<int64_t> stride,
      torch::List<int64_t> padding,
      torch::List<int64_t> dilation,
      int64_t groups) {
    TORCH_CHECK(
        weight.ndimension() == kSpatialDim + 2,
        "Weights are expected to have ",
        kSpatialDim + 2,
        " dimensions");

    TORCH_CHECK(
        stride.size() == kSpatialDim,
        "stride should contain ",
        kSpatialDim,
        " elements for ",
        kSpatialDim,
        "D convolution.");
    TORCH_CHECK(
        padding.size() == kSpatialDim,
        "Specify front/top/left padding only. "
        "end/bottom/right padding assumed to be equal to front/top/left");
    TORCH_CHECK(
        dilation.size() == kSpatialDim,
        "dilation should contain ",
        kSpatialDim,
        " elements for ",
        kSpatialDim,
        "D convolution.");
    const int output_channels = weight.size(0);
    const int input_channels_per_group = weight.size(1);
    const int kernel_d = kSpatialDim == 2 ? 1 : weight.size(2);
    const int kernel_h = weight.size(kSpatialDim);
    const int kernel_w = weight.size(kSpatialDim + 1);

    // mini-batch doesn't have any impact on how we pack weights
    // so we pass it as 1
    // Input image height/width also don't have any impact on how we pack
    // weights so we can pass any values
    const fbgemm::conv_param_t<kSpatialDim> conv_p =
        fbgemm_utils::MakeFbgemmConvParam<kSpatialDim>(
            1, // dummy batch size
            input_channels_per_group * groups, // input channels
            output_channels,
            kSpatialDim == 2 ? std::vector<int>{28, 28} // dummy image size
                             : std::vector<int>{28, 28, 28},
            groups,
            kSpatialDim == 2 ? std::vector<int>{kernel_h, kernel_w}
                             : std::vector<int>{kernel_d, kernel_h, kernel_w},
            std::vector<int>(stride.begin(), stride.end()),
            std::vector<int>(padding.begin(), padding.end()),
            std::vector<int>(dilation.begin(), dilation.end()));

    const auto qtype = weight.qscheme();
    std::vector<int32_t> zero_points;
    if (qtype == kPerTensorAffine) {
      zero_points = {static_cast<int32_t>(weight.q_zero_point())};
    } else if (qtype == kPerChannelAffine) {
      int64_t axis = weight.q_per_channel_axis();
      TORCH_CHECK(
          axis == 0,
          "Only per output channel quantization is supported for the weights");
      zero_points.resize(output_channels);
      for (int i = 0; i < output_channels; ++i) {
        zero_points[i] = weight.q_per_channel_zero_points()[i].item<int32_t>();
      }
    } else {
      TORCH_CHECK(false, "Unsupported qscheme: ", toString(qtype));
    }

    // FBGEMM expects weights to be in channels last
    // TODO: Change this when ChannelsLast3d is ready.
    const Tensor weight_nhwc = kSpatialDim == 2
        ? weight.contiguous(MemoryFormat::ChannelsLast)
        : fbgemm_utils::ConvertToChannelsLast3dTensor(weight);
    const int8_t* weight_data_int8 =
        reinterpret_cast<int8_t*>(weight_nhwc.data_ptr<c10::qint8>());
    std::vector<int32_t> col_offsets(output_channels);
    // compute column offsets (Similar to
    // fbgemm::col_offsets_with_zero_pt_s8acc32_ref) please note that offsets
    // include the sum of columns as well as the scalar term weight_zero_point *
    // KDim
    const int output_channels_per_group = output_channels / groups;
    const int inner_size =
        kernel_d * kernel_h * kernel_w * input_channels_per_group;
    for (int g = 0; g < groups; ++g) {
      for (int i = 0; i < output_channels_per_group; ++i) {
        const int c = g * output_channels_per_group + i;
        int32_t sum = 0;
        for (int j = 0; j < inner_size; ++j) {
          sum += static_cast<int32_t>(weight_data_int8[c * inner_size + j]);
        }
        if (qtype == kPerTensorAffine) {
          col_offsets[c] = sum - zero_points[0] * inner_size;
        } else {
          col_offsets[c] = sum - zero_points[c] * inner_size;
        }
      }
    }

    std::vector<float> scales;
    if (qtype == kPerTensorAffine) {
      scales = {static_cast<float>(weight.q_scale())};
    } else if (qtype == kPerChannelAffine) {
      scales.resize(output_channels);
      for (int i = 0; i < output_channels; ++i) {
        scales[i] = weight.q_per_channel_scales()[i].item<float>();
      }
    }

    c10::optional<at::Tensor> bias_contig;
    if (bias.has_value()) {
      Tensor bias_vec = bias.value();
      TORCH_CHECK(bias_vec.dim() == 1, "bias should be a vector (1D Tensor)");
      TORCH_CHECK(
          bias_vec.size(0) == output_channels,
          "bias should have K elements: " + std::to_string(output_channels));
      bias_contig = bias->contiguous();
    }

    auto ret_ptr = std::make_unique<PackedConvWeight<kSpatialDim>>(
        PackedConvWeight<kSpatialDim>{
            std::make_unique<fbgemm::PackWeightsForConv<kSpatialDim>>(
                conv_p, weight_data_int8),
            bias_contig,
            col_offsets,
            kSpatialDim == 2
                ? std::vector<int64_t>{kernel_h, kernel_w}
                : std::vector<int64_t>{kernel_d, kernel_h, kernel_w},
            scales,
            zero_points,
            qtype});

    // TODO: we will need to replace this with torchscript classes at a later
    // point.
    return cpp_custom_type_hack::create(std::move(ret_ptr), weight.options());
  }
#endif // USE_FBGEMM

#ifdef USE_PYTORCH_QNNPACK
  static at::Tensor qnnpack_conv_prepack(
      Tensor weight,
      c10::optional<Tensor> bias_in,
      torch::List<int64_t> stride,
      torch::List<int64_t> input_padding,
      torch::List<int64_t> output_padding,
      torch::List<int64_t> dilation,
      int64_t groups,
      bool transpose) {
    TORCH_CHECK(
        weight.ndimension() == 4,
        "quantized::conv2d_prepack (qnnpack): Weights are expected to have 4 "
        "dimensions");
    const auto qtype = weight.qscheme();
    TORCH_CHECK(
        weight.qscheme() == kPerTensorAffine,
        "quantized::conv2d_prepack (qnnpack): only supports Per Tensor "
        "Quantization Scheme")
    TORCH_CHECK(
        stride.size() == 2,
        "quantized::conv2d_prepack (qnnpack): 2D convolution only");
    TORCH_CHECK(
        input_padding.size() == 2,
        "quantized::conv2d_prepack (qnnpack): Specify top/left padding only. "
        "bottom/right padding assumed to be equal to top/left");
    TORCH_CHECK(
        !transpose || output_padding.size() == 2,
        "quantized::conv2d_prepack (qnnpack): Specify top/left padding only. "
        "bottom/right padding assumed to be equal to top/left");
    TORCH_CHECK(
        dilation.size() == 2,
        " quantized::conv2d_prepack (qnnpack): 2D convolution only");

    initQNNPACK();

    // QNNPACK expects weights to be of the format {out_c, kH, kW, in_c/groups},
    // but PyTorch lays them out as {out_c, in_c/groups, kH, kW}
    const size_t out_ch = transpose ? weight.size(1) * groups : weight.size(0);
    const size_t in_ch = transpose ? weight.size(0) : weight.size(1) * groups;
    const uint32_t kernel_h = weight.size(2);
    const uint32_t kernel_w = weight.size(3);

    Tensor bias_fp32;
    if (bias_in.has_value()) {
      bias_fp32 = bias_in.value();
    } else {
      bias_fp32 = at::zeros(out_ch, weight.options().dtype(at::kFloat));
    }
    TORCH_CHECK(
        !bias_fp32.defined() ||
            (bias_fp32.ndimension() == 1 && bias_fp32.size(0) == out_ch),
        "quantized::conv2d_prepack (qnnpack): expected bias to be 1-dimensional "
        "with ",
        out_ch,
        " elements",
        ", but got bias of size ",
        bias_fp32.sizes(),
        " instead");

    uint32_t stride_h = stride[0];
    uint32_t stride_w = stride[1];
    uint32_t input_pad_t = input_padding[0];
    uint32_t input_pad_l = input_padding[1];
    uint32_t output_height_adjustment = output_padding[0];
    uint32_t output_width_adjustment = output_padding[1];
    uint32_t dilation_h = dilation[0];
    uint32_t dilation_w = dilation[1];

    qnnpack::conv_param_t conv_p(
        {kernel_w, kernel_h},
        {stride_w, stride_h},
        {dilation_w, dilation_h},
        {input_pad_t, input_pad_l, input_pad_t, input_pad_l},
        {output_width_adjustment, output_height_adjustment},
        groups,
        in_ch,
        out_ch,
        weight.q_zero_point(),
        weight.q_scale(),
        std::numeric_limits<uint8_t>::min(),
        std::numeric_limits<uint8_t>::max(),
        transpose);

    auto weight_contig = weight.contiguous(MemoryFormat::ChannelsLast);
    auto weight_zp = weight.q_zero_point();

    // We set the pre-packed conv weights to nullptr below as we call pre-pack
    // during the first invocation of operator run. Refer to qconv.cpp for more
    // details. TODO Update to actually call pre-pack here once bias is removed
    // from pre-packing step.
    auto wt_ptr = std::make_unique<PackedConvWeightsQnnp>(
        PackedConvWeightsQnnp{nullptr, /* PrePackConvWeights */
                              weight_contig, /* int8_t weight */
                              bias_fp32.contiguous(), /* fp32 bias */
                              c10::nullopt, /* input_scale */
                              {kernel_h, kernel_w},
                              weight.q_scale(),
                              weight_zp});

    return cpp_custom_type_hack::create(std::move(wt_ptr), weight.options());
  }
#endif // USE_PYTORCH_QNNPACK
};

TORCH_LIBRARY_IMPL(quantized, QuantizedCPU, m) {
  // conv_prepack is deprecated, please use conv2d_prepack for 2D conv.
  m.impl("conv_prepack", QConvPackWeightInt8<2>::run_conv);
  m.impl("conv1d_prepack", QConvPackWeightInt8<1>::run_conv);
  m.impl("conv2d_prepack", QConvPackWeightInt8<2>::run_conv);
  m.impl("conv3d_prepack", QConvPackWeightInt8<3>::run_conv);
  m.impl("conv_transpose1d_prepack", QConvPackWeightInt8<1>::run_deconv);
  m.impl("conv_transpose2d_prepack", QConvPackWeightInt8<2>::run_deconv);
}

TORCH_LIBRARY_IMPL(_quantized, QuantizedCPU, m) {
  m.impl("conv1d_prepack", QConvPackWeightInt8<1>::run_conv);
  m.impl("conv2d_prepack", QConvPackWeightInt8<2>::run_conv);
  m.impl("conv_transpose1d_prepack", QConvPackWeightInt8<1>::run_deconv);
  m.impl("conv_transpose2d_prepack", QConvPackWeightInt8<2>::run_deconv);
}

} // namespace
} // namespace native
} // namespace at
