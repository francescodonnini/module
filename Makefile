SRCS      := $(wildcard *.c core/*.c core/*.h include/*.h)
PWD       := $(CURDIR)
BUILD_DIR := $(PWD)/build
KDIR      := /lib/modules/$(shell uname -r)/build
KREL      := $(firstword $(subst -, ,$(shell uname -r)))
HAS_MO    := $(shell printf '6.13\n%s\n' '$(KREL)' | sort -V -C && echo y)

ifeq ($(HAS_MO),y)
	KBUILD := $(MAKE) -C $(KDIR) M="$(PWD)" MO="$(BUILD_DIR)"
	SYNC   := @mkdir -p "$(BUILD_DIR)"
	COMPDB := cp "$(BUILD_DIR)/compile_commands.json" "$(PWD)/"
else
	KBUILD := $(MAKE) -C $(KDIR) M="$(BUILD_DIR)"
	SYNC   := @mkdir -p "$(BUILD_DIR)" && rsync -a --exclude=build/ "$(PWD)/" "$(BUILD_DIR)/"
	COMPDB := sed 's|$(BUILD_DIR)|$(PWD)|g' "$(BUILD_DIR)/compile_commands.json" > "$(PWD)/compile_commands.json"
endif

.PHONY: all compdb checkpatch format-check mount umount clean

all:
	$(SYNC)
	$(KBUILD) modules

checkpatch:
	@test -n "$(SRCS)" || { echo "SRCS is empty"; exit 1; }
	$(KDIR)/scripts/checkpatch.pl --ignore LINUX_VERSION_CODE --no-tree -f $(SRCS)

compdb: all
	$(KBUILD) compile_commands.json
	$(COMPDB)

format-check:
	clang-format --dry-run --Werror $(SRCS)

mount:
	@insmod "$(BUILD_DIR)/shadowstack_runtime.ko"

umount:
	@rmmod shadowstack_runtime

clean:
	rm -rf "$(BUILD_DIR)"