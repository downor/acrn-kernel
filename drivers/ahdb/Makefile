obj-y := ahdb_buflist.o \

# for ACRN Service OS
ifeq ($(CONFIG_AHDB_BE), y)
	obj-y += ahdb_sos.o \
		 ahdb_sos_comm.o \
		 ahdb_dmabuf_ops.o
# for ACRN Guest OS
else
	obj-y += ahdb_uos.o \
		 ahdb_uos_comm.o \
		 ahdb_hid.o
endif
