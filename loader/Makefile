obj-m += quest_kexec.o
quest_kexec-objs := loader.o transition.o shutdown.o shutdown_io.o
ccflags-y += -DQKX_ENABLE_EXPERIMENTAL_JUMP

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
