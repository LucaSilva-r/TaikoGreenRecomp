/*
 * ps3recomp - sceNpCommerce HLE implementation
 *
 * Offline stub. Store requests return NOT_CONNECTED; games typically handle
 * that gracefully with "store unavailable" UI.
 *
 * The context/session lifecycle (Init, Term, CreateCtx, DestroyCtx,
 * GetCategoryContentsStart, DoCheckoutFinishAsync) lives in sceNpCommerce2.c,
 * which implements it against the real request objects. Both files used to
 * define those six -- a multiple-definition link error on any linker that
 * doesn't silently pick the first.
 */

#include "sceNpCommerce.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/






s32 sceNpCommerce2GetCategoryInfo(SceNpCommerce2Ctx ctx,
                                    SceNpCommerce2CategoryInfo* info)
{
    (void)ctx; (void)info;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2GetProductInfo(SceNpCommerce2Ctx ctx, u32 index,
                                   SceNpCommerce2ProductInfo* info)
{
    (void)ctx; (void)index; (void)info;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2InitStoreRequest(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpCommerce2DoCheckoutStartAsync(SceNpCommerce2Ctx ctx,
                                         const char* productId)
{
    (void)ctx; (void)productId;
    printf("[sceNpCommerce] DoCheckoutStartAsync()\n");
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}


s32 sceNpCommerce2GetResult(SceNpCommerce2Ctx ctx, s32* result)
{
    (void)ctx;
    if (result) *result = (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
    return CELL_OK;
}

s32 sceNpCommerce2DoEntitlementListStartAsync(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return (s32)SCE_NP_COMMERCE2_ERROR_NOT_CONNECTED;
}

s32 sceNpCommerce2DoEntitlementListFinishAsync(SceNpCommerce2Ctx ctx)
{
    (void)ctx;
    return CELL_OK;
}
