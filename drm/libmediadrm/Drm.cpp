/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "Drm"
#include <utils/Log.h>

#include <dirent.h>
#include <dlfcn.h>

#include <media/DrmSessionClientInterface.h>
#include <media/DrmSessionManager.h>
#include <media/Drm.h>
#include <media/drm/DrmAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>

namespace android {

static inline int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static bool checkPermission(const char* permissionString) {
    if (getpid() == IPCThreadState::self()->getCallingPid()) return true;
    bool ok = checkCallingPermission(String16(permissionString));
    if (!ok) ALOGE("Request requires %s", permissionString);
    return ok;
}

KeyedVector<Vector<uint8_t>, String8> Drm::mUUIDToLibraryPathMap;
KeyedVector<String8, wp<SharedLibrary> > Drm::mLibraryPathToOpenLibraryMap;
Mutex Drm::mMapLock;
Mutex Drm::mLock;

static bool operator<(const Vector<uint8_t> &lhs, const Vector<uint8_t> &rhs) {
    if (lhs.size() < rhs.size()) {
        return true;
    } else if (lhs.size() > rhs.size()) {
        return false;
    }

    return memcmp((void *)lhs.array(), (void *)rhs.array(), rhs.size()) < 0;
}

struct DrmSessionClient : public DrmSessionClientInterface {
    DrmSessionClient(Drm* drm) : mDrm(drm) {}

    virtual bool reclaimSession(const Vector<uint8_t>& sessionId) {
        sp<Drm> drm = mDrm.promote();
        if (drm == NULL) {
            return true;
        }
        status_t err = drm->closeSession(sessionId);
        if (err != OK) {
            return false;
        }
        drm->sendEvent(DrmPlugin::kDrmPluginEventSessionReclaimed, 0, &sessionId, NULL);
        return true;
    }

protected:
    virtual ~DrmSessionClient() {}

private:
    wp<Drm> mDrm;

    DISALLOW_EVIL_CONSTRUCTORS(DrmSessionClient);
};

Drm::Drm()
    : mInitCheck(NO_INIT),
      mDrmSessionClient(new DrmSessionClient(this)),
      mListener(NULL),
      mFactory(NULL),
      mPlugin(NULL) {
}

Drm::~Drm() {
    DrmSessionManager::Instance()->removeDrm(mDrmSessionClient);
    delete mPlugin;
    mPlugin = NULL;
    closeFactory();
}

void Drm::closeFactory() {
    delete mFactory;
    mFactory = NULL;
    mLibrary.clear();
}

status_t Drm::initCheck() const {
    return mInitCheck;
}

status_t Drm::setListener(const sp<IDrmClient>& listener)
{
    Mutex::Autolock lock(mEventLock);
    if (mListener != NULL){
        IInterface::asBinder(mListener)->unlinkToDeath(this);
    }
    if (listener != NULL) {
        IInterface::asBinder(listener)->linkToDeath(this);
    }
    mListener = listener;
    return NO_ERROR;
}

void Drm::sendEvent(DrmPlugin::EventType eventType, int extra,
                    Vector<uint8_t> const *sessionId,
                    Vector<uint8_t> const *data)
{
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Parcel obj;
        writeByteArray(obj, sessionId);
        writeByteArray(obj, data);

        Mutex::Autolock lock(mNotifyLock);
        listener->notify(eventType, extra, &obj);
    }
}

void Drm::sendExpirationUpdate(Vector<uint8_t> const *sessionId,
                               int64_t expiryTimeInMS)
{
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Parcel obj;
        writeByteArray(obj, sessionId);
        obj.writeInt64(expiryTimeInMS);

        Mutex::Autolock lock(mNotifyLock);
        listener->notify(DrmPlugin::kDrmPluginEventExpirationUpdate, 0, &obj);
    }
}

void Drm::sendKeysChange(Vector<uint8_t> const *sessionId,
                         Vector<DrmPlugin::KeyStatus> const *keyStatusList,
                         bool hasNewUsableKey)
{
    mEventLock.lock();
    sp<IDrmClient> listener = mListener;
    mEventLock.unlock();

    if (listener != NULL) {
        Parcel obj;
        writeByteArray(obj, sessionId);

        size_t nkeys = keyStatusList->size();
        obj.writeInt32(keyStatusList->size());
        for (size_t i = 0; i < nkeys; ++i) {
            const DrmPlugin::KeyStatus *keyStatus = &keyStatusList->itemAt(i);
            writeByteArray(obj, &keyStatus->mKeyId);
            obj.writeInt32(keyStatus->mType);
        }
        obj.writeInt32(hasNewUsableKey);

        Mutex::Autolock lock(mNotifyLock);
        listener->notify(DrmPlugin::kDrmPluginEventKeysChange, 0, &obj);
    }
}

/*
 * Search the plugins directory for a plugin that supports the scheme
 * specified by uuid
 *
 * If found:
 *    mLibrary holds a strong pointer to the dlopen'd library
 *    mFactory is set to the library's factory method
 *    mInitCheck is set to OK
 *
 * If not found:
 *    mLibrary is cleared and mFactory are set to NULL
 *    mInitCheck is set to an error (!OK)
 */
void Drm::findFactoryForScheme(const uint8_t uuid[16]) {

    closeFactory();

    // lock static maps
    Mutex::Autolock autoLock(mMapLock);

    // first check cache
    Vector<uint8_t> uuidVector;
    uuidVector.appendArray(uuid, sizeof(uuid[0]) * 16);
    ssize_t index = mUUIDToLibraryPathMap.indexOfKey(uuidVector);
    if (index >= 0) {
        if (loadLibraryForScheme(mUUIDToLibraryPathMap[index], uuid)) {
            mInitCheck = OK;
            return;
        } else {
            ALOGE("Failed to load from cached library path!");
            mInitCheck = ERROR_UNSUPPORTED;
            return;
        }
    }

    // no luck, have to search
    String8 dirPath("/vendor/lib/mediadrm");
    DIR* pDir = opendir(dirPath.string());

    if (pDir == NULL) {
        mInitCheck = ERROR_UNSUPPORTED;
        ALOGE("Failed to open plugin directory %s", dirPath.string());
        return;
    }


    struct dirent* pEntry;
    while ((pEntry = readdir(pDir))) {

        String8 pluginPath = dirPath + "/" + pEntry->d_name;

        if (pluginPath.getPathExtension() == ".so") {

            ALOGE("Testing lib %s\n", pluginPath.string());
            if (loadLibraryForScheme(pluginPath, uuid)) {
                mUUIDToLibraryPathMap.add(uuidVector, pluginPath);
                mInitCheck = OK;
                closedir(pDir);
                return;
            }
        }
    }

    closedir(pDir);

    ALOGE("Failed to find drm plugin (%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X)", 
      uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], 
      uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);

    mInitCheck = ERROR_UNSUPPORTED;
}

bool Drm::loadLibraryForScheme(const String8 &path, const uint8_t uuid[16]) {

    // get strong pointer to open shared library
    ssize_t index = mLibraryPathToOpenLibraryMap.indexOfKey(path);
    if (index >= 0) {
        mLibrary = mLibraryPathToOpenLibraryMap[index].promote();
    } else {
        index = mLibraryPathToOpenLibraryMap.add(path, NULL);
    }
    ALOGE("index %d\n", index);

    if (!mLibrary.get()) {
        ALOGE("!mLibrary.get()");
        mLibrary = new SharedLibrary(path);
        if (!*mLibrary) {
            ALOGE("new SharedLibrary failed\n");
            return false;
        }
        ALOGE("index %d\n", index);

        mLibraryPathToOpenLibraryMap.replaceValueAt(index, mLibrary);
    }

    typedef DrmFactory *(*CreateDrmFactoryFunc)();

    CreateDrmFactoryFunc createDrmFactory =
        (CreateDrmFactoryFunc)mLibrary->lookup("createDrmFactory");

    if (createDrmFactory == NULL ||
        (mFactory = createDrmFactory()) == NULL ||
        !mFactory->isCryptoSchemeSupported(uuid)) {
        closeFactory();
        ALOGE("createDrmFactory failed\n");
        return false;
    }
    return true;
}

bool Drm::isCryptoSchemeSupported(const uint8_t uuid[16], const String8 &mimeType) {

    Mutex::Autolock autoLock(mLock);

    if (!mFactory || !mFactory->isCryptoSchemeSupported(uuid)) {
        findFactoryForScheme(uuid);
        if (mInitCheck != OK) {
            return false;
        }
    }

    if (mimeType != "") {
        return mFactory->isContentTypeSupported(mimeType);
    }

    return true;
}

status_t Drm::createPlugin(const uint8_t uuid[16]) {
    Mutex::Autolock autoLock(mLock);

    if (mPlugin != NULL) {
        return -EINVAL;
    }

    if (!mFactory || !mFactory->isCryptoSchemeSupported(uuid)) {
        findFactoryForScheme(uuid);
    }

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    status_t result = mFactory->createDrmPlugin(uuid, &mPlugin);
    mPlugin->setListener(this);
    return result;
}

status_t Drm::destroyPlugin() {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    delete mPlugin;
    mPlugin = NULL;

    return OK;
}

status_t Drm::openSession(Vector<uint8_t> &sessionId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    status_t err = mPlugin->openSession(sessionId);
    if (err == ERROR_DRM_RESOURCE_BUSY) {
        bool retry = false;
        mLock.unlock();
        // reclaimSession may call back to closeSession, since mLock is shared between Drm
        // instances, we should unlock here to avoid deadlock.
        retry = DrmSessionManager::Instance()->reclaimSession(getCallingPid());
        mLock.lock();
        if (mInitCheck != OK) {
            return mInitCheck;
        }

        if (mPlugin == NULL) {
            return -EINVAL;
        }
        if (retry) {
            err = mPlugin->openSession(sessionId);
        }
    }
    if (err == OK) {
        DrmSessionManager::Instance()->addSession(getCallingPid(), mDrmSessionClient, sessionId);
    }
    return err;
}

status_t Drm::closeSession(Vector<uint8_t> const &sessionId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    status_t err = mPlugin->closeSession(sessionId);
    if (err == OK) {
        DrmSessionManager::Instance()->removeSession(sessionId);
    }
    return err;
}

status_t Drm::getKeyRequest(Vector<uint8_t> const &sessionId,
                            Vector<uint8_t> const &initData,
                            String8 const &mimeType, DrmPlugin::KeyType keyType,
                            KeyedVector<String8, String8> const &optionalParameters,
                            Vector<uint8_t> &request, String8 &defaultUrl,
                            DrmPlugin::KeyRequestType *keyRequestType) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->getKeyRequest(sessionId, initData, mimeType, keyType,
                                  optionalParameters, request, defaultUrl,
                                  keyRequestType);
}

status_t Drm::provideKeyResponse(Vector<uint8_t> const &sessionId,
                                 Vector<uint8_t> const &response,
                                 Vector<uint8_t> &keySetId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->provideKeyResponse(sessionId, response, keySetId);
}

status_t Drm::removeKeys(Vector<uint8_t> const &keySetId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->removeKeys(keySetId);
}

status_t Drm::restoreKeys(Vector<uint8_t> const &sessionId,
                          Vector<uint8_t> const &keySetId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->restoreKeys(sessionId, keySetId);
}

status_t Drm::queryKeyStatus(Vector<uint8_t> const &sessionId,
                             KeyedVector<String8, String8> &infoMap) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->queryKeyStatus(sessionId, infoMap);
}

status_t Drm::getProvisionRequest(String8 const &certType, String8 const &certAuthority,
                                  Vector<uint8_t> &request, String8 &defaultUrl) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->getProvisionRequest(certType, certAuthority,
                                        request, defaultUrl);
}

status_t Drm::provideProvisionResponse(Vector<uint8_t> const &response,
                                       Vector<uint8_t> &certificate,
                                       Vector<uint8_t> &wrappedKey) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->provideProvisionResponse(response, certificate, wrappedKey);
}

status_t Drm::getSecureStops(List<Vector<uint8_t> > &secureStops) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->getSecureStops(secureStops);
}

status_t Drm::getSecureStop(Vector<uint8_t> const &ssid, Vector<uint8_t> &secureStop) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->getSecureStop(ssid, secureStop);
}

status_t Drm::releaseSecureStops(Vector<uint8_t> const &ssRelease) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->releaseSecureStops(ssRelease);
}

status_t Drm::releaseAllSecureStops() {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->releaseAllSecureStops();
}

status_t Drm::getPropertyString(String8 const &name, String8 &value ) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->getPropertyString(name, value);
}

status_t Drm::getPropertyByteArray(String8 const &name, Vector<uint8_t> &value ) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->getPropertyByteArray(name, value);
}

status_t Drm::setPropertyString(String8 const &name, String8 const &value ) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->setPropertyString(name, value);
}

status_t Drm::setPropertyByteArray(String8 const &name,
                                   Vector<uint8_t> const &value ) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->setPropertyByteArray(name, value);
}


status_t Drm::setCipherAlgorithm(Vector<uint8_t> const &sessionId,
                                 String8 const &algorithm) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->setCipherAlgorithm(sessionId, algorithm);
}

status_t Drm::setMacAlgorithm(Vector<uint8_t> const &sessionId,
                              String8 const &algorithm) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->setMacAlgorithm(sessionId, algorithm);
}

status_t Drm::encrypt(Vector<uint8_t> const &sessionId,
                      Vector<uint8_t> const &keyId,
                      Vector<uint8_t> const &input,
                      Vector<uint8_t> const &iv,
                      Vector<uint8_t> &output) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->encrypt(sessionId, keyId, input, iv, output);
}

status_t Drm::decrypt(Vector<uint8_t> const &sessionId,
                      Vector<uint8_t> const &keyId,
                      Vector<uint8_t> const &input,
                      Vector<uint8_t> const &iv,
                      Vector<uint8_t> &output) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->decrypt(sessionId, keyId, input, iv, output);
}

status_t Drm::sign(Vector<uint8_t> const &sessionId,
                   Vector<uint8_t> const &keyId,
                   Vector<uint8_t> const &message,
                   Vector<uint8_t> &signature) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->sign(sessionId, keyId, message, signature);
}

status_t Drm::verify(Vector<uint8_t> const &sessionId,
                     Vector<uint8_t> const &keyId,
                     Vector<uint8_t> const &message,
                     Vector<uint8_t> const &signature,
                     bool &match) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->verify(sessionId, keyId, message, signature, match);
}

status_t Drm::signRSA(Vector<uint8_t> const &sessionId,
                      String8 const &algorithm,
                      Vector<uint8_t> const &message,
                      Vector<uint8_t> const &wrappedKey,
                      Vector<uint8_t> &signature) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    if (!checkPermission("android.permission.ACCESS_DRM_CERTIFICATES")) {
        return -EPERM;
    }

    DrmSessionManager::Instance()->useSession(sessionId);

    return mPlugin->signRSA(sessionId, algorithm, message, wrappedKey, signature);
}

void Drm::binderDied(const wp<IBinder> &the_late_who __unused)
{
    mEventLock.lock();
    mListener.clear();
    mEventLock.unlock();

    Mutex::Autolock autoLock(mLock);
    delete mPlugin;
    mPlugin = NULL;
    closeFactory();
}

void Drm::writeByteArray(Parcel &obj, Vector<uint8_t> const *array)
{
    if (array && array->size()) {
        obj.writeInt32(array->size());
        obj.write(array->array(), array->size());
    } else {
        obj.writeInt32(0);
    }
}

}  // namespace android
