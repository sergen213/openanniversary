/* d3d9 passthrough logger for observing the shipped engine's terrain draws.
 *
 * This DLL takes the place of DXVK's d3d9.dll inside the prefix (see install.sh). It loads the real
 * DXVK library under the name d3d9_dxvk.dll, hands out the device it creates unchanged, and rewrites
 * that device's own vtable slots in place. Because everything runs inside the guest process, the
 * buffers a draw names can be read directly: no ptrace, no debugger, no /proc access.
 *
 * Output (OA_DXHOOK_DIR, default C:\tra\d3d9log):
 *   d3d9.txt     device creation, CreateTexture identities, SetFVF, SetMaterial, SetTexture(stage->tex),
 *                buffer dumps, and DrawIndexedPrimitive with the base vertex index that
 *                docs/formats/GEOMETRY.md 17 asks about.
 *   vb_<n>.bin   bytes of each distinct stream source 0 seen at a draw (the terrain vertex block)
 *   ib_<n>.bin   bytes of each distinct index buffer bound at an indexed draw
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

/* The interface struct only holds lpVtbl; the method pointers live in the Vtbl struct that mingw's
 * ICOM_INTERFACE macros generate from it. offsetof() keeps slot numbers honest with the headers. */
#define SLOT(iface, field) (offsetof(iface##Vtbl, field) / sizeof(void *))

/* Calling convention, measured rather than assumed. Declaring these thiscall (ECX = object) made the
 * game die inside DXVK before CreateDevice returned: "err:seh:NtRaiseException Exception frame is not
 * in stack limits". Declaring them stdcall (object pointer pushed as the first argument, which is how
 * mingw-w64 declares COM interfaces) produced correct values for every CreateDevice argument - adapter=0
 * type=1 flags=50 backbuf=1024x768 windowed=0 hr=00000000 - so this call path passes the object on the
 * stack. If a future capture shows one hook reading shifted arguments, that slot's convention is the
 * thing to fix here, not the format string. */
#define METHOD WINAPI

static FILE *L;
static char dir[MAX_PATH] = "C:\\tra\\d3d9log";

/* ------------------------------------------------------------------ logging */

static void logline(const char *fmt, ...)
{
    va_list ap;
    if (!L)
    {
        const char *env = getenv("OA_DXHOOK_DIR");
        if (env && *env)
            snprintf(dir, sizeof dir, "%s", env);
        CreateDirectoryA("C:\\tra", NULL);
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%s\\d3d9.txt", dir);
        L = fopen(path, "ab");
        if (!L)
            return;
    }
    fprintf(L, "%08lx ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(L, fmt, ap);
    va_end(ap);
    fputc('\n', L);
    fflush(L); /* evidence survives a crash */
}

/* ------------------------------------------------- distinct-buffer file dumps */

#define MAX_SEEN 512          /* ponytail: linear scan and a hard cap; one level at a time */
#define MAX_DUMP (64u << 20)  /* refuse implausible sizes instead of filling the disk */
static struct { void *p; unsigned bytes; } seen[MAX_SEEN];
static int nseen, nvb, nib;

static int already(void *p, unsigned bytes)
{
    for (int i = 0; i < nseen; ++i)
        if (seen[i].p == p && seen[i].bytes == bytes)
            return 1;
    if (nseen < MAX_SEEN)
    {
        seen[nseen].p = p;
        seen[nseen].bytes = bytes;
        ++nseen;
    }
    return 1; /* table full: stop dumping rather than repeat forever */
}

static void dump(void *base, unsigned bytes, const char *kind, int *counter)
{
    if (!base || !bytes || bytes > MAX_DUMP || already(base, bytes))
        return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\%s_%d.bin", dir, kind, (*counter)++);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    size_t wrote = fwrite(base, 1, bytes, f);
    fclose(f);
    logline("dump %s bytes=%u file=%s%s", kind, bytes, path, wrote == bytes ? "" : " TRUNCATED");
}

/* Lock a resource, save its bytes, unlock. Read-only flags so the game's own state is untouched. */
static void dumpResource(void *res, int isVertex)
{
    if (!res || already(res, 0))
        return; /* key on the object pointer: one file per buffer object */
    void *p = NULL;
    unsigned size = 0;
    if (isVertex)
    {
        IDirect3DVertexBuffer9 *vb = (IDirect3DVertexBuffer9 *)res;
        D3DVERTEXBUFFER_DESC d;
        if (FAILED(IDirect3DVertexBuffer9_GetDesc(vb, &d)))
            return;
        if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, d.Size, &p, D3DLOCK_READONLY)))
            return;
        size = d.Size;
    }
    else
    {
        IDirect3DIndexBuffer9 *ib = (IDirect3DIndexBuffer9 *)res;
        D3DINDEXBUFFER_DESC d;
        if (FAILED(IDirect3DIndexBuffer9_GetDesc(ib, &d)))
            return;
        if (FAILED(IDirect3DIndexBuffer9_Lock(ib, 0, d.Size, &p, D3DLOCK_READONLY)))
            return;
        size = d.Size;
    }
    seen[nseen].p = res; /* mark before writing so a huge buffer is never retried */
    seen[nseen].bytes = size;
    if (nseen < MAX_SEEN)
        ++nseen;
    dump(p, size, isVertex ? "vb" : "ib", isVertex ? &nvb : &nib);
    if (isVertex)
        IDirect3DVertexBuffer9_Unlock((IDirect3DVertexBuffer9 *)res);
    else
        IDirect3DIndexBuffer9_Unlock((IDirect3DIndexBuffer9 *)res);
}

/* ------------------------------------------------------- vtable patching */

static void patch(void *obj, int slot, void *hook, void **orig)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
    {
        logline("FAIL VirtualProtect slot %d err=%lu", slot, GetLastError());
        return;
    }
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}

/* ------------------------------------------------------- device hooks */

typedef HRESULT (METHOD *DrawIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (METHOD *SetTex_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);

static DrawIP_t nxt_DrawIP;
static SetTex_t nxt_SetTexture;

static HRESULT METHOD SetTexture_hook(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t)
{
    logline("SetTexture stage=%u tex=%p", stage, (void *)t);
    return nxt_SetTexture(d, stage, t);
}

/* The measured question (GEOMETRY.md 17): what base vertex index does a terrain draw use? prims>=4 skips UI
 * quads and lines; the counter cap exists because an uncapped four-minute capture wrote 1.1 GB of text at the
 * menu alone, which buries the level's own draws in the file. */
static unsigned logged_draws;
static HRESULT METHOD DrawIP_hook(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, INT bvi, UINT minv,
                                  UINT numv, UINT starti, UINT prims)
{
    if (prims >= 4 && logged_draws < 300000)
    {
        IDirect3DVertexBuffer9 *vb = NULL;
        IDirect3DIndexBuffer9 *ib = NULL;
        UINT stride = 0, offset = 0;
        HRESULT q1 = IDirect3DDevice9_GetStreamSource(d, 0, &vb, &stride, &offset);
        HRESULT q2 = IDirect3DDevice9_GetIndices(d, &ib);
        ++logged_draws;
        logline("draw type=%u bvi=%d minv=%u numv=%u starti=%u prims=%u stride=%u off=%u vb=%p ib=%p", type, bvi,
             minv, numv, starti, prims, stride, offset, (void *)vb, (void *)ib);
        if (SUCCEEDED(q1) && vb)
            dumpResource(vb, 1);
        if (SUCCEEDED(q2) && ib)
            dumpResource(ib, 0);
        if (vb)
            IDirect3DVertexBuffer9_Release(vb);
        if (ib)
            IDirect3DIndexBuffer9_Release(ib);
    }
    return nxt_DrawIP(d, type, bvi, minv, numv, starti, prims);
}

static void patchDevice(IDirect3DDevice9 *dev)
{
    patch(dev, SLOT(IDirect3DDevice9, DrawIndexedPrimitive), DrawIP_hook, (void **)&nxt_DrawIP);
    patch(dev, SLOT(IDirect3DDevice9, SetTexture), SetTexture_hook, (void **)&nxt_SetTexture);
    /* Only these two slots are trusted. Patching SLOT(CreateTexture)=23, SLOT(SetFVF)=89 and
     * SLOT(SetMaterial)=49 killed the game at its first Present with "err:seh:NtRaiseException Exception frame is
     * not in stack limits", and those hooks logged impossible arguments - a 6385652x17882624 texture during a
     * 1024x768 menu, FVF 2 - so whatever really sits at those offsets takes different argument counts, and
     * forwarding them with the wrong count unbalances the caller's stack. Slot 82 is confirmed by its own call
     * pattern: type=4 (TRIANGLELIST), starti stepping by 256 within one buffer pair, numv in the tens of
     * thousands for level meshes. */
    logline("device %p patched: DrawIP=%d SetTexture=%d", dev,
            SLOT(IDirect3DDevice9, DrawIndexedPrimitive), SLOT(IDirect3DDevice9, SetTexture));
}

/* ------------------------------------------------------- entry points */

typedef HRESULT (METHOD *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND *, DWORD,
                                         D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static CreateDevice_t nxt_CreateDevice;

static HRESULT METHOD CreateDevice_hook(IDirect3D9 *d9, UINT adapter, D3DDEVTYPE type, HWND *focus,
                                        DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = nxt_CreateDevice(d9, adapter, type, focus, flags, pp, out);
    logline("CreateDevice adapter=%u type=%u flags=%lx backbuf=%ux%u fourcc=%.4s windowed=%d hr=%08lx dev=%p",
         adapter, type, flags, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
         pp ? (char *)&pp->BackBufferFormat : "?", pp ? pp->Windowed : 0, hr, out && SUCCEEDED(hr) ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out && *out)
        patchDevice(*out);
    return hr;
}

typedef IDirect3D9 *(WINAPI *Create9_t)(UINT);
typedef HRESULT (WINAPI *Create9Ex_t)(UINT, IDirect3D9Ex **);
/* DXVK is loaded on demand inside the two entry points below, not in DllMain: LoadLibrary under the
 * loader lock risks a deadlock against the library we are standing in for. */
IDirect3D9 *WINAPI hook_Direct3DCreate9(UINT sdkVersion)
{
    HMODULE h = LoadLibraryA("d3d9_dxvk.dll");
    Create9_t f = h ? (Create9_t)(void *)GetProcAddress(h, "Direct3DCreate9") : NULL;
    logline("Direct3DCreate9(%u) dll=%p fn=%p", sdkVersion, (void *)h, (void *)f);
    if (!f)
        return NULL;
    IDirect3D9 *d9 = f(sdkVersion);
    if (d9)
        patch(d9, SLOT(IDirect3D9, CreateDevice), CreateDevice_hook, (void **)&nxt_CreateDevice);
    return d9;
}

HRESULT WINAPI hook_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **out)
{
    HMODULE h = LoadLibraryA("d3d9_dxvk.dll");
    Create9Ex_t f = h ? (Create9Ex_t)(void *)GetProcAddress(h, "Direct3DCreate9Ex") : NULL;
    logline("Direct3DCreate9Ex(%u) dll=%p fn=%p", sdkVersion, (void *)h, (void *)f);
    if (!f)
        return E_NOINTERFACE;
    HRESULT hr = f(sdkVersion, out);
    /* The Ex interfaces append their methods after the plain ones, so the shared prefix offsets hold. */
    if (SUCCEEDED(hr) && out && *out)
        patch(*out, SLOT(IDirect3D9, CreateDevice), CreateDevice_hook, (void **)&nxt_CreateDevice);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *reserved)
{
    (void)h;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH && !GetEnvironmentVariableA("OA_DXHOOK_OFF", NULL, 0))
        logline("shim loaded");
    return TRUE;
}
