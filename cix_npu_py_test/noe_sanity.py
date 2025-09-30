import numpy as np
import filecmp
import argparse
from libnoe import *

parser = argparse.ArgumentParser(description="NOE python inference test program")
parser.add_argument('-g', '--graph', type=str, default='/usr/share/cix/testdata/npu/onnx_resnet50_3core/noe.cix', help='graph file path')
parser.add_argument('-i', '--ifile', type=str, default='/usr/share/cix/testdata/npu/onnx_resnet50_3core/input0.bin', help="input file path")
parser.add_argument('-o', '--ofile', type=str, default='/usr/share/cix/testdata/npu/onnx_resnet50_3core/output_soc.bin', help="output file path")
parser.add_argument('-c', '--cfile', type=str, default='/usr/share/cix/testdata/npu/onnx_resnet50_3core/output.bin', help="golden file path")
parser.add_argument('-n', '--nums', type=int, default='1000', help='inference times')
parser.add_argument('-m', '--mode', type=str, default='single', help='single or multi inference job')
args = parser.parse_args()

print(f"Graph file path: {args.graph}")
print(f"Input file path: {args.ifile}")
print(f"Output file path: {args.ofile}")
print(f"Golden file path: {args.cfile}")
print(f"Inference times: {args.nums}")
print(f"Inference mode: {args.mode}")


fm_idxes = []
wt_idxes = []
job_cfg = {
    "partition_id" : 0,
    "dbg_dispatch" : 0,
    "dbg_core_id" : 0,
    "qos_level" : 0
}

cix_noe_bin = args.graph
input_bin = args.ifile
output_bin = args.ofile
golden_bin = args.cfile
inference_times = args.nums
inference_mode = args.mode


in_tensor_desc = tensor_desc_t()
out_tensor_desc = tensor_desc_t()

npu = NPU()

ret = npu.noe_init_context()
if ret == 0:
    print("npu: noe_init_context success")
else:
    print("npu: noe_init_context fail")
    exit(-1)

retmap = npu.noe_load_graph(cix_noe_bin)
if retmap["ret"] == 0:
    print("npu: noe_load_graph success")
    graph_id = retmap["data"]
else:
    print("npu: noe_load_graph fail ")
    exit(-1)

retmap = npu.noe_get_tensor_count(graph_id, NOE_TENSOR_TYPE_INPUT)
if retmap["ret"] == 0:
    input_cnt = retmap["data"]
else:
    print("npu: noe_get_tensor_count fail")
    exit(-1)
print("input tensor count is " + str(input_cnt))

in_tensor_desc = npu.noe_get_tensor_descriptor(graph_id, NOE_TENSOR_TYPE_INPUT, 0)
if in_tensor_desc.data_type == noe_data_type_t.NOE_DATA_TYPE_U8:
    input_type = np.uint8
    input_dtype_min = 0
    input_dtype_max = 255
else:
    input_type = np.int8
    input_dtype_min = -127
    input_dtype_max = 128

retmap = npu.noe_get_tensor_count(graph_id, NOE_TENSOR_TYPE_OUTPUT)
if retmap["ret"] == 0:
    output_cnt = retmap["data"]
else:
    print("npu: noe_get_output_tensor fail")
    exit(-1)
print("output tensor count is " + str(output_cnt))

out_tensor_desc = npu.noe_get_tensor_descriptor(graph_id, NOE_TENSOR_TYPE_OUTPUT, 0)
if out_tensor_desc.data_type == noe_data_type_t.NOE_DATA_TYPE_S8:
    output_type = np.int8
    output_dtype_min = -127
    output_dtype_max = 128
else:
    output_type = np.uint8
    output_dtype_min = 0
    output_dtype_max = 255

output_memory = [np.empty(out_tensor_desc.size, dtype=output_type) for _ in range(output_cnt)]

if inference_mode == 'single':
    retmap = npu.noe_create_job(graph_id, job_cfg, fm_idxes)
    if retmap["ret"] == 0:
        print("npu single: noe_create_job success")
        job_id = retmap["data"]
    else:
        print("npu single: noe_create_job fail")
        exit(-1)

for i in range(inference_times):
    if inference_mode == 'multi':
        retmap = npu.noe_create_job(graph_id, job_cfg, fm_idxes)
        if retmap["ret"] == 0:
            print("npu multi: noe_create_job success")
            job_id = retmap["data"]
        else:
            print("npu multi: noe_create_job fail")
            exit(-1)

    ret = npu.noe_load_tensor_from_file(job_id, 0, input_bin)

    ret = npu.noe_job_infer_sync(job_id, -1)

    for j in range(output_cnt):
        retmap = npu.noe_get_tensor(job_id, NOE_TENSOR_TYPE_OUTPUT, j, D_INT8)
        if retmap["ret"][0] == 0:
            output_data = retmap["data"]
        else:
            print("npu: noe_get_tensor output fail")
            exit(-1)

        np.copyto(output_memory[j], output_data)
        output_memory[j].tofile(output_bin)

        ret = filecmp.cmp(output_bin, golden_bin)
        if ret:
            print(f"npu: comparing pass for {i}")
        else:
            print(f"npu: comparing fail for {i}")

        del output_data

    if inference_mode == 'multi':
        ret = npu.noe_clean_job(job_id)
        if ret == 0:
            print("npu multi: noe_clean_job success")
        else:
            print("npu multi: noe_clean_job fail")
            exit(-1)

if inference_mode == 'single':
    ret = npu.noe_clean_job(job_id)
    if ret == 0:
        print("npu single: noe_clean_job success")
    else:
        print("npu single: noe_clean_job fail")
        exit(-1)

ret = npu.noe_unload_graph(graph_id)
if ret == 0:
    print("npu: noe_unload_graph success")
else:
    print("npu: noe_unload_graph fail")
    exit(-1)

ret = npu.noe_deinit_context()
if ret == 0:
    print("npu: noe_deinit_context success\n")
else:
    print("npu: noe_deinit_context fail\n")
    exit(-1)
