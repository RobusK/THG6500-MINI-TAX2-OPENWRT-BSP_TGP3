#
# Recreated after the original package source was lost (see build-output/bpi-packages).
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bpi-temp
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bpi-temp
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Other modules
  TITLE:=TR6560 SoC temperature readout (/proc/bpi_temp)
  FILES:=$(PKG_BUILD_DIR)/bpi_temp.ko
  AUTOLOAD:=$(call AutoLoad,95,bpi_temp)
endef

define KernelPackage/bpi-temp/description
  Exposes the TR6560 on-die sensor temperature at /proc/bpi_temp by resolving
  the vendor HAL symbol hcall_tri_kernel_hal_sensor_temp_get at load time.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,bpi-temp))
