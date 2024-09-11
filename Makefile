CFLAGS += -Wall
LDFLAGS += -libverbs -lrdmacm -lpthread
XSLTPROC = /usr/bin/xsltproc
CC = /usr/bin/gcc

# DIR #
DIRINCLUDE 	= ./include
DIRRDCPCS 	= ./rdcpcs
DIRUTILS 	= ./utils
DIRXSL		= ./xsl

# SRC #
SRCRDCPCS = $(wildcard $(DIRRDCPCS)/*.c)
SRCUTILS = $(wildcard $(DIRUTILS)/*.c)
SRCFILE += $(SRCRDCPCS) $(SRCUTILS) rdcp.c

# INCLUDE #
RDCPINCLUDE = -I$(DIRINCLUDE)

all: rdcp rdcp.8 rdcp.man
default: all

#生成ngroff格式文件
rdcp.8: rdcp.8.xml
	$(XSLTPROC) -o $@ $(DIRXSL)/manpages/docbook.xsl $<

#生成rdcp使用手册
rdcp.man:
	nroff -man rdcp.8 | sed 's/.\{0,1\}//g' > rdcp.man 

#生成rdcp程序
rdcp: $(SRCFILE)
	$(CC) $(CFLAGS) $(RDCPINCLUDE) -o rdcp $(SRCFILE) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f rdcp
	rm -f rdcp.8
	rm -f rdcp.man
