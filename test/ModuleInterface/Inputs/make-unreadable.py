
import platform
import subprocess
import sys

if platform.system() == 'Windows':
    import ctypes
    AdvAPI32 = ctypes.windll.Advapi32

    from ctypes import POINTER

    UNLEN = 256

    GetUserNameW = AdvAPI32.GetUserNameW
    GetUserNameW.argtypes = (
        ctypes.c_wchar_p,           # _In_Out_ lpBuffer
        POINTER(ctypes.c_uint)      # _In_out_ pcBuffer
    )
    GetUserNameW.restype = ctypes.c_uint

    buffer = ctypes.create_unicode_buffer(UNLEN + 1)
    size = ctypes.c_uint(len(buffer))
    GetUserNameW(buffer, ctypes.byref(size))
    # For NetworkService, Host$ is returned, so we choose have to turn it back
    # into something that icacls understands.
    if not buffer.value.endswith('$'):
        user_name = buffer.value
    else:
        user_name = 'NT AUTHORITY\\NetworkService'

    for path in sys.argv[1:]:
        subprocess.call(['icacls', path, '/deny',
                         '{}:(RD)'.format(user_name)])
else:
    for path in sys.argv[1:]:
        subprocess.call(['chmod', 'a-r', path])
