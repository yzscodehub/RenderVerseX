/**
 * @file UITextOverflow.h
 * @brief Single-line UI text overflow helpers
 */

#pragma once

#include "UI/UITypes.h"

#include <string>

namespace RVX::UI
{

class UIRenderer;

/**
 * @brief Resolve the text that should be submitted for a constrained line.
 *
 * The returned text is for rendering only. Callers should keep their logical
 * text unchanged so selection, tooltips, serialization, and tests can still
 * observe the full value.
 */
std::string ResolveSingleLineTextOverflow(UIRenderer& renderer,
                                          const std::string& text,
                                          float width,
                                          float fontSize,
                                          TextOverflowMode mode);

} // namespace RVX::UI
