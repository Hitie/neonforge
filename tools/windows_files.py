"""Checked local Windows filesystem primitives for owned developer stores."""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes as wt
import msvcrt
import os
from pathlib import Path


class FileError(RuntimeError):
    pass


_win = ctypes.WinDLL('kernel32', use_last_error=True)
_win.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
_win.CreateFileW.restype = wt.HANDLE
_win.CloseHandle.argtypes = [wt.HANDLE]
_win.CloseHandle.restype = wt.BOOL
_win.SetFileInformationByHandle.argtypes = [wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD]
_win.SetFileInformationByHandle.restype = wt.BOOL
_win.GetDriveTypeW.argtypes = [wt.LPCWSTR]
_win.GetDriveTypeW.restype = wt.UINT
_win.GetFinalPathNameByHandleW.argtypes = [wt.HANDLE, wt.LPWSTR, wt.DWORD, wt.DWORD]
_win.GetFinalPathNameByHandleW.restype = wt.DWORD
_win.GetFileType.argtypes = [wt.HANDLE]
_win.GetFileType.restype = wt.DWORD
_INVALID = ctypes.c_void_p(-1).value


class _Info(ctypes.Structure):
    _fields_ = [('attributes', wt.DWORD), ('created', wt.FILETIME), ('accessed', wt.FILETIME),
                ('modified', wt.FILETIME), ('volume', wt.DWORD), ('size_high', wt.DWORD),
                ('size_low', wt.DWORD), ('links', wt.DWORD), ('index_high', wt.DWORD), ('index_low', wt.DWORD)]


_win.GetFileInformationByHandle.argtypes = [wt.HANDLE, ctypes.POINTER(_Info)]
_win.GetFileInformationByHandle.restype = wt.BOOL


def absolute(path):
    path = Path(path)
    if '..' in path.parts:
        raise FileError('parent traversal is not accepted')
    # Win32 abspath can silently remove these spellings; inspect supplied parts first.
    for part in path.parts[(1 if path.anchor else 0):]:
        if part.endswith((' ', '.')) or ':' in part:
            raise FileError('ambiguous or alternate-stream raw path refused')
    path = Path(os.path.abspath(path))
    if len(path.drive) != 2 or path.drive[1] != ':' or len(str(path)) > 240:
        raise FileError('requires a local Windows path of at most 240 characters')
    if _win.GetDriveTypeW(path.anchor) != 3:
        raise FileError('requires a local fixed drive')
    reserved = {'CON', 'PRN', 'AUX', 'NUL', *('COM'+str(i) for i in range(1,10)), *('LPT'+str(i) for i in range(1,10))}
    for part in path.parts[1:]:
        if ':' in part or part.endswith((' ', '.')) or part.split('.')[0].upper() in reserved:
            raise FileError('device, alternate-stream or ambiguous path refused')
    return path


def _open(path, access, share, disposition, directory=False):
    flags = 0x00200000 | (0x02000000 if directory else 0x80)  # OPEN_REPARSE_POINT / BACKUP_SEMANTICS
    handle = _win.CreateFileW(str(path), access, share, None, disposition, flags, None)
    if handle == _INVALID:
        error = ctypes.get_last_error()
        if error in (2,3): raise FileNotFoundError(str(path))
        raise FileError('cannot open checked path (Windows error '+str(error)+'): '+str(path))
    try:
        info = _Info()
        if not _win.GetFileInformationByHandle(handle, ctypes.byref(info)):
            raise FileError('cannot inspect open file handle')
        if info.attributes & 0x400 or bool(info.attributes & 0x10) != directory:
            raise FileError('reparse or wrong file kind refused')
        if not directory and (_win.GetFileType(handle) != 1 or info.links != 1):
            raise FileError('requires a single-link regular disk file')
        return handle
    except BaseException:
        _win.CloseHandle(handle)
        raise


@contextmanager
def anchor(directory):
    """Pin every existing directory component against rename while in use."""
    directory = absolute(directory)
    handles = []
    try:
        current = Path(directory.anchor)
        handles.append(_open(current, 0x80, 3, 3, True))
        for part in directory.parts[1:]:
            current /= part
            handles.append(_open(current, 0x80, 3, 3, True))
        yield directory
    finally:
        for handle in reversed(handles): _win.CloseHandle(handle)


@contextmanager
def _stream(path, access, share, disposition, mode):
    handle = _open(path, access, share, disposition)
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_BINARY | (os.O_RDONLY if mode == 'rb' else os.O_RDWR))
    except BaseException:
        _win.CloseHandle(handle)
        raise
    with os.fdopen(descriptor, mode, buffering=0) as stream:
        yield stream


def read(path, limit):
    path = absolute(path)
    try:
        with anchor(path.parent), _stream(path, 0x80000000, 1, 3, 'rb') as stream:
            data = stream.read(limit + 1)
            if len(data) > limit: raise FileError('file byte limit exceeded')
            return data
    except FileNotFoundError:
        raise
    except OSError as error:
        raise FileError(str(error)) from error


@contextmanager
def exclusive(path):
    path = absolute(path)
    with anchor(path.parent):
        with _stream(path, 0xc0000000, 0, 4, 'r+b'):
            yield


def regular_or_missing(path):
    try:
        handle = _open(path, 0, 7, 3)
    except FileNotFoundError:
        return False
    _win.CloseHandle(handle)
    return True


class _Pinned:
    def __init__(self, path, handle):
        self.path, self.handle, self.published = path, handle, False


class _Rename(ctypes.Structure):
    _fields_ = [('replace', wt.BOOLEAN), ('root', wt.HANDLE),
                ('length', wt.DWORD), ('name', wt.WCHAR * 1)]


def _discard(handle):
    flag = wt.BOOLEAN(True)
    if not _win.SetFileInformationByHandle(handle, 4, ctypes.byref(flag), ctypes.sizeof(flag)):
        raise FileError('cannot discard the verified staging handle')


@contextmanager
def pinned_directory(path):
    handle = _open(path, 0x10080, 3, 3, True)  # DELETE plus attributes
    try: yield _Pinned(path, handle)
    finally: _win.CloseHandle(handle)


@contextmanager
def staged(path, data, fault=lambda stage: None, prefix='write'):
    with _stream(path, 0xc0010000, 0, 1, 'r+b') as stream:
        pinned = _Pinned(path, msvcrt.get_osfhandle(stream.fileno()))
        try:
            middle = len(data) // 2
            for index, chunk in enumerate((data[:middle], data[middle:])):
                while chunk:
                    written = stream.write(chunk)
                    if not written: raise FileError('short store write')
                    chunk = chunk[written:]
                if index == 0: fault(prefix + ':half')
            os.fsync(stream.fileno())
            fault(prefix + ':flushed')
            yield pinned
        finally:
            if not pinned.published: _discard(pinned.handle)


def create(path, data, fault=lambda stage: None, prefix='write'):
    """Exclusive new file, checked writes and flush; caller holds its parent anchor."""
    try:
        with staged(path, data, fault, prefix) as pinned:
            pinned.published = True
    except OSError as error:
        raise FileError(str(error)) from error


def move(source, target, replace=True):
    if not isinstance(source, _Pinned): raise FileError('publication requires a pinned source handle')
    if replace: regular_or_missing(target)
    encoded = str(target).encode('utf-16-le')
    length = _Rename.name.offset + len(encoded) + 2
    buffer = ctypes.create_string_buffer(max(length, ctypes.sizeof(_Rename)))
    info = ctypes.cast(buffer, ctypes.POINTER(_Rename)).contents
    info.replace, info.root, info.length = replace, None, len(encoded)
    ctypes.memmove(ctypes.addressof(buffer) + _Rename.name.offset, encoded, len(encoded))
    if not _win.SetFileInformationByHandle(source.handle, 3, buffer, len(buffer)):
        raise FileError('cannot publish store file (Windows error '+str(ctypes.get_last_error())+')')
    source.published = True


def remove_reserved(path):
    try:
        with _stream(path, 0x80010000, 0, 3, 'rb') as stream:
            _discard(msvcrt.get_osfhandle(stream.fileno()))
    except FileNotFoundError:
        pass


def canonical_directory(path):
    path = absolute(path)
    with anchor(path):
        handle = _open(path, 0x80, 7, 3, True)
        try:
            buffer = ctypes.create_unicode_buffer(32768)
            length = _win.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 1)
            if not length or length >= len(buffer): raise FileError('cannot resolve volume-normalized directory identity')
            return buffer.value.rstrip('\\').casefold()
        finally: _win.CloseHandle(handle)


def is_within(source, root):
    source, root = absolute(source), absolute(root)
    parent = canonical_directory(source.parent)
    actual_root = canonical_directory(root) if root.exists() else canonical_directory(root.parent) + '\\' + root.name.casefold()
    return parent == actual_root or parent.startswith(actual_root + '\\')
