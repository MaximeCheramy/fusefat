fat: fat.c
	#gcc fat.c -g -lfuse -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=22 -o fat
	gcc fat.c -fno-stack-protector -g -lfuse -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=22 -o fat

clean:
	@rm -f fat *.o
