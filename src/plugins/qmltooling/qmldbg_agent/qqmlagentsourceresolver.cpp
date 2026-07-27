// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentsourceresolver_p.h"

#include "qqmlagentjsonutils_p.h"

#include <QtCore/qfile.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qlogging.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qobject.h>
#include <QtCore/qproperty.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qset.h>
#include <QtCore/qurl.h>
#include <QtCore/qvariant.h>
#include <QtQml/qqmlerror.h>
#include <QtQml/qqmlproperty.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/private/qquickloader_p.h>

#include <private/qqmlabstractbinding_p.h>
#include <private/qqmlbinding_p.h>
#include <private/qqmlcontextdata_p.h>
#include <private/qqmldata_p.h>
#include <private/qqmlproperty_p.h>
#include <private/qproperty_p.h>

QT_BEGIN_NAMESPACE

static QJsonObject makeLocation(const QString &file, int line, int column, const QString &method,
                                double confidence, const QJsonArray &limitations = {})
{
    QJsonObject location{
        { QStringLiteral("file"), file },
        { QStringLiteral("method"), method },
        { QStringLiteral("confidence"), confidence },
        { QStringLiteral("limitations"), limitations },
    };
    if (line > 0)
        location.insert(QStringLiteral("line"), line);
    if (column > 0)
        location.insert(QStringLiteral("column"), column);
    return location;
}

static QString fileFromData(const QQmlData *data)
{
    if (!data)
        return {};

    if (data->outerContext && data->outerContext->isValid())
        return data->outerContext->urlString();
    if (data->context && data->context->isValid())
        return data->context->urlString();
    return {};
}

static QString readableSourcePath(const QString &file)
{
    const QUrl url(file);
    if (url.isLocalFile())
        return url.toLocalFile();
    if (url.scheme() == QLatin1String("qrc"))
        return QLatin1Char(':') + url.path();
    if (url.scheme().isEmpty())
        return file;
    return {};
}

struct PropertySourceSnippet
{
    QString expression;
    int line = -1;
    int column = -1;
    QString method;
    double confidence = 0.0;
    QJsonArray limitations;
};

static QString stripLineComment(QString line)
{
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escaped = false;
    for (qsizetype i = 0; i + 1 < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (!inDoubleQuote && ch == QLatin1Char('\'')) {
            inSingleQuote = !inSingleQuote;
            continue;
        }
        if (!inSingleQuote && ch == QLatin1Char('"')) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }
        if (!inSingleQuote && !inDoubleQuote && ch == QLatin1Char('/')
                && line.at(i + 1) == QLatin1Char('/')) {
            return line.left(i);
        }
    }
    return line;
}

static QString rhsOnLine(const QString &line, const QString &propertyName, int *column)
{
    const QRegularExpression expression(QStringLiteral("(^|[^A-Za-z0-9_\\.])%1\\s*:")
                                                .arg(QRegularExpression::escape(propertyName)));
    const QRegularExpressionMatch match = expression.match(line);
    if (!match.hasMatch())
        return {};

    const int colon = line.indexOf(QLatin1Char(':'), match.capturedEnd(0) - 1);
    if (colon < 0)
        return {};
    if (column)
        *column = colon + 2;

    QString rhs = stripLineComment(line.mid(colon + 1)).trimmed();
    if (rhs.endsWith(QLatin1Char(',')) || rhs.endsWith(QLatin1Char(';')))
        rhs.chop(1);
    return rhs.trimmed();
}

static bool expressionLikelyContinues(const QString &expression)
{
    const QString trimmed = expression.trimmed();
    if (trimmed.isEmpty())
        return true;

    int parenDepth = 0;
    int bracketDepth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escaped = false;
    for (const QChar ch : trimmed) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (!inDoubleQuote && ch == QLatin1Char('\'')) {
            inSingleQuote = !inSingleQuote;
            continue;
        }
        if (!inSingleQuote && ch == QLatin1Char('"')) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }
        if (inSingleQuote || inDoubleQuote)
            continue;
        if (ch == QLatin1Char('('))
            ++parenDepth;
        else if (ch == QLatin1Char(')'))
            --parenDepth;
        else if (ch == QLatin1Char('['))
            ++bracketDepth;
        else if (ch == QLatin1Char(']'))
            --bracketDepth;
    }

    if (parenDepth > 0 || bracketDepth > 0)
        return true;

    static const QRegularExpression trailingOperator(
            QStringLiteral("(\\?|:|\\+|-|\\*|/|%|&&|\\|\\||===|==|!==|!=|>=|<=|>|<|\\.|,)\\s*$"));
    return trailingOperator.match(trimmed).hasMatch();
}

static bool expressionLineStartsContinuation(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return false;
    static const QStringList operators{
        QStringLiteral("+"),
        QStringLiteral("-"),
        QStringLiteral("*"),
        QStringLiteral("/"),
        QStringLiteral("%"),
        QStringLiteral("&&"),
        QStringLiteral("||"),
        QStringLiteral("==="),
        QStringLiteral("=="),
        QStringLiteral("!=="),
        QStringLiteral("!="),
        QStringLiteral(">="),
        QStringLiteral("<="),
        QStringLiteral(">"),
        QStringLiteral("<"),
        QStringLiteral("?"),
        QStringLiteral(":"),
        QStringLiteral("."),
        QStringLiteral(","),
    };
    for (const QString &op : operators) {
        if (trimmed.startsWith(op))
            return true;
    }
    return false;
}

static QString rhsFromLines(const QStringList &lines, int lineIndex, const QString &propertyName,
                            int *column)
{
    QString rhs = rhsOnLine(lines.at(lineIndex), propertyName, column);
    if (rhs.isEmpty() && propertyName.contains(QLatin1Char('.')))
        rhs = rhsOnLine(lines.at(lineIndex), propertyName.section(QLatin1Char('.'), -1), column);
    if (rhs.isEmpty())
        return {};

    QStringList expressionLines{ rhs };
    for (int i = lineIndex + 1; i < lines.size() && i <= lineIndex + 8; ++i) {
        QString next = stripLineComment(lines.at(i)).trimmed();
        if (next.isEmpty())
            continue;
        if (!expressionLikelyContinues(expressionLines.join(QLatin1Char(' ')))
                && !expressionLineStartsContinuation(next)) {
            break;
        }

        if (next.endsWith(QLatin1Char(',')) || next.endsWith(QLatin1Char(';')))
            next.chop(1);
        expressionLines.append(next.trimmed());
    }

    return expressionLines.join(QLatin1Char(' ')).trimmed();
}

static QJsonArray candidateIdentifiersFromExpression(const QString &expression)
{
    QJsonArray identifiers;
    if (expression.isEmpty())
        return identifiers;

    static const QSet<QString> reserved{
        QStringLiteral("as"),
        QStringLiteral("break"),
        QStringLiteral("case"),
        QStringLiteral("catch"),
        QStringLiteral("const"),
        QStringLiteral("continue"),
        QStringLiteral("default"),
        QStringLiteral("else"),
        QStringLiteral("false"),
        QStringLiteral("for"),
        QStringLiteral("function"),
        QStringLiteral("if"),
        QStringLiteral("import"),
        QStringLiteral("in"),
        QStringLiteral("let"),
        QStringLiteral("new"),
        QStringLiteral("null"),
        QStringLiteral("return"),
        QStringLiteral("switch"),
        QStringLiteral("this"),
        QStringLiteral("true"),
        QStringLiteral("undefined"),
        QStringLiteral("var"),
        QStringLiteral("while"),
    };

    QSet<QString> seen;
    const QRegularExpression identifierPattern(
            QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)*\\b"));
    QRegularExpressionMatchIterator matches = identifierPattern.globalMatch(expression);
    while (matches.hasNext() && identifiers.size() < 12) {
        const QRegularExpressionMatch match = matches.next();
        const QString name = match.captured(0);
        const QString baseName = name.section(QLatin1Char('.'), 0, 0);
        if (reserved.contains(name) || reserved.contains(baseName) || seen.contains(name))
            continue;
        seen.insert(name);

        QJsonObject candidate{
            { QStringLiteral("name"), name },
            { QStringLiteral("confidence"), 0.35 },
            { QStringLiteral("method"), QStringLiteral("source-token-hint") },
            { QStringLiteral("limitations"), QJsonArray{
                QStringLiteral("identifier was tokenized from source text; it is a follow-up hint, not runtime dependency proof"),
            } },
        };
        if (name.contains(QLatin1Char('.'))) {
            candidate.insert(QStringLiteral("nextHints"), QJsonArray{ QJsonObject{
                { QStringLiteral("method"), QStringLiteral("UI.query") },
                { QStringLiteral("params"), QJsonObject{
                    { QStringLiteral("selector"),
                      QStringLiteral("id=\"%1\"").arg(baseName) },
                    { QStringLiteral("properties"),
                      QJsonArray{ name.section(QLatin1Char('.'), 1) } },
                } },
                { QStringLiteral("reason"),
                  QStringLiteral("Resolve this source-token hint structurally if the left side is a QML id.") },
            } });
        }
        identifiers.append(candidate);
    }
    return identifiers;
}

static void insertCandidateIdentifiers(QJsonObject *provenance, const QString &expression)
{
    const QJsonArray identifiers = candidateIdentifiersFromExpression(expression);
    if (!identifiers.isEmpty())
        provenance->insert(QStringLiteral("candidateIdentifiers"), identifiers);
}

static PropertySourceSnippet propertySourceSnippet(const QJsonObject &location,
                                                   const QString &propertyName,
                                                   int objectLine)
{
    PropertySourceSnippet snippet;
    const QString file = location.value(QStringLiteral("file")).toString();
    const QString path = readableSourcePath(file);
    if (path.isEmpty()) {
        snippet.limitations.append(QStringLiteral("source file is not locally readable"));
        return snippet;
    }

    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        snippet.limitations.append(QStringLiteral("source file could not be opened"));
        return snippet;
    }

    const QStringList lines = QString::fromUtf8(sourceFile.readAll()).split(QLatin1Char('\n'));
    const int exactLine = location.value(QStringLiteral("line")).toInt(-1);
    if (exactLine > 0 && exactLine <= lines.size()) {
        int column = -1;
        const QString rhs = rhsFromLines(lines, exactLine - 1, propertyName, &column);
        if (!rhs.isEmpty()) {
            snippet.expression = rhs;
            snippet.line = exactLine;
            snippet.column = column;
            snippet.method = QStringLiteral("binding-source-line");
            snippet.confidence = 0.85;
            snippet.limitations.append(QStringLiteral("snippet is source text near runtime binding location, not a parsed AST"));
            return snippet;
        }
    }

    const int startLine = objectLine > 0 ? objectLine : exactLine;
    if (startLine <= 0 || startLine > lines.size()) {
        snippet.limitations.append(QStringLiteral("object source line unavailable for bounded source scan"));
        return snippet;
    }

    const int maxLine = std::min<int>(int(lines.size()), startLine + 160);
    int braceDepth = 0;
    bool enteredObject = false;
    for (int i = startLine - 1; i < maxLine; ++i) {
        const QString line = lines.at(i);
        for (const QChar ch : line) {
            if (ch == QLatin1Char('{')) {
                ++braceDepth;
                enteredObject = true;
            } else if (ch == QLatin1Char('}')) {
                --braceDepth;
                if (enteredObject && braceDepth <= 0 && i > startLine - 1)
                    return snippet;
            }
        }

        int column = -1;
        const QString rhs = enteredObject && braceDepth == 1
                ? rhsFromLines(lines, i, propertyName, &column) : QString();
        if (!rhs.isEmpty()) {
            snippet.expression = rhs;
            snippet.line = i + 1;
            snippet.column = column;
            snippet.method = QStringLiteral("object-source-scan");
            snippet.confidence = 0.55;
            snippet.limitations.append(QStringLiteral("fallback source scan is bounded by object line and braces; it is not a parsed QML AST"));
            snippet.limitations.append(QStringLiteral("fallback source scan only considers direct assignments on the target object's body"));
            return snippet;
        }
    }

    return snippet;
}

static QString bindingKindName(QQmlAbstractBinding::Kind kind)
{
    switch (kind) {
    case QQmlAbstractBinding::ValueTypeProxy:
        return QStringLiteral("valueTypeProxy");
    case QQmlAbstractBinding::QmlBinding:
        return QStringLiteral("qmlBinding");
    case QQmlAbstractBinding::PropertyToPropertyBinding:
        return QStringLiteral("propertyToPropertyBinding");
    }
    return QStringLiteral("unknown");
}

static QJsonObject sourceLocationFromBinding(const QQmlAbstractBinding *binding)
{
    const QQmlBinding *qmlBinding = dynamic_cast<const QQmlBinding *>(binding);
    if (!qmlBinding)
        return QQmlAgentSourceResolver::unknownLocation(QStringLiteral("binding-metadata"));

    const QQmlSourceLocation location = qmlBinding->sourceLocation();
    if (!location.sourceFile.isEmpty() && location.line > 0) {
        return makeLocation(location.sourceFile, location.line, location.column,
                            QStringLiteral("qqmlbinding-source-location"), 0.95);
    }
    if (!location.sourceFile.isEmpty()) {
        return makeLocation(location.sourceFile, -1, -1,
                            QStringLiteral("qqmlbinding-source-location"), 0.70,
                            { QStringLiteral("binding source has URL but no exact line") });
    }
    return QQmlAgentSourceResolver::unknownLocation(QStringLiteral("binding-metadata"));
}

static QPropertyBindingPrivate *bindablePropertyBinding(QObject *object, const QString &propertyName)
{
    const QMetaObject *metaObject = object ? object->metaObject() : nullptr;
    if (!metaObject)
        return nullptr;

    const int propertyIndex = metaObject->indexOfProperty(propertyName.toUtf8().constData());
    if (propertyIndex < 0)
        return nullptr;

    const QMetaProperty metaProperty = metaObject->property(propertyIndex);
    if (!metaProperty.isBindable())
        return nullptr;

    const QUntypedBindable bindable = metaProperty.bindable(object);
    if (!bindable.isBindable())
        return nullptr;

    const QUntypedPropertyBinding binding = bindable.binding();
    return QPropertyBindingPrivate::get(binding);
}

static QJsonObject sourceLocationFromBindableBinding(QPropertyBindingPrivate *binding)
{
    if (!binding)
        return QQmlAgentSourceResolver::unknownLocation(QStringLiteral("qproperty-binding"));

    const QPropertyBindingSourceLocation cppLocation = binding->sourceLocation();
    const QString cppFile = QString::fromUtf8(cppLocation.fileName ? cppLocation.fileName : "");
    if (!cppFile.isEmpty() && cppFile != QLatin1String("Custom location")) {
        return makeLocation(cppFile, int(cppLocation.line), int(cppLocation.column),
                            QStringLiteral("qproperty-binding-source-location"), 0.75,
                            { QStringLiteral("source location is from QPropertyBinding, not QQmlBinding") });
    }

    QJsonObject unknown = QQmlAgentSourceResolver::unknownLocation(QStringLiteral("qproperty-binding"));
    unknown.insert(QStringLiteral("limitations"), QJsonArray{
        QStringLiteral("QPropertyBindingPrivate does not safely expose QQmlPropertyBinding source metadata from the plugin boundary"),
    });
    return unknown;
}

static QJsonObject bindableDependencySummary(QPropertyBindingPrivate *binding)
{
    if (!binding)
        return {};

    QJsonObject summary{
        { QStringLiteral("qPropertyObserverCount"), int(binding->dependencyObserverCount) },
        { QStringLiteral("method"), QStringLiteral("qproperty-binding-private") },
        { QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("QProperty dependency observers do not expose source QObject/property/value pairs from the plugin boundary"),
        } },
    };

    if (binding->hasCustomVTable()) {
        summary.insert(QStringLiteral("hasCustomVTable"), true);
        summary.insert(QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("QProperty dependency observers do not expose source QObject/property/value pairs from the plugin boundary"),
            QStringLiteral("Custom QProperty binding vtables may be QML-owned, but QmlAgent does not cast them to QQmlPropertyBinding because that is not type-safe from the plugin boundary"),
        });
    }

    return summary;
}

static QString quotedSelectorValue(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

static QString dependencySelector(const QString &qmlId, const QString &objectName)
{
    if (!qmlId.isEmpty())
        return QStringLiteral("id=%1").arg(quotedSelectorValue(qmlId));
    if (!objectName.isEmpty())
        return QStringLiteral("objectName=%1").arg(quotedSelectorValue(objectName));
    return {};
}

static QJsonArray dependencyEvidence(const QQmlBinding *binding)
{
    QJsonArray dependencies;
    if (!binding)
        return dependencies;

    const QList<QQmlProperty> bindingDependencies = binding->dependencies();
    constexpr qsizetype MaxDependencies = 12;
    for (qsizetype i = 0; i < bindingDependencies.size() && i < MaxDependencies; ++i) {
        const QQmlProperty dependency = bindingDependencies.at(i);
        QObject *object = dependency.object();
        QJsonObject entry{
            { QStringLiteral("property"), dependency.name() },
            { QStringLiteral("value"), QQmlAgentJsonUtils::valueFromVariant(dependency.read()) },
        };
        if (object) {
            const QQmlData *data = QQmlData::get(object);
            const QString id = data && data->outerContext
                    ? data->outerContext->findObjectId(object)
                    : QString();
            if (!id.isEmpty())
                entry.insert(QStringLiteral("qmlId"), id);
            if (!object->objectName().isEmpty())
                entry.insert(QStringLiteral("objectName"), object->objectName());
            entry.insert(QStringLiteral("type"), QString::fromUtf8(object->metaObject()->className()));
            entry.insert(QStringLiteral("sourceLocation"),
                         QQmlAgentSourceResolver::sourceLocationForObject(object));
            const QString selector = dependencySelector(id, object->objectName());
            if (!selector.isEmpty()) {
                entry.insert(QStringLiteral("selector"), selector);
                entry.insert(QStringLiteral("nextHints"), QJsonArray{ QJsonObject{
                    { QStringLiteral("method"), QStringLiteral("UI.query") },
                    { QStringLiteral("params"), QJsonObject{
                        { QStringLiteral("selector"), selector },
                        { QStringLiteral("properties"), QJsonArray{ dependency.name() } },
                    } },
                    { QStringLiteral("reason"),
                      QStringLiteral("Inspect the runtime dependency value that feeds this binding.") },
                } });
            }
        }
        dependencies.append(entry);
    }

    if (bindingDependencies.size() > MaxDependencies) {
        dependencies.append(QJsonObject{
            { QStringLiteral("truncated"), true },
            { QStringLiteral("omittedDependencyCount"), int(bindingDependencies.size() - MaxDependencies) },
        });
    }
    return dependencies;
}

static QRegularExpression objectNameBindingExpression(const QString &objectName)
{
    const QString quotedName = QRegularExpression::escape(objectName);
    return QRegularExpression(QStringLiteral("\\bobjectName\\s*:\\s*([\"'])%1\\1").arg(quotedName));
}

static int uniqueObjectNameBindingLine(const QString &source, const QString &objectName)
{
    const QRegularExpression expression = objectNameBindingExpression(objectName);
    QRegularExpressionMatchIterator matches = expression.globalMatch(source);
    int matchStart = -1;
    int count = 0;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        matchStart = match.capturedStart();
        ++count;
        if (count > 1)
            return -1;
    }

    if (count != 1 || matchStart < 0)
        return -1;

    int line = 1;
    for (int i = 0; i < matchStart; ++i) {
        if (source.at(i) == QLatin1Char('\n'))
            ++line;
    }
    return line;
}

static bool hasLoaderAncestor(const QObject *object)
{
    for (const QObject *parent = object ? object->parent() : nullptr; parent; parent = parent->parent()) {
        if (qobject_cast<const QQuickLoader *>(parent))
            return true;
    }

    const QQuickItem *item = qobject_cast<const QQuickItem *>(object);
    for (const QQuickItem *parent = item ? item->parentItem() : nullptr; parent;
         parent = parent->parentItem()) {
        if (qobject_cast<const QQuickLoader *>(parent))
            return true;
    }

    return false;
}

static bool hasDelegateContext(const QQmlContextData *context)
{
    for (const QQmlContextData *current = context; current;) {
        if (current->extraObject())
            return true;
        const QQmlRefPointer<QQmlContextData> parent = current->parent();
        current = parent.data();
    }
    return false;
}

static bool hasAuthorContextId(const QObject *object, const QQmlData *data)
{
    return data && data->outerContext && !data->outerContext->findObjectId(object).isEmpty();
}

static bool isDynamicallyCreatedComponentRoot(const QQmlData *data)
{
    return data && data->ownContext && !data->indestructible && !data->explicitIndestructibleSet;
}

static bool hasDynamicComponentRoot(const QObject *object)
{
    for (const QObject *current = object; current; current = current->parent()) {
        if (isDynamicallyCreatedComponentRoot(QQmlData::get(current)))
            return true;
    }

    const QQuickItem *item = qobject_cast<const QQuickItem *>(object);
    for (const QQuickItem *current = item; current; current = current->parentItem()) {
        if (isDynamicallyCreatedComponentRoot(QQmlData::get(current)))
            return true;
    }

    return false;
}

struct SourceMethod
{
    QString method;
    double confidence = 0.0;
    QJsonArray limitations;
};

static QJsonArray directSourceLimitations()
{
    return {
        QStringLiteral("confidence reflects QQmlData source metadata for the live object"),
        QStringLiteral("does not prove the object was not moved, reparented, or mutated after creation"),
    };
}

static SourceMethod classifySourceMethod(const QObject *object, const QQmlData *data)
{
    if (hasLoaderAncestor(object)) {
        return {
            QStringLiteral("qqmldata-loaded"),
            0.80,
            {
                QStringLiteral("location is in loaded component, not necessarily Loader caller"),
                QStringLiteral("does not prove Loader source or active item was not changed after creation"),
            },
        };
    }

    if (hasDelegateContext(data->outerContext) || hasDelegateContext(data->context)) {
        return {
            QStringLiteral("qqmldata-delegate"),
            0.75,
            {
                QStringLiteral("location is delegate template, not model row instance"),
                QStringLiteral("delegate instances may be recycled by views"),
            },
        };
    }

    if (hasAuthorContextId(object, data))
        return { QStringLiteral("qqmldata-direct"), 0.90, directSourceLimitations() };

    if (hasDynamicComponentRoot(object)) {
        return {
            QStringLiteral("qqmldata-dynamic"),
            0.65,
            { QStringLiteral("location is component template; dynamic creation call site is "
                             "unavailable from QQmlData") },
        };
    }

    return { QStringLiteral("qqmldata-direct"), 0.85, directSourceLimitations() };
}

QJsonObject QQmlAgentSourceResolver::unknownLocation(const QString &method)
{
    return makeLocation(QString(), -1, -1, method, 0.0, { QStringLiteral("no QML source metadata available") });
}

QJsonObject QQmlAgentSourceResolver::sourceLocationForObject(const QObject *object)
{
    if (!object)
        return unknownLocation();

    const QQmlData *data = QQmlData::get(object);
    if (!data)
        return makeLocation(QString(), -1, -1, QStringLiteral("cpp-object"), 0.0,
                            { QStringLiteral("object has no QQmlData") });

    const QString file = fileFromData(data);

    const SourceMethod sourceMethod = classifySourceMethod(object, data);
    if (!file.isEmpty() && data->lineNumber > 0)
        return makeLocation(file, data->lineNumber, data->columnNumber, sourceMethod.method,
                            sourceMethod.confidence, sourceMethod.limitations);

    if (!file.isEmpty()) {
        return makeLocation(file, -1, -1, QStringLiteral("component-url"), 0.40,
                            { QStringLiteral("line and column unavailable from QQmlData") });
    }

    return unknownLocation();
}

// The single ranked "edit here" answer (F-016): primary is the precise
// assignment/binding location when one exists with confidence; otherwise
// the object declaration, explicitly downgraded so callers can tell the
// difference without knowing the dossier's precedence rules.
static QJsonObject assignmentSite(const QJsonObject &primary,
                                  const QJsonObject &objectLocation,
                                  const QString &primaryMethod)
{
    const bool primaryUsable =
            primary.value(QStringLiteral("confidence")).toDouble() > 0.0
            && !primary.value(QStringLiteral("file")).toString().isEmpty();
    QJsonObject site{
        { QStringLiteral("sourceLocation"),
          primaryUsable ? primary : objectLocation },
        { QStringLiteral("method"),
          primaryUsable ? primaryMethod : QStringLiteral("objectDeclaration") },
    };
    if (!primaryUsable) {
        site.insert(QStringLiteral("note"),
                    QStringLiteral("no per-property location available; this is "
                                   "the object declaration, not the assignment"));
    }
    return site;
}

QJsonObject QQmlAgentSourceResolver::bindingProvenanceForProperty(QObject *object,
                                                                  const QString &propertyName)
{
    QJsonObject result{
        { QStringLiteral("ok"), false },
        { QStringLiteral("property"), propertyName },
    };

    if (!object) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("binding.target_not_found") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("message"), QStringLiteral("Binding target could not be resolved.") },
        } });
        return result;
    }

    if (propertyName.isEmpty()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("binding.property_required") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("message"), QStringLiteral("Property name is required.") },
        } });
        return result;
    }

    QQmlProperty property(object, propertyName);
    if (!property.isValid()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("binding.property_not_found") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("message"), QStringLiteral("Property does not exist on runtime target.") },
            { QStringLiteral("property"), propertyName },
        } });
        return result;
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("value"), QQmlAgentJsonUtils::propertyValue(object, propertyName));

    QQmlAbstractBinding *binding = QQmlPropertyPrivate::binding(property);
    QPropertyBindingPrivate *qpropertyBinding = binding ? nullptr
                                                        : bindablePropertyBinding(object, propertyName);
    const QJsonObject objectLocation = sourceLocationForObject(object);
    const int objectLine = objectLocation.value(QStringLiteral("line")).toInt(-1);

    QJsonObject provenance{
        { QStringLiteral("property"), propertyName },
        { QStringLiteral("objectSourceLocation"), objectLocation },
    };

    if (!binding && !qpropertyBinding) {
        provenance.insert(QStringLiteral("kind"), QStringLiteral("runtimeValue"));
        provenance.insert(QStringLiteral("isBinding"), false);
        provenance.insert(QStringLiteral("confidence"), 0.60);
        provenance.insert(QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("no active QQmlAbstractBinding is installed for this property"),
            QStringLiteral("runtime metadata cannot distinguish original literal assignment from imperative writes or a removed binding"),
        });

        const PropertySourceSnippet snippet = propertySourceSnippet(objectLocation, propertyName, objectLine);
        QJsonObject assignmentLocation;
        if (!snippet.expression.isEmpty()) {
            insertCandidateIdentifiers(&provenance, snippet.expression);
            assignmentLocation = makeLocation(
                    objectLocation.value(QStringLiteral("file")).toString(),
                    snippet.line, snippet.column, snippet.method, snippet.confidence,
                    snippet.limitations);
            provenance.insert(QStringLiteral("sourceAssignment"), QJsonObject{
                { QStringLiteral("expression"), snippet.expression },
                { QStringLiteral("sourceLocation"), assignmentLocation },
            });
        }
        result.insert(QStringLiteral("provenance"), provenance);
        // Agent-first ranked answer: the caller's question is "which line
        // do I edit to change this value". The dossier above remains for
        // verification; this field applies the precedence for them.
        result.insert(QStringLiteral("assignmentSite"),
                      assignmentSite(assignmentLocation, objectLocation,
                                     QStringLiteral("sourceAssignment")));
        return result;
    }

    const QQmlBinding *qmlBinding = dynamic_cast<const QQmlBinding *>(binding);
    const QJsonObject bindingLocation = binding
            ? sourceLocationFromBinding(binding)
            : sourceLocationFromBindableBinding(qpropertyBinding);
    provenance.insert(QStringLiteral("kind"), QStringLiteral("binding"));
    provenance.insert(QStringLiteral("isBinding"), true);
    provenance.insert(QStringLiteral("bindingKind"),
                      binding ? bindingKindName(binding->kind())
                              : QStringLiteral("qpropertyBinding"));
    provenance.insert(QStringLiteral("confidence"), qmlBinding ? 0.85
                                                               : (qpropertyBinding ? 0.65 : 0.70));
    QJsonArray limitations{
        QStringLiteral("binding metadata is read from QQmlAbstractBinding private API"),
        QStringLiteral("expression text is reported as a bounded source snippet when available, not a parsed QML AST"),
        QStringLiteral("runtime dependency capture can under-report unevaluated branches and lazy paths"),
    };
    const bool hasBindingSourceLocation =
            bindingLocation.value(QStringLiteral("confidence")).toDouble() > 0.0;
    if (qpropertyBinding) {
        limitations.replace(0, QStringLiteral("binding metadata is read from Qt bindable-property private API"));
        limitations.append(QStringLiteral("bindable-property dependency values are not exposed in this first implementation"));
    }
    provenance.insert(QStringLiteral("limitations"), limitations);

    const QJsonObject snippetBaseLocation =
            bindingLocation.value(QStringLiteral("file")).toString().isEmpty()
            ? objectLocation
            : bindingLocation;
    const PropertySourceSnippet snippet = propertySourceSnippet(snippetBaseLocation, propertyName,
                                                               objectLine);
    const QJsonArray snippetCandidates = candidateIdentifiersFromExpression(snippet.expression);
    const bool mayUseObjectSourceFallback = !qpropertyBinding || hasBindingSourceLocation
            || !snippetCandidates.isEmpty();
    if (qpropertyBinding && !hasBindingSourceLocation && !mayUseObjectSourceFallback) {
        QJsonArray updatedLimitations = provenance.value(QStringLiteral("limitations")).toArray();
        updatedLimitations.append(QStringLiteral("source expression omitted because generic QProperty binding metadata does not identify whether the active binding came from the object body, a state, or imperative replacement"));
        provenance.insert(QStringLiteral("limitations"), updatedLimitations);
    }
    QJsonObject expressionLocation;
    if (mayUseObjectSourceFallback && !snippet.expression.isEmpty()) {
        provenance.insert(QStringLiteral("expression"), snippet.expression);
        if (!snippetCandidates.isEmpty())
            provenance.insert(QStringLiteral("candidateIdentifiers"), snippetCandidates);
        expressionLocation = makeLocation(snippetBaseLocation.value(QStringLiteral("file")).toString(),
                                          snippet.line, snippet.column, snippet.method,
                                          snippet.confidence, snippet.limitations);
        provenance.insert(QStringLiteral("expressionSourceLocation"), expressionLocation);
    }
    provenance.insert(QStringLiteral("sourceLocation"),
                      bindingLocation.value(QStringLiteral("confidence")).toDouble() > 0.0
                              || expressionLocation.isEmpty()
                              ? bindingLocation
                              : expressionLocation);

    if (qmlBinding) {
        const QJsonArray dependencies = dependencyEvidence(qmlBinding);
        provenance.insert(QStringLiteral("dependencies"), dependencies);
        provenance.insert(QStringLiteral("dependencyLimitations"), QJsonArray{
            QStringLiteral("dependencies are the currently captured runtime dependency set and may change after reevaluation"),
        });
    } else if (qpropertyBinding) {
        provenance.insert(QStringLiteral("dependencies"), QJsonArray{});
        provenance.insert(QStringLiteral("dependencySummary"),
                          bindableDependencySummary(qpropertyBinding));
        provenance.insert(QStringLiteral("dependencyLimitations"), QJsonArray{
            QStringLiteral("Qt bindable-property dependencies are tracked internally but not yet exposed as property/value pairs"),
        });
    }

    result.insert(QStringLiteral("provenance"), provenance);
    const QJsonObject bestBindingLocation =
            provenance.value(QStringLiteral("sourceLocation")).toObject();
    result.insert(QStringLiteral("assignmentSite"),
                  assignmentSite(bestBindingLocation, objectLocation,
                                 QStringLiteral("bindingLocation")));
    return result;
}

QJsonArray QQmlAgentSourceResolver::fallbackLocationsForObject(const QObject *object,
                                                               const QJsonObject &primaryLocation)
{
    QJsonArray fallbackLocations;
    if (!object || object->objectName().isEmpty())
        return fallbackLocations;

    const QQmlData *data = QQmlData::get(object);
    const QString file = primaryLocation.value(QStringLiteral("file")).toString(fileFromData(data));
    if (file.isEmpty())
        return fallbackLocations;

    QFile sourceFile(readableSourcePath(file));
    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallbackLocations;

    const QString source = QString::fromUtf8(sourceFile.readAll());
    const int line = uniqueObjectNameBindingLine(source, object->objectName());
    if (line <= 0)
        return fallbackLocations;

    fallbackLocations.append(makeLocation(
            file, line, -1, QStringLiteral("objectName-source-scan"), 0.75,
            { QStringLiteral("static fallback from unique objectName binding; points to the "
                             "objectName property, not necessarily the object declaration") }));
    return fallbackLocations;
}

QJsonObject QQmlAgentSourceResolver::sourceLocationForWarning(const QQmlError &warning)
{
    const QString file = warning.url().toString();
    if (!file.isEmpty() && warning.line() > 0) {
        return makeLocation(file, warning.line(), warning.column(),
                            QStringLiteral("qml-warning"), 1.0);
    }

    if (!file.isEmpty()) {
        return makeLocation(file, -1, -1, QStringLiteral("qml-warning"), 0.55,
                            { QStringLiteral("warning has URL but no exact line") });
    }

    return unknownLocation(QStringLiteral("qml-warning"));
}

QJsonObject QQmlAgentSourceResolver::sourceLocationForMessageContext(
        const QMessageLogContext &context)
{
    const QString file = QString::fromUtf8(context.file ? context.file : "");
    if (!file.isEmpty() && context.line > 0) {
        return makeLocation(file, context.line, -1, QStringLiteral("qml-warning"), 0.70,
                            { QStringLiteral("location came from QMessageLogContext") });
    }
    return unknownLocation(QStringLiteral("unknown"));
}

QT_END_NAMESPACE
