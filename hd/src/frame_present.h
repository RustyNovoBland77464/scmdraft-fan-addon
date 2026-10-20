#pragma once
#include <windows.h>
#include <algorithm>

// Compose terrain and overlays into a private bitmap. The window receives only the
// completed frame, never the intermediate map-only image that erases the mining UI.
class FramePresenter {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    int width_ = 0, height_ = 0;
    bool ensure(int width,int height) {
        if (dc_ && width<=width_ && height<=height_) return true;
        width=std::max(width,width_); height=std::max(height,height_);
        BITMAPINFO info{};
        info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
        void *bits=nullptr;
        HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!bitmap) return false;
        HDC dc=CreateCompatibleDC(nullptr);
        if (!dc) {DeleteObject(bitmap);return false;}
        reset();
        dc_=dc; bitmap_=bitmap; previous_=SelectObject(dc_,bitmap_); width_=width; height_=height;
        return true;
    }
    void reset() {
        if (dc_) {SelectObject(dc_,previous_);DeleteObject(bitmap_);DeleteDC(dc_);}
        dc_=nullptr;bitmap_=nullptr;previous_=nullptr;width_=height_=0;
    }
public:
    ~FramePresenter() {reset();}
    FramePresenter() = default;
    FramePresenter(const FramePresenter &) = delete;
    FramePresenter &operator=(const FramePresenter &) = delete;
    template<class Draw> bool present(HDC target,RECT bounds,Draw draw) {
        RECT clip{},visible{};
        int type=GetClipBox(target,&clip);
        if (type==ERROR) return false;
        if (type==NULLREGION || !IntersectRect(&visible,&bounds,&clip)) return true;
        int w=visible.right-visible.left,h=visible.bottom-visible.top;
        if (!ensure(w,h)) return false;
        int saved=SaveDC(dc_);
        if (!saved) return false;
        SetViewportOrgEx(dc_,-visible.left,-visible.top,nullptr);
        IntersectClipRect(dc_,visible.left,visible.top,visible.right,visible.bottom);
        bool ok=draw(dc_);
        RestoreDC(dc_,saved);
        return ok && BitBlt(target,visible.left,visible.top,w,h,dc_,0,0,SRCCOPY);
    }
};
