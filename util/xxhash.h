/*
   xxHash - Fast Hash algorithm
   Header File
   Copyright (C) 2012-2014, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - xxHash source repository : http://code.google.com/p/xxhash/
*/

/* Notice extracted from xxHash homepage :

xxHash is an extremely fast Hash algorithm, running at RAM speed limits.
It also successfully passes all tests from the SMHasher suite.

Comparison (single thread, Windows Seven 32 bits, using SMHasher on a Core 2 Duo @3GHz)

Name            Speed       Q.Score   Author
xxHash          5.4 GB/s     10
CrapWow         3.2 GB/s      2       Andrew
MumurHash 3a    2.7 GB/s     10       Austin Appleby
SpookyHash      2.0 GB/s     10       Bob Jenkins
SBox            1.4 GB/s      9       Bret Mulvey
Lookup3         1.2 GB/s      9       Bob Jenkins
SuperFastHash   1.2 GB/s      1       Paul Hsieh
CityHash64      1.05 GB/s    10       Pike & Alakuijala
FNV             0.55 GB/s     5       Fowler, Noll, Vo
CRC32           0.43 GB/s     9
MD5-32          0.33 GB/s    10       Ronald L. Rivest
SHA1-32         0.28 GB/s    10

Q.Score is a measure of quality of the hash function.
It depends on successfully passing SMHasher test set.
10 is a perfect score.
*/

#pragma once
#include <stdlib.h>
#if !defined(__VMS) &&       \
    (defined(__cplusplus) || \
     (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */))
#include <stdint.h>
#endif

#if defined (__cplusplus)
namespace rocksdb {
#endif


//****************************
// Type
//****************************
/* size_t */
typedef enum { XXH_OK=0, XXH_ERROR } XXH_errorcode;



//****************************
// Simple Hash Functions
//****************************

unsigned int XXH32 (const void* input, int len, unsigned int seed);

/*
XXH32() :
    Calculate the 32-bits hash of sequence of length "len" stored at memory address "input".
    The memory between input & input+len must be valid (allocated and read-accessible).
    "seed" can be used to alter the result predictably.
    This function successfully passes all SMHasher tests.
    Speed on Core 2 Duo @ 3 GHz (single thread, SMHasher benchmark) : 5.4 GB/s
    Note that "len" is type "int", which means it is limited to 2^31-1.
    If your data is larger, use the advanced functions below.
*/



//****************************
// Advanced Hash Functions
//****************************

void*         XXH32_init   (unsigned int seed);
XXH_errorcode XXH32_update (void* state, const void* input, int len);
unsigned int  XXH32_digest (void* state);

/*
These functions calculate the xxhash of an input provided in several small packets,
as opposed to an input provided as a single block.

It must be started with :
void* XXH32_init()
The function returns a pointer which holds the state of calculation.

This pointer must be provided as "void* state" parameter for XXH32_update().
XXH32_update() can be called as many times as necessary.
The user must provide a valid (allocated) input.
The function returns an error code, with 0 meaning OK, and any other value meaning there is an error.
Note that "len" is type "int", which means it is limited to 2^31-1.
If your data is larger, it is recommended to chunk your data into blocks
of size for example 2^30 (1GB) to avoid any "int" overflow issue.

Finally, you can end the calculation anytime, by using XXH32_digest().
This function returns the final 32-bits hash.
You must provide the same "void* state" parameter created by XXH32_init().
Memory will be freed by XXH32_digest().
*/


int           XXH32_sizeofState();
XXH_errorcode XXH32_resetState(void* state, unsigned int seed);

#define       XXH32_SIZEOFSTATE 48
typedef struct { long long ll[(XXH32_SIZEOFSTATE+(sizeof(long long)-1))/sizeof(long long)]; } XXH32_stateSpace_t;
/*
These functions allow user application to make its own allocation for state.

XXH32_sizeofState() is used to know how much space must be allocated for the xxHash 32-bits state.
Note that the state must be aligned to access 'long long' fields. Memory must be allocated and referenced by a pointer.
This pointer must then be provided as 'state' into XXH32_resetState(), which initializes the state.

For static allocation purposes (such as allocation on stack, or freestanding systems without malloc()),
use the structure XXH32_stateSpace_t, which will ensure that memory space is large enough and correctly aligned to access 'long long' fields.
*/


unsigned int XXH32_intermediateDigest (void* state);
/*
This function does the same as XXH32_digest(), generating a 32-bit hash,
but preserve memory context.
This way, it becomes possible to generate intermediate hashes, and then continue feeding data with XXH32_update().
To free memory context, use XXH32_digest(), or free().
*/



//****************************
// Deprecated function names
//****************************
// The following translations are provided to ease code transition
// You are encouraged to no longer this function names
#define XXH32_feed   XXH32_update
#define XXH32_result XXH32_digest
#define XXH32_getIntermediateResult XXH32_intermediateDigest

/*-**********************************************************************
 *  64-bit hash
 ************************************************************************/
typedef unsigned long long XXH64_hash_t;

/*! XXH64() :
    Calculate the 64-bit hash of sequence of length "len" stored at memory
   address "input". "seed" can be used to alter the result predictably. This
   function runs faster on 64-bit systems, but slower on 32-bit systems (see
   benchmark).
*/
XXH64_hash_t XXH64(const void* input, size_t length, unsigned long long seed);

/*======   Streaming   ======*/
typedef struct XXH64_state_s XXH64_state_t; /* incomplete type */
XXH64_state_t* XXH64_createState(void);
XXH_errorcode XXH64_freeState(XXH64_state_t* statePtr);
void XXH64_copyState(XXH64_state_t* dst_state, const XXH64_state_t* src_state);

XXH_errorcode XXH64_reset(XXH64_state_t* statePtr, unsigned long long seed);
XXH_errorcode XXH64_update(XXH64_state_t* statePtr, const void* input,
                           size_t length);
XXH64_hash_t XXH64_digest(const XXH64_state_t* statePtr);

/*======   Canonical representation   ======*/
typedef struct {
  unsigned char digest[8];
} XXH64_canonical_t;
void XXH64_canonicalFromHash(XXH64_canonical_t* dst, XXH64_hash_t hash);
XXH64_hash_t XXH64_hashFromCanonical(const XXH64_canonical_t* src);

/* These definitions are only present to allow
 * static allocation of XXH state, on stack or in a struct for example.
 * Never **ever** use members directly. */

#if !defined(__VMS) &&       \
    (defined(__cplusplus) || \
     (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */))

struct XXH64_state_s {
  uint64_t total_len;
  uint64_t v1;
  uint64_t v2;
  uint64_t v3;
  uint64_t v4;
  uint64_t mem64[4];
  uint32_t memsize;
  uint32_t reserved[2]; /* never read nor write, might be removed in a future
                           version */
};                      /* typedef'd to XXH64_state_t */

#else

#ifndef XXH_NO_LONG_LONG /* remove 64-bit support */
struct XXH64_state_s {
  unsigned long long total_len;
  unsigned long long v1;
  unsigned long long v2;
  unsigned long long v3;
  unsigned long long v4;
  unsigned long long mem64[4];
  unsigned memsize;
  unsigned reserved[2]; /* never read nor write, might be removed in a future
                           version */
};                      /* typedef'd to XXH64_state_t */
#endif

#endif

#if defined (__cplusplus)
}  // namespace rocksdb
#endif
