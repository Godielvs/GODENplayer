#pragma once

#include <windows.h>
#include <oleacc.h>
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nekorox::component {

struct AccessibleSurfaceNode {
    std::wstring name, value;
    RECT screen{};
    long role=ROLE_SYSTEM_PUSHBUTTON, state=STATE_SYSTEM_FOCUSABLE;
};

// Simple MSAA children for controls drawn inside a retained D2D surface.
// The owner detaches callbacks before destroying its HWND; COM clients may
// retain the provider afterwards without retaining a dangling panel pointer.
class AccessibleSurface final : public IAccessible {
public:
    using Nodes=std::function<std::vector<AccessibleSurfaceNode>()>;
    AccessibleSurface(HWND window,Nodes nodes,std::function<long()> focus,
        std::function<void(long,bool)> activate):window_(window),nodes_(std::move(nodes)),
        focus_(std::move(focus)),activate_(std::move(activate)){}
    void detach(){window_=nullptr;nodes_={};focus_={};activate_={};}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=IID_IUnknown&&id!=IID_IDispatch&&id!=IID_IAccessible)return E_NOINTERFACE;
        *out=static_cast<IAccessible*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override{if(!n)return E_POINTER;*n=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*) override{return DISP_E_UNKNOWNNAME;}
    HRESULT STDMETHODCALLTYPE Invoke(DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*) override{return DISP_E_MEMBERNOTFOUND;}
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        return window_?AccessibleObjectFromWindow(GetParent(window_),OBJID_CLIENT,IID_IDispatch,reinterpret_cast<void**>(out)):S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* n) override{if(!n)return E_POINTER;*n=long(nodes().size());return S_OK;}
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT,IDispatch** out) override{if(!out)return E_POINTER;*out=nullptr;return S_FALSE;}
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT c,BSTR* out) override {
        if(c.vt==VT_I4&&c.lVal==CHILDID_SELF)return string(L"GODENplayer Album",out);
        const auto n=node(c);return n?string(n->name,out):empty(out);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT c,BSTR* out) override{const auto n=node(c);return n?string(n->value,out):empty(out);}
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT,BSTR* out) override{return string(L"Tab moves focus. Enter or Space activates. Arrow keys select tracks; Page Up and Page Down scroll lyrics.",out);}
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT c,VARIANT* out) override {
        if(!out)return E_POINTER;VariantInit(out);out->vt=VT_I4;
        if(c.vt==VT_I4&&c.lVal==CHILDID_SELF){out->lVal=ROLE_SYSTEM_PANE;return S_OK;}
        const auto n=node(c);if(!n)return E_INVALIDARG;out->lVal=n->role;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT c,VARIANT* out) override {
        if(!out)return E_POINTER;VariantInit(out);out->vt=VT_I4;
        if(c.vt==VT_I4&&c.lVal==CHILDID_SELF){out->lVal=window_?0:STATE_SYSTEM_UNAVAILABLE;return S_OK;}
        const auto n=node(c);if(!n)return E_INVALIDARG;out->lVal=n->state;
        if(GetFocus()==window_&&focus_&&focus_()==c.lVal)out->lVal|=STATE_SYSTEM_FOCUSED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT,BSTR* out) override{return empty(out);}
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file,VARIANT,long* topic) override{if(file)*file=nullptr;if(topic)*topic=0;return S_FALSE;}
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT,BSTR* out) override{return empty(out);}
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out) override {
        if(!out)return E_POINTER;VariantInit(out);
        if(window_&&GetFocus()==window_&&focus_){out->vt=VT_I4;out->lVal=focus_();return S_OK;}return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out) override{return get_accFocus(out);}
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT c,BSTR* out) override{return node(c)?string(L"Activate",out):empty(out);}
    HRESULT STDMETHODCALLTYPE accSelect(long flags,VARIANT c) override {
        const auto n=node(c);if(!n || (n->state&STATE_SYSTEM_INVISIBLE))return E_INVALIDARG;
        if(flags&SELFLAG_TAKEFOCUS){if(activate_)activate_(c.lVal,false);return S_OK;}return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* x,long* y,long* w,long* h,VARIANT c) override {
        if(!x||!y||!w||!h)return E_POINTER;RECT rect{};
        if(c.vt==VT_I4&&c.lVal==CHILDID_SELF){if(!window_)return S_FALSE;GetWindowRect(window_,&rect);}
        else{const auto n=node(c);if(!n)return E_INVALIDARG;rect=n->screen;}
        *x=rect.left;*y=rect.top;*w=rect.right-rect.left;*h=rect.bottom-rect.top;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction,VARIANT start,VARIANT* out) override {
        if(!out)return E_POINTER;VariantInit(out);if(start.vt!=VT_I4)return E_INVALIDARG;
        const auto all=nodes();long i=direction==NAVDIR_FIRSTCHILD?1:direction==NAVDIR_LASTCHILD?long(all.size()):
            direction==NAVDIR_NEXT?start.lVal+1:direction==NAVDIR_PREVIOUS?start.lVal-1:0;
        const auto step=direction==NAVDIR_PREVIOUS||direction==NAVDIR_LASTCHILD?-1:1;
        for(;i>0&&i<=long(all.size());i+=step)if(!(all[i-1].state&STATE_SYSTEM_INVISIBLE)){out->vt=VT_I4;out->lVal=i;return S_OK;}
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x,long y,VARIANT* out) override {
        if(!out)return E_POINTER;VariantInit(out);const auto all=nodes();POINT p{x,y};
        for(std::size_t i=0;i<all.size();++i)if(!(all[i].state&STATE_SYSTEM_INVISIBLE)&&PtInRect(&all[i].screen,p)){
            out->vt=VT_I4;out->lVal=long(i+1);return S_OK;}
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT c) override {
        const auto n=node(c);if(!n || (n->state&(STATE_SYSTEM_UNAVAILABLE|STATE_SYSTEM_INVISIBLE)))return E_INVALIDARG;
        if(activate_)activate_(c.lVal,true);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT,BSTR) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT,BSTR) override{return E_NOTIMPL;}
private:
    std::vector<AccessibleSurfaceNode> nodes()const{return nodes_?nodes_():std::vector<AccessibleSurfaceNode>{};}
    std::optional<AccessibleSurfaceNode> node(VARIANT c)const{
        if(c.vt!=VT_I4||c.lVal<=0)return {};auto all=nodes();
        return std::size_t(c.lVal)<=all.size()?std::optional<AccessibleSurfaceNode>(all[c.lVal-1]):std::nullopt;
    }
    static HRESULT empty(BSTR* out){if(!out)return E_POINTER;*out=nullptr;return S_FALSE;}
    static HRESULT string(const std::wstring& value,BSTR* out){if(!out)return E_POINTER;*out=SysAllocString(value.c_str());return *out?S_OK:E_OUTOFMEMORY;}
    std::atomic<ULONG> references_{1};HWND window_=nullptr;Nodes nodes_;
    std::function<long()> focus_;std::function<void(long,bool)> activate_;
};
} // namespace nekorox::component
