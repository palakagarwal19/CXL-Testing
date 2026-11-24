	
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

CFLAGS ?= -O2 -Wall -Wextra -std=c11

PROGRAMS = test_identify \
           test_partition_info \
           test_lsa_get \
           test_lsa_set \
           test_health_info \
           test_event_records

all: $(PROGRAMS)

%: %.c cxl_test_common.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS) *.log *_result.txt

.PHONY: all clean

