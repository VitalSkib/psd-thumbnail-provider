import ctypes as c
import struct
import sys
import tempfile
import uuid
from pathlib import Path
from ctypes import wintypes as w
P=c.c_void_p
H=c.c_long
def guid(x): return (c.c_ubyte*16).from_buffer_copy(uuid.UUID(x).bytes_le)
def method(obj,idx,ret,*args):
    table=c.cast(obj,c.POINTER(c.POINTER(P))).contents
    return c.WINFUNCTYPE(ret,P,*args)(table[idx])
def check(hr):
    if hr < 0: raise OSError(hex(hr & 0xffffffff))
def release(obj): method(obj,2,w.ULONG)(obj)
dll=c.WinDLL(str(Path(sys.argv[1]).resolve()))
dll.DllGetClassObject.argtypes=[P,P,c.POINTER(P)]
def create(clsid,iid):
    factory=P(); obj=P()
    check(dll.DllGetClassObject(guid(clsid),guid('00000001-0000-0000-c000-000000000046'),c.byref(factory)))
    check(method(factory,3,H,P,P,c.POINTER(P))(factory,None,guid(iid),c.byref(obj)))
    release(factory)
    return obj
sh=c.WinDLL('shlwapi')
sh.SHCreateStreamOnFileEx.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,w.BOOL,P,c.POINTER(P)]
gdi=c.WinDLL('gdi32')
gdi.GetObjectW.argtypes=[P,c.c_int,P]
gdi.DeleteObject.argtypes=[P]
class Bitmap(c.Structure):
    _fields_=[('type',w.LONG),('width',w.LONG),('height',w.LONG),('row',w.LONG),('planes',w.WORD),('bpp',w.WORD),('bits',P)]

def thumbnail(path, requested):
    stream=P(); init=P(); bmp=P(); alpha=w.DWORD()
    obj=create('5bb45d32-af01-414f-b60a-5e999b986681','e357fccd-a995-4576-b01f-234630154e96')
    check(sh.SHCreateStreamOnFileEx(str(path),0,128,False,None,c.byref(stream)))
    check(method(obj,0,H,P,c.POINTER(P))(obj,guid('b824b49d-22ac-4161-ac8a-9916e8fa3f7f'),c.byref(init)))
    check(method(init,3,H,P,w.DWORD)(init,stream,0))
    check(method(obj,3,H,w.UINT,c.POINTER(P),c.POINTER(w.DWORD))(obj,requested,c.byref(bmp),c.byref(alpha)))
    info=Bitmap()
    assert gdi.GetObjectW(bmp,c.sizeof(info),c.byref(info))
    pixels=c.string_at(info.bits,info.width*4) if info.bits else b''
    assert gdi.DeleteObject(bmp)
    for v in (init,obj,stream): release(v)
    return (info.width,info.height),pixels

with tempfile.TemporaryDirectory() as workspace:
    for version, ext in [(1,'psd'),(2,'psb')]:
        sample=Path(workspace) / f'linear-test.{ext}'
        values=(0.5, 2.0, 4.0)
        header=struct.pack('>4sH6xHIIHHII',b'8BPS',version,3,1,3,32,3,0,0)
        sample.write_bytes(header+b'\0'*(6 if version==1 else 10)+struct.pack('>fff',*values)*3)
        size,pixels=thumbnail(sample,1024)
        expected=[int((value/(1+value))**(1/2.2)*255+0.5) for value in values]
        assert size==(3,1) and list(pixels[::4])==expected, (size,pixels,expected)
        print(ext, 'Reinhard',list(pixels[::4]))
