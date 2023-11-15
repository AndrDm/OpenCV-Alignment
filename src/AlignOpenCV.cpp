//==================================================================================
//
// Title:		AlignOpenCV
// Purpose:		Alignment demo
//
//==================================================================================

#include <chrono>
#include <print>
#include <Windows.h>
#include <Psapi.h>
#include <immintrin.h>

#define WAIT 0 //Enable if you would like to see images and stop between tests

#define CV_AVX2 1
#include "OpenCV/opencv.hpp"
#include "OpenCV/core/hal/intrin.hpp"

#ifdef _DEBUG
#pragma comment( lib, "OpenCV/opencv_world481d" )
#else
#pragma comment( lib, "OpenCV/opencv_world481" )
#endif

extern "C" uint64_t fnProcessA(void* dst, void* src, size_t Bytes);
extern "C" uint64_t fnProcessU(void* dst, void* src, size_t Bytes);
#pragma comment( lib, "Assembler/AVX2Process" )

#define ALIGN(__intptr, __align) ((__intptr) - 1u + (__align)) & -(__align)

#define HEIGHT 32768
#define WIDTH 32768
#define LINE_WIDTH WIDTH
#define LARGE (HEIGHT*WIDTH)
#define MS duration_cast<milliseconds>

using namespace cv;
using namespace std;
using namespace std::chrono;

#define NUM_PROC_THREADS 16

static HANDLE hProcessThreads[NUM_PROC_THREADS] = { 0 };
static HANDLE hStartSemaphores[NUM_PROC_THREADS] = { 0 };
static HANDLE hStopSemaphores[NUM_PROC_THREADS] = { 0 };

typedef struct{
    int ct;
    void* src, * dest;
    size_t size;
} mt_proc_t;

mt_proc_t mtParamters[NUM_PROC_THREADS] = { 0 };

//---------------------------------------
// Processing Threads
//---------------------------------------
DWORD WINAPI thread_proc(LPVOID param)
{
    mt_proc_t* p = (mt_proc_t*)param;

    while (1){
        WaitForSingleObject(hStartSemaphores[p->ct], INFINITE);
        fnProcessA(p->dest, p->src, p->size);//pointers must be aligned!
        ReleaseSemaphore(hStopSemaphores[p->ct], 1, NULL);
    }

    return 0;
}

void startProcessingThreads()
{
    for (int ctr = 0; ctr < NUM_PROC_THREADS; ctr++){
        hStartSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
        hStopSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
        mtParamters[ctr].ct = ctr;
        hProcessThreads[ctr] = CreateThread(0, 0, thread_proc, &mtParamters[ctr], 0, NULL);
    }
}

void* runProcessingThreads(void* dest, void* src, size_t bytes)
{
    //set up parameters
    for (int ctr = 0; ctr < NUM_PROC_THREADS; ctr++){
        mtParamters[ctr].dest = (char*)dest + ctr * bytes / NUM_PROC_THREADS;
        mtParamters[ctr].src = (char*)src + ctr * bytes / NUM_PROC_THREADS;
        mtParamters[ctr].size = (ctr + 1) * bytes / NUM_PROC_THREADS - ctr * bytes / NUM_PROC_THREADS;
    }

    //release semaphores to start computation
    for (int ctr = 0; ctr < NUM_PROC_THREADS; ctr++) ReleaseSemaphore(hStartSemaphores[ctr], 1, 0);
    //wait for all threads to finish
    WaitForMultipleObjects(NUM_PROC_THREADS, hStopSemaphores, TRUE, INFINITE);

    return dest;
}

void stopProcessingThreads()
{
    for (int ctr = 0; ctr < NUM_PROC_THREADS; ctr++){
        CloseHandle(hStartSemaphores[ctr]);
        CloseHandle(hStopSemaphores[ctr]);
    }
}

uint64_t fnIntrinsics(uint8_t *Dst, uint8_t *Src, size_t Bytes)
{
    size_t Blocks = Bytes >> 7;
    __declspec(align(32)) uint8_t U255[32]; fill_n(U255, sizeof(U255) / sizeof(*U255), 255);
    uint64_t start, end;
    start = __rdtsc();
    __m256i u255 = _mm256_load_si256((__m256i*)U255); //fill with constant
    assume_aligned<64>(Src);
    assume_aligned<64>(Dst);
    for (size_t i = 0; i < Blocks; i++) {
        // There is no penalty to using VMOVDQU when the address is aligned.
        __m256i b0 = _mm256_load_si256((__m256i*)Src); //32 Source pixels
        __m256i b1 = _mm256_load_si256((__m256i*)(Src + 32)); //next 32 pixels
        __m256i b2 = _mm256_load_si256((__m256i*)(Src + 64)); //and so on
        __m256i b3 = _mm256_load_si256((__m256i*)(Src + 96)); //4x unroll

        b0 = _mm256_subs_epi8(u255, b0);
        b1 = _mm256_subs_epi8(u255, b1);
        b2 = _mm256_subs_epi8(u255, b2);
        b3 = _mm256_subs_epi8(u255, b3);

        _mm256_store_si256((__m256i*)Dst, b0); //or vmovntdq
        _mm256_store_si256((__m256i*)(Dst + 32), b1);
        _mm256_store_si256((__m256i*)(Dst + 64), b2);
        _mm256_store_si256((__m256i*)(Dst + 96), b3);
        Src += 128; Dst += 128;
    }
    end = __rdtsc();

    return end - start;
}

uint64_t OpenCVIntrinsics(uchar *Dst, uchar *Src, size_t Bytes)
{
    size_t Blocks = Bytes >> 7;
    __declspec(align(32)) uint8_t U255[32];
    fill_n(U255, sizeof(U255) / sizeof(*U255), 255); // fill with 255
    uint64_t start, end;
    unsigned int id;
    start = __rdtscp(&id);
    v_uint8x32 u255 = v256_load_aligned((uchar*)U255);
    assume_aligned<64>(Src);
    assume_aligned<64>(Dst);

    for (size_t i = 0; i < Blocks; i++) {
        v_uint8x32 pix0 = v256_load_aligned(Src);
        v_uint8x32 pix1 = v256_load_aligned(Src+16);
        v_uint8x32 pix2 = v256_load_aligned(Src+32);
        v_uint8x32 pix3 = v256_load_aligned(Src+48);
        v_uint8x32 neg0 = u255 - pix0;
        v_uint8x32 neg1 = u255 - pix1;
        v_uint8x32 neg2 = u255 - pix2;
        v_uint8x32 neg3 = u255 - pix3;
        v_store_aligned(Dst, neg0);
        v_store_aligned(Dst+16, neg1);
        v_store_aligned(Dst+32, neg2);
        v_store_aligned(Dst+48, neg3);
        Src += 128; Dst += 128;
    }

    end = __rdtscp(&id);

    return end - start;
}


#pragma optimize( "", off )
int main()
{
    int Width, Height;
    print("Hello OpenCV!\n");

    //==============================================================================
    // Open Image:
    //
    Mat Src = imread("Building.jpg", CV_8U);
    
    if (WAIT) { imshow("Building", Src); waitKey(0); }

    //==============================================================================
    // Create Destination Image and copy inverted pixel by pixel like bitwise_not();
    //
    Width = Src.cols; Height = Src.rows;

    Mat Dst(Height, Width, CV_8U);

    //Mat Dst2 = 255 - Src;
    for (int i = 0; i < Width * Height; i++) Dst.data[i] = 255 - Src.data[i];
    printf("Src ptr = 0x%p, Dst ptr = 0x%p, Hit Enter to continue\n", Src.data, Dst.data);

    if (WAIT) { imshow("Building Dst", Dst); waitKey(0); }

    //==============================================================================
    // Create Aligned Destination Image
    //
    int AlignedWidth = ALIGN(Width, 64);
    println("Alignment: Width = {}, aligned width = {}", Width, AlignedWidth);
    uint8_t *DstPtr;

    DstPtr = (uint8_t*)_aligned_malloc(Height * AlignedWidth * sizeof(uint8_t), 64);
    Mat DstAligned(Height, Width, CV_8U, DstPtr, AlignedWidth);

    //==============================================================================
    // Processing Pixel by Pixel from unaligned to aligned - Wrong way:
    //
    for (int i = 0; i < Width * Height; i++)  DstAligned.data[i] = 255 - Src.data[i];

    if (WAIT) { imshow("Building Dst - Wrong", DstAligned); waitKey(0); }

    //==============================================================================
    // Processing with LineWidth - Correct way:
    //
    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            DstAligned.data[y * AlignedWidth + x] = 255 - Src.data[y * Width + x];
        }
    }

    //==============================================================================
    // Correct way alternatively with pointers:
    //
    uint8_t *pSrc, *pDst;
    pSrc = Src.data;
    pDst = DstAligned.data;

    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            *pDst++ = 255 - *pSrc++;
        }
        pDst += AlignedWidth - Width;
    }
    if (WAIT) { imshow("Building Dst - Correct", DstAligned); waitKey(0); }

    //==============================================================================
    // Check if the image contgnuos or not:
    //
    if (Src.isContinuous()) println("Src is continuous");
    if (!DstAligned.isContinuous()) println("Dst is not continuous");

    //==============================================================================
    // Allocate large images, aligned to Page boundary (4K):
    //
    if (WAIT) { println("\nBefore _aligned_malloc(), hit Enter to continue"); getchar(); }
    uint8_t* LargeSrc = (uint8_t*)_aligned_malloc(LARGE * sizeof(uint8_t), 4096);
    uint8_t* LargeDst = (uint8_t*)_aligned_malloc(LARGE * sizeof(uint8_t), 4096);
    if (WAIT) { println("After _aligned_malloc(), hit Enter to continue"); getchar(); }
    if (!LargeSrc || !LargeDst) { println("Not enough Memory"); return 1; }
    Mat LargeSrcOCV(HEIGHT, WIDTH, CV_8U, LargeSrc, LINE_WIDTH);
    Mat LargeDstOCV(HEIGHT, WIDTH, CV_8U, LargeDst, LINE_WIDTH);

    //==============================================================================
    // Page Faults and Memory before and after memcpy:
    //
    PROCESS_MEMORY_COUNTERS memCounter;
    BOOL result = GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));
    println("\nPage Faults before 1st memcpy - {}", memCounter.PageFaultCount);
    println("Working Set Size before 1st memcpy - {}", memCounter.WorkingSetSize);

    auto start = system_clock::now();
    memcpy(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    auto end = system_clock::now();
    
    if (WAIT) { println("after memcpy(), hit Enter to continue"); getchar(); }

    GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));
    println("Page Faults after 1st memcpy - {}", memCounter.PageFaultCount);
    println("Working Set Size after 1st memcpy - {}", memCounter.WorkingSetSize);
    println("\nmemcpy 1st call - {} ms", MS(end - start).count());
    if (WAIT) getchar();

    start = system_clock::now();
    memcpy(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    end = system_clock::now();
    println("memcpy 2nd call - {} ms", MS(end - start).count());


    //==============================================================================
    // 64 Bytes Cache Lines Test
    //
    memcpy(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));

    start = system_clock::now();
    for (int i = 0; i < LARGE / 32; i+= 2) LargeDst[i] = LargeSrc[i];
    end = system_clock::now();
    println("\nCopy every 2nd elementh - {} ms", MS(end - start).count());

    start = system_clock::now();
    for (int i = 0; i < LARGE; i += 64) LargeDst[i] = LargeSrc[i];
    end = system_clock::now();
    println("Copy every 64th elementh - {} ms", MS(end - start).count());

    
    //==============================================================================
    // Processing with unaligned commands
    //
    start = system_clock::now();
    uint64_t ticksU = fnProcessU(LargeDst+1, LargeSrc+1, LARGE * sizeof(uint8_t)-128);
    end = system_clock::now();
    println("\nProcessing - unaligned call -  {} ms ({} ticks)", MS(end - start).count(), ticksU);


    //==============================================================================
    // Processing with aligned commands
    //
    start = system_clock::now();
    uint64_t ticksA = fnProcessA(LargeDst, LargeSrc, LARGE * sizeof(uint8_t)); //!! MUST BE ALIGNED !!
    end = system_clock::now();
    println("Processing - aligned call - {} ms ({} ticks)", MS(end - start).count(), ticksA);


    //==============================================================================
    // Processing with intrinsics
    //
    start = system_clock::now();
    uint64_t ticksI = fnIntrinsics(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    end = system_clock::now();
    println("\nProcessing Intrinsics - {} ms ({} ticks)", MS(end - start).count(), ticksI);


    //==============================================================================
    // Processing with OpenCV intrinsics
    //
    start = system_clock::now();
    uint64_t ticksO = OpenCVIntrinsics(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    end = system_clock::now();
    println("Processing OpenCV Intrinsics - {} ms ({} ticks)", MS(end - start).count(), ticksO);


    //==============================================================================
    // Processing with OpenCV
    //
    start = system_clock::now();
    LargeDstOCV = 255 - LargeSrcOCV; 
    end = system_clock::now();
    println("Processing OpenCV - {} ms", MS(end - start).count());

    //==============================================================================
    // Multithreaded with OpenMP - Aligned
    //
    uint8_t* srcTmp = LargeSrc;
    uint8_t* dstTmp = LargeDst;
    start = system_clock::now();
#pragma omp parallel for num_threads(NUM_PROC_THREADS)
    for (int x = 0; x < HEIGHT; x++) {
        fnProcessA(dstTmp, srcTmp, WIDTH * sizeof(uint8_t));
        dstTmp += WIDTH;
        srcTmp += WIDTH;
    }
    end = system_clock::now();
    println("\nParallel Processing with OpenMP Aligned - {} ms ", MS(end - start).count());
 

    //==============================================================================
    // Multithreaded with OpenMP - Unaligned
    //
    srcTmp = LargeSrc;
    dstTmp = LargeDst;
    start = system_clock::now();
#pragma omp parallel for num_threads(NUM_PROC_THREADS)
    for (int x = 0; x < HEIGHT; x++) {
        fnProcessU(dstTmp, srcTmp, WIDTH * sizeof(uint8_t));
        dstTmp += WIDTH;
        srcTmp += WIDTH;
    }
    end = system_clock::now();
    println("Parallel Processing with OpenMP - nonaligned {} ms ", MS(end - start).count());


    //==============================================================================
    // Multithreaded processing - classic
    //
    startProcessingThreads();

    start = system_clock::now();
    runProcessingThreads(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    end = system_clock::now();

    stopProcessingThreads();

    println("Done.");
    if(WAIT)getchar();

    //==============================================================================
    // Aligned allocation requires aligned free:
    //
    _aligned_free(DstPtr);
    _aligned_free(LargeSrc);
    _aligned_free(LargeDst);

}
#pragma optimize( "", on )
