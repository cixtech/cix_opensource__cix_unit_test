#!/bin/sh

if [ $# = 0 ]; then
    echo "Usage:"
    echo "./llamacpp_bench.sh bench path/to/model.gguf"
    echo "./llamacpp_bench.sh perplexity path/to/model.gguf"
    exit 1
fi

if  [ $1 = "bench" ]; then
    /usr/share/cix/bin/llama-bench -C 0xFE1 -m $2 -pg 128,128 -t 8
fi
if  [ $1 = "perplexity" ]; then
    /usr/share/cix/bin/llama-perplexity -C 0xFE1 --prio 3 -m $2 -f ./wiki.test.raw -t 8
fi

