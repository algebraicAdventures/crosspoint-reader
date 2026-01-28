#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class ImageViewerActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  enum class State {
    Browsing,
    Viewing,
    Slideshow,
    Settings,
    ImageSettings  // Brightness/contrast adjustment for current image
  };

  State state = State::Browsing;

  // Browsing state
  std::string basepath = "/";
  std::vector<std::string> files;
  int selectorIndex = 0;

  // Viewing/Slideshow state
  std::vector<std::string> imageFiles; // List of images in current folder
  int currentImageIndex = 0;
  unsigned long lastSlideTime = 0;

  // Slideshow Settings
  int slideshowIntervalSeconds = 5;
  const int availableIntervals[5] = {3, 5, 10, 30, 60};
  int intervalIndex = 1; // Default to 5s

  // Image Settings (brightness/contrast)
  int brightness = 0;         // Range: -50 to +50, default 0
  int contrast = 0;           // Range: -50 to +50, default 0
  int imageSettingSelection = 0;  // 0 = brightness, 1 = contrast

  bool updateRequired = false;

  // Callbacks
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();

  void render() const;
  void renderBrowser() const;
  void renderViewer() const;
  void renderSettings() const;
  void renderImageSettings() const;

  void loadFiles();
  void loadImageList();
  void enterFolder(const std::string& folderName);
  void openImage(const std::string& imageName);
  void startSlideshow(const std::string& startImage = "");
  void nextSlide();
  void prevSlide();

 public:
  explicit ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onGoHome)
      : Activity("ImageViewer", renderer, mappedInput),
        onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
};
