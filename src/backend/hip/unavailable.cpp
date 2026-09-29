#include "keyhunt/backend/device.h"
#include <stdexcept>

namespace keyhunt::backend {
bool hip_available() { return false; }
DeviceInventory discover_hip() {
    throw std::runtime_error("HIP backend is not built; configure with KEYHUNT_ENABLE_HIP=ON");
}
}
