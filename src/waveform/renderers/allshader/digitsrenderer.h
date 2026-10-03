#pragma once

#include <string_view>

#include "rendergraph/context.h"
#include "rendergraph/geometrynode.h"
#include "util/class.h"

namespace rendergraph {
class TexturedVertexUpdater;
} // namespace rendergraph

namespace allshader {
class DigitsRenderNode;
} // namespace allshader

class allshader::DigitsRenderNode : public rendergraph::GeometryNode {
  public:
    DigitsRenderNode();
    ~DigitsRenderNode();

    void updateTexture(rendergraph::Context* pContext,
            float fontPointSize,
            float maxHeight,
            float devicePixelRatio);

    void update(
            float x,
            float y,
            bool multiLine,
            const QString& s1,
            const QString& s2);

    void clear();

    float height() const;

  private:
    static constexpr std::string_view kCharacters{"0123456789:.,"};
    static constexpr std::size_t kCharacterCount = kCharacters.size();

    static constexpr char indexToChar(std::size_t index) {
        return kCharacters[index];
    }
    static constexpr std::size_t charToIndex(char ch) {
        if (ch >= '0' && ch <= '9') {
            return static_cast<std::size_t>(ch - '0');
        }
        if (ch == ':') {
            return 10;
        }
        if (ch == '.') {
            return 11;
        }
        if (ch == ',') {
            return 12;
        }
        return 11; // Unsupported input falls back to the existing dot glyph.
    }
    static constexpr bool checkCharacterMapping() {
        for (std::size_t i = 0; i < kCharacterCount; ++i) {
            if (charToIndex(indexToChar(i)) != i) {
                return false;
            }
        }
        return true;
    }

    float addVertices(rendergraph::TexturedVertexUpdater& vertexUpdater,
            float x,
            float y,
            const QString& s);

    int m_penWidth;
    float m_offset[kCharacterCount + 1];
    float m_width[kCharacterCount];
    float m_fontPointSize{};
    float m_height{};
    float m_maxHeight{};
    float m_devicePixelRatio{};
    DISALLOW_COPY_AND_ASSIGN(DigitsRenderNode);
};
