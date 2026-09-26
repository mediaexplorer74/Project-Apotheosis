#include "config.h"
#include <WebCore/DocumentWriter.h>
#include <WebCore/Editor.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/HTMLNames.h>
#include <WebCore/ResourceRequestBase.h>

namespace WebCore {

void DocumentWriter::addData(const SharedBuffer&)
{
}

bool Editor::insertTextWithoutSendingTextEvent(const String&, bool, TextEvent*)
{
    return false;
}

double ResourceRequestBase::s_defaultTimeoutInterval = 10.0;

bool hasPlatformStrategies()
{
    return false;
}

namespace HTMLNames {

LazyNeverDestroyed<const HTMLQualifiedName> aTag;
LazyNeverDestroyed<const HTMLQualifiedName> inputTag;
LazyNeverDestroyed<const HTMLQualifiedName> textareaTag;

} // namespace HTMLNames
} // namespace WebCore
