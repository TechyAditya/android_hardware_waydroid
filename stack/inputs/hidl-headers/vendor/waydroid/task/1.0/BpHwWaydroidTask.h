#ifndef HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BPHWWAYDROIDTASK_H
#define HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BPHWWAYDROIDTASK_H

#include <hidl/HidlTransportSupport.h>

#include <vendor/waydroid/task/1.0/IHwWaydroidTask.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_0 {

struct BpHwWaydroidTask : public ::android::hardware::BpInterface<IWaydroidTask>, public ::android::hardware::details::HidlInstrumentor {
    explicit BpHwWaydroidTask(const ::android::sp<::android::hardware::IBinder> &_hidl_impl);

    /**
     * The pure class is what this class wraps.
     */
    typedef IWaydroidTask Pure;

    /**
     * Type tag for use in template logic that indicates this is a 'proxy' class.
     */
    typedef android::hardware::details::bphw_tag _hidl_tag;

    virtual bool isRemote() const override { return true; }

    // Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
    static ::android::hardware::Return<void>  _hidl_setFocusedTask(::android::hardware::IInterface* _hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor, uint32_t taskID);
    static ::android::hardware::Return<void>  _hidl_removeTask(::android::hardware::IInterface* _hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor, uint32_t taskID);
    static ::android::hardware::Return<void>  _hidl_removeAllVisibleRecentTasks(::android::hardware::IInterface* _hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor);
    static ::android::hardware::Return<void>  _hidl_getAppName(::android::hardware::IInterface* _hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor, const ::android::hardware::hidl_string& packageName, getAppName_cb _hidl_cb);

    // Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
    ::android::hardware::Return<void> setFocusedTask(uint32_t taskID) override;
    ::android::hardware::Return<void> removeTask(uint32_t taskID) override;
    ::android::hardware::Return<void> removeAllVisibleRecentTasks() override;
    ::android::hardware::Return<void> getAppName(const ::android::hardware::hidl_string& packageName, getAppName_cb _hidl_cb) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.
    ::android::hardware::Return<void> interfaceChain(interfaceChain_cb _hidl_cb) override;
    ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options) override;
    ::android::hardware::Return<void> interfaceDescriptor(interfaceDescriptor_cb _hidl_cb) override;
    ::android::hardware::Return<void> getHashChain(getHashChain_cb _hidl_cb) override;
    ::android::hardware::Return<void> setHALInstrumentation() override;
    ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie) override;
    ::android::hardware::Return<void> ping() override;
    ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb) override;
    ::android::hardware::Return<void> notifySyspropsChanged() override;
    ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) override;

private:
    std::mutex _hidl_mMutex;
    std::vector<::android::sp<::android::hardware::hidl_binder_death_recipient>> _hidl_mDeathRecipients;
};

}  // namespace V1_0
}  // namespace task
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BPHWWAYDROIDTASK_H
