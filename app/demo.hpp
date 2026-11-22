#pragma once

#include "flock/core.hpp"

namespace flockv0::app {
std::unique_ptr<Model> makeDemoModel(std::string question = {});
std::unique_ptr<T3> makeDemoT3();
}
