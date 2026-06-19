CC := x86_64-linux-gnu-gcc
CFLAGS := -static -nostdlib -ffreestanding -O2

all: hello

hello: hello.c
	$(CC) $(CFLAGS) hello.c -o hello

clean:
	rm -f hello
