#include "controllers/rendering/controllerrenderingengine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QDomDocument>
#include <QTest>

#include "controllers/controllerenginethreadcontrol.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "helpers/log_test.h"
#include "test/mixxxtest.h"

using ::testing::_;
using namespace std::chrono_literals;

class ControllerRenderingEngineTest : public MixxxTest {
  public:
    void SetUp() override {
        mixxx::Time::setTestMode(true);
        mixxx::Time::addTestTime(10ms);
    }

    QList<QImage::Format> supportedPixelFormat() const {
        return LegacyControllerMappingFileHandler::kSupportedPixelFormat.values();
    }

  private:
    LogCaptureGuard m_logCaptureGuard;
};

class MockRenderingEngine : public ControllerRenderingEngine {
  public:
    MockRenderingEngine(const LegacyControllerMapping::ScreenInfo& info,
            ControllerEngineThreadControl* engineThreadControl)
            : ControllerRenderingEngine(info, engineThreadControl) {
    }
};

TEST_F(ControllerRenderingEngineTest, createValidRendererWithSupportedTypes) {
    const auto& supportedPixelFormats = supportedPixelFormat();
    for (const auto& pixelFormat : supportedPixelFormats) {
        ControllerEngineThreadControl engineThreadControl;
        const LegacyControllerMapping::ScreenInfo screenInfo{
                "",                                                    // identifier
                QSize(0, 0),                                           // size
                10,                                                    // target_fps
                1,                                                     // msaa
                std::chrono::milliseconds(10),                         // splash_off
                pixelFormat,                                           // pixelFormat
                LegacyControllerMapping::ScreenInfo::ColorEndian::Big, // endian
                false,                                                 // reversedColor
                false                                                  // rawData
        };
        MockRenderingEngine screenTest(screenInfo, &engineThreadControl);
        EXPECT_TRUE(screenTest.isValid());
        EXPECT_TRUE(screenTest.stop());
    }
}
