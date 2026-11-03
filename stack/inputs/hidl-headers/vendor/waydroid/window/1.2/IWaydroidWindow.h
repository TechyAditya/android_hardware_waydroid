#ifndef HIDL_GENERATED_VENDOR_WAYDROID_WINDOW_V1_2_IWAYDROIDWINDOW_H
#define HIDL_GENERATED_VENDOR_WAYDROID_WINDOW_V1_2_IWAYDROIDWINDOW_H

#include <vendor/waydroid/window/1.1/IWaydroidWindow.h>
#include <android/hidl/base/1.0/IBase.h>
#include <android/hidl/manager/1.0/IServiceNotification.h>
#include <hidl/HidlSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>
#include <utils/NativeHandle.h>
#include <utils/misc.h>

namespace vendor {
namespace waydroid {
namespace window {
namespace V1_2 {

struct IWaydroidWindow : public ::vendor::waydroid::window::V1_1::IWaydroidWindow {
    typedef ::android::hardware::details::i_tag _hidl_tag;
    static const char* descriptor;
    virtual bool isRemote() const override { return false; }
    virtual ::android::hardware::Return<bool> minimize(const ::android::hardware::hidl_string& packageName) = 0;
    virtual ::android::hardware::Return<void> setPointerCapture(const ::android::hardware::hidl_string& packageName, bool enabled) = 0;
    virtual ::android::hardware::Return<void> setIdleInhibit(const ::android::hardware::hidl_string& taskID, bool enabled) = 0;
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
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::window::V1_2::IWaydroidWindow>> castFrom(const ::android::sp<::vendor::waydroid::window::V1_2::IWaydroidWindow>& parent, bool emitError = false);
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::window::V1_2::IWaydroidWindow>> castFrom(const ::android::sp<::vendor::waydroid::window::V1_1::IWaydroidWindow>& parent, bool emitError = false);
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::window::V1_2::IWaydroidWindow>> castFrom(const ::android::sp<::vendor::waydroid::window::V1_0::IWaydroidWindow>& parent, bool emitError = false);
    static ::android::hardware::Return<::android::sp<::vendor::waydroid::window::V1_2::IWaydroidWindow>> castFrom(const ::android::sp<::android::hidl::base::V1_0::IBase>& parent, bool emitError = false);
    static ::android::sp<IWaydroidWindow> tryGetService(const std::string &serviceName="default", bool getStub=false);
    static ::android::sp<IWaydroidWindow> tryGetService(const char serviceName[], bool getStub=false)  { std::string str(serviceName ? serviceName : "");      return tryGetService(str, getStub); }
    static ::android::sp<IWaydroidWindow> tryGetService(const ::android::hardware::hidl_string& serviceName, bool getStub=false)  { std::string str(serviceName.c_str());      return tryGetService(str, getStub); }
    static ::android::sp<IWaydroidWindow> tryGetService(bool getStub) { return tryGetService("default", getStub); }
    static ::android::sp<IWaydroidWindow> getService(const std::string &serviceName="default", bool getStub=false);
    static ::android::sp<IWaydroidWindow> getService(const char serviceName[], bool getStub=false)  { std::string str(serviceName ? serviceName : "");      return getService(str, getStub); }
    static ::android::sp<IWaydroidWindow> getService(const ::android::hardware::hidl_string& serviceName, bool getStub=false)  { std::string str(serviceName.c_str());      return getService(str, getStub); }
    static ::android::sp<IWaydroidWindow> getService(bool getStub) { return getService("default", getStub); }
    __attribute__ ((warn_unused_result))::android::status_t registerAsService(const std::string &serviceName="default");
    static bool registerForNotifications(
            const std::string &serviceName,
            const ::android::sp<::android::hidl::manager::V1_0::IServiceNotification> &notification);
};

}  // namespace V1_2
}  // namespace window
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_WINDOW_V1_2_IWAYDROIDWINDOW_H
