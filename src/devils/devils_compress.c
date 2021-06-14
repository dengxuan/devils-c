/** 
 @file compress.c
 @brief An adaptive order-2 PPM range coder
*/
#define DEVILS_BUILDING_LIB 1
#include <string.h>
#include "include/devils.h"

typedef struct _devils_symbol
{
    /* binary indexed tree of symbols */
    devils_uint8 value;
    devils_uint8 count;
    devils_uint16 under;
    devils_uint16 left, right;

    /* context defined by this symbol */
    devils_uint16 symbols;
    devils_uint16 escapes;
    devils_uint16 total;
    devils_uint16 parent;
} devils_symbol;

/* adaptation constants tuned aggressively for small packet sizes rather than large file compression */
enum
{
    DEVILS_RANGE_CODER_TOP = 1 << 24,
    DEVILS_RANGE_CODER_BOTTOM = 1 << 16,

    DEVILS_CONTEXT_SYMBOL_DELTA = 3,
    DEVILS_CONTEXT_SYMBOL_MINIMUM = 1,
    DEVILS_CONTEXT_ESCAPE_MINIMUM = 1,

    DEVILS_SUBCONTEXT_ORDER = 2,
    DEVILS_SUBCONTEXT_SYMBOL_DELTA = 2,
    DEVILS_SUBCONTEXT_ESCAPE_DELTA = 5
};

/* context exclusion roughly halves compression speed, so disable for now */
#undef DEVILS_CONTEXT_EXCLUSION

typedef struct _devils_range_coder
{
    /* only allocate enough symbols for reasonable MTUs, would need to be larger for large file compression */
    devils_symbol symbols[4096];
} devils_range_coder;

void *
devils_range_coder_create(void)
{
    devils_range_coder *rangeCoder = (devils_range_coder *)devils_malloc(sizeof(devils_range_coder));
    if (rangeCoder == NULL)
        return NULL;

    return rangeCoder;
}

void devils_range_coder_destroy(void *context)
{
    devils_range_coder *rangeCoder = (devils_range_coder *)context;
    if (rangeCoder == NULL)
        return;

    devils_free(rangeCoder);
}

#define DEVILS_SYMBOL_CREATE(symbol, value_, count_) \
    {                                                \
        symbol = &rangeCoder->symbols[nextSymbol++]; \
        symbol->value = value_;                      \
        symbol->count = count_;                      \
        symbol->under = count_;                      \
        symbol->left = 0;                            \
        symbol->right = 0;                           \
        symbol->symbols = 0;                         \
        symbol->escapes = 0;                         \
        symbol->total = 0;                           \
        symbol->parent = 0;                          \
    }

#define DEVILS_CONTEXT_CREATE(context, escapes_, minimum) \
    {                                                     \
        DEVILS_SYMBOL_CREATE(context, 0, 0);              \
        (context)->escapes = escapes_;                    \
        (context)->total = escapes_ + 256 * minimum;      \
        (context)->symbols = 0;                           \
    }

static devils_uint16
devils_symbol_rescale(devils_symbol *symbol)
{
    devils_uint16 total = 0;
    for (;;)
    {
        symbol->count -= symbol->count >> 1;
        symbol->under = symbol->count;
        if (symbol->left)
            symbol->under += devils_symbol_rescale(symbol + symbol->left);
        total += symbol->under;
        if (!symbol->right)
            break;
        symbol += symbol->right;
    }
    return total;
}

#define DEVILS_CONTEXT_RESCALE(context, minimum)                                                           \
    {                                                                                                      \
        (context)->total = (context)->symbols ? devils_symbol_rescale((context) + (context)->symbols) : 0; \
        (context)->escapes -= (context)->escapes >> 1;                                                     \
        (context)->total += (context)->escapes + 256 * minimum;                                            \
    }

#define DEVILS_RANGE_CODER_OUTPUT(value) \
    {                                    \
        if (outData >= outEnd)           \
            return 0;                    \
        *outData++ = value;              \
    }

#define DEVILS_RANGE_CODER_ENCODE(under, count, total)                             \
    {                                                                              \
        encodeRange /= (total);                                                    \
        encodeLow += (under)*encodeRange;                                          \
        encodeRange *= (count);                                                    \
        for (;;)                                                                   \
        {                                                                          \
            if ((encodeLow ^ (encodeLow + encodeRange)) >= DEVILS_RANGE_CODER_TOP) \
            {                                                                      \
                if (encodeRange >= DEVILS_RANGE_CODER_BOTTOM)                      \
                    break;                                                         \
                encodeRange = -encodeLow & (DEVILS_RANGE_CODER_BOTTOM - 1);        \
            }                                                                      \
            DEVILS_RANGE_CODER_OUTPUT(encodeLow >> 24);                            \
            encodeRange <<= 8;                                                     \
            encodeLow <<= 8;                                                       \
        }                                                                          \
    }

#define DEVILS_RANGE_CODER_FLUSH                        \
    {                                                   \
        while (encodeLow)                               \
        {                                               \
            DEVILS_RANGE_CODER_OUTPUT(encodeLow >> 24); \
            encodeLow <<= 8;                            \
        }                                               \
    }

#define DEVILS_RANGE_CODER_FREE_SYMBOLS                                                                  \
    {                                                                                                    \
        if (nextSymbol >= sizeof(rangeCoder->symbols) / sizeof(devils_symbol) - DEVILS_SUBCONTEXT_ORDER) \
        {                                                                                                \
            nextSymbol = 0;                                                                              \
            DEVILS_CONTEXT_CREATE(root, DEVILS_CONTEXT_ESCAPE_MINIMUM, DEVILS_CONTEXT_SYMBOL_MINIMUM);   \
            predicted = 0;                                                                               \
            order = 0;                                                                                   \
        }                                                                                                \
    }

#define DEVILS_CONTEXT_ENCODE(context, symbol_, value_, under_, count_, update, minimum) \
    {                                                                                    \
        under_ = value * minimum;                                                        \
        count_ = minimum;                                                                \
        if (!(context)->symbols)                                                         \
        {                                                                                \
            DEVILS_SYMBOL_CREATE(symbol_, value_, update);                               \
            (context)->symbols = symbol_ - (context);                                    \
        }                                                                                \
        else                                                                             \
        {                                                                                \
            devils_symbol *node = (context) + (context)->symbols;                        \
            for (;;)                                                                     \
            {                                                                            \
                if (value_ < node->value)                                                \
                {                                                                        \
                    node->under += update;                                               \
                    if (node->left)                                                      \
                    {                                                                    \
                        node += node->left;                                              \
                        continue;                                                        \
                    }                                                                    \
                    DEVILS_SYMBOL_CREATE(symbol_, value_, update);                       \
                    node->left = symbol_ - node;                                         \
                }                                                                        \
                else if (value_ > node->value)                                           \
                {                                                                        \
                    under_ += node->under;                                               \
                    if (node->right)                                                     \
                    {                                                                    \
                        node += node->right;                                             \
                        continue;                                                        \
                    }                                                                    \
                    DEVILS_SYMBOL_CREATE(symbol_, value_, update);                       \
                    node->right = symbol_ - node;                                        \
                }                                                                        \
                else                                                                     \
                {                                                                        \
                    count_ += node->count;                                               \
                    under_ += node->under - node->count;                                 \
                    node->under += update;                                               \
                    node->count += update;                                               \
                    symbol_ = node;                                                      \
                }                                                                        \
                break;                                                                   \
            }                                                                            \
        }                                                                                \
    }

#ifdef DEVILS_CONTEXT_EXCLUSION
static const devils_symbol emptyContext = {0, 0, 0, 0, 0, 0, 0, 0, 0};

#define DEVILS_CONTEXT_WALK(context, body)                          \
    {                                                               \
        const devils_symbol *node = (context) + (context)->symbols; \
        const devils_symbol *stack[256];                            \
        size_t stackSize = 0;                                       \
        while (node->left)                                          \
        {                                                           \
            stack[stackSize++] = node;                              \
            node += node->left;                                     \
        }                                                           \
        for (;;)                                                    \
        {                                                           \
            body;                                                   \
            if (node->right)                                        \
            {                                                       \
                node += node->right;                                \
                while (node->left)                                  \
                {                                                   \
                    stack[stackSize++] = node;                      \
                    node += node->left;                             \
                }                                                   \
            }                                                       \
            else if (stackSize <= 0)                                \
                break;                                              \
            else                                                    \
                node = stack[--stackSize];                          \
        }                                                           \
    }

#define DEVILS_CONTEXT_ENCODE_EXCLUDE(context, value_, under, total, minimum)                                  \
    DEVILS_CONTEXT_WALK(context,                                                                               \
                        {                                                                                      \
                            if (node->value != value_)                                                         \
                            {                                                                                  \
                                devils_uint16 parentCount = rangeCoder->symbols[node->parent].count + minimum; \
                                if (node->value < value_)                                                      \
                                    under -= parentCount;                                                      \
                                total -= parentCount;                                                          \
                            }                                                                                  \
                        })
#endif

size_t
devils_range_coder_compress(void *context, const devils_buffer *inBuffers, size_t inBufferCount, size_t inLimit, devils_uint8 *outData, size_t outLimit)
{
    devils_range_coder *rangeCoder = (devils_range_coder *)context;
    devils_uint8 *outStart = outData, *outEnd = &outData[outLimit];
    const devils_uint8 *inData, *inEnd;
    devils_uint32 encodeLow = 0, encodeRange = ~0;
    devils_symbol *root;
    devils_uint16 predicted = 0;
    size_t order = 0, nextSymbol = 0;

    if (rangeCoder == NULL || inBufferCount <= 0 || inLimit <= 0)
        return 0;

    inData = (const devils_uint8 *)inBuffers->data;
    inEnd = &inData[inBuffers->dataLength];
    inBuffers++;
    inBufferCount--;

    DEVILS_CONTEXT_CREATE(root, DEVILS_CONTEXT_ESCAPE_MINIMUM, DEVILS_CONTEXT_SYMBOL_MINIMUM);

    for (;;)
    {
        devils_symbol *subcontext, *symbol;
#ifdef DEVILS_CONTEXT_EXCLUSION
        const devils_symbol *childContext = &emptyContext;
#endif
        devils_uint8 value;
        devils_uint16 count, under, *parent = &predicted, total;
        if (inData >= inEnd)
        {
            if (inBufferCount <= 0)
                break;
            inData = (const devils_uint8 *)inBuffers->data;
            inEnd = &inData[inBuffers->dataLength];
            inBuffers++;
            inBufferCount--;
        }
        value = *inData++;

        for (subcontext = &rangeCoder->symbols[predicted];
             subcontext != root;
#ifdef DEVILS_CONTEXT_EXCLUSION
             childContext = subcontext,
#endif
            subcontext = &rangeCoder->symbols[subcontext->parent])
        {
            DEVILS_CONTEXT_ENCODE(subcontext, symbol, value, under, count, DEVILS_SUBCONTEXT_SYMBOL_DELTA, 0);
            *parent = symbol - rangeCoder->symbols;
            parent = &symbol->parent;
            total = subcontext->total;
#ifdef DEVILS_CONTEXT_EXCLUSION
            if (childContext->total > DEVILS_SUBCONTEXT_SYMBOL_DELTA + DEVILS_SUBCONTEXT_ESCAPE_DELTA)
                DEVILS_CONTEXT_ENCODE_EXCLUDE(childContext, value, under, total, 0);
#endif
            if (count > 0)
            {
                DEVILS_RANGE_CODER_ENCODE(subcontext->escapes + under, count, total);
            }
            else
            {
                if (subcontext->escapes > 0 && subcontext->escapes < total)
                    DEVILS_RANGE_CODER_ENCODE(0, subcontext->escapes, total);
                subcontext->escapes += DEVILS_SUBCONTEXT_ESCAPE_DELTA;
                subcontext->total += DEVILS_SUBCONTEXT_ESCAPE_DELTA;
            }
            subcontext->total += DEVILS_SUBCONTEXT_SYMBOL_DELTA;
            if (count > 0xFF - 2 * DEVILS_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > DEVILS_RANGE_CODER_BOTTOM - 0x100)
                DEVILS_CONTEXT_RESCALE(subcontext, 0);
            if (count > 0)
                goto nextInput;
        }

        DEVILS_CONTEXT_ENCODE(root, symbol, value, under, count, DEVILS_CONTEXT_SYMBOL_DELTA, DEVILS_CONTEXT_SYMBOL_MINIMUM);
        *parent = symbol - rangeCoder->symbols;
        parent = &symbol->parent;
        total = root->total;
#ifdef DEVILS_CONTEXT_EXCLUSION
        if (childContext->total > DEVILS_SUBCONTEXT_SYMBOL_DELTA + DEVILS_SUBCONTEXT_ESCAPE_DELTA)
            DEVILS_CONTEXT_ENCODE_EXCLUDE(childContext, value, under, total, DEVILS_CONTEXT_SYMBOL_MINIMUM);
#endif
        DEVILS_RANGE_CODER_ENCODE(root->escapes + under, count, total);
        root->total += DEVILS_CONTEXT_SYMBOL_DELTA;
        if (count > 0xFF - 2 * DEVILS_CONTEXT_SYMBOL_DELTA + DEVILS_CONTEXT_SYMBOL_MINIMUM || root->total > DEVILS_RANGE_CODER_BOTTOM - 0x100)
            DEVILS_CONTEXT_RESCALE(root, DEVILS_CONTEXT_SYMBOL_MINIMUM);

    nextInput:
        if (order >= DEVILS_SUBCONTEXT_ORDER)
            predicted = rangeCoder->symbols[predicted].parent;
        else
            order++;
        DEVILS_RANGE_CODER_FREE_SYMBOLS;
    }

    DEVILS_RANGE_CODER_FLUSH;

    return (size_t)(outData - outStart);
}

#define DEVILS_RANGE_CODER_SEED            \
    {                                      \
        if (inData < inEnd)                \
            decodeCode |= *inData++ << 24; \
        if (inData < inEnd)                \
            decodeCode |= *inData++ << 16; \
        if (inData < inEnd)                \
            decodeCode |= *inData++ << 8;  \
        if (inData < inEnd)                \
            decodeCode |= *inData++;       \
    }

#define DEVILS_RANGE_CODER_READ(total) ((decodeCode - decodeLow) / (decodeRange /= (total)))

#define DEVILS_RANGE_CODER_DECODE(under, count, total)                             \
    {                                                                              \
        decodeLow += (under)*decodeRange;                                          \
        decodeRange *= (count);                                                    \
        for (;;)                                                                   \
        {                                                                          \
            if ((decodeLow ^ (decodeLow + decodeRange)) >= DEVILS_RANGE_CODER_TOP) \
            {                                                                      \
                if (decodeRange >= DEVILS_RANGE_CODER_BOTTOM)                      \
                    break;                                                         \
                decodeRange = -decodeLow & (DEVILS_RANGE_CODER_BOTTOM - 1);        \
            }                                                                      \
            decodeCode <<= 8;                                                      \
            if (inData < inEnd)                                                    \
                decodeCode |= *inData++;                                           \
            decodeRange <<= 8;                                                     \
            decodeLow <<= 8;                                                       \
        }                                                                          \
    }

#define DEVILS_CONTEXT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, createRoot, visitNode, createRight, createLeft) \
    {                                                                                                                                          \
        under_ = 0;                                                                                                                            \
        count_ = minimum;                                                                                                                      \
        if (!(context)->symbols)                                                                                                               \
        {                                                                                                                                      \
            createRoot;                                                                                                                        \
        }                                                                                                                                      \
        else                                                                                                                                   \
        {                                                                                                                                      \
            devils_symbol *node = (context) + (context)->symbols;                                                                              \
            for (;;)                                                                                                                           \
            {                                                                                                                                  \
                devils_uint16 after = under_ + node->under + (node->value + 1) * minimum, before = node->count + minimum;                      \
                visitNode;                                                                                                                     \
                if (code >= after)                                                                                                             \
                {                                                                                                                              \
                    under_ += node->under;                                                                                                     \
                    if (node->right)                                                                                                           \
                    {                                                                                                                          \
                        node += node->right;                                                                                                   \
                        continue;                                                                                                              \
                    }                                                                                                                          \
                    createRight;                                                                                                               \
                }                                                                                                                              \
                else if (code < after - before)                                                                                                \
                {                                                                                                                              \
                    node->under += update;                                                                                                     \
                    if (node->left)                                                                                                            \
                    {                                                                                                                          \
                        node += node->left;                                                                                                    \
                        continue;                                                                                                              \
                    }                                                                                                                          \
                    createLeft;                                                                                                                \
                }                                                                                                                              \
                else                                                                                                                           \
                {                                                                                                                              \
                    value_ = node->value;                                                                                                      \
                    count_ += node->count;                                                                                                     \
                    under_ = after - before;                                                                                                   \
                    node->under += update;                                                                                                     \
                    node->count += update;                                                                                                     \
                    symbol_ = node;                                                                                                            \
                }                                                                                                                              \
                break;                                                                                                                         \
            }                                                                                                                                  \
        }                                                                                                                                      \
    }

#define DEVILS_CONTEXT_TRY_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
    DEVILS_CONTEXT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, return 0, exclude(node->value, after, before), return 0, return 0)

#define DEVILS_CONTEXT_ROOT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
    DEVILS_CONTEXT_DECODE(                                                                                   \
        context, symbol_, code, value_, under_, count_, update, minimum,                                     \
        {                                                                                                    \
            value_ = code / minimum;                                                                         \
            under_ = code - code % minimum;                                                                  \
            DEVILS_SYMBOL_CREATE(symbol_, value_, update);                                                   \
            (context)->symbols = symbol_ - (context);                                                        \
        },                                                                                                   \
        exclude(node->value, after, before),                                                                 \
        {                                                                                                    \
            value_ = node->value + 1 + (code - after) / minimum;                                             \
            under_ = code - (code - after) % minimum;                                                        \
            DEVILS_SYMBOL_CREATE(symbol_, value_, update);                                                   \
            node->right = symbol_ - node;                                                                    \
        },                                                                                                   \
        {                                                                                                    \
            value_ = node->value - 1 - (after - before - code - 1) / minimum;                                \
            under_ = code - (after - before - code - 1) % minimum;                                           \
            DEVILS_SYMBOL_CREATE(symbol_, value_, update);                                                   \
            node->left = symbol_ - node;                                                                     \
        })

#ifdef DEVILS_CONTEXT_EXCLUSION
typedef struct _devils_exclude
{
    devils_uint8 value;
    devils_uint16 under;
} devils_exclude;

#define DEVILS_CONTEXT_DECODE_EXCLUDE(context, total, minimum)                              \
    {                                                                                       \
        devils_uint16 under = 0;                                                            \
        nextExclude = excludes;                                                             \
        DEVILS_CONTEXT_WALK(context,                                                        \
                            {                                                               \
                                under += rangeCoder->symbols[node->parent].count + minimum; \
                                nextExclude->value = node->value;                           \
                                nextExclude->under = under;                                 \
                                nextExclude++;                                              \
                            });                                                             \
        total -= under;                                                                     \
    }

#define DEVILS_CONTEXT_EXCLUDED(value_, after, before)      \
    {                                                       \
        size_t low = 0, high = nextExclude - excludes;      \
        for (;;)                                            \
        {                                                   \
            size_t mid = (low + high) >> 1;                 \
            const devils_exclude *exclude = &excludes[mid]; \
            if (value_ < exclude->value)                    \
            {                                               \
                if (low + 1 < high)                         \
                {                                           \
                    high = mid;                             \
                    continue;                               \
                }                                           \
                if (exclude > excludes)                     \
                    after -= exclude[-1].under;             \
            }                                               \
            else                                            \
            {                                               \
                if (value_ > exclude->value)                \
                {                                           \
                    if (low + 1 < high)                     \
                    {                                       \
                        low = mid;                          \
                        continue;                           \
                    }                                       \
                }                                           \
                else                                        \
                    before = 0;                             \
                after -= exclude->under;                    \
            }                                               \
            break;                                          \
        }                                                   \
    }
#endif

#define DEVILS_CONTEXT_NOT_EXCLUDED(value_, after, before)

size_t
devils_range_coder_decompress(void *context, const devils_uint8 *inData, size_t inLimit, devils_uint8 *outData, size_t outLimit)
{
    devils_range_coder *rangeCoder = (devils_range_coder *)context;
    devils_uint8 *outStart = outData, *outEnd = &outData[outLimit];
    const devils_uint8 *inEnd = &inData[inLimit];
    devils_uint32 decodeLow = 0, decodeCode = 0, decodeRange = ~0;
    devils_symbol *root;
    devils_uint16 predicted = 0;
    size_t order = 0, nextSymbol = 0;
#ifdef DEVILS_CONTEXT_EXCLUSION
    devils_exclude excludes[256];
    devils_exclude *nextExclude = excludes;
#endif

    if (rangeCoder == NULL || inLimit <= 0)
        return 0;

    DEVILS_CONTEXT_CREATE(root, DEVILS_CONTEXT_ESCAPE_MINIMUM, DEVILS_CONTEXT_SYMBOL_MINIMUM);

    DEVILS_RANGE_CODER_SEED;

    for (;;)
    {
        devils_symbol *subcontext, *symbol, *patch;
#ifdef DEVILS_CONTEXT_EXCLUSION
        const devils_symbol *childContext = &emptyContext;
#endif
        devils_uint8 value = 0;
        devils_uint16 code, under, count, bottom, *parent = &predicted, total;

        for (subcontext = &rangeCoder->symbols[predicted];
             subcontext != root;
#ifdef DEVILS_CONTEXT_EXCLUSION
             childContext = subcontext,
#endif
            subcontext = &rangeCoder->symbols[subcontext->parent])
        {
            if (subcontext->escapes <= 0)
                continue;
            total = subcontext->total;
#ifdef DEVILS_CONTEXT_EXCLUSION
            if (childContext->total > 0)
                DEVILS_CONTEXT_DECODE_EXCLUDE(childContext, total, 0);
#endif
            if (subcontext->escapes >= total)
                continue;
            code = DEVILS_RANGE_CODER_READ(total);
            if (code < subcontext->escapes)
            {
                DEVILS_RANGE_CODER_DECODE(0, subcontext->escapes, total);
                continue;
            }
            code -= subcontext->escapes;
#ifdef DEVILS_CONTEXT_EXCLUSION
            if (childContext->total > 0)
            {
                DEVILS_CONTEXT_TRY_DECODE(subcontext, symbol, code, value, under, count, DEVILS_SUBCONTEXT_SYMBOL_DELTA, 0, DEVILS_CONTEXT_EXCLUDED);
            }
            else
#endif
            {
                DEVILS_CONTEXT_TRY_DECODE(subcontext, symbol, code, value, under, count, DEVILS_SUBCONTEXT_SYMBOL_DELTA, 0, DEVILS_CONTEXT_NOT_EXCLUDED);
            }
            bottom = symbol - rangeCoder->symbols;
            DEVILS_RANGE_CODER_DECODE(subcontext->escapes + under, count, total);
            subcontext->total += DEVILS_SUBCONTEXT_SYMBOL_DELTA;
            if (count > 0xFF - 2 * DEVILS_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > DEVILS_RANGE_CODER_BOTTOM - 0x100)
                DEVILS_CONTEXT_RESCALE(subcontext, 0);
            goto patchContexts;
        }

        total = root->total;
#ifdef DEVILS_CONTEXT_EXCLUSION
        if (childContext->total > 0)
            DEVILS_CONTEXT_DECODE_EXCLUDE(childContext, total, DEVILS_CONTEXT_SYMBOL_MINIMUM);
#endif
        code = DEVILS_RANGE_CODER_READ(total);
        if (code < root->escapes)
        {
            DEVILS_RANGE_CODER_DECODE(0, root->escapes, total);
            break;
        }
        code -= root->escapes;
#ifdef DEVILS_CONTEXT_EXCLUSION
        if (childContext->total > 0)
        {
            DEVILS_CONTEXT_ROOT_DECODE(root, symbol, code, value, under, count, DEVILS_CONTEXT_SYMBOL_DELTA, DEVILS_CONTEXT_SYMBOL_MINIMUM, DEVILS_CONTEXT_EXCLUDED);
        }
        else
#endif
        {
            DEVILS_CONTEXT_ROOT_DECODE(root, symbol, code, value, under, count, DEVILS_CONTEXT_SYMBOL_DELTA, DEVILS_CONTEXT_SYMBOL_MINIMUM, DEVILS_CONTEXT_NOT_EXCLUDED);
        }
        bottom = symbol - rangeCoder->symbols;
        DEVILS_RANGE_CODER_DECODE(root->escapes + under, count, total);
        root->total += DEVILS_CONTEXT_SYMBOL_DELTA;
        if (count > 0xFF - 2 * DEVILS_CONTEXT_SYMBOL_DELTA + DEVILS_CONTEXT_SYMBOL_MINIMUM || root->total > DEVILS_RANGE_CODER_BOTTOM - 0x100)
            DEVILS_CONTEXT_RESCALE(root, DEVILS_CONTEXT_SYMBOL_MINIMUM);

    patchContexts:
        for (patch = &rangeCoder->symbols[predicted];
             patch != subcontext;
             patch = &rangeCoder->symbols[patch->parent])
        {
            DEVILS_CONTEXT_ENCODE(patch, symbol, value, under, count, DEVILS_SUBCONTEXT_SYMBOL_DELTA, 0);
            *parent = symbol - rangeCoder->symbols;
            parent = &symbol->parent;
            if (count <= 0)
            {
                patch->escapes += DEVILS_SUBCONTEXT_ESCAPE_DELTA;
                patch->total += DEVILS_SUBCONTEXT_ESCAPE_DELTA;
            }
            patch->total += DEVILS_SUBCONTEXT_SYMBOL_DELTA;
            if (count > 0xFF - 2 * DEVILS_SUBCONTEXT_SYMBOL_DELTA || patch->total > DEVILS_RANGE_CODER_BOTTOM - 0x100)
                DEVILS_CONTEXT_RESCALE(patch, 0);
        }
        *parent = bottom;

        DEVILS_RANGE_CODER_OUTPUT(value);

        if (order >= DEVILS_SUBCONTEXT_ORDER)
            predicted = rangeCoder->symbols[predicted].parent;
        else
            order++;
        DEVILS_RANGE_CODER_FREE_SYMBOLS;
    }

    return (size_t)(outData - outStart);
}

/** @defgroup host ENet host functions
    @{
*/

/** Sets the packet compressor the host should use to the default range coder.
    @param host host to enable the range coder for
    @returns 0 on success, < 0 on failure
*/
int devils_host_compress_with_range_coder(devils_host *host)
{
    devils_compressor compressor;
    memset(&compressor, 0, sizeof(compressor));
    compressor.context = devils_range_coder_create();
    if (compressor.context == NULL)
        return -1;
    compressor.compress = devils_range_coder_compress;
    compressor.decompress = devils_range_coder_decompress;
    compressor.destroy = devils_range_coder_destroy;
    devils_host_compress(host, &compressor);
    return 0;
}

/** @} */
