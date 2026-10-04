#pragma once

namespace object_model {
constexpr const char* kFileName = "objects_224.espdl";
constexpr const char* kClassNames[] = {"Battery", "Marker", "Screwdriver"};
constexpr unsigned kClassCount = 3;
inline const char* className(unsigned category) {
  return category < kClassCount ? kClassNames[category] : "Object";
}
}
