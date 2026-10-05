#pragma once

namespace mixxx {

class AudioCallbackScope final {
  public:
    AudioCallbackScope() noexcept {
        ++s_depth;
    }
    ~AudioCallbackScope() {
        --s_depth;
    }
    AudioCallbackScope(const AudioCallbackScope&) = delete;
    AudioCallbackScope& operator=(const AudioCallbackScope&) = delete;

    static bool isActive() noexcept {
        return s_depth != 0;
    }

  private:
    inline static thread_local unsigned int s_depth = 0;
};

}
