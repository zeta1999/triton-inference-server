// Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/backends/tensorflow/base_backend.h"

#include <set>
#include "src/backends/tensorflow/tf_utils.h"
#include "src/backends/tensorflow/tf_virtual_device.h"
#include "src/core/constants.h"
#include "src/core/cuda_utils.h"
#include "src/core/logging.h"
#include "src/core/model_config.pb.h"
#include "src/core/model_config_utils.h"
#include "src/core/server_status.h"

#ifdef TRTIS_ENABLE_GPU
#include <cuda_runtime_api.h>
#endif  // TRTIS_ENABLE_GPU

namespace nvidia { namespace inferenceserver {

BaseBackend::Context::Context(
    const std::string& name, const int gpu_device, const int max_batch_size,
    const bool enable_pinned_input, const bool enable_pinned_output)
    : BackendContext(
          name, gpu_device, max_batch_size, enable_pinned_input,
          enable_pinned_output),
      trtistf_model_(nullptr, TRTISTF_ModelDelete),
      input_device_id_(MODEL_DEVICE)
{
}

BaseBackend::Context::~Context()
{
  LOG_VERBOSE(1) << "~BaseBackend::Context ";
}

Status
BaseBackend::Init(
    const std::string& path, const ModelConfig& model_config,
    const GraphDefBackendFactory::Config* backend_config,
    const std::string& platform)
{
  RETURN_IF_ERROR(InferenceBackend::Init(path, model_config, platform));
  backend_config_ = backend_config;
  return Status::Success;
}

Status
BaseBackend::CreateExecutionContexts(
    const std::unordered_map<std::string, std::string>& paths)
{
  if (LOG_VERBOSE_IS_ON(1)) {
    LOG_INFO << "Creating execution contexts for:";
    for (const auto p : paths) {
      LOG_INFO << "  " << p.first << ": " << p.second;
    }
  }
  uint32_t total_context_cnt = 0;

  for (const auto& group : Config().instance_group()) {
    for (int c = 0; c < group.count(); c++) {
      if (group.kind() == ModelInstanceGroup::KIND_CPU) {
        const std::string instance_name =
            group.name() + "_" + std::to_string(c) + "_cpu";
        RETURN_IF_ERROR(CreateExecutionContext(
            instance_name, Context::NO_GPU_DEVICE, paths));
        total_context_cnt++;
      } else if (group.kind() == ModelInstanceGroup::KIND_MODEL) {
        const std::string instance_name =
            group.name() + "_" + std::to_string(c) + "_model_device";
        RETURN_IF_ERROR(CreateExecutionContext(
            instance_name, Context::MODEL_DEVICE, paths));
        total_context_cnt++;
      } else {
        for (int gpu_device : group.gpus()) {
          const std::string instance_name = group.name() + "_" +
                                            std::to_string(c) + "_gpu" +
                                            std::to_string(gpu_device);
          RETURN_IF_ERROR(
              CreateExecutionContext(instance_name, gpu_device, paths));
          total_context_cnt++;
        }
      }
    }
  }

  // Create a scheduler with one thread for each context available for
  // this model. Each runner is exclusively tied to the context.
  RETURN_IF_ERROR(SetConfiguredScheduler(
      total_context_cnt,
      [](uint32_t runner_idx) -> Status { return Status::Success; },
      [this](
          uint32_t runner_idx, std::vector<Scheduler::Payload>* payloads,
          std::function<void(Status)> func) {
        Run(runner_idx, payloads, func);
      },
      [this](
          uint32_t runner_idx, const InferenceRequest::Input& input,
          const Scheduler::Payload& payload,
          std::vector<int64_t>* shape) -> Status { return Status::Success; }));

  LOG_VERBOSE(1) << "backend for " << Name() << std::endl << *this;

  return Status::Success;
}

Status
BaseBackend::CreateExecutionContext(
    const std::string& instance_name, const int gpu_device,
    const std::unordered_map<std::string, std::string>& paths)
{
  // For a GPU context, determine the model file to use for device
  // compute capability. CPU always uses the default model file.
  std::string cc_model_filename;
  int vgpu_device = gpu_device;

  if (gpu_device == Context::NO_GPU_DEVICE) {
    cc_model_filename = Config().default_model_filename();

    LOG_INFO << "Creating instance " << instance_name << " on CPU using "
             << cc_model_filename;
  } else if (gpu_device == Context::MODEL_DEVICE) {
    cc_model_filename = Config().default_model_filename();

    LOG_INFO << "Creating instance " << instance_name
             << " on devices as specified in " << cc_model_filename;
  } else {
#ifdef TRTIS_ENABLE_GPU
    cudaDeviceProp cuprops;
    cudaError_t cuerr = cudaGetDeviceProperties(&cuprops, gpu_device);
    if (cuerr != cudaSuccess) {
      return Status(
          Status::Code::INTERNAL, "unable to get CUDA device properties for " +
                                      Name() + ": " +
                                      cudaGetErrorString(cuerr));
    }

    const std::string cc =
        std::to_string(cuprops.major) + "." + std::to_string(cuprops.minor);
    const auto& cc_itr = Config().cc_model_filenames().find(cc);
    cc_model_filename = (cc_itr == Config().cc_model_filenames().end())
                            ? Config().default_model_filename()
                            : cc_itr->second;

    // Get virtual device tracker instance, and get next device id
    if (VirtualDeviceTracker::HasVirtualDevice()) {
      RETURN_IF_ERROR(
          VirtualDeviceTracker::GetNextVirtualDevice(gpu_device, &vgpu_device));
    }

    LOG_INFO << "Creating instance " << instance_name << " on GPU "
             << vgpu_device << " (" << cc << ") using " << cc_model_filename;
#else
    return Status(Status::Code::INTERNAL, "GPU instances not supported");
#endif  // TRTIS_ENABLE_GPU
  }

  const auto& gdp_itr = paths.find(cc_model_filename);
  if (gdp_itr == paths.end()) {
    return Status(
        Status::Code::INTERNAL,
        "unable to find model '" + cc_model_filename + "' for " + Name());
  }

  // Max batch size. A value of 0 in the config becomes NO_BATCHING.
  const int mbs = (Config().max_batch_size() <= 0) ? Context::NO_BATCHING
                                                   : Config().max_batch_size();
  const bool pinned_input =
      Config().optimization().input_pinned_memory().enable();
  const bool pinned_output =
      Config().optimization().output_pinned_memory().enable();

  contexts_.emplace_back(
      new Context(instance_name, gpu_device, mbs, pinned_input, pinned_output));
  Context* context = static_cast<Context*>(contexts_.back().get());

  RETURN_IF_ERROR(context->CreateCudaStream());

  RETURN_IF_ERROR(context->ValidateInputs(Config().input()));
  RETURN_IF_ERROR(context->ValidateOutputs(Config().output()));

  TRTISTF_TFTRTConfig* tftrt_config_ptr = nullptr;
  TRTISTF_TFTRTConfig tftrt_config;
  if (Config().optimization().has_execution_accelerators()) {
    // Set default values. is_dynamic_op is always true for online
    // TF-TRT.
    tftrt_config.minimum_segment_size_ = 3;
    tftrt_config.max_workspace_size_bytes_ = 1 << 30;
    tftrt_config.max_cached_engines_ = 100;
    tftrt_config.max_batch_size_ = std::max(Config().max_batch_size(), 1);
    tftrt_config.precision_mode_ = TRTISTF_MODE_FP32;
    tftrt_config.is_dynamic_op_ = true;

    if (!Config()
             .optimization()
             .execution_accelerators()
             .cpu_execution_accelerator()
             .empty()) {
      return Status(
          Status::Code::INVALID_ARG,
          "CPU Execution Accelerator is not supported in TensorFlow backend");
    }

    if (gpu_device == Context::NO_GPU_DEVICE) {
      return Status(
          Status::Code::INVALID_ARG,
          "GPU Execution Accelerator can only be set on non-CPU backend "
          "context");
    }
    for (const auto& execution_accelerator : Config()
                                                 .optimization()
                                                 .execution_accelerators()
                                                 .gpu_execution_accelerator()) {
      if (execution_accelerator.name() == kTensorRTExecutionAccelerator) {
        // Validate and set parameters
        for (const auto& parameter : execution_accelerator.parameters()) {
          if (parameter.first == "precision_mode") {
            if (parameter.second == "FP32") {
              tftrt_config.precision_mode_ = TRTISTF_MODE_FP32;
            } else if (parameter.second == "FP16") {
              tftrt_config.precision_mode_ = TRTISTF_MODE_FP16;
            } else {
              return Status(
                  Status::Code::INVALID_ARG, "unsupported precision mode '" +
                                                 parameter.second +
                                                 "' is requested");
            }
          } else if (parameter.first == "minimum_segment_size") {
            RETURN_IF_ERROR(ParseLongLongParameter(
                parameter.first, parameter.second,
                &tftrt_config.minimum_segment_size_));
          } else if (parameter.first == "max_workspace_size_bytes") {
            RETURN_IF_ERROR(ParseLongLongParameter(
                parameter.first, parameter.second,
                &tftrt_config.max_workspace_size_bytes_));
          } else if (parameter.first == "max_cached_engines") {
            RETURN_IF_ERROR(ParseLongLongParameter(
                parameter.first, parameter.second,
                &tftrt_config.max_cached_engines_));
          } else {
            return Status(
                Status::Code::INVALID_ARG,
                "unknown parameter '" + parameter.first +
                    "' is provided for TensorRT Execution Accelerator");
          }
        }
        LOG_VERBOSE(1) << "TensorRT Execution Accelerator is set for "
                       << instance_name;
      } else if (execution_accelerator.name() == kGPUIOExecutionAccelerator) {
        // GPU I/O can be set, set hint
        if ((gpu_device != Context::NO_GPU_DEVICE) &&
            (gpu_device != Context::MODEL_DEVICE)) {
          // In TensorFlow, TF device (vGPU) is used for device utilities
          context->input_device_id_ = vgpu_device;
        }
      } else {
        return Status(
            Status::Code::INVALID_ARG, "unknown Execution Accelerator '" +
                                           execution_accelerator.name() +
                                           "' is requested");
      }
    }
    tftrt_config_ptr = &tftrt_config;
  }

  RETURN_IF_ERROR(CreateTRTISTFModel(
      backend_config_, vgpu_device, Config().optimization().has_graph(),
      Config().optimization().graph().level(), gdp_itr->first, gdp_itr->second,
      &context->trtistf_model_, &context->input_name_map_,
      &context->output_name_map_, tftrt_config_ptr));


  if (context->input_device_id_ != Context::MODEL_DEVICE) {
    const size_t num_inputs = Config().input_size();
    const size_t num_outputs = Config().output_size();
    std::vector<const char*> input_names, output_names;
    std::vector<TRTISTF_DataType> input_types, output_types;
    for (const auto& io : Config().input()) {
      input_names.push_back(io.name().c_str());
      input_types.push_back(ConvertDataType(io.data_type()));
    }
    for (const auto& io : Config().output()) {
      output_names.push_back(io.name().c_str());
      output_types.push_back(ConvertDataType(io.data_type()));
    }
    TRTISTF_ModelMakeCallable(
        context->trtistf_model_.get(), input_names.data(), input_types.data(),
        num_inputs, output_names.data(), output_types.data(), num_outputs);
  }

  return Status::Success;
}

Status
BaseBackend::Context::ValidateInputs(
    const ::google::protobuf::RepeatedPtrField<ModelInput>& ios)
{
  for (const auto& io : ios) {
    if (ConvertDataType(io.data_type()) ==
        TRTISTF_DataType::TRTISTF_TYPE_INVALID) {
      return Status(
          Status::Code::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for input '" + io.name() + "' for model '" + name_ + "'");
    }
  }

  return Status::Success;
}


Status
BaseBackend::Context::ValidateOutputs(
    const ::google::protobuf::RepeatedPtrField<ModelOutput>& ios)
{
  for (const auto& io : ios) {
    if (ConvertDataType(io.data_type()) ==
        TRTISTF_DataType::TRTISTF_TYPE_INVALID) {
      return Status(
          Status::Code::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for output '" + io.name() + "' for model '" + name_ + "'");
    }
  }

  return Status::Success;
}

namespace {

void
FillStringTensor(TRTISTF_Tensor* tensor, const size_t idx, const size_t cnt)
{
  for (size_t c = 0; c < cnt; ++c) {
    TRTISTF_TensorSetString(tensor, idx + c, nullptr, 0);
  }
}

}  // namespace

Status
BaseBackend::Context::SetInput(
    const std::string& name, const DataType datatype,
    const std::vector<int64_t>& dims, const size_t total_batch_size,
    std::vector<Scheduler::Payload>* payloads, std::vector<InputInfo>* inputs,
    TRTISTF_TensorList** input_tensors, bool* cuda_copy)
{
  // Get the shape of the input. The request normalizer has already
  // checked that the request shape is valid so don't need to do it
  // here.
  std::vector<int64_t> shape;

  // If model supports batching then prepend the batch dimension
  // onto the input shape.
  if (max_batch_size_ != NO_BATCHING) {
    shape.push_back(total_batch_size);
  }

  size_t batch1_element_cnt = 1;
  for (auto dim : dims) {
    shape.push_back(dim);
    batch1_element_cnt *= dim;
  }

  const std::string* input_tensor_name = &name;
  const auto& tn_itr = input_name_map_.find(name);
  if (tn_itr != input_name_map_.end()) {
    input_tensor_name = &tn_itr->second;
  }

  // Only try to create a tensor on specific device if 'input_device_id_' is set
  const TRTISTF_DataType dtype = ConvertDataType(datatype);
  TRTISTF_Tensor* tensor = TRTISTF_TensorNew(
      input_tensor_name->c_str(), dtype, shape.size(),
      (shape.size() == 0) ? nullptr : &shape[0], input_device_id_);
  if (tensor == nullptr) {
    return Status(
        Status::Code::INTERNAL,
        "failed to create input tensor '" + name + "' with shape " +
            DimsListToString(shape) + " and data type " +
            DataType_Name(datatype) + " for '" + name_ + "'");
  }

  TRTISTF_TensorList* tlink = TRTISTF_TensorListNew(tensor, *input_tensors);
  *input_tensors = tlink;

  if (dtype != TRTISTF_DataType::TRTISTF_TYPE_STRING) {
    const size_t batch1_byte_size =
        batch1_element_cnt * TRTISTF_TensorDataTypeByteSize(tensor);
    if ((batch1_byte_size * total_batch_size) !=
        TRTISTF_TensorDataByteSize(tensor)) {
      return Status(
          Status::Code::INTERNAL,
          "failed to create input tensor '" + name +
              "' with expected byte size " +
              std::to_string(batch1_byte_size * total_batch_size) + ", got " +
              std::to_string(TRTISTF_TensorDataByteSize(tensor)));
    }
    inputs->emplace_back();
    SetFixedSizedInputTensor(
        tensor, name, batch1_byte_size, payloads, &inputs->back(), cuda_copy);
  } else {
    SetStringInputTensor(tensor, name, batch1_element_cnt, payloads);
  }

  return Status::Success;
}

void
BaseBackend::Context::SetFixedSizedInputTensor(
    TRTISTF_Tensor* tensor, const std::string& input_name,
    const size_t batch1_byte_size, std::vector<Scheduler::Payload>* payloads,
    InputInfo* input, bool* cuda_copy)
{
  input->input_buffer_ = TRTISTF_TensorData(tensor);

  // Visit the payloads in order and copy the input values into the
  // input tensor. Skip payloads that had errors since they are not
  // included in the dynamic batch.
  std::vector<size_t> expected_byte_sizes;
  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;
    expected_byte_sizes.push_back(irequest->BatchSize() * batch1_byte_size);
  }

  input->memory_type_ = (TRTISTF_TensorIsGPUTensor(tensor))
                            ? TRITONSERVER_MEMORY_GPU
                            : TRITONSERVER_MEMORY_CPU;
  input->memory_type_id_ =
      (TRTISTF_TensorIsGPUTensor(tensor)) ? gpu_device_ : 0;
  LOG_VERBOSE(1) << "input '" << input_name
                 << "' is GPU tensor: " << TRTISTF_TensorIsGPUTensor(tensor);
  *cuda_copy |=
      SetInputBuffer(input_name, expected_byte_sizes, payloads, input);
}

void
BaseBackend::Context::SetStringInputTensor(
    TRTISTF_Tensor* tensor, const std::string& input_name,
    const size_t batch1_element_cnt, std::vector<Scheduler::Payload>* payloads)
{
  size_t tensor_element_idx = 0;

  // Visit the payloads in order and copy the input values into the
  // input tensor. Skip payloads that had errors since they are not
  // included in the dynamic batch.
  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;
    const size_t expected_element_cnt =
        irequest->BatchSize() * batch1_element_cnt;
    size_t element_idx = 0;

    // For string data type, we always need to copy the data to CPU so that
    // we can read string length and construct the string properly.
    auto buffer_memory_type = TRITONSERVER_MEMORY_CPU_PINNED;
    int64_t buffer_memory_type_id = 0;
    const char* content;
    size_t content_byte_size = expected_element_cnt * sizeof(uint32_t);
    // If contiguous buffer is created, it needs to live until tensor is filled
    std::unique_ptr<AllocatedMemory> contiguous_buffer;
    bool cuda_copy = false;
    payload.status_ = GetContiguousInputContent(
        input_name, buffer_memory_type, buffer_memory_type_id, payload,
        &content, &content_byte_size, &contiguous_buffer, &cuda_copy);

    if (!payload.status_.IsOk()) {
      FillStringTensor(
          tensor, tensor_element_idx + element_idx,
          expected_element_cnt - element_idx);
      continue;
    }

    // [TODO] defer synchronize as far as possible, need rework on setting
    // String input. i.e. get all contiguous data first, then sync and set.
#ifdef TRTIS_ENABLE_GPU
    if (cuda_copy) {
      cudaStreamSynchronize(stream_);
    }
#endif  // TRTIS_ENABLE_GPU

    // Parse content and assign them to the 'tensor'. Each string
    // in 'content' is a 4-byte length followed by the string
    // itself with no null-terminator.
    while (content_byte_size >= sizeof(uint32_t)) {
      if (element_idx >= expected_element_cnt) {
        payload.status_ = Status(
            Status::Code::INVALID_ARG,
            "unexpected number of string elements " +
                std::to_string(element_idx + 1) + " for inference input '" +
                input_name + "', expecting " +
                std::to_string(expected_element_cnt));
        FillStringTensor(
            tensor, tensor_element_idx + element_idx,
            expected_element_cnt - element_idx);
        break;
      }

      const uint32_t len = *(reinterpret_cast<const uint32_t*>(content));
      content += sizeof(uint32_t);
      content_byte_size -= sizeof(uint32_t);

      if (content_byte_size < len) {
        payload.status_ = Status(
            Status::Code::INVALID_ARG,
            "incomplete string data for inference input '" + input_name +
                "', expecting string of length " + std::to_string(len) +
                " but only " + std::to_string(content_byte_size) +
                " bytes available");
        FillStringTensor(
            tensor, tensor_element_idx + element_idx,
            expected_element_cnt - element_idx);
        break;
      }

      TRTISTF_TensorSetString(
          tensor, tensor_element_idx + element_idx, content, len);
      content += len;
      content_byte_size -= len;
      element_idx++;
    }

    if (payload.status_.IsOk() && (element_idx != expected_element_cnt)) {
      payload.status_ = Status(
          Status::Code::INTERNAL,
          "expected " + std::to_string(expected_element_cnt) +
              " strings for inference input '" + input_name + "', got " +
              std::to_string(element_idx));
      FillStringTensor(
          tensor, tensor_element_idx + element_idx,
          expected_element_cnt - element_idx);
    }

    tensor_element_idx += expected_element_cnt;
  }
}

void
BaseBackend::Context::ReadFixedSizedOutputTensor(
    TRTISTF_Tensor* tensor, const std::string& output_name,
    const size_t batch1_byte_size, std::vector<Scheduler::Payload>* payloads,
    OutputInfo* output, bool* cuda_copy)
{
  output->output_buffer_ = TRTISTF_TensorData(tensor);
  output->memory_type_ = (TRTISTF_TensorIsGPUTensor(tensor))
                             ? TRITONSERVER_MEMORY_GPU
                             : TRITONSERVER_MEMORY_CPU;
  output->memory_type_id_ =
      (TRTISTF_TensorIsGPUTensor(tensor)) ? gpu_device_ : 0;
  LOG_VERBOSE(1) << "output '" << output_name
                 << "' is GPU tensor: " << TRTISTF_TensorIsGPUTensor(tensor);
  *cuda_copy |=
      SetFixedSizeOutputBuffer(output_name, batch1_byte_size, output, payloads);
}

void
BaseBackend::Context::ReadStringOutputTensor(
    TRTISTF_Tensor* tensor, const std::string& output_name,
    const std::vector<int64_t>& shape, const size_t batch1_element_cnt,
    std::vector<Scheduler::Payload>* payloads, bool* cuda_copy)
{
  size_t tensor_element_idx = 0;

  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;
    const size_t expected_element_cnt =
        irequest->BatchSize() * batch1_element_cnt;

    // If 'payload' should have valid output (status ok) and
    // if 'payload' requested this output then copy it from
    // tensor. If it did not request this output then just
    // skip it.
    if (payload.status_.IsOk() && (payload.response_provider_ != nullptr) &&
        payload.response_provider_->RequiresOutput(output_name)) {
      // Serialize the output tensor strings. Each string is
      // serialized as a 4-byte length followed by the string itself
      // with no null-terminator.
      std::string serialized;
      for (size_t e = 0; e < expected_element_cnt; ++e) {
        size_t len;
        const char* cstr =
            TRTISTF_TensorString(tensor, tensor_element_idx + e, &len);
        serialized.append(
            reinterpret_cast<const char*>(&len), sizeof(uint32_t));
        if (len > 0) {
          serialized.append(cstr, len);
        }
      }

      void* content;
      TRITONSERVER_MemoryType actual_memory_type;
      int64_t actual_memory_type_id;
      Status status = payload.response_provider_->AllocateOutputBuffer(
          output_name, &content, serialized.size(), shape,
          TRITONSERVER_MEMORY_CPU_PINNED /* preferred_memory_type */,
          0 /* preferred_memory_type_id */, &actual_memory_type,
          &actual_memory_type_id);
      if (status.IsOk()) {
        bool cuda_used = false;
        status = CopyBuffer(
            output_name, TRITONSERVER_MEMORY_CPU /* src_memory_type */,
            0 /* src_memory_type_id */, actual_memory_type,
            actual_memory_type_id, serialized.size(),
            reinterpret_cast<const void*>(serialized.c_str()), content, stream_,
            &cuda_used);
        *cuda_copy |= cuda_used;
      }

      payload.status_ = status;
    }

    tensor_element_idx += expected_element_cnt;
  }
}

Status
BaseBackend::Context::Run(
    const InferenceBackend* base, std::vector<Scheduler::Payload>* payloads)
{
  LOG_VERBOSE(1) << "Running " << name_ << " with " << payloads->size()
                 << " request payloads";

  const InferenceRequest* repr_input_request = nullptr;

  // For each request in 'payloads' collect the total batch size for
  // this inference execution. The batch-size, number of inputs, and
  // size of each input has already been checked by each payloads
  // request normalizer so don't need to do that here.
  size_t total_batch_size = 0;
  for (auto& payload : *payloads) {
    if (!payload.status_.IsOk()) {
      return Status(
          Status::Code::INTERNAL,
          "unexpected payload with non-OK status given to runner for '" +
              name_ + "'");
    }

    total_batch_size += payload.request_->BatchSize();

    // All payloads must have equally-sized input tensors so use any
    // payload as the representative for the input tensors.
    repr_input_request = payload.request_.get();
  }

  // If there are no valid payloads then no need to run the
  // inference. The payloads will have their error status set so can
  // just return.
  if (total_batch_size == 0) {
    return Status::Success;
  }

  // total_batch_size can be 1 for models that don't support batching
  // (i.e. max_batch_size_ == 0).
  if ((total_batch_size != 1) && (total_batch_size > (size_t)max_batch_size_)) {
    return Status(
        Status::Code::INTERNAL,
        "dynamic batch size " + std::to_string(total_batch_size) + " for '" +
            name_ + "', max allowed is " + std::to_string(max_batch_size_));
  }

  // Create a tensor for each input sized correctly for the total
  // payload batch size. Concatenate input values from each payload
  // into the corresponding tensor.

  // Smart pointer is TensorList** as the pointer to input head (TensorList*)
  // will be updated in SetInput()
  TRTISTF_TensorList* input_head_ptr = nullptr;
  static auto input_deleter = [](TRTISTF_TensorList** list) {
    if (list != nullptr) {
      TRTISTF_TensorListDelete(*list);
    }
  };
  std::unique_ptr<TRTISTF_TensorList*, decltype(input_deleter)> input_tensors(
      &input_head_ptr, input_deleter);

  // Inputs from the request...
  std::vector<InputInfo> inputs;
  bool cuda_copy = false;
  for (const auto& pr : repr_input_request->ImmutableInputs()) {
    const InferenceRequest::Input* input = pr.second;
    const std::string& name = input->Name();

    RETURN_IF_ERROR(SetInput(
        name, input->DType(), input->Shape(), total_batch_size, payloads,
        &inputs, input_tensors.get(), &cuda_copy));
  }

  // Collect the names of outputs requested by any request
  // payload.
  std::set<std::string> required_outputs;
  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;
    for (const auto& pr : irequest->RequestedOutputs()) {
      required_outputs.insert(pr.first);
    }
  }

  // Create the vector of required output names using the names
  // expected by the model.
  std::vector<std::string> model_output_names;
  const char* output_names_cstr[required_outputs.size()];
  {
    size_t oidx = 0;
    for (const auto& name : required_outputs) {
      model_output_names.push_back(name);
      const auto& tn_itr = output_name_map_.find(name);
      if (tn_itr == output_name_map_.end()) {
        output_names_cstr[oidx] = name.c_str();
      } else {
        output_names_cstr[oidx] = tn_itr->second.c_str();
      }
      oidx++;
    }
  }

#ifdef TRTIS_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
  cuda_copy = false;
  for (auto& input : inputs) {
    for (auto& indirect_buffer : input.indirect_buffers_) {
      bool cuda_used;
      TRITONSERVER_MemoryType buffer_memory_type;
      int64_t buffer_memory_id;
      size_t buffer_byte_size;
      auto buffer =
          std::get<0>(indirect_buffer)
              ->BufferAt(
                  0, &buffer_byte_size, &buffer_memory_type, &buffer_memory_id);
      auto status = CopyBuffer(
          "indirect buffer", buffer_memory_type, buffer_memory_id,
          input.memory_type_, input.memory_type_id_, buffer_byte_size, buffer,
          input.input_buffer_ + std::get<1>(indirect_buffer), stream_,
          &cuda_used);
      if (!status.IsOk()) {
        for (const auto& payload_idx : std::get<2>(indirect_buffer)) {
          (*payloads)[payload_idx].status_ = status;
        }
      } else {
        cuda_copy |= cuda_used;
      }
    }
  }
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
#endif  // TRTIS_ENABLE_GPU

#ifdef TRTIS_ENABLE_STATS
  for (auto& payload : *payloads) {
    if (payload.stats_ != nullptr) {
      payload.stats_->CaptureTimestamp(
          ModelInferStats::TimestampKind::kComputeInputEnd);
    }
  }
#endif  // TRTIS_ENABLE_STATS

  // Run. Session will update the 'output_tensors'.
  std::unique_ptr<TRTISTF_TensorList, decltype(&TRTISTF_TensorListDelete)>
      output_tensors(nullptr, TRTISTF_TensorListDelete);

  {
    TRTISTF_TensorList* rtl;
    RETURN_IF_TRTISTF_ERROR(TRTISTF_ModelRun(
        trtistf_model_.get(), *(input_tensors.release()),
        required_outputs.size(), output_names_cstr, &rtl));
    output_tensors.reset(rtl);
  }

#ifdef TRTIS_ENABLE_STATS
  for (auto& payload : *payloads) {
    if (payload.stats_ != nullptr) {
      payload.stats_->CaptureTimestamp(
          ModelInferStats::TimestampKind::kComputeOutputStart);
    }
  }
#endif  // TRTIS_ENABLE_STATS

  // Make sure each output is of the expected size and copy it into
  // the appropriate response providers.
  cuda_copy = false;
  std::vector<OutputInfo> outputs;
  TRTISTF_TensorList* output_tensor_itr = output_tensors.get();
  for (const auto& name : model_output_names) {
    outputs.emplace_back();
    const ModelOutput* output_config;
    RETURN_IF_ERROR(base->GetOutput(name, &output_config));

    TRTISTF_Tensor* output_tensor = output_tensor_itr->tensor_;

    // Get the shape and datatype of the output from the output
    // tensor.
    TRTISTF_Shape* shape = TRTISTF_TensorShape(output_tensor);

    bool skip_element_cnt = (max_batch_size_ != NO_BATCHING);
    size_t batch1_element_cnt = 1;
    for (size_t itr = 0; itr < shape->rank_; itr++) {
      const int64_t dim = shape->dims_[itr];
      outputs.back().output_shape_.push_back(dim);
      if (!skip_element_cnt) {
        batch1_element_cnt *= dim;
      }
      skip_element_cnt = false;
    }

    const DimsList& output_dims = (output_config->has_reshape())
                                      ? output_config->reshape().shape()
                                      : output_config->dims();

    // verify shape of output matches shape from model config
    RETURN_IF_ERROR(CompareOutputDims(
        name, outputs.back().output_shape_, output_dims,
        max_batch_size_ != NO_BATCHING /* supports_batching */));

    TRTISTF_DataType dtype = ConvertDataType(output_config->data_type());
    if (dtype != TRTISTF_TensorDataType(output_tensor)) {
      return Status(
          Status::Code::INVALID_ARG,
          "unexpected datatype " +
              DataType_Name(
                  ConvertDataType(TRTISTF_TensorDataType(output_tensor))) +
              " for inference output '" + name + "', expecting " +
              DataType_Name(output_config->data_type()));
    }

    if (dtype != TRTISTF_DataType::TRTISTF_TYPE_STRING) {
      const size_t batch1_byte_size =
          batch1_element_cnt * TRTISTF_TensorDataTypeByteSize(output_tensor);
      if ((batch1_byte_size * total_batch_size) !=
          TRTISTF_TensorDataByteSize(output_tensor)) {
        return Status(
            Status::Code::INVALID_ARG,
            "unexpected size for output '" + name + "', byte-size " +
                std::to_string(TRTISTF_TensorDataByteSize(output_tensor)) +
                " does not equal " + std::to_string(total_batch_size) + " * " +
                std::to_string(batch1_byte_size));
      }
      ReadFixedSizedOutputTensor(
          output_tensor, name, batch1_byte_size, payloads, &outputs.back(),
          &cuda_copy);
    } else {
      ReadStringOutputTensor(
          output_tensor, name, outputs.back().output_shape_, batch1_element_cnt,
          payloads, &cuda_copy);
    }

    output_tensor_itr = output_tensor_itr->next_;
  }

#ifdef TRTIS_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
  cuda_copy = false;
  for (auto& output : outputs) {
    for (auto& indirect_buffer : output.indirect_buffers_) {
      bool cuda_used;
      TRITONSERVER_MemoryType src_memory_type;
      int64_t src_memory_type_id;
      // placeholder, copy byte size is determined by dst_byte_size
      size_t src_byte_size;
      auto src = indirect_buffer.first->BufferAt(
          0, &src_byte_size, &src_memory_type, &src_memory_type_id);
      TRITONSERVER_MemoryType dst_memory_type;
      int64_t dst_memory_type_id;
      for (auto& payload_output : indirect_buffer.second) {
        char* dst = payload_output.second->MutableBuffer(
            &dst_memory_type, &dst_memory_type_id);
        auto dst_byte_size = payload_output.second->TotalByteSize();
        (*payloads)[payload_output.first].status_ = CopyBuffer(
            "indirect buffer", src_memory_type, src_memory_type_id,
            dst_memory_type, dst_memory_type_id, dst_byte_size, src, dst,
            stream_, &cuda_used);
        cuda_copy |= cuda_used;
        src += dst_byte_size;
      }
    }
  }
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
#endif  // TRTIS_ENABLE_GPU

  return Status::Success;
}

std::ostream&
operator<<(std::ostream& out, const BaseBackend& pb)
{
  out << "name=" << pb.Name() << std::endl;
  out << "contexts:" << std::endl;
  for (const auto& context : pb.contexts_) {
    out << "  name=" << context->name_ << ", gpu="
        << ((context->gpu_device_ == BaseBackend::Context::NO_GPU_DEVICE)
                ? "<none>"
                : std::to_string(context->gpu_device_))
        << ", max_batch_size="
        << ((context->max_batch_size_ == BaseBackend::Context::NO_BATCHING)
                ? "<none>"
                : std::to_string(context->max_batch_size_))
        << std::endl;
  }

  return out;
}

}}  // namespace nvidia::inferenceserver
