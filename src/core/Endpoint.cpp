#include "core/Endpoint.h"

namespace echo {

const char* endpointStatusName(EndpointStatus s) {
    switch (s) {
    case EndpointStatus::Starting: return "Starting";
    case EndpointStatus::Running: return "Running";
    case EndpointStatus::Lost: return "Disconnected";
    case EndpointStatus::Error: return "Error";
    case EndpointStatus::Stopped: return "Stopped";
    }
    return "Unknown";
}

} // namespace echo
