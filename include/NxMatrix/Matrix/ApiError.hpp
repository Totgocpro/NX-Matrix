#pragma once
#include <string>
#include <utility>

namespace NxMatrix::Matrix {

// Error of a Matrix Client-Server API call: HTTP error, Matrix error body
// (errcode/error) or a transport failure (curl level).
struct ApiError {
    long StatusCode = 0;
    std::string ErrCode;
    std::string Message;
    std::string TransportError;

    bool IsTransportError() const;
    std::string ToDisplayString() const;
};

template <typename T> struct Result {
    bool Ok = false;
    T Value{};
    ApiError Error{};

    static Result Success(T InValue) {
        Result Out;
        Out.Ok = true;
        Out.Value = std::move(InValue);
        return Out;
    }

    static Result Failure(ApiError InError) {
        Result Out;
        Out.Ok = false;
        Out.Error = std::move(InError);
        return Out;
    }
};

struct Status {
    bool Ok = false;
    ApiError Error{};

    static Status Success() {
        Status Out;
        Out.Ok = true;
        return Out;
    }

    static Status Failure(ApiError InError) {
        Status Out;
        Out.Ok = false;
        Out.Error = std::move(InError);
        return Out;
    }
};

} // namespace NxMatrix::Matrix