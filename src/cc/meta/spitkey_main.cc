#include <iostream>
#include <stdio.h>
#include <string>

#include "kfsio/blockname.h"
#include "kfstree.h"
#include "Checkpoint.h"
#include "Logger.h"
#include "common/kfsdecls.h"
#include "common/MsgLogger.h"
#include "kfsio/blockname.h"
#include "Replay.h"
#include "Restorer.h"

using namespace KFS;
using std::cin;

static int RestoreCheckpoint()
{
    Restorer theRestorer;
    return (theRestorer.rebuild(LASTCP) ? 0 : -EIO);
}

int
main(int argc, char **argv)
{
    string checkpointDir = "/home/aws-s3-bfsm0/state/checkpoint";
    string transactionDir = "/home/aws-s3-bfsm0/state/transactions";
    bool replayLastLogFlag = false;
    MsgLogger::LogLevel logLevel = MsgLogger::kLogLevelDEBUG;
    bool verbose = false;

    MsgLogger::Init(0, logLevel);
    checkpointer_setup_paths(checkpointDir);
    logger_setup_paths(transactionDir);

    int status;
    status = RestoreCheckpoint();
    if( status != 0) {
        printf("can't restore checkpoint!");
        return 1;
    }

    status = replayer.playLogs(replayLastLogFlag);
    if( status != 0) {
        printf("can't replay logs!");
        return 1;
    }

    const int64_t theFileSystemId = metatree.GetFsId();
    KFS_LOG_STREAM_ERROR << "file-system id: " << theFileSystemId << KFS_LOG_EOM;

    // MetaFattr* const theFattrPtr;
    fid_t parentDirFid = ROOTFID;
    kfsUid_t euser = 0;
    kfsGid_t egroup;

    string givenFilename;
    givenFilename.reserve(256);
    while (getline(cin, givenFilename)) {
        KFS_LOG_STREAM_INFO << "filename: " << givenFilename << KFS_LOG_EOM;
        MetaFattr* fa;
        string objectKey;
        objectKey.reserve(256);
        status = metatree.lookupPath(parentDirFid, givenFilename, euser, egroup, fa);
        if (status != 0) {
            KFS_LOG_STREAM_ERROR << "lookup operation failed!" << KFS_LOG_EOM;
            return 1;
        }
        string fsIdSuffix;
        chunkOff_t filesize = fa->filesize;
        kfsSTier_t minTier = fa->minSTier;
        fid_t fid = fa->id();
        for(chunkOff_t s = 0; s < filesize; s += (chunkOff_t)CHUNKSIZE) {
            seq_t version = -1 - minTier - s;
            if ( ! AppendChunkFileNameOrObjectStoreBlockKey(
                                objectKey,
                                theFileSystemId,
                                fid,
                                fid,
                                version,
                                fsIdSuffix)) {
                KFS_LOG_STREAM_ERROR << "something went wrong when converting the object key!" << KFS_LOG_EOM;
                return 1;
            }
            KFS_LOG_STREAM_INFO << "s: " << s << " key: " << objectKey << KFS_LOG_EOM;
            objectKey.clear();
        }
        givenFilename.clear();
    }
}

