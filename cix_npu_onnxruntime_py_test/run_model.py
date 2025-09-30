import os
import sys
import argparse
import numpy as np
import onnx
from onnx import TensorProto
from onnx import numpy_helper
import onnxruntime
from ZhouyiOperators import operators
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))


def get_tensor_info(graph_info):
    tensors_name = ""
    tensors_types = []
    tensors_shapes = []
    for inp in graph_info:
        tensors_types.append(inp.type.tensor_type.elem_type)
        tensors_name = tensors_name + "," + inp.name if tensors_name != "" else inp.name
        shape = []
        for dim in inp.type.tensor_type.shape.dim:
            shape.append(dim.dim_value)
        tensors_shapes.append(shape)
    return tensors_name, tensors_types, tensors_shapes


def np_type(tensor_type):
    typemaps = {
        TensorProto.INT8: np.int8,
        TensorProto.UINT8: np.uint8,
        TensorProto.FLOAT: np.float32,
        TensorProto.FLOAT16: np.float16,
        TensorProto.UINT16: np.uint16,
        TensorProto.UINT32: np.uint32,
        TensorProto.INT16: np.int16,
        TensorProto.INT32: np.int32,
        TensorProto.INT64: np.int64,
        TensorProto.UINT64: np.uint64,
        TensorProto.BOOL: np.bool_,
    }
    if tensor_type in typemaps:
        return typemaps[tensor_type]


def get_args():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("-m", "--model", required=True, help="The model file.")
    parser.add_argument(
        "-i",
        "--input",
        default=None,
        help="The input file, format as: 'input0.pb,input1.pb', if not provide, generate random data.",
    )
    parser.add_argument(
        "-e",
        "--ep",
        required=False,
        default="ZhouyiExecutionProvider",
        help="Execution Provider. default:Zhouyi",
    )
    parser.add_argument("-f", "--enable_cache", action="store_true", default=False, help="enable EP Context cache")
    parser.add_argument(
      "-b",
      "--embed_mode",
      required=False,
      default=1,
      help="EPContext node attribute: 'ep_cache_context': 0: relative_path, 1: cache content. Default to 1")
    parser.add_argument(
      "-p",
      "--cache_path",
      required=False,
      default="",
      help="specify file path for Onnx model which has EP context, such as './xx_ctx.onnx'. Default to <origin_onnxfile_name>_ctx.onnx if not specified")
    parser.add_argument(
      "-v",
      "--verbose",
      required=False,
      action="store_true",
      help="logging with verbose")
    return parser.parse_args()


def get_modified_params(args):
    params = dict()
    params["model"] = args.model
    params["input"] = args.input
    ep_upper = args.ep.upper()
    if "CPU" in ep_upper:
        params["ep"] = "CPUExecutionProvider"
    elif "ZHOUYI" in ep_upper:
        params["ep"] = "ZhouyiExecutionProvider"
    else:
        params["ep"] = "ZhouyiExecutionProvider"
    params["enable_cache"] = args.enable_cache
    params["embed_mode"] = args.embed_mode
    params["cache_path"] = args.cache_path
    params["verbose"] = args.verbose
    return params


def main():
    print("Supports providers: ", onnxruntime.get_available_providers())

    args = get_args()
    param_dict = get_modified_params(args)
    model_path = param_dict["model"]
    input_path = param_dict["input"]
    print(param_dict["ep"])
    onnx_model = onnx.load(model_path)
    onnx.checker.check_model(onnx_model)
    inputs_name, inputs_type, inputs_shape = get_tensor_info(onnx_model.graph.input)
    print(f"inputs name: {inputs_name}, type: {inputs_type}, shape: {inputs_shape}")
    outputs_name, outputs_type, outputs_shape = get_tensor_info(onnx_model.graph.output)
    print(f"outputs name: {outputs_name}, type: {outputs_type}, shape: {outputs_shape}")

    inputs_list = []
    if input_path:
        input_path_list = input_path.split(",")
        for i, inp in enumerate(input_path_list):
            file_path = os.path.dirname(os.path.abspath(inp))
            if inp.endswith(".pb"):
                with open(inp, "rb") as f:
                    proto = TensorProto()
                    proto.ParseFromString(f.read())
                    npy_input = np.frombuffer(
                        proto.raw_data, dtype=np_type(inputs_type[i])
                    )
                    inputs_list.append(npy_input.reshape(inputs_shape[i]))
            elif inp.endswith(".bin"):
                npy_input = np.fromfile(inp, dtype=np_type(inputs_type[i]))
                inputs_list.append(npy_input.reshape(inputs_shape[i]))
            elif inp.endswith(".npy"):
                npy_input = np.load(inp)
                inputs_list.append(npy_input)

    else:
        file_path = "./"
        for i in range(len(inputs_shape)):
            shape_size = 1
            for shape in inputs_shape[i]:
                shape_size *= shape
            inp = np.random.rand(shape_size).astype(np_type(inputs_type[i]))
            inp = inp.astype(np_type(inputs_type[i]))
            inp = inp.reshape(inputs_shape[i])
            inputs_list.append(inp)
            tensor = numpy_helper.from_array(inp)
            with open(os.path.join(file_path, f"input_{i}.pb"), "wb") as f:
                f.write(tensor.SerializeToString())

    ort_inputs = {}
    session_options = onnxruntime.SessionOptions()
    if param_dict["verbose"]:
        session_options.log_severity_level = 0
    if param_dict["enable_cache"]:
        session_options.add_session_config_entry("ep.context_enable", "1")
        session_options.add_session_config_entry("ep.context_embed_mode", str(param_dict["embed_mode"]))
        session_options.add_session_config_entry("ep.context_file_path", str(param_dict["cache_path"]))

    # onnx_model.SerializeToString() will not provide information of model path, which will result cpp ModelPath() null
    ort_session = onnxruntime.InferenceSession(
        model_path, sess_options=session_options, providers=[param_dict["ep"]]
    )
    if len(inputs_list) != len(ort_session.get_inputs()):
        print(
            f"[ERROR].inputs list size: {len(inputs_list)}, session input size: {len(ort_session.get_inputs())}!"
        )
        return

    for i, input_ele in enumerate(ort_session.get_inputs()):
        ort_inputs[input_ele.name] = inputs_list[i]
    ort_outputs_name = [x.name for x in ort_session.get_outputs()]
    print(f"outputs name: {ort_outputs_name}")

    start = time.perf_counter()
    ort_outs = ort_session.run(ort_outputs_name, ort_inputs)
    print(f"Inference time: {(time.perf_counter() - start)*1000:.2f} ms")
    
    for i in range(len(ort_outs)):
        tensor_o = numpy_helper.from_array(ort_outs[i])
        with open(os.path.join(file_path, f"output_{i}.pb"), "wb") as f:
            f.write(tensor_o.SerializeToString())
        npy_data = np.frombuffer(tensor_o.raw_data, dtype=np_type(tensor_o.data_type))
        np.save(f"output_{i}.npy", npy_data)
        npy_data.tofile(f"output_{i}.bin")


if __name__ == "__main__":
    main()