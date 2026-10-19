/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * TPIU Decoder Module
 * ===================
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "generics.h"          /* EOL / V_WARN / genericsTimestampuS() */
/* MSVC 的传统预处理器不接受 GCC 的 `#define f(x...)` 变参宏写法，用标准形式。
   本文件的告警由驱动统一走 ORB_LOG，这里直接盖掉。
   **顺序要紧**：必须在 include generics.h 之后，否则会把头里的函数声明一起吃掉。 */
#define genericsReport(...)
#include "tpiuDecoder.h"


#define SYNCPATTERN 0xFFFFFF7F           // Full sync indicator
#define HALFSYNC_HIGH 0x7F               // Halfsync indicator, last arriving byte
#define HALFSYNC_LOW  0XFF               // Halfsync indicator, first arriving byte
#define NO_CHANNEL_CHANGE (0xFF)         // Flag to not change channel
#define TPIU_TIMEOUT_US (200000U)        // Note that this timeout must be less than 1sec (<1000000 us)
#define STAT_SYNC_BYTE (0xA6)            // Sync header for status

// ====================================================================================================
struct TPIUDecoder *TPIUDecoderCreate( void )

{
    struct TPIUDecoder *i = ( struct TPIUDecoder * )calloc( 1, sizeof( struct TPIUDecoder ) );
    i->selfAllocated = true;
    return i;
}
// ====================================================================================================
void TPIUDecoderInit( struct TPIUDecoder *t )

/* Reset a TPIUDecoder instance */

{
    if ( !t )
    {
        /* C 允许 void* 隐式转对象指针，C++ 不允许 —— 显式转换。 */
        t = ( struct TPIUDecoder * )calloc( 1, sizeof( struct TPIUDecoder ) );
        t->selfAllocated = true;
    }

    t->state = TPIU_UNSYNCED;
    t->syncMonitor = 0;
    TPIUDecoderZeroStats( t );
}
// ====================================================================================================
void TPIUDecoderZeroStats( struct TPIUDecoder *t )

{
    memset( &t->stats, 0, sizeof( struct TPIUDecoderStats ) );
}
// ====================================================================================================
bool TPIUDecoderSynced( struct TPIUDecoder *t )
{
    return t->state != TPIU_UNSYNCED;
}
// ====================================================================================================
struct TPIUDecoderStats *TPIUDecoderGetStats( struct TPIUDecoder *t )
{
    return &t->stats;
}
// ====================================================================================================
void TPIUDecoderForceSync( struct TPIUDecoder *t, uint8_t offset )

/* Force the decoder into a specific sync state */

{
    if ( t->state == TPIU_UNSYNCED )
    {
        t->stats.syncCount++;
    }

    t->state = TPIU_RXING;
    t->byteCount = offset;

    /* Consider this a valid timestamp */
    t->lastPacketUs = genericsTimestampuS();
}
// ====================================================================================================
bool _getPacket( struct TPIUDecoder *t, struct TPIUPacket *p )

/* Copy received packet into transfer buffer, and reset receiver */

{
    uint8_t delayedStreamChange = NO_CHANNEL_CHANGE;

    /* This should have been reset in the call */
    if ( ( t->byteCount ) || ( !p ) )
    {
        return false;
    }

    p->len = 0;
    uint8_t lowbits = t->rxedPacket[TPIU_PACKET_LEN - 1];

    for ( uint32_t i = 0; i < TPIU_PACKET_LEN; i += 2 )
    {
        if ( ( t->rxedPacket[i] ) & 1 )
        {
            /* This is a stream change - either before or after the data byte */
            if ( lowbits & 1 )
            {
                delayedStreamChange = t->rxedPacket[i] >> 1;
            }
            else
            {
                t->currentStream = t->rxedPacket[i] >> 1;
            }
        }
        else
        {
            /* This is a data byte - store it, provided it's not padding */
            if ( t->currentStream )
            {
                p->packet[p->len].d = t->rxedPacket[i] | ( lowbits & 1 );
                p->packet[p->len].s = t->currentStream;
                p->len++;
            }
        }

        /* Now deal with the second byte of the pair */
        if ( i < 14 )
        {
            /* Now deal with the other byte of the pair ... this is always data */
            if ( t->currentStream )
            {
                p->packet[p->len].d = t->rxedPacket[i + 1];
                p->packet[p->len].s = t->currentStream;
                p->len++;
            }
        }

        /* ... and finally, if there's a delayed channel change, deal with it */
        if ( delayedStreamChange != NO_CHANNEL_CHANGE )
        {
            t->currentStream = delayedStreamChange;
            delayedStreamChange = NO_CHANNEL_CHANGE;
        }

        /* Make sure we accomodate the lowbit for the next two bytes */
        lowbits >>= 1;
    }

    return true;
}
// ====================================================================================================
struct TPIUCommsStats *TPIUGetCommsStats( struct TPIUDecoder *t )

{
    return &t->commsStats;
}

// ====================================================================================================
void _decodeCommsStats( struct TPIUDecoder *t )

/* Decode received communication stats into transfer buffer */

{
    t->commsStats.pendingCount = ( t->rxedPacket[2] << 8 ) | t->rxedPacket[1];
    t->commsStats.leds         = t->rxedPacket[5];
    t->commsStats.lostFrames   = ( t->rxedPacket[7] << 8 ) | t->rxedPacket[6];
    t->commsStats.totalFrames  = ( t->rxedPacket[11] << 24 ) | ( t->rxedPacket[10] << 16 ) | ( t->rxedPacket[9] << 8 ) | ( t->rxedPacket[8] );
}
// ====================================================================================================
void TPIUPump( struct TPIUDecoder *t, uint8_t *frame, int len,
               void ( *packetRxed )( enum TPIUPumpEvent e, struct TPIUPacket *p, void *param ),
               void *param )


/* Assemble this packet into TPIU frames and call them back */

{
    uint8_t d;
    struct TPIUPacket _packet;

    /* 包间隔看门狗。orbuculum 用 gettimeofday()/struct timeval（POSIX，MSVC
     * 没有），这里换成 genericsTimestampuS() 的微秒计数 —— 逻辑不变。
     * lastPacketUs == 0 表示"还没收到过包"，也就是原来的 tv_sec == 0。 */
    const uint64_t nowUs = genericsTimestampuS();

    /* If it excedes the keepalive time then it's not validly synced */
    if ( t->lastPacketUs && ( ( nowUs - t->lastPacketUs ) > TPIU_TIMEOUT_US ) )
    {
        /* There was a legal value for last time...this is not the startup case */
        genericsReport( V_WARN, ">>>>>>>>> PACKET INTERVAL TOO LONG <<<<<<<<<<<<<<" EOL );
        t->stats.lostSync++;

        t->state = TPIU_UNSYNCED;
        packetRxed( TPIU_EV_UNSYNCED, NULL, param );
    }

    t->lastPacketUs = nowUs;

    /* Now process the packet */
    while ( len-- )
    {
        d = *frame++;
        t->syncMonitor = ( t->syncMonitor << 8 ) | d;

        /* ----------------------------------------------------------------------------------- */
        /* First case : This is a sync pattern. If so then process it, then move to next octet */
        if ( t->syncMonitor == SYNCPATTERN )
        {
            packetRxed( ( t->state == TPIU_UNSYNCED ) ? TPIU_EV_NEWSYNC : TPIU_EV_SYNCED, NULL, param );

            /* Deal with the special state that these are communication stats from the link */
            /* ...it is still a reset though!                                               */
            if ( ( t->byteCount == 14 ) && ( t->rxedPacket[0] == STAT_SYNC_BYTE ) )
            {
                _decodeCommsStats( t );
            }

            t->state = TPIU_RXING;
            t->stats.syncCount++;
            t->byteCount = 0;
            t->got_lowbits = false;
            genericsReport( V_DEBUG, "!!!! " EOL );

            /* Consider this a valid timestamp */
            t->lastPacketUs = genericsTimestampuS();
            continue;
        }

        /* ----------------------------------------------- */
        /* Second case : We're not synced, just move along */
        if ( t->state == TPIU_UNSYNCED )
        {
            continue;
        }

        /* ------------------------------------------------------------------------------- */
        /* Otherwise : Process this into a frame, and deal with the frame if it's complete */

        // We collect in sets of 16 bits, in order to filter halfsyncs (0x7fff)
        if ( !t->got_lowbits )
        {
            t->got_lowbits = true;
            t->rxedPacket[t->byteCount] = d;
            continue;
        }

        t->got_lowbits = false;

        if ( ( d == HALFSYNC_HIGH ) && ( t->rxedPacket[t->byteCount] == HALFSYNC_LOW ) )
        {
            // A halfsync, waste of space, to be ignored
            t->stats.halfSyncCount++;
            continue;
        }

        // Pre-increment for the low byte we already got, post increment for this one
        genericsReport( V_DEBUG, "[%02x %02x] ", t->rxedPacket[t->byteCount], d );
        t->byteCount++;
        t->rxedPacket[t->byteCount++] = d;

        if ( t->byteCount == TPIU_PACKET_LEN )
        {
            t->stats.packets++;
            t->byteCount = 0;
            genericsReport( V_DEBUG, EOL );

            if ( _getPacket( t, &_packet ) )
            {
                packetRxed( TPIU_EV_RXEDPACKET, &_packet, param );
            }
        }

        continue;
    }
}
// ====================================================================================================
