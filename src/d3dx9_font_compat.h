#ifndef REFRAIN_D3DX9_FONT_COMPAT_H
#define REFRAIN_D3DX9_FONT_COMPAT_H

#include <stddef.h>

typedef struct ID3DXFont ID3DXFont;
typedef ID3DXFont *LPD3DXFONT;
typedef struct ID3DXSprite ID3DXSprite;

/* Layout of ID3DXFont in d3dx9core.h.  The slot order must match exactly:
   the slots that are never called are only placeholders to keep DrawTextA at
   index 14.  Missing or reordered slots make DrawTextA call a different method
   with the wrong argument count and corrupt the stack. */
typedef struct ID3DXFontVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3DXFont *, REFIID, void **);   /*  0 */
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3DXFont *);                              /*  1 */
    ULONG (STDMETHODCALLTYPE *Release)(ID3DXFont *);                             /*  2 */
    void (STDMETHODCALLTYPE *GetDevice)(void);                                   /*  3 */
    void (STDMETHODCALLTYPE *GetDescA)(void);                                    /*  4 */
    void (STDMETHODCALLTYPE *GetDescW)(void);                                    /*  5 */
    void (STDMETHODCALLTYPE *GetTextMetricsA)(void);                             /*  6 */
    void (STDMETHODCALLTYPE *GetTextMetricsW)(void);                             /*  7 */
    void (STDMETHODCALLTYPE *GetDC)(void);                                       /*  8 */
    void (STDMETHODCALLTYPE *GetGlyphData)(void);                                /*  9 */
    void (STDMETHODCALLTYPE *PreloadCharacters)(void);                           /* 10 */
    void (STDMETHODCALLTYPE *PreloadGlyphs)(void);                               /* 11 */
    void (STDMETHODCALLTYPE *PreloadTextA)(void);                                /* 12 */
    void (STDMETHODCALLTYPE *PreloadTextW)(void);                                /* 13 */
    INT (STDMETHODCALLTYPE *DrawTextA)(ID3DXFont *, ID3DXSprite *, LPCSTR,
                                       INT, LPRECT, DWORD, D3DCOLOR);            /* 14 */
    void (STDMETHODCALLTYPE *DrawTextW)(void);                                   /* 15 */
    void (STDMETHODCALLTYPE *OnLostDevice)(void);                                /* 16 */
    void (STDMETHODCALLTYPE *OnResetDevice)(void);                               /* 17 */
} ID3DXFontVtbl;

struct ID3DXFont {
    const ID3DXFontVtbl *lpVtbl;
};

/* Compile-time guard: breaks the build if DrawTextA is not at slot 14. */
typedef char d3dx9_font_vtbl_layout_check[
    (offsetof(ID3DXFontVtbl, DrawTextA) == 14 * sizeof(void *)) ? 1 : -1];

#endif
