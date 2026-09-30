#pragma once
#include "keyhunt/storage/checkpoint.h"
#include <memory>

namespace keyhunt::backend {
// Linux local control channel. The listener is created lazily by poll(), only
// after CheckpointRun owns executor.lock. close() must precede lock release.
class LocalCheckpointControl {
public:
    LocalCheckpointControl(const std::string& directory,const storage::Grant&,int device,size_t visible_devices);
    // A persistent device owns its slot lock before opening this endpoint.
    LocalCheckpointControl(const std::string& directory,int device,size_t visible_devices,const std::string& slot);
    void bind(const storage::Grant*); // nullptr means no active grant; keeps pause/stop intent.
    ~LocalCheckpointControl();
    LocalCheckpointControl(const LocalCheckpointControl&)=delete;
    storage::CheckpointControl callbacks();
    void close() noexcept;
    static std::string command(const std::string& directory,const std::string& action,const std::string& slot="");
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
