/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define LOG_TAG "C2MTKStore"
// #define LOG_NDEBUG 0
#include <utils/Log.h>

#include <C2DmaBufAllocator.h>
#include <C2Component.h>
#include <C2Config.h>
#include <C2PlatformStorePluginLoader.h>
#include <C2PlatformSupport.h>
#include <codec2/common/HalSelection.h>
#include <cutils/properties.h>
#include <util/C2InterfaceHelper.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include <dlfcn.h>
#include <unistd.h> // getpagesize

#include <map>
#include <memory>
#include <mutex>

namespace android {

/**
 * Returns the preferred component store in this process to access its interface.
 */
std::shared_ptr<C2ComponentStore> GetPreferredCodec2ComponentStore();

class C2MtkComponentStore : public C2ComponentStore {
public:
    virtual std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;
    virtual std::shared_ptr<C2ParamReflector> getParamReflector() const override;
    virtual C2String getName() const override;
    virtual c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery> &fields) const override;
    virtual c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const override;
    virtual c2_status_t query_sm(
            const std::vector<C2Param*> &stackParams,
            const std::vector<C2Param::Index> &heapParamIndices,
            std::vector<std::unique_ptr<C2Param>> *const heapParams) const override;
    virtual c2_status_t createInterface(
            C2String name, std::shared_ptr<C2ComponentInterface> *const interface) override;
    virtual c2_status_t createComponent(
            C2String name, std::shared_ptr<C2Component> *const component) override;
    virtual c2_status_t copyBuffer(
            std::shared_ptr<C2GraphicBuffer> src, std::shared_ptr<C2GraphicBuffer> dst) override;
    virtual c2_status_t config_sm(
            const std::vector<C2Param*> &params,
            std::vector<std::unique_ptr<C2SettingResult>> *const failures) override;
    C2MtkComponentStore();

    virtual ~C2MtkComponentStore() override = default;

private:

    /**
     * An object encapsulating a loaded component module.
     *
     * \todo provide a way to add traits to known components here to avoid loading the .so-s
     * for listComponents
     */
    struct ComponentModule : public C2ComponentFactory,
            public std::enable_shared_from_this<ComponentModule> {
        virtual c2_status_t createComponent(
                c2_node_id_t id, std::shared_ptr<C2Component> *component,
                ComponentDeleter deleter = std::default_delete<C2Component>()) override;
        virtual c2_status_t createInterface(
                c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
                InterfaceDeleter deleter = std::default_delete<C2ComponentInterface>()) override;
        virtual c2_status_t createInitInterface(
                c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
                InterfaceDeleter deleter = std::default_delete<C2ComponentInterface>());

        /**
         * \returns the traits of the component in this module.
         */
        std::shared_ptr<const C2Component::Traits> getTraits();

        /**
         * Creates an uninitialized component module.
         *
         * \param name[in]  component name.
         *
         * \note Only used by ComponentLoader.
         */
        ComponentModule()
            : mInit(C2_NO_INIT),
              mLibHandle(nullptr),
              createInitFactory(nullptr),
              createFactory(nullptr),
              destroyFactory(nullptr),
              mComponentFactory(nullptr),
              mComponentInitFactory(nullptr) {
        }

        /**
         * Initializes a component module with a given library path. Must be called exactly once.
         *
         * \note Only used by ComponentLoader.
         *
         * \param libPath[in] library path
         *
         * \retval C2_OK        the component module has been successfully loaded
         * \retval C2_NO_MEMORY not enough memory to loading the component module
         * \retval C2_NOT_FOUND could not locate the component module
         * \retval C2_CORRUPTED the component module could not be loaded (unexpected)
         * \retval C2_REFUSED   permission denied to load the component module (unexpected)
         * \retval C2_TIMED_OUT could not load the module within the time limit (unexpected)
         */
        c2_status_t init(std::string libPath, std::string mediaType, bool isSecure, bool isLowLatency);

        virtual ~ComponentModule() override;
        typedef ::C2ComponentFactory* (*CreateCodec2InitFactoryFunc)(
            std::string, bool, bool,
            std::function<uint32_t(bool, uint32_t, std::string, std::string)>);

    protected:
        std::recursive_mutex mLock; ///< lock protecting mTraits
        std::shared_ptr<C2Component::Traits> mTraits; ///< cached component traits

        c2_status_t mInit; ///< initialization result

        void *mLibHandle; ///< loaded library handle
        CreateCodec2InitFactoryFunc createInitFactory; ///< loaded create function for init
        C2ComponentFactory::CreateCodec2FactoryFunc createFactory; ///< loaded create function
        C2ComponentFactory::DestroyCodec2FactoryFunc destroyFactory; ///< loaded destroy function
        C2ComponentFactory *mComponentFactory; ///< loaded/created component factory
        C2ComponentFactory *mComponentInitFactory; ///< loaded/created component factory for init
    };

    /**
     * An object encapsulating a loadable component module.
     *
     * \todo make this also work for enumerations
     */
    struct ComponentLoader {
        /**
         * Load the component module.
         *
         * This method simply returns the component module if it is already currently loaded, or
         * attempts to load it if it is not.
         *
         * \param module[out] pointer to the shared pointer where the loaded module shall be stored.
         *                    This will be nullptr on error.
         *
         * \retval C2_OK        the component module has been successfully loaded
         * \retval C2_NO_MEMORY not enough memory to loading the component module
         * \retval C2_NOT_FOUND could not locate the component module
         * \retval C2_CORRUPTED the component module could not be loaded
         * \retval C2_REFUSED   permission denied to load the component module
         */
        c2_status_t fetchModule(std::shared_ptr<ComponentModule> *module, bool isSecure, bool isLowLatency) {
            c2_status_t res = C2_OK;
            std::lock_guard<std::mutex> lock(mMutex);
            std::shared_ptr<ComponentModule> localModule = mModule.lock();
            if (localModule == nullptr) {
                localModule = std::make_shared<ComponentModule>();
                res = localModule->init(mLibPath, mMediaType, isSecure, isLowLatency);
                if (res == C2_OK) {
                    mModule = localModule;
                }
            }
            *module = localModule;
            return res;
        }

        /**
         * Creates a component loader for a specific library path (or name).
         */
        ComponentLoader(std::string libPath)
            : mLibPath(libPath) {}

        ComponentLoader(std::string libPath, std::string mediaType)
            : mLibPath(libPath), mMediaType(mediaType) {}

    private:
        std::mutex mMutex; ///< mutex guarding the module
        std::weak_ptr<ComponentModule> mModule; ///< weak reference to the loaded module
        std::string mLibPath; ///< library path
        std::string mMediaType;
    };

    struct Interface : public C2InterfaceHelper {
        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
        std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;

        Interface(std::shared_ptr<C2ReflectorHelper> reflector)
            : C2InterfaceHelper(reflector) {
            setDerivedInstance(this);

            struct Setter {
                static C2R setIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
                    me.set().heapMask = ~0;
                    me.set().allocFlags = 0;
                    me.set().minAlignment = 0;
                    return C2R::Ok();
                };

                static C2R setDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
                    long long usage = (long long)me.get().m.usage;
                    if (C2DmaBufAllocator::system_uncached_supported() &&
                        !(usage & (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE))) {
                        strncpy(me.set().m.heapName, "system-uncached", me.v.flexCount());
                    } else {
                        strncpy(me.set().m.heapName, "system", me.v.flexCount());
                    }
                    me.set().m.allocFlags = 0;
                    return C2R::Ok();
                };
            };

            addParameter(
                DefineParam(mIonUsageInfo, "ion-usage")
                .withDefault(new C2StoreIonUsageInfo())
                .withFields({
                    C2F(mIonUsageInfo, usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mIonUsageInfo, capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mIonUsageInfo, heapMask).any(),
                    C2F(mIonUsageInfo, allocFlags).flags({}),
                    C2F(mIonUsageInfo, minAlignment).equalTo(0)
                })
                .withSetter(Setter::setIonUsage)
                .build());

            addParameter(
                DefineParam(mDmaBufUsageInfo, "dmabuf-usage")
                .withDefault(C2StoreDmaBufUsageInfo::AllocShared(0))
                .withFields({
                    C2F(mDmaBufUsageInfo, m.usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mDmaBufUsageInfo, m.capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
                    C2F(mDmaBufUsageInfo, m.heapName).any(),
                })
                .withSetter(Setter::setDmaBufUsage)
                .build());
        }
    };

    /**
     * Retrieves the component module for a component.
     *
     * \param module pointer to a shared_pointer where the component module will be stored on
     *               success.
     *
     * \retval C2_OK        the component loader has been successfully retrieved
     * \retval C2_NO_MEMORY not enough memory to locate the component loader
     * \retval C2_NOT_FOUND could not locate the component to be loaded
     * \retval C2_CORRUPTED the component loader could not be identified due to some modules being
     *                      corrupted (this can happen if the name does not refer to an already
     *                      identified component but some components could not be loaded due to
     *                      bad library)
     * \retval C2_REFUSED   permission denied to find the component loader for the named component
     *                      (this can happen if the name does not refer to an already identified
     *                      component but some components could not be loaded due to lack of
     *                      permissions)
     */
    c2_status_t findComponent(C2String name, std::shared_ptr<ComponentModule> *module);

    /**
     * Loads each component module and discover its contents.
     */
    void visitComponents();

    std::mutex mMutex; ///< mutex guarding the component lists during construction
    bool mVisited; ///< component modules visited
    std::map<C2String, ComponentLoader> mComponents; ///< path -> component module
    std::map<C2String, C2String> mComponentNameToPath; ///< name -> path
    std::vector<std::shared_ptr<const C2Component::Traits>> mComponentList;

    std::shared_ptr<C2ReflectorHelper> mReflector;
    Interface mInterface;
};

c2_status_t C2MtkComponentStore::ComponentModule::init(
        std::string libPath,
        std::string mediaType,
        bool isSecure,
        bool isLowLatency) {
    ALOGV("in %s", __func__);
    ALOGV("loading dll");
    {
        mLibHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    }
    if (mLibHandle == nullptr) {
        ALOGE("could not dlopen %s: %s", libPath.c_str(), dlerror());
        return C2_NOT_FOUND;
    }

    std::string createFactoryName = "CreateCodec2Factory";
    std::string createInitFactoryName = "CreateCodec2InitFactory";
    std::string destroyFactoryName = "DestroyCodec2Factory";

    if (MEDIA_MIMETYPE_VIDEO_MPEG2 && MEDIA_MIMETYPE_VIDEO_MPEG2 == mediaType) {
        createFactoryName = "CreateCodec2Mpeg2Factory";
        destroyFactoryName = "DestroyCodec2Mpeg2Factory";
    } else if (MEDIA_MIMETYPE_VIDEO_HEVC && MEDIA_MIMETYPE_VIDEO_HEVC == mediaType && !isSecure && !isLowLatency) {
        createFactoryName = "CreateCodec2HevcFactory";
        destroyFactoryName = "DestroyCodec2HevcFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_HEVC && MEDIA_MIMETYPE_VIDEO_HEVC == mediaType && isSecure) {
        createFactoryName = "CreateCodec2HevcSecFactory";
        destroyFactoryName = "DestroyCodec2HevcSecFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_HEVC && MEDIA_MIMETYPE_VIDEO_HEVC == mediaType && isLowLatency) {
        createFactoryName = "CreateCodec2HevcLowLatencyFactory";
        destroyFactoryName = "DestroyCodec2HevcLowLatencyFactory";
    } else if (MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC && MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC == mediaType) {
        createFactoryName = "CreateCodec2HeifFactory";
        destroyFactoryName = "DestroyCodec2HeifFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_MPEG4 && MEDIA_MIMETYPE_VIDEO_MPEG4 == mediaType) {
        createFactoryName = "CreateCodec2Mpeg4Factory";
        destroyFactoryName = "DestroyCodec2Mpeg4Factory";
    } else if (MEDIA_MIMETYPE_VIDEO_H263 && MEDIA_MIMETYPE_VIDEO_H263 == mediaType) {
        createFactoryName = "CreateCodec2H263Factory";
        destroyFactoryName = "DestroyCodec2H263Factory";
    } else if (MEDIA_MIMETYPE_VIDEO_AVC && MEDIA_MIMETYPE_VIDEO_AVC == mediaType && !isSecure && !isLowLatency) {
        createFactoryName = "CreateCodec2AvcFactory";
        destroyFactoryName = "DestroyCodec2AvcFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_AVC && MEDIA_MIMETYPE_VIDEO_AVC == mediaType && isSecure) {
        createFactoryName = "CreateCodec2AvcSecFactory";
        destroyFactoryName = "DestroyCodec2AvcSecFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_AVC && MEDIA_MIMETYPE_VIDEO_AVC == mediaType && isLowLatency) {
        createFactoryName = "CreateCodec2AvcLowLatencyFactory";
        destroyFactoryName = "DestroyCodec2AvcLowLatencyFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_VP8 && MEDIA_MIMETYPE_VIDEO_VP8 == mediaType) {
        createFactoryName = "CreateCodec2VpxFactory";
        destroyFactoryName = "DestroyCodec2VpxFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_VP9 && MEDIA_MIMETYPE_VIDEO_VP9 == mediaType && !isSecure && !isLowLatency) {
        createFactoryName = "CreateCodec2Vp9Factory";
        destroyFactoryName = "DestroyCodec2Vp9Factory";
    } else if (MEDIA_MIMETYPE_VIDEO_VP9 && MEDIA_MIMETYPE_VIDEO_VP9 == mediaType && isSecure) {
        createFactoryName = "CreateCodec2Vp9SecFactory";
        destroyFactoryName = "DestroyCodec2Vp9SecFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_VP9 && MEDIA_MIMETYPE_VIDEO_VP9 == mediaType && isLowLatency) {
        createFactoryName = "CreateCodec2Vp9LowLatencyFactory";
        destroyFactoryName = "DestroyCodec2Vp9LowLatencyFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_AV1 && MEDIA_MIMETYPE_VIDEO_AV1 == mediaType && !isSecure && !isLowLatency) {
        createFactoryName = "CreateCodec2Av1Factory";
        destroyFactoryName = "DestroyCodec2Av1Factory";
    } else if (MEDIA_MIMETYPE_VIDEO_AV1 && MEDIA_MIMETYPE_VIDEO_AV1 == mediaType && isSecure) {
        createFactoryName = "CreateCodec2Av1SecFactory";
        destroyFactoryName = "DestroyCodec2Av1SecFactory";
    } else if (MEDIA_MIMETYPE_VIDEO_AV1 && MEDIA_MIMETYPE_VIDEO_AV1 == mediaType && isLowLatency) {
        createFactoryName = "CreateCodec2Av1LowLatencyFactory";
        destroyFactoryName = "DestroyCodec2Av1LowLatencyFactory";
    } else if ("video/x-ms-wmv" == mediaType) {
        createFactoryName = "CreateCodec2Vc1Factory";
        destroyFactoryName = "DestroyCodec2Vc1Factory";
    }

    createFactory = (C2ComponentFactory::CreateCodec2FactoryFunc)dlsym(mLibHandle, createFactoryName.c_str());
    LOG_ALWAYS_FATAL_IF(createFactory == nullptr, "createFactory is null in %s", libPath.c_str());

    createInitFactory = (CreateCodec2InitFactoryFunc)dlsym(mLibHandle, createInitFactoryName.c_str());

    destroyFactory = (C2ComponentFactory::DestroyCodec2FactoryFunc)dlsym(mLibHandle, destroyFactoryName.c_str());
    LOG_ALWAYS_FATAL_IF(destroyFactory == nullptr, "destroyFactory is null in %s", libPath.c_str());

    mComponentFactory = createFactory();
    if (createInitFactory != nullptr) {
        mComponentInitFactory = createInitFactory(mediaType, isSecure, isLowLatency, 0);
    }

    if (mComponentFactory == nullptr) {
        ALOGD("could not create factory in %s", libPath.c_str());
        mInit = C2_NO_MEMORY;
    } else if (createInitFactory != nullptr && mComponentInitFactory == nullptr) {
        ALOGD("could not create init factory in %s", libPath.c_str());
        mInit = C2_NO_MEMORY;
    } else {
        mInit = C2_OK;
    }

    if (mInit != C2_OK) {
        return mInit;
    }

    std::shared_ptr<C2ComponentInterface> intf;
    c2_status_t res = mComponentInitFactory ? createInitInterface(0, &intf) : createInterface(0, &intf);
    if (res != C2_OK) {
        ALOGD("failed to create interface: %d", res);
        return mInit;
    }

    std::shared_ptr<C2Component::Traits> traits(new (std::nothrow) C2Component::Traits);
    if (traits) {
        traits->name = intf->getName();

        C2ComponentKindSetting kind;
        C2ComponentDomainSetting domain;
        res = intf->query_vb({ &kind, &domain }, {}, C2_MAY_BLOCK, nullptr);
        bool fixDomain = res != C2_OK;
        if (res == C2_OK) {
            traits->kind = kind.value;
            traits->domain = domain.value;
        } else {
            // TODO: remove this fall-back
            ALOGD("failed to query interface for kind and domain: %d", res);

            traits->kind =
                (traits->name.find("encoder") != std::string::npos) ? C2Component::KIND_ENCODER :
                (traits->name.find("decoder") != std::string::npos) ? C2Component::KIND_DECODER :
                C2Component::KIND_OTHER;
        }

        uint32_t mediaTypeIndex =
            traits->kind == C2Component::KIND_ENCODER ? C2PortMediaTypeSetting::output::PARAM_TYPE
            : C2PortMediaTypeSetting::input::PARAM_TYPE;
        std::vector<std::unique_ptr<C2Param>> params;
        res = intf->query_vb({}, { mediaTypeIndex }, C2_MAY_BLOCK, &params);
        if (res != C2_OK) {
            ALOGD("failed to query interface: %d", res);
            return mInit;
        }
        if (params.size() != 1u) {
            ALOGD("failed to query interface: unexpected vector size: %zu", params.size());
            return mInit;
        }
        C2PortMediaTypeSetting *mediaTypeConfig = C2PortMediaTypeSetting::From(params[0].get());
        if (mediaTypeConfig == nullptr) {
            ALOGD("failed to query media type");
            return mInit;
        }
	    traits->mediaType =
            std::string(mediaTypeConfig->m.value,
                        strnlen(mediaTypeConfig->m.value, mediaTypeConfig->flexCount()));

        if (fixDomain) {
            if (strncmp(traits->mediaType.c_str(), "audio/", 6) == 0) {
                traits->domain = C2Component::DOMAIN_AUDIO;
            } else if (strncmp(traits->mediaType.c_str(), "video/", 6) == 0) {
                traits->domain = C2Component::DOMAIN_VIDEO;
            } else if (strncmp(traits->mediaType.c_str(), "image/", 6) == 0) {
                traits->domain = C2Component::DOMAIN_IMAGE;
            } else {
                traits->domain = C2Component::DOMAIN_OTHER;
            }
        }

        // TODO: get this properly from the store during emplace
        switch (traits->domain) {
        case C2Component::DOMAIN_AUDIO:
            traits->rank = 8;
            break;
        default:
            traits->rank = 512;
        }

	    params.clear();
        res = intf->query_vb({}, { C2ComponentAliasesSetting::PARAM_TYPE }, C2_MAY_BLOCK, &params);
        if (res == C2_OK && params.size() == 1u) {
            C2ComponentAliasesSetting *aliasesSetting =
                C2ComponentAliasesSetting::From(params[0].get());
            if (aliasesSetting) {
                // Split aliases on ','
                // This looks simpler in plain C and even std::string would still make a copy.
                char *aliases = ::strndup(aliasesSetting->m.value, aliasesSetting->flexCount());
                ALOGD("'%s' has aliases: '%s'", intf->getName().c_str(), aliases);

                for (char *tok, *ptr, *str = aliases; (tok = ::strtok_r(str, ",", &ptr));
                     str = nullptr) {
                    traits->aliases.push_back(tok);
                    ALOGD("adding alias: '%s'", tok);
                }
                free(aliases);
            }
        }
    }
    mTraits = traits;

    return mInit;
}

C2MtkComponentStore::ComponentModule::~ComponentModule() {
    ALOGV("in %s", __func__);
    if (destroyFactory) {
        if (mComponentInitFactory) {
            destroyFactory(mComponentInitFactory);
            mComponentInitFactory = nullptr;
        }
        if (mComponentFactory) {
            destroyFactory(mComponentFactory);
            mComponentFactory = nullptr;
        }
    }
    if (mLibHandle) {
        ALOGV("unloading dll");
        dlclose(mLibHandle);
    }
}

c2_status_t C2MtkComponentStore::ComponentModule::createInterface(
        c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
        std::function<void(::C2ComponentInterface*)> deleter) {
    interface->reset();
    if (mInit != C2_OK) {
        return mInit;
    }
    std::shared_ptr<ComponentModule> module = shared_from_this();
    c2_status_t res = mComponentFactory->createInterface(
            id, interface, [module, deleter](C2ComponentInterface *p) mutable {
                // capture module so that we ensure we still have it while deleting interface
                deleter(p); // delete interface first
                module.reset(); // remove module ref (not technically needed)
    });
    return res;
}

c2_status_t C2MtkComponentStore::ComponentModule::createInitInterface(
        c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
        std::function<void(::C2ComponentInterface*)> deleter) {
    interface->reset();
    if (mInit != C2_OK || !mComponentInitFactory) {
        return mComponentInitFactory ? mInit : C2_NOT_FOUND;
    }
    std::shared_ptr<ComponentModule> module = shared_from_this();
    c2_status_t res = mComponentInitFactory->createInterface(
            id, interface, [module, deleter](C2ComponentInterface *p) mutable {
                // capture module so that we ensure we still have it while deleting interface
                deleter(p); // delete interface first
                module.reset(); // remove module ref (not technically needed)
    });
    return res;
}

c2_status_t C2MtkComponentStore::ComponentModule::createComponent(
        c2_node_id_t id, std::shared_ptr<C2Component> *component,
        std::function<void(::C2Component*)> deleter) {
    component->reset();
    if (mInit != C2_OK) {
        return mInit;
    }
    std::shared_ptr<ComponentModule> module = shared_from_this();
    c2_status_t res = mComponentFactory->createComponent(
            id, component, [module, deleter](C2Component *p) mutable {
                // capture module so that we ensure we still have it while deleting component
                deleter(p); // delete component first
                module.reset(); // remove module ref (not technically needed)
    });
    return res;
}

std::shared_ptr<const C2Component::Traits> C2MtkComponentStore::ComponentModule::getTraits() {
    std::unique_lock<std::recursive_mutex> lock(mLock);
    return mTraits;
}

C2MtkComponentStore::C2MtkComponentStore()
    : mVisited(false),
      mReflector(std::make_shared<C2ReflectorHelper>()),
      mInterface(mReflector) {

    auto emplace = [this](const char *key, const char *libPath, const char *mediaType) {
        mComponents.emplace(std::piecewise_construct,
                            std::forward_as_tuple(key),
                            std::forward_as_tuple(libPath, mediaType));
    };

    // MTK video decoders
    emplace("c2.mtk.mpeg2.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_MPEG2);
    emplace("c2.mtk.hevc.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_HEVC);
    emplace("c2.mtk.hevc.decoder.secure", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_HEVC);
    emplace("c2.mtk.hevc.decoder.lowlatency", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_HEVC);
    emplace("c2.mtk.heif.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC);
    emplace("c2.mtk.mpeg4.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_MPEG4);
    emplace("c2.mtk.h263.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_H263);
    emplace("c2.mtk.avc.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AVC);
    emplace("c2.mtk.avc.decoder.secure", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AVC);
    emplace("c2.mtk.avc.decoder.lowlatency", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AVC);
    emplace("c2.mtk.vpx.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_VP8);
    emplace("c2.mtk.vp9.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_VP9);
    emplace("c2.mtk.vp9.decoder.secure", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_VP9);
    emplace("c2.mtk.vp9.decoder.lowlatency", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_VP9);
    emplace("c2.mtk.av1.decoder", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AV1);
    emplace("c2.mtk.av1.decoder.secure", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AV1);
    emplace("c2.mtk.av1.decoder.lowlatency", "libcodec2_mtk_vdec.so", MEDIA_MIMETYPE_VIDEO_AV1);
    emplace("c2.mtk.vc1.decoder", "libcodec2_mtk_vdec.so", "video/x-ms-wmv");

    // MTK video encoders
    emplace("c2.mtk.mpeg4.encoder", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_MPEG4);
    emplace("c2.mtk.h263.encoder", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_H263);
    emplace("c2.mtk.avc.encoder", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_AVC);
    emplace("c2.mtk.avc.encoder.secure", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_AVC);
    emplace("c2.mtk.hevc.encoder", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_HEVC);
    emplace("c2.mtk.hevc.encoder.secure", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_VIDEO_HEVC);
    emplace("c2.mtk.heif.encoder", "libcodec2_mtk_venc.so", MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC);
}

c2_status_t C2MtkComponentStore::copyBuffer(
        std::shared_ptr<C2GraphicBuffer> src, std::shared_ptr<C2GraphicBuffer> dst) {
    (void)src;
    (void)dst;
    return C2_OMITTED;
}

c2_status_t C2MtkComponentStore::query_sm(
        const std::vector<C2Param*> &stackParams,
        const std::vector<C2Param::Index> &heapParamIndices,
        std::vector<std::unique_ptr<C2Param>> *const heapParams) const {
    return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
}

c2_status_t C2MtkComponentStore::config_sm(
        const std::vector<C2Param*> &params,
        std::vector<std::unique_ptr<C2SettingResult>> *const failures) {
    return mInterface.config(params, C2_MAY_BLOCK, failures);
}

void C2MtkComponentStore::visitComponents() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mVisited) {
        return;
    }
    for (auto &pathAndLoader : mComponents) {
        const C2String &key = pathAndLoader.first;
        ComponentLoader &loader = pathAndLoader.second;
        std::shared_ptr<ComponentModule> module;
        bool isSecure  = false;
        if (key.find("secure") != std::string::npos) {
            isSecure = true;
        }
        bool isLowLatency  = false;
        if (key.find("lowlatency") != std::string::npos) {
            isLowLatency = true;
        }

        if (loader.fetchModule(&module, isSecure, isLowLatency) == C2_OK) {
            std::shared_ptr<const C2Component::Traits> traits = module->getTraits();
            if (traits) {
                mComponentList.push_back(traits);
                mComponentNameToPath.emplace(traits->name, key);
                for (const C2String &alias : traits->aliases) {
                    mComponentNameToPath.emplace(alias, key);
                }
            }
        }
    }
    mVisited = true;
}

std::vector<std::shared_ptr<const C2Component::Traits>> C2MtkComponentStore::listComponents() {
    // This method SHALL return within 500ms.
    visitComponents();
    return mComponentList;
}

c2_status_t C2MtkComponentStore::findComponent(
        C2String name, std::shared_ptr<ComponentModule> *module) {
    (*module).reset();
    visitComponents();

    bool isSecure  = false;
    if (name.find("secure") != std::string::npos) {
        isSecure = true;
    }
    bool isLowLatency  = false;
    if (name.find("lowlatency") != std::string::npos) {
        isLowLatency = true;
    }

    auto pos = mComponentNameToPath.find(name);
    if (pos != mComponentNameToPath.end()) {
        return mComponents.at(pos->second).fetchModule(module, isSecure, isLowLatency);
    }
    return C2_NOT_FOUND;
}

c2_status_t C2MtkComponentStore::createComponent(
        C2String name, std::shared_ptr<C2Component> *const component) {
    // This method SHALL return within 100ms.
    component->reset();
    std::shared_ptr<ComponentModule> module;
    c2_status_t res = findComponent(name, &module);
    if (res == C2_OK) {
        // TODO: get a unique node ID
        res = module->createComponent(0, component);
    }
    return res;
}

c2_status_t C2MtkComponentStore::createInterface(
        C2String name, std::shared_ptr<C2ComponentInterface> *const interface) {
    // This method SHALL return within 100ms.
    interface->reset();
    std::shared_ptr<ComponentModule> module;
    c2_status_t res = findComponent(name, &module);
    if (res == C2_OK) {
        // TODO: get a unique node ID
        res = module->createInterface(0, interface);
    }
    return res;
}

c2_status_t C2MtkComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const {
    return mInterface.querySupportedParams(params);
}

c2_status_t C2MtkComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery> &fields) const {
    return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
}

C2String C2MtkComponentStore::getName() const {
    return "android.componentStore.mtk";
}

std::shared_ptr<C2ParamReflector> C2MtkComponentStore::getParamReflector() const {
    return mReflector;
}

std::shared_ptr<C2ComponentStore> GetCodeC2MtkComponentStore() {
    static std::mutex mutex;
    static std::weak_ptr<C2ComponentStore> platformStore;
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<C2ComponentStore> store = platformStore.lock();
    if (store == nullptr) {
        store = std::make_shared<C2MtkComponentStore>();
        platformStore = store;
    }
    return store;
}

} // namespace android
