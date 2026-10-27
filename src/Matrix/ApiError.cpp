#include "NxMatrix/Matrix/ApiError.hpp"

namespace NxMatrix::Matrix {

bool ApiError::IsTransportError() const {
    return !TransportError.empty();
}

std::string ApiError::ToDisplayString() const {
    if(IsTransportError()) {
        return "Network error: " + TransportError;
    }
    std::string Out = "HTTP " + std::to_string(StatusCode);
    if(!ErrCode.empty()) {
        Out += " " + ErrCode;
    }
    if(!Message.empty()) {
        Out += ": " + Message;
    }
    return Out;
}

} // namespace NxMatrix::Matrix