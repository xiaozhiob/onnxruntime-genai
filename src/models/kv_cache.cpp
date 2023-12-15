#include "../generators.h"
#include "kv_cache.h"

namespace Generators {

KV_Cache_Combined::KV_Cache_Combined(const SearchParams& search_params, const Config& config, Ort::Allocator& allocator, cudaStream_t cuda_stream, ONNXTensorElementDataType score_type)
    : allocator_{allocator},
      cuda_stream_{cuda_stream},
      score_type_{score_type},
      layer_count_{config.num_hidden_layers},
      is_cuda_{allocator_.GetInfo().GetDeviceType() == OrtMemoryInfoDeviceType_GPU},
      shape_{2, search_params.batch_size * search_params.num_beams, config.num_attention_heads, 0, config.hidden_size},
      empty_past_{OrtValue::CreateTensor(allocator, shape_, score_type_)} {
  pasts_.resize(layer_count_);
  presents_.reserve(layer_count_);

  shape_[3] = search_params.sequence_length;
  for (int i = 0; i < layer_count_; ++i) {
    presents_.push_back(OrtValue::CreateTensor(allocator, shape_, score_type_));

    char string[32];
    snprintf(string, std::size(string), past_name_, i);
    input_name_strings_.push_back(string);

    snprintf(string, std::size(string), present_name_, i);
    output_name_strings_.push_back(string);
  }
}

void KV_Cache_Combined::Update(std::span<const int32_t> beam_indices, int current_length) {
  for (int i = 0; i < layer_count_; i++) {
    if (beam_indices.empty())
      pasts_[i] = std::move(presents_[i]);
    else
      PickPastState(beam_indices, i);
  }

  shape_[3] = current_length;
  for (int i = 0; i < layer_count_; i++)
    presents_[i] = OrtValue::CreateTensor(allocator_, shape_, score_type_);
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache_Combined::PickPastState(std::span<const int32_t> beam_indices, int index) {
  auto block_size_per_beam = shape_[2] * shape_[3] * shape_[4];
  auto past_key_size = shape_[1] * block_size_per_beam;
  auto element_count = shape_[0] * past_key_size;

  const OrtValue& present = *presents_[index];
  auto past = OrtValue::CreateTensor<ScoreType>(allocator_, shape_);
  auto past_span = std::span<ScoreType>(past->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (is_cuda_) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past_key.data(), present_key.data(), present_key.size_bytes(), cudaMemcpyDeviceToDevice, cuda_stream_);
      cudaMemcpyAsync(past_value.data(), present_value.data(), present_value.size_bytes(), cudaMemcpyDeviceToDevice, cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      copy(present_key, past_key);
      copy(present_value, past_value);
    }
  }

  pasts_[index] = std::move(past);
}

void KV_Cache_Combined::PickPastState(std::span<const int32_t> beam_indices, int index) {
  if (score_type_ == Ort::TypeToTensorType<float>::type)
    PickPastState<float>(beam_indices, index);
  else
    PickPastState<Ort::Float16_t>(beam_indices, index);
}

KV_Cache::KV_Cache(const SearchParams& search_params, const Config& config, Ort::Allocator& allocator, cudaStream_t cuda_stream, ONNXTensorElementDataType score_type)
    : allocator_{allocator},
      cuda_stream_{cuda_stream},
      score_type_{score_type},
      layer_count_{config.num_hidden_layers},
      is_cuda_{allocator_.GetInfo().GetDeviceType() == OrtMemoryInfoDeviceType_GPU},
      shape_{search_params.batch_size * search_params.num_beams, config.num_attention_heads, 0, config.hidden_size},
      empty_past_{OrtValue::CreateTensor(allocator, shape_, score_type_)} {
  pasts_.resize(layer_count_ * 2);
  presents_.reserve(layer_count_ * 2);

  shape_[2] = search_params.sequence_length;
  for (int i = 0; i < layer_count_; ++i) {
    presents_.push_back(OrtValue::CreateTensor(allocator, shape_, score_type_));
    presents_.push_back(OrtValue::CreateTensor(allocator, shape_, score_type_));

    char string[32];
    snprintf(string, std::size(string), past_key_name_, i);
    input_name_strings_.push_back(string);
    snprintf(string, std::size(string), past_value_name_, i);
    input_name_strings_.push_back(string);

    snprintf(string, std::size(string), present_key_name_, i);
    output_name_strings_.push_back(string);
    snprintf(string, std::size(string), present_value_name_, i);
    output_name_strings_.push_back(string);
  }
}

void KV_Cache::Update(std::span<const int32_t> beam_indices, int current_length) {
  for (int i = 0; i < layer_count_ * 2; i++) {
    if (beam_indices.empty())
      pasts_[i] = std::move(presents_[i]);
    else
      PickPastState(beam_indices, i);
  }

  shape_[2] = current_length;
  for (int i = 0; i < layer_count_ * 2; i++)
    presents_[i] = OrtValue::CreateTensor(allocator_, shape_, score_type_);
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache::PickPastState(std::span<const int32_t> beam_indices, int index) {
  auto block_size_per_beam = shape_[1] * shape_[2] * shape_[3];
  auto element_count = shape_[0] * block_size_per_beam;

  const OrtValue& present = *presents_[index];
  auto past = OrtValue::CreateTensor<ScoreType>(allocator_, shape_);
  auto past_span = std::span<ScoreType>(past->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (is_cuda_) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past.data(), present.data(), present.size_bytes(), cudaMemcpyDeviceToDevice, cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      copy(present, past);
    }
  }

  pasts_[index] = std::move(past);
}

void KV_Cache::PickPastState(std::span<const int32_t> beam_indices, int index) {
  if (score_type_ == Ort::TypeToTensorType<float>::type)
    PickPastState<float>(beam_indices, index);
  else
    PickPastState<Ort::Float16_t>(beam_indices, index);
}

}  // namespace Generators