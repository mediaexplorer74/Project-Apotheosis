#include "config.h"
#include <WebCore/ChromeClient.h>

namespace WebCore {

RefPtr<ShapeDetection::BarcodeDetector> ChromeClient::createBarcodeDetector(const ShapeDetection::BarcodeDetectorOptions&) const
{
    return nullptr;
}

void ChromeClient::getBarcodeDetectorSupportedFormats(CompletionHandler<void(Vector<ShapeDetection::BarcodeFormat>&&)>&& completion) const
{
    completion({ });
}

RefPtr<ShapeDetection::FaceDetector> ChromeClient::createFaceDetector(const ShapeDetection::FaceDetectorOptions&) const
{
    return nullptr;
}

RefPtr<ShapeDetection::TextDetector> ChromeClient::createTextDetector() const
{
    return nullptr;
}

} // namespace WebCore
