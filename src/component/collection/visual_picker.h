#pragma once
#include "collection_preferences.h"
#include "../a4/a4_preferences.h"
#include "../curated/curated_preferences.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace nekorox::component::collection {
inline UINT visualChangedMessage() noexcept {
    static const UINT m=RegisterWindowMessageW(L"Goden.C5.VisualChanged.V1");return m;
}
namespace picker_detail {
inline HWND openWindow=nullptr;
constexpr int Search=3101,Group=3102,List=3103,Fav=3104,Original=3105,Music=3106,
    Album=3107,Author=3108,Retry=3109,Advanced=3110,Quiet=3111,Balanced=3112,Status=3113;
struct Entry {std::string id;std::wstring name;unsigned group=0,index=0,style=4;
    unsigned material=~0U;};
inline unsigned groupFor(std::string_view id) noexcept {
    constexpr std::array frequency{"avtqpukv","rw3dfaou","di76dy6c","tcsfryp6","gtl4w04i"};
    constexpr std::array fluids{"fspiu4nn","csln1b0s","sculpted_ribbons"};
    for(auto x:frequency)if(id==x)return 2;
    for(auto x:fluids)if(id==x)return 3;
    return 4;
}
inline std::wstring lower(std::wstring s){for(auto&c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
struct State {
    HWND owner=nullptr,window=nullptr,search=nullptr,group=nullptr,list=nullptr;
    HFONT font=nullptr;HBRUSH background=nullptr;bool suppress=false,*adopted=nullptr;
    std::vector<Entry> entries;std::vector<unsigned> filtered;
    ~State(){if(font)DeleteObject(font);if(background)DeleteObject(background);}
    void status(){
        const auto chosen=selection();const std::wstring label=selected()
            ? std::wstring(L"Ativo: ")+title()+(chosen.music?L" · Music · ":L" · Original · ")+
                (chosen.albumColors?L"Álbum · ":L"Autor · ")+std::to_wstring(int(chosen.intensity*100+.5f))+L"%"
            : L"Visual preservado selecionado — opções avançadas disponíveis";
        SetWindowTextW(GetDlgItem(window,Status),label.c_str());
        const bool collectionActive=selected();
        const auto style=a4::style();
        const bool curatedActive=style==a4::Style::curatedCaustics||style==a4::Style::curatedWaves;
        const bool music=collectionActive?chosen.music:curated::mode()==curated::Mode::music;
        const bool album=collectionActive?chosen.albumColors:curated::palette()==curated::Palette::artwork;
        for(const int id:{Original,Music,Album,Author,Quiet,Balanced}){
            const bool enabled=(id==Quiet||id==Balanced)?collectionActive:(collectionActive||curatedActive);
            const bool checked=id==Original?!music:id==Music?music:id==Album?album:id==Author?!album:
                id==Quiet?std::abs(chosen.intensity-.75f)<.001f:std::abs(chosen.intensity-1.f)<.001f;
            EnableWindow(GetDlgItem(window,id),enabled);
            SendMessageW(GetDlgItem(window,id),BM_SETCHECK,enabled&&checked?BST_CHECKED:BST_UNCHECKED,0);
        }
    }
    void notify(){status();if(IsWindow(owner))PostMessageW(owner,visualChangedMessage(),0,0);}
    int chosen()const noexcept {return list?static_cast<int>(SendMessageW(list,LB_GETCURSEL,0,0)):-1;}
    Entry* entry(){const int i=chosen();return i>=0&&static_cast<std::size_t>(i)<filtered.size()?&entries[filtered[i]]:nullptr;}
    void reload(){
        suppress=true;wchar_t q[256]{};GetWindowTextW(search,q,256);const auto query=lower(q);
        const auto groupIndex=static_cast<unsigned>(SendMessageW(group,CB_GETCURSEL,0,0));
        const auto selectedStyle=static_cast<unsigned>(a4::style());const auto selectedIndex=selection().preset;
        SendMessageW(list,WM_SETREDRAW,FALSE,0);SendMessageW(list,LB_RESETCONTENT,0,0);filtered.clear();
        int select=-1;
        for(unsigned i=0;i<entries.size();++i){auto&e=entries[i];
            if(groupIndex==1&&!favorite(e.id))continue;
            if(groupIndex>1&&groupIndex!=e.group)continue;
            if(!query.empty()&&lower(e.name).find(query)==std::wstring::npos)continue;
            auto line=(favorite(e.id)?L"★  ":L"    ")+e.name;
            SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()));filtered.push_back(i);
            const bool materialMatches=e.material==~0U ||
                e.material==static_cast<unsigned>(a4::materialStyle());
            if(selectedStyle==e.style&&materialMatches&&
                (e.style!=4||selectedIndex==e.index))select=static_cast<int>(filtered.size()-1);
        }
        if(select>=0)SendMessageW(list,LB_SETCURSEL,select,0);
        SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,TRUE);suppress=false;status();
    }
    void apply(){if(suppress)return;if(auto*e=entry()){
        if(e->style==4){handleMenu(47000U+e->index);}
        else {if(e->material!=~0U)(void)a4::setMaterialStyle(
                  static_cast<a4::MaterialStyle>(e->material));
              (void)a4::setStyle(static_cast<a4::Style>(e->style));}
        notify();}}
    HWND control(const wchar_t*cls,const wchar_t*text,DWORD style,int id){
        auto h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!h)throw std::bad_alloc();SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;
    }
    void resize(){
        RECT r{};GetClientRect(window,&r);const auto dpi=std::max<UINT>(96,GetDpiForWindow(window));
        auto px=[&](int v){return MulDiv(v,int(dpi),96);};int m=px(14),w=std::max(1,int(r.right)-2*m),h=int(r.bottom);
        MoveWindow(GetDlgItem(window,Status),m,m,w,px(22),TRUE);
        MoveWindow(search,m,m+px(24),w,px(29),TRUE);MoveWindow(group,m,m+px(61),w,px(220),TRUE);
        const int top=m+px(101),bottom=h-px(152);MoveWindow(list,m,top,w,std::max(px(80),bottom-top),TRUE);
        auto place=[&](int id,int col,int row,int cols){int gap=px(6),cw=(w-gap*(cols-1))/cols;MoveWindow(GetDlgItem(window,id),m+col*(cw+gap),bottom+px(9+row*34),cw,px(28),TRUE);};
        place(Fav,0,0,3);place(Original,1,0,3);place(Music,2,0,3);
        place(Album,0,1,2);place(Author,1,1,2);
        place(Quiet,0,2,3);place(Balanced,1,2,3);place(Retry,2,2,3);
        place(Advanced,0,3,1);
    }
};
inline LRESULT CALLBACK childProc(HWND w,UINT m,WPARAM a,LPARAM b){
    auto old=reinterpret_cast<WNDPROC>(GetPropW(w,L"C5.OldProc"));
    const int id=GetDlgCtrlID(w);
    if(m==WM_KEYDOWN&&(a==VK_ESCAPE||a==VK_TAB||(a==VK_RETURN&&(id==Search||id==List)))){
        PostMessageW(GetParent(w),WM_KEYDOWN,a,reinterpret_cast<LPARAM>(w));return 0;
    }
    if(m==WM_NCDESTROY)RemovePropW(w,L"C5.OldProc");
    return old?CallWindowProcW(old,w,m,a,b):DefWindowProcW(w,m,a,b);
}
inline LRESULT CALLBACK procedure(HWND w,UINT m,WPARAM a,LPARAM b) noexcept {
    auto*s=reinterpret_cast<State*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        if(!s)return FALSE;s->window=w;if(s->adopted){*s->adopted=true;s->adopted=nullptr;}
        SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));return TRUE;}
    try {
      if(!s)return DefWindowProcW(w,m,a,b);
      switch(m){
      case WM_CREATE:{
        const UINT dpi=std::max<UINT>(96,GetDpiForWindow(w));
        s->font=CreateFontW(-MulDiv(13,int(dpi),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        s->background=CreateSolidBrush(RGB(17,20,22));
        s->control(L"STATIC",L"",SS_ENDELLIPSIS,Status);
        s->search=s->control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,Search);
        SendMessageW(s->search,EM_SETLIMITTEXT,255,0);
        s->group=s->control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,Group);
        for(const auto*label:{L"Todos os efeitos",L"Favoritos",L"Frequência e células",L"Fluidos e ondas",L"Espaço e luz",L"Preservados"})
            SendMessageW(s->group,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(s->group,CB_SETCURSEL,0,0);
        s->list=s->control(L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,List);
        const auto add=[&](int id,const wchar_t*text){
            const bool choice=id==Original||id==Music||id==Album||id==Author||id==Quiet||id==Balanced;
            s->control(L"BUTTON",text,WS_TABSTOP|(choice?(BS_CHECKBOX|BS_PUSHLIKE):BS_PUSHBUTTON),id);};
        add(Fav,L"★ Favorito");add(Original,L"Original");add(Music,L"Music");
        add(Album,L"Cores do álbum");add(Author,L"Cores do autor");
        add(Quiet,L"Intensidade 75%");add(Balanced,L"Intensidade 100%");add(Retry,L"Reiniciar / Retry");add(Advanced,L"Opções avançadas e visuais preservados…");
        s->entries={{"flow31",L"Flow A3.1",5,0,0,~0U},
            {"alloy",L"Clovis · C12 refinado",5,0,1,2},
            {"caustics",L"Caustics",3,0,2,~0U},{"wavefield",L"Wave Field",3,0,3,~0U}};
        const auto& all=presets();
        const auto addPreset=[&](unsigned i){const auto&p=all[i];s->entries.push_back({p.id,p.title,groupFor(p.id),i,4,~0U});};
        for(unsigned i=0;i<all.size();++i){
            addPreset(i);
        }
        for(HWND h=s->search;h;h=GetWindow(h,GW_HWNDNEXT)){
            auto old=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(childProc)));
            SetPropW(h,L"C5.OldProc",reinterpret_cast<HANDLE>(old));
        }
        s->reload();s->resize();SetFocus(s->search);return 0;
      }
      case WM_SIZE:s->resize();return 0;
      case WM_DPICHANGED:{
        auto font=CreateFontW(-MulDiv(13,int(HIWORD(a)),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(font){for(HWND h=GetWindow(w,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT))SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            if(s->font)DeleteObject(s->font);s->font=font;}
        auto*r=reinterpret_cast<RECT*>(b);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOACTIVATE|SWP_NOZORDER);s->resize();return 0;}
      case WM_GETMINMAXINFO:{auto*p=reinterpret_cast<MINMAXINFO*>(b);const UINT dpi=std::max<UINT>(96,GetDpiForWindow(w));p->ptMinTrackSize={MulDiv(480,int(dpi),96),MulDiv(470,int(dpi),96)};return 0;}
      case WM_KEYDOWN:
        if(a==VK_ESCAPE){DestroyWindow(w);return 0;}
        if(a==VK_RETURN){if(GetFocus()==s->search&&s->chosen()<0&& !s->filtered.empty())SendMessageW(s->list,LB_SETCURSEL,0,0);s->apply();return 0;}
        if(a==VK_TAB){auto from=reinterpret_cast<HWND>(b);SetFocus(GetNextDlgTabItem(w,from,(GetKeyState(VK_SHIFT)&0x8000)!=0));return 0;}break;
      case WM_COMMAND:{
        const int id=LOWORD(a),code=HIWORD(a);
        if((id==Search&&code==EN_CHANGE)||(id==Group&&code==CBN_SELCHANGE)){s->reload();return 0;}
        if(id==List&&code==LBN_SELCHANGE){s->apply();return 0;}
        if(id==Fav){if(auto*e=s->entry())toggleFavorite(e->id);s->reload();return 0;}
        if(id==Advanced){POINT p{};GetCursorPos(&p);if(a4::showMenu(w,p)){s->notify();s->reload();}return 0;}
        if(id==Original||id==Music){if(selected())handleMenu(id==Original?48000:48001);else curated::setMode(id==Original?curated::Mode::original:curated::Mode::music);s->notify();return 0;}
        if(id==Album||id==Author){if(selected())handleMenu(id==Album?48006:48007);else curated::setPalette(id==Album?curated::Palette::artwork:curated::Palette::original);s->notify();return 0;}
        if(id==Quiet||id==Balanced){if(selected()){handleMenu(id==Quiet?48009:48008);s->notify();}return 0;}
        if(id==Retry){if(selected())handleMenu(48004);else curated::restart();s->notify();return 0;}
        break;
      }
      case WM_CTLCOLORLISTBOX:case WM_CTLCOLOREDIT:case WM_CTLCOLORSTATIC:{auto dc=reinterpret_cast<HDC>(a);SetTextColor(dc,RGB(214,221,226));SetBkColor(dc,RGB(17,20,22));return reinterpret_cast<LRESULT>(s->background);}
      case WM_CLOSE:DestroyWindow(w);return 0;
      case WM_NCDESTROY:if(openWindow==w)openWindow=nullptr;SetWindowLongPtrW(w,GWLP_USERDATA,0);delete s;return DefWindowProcW(w,m,a,b);
      }
    }catch(...){if(m==WM_CREATE)return -1;}
    return DefWindowProcW(w,m,a,b);
}
} // namespace picker_detail
inline bool showVisualPicker(HWND owner,POINT position) noexcept {
    using namespace picker_detail;
    if(IsWindow(openWindow)){ShowWindow(openWindow,SW_SHOWNORMAL);SetForegroundWindow(openWindow);return false;}
    if(!IsWindow(owner))return false;
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    wc.lpszClassName=L"Goden.C5.VisualPicker.V1";
    if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    auto*s=new(std::nothrow)State;if(!s)return false;bool adopted=false;s->adopted=&adopted;s->owner=owner;
    MONITORINFO mi{sizeof(mi)};
    if(!GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&mi)){
        mi.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    }
    int dpi=int(std::max<UINT>(96,GetDpiForWindow(owner))),width=MulDiv(530,dpi,96),height=MulDiv(630,dpi,96);
    width=std::min(width,int(mi.rcWork.right-mi.rcWork.left));height=std::min(height,int(mi.rcWork.bottom-mi.rcWork.top));
    const int x=std::clamp(int(position.x)-width,int(mi.rcWork.left),int(mi.rcWork.right)-width);
    const int y=std::clamp(int(position.y),int(mi.rcWork.top),int(mi.rcWork.bottom)-height);
    openWindow=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"GODENplayer · Visuais",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_VISIBLE,x,y,width,height,owner,nullptr,wc.hInstance,s);
    if(!openWindow&&!adopted)delete s;
    return false; // Opening the picker does not itself change a render preference.
}
} // namespace nekorox::component::collection
