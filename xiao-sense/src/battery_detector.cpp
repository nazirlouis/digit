#include "battery_detector.h"
#include "dl_detect_espdet_postprocessor.hpp"
#include "object_model_config.h"

BatteryDetector::BatteryDetector() {
  m_model = new dl::Model("pedestrian_det", object_model::kFileName,
                          fbs::MODEL_LOCATION_IN_FLASH_PARTITION);
  m_model->minimize();
  // The Roboflow images contain black padding and RGB values normalized by 255.
  m_image_preprocessor = new dl::image::ImagePreprocessor(
      m_model, {0, 0, 0}, {255, 255, 255});
  m_image_preprocessor->enable_letterbox({0, 0, 0});
  m_postprocessor = new dl::detect::ESPDetPostProcessor(
      m_model, m_image_preprocessor, 0.35f, 0.45f, 10,
      {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
}
