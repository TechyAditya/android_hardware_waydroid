#ifndef HIDL_GENERATED_ANDROID_HARDWARE_GRAPHICS_COMPOSER_V2_1_TYPES_H
#define HIDL_GENERATED_ANDROID_HARDWARE_GRAPHICS_COMPOSER_V2_1_TYPES_H

#include <hidl/HidlSupport.h>
#include <hidl/MQDescriptor.h>
#include <utils/NativeHandle.h>
#include <utils/misc.h>

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_1 {

enum class Error : int32_t {
    NONE = 0,
    BAD_CONFIG = 1,
    BAD_DISPLAY = 2,
    BAD_LAYER = 3,
    BAD_PARAMETER = 4,
    NO_RESOURCES = 6,
    NOT_VALIDATED = 7,
    UNSUPPORTED = 8,
};

}  // namespace V2_1
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android

#endif  // HIDL_GENERATED_ANDROID_HARDWARE_GRAPHICS_COMPOSER_V2_1_TYPES_H
