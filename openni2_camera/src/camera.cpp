/**
 * Copyright (c) 2013 Christian Kerl <christian.kerl@in.tum.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <openni2_camera/camera.h>
#include <openni2_camera/CameraConfig.h>

#include <image_transport/image_transport.h>
#include <camera_info_manager/camera_info_manager.h>
#include <sensor_msgs/image_encodings.h>
#include <dynamic_reconfigure/server.h>

#include <boost/bind.hpp>

namespace openni2_camera
{

namespace internal
{

using namespace openni;

int findVideoMode(const Array<VideoMode>& modes, int x, int y, PixelFormat format, int fps = 30)
{
  int result = 0;

  for(int idx = 0; idx < modes.getSize(); ++idx)
  {
    if(modes[idx].getResolutionX() == x && modes[idx].getResolutionY() == y && modes[idx].getPixelFormat() == format && modes[idx].getFps() == fps)
    {
      result = idx;
      break;
    }
  }

  return result;
}

std::string toString(const PixelFormat& format)
{
  switch(format)
  {
  case PIXEL_FORMAT_DEPTH_1_MM:
    return "DEPTH_1_MM";
  case PIXEL_FORMAT_DEPTH_100_UM:
    return "DEPTH_100_UM";
  case PIXEL_FORMAT_SHIFT_9_2:
    return "SHIFT_9_2";
  case PIXEL_FORMAT_SHIFT_9_3:
    return "SHIFT_9_3";

  case PIXEL_FORMAT_RGB888:
    return "RGB888";
  case PIXEL_FORMAT_YUV422:
    return "YUV422";
  case PIXEL_FORMAT_GRAY8:
    return "GRAY8";
  case PIXEL_FORMAT_GRAY16:
    return "GRAY16";
  case PIXEL_FORMAT_JPEG:
    return "JPEG";
  default:
    return "unknown";
  }
}

std::string toString(const SensorType& type)
{
  switch(type)
  {
  case SENSOR_COLOR:
    return "COLOR";
  case SENSOR_DEPTH:
    return "DEPTH";
  case SENSOR_IR:
    return "IR";
  default:
    return "unknown";
  }
}

class MethodNotSupportedException : std::exception
{
private:
  std::string cause_;
public:
  MethodNotSupportedException(const char* method) throw()
  {
    cause_ = "Method '" + std::string(method) + "' is not supported!";
  }
  virtual ~MethodNotSupportedException() throw() { }

  virtual const char* what() const throw()
  {
    return cause_.c_str();
  }
};

class SensorStreamManagerBase
{
public:
  SensorStreamManagerBase() {}
  virtual ~SensorStreamManagerBase() {}

  virtual VideoStream& stream()
  {
    throw MethodNotSupportedException("SensorStreamManagerBase::stream()");
  }

  virtual bool beginConfigure()
  {
    return false;
  }

  virtual bool tryConfigureVideoMode(VideoMode& mode)
  {
    throw MethodNotSupportedException("SensorStreamManagerBase::tryConfigureVideoMode()");
  }

  virtual void endConfigure()
  {
    throw MethodNotSupportedException("SensorStreamManagerBase::endConfigure()");
  }
};

class SensorStreamManager : public SensorStreamManagerBase, public VideoStream::NewFrameListener
{
protected:
  Device& device_;
  VideoStream stream_;
  VideoMode default_mode_;
  std::string name_, frame_id_;
  bool running_, was_running_;

  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  camera_info_manager::CameraInfoManager camera_info_manager_;
  image_transport::CameraPublisher publisher_;
  image_transport::SubscriberStatusCallback callback_;


  virtual void publish(sensor_msgs::Image::Ptr& image, sensor_msgs::CameraInfo::Ptr& camera_info)
  {
    publisher_.publish(image, camera_info);
  }
public:
  SensorStreamManager(ros::NodeHandle& nh, Device& device, SensorType type, std::string name, std::string frame_id, VideoMode& default_mode) :
    device_(device),
    default_mode_(default_mode),
    name_(name),
    frame_id_(frame_id),
    running_(false),
    nh_(nh, name_),
    it_(nh_),
    camera_info_manager_(nh_)
  {
    assert(device_.hasSensor(type));

    callback_ = boost::bind(&SensorStreamManager::onSubscriptionChanged, this, _1);
    publisher_ = it_.advertiseCamera("image_raw", 1, callback_, callback_);

    ROS_ERROR_STREAM_COND(stream_.create(device_, type) != STATUS_OK, "Failed to create stream '" << toString(type) << "'!");
    stream_.addNewFrameListener(this);
    ROS_ERROR_STREAM_COND(stream_.setVideoMode(default_mode_) != STATUS_OK, "Failed to set default video mode for stream '" << toString(type) << "'!");
  }

  virtual ~SensorStreamManager()
  {
    stream_.removeNewFrameListener(this);
    stream_.stop();
    stream_.destroy();

    publisher_.shutdown();
  }

  virtual VideoStream& stream()
  {
    return stream_;
  }

  virtual bool beginConfigure()
  {
    was_running_ = running_;
    if(was_running_) stream_.stop();
    running_ = false;

    return true;
  }

  virtual void endConfigure()
  {
    if(was_running_)
    {
      Status rc = stream_.start();

      if(rc != STATUS_OK)
      {
        SensorType type = stream_.getSensorInfo().getSensorType();
        ROS_WARN_STREAM("Failed to restart stream '" << name_ << "' after configuration!");

        int max_trials = 1;

        for(int trials = 0; trials < max_trials && rc != STATUS_OK; ++trials)
        {
          ros::Duration(0.1).sleep();

          stream_.removeNewFrameListener(this);
          stream_.destroy();
          stream_.create(device_, type);
          stream_.addNewFrameListener(this);
          //stream_.setVideoMode(default_mode_);
          rc = stream_.start();

          ROS_WARN_STREAM_COND(rc != STATUS_OK, "Recovery trial " << trials << " failed!");
        }

        ROS_ERROR_STREAM_COND(rc != STATUS_OK, "Failed to recover stream '" << name_ << "'! Restart required!");
        ROS_INFO_STREAM_COND(rc == STATUS_OK, "Recovered stream '" << name_ << "'.");
      }

      if(rc == STATUS_OK)
      {
        running_ = true;
      }
    }
  }

  virtual bool tryConfigureVideoMode(VideoMode& mode)
  {
    bool result = true;
    VideoMode old = stream_.getVideoMode();

    if(stream_.setVideoMode(mode) != STATUS_OK)
    {
      ROS_ERROR_STREAM_COND(stream_.setVideoMode(old) != STATUS_OK, "Failed to recover old video mode!");
      result = false;
    }

    return result;
  }

  virtual void onSubscriptionChanged(const image_transport::SingleSubscriberPublisher& topic)
  {
    if(topic.getNumSubscribers() > 0)
    {
      if(!running_ && stream_.start() == STATUS_OK)
      {
        running_ = true;
      }
    }
    else
    {
      stream_.stop();
      running_ = false;
    }
  }

  virtual void onNewFrame(VideoStream& stream)
  {
    ros::Time ts = ros::Time::now();

    VideoFrameRef frame;
    stream.readFrame(&frame);

    sensor_msgs::Image::Ptr img(new sensor_msgs::Image);
    sensor_msgs::CameraInfo::Ptr info(new sensor_msgs::CameraInfo);

    double scale = double(frame.getWidth()) / double(1280);

    info->header.stamp = ts;
    info->header.frame_id = frame_id_;
    info->width = frame.getWidth();
    info->height = frame.getHeight();
    info->K.assign(0);
    info->K[0] = 1050.0 * scale;
    info->K[4] = 1050.0 * scale;
    info->K[2] = frame.getWidth() / 2.0 - 0.5;
    info->K[5] = frame.getHeight() / 2.0 - 0.5;
    info->P.assign(0);
    info->P[0] = 1050.0 * scale;
    info->P[5] = 1050.0 * scale;
    info->P[2] = frame.getWidth() / 2.0 - 0.5;
    info->P[6] = frame.getHeight() / 2.0 - 0.5;

    switch(frame.getVideoMode().getPixelFormat())
    {
    case PIXEL_FORMAT_GRAY8:
      img->encoding = sensor_msgs::image_encodings::MONO8;
      break;
    case PIXEL_FORMAT_GRAY16:
      img->encoding = sensor_msgs::image_encodings::MONO16;
      break;
    case PIXEL_FORMAT_YUV422:
      img->encoding = sensor_msgs::image_encodings::YUV422;
      break;
    case PIXEL_FORMAT_RGB888:
      img->encoding = sensor_msgs::image_encodings::RGB8;
      break;
    case PIXEL_FORMAT_SHIFT_9_2:
    case PIXEL_FORMAT_DEPTH_1_MM:
      img->encoding = sensor_msgs::image_encodings::TYPE_16UC1;
      break;
    default:
      ROS_WARN("Unknown OpenNI pixel format!");
      break;
    }
    img->header.stamp = ts;
    img->header.frame_id = frame_id_;
    img->height = frame.getHeight();
    img->width = frame.getWidth();
    img->step = frame.getStrideInBytes();
    img->data.resize(frame.getDataSize());
    std::copy(static_cast<const uint8_t*>(frame.getData()), static_cast<const uint8_t*>(frame.getData()) + frame.getDataSize(), img->data.begin());

    publish(img, info);
  }
};

class DepthSensorStreamManager : public SensorStreamManager
{
protected:
  ros::NodeHandle nh_registered_;
  image_transport::ImageTransport it_registered_;
  image_transport::CameraPublisher depth_registered_publisher_, disparity_publisher_, disparity_registered_publisher_, *active_publisher_;
  std::string rgb_frame_id_, depth_frame_id_;

  virtual void publish(sensor_msgs::Image::Ptr& image, sensor_msgs::CameraInfo::Ptr& camera_info)
  {
    if(active_publisher_ != 0)
      active_publisher_->publish(image, camera_info);
  }

  void updateActivePublisher()
  {
    image_transport::CameraPublisher *p_depth, *p_disparity;

    if(device_.getImageRegistrationMode() == IMAGE_REGISTRATION_DEPTH_TO_COLOR)
    {
      p_depth = &depth_registered_publisher_;
      p_disparity = &disparity_registered_publisher_;
      frame_id_ = rgb_frame_id_;
    }
    else
    {
      p_depth = &publisher_;
      p_disparity = &disparity_publisher_;
      frame_id_ = depth_frame_id_;
    }

    if(stream_.getVideoMode().getPixelFormat() == PIXEL_FORMAT_DEPTH_1_MM)
    {
      active_publisher_ = p_depth;
    }
    else if(stream_.getVideoMode().getPixelFormat() == PIXEL_FORMAT_SHIFT_9_2)
    {
      active_publisher_ = p_disparity;
    }
    else
    {
      active_publisher_ = 0;
    }
  }
public:
  DepthSensorStreamManager(ros::NodeHandle& nh, Device& device, std::string rgb_frame_id, std::string depth_frame_id, VideoMode& default_mode) :
    SensorStreamManager(nh, device, SENSOR_DEPTH, "depth", depth_frame_id, default_mode),
    nh_registered_(nh, "depth_registered"),
    it_registered_(nh_registered_),
    active_publisher_(0),
    rgb_frame_id_(rgb_frame_id),
    depth_frame_id_(depth_frame_id)
  {
    depth_registered_publisher_ = it_registered_.advertiseCamera("image_raw", 1, callback_, callback_);
    disparity_publisher_ = it_.advertiseCamera("disparity", 1, callback_, callback_);
    disparity_registered_publisher_ = it_registered_.advertiseCamera("disparity", 1, callback_, callback_);
  }

  virtual void onSubscriptionChanged(const image_transport::SingleSubscriberPublisher& topic)
  {
    size_t disparity_clients = disparity_publisher_.getNumSubscribers() + disparity_registered_publisher_.getNumSubscribers();
    size_t depth_clients = publisher_.getNumSubscribers() + depth_registered_publisher_.getNumSubscribers();
    size_t all_clients = disparity_clients + depth_clients;

    if(!running_ && all_clients > 0)
    {
      running_ = (stream_.start() == STATUS_OK);
    }
    else if(running_ && all_clients == 0)
    {
      stream_.stop();
      running_ = false;
    }

    if(running_)
    {
      updateActivePublisher();
    }
  }

  virtual void endConfigure()
  {
    SensorStreamManager::endConfigure();

    if(running_)
    {
      updateActivePublisher();
    }
  }
};

class CameraImpl
{
public:
  CameraImpl(ros::NodeHandle& nh, ros::NodeHandle& nh_private, const openni::DeviceInfo& device_info) :
    rgb_sensor_(new SensorStreamManagerBase()),
    depth_sensor_(new SensorStreamManagerBase()),
    ir_sensor_(new SensorStreamManagerBase()),
    reconfigure_server_(nh_private)
  {
    device_.open(device_info.getUri());

    printDeviceInfo();
    printVideoModes();
    buildResolutionMap();

    device_.setDepthColorSyncEnabled(true);

    std::string rgb_frame_id, depth_frame_id;
    nh_private.param(std::string("rgb_frame_id"), rgb_frame_id, std::string("camera_rgb_optical_frame"));
    nh_private.param(std::string("depth_frame_id"), depth_frame_id, std::string("camera_depth_optical_frame"));

    if(device_.hasSensor(SENSOR_COLOR))
    {
      rgb_sensor_.reset(new SensorStreamManager(nh, device_, SENSOR_COLOR, "rgb", rgb_frame_id, resolutions_[Camera_RGB_640x480_30Hz]));
    }

    if(device_.hasSensor(SENSOR_DEPTH))
    {
      depth_sensor_.reset(new DepthSensorStreamManager(nh, device_, rgb_frame_id, depth_frame_id, resolutions_[Camera_DEPTH_640x480_30Hz]));
    }

    if(device_.hasSensor(SENSOR_IR))
    {
      ir_sensor_.reset(new SensorStreamManager(nh, device_, SENSOR_IR, "ir", depth_frame_id, resolutions_[Camera_IR_640x480_30Hz]));
    }

    reconfigure_server_.setCallback(boost::bind(&CameraImpl::configure, this, _1, _2));
  }

  ~CameraImpl()
  {
    rgb_sensor_.reset();
    depth_sensor_.reset();
    ir_sensor_.reset();

    device_.close();
  }

  void printDeviceInfo()
  {
    const DeviceInfo& info = device_.getDeviceInfo();

    char buffer[512];
    int size = 512;
    std::stringstream summary;

    size = 512;
    if(device_.getProperty(DEVICE_PROPERTY_HARDWARE_VERSION, buffer, &size) == STATUS_OK)
    {
      std::string hw(buffer, size_t(size));
      summary << " Hardware: " << hw;
    }

    size = 512;
    if(device_.getProperty(DEVICE_PROPERTY_FIRMWARE_VERSION, buffer, &size) == STATUS_OK)
    {
      std::string fw(buffer, size);
      summary << " Firmware: " << fw;
    }

    size = 512;
    if(device_.getProperty(DEVICE_PROPERTY_DRIVER_VERSION, buffer, &size) == STATUS_OK)
    {
      std::string drv(buffer, size);
      summary << " Driver: " << drv;
    }

    ROS_INFO_STREAM(info.getVendor() << " " << info.getName() << summary.str());
  }

  void printVideoModes()
  {
    static const size_t ntypes = 3;
    SensorType types[ntypes] = { SENSOR_COLOR, SENSOR_DEPTH, SENSOR_IR };

    for(size_t tidx = 0; tidx < ntypes; ++tidx)
    {
      if(!device_.hasSensor(types[tidx])) continue;

      const SensorInfo* info = device_.getSensorInfo(types[tidx]);
      ROS_INFO_STREAM("  " << toString(info->getSensorType()));

      const Array<VideoMode>& modes = info->getSupportedVideoModes();

      for(int idx = 0; idx < modes.getSize(); ++idx)
      {
        const VideoMode& mode = modes[idx];
        ROS_INFO_STREAM("    " << toString(mode.getPixelFormat()) << " " << mode.getResolutionX() << "x" << mode.getResolutionY() << "@" << mode.getFps());
      }
    }
  }

  void createVideoMode(VideoMode& m, int x, int y, int fps, PixelFormat format)
  {
    m.setResolution(x, y);
    m.setFps(fps);
    m.setPixelFormat(format);
  }

  void buildResolutionMap()
  {
    createVideoMode(resolutions_[Camera_RGB_320x240_30Hz], 320, 240, 30, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_RGB_320x240_60Hz], 320, 240, 60, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_RGB_640x480_30Hz], 640, 480, 30, PIXEL_FORMAT_RGB888);
    // i don't think this one is supported as it is overridden in XnHostProtocol.cpp#L429
    createVideoMode(resolutions_[Camera_RGB_1280x720_30Hz], 1280, 720, 30, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_RGB_1280x1024_30Hz], 1280, 1024, 30, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_YUV_320x240_30Hz], 320, 240, 30, PIXEL_FORMAT_YUV422);
    createVideoMode(resolutions_[Camera_YUV_320x240_60Hz], 320, 240, 60, PIXEL_FORMAT_YUV422);
    createVideoMode(resolutions_[Camera_YUV_640x480_30Hz], 640, 480, 30, PIXEL_FORMAT_YUV422);
    createVideoMode(resolutions_[Camera_YUV_1280x1024_30Hz], 1280, 1024, 30, PIXEL_FORMAT_YUV422);

    createVideoMode(resolutions_[Camera_DEPTH_320x240_30Hz], 320, 240, 30, PIXEL_FORMAT_DEPTH_1_MM);
    createVideoMode(resolutions_[Camera_DEPTH_320x240_60Hz], 320, 240, 60, PIXEL_FORMAT_DEPTH_1_MM);
    createVideoMode(resolutions_[Camera_DEPTH_640x480_30Hz], 640, 480, 30, PIXEL_FORMAT_DEPTH_1_MM);

    createVideoMode(resolutions_[Camera_DISPARITY_320x240_30Hz], 320, 240, 30, PIXEL_FORMAT_SHIFT_9_2);
    createVideoMode(resolutions_[Camera_DISPARITY_320x240_60Hz], 320, 240, 60, PIXEL_FORMAT_SHIFT_9_2);
    createVideoMode(resolutions_[Camera_DISPARITY_640x480_30Hz], 640, 480, 30, PIXEL_FORMAT_SHIFT_9_2);

    createVideoMode(resolutions_[Camera_IR_320x240_30Hz], 320, 240, 30, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_IR_320x240_60Hz], 320, 240, 60, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_IR_640x480_30Hz], 640, 480, 30, PIXEL_FORMAT_RGB888);
    createVideoMode(resolutions_[Camera_IR_1280x1024_30Hz], 1280, 1024, 30, PIXEL_FORMAT_RGB888);
  }

  void configure(CameraConfig& cfg, uint32_t level)
  {
    if(rgb_sensor_->beginConfigure())
    {
      if((level & 8) != 0)
      {
        ResolutionMap::iterator e = resolutions_.find(cfg.rgb_resolution);
        assert(e != resolutions_.end());

        rgb_sensor_->tryConfigureVideoMode(e->second);
      }

      if((level & 2) != 0)
      {
        rgb_sensor_->stream().getCameraSettings()->setAutoExposureEnabled(cfg.auto_exposure);
      }

      if((level & 4) != 0)
      {
        rgb_sensor_->stream().getCameraSettings()->setAutoWhiteBalanceEnabled(cfg.auto_white_balance);
      }

      if((level & 64) != 0)
      {
        rgb_sensor_->stream().setMirroringEnabled(cfg.mirror);
      }
      rgb_sensor_->endConfigure();
    }

    if(depth_sensor_->beginConfigure())
    {
      if((level & 16) != 0)
      {
        ResolutionMap::iterator e = resolutions_.find(cfg.depth_resolution);
        assert(e != resolutions_.end());

        depth_sensor_->tryConfigureVideoMode(e->second);
      }

      if((level & 1) != 0)
      {
        if(cfg.depth_registration)
        {
          if(device_.isImageRegistrationModeSupported(IMAGE_REGISTRATION_DEPTH_TO_COLOR))
          {
            device_.setImageRegistrationMode(IMAGE_REGISTRATION_DEPTH_TO_COLOR);
          }
          else
          {
            cfg.depth_registration = false;
          }
        }
        else
        {
          device_.setImageRegistrationMode(IMAGE_REGISTRATION_OFF);
        }
      }

      if((level & 64) != 0)
      {
        depth_sensor_->stream().setMirroringEnabled(cfg.mirror);
      }

      depth_sensor_->endConfigure();
    }


    if(ir_sensor_->beginConfigure())
    {
      if((level & 32) != 0)
      {
        ResolutionMap::iterator e = resolutions_.find(cfg.ir_resolution);
        assert(e != resolutions_.end());

        ir_sensor_->tryConfigureVideoMode(e->second);
      }

      if((level & 64) != 0)
      {
        ir_sensor_->stream().setMirroringEnabled(cfg.mirror);
      }

      ir_sensor_->endConfigure();
    }

    device_.setDepthColorSyncEnabled(true);
  }
private:
  boost::shared_ptr<SensorStreamManagerBase> rgb_sensor_, depth_sensor_, ir_sensor_;
  dynamic_reconfigure::Server<CameraConfig> reconfigure_server_;

  typedef std::map<int, VideoMode> ResolutionMap;

  ResolutionMap resolutions_;

  Device device_;
};


} /* namespace internal */

Camera::Camera(ros::NodeHandle& nh, ros::NodeHandle& nh_private, const openni::DeviceInfo& device_info) :
    impl_(new internal::CameraImpl(nh, nh_private, device_info))
{
}

Camera::~Camera()
{
  delete impl_;
}

} /* namespace openni2_camera */
