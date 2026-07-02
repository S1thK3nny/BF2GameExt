"""Dump vtable slot order for a class from a PDB, via the DIA SDK (msdia140.dll).

Usage:  py -3 dia_vtable.py <ClassName> [<ClassName> ...]

No registry changes: activates msdia140 in-proc through DllGetClassObject.
NOTE: the PDB is from a *different* build than the modtools binary, so always
cross-check the printed order against the actual binary vtable.
"""
import sys, ctypes, glob, struct
from ctypes import POINTER, byref, c_void_p, c_int, HRESULT
import comtypes
from comtypes import GUID, IUnknown, COMMETHOD
import comtypes.client

PDB = r"E:\Battlefront 2 uncompiled\Battlefront2.pdb"

def pe_machine(path):
    with open(path, 'rb') as f:
        f.seek(0x3c); e = int.from_bytes(f.read(4), 'little')
        f.seek(e + 4); return int.from_bytes(f.read(2), 'little')

def find_msdia():
    want = 0x8664 if struct.calcsize('P') == 8 else 0x14c  # AMD64 vs I386
    hits = glob.glob(r"C:\Program Files*\Microsoft Visual Studio\**\msdia140.dll", recursive=True)
    for h in hits:
        try:
            if pe_machine(h) == want:
                return h
        except Exception:
            pass
    raise SystemExit(f"no msdia140.dll matching python bitness ({want:#x})")

MSDIA = find_msdia()
dia = comtypes.client.GetModule(MSDIA)   # generate IDiaDataSource etc. from embedded TLB

CLSID_DiaSource = GUID('{E6756135-1E65-4D17-8576-610761398C3C}')

class IClassFactory(IUnknown):
    _iid_ = GUID('{00000001-0000-0000-C000-000000000046}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'CreateInstance',
                  (['in'], POINTER(IUnknown), 'pUnkOuter'),
                  (['in'], POINTER(GUID), 'riid'),
                  (['out'], POINTER(c_void_p), 'ppv')),
        COMMETHOD([], HRESULT, 'LockServer', (['in'], c_int, 'fLock')),
    ]

def make_datasource():
    dll = ctypes.WinDLL(MSDIA)
    fn = dll.DllGetClassObject
    fn.argtypes = [POINTER(GUID), POINTER(GUID), POINTER(c_void_p)]
    fn.restype = ctypes.c_long
    pcf = c_void_p()
    hr = fn(byref(CLSID_DiaSource), byref(IClassFactory._iid_), byref(pcf))
    if hr != 0:
        raise OSError(f"DllGetClassObject failed: 0x{hr & 0xffffffff:08x}")
    factory = ctypes.cast(pcf, POINTER(IClassFactory))
    pv = factory.CreateInstance(None, byref(dia.IDiaDataSource._iid_))
    return ctypes.cast(pv, POINTER(dia.IDiaDataSource))

SymTagUDT = 11
SymTagFunction = 5
nsfCaseSensitive = 0x1

def enum(sym, tag, name=None):
    e = sym.findChildren(tag, name, nsfCaseSensitive if name else 0)
    out = []
    for i in range(e.count):
        out.append(e.Item(i))
    return out

def dump_class(g, classname):
    udts = enum(g, SymTagUDT, classname)
    if not udts:
        print(f"\n## {classname}: NOT FOUND in PDB")
        return
    u = udts[0]
    funcs = enum(u, SymTagFunction)
    virt = []
    for f in funcs:
        try:
            if f.virtual:
                virt.append((f.virtualBaseOffset, f.name))
        except Exception:
            pass
    virt.sort(key=lambda t: t[0])
    print(f"\n## {classname}  (len={getattr(u,'length',0)} bytes, {len(virt)} own virtual methods)")
    print("slot  byteoff  name")
    for off, name in virt:
        print(f"{off//4:>4}  {off:>7}  {name}")

def main():
    ds = make_datasource()
    ds.loadDataFromPdb(PDB)
    session = ds.openSession()
    g = session.globalScope
    names = sys.argv[1:] or ["EntityEx"]
    for n in names:
        dump_class(g, n)

if __name__ == "__main__":
    main()
