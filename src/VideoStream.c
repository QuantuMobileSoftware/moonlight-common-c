#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "RtpFecQueue.h"

static int rtp_forward_fd= 0;
static PLT_MUTEX rtp_forward_addr_lock;
static struct sockaddr_in rtp_forward_addr;
static uint8_t is_rtp_forward_addr_set = 0;

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define RTP_PORT 47998
#define FIRST_FRAME_PORT 47996

#define RTP_RECV_BUFFER (512 * 1024)

static RTP_FEC_QUEUE rtpQueue;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10


// Initialize the video stream
void initializeVideoStream(void) {
    char* rtp_server_port = getenv("RTP_SERVER_VIDEO_PORT");
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
    
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpfInitializeQueue(&rtpQueue); //TODO RTP_QUEUE_DELAY
}

// Clean up the video stream
void destroyVideoStream(void) {
    destroyVideoDepacketizer();
    RtpfCleanupQueue(&rtpQueue);
    if (rtp_forward_fd != 0) {
        close(rtp_forward_fd);
        PltDeleteMutex(&rtp_forward_addr_lock);
    }
}

// UDP Ping proc
static void UdpPingThreadProc(void* context) {
    char pingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    struct sockaddr_in6 saddr;
    SOCK_RET err;

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    saddr.sin6_port = htons(RTP_PORT);

    while (!PltIsThreadInterrupted(&udpPingThread)) {
        err = sendto(rtpSocket, pingData, sizeof(pingData), 0, (struct sockaddr*)&saddr, RemoteAddrLen);
        if (err != sizeof(pingData)) {
            Limelog("Video Ping: send() failed: %d\n", (int)LastSocketError());
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

// Receive thread proc
static void ReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize;
    char* buffer;
    int queueStatus;
    PRTPFEC_QUEUE_ENTRY queueEntry;

    receiveSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    bufferSize = receiveSize + sizeof(RTPFEC_QUEUE_ENTRY);
    buffer = NULL;

    while (!PltIsThreadInterrupted(&receiveThread)) {
        PRTP_PACKET packet;

        if (buffer == NULL) {
            buffer = (char*)malloc(bufferSize);
            if (buffer == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                return;
            }
        }

        err = recvUdpSocket(rtpSocket, buffer, receiveSize);
        if (err < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketError());
            break;
        }
        else if  (err == 0) {
            // Receive timed out; try again
            continue;
        }
        
        if (rtp_forward_fd > 0) {
            PltLockMutex(&rtp_forward_addr_lock);
            if (is_rtp_forward_addr_set) {
                if (sendto(rtp_forward_fd, buffer, err, 0, (struct sockaddr*) &rtp_forward_addr, sizeof (rtp_forward_addr)) == -1) {
                    Limelog("RTP forward failed: %d\n", (int) LastSocketError());
                }
            }
            PltLockMutex(&rtp_forward_addr_lock);
            continue;
        }

        // RTP sequence number must be in host order for the RTP queue
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = htons(packet->sequenceNumber);

        queueStatus = RtpfAddPacket(&rtpQueue, packet, err, (PRTPFEC_QUEUE_ENTRY)&buffer[receiveSize]);
        if (queueStatus == RTPF_RET_QUEUED_PACKETS_READY) {
            // The packet queue now has packets ready
            buffer = NULL;
            while ((queueEntry = RtpfGetQueuedPacket(&rtpQueue)) != NULL) {
                queueRtpPacket(queueEntry);
                free(queueEntry->packet);
            }
        }
        else if (queueStatus == RTPF_RET_QUEUED_NOTHING_READY) {
            // The queue owns the buffer
            buffer = NULL;
        }
    }

    if (buffer != NULL) {
        free(buffer);
    }
}

// Decoder thread proc
static void DecoderThreadProc(void* context) {
    PQUEUED_DECODE_UNIT qdu;
    while (!PltIsThreadInterrupted(&decoderThread)) {
        if (!getNextQueuedDecodeUnit(&qdu)) {
            return;
        }

        int ret = VideoCallbacks.submitDecodeUnit(&qdu->decodeUnit);

        freeQueuedDecodeUnit(qdu);

        if (ret == DR_NEED_IDR) {
            Limelog("Requesting IDR frame on behalf of DR\n");
            requestDecoderRefresh();
        }
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();
    
    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltJoinThread(&decoderThread);
    }

    PltCloseThread(&udpPingThread);
    PltCloseThread(&receiveThread);
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltCloseThread(&decoderThread);
    }
    
    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, RTP_RECV_BUFFER);
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }

    VideoCallbacks.start();

    err = PltCreateThread(ReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        closeSocket(rtpSocket);
        VideoCallbacks.cleanup();
        return err;
    }

    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        err = PltCreateThread(DecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            PltCloseThread(&receiveThread);
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, RemoteAddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            VideoCallbacks.stop();
            stopVideoDepacketizer();
            PltInterruptThread(&receiveThread);
            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                PltInterruptThread(&decoderThread);
            }
            PltJoinThread(&receiveThread);
            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                PltJoinThread(&decoderThread);
            }
            PltCloseThread(&receiveThread);
            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                PltCloseThread(&decoderThread);
            }
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return LastSocketError();
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread(UdpPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
            PltJoinThread(&decoderThread);
        }
        PltCloseThread(&receiveThread);
        if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
            PltCloseThread(&decoderThread);
        }
        closeSocket(rtpSocket);
        if (firstFrameSocket != INVALID_SOCKET) {
            closeSocket(firstFrameSocket);
            firstFrameSocket = INVALID_SOCKET;
        }
        VideoCallbacks.cleanup();
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}
