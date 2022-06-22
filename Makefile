TMPDIR = /tmp/ssd

all: ssd_fuse ssd_fuse_dut
ssd_fuse: ssd_fuse.c ssd_fuse_header.h
	gcc -Wall ssd_fuse.c `pkg-config fuse3 --cflags --libs` -D_FILE_OFFSET_BITS=64 -o ssd_fuse
ssd_fuse_dut: ssd_fuse_dut.c ssd_fuse_header.h
	gcc -Wall ssd_fuse_dut.c -o ssd_fuse_dut
clean:
	rm ssd_fuse ssd_fuse_dut
mount: ssd_fuse
	mkdir -p $(TMPDIR)
	./ssd_fuse -d /tmp/ssd
	rm -rf $(TMPDIR)