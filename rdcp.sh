#!/bin/bash
limit=100

i=0
while [ $i -lt $limit ]
do
    ((i++))
    cat rdcp.c >> rdcptest
done