/*
     * Copyright 2018 Google LLC
     *
     * Licensed under the Apache License, Version 2.0 (the "License");
     * you may not use this file except in compliance with the License.
     * You may obtain a copy of the License at
     *
     *     https://www.apache.org/licenses/LICENSE-2.0
     *
     * Unless required by applicable law or agreed to in writing, software
     * distributed under the License is distributed on an "AS IS" BASIS,
     * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
     * See the License for the specific language governing permissions and
     * limitations under the License.
     */

#include <string>

#include "ostream_nullptr.h"
#include "glog/logging.h"
#include "cuda_util.h"
#include "cudnn.pb.h"
#include "cudnn_test.h"
#include "load_textproto.h"
#include "test_util.h"


DEFINE_string(proto_path, "cudnn_tests.textproto",
              "Path to text proto file containing benchmarks to run.");

namespace nvidia_libs_test {
namespace {
template <typename T>
Status TensorDataEqual(const DeviceMemory& first, const DeviceMemory& second,
                       const TensorDescriptor& descriptor, double tolerance) {
  return DeviceDataEqual(static_cast<const T*>(first.get()),
                         static_cast<const T*>(second.get()),
                         GetTensorNumElements(descriptor), tolerance);
}
}  // namespace

Status TensorDataEqual(const DeviceMemory& first, const DeviceMemory& second,
                       const TensorDescriptor& descriptor, double tolerance) {
  switch (GetTensorDataType(descriptor)) {
    case CUDNN_DATA_FLOAT:
      return TensorDataEqual<float>(first, second, descriptor, tolerance);
    case CUDNN_DATA_DOUBLE:
      return TensorDataEqual<double>(first, second, descriptor, tolerance);
    case CUDNN_DATA_HALF:
      return TensorDataEqual<__half>(first, second, descriptor, tolerance);
    default:
      LOG(FATAL) << "Not yet supported";
  }
}

proto::Tests GetCudnnTestsFromFile() {
  proto::Tests tests;
  LoadTextProto(FLAGS_proto_path, &tests);
  return tests;
}

std::ostream& operator<<(std::ostream& ostr, Padding padding) {
  switch (padding) {
    case Padding::SAME:
      return ostr << "SAME";
    case Padding::VALID:
      return ostr << "VALID";
  }
  return ostr;
}

// Tests that cudnnGetWorkspaceSize either returns unsupported status or a
// reasonable value.
//
// cuDNN before version 7 returns huge workspace sizes for some configurations
// that look like the internal computation overflowed.
//
// See nvbugs/1893243.
TEST(ConvolutionTest, GetWorkspaceSize_Overflow) {
  CudnnHandle handle;
  proto::TensorDescriptor input_desc;
  input_desc.set_data_type(proto::DATA_FLOAT);
  input_desc.set_format(proto::TENSOR_NCHW);
  for (int dim : {1, 128, 300, 300}) {
    input_desc.add_dimension(dim);
  }
  auto input = CreateTensorDescriptor(input_desc);

  proto::FilterDescriptor filter_desc;
  filter_desc.set_data_type(proto::DATA_FLOAT);
  filter_desc.set_format(proto::TENSOR_NCHW);
  for (int dim : {768, 128, 3, 3}) {
    filter_desc.add_dimension(dim);
  }
  auto filter = CreateFilterDescriptor(filter_desc);

  proto::ConvolutionDescriptor conv_desc;
  conv_desc.set_compute_mode(proto::DATA_FLOAT);
  for (int pad : {1, 1}) {
    conv_desc.add_pad(pad);
  }
  auto convolution = CreateConvolutionDescriptor(conv_desc);

  auto output_or =
      CreateOutputDescriptor(proto::TENSOR_NCHW, input, filter, convolution);
  ASSERT_TRUE(IsOk(output_or));
  auto output = std::move(output_or.ValueOrDie());

  size_t workspace_size = 0;
  auto status = cudnnGetConvolutionForwardWorkspaceSize(
      CreateCudnnHandle().get(), input.get(), filter.get(), convolution.get(),
      output.get(), CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED,
      &workspace_size);

  if (status == CUDNN_STATUS_SUCCESS) {
    EXPECT_LE(workspace_size, 1ull << 63);
  } else {
    EXPECT_EQ(status, CUDNN_STATUS_NOT_SUPPORTED);
  }
}

// Tests the supported range of the arrayLengthRequested parameter for
// cudnnGetConvolutionNdDescriptor, which should be [0, CUDNN_DIM_MAX]
// according to the documentation, but cuDNN reports CUDNN_STATUS_NOT_SUPPORTED
// for anything larger than 6.
//
// See nvbugs/2064417.
TEST(ConvolutionTest, GetConvolutionDesciptor_ArrayLengthRequested_Range) {
  proto::ConvolutionDescriptor proto;
  proto.set_compute_mode(proto::DATA_FLOAT);
  proto.add_pad(0);
  proto.add_pad(0);

  auto conv_desc = CreateConvolutionDescriptor(proto);

  const int array_length = CUDNN_DIM_MAX;

  int rank;
  int pad[array_length];
  int stride[array_length];
  int dilation[array_length];
  cudnnConvolutionMode_t convolution_mode;
  cudnnDataType_t compute_type;

  for (int array_length_requested = 0; array_length_requested <= array_length;
       ++array_length_requested) {
    EXPECT_TRUE(IsOk(GetStatus(cudnnGetConvolutionNdDescriptor(
                         conv_desc.get(), array_length_requested, &rank, pad,
                         stride, dilation, &convolution_mode, &compute_type))
                     << " array_length_requested = "
                     << array_length_requested));
  }
}
}  // namespace nvidia_libs_test
