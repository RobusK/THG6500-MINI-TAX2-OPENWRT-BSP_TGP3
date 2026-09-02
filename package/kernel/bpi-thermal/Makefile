#
# bpi-thermal: TR6560 PID fan controller.
#
# The original source was lost with the build volume; this packages the proven
# prebuilt module (v2.0) recovered from the running device. It is a hwmon
# device (pwm1/pwm1_enable/temp1_input) whose control thread runs a PID loop
# (params kp_x100, ki_x100, target_temp_c, poll_ms, filter_alpha, min_change),
# reading temperature via hcall_tri_kernel_hal_sensor_temp_get and driving the
# fan PWM via hcall_tri_dfx_pwm_gpio_*. Both HAL symbols are resolved at load
# time with kallsyms_lookup_name, so the module has no build-time dependency.
# It loads on any kernel with a matching vermagic (5.10.138, same .config).
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bpi-thermal
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bpi-thermal
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Other modules
  TITLE:=TR6560 PID fan controller (prebuilt v2.0)
  FILES:=$(PKG_BUILD_DIR)/bpi_thermal.ko
  AUTOLOAD:=$(call AutoLoad,95,bpi_thermal)
  KCONFIG:=CONFIG_HWMON=y
endef

define KernelPackage/bpi-thermal/description
  Prebuilt PID fan controller for the TR6560. Exposes a hwmon device
  (target 60C, Kp 4.00, Ki 0.30, 3s poll by default) and drives the fan PWM.
  Recovered binary; rebuild from source is not available.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files/bpi_thermal.ko $(PKG_BUILD_DIR)/bpi_thermal.ko
endef

define Build/Compile
endef

$(eval $(call KernelPackage,bpi-thermal))
