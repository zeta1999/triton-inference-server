// Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
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

#include "src/backends/onnx/onnx_backend.h"

#include <stdint.h>
#include <mutex>
#include "src/backends/onnx/loader.h"
#include "src/backends/onnx/onnx_utils.h"
#include "src/core/constants.h"
#include "src/core/cuda_utils.h"
#include "src/core/logging.h"
#include "src/core/model_config_cuda.h"
#include "src/core/model_config_utils.h"
#include "src/core/server_status.h"

#ifdef TRTIS_ENABLE_GPU
#include <cuda_provider_factory.h>
#include <cuda_runtime_api.h>
#endif  // TRTIS_ENABLE_GPU

#ifdef TRTIS_ENABLE_ONNXRUNTIME_TENSORRT
#include <tensorrt_provider_factory.h>
#endif  // TRTIS_ENABLE_ONNXRUNTIME_TENSORRT

#ifdef TRTIS_ENABLE_ONNXRUNTIME_OPENVINO
#include <openvino_provider_factory.h>
#endif  // TRTIS_ENABLE_ONNXRUNTIME_OPENVINO

namespace nvidia { namespace inferenceserver {

OnnxBackend::Context::Context(
    const std::string& name, const int gpu_device, const int max_batch_size,
    const bool enable_pinned_input, const bool enable_pinned_output)
    : BackendContext(
          name, gpu_device, max_batch_size, enable_pinned_input,
          enable_pinned_output),
      session_(nullptr), allocator_(nullptr)
{
}

OnnxBackend::Context::~Context()
{
  LOG_VERBOSE(1) << "~OnnxBackend::Context ";

  ReleaseOrtRunResources();
  if (session_ != nullptr) {
    OnnxLoader::UnloadSession(session_);
  }
  // 'allocator_' is default allocator which is managed by ONNX Runtime
}

Status
OnnxBackend::CreateExecutionContexts(
    const std::unordered_map<std::string, std::pair<bool, std::string>>& models)
{
  // [TODO] configurable like optimization policy in Tensorflow models
  // Create a "prototype" session option, which will be cloned and set
  // context-specific option on context creation.
  OrtSessionOptions* session_options;
  RETURN_IF_ORT_ERROR(ort_api->CreateSessionOptions(&session_options));

  OrtResourceWrapper<OrtSessionOptions*> options_wrapper(
      session_options, ort_api->ReleaseSessionOptions);
  RETURN_IF_ORT_ERROR(ort_api->SetIntraOpNumThreads(session_options, 1));

  // set graph optimization level
  GraphOptimizationLevel optimization_level =
      GraphOptimizationLevel::ORT_ENABLE_ALL;
  if (Config().optimization().has_graph()) {
    int graph_level = Config().optimization().graph().level();
    if (graph_level == -1) {
      optimization_level = GraphOptimizationLevel::ORT_ENABLE_BASIC;
    } else if (graph_level == 1) {
      optimization_level = GraphOptimizationLevel::ORT_ENABLE_EXTENDED;
    }
  }
  RETURN_IF_ORT_ERROR(ort_api->SetSessionGraphOptimizationLevel(
      session_options, optimization_level));

  RETURN_IF_ERROR(CreateExecutionContextsHelper(session_options, models));

  LOG_VERBOSE(1) << "onnx backend for " << Name() << std::endl << *this;

  return Status::Success;
}

Status
OnnxBackend::CreateExecutionContextsHelper(
    OrtSessionOptions* session_options,
    const std::unordered_map<std::string, std::pair<bool, std::string>>& models)
{
  uint32_t total_context_cnt = 0;

  // Create a session for each instance.
  for (const auto& group : Config().instance_group()) {
    for (int c = 0; c < group.count(); c++) {
      if (group.kind() == ModelInstanceGroup::KIND_CPU) {
        const std::string instance_name =
            group.name() + "_" + std::to_string(c) + "_cpu";
        RETURN_IF_ERROR(CreateExecutionContext(
            instance_name, Context::NO_GPU_DEVICE, session_options, models));
        total_context_cnt++;
      } else {
        for (int gpu_device : group.gpus()) {
          const std::string instance_name = group.name() + "_" +
                                            std::to_string(c) + "_gpu" +
                                            std::to_string(gpu_device);
          RETURN_IF_ERROR(CreateExecutionContext(
              instance_name, gpu_device, session_options, models));
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

  return Status::Success;
}

Status
OnnxBackend::CreateExecutionContext(
    const std::string& instance_name, const int gpu_device,
    OrtSessionOptions* base_session_options,
    const std::unordered_map<std::string, std::pair<bool, std::string>>& models)
{
  // For a GPU context, determine the model file to use for device
  // compute capability. CPU always uses the default model file.
  std::string cc;
  std::string cc_model_filename;
  if (gpu_device == Context::NO_GPU_DEVICE) {
    cc_model_filename = Config().default_model_filename();
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

    cc = std::to_string(cuprops.major) + "." + std::to_string(cuprops.minor);
    const auto& cc_itr = Config().cc_model_filenames().find(cc);
    cc_model_filename = (cc_itr == Config().cc_model_filenames().end())
                            ? Config().default_model_filename()
                            : cc_itr->second;
#else
    return Status(Status::Code::INTERNAL, "GPU instances not supported");
#endif  // TRTIS_ENABLE_GPU
  }

  const auto& op_itr = models.find(cc_model_filename);
  if (op_itr == models.end()) {
    return Status(
        Status::Code::INTERNAL,
        "unable to find model '" + cc_model_filename + "' for " + Name());
  }

  if (gpu_device == Context::NO_GPU_DEVICE) {
    LOG_INFO << "Creating instance " << instance_name << " on CPU using "
             << cc_model_filename;
  } else {
    LOG_INFO << "Creating instance " << instance_name << " on GPU "
             << gpu_device << " (" << cc << ") using " << cc_model_filename;
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

  // Set Onnx session option with proper device
  OrtSessionOptions* session_options;
  RETURN_IF_ORT_ERROR(
      ort_api->CloneSessionOptions(base_session_options, &session_options));

  OrtResourceWrapper<OrtSessionOptions*> options_wrapper(
      session_options, ort_api->ReleaseSessionOptions);

  // Set execution execution_accelerators (execution providers in ONNX Runtime)
  if (gpu_device != Context::NO_GPU_DEVICE) {
#ifdef TRTIS_ENABLE_GPU
    if (Config().optimization().has_execution_accelerators()) {
      // Don't need to ensure uniqueness of the providers,
      // ONNX Runtime will check it.
      for (const auto& execution_accelerator :
           Config()
               .optimization()
               .execution_accelerators()
               .gpu_execution_accelerator()) {
#ifdef TRTIS_ENABLE_ONNXRUNTIME_TENSORRT
        if (execution_accelerator.name() == kTensorRTExecutionAccelerator) {
          RETURN_IF_ORT_ERROR(OrtSessionOptionsAppendExecutionProvider_Tensorrt(
              session_options, gpu_device));
          LOG_VERBOSE(1) << "TensorRT Execution Accelerator is set for "
                         << instance_name << " on device " << gpu_device;
        } else
#endif  // TRTIS_ENABLE_ONNXRUNTIME_TENSORRT
        {
          return Status(
              Status::Code::INVALID_ARG, "unknown Execution Accelerator '" +
                                             execution_accelerator.name() +
                                             "' is requested");
        }
      }
    }
    RETURN_IF_ORT_ERROR(OrtSessionOptionsAppendExecutionProvider_CUDA(
        session_options, gpu_device));
    LOG_VERBOSE(1) << "CUDA Execution Accelerator is set for " << instance_name
                   << " on device " << gpu_device;
#else
    return Status(Status::Code::INTERNAL, "GPU instances not supported");
#endif  // TRTIS_ENABLE_GPU
  }

  bool need_lock = false;
  if (Config().optimization().has_execution_accelerators()) {
    for (const auto& execution_accelerator : Config()
                                                 .optimization()
                                                 .execution_accelerators()
                                                 .cpu_execution_accelerator()) {
      if (execution_accelerator.name() == kOpenVINOExecutionAccelerator) {
#ifdef TRTIS_ENABLE_ONNXRUNTIME_OPENVINO
        need_lock = true;
        RETURN_IF_ORT_ERROR(OrtSessionOptionsAppendExecutionProvider_OpenVINO(
            session_options, "CPU"));
        LOG_VERBOSE(1) << "OpenVINO Execution Accelerator is set for "
                       << instance_name << " on device CPU";
#else
        return Status(
            Status::Code::INVALID_ARG,
            "OpenVINO Execution Accelerator is not enabled");
#endif  // TRTIS_ENABLE_ONNXRUNTIME_OPENVINO
      } else {
        return Status(
            Status::Code::INVALID_ARG, "unknown Execution Accelerator '" +
                                           execution_accelerator.name() +
                                           "' is requested");
      }
    }
  }

  // ONNX session creation with OpenVINO is not thread-safe,
  // so multiple creations are serialized with a global lock.
  static std::mutex global_context_mu;
  std::unique_lock<std::mutex> glock(global_context_mu, std::defer_lock);
  if (need_lock) {
    glock.lock();
  }

  RETURN_IF_ERROR(OnnxLoader::LoadSession(
      op_itr->second, session_options, &context->session_));
  RETURN_IF_ORT_ERROR(
      ort_api->GetAllocatorWithDefaultOptions(&context->allocator_));

  size_t expected_input_cnt = (size_t)Config().input().size();

  // If this is a sequence model then make sure that the required
  // inputs are present in the model and have the correct shape and
  // datatype.
  if (Config().has_sequence_batching()) {
    bool have_start, have_end, have_ready, have_corrid;
    RETURN_IF_ERROR(context->ValidateBooleanSequenceControl(
        Config().name(), Config().sequence_batching(),
        ModelSequenceBatching::Control::CONTROL_SEQUENCE_START,
        false /* required */, &have_start));
    RETURN_IF_ERROR(context->ValidateBooleanSequenceControl(
        Config().name(), Config().sequence_batching(),
        ModelSequenceBatching::Control::CONTROL_SEQUENCE_END,
        false /* required */, &have_end));
    RETURN_IF_ERROR(context->ValidateBooleanSequenceControl(
        Config().name(), Config().sequence_batching(),
        ModelSequenceBatching::Control::CONTROL_SEQUENCE_READY,
        false /* required */, &have_ready));
    RETURN_IF_ERROR(context->ValidateTypedSequenceControl(
        Config().name(), Config().sequence_batching(),
        ModelSequenceBatching::Control::CONTROL_SEQUENCE_CORRID,
        false /* required */, &have_corrid));
    if (have_start) {
      expected_input_cnt += 1;
    }
    if (have_end) {
      expected_input_cnt += 1;
    }
    if (have_ready) {
      expected_input_cnt += 1;
    }
    if (have_corrid) {
      expected_input_cnt += 1;
    }
  }

  RETURN_IF_ERROR(context->ValidateInputs(
      Config().name(), Config().input(), expected_input_cnt));
  RETURN_IF_ERROR(context->ValidateOutputs(Config().name(), Config().output()));

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateBooleanSequenceControl(
    const std::string& model_name, const ModelSequenceBatching& batcher,
    const ModelSequenceBatching::Control::Kind control_kind, bool required,
    bool* have_control)
{
  std::string tensor_name;
  DataType tensor_datatype;
  RETURN_IF_ERROR(GetBooleanSequenceControlProperties(
      batcher, model_name, control_kind, required, &tensor_name,
      &tensor_datatype, nullptr, nullptr, nullptr, nullptr));
  *have_control = !tensor_name.empty();
  if (*have_control) {
    OnnxTensorInfoMap input_tensor_infos;
    RETURN_IF_ERROR(InputInfos(session_, allocator_, input_tensor_infos));
    const auto& iit = input_tensor_infos.find(tensor_name);
    if (iit == input_tensor_infos.end()) {
      return Status(
          Status::Code::INTERNAL,
          "configuration specified sequence control '" + tensor_name +
              "', but model does not provide that input");
    }

    // Control tensors must have shape [1].
    const int nonbatch_start_idx = (max_batch_size_ > 0) ? 1 : 0;
    std::vector<int64_t> debatched_dims;
    for (size_t i = nonbatch_start_idx; i < iit->second.dims_.size(); i++) {
      debatched_dims.push_back(iit->second.dims_[i]);
    }

    if ((debatched_dims.size() != 1) || (debatched_dims[0] != 1)) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "' in model has dims " +
              DimsListToString(debatched_dims) + " but dims [1] is expected");
    }

    if (ConvertToOnnxDataType(tensor_datatype) != iit->second.type_) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "', the model expects data-type " +
              OnnxDataTypeName(iit->second.type_) +
              " but the model configuration specifies data-type " +
              DataType_Name(tensor_datatype));
    }
  }

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateTypedSequenceControl(
    const std::string& model_name, const ModelSequenceBatching& batcher,
    const ModelSequenceBatching::Control::Kind control_kind, bool required,
    bool* have_control)
{
  std::string tensor_name;
  DataType tensor_datatype;
  RETURN_IF_ERROR(GetTypedSequenceControlProperties(
      batcher, model_name, control_kind, required, &tensor_name,
      &tensor_datatype));
  *have_control = !tensor_name.empty();
  if (*have_control) {
    OnnxTensorInfoMap input_tensor_infos;
    RETURN_IF_ERROR(InputInfos(session_, allocator_, input_tensor_infos));
    const auto& iit = input_tensor_infos.find(tensor_name);
    if (iit == input_tensor_infos.end()) {
      return Status(
          Status::Code::INTERNAL,
          "configuration specified sequence control '" + tensor_name +
              "', but model does not provide that input");
    }

    // Control tensors must have shape [1].
    const int nonbatch_start_idx = (max_batch_size_ > 0) ? 1 : 0;
    std::vector<int64_t> debatched_dims;
    for (size_t i = nonbatch_start_idx; i < iit->second.dims_.size(); i++) {
      debatched_dims.push_back(iit->second.dims_[i]);
    }

    if ((debatched_dims.size() != 1) || (debatched_dims[0] != 1)) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "' in model has dims " +
              DimsListToString(debatched_dims) + " but dims [1] is expected");
    }

    if (ConvertToOnnxDataType(tensor_datatype) != iit->second.type_) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + "', sequence control '" +
              tensor_name + "', the model expects data-type " +
              OnnxDataTypeName(iit->second.type_) +
              " but the model configuration specifies data-type " +
              DataType_Name(tensor_datatype));
    }
  }

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateInputs(
    const std::string& model_name,
    const ::google::protobuf::RepeatedPtrField<ModelInput>& ios,
    const size_t expected_input_cnt)
{
  std::set<std::string> input_tensor_names;
  RETURN_IF_ERROR(InputNames(session_, input_tensor_names));

  OnnxTensorInfoMap input_tensor_infos;
  RETURN_IF_ERROR(InputInfos(session_, allocator_, input_tensor_infos));

  if (input_tensor_infos.size() != expected_input_cnt) {
    return Status(
        Status::Code::INVALID_ARG,
        "unable to load model '" + model_name + "', configuration expects " +
            std::to_string(expected_input_cnt) + " inputs, model provides " +
            std::to_string(input_tensor_infos.size()));
  }

  for (const auto& io : ios) {
    auto iit = input_tensor_infos.find(io.name());
    if (iit == input_tensor_infos.end()) {
      RETURN_IF_ERROR(CheckAllowedModelInput(io, input_tensor_names));
    }

    auto onnx_data_type = ConvertToOnnxDataType(io.data_type());
    if (onnx_data_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
      return Status(
          Status::Code::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for input '" + io.name() + "' for model '" + model_name + "'");
    } else if (onnx_data_type != iit->second.type_) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + ", unexpected datatype " +
              DataType_Name(ConvertFromOnnxDataType(iit->second.type_)) +
              " for input '" + io.name() + "', expecting " +
              DataType_Name(io.data_type()));
    }

    // If a reshape is provided for the input then use that when
    // validating that the model matches what is expected.
    const DimsList& dims =
        (io.has_reshape()) ? io.reshape().shape() : io.dims();
    RETURN_IF_ERROR(CompareDimsSupported(
        model_name, io.name(), iit->second.dims_, dims, max_batch_size_,
        false /* compare_exact */));
  }

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateOutputs(
    const std::string& model_name,
    const ::google::protobuf::RepeatedPtrField<ModelOutput>& ios)
{
  std::set<std::string> output_tensor_names;
  RETURN_IF_ERROR(OutputNames(session_, output_tensor_names));

  OnnxTensorInfoMap output_tensor_infos;
  RETURN_IF_ERROR(OutputInfos(session_, allocator_, output_tensor_infos));

  for (const auto& io : ios) {
    auto iit = output_tensor_infos.find(io.name());
    if (iit == output_tensor_infos.end()) {
      RETURN_IF_ERROR(CheckAllowedModelOutput(io, output_tensor_names));
    }

    auto onnx_data_type = ConvertToOnnxDataType(io.data_type());
    if (onnx_data_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
      return Status(
          Status::Code::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for output '" + io.name() + "' for model '" + model_name + "'");
    } else if (onnx_data_type != iit->second.type_) {
      return Status(
          Status::Code::INVALID_ARG,
          "unable to load model '" + model_name + ", unexpected datatype " +
              DataType_Name(ConvertFromOnnxDataType(iit->second.type_)) +
              " for output '" + io.name() + "', expecting " +
              DataType_Name(io.data_type()));
    }

    // If a reshape is provided for the input then use that when
    // validating that the model matches what is expected.
    const DimsList& dims =
        (io.has_reshape()) ? io.reshape().shape() : io.dims();
    RETURN_IF_ERROR(CompareDimsSupported(
        model_name, io.name(), iit->second.dims_, dims, max_batch_size_,
        true /* compare_exact */));
  }

  return Status::Success;
}

Status
OnnxBackend::Context::Run(
    const InferenceBackend* base, std::vector<Scheduler::Payload>* payloads)
{
  LOG_VERBOSE(1) << "Running " << name_ << " with " << payloads->size()
                 << " request payloads";

  const InferenceRequest* repr_input_request = nullptr;

  // For each request in 'payloads' collect the total batch size for
  // this inference execution. The batch-size, number of inputs, and
  // size of each input has already been checked by each payloads
  // request provider so don't need to do that here.
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

  // use Scoped wrapper to clean up Ort tensors when Run() returns
  static auto io_tensor_deleter = [](Context* ctx) {
    if (ctx != nullptr) {
      ctx->ReleaseOrtRunResources();
    }
  };
  OrtResourceWrapper<Context*> io_tensor_wrapper(this, io_tensor_deleter);

  // Hold reference to each buffer of input data so that it stays
  // until the inference has completed.
  std::vector<std::unique_ptr<AllocatedMemory>> input_buffers;
  std::vector<InputInfo> inputs;
  std::vector<const char*> input_names;
  bool cuda_copy = false;

  for (const auto& pr : repr_input_request->ImmutableInputs()) {
    const InferenceRequest::Input* input = pr.second;
    const std::string& name = input->Name();

    // Create a tensor for each input sized correctly for the total
    // payload batch size. Concatenate input values from each payload
    // into the corresponding tensor.
    RETURN_IF_ERROR(SetInputTensor(
        name, input->DType(), input->Shape(), total_batch_size, payloads,
        &input_buffers, &inputs, &input_names, &cuda_copy));
  }

  // Request to retrieve all output specified in model config
  // and reserve placeholder for output tensors
  std::vector<const char*> output_names;
  for (const auto& output : base->Config().output()) {
    output_names.emplace_back(output.name().c_str());
    output_tensors_.emplace_back(nullptr);
  }

#ifdef TRTIS_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
  cuda_copy = false;
  for (auto& input : inputs) {
    for (auto& indirect_buffer : input.indirect_buffers_) {
      bool cuda_used;
      TRTSERVER_Memory_Type buffer_memory_type;
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
        cuda_copy |= cuda_copy;
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

  // Run...
  RETURN_IF_ORT_ERROR(ort_api->Run(
      session_, NULL /* run options */, input_names.data(),
      (const OrtValue* const*)input_tensors_.data(), input_tensors_.size(),
      output_names.data(), output_names.size(), output_tensors_.data()));

#ifdef TRTIS_ENABLE_STATS
  for (auto& payload : *payloads) {
    if (payload.stats_ != nullptr) {
      payload.stats_->CaptureTimestamp(
          ModelInferStats::TimestampKind::kComputeOutputStart);
    }
  }
#endif  // TRTIS_ENABLE_STATS

  // Make sure each output is of the expected size and copy it into
  // the payload responses.
  return ReadOutputTensors(base, total_batch_size, output_names, payloads);
}

Status
OnnxBackend::Context::SetInputTensor(
    const std::string& name, const DataType data_type,
    const std::vector<int64_t>& dims, size_t total_batch_size,
    std::vector<Scheduler::Payload>* payloads,
    std::vector<std::unique_ptr<AllocatedMemory>>* input_buffers,
    std::vector<InputInfo>* inputs, std::vector<const char*>* input_names,
    bool* cuda_used)
{
  input_names->emplace_back(name.c_str());
  input_tensors_.emplace_back(nullptr);

  size_t batch1_element_cnt = 1;
  std::vector<int64_t> input_dims;

  // Only add batch dimension if the model support batching
  if (max_batch_size_ != NO_BATCHING) {
    input_dims.push_back(total_batch_size);
  }
  for (const auto dim : dims) {
    input_dims.push_back(dim);
    batch1_element_cnt *= dim;
  }

  size_t total_byte_size = 0;
  std::vector<size_t> expected_byte_sizes;
  std::vector<size_t> expected_element_cnts;
  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;

    expected_element_cnts.push_back(irequest->BatchSize() * batch1_element_cnt);

    if (data_type == TYPE_STRING) {
      // For String data byte, obtain expected byte size from
      // 'batch_byte_size' The request normalizer has already checked
      // that batch_byte_size is set
      const InferenceRequest::Input* in;
      RETURN_IF_ERROR(irequest->ImmutableInput(name, &in));
      expected_byte_sizes.push_back(in->BatchByteSize());
    } else {
      // Otherwise calculate expected byte size from 'expected_element_cnts',
      // so that the byte size for override input (not provided in request
      // header's input field) can also be set correctly.
      expected_byte_sizes.push_back(
          expected_element_cnts.back() * GetDataTypeByteSize(data_type));
    }
    total_byte_size += expected_byte_sizes.back();
  }

  // Reserve one more byte at the end of input_buffer to ensure last element
  // of String data can become valid C string.
  const size_t buffer_size =
      total_byte_size + ((data_type != TYPE_STRING) ? 0 : 1);
  input_buffers->emplace_back(
      new AllocatedMemory(buffer_size, TRTSERVER_MEMORY_CPU_PINNED, 0));
  inputs->emplace_back();
  auto& input = inputs->back();
  input.input_buffer_ = input_buffers->back()->MutableBuffer(
      &input.memory_type_, &input.memory_type_id_);

  // Note that 'cuda_used' will be updated only
  // for non-string data type. For string, the data must be ready to proceed.
  auto tmp_cuda_used =
      SetInputBuffer(name, expected_byte_sizes, payloads, &input);

  if (data_type != TYPE_STRING) {
    const OrtMemoryInfo* allocator_info;
    RETURN_IF_ORT_ERROR(ort_api->AllocatorGetInfo(allocator_, &allocator_info));
    RETURN_IF_ORT_ERROR(ort_api->CreateTensorWithDataAsOrtValue(
        allocator_info, (void*)input.input_buffer_, total_byte_size,
        input_dims.data(), input_dims.size(), ConvertToOnnxDataType(data_type),
        &input_tensors_.back()));
    *cuda_used |= tmp_cuda_used;
  } else {
#ifdef TRTIS_ENABLE_GPU
    if (tmp_cuda_used) {
      cudaStreamSynchronize(stream_);
    }
#endif  // TRTIS_ENABLE_GPU

    std::vector<const char*> string_data;
    // Onnx String tensor is created by passing array of C strings,
    // set such array and modify data in input buffer to be C strings
    SetStringInputBuffer(
        name, expected_byte_sizes, expected_element_cnts, payloads,
        input.input_buffer_, &string_data);
    // Make sure to make the last string data valid C string
    input.input_buffer_[total_byte_size] = 0;

    RETURN_IF_ORT_ERROR(ort_api->CreateTensorAsOrtValue(
        allocator_, input_dims.data(), input_dims.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING, &input_tensors_.back()));
    RETURN_IF_ORT_ERROR(ort_api->FillStringTensor(
        input_tensors_.back(), string_data.data(), string_data.size()));
  }

  return Status::Success;
}

void
OnnxBackend::Context::SetStringInputBuffer(
    const std::string& name, const std::vector<size_t>& expected_byte_sizes,
    const std::vector<size_t>& expected_element_cnts,
    std::vector<Scheduler::Payload>* payloads, char* input_buffer,
    std::vector<const char*>* string_data)
{
  // offset for each payload
  size_t buffer_copy_offset = 0;
  for (size_t idx = 0; idx < expected_byte_sizes.size(); idx++) {
    auto& payload = (*payloads)[idx];
    const size_t expected_byte_size = expected_byte_sizes[idx];
    const size_t expected_element_cnt = expected_element_cnts[idx];

    size_t element_cnt = 0;
    if (payload.status_.IsOk()) {
      size_t remaining_bytes = expected_byte_size;
      char* data_content = input_buffer + buffer_copy_offset;
      // Continue if the remaining bytes may still contain size info
      while (remaining_bytes >= sizeof(uint32_t)) {
        if (element_cnt >= expected_element_cnt) {
          payload.status_ = Status(
              Status::Code::INVALID_ARG,
              "unexpected number of string elements " +
                  std::to_string(element_cnt + 1) + " for inference input '" +
                  name + "', expecting " +
                  std::to_string(expected_element_cnt));
          break;
        }

        const uint32_t len = *(reinterpret_cast<const uint32_t*>(data_content));
        remaining_bytes -= sizeof(uint32_t);
        // Make first byte of size info 0, so that if there is string data
        // in front of it, the data becomes valid C string.
        *data_content = 0;
        data_content = data_content + sizeof(uint32_t);
        if (len > remaining_bytes) {
          payload.status_ = Status(
              Status::Code::INVALID_ARG,
              "incomplete string data for inference input '" + name +
                  "', expecting string of length " + std::to_string(len) +
                  " but only " + std::to_string(remaining_bytes) +
                  " bytes available");
          break;
        } else {
          string_data->push_back(data_content);
          element_cnt++;
          data_content = data_content + len;
          remaining_bytes -= len;
        }
      }
    }

    FillStringData(string_data, expected_element_cnt - element_cnt);

    buffer_copy_offset += expected_byte_size;
  }
}

void
OnnxBackend::Context::FillStringData(
    std::vector<const char*>* string_data, size_t cnt)
{
  static const char* empty = "";
  for (size_t c = 0; c < cnt; c++) {
    string_data->push_back(empty);
  }
}

Status
OnnxBackend::Context::ReadOutputTensors(
    const InferenceBackend* base, size_t total_batch_size,
    const std::vector<const char*>& output_names,
    std::vector<Scheduler::Payload>* payloads)
{
  bool cuda_copy = false;
  std::vector<OutputInfo> outputs;
  std::vector<std::vector<char>> string_buffers;
  for (size_t idx = 0; idx < output_names.size(); idx++) {
    outputs.emplace_back();
    auto& output = outputs.back();
    std::string name = std::string(output_names[idx]);

    const ModelOutput* output_config;
    RETURN_IF_ERROR(base->GetOutput(name, &output_config));

    OrtValue* output_tensor = output_tensors_[idx];
    if (output_tensor == nullptr) {
      return Status(
          Status::Code::INTERNAL,
          "output tensor '" + name + "' does not found");
    }

    // Get output type and shape
    OrtTypeInfo* typeinfo;
    RETURN_IF_ORT_ERROR(ort_api->GetTypeInfo(output_tensor, &typeinfo));
    OrtResourceWrapper<OrtTypeInfo*> typeinfo_wrapper(
        typeinfo, ort_api->ReleaseTypeInfo);

    const OrtTensorTypeAndShapeInfo* type_and_shape;
    RETURN_IF_ORT_ERROR(
        ort_api->CastTypeInfoToTensorInfo(typeinfo, &type_and_shape));


    size_t num_dims;
    RETURN_IF_ORT_ERROR(ort_api->GetDimensionsCount(type_and_shape, &num_dims));

    output.output_shape_.resize(num_dims);
    RETURN_IF_ORT_ERROR(ort_api->GetDimensions(
        type_and_shape, output.output_shape_.data(),
        output.output_shape_.size()));
    const size_t element_count = GetElementCount(output.output_shape_);

    ONNXTensorElementDataType type;
    RETURN_IF_ORT_ERROR(ort_api->GetTensorElementType(type_and_shape, &type));

    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING) {
      const size_t batch1_element_cnt = element_count / total_batch_size;
      size_t total_length = 0;
      RETURN_IF_ORT_ERROR(
          ort_api->GetStringTensorDataLength(output_tensor, &total_length));

      string_buffers.emplace_back(std::vector<char>(total_length));
      auto content = string_buffers.back().data();
      size_t offsets[element_count + 1];
      RETURN_IF_ORT_ERROR(ort_api->GetStringTensorContent(
          output_tensor, content, total_length, offsets, element_count));
      // Mark "passed end byte offset"
      offsets[element_count] = total_length;

      cuda_copy |= SetStringOutputBuffer(
          name, batch1_element_cnt, content, output.output_shape_, offsets,
          payloads);
    } else {
      // Fixed size data type...
      const size_t actual_byte_size =
          element_count * GetDataTypeByteSize(ConvertFromOnnxDataType(type));
      const size_t expected_byte_size =
          element_count * GetDataTypeByteSize(output_config->data_type());
      const size_t batch1_byte_size = expected_byte_size / total_batch_size;
      if (actual_byte_size != expected_byte_size) {
        return Status(
            Status::Code::INTERNAL,
            "unexpected size for output '" + name + "', byte-size " +
                std::to_string(actual_byte_size) + " does not equal " +
                std::to_string(total_batch_size) + " * " +
                std::to_string(batch1_byte_size));
      }

      RETURN_IF_ORT_ERROR(ort_api->GetTensorMutableData(
          output_tensor, (void**)&output.output_buffer_));

      // [TODO] currently ONNX output data are always on CPU
      // https://github.com/microsoft/onnxruntime/issues/1621
      output.memory_type_ = TRTSERVER_MEMORY_CPU;
      output.memory_type_id_ = 0;
      cuda_copy |=
          SetFixedSizeOutputBuffer(name, batch1_byte_size, &output, payloads);
    }
  }

#ifdef TRTIS_ENABLE_GPU
  if (cuda_copy) {
    cudaStreamSynchronize(stream_);
  }
  cuda_copy = false;
  for (auto& output : outputs) {
    for (auto& indirect_buffer : output.indirect_buffers_) {
      bool cuda_used;
      TRTSERVER_Memory_Type src_memory_type;
      int64_t src_memory_type_id;
      // placeholder, copy byte size is determined by dst_byte_size
      size_t src_byte_size;
      auto src = indirect_buffer.first->BufferAt(
          0, &src_byte_size, &src_memory_type, &src_memory_type_id);
      TRTSERVER_Memory_Type dst_memory_type;
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

bool
OnnxBackend::Context::SetStringOutputBuffer(
    const std::string& name, const size_t batch1_element_cnt,
    const char* content, const std::vector<int64_t>& content_shape,
    const size_t* offsets, std::vector<Scheduler::Payload>* payloads)
{
  size_t element_idx = 0;
  bool cuda_copy = false;
  for (auto& payload : *payloads) {
    const auto& irequest = payload.request_;
    const size_t expected_element_cnt =
        irequest->BatchSize() * batch1_element_cnt;

    // If 'payload' requested this output then copy it from
    // 'content'. If it did not request this output then just
    // skip it in the 'content'.
    if ((payload.response_provider_ != nullptr) &&
        payload.response_provider_->RequiresOutput(name)) {
      // Calculate expected byte size in advance using string offsets
      const size_t data_byte_size =
          offsets[element_idx + expected_element_cnt] - offsets[element_idx];
      const size_t expected_byte_size =
          data_byte_size + sizeof(uint32_t) * expected_element_cnt;

      void* buffer;
      TRTSERVER_Memory_Type preferred_memory_type = TRTSERVER_MEMORY_CPU_PINNED;
      TRTSERVER_Memory_Type actual_memory_type;
      int64_t actual_memory_type_id;
      Status status = payload.response_provider_->AllocateOutputBuffer(
          name, &buffer, expected_byte_size, content_shape,
          preferred_memory_type, 0 /* preferred_memory_type_id */,
          &actual_memory_type, &actual_memory_type_id);
      if (status.IsOk()) {
        bool cuda_used = false;
        size_t copied_byte_size = 0;
        for (size_t e = 0; e < expected_element_cnt; ++e) {
          const uint32_t len =
              offsets[element_idx + e + 1] - offsets[element_idx + e];
          // Prepend size of the string
          payload.status_ = CopyBuffer(
              name, TRTSERVER_MEMORY_CPU /* src_memory_type */,
              0 /* src_memory_type_id */, actual_memory_type,
              actual_memory_type_id, sizeof(uint32_t),
              static_cast<const void*>(&len),
              static_cast<char*>(buffer) + copied_byte_size, stream_,
              &cuda_used);

          cuda_copy |= cuda_used;
          copied_byte_size += sizeof(uint32_t);

          // Copy raw string content
          payload.status_ = CopyBuffer(
              name, TRTSERVER_MEMORY_CPU /* src_memory_type */,
              0 /* src_memory_type_id */, actual_memory_type,
              actual_memory_type_id, len, content + offsets[element_idx + e],
              static_cast<char*>(buffer) + copied_byte_size, stream_,
              &cuda_used);

          cuda_copy |= cuda_used;
          copied_byte_size += len;
        }
      } else {
        payload.status_ = status;
      }
    }

    element_idx += expected_element_cnt;
  }

  return cuda_copy;
}

void
OnnxBackend::Context::ReleaseOrtRunResources()
{
  // Release input tensor if set
  for (auto& tensor : input_tensors_) {
    if (tensor != nullptr) {
      ort_api->ReleaseValue(tensor);
    }
  }
  input_tensors_.clear();

  // Release output tensor if set
  for (auto& tensor : output_tensors_) {
    if (tensor != nullptr) {
      ort_api->ReleaseValue(tensor);
    }
  }
  output_tensors_.clear();
}

std::ostream&
operator<<(std::ostream& out, const OnnxBackend& pb)
{
  out << "name=" << pb.Name() << std::endl;
  out << "contexts:" << std::endl;
  for (const auto& context : pb.contexts_) {
    out << "  name=" << context->name_ << ", gpu="
        << ((context->gpu_device_ == OnnxBackend::Context::NO_GPU_DEVICE)
                ? "<none>"
                : std::to_string(context->gpu_device_))
        << ", max_batch_size="
        << ((context->max_batch_size_ == OnnxBackend::Context::NO_BATCHING)
                ? "<none>"
                : std::to_string(context->max_batch_size_))
        << std::endl;
  }

  return out;
}

}}  // namespace nvidia::inferenceserver
