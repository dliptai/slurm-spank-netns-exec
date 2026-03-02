# Where are the Slurm include files?
# SLURMINC=/apps/slurm/latest/include
SLURMINC=/usr/include

PLUGINDIR=.
NETNSSPANKDIR=${PLUGINDIR}/netns_isolate


all: netns_isolate.so

netns_isolate.so: netns_isolate.c
	gcc -I$(SLURMINC) -std=gnu99 -Wall -o netns_isolate.o -fPIC -c netns_isolate.c
	gcc -shared -o netns_isolate.so netns_isolate.o

clean:
	rm -f netns_isolate.o netns_isolate.so

install: all
	mkdir -p ${NETNSSPANKDIR}
	install -m 755 netns_isolate.so ${NETNSSPANKDIR}/
