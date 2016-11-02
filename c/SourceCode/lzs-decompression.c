/*****************************************************************************
 *
 * \file
 *
 * \brief LZS Decompression
 *
 * This implements LZS (Lempel-Ziv-Stac) decompression, which is an LZ77
 * derived algorithm with a 2kB sliding window and Huffman coding.
 *
 * See:
 *     * ANSI X3.241-1994
 *     * RFC 1967
 *     * RFC 1974
 *     * RFC 2395
 *     * RFC 3943
 *
 ****************************************************************************/


/*****************************************************************************
 * Includes
 ****************************************************************************/

#include "lzs.h"

#include <stdint.h>


/*****************************************************************************
 * Defines
 ****************************************************************************/

#define SHORT_OFFSET_BITS           7u
#define LONG_OFFSET_BITS            11u
#define BIT_QUEUE_BITS              32u

#if (MAX_HISTORY_SIZE < ((1u << LONG_OFFSET_BITS) - 1u))
#error MAX_HISTORY_SIZE is too small
#endif

#define LENGTH_MAX_BIT_WIDTH        4u
#define MAX_EXTENDED_LENGTH         15u

#define LENGTH_DECODE_METHOD_CODE   0
#define LENGTH_DECODE_METHOD_TABLE  1u
// Choose which method to use
#define LENGTH_DECODE_METHOD        LENGTH_DECODE_METHOD_TABLE

//#define LZS_DEBUG(X)    printf X
#define LZS_DEBUG(X)

#define LZS_ASSERT(X)


/*****************************************************************************
 * Typedefs
 ****************************************************************************/

typedef enum
{
    DECOMPRESS_NORMAL,
    DECOMPRESS_EXTENDED
} SimpleDecompressState_t;

typedef enum
{
    DECOMPRESS_COPY_DATA,           // Must come before DECOMPRESS_GET_TOKEN_TYPE, so state transition can be done by increment
    DECOMPRESS_GET_TOKEN_TYPE,
    DECOMPRESS_GET_LITERAL,
    DECOMPRESS_GET_OFFSET_TYPE,
    DECOMPRESS_GET_OFFSET_SHORT,
    DECOMPRESS_GET_OFFSET_LONG,
    DECOMPRESS_GET_LENGTH,
    DECOMPRESS_COPY_EXTENDED_DATA,  // Must come before DECOMPRESS_GET_EXTENDED_LENGTH, so state transition can be done by increment
    DECOMPRESS_GET_EXTENDED_LENGTH,

    NUM_DECOMPRESS_STATES
} LzsDecompressState_t;


/*****************************************************************************
 * Tables
 ****************************************************************************/

#if LENGTH_DECODE_METHOD == LENGTH_DECODE_METHOD_TABLE
#define MAX_INITIAL_LENGTH          8u      // keep in sync with lengthDecodeTable[]

static const uint8_t lengthDecodeTable[(1u << LENGTH_MAX_BIT_WIDTH)] =
{
    /* Length is encoded as:
     *  0b00 --> 2
     *  0b01 --> 3
     *  0b10 --> 4
     *  0b1100 --> 5
     *  0b1101 --> 6
     *  0b1110 --> 7
     *  0b1111 xxxx --> 8 (extended)
     */
    // Look at 4 bits. Map 0bWXYZ to a length value, and a number of bits actually used for symbol.
    // High 4 bits are length value. Low 4 bits are the width of the bit field.
    0x22, 0x22, 0x22, 0x22,     // 0b00 --> 2
    0x32, 0x32, 0x32, 0x32,     // 0b01 --> 3
    0x42, 0x42, 0x42, 0x42,     // 0b10 --> 4
    0x54, 0x64, 0x74, 0x84,     // 0b11xy --> 5, 6, 7, and also 8 (see MAX_INITIAL_LENGTH) which goes into extended lengths
};
#endif


static const uint_fast8_t StateBitMinimumWidth[NUM_DECOMPRESS_STATES] =
{
    0,                          // DECOMPRESS_COPY_DATA,
    1u,                         // DECOMPRESS_GET_TOKEN_TYPE,
    8u,                         // DECOMPRESS_GET_LITERAL,
    1u,                         // DECOMPRESS_GET_OFFSET_TYPE,
    SHORT_OFFSET_BITS,          // DECOMPRESS_GET_OFFSET_SHORT,
    LONG_OFFSET_BITS,           // DECOMPRESS_GET_OFFSET_LONG,
    0,                          // DECOMPRESS_GET_LENGTH,
    0,                          // DECOMPRESS_COPY_EXTENDED_DATA,
    LENGTH_MAX_BIT_WIDTH,       // DECOMPRESS_GET_EXTENDED_LENGTH,
};


/*****************************************************************************
 * Functions
 ****************************************************************************/

/*
 * Single-call decompression
 *
 * No state is kept between calls. Decompression is expected to complete in a single call.
 * It will stop if/when it reaches the end of either the input or the output buffer.
 */
size_t lzs_decompress(uint8_t * a_pOutData, size_t a_outBufferSize, const uint8_t * a_pInData, size_t a_inLen)
{
    const uint8_t     * inPtr;
    uint8_t           * outPtr;
    size_t              inRemaining;        // Count of remaining bytes of input
    size_t              outCount;           // Count of output bytes that have been generated
    uint32_t            bitFieldQueue;      // Code assumes bits will disappear past MS-bit 31 when shifted left.
    int_fast8_t         bitFieldQueueLen;
    uint_fast16_t       offset;
    uint_fast8_t        length;
    uint8_t             temp8;
    SimpleDecompressState_t state;


    bitFieldQueue = 0;
    bitFieldQueueLen = 0;
    inPtr = a_pInData;
    outPtr = a_pOutData;
    inRemaining = a_inLen;
    outCount = 0;
    state = DECOMPRESS_NORMAL;

    for (;;)
    {
        // Load input data into the bit field queue
        while ((inRemaining > 0) && (bitFieldQueueLen <= BIT_QUEUE_BITS - 8u))
        {
            bitFieldQueue |= (*inPtr++ << (BIT_QUEUE_BITS - 8u - bitFieldQueueLen));
            bitFieldQueueLen += 8u;
            LZS_DEBUG(("Load queue: %04X\n", bitFieldQueue));
            inRemaining--;
        }
        // Check if we've reached the end of our input data
        if (bitFieldQueueLen <= 0)
        {
            LZS_ASSERT(bitFieldQueueLen >= 0);
            break;
        }
        // Check if we've run out of output buffer space
        if (outCount >= a_outBufferSize)
        {
            break;
        }

        switch (state)
        {
            case DECOMPRESS_NORMAL:
                // Get token-type bit
                //      0 means literal byte
                //      1 means offset/length token

                // We don't need to check bitFieldQueueLen here because
                // we already checked above that there is at least 1 bit.
                temp8 = (bitFieldQueue & (1u << (BIT_QUEUE_BITS - 1u))) ? 1u : 0;
                bitFieldQueue <<= 1u;
                bitFieldQueueLen--;
                if (temp8 == 0)
                {
                    // Literal
                    if (bitFieldQueueLen < 8u)
                    {
                        break;
                    }
                    temp8 = (uint8_t) (bitFieldQueue >> (BIT_QUEUE_BITS - 8u));
                    bitFieldQueue <<= 8u;
                    bitFieldQueueLen -= 8u;
                    LZS_DEBUG(("Literal %c\n", temp8));

                    // Write to output
                    // Not necessary to check for space, because that was done at the top of the main loop.
                    *outPtr++ = temp8;
                    outCount++;
                }
                else
                {
                    // Offset+length token
                    // Decode offset
                    if (bitFieldQueueLen < 1u)
                    {
                        break;
                    }
                    temp8 = (bitFieldQueue & (1u << (BIT_QUEUE_BITS - 1u))) ? 1u : 0;
                    bitFieldQueue <<= 1u;
                    bitFieldQueueLen--;
                    if (temp8)
                    {
                        // Short offset
                        if (bitFieldQueueLen < SHORT_OFFSET_BITS)
                        {
                            break;
                        }
                        offset = bitFieldQueue >> (BIT_QUEUE_BITS - SHORT_OFFSET_BITS);
                        bitFieldQueue <<= SHORT_OFFSET_BITS;
                        bitFieldQueueLen -= SHORT_OFFSET_BITS;
                        if (offset == 0)
                        {
                            LZS_DEBUG(("End marker\n"));
                            // Discard any bits that are fractions of a byte, to align with a byte boundary
                            temp8 = bitFieldQueueLen % 8u;
                            bitFieldQueue <<= temp8;
                            bitFieldQueueLen -= temp8;
                        }
                    }
                    else
                    {
                        // Long offset
                        if (bitFieldQueueLen < LONG_OFFSET_BITS)
                        {
                            break;
                        }
                        offset = bitFieldQueue >> (BIT_QUEUE_BITS - LONG_OFFSET_BITS);
                        bitFieldQueue <<= LONG_OFFSET_BITS;
                        bitFieldQueueLen -= LONG_OFFSET_BITS;
                    }
                    if (offset != 0)
                    {
                        // Decode length and copy characters
#if LENGTH_DECODE_METHOD == LENGTH_DECODE_METHOD_CODE
                        /* Length is encoded as:
                         *  0b00 --> 2
                         *  0b01 --> 3
                         *  0b10 --> 4
                         *  0b1100 --> 5
                         *  0b1101 --> 6
                         *  0b1110 --> 7
                         *  0b1111 xxxx --> 8 (extended)
                         */
                        // Get 4 bits
                        temp8 = (uint8_t) (bitFieldQueue >> (BIT_QUEUE_BITS - 4u));
                        if (temp8 < 0xC)    // 0xC is 0b1100
                        {
                            // Length of 2, 3 or 4, encoded in 2 bits
                            if (bitFieldQueueLen < 2u)
                            {
                                break;
                            }
                            length = (temp8 >> 2u) + 2u;
                            bitFieldQueue <<= 2u;
                            bitFieldQueueLen -= 2u;
                        }
                        else
                        {
                            // Length (encoded in 4 bits) of 5, 6, 7, or (8 + extended)
                            if (bitFieldQueueLen < 4u)
                            {
                                break;
                            }
                            length = (temp8 - 0xC + 5u);
                            bitFieldQueue <<= 4u;
                            bitFieldQueueLen -= 4u;
                            if (length == 8u)
                            {
                                // We must go into extended length decode mode
                                state = DECOMPRESS_EXTENDED;
                            }
                        }
#endif
#if LENGTH_DECODE_METHOD == LENGTH_DECODE_METHOD_TABLE
                        // Get 4 bits, then look up decode data
                        temp8 = lengthDecodeTable[
                                                  (uint8_t) (bitFieldQueue >> (BIT_QUEUE_BITS - LENGTH_MAX_BIT_WIDTH))
                                                 ];
                        // Length value is in upper nibble
                        length = temp8 >> 4u;
                        // Number of bits for this length token is in the lower nibble
                        temp8 &= 0xF;
                        if (bitFieldQueueLen < temp8)
                        {
                            break;
                        }
                        bitFieldQueue <<= temp8;
                        bitFieldQueueLen -= temp8;
                        if (length == MAX_INITIAL_LENGTH)
                        {
                            // We must go into extended length decode mode
                            state = DECOMPRESS_EXTENDED;
                        }
#endif
                        LZS_DEBUG(("(%d, %d)\n", offset, length));
                        // Now copy (offset, length) bytes
                        for (temp8 = 0; temp8 < length; temp8++)
                        {
                            // Check offset is within range of valid history.
                            // If it's not, then write zeros. Avoid information leak.
                            if (outPtr - offset >= a_pOutData)
                            {
                                *outPtr = *(outPtr - offset);
                            }
                            else
                            {
                                *outPtr = 0;
                            }
                            ++outPtr;
                            ++outCount;

                            if (outCount >= a_outBufferSize)
                            {
                                break;
                            }
                        }
                    }
                }
                break;

            case DECOMPRESS_EXTENDED:
                // Extended length token
                // Get 4 bits
                if (bitFieldQueueLen < LENGTH_MAX_BIT_WIDTH)
                {
                    break;
                }
                length = (uint8_t) (bitFieldQueue >> (BIT_QUEUE_BITS - LENGTH_MAX_BIT_WIDTH));
                bitFieldQueue <<= LENGTH_MAX_BIT_WIDTH;
                bitFieldQueueLen -= LENGTH_MAX_BIT_WIDTH;
                // Now copy (offset, length) bytes
                for (temp8 = 0; temp8 < length; temp8++)
                {
                    // Check offset is within range of valid history.
                    // If it's not, then write zeros. Avoid information leak.
                    if (outPtr - offset >= a_pOutData)
                    {
                        *outPtr = *(outPtr - offset);
                    }
                    else
                    {
                        *outPtr = 0;
                    }
                    ++outPtr;
                    ++outCount;

                    if (outCount >= a_outBufferSize)
                    {
                        break;
                    }
                }
                if (length != MAX_EXTENDED_LENGTH)
                {
                    // We're finished with extended length decode mode; go back to normal
                    state = DECOMPRESS_NORMAL;
                }
                break;
        }
    }

    return outCount;
}


/*
 * \brief Initialise incremental decompression
 */
void lzs_decompress_init(LzsDecompressParameters_t * pParams)
{
    pParams->status = LZS_D_STATUS_NONE;
    pParams->bitFieldQueue = 0;
    pParams->bitFieldQueueLen = 0;
    pParams->state = DECOMPRESS_GET_TOKEN_TYPE;
    pParams->historyLatestIdx = 0;
    pParams->historySize = 0;
}


/*
 * \brief Incremental decompression
 *
 * State is kept between calls, so decompression can be done gradually, and flexibly
 * depending on the application's needs for input/output buffer handling.
 *
 * It will stop if/when it reaches the end of either the input or the output buffer.
 * It will also stop if/when it reaches an end marker.
 */
size_t lzs_decompress_incremental(LzsDecompressParameters_t * pParams)
{
    size_t              outCount;           // Count of output bytes that have been generated
    uint_fast16_t       offset;
    uint_fast8_t        temp8;


    pParams->status = LZS_D_STATUS_NONE;
    outCount = 0;

    for (;;)
    {
        // Load input data into the bit field queue
        while ((pParams->inLength > 0) && (pParams->bitFieldQueueLen <= BIT_QUEUE_BITS - 8u))
        {
            pParams->bitFieldQueue |= (*pParams->inPtr++ << (BIT_QUEUE_BITS - 8u - pParams->bitFieldQueueLen));
            pParams->bitFieldQueueLen += 8u;
            LZS_DEBUG(("Load queue: %04X\n", pParams->bitFieldQueue));
            pParams->inLength--;
        }
        // Check if we've reached the end of our input data
        if (pParams->bitFieldQueueLen <= 0)
        {
            LZS_ASSERT(pParams->bitFieldQueueLen >= 0);     // It should never go negative. That is a bug.
            pParams->status |= LZS_D_STATUS_INPUT_FINISHED | LZS_D_STATUS_INPUT_STARVED;
        }
        // Check if we have enough input data to do something useful
        if (pParams->bitFieldQueueLen < StateBitMinimumWidth[pParams->state])
        {
            // We don't have enough input bits, so we're done for now.
            pParams->status |= LZS_D_STATUS_INPUT_STARVED;
        }

        // Check if we need to finish for whatever reason
        if (pParams->status != LZS_D_STATUS_NONE)
        {
            // Break out of the top-level loop
            break;
        }

        // Process input data in a state machine
        switch (pParams->state)
        {
            case DECOMPRESS_GET_TOKEN_TYPE:
                // Get token-type bit
                if (pParams->bitFieldQueue & (1u << (BIT_QUEUE_BITS - 1u)))
                {
                    pParams->state = DECOMPRESS_GET_OFFSET_TYPE;
                }
                else
                {
                    pParams->state = DECOMPRESS_GET_LITERAL;
                }
                pParams->bitFieldQueue <<= 1u;
                pParams->bitFieldQueueLen--;
                break;

            case DECOMPRESS_GET_LITERAL:
                // Literal
                // Check if we have space in the output buffer
                if (pParams->outLength == 0)
                {
                    pParams->status |= LZS_D_STATUS_NO_OUTPUT_BUFFER_SPACE;
                }
                else
                {
                    temp8 = (uint8_t) (pParams->bitFieldQueue >> (BIT_QUEUE_BITS - 8u));
                    pParams->bitFieldQueue <<= 8u;
                    pParams->bitFieldQueueLen -= 8u;
                    LZS_DEBUG(("Literal %c\n", temp8));

                    *pParams->outPtr++ = temp8;
                    pParams->outLength--;
                    outCount++;

                    // Write to history
                    pParams->historyPtr[pParams->historyLatestIdx] = temp8;

                    // Increment write index, and wrap if necessary
                    pParams->historyLatestIdx++;
                    if (pParams->historyLatestIdx >= pParams->historyBufferSize)
                    {
                        pParams->historyLatestIdx = 0;
                    }

                    pParams->state = DECOMPRESS_GET_TOKEN_TYPE;
                }
                break;

            case DECOMPRESS_GET_OFFSET_TYPE:
                // Offset+length token
                // Decode offset
                temp8 = (pParams->bitFieldQueue & (1u << (BIT_QUEUE_BITS - 1u))) ? 1u : 0;
                pParams->bitFieldQueue <<= 1u;
                pParams->bitFieldQueueLen--;
                if (temp8)
                {
                    pParams->state = DECOMPRESS_GET_OFFSET_SHORT;
                }
                else
                {
                    pParams->state = DECOMPRESS_GET_OFFSET_LONG;
                }
                break;

            case DECOMPRESS_GET_OFFSET_SHORT:
                // Short offset
                offset = pParams->bitFieldQueue >> (BIT_QUEUE_BITS - SHORT_OFFSET_BITS);
                pParams->bitFieldQueue <<= SHORT_OFFSET_BITS;
                pParams->bitFieldQueueLen -= SHORT_OFFSET_BITS;
                if (offset == 0)
                {
                    LZS_DEBUG(("End marker\n"));
                    // Discard any bits that are fractions of a byte, to align with a byte boundary
                    temp8 = pParams->bitFieldQueueLen % 8u;
                    pParams->bitFieldQueue <<= temp8;
                    pParams->bitFieldQueueLen -= temp8;

                    // Set status saying we found an end marker
                    pParams->status |= LZS_D_STATUS_END_MARKER;

                    pParams->state = DECOMPRESS_GET_TOKEN_TYPE;
                }
                else
                {
                    pParams->offset = offset;
                    pParams->state = DECOMPRESS_GET_LENGTH;
                }
                break;

        case DECOMPRESS_GET_OFFSET_LONG:
                // Long offset
                pParams->offset = pParams->bitFieldQueue >> (BIT_QUEUE_BITS - LONG_OFFSET_BITS);
                pParams->bitFieldQueue <<= LONG_OFFSET_BITS;
                pParams->bitFieldQueueLen -= LONG_OFFSET_BITS;

                pParams->state = DECOMPRESS_GET_LENGTH;
                break;

            case DECOMPRESS_GET_LENGTH:
                // Decode length and copy characters
#if LENGTH_DECODE_METHOD == LENGTH_DECODE_METHOD_CODE
                // TODO: fill this in
#endif
#if LENGTH_DECODE_METHOD == LENGTH_DECODE_METHOD_TABLE
                // Get 4 bits, then look up decode data
                temp8 = lengthDecodeTable[
                                          pParams->bitFieldQueue >> (BIT_QUEUE_BITS - LENGTH_MAX_BIT_WIDTH)
                                         ];
                // Length value is in upper nibble
                pParams->length = temp8 >> 4u;
                // Number of bits for this length token is in the lower nibble
                temp8 &= 0xF;
#endif
                if (pParams->bitFieldQueueLen < temp8)
                {
                    // We don't have enough input bits, so we're done for now.
                    pParams->status |= LZS_D_STATUS_INPUT_STARVED;
                }
                else
                {
                    pParams->bitFieldQueue <<= temp8;
                    pParams->bitFieldQueueLen -= temp8;
                    if (pParams->length == MAX_INITIAL_LENGTH)
                    {
                        // We must go into extended length decode mode
                        pParams->state = DECOMPRESS_COPY_EXTENDED_DATA;
                    }
                    else
                    {
                        pParams->state = DECOMPRESS_COPY_DATA;
                    }

                    // Do some offset calculations before beginning to copy
                    offset = pParams->offset;
                    LZS_ASSERT(offset <= pParams->historyBufferSize);
#if 0
                    // This code is a "safe" version (no overflows as long as pParams->historyBufferSize < MAX_UINT16/2)
                    if (offset > pParams->historyLatestIdx)
                    {
                        pParams->historyReadIdx = pParams->historyLatestIdx + pParams->historyBufferSize - offset;
                    }
                    else
                    {
                        pParams->historyReadIdx = pParams->historyLatestIdx - offset;
                    }
#else
                    // This code is simpler, but relies on calculation overflows wrapping as expected.
                    if (offset > pParams->historyLatestIdx)
                    {
                        // This relies on two overflows of uint (during the two subtractions) cancelling out to a sensible value
                        offset -= pParams->historyBufferSize;
                    }
                    pParams->historyReadIdx = pParams->historyLatestIdx - offset;
#endif
                    LZS_DEBUG(("(%d, %d)\n", offset, pParams->length));
                }
                break;

            case DECOMPRESS_COPY_DATA:
            case DECOMPRESS_COPY_EXTENDED_DATA:
                // Copy (offset, length) bytes.
                // Offset has already been used to calculate pParams->historyReadIdx.
                for (;;)
                {
                    if (pParams->length == 0)
                    {
                        // We're finished copying. Change state, and exit this inner copying loop.
                        pParams->state++;   // Goes to either DECOMPRESS_GET_TOKEN_TYPE or DECOMPRESS_GET_EXTENDED_LENGTH
                        break;
                    }
                    // Check if we have space in the output buffer
                    if (pParams->outLength == 0)
                    {
                        // We're out of space in the output buffer.
                        // Set status, exit this inner copying loop, but maintain the current state.
                        pParams->status |= LZS_D_STATUS_NO_OUTPUT_BUFFER_SPACE;
                        break;
                    }

                    // Get byte from history
                    temp8 = (pParams->historyPtr)[pParams->historyReadIdx];

                    // Increment read index, and wrap if necessary
                    pParams->historyReadIdx++;
                    if (pParams->historyReadIdx >= pParams->historyBufferSize)
                    {
                        pParams->historyReadIdx = 0;
                    }

                    // Write to output
                    *pParams->outPtr++ = temp8;
                    pParams->outLength--;
                    pParams->length--;
                    ++outCount;

                    // Write to history
                    pParams->historyPtr[pParams->historyLatestIdx] = temp8;

                    // Increment write index, and wrap if necessary
                    pParams->historyLatestIdx++;
                    if (pParams->historyLatestIdx >= pParams->historyBufferSize)
                    {
                        pParams->historyLatestIdx = 0;
                    }
                }
                break;

            case DECOMPRESS_GET_EXTENDED_LENGTH:
                // Extended length token
                // Get 4 bits
                pParams->length = (uint8_t) (pParams->bitFieldQueue >> (BIT_QUEUE_BITS - LENGTH_MAX_BIT_WIDTH));
                pParams->bitFieldQueue <<= LENGTH_MAX_BIT_WIDTH;
                pParams->bitFieldQueueLen -= LENGTH_MAX_BIT_WIDTH;
                if (pParams->length == MAX_EXTENDED_LENGTH)
                {
                    // We stay in extended length decode mode
                    pParams->state = DECOMPRESS_COPY_EXTENDED_DATA;
                }
                else
                {
                    // We're finished with extended length decode mode; go back to normal
                    pParams->state = DECOMPRESS_COPY_DATA;
                }
                break;

            default:
                // TODO: It is an error if we ever get here. Need to do some handling.
                LZS_ASSERT(0);
                break;
        }
    }

    return outCount;
}
