obj-m += shadowstack_runtime.o

shadowstack_runtime-objs := main.o \
                            core/api.o \
							core/debug.o \
                            core/device.o \
                            core/ioctl.o \
							core/pt_bindings.o

ccflags-y += -I$(src)/include
CFLAGS_main.o += -DDEBUG