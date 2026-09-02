#
# bpi-gpio: TR6560 GPIO / LED-PWM helper.
#
# The original source was lost with the build volume; this packages the proven
# prebuilt module recovered from the running device. It exposes /proc/bpi_gpio,
# /proc/bpi_gpio_mode and /proc/bpi_pwm (LED brightness/wink), driving the
# vendor GPIO/PWM block via hcall_tri_dfx_pwm_gpio_attr_* resolved at load time.
# Loads on any kernel with a matching vermagic (5.10.138, same .config).
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bpi-gpio
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bpi-gpio
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Other modules
  TITLE:=TR6560 GPIO/LED-PWM helper (prebuilt)
  FILES:=$(PKG_BUILD_DIR)/bpi_gpio.ko
  AUTOLOAD:=$(call AutoLoad,95,bpi_gpio)
endef

define KernelPackage/bpi-gpio/description
  Prebuilt GPIO/LED-PWM helper for the TR6560. Provides /proc/bpi_gpio,
  /proc/bpi_gpio_mode and /proc/bpi_pwm. Recovered binary; rebuild from
  source is not available.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files/bpi_gpio.ko $(PKG_BUILD_DIR)/bpi_gpio.ko
endef

define Build/Compile
endef

$(eval $(call KernelPackage,bpi-gpio))
