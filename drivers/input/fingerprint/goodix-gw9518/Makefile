#
# Makefile for the fingerprint device.
#

# Each configuration option enables a list of files.
#include $(srctree)/drivers/misc/mediatek/Makefile.custom

# for microtrust tee start
ifneq ($(filter y m, $(CONFIG_MICROTRUST_TEE_SUPPORT)),)
ccflags-y += -I$(srctree)/drivers/spi/mediatek/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -I$(srctree)/drivers/spi/mediatek/$(MTK_PLATFORM)

include $(DEVICE_MODULES_PATH)/drivers/tee/teei/Makefile.include
endif
# for microtrust tee end

obj-$(CONFIG_GOODIX_GW9518_FINGERPRINT)	+= gf_tee.o
#gf_tee-y := gf_spi_tee.o gf_spi_factory.o
gf_tee-y := gf_spi_tee.o
