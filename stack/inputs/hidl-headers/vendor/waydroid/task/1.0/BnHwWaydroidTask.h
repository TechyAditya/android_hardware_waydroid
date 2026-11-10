#ifndef HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BNHWWAYDROIDTASK_H
#define HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BNHWWAYDROIDTASK_H

#include <vendor/waydroid/task/1.0/IHwWaydroidTask.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_0 {

struct BnHwWaydroidTask : public ::android::hidl::base::V1_0::BnHwBase {
    explicit BnHwWaydroidTask(const ::android::sp<IWaydroidTask> &_hidl_impl);
    explicit BnHwWaydroidTask(const ::android::sp<IWaydroidTask> &_hidl_impl, const std::string& HidlInstrumentor_package, const std::string& HidlInstrumentor_interface);

    virtual ~BnHwWaydroidTask();

    ::android::status_t onTransact(
            uint32_t _hidl_code,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            uint32_t _hidl_flags = 0,
            TransactCallback _hidl_cb = nullptr) override;


    /**
     * The pure class is what this class wraps.
     */
    typedef IWaydroidTask Pure;

    /**
     * Type tag for use in template logic that indicates this is a 'native' class.
     */
    typedef android::hardware::details::bnhw_tag _hidl_tag;

    ::android::sp<IWaydroidTask> getImpl() { return _hidl_mImpl; }
    // Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
    static ::android::status_t _hidl_setFocusedTask(
            ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            TransactCallback _hidl_cb);


    static ::android::status_t _hidl_removeTask(
            ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            TransactCallback _hidl_cb);


    static ::android::status_t _hidl_removeAllVisibleRecentTasks(
            ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            TransactCallback _hidl_cb);


    static ::android::status_t _hidl_getAppName(
            ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            TransactCallback _hidl_cb);



private:
    // Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.

    // Methods from ::android::hidl::base::V1_0::IBase follow.
    ::android::hardware::Return<void> ping();
    using getDebugInfo_cb = ::android::hidl::base::V1_0::IBase::getDebugInfo_cb;
    ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb);

    ::android::sp<IWaydroidTask> _hidl_mImpl;
};

}  // namespace V1_0
}  // namespace task
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_TASK_V1_0_BNHWWAYDROIDTASK_H
