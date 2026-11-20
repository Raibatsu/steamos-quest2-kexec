obj-m += qkx_backends.o
qkx_backends-objs := module_main.o shutdown.o shutdown_io.o \
	backend_gpu.o backend_ufs.o backend_usb.o backend_camera.o \
	backend_aop.o backend_adsp.o backend_syncboss.o backend_tz.o \
	backend_smmu.o backend_video.o

KDIR ?= /mnt/HDD/quest2-kexec-build/android-out
SRCDIR ?= /mnt/HDD/quest2-kexec-build/kernel-src
PWD := $(shell pwd)

default:
	$(MAKE) -C $(SRCDIR) O=$(KDIR) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
		CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm \
		OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
		M=$(PWD) modules

clean:
	$(MAKE) -C $(SRCDIR) O=$(KDIR) ARCH=arm64 M=$(PWD) clean
