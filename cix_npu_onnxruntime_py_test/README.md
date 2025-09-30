## Quick Start
### Run python example:

```shell
$ cd /usr/share/cix/bin/npu
$ python3 run_model.py -m /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/mnist-12-int8.onnx -i /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/input_0.pb -e CPU
$ python3 run_model.py -m /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/mnist-12-int8.onnx -i /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/input_0.pb -e ZHOUYI
```

#### python with cache mode

1. Generate cache context onnx model: -b: embed_mode, -p: specify filepath of context onnx model

```shell
$ python3 run_model.py -m /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/mnist-12-int8.onnx -i /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/input_0.pb -b 1 -p /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/mnist-12-int8.onnx_ctx.onnx -f
```

2. Runcache context onnx model

```shell
$ python3 run_model.py -m /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/mnist-12-int8.onnx_ctx.onnx -i /usr/share/cix/testdata/npu/onnxruntime/mnist-12-int8/input_0.pb -e ZHOUYI
```
