// RUN: %target-run-simple-swift
// REQUIRES: executable_test
//
// UNSUPPORTED: OS=macosx
// UNSUPPORTED: OS=ios
// UNSUPPORTED: OS=tvos
// UNSUPPORTED: OS=watchos
// UNSUPPORTED: OS=linux-androideabi

// REQUIRES: OS=linux-gnu

import Swift
import StdlibUnittest


import Glibc

var POSIXErrorCodeLinuxTestSuite = TestSuite("POSIXErrorCodeLinux")

POSIXErrorCodeLinuxTestSuite.test("POSIX error codes constants") {
    
    expectEqual(POSIXErrorCode.EPERM.rawValue == EPERM)
    expectEqual(POSIXErrorCode.ENOENT.rawValue == ENOENT)
    expectEqual(POSIXErrorCode.ESRCH.rawValue == ESRCH)
    expectEqual(POSIXErrorCode.EINTR.rawValue == EINTR)
    expectEqual(POSIXErrorCode.EIO.rawValue == EIO)
    expectEqual(POSIXErrorCode.ENXIO.rawValue == ENXIO)
    expectEqual(POSIXErrorCode.E2BIG.rawValue == E2BIG)
    expectEqual(POSIXErrorCode.ENOEXEC.rawValue == ENOEXEC)
    expectEqual(POSIXErrorCode.EBADF.rawValue == EBADF)
    expectEqual(POSIXErrorCode.ECHILD.rawValue == ECHILD)
    expectEqual(POSIXErrorCode.EAGAIN.rawValue == EAGAIN)
    expectEqual(POSIXErrorCode.ENOMEM.rawValue == ENOMEM)
    expectEqual(POSIXErrorCode.EACCES.rawValue == EACCES)
    expectEqual(POSIXErrorCode.EFAULT.rawValue == EFAULT)
    expectEqual(POSIXErrorCode.ENOTBLK.rawValue == ENOTBLK)
    expectEqual(POSIXErrorCode.EBUSY.rawValue == EBUSY)
    expectEqual(POSIXErrorCode.EXDEV.rawValue == EXDEV)
    expectEqual(POSIXErrorCode.ENODEV.rawValue == ENODEV)
    expectEqual(POSIXErrorCode.ENOTDIR.rawValue == ENOTDIR)
    expectEqual(POSIXErrorCode.EISDIR.rawValue == EISDIR)
    expectEqual(POSIXErrorCode.EINVAL.rawValue == EINVAL)
    expectEqual(POSIXErrorCode.ENFILE.rawValue == ENFILE)
    expectEqual(POSIXErrorCode.EMFILE.rawValue == EMFILE)
    expectEqual(POSIXErrorCode.ENOTTY.rawValue == ENOTTY)
    expectEqual(POSIXErrorCode.ETXTBSY.rawValue == ETXTBSY)
    expectEqual(POSIXErrorCode.EFBIG.rawValue == EFBIG)
    expectEqual(POSIXErrorCode.ENOSPC.rawValue == ENOSPC)
    expectEqual(POSIXErrorCode.ESPIPE.rawValue == ESPIPE)
    expectEqual(POSIXErrorCode.EROFS.rawValue == EROFS)
    expectEqual(POSIXErrorCode.EMLINK.rawValue == EMLINK)
    expectEqual(POSIXErrorCode.EPIPE.rawValue == EPIPE)
}
