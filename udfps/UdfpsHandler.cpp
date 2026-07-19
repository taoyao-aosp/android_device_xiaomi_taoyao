/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <poll.h>
#include <fstream>
#include <mutex>
#include <thread>

#include "UdfpsHandler.h"

#include <linux/xiaomi_touch.h>
#include <display/drm/mi_disp.h>

// Fingerprint hwmodule commands
#define COMMAND_NIT 10
#define PARAM_NIT_UDFPS 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

// Touchscreen and HBM
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define FOD_STATUS_PATH "/sys/devices/platform/goodix_ts.0/fod_enable"

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

struct disp_base displayBasePrimary = {
    .flag = 0,
    .disp_id = MI_DISP_PRIMARY,
};

class XiaomiUdfpsHandler : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) {
        mDevice = device;
        dispFeatureFd = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        touchUniqueFd = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        if (mAuthSuccess) return;

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, PARAM_FOD_PRESSED};
        ioctl(touchUniqueFd.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);

        mDevice->extCmd(mDevice, COMMAND_NIT, PARAM_NIT_UDFPS);

        struct disp_feature_req req = {
            .base = displayBasePrimary,
            .feature_id = DISP_FEATURE_LOCAL_HBM,
            .feature_val = LOCAL_HBM_NORMAL_WHITE_1000NIT,
        };
        ioctl(dispFeatureFd.get(), MI_DISP_IOCTL_SET_FEATURE, &req);
        
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
    }

    void onFingerUp() {
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, PARAM_FOD_RELEASED};
        ioctl(touchUniqueFd.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);

        mDevice->extCmd(mDevice, COMMAND_NIT, PARAM_NIT_NONE);

        struct disp_feature_req req = {
            .base = displayBasePrimary,
            .feature_id = DISP_FEATURE_LOCAL_HBM,
            .feature_val = LOCAL_HBM_OFF_TO_NORMAL,
        };
        ioctl(dispFeatureFd.get(), MI_DISP_IOCTL_SET_FEATURE, &req);

        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
    }

    void onAcquired(int32_t result, int32_t /*vendorCode*/) {
        switch (static_cast<AcquiredInfo>(result)) {
            case AcquiredInfo::GOOD:
            case AcquiredInfo::PARTIAL:
            case AcquiredInfo::INSUFFICIENT:
            case AcquiredInfo::SENSOR_DIRTY:
            case AcquiredInfo::TOO_SLOW:
            case AcquiredInfo::TOO_FAST:
            case AcquiredInfo::TOO_DARK:
            case AcquiredInfo::TOO_BRIGHT:
            case AcquiredInfo::IMMOBILE:
            case AcquiredInfo::LIFT_TOO_SOON:
                onFingerUp();
                break;
            default:
                break;
        }
    }

    void onAuthenticationSucceeded() {
        mAuthSuccess = true;
        onFingerUp();
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            mAuthSuccess = false;
        }).detach();
    }

    void onAuthenticationFailed() { onFingerUp(); }

  private:
    fingerprint_device_t* mDevice;
    bool mAuthSuccess = false;
    android::base::unique_fd dispFeatureFd;
    android::base::unique_fd touchUniqueFd;
};

static UdfpsHandler* create() {
    return new XiaomiUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
