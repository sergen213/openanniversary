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
        return 0; /* first sight of this region: let the dump through */
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

/* The motion channel (Milestone 5's validation against the original): the matrix setters are exactly the
 * methods mingw's d3d9 interface header omits around SetViewport, so the real slot is one of a small
 * window around the header's SetViewport index. Each window slot gets its own shape-correct probe that
 * logs the 16 floats and forwards unchanged - a mis-shaped slot at worst ignores or reads a garbage
 * pointer, which disturbs rendering only, never the game logic. The probe that fires every frame with
 * orthonormal rows names the view (or world-view-projection) matrix; the camera follows the player, so
 * its translation measures her motion in engine units against the guest clock in ms. */
typedef HRESULT (METHOD *Probe_t)(IDirect3DDevice9 *, const float *);
static Probe_t nxt_probe[5];
#define MKPROBE(k) static HRESULT METHOD probe##k(IDirect3DDevice9 *d, const float *m) \
{ \
    logline("probe%d t=%u m=%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g", \
            k, (unsigned)GetTickCount(), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], \
            m[12], m[13], m[14], m[15]); \
    return nxt_probe[k](d, m); \
}
MKPROBE(0) MKPROBE(1) MKPROBE(2) MKPROBE(3) MKPROBE(4)

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

/* The motion channel (Milestone 5's validation against the original): the third view-matrix row is the
 * camera's world position negated, and the camera follows the player, so consecutive lines measure her
 * movement in engine units. GetTickCount is the guest clock in ms since start - the engine's frame rate
 * is not known, so frames alone cannot give units per second. */
static unsigned logged_view;

/* ------------------------------------------- vertex/index buffer creation and filling
 * The terrain bytes are not available to a probe of our own: those buffers are created WRITEONLY|DYNAMIC (seen in the
 * create lines), so an outside read-only Lock on them fails, and the game re-locks them every frame without calling
 * Unlock. Watch the calls that create and fill them instead. Both resource interfaces carry the IDirect3DResource9
 * prefix (GetType, AddUpdateLock, ReleaseUpdateLock, GetDesc, Lock, Unlock) and DXVK keeps one vtable per class, so
 * patching the first object of a kind covers every later one; originals are saved once per kind. */

typedef HRESULT (METHOD *CreateVB_t)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT (METHOD *CreateIB_t)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
typedef HRESULT (METHOD *LockRes_t)(void *, unsigned, unsigned, void **, DWORD);
typedef HRESULT (METHOD *UnlockRes_t)(void *);

/* defined below, referenced by patchResourceVtbl */
static HRESULT METHOD LockVB_hook(void *, unsigned, unsigned, void **, DWORD);
static HRESULT METHOD LockIB_hook(void *, unsigned, unsigned, void **, DWORD);
static HRESULT METHOD UnlockVB_hook(void *);
static HRESULT METHOD UnlockIB_hook(void *);

static CreateVB_t nxt_CreateVB;
static CreateIB_t nxt_CreateIB;
static LockRes_t nxt_Lock_vb, nxt_Lock_ib;
static UnlockRes_t nxt_Unlock_vb, nxt_Unlock_ib;
static int vtbl_done[2];

#define MAX_LOCKS 256 /* ponytail: linear scan, one entry per live buffer; the engine keeps far fewer */
static struct { void *res; void *ptr; unsigned len; } locks[MAX_LOCKS];
static int nlocks;

static void remember(void *res, void *ptr, unsigned len)
{
    for (int i = 0; i < nlocks; ++i)
        if (locks[i].res == res)
        {
            locks[i].ptr = ptr;
            locks[i].len = len;
            return;
        }
    if (nlocks < MAX_LOCKS)
    {
        locks[nlocks].res = res;
        locks[nlocks].ptr = ptr;
        locks[nlocks].len = len;
        ++nlocks;
    }
}

static void patchResourceVtbl(void *obj, int isVertex)
{
    if (vtbl_done[isVertex])
        return;
    vtbl_done[isVertex] = 1;
    if (isVertex)
    {
        patch(obj, SLOT(IDirect3DVertexBuffer9, Lock), (void *)LockVB_hook, (void **)&nxt_Lock_vb);
        patch(obj, SLOT(IDirect3DVertexBuffer9, Unlock), (void *)UnlockVB_hook, (void **)&nxt_Unlock_vb);
    }
    else
    {
        patch(obj, SLOT(IDirect3DIndexBuffer9, Lock), (void *)LockIB_hook, (void **)&nxt_Lock_ib);
        patch(obj, SLOT(IDirect3DIndexBuffer9, Unlock), (void *)UnlockIB_hook, (void **)&nxt_Unlock_ib);
    }
}

static unsigned bufferBytes(void *res, int isVertex)
{
    if (isVertex)
    {
        D3DVERTEXBUFFER_DESC vd;
        return SUCCEEDED(IDirect3DVertexBuffer9_GetDesc((IDirect3DVertexBuffer9 *)res, &vd)) ? vd.Size : 0;
    }
    D3DINDEXBUFFER_DESC id2;
    return SUCCEEDED(IDirect3DIndexBuffer9_GetDesc((IDirect3DIndexBuffer9 *)res, &id2)) ? id2.Size : 0;
}

/* The engine re-locks these dynamic buffers every frame and never calls Unlock (measured: a run with 33,398 successful
 * locks logged none), so the previous lock's bytes are taken here - just before they are overwritten. Level geometry is
 * static between those re-uploads, which is what this capture needs; already() keeps one file per distinct region. */
static void savePrevious(void *res, int isVertex)
{
    for (int i = 0; i < nlocks; ++i)
        if (locks[i].res == res && locks[i].ptr)
        {
            unsigned bytes = locks[i].len ? locks[i].len : bufferBytes(res, isVertex);
            logline("save %s res=%p ptr=%p bytes=%u", isVertex ? "vb" : "ib", res, locks[i].ptr, bytes);
            dump(locks[i].ptr, bytes, isVertex ? "vb" : "ib", isVertex ? &nvb : &nib);
            return;
        }
}

static HRESULT METHOD lockRes(void *res, unsigned off, unsigned len, void **pp, DWORD flags, LockRes_t nxt, int isVertex)
{
    savePrevious(res, isVertex);
    HRESULT hr = nxt(res, off, len, pp, flags);
    logline("lock %s res=%p off=%u len=%u flags=%lx hr=%08lx ptr=%p", isVertex ? "vb" : "ib", res, off, len, flags, hr,
            (SUCCEEDED(hr) && pp) ? *pp : NULL);
    if (SUCCEEDED(hr) && pp && *pp)
        remember(res, *pp, len);
    return hr;
}

static HRESULT METHOD unlockRes(void *res, UnlockRes_t nxt, int isVertex)
{
    savePrevious(res, isVertex);
    return nxt(res);
}

static HRESULT METHOD LockVB_hook(void *r, unsigned o, unsigned l, void **pp, DWORD f) { return lockRes(r, o, l, pp, f, nxt_Lock_vb, 1); }
static HRESULT METHOD LockIB_hook(void *r, unsigned o, unsigned l, void **pp, DWORD f) { return lockRes(r, o, l, pp, f, nxt_Lock_ib, 0); }
static HRESULT METHOD UnlockVB_hook(void *r) { return unlockRes(r, nxt_Unlock_vb, 1); }
static HRESULT METHOD UnlockIB_hook(void *r) { return unlockRes(r, nxt_Unlock_ib, 0); }

static HRESULT METHOD CreateVB_hook(IDirect3DDevice9 *d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                    IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    HRESULT hr = nxt_CreateVB(d, len, usage, fvf, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out)
    {
        logline("create vb %p bytes=%u usage=%lx fvf=%lx pool=%d", (void *)*out, len, usage, fvf, pool);
        patchResourceVtbl(*out, 1);
    }
    return hr;
}

static HRESULT METHOD CreateIB_hook(IDirect3DDevice9 *d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                    IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    HRESULT hr = nxt_CreateIB(d, len, usage, fmt, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out)
    {
        logline("create ib %p bytes=%u usage=%lx fmt=0x%08x pool=%d", (void *)*out, len, usage, fmt, pool);
        patchResourceVtbl(*out, 0);
    }
    return hr;
}

static void patchDevice(IDirect3DDevice9 *dev)
{
    patch(dev, SLOT(IDirect3DDevice9, DrawIndexedPrimitive), DrawIP_hook, (void **)&nxt_DrawIP);
    patch(dev, SLOT(IDirect3DDevice9, SetTexture), SetTexture_hook, (void **)&nxt_SetTexture);
    patch(dev, SLOT(IDirect3DDevice9, CreateVertexBuffer), CreateVB_hook, (void **)&nxt_CreateVB);
    patch(dev, SLOT(IDirect3DDevice9, CreateIndexBuffer), CreateIB_hook, (void **)&nxt_CreateIB);
    const char *win = getenv("OA_PROBE_SLOT");
    if (win)
    {
        const int window = atoi(win);   // absolute vtbl slot, chosen by the probe runs outside this DLL
        patch(dev, window, probe0, (void **)&nxt_probe[0]);
    }
    else
    {
        logline("device %p patched: DrawIP=%d SetTexture=%d", dev,
                SLOT(IDirect3DDevice9, DrawIndexedPrimitive), SLOT(IDirect3DDevice9, SetTexture));
    }
    /* Only these two slots are trusted. Patching SLOT(CreateTexture)=23, SLOT(SetFVF)=89 and
     * SLOT(SetMaterial)=49 killed the game at its first Present with "err:seh:NtRaiseException Exception frame is
     * not in stack limits", and those hooks logged impossible arguments - a 6385652x17882624 texture during a
     * 1024x768 menu, FVF 2 - so whatever really sits at those offsets takes different argument counts, and
     * forwarding them with the wrong count unbalances the caller's stack. Slot 82 is confirmed by its own call
     * pattern: type=4 (TRIANGLELIST), starti stepping by 256 within one buffer pair, numv in the tens of
     * thousands for level meshes. */
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
