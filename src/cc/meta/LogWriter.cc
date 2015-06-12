//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2015/03/21
// Author: Mike Ovsiannikov
//
// Copyright 2015 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// Transaction log writer.
//
//
//----------------------------------------------------------------------------

#include "LogWriter.h"
#include "LogTransmitter.h"
#include "MetaRequest.h"
#include "util.h"

#include "common/MsgLogger.h"
#include "common/MdStream.h"
#include "common/time.h"
#include "common/kfserrno.h"
#include "common/RequestParser.h"

#include "kfsio/NetManager.h"
#include "kfsio/ITimeout.h"
#include "kfsio/checksum.h"
#include "kfsio/PrngIsaac64.h"

#include "qcdio/QCThread.h"
#include "qcdio/QCMutex.h"
#include "qcdio/QCUtils.h"
#include "qcdio/qcstutils.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

namespace KFS
{
using std::ofstream;

class LogWriter::Impl :
    private ITimeout,
    private QCRunnable,
    private LogTransmitter::CommitObserver,
    private NetManager::Dispatcher
{
public:
    Impl()
        : ITimeout(),
          QCRunnable(),
          LogTransmitter::CommitObserver(),
          NetManager::Dispatcher(),
          mNetManagerPtr(0),
          mNetManager(),
          mLogTransmitter(mNetManager, *this),
          mTransmitCommitted(-1),
          mTransmitterUpFlag(false),
          mNextSeq(-1),
          mMaxDoneLogSeq(-1),
          mCommitted(),
          mThread(),
          mMutex(),
          mStopFlag(false),
          mOmitDefaultsFlag(true),
          mMaxBlockSize(256),
          mPendingCount(0),
          mLogDir("./kfslog"),
          mPendingQueue(),
          mInQueue(),
          mOutQueue(),
          mPendingAckQueue(),
          mPendingCommitted(),
          mInFlightCommitted(),
          mNextLogSeq(-1),
          mNextBlockSeq(-1),
          mLastLogSeq(-1),
          mBlockChecksum(kKfsNullChecksum),
          mNextBlockChecksum(kKfsNullChecksum),
          mLogFd(-1),
          mError(0),
          mMdStream(
            this,
            false,     // inSyncFlag
            string(),  // inDigestName
            1 << 20,   // inBufSize,
            true       // inResizeFlag
            ),
          mReqOstream(mMdStream),
          mCurLogStartTime(-1),
          mCurLogStartSeq(-1),
          mLogNum(0),
          mLogName(),
          mWriteState(kWriteStateNone),
          mLogRotateInterval(600 * 1000 * 1000),
          mPanicOnIoErrorFlag(false),
          mSyncFlag(false),
          mWokenFlag(false),
          mLastLogName("last"),
          mLastLogPath(mLogDir + "/" + mLastLogName),
          mFailureSimulationInterval(0),
          mRandom(),
          mLogFileNamePrefix("log")
        {}
    ~Impl()
        { Impl::Shutdown(); }
    int Start(
        NetManager&       inNetManager,
        seq_t             inLogNum,
        seq_t             inLogSeq,
        seq_t             inCommittedLogSeq,
        fid_t             inCommittedFidSeed,
        int64_t           inCommittedErrCheckSum,
        int               inCommittedStatus,
        const MdStateCtx* inLogAppendMdStatePtr,
        seq_t             inLogAppendStartSeq,
        seq_t             inLogAppendLastBlockSeq,
        bool              inLogAppendHexFlag,
        const char*       inParametersPrefixPtr,
        const Properties& inParameters,
        string&           outCurLogFileName)
    {
        if (inLogNum < 0 || inLogSeq < 0 ||
                (inLogAppendMdStatePtr && inLogSeq < inLogAppendStartSeq) ||
                (mThread.IsStarted() || mNetManagerPtr)) {
            return -EINVAL;
        }
        mNextBlockChecksum = ComputeBlockChecksum(kKfsNullChecksum, "\n", 1);
        mLogNum = inLogNum;
        const int theErr = SetParameters(inParametersPrefixPtr, inParameters);
        if (0 != theErr) {
            return theErr;
        }
        mMdStream.Reset(this);
        mCommitted.mErrChkSum = inCommittedErrCheckSum;
        mCommitted.mSeq       = inCommittedLogSeq;
        mCommitted.mFidSeed   = inCommittedFidSeed;
        mCommitted.mStatus    = inCommittedStatus;
        mPendingCommitted  = mCommitted;
        mInFlightCommitted = mPendingCommitted;
        if (inLogAppendMdStatePtr) {
            SetLogName(inLogSeq);
            mCurLogStartTime = microseconds();
            mCurLogStartSeq  = inLogAppendStartSeq;
            mMdStream.SetMdState(*inLogAppendMdStatePtr);
            if (! mMdStream) {
                KFS_LOG_STREAM_ERROR <<
                    "log append:" <<
                    " failed to set md context" <<
                KFS_LOG_EOM;
                return -EIO;
            }
            Close();
            mError = 0;
            if ((mLogFd = open(mLogName.c_str(), O_WRONLY, 0666)) < 0) {
                IoError(errno);
                return mError;
            }
            const off_t theSize = lseek(mLogFd, 0, SEEK_END);
            if (theSize < 0) {
                IoError(errno);
                return mError;
            }
            if (theSize == 0) {
                KFS_LOG_STREAM_ERROR <<
                    "log append: invalid empty file"
                    " file: " << mLogName <<
                    " size: " << theSize <<
                KFS_LOG_EOM;
                return -EINVAL;
            }
            KFS_LOG_STREAM_INFO <<
                "log append:" <<
                " idx: "      << mLogNum <<
                " start: "    << mCurLogStartSeq <<
                " cur: "      << mNextLogSeq <<
                " block: "    << inLogAppendLastBlockSeq <<
                " hex: "      << inLogAppendHexFlag <<
                " file: "     << mLogName <<
                " size: "     << theSize <<
                " checksum: " << mMdStream.GetMd() <<
            KFS_LOG_EOM;
            mMdStream.setf(
                inLogAppendHexFlag ? ostream::hex : ostream::dec,
                ostream::basefield
            );
            mNextBlockSeq = inLogAppendLastBlockSeq;
            if (inLogAppendLastBlockSeq < 0 || ! inLogAppendHexFlag) {
                // Previous / "old" log format.
                // Close the log segment even if it is empty and start new one.
                StartNextLog();
            } else {
                StartBlock(mNextBlockChecksum);
            }
        } else {
            NewLog(inLogSeq);
        }
        if (! IsLogStreamGood()) {
            return mError;
        }
        outCurLogFileName = mLogName;
        mStopFlag         = false;
        mNetManagerPtr    = &inNetManager;
        const int kStackSize = 64 << 10;
        mThread.Start(this, kStackSize, "LogWriter");
        mNetManagerPtr->RegisterTimeoutHandler(this);
        return 0;
    }
    bool Enqueue(
        MetaRequest& inRequest)
    {
        inRequest.next  = 0;
        inRequest.seqno = ++mNextSeq;
        if (mStopFlag) {
            inRequest.status    = -ELOGFAILED;
            inRequest.statusMsg = "log writer is not running";
            return false;
        }
        int* const theCounterPtr = inRequest.GetLogQueueCounter();
        if ((mPendingCount <= 0 ||
                    ! theCounterPtr || *theCounterPtr <= 0) &&
                (MetaRequest::kLogNever == inRequest.logAction ||
                (MetaRequest::kLogIfOk == inRequest.logAction &&
                    0 != inRequest.status))) {
            return false;
        }
        if (theCounterPtr) {
            if (++*theCounterPtr <= 0) {
                panic("request enqueue: invalid log queue counter");
            }
        }
        inRequest.commitPendingFlag = true;
        if (++mPendingCount <= 0) {
            panic("log writer: invalid pending count");
        }
        mPendingQueue.PushBack(inRequest);
        return true;
    }
    void RequestCommitted(
        MetaRequest& inRequest,
        fid_t        inFidSeed)
    {
        if (! inRequest.commitPendingFlag) {
            return;
        }
        int* const theCounterPtr = inRequest.GetLogQueueCounter();
        if (theCounterPtr) {
            if (--*theCounterPtr < 0) {
                panic("request committed: invalid log queue counter");
            }
        }
        inRequest.commitPendingFlag = false;
        if (inRequest.logseq < 0) {
            return;
        }
        if (inRequest.suspended) {
            panic("request committed: invalid suspended state");
        }
        if (mCommitted.mSeq + 1 != inRequest.logseq &&
                0 <= mCommitted.mSeq) {
            panic("request committed: invalid out of order log sequence");
            return;
        }
        const int theStatus = inRequest.status < 0 ?
            SysToKfsErrno(-inRequest.status) : 0;
        mCommitted.mErrChkSum += theStatus;
        mCommitted.mSeq       = inRequest.logseq;
        mCommitted.mFidSeed   = inFidSeed;
        mCommitted.mStatus    = theStatus;
    }
    seq_t GetCommittedLogSeq() const
        { return mCommitted.mSeq; }
    void GetCommitted(
        seq_t&   outLogSeq,
        int64_t& outErrChecksum,
        fid_t&   outFidSeed,
        int&     outStatus) const
    {
        outLogSeq      = mCommitted.mSeq;
        outErrChecksum = mCommitted.mErrChkSum;
        outFidSeed     = mCommitted.mFidSeed;
        outStatus      = mCommitted.mStatus;
    }
    void SetCommitted(
        seq_t   inLogSeq,
        int64_t inErrChecksum,
        fid_t   inFidSeed,
        int     inStatus)
    {
        mCommitted.mSeq       = inLogSeq;
        mCommitted.mErrChkSum = inErrChecksum;
        mCommitted.mFidSeed   = inFidSeed;
        mCommitted.mStatus    = inStatus;
    }
    void ScheduleFlush()
    {
        if (mPendingQueue.IsEmpty()) {
            return;
        }
        QCStMutexLocker theLock(mMutex);
        mPendingCommitted = mCommitted;
        mInQueue.PushBack(mPendingQueue);
        theLock.Unlock();
        mNetManager.Wakeup();
    }
    void Shutdown()
    {
        if (! mThread.IsStarted() || mStopFlag) {
            return;
        }
        QCStMutexLocker theLock(mMutex);
        // Mark everything committed to cleanup queues.
        mTransmitCommitted = mNextLogSeq;
        mStopFlag = true;
        mNetManager.Wakeup();
        theLock.Unlock();
        mThread.Join();
        if (mNetManagerPtr) {
            mNetManagerPtr->UnRegisterTimeoutHandler(this);
            mNetManagerPtr = 0;
        }
    }
    void ChildAtFork()
    {
        mNetManager.ChildAtFork();
        Close();
    }
    virtual void Notify(
        seq_t inSeq)
    {
        const bool theWakeupFlag = mTransmitCommitted < inSeq &&
            ! mWokenFlag;
        mTransmitCommitted = inSeq;
        mTransmitterUpFlag = mLogTransmitter.IsUp();
        if (theWakeupFlag) {
            mWokenFlag = true;
            // mNetManager.Wakeup();
        }
    }
private:
    typedef uint32_t Checksum;
    class Committed
    {
    public:
        seq_t   mSeq;
        fid_t   mFidSeed;
        int64_t mErrChkSum;
        int     mStatus;
        Committed()
            : mSeq(-1),
              mFidSeed(-1),
              mErrChkSum(0),
              mStatus(0)
            {}
    };
    class Queue
    {
    public:
        Queue()
            : mHeadPtr(0),
              mTailPtr(0)
            {}
        Queue(
            MetaRequest* inHeadPtr,
            MetaRequest* inTailPtr)
            : mHeadPtr(inHeadPtr),
              mTailPtr(inTailPtr)
            {}
        void Reset()
        {
            mHeadPtr = 0;
            mTailPtr = 0;
        }
        void PushBack(
            Queue& inQueue)
        {
            if (mTailPtr) {
                mTailPtr->next = inQueue.mHeadPtr;
            } else {
                mHeadPtr = inQueue.mHeadPtr;
            }
            mTailPtr = inQueue.mTailPtr;
            inQueue.Reset();
        }
        void PushBack(
            MetaRequest& inRequest)
        {
            if (mTailPtr) {
                mTailPtr->next = &inRequest;
            } else {
                mHeadPtr = &inRequest;
            }
            mTailPtr = &inRequest;
        }
        MetaRequest* Front() const
            { return mHeadPtr; }
        MetaRequest* Back() const
            { return mTailPtr; }
        bool IsEmpty() const
            { return (! mHeadPtr); }
    private:
        MetaRequest* mHeadPtr;
        MetaRequest* mTailPtr;
    };
    typedef MdStreamT<Impl> MdStream;
    enum WriteState
    {
        kWriteStateNone,
        kUpdateBlockChecksum
    };

    NetManager*    mNetManagerPtr;
    NetManager     mNetManager;
    LogTransmitter mLogTransmitter;
    seq_t          mTransmitCommitted;
    bool           mTransmitterUpFlag;
    seq_t          mNextSeq;
    seq_t          mMaxDoneLogSeq;
    Committed      mCommitted;
    QCThread       mThread;
    QCMutex        mMutex;
    bool           mStopFlag;
    bool           mOmitDefaultsFlag;
    int            mMaxBlockSize;
    int            mPendingCount;
    string         mLogDir;
    Queue          mPendingQueue;
    Queue          mInQueue;
    Queue          mOutQueue;
    Queue          mPendingAckQueue;
    Committed      mPendingCommitted;
    Committed      mInFlightCommitted;
    seq_t          mNextLogSeq;
    seq_t          mNextBlockSeq;
    seq_t          mLastLogSeq;
    Checksum       mBlockChecksum;
    Checksum       mNextBlockChecksum;
    int            mLogFd;
    int            mError;
    MdStream       mMdStream;
    ReqOstream     mReqOstream;
    int64_t        mCurLogStartTime;
    seq_t          mCurLogStartSeq;
    seq_t          mLogNum;
    string         mLogName;
    WriteState     mWriteState;
    int64_t        mLogRotateInterval;
    bool           mPanicOnIoErrorFlag;
    bool           mSyncFlag;
    bool           mWokenFlag;
    string         mLastLogName;
    string         mLastLogPath;
    int64_t        mFailureSimulationInterval;
    PrngIsaac64    mRandom;
    const string   mLogFileNamePrefix;

    virtual void Timeout()
    {
        if (mPendingCount <= 0) {
            return;
        }
        QCStMutexLocker theLock(mMutex);
        MetaRequest* theDonePtr = mOutQueue.Front();
        mOutQueue.Reset();
        theLock.Unlock();
        while (theDonePtr) {
            MetaRequest& theReq = *theDonePtr;
            theDonePtr = theReq.next;
            theReq.next = 0;
            if (0 <= theReq.logseq) {
                if (theReq.logseq <= mMaxDoneLogSeq) {
                    panic("log writer: invalid log sequence number");
                }
                mMaxDoneLogSeq = theReq.logseq;
            }
            if (--mPendingCount < 0) {
                panic("log writer: request completion invalid pending count");
            }
            submit_request(&theReq);
        }
    }
    void ProcessPendingAckQueue(
        Queue& inDoneQueue)
    {
        mWokenFlag = false;
        mPendingAckQueue.PushBack(inDoneQueue);
        if (mTransmitCommitted < mNextLogSeq) {
            MetaRequest* thePtr     = mPendingAckQueue.Front();
            MetaRequest* thePrevPtr = 0;
            while (thePtr) {
                if (mTransmitCommitted < thePtr->logseq) {
                    if (thePrevPtr) {
                        thePrevPtr->next = 0;
                        inDoneQueue =
                            Queue(mPendingAckQueue.Front(), thePrevPtr);
                        mPendingAckQueue =
                            Queue(thePtr, mPendingAckQueue.Back());
                    }
                    break;
                }
                thePrevPtr = thePtr;
                thePtr     = thePtr->next;
            }
        } else {
            inDoneQueue.PushBack(mPendingAckQueue);
        }
        if (inDoneQueue.IsEmpty()) {
            return;
        }
        QCStMutexLocker theLocker(mMutex);
        mOutQueue.PushBack(inDoneQueue);
        mNetManagerPtr->Wakeup();
    }
    virtual void DispatchStart()
    {
        QCStMutexLocker theLocker(mMutex);
        if (mStopFlag) {
            mNetManager.Shutdown();
        }
        if (mInQueue.IsEmpty()) {
            if (mWokenFlag) {
                Queue theTmp;
                ProcessPendingAckQueue(theTmp);
            }
            return;
        }
        Queue theWriteQueue = mInQueue;
        mInQueue.Reset();
        mInFlightCommitted = mPendingCommitted;
        theLocker.Unlock();
        mWokenFlag = true;
        Write(*theWriteQueue.Front());
        ProcessPendingAckQueue(theWriteQueue);
    }
    virtual void DispatchEnd()
    {
        if (mWokenFlag) {
            Queue theTmp;
            ProcessPendingAckQueue(theTmp);
        }
    }
    virtual void DispatchExit()
        {}
    virtual void Run()
    {
        QCMutex* const kMutexPtr             = 0;
        bool const     kWakeupAndCleanupFlag = true;
        mNetManager.MainLoop(kMutexPtr, kWakeupAndCleanupFlag, this);
        Sync();
        Close();
    }
    void Write(
        MetaRequest& inHead)
    {
        if (! IsLogStreamGood()) {
            if (mCurLogStartSeq < mNextLogSeq) {
                StartNextLog();
            } else {
                NewLog(mNextLogSeq);
            }
        }
        mMdStream.SetSync(false);
        ostream&     theStream = mMdStream;
        MetaRequest* theCurPtr = &inHead;
        while (theCurPtr) {
            mLastLogSeq = mNextLogSeq;
            MetaRequest*          thePtr                 = theCurPtr;
            seq_t                 theEndBlockSeq         =
                mNextLogSeq + mMaxBlockSize;
            const bool            theSimulateFailureFlag = IsSimulateFailure();
            const bool            theTransmitterUpFlag   = mTransmitterUpFlag;
            MetaLogWriterControl* theCtlPtr              = 0;
            for ( ; thePtr; thePtr = thePtr->next) {
                if (META_LOG_WRITER_CONTROL == thePtr->op) {
                    theCtlPtr = static_cast<MetaLogWriterControl*>(thePtr);
                    if (Control(*theCtlPtr)) {
                        break;
                    }
                    theCtlPtr = 0;
                    theEndBlockSeq = mNextLogSeq + mMaxBlockSize;
                    continue;
                }
                if (! theStream || ! theTransmitterUpFlag) {
                    continue;
                }
                if (((MetaRequest::kLogIfOk == thePtr->logAction &&
                            0 == thePtr->status) ||
                        MetaRequest::kLogAlways == thePtr->logAction)) {
                    if (theSimulateFailureFlag) {
                        KFS_LOG_STREAM_ERROR <<
                            "log writer: simulating write error:"
                            " " << thePtr->Show() <<
                        KFS_LOG_EOM;
                        break;
                    }
                    thePtr->logseq = ++mLastLogSeq;
                    if (! thePtr->WriteLog(theStream, mOmitDefaultsFlag)) {
                        panic("log writer: invalid request ");
                    }
                    if (! theStream) {
                        --mLastLogSeq;
                        LogError(*thePtr);
                    }
                }
                if (theEndBlockSeq <= mLastLogSeq) {
                    break;
                }
                if (mMdStream.GetBufferedStart() +
                            mMdStream.GetBufferSize() / 4 * 3 <
                        mMdStream.GetBufferedEnd()) {
                    break;
                }
            }
            MetaRequest* const theEndPtr = thePtr ? thePtr->next : thePtr;
            if (mNextLogSeq < mLastLogSeq &&
                    theTransmitterUpFlag && IsLogStreamGood()) {
                FlushBlock(mLastLogSeq);
            }
            if (IsLogStreamGood() &&
                    ! theSimulateFailureFlag && theTransmitterUpFlag) {
                mNextLogSeq = mLastLogSeq;
            } else {
                mLastLogSeq = mNextLogSeq;
                // Write failure.
                for (thePtr = theCurPtr;
                        theEndPtr != thePtr;
                        thePtr = thePtr->next) {
                    if (META_LOG_WRITER_CONTROL != thePtr->op &&
                            ((MetaRequest::kLogIfOk == thePtr->logAction &&
                                0 == thePtr->status) ||
                            MetaRequest::kLogAlways == thePtr->logAction)) {
                        LogError(*thePtr);
                    }
                }
            }
            if (theCtlPtr &&
                    MetaLogWriterControl::kWriteBlock == theCtlPtr->type) {
                WriteBlock(*theCtlPtr);
            }
            theCurPtr = theEndPtr;
        }
        if (mCurLogStartSeq < mNextLogSeq && IsLogStreamGood() &&
                mCurLogStartTime + mLogRotateInterval <
                microseconds()) {
            StartNextLog();
        }
    }
    void StartNextLog()
    {
        CloseLog();
        mLogNum++;
        NewLog(mLastLogSeq);
    }
    void LogError(
        MetaRequest& inReq)
    {
        inReq.logseq    = -1;
        inReq.status    = -ELOGFAILED;
        inReq.statusMsg = "transaction log write error";
    }
    void StartBlock(
        Checksum inStartCheckSum)
    {
        mMdStream.SetSync(false);
        mWriteState    = kUpdateBlockChecksum;
        mBlockChecksum = inStartCheckSum;
    }
    void FlushBlock(
        seq_t inLogSeq)
    {
        ++mNextBlockSeq;
        mReqOstream << "c"
            "/" << mInFlightCommitted.mSeq <<
            "/" << mInFlightCommitted.mFidSeed <<
            "/" << mInFlightCommitted.mErrChkSum <<
            "/" << mInFlightCommitted.mStatus <<
            "/" << inLogSeq <<
            "/"
        ;
        mReqOstream.flush();
        const char*  theStartPtr = mMdStream.GetBufferedStart();
        const size_t theTxLen    = mMdStream.GetBufferedEnd() - theStartPtr;
        mBlockChecksum = ComputeBlockChecksum(
            mBlockChecksum, theStartPtr, theTxLen);
        Checksum const theTxChecksum = mBlockChecksum;
        mReqOstream <<
            mNextBlockSeq <<
            "/";
        mReqOstream.flush();
        theStartPtr = mMdStream.GetBufferedStart() + theTxLen;
        mBlockChecksum = ComputeBlockChecksum(
            mBlockChecksum,
            theStartPtr,
            mMdStream.GetBufferedEnd() - theStartPtr);
        mWriteState = kWriteStateNone;
        mReqOstream << mBlockChecksum << "\n";
        mReqOstream.flush();
        // Transmit all blocks, except first one, which has only block header.
        if (0 < mNextBlockSeq) {
            theStartPtr = mMdStream.GetBufferedStart();
            int theStatus;
            if (mMdStream.GetBufferedEnd() < theStartPtr + theTxLen) {
                panic("invalid log write write buffer length");
                theStatus = -EFAULT;
            } else {
                theStatus = mLogTransmitter.TransmitBlock(
                    inLogSeq,
                    (int)(inLogSeq - mNextLogSeq),
                    theStartPtr,
                    theTxLen,
                    theTxChecksum,
                    theTxLen
                );
            }
            if (0 != theStatus) {
                KFS_LOG_STREAM_ERROR <<
                    "block transmit failure:"
                    " seq: "    << inLogSeq  <<
                    " status: " << theStatus <<
                KFS_LOG_EOM;
                mTransmitterUpFlag = false;
            }
        }
        mMdStream.SetSync(true);
        mReqOstream.flush();
        Sync();
        StartBlock(mNextBlockChecksum);
    }
    void Sync()
    {
        if (mLogFd < 0 || ! mSyncFlag) {
            return;
        }
        if (fsync(mLogFd)) {
            IoError(-errno);
        }
    }
    void IoError(
        int         inError,
        const char* inMsgPtr = 0)
    {
        mError = inError;
        if (0 < mError) {
            mError = -mError;
        } else if (mError == 0) {
            mError = -EIO;
        }
        KFS_LOG_STREAM_ERROR <<
            (inMsgPtr ? inMsgPtr : "transaction log writer error:") <<
             " " << mLogName << ": " << QCUtils::SysError(inError) <<
        KFS_LOG_EOM;
        if (mPanicOnIoErrorFlag) {
            panic("transaction log io failure");
        }
    }
    void IoError(
        int           inError,
        const string& inMsg)
        { IoError(inError, inMsg.c_str()); }
    bool Control(
        MetaLogWriterControl& inRequest)
    {
        KFS_LOG_STREAM_DEBUG <<
            inRequest.Show() <<
        KFS_LOG_EOM;
        bool theRetFlag = true;
        switch (inRequest.type) {
            default:
            case MetaLogWriterControl::kNop:
                theRetFlag = false;
                break;
            case MetaLogWriterControl::kNewLog:
                if (mCurLogStartSeq < mLastLogSeq) {
                    StartNextLog();
                }
                break;
            case MetaLogWriterControl::kWriteBlock:
                return true;
            case MetaLogWriterControl::kSetParameters:
                SetParameters(
                    inRequest.paramsPrefix.c_str(),
                    inRequest.params
                );
                return false; // Do not start new record block.
        }
        inRequest.committed  = mInFlightCommitted.mSeq;
        inRequest.lastLogSeq = mLastLogSeq;
        inRequest.logName    = mLogName;
        return (theRetFlag && IsLogStreamGood());
    }
    void WriteBlock(
        MetaLogWriterControl& inRequest)
    {
        if (inRequest.blockData.BytesConsumable() <= 0) {
            panic("write block: invalid block length");
            inRequest.status = -EFAULT;
            return;
        }
        if (inRequest.blockLines.IsEmpty()) {
            panic("write block: invalid invocation, no log lines");
            inRequest.status = -EFAULT;
            return;
        }
        if (mLastLogSeq != mNextLogSeq) {
            panic("invalid write block invocation");
            inRequest.status = -EFAULT;
            return;
        }
        if (inRequest.blockStartSeq != mLastLogSeq) {
            inRequest.status    = -EINVAL;
            inRequest.statusMsg = "invalid block start sequence";
            return;
        }
        if (! IsLogStreamGood()) {
            inRequest.status    = -EIO;
            inRequest.statusMsg = "log write error";
            return;
        }
        // Copy block data, and write block sequence and updated checksum.
        mMdStream.SetSync(false);
        mWriteState = kWriteStateNone;
        // To include leading \n, if any, "combine" block checksum.
        mBlockChecksum = ChecksumBlocksCombine(
            mBlockChecksum,
            inRequest.blockChecksum,
            inRequest.blockData.BytesConsumable()
        );
        const size_t thePos = mMdStream.GetBufferedEnd() -
            mMdStream.GetBufferedStart();
        for (IOBuffer::iterator theIt = inRequest.blockData.begin();
                theIt != inRequest.blockData.end();
                ++theIt) {
            const char* const thePtr = theIt->Consumer();
            mReqOstream.write(thePtr, theIt->Producer() - thePtr);
        }
        const size_t theLen = mMdStream.GetBufferedEnd() -
            (mMdStream.GetBufferedStart() + thePos);
        ++mNextBlockSeq;
        mReqOstream <<
            mNextBlockSeq <<
            "/";
        mReqOstream.flush();
        const char* thePtr        = mMdStream.GetBufferedStart() +
            thePos + theLen;
        size_t      theTrailerLen = mMdStream.GetBufferedEnd() - thePtr;
        mBlockChecksum = ComputeBlockChecksum(
            mBlockChecksum, thePtr, theTrailerLen);
        mReqOstream << mBlockChecksum << "\n";
        mReqOstream.flush();
        // Append trailer to make block replay work, and parse committed.
        thePtr        = mMdStream.GetBufferedStart() + thePos + theLen;
        theTrailerLen = mMdStream.GetBufferedEnd() - thePtr;
        inRequest.blockData.CopyIn(thePtr, theTrailerLen);
        const char* const theEndPtr = thePtr;
        thePtr = theEndPtr - inRequest.blockLines.Back();
        inRequest.blockLines.Back() += theTrailerLen;
        inRequest.blockCommitted = -1;
        if (thePtr + 2 < theEndPtr &&
                (*thePtr & 0xFF) == 'c' && (thePtr[1] & 0xFF) == '/') {
            thePtr += 2;
            const char* theStartPtr = thePtr;
            while (thePtr < theEndPtr && (*thePtr & 0xFF) != '/') {
                ++thePtr;
            }
            if (thePtr < theEndPtr &&
                    (*thePtr & 0xFF) == '/' &&
                    ! HexIntParser::Parse(
                        theStartPtr,
                        thePtr - theStartPtr,
                        inRequest.blockCommitted)) {
                inRequest.blockCommitted = -1;
            }
        }
        if (inRequest.blockCommitted < 0) {
            mMdStream.ClearBuffer();
            --mNextBlockSeq;
            inRequest.status    = -EIO;
            inRequest.statusMsg = "log write: invalid block format";
            return;
        }
        const int theStatus = mLogTransmitter.TransmitBlock(
            inRequest.blockEndSeq,
            (int)(inRequest.blockEndSeq - inRequest.blockStartSeq),
            mMdStream.GetBufferedStart() + thePos,
            theLen,
            inRequest.blockChecksum,
            theLen
        );
        if (0 != theStatus) {
            KFS_LOG_STREAM_ERROR <<
                "write block: block transmit failure:"
                " ["    << inRequest.blockStartSeq  <<
                ":"     << inRequest.blockEndSeq <<
                "]"
                " status: " << theStatus <<
            KFS_LOG_EOM;
            mTransmitterUpFlag = false;
        }
        mMdStream.SetSync(true);
        mReqOstream.flush();
        Sync();
        if (IsLogStreamGood()) {
            inRequest.blockSeq = mNextBlockSeq;
            mLastLogSeq      = inRequest.blockEndSeq;
            mNextLogSeq      = mLastLogSeq;
            inRequest.status = 0;
            StartBlock(mNextBlockChecksum);
        } else {
            inRequest.status    = -EIO;
            inRequest.statusMsg = "log write error";
        }
    }
    void CloseLog()
    {
        if (IsLogStreamGood()) {
            if (mLastLogSeq != mNextLogSeq) {
                FlushBlock(mLastLogSeq);
                if (! IsLogStreamGood()) {
                    mLastLogSeq = mNextLogSeq;
                    return;
                }
            }
            mWriteState = kWriteStateNone;
            mMdStream.SetSync(true);
            mMdStream << "time/" << DisplayIsoDateTime() << "\n";
            const string theChecksum = mMdStream.GetMd();
            mMdStream << "checksum/" << theChecksum << "\n";
            mMdStream.flush();
        } else {
            mLastLogSeq = mNextLogSeq;
        }
        Sync();
        if (0 <= mLogFd && Close() && link_latest(mLogName, mLastLogPath)) {
            IoError(errno, "failed to link to: " + mLastLogPath);
        }
    }
    void NewLog(
        seq_t inLogSeq)
    {
        Close();
        mCurLogStartTime = microseconds();
        mNextBlockSeq    = -1;
        mError           = 0;
        mWriteState      = kWriteStateNone;
        SetLogName(inLogSeq);
        if ((mLogFd = open(
                mLogName.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666)) < 0) {
            IoError(errno);
            return;
        }
        StartBlock(kKfsNullChecksum);
        mMdStream.Reset(this);
        mMdStream.clear();
        mReqOstream.Get().clear();
        mMdStream.setf(ostream::dec, ostream::basefield);
        mMdStream.SetSync(false);
        mMdStream <<
            "version/" << int(LogWriter::VERSION) << "\n"
            "checksum/last-line\n"
            "setintbase/16\n"
            "time/" << DisplayIsoDateTime() << "\n"
        ;
        mMdStream.setf(ostream::hex, ostream::basefield);
        FlushBlock(mLastLogSeq);
        if (IsLogStreamGood()) {
            mNextLogSeq = mLastLogSeq;
        } else {
            mLastLogSeq = mNextLogSeq;
        }
    }
    void SetLogName(
        seq_t inLogSeq)
    {
        mCurLogStartSeq = inLogSeq;
        mNextLogSeq     = inLogSeq;
        mLastLogSeq     = inLogSeq;
        mLogName        = makename(mLogDir, mLogFileNamePrefix, mLogNum);
    }
    int SetParameters(
        const char*       inParametersPrefixPtr,
        const Properties& inParameters)
    {
        Properties::String theName(
            inParametersPrefixPtr ? inParametersPrefixPtr : "");
        const size_t       thePrefixLen = theName.length();
        mOmitDefaultsFlag = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("omitDefaults"),
            mOmitDefaultsFlag ? 1 : 0) != 0;
        mMaxBlockSize = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("maxBlockSize"),
            mMaxBlockSize);
        mLogDir = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("logDir"),
            mLogDir);
        mLastLogName = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("lastLogName"),
            mLastLogName);
        mLogRotateInterval = (int64_t)(inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("rotateIntervalSec"),
            mLogRotateInterval * 1e-6) * 1e6);
        mPanicOnIoErrorFlag = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("panicOnIoError"),
            mPanicOnIoErrorFlag ? 1 : 0) != 0;
        mSyncFlag = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("sync"),
            mSyncFlag ? 1 : 0) != 0;
        mFailureSimulationInterval = inParameters.getValue(
            theName.Truncate(thePrefixLen).Append("failureSimulationInterval"),
            mFailureSimulationInterval);
        mLastLogPath = mLogDir + "/" + mLastLogName;
        return mLogTransmitter.SetParameters(
            theName.Truncate(thePrefixLen).Append("transmitter.").c_str(),
            inParameters
        );
    }
    bool IsLogStreamGood()
    {
        if (0 != mError || mLogFd < 0) {
            return false;
        }
        if (! mMdStream) {
            IoError(EIO, "log md5 failure");
            return false;
        }
        return true;
    }
    // File write methods.
    friend class MdStreamT<Impl>;
    bool write(
        const char* inBufPtr,
        size_t      inSize)
    {
        if (! mMdStream.IsSync()) {
            panic("invalid write invocation");
            return false;
        }
        if (kUpdateBlockChecksum == mWriteState) {
            mBlockChecksum = ComputeBlockChecksum(
                mBlockChecksum, inBufPtr, inSize);
        }
        if (0 < inSize && IsLogStreamGood()) {
            const char*       thePtr    = inBufPtr;
            const char* const theEndPtr = thePtr + inSize;
            ssize_t           theNWr    = 0;
            while (thePtr < theEndPtr && 0 <= (theNWr = ::write(
                        mLogFd, inBufPtr, theEndPtr - thePtr))) {
                thePtr += theNWr;
            }
            if (theNWr < 0) {
                IoError(errno);
            }
        }
        return IsLogStreamGood();
    }
    bool flush()
        { return IsLogStreamGood(); }
    bool IsSimulateFailure()
    {
        return (
            0 < mFailureSimulationInterval &&
            0 == (mRandom.Rand() % mFailureSimulationInterval)
        );
    }
    bool Close()
    {
        if (mLogFd <= 0) {
            return false;
        }
        const bool theOkFlag = close(mLogFd) == 0;
        if (! theOkFlag) {
            IoError(errno);
        }
        mLogFd = -1;
        return theOkFlag;
    }
};

LogWriter::LogWriter()
    : mImpl(*(new Impl()))
{}

LogWriter::~LogWriter()
{
    delete &mImpl;
}

    int
LogWriter::Start(
    NetManager&       inNetManager,
    seq_t             inLogNum,
    seq_t             inLogSeq,
    seq_t             inCommittedLogSeq,
    fid_t             inCommittedFidSeed,
    int64_t           inCommittedErrCheckSum,
    int               inCommittedStatus,
    const MdStateCtx* inLogAppendMdStatePtr,
    seq_t             inLogAppendStartSeq,
    seq_t             inLogAppendLastBlockSeq,
    bool              inLogAppendHexFlag,
    const char*       inParametersPrefixPtr,
    const Properties& inParameters,
    string&           outCurLogFileName)
{
    return mImpl.Start(
        inNetManager,
        inLogNum,
        inLogSeq,
        inCommittedLogSeq,
        inCommittedFidSeed,
        inCommittedErrCheckSum,
        inCommittedStatus,
        inLogAppendMdStatePtr,
        inLogAppendStartSeq,
        inLogAppendLastBlockSeq,
        inLogAppendHexFlag,
        inParametersPrefixPtr,
        inParameters,
        outCurLogFileName
    );
}

    bool
LogWriter::Enqueue(
    MetaRequest& inRequest)
{
    return mImpl.Enqueue(inRequest);
}

    void
LogWriter::Committed(
    MetaRequest& inRequest,
    fid_t        inFidSeed)
{
    mImpl.RequestCommitted(inRequest, inFidSeed);
}

    void
LogWriter::GetCommitted(
    seq_t&   outLogSeq,
    int64_t& outErrChecksum,
    fid_t&   outFidSeed,
    int&     outStatus) const
{
    mImpl.GetCommitted(
        outLogSeq,
        outErrChecksum,
        outFidSeed,
        outStatus
    );
}

    void
LogWriter::SetCommitted(
    seq_t   inLogSeq,
    int64_t inErrChecksum,
    fid_t   inFidSeed,
    int     inStatus)
{
    mImpl.SetCommitted(
        inLogSeq,
        inErrChecksum,
        inFidSeed,
        inStatus
    );
}

    seq_t
LogWriter::GetCommittedLogSeq() const
{
    return mImpl.GetCommittedLogSeq();
}

    void
LogWriter::ScheduleFlush()
{
   mImpl.ScheduleFlush();
}

    void
LogWriter::ChildAtFork()
{
    mImpl.ChildAtFork();
}

    void
LogWriter::Shutdown()
{
   mImpl.Shutdown();
}

} // namespace KFS