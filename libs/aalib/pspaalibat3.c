////////////////////////////////////////////////
//
//		pspaalibat3.c
//		Part of the PSP Advanced Audio Library
//		Created by Arshia001
//
//		This file includes functions for playing AT3 files
//		via the PSP's Media Engine.
//		Special thanks to mowglisanu of the PSP-Programming
//		forums for helping me with the following code.
//
////////////////////////////////////////////////

#include "pspaalibat3.h"

typedef struct
{
    int id;
    unsigned short tempBuf[4096];
    unsigned short *buf;
    int bufSize;
    void *data;
    int dataSize;
    int stopReason;
    bool paused;
    bool initialized;
    bool autoloop;
} At3FileInfo;

At3FileInfo streamsAt3[2];

// One lock for every call into sceAtrac. The play thread decodes
// (GetBufferAt3) while the game stops, rewinds, loops and unloads the same
// ATRAC ID from its own thread; sceAtrac is not safe for that, and the race
// ended in a bus error in either thread. The unload clears `initialized`
// under the lock, so a play thread that waited for it finds the channel empty
// and gives silence.
static SceUID at3Lock = -1;
static void LockAt3(void) { if (at3Lock >= 0) sceKernelWaitSema(at3Lock, 1, NULL); }
static void UnlockAt3(void) { if (at3Lock >= 0) sceKernelSignalSema(at3Lock, 1); }

// The rewind without the lock, for the callers that hold it.
static void ResetAt3(int channel) {
    sceAtracResetPlayPosition(streamsAt3[channel].id, 0, 0, 0);
}

bool GetPausedAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    return streamsAt3[channel].paused;
}

int SetAutoloopAt3(int channel, bool autoloop) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    LockAt3();
    streamsAt3[channel].autoloop = autoloop;
    // A file with a loop (a smpl chunk) is looped by sceAtrac itself, so its
    // beginning is played once. Without one the call fails and the stream is
    // rewound to the start at its end (GetBufferAt3).
    if (streamsAt3[channel].initialized)
        sceAtracSetLoopNum(streamsAt3[channel].id, autoloop ? -1 : 0);
    UnlockAt3();
    return PSPAALIB_SUCCESS;
}

int GetStopReasonAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    return streamsAt3[channel].stopReason;
}

int PlayAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    streamsAt3[channel].paused = FALSE;
    streamsAt3[channel].stopReason = PSPAALIB_STOP_NOT_STOPPED;
    return PSPAALIB_SUCCESS;
}

int StopAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    LockAt3();
    if (streamsAt3[channel].initialized)
        ResetAt3(channel);
    streamsAt3[channel].paused = TRUE;
    streamsAt3[channel].stopReason = PSPAALIB_STOP_ON_REQUEST;
    UnlockAt3();
    return PSPAALIB_SUCCESS;
}

int PauseAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    streamsAt3[channel].paused = !streamsAt3[channel].paused;
    streamsAt3[channel].stopReason = PSPAALIB_STOP_NOT_STOPPED;
    return PSPAALIB_SUCCESS;
}

int RewindAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (!streamsAt3[channel].initialized) {
        return PSPAALIB_ERROR_AT3_UNINITIALIZED_CHANNEL;
    }
    LockAt3();
    if (streamsAt3[channel].initialized)
        ResetAt3(channel);
    UnlockAt3();
    return PSPAALIB_SUCCESS;
}

int GetBufferAt3(short *buf, int length, float amp, int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    int byteLength = length << 2;
    LockAt3();
    if ((streamsAt3[channel].paused) || (!streamsAt3[channel].initialized)) {
        UnlockAt3();
        memset(buf, 0, byteLength);
        return PSPAALIB_WARNING_PAUSED_BUFFER_REQUESTED;
    }
    int num, end, remain, i;
    while (streamsAt3[channel].bufSize < byteLength) {
        sceAtracDecodeData(streamsAt3[channel].id, (void *)streamsAt3[channel].tempBuf, &num, &end, &remain);
        if (end) {
            if (!streamsAt3[channel].autoloop) {
                streamsAt3[channel].paused = TRUE;
                streamsAt3[channel].stopReason = PSPAALIB_STOP_END_OF_STREAM;
                UnlockAt3();
                memset(buf, 0, byteLength);
                return PSPAALIB_WARNING_END_OF_STREAM_REACHED;
            }
            ResetAt3(channel);
        }
        streamsAt3[channel].buf = (unsigned short *)realloc(streamsAt3[channel].buf, streamsAt3[channel].bufSize + (4 * num));
        memcpy((void *)streamsAt3[channel].buf + streamsAt3[channel].bufSize, streamsAt3[channel].tempBuf, 4 * num);
        streamsAt3[channel].bufSize += 4 * num;
    }
    for (i = 0;i < 2 * length;i++) {
        buf[i] = streamsAt3[channel].buf[i] * amp;
    }
    streamsAt3[channel].bufSize -= byteLength;
    memmove((void *)streamsAt3[channel].buf, (void *)streamsAt3[channel].buf + byteLength, streamsAt3[channel].bufSize);
    UnlockAt3();
    return PSPAALIB_SUCCESS;
}

int LoadAt3(char *filename, int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    if (streamsAt3[channel].initialized) {
        UnloadAt3(channel);
    }
    SceUID file = sceIoOpen(filename, PSP_O_RDONLY, 0777);
    if (file <= 0) {
        return PSPAALIB_ERROR_AT3_INVALID_FILE;
    }
    streamsAt3[channel].dataSize = sceIoLseek(file, 0, PSP_SEEK_END);
    sceIoLseek(file, 0, PSP_SEEK_SET);
    streamsAt3[channel].data = malloc(streamsAt3[channel].dataSize);
    if (!streamsAt3[channel].data) {
        sceIoClose(file);
        return PSPAALIB_ERROR_AT3_INSUFFICIENT_RAM;
    }
    sceIoRead(file, streamsAt3[channel].data, streamsAt3[channel].dataSize);
    sceIoClose(file);
    streamsAt3[channel].id = sceAtracSetDataAndGetID(streamsAt3[channel].data, streamsAt3[channel].dataSize);
    if (streamsAt3[channel].id < 0) {
        free(streamsAt3[channel].data);
        sceIoClose(file);
        return PSPAALIB_ERROR_AT3_GET_ID;
    }
    streamsAt3[channel].bufSize = 0;
    streamsAt3[channel].paused = TRUE;
    streamsAt3[channel].autoloop = FALSE;
    streamsAt3[channel].initialized = TRUE;
    streamsAt3[channel].stopReason = PSPAALIB_STOP_JUST_LOADED;
    return PSPAALIB_SUCCESS;
}

int UnloadAt3(int channel) {
    if ((channel < 0) || (channel > 1)) {
        return PSPAALIB_ERROR_AT3_INVALID_CHANNEL;
    }
    LockAt3();
    streamsAt3[channel].initialized = FALSE;
    streamsAt3[channel].paused = TRUE;
    streamsAt3[channel].stopReason = PSPAALIB_STOP_UNLOADED;

    if (streamsAt3[channel].id >= 0) {
        sceAtracReleaseAtracID(streamsAt3[channel].id);
        streamsAt3[channel].id = -1;
    }

    free(streamsAt3[channel].data);
    streamsAt3[channel].data = NULL;
    free(streamsAt3[channel].buf);
    streamsAt3[channel].buf = NULL;
    streamsAt3[channel].paused = TRUE;
    UnlockAt3();

    return PSPAALIB_SUCCESS;
}

int InitAt3() {
    sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);
    if (at3Lock < 0)
        at3Lock = sceKernelCreateSema("aalibAt3", 0, 1, 1, NULL);
    // No stream is loaded: no ID to release (0 is a valid ATRAC ID).
    streamsAt3[0].id = -1;
    streamsAt3[1].id = -1;
    return PSPAALIB_SUCCESS;
}
