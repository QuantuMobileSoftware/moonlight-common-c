#include "Limelight-internal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <stdlib.h>

#define DECODER_BUFFER_SIZE 92*1024
#define FF_INPUT_BUFFER_PADDING_SIZE 32
static int h264streamfd = 0;
static char ffmpeg_buffer[DECODER_BUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];

static int fakeDrSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    char* h264stream = getenv("RAW_H264_STREAM");
    if (h264stream != NULL) {
        h264streamfd = open(h264stream, O_WRONLY);
        if (h264streamfd == -1) {
            fprintf(stderr, "Couldn't open FIFO\n");
            return -1;
        }
    }
    
    return 0;
}
static void fakeDrStart(void) {}
static void fakeDrStop(void) {}
static void fakeDrCleanup(void) {
    if (h264streamfd != 0) {
        close(h264streamfd);
        h264streamfd = 0;
    }
}
static int fakeDrSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    if (decodeUnit->fullLength < DECODER_BUFFER_SIZE) {
        PLENTRY entry = decodeUnit->bufferList;
        int length = 0;
        while (entry != NULL) {
            memcpy(ffmpeg_buffer + length, entry->data, entry->length);
            length += entry->length;
            entry = entry->next;
        }

        if (h264streamfd != 0) {
            ssize_t left = length;
            while (left > 0) {
                ssize_t written = write(h264streamfd, ffmpeg_buffer + (length - left), left);
                if (written == -1) {
                    fprintf(stderr, "Couldn't write to FIFO\n");
                    break;
                }
                left -= written;
            }
        }
    } else {
        fprintf(stderr, "Video decode buffer too small");
    }
    return DR_OK;
}
static DECODER_RENDERER_CALLBACKS fakeDrCallbacks = {
    .setup = fakeDrSetup,
    .start = fakeDrStart,
    .stop = fakeDrStop,
    .cleanup = fakeDrCleanup,
    .submitDecodeUnit = fakeDrSubmitDecodeUnit,
};

static int fakeArInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) { return 0; }
static void fakeArStart(void) {}
static void fakeArStop(void) {}
static void fakeArCleanup(void) {}
static void fakeArDecodeAndPlaySample(char* sampleData, int sampleLength) {}

AUDIO_RENDERER_CALLBACKS fakeArCallbacks = {
    .init = fakeArInit,
    .start = fakeArStart,
    .stop = fakeArStop,
    .cleanup = fakeArCleanup,
    .decodeAndPlaySample = fakeArDecodeAndPlaySample,
};

static void fakeClStageStarting(int stage) {}
static void fakeClStageComplete(int stage) {}
static void fakeClStageFailed(int stage, long errorCode) {}
static void fakeClConnectionStarted(void) {}
static void fakeClConnectionTerminated(long errorCode) {}
static void fakeClDisplayMessage(const char* message) {}
static void fakeClDisplayTransientMessage(const char* message) {}
static void fakeClLogMessage(const char* format, ...) {}

static CONNECTION_LISTENER_CALLBACKS fakeClCallbacks = {
    .stageStarting = fakeClStageStarting,
    .stageComplete = fakeClStageComplete,
    .stageFailed = fakeClStageFailed,
    .connectionStarted = fakeClConnectionStarted,
    .connectionTerminated = fakeClConnectionTerminated,
    .displayMessage = fakeClDisplayMessage,
    .displayTransientMessage = fakeClDisplayTransientMessage,
    .logMessage = fakeClLogMessage,
};

void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* drCallbacks, PAUDIO_RENDERER_CALLBACKS* arCallbacks,
    PCONNECTION_LISTENER_CALLBACKS* clCallbacks)
{
    if (*drCallbacks == NULL) {
        *drCallbacks = &fakeDrCallbacks;
    }
    else {
        if ((*drCallbacks)->setup == NULL) {
            (*drCallbacks)->setup = fakeDrSetup;
        }
        if ((*drCallbacks)->start == NULL) {
            (*drCallbacks)->start = fakeDrStart;
        }
        if ((*drCallbacks)->stop == NULL) {
            (*drCallbacks)->stop = fakeDrStop;
        }
        if ((*drCallbacks)->cleanup == NULL) {
            (*drCallbacks)->cleanup = fakeDrCleanup;
        }
        if ((*drCallbacks)->submitDecodeUnit == NULL) {
            (*drCallbacks)->submitDecodeUnit = fakeDrSubmitDecodeUnit;
        }
    }

    if (*arCallbacks == NULL) {
        *arCallbacks = &fakeArCallbacks;
    }
    else {
        if ((*arCallbacks)->init == NULL) {
            (*arCallbacks)->init = fakeArInit;
        }
        if ((*arCallbacks)->start == NULL) {
            (*arCallbacks)->start = fakeArStart;
        }
        if ((*arCallbacks)->stop == NULL) {
            (*arCallbacks)->stop = fakeArStop;
        }
        if ((*arCallbacks)->cleanup == NULL) {
            (*arCallbacks)->cleanup = fakeArCleanup;
        }
        if ((*arCallbacks)->decodeAndPlaySample == NULL) {
            (*arCallbacks)->decodeAndPlaySample = fakeArDecodeAndPlaySample;
        }
    }

    if (*clCallbacks == NULL) {
        *clCallbacks = &fakeClCallbacks;
    }
    else {
        if ((*clCallbacks)->stageStarting == NULL) {
            (*clCallbacks)->stageStarting = fakeClStageStarting;
        }
        if ((*clCallbacks)->stageComplete == NULL) {
            (*clCallbacks)->stageComplete = fakeClStageComplete;
        }
        if ((*clCallbacks)->stageFailed == NULL) {
            (*clCallbacks)->stageFailed = fakeClStageFailed;
        }
        if ((*clCallbacks)->connectionStarted == NULL) {
            (*clCallbacks)->connectionStarted = fakeClConnectionStarted;
        }
        if ((*clCallbacks)->connectionTerminated == NULL) {
            (*clCallbacks)->connectionTerminated = fakeClConnectionTerminated;
        }
        if ((*clCallbacks)->displayMessage == NULL) {
            (*clCallbacks)->displayMessage = fakeClDisplayMessage;
        }
        if ((*clCallbacks)->displayTransientMessage == NULL) {
            (*clCallbacks)->displayTransientMessage = fakeClDisplayTransientMessage;
        }
        if ((*clCallbacks)->logMessage == NULL) {
            (*clCallbacks)->logMessage = fakeClLogMessage;
        }
    }
}
