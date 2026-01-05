#include "ImageViewerActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Bitmap.h>
#include <JpegToBmpConverter.h>
#include <algorithm> // for std::sort
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;

bool isImageFile(const std::string& filename) {
  std::string ext4 = filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
  std::transform(ext4.begin(), ext4.end(), ext4.begin(), ::tolower);
  if (ext4 == ".bmp" || ext4 == ".jpg" || ext4 == ".jpeg") { // Added jpeg
      return true;
  }
  return false;
}

bool isJpeg(const std::string& filename) {
  std::string ext4 = filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
  std::string ext5 = filename.length() >= 5 ? filename.substr(filename.length() - 5) : "";
  std::transform(ext4.begin(), ext4.end(), ext4.begin(), ::tolower);
  std::transform(ext5.begin(), ext5.end(), ext5.begin(), ::tolower);
  return ext4 == ".jpg" || ext5 == ".jpeg";
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

}  // namespace

void ImageViewerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ImageViewerActivity*>(param);
  self->displayTaskLoop();
}

void ImageViewerActivity::loadFiles() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  files.clear();
  selectorIndex = 0;

  // Add "Play Slideshow" option if current folder has images
  bool hasImages = false;

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    xSemaphoreGive(renderingMutex);
    return;
  }

  root.rewindDirectory();

  char name[128];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (isImageFile(filename)) {
        files.emplace_back(filename);
        hasImages = true;
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);

  if (hasImages) {
      files.insert(files.begin(), "< Slideshow >");
  }
  xSemaphoreGive(renderingMutex);
}

void ImageViewerActivity::loadImageList() {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    imageFiles.clear();
    auto root = SdMan.open(basepath.c_str());
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        xSemaphoreGive(renderingMutex);
        return;
    }
    root.rewindDirectory();
    char name[128];
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
        file.getName(name, sizeof(name));
        if (name[0] == '.' || file.isDirectory()) {
            file.close();
            continue;
        }
        std::string filename(name);
        if (isImageFile(filename)) {
            imageFiles.emplace_back(filename);
        }
        file.close();
    }
    root.close();
    // Sort to match browser order
    sortFileList(imageFiles);
    xSemaphoreGive(renderingMutex);
}

void ImageViewerActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  selectorIndex = 0;
  state = State::Browsing;
  xSemaphoreGive(renderingMutex);

  updateRequired = true;

  xTaskCreate(&ImageViewerActivity::taskTrampoline, "ImageViewerActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ImageViewerActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  files.clear();
  imageFiles.clear();
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ImageViewerActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool settingsReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool backReleased = mappedInput.wasReleased(MappedInputManager::Button::Back);

  if (state == State::Browsing) {
      // Browsing Logic
      const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

      if (backReleased) {
        if (mappedInput.getHeldTime() >= GO_HOME_MS) {
            onGoHome();
        } else {
            std::string newPath = basepath;
            if (newPath != "/") {
                size_t lastSlash = newPath.find_last_of('/', newPath.length() - 2);
                if (lastSlash != std::string::npos) {
                     newPath.replace(lastSlash + 1, std::string::npos, "");
                } else {
                     newPath = "/";
                }

                if (newPath.empty()) newPath = "/";

                xSemaphoreTake(renderingMutex, portMAX_DELAY);
                basepath = newPath;
                xSemaphoreGive(renderingMutex);

                loadFiles();
                updateRequired = true;
            } else {
                onGoHome();
            }
        }
      } else if (confirmReleased) {
        if (files.empty()) return;

        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        std::string selected = files[selectorIndex];
        xSemaphoreGive(renderingMutex);

        if (selected == "< Slideshow >") {
             startSlideshow();
        } else if (selected.back() == '/') {
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            basepath += selected;
            xSemaphoreGive(renderingMutex);
            loadFiles();
            updateRequired = true;
        } else {
            // It's an image
            openImage(selected);
        }
      } else if (prevReleased) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (skipPage) {
          selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
        } else {
          selectorIndex = (selectorIndex + files.size() - 1) % files.size();
        }
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      } else if (nextReleased) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        if (skipPage) {
          selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
        } else {
          selectorIndex = (selectorIndex + 1) % files.size();
        }
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      } else if (settingsReleased) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = State::Settings;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      }

  } else if (state == State::Viewing) {
      if (backReleased) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = State::Browsing;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (nextReleased) {
          nextSlide();
      } else if (prevReleased) {
          prevSlide();
      } else if (confirmReleased) {
           // Toggle slideshow?
           xSemaphoreTake(renderingMutex, portMAX_DELAY);
           std::string currentImage = imageFiles[currentImageIndex];
           xSemaphoreGive(renderingMutex);
           startSlideshow(currentImage);
      } else if (settingsReleased) {
          // Open image settings (brightness/contrast)
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          imageSettingSelection = 0;  // Start with brightness selected
          state = State::ImageSettings;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      }
  } else if (state == State::Slideshow) {
      if (backReleased || confirmReleased) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = State::Viewing; // Stop slideshow
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      }
      // Timer check is done in displayTaskLoop (Wait, no, I should do it here)
      if (millis() - lastSlideTime > (unsigned long)slideshowIntervalSeconds * 1000) {
          nextSlide();
          lastSlideTime = millis();
      }
  } else if (state == State::Settings) {
      // Ignore Confirm if it is still held from the entry
      if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > 500) {
          return;
      }

      if (backReleased || confirmReleased) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = State::Browsing;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (prevReleased) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          intervalIndex = (intervalIndex - 1 + 5) % 5;
          slideshowIntervalSeconds = availableIntervals[intervalIndex];
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (nextReleased) {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          intervalIndex = (intervalIndex + 1) % 5;
          slideshowIntervalSeconds = availableIntervals[intervalIndex];
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      }
  } else if (state == State::ImageSettings) {
      // Image brightness/contrast settings
      // Use direct button checks for cleaner control
      const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
      const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
      const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
      const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);

      if (backReleased) {
          // Cancel and return to viewing
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = State::Viewing;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (confirmReleased) {
          // Apply settings and return to viewing
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          state = State::Viewing;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (upReleased || downReleased) {
          // Toggle between brightness (0) and contrast (1)
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          imageSettingSelection = (imageSettingSelection + 1) % 2;
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (leftReleased) {
          // Decrease current setting
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          if (imageSettingSelection == 0) {
              brightness = std::max(-50, brightness - 1);
          } else {
              contrast = std::max(-50, contrast - 1);
          }
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      } else if (rightReleased) {
          // Increase current setting
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          if (imageSettingSelection == 0) {
              brightness = std::min(50, brightness + 1);
          } else {
              contrast = std::min(50, contrast + 1);
          }
          xSemaphoreGive(renderingMutex);
          updateRequired = true;
      }
  }
}

void ImageViewerActivity::openImage(const std::string& imageName) {
    loadImageList();
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    for (size_t i = 0; i < imageFiles.size(); ++i) {
        if (imageFiles[i] == imageName) {
            currentImageIndex = i;
            break;
        }
    }
    state = State::Viewing;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
}

void ImageViewerActivity::startSlideshow(const std::string& startImage) {
    loadImageList();
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (imageFiles.empty()) {
        xSemaphoreGive(renderingMutex);
        return;
    }

    if (!startImage.empty()) {
        for (size_t i = 0; i < imageFiles.size(); ++i) {
            if (imageFiles[i] == startImage) {
                currentImageIndex = i;
                break;
            }
        }
    } else {
        currentImageIndex = 0;
    }

    state = State::Slideshow;
    lastSlideTime = millis();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
}

void ImageViewerActivity::nextSlide() {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (imageFiles.empty()) {
        xSemaphoreGive(renderingMutex);
        return;
    }
    currentImageIndex = (currentImageIndex + 1) % imageFiles.size();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
}

void ImageViewerActivity::prevSlide() {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (imageFiles.empty()) {
        xSemaphoreGive(renderingMutex);
        return;
    }
    currentImageIndex = (currentImageIndex - 1 + imageFiles.size()) % imageFiles.size();
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
}


void ImageViewerActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void ImageViewerActivity::render() const {
  renderer.clearScreen();

  if (state == State::Browsing) {
      renderBrowser();
  } else if (state == State::Viewing || state == State::Slideshow) {
      renderViewer();
  } else if (state == State::Settings) {
      renderSettings();
  } else if (state == State::ImageSettings) {
      renderImageSettings();
  }

  renderer.displayBuffer();
}

void ImageViewerActivity::renderBrowser() const {
  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Image Viewer", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("Back", "Open", "", "Settings");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No files found");
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }
}

void ImageViewerActivity::renderViewer() const {
    if (imageFiles.empty()) {
        renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight()/2, "No images");
        return;
    }

    std::string fullPath = basepath + imageFiles[currentImageIndex];

    bool deleteTemp = false;
    std::string fileToOpen = fullPath;

    if (isJpeg(imageFiles[currentImageIndex])) {
        // Convert to temp BMP
        FsFile jpgFile = SdMan.open(fullPath.c_str());
        if (jpgFile) {
            if (SdMan.exists("/temp_view.bmp")) {
                SdMan.remove("/temp_view.bmp");
            }
            FsFile bmpFile = SdMan.open("/temp_view.bmp", O_RDWR | O_CREAT | O_TRUNC);
            if (bmpFile) {
                if (JpegToBmpConverter::jpegFileToBmpStream(jpgFile, bmpFile)) {
                    fileToOpen = "/temp_view.bmp";
                    deleteTemp = true;
                }
                bmpFile.close();
            }
            jpgFile.close();
        }
    }

    FsFile file = SdMan.open(fileToOpen.c_str());
    if (!file) {
        renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight()/2, "Error opening file");
        if (deleteTemp) SdMan.remove("/temp_view.bmp");
        return;
    }

    Bitmap bmp(file);
    if (bmp.parseHeaders() == BmpReaderError::Ok) {
         // Apply brightness/contrast settings
         bmp.setBrightness(brightness);
         bmp.setContrast(contrast);

         const int screenWidth = renderer.getScreenWidth();
         const int screenHeight = renderer.getScreenHeight();
         const int imgWidth = bmp.getWidth();
         const int imgHeight = bmp.getHeight();

         // Center the image if it's smaller than the screen
         int x = 0;
         int y = 0;
         if (imgWidth < screenWidth) {
             x = (screenWidth - imgWidth) / 2;
         }
         if (imgHeight < screenHeight) {
             y = (screenHeight - imgHeight) / 2;
         }

         renderer.drawBitmap(bmp, x, y, screenWidth, screenHeight);
    } else {
         renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight()/2, "Invalid image format");
    }
    file.close();

    if (deleteTemp) {
        SdMan.remove("/temp_view.bmp");
    }

    // Draw overlays if not slideshow
    if (state != State::Slideshow) {
         // Show button hints only (filename shown in settings)
         const auto labels = mappedInput.mapLabels("Back", "Slideshow", "<  >", "Adjust");
         renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
}

void ImageViewerActivity::renderSettings() const {
    renderer.drawCenteredText(UI_12_FONT_ID, 50, "Slideshow Settings", true, EpdFontFamily::BOLD);

    char buf[64];
    sprintf(buf, "Interval: %d seconds", slideshowIntervalSeconds);
    renderer.drawCenteredText(UI_12_FONT_ID, 150, buf);

    renderer.drawCenteredText(UI_10_FONT_ID, 250, "< Change Interval >");
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Press Confirm/Back to Exit");
}

void ImageViewerActivity::renderImageSettings() const {
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();

    // Draw semi-transparent overlay panel
    constexpr int panelWidth = 340;
    constexpr int panelHeight = 280;
    const int panelX = (screenWidth - panelWidth) / 2;
    const int panelY = (screenHeight - panelHeight) / 2;

    // Draw panel background (white) with border
    renderer.fillRect(panelX, panelY, panelWidth, panelHeight, false);
    renderer.drawRect(panelX, panelY, panelWidth, panelHeight, true);
    renderer.drawRect(panelX + 1, panelY + 1, panelWidth - 2, panelHeight - 2, true);

    // Title
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + 20, "Image Settings", true, EpdFontFamily::BOLD);

    // Show filename below title
    if (!imageFiles.empty() && currentImageIndex < imageFiles.size()) {
        auto truncatedName = renderer.truncatedText(UI_10_FONT_ID, imageFiles[currentImageIndex].c_str(), panelWidth - 40);
        renderer.drawCenteredText(UI_10_FONT_ID, panelY + 42, truncatedName.c_str());
    }

    // Draw separator line
    renderer.drawLine(panelX + 20, panelY + 65, panelX + panelWidth - 20, panelY + 65);

    // Settings items
    constexpr int itemStartY = 90;
    constexpr int itemHeight = 60;
    constexpr int barWidth = 200;
    constexpr int barHeight = 20;
    const int barX = (screenWidth - barWidth) / 2;

    // Brightness setting
    const int brightnessY = panelY + itemStartY;
    if (imageSettingSelection == 0) {
        // Highlight selected item
        renderer.fillRect(panelX + 10, brightnessY - 5, panelWidth - 20, itemHeight + 10);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, brightnessY, "Brightness", imageSettingSelection != 0);

    // Draw brightness bar
    const int brightnessBarY = brightnessY + 25;
    renderer.drawRect(barX, brightnessBarY, barWidth, barHeight, imageSettingSelection != 0);
    // Fill bar based on brightness value (-50 to +50 mapped to 0-100%)
    const int brightnessPercent = (brightness + 50);  // 0 to 100
    const int brightnessWidth = (barWidth - 4) * brightnessPercent / 100;
    if (brightnessWidth > 0) {
        renderer.fillRect(barX + 2, brightnessBarY + 2, brightnessWidth, barHeight - 4, imageSettingSelection != 0);
    }
    // Draw center marker
    renderer.drawLine(barX + barWidth / 2, brightnessBarY - 3, barX + barWidth / 2, brightnessBarY + barHeight + 3, imageSettingSelection != 0);
    // Draw value below the bar
    char valBuf[16];
    sprintf(valBuf, "%+d", brightness);
    renderer.drawCenteredText(UI_10_FONT_ID, brightnessBarY + barHeight + 5, valBuf, imageSettingSelection != 0);

    // Contrast setting
    const int contrastY = panelY + itemStartY + itemHeight + 30;
    if (imageSettingSelection == 1) {
        // Highlight selected item
        renderer.fillRect(panelX + 10, contrastY - 5, panelWidth - 20, itemHeight + 10);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, contrastY, "Contrast", imageSettingSelection != 1);

    // Draw contrast bar
    const int contrastBarY = contrastY + 25;
    renderer.drawRect(barX, contrastBarY, barWidth, barHeight, imageSettingSelection != 1);
    // Fill bar based on contrast value (-50 to +50 mapped to 0-100%)
    const int contrastPercent = (contrast + 50);  // 0 to 100
    const int contrastWidth = (barWidth - 4) * contrastPercent / 100;
    if (contrastWidth > 0) {
        renderer.fillRect(barX + 2, contrastBarY + 2, contrastWidth, barHeight - 4, imageSettingSelection != 1);
    }
    // Draw center marker
    renderer.drawLine(barX + barWidth / 2, contrastBarY - 3, barX + barWidth / 2, contrastBarY + barHeight + 3, imageSettingSelection != 1);
    // Draw value below the bar
    sprintf(valBuf, "%+d", contrast);
    renderer.drawCenteredText(UI_10_FONT_ID, contrastBarY + barHeight + 5, valBuf, imageSettingSelection != 1);

    // Instructions at bottom
    const auto labels = mappedInput.mapLabels("Cancel", "Apply", "<  >", "Adjust");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
