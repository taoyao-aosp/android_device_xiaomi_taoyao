/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.taoyao"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <poll.h>
#include <fstream>
#include <mutex>
#include <thread>
#include <sys/ioctl.h>

#include <linux/xiaomi_touch.h>
#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

// Touchscreen and HBM
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

template <typename T>
static bool set(const std::string& path, const T& value) {
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG(WARNING) << "sysfs open failed: " << path;
        return false;
    }
    file << value;
    if (!file) {
        LOG(WARNING) << "sysfs write failed: " << path;
        return false;
    }
    return true;
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
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        int bufAod[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Aod_Enable, FOD_STATUS_ON};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &bufAod);

        // Thread to notify fingeprint hwmodule about fod presses
        std::thread([this]() {
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << FOD_PRESS_STATUS_PATH << " , err: " << fd;
                return;
            }

            struct pollfd fodPressStatusPoll = {
                .fd = fd,
                .events = POLLERR | POLLPRI,
                .revents = 0,
            };

            while (true) {
                int rc = poll(&fodPressStatusPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    continue;
                }

                bool pressed = readBool(fd);
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
            }
        }).detach();
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        if(mAuthSuccess) return;
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
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
            setFingerDown(false);
                break;
            default:
                break;
        }

        /* vendorCode
         * 21: waiting for finger
         * 22: finger down
         * 23: finger up
         */
        if (vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void onAuthenticationSucceeded() {
        mAuthSuccess = true;
        setFingerDown(false);
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            mAuthSuccess = false;
        }).detach();
    }

    void onAuthenticationFailed() { setFingerDown(false); }

    void cancel() {
        LOG(INFO) << __func__;
        setFingerDown(false);
    }

  private:
    fingerprint_device_t* mDevice;
    bool mAuthSuccess = false;

    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;

    void setFodStatus(int value) {
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }

    void setFingerDown(bool pressed) {
        mDevice->extCmd(mDevice, COMMAND_NIT, pressed ? PARAM_NIT_FOD : PARAM_NIT_NONE);

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, pressed ? FOD_STATUS_ON : FOD_STATUS_OFF};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);

        struct disp_feature_req req = {
            .base = displayBasePrimary,
            .feature_id = DISP_FEATURE_LOCAL_HBM,
            .feature_val = pressed ? LOCAL_HBM_NORMAL_WHITE_1000NIT : LOCAL_HBM_OFF_TO_NORMAL,
        };
        ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_FEATURE, &req);
    }
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
