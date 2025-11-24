#
# Copyright (C) 2022 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

PRODUCT_PACKAGES += \
    MssiFrameworkOverlay \
	MssiNetworkStackOverlay \
	MssiWifiOverlay

ifeq ($(ENABLE_VENDOR_RIL_SERVICE), true)
PRODUCT_PACKAGES += \
    MssiFrameworkTelephonyOverlay \
	MssiTelephonyOverlay
endif

ifeq ($(WIFI_FEATURE_HOSTAPD_11AX), true)
PRODUCT_PACKAGES += \
    MssiWifiMultiStaOverlay

ifeq (,$(filter $(TARGET_BOARD_PLATFORM),mt6779 mt6833 mt6853 mt6873 mt6875 mt6877 mt6883 mt6885 mt6889 mt6891))
PRODUCT_PACKAGES += \
    MssiWifi6gOverlay
endif
endif
