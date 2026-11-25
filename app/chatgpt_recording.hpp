#pragma once

#include "flock/json.hpp"

#include <vector>

namespace flockv0::recording {
class Capture;
}

namespace flockv0::app::chatgpt {
struct HttpResponse;

class ResponseRecording {
public:
    ResponseRecording(recording::Capture&, const Json& request);
    ~ResponseRecording();

    void event(const Json&, long status) noexcept;
    void httpError(const HttpResponse&) noexcept;
private:
    recording::Capture& capture_;
    const Json& request_;
    std::vector<Json> events_;
    long status_ = 0;
    bool collectionFailed_ = false;
};

}
