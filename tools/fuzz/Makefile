# SPDX-License-Identifier: GPL-2.0
# libFuzzer harness for driver/gb-bt656.c. Needs clang with compiler-rt.
#
#   make                  build fuzz-bt656
#   make seeds            cut a seed corpus from captures/samples/
#   make run              fuzz for SECS seconds (default 3600) on FORK workers

CC        := clang
CFLAGS    := -g -O1 -fno-omit-frame-pointer \
             -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
ROOT      := ../..
SECS      ?= 3600
FORK      ?= 36

fuzz-bt656: fuzz-bt656.c $(ROOT)/driver/gb-bt656.c $(ROOT)/driver/gb-bt656.h
	$(CC) $(CFLAGS) -I$(ROOT)/driver -o $@ fuzz-bt656.c $(ROOT)/driver/gb-bt656.c

# Each seed: a config byte (bit 0 PAL, bit 1 progressive), a chunk-pattern byte,
# then a 3 MB slice of a real capture. Under ~2 MB no frame is ACCEPTED, and
# the accepted path is the one that hands the assembler a new buffer.
seeds:
	mkdir -p corpus
	for s in bt656-slice bt656-osd-slice; do \
	  for cfg in 0 1 2 3; do \
	    for pat in 0 1 4; do \
	      { printf "\\$$(printf %o $$cfg)\\$$(printf %o $$pat)"; \
	        head -c 3000000 $(ROOT)/captures/samples/$$s.raw; } \
	        > corpus/$$s-c$$cfg-p$$pat; \
	    done; \
	  done; \
	done

run: fuzz-bt656 seeds
	mkdir -p crashes
	./fuzz-bt656 -max_len=3200000 -timeout=30 -rss_limit_mb=4096 \
	  -dict=bt656.dict -fork=$(FORK) -ignore_crashes=1 \
	  -max_total_time=$(SECS) -artifact_prefix=crashes/ \
	  -print_final_stats=1 corpus/

clean:
	rm -f fuzz-bt656

.PHONY: seeds run clean
