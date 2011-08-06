fat: fat.c fat.h
	gcc fat.c -fno-stack-protector -g -lfuse -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 -o fat

clean:
	@rm -f fat *.o
