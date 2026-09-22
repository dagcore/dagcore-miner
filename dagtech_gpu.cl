/*
 * DagTech GPU Kernel - OpenCL 1.2
 * Copyright (c) 2024-2026 DagTech Ltd / Dawie Nel
 * https://dagtech.network
 *
 * Exact port of the DagTech scrypt_1024_1_1_256 algorithm with proprietary
 * post-ROMix X[0] modification.  Every constant and operation matches
 * dagtech_miner.c so that CPU and GPU produce identical hashes for the
 * same nonce.
 *
 * Kernel entry point: dagtech_search
 *   header80  - 80-byte block header as 20 uint words (nonce at word 19)
 *   output    - [0] best nonce (atomic_min), [1] found count (atomic_inc)
 *   V         - global V array; each work-item gets its own 1024*32 uint slice
 *   target    - 32-bit difficulty target: (uint)(0xFFFFFFFFull / difficulty)
 *   nonce_base- nonce = nonce_base + get_global_id(0)
 */

/* Enable global-memory atomic operations (required by some drivers) */
#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics     : enable
#pragma OPENCL EXTENSION cl_khr_global_int32_extended_atomics : enable

/* =========================================================================
 * Helpers
 * ========================================================================= */
#define BSWAP(x) ((rotate((x),8u)&0x00FF00FFu)|(rotate((x),24u)&0xFF00FF00u))

/* =========================================================================
 * SHA-256 constants — identical to dagtech_sha256_k[64] in dagtech_sha256.h
 * ========================================================================= */
__constant uint SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* SHA-256 IV — identical to dagtech_sha256_iv[8] in dagtech_miner.c */
#define SHA256_IV_0 0x6a09e667u
#define SHA256_IV_1 0xbb67ae85u
#define SHA256_IV_2 0x3c6ef372u
#define SHA256_IV_3 0xa54ff53au
#define SHA256_IV_4 0x510e527fu
#define SHA256_IV_5 0x9b05688cu
#define SHA256_IV_6 0x1f83d9abu
#define SHA256_IV_7 0x5be0cd19u

/* =========================================================================
 * PBKDF2 padding constants — exact same values as in dagtech_miner.c
 * ========================================================================= */

/* scrypt_keypad[12]: { 0x80000000,0,0,0,0,0,0,0,0,0,0,0x00000280 } */
#define KEYPAD_0  0x80000000u
/* [1..10] = 0 */
#define KEYPAD_11 0x00000280u

/* scrypt_innerpad[11]: { 0x80000000,0,0,0,0,0,0,0,0,0,0x000004a0 } */
#define INNERPAD_0  0x80000000u
/* [1..9] = 0 */
#define INNERPAD_10 0x000004a0u

/* scrypt_outerpad[8]: { 0x80000000,0,0,0,0,0,0,0x00000300 } */
#define OUTERPAD_0 0x80000000u
/* [1..6] = 0 */
#define OUTERPAD_7 0x00000300u

/* scrypt_finalblk[16]: { 0x00000001,0x80000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0x00000620 } */
#define FINALBLK_0  0x00000001u
#define FINALBLK_1  0x80000000u
/* [2..14] = 0 */
#define FINALBLK_15 0x00000620u

/* =========================================================================
 * SHA-256 macros
 * ========================================================================= */
#define ROR32(x,n) (rotate((x), (uint)(32u-(n))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)  (ROR32(x,2)  ^ ROR32(x,13) ^ ROR32(x,22))
#define EP1(x)  (ROR32(x,6)  ^ ROR32(x,11) ^ ROR32(x,25))
#define SIG0(x) (ROR32(x,7)  ^ ROR32(x,18) ^ ((x)>>3u))
#define SIG1(x) (ROR32(x,17) ^ ROR32(x,19) ^ ((x)>>10u))

/* =========================================================================
 * sha256_xform — port of dagtech_sha256_xform(state, block, swap)
 *
 * swap=0: W[i] = block[i]          (words already LE in register)
 * swap=1: W[i] = BSWAP(block[i])   (convert BE-stored words to LE)
 *
 * In OpenCL we always pass the 16 words as explicit parameters to keep
 * everything in private registers; the caller supplies them pre-swapped
 * or raw depending on the call site.
 * ========================================================================= */
static void sha256_xform(__private uint state[8],
                         uint w0,  uint w1,  uint w2,  uint w3,
                         uint w4,  uint w5,  uint w6,  uint w7,
                         uint w8,  uint w9,  uint w10, uint w11,
                         uint w12, uint w13, uint w14, uint w15)
{
    uint W[64];
    W[0]=w0;  W[1]=w1;  W[2]=w2;  W[3]=w3;
    W[4]=w4;  W[5]=w5;  W[6]=w6;  W[7]=w7;
    W[8]=w8;  W[9]=w9;  W[10]=w10;W[11]=w11;
    W[12]=w12;W[13]=w13;W[14]=w14;W[15]=w15;

    for (int i = 16; i < 64; i++)
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];

    uint a=state[0], b=state[1], c=state[2], d=state[3];
    uint e=state[4], f=state[5], g=state[6], h=state[7];

    for (int i = 0; i < 64; i++) {
        uint t1 = h + EP1(e) + CH(e,f,g) + SHA256_K[i] + W[i];
        uint t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

/* =========================================================================
 * hmac80_init — port of dagtech_hmac80_init
 *
 * Inputs:
 *   key[20]  - 80-byte key as 20 uint words (LE, same as C)
 *   tstate   - IN: midstate (SHA256 of key[0..15]), OUT: new tstate
 *   ostate   - OUT: outer state
 * ========================================================================= */
static void hmac80_init(__private const uint key[20],
                        __private uint tstate[8],
                        __private uint ostate[8])
{
    uint ihash[8];

    /* Finish inner hash: process key[16..19] + keypad
       pad = { key[16], key[17], key[18], key[19],
               KEYPAD_0, 0,0,0,0,0,0,0,0,0,0, KEYPAD_11 }
       (memcpy(pad, key+16, 16) then memcpy(pad+4, scrypt_keypad, 48)) */
    sha256_xform(tstate,
        key[16], key[17], key[18], key[19],
        KEYPAD_0, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, KEYPAD_11);

    for (int i = 0; i < 8; i++) ihash[i] = tstate[i];

    /* ostate = SHA256_IV ^ (ihash ^ 0x5c5c5c5c) pad */
    ostate[0]=SHA256_IV_0; ostate[1]=SHA256_IV_1;
    ostate[2]=SHA256_IV_2; ostate[3]=SHA256_IV_3;
    ostate[4]=SHA256_IV_4; ostate[5]=SHA256_IV_5;
    ostate[6]=SHA256_IV_6; ostate[7]=SHA256_IV_7;
    sha256_xform(ostate,
        ihash[0]^0x5c5c5c5cu, ihash[1]^0x5c5c5c5cu,
        ihash[2]^0x5c5c5c5cu, ihash[3]^0x5c5c5c5cu,
        ihash[4]^0x5c5c5c5cu, ihash[5]^0x5c5c5c5cu,
        ihash[6]^0x5c5c5c5cu, ihash[7]^0x5c5c5c5cu,
        0x5c5c5c5cu, 0x5c5c5c5cu, 0x5c5c5c5cu, 0x5c5c5c5cu,
        0x5c5c5c5cu, 0x5c5c5c5cu, 0x5c5c5c5cu, 0x5c5c5c5cu);

    /* tstate = SHA256_IV ^ (ihash ^ 0x36363636) pad */
    tstate[0]=SHA256_IV_0; tstate[1]=SHA256_IV_1;
    tstate[2]=SHA256_IV_2; tstate[3]=SHA256_IV_3;
    tstate[4]=SHA256_IV_4; tstate[5]=SHA256_IV_5;
    tstate[6]=SHA256_IV_6; tstate[7]=SHA256_IV_7;
    sha256_xform(tstate,
        ihash[0]^0x36363636u, ihash[1]^0x36363636u,
        ihash[2]^0x36363636u, ihash[3]^0x36363636u,
        ihash[4]^0x36363636u, ihash[5]^0x36363636u,
        ihash[6]^0x36363636u, ihash[7]^0x36363636u,
        0x36363636u, 0x36363636u, 0x36363636u, 0x36363636u,
        0x36363636u, 0x36363636u, 0x36363636u, 0x36363636u);
}

/* =========================================================================
 * pbkdf2_80_128 — port of dagtech_pbkdf2_80_128
 *
 * tstate_in / ostate_in are const; local copies are used so callers'
 * tstate/ostate are not modified.
 *
 * Produces 32 uint words (128 bytes) in output[0..31].
 * ========================================================================= */
static void pbkdf2_80_128(__private const uint tstate_in[8],
                          __private const uint ostate_in[8],
                          __private const uint salt[20],
                          __private uint output[32])
{
    /* istate = tstate XORed with SHA256 of salt[0..15] */
    uint istate[8];
    for (int i = 0; i < 8; i++) istate[i] = tstate_in[i];
    sha256_xform(istate,
        salt[0],  salt[1],  salt[2],  salt[3],
        salt[4],  salt[5],  salt[6],  salt[7],
        salt[8],  salt[9],  salt[10], salt[11],
        salt[12], salt[13], salt[14], salt[15]);

    /* ibuf = { salt[16], salt[17], salt[18], salt[19], counter,
                INNERPAD_0, 0,0,0,0,0,0,0,0,0, INNERPAD_10 }
       (memcpy(ibuf, salt+16, 16) then memcpy(ibuf+5, scrypt_innerpad, 44)) */

    /* obuf layout: obuf[0..7]=obuf_state, obuf[8..15] = outerpad constant
       (memcpy(obuf+8, scrypt_outerpad, 32)) */

    for (int i = 0; i < 4; i++) {
        uint obuf_state[8];
        for (int j = 0; j < 8; j++) obuf_state[j] = istate[j];

        /* Process ibuf: salt[16..19], counter i+1, innerpad */
        sha256_xform(obuf_state,
            salt[16], salt[17], salt[18], salt[19],
            (uint)(i+1),
            INNERPAD_0, 0u, 0u,
            0u, 0u, 0u, 0u,
            0u, 0u, 0u, INNERPAD_10);

        /* ostate2 = outer; process obuf_state + outerpad */
        uint ostate2[8];
        for (int j = 0; j < 8; j++) ostate2[j] = ostate_in[j];
        sha256_xform(ostate2,
            obuf_state[0], obuf_state[1], obuf_state[2], obuf_state[3],
            obuf_state[4], obuf_state[5], obuf_state[6], obuf_state[7],
            OUTERPAD_0, 0u, 0u, 0u,
            0u, 0u, 0u, OUTERPAD_7);

        /* Store as big-endian words (BSWAP matches DAGTECH_SWAB32) */
        for (int j = 0; j < 8; j++)
            output[8*i + j] = BSWAP(ostate2[j]);
    }
}

/* =========================================================================
 * pbkdf2_128_32 — port of dagtech_pbkdf2_128_32
 *
 * Modifies tstate and ostate in place (last use at the call site).
 * Produces 8 uint words (32 bytes) in output[0..7].
 * ========================================================================= */
static void pbkdf2_128_32(__private uint tstate[8],
                          __private uint ostate[8],
                          __private const uint salt[32],
                          __private uint output[8])
{
    /* dagtech_sha256_xform(tstate, salt,      1)  -- swap=1 */
    sha256_xform(tstate,
        BSWAP(salt[0]),  BSWAP(salt[1]),  BSWAP(salt[2]),  BSWAP(salt[3]),
        BSWAP(salt[4]),  BSWAP(salt[5]),  BSWAP(salt[6]),  BSWAP(salt[7]),
        BSWAP(salt[8]),  BSWAP(salt[9]),  BSWAP(salt[10]), BSWAP(salt[11]),
        BSWAP(salt[12]), BSWAP(salt[13]), BSWAP(salt[14]), BSWAP(salt[15]));

    /* dagtech_sha256_xform(tstate, salt+16, 1) -- swap=1 */
    sha256_xform(tstate,
        BSWAP(salt[16]), BSWAP(salt[17]), BSWAP(salt[18]), BSWAP(salt[19]),
        BSWAP(salt[20]), BSWAP(salt[21]), BSWAP(salt[22]), BSWAP(salt[23]),
        BSWAP(salt[24]), BSWAP(salt[25]), BSWAP(salt[26]), BSWAP(salt[27]),
        BSWAP(salt[28]), BSWAP(salt[29]), BSWAP(salt[30]), BSWAP(salt[31]));

    /* dagtech_sha256_xform(tstate, scrypt_finalblk, 0) -- swap=0 */
    sha256_xform(tstate,
        FINALBLK_0, FINALBLK_1, 0u, 0u,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, FINALBLK_15);

    /* buf = { tstate[0..7], OUTERPAD_0, 0,0,0,0,0,0, OUTERPAD_7 } */
    sha256_xform(ostate,
        tstate[0], tstate[1], tstate[2], tstate[3],
        tstate[4], tstate[5], tstate[6], tstate[7],
        OUTERPAD_0, 0u, 0u, 0u,
        0u, 0u, 0u, OUTERPAD_7);

    /* Output: BSWAP each word (matches DAGTECH_SWAB32) */
    for (int i = 0; i < 8; i++)
        output[i] = BSWAP(ostate[i]);
}

/* =========================================================================
 * xor_salsa8 — port of dagtech_xor_salsa8
 *
 * Salsa20/8 quarter-rounds: column then row ordering, exactly as in C.
 * ========================================================================= */
static void xor_salsa8(__private uint B[16], __private const uint Bx[16])
{
    uint x00=(B[0]^=Bx[0]),  x01=(B[1]^=Bx[1]),
         x02=(B[2]^=Bx[2]),  x03=(B[3]^=Bx[3]);
    uint x04=(B[4]^=Bx[4]),  x05=(B[5]^=Bx[5]),
         x06=(B[6]^=Bx[6]),  x07=(B[7]^=Bx[7]);
    uint x08=(B[8]^=Bx[8]),  x09=(B[9]^=Bx[9]),
         x10=(B[10]^=Bx[10]), x11=(B[11]^=Bx[11]);
    uint x12=(B[12]^=Bx[12]), x13=(B[13]^=Bx[13]),
         x14=(B[14]^=Bx[14]), x15=(B[15]^=Bx[15]);

    for (int i = 0; i < 8; i += 2) {
        /* Column rounds */
        x04^=rotate(x00+x12,7u);  x09^=rotate(x05+x01,7u);
        x14^=rotate(x10+x06,7u);  x03^=rotate(x15+x11,7u);
        x08^=rotate(x04+x00,9u);  x13^=rotate(x09+x05,9u);
        x02^=rotate(x14+x10,9u);  x07^=rotate(x03+x15,9u);
        x12^=rotate(x08+x04,13u); x01^=rotate(x13+x09,13u);
        x06^=rotate(x02+x14,13u); x11^=rotate(x07+x03,13u);
        x00^=rotate(x12+x08,18u); x05^=rotate(x01+x13,18u);
        x10^=rotate(x06+x02,18u); x15^=rotate(x11+x07,18u);
        /* Row rounds */
        x01^=rotate(x00+x03,7u);  x06^=rotate(x05+x04,7u);
        x11^=rotate(x10+x09,7u);  x12^=rotate(x15+x14,7u);
        x02^=rotate(x01+x00,9u);  x07^=rotate(x06+x05,9u);
        x08^=rotate(x11+x10,9u);  x13^=rotate(x12+x15,9u);
        x03^=rotate(x02+x01,13u); x04^=rotate(x07+x06,13u);
        x09^=rotate(x08+x11,13u); x14^=rotate(x13+x12,13u);
        x00^=rotate(x03+x02,18u); x05^=rotate(x04+x07,18u);
        x10^=rotate(x09+x08,18u); x15^=rotate(x14+x13,18u);
    }

    B[0]+=x00;  B[1]+=x01;  B[2]+=x02;  B[3]+=x03;
    B[4]+=x04;  B[5]+=x05;  B[6]+=x06;  B[7]+=x07;
    B[8]+=x08;  B[9]+=x09;  B[10]+=x10; B[11]+=x11;
    B[12]+=x12; B[13]+=x13; B[14]+=x14; B[15]+=x15;
}

/* =========================================================================
 * xor_salsa8_coop — Salsa20/8 parallelized across 4 cooperating threads.
 *
 * Salsa20 uses DIAGONAL column ownership (column c starts at row c, then
 * wraps). So thread t owns the diagonal column t:
 *   Thread 0: positions (0, 4, 8, 12)         = (x[0],  x[4],  x[8],  x[12])
 *   Thread 1: positions (5, 9, 13, 1)         = (x[5],  x[9],  x[13], x[1])
 *   Thread 2: positions (10, 14, 2, 6)        = (x[10], x[14], x[2],  x[6])
 *   Thread 3: positions (15, 3, 7, 11)        = (x[15], x[3],  x[7],  x[11])
 *
 * With this rotation, the column-round formula is IDENTICAL for every
 * thread (r1 ^= rot(r0+r3,7); r2 ^= rot(r1+r0,9); ...). After the column
 * round we transpose to DIAGONAL row ownership:
 *   Thread 0: (x[0],  x[1],  x[2],  x[3])  — row 0, regular order
 *   Thread 1: (x[5],  x[6],  x[7],  x[4])  — row 1, cycled by +1
 *   Thread 2: (x[10], x[11], x[8],  x[9])  — row 2, cycled by +2
 *   Thread 3: (x[15], x[12], x[13], x[14]) — row 3, cycled by +3
 * The row-round formula is the SAME 4-op sequence on these.
 *
 * The diagonal-col → diagonal-row transpose: thread t writes lane k to
 * Lh[t*4 + k]; the destination thread t reads new lane k from Lh[t_src*4
 * + k_src] where t_src = (t + k) & 3, k_src = (-k) & 3 = (4 - k) & 3.
 * (Derivation: row_pos(t,k) = x_{4t + (k+t)%4}; this value sits at
 * col_pos(t_src, k_src) where t_src = (k+t)%4 and k_src = (t - t_src)%4.)
 *
 * The row → col transpose uses the same formula by symmetry.
 *
 * Algorithm:
 *   1. B ^= Bx (in place); save result as rounds-input x0..x3.
 *   2. 4 iterations of (column round, transpose, row round, transpose-back).
 *   3. B += final x0..x3 (still in diagonal-column ownership).
 *
 * Returns with B in diagonal-column ownership. Lh is left dirty.
 * ========================================================================= */
static void xor_salsa8_coop(__private uint B[4], __private const uint Bx[4],
                            __local uint *Lh, uint t)
{
    /* B ^= Bx (in place); capture pre-rounds value for final add */
    uint x0 = (B[0] ^= Bx[0]);
    uint x1 = (B[1] ^= Bx[1]);
    uint x2 = (B[2] ^= Bx[2]);
    uint x3 = (B[3] ^= Bx[3]);

    /* 4 iterations × (col-round + row-round) = 8 rounds total */
    for (int i = 0; i < 4; i++) {
        /* Column round — each thread on its own diagonal column, no comms */
        x1 ^= rotate(x0 + x3,  7u);
        x2 ^= rotate(x1 + x0,  9u);
        x3 ^= rotate(x2 + x1, 13u);
        x0 ^= rotate(x3 + x2, 18u);

        /* Transpose: diagonal-column → diagonal-row */
        Lh[t*4 + 0] = x0;
        Lh[t*4 + 1] = x1;
        Lh[t*4 + 2] = x2;
        Lh[t*4 + 3] = x3;
        barrier(CLK_LOCAL_MEM_FENCE);
        x0 = Lh[((t + 0u) & 3u) * 4u + ((4u - 0u) & 3u)];  /* = Lh[t*4 + 0]      */
        x1 = Lh[((t + 1u) & 3u) * 4u + ((4u - 1u) & 3u)];  /* = Lh[((t+1)&3)*4+3] */
        x2 = Lh[((t + 2u) & 3u) * 4u + ((4u - 2u) & 3u)];  /* = Lh[((t+2)&3)*4+2] */
        x3 = Lh[((t + 3u) & 3u) * 4u + ((4u - 3u) & 3u)];  /* = Lh[((t+3)&3)*4+1] */
        barrier(CLK_LOCAL_MEM_FENCE);

        /* Row round — same 4-op formula on diagonal-row ownership */
        x1 ^= rotate(x0 + x3,  7u);
        x2 ^= rotate(x1 + x0,  9u);
        x3 ^= rotate(x2 + x1, 13u);
        x0 ^= rotate(x3 + x2, 18u);

        /* Transpose: diagonal-row → diagonal-column (same formula by symmetry) */
        Lh[t*4 + 0] = x0;
        Lh[t*4 + 1] = x1;
        Lh[t*4 + 2] = x2;
        Lh[t*4 + 3] = x3;
        barrier(CLK_LOCAL_MEM_FENCE);
        x0 = Lh[((t + 0u) & 3u) * 4u + ((4u - 0u) & 3u)];
        x1 = Lh[((t + 1u) & 3u) * 4u + ((4u - 1u) & 3u)];
        x2 = Lh[((t + 2u) & 3u) * 4u + ((4u - 2u) & 3u)];
        x3 = Lh[((t + 3u) & 3u) * 4u + ((4u - 3u) & 3u)];
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    /* B += final-rounds x (still in diagonal-column ownership) */
    B[0] += x0;
    B[1] += x1;
    B[2] += x2;
    B[3] += x3;
}

/* =========================================================================
 * scrypt_romix — port of dagtech_scrypt_romix (N=1024)
 *
 * V_slice is the work-item's own 1024*32 uint slice of the global V buffer.
 * X[0..31] is the 128-byte scrypt block in private memory.
 *
 * GPU-2026.0607.3 (#40 increment 1): V access uses uint4 vector loads/stores
 * for 16-byte memory transactions instead of 4-byte. V is clCreateBuffer-
 * allocated (page-aligned) and gid*1024*32 + i*32 is always a multiple of
 * 32 uints = 128 bytes, so the __global uint4* cast is well-aligned.
 * Algorithm and data flow unchanged - pool acceptance preserved.
 * ========================================================================= */
static void scrypt_romix(__private uint X[32], __global uint *V_slice)
{
    /* Fill phase: 8 x uint4 stores per row (32 uints = 128 bytes = 8 x 16) */
    for (int i = 0; i < 1024; i++) {
        __global uint4 *Vp = (__global uint4 *)(V_slice + i*32);
        Vp[0] = (uint4)(X[ 0], X[ 1], X[ 2], X[ 3]);
        Vp[1] = (uint4)(X[ 4], X[ 5], X[ 6], X[ 7]);
        Vp[2] = (uint4)(X[ 8], X[ 9], X[10], X[11]);
        Vp[3] = (uint4)(X[12], X[13], X[14], X[15]);
        Vp[4] = (uint4)(X[16], X[17], X[18], X[19]);
        Vp[5] = (uint4)(X[20], X[21], X[22], X[23]);
        Vp[6] = (uint4)(X[24], X[25], X[26], X[27]);
        Vp[7] = (uint4)(X[28], X[29], X[30], X[31]);
        xor_salsa8(&X[0],  &X[16]);
        xor_salsa8(&X[16], &X[0]);
    }
    /* Mix phase: 8 x uint4 loads per row, XOR into private X[] */
    for (int i = 0; i < 1024; i++) {
        int j = (int)(X[16] & 1023u);
        __global uint4 *Vp = (__global uint4 *)(V_slice + j*32);
        uint4 v;
        v = Vp[0]; X[ 0] ^= v.x; X[ 1] ^= v.y; X[ 2] ^= v.z; X[ 3] ^= v.w;
        v = Vp[1]; X[ 4] ^= v.x; X[ 5] ^= v.y; X[ 6] ^= v.z; X[ 7] ^= v.w;
        v = Vp[2]; X[ 8] ^= v.x; X[ 9] ^= v.y; X[10] ^= v.z; X[11] ^= v.w;
        v = Vp[3]; X[12] ^= v.x; X[13] ^= v.y; X[14] ^= v.z; X[15] ^= v.w;
        v = Vp[4]; X[16] ^= v.x; X[17] ^= v.y; X[18] ^= v.z; X[19] ^= v.w;
        v = Vp[5]; X[20] ^= v.x; X[21] ^= v.y; X[22] ^= v.z; X[23] ^= v.w;
        v = Vp[6]; X[24] ^= v.x; X[25] ^= v.y; X[26] ^= v.z; X[27] ^= v.w;
        v = Vp[7]; X[28] ^= v.x; X[29] ^= v.y; X[30] ^= v.z; X[31] ^= v.w;
        xor_salsa8(&X[0],  &X[16]);
        xor_salsa8(&X[16], &X[0]);
    }
}

/* =========================================================================
 * dagtech_search — main kernel
 *
 * Each work-item hashes one nonce = nonce_base + get_global_id(0).
 * Full pipeline:
 *   1. midstate (SHA256 of header[0..15], swap=0)
 *   2. hmac80_init
 *   3. pbkdf2_80_128
 *   4. scrypt_romix (using per-work-item V slice)
 *   5. Post-ROMix X[0] modification (proprietary DagTech step)
 *   6. pbkdf2_128_32
 *   7. Compare hash[7] against 32-bit target
 * ========================================================================= */
__kernel void dagtech_search(__global const uint *header80,
                             __global uint *output,
                             __global uint *V,
                             uint target,
                             uint nonce_base)
{
    uint gid   = get_global_id(0);
    uint nonce = nonce_base + gid;

    /* Copy header to private; set nonce at word 19 */
    __private uint hdr[20];
    for (int i = 0; i < 20; i++) hdr[i] = header80[i];
    hdr[19] = nonce;

    /* --- Step 1: midstate = SHA256_IV, then xform with hdr[0..15] (swap=0) --- */
    __private uint tstate[8];
    tstate[0]=SHA256_IV_0; tstate[1]=SHA256_IV_1;
    tstate[2]=SHA256_IV_2; tstate[3]=SHA256_IV_3;
    tstate[4]=SHA256_IV_4; tstate[5]=SHA256_IV_5;
    tstate[6]=SHA256_IV_6; tstate[7]=SHA256_IV_7;

    sha256_xform(tstate,
        hdr[0],  hdr[1],  hdr[2],  hdr[3],
        hdr[4],  hdr[5],  hdr[6],  hdr[7],
        hdr[8],  hdr[9],  hdr[10], hdr[11],
        hdr[12], hdr[13], hdr[14], hdr[15]);

    /* --- Step 2: HMAC-SHA256 init --- */
    __private uint ostate[8];
    hmac80_init(hdr, tstate, ostate);

    /* --- Step 3: PBKDF2 80 -> 128 --- */
    __private uint X[32];
    pbkdf2_80_128(tstate, ostate, hdr, X);

    /* --- Step 4: scrypt ROMix on per-work-item V slice --- */
    __global uint *V_slice = V + (size_t)gid * (size_t)(1024 * 32);
    scrypt_romix(X, V_slice);

    /* --- Step 5: Proprietary post-ROMix X[0] modification ---
         uint32_t B = DAGTECH_SWAB32(X[0]);
         uint32_t M = (B & 0xffff8000) | ((B + 0xe0) & 0x7fff);
         X[0] = DAGTECH_SWAB32(M);                                   */
    {
        uint B = BSWAP(X[0]);
        uint M = (B & 0xffff8000u) | ((B + 0xe0u) & 0x7fffu);
        X[0]  = BSWAP(M);
    }

    /* --- Step 6: PBKDF2 128 -> 32 --- */
    __private uint hash[8];
    pbkdf2_128_32(tstate, ostate, X, hash);

    /* --- Step 7: Check target (hash[7] is the highest-order word after BSWAP) ---
       The C check uses hash[31..24] as a big-endian uint64.
       After pbkdf2_128_32, hash[i] = BSWAP(ostate[i]), so
       hash[7] is the most-significant 32 bits of the big-endian hash.
       We check hash[7] <= target (32-bit approximation). */
    if (hash[7] <= target) {
        atomic_min(&output[0], nonce);
        atomic_inc(&output[1]);
    }
}

/* =========================================================================
 * GPU-2026.0607.4 (#40 increment 2): split-kernel pipeline
 *
 * Same algorithm as dagtech_search, but partitioned into three kernels
 * sharing an intermediate X_buf (one 32-uint slot per work-item):
 *
 *   dagtech_pre   : steps 1-3, write X[32] into X_buf
 *   dagtech_romix : read X from X_buf, run scrypt_romix, write X back
 *   dagtech_post  : read X from X_buf, recompute tstate/ostate,
 *                   apply post-ROMix X[0] tweak, run pbkdf2_128_32,
 *                   check target
 *
 * Rationale: isolates the ROMix step so a future increment can rewrite
 * its work-item layout (4-threads-per-hash + interleaved V) without
 * touching the surrounding kernels. tstate/ostate recomputed in post
 * (cost: 1 SHA256 + 4 hmac transforms = trivial vs. 2048 PBKDF2 rounds).
 *
 * dagtech_search remains compiled for the legacy single-kernel fallback
 * path (GPU_KERNEL_MODE=legacy).
 * ========================================================================= */

/* --- dagtech_pre: steps 1-3, output X[gid*32 + 0..31] in global X_buf --- */
__kernel void dagtech_pre(__global const uint *header80,
                          __global uint *X_buf,
                          uint nonce_base)
{
    uint gid   = get_global_id(0);
    uint nonce = nonce_base + gid;

    __private uint hdr[20];
    for (int i = 0; i < 20; i++) hdr[i] = header80[i];
    hdr[19] = nonce;

    /* Step 1: midstate */
    __private uint tstate[8];
    tstate[0]=SHA256_IV_0; tstate[1]=SHA256_IV_1;
    tstate[2]=SHA256_IV_2; tstate[3]=SHA256_IV_3;
    tstate[4]=SHA256_IV_4; tstate[5]=SHA256_IV_5;
    tstate[6]=SHA256_IV_6; tstate[7]=SHA256_IV_7;
    sha256_xform(tstate,
        hdr[0],  hdr[1],  hdr[2],  hdr[3],
        hdr[4],  hdr[5],  hdr[6],  hdr[7],
        hdr[8],  hdr[9],  hdr[10], hdr[11],
        hdr[12], hdr[13], hdr[14], hdr[15]);

    /* Step 2: hmac80 init */
    __private uint ostate[8];
    hmac80_init(hdr, tstate, ostate);

    /* Step 3: pbkdf2 80 -> 128 (output X[32]) */
    __private uint X[32];
    pbkdf2_80_128(tstate, ostate, hdr, X);

    /* Persist X to global X_buf. uint4 stores; X_buf is clCreateBuffer-
       aligned and gid*32 is multiple of 128 bytes -> safe cast. */
    __global uint4 *Xp = (__global uint4 *)(X_buf + (size_t)gid * 32);
    Xp[0] = (uint4)(X[ 0], X[ 1], X[ 2], X[ 3]);
    Xp[1] = (uint4)(X[ 4], X[ 5], X[ 6], X[ 7]);
    Xp[2] = (uint4)(X[ 8], X[ 9], X[10], X[11]);
    Xp[3] = (uint4)(X[12], X[13], X[14], X[15]);
    Xp[4] = (uint4)(X[16], X[17], X[18], X[19]);
    Xp[5] = (uint4)(X[20], X[21], X[22], X[23]);
    Xp[6] = (uint4)(X[24], X[25], X[26], X[27]);
    Xp[7] = (uint4)(X[28], X[29], X[30], X[31]);
}

/* --- dagtech_romix: read X, run scrypt_romix using V_slice, write X back --- */
__kernel void dagtech_romix(__global uint *X_buf,
                            __global uint *V)
{
    uint gid = get_global_id(0);

    /* Load X[32] from global X_buf into private */
    __private uint X[32];
    __global uint4 *Xp = (__global uint4 *)(X_buf + (size_t)gid * 32);
    uint4 v;
    v = Xp[0]; X[ 0]=v.x; X[ 1]=v.y; X[ 2]=v.z; X[ 3]=v.w;
    v = Xp[1]; X[ 4]=v.x; X[ 5]=v.y; X[ 6]=v.z; X[ 7]=v.w;
    v = Xp[2]; X[ 8]=v.x; X[ 9]=v.y; X[10]=v.z; X[11]=v.w;
    v = Xp[3]; X[12]=v.x; X[13]=v.y; X[14]=v.z; X[15]=v.w;
    v = Xp[4]; X[16]=v.x; X[17]=v.y; X[18]=v.z; X[19]=v.w;
    v = Xp[5]; X[20]=v.x; X[21]=v.y; X[22]=v.z; X[23]=v.w;
    v = Xp[6]; X[24]=v.x; X[25]=v.y; X[26]=v.z; X[27]=v.w;
    v = Xp[7]; X[28]=v.x; X[29]=v.y; X[30]=v.z; X[31]=v.w;

    /* Run the (uint4-vectorised) Fill+Mix using this work-item's V slice */
    __global uint *V_slice = V + (size_t)gid * (size_t)(1024 * 32);
    scrypt_romix(X, V_slice);

    /* Write X back to X_buf */
    Xp[0] = (uint4)(X[ 0], X[ 1], X[ 2], X[ 3]);
    Xp[1] = (uint4)(X[ 4], X[ 5], X[ 6], X[ 7]);
    Xp[2] = (uint4)(X[ 8], X[ 9], X[10], X[11]);
    Xp[3] = (uint4)(X[12], X[13], X[14], X[15]);
    Xp[4] = (uint4)(X[16], X[17], X[18], X[19]);
    Xp[5] = (uint4)(X[20], X[21], X[22], X[23]);
    Xp[6] = (uint4)(X[24], X[25], X[26], X[27]);
    Xp[7] = (uint4)(X[28], X[29], X[30], X[31]);
}

/* --- dagtech_post: read X, recompute tstate/ostate, tweak, pbkdf2 128->32,
       compare hash[7] vs target, atomic_min into output --- */
__kernel void dagtech_post(__global uint *X_buf,
                           __global const uint *header80,
                           __global uint *output,
                           uint target,
                           uint nonce_base)
{
    uint gid   = get_global_id(0);
    uint nonce = nonce_base + gid;

    /* Load X[32] from X_buf */
    __private uint X[32];
    __global uint4 *Xp = (__global uint4 *)(X_buf + (size_t)gid * 32);
    uint4 v;
    v = Xp[0]; X[ 0]=v.x; X[ 1]=v.y; X[ 2]=v.z; X[ 3]=v.w;
    v = Xp[1]; X[ 4]=v.x; X[ 5]=v.y; X[ 6]=v.z; X[ 7]=v.w;
    v = Xp[2]; X[ 8]=v.x; X[ 9]=v.y; X[10]=v.z; X[11]=v.w;
    v = Xp[3]; X[12]=v.x; X[13]=v.y; X[14]=v.z; X[15]=v.w;
    v = Xp[4]; X[16]=v.x; X[17]=v.y; X[18]=v.z; X[19]=v.w;
    v = Xp[5]; X[20]=v.x; X[21]=v.y; X[22]=v.z; X[23]=v.w;
    v = Xp[6]; X[24]=v.x; X[25]=v.y; X[26]=v.z; X[27]=v.w;
    v = Xp[7]; X[28]=v.x; X[29]=v.y; X[30]=v.z; X[31]=v.w;

    /* Recompute tstate/ostate (cheap: SHA256_IV -> midstate(hdr[0..15]),
       then hmac80_init(hdr,...) -> 4 sha256_xform calls total). The
       per-nonce hdr[19] doesn't affect midstate (which only uses
       hdr[0..15]) or hmac80_init's tstate (which uses hdr[16..19] but
       the proprietary tweak is bit-identical regardless of nonce since
       the algorithm operates on the same word positions). */
    __private uint hdr[20];
    for (int i = 0; i < 20; i++) hdr[i] = header80[i];
    hdr[19] = nonce;

    __private uint tstate[8];
    tstate[0]=SHA256_IV_0; tstate[1]=SHA256_IV_1;
    tstate[2]=SHA256_IV_2; tstate[3]=SHA256_IV_3;
    tstate[4]=SHA256_IV_4; tstate[5]=SHA256_IV_5;
    tstate[6]=SHA256_IV_6; tstate[7]=SHA256_IV_7;
    sha256_xform(tstate,
        hdr[0],  hdr[1],  hdr[2],  hdr[3],
        hdr[4],  hdr[5],  hdr[6],  hdr[7],
        hdr[8],  hdr[9],  hdr[10], hdr[11],
        hdr[12], hdr[13], hdr[14], hdr[15]);
    __private uint ostate[8];
    hmac80_init(hdr, tstate, ostate);

    /* Step 5: post-ROMix X[0] tweak (bit-identical to dagtech_search) */
    {
        uint B = BSWAP(X[0]);
        uint M = (B & 0xffff8000u) | ((B + 0xe0u) & 0x7fffu);
        X[0]  = BSWAP(M);
    }

    /* Step 6: pbkdf2 128 -> 32 */
    __private uint hash[8];
    pbkdf2_128_32(tstate, ostate, X, hash);

    /* Step 7: target check */
    if (hash[7] <= target) {
        atomic_min(&output[0], nonce);
        atomic_inc(&output[1]);
    }
}

/* =========================================================================
 * GPU-2026.0607.5 (#40 increment 3): dagtech_romix_coop
 *
 * Cooperative-thread ROMix kernel. Replaces dagtech_romix when
 * kernel_mode == 2 (coop). dagtech_pre and dagtech_post are unchanged.
 *
 * Launch shape: 4 work-items per hash. global_size_romix = 4 * hashes.
 * Local-work-size MUST be a multiple of 4; recommended 64 (= 16 hashes per
 * work-group, all 4 threads of any hash live in the same NVIDIA warp / AMD
 * wavefront, so barrier(CLK_LOCAL_MEM_FENCE) is cheap intra-warp sync).
 *
 * Local memory (passed as kernel arg with host-supplied size):
 *   - 32 uints per hash. Layout per hash:
 *       L[ 0..15]  used as transpose scratch by xor_salsa8_coop
 *       L[16..47]  used for: X rearrangement on entry/exit (32 uints),
 *                  AND j broadcast in Mix phase (1 uint, slot [16])
 *     (Disjoint in time: rearrangement happens before/after the loops;
 *     j broadcast happens between salsas. Salsa transposes use [0..15].)
 *   Host computes:  local_bytes = (lws / 4) * 32 * sizeof(cl_uint).
 *
 * Per-hash V layout in this kernel (only used internally — pre/post never
 * touch V). Each row of 32 uints holds 4 diagonal-column ownership chunks
 * of B then 4 of Bx, so 4 cooperating threads write/read 4 adjacent
 * 16-byte chunks (one 128-byte coalesced transaction per chunk):
 *     V_slice[i*32 +  0.. 3]  thread 0's diag-Bcol  = (B[0],  B[4],  B[8],  B[12])
 *     V_slice[i*32 +  4.. 7]  thread 1's diag-Bcol  = (B[5],  B[9],  B[13], B[1])
 *     V_slice[i*32 +  8..11]  thread 2's diag-Bcol  = (B[10], B[14], B[2],  B[6])
 *     V_slice[i*32 + 12..15]  thread 3's diag-Bcol  = (B[15], B[3],  B[7],  B[11])
 *     V_slice[i*32 + 16..19]  thread 0's diag-Bxcol = (Bx[0],  Bx[4],  Bx[8],  Bx[12])
 *     V_slice[i*32 + 20..23]  thread 1's diag-Bxcol = (Bx[5],  Bx[9],  Bx[13], Bx[1])
 *     V_slice[i*32 + 24..27]  thread 2's diag-Bxcol = (Bx[10], Bx[14], Bx[2],  Bx[6])
 *     V_slice[i*32 + 28..31]  thread 3's diag-Bxcol = (Bx[15], Bx[3],  Bx[7],  Bx[11])
 * V's internal layout doesn't match X's flat layout but that's fine — V is
 * private to this kernel; the rearrange-on-entry and rearrange-on-exit map
 * to/from X's flat X[0..31] for the pre/post handoff.
 * ========================================================================= */
__kernel void dagtech_romix_coop(__global uint *X_buf,
                                 __global uint *V,
                                 __local  uint *L)
{
    uint gid       = get_global_id(0);
    uint lid       = get_local_id(0);
    uint t         = gid & 3u;            /* thread within hash (0..3) */
    uint hash_gid  = gid >> 2;            /* hash index globally */
    uint hash_lid  = lid >> 2;            /* hash index within work-group */

    /* This hash's 32-uint slice of local memory */
    __local uint *Lh = L + hash_lid * 32;
    __local uint *Lt = Lh;             /* transpose scratch (16 uints) */
    __local uint *Lx = Lh + 16;        /* X-rearrangement scratch (32 uints)
                                          AND j broadcast (slot [0] of Lx) */

    __global uint *V_slice = V + (size_t)hash_gid * (size_t)(1024 * 32);
    __global uint *X_ptr   = X_buf + (size_t)hash_gid * 32;

    /* --- Load X[t*8 .. t*8+7] (regular contiguous layout from dagtech_pre) --- */
    uint X8[8];
    {
        __global uint4 *Xp = (__global uint4 *)(X_ptr + t * 8);
        uint4 v;
        v = Xp[0]; X8[0]=v.x; X8[1]=v.y; X8[2]=v.z; X8[3]=v.w;
        v = Xp[1]; X8[4]=v.x; X8[5]=v.y; X8[6]=v.z; X8[7]=v.w;
    }

    /* --- Rearrange into DIAGONAL-column ownership via Lx (32 uints/hash) ---
       Each thread t writes its 8 contiguous X[t*8..t*8+7] into Lx[t*8..t*8+7].
       After barrier, each thread reads its 8 diag-column-owned values:
         Bcol[k]  = Lx[ 0 + t + 4*((k+t) & 3) ]    for k=0..3
         Bxcol[k] = Lx[16 + t + 4*((k+t) & 3) ]    for k=0..3
       Examples:
         t=0: Bcol = (Lx[0],  Lx[4],  Lx[8],  Lx[12])  = (X[0],  X[4],  X[8],  X[12])
         t=1: Bcol = (Lx[5],  Lx[9],  Lx[13], Lx[1])   = (X[5],  X[9],  X[13], X[1])
         t=2: Bcol = (Lx[10], Lx[14], Lx[2],  Lx[6])
         t=3: Bcol = (Lx[15], Lx[3],  Lx[7],  Lx[11])
    */
    for (int k = 0; k < 8; k++) Lx[t*8 + k] = X8[k];
    barrier(CLK_LOCAL_MEM_FENCE);

    uint Bcol[4], Bxcol[4];
    Bcol[0]  = Lx[ 0u + t + 4u * ((0u + t) & 3u) ];
    Bcol[1]  = Lx[ 0u + t + 4u * ((1u + t) & 3u) ];
    Bcol[2]  = Lx[ 0u + t + 4u * ((2u + t) & 3u) ];
    Bcol[3]  = Lx[ 0u + t + 4u * ((3u + t) & 3u) ];
    Bxcol[0] = Lx[16u + t + 4u * ((0u + t) & 3u) ];
    Bxcol[1] = Lx[16u + t + 4u * ((1u + t) & 3u) ];
    Bxcol[2] = Lx[16u + t + 4u * ((2u + t) & 3u) ];
    Bxcol[3] = Lx[16u + t + 4u * ((3u + t) & 3u) ];
    barrier(CLK_LOCAL_MEM_FENCE);

    /* --- Fill phase: 1024 iterations of (write V row) + (2 coop salsas) --- */
    for (int i = 0; i < 1024; i++) {
        /* Write this thread's 4-uint B chunk and 4-uint Bx chunk to V row i.
           Four threads' uint4 writes = one 128-byte transaction per chunk. */
        __global uint4 *Vp_b  = (__global uint4 *)(V_slice + i*32 +  0 + t*4);
        __global uint4 *Vp_bx = (__global uint4 *)(V_slice + i*32 + 16 + t*4);
        Vp_b [0] = (uint4)(Bcol[0],  Bcol[1],  Bcol[2],  Bcol[3]);
        Vp_bx[0] = (uint4)(Bxcol[0], Bxcol[1], Bxcol[2], Bxcol[3]);

        /* xor_salsa8(B, Bx)  →  B becomes (B^Bx) + salsa(B^Bx)  */
        xor_salsa8_coop(Bcol, Bxcol, Lt, t);
        /* xor_salsa8(Bx, B)  →  Bx becomes (Bx^new_B) + salsa(Bx^new_B)  */
        xor_salsa8_coop(Bxcol, Bcol, Lt, t);
    }

    /* --- Mix phase: 1024 iterations of (broadcast j) + (XOR V row) + (2 coop salsas) --- */
    for (int i = 0; i < 1024; i++) {
        /* j = X[16] & 1023u. X[16] = Bx[0]. In column ownership, Bx[0] is the
           lane-0 value of thread 0's Bxcol. Broadcast via Lx[0]. */
        if (t == 0) Lx[0] = Bxcol[0];
        barrier(CLK_LOCAL_MEM_FENCE);
        int j = (int)(Lx[0] & 1023u);
        barrier(CLK_LOCAL_MEM_FENCE);

        /* XOR V[j] into B and Bx (same layout as Fill writes) */
        __global uint4 *Vp_b  = (__global uint4 *)(V_slice + j*32 +  0 + t*4);
        __global uint4 *Vp_bx = (__global uint4 *)(V_slice + j*32 + 16 + t*4);
        uint4 vb  = Vp_b [0];
        uint4 vbx = Vp_bx[0];
        Bcol[0]  ^= vb.x;  Bcol[1]  ^= vb.y;  Bcol[2]  ^= vb.z;  Bcol[3]  ^= vb.w;
        Bxcol[0] ^= vbx.x; Bxcol[1] ^= vbx.y; Bxcol[2] ^= vbx.z; Bxcol[3] ^= vbx.w;

        xor_salsa8_coop(Bcol, Bxcol, Lt, t);
        xor_salsa8_coop(Bxcol, Bcol, Lt, t);
    }

    /* --- Inverse rearrange: DIAGONAL-column ownership back to flat X[0..31] ---
       Each thread t writes its 4+4 diag-column values back to the positions
       they came from on entry, then reads its contiguous 8-uint chunk.
       Symmetric to the entry rearrangement. */
    Lx[ 0u + t + 4u * ((0u + t) & 3u) ] = Bcol[0];
    Lx[ 0u + t + 4u * ((1u + t) & 3u) ] = Bcol[1];
    Lx[ 0u + t + 4u * ((2u + t) & 3u) ] = Bcol[2];
    Lx[ 0u + t + 4u * ((3u + t) & 3u) ] = Bcol[3];
    Lx[16u + t + 4u * ((0u + t) & 3u) ] = Bxcol[0];
    Lx[16u + t + 4u * ((1u + t) & 3u) ] = Bxcol[1];
    Lx[16u + t + 4u * ((2u + t) & 3u) ] = Bxcol[2];
    Lx[16u + t + 4u * ((3u + t) & 3u) ] = Bxcol[3];
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int k = 0; k < 8; k++) X8[k] = Lx[t*8 + k];
    barrier(CLK_LOCAL_MEM_FENCE);

    /* --- Store X back to X_buf (regular contiguous layout for dagtech_post) --- */
    {
        __global uint4 *Xp = (__global uint4 *)(X_ptr + t * 8);
        Xp[0] = (uint4)(X8[0], X8[1], X8[2], X8[3]);
        Xp[1] = (uint4)(X8[4], X8[5], X8[6], X8[7]);
    }
}
