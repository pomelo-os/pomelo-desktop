#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <stdio.h>
#include <math.h>
int main(int argc,char**argv){
 Display*d=XOpenDisplay(NULL); int scr=DefaultScreen(d);
 FcPattern*p=FcNameParse((FcChar8*)"monospace:size=11"); FcPatternDel(p,FC_SIZE); FcPatternAddDouble(p,FC_SIZE,11);
 FcConfigSubstitute(NULL,p,FcMatchPattern); XftDefaultSubstitute(d,scr,p);
 double dpi=0, px=0; FcPatternGetDouble(p,FC_DPI,0,&dpi); FcPatternGetDouble(p,FC_PIXEL_SIZE,0,&px);
 FcResult r; FcPattern*m=FcFontMatch(NULL,p,&r); XftFont*f=XftFontOpenPattern(d,XftFontMatch(d,scr,p,&r)); 
 XGlyphInfo gi; XftTextExtentsUtf8(d,f,(FcChar8*)"M",1,&gi);
 printf("Xft: dpi=%.1f px=%.2f ascent=%d descent=%d height=%d adv=%d maxadv=%d\n",dpi,px,f->ascent,f->descent,f->height,gi.xOff,f->max_advance_width);
 FcChar8*fn; FcPatternGetString(m,FC_FILE,0,&fn); printf("file=%s\n",fn);
 FcPatternPrint(p);
 return 0;}
