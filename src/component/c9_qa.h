#pragma once
#include <windows.h>
#include "qa_hooks.h"

namespace nekorox::component {
// An opt-in test switch, NOT an authentication boundary against the same user.
inline bool c9QaAllowed() noexcept { return qa::hooksAllowed(); }
inline UINT c9WheelForwardMessage() noexcept {
    static const UINT value=RegisterWindowMessageW(L"GodenAlbumWheelForwardC9V1");
    return value;
}
inline UINT c9DiagnosticsMessage() noexcept {
    static const UINT value=RegisterWindowMessageW(L"GodenC9DiagnosticsV1");
    return value;
}
inline WORD c9WheelKeys() noexcept {
    WORD result=0;
    if(GetAsyncKeyState(VK_CONTROL)&0x8000)result|=MK_CONTROL;
    if(GetAsyncKeyState(VK_SHIFT)&0x8000)result|=MK_SHIFT;
    if(GetAsyncKeyState(VK_LBUTTON)&0x8000)result|=MK_LBUTTON;
    if(GetAsyncKeyState(VK_RBUTTON)&0x8000)result|=MK_RBUTTON;
    if(GetAsyncKeyState(VK_MBUTTON)&0x8000)result|=MK_MBUTTON;
    if(GetAsyncKeyState(VK_XBUTTON1)&0x8000)result|=MK_XBUTTON1;
    if(GetAsyncKeyState(VK_XBUTTON2)&0x8000)result|=MK_XBUTTON2;
    return result;
}
} // namespace
