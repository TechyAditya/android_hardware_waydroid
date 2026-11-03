#ifndef HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_1_IWAYDROIDDISPLAY_H
#define HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_1_IWAYDROIDDISPLAY_H

#include <android/hardware/graphics/composer/2.1/types.h>
#include <vendor/waydroid/display/1.0/IWaydroidDisplay.h>
#include <android/hidl/base/1.0/IBase.h>
#include <android/hidl/manager/1.0/IServiceNotification.h>
#include <hidl/HidlSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>
#include <utils/NativeHandle.h>
#include <utils/misc.h>

namespace vendor {
namespace waydroid {
namespace display {
namespace V1_1 {

struct IWaydroidDisplay : public ::vendor::waydroid::display::V1_0::IWaydroidDisplay {
    typedef ::android::hardware::details::i_tag _hidl_tag;
    static const char* descriptor;
    virtual bool isRemote() const override { return false; }
    virtual ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerName(uint32_t layer, const ::android::hardware::hidl_string& name) = 0;
    virtual ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) = 0;
    virtual ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setTargetLayerHandleInfo(uint32_t format, uint32_t stride) = 0;
    virtual ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerSize(uint32_t layer, uint32_t width, uint32_t height) = 0;
    virtual ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setTargetLayerSize(uint32_t width, uint32_t height) = 0;
    using interfaceChain_cb = std::function<void(const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& descriptors)>;
    virtual ::android::hardware::Return<void> interfaceChain(interfaceChain_cb _hidl_cb) override;
    virtual ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options) override;
    using interfaceDescriptor_cb = std::function<void(const ::android::hardware::hidl_string& descriptor)>;
    virtual ::android::hardware::Return<void> interfaceDescriptor(interfaceDescriptor_cb _hidl_cb) override;
    using getHashChain_cb = std::function<void(const ::android::hardware::hidl_vec<::android::hardware::hidl_array<uint8_t, 32>>& hashchain)>;
    virtual ::android::hardware::Return<void> getHashChain(getHashChain_cb _hidl_cb) override;
    virtual ::android::hardware::Return<void> setHALInstrumentation() override;
    virtual ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie) override;
    virtual ::android::hardware::Return<void> ping() override;
    using getDebugInfo_cb = std::function<void(const ::android::hidl::base::V1_0::DebugInfo& info)>;
    virtual ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb) override;
    virtual ::android::hardware::Return<void> notifySyspropsChanged() override;
    virtual ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) override;
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::display::V1_1::IWaydroidDisplay>> castFrom(const ::android::sp<::vendor::waydroid::display::V1_1::IWaydroidDisplay>& parent, bool emitError = false);
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::display::V1_1::IWaydroidDisplay>> castFrom(const ::android::sp<::vendor::waydroid::display::V1_0::IWaydroidDisplay>& parent, bool emitError = false);
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::display::V1_1::IWaydroidDisplay>> castFrom(const ::android::sp<::android::hidl::base::V1_0::IBase>& parent, bool emitError = false);
    static ::android::sp<IWaydroidDisplay> tryGetService(const std::string &serviceName="default", bool getStub=false);
    static ::android::sp<IWaydroidDisplay> tryGetService(const char serviceName[], bool getStub=false)  { std::string str(serviceName ? serviceName : "");      return tryGetService(str, getStub); }
    static ::android::sp<IWaydroidDisplay> tryGetService(const ::android::hardware::hidl_string& serviceName, bool getStub=false)  { std::string str(serviceName.c_str());      return tryGetService(str, getStub); }
    static ::android::sp<IWaydroidDisplay> tryGetService(bool getStub) { return tryGetService("default", getStub); }
    static ::android::sp<IWaydroidDisplay> getService(const std::string &serviceName="default", bool getStub=false);
    static ::android::sp<IWaydroidDisplay> getService(const char serviceName[], bool getStub=false)  { std::string str(serviceName ? serviceName : "");      return getService(str, getStub); }
    static ::android::sp<IWaydroidDisplay> getService(const ::android::hardware::hidl_string& serviceName, bool getStub=false)  { std::string str(serviceName.c_str());      return getService(str, getStub); }
    static ::android::sp<IWaydroidDisplay> getService(bool getStub) { return getService("default", getStub); }
    __attribute__ ((warn_unused_result))::android::status_t registerAsService(const std::string &serviceName="default");
    static bool registerForNotifications(
            const std::string &serviceName,
            const ::android::sp<::android::hidl::manager::V1_0::IServiceNotification> &notification);
};

}  // namespace V1_1
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_1_IWAYDROIDDISPLAY_H
