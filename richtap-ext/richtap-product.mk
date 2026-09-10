LOCAL_PATH := $(call my-dir)

#PRODUCT_PACKAGES += \
#        vendor.aac.hardware.richtap.vibrator

BOARD_VENDOR_SEPOLICY_DIRS += vendor/qcom/opensource/vibrator/richtap-ext/sepolicy

# aac richtap config path
# PRODUCT_PROPERTY_OVERRIDES += \
#     vendor.aac_richtap.config.path=/odm/etc/aac_richtap.config \
#     vendor.aac_richtap.device.path=/dev/aw8697_haptic

# PRODUCT_COPY_FILES += \
#     $(LOCAL_PATH)/aac_richtap_config/$(PRODUCT_DEVICE)/aac_richtap_1010.config:$(TARGET_COPY_OUT_ODM)/etc/aac_richtap.config
