#pragma once
#include "a4_model.h"
#include <d2d1_1.h>
#include <wrl/client.h>
namespace nekorox::component::a4 {
enum class Pass:unsigned { field=0,glass=1,cover=2,film=3,encode=4 };
HRESULT createEffect(ID2D1DeviceContext*,Pass,ID2D1Effect**) noexcept;
HRESULT updateEffect(ID2D1Effect*,const Constants&) noexcept;
}
