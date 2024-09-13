#!/bin/bash
set -x
limit=300

i=0
plim=50
while [ $i -lt $limit ]
do
    ((i++))
    if [ "$i" -eq "$plim" ] 
    then
        cat rdcp.c >> rdcptest
        echo -e "plim: $plim\n"
        ((plim+=50))
    fi
done