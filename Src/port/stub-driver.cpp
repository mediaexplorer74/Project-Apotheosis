#include "WebCoreDriver.h"
#include <cstdint>

extern "C" {

int WebCoreRenderHtml(const char*, int, int, uint8_t*) { return 0; }
int WebCoreLoadUrl(const char*, int, int, uint8_t*) { return 0; }
void WebCoreSetCACertPath(const char*) { }
void WebCoreReleaseMemory(int) { }
int WebCoreSessionLoad(const char*, int, int, uint8_t*) { return 0; }
void WebCoreCloseSession() { }
int WebCoreClickAt(int, int, uint8_t*) { return 0; }
int WebCoreScrollBy(int, int, uint8_t*) { return 0; }
int WebCoreSyncLinks() { return 0; }
int WebCoreEditDebug(char*, int) { return 0; }
int WebCoreSetPageScale(float, int, int, uint8_t*) { return 0; }
int WebCoreGetPageScale() { return 0; }
int WebCoreSessionPaint(uint8_t*) { return 0; }
int WebCoreGetUrl(char*, int) { return 0; }
int WebCoreFocusedEditable() { return 0; }
int WebCoreTypeText(const char*, uint8_t*) { return 0; }
int WebCoreKeyAction(int, uint8_t*) { return 0; }
void WebCoreSetUserAgentMobile(int) { }
void WebCoreSetUserAgentString(const char*) { }
int WebCoreEnableCompositing() { return 0; }
int WebCoreGpuInit(void*, int, int) { return 0; }
int WebCoreComposite() { return 0; }
int WebCoreCompositeReadback(uint8_t*) { return 0; }
void WebCoreGpuSetFlip(int, int) { }
int WebCoreGpuLayerInfo(char*, int) { return 0; }
int WebCoreEvalJS(const char*, char*, int) { return 0; }
int WebCoreLiveTick(uint8_t*) { return 0; }
int WebCoreGetPendingResourceCount() { return 0; }
unsigned WebCoreGetFrameHash() { return 0; }
int WebCoreFindString(const char*, int, int, uint8_t*) { return 0; }
int WebCoreFindNext(int, uint8_t*) { return 0; }
int WebCoreFindClear(uint8_t*) { return 0; }
int WebCoreRenderHtmlStub(const char*, int, int, uint8_t*) { return 0; }
void WebCoreSetCACertBlob(const uint8_t*, int) { }
int WebCoreGetLastError(char*, int) { return 0; }
int WebCoreGetDiag(char*, int) { return 0; }
int WebCoreGetTitle(char*, int) { return 0; }
int WebCoreDownload(const char*, const char*) { return 200; }
int WebCoreGetLinkCount() { return 0; }
int WebCoreGetLink(int, int*, int*, int*, int*, char*, int) { return 0; }

}
