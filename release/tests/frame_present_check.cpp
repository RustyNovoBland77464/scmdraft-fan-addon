// Asset-free GDI regression: compile with MinGW -static -lgdi32, run on Windows/Wine.
#include "../../hd/src/frame_present.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::fprintf(out,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    FILE *out=std::fopen(argv[1],"w"); if(!out) return 2;
    HDC target=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateBitmap(320,240,1,32,nullptr);
    HGDIOBJ old=SelectObject(target,bitmap);
    FramePresenter frame;
    const COLORREF before=RGB(255,0,0),map=RGB(0,255,0),menu=RGB(0,0,255);
    HBRUSH red=CreateSolidBrush(before),green=CreateSolidBrush(map),blue=CreateSolidBrush(menu);
    RECT all{0,0,320,240};FillRect(target,&all,red);
    CHECK(frame.present(target,all,[&](HDC back) {
        FillRect(back,&all,green);
        if(GetPixel(target,30,30)!=before) return false; // no intermediate map-only frame reached the target
        RECT panel{20,20,100,60};FillRect(back,&panel,blue);
        return true;
    }));
    CHECK(GetPixel(target,30,30)==menu && GetPixel(target,110,30)==map);
    // A partial repaint preserves screen coordinates and leaves other pixels intact.
    IntersectClipRect(target,40,35,80,55);
    CHECK(frame.present(target,{-20,-10,640,480},[&](HDC back) {
        if(GetPixel(target,50,40)!=menu) return false;
        FillRect(back,&all,green);
        RECT panel{50,40,70,50};FillRect(back,&panel,blue);
        return true;
    }));
    CHECK(GetPixel(target,45,40)==map && GetPixel(target,55,45)==menu);
    SelectClipRgn(target,nullptr);
    CHECK(GetPixel(target,30,30)==menu && GetPixel(target,90,40)==menu);
    // A failed composition must not publish its partly drawn background.
    CHECK(!frame.present(target,all,[&](HDC back){FillRect(back,&all,red);return false;}));
    CHECK(GetPixel(target,30,30)==menu);
    // Verify that a map-sized rectangle does not overwrite the surrounding area.
    CHECK(frame.present(target,{100,100,300,230},[&](HDC back){FillRect(back,&all,blue);return true;}));
    CHECK(GetPixel(target,250,200)==menu && GetPixel(target,10,200)==map);
    DeleteObject(red);DeleteObject(green);DeleteObject(blue);
    SelectObject(target,old);DeleteObject(bitmap);DeleteDC(target);
    std::fputs("PASS: complete-frame presentation, partial repaint coordinates, clipping and failed-draw isolation.\n",out);std::fclose(out);
}
