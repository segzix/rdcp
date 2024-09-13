#!/bin/bash
limit=300000

i=0
plim=5000
while [ $i -lt $limit ]
do
    ((i++))
    if [ "$i" -eq "$plim" ] 
    then
        cat rdcp.c >> rdcptest
        echo -e "plim: $plim\n"
        ((plim+=5000))
    fi
done