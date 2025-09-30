import numpy as np
from libnoe import *

fm_idxes = []
wt_idxes = []
job_cfg = {
    "partition_id" : 0,
    "dbg_dispatch" : 0,
    "dbg_core_id" : 0,
    "qos_level" : 0
}

CIX_NOE_BIN = "/usr/share/cix/testdata/npu/onnx_resnet50_3core/noe.cix"
INPUT_BIN = "/usr/share/cix/testdata/npu/onnx_resnet50_3core/input0.bin"
GOLDEN_BIN = "/usr/share/cix/testdata/npu/onnx_resnet50_3core/output.bin"

in_tensor_desc = tensor_desc_t()
out_tensor_desc = tensor_desc_t()

npu = NPU()

ret = npu.noe_init_context()
if ret == 0:
    print("npu: noe_init_context success")
else:
    print("npu: noe_init_context fail")
    exit(-1)

retmap = npu.noe_load_graph(CIX_NOE_BIN)
if retmap["ret"] == 0:
    graph_id = retmap["data"]
else:
    print("npu: noe_load_graph fail, graph: ")
    exit(-1)

retmap = npu.noe_get_tensor_count(graph_id, NOE_TENSOR_TYPE_INPUT)
if retmap["ret"] == 0:
    input_cnt = retmap["data"]
else:
    print("npu: noe_get_tensor_count fail")
    exit(-1)

in_tensor_desc = npu.noe_get_tensor_descriptor(graph_id, NOE_TENSOR_TYPE_INPUT, 0)
# print(in_tensor_desc.id)
# print(in_tensor_desc.size)
# print(in_tensor_desc.scale)
# print(in_tensor_desc.zero_point)
# print(in_tensor_desc.data_type)

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

out_tensor_desc = npu.noe_get_tensor_descriptor(graph_id, NOE_TENSOR_TYPE_OUTPUT, 0)
# print(out_tensor_desc.id)
# print(out_tensor_desc.size)
# print(out_tensor_desc.scale)
# print(out_tensor_desc.zero_point)
# print(out_tensor_desc.data_type)

if out_tensor_desc.data_type == noe_data_type_t.NOE_DATA_TYPE_S8:
    output_type = np.int8
    output_dtype_min = -127
    output_dtype_max = 128
else:
    output_type = np.uint8
    output_dtype_min = 0
    output_dtype_max = 255

# cfg_types = 0x30
# mem_dump_config= {
#     "dump_dir" : "./"
# }
## NOE_JOB_CONFIG_TYPE_DUMP_TEXT            = 0x1,
## NOE_JOB_CONFIG_TYPE_DUMP_WEIGHT          = 0x2,
## NOE_JOB_CONFIG_TYPE_DUMP_RODATA          = 0x4,
## NOE_JOB_CONFIG_TYPE_DUMP_DESCRIPTOR      = 0x8,
## NOE_JOB_CONFIG_TYPE_DUMP_INPUT           = 0x10,
## NOE_JOB_CONFIG_TYPE_DUMP_OUTPUT          = 0x20,
## NOE_JOB_CONFIG_TYPE_DUMP_REUSE           = 0x40,
## NOE_JOB_CONFIG_TYPE_DUMP_TCB_CHAIN       = 0x80,
## NOE_JOB_CONFIG_TYPE_DUMP_EMULATION       = 0x100,
## NOE_JOB_CONFIG_TYPE_DUMP_PROFILE         = 0x200,
## NOE_CONFIG_TYPE_SIMULATION               = 0x400,
## NOE_CONFIG_TYPE_HW                       = 0x800,
## NOE_GLOBAL_CONFIG_TYPE_DISABLE_VER_CHECK = 0x1000,
## NOE_GLOBAL_CONFIG_TYPE_ENABLE_VER_CHECK  = 0x2000,
# ret = npu.noe_config_job(job_id, cfg_types, mem_dump_config)

for loop_count in range(10000):
    retmap = npu.noe_create_job(graph_id, job_cfg, fm_idxes, wt_idxes)
    if retmap["ret"] == 0:
        job_id = retmap["data"]
    else:
        print("npu: noe_create_job fail")
        exit(-1)

    for i in range(input_cnt):
        ret = npu.noe_load_tensor_from_file(job_id, 0, INPUT_BIN)
        ret = npu.noe_job_infer_sync(job_id, -1)

        for j in range(output_cnt):
            retmap = npu.noe_get_tensor(job_id, NOE_TENSOR_TYPE_OUTPUT, j, D_INT8)
            if retmap["ret"][0] == 0:
                output_data = retmap["data"]
            else:
                print("npu: noe_get_tensor output fail")
                exit(-1)

        golden_output = np.fromfile(GOLDEN_BIN, dtype=output_type)

        if not np.array_equal(output_data, golden_output):
            print(f"Output verification failed at loop {loop_count}")
            exit(-1)
    ret = npu.noe_clean_job(job_id)

print(f"NPU stress successful over loop {loop_count}")

ret = npu.noe_unload_graph(graph_id)
ret = npu.noe_deinit_context()
if ret == 0:
    print("npu: noe_deinit_context success")
else:
    print("npu: noe_deinit_context fail")
    exit(-1)
