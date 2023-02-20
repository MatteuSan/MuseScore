/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "bufferedpaintprovider.h"

#include "utils/drawlogger.h"
#include "log.h"

using namespace mu;
using namespace mu::draw;

BufferedPaintProvider::BufferedPaintProvider()
{
    m_drawObjectsLogger = new DrawObjectsLogger();
    clear();
}

BufferedPaintProvider::~BufferedPaintProvider()
{
    delete m_drawObjectsLogger;
}

void BufferedPaintProvider::beginTarget(const std::string& name)
{
    clear();
    m_buf->name = name;
    beginObject("target_" + name);
    m_isActive = true;
}

void BufferedPaintProvider::beforeEndTargetHook(Painter*)
{
}

bool BufferedPaintProvider::endTarget(bool endDraw)
{
    UNUSED(endDraw);
    if (m_isActive) {
        m_isActive = false;
        endObject();
    }
    return true;
}

bool BufferedPaintProvider::isActive() const
{
    return m_isActive;
}

void BufferedPaintProvider::beginObject(const std::string& name)
{
    // no begin any objects
    if (m_itemLevel < 0) {
        m_buf->item.name = name;
        m_buf->item.datas.emplace_back(); // default state
        m_itemLevel = 0;
        return;
    }

    // add new object
    DrawData::Item& parent = editableObject();
    DrawData::Item& ch = parent.chilren.emplace_back(name);
    ch.datas.emplace_back(); // default state

    ++m_itemLevel;

#ifdef MUE_ENABLE_DRAW_TRACE
    m_drawObjectsLogger->beginObject(name, pagePos);
#endif
}

void BufferedPaintProvider::endObject()
{
    TRACEFUNC;
    IF_ASSERT_FAILED(m_itemLevel > -1) {
        return;
    }

    DrawData::Item& obj = editableObject();

    // remove default state or state without data
    if (obj.datas.back().empty()) {
        obj.datas.pop_back();
    }

    --m_itemLevel;

#ifdef MUE_ENABLE_DRAW_TRACE
    m_drawObjectsLogger->endObject();
#endif
}

const DrawData::Item& BufferedPaintProvider::currentObject() const
{
    DrawData::Item* item = &m_buf->item;
    for (int i = 0; i < m_itemLevel; ++i) {
        item = &item->chilren.back();
    }

    if (item->datas.empty()) {
        item->datas.emplace_back(); // default state
    }

    return *item;
}

DrawData::Item& BufferedPaintProvider::editableObject()
{
    DrawData::Item* item = &m_buf->item;
    for (int i = 0; i < m_itemLevel; ++i) {
        item = &item->chilren.back();
    }

    if (item->datas.empty()) {
        item->datas.emplace_back(); // default state
    }

    return *item;
}

const DrawData::Data& BufferedPaintProvider::currentData() const
{
    return currentObject().datas.back();
}

const DrawData::State& BufferedPaintProvider::currentState() const
{
    return currentData().state;
}

DrawData::Data& BufferedPaintProvider::editableData()
{
    return editableObject().datas.back();
}

DrawData::State& BufferedPaintProvider::editableState()
{
    DrawData::Item& obj = editableObject();
    IF_ASSERT_FAILED(obj.datas.size() > 0) {
        obj.datas.emplace_back();
    }

    DrawData::Data& data = obj.datas.back();
    if (data.empty()) {
        return data.state;
    }

    DrawData::Data& newData = obj.datas.emplace_back();
    newData.state = data.state;

    return newData.state;
}

void BufferedPaintProvider::setAntialiasing(bool arg)
{
    editableState().isAntialiasing = arg;
}

void BufferedPaintProvider::setCompositionMode(CompositionMode mode)
{
    editableState().compositionMode = mode;
}

void BufferedPaintProvider::setWindow(const RectF&)
{
}

void BufferedPaintProvider::setViewport(const RectF& viewport)
{
    m_buf->viewport = viewport;
}

void BufferedPaintProvider::setFont(const Font& f)
{
    editableState().font = f;
}

const Font& BufferedPaintProvider::font() const
{
    return currentState().font;
}

void BufferedPaintProvider::setPen(const Pen& pen)
{
    editableState().pen = pen;
}

void BufferedPaintProvider::setNoPen()
{
    editableState().pen.setStyle(PenStyle::NoPen);
}

const Pen& BufferedPaintProvider::pen() const
{
    return currentState().pen;
}

void BufferedPaintProvider::setBrush(const Brush& brush)
{
    editableState().brush = brush;
}

const Brush& BufferedPaintProvider::brush() const
{
    return currentState().brush;
}

void BufferedPaintProvider::save()
{
}

void BufferedPaintProvider::restore()
{
}

void BufferedPaintProvider::setTransform(const Transform& transform)
{
    DrawData::State& st = editableState();
    st.transform = transform;
}

const Transform& BufferedPaintProvider::transform() const
{
    return currentState().transform;
}

// drawing functions

void BufferedPaintProvider::drawPath(const PainterPath& path)
{
    const DrawData::State& st = currentState();
    DrawMode mode = DrawMode::StrokeAndFill;
    if (st.pen.style() == PenStyle::NoPen) {
        mode = DrawMode::Fill;
    } else if (st.brush.style() == BrushStyle::NoBrush) {
        mode = DrawMode::Stroke;
    }
    editableData().paths.push_back({ path, st.pen, st.brush, mode });
}

void BufferedPaintProvider::drawPolygon(const PointF* points, size_t pointCount, PolygonMode mode)
{
    PolygonF pol(pointCount);
    for (size_t i = 0; i < pointCount; ++i) {
        pol[i] = PointF(points[i].x(), points[i].y());
    }
    editableData().polygons.push_back(DrawPolygon { pol, mode });
}

void BufferedPaintProvider::drawText(const PointF& point, const String& text)
{
    editableData().texts.push_back(DrawText { DrawText::Point, RectF(point, SizeF()), 0, text });
}

void BufferedPaintProvider::drawText(const RectF& rect, int flags, const String& text)
{
    editableData().texts.push_back(DrawText { DrawText::Rect, rect, flags, text });
}

void BufferedPaintProvider::drawTextWorkaround(const Font& f, const PointF& pos, const String& text)
{
    setFont(f);
    drawText(pos, text);
}

void BufferedPaintProvider::drawSymbol(const PointF& point, char32_t ucs4Code)
{
    drawText(point, String::fromUcs4(&ucs4Code, 1));
}

void BufferedPaintProvider::drawPixmap(const PointF& p, const Pixmap& pm)
{
    editableData().pixmaps.push_back(DrawPixmap { DrawPixmap::Single, RectF(p, SizeF()), pm, PointF() });
}

void BufferedPaintProvider::drawTiledPixmap(const RectF& rect, const Pixmap& pm, const PointF& offset)
{
    editableData().pixmaps.push_back(DrawPixmap { DrawPixmap::Tiled, rect, pm, offset });
}

#ifndef NO_QT_SUPPORT
void BufferedPaintProvider::drawPixmap(const PointF& p, const QPixmap& pm)
{
    editableData().pixmaps.push_back(DrawPixmap { DrawPixmap::Single, RectF(p, SizeF()), Pixmap::fromQPixmap(pm), PointF() });
}

void BufferedPaintProvider::drawTiledPixmap(const RectF& rect, const QPixmap& pm, const PointF& offset)
{
    editableData().pixmaps.push_back(DrawPixmap { DrawPixmap::Tiled, rect, Pixmap::fromQPixmap(pm), offset });
}

#endif

void BufferedPaintProvider::setClipRect(const RectF& rect)
{
    UNUSED(rect);
}

void BufferedPaintProvider::setClipping(bool enable)
{
    UNUSED(enable);
}

DrawDataPtr BufferedPaintProvider::drawData() const
{
    return m_buf;
}

void BufferedPaintProvider::clear()
{
    m_buf = std::make_shared<DrawData>();
    m_pageNo = 0;
    m_itemLevel = -1;
}
