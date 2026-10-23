# Out-of-tree build against the locally prepared kernel tree.
KDIR ?= /home/jiangshihao/kernel-camellia-t-out
CLANG_DIR ?= /home/jiangshihao/toolchains/clang-r383902/bin

export PATH := $(CLANG_DIR):$(PATH)

obj-m += uefi_jump.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 \
		CC=$(CLANG_DIR)/clang \
		LD=$(CLANG_DIR)/ld.lld \
		AR=$(CLANG_DIR)/llvm-ar \
		NM=$(CLANG_DIR)/llvm-nm \
		OBJCOPY=$(CLANG_DIR)/llvm-objcopy \
		STRIP=$(CLANG_DIR)/llvm-strip \
		CLANG_TRIPLE=aarch64-linux-gnu- \
		CROSS_COMPILE=aarch64-linux-gnu- \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
