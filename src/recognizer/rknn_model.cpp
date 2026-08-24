#include "plate_recognizer_internal.h"

#include <cstdio>
#include <fstream>

namespace plate_internal {

RknnModel::~RknnModel() {
  if (context_) {
    rknn_destroy(context_);
  }
}

bool RknnModel::load(const char *path, rknn_core_mask core, std::string &error) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    error = std::string("cannot open model: ") + path;
    return false;
  }
  const std::streamsize length = stream.tellg();
  if (length <= 0) {
    error = std::string("empty model: ") + path;
    return false;
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(bytes.data()), length);

  int status = rknn_init(
      &context_,
      bytes.data(),
      static_cast<uint32_t>(bytes.size()),
      0,
      nullptr);
  if (status != RKNN_SUCC) {
    error = "rknn_init failed: " + std::to_string(status);
    return false;
  }
  status = rknn_set_core_mask(context_, core);
  if (status != RKNN_SUCC) {
    std::fprintf(
        stderr,
        "warning: rknn_set_core_mask failed: %d\n",
        status);
  }

  rknn_input_output_num io{};
  status = rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
  if (status != RKNN_SUCC || io.n_input != 1) {
    error = "unexpected RKNN model I/O";
    return false;
  }
  output_count_ = io.n_output;
  return true;
}

bool RknnModel::infer(const cv::Mat &image,
                      std::vector<std::vector<float>> &result,
                      std::string &error) {
  if (!image.isContinuous() || image.type() != CV_8UC3) {
    error = "RKNN input must be continuous CV_8UC3";
    return false;
  }
  rknn_input input{};
  input.index = 0;
  input.buf = image.data;
  input.size = static_cast<uint32_t>(image.total() * image.elemSize());
  input.type = RKNN_TENSOR_UINT8;
  input.fmt = RKNN_TENSOR_NHWC;

  int status = rknn_inputs_set(context_, 1, &input);
  if (status != RKNN_SUCC) {
    error = "rknn_inputs_set failed: " + std::to_string(status);
    return false;
  }
  status = rknn_run(context_, nullptr);
  if (status != RKNN_SUCC) {
    error = "rknn_run failed: " + std::to_string(status);
    return false;
  }

  std::vector<rknn_output> outputs(output_count_);
  for (uint32_t index = 0; index < output_count_; ++index) {
    outputs[index].index = index;
    outputs[index].want_float = 1;
  }
  status = rknn_outputs_get(context_, output_count_, outputs.data(), nullptr);
  if (status != RKNN_SUCC) {
    error = "rknn_outputs_get failed: " + std::to_string(status);
    return false;
  }

  result.clear();
  result.reserve(output_count_);
  for (const rknn_output &output : outputs) {
    const size_t count = output.size / sizeof(float);
    const float *values = static_cast<const float *>(output.buf);
    result.emplace_back(values, values + count);
  }
  rknn_outputs_release(context_, output_count_, outputs.data());
  return true;
}

}  // namespace plate_internal
