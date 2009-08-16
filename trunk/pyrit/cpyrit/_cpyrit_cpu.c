/*
#
#    Copyright 2008, 2009, Lukas Lueg, lukas.lueg@gmail.com
#
#    This file is part of Pyrit.
#
#    Pyrit is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    Pyrit is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with Pyrit.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Python.h>
#include <stdint.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include "_cpyrit_cpu.h"

struct pmk_ctr
{
    SHA_CTX ctx_ipad;
    SHA_CTX ctx_opad;
    uint32_t e1[5];
    uint32_t e2[5];
};

typedef struct
{
    PyObject_HEAD
} CPUDevice;

static PyObject *PlatformString;
static void (*prepare_pmk)(const unsigned char *essid_pre, int essidlen, const unsigned char *password, int passwdlen, struct pmk_ctr *ctr) = NULL;
static int (*finalize_pmk)(struct pmk_ctr *ctr) = NULL;

static uint32_t sha1_constants[6][4];

#ifdef COMPILE_PADLOCK
    struct xsha1_ctx {
        unsigned int state[32];
        unsigned char inputbuffer[20+64];
    } __attribute__((aligned(16)));

    #include <sys/ucontext.h>
    #include <signal.h>
    #include <errno.h>
    #include <sys/mman.h>

    // Snippet taken from OpenSSL 0.9.8
    static int
    detect_padlock(void)
    {
        char vendor_string[16];
        unsigned int eax, edx;

        eax = 0x00000000;
        vendor_string[12] = 0;
        asm volatile (
                "pushl  %%ebx\n"
                "cpuid\n"
                "movl   %%ebx,(%%edi)\n"
                "movl   %%edx,4(%%edi)\n"
                "movl   %%ecx,8(%%edi)\n"
                "popl   %%ebx"
                : "+a"(eax) : "D"(vendor_string) : "ecx", "edx");
        if (strcmp(vendor_string, "CentaurHauls") != 0)
                return 0;

        /* Check for Centaur Extended Feature Flags presence */
        eax = 0xC0000000;
        asm volatile ("pushl %%ebx; cpuid; popl %%ebx"
                : "+a"(eax) : : "ecx", "edx");
        if (eax < 0xC0000001)
                return 0;

        /* Read the Centaur Extended Feature Flags */
        eax = 0xC0000001;
        asm volatile ("pushl %%ebx; cpuid; popl %%ebx"
                : "+a"(eax), "=d"(edx) : : "ecx");

        return (edx & 0x300) == 0x300;
    }

    // This instruction is not available on all CPUs (do we really care about those?)
    // Therefor it is only used on padlock-enabled machines
    static inline int bswap(int x)
    {
        asm volatile ("bswap %0":
                    "=r" (x):
                    "0" (x));
        return x;
    }

    static int
    padlock_xsha1(unsigned char *input, unsigned int *output, int done, int count)
    {
        int volatile d = done;
        asm volatile ("xsha1"
                  : "+S"(input), "+D"(output), "+a"(d)
                  : "c"(count));
        return d;
    }

    // This handler will ignore the SIGSEGV deliberately caused by padlock_xsha1_prepare
    static void
    segv_action(int sig, siginfo_t *info, void *uctxp)            
    {
        MCTX_EIP((ucontext_t*)uctxp) += 4;
    }

    // REP XSHA1 is crashed into the mprotect'ed page so we can
    // steal the state at *EDI before finalizing.
    static int
    padlock_xsha1_prepare(const unsigned char *input, SHA_CTX *output)
    {
        size_t page_size = getpagesize(), buffersize = 2 * page_size, hashed = 0;
        struct sigaction act, oldact;
        unsigned char *cnt, *inputbuffer;
        
        inputbuffer = mmap(0, buffersize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

        mprotect(inputbuffer + page_size, page_size, PROT_NONE);

        struct xsha1_ctx ctx;
        ((int*)ctx.state)[0] = 0x67452301;
        ((int*)ctx.state)[1] = 0xEFCDAB89;
        ((int*)ctx.state)[2] = 0x98BADCFE;
        ((int*)ctx.state)[3] = 0x10325476;
        ((int*)ctx.state)[4] = 0xC3D2E1F0;

        cnt = inputbuffer + page_size - (64*2);
        memcpy(cnt, input, 64);
        memset(&act, 0, sizeof(act));
        act.sa_sigaction = segv_action;
        act.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &act, &oldact);
        hashed = padlock_xsha1(cnt, ctx.state, 0, (64*2));        
        sigaction(SIGSEGV, &oldact, NULL);

        munmap(inputbuffer, buffersize);
        
        output->h0 = ((int*)ctx.state)[0];
        output->h1 = ((int*)ctx.state)[1];
        output->h2 = ((int*)ctx.state)[2];
        output->h3 = ((int*)ctx.state)[3];
        output->h4 = ((int*)ctx.state)[4];

        return hashed;
    }

    // Now lie about the total number of bytes hashed by this call to get the correct hash
    static inline int
    padlock_xsha1_finalize(SHA_CTX *prestate, unsigned char *buffer)
    {
        struct xsha1_ctx ctx;
        size_t hashed;

        ((int*)ctx.state)[0] = prestate->h0;
        ((int*)ctx.state)[1] = prestate->h1;
        ((int*)ctx.state)[2] = prestate->h2;
        ((int*)ctx.state)[3] = prestate->h3;
        ((int*)ctx.state)[4] = prestate->h4;
            
        memcpy(ctx.inputbuffer, buffer, 20);
        hashed = padlock_xsha1(ctx.inputbuffer, ctx.state, 64, 64+20);

        ((int*)buffer)[0] = bswap(ctx.state[0]); ((int*)buffer)[1] = bswap(ctx.state[1]);
        ((int*)buffer)[2] = bswap(ctx.state[2]); ((int*)buffer)[3] = bswap(ctx.state[3]);
        ((int*)buffer)[4] = bswap(ctx.state[4]);

        return hashed;
    }

    static void
    prepare_pmk_padlock(const unsigned char *essid_pre, int essidlen, const unsigned char *password, int passwdlen, struct pmk_ctr *ctr)
    {
        int i;
        unsigned char pad[64], essid[32+4];

        essidlen = essidlen > 32 ? 32 : essidlen;
        passwdlen = passwdlen > 64 ? 64 : passwdlen;

        memcpy(essid, essid_pre, essidlen);
        memset(essid + essidlen, 0, sizeof(essid) - essidlen);

        memcpy(pad, password, passwdlen);
        memset(pad + passwdlen, 0, sizeof(pad) - passwdlen);
        for (i = 0; i < 16; i++)
            ((unsigned int*)pad)[i] ^= 0x36363636;
        padlock_xsha1_prepare(pad, &ctr->ctx_ipad);
        for (i = 0; i < 16; i++)
            ((unsigned int*)pad)[i] ^= 0x6A6A6A6A;
        padlock_xsha1_prepare(pad, &ctr->ctx_opad);

        essid[essidlen + 4 - 1] = '\1';
        HMAC(EVP_sha1(), password, passwdlen, essid, essidlen + 4, (unsigned char*)ctr->e1, NULL);

        essid[essidlen + 4 - 1] = '\2';
        HMAC(EVP_sha1(), password, passwdlen, essid, essidlen + 4, (unsigned char*)ctr->e2, NULL);
    }

    static int
    finalize_pmk_padlock(struct pmk_ctr *ctr)
    {
        int i, j;
        unsigned int e1_buffer[5], e2_buffer[5];

        memcpy(e1_buffer, ctr->e1, 20);
        memcpy(e2_buffer, ctr->e2, 20);
        for (i = 0; i < 4096-1; i++)
        {
            padlock_xsha1_finalize(&ctr->ctx_ipad, (unsigned char*)e1_buffer);
            padlock_xsha1_finalize(&ctr->ctx_opad, (unsigned char*)e1_buffer);
            for (j = 0; j < 5; j++)
                ctr->e1[j] ^= e1_buffer[j];
            
            padlock_xsha1_finalize(&ctr->ctx_ipad, (unsigned char*)e2_buffer);
            padlock_xsha1_finalize(&ctr->ctx_opad, (unsigned char*)e2_buffer);
            for (j = 0; j < 3; j++)
                ctr->e2[j] ^= e2_buffer[j];
        }
        
        return 1;
    }

#endif // COMPILE_PADLOCK


#ifdef COMPILE_SSE2
    extern int detect_sse2(void);
    extern int sse2_sha1_update(uint32_t ctx[4*5+4*6], uint32_t data[4*16], uint32_t wrkbuf[4*80]) __attribute__ ((regparm(3)));
    extern int sse2_sha1_finalize(uint32_t ctx[4*5+4*6], uint32_t digests[4*5]) __attribute__ ((regparm(2)));

    static int
    finalize_pmk_sse2(struct pmk_ctr *ctr)
    {
        int i, j, k;
        uint32_t ctx_ipad[4*5]      __attribute__ ((aligned (16)));
        uint32_t ctx_opad[4*5]      __attribute__ ((aligned (16)));
        uint32_t sha1_ctx[4*5+4*6]  __attribute__ ((aligned (16)));
        uint32_t e1_buffer[4*16]    __attribute__ ((aligned (16)));
        uint32_t e2_buffer[4*16]    __attribute__ ((aligned (16)));
        uint32_t wrkbuf[4*80]       __attribute__ ((aligned (16)));

        memset(e1_buffer, 0, sizeof(e1_buffer));
        memset(e2_buffer, 0, sizeof(e2_buffer));
        memcpy(&sha1_ctx[4*5], sha1_constants, sizeof(sha1_constants));

        // Interleave four ipads, opads and first-round-PMKs to local buffers
        for (i = 0; i < 4; i++)
        {
            ctx_ipad[i+ 0] = ctr[i].ctx_ipad.h0;
            ctx_ipad[i+ 4] = ctr[i].ctx_ipad.h1;
            ctx_ipad[i+ 8] = ctr[i].ctx_ipad.h2;
            ctx_ipad[i+12] = ctr[i].ctx_ipad.h3;
            ctx_ipad[i+16] = ctr[i].ctx_ipad.h4;
     
            ctx_opad[i+ 0] = ctr[i].ctx_opad.h0;
            ctx_opad[i+ 4] = ctr[i].ctx_opad.h1;
            ctx_opad[i+ 8] = ctr[i].ctx_opad.h2;
            ctx_opad[i+12] = ctr[i].ctx_opad.h3;
            ctx_opad[i+16] = ctr[i].ctx_opad.h4;

            e1_buffer[20+i] = e2_buffer[20+i] = 0x80; // Terminator bit
            e1_buffer[60+i] = e2_buffer[60+i] = 0xA0020000; // size = (64+20)*8
            for (j = 0; j < 5; j++)
            {
                e1_buffer[j*4 + i] = ctr[i].e1[j];
                e2_buffer[j*4 + i] = ctr[i].e2[j];
            }
        }
        
        // Process through SSE2 and de-interleave back to ctr
        for (i = 0; i < 4096-1; i++)
        {
            memcpy(sha1_ctx, ctx_ipad, 4 * 5 * sizeof(uint32_t));
            sse2_sha1_update(sha1_ctx, e1_buffer, wrkbuf);
            sse2_sha1_finalize(sha1_ctx, e1_buffer);
            
            memcpy(sha1_ctx, ctx_opad, 4 * 5 * sizeof(uint32_t));
            sse2_sha1_update(sha1_ctx, e1_buffer, wrkbuf);
            sse2_sha1_finalize(sha1_ctx, e1_buffer);

            memcpy(sha1_ctx, ctx_ipad, 4 * 5 * sizeof(uint32_t));
            sse2_sha1_update(sha1_ctx, e2_buffer, wrkbuf);
            sse2_sha1_finalize(sha1_ctx, e2_buffer);
            
            memcpy(sha1_ctx, ctx_opad, 4 * 5 * sizeof(uint32_t));
            sse2_sha1_update(sha1_ctx, e2_buffer, wrkbuf);
            sse2_sha1_finalize(sha1_ctx, e2_buffer);
            
            for (j = 0; j < 4; j++)
            {
                for (k = 0; k < 5; k++)
                {
                    ctr[j].e1[k] ^= e1_buffer[k*4 + j];
                    ctr[j].e2[k] ^= e2_buffer[k*4 + j];
                }
            }
        }

        return 4;
    }

#endif // COMPILE_SSE2

static void
prepare_pmk_openssl(const unsigned char *essid_pre, int essidlen, const unsigned char *password, int passwdlen, struct pmk_ctr *ctr)
{
    int i;
    unsigned char pad[64], essid[32+4];

    essidlen = essidlen > 32 ? 32 : essidlen;
    passwdlen = passwdlen > 64 ? 64 : passwdlen;

    memcpy(essid, essid_pre, essidlen);
    memset(essid + essidlen, 0, sizeof(essid) - essidlen);
    
    memcpy(pad, password, passwdlen);
    memset(pad + passwdlen, 0, sizeof(pad) - passwdlen);
    for( i = 0; i < 16; i++ )
	    ((unsigned int*)pad)[i] ^= 0x36363636;
    SHA1_Init(&ctr->ctx_ipad);
    SHA1_Update(&ctr->ctx_ipad, pad, 64);
    for( i = 0; i < 16; i++ )
	    ((unsigned int*)pad)[i] ^= 0x6A6A6A6A;
    SHA1_Init(&ctr->ctx_opad);
    SHA1_Update(&ctr->ctx_opad, pad, 64);

    essid[essidlen + 4 - 1] = '\1';
    HMAC(EVP_sha1(), password, passwdlen, essid, essidlen + 4, (unsigned char*)ctr->e1, NULL);
    
    essid[essidlen + 4 - 1] = '\2';
    HMAC(EVP_sha1(), password, passwdlen, essid, essidlen + 4, (unsigned char*)ctr->e2, NULL);
}

static int
finalize_pmk_openssl(struct pmk_ctr *ctr)
{
    int i, j;
    SHA_CTX ctx;
    unsigned int e1_buffer[5], e2_buffer[5];

    memcpy(e1_buffer, ctr->e1, 20);
    memcpy(e2_buffer, ctr->e2, 20);
    for(i = 0; i < 4096-1; i++)
    {
        memcpy(&ctx, &ctr->ctx_ipad, sizeof(ctx));
        SHA1_Update(&ctx, (unsigned char*)e1_buffer, 20);
        SHA1_Final((unsigned char*)e1_buffer, &ctx);
        
        memcpy(&ctx, &ctr->ctx_opad, sizeof(ctx));
        SHA1_Update(&ctx, (unsigned char*)e1_buffer, 20);
        SHA1_Final((unsigned char*)e1_buffer, &ctx);

        for (j = 0; j < 5; j++)
            ctr->e1[j] ^= e1_buffer[j];
        
        memcpy(&ctx, &ctr->ctx_ipad, sizeof(ctx));
        SHA1_Update(&ctx, (unsigned char*)e2_buffer, 20);
        SHA1_Final((unsigned char*)e2_buffer, &ctx);
        
        memcpy(&ctx, &ctr->ctx_opad, sizeof(ctx));
        SHA1_Update(&ctx, (unsigned char*)e2_buffer, 20);
        SHA1_Final((unsigned char*)e2_buffer, &ctx);

        for (j = 0; j < 3; j++)
            ctr->e2[j] ^= e2_buffer[j]; 
    }
    
    return 1;
}

static PyObject *
cpyrit_getPlatform(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
        return NULL;
    Py_INCREF(PlatformString);
    return PlatformString;
}

static PyObject *
cpyrit_solve(PyObject *self, PyObject *args)
{
    unsigned char *essid, *passwd;
    PyObject *passwd_seq, *passwd_obj, *essid_obj, *result;
    int i, arraysize, essidlen, passwdlen;
    struct pmk_ctr *pmk_buffer, *t;

    if (!PyArg_ParseTuple(args, "OO", &essid_obj, &passwd_seq)) return NULL;
    passwd_seq = PyObject_GetIter(passwd_seq);
    if (!passwd_seq) return NULL;
    
    essid = (unsigned char*)PyString_AsString(essid_obj);
    essidlen = PyString_Size(essid_obj);
    if (essid == NULL || essidlen < 1 || essidlen > 32)
    {
        Py_DECREF(passwd_seq);
        PyErr_SetString(PyExc_ValueError, "ESSID must be a string between 1 and 32 bytes.");
        return NULL;
    }
    
    arraysize = 0;
    pmk_buffer = NULL;
    while ((passwd_obj=PyIter_Next(passwd_seq)))
    {
        if (arraysize % 100 == 0)
        {
            // Step-size must be aligned to four entries so finalize_pmk_sse2 has air to breath
            t = PyMem_Realloc(pmk_buffer, sizeof(struct pmk_ctr) * (arraysize+100));
            if (!t)
            {
                Py_DECREF(passwd_obj);
                Py_DECREF(passwd_seq);
                PyMem_Free(pmk_buffer);
                PyErr_NoMemory();
                return NULL;
            }
            pmk_buffer = t;
        }
        passwd = (unsigned char*)PyString_AsString(passwd_obj);
        passwdlen = PyString_Size(passwd_obj);
        if (passwd == NULL || passwdlen < 8 || passwdlen > 63)
        {
            Py_DECREF(passwd_obj);
            Py_DECREF(passwd_seq);
            PyMem_Free(pmk_buffer);
            PyErr_SetString(PyExc_ValueError, "All passwords must be strings between 8 and 63 characters");
            return NULL;
        }
        prepare_pmk(essid, essidlen, passwd, passwdlen, &pmk_buffer[arraysize]);
        Py_DECREF(passwd_obj);
        arraysize++;
    }
    Py_DECREF(passwd_seq);
    
    if (arraysize > 0)
    {
        Py_BEGIN_ALLOW_THREADS;
        i = 0;
        do
            i += finalize_pmk(&pmk_buffer[i]);
        while (i < arraysize);
        Py_END_ALLOW_THREADS;

        result = PyTuple_New(arraysize);
        for (i = 0; i < arraysize; i++)
            PyTuple_SetItem(result, i, Py_BuildValue("s#", pmk_buffer[i].e1, 32));
    } else {
        result = PyTuple_New(0);
    }

    PyMem_Free(pmk_buffer);

    return result;
}

static PyMethodDef CPUDevice_methods[] =
{
    {"solve", (PyCFunction)cpyrit_solve, METH_VARARGS, "Calculate PMKs from ESSID and iterable of strings."},
    {NULL, NULL}
};

static PyTypeObject CPUDevice_type = {
    PyObject_HEAD_INIT(NULL)
    0,                          /*ob_size*/
    "_cpyrit_cpu.CPUDevice",    /*tp_name*/
    sizeof(CPUDevice),          /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    0,                          /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash*/
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT          /*tp_flags*/
     | Py_TPFLAGS_BASETYPE,
    0,                          /*tp_doc*/
    0,                          /*tp_traverse*/
    0,                          /*tp_clear*/
    0,                          /*tp_richcompare*/
    0,                          /*tp_weaklistoffset*/
    0,                          /*tp_iter*/
    0,                          /*tp_iternext*/
    CPUDevice_methods,          /*tp_methods*/
    0,                          /*tp_members*/
    0,                          /*tp_getset*/
    0,                          /*tp_base*/
    0,                          /*tp_dict*/
    0,                          /*tp_descr_get*/
    0,                          /*tp_descr_set*/
    0,                          /*tp_dictoffset*/
    0,                          /*tp_init*/
    0,                          /*tp_alloc*/
    0,                          /*tp_new*/
    0,                          /*tp_free*/
    0,                          /*tp_is_gc*/
};

static PyMethodDef CPyritCPUMethods[] = {
    {"getPlatform", cpyrit_getPlatform, METH_VARARGS, "Determine CPU-type/name"},
    {NULL, NULL, 0, NULL}
};

static void pathconfig(void)
{
    #ifdef COMPILE_PADLOCK
        if (detect_padlock())
        {
            PlatformString = PyString_FromString("VIA Padlock");
            prepare_pmk = prepare_pmk_padlock;
            finalize_pmk = finalize_pmk_padlock;
            return;
        }
    #endif
    #ifdef COMPILE_SSE2
        if (detect_sse2())
        {
            PlatformString = PyString_FromString("SSE2");
            prepare_pmk = prepare_pmk_openssl;
            finalize_pmk = finalize_pmk_sse2;
            return;
        }
    #endif 

    PlatformString = PyString_FromString("x86");
    prepare_pmk = prepare_pmk_openssl;
    finalize_pmk = finalize_pmk_openssl;
}

PyMODINIT_FUNC
init_cpyrit_cpu(void)
{
    PyObject *m;
    int i;

    for (i = 0; i < 4; i++)
    {
        sha1_constants[0][i] = 0x5A827999; /* const_stage0 */
        sha1_constants[1][i] = 0x6ED9EBA1; /* const_stage1 */
        sha1_constants[2][i] = 0x8F1BBCDC; /* const_stage2 */
        sha1_constants[3][i] = 0xCA62C1D6; /* const_stage3 */
        sha1_constants[4][i] = 0xFF00FF00; /* const_ff00   */
        sha1_constants[5][i] = 0x00FF00FF; /* const_00ff   */
    }

    pathconfig();

    CPUDevice_type.tp_getattro = PyObject_GenericGetAttr;
    CPUDevice_type.tp_setattro = PyObject_GenericSetAttr;
    CPUDevice_type.tp_alloc  = PyType_GenericAlloc;
    CPUDevice_type.tp_new = PyType_GenericNew;
    CPUDevice_type.tp_free = _PyObject_Del;  
    if (PyType_Ready(&CPUDevice_type) < 0)
	    return;

    m = Py_InitModule("_cpyrit_cpu", CPyritCPUMethods);

    Py_INCREF(&CPUDevice_type);
    PyModule_AddObject(m, "CPUDevice", (PyObject*)&CPUDevice_type);

}
