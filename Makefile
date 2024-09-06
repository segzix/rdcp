CFLAGS += -Wall
LDFLAGS += -libverbs -lrdmacm -lpthread
XSLTPROC = /usr/bin/xsltproc
CC = /usr/bin/gcc

all: rdcp rdcp.8 rdcp.man
default: all

#生成ngroff格式文件
rdcp.8: rdcp.8.xml
	$(XSLTPROC) -o $@ ./xsl/manpages/docbook.xsl $<

#生成rdcp使用手册
rdcp.man:
	nroff -man rdcp.8 | sed 's/.\{0,1\}//g' > rdcp.man 

.PHONY: clean
clean:
	rm -f rdcp
	rm -f rdcp.8
	rm -f rdcp.man

#生成rdcp程序
rdcp: rdcp.c
	$(CC) $(CFLAGS) -o rdcp rdcp.c $(LDFLAGS)
