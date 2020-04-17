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
from tritonhttpclient.utils import triton_to_np_dtype, np_to_triton_dtype
from tritonhttpclient.utils import InferenceServerException
import tritonsharedmemoryutils.shared_memory as shm
import tritonsharedmemoryutils.cuda_shared_memory as cudashm
import test_util as tu
import shm_util_v2 as su

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

    OUTPUT0 = "OUTPUT0"
    OUTPUT1 = "OUTPUT1"
    INPUT0 = "INPUT0"
    INPUT1 = "INPUT1"
    if pf == "libtorch" or pf == "libtorch_nobatch":
        OUTPUT0 = "OUTPUT__0"
        OUTPUT1 = "OUTPUT__1"
        INPUT0 = "INPUT__0"
        INPUT1 = "INPUT__1"

    inputs = []
    inputs.append(httpclient.InferInput(INPUT0, tensor_shape, np_to_triton_dtype(input_dtype)))
    inputs.append(httpclient.InferInput(INPUT1, tensor_shape, np_to_triton_dtype(input_dtype)))

    output0_byte_size = sum([e0.nbytes for e0 in expected0_list_tmp])
    output1_byte_size = sum([e1.nbytes for e1 in expected1_list_tmp])

    # Create and register system/cuda shared memory regions if needed
    shm_regions = su.create_register_set_shm_regions(inputs, input0_list_tmp, input1_list_tmp, output0_byte_size,
                                                    output1_byte_size, outputs, shm_region_names, precreated_shm_regions,
                                                    use_system_shared_memory, use_cuda_shared_memory)

    # Run inference and check results for each config
    for config in configs:
        model_name = tu.get_model_name(pf, input_dtype, output0_dtype, output1_dtype)
        
        if config[1] == ProtocolType.HTTP:
            triton_client = httpclient.InferenceServerClient(config[0])
        else:
            triton_client = grpcclient.InferenceServerClient(config[0])

        expected0_sort_idx = [ np.flip(np.argsort(x.flatten()), 0) for x in expected0_val_list ]
        expected1_sort_idx = [ np.flip(np.argsort(x.flatten()), 0) for x in expected1_val_list ]

        output_req = []
        i=0
        if "OUTPUT0" in outputs:
            if len(shm_regions) != 0:
                if config[1] == ProtocolType.HTTP:
                    output_req.append(httpclient.InferOutput(OUTPUT0, binary_data=config[3]))
                else:
                    output_req.append(grpcclient.InferOutput(OUTPUT0))
                
                if precreated_shm_regions is None:
                    output_req[-1].set_shared_memory_region(shm_regions[2]+'_data', output0_byte_size)
                else:
                    output_req[-1].set_shared_memory_region(
                        precreated_shm_regions[0], output0_byte_size)
            else:
                if output0_raw:
                    if config[1] == ProtocolType.HTTP:
                        outputs.append(httpclient.InferOutput(OUTPUT0, binary_data=config[3]))
                    else:
                        outputs.append(grpcclient.InferOutput(OUTPUT0))
                else:
                    if config[1] == ProtocolType.HTTP:
                        output_req.append(httpclient.InferOutput(
                            OUTPUT0, binary_data=config[3], class_count=num_classes))
                    else:
                        output_req.append(grpcclient.InferOutput(OUTPUT0, class_count=num_classes))                        
            i+=1
        if "OUTPUT1" in outputs:
            if len(shm_regions) != 0:
                if config[1] == ProtocolType.HTTP:
                    output_req.append(httpclient.InferOutput(OUTPUT1, binary_data=config[3]))
                else:
                    output_req.append(grpcclient.InferOutput(OUTPUT1))
                
                if precreated_shm_regions is None:
                    output_req[-1].set_shared_memory_region(shm_regions[2+i]+'_data', output1_byte_size)
                else:
                    output_req[-1].set_shared_memory_region(
                        precreated_shm_regions[i], output1_byte_size)
            else:
                if output1_raw:
                    if config[1] == ProtocolType.HTTP:
                        output_req.append(httpclient.InferOutput(OUTPUT1, binary_data=config[3]))
                    else:
                        output_req.append(grpcclient.InferOutput(OUTPUT1))
                else:
                    if config[1] == ProtocolType.HTTP:
                        output_req.append(httpclient.InferOutput(
                            OUTPUT1, binary_data=config[3], class_count=num_classes))
                    else:
                        output_req.append(grpcclient.InferOutput(
                            OUTPUT1, class_count=num_classes))

        if config[2]:
            results = triton_client.async_stream_infer(model_name,
                                             inputs,
                                             model_version=model_version,
                                             stream=stream,
                                             outputs=output_req)
        else:
            results = triton_client.infer(model_name,
                                inputs,
                                model_version=model_version,
                                outputs=output_req)

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
    su.unregister_cleanup_shm_regions(shm_regions, precreated_shm_regions, outputs,
                                        use_system_shared_memory, use_cuda_shared_memory)

    return results