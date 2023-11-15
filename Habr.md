В разговорах с коллегами, а также по отдельным постам на форумах я заметил, что даже относительно опытные разработчики порой не очень понимают особенности хранения изображений в памяти. Если вы знаете, что такое выравнивание на границу 64-x байт, а также термины типа «длина или шаг строки (LineWidth/StepWidth, Stride)», «выравнивающий промежуток (Alignment Gap)», кроме того в курсе размеров линий кэша и страниц, то вам, вероятно, не будет интересно, а остальные, особенно те, кто интересуется обработкой изображений — могут ознакомиться с предлагаемым материалом, и, возможно найдут для себя что-то новое и полезное. Под катом будет немножко кода на Си и ассемблере, пара LabVIEW скриншотов, предполагается также, что у читателя есть базовые знания OpenCV. Для экспериментов понадобится компьютер с камушком, поддерживающим AVX2, всё это под Windows 11 x64.

---

Как обычно — дисклеймер о том, что я никак не связан ни с одним упоминающимся ниже продуктом, рекламной нагрузки этот чисто технический пост не несёт.

Обычно знакомство с OpenCV начинают с кода вроде этого:

```cpp
#include "OpenCV/opencv.hpp"
using namespace cv;
//...
Mat Src = imread("Building.jpg");
imshow("Building", Src);
waitKey(0);
```

«Building.jpg» — это изображение из примеров OpenCV (обычно используется для упражнений с преобразованием Хафа). Предположим, мы хотим сделать что-то с картинкой попиксельно (ну, к примеру, банально инвертировать) и записать результат в Dst, это можно сделать одиночным проходом по всем пикселам и я в курсе, что можно просто написать
```cpp
Mat Dst = 255 - Src;
```
но это просто учебный пример, вы ниже поймёте, зачем я так:

```cpp
Mat Src = imread("Building.jpg", CV_8U); //load as grayscale

int Width = Src.cols;
int Height = Src.rows;

Mat Dst(Height, Width, CV_8U);

for (int i = 0; i < Width * Height; i++) Dst.data[i] = 255 - Src.data[i];

imshow("Building Dst", Dst);
println("Hit Enter to continue");
waitKey(0);
```

Выше я читаю картинку как серую одноканальную и восьмибитную. Пока всё ожидаемо, негатив, как есть:

![негатив](https://habrastorage.org/webt/af/bg/p8/afbgp8lxydroqhnxothsum630d8.jpeg)

Если вы посмотрите на то, куда указывают адреса «нулевых» пикселей Src.data и Dst.data в обоих изображениях, то вы, возможно, заметите, что они выровнены на границу шестидесяти четырёх байт (это значит, что адрес делится на 64 без остатка, последние шесть битов у этих адресов сброшены, стало быть, если последние две цифры в шестнадцатеричном представлении 00, 40, 80 или С0, то адрес однозначно делится на 64):

```cpp
    printf("Src ptr = 0x%p, Dst ptr = 0x%p\n", Src.data, Dst.data);
    
    Src ptr = 0x000001B98E6C00C0, Dst ptr = 0x000001B98FC900C0
```

Почему так — несложно понять, взглянув на исходники OpenCV:  [alloc.cpp](https://github.com/opencv/opencv/blob/4.x/modules/core/src/alloc.cpp#L154) и [private.hpp](https://github.com/opencv/opencv/blob/4.x/modules/core/include/opencv2/core/private.hpp#L119), там используется [_aligned_malloc](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc?view=msvc-170), вот так:

```cpp
/* the alignment of all the allocated buffers */
#define  CV_MALLOC_ALIGN    64
//...
void* ptr = _aligned_malloc(size, CV_MALLOC_ALIGN);
```

Зачем нам нужно такое выравнивание? В основном для производительности, поскольку во многих (я очень осторожно скажу «в большинстве») современных архитектур процессоров линии кэша длиной как раз 64 байта. То есть даже если вы запрашиваете одиночный байт из оперативной памяти, то все его шестьдесят три соседа также загружаются в кэш. При обращении к соседу, если он уже в кэше, обращения к памяти не происходит, а вот если он располагается в соседней, ещё не подгруженной линии, то грузятся следующие 64 байта. 

Это на самом деле очень просто проверить — зарезервируйте, скажем, гибибайт памяти, а затем пройдите по началу массива последовательно, либо по всему массиву шагами по 64 байта, замерьте время выполнения, и вы моментально увидите разницу, хотя количество итераций и там и сям одинаково:

```cpp
	for (int i = 0; i < N/64; i++) dst[i] = src[i];

	for (int i = 0; i < N; i+=64) dst[i] = src[i];
```

При обращении по «недостаточно» выровненным данным, скажем при обращении к 32-х байтам (мы хотим обрабатывать 32 байта одной итерацией) при стандартном 16-ти байтном выравнивании, которое предоставляет стандартный malloc(), можно попасть на границу двух линий кэша, что и даст нам пенальти в виде двукратной загрузки при промахе вместо одной. Кроме того, некоторые SIMD команды также требуют выравнивания, AVX-512 оперируют как раз с 64-х байтными данными, так что выровняв данные «по максимуму», мы удовлетворим требованиям всех наборов AVX команд (так как AVX требует выравнивания на 16, а AVX2 — на 32 байта). Это вовсе не означает, что вы совсем не можете обращаться по невыровненным адресам, просто для обращения к выровненным и невыровненным данным у расширений AVX есть разные команды, например пары MOVDQU и MOVDQA либо MOVAPS и MOVUPS, и если использовать команду, предназначенную для загрузки выровненных данных применительно к невыровненным, то будет выброшено исключение, и ваше приложение рухнет вам в руки (или в отладчик).

### Построчное выравнивание

Но мы пойдем чуть дальше нулевого пиксела. Возможно вы заметили вот такой способ создания объекта Mat, с парой дополнительных параметров в виде внешнего указателя и шага:

![](https://habrastorage.org/webt/ui/bn/hu/uibnhutjdyyjhth3j-xs4zk72uc.png)

Давайте сделаем так, чтобы не только самый нулевой (ну или первый — это кто как считает) пиксель, но и **каждая строка** изображения начиналась бы с адреса, кратного 64. Это автоматически происходит при ширине изображения в 64, 128, 192, 256 и так далее пикселей, но в нашем случае картинка размером 868x600 (я не случайно взял именно её) и 868 нацело на 64 ну никак не делится, нам нужна ширина на 28 байтов больше — 896. Чтобы округлить, нам понадобится нехитрый макрос, который я стащил откуда-то из недр StackOverflow:

```cpp
#define ALIGN(__intptr, __align) ((__intptr) - 1u + (__align)) & -(__align)
```

Ну а для выделения памяти я воспользуюсь тем же _aligned_malloc, но теперь уже вот так:

```cpp
int LineWidth = ALIGN(Width, 64);
uint8_t* pDst;
pDst = (uint8_t*)_aligned_malloc(Height * LineWidth * sizeof(uint8_t), 64);
Mat DstAligned(Height, Width, CV_8U, pDst, LineWidth);
println("Alignment: Width = {}, aligned line width = {}", Width, LineWidth);
```

Сам по себе конструктор Mat при таком вызове память не выделяет, он просто использует уже выделенную извне. Следует помнить, что и освобождением памяти OpenCV в этом случае не занимается, так что мы должны будем вызвать _aligned_free(pDst) и именно _aligned_free(), а не просто free().

То есть фактически вместо 868х600 мы выделяем 896х600. В памяти наше изображение будет располагаться теперь вот таким образом:

![](https://habrastorage.org/webt/td/vc/sa/tdvcsakkwu4r_nqrnzs0nq53tca.png)

И вот эти «дырки» (которые в нашем случае занимают 28 байт) называются Alignment Gaps (я приношу мои извинения уважаемой аудитории за иллюстрацию на английском, но я честно не знаю как элегантно перевести этот термин на русский, кроме того, хочу позднее «переиспользовать» картинку для коллег, не владеющих русским языком).

Как бы то ни было, теперь работать с изображением так, как раньше будет ошибочно:

```cpp
for (int i = 0; i < Width * Height; i++)
    DstAligned.data[i] = 255 - Src.data[i];
```

ибо приведёт вот к такому результату:

![Некорректно](https://habrastorage.org/webt/ug/6l/ct/ug6lctkay5_hinvgpkfitlpnkde.jpeg)

Так выглядит типичнейшая ошибка выравнивания, когда мы его либо вообще не учли, либо неправильно задали. Разумеется, если выравнивание организовано для источника и приёмника одинаковым образом, то можно пройти и «сплошным» циклом, захватывая и промежутки между строками (и с учётом того, что ширина увеличилась, иначе мы остановимся, не дойдя до конца изображения), но в общем случае выравнивание для источника и приёмника может быть различным в силу некоторых причин, о которых ниже.

А правильно теперь будет вот так (в предположении, что выровнен только приёмник):

```cpp
for (int y = 0; y < Height; y++) {
    for (int x = 0; x < Width; x++) {
        DstAligned.data[y * AlignedWidth + x] = 255 - Src.data[y * Width + x];
    }
}
```

Или вот так, если больше нравятся указатели, принципиальной разницы в общем нет:

```cpp
    uint8_t *pSrc, *pDst;
    pSrc = Src.data;
    pDst = DstAligned.data;

    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            *pDst++ = 255 - *pSrc++;
        }
        pDst += AlignedWidth - Width;
    }
```
(Кстати, не все компиляторы одинаково хорошо оптимизируют код, использующий массивы или указатели; по моим субъективным наблюдениям Visul Studio более «чувствительна», а вот векторизатор интеловского компилятора нынче одинаково хорошо справляется, но массивы обычно предпочтительнее, хотя и не всегда)

Или в самом общем случае, если выравнивания источника и приёмника различны, тогда после прохода по строке надо скорректировать указатели обоих изображений, чтобы «перескочить» через дырки:

```cpp
    for (int y = 0; y < Height; y++) {
        for (int x = 0; x < Width; x++) {
            *pDst++ = 255 - *pSrc++;
        }
        pSrc += AlignedWidthSrc - Width;
        pDst += AlignedWidthDst - Width;
    }
```

Вы можете спросить, почему так может получиться? Разные значения длины строки могут получиться в случае разной глубины цвета, скажем вы работаете с 8 и 16 бит изображениями, либо при наличии окантовывающего бордюра (о нём речь чуть ниже)

Что нам это даёт кроме лёгких неудобств в циклах и несколько повышенного расхода памяти? Очевидно, что если мы обрабатываем изображение построчно, то в начале каждой строки у нас всегда будет обращение по выровненным адресам, опять же при многопоточной построчной обработке изображения мы автоматически будем попадать на выровненный адрес, и тем самым в самое начало линии кэша и это хорошо.

В OpenCV, кстати, есть метод isContinuous(), он как раз и расскажет нам о непрерывности изображения в памяти:

```cpp
    if (Src.isContinuous()) println("Src is continuous");
    if (!DstAligned.isContinuous()) println("Dst is not continuous");
```

Для того, чтобы перебросить невыровненное изображение в выровненное можно воспользоваться вот такими методами (в зависимости от того, надо ли менять тип или нет)

```cpp
Src.copyTo(DstAligned);
Src.convertTo(DstAligned, CV_8U);
```

Другая необходимость иметь возможность создать выровненное изображение в OpenCV — это одновременная работа с несколькими библиотеками в одном проекте. Так, если вы также используете Intel IPP, то обычно выделяете память, вызывая [ippiMalloc_](https://www.intel.com/content/www/us/en/docs/ipp/developer-reference/2021-9/malloc-001.html),  в нашем случае серой восьмибитной картинки вот так:

```cpp
ippiMalloc_8u_C1(int widthPixels, int heightPixels, int* pStepBytes);
```

И вот pStepBytes — как раз наше выравнивание (шаг) строк. Теперь мы можем просто создать объект Mat, используя возвращённый указатель и длину линии и использовать функции OpenCV для процессинга картинок из Intel IPP, скажем загружать изображения из файлов при помощи imread(), либо показывать их на экране через imshow().

Само собой, «расшаривая» указатель между двумя библиотеками, нужно быть осторожным с точки зрения перевыделения памяти при изменении размеров изображений.

### Выравнивание на границу страницы

Помимо OpenCV и Intel IPP я также использую библиотеку NI Vision Development Toolkit (для меня она основная). Эта библиотека не так широко известна, но общие принципы работы с ней принципиально не отличаются от OpenCV или Intel IPP (там несколько DLL, и свой проприетарный тип), однако есть нюансы. Если мы создаём там изображение через [imaqCreateImage()](https://documentation.help/NI-Vision-LabWindows.CVI-Function/imaqCreateImage.htm), то в ней тоже идёт автоматическое построчное выравнивание на 64 байта, как и в Intel IPP, а вот нулевой пиксель выровнен аж на границу 4096 байт. Зачем это? Как вы знаете (а если не знаете, то немедленно идёте читать Таненбаума или Руссиновича), виртуальная память в компьютере имеет страничную организацию, и вот размер страницы как раз [четыре килобайта](https://devblogs.microsoft.com/oldnewthing/20210510-00/?p=105200) (в нашем частном случае Windows на x86-64). Соответственно, при резервировании памяти без выравнивания по страницам мы можем занять больше страниц, чем нужно (скажем, десятикилобайтный массив может запросто сидеть на четырёх страницах, хотя мог бы гарантированно вписаться в три), и выравнивание на 64 байта нас не спасает, так как нам надо равняться на 4096 байт:

```cpp
uint8_t *Dst;
Dst = (uint8_t*)_aligned_malloc(LineWidth * Height * sizeof(uint8_t), 4096);
```

Минимизировать количество задействованных страниц памяти так же хорошо, как и минимизировать количество используемых кэширующих линий, в этом оба выравнивания в чём-то похожи, хотя природа у них принципиально разная.

Кроме того, ещё одной особенностью работы с этой библиотекой является то, что изображение создаётся с бордюром (по умолчанию в три пиксела), и это очень удобно для операций фильтрации (скажем, свёрткой или медианой), поскольку мы можем смело читать «за пределами» изображения, левее крайних пикселов, и это является вполне себе валидной операцией, поскольку память зарезервирована вот так:

![](https://habrastorage.org/webt/og/ib/9a/ogib9adrb8az7uol-eiqppjyiag.png)

Это довольно частный случай, но при применении операций, требующих бордюра, надо также понимать особенности выделения памяти под него (разные библиотеки могут работать разным образом). Бордюра в три пиксела достаточно для фильтрации с ядром 7x7 (причём только у источника, разумеется). Заполнение этой окантовки зависит от алгоритма — в каких-то случаях заливают константой, а чаще отражают туда пиксели изображения из пограничной области. Однако надо понимать, что бордюр этот «выдавливает» нас в следующее значение выравнивания, кратное 64. Ну то есть ширина строки для картинки шириной 256 пикселей и трёхпиксельным бордюром будет уже 320 пикселей (320 байт в нашем случае восьмибитного изображения). Кстати, в случае шестнадцатибитного изображения шириной 256 пикселов с трёхпиксельным бордюром ширина линии будет уже 288 пикселей (или 576 байт, поскольку у нас два байта на пиксель). Как работающий в LabVIEW, не могу не проиллюстрировать это вот таким кодом:

![](https://habrastorage.org/webt/7t/h6/kt/7th6ktrcqagpwjnn_ppt2rjsrqs.png)

Здесь мы создаём U16 изображение, устанавливаем размер 256х256 и получаем его указатель и геометрию. Вообще всё, что написано выше и будет написано ниже, применимо и к LabVIEW в том числе. На самом деле [NI Vision Development Toolkit](https://www.ni.com/en/shop/data-acquisition-and-control/add-ons-for-data-acquisition-and-control/what-is-vision-development-module.html) — вполне неплохая библиотека, эх если б не стоила таких невменяемых денег, причём платить придётся не только за библиотеку разработчика, но и за рантайм.

## Эксперименты

Ну вот, теперь, так сказать для закрепления материала, можно немного поэкспериментировать.

Коль скоро мы выше затронули страничную организацию и выделение памяти, то нужно также коснуться темы ошибок страниц (которые Page Faults). Давайте проведём несложный эксперимент, выделив себе по гибибайту для источника и приёмника:

```cpp
#define HEIGHT 32768
#define WIDTH 32768
#define LINE_WIDTH WIDTH
#define LARGE (HEIGHT*WIDTH)

uint8_t* LargeSrc = (uint8_t*)_aligned_malloc(LARGE * sizeof(uint8_t), 4096);
uint8_t* LargeDst = (uint8_t*)_aligned_malloc(LARGE * sizeof(uint8_t), 4096);
```

Вопрос на засыпку: насколько изменится количество занимаемой памяти (я имею в виду Working Set) сразу после этих двух вызовов? На самом деле почти не изменится, поскольку память будет зарезервирована и выделена (и у вас на руках вполне себе валидные указатели, отличные от нулей), но как бы ещё не совсем подгружена («как бы» потому что мне сложно подобрать правильные слова, поэтому я оставлю здесь цитату из «Windows Internals». 
> The Windows memory manager uses a demand-paging algorithm to load pages into memory. In demand paging, a page is brought into memory only when a request for it occurs, not in advance. When a reference is made to an address on a page not present in main memory, it is called a page fault. When an application receives a page fault, the memory manager loads into memory the faulted page. The set of pages that a program is actively using, called the Working Set. The amount of pages-backed virtual address space in use, called Commit Size.

В любом случае суть в том, что при первом обращении, неважно на запись или на чтение, мы получим Page Fault, страница памяти будет вначале подгружена и отображена в виртуальное адресное пространство, и лишь затем будет произведена требуемая операция. Дальнейшие чтения/запись остальных 4095 байтов особых проблем не вызовут, а вот при чтении 4097-го байта нам снова прилетит. Это можно увидеть в TaskManager, либо вообще программно, если до и после вызова memcpy() попросить значения соответствующих счётчиков:

```cpp
  PROCESS_MEMORY_COUNTERS memCounter;
  GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));
  println("Page Faults before 1st memcpy - {}", memCounter.PageFaultCount);
  println("Working Set Size before 1st memcpy - {}", memCounter.WorkingSetSize);

  auto start = system_clock::now();
  memcpy(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
  auto end = system_clock::now();
  println("memcpy 1st call - {} ms", MS(end - start).count());

  GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));
  println("Page Faults after 1st memcpy - {}", memCounter.PageFaultCount);
  println("Working Set Size after 1st memcpy - {}", memCounter.WorkingSetSize);

  start = system_clock::now();
  memcpy(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
  end = system_clock::now();
  println("memcpy 2nd call - {} ms", MS(end - start).count());
```

Точности замера времени через system_clock::now() нам будет более чем достаточно. «MS» — нехитрый макрос для перевода в миллисекунды:

```cpp
#define MS duration_cast<milliseconds>
```

Время выполнения двух malloc() мы увидим чуть ниже, а пока посмотрим что показывает [vmmap](https://learn.microsoft.com/en-us/sysinternals/downloads/vmmap) непосредственно перед вызовами _aligned_malloc:

![](https://habrastorage.org/webt/nr/lv/rr/nrlvrrdbbhmazqfkdqxucznghpq.png)

А вот что сразу после, видно что в куче выделено два гигабайта, но Working Set пока пуст:

![](https://habrastorage.org/webt/nl/cc/ka/nlcckasqok-wwlpsyjx1q5gjrf8.png)

Ну а вот что после memcpy(), Working Set принял нагрузку:

![](https://habrastorage.org/webt/4h/lm/ka/4hlmkadecagyxyxjsflcxacgbv4.png)

Вот что получилось с точки зрения времени выполнения (чтобы чересчур умный компилятор не выкинул первый вызов memcpy, я отключил оптимизацию через #pragma optimize( "", off )):

```
Page Faults before 1st memcpy - 23647
Working Set Size before 1st memcpy - 91373568
memcpy 1st call - 387 ms
Page Faults after 1st memcpy - 524621
Working Set Size after 1st memcpy - 2139373568
memcpy 2nd call - 100 ms
```

Как видите первый вызов занял почти в четыре раза больше времени и нам прилетело чуть больше полумиллиона страниц с ошибками (ну да, «прогревать» память вполне имеет смысл, это вам не аудиокабели). В рамках данной статьи мы не будем углубляться в премудрости TLB (который Translation Lookaside Buffer/Буфер ассоциативной трансляции), отметим лишь, что в хорошо спроектированном приложении в общем не должно быть большого и постоянно растущего количества Page Faults, исчисляемого миллионами.

### Эффект линий кэша

Теперь пробежимся по нашим байтовым массивам поэлементно либо с шагом в 64 байта (чтобы поставить оба цикла в более-менее равные условия я сделаю в первом цикле инкремент на два значения, чтобы избежать использования простого inc на тот случай если inc быстрее чем add, хотя на современных архитектурах они почти одинаковы):

```cpp
//============================================================================
// Cache line miss illustration
//

    start = system_clock::now();
    for (int i = 0; i < LARGE / 32; i+= 2) LargeDst[i] = LargeSrc[i];
    end = system_clock::now();
    println("\nCopy every 2nd elementh - {} ms", MS(end - start).count());

    start = system_clock::now();
    for (int i = 0; i < LARGE; i += 64) LargeDst[i] = LargeSrc[i];
    end = system_clock::now();
    println("Copy every 64th elementh - {} ms", MS(end - start).count());
```

Вот результат:

```
Copy every 2nd elementh - 31 ms
Copy every 64th elementh - 158 ms
```

Несмотря на то, что в обоих случаях у нас 16777216 итераций, первый цикл заметно быстрее, так как мы промахиваемся мимо кэша лишь на каждой 32-й итерации.

Следующий эксперимент, который имеет смысл сделать — перейти к SIMD командам и посмотреть какой выигрыш дают команды, предназначенные для работы с выровненной памятью. Как было отмечено выше, если данные невыровнены, то это вовсе не означает, что их запрещено читать, просто команды там другие.

Проще всего воспользоваться ассемблером.

### Работа с выровненными данными на ассемблере

Почему я сразу не взял интрисинки? Дело в том, что программируя прямо на ассемблере вы с компьютером «один-на-один», у вас есть полный контроль над используемыми командами и порядком их следования, а вот если вы пользуетесь интрисинками, то всегда не лишне будет заглянуть в ассемблерный листинг и проверить, во что же они там компиляются, и да, там [бывает не всё однозначно](https://stackoverflow.com/questions/31089502/aligned-and-unaligned-memory-access-with-avx-avx2-intrinsics).

В данном случае упражнение очень простое. Мы сделаем на ассемблере динамическую библиотеку DLL, которую будем вызывать из Си-кода. Всё, что нужно знать — это [соглашение о вызовах](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170), оно для х64 совсем несложное. Первые четыре параметра передаются через регистры RCX, RDX, R8 и R9, остальные через стек. Результат, если вы его возвращаете — через RAX. Если вы трогаете регистры RBX, RBP, RDI, RSI, RSP, R12, R13, R14, R15, и XMM6-XMM15, то их надо затолкать в стек, а потом восстановить. Вот и всё. Чтобы упростить эксперимент, мы резервируем гибибайт выровненной памяти, как бы использовав изображение размером 32768х32768 пикселей, что даст нам 1024³ байт. Ширина линии в этом случае будет равна ширине изображения, то есть 32768, выравнивающие промежутки нам не нужны., так что мы можем обойтись всего тремя параметрами — адресами источника и приёмника да общим размером данных для процессинга. Всё, что написано ниже, разумеется справедливо и для общего случая изображения произвольного размера и глубины, просто надо будет передать не один дополнительный, а три — размеры по обоим измерениям и ширину линии с учётом выравнивания (так обычно вызываются функции Intel IPP). В случае 32-х бит не забудьте, что соглашения о вызовах несколько отличаются, и учтите, что там есть stdcall и cdecl.

Короче, слепим пару вот таких функций:

```cpp
extern "C" uint64_t fnProcessU(void* dst, void* src, size_t Bytes);
extern "C" uint64_t fnProcessA(void* dst, void* src, size_t Bytes);
```

Первая функция «fnProcessU» — будет использовать команду [vmovdqu](https://www.felixcloutier.com/x86/movdqu:vmovdqu8:vmovdqu16:vmovdqu32:vmovdqu64) для невыровненных данных как для чтения, так и для записи:

```assembly
	U255 DY 32*BYTE 255

fnProcessU PROC
	shr r8, 5;     ;divide by 32
	vmovdqu ymm5, [U255]
LU:
    vmovdqu  ymm1, [rdx]
	vpsubb ymm1, ymm5, ymm1
    vmovdqu [rcx], ymm1
    add rdx, 32
    add rcx, 32
    dec r8
    jnz LU

	RET
ENDP fnProcessU
```

Даже если вы не знаете ассемблер, то тут всё достаточно тривиально: shr это сдвиг вправо, стандартный способ деления на степени двойки, add и dec говорят сами за себя, а jnz — это переход (Jump if Not Zero). Для более углублённого погружения могу порекомендовать книжку Даниэля Куссвюрма (см. список литературы в конце). Кстати, только команда vpsubb требует AVX2, остальные — нет (то есть ymm* регистры доступны и на AVX). Если у вас нет поддержки AVX2, то можете просто перейти на регистры xmm, скорректировав счётчики, а если есть AVX 512, то на zmm, впрочем это не даст сильного выигрыша, поскольку мы упираемся в производительность памяти.

Ну а для выровненных данных я воспользуюсь парой [vmovdqa](https://www.felixcloutier.com/x86/movdqa:vmovdqa32:vmovdqa64)/[vmovntdq](https://www.felixcloutier.com/x86/movntdq):

```assembly
LA:
    vmovdqa  ymm1, [rdx]
	vpsubb ymm1, ymm5, ymm1
    vmovntdq [rcx], ymm1     
    add rdx, 32
    add rcx, 32
    dec r8
    jnz LA
```

vmovntdq также пишет данные «мимо кэша», что даёт дополнительное ускорение (но не всегда). Подсветка синтаксиса ассембера выглядит на Хабре забавно, но так даже веселее.

Ещё пара усовершенствований, которые мы сделаем — четырёхкратный разворот (unroll) цикла, чтобы немного снизить оверхед от счётчика и кроме того заодно уж посчитаем количество тактов, используя [cpuid/rdtsc](https://habr.com/ru/articles/147852/), благо на ассемблере это несложно, и вернём их через RAX.

<spoiler title="Полный листинг под спойлером:">

```assembly
EXPORT fnProcessA ;aligned processing
fnProcessA PROC
	push rsi
	push rdi
	push rbx

    shr r8, 6
	vmovdqu ymm5, [U255]
	mov rsi, rdx
	mov rdi, rcx

	cpuid ; force all previous instructions to complete and reset rax...rdx registerss!
	rdtsc ; read time stamp counter
	mov r10d, eax ; save EAX for later
	mov r11d, edx ; save EDX for later
LA:
    vmovdqa  ymm1, [rsi]
    vmovdqa  ymm2, [rsi+32]
    vmovdqa  ymm3, [rsi+32*2]
    vmovdqa  ymm4, [rsi+32*3]
	vpsubb ymm1, ymm5, ymm1
	vpsubb ymm2, ymm5, ymm2
	vpsubb ymm3, ymm5, ymm3
	vpsubb ymm4, ymm5, ymm4
    movntdq [rdi], ymm1     
    movntdq [rdi+32*1], ymm2
    movntdq [rdi+32*2], ymm3
    movntdq [rdi+32*3], ymm4
    add rdi, 128
    add rsi, 128
    dec r8
    jnz LA
	
	;vzeroupper	

	cpuid ; wait for FDIV to complete before RDTSC
	rdtsc ; read time stamp counter
	sub eax, r10d ; subtract the most recent CPU ticks from the original CPU ticks
	sbb edx, r11d ; now, subtract with borrow
    shl rax, 32
	shrd rax, rdx, 32

	pop rbx
	pop rdi
	pop rsi

    RET
ENDP fnProcessA
```
</spoiler>

Код выше подразумевает, что ширина изображения кратна 128 байтам, если же нет, то мы будем обрабатывать и «лишние» пикселы из выравнивающих промежутков. В принципе ничего страшного в этом нет, если же необходимо их исключить, то нужно добавить код для обработки «хвоста», и здесь, при наличии поддержки процессором, могут помочь AVX-512 команды, где [использование масочных регистров](https://habr.com/ru/companies/intel/articles/266055/) позволяет заметно ускорить обработку.

Пара слов по поводу используемого макроассемблера. Вы можете воспользоваться практически любым ассемблером, поддерживающим х64 (FASM, NASM, MASM, YASM, и т.д.), но я воспользовался довольно малоизвестным [EuroAsssembler](https://euroassembler.eu). Это весьма легковесный инструмент (исполняемый файл всего-то около 400 килобайт), написанный на себе самом, очень прост и просто идеален для учебных целей, на мой взгляд. Автор — Pavel Šrubař из Чехии. Причём это и ассемблер и линковщих в одном флаконе, ему не нужен отдельный линкер, как в случае некоторых других ассемблеров. Все опции прописываются прямо в asm файле, компиляция в библиотеку осуществляется одной командой

```
>euroasm.exe AVX2Process.asm 
```

lib из dll, если нужно, делается элементарно парой команд (из под промпта Visual Studio)

```
DUMPBIN AVX2Process.dll /EXPORTS /OUT:AVX2Process.def.
//Modify def file for LIB.
LIB /DEF:AVX2Process.def /machine:X64 /OUT:AVX2Process.lib.
```

Линковка — стандартная, я обычно делаю вот так, потому что лень лазать в опции

```
#pragma comment( lib, "Assembler/AVX2Process" )
```

Вот теперь можно замерить производительность:

```cpp
//============================================================================
// Processing with unaligned commands
//
start = system_clock::now();
uint64_t ticksU = fnProcessU(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
end = system_clock::now();
println("\nProcessing unalign -  {} ms ({} ticks)", MS(end - start).count(), ticksU);

//============================================================================
// Processing with aligned commands
//
start = system_clock::now();
uint64_t ticksA = fnProcessA(LargeDst, LargeSrc, LARGE * sizeof(uint8_t)); //!Aligned
end = system_clock::now();
println("Processing (aligned) - {} ms ({} ticks)", MS(end - start).count(), ticksA);
println("difference - {} ticks", ticksU - ticksA);
```

Разница в скорости налицо (прогон был выполнен на i7-7700):

```
Processing - unaligned call -  154 ms (604931386 ticks)
Processing - aligned call - 114 ms (448352055 ticks)
difference - 156579331 ticks
```

При этом мы в обоих случаях работали по одним и тем же данным, просто разными командами. В первом случае мы слегка медленнее «чистой» memcpy (отчасти оттого, что в memcpy() идет восьмикратная развёртка, кроме того, у нас есть дополнительная операция вычитания), зато во втором случае мы даже слегка обгоняем её, несмотря на дополнительную операцию. Кстати, исходник memcpy.asm для текущей на данный момент версии лежит в папке (при установке по умолчанию) "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.38.33130\crt\src\x64", если вдруг кому-нибудь захочется полюбопытствовать, там есть пара любопытных моментов.

Вообще использование ассемблера открывает широчайшие возможности для экспериментов.  Вот что будет быстрее — использовать пару vmovdqa/vpsubb или сразу vpsubsb или переставить команды так, чтобы они чередовались в надежде выжать ещё немножко из конвейеризации? И так далее, но эта «тонкая настройка» может запросто вылиться в отдельную статью.

DLL эту, само собой я могу вызывать также и из LabVIEW (да вообще откуда угодно где я могу вызвать DLL), передавая ей указатели на изображения:

![](https://habrastorage.org/webt/xm/g7/km/xmg7km2ckbnyxaxprrjtw6cevau.png)

В данном случае я немного схитрил, взяв изображение «baboon.jpg» (которое 512х512) и выключив бордюр у изображений, тем самым избавившись от выравнивающих промежутков, а в реальной жизни количество «спагетти» и проверок чуть больше, конечно.

Разумеется, всё это можно переложить почти один-в-один и на интрисинки.

### Интрисинки

Здесь всё достаточно просто:

```cpp
#include <immintrin.h>
//...
	for (size_t i = 0; i < Blocks; i++) {
        // There is no penalty to using vmovdqu when the address is aligned.
        __m256i b0 = _mm256_load_si256((__m256i*)Src); //32 Source pixels
        __m256i b1 = _mm256_load_si256((__m256i*)(Src + 32)); //next 32 pixels
        __m256i b2 = _mm256_load_si256((__m256i*)(Src + 64)); //and so on
        __m256i b3 = _mm256_load_si256((__m256i*)(Src + 96)); //4x unroll

        b0 = _mm256_subs_epi8(u255, b0);
        b1 = _mm256_subs_epi8(u255, b1);
        b2 = _mm256_subs_epi8(u255, b2);
        b3 = _mm256_subs_epi8(u255, b3);

        _mm256_store_si256((__m256i*)Dst, b0); //или vmovntdq -> _mm256_stream_si256()
        _mm256_store_si256((__m256i*)(Dst + 32), b1);
        _mm256_store_si256((__m256i*)(Dst + 64), b2);
        _mm256_store_si256((__m256i*)(Dst + 96), b3);
        Src += 128; Dst += 128;
    }
```

Упражнение с заменой _mm256_store_si256 на _mm256_stream_si256 я, пожалуй, оставлю читателю.

Как отмечалось выше, имеет смысл заглянуть в ассемблерный листинг, в Visual Studio он включается опцией Assembly, Machine Code and Source (/FAcs).

Там вы как раз увидите, к примеру, что пара _mm256_load_si256 / _mm256_subs_epi8 разворачивается в одну команду [vpsubsb](https://www.felixcloutier.com/x86/psubsb:psubsw) ymm0, ymm4, YMMWORD PTR [rcx], которая одновременно и читает из памяти и вычитает.

Кстати, возможно не все знают, что в OpenCV также есть [свои собственные итринсинки](https://docs.opencv.org/3.4/df/d91/group__core__hal__intrin.html). Давайте для разнообразия воспользуемся _aligned версиями вызовов, а  для того, чтобы чтобы воспользоваться AVX2 и типом v_uint8x32, надо определить CV_AVX2:

```cpp
#define CV_AVX2 1
#include "OpenCV/opencv.hpp"
#include "OpenCV/core/hal/intrin.hpp"
//...
void OpenCVIntrinsics(uchar *Dst, uchar *Src, size_t Bytes)
{
    size_t Blocks = Bytes >> 7;
    __declspec(align(32)) uint8_t U255[32];
    fill_n(U255, sizeof(U255) / sizeof(*U255), 255); // fill with 255
    v_uint8x32 u255 = v256_load_aligned((uchar*)U255);

    for (size_t i = 0; i < Blocks; i++) {
        v_uint8x32 pix0 = v256_load_aligned(Src);
        v_uint8x32 pix1 = v256_load_aligned(Src + 32);
        v_uint8x32 pix2 = v256_load_aligned(Src + 64);
        v_uint8x32 pix3 = v256_load_aligned(Src + 96);
        v_uint8x32 neg0 = u255 - pix0;
        v_uint8x32 neg1 = u255 - pix1;
        v_uint8x32 neg2 = u255 - pix2;
        v_uint8x32 neg3 = u255 - pix3;
        v_store_aligned(Dst, neg0);
        v_store_aligned(Dst + 32, neg1);
        v_store_aligned(Dst + 64, neg2);
        v_store_aligned(Dst + 96, neg3);
        Src += 128; Dst += 128;
    }
}
```

Результат:

```
Processing Intrinsics - 147 ms (576243042 ticks)
Processing OpenCV Intrinsics - 152 ms (596949208 ticks)
```

Тут скорость примерно одинакова в пределах статистической погрешности.

Кстати, если заглянуть таки в листинг, то мы там увидим, что v256_load_aligned разворачивается в vmovdqu, хотя я бы, пожалуй, ожидал vmovdqа:

```
?v256_load_aligned ... v_uint8x32@12@PEBE@Z PROC ;
cv::hal_baseline::v256_load_aligned, COMDAT

; 393  : OPENCV_HAL_IMPL_AVX_LOADSTORE(v_uint8x32,  uchar)

00000	c5 fe 6f 02	 vmovdqu ymm0, YMMWORD PTR [rdx]
```

Где-то проскакивали сообщения о том, что вызов vmovdqu не приводит к пенальти если оперирует выровненными данными и идентичен VMOVDQA, но мои эксперименты того не показывают — VMOVDQA слегка быстрее. С другой стороны учебник учит, что для того, чтоб интринсинки включили VMOVDQA нужно явно указать, что наши данные выровнены:

```cpp
    assume_aligned<64>(Src);
    assume_aligned<64>(Dst);
```

Но даже с учётом этого мне не удалось заставить компилятор использовать именно VMOVDQA. Так что в небольших проектах проще сразу на ассемблере написать так, как надо (либо воспользоваться интрисинками «для затравки»), и попутно замерить скорость. Впрочем я не являюсь профессионалом в интрисинках, и уверен, среди читателей найдутся более опытные пользователи, если так, то напишите пару строк в комментах — это будет полезно всем. Не лишним будет и проанализировать «горячие» участки при помощи [Intel VTune](https://www.intel.com/content/www/us/en/developer/tools/oneapi/vtune-profiler.html#gs.0xthlx), но этот эксперимент несколько выходит за рамки статьи, кроме того у нас есть и другие способы увеличить производительность.

Ах да, безусловно любопытно проверить насколько производительны встроенные функции OpenCV сами по себе:

```cpp
//============================================================================
// Processing with OpenCV
//
   start = system_clock::now();
   LargeDstOCV = 255 - LargeSrcOCV; 
   end = system_clock::now();
   println("Processing OpenCV - {} ms", MS(end - start).count());
```

Результат вполне сравним с интрисинками, OpenCV неплохо оптимизирована:

```
Processing OpenCV - 158 ms
```

### Мультипоточная обработка

Напоследок, как было отмечено выше, тот факт,что каждая строка начинается с выровненного адреса сильно упрощает многопоточное программирование. В нашем случае самый простой способ — воспользоваться OpеnMP (включив соответствующую опцию в настройках компилятора), например раскидав выполнение на четыре потока и используя ту же функцию для выровненных данных на ассемблере из примера выше:

```cpp
    uint8_t* srcTmp = LargeSrc;
    uint8_t* dstTmp = LargeDst;
    start = system_clock::now();
#pragma omp parallel for num_threads(4)
    for (int x = 0; x < HEIGHT; x++) {
        fnProcessA(dstTmp, srcTmp, WIDTH * sizeof(uint8_t));
        dstTmp += WIDTH;
        srcTmp += WIDTH;
    }
    end = system_clock::now();
    println("\nParallel Processing with OpenMP - {} ms ", MS(end - start).count());
```

Но никто не запрещает и «по-старинке», создавая потоки «ручками», нам тогда понадобится несложная структура:

```cpp
#define N_THREADS 4

static HANDLE hProcessThreads[N_THREADS] = { 0 };
static HANDLE hStartSemaphores[N_THREADS] = { 0 };
static HANDLE hStopSemaphores[N_THREADS] = { 0 };

typedef struct{
    int ct;
    void* src, * dest;
    size_t size;
} mt_proc_t;

mt_proc_t mtParamters[N_THREADS] = { 0 };
```

Вот функция обработки, которую мы будем запускать из каждого потока:

```cpp
DWORD WINAPI thread_proc(LPVOID param)
{
    mt_cpy_t* p = (mt_cpy_t*)param;

    while (1){
        WaitForSingleObject(hStartSemaphores[p->ct], INFINITE);
        fnProcessA(p->dest, p->src, p->size);//pointers must be aligned!
        ReleaseSemaphore(hStopSemaphores[p->ct], 1, NULL);
    }
    return 0;
}
```

создавать потоки мы будем вот так:

```cpp
void startProcessingThreads()
{
    for (int ctr = 0; ctr < N_THREADS; ctr++){
        hStartSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
        hStopSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
        mtParamters[ctr].ct = ctr;
        hProcessThreads[ctr] = CreateThread(0,0, thread_proc, &mtParamters[ctr], 0,0);
    }
}
```
а запускать вот так, разрезая картинку на четыре равные части, по числу потоков:

```cpp
void* runProcessingThreads(void* dest, void* src, size_t bytes)
{
    //set up parameters
    for (int ctr = 0; ctr < N_THREADS; ctr++){
        mtParamters[ctr].dest = (char*)dest + ctr * bytes / N_THREADS;
        mtParamters[ctr].src = (char*)src + ctr * bytes / N_THREADS;
        mtParamters[ctr].size = (ctr + 1)*bytes/N_THREADS - ctr*bytes/N_THREADS;
    }

    //release semaphores to start computation
    for (int ctr = 0; ctr < N_THREADS; ctr++)
        ReleaseSemaphore(hStartSemaphores[ctr], 1, 0);
    //wait for all threads to finish
    WaitForMultipleObjects(N_THREADS, hStopSemaphores, TRUE, INFINITE);

    return dest;
}
```

ну и в конце останавливать:

```cpp
void stopProcessingThreads()
{
    for (int ctr = 0; ctr < N_THREADS; ctr++){
        TerminateThread(hProcessThreads[ctr], 0);
        CloseHandle(hStartSemaphores[ctr]);
        CloseHandle(hStopSemaphores[ctr]);
    }
}
```

Всё вместе выглядит вот так:

```cpp
//============================================================================
// Multithreaded processing - classic
//
    startProcessingThreads();

    start = system_clock::now();
    runProcessingThreads(LargeDst, LargeSrc, LARGE * sizeof(uint8_t));
    end = system_clock::now();
    println("Multi threaded processing {} ms", MS(end - start).count());

    stopProcessingThreads();
```

Результат что так что сяк примерно одинаков:

```
Parallel Processing with OpenMP - 60 ms
Multi threaded processing 61 ms
```

Мы не получаем здесь четырёхкратного прироста, потому что намертво упираемся в производительность двухканальной памяти, но на каком-нибудь Xeon с шестиканальной памятью, можно «выжать» куда как больше, но и двукратное ускорение, которое мы видим — вполне неплохо.

## Результаты

Я погонял эти несложные тесты также на двух Xeon процессорах (при написании статьи использовался i7-7700):

Intel Xeon W-2245 3.9 GHz 8 Cores 16 Threads
Intel Xeon Gold 6132 2.6 GHz 28 Cores 56 Threads

| Тест (32768х32768 = 1024³ байтов)                            | Xeon W-2245 3.9 GHz | Xeon Gold 6132 2.6 GHz |
| ------------------------------------------------------------ | ------------------- | ---------------------- |
| memcpy() первый вызов                                        | 347 мс              | 467 мс                 |
| memcpy() второй вызов                                        | 123 мс              | 205 мс                 |
| Копирование каждого второго элемента                         | 31 мс               | 39 мс                  |
| Копирование каждого 64-го элемента                           | 158 мс              | 217 мс                 |
| Невыровненный процессинг (ассемблер, используя vmovdqu)      | 154 мс              | 154 мс                 |
| Выровненный процессинг (ассемблер, используя vmovdqa/vmovntdq) | 114 мс              | 203 мс                 |
| Интрисинки                                                   | 155 мс              | 156 мс                 |
| OpenCV интрисинки                                            | 160 мс              | 169 мс                 |
| OpenCV нативный вызов                                        | 160 мс              | 195 мс                 |
| OpenMP на 16 потоков (выровнено)                             | 62 мс               | 34 мс                  |
| OpenMP на 16 потоков (невыровнено)                           | 87 мс               | 37 мс                  |
| Классические потоки (выровнено)                              | 61 мс               | 33 мс                  |

В общем результаты предсказуемы — W-2245 в целом лидирует на частоте в полтора раза выше, чем Gold 6132, за исключением кода на ассемблере — здесь попытка применить vmovntdq вызывает замедление., хотя тот же код, работающий по нескольким потокам даёт некоторое преимущество. Но это не совсем «простая» инструкция. Ну и конечно мы обгоняем при мультипоточности — двадцать восемь ядер вкупе с шестиканальной памятью творят чудеса.

## Заключение

Значит ли всё изложенное выше, что нужно фанатично выравнивать всё и вся? Нет, конечно. До тех пор пока работа с невыровненными данными не является ярко выраженным «бутылочным горлышком», надо просто работать так, как удобно. Если используемая библиотека выделяет память с выравниванием «из коробки» — что ж, это хорошо. Если вы работаете с OpenCV, то принудительное выравнивание привнесёт дополнительное количество «обвязки» в код, да и преждевременная оптимизация — она вредна в большинстве случаев.

Однако если вы всё-таки работаете с выравниванием, то нужно обращать внимание на несколько вещей.

Во-первых, не стоит «забивать гвоздями» выравнивание на 64 байта в код. Сегодня оно 64 байта, но я помню времена, когда оно было 32 байта (при этом был период, когда одна и та же версия библиотеки в 32-х битной версии работала с выравниванием 32 байта, а та же самая, но 64-х битная выравнивала линии уже на 64 байта). Ну а завтра появится какой-нибудь супер AVX 10+ и мы запросто получим требование выравнивания на 128 байт. Пишите сразу универсально.

Во-вторых при работе со сторонними библиотеками обращайте внимание на то, что некоторые возвращают длину линии в байтах, а некоторые — в пикселах. Это не вызывает проблем до тех пор пока вы не начинаете работать с 16-ти битными данными (или большей разрядности), разумеется.

И в-третьих выравнивание зависит не только от ширины, но и от общей геометрии изображений, у двух изображений одинакового размера шаг строк может отличаться, если у них разные бордюры или разная глубина цвета.

В современном многоядерном мире параллелизация обычно даёт куда как больший выигрыш, ну а правильное выравнивание данных может дать приятный бонус и в то же время несколько упростить распараллеливание.

Вот собственно и всё, о чём я хотел рассказать, надеюсь кому-нибудь окажется полезно, если не как «туториал», то по крайней мере как «стартовая точка» для дальнейших увлекательных экспериментов.

### Список литературы и полезные ссылки

Эндрю Таненбаум, Хербет Бос: Современные операционные системы, 4-е издание, ISBN: 978-5-4461-1155-8.

Э. Таненбаум, Т. Остин: Архитектура компьютера, 6-е издание, ISBN: 978-5-4461-1103-9.

М. Руссинович, Д. Соломон, П. Йосифович, А.Ионеску: Внутреннее устройство Windows, 7-е издание, ISBN: 978-5-4461-0663-9.

Крис Касперски:  Техника оптимизации программ. Эффективное использование памяти, ISBN: 5-94157-232-8.

Даниэль Куссвюрм: Профессиональное программирование на ассемблере x64 с расширениями AVX, AVX2 и AVX-512, ISBN: 978-5-97060-928-6

Agner Fog: [Software optimization resources](https://www.agner.org/optimize/).

Ulrich Drepper: [What Every Programmer Should Know About Memory](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf).

[Intel Intrinsics Guide](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html)

### Пример

Весь код, показанный выше, находится вот [здесь](https://github.com/AndrDm/OpenCV-Alignment), на гитхабе (могут быть незначительные отличия, я набрасывал его параллельно со статьёй). Использованы OpenCV 4.8.1 и Visual Studio 2022 17.8.0, актуальные на данный момент. Как вы заметили, в примерах я использовал предварительный последний стандарт С++ (/std:c++latest)), и там где-то возникла пара небольших конфликтов в заголовочных файлах, так что я их слегка поправил, банально закомментировав проблемные места, и попутно отвязав их от намертво прибитой папки "opencv2", так что если вы соберётесь с элементами из примера идти «в продакшен», то лучше таки взять оригинальные файлы из официального [OpenCV репозитория](https://github.com/opencv/opencv).