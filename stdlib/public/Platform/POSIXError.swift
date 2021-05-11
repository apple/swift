//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if (os(macOS) || os(iOS) || os(tvOS) || os(watchOS)) && _runtime(_ObjC)

/// Enumeration describing POSIX error codes.
@objc
public enum POSIXErrorCode : Int32 {
  /// Operation not permitted.
  case EPERM           = 1
  /// No such file or directory.
  case ENOENT          = 2
  /// No such process.
  case ESRCH           = 3
  /// Interrupted system call.
  case EINTR           = 4
  /// Input/output error.
  case EIO             = 5
  /// Device not configured.
  case ENXIO           = 6
  /// Argument list too long.
  case E2BIG           = 7
  /// Exec format error.
  case ENOEXEC         = 8
  /// Bad file descriptor.
  case EBADF           = 9
  /// No child processes.
  case ECHILD          = 10
  /// Resource deadlock avoided.
  case EDEADLK         = 11
  /// 11 was EAGAIN.
  /// Cannot allocate memory.
  case ENOMEM          = 12
  /// Permission denied.
  case EACCES          = 13
  /// Bad address.
  case EFAULT          = 14
  /// Block device required.
  case ENOTBLK         = 15
  /// Device / Resource busy.
  case EBUSY           = 16
  /// File exists.
  case EEXIST          = 17
  /// Cross-device link.
  case EXDEV           = 18
  /// Operation not supported by device.
  case ENODEV          = 19
  /// Not a directory.
  case ENOTDIR         = 20
  /// Is a directory.
  case EISDIR          = 21
  /// Invalid argument.
  case EINVAL          = 22
  /// Too many open files in system.
  case ENFILE          = 23
  /// Too many open files.
  case EMFILE          = 24
  /// Inappropriate ioctl for device.
  case ENOTTY          = 25
  /// Text file busy.
  case ETXTBSY         = 26
  /// File too large.
  case EFBIG           = 27
  /// No space left on device.
  case ENOSPC          = 28
  /// Illegal seek.
  case ESPIPE          = 29
  /// Read-only file system.
  case EROFS           = 30
  /// Too many links.
  case EMLINK          = 31
  /// Broken pipe.
  case EPIPE           = 32
    
  /// math software.
  /// Numerical argument out of domain.
  case EDOM            = 33
  /// Result too large.
  case ERANGE          = 34
    
  /// non-blocking and interrupt i/o.
  /// Resource temporarily unavailable.
  case EAGAIN          = 35
  /// Operation would block.
  public static var EWOULDBLOCK: POSIXErrorCode { return EAGAIN }
  /// Operation now in progress.
  case EINPROGRESS     = 36
  /// Operation already in progress.
  case EALREADY        = 37
    
  /// ipc/network software -- argument errors.
  /// Socket operation on non-socket.
  case ENOTSOCK        = 38
  /// Destination address required.
  case EDESTADDRREQ    = 39
  /// Message too long.
  case EMSGSIZE        = 40
  /// Protocol wrong type for socket.
  case EPROTOTYPE      = 41
  /// Protocol not available.
  case ENOPROTOOPT     = 42
  /// Protocol not supported.
  case EPROTONOSUPPORT = 43
  /// Socket type not supported.
  case ESOCKTNOSUPPORT = 44
  /// Operation not supported.
  case ENOTSUP         = 45
  /// Protocol family not supported.
  case EPFNOSUPPORT    = 46
  /// Address family not supported by protocol family.
  case EAFNOSUPPORT    = 47
  /// Address already in use.
  case EADDRINUSE      = 48
  /// Can't assign requested address.
  case EADDRNOTAVAIL   = 49
    
  /// ipc/network software -- operational errors
  /// Network is down.
  case ENETDOWN        = 50
  /// Network is unreachable.
  case ENETUNREACH     = 51
  /// Network dropped connection on reset.
  case ENETRESET       = 52
  /// Software caused connection abort.
  case ECONNABORTED    = 53
  /// Connection reset by peer.
  case ECONNRESET      = 54
  /// No buffer space available.
  case ENOBUFS         = 55
  /// Socket is already connected.
  case EISCONN         = 56
  /// Socket is not connected.
  case ENOTCONN        = 57
  /// Can't send after socket shutdown.
  case ESHUTDOWN       = 58
  /// Too many references: can't splice.
  case ETOOMANYREFS    = 59
  /// Operation timed out.
  case ETIMEDOUT       = 60
  /// Connection refused.
  case ECONNREFUSED    = 61
    
  /// Too many levels of symbolic links.
  case ELOOP           = 62
  /// File name too long.
  case ENAMETOOLONG    = 63
    
  /// Host is down.
  case EHOSTDOWN       = 64
  /// No route to host.
  case EHOSTUNREACH    = 65
  /// Directory not empty.
  case ENOTEMPTY       = 66
    
  /// quotas & mush.
  /// Too many processes.
  case EPROCLIM        = 67
  /// Too many users.
  case EUSERS          = 68
  /// Disc quota exceeded.
  case EDQUOT          = 69
    
  /// Network File System.
  /// Stale NFS file handle.
  case ESTALE          = 70
  /// Too many levels of remote in path.
  case EREMOTE         = 71
  /// RPC struct is bad.
  case EBADRPC         = 72
  /// RPC version wrong.
  case ERPCMISMATCH    = 73
  /// RPC prog. not avail.
  case EPROGUNAVAIL    = 74
  /// Program version wrong.
  case EPROGMISMATCH   = 75
  /// Bad procedure for program.
  case EPROCUNAVAIL    = 76
    
  /// No locks available.
  case ENOLCK          = 77
  /// Function not implemented.
  case ENOSYS          = 78
    
  /// Inappropriate file type or format.
  case EFTYPE          = 79
  /// Authentication error.
  case EAUTH           = 80
  /// Need authenticator.
  case ENEEDAUTH       = 81
    
  /// Intelligent device errors.
  /// Device power is off.
  case EPWROFF         = 82
  /// Device error, e.g. paper out.
  case EDEVERR         = 83
    
  /// Value too large to be stored in data type.
  case EOVERFLOW       = 84

  // MARK: Program loading errors.

  /// Bad executable.
  case EBADEXEC        = 85
  /// Bad CPU type in executable.
  case EBADARCH        = 86
  /// Shared library version mismatch.
  case ESHLIBVERS      = 87
  /// Malformed Macho file.
  case EBADMACHO       = 88
    
  /// Operation canceled.
  case ECANCELED       = 89
    
  /// Identifier removed.
  case EIDRM           = 90
  /// No message of desired type.
  case ENOMSG          = 91
  /// Illegal byte sequence.
  case EILSEQ          = 92
  /// Attribute not found.
  case ENOATTR         = 93
    
  /// Bad message.
  case EBADMSG         = 94
  /// Reserved.
  case EMULTIHOP       = 95
  /// No message available on STREAM.
  case ENODATA         = 96
  /// Reserved.
  case ENOLINK         = 97
  /// No STREAM resources.
  case ENOSR           = 98
  /// Not a STREAM.
  case ENOSTR          = 99
  /// Protocol error.
  case EPROTO          = 100
  /// STREAM ioctl timeout.
  case ETIME           = 101
    
  /// No such policy registered.
  case ENOPOLICY       = 103
    
  /// State not recoverable.
  case ENOTRECOVERABLE = 104
  /// Previous owner died.
  case EOWNERDEAD      = 105
  
  /// Interface output queue is full.
  case EQFULL          = 106
  /// Must be equal largest errno.
  public static var ELAST: POSIXErrorCode { return EQFULL }
    
  // FIXME: EOPNOTSUPP has different values depending on __DARWIN_UNIX03 and
  // KERNEL.
}

#elseif os(Linux) || os(Android)

/// Enumeration describing POSIX error codes.
public enum POSIXErrorCode : Int32 {
  /// Operation not permitted.
  case EPERM           = 1
  /// No such file or directory.
  case ENOENT          = 2
  /// No such process.
  case ESRCH           = 3
  /// Interrupted system call.
  case EINTR           = 4
  /// Input/output error.
  case EIO             = 5
  /// Device not configured.
  case ENXIO           = 6
  /// Argument list too long.
  case E2BIG           = 7
  /// Exec format error.
  case ENOEXEC         = 8
  /// Bad file descriptor.
  case EBADF           = 9
  /// No child processes.
  case ECHILD          = 10
  /// Try again.
  case EAGAIN         = 11
  /// Cannot allocate memory.
  case ENOMEM          = 12
  /// Permission denied.
  case EACCES          = 13
  /// Bad address.
  case EFAULT          = 14
  /// Block device required.
  case ENOTBLK         = 15
  /// Device / Resource busy.
  case EBUSY           = 16
  /// File exists.
  case EEXIST          = 17
  /// Cross-device link.
  case EXDEV           = 18
  /// Operation not supported by device.
  case ENODEV          = 19
  /// Not a directory.
  case ENOTDIR         = 20
  /// Is a directory.
  case EISDIR          = 21
  /// Invalid argument.
  case EINVAL          = 22
  /// Too many open files in system.
  case ENFILE          = 23
  /// Too many open files.
  case EMFILE          = 24
  /// Inappropriate ioctl for device.
  case ENOTTY          = 25
  /// Text file busy.
  case ETXTBSY         = 26
  /// File too large.
  case EFBIG           = 27
  /// No space left on device.
  case ENOSPC          = 28
  /// Illegal seek.
  case ESPIPE          = 29
  /// Read-only file system.
  case EROFS           = 30
  /// Too many links.
  case EMLINK          = 31
  /// Broken pipe.
  case EPIPE           = 32

  /// Numerical argument out of domain.
  case EDOM            = 33
  /// Result too large.
  case ERANGE          = 34
    
  /// Resource deadlock would occur.
  case EDEADLK          = 35
    
  /// File name too long.
  case ENAMETOOLONG     = 36
    
  /// No record locks available
  case ENOLCK           = 37
    
  /// Function not implemented.
  case ENOSYS           = 38
    
  /// Directory not empty.
  case ENOTEMPTY        = 39
    
  /// Too many symbolic links encountered
  case ELOOP            = 40
    
  /// Operation would block.
  public static var EWOULDBLOCK: POSIXErrorCode { return .EAGAIN }

  /// No message of desired type.
  case ENOMSG          = 42
    
  /// Identifier removed.
  case EIDRM           = 43
    
  /// Channel number out of range.
  case ECHRNG          = 44
    
  /// Level 2 not synchronized.
  case EL2NSYNC        = 45
    
  /// Level 3 halted
  case EL3HLT          = 46
    
  /// Level 3 reset.
  case EL3RST          = 47
    
  /// Link number out of range.
  case ELNRNG          = 48
    
  /// Protocol driver not attached.
  case EUNATCH         = 49
    
  /// No CSI structure available.
  case ENOCSI          = 50
    
  /// Level 2 halted.
  case EL2HLT          = 51
  /// Invalid exchange
  case EBADE           = 52
  /// Invalid request descriptor
  case EBADR           = 53
  /// Exchange full
  case EXFULL          = 54
  /// No anode
  case ENOANO          = 55
  /// Invalid request code
  case EBADRQC         = 56
  /// Invalid slot
  case EBADSLT         = 57

  public static var EDEADLOCK: POSIXErrorCode { return .EDEADLK }

  /// Bad font file format
  case EBFONT          = 59
  /// Device not a stream
  case ENOSTR          = 60
  /// No data available
  case ENODATA         = 61
  /// Timer expired
  case ETIME           = 62
  /// Out of streams resources
  case ENOSR           = 63
  /// Machine is not on the network
  case ENONET          = 64
  /// Package not installed
  case ENOPKG          = 65
  /// Object is remote
  case EREMOTE         = 66
  /// Link has been severed
  case ENOLINK         = 67
  /// Advertise error
  case EADV            = 68
  /// Srmount error
  case ESRMNT          = 69
  /// Communication error on send
  case ECOMM           = 70
  /// Protocol error
  case EPROTO          = 71
  /// Multihop attempted
  case EMULTIHOP       = 72
  /// RFS specific error
  case EDOTDOT         = 73
  /// Not a data message
  case EBADMSG         = 74
  /// Value too large for defined data type
  case EOVERFLOW       = 75
  /// Name not unique on network
  case ENOTUNIQ        = 76
  /// File descriptor in bad state
  case EBADFD          = 77
  /// Remote address changed
  case EREMCHG         = 78
  /// Can not access a needed shared library
  case ELIBACC         = 79
  /// Accessing a corrupted shared library
  case ELIBBAD         = 80
  /// .lib section in a.out corrupted
  case ELIBSCN         = 81
  /// Attempting to link in too many shared libraries
  case ELIBMAX         = 82
  /// Cannot exec a shared library directly
  case ELIBEXEC        = 83
  /// Illegal byte sequence
  case EILSEQ          = 84
  /// Interrupted system call should be restarted
  case ERESTART        = 85
  /// Streams pipe error
  case ESTRPIPE        = 86
  /// Too many users
  case EUSERS          = 87
  /// Socket operation on non-socket
  case ENOTSOCK        = 88
  /// Destination address required
  case EDESTADDRREQ    = 89
  /// Message too long
  case EMSGSIZE        = 90
  /// Protocol wrong type for socket
  case EPROTOTYPE      = 91
  /// Protocol not available
  case ENOPROTOOPT     = 92
  /// Protocol not supported
  case EPROTONOSUPPORT = 93
  /// Socket type not supported
  case ESOCKTNOSUPPORT = 94
  /// Operation not supported on transport endpoint
  case EOPNOTSUPP      = 95
  /// Protocol family not supported
  case EPFNOSUPPORT    = 96
  /// Address family not supported by protocol
  case EAFNOSUPPORT    = 97
  /// Address already in use
  case EADDRINUSE      = 98
  /// Cannot assign requested address
  case EADDRNOTAVAIL   = 99
  /// Network is down
  case ENETDOWN        = 100
  /// Network is unreachable
  case ENETUNREACH     = 101
  /// Network dropped connection because of reset
  case ENETRESET       = 102
  /// Software caused connection abort
  case ECONNABORTED    = 103
  /// Connection reset by peer
  case ECONNRESET      = 104
  /// No buffer space available
  case ENOBUFS         = 105
  /// Transport endpoint is already connected
  case EISCONN         = 106
  /// Transport endpoint is not connected
  case ENOTCONN        = 107
  /// Cannot send after transport endpoint shutdown
  case ESHUTDOWN       = 108
  /// Too many references: cannot splice
  case ETOOMANYREFS    = 109
  /// Connection timed out
  case ETIMEDOUT       = 110
  /// Connection refused
  case ECONNREFUSED    = 111
  /// Host is down
  case EHOSTDOWN       = 112
  /// No route to host
  case EHOSTUNREACH    = 113
  /// Operation already in progress
  case EALREADY        = 114
  /// Operation now in progress
  case EINPROGRESS     = 115
  /// Stale NFS file handle
  case ESTALE          = 116
  /// Structure needs cleaning
  case EUCLEAN         = 117
  /// Not a XENIX named type file
  case ENOTNAM         = 118
  /// No XENIX semaphores available
  case ENAVAIL         = 119
  /// Is a named type file
  case EISNAM          = 120
  /// Remote I/O error
  case EREMOTEIO       = 121
  /// Quota exceeded
  case EDQUOT          = 122

  /// No medium found
  case ENOMEDIUM       = 123
  /// Wrong medium type
  case EMEDIUMTYPE     = 124
  /// Operation Canceled
  case ECANCELED       = 125
  /// Required key not available
  case ENOKEY          = 126
  /// Key has expired
  case EKEYEXPIRED     = 127
  /// Key has been revoked
  case EKEYREVOKED     = 128
  /// Key was rejected by service
  case EKEYREJECTED    = 129
  
  /// Owner died
  case EOWNERDEAD      = 130
  /// State not recoverable
  case ENOTRECOVERABLE = 131
  /// Operation not possible due to RF-kill
  case ERFKILL         = 132
  /// Memory page has hardware error
  case EHWPOISON       = 133
}

#elseif os(WASI)

/// Enumeration describing POSIX error codes.
public enum POSIXErrorCode : Int32 {
  /// Argument list too long.
  case E2BIG = 1
  /// Permission denied.
  case EACCES = 2
  /// Address in use.
  case EADDRINUSE = 3
  /// Address not available.
  case EADDRNOTAVAIL = 4
  /// Address family not supported.
  case EAFNOSUPPORT = 5
  /// Resource unavailable, or operation would block.
  case EAGAIN = 6
  /// Operation would block.
  public static var EWOULDBLOCK: POSIXErrorCode { return .EAGAIN }
  /// Connection already in progress.
  case EALREADY = 7
  /// Bad file descriptor.
  case EBADF = 8
  /// Bad message.
  case EBADMSG = 9
  /// Device or resource busy.
  case EBUSY = 10
  /// Operation canceled.
  case ECANCELED = 11
  /// No child processes.
  case ECHILD = 12
  /// Connection aborted.
  case ECONNABORTED = 13
  /// Connection refused.
  case ECONNREFUSED = 14
  /// Connection reset.
  case ECONNRESET = 15
  /// Resource deadlock would occur.
  case EDEADLK = 16
  /// Destination address required.
  case EDESTADDRREQ = 17
  /// Mathematics argument out of domain of function.
  case EDOM = 18
  /// Reserved.
  case EDQUOT = 19
  /// File exists.
  case EEXIST = 20
  /// Bad address.
  case EFAULT = 21
  /// File too large.
  case EFBIG = 22
  /// Host is unreachable.
  case EHOSTUNREACH = 23
  /// Identifier removed.
  case EIDRM = 24
  /// Illegal byte sequence.
  case EILSEQ = 25
  /// Operation in progress.
  case EINPROGRESS = 26
  /// Interrupted function.
  case EINTR = 27
  /// Invalid argument.
  case EINVAL = 28
  /// I/O error.
  case EIO = 29
  /// Socket is connected.
  case EISCONN = 30
  /// Is a directory.
  case EISDIR = 31
  /// Too many levels of symbolic links.
  case ELOOP = 32
  /// File descriptor value too large.
  case EMFILE = 33
  /// Too many links.
  case EMLINK = 34
  /// Message too large.
  case EMSGSIZE = 35
  /// Reserved.
  case EMULTIHOP = 36
  /// Filename too long.
  case ENAMETOOLONG = 37
  /// Network is down.
  case ENETDOWN = 38
  /// Connection aborted by network.
  case ENETRESET = 39
  /// Network unreachable.
  case ENETUNREACH = 40
  /// Too many files open in system.
  case ENFILE = 41
  /// No buffer space available.
  case ENOBUFS = 42
  /// No such device.
  case ENODEV = 43
  /// No such file or directory.
  case ENOENT = 44
  /// Executable file format error.
  case ENOEXEC = 45
  /// No locks available.
  case ENOLCK = 46
  /// Reserved.
  case ENOLINK = 47
  /// Not enough space.
  case ENOMEM = 48
  /// No message of the desired type.
  case ENOMSG = 49
  /// Protocol not available.
  case ENOPROTOOPT = 50
  /// No space left on device.
  case ENOSPC = 51
  /// Function not supported.
  case ENOSYS = 52
  /// The socket is not connected.
  case ENOTCONN = 53
  /// Not a directory or a symbolic link to a directory.
  case ENOTDIR = 54
  /// Directory not empty.
  case ENOTEMPTY = 55
  /// State not recoverable.
  case ENOTRECOVERABLE = 56
  /// Not a socket.
  case ENOTSOCK = 57
  /// Not supported, or operation not supported on socket.
  case ENOTSUP = 58
  /// Operation not supported on transport endpoint
  public static var EOPNOTSUPP: POSIXErrorCode { return .ENOTSUP }
  /// Inappropriate I/O control operation.
  case ENOTTY = 59
  /// No such device or address.
  case ENXIO = 60
  /// Value too large to be stored in data type.
  case EOVERFLOW = 61
  /// Previous owner died.
  case EOWNERDEAD = 62
  /// Operation not permitted.
  case EPERM = 63
  /// Broken pipe.
  case EPIPE = 64
  /// Protocol error.
  case EPROTO = 65
  /// Protocol not supported.
  case EPROTONOSUPPORT = 66
  /// Protocol wrong type for socket.
  case EPROTOTYPE = 67
  /// Result too large.
  case ERANGE = 68
  /// Read-only file system.
  case EROFS = 69
  /// Invalid seek.
  case ESPIPE = 70
  /// No such process.
  case ESRCH = 71
  /// Reserved.
  case ESTALE = 72
  /// Connection timed out.
  case ETIMEDOUT = 73
  /// Text file busy.
  case ETXTBSY = 74
  /// Cross-device link.
  case EXDEV = 75
  /// Extension: Capabilities insufficient.
  case ENOTCAPABLE = 76
}

#elseif os(Windows)

/// Enumeration describing POSIX error codes.
public enum POSIXErrorCode : Int32 {
    
    /// Operation not permitted
    case EPERM          = 1
    
    /// No such file or directory
    case ENOENT         = 2
    
    /// No such process
    case ESRCH          = 3
    
    /// Interrupted function
    case EINTR          = 4
    
    /// I/O error
    case EIO            = 5
    
    /// No such device or address
    case ENXIO          = 6
    
    /// Argument list too long
    case E2BIG          = 7
    
    /// Exec format error
    case ENOEXEC        = 8
    
    /// Bad file number
    case EBADF          = 9
    
    /// No spawned processes
    case ECHILD         = 10
    
    /// No more processes or not enough memory or maximum nesting level reached
    case EAGAIN         = 11
    
    /// Not enough memory
    case ENOMEM         = 12
    
    /// Permission denied
    case EACCES         = 13
    
    /// Bad address
    case EFAULT         = 14
    
    /// Device or resource busy
    case EBUSY          = 16
    
    /// File exists
    case EEXIST         = 17
    
    /// Cross-device link
    case EXDEV          = 18
    
    /// No such device
    case ENODEV         = 19
    
    /// Not a directory
    case ENOTDIR        = 20
    
    /// Is a directory
    case EISDIR         = 21
    
    /// Invalid argument
    case EINVAL         = 22
    
    /// Too many files open in system
    case ENFILE         = 23
    
    /// Too many open files
    case EMFILE         = 24
    
    /// Inappropriate I/O control operation
    case ENOTTY         = 25
    
    /// File too large
    case EFBIG          = 27
    
    /// No space left on device
    case ENOSPC         = 28
    
    /// Invalid seek
    case ESPIPE         = 29
    
    /// Read-only file system
    case EROFS          = 30
    
    /// Too many links
    case EMLINK         = 31
    
    /// Broken pipe
    case EPIPE          = 32
    
    /// Math argument
    case EDOM           = 33
    
    /// Result too large
    case ERANGE         = 34
    
    /// Resource deadlock would occur
    case EDEADLK        = 36
    
    /// Same as EDEADLK for compatibility with older Microsoft C versions
    public static var EDEADLOCK: POSIXErrorCode { return .EDEADLK }
    
    /// Filename too long
    case ENAMETOOLONG   = 38
    
    /// No locks available
    case ENOLCK         = 39
    
    /// Function not supported
    case ENOSYS         = 40
    
    /// Directory not empty
    case ENOTEMPTY      = 41
    
    /// Illegal byte sequence
    case EILSEQ         = 42
    
    /// String was truncated
    case STRUNCATE      = 80
}

#endif
