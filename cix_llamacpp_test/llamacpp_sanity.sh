#!/bin/sh

if [ $# = 0 ]; then
    echo "Usage:"
    echo "./llamacpp_sanity.sh server path/to/model.gguf"
    echo "./llamacpp_sanity.sh cli path/to/model.gguf"
    exit 1
fi

if  [ $1 = "server" ]; then
    hostname=$(hostname -I | awk '{print $1}')
    /usr/share/cix/bin/llama-server -C 0xFE1 --prio 3 -m $2 -t 8 -c 4096 --host $hostname --port 8080
fi
if  [ $1 = "cli" ]; then
    /usr/share/cix/bin/llama-cli -C 0xFE1 --prio 3 -m $2 -t 8 -c 4096 --conversation
fi

