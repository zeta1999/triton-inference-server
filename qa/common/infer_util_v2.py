# Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import sys
import os
import numpy as np
import tritongrpcclient.core as grpcclient
import tritongrpcclient.model_config_pb2 as mc
import tritonhttpclient.core as httpclient
from tritonhttpclient.utils import triton_to_np_dtype
from tritonhttpclient.utils import InferenceServerException
import tritonsharedmemoryutils.shared_memory as shm
import tritonsharedmemoryutils.cuda_shared_memory as cudashm
import test_util as tu
import shm_util as su

# unicode() doesn't exist on python3, for how we use it the
# corresponding function is bytes()
if sys.version_info.major == 3:
    unicode = bytes

_seen_request_ids = set()

def _range_repr_dtype(dtype):
    if dtype == np.float64:
        return np.int32
    elif dtype == np.float32:
        return np.int16
    elif dtype == np.float16:
        return np.int8
    elif dtype == np.object:  # TYPE_STRING
        return np.int32
    return dtype

def _prepend_string_size(input_values):
    input_list = []
    for input_value in input_values:
        input_list.append(serialize_string_tensor(input_value))
    return input_list

# Perform inference using an "addsum" type verification backend.
def infer_exact(tester, pf, tensor_shape, batch_size,
                input_dtype, output0_dtype, output1_dtype,
                output0_raw=True, output1_raw=True,
                model_version=None, swap=False,
                outputs=("OUTPUT0", "OUTPUT1"), use_http=True, use_grpc=True,
                use_json=True, skip_request_id_check=False, use_streaming=True,
                correlation_id=0, shm_region_names=None, precreated_shm_regions=None,
                use_system_shared_memory=False, use_cuda_shared_memory=False,
                priority=0, timeout_us=0):
    tester.assertTrue(use_http or use_grpc or use_streaming)
    configs = []
    if use_http:
        configs.append(("localhost:8000", ProtocolType.HTTP, False, True))
    if use_http:
        configs.append(("localhost:8000", ProtocolType.HTTP, False, False))
    if use_grpc:
        configs.append(("localhost:8001", ProtocolType.GRPC, False, False))
    if use_streaming:
        configs.append(("localhost:8001", ProtocolType.GRPC, True, False))

    # outputs are sum and difference of inputs so set max input
    # values so that they will not overflow the output. This
    # allows us to do an exact match. For float types use 8, 16,
    # 32 int range for fp 16, 32, 64 respectively. When getting
    # class outputs the result value/probability is returned as a
    # float so must use fp32 range in that case.
    rinput_dtype = _range_repr_dtype(input_dtype)
    routput0_dtype = _range_repr_dtype(output0_dtype if output0_raw else np.float32)
    routput1_dtype = _range_repr_dtype(output1_dtype if output1_raw else np.float32)
    val_min = max(np.iinfo(rinput_dtype).min,
                np.iinfo(routput0_dtype).min,
                np.iinfo(routput1_dtype).min) / 2
    val_max = min(np.iinfo(rinput_dtype).max,
                np.iinfo(routput0_dtype).max,
                np.iinfo(routput1_dtype).max) / 2

    num_classes = 3

    input0_list = list()
    input1_list = list()
    expected0_list = list()
    expected1_list = list()
    expected0_val_list = list()
    expected1_val_list = list()
    for b in range(batch_size):
        in0 = np.random.randint(low=val_min, high=val_max,
                                size=tensor_shape, dtype=rinput_dtype)
        in1 = np.random.randint(low=val_min, high=val_max,
                                size=tensor_shape, dtype=rinput_dtype)
        if input_dtype != np.object:
            in0 = in0.astype(input_dtype)
            in1 = in1.astype(input_dtype)

        if not swap:
            op0 = in0 + in1
            op1 = in0 - in1
        else:
            op0 = in0 - in1
            op1 = in0 + in1

        expected0_val_list.append(op0)
        expected1_val_list.append(op1)
        if output0_dtype == np.object:
            expected0_list.append(np.array([unicode(str(x), encoding='utf-8')
                                            for x in (op0.flatten())], dtype=object).reshape(op0.shape))
        else:
            expected0_list.append(op0.astype(output0_dtype))
        if output1_dtype == np.object:
            expected1_list.append(np.array([unicode(str(x), encoding='utf-8')
                                            for x in (op1.flatten())], dtype=object).reshape(op1.shape))
        else:
            expected1_list.append(op1.astype(output1_dtype))

        if input_dtype == np.object:
            in0n = np.array([str(x) for x in in0.reshape(in0.size)], dtype=object)
            in0 = in0n.reshape(in0.shape)
            in1n = np.array([str(x) for x in in1.reshape(in1.size)], dtype=object)
            in1 = in1n.reshape(in1.shape)

        input0_list.append(in0)
        input1_list.append(in1)

    # prepend size of string to input string data
    if input_dtype == np.object:
        input0_list_tmp = _prepend_string_size(input0_list)
        input1_list_tmp = _prepend_string_size(input1_list)
    else:
        input0_list_tmp = input0_list
        input1_list_tmp = input1_list

    if output0_dtype == np.object:
        expected0_list_tmp = _prepend_string_size(expected0_list)
    else:
        expected0_list_tmp = expected0_list

    if output1_dtype == np.object:
        expected1_list_tmp = _prepend_string_size(expected1_list)
    else:
        expected1_list_tmp = expected1_list

    # Create and register system/cuda shared memory regions if needed
    shm_handles = su.create_register_set_shm_regions(input0_list_tmp, input1_list_tmp, expected0_list_tmp,
                                    expected1_list_tmp, outputs, shm_region_names, precreated_shm_regions,
                                    use_system_shared_memory, use_cuda_shared_memory)

    # Run inference and check results for each config
    for config in configs:
        model_name = tu.get_model_name(pf, input_dtype, output0_dtype, output1_dtype)

        ctx = InferContext(config[0], config[1], model_name, model_version,
                       correlation_id=correlation_id, streaming=config[2],
                       verbose=True)

        expected0_sort_idx = [ np.flip(np.argsort(x.flatten()), 0) for x in expected0_val_list ]
        expected1_sort_idx = [ np.flip(np.argsort(x.flatten()), 0) for x in expected1_val_list ]

        output_req = {}
        OUTPUT0 = "OUTPUT0"
        OUTPUT1 = "OUTPUT1"
        INPUT0 = "INPUT0"
        INPUT1 = "INPUT1"
        if pf == "libtorch" or pf == "libtorch_nobatch":
            OUTPUT0 = "OUTPUT__0"
            OUTPUT1 = "OUTPUT__1"
            INPUT0 = "INPUT__0"
            INPUT1 = "INPUT__1"
        i=0
        if "OUTPUT0" in outputs:
            if len(shm_handles) != 0:
                output_req[OUTPUT0] = (InferContext.ResultFormat.RAW, shm_handles[2])
            else:
                if output0_raw:
                    output_req[OUTPUT0] = InferContext.ResultFormat.RAW
                else:
                    output_req[OUTPUT0] = (InferContext.ResultFormat.CLASS, num_classes)
            i+=1
        if "OUTPUT1" in outputs:
            if len(shm_handles) != 0:
                output_req[OUTPUT1] = (InferContext.ResultFormat.RAW, shm_handles[2+i])
            else:
                if output1_raw:
                    output_req[OUTPUT1] = InferContext.ResultFormat.RAW
                else:
                    output_req[OUTPUT1] = (InferContext.ResultFormat.CLASS, num_classes)

        if len(shm_handles) != 0:
            results = ctx.run(
                    { INPUT0 : (shm_handles[0], tensor_shape),
                    INPUT1 : (shm_handles[1], tensor_shape) },
                    output_req, batch_size,
                    priority=priority, timeout_us=timeout_us)
        else:
            results = ctx.run(
                    { INPUT0 : input0_list, INPUT1 : input1_list },
                    output_req, batch_size,
                    priority=priority, timeout_us=timeout_us)

        if not skip_request_id_check:
            global _seen_request_ids
            request_id = ctx.get_last_request_id()
            tester.assertFalse(request_id in _seen_request_ids,
                               "request_id: {}".format(request_id))
            _seen_request_ids.add(request_id)

        tester.assertEqual(ctx.get_last_request_model_name(), model_name)
        if model_version is not None:
            tester.assertEqual(ctx.get_last_request_model_version(), model_version)

        tester.assertEqual(len(results), len(outputs))
        for (result_name, result_val) in iteritems(results):
            for b in range(batch_size):
                if ((result_name == OUTPUT0 and output0_raw) or
                    (result_name == OUTPUT1 and output1_raw)):
                    if result_name == OUTPUT0:
                        tester.assertTrue(np.array_equal(result_val[b], expected0_list[b]),
                                        "{}, {} expected: {}, got {}".format(
                                            model_name, OUTPUT0, expected0_list[b], result_val[b]))
                    elif result_name == OUTPUT1:
                        tester.assertTrue(np.array_equal(result_val[b], expected1_list[b]),
                                        "{}, {} expected: {}, got {}".format(
                                            model_name, OUTPUT1, expected1_list[b], result_val[b]))
                    else:
                        tester.assertTrue(False, "unexpected raw result {}".format(result_name))
                else:
                    # num_classes values must be returned and must
                    # match expected top values
                    class_list = result_val[b]
                    tester.assertEqual(len(class_list), num_classes)

                    expected0_flatten = expected0_list[b].flatten()
                    expected1_flatten = expected1_list[b].flatten()

                    for idx, ctuple in enumerate(class_list):
                        if result_name == OUTPUT0:
                            # can't compare indices since could have
                            # different indices with the same
                            # value/prob, so compare that the value of
                            # each index equals the expected
                            # value. Can only compare labels when the
                            # indices are equal.
                            tester.assertEqual(ctuple[1], expected0_flatten[ctuple[0]])
                            tester.assertEqual(ctuple[1], expected0_flatten[expected0_sort_idx[b][idx]])
                            if ctuple[0] == expected0_sort_idx[b][idx]:
                                tester.assertEqual(ctuple[2], 'label{}'.format(expected0_sort_idx[b][idx]))
                        elif result_name == OUTPUT1:
                            tester.assertEqual(ctuple[1], expected1_flatten[ctuple[0]])
                            tester.assertEqual(ctuple[1], expected1_flatten[expected1_sort_idx[b][idx]])
                        else:
                            tester.assertTrue(False, "unexpected class result {}".format(result_name))

    # Unregister system/cuda shared memory regions if they exist
    su.unregister_cleanup_shm_regions(shm_handles, precreated_shm_regions, outputs,
                                        use_system_shared_memory, use_cuda_shared_memory)

    return results


# Perform inference using a "nop" model that expects some form or
# zero-sized input/output tensor.
def infer_zero(tester, pf, batch_size, tensor_dtype, input_shapes, output_shapes,
               model_version=None, use_http=True, use_grpc=True,
               use_streaming=True, shm_region_name_prefix=None,
               use_system_shared_memory=False, use_cuda_shared_memory=False,
               priority=0, timeout_us=0):
    tester.assertTrue(use_http or use_grpc or use_streaming)
    configs = []
    if use_http:
        configs.append(("localhost:8000", ProtocolType.HTTP, False))
    if use_grpc:
        configs.append(("localhost:8001", ProtocolType.GRPC, False))
    if use_streaming:
        configs.append(("localhost:8001", ProtocolType.GRPC, True))
    tester.assertEqual(len(input_shapes), len(output_shapes))
    io_cnt = len(input_shapes)

    if shm_region_name_prefix is None:
        shm_region_name_prefix = ["input", "output"]

    input_dict = {}
    output_dict = {}
    expected_dict = {}
    shm_ip_handles = list()
    shm_op_handles = list()
    shared_memory_ctx = SharedMemoryControlContext("localhost:8000",  ProtocolType.HTTP, verbose=False)

    for io_num in range(io_cnt):
        if pf == "libtorch" or pf == "libtorch_nobatch":
            input_name = "INPUT__{}".format(io_num)
            output_name = "OUTPUT__{}".format(io_num)
        else:
            input_name = "INPUT{}".format(io_num)
            output_name = "OUTPUT{}".format(io_num)

        input_list = list()
        expected_list = list()
        for b in range(batch_size):
            rtensor_dtype = _range_repr_dtype(tensor_dtype)
            if (rtensor_dtype != np.bool):
                in0 = np.random.randint(low=np.iinfo(rtensor_dtype).min,
                                        high=np.iinfo(rtensor_dtype).max,
                                        size=input_shapes[io_num], dtype=rtensor_dtype)
            else:
                in0 = np.random.choice(a=[False, True], size=input_shapes[io_num])
            if tensor_dtype != np.object:
                in0 = in0.astype(tensor_dtype)
                expected0 = np.ndarray.copy(in0)
            else:
                expected0 = np.array([unicode(str(x), encoding='utf-8')
                                for x in in0.flatten()], dtype=object)
                in0 = np.array([str(x) for x in in0.flatten()],
                                dtype=object).reshape(in0.shape)

            expected0 = expected0.reshape(output_shapes[io_num])

            input_list.append(in0)
            expected_list.append(expected0)

        expected_dict[output_name] = expected_list

        input_byte_size = tu.shape_element_count(input_shapes[io_num]) *\
                            np.dtype(tensor_dtype).itemsize * batch_size
        output_byte_size = tu.shape_element_count(output_shapes[io_num]) *\
                            np.dtype(tensor_dtype).itemsize * batch_size
        # create and register shared memory region for inputs and outputs
        shm_io_handle = su.create_register_set_either_shm_region([shm_region_name_prefix[0]+str(io_num),
                                                shm_region_name_prefix[1]+str(io_num)], input_list,
                                                input_byte_size, output_byte_size, shared_memory_ctx,
                                                use_system_shared_memory, use_cuda_shared_memory)
        if len(shm_io_handle) != 0:
            shm_ip_handles.append(shm_io_handle[0])
            shm_op_handles.append(shm_io_handle[1])
            input_dict[input_name] = (shm_ip_handles[io_num], input_shapes)
            output_dict[output_name] = (InferContext.ResultFormat.RAW, shm_op_handles[io_num])
        else:
            input_dict[input_name] = input_list
            output_dict[output_name] = InferContext.ResultFormat.RAW

    # Run inference and check results for each config
    for config in configs:
        model_name = tu.get_zero_model_name(pf, io_cnt, tensor_dtype)

        ctx = InferContext(config[0], config[1], model_name, model_version,
                           correlation_id=0, streaming=config[2],
                           verbose=True)
        results = ctx.run(input_dict, output_dict, batch_size,
                          priority=priority, timeout_us=timeout_us)

        tester.assertEqual(ctx.get_last_request_model_name(), model_name)
        if model_version is not None:
            tester.assertEqual(ctx.get_last_request_model_version(), model_version)

        tester.assertEqual(len(results), io_cnt)
        for (result_name, result_val) in iteritems(results):
            tester.assertTrue(result_name in output_dict)
            tester.assertTrue(result_name in expected_dict)
            for b in range(batch_size):
                expected = expected_dict[result_name][b]
                tester.assertEqual(result_val[b].shape, expected.shape)
                tester.assertTrue(np.array_equal(result_val[b], expected),
                                  "{}, {}, slot {}, expected: {}, got {}".format(
                                      model_name, result_name, b, expected, result_val[b]))

    if len(shm_ip_handles) != 0:
        for io_num in range(io_cnt):
            shared_memory_ctx.unregister(shm_ip_handles[io_num])
            shared_memory_ctx.unregister(shm_op_handles[io_num])
            su.destroy_either_shm_region(shm_ip_handles[io_num], use_system_shared_memory, use_cuda_shared_memory)
            su.destroy_either_shm_region(shm_op_handles[io_num], use_system_shared_memory, use_cuda_shared_memory)

    return results

# Perform inference on a model that takes a shape and a dummy tensors as inputs,
# resize the dummy tensor with the provided values in the shape tensor and finally
# return the shape of the resized tensor.
def infer_shape_tensor(tester, pf, batch_size, tensor_dtype, input_shape_values, dummy_input_shapes,
               model_version=None, use_http=True, use_grpc=True,
               use_streaming=True, shm_suffix="", use_system_shared_memory=False,
               use_cuda_shared_memory=False, priority=0, timeout_us=0):
    tester.assertTrue(use_http or use_grpc or use_streaming)
    configs = []
    if use_http:
        configs.append(("localhost:8000", ProtocolType.HTTP, False))
    if use_grpc:
        configs.append(("localhost:8001", ProtocolType.GRPC, False))
    if use_streaming:
        configs.append(("localhost:8001", ProtocolType.GRPC, True))
    tester.assertEqual(len(input_shape_values), len(dummy_input_shapes))
    io_cnt = len(input_shape_values)

    if use_system_shared_memory and use_cuda_shared_memory:
        raise ValueError("Cannot set both System and CUDA shared memory flags to 1")

    input_dict = {}
    output_dict = {}
    expected_dict = {}
    shm_ip_handles = list()
    shm_op_handles = list()
    shared_memory_ctx = SharedMemoryControlContext("localhost:8000",  ProtocolType.HTTP, verbose=False)

    for io_num in range(io_cnt):
        tester.assertTrue(pf == "plan" or pf == "plan_nobatch")

        input_name = "INPUT{}".format(io_num)
        output_name = "OUTPUT{}".format(io_num)
        dummy_input_name = "DUMMY_INPUT{}".format(io_num)
        dummy_output_name = "DUMMY_OUTPUT{}".format(io_num)

        input_list = list()
        dummy_input_list = list()
        expected_list = list()
        for b in range(batch_size):
            # Prepare the dummy tensor
            rtensor_dtype = _range_repr_dtype(tensor_dtype)
            if (rtensor_dtype != np.bool):
                dummy_in0 = np.random.randint(low=np.iinfo(rtensor_dtype).min,
                                        high=np.iinfo(rtensor_dtype).max,
                                        size=dummy_input_shapes[io_num], dtype=rtensor_dtype)
            else:
                dummy_in0 = np.random.choice(a=[False, True], size=dummy_input_shapes[io_num])
            if tensor_dtype != np.object:
                dummy_in0 = dummy_in0.astype(tensor_dtype)
            else:
                dummy_in0 = np.array([str(x) for x in in0.flatten()],
                                dtype=object).reshape(in0.shape)

            dummy_input_list.append(dummy_in0)

        # Prepare shape input tensor. Only one tensor per batch
        in0 = np.asarray(input_shape_values[io_num], dtype=np.int32)
        input_list.append(in0)

        # Prepare the expected list for the output
        expected0 = np.ndarray.copy(in0)
        expected_list.append(expected0)

        expected_dict[output_name] = expected_list

        input_byte_size = len(in0) * np.dtype(tensor_dtype).itemsize
        output_byte_size = input_byte_size * batch_size
        dummy_input_byte_size = tu.shape_element_count(dummy_input_shapes[io_num]) *\
                            np.dtype(tensor_dtype).itemsize * batch_size
        # The dimension of this tensor will be the value of the shape tensor
        dummy_output_byte_size = tu.shape_element_count(in0) *\
                            np.dtype(tensor_dtype).itemsize * batch_size
        
        # create and register shared memory region for inputs and outputs
        if use_cuda_shared_memory:
            shm_ip_handles.append(cudashm.create_shared_memory_region("input"+str(io_num)+"_data"+shm_suffix,
                                                                input_byte_size, 0))
            shm_ip_handles.append(cudashm.create_shared_memory_region("dummy_input"+str(io_num)+"_data"+shm_suffix,
                                                                dummy_input_byte_size, 0))
            shm_op_handles.append(cudashm.create_shared_memory_region("output"+str(io_num)+"_data"+shm_suffix,
                                                                output_byte_size, 0))
            shm_op_handles.append(cudashm.create_shared_memory_region("dummy_output"+str(io_num)+"_data"+shm_suffix,
                                                                dummy_output_byte_size, 0))
            
            shared_memory_ctx.cuda_register(shm_ip_handles[2 * io_num])
            shared_memory_ctx.cuda_register(shm_ip_handles[2 * io_num + 1])
            shared_memory_ctx.cuda_register(shm_op_handles[2 * io_num])
            shared_memory_ctx.cuda_register(shm_op_handles[2 * io_num + 1])

            # copy data into shared memory region for input values
            cudashm.set_shared_memory_region(shm_ip_handles[2 * io_num], input_list)
            cudashm.set_shared_memory_region(shm_ip_handles[2 * io_num + 1], dummy_input_list)
        elif use_system_shared_memory:
            shm_ip_handles.append(shm.create_shared_memory_region("input"+str(io_num)+"_data"+shm_suffix,\
                                        "/input"+str(io_num)+shm_suffix, input_byte_size))
            shm_ip_handles.append(shm.create_shared_memory_region("dumy_input"+str(io_num)+"_data"+shm_suffix,\
                                        "/dummy_input"+str(io_num)+shm_suffix, dummy_input_byte_size))
            shm_op_handles.append(shm.create_shared_memory_region("output"+str(io_num)+"_data"+shm_suffix,\
                                        "/output"+str(io_num)+shm_suffix, output_byte_size))
            shm_op_handles.append(shm.create_shared_memory_region("dummy_output"+str(io_num)+"_data"+shm_suffix,\
                                        "/dummy_output"+str(io_num)+shm_suffix, dummy_output_byte_size)) 
            shared_memory_ctx.register(shm_ip_handles[2 * io_num])
            shared_memory_ctx.register(shm_ip_handles[2 * io_num + 1])
            shared_memory_ctx.register(shm_op_handles[2 * io_num])
            shared_memory_ctx.register(shm_op_handles[2 * io_num + 1])
            # copy data into shared memory region for input values
            shm.set_shared_memory_region(shm_ip_handles[2 * io_num], input_list)
            shm.set_shared_memory_region(shm_ip_handles[2 * io_num + 1], dummy_input_list)
        if use_system_shared_memory or use_cuda_shared_memory:
            input_dict[input_name] = (shm_ip_handles[2 * io_num], [len(input_shape_values[0])])
            input_dict[dummy_input_name] = (shm_ip_handles[2 * io_num + 1], dummy_input_shapes[io_num])
            output_dict[output_name] = (InferContext.ResultFormat.RAW, shm_op_handles[2 * io_num])
            output_dict[dummy_output_name] = (InferContext.ResultFormat.RAW, shm_op_handles[2 * io_num + 1])
        else:
            input_dict[input_name] = input_list
            input_dict[dummy_input_name] = dummy_input_list
            output_dict[output_name] = InferContext.ResultFormat.RAW
            output_dict[dummy_output_name] = InferContext.ResultFormat.RAW

    # Run inference and check results for each config
    for config in configs:
        model_name = tu.get_zero_model_name(pf, io_cnt, tensor_dtype)

        ctx = InferContext(config[0], config[1], model_name, model_version,
                           correlation_id=0, streaming=config[2],
                           verbose=True)
        results = ctx.run(input_dict, output_dict, batch_size,
                          priority=priority, timeout_us=timeout_us)

        tester.assertEqual(ctx.get_last_request_model_name(), model_name)
        if model_version is not None:
            tester.assertEqual(ctx.get_last_request_model_version(), model_version)

        tester.assertEqual(len(results), 2*io_cnt)
        for (result_name, result_val) in iteritems(results):
            tester.assertTrue(result_name in output_dict)
            expected = expected_dict[output_name][0]
            for b in range(batch_size):
                if result_name == output_name:
                    tester.assertEqual(result_val[b].shape, expected.shape)
                    tester.assertTrue(np.array_equal(result_val[b], expected),
                                  "{}, {}, slot {}, expected: {}, got {}".format(
                                  model_name, result_name, b, expected, result_val[b]))
                elif result_name == dummy_output_name:
                    # The shape of the dummy output should be equal to the shape values
                    # specified in the shape tensor
                    tester.assertTrue(np.array_equal(result_val[b].shape, expected),
                                  "{}, {}, slot {}, expected: {}, got {}".format(
                                  model_name, result_name, b, expected, result_val[b]))

    if use_cuda_shared_memory or use_system_shared_memory:
        for io_num in range(2 * io_cnt):
            shared_memory_ctx.unregister(shm_ip_handles[io_num])
            shared_memory_ctx.unregister(shm_op_handles[io_num])
            if use_cuda_shared_memory:
                cudashm.destroy_shared_memory_region(shm_ip_handles[io_num])
                cudashm.destroy_shared_memory_region(shm_op_handles[io_num])
            else:
                shm.destroy_shared_memory_region(shm_ip_handles[io_num])
                shm.destroy_shared_memory_region(shm_op_handles[io_num])

    return results

