obj-m += shdwstck_runtime.o shdwstck_debug.o

shdwstck_runtime-objs := main.o \
                            core/api.o \
                            core/device.o \
                            core/ioctl.o \
							core/pt_bindings.o

shdwstck_debug-objs := core/debug.o

ccflags-y += -I$(src)/include
CFLAGS_main.o += -DDEBUG