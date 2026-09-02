#
# Recreated from the surviving bpi_voltage.c (v1.2) after the original package
# source was lost (see build-output/bpi-packages).
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bpi-voltage
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bpi-voltage
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Other modules
  TITLE:=TR6560 CPU voltage offset control (/proc/bpi_voltage)
  FILES:=$(PKG_BUILD_DIR)/bpi_voltage.ko
  AUTOLOAD:=$(call AutoLoad,96,bpi_voltage)
endef

define KernelPackage/bpi-voltage/description
  Undervolt/overvolt the TR6560 CPU by offsetting the PWM duty cycle that sets
  the core voltage. Write an integer offset to /proc/bpi_voltage (+N undervolts,
  -N overvolts); a timer re-applies the offset when cpufreq changes the duty.
  Default offset is 0 (no change).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,bpi-voltage))
