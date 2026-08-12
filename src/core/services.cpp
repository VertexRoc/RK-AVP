#include "rkavp/services.hpp"

#include <fstream>
#include <iterator>

namespace rkavp {

Status SidePacketSet::Set(std::string name, Packet packet) {
  if (name.empty() || packet.empty())
    return Status::Invalid("side packet name and value are required");
  if (!packets_.emplace(std::move(name), std::move(packet)).second) {
    return Status::AlreadyExists("side packet is immutable and already set");
  }
  return Status::Ok();
}

const Packet* SidePacketSet::Find(const std::string& name) const {
  const auto it = packets_.find(name);
  return it == packets_.end() ? nullptr : &it->second;
}

FileResourceManager::FileResourceManager(std::string root) : root_(std::move(root)) {}

Status FileResourceManager::Read(const std::string& uri, std::string* data) const {
  if (data == nullptr) return Status::Invalid("resource output is null");
  if (uri.empty() || uri.find("..") != std::string::npos)
    return Status::Invalid("invalid resource URI");
  const std::string path = root_.empty() || uri.front() == '/' ? uri : root_ + "/" + uri;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return Status::NotFound("resource not found: " + uri);
  data->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return Status::Ok();
}

}  // namespace rkavp
