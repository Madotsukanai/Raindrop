#ifndef REFRAIN_D3DX9_FONT_COMPAT_H
#define REFRAIN_D3DX9_FONT_COMPAT_H

typedef struct ID3DXFont ID3DXFont;
typedef ID3DXFont *LPD3DXFONT;
typedef struct ID3DXSprite ID3DXSprite;

typedef struct ID3DXFontVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3DXFont *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3DXFont *);
    ULONG (STDMETHODCALLTYPE *Release)(ID3DXFont *);
    void (STDMETHODCALLTYPE *GetDevice)(void);
    void (STDMETHODCALLTYPE *GetDescA)(void);
    void (STDMETHODCALLTYPE *GetTextMetricsA)(void);
    void (STDMETHODCALLTYPE *GetDC)(void);
    void (STDMETHODCALLTYPE *GetGlyphData)(void);
    void (STDMETHODCALLTYPE *PreloadCharacters)(void);
    void (STDMETHODCALLTYPE *PreloadGlyphs)(void);
    void (STDMETHODCALLTYPE *PreloadTextA)(void);
    void (STDMETHODCALLTYPE *PreloadTextW)(void);
    INT (STDMETHODCALLTYPE *DrawTextA)(ID3DXFont *, ID3DXSprite *, LPCSTR,
                                       INT, LPRECT, DWORD, D3DCOLOR);
} ID3DXFontVtbl;

struct ID3DXFont {
    const ID3DXFontVtbl *lpVtbl;
};

#endif
