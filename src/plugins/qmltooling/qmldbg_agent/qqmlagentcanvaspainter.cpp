// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentcanvaspainter_p.h"

#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qvariant.h>

#ifdef QMLAGENT_HAS_CANVASPAINTER
#include <QtCanvasPainter/qcanvasboxgradient.h>
#include <QtCanvasPainter/qcanvasboxshadow.h>
#include <QtCanvasPainter/qcanvasbrush.h>
#include <QtCanvasPainter/qcanvasconicalgradient.h>
#include <QtCanvasPainter/qcanvascustombrush.h>
#include <QtCanvasPainter/qcanvasgradient.h>
#include <QtCanvasPainter/qcanvasgridpattern.h>
#include <QtCanvasPainter/qcanvasimage.h>
#include <QtCanvasPainter/qcanvasimagepattern.h>
#include <QtCanvasPainter/qcanvaslineargradient.h>
#include <QtCanvasPainter/qcanvasoffscreencanvas.h>
#include <QtCanvasPainter/qcanvaspath.h>
#include <QtCanvasPainter/qcanvasradialgradient.h>
#endif

QT_BEGIN_NAMESPACE

namespace {

#ifdef QMLAGENT_HAS_CANVASPAINTER
QJsonObject canvasValue(const QString &kind)
{
    return {
        { QStringLiteral("kind"), kind },
        { QStringLiteral("evidenceRole"), QStringLiteral("application-exposed-state") },
    };
}

QString brushTypeName(QCanvasBrush::BrushType type)
{
    switch (type) {
    case QCanvasBrush::BrushType::Invalid:
        return QStringLiteral("invalid");
    case QCanvasBrush::BrushType::LinearGradient:
        return QStringLiteral("linearGradient");
    case QCanvasBrush::BrushType::RadialGradient:
        return QStringLiteral("radialGradient");
    case QCanvasBrush::BrushType::ConicalGradient:
        return QStringLiteral("conicalGradient");
    case QCanvasBrush::BrushType::BoxGradient:
        return QStringLiteral("boxGradient");
    case QCanvasBrush::BrushType::BoxShadow:
        return QStringLiteral("boxShadow");
    case QCanvasBrush::BrushType::ImagePattern:
        return QStringLiteral("imagePattern");
    case QCanvasBrush::BrushType::GridPattern:
        return QStringLiteral("gridPattern");
    case QCanvasBrush::BrushType::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("unknown");
}

QJsonValue colorValue(const QColor &color)
{
    if (!color.isValid())
        return QJsonValue();
    return color.alpha() == 255 ? color.name(QColor::HexRgb) : color.name(QColor::HexArgb);
}

QJsonObject pointValue(const QPointF &point)
{
    return {
        { QStringLiteral("x"), point.x() },
        { QStringLiteral("y"), point.y() },
    };
}

QJsonObject sizeValue(const QSizeF &size)
{
    return {
        { QStringLiteral("width"), size.width() },
        { QStringLiteral("height"), size.height() },
    };
}

QJsonArray rectValue(const QRectF &rect)
{
    return { rect.x(), rect.y(), rect.width(), rect.height() };
}

QJsonObject imageValue(const QCanvasImage &image)
{
    QJsonObject result = canvasValue(QStringLiteral("QCanvasImage"));
    result.insert(QStringLiteral("id"), image.id());
    result.insert(QStringLiteral("width"), image.width());
    result.insert(QStringLiteral("height"), image.height());
    result.insert(QStringLiteral("sizeInBytes"), double(image.sizeInBytes()));
    result.insert(QStringLiteral("isNull"), image.isNull());
    result.insert(QStringLiteral("tintColor"), colorValue(image.tintColor()));
    result.insert(QStringLiteral("limitations"),
                  QJsonArray{
                          QStringLiteral("image id is painter-local and not stable across runs"),
                          QStringLiteral("source filename is not retained by QCanvasPainter"),
                  });
    return result;
}

QJsonArray gradientStops(const QCanvasGradient &gradient)
{
    QJsonArray stops;
    for (const QCanvasGradientStop &stop : gradient.stops()) {
        stops.append(QJsonObject{
                { QStringLiteral("position"), stop.position },
                { QStringLiteral("color"), colorValue(stop.color) },
        });
    }
    return stops;
}

QJsonObject gradientValue(const QCanvasGradient &gradient, const QString &kind)
{
    QJsonObject result = canvasValue(kind);
    result.insert(QStringLiteral("brushType"), brushTypeName(gradient.type()));
    result.insert(QStringLiteral("startColor"), colorValue(gradient.startColor()));
    result.insert(QStringLiteral("endColor"), colorValue(gradient.endColor()));
    result.insert(QStringLiteral("stops"), gradientStops(gradient));
    return result;
}
#endif

} // namespace

namespace QQmlAgentCanvasPainter {

QJsonValue valueFromVariant(const QVariant &value)
{
#ifdef QMLAGENT_HAS_CANVASPAINTER
    const QMetaType type = value.metaType();

    if (type == QMetaType::fromType<QCanvasLinearGradient>()) {
        const QCanvasLinearGradient gradient = value.value<QCanvasLinearGradient>();
        QJsonObject result = gradientValue(gradient, QStringLiteral("QCanvasLinearGradient"));
        result.insert(QStringLiteral("startPosition"), pointValue(gradient.startPosition()));
        result.insert(QStringLiteral("endPosition"), pointValue(gradient.endPosition()));
        return result;
    }
    if (type == QMetaType::fromType<QCanvasRadialGradient>()) {
        const QCanvasRadialGradient gradient = value.value<QCanvasRadialGradient>();
        QJsonObject result = gradientValue(gradient, QStringLiteral("QCanvasRadialGradient"));
        result.insert(QStringLiteral("centerPosition"), pointValue(gradient.centerPosition()));
        result.insert(QStringLiteral("outerRadius"), gradient.outerRadius());
        result.insert(QStringLiteral("innerRadius"), gradient.innerRadius());
        return result;
    }
    if (type == QMetaType::fromType<QCanvasConicalGradient>()) {
        const QCanvasConicalGradient gradient = value.value<QCanvasConicalGradient>();
        QJsonObject result = gradientValue(gradient, QStringLiteral("QCanvasConicalGradient"));
        result.insert(QStringLiteral("centerPosition"), pointValue(gradient.centerPosition()));
        result.insert(QStringLiteral("angle"), gradient.angle());
        return result;
    }
    if (type == QMetaType::fromType<QCanvasBoxGradient>()) {
        const QCanvasBoxGradient gradient = value.value<QCanvasBoxGradient>();
        QJsonObject result = gradientValue(gradient, QStringLiteral("QCanvasBoxGradient"));
        result.insert(QStringLiteral("rect"), rectValue(gradient.rect()));
        result.insert(QStringLiteral("feather"), gradient.feather());
        result.insert(QStringLiteral("radius"), gradient.radius());
        return result;
    }
    if (type == QMetaType::fromType<QCanvasGradient>()) {
        const auto &gradient = *static_cast<const QCanvasGradient *>(value.constData());
        QJsonObject result = gradientValue(gradient, QStringLiteral("QCanvasGradient"));
        result.insert(QStringLiteral("limitations"),
                      QJsonArray{
                              QStringLiteral("concrete gradient geometry is unavailable after "
                                             "type erasure to QCanvasGradient"),
                      });
        return result;
    }
    if (type == QMetaType::fromType<QCanvasBoxShadow>()) {
        const QCanvasBoxShadow shadow = value.value<QCanvasBoxShadow>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasBoxShadow"));
        result.insert(QStringLiteral("brushType"), brushTypeName(shadow.type()));
        result.insert(QStringLiteral("rect"), rectValue(shadow.rect()));
        result.insert(QStringLiteral("boundingRect"), rectValue(shadow.boundingRect()));
        result.insert(QStringLiteral("radius"), shadow.radius());
        result.insert(QStringLiteral("blur"), shadow.blur());
        result.insert(QStringLiteral("spread"), shadow.spread());
        result.insert(QStringLiteral("color"), colorValue(shadow.color()));
        result.insert(QStringLiteral("topLeftRadius"), shadow.topLeftRadius());
        result.insert(QStringLiteral("topRightRadius"), shadow.topRightRadius());
        result.insert(QStringLiteral("bottomLeftRadius"), shadow.bottomLeftRadius());
        result.insert(QStringLiteral("bottomRightRadius"), shadow.bottomRightRadius());
        return result;
    }
    if (type == QMetaType::fromType<QCanvasImage>())
        return imageValue(value.value<QCanvasImage>());
    if (type == QMetaType::fromType<QCanvasImagePattern>()) {
        const QCanvasImagePattern pattern = value.value<QCanvasImagePattern>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasImagePattern"));
        result.insert(QStringLiteral("brushType"), brushTypeName(pattern.type()));
        result.insert(QStringLiteral("startPosition"), pointValue(pattern.startPosition()));
        result.insert(QStringLiteral("imageSize"), sizeValue(pattern.imageSize()));
        result.insert(QStringLiteral("image"), imageValue(pattern.image()));
        result.insert(QStringLiteral("rotation"), pattern.rotation());
        result.insert(QStringLiteral("tintColor"), colorValue(pattern.tintColor()));
        return result;
    }
    if (type == QMetaType::fromType<QCanvasGridPattern>()) {
        const QCanvasGridPattern pattern = value.value<QCanvasGridPattern>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasGridPattern"));
        result.insert(QStringLiteral("brushType"), brushTypeName(pattern.type()));
        result.insert(QStringLiteral("startPosition"), pointValue(pattern.startPosition()));
        result.insert(QStringLiteral("cellSize"), sizeValue(pattern.cellSize()));
        result.insert(QStringLiteral("lineWidth"), pattern.lineWidth());
        result.insert(QStringLiteral("feather"), pattern.feather());
        result.insert(QStringLiteral("rotation"), pattern.rotation());
        result.insert(QStringLiteral("lineColor"), colorValue(pattern.lineColor()));
        result.insert(QStringLiteral("backgroundColor"), colorValue(pattern.backgroundColor()));
        return result;
    }
    if (type == QMetaType::fromType<QCanvasCustomBrush>()) {
        const QCanvasCustomBrush brush = value.value<QCanvasCustomBrush>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasCustomBrush"));
        result.insert(QStringLiteral("brushType"), brushTypeName(brush.type()));
        result.insert(QStringLiteral("timeRunning"), brush.timeRunning());
        result.insert(QStringLiteral("limitations"),
                      QJsonArray{
                              QStringLiteral("shader source and custom data are not "
                                             "readable through the public API"),
                      });
        return result;
    }
    if (type == QMetaType::fromType<QCanvasOffscreenCanvas>()) {
        const QCanvasOffscreenCanvas canvas = value.value<QCanvasOffscreenCanvas>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasOffscreenCanvas"));
        result.insert(QStringLiteral("isNull"), canvas.isNull());
        result.insert(QStringLiteral("flags"), int(canvas.flags().toInt()));
        result.insert(QStringLiteral("fillColor"), colorValue(canvas.fillColor()));
        result.insert(
                QStringLiteral("limitations"),
                QJsonArray{
                        QStringLiteral("render-backend texture handles are intentionally omitted"),
                });
        return result;
    }
    if (type == QMetaType::fromType<QCanvasPath>()) {
        const QCanvasPath path = value.value<QCanvasPath>();
        constexpr qsizetype maximumEndpointSamples = 32;
        const qsizetype commandCount = path.commandsSize();
        const qsizetype sampleCount = qMin(commandCount, maximumEndpointSamples);
        QJsonArray endpoints;
        for (qsizetype index = 0; index < sampleCount; ++index) {
            endpoints.append(QJsonObject{
                    { QStringLiteral("commandIndex"), int(index) },
                    { QStringLiteral("position"), pointValue(path.positionAt(index)) },
            });
        }

        QJsonObject result = canvasValue(QStringLiteral("QCanvasPath"));
        result.insert(QStringLiteral("isEmpty"), path.isEmpty());
        result.insert(QStringLiteral("commandCount"), double(commandCount));
        result.insert(QStringLiteral("commandDataCount"), double(path.commandsDataSize()));
        if (!path.isEmpty())
            result.insert(QStringLiteral("currentPosition"), pointValue(path.currentPosition()));
        result.insert(QStringLiteral("endpointSamples"), endpoints);
        result.insert(QStringLiteral("sampledCommandCount"), double(sampleCount));
        result.insert(QStringLiteral("omittedCommandCount"), double(commandCount - sampleCount));
        result.insert(QStringLiteral("truncated"), sampleCount < commandCount);
        result.insert(
                QStringLiteral("limitations"),
                QJsonArray{
                        QStringLiteral("public API exposes command endpoints but not "
                                       "command types or Bezier control points"),
                        QStringLiteral("endpoint samples are not a complete path reconstruction"),
                });
        return result;
    }
    if (type == QMetaType::fromType<QCanvasBrush>()) {
        const QCanvasBrush brush = value.value<QCanvasBrush>();
        QJsonObject result = canvasValue(QStringLiteral("QCanvasBrush"));
        result.insert(QStringLiteral("brushType"), brushTypeName(brush.type()));
        result.insert(QStringLiteral("limitations"),
                      QJsonArray{
                              QStringLiteral("base QCanvasBrush exposes its brush type only"),
                      });
        return result;
    }
#else
    Q_UNUSED(value);
#endif
    return QJsonValue(QJsonValue::Undefined);
}

} // namespace QQmlAgentCanvasPainter

QT_END_NAMESPACE
