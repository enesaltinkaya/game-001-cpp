# Save this as symbolize.sh
#!/bin/bash
while read line; do
    if [[ $line =~ build/c-game\(\+0x([0-9a-f]+)\) ]]; then
        addr="0x${BASH_REMATCH[1]}"
        echo "Address $addr:"
        llvm-symbolizer -e build/c-game -f -C -i "$addr"
        echo
    fi
done