#define LOG_TAG "vendor.waydroid.task@1.1::WaydroidTask"

#include <log/log.h>
#include <cutils/trace.h>
#include <hidl/HidlTransportSupport.h>

#include <hidl/Static.h>
#include <hwbinder/ProcessState.h>
#include <utils/Trace.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <vendor/waydroid/task/1.1/BpHwWaydroidTask.h>
#include <vendor/waydroid/task/1.1/BnHwWaydroidTask.h>
#include <vendor/waydroid/task/1.1/BsWaydroidTask.h>
#include <vendor/waydroid/task/1.0/BpHwWaydroidTask.h>
#include <android/hidl/base/1.0/BpHwBase.h>
#include <hidl/ServiceManagement.h>

namespace vendor {
namespace waydroid {
namespace task {
namespace V1_1 {

const char* IWaydroidTask::descriptor("vendor.waydroid.task@1.1::IWaydroidTask");

__attribute__((constructor)) static void static_constructor() {
    ::android::hardware::details::getBnConstructorMap().set(IWaydroidTask::descriptor,
            [](void *iIntf) -> ::android::sp<::android::hardware::IBinder> {
                return new BnHwWaydroidTask(static_cast<IWaydroidTask *>(iIntf));
            });
    ::android::hardware::details::getBsConstructorMap().set(IWaydroidTask::descriptor,
            [](void *iIntf) -> ::android::sp<::android::hidl::base::V1_0::IBase> {
                return new BsWaydroidTask(static_cast<IWaydroidTask *>(iIntf));
            });
};

__attribute__((destructor))static void static_destructor() {
    ::android::hardware::details::getBnConstructorMap().erase(IWaydroidTask::descriptor);
    ::android::hardware::details::getBsConstructorMap().erase(IWaydroidTask::descriptor);
};

// Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
// no default implementation for: ::android::hardware::Return<void> IWaydroidTask::setFocusedTask(uint32_t taskID)
// no default implementation for: ::android::hardware::Return<void> IWaydroidTask::removeTask(uint32_t taskID)
// no default implementation for: ::android::hardware::Return<void> IWaydroidTask::removeAllVisibleRecentTasks()
// no default implementation for: ::android::hardware::Return<void> IWaydroidTask::getAppName(const ::android::hardware::hidl_string& packageName, getAppName_cb _hidl_cb)

// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.
// no default implementation for: ::android::hardware::Return<void> IWaydroidTask::setRefreshRate(uint32_t millihertz)

// Methods from ::android::hidl::base::V1_0::IBase follow.
::android::hardware::Return<void> IWaydroidTask::interfaceChain(interfaceChain_cb _hidl_cb){
    _hidl_cb({
        ::vendor::waydroid::task::V1_1::IWaydroidTask::descriptor,
        ::vendor::waydroid::task::V1_0::IWaydroidTask::descriptor,
        ::android::hidl::base::V1_0::IBase::descriptor,
    });
    return ::android::hardware::Void();}

::android::hardware::Return<void> IWaydroidTask::debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options){
    (void)fd;
    (void)options;
    return ::android::hardware::Void();
}

::android::hardware::Return<void> IWaydroidTask::interfaceDescriptor(interfaceDescriptor_cb _hidl_cb){
    _hidl_cb(::vendor::waydroid::task::V1_1::IWaydroidTask::descriptor);
    return ::android::hardware::Void();
}

::android::hardware::Return<void> IWaydroidTask::getHashChain(getHashChain_cb _hidl_cb){
    _hidl_cb({
        (uint8_t[32]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} /* 0000000000000000000000000000000000000000000000000000000000000000 */,
        (uint8_t[32]){152,162,151,205,119,229,175,113,65,252,184,228,234,150,45,249,83,142,151,85,164,160,114,95,153,245,228,157,96,205,190,140} /* 98a297cd77e5af7141fcb8e4ea962df9538e9755a4a0725f99f5e49d60cdbe8c */,
        (uint8_t[32]){236,127,215,158,208,45,250,133,188,73,148,38,173,174,62,190,35,239,5,36,243,205,105,87,19,147,36,184,59,24,202,76} /* ec7fd79ed02dfa85bc499426adae3ebe23ef0524f3cd6957139324b83b18ca4c */});
    return ::android::hardware::Void();
}

::android::hardware::Return<void> IWaydroidTask::setHALInstrumentation(){
    return ::android::hardware::Void();
}

::android::hardware::Return<bool> IWaydroidTask::linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie){
    (void)cookie;
    return (recipient != nullptr);
}

::android::hardware::Return<void> IWaydroidTask::ping(){
    return ::android::hardware::Void();
}

::android::hardware::Return<void> IWaydroidTask::getDebugInfo(getDebugInfo_cb _hidl_cb){
    ::android::hidl::base::V1_0::DebugInfo info = {};
    info.pid = -1;
    info.ptr = 0;
    info.arch = 
    #if defined(__LP64__)
    ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_64BIT
    #else
    ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_32BIT
    #endif
    ;
    _hidl_cb(info);
    return ::android::hardware::Void();
}

::android::hardware::Return<void> IWaydroidTask::notifySyspropsChanged(){
    ::android::report_sysprop_change();
    return ::android::hardware::Void();
}

::android::hardware::Return<bool> IWaydroidTask::unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient){
    return (recipient != nullptr);
}


::android::hardware::Return<::android::sp<::vendor::waydroid::task::V1_1::IWaydroidTask>> IWaydroidTask::castFrom(const ::android::sp<::vendor::waydroid::task::V1_1::IWaydroidTask>& parent, bool /* emitError */) {
    return parent;
}

::android::hardware::Return<::android::sp<::vendor::waydroid::task::V1_1::IWaydroidTask>> IWaydroidTask::castFrom(const ::android::sp<::vendor::waydroid::task::V1_0::IWaydroidTask>& parent, bool emitError) {
    return ::android::hardware::details::castInterface<IWaydroidTask, ::vendor::waydroid::task::V1_0::IWaydroidTask, BpHwWaydroidTask>(
            parent, "vendor.waydroid.task@1.1::IWaydroidTask", emitError);
}

::android::hardware::Return<::android::sp<::vendor::waydroid::task::V1_1::IWaydroidTask>> IWaydroidTask::castFrom(const ::android::sp<::android::hidl::base::V1_0::IBase>& parent, bool emitError) {
    return ::android::hardware::details::castInterface<IWaydroidTask, ::android::hidl::base::V1_0::IBase, BpHwWaydroidTask>(
            parent, "vendor.waydroid.task@1.1::IWaydroidTask", emitError);
}

BpHwWaydroidTask::BpHwWaydroidTask(const ::android::sp<::android::hardware::IBinder> &_hidl_impl)
        : BpInterface<IWaydroidTask>(_hidl_impl),
          ::android::hardware::details::HidlInstrumentor("vendor.waydroid.task@1.1", "IWaydroidTask") {
}

// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.
::android::hardware::Return<void> BpHwWaydroidTask::_hidl_setRefreshRate(::android::hardware::IInterface *_hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor, uint32_t millihertz) {
    #ifdef __ANDROID_DEBUGGABLE__
    bool mEnableInstrumentation = _hidl_this_instrumentor->isInstrumentationEnabled();
    const auto &mInstrumentationCallbacks = _hidl_this_instrumentor->getInstrumentationCallbacks();
    #else
    (void) _hidl_this_instrumentor;
    #endif // __ANDROID_DEBUGGABLE__
    ::android::ScopedTrace PASTE(___tracer, __LINE__) (ATRACE_TAG_HAL, "HIDL::IWaydroidTask::setRefreshRate::client");
    #ifdef __ANDROID_DEBUGGABLE__
    if (UNLIKELY(mEnableInstrumentation)) {
        std::vector<void *> _hidl_args;
        _hidl_args.push_back((void *)&millihertz);
        for (const auto &callback: mInstrumentationCallbacks) {
            callback(InstrumentationEvent::CLIENT_API_ENTRY, "vendor.waydroid.task", "1.1", "IWaydroidTask", "setRefreshRate", &_hidl_args);
        }
    }
    #endif // __ANDROID_DEBUGGABLE__

    bool isHostHwBinder = ::android::hardware::IInterface::isHostHwBinder(_hidl_this);
    ::android::hardware::Parcel _hidl_data(isHostHwBinder);
    ::android::hardware::Parcel _hidl_reply(isHostHwBinder);
    ::android::status_t _hidl_err;
    ::android::hardware::Status _hidl_status;

    _hidl_err = _hidl_data.writeInterfaceToken(BpHwWaydroidTask::descriptor);
    if (_hidl_err != ::android::OK) { goto _hidl_error; }

    _hidl_err = _hidl_data.writeUint32(millihertz);
    if (_hidl_err != ::android::OK) { goto _hidl_error; }

    _hidl_err = ::android::hardware::IInterface::asBinder(_hidl_this)->transact(5 /* setRefreshRate */, _hidl_data, &_hidl_reply, 1u /* oneway */);
    if (_hidl_err != ::android::OK) { goto _hidl_error; }

    #ifdef __ANDROID_DEBUGGABLE__
    if (UNLIKELY(mEnableInstrumentation)) {
        std::vector<void *> _hidl_args;
        for (const auto &callback: mInstrumentationCallbacks) {
            callback(InstrumentationEvent::CLIENT_API_EXIT, "vendor.waydroid.task", "1.1", "IWaydroidTask", "setRefreshRate", &_hidl_args);
        }
    }
    #endif // __ANDROID_DEBUGGABLE__

    _hidl_status.setFromStatusT(_hidl_err);
    return ::android::hardware::Return<void>();

_hidl_error:
    _hidl_status.setFromStatusT(_hidl_err);
    return ::android::hardware::Return<void>(_hidl_status);
}


// Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.
::android::hardware::Return<void> BpHwWaydroidTask::setFocusedTask(uint32_t taskID){
    ::android::hardware::Return<void>  _hidl_out = ::vendor::waydroid::task::V1_0::BpHwWaydroidTask::_hidl_setFocusedTask(this, this, taskID);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::removeTask(uint32_t taskID){
    ::android::hardware::Return<void>  _hidl_out = ::vendor::waydroid::task::V1_0::BpHwWaydroidTask::_hidl_removeTask(this, this, taskID);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::removeAllVisibleRecentTasks(){
    ::android::hardware::Return<void>  _hidl_out = ::vendor::waydroid::task::V1_0::BpHwWaydroidTask::_hidl_removeAllVisibleRecentTasks(this, this);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::getAppName(const ::android::hardware::hidl_string& packageName, getAppName_cb _hidl_cb){
    ::android::hardware::Return<void>  _hidl_out = ::vendor::waydroid::task::V1_0::BpHwWaydroidTask::_hidl_getAppName(this, this, packageName, _hidl_cb);

    return _hidl_out;
}


// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.
::android::hardware::Return<void> BpHwWaydroidTask::setRefreshRate(uint32_t millihertz){
    ::android::hardware::Return<void>  _hidl_out = ::vendor::waydroid::task::V1_1::BpHwWaydroidTask::_hidl_setRefreshRate(this, this, millihertz);

    return _hidl_out;
}


// Methods from ::android::hidl::base::V1_0::IBase follow.
::android::hardware::Return<void> BpHwWaydroidTask::interfaceChain(interfaceChain_cb _hidl_cb){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_interfaceChain(this, this, _hidl_cb);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_debug(this, this, fd, options);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::interfaceDescriptor(interfaceDescriptor_cb _hidl_cb){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_interfaceDescriptor(this, this, _hidl_cb);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::getHashChain(getHashChain_cb _hidl_cb){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_getHashChain(this, this, _hidl_cb);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::setHALInstrumentation(){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_setHALInstrumentation(this, this);

    return _hidl_out;
}

::android::hardware::Return<bool> BpHwWaydroidTask::linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie){
    ::android::hardware::ProcessState::self(remote()->isHostHwBinder())->startThreadPool();
    ::android::hardware::hidl_binder_death_recipient *binder_recipient = new ::android::hardware::hidl_binder_death_recipient(recipient, cookie, this);
    std::unique_lock<std::mutex> lock(_hidl_mMutex);
    _hidl_mDeathRecipients.push_back(binder_recipient);
    return (remote()->linkToDeath(binder_recipient) == ::android::OK);
}

::android::hardware::Return<void> BpHwWaydroidTask::ping(){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_ping(this, this);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::getDebugInfo(getDebugInfo_cb _hidl_cb){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_getDebugInfo(this, this, _hidl_cb);

    return _hidl_out;
}

::android::hardware::Return<void> BpHwWaydroidTask::notifySyspropsChanged(){
    ::android::hardware::Return<void>  _hidl_out = ::android::hidl::base::V1_0::BpHwBase::_hidl_notifySyspropsChanged(this, this);

    return _hidl_out;
}

::android::hardware::Return<bool> BpHwWaydroidTask::unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient){
    std::unique_lock<std::mutex> lock(_hidl_mMutex);
    for (auto it = _hidl_mDeathRecipients.rbegin();it != _hidl_mDeathRecipients.rend();++it) {
        if ((*it)->getRecipient() == recipient) {
            ::android::status_t status = remote()->unlinkToDeath(*it);
            _hidl_mDeathRecipients.erase(it.base()-1);
            return status == ::android::OK;
        }
    }
    return false;
}


BnHwWaydroidTask::BnHwWaydroidTask(const ::android::sp<IWaydroidTask> &_hidl_impl)
        : ::android::hidl::base::V1_0::BnHwBase(_hidl_impl, "vendor.waydroid.task@1.1", "IWaydroidTask") { 
            _hidl_mImpl = _hidl_impl;
            auto prio = ::android::hardware::getMinSchedulerPolicy(_hidl_impl);
            mSchedPolicy = prio.sched_policy;
            mSchedPriority = prio.prio;
            setRequestingSid(::android::hardware::getRequestingSid(_hidl_impl));
}

BnHwWaydroidTask::~BnHwWaydroidTask() {
    ::android::hardware::details::gBnMap->eraseIfEqual(_hidl_mImpl.get(), this);
}

// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.
::android::status_t BnHwWaydroidTask::_hidl_setRefreshRate(
        ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
        const ::android::hardware::Parcel &_hidl_data,
        ::android::hardware::Parcel *_hidl_reply,
        TransactCallback _hidl_cb) {
    #ifdef __ANDROID_DEBUGGABLE__
    bool mEnableInstrumentation = _hidl_this->isInstrumentationEnabled();
    const auto &mInstrumentationCallbacks = _hidl_this->getInstrumentationCallbacks();
    #endif // __ANDROID_DEBUGGABLE__

    ::android::status_t _hidl_err = ::android::OK;
    if (!_hidl_data.enforceInterface(BnHwWaydroidTask::Pure::descriptor)) {
        _hidl_err = ::android::BAD_TYPE;
        return _hidl_err;
    }

    uint32_t millihertz;

    _hidl_err = _hidl_data.readUint32(&millihertz);
    if (_hidl_err != ::android::OK) { return _hidl_err; }

    atrace_begin(ATRACE_TAG_HAL, "HIDL::IWaydroidTask::setRefreshRate::server");
    #ifdef __ANDROID_DEBUGGABLE__
    if (UNLIKELY(mEnableInstrumentation)) {
        std::vector<void *> _hidl_args;
        _hidl_args.push_back((void *)&millihertz);
        for (const auto &callback: mInstrumentationCallbacks) {
            callback(InstrumentationEvent::SERVER_API_ENTRY, "vendor.waydroid.task", "1.1", "IWaydroidTask", "setRefreshRate", &_hidl_args);
        }
    }
    #endif // __ANDROID_DEBUGGABLE__

    ::android::hardware::Return<void> _hidl_ret = static_cast<IWaydroidTask*>(_hidl_this->getImpl().get())->setRefreshRate(millihertz);

    (void) _hidl_cb;

    atrace_end(ATRACE_TAG_HAL);
    #ifdef __ANDROID_DEBUGGABLE__
    if (UNLIKELY(mEnableInstrumentation)) {
        std::vector<void *> _hidl_args;
        for (const auto &callback: mInstrumentationCallbacks) {
            callback(InstrumentationEvent::SERVER_API_EXIT, "vendor.waydroid.task", "1.1", "IWaydroidTask", "setRefreshRate", &_hidl_args);
        }
    }
    #endif // __ANDROID_DEBUGGABLE__

    _hidl_ret.assertOk();
    ::android::hardware::writeToParcel(::android::hardware::Status::ok(), _hidl_reply);

    return _hidl_err;
}


// Methods from ::vendor::waydroid::task::V1_0::IWaydroidTask follow.

// Methods from ::vendor::waydroid::task::V1_1::IWaydroidTask follow.

// Methods from ::android::hidl::base::V1_0::IBase follow.
::android::hardware::Return<void> BnHwWaydroidTask::ping() {
    return ::android::hardware::Void();
}
::android::hardware::Return<void> BnHwWaydroidTask::getDebugInfo(getDebugInfo_cb _hidl_cb) {
    ::android::hidl::base::V1_0::DebugInfo info = {};
    info.pid = ::android::hardware::details::getPidIfSharable();
    info.ptr = ::android::hardware::details::debuggable()? reinterpret_cast<uint64_t>(this) : 0;
    info.arch = 
    #if defined(__LP64__)
    ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_64BIT
    #else
    ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_32BIT
    #endif
    ;
    _hidl_cb(info);
    return ::android::hardware::Void();
}

::android::status_t BnHwWaydroidTask::onTransact(
        uint32_t _hidl_code,
        const ::android::hardware::Parcel &_hidl_data,
        ::android::hardware::Parcel *_hidl_reply,
        uint32_t _hidl_flags,
        TransactCallback _hidl_cb) {
    ::android::status_t _hidl_err = ::android::OK;

    switch (_hidl_code) {
        case 1 /* setFocusedTask */:
        {
            bool _hidl_is_oneway = _hidl_flags & 1u /* oneway */;
            if (_hidl_is_oneway != false) {
                return ::android::UNKNOWN_ERROR;
            }

            _hidl_err = ::vendor::waydroid::task::V1_0::BnHwWaydroidTask::_hidl_setFocusedTask(this, _hidl_data, _hidl_reply, _hidl_cb);
            break;
        }

        case 2 /* removeTask */:
        {
            bool _hidl_is_oneway = _hidl_flags & 1u /* oneway */;
            if (_hidl_is_oneway != false) {
                return ::android::UNKNOWN_ERROR;
            }

            _hidl_err = ::vendor::waydroid::task::V1_0::BnHwWaydroidTask::_hidl_removeTask(this, _hidl_data, _hidl_reply, _hidl_cb);
            break;
        }

        case 3 /* removeAllVisibleRecentTasks */:
        {
            bool _hidl_is_oneway = _hidl_flags & 1u /* oneway */;
            if (_hidl_is_oneway != false) {
                return ::android::UNKNOWN_ERROR;
            }

            _hidl_err = ::vendor::waydroid::task::V1_0::BnHwWaydroidTask::_hidl_removeAllVisibleRecentTasks(this, _hidl_data, _hidl_reply, _hidl_cb);
            break;
        }

        case 4 /* getAppName */:
        {
            bool _hidl_is_oneway = _hidl_flags & 1u /* oneway */;
            if (_hidl_is_oneway != false) {
                return ::android::UNKNOWN_ERROR;
            }

            _hidl_err = ::vendor::waydroid::task::V1_0::BnHwWaydroidTask::_hidl_getAppName(this, _hidl_data, _hidl_reply, _hidl_cb);
            break;
        }

        case 5 /* setRefreshRate */:
        {
            bool _hidl_is_oneway = _hidl_flags & 1u /* oneway */;
            if (_hidl_is_oneway != true) {
                return ::android::UNKNOWN_ERROR;
            }

            _hidl_err = ::vendor::waydroid::task::V1_1::BnHwWaydroidTask::_hidl_setRefreshRate(this, _hidl_data, _hidl_reply, _hidl_cb);
            break;
        }

        default:
        {
            return ::android::hidl::base::V1_0::BnHwBase::onTransact(
                    _hidl_code, _hidl_data, _hidl_reply, _hidl_flags, _hidl_cb);
        }
    }

    if (_hidl_err == ::android::UNEXPECTED_NULL) {
        _hidl_err = ::android::hardware::writeToParcel(
                ::android::hardware::Status::fromExceptionCode(::android::hardware::Status::EX_NULL_POINTER),
                _hidl_reply);
    }return _hidl_err;
}

BsWaydroidTask::BsWaydroidTask(const ::android::sp<::vendor::waydroid::task::V1_1::IWaydroidTask> impl) : ::android::hardware::details::HidlInstrumentor("vendor.waydroid.task@1.1", "IWaydroidTask"), mImpl(impl) {
    mOnewayQueue.start(3000 /* similar limit to binderized */);
}

::android::hardware::Return<void> BsWaydroidTask::addOnewayTask(std::function<void(void)> fun) {
    if (!mOnewayQueue.push(fun)) {
        return ::android::hardware::Status::fromExceptionCode(
                ::android::hardware::Status::EX_TRANSACTION_FAILED,
                "Passthrough oneway function queue exceeds maximum size.");
    }
    return ::android::hardware::Status();
}

::android::sp<IWaydroidTask> IWaydroidTask::tryGetService(const std::string &serviceName, const bool getStub) {
    return ::android::hardware::details::getServiceInternal<BpHwWaydroidTask>(serviceName, false, getStub);
}

::android::sp<IWaydroidTask> IWaydroidTask::getService(const std::string &serviceName, const bool getStub) {
    return ::android::hardware::details::getServiceInternal<BpHwWaydroidTask>(serviceName, true, getStub);
}

::android::status_t IWaydroidTask::registerAsService(const std::string &serviceName) {
    return ::android::hardware::details::registerAsServiceInternal(this, serviceName);
}

bool IWaydroidTask::registerForNotifications(
        const std::string &serviceName,
        const ::android::sp<::android::hidl::manager::V1_0::IServiceNotification> &notification) {
    const ::android::sp<::android::hidl::manager::V1_0::IServiceManager> sm
            = ::android::hardware::defaultServiceManager();
    if (sm == nullptr) {
        return false;
    }
    ::android::hardware::Return<bool> success =
            sm->registerForNotifications("vendor.waydroid.task@1.1::IWaydroidTask",
                    serviceName, notification);
    return success.isOk() && success;
}

static_assert(sizeof(::android::hardware::MQDescriptor<char, ::android::hardware::kSynchronizedReadWrite>) == 32, "wrong size");
static_assert(sizeof(::android::hardware::hidl_handle) == 16, "wrong size");
static_assert(sizeof(::android::hardware::hidl_memory) == 40, "wrong size");
static_assert(sizeof(::android::hardware::hidl_string) == 16, "wrong size");
static_assert(sizeof(::android::hardware::hidl_vec<char>) == 16, "wrong size");

}  // namespace V1_1
}  // namespace task
}  // namespace waydroid
}  // namespace vendor
