#
# bpi-fan: quiet fan controller for the BPI-Wifi6-mini (replaces bpi-thermal).
# Details in the package description below and in src/fan_main.c.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bpi-fan
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bpi-fan
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Other modules
  TITLE:=BPI-Wifi6-mini quiet fan controller (FIQ/hrtimer PWM)
  FILES:=$(PKG_BUILD_DIR)/bpi_fan.ko
  AUTOLOAD:=$(call AutoLoad,95,bpi_fan)
  CONFLICTS:=kmod-bpi-thermal
endef

define KernelPackage/bpi-fan/description
  PI fan controller for the TR6560 BPI-Wifi6-mini. Drives the fan PWM input
  on GPIO19 with a software PWM (default 6 kHz; SP804 timer FIQ, or an
  hrtimer fallback) instead of the LED blink engine used by bpi-thermal,
  which makes the fan crackle. Exposes a
  hwmon device (temp1_input, pwm1, pwm1_enable); tunables are module
  parameters (target_temp_c, off_hyst_c, min_duty, kp, ki, slew, ...).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,bpi-fan))
