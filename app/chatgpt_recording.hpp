#pragma once

#include "chatgpt.hpp"
#include "flock/detail/traffic_recording.hpp"

namespace flockv0::app::chatgpt {

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
