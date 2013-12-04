// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/media/media_stream_manager.h"

#include <list>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/run_loop.h"
#include "base/threading/thread.h"
#include "content/browser/renderer_host/media/audio_input_device_manager.h"
#include "content/browser/renderer_host/media/device_request_message_filter.h"
#include "content/browser/renderer_host/media/media_stream_requester.h"
#include "content/browser/renderer_host/media/media_stream_ui_proxy.h"
#include "content/browser/renderer_host/media/video_capture_manager.h"
#include "content/browser/renderer_host/media/web_contents_capture_util.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/media_device_id.h"
#include "content/public/browser/media_observer.h"
#include "content/public/browser/media_request_state.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/media_stream_request.h"
#include "media/audio/audio_manager_base.h"
#include "media/audio/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "url/gurl.h"

#if defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#endif

namespace content {

// Creates a random label used to identify requests.
static std::string RandomLabel() {
  // An earlier PeerConnection spec,
  // http://dev.w3.org/2011/webrtc/editor/webrtc.html, specified the
  // MediaStream::label alphabet as containing 36 characters from
  // range: U+0021, U+0023 to U+0027, U+002A to U+002B, U+002D to U+002E,
  // U+0030 to U+0039, U+0041 to U+005A, U+005E to U+007E.
  // Here we use a safe subset.
  static const char kAlphabet[] = "0123456789"
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  std::string label(36, ' ');
  for (size_t i = 0; i < label.size(); ++i) {
    int random_char = base::RandGenerator(sizeof(kAlphabet) - 1);
    label[i] = kAlphabet[random_char];
  }
  return label;
}

// Helper to verify if a media stream type is part of options or not.
static bool Requested(const MediaStreamRequest& request,
                      MediaStreamType stream_type) {
  return (request.audio_type == stream_type ||
          request.video_type == stream_type);
}

class MediaStreamManager::DeviceRequest {
 public:
  DeviceRequest(MediaStreamRequester* requester,
                const MediaStreamRequest& request,
                int requesting_process_id,
                int requesting_view_id,
                ResourceContext* resource_context)
      : requester(requester),
        request(request),
        requesting_process_id(requesting_process_id),
        requesting_view_id(requesting_view_id),
        resource_context(resource_context),
        state_(NUM_MEDIA_TYPES, MEDIA_REQUEST_STATE_NOT_REQUESTED) {
  }

  ~DeviceRequest() {}

  // Update the request state and notify observers.
  void SetState(MediaStreamType stream_type, MediaRequestState new_state) {
    if (stream_type == NUM_MEDIA_TYPES) {
      for (int i = MEDIA_NO_SERVICE + 1; i < NUM_MEDIA_TYPES; ++i) {
        const MediaStreamType stream_type = static_cast<MediaStreamType>(i);
        state_[stream_type] = new_state;
      }
    } else {
      state_[stream_type] = new_state;
    }

    if (request.video_type != MEDIA_TAB_VIDEO_CAPTURE &&
        request.audio_type != MEDIA_TAB_AUDIO_CAPTURE &&
        new_state != MEDIA_REQUEST_STATE_CLOSING) {
      return;
    }

    MediaObserver* media_observer =
        GetContentClient()->browser()->GetMediaObserver();
    if (media_observer == NULL)
      return;

    // If we appended a device_id scheme, we want to remove it when notifying
    // observers which may be in different modules since this scheme is only
    // used internally within the content module.
    std::string device_id =
        WebContentsCaptureUtil::StripWebContentsDeviceScheme(
            request.tab_capture_device_id);

    media_observer->OnMediaRequestStateChanged(
        request.render_process_id, request.render_view_id,
        request.page_request_id,
        MediaStreamDevice(stream_type, device_id, device_id), new_state);
  }

  MediaRequestState state(MediaStreamType stream_type) const {
    return state_[stream_type];
  }

  MediaStreamRequester* const requester;  // Can be NULL.
  MediaStreamRequest request;

  // The render process id that requested this stream to be generated and that
  // will receive a handle to the MediaStream. This may be different from
  // MediaStreamRequest::render_process_id which in the tab capture case
  // specifies the target renderer from which audio and video is captured.
  const int requesting_process_id;

  // The render view id that requested this stream to be generated and that
  // will receive a handle to the MediaStream. This may be different from
  // MediaStreamRequest::render_view_id which in the tab capture case
  // specifies the target renderer from which audio and video is captured.
  const int requesting_view_id;

  ResourceContext* resource_context;

  StreamDeviceInfoArray devices;

  // Callback to the requester which audio/video devices have been selected.
  // It can be null if the requester has no interest to know the result.
  // Currently it is only used by |DEVICE_ACCESS| type.
  MediaStreamManager::MediaRequestResponseCallback callback;

  scoped_ptr<MediaStreamUIProxy> ui_proxy;

 private:
  std::vector<MediaRequestState> state_;
};

MediaStreamManager::EnumerationCache::EnumerationCache()
    : valid(false) {
}

MediaStreamManager::EnumerationCache::~EnumerationCache() {
}

MediaStreamManager::MediaStreamManager()
    : audio_manager_(NULL),
      monitoring_started_(false),
      io_loop_(NULL),
      use_fake_ui_(false) {}

MediaStreamManager::MediaStreamManager(media::AudioManager* audio_manager)
    : audio_manager_(audio_manager),
      monitoring_started_(false),
      io_loop_(NULL),
      use_fake_ui_(false) {
  DCHECK(audio_manager_);
  memset(active_enumeration_ref_count_, 0,
         sizeof(active_enumeration_ref_count_));

  // Some unit tests create the MSM in the IO thread and assumes the
  // initialization is done synchronously.
  if (BrowserThread::CurrentlyOn(BrowserThread::IO)) {
    InitializeDeviceManagersOnIOThread();
  } else {
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::Bind(&MediaStreamManager::InitializeDeviceManagersOnIOThread,
                   base::Unretained(this)));
  }
}

MediaStreamManager::~MediaStreamManager() {
  DVLOG(1) << "~MediaStreamManager";
  DCHECK(requests_.empty());
  DCHECK(!device_thread_.get());
}

VideoCaptureManager* MediaStreamManager::video_capture_manager() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(video_capture_manager_.get());
  return video_capture_manager_.get();
}

AudioInputDeviceManager* MediaStreamManager::audio_input_device_manager() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(audio_input_device_manager_.get());
  return audio_input_device_manager_.get();
}

std::string MediaStreamManager::MakeMediaAccessRequest(
    int render_process_id,
    int render_view_id,
    int page_request_id,
    const StreamOptions& options,
    const GURL& security_origin,
    const MediaRequestResponseCallback& callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  // Create a new request based on options.
  MediaStreamRequest stream_request(
      render_process_id, render_view_id, page_request_id,
      security_origin, MEDIA_DEVICE_ACCESS, std::string(), std::string(),
      options.audio_type, options.video_type);
  // TODO(perkj): The argument list with NULL parameters to DeviceRequest
  // suggests that this is the wrong design. Can this be refactored?
  DeviceRequest* request = new DeviceRequest(NULL, stream_request,
                                             render_process_id, render_view_id,
                                             NULL);
  const std::string& label = AddRequest(request);

  request->callback = callback;
  // Post a task and handle the request asynchronously. The reason is that the
  // requester won't have a label for the request until this function returns
  // and thus can not handle a response. Using base::Unretained is safe since
  // MediaStreamManager is deleted on the UI thread, after the IO thread has
  // been stopped.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&MediaStreamManager::SetupRequest,
                 base::Unretained(this), label));
  return label;
}

std::string MediaStreamManager::GenerateStream(MediaStreamRequester* requester,
                                               int render_process_id,
                                               int render_view_id,
    ResourceContext* rc,
                                               int page_request_id,
                                               const StreamOptions& options,
                                               const GURL& security_origin) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "GenerateStream()";
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kUseFakeDeviceForMediaStream)) {
    UseFakeDevice();
  }
  if (CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kUseFakeUIForMediaStream)) {
    UseFakeUI(scoped_ptr<FakeMediaStreamUIProxy>());
  }

  // Create a new request based on options.
  MediaStreamRequest stream_request(
      render_process_id, render_view_id, page_request_id,
      security_origin, MEDIA_GENERATE_STREAM,
      options.audio_device_id, options.video_device_id,
      options.audio_type, options.video_type);
  DeviceRequest* request = new DeviceRequest(requester, stream_request,
                                             render_process_id,
                                             render_view_id,
                                             rc);
  const std::string& label = AddRequest(request);

  // Post a task and handle the request asynchronously. The reason is that the
  // requester won't have a label for the request until this function returns
  // and thus can not handle a response. Using base::Unretained is safe since
  // MediaStreamManager is deleted on the UI thread, after the IO thread has
  // been stopped.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&MediaStreamManager::SetupRequest,
                 base::Unretained(this), label));
  return label;
}

void MediaStreamManager::CancelRequest(const std::string& label) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "CancelRequest({label = " << label <<  "})";
  DeviceRequest* request = FindRequest(label);
  if (!request) {
    // The request does not exist.
    LOG(ERROR) << "The request with label = " << label  << " does not exist.";
    return;
  }
  if (request->request.request_type == MEDIA_ENUMERATE_DEVICES) {
    DeleteRequest(label);
    return;
  }

  // This is a request for opening one or more devices.
  for (StreamDeviceInfoArray::iterator device_it = request->devices.begin();
       device_it != request->devices.end(); ++device_it) {
    MediaRequestState state = request->state(device_it->device.type);
    // If we have not yet requested the device to be opened - just ignore it.
    if (state != MEDIA_REQUEST_STATE_OPENING &&
        state != MEDIA_REQUEST_STATE_DONE) {
      continue;
    }
    // Stop the opening/opened devices of the requests.
    CloseDevice(device_it->device.type, device_it->session_id);
  }

  // Cancel the request if still pending at UI side.
  request->SetState(NUM_MEDIA_TYPES, MEDIA_REQUEST_STATE_CLOSING);
  DeleteRequest(label);
}

void MediaStreamManager::CancelAllRequests(int render_process_id) {
  DeviceRequests::iterator request_it = requests_.begin();
  while (request_it != requests_.end()) {
    if (request_it->second->requesting_process_id != render_process_id) {
      ++request_it;
      continue;
    }

    std::string label = request_it->first;
    ++request_it;
    CancelRequest(label);
  }
}

void MediaStreamManager::StopStreamDevice(int render_process_id,
                                          int render_view_id,
                                          const std::string& device_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "StopStreamDevice({render_view_id = " << render_view_id <<  "} "
           << ", {device_id = " << device_id << "})";
  // Find the first request for this |render_process_id| and |render_view_id|
  // of type MEDIA_GENERATE_STREAM that has requested to use |device_id| and
  // stop it.
  for (DeviceRequests::iterator request_it = requests_.begin();
       request_it  != requests_.end(); ++request_it) {
    DeviceRequest* request = request_it->second;
    const MediaStreamRequest& ms_request = request->request;
    if (request->requesting_process_id != render_process_id ||
        request->requesting_view_id != render_view_id ||
        ms_request.request_type != MEDIA_GENERATE_STREAM) {
      continue;
    }

    StreamDeviceInfoArray& devices = request->devices;
    for (StreamDeviceInfoArray::iterator device_it = devices.begin();
         device_it != devices.end(); ++device_it) {
      if (device_it->device.id == device_id) {
        StopDevice(device_it->device.type, device_it->session_id);
        return;
      }
    }
  }
}

void MediaStreamManager::StopDevice(MediaStreamType type, int session_id) {
  DVLOG(1) << "StopDevice"
           << "{type = " << type << "}"
           << "{session_id = " << session_id << "}";
  DeviceRequests::iterator request_it = requests_.begin();
  while (request_it != requests_.end()) {
    DeviceRequest* request = request_it->second;
    StreamDeviceInfoArray* devices = &request->devices;
    StreamDeviceInfoArray::iterator device_it = devices->begin();
    while (device_it != devices->end()) {
      if (device_it->device.type != type ||
          device_it->session_id != session_id) {
        ++device_it;
        continue;
      }
      if (request->state(type) == MEDIA_REQUEST_STATE_DONE)
        CloseDevice(type, session_id);
      device_it = devices->erase(device_it);
    }
    // If this request doesn't have any active devices, remove the request.
    if (devices->empty()) {
      std::string label = request_it->first;
      ++request_it;
      DeleteRequest(label);
    } else {
      ++request_it;
    }
  }
}

void MediaStreamManager::CloseDevice(MediaStreamType type, int session_id) {
  DVLOG(1) << "CloseDevice("
           << "{type = " << type <<  "} "
           << "{session_id = " << session_id << "})";
  GetDeviceManager(type)->Close(session_id);

  for (DeviceRequests::iterator request_it = requests_.begin();
       request_it != requests_.end() ; ++request_it) {
    StreamDeviceInfoArray* devices = &request_it->second->devices;
    for (StreamDeviceInfoArray::iterator device_it = devices->begin();
         device_it != devices->end(); ++device_it) {
      if (device_it->session_id == session_id &&
          device_it->device.type == type) {
        // Notify observers that this device is being closed.
        // Note that only one device per type can be opened.
        request_it->second->SetState(type, MEDIA_REQUEST_STATE_CLOSING);
      }
    }
  }
}

std::string MediaStreamManager::EnumerateDevices(
    MediaStreamRequester* requester,
    int render_process_id,
    int render_view_id,
    ResourceContext* rc,
    int page_request_id,
    MediaStreamType type,
    const GURL& security_origin) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(requester);
  DCHECK(type == MEDIA_DEVICE_AUDIO_CAPTURE ||
         type == MEDIA_DEVICE_VIDEO_CAPTURE);

  // Create a new request.
  StreamOptions options;
  if (type == MEDIA_DEVICE_AUDIO_CAPTURE) {
    options.audio_type = type;
  } else if (type == MEDIA_DEVICE_VIDEO_CAPTURE) {
    options.video_type = type;
  } else {
    NOTREACHED();
    return std::string();
  }

  MediaStreamRequest stream_request(
      render_process_id, render_view_id, page_request_id,
      security_origin, MEDIA_ENUMERATE_DEVICES, std::string(), std::string(),
      options.audio_type, options.video_type);
  DeviceRequest* request = new DeviceRequest(requester, stream_request,
                                             render_process_id, render_view_id,
                                             rc);
  const std::string& label = AddRequest(request);
  // Post a task and handle the request asynchronously. The reason is that the
  // requester won't have a label for the request until this function returns
  // and thus can not handle a response. Using base::Unretained is safe since
  // MediaStreamManager is deleted on the UI thread, after the IO thread has
  // been stopped.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&MediaStreamManager::DoEnumerateDevices,
                 base::Unretained(this), label));

  return label;
}

void MediaStreamManager::DoEnumerateDevices(const std::string& label) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DeviceRequest* request = FindRequest(label);
  if (!request)
    return;  // This can happen if the request has been canceled.

  MediaStreamType type;
  EnumerationCache* cache;
  if (request->request.audio_type == MEDIA_DEVICE_AUDIO_CAPTURE) {
    DCHECK_EQ(MEDIA_NO_SERVICE, request->request.video_type);
    type = MEDIA_DEVICE_AUDIO_CAPTURE;
    cache = &audio_enumeration_cache_;
  } else {
    DCHECK_EQ(MEDIA_DEVICE_VIDEO_CAPTURE, request->request.video_type);
    type = MEDIA_DEVICE_VIDEO_CAPTURE;
    cache = &video_enumeration_cache_;
  }

  if (cache->valid) {
    // Cached device list of this type exists. Just send it out.
    request->SetState(type, MEDIA_REQUEST_STATE_REQUESTED);
    request->devices = cache->devices;
    FinalizeEnumerateDevices(label, request);
  } else {
    StartEnumeration(request);
  }
  DVLOG(1) << "Enumerate Devices ({label = " << label <<  "})";
}

std::string MediaStreamManager::OpenDevice(
    MediaStreamRequester* requester,
    int render_process_id,
    int render_view_id,
    ResourceContext* rc,
    int page_request_id,
    const std::string& device_id,
    MediaStreamType type,
    const GURL& security_origin) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(type == MEDIA_DEVICE_AUDIO_CAPTURE ||
         type == MEDIA_DEVICE_VIDEO_CAPTURE);

  // Create a new request.
  StreamOptions options;
  if (IsAudioMediaType(type)) {
    options.audio_type = type;
    options.audio_device_id = device_id;
  } else if (IsVideoMediaType(type)) {
    options.video_type = type;
    options.video_device_id = device_id;
  } else {
    NOTREACHED();
    return std::string();
  }

  MediaStreamRequest stream_request(
      render_process_id, render_view_id, page_request_id,
      security_origin, MEDIA_OPEN_DEVICE, options.audio_device_id,
      options.video_device_id, options.audio_type, options.video_type);
  DeviceRequest* request = new DeviceRequest(requester, stream_request,
                                             render_process_id, render_view_id,
                                             rc);
  const std::string& label = AddRequest(request);
  // Post a task and handle the request asynchronously. The reason is that the
  // requester won't have a label for the request until this function returns
  // and thus can not handle a response. Using base::Unretained is safe since
  // MediaStreamManager is deleted on the UI thread, after the IO thread has
  // been stopped.
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&MediaStreamManager::SetupRequest,
                 base::Unretained(this), label));

  DVLOG(1) << "OpenDevice ({label = " << label <<  "})";
  return label;
}

void MediaStreamManager::EnsureDeviceMonitorStarted() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (!monitoring_started_)
    StartMonitoring();
}

void MediaStreamManager::StopRemovedDevices(
    const StreamDeviceInfoArray& old_devices,
    const StreamDeviceInfoArray& new_devices) {
  DVLOG(1) << "StopRemovedDevices("
           << "{#old_devices = " << old_devices.size() <<  "} "
           << "{#new_devices = " << new_devices.size() << "})";
  for (StreamDeviceInfoArray::const_iterator old_dev_it = old_devices.begin();
       old_dev_it != old_devices.end(); ++old_dev_it) {
    bool device_found = false;
    for (StreamDeviceInfoArray::const_iterator new_dev_it = new_devices.begin();
         new_dev_it != new_devices.end(); ++new_dev_it) {
      if (old_dev_it->device.id == new_dev_it->device.id) {
        device_found = true;
        break;
      }
    }

    if (!device_found) {
      // A device has been removed. We need to check if it is used by a
      // MediaStream and in that case cleanup and notify the render process.
      StopRemovedDevice(old_dev_it->device);
    }
  }
}

void MediaStreamManager::StopRemovedDevice(const MediaStreamDevice& device) {
  std::vector<int> session_ids;
  for (DeviceRequests::const_iterator it = requests_.begin();
       it != requests_.end() ; ++it) {
    const DeviceRequest* request = it->second;
    for (StreamDeviceInfoArray::const_iterator device_it =
             request->devices.begin();
         device_it != request->devices.end(); ++device_it) {
      std::string source_id = content::GetHMACForMediaDeviceID(
          request->resource_context,
          request->request.security_origin,
          device.id);
      if (device_it->device.id == source_id &&
          device_it->device.type == device.type) {
        session_ids.push_back(device_it->session_id);
        if (it->second->requester) {
          it->second->requester->DeviceStopped(
              it->second->requesting_view_id,
              it->first,
              *device_it);
        }
      }
    }
  }
  for (std::vector<int>::const_iterator it = session_ids.begin();
       it != session_ids.end(); ++it) {
    StopDevice(device.type, *it);
  }
}

void MediaStreamManager::StartMonitoring() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (!base::SystemMonitor::Get())
    return;

  if (!monitoring_started_) {
    monitoring_started_ = true;
    base::SystemMonitor::Get()->AddDevicesChangedObserver(this);

    // Enumerate both the audio and video devices to cache the device lists
    // and send them to media observer.
    ++active_enumeration_ref_count_[MEDIA_DEVICE_AUDIO_CAPTURE];
    audio_input_device_manager_->EnumerateDevices(MEDIA_DEVICE_AUDIO_CAPTURE);
    ++active_enumeration_ref_count_[MEDIA_DEVICE_VIDEO_CAPTURE];
    video_capture_manager_->EnumerateDevices(MEDIA_DEVICE_VIDEO_CAPTURE);
  }
}

void MediaStreamManager::StopMonitoring() {
  DCHECK_EQ(base::MessageLoop::current(), io_loop_);
  if (monitoring_started_) {
    base::SystemMonitor::Get()->RemoveDevicesChangedObserver(this);
    monitoring_started_ = false;
    ClearEnumerationCache(&audio_enumeration_cache_);
    ClearEnumerationCache(&video_enumeration_cache_);
  }
}

bool MediaStreamManager::TranslateRequestedSourceIdToDeviceId(
    DeviceRequest* request) {
  MediaStreamRequest* ms_request = &request->request;
  // If a specific device has been requested we need to find the real device id.
  if (ms_request->audio_type == MEDIA_DEVICE_AUDIO_CAPTURE &&
      !ms_request->requested_audio_device_id.empty()) {
    if (!TranslateSourceIdToDeviceId(MEDIA_DEVICE_AUDIO_CAPTURE,
                                     request->resource_context,
                                     ms_request->security_origin,
                                     ms_request->requested_audio_device_id,
                                     &ms_request->requested_audio_device_id)) {
      // TODO(perkj): gUM should support mandatory and optional constraints.
      // Ie - if the sourceId is mandatory but it does not match - gUM should
      // fail. For now we treat sourceId as an optional constraint.
      LOG(WARNING) << "Requested device does not exist.";
      ms_request->requested_audio_device_id = "";
      return true;;
    }
  }

  if (ms_request->video_type == MEDIA_DEVICE_VIDEO_CAPTURE &&
      !ms_request->requested_video_device_id.empty()) {
    if (!TranslateSourceIdToDeviceId(MEDIA_DEVICE_VIDEO_CAPTURE,
                                     request->resource_context,
                                     ms_request->security_origin,
                                     ms_request->requested_video_device_id,
                                     &ms_request->requested_video_device_id)) {
      // TODO(perkj): gUM should support mandatory and optional constraints.
      // Ie - if the sourceId is mandatory but it does not match - gUM should
      // fail. For now we treat sourceId as an optional constraint.
      LOG(WARNING) << "Requested device does not exist.";
      ms_request->requested_video_device_id = "";
      return true;
    }
  }
  DVLOG(3) << "Requested audio device "
           << ms_request->requested_audio_device_id
           << "Requested video device "
           << ms_request->requested_video_device_id;
  return true;
}

void MediaStreamManager::TranslateDeviceIdToSourceId(
    DeviceRequest* request,
    MediaStreamDevice* device) {
  if (request->request.audio_type == MEDIA_DEVICE_AUDIO_CAPTURE ||
      request->request.video_type == MEDIA_DEVICE_VIDEO_CAPTURE) {
    device->id = content::GetHMACForMediaDeviceID(
        request->resource_context,
        request->request.security_origin,
        device->id);
  }
}

bool MediaStreamManager::TranslateSourceIdToDeviceId(
    MediaStreamType stream_type,
    ResourceContext* rc,
    const GURL& security_origin,
    const std::string& source_id,
    std::string* device_id) {
  DCHECK(stream_type == MEDIA_DEVICE_AUDIO_CAPTURE ||
         stream_type == MEDIA_DEVICE_VIDEO_CAPTURE);
  DCHECK(!source_id.empty());

  EnumerationCache* cache =
      stream_type == MEDIA_DEVICE_AUDIO_CAPTURE ?
      &audio_enumeration_cache_ : &video_enumeration_cache_;

  // If device monitoring hasn't started, the |device_guid| is not valid.
  if (!cache->valid)
    return false;

  for (StreamDeviceInfoArray::const_iterator it = cache->devices.begin();
       it != cache->devices.end();
       ++it) {
    if (content::DoesMediaDeviceIDMatchHMAC(rc, security_origin, source_id,
                                            it->device.id)) {
      *device_id = it->device.id;
      return true;
    }
  }
  return false;
}

void MediaStreamManager::ClearEnumerationCache(EnumerationCache* cache) {
  DCHECK_EQ(base::MessageLoop::current(), io_loop_);
  cache->valid = false;
}

void MediaStreamManager::StartEnumeration(DeviceRequest* request) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // Start monitoring the devices when doing the first enumeration.
  if (!monitoring_started_ && base::SystemMonitor::Get()) {
    StartMonitoring();
  }

  // Start enumeration for devices of all requested device types.
  for (int i = MEDIA_NO_SERVICE + 1; i < NUM_MEDIA_TYPES; ++i) {
    const MediaStreamType stream_type = static_cast<MediaStreamType>(i);
    if (Requested(request->request, stream_type)) {
      request->SetState(stream_type, MEDIA_REQUEST_STATE_REQUESTED);
      DCHECK_GE(active_enumeration_ref_count_[stream_type], 0);
      if (active_enumeration_ref_count_[stream_type] == 0) {
        ++active_enumeration_ref_count_[stream_type];
        GetDeviceManager(stream_type)->EnumerateDevices(stream_type);
      }
    }
  }
}

std::string MediaStreamManager::AddRequest(DeviceRequest* request) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // Create a label for this request and verify it is unique.
  std::string unique_label;
  do {
    unique_label = RandomLabel();
  } while (requests_.find(unique_label) != requests_.end());

  requests_.insert(std::make_pair(unique_label, request));

  return unique_label;
}

MediaStreamManager::DeviceRequest*
MediaStreamManager::FindRequest(const std::string& label) const {
  DeviceRequests::const_iterator request_it = requests_.find(label);
  return request_it == requests_.end() ? NULL : request_it->second;
}

void MediaStreamManager::DeleteRequest(const std::string& label) {
  DeviceRequests::iterator it = requests_.find(label);
  scoped_ptr<DeviceRequest> request(it->second);
  requests_.erase(it);
}

void MediaStreamManager::PostRequestToUI(const std::string& label,
                                         DeviceRequest* request) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "PostRequestToUI({label= " << label << "})";
  // If a specific device has been requested we need to find the real device id.
  if (!TranslateRequestedSourceIdToDeviceId(request)) {
    FinalizeRequestFailed(label, request);
    return;
  }

  const MediaStreamType audio_type = request->request.audio_type;
  const MediaStreamType video_type = request->request.video_type;

  // Post the request to UI and set the state.
  if (IsAudioMediaType(audio_type))
    request->SetState(audio_type, MEDIA_REQUEST_STATE_PENDING_APPROVAL);
  if (IsVideoMediaType(video_type))
    request->SetState(video_type, MEDIA_REQUEST_STATE_PENDING_APPROVAL);

  if (use_fake_ui_) {
    if (!fake_ui_)
      fake_ui_.reset(new FakeMediaStreamUIProxy());

    MediaStreamDevices devices;
    if (audio_enumeration_cache_.valid) {
      for (StreamDeviceInfoArray::const_iterator it =
               audio_enumeration_cache_.devices.begin();
           it != audio_enumeration_cache_.devices.end(); ++it) {
        devices.push_back(it->device);
      }
    }
    if (video_enumeration_cache_.valid) {
      for (StreamDeviceInfoArray::const_iterator it =
               video_enumeration_cache_.devices.begin();
           it != video_enumeration_cache_.devices.end(); ++it) {
        devices.push_back(it->device);
      }
    }

    fake_ui_->SetAvailableDevices(devices);

    request->ui_proxy = fake_ui_.Pass();
  } else {
    request->ui_proxy = MediaStreamUIProxy::Create();
  }

  request->ui_proxy->RequestAccess(
      request->request,
      base::Bind(&MediaStreamManager::HandleAccessRequestResponse,
                 base::Unretained(this), label));
}

void MediaStreamManager::SetupRequest(const std::string& label) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DeviceRequest* request = FindRequest(label);
  if (!request) {
    DVLOG(1) << "SetupRequest label " << label << " doesn't exist!!";
    return;  // This can happen if the request has been canceled.
  }

  if (!request->request.security_origin.is_valid()) {
    LOG(ERROR) << "Invalid security origin. "
               << request->request.security_origin;
    FinalizeRequestFailed(label, request);
    return;
  }

  const MediaStreamType audio_type = request->request.audio_type;
  const MediaStreamType video_type = request->request.video_type;

  bool is_web_contents_capture =
      audio_type == MEDIA_TAB_AUDIO_CAPTURE ||
      video_type == MEDIA_TAB_VIDEO_CAPTURE;
  if (is_web_contents_capture && !SetupTabCaptureRequest(request)) {
    FinalizeRequestFailed(label, request);
    return;
  }

  bool is_screen_capture =
      video_type == MEDIA_DESKTOP_VIDEO_CAPTURE;
  if (is_screen_capture && !SetupScreenCaptureRequest(request)) {
    FinalizeRequestFailed(label, request);
    return;
  }

  if (!is_web_contents_capture &&
      !is_screen_capture &&
      ((IsAudioMediaType(audio_type) && !audio_enumeration_cache_.valid) ||
       (IsVideoMediaType(video_type) && !video_enumeration_cache_.valid))) {
    // Enumerate the devices if there is no valid device lists to be used.
    StartEnumeration(request);
    return;
  }
  PostRequestToUI(label, request);
}

bool MediaStreamManager::SetupTabCaptureRequest(DeviceRequest* request) {
  DCHECK(request->request.audio_type == MEDIA_TAB_AUDIO_CAPTURE ||
         request->request.video_type == MEDIA_TAB_VIDEO_CAPTURE);

  MediaStreamRequest* ms_request = &request->request;
  // Customize options for a WebContents based capture.
  int target_render_process_id = 0;
  int target_render_view_id = 0;

  // TODO(justinlin): Can't plumb audio mirroring using stream type right
  // now, so plumbing by device_id. Will revisit once it's refactored.
  // http://crbug.com/163100
  std::string tab_capture_device_id =
        WebContentsCaptureUtil::AppendWebContentsDeviceScheme(
            !ms_request->requested_video_device_id.empty() ?
             ms_request->requested_video_device_id :
             ms_request->requested_audio_device_id);

  bool has_valid_device_id = WebContentsCaptureUtil::ExtractTabCaptureTarget(
      tab_capture_device_id, &target_render_process_id,
      &target_render_view_id);
  if (!has_valid_device_id ||
      (ms_request->audio_type != MEDIA_TAB_AUDIO_CAPTURE &&
       ms_request->audio_type != MEDIA_NO_SERVICE) ||
      (ms_request->video_type != MEDIA_TAB_VIDEO_CAPTURE &&
       ms_request->video_type != MEDIA_NO_SERVICE)) {
    return false;
  }
  ms_request->tab_capture_device_id = tab_capture_device_id;
  ms_request->render_process_id = target_render_process_id;
  ms_request->render_view_id = target_render_view_id;
  DVLOG(3) << "SetupTabCaptureRequest "
           << ", {tab_capture_device_id = " << tab_capture_device_id <<  "}"
           << ", {target_render_process_id = " << target_render_process_id
           << "}"
           << ", {target_render_view_id = " << target_render_view_id << "}";
  return true;
}

bool MediaStreamManager::SetupScreenCaptureRequest(DeviceRequest* request) {
  DCHECK(request->request.audio_type ==  MEDIA_LOOPBACK_AUDIO_CAPTURE ||
         request->request.video_type == MEDIA_DESKTOP_VIDEO_CAPTURE);
  const MediaStreamRequest& ms_request = request->request;

  // For screen capture we only support two valid combinations:
  // (1) screen video capture only, or
  // (2) screen video capture with loopback audio capture.
  if (ms_request.video_type != MEDIA_DESKTOP_VIDEO_CAPTURE ||
      (ms_request.audio_type != MEDIA_NO_SERVICE &&
       ms_request.audio_type != MEDIA_LOOPBACK_AUDIO_CAPTURE)) {
    // TODO(sergeyu): Surface error message to the calling JS code.
    LOG(ERROR) << "Invalid screen capture request.";
    return false;
  }
  return true;
}

StreamDeviceInfoArray MediaStreamManager::GetDevicesOpenedByRequest(
    const std::string& label) const {
  DeviceRequest* request = FindRequest(label);
  if (!request)
    return StreamDeviceInfoArray();
  return request->devices;
}

bool MediaStreamManager::FindExistingRequestedDeviceInfo(
    const DeviceRequest& new_request,
    const MediaStreamDevice& new_device_info,
    StreamDeviceInfo* existing_device_info,
    MediaRequestState* existing_request_state) const {
  DCHECK(existing_device_info);
  DCHECK(existing_request_state);

  const MediaStreamRequest& new_ms_request = new_request.request;

  std::string source_id = content::GetHMACForMediaDeviceID(
      new_request.resource_context,
      new_ms_request.security_origin,
      new_device_info.id);

  for (DeviceRequests::const_iterator it = requests_.begin();
       it != requests_.end() ; ++it) {
    const DeviceRequest* request = it->second;
    if (request->requesting_process_id == new_request.requesting_process_id &&
        request->requesting_view_id == new_request.requesting_view_id &&
        request->request.request_type == new_ms_request.request_type) {
      for (StreamDeviceInfoArray::const_iterator device_it =
               request->devices.begin();
           device_it != request->devices.end(); ++device_it) {
        if (device_it->device.id == source_id &&
            device_it->device.type == new_device_info.type) {
            *existing_device_info = *device_it;
            *existing_request_state = request->state(device_it->device.type);
          return true;
        }
      }
    }
  }
  return false;
}

void MediaStreamManager::FinalizeGenerateStream(const std::string& label,
                                                DeviceRequest* request) {
  DVLOG(1) << "FinalizeGenerateStream label " << label;
  const StreamDeviceInfoArray& requested_devices = request->devices;

  // Partition the array of devices into audio vs video.
  StreamDeviceInfoArray audio_devices, video_devices;
  for (StreamDeviceInfoArray::const_iterator device_it =
           requested_devices.begin();
       device_it != requested_devices.end(); ++device_it) {
    if (IsAudioMediaType(device_it->device.type)) {
      audio_devices.push_back(*device_it);
    } else if (IsVideoMediaType(device_it->device.type)) {
      video_devices.push_back(*device_it);
    } else {
      NOTREACHED();
    }
  }

  request->requester->StreamGenerated(label, audio_devices, video_devices);
}

void MediaStreamManager::FinalizeRequestFailed(
    const std::string& label,
    DeviceRequest* request) {
  if (request->requester)
    request->requester->StreamGenerationFailed(label);

  if (request->request.request_type == MEDIA_DEVICE_ACCESS &&
      !request->callback.is_null()) {
    request->callback.Run(MediaStreamDevices(), request->ui_proxy.Pass());
  }

  DeleteRequest(label);
}

void MediaStreamManager::FinalizeOpenDevice(const std::string& label,
                                            DeviceRequest* request) {
  const StreamDeviceInfoArray& requested_devices = request->devices;
  request->requester->DeviceOpened(label, requested_devices.front());
}

void MediaStreamManager::FinalizeEnumerateDevices(const std::string& label,
                                                  DeviceRequest* request) {
  if (!request->request.security_origin.is_valid()) {
    request->requester->DevicesEnumerated(label, StreamDeviceInfoArray());
    return;
  }
  for (StreamDeviceInfoArray::iterator it = request->devices.begin();
       it != request->devices.end(); ++it) {
    TranslateDeviceIdToSourceId(request, &it->device);
  }
  request->requester->DevicesEnumerated(label, request->devices);
}

void MediaStreamManager::FinalizeMediaAccessRequest(
    const std::string& label,
    DeviceRequest* request,
    const MediaStreamDevices& devices) {
  if (!request->callback.is_null())
    request->callback.Run(devices, request->ui_proxy.Pass());

  // Delete the request since it is done.
  DeleteRequest(label);
}

void MediaStreamManager::InitializeDeviceManagersOnIOThread() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (device_thread_)
    return;

  device_thread_.reset(new base::Thread("MediaStreamDeviceThread"));
#if defined(OS_WIN)
  device_thread_->init_com_with_mta(true);
#endif
  CHECK(device_thread_->Start());

  audio_input_device_manager_ = new AudioInputDeviceManager(audio_manager_);
  audio_input_device_manager_->Register(
      this, device_thread_->message_loop_proxy().get());

  video_capture_manager_ = new VideoCaptureManager();
  video_capture_manager_->Register(this,
                                   device_thread_->message_loop_proxy().get());

  // We want to be notified of IO message loop destruction to delete the thread
  // and the device managers.
  io_loop_ = base::MessageLoop::current();
  io_loop_->AddDestructionObserver(this);
}

void MediaStreamManager::Opened(MediaStreamType stream_type,
                                int capture_session_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "Opened({stream_type = " << stream_type <<  "} "
           << "{capture_session_id = " << capture_session_id << "})";
  // Find the request(s) containing this device and mark it as used.
  // It can be used in several requests since the same device can be
  // requested from the same web page.
  for (DeviceRequests::iterator request_it = requests_.begin();
       request_it != requests_.end(); ++request_it) {
    const std::string& label = request_it->first;
    DeviceRequest* request = request_it->second;
    StreamDeviceInfoArray* devices = &(request->devices);
    for (StreamDeviceInfoArray::iterator device_it = devices->begin();
         device_it != devices->end(); ++device_it) {
      if (device_it->device.type == stream_type &&
          device_it->session_id == capture_session_id) {
        CHECK(request->state(device_it->device.type) ==
            MEDIA_REQUEST_STATE_OPENING);
        // We've found a matching request.
        request->SetState(device_it->device.type, MEDIA_REQUEST_STATE_DONE);

        if (IsAudioMediaType(device_it->device.type)) {
          // Store the native audio parameters in the device struct.
          // TODO(xians): Handle the tab capture sample rate/channel layout
          // in AudioInputDeviceManager::Open().
          if (device_it->device.type != content::MEDIA_TAB_AUDIO_CAPTURE) {
            const StreamDeviceInfo* info =
                audio_input_device_manager_->GetOpenedDeviceInfoById(
                    device_it->session_id);
            device_it->device.input = info->device.input;
            device_it->device.matched_output = info->device.matched_output;
          }
        }
        if (RequestDone(*request))
          HandleRequestDone(label, request);
        break;
      }
    }
  }
}

void MediaStreamManager::HandleRequestDone(const std::string& label,
                                           DeviceRequest* request) {
  DCHECK(RequestDone(*request));
  DVLOG(1) << "HandleRequestDone("
           << ", {label = " << label <<  "})";

  switch (request->request.request_type) {
    case MEDIA_OPEN_DEVICE:
      FinalizeOpenDevice(label, request);
      break;
    case MEDIA_GENERATE_STREAM: {
      FinalizeGenerateStream(label, request);
      break;
    }
    default:
      NOTREACHED();
      break;
  }

  if (request->ui_proxy.get()) {
    request->ui_proxy->OnStarted(
        base::Bind(&MediaStreamManager::StopMediaStreamFromBrowser,
                   base::Unretained(this), label));
  }
}

void MediaStreamManager::Closed(MediaStreamType stream_type,
                                int capture_session_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
}

void MediaStreamManager::DevicesEnumerated(
    MediaStreamType stream_type, const StreamDeviceInfoArray& devices) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "DevicesEnumerated("
           << ", {stream_type = " << stream_type <<  "})";

  // Only cache the device list when the device list has been changed.
  bool need_update_clients = false;
  EnumerationCache* cache =
      stream_type == MEDIA_DEVICE_AUDIO_CAPTURE ?
      &audio_enumeration_cache_ : &video_enumeration_cache_;
  if (!cache->valid ||
      devices.size() != cache->devices.size() ||
      !std::equal(devices.begin(), devices.end(), cache->devices.begin(),
                  StreamDeviceInfo::IsEqual)) {
    StopRemovedDevices(cache->devices, devices);
    cache->devices = devices;
    need_update_clients = true;

    // The device might not be able to be enumerated when it is not warmed up,
    // for example, when the machine just wakes up from sleep. We set the cache
    // to be invalid so that the next media request will trigger the
    // enumeration again. See issue/317673.
    cache->valid = !devices.empty();
  }

  if (need_update_clients && monitoring_started_)
    NotifyDevicesChanged(stream_type, devices);

  // Publish the result for all requests waiting for device list(s).
  // Find the requests waiting for this device list, store their labels and
  // release the iterator before calling device settings. We might get a call
  // back from device_settings that will need to iterate through devices.
  std::list<std::string> label_list;
  for (DeviceRequests::iterator it = requests_.begin(); it != requests_.end();
       ++it) {
    if (it->second->state(stream_type) == MEDIA_REQUEST_STATE_REQUESTED &&
        Requested(it->second->request, stream_type)) {
      if (it->second->request.request_type != MEDIA_ENUMERATE_DEVICES)
        it->second->SetState(stream_type, MEDIA_REQUEST_STATE_PENDING_APPROVAL);
      label_list.push_back(it->first);
    }
  }
  for (std::list<std::string>::iterator it = label_list.begin();
       it != label_list.end(); ++it) {
    DeviceRequest* request = FindRequest(*it);
    switch (request->request.request_type) {
      case MEDIA_ENUMERATE_DEVICES:
        if (need_update_clients && request->requester) {
          request->devices = devices;
          FinalizeEnumerateDevices(*it, request);
        }
        break;
      default:
        if (request->state(request->request.audio_type) ==
                MEDIA_REQUEST_STATE_REQUESTED ||
            request->state(request->request.video_type) ==
                MEDIA_REQUEST_STATE_REQUESTED) {
          // We are doing enumeration for other type of media, wait until it is
          // all done before posting the request to UI because UI needs
          // the device lists to handle the request.
          break;
        }

        PostRequestToUI(*it, request);
        break;
    }
  }
  label_list.clear();
  --active_enumeration_ref_count_[stream_type];
  DCHECK_GE(active_enumeration_ref_count_[stream_type], 0);
}

void MediaStreamManager::HandleAccessRequestResponse(
    const std::string& label,
    const MediaStreamDevices& devices) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DVLOG(1) << "HandleAccessRequestResponse("
           << ", {label = " << label <<  "})";

  DeviceRequest* request = FindRequest(label);
  if (!request) {
    // The request has been canceled before the UI returned.
    return;
  }

  if (request->request.request_type == MEDIA_DEVICE_ACCESS) {
    FinalizeMediaAccessRequest(label, request, devices);
    return;
  }

  // Handle the case when the request was denied.
  if (devices.empty()) {
    FinalizeRequestFailed(label, request);
    return;
  }

  // Process all newly-accepted devices for this request.
  bool found_audio = false;
  bool found_video = false;
  for (MediaStreamDevices::const_iterator device_it = devices.begin();
       device_it != devices.end(); ++device_it) {
    StreamDeviceInfo device_info;
    device_info.device = *device_it;

    // TODO(justinlin): Nicer way to do this?
    // Re-append the device's id since we lost it when posting request to UI.
    if (device_info.device.type == content::MEDIA_TAB_VIDEO_CAPTURE ||
        device_info.device.type == content::MEDIA_TAB_AUDIO_CAPTURE) {
      device_info.device.id = request->request.tab_capture_device_id;

      // Initialize the sample_rate and channel_layout here since for audio
      // mirroring, we don't go through EnumerateDevices where these are usually
      // initialized.
      if (device_info.device.type == content::MEDIA_TAB_AUDIO_CAPTURE) {
        const media::AudioParameters parameters =
            audio_manager_->GetDefaultOutputStreamParameters();
        int sample_rate = parameters.sample_rate();
        // If we weren't able to get the native sampling rate or the sample_rate
        // is outside the valid range for input devices set reasonable defaults.
        if (sample_rate <= 0 || sample_rate > 96000)
          sample_rate = 44100;

        device_info.device.input.sample_rate = sample_rate;
        device_info.device.input.channel_layout = media::CHANNEL_LAYOUT_STEREO;
      }
    }

    if (device_info.device.type == request->request.audio_type) {
      found_audio = true;
    } else if (device_info.device.type == request->request.video_type) {
      found_video = true;
    }

    // If this is request for a new MediaStream, a device is only opened once
    // per render view. This is so that the permission to use a device can be
    // revoked by a single call to StopStreamDevice regardless of how many
    // MediaStreams it is being used in.
    if (request->request.request_type == MEDIA_GENERATE_STREAM) {
      MediaRequestState state;
      if (FindExistingRequestedDeviceInfo(*request,
                                          device_info.device,
                                          &device_info,
                                          &state)) {
        request->devices.push_back(device_info);
        request->SetState(device_info.device.type, state);
        DVLOG(1) << "HandleAccessRequestResponse - device already opened "
                 << ", {label = " << label <<  "}"
                 << ", device_id = " << device_it->id << "}";
        continue;
      }
    }
    device_info.session_id =
        GetDeviceManager(device_info.device.type)->Open(device_info);
    TranslateDeviceIdToSourceId(request, &device_info.device);
    request->devices.push_back(device_info);

    request->SetState(device_info.device.type, MEDIA_REQUEST_STATE_OPENING);
    DVLOG(1) << "HandleAccessRequestResponse - opening device "
             << ", {label = " << label <<  "}"
             << ", {device_id = " << device_info.device.id << "}"
             << ", {session_id = " << device_info.session_id << "}";
  }

  // Check whether we've received all stream types requested.
  if (!found_audio && IsAudioMediaType(request->request.audio_type)) {
    request->SetState(request->request.audio_type, MEDIA_REQUEST_STATE_ERROR);
    DVLOG(1) << "Set no audio found label " << label;
  }

  if (!found_video && IsVideoMediaType(request->request.video_type))
    request->SetState(request->request.video_type, MEDIA_REQUEST_STATE_ERROR);

  if (RequestDone(*request))
    HandleRequestDone(label, request);
}

void MediaStreamManager::StopMediaStreamFromBrowser(const std::string& label) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  DeviceRequest* request = FindRequest(label);
  if (!request)
    return;

  // Notify renderers that the devices in the stream will be stopped.
  if (request->requester) {
    for (StreamDeviceInfoArray::iterator device_it = request->devices.begin();
         device_it != request->devices.end(); ++device_it) {
      request->requester->DeviceStopped(request->requesting_view_id,
                                        label,
                                        *device_it);
    }
  }

  CancelRequest(label);
}

void MediaStreamManager::UseFakeDevice() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  video_capture_manager()->UseFakeDevice();
  audio_input_device_manager()->UseFakeDevice();
}

void MediaStreamManager::UseFakeUI(scoped_ptr<FakeMediaStreamUIProxy> fake_ui) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  use_fake_ui_ = true;
  fake_ui_ = fake_ui.Pass();
}

void MediaStreamManager::WillDestroyCurrentMessageLoop() {
  DVLOG(3) << "MediaStreamManager::WillDestroyCurrentMessageLoop()";
  DCHECK_EQ(base::MessageLoop::current(), io_loop_);
  DCHECK(requests_.empty());
  if (device_thread_) {
    StopMonitoring();

    video_capture_manager_->Unregister();
    audio_input_device_manager_->Unregister();
    device_thread_.reset();
  }

  audio_input_device_manager_ = NULL;
  video_capture_manager_ = NULL;
}

void MediaStreamManager::NotifyDevicesChanged(
    MediaStreamType stream_type,
    const StreamDeviceInfoArray& devices) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  MediaObserver* media_observer =
      GetContentClient()->browser()->GetMediaObserver();
  if (media_observer == NULL)
    return;

  // Map the devices to MediaStreamDevices.
  MediaStreamDevices new_devices;
  for (StreamDeviceInfoArray::const_iterator it = devices.begin();
       it != devices.end(); ++it) {
    new_devices.push_back(it->device);
  }

  if (IsAudioMediaType(stream_type)) {
    media_observer->OnAudioCaptureDevicesChanged(new_devices);
  } else if (IsVideoMediaType(stream_type)) {
    media_observer->OnVideoCaptureDevicesChanged(new_devices);
  } else {
    NOTREACHED();
  }
}

bool MediaStreamManager::RequestDone(const DeviceRequest& request) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  const bool requested_audio = IsAudioMediaType(request.request.audio_type);
  const bool requested_video = IsVideoMediaType(request.request.video_type);

  const bool audio_done =
      !requested_audio ||
      request.state(request.request.audio_type) ==
      MEDIA_REQUEST_STATE_DONE ||
      request.state(request.request.audio_type) ==
      MEDIA_REQUEST_STATE_ERROR;
  if (!audio_done)
    return false;

  const bool video_done =
      !requested_video ||
      request.state(request.request.video_type) ==
      MEDIA_REQUEST_STATE_DONE ||
      request.state(request.request.video_type) ==
      MEDIA_REQUEST_STATE_ERROR;
  if (!video_done)
    return false;

  return true;
}

MediaStreamProvider* MediaStreamManager::GetDeviceManager(
    MediaStreamType stream_type) {
  if (IsVideoMediaType(stream_type)) {
    return video_capture_manager();
  } else if (IsAudioMediaType(stream_type)) {
    return audio_input_device_manager();
  }
  NOTREACHED();
  return NULL;
}

void MediaStreamManager::OnDevicesChanged(
    base::SystemMonitor::DeviceType device_type) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  // NOTE: This method is only called in response to physical audio/video device
  // changes (from the operating system).

  MediaStreamType stream_type;
  if (device_type == base::SystemMonitor::DEVTYPE_AUDIO_CAPTURE) {
    stream_type = MEDIA_DEVICE_AUDIO_CAPTURE;
  } else if (device_type == base::SystemMonitor::DEVTYPE_VIDEO_CAPTURE) {
    stream_type = MEDIA_DEVICE_VIDEO_CAPTURE;
  } else {
    return;  // Uninteresting device change.
  }

  // Always do enumeration even though some enumeration is in progress,
  // because those enumeration commands could be sent before these devices
  // change.
  ++active_enumeration_ref_count_[stream_type];
  GetDeviceManager(stream_type)->EnumerateDevices(stream_type);
}

}  // namespace content
