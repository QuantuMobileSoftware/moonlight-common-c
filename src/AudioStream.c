#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "LinkedBlockingQueue.h"
#include "RtpReorderQueue.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>

static int rtp_forward_fd = 0;
static PLT_MUTEX rtp_forward_addr_lock;
static struct sockaddr_in rtp_forward_addr;
static uint8_t is_rtp_forward_addr_set = 0;
static uint32_t start_timestamp;

static SOCKET rtpSocket = INVALID_SOCKET;

static LINKED_BLOCKING_QUEUE packetQueue;
static RTP_REORDER_QUEUE rtpReorderQueue;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static unsigned short lastSeq;

#define RTP_PORT 48000

#define MAX_PACKET_SIZE 400

// This is much larger than we should typically have buffered, but
// it needs to be. We need a cushion in case our thread gets blocked
// for longer than normal.
#define RTP_RECV_BUFFER (64 * 1024)

#define SAMPLE_RATE 48000

static OPUS_MULTISTREAM_CONFIGURATION opusStereoConfig = {
    .sampleRate = SAMPLE_RATE,
    .channelCount = 2,
    .streams = 1,
    .coupledStreams = 1,
    .mapping = {0, 1}
};

static OPUS_MULTISTREAM_CONFIGURATION opus51SurroundConfig = {
    .sampleRate = SAMPLE_RATE,
    .channelCount = 6,
    .streams = 4,
    .coupledStreams = 2,
    .mapping = {0, 4, 1, 5, 2, 3}
};

static POPUS_MULTISTREAM_CONFIGURATION opusConfigArray[] = {
    &opusStereoConfig,
    &opus51SurroundConfig,
};

typedef struct _QUEUED_AUDIO_PACKET {
    // data must remain at the front
    char data[MAX_PACKET_SIZE];

    int size;
    union {
        RTP_QUEUE_ENTRY rentry;
        LINKED_BLOCKING_QUEUE_ENTRY lentry;
    } q;
} QUEUED_AUDIO_PACKET, *PQUEUED_AUDIO_PACKET;

// Initialize the audio stream
void initializeAudioStream(void) {
    char* rtp_server_port = getenv("RTP_SERVER_AUDIO_PORT");
    if (rtp_server_port != NULL) {
        PltCreateMutex(&rtp_forward_addr_lock);
        rtp_forward_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_forward_fd == -1) {
            perror("Cannot open socket");
            exit(1);
        }
        struct sockaddr_in rtp_server_addr;
        memset(&rtp_server_addr, 0, sizeof (rtp_server_addr));
        rtp_server_addr.sin_family = AF_INET;
        rtp_server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        rtp_server_addr.sin_port = htons(atoi(rtp_server_port));
        if (bind(rtp_forward_fd, (struct sockaddr*) &rtp_server_addr, sizeof (rtp_server_addr)) == -1) {
            perror("Cannot bind socket");
            exit(1);
        }
    }
    char* rtp_forward_port = getenv("RTP_FORWARD_AUDIO_PORT");
    if (rtp_forward_port != NULL && rtp_forward_fd == 0) {
        PltCreateMutex(&rtp_forward_addr_lock);
        rtp_forward_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_forward_fd == -1) {
            perror("Cannot open socket");
            exit(1);
        }
        memset(&rtp_forward_addr, 0, sizeof (rtp_forward_addr));
        rtp_forward_addr.sin_family = AF_INET;
        rtp_forward_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        rtp_forward_addr.sin_port = htons(atoi(rtp_forward_port));
        start_timestamp = time(NULL) * 960;
        is_rtp_forward_addr_set = 1;
    }
    
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        LbqInitializeLinkedBlockingQueue(&packetQueue, 30);
    }
    RtpqInitializeQueue(&rtpReorderQueue, RTPQ_DEFAULT_MAX_SIZE, RTPQ_DEFAULT_QUEUE_TIME);
    lastSeq = 0;
}

static void freePacketList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored within the data allocation
        free(entry->data);

        entry = nextEntry;
    }
}

// Tear down the audio stream once we're done with it
void destroyAudioStream(void) {
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        freePacketList(LbqDestroyLinkedBlockingQueue(&packetQueue));
    }
    RtpqCleanupQueue(&rtpReorderQueue);
    if (rtp_forward_fd != 0) {
        close(rtp_forward_fd);
        PltDeleteMutex(&rtp_forward_addr_lock);
    }
}

static void UdpPingThreadProc(void* context) {
    // Ping in ASCII
    char pingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    struct sockaddr_in6 saddr;
    SOCK_RET err;

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    saddr.sin6_port = htons(RTP_PORT);

    // Send PING every 500 milliseconds
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        err = sendto(rtpSocket, pingData, sizeof(pingData), 0, (struct sockaddr*)&saddr, RemoteAddrLen);
        if (err != sizeof(pingData)) {
            Limelog("Audio Ping: sendto() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketError());
            return;
        }
        
        if (rtp_forward_fd != 0) {
            char buf[5];
            struct sockaddr_in tmp_addr;
            socklen_t peer_addr_len = sizeof (tmp_addr);
            ssize_t nread = recvfrom(rtp_forward_fd, buf, sizeof (buf), MSG_DONTWAIT,
                    (struct sockaddr *) &tmp_addr, &peer_addr_len);
            if (nread != -1) {
                PltLockMutex(&rtp_forward_addr_lock);
                rtp_forward_addr = tmp_addr;
                is_rtp_forward_addr_set = 1;
                PltLockMutex(&rtp_forward_addr_lock);
            }
        }

        PltSleepMs(500);
    }
}

static int queuePacketToLbq(PQUEUED_AUDIO_PACKET* packet) {
    int err;

    err = LbqOfferQueueItem(&packetQueue, *packet, &(*packet)->q.lentry);
    if (err == LBQ_SUCCESS) {
        // The LBQ owns the buffer now
        *packet = NULL;
    }
    else if (err == LBQ_BOUND_EXCEEDED) {
        Limelog("Audio packet queue overflow\n");
        freePacketList(LbqFlushQueueItems(&packetQueue));
    }
    else if (err == LBQ_INTERRUPTED) {
        return 0;
    }

    return 1;
}

static void decodeInputData(PQUEUED_AUDIO_PACKET packet) {
    PRTP_PACKET rtp;

    rtp = (PRTP_PACKET)&packet->data[0];
    if (lastSeq != 0 && (unsigned short)(lastSeq + 1) != rtp->sequenceNumber) {
        Limelog("Received OOS audio data (expected %d, but got %d)\n", lastSeq + 1, rtp->sequenceNumber);

        AudioCallbacks.decodeAndPlaySample(NULL, 0);
    }

    lastSeq = rtp->sequenceNumber;

    AudioCallbacks.decodeAndPlaySample((char*)(rtp + 1), packet->size - sizeof(*rtp));
}

static void ReceiveThreadProc(void* context) {
    PRTP_PACKET rtp;
    PQUEUED_AUDIO_PACKET packet;
    int queueStatus;

    packet = NULL;

    while (!PltIsThreadInterrupted(&receiveThread)) {
        if (packet == NULL) {
            packet = (PQUEUED_AUDIO_PACKET)malloc(sizeof(*packet));
            if (packet == NULL) {
                Limelog("Audio Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }

        packet->size = recvUdpSocket(rtpSocket, &packet->data[0], MAX_PACKET_SIZE);
        if (packet->size < 0) {
            Limelog("Audio Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketError());
            break;
        }
        else if (packet->size == 0) {
            // Receive timed out; try again
            continue;
        }

        if (packet->size < sizeof(RTP_PACKET)) {
            // Runt packet
            continue;
        }

        rtp = (PRTP_PACKET)&packet->data[0];
        if (rtp->packetType != 97) {
            // Not audio
            continue;
        }

        if (rtp_forward_fd > 0) {
            PltLockMutex(&rtp_forward_addr_lock);
            if (is_rtp_forward_addr_set) {
                uint32_t timestamp = ntohl(*(uint32_t*)&rtp->reserved[0]);
                *(uint32_t*)&rtp->reserved[0] = htonl(start_timestamp + timestamp / 5 * 240);
                if (sendto(rtp_forward_fd, packet->data, packet->size, 0, (struct sockaddr*) &rtp_forward_addr, sizeof (rtp_forward_addr)) == -1) {
                    Limelog("RTP forward failed: %d\n", (int) LastSocketError());
                }
            }
            PltLockMutex(&rtp_forward_addr_lock);
            continue;
        }

        // RTP sequence number must be in host order for the RTP queue
        rtp->sequenceNumber = htons(rtp->sequenceNumber);

        queueStatus = RtpqAddPacket(&rtpReorderQueue, (PRTP_PACKET)packet, &packet->q.rentry);
        if (queueStatus == RTPQ_RET_HANDLE_IMMEDIATELY) {
            if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                if (!queuePacketToLbq(&packet)) {
                    // An exit signal was received
                    break;
                }
            }
            else {
                decodeInputData(packet);
            }
        }
        else {
            if (queueStatus != RTPQ_RET_REJECTED) {
                // The queue consumed our packet, so we must allocate a new one
                packet = NULL;
            }

            if (queueStatus == RTPQ_RET_QUEUED_PACKETS_READY) {
                // If packets are ready, pull them and send them to the decoder
                while ((packet = (PQUEUED_AUDIO_PACKET)RtpqGetQueuedPacket(&rtpReorderQueue)) != NULL) {
                    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                        if (!queuePacketToLbq(&packet)) {
                            // An exit signal was received
                            break;
                        }
                    }
                    else {
                        decodeInputData(packet);
                        free(packet);
                    }
                }
                
                // Break on exit
                if (packet != NULL) {
                    break;
                }
            }
        }
    }
    
    if (packet != NULL) {
        free(packet);
    }
}

static void DecoderThreadProc(void* context) {
    int err;
    PQUEUED_AUDIO_PACKET packet;

    while (!PltIsThreadInterrupted(&decoderThread)) {
        err = LbqWaitForQueueElement(&packetQueue, (void**)&packet);
        if (err != LBQ_SUCCESS) {
            // An exit signal was received
            return;
        }

        decodeInputData(packet);

        free(packet);
    }
}

void stopAudioStream(void) {
    AudioCallbacks.stop();

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {        
        // Signal threads waiting on the LBQ
        LbqSignalQueueShutdown(&packetQueue);
        PltInterruptThread(&decoderThread);
    }
    
    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltJoinThread(&decoderThread);
    }

    PltCloseThread(&udpPingThread);
    PltCloseThread(&receiveThread);
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltCloseThread(&decoderThread);
    }
    
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    AudioCallbacks.cleanup();
}

int startAudioStream(void* audioContext, int arFlags) {
    int err;

    err = AudioCallbacks.init(StreamConfig.audioConfiguration,
        opusConfigArray[StreamConfig.audioConfiguration], audioContext, arFlags);
    if (err != 0) {
        return err;
    }

    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, RTP_RECV_BUFFER);
    if (rtpSocket == INVALID_SOCKET) {
        err = LastSocketFail();
        AudioCallbacks.cleanup();
        return err;
    }

    err = PltCreateThread(UdpPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        AudioCallbacks.cleanup();
        closeSocket(rtpSocket);
        return err;
    }

    AudioCallbacks.start();

    err = PltCreateThread(ReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        AudioCallbacks.stop();
        PltInterruptThread(&udpPingThread);
        PltJoinThread(&udpPingThread);
        PltCloseThread(&udpPingThread);
        closeSocket(rtpSocket);
        AudioCallbacks.cleanup();
        return err;
    }

    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        err = PltCreateThread(DecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            AudioCallbacks.stop();
            PltInterruptThread(&udpPingThread);
            PltInterruptThread(&receiveThread);
            PltJoinThread(&udpPingThread);
            PltJoinThread(&receiveThread);
            PltCloseThread(&udpPingThread);
            PltCloseThread(&receiveThread);
            closeSocket(rtpSocket);
            AudioCallbacks.cleanup();
            return err;
        }
    }

    return 0;
}
