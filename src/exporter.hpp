#pragma once

#include "capture.hpp"
#include "editor.hpp"

namespace rc {

struct RenderedImage {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::vector<uint8_t> bgra;
  std::vector<uint16_t> hdrRgba;
  bool hasHdr = false;
  float peakLuminanceNits = 203.0f;
};

class ImageExporter {
 public:
  bool Render(const DesktopSnapshot& snapshot, const RECT& selection,
              const EditorDocument& document, const AppConfig& config,
              bool addWindowShadow, RenderedImage& image, std::wstring& error) const;
  bool CopyToClipboard(HWND owner, const RenderedImage& image, std::wstring& error) const;
  bool Save(const RenderedImage& image, const std::filesystem::path& directory,
            std::wstring_view filenameTemplate, int jpegQuality,
            std::filesystem::path& savedPath, std::wstring& error) const;

  static std::wstring MakeFilename(std::wstring_view filenameTemplate, const SYSTEMTIME& time);

 private:
  bool SaveJpegWic(const RenderedImage& image, const std::filesystem::path& path,
                   int quality, std::wstring& error) const;
  bool SaveUltraHdr(const RenderedImage& image, const std::filesystem::path& path,
                    int quality, std::wstring& error) const;
};

}  // namespace rc
