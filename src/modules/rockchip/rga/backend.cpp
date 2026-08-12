#include <fcntl.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <im2d.hpp>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "../backend_factory.hpp"

namespace rkavp {
namespace {

struct DmaHeapAllocationData {
  std::uint64_t len;
  std::uint32_t fd;
  std::uint32_t fd_flags;
  std::uint64_t heap_flags;
};
#ifndef DMA_HEAP_IOCTL_ALLOC
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct DmaHeapAllocationData)
#endif

int RgaFormat(const std::string& format) {
  if (format == "nv12") return RK_FORMAT_YCbCr_420_SP;
  if (format == "nv21") return RK_FORMAT_YCrCb_420_SP;
  if (format == "rgb888") return RK_FORMAT_RGB_888;
  if (format == "bgr888") return RK_FORMAT_BGR_888;
  if (format == "rgba8888") return RK_FORMAT_RGBA_8888;
  if (format == "bgra8888") return RK_FORMAT_BGRA_8888;
  if (format == "yuyv") return RK_FORMAT_YUYV_422;
  return -1;
}

std::size_t ImageSize(int width, int height, const std::string& format) {
  if (format == "nv12" || format == "nv21") return static_cast<std::size_t>(width) * height * 3 / 2;
  if (format == "rgb888" || format == "bgr888") return static_cast<std::size_t>(width) * height * 3;
  if (format == "rgba8888" || format == "bgra8888")
    return static_cast<std::size_t>(width) * height * 4;
  if (format == "yuyv") return static_cast<std::size_t>(width) * height * 2;
  return 0;
}

Status AllocateDmaBuffer(std::size_t size, BufferPtr* output) {
  if (output == nullptr || size == 0) return Status::Invalid("invalid DMA allocation request");
  const char* heaps[] = {"/dev/dma_heap/system-uncached", "/dev/dma_heap/system",
                         "/dev/dma_heap/reserved"};
  for (const char* heap : heaps) {
    const int heap_fd = ::open(heap, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) continue;
    DmaHeapAllocationData request{};
    request.len = size;
    request.fd_flags = O_RDWR | O_CLOEXEC;
    const int result = ::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &request);
    ::close(heap_fd);
    if (result == 0) {
      *output = DmaBuffer::Adopt(static_cast<int>(request.fd), size);
      return Status::Ok();
    }
  }
  return Status::Unavailable(std::string("DMA heap allocation failed: ") + std::strerror(errno));
}

class ImportedRgaHandle final {
 public:
  explicit ImportedRgaHandle(rga_buffer_handle_t handle) : handle_(handle) {}
  ~ImportedRgaHandle() {
    if (handle_ != 0) releasebuffer_handle(handle_);
  }

  ImportedRgaHandle(const ImportedRgaHandle&) = delete;
  ImportedRgaHandle& operator=(const ImportedRgaHandle&) = delete;

  rga_buffer_handle_t get() const { return handle_; }

 private:
  rga_buffer_handle_t handle_ = 0;
};

using ImportedRgaHandlePtr = std::shared_ptr<ImportedRgaHandle>;

class RgaFence final : public Fence {
 public:
  RgaFence(int fd, std::vector<BufferPtr> buffers, std::vector<ImportedRgaHandlePtr> handles)
      : fd_(fd), buffers_(std::move(buffers)), handles_(std::move(handles)) {}
  ~RgaFence() override { Wait(-1); }
  Status Wait(std::int64_t) override {
    if (fd_ < 0) return Status::Ok();
    const IM_STATUS result = imsync(fd_);
    fd_ = -1;
    buffers_.clear();
    handles_.clear();
    return result == IM_STATUS_SUCCESS ? Status::Ok()
                                       : Status::Unavailable("RGA fence wait failed");
  }

 private:
  int fd_ = -1;
  std::vector<BufferPtr> buffers_;
  std::vector<ImportedRgaHandlePtr> handles_;
};

class RockchipRgaBackend final : public IRgaBackend {
 public:
  ~RockchipRgaBackend() override { ReleaseCachedHandles(); }

  Status Transform(const VideoFrame& source, const RgaTransformRequest& request,
                   VideoFrame* destination) override {
    if (destination == nullptr || !source.buffer || source.buffer->planes().empty())
      return Status::Invalid("invalid RGA transform input");
    const int source_format = RgaFormat(source.format);
    const int destination_format = RgaFormat(request.destination_format);
    if (source_format < 0 || destination_format < 0 || request.destination_width <= 0 ||
        request.destination_height <= 0) {
      return Status::Invalid("unsupported RGA format or geometry");
    }
    const std::size_t size = ImageSize(request.destination_width, request.destination_height,
                                       request.destination_format);
    Status status = AllocateDmaBuffer(size, &destination->buffer);
    if (!status.ok()) return status;
    const ImportedRgaHandlePtr source_handle = Import(source.buffer);
    const ImportedRgaHandlePtr destination_handle = Import(destination->buffer);
    if (!source_handle || !destination_handle)
      return Status::Unavailable("RGA DMA-BUF import failed");
    rga_buffer_t source_buffer =
        wrapbuffer_handle(source_handle->get(), source.width, source.height, source_format,
                          source.horizontal_stride > 0 ? source.horizontal_stride : source.width,
                          source.vertical_stride > 0 ? source.vertical_stride : source.height);
    rga_buffer_t destination_buffer = wrapbuffer_handle(
        destination_handle->get(), request.destination_width, request.destination_height,
        destination_format, request.destination_width, request.destination_height);
    im_rect source_rect{
        request.source_rect.x, request.source_rect.y,
        request.source_rect.width > 0 ? request.source_rect.width : source.width,
        request.source_rect.height > 0 ? request.source_rect.height : source.height};
    im_rect destination_rect{request.destination_rect.x, request.destination_rect.y,
                             request.destination_rect.width > 0 ? request.destination_rect.width
                                                                : request.destination_width,
                             request.destination_rect.height > 0 ? request.destination_rect.height
                                                                 : request.destination_height};
    im_rect empty{};
    int usage = request.asynchronous ? IM_ASYNC : IM_SYNC;
    if (request.rotation_degrees == 90)
      usage |= IM_HAL_TRANSFORM_ROT_90;
    else if (request.rotation_degrees == 180)
      usage |= IM_HAL_TRANSFORM_ROT_180;
    else if (request.rotation_degrees == 270)
      usage |= IM_HAL_TRANSFORM_ROT_270;
    if (source.buffer->fence()) {
      status = source.buffer->fence()->Wait(-1);
      if (!status.ok()) return status;
    }
    int release_fence = -1;
    const int acquire_fence = -1;
    const IM_STATUS result =
        improcess(source_buffer, destination_buffer, {}, source_rect, destination_rect, empty,
                  acquire_fence, &release_fence, nullptr, usage);
    if (result != IM_STATUS_SUCCESS)
      return Status::Unavailable(std::string("RGA transform failed: ") + imStrError(result));
    destination->frame_id = source.frame_id;
    destination->pts = source.pts;
    destination->width = request.destination_width;
    destination->height = request.destination_height;
    destination->horizontal_stride = request.destination_width;
    destination->vertical_stride = request.destination_height;
    destination->format = request.destination_format;
    if (release_fence >= 0) {
      destination->buffer->set_fence(std::make_shared<RgaFence>(
          release_fence, std::vector<BufferPtr>{source.buffer},
          std::vector<ImportedRgaHandlePtr>{source_handle, destination_handle}));
    }
    return Status::Ok();
  }

  Status Blit(const VideoFrame& source, const RgaTransformRequest& request,
              VideoFrame* destination) override {
    return Transform(source, request, destination);
  }

  Status Composite(const VideoFrame& foreground, const VideoFrame& background,
                   const RgaCompositeRequest& request, VideoFrame* destination) override {
    if (destination == nullptr || !foreground.buffer || !background.buffer ||
        foreground.buffer->planes().empty() || background.buffer->planes().empty()) {
      return Status::Invalid("invalid RGA composite input");
    }
    const auto rect_is_default = [](const Rect& rect) {
      return rect.x == 0 && rect.y == 0 && rect.width == 0 && rect.height == 0;
    };
    if (!rect_is_default(request.foreground_rect) || !rect_is_default(request.background_rect) ||
        !rect_is_default(request.destination_rect)) {
      return Status::Invalid(
          "RGA composite rectangles are not implemented by this backend; pre-transform inputs "
          "before compositing");
    }
    if (foreground.buffer->fence()) {
      Status status = foreground.buffer->fence()->Wait(-1);
      if (!status.ok()) return status;
    }
    if (background.buffer->fence()) {
      Status status = background.buffer->fence()->Wait(-1);
      if (!status.ok()) return status;
    }
    const std::size_t size = ImageSize(background.width, background.height, background.format);
    Status status = AllocateDmaBuffer(size, &destination->buffer);
    if (!status.ok()) return status;
    const int fg_format = RgaFormat(foreground.format);
    const int bg_format = RgaFormat(background.format);
    if (fg_format < 0 || bg_format < 0) return Status::Invalid("unsupported RGA composite format");
    const ImportedRgaHandlePtr foreground_handle = Import(foreground.buffer);
    const ImportedRgaHandlePtr background_handle = Import(background.buffer);
    const ImportedRgaHandlePtr destination_handle = Import(destination->buffer);
    if (!foreground_handle || !background_handle || !destination_handle) {
      return Status::Unavailable("RGA DMA-BUF import failed");
    }
    rga_buffer_t fg =
        wrapbuffer_handle(foreground_handle->get(), foreground.width, foreground.height, fg_format);
    rga_buffer_t bg =
        wrapbuffer_handle(background_handle->get(), background.width, background.height, bg_format);
    rga_buffer_t dst = wrapbuffer_handle(destination_handle->get(), background.width,
                                         background.height, bg_format);
    imsetOpacity(&fg,
                 static_cast<std::uint8_t>(std::max(0.0F, std::min(1.0F, request.alpha)) * 255.0F));
    int release_fence = -1;
    const IM_STATUS result = imcomposite(fg, bg, dst, IM_ALPHA_BLEND_SRC_OVER,
                                         request.asynchronous ? IM_ASYNC : IM_SYNC, &release_fence);
    if (result != IM_STATUS_SUCCESS)
      return Status::Unavailable(std::string("RGA composite failed: ") + imStrError(result));
    BufferPtr composed_buffer = std::move(destination->buffer);
    *destination = background;
    destination->buffer = std::move(composed_buffer);
    if (release_fence >= 0) {
      destination->buffer->set_fence(std::make_shared<RgaFence>(
          release_fence, std::vector<BufferPtr>{foreground.buffer, background.buffer},
          std::vector<ImportedRgaHandlePtr>{foreground_handle, background_handle,
                                            destination_handle}));
    }
    return Status::Ok();
  }

  void ReleaseCachedHandles() override {
    std::lock_guard<std::mutex> lock(mutex_);
    handles_.clear();
  }

 private:
  struct CacheEntry {
    std::weak_ptr<Buffer> buffer;
    ImportedRgaHandlePtr handle;
    std::uint64_t last_used = 0;
  };

  ImportedRgaHandlePtr Import(const BufferPtr& buffer) {
    if (!buffer || buffer->planes().empty() || buffer->planes().front().fd < 0 ||
        buffer->size() == 0 ||
        buffer->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = handles_.begin(); it != handles_.end();) {
      if (it->second.buffer.expired())
        it = handles_.erase(it);
      else
        ++it;
    }
    const Buffer* key = buffer.get();
    const auto existing = handles_.find(key);
    if (existing != handles_.end()) {
      const auto owner = existing->second.buffer.lock();
      if (owner && owner.get() == key) {
        existing->second.last_used = ++use_counter_;
        return existing->second.handle;
      }
      handles_.erase(existing);
    }
    if (handles_.size() >= kMaxCachedHandles) {
      const auto oldest = std::min_element(handles_.begin(), handles_.end(),
                                           [](const auto& left, const auto& right) {
                                             return left.second.last_used < right.second.last_used;
                                           });
      if (oldest != handles_.end()) handles_.erase(oldest);
    }
    const rga_buffer_handle_t imported =
        importbuffer_fd(buffer->planes().front().fd, static_cast<int>(buffer->size()));
    if (imported == 0) return {};
    auto handle = std::make_shared<ImportedRgaHandle>(imported);
    handles_.emplace(key, CacheEntry{buffer, handle, ++use_counter_});
    return handle;
  }
  static constexpr std::size_t kMaxCachedHandles = 64;
  std::mutex mutex_;
  std::unordered_map<const Buffer*, CacheEntry> handles_;
  std::uint64_t use_counter_ = 0;
};

}  // namespace

std::unique_ptr<IRgaBackend> CreateRockchipRga() { return std::make_unique<RockchipRgaBackend>(); }

}  // namespace rkavp
