#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#include "../backend_factory.hpp"

namespace rkavp {
namespace {

int IoctlRetry(int fd, unsigned long request, void* argument) {
  int result;
  do {
    result = ::ioctl(fd, request, argument);
  } while (result < 0 && errno == EINTR);
  return result;
}

std::uint32_t V4l2Format(const std::string& format) {
  if (format == "mjpeg" || format == "jpeg") return V4L2_PIX_FMT_MJPEG;
  if (format == "h264") return V4L2_PIX_FMT_H264;
  if (format == "h265" || format == "hevc") return V4L2_PIX_FMT_HEVC;
  if (format == "nv12") return V4L2_PIX_FMT_NV12;
  if (format == "nv21") return V4L2_PIX_FMT_NV21;
  if (format == "yuyv") return V4L2_PIX_FMT_YUYV;
  return 0;
}

struct CaptureState {
  struct Mapping {
    void* address = nullptr;
    std::size_t length = 0;
    int dma_fd = -1;
  };
  int fd = -1;
  int wake_read = -1;
  int wake_write = -1;
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  std::vector<Mapping> mappings;
  std::mutex mutex;
  bool streaming = false;
  bool closing = false;

  ~CaptureState() {
    std::lock_guard<std::mutex> lock(mutex);
    closing = true;
    if (streaming && fd >= 0) IoctlRetry(fd, VIDIOC_STREAMOFF, &type);
    for (auto& mapping : mappings) {
      if (mapping.address != nullptr && mapping.address != MAP_FAILED)
        ::munmap(mapping.address, mapping.length);
      if (mapping.dma_fd >= 0) ::close(mapping.dma_fd);
    }
    if (fd >= 0) ::close(fd);
    if (wake_read >= 0) ::close(wake_read);
    if (wake_write >= 0) ::close(wake_write);
  }
};

class V4l2LeaseBuffer final : public Buffer {
 public:
  V4l2LeaseBuffer(std::shared_ptr<CaptureState> state, std::uint32_t index, std::size_t bytes_used,
                  std::size_t row_stride, MemoryType memory_type)
      : state_(std::move(state)),
        index_(index),
        bytes_used_(bytes_used),
        memory_type_(memory_type) {
    const auto& mapping = state_->mappings[index_];
    planes_.push_back({mapping.dma_fd, 0, bytes_used_, row_stride, 1, 0});
  }
  ~V4l2LeaseBuffer() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || !state_->streaming) return;
    v4l2_buffer buffer{};
    buffer.type = state_->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index_;
    IoctlRetry(state_->fd, VIDIOC_QBUF, &buffer);
  }
  MemoryType memory_type() const override { return memory_type_; }
  std::size_t size() const override { return bytes_used_; }
  const std::vector<BufferPlane>& planes() const override { return planes_; }
  void* Map(MapAccess) override { return state_->mappings[index_].address; }

 private:
  std::shared_ptr<CaptureState> state_;
  std::uint32_t index_;
  std::size_t bytes_used_;
  MemoryType memory_type_ = MemoryType::kHost;
  std::vector<BufferPlane> planes_;
};

class LinuxV4l2Backend final : public IV4l2Backend {
 public:
  Status Open(const V4l2CaptureConfig& config) override {
    Close();
    const std::uint32_t pixel_format = V4l2Format(config.format);
    if (config.device.empty() || pixel_format == 0 || config.width <= 0 || config.height <= 0 ||
        config.fps <= 0) {
      return Status::Invalid("invalid V4L2 capture configuration");
    }
    auto state = std::make_shared<CaptureState>();
    state->fd = ::open(config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (state->fd < 0)
      return Status::Unavailable(std::string("open V4L2 device failed: ") + std::strerror(errno));
    int wake[2];
    if (::pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0)
      return Status::Internal("create V4L2 wake pipe failed");
    state->wake_read = wake[0];
    state->wake_write = wake[1];

    v4l2_capability capability{};
    if (IoctlRetry(state->fd, VIDIOC_QUERYCAP, &capability) != 0 ||
        (capability.capabilities & V4L2_CAP_STREAMING) == 0) {
      return Status::Unavailable("V4L2 device does not support streaming");
    }
    v4l2_format format{};
    format.type = state->type;
    format.fmt.pix.width = static_cast<std::uint32_t>(config.width);
    format.fmt.pix.height = static_cast<std::uint32_t>(config.height);
    format.fmt.pix.pixelformat = pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (IoctlRetry(state->fd, VIDIOC_S_FMT, &format) != 0) {
      return Status::Unavailable(std::string("VIDIOC_S_FMT failed: ") + std::strerror(errno));
    }
    v4l2_streamparm parameters{};
    parameters.type = state->type;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(config.fps);
    IoctlRetry(state->fd, VIDIOC_S_PARM, &parameters);

    v4l2_requestbuffers request{};
    request.count = static_cast<std::uint32_t>(config.buffer_count);
    request.type = state->type;
    request.memory = V4L2_MEMORY_MMAP;
    if (IoctlRetry(state->fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) {
      return Status::Unavailable("V4L2 buffer allocation failed");
    }
    state->mappings.resize(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
      v4l2_buffer buffer{};
      buffer.type = state->type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      if (IoctlRetry(state->fd, VIDIOC_QUERYBUF, &buffer) != 0)
        return Status::Unavailable("VIDIOC_QUERYBUF failed");
      auto& mapping = state->mappings[index];
      mapping.length = buffer.length;
      mapping.address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               state->fd, buffer.m.offset);
      if (mapping.address == MAP_FAILED) return Status::Unavailable("mmap V4L2 buffer failed");
      v4l2_exportbuffer export_buffer{};
      export_buffer.type = state->type;
      export_buffer.index = index;
      export_buffer.flags = O_CLOEXEC;
      if (IoctlRetry(state->fd, VIDIOC_EXPBUF, &export_buffer) == 0)
        mapping.dma_fd = export_buffer.fd;
      if (config.io_mode == V4l2IoMode::kExportDmaBuf && mapping.dma_fd < 0) {
        return Status::Unavailable("V4L2 device cannot export DMA-BUF");
      }
      if (IoctlRetry(state->fd, VIDIOC_QBUF, &buffer) != 0)
        return Status::Unavailable("VIDIOC_QBUF failed");
    }
    if (IoctlRetry(state->fd, VIDIOC_STREAMON, &state->type) != 0)
      return Status::Unavailable("VIDIOC_STREAMON failed");
    state->streaming = true;
    state_ = std::move(state);
    config_ = config;
    config_.width = static_cast<int>(format.fmt.pix.width);
    config_.height = static_cast<int>(format.fmt.pix.height);
    bytes_per_line_ = format.fmt.pix.bytesperline;
    sequence_ = 0;
    stop_ = false;
    return Status::Ok();
  }

  Status Read(Packet* packet) override {
    if (packet == nullptr || !state_) return Status::FailedPrecondition("V4L2 backend is not open");
    if (stop_) return Status::Cancelled("V4L2 capture stopped");
    pollfd descriptors[2]{{state_->fd, POLLIN, 0}, {state_->wake_read, POLLIN, 0}};
    const int result = ::poll(descriptors, 2, 2000);
    if (result < 0)
      return errno == EINTR
                 ? Status::Unavailable("V4L2 poll interrupted")
                 : Status::Internal(std::string("V4L2 poll failed: ") + std::strerror(errno));
    if (result == 0) return Status::Unavailable("V4L2 poll timed out");
    if ((descriptors[1].revents & POLLIN) != 0 || stop_)
      return Status::Cancelled("V4L2 capture stopped");
    if ((descriptors[0].revents & POLLIN) == 0) return Status::Unavailable("V4L2 frame not ready");
    v4l2_buffer buffer{};
    buffer.type = state_->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (IoctlRetry(state_->fd, VIDIOC_DQBUF, &buffer) != 0) {
      if (errno == EAGAIN) return Status::Unavailable("V4L2 dequeue would block");
      return Status::Internal(std::string("VIDIOC_DQBUF failed: ") + std::strerror(errno));
    }
    const Timestamp timestamp = Timestamp::FromMicroseconds(
        static_cast<std::int64_t>(buffer.timestamp.tv_sec) * 1000000LL + buffer.timestamp.tv_usec);
    BufferPtr payload = std::make_shared<V4l2LeaseBuffer>(
        state_, buffer.index, buffer.bytesused, bytes_per_line_,
        config_.io_mode == V4l2IoMode::kExportDmaBuf ? MemoryType::kDmaBuf : MemoryType::kHost);
    if (config_.format == "mjpeg" || config_.format == "jpeg" || config_.format == "h264" ||
        config_.format == "h265" || config_.format == "hevc") {
      EncodedPacket encoded;
      encoded.pts = timestamp;
      encoded.dts = timestamp;
      encoded.codec = config_.format == "mjpeg" ? "mjpeg" : config_.format;
      encoded.buffer = std::move(payload);
      *packet = Packet::Make(std::move(encoded), timestamp);
    } else {
      VideoFrame frame;
      frame.frame_id = sequence_++;
      frame.pts = timestamp;
      frame.width = config_.width;
      frame.height = config_.height;
      frame.horizontal_stride = static_cast<int>(bytes_per_line_);
      frame.vertical_stride = config_.height;
      frame.format = config_.format;
      frame.buffer = std::move(payload);
      *packet = Packet::Make(std::move(frame), timestamp);
    }
    return Status::Ok();
  }

  void RequestStop() override {
    stop_ = true;
    if (state_ && state_->wake_write >= 0) {
      const std::uint8_t byte = 1;
      ssize_t result;
      do {
        result = ::write(state_->wake_write, &byte, sizeof(byte));
      } while (result < 0 && errno == EINTR);
      // EAGAIN means the non-blocking pipe already contains a wake byte. Other write failures do
      // not lose cancellation because Read() also checks stop_ before and after poll().
    }
  }

  void Close() override {
    RequestStop();
    if (state_) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->closing = true;
      if (state_->streaming) {
        IoctlRetry(state_->fd, VIDIOC_STREAMOFF, &state_->type);
        state_->streaming = false;
      }
    }
    state_.reset();
  }

 private:
  std::shared_ptr<CaptureState> state_;
  V4l2CaptureConfig config_;
  std::size_t bytes_per_line_ = 0;
  std::uint64_t sequence_ = 0;
  std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<IV4l2Backend> CreateLinuxV4l2() { return std::make_unique<LinuxV4l2Backend>(); }

}  // namespace rkavp
