#define LOG_TAG "elliptic_notify_audio_hal_hidl"

#include <android/hardware/audio/7.0/IDevice.h>
#include <android/hardware/audio/7.0/IDevicesFactory.h>
#include <android/hardware/audio/7.0/types.h>
#include <log/log.h>
#include <string>
#include <vector>

using android::sp;
using android::hardware::hidl_string;
using android::hardware::hidl_vec;
using android::hardware::audio::V7_0::IDevice;
using android::hardware::audio::V7_0::IDevicesFactory;
using android::hardware::audio::V7_0::ParameterValue;
using android::hardware::audio::V7_0::Result;

extern "C" int elliptic_notify_audio_hal(const char* param) {
    if (!param) return 1;

    std::string input(param);
    size_t pos = input.find('=');
    if (pos == std::string::npos || pos == 0) {
        ALOGE("Invalid input string: %s\n", param);
        return 1;
    }

    std::string key = input.substr(0, pos);
    std::string value = input.substr(pos + 1);

    ALOGD("setting value: %s\n", param);

    sp<IDevicesFactory> devicesFactory = IDevicesFactory::getService();
    if (devicesFactory == nullptr) {
        ALOGE("devicesFactory is nullptr\n");
        return 1;
    }

    sp<IDevice> audioDevice = nullptr;
    Result openResult = Result::NOT_INITIALIZED;

    auto hidlReturn =
            devicesFactory->openDevice("primary", [&](Result retval, const sp<IDevice>& result) {
                openResult = retval;
                audioDevice = result;
            });

    if (!hidlReturn.isOk() || openResult != Result::OK) {
        if (!hidlReturn.isOk()) {
            ALOGE("openDevice failed: %s", hidlReturn.description().c_str());
        } else {
            ALOGE("openDevice failed\n");
        }
        return 1;
    }

    if (audioDevice == nullptr) {
        ALOGE("device is nullptr\n");
        return 1;
    }

    hidl_vec<ParameterValue> parameters;
    parameters.resize(1);
    parameters[0].key = key;
    parameters[0].value = value;

    Result setParamResult = audioDevice->setParameters({}, parameters);

    if (setParamResult != Result::OK) {
        ALOGE("setParameters failed\n");
        return 1;
    }

    ALOGD("successfully set value: %s\n", param);
    return 0;
}

extern "C" int elliptic_ultrasound_supported(void) {
    return 1;
}
