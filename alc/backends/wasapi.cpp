/**
 * OpenAL cross platform audio library
 * Copyright (C) 2011 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "wasapi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include <wtypes.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cguid.h>
#include <devpropdef.h>
#include <mmreg.h>
#include <propsys.h>
#include <propkey.h>
#include <devpkey.h>
#ifndef _WAVEFORMATEXTENSIBLE_
#include <ks.h>
#include <ksmedia.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "albit.h"
#include "alc/alconfig.h"
#include "alc/events.h"
#include "alnumeric.h"
#include "alspan.h"
#include "althrd_setname.h"
#include "comptr.h"
#include "core/converter.h"
#include "core/device.h"
#include "core/helpers.h"
#include "core/logging.h"
#include "ringbuffer.h"
#include "strutils.h"

#if defined(ALSOFT_UWP)
#include <collection.h>
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Media::Devices;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Media::Devices;
#endif

/* Some headers seem to define these as macros for __uuidof, which is annoying
 * since some headers don't declare them at all. Hopefully the ifdef is enough
 * to tell if they need to be declared.
 */
#ifndef KSDATAFORMAT_SUBTYPE_PCM
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
#if !defined(ALSOFT_UWP)
DEFINE_DEVPROPKEY(DEVPKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80,0x20, 0x67,0xd1,0x46,0xa8,0x50,0xe0, 14);
DEFINE_PROPERTYKEY(PKEY_AudioEndpoint_FormFactor, 0x1da5d803, 0xd492, 0x4edd, 0x8c,0x23, 0xe0,0xc0,0xff,0xee,0x7f,0x0e, 0);
DEFINE_PROPERTYKEY(PKEY_AudioEndpoint_GUID, 0x1da5d803, 0xd492, 0x4edd, 0x8c, 0x23,0xe0, 0xc0,0xff,0xee,0x7f,0x0e, 4 );
#endif

namespace {

using std::chrono::nanoseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

using ReferenceTime = std::chrono::duration<REFERENCE_TIME,std::ratio<1,10000000>>;

inline constexpr ReferenceTime operator "" _reftime(unsigned long long int n) noexcept
{ return ReferenceTime{static_cast<REFERENCE_TIME>(n)}; }


#define MONO SPEAKER_FRONT_CENTER
#define STEREO (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT)
#define QUAD (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X5DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X5DOT1REAR (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT)
#define X6DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_CENTER|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT)
#define X7DOT1DOT4 (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT|SPEAKER_TOP_FRONT_LEFT|SPEAKER_TOP_FRONT_RIGHT|SPEAKER_TOP_BACK_LEFT|SPEAKER_TOP_BACK_RIGHT)

constexpr inline DWORD MaskFromTopBits(DWORD b) noexcept
{
    b |= b>>1;
    b |= b>>2;
    b |= b>>4;
    b |= b>>8;
    b |= b>>16;
    return b;
}
constexpr DWORD MonoMask{MaskFromTopBits(MONO)};
constexpr DWORD StereoMask{MaskFromTopBits(STEREO)};
constexpr DWORD QuadMask{MaskFromTopBits(QUAD)};
constexpr DWORD X51Mask{MaskFromTopBits(X5DOT1)};
constexpr DWORD X51RearMask{MaskFromTopBits(X5DOT1REAR)};
constexpr DWORD X61Mask{MaskFromTopBits(X6DOT1)};
constexpr DWORD X71Mask{MaskFromTopBits(X7DOT1)};
constexpr DWORD X714Mask{MaskFromTopBits(X7DOT1DOT4)};

constexpr char DevNameHead[] = "OpenAL Soft on ";
constexpr size_t DevNameHeadLen{std::size(DevNameHead) - 1};


/* Scales the given reftime value, rounding the result. */
inline uint RefTime2Samples(const ReferenceTime &val, uint srate)
{
    const auto retval = (val*srate + ReferenceTime{seconds{1}}/2) / seconds{1};
    return static_cast<uint>(mini64(retval, std::numeric_limits<uint>::max()));
}


class GuidPrinter {
    char mMsg[64];

public:
    GuidPrinter(const GUID &guid)
    {
        std::snprintf(mMsg, std::size(mMsg), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            DWORD{guid.Data1}, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
            guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    }
    const char *c_str() const { return mMsg; }
};

struct PropVariant {
    PROPVARIANT mProp;

public:
    PropVariant() { PropVariantInit(&mProp); }
    ~PropVariant() { clear(); }

    void clear() { PropVariantClear(&mProp); }

    PROPVARIANT* get() noexcept { return &mProp; }

    PROPVARIANT& operator*() noexcept { return mProp; }
    const PROPVARIANT& operator*() const noexcept { return mProp; }

    PROPVARIANT* operator->() noexcept { return &mProp; }
    const PROPVARIANT* operator->() const noexcept { return &mProp; }
};

struct DevMap {
    std::string name;
    std::string endpoint_guid; // obtained from PKEY_AudioEndpoint_GUID , set to "Unknown device GUID" if absent.
    std::wstring devid;

    template<typename T0, typename T1, typename T2>
    DevMap(T0&& name_, T1&& guid_, T2&& devid_)
      : name{std::forward<T0>(name_)}
      , endpoint_guid{std::forward<T1>(guid_)}
      , devid{std::forward<T2>(devid_)}
    { }
};

bool checkName(const al::span<DevMap> list, const std::string &name)
{
    auto match_name = [&name](const DevMap &entry) -> bool { return entry.name == name; };
    return std::find_if(list.cbegin(), list.cend(), match_name) != list.cend();
}

std::vector<DevMap> PlaybackDevices;
std::vector<DevMap> CaptureDevices;

#if defined(ALSOFT_UWP)
enum EDataFlow
{
    eRender              = 0,
    eCapture             = (eRender + 1),
    eAll                 = (eCapture + 1),
    EDataFlow_enum_count = (eAll + 1)
};
#endif

#if defined(ALSOFT_UWP)
struct DeviceHandle
{
    DeviceHandle& operator=(std::nullptr_t)
    {
        value = nullptr;
        return *this;
    }
    DeviceInformation^ value{nullptr};
};
using EventRegistrationToken = Windows::Foundation::EventRegistrationToken;
#else
using DeviceHandle           = ComPtr<IMMDevice>;
using EventRegistrationToken = void*;
#endif

#if defined(ALSOFT_UWP)
struct DeviceHelper final : public IActivateAudioInterfaceCompletionHandler
#else
struct DeviceHelper final : private IMMNotificationClient
#endif
{
public:
    DeviceHelper()
    {
#if defined(ALSOFT_UWP)
        mActiveClientEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        mRenderDeviceChangedToken = MediaDevice::DefaultAudioRenderDeviceChanged +=
            ref new TypedEventHandler<Platform::Object ^, DefaultAudioRenderDeviceChangedEventArgs ^>(
                [this](Platform::Object ^ sender, DefaultAudioRenderDeviceChangedEventArgs ^ args) {
                if(args->Role == AudioDeviceRole::Default)
                {
                    const std::string msg{"Default playback device changed: "+
                        wstr_to_utf8(args->Id->Data())};
                    alc::Event(alc::EventType::DefaultDeviceChanged, alc::DeviceType::Playback,
                        msg);
                }
            });
        mCaptureDeviceChangedToken = MediaDevice::DefaultAudioCaptureDeviceChanged +=
            ref new TypedEventHandler<Platform::Object ^, DefaultAudioCaptureDeviceChangedEventArgs ^>(
                [this](Platform::Object ^ sender, DefaultAudioCaptureDeviceChangedEventArgs ^ args) {
                if(args->Role == AudioDeviceRole::Default)
                {
                    const std::string msg{"Default capture device changed: "+
                        wstr_to_utf8(args->Id->Data())};
                    alc::Event(alc::EventType::DefaultDeviceChanged, alc::DeviceType::Capture,
                        msg);
                }
            });
#else
        HRESULT hr{CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IMMDeviceEnumerator, al::out_ptr(mEnumerator))};
        if(SUCCEEDED(hr))
            mEnumerator->RegisterEndpointNotificationCallback(this);
        else
            WARN("Failed to create IMMDeviceEnumerator instance: 0x%08lx\n", hr);
#endif
    }
    ~DeviceHelper()
    {
#if defined(ALSOFT_UWP)
        MediaDevice::DefaultAudioRenderDeviceChanged -= mRenderDeviceChangedToken;
        MediaDevice::DefaultAudioCaptureDeviceChanged -= mCaptureDeviceChangedToken;

        if(mActiveClientEvent != nullptr)
            CloseHandle(mActiveClientEvent);
        mActiveClientEvent = nullptr;
#else
        if(mEnumerator)
            mEnumerator->UnregisterEndpointNotificationCallback(this);
        mEnumerator = nullptr;
#endif
    }

    /** -------------------------- IUnkonwn ----------------------------- */
    std::atomic<ULONG> mRefCount{1};
    STDMETHODIMP_(ULONG) AddRef() noexcept override { return mRefCount.fetch_add(1u) + 1u; }

    STDMETHODIMP_(ULONG) Release() noexcept override
    {
        auto ret = mRefCount.fetch_sub(1u) - 1u;
        if(!ret) delete this;
        return ret;
    }

    STDMETHODIMP QueryInterface(const IID& IId, void **UnknownPtrPtr) noexcept override
    {
        // Three rules of QueryInterface:
        // https://docs.microsoft.com/en-us/windows/win32/com/rules-for-implementing-queryinterface
        // 1. Objects must have identity.
        // 2. The set of interfaces on an object instance must be static.
        // 3. It must be possible to query successfully for any interface on an object from any other interface.

        // If ppvObject(the address) is nullptr, then this method returns E_POINTER.
        if(!UnknownPtrPtr)
            return E_POINTER;

        // https://docs.microsoft.com/en-us/windows/win32/com/implementing-reference-counting
        // Whenever a client calls a method(or API function), such as QueryInterface, that returns a new interface
        // pointer, the method being called is responsible for incrementing the reference count through the returned
        // pointer. For example, when a client first creates an object, it receives an interface pointer to an object
        // that, from the client's point of view, has a reference count of one. If the client then calls AddRef on the
        // interface pointer, the reference count becomes two. The client must call Release twice on the interface
        // pointer to drop all of its references to the object.
#if defined(ALSOFT_UWP)
        if(IId == __uuidof(IActivateAudioInterfaceCompletionHandler))
        {
            *UnknownPtrPtr = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
#else
        if(IId == __uuidof(IMMNotificationClient))
        {
            *UnknownPtrPtr = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
#endif
        else if(IId == __uuidof(IAgileObject) || IId == __uuidof(IUnknown))
        {
            *UnknownPtrPtr = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }

        // This method returns S_OK if the interface is supported, and E_NOINTERFACE otherwise.
        *UnknownPtrPtr = nullptr;
        return E_NOINTERFACE;
    }

#if defined(ALSOFT_UWP)
    /** ----------------------- IActivateAudioInterfaceCompletionHandler ------------ */
    HRESULT ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT hrActivateResult     = S_OK;
        IUnknown* punkAudioInterface = nullptr;

        HRESULT hr = operation->GetActivateResult(&hrActivateResult, &punkAudioInterface);
        // Check for a successful activation result
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivateResult))
        {
            if (mPPV)
            {
                // Get the pointer for the Audio Client
                IAudioClient3* audioClient;
                punkAudioInterface->QueryInterface(IID_PPV_ARGS(&audioClient));
                *mPPV = audioClient;
            }
        }

        SetEvent(mActiveClientEvent);

        // Need to return S_OK
        return S_OK;
    }
#else
    /** ----------------------- IMMNotificationClient ------------ */
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) noexcept override { return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) noexcept override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) noexcept override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) noexcept override { return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) noexcept override
    {
        if(role != eMultimedia)
            return S_OK;

        if(flow == eRender)
        {
            const std::string msg{"Default playback device changed: "+
                wstr_to_utf8(pwstrDefaultDeviceId)};
            alc::Event(alc::EventType::DefaultDeviceChanged, alc::DeviceType::Playback, msg);
        }
        else if(flow == eCapture)
        {
            const std::string msg{"Default capture device changed: "+
                wstr_to_utf8(pwstrDefaultDeviceId)};
            alc::Event(alc::EventType::DefaultDeviceChanged, alc::DeviceType::Capture, msg);
        }
        return S_OK;
    }
#endif

    /** -------------------------- DeviceHelper ----------------------------- */
    HRESULT OpenDevice(LPCWSTR devid, EDataFlow flow, DeviceHandle& device) 
    {
#if !defined(ALSOFT_UWP)
        HRESULT hr{E_POINTER};
        if(mEnumerator)
        {
            if (!devid)
                hr = mEnumerator->GetDefaultAudioEndpoint(flow, eMultimedia, al::out_ptr(device));
            else
                hr = mEnumerator->GetDevice(devid, al::out_ptr(device));
        }
        return hr;
#else
        const auto deviceRole = Windows::Media::Devices::AudioDeviceRole::Default;
		Platform::String^ devIfPath =
			!devid ? (flow == eRender ? MediaDevice::GetDefaultAudioRenderId(deviceRole) : MediaDevice::GetDefaultAudioCaptureId(deviceRole))
			: ref new Platform::String(devid);

		Concurrency::task<DeviceInformation^> createDeviceOp(
			DeviceInformation::CreateFromIdAsync(devIfPath, nullptr, DeviceInformationKind::DeviceInterface));
		auto status = createDeviceOp.then([&](DeviceInformation^ deviceInfo) {
            device.value = deviceInfo;
		}).wait();
		if (status != Concurrency::task_status::completed)
		{
			return E_NOINTERFACE;
		}
        return S_OK;
#endif
    }

    HRESULT ActivateAudioClient(_In_ DeviceHandle& device, void **ppv)
    {
#if !defined(ALSOFT_UWP)
        HRESULT hr{device->Activate(__uuidof(IAudioClient3), CLSCTX_INPROC_SERVER, nullptr, ppv)};
#else
        HRESULT hr{ActivateAudioInterface(device.value->Id->Data(), __uuidof(IAudioClient3),
            nullptr, ppv)};
#endif
        return hr;
    }

    HRESULT probe_devices(EDataFlow flowdir, std::vector<DevMap>& list)
    {
        std::vector<DevMap>{}.swap(list);

#if !defined(ALSOFT_UWP)
        ComPtr<IMMDeviceCollection> coll;
        HRESULT hr{mEnumerator->EnumAudioEndpoints(flowdir, DEVICE_STATE_ACTIVE,
            al::out_ptr(coll))};
        if(FAILED(hr))
        {
            ERR("Failed to enumerate audio endpoints: 0x%08lx\n", hr);
            return hr;
        }

        UINT count{0};
        hr = coll->GetCount(&count);
        if(SUCCEEDED(hr) && count > 0)
            list.reserve(count);

        ComPtr<IMMDevice> device;
        hr = mEnumerator->GetDefaultAudioEndpoint(flowdir, eMultimedia, al::out_ptr(device));
        if(SUCCEEDED(hr))
        {
            if(WCHAR *devid{get_device_id(device.get())})
            {
                add_device(device, devid, list);
                CoTaskMemFree(devid);
            }
            device = nullptr;
        }

        for(UINT i{0};i < count;++i)
        {
            hr = coll->Item(i, al::out_ptr(device));
            if(FAILED(hr))
                continue;

            if(WCHAR *devid{get_device_id(device.get())})
            {
                add_device(device, devid, list);
                CoTaskMemFree(devid);
            }
            device = nullptr;
        }

        return S_OK;
#else
        const auto deviceRole = Windows::Media::Devices::AudioDeviceRole::Default;
        auto DefaultAudioId   = flowdir == eRender ? MediaDevice::GetDefaultAudioRenderId(deviceRole)
                                                   : MediaDevice::GetDefaultAudioCaptureId(deviceRole);
        Concurrency::task<DeviceInformation ^> createDefaultOp(DeviceInformation::CreateFromIdAsync(DefaultAudioId, nullptr, DeviceInformationKind::DeviceInterface));
        auto task_status = createDefaultOp
                               .then([this, &list](DeviceInformation ^ deviceInfo) {
            if (deviceInfo)
                add_device(DeviceHandle{deviceInfo}, deviceInfo->Id->Data(), list);
        }).wait();
        if (task_status != Concurrency::task_group_status::completed)
            return E_FAIL;

        // Get the string identifier of the audio renderer
        auto AudioSelector = flowdir == eRender ? MediaDevice::GetAudioRenderSelector() : MediaDevice::GetAudioCaptureSelector();

        // Setup the asynchronous callback
        Concurrency::task<DeviceInformationCollection ^> enumOperation(
            DeviceInformation::FindAllAsync(AudioSelector, /*PropertyList*/nullptr, DeviceInformationKind::DeviceInterface));
        task_status = enumOperation
                          .then([this, &list](DeviceInformationCollection ^ DeviceInfoCollection) {
            if (DeviceInfoCollection)
            {
                try
                {
                    auto deviceCount = DeviceInfoCollection->Size;
                    for (unsigned int i = 0; i < deviceCount; ++i)
                    {
                        DeviceInformation ^ deviceInfo = DeviceInfoCollection->GetAt(i);
                        if (deviceInfo)
                            add_device(DeviceHandle{deviceInfo}, deviceInfo->Id->Data(), list);
                    }
                }
                catch (Platform::Exception ^ e)
                {
                }
            }
        }).wait();

        return task_status == Concurrency::task_group_status::completed ? S_OK : E_FAIL;
#endif
    }

    using NameGUIDPair = std::pair<std::string, std::string>;
    static NameGUIDPair get_device_name_and_guid(const DeviceHandle& device)
    {
#if !defined(ALSOFT_UWP)
        static constexpr char UnknownName[]{"Unknown Device Name"};
        static constexpr char UnknownGuid[]{"Unknown Device GUID"};
        std::string name, guid;

        ComPtr<IPropertyStore> ps;
        HRESULT hr = device->OpenPropertyStore(STGM_READ, al::out_ptr(ps));
        if (FAILED(hr))
        {
            WARN("OpenPropertyStore failed: 0x%08lx\n", hr);
            return std::make_pair(UnknownName, UnknownGuid);
        }

        PropVariant pvprop;
        hr = ps->GetValue(al::bit_cast<PROPERTYKEY>(DEVPKEY_Device_FriendlyName), pvprop.get());
        if (FAILED(hr))
        {
            WARN("GetValue Device_FriendlyName failed: 0x%08lx\n", hr);
            name += UnknownName;
        }
        else if (pvprop->vt == VT_LPWSTR)
            name += wstr_to_utf8(pvprop->pwszVal);
        else
        {
            WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvprop->vt);
            name += UnknownName;
        }

        pvprop.clear();
        hr = ps->GetValue(al::bit_cast<PROPERTYKEY>(PKEY_AudioEndpoint_GUID), pvprop.get());
        if (FAILED(hr))
        {
            WARN("GetValue AudioEndpoint_GUID failed: 0x%08lx\n", hr);
            guid = UnknownGuid;
        }
        else if (pvprop->vt == VT_LPWSTR)
            guid = wstr_to_utf8(pvprop->pwszVal);
        else
        {
            WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvprop->vt);
            guid = UnknownGuid;
        }

#else
        auto devInfo     = device.value;
        std::string name = wstr_to_utf8(devInfo->Name->Data());
        std::string guid;
        // devInfo->Id is DeviceInterfacePath: \\?\SWD#MMDEVAPI#{0.0.0.00000000}.{a21c17a0-fc1d-405e-ab5a-b513422b57d1}#{e6327cad-dcec-4949-ae8a-991e976a79d2}
        Platform::String ^ devIfPath = devInfo->Id;
        auto wcsDevIfPath            = devIfPath->Data();
        auto devIdStart              = wcsstr(wcsDevIfPath, L"}.");
        if (devIdStart)
        {
            devIdStart += 2;  // L"}."
            auto devIdStartEnd = wcschr(devIdStart, L'#');
            if (devIdStartEnd)
            {
                std::wstring wDevId{devIdStart, static_cast<size_t>(devIdStartEnd - devIdStart)};
                guid = wstr_to_utf8(wDevId.c_str());
                std::transform(guid.begin(), guid.end(), guid.begin(), [](char ch) { return static_cast<char>(std::toupper(ch)); });
            }
        }
#endif
        return std::make_pair(std::move(name), std::move(guid));
    }

    static void add_device(const DeviceHandle& device, const WCHAR* devid, std::vector<DevMap>& list)
    {
        for (auto& entry : list)
        {
            if (entry.devid == devid)
                return;
        }

        auto name_guid = get_device_name_and_guid(device);
        int count{1};
        std::string newname{name_guid.first};
        while (checkName(list, newname))
        {
            newname = name_guid.first;
            newname += " #";
            newname += std::to_string(++count);
        }
        list.emplace_back(std::move(newname), std::move(name_guid.second), devid);
        const DevMap& newentry = list.back();

        TRACE("Got device \"%s\", \"%s\", \"%ls\"\n", newentry.name.c_str(), newentry.endpoint_guid.c_str(),
              newentry.devid.c_str());
    }

#if !defined(ALSOFT_UWP)
    static WCHAR *get_device_id(IMMDevice* device)
    {
        WCHAR *devid;

        const HRESULT hr{device->GetId(&devid)};
        if(FAILED(hr))
        {
            ERR("Failed to get device id: %lx\n", hr);
            return nullptr;
        }

        return devid;
    }
    static EndpointFormFactor get_device_formfactor(IMMDevice* device)
    {
        ComPtr<IPropertyStore> ps;
        HRESULT hr{device->OpenPropertyStore(STGM_READ, al::out_ptr(ps))};
        if(FAILED(hr))
        {
            WARN("OpenPropertyStore failed: 0x%08lx\n", hr);
            return UnknownFormFactor;
        }

        EndpointFormFactor formfactor{UnknownFormFactor};
        PropVariant pvform;
        hr = ps->GetValue(PKEY_AudioEndpoint_FormFactor, pvform.get());
        if(FAILED(hr))
            WARN("GetValue AudioEndpoint_FormFactor failed: 0x%08lx\n", hr);
        else if(pvform->vt == VT_UI4)
            formfactor = static_cast<EndpointFormFactor>(pvform->ulVal);
        else if(pvform->vt != VT_EMPTY)
            WARN("Unexpected PROPVARIANT type: 0x%04x\n", pvform->vt);
        return formfactor;
    }
#endif

private:
#if defined(ALSOFT_UWP)
    HRESULT ActivateAudioInterface(_In_ LPCWSTR deviceInterfacePath,
                                   _In_ REFIID riid,
                                   _In_opt_ PROPVARIANT* activationParams,
                                   void** ppv)
    {
        IActivateAudioInterfaceAsyncOperation* asyncOp{nullptr};
        mPPV       = ppv;
        HRESULT hr = ActivateAudioInterfaceAsync(deviceInterfacePath, riid, activationParams, this, &asyncOp);
        if(FAILED(hr))
            return hr;
        if(asyncOp)
            asyncOp->Release();

        DWORD res{WaitForSingleObjectEx(mActiveClientEvent, 2000, FALSE)};
        if(res != WAIT_OBJECT_0)
            ERR("WaitForSingleObjectEx error: 0x%lx\n", res);
        return res;
    }

    HANDLE mActiveClientEvent{nullptr};
    void** mPPV{nullptr};

    EventRegistrationToken mRenderDeviceChangedToken;
    EventRegistrationToken mCaptureDeviceChangedToken;
#else
    ComPtr<IMMDeviceEnumerator> mEnumerator{nullptr};
#endif
};

bool MakeExtensible(WAVEFORMATEXTENSIBLE *out, const WAVEFORMATEX *in)
{
    *out = WAVEFORMATEXTENSIBLE{};
    if(in->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        *out = *CONTAINING_RECORD(in, const WAVEFORMATEXTENSIBLE, Format);
        out->Format.cbSize = sizeof(*out) - sizeof(out->Format);
    }
    else if(in->wFormatTag == WAVE_FORMAT_PCM)
    {
        out->Format = *in;
        out->Format.cbSize = 0;
        out->Samples.wValidBitsPerSample = out->Format.wBitsPerSample;
        if(out->Format.nChannels == 1)
            out->dwChannelMask = MONO;
        else if(out->Format.nChannels == 2)
            out->dwChannelMask = STEREO;
        else
            ERR("Unhandled PCM channel count: %d\n", out->Format.nChannels);
        out->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else if(in->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        out->Format = *in;
        out->Format.cbSize = 0;
        out->Samples.wValidBitsPerSample = out->Format.wBitsPerSample;
        if(out->Format.nChannels == 1)
            out->dwChannelMask = MONO;
        else if(out->Format.nChannels == 2)
            out->dwChannelMask = STEREO;
        else
            ERR("Unhandled IEEE float channel count: %d\n", out->Format.nChannels);
        out->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    else
    {
        ERR("Unhandled format tag: 0x%04x\n", in->wFormatTag);
        return false;
    }
    return true;
}

void TraceFormat(const char *msg, const WAVEFORMATEX *format)
{
    constexpr size_t fmtex_extra_size{sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX)};
    if(format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= fmtex_extra_size)
    {
        const WAVEFORMATEXTENSIBLE *fmtex{
            CONTAINING_RECORD(format, const WAVEFORMATEXTENSIBLE, Format)};
        TRACE("%s:\n"
            "    FormatTag      = 0x%04x\n"
            "    Channels       = %d\n"
            "    SamplesPerSec  = %lu\n"
            "    AvgBytesPerSec = %lu\n"
            "    BlockAlign     = %d\n"
            "    BitsPerSample  = %d\n"
            "    Size           = %d\n"
            "    Samples        = %d\n"
            "    ChannelMask    = 0x%lx\n"
            "    SubFormat      = %s\n",
            msg, fmtex->Format.wFormatTag, fmtex->Format.nChannels, fmtex->Format.nSamplesPerSec,
            fmtex->Format.nAvgBytesPerSec, fmtex->Format.nBlockAlign, fmtex->Format.wBitsPerSample,
            fmtex->Format.cbSize, fmtex->Samples.wReserved, fmtex->dwChannelMask,
            GuidPrinter{fmtex->SubFormat}.c_str());
    }
    else
        TRACE("%s:\n"
            "    FormatTag      = 0x%04x\n"
            "    Channels       = %d\n"
            "    SamplesPerSec  = %lu\n"
            "    AvgBytesPerSec = %lu\n"
            "    BlockAlign     = %d\n"
            "    BitsPerSample  = %d\n"
            "    Size           = %d\n",
            msg, format->wFormatTag, format->nChannels, format->nSamplesPerSec,
            format->nAvgBytesPerSec, format->nBlockAlign, format->wBitsPerSample, format->cbSize);
}


enum class MsgType {
    OpenDevice,
    ResetDevice,
    StartDevice,
    StopDevice,
    CloseDevice,
    EnumeratePlayback,
    EnumerateCapture,

    Count,
    QuitThread = Count
};

constexpr char MessageStr[static_cast<size_t>(MsgType::Count)][20]{
    "Open Device",
    "Reset Device",
    "Start Device",
    "Stop Device",
    "Close Device",
    "Enumerate Playback",
    "Enumerate Capture"
};


/* Proxy interface used by the message handler. */
struct WasapiProxy {
    virtual ~WasapiProxy() = default;

    virtual HRESULT openProxy(const char *name) = 0;
    virtual void closeProxy() = 0;

    virtual HRESULT resetProxy() = 0;
    virtual HRESULT startProxy() = 0;
    virtual void  stopProxy() = 0;

    struct Msg {
        MsgType mType;
        WasapiProxy *mProxy;
        const char *mParam;
        std::promise<HRESULT> mPromise;

        explicit operator bool() const noexcept { return mType != MsgType::QuitThread; }
    };
    static std::thread sThread;
    static std::deque<Msg> mMsgQueue;
    static std::mutex mMsgQueueLock;
    static std::condition_variable mMsgQueueCond;
    static std::mutex sThreadLock;
    static size_t sInitCount;

    static ComPtr<DeviceHelper> sDeviceHelper;

    std::future<HRESULT> pushMessage(MsgType type, const char *param=nullptr)
    {
        std::promise<HRESULT> promise;
        std::future<HRESULT> future{promise.get_future()};
        {
            std::lock_guard<std::mutex> _{mMsgQueueLock};
            mMsgQueue.emplace_back(Msg{type, this, param, std::move(promise)});
        }
        mMsgQueueCond.notify_one();
        return future;
    }

    static std::future<HRESULT> pushMessageStatic(MsgType type)
    {
        std::promise<HRESULT> promise;
        std::future<HRESULT> future{promise.get_future()};
        {
            std::lock_guard<std::mutex> _{mMsgQueueLock};
            mMsgQueue.emplace_back(Msg{type, nullptr, nullptr, std::move(promise)});
        }
        mMsgQueueCond.notify_one();
        return future;
    }

    static Msg popMessage()
    {
        std::unique_lock<std::mutex> lock{mMsgQueueLock};
        mMsgQueueCond.wait(lock, []{return !mMsgQueue.empty();});
        Msg msg{std::move(mMsgQueue.front())};
        mMsgQueue.pop_front();
        return msg;
    }

    static int messageHandler(std::promise<HRESULT> *promise);

    static HRESULT InitThread()
    {
        std::lock_guard<std::mutex> _{sThreadLock};
        HRESULT res{S_OK};
        if(!sThread.joinable())
        {
            std::promise<HRESULT> promise;
            auto future = promise.get_future();

            sThread = std::thread{&WasapiProxy::messageHandler, &promise};
            res = future.get();
            if(FAILED(res))
            {
                sThread.join();
                return res;
            }
        }
        ++sInitCount;
        return res;
    }

    static void DeinitThread()
    {
        std::lock_guard<std::mutex> _{sThreadLock};
        if(!--sInitCount && sThread.joinable())
        {
            pushMessageStatic(MsgType::QuitThread);
            sThread.join();
        }
    }
};
std::thread WasapiProxy::sThread;
std::deque<WasapiProxy::Msg> WasapiProxy::mMsgQueue;
std::mutex WasapiProxy::mMsgQueueLock;
std::condition_variable WasapiProxy::mMsgQueueCond;
std::mutex WasapiProxy::sThreadLock;
ComPtr<DeviceHelper> WasapiProxy::sDeviceHelper;
size_t WasapiProxy::sInitCount{0};

int WasapiProxy::messageHandler(std::promise<HRESULT> *promise)
{
    TRACE("Starting message thread\n");

    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    if(FAILED(hr))
    {
        WARN("Failed to initialize COM: 0x%08lx\n", hr);
        promise->set_value(hr);
        return 0;
    }
    promise->set_value(S_OK);
    promise = nullptr;

    TRACE("Starting message loop\n");
    while(Msg msg{popMessage()})
    {
        TRACE("Got message \"%s\" (0x%04x, this=%p, param=%p)\n",
            MessageStr[static_cast<size_t>(msg.mType)], static_cast<uint>(msg.mType),
            static_cast<void*>(msg.mProxy), static_cast<const void*>(msg.mParam));

        switch(msg.mType)
        {
        case MsgType::OpenDevice:
            hr = msg.mProxy->openProxy(msg.mParam);
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::ResetDevice:
            hr = msg.mProxy->resetProxy();
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::StartDevice:
            hr = msg.mProxy->startProxy();
            msg.mPromise.set_value(hr);
            continue;

        case MsgType::StopDevice:
            msg.mProxy->stopProxy();
            msg.mPromise.set_value(S_OK);
            continue;

        case MsgType::CloseDevice:
            msg.mProxy->closeProxy();
            msg.mPromise.set_value(S_OK);
            continue;

        case MsgType::EnumeratePlayback:
        case MsgType::EnumerateCapture:
            if(msg.mType == MsgType::EnumeratePlayback)
                msg.mPromise.set_value(sDeviceHelper->probe_devices(eRender, PlaybackDevices));
            else if(msg.mType == MsgType::EnumerateCapture)
                msg.mPromise.set_value(sDeviceHelper->probe_devices(eCapture, CaptureDevices));
            else
                msg.mPromise.set_value(E_FAIL);
            continue;

        case MsgType::QuitThread:
            break;
        }
        ERR("Unexpected message: %u\n", static_cast<uint>(msg.mType));
        msg.mPromise.set_value(E_FAIL);
    }
    TRACE("Message loop finished\n");
    CoUninitialize();

    return 0;
}

struct WasapiPlayback final : public BackendBase, WasapiProxy {
    WasapiPlayback(DeviceBase *device) noexcept : BackendBase{device} { }
    ~WasapiPlayback() override;

    int mixerProc();

    void open(const char *name) override;
    HRESULT openProxy(const char *name) override;
    void closeProxy() override;

    bool reset() override;
    HRESULT resetProxy() override;
    void start() override;
    HRESULT startProxy() override;
    void stop() override;
    void stopProxy() override;

    ClockLatency getClockLatency() override;

    HRESULT mOpenStatus{E_FAIL};
    DeviceHandle mMMDev{nullptr};
    ComPtr<IAudioClient> mClient{nullptr};
    ComPtr<IAudioRenderClient> mRender{nullptr};
    HANDLE mNotifyEvent{nullptr};

    UINT32 mOrigBufferSize{}, mOrigUpdateSize{};
    std::unique_ptr<char[]> mResampleBuffer{};
    uint mBufferFilled{0};
    SampleConverterPtr mResampler;

    WAVEFORMATEXTENSIBLE mFormat{};
    std::atomic<UINT32> mPadding{0u};

    std::mutex mMutex;

    std::atomic<bool> mKillNow{true};
    std::thread mThread;

    DEF_NEWDEL(WasapiPlayback)
};

WasapiPlayback::~WasapiPlayback()
{
    if(SUCCEEDED(mOpenStatus))
    {
        pushMessage(MsgType::CloseDevice).wait();
        DeinitThread();
    }
    mOpenStatus = E_FAIL;

    if(mNotifyEvent != nullptr)
        CloseHandle(mNotifyEvent);
    mNotifyEvent = nullptr;
}


FORCE_ALIGN int WasapiPlayback::mixerProc()
{
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    if(FAILED(hr))
    {
        ERR("CoInitializeEx(nullptr, COINIT_MULTITHREADED) failed: 0x%08lx\n", hr);
        mDevice->handleDisconnect("COM init failed: 0x%08lx", hr);
        return 1;
    }

    SetRTPriority();
    althrd_setname(MIXER_THREAD_NAME);

    const uint frame_size{mFormat.Format.nChannels * mFormat.Format.wBitsPerSample / 8u};
    const uint update_size{mOrigUpdateSize};
    const UINT32 buffer_len{mOrigBufferSize};
    while(!mKillNow.load(std::memory_order_relaxed))
    {
        UINT32 written;
        hr = mClient->GetCurrentPadding(&written);
        if(FAILED(hr))
        {
            ERR("Failed to get padding: 0x%08lx\n", hr);
            mDevice->handleDisconnect("Failed to retrieve buffer padding: 0x%08lx", hr);
            break;
        }
        mPadding.store(written, std::memory_order_relaxed);

        uint len{buffer_len - written};
        if(len < update_size)
        {
            DWORD res{WaitForSingleObjectEx(mNotifyEvent, 2000, FALSE)};
            if(res != WAIT_OBJECT_0)
                ERR("WaitForSingleObjectEx error: 0x%lx\n", res);
            continue;
        }

        BYTE *buffer;
        hr = mRender->GetBuffer(len, &buffer);
        if(SUCCEEDED(hr))
        {
            if(mResampler)
            {
                std::lock_guard<std::mutex> _{mMutex};
                for(UINT32 done{0};done < len;)
                {
                    if(mBufferFilled == 0)
                    {
                        mDevice->renderSamples(mResampleBuffer.get(), mDevice->UpdateSize,
                            mFormat.Format.nChannels);
                        mBufferFilled = mDevice->UpdateSize;
                    }

                    const void *src{mResampleBuffer.get()};
                    uint srclen{mBufferFilled};
                    uint got{mResampler->convert(&src, &srclen, buffer, len-done)};
                    buffer += got*frame_size;
                    done += got;

                    mPadding.store(written + done, std::memory_order_relaxed);
                    if(srclen)
                    {
                        const char *bsrc{static_cast<const char*>(src)};
                        std::copy(bsrc, bsrc + srclen*frame_size, mResampleBuffer.get());
                    }
                    mBufferFilled = srclen;
                }
            }
            else
            {
                std::lock_guard<std::mutex> _{mMutex};
                mDevice->renderSamples(buffer, len, mFormat.Format.nChannels);
                mPadding.store(written + len, std::memory_order_relaxed);
            }
            hr = mRender->ReleaseBuffer(len, 0);
        }
        if(FAILED(hr))
        {
            ERR("Failed to buffer data: 0x%08lx\n", hr);
            mDevice->handleDisconnect("Failed to send playback samples: 0x%08lx", hr);
            break;
        }
    }
    mPadding.store(0u, std::memory_order_release);

    CoUninitialize();
    return 0;
}


void WasapiPlayback::open(const char *name)
{
    if(SUCCEEDED(mOpenStatus))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Unexpected duplicate open call"};

    mNotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(mNotifyEvent == nullptr)
    {
        ERR("Failed to create notify events: %lu\n", GetLastError());
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create notify events"};
    }

    HRESULT hr{InitThread()};
    if(FAILED(hr))
    {
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to init COM thread: 0x%08lx", hr};
    }

    if(name)
    {
        if(PlaybackDevices.empty())
            pushMessage(MsgType::EnumeratePlayback);
        if(std::strncmp(name, DevNameHead, DevNameHeadLen) == 0)
        {
            name += DevNameHeadLen;
            if(*name == '\0')
                name = nullptr;
        }
    }

    mOpenStatus = pushMessage(MsgType::OpenDevice, name).get();
    if(FAILED(mOpenStatus))
    {
        DeinitThread();
        throw al::backend_exception{al::backend_error::DeviceError, "Device init failed: 0x%08lx",
            mOpenStatus};
    }
}

HRESULT WasapiPlayback::openProxy(const char *name)
{
    const wchar_t *devid{nullptr};
    if(name)
    {
        auto iter = std::find_if(PlaybackDevices.cbegin(), PlaybackDevices.cend(),
            [name](const DevMap &entry) -> bool
            { return entry.name == name || entry.endpoint_guid == name; });
        if(iter == PlaybackDevices.cend())
        {
            const std::wstring wname{utf8_to_wstr(name)};
            iter = std::find_if(PlaybackDevices.cbegin(), PlaybackDevices.cend(),
                [&wname](const DevMap &entry) -> bool
                { return entry.devid == wname; });
        }
        if(iter == PlaybackDevices.cend())
        {
            WARN("Failed to find device name matching \"%s\"\n", name);
            return E_FAIL;
        }
        name = iter->name.c_str();
        devid = iter->devid.c_str();
    }

    HRESULT hr{sDeviceHelper->OpenDevice(devid, eRender, mMMDev)};
    if (FAILED(hr))
    {
        WARN("Failed to open device \"%s\"\n", name ? name : "(default)");
        return hr;
    }
    mClient = nullptr;
    if (name)
        mDevice->DeviceName = std::string{DevNameHead} + name;
    else
        mDevice->DeviceName = DevNameHead + DeviceHelper::get_device_name_and_guid(mMMDev).first;

    return S_OK;
}

void WasapiPlayback::closeProxy()
{
    mClient = nullptr;
    mMMDev = nullptr;
}


bool WasapiPlayback::reset()
{
    HRESULT hr{pushMessage(MsgType::ResetDevice).get()};
    if(FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError, "0x%08lx", hr};
    return true;
}

HRESULT WasapiPlayback::resetProxy()
{
    mClient = nullptr;
    HRESULT hr{sDeviceHelper->ActivateAudioClient(mMMDev, al::out_ptr(mClient))};
    if(FAILED(hr))
    {
        ERR("Failed to reactivate audio client: 0x%08lx\n", hr);
        return hr;
    }

    WAVEFORMATEX *wfx;
    hr = mClient->GetMixFormat(&wfx);
    if(FAILED(hr))
    {
        ERR("Failed to get mix format: 0x%08lx\n", hr);
        return hr;
    }
    TraceFormat("Device mix format", wfx);

    WAVEFORMATEXTENSIBLE OutputType;
    if(!MakeExtensible(&OutputType, wfx))
    {
        CoTaskMemFree(wfx);
        return E_FAIL;
    }
    CoTaskMemFree(wfx);
    wfx = nullptr;

    const ReferenceTime per_time{ReferenceTime{seconds{mDevice->UpdateSize}} / mDevice->Frequency};
    const ReferenceTime buf_time{ReferenceTime{seconds{mDevice->BufferSize}} / mDevice->Frequency};
    bool isRear51{false};

    if(!mDevice->Flags.test(FrequencyRequest))
        mDevice->Frequency = OutputType.Format.nSamplesPerSec;
    if(!mDevice->Flags.test(ChannelsRequest))
    {
        /* If not requesting a channel configuration, auto-select given what
         * fits the mask's lsb (to ensure no gaps in the output channels). If
         * there's no mask, we can only assume mono or stereo.
         */
        const uint32_t chancount{OutputType.Format.nChannels};
        const DWORD chanmask{OutputType.dwChannelMask};
        if(chancount >= 12 && (chanmask&X714Mask) == X7DOT1DOT4)
            mDevice->FmtChans = DevFmtX71;
        else if(chancount >= 8 && (chanmask&X71Mask) == X7DOT1)
            mDevice->FmtChans = DevFmtX71;
        else if(chancount >= 7 && (chanmask&X61Mask) == X6DOT1)
            mDevice->FmtChans = DevFmtX61;
        else if(chancount >= 6 && (chanmask&X51Mask) == X5DOT1)
            mDevice->FmtChans = DevFmtX51;
        else if(chancount >= 6 && (chanmask&X51RearMask) == X5DOT1REAR)
        {
            mDevice->FmtChans = DevFmtX51;
            isRear51 = true;
        }
        else if(chancount >= 4 && (chanmask&QuadMask) == QUAD)
            mDevice->FmtChans = DevFmtQuad;
        else if(chancount >= 2 && ((chanmask&StereoMask) == STEREO || !chanmask))
            mDevice->FmtChans = DevFmtStereo;
        else if(chancount >= 1 && ((chanmask&MonoMask) == MONO || !chanmask))
            mDevice->FmtChans = DevFmtMono;
        else
            ERR("Unhandled channel config: %d -- 0x%08lx\n", chancount, chanmask);
    }
    else
    {
        const uint32_t chancount{OutputType.Format.nChannels};
        const DWORD chanmask{OutputType.dwChannelMask};
        isRear51 = (chancount == 6 && (chanmask&X51RearMask) == X5DOT1REAR);
    }

    OutputType.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    switch(mDevice->FmtChans)
    {
    case DevFmtMono:
        OutputType.Format.nChannels = 1;
        OutputType.dwChannelMask = MONO;
        break;
    case DevFmtAmbi3D:
        mDevice->FmtChans = DevFmtStereo;
        /*fall-through*/
    case DevFmtStereo:
        OutputType.Format.nChannels = 2;
        OutputType.dwChannelMask = STEREO;
        break;
    case DevFmtQuad:
        OutputType.Format.nChannels = 4;
        OutputType.dwChannelMask = QUAD;
        break;
    case DevFmtX51:
        OutputType.Format.nChannels = 6;
        OutputType.dwChannelMask = isRear51 ? X5DOT1REAR : X5DOT1;
        break;
    case DevFmtX61:
        OutputType.Format.nChannels = 7;
        OutputType.dwChannelMask = X6DOT1;
        break;
    case DevFmtX71:
    case DevFmtX3D71:
        OutputType.Format.nChannels = 8;
        OutputType.dwChannelMask = X7DOT1;
        break;
    case DevFmtX714:
        OutputType.Format.nChannels = 12;
        OutputType.dwChannelMask = X7DOT1DOT4;
        break;
    }
    switch(mDevice->FmtType)
    {
    case DevFmtByte:
        mDevice->FmtType = DevFmtUByte;
        /* fall-through */
    case DevFmtUByte:
        OutputType.Format.wBitsPerSample = 8;
        OutputType.Samples.wValidBitsPerSample = 8;
        OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtUShort:
        mDevice->FmtType = DevFmtShort;
        /* fall-through */
    case DevFmtShort:
        OutputType.Format.wBitsPerSample = 16;
        OutputType.Samples.wValidBitsPerSample = 16;
        OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtUInt:
        mDevice->FmtType = DevFmtInt;
        /* fall-through */
    case DevFmtInt:
        OutputType.Format.wBitsPerSample = 32;
        OutputType.Samples.wValidBitsPerSample = 32;
        OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtFloat:
        OutputType.Format.wBitsPerSample = 32;
        OutputType.Samples.wValidBitsPerSample = 32;
        OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    }
    OutputType.Format.nSamplesPerSec = mDevice->Frequency;

    OutputType.Format.nBlockAlign = static_cast<WORD>(OutputType.Format.nChannels *
        OutputType.Format.wBitsPerSample / 8);
    OutputType.Format.nAvgBytesPerSec = OutputType.Format.nSamplesPerSec *
        OutputType.Format.nBlockAlign;

    TraceFormat("Requesting playback format", &OutputType.Format);
    hr = mClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &OutputType.Format, &wfx);
    if(FAILED(hr))
    {
        WARN("Failed to check format support: 0x%08lx\n", hr);
        hr = mClient->GetMixFormat(&wfx);
    }
    if(FAILED(hr))
    {
        ERR("Failed to find a supported format: 0x%08lx\n", hr);
        return hr;
    }

    if(wfx != nullptr)
    {
        TraceFormat("Got playback format", wfx);
        if(!MakeExtensible(&OutputType, wfx))
        {
            CoTaskMemFree(wfx);
            return E_FAIL;
        }
        CoTaskMemFree(wfx);
        wfx = nullptr;

        if(!GetConfigValueBool(mDevice->DeviceName.c_str(), "wasapi", "allow-resampler", true))
            mDevice->Frequency = OutputType.Format.nSamplesPerSec;
        else
            mDevice->Frequency = minu(mDevice->Frequency, OutputType.Format.nSamplesPerSec);

        const uint32_t chancount{OutputType.Format.nChannels};
        const DWORD chanmask{OutputType.dwChannelMask};
        /* Don't update the channel format if the requested format fits what's
         * supported.
         */
        bool chansok{false};
        if(mDevice->Flags.test(ChannelsRequest))
        {
            /* When requesting a channel configuration, make sure it fits the
             * mask's lsb (to ensure no gaps in the output channels). If
             * there's no mask, assume the request fits with enough channels.
             */
            switch(mDevice->FmtChans)
            {
            case DevFmtMono:
                chansok = (chancount >= 1 && ((chanmask&MonoMask) == MONO || !chanmask));
                break;
            case DevFmtStereo:
                chansok = (chancount >= 2 && ((chanmask&StereoMask) == STEREO || !chanmask));
                break;
            case DevFmtQuad:
                chansok = (chancount >= 4 && ((chanmask&QuadMask) == QUAD || !chanmask));
                break;
            case DevFmtX51:
                chansok = (chancount >= 6 && ((chanmask&X51Mask) == X5DOT1
                        || (chanmask&X51RearMask) == X5DOT1REAR || !chanmask));
                break;
            case DevFmtX61:
                chansok = (chancount >= 7 && ((chanmask&X61Mask) == X6DOT1 || !chanmask));
                break;
            case DevFmtX71:
            case DevFmtX3D71:
                chansok = (chancount >= 8 && ((chanmask&X71Mask) == X7DOT1 || !chanmask));
                break;
            case DevFmtX714:
                chansok = (chancount >= 12 && ((chanmask&X714Mask) == X7DOT1DOT4 || !chanmask));
            case DevFmtAmbi3D:
                break;
            }
        }
        if(!chansok)
        {
            if(chancount >= 12 && (chanmask&X714Mask) == X7DOT1DOT4)
                mDevice->FmtChans = DevFmtX714;
            else if(chancount >= 8 && (chanmask&X71Mask) == X7DOT1)
                mDevice->FmtChans = DevFmtX71;
            else if(chancount >= 7 && (chanmask&X61Mask) == X6DOT1)
                mDevice->FmtChans = DevFmtX61;
            else if(chancount >= 6 && ((chanmask&X51Mask) == X5DOT1
                || (chanmask&X51RearMask) == X5DOT1REAR))
                mDevice->FmtChans = DevFmtX51;
            else if(chancount >= 4 && (chanmask&QuadMask) == QUAD)
                mDevice->FmtChans = DevFmtQuad;
            else if(chancount >= 2 && ((chanmask&StereoMask) == STEREO || !chanmask))
                mDevice->FmtChans = DevFmtStereo;
            else if(chancount >= 1 && ((chanmask&MonoMask) == MONO || !chanmask))
                mDevice->FmtChans = DevFmtMono;
            else
            {
                ERR("Unhandled extensible channels: %d -- 0x%08lx\n", OutputType.Format.nChannels,
                    OutputType.dwChannelMask);
                mDevice->FmtChans = DevFmtStereo;
                OutputType.Format.nChannels = 2;
                OutputType.dwChannelMask = STEREO;
            }
        }

        if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            if(OutputType.Format.wBitsPerSample == 8)
                mDevice->FmtType = DevFmtUByte;
            else if(OutputType.Format.wBitsPerSample == 16)
                mDevice->FmtType = DevFmtShort;
            else if(OutputType.Format.wBitsPerSample == 32)
                mDevice->FmtType = DevFmtInt;
            else
            {
                mDevice->FmtType = DevFmtShort;
                OutputType.Format.wBitsPerSample = 16;
            }
        }
        else if(IsEqualGUID(OutputType.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        {
            mDevice->FmtType = DevFmtFloat;
            OutputType.Format.wBitsPerSample = 32;
        }
        else
        {
            ERR("Unhandled format sub-type: %s\n", GuidPrinter{OutputType.SubFormat}.c_str());
            mDevice->FmtType = DevFmtShort;
            if(OutputType.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE)
                OutputType.Format.wFormatTag = WAVE_FORMAT_PCM;
            OutputType.Format.wBitsPerSample = 16;
            OutputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        }
        OutputType.Samples.wValidBitsPerSample = OutputType.Format.wBitsPerSample;
    }
    mFormat = OutputType;

#if !defined(ALSOFT_UWP)
    const EndpointFormFactor formfactor{DeviceHelper::get_device_formfactor(mMMDev.get())};
    mDevice->Flags.set(DirectEar, (formfactor == Headphones || formfactor == Headset));
#else
    mDevice->Flags.set(DirectEar, false);
#endif
    setDefaultWFXChannelOrder();

    hr = mClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_time.count(), 0, &OutputType.Format, nullptr);
    if(FAILED(hr))
    {
        ERR("Failed to initialize audio client: 0x%08lx\n", hr);
        return hr;
    }

    UINT32 buffer_len{};
    ReferenceTime min_per{};
    hr = mClient->GetDevicePeriod(&reinterpret_cast<REFERENCE_TIME&>(min_per), nullptr);
    if(SUCCEEDED(hr))
        hr = mClient->GetBufferSize(&buffer_len);
    if(FAILED(hr))
    {
        ERR("Failed to get audio buffer info: 0x%08lx\n", hr);
        return hr;
    }

    /* Find the nearest multiple of the period size to the update size */
    if(min_per < per_time)
        min_per *= maxi64((per_time + min_per/2) / min_per, 1);

    mOrigBufferSize = buffer_len;
    mOrigUpdateSize = minu(RefTime2Samples(min_per, mFormat.Format.nSamplesPerSec), buffer_len/2);

    mDevice->BufferSize = static_cast<uint>(uint64_t{buffer_len} * mDevice->Frequency /
        mFormat.Format.nSamplesPerSec);
    mDevice->UpdateSize = minu(RefTime2Samples(min_per, mDevice->Frequency),
        mDevice->BufferSize/2);

    mResampler = nullptr;
    mResampleBuffer = nullptr;
    mBufferFilled = 0;
    if(mDevice->Frequency != mFormat.Format.nSamplesPerSec)
    {
        mResampler = SampleConverter::Create(mDevice->FmtType, mDevice->FmtType,
            mFormat.Format.nChannels, mDevice->Frequency, mFormat.Format.nSamplesPerSec,
            Resampler::FastBSinc24);
        mResampleBuffer = std::make_unique<char[]>(size_t{mDevice->UpdateSize} *
            mFormat.Format.nChannels * mFormat.Format.wBitsPerSample / 8);

        TRACE("Created converter for %s/%s format, dst: %luhz (%u), src: %uhz (%u)\n",
            DevFmtChannelsString(mDevice->FmtChans), DevFmtTypeString(mDevice->FmtType),
            mFormat.Format.nSamplesPerSec, mOrigUpdateSize, mDevice->Frequency,
            mDevice->UpdateSize);
    }

    hr = mClient->SetEventHandle(mNotifyEvent);
    if(FAILED(hr))
    {
        ERR("Failed to set event handle: 0x%08lx\n", hr);
        return hr;
    }

    return hr;
}


void WasapiPlayback::start()
{
    const HRESULT hr{pushMessage(MsgType::StartDevice).get()};
    if(FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start playback: 0x%lx", hr};
}

HRESULT WasapiPlayback::startProxy()
{
    ResetEvent(mNotifyEvent);

    HRESULT hr{mClient->Start()};
    if(FAILED(hr))
    {
        ERR("Failed to start audio client: 0x%08lx\n", hr);
        return hr;
    }

    hr = mClient->GetService(IID_IAudioRenderClient, al::out_ptr(mRender));
    if(SUCCEEDED(hr))
    {
        try {
            mKillNow.store(false, std::memory_order_release);
            mThread = std::thread{std::mem_fn(&WasapiPlayback::mixerProc), this};
        }
        catch(...) {
            mRender = nullptr;
            ERR("Failed to start thread\n");
            hr = E_FAIL;
        }
    }

    if(FAILED(hr))
        mClient->Stop();

    return hr;
}


void WasapiPlayback::stop()
{ pushMessage(MsgType::StopDevice).wait(); }

void WasapiPlayback::stopProxy()
{
    if(!mRender || !mThread.joinable())
        return;

    mKillNow.store(true, std::memory_order_release);
    mThread.join();

    mRender = nullptr;
    mClient->Stop();
}


ClockLatency WasapiPlayback::getClockLatency()
{
    ClockLatency ret;

    std::lock_guard<std::mutex> _{mMutex};
    ret.ClockTime = GetDeviceClockTime(mDevice);
    ret.Latency  = seconds{mPadding.load(std::memory_order_relaxed)};
    ret.Latency /= mFormat.Format.nSamplesPerSec;
    if(mResampler)
    {
        auto extra = mResampler->currentInputDelay();
        ret.Latency += std::chrono::duration_cast<nanoseconds>(extra) / mDevice->Frequency;
        ret.Latency += nanoseconds{seconds{mBufferFilled}} / mDevice->Frequency;
    }

    return ret;
}


struct WasapiCapture final : public BackendBase, WasapiProxy {
    WasapiCapture(DeviceBase *device) noexcept : BackendBase{device} { }
    ~WasapiCapture() override;

    int recordProc();

    void open(const char *name) override;
    HRESULT openProxy(const char *name) override;
    void closeProxy() override;

    HRESULT resetProxy() override;
    void start() override;
    HRESULT startProxy() override;
    void stop() override;
    void stopProxy() override;

    void captureSamples(std::byte *buffer, uint samples) override;
    uint availableSamples() override;

    HRESULT mOpenStatus{E_FAIL};
    DeviceHandle mMMDev{nullptr};
    ComPtr<IAudioClient> mClient{nullptr};
    ComPtr<IAudioCaptureClient> mCapture{nullptr};
    HANDLE mNotifyEvent{nullptr};

    ChannelConverter mChannelConv{};
    SampleConverterPtr mSampleConv;
    RingBufferPtr mRing;

    std::atomic<bool> mKillNow{true};
    std::thread mThread;

    DEF_NEWDEL(WasapiCapture)
};

WasapiCapture::~WasapiCapture()
{
    if(SUCCEEDED(mOpenStatus))
    {
        pushMessage(MsgType::CloseDevice).wait();
        DeinitThread();
    }
    mOpenStatus = E_FAIL;

    if(mNotifyEvent != nullptr)
        CloseHandle(mNotifyEvent);
    mNotifyEvent = nullptr;
}


FORCE_ALIGN int WasapiCapture::recordProc()
{
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    if(FAILED(hr))
    {
        ERR("CoInitializeEx(nullptr, COINIT_MULTITHREADED) failed: 0x%08lx\n", hr);
        mDevice->handleDisconnect("COM init failed: 0x%08lx", hr);
        return 1;
    }

    althrd_setname(RECORD_THREAD_NAME);

    std::vector<float> samples;
    while(!mKillNow.load(std::memory_order_relaxed))
    {
        UINT32 avail;
        hr = mCapture->GetNextPacketSize(&avail);
        if(FAILED(hr))
            ERR("Failed to get next packet size: 0x%08lx\n", hr);
        else if(avail > 0)
        {
            UINT32 numsamples;
            DWORD flags;
            BYTE *rdata;

            hr = mCapture->GetBuffer(&rdata, &numsamples, &flags, nullptr, nullptr);
            if(FAILED(hr))
                ERR("Failed to get capture buffer: 0x%08lx\n", hr);
            else
            {
                if(mChannelConv.is_active())
                {
                    samples.resize(numsamples*2);
                    mChannelConv.convert(rdata, samples.data(), numsamples);
                    rdata = reinterpret_cast<BYTE*>(samples.data());
                }

                auto data = mRing->getWriteVector();

                size_t dstframes;
                if(mSampleConv)
                {
                    const void *srcdata{rdata};
                    uint srcframes{numsamples};

                    dstframes = mSampleConv->convert(&srcdata, &srcframes, data.first.buf,
                        static_cast<uint>(minz(data.first.len, INT_MAX)));
                    if(srcframes > 0 && dstframes == data.first.len && data.second.len > 0)
                    {
                        /* If some source samples remain, all of the first dest
                         * block was filled, and there's space in the second
                         * dest block, do another run for the second block.
                         */
                        dstframes += mSampleConv->convert(&srcdata, &srcframes, data.second.buf,
                            static_cast<uint>(minz(data.second.len, INT_MAX)));
                    }
                }
                else
                {
                    const uint framesize{mDevice->frameSizeFromFmt()};
                    size_t len1{minz(data.first.len, numsamples)};
                    size_t len2{minz(data.second.len, numsamples-len1)};

                    memcpy(data.first.buf, rdata, len1*framesize);
                    if(len2 > 0)
                        memcpy(data.second.buf, rdata+len1*framesize, len2*framesize);
                    dstframes = len1 + len2;
                }

                mRing->writeAdvance(dstframes);

                hr = mCapture->ReleaseBuffer(numsamples);
                if(FAILED(hr)) ERR("Failed to release capture buffer: 0x%08lx\n", hr);
            }
        }

        if(FAILED(hr))
        {
            mDevice->handleDisconnect("Failed to capture samples: 0x%08lx", hr);
            break;
        }

        DWORD res{WaitForSingleObjectEx(mNotifyEvent, 2000, FALSE)};
        if(res != WAIT_OBJECT_0)
            ERR("WaitForSingleObjectEx error: 0x%lx\n", res);
    }

    CoUninitialize();
    return 0;
}


void WasapiCapture::open(const char *name)
{
    if(SUCCEEDED(mOpenStatus))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Unexpected duplicate open call"};

    mNotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if(mNotifyEvent == nullptr)
    {
        ERR("Failed to create notify events: %lu\n", GetLastError());
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create notify events"};
    }

    HRESULT hr{InitThread()};
    if(FAILED(hr))
    {
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to init COM thread: 0x%08lx", hr};
    }

    if(name)
    {
        if(CaptureDevices.empty())
            pushMessage(MsgType::EnumerateCapture);
        if(std::strncmp(name, DevNameHead, DevNameHeadLen) == 0)
        {
            name += DevNameHeadLen;
            if(*name == '\0')
                name = nullptr;
        }
    }

    mOpenStatus = pushMessage(MsgType::OpenDevice, name).get();
    if(FAILED(mOpenStatus))
    {
        DeinitThread();
        throw al::backend_exception{al::backend_error::DeviceError, "Device init failed: 0x%08lx",
            mOpenStatus};
    }

    hr = pushMessage(MsgType::ResetDevice).get();
    if(FAILED(hr))
    {
        if(hr == E_OUTOFMEMORY)
            throw al::backend_exception{al::backend_error::OutOfMemory, "Out of memory"};
        throw al::backend_exception{al::backend_error::DeviceError, "Device reset failed"};
    }
}

HRESULT WasapiCapture::openProxy(const char *name)
{
    const wchar_t *devid{nullptr};
    if(name)
    {
        auto iter = std::find_if(CaptureDevices.cbegin(), CaptureDevices.cend(),
            [name](const DevMap &entry) -> bool
            { return entry.name == name || entry.endpoint_guid == name; });
        if(iter == CaptureDevices.cend())
        {
            const std::wstring wname{utf8_to_wstr(name)};
            iter = std::find_if(CaptureDevices.cbegin(), CaptureDevices.cend(),
                [&wname](const DevMap &entry) -> bool
                { return entry.devid == wname; });
        }
        if(iter == CaptureDevices.cend())
        {
            WARN("Failed to find device name matching \"%s\"\n", name);
            return E_FAIL;
        }
        name = iter->name.c_str();
        devid = iter->devid.c_str();
    }

    HRESULT hr{sDeviceHelper->OpenDevice(devid, eCapture, mMMDev)};
    if (FAILED(hr))
    {
        WARN("Failed to open device \"%s\"\n", name ? name : "(default)");
        return hr;
    }
    mClient = nullptr;
    if (name)
        mDevice->DeviceName = std::string{ DevNameHead } + name;
    else
        mDevice->DeviceName = DevNameHead + DeviceHelper::get_device_name_and_guid(mMMDev).first;

    return S_OK;
}

void WasapiCapture::closeProxy()
{
    mClient = nullptr;
    mMMDev = nullptr;
}

HRESULT WasapiCapture::resetProxy()
{
    mClient = nullptr;

    HRESULT hr{sDeviceHelper->ActivateAudioClient(mMMDev, al::out_ptr(mClient))};
    if(FAILED(hr))
    {
        ERR("Failed to reactivate audio client: 0x%08lx\n", hr);
        return hr;
    }

    WAVEFORMATEX *wfx;
    hr = mClient->GetMixFormat(&wfx);
    if(FAILED(hr))
    {
        ERR("Failed to get capture format: 0x%08lx\n", hr);
        return hr;
    }
    TraceFormat("Device capture format", wfx);

    WAVEFORMATEXTENSIBLE InputType{};
    if(!MakeExtensible(&InputType, wfx))
    {
        CoTaskMemFree(wfx);
        return E_FAIL;
    }
    CoTaskMemFree(wfx);
    wfx = nullptr;

    const bool isRear51{InputType.Format.nChannels == 6
        && (InputType.dwChannelMask&X51RearMask) == X5DOT1REAR};

    // Make sure buffer is at least 100ms in size
    ReferenceTime buf_time{ReferenceTime{seconds{mDevice->BufferSize}} / mDevice->Frequency};
    buf_time = std::max(buf_time, ReferenceTime{milliseconds{100}});

    InputType = {};
    InputType.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    switch(mDevice->FmtChans)
    {
    case DevFmtMono:
        InputType.Format.nChannels = 1;
        InputType.dwChannelMask = MONO;
        break;
    case DevFmtStereo:
        InputType.Format.nChannels = 2;
        InputType.dwChannelMask = STEREO;
        break;
    case DevFmtQuad:
        InputType.Format.nChannels = 4;
        InputType.dwChannelMask = QUAD;
        break;
    case DevFmtX51:
        InputType.Format.nChannels = 6;
        InputType.dwChannelMask = isRear51 ? X5DOT1REAR : X5DOT1;
        break;
    case DevFmtX61:
        InputType.Format.nChannels = 7;
        InputType.dwChannelMask = X6DOT1;
        break;
    case DevFmtX71:
        InputType.Format.nChannels = 8;
        InputType.dwChannelMask = X7DOT1;
        break;
    case DevFmtX714:
        InputType.Format.nChannels = 12;
        InputType.dwChannelMask = X7DOT1DOT4;
        break;

    case DevFmtX3D71:
    case DevFmtAmbi3D:
        return E_FAIL;
    }
    switch(mDevice->FmtType)
    {
    /* NOTE: Signedness doesn't matter, the converter will handle it. */
    case DevFmtByte:
    case DevFmtUByte:
        InputType.Format.wBitsPerSample = 8;
        InputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtShort:
    case DevFmtUShort:
        InputType.Format.wBitsPerSample = 16;
        InputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtInt:
    case DevFmtUInt:
        InputType.Format.wBitsPerSample = 32;
        InputType.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case DevFmtFloat:
        InputType.Format.wBitsPerSample = 32;
        InputType.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    }
    InputType.Samples.wValidBitsPerSample = InputType.Format.wBitsPerSample;
    InputType.Format.nSamplesPerSec = mDevice->Frequency;

    InputType.Format.nBlockAlign = static_cast<WORD>(InputType.Format.nChannels *
        InputType.Format.wBitsPerSample / 8);
    InputType.Format.nAvgBytesPerSec = InputType.Format.nSamplesPerSec *
        InputType.Format.nBlockAlign;
    InputType.Format.cbSize = sizeof(InputType) - sizeof(InputType.Format);

    TraceFormat("Requesting capture format", &InputType.Format);
    hr = mClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &InputType.Format, &wfx);
    if(FAILED(hr))
    {
        WARN("Failed to check capture format support: 0x%08lx\n", hr);
        hr = mClient->GetMixFormat(&wfx);
    }
    if(FAILED(hr))
    {
        ERR("Failed to find a supported capture format: 0x%08lx\n", hr);
        return hr;
    }

    mSampleConv = nullptr;
    mChannelConv = {};

    if(wfx != nullptr)
    {
        TraceFormat("Got capture format", wfx);
        if(!MakeExtensible(&InputType, wfx))
        {
            CoTaskMemFree(wfx);
            return E_FAIL;
        }
        CoTaskMemFree(wfx);
        wfx = nullptr;

        auto validate_fmt = [](DeviceBase *device, uint32_t chancount, DWORD chanmask) noexcept
            -> bool
        {
            switch(device->FmtChans)
            {
            /* If the device wants mono, we can handle any input. */
            case DevFmtMono:
                return true;
            /* If the device wants stereo, we can handle mono or stereo input. */
            case DevFmtStereo:
                return (chancount == 2 && (chanmask == 0 || (chanmask&StereoMask) == STEREO))
                    || (chancount == 1 && (chanmask&MonoMask) == MONO);
            /* Otherwise, the device must match the input type. */
            case DevFmtQuad:
                return (chancount == 4 && (chanmask == 0 || (chanmask&QuadMask) == QUAD));
            /* 5.1 (Side) and 5.1 (Rear) are interchangeable here. */
            case DevFmtX51:
                return (chancount == 6 && (chanmask == 0 || (chanmask&X51Mask) == X5DOT1
                        || (chanmask&X51RearMask) == X5DOT1REAR));
            case DevFmtX61:
                return (chancount == 7 && (chanmask == 0 || (chanmask&X61Mask) == X6DOT1));
            case DevFmtX71:
            case DevFmtX3D71:
                return (chancount == 8 && (chanmask == 0 || (chanmask&X71Mask) == X7DOT1));
            case DevFmtX714:
                return (chancount == 12 && (chanmask == 0 || (chanmask&X714Mask) == X7DOT1DOT4));
            case DevFmtAmbi3D:
                return (chanmask == 0 && chancount == device->channelsFromFmt());
            }
            return false;
        };
        if(!validate_fmt(mDevice, InputType.Format.nChannels, InputType.dwChannelMask))
        {
            ERR("Failed to match format, wanted: %s %s %uhz, got: 0x%08lx mask %d channel%s %d-bit %luhz\n",
                DevFmtChannelsString(mDevice->FmtChans), DevFmtTypeString(mDevice->FmtType),
                mDevice->Frequency, InputType.dwChannelMask, InputType.Format.nChannels,
                (InputType.Format.nChannels==1)?"":"s", InputType.Format.wBitsPerSample,
                InputType.Format.nSamplesPerSec);
            return E_FAIL;
        }
    }

    DevFmtType srcType{};
    if(IsEqualGUID(InputType.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
    {
        if(InputType.Format.wBitsPerSample == 8)
            srcType = DevFmtUByte;
        else if(InputType.Format.wBitsPerSample == 16)
            srcType = DevFmtShort;
        else if(InputType.Format.wBitsPerSample == 32)
            srcType = DevFmtInt;
        else
        {
            ERR("Unhandled integer bit depth: %d\n", InputType.Format.wBitsPerSample);
            return E_FAIL;
        }
    }
    else if(IsEqualGUID(InputType.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
    {
        if(InputType.Format.wBitsPerSample == 32)
            srcType = DevFmtFloat;
        else
        {
            ERR("Unhandled float bit depth: %d\n", InputType.Format.wBitsPerSample);
            return E_FAIL;
        }
    }
    else
    {
        ERR("Unhandled format sub-type: %s\n", GuidPrinter{InputType.SubFormat}.c_str());
        return E_FAIL;
    }

    if(mDevice->FmtChans == DevFmtMono && InputType.Format.nChannels != 1)
    {
        uint chanmask{(1u<<InputType.Format.nChannels) - 1u};
        /* Exclude LFE from the downmix. */
        if((InputType.dwChannelMask&SPEAKER_LOW_FREQUENCY))
        {
            constexpr auto lfemask = MaskFromTopBits(SPEAKER_LOW_FREQUENCY);
            const int lfeidx{al::popcount(InputType.dwChannelMask&lfemask) - 1};
            chanmask &= ~(1u << lfeidx);
        }

        mChannelConv = ChannelConverter{srcType, InputType.Format.nChannels, chanmask,
            mDevice->FmtChans};
        TRACE("Created %s multichannel-to-mono converter\n", DevFmtTypeString(srcType));
        /* The channel converter always outputs float, so change the input type
         * for the resampler/type-converter.
         */
        srcType = DevFmtFloat;
    }
    else if(mDevice->FmtChans == DevFmtStereo && InputType.Format.nChannels == 1)
    {
        mChannelConv = ChannelConverter{srcType, 1, 0x1, mDevice->FmtChans};
        TRACE("Created %s mono-to-stereo converter\n", DevFmtTypeString(srcType));
        srcType = DevFmtFloat;
    }

    if(mDevice->Frequency != InputType.Format.nSamplesPerSec || mDevice->FmtType != srcType)
    {
        mSampleConv = SampleConverter::Create(srcType, mDevice->FmtType,
            mDevice->channelsFromFmt(), InputType.Format.nSamplesPerSec, mDevice->Frequency,
            Resampler::FastBSinc24);
        if(!mSampleConv)
        {
            ERR("Failed to create converter for %s format, dst: %s %uhz, src: %s %luhz\n",
                DevFmtChannelsString(mDevice->FmtChans), DevFmtTypeString(mDevice->FmtType),
                mDevice->Frequency, DevFmtTypeString(srcType), InputType.Format.nSamplesPerSec);
            return E_FAIL;
        }
        TRACE("Created converter for %s format, dst: %s %uhz, src: %s %luhz\n",
            DevFmtChannelsString(mDevice->FmtChans), DevFmtTypeString(mDevice->FmtType),
            mDevice->Frequency, DevFmtTypeString(srcType), InputType.Format.nSamplesPerSec);
    }

    hr = mClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_time.count(), 0, &InputType.Format, nullptr);
    if(FAILED(hr))
    {
        ERR("Failed to initialize audio client: 0x%08lx\n", hr);
        return hr;
    }

    UINT32 buffer_len{};
    ReferenceTime min_per{};
    hr = mClient->GetDevicePeriod(&reinterpret_cast<REFERENCE_TIME&>(min_per), nullptr);
    if(SUCCEEDED(hr))
        hr = mClient->GetBufferSize(&buffer_len);
    if(FAILED(hr))
    {
        ERR("Failed to get buffer size: 0x%08lx\n", hr);
        return hr;
    }
    mDevice->UpdateSize = RefTime2Samples(min_per, mDevice->Frequency);
    mDevice->BufferSize = buffer_len;

    mRing = RingBuffer::Create(buffer_len, mDevice->frameSizeFromFmt(), false);

    hr = mClient->SetEventHandle(mNotifyEvent);
    if(FAILED(hr))
    {
        ERR("Failed to set event handle: 0x%08lx\n", hr);
        return hr;
    }

    return hr;
}


void WasapiCapture::start()
{
    const HRESULT hr{pushMessage(MsgType::StartDevice).get()};
    if(FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start recording: 0x%lx", hr};
}

HRESULT WasapiCapture::startProxy()
{
    ResetEvent(mNotifyEvent);

    HRESULT hr{mClient->Start()};
    if(FAILED(hr))
    {
        ERR("Failed to start audio client: 0x%08lx\n", hr);
        return hr;
    }

    hr = mClient->GetService(IID_IAudioCaptureClient, al::out_ptr(mCapture));
    if(SUCCEEDED(hr))
    {
        try {
            mKillNow.store(false, std::memory_order_release);
            mThread = std::thread{std::mem_fn(&WasapiCapture::recordProc), this};
        }
        catch(...) {
            mCapture = nullptr;
            ERR("Failed to start thread\n");
            hr = E_FAIL;
        }
    }

    if(FAILED(hr))
    {
        mClient->Stop();
        mClient->Reset();
    }

    return hr;
}


void WasapiCapture::stop()
{ pushMessage(MsgType::StopDevice).wait(); }

void WasapiCapture::stopProxy()
{
    if(!mCapture || !mThread.joinable())
        return;

    mKillNow.store(true, std::memory_order_release);
    mThread.join();

    mCapture = nullptr;
    mClient->Stop();
    mClient->Reset();
}


void WasapiCapture::captureSamples(std::byte *buffer, uint samples)
{ mRing->read(buffer, samples); }

uint WasapiCapture::availableSamples()
{ return static_cast<uint>(mRing->readSpace()); }

} // namespace


bool WasapiBackendFactory::init()
{
    static HRESULT InitResult{E_FAIL};

    if(FAILED(InitResult)) try
    {
        auto res = std::async(std::launch::async, []() -> HRESULT
        {
            HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
            if(FAILED(hr))
            {
                WARN("Failed to initialize COM: 0x%08lx\n", hr);
                return hr;
            }
#if !defined(ALSOFT_UWP)
            ComPtr<IMMDeviceEnumerator> enumerator;
            hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                IID_IMMDeviceEnumerator, al::out_ptr(enumerator));
            if(FAILED(hr))
                WARN("Failed to create IMMDeviceEnumerator instance: 0x%08lx\n", hr);
            enumerator = nullptr;
#endif
            if(SUCCEEDED(hr))
                WasapiProxy::sDeviceHelper.reset(new DeviceHelper{});

            CoUninitialize();

            return hr;
        });

        InitResult = res.get();
    }
    catch(...) {
    }

    return SUCCEEDED(InitResult);
}

bool WasapiBackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback || type == BackendType::Capture; }

std::string WasapiBackendFactory::probe(BackendType type)
{
    struct ProxyControl {
        HRESULT mResult{};
        ProxyControl() { mResult = WasapiProxy::InitThread(); }
        ~ProxyControl() { if(SUCCEEDED(mResult)) WasapiProxy::DeinitThread(); }
    };
    ProxyControl proxy;

    std::string outnames;
    if(FAILED(proxy.mResult))
        return outnames;

    switch(type)
    {
    case BackendType::Playback:
        WasapiProxy::pushMessageStatic(MsgType::EnumeratePlayback).wait();
        for(const DevMap &entry : PlaybackDevices)
        {
            /* +1 to also append the null char (to ensure a null-separated list
             * and double-null terminated list).
             */
            outnames.append(DevNameHead).append(entry.name.c_str(), entry.name.length()+1);
        }
        break;

    case BackendType::Capture:
        WasapiProxy::pushMessageStatic(MsgType::EnumerateCapture).wait();
        for(const DevMap &entry : CaptureDevices)
            outnames.append(DevNameHead).append(entry.name.c_str(), entry.name.length()+1);
        break;
    }

    return outnames;
}

BackendPtr WasapiBackendFactory::createBackend(DeviceBase *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new WasapiPlayback{device}};
    if(type == BackendType::Capture)
        return BackendPtr{new WasapiCapture{device}};
    return nullptr;
}

BackendFactory &WasapiBackendFactory::getFactory()
{
    static WasapiBackendFactory factory{};
    return factory;
}
