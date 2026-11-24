CC := riscv64-linux-gnu-gcc
CFLAGS := -O2 -Wall -Wextra -static -I$(HOME)/linux/usr/include
TARGET := cci_full_suite
SRC := cci_full_suite.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) *.log *_result.txt

.PHONY: all clean
