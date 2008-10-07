/*
 * Copyright 2008 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "mshtml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

typedef struct {
    HTMLElement element;
    const IHTMLFrameBase2Vtbl  *lpIHTMLFrameBase2Vtbl;

    LONG ref;

    nsIDOMHTMLIFrameElement *nsiframe;
} HTMLIFrame;

#define HTMLFRAMEBASE2(x)  ((IHTMLFrameBase2*)  &(x)->lpIHTMLFrameBase2Vtbl)

#define HTMLFRAMEBASE2_THIS(iface) DEFINE_THIS(HTMLIFrame, IHTMLFrameBase2, iface)

static HRESULT WINAPI HTMLIFrameBase2_QueryInterface(IHTMLFrameBase2 *iface, REFIID riid, void **ppv)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);

    return IHTMLDOMNode_QueryInterface(HTMLDOMNODE(&This->element.node), riid, ppv);
}

static ULONG WINAPI HTMLIFrameBase2_AddRef(IHTMLFrameBase2 *iface)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);

    return IHTMLDOMNode_AddRef(HTMLDOMNODE(&This->element.node));
}

static ULONG WINAPI HTMLIFrameBase2_Release(IHTMLFrameBase2 *iface)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);

    return IHTMLDOMNode_Release(HTMLDOMNODE(&This->element.node));
}

static HRESULT WINAPI HTMLIFrameBase2_GetTypeInfoCount(IHTMLFrameBase2 *iface, UINT *pctinfo)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_GetTypeInfo(IHTMLFrameBase2 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_GetIDsOfNames(IHTMLFrameBase2 *iface, REFIID riid,
        LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_Invoke(IHTMLFrameBase2 *iface, DISPID dispIdMember,
        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_get_contentWindow(IHTMLFrameBase2 *iface, IHTMLWindow2 **p)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_put_onload(IHTMLFrameBase2 *iface, VARIANT v)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_variant(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_get_onload(IHTMLFrameBase2 *iface, VARIANT *p)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_put_onreadystatechange(IHTMLFrameBase2 *iface, VARIANT v)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_variant(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_get_onreadystatechange(IHTMLFrameBase2 *iface, VARIANT *p)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_get_readyState(IHTMLFrameBase2 *iface, BSTR *p)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_put_allowTransparency(IHTMLFrameBase2 *iface, VARIANT_BOOL v)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%x)\n", This, v);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLIFrameBase2_get_allowTransparency(IHTMLFrameBase2 *iface, VARIANT_BOOL *p)
{
    HTMLIFrame *This = HTMLFRAMEBASE2_THIS(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

#undef HTMLFRAMEBASE2_THIS

static const IHTMLFrameBase2Vtbl HTMLIFrameBase2Vtbl = {
    HTMLIFrameBase2_QueryInterface,
    HTMLIFrameBase2_AddRef,
    HTMLIFrameBase2_Release,
    HTMLIFrameBase2_GetTypeInfoCount,
    HTMLIFrameBase2_GetTypeInfo,
    HTMLIFrameBase2_GetIDsOfNames,
    HTMLIFrameBase2_Invoke,
    HTMLIFrameBase2_get_contentWindow,
    HTMLIFrameBase2_put_onload,
    HTMLIFrameBase2_get_onload,
    HTMLIFrameBase2_put_onreadystatechange,
    HTMLIFrameBase2_get_onreadystatechange,
    HTMLIFrameBase2_get_readyState,
    HTMLIFrameBase2_put_allowTransparency,
    HTMLIFrameBase2_get_allowTransparency
};

#define HTMLIFRAME_NODE_THIS(iface) DEFINE_THIS2(HTMLIFrame, element.node, iface)

static HRESULT HTMLIFrame_QI(HTMLDOMNode *iface, REFIID riid, void **ppv)
{
    HTMLIFrame *This = HTMLIFRAME_NODE_THIS(iface);

    *ppv = NULL;

    if(IsEqualGUID(&IID_IHTMLFrameBase2, riid)) {
        TRACE("(%p)->(IID_IHTMLFrameBase2 %p)\n", This, ppv);
        *ppv = HTMLFRAMEBASE2(This);
    }else {
        return HTMLElement_QI(&This->element.node, riid, ppv);
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static void HTMLIFrame_destructor(HTMLDOMNode *iface)
{
    HTMLIFrame *This = HTMLIFRAME_NODE_THIS(iface);

    if(This->nsiframe)
        nsIDOMHTMLIFrameElement_Release(This->nsiframe);

    HTMLElement_destructor(&This->element.node);
}

#undef HTMLIFRAME_NODE_THIS

static const NodeImplVtbl HTMLIFrameImplVtbl = {
    HTMLIFrame_QI,
    HTMLIFrame_destructor
};

HTMLElement *HTMLIFrame_Create(nsIDOMHTMLElement *nselem)
{
    HTMLIFrame *ret;
    nsresult nsres;

    ret = heap_alloc_zero(sizeof(HTMLIFrame));

    ret->lpIHTMLFrameBase2Vtbl = &HTMLIFrameBase2Vtbl;
    ret->element.node.vtbl = &HTMLIFrameImplVtbl;

    HTMLElement_Init(&ret->element);

    nsres = nsIDOMHTMLElement_QueryInterface(nselem, &IID_nsIDOMHTMLIFrameElement, (void**)&ret->nsiframe);
    if(NS_FAILED(nsres))
        ERR("Could not get nsIDOMHTMLIFrameElement iface: %08x\n", nsres);

    return &ret->element;
}
