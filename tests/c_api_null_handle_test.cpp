// 本文件由 scripts/gen_c_api_null_handle_test.py 从 include/op_c_api.h 生成，**勿手改**。
// 改契约请改生成器或头文件后重跑：python scripts/gen_c_api_null_handle_test.py
//
// 契约：带 op_handle 的 C API 在 handle=nullptr 时必须返回**失败值**且不崩溃
//       （int/intptr_t → 0，const wchar_t* → 空串）；索引语义 API 例外（-1）。

#include "op_c_api.h"

#include <gtest/gtest.h>

namespace {

unsigned char g_pixel4[4] = {0, 0, 0, 0};
long g_words4[4] = {0, 0, 0, 0};

void expect_empty(const wchar_t *s, const char *api) {
    ASSERT_NE(s, nullptr) << api << " returned null instead of empty string";
    EXPECT_EQ(s[0], L'\0') << api << " returned non-empty string for null handle";
}

void expect_non_empty(const wchar_t *s, const char *api) {
    ASSERT_NE(s, nullptr) << api;
    EXPECT_NE(s[0], L'\0') << api << " returned empty";
}

// JSON 结果型 API 的失败串：至少含 "ok":0。不同 API 字段数不同
// （如 OpYoloDetectFromFile 返回 {"ok":0,"code":-1,"results":[]}），
// 只卡 "ok":0 这个不变量，避免以后加字段就误报。
void expect_json_failure(const wchar_t *s, const char *api) {
    ASSERT_NE(s, nullptr) << api;
    EXPECT_NE(std::wstring(s).find(L"\"ok\":0"), std::wstring::npos) << api << " -> " << s;
}

} // namespace

TEST(CApiNullHandle, AllHandleFunctionsReturnFailure) {
    OpDestroy(nullptr);  // void：只保证不崩溃
    EXPECT_EQ(OpSetPath(nullptr, L""), 0) << "OpSetPath";
    expect_empty(OpGetPath(nullptr), "OpGetPath");
    expect_empty(OpGetBasePath(nullptr), "OpGetBasePath");
    EXPECT_EQ(OpGetID(nullptr), 0) << "OpGetID";
    EXPECT_EQ(OpGetLastError(nullptr), 0) << "OpGetLastError";
    EXPECT_EQ(OpSetShowErrorMsg(nullptr, 0), 0) << "OpSetShowErrorMsg";
    EXPECT_EQ(OpSleep(nullptr, 0), 0) << "OpSleep";
    EXPECT_EQ(OpInjectDll(nullptr, L"", L""), 0) << "OpInjectDll";
    EXPECT_EQ(OpEnablePicCache(nullptr, 0), 0) << "OpEnablePicCache";
    EXPECT_EQ(OpClearPicCache(nullptr), 0) << "OpClearPicCache";
    EXPECT_EQ(OpSetPicCacheMax(nullptr, 0), 0) << "OpSetPicCacheMax";
    EXPECT_EQ(OpSetAStarMap(nullptr, L"", 0, 0, 0), 0) << "OpSetAStarMap";
    EXPECT_EQ(OpSetAStarMapData(nullptr, 0, 0, nullptr, 0, 0, 0, 0), 0) << "OpSetAStarMapData";
    expect_empty(OpAStarFindPathBM(nullptr, 0, 0, 0, 0), "OpAStarFindPathBM");
    expect_empty(OpAStarFindPathWay(nullptr, L""), "OpAStarFindPathWay");
    expect_empty(OpSmoothPathByLOS(nullptr, L""), "OpSmoothPathByLOS");
    expect_empty(OpSimplifyPath(nullptr, L"", 0.0), "OpSimplifyPath");
    EXPECT_EQ(OpIsLineBlocked(nullptr, 0, 0, 0, 0), 0) << "OpIsLineBlocked";
    EXPECT_EQ(OpFindNearestPathPoint(nullptr, L"", 0, 0, nullptr, nullptr, nullptr), 0) << "OpFindNearestPathPoint";
    EXPECT_EQ(OpPointInPolygon(nullptr, L"", L""), 0) << "OpPointInPolygon";
    EXPECT_EQ(OpCapturePre(nullptr, L""), 0) << "OpCapturePre";
    EXPECT_EQ(OpSetScreenDataMode(nullptr, 0), 0) << "OpSetScreenDataMode";
    expect_empty(OpAStarFindPath(nullptr, 0, 0, L"", 0, 0, 0, 0), "OpAStarFindPath");
    expect_empty(OpFindNearestPos(nullptr, L"", 0, 0, 0), "OpFindNearestPos");
    expect_empty(OpEnumWindow(nullptr, 0, L"", L"", 0), "OpEnumWindow");
    expect_empty(OpEnumWindowByProcess(nullptr, L"", L"", L"", 0), "OpEnumWindowByProcess");
    expect_empty(OpEnumProcess(nullptr, L""), "OpEnumProcess");
    EXPECT_EQ(OpClientToScreen(nullptr, 0, nullptr, nullptr), 0) << "OpClientToScreen";
    EXPECT_EQ(OpFindWindow(nullptr, L"", L""), 0) << "OpFindWindow";
    EXPECT_EQ(OpFindWindowByProcess(nullptr, L"", L"", L""), 0) << "OpFindWindowByProcess";
    EXPECT_EQ(OpFindWindowByProcessId(nullptr, 0, L"", L""), 0) << "OpFindWindowByProcessId";
    EXPECT_EQ(OpFindWindowEx(nullptr, 0, L"", L""), 0) << "OpFindWindowEx";
    EXPECT_EQ(OpGetClientRect(nullptr, 0, nullptr, nullptr, nullptr, nullptr), 0) << "OpGetClientRect";
    EXPECT_EQ(OpGetClientSize(nullptr, 0, nullptr, nullptr), 0) << "OpGetClientSize";
    EXPECT_EQ(OpGetForegroundFocus(nullptr), 0) << "OpGetForegroundFocus";
    EXPECT_EQ(OpGetForegroundWindow(nullptr), 0) << "OpGetForegroundWindow";
    EXPECT_EQ(OpGetMousePointWindow(nullptr), 0) << "OpGetMousePointWindow";
    EXPECT_EQ(OpGetPointWindow(nullptr, 0, 0), 0) << "OpGetPointWindow";
    expect_empty(OpGetProcessInfo(nullptr, 0), "OpGetProcessInfo");
    EXPECT_EQ(OpGetSpecialWindow(nullptr, 0), 0) << "OpGetSpecialWindow";
    EXPECT_EQ(OpGetWindow(nullptr, 0, 0), 0) << "OpGetWindow";
    expect_empty(OpGetWindowClass(nullptr, 0), "OpGetWindowClass");
    EXPECT_EQ(OpGetWindowProcessId(nullptr, 0), 0) << "OpGetWindowProcessId";
    expect_empty(OpGetWindowProcessPath(nullptr, 0), "OpGetWindowProcessPath");
    EXPECT_EQ(OpGetWindowRect(nullptr, 0, nullptr, nullptr, nullptr, nullptr), 0) << "OpGetWindowRect";
    EXPECT_EQ(OpGetWindowState(nullptr, 0, 0), 0) << "OpGetWindowState";
    expect_empty(OpGetWindowTitle(nullptr, 0), "OpGetWindowTitle");
    EXPECT_EQ(OpMoveWindow(nullptr, 0, 0, 0), 0) << "OpMoveWindow";
    EXPECT_EQ(OpScreenToClient(nullptr, 0, nullptr, nullptr), 0) << "OpScreenToClient";
    EXPECT_EQ(OpSendPaste(nullptr, 0), 0) << "OpSendPaste";
    EXPECT_EQ(OpSetClientSize(nullptr, 0, 0, 0), 0) << "OpSetClientSize";
    EXPECT_EQ(OpSetWindowState(nullptr, 0, 0), 0) << "OpSetWindowState";
    EXPECT_EQ(OpSetWindowSize(nullptr, 0, 0, 0), 0) << "OpSetWindowSize";
    EXPECT_EQ(OpLayoutWindows(nullptr, L"", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 0) << "OpLayoutWindows";
    EXPECT_EQ(OpSetWindowText(nullptr, 0, L""), 0) << "OpSetWindowText";
    EXPECT_EQ(OpSetWindowTransparent(nullptr, 0, 0), 0) << "OpSetWindowTransparent";
    EXPECT_EQ(OpSendString(nullptr, 0, L""), 0) << "OpSendString";
    EXPECT_EQ(OpSendStringIme(nullptr, 0, L""), 0) << "OpSendStringIme";
    EXPECT_EQ(OpLockWindowPosition(nullptr, 0, 0), 0) << "OpLockWindowPosition";
    EXPECT_EQ(OpLockWindowSize(nullptr, 0, 0), 0) << "OpLockWindowSize";
    EXPECT_EQ(OpDisableMinMax(nullptr, 0, 0), 0) << "OpDisableMinMax";
    EXPECT_EQ(OpSetIme(nullptr, 0, 0), 0) << "OpSetIme";
    EXPECT_EQ(OpRunApp(nullptr, L"", 0, nullptr), 0) << "OpRunApp";
    EXPECT_EQ(OpWinExec(nullptr, L"", 0), 0) << "OpWinExec";
    expect_empty(OpGetCmdStr(nullptr, L"", 0), "OpGetCmdStr");
    EXPECT_EQ(OpSetClipboard(nullptr, L""), 0) << "OpSetClipboard";
    expect_empty(OpGetClipboard(nullptr), "OpGetClipboard");
    EXPECT_EQ(OpDelay(nullptr, 0), 0) << "OpDelay";
    EXPECT_EQ(OpDelays(nullptr, 0, 0), 0) << "OpDelays";
    EXPECT_EQ(OpGetScreenWidth(nullptr), 0) << "OpGetScreenWidth";
    EXPECT_EQ(OpGetScreenHeight(nullptr), 0) << "OpGetScreenHeight";
    EXPECT_EQ(OpGetScreenDepth(nullptr), 0) << "OpGetScreenDepth";
    EXPECT_EQ(OpGetDPI(nullptr), 0) << "OpGetDPI";
    expect_empty(OpGetTime(nullptr), "OpGetTime");
    EXPECT_EQ(OpBeep(nullptr, 0, 0), 0) << "OpBeep";
    EXPECT_EQ(OpGetRandomNumber(nullptr, 0, 0), 0) << "OpGetRandomNumber";
    (void)OpGetRandomDouble(nullptr, 0.0, 0.0);  // 未知返回类型：只保证不崩溃
    EXPECT_EQ(OpGaiLu(nullptr, 0), 0) << "OpGaiLu";
    expect_empty(OpGetMachineCode(nullptr), "OpGetMachineCode");
    EXPECT_EQ(OpIsElevated(nullptr), 0) << "OpIsElevated";
    EXPECT_EQ(OpBindWindow(nullptr, 0, L"", L"", L"", 0), 0) << "OpBindWindow";
    EXPECT_EQ(OpBindWindowEx(nullptr, 0, 0, L"", L"", L"", 0), 0) << "OpBindWindowEx";
    EXPECT_EQ(OpUnBindWindow(nullptr), 0) << "OpUnBindWindow";
    EXPECT_EQ(OpLockInput(nullptr, 0), 0) << "OpLockInput";
    EXPECT_EQ(OpDownCpu(nullptr, 0, 0), 0) << "OpDownCpu";
    EXPECT_EQ(OpSetDxAttr(nullptr, 0, 0), 0) << "OpSetDxAttr";
    EXPECT_EQ(OpGetDxAttr(nullptr), 0) << "OpGetDxAttr";
    EXPECT_EQ(OpGetBindWindow(nullptr), 0) << "OpGetBindWindow";
    EXPECT_EQ(OpIsBind(nullptr), 0) << "OpIsBind";
    EXPECT_EQ(OpGetCursorPos(nullptr, nullptr, nullptr), 0) << "OpGetCursorPos";
    expect_empty(OpGetCursorShape(nullptr), "OpGetCursorShape");
    EXPECT_EQ(OpMoveR(nullptr, 0, 0), 0) << "OpMoveR";
    EXPECT_EQ(OpMoveTo(nullptr, 0, 0), 0) << "OpMoveTo";
    expect_empty(OpMoveToEx(nullptr, 0, 0, 0, 0), "OpMoveToEx");
    EXPECT_EQ(OpMoveToSmooth(nullptr, 0, 0, 0), 0) << "OpMoveToSmooth";
    expect_empty(OpMoveToExSmooth(nullptr, 0, 0, 0, 0, 0), "OpMoveToExSmooth");
    EXPECT_EQ(OpMovePath(nullptr, L"", 0), 0) << "OpMovePath";
    EXPECT_EQ(OpDragPath(nullptr, L"", 0), 0) << "OpDragPath";
    EXPECT_EQ(OpSetMouseTrajectory(nullptr, 0, 0, 0, 0, 0, 0), 0) << "OpSetMouseTrajectory";
    EXPECT_EQ(OpLeftClick(nullptr), 0) << "OpLeftClick";
    EXPECT_EQ(OpLeftDoubleClick(nullptr), 0) << "OpLeftDoubleClick";
    EXPECT_EQ(OpLeftDown(nullptr), 0) << "OpLeftDown";
    EXPECT_EQ(OpLeftUp(nullptr), 0) << "OpLeftUp";
    EXPECT_EQ(OpMiddleClick(nullptr), 0) << "OpMiddleClick";
    EXPECT_EQ(OpMiddleDoubleClick(nullptr), 0) << "OpMiddleDoubleClick";
    EXPECT_EQ(OpMiddleDown(nullptr), 0) << "OpMiddleDown";
    EXPECT_EQ(OpMiddleUp(nullptr), 0) << "OpMiddleUp";
    EXPECT_EQ(OpRightClick(nullptr), 0) << "OpRightClick";
    EXPECT_EQ(OpRightDoubleClick(nullptr), 0) << "OpRightDoubleClick";
    EXPECT_EQ(OpRightDown(nullptr), 0) << "OpRightDown";
    EXPECT_EQ(OpRightUp(nullptr), 0) << "OpRightUp";
    EXPECT_EQ(OpXButton1Click(nullptr), 0) << "OpXButton1Click";
    EXPECT_EQ(OpXButton1DoubleClick(nullptr), 0) << "OpXButton1DoubleClick";
    EXPECT_EQ(OpXButton1Down(nullptr), 0) << "OpXButton1Down";
    EXPECT_EQ(OpXButton1Up(nullptr), 0) << "OpXButton1Up";
    EXPECT_EQ(OpXButton2Click(nullptr), 0) << "OpXButton2Click";
    EXPECT_EQ(OpXButton2DoubleClick(nullptr), 0) << "OpXButton2DoubleClick";
    EXPECT_EQ(OpXButton2Down(nullptr), 0) << "OpXButton2Down";
    EXPECT_EQ(OpXButton2Up(nullptr), 0) << "OpXButton2Up";
    EXPECT_EQ(OpWheel(nullptr, 0), 0) << "OpWheel";
    EXPECT_EQ(OpHWheel(nullptr, 0), 0) << "OpHWheel";
    EXPECT_EQ(OpWheelDown(nullptr), 0) << "OpWheelDown";
    EXPECT_EQ(OpWheelUp(nullptr), 0) << "OpWheelUp";
    EXPECT_EQ(OpSetMouseDelay(nullptr, L"", 0), 0) << "OpSetMouseDelay";
    EXPECT_EQ(OpGetKeyState(nullptr, 0), 0) << "OpGetKeyState";
    EXPECT_EQ(OpKeyDown(nullptr, 0), 0) << "OpKeyDown";
    EXPECT_EQ(OpKeyDownChar(nullptr, L""), 0) << "OpKeyDownChar";
    EXPECT_EQ(OpKeyUp(nullptr, 0), 0) << "OpKeyUp";
    EXPECT_EQ(OpKeyUpChar(nullptr, L""), 0) << "OpKeyUpChar";
    EXPECT_EQ(OpWaitKey(nullptr, 0, 0), 0) << "OpWaitKey";
    EXPECT_EQ(OpKeyPress(nullptr, 0), 0) << "OpKeyPress";
    EXPECT_EQ(OpKeyPressChar(nullptr, L""), 0) << "OpKeyPressChar";
    EXPECT_EQ(OpSetKeypadDelay(nullptr, L"", 0), 0) << "OpSetKeypadDelay";
    EXPECT_EQ(OpKeyPressStr(nullptr, L"", 0), 0) << "OpKeyPressStr";
    EXPECT_EQ(OpCapture(nullptr, 0, 0, 0, 0, L""), 0) << "OpCapture";
    EXPECT_EQ(OpCmpColor(nullptr, 0, 0, L"", 0.0), 0) << "OpCmpColor";
    EXPECT_EQ(OpFindColor(nullptr, 0, 0, 0, 0, L"", 0.0, 0, nullptr, nullptr), 0) << "OpFindColor";
    expect_empty(OpFindColorEx(nullptr, 0, 0, 0, 0, L"", 0.0, 0), "OpFindColorEx");
    EXPECT_EQ(OpFindMultiColor(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0, nullptr, nullptr), 0) << "OpFindMultiColor";
    expect_empty(OpFindMultiColorEx(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0), "OpFindMultiColorEx");
    EXPECT_EQ(OpFindPic(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0, nullptr, nullptr), -1) << "OpFindPic"; // 索引语义：-1=未找到
    expect_empty(OpFindPicEx(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0), "OpFindPicEx");
    expect_empty(OpFindPicExS(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0), "OpFindPicExS");
    EXPECT_EQ(OpFindColorBlock(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0, nullptr, nullptr), 0) << "OpFindColorBlock";
    expect_empty(OpFindColorBlockEx(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0), "OpFindColorBlockEx");
    expect_empty(OpGetColor(nullptr, 0, 0), "OpGetColor");
    EXPECT_EQ(OpGetColorNum(nullptr, 0, 0, 0, 0, L"", 0.0), 0) << "OpGetColorNum";
    EXPECT_EQ(OpSetDisplayInput(nullptr, L""), 0) << "OpSetDisplayInput";
    EXPECT_EQ(OpLoadPic(nullptr, L""), 0) << "OpLoadPic";
    EXPECT_EQ(OpFreePic(nullptr, L""), 0) << "OpFreePic";
    EXPECT_EQ(OpLoadMemPic(nullptr, L"", nullptr, 0), 0) << "OpLoadMemPic";
    EXPECT_EQ(OpGetPicSize(nullptr, L"", nullptr, nullptr), 0) << "OpGetPicSize";
    EXPECT_EQ(OpGetScreenData(nullptr, 0, 0, 0, 0, nullptr), 0) << "OpGetScreenData";
    EXPECT_EQ(OpGetScreenDataBmp(nullptr, 0, 0, 0, 0, nullptr, nullptr), 0) << "OpGetScreenDataBmp";
    OpGetScreenFrameInfo(nullptr, nullptr, nullptr);  // void：只保证不崩溃
    EXPECT_EQ(OpGetFPS(nullptr), 0) << "OpGetFPS";
    expect_empty(OpMatchPicName(nullptr, L""), "OpMatchPicName");
    EXPECT_EQ(OpCvLoadTemplate(nullptr, L"", L""), 0) << "OpCvLoadTemplate";
    EXPECT_EQ(OpCvLoadMaskedTemplate(nullptr, L"", L"", L""), 0) << "OpCvLoadMaskedTemplate";
    EXPECT_EQ(OpCvRemoveTemplate(nullptr, L""), 0) << "OpCvRemoveTemplate";
    EXPECT_EQ(OpCvRemoveAllTemplates(nullptr), 0) << "OpCvRemoveAllTemplates";
    EXPECT_EQ(OpCvHasTemplate(nullptr, L""), 0) << "OpCvHasTemplate";
    EXPECT_EQ(OpCvGetTemplateCount(nullptr), 0) << "OpCvGetTemplateCount";
    expect_empty(OpCvGetAllTemplateNames(nullptr), "OpCvGetAllTemplateNames");
    expect_empty(OpCvGetOpenCvVersion(nullptr), "OpCvGetOpenCvVersion");
    EXPECT_EQ(OpCvLoadTemplateList(nullptr, L""), 0) << "OpCvLoadTemplateList";
    EXPECT_EQ(OpCvToGray(nullptr, L"", L""), 0) << "OpCvToGray";
    EXPECT_EQ(OpCvToBinary(nullptr, L"", L""), 0) << "OpCvToBinary";
    EXPECT_EQ(OpCvToEdge(nullptr, L"", L""), 0) << "OpCvToEdge";
    EXPECT_EQ(OpCvToOutline(nullptr, L"", L""), 0) << "OpCvToOutline";
    EXPECT_EQ(OpCvDenoise(nullptr, L"", L""), 0) << "OpCvDenoise";
    EXPECT_EQ(OpCvEqualize(nullptr, L"", L""), 0) << "OpCvEqualize";
    EXPECT_EQ(OpCvCLAHE(nullptr, L"", L"", 0.0, 0), 0) << "OpCvCLAHE";
    EXPECT_EQ(OpCvBlur(nullptr, L"", L"", L"", 0), 0) << "OpCvBlur";
    EXPECT_EQ(OpCvSharpen(nullptr, L"", L"", 0.0), 0) << "OpCvSharpen";
    EXPECT_EQ(OpCvCropValid(nullptr, L"", L""), 0) << "OpCvCropValid";
    expect_json_failure(OpCvConnectedComponents(nullptr, L"", 0.0), "OpCvConnectedComponents");
    expect_json_failure(OpCvFindContours(nullptr, L"", 0.0), "OpCvFindContours");
    EXPECT_EQ(OpCvPreprocessPipeline(nullptr, L"", L"", L""), 0) << "OpCvPreprocessPipeline";
    EXPECT_EQ(OpCvCrop(nullptr, L"", 0, 0, 0, 0, L""), 0) << "OpCvCrop";
    EXPECT_EQ(OpCvResize(nullptr, L"", 0, 0, L""), 0) << "OpCvResize";
    EXPECT_EQ(OpCvThreshold(nullptr, L"", L"", 0.0, 0.0, L""), 0) << "OpCvThreshold";
    EXPECT_EQ(OpCvInRange(nullptr, L"", L"", L"", L"", L""), 0) << "OpCvInRange";
    EXPECT_EQ(OpCvMorphology(nullptr, L"", L"", L"", 0, 0), 0) << "OpCvMorphology";
    EXPECT_EQ(OpCvThin(nullptr, L"", L"", L""), 0) << "OpCvThin";
    expect_json_failure(OpCvMatchTemplate(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0, 0), "OpCvMatchTemplate");
    expect_json_failure(OpCvMatchTemplateScale(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0, 0), "OpCvMatchTemplateScale");
    expect_json_failure(OpCvMatchAnyTemplate(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0, 0), "OpCvMatchAnyTemplate");
    expect_json_failure(OpCvMatchAllTemplates(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0, 0), "OpCvMatchAllTemplates");
    expect_json_failure(OpCvFeatureMatchTemplate(nullptr, 0, 0, 0, 0, L"", 0.0), "OpCvFeatureMatchTemplate");
    expect_json_failure(OpCvEdgeMatchTemplate(nullptr, 0, 0, 0, 0, L"", 0.0), "OpCvEdgeMatchTemplate");
    expect_json_failure(OpCvShapeMatchTemplate(nullptr, 0, 0, 0, 0, L"", 0.0), "OpCvShapeMatchTemplate");
    expect_json_failure(OpCvMatchTemplateRot(nullptr, 0, 0, 0, 0, L"", L"", 0.0, 0, 0), "OpCvMatchTemplateRot");
    EXPECT_EQ(OpSetOcrEngine(nullptr, L"", L"", L""), 0) << "OpSetOcrEngine";
    EXPECT_EQ(OpSetYoloEngine(nullptr, L"", L"", L""), 0) << "OpSetYoloEngine";
    expect_json_failure(OpYoloDetect(nullptr, 0, 0, 0, 0, 0.0, 0.0), "OpYoloDetect");
    expect_json_failure(OpYoloDetectFromFile(nullptr, L"", 0.0, 0.0), "OpYoloDetectFromFile");
    EXPECT_EQ(OpSetDict(nullptr, 0, L""), 0) << "OpSetDict";
    expect_empty(OpGetDict(nullptr, 0, 0), "OpGetDict");
    EXPECT_EQ(OpSetMemDict(nullptr, 0, nullptr, 0), 0) << "OpSetMemDict";
    EXPECT_EQ(OpUseDict(nullptr, 0), 0) << "OpUseDict";
    EXPECT_EQ(OpAddDict(nullptr, 0, L""), 0) << "OpAddDict";
    EXPECT_EQ(OpSaveDict(nullptr, 0, L""), 0) << "OpSaveDict";
    EXPECT_EQ(OpClearDict(nullptr, 0), 0) << "OpClearDict";
    EXPECT_EQ(OpGetDictCount(nullptr, 0), 0) << "OpGetDictCount";
    EXPECT_EQ(OpGetNowDict(nullptr), 0) << "OpGetNowDict";
    EXPECT_EQ(OpSetBinaryPreprocess(nullptr, 0, 0, 0, 0), 0) << "OpSetBinaryPreprocess";
    EXPECT_EQ(OpGetBinaryPreprocess(nullptr, nullptr, nullptr, nullptr, nullptr), 0) << "OpGetBinaryPreprocess";
    expect_empty(OpFetchWord(nullptr, 0, 0, 0, 0, L"", L""), "OpFetchWord");
    expect_empty(OpFetchWordEx(nullptr, 0, 0, 0, 0, L"", 0.0, L""), "OpFetchWordEx");
    expect_empty(OpExtractWordRects(nullptr, 0, 0, 0, 0, L"", 0.0, 0), "OpExtractWordRects");
    expect_empty(OpExtractWordRectsEx(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0), "OpExtractWordRectsEx");
    expect_empty(OpFetchWords(nullptr, 0, 0, 0, 0, L"", 0.0, L"", 0), "OpFetchWords");
    expect_empty(OpFetchWordsEx(nullptr, 0, 0, 0, 0, L"", 0.0, L"", 0, 0, 0), "OpFetchWordsEx");
    expect_empty(OpFetchWordsByRects(nullptr, 0, 0, 0, 0, L"", 0.0, L"", L""), "OpFetchWordsByRects");
    expect_empty(OpGetBinaryPreview(nullptr, 0, 0, 0, 0, L"", 0.0, nullptr), "OpGetBinaryPreview");
    expect_empty(OpGetWordPreview(nullptr, L"", nullptr), "OpGetWordPreview");
    expect_empty(OpCheckWordDict(nullptr, L"", nullptr), "OpCheckWordDict");
    expect_empty(OpNormalizeWordDict(nullptr, L"", nullptr), "OpNormalizeWordDict");
    expect_empty(OpRenameWordDict(nullptr, L"", L"", nullptr), "OpRenameWordDict");
    expect_empty(OpGetWordsNoDict(nullptr, 0, 0, 0, 0, L""), "OpGetWordsNoDict");
    EXPECT_EQ(OpGetWordResultCount(nullptr, L""), 0) << "OpGetWordResultCount";
    EXPECT_EQ(OpGetWordResultPos(nullptr, L"", 0, nullptr, nullptr), 0) << "OpGetWordResultPos";
    expect_empty(OpGetWordResultStr(nullptr, L"", 0), "OpGetWordResultStr");
    expect_empty(OpOcr(nullptr, 0, 0, 0, 0, L"", 0.0), "OpOcr");
    expect_empty(OpOcrEx(nullptr, 0, 0, 0, 0, L"", 0.0), "OpOcrEx");
    expect_empty(OpAutoOcr(nullptr, 0, 0, 0, 0, L"", 0.0), "OpAutoOcr");
    expect_empty(OpAutoOcrLine(nullptr, 0, 0, 0, 0, L"", 0.0), "OpAutoOcrLine");
    expect_empty(OpAutoOcrEx(nullptr, 0, 0, 0, 0, L"", 0.0, nullptr), "OpAutoOcrEx");
    EXPECT_EQ(OpFindStr(nullptr, 0, 0, 0, 0, L"", L"", 0.0, nullptr, nullptr), 0) << "OpFindStr";
    expect_empty(OpFindStrEx(nullptr, 0, 0, 0, 0, L"", L"", 0.0), "OpFindStrEx");
    expect_empty(OpOcrAuto(nullptr, 0, 0, 0, 0, 0.0), "OpOcrAuto");
    expect_empty(OpOcrFromFile(nullptr, L"", L"", 0.0), "OpOcrFromFile");
    expect_empty(OpAutoOcrFromFile(nullptr, L"", L"", 0.0), "OpAutoOcrFromFile");
    expect_empty(OpOcrAutoFromFile(nullptr, L"", 0.0), "OpOcrAutoFromFile");
    expect_empty(OpFindLine(nullptr, 0, 0, 0, 0, L"", 0.0), "OpFindLine");
    expect_empty(OpFindLineEx(nullptr, 0, 0, 0, 0, L"", 0.0, nullptr), "OpFindLineEx");
    EXPECT_EQ(OpWriteData(nullptr, 0, L"", L"", 0), 0) << "OpWriteData";
    expect_empty(OpReadData(nullptr, 0, L"", 0), "OpReadData");
    EXPECT_EQ(OpReadInt(nullptr, 0, L"", 0, nullptr), 0) << "OpReadInt";
    EXPECT_EQ(OpWriteInt(nullptr, 0, L"", 0, 0), 0) << "OpWriteInt";
    EXPECT_EQ(OpReadFloat(nullptr, 0, L"", nullptr), 0) << "OpReadFloat";
    EXPECT_EQ(OpWriteFloat(nullptr, 0, L"", 0.0), 0) << "OpWriteFloat";
    EXPECT_EQ(OpReadDouble(nullptr, 0, L"", nullptr), 0) << "OpReadDouble";
    EXPECT_EQ(OpWriteDouble(nullptr, 0, L"", 0.0), 0) << "OpWriteDouble";
    expect_empty(OpReadString(nullptr, 0, L"", 0, 0), "OpReadString");
    EXPECT_EQ(OpWriteString(nullptr, 0, L"", 0, L""), 0) << "OpWriteString";
    expect_empty(OpFindData(nullptr, 0, L"", L""), "OpFindData");
    expect_empty(OpFindDataEx(nullptr, 0, L"", L"", 0, 0), "OpFindDataEx");
    expect_empty(OpGetModuleBaseAddr(nullptr, 0, L""), "OpGetModuleBaseAddr");
    expect_empty(OpFindColorBlockExS(nullptr, 0, 0, 0, 0, L"", 0.0, 0, 0, 0, 0), "OpFindColorBlockExS");
    expect_empty(OpFindLineExS(nullptr, 0, 0, 0, 0, L"", 0.0, 0, nullptr), "OpFindLineExS");
}

TEST(CApiNullHandle, FreeFunctionsAreSafe) {
    // OpCreate 无 handle 参数：正常应拿到句柄，销毁后不得崩
    op_handle h = OpCreate();
    EXPECT_NE(h, nullptr) << "OpCreate";
    OpDestroy(h);
    expect_non_empty(OpVer(), "OpVer");
    // 有真实副作用，契约由 image_color_test 钉住，此处只保证不崩溃
    (void)OpRequestCaptureForTest(L"", 0, 0, 0, 0, g_pixel4);
}
