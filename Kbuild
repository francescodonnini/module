obj-m += shadowstack_runtime.o

shadowstack_runtime-objs := main.o \
                            core/chrdev.o \
							core/chrdev_fops.o \
                            core/chrdev_ioctl.o \
							core/debug.o \
                            core/api.o \
							core/pt_bindings.o

ccflags-y += -I$(src)/include
CFLAGS_main.o += -DDEBUG