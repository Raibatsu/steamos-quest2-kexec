#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/kexec.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef KEXEC_FILE_UNLOAD
#define KEXEC_FILE_UNLOAD 0x00000001
#endif
#ifndef KEXEC_FILE_NO_INITRAMFS
#define KEXEC_FILE_NO_INITRAMFS 0x00000004
#endif
#ifndef KEXEC_FILE_DEBUG
#define KEXEC_FILE_DEBUG 0x00000008
#endif
#ifndef KEXEC_FILE_NO_CMA
#define KEXEC_FILE_NO_CMA 0x00000010
#endif
#ifndef KEXEC_ARCH_AARCH64
#define KEXEC_ARCH_AARCH64 (183 << 16)
#endif

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s [-d] [-n] [-l] [-K] <Image> <initrd|-> [cmdline]\n"
		"  %s [-l] [-K] -D <dtb> <Image> <initrd|->\n"
		"  %s -e [-K]\n"
		"  %s -u\n",
		prog, prog, prog, prog);
}

static uint64_t align_up_u64(uint64_t val, uint64_t align)
{
	return (val + align - 1) & ~(align - 1);
}

static uint64_t read_le64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static void *read_entire_file(const char *path, size_t *out_sz)
{
	struct stat st;
	int fd;
	uint8_t *buf;
	size_t off = 0, sz;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return NULL;
	}
	if (fstat(fd, &st) < 0 || st.st_size <= 0) {
		perror(path);
		close(fd);
		return NULL;
	}
	sz = (size_t)st.st_size;
	buf = malloc(sz);
	if (!buf) {
		perror("malloc");
		close(fd);
		return NULL;
	}
	while (off < sz) {
		ssize_t n = read(fd, buf + off, sz - off);
		if (n <= 0) {
			perror(path);
			free(buf);
			close(fd);
			return NULL;
		}
		off += (size_t)n;
	}
	close(fd);
	*out_sz = sz;
	return buf;
}

struct arm64_trampoline {
	uint32_t code[6];
	uint64_t addrs[2];
};

static int load_with_dtb(const char *kpath, const char *rpath, const char *dpath)
{
	uint8_t *kbuf = NULL, *rbuf = NULL, *dbuf = NULL;
	size_t ksz = 0, rsz = 0, dsz = 0;
	uint64_t text_offset, image_size;
	uint64_t kernel_phys, kernel_memsz, initrd_phys, initrd_memsz = 0;
	uint64_t dtb_phys, dtb_memsz, tramp_phys;
	struct arm64_trampoline *tramp = NULL;
	struct kexec_segment segs[4];
	unsigned long nr_segs = 0;
	int ret = -1;

	kbuf = read_entire_file(kpath, &ksz);
	if (!kbuf)
		goto out;
	if (ksz < 64 || memcmp(kbuf + 56, "ARMd", 4) != 0) {
		fprintf(stderr, "%s: not an arm64 Image\n", kpath);
		goto out;
	}
	text_offset = read_le64(kbuf + 8);
	image_size = read_le64(kbuf + 16);
	if (image_size < ksz)
		image_size = ksz;

	if (strcmp(rpath, "-") != 0) {
		rbuf = read_entire_file(rpath, &rsz);
		if (!rbuf)
			goto out;
	}

	dbuf = read_entire_file(dpath, &dsz);
	if (!dbuf)
		goto out;

	tramp = calloc(1, 4096);
	if (!tramp) {
		perror("calloc");
		goto out;
	}

	kernel_phys = 0x90600000ULL + text_offset;
	kernel_memsz = align_up_u64(image_size, 4096);
	initrd_phys = kernel_phys + kernel_memsz;
	if (rbuf)
		initrd_memsz = align_up_u64(rsz, 4096);
	dtb_phys = align_up_u64(initrd_phys + initrd_memsz, 0x200000ULL);
	dtb_memsz = align_up_u64(dsz, 4096);
	tramp_phys = dtb_phys + dtb_memsz;

	/* ldr x0, .+24; ldr x4, .+28; mov x1,xzr; mov x2,xzr; mov x3,xzr; br x4 */
	tramp->code[0] = 0x580000c0U;
	tramp->code[1] = 0x580000e4U;
	tramp->code[2] = 0xaa1f03e1U;
	tramp->code[3] = 0xaa1f03e2U;
	tramp->code[4] = 0xaa1f03e3U;
	tramp->code[5] = 0xd61f0080U;
	tramp->addrs[0] = dtb_phys;
	tramp->addrs[1] = kernel_phys;

	memset(segs, 0, sizeof(segs));
	segs[nr_segs++] = (struct kexec_segment){
		.buf = kbuf, .bufsz = ksz,
		.mem = (const void *)(uintptr_t)kernel_phys, .memsz = kernel_memsz,
	};
	if (rbuf) {
		segs[nr_segs++] = (struct kexec_segment){
			.buf = rbuf, .bufsz = rsz,
			.mem = (const void *)(uintptr_t)initrd_phys, .memsz = initrd_memsz,
		};
	}
	segs[nr_segs++] = (struct kexec_segment){
		.buf = dbuf, .bufsz = dsz,
		.mem = (const void *)(uintptr_t)dtb_phys, .memsz = dtb_memsz,
	};
	segs[nr_segs++] = (struct kexec_segment){
		.buf = tramp, .bufsz = sizeof(*tramp),
		.mem = (const void *)(uintptr_t)tramp_phys, .memsz = 4096,
	};

	printf("kexec_load: kernel=0x%llx+0x%llx initrd=0x%llx+0x%llx dtb=0x%llx+0x%llx tramp=0x%llx\n",
	       (unsigned long long)kernel_phys, (unsigned long long)kernel_memsz,
	       (unsigned long long)initrd_phys, (unsigned long long)initrd_memsz,
	       (unsigned long long)dtb_phys, (unsigned long long)dtb_memsz,
	       (unsigned long long)tramp_phys);

	if (syscall(SYS_kexec_load, (unsigned long)tramp_phys, nr_segs, segs,
		    KEXEC_ARCH_AARCH64) < 0) {
		perror("kexec_load");
		goto out;
	}
	ret = 0;

out:
	free(tramp);
	free(dbuf);
	free(rbuf);
	free(kbuf);
	return ret;
}

static void write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	size_t len;
	ssize_t ignored;

	if (fd < 0)
		return;
	len = strlen(val);
	ignored = write(fd, val, len);
	(void)ignored;
	close(fd);
}

static int read_proc_cmdline(char *buf, size_t sz)
{
	int fd = open("/proc/cmdline", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sz - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	const char *kpath, *rpath, *cmdline, *dpath = NULL;
	char proc_cmd[4096];
	int kfd, rfd = -1;
	int opt;
	int load_only = 0, exec_only = 0, unload = 0, skip_usb_unbind = 0;
	unsigned long flags = 0;
	unsigned long clen;

	while ((opt = getopt(argc, argv, "+dnleuKD:")) != -1) {
		switch (opt) {
		case 'd':
			flags |= KEXEC_FILE_DEBUG;
			break;
		case 'n':
			flags |= KEXEC_FILE_NO_CMA;
			break;
		case 'l':
			load_only = 1;
			break;
		case 'e':
			exec_only = 1;
			break;
		case 'u':
			unload = 1;
			break;
		case 'K':
			skip_usb_unbind = 1;
			break;
		case 'D':
			dpath = optarg;
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (unload + exec_only + load_only > 1 || (dpath && (unload || exec_only))) {
		usage(argv[0]);
		return 2;
	}

	if (unload) {
		if (optind != argc) {
			usage(argv[0]);
			return 2;
		}
		if (syscall(SYS_kexec_file_load, -1, -1, 0, NULL,
			    KEXEC_FILE_UNLOAD) < 0) {
			perror("kexec_file_load(UNLOAD)");
			return 1;
		}
		printf("kexec_file_load UNLOAD OK\n");
		return 0;
	}

	if (exec_only) {
		if (optind != argc) {
			usage(argv[0]);
			return 2;
		}
		goto do_exec;
	}

	if (dpath) {
		if (argc - optind != 2) {
			usage(argv[0]);
			return 2;
		}
		if (load_with_dtb(argv[optind], argv[optind + 1], dpath) < 0)
			return 1;
		if (load_only) {
			printf("kexec_load OK (load-only)\n");
			return 0;
		}
		goto do_exec;
	}

	if (argc - optind < 2 || argc - optind > 3) {
		usage(argv[0]);
		return 2;
	}

	kpath = argv[optind];
	rpath = argv[optind + 1];

	kfd = open(kpath, O_RDONLY);
	if (kfd < 0) {
		perror(kpath);
		return 1;
	}

	if (!strcmp(rpath, "-")) {
		flags |= KEXEC_FILE_NO_INITRAMFS;
	} else {
		rfd = open(rpath, O_RDONLY);
		if (rfd < 0) {
			perror(rpath);
			close(kfd);
			return 1;
		}
	}

	if (argc - optind == 3) {
		cmdline = argv[optind + 2];
	} else {
		if (read_proc_cmdline(proc_cmd, sizeof(proc_cmd)) < 0) {
			perror("/proc/cmdline");
			if (rfd >= 0)
				close(rfd);
			close(kfd);
			return 1;
		}
		cmdline = proc_cmd;
	}

	clen = strlen(cmdline);
	if (clen > 0)
		clen++; /* include NUL byte required by kexec_file_load */
	else
		cmdline = NULL;

	if (syscall(SYS_kexec_file_load, kfd, rfd, clen, cmdline, flags) < 0) {
		perror("kexec_file_load");
		if (rfd >= 0)
			close(rfd);
		close(kfd);
		return 1;
	}

	if (rfd >= 0)
		close(rfd);
	close(kfd);

	if (load_only) {
		printf("kexec_file_load OK (load-only)\n");
		return 0;
	}

do_exec:
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	if (!skip_usb_unbind) {
		write_sysfs("/sys/kernel/config/usb_gadget/q2/UDC", "\n");
		write_sysfs("/sys/bus/platform/drivers/dwc3-qcom-legacy/unbind",
			    "a6f8800.usb");
		usleep(100000);
	}
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_KEXEC, NULL);
	perror("reboot(KEXEC)");
	return 1;
}
