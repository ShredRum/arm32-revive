// OpenSL ES for the guest: libOpenSLES as a 32-bit guest sees it, served by
// the host's own libOpenSLES.
//
// Why: Android's native audio API. A game reaches it by import or by
// dlopen("libOpenSLES.so") + dlsym (FMOD does the latter and picks it on
// every device that reports android.hardware.audio.low_latency, i.e. most
// phones; without it, Minecraft: Story Mode waits for its sound forever).
//
// Model. An OpenSL object or interface is a pointer to a pointer to a table
// of function pointers ((*itf)->Method(itf, ...)). Each guest handle is one
// guest word holding the address of a guest table of run-time import
// trampolines named "SL:<Interface>.<Method>"; the call lands in OpenSlCall,
// which finds the host object or interface behind the handle and calls the
// host's method with the arguments translated (guest pointers become host
// pointers into guest memory; structs with pointers are rebuilt). Interface
// IDs are compared by their 16 bytes. Host callbacks (buffer queue, play,
// prefetch events) run the guest callback on the host's callback thread
// through g_call_guest_hook. Buffers handed to Enqueue stay in guest memory,
// which the host reads in place (the guest keeps them alive, as the API
// requires).
//
// Served: engine (audio player, output mix), object, play, volume, seek,
// prefetch status, buffer queue, Android simple buffer queue, Android
// configuration. Sources: (Android simple) buffer queue with PCM, Android
// fd (packed files included) and URI with MIME; sink: output mix. The rest
// (recorder, LED, vibra, MIDI, 3D, effects, metadata) answers
// SL_RESULT_FEATURE_UNSUPPORTED, as a device without them would.
#include "dispatch.h"

#include <cstring>

#if defined(__ANDROID__)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

namespace abr {

std::uint64_t (*Runtime::g_call_guest_hook)(const char*, std::uint32_t,
                                            const std::vector<std::uint32_t>&) = nullptr;

namespace {
constexpr std::uint32_t kSlSuccess = 0;
constexpr std::uint32_t kSlParameterInvalid = 2;
constexpr std::uint32_t kSlMemoryFailure = 3;
constexpr std::uint32_t kSlContentUnsupported = 9;
constexpr std::uint32_t kSlFeatureUnsupported = 12;

// Interface kinds the bridge serves, and their methods in table order
// (OpenSLES.h / OpenSLES_Android.h).
enum Kind {
    kObject,
    kEngine,
    kPlay,
    kVolume,
    kSeek,
    kPrefetch,
    kBufferQueue,
    kSimpleBufferQueue,
    kAndroidConfig,
    kKinds
};
struct KindInfo {
    const char* name;
    std::vector<const char*> methods;
};
const KindInfo kKindInfo[kKinds] = {
    {"Object",
     {"Realize", "Resume", "GetState", "GetInterface", "RegisterCallback", "AbortAsyncOperation",
      "Destroy", "SetPriority", "GetPriority", "SetLossOfControlInterfaces"}},
    {"Engine",
     {"CreateLEDDevice", "CreateVibraDevice", "CreateAudioPlayer", "CreateAudioRecorder",
      "CreateMidiPlayer", "CreateListener", "Create3DGroup", "CreateOutputMix",
      "CreateMetadataExtractor", "CreateExtensionObject", "QueryNumSupportedInterfaces",
      "QuerySupportedInterfaces", "QueryNumSupportedExtensions", "QuerySupportedExtension",
      "IsExtensionSupported"}},
    {"Play",
     {"SetPlayState", "GetPlayState", "GetDuration", "GetPosition", "RegisterCallback",
      "SetCallbackEventsMask", "GetCallbackEventsMask", "SetMarkerPosition", "ClearMarkerPosition",
      "GetMarkerPosition", "SetPositionUpdatePeriod", "GetPositionUpdatePeriod"}},
    {"Volume",
     {"SetVolumeLevel", "GetVolumeLevel", "GetMaxVolumeLevel", "SetMute", "GetMute",
      "EnableStereoPosition", "IsEnabledStereoPosition", "SetStereoPosition", "GetStereoPosition"}},
    {"Seek", {"SetPosition", "SetLoop", "GetLoop"}},
    {"PrefetchStatus",
     {"GetPrefetchStatus", "GetFillLevel", "RegisterCallback", "SetCallbackEventsMask",
      "GetCallbackEventsMask", "SetFillUpdatePeriod", "GetFillUpdatePeriod"}},
    {"BufferQueue", {"Enqueue", "Clear", "GetState", "RegisterCallback"}},
    {"AndroidSimpleBufferQueue", {"Enqueue", "Clear", "GetState", "RegisterCallback"}},
    {"AndroidConfiguration",
     {"SetConfiguration", "GetConfiguration", "AcquireJavaProxy", "ReleaseJavaProxy"}},
};

// Every interface ID name libOpenSLES exports (the guest may ask dlsym for
// any of them, served or not).
const char* const kIidNames[] = {
    "SL_IID_NULL", "SL_IID_OBJECT", "SL_IID_AUDIOIODEVICECAPABILITIES", "SL_IID_LED",
    "SL_IID_VIBRA", "SL_IID_METADATAEXTRACTION", "SL_IID_METADATATRAVERSAL",
    "SL_IID_DYNAMICSOURCE", "SL_IID_OUTPUTMIX", "SL_IID_PLAY", "SL_IID_PREFETCHSTATUS",
    "SL_IID_PLAYBACKRATE", "SL_IID_SEEK", "SL_IID_RECORD", "SL_IID_EQUALIZER", "SL_IID_VOLUME",
    "SL_IID_DEVICEVOLUME", "SL_IID_BUFFERQUEUE", "SL_IID_PRESETREVERB",
    "SL_IID_ENVIRONMENTALREVERB", "SL_IID_EFFECTSEND", "SL_IID_3DGROUPING", "SL_IID_3DCOMMIT",
    "SL_IID_3DLOCATION", "SL_IID_3DDOPPLER", "SL_IID_3DSOURCE", "SL_IID_3DMACROSCOPIC",
    "SL_IID_MUTESOLO", "SL_IID_DYNAMICINTERFACEMANAGEMENT", "SL_IID_MIDIMESSAGE",
    "SL_IID_MIDIMUTESOLO", "SL_IID_MIDITEMPO", "SL_IID_MIDITIME",
    "SL_IID_AUDIODECODERCAPABILITIES", "SL_IID_AUDIOENCODERCAPABILITIES", "SL_IID_AUDIOENCODER",
    "SL_IID_BASSBOOST", "SL_IID_PITCH", "SL_IID_RATEPITCH", "SL_IID_VIRTUALIZER",
    "SL_IID_VISUALIZATION", "SL_IID_ENGINE", "SL_IID_ENGINECAPABILITIES", "SL_IID_THREADSYNC",
    "SL_IID_ANDROIDEFFECT", "SL_IID_ANDROIDEFFECTSEND", "SL_IID_ANDROIDEFFECTCAPABILITIES",
    "SL_IID_ANDROIDCONFIGURATION", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
    "SL_IID_ANDROIDBUFFERQUEUESOURCE", "SL_IID_ANDROIDACOUSTICECHOCANCELLATION",
    "SL_IID_ANDROIDAUTOMATICGAINCONTROL", "SL_IID_ANDROIDNOISESUPPRESSION",
};
constexpr std::size_t kIids = sizeof(kIidNames) / sizeof(kIidNames[0]);
}  // namespace

struct Runtime::OpenSl {
    // A guest callback registered on a host interface: the host calls back
    // with this record as its context.
    struct Cb {
        Runtime* rt = nullptr;
        std::uint32_t fn = 0, ctx = 0, caller = 0;
    };
    struct Itf {
        Kind kind = kObject;
        void* host = nullptr;     // the host SLObjectItf / SL<X>Itf
        std::uint32_t obj = 0;    // guest handle of the object it belongs to
        std::unique_ptr<Cb> cb;   // its RegisterCallback record, if any
    };
    struct Obj {
        std::vector<std::uint32_t> itfs;  // guest interface handles made for it
        std::unique_ptr<Cb> cb;           // object callback (async Realize)
        int packed_fd = -1;               // the APK fd a packed URI source plays from
    };
    std::mutex mu;  // the maps; never held across a host OpenSL call
    std::unordered_map<std::uint32_t, Itf> itfs;  // guest handle -> interface (objects too)
    std::unordered_map<std::uint32_t, Obj> objs;  // guest object handle -> its interfaces
    std::uint32_t vtable[kKinds] = {};
    std::uint32_t iid_block = 0;  // kIids x (16-byte ID + 4-byte SL_IID_ variable)
    const void* host_iid[kIids] = {};  // the host's SLInterfaceID for each name, if it has one
};

Runtime::~Runtime() { delete sl_; }

Runtime::OpenSl& Runtime::Sl() {
    std::call_once(sl_once_, [this] {
        sl_ = new OpenSl();
        OpenSl& s = *sl_;
        for (int k = 0; k < kKinds; k++) {
            const auto& ms = kKindInfo[k].methods;
            const std::uint32_t t = GuestMalloc(static_cast<std::uint32_t>(ms.size() * 4));
            for (std::size_t i = 0; t && i < ms.size(); i++) {
                const std::uint32_t va = RuntimeImport(std::string("SL:") + kKindInfo[k].name + "." + ms[i]);
                std::memcpy(H(t + static_cast<std::uint32_t>(i) * 4), &va, 4);
            }
            s.vtable[k] = t;
        }
#if defined(__ANDROID__)
        const SLInterfaceID host[kIids] = {
            SL_IID_NULL, SL_IID_OBJECT, SL_IID_AUDIOIODEVICECAPABILITIES, SL_IID_LED, SL_IID_VIBRA,
            SL_IID_METADATAEXTRACTION, SL_IID_METADATATRAVERSAL, SL_IID_DYNAMICSOURCE,
            SL_IID_OUTPUTMIX, SL_IID_PLAY, SL_IID_PREFETCHSTATUS, SL_IID_PLAYBACKRATE, SL_IID_SEEK,
            SL_IID_RECORD, SL_IID_EQUALIZER, SL_IID_VOLUME, SL_IID_DEVICEVOLUME, SL_IID_BUFFERQUEUE,
            SL_IID_PRESETREVERB, SL_IID_ENVIRONMENTALREVERB, SL_IID_EFFECTSEND, SL_IID_3DGROUPING,
            SL_IID_3DCOMMIT, SL_IID_3DLOCATION, SL_IID_3DDOPPLER, SL_IID_3DSOURCE,
            SL_IID_3DMACROSCOPIC, SL_IID_MUTESOLO, SL_IID_DYNAMICINTERFACEMANAGEMENT,
            SL_IID_MIDIMESSAGE, SL_IID_MIDIMUTESOLO, SL_IID_MIDITEMPO, SL_IID_MIDITIME,
            SL_IID_AUDIODECODERCAPABILITIES, SL_IID_AUDIOENCODERCAPABILITIES, SL_IID_AUDIOENCODER,
            SL_IID_BASSBOOST, SL_IID_PITCH, SL_IID_RATEPITCH, SL_IID_VIRTUALIZER,
            SL_IID_VISUALIZATION, SL_IID_ENGINE, SL_IID_ENGINECAPABILITIES, SL_IID_THREADSYNC,
            SL_IID_ANDROIDEFFECT, SL_IID_ANDROIDEFFECTSEND, SL_IID_ANDROIDEFFECTCAPABILITIES,
            SL_IID_ANDROIDCONFIGURATION, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            SL_IID_ANDROIDBUFFERQUEUESOURCE, SL_IID_ANDROIDACOUSTICECHOCANCELLATION,
            SL_IID_ANDROIDAUTOMATICGAINCONTROL, SL_IID_ANDROIDNOISESUPPRESSION,
        };
        for (std::size_t i = 0; i < kIids; i++) {
            s.host_iid[i] = host[i];
        }
#endif
        s.iid_block = GuestMalloc(static_cast<std::uint32_t>(kIids * 20));
        if (s.iid_block) {
            std::memset(H(s.iid_block), 0, kIids * 20);
            for (std::size_t i = 0; i < kIids; i++) {
                const std::uint32_t id = s.iid_block + static_cast<std::uint32_t>(i * 20);
                if (s.host_iid[i]) {
                    std::memcpy(H(id), s.host_iid[i], 16);
                }
                // SL_IID_<X> is a variable holding the address of the ID.
                std::memcpy(H(id + 16), &id, 4);
            }
        }
    });
    return *sl_;
}

bool Runtime::OpenSlServes(const std::string& n) {
    return n == "slCreateEngine" || n == "slQueryNumSupportedEngineInterfaces" ||
           n == "slQuerySupportedEngineInterfaces";
}

std::uint32_t Runtime::OpenSlIid(const std::string& name) {
    OpenSl& s = Sl();
    for (std::size_t i = 0; i < kIids; i++) {
        if (name == kIidNames[i]) {
            return s.iid_block ? s.iid_block + static_cast<std::uint32_t>(i * 20) + 16 : 0;
        }
    }
    return 0;
}

#if !defined(__ANDROID__)

// No OpenSL ES on the PC: every engine is absent.
bool Runtime::OpenSlCall(const std::string& n, Regs& regs) {
    if (n.compare(0, 3, "SL:") != 0 && !OpenSlServes(n)) {
        return false;
    }
    std::printf("[sles] %s: no OpenSL ES on this host -> FEATURE_UNSUPPORTED\n", n.c_str());
    Ret(regs, kSlFeatureUnsupported);
    return true;
}

#else


namespace {

Kind KindOfIid(SLInterfaceID id, bool* served) {
    *served = true;
    if (id == SL_IID_OBJECT) return kObject;
    if (id == SL_IID_ENGINE) return kEngine;
    if (id == SL_IID_PLAY) return kPlay;
    if (id == SL_IID_VOLUME) return kVolume;
    if (id == SL_IID_SEEK) return kSeek;
    if (id == SL_IID_PREFETCHSTATUS) return kPrefetch;
    if (id == SL_IID_BUFFERQUEUE) return kBufferQueue;
    if (id == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) return kSimpleBufferQueue;
    if (id == SL_IID_ANDROIDCONFIGURATION) return kAndroidConfig;
    *served = false;
    return kObject;
}

}  // namespace

bool Runtime::OpenSlCall(const std::string& n, Regs& regs) {
    const bool method = n.compare(0, 3, "SL:") == 0;
    if (!method && !OpenSlServes(n)) {
        return false;
    }
    OpenSl& s = Sl();
    // Argument i of the call (AAPCS32: r0-r3, then the stack).
    auto arg = [&](int i) -> std::uint32_t {
        if (i < 4) return regs[static_cast<std::size_t>(i)];
        const std::uint32_t at = regs[13] + static_cast<std::uint32_t>(i - 4) * 4;
        std::uint32_t v = 0;
        if (CheckRange(at, 4, false)) std::memcpy(&v, H(at), 4);
        return v;
    };
    auto put32 = [&](std::uint32_t at, std::uint32_t v) -> bool {
        if (!at || !CheckRange(at, 4, true)) return false;
        std::memcpy(H(at), &v, 4);
        return true;
    };
    auto put16 = [&](std::uint32_t at, std::uint16_t v) -> bool {
        if (!at || !CheckRange(at, 2, true)) return false;
        std::memcpy(H(at), &v, 2);
        return true;
    };
    // The host interface ID matching a guest SLInterfaceID (its 16 bytes),
    // with its index in kIidNames; null for a bad pointer or an unknown ID.
    auto host_iid = [&](std::uint32_t gid, int* index) -> SLInterfaceID {
        *index = -1;
        if (!gid || !CheckRange(gid, 16, false)) return nullptr;
        const void* g = H(gid);
        for (std::size_t i = 0; i < kIids; i++) {
            if (s.host_iid[i] && std::memcmp(s.host_iid[i], g, 16) == 0) {
                *index = static_cast<int>(i);
                return static_cast<SLInterfaceID>(s.host_iid[i]);
            }
        }
        return nullptr;
    };
    auto ret = [&](SLresult r) {
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    };
    // A new guest handle for host interface `host` of kind k (an object
    // when k is kObject), owned by guest object `obj` (itself for objects).
    auto make_handle = [&](Kind k, const void* host, std::uint32_t obj) -> std::uint32_t {
        const std::uint32_t h = GuestMalloc(4);
        if (!h) return 0;
        std::memcpy(H(h), &s.vtable[k], 4);
        std::lock_guard<std::mutex> l(s.mu);
        OpenSl::Itf it;
        it.kind = k;
        it.host = const_cast<void*>(host);  // SL<X>Itf are pointers to const
        it.obj = k == kObject ? h : obj;
        s.itfs[h] = std::move(it);
        if (k == kObject) {
            s.objs[h];
        } else {
            s.objs[obj].itfs.push_back(h);
        }
        return h;
    };
    auto host_of = [&](std::uint32_t h, Kind want) -> void* {
        std::lock_guard<std::mutex> l(s.mu);
        const auto it = s.itfs.find(h);
        return it != s.itfs.end() && it->second.kind == want ? it->second.host : nullptr;
    };
    // The translated interface lists of a Create* call.
    struct IidList {
        std::vector<SLInterfaceID> ids;
        std::vector<SLboolean> req;
    };
    auto iid_list = [&](std::uint32_t num, std::uint32_t gids, std::uint32_t greq,
                        IidList* out) -> SLresult {
        if (num > 64 || (num && (!CheckRange(gids, num * 4, false) ||
                                 (greq && !CheckRange(greq, num * 4, false))))) {
            return kSlParameterInvalid;
        }
        for (std::uint32_t i = 0; i < num; i++) {
            std::uint32_t gid = 0, required = 0;
            std::memcpy(&gid, H(gids + i * 4), 4);
            if (greq) std::memcpy(&required, H(greq + i * 4), 4);
            int index = -1;
            const SLInterfaceID id = host_iid(gid, &index);
            bool served = false;
            if (id) KindOfIid(id, &served);
            if (!served) {
                // An interface the bridge has no table for: fail if the
                // guest requires it, drop it otherwise.
                std::printf("[sles] interface %s not served (%s)\n",
                            index >= 0 ? kIidNames[index] : "unknown", required ? "required" : "dropped");
                if (required) return kSlFeatureUnsupported;
                continue;
            }
            out->ids.push_back(id);
            out->req.push_back(required ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE);
        }
        return kSlSuccess;
    };

    if (!method) {
        if (n == "slCreateEngine") {
            // (SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption*,
            //  SLuint32 numInterfaces, const SLInterfaceID*, const SLboolean*)
            const std::uint32_t out = arg(0), nopt = arg(1), gopt = arg(2);
            if (!out || !CheckRange(out, 4, true) || nopt > 16 ||
                (nopt && !CheckRange(gopt, nopt * 8, false))) {
                return ret(kSlParameterInvalid);
            }
            std::vector<SLEngineOption> opts(nopt);
            for (std::uint32_t i = 0; i < nopt; i++) {
                std::memcpy(&opts[i].feature, H(gopt + i * 8), 4);
                std::memcpy(&opts[i].data, H(gopt + i * 8 + 4), 4);
            }
            IidList l;
            if (const SLresult r = iid_list(arg(3), arg(4), arg(5), &l); r != kSlSuccess) return ret(r);
            SLObjectItf eng = nullptr;
            const SLresult r = slCreateEngine(&eng, nopt, nopt ? opts.data() : nullptr,
                                              static_cast<SLuint32>(l.ids.size()), l.ids.data(),
                                              l.req.data());
            if (r != SL_RESULT_SUCCESS) return ret(r);
            const std::uint32_t h = make_handle(kObject, eng, 0);
            if (!h) {
                (*eng)->Destroy(eng);
                return ret(kSlMemoryFailure);
            }
            put32(out, h);
            return ret(SL_RESULT_SUCCESS);
        }
        if (n == "slQueryNumSupportedEngineInterfaces") {
            SLuint32 num = 0;
            const SLresult r = slQueryNumSupportedEngineInterfaces(&num);
            put32(arg(0), num);
            return ret(r);
        }
        // slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID* pInterfaceId)
        SLInterfaceID id = nullptr;
        const SLresult r = slQuerySupportedEngineInterfaces(arg(0), &id);
        std::uint32_t gid = 0;
        for (std::size_t i = 0; r == SL_RESULT_SUCCESS && i < kIids && s.iid_block; i++) {
            if (s.host_iid[i] && id && std::memcmp(s.host_iid[i], id, 16) == 0) {
                gid = s.iid_block + static_cast<std::uint32_t>(i * 20);
            }
        }
        put32(arg(1), gid);
        return ret(r);
    }

    // "SL:<Kind>.<Method>": find the kind and the method's table index.
    const std::size_t dot = n.find('.', 3);
    const std::string kname = n.substr(3, dot - 3), mname = n.substr(dot + 1);
    int k = -1, m = -1;
    for (int i = 0; i < kKinds && k < 0; i++) {
        if (kname == kKindInfo[i].name) {
            k = i;
            for (std::size_t j = 0; j < kKindInfo[i].methods.size(); j++) {
                if (mname == kKindInfo[i].methods[j]) m = static_cast<int>(j);
            }
        }
    }
    const std::uint32_t self = arg(0);
    void* hp = (k >= 0 && m >= 0) ? host_of(self, static_cast<Kind>(k)) : nullptr;
    if (!hp) {
        std::printf("[sles] %s on an unknown handle %08x -> PARAMETER_INVALID\n", n.c_str(), self);
        return ret(kSlParameterInvalid);
    }

    switch (static_cast<Kind>(k)) {
    case kObject: {
        SLObjectItf o = static_cast<SLObjectItf>(hp);
        switch (m) {
        case 0: {  // Realize(self, async): done here synchronously; an async
                   // caller gets its completion callback right after.
            const SLresult r = (*o)->Realize(o, SL_BOOLEAN_FALSE);
            Ret(regs, r);
            if (arg(1)) {
                OpenSl::Cb cb;
                {
                    std::lock_guard<std::mutex> l(s.mu);
                    const auto it = s.objs.find(self);
                    if (it != s.objs.end() && it->second.cb) cb = *it->second.cb;
                }
                if (cb.fn && g_call_guest_hook) {
                    // slObjectCallback(caller, pContext, event, result, param, pInterface)
                    g_call_guest_hook("SL object callback", cb.fn,
                                      {self, cb.ctx, SL_OBJECT_EVENT_ASYNC_TERMINATION,
                                       static_cast<std::uint32_t>(r), SL_OBJECT_STATE_REALIZED, 0});
                    Ret(regs, r);
                }
            }
            return true;
        }
        case 1:
            return ret((*o)->Resume(o, SL_BOOLEAN_FALSE));
        case 2: {
            SLuint32 st = 0;
            const SLresult r = (*o)->GetState(o, &st);
            put32(arg(1), st);
            return ret(r);
        }
        case 3: {  // GetInterface(self, iid, void* pInterface)
            int index = -1;
            const SLInterfaceID id = host_iid(arg(1), &index);
            bool served = false;
            const Kind kind = id ? KindOfIid(id, &served) : kObject;
            if (!served || kind == kObject) {
                return ret(kSlFeatureUnsupported);
            }
            {  // the same handle for the same interface, as the host does
                std::lock_guard<std::mutex> l(s.mu);
                for (std::uint32_t h : s.objs[self].itfs) {
                    if (s.itfs[h].kind == kind) {
                        put32(arg(2), h);
                        return ret(SL_RESULT_SUCCESS);
                    }
                }
            }
            void* itf = nullptr;
            const SLresult r = (*o)->GetInterface(o, id, &itf);
            if (r != SL_RESULT_SUCCESS) return ret(r);
            const std::uint32_t h = make_handle(kind, itf, self);
            if (!h) return ret(kSlMemoryFailure);
            put32(arg(2), h);
            return ret(SL_RESULT_SUCCESS);
        }
        case 4: {  // RegisterCallback(self, callback, pContext): used for async Realize
            std::lock_guard<std::mutex> l(s.mu);
            auto& slot = s.objs[self].cb;
            if (!slot) slot = std::make_unique<OpenSl::Cb>();
            slot->rt = this;
            slot->fn = arg(1);
            slot->ctx = arg(2);
            slot->caller = self;
            return ret(SL_RESULT_SUCCESS);
        }
        case 5:
            (*o)->AbortAsyncOperation(o);
            return ret(SL_RESULT_SUCCESS);
        case 6: {  // Destroy: the host waits for its callbacks; then the handles go
            (*o)->Destroy(o);
            std::vector<std::uint32_t> gone;
            int packed_fd = -1;
            {
                std::lock_guard<std::mutex> l(s.mu);
                const auto it = s.objs.find(self);
                if (it != s.objs.end()) {
                    gone = it->second.itfs;
                    packed_fd = it->second.packed_fd;
                    s.objs.erase(it);
                }
                gone.push_back(self);
                for (std::uint32_t h : gone) s.itfs.erase(h);
            }
            for (std::uint32_t h : gone) GuestFree(h);
            if (packed_fd >= 0) ::close(packed_fd);  // the host player is gone
            return ret(SL_RESULT_SUCCESS);
        }
        case 7:
            return ret((*o)->SetPriority(o, static_cast<SLint32>(arg(1)), arg(2)));
        case 8: {
            SLint32 pri = 0;
            SLboolean pre = 0;
            const SLresult r = (*o)->GetPriority(o, &pri, &pre);
            put32(arg(1), static_cast<std::uint32_t>(pri));
            put32(arg(2), pre);
            return ret(r);
        }
        default:
            return ret(kSlFeatureUnsupported);  // SetLossOfControlInterfaces
        }
    }
    case kEngine: {
        SLEngineItf e = static_cast<SLEngineItf>(hp);
        switch (m) {
        case 2: {  // CreateAudioPlayer(self, pPlayer, pAudioSrc, pAudioSnk, num, ids, req)
            const std::uint32_t out = arg(1), gsrc = arg(2), gsnk = arg(3);
            if (!out || !CheckRange(out, 4, true) || !CheckRange(gsrc, 8, false) ||
                !CheckRange(gsnk, 8, false)) {
                return ret(kSlParameterInvalid);
            }
            IidList l;
            if (const SLresult r = iid_list(arg(4), arg(5), arg(6), &l); r != kSlSuccess) return ret(r);
            std::uint32_t sloc = 0, sfmt = 0, kloc = 0;
            std::memcpy(&sloc, H(gsrc), 4);
            std::memcpy(&sfmt, H(gsrc + 4), 4);
            std::memcpy(&kloc, H(gsnk), 4);
            if (!CheckRange(sloc, 4, false) || !CheckRange(kloc, 8, false)) {
                return ret(kSlParameterInvalid);
            }
            // Source locator.
            SLDataLocator_AndroidSimpleBufferQueue loc_sbq{};
            SLDataLocator_BufferQueue loc_bq{};
            SLDataLocator_AndroidFD loc_fd{};
            SLDataLocator_URI loc_uri{};
            std::string uri;
            void* hloc = nullptr;
            // The APK fd of a packed URI source: closed on every early return,
            // handed to the player's record once it exists.
            struct FdOwner {
                int fd = -1;
                ~FdOwner() {
                    if (fd >= 0) ::close(fd);
                }
            } packed;
            int& packed_fd = packed.fd;
            std::uint32_t ltype = 0;
            std::memcpy(&ltype, H(sloc), 4);
            if ((ltype == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ||
                 ltype == SL_DATALOCATOR_BUFFERQUEUE) &&
                CheckRange(sloc, 8, false)) {
                std::uint32_t nb = 0;
                std::memcpy(&nb, H(sloc + 4), 4);
                if (ltype == SL_DATALOCATOR_BUFFERQUEUE) {
                    loc_bq = {SL_DATALOCATOR_BUFFERQUEUE, nb};
                    hloc = &loc_bq;
                } else {
                    loc_sbq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, nb};
                    hloc = &loc_sbq;
                }
            } else if (ltype == SL_DATALOCATOR_ANDROIDFD && CheckRange(sloc, 24, false)) {
                std::int32_t gfd = 0;
                std::int64_t off = 0, len = 0;
                std::memcpy(&gfd, H(sloc + 4), 4);
                std::memcpy(&off, H(sloc + 8), 8);
                std::memcpy(&len, H(sloc + 16), 8);
                int hfd = -1;
                {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    const auto it = fd_map_.find(static_cast<std::uint32_t>(gfd));
                    if (it != fd_map_.end()) hfd = it->second;
                }
                if (hfd < 0) return ret(kSlParameterInvalid);
                // A packed file: its window inside the APK.
                if (const std::shared_ptr<abvfs::Slice> sl = FdSlice(static_cast<std::uint32_t>(gfd))) {
                    if (len == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE) {
                        len = static_cast<std::int64_t>(sl->size) - off;
                    }
                    off += static_cast<std::int64_t>(sl->off);
                }
                loc_fd = {SL_DATALOCATOR_ANDROIDFD, hfd, off, len};
                hloc = &loc_fd;
            } else if (ltype == SL_DATALOCATOR_URI && CheckRange(sloc, 8, false)) {
                std::uint32_t gp = 0;
                std::memcpy(&gp, H(sloc + 4), 4);
                std::string g = CheckRange(gp, 1, false) ? GuestStr(gp) : "";
                // A file URI or path is the guest's path: mapped to the host's.
                const bool file = g.compare(0, 7, "file://") == 0;
                if (file || (!g.empty() && g[0] == '/')) {
                    const std::string hp2 = MapGuestPathString(file ? g.substr(7) : g);
                    if (hp2.empty()) return ret(SL_RESULT_CONTENT_NOT_FOUND);
                    struct stat rst {};
                    const abvfs::File* pf =
                        vfs_on_ && ::lstat(hp2.c_str(), &rst) != 0 ? vfs_.Find(hp2) : nullptr;
                    if (pf) {
                        // A packed file has no host path: its window in the
                        // APK, as for an Android FD source (fd kept open for
                        // the player's lifetime, closed at Destroy).
                        std::shared_ptr<abvfs::Slice> sl;
                        packed_fd = vfs_.Open(*pf, &sl);
                        if (packed_fd < 0) return ret(SL_RESULT_CONTENT_NOT_FOUND);
                        loc_fd = {SL_DATALOCATOR_ANDROIDFD, packed_fd, static_cast<SLAint64>(sl->off),
                                  static_cast<SLAint64>(sl->size)};
                        hloc = &loc_fd;
                    }
                    uri = "file://" + hp2;
                } else {
                    uri = g;  // a network URL as given
                }
                if (packed_fd < 0) {
                    loc_uri = {SL_DATALOCATOR_URI,
                               reinterpret_cast<SLchar*>(const_cast<char*>(uri.c_str()))};
                    hloc = &loc_uri;
                }
            } else {
                std::printf("[sles] CreateAudioPlayer: source locator %08x not served\n", ltype);
                return ret(kSlContentUnsupported);
            }
            // Source format.
            SLDataFormat_PCM fmt_pcm{};
            SLAndroidDataFormat_PCM_EX fmt_ex{};
            SLDataFormat_MIME fmt_mime{};
            std::string mime;
            void* hfmt = nullptr;
            if (sfmt) {
                std::uint32_t ftype = 0;
                if (!CheckRange(sfmt, 4, false)) return ret(kSlParameterInvalid);
                std::memcpy(&ftype, H(sfmt), 4);
                if (ftype == SL_DATAFORMAT_PCM && CheckRange(sfmt, sizeof fmt_pcm, false)) {
                    std::memcpy(&fmt_pcm, H(sfmt), sizeof fmt_pcm);  // seven SLuint32
                    hfmt = &fmt_pcm;
                } else if (ftype == SL_ANDROID_DATAFORMAT_PCM_EX &&
                           CheckRange(sfmt, sizeof fmt_ex, false)) {
                    std::memcpy(&fmt_ex, H(sfmt), sizeof fmt_ex);  // eight SLuint32
                    hfmt = &fmt_ex;
                } else if (ftype == SL_DATAFORMAT_MIME && CheckRange(sfmt, 12, false)) {
                    std::uint32_t gm = 0, ct = 0;
                    std::memcpy(&gm, H(sfmt + 4), 4);
                    std::memcpy(&ct, H(sfmt + 8), 4);
                    if (gm && CheckRange(gm, 1, false)) mime = GuestStr(gm);
                    fmt_mime = {SL_DATAFORMAT_MIME,
                                gm ? reinterpret_cast<SLchar*>(const_cast<char*>(mime.c_str())) : nullptr,
                                ct};
                    hfmt = &fmt_mime;
                } else {
                    std::printf("[sles] CreateAudioPlayer: source format %08x not served\n", ftype);
                    return ret(kSlContentUnsupported);
                }
            }
            // Sink: an output mix.
            std::uint32_t ktype = 0, gmix = 0;
            std::memcpy(&ktype, H(kloc), 4);
            std::memcpy(&gmix, H(kloc + 4), 4);
            void* mix = ktype == SL_DATALOCATOR_OUTPUTMIX ? host_of(gmix, kObject) : nullptr;
            if (!mix) {
                std::printf("[sles] CreateAudioPlayer: sink locator %08x not served\n", ktype);
                return ret(kSlContentUnsupported);
            }
            SLDataLocator_OutputMix loc_mix{SL_DATALOCATOR_OUTPUTMIX, static_cast<SLObjectItf>(mix)};
            SLDataSource src{hloc, hfmt};
            SLDataSink snk{&loc_mix, nullptr};
            SLObjectItf player = nullptr;
            const SLresult r = (*e)->CreateAudioPlayer(e, &player, &src, &snk,
                                                       static_cast<SLuint32>(l.ids.size()),
                                                       l.ids.data(), l.req.data());
            if (r != SL_RESULT_SUCCESS) return ret(r);
            const std::uint32_t h = make_handle(kObject, player, 0);
            if (!h) {
                (*player)->Destroy(player);
                return ret(kSlMemoryFailure);
            }
            if (packed_fd >= 0) {
                std::lock_guard<std::mutex> l(s.mu);
                s.objs[h].packed_fd = packed_fd;
                packed_fd = -1;  // the record owns it now
            }
            put32(out, h);
            return ret(SL_RESULT_SUCCESS);
        }
        case 7: {  // CreateOutputMix(self, pMix, num, ids, req)
            const std::uint32_t out = arg(1);
            if (!out || !CheckRange(out, 4, true)) return ret(kSlParameterInvalid);
            IidList l;
            if (const SLresult r = iid_list(arg(2), arg(3), arg(4), &l); r != kSlSuccess) return ret(r);
            SLObjectItf mix = nullptr;
            const SLresult r = (*e)->CreateOutputMix(e, &mix, static_cast<SLuint32>(l.ids.size()),
                                                     l.ids.data(), l.req.data());
            if (r != SL_RESULT_SUCCESS) return ret(r);
            const std::uint32_t h = make_handle(kObject, mix, 0);
            if (!h) {
                (*mix)->Destroy(mix);
                return ret(kSlMemoryFailure);
            }
            put32(out, h);
            return ret(SL_RESULT_SUCCESS);
        }
        case 10: {  // QueryNumSupportedInterfaces(self, objectID, SLuint32*)
            SLuint32 num = 0;
            const SLresult r = (*e)->QueryNumSupportedInterfaces(e, arg(1), &num);
            put32(arg(2), num);
            return ret(r);
        }
        case 11: {  // QuerySupportedInterfaces(self, objectID, index, SLInterfaceID*)
            SLInterfaceID id = nullptr;
            const SLresult r = (*e)->QuerySupportedInterfaces(e, arg(1), arg(2), &id);
            std::uint32_t gid = 0;
            for (std::size_t i = 0; r == SL_RESULT_SUCCESS && i < kIids && s.iid_block; i++) {
                if (s.host_iid[i] && id && std::memcmp(s.host_iid[i], id, 16) == 0) {
                    gid = s.iid_block + static_cast<std::uint32_t>(i * 20);
                }
            }
            put32(arg(3), gid);
            return ret(r);
        }
        case 12: {  // QueryNumSupportedExtensions(self, SLuint32*)
            SLuint32 num = 0;
            const SLresult r = (*e)->QueryNumSupportedExtensions(e, &num);
            put32(arg(1), num);
            return ret(r);
        }
        case 13: {  // QuerySupportedExtension(self, index, SLchar* name, SLint16* pNameLength)
            const std::uint32_t gname = arg(2), glen = arg(3);
            if (!glen || !CheckRange(glen, 2, true)) return ret(kSlParameterInvalid);
            SLint16 len = 0;
            std::memcpy(&len, H(glen), 2);
            if (gname && (len <= 0 || !CheckRange(gname, static_cast<std::size_t>(len), true))) {
                return ret(kSlParameterInvalid);
            }
            const SLresult r = (*e)->QuerySupportedExtension(
                e, arg(1), gname ? reinterpret_cast<SLchar*>(H(gname)) : nullptr, &len);
            put16(glen, static_cast<std::uint16_t>(len));
            return ret(r);
        }
        case 14: {  // IsExtensionSupported(self, const SLchar* name, SLboolean*)
            const std::uint32_t gname = arg(1);
            if (!gname || !CheckRange(gname, 1, false)) return ret(kSlParameterInvalid);
            SLboolean sup = SL_BOOLEAN_FALSE;
            const SLresult r = (*e)->IsExtensionSupported(
                e, reinterpret_cast<const SLchar*>(GuestStr(gname)), &sup);
            put32(arg(2), sup);
            return ret(r);
        }
        default:
            std::printf("[sles] Engine.%s not served -> FEATURE_UNSUPPORTED\n", mname.c_str());
            return ret(kSlFeatureUnsupported);
        }
    }
    case kPlay: {
        SLPlayItf p = static_cast<SLPlayItf>(hp);
        SLuint32 v = 0;
        SLresult r = SL_RESULT_SUCCESS;
        switch (m) {
        case 0:
            return ret((*p)->SetPlayState(p, arg(1)));
        case 1:
            r = (*p)->GetPlayState(p, &v);
            put32(arg(1), v);
            return ret(r);
        case 2:
            r = (*p)->GetDuration(p, &v);
            put32(arg(1), v);
            return ret(r);
        case 3:
            r = (*p)->GetPosition(p, &v);
            put32(arg(1), v);
            return ret(r);
        case 4: {  // RegisterCallback(self, slPlayCallback(caller, ctx, event), ctx)
            OpenSl::Cb* cb = nullptr;
            {
                std::lock_guard<std::mutex> l(s.mu);
                auto& slot = s.itfs[self].cb;
                if (!slot) slot = std::make_unique<OpenSl::Cb>();
                cb = slot.get();
            }
            cb->rt = this;
            cb->fn = arg(1);
            cb->ctx = arg(2);
            cb->caller = self;
            auto host_cb = [](SLPlayItf, void* c, SLuint32 event) {
                auto* k = static_cast<OpenSl::Cb*>(c);
                if (k->fn && g_call_guest_hook) {
                    g_call_guest_hook("SL play callback", k->fn, {k->caller, k->ctx, event});
                }
            };
            return ret((*p)->RegisterCallback(p, arg(1) ? +host_cb : nullptr, cb));
        }
        case 5:
            return ret((*p)->SetCallbackEventsMask(p, arg(1)));
        case 6:
            r = (*p)->GetCallbackEventsMask(p, &v);
            put32(arg(1), v);
            return ret(r);
        case 7:
            return ret((*p)->SetMarkerPosition(p, arg(1)));
        case 8:
            return ret((*p)->ClearMarkerPosition(p));
        case 9:
            r = (*p)->GetMarkerPosition(p, &v);
            put32(arg(1), v);
            return ret(r);
        case 10:
            return ret((*p)->SetPositionUpdatePeriod(p, arg(1)));
        default:
            r = (*p)->GetPositionUpdatePeriod(p, &v);
            put32(arg(1), v);
            return ret(r);
        }
    }
    case kVolume: {
        SLVolumeItf v = static_cast<SLVolumeItf>(hp);
        SLmillibel mb = 0;
        SLboolean b = 0;
        SLpermille pm = 0;
        SLresult r = SL_RESULT_SUCCESS;
        switch (m) {
        case 0:
            return ret((*v)->SetVolumeLevel(v, static_cast<SLmillibel>(arg(1))));
        case 1:
            r = (*v)->GetVolumeLevel(v, &mb);
            put16(arg(1), static_cast<std::uint16_t>(mb));
            return ret(r);
        case 2:
            r = (*v)->GetMaxVolumeLevel(v, &mb);
            put16(arg(1), static_cast<std::uint16_t>(mb));
            return ret(r);
        case 3:
            return ret((*v)->SetMute(v, arg(1)));
        case 4:
            r = (*v)->GetMute(v, &b);
            put32(arg(1), b);
            return ret(r);
        case 5:
            return ret((*v)->EnableStereoPosition(v, arg(1)));
        case 6:
            r = (*v)->IsEnabledStereoPosition(v, &b);
            put32(arg(1), b);
            return ret(r);
        case 7:
            return ret((*v)->SetStereoPosition(v, static_cast<SLpermille>(arg(1))));
        default:
            r = (*v)->GetStereoPosition(v, &pm);
            put16(arg(1), static_cast<std::uint16_t>(pm));
            return ret(r);
        }
    }
    case kSeek: {
        SLSeekItf sk = static_cast<SLSeekItf>(hp);
        if (m == 0) return ret((*sk)->SetPosition(sk, arg(1), arg(2)));
        if (m == 1) return ret((*sk)->SetLoop(sk, arg(1), arg(2), arg(3)));
        SLboolean en = 0;
        SLmillisecond a = 0, b = 0;
        const SLresult r = (*sk)->GetLoop(sk, &en, &a, &b);
        put32(arg(1), en);
        put32(arg(2), a);
        put32(arg(3), b);
        return ret(r);
    }
    case kPrefetch: {
        SLPrefetchStatusItf pf = static_cast<SLPrefetchStatusItf>(hp);
        SLuint32 u = 0;
        SLpermille pm = 0;
        SLresult r = SL_RESULT_SUCCESS;
        switch (m) {
        case 0:
            r = (*pf)->GetPrefetchStatus(pf, &u);
            put32(arg(1), u);
            return ret(r);
        case 1:
            r = (*pf)->GetFillLevel(pf, &pm);
            put16(arg(1), static_cast<std::uint16_t>(pm));
            return ret(r);
        case 2: {  // RegisterCallback(self, slPrefetchCallback(caller, ctx, event), ctx)
            OpenSl::Cb* cb = nullptr;
            {
                std::lock_guard<std::mutex> l(s.mu);
                auto& slot = s.itfs[self].cb;
                if (!slot) slot = std::make_unique<OpenSl::Cb>();
                cb = slot.get();
            }
            cb->rt = this;
            cb->fn = arg(1);
            cb->ctx = arg(2);
            cb->caller = self;
            auto host_cb = [](SLPrefetchStatusItf, void* c, SLuint32 event) {
                auto* k = static_cast<OpenSl::Cb*>(c);
                if (k->fn && g_call_guest_hook) {
                    g_call_guest_hook("SL prefetch callback", k->fn, {k->caller, k->ctx, event});
                }
            };
            return ret((*pf)->RegisterCallback(pf, arg(1) ? +host_cb : nullptr, cb));
        }
        case 3:
            return ret((*pf)->SetCallbackEventsMask(pf, arg(1)));
        case 4:
            r = (*pf)->GetCallbackEventsMask(pf, &u);
            put32(arg(1), u);
            return ret(r);
        case 5:
            return ret((*pf)->SetFillUpdatePeriod(pf, static_cast<SLpermille>(arg(1))));
        default:
            r = (*pf)->GetFillUpdatePeriod(pf, &pm);
            put16(arg(1), static_cast<std::uint16_t>(pm));
            return ret(r);
        }
    }
    case kBufferQueue:
    case kSimpleBufferQueue: {
        // Both tables are {Enqueue, Clear, GetState, RegisterCallback} with
        // the same argument shapes and a two-word state.
        const bool simple = k == kSimpleBufferQueue;
        SLAndroidSimpleBufferQueueItf sq = static_cast<SLAndroidSimpleBufferQueueItf>(hp);
        SLBufferQueueItf bq = static_cast<SLBufferQueueItf>(hp);
        switch (m) {
        case 0: {  // Enqueue(self, const void* pBuffer, SLuint32 size): read in place
            const std::uint32_t buf = arg(1), size = arg(2);
            if (size && !CheckRange(buf, size, false)) return ret(kSlParameterInvalid);
            const void* p = size ? static_cast<const void*>(H(buf)) : nullptr;
            return ret(simple ? (*sq)->Enqueue(sq, p, size) : (*bq)->Enqueue(bq, p, size));
        }
        case 1:
            return ret(simple ? (*sq)->Clear(sq) : (*bq)->Clear(bq));
        case 2: {
            std::uint32_t w[2] = {0, 0};
            SLresult r;
            if (simple) {
                SLAndroidSimpleBufferQueueState st{};
                r = (*sq)->GetState(sq, &st);
                w[0] = st.count;
                w[1] = st.index;
            } else {
                SLBufferQueueState st{};
                r = (*bq)->GetState(bq, &st);
                w[0] = st.count;
                w[1] = st.playIndex;
            }
            if (!arg(1) || !CheckRange(arg(1), 8, true)) return ret(kSlParameterInvalid);
            std::memcpy(H(arg(1)), w, 8);
            return ret(r);
        }
        default: {  // RegisterCallback(self, callback(caller, ctx), ctx)
            OpenSl::Cb* cb = nullptr;
            {
                std::lock_guard<std::mutex> l(s.mu);
                auto& slot = s.itfs[self].cb;
                if (!slot) slot = std::make_unique<OpenSl::Cb>();
                cb = slot.get();
            }
            cb->rt = this;
            cb->fn = arg(1);
            cb->ctx = arg(2);
            cb->caller = self;
            if (simple) {
                slAndroidSimpleBufferQueueCallback f = nullptr;
                if (arg(1)) {
                    f = [](SLAndroidSimpleBufferQueueItf, void* c) {
                        auto* k = static_cast<OpenSl::Cb*>(c);
                        if (k->fn && g_call_guest_hook) {
                            g_call_guest_hook("SL buffer queue callback", k->fn, {k->caller, k->ctx});
                        }
                    };
                }
                return ret((*sq)->RegisterCallback(sq, f, cb));
            }
            slBufferQueueCallback f = nullptr;
            if (arg(1)) {
                f = [](SLBufferQueueItf, void* c) {
                    auto* k = static_cast<OpenSl::Cb*>(c);
                    if (k->fn && g_call_guest_hook) {
                        g_call_guest_hook("SL buffer queue callback", k->fn, {k->caller, k->ctx});
                    }
                };
            }
            return ret((*bq)->RegisterCallback(bq, f, cb));
        }
        }
    }
    case kAndroidConfig: {
        SLAndroidConfigurationItf c = static_cast<SLAndroidConfigurationItf>(hp);
        if (m == 0) {  // SetConfiguration(self, const SLchar* key, const void* value, SLuint32 size)
            const std::uint32_t gkey = arg(1), gval = arg(2), size = arg(3);
            if (!gkey || !CheckRange(gkey, 1, false) || (size && !CheckRange(gval, size, false))) {
                return ret(kSlParameterInvalid);
            }
            return ret((*c)->SetConfiguration(c, reinterpret_cast<const SLchar*>(GuestStr(gkey)),
                                              size ? H(gval) : nullptr, size));
        }
        if (m == 1) {  // GetConfiguration(self, key, SLuint32* pValueSize, void* pConfigValue)
            const std::uint32_t gkey = arg(1), gsize = arg(2), gval = arg(3);
            if (!gkey || !CheckRange(gkey, 1, false) || !gsize || !CheckRange(gsize, 4, true)) {
                return ret(kSlParameterInvalid);
            }
            SLuint32 size = 0;
            std::memcpy(&size, H(gsize), 4);
            if (gval && size && !CheckRange(gval, size, true)) return ret(kSlParameterInvalid);
            const SLresult r = (*c)->GetConfiguration(c, reinterpret_cast<const SLchar*>(GuestStr(gkey)),
                                                      &size, gval ? H(gval) : nullptr);
            put32(gsize, size);
            return ret(r);
        }
        // AcquireJavaProxy / ReleaseJavaProxy: a host jobject has no guest form.
        return ret(kSlFeatureUnsupported);
    }
    default:
        return ret(kSlFeatureUnsupported);
    }
}


#endif  // __ANDROID__

}  // namespace abr
